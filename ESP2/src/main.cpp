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
 *                 DCEIL,<sA>,<sB>,<sC>,BATCHV,<L>,EC,<lo>,<hi>,PH,<lo>,<hi>,MIX,<ms>,<END>
 *                 (DCEIL = per-dose 2x timed ceiling s; a dose past it -> DOSE_TIMEOUT hold, dosing spec §2.4/§5)
 *         <START>,STOP_ALL,<END> / RESET_SELF / STATUS_REQ
 *         RESUME,<NORMAL|IRRIGATE|RELEASE>  (user-gated fault recovery, sec.19.4.8.2):
 *                 NORMAL  = re-energize P17, resume the paused sequence from where it stopped;
 *                 IRRIGATE= finish as irrigation only (top up plain water to budget, no dosing);
 *                 RELEASE = dump the current tank contents to the assigned column as-is.
 *         EXERCISE,<TRANSFER|BOOSTER|MIXER>  (preventive 5 s pump run, scheduled by
 *                 ESP1 sec.14.9.1 -> ACK,EXERCISE then DONE,EXERCISE,<pump>)
 *         CAL_START,<SENSOR_ID> / CAL_STOP,<SENSOR_ID>  (calibration raw stream, spec §A.3):
 *                 ESP2 streams CAL,<id>,<raw> ~1.6 Hz for pH/EC/ACS712/FLOW_*; idle-only.
 *         SET_CAL,<SENSOR_ID>,<VALUE>[,<VALUE2>]  (push a runtime cal const; ACK,SET_CAL,<id>)
 *         PRIME_START,<LINE_ID>[,<COL>] / PRIME_STOP,<LINE_ID>  (purge a line: valve(s)+pump
 *                 under dead-man + generous cap, spec §A.4.2)
 *         TEST,ENTER / TEST,EXIT / TEST,HOLD,<bit> / TEST,RELEASE  (dead-man manual
 *                 relay test, sec.18.10.8): ESP1 streams HOLD while ENTER held; ESP2
 *                 keeps ONE relay ON only while HOLD arrives (timeout), with a 10s
 *                 hard cap. bit = PCF8575 OUT_* index 0..15.
 *    OUT: ACK,<cmd> -> DONE,<cmd>[,WATER,<L>];  DOSE,NUT_x,<target>,<measured>,COL_y
 *         (per-nutrient result, sec.25.2.1); FLOW_FAIL/PWR_FAIL/EC_FAIL/PH_FAIL/
 *         SAFE_STOP/ERROR; DOSE_TIMEOUT,NUT_x (dosing pump past its ceiling -> ESP1 fault-hold);
 *         DEGRADED,<chan> (channel disabled, sequence continues ->
 *         ESP1 Major, no shutdown); BUSY,ACTIVE/TEST/HELD; INVALID,<reason>;
 *         PCF_FAIL,I2C (relay bus dead -> ESP1 Major) + PCF_OK (recovered);
 *         SENSOR_FAIL,EC / SENSOR_FAIL,PH (probe railed/disconnected -> ESP1 Major,
 *         distinct from EC_FAIL/PH_FAIL = out-of-window); STATUS,ESP2,OK; READY,ESP2;
 *         DONE,EXERCISE,<pump>; DONE,RESUME,<mode>
 *
 *  P17 MASTER ACTUATOR-POWER CUTOFF & FAULT HOLD (sec.19.4.8): PCF8575 P17 (bit 15) is
 *  a master relay gating power to the whole P0-P16 actuator bank (fail-safe: loss of P17 =
 *  all actuators OFF). On a hard fault ESP2 drops P17, PAUSES the sequence, and STAYS ALIVE
 *  holding the mixing-tank volume (it does NOT power itself off -- that would forget the
 *  water and overfill on resume). Recovery is the user-gated RESUME,<mode> above. The mix-tank
 *  volume is metered from ESP2's OWN flow sensors and persisted to NVS (survives a reboot);
 *  the Nano ultrasonic level is display-only and NOT trusted for control.
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
 *    The mixing tank's ONLY outlet is the booster -> a column (no drain line).
 *
 *  >>> Values to MEASURE / set at commissioning are tagged [MEASURE] / [TBD].
 * ========================================================================== */

#include <Arduino.h>
#include <Wire.h>
#include <PZEM004Tv30.h>
#include <esp_task_wdt.h>
#include <Preferences.h>       // NVS: persist mixing-tank volume across reboot/power-down

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

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
#define OUT_MASTER_CUTOFF 15 // P17 master actuator-power cutoff (sec.19.4.8; was Nano RESET).
                             // Gates power to the whole P0-P16 bank. Fail-safe: de-energized = all OFF.
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

/* ---- Flow K-factors (pulses per LITER) [MEASURE] ------------------------- *
 * RUNTIME (not const): set on the fly by ESP1 Calibration Mode via SET_CAL, refreshed at
 * STARTUP_SYNC and carried in each work order (spec §A.5.1). Defaults are the bench values.  */
float K_RES_MIX = 450.0f;
float K_MIX_IRR = 450.0f;
float K_NUT[3]   = { 450.0f, 450.0f, 450.0f };   // small dosing sensors differ [MEASURE]

/* ---- Per-column water budget (liters per service) [TBD] ------------------ */
const float WATER_BUDGET_L[3] = { 5.0f, 5.0f, 5.0f };
bool COLUMN_ENABLED[3] = { true, true, false };        // [CONFIRM] mirror ESP1/Nano

/* ---- Stage timings ------------------------------------------------------- */
const unsigned long VALVE_SWITCH_MS   = 1000;   // valve settle before pump (DC stages)
const unsigned long INVERTER_WARMUP_MS = 2000;  // inverter+valve settle before AC pump
const unsigned long FLOW_TIMEOUT_MS   = 10000;  // no-flow after pump start -> FLOW_FAIL
const unsigned long STAGE_MAX_MS      = 180000; // hard safety cap per metered stage
const unsigned long MIX_DEFAULT_MS    = 30000;  // used if work order omits MIX

/* ---- PZEM AC validation (spec sec.23.1.2.1) ------------------------------ */
const float PZEM_MIN_CURRENT_A = 0.5f;    // pump ON but below this = no draw
const float PZEM_OVERCURRENT_A = 3.0f;
const float PZEM_V_MIN = 215.0f, PZEM_V_MAX = 240.0f;
const unsigned long PZEM_NO_CURRENT_MS = 30000;   // tolerate before PWR_FAIL
const unsigned long PZEM_POLL_MS       = 500;     // throttle Modbus reads (sec.19.2.1; non-blocking)

/* ---- ACS712 mixer current (spec sec.21.3 / 23.2.2.4) --------------------- */
const float ACS712_SENS_V_PER_A = 0.100f; // 20A module ~100 mV/A; 30A ~66mV/A [MEASURE]
float       ACS712_ZERO_V       = 1.65f;  // quiescent output at 0 A -- RUNTIME (SET_CAL ACS712)
const float MIXER_MIN_CURRENT_A = 0.3f;   // mixer ON but below this = no load
const float MIXER_OVERCURRENT_A = 2.5f;

