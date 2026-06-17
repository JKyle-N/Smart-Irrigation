/* =============================================================================
 *  SMART IRRIGATION & FERTIGATION SYSTEM  --  ESP32 #2  ACTUATOR CONTROLLER
 *  Controller 3 of 3   (Nano  ->  ESP32 #1  ->  ESP32 #2)
 * -----------------------------------------------------------------------------
 *  ROLE (spec sec.10.2, 9.8.2 / CLAUDE.md): ACTUATOR EXECUTION SUBSYSTEM.
 *    - Receives a COMPLETE work order from ESP32 #1 and executes the entire
 *      valve/pump/mixer/dosing sequence autonomously (it owns HOW + timing).
 *    - Meters delivered/dosed VOLUME from flow sensors (pulses / K-factor).
 *    - Validates power (PZEM) and currents (ACS712); performs IMMEDIATE local
 *      protective stops for hardware safety (no-flow, overcurrent, EC/pH out of
 *      window) on its own authority (sec.9.8.1.2, sec.11.1).
 *    - Reports ACK / DONE / fault back to ESP32 #1. Does NOT decide policy,
 *      schedule, or classify global severity (that stays with ESP32 #1).
 *
 *  PROTOCOL (must match what ESP32 #1 emits; framed sec.9.9):
 *    IN : <START>,SEQ_IRRIGATION_<X>,FLUSH,<pct>,<END>
 *         <START>,SEQ_FERTIGATION_<X>,FLUSH,<pct>,DOSE,<mlA>,<mlB>,<mlC>,
 *                 EC,<lo>,<hi>,PH,<lo>,<hi>,MIX,<ms>,<END>
 *         <START>,STOP_ALL,<END> / RESET_NANO / STATUS_REQ
 *         TEST,ENTER / TEST,EXIT / TEST,HOLD,<bit> / TEST,RELEASE  (dead-man manual
 *                 relay test, sec.18.10.8): ESP1 streams HOLD while ENTER held; ESP2
 *                 keeps ONE relay ON only while HOLD arrives (timeout), with a 10s
 *                 hard cap. bit = PCF8575 OUT_* index 0..15.
 *    OUT: ACK,<cmd> -> DONE,<cmd>;  FLOW_FAIL/PWR_FAIL/EC_FAIL/PH_FAIL/SAFE_STOP/
 *         ERROR; DEGRADED,<chan> (channel disabled, sequence continues -> ESP1 Major,
 *         no shutdown); BUSY,ACTIVE; INVALID,<reason>; STATUS,ESP2,OK; READY,ESP2
 *
 *  NON-BLOCKING: no delay() anywhere; every stage is a millis()-based step in a
 *  FSM. Task watchdog petted each loop iteration.
 *
 *  PLUMBING MODEL (documented assumption -- spec delegates HOW to ESP2 and does
 *  not step-specify the hydraulics; confirm against the real rig):
 *    Transfer pump (P6): reservoir -> mixing tank   (metered by flow sensor 4)
 *    Booster pump  (P7): mixing tank -> columns      (metered by flow sensor 5)
 *    Nutrient pumps (P11/12/13): dose into mixing tank (metered by 18/19/23)
 *    Inverter relay (P5) enabled before any AC pump (transfer/booster/mixer).
 *
 *  >>> Values to MEASURE / set at commissioning are tagged [MEASURE] / [TBD].
 * ========================================================================== */

#include <Arduino.h>
#include <Wire.h>
#include <PZEM004Tv30.h>
#include <esp_task_wdt.h>

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

/* =============================================================================
 *  DEBUG BUILD  --  FAKE SENSOR INPUTS (no real components on the bench)
 * -----------------------------------------------------------------------------
 *  With DEBUG_FAKE set, ESP32 #2 fabricates every analog/flow/power reading so a
 *  full work-order sequence runs to completion (DONE) WITHOUT any PZEM, ACS712,
 *  pH/EC probes, or flow sensors wired:
 *    - flow: liters are simulated from elapsed pump-on time (SIM_FLOW_LPS), so
 *      metered FILL/DOSE/DELIVER/FLUSH stages reach target instead of FLOW_FAIL.
 *    - PZEM: fake nominal mains + healthy current while an AC pump is ON.
 *    - mixer current / pH / EC: fake in-window healthy values.
 *  All command parsing, the sequence FSM, TEST dead-man, and UART framing are
 *  UNCHANGED -- only the leaf sensor reads are faked. Set DEBUG_FAKE 0 to restore
 *  real hardware reads (identical to the production ESP2/ build).
 * ========================================================================== */
#define DEBUG_FAKE     1
#define SIM_FLOW_LPS   1.0f      // simulated delivered flow, liters/second

/* =============================================================================
 *  EDITABLE CONSTANTS
 * ========================================================================== */

