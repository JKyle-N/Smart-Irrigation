/* =============================================================================
 *  SMART IRRIGATION & FERTIGATION SYSTEM  --  ARDUINO NANO SENSOR HUB
 *  Controller 1 of 3   (Nano  ->  ESP32 #1  ->  ESP32 #2)
 * -----------------------------------------------------------------------------
 *  ROLE (spec sec.10.3, 11.5 / CLAUDE.md): SENSOR ACQUISITION ONLY.
 *    - Reads sensors, frames packets, transmits to ESP32 #1 over UART.
 *    - Obeys a small inbound command set (interval switch + reset).
 *    - Makes NO decisions, controls NO actuators, classifies NO faults.
 *      Invalid sensor reads are reported honestly with a -1 sentinel; ESP32 #1
 *      decides what that means. -1 is a valid "invalid read" flag, NOT garbage.
 *
 *  UART PROTOCOL (spec sec.9.9): framed, UPPERCASE, comma-separated, <=128 bytes
 *      Outbound (Nano -> ESP32 #1):
 *        <START>,ENV,<temp>,<hum>,<END>
 *        <START>,TANK,<res%>,<mix%>,<flowLpm>,<END>
 *        <START>,SOIL,<COL>,<p1>,<p2>,...,<END>      (tag + 2 raw probes per enabled column; disabled omitted)
 *        <START>,LIGHT,<lux>,<END>
 *        <START>,NPK,<COL>,<moist>,<temp>,<EC>,<pH>,<N>,<P>,<K>,<reason>,<END>  (one per enabled column;
 *              reason = OK|TIMEOUT|BADADDR|BADLEN|BADCRC -- OK carries values, the rest carry -1s)
 *        <START>,STATUS,NANO,OK,<END>                (heartbeat, spec sec.10.8.2)
 *      Inbound (ESP32 #1 -> Nano, spec sec.9.7.1.1):
 *        <START>,ACTIVE,<END>     fast interval (10 s)
 *        <START>,DAY,<END>        day-idle interval (20 min)
 *        <START>,NIGHT,<END>      night-idle interval (1 hr)
 *        <START>,RESET_REQ,<END>  clean software self-reset (short-timeout WDT)
 *
 *  RECOVERY LADDER (spec sec.17.4.1 / CLAUDE.md) -- the Nano owns step 1 only:
 *      1. Internal ATmega328 WDT, petted in loop()  <-- primary self-recovery
 *      2. ESP32 #1 RESET_REQ over UART              <-- handled here (self-reset)
 *      3. Hardware reset via ESP32 #2 / PCF8575     <-- not the Nano's concern
 *
 *  NON-BLOCKING: no delay() in loop() except a single one-time settle in setup() and
 *      one bounded ~5 ms RS485 DE settle inside the NPK read (well under WDTO_4S).
 *      Bounded micro-waits (pulseIn timeout, ultrasonic trigger, sensor reads)
 *      are sub-second and never stall the loop past the watchdog timeout.
 *
 *  >>> HARDWARE VALUES TO MEASURE / CONFIRM are tagged  [MEASURE]  and  [CONFIRM]
 *      in the EDITABLE CONSTANTS block below. Tune them without touching logic.
 *
 *  ---- ARDUINO IDE BUILD ----------------------------------------------------
 *  This is an Arduino IDE sketch (Nano/Nano.ino -- folder name must match file).
 *    Board:  Tools > Board > Arduino AVR > Arduino Nano
 *    Processor: "ATmega328P". If a WDT reset boot-loops on an older clone, pick
 *               "ATmega328P (Old Bootloader)" -> if it STILL loops, the chip has
 *               the classic (non-Optiboot) bootloader; flash Optiboot or use a
 *               board whose bootloader clears the WDT after reset.
 *    Upload:  DISCONNECT D0/D1 (UART to ESP32 #1) before uploading, reconnect after.
 *    Libraries (Tools > Manage Libraries -> install):
 *      - "DHT sensor library" by Adafruit
 *      - "Adafruit Unified Sensor" by Adafruit   (DHT dependency)
 *      - "BH1750" by Christopher Laws
 *    (SoftwareSerial for the RS485/NPK bus and Wire for BH1750 I2C ship with the
 *     AVR core; the NPK Modbus query is hand-rolled so a proven config can be pasted.)
 * ========================================================================== */