/* ---- pH / EC analog calibration (linear: value = M*adc + B) [MEASURE] ----- *
 * RUNTIME (not const): set by ESP1 Calibration Mode (SET_CAL PH/EC), refreshed at startup
 * sync + in each work order. Stored as slope (M) + offset (B). Defaults are bench values.     */
float PH_CAL_M = 0.0036621f, PH_CAL_B = 0.0f;   // [MEASURE]
float EC_CAL_M = 0.0009766f, EC_CAL_B = 0.0f;   // [MEASURE]

/* ---- Preventive pump exercise (spec sec.19.4.2) -------------------------- *
 * The 2-day SCHEDULE lives on ESP1 (always-on/RTC); ESP1 sends EXERCISE,<pump>.
 * ESP2 only owns the run DURATION below.                                       */
const unsigned long PUMP_EXERCISE_RUN_MS = 5000;             // 5 s

/* ---- Misc ---------------------------------------------------------------- */
const unsigned long HEARTBEAT_MS    = 5000;
const unsigned long IDLE_RESET_MS   = 1800000;  // 30 min continuously idle -> self-reboot (self-heal)
#define WDT_TIMEOUT_S 8

/* ---- Calibration Mode support (companion spec §A) ------------------------ *
 * CAL streaming: while ESP1 has a sensor selected, ESP2 streams that one raw value fast.
 * Prime: open a line's valve(s) + run its paired pump under a dead-man + generous cap.      */
const unsigned long CAL_STREAM_MS        = 600;     // ~1.6 Hz raw stream cadence
const unsigned long PRIME_CAP_MS         = 45000;   // generous hard cap for purging air [TBD]
const unsigned long PRIME_HOLD_TIMEOUT_MS = 400;    // no keep-alive within this -> stop (dead-man)

/* ---- Mixing-tank volume tolerance ---------------------------------------- *
 * A FILL stage whose remaining target is below this is treated as "already full"
 * (no pump run) -- avoids a zero/negative-target pump cycle when resuming.       */
const float TANK_EPS_L = 0.05f;

/* ---- PCF8575 health / fault reporting ------------------------------------ */
const unsigned long PCF_PROBE_MS    = 2000;    // active I2C probe cadence
const uint8_t       PCF_FAIL_LIMIT  = 3;       // consecutive failures before declaring a fault
const unsigned long PCF_REPORT_MS   = 10000;   // re-assert PCF_FAIL this often while faulted

/* ---- EC / pH sensor-fault rails (raw ADC, 12-bit) ------------------------ *
 * A railed reading means the probe is disconnected/shorted -- distinct from an
 * in-range value that is merely outside the EC/pH safe window.       [MEASURE] */
const int EC_ADC_FAULT_LO = 5,  EC_ADC_FAULT_HI = 4090;
const int PH_ADC_FAULT_LO = 5,  PH_ADC_FAULT_HI = 4090;

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

/* ---- PCF8575 health state ------------------------------------------------ */
uint8_t  pcfFailCount = 0;            // consecutive write/probe failures
bool     pcfFaultReported = false;    // PCF_FAIL currently asserted to ESP1
unsigned long lastPcfProbeMs = 0;
unsigned long lastPcfReportMs = 0;

/* ---- Manual TEST mode (dead-man, spec sec.18.10.8) ----------------------- *
 * ESP1 streams TEST,HOLD,<bit> while the operator holds ENTER. ESP2 keeps the
 * relay ON only while those keep arriving (dead-man), one relay at a time, with
 * a HARD 10 s cap regardless of ESP1. The cap lives SOLELY here on ESP2.        */
bool     testMode      = false;
int      testHeldBit   = -1;          // relay currently driven by the dead-man (-1 = none)
unsigned long idleSinceMs = 0;        // when ESP2 became idle (0 = not idle now); drives IDLE_RESET_MS self-heal
bool     testCapped    = false;       // 10 s hard cap reached -> stay OFF until release
unsigned long testRelayOnMs = 0;      // when the current relay went ON (for the cap)
unsigned long testLastHoldMs = 0;     // last TEST,HOLD received (for the release timeout)
const unsigned long TEST_HOLD_TIMEOUT_MS = 400;     // no HOLD within this -> release (fail-safe)
const unsigned long TEST_HARD_CAP_MS     = 10000;   // max continuous ON, single relay (sec.18.10.8.3)
const unsigned long TEST_COMBO_CAP_MS    = 30000;   // longer cap for valve+pump PRIMING combos

/* ---- Testing valve+pump priming combos (TEST,HOLD idx > 15) --------------- *
 * idx 16 = Fill (Reservoir -> Mixing), 17/18/19 = Push (Mixing -> Column A/B/C). Each lists the
 * PCF8575 OUT_* bits energized together (inverter first, then valve(s), then the pump).            */
const uint8_t COMBO_FILL[]   = { OUT_INVERTER, OUT_RES_VALVE, OUT_TRANSFER };
const uint8_t COMBO_PUSH_A[] = { OUT_INVERTER, OUT_MIX_VALVE, OUT_COL_A, OUT_BOOSTER };
const uint8_t COMBO_PUSH_B[] = { OUT_INVERTER, OUT_MIX_VALVE, OUT_COL_B, OUT_BOOSTER };
const uint8_t COMBO_PUSH_C[] = { OUT_INVERTER, OUT_MIX_VALVE, OUT_COL_C, OUT_BOOSTER };

/* ---- Calibration: raw stream + Prime (companion spec §A) ----------------- */
String   calId = "";                  // active CAL sensor id ("" = none)
unsigned long lastCalMs = 0;
// Prime dead-man (valve+pump pairing, §A.4.2): one line at a time, generous cap.
String   primeLine = "";              // active prime line ("" = none)
int      primeValveA = -1, primeValveB = -1, primePump = -1;
bool     primeIsAC = false, primeCapped = false;
unsigned long primeOnMs = 0, primeLastMs = 0;

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
  unsigned long doseCeilMs[3];   // per-dose 2x timed ceiling (ms); DOSE_TIMEOUT if exceeded (dosing spec §2.4)
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
float   woWaterL = 0.0f;        // metered water delivered to the column this work order (for DONE,WATER)

/* ---- Mixing-tank volume + fault hold (sec.19.4.8) ------------------------- *
 * mixTankL is the authoritative liters currently in the mixing tank, metered from
 * ESP2's OWN fill/deliver flow sensors (the Nano ultrasonic is display-only). It is
 * persisted to NVS so a reboot/power-down can NEVER make ESP2 forget the water and
 * overfill on the next run. faultHeld pauses the sequence after a hard fault until the
 * user-gated RESUME,<mode> arrives; the work order + step are kept (not cleared).      */
Preferences prefs;
float   mixTankL  = 0.0f;       // liters currently in the mixing tank (persisted)
bool    faultHeld = false;      // sequence paused after a fault, awaiting RESUME