/* ---- ESP1 link (HW UART2) ------------------------------------------------ */
#define ESP1_RX_PIN  16
#define ESP1_TX_PIN  17
#define ESP1_BAUD    9600
#define DEBUG_BAUD   115200

/* ---- PZEM-004T (HW UART1, spec sec.19.2.1) ------------------------------- */
#define PZEM_TX_PIN  13        // ESP TX -> PZEM RX
#define PZEM_RX_PIN  14        // ESP RX <- PZEM TX

/* ---- I2C / PCF8575 ------------------------------------------------------- */
#define I2C_SDA      21
#define I2C_SCL      22
#define PCF_ADDR     0x20

/* ---- PCF8575 output bits (active-LOW; spec sec.19.4.7) -------------------- */
#define OUT_RES_VALVE  0    // P0  reservoir valve
#define OUT_COL_A      1    // P1
#define OUT_COL_B      2    // P2
#define OUT_COL_C      3    // P3
#define OUT_MIX_VALVE  4    // P4  mixing-tank valve
#define OUT_INVERTER   5    // P5  inverter relay (AC source)
#define OUT_TRANSFER   6    // P6  transfer pump (AC)
#define OUT_BOOSTER    7    // P7  booster pump  (AC)
#define OUT_MIXER      8    // P10 mixer motor   (AC, ACS712-monitored)
#define OUT_NUT_A      9    // P11 nutrient A pump (DC dosing)
#define OUT_NUT_B     10    // P12 nutrient B pump
#define OUT_NUT_C     11    // P13 nutrient C pump
#define OUT_NUT_D     12    // P14 nutrient D pump -- UNUSED (never driven)
#define OUT_PH_UP     13    // P15 pH up pump
#define OUT_PH_DN     14    // P16 pH down pump
#define OUT_NANO_RST  15    // P17 Nano RESET transistor
const uint8_t COL_VALVE_BIT[3] = { OUT_COL_A, OUT_COL_B, OUT_COL_C };
const uint8_t NUT_PUMP_BIT[3]  = { OUT_NUT_A, OUT_NUT_B, OUT_NUT_C };

/* ---- Flow sensors (interrupt, stage-based; spec sec.19.4.5) --------------- */
#define FLOW_RES_MIX  4
#define FLOW_MIX_IRR  5
#define FLOW_NUT_A   18
#define FLOW_NUT_B   19
#define FLOW_NUT_C   23
const uint8_t NUT_FLOW_PIN[3] = { FLOW_NUT_A, FLOW_NUT_B, FLOW_NUT_C };
// (NutD 25, pHUp 26, pHDn 27 flow pins exist in hardware but are not metered here.)

/* ---- Analog sensors (ADC1; spec sec.19.4.6) ------------------------------ */
#define PIN_PH        32
#define PIN_EC        33
#define PIN_MIXER_I   36       // ACS712 mixer current
#define PIN_SPARE     39

/* ---- Flow K-factors (pulses per LITER) [MEASURE] ------------------------- */
const float K_RES_MIX = 450.0f;
const float K_MIX_IRR = 450.0f;
const float K_NUT[3]   = { 450.0f, 450.0f, 450.0f };   // small dosing sensors differ [MEASURE]

/* ---- Per-column water budget (liters per service) [TBD] ------------------ */
const float WATER_BUDGET_L[3] = { 5.0f, 5.0f, 5.0f };
bool COLUMN_ENABLED[3] = { true, true, false };        // [CONFIRM] mirror ESP1/Nano

/* ---- Stage timings ------------------------------------------------------- */
const unsigned long VALVE_SWITCH_MS   = 1000;   // valve settle before pump (DC stages)
const unsigned long INVERTER_WARMUP_MS = 2000;  // inverter+valve settle before AC pump
const unsigned long FLOW_TIMEOUT_MS   = 10000;  // no-flow after pump start -> FLOW_FAIL
const unsigned long STAGE_MAX_MS      = 180000; // hard safety cap per metered stage
const unsigned long MIX_DEFAULT_MS    = 30000;  // used if work order omits MIX
const unsigned long SAFE_STOP_MS      = 500;

/* ---- PZEM AC validation (spec sec.23.1.2.1) ------------------------------ */
const float PZEM_MIN_CURRENT_A = 0.5f;    // pump ON but below this = no draw
const float PZEM_OVERCURRENT_A = 3.0f;
const float PZEM_V_MIN = 215.0f, PZEM_V_MAX = 240.0f;
const unsigned long PZEM_NO_CURRENT_MS = 30000;   // tolerate before PWR_FAIL
const unsigned long PZEM_POLL_MS       = 500;     // throttle Modbus reads (sec.19.2.1; non-blocking)