#include <Arduino.h>
#include <avr/wdt.h>
#include <Wire.h>
#include <BH1750.h>
#include <DHT.h>
#include <SoftwareSerial.h>

/* =============================================================================
 *  EDITABLE CONSTANTS  --  tune here, do not touch the logic below
 * ========================================================================== */

/* ---- Column configuration ------------------------------------------------ *
 * 3 columns exist in code; flip a flag to enable/disable. A disabled column is
 * skipped for BOTH reading and reporting -- its soil pins are NOT read (column C
 * is physically unconnected, so its A6/A7 inputs would float) and its NPK sensor
 * is NOT polled. SOIL omits disabled columns entirely (tag-based, variable-length).
 * Re-enabling later is just flipping the flag -- no structural change.          */
const uint8_t NUM_COLUMNS = 3;
bool COLUMN_ENABLED[NUM_COLUMNS] = { true, true, false };   // A, B, C   [CONFIRM]
const char    COLUMN_TAG[NUM_COLUMNS] = { 'A', 'B', 'C' };

/* ---- Pin assignments (spec sec.17.2-17.3 -- do not invent pins) ----------- */
// Communication: D0/D1 = hardware UART (Serial) to ESP32 #1. Not redefined here.
const uint8_t PIN_FLOW      = 2;    // D2  reservoir flow-detect (INT0, interrupt)
// D3 reserved
const uint8_t PIN_TRIG_RES  = 4;    // D4  ultrasonic reservoir TRIG
const uint8_t PIN_ECHO_RES  = 5;    // D5  ultrasonic reservoir ECHO
const uint8_t PIN_TRIG_MIX  = 6;    // D6  ultrasonic mixing TRIG
const uint8_t PIN_ECHO_MIX  = 7;    // D7  ultrasonic mixing ECHO
const uint8_t PIN_RS485_DE  = 8;    // D8  RS485 DE/RE (HIGH=transmit, LOW=receive)
const uint8_t PIN_RS485_RX  = 9;    // D9  RS485 RX (NPK -> Nano)
const uint8_t PIN_RS485_TX  = 10;   // D10 RS485 TX (Nano -> NPK)
const uint8_t PIN_DHT       = 11;   // D11 DHT22 data
// D12 reserved
const uint8_t PIN_LED       = 13;   // D13 status LED
// Soil capacitive sensors: 2 per column, sent to ESP32 #1 individually (ESP1 averages + maps to %).
// A6/A7 are analog-input only.
const uint8_t SOIL_PINS[NUM_COLUMNS][2] = {
  { A0, A1 },   // Column A
  { A2, A3 },   // Column B
  { A6, A7 },   // Column C
};
// I2C BH1750 uses A4 (SDA) / A5 (SCL) via the Wire library (fixed by hardware).

/* ---- UART link to ESP32 #1 ----------------------------------------------- */
const unsigned long LINK_BAUD = 9600;          // CLAUDE.md locked

/* ---- Transmission + heartbeat intervals (spec sec.17.4.4) ---------------- *
 * Switched at runtime by ACTIVE / DAY / NIGHT commands from ESP32 #1.         */
const unsigned long TX_INTERVAL_ACTIVE_MS = 10000UL;     // 10 s   (actuator active)
const unsigned long TX_INTERVAL_DAY_MS    = 1200000UL;   // 20 min (idle, daytime)
const unsigned long TX_INTERVAL_NIGHT_MS  = 3600000UL;   // 1 hr   (idle, night)
// Heartbeat is separate from data (spec sec.10.8.7) and faster, so ESP32 #1 can
// notice a dead Nano without waiting a whole idle interval.
const unsigned long HB_INTERVAL_ACTIVE_MS = 10000UL;     // 10 s
const unsigned long HB_INTERVAL_DAY_MS    = 60000UL;     // 1 min
const unsigned long HB_INTERVAL_NIGHT_MS  = 120000UL;    // 2 min
// State the Nano boots into until ESP32 #1 commands otherwise. ACTIVE gives fast
// initial data during startup sync (treated as stabilization packets by ESP32 #1).
#define BOOT_TX_STATE  TX_ACTIVE