/* ---- Metered-stage state ------------------------------------------------- */
int   sgPump = -1, sgFlow = -1;
float sgK = 1, sgTarget = 0;
int   sgTankSign = 0;          // +1 = this stage FILLS the mix tank, -1 = DRAINS it, 0 = neither (dose)
unsigned long sgCapMs = STAGE_MAX_MS;   // per-stage hard cap (dose stages override with the 2x ceiling)
bool  sgIsAC = false, sgPumpOn = false, sgSawFlow = false;
unsigned long sgT0 = 0;
unsigned long acNoCurrentSince = 0;
unsigned long lastPzemMs = 0;     // PZEM read throttle (see PZEM_POLL_MS)

/* ---- Misc state ---------------------------------------------------------- */
unsigned long lastHeartbeat = 0;
// ESP1-commanded preventive pump exercise (sec.14.9.1). The schedule lives on ESP1
// (always-on, RTC); ESP2 just runs the named pump briefly when told to. Non-blocking:
// turn the pump on, note the off-time, and complete it in loop() (like the Nano-RST pulse).
int  exRunBit = -1;                        // PCF bit being exercised (-1 = none)
unsigned long exRunOffAt = 0;
String exRunName;
String rxLine;

/* ---- Forward declarations ------------------------------------------------ */
void wdtSetup();
void feedWDT();
bool pcfWrite(uint16_t s);
void pcfOn(uint8_t bit);
void pcfOff(uint8_t bit);
void stopAll();
void stopKeepBank();
void cutoffEnergize();
void cutoffDeenergize();
void tankSave();
void tankLoad();
void holdFault(const char *resp, const char *loc);
void resumeWork(const String &mode);
bool pcfPresent();
void pcfHealth();
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
float endMeteredStage();
void safetyMonitor();
float readMixerCurrent();
void heartbeat();
void idleResetTick();
void testSafety();
void testComboOn(int idx);
void testOff();
void calStreamTick();
int  flowPinForId(const String &id);
bool applySetCal(const String &id, const String *tok, int n, int i);
void setupPrime(const String &line, const String &col);
void stopPrime();
void primeSafety();

/* =============================================================================
 *  SETUP  --  SAFE boot: all outputs OFF before anything else (sec.19.5.4)
 * ========================================================================== */