/* ---- ACS712 mixer current (spec sec.21.3 / 23.2.2.4) --------------------- */
const float ACS712_SENS_V_PER_A = 0.100f; // 20A module ~100 mV/A; 30A ~66mV/A [MEASURE]
const float ACS712_ZERO_V       = 1.65f;  // quiescent output at 0 A           [MEASURE]
const float MIXER_MIN_CURRENT_A = 0.3f;   // mixer ON but below this = no load
const float MIXER_OVERCURRENT_A = 2.5f;

/* ---- pH / EC analog calibration (linear: value = M*adc + B) [MEASURE] ----- */
const float PH_CAL_M = 0.0036621f, PH_CAL_B = 0.0f;   // placeholder [MEASURE]
const float EC_CAL_M = 0.0009766f, EC_CAL_B = 0.0f;   // placeholder [MEASURE]

/* ---- Preventive pump exercise (spec sec.19.4.2) -------------------------- */
const unsigned long PUMP_EXERCISE_INTERVAL_MS = 172800000UL;  // 2 days
const unsigned long PUMP_EXERCISE_RUN_MS      = 5000;         // 5 s

/* ---- Misc ---------------------------------------------------------------- */
const unsigned long HEARTBEAT_MS    = 5000;
const unsigned long NANO_RST_PULSE_MS = 150;
#define WDT_TIMEOUT_S 8

/* =============================================================================
 *  PROTOCOL FRAMING
 * ========================================================================== */
const char *FRAME_START = "<START>";
const char *FRAME_END   = "<END>";

/* =============================================================================
 *  GLOBALS
 * ========================================================================== */
HardwareSerial   esp1Serial(2);
PZEM004Tv30      pzem(Serial1, PZEM_RX_PIN, PZEM_TX_PIN);

uint16_t pcfShadow = 0xFFFF;          // all OFF (active-LOW)

/* ---- Manual TEST mode (dead-man, spec sec.18.10.8) ----------------------- *
 * ESP1 streams TEST,HOLD,<bit> while the operator holds ENTER. ESP2 keeps the
 * relay ON only while those keep arriving (dead-man), one relay at a time, with
 * a HARD 10 s cap regardless of ESP1. The cap lives SOLELY here on ESP2.        */
bool     testMode      = false;
int      testHeldBit   = -1;          // relay currently driven by the dead-man (-1 = none)
bool     testCapped    = false;       // 10 s hard cap reached -> stay OFF until release
unsigned long testRelayOnMs = 0;      // when the current relay went ON (for the cap)
unsigned long testLastHoldMs = 0;     // last TEST,HOLD received (for the release timeout)
const unsigned long TEST_HOLD_TIMEOUT_MS = 400;     // no HOLD within this -> release (fail-safe)
const unsigned long TEST_HARD_CAP_MS     = 10000;   // max continuous ON (spec sec.18.10.8.3)

/* ---- Flow ISR (one active sensor at a time) ------------------------------ */
volatile unsigned long flowPulses = 0;
int activeFlowPin = -1;
void IRAM_ATTR flowISR() { flowPulses++; }

/* ---- Work order ---------------------------------------------------------- */
struct WorkOrder {
  bool   active;
  int    col;            // 0..2
  bool   fertigate;
  float  flushPct;       // 0..100
  float  dose[3];        // mL A,B,C
  float  ecLo, ecHi, phLo, phHi;
  unsigned long mixMs;
  String cmdName;        // for ACK/DONE echo
};
WorkOrder wo;

/* ---- Sequence FSM -------------------------------------------------------- */
enum SeqStep {
  SEQ_NONE, SEQ_FILL, SEQ_DOSE, SEQ_MIX, SEQ_ECPH,
  SEQ_DELIVER, SEQ_FLUSH_FILL, SEQ_FLUSH_DELIVER
};
SeqStep step = SEQ_NONE;
bool    stepInit = false;
int     doseIdx = 0;
bool    ecphFailed = false;
unsigned long stepStart = 0;

/* ---- Metered-stage state ------------------------------------------------- */
int   sgPump = -1, sgFlow = -1;
float sgK = 1, sgTarget = 0;
bool  sgIsAC = false, sgPumpOn = false, sgSawFlow = false;
unsigned long sgT0 = 0;
unsigned long acNoCurrentSince = 0;
unsigned long lastPzemMs = 0;     // PZEM read throttle (see PZEM_POLL_MS)

/* ---- Misc state ---------------------------------------------------------- */
unsigned long lastHeartbeat = 0;
unsigned long nanoRstOffAt = 0;
unsigned long lastRunAC[3] = { 0, 0, 0 };   // transfer, booster, mixer last-run (exercise)
const uint8_t EX_BIT[3] = { OUT_TRANSFER, OUT_BOOSTER, OUT_MIXER };
int  exerciseIdx = -1;
unsigned long exerciseOffAt = 0;
String rxLine;