/* ---- Watchdog ------------------------------------------------------------ *
 * Must comfortably exceed the worst-case single loop iteration (a full sensor
 * cycle, dominated by NPK Modbus timeouts). WDTO_4S leaves a wide margin.      */
#define WDT_TIMEOUT  WDTO_4S

/* ---- One-time sensor settle (setup() only) ------------------------------- */
const unsigned long SENSOR_SETTLE_MS = 2000;   // DHT22 warm-up + ultrasonic settle

/* ---- Conversion constants moved to ESP32 #1 (companion spec §A) ----------- *
 * This Nano now streams RAW signals (raw soil ADC, raw ultrasonic distance cm, raw NPK
 * registers); ESP32 #1 owns and applies all calibration via its Calibration menu. So the soil
 * dry/wet ADC endpoints, the ultrasonic empty/full tank geometry, and the NPK scale factors live
 * on ESP32 #1 (NVS) -- do NOT tune them here, it would have no effect.                          */

/* ---- Ultrasonic raw read (HC-SR04 style) --------------------------------- *
 * distance(cm) = echo_us * SOUND_CM_PER_US. Sent raw; ESP32 #1 maps to %.                       */
const float         SOUND_CM_PER_US      = 0.0343f / 2.0f;  // round-trip -> one way
const unsigned long ULTRASONIC_TIMEOUT_US = 25000UL;        // ~4.3 m cap, bounds pulseIn

/* ---- Reservoir flow sensor (interrupt-counted, never polled) -------------- *
 * Pulses are counted in an ISR and converted to L/min with this K-factor.
 * Calibrate against a measured volume (typical YF-S201 ~= 450).        [MEASURE] */
const float FLOW_K_PULSES_PER_LITER = 450.0f;   // [MEASURE]
// Edge to count. Proven working config uses RISING (matches the bench test code).
#define FLOW_INTERRUPT_EDGE  RISING             // [CONFIRM] (proven test value)

/* =============================================================================
 *  NPK RS485 (7-in-1) MODBUS  --  >>> PASTE YOUR PROVEN CONFIG HERE <<<
 * -----------------------------------------------------------------------------
 *  One 7-in-1 sensor per column, each with a UNIQUE Modbus address, all sharing
 *  the single RS485 bus on D8/D9/D10. readNPK() polls only ENABLED columns.
 *  Everything in this block is vendor-specific -- replace with your working
 *  values. The parser reads NPK_REG_COUNT 16-bit big-endian registers starting
 *  at NPK_REG_START via function NPK_FUNCTION, checks the CRC, and sends the RAW registers.
 *  ESP32 #1 applies the per-element scale + offset (the scale lives there now). On ANY failure
 *  every field is reported as -1.
 * ========================================================================== */
const uint8_t  NPK_ADDR[NUM_COLUMNS] = { 0x03, 0x01, 0x02 };  // A=NPK1(0x03) B=NPK2(0x01) C=NPK3(0x02) -- from FINALTESTCODEWITHNPK
const unsigned long NPK_BAUD = 4800;            // proven working baud (bench test code) [CONFIRM]
const uint8_t  NPK_FUNCTION  = 0x03;            // 0x03 read-holding (some use 0x04) [CONFIRM]
const uint16_t NPK_REG_START = 0x0000;          // first register               [CONFIRM]
const uint16_t NPK_REG_COUNT = 7;               // moist,temp,EC,pH,N,P,K        [CONFIRM]
const unsigned long NPK_RESPONSE_TIMEOUT_MS = 1000;  // bounded wait for reply (proven test value)
// Print precision per field (decimals) for the RAW register outbound packet (moist,temp,EC,pH,N,P,K).
const uint8_t NPK_PREC[7]  = { 1, 1, 0, 1, 0, 0, 0 };
/* ---- end NPK editable block ---------------------------------------------- */

/* =============================================================================
 *  GLOBALS  (no further user tuning below this line)
 * ========================================================================== */
DHT          dht(PIN_DHT, DHT22);
BH1750       lightMeter;                                  // I2C on A4/A5
SoftwareSerial npkSerial(PIN_RS485_RX, PIN_RS485_TX);     // RX=D9, TX=D10