void setup() {
  Serial.begin(DEBUG_BAUD);
  delay(50);
  Serial.println(F("\n=== ESP32 #2 Actuator Controller boot ==="));

  Wire.begin(I2C_SDA, I2C_SCL);
  pcfWrite(0xFFFF);                         // ALL actuators OFF + master cutoff de-energized (SAFE)

  esp1Serial.begin(ESP1_BAUD, SERIAL_8N1, ESP1_RX_PIN, ESP1_TX_PIN);
  pzem.setAddress(0x01);                    // default PZEM Modbus address (begins Serial1)

  analogReadResolution(12);                 // 0..4095

  wo.active = false;
  lastHeartbeat = millis();
  rxLine.reserve(160);

  tankLoad();                               // restore persisted mixing-tank volume (overfill guard)
  Serial.printf("Restored mixing-tank volume: %.2f L\n", mixTankL);

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
  calStreamTick();             // Calibration Mode: stream the selected sensor's raw value (§A.3)
  primeSafety();               // Prime dead-man + generous cap (§A.4.2)
  pcfHealth();                 // PCF8575 bus watchdog -> PCF_FAIL / PCF_OK
  heartbeat();
  idleResetTick();             // self-heal: reboot after prolonged idle (30 min)

  // non-blocking preventive-exercise completion (5 s pump run commanded by ESP1)
  if (exRunOffAt && millis() >= exRunOffAt) {
    pcfOff(exRunBit);
    pcfOff(OUT_INVERTER);
    cutoffDeenergize();                  // drop the master cutoff again (back to safe idle)
    exRunOffAt = 0; exRunBit = -1;
    reply("DONE,EXERCISE," + exRunName);
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
bool pcfWrite(uint16_t s) {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(lowByte(s));
  Wire.write(highByte(s));
  bool ok = (Wire.endTransmission() == 0);          // 0 = ACKed; non-zero = bus/device fault
  pcfShadow = s;
  if (!ok) { if (pcfFailCount < 250) pcfFailCount++; }   // consecutive-failure counter (debounce)
  else     { pcfFailCount = 0; }
  return ok;
}
void pcfOn(uint8_t bit)  { pcfWrite(pcfShadow & ~(1 << bit)); }   // clear bit = ON
void pcfOff(uint8_t bit) { pcfWrite(pcfShadow |  (1 << bit)); }   // set bit  = OFF
void stopAll()           { pcfWrite(0xFFFF); }                    // all OFF incl. cutoff de-energized
// All actuators OFF but the master cutoff kept ENERGIZED (bank stays powered). One atomic write
// (bit15=0 -> P17 on), so switching Testing components never pulses P17 off->on (sec.19.4.8.4).
void stopKeepBank()      { pcfWrite(0x7FFF); }

/* ---- P17 master actuator-power cutoff (sec.19.4.8) ----------------------- *
 * Active-LOW: clearing bit 15 ENERGIZES the master relay (bank powered); setting it
 * DE-energizes (whole P0-P16 bank goes hardware-dead, fail-safe). Energize before any
 * actuation; de-energize on fault / completion / idle.                                */
void cutoffEnergize()    { pcfOn(OUT_MASTER_CUTOFF); }
void cutoffDeenergize()  { pcfOff(OUT_MASTER_CUTOFF); }

// Energize all PCF bits of a Testing priming combo (idx 16=Fill, 17/18/19=Push>Col A/B/C).
void testComboOn(int idx) {
  const uint8_t *bits; uint8_t cnt;
  switch (idx) {
    case 16: bits = COMBO_FILL;   cnt = sizeof(COMBO_FILL);   break;
    case 17: bits = COMBO_PUSH_A; cnt = sizeof(COMBO_PUSH_A); break;
    case 18: bits = COMBO_PUSH_B; cnt = sizeof(COMBO_PUSH_B); break;
    case 19: bits = COMBO_PUSH_C; cnt = sizeof(COMBO_PUSH_C); break;
    default: return;
  }
  for (uint8_t i = 0; i < cnt; i++) pcfOn(bits[i]);
}
// Turn off whatever Testing item is held (single relay OR a whole combo) and return to the
// resting bank-powered state.
void testOff() {
  if (testHeldBit > 15)      stopKeepBank();         // combo: all actuators off, bank stays powered (no P17 glitch)
  else if (testHeldBit >= 0) pcfOff(testHeldBit);    // single relay off (P17 untouched)
  cutoffEnergize();                                  // ensure bank powered on release (no-op if already on)
}

/* ---- Mixing-tank volume persistence (NVS) -------------------------------- *
 * Persist mixTankL so a reboot/brown-out/power-down can never make ESP2 assume an
 * empty tank and overfill on the next fill (sec.19.4.8). Written on every change.      */
void tankSave() { prefs.putFloat("mixL", mixTankL); }
void tankLoad() {
  prefs.begin("esp2", false);
  mixTankL = prefs.getFloat("mixL", 0.0f);
  if (mixTankL < 0.0f) mixTankL = 0.0f;
}
// Apply a metered stage's delivered volume to the tank: +liters for a FILL (reservoir->
// mix), -liters for a DELIVER (mix->column). Clamps at 0 and persists.
void tankApply(float deltaL) {
  mixTankL += deltaL;
  if (mixTankL < 0.0f) mixTankL = 0.0f;
  tankSave();
}

/* =============================================================================
 *  PCF8575 HEALTH  --  detect a dead/unresponsive relay bus, report to ESP1
 * -----------------------------------------------------------------------------
 *  Active-probes the PCF every PCF_PROBE_MS and watches pcfWrite() failures via a
 *  consecutive-failure counter (so a single I2C glitch can't false-trip). On a real
 *  fault it reports PCF_FAIL,I2C (re-asserted every PCF_REPORT_MS so a missed UART
 *  line still gets through) and PCF_OK on recovery. Major on ESP1 (no shutdown) --
 *  other safeties (PZEM no-current, flow timeout) still apply.                    */
bool pcfPresent() {
  Wire.beginTransmission(PCF_ADDR);
  return (Wire.endTransmission() == 0);
}
void pcfHealth() {
  if (millis() - lastPcfProbeMs >= PCF_PROBE_MS) {  // periodic active probe (covers idle)
    lastPcfProbeMs = millis();
    if (pcfPresent()) pcfFailCount = 0;
    else if (pcfFailCount < 250) pcfFailCount++;
  }
  bool faulted = (pcfFailCount >= PCF_FAIL_LIMIT);
  if (faulted) {
    if (!pcfFaultReported || millis() - lastPcfReportMs >= PCF_REPORT_MS) {
      pcfFaultReported = true; lastPcfReportMs = millis();
      reply("PCF_FAIL,I2C");                        // ESP1 = Major (alert + log)
    }
  } else if (pcfFaultReported) {                    // recovered
    pcfFaultReported = false;
    reply("PCF_OK");
  }
}

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
  noInterrupts(); unsigned long p = flowPulses; interrupts();
  return (float)p / k;
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
    stopAll(); step = SEQ_NONE; wo.active = false; faultHeld = false; detachFlow();
    testMode = false; testHeldBit = -1; testCapped = false;
    if (primeLine != "") stopPrime(); calId = "";        // drop any calibration stream / prime

    // Cancel any in-flight one-shot timers so loop() can't emit a stray late DONE
    // (DONE,EXERCISE) after the stop. NOTE: mixTankL is kept (persisted) -- the tank may
    // still physically hold water, and the next run must account for it (no overfill).
    exRunOffAt = 0; exRunBit = -1;
    reply("DONE,STOP_ALL");
    return;
  }
  if (cmd == "STATUS_REQ") { reply("STATUS,ESP2,OK"); return; }
  if (cmd == "RESET_SELF") {                 // soft reboot on ESP1 request (spec sec.9.7.2)
    reply("ACK,RESET_SELF");
    stopAll();                               // force SAFE (all outputs OFF) before reboot
    esp1Serial.flush();                      // shift the ACK out before we reset (no delay(); we reboot next)
    ESP.restart();
  }
  // ---- user-gated fault recovery (sec.19.4.8.2): RESUME,<NORMAL|IRRIGATE|RELEASE> ----
  if (cmd == "RESUME") {
    if (!faultHeld) { reply("INVALID,NOT_HELD"); return; }
    String mode = (n >= 2) ? tok[1] : "NORMAL";
    if (mode != "NORMAL" && mode != "IRRIGATE" && mode != "RELEASE") { reply("INVALID,RESUME_MODE"); return; }
    resumeWork(mode);                        // re-energizes P17, resumes/rewrites the FSM, replies ACK,RESUME
    return;
  }

  // ---- Calibration Mode (companion spec §A): raw stream / push const / prime ----
  if (cmd == "CAL_START") {
    if (wo.active || faultHeld || testMode) { reply("BUSY,ACTIVE"); return; }
    if (n < 2) { reply("INVALID,CAL_START"); return; }
    calId = tok[1];
    int fp = flowPinForId(calId);
    if (fp >= 0) attachFlow(fp);             // arm + zero this flow ISR (pulses accumulate across bursts)
    reply("ACK,CAL_START," + calId);
    return;
  }
  if (cmd == "CAL_STOP") {
    if (calId.startsWith("FLOW_")) detachFlow();
    calId = "";
    reply("ACK,CAL_STOP");
    return;
  }
  if (cmd == "SET_CAL") {                     // push one runtime calibration constant (block-until-ACK on ESP1)
    if (n < 3) { reply("INVALID,SET_CAL"); return; }
    bool ok = applySetCal(tok[1], tok, n, 2);
    reply(ok ? ("ACK,SET_CAL," + tok[1]) : "INVALID,SET_CAL_ID");
    return;
  }
  if (cmd == "PRIME_START") {
    if (wo.active || faultHeld || testMode) { reply("BUSY,ACTIVE"); return; }
    if (n < 2) { reply("INVALID,PRIME"); return; }
    if (primeLine == "" || primeLine != tok[1]) setupPrime(tok[1], (n >= 3) ? tok[2] : "");
    primeLastMs = millis();                   // (re)arm the dead-man keep-alive
    return;
  }
  if (cmd == "PRIME_STOP") { stopPrime(); reply("DONE,PRIME"); return; }

  // ---- manual relay test, dead-man (ESP1 Settings>Testing, spec sec.18.10.8) ----
  if (cmd == "TEST") {
    String sub = (n >= 2) ? tok[1] : "";
    if (sub == "ENTER") {                                  // enter TEST_MODE, force SAFE
      if (wo.active || step != SEQ_NONE) { reply(faultHeld ? "BUSY,HELD" : "BUSY,ACTIVE"); return; }
      stopAll(); testMode = true; testHeldBit = -1; testCapped = false;
      cutoffEnergize();                                    // power the bank so manual relay control works (sec.19.4.8.4)
      exRunOffAt = 0; exRunBit = -1;                       // drop any pending one-shot timers
      reply("ACK,TEST,ENTER"); return;
    }
    if (sub == "EXIT") {
      stopAll(); testMode = false; testHeldBit = -1;       // all OFF incl. master cutoff de-energized
      exRunOffAt = 0; exRunBit = -1;                       // drop any pending one-shot timers
      reply("DONE,TEST"); return;
    }
    if (sub == "RELEASE") {                                // explicit button-up
      testOff();                                           // single relay OR combo -> off, bank stays powered
      testHeldBit = -1; testCapped = false; return;
    }
    if (sub == "HOLD") {                                   // dead-man keep-alive for <bit/combo>
      if (!testMode || n < 3) return;
      int bit = tok[2].toInt();
      if (bit < 0 || bit > 19) { reply("INVALID,TEST_BIT"); return; }   // 0..15 relays, 16..19 combos
      if (bit != testHeldBit) {                            // new selection -> one-at-a-time switch
        if (bit == OUT_MASTER_CUTOFF) {
          // REVERSED in test mode: the bank is kept powered by default, so holding the master
          // cutoff entry DE-energizes the WHOLE bank (operator verifies the cutoff drops power).
          stopAll();                                       // all actuators + P17 off -- this IS the cutoff test
        } else if (bit > 15) {                             // valve+pump priming combo
          stopKeepBank();                                  // clear prev selection, keep P17 up (no glitch)
          testComboOn(bit);
        } else {                                           // single relay
          stopKeepBank();                                  // clear prev selection, keep P17 up (no glitch)
          pcfOn(bit);
        }
        testHeldBit = bit; testRelayOnMs = millis(); testCapped = false;
      }
      testLastHoldMs = millis();                           // refresh dead-man timer
      return;
    }
    reply("INVALID,TEST_SUB"); return;
  }
  // ---- preventive pump exercise, commanded by ESP1 (sec.14.9.1) ----
  if (cmd == "EXERCISE") {
    if (faultHeld) { reply("BUSY,HELD"); return; }
    if (wo.active || step != SEQ_NONE || testMode) { reply("BUSY,ACTIVE"); return; }
    if (n < 2) { reply("INVALID,EXERCISE"); return; }
    int bit = (tok[1] == "TRANSFER") ? OUT_TRANSFER
            : (tok[1] == "BOOSTER")  ? OUT_BOOSTER
            : (tok[1] == "MIXER")    ? OUT_MIXER : -1;
    if (bit < 0) { reply("INVALID,EXERCISE_DEV"); return; }
    reply("ACK,EXERCISE");
    cutoffEnergize();                                  // power the bank (sec.19.4.8)
    pcfOn(OUT_INVERTER); pcfOn(bit);                   // AC source on, then the pump
    exRunBit = bit; exRunName = tok[1];
    exRunOffAt = millis() + PUMP_EXERCISE_RUN_MS;       // loop() turns it off + reports DONE
    return;
  }

  // ---- sequence commands ----
  bool isIrr  = cmd.startsWith("SEQ_IRRIGATION_");
  bool isFert = cmd.startsWith("SEQ_FERTIGATION_");
  if (!isIrr && !isFert) { reply("INVALID,UNKNOWN_CMD"); return; }
  if (faultHeld) { reply("BUSY,HELD"); return; }                 // a held fault must be resolved (RESUME) first
  if (testMode) { reply("BUSY,TEST"); return; }                  // never auto-run during a manual test
  if (wo.active || step != SEQ_NONE) { reply("BUSY,ACTIVE"); return; }

  char colCh = cmd.charAt(cmd.length() - 1);
  int c = (colCh == 'A') ? 0 : (colCh == 'B') ? 1 : (colCh == 'C') ? 2 : -1;
  if (c < 0) { reply("INVALID,COLUMN"); return; }
  if (!COLUMN_ENABLED[c]) { reply("INVALID,COL_DISABLED"); return; }

  // defaults
  wo.col = c; wo.fertigate = isFert; wo.flushPct = 0;
  wo.dose[0] = wo.dose[1] = wo.dose[2] = 0;
  wo.doseCeilMs[0] = wo.doseCeilMs[1] = wo.doseCeilMs[2] = 0;   // 0 -> fall back to STAGE_MAX_MS
  wo.ecLo = 0.0f; wo.ecHi = 99; wo.phLo = 0; wo.phHi = 14;   // wide defaults; ESP1 sends real window
  wo.mixMs = MIX_DEFAULT_MS; wo.cmdName = cmd;

  // parse key/value groups
  for (int i = 1; i < n; i++) {
    if (tok[i] == "FLUSH" && i + 1 < n)      { wo.flushPct = tok[++i].toFloat(); }
    else if (tok[i] == "DOSE" && i + 3 < n)  { wo.dose[0] = tok[++i].toFloat(); wo.dose[1] = tok[++i].toFloat(); wo.dose[2] = tok[++i].toFloat(); }
    else if (tok[i] == "DCEIL" && i + 3 < n) { for (int j = 0; j < 3; j++) wo.doseCeilMs[j] = (unsigned long)(tok[++i].toFloat() * 1000.0f); }  // per-dose 2x ceiling (s->ms)
    else if (tok[i] == "BATCHV" && i + 1 < n){ ++i; /* planned batch L: ESP1 dose-calc input; logged there */ }
    else if (tok[i] == "EC" && i + 2 < n)    { wo.ecLo = tok[++i].toFloat(); wo.ecHi = tok[++i].toFloat(); }
    else if (tok[i] == "PH" && i + 2 < n)    { wo.phLo = tok[++i].toFloat(); wo.phHi = tok[++i].toFloat(); }
    else if (tok[i] == "MIX" && i + 1 < n)   { wo.mixMs = (unsigned long)tok[++i].toInt(); }
    // Job-critical calibration carried in the work order (§A.5.1 #3): never run a dose on stale K/cal.
    else if (tok[i] == "KMAIN" && i + 2 < n) { K_RES_MIX = tok[++i].toFloat(); K_MIX_IRR = tok[++i].toFloat(); }
    else if (tok[i] == "KNUT" && i + 3 < n)  { K_NUT[0] = tok[++i].toFloat(); K_NUT[1] = tok[++i].toFloat(); K_NUT[2] = tok[++i].toFloat(); }
    else if (tok[i] == "ECCAL" && i + 2 < n) { EC_CAL_M = tok[++i].toFloat(); EC_CAL_B = tok[++i].toFloat(); }
    else if (tok[i] == "PHCAL" && i + 2 < n) { PH_CAL_M = tok[++i].toFloat(); PH_CAL_B = tok[++i].toFloat(); }
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
  faultHeld = false;
  ecphFailed = false;
  doseIdx = 0;
  woWaterL = 0.0f;
  cutoffEnergize();              // power the actuator bank (sec.19.4.8); individual relays still gate each load
  Serial.printf("Work order: %s fert=%d flush=%.0f%% tank=%.2fL\n",
                wo.cmdName.c_str(), wo.fertigate, wo.flushPct, mixTankL);
  goStep(SEQ_FILL);
}

void goStep(SeqStep s) { step = s; stepInit = false; }

void stageBegin(int pumpBit, bool isAC, int flowPin, float k, float targetL) {
  sgPump = pumpBit; sgIsAC = isAC; sgFlow = flowPin; sgK = k; sgTarget = targetL;
  sgTankSign = 0;                // default: stage does not move tank water (set by caller for fill/deliver)
  sgCapMs = STAGE_MAX_MS;        // default cap (dose stages override with the per-dose ceiling)
  sgPumpOn = false; sgSawFlow = false; sgT0 = millis();
  acNoCurrentSince = 0;
  if (isAC) pcfOn(OUT_INVERTER);
}

// End a metered FILL/DELIVER stage, crediting the liters it moved to the persisted tank
// volume (and, for a DELIVER, to the per-order water-to-column tally). Reads before stageEnd
// so it works for both normal completion and a mid-stage fault (partial liters still count).
float endMeteredStage() {
  float liters = litersSoFar(sgK);
  if      (sgTankSign > 0) tankApply(+liters);                 // reservoir -> mix
  else if (sgTankSign < 0) { tankApply(-liters); woWaterL += liters; }  // mix -> column
  stageEnd();
  return liters;
}
int stagePoll() {
  if (!sgPumpOn) {                          // valve/inverter settle, then start pump
    if (millis() - sgT0 >= (sgIsAC ? INVERTER_WARMUP_MS : VALVE_SWITCH_MS)) {
      attachFlow(sgFlow); pcfOn(sgPump); sgPumpOn = true; sgT0 = millis();
    }
    return 0;
  }
  float L = litersSoFar(sgK);
  if (L > 0.0005f) sgSawFlow = true;
  if (L >= sgTarget) return 1;
  unsigned long el = millis() - sgT0;
  if (!sgSawFlow && el > FLOW_TIMEOUT_MS) return -1;   // dry run / no flow
  if (el > sgCapMs) return -1;                          // hard cap (per-dose ceiling for dose stages)
  return 0;
}
void stageEnd() {
  if (sgPump >= 0) pcfOff(sgPump);
  detachFlow();
  sgPump = -1; sgPumpOn = false;
}

void finishOk() {
  stopAll();                         // all OFF + master cutoff de-energized
  mixTankL = 0.0f; tankSave();       // tank emptied to the column -> clear the persisted volume
  String name = wo.cmdName;
  float water = woWaterL;
  wo.active = false; faultHeld = false; step = SEQ_NONE;
  // Append measured water so ESP1 can tally per-column daily usage (sec.12.1.3 / B1).
  reply("DONE," + name + ",WATER," + String(water, 2));
}

// Hard fault (sec.19.4.8.1): drop the master cutoff (bank hardware-dead), PAUSE the sequence
// but KEEP the work order + step + the persisted mix-tank volume, and STAY ALIVE awaiting a
// user-gated RESUME,<mode>. ESP2 does NOT power itself off -- that would forget the water and
// overfill on resume; ESP1 keeps it powered while held.
void holdFault(const char *resp, const char *loc) {
  stageEnd();                        // pump off + flow detached (partial liters already credited by caller)
  stopAll();                         // all OFF, master cutoff de-energized
  faultHeld = true;
  stepInit  = false;                 // on RESUME the paused step re-inits with the updated tank volume
  reply(String(resp) + "," + loc);
}

// User-gated recovery (sec.19.4.8.2). NORMAL resumes the paused step; IRRIGATE finishes as
// irrigation-only (top up plain water to the column budget, no dosing); RELEASE dumps the
// current tank contents to the assigned column as-is. All re-energize P17 first.
void resumeWork(const String &mode) {
  faultHeld = false;
  cutoffEnergize();
  if (mode == "IRRIGATE") {
    wo.fertigate = false; wo.flushPct = 0;     // top-up FILL to budget, then DELIVER, no flush/dosing
    goStep(SEQ_FILL);
  } else if (mode == "RELEASE") {
    wo.fertigate = false; wo.flushPct = 0;     // skip fill/dose: just DELIVER whatever is in the tank
    goStep(SEQ_DELIVER);
  } else {                                     // NORMAL: continue the paused sequence as-is
    stepInit = false;                          // re-init the current step with the updated tank volume
  }
  reply("ACK,RESUME," + mode);
}

void runSequence() {
  if (step == SEQ_NONE) return;
  if (faultHeld) return;                 // paused after a fault -> wait for RESUME (sec.19.4.8)
  int c = wo.col;
  const char *colLoc = (c == 0) ? "COL_A" : (c == 1) ? "COL_B" : "COL_C";

  switch (step) {

    /* ---- FILL: reservoir -> mixing tank (transfer pump, flow 4) ---------- *
     * Target is the REMAINING liters to reach the desired mix level minus what is
     * already in the tank (mixTankL) -- so a resume never re-fills from 0 and overfills. */
    case SEQ_FILL: {
      if (!stepInit) {
        float desired = wo.fertigate ? WATER_BUDGET_L[c] * (1.0f - wo.flushPct / 100.0f)
                                     : WATER_BUDGET_L[c];
        float remaining = desired - mixTankL;
        if (remaining < TANK_EPS_L) {                 // already enough in the tank -> skip the fill
          goStep(wo.fertigate ? SEQ_DOSE : SEQ_DELIVER); break;
        }
        pcfOn(OUT_RES_VALVE);
        stageBegin(OUT_TRANSFER, true, FLOW_RES_MIX, K_RES_MIX, remaining);
        sgTankSign = +1;                              // this stage adds water to the mix tank
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1)      { endMeteredStage(); pcfOff(OUT_RES_VALVE); goStep(wo.fertigate ? SEQ_DOSE : SEQ_DELIVER); }
      else if (r == -1){ endMeteredStage(); pcfOff(OUT_RES_VALVE); holdFault("FLOW_FAIL", "TRANSFER"); }
      break;
    }

    /* ---- DOSE: nutrients A/B/C into mixing tank (sequential) ------------- */
    case SEQ_DOSE: {
      if (!stepInit) {
        pcfOff(OUT_INVERTER);                            // nutrient pumps are DC; drop the AC source
        // skip zero / disabled doses
        while (doseIdx < 3 && wo.dose[doseIdx] <= 0.0f) doseIdx++;
        if (doseIdx >= 3) { goStep(SEQ_MIX); break; }
        float targetL = wo.dose[doseIdx] / 1000.0f;     // mL -> L
        stageBegin(NUT_PUMP_BIT[doseIdx], false, NUT_FLOW_PIN[doseIdx], K_NUT[doseIdx], targetL);
        sgCapMs = wo.doseCeilMs[doseIdx] ? wo.doseCeilMs[doseIdx] : STAGE_MAX_MS;   // per-dose 2x ceiling (spec §2.4)
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1) {
        float measuredML = litersSoFar(K_NUT[doseIdx]) * 1000.0f;   // read before stageEnd
        stageEnd();
        Serial.printf("Dosed nutrient %d: %.1f mL\n", doseIdx, measuredML);
        // Report dose to ESP1 (target + measured) for the daily report / thesis data (sec.25.2.1).
        reply(String("DOSE,NUT_") + (char)('A' + doseIdx) + "," +
              String(wo.dose[doseIdx], 1) + "," + String(measuredML, 1) + "," + colLoc);
        doseIdx++;
        stepInit = false;                    // re-enter to dose next (or advance to MIX)
        if (doseIdx >= 3) goStep(SEQ_MIX);
      } else if (r == -1) {
        // Dosing pump ran past its timed ceiling (or never flowed) -> DOSE_TIMEOUT, NOT a fallback
        // (dosing spec §2.3/§5): stuck-low flow sensor or unprimed line. Stop and HOLD for the user.
        stageEnd();
        String nut = String("NUT_") + (char)('A' + doseIdx);
        holdFault("DOSE_TIMEOUT", nut.c_str());
      }
      break;
    }

    /* ---- MIX: homogenize; validate mixer current (ACS712) ---------------- */
    case SEQ_MIX: {
      if (!stepInit) { pcfOn(OUT_INVERTER); pcfOn(OUT_MIXER); stepStart = millis(); stepInit = true; }
      float mi = readMixerCurrent();
      if (mi > MIXER_OVERCURRENT_A) { pcfOff(OUT_MIXER); holdFault("SAFE_STOP", "MIXER_OC"); break; }
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
      int phRaw = analogRead(PIN_PH), ecRaw = analogRead(PIN_EC);
      float ph = PH_CAL_M * phRaw + PH_CAL_B;
      float ec = EC_CAL_M * ecRaw + EC_CAL_B;
      // HARDWARE fault first (railed ADC = probe disconnected/shorted) -> SENSOR_FAIL, distinct
      // from an in-range value that is merely outside the safe window (EC_FAIL/PH_FAIL). Both
      // are Major on ESP1; check EC and pH independently so a double-fault reports both.
      if (phRaw <= PH_ADC_FAULT_LO || phRaw >= PH_ADC_FAULT_HI) { ecphFailed = true; reply("SENSOR_FAIL,PH"); }
      else if (ph < wo.phLo || ph > wo.phHi)                    { ecphFailed = true; reply(String("PH_FAIL,") + colLoc); }
      if (ecRaw <= EC_ADC_FAULT_LO || ecRaw >= EC_ADC_FAULT_HI) { ecphFailed = true; reply("SENSOR_FAIL,EC"); }
      else if (ec < wo.ecLo || ec > wo.ecHi)                    { ecphFailed = true; reply(String("EC_FAIL,") + colLoc); }
      // Plumbing has no drain: deliver the mixed batch regardless, fault already reported.
      goStep(SEQ_DELIVER);
      break;
    }

    /* ---- DELIVER: mixing tank -> column (booster pump, flow 5) ----------- *
     * Target is the whole current tank volume (mixTankL) -- delivering empties it. On a
     * resume this is exactly the remaining water, so it never under/over-delivers.        */
    case SEQ_DELIVER: {
      if (!stepInit) {
        if (mixTankL < TANK_EPS_L) { finishOk(); break; }   // nothing to deliver
        pcfOn(OUT_MIX_VALVE); pcfOn(COL_VALVE_BIT[c]);
        stageBegin(OUT_BOOSTER, true, FLOW_MIX_IRR, K_MIX_IRR, mixTankL);
        sgTankSign = -1;                                     // this stage drains the mix tank
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1) {
        endMeteredStage(); pcfOff(OUT_MIX_VALVE); pcfOff(COL_VALVE_BIT[c]);
        if (wo.fertigate && wo.flushPct > 0) goStep(SEQ_FLUSH_FILL);
        else finishOk();
      } else if (r == -1) {
        endMeteredStage(); pcfOff(OUT_MIX_VALVE); pcfOff(COL_VALVE_BIT[c]);
        holdFault("FLOW_FAIL", "MAIN");
      }
      break;
    }

    /* ---- FLUSH_FILL: plain reservoir water -> mixing tank ---------------- */
    case SEQ_FLUSH_FILL: {
      if (!stepInit) {
        float remaining = WATER_BUDGET_L[c] * (wo.flushPct / 100.0f) - mixTankL;
        if (remaining < TANK_EPS_L) { goStep(SEQ_FLUSH_DELIVER); break; }
        pcfOn(OUT_RES_VALVE);
        stageBegin(OUT_TRANSFER, true, FLOW_RES_MIX, K_RES_MIX, remaining);
        sgTankSign = +1;
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1)      { endMeteredStage(); pcfOff(OUT_RES_VALVE); goStep(SEQ_FLUSH_DELIVER); }
      else if (r == -1){ endMeteredStage(); pcfOff(OUT_RES_VALVE); holdFault("FLOW_FAIL", "TRANSFER"); }
      break;
    }

    /* ---- FLUSH_DELIVER: plain water -> column (final flush step) --------- */
    case SEQ_FLUSH_DELIVER: {
      if (!stepInit) {
        if (mixTankL < TANK_EPS_L) { finishOk(); break; }
        pcfOn(OUT_MIX_VALVE); pcfOn(COL_VALVE_BIT[c]);
        stageBegin(OUT_BOOSTER, true, FLOW_MIX_IRR, K_MIX_IRR, mixTankL);
        sgTankSign = -1;
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1)      { endMeteredStage(); pcfOff(OUT_MIX_VALVE); pcfOff(COL_VALVE_BIT[c]); finishOk(); }
      else if (r == -1){ endMeteredStage(); pcfOff(OUT_MIX_VALVE); pcfOff(COL_VALVE_BIT[c]); holdFault("FLOW_FAIL", "MAIN"); }
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

  float v = pzem.voltage();
  float i = pzem.current();
  if (isnan(v) || isnan(i)) return;         // PZEM absent/unreadable -> skip (graceful)

  // Credit the partial liters this metered stage moved before holding (overfill guard), then
  // PAUSE on the master cutoff awaiting RESUME (sec.19.4.8) -- ESP2 stays alive holding the tank.
  if (i > PZEM_OVERCURRENT_A)        { endMeteredStage(); holdFault("PWR_FAIL", "OVERCURRENT"); return; }
  if (v < PZEM_V_MIN || v > PZEM_V_MAX) { endMeteredStage(); holdFault("PWR_FAIL", "VOLTAGE"); return; }

  if (i < PZEM_MIN_CURRENT_A) {             // pump ON but drawing no current
    if (acNoCurrentSince == 0) acNoCurrentSince = millis();
    else if (millis() - acNoCurrentSince > PZEM_NO_CURRENT_MS) { endMeteredStage(); holdFault("PWR_FAIL", "NO_CURRENT"); return; }
  } else {
    acNoCurrentSince = 0;
  }
}

float readMixerCurrent() {
  int raw = analogRead(PIN_MIXER_I);
  float v = (raw / 4095.0f) * 3.3f;
  float a = (v - ACS712_ZERO_V) / ACS712_SENS_V_PER_A;
  return fabs(a);
}
// (EC/pH are read inline in the SEQ_ECPH stage; standalone readPH()/readEC() removed as unused.)

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
 *  IDLE SELF-HEAL  --  reboot after prolonged idle so a wedged-but-working ESP2
 *  recovers on its own, even if ESP1 wrongly believes it is silent. Never fires
 *  during any real activity (a held fault MUST survive until the operator resolves
 *  it). mixTankL is NVS-persisted, so the tank volume survives the reboot.
 * ========================================================================== */
void idleResetTick() {
  bool idle = (step == SEQ_NONE && !wo.active && !faultHeld && !testMode
               && calId == "" && primeLine == "" && exRunOffAt == 0);
  if (!idle)            { idleSinceMs = 0; return; }   // any activity restarts the idle clock
  if (idleSinceMs == 0) { idleSinceMs = millis(); return; }
  if (millis() - idleSinceMs >= IDLE_RESET_MS) {
    reply("INFO,IDLE_RESET");                          // best-effort notice (ESP1 logs it)
    delay(20);                                         // flush the UART line before the reboot
    ESP.restart();                                     // returns as READY,ESP2; mixTankL persists
  }
}

/* =============================================================================
 *  TEST DEAD-MAN ENFORCEMENT  (sole safety authority in TEST mode, sec.18.10.8.3)
 * ========================================================================== */
void testSafety() {
  if (!testMode || testHeldBit < 0) return;
  unsigned long now = millis();
  // HARD CAP: force OFF regardless of ESP1. Combos get the longer priming cap; single relays 10 s.
  unsigned long cap = (testHeldBit > 15) ? TEST_COMBO_CAP_MS : TEST_HARD_CAP_MS;
  if (!testCapped && now - testRelayOnMs >= cap) {
    testOff(); testCapped = true;                  // back to resting (bank powered)
    reply("DONE,TEST_CAP");
  }
  // Dead-man: no HOLD within the timeout (button released, ESP1 hung, or link lost)
  // -> turn off and clear selection (fail-safe). Restore the resting bank-powered state.
  if (now - testLastHoldMs > TEST_HOLD_TIMEOUT_MS) {
    if (!testCapped) testOff();
    testHeldBit = -1; testCapped = false;
  }
}

/* =============================================================================
 *  CALIBRATION MODE  (companion spec §A)  --  raw stream, SET_CAL, Prime dead-man
 * ========================================================================== */
// Map a FLOW_* CAL/Prime id to its hardware pin (-1 if not a flow id).
int flowPinForId(const String &id) {
  if (id == "FLOW_RESMIX") return FLOW_RES_MIX;
  if (id == "FLOW_MIXIRR") return FLOW_MIX_IRR;
  if (id == "FLOW_NUTA")   return FLOW_NUT_A;
  if (id == "FLOW_NUTB")   return FLOW_NUT_B;
  if (id == "FLOW_NUTC")   return FLOW_NUT_C;
  if (id == "FLOW_NUTD")   return 25;   // wired but not metered in normal operation
  if (id == "FLOW_PHUP")   return 26;
  if (id == "FLOW_PHDN")   return 27;
  return -1;
}

// Stream the selected sensor's RAW value (~1.6 Hz) for interactive calibration (§A.3).
void calStreamTick() {
  if (calId == "") return;
  if (millis() - lastCalMs < CAL_STREAM_MS) return;
  lastCalMs = millis();
  long raw;
  if      (calId == "PH")     raw = analogRead(PIN_PH);
  else if (calId == "EC")     raw = analogRead(PIN_EC);
  else if (calId == "ACS712") raw = analogRead(PIN_MIXER_I);
  else if (calId.startsWith("FLOW_")) { noInterrupts(); raw = (long)flowPulses; interrupts(); }
  else return;
  reply("CAL," + calId + "," + String(raw));
}

// Apply one pushed calibration constant to the matching runtime variable. pH/EC carry two
// values (slope M then offset B). Flow ids that ESP2 does not meter in normal operation
// (NUTD/PHUP/PHDN) are accepted as a no-op so the ESP1 block-until-ACK save still completes.
bool applySetCal(const String &id, const String *tok, int n, int i) {
  if      (id == "FLOW_RESMIX") K_RES_MIX = tok[i].toFloat();
  else if (id == "FLOW_MIXIRR") K_MIX_IRR = tok[i].toFloat();
  else if (id == "FLOW_NUTA")   K_NUT[0]  = tok[i].toFloat();
  else if (id == "FLOW_NUTB")   K_NUT[1]  = tok[i].toFloat();
  else if (id == "FLOW_NUTC")   K_NUT[2]  = tok[i].toFloat();
  else if (id == "ACS712")      ACS712_ZERO_V = tok[i].toFloat();
  else if (id == "PH") { PH_CAL_M = tok[i].toFloat(); if (i + 1 < n) PH_CAL_B = tok[i + 1].toFloat(); }
  else if (id == "EC") { EC_CAL_M = tok[i].toFloat(); if (i + 1 < n) EC_CAL_B = tok[i + 1].toFloat(); }
  else if (id == "FLOW_NUTD" || id == "FLOW_PHUP" || id == "FLOW_PHDN") { /* not metered here: accept */ }
  else return false;
  return true;
}

// Arm a prime line: open its valve(s) + run its paired pump together (§A.4.2.1). Energizes the
// master cutoff first so the bank has power. Switching lines stops the previous one.
void setupPrime(const String &line, const String &col) {
  if (primeLine != "") stopPrime();
  int valveA = -1, valveB = -1, pump = -1; bool isAC = false;
  if      (line == "RESMIX") { valveA = OUT_RES_VALVE; pump = OUT_TRANSFER; isAC = true; }
  else if (line == "MIXIRR") {
    valveA = OUT_MIX_VALVE; pump = OUT_BOOSTER; isAC = true;
    int c = (col == "A") ? 0 : (col == "B") ? 1 : (col == "C") ? 2 : -1;
    if (c >= 0) valveB = COL_VALVE_BIT[c];
  }
  else if (line == "NUTA") pump = OUT_NUT_A;
  else if (line == "NUTB") pump = OUT_NUT_B;
  else if (line == "NUTC") pump = OUT_NUT_C;
  else if (line == "NUTD") pump = OUT_NUT_D;
  else if (line == "PHUP") pump = OUT_PH_UP;
  else if (line == "PHDN") pump = OUT_PH_DN;
  else { reply("INVALID,PRIME_LINE"); return; }

  cutoffEnergize();
  if (isAC) pcfOn(OUT_INVERTER);
  if (valveA >= 0) pcfOn(valveA);
  if (valveB >= 0) pcfOn(valveB);
  pcfOn(pump);
  primeLine = line; primeValveA = valveA; primeValveB = valveB; primePump = pump;
  primeIsAC = isAC; primeOnMs = millis(); primeCapped = false;
  reply("ACK,PRIME_START," + line);
}

void stopPrime() {
  if (primeLine == "") return;
  if (primePump  >= 0) pcfOff(primePump);
  if (primeValveA >= 0) pcfOff(primeValveA);
  if (primeValveB >= 0) pcfOff(primeValveB);
  if (primeIsAC) pcfOff(OUT_INVERTER);
  cutoffDeenergize();
  primeLine = ""; primeValveA = primeValveB = primePump = -1; primeIsAC = false; primeCapped = false;
}

// Prime dead-man + generous cap (§A.4.2.2): stop if no keep-alive arrives in time, or the cap elapses.
void primeSafety() {
  if (primeLine == "") return;
  unsigned long now = millis();
  if (now - primeOnMs >= PRIME_CAP_MS) { stopPrime(); reply("DONE,PRIME_CAP"); return; }
  if (now - primeLastMs > PRIME_HOLD_TIMEOUT_MS) { stopPrime(); reply("DONE,PRIME"); }
}