/* ---- Forward declarations ------------------------------------------------ */
void wdtSetup();
void feedWDT();
void pcfWrite(uint16_t s);
void pcfOn(uint8_t bit);
void pcfOff(uint8_t bit);
void stopAll();
void reply(const String &body);
void attachFlow(int pin);
void detachFlow();
float litersSoFar(float k);
void pollEsp1();
void dispatch(const String &payload);
void startWorkOrder();
void runSequence();
void stageBegin(int pumpBit, bool isAC, int flowPin, float k, float targetL);
int  stagePoll();          // 1 done, 0 running, -1 no-flow fail
void stageEnd();
void goStep(SeqStep s);
void finishOk();
void failSequence(const char *resp, const char *loc);
void safetyMonitor();
float readMixerCurrent();
float readPH();
float readEC();
void pumpExercise();
void heartbeat();
void testSafety();

/* =============================================================================
 *  SETUP  --  SAFE boot: all outputs OFF before anything else (sec.19.5.4)
 * ========================================================================== */
void setup() {
  Serial.begin(DEBUG_BAUD);
  delay(50);
  Serial.println(F("\n=== ESP32 #2 Actuator Controller boot [DEBUG: fake sensors/flow] ==="));

  Wire.begin(I2C_SDA, I2C_SCL);
  pcfWrite(0xFFFF);                         // ALL actuators OFF first (SAFE state)

  esp1Serial.begin(ESP1_BAUD, SERIAL_8N1, ESP1_RX_PIN, ESP1_TX_PIN);
  pzem.setAddress(0x01);                    // default PZEM Modbus address (begins Serial1)

  analogReadResolution(12);                 // 0..4095

  wo.active = false;
  unsigned long now = millis();
  lastRunAC[0] = lastRunAC[1] = lastRunAC[2] = now;
  lastHeartbeat = now;
  rxLine.reserve(160);

  wdtSetup();
  reply("READY,ESP2");                      // tell ESP32 #1 we are up & SAFE (sec.10.7.4)
  Serial.println(F("Setup complete -> SAFE/IDLE, sent READY"));
}

/* =============================================================================
 *  LOOP  (non-blocking)
 * ========================================================================== */
void loop() {
  feedWDT();
  pollEsp1();
  runSequence();
  safetyMonitor();
  testSafety();
  pumpExercise();
  heartbeat();

  // non-blocking Nano-RESET pulse completion
  if (nanoRstOffAt && millis() >= nanoRstOffAt) {
    pcfOff(OUT_NANO_RST);
    nanoRstOffAt = 0;
    reply("DONE,RESET_NANO");
  }
}

/* =============================================================================
 *  WATCHDOG
 * ========================================================================== */
void wdtSetup() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = { .timeout_ms = WDT_TIMEOUT_S * 1000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_deinit();
  esp_task_wdt_init(&cfg);
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
#endif
}
void feedWDT() { esp_task_wdt_reset(); }

/* =============================================================================
 *  PCF8575  (raw I2C, active-LOW -- proven pattern)
 * ========================================================================== */
void pcfWrite(uint16_t s) {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(lowByte(s));
  Wire.write(highByte(s));
  Wire.endTransmission();
  pcfShadow = s;
}
void pcfOn(uint8_t bit)  { pcfWrite(pcfShadow & ~(1 << bit)); }   // clear bit = ON
void pcfOff(uint8_t bit) { pcfWrite(pcfShadow |  (1 << bit)); }   // set bit  = OFF
void stopAll()           { pcfWrite(0xFFFF); }

/* =============================================================================
 *  UART reply helper
 * ========================================================================== */
void reply(const String &body) {
  esp1Serial.print(FRAME_START);
  esp1Serial.print(",");
  esp1Serial.print(body);
  esp1Serial.print(",");
  esp1Serial.println(FRAME_END);
  Serial.println(">> " + body);
}

/* =============================================================================
 *  FLOW METERING (stage-based interrupt)
 * ========================================================================== */
void attachFlow(int pin) {
  flowPulses = 0;
  activeFlowPin = pin;
  pinMode(pin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(pin), flowISR, FALLING);
}
void detachFlow() {
  if (activeFlowPin >= 0) { detachInterrupt(digitalPinToInterrupt(activeFlowPin)); activeFlowPin = -1; }
}
float litersSoFar(float k) {
#if DEBUG_FAKE
  // Fake metering: liters ramp with elapsed pump-on time so every metered stage
  // reaches its target and completes (no real flow sensors). sgT0 is reset to
  // millis() the instant the stage pump turns ON (see stagePoll), so this is
  // "seconds since pump start * simulated L/s".
  (void)k;
  if (sgPumpOn) return (float)(millis() - sgT0) / 1000.0f * SIM_FLOW_LPS;
  return 0.0f;
#else
  noInterrupts(); unsigned long p = flowPulses; interrupts();
  return (float)p / k;
#endif
}

/* =============================================================================
 *  ESP1 LINK  --  parse framed work orders
 * ========================================================================== */