enum TxState { TX_ACTIVE, TX_DAY, TX_NIGHT };
TxState txState = BOOT_TX_STATE;

volatile unsigned long flowPulseCount = 0;     // incremented in ISR
unsigned long lastFlowCalcMs = 0;

unsigned long lastTxMs = 0;
unsigned long lastHbMs = 0;
bool          forceTx  = false;                // send immediately on interval switch

char pkt[140];                                 // outbound packet builder (cap 128 enforced)

#define INBOUND_BUF 96
char    inBuf[INBOUND_BUF];
uint8_t inLen = 0;

bool ledState = false;

/* ---- Calibration raw streaming (companion spec §A.3) --------------------- *
 * While ESP32 #1 has a Nano sensor selected, stream ONLY that sensor's raw value fast
 * (~1.6 Hz) via <START>,CAL,<id>,<raw>,<END>. Additive: normal cadence is unaffected.       */
char     calId[12]   = "";
bool     calActive   = false;
unsigned long lastCalMs = 0;
const unsigned long CAL_STREAM_MS = 600;

/* ---- Forward declarations ------------------------------------------------ */
void pollUart();
void handleLine(char *line);
void setTxState(TxState s);
unsigned long getTxInterval();
unsigned long getHbInterval();
void sendEnv();
void sendTank();
void sendSoil();
void sendLight();
void sendNpk();
void sendStatus();
bool  readEnv(float &t, float &h);
float readDistanceCm(uint8_t trig, uint8_t echo);
void  calStreamTick();
bool  calReadRaw(float &raw);
float readLux();
float computeFlowLpm();
// NPK read outcome (companion spec §A): OK carries values, the rest identify WHY the read failed so
// ESP1 can tell "no response" (TIMEOUT/BADADDR -> dead device/wiring) from "corrupted" (BADCRC -> bus EMI).
enum NpkResult { NPK_OK = 0, NPK_TIMEOUT, NPK_BADADDR, NPK_BADLEN, NPK_BADCRC };
uint8_t  readNpkColumn(uint8_t addr, float out[7]);   // returns an NpkResult code
const char *npkReasonStr(uint8_t rc);
uint16_t modbusCRC(const uint8_t *buf, uint8_t len);
void startPkt(const char *cmd);
void pktAddRaw(const char *s);
void pktAddIntOrInvalid(long v, bool valid);
void pktAddFloatOrInvalid(float v, uint8_t prec, bool valid);
void endPkt();
void softwareReset();
void flowISR();

/* ---- Disable the watchdog very early, before setup() --------------------- *
 * Belt-and-suspenders against a WDT reset boot-loop: clears the reset flags and
 * turns the WDT off before any sketch code runs.                              */
void wdtEarlyDisable(void) __attribute__((naked, used, section(".init3")));
void wdtEarlyDisable(void) {
  MCUSR = 0;
  wdt_disable();
}

/* =============================================================================
 *  SETUP  --  the only place a blocking settle delay is allowed
 * ========================================================================== */
void setup() {
  MCUSR = 0;
  wdt_disable();

  Serial.begin(LINK_BAUD);          // hardware UART to ESP32 #1 (D0/D1)
  Wire.begin();                     // I2C for BH1750 (A4/A5)
  lightMeter.begin();
  dht.begin();
  npkSerial.begin(NPK_BAUD);        // RS485 bus

  pinMode(PIN_RS485_DE, OUTPUT);
  digitalWrite(PIN_RS485_DE, LOW);  // default to receive

  pinMode(PIN_FLOW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), flowISR, FLOW_INTERRUPT_EDGE);

  pinMode(PIN_TRIG_RES, OUTPUT); digitalWrite(PIN_TRIG_RES, LOW);
  pinMode(PIN_ECHO_RES, INPUT);
  pinMode(PIN_TRIG_MIX, OUTPUT); digitalWrite(PIN_TRIG_MIX, LOW);
  pinMode(PIN_ECHO_MIX, INPUT);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  unsigned long now = millis();
  lastTxMs = now;
  lastHbMs = now;
  lastFlowCalcMs = now;

  delay(SENSOR_SETTLE_MS);          // ONE-TIME settle (DHT warm-up). Allowed here only.

  wdt_enable(WDT_TIMEOUT);          // arm watchdog -- primary self-recovery
}