void pollEsp1() {
  while (esp1Serial.available()) {
    char c = (char)esp1Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxLine.length() > 0) {
        String raw = rxLine; rxLine = "";
        int s = raw.indexOf(FRAME_START), e = raw.indexOf(FRAME_END);
        if (s < 0 || e <= s || raw.length() > 128) { reply("INVALID,FRAMING"); continue; }
        String p = raw.substring(s + 7, e);
        p.trim();
        if (p.startsWith(",")) p = p.substring(1);
        if (p.endsWith(","))   p = p.substring(0, p.length() - 1);
        dispatch(p);
      }
    } else if (rxLine.length() < 150) rxLine += c; else rxLine = "";
  }
}

void dispatch(const String &payload) {
  // tokenize
  const int MAXT = 24; String tok[MAXT]; int n = 0, start = 0;
  for (int i = 0; i <= payload.length() && n < MAXT; i++) {
    if (i == payload.length() || payload[i] == ',') { tok[n++] = payload.substring(start, i); start = i + 1; }
  }
  if (n == 0) { reply("INVALID,EMPTY"); return; }
  String cmd = tok[0];

  // ---- always-honored commands ----
  if (cmd == "STOP_ALL") {
    stopAll(); step = SEQ_NONE; wo.active = false; detachFlow();
    testMode = false; testHeldBit = -1; testCapped = false;
    reply("DONE,STOP_ALL");
    return;
  }
  if (cmd == "STATUS_REQ") { reply("STATUS,ESP2,OK"); return; }

  // ---- manual relay test, dead-man (ESP1 Settings>Testing, spec sec.18.10.8) ----
  if (cmd == "TEST") {
    String sub = (n >= 2) ? tok[1] : "";
    if (sub == "ENTER") {                                  // enter TEST_MODE, force SAFE
      if (wo.active || step != SEQ_NONE) { reply("BUSY,ACTIVE"); return; }
      stopAll(); testMode = true; testHeldBit = -1; testCapped = false;
      reply("ACK,TEST,ENTER"); return;
    }
    if (sub == "EXIT") { stopAll(); testMode = false; testHeldBit = -1; reply("DONE,TEST"); return; }
    if (sub == "RELEASE") {                                // explicit button-up
      if (testHeldBit >= 0) pcfOff(testHeldBit);
      testHeldBit = -1; testCapped = false; return;
    }
    if (sub == "HOLD") {                                   // dead-man keep-alive for <bit>
      if (!testMode || n < 3) return;
      int bit = tok[2].toInt();
      if (bit < 0 || bit > 15) { reply("INVALID,TEST_BIT"); return; }
      if (bit != testHeldBit) {                            // new selection -> one-at-a-time switch
        stopAll(); pcfOn(bit);
        testHeldBit = bit; testRelayOnMs = millis(); testCapped = false;
      }
      testLastHoldMs = millis();                           // refresh dead-man timer
      return;
    }
    reply("INVALID,TEST_SUB"); return;
  }
  if (cmd == "RESET_NANO") {
    pcfOn(OUT_NANO_RST);                    // transistor pulls Nano RESET low
    nanoRstOffAt = millis() + NANO_RST_PULSE_MS;
    reply("ACK,RESET_NANO");
    return;
  }

  // ---- sequence commands ----
  bool isIrr  = cmd.startsWith("SEQ_IRRIGATION_");
  bool isFert = cmd.startsWith("SEQ_FERTIGATION_");
  if (!isIrr && !isFert) { reply("INVALID,UNKNOWN_CMD"); return; }
  if (wo.active || step != SEQ_NONE) { reply("BUSY,ACTIVE"); return; }

  char colCh = cmd.charAt(cmd.length() - 1);
  int c = (colCh == 'A') ? 0 : (colCh == 'B') ? 1 : (colCh == 'C') ? 2 : -1;
  if (c < 0) { reply("INVALID,COLUMN"); return; }
  if (!COLUMN_ENABLED[c]) { reply("INVALID,COL_DISABLED"); return; }

  // defaults
  wo.col = c; wo.fertigate = isFert; wo.flushPct = 0;
  wo.dose[0] = wo.dose[1] = wo.dose[2] = 0;
  wo.ecLo = 0.0f; wo.ecHi = 99; wo.phLo = 0; wo.phHi = 14;   // wide defaults; ESP1 sends real window
  wo.mixMs = MIX_DEFAULT_MS; wo.cmdName = cmd;

  // parse key/value groups
  for (int i = 1; i < n; i++) {
    if (tok[i] == "FLUSH" && i + 1 < n)      { wo.flushPct = tok[++i].toFloat(); }
    else if (tok[i] == "DOSE" && i + 3 < n)  { wo.dose[0] = tok[++i].toFloat(); wo.dose[1] = tok[++i].toFloat(); wo.dose[2] = tok[++i].toFloat(); }
    else if (tok[i] == "EC" && i + 2 < n)    { wo.ecLo = tok[++i].toFloat(); wo.ecHi = tok[++i].toFloat(); }
    else if (tok[i] == "PH" && i + 2 < n)    { wo.phLo = tok[++i].toFloat(); wo.phHi = tok[++i].toFloat(); }
    else if (tok[i] == "MIX" && i + 1 < n)   { wo.mixMs = (unsigned long)tok[++i].toInt(); }
  }
  if (wo.flushPct < 0) wo.flushPct = 0; if (wo.flushPct > 90) wo.flushPct = 90;

  reply("ACK," + wo.cmdName);
  startWorkOrder();
}