/* =============================================================================
 *  LOOP  --  fully non-blocking
 * ========================================================================== */
void loop() {
  wdt_reset();                      // pet the watchdog every iteration

  pollUart();                       // handle inbound ACTIVE/DAY/NIGHT/RESET_REQ/CAL_*
  calStreamTick();                  // §A.3: stream the selected sensor's raw value when active

  unsigned long now = millis();

  if (forceTx || (unsigned long)(now - lastTxMs) >= getTxInterval()) {
    lastTxMs = now;
    forceTx  = false;
    sendEnv();
    sendTank();
    sendSoil();
    sendLight();
    sendNpk();
  }

  if ((unsigned long)(now - lastHbMs) >= getHbInterval()) {
    lastHbMs = now;
    sendStatus();
    ledState = !ledState;
    digitalWrite(PIN_LED, ledState ? HIGH : LOW);
  }
}

/* =============================================================================
 *  INBOUND UART  --  parse framed commands from ESP32 #1
 * ========================================================================== */
void pollUart() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (inLen > 0) {
        inBuf[inLen] = '\0';
        handleLine(inBuf);
        inLen = 0;
      }
    } else if (inLen < INBOUND_BUF - 1) {
      inBuf[inLen++] = c;
    } else {
      inLen = 0;                    // overflow: drop the malformed line
    }
  }
}

// Validate framing and dispatch. Unknown/malformed lines are ignored silently
// (the Nano does not classify garbage -- that is ESP32 #1's job).
void handleLine(char *line) {
  char *s = strstr(line, "<START>");
  char *e = strstr(line, "<END>");
  if (!s || !e || e < s) return;

  char *p = s + 7;                  // step past "<START>"
  if (*p == ',') p++;

  char cmd[16];
  uint8_t n = 0;
  while (*p && *p != ',' && *p != '<' && n < (uint8_t)(sizeof(cmd) - 1)) {
    cmd[n++] = *p++;
  }
  cmd[n] = '\0';

  if      (strcmp(cmd, "ACTIVE")    == 0) setTxState(TX_ACTIVE);
  else if (strcmp(cmd, "DAY")       == 0) setTxState(TX_DAY);
  else if (strcmp(cmd, "NIGHT")     == 0) setTxState(TX_NIGHT);
  else if (strcmp(cmd, "RESET_REQ") == 0) softwareReset();
  else if (strcmp(cmd, "CAL_START") == 0) {       // §A.3: begin fast raw stream for one sensor
    if (*p == ',') p++;
    uint8_t k = 0;                                 // copy the SENSOR_ID token
    while (*p && *p != ',' && *p != '<' && k < (uint8_t)(sizeof(calId) - 1)) calId[k++] = *p++;
    calId[k] = '\0';
    calActive = (k > 0);
    if (!strcmp(calId, "FLOW")) {                  // reservoir-flow cal: zero so the run starts at 0
      noInterrupts(); flowPulseCount = 0; interrupts();
    }
  }
  else if (strcmp(cmd, "CAL_STOP")  == 0) { calActive = false; calId[0] = '\0'; }
  // else: ignore
}

void setTxState(TxState s) {
  txState = s;
  forceTx = true;                   // send a fresh cycle right away on switch
}

unsigned long getTxInterval() {
  switch (txState) {
    case TX_ACTIVE: return TX_INTERVAL_ACTIVE_MS;
    case TX_DAY:    return TX_INTERVAL_DAY_MS;
    default:        return TX_INTERVAL_NIGHT_MS;
  }
}

unsigned long getHbInterval() {
  switch (txState) {
    case TX_ACTIVE: return HB_INTERVAL_ACTIVE_MS;
    case TX_DAY:    return HB_INTERVAL_DAY_MS;
    default:        return HB_INTERVAL_NIGHT_MS;
  }
}

/* =============================================================================
 *  OUTBOUND PACKETS
 * ========================================================================== */
void sendEnv() {
  float t, h;
  bool ok = readEnv(t, h);
  startPkt("ENV");
  pktAddFloatOrInvalid(t, 1, ok);
  pktAddFloatOrInvalid(h, 1, ok);
  endPkt();
}