/* =============================================================================
 *  SEQUENCE ENGINE  (non-blocking FSM; documented plumbing)
 * ========================================================================== */
void startWorkOrder() {
  wo.active = true;
  ecphFailed = false;
  doseIdx = 0;
  Serial.printf("Work order: %s fert=%d flush=%.0f%%\n", wo.cmdName.c_str(), wo.fertigate, wo.flushPct);
  goStep(SEQ_FILL);
}

void goStep(SeqStep s) { step = s; stepInit = false; }

void stageBegin(int pumpBit, bool isAC, int flowPin, float k, float targetL) {
  sgPump = pumpBit; sgIsAC = isAC; sgFlow = flowPin; sgK = k; sgTarget = targetL;
  sgPumpOn = false; sgSawFlow = false; sgT0 = millis();
  acNoCurrentSince = 0;
  if (isAC) pcfOn(OUT_INVERTER);
}
int stagePoll() {
  if (!sgPumpOn) {                          // valve/inverter settle, then start pump
    if (millis() - sgT0 >= (sgIsAC ? INVERTER_WARMUP_MS : VALVE_SWITCH_MS)) {
      attachFlow(sgFlow); pcfOn(sgPump); sgPumpOn = true; sgT0 = millis();
      if (sgPump == OUT_TRANSFER) lastRunAC[0] = millis();
      if (sgPump == OUT_BOOSTER)  lastRunAC[1] = millis();
    }
    return 0;
  }
  float L = litersSoFar(sgK);
  if (L > 0.0005f) sgSawFlow = true;
  if (L >= sgTarget) return 1;
  unsigned long el = millis() - sgT0;
  if (!sgSawFlow && el > FLOW_TIMEOUT_MS) return -1;   // dry run / no flow
  if (el > STAGE_MAX_MS) return -1;
  return 0;
}
void stageEnd() {
  if (sgPump >= 0) pcfOff(sgPump);
  detachFlow();
  sgPump = -1; sgPumpOn = false;
}

void finishOk() {
  stopAll();
  String name = wo.cmdName;
  wo.active = false; step = SEQ_NONE;
  reply("DONE," + name);
}
void failSequence(const char *resp, const char *loc) {
  stageEnd();
  stopAll();
  wo.active = false; step = SEQ_NONE;
  reply(String(resp) + "," + loc);
}