// RAW packet (companion spec §A): reservoir + mixing distances in cm (ESP32 #1 maps to %
// using the stored empty/full geometry + zero offset); flow in L/min (ESP1 applies its scale).
void sendTank() {
  float res  = readDistanceCm(PIN_TRIG_RES, PIN_ECHO_RES);
  float mix  = readDistanceCm(PIN_TRIG_MIX, PIN_ECHO_MIX);
  float flow = computeFlowLpm();    // always valid (0 if no flow)
  startPkt("TANK");
  pktAddFloatOrInvalid(res, 1, res >= 0.0f);
  pktAddFloatOrInvalid(mix, 1, mix >= 0.0f);
  pktAddFloatOrInvalid(flow, 1, true);
  endPkt();
}

// Tag-based, variable-length: one <tag>,<probe1>,<probe2> TRIPLET per ENABLED column; disabled
// columns are omitted entirely (their pins are never read). Both raw capacitive probes are sent
// individually (NOT pre-averaged) so ESP32 #1 can log/diagnose each probe; ESP32 #1 averages the
// two for the control value and maps to % with the stored air/water endpoints (companion spec §A).
void sendSoil() {
  // If every column is disabled there are no triplets to send; an empty <START>,SOIL,<END>
  // would be rejected as garbage by ESP32 #1, so skip the packet entirely.
  bool anyEnabled = false;
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) if (COLUMN_ENABLED[c]) { anyEnabled = true; break; }
  if (!anyEnabled) return;

  char tag[2] = { 0, 0 };
  startPkt("SOIL");
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c]) continue;               // omit disabled column entirely
    tag[0] = COLUMN_TAG[c];
    pktAddRaw(tag);                                  // column tag
    pktAddIntOrInvalid(analogRead(SOIL_PINS[c][0]), true);   // probe 1 raw ADC
    pktAddIntOrInvalid(analogRead(SOIL_PINS[c][1]), true);   // probe 2 raw ADC
  }
  endPkt();
}

void sendLight() {
  float lux = readLux();
  startPkt("LIGHT");
  pktAddFloatOrInvalid(lux, 0, lux >= 0.0f);
  endPkt();
}

void sendNpk() {
  char tag[2] = { 0, 0 };
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c]) continue;               // skip disabled columns entirely
    wdt_reset();                                    // longest read -- keep WDT happy
    float vals[7];
    uint8_t rc = readNpkColumn(NPK_ADDR[c], vals);
    bool ok = (rc == NPK_OK);
    startPkt("NPK");
    tag[0] = COLUMN_TAG[c];
    pktAddRaw(tag);
    for (uint8_t i = 0; i < 7; i++) {
      pktAddFloatOrInvalid(vals[i], NPK_PREC[i], ok);
    }
    pktAddRaw(npkReasonStr(rc));                     // trailing reason token (OK on success, else why it failed)
    endPkt();
  }
}

void sendStatus() {
  startPkt("STATUS");
  pktAddRaw("NANO");
  pktAddRaw("OK");
  endPkt();
}

/* =============================================================================
 *  SENSOR READS  (all bounded, no delay())
 * ========================================================================== */
bool readEnv(float &t, float &h) {
  h = dht.readHumidity();
  t = dht.readTemperature();
  return !(isnan(h) || isnan(t));
}

// Raw one-way distance in cm (echo timing), or -1.0 on timeout. Sent raw (ESP1 maps to %).
float readDistanceCm(uint8_t trig, uint8_t echo) {
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long dur = pulseIn(echo, HIGH, ULTRASONIC_TIMEOUT_US);
  if (dur == 0) return -1.0f;
  return (float)dur * SOUND_CM_PER_US;
}

// Returns lux, or -1.0 on sensor error.
float readLux() {
  float lux = lightMeter.readLightLevel();
  return (lux < 0.0f) ? -1.0f : lux;
}

/* =============================================================================
 *  CALIBRATION RAW STREAM  (companion spec §A.3)
 * ========================================================================== */
// Read the RAW value for the active CAL sensor. Returns false if the id is unknown.
bool calReadRaw(float &raw) {
  if (!strncmp(calId, "SOIL_", 5)) {
    int col = calId[5] - 'A';                      // A/B/C
    if (col < 0 || col >= NUM_COLUMNS) return false;
    if (calId[6] == '\0') {                         // "SOIL_A" -> column AVERAGE (ESP1 calibrates the 2-probe avg)
      raw = (float)((analogRead(SOIL_PINS[col][0]) + analogRead(SOIL_PINS[col][1])) / 2);
      return true;
    }
    int ch = calId[6] - '1';                        // "SOIL_A1"/"A2" -> single channel 0/1
    if (ch < 0 || ch > 1) return false;
    raw = (float)analogRead(SOIL_PINS[col][ch]);    // raw 10-bit ADC
    return true;
  }
  if (!strncmp(calId, "NPK_", 4)) {                 // raw N register for that column (display-only, §A.3)
    int col = calId[4] - 'A';
    if (col < 0 || col >= NUM_COLUMNS) return false;
    float vals[7];
    if (readNpkColumn(NPK_ADDR[col], vals) != NPK_OK) return false;
    raw = vals[4];                                  // N (index 4) raw register
    return true;
  }
  if (!strcmp(calId, "ULTRA_RES")) { raw = readDistanceCm(PIN_TRIG_RES, PIN_ECHO_RES); return true; }
  if (!strcmp(calId, "ULTRA_MIX")) { raw = readDistanceCm(PIN_TRIG_MIX, PIN_ECHO_MIX); return true; }
  if (!strcmp(calId, "DHT_T")) { float t = dht.readTemperature(); if (isnan(t)) return false; raw = t; return true; }
  if (!strcmp(calId, "DHT_H")) { float h = dht.readHumidity();    if (isnan(h)) return false; raw = h; return true; }
  if (!strcmp(calId, "LUX"))   { raw = readLux(); return true; }
  if (!strcmp(calId, "FLOW")) {                    // reservoir flow: accumulated raw pulses
    noInterrupts(); unsigned long p = flowPulseCount; interrupts();
    raw = (float)p; return true;
  }
  return false;
}

void calStreamTick() {
  if (!calActive) return;
  // DHT22 (~0.5 Hz) and NPK (slow Modbus) cap their cal cadence at 2 s; others stream fast.
  unsigned long cadence = (strncmp(calId, "DHT", 3) == 0 || strncmp(calId, "NPK", 3) == 0) ? 2000UL : CAL_STREAM_MS;
  if ((unsigned long)(millis() - lastCalMs) < cadence) return;
  lastCalMs = millis();
  float raw;
  bool ok = calReadRaw(raw);
  startPkt("CAL");
  pktAddRaw(calId);
  pktAddFloatOrInvalid(raw, 1, ok);                // -1 if the id is unknown / read failed
  endPkt();
}

// Converts pulses accumulated since the last call to L/min. Always returns >=0.
float computeFlowLpm() {
  // While the reservoir flow is being CALIBRATED, the CAL stream owns flowPulseCount (it must
  // accumulate across the run) -- do NOT consume/zero it here, just report 0 for the normal packet.
  if (calActive && !strcmp(calId, "FLOW")) return 0.0f;
  unsigned long now = millis();
  noInterrupts();
  unsigned long pulses = flowPulseCount;
  flowPulseCount = 0;
  interrupts();

  unsigned long elapsed = now - lastFlowCalcMs;
  lastFlowCalcMs = now;
  if (elapsed == 0) return 0.0f;

  // L/min = (pulses / K_per_liter) / (elapsed_ms / 60000)
  return (float)pulses * 60000.0f / (FLOW_K_PULSES_PER_LITER * (float)elapsed);
}

/* =============================================================================
 *  NPK RS485 MODBUS  (hand-rolled so the proven config can be pasted above)
 * ========================================================================== */
// Returns true and fills out[0..6] on a valid CRC-checked reply; false otherwise.
const char *npkReasonStr(uint8_t rc) {
  switch (rc) {
    case NPK_OK:      return "OK";
    case NPK_TIMEOUT: return "TIMEOUT";
    case NPK_BADADDR: return "BADADDR";
    case NPK_BADLEN:  return "BADLEN";
    default:          return "BADCRC";
  }
}