void runSequence() {
  if (step == SEQ_NONE) return;
  int c = wo.col;
  const char *colLoc = (c == 0) ? "COL_A" : (c == 1) ? "COL_B" : "COL_C";

  switch (step) {

    /* ---- FILL: reservoir -> mixing tank (transfer pump, flow 4) ---------- */
    case SEQ_FILL: {
      if (!stepInit) {
        float target = wo.fertigate ? WATER_BUDGET_L[c] * (1.0f - wo.flushPct / 100.0f)
                                     : WATER_BUDGET_L[c];
        pcfOn(OUT_RES_VALVE);
        stageBegin(OUT_TRANSFER, true, FLOW_RES_MIX, K_RES_MIX, target);
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1)      { stageEnd(); pcfOff(OUT_RES_VALVE); goStep(wo.fertigate ? SEQ_DOSE : SEQ_DELIVER); }
      else if (r == -1){ pcfOff(OUT_RES_VALVE); failSequence("FLOW_FAIL", "TRANSFER"); }
      break;
    }

    /* ---- DOSE: nutrients A/B/C into mixing tank (sequential) ------------- */
    case SEQ_DOSE: {
      if (!stepInit) {
        // skip zero / disabled doses
        while (doseIdx < 3 && wo.dose[doseIdx] <= 0.0f) doseIdx++;
        if (doseIdx >= 3) { goStep(SEQ_MIX); break; }
        float targetL = wo.dose[doseIdx] / 1000.0f;     // mL -> L
        stageBegin(NUT_PUMP_BIT[doseIdx], false, NUT_FLOW_PIN[doseIdx], K_NUT[doseIdx], targetL);
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1) {
        stageEnd();
        Serial.printf("Dosed nutrient %d: %.1f mL\n", doseIdx, wo.dose[doseIdx]);
        doseIdx++;
        stepInit = false;                    // re-enter to dose next (or advance to MIX)
        if (doseIdx >= 3) goStep(SEQ_MIX);
      } else if (r == -1) {
        // nutrient channel no-flow: disable that channel, continue (spec sec.23.2.2.1).
        // DEGRADED (not ERROR) so ESP1 classifies Major and does NOT critical-stop/cut power.
        stageEnd();
        reply(String("DEGRADED,NUT_") + (char)('A' + doseIdx));
        doseIdx++;
        stepInit = false;
        if (doseIdx >= 3) goStep(SEQ_MIX);
      }
      break;
    }

    /* ---- MIX: homogenize; validate mixer current (ACS712) ---------------- */
    case SEQ_MIX: {
      if (!stepInit) { pcfOn(OUT_INVERTER); pcfOn(OUT_MIXER); lastRunAC[2] = millis(); stepStart = millis(); stepInit = true; }
      float mi = readMixerCurrent();
      if (mi > MIXER_OVERCURRENT_A) { pcfOff(OUT_MIXER); failSequence("SAFE_STOP", "MIXER_OC"); break; }
      if (millis() - stepStart >= wo.mixMs) {
        // no-load check at end of mix window (spec sec.23.2.2.4). DEGRADED (not ERROR):
        // Major condition, ESP1 must not critical-stop -- the batch still delivers.
        if (mi < MIXER_MIN_CURRENT_A) { pcfOff(OUT_MIXER); reply("DEGRADED,MIXER_NOLOAD"); }
        pcfOff(OUT_MIXER);
        goStep(SEQ_ECPH);
      }
      break;
    }

    /* ---- EC/pH validation (local protective check, spec sec.14.2.4) ------ */
    case SEQ_ECPH: {
      float ph = readPH(), ec = readEC();
      if (ph < wo.phLo || ph > wo.phHi) { ecphFailed = true; reply(String("PH_FAIL,") + colLoc); }
      else if (ec < wo.ecLo || ec > wo.ecHi) { ecphFailed = true; reply(String("EC_FAIL,") + colLoc); }
      // Plumbing has no drain: deliver the mixed batch regardless, fault already reported.
      goStep(SEQ_DELIVER);
      break;
    }

    /* ---- DELIVER: mixing tank -> column (booster pump, flow 5) ----------- */
    case SEQ_DELIVER: {
      if (!stepInit) {
        float target = wo.fertigate ? WATER_BUDGET_L[c] * (1.0f - wo.flushPct / 100.0f)
                                     : WATER_BUDGET_L[c];
        pcfOn(OUT_MIX_VALVE); pcfOn(COL_VALVE_BIT[c]);
        stageBegin(OUT_BOOSTER, true, FLOW_MIX_IRR, K_MIX_IRR, target);
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1) {
        stageEnd(); pcfOff(OUT_MIX_VALVE); pcfOff(COL_VALVE_BIT[c]);
        if (wo.fertigate && wo.flushPct > 0) goStep(SEQ_FLUSH_FILL);
        else finishOk();
      } else if (r == -1) {
        pcfOff(OUT_MIX_VALVE); pcfOff(COL_VALVE_BIT[c]);
        failSequence("FLOW_FAIL", "MAIN");
      }
      break;
    }

    /* ---- FLUSH_FILL: plain reservoir water -> mixing tank ---------------- */
    case SEQ_FLUSH_FILL: {
      if (!stepInit) {
        float target = WATER_BUDGET_L[c] * (wo.flushPct / 100.0f);
        pcfOn(OUT_RES_VALVE);
        stageBegin(OUT_TRANSFER, true, FLOW_RES_MIX, K_RES_MIX, target);
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1)      { stageEnd(); pcfOff(OUT_RES_VALVE); goStep(SEQ_FLUSH_DELIVER); }
      else if (r == -1){ pcfOff(OUT_RES_VALVE); failSequence("FLOW_FAIL", "TRANSFER"); }
      break;
    }

    /* ---- FLUSH_DELIVER: plain water -> column (final flush step) --------- */
    case SEQ_FLUSH_DELIVER: {
      if (!stepInit) {
        float target = WATER_BUDGET_L[c] * (wo.flushPct / 100.0f);
        pcfOn(OUT_MIX_VALVE); pcfOn(COL_VALVE_BIT[c]);
        stageBegin(OUT_BOOSTER, true, FLOW_MIX_IRR, K_MIX_IRR, target);
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1)      { stageEnd(); pcfOff(OUT_MIX_VALVE); pcfOff(COL_VALVE_BIT[c]); finishOk(); }
      else if (r == -1){ pcfOff(OUT_MIX_VALVE); pcfOff(COL_VALVE_BIT[c]); failSequence("FLOW_FAIL", "MAIN"); }
      break;
    }

    default: break;
  }
}

/* =============================================================================
 *  SAFETY MONITOR  --  immediate local protective stops (spec sec.9.8.1.2)
 * ========================================================================== */
void safetyMonitor() {
  // PZEM validation only while an AC pump is actively running
  if (step == SEQ_NONE || !sgPumpOn || !sgIsAC) { acNoCurrentSince = 0; return; }

  // Throttle the (blocking) Modbus reads so we don't poll every loop iteration.
  if (millis() - lastPzemMs < PZEM_POLL_MS) return;
  lastPzemMs = millis();

#if DEBUG_FAKE
  float v = 220.0f;                         // fake nominal mains voltage
  float i = sgPumpOn ? 1.5f : 0.0f;         // fake healthy pump current while ON
#else
  float v = pzem.voltage();
  float i = pzem.current();
#endif
  if (isnan(v) || isnan(i)) return;         // PZEM absent/unreadable -> skip (graceful)

  if (i > PZEM_OVERCURRENT_A)        { failSequence("PWR_FAIL", "OVERCURRENT"); return; }
  if (v < PZEM_V_MIN || v > PZEM_V_MAX) { failSequence("PWR_FAIL", "VOLTAGE"); return; }

  if (i < PZEM_MIN_CURRENT_A) {             // pump ON but drawing no current
    if (acNoCurrentSince == 0) acNoCurrentSince = millis();
    else if (millis() - acNoCurrentSince > PZEM_NO_CURRENT_MS) { failSequence("PWR_FAIL", "NO_CURRENT"); return; }
  } else {
    acNoCurrentSince = 0;
  }
}

float readMixerCurrent() {
#if DEBUG_FAKE
  return 1.0f;    // fake healthy mixer current (between no-load and overcurrent)
#else
  int raw = analogRead(PIN_MIXER_I);
  float v = (raw / 4095.0f) * 3.3f;
  float a = (v - ACS712_ZERO_V) / ACS712_SENS_V_PER_A;
  return fabs(a);
#endif
}
#if DEBUG_FAKE
float readPH() { return 6.0f; }   // fake pH inside the safe window
float readEC() { return 1.5f; }   // fake EC inside the safe window
#else
float readPH() { return PH_CAL_M * analogRead(PIN_PH) + PH_CAL_B; }
float readEC() { return EC_CAL_M * analogRead(PIN_EC) + EC_CAL_B; }
#endif

/* =============================================================================
 *  PREVENTIVE PUMP EXERCISE  (spec sec.19.4.2)  -- only when idle
 * ========================================================================== */
void pumpExercise() {
  if (exerciseIdx >= 0) {                    // an exercise run is in progress
    if (millis() >= exerciseOffAt) {
      pcfOff(EX_BIT[exerciseIdx]);
      if (EX_BIT[exerciseIdx] == OUT_TRANSFER || EX_BIT[exerciseIdx] == OUT_BOOSTER || EX_BIT[exerciseIdx] == OUT_MIXER)
        pcfOff(OUT_INVERTER);
      lastRunAC[exerciseIdx] = millis();
      exerciseIdx = -1;
    }
    return;
  }
  if (wo.active || step != SEQ_NONE || testMode) return; // never exercise during a work order / manual test
  for (int k = 0; k < 3; k++) {
    if (millis() - lastRunAC[k] > PUMP_EXERCISE_INTERVAL_MS) {
      exerciseIdx = k;
      pcfOn(OUT_INVERTER);
      pcfOn(EX_BIT[k]);
      exerciseOffAt = millis() + PUMP_EXERCISE_RUN_MS;
      Serial.printf("Pump exercise: actuator bit %d\n", EX_BIT[k]);
      break;
    }
  }
}

/* =============================================================================
 *  HEARTBEAT  (spec sec.10.8.3)
 * ========================================================================== */
void heartbeat() {
  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = millis();
    reply("STATUS,ESP2,OK");
  }
}

/* =============================================================================
 *  TEST DEAD-MAN ENFORCEMENT  (sole safety authority in TEST mode, sec.18.10.8.3)
 * ========================================================================== */
void testSafety() {
  if (!testMode || testHeldBit < 0) return;
  unsigned long now = millis();
  // 10 s HARD CAP: force OFF regardless of ESP1, stay capped until the operator
  // releases (HOLD stream stops -> the timeout below clears testHeldBit).
  if (!testCapped && now - testRelayOnMs >= TEST_HARD_CAP_MS) {
    pcfOff(testHeldBit); testCapped = true;
    reply("DONE,TEST_CAP");
  }
  // Dead-man: no HOLD within the timeout (button released, ESP1 hung, or link lost)
  // -> relay OFF and clear selection (fail-safe).
  if (now - testLastHoldMs > TEST_HOLD_TIMEOUT_MS) {
    if (!testCapped) pcfOff(testHeldBit);
    testHeldBit = -1; testCapped = false;
  }
}