uint8_t readNpkColumn(uint8_t addr, float out[7]) {
  for (uint8_t i = 0; i < 7; i++) out[i] = -1.0f;

  uint8_t q[8];
  q[0] = addr;
  q[1] = NPK_FUNCTION;
  q[2] = highByte(NPK_REG_START);
  q[3] = lowByte(NPK_REG_START);
  q[4] = highByte(NPK_REG_COUNT);
  q[5] = lowByte(NPK_REG_COUNT);
  uint16_t crc = modbusCRC(q, 6);
  q[6] = lowByte(crc);
  q[7] = highByte(crc);

  while (npkSerial.available()) npkSerial.read();   // drop stale bytes

  digitalWrite(PIN_RS485_DE, HIGH);                 // transmit
  // The ONE exception to the "no delay() outside setup()" rule: a single bounded ~5 ms
  // RS485 DE settle (proven value). Far under WDTO_4S and the loop pets the WDT around it.
  delay(5);
  npkSerial.write(q, 8);                            // SoftwareSerial.write blocks until sent
  npkSerial.flush();                                // ensure last stop bit is shifted out
  digitalWrite(PIN_RS485_DE, LOW);                  // back to receive

  const uint8_t expected = 3 + 2 * NPK_REG_COUNT + 2;   // addr+func+bytecount + data + crc
  uint8_t resp[40];
  uint8_t idx = 0;
  unsigned long start = millis();
  while (idx < expected &&
         (unsigned long)(millis() - start) < NPK_RESPONSE_TIMEOUT_MS) {
    if (npkSerial.available()) resp[idx++] = (uint8_t)npkSerial.read();
  }

  if (idx < expected)                             return NPK_TIMEOUT;   // no / short response
  if (resp[0] != addr || resp[1] != NPK_FUNCTION) return NPK_BADADDR;    // wrong responder / function
  if (resp[2] != 2 * NPK_REG_COUNT)               return NPK_BADLEN;     // unexpected byte count

  uint16_t rcrc = modbusCRC(resp, expected - 2);
  if (lowByte(rcrc) != resp[expected - 2] ||
      highByte(rcrc) != resp[expected - 1])       return NPK_BADCRC;     // corrupted (bus noise/EMI)

  // RAW 16-bit registers (companion spec §A): ESP32 #1 applies NPK_SCALE + per-element offset.
  for (uint8_t i = 0; i < 7; i++) {
    uint16_t raw = ((uint16_t)resp[3 + 2 * i] << 8) | resp[4 + 2 * i];
    out[i] = (float)raw;
  }
  return NPK_OK;
}

uint16_t modbusCRC(const uint8_t *buf, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= (uint16_t)buf[i];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
      else              { crc >>= 1; }
    }
  }
  return crc;
}

/* =============================================================================
 *  PACKET BUILDER  (frames <START>,...,<END>, enforces <=128 bytes)
 * ========================================================================== */
void startPkt(const char *cmd) {
  strcpy(pkt, "<START>,");
  strcat(pkt, cmd);
}

void pktAddRaw(const char *s) {
  strcat(pkt, ",");
  strcat(pkt, s);
}

void pktAddIntOrInvalid(long v, bool valid) {
  strcat(pkt, ",");
  if (!valid) { strcat(pkt, "-1"); return; }
  char t[12];
  ltoa(v, t, 10);
  strcat(pkt, t);
}

void pktAddFloatOrInvalid(float v, uint8_t prec, bool valid) {
  strcat(pkt, ",");
  if (!valid) { strcat(pkt, "-1"); return; }
  char t[16];
  dtostrf(v, 0, prec, t);
  strcat(pkt, t);
}

void endPkt() {
  strcat(pkt, ",<END>");
  if (strlen(pkt) <= 128) {
    Serial.println(pkt);            // \r\n terminator; framing markers bound the payload
  }
  // oversize packets are dropped rather than sent truncated
}

/* =============================================================================
 *  RESET  --  RESET_REQ handler: clean self-reset via short-timeout WDT
 * ========================================================================== */
void softwareReset() {
  wdt_enable(WDTO_15MS);
  while (1) { }                     // let the watchdog fire -> true hardware reset
}

/* =============================================================================
 *  ISR  --  reservoir flow pulse counter
 * ========================================================================== */
void flowISR() {
  flowPulseCount++;
}
