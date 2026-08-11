/* =============================================================================
 *  SMART IRRIGATION & FERTIGATION SYSTEM  --  ESP32 #1  MASTER CONTROLLER
 *  Controller 2 of 3   (Nano  ->  ESP32 #1  ->  ESP32 #2)
 * -----------------------------------------------------------------------------
 *  ROLE (spec sec.10.1 / CLAUDE.md): SINGLE DECISION-MAKING AUTHORITY.
 *    - Ingests framed sensor packets from the Nano (sec.9.9).
 *    - Decides per-column irrigation vs. fertigation (windowed schedule + soil
 *      threshold + NPK gap), and hands COMPLETE work orders to ESP32 #2
 *      (supervisor / work-order model, sec.9.8.1.1) -- then monitors.
 *    - Talks to the user over SIM800L SMS (alerts, run notices, daily report).
 *    - Logs every event to microSD (CSV, RTC-stamped, sec.25).
 *    - Monitors battery via INA226 (display/log/telemetry only -- no battery-triggered
 *      stops/alerts); classifies OTHER faults (3 tiers, sec.23).
 *    - Drives the LCD UI; owns all recovery escalation (sec.18.9).
 *
 *  AUTHORITY BOUNDARIES: ESP32 #1 decides; ESP32 #2 executes; Nano senses.
 *  This file implements ESP32 #1's side of every protocol. ESP32 #2 (controller
 *  3) is built last, so the ESP2-facing work-order path is implemented to spec
 *  here but can only be end-to-end tested once ESP32 #2 exists.
 *
 *  NON-BLOCKING: no delay() in loop(); the SIM800L AT power-up sequence uses
 *  one-time delays in setup() only (proven sequence). Task watchdog petted each
 *  loop iteration.
 *
 *  DELEGATED (spec sec.12.4 / CLAUDE.md): the mg/kg -> mL dosing CALCULATION is
 *  out of scope and left as a clearly-marked STUB with editable tables.
 *
 *  >>> Values to MEASURE / set at commissioning are tagged [MEASURE] / [TBD].
 * ========================================================================== */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <SoftwareSerial.h>      // EspSoftwareSerial (plerup) -- Nano link
#include <esp_task_wdt.h>
#include <WiFi.h>                // built-in ESP32 WiFi (telemetry uplink, Part A)
#include <HTTPClient.h>          // ThingSpeak HTTP upload + Supabase CSV upload
#include <WiFiClientSecure.h>    // TLS client for the Supabase Storage + Firebase RTDB HTTPS uploads
#include <ArduinoJson.h>         // Firebase live-snapshot JSON payload
#include <WebServer.h>           // SoftAP provisioning portal (WiFi setup form)
#include <DNSServer.h>           // captive-portal DNS for the provisioning AP

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

/* =============================================================================
 *  EDITABLE CONSTANTS  --  tune here; logic below does not need editing
 * ========================================================================== */

/* ---- Pins (spec sec.18 -- verbatim) -------------------------------------- */
#define NANO_RX_PIN   16        // EspSoftwareSerial RX  (<- Nano D1 TX)
#define NANO_TX_PIN   17        // EspSoftwareSerial TX  (-> Nano D0 RX)
#define ESP2_RX_PIN   33        // HW UART1 RX  (<- ESP2 GPIO17 TX)
#define ESP2_TX_PIN   25        // HW UART1 TX  (-> ESP2 GPIO16 RX)
#define SIM_RX_PIN    26        // HW UART2 RX  (<- SIM800L TX)
#define SIM_TX_PIN    27        // HW UART2 TX  (-> SIM800L RX)
#define I2C_SDA       21
#define I2C_SCL       22
#define SD_SCK        18
#define SD_MISO       19
#define SD_MOSI       23
#define SD_CS          5
#define BTN_UP         0        // NOTE: GPIO0/2/12/15 are boot-strapping pins
#define BTN_DOWN      12
#define BTN_ENTER     13
#define BTN_BACK      15
#define BTN_MODE       2
#define ESP2_PWR_PIN   4        // ESP2 power relay (active HIGH)
#define PIN_BATT_I    34        // battery CURRENT via opto -> ADC1 (nonlinear; polynomial-calibrated)
#define PIN_BATT_V    35        // battery VOLTAGE via opto -> ADC1 (nonlinear; polynomial-calibrated)

/* ---- Battery safety: READY, NOT IMPLEMENTED ----------------------------------------------------- *
 * The INA226-tied battery protections (BATTERY_CRITICAL emergency-stop, run-block, low-battery
 * fertigation disable + alerts, and the pump-exercise gate) were removed by operator request. They are
 * SCAFFOLDED behind this flag: set to 1 to re-enable all of them (the code is present but compiled out).
 * BATT_LOW_V / BATT_CRIT_V thresholds are kept. (The old INA226 device-loss auto-reboot is moot -- battery
 * is now read from the ADC, not the INA226 -- so it is not scaffolded.)                              */
#define BATTERY_SAFETY_ENABLED 0

/* ---- Bauds --------------------------------------------------------------- */
#define DEBUG_BAUD   115200
#define NANO_BAUD      9600
#define ESP2_BAUD      9600
#define SIM_BAUD       9600

/* ---- I2C addresses ------------------------------------------------------- */
#define LCD_ADDR     0x27
#define INA226_ADDR  0x40
// DS3231 0x68 and EEPROM 0x57 are auto-addressed by their libraries / unused.

/* ---- GSM ----------------------------------------------------------------- */
String PHONE_NUMBER = "09150424784";    // [CONFIRM] recipient for alerts/reports

/* ---- Columns (spec sec.14.1.4.1) ----------------------------------------- */
#define NUM_COLUMNS 3
// Runtime + NVS (Column Mode OFF/ON on the LCD flips this, sec.18.10.7). NOTE: this
// is ESP1-SIDE ONLY -- the Nano keeps its own compiled COLUMN_ENABLED, so toggling a
// column here does not change which columns the Nano physically reads (sec.18.10.7.4).
bool COLUMN_ENABLED[NUM_COLUMNS] = { true, true, false };   // A, B, C [CONFIRM]
const char COL_TAG[NUM_COLUMNS] = { 'A', 'B', 'C' };

/* ---- Irrigation thresholds & windows (spec sec.14.1.3, windowed) --------- *
 * Windowed scheduling: a column is serviced only inside its RTC window, and
 * within the window the soil start/stop hysteresis triggers the run.          */
// Editable at commissioning AND on-device via Settings menu (persisted to NVS).
int soilStartPct = 35;   // start irrigation below this %   [TBD]
int soilStopPct  = 45;   // stop irrigation above this %    [TBD]
// DEFAULT (AUTO) per-column service window, minutes since midnight {start, end}  [TBD]
const uint16_t DEF_WIN_START[NUM_COLUMNS] = { 6*60, 8*60, 6*60 };   // 06:00 / 08:00 / 06:00
const uint16_t DEF_WIN_END[NUM_COLUMNS]   = { 8*60, 10*60, 8*60 };  // 08:00 / 10:00 / 08:00
// MANUAL (operator-set) window, used when colSchedMode==SCHED_MANUAL (sec.18.10.7.2).
uint16_t COL_WIN_START[NUM_COLUMNS] = { 6*60, 8*60, 6*60 };
uint16_t COL_WIN_END[NUM_COLUMNS]   = { 8*60, 10*60, 8*60 };
// Schedule axis per column: AUTO = use DEF_* window; MANUAL = use COL_WIN_* (perpetual).
enum SchedMode { SCHED_AUTO = 0, SCHED_MANUAL = 1 };
uint8_t colSchedMode[NUM_COLUMNS] = { SCHED_AUTO, SCHED_AUTO, SCHED_AUTO };

/* ---- Fertigation decision (spec sec.14.2.0) ------------------------------ */
float fertGap = 30.0f;                        // mg/kg below target -> fertigate (FERTIGATE_TRIGGER_GAP, §6; NVS) [TBD]
const float FLUSH_PCT             = 20.0f;    // post-fertigation flush % (sec.14.2.0.2)
const unsigned long MIXING_DURATION_MS = 30000UL;  // homogenize time           [TBD]

/* ---- EC / pH safe window (pushed to ESP2, spec sec.14.2.4) --------------- */
const float EC_MIN = 0.0f,  EC_MAX = 3.0f;    // [TBD]
const float PH_MIN = 5.0f,  PH_MAX = 7.0f;    // [TBD]

/* =============================================================================
 *  CALIBRATION CONSTANTS  (companion spec §A) -- own NVS namespace "calib"
 * -----------------------------------------------------------------------------
 *  Subsystems stream RAW; ESP32 #1 applies these. Nano-owned consts are applied here when
 *  interpreting packets; ESP2-owned consts (flow K, EC/pH, ACS712 zero) are the authoritative
 *  copy ESP1 pushes to ESP2 (SET_CAL + startup sync + work order, §A.5.1). Defaults = bench.   */
// Nano-applied:
// Per-column soil endpoints (raw ADC), applied to the 2-probe average. Measured on the deployed rig
// (dry = HIGH ADC, wet = LOW). The probes were found CROSS-WIRED: column A's two ADC channels are
// physically {B2, A2} and column B's are {B1, A1}. Per-probe endpoints (dry/wet):
//   A1 748/392  A2 661/518  B1 666/390  B2 650/529
// Column endpoint = average of its two (corrected) probes:
//   A = {B2,A2} -> dry (650+661)/2=656, wet (529+518)/2=524
//   B = {B1,A1} -> dry (666+748)/2=707, wet (390+392)/2=391
//   C: disabled -> defaults          [MEASURE per rig]
int   calSoilAir[NUM_COLUMNS]   = { 656, 707, 800 };   // raw ADC when DRY  (maps to 0%)
int   calSoilWater[NUM_COLUMNS] = { 524, 391, 300 };   // raw ADC when WET  (maps to 100%)
// A capacitive probe reading <=LO or >=HI is treated as disconnected/shorted and dropped from the combine.
const int SOIL_ADC_RAIL_LO = 8, SOIL_ADC_RAIL_HI = 1015;
// Measured on the rig: sensor-to-water 38 cm = empty (0 %), 11 cm = full (100 %).
// NOTE: loadCal() overrides these from the `calib` NVS namespace, so a value captured earlier by
// the Calibration menu WINS over these defaults. If the tank still reads wrong after flashing,
// re-run Calibration > ultrasonic (or clear NVS) -- editing the constant alone will not take.
float calResEmptyCm = 38.0f, calResFullCm = 11.0f;     // ultrasonic geometry (moved from Nano) [MEASURED]
float calMixEmptyCm = 50.0f, calMixFullCm = 4.0f;
float calFlowResScale = 1.0f;                          // reservoir flow correction factor
float calNpkScale[7] = { 10, 10, 1, 10, 1, 1, 1 };     // raw register -> engineering (moved from Nano)
float calNpkOff[NUM_COLUMNS][3] = { {0,0,0}, {0,0,0}, {0,0,0} };  // N/P/K offset trim (default 0, §A.4.3)
float calTempOff = 0, calHumOff = 0, calLuxOff = 0;    // env/light offset trims
// ESP2-owned (ESP1 holds authoritative copy, pushes to ESP2):
float calKResMix = 450, calKMixIrr = 450, calKNut[3] = { 450, 450, 450 };
float calKNutD = 450, calKPhUp = 450, calKPhDn = 450;
float calPhM = 0.0036621f, calPhB = 0, calEcM = 0.0009766f, calEcB = 0;
float calAcs712Zero = 1.65f;
// Runtime: live raw stream + SET_CAL ack capture (block-until-ACK).
String lastSetCalAck = "";              // arg of the last ACK,SET_CAL,<id>
String calRxId = "";                    // sensor id of the last CAL sample received
float  calRxRaw = 0; bool calRxValid = false; unsigned long calRxMs = 0;

// Map a raw soil ADC to 0..100 % using the per-column endpoints (dry=high ADC, wet=low ADC).
// Column moisture is CAPACITIVE-ONLY: the NPK 7-in-1 moisture field is never blended in here (it reads
// unreliably in unsaturated soil) -- the NPK parser writes only sensor.npk[c][*], never sensor.soil[c].
static int soilPct(int col, int raw) {
  long p = map(raw, calSoilAir[col], calSoilWater[col], 0, 100);
  if (p < 0) p = 0; if (p > 100) p = 100;
  return (int)p;
}
// Combine the two capacitive probes of a column, dropping one that reads a rail (disconnected/shorted)
// so a single dead probe can't peg the column. Returns 0..100 %, or -1 if BOTH probes look dead.
static int soilCombine(int col, int v1, int v2) {
  bool ok1 = (v1 > SOIL_ADC_RAIL_LO && v1 < SOIL_ADC_RAIL_HI);
  bool ok2 = (v2 > SOIL_ADC_RAIL_LO && v2 < SOIL_ADC_RAIL_HI);
  int raw;
  if      (ok1 && ok2) raw = (v1 + v2) / 2;   // both good -> average
  else if (ok1)        raw = v1;              // one railed -> use the good probe
  else if (ok2)        raw = v2;
  else                 return -1;             // both look disconnected -> honest invalid
  return soilPct(col, raw);
}
// Map a raw ultrasonic distance (cm) to a WHOLE percent, 0..100, using empty/full geometry.
// Rounded to an integer on purpose: an ultrasonic ranger's real resolution is coarser than 1 % of
// a 27 cm span, so the decimals were noise being displayed and logged as if they meant something.
// The clamp is hard at both ends -- a reading past "full" reports exactly 100, never 101.
static float levelPct(float distCm, float emptyCm, float fullCm) {
  if (emptyCm == fullCm) return 0;                    // degenerate geometry -> refuse to divide by 0
  float pct = (emptyCm - distCm) * 100.0f / (emptyCm - fullCm);
  pct = roundf(pct);
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

/* ---- Tank levels (%) (spec sec.14.4) ------------------------------------- */
const float RES_LOW_PCT      = 15.0f;   // reservoir too low -> block ops  [TBD]
const float MIX_TARGET_PCT   = 70.0f;   // mixing tank fill target          [TBD]
const float MIX_MAX_PCT      = 95.0f;   // mixing tank overflow guard       [TBD]

/* ---- Battery (INA226) (spec sec.21.1) ------------------------------------ */
const float BATT_LOW_V       = 11.8f;   // disable fertigation below        [TBD]
const float BATT_CRIT_V      = 11.2f;   // stop all below                   [TBD]
const float INA226_SHUNT_OHMS   = 0.002f;   // shunt resistor value         [MEASURE]
const float INA226_MAX_CURRENT_A = 10.0f;   // expected max current         [MEASURE]

/* ---- Battery CURRENT: ACS758-050B via ADS1115 (replaces the opto ADC) ------ *
 * The GPIO34 opto path logged I=0.00 on all 44,303 samples across 16 days -- battery %, the daily
 * Wh totals and the low/critical thresholds never had real data. An ADS1115 gives 125 uV/LSB against
 * a stable internal reference, so the current is now read from a hall sensor on the battery wire.
 *
 * WIRING: the ACS758 runs on 5 V and its output sits at VCC/2 (~2.5 V), swinging 0.5..4.5 V over
 * +/-50 A. The ADS1115 must be powered from 3.3 V (at 5 V its logic-high threshold is 0.7*VDD =
 * 3.5 V, which the ESP32's 3.3 V I2C cannot reach) and NO input may exceed VDD+0.3 V. So a 2:1
 * divider on A0 is REQUIRED -- wired direct, anything above ~+27 A drives the input past the limit.
 * With 10k/10k: zero -> 1.25 V, +/-50 A -> 0.25..2.25 V, inside the +/-4.096 V PGA, ~6 mA/LSB.
 * Set ACS_DIV to 1.0 only if the divider is genuinely absent.                                     */
#define ACS_ADDR        0x48        // ADS1115 ADDR -> GND (no clash: LCD 0x27, EEPROM 0x57, RTC 0x68)
#define ACS_CHANNEL     0           // ACS758 output on A0; A1..A3 unused
const float ACS_DIV          = 2.0f;    // (R1+R2)/R2 in front of A0                    [MEASURE]
const float ACS_ZERO_V       = 2.5f;    // sensor output at 0 A = VCC/2 (050B)          [MEASURE]
const float ACS_SENS_V_PER_A = 0.040f;  // 40 mV/A = ACS758LCB-050B                     [CONFIRM]
const uint8_t ACS_PGA        = 1;       // 1 = +/-4.096 V full scale (125 uV/LSB)
// ThingSpeak Ch3 ("Chem") slot for the battery current. Ch2 "System" is full (all 8 fields).
// Must stay ABOVE 2 x (enabled columns), since the EC/pH loop claims 2 fields per enabled column.
const int TS3_CURRENT_FIELD  = 5;       // safe while <=2 columns are enabled  [CONFIRM if C is on]
bool  acsOk = false;                    // last ADS1115 read succeeded -- a dead ADC must be VISIBLE,
                                        // not silently read as 0 A, which is how the opto failure hid
float battIsigned = 0;                  // signed amps (050B is bidirectional); battI stays magnitude

/* ---- Timing / supervisory constants (spec sec.10.14) --------------------- */
const unsigned long UART_ACK_TIMEOUT_MS   = 3000;    // ESP2 ACK wait
const unsigned long UART_DONE_TIMEOUT_MS  = 600000;  // ESP2 sequence completion (10 min)
const uint8_t       MAX_UART_RETRY        = 3;
const unsigned long RECOVERY_COOLDOWN_MS  = 30000;
const unsigned long STARTUP_SYNC_TIMEOUT_MS = 15000;
const unsigned long HEARTBEAT_TIMEOUT_MS  = 120000;  // subsystem freshness (ESP2 silence, startup sync)
// Nano silence needs its OWN, wider window: the Nano's NIGHT heartbeat is 120s (HB_INTERVAL_NIGHT_MS),
// exactly equal to HEARTBEAT_TIMEOUT_MS -> zero margin, so any jitter or one dropped frame (~10% UART
// corruption) trips a false NANO_SILENCE + SMS every ~5 min at night. 2.5x the slowest heartbeat gives
// real margin; a genuine Nano outage still self-recovers via the Nano's own WDT (sec.18.9.5.0.1).
const unsigned long NANO_SILENCE_TIMEOUT_MS = 300000;  // 5 min = 2.5x Nano night heartbeat (120s)
// ESP2 silence is CONFIRMED, not assumed: after the freshness gap, actively probe with STATUS_REQ a few
// times before declaring ESP2_SILENCE, so a dropped-frame / starved-loop glitch doesn't force a needless reset.
const unsigned long SILENCE_PROBE_INTERVAL_MS = 2000;  // spacing between confirm probes
const uint8_t       SILENCE_PROBE_MAX          = 5;     // unanswered probes before ESP2_SILENCE (~10 s)
const unsigned long INA_READ_INTERVAL_MS  = 5000;
const unsigned long LCD_REFRESH_MS        = 500;
const unsigned long LCD_BACKLIGHT_MS      = 10000;   // button backlight timeout
const unsigned long LCD_FAULT_BACKLIGHT_MS = 60000;  // fault page stays lit 1 min, then dims
const unsigned long BTN_DEBOUNCE_MS       = 40;      // per-button debounce window
const unsigned long LOG_FLUSH_INTERVAL_MS = 5000;    // batched SD flush
const uint16_t      LOG_FLUSH_LINES       = 20;
// Hard ceiling on the RAM buffer. logFlush() returns WITHOUT clearing when it cannot take sdMux
// (the core-0 portal holds it for the whole of a /download), so a long transfer would otherwise let
// logBuf grow unbounded on the heap. Every other failure path already drops the buffer.
const size_t        LOG_BUF_MAX           = 8192;
const uint8_t       GARBAGE_LIMIT         = 5;       // consecutive (spec sec.18.9.5)
// (Nano hardware-reset layer removed -- P17 is now the master cutoff; ladder is 2 layers, sec.18.9.5.1)

/* ---- ESP2 power model: OFF during idle (power-saving, spec sec.18.8) ------ *
 * ESP2 is unpowered in IDLE_STATE. It is powered ON only for: startup validation,
 * a scheduled run (warm-up -> READY -> work order -> OFF), Testing, recovery, and
 * the last-resort HW Nano reset (sec.18.9.5.2). Warm-up bounds the boot wait.    */
const unsigned long ESP2_WARMUP_TIMEOUT_MS = 8000;   // ESP2 cold-boot -> READY budget
// Recovery ladder: try a cheap soft reset (RESET_SELF) before the relay power-cycle. This is the
// RESET_SELF -> READY budget; if ESP2 stays dark past it we escalate to esp2PowerCycle() (sec.18.9).
const unsigned long ESP2_SOFT_RESET_TIMEOUT_MS = 10000;  // ESP2 cold-boot 8s + margin for frame/stopAll
// Comm-recovery workaround (sec.18.9): power-cycling ESP2 can't fix a wedged ESP1 UART RX or a broken link,
// so cap the fast power-cycles, then latch ESP2_COMM_LOST and fall back to a gentle slow-retry (no relay
// hammering). Every attempt also re-inits ESP1's own UART (esp2ReinitUart), the one side cycling ESP2 misses.
const uint8_t       ESP2_MAX_FAST_CYCLES = 5;        // fast power-cycles in an episode before giving up
const unsigned long ESP2_SLOW_RETRY_MS   = 1200000;  // then one gentle cycle every 20 min until it recovers

/* ---- Preventive pump exercise (spec sec.14.9.1 / 19.4.2) ----------------- *
 * MOVED to ESP1: with ESP2 OFF during idle it loses millis() each power-down and
 * could never reach the 2-day mark, so ESP1 (always on, RTC) owns the schedule --
 * it powers ESP2 up on demand, sends EXERCISE,<pump>, and powers it back off.
 * A real irrigation/fertigation run also counts as exercise (resets the timer).  */
const unsigned long PUMP_EXERCISE_INTERVAL_MS = 172800000UL;  // 2 days
const unsigned long PUMP_EXERCISE_TIMEOUT_MS  = 20000;        // ESP2 boot + 5 s run + margin

/* ---- GSM live-health poll (LCD PAGE_GSM, modeled on GSM-WITH-LCD bench) ---- */
const unsigned long GSM_HEALTH_INTERVAL_MS = 15000;  // AT+CSQ/CREG/CPIN refresh

/* ---- Power logging throttle (B3, spec sec.25.2.1: not every read) --------- */
const unsigned long PWR_LOG_INTERVAL_MS = 60000;     // log a PWR snapshot ~1/min (+ on state change)

/* ---- SUMMARY command (today's SD log digest, sec.12.1.3 style; NEW kw) ----- *
 * NOTE: SUMMARY is NOT in the spec inbound catalog (sec.12.2 lists only STATUS);
 * added by explicit request. It parses today's YYYYMMDD.CSV incrementally so the
 * read never blocks the loop/WDT. Tunables below cap the SMS volume.             */
const uint8_t       SUMMARY_MAX_SMS     = 4;    // SHORT SUMMARY: cap reply length (>=1)
const uint8_t       SUMMARY_MAX_EVENTS  = 12;   // significant events listed before +more@SD
const uint16_t      SUMMARY_LINES_PER_TICK = 40;   // bounded SD lines parsed per loop (non-blocking)
// FULL SUMMARY: hourly nutrient/moisture/power record + deduped errors + events + peaks.
// Uncapped by request (full day/night); 0 = unlimited segments. A busy day can be many SMS.
const uint8_t       FULL_SUMMARY_MAX_SMS = 0;   // 0 = unlimited (user choice)
const uint8_t       FULL_MAX_EVENTS      = 60;  // RAM cap on the in-report run/dose event list
const uint8_t       SUM_MAX_ERRORS       = 12;  // distinct error codes tracked (deduped, first ts)

/* ---- Daily schedule (minutes since midnight) ----------------------------- */
const uint16_t DAILY_REPORT_MIN     = 18*60;   // 18:00 daily summary (sec.12.1.3) [TBD]
const uint16_t DAILY_NANO_RESET_MIN = 3*60;    // 03:00 fresh-start RESET_REQ (sec.18.9.5.0.3)
const uint16_t NIGHT_START_MIN      = 18*60;   // night idle begins (Nano NIGHT interval)
const uint16_t NIGHT_END_MIN        = 6*60;    // night idle ends

/* ---- Nano semantic-range limits (garbage Tier 2, spec sec.18.9.5.0) ------ */
const float TEMP_MIN = -10.0f, TEMP_MAX = 70.0f;

/* =============================================================================
 *  NUTRIENT DOSING  (companion spec Nutrient_Dosing_Firmware_Spec.md)  -- EDIT TABLES
 * -----------------------------------------------------------------------------
 *  Open-loop, gap-based: ESP32 #1 computes per-nutrient mL from the lab soil baseline,
 *  quantizes to whole flow-sensor pulses (with a floor), derives a 2x timed ceiling, and
 *  ships it in the work order. ESP32 #2 doses flow-metered and trips DOSE_TIMEOUT on a
 *  ceiling/no-flow. NPK sensor is the recorded OUTCOME, never a mid-dose input (§12.4).
 *  >>> The §3.1 gap arithmetic is transcribed literally; final unit/soil-mass basis +
 *      diluted STOCK_* values are commissioning math (one editable block).
 * ========================================================================== */
// ---- Soil baseline (mg/kg) -- F.A.S.T. Labs MC2812-6783 (§6) [MEASURE] ----
const float SOIL_BASELINE_N = 1060.0f;   // Kjeldahl
const float SOIL_BASELINE_P = 80.0f;     // elemental (from P2O5)
const float SOIL_BASELINE_K = 200.0f;    // elemental (from K2O)
// ---- Stock concentrations (mg/L) -- DILUTE at commissioning so doses clear the floor (§3.5) ----
const float STOCK_A_N = 3100.0f, STOCK_A_Ca = 3800.0f;   // CALCINIT (A) -- N driver, gap-gated (§3.3)
const float STOCK_B_P = 1338.0f, STOCK_B_N  = 590.0f;    // MAP (B) -- carries the P gap
const float STOCK_C_K = 3383.0f, STOCK_C_N  = 566.0f;    // KNO3 (C) -- carries the K gap
// ---- Pulse-quantized dosing (§3.4) ----
const int   MIN_DOSE_PULSES = 3;         // ~12 mL floor; below this -> skip + log (LOCKED)
// (pulses/mL "SCALE" comes live from the per-channel flow K-factor calKNut[i]/1000, not a constant.)
// ---- Timed-ceiling backstop (§2.4) ----
const float CEILING_MARGIN  = 2.0f;      // ceiling_s = 2x expected runtime (LOCKED)
const float PUMP_FLOWRATE_MLPM[3] = { 50.0f, 50.0f, 50.0f };   // pre-cal default mL/min [MEASURE]
// ---- Mixing tank (§1.1): batch volume is VARIABLE; MAX is a ceiling, not the formula input ----
const float MIXING_TANK_MAX_VOLUME = 50.0f;   // L (safety ceiling)
const float MIXING_TANK_SAFE_MIN   = 5.0f;    // L (refuse dosing below)
// Per-column water budget per service (must mirror ESP2's WATER_BUDGET_L) [TBD]
const float WATER_BUDGET_L[NUM_COLUMNS] = { 5.0f, 5.0f, 5.0f };

// Built-in crop presets (mg/kg, §6) -> target N,P,K + pH (pH is the EC/pH-window concern, not in §6).
struct CropPreset { const char* name; float N, P, K, pH; };
const CropPreset CROP_PRESETS[] = {
  { "PECHAY",    180,  60, 120, 6.0f },
  { "TOMATO_S1", 200,  80, 160, 6.0f },
  { "TOMATO_S2", 240, 100, 200, 6.0f },
  { "TOMATO_S3", 150,  80, 300, 6.0f },
};
const uint8_t NUM_PRESETS = sizeof(CROP_PRESETS) / sizeof(CROP_PRESETS[0]);

// Open-loop gap dose (§3): fills per-nutrient delivered mL[0..2]=A/B/C and the 2x timed ceiling
// seconds ceilS[0..2]. Quantizes to whole pulses with a floor; logs intended vs delivered + skips.
// Defined later (needs col[]/calKNut/logEvent); forward-declared here next to its tables.
void calcDose(int c, float mL[3], float ceilS[3]);

/* =============================================================================
 *  PROTOCOL FRAMING
 * ========================================================================== */
const char *FRAME_START = "<START>";
const char *FRAME_END   = "<END>";

/* =============================================================================
 *  GLOBALS
 * ========================================================================== */
LiquidCrystal_I2C lcd(LCD_ADDR, 20, 4);
RTC_DS3231        rtc;
Preferences       prefs;
Preferences       prefsCal;        // separate calibration namespace (§A.5 / §C: never reset by defaults)
SoftwareSerial    nanoSerial(NANO_RX_PIN, NANO_TX_PIN);   // EspSoftwareSerial
HardwareSerial    esp2Serial(1);
HardwareSerial    simSerial(2);

bool rtcOk = false, sdOk = false, inaOk = false;

/* ---- WDT reboot breadcrumb (survives SW_CPU_RESET in RTC RAM) -------------- *
 * g_lastStage holds a one-char marker of the loop section in progress. After a
 * watchdog panic the next boot prints it, so a blocking peripheral call (I2C/SD)
 * is identified by name instead of guessed at. Stage codes: see loop().         */
RTC_NOINIT_ATTR char     g_lastStage;
RTC_NOINIT_ATTR uint32_t g_stageMagic;
#define STAGE_MAGIC 0xC0FFEE01u

/* ---- System state machine (spec sec.10.11) ------------------------------- */
enum SystemState {
  BOOT_STATE, STARTUP_SYNC, IDLE_STATE, ACTIVE_STATE,
  RECOVERY_STATE, SAFE_MODE, EMERGENCY_STOP, TEST_MODE
};
SystemState sysState = BOOT_STATE;
const char *stateName(SystemState s) {
  switch (s) {
    case BOOT_STATE:     return "BOOT_STATE";
    case STARTUP_SYNC:   return "STARTUP_SYNC";
    case IDLE_STATE:     return "IDLE_STATE";
    case ACTIVE_STATE:   return "ACTIVE_STATE";
    case RECOVERY_STATE: return "RECOVERY_STATE";
    case SAFE_MODE:      return "SAFE_MODE";
    case TEST_MODE:      return "TEST_MODE";
    default:             return "EMERGENCY_STOP";
  }
}

/* ---- Per-column configuration (NVS-persisted) ---------------------------- */
enum ColMode { MODE_AUTO = 0, MODE_IRRIGATION_ONLY = 1 };
struct ColumnConfig {
  uint8_t mode;
  float   targetN, targetP, targetK, targetPH;
  char    name[17];
  long    lastServicedStamp;     // yyyymmdd of last service (once/day in window)
};
ColumnConfig col[NUM_COLUMNS];

/* ---- Latest Nano sensor snapshot ----------------------------------------- */
struct SensorData {
  float temp, hum;                 // ENV
  float resLevel, mixLevel, flow;  // TANK
  int   soil[NUM_COLUMNS];         // SOIL
  float lux;                       // LIGHT
  float npk[NUM_COLUMNS][7];       // NPK per column: moist,temp,EC,pH,N,P,K
  bool  npkValid[NUM_COLUMNS];
  String npkReason[NUM_COLUMNS];   // last NPK read outcome from the Nano (OK/TIMEOUT/BADADDR/BADLEN/BADCRC)
  bool  envValid, tankValid, lightValid;
  unsigned long lastNanoMs;
  // --- RAW signals retained for the Sensor Diag screen (pre-conversion, honest -1 on fault) ---
  float rawTemp, rawHum, rawLux;         // ENV / LIGHT raw (Nano sends engineering, no offset)
  float rawResCm, rawMixCm, rawFlow;     // TANK: raw ultrasonic distances (cm) + raw flow L/min
  int   rawSoil[NUM_COLUMNS][2];         // raw per-probe soil ADC (0..1023); [c][0]=probe1 [c][1]=probe2
  float rawNpk[NUM_COLUMNS][7];          // raw Modbus registers per column
  // Per-branch last-seen so a single dead Nano sensor is detectable (lastNanoMs is whole-link).
  unsigned long msEnv, msTank, msSoil, msLight, msNpk[NUM_COLUMNS];
};
SensorData sensor;

/* ---- Nano garbage tracking / recovery ------------------------------------ */
uint8_t garbageCount = 0;
long    lastResetDayStamp = -1;
bool    nanoResetReqInFlight = false;
unsigned long nanoResetReqMs = 0;

/* ---- ESP2 work-order FSM ------------------------------------------------- */
enum WoStage { WO_IDLE, WO_SENT, WO_ACKED };
struct WorkOrder {
  bool     active;
  WoStage  stage;
  int      colIdx;
  bool     fertigate;
  String   cmd;            // full framed command (for retries)
  uint8_t  retries;
  unsigned long sentMs;
};
WorkOrder wo = { false, WO_IDLE, -1, false, "", 0, 0 };
bool esp2Available = false;
unsigned long lastEsp2Ms = 0;
uint8_t       silenceProbes  = 0;    // consecutive unanswered STATUS_REQ probes (silence confirmation)
unsigned long silenceProbeMs = 0;    // last silence-probe send time
uint8_t esp2PowerCycles = 0;
unsigned long lastRecoveryMs = 0;
unsigned long esp2OffMs = 0;     // timestamp ESP2 power was cut, for the power-cycle OFF hold
const unsigned long POWER_CYCLE_OFF_MS = 3500;   // hold ESP2 relay OFF long enough for its rail to fully
                                                 // discharge -> clean power-on-reset (a short off can leave a
                                                 // half-reset ESP2 that only a manual EN clears; boot-hang fix)
// Recovery escalation ladder: soft reset (RESET_SELF) first, power-cycle as last resort (sec.18.9).
bool esp2SoftResetTried = false; // soft reset already attempted this recovery episode
unsigned long esp2RecoverMs = 0; // when RESET_SELF was sent (soft-reset READY-wait timeout reference)
uint8_t       esp2FastCycles  = 0;      // power-cycles in the current comm-failure episode (cap: sec.18.9)
bool          esp2CommLost    = false;  // latched after the fast cap; slow-retry owns recovery (not the fast loop)
unsigned long esp2SlowRetryMs = 0;      // last gentle slow-retry attempt while esp2CommLost

/* ---- ESP2 power lifecycle (OFF-during-idle model) ------------------------- *
 * esp2Powered  : current relay state mirror (avoids redundant digitalWrites + lets
 *                callers reason about power without reading the pin).
 * pendingRun   : a column run waiting for ESP2 to finish booting (warm-up phase).
 * esp2WarmupMs : start of the current warm-up wait (READY budget).                  */
bool esp2Powered = false;
struct PendingRun { bool active; int colIdx; bool fertigate; };
PendingRun pendingRun = { false, -1, false };
unsigned long esp2WarmupMs = 0;
// Minimum ESP2 on-time + periodic idle heartbeat poll (so a too-brief power-up can't leave ESP2 unable to
// boot/ACK, and ESP2 is proven alive on a cadence while idle). See esp2SetPower() + esp2PowerTick().
const unsigned long ESP2_MIN_ON_MS          = 10000;   // min continuous ON per power-up (boot+ACK+heartbeat)
const unsigned long ESP2_IDLE_POLL_DAY_MS   = 600000;  // idle heartbeat poll cadence, day  (10 min)
const unsigned long ESP2_IDLE_POLL_NIGHT_MS = 3600000; // idle heartbeat poll cadence, night (1 h)
const unsigned long ESP2_POLL_TIMEOUT_MS    = 12000;   // boot+reply window for an idle poll (> min-on)
unsigned long esp2OnMs = 0;                            // when ESP2 was powered on (min-on reference)
bool          esp2OffPending = false;                 // a graceful off is waiting out the min-on
unsigned long esp2OffAt = 0;
bool          esp2PollActive = false;                 // an idle heartbeat poll is in progress
unsigned long esp2PollStartMs = 0;
bool testArmPending = false;     // Testing: TEST,ENTER deferred until ESP2 READY (sec.18.10.8.1)
// Testing arming robustness: ESP2 is cold-booted by the GPIO4 relay on entry, so its single
// boot READY can be lost. We PRIME (let ESP2 boot) then re-send TEST,ENTER until ESP2 confirms
// with ACK,TEST,ENTER. esp2TestArmed gates the LCD + stops the retries.
bool esp2TestArmed = false;      // ESP2 has confirmed it is in TEST mode (ACK,TEST,ENTER)
unsigned long lastTestArmMs = 0; // last TEST,ENTER (re)send
uint8_t testArmTries = 0;        // sends since entering Testing (for a NO-ACK link hint)
const unsigned long TEST_PRIME_MS     = 2000;  // GPIO4 power-on -> let ESP2 boot before arming
const unsigned long TEST_ARM_RETRY_MS = 1000;  // re-send TEST,ENTER this often until ACK
const uint8_t       TEST_ARM_NOACK_HINT = 5;   // sends w/o ACK (ESP2 alive) -> suspect ESP1->ESP2 link
const uint8_t       TEST_ARM_MAX_TRIES  = 20;  // stop re-sending after this (link likely broken; no log spam)

/* ---- Preventive pump exercise (ESP1-owned; sec.14.9.1) -------------------- *
 * lastPumpUseMs[]: last time each AC pump ran (real run OR exercise). Indices:
 * 0=transfer, 1=booster, 2=mixer. pendingExercise drives the on-demand ESP2 cycle
 * exactly like pendingRun (power up -> READY -> EXERCISE,<pump> -> DONE -> power off). */
const char *EX_NAME[3] = { "TRANSFER", "BOOSTER", "MIXER" };
unsigned long lastPumpUseMs[3] = { 0, 0, 0 };
struct PendingExercise { bool active; int idx; bool sent; bool acked; };  // acked: ESP2 confirmed receipt (ACK,EXERCISE)
PendingExercise pendingExercise = { false, -1, false };

/* ---- GSM ----------------------------------------------------------------- */
bool reportPending  = false;    // deferred STATUS report (sec.12.1.3.1)
bool summaryPending = false;    // deferred SUMMARY/FULL report (same pattern as STATUS)
// Remembered request shape for a deferred summary (fired by scheduleTick when idle).
// (SumMode/long defined with the summary globals below; declared here for the pending slot.)

/* ---- GSM live health (LCD PAGE_GSM) -------------------------------------- */
int  lastRssi      = -1;        // AT+CSQ RSSI 0..31 (99/-1 = no signal)
bool netRegistered = false;     // AT+CREG stat 1 (home) or 5 (roaming)
int  lastCreg      = -1;        // AT+CREG raw stat (2=searching, 3=denied, etc.) -- logged, not just the bool
bool simReady      = false;     // AT+CPIN? == READY
unsigned long lastGsmHealthMs = 0;
uint8_t gsmHealthStep = 0;      // round-robin: 0=CSQ 1=CREG 2=CPIN

// Outbound SMS queue + non-blocking send FSM. sendSMS() only enqueues; gsmTxTick()
// drives the AT exchange with millis() timing so fault/alert paths never stall the
// loop (CLAUDE.md: non-blocking everywhere -- protects UART/flow/watchdog timing).
#define SMS_QUEUE_SIZE 8
String   smsQueue[SMS_QUEUE_SIZE];
String   smsTo[SMS_QUEUE_SIZE];        // per-message recipient (see sendSMS)
uint8_t  smsHead = 0, smsTail = 0, smsCount = 0;
// Reply target for messages generated while handling an INBOUND command: set to the
// sender's number in pollGSM so STATUS/ACK replies go back to whoever texted. Empty
// => autonomous alerts/reports go to the configured owner PHONE_NUMBER.
String   replyTarget = "";
enum GsmTx { GTX_IDLE, GTX_CMGF_WAIT, GTX_PROMPT_WAIT, GTX_BODY_SETTLE };
GsmTx    gtx = GTX_IDLE;
unsigned long gtxMs = 0;
String   gtxMsg;
String   gtxTo;                        // recipient of the in-flight outbound message
bool     gtxResultSeen = false;        // saw +CMGS/+CMS ERROR for the in-flight send (else -> TX_UNKNOWN)
const unsigned long GSM_CMGF_SETTLE_MS    = 300;
const unsigned long GSM_PROMPT_TIMEOUT_MS = 5000;
const unsigned long GSM_BODY_SETTLE_MS    = 1500;
const unsigned long GSM_SEND_RESULT_TIMEOUT_MS = 6000;  // hard cap waiting for +CMGS/+CMS ERROR after CTRL+Z

/* ---- WiFi + ThingSpeak telemetry (Part A) -------------------------------- *
 * ESP32 #1 joins the nearby WiFi and uploads a numeric snapshot to ThingSpeak,
 * which draws a live graph per field (viewable from anywhere). All network work
 * runs in a core-0 FreeRTOS task so blocking HTTP never disturbs the core-1 loop
 * or the 8 s task WDT. Credentials + write keys are set by SMS (Part B), NVS-kept.
 * Channel 1 "Columns" = per-column moisture + N,P,K. Channel 2 "System" = temp,
 * hum, lux, reservoir%, mixing%, battV, battW, flow. Channel 3 "Chem" = per-column
 * EC + pH.                                                                       */
const char *TS_HOST = "api.thingspeak.com";
const unsigned long TS_UPLOAD_IDLE_MS   = 60000;   // upload cadence when idle
const unsigned long TS_UPLOAD_ACTIVE_MS = 20000;   // faster during ACTIVE_STATE
const unsigned long WIFI_RETRY_MS       = 15000;   // reconnect backoff
const unsigned long WIFI_CONNECT_MS     = 12000;   // per-attempt connect budget

/* ---- Firebase Realtime Database bridge (live dashboard snapshot) ---------- *
 * Phase 1 of Web_Dashboard_Firebase_Plan.md: ESP32 #1 PUTs the latest verified
 * snapshot to <db>/irrigation/live.json so the website can render "now". PUT
 * replaces the node, so RTDB storage stays flat (~1 KB) no matter how long it runs.
 * History is NOT duplicated here -- ThingSpeak keeps the graphs and Supabase keeps
 * the daily CSV archive. The three are complementary, not redundant.
 *
 * Remote COMMANDS: the dashboard may request an EMERGENCY_STOP or a bounded 5 s pump
 * test. ESP32 #1 remains the single decision-making authority -- core 0 only TRANSPORTS
 * a request; core 1 validates it against the same idle gate the local UI uses and
 * dispatches through the existing pendingExercise flow. There is no remote direct
 * relay/valve/nutrient control. SMS/LCD stay the authoritative stop path, because a
 * remote e-stop silently does nothing when WiFi is down.
 *
 * SECURITY:
 *   - Every RTDB request carries "?auth=<idToken>" from a dedicated device account.
 *     Lock the RTDB rules to that device UID; test-mode rules make the token pointless.
 *   - Credentials are NVS-only (SoftAP admin form / FBASE SMS) -- never hardcoded in
 *     tracked source, same rule as the Supabase creds. The password is used ONCE at
 *     provisioning; steady state refreshes with the stored refresh token, so the
 *     password can be wiped afterwards (FBASE,SIGNOUT or the portal's Forget button). */
const unsigned long FIREBASE_UPLOAD_IDLE_MS   = 60000;  // ~1 min dashboard refresh when idle
const unsigned long FIREBASE_UPLOAD_ACTIVE_MS = 20000;  // faster during ACTIVE_STATE
// Command poll cadence. The TLS connection is REUSED between polls (fbClient/fbHttp below),
// so this is one small GET on an open socket -- not a handshake every 3 s.
const unsigned long FIREBASE_COMMAND_POLL_MS  = 3000;
/* ---- Operator FORCE run limits -------------------------------------------- *
 * A FORCE_RUN is a full irrigation/fertigation with operator-supplied volume and doses -- far more
 * consequential than the bounded 5 s pump test the other remote commands allow. These are the caps
 * that stop a typo (500 instead of 5.0) from trying to move half a tonne of water, or 5000 instead
 * of 50 from running a dosing pump to its timed ceiling. ESP2 re-checks the volume independently. */
const float FORCE_MAX_LITERS   = 20.0f;   // per request, TOTAL across the selected columns  [CONFIRM]
const float FORCE_MAX_DOSE_ML  = 500.0f;  // per nutrient, per request                        [CONFIRM]
// Columns the in-flight FORCE run is delivering into (0 = not a force run), and the dashboard
// command id to report completion against ("" = started from SMS/LCD).
uint8_t forceMask = 0;
char    forceCmdId[48] = "";
// Skip an upload below this much free heap: an mbedTLS session needs ~40-50 KB and a
// failed alloc mid-handshake is far worse than a skipped dashboard tick.
const uint32_t FIREBASE_MIN_HEAP = 60000;
// URL is NVS-backed (SoftAP admin form / FBASE SMS) -- never hardcoded in tracked source,
// same rule as the Supabase creds. Empty URL or firebaseEnabled=false disables the feature.
String   fbUrl = "";                               // e.g. "https://<ref>-default-rtdb.<region>.firebasedatabase.app"
bool     firebaseEnabled = true;                   // master switch (NVS "fben")
volatile bool fbPersistPending = false;            // core0 admin form -> core1: persist fbUrl/firebaseEnabled
volatile bool fbLastOk = false;                    // last PUT result (diagnostics page)
volatile unsigned long fbLastUploadMs = 0;         // cadence gate + "last seen" age
volatile uint32_t fbLastHeap = 0;                  // free heap observed at the last attempt
volatile int  fbLastHttp = 0;                      // last HTTP status (0 = never attempted / skipped)
volatile uint32_t fbAttempts = 0, fbFailures = 0;  // lifetime counters (logged on transition)

/* ---- Firebase Auth (device account) --------------------------------------- *
 * Provision once with email+password -> signInWithPassword returns an ID token AND a
 * refresh token. Only the refresh token is kept long-term: steady-state renewal hits
 * securetoken.googleapis.com, so the password never goes on the wire again and can be
 * erased. All NVS-backed, all netMux-guarded, never logged.                          */
String   fbEmail = "", fbPassword = "", fbApiKey = "", fbRefresh = "";
String   fbIdToken = "";                           // RAM only -- short-lived, never persisted
String   fbUid = "";                               // localId from the sign-in: THIS is what RTDB rules
                                                   // match as auth.uid. Shown in the portal so you can
                                                   // paste it into the rules; the firmware never needs it.
String   fbAuthErr = "";                           // Google's reason string on the last failure (not a secret)
unsigned long fbAuthErrMs = 0;                     // when it happened -- the portal shows the age, because
                                                   // opening the portal SUSPENDS all Firebase work, so what
                                                   // you read there is always from the previous STA session
// Core0 -> core1 handoff so auth failures reach the SD log. logEvent() is core-1 only, and
// fbNoteAuthError runs on netTask, so it cannot log directly. Fixed buffer: no String across cores.
char     fbAuthErrLog[96] = "";
volatile bool fbAuthErrLogPending = false;
bool     fbPinCa = true;                           // validate the server cert against FB_ROOT_CA (NVS "fbpin")
unsigned long fbTokenExpiryMs = 0;                 // millis() deadline for the current ID token
volatile bool fbAuthOk = false;                    // last token operation succeeded (diagnostics)
volatile int  fbAuthHttp = 0;                      // last auth HTTP status
volatile bool fbSignInPending = false;             // portal/SMS -> netTask: force a password sign-in now
volatile bool fbCredsPersistPending = false;       // core0 admin form -> core1: persist auth creds
// Auth backoff: a wrong password must NOT be retried every poll (that is credential
// hammering and Google will rate-limit the account). Same ladder shape as uploadTick().
const unsigned long FB_AUTH_BACKOFF[4] = { 30000UL, 120000UL, 480000UL, 1800000UL };
uint8_t  fbAuthFails = 0;
unsigned long fbAuthNextTryMs = 0;

/* ---- Firebase remote commands --------------------------------------------- *
 * Core 0 (netTask) only transports; core 1 validates and dispatches. FreeRTOS queues
 * carry POD structs across the core boundary -- no Strings, no shared mutable state.  */
// FORCE_RUN carries an operator-chosen batch: which columns, how many litres TOTAL, and explicit
// per-nutrient mL. POD only -- this crosses a core boundary through a FreeRTOS queue.
struct FirebaseCommand       { char id[48]; char type[32]; char pump[16];
                               char cols[4]; float liters; float doseMl[3]; };
struct FirebaseCommandStatus { char id[48]; char status[16]; char detail[48]; };
QueueHandle_t fbCommandQueue = NULL;                // core0 -> core1: requests awaiting validation
QueueHandle_t fbStatusQueue  = NULL;                // core1 -> core0: results to PATCH back
char     fbRemoteExerciseId[48] = "";               // command id owning the in-flight pump test ("" = none)
unsigned long fbLastCommandPollMs = 0;
// Server-side throttle between ACCEPTED remote actuations: a browser-side lock can be bypassed by a
// second tab or a direct RTDB write. Only a command that actually ran starts the cooldown -- see
// firebaseCommandTick() for why arming it on a rejection is worse than not throttling at all.
unsigned long fbLastRemoteActionMs = 0;
const unsigned long FB_REMOTE_MIN_GAP_MS = 10000;
volatile uint32_t fbHandshakes = 0;                 // TLS connects; the keep-alive health metric (H-1)

/* Google Trust Services Root R1 (https://pki.goog/repo/certs/gtsr1.pem, valid to 2036-06-22)
 * -- the CA behind *.googleapis.com and *.firebasedatabase.app. With this set, the Firebase
 * client VALIDATES the server certificate, so the provisioning sign-in cannot be MITM'd for
 * the device password. Blank it to fall back to setInsecure() (encrypted but unauthenticated).
 * If Google ever re-roots, TLS starts failing and PAGE_GSM shows FB:!! -- replace this PEM.  */
const char FB_ROOT_CA[] = R"EOF(-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
)EOF";

// Creds/keys live in RAM (loaded from NVS); guarded by netMux between the two cores.
String wifiSsid = "", wifiPass = "";
String tsKey1 = "", tsKey2 = "", tsKey3 = "";      // ThingSpeak write keys (ch1 cols, ch2 system, ch3 EC/pH)
SemaphoreHandle_t netMux = NULL;                   // guards the cred/key Strings across cores

// ---- Supabase Storage upload (daily CSV push; outbound HTTPS, private bucket) ----
// Bucket name is CASE-SENSITIVE and hyphenated -- exactly "CSV-Logs". A case mismatch = 404.
const char *SUPA_BUCKET = "CSV-Logs";
// URL + service_role key are NVS-only (set by SMS SUPA,... or the SoftAP admin form) -- NEVER hardcoded
// in tracked source (see the WiFi/ThingSpeak cred pattern). Guarded by netMux like the other secrets.
String   supaUrl = "", supaKey = "";               // e.g. "https://<ref>.supabase.co" + service_role key
long     supaLast = 0;                             // yyyymmdd of the last CSV successfully uploaded (NVS)
volatile bool supaPersistPending = false;          // core0 admin form -> core1: persist supaUrl/supaKey
// Reduce whatever the user pastes to the project ORIGIN (https://<host>). The firmware appends
// /storage/v1/object/CSV-Logs/<file> itself, so a pasted endpoint/signed path (e.g. .../storage/v1/
// object/sign/CSV-logs/) must not double up -- strip any path after the host + trailing slashes.
static String supaNormalizeUrl(String u) {
  u.trim();
  int scheme = u.indexOf("://");
  if (scheme >= 0) {
    int slash = u.indexOf('/', scheme + 3);        // first '/' after the host -> cut the path
    if (slash >= 0) u = u.substring(0, slash);
  }
  while (u.endsWith("/")) u = u.substring(0, u.length() - 1);
  return u;
}
// Cross-core upload hand-off: core 1 chooses the day, netTask (core 0) does the HTTPS PUT.
volatile long uploadReqStamp = 0;                  // core1 -> netTask: yyyymmdd to upload (0 = idle)
volatile bool uploadBusy     = false;              // netTask: an upload is in flight
volatile int  uploadResult   = 0;                  // netTask -> core1: 0 none / 1 ok / 2 fail
volatile long uploadResStamp = 0;                  // which day the result is for
volatile int  uploadHttp     = 0;                  // last HTTP status (for the FAIL log)
volatile long manualUploadStamp = 0;               // SMS/admin one-shot request (core 1), prioritized
volatile bool adminUploadReq    = false;           // SoftAP admin "Upload logs" button (core0 -> core1)
volatile bool wifiEnabled   = true;                // Settings > WiFi master switch (STA + telemetry); NVS-kept
volatile bool wifiConnected = false;
volatile int  wifiRssiVal   = 0;
char    wifiIpStr[16] = "0.0.0.0";                 // fixed buffer (written core 0, read core 1; no String race)
volatile bool wifiCredsChanged = false;            // SMS sets new creds -> task reconnects
volatile unsigned long lastTsUploadMs = 0;
volatile bool lastTsOk = false;
TaskHandle_t  netTaskHandle = NULL;

// ---- WiFi provisioning portal (SoftAP + captive web form, edit SSID/pass from a phone) ----
// The ESP32 briefly hosts its own WPA2 AP + a web form so creds can be set without SMS/buttons.
// Runs entirely in netTask (core 0); NVS is written by the core-1 loop (wifiPersistPending).
const char *AP_SSID = "Irrig-Setup";               // provisioning AP name
const char *AP_PASS = "irrigate123";               // WPA2 (>=8 chars) -- NOT open (documented in manual)
const unsigned long PORTAL_TIMEOUT_MS = 600000;    // auto-close after 10 min unused
volatile bool portalRequested    = false;          // core1/SMS -> netTask: start the portal
volatile bool portalActive       = false;          // netTask -> core1: AP is up (LCD banner)
volatile bool portalCancel       = false;          // core1/SMS -> netTask: tear down now
volatile bool wifiPersistPending = false;          // netTask -> core1: persist staged creds to NVS
char portalApIp[16] = "192.168.4.1";               // softAP IP, shown on LCD/SMS

// ---- Portal admin (PIN-gated: SD format/download/review + owner-number edit) ----
// Admin actions run on core 0 (portal) but touch core-1-owned resources: SD (guarded by sdMux) and
// PHONE_NUMBER (staged to core 1 via pendingOwner). PIN + owner persist in NVS on core 1.
const char *ADMIN_PIN_DEFAULT = "1234";            // change at commissioning (documented in manual)
String   adminPin = ADMIN_PIN_DEFAULT;             // NVS "apin"
volatile bool portalAdminUnlocked = false;         // set by correct PIN; cleared every portalStop()
String   pendingOwner = "";                        // core0 -> core1: new owner number to persist
volatile bool ownerPersistPending = false;
String   pendingAdminPin = "";                     // core0 -> core1: new admin PIN to persist
volatile bool adminPinPersistPending = false;
volatile bool tsKeyPersistPending = false;         // core0 -> core1: persist tsKey1-3 to NVS (set directly on core0)
SemaphoreHandle_t sdMux = NULL;                     // serializes ALL SD access across cores (core1 log/summary + core0 portal)

// ---- Portal config queue (core0 -> core1): the portal builds SMS-format command strings from its
// forms (TSKEY/SET/MODE/NAME/THRESH) and enqueues them here; the core-1 loop replays each through
// handleSms() with SMS replies muted. Reuses all existing parsing/validation; no cross-core config write.
String        portalCfgQ[6];
volatile uint8_t cfgHead = 0, cfgTail = 0;
volatile bool smsMute = false;                     // when set, sendSMS() drops (suppress ACKs for portal replays)

// Inter-core telemetry snapshot: filled on core 1 (telemetryCollect, POD only -> spinlock),
// read on core 0 by the upload task.
struct TelemetrySnapshot {
  float temp, hum, lux, resLevel, mixLevel, flow;
  int   soil[NUM_COLUMNS];
  float npkN[NUM_COLUMNS], npkP[NUM_COLUMNS], npkK[NUM_COLUMNS];
  float npkEC[NUM_COLUMNS], npkPH[NUM_COLUMNS];
  bool  npkValid[NUM_COLUMNS];
  float battV, battP, battI;   // battI added for the ACS758 ThingSpeak field (core-0 uploader)
  // Validity/state mirrored here so the core-0 uploaders never touch core-1's `sensor`/`wo`/`sysState`
  // directly (a torn read there publishes e.g. tankValid against a half-updated level).
  bool  envValid, tankValid, lightValid, inaValid;
  SystemState state;
  bool  woActive;
  bool  valid;
};
TelemetrySnapshot telem = {};
portMUX_TYPE telemMux = portMUX_INITIALIZER_UNLOCKED;
unsigned long lastTelemCollectMs = 0;

/* ---- Power (battery via opto-isolated ADC, polynomial-calibrated) --------- *
 * Battery V/I now come from GPIO35/34 through an optocoupler (nonlinear), fitted with a least-squares
 * polynomial: value = a0 + a1*u + a2*u^2 + a3*u^3, u = raw/4095. Calibrate with the bench tool
 * Test Code/ESP1_ADC_OPTO_CAL, then PASTE its printed a0..a3 into the defaults below (or populate NVS
 * namespace "adccal"). The tool's NVS record layout is mirrored so a matched-partition tool save also works. */
struct AdcCal { int degree; double coef[4]; };
// TODO: paste a0..a3 from the bench tool. Defaults are PLACEHOLDERS (raw fraction 0..1) until calibrated.
AdcCal calBattV = { 1, { 0.0, 1.0, 0.0, 0.0 } };   // voltage channel (GPIO35)
AdcCal calBattI = { 1, { 0.0, 1.0, 0.0, 0.0 } };   // current channel (GPIO34)
bool   battCalLoaded = false;                       // true once NVS "adccal" overrode the defaults
float battV = 0, battI = 0, battP = 0;
bool  batteryLow = false, batteryCritical = false;
unsigned long lastInaMs = 0;                         // ADC read throttle (kept name to avoid churn)
// Energy integration for the daily report (sec.12.1.3). The opto current is MAGNITUDE only, so all
// energy is counted as consumption (charge/discharge split removed with the INA226).
double energyConsumedWh = 0.0;   // energy today (Wh)
double energyChargedWh  = 0.0;   // kept for the report format (stays 0)
bool   prevBattStateLow = false, prevBattStateCrit = false;  // PWR-log-on-change (B3)

/* ---- Daily counters (for report) ----------------------------------------- */
uint16_t faultsToday[3] = { 0, 0, 0 };   // CRIT, MAJ, MIN
uint16_t nanoResetsToday = 0;
float    waterUsedToday[NUM_COLUMNS]    = { 0 };
float    nutrientUsedToday[NUM_COLUMNS] = { 0 };
bool     reportSentToday = false;
bool     nanoDailyResetDone = false;
long     currentDayStamp = -1;

/* ---- Logging buffer ------------------------------------------------------ */
String   logBuf;
uint16_t logLineCount = 0;
unsigned long lastLogFlushMs = 0;

/* ---- SUMMARY job: incremental today's-CSV reader (non-blocking, sec.25) --- *
 * Opens today's log once and parses a bounded number of lines per loop tick so a
 * large daily file can never stall the loop past the 8 s WDT. Accumulates counts
 * and a capped chronological list of SIGNIFICANT events, then enqueues <=3 SMS.   */
enum SummaryStage { SUM_IDLE, SUM_OPEN, SUM_READ, SUM_BUILD, SUM_STREAM };
SummaryStage summaryStage = SUM_IDLE;
enum SumMode { SUM_SHORT, SUM_FULL };      // SHORT = daily averages; FULL = hourly record
SumMode      sumMode = SUM_SHORT;
SumMode      pendingSumMode = SUM_SHORT;   // deferred-request shape (fired when idle)
long         pendingSumTarget = -1;
long         summaryTargetStamp = -1;      // yyyymmdd to open; 0 = NODATE file; resolved at request
File         summaryFile;
String       summaryReplyTo = "";          // who asked (reply target captured at request)
uint16_t     sumFltC = 0, sumFltM = 0, sumFltm = 0;   // faults by tier
uint16_t     sumRst = 0, sumFert = 0, sumIrr = 0, sumDose = 0;
uint8_t      sumEvtCount = 0;              // significant events captured
bool         sumTruncated = false;         // more events than the cap
String       sumEvents = "";               // pipe-joined compact event list (run/dose, FULL)
String       sumPartial = "";              // carry for a CSV line split across read chunks

/* ---- SUMMARY numeric accumulators (averages for SHORT, peaks shared) ------- */
double   sumNsum[NUM_COLUMNS] = {0}, sumPsum[NUM_COLUMNS] = {0}, sumKsum[NUM_COLUMNS] = {0};
uint16_t sumNPKc[NUM_COLUMNS] = {0};
double   sumMoistSum[NUM_COLUMNS] = {0}; uint16_t sumMoistC[NUM_COLUMNS] = {0};
double   sumBattVsum = 0, sumBattWsum = 0; uint32_t sumBattC = 0;
float    sumConsWh = 0, sumChgWh = 0;      // last cumulative Wh seen in PWR rows
float    sumWaterTot[NUM_COLUMNS] = {0}, sumNutTot[NUM_COLUMNS] = {0};
float    sumMaxTemp = -1000, sumMinBattV = 100000, sumPeakW = 0;

/* ---- FULL SUMMARY hourly buckets (24h x per-column) ----------------------- */
float    hrNsum[24][NUM_COLUMNS], hrPsum[24][NUM_COLUMNS], hrKsum[24][NUM_COLUMNS];
uint16_t hrNPKc[24][NUM_COLUMNS];
float    hrMoistSum[24][NUM_COLUMNS]; uint16_t hrMoistC[24][NUM_COLUMNS];
float    hrBattVsum[24], hrBattWsum[24]; uint16_t hrBattC[24];

/* ---- Error dedup (each code once, with first timestamp) ------------------- */
String   sumErrCode[SUM_MAX_ERRORS]; String sumErrTs[SUM_MAX_ERRORS]; uint8_t sumErrN = 0;

/* ---- Paced report streaming (build once, emit one segment per tick) -------- */
String   sumReport = "";                   // full assembled report text
int      sumSegIdx = 0, sumSegTotal = 0;   // segment cursor / count

/* ---- LCD UI -------------------------------------------------------------- */
enum LcdPage { PAGE_HOME, PAGE_SENSORS, PAGE_COLUMNS, PAGE_POWER, PAGE_GSM, PAGE_FAULT, PAGE_COUNT };
uint8_t   lcdPage = PAGE_HOME;
bool      backlightOn = true;
unsigned long backlightMs = 0;
unsigned long lastLcdMs = 0;
// Menu inactivity fail-safe: auto-return to the data screen (resume normal operation) if the operator
// wandered off in a menu that pauses automation. Reset on every real button press.
unsigned long lastBtnMs = 0;
const unsigned long UI_IDLE_TIMEOUT_MS = 300000;   // 5 min
String    lastFaultMsg = "none";
String    lastFaultTime = "";        // RTC timestamp of the last fault (shown on the Fault page)

/* ---- Settings / Testing UI (MODE button, spec sec.18.10) ----------------- */
enum UiMode { UI_DATA, UI_MENU, UI_EDIT, UI_TEST, UI_CAL, UI_DIAG };
UiMode    uiMode = UI_DATA;
// Top-level Settings menu rows
enum SetItem { SET_CLOCK, SET_SCHEDULE, SET_COLMODE, SET_PRESET, SET_THRESH, SET_CALIB, SET_DIAG, SET_TESTING, SET_WIFI, SET_SOFTAP, SET_RESTORE, SET_LOCK, SET_RESET, SET_EXIT, SET_COUNT };
// SET_WIFI and SET_SOFTAP show a live [ON]/[OFF] suffix via setRowLabel(); their base names here are placeholders.
const char *SET_NAMES[SET_COUNT] = { "Set Clock", "Schedule", "Column Mode", "Preset", "Thresholds", "Calibration", "Sensor Diag", "Testing", "WiFi", "Setup AP", "Restore Defaults", "Lock Screen", "Reboot ESP1", "Exit" };
int  setSel    = 0;          // selected settings row
int  editItem  = -1;         // SetItem currently being edited
int  editField = 0;          // field index within the editor
int  editCol   = 0;          // column index for per-column editors
int  editTmp[6] = { 0 };     // working copy of the field values being edited
// Edit-Confirmation (companion spec §B): a "dirty" edit shows SAVE/DISCARD/CANCEL on exit.
bool editDirty   = false;    // any value changed since the editor opened
bool editConfirm = false;    // the three-way unsaved-changes dialog is open
int  confirmSel  = 0;        // 0 SAVE, 1 DISCARD, 2 CANCEL
// Restore-Defaults (companion spec §C): destructive, double-confirm.
bool restoreConfirm = false; // the YES/NO restore dialog is open
int  restoreSel  = 0;        // 0 NO, 1 YES
// Manual ESP1 reboot (Settings > Reboot / SoftAP button): double-confirm on the LCD.
bool resetConfirm = false;   // the YES/NO reboot dialog is open
int  resetSel    = 0;        // 0 NO, 1 YES
volatile bool rebootPending = false;   // menu/portal -> core-1 loop: clean logFlush then ESP.restart()
// Remote SMS config-write deferral while a local edit is open (companion spec §B.3.1).
String pendingCfgSms = "";   // queued config SMS, applied when the local edit exits
// Testing submenu: PCF8575 OUT_* bit -> short name (MUST match ESP2 OUT_* numbering)
const char *TEST_NAMES[16] = {
  "ResValve", "Col A Vlv", "Col B Vlv", "Col C Vlv", "Mix Valve", "Inverter",
  "Transfer", "Booster", "Mixer", "Nut A", "Nut B", "Nut C", "Nut D",
  "pH Up", "pH Down", "Mast Cutoff"
};
int  testSel   = 0;          // selected component row
int  testOnBit = -1;         // which bit is currently ON (-1 = none)
// Testing rows: 16 single relays (0..15) + Fill combo (16) + one Push>Col combo per column (17+c).
// These extra indices double as the TEST,HOLD value; ESP2 treats idx>15 as a valve+pump priming combo.
const int TEST_FILL_ROW = 16;
const int TEST_ROWS = 16 + 1 + NUM_COLUMNS;        // = 20

/* ---- Calibration UI (companion spec §A) ---------------------------------- */
enum CalKind { CK_SOIL, CK_PH, CK_EC, CK_ACS, CK_LEVEL_RES, CK_LEVEL_MIX, CK_FLOW, CK_OFFSET, CK_NPK, CK_FLOW_RES };
struct CalTgt { const char *name; const char *id; uint8_t kind; int col; };
const CalTgt CAL_TGTS[] = {
  { "Soil A",      "SOIL_A",      CK_SOIL,      0 },
  { "Soil B",      "SOIL_B",      CK_SOIL,      1 },
  { "Soil C",      "SOIL_C",      CK_SOIL,      2 },
  { "pH (7 then 4)", "PH",        CK_PH,       -1 },
  { "EC (0 then std)", "EC",      CK_EC,       -1 },
  { "ACS712 zero", "ACS712",      CK_ACS,      -1 },
  { "Ultra Res empty", "ULTRA_RES", CK_LEVEL_RES, -1 },
  { "Ultra Mix empty", "ULTRA_MIX", CK_LEVEL_MIX, -1 },
  { "Flow Reservoir", "FLOW",      CK_FLOW_RES,  -1 },   // Nano fill-flow: manual run, scale cal
  { "Temp offset", "DHT_T",       CK_OFFSET,    0 },   // col selects tOff/hOff/lOff (0/1/2)
  { "Hum offset",  "DHT_H",       CK_OFFSET,    1 },
  { "Lux offset",  "LUX",         CK_OFFSET,    2 },
  { "NPK trim A",  "NPK_A",       CK_NPK,       0 },
  { "NPK trim B",  "NPK_B",       CK_NPK,       1 },
  { "NPK trim C",  "NPK_C",       CK_NPK,       2 },
  { "Flow ResMix", "FLOW_RESMIX", CK_FLOW,     -1 },
  { "Flow MixIrr", "FLOW_MIXIRR", CK_FLOW,     -1 },
  { "Flow Nut A",  "FLOW_NUTA",   CK_FLOW,      0 },
  { "Flow Nut B",  "FLOW_NUTB",   CK_FLOW,      1 },
  { "Flow Nut C",  "FLOW_NUTC",   CK_FLOW,      2 },
};
const int CAL_TGT_COUNT = sizeof(CAL_TGTS) / sizeof(CAL_TGTS[0]);
int   calSel = 0;            // selected target in the list
bool  calOpen = false;       // a target is open (streaming/capturing)
int   calStep = 0;           // capture step within the open target
float calCap[2] = { 0, 0 };  // captured raw points (or NPK N/P/K offsets being edited)
float calVolL = 0.50f;       // flow: entered reference volume (L); reused as the offset/ref entry
unsigned long calFlowPulses = 0;  // flow: captured pulse count for the current run
bool  calEnterWasDown = false;    // edge tracking for the flow dead-man
// 3-run flow K (§A.4.1.4/.7)
float calKRuns[3] = { 0, 0, 0 };
int   calRunIdx = 0;
float calNpkEdit[3] = { 0, 0, 0 };   // N/P/K offsets being edited in the NPK trim screen
const unsigned long CAL_STALE_MS = 2500;   // raw sample considered stale after this
const float CAL_OUTLIER_PCT = 8.0f;        // 3-run K disagreement tolerance [TBD]
const float EC_STD_MSCM = 1.413f;          // EC calibration standard solution [TBD]
const float CAL_MIN_SPAN_ADC = 20.0f;      // reject pH/EC cal if the two raw points are this close (degenerate)
const float NANO_FLOW_K = 450.0f;          // MUST mirror the Nano's FLOW_K_PULSES_PER_LITER (reservoir-flow scale)
String calMsg = "";                        // transient calibration message (e.g. "pts too close")

/* ---- Sensor Diag (Settings > Sensor Diag): read-only RAW viewer, all 3 controllers ------ *
 * Nano + ESP1 raw are already on-hand (SensorData raw fields / battery ADC). ESP2's sensors are
 * NOT streamed in normal operation, so the ESP2 pages power ESP2 up and CAL-stream each sensor's
 * raw value in turn (one at a time), reusing the calibration transport. Purely diagnostic.      */
uint8_t diagPage = 0;
const uint8_t DIAG_PAGES = 10;             // 0 Env,1 Tank,2 Soil,3 NPK,4 NPK chem,5 ESP1,6 ESP2 chem,7 ESP2 pwr,8 flow1,9 flow2
const unsigned long NANO_STALE_MS = 90000; // a Nano sensor is "stale" after this (~2x the active TX interval)
// ESP2 sensors are pulled live one-at-a-time via a CAL round-robin (not streamed in normal operation).
const char *DIAG_ESP2_ID[] = { "PH", "EC", "ACS712", "PZEM_V", "PZEM_I", "PZEM_P",
                               "FLOW_RESMIX", "FLOW_MIXIRR", "FLOW_NUTA", "FLOW_NUTB", "FLOW_NUTC" };
const uint8_t DIAG_ESP2_N = sizeof(DIAG_ESP2_ID) / sizeof(DIAG_ESP2_ID[0]);
float         diagEsp2Raw[DIAG_ESP2_N];
bool          diagEsp2Valid[DIAG_ESP2_N];
unsigned long diagEsp2Ms[DIAG_ESP2_N];
int           diagEsp2Idx = -1;            // index currently CAL-streaming (-1 = not streaming)
unsigned long diagEsp2DwellMs = 0;         // when the current id started streaming
bool          diagEsp2Streaming = false;   // sweep active (idle + ESP2 powered)
const unsigned long DIAG_ESP2_DWELL_MS = 900;    // per-id dwell before advancing the round-robin
const unsigned long DIAG_ESP2_STALE_MS = 16000;  // full sweep ~10 s; mark a value stale a bit beyond
// Per-probe soil raw for the Soil page comes straight from sensor.rawSoil[][] (the Nano now sends both
// probes in the normal SOIL packet) -- no CAL sweep needed for soil.

/* ---- Run progress UI ------------------------------------------------------ *
 * A service run takes over the LCD: pre-run receipt -> live stage screen (fed by ESP2 STAGE/PROG/DOSE)
 * -> post-run receipt (measured + expected nutrient increase) -> error table (only if it logged errors).
 * Receipts advance on ENTER, or auto-continue after RECEIPT_TIMEOUT_MS so an unattended scheduled run
 * never stalls waiting for a person. While locked, buttons are swallowed except the MODE+BACK e-stop
 * and UP+DOWN (which only releases the screen -- it never stops the run).                            */
enum RunPhase { RUN_NONE, RUN_PRE, RUN_LIVE, RUN_POST, RUN_ERR };
RunPhase runPhase = RUN_NONE;
struct RunUi {
  bool     fert;
  int      col;
  uint8_t  ord, total;              // stage ordinal / total, as reported by ESP2
  char     stage[18];               // stage name (LCD-friendly, from ESP2)
  float    doseTgt[3], doseGot[3];  // per-nutrient mL planned / measured
  float    ceilS[3];                // per-dose timed ceilings (s) carried in the work order
  float    waterTgt, waterGot;      // liters planned / delivered
  float    stageL, stageTgt;        // live progress within the current stage
  float    npkBefore[3], npkAfter[3];
  float    batchV;                  // batch liters the doses were computed against
  unsigned long startMs;
  bool     ok;
} run;
uint8_t       runPage = 0;                        // paging inside a receipt / the error table
unsigned long runPhaseMs = 0;                     // phase start (auto-continue timeout)
bool          runLocked = true;                   // screen takeover + buttons swallowed
const unsigned long RECEIPT_TIMEOUT_MS = 20000;   // unattended auto-continue
const uint8_t RUN_ERR_MAX = 6;
String        runErr[RUN_ERR_MAX];
uint8_t       runErrN = 0;

/* ---- Heartbeat ----------------------------------------------------------- */
bool nanoSilent = false;        // one-shot latch so silence alerts/counts fire once per outage

/* ---- Reservoir-low latch (A1: one-shot, prevents fault/SMS storm) --------- */
bool resLowLatched = false;     // raised once per low-reservoir episode; cleared on recovery

/* ---- Control-decision trace: log WHY a column is/ isn't serviced (debug the "never irrigates") --- *
 * Logged on change (or re-emitted every 10 min) so the SD log explains itself without spamming.      */
String        ctrlReason[NUM_COLUMNS];
unsigned long ctrlReasonMs[NUM_COLUMNS] = { 0 };

/* ---- Nano interval pacing (A2: ACTIVE / DAY / NIGHT, sent only on change) -- */
enum NanoPace { PACE_NONE, PACE_ACTIVE, PACE_DAY, PACE_NIGHT };
NanoPace lastNanoPace = PACE_NONE;

/* ---- LCD lock (Part D: manual stranger-lock, independent of uiMode) -------- *
 * lcdLocked gates ONLY the buttons + LCD render -- automation still runs because
 * controlTick/exerciseTick gate on uiMode (which stays UI_DATA while locked).    */
bool lcdLocked = false;

/* ---- Emergency-Off combo + recovery (Part B: MODE+BACK) ------------------- */
bool estopComboLatch = false;   // one-shot so the held combo fires E-stop once
int  estopSel = 0;              // recovery cursor: 0 = Return to normal, 1 = Stay stopped

/* ---- ESP2 fault hold + user-gated recovery (sec.19.4.8) ------------------- *
 * On a hard ESP2 fault, ESP2 drops the P17 master cutoff, PAUSES with the mix-tank
 * volume retained, and STAYS ALIVE. ESP1 keeps it powered (does NOT cut GPIO4),
 * alerts over GSM with the operation + column, and waits for the user's reply:
 *   STOP (hold/ack) | RELEASE (dump tank to its column) | IRRIGATE (water-only finish)
 *   | NORMAL (resume the paused sequence). Sent over SMS or chosen on the LCD.        */
bool esp2Held = false;          // an ESP2 hard fault is held, awaiting user recovery
int  faultRecovSel = 0;         // LCD recovery cursor: 0 Hold, 1 Release, 2 OnlyIrr, 3 Normal, 4 Cancel
// "Cancel run" is the ABORT this menu previously lacked: Hold/Release/Irrigate/Normal all try to
// CONTINUE the run, so a fault the rig cannot recover from (dead flow sensor) had no exit but a
// physical reset with the pumps still live.
const int RECOV_N = 5;
const char *RECOV_NAMES[RECOV_N] = { "Hold (wait)", "Release tank", "Only irrigate",
                                     "Resume normal", "Cancel run..." };
// Re-hold loop guard: a flow-metered resume of a dead flow sensor keeps re-holding FLOW_FAIL. After a
// few identical holds, steer the operator to the flow-independent Release/Irrigate options (ESP2 runs
// those on a timer for a flow fault) so the recovery can't loop forever.
String lastHoldCode = "";
int    sameHoldN    = 0;
bool   steerRelease = false;    // set after repeated FLOW_FAIL holds -> default cursor to Release + warn
// One-shot per column: the Nano's COLUMN_ENABLED is compile-time and is NOT distributed from ESP1
// (spec sec.9.7.1.1 has no such command), so enabling a column here that the Nano does not report
// used to make controlTick log SOIL_INVALID forever and never irrigate -- silently. Latch like
// resLowLatched so the fault alerts once per outage, not once per packet.
bool   soilMissingLatched[NUM_COLUMNS] = { false };
// Hard circuit breaker: steerRelease only moves the cursor, so an operator who keeps choosing
// "Resume normal" against dead hardware loops forever. At this many identical holds the run
// self-cancels instead of presenting the menu again.
const int HOLD_AUTOCANCEL_N = 4;

/* ---- Cancel run (operator abort) ------------------------------------------ *
 * Reachable from the held-fault menu AND from a live run. Confirms with three choices,
 * because "stop this run" almost always means one of: the column is broken (turn it off),
 * or come back shortly (snooze).                                                        */
bool cancelPrompt = false;      // the 3-way confirm is open (over the fault menu or the run UI)
int  cancelSel    = 0;          // 0 Off column, 1 Snooze 30m, 2 Snooze 10m
const char *CANCEL_NAMES[3] = { "Turn OFF column", "Snooze 30 min", "Snooze 10 min" };
// Per-column suppression window. RAM-only and millis-based on purpose: a snooze is transient and a
// reboot legitimately clears it. The persistent choice is "Turn OFF column" (COLUMN_ENABLED + NVS).
unsigned long colSnoozeUntil[NUM_COLUMNS] = { 0 };
const unsigned long SNOOZE_LONG_MS  = 30UL * 60UL * 1000UL;
const unsigned long SNOOZE_SHORT_MS = 10UL * 60UL * 1000UL;

/* ---- Device health / daily self-reset (Part C) --------------------------- *
 * bootPresent[] = devices seen at boot; a present->absent transition at runtime
 * triggers ONE self-reset per calendar day. Order: RTC, LCD, INA226, SD.        */
enum DevId { DEV_RTC = 0, DEV_LCD, DEV_INA, DEV_SD, DEV_COUNT };
bool bootPresent[DEV_COUNT] = { false, false, false, false };
unsigned long lastDevHealthMs = 0;
const unsigned long DEV_HEALTH_INTERVAL_MS = 30000;   // re-probe cadence
RTC_NOINIT_ATTR uint32_t g_lastSelfResetDay;          // yyyymmdd of last self-reset (survives SW reset)
RTC_NOINIT_ATTR uint32_t g_selfResetMagic;
#define SELFRESET_MAGIC 0x5E1F0001u

/* ---- Forward declarations ------------------------------------------------ */
void wdtSetup();
void feedWDT();
long dayStamp(const DateTime &dt);
uint16_t minuteOfDay();
String tsString();
void setState(SystemState s);
void loadConfig();
void loadCal();
void pushAllCalToEsp2();
bool setCalPushBlocking(const String &id, const String &payload);
void saveColumn(int c);
void saveSchedule(int c);
void saveColEnable(int c);
void saveThresholds();
static int clampi(int v, int lo, int hi);   // used by the THRESH SMS handler (defined near the LCD editor)
void testHoldTick();

void pollNano();
bool classifyAndApply(const String &payload, const String &raw);
void escalateNanoRecovery();
void sendNanoCommand(const char *cmd);

void pollESP2();
void sendWorkOrder(int c, bool fertigate);
void sendForceWorkOrder(uint8_t mask, float liters, const float doseMl[3], const char *fbCmdId);
void handleEsp2Response(const String &payload);
void esp2PowerCycle();
void esp2ReinitUart();
void esp2Escalate();
void esp2CommRecovered();
void esp2CommRetryTick();
void esp1SelfResetOncePerDay(const char *reason);
void rtcDeadRebootTick();
void esp2SetPower(bool on, bool force = false);
void esp2PowerTick();
void dispatchPendingRun();
void dispatchPendingExercise();
void runUiBegin(int c, bool fert);
void runUiFinish(bool ok);
void runUiTick();
void runUiAdvance();
void runUiAbort(const char *why);
void runNoteErr(const String &e);
void lcdRenderRun();
static void lcdRow(uint8_t row, const char *s);   // full-width (20-col) padded row writer
void exerciseTick();
void i2cBusRecover();

void pollGSM();
void gsmTxTick();
void gsmFeedInbound(char c);
void gsmHealthTick();
void handleSms(const String &body);
void sendSMS(const String &msg);
void sendDailyReport();
void startSummaryJob(SumMode mode, long targetStamp);
static long resolveSummaryDay(const String &arg);
void summaryTick();

void handleButtons();
void settingsButton(int i);
void settingsLeaveToData();
void uiIdleTick();
void healthTick();
void enterEditor(int item);
void commitEditor();
void restoreDefaults();
void lcdRenderSettings();
void enterCal();
void calButton(int i);
void calHoldTick();
void lcdRenderCal();
void enterDiag();
void exitDiag();
void diagTick();
void diagStopStream();
void diagButton(int i);
void lcdRenderDiag();
void sendEsp2(const String &body);
void lcdTick();
void wakeBacklight();

void stateMachineTick();
void controlTick();
bool decideFertigate(int c);
void powerTick();
void loadBattCal();
void readBatteryAdc();
void batterySafetyTick();
void scheduleTick();
void heartbeatTick();
void nanoPaceTick();
void deviceHealthTick();
static bool i2cPresent(uint8_t addr);
void saveLock();
void saveWifiEn();
void saveOwner();
void saveAdminPin();
void saveWifi();
void saveTsKey();
void saveSupa();
void saveFb();
void saveSupaLast();
bool supaUploadFile(const char *path, const char *objName);
void uploadTick();
static bool sdTake(uint32_t ms);
static void sdGive();
void telemetryCollect();
void netTask(void *pv);
static bool firebaseUploadLive();
void firebaseLogTick();
void firebaseCommandTick();
void firebaseQueueStatus(const char *id, const char *status, const char *detail);
void firebaseRemoteExerciseDone(const char *status, const char *detail);
void saveFbAuth();
static void fbResetTls();
static bool senderIsOwner();

void raiseFault(char tier, const char *code, const char *loc);
void enterEmergencyStop(bool cutPower);
void enterFaultHold(const char *code, const char *loc);
void issueRecovery(int sel);
void cancelRun(int mode, const char *why);

void logEvent(const char *source, const char *type, const String &detail);
void logFlush(bool force);

/* =============================================================================
 *  SETUP
 * ========================================================================== */
void setup() {
  Serial.begin(DEBUG_BAUD);
  delay(50);

  // ---- I2C bus recovery: release a stuck slave before Wire.begin ----
  // If the previous run crashed mid-I2C transaction, a slave can be left holding
  // SDA low and deadlock the bus (LCD 0x27 / INA 0x40 / RTC 0x68 all dead until a
  // power cycle -- the stage 'L' WDT hang). Clocking SCL frees it (see i2cBusRecover).
  i2cBusRecover();

  Serial.println(F("\n=== ESP32 #1 Master Controller boot [I2C bus recovered] ==="));

  // Reboot breadcrumb: if the last run died in the WDT, name the stuck section.
  if (g_stageMagic == STAGE_MAGIC) {
    Serial.print(F("!! Previous run rebooted while in loop stage '"));
    Serial.print(g_lastStage); Serial.println(F("' (likely the blocking call)"));
  }
  g_stageMagic = STAGE_MAGIC;
  g_lastStage  = 'S';   // 'S' = still in setup()

  // Daily self-reset stamp (Part C): validate the RTC-RAM marker; init on a cold boot to a
  // sentinel that no real yyyymmdd can equal (so the first device-loss can always reset).
  if (g_selfResetMagic != SELFRESET_MAGIC) { g_selfResetMagic = SELFRESET_MAGIC; g_lastSelfResetDay = 0xFFFFFFFFu; }

  pinMode(ESP2_PWR_PIN, OUTPUT);
  digitalWrite(ESP2_PWR_PIN, LOW);          // ESP2 powered off until STARTUP_SYNC
  esp2Powered = false;                      // OFF-during-idle power model (sec.18.8)

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_ENTER, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);

  // ---- I2C bus + devices ----
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(50);                       // bound stuck I2C xfers (no pull-ups / dead device)
                                             // so they can't block the loop past the 8s WDT
  lcd.init();
  lcd.backlight();
  backlightOn = true; backlightMs = millis();
  lcd.setCursor(0, 0); lcd.print("Smart Irrigation");
  lcd.setCursor(0, 1); lcd.print("ESP1 booting...");

  // ---- Battery ADC (opto-isolated, polynomial-calibrated) ----
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATT_I, ADC_11db);   // ~0..3.1 V full scale (clips above)
  analogSetPinAttenuation(PIN_BATT_V, ADC_11db);
  loadBattCal();                                   // NVS "adccal" overrides the hardcoded defaults if present

  // ---- RTC init ----
  rtcOk = rtc.begin();
  if (rtcOk && rtc.lostPower()) {
    // RTC lost time; logging falls back to NODATE until the clock is set.
    Serial.println(F("WARN: RTC lost power -- backup battery may be dead. Clock cannot persist across reboots."));
    Serial.println(F("ACTION: Replace DS3231 CR2032 backup battery, then power cycle the ESP32."));
  }

  // ---- microSD ----
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS);
  if (!sdOk) Serial.println(F("WARN: microSD init failed (logging degraded)"));

  // ---- Device-presence snapshot (Part C) ----
  // Record which devices answered at boot. Only a present->absent dropout at runtime
  // triggers the daily self-reset, so a never-connected device cannot cause a boot loop.
  bootPresent[DEV_RTC] = i2cPresent(0x68);
  bootPresent[DEV_LCD] = i2cPresent(LCD_ADDR);
  bootPresent[DEV_INA] = i2cPresent(INA226_ADDR);
  bootPresent[DEV_SD]  = sdOk;

  // ---- Config (NVS) ----
  prefs.begin("irrig", false);
  loadConfig();
  loadCal();                          // calibration constants (separate namespace, §A.5)

  // ---- UART links ----
  nanoSerial.begin(NANO_BAUD);
  esp2Serial.begin(ESP2_BAUD, SERIAL_8N1, ESP2_RX_PIN, ESP2_TX_PIN);
  simSerial.begin(SIM_BAUD, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);

  // ---- SIM800L AT init (proven sequence; one-time setup delays OK) ----
  delay(3000);                              // network/module stabilize
  simSerial.println("AT");                  delay(300);
  simSerial.println("AT+CMGF=1");           delay(300);   // text mode
  simSerial.println("AT+CSCS=\"GSM\"");     delay(300);
  simSerial.println("AT+CNMI=2,2,0,0,0");   delay(300);   // push +CMT on receive
  simSerial.println("AT+CSMP=17,167,0,0");  delay(300);
  while (simSerial.available()) simSerial.read();

  // ---- sensor snapshot defaults (invalid until first packet) ----
  sensor.envValid = sensor.tankValid = sensor.lightValid = false;
  for (int i = 0; i < NUM_COLUMNS; i++) { sensor.soil[i] = -1; sensor.npkValid[i] = false; sensor.npkReason[i] = "INIT"; }
  sensor.lastNanoMs = millis();
  lastEsp2Ms = millis();
  lastPumpUseMs[0] = lastPumpUseMs[1] = lastPumpUseMs[2] = millis();   // exercise clock starts at boot

  logBuf.reserve(1024);
  logEvent("SYS", "STATE", "BOOT_STATE");

  // ---- WiFi/ThingSpeak uplink task on core 0 (Part A) ----
  // Started after NVS load so creds are ready. Runs network I/O off the core-1 loop.
  netMux = xSemaphoreCreateMutex();
  sdMux  = xSemaphoreCreateMutex();          // serialize SD across cores (must exist before netTask/logFlush)
  // Firebase remote-command transport. Created BEFORE netTask so the first poll can never
  // touch a null queue. Depth 4/8: one command is in flight at a time, statuses can burst.
  fbCommandQueue = xQueueCreate(4, sizeof(FirebaseCommand));
  fbStatusQueue  = xQueueCreate(8, sizeof(FirebaseCommandStatus));
  if (!fbCommandQueue || !fbStatusQueue) Serial.println(F("[FIREBASE] command queues unavailable"));
  // 28 KB: WebServer/scan/SD stream (portal) + mbedTLS handshake frames. Two TLS consumers now share
  // this task (Supabase daily CSV, Firebase every ~60 s) -- they serialize, but the deepest frame wins.
  xTaskCreatePinnedToCore(netTask, "netTask", 28672, NULL, 1, &netTaskHandle, 0);

  wdtSetup();                               // arm task watchdog (core-1 loop only)
  setState(STARTUP_SYNC);
  Serial.println(F("Setup complete -> STARTUP_SYNC"));
}

/* =============================================================================
 *  LOOP  (non-blocking)
 * ========================================================================== */
void loop() {
  feedWDT();

  // g_lastStage breadcrumb (see globals): each marker is printed on the next boot
  // if the WDT panics here, so a blocking peripheral call is pinpointed by section.
  g_lastStage = 'N'; pollNano();
  g_lastStage = 'E'; pollESP2();
  g_lastStage = 'G'; pollGSM();
  g_lastStage = 'g'; gsmTxTick();
  g_lastStage = 'q'; gsmHealthTick();      // periodic AT+CSQ/CREG/CPIN (PAGE_GSM)

  g_lastStage = 'M'; stateMachineTick();
  g_lastStage = 'C'; controlTick();
  g_lastStage = 'X'; exerciseTick();       // preventive pump exercise (sec.14.9.1)
  g_lastStage = 'P'; powerTick();          // INA226 I2C read
  g_lastStage = 'D'; scheduleTick();
  g_lastStage = 'H'; heartbeatTick();
  g_lastStage = 'p'; esp2PowerTick();      // ESP2 min-on hold + periodic idle heartbeat poll (sec.18.8)
  g_lastStage = 'r'; esp2CommRetryTick();  // gentle 20-min retry while ESP2_COMM_LOST is latched (sec.18.9)
  g_lastStage = 'n'; nanoPaceTick();       // drive Nano ACTIVE/DAY/NIGHT interval (A2)
  g_lastStage = 'd'; deviceHealthTick();   // device-loss watchdog -> daily self-reset (Part C)
  g_lastStage = 'w'; telemetryCollect();   // snapshot for the WiFi/ThingSpeak uplink task (Part A)
  g_lastStage = 'U'; summaryTick();        // incremental SD parse for SUMMARY (bounded)

  g_lastStage = 'B'; handleButtons();
  g_lastStage = 'T'; testHoldTick();
  g_lastStage = 'C'; calHoldTick();        // flow-cal dead-man (prime pump while ENTER held)
  g_lastStage = 'D'; diagTick();           // Sensor Diag: round-robin CAL sweep of ESP2 sensors
  g_lastStage = 'U'; uiIdleTick();         // fail-safe: auto-return to data screen after 5 min idle in a menu
  g_lastStage = 'M'; healthTick();         // module-health logging: edge changes + periodic HEALTH snapshot
  g_lastStage = 'Z'; rtcDeadRebootTick();  // dead-RTC (all-zero timestamp): daily idle-only reboot to recover
  g_lastStage = 'R'; runUiTick();          // run receipts: unattended auto-continue past PRE/POST/ERR
  g_lastStage = 'X'; uploadTick();         // Supabase CSV push scheduling (rollover + reconnect catch-up)
  g_lastStage = 'F'; firebaseLogTick();    // Firebase health: log ok<->fail transitions (core-0 does the PUT)
  g_lastStage = 'k'; firebaseCommandTick();// remote commands: validated + dispatched HERE, on the control core
  // Apply a config SMS that was deferred during a local edit, once the edit has closed (§B.3.1).
  if (pendingCfgSms.length() && uiMode != UI_EDIT && !editConfirm) {
    String s = pendingCfgSms; pendingCfgSms = ""; handleSms(s);
  }
  // Persist WiFi creds saved by the core-0 portal (all NVS writes stay on core 1).
  if (wifiPersistPending) { wifiPersistPending = false; saveWifi(); saveWifiEn(); logEvent("ESP1", "CFG", "WIFI|PORTAL"); }
  // Apply portal-staged owner number / admin PIN on core 1 (PHONE_NUMBER is read here by senderIsOwner/
  // sendSMS, so the write must happen on this core, not core 0).
  if (ownerPersistPending) {
    ownerPersistPending = false;
    xSemaphoreTake(netMux, portMAX_DELAY); PHONE_NUMBER = pendingOwner; xSemaphoreGive(netMux);
    saveOwner(); logEvent("ESP1", "CFG", "OWNER|PORTAL");
  }
  if (adminPinPersistPending) {
    adminPinPersistPending = false;
    xSemaphoreTake(netMux, portMAX_DELAY); adminPin = pendingAdminPin; xSemaphoreGive(netMux);
    saveAdminPin(); logEvent("ESP1", "CFG", "ADMINPIN|PORTAL");
  }
  // Persist ThingSpeak keys set directly on core 0 by the portal (tsKey1-3 already updated under netMux).
  if (tsKeyPersistPending) { tsKeyPersistPending = false; saveTsKey(); logEvent("ESP1", "CFG", "TSKEY|PORTAL"); }
  // Persist Supabase URL+key set by the SoftAP admin form (already updated under netMux). Never logs the key.
  if (supaPersistPending) { supaPersistPending = false; saveSupa(); logEvent("ESP1", "CFG", "SUPA|PORTAL"); }
  // Persist Firebase URL + enable flag set by the SoftAP admin form (already updated under netMux).
  if (fbPersistPending) { fbPersistPending = false; saveFb(); logEvent("ESP1", "CFG", "FBASE|PORTAL"); }
  // Persist Firebase auth creds. Also fires after netTask mints a refresh token on core 0, so the
  // device survives a reboot without needing the password again. Never logs the creds themselves.
  if (fbCredsPersistPending) { fbCredsPersistPending = false; saveFbAuth(); logEvent("ESP1", "CFG", "FBASE|AUTH"); }
  // Manual reboot requested (Settings > Reboot or the SoftAP button). Always run on core 1 so logs flush
  // cleanly (config persists in NVS; ESP2 keeps executing any in-flight run and re-syncs on our return).
  if (rebootPending) {
    rebootPending = false;
    logEvent("ESP1", "RESET", "MANUAL|REBOOT");
    logFlush(true); delay(60);
    ESP.restart();
  }
  // Drain one portal config command (SET/MODE/NAME/THRESH) through handleSms with SMS muted. Allowed in
  // UI_DATA *and* UI_MENU: the portal is often launched from the Settings menu, which leaves uiMode==UI_MENU
  // the whole time it runs -- gating on UI_DATA alone meant those edits never applied. Still never drains
  // during an actual editor (UI_EDIT/editConfirm); handleSms re-defers SET/MODE/NAME if one opens anyway.
  if (cfgHead != cfgTail && (uiMode == UI_DATA || uiMode == UI_MENU) && !editConfirm) {
    // Pop under netMux (barrier + mutual exclusion with the core-0 enqueue), then RELEASE before
    // handleSms -- handleSms/sendSMS take netMux themselves, so holding it across would deadlock.
    xSemaphoreTake(netMux, portMAX_DELAY);
    String cmd = portalCfgQ[cfgHead]; cfgHead = (uint8_t)((cfgHead + 1) % 6);
    xSemaphoreGive(netMux);
    logEvent("ESP1", "CFG", "PORTAL|" + cmd);
    smsMute = true; handleSms(cmd); smsMute = false;
  }
  g_lastStage = 'L'; lcdTick();            // LCD I2C writes
  g_lastStage = 'F'; logFlush(false);      // microSD (SPI) write
  g_lastStage = '.';                       // idle: loop completed cleanly
}

/* =============================================================================
 *  WATCHDOG
 * ========================================================================== */
void wdtSetup() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = { .timeout_ms = 8000, .idle_core_mask = 0, .trigger_panic = true };
  esp_task_wdt_deinit();
  esp_task_wdt_init(&cfg);
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(8, true);
  esp_task_wdt_add(NULL);
#endif
}
void feedWDT() { esp_task_wdt_reset(); }

/* =============================================================================
 *  I2C BUS RECOVERY  (release a slave stuck holding SDA low)
 * -----------------------------------------------------------------------------
 *  A slave reset mid-byte can hold SDA low forever, wedging the bus so the next
 *  Wire transaction blocks the loop into the 8 s WDT (the diagnosed stage 'L'
 *  hang -- LCD 0x27 and RTC 0x68 both dead on one bus). Bit-banging up to 9 SCL
 *  pulses lets the slave finish its byte and release SDA, then we issue a STOP.
 *  Called once before Wire.begin(); cheap, one-time, setup-only.
 * ========================================================================== */
void i2cBusRecover() {
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delay(5);
  if (digitalRead(I2C_SDA) == LOW) {              // a slave is holding the bus
    pinMode(I2C_SCL, OUTPUT);
    for (int i = 0; i < 9 && digitalRead(I2C_SDA) == LOW; i++) {
      digitalWrite(I2C_SCL, LOW);  delayMicroseconds(5);
      digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5);   // clock out one bit
    }
    // Generate a STOP: SDA low->high while SCL high.
    pinMode(I2C_SDA, OUTPUT);
    digitalWrite(I2C_SDA, LOW);  delayMicroseconds(5);
    digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5);
    digitalWrite(I2C_SDA, HIGH); delayMicroseconds(5);
  }
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delay(5);
}

/* =============================================================================
 *  TIME / UTIL
 * ========================================================================== */
long dayStamp(const DateTime &dt) {
  return (long)dt.year() * 10000L + (long)dt.month() * 100L + (long)dt.day();
}
uint16_t minuteOfDay() {
  if (!rtcOk) return 0;
  DateTime now = rtc.now();
  return now.hour() * 60 + now.minute();
}
String tsString() {
  if (!rtcOk) return String("0000-00-00 00:00:00");
  DateTime n = rtc.now();
  char b[20];
  snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d",
           n.year(), n.month(), n.day(), n.hour(), n.minute(), n.second());
  return String(b);
}
void setState(SystemState s) {
  if (s == sysState) return;
  String d = String(stateName(sysState)) + "->" + stateName(s);
  sysState = s;
  logEvent("ESP1", "STATE", d);
  wakeBacklight();
}

/* =============================================================================
 *  CONFIG (NVS)
 * ========================================================================== */
void loadConfig() {
  for (int c = 0; c < NUM_COLUMNS; c++) {
    char key[8];
    snprintf(key, sizeof(key), "c%d", c);
    col[c].mode    = prefs.getUChar((String(key) + "m").c_str(), MODE_AUTO);
    col[c].targetN = prefs.getFloat((String(key) + "N").c_str(), 0.0f);
    col[c].targetP = prefs.getFloat((String(key) + "P").c_str(), 0.0f);
    col[c].targetK = prefs.getFloat((String(key) + "K").c_str(), 0.0f);
    col[c].targetPH = prefs.getFloat((String(key) + "H").c_str(), 6.0f);
    String nm = prefs.getString((String(key) + "n").c_str(), "");
    strncpy(col[c].name, nm.c_str(), sizeof(col[c].name) - 1);
    col[c].name[sizeof(col[c].name) - 1] = 0;
    col[c].lastServicedStamp = -1;
    // per-column schedule (manual window + AUTO/MANUAL axis) and enable flag
    COL_WIN_START[c] = prefs.getUShort((String(key) + "ws").c_str(), COL_WIN_START[c]);
    COL_WIN_END[c]   = prefs.getUShort((String(key) + "we").c_str(), COL_WIN_END[c]);
    colSchedMode[c]  = prefs.getUChar((String(key) + "sm").c_str(), colSchedMode[c]);
    COLUMN_ENABLED[c] = prefs.getBool((String(key) + "en").c_str(), COLUMN_ENABLED[c]);
  }
  // global thresholds (settable on-device, sec.10.10.1)
  soilStartPct = prefs.getInt("sstart", soilStartPct);
  soilStopPct  = prefs.getInt("sstop",  soilStopPct);
  fertGap      = prefs.getFloat("fgap", fertGap);
  lcdLocked    = prefs.getBool("lock", false);   // LCD lock persists across reboot (Part D)
  // WiFi + ThingSpeak (Part A/B): creds + write keys persist in NVS.
  wifiEnabled = prefs.getBool("wifien", true);   // WiFi master switch (Settings > WiFi)
  PHONE_NUMBER = prefs.getString("owner", PHONE_NUMBER);   // owner number (portal-editable; SMS gating)
  adminPin     = prefs.getString("apin", ADMIN_PIN_DEFAULT); // portal admin PIN
  wifiSsid = prefs.getString("wssid", "");
  wifiPass = prefs.getString("wpass", "");
  tsKey1   = prefs.getString("tsk1", "");
  tsKey2   = prefs.getString("tsk2", "");
  tsKey3   = prefs.getString("tsk3", "");
  supaUrl  = prefs.getString("supaurl", "");        // Supabase project URL + service_role key (NVS-only)
  supaKey  = prefs.getString("supakey", "");
  supaLast = prefs.getLong("supalast", 0);          // last day (yyyymmdd) successfully uploaded
  fbUrl    = prefs.getString("fburl", "");          // Firebase RTDB base URL (NVS-only, like the Supabase creds)
  firebaseEnabled = prefs.getBool("fben", true);    // master switch; URL still has to be set for it to run
  fbEmail    = prefs.getString("fbmail", "");       // device-account creds (NVS-only, never in tracked source)
  fbPassword = prefs.getString("fbpass", "");       // wiped once a refresh token exists
  fbApiKey   = prefs.getString("fbkey",  "");       // Firebase Web API key
  fbRefresh  = prefs.getString("fbref",  "");       // long-lived; this is what keeps us signed in
  fbPinCa    = prefs.getBool("fbpin", true);        // cert validation on by default
}
/* =============================================================================
 *  CALIBRATION NVS + DISTRIBUTION  (companion spec §A.5.1)
 * ========================================================================== */
void loadCal() {
  prefsCal.begin("calib", false);                 // separate namespace; Restore-Defaults never touches it
  for (int c = 0; c < NUM_COLUMNS; c++) {
    String k = "s" + String(c);
    // "a2"/"w2" keys (bumped from "a"/"w"): a stale pre-remap on-device soil cal no longer overrides the
    // measured compiled defaults -- the correct endpoints apply on flash until a fresh cal is saved.
    calSoilAir[c]   = prefsCal.getInt((k + "a2").c_str(), calSoilAir[c]);
    calSoilWater[c] = prefsCal.getInt((k + "w2").c_str(), calSoilWater[c]);
    calNpkOff[c][0] = prefsCal.getFloat((k + "N").c_str(), 0);
    calNpkOff[c][1] = prefsCal.getFloat((k + "P").c_str(), 0);
    calNpkOff[c][2] = prefsCal.getFloat((k + "K").c_str(), 0);
  }
  calResEmptyCm = prefsCal.getFloat("reCm", calResEmptyCm);
  calResFullCm  = prefsCal.getFloat("rfCm", calResFullCm);
  calMixEmptyCm = prefsCal.getFloat("meCm", calMixEmptyCm);
  calMixFullCm  = prefsCal.getFloat("mfCm", calMixFullCm);
  calFlowResScale = prefsCal.getFloat("frS", calFlowResScale);
  calTempOff = prefsCal.getFloat("tOff", 0);
  calHumOff  = prefsCal.getFloat("hOff", 0);
  calLuxOff  = prefsCal.getFloat("lOff", 0);
  calKResMix = prefsCal.getFloat("kRM", calKResMix);
  calKMixIrr = prefsCal.getFloat("kMI", calKMixIrr);
  calKNut[0] = prefsCal.getFloat("kNA", calKNut[0]);
  calKNut[1] = prefsCal.getFloat("kNB", calKNut[1]);
  calKNut[2] = prefsCal.getFloat("kNC", calKNut[2]);
  calKNutD   = prefsCal.getFloat("kND", calKNutD);
  calKPhUp   = prefsCal.getFloat("kPU", calKPhUp);
  calKPhDn   = prefsCal.getFloat("kPD", calKPhDn);
  calPhM = prefsCal.getFloat("phM", calPhM); calPhB = prefsCal.getFloat("phB", calPhB);
  calEcM = prefsCal.getFloat("ecM", calEcM); calEcB = prefsCal.getFloat("ecB", calEcB);
  calAcs712Zero = prefsCal.getFloat("acs", calAcs712Zero);
}

// Push every ESP2-owned calibration constant (baseline after any ESP2 reset, §A.5.1 #2).
// Best-effort fire (not block-until-ACK); the work order re-carries job-critical values anyway.
void pushAllCalToEsp2() {
  sendEsp2("SET_CAL,FLOW_RESMIX," + String(calKResMix, 1));
  sendEsp2("SET_CAL,FLOW_MIXIRR," + String(calKMixIrr, 1));
  sendEsp2("SET_CAL,FLOW_NUTA," + String(calKNut[0], 1));
  sendEsp2("SET_CAL,FLOW_NUTB," + String(calKNut[1], 1));
  sendEsp2("SET_CAL,FLOW_NUTC," + String(calKNut[2], 1));
  sendEsp2("SET_CAL,ACS712," + String(calAcs712Zero, 4));
  sendEsp2("SET_CAL,PH," + String(calPhM, 6) + "," + String(calPhB, 4));
  sendEsp2("SET_CAL,EC," + String(calEcM, 6) + "," + String(calEcB, 4));
}

// Save-time push with block-until-ACK (§A.5.1 #1): returns false if ESP2 never confirms.
bool setCalPushBlocking(const String &id, const String &payload) {
  lastSetCalAck = "";
  sendEsp2("SET_CAL," + id + "," + payload);
  unsigned long t0 = millis();
  while (millis() - t0 < 2500) {                  // bounded; runs in the idle calibration UI
    feedWDT();
    pollESP2();
    if (lastSetCalAck == String("SET_CAL,") + id) return true;
  }
  return false;
}

void saveLock() {
  prefs.putBool("lock", lcdLocked);
}
void saveWifiEn() {
  prefs.putBool("wifien", wifiEnabled);
}
void saveOwner()    { prefs.putString("owner", PHONE_NUMBER); }   // owner number (portal edit)
void saveAdminPin() { prefs.putString("apin", adminPin); }        // portal admin PIN
void saveSupaLast() { prefs.putLong("supalast", supaLast); }      // last uploaded day (yyyymmdd)
// Supabase creds under netMux (the core-0 admin form may write them), NVS write from locals.
void saveSupa() {
  String u, k; xSemaphoreTake(netMux, portMAX_DELAY); u = supaUrl; k = supaKey; xSemaphoreGive(netMux);
  prefs.putString("supaurl", u); prefs.putString("supakey", k);
}
// Firebase RTDB URL + enable flag under netMux (the core-0 admin form may write them), NVS write from locals.
void saveFb() {
  String u; bool en;
  xSemaphoreTake(netMux, portMAX_DELAY); u = fbUrl; en = firebaseEnabled; xSemaphoreGive(netMux);
  prefs.putString("fburl", u); prefs.putBool("fben", en);
}
// Firebase device-account creds. The refresh token is the long-lived secret; the password is only
// needed until the first successful sign-in and can then be wiped (FBASE,SIGNOUT / portal Forget).
// The ID token is deliberately NOT persisted -- it expires in an hour and is cheap to re-mint.
void saveFbAuth() {
  String e, p, k, r;
  xSemaphoreTake(netMux, portMAX_DELAY);
  e = fbEmail; p = fbPassword; k = fbApiKey; r = fbRefresh;
  xSemaphoreGive(netMux);
  prefs.putString("fbmail", e); prefs.putString("fbpass", p);
  prefs.putString("fbkey",  k); prefs.putString("fbref",  r);
  prefs.putBool("fbpin", fbPinCa);
}
// Copy the cred Strings under netMux (the core-0 portal may write them), then write NVS from the locals
// so the putString can't torn-read a String mid-write.
void saveWifi() {
  String ss, pw; xSemaphoreTake(netMux, portMAX_DELAY); ss = wifiSsid; pw = wifiPass; xSemaphoreGive(netMux);
  prefs.putString("wssid", ss);
  prefs.putString("wpass", pw);
}
void saveTsKey() {
  String k1, k2, k3; xSemaphoreTake(netMux, portMAX_DELAY); k1 = tsKey1; k2 = tsKey2; k3 = tsKey3; xSemaphoreGive(netMux);
  prefs.putString("tsk1", k1);
  prefs.putString("tsk2", k2);
  prefs.putString("tsk3", k3);
}
void saveColumn(int c) {
  char key[8];
  snprintf(key, sizeof(key), "c%d", c);
  prefs.putUChar((String(key) + "m").c_str(), col[c].mode);
  prefs.putFloat((String(key) + "N").c_str(), col[c].targetN);
  prefs.putFloat((String(key) + "P").c_str(), col[c].targetP);
  prefs.putFloat((String(key) + "K").c_str(), col[c].targetK);
  prefs.putFloat((String(key) + "H").c_str(), col[c].targetPH);
  prefs.putString((String(key) + "n").c_str(), col[c].name);
}
void saveSchedule(int c) {
  char key[8];
  snprintf(key, sizeof(key), "c%d", c);
  prefs.putUShort((String(key) + "ws").c_str(), COL_WIN_START[c]);
  prefs.putUShort((String(key) + "we").c_str(), COL_WIN_END[c]);
  prefs.putUChar((String(key) + "sm").c_str(), colSchedMode[c]);
}
void saveColEnable(int c) {
  char key[8];
  snprintf(key, sizeof(key), "c%d", c);
  prefs.putBool((String(key) + "en").c_str(), COLUMN_ENABLED[c]);
}
void saveThresholds() {
  prefs.putInt("sstart", soilStartPct);
  prefs.putInt("sstop",  soilStopPct);
  prefs.putFloat("fgap", fertGap);
}

/* =============================================================================
 *  NANO LINK  --  framed parse, garbage classification (spec sec.18.9.5.0)
 * ========================================================================== */
void pollNano() {
  static String line;
  while (nanoSerial.available()) {
    char c = (char)nanoSerial.read();
    if (c == '\n' || c == '\r') {
      if (line.length() > 0) {
        String raw = line;
        line = "";
        // structural: framing
        int s = raw.indexOf(FRAME_START);
        int e = raw.indexOf(FRAME_END);
        bool clean = false;
        if (s >= 0 && e > s && raw.length() <= 128) {
          String payload = raw.substring(s + 7, e);   // between markers
          payload.trim();
          if (payload.startsWith(",")) payload = payload.substring(1);
          if (payload.endsWith(","))   payload = payload.substring(0, payload.length() - 1);
          clean = classifyAndApply(payload, raw);
        }
        if (clean) {
          garbageCount = 0;
          nanoResetReqInFlight = false;   // valid data resumed: software reset path is clear again
          nanoSilent = false;             // fresh data clears the silence one-shot
        } else {
          if (garbageCount < 255) garbageCount++;
          // Throttle: a mis-flashed Nano (unframed debug text) can stream junk
          // faster than the SD/Serial can drain it. Log at most ~1 line/sec so the
          // logging path can't back-pressure and stall the loop into the WDT.
          static unsigned long lastGarbageLogMs = 0;
          if (millis() - lastGarbageLogMs > 1000) {
            lastGarbageLogMs = millis();
            String d = "COUNT=" + String(garbageCount) + "|RAW=" + raw.substring(0, 64);
            logEvent("NANO", "GARBAGE", d);
          }
          if (garbageCount >= GARBAGE_LIMIT) escalateNanoRecovery();
        }
      }
    } else if (line.length() < 140) {
      line += c;
    } else {
      line = "";   // overflow -> drop
    }
  }
}

// A field must actually be a number before we believe it. Guards against a spliced frame handing
// the parser a token like "ENV" or "<START>", which String::toFloat() silently turns into 0.0.
static bool isNumericToken(const String &s) {
  if (s.length() == 0) return false;
  int i = 0, digits = 0, dots = 0;
  if (s[0] == '-' || s[0] == '+') i = 1;
  for (; i < (int)s.length(); i++) {
    if (isDigit(s[i])) digits++;
    else if (s[i] == '.') { if (++dots > 1) return false; }
    else return false;
  }
  return digits > 0;
}

// Returns true if packet is clean+plausible (and applies it); false = garbage.
bool classifyAndApply(const String &payload, const String &raw) {
  // STRUCTURAL (Tier 1): the payload is taken from BETWEEN the frame markers, so a marker inside it
  // means two packets were spliced -- ESP1's SoftwareSerial RX dropped the bytes that separated them.
  // Such a line can still present the right comma count and a valid column tag, so it must be
  // rejected here or it is indistinguishable from real data downstream (spec sec.9.9.5/9.9.6).
  if (payload.indexOf(FRAME_START) >= 0 || payload.indexOf(FRAME_END) >= 0) return false;

  // tokenize by comma
  const int MAXT = 12;
  String tok[MAXT];
  int n = 0, start = 0;
  for (int i = 0; i <= payload.length() && n < MAXT; i++) {
    if (i == payload.length() || payload[i] == ',') {
      tok[n++] = payload.substring(start, i);
      start = i + 1;
    }
  }
  if (n == 0) return false;
  String cmd = tok[0];

  if (cmd == "CAL" && n == 3) {               // live raw sample from the Nano (§A.3): id,raw
    calRxId = tok[1]; calRxRaw = tok[2].toFloat();
    calRxValid = (tok[2] != "-1"); calRxMs = millis();
    sensor.lastNanoMs = millis();
    return true;                              // expected calibration traffic, not garbage
  }
  if (cmd == "ENV" && n == 3) {
    float t = tok[1].toFloat(), h = tok[2].toFloat();
    bool tInv = (tok[1] == "-1"), hInv = (tok[2] == "-1");
    if (!tInv && (t < TEMP_MIN || t > TEMP_MAX)) return false;   // Tier 2 (raw range)
    if (!hInv && (h < 0 || h > 100)) return false;
    sensor.temp = tInv ? t : t + calTempOff;     // apply calibration offset (§A)
    sensor.hum  = hInv ? h : h + calHumOff;
    sensor.envValid = !(tInv || hInv);
    sensor.rawTemp = t; sensor.rawHum = h; sensor.msEnv = millis();   // Sensor Diag: raw + last-seen
    sensor.lastNanoMs = millis();
    logEvent("NANO", "SENSOR", "ENV|" + tok[1] + "|" + tok[2]);
    return true;
  }
  if (cmd == "TANK" && n == 4) {
    // RAW distances in cm (companion spec §A); ESP1 maps to % with the stored geometry.
    float rd = tok[1].toFloat(), md = tok[2].toFloat();
    bool rInv = (tok[1] == "-1"), mInv = (tok[2] == "-1");
    if (!rInv && (rd < 0 || rd > 500)) return false;   // raw distance plausibility (Tier 2)
    if (!mInv && (md < 0 || md > 500)) return false;
    sensor.resLevel = rInv ? -1 : levelPct(rd, calResEmptyCm, calResFullCm);
    sensor.mixLevel = mInv ? -1 : levelPct(md, calMixEmptyCm, calMixFullCm);
    sensor.flow = tok[3].toFloat() * calFlowResScale;
    sensor.tankValid = !(rInv || mInv);
    sensor.rawResCm = rInv ? -1 : rd; sensor.rawMixCm = mInv ? -1 : md;   // Sensor Diag: raw cm + L/min
    sensor.rawFlow = tok[3].toFloat(); sensor.msTank = millis();
    // A1: clear the reservoir-low latch once the level recovers (with hysteresis).
    if (!rInv && sensor.resLevel >= RES_LOW_PCT + 5.0f) resLowLatched = false;
    sensor.lastNanoMs = millis();
    logEvent("NANO", "SENSOR", "TANK|" + tok[1] + "|" + tok[2] + "|" + tok[3]);
    return true;
  }
  if (cmd == "SOIL") {
    // Tag-based, variable-length: SOIL,<tag>,<probe1>,<probe2>,... (enabled columns only; disabled omitted).
    // Both raw capacitive probes per column; ESP1 averages them for control + maps to %, and logs each raw.
    if (n < 4 || ((n - 1) % 3) != 0) return false;     // need >=1 triplet, (tag,p1,p2) groups (Tier 1)
    int tmp[NUM_COLUMNS], rawTmp[NUM_COLUMNS][2];
    for (int c = 0; c < NUM_COLUMNS; c++) { tmp[c] = -1; rawTmp[c][0] = -1; rawTmp[c][1] = -1; }  // absent stay -1
    for (int i = 1; i + 2 < n; i += 3) {
      int c = -1;
      for (int j = 0; j < NUM_COLUMNS; j++) if (tok[i] == String(COL_TAG[j])) c = j;
      if (c < 0) return false;                          // unknown tag = Tier 1 garbage
      int v1 = tok[i + 1].toInt(), v2 = tok[i + 2].toInt();   // RAW per-probe ADC (companion spec §A)
      if (v1 < 0 || v1 > 1023 || v2 < 0 || v2 > 1023) return false;   // Tier 2: 10-bit ADC range
      rawTmp[c][0] = v1; rawTmp[c][1] = v2;              // keep each raw probe (log + Sensor Diag)
      tmp[c] = soilCombine(c, v1, v2);                  // 2-probe combine (drops a railed probe) -> %
      if (abs(v1 - v2) > 400) logEvent("NANO", "SOIL_DIVERGE", String(COL_TAG[c]) + "|" + v1 + "|" + v2);
    }
    for (int c = 0; c < NUM_COLUMNS; c++) {
      sensor.soil[c] = tmp[c];
      sensor.rawSoil[c][0] = rawTmp[c][0]; sensor.rawSoil[c][1] = rawTmp[c][1];
      // A column ESP1 has ENABLED but the Nano did not report. Without this the column just logs
      // SOIL_INVALID in controlTick and never irrigates, with nothing raised to the operator --
      // the failure mode when the two COLUMN_ENABLED tables disagree (see the latch declaration).
      if (COLUMN_ENABLED[c] && tmp[c] < 0) {
        if (!soilMissingLatched[c]) {
          soilMissingLatched[c] = true;
          raiseFault('M', "SOIL_MISSING", c == 0 ? "COL_A" : c == 1 ? "COL_B" : "COL_C");
        }
      } else if (tmp[c] >= 0) soilMissingLatched[c] = false;      // reporting again -> re-arm
    }
    sensor.msSoil = millis();
    sensor.lastNanoMs = millis();
    // Log the column % (X=<pct>, read by the summary parser) PLUS each raw probe (X1/X2) for debugging;
    // pipe-delimited/CSV-safe (sec.25.2.1). Disabled columns -> DISABLED (distinguishes off vs 0/fault).
    String d = "SOIL";
    for (int c = 0; c < NUM_COLUMNS; c++) {
      d += "|" + String(COL_TAG[c]) + "=";
      if (!COLUMN_ENABLED[c]) { d += "DISABLED"; continue; }
      d += String(tmp[c]);
      d += "|" + String(COL_TAG[c]) + "1=" + String(rawTmp[c][0]);
      d += "|" + String(COL_TAG[c]) + "2=" + String(rawTmp[c][1]);
    }
    logEvent("NANO", "SENSOR", d);
    return true;
  }
  if (cmd == "LIGHT" && n == 2) {
    float l = tok[1].toFloat();
    if (tok[1] != "-1" && l < 0) return false;
    sensor.lux = (tok[1] == "-1") ? l : l + calLuxOff;   // apply calibration offset (§A)
    sensor.lightValid = (tok[1] != "-1");
    sensor.rawLux = l; sensor.msLight = millis();        // Sensor Diag: raw lux + last-seen
    sensor.lastNanoMs = millis();
    logEvent("NANO", "SENSOR", "LIGHT|" + tok[1]);
    return true;
  }
  if (cmd == "NPK" && n == 10) {          // tag + 7 values + trailing reason token (companion spec §A)
    int c = -1;
    for (int i = 0; i < NUM_COLUMNS; i++) if (tok[1] == String(COL_TAG[i])) c = i;
    if (c < 0) return false;
    // SEMANTIC (Tier 2): NPK was the only packet with no value validation -- ENV/TANK/SOIL/LIGHT all
    // range-check. A corrupted field therefore became plausible-looking nutrient data (e.g. a spliced
    // ENV packet logging temperature as phosphorus), which then drove decideFertigate(). Registers are
    // unsigned 16-bit, so anything outside 0..65535 (or non-numeric) is impossible.
    for (int i = 0; i < 7; i++) {
      const String &v = tok[i + 2];
      if (v == "-1") continue;                    // honest invalid-read sentinel, NOT garbage
      if (!isNumericToken(v)) return false;
      float f = v.toFloat();
      if (f < 0.0f || f > 65535.0f) return false;
    }
    bool anyInvalid = false;
    for (int i = 0; i < 7; i++) {                 // RAW registers (companion spec §A): apply scale + offset
      if (tok[i + 2] == "-1") { anyInvalid = true; sensor.npk[c][i] = -1; sensor.rawNpk[c][i] = -1; continue; }
      sensor.rawNpk[c][i] = tok[i + 2].toFloat(); // Sensor Diag: keep the raw Modbus register
      float val = tok[i + 2].toFloat() / calNpkScale[i];
      if (i >= 4) val += calNpkOff[c][i - 4];     // N/P/K offset trim (i=4/5/6)
      sensor.npk[c][i] = val;
    }
    sensor.npkReason[c] = tok[9];         // WHY the read failed (or OK) -- surfaced on the FAIL health line
    sensor.npkValid[c] = !anyInvalid;     // -1 sentinel is honest, NOT garbage
    sensor.msNpk[c] = millis();
    sensor.lastNanoMs = millis();
    String d = "NPK";                     // pipe-delimited, CSV-safe (sec.25.2 -- no commas in detail)
    for (int i = 1; i < n; i++) d += "|" + tok[i];
    logEvent("NANO", "SENSOR", d);
    return true;
  }
  if (cmd == "STATUS" && n >= 3) {        // heartbeat: STATUS,NANO,OK
    sensor.lastNanoMs = millis();
    return true;
  }
  return false;   // unrecognized command / wrong field count = Tier 1 garbage
}

void escalateNanoRecovery() {
  // 2-LAYER LADDER (sec.18.9.5): the Nano hardware-reset layer was REMOVED when PCF8575 P17 was
  // repurposed to the master actuator-power cutoff (sec.19.4.8) -- the Nano RESET pin is no longer
  // driven. Recovery is now (1) the Nano's own internal WDT and (2) the software RESET_REQ below.
  // Layer 2: software RESET_REQ (sec.18.9.5.0.2)
  if (!nanoResetReqInFlight) {
    sendNanoCommand("RESET_REQ");
    nanoResetReqInFlight = true;
    nanoResetReqMs = millis();
    nanoResetsToday++;
    logEvent("ESP1", "RESET", "NANO|RESET_REQ|GARBAGE5");
    raiseFault('W', "NANO_RESET", "SOFT");
    garbageCount = 0;
    return;
  }
  // No further automatic reset: if RESET_REQ does not restore valid data, ESP1 logs/alerts once
  // and keeps operating on the most recent valid data (deliberate reduction, sec.18.9.5.1).
  if (millis() - nanoResetReqMs > 10000) {   // software path didn't restore valid data
    logEvent("ESP1", "RESET", "NANO|RESET_REQ_FAILED|LAST_VALID");
    raiseFault('M', "NANO_GARBAGE", "PERSIST");
    nanoResetReqInFlight = false;
    garbageCount = 0;
  }
}

void sendNanoCommand(const char *cmd) {
  nanoSerial.print(FRAME_START);
  nanoSerial.print(",");
  nanoSerial.print(cmd);
  nanoSerial.print(",");
  nanoSerial.println(FRAME_END);
  logEvent("ESP1", "CMD", String("NANO|") + cmd);
}

/* =============================================================================
 *  ESP2 LINK  --  work-order supervisor (spec sec.9.8.1.1)
 * ========================================================================== */
void pollESP2() {
  static String line;
  while (esp2Serial.available()) {
    char c = (char)esp2Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length() > 0) {
        String raw = line; line = "";
        lastEsp2Ms = millis();
        esp2Available = true;
        silenceProbes = 0;                       // any frame clears an in-progress silence probe
        // ESP1 heard from ESP2 -> any prior comm fault is resolved; stop showing it as the Last Fault
        // (it otherwise latches and keeps displaying even while ESP2 is idle/unpowered).
        if (lastFaultMsg.startsWith("ESP2_SILENCE") || lastFaultMsg.startsWith("ESP2_NO_READY")) {
          lastFaultMsg = "none"; lastFaultTime = "";
        }
        int s = raw.indexOf(FRAME_START), e = raw.indexOf(FRAME_END);
        String payload = (s >= 0 && e > s) ? raw.substring(s + 7, e) : raw;
        payload.trim();
        if (payload.startsWith(",")) payload = payload.substring(1);
        if (payload.endsWith(","))   payload = payload.substring(0, payload.length() - 1);
        handleEsp2Response(payload);
      }
    } else if (line.length() < 140) line += c; else line = "";
  }

  // ACK / DONE timeout supervision (suspended while a fault is HELD -- ESP2 is paused, not hung)
  if (esp2Held) return;
  if (wo.active && wo.stage == WO_SENT && millis() - wo.sentMs > UART_ACK_TIMEOUT_MS) {
    if (wo.retries < MAX_UART_RETRY) {
      wo.retries++;
      esp2Serial.println(wo.cmd);
      wo.sentMs = millis();
      logEvent("ESP1", "RESP", "RETRY|" + wo.cmd + "|" + String(wo.retries));
    } else {
      logEvent("ESP1", "RESP", "TIMEOUT|" + wo.cmd);
      wo.active = false; wo.stage = WO_IDLE;
      esp2SoftResetTried = false;            // fresh ladder: soft reset (RESET_SELF) before power-cycle
      setState(RECOVERY_STATE);              // RECOVERY_STATE tick owns the escalation now
    }
  }
  if (wo.active && wo.stage == WO_ACKED && millis() - wo.sentMs > UART_DONE_TIMEOUT_MS) {
    logEvent("ESP1", "RESP", "TIMEOUT|" + wo.cmd);
    raiseFault('C', "ESP2_DONE_TIMEOUT", "ESP2");
    wo.active = false; wo.stage = WO_IDLE;
  }
}

void handleEsp2Response(const String &payload) {
  // payload e.g. "DONE,SEQ_IRRIGATION_A" or "ACK,..." or "FLOW_FAIL,MAIN"
  int comma = payload.indexOf(',');
  String resp = (comma < 0) ? payload : payload.substring(0, comma);
  String arg  = (comma < 0) ? ""      : payload.substring(comma + 1);

  if (resp == "CAL") {                                    // live raw sample from ESP2 (§A.3): id,raw
    int c2 = arg.indexOf(',');
    if (c2 > 0) { calRxId = arg.substring(0, c2); calRxRaw = arg.substring(c2 + 1).toFloat();
                  calRxValid = (calRxRaw != -1); calRxMs = millis(); }
    return;
  }
  if (resp == "TELE") {                                   // power telemetry during a run: PZEM,<v>,<i>,<p>,ACS,<a>
    // arg = "PZEM,228.0,1.40,319.0,ACS,0.80" -> log verbatim as pipe-delimited for the SD log / SUMMARY.
    String d = arg; d.replace(",", "|");                  // CSV-safe (sec.25.2: no commas in detail)
    logEvent("ESP2", "SENSOR", d);
    return;
  }
  if (resp == "READY") {
    // ESP2 finished booting (OFF-during-idle model). Fire whatever was waiting on it.
    logEvent("ESP2", "RESP", payload);
    esp2CommRecovered();                                         // any READY = link alive -> clear COMM_LOST latch
    // Soft-reset recovery: ESP2 came back after a RESET_SELF -> recovered WITHOUT a power-cycle.
    // (Gated on READY, not esp2Available: ESP2 ACKs RESET_SELF before it reboots, sec.9.7.2.)
    if (esp2SoftResetTried && sysState == RECOVERY_STATE) {
      logEvent("ESP2", "RESP", "SOFT_RECOVERED");
      esp2SoftResetTried = false;
      pendingRun.active = false; pendingRun.colIdx = -1;          // drop anything abandoned at failure
      pendingExercise.active = false; pendingExercise.idx = -1; pendingExercise.sent = false;
      firebaseRemoteExerciseDone("failed", "aborted by ESP2 recovery");
      setState(STARTUP_SYNC);                                     // re-validate before resuming (sec.10.7.7)
      return;
    }
    // Testing arming is handled by the primed retry in testHoldTick (the single READY here
    // can be lost on a cold relay boot), so we do NOT send TEST,ENTER from this one message.
    if (pendingRun.active && !wo.active) dispatchPendingRun();  // scheduled-run warm-up complete
    if (pendingExercise.active && !pendingExercise.sent) dispatchPendingExercise();  // exercise warm-up done
    return;
  }
  if (resp == "STATUS") return;                                // heartbeat
  if (resp == "INFO") { logEvent("ESP2", "RESP", payload); return; }  // e.g. INFO,IDLE_RESET (self-heal notice)

  if (resp == "STAGE") {                                    // STAGE,<ord>,<total>,<Name> -> run-progress LCD
    String t[4]; int nt = 0, st = 0;
    for (int i = 0; i <= (int)arg.length() && nt < 4; i++)
      if (i == (int)arg.length() || arg[i] == ',') { t[nt++] = arg.substring(st, i); st = i + 1; }
    if (nt >= 3 && runPhase != RUN_NONE) {
      run.ord = (uint8_t)t[0].toInt(); run.total = (uint8_t)t[1].toInt();
      strncpy(run.stage, t[2].c_str(), sizeof(run.stage) - 1); run.stage[sizeof(run.stage) - 1] = 0;
      run.stageL = 0; run.stageTgt = 0;                      // new stage -> reset the bar
      wakeBacklight();
    }
    logEvent("ESP2", "STAGE", arg);
    return;
  }
  if (resp == "PROG") {                                     // PROG,<stageL>,<stageTgt>,<waterSoFar> (~1 Hz, not logged)
    String t[3]; int nt = 0, st = 0;
    for (int i = 0; i <= (int)arg.length() && nt < 3; i++)
      if (i == (int)arg.length() || arg[i] == ',') { t[nt++] = arg.substring(st, i); st = i + 1; }
    if (nt >= 3 && runPhase != RUN_NONE) {
      run.stageL = t[0].toFloat(); run.stageTgt = t[1].toFloat(); run.waterGot = t[2].toFloat();
    }
    return;
  }

  if (resp == "DOSE") {
    // Per-nutrient dose result: DOSE,NUT_x,target_mL,measured_mL,COL_y (sec.25.2.1).
    // Accumulate measured nutrient volume for the daily report (B1); log as DOSE.
    String t[5]; int nt = 0, st = 0;
    for (int i = 0; i <= (int)arg.length() && nt < 5; i++)
      if (i == (int)arg.length() || arg[i] == ',') { t[nt++] = arg.substring(st, i); st = i + 1; }
    if (nt >= 4) {
      int dc = -1;
      for (int j = 0; j < NUM_COLUMNS; j++) if (t[3] == String("COL_") + COL_TAG[j]) dc = j;
      if (dc >= 0) nutrientUsedToday[dc] += t[2].toFloat();     // measured mL
      if (runPhase != RUN_NONE && t[0].length() >= 5) {         // NUT_A/B/C -> the post-run receipt
        int ni = t[0].charAt(4) - 'A';
        if (ni >= 0 && ni < 3) run.doseGot[ni] = t[2].toFloat();
      }
      logEvent("ESP2", "DOSE", t[0] + "|" + t[1] + "|" + t[2] + "|" + t[3]);
    }
    return;
  }

  logEvent("ESP2", "RESP", payload);

  if (resp == "ACK") {
    if (arg.startsWith("TEST")) esp2TestArmed = true;          // ESP2 confirmed TEST mode -> stop retrying
    if (arg.startsWith("SET_CAL")) lastSetCalAck = arg;        // block-until-ACK on calibration save (§A.5.1)
    if (arg.startsWith("EXERCISE") && pendingExercise.active && !pendingExercise.acked) {
      pendingExercise.acked = true;                            // ESP2 confirmed receipt -> log START now (not at dispatch)
      logEvent("ESP1", "ACT", String("EXERCISE|START|") + (pendingExercise.idx >= 0 ? EX_NAME[pendingExercise.idx] : "?"));
    }
    if (wo.active) { wo.stage = WO_ACKED; wo.sentMs = millis(); }
  } else if (resp == "DONE") {
    if (arg.startsWith("EXERCISE")) {                          // preventive exercise finished
      int k = pendingExercise.idx;
      if (k >= 0 && k < 3) lastPumpUseMs[k] = millis();        // reset that pump's 2-day clock
      logEvent("ESP1", "ACT", String("EXERCISE|STOP|") + (k >= 0 ? EX_NAME[k] : "?"));
      pendingExercise.active = false; pendingExercise.idx = -1; pendingExercise.sent = false;
      firebaseRemoteExerciseDone("completed", "5 second pump test finished");   // no-op if locally initiated
      if (sysState == ACTIVE_STATE) setState(IDLE_STATE);
      esp2SetPower(false);
    } else if (wo.active) {
      int c = wo.colIdx;
      if (c >= 0 && c < NUM_COLUMNS) {
        col[c].lastServicedStamp = currentDayStamp;
        // B1: optional measured water volume reported as ...,WATER,<liters> on DONE.
        int wi = payload.indexOf("WATER,");
        float liters = (wi >= 0) ? payload.substring(wi + 6).toFloat() : 0.0f;
        if (forceMask && __builtin_popcount(forceMask) > 1) {
          // Multi-column FORCE run: one tank, one outlet, one flow meter. The measured litres are a
          // COMBINED total and the per-column split is set by plumbing resistance, not by us. Split
          // it evenly for the daily tally but mark the row EST so the thesis data never presents a
          // divided estimate as if it were measured.
          int nc = __builtin_popcount(forceMask);
          String cols;
          for (int b = 0; b < NUM_COLUMNS; b++) if (forceMask & (1 << b)) {
            waterUsedToday[b] += liters / nc;
            cols += (char)('A' + b);
          }
          logEvent("ESP1", "ACT", String(wo.fertigate ? "FERTIGATION" : "IRRIGATION")
                                    + "|STOP|FORCE|COL_" + cols + "|W=" + String(liters, 2)
                                    + "|SPLIT=EST/" + String(nc));
        } else {
          if (wi >= 0) waterUsedToday[c] += liters;
          // Log the litres in the ACT row (|W=) so SUMMARY/FULL can recover per-day water totals.
          logEvent("ESP1", "ACT", String(wo.fertigate ? "FERTIGATION" : "IRRIGATION")
                                    + "|STOP|" + (forceMask ? "FORCE|" : "") + "COL_" + COL_TAG[c]
                                    + "|W=" + String(liters, 2));
        }
        if (forceCmdId[0]) {                       // report the run back to the dashboard
          firebaseQueueStatus(forceCmdId, "completed", (String("delivered ") + String(liters, 2) + " L").c_str());
          forceCmdId[0] = 0;
        }
        forceMask = 0;
      }
      // A real run exercises transfer+booster (and mixer if fertigating) -> reset their clocks.
      lastPumpUseMs[0] = millis(); lastPumpUseMs[1] = millis();
      if (wo.fertigate) lastPumpUseMs[2] = millis();
      wo.active = false; wo.stage = WO_IDLE; wo.colIdx = -1;
      if (sysState == ACTIVE_STATE) setState(IDLE_STATE);
      esp2SetPower(false);             // run complete -> ESP2 off (idle power-saving, sec.18.8)
      lastHoldCode = ""; sameHoldN = 0; steerRelease = false;   // run completed -> clear the re-hold guard
      runUiFinish(true);               // -> post-run receipt (+ error table if the run logged problems)
    }
  } else if (resp == "BUSY") {
    // ESP2 busy; leave work order pending, it will retry/await
  } else if (resp == "DEGRADED") {
    // ESP2 disabled one channel (nutrient/mixer) but is STILL running the sequence:
    // Major, NOT Critical -- do not abort or cut power, just log+alert (sec.23.2.2.1/.4).
    raiseFault('M', "ESP2_DEGRADED", arg.c_str());
  } else if (resp == "ERROR") {
    enterFaultHold("ESP2_ERROR", arg.c_str());   // ESP2 holds (P17 dropped); keep wo for user-gated RESUME
  } else if (resp == "FLOW_FAIL") {
    enterFaultHold("FLOW_FAIL", arg.c_str());
  } else if (resp == "PWR_FAIL") {
    enterFaultHold("PWR_FAIL", arg.c_str());
  } else if (resp == "EC_FAIL") {
    raiseFault('M', "EC_FAIL", arg.c_str());
  } else if (resp == "PH_FAIL") {
    raiseFault('M', "PH_FAIL", arg.c_str());
  } else if (resp == "SENSOR_FAIL") {
    // ESP2 EC/pH probe railed (disconnected/shorted) -- hardware sensor fault, distinct from
    // EC_FAIL/PH_FAIL (out-of-window). Major: alert + log, batch still delivered.
    raiseFault('M', "SENSOR_FAIL", arg.c_str());
  } else if (resp == "PCF_FAIL") {
    // ESP2 relay driver (PCF8575) not responding. Major: alert + log, no shutdown (user policy);
    // other safeties (PZEM no-current, flow timeout) still apply.
    raiseFault('M', "PCF_FAIL", arg.c_str());
  } else if (resp == "PCF_OK") {
    logEvent("ESP2", "RESP", "PCF_RECOVERED");                 // informational (relay bus back)
  } else if (resp == "SAFE_STOP") {
    enterFaultHold("SAFE_STOP", arg.c_str());
  } else if (resp == "DOSE_TIMEOUT") {
    // A nutrient pump ran past its 2x timed ceiling (stuck-low flow sensor or unprimed line, §5).
    // NOT a fallback -- hold for the user (prime the line, then RELEASE/IRRIGATE/NORMAL).
    enterFaultHold("DOSE_TIMEOUT", arg.c_str());
  } else if (resp == "INVALID") {
    // A recovery RESUME was refused. The usual cause is that ESP2 restarted during the hold and
    // lost its paused sequence (the mix-tank volume still survives in its NVS). Clear the held UI
    // and let the still-due column re-run normally -- ESP2 will fill only the remaining liters.
    if (esp2Held && arg.startsWith("NOT_HELD")) {
      esp2Held = false; wo.active = false; wo.stage = WO_IDLE;
      logEvent("ESP1", "RESP", "RESUME_REFUSED|ESP2_RESTARTED");
      sendSMS("ESP2 restarted; tank volume kept. Column will re-run on its own.");
      if (sysState == EMERGENCY_STOP) setState(IDLE_STATE);
    }
  }
}

// Open-loop gap dose (§3): see the forward-declared comment in the dosing tables block.
void calcDose(int c, float mL[3], float ceilS[3]) {
  for (int i = 0; i < 3; i++) { mL[i] = 0; ceilS[i] = 0; }
  float V = WATER_BUDGET_L[c] * (1.0f - FLUSH_PCT / 100.0f);   // planned batch volume this run (§1.1)
  if (V < MIXING_TANK_SAFE_MIN) { logEvent("ESP1", "DOSE", "BATCH_LOW|water-only"); return; }
  if (V > MIXING_TANK_MAX_VOLUME) V = MIXING_TANK_MAX_VOLUME;
  // gaps (mg/kg); only positive gaps dose. A=N (gated), B=P, C=K (§3.2/3.3). dose_mL = gap*V/stock (§3.1).
  float gap[3]   = { col[c].targetN - SOIL_BASELINE_N, col[c].targetP - SOIL_BASELINE_P, col[c].targetK - SOIL_BASELINE_K };
  float stock[3] = { STOCK_A_N, STOCK_B_P, STOCK_C_K };
  for (int i = 0; i < 3; i++) {
    if (gap[i] <= 0 || stock[i] <= 0 || calKNut[i] <= 0) continue;   // non-positive gap -> skip (A gap_N gate)
    float dose = gap[i] * V / stock[i];                              // §3.1 (literal)
    long pulses = lround(dose * calKNut[i] / 1000.0f);               // SCALE = K/1000 pulses/mL (§3.4)
    char b[48];
    if (pulses < MIN_DOSE_PULSES) {                                  // sub-resolution -> skip + log (thesis record)
      snprintf(b, sizeof(b), "SUBRES|NUT_%c|int=%dmL", (char)('A' + i), (int)(dose + 0.5f));
      logEvent("ESP1", "DOSE", b);
      continue;
    }
    mL[i] = pulses * 1000.0f / calKNut[i];                           // honest whole-pulse delivered volume
    ceilS[i] = mL[i] / PUMP_FLOWRATE_MLPM[i] * 60.0f * CEILING_MARGIN;
    snprintf(b, sizeof(b), "NUT_%c|int=%d|del=%dmL", (char)('A' + i), (int)(dose + 0.5f), (int)(mL[i] + 0.5f));
    logEvent("ESP1", "DOSE", b);
  }
}

void sendWorkOrder(int c, bool fertigate) {
  // Doses were computed once in runUiBegin() (so the pre-run receipt and the work order agree, and
  // calcDose's per-nutrient log lines are not emitted twice). Fall back if the run state is stale.
  float mL[3] = { 0, 0, 0 }, ceilS[3] = { 0, 0, 0 };
  if (fertigate) {
    if (run.col == c && run.fert) { for (int i = 0; i < 3; i++) { mL[i] = run.doseTgt[i]; ceilS[i] = run.ceilS[i]; } }
    else calcDose(c, mL, ceilS);
  }

  // Complete work order: column, water/flush split, dose mL + ceilings, EC/pH window (sec.9.8.1.1)
  String name = String("SEQ_") + (fertigate ? "FERTIGATION_" : "IRRIGATION_") + COL_TAG[c];
  String cmd = String(FRAME_START) + "," + name +
               ",FLUSH," + String((int)FLUSH_PCT);
  if (fertigate) {
    cmd += ",DOSE," + String(mL[0], 1) + "," + String(mL[1], 1) + "," + String(mL[2], 1);
    cmd += ",DCEIL," + String(ceilS[0], 0) + "," + String(ceilS[1], 0) + "," + String(ceilS[2], 0);  // 2x timed ceilings (s)
    cmd += ",BATCHV," + String(WATER_BUDGET_L[c] * (1.0f - FLUSH_PCT / 100.0f), 1);                   // planned batch L (§1.1)
    cmd += ",EC," + String(EC_MIN, 1) + "," + String(EC_MAX, 1);
    cmd += ",PH," + String(PH_MIN, 1) + "," + String(PH_MAX, 1);
    cmd += ",MIX," + String(MIXING_DURATION_MS);
  }
  // Job-critical calibration (companion spec §A.5.1 #3): make the job self-contained so a reset
  // moments before it runs can never execute on stale K-factors / EC-pH cal.
  cmd += ",KMAIN," + String(calKResMix, 1) + "," + String(calKMixIrr, 1);
  if (fertigate) {
    cmd += ",KNUT," + String(calKNut[0], 1) + "," + String(calKNut[1], 1) + "," + String(calKNut[2], 1);
    cmd += ",ECCAL," + String(calEcM, 6) + "," + String(calEcB, 4);
    cmd += ",PHCAL," + String(calPhM, 6) + "," + String(calPhB, 4);
  }
  cmd += "," + String(FRAME_END);

  wo.active = true; wo.stage = WO_SENT; wo.colIdx = c; wo.fertigate = fertigate;
  wo.cmd = cmd; wo.retries = 0; wo.sentMs = millis();
  esp2Serial.println(cmd);
  logEvent("ESP1", "CMD", "ESP2|" + name);
  logEvent("ESP1", "ACT", String(fertigate ? "FERTIGATION" : "IRRIGATION")
                            + "|START|COL_" + COL_TAG[c]);

  // Consolidated run notice (one per column service; sec.12.1.2)
  sendSMS(String("RUN,COL_") + COL_TAG[c] + "," + (fertigate ? "FERTIGATION" : "IRRIGATION"));
}

/* Operator FORCE run: build a work order from explicit litres + explicit per-nutrient mL instead of
 * the schedule and calcDose(). Everything is already bounds-checked by the caller.
 * `mask` may name several columns -- ESP2 opens them together, so `liters` is the TOTAL batch.
 * fbCmdId is echoed to the dashboard when the run finishes ("" for an SMS-initiated force). */
void sendForceWorkOrder(uint8_t mask, float liters, const float doseMl[3], const char *fbCmdId) {
  int primary = 0;                                   // lowest column in the mask: budget + logging anchor
  while (primary < NUM_COLUMNS && !(mask & (1 << primary))) primary++;
  if (primary >= NUM_COLUMNS) return;

  bool fert = (doseMl[0] > 0.0f || doseMl[1] > 0.0f || doseMl[2] > 0.0f);
  String cols;
  for (int b = 0; b < NUM_COLUMNS; b++) if (mask & (1 << b)) cols += (char)('A' + b);

  String name = String("SEQ_") + (fert ? "FERTIGATION_" : "IRRIGATION_") + COL_TAG[primary];
  String cmd  = String(FRAME_START) + "," + name +
                ",COLS," + cols +
                ",WATER," + String(liters, 2) +
                ",FLUSH," + String((int)FLUSH_PCT);
  if (fert) {
    cmd += ",DOSE," + String(doseMl[0], 1) + "," + String(doseMl[1], 1) + "," + String(doseMl[2], 1);
    // Same 2x timed ceiling calcDose() uses, derived from the operator's mL rather than the gap.
    cmd += ",DCEIL,";
    for (int i = 0; i < 3; i++) {
      float s = (doseMl[i] > 0.0f) ? doseMl[i] / PUMP_FLOWRATE_MLPM[i] * 60.0f * CEILING_MARGIN : 0.0f;
      cmd += String(s, 0) + (i < 2 ? "," : "");
    }
    cmd += ",BATCHV," + String(liters * (1.0f - FLUSH_PCT / 100.0f), 1);
    cmd += ",EC," + String(EC_MIN, 1) + "," + String(EC_MAX, 1);
    cmd += ",PH," + String(PH_MIN, 1) + "," + String(PH_MAX, 1);
    cmd += ",MIX," + String(MIXING_DURATION_MS);
  }
  cmd += ",KMAIN," + String(calKResMix, 1) + "," + String(calKMixIrr, 1);
  if (fert) {
    cmd += ",KNUT," + String(calKNut[0], 1) + "," + String(calKNut[1], 1) + "," + String(calKNut[2], 1);
    cmd += ",ECCAL," + String(calEcM, 6) + "," + String(calEcB, 4);
    cmd += ",PHCAL," + String(calPhM, 6) + "," + String(calPhB, 4);
  }
  cmd += "," + String(FRAME_END);

  wo.active = true; wo.stage = WO_SENT; wo.colIdx = primary; wo.fertigate = fert;
  wo.cmd = cmd; wo.retries = 0; wo.sentMs = millis();
  forceMask = mask;                                  // remembered so DONE can be attributed honestly
  strlcpy(forceCmdId, fbCmdId ? fbCmdId : "", sizeof(forceCmdId));

  setState(ACTIVE_STATE);
  runUiBegin(primary, fert);
  esp2WarmupMs = millis();
  if (esp2Available && esp2Powered) esp2Serial.println(cmd);   // already up -> send now
  else                             esp2SetPower(true);         // warm up; READY re-sends via wo.cmd

  logEvent("ESP1", "CMD", "ESP2|" + name + "|FORCE");
  logEvent("ESP1", "ACT", String(fert ? "FERTIGATION" : "IRRIGATION") + "|START|FORCE|COL_" + cols +
                          "|W=" + String(liters, 2) +
                          (fert ? "|mL=" + String(doseMl[0], 0) + "/" + String(doseMl[1], 0) + "/" + String(doseMl[2], 0) : ""));
  sendSMS(String("RUN,FORCE,COL_") + cols + "," + (fert ? "FERTIGATION" : "IRRIGATION") +
          "," + String(liters, 1) + "L");
  if (fbCmdId && *fbCmdId) firebaseQueueStatus(fbCmdId, "accepted", "force run started");
}

/* =============================================================================
 *  RUN PROGRESS UI  --  pre-receipt / live stages / post-receipt / error table
 * ========================================================================== */
static const char *runPlant(int c) { return strlen(col[c].name) ? col[c].name : "-"; }

// Record a problem seen during a run so the post-run error table can show it (ring, oldest kept).
void runNoteErr(const String &e) {
  if (runPhase == RUN_NONE || runErrN >= RUN_ERR_MAX) return;
  runErr[runErrN++] = e;
  run.ok = false;
}

// Queue-time: compute the doses ONCE, snapshot the "before" NPK, and raise the pre-run receipt.
// dispatchPendingRun() waits for this receipt to be acknowledged (or time out) before sending.
void runUiBegin(int c, bool fert) {
  memset(&run, 0, sizeof(run));
  run.fert = fert; run.col = c; run.ok = true;
  run.total = fert ? ((FLUSH_PCT > 0) ? 7 : 5) : 2;
  run.ord = 0;
  strncpy(run.stage, "Starting", sizeof(run.stage) - 1);
  run.waterTgt = WATER_BUDGET_L[c];
  run.batchV   = WATER_BUDGET_L[c] * (1.0f - FLUSH_PCT / 100.0f);
  if (fert) calcDose(c, run.doseTgt, run.ceilS);              // computed once; sendWorkOrder reuses these
  for (int i = 0; i < 3; i++) run.npkBefore[i] = sensor.npk[c][4 + i];
  run.startMs = millis();
  runErrN = 0; runPage = 0;
  runPhase = RUN_PRE; runPhaseMs = millis(); runLocked = true;
  wakeBacklight();

  String d = String("PRE|COL_") + COL_TAG[c] + "|" + (fert ? "FERTIGATION" : "IRRIGATION")
           + "|PLANT=" + runPlant(c)
           + "|WATER=" + String(run.waterTgt, 1) + "|FLUSH=" + String((int)FLUSH_PCT)
           + "|STEPS=" + String(run.total);
  if (fert) {
    d += "|N=" + String((int)col[c].targetN) + "|P=" + String((int)col[c].targetP) + "|K=" + String((int)col[c].targetK);
    d += "|A=" + String(run.doseTgt[0], 1) + "|B=" + String(run.doseTgt[1], 1) + "|C=" + String(run.doseTgt[2], 1);
  }
  logEvent("ESP1", "RECEIPT", d);
}

// Expected mg/kg increase from the mL actually pumped -- inverts calcDose (gap = mL * stock / V).
static float runExpected(int i) {
  const float stock[3] = { STOCK_A_N, STOCK_B_P, STOCK_C_K };
  if (run.batchV <= 0 || stock[i] <= 0) return 0;
  return run.doseGot[i] * stock[i] / run.batchV;
}

// Run finished (DONE or a terminal fault): capture the "after" NPK, log the post receipt, show it.
void runUiFinish(bool ok) {
  if (runPhase == RUN_NONE) return;
  if (!ok) run.ok = false;
  for (int i = 0; i < 3; i++) run.npkAfter[i] = sensor.npk[run.col][4 + i];

  String d = String("POST|COL_") + COL_TAG[run.col] + "|" + (run.fert ? "FERTIGATION" : "IRRIGATION")
           + "|" + (run.ok ? "OK" : "WITH_ERRORS")
           + "|WATER=" + String(run.waterGot, 2) + "of" + String(run.waterTgt, 1)
           + "|SECS=" + String((millis() - run.startMs) / 1000);
  if (run.fert) {
    const char nm[3] = { 'N', 'P', 'K' };
    for (int i = 0; i < 3; i++) {
      d += String("|") + nm[i] + "=" + String(run.npkBefore[i], 0) + ">" + String(run.npkAfter[i], 0)
         + "|exp" + nm[i] + "=" + String(runExpected(i), 0) + "|mL" + (char)('A' + i) + "=" + String(run.doseGot[i], 1);
    }
  }
  d += "|ERRORS=" + String(runErrN);
  logEvent("ESP1", "RECEIPT", d);
  for (uint8_t k = 0; k < runErrN; k++) logEvent("ESP1", "RECEIPT", "ERR|" + String(k + 1) + "|" + runErr[k]);

  runPhase = RUN_POST; runPhaseMs = millis(); runPage = 0; runLocked = true;
  wakeBacklight();
}

// Advance the receipt phases: ENTER (handled in handleButtons) or the unattended auto-continue.
void runUiTick() {
  if (runPhase == RUN_PRE || runPhase == RUN_POST || runPhase == RUN_ERR) {
    if (millis() - runPhaseMs >= RECEIPT_TIMEOUT_MS) runUiAdvance();
  }
}

// 8-cell progress bar, e.g. "[####----]". frac is clamped 0..1.
static String runBar(float frac) {
  if (!(frac > 0)) frac = 0; if (frac > 1) frac = 1;
  int on = (int)(frac * 8 + 0.5f);
  String s = "[";
  for (int i = 0; i < 8; i++) s += (i < on) ? '#' : '-';
  return s + "]";
}

// The run screens. Layout: header (operation / column / plant), stage line with the step counter,
// then the targets and a live detail row. Same shape for irrigation, minus the nutrient rows.
void lcdRenderRun() {
  char l[21];
  int  c = run.col;

  if (runPhase == RUN_PRE) {
    snprintf(l, 21, "RECEIPT  %c  %s", COL_TAG[c], runPlant(c));                       lcdRow(0, l);
    snprintf(l, 21, "%s %.1fL +%d%%", run.fert ? "FERTIGATE" : "IRRIGATE", run.waterTgt, (int)FLUSH_PCT); lcdRow(1, l);
    if (run.fert) {
      snprintf(l, 21, "Tgt N%d P%d K%d", (int)col[c].targetN, (int)col[c].targetP, (int)col[c].targetK); lcdRow(2, l);
      snprintf(l, 21, "A%d B%d C%dmL ENT=go", (int)run.doseTgt[0], (int)run.doseTgt[1], (int)run.doseTgt[2]); lcdRow(3, l);
    } else {
      snprintf(l, 21, "%d steps to run", run.total);                                    lcdRow(2, l);
      lcdRow(3, "ENTER = start");
    }
    return;
  }

  if (runPhase == RUN_LIVE) {
    snprintf(l, 21, "%s %c %s", run.fert ? "FERTIGATE" : "IRRIGATE", COL_TAG[c], runPlant(c)); lcdRow(0, l);
    snprintf(l, 21, "%d/%d %s", run.ord, run.total, run.stage);                          lcdRow(1, l);
    if (run.fert) {
      snprintf(l, 21, "Tgt N%d P%d K%d", (int)col[c].targetN, (int)col[c].targetP, (int)col[c].targetK); lcdRow(2, l);
      // Detail row: during dosing show the nutrient mL, otherwise the water moved so far.
      float frac = (run.stageTgt > 0) ? (run.stageL / run.stageTgt) : 0;
      if (run.ord == 2) snprintf(l, 21, "Nut %s %s", String(run.doseGot[0] + run.doseGot[1] + run.doseGot[2], 0).c_str(), runBar(frac).c_str());
      else              snprintf(l, 21, "%.1fL %s", run.waterGot, runBar(frac).c_str());
      lcdRow(3, l);
    } else {
      snprintf(l, 21, "Target %.1f L", run.waterTgt);                                    lcdRow(2, l);
      float frac = (run.waterTgt > 0) ? (run.waterGot / run.waterTgt) : 0;
      snprintf(l, 21, "%.1fL %s", run.waterGot, runBar(frac).c_str());                    lcdRow(3, l);
    }
    return;
  }

  if (runPhase == RUN_POST) {
    snprintf(l, 21, "RESULT %c %s %s", COL_TAG[c], runPlant(c), run.ok ? "OK" : "!!");    lcdRow(0, l);
    snprintf(l, 21, "Water %.1fL of %.1fL", run.waterGot, run.waterTgt);                  lcdRow(1, l);
    if (run.fert) {
      // Measured NPK delta next to the expected increase from the mL actually pumped.
      const char nm[3] = { 'N', 'P', 'K' };
      int i0 = (runPage == 0) ? 0 : 2;                    // page 0: N,P   page 1: K (+ hint)
      for (int r = 0; r < 2; r++) {
        int i = i0 + r;
        if (i > 2) { lcdRow(2 + r, runErrN ? "ENTER = errors" : "ENTER = done"); continue; }
        snprintf(l, 21, "%c%d>%d e+%d %dmL", nm[i], (int)run.npkBefore[i], (int)run.npkAfter[i],
                 (int)runExpected(i), (int)run.doseGot[i]);
        lcdRow(2 + r, l);
      }
    } else {
      snprintf(l, 21, "Took %lus", (millis() - run.startMs) / 1000);                      lcdRow(2, l);
      lcdRow(3, runErrN ? "ENTER = errors" : "ENTER = done");
    }
    return;
  }

  // RUN_ERR: one problem per screen.
  snprintf(l, 21, "ERRORS (%d)     %d/%d", runErrN, runPage + 1, runErrN);                lcdRow(0, l);
  String e = (runPage < runErrN) ? runErr[runPage] : String("");
  snprintf(l, 21, "%s", e.substring(0, 20).c_str());                                      lcdRow(1, l);
  snprintf(l, 21, "%s", e.length() > 20 ? e.substring(20, 40).c_str() : "");              lcdRow(2, l);
  lcdRow(3, (runPage + 1 < runErrN) ? "ENTER = next" : "ENTER = done");
}

// Abnormal end (held fault / emergency stop): close the run record out so the receipt + error rows
// still reach the log, then RELEASE the screen immediately -- the fault / recovery UI must own the
// LCD and the buttons, otherwise the operator could not answer STOP/RELEASE/IRRIGATE/NORMAL.
void runUiAbort(const char *why) {
  if (runPhase == RUN_NONE) return;
  runNoteErr(String(why));
  runUiFinish(false);            // logs RECEIPT|POST + RECEIPT|ERR rows
  runPhase = RUN_NONE;           // do NOT hold the screen on the receipt during a fault
  runLocked = true;              // re-armed for the next run
}

// One step forward through PRE -> (run) -> POST -> ERR -> done.
void runUiAdvance() {
  if (runPhase == RUN_PRE) {
    runPhase = RUN_LIVE; runPhaseMs = millis();
    esp2WarmupMs = millis();     // the ESP2 cold-boot budget starts NOW, not while the receipt was up
    return;                      // releases dispatchPendingRun()
  }
  if (runPhase == RUN_POST) {
    if (runErrN > 0) { runPhase = RUN_ERR; runPage = 0; runPhaseMs = millis(); return; }
    runPhase = RUN_NONE; runLocked = true; return;
  }
  if (runPhase == RUN_ERR) {
    if (++runPage < runErrN) { runPhaseMs = millis(); return; }
    runPhase = RUN_NONE; runLocked = true; return;
  }
}

// Re-initialize ESP1's own UART1 to ESP2 (sec.18.9). Power-cycling ESP2 can't clear a wedged RX peripheral
// (framing-error lockup / stuck FIFO) on THIS side, so every recovery attempt tears the port down and back
// up and drains any stale bytes -- the one link element that resetting ESP2 alone never touches.
void esp2ReinitUart() {
  esp2Serial.end();
  esp2Serial.begin(ESP2_BAUD, SERIAL_8N1, ESP2_RX_PIN, ESP2_TX_PIN);
  while (esp2Serial.available()) esp2Serial.read();     // drain partial/garbage frame after re-init
  logEvent("ESP1", "RESET", "ESP2|UART_REINIT");
}

// Software self-reset, at most once per calendar day (shared RTC-RAM guard with deviceHealthTick's
// device-loss reset -> at most one self-reset/day for any reason, no boot loop). The final rung of the
// comm-recovery ladder: a full ESP1 reboot reinitializes every peripheral incl. UART1.
void esp1SelfResetOncePerDay(const char *reason) {
  uint32_t today = (currentDayStamp > 0) ? (uint32_t)currentDayStamp : 0;
  if (g_lastSelfResetDay == today) return;             // already used today's one reset -> don't loop
  logEvent("ESP1", "RESET", String("SELF|") + reason);
  sendSMS(String("ALERT,MAJ,SELF_RESET,") + reason);   // best-effort (may not flush before reboot)
  logFlush(true);
  g_lastSelfResetDay = today;
  delay(60);                                            // let the last UART/SD bytes drain
  ESP.restart();
}

/* ---- Dead-RTC daily reboot ------------------------------------------------ *
 * When the RTC is unreadable, tsString() stamps every row "0000-00-00 00:00:00" and logs land in
 * NODATE.CSV -- scheduling is dead too (controlTick bails on !rtcOk), so the rig silently does nothing.
 * A reboot re-runs the I2C/RTC init and often clears a wedged bus. Reboot at most once every 24 h of
 * uptime while the timestamp stays all-zero, and ONLY when truly idle with nothing pending -- never
 * mid-run, mid-fault-hold, or while the operator is in a menu / the WiFi portal.
 * The 24 h clock is uptime-based on purpose: with a dead RTC there is no calendar day to key off, and
 * millis() resets on reboot, so the next attempt is naturally ~24 h later (no boot loop).            */
const unsigned long RTC_DEAD_REBOOT_MS = 86400000UL;   // 24 h of continuous all-zero timestamp
unsigned long rtcDeadSinceMs = 0;                       // 0 = timestamp currently valid
void rtcDeadRebootTick() {
  if (rtcOk) { rtcDeadSinceMs = 0; return; }            // timestamp valid -> reset the clock
  if (rtcDeadSinceMs == 0) { rtcDeadSinceMs = millis(); return; }   // start the 24 h clock
  if (millis() - rtcDeadSinceMs < RTC_DEAD_REBOOT_MS) return;
  // Idle-only, nothing pending, operator not busy -> otherwise wait and retry on a later tick.
  if (sysState != IDLE_STATE || wo.active || pendingRun.active || pendingExercise.active
      || esp2Held || uiMode != UI_DATA || portalActive) return;
  logEvent("ESP1", "RESET", "SELF|RTC_TS_ZERO|24h_idle");
  logFlush(true);
  delay(60);                                            // let the last UART/SD bytes drain
  ESP.restart();
}

void esp2PowerCycle() {
  if (millis() - lastRecoveryMs < RECOVERY_COOLDOWN_MS) return;
  lastRecoveryMs = millis();
  esp2ReinitUart();                      // reset ESP1's UART too -- cycling ESP2 alone can't clear an ESP1 RX wedge
  esp2SoftResetTried = false;            // power-cycle ends the ladder; a future episode soft-resets first
  esp2PowerCycles++;
  logEvent("ESP1", "RESET", "ESP2|HW|POWERCYCLE");
  digitalWrite(ESP2_PWR_PIN, LOW);
  esp2Powered = false; esp2OffPending = false;   // hard cut ends any deferred graceful off
  esp2OffMs = millis();                  // STARTUP_SYNC holds OFF for POWER_CYCLE_OFF_MS, then powers on
  esp2Available = false;
  // Abandon anything that was warming up against this ESP2 (recovery restarts it clean).
  pendingRun.active = false; pendingRun.colIdx = -1;
  pendingExercise.active = false; pendingExercise.idx = -1; pendingExercise.sent = false;
  firebaseRemoteExerciseDone("failed", "aborted by ESP2 power cycle");
  setState(STARTUP_SYNC);
}

// Capped escalation to the power-cycle (sec.18.9). Fast power-cycling ESP2 can't recover a broken link or
// a wedged ESP1 UART, so after ESP2_MAX_FAST_CYCLES we STOP hammering the relay: latch ESP2_COMM_LOST, alert
// once, and hand off to the 20-min slow-retry (esp2CommRetryTick). Final rung: one ESP1 self-reset/day.
void esp2Escalate() {
  if (esp2FastCycles >= ESP2_MAX_FAST_CYCLES) {
    if (!esp2CommLost) {
      esp2CommLost = true; esp2SlowRetryMs = millis();
      raiseFault('M', "ESP2_COMM_LOST", "LINK");       // one-shot SMS + log (check wiring/ground/baud)
      logEvent("ESP1", "FAULT", "ESP2_COMM_LOST -> slow-retry 20min (relay hammering stopped)");
    }
    setState(IDLE_STATE);                              // leave the fast recovery loop
    esp1SelfResetOncePerDay("ESP2_COMM_LOST");         // final escalation (no-op if already reset today)
    return;
  }
  esp2FastCycles++;
  esp2PowerCycle();
}

// ESP2 comm confirmed again (READY / STARTUP_SYNC validate): drop the episode counter and clear the latch.
void esp2CommRecovered() {
  esp2FastCycles = 0;
  if (esp2CommLost) {
    esp2CommLost = false;
    logEvent("ESP1", "RESET", "ESP2|COMM_RECOVERED");
    sendSMS("ALERT,MIN,ESP2_COMM_OK,LINK");
  }
}

// Slow-retry while comm is latched-lost: one gentle re-init + power-cycle every ESP2_SLOW_RETRY_MS. Success
// is validated by STARTUP_SYNC (which calls esp2CommRecovered); failure just waits another 20 min -- the
// fast loop stays disabled (RECOVERY_STATE bails while esp2CommLost), so no relay/log hammering.
void esp2CommRetryTick() {
  if (!esp2CommLost) return;
  if (millis() - esp2SlowRetryMs < ESP2_SLOW_RETRY_MS) return;
  esp2SlowRetryMs = millis();
  logEvent("ESP1", "RESET", "ESP2|SLOW_RETRY");
  esp2ReinitUart();
  lastRecoveryMs = 0;                                  // bypass the 30 s cooldown for this one paced attempt
  esp2PowerCycle();                                    // -> STARTUP_SYNC; validate clears the latch
}

// Relay power control for ESP2 (OFF-during-idle model, sec.18.8). MINIMUM ON-TIME: a graceful off
// (force=false) while ESP2 has been on < ESP2_MIN_ON_MS is DEFERRED (esp2PowerTick services it) so a
// too-brief power-up can't cut ESP2 before it boots/ACKs/heartbeats. force=true cuts immediately (safety).
// When cutting power, also drop esp2Available + any deferred arm so stale flags can't fire against a dead ESP2.
void esp2SetPower(bool on, bool force) {
  if (on) {
    esp2OffPending = false;                      // a new power-up cancels a pending graceful off
    if (esp2Powered) return;
    digitalWrite(ESP2_PWR_PIN, HIGH);
    esp2Powered = true; esp2OnMs = millis();
    return;
  }
  if (!esp2Powered) { esp2OffPending = false; return; }
  if (!force && millis() - esp2OnMs < ESP2_MIN_ON_MS) {    // hold the minimum on-time before turning off
    esp2OffPending = true; esp2OffAt = esp2OnMs + ESP2_MIN_ON_MS;
    return;
  }
  digitalWrite(ESP2_PWR_PIN, LOW);
  esp2Powered = false; esp2OffPending = false;
  esp2Available = false; testArmPending = false; esp2TestArmed = false; testArmTries = 0;
}

// Serviced each loop: (1) complete a deferred graceful off once min-on elapses; (2) periodic idle
// heartbeat poll -- when idle and ESP2 hasn't been heard for the day/night interval, power it up briefly
// to confirm it's alive, then off; a no-reply poll runs the recovery ladder (sec.18.8).
static bool isNight() {
  if (!rtcOk) return false;
  uint16_t mod = minuteOfDay();
  return (mod >= NIGHT_START_MIN) || (mod < NIGHT_END_MIN);
}
void esp2PowerTick() {
  if (esp2OffPending && esp2Powered && millis() >= esp2OffAt) esp2SetPower(false);   // min-on elapsed -> off
  if (sysState != IDLE_STATE || pendingRun.active || pendingExercise.active || esp2Held || esp2CommLost) { esp2PollActive = false; return; }
  unsigned long interval = isNight() ? ESP2_IDLE_POLL_NIGHT_MS : ESP2_IDLE_POLL_DAY_MS;
  if (!esp2PollActive) {
    if (millis() - lastEsp2Ms < interval) return;          // heard from ESP2 recently -> no poll needed
    esp2PollActive = true; esp2PollStartMs = millis();
    esp2SetPower(true);                                    // power on (or reuse if in a deferred-off window)
    logEvent("ESP1", "ESP2", "IDLE_POLL|START");
  } else if (esp2Available) {                              // ESP2 spoke -> alive
    logEvent("ESP1", "ESP2", "IDLE_POLL|ALIVE");
    esp2PollActive = false; esp2SetPower(false);           // graceful -> off after the 10 s min-on
  } else if (millis() - esp2PollStartMs > ESP2_POLL_TIMEOUT_MS) {
    logEvent("ESP1", "ESP2", "IDLE_POLL|NO_REPLY");
    esp2PollActive = false; esp2SoftResetTried = false; setState(RECOVERY_STATE);   // recovery ladder
  }
}

// Warm-up complete (ESP2 READY): send the queued scheduled run, then clear the pending slot.
void dispatchPendingRun() {
  if (!pendingRun.active) return;
  if (runPhase == RUN_PRE) return;      // hold until the pre-run receipt is acknowledged / times out
  int c = pendingRun.colIdx; bool f = pendingRun.fertigate;
  pendingRun.active = false; pendingRun.colIdx = -1;
  sendWorkOrder(c, f);
}

// Warm-up complete: send the EXERCISE command for the selected pump. esp2WarmupMs is
// repurposed as the run-completion timeout once sent (see stateMachineTick ACTIVE_STATE).
void dispatchPendingExercise() {
  if (!pendingExercise.active || pendingExercise.sent) return;
  pendingExercise.sent = true;
  esp2WarmupMs = millis();                       // now timing the 5 s run -> DONE,EXERCISE
  // sendEsp2 logs the CMD,ESP2|EXERCISE intent line. START is logged only once ESP2 confirms receipt
  // (ACK,EXERCISE, handleEsp2Response), so the log reflects a genuine command reception, not just dispatch.
  sendEsp2(String("EXERCISE,") + EX_NAME[pendingExercise.idx]);
}

// Preventive pump exercise (sec.14.9.1): when fully idle, if a pump has not run for
// PUMP_EXERCISE_INTERVAL_MS, power ESP2 up and have it run that pump briefly. Lowest
// priority -- controlTick runs first, so a due column always wins.
void exerciseTick() {
  if (sysState != IDLE_STATE) return;
  if (uiMode != UI_DATA) return;
  if (wo.active || pendingRun.active || pendingExercise.active) return;
#if BATTERY_SAFETY_ENABLED
  if (batteryLow || batteryCritical) return;   // don't exercise on a weak battery (READY, not active)
#endif
  for (int k = 0; k < 3; k++) {
    if (millis() - lastPumpUseMs[k] > PUMP_EXERCISE_INTERVAL_MS) {
      pendingExercise.active = true; pendingExercise.idx = k; pendingExercise.sent = false; pendingExercise.acked = false;
      esp2WarmupMs = millis();
      setState(ACTIVE_STATE);
      if (esp2Available && esp2Powered) dispatchPendingExercise();   // already up
      else                              esp2SetPower(true);          // warm up; READY dispatches
      return;
    }
  }
}

/* =============================================================================
 *  GSM  (inbound SMS parse + outbound, proven SIM800L pattern)
 * ========================================================================== */
void pollGSM() {
  if (gtx != GTX_IDLE) return;          // gsmTxTick owns the SIM UART now (it routes inbound too, B5)
  while (simSerial.available()) gsmFeedInbound((char)simSerial.read());
}

// Shared SIM inbound line parser. Handles +CMT push (sender + body) AND the solicited
// health replies (+CSQ/+CREG/+CPIN) for PAGE_GSM. Called from BOTH pollGSM (idle) and
// gsmTxTick (mid-send) so an inbound command is never dropped during an outbound SMS (B5).
void gsmFeedInbound(char c) {
  static String line;
  static bool expectBody = false;
  if (c == '\n') {
    line.trim();
    if (line.startsWith("+CMT:")) {                 // incoming SMS header: sender in 1st quoted field
      int q1 = line.indexOf('"');
      int q2 = (q1 >= 0) ? line.indexOf('"', q1 + 1) : -1;
      replyTarget = (q1 >= 0 && q2 > q1) ? line.substring(q1 + 1, q2) : "";
      expectBody = true;                            // next non-empty line is the message body
    } else if (line.startsWith("+CSQ:")) {          // signal quality
      lastRssi = line.substring(5).toInt();
    } else if (line.indexOf("+CREG:") >= 0) {       // network registration: stat after the comma
      int cm = line.indexOf(',');
      int stat = (cm >= 0) ? line.substring(cm + 1).toInt() : -1;
      lastCreg = stat;                              // keep the raw stat (2=searching,3=denied) for logging
      netRegistered = (stat == 1 || stat == 5);
    } else if (line.indexOf("+CPIN:") >= 0) {       // SIM ready?
      simReady = (line.indexOf("READY") >= 0);
    } else if (line.startsWith("+CMGS:")) {          // send ACCEPTED by the network -> message reference
      gtxResultSeen = true;
      String ref = line.substring(6); ref.trim();    // strip the space after the colon
      logEvent("GSM", "GSM", "TX_OK|" + ref);
    } else if (line.startsWith("+CMS ERROR:")) {     // send REJECTED -> numeric code (no credit / no reg / bad recipient)
      gtxResultSeen = true;
      String code = line.substring(11); code.trim();
      logEvent("GSM", "GSM", "TX_ERR|" + code);
    } else if (line == "ERROR" && gtx == GTX_BODY_SETTLE) {   // bare ERROR during a send = rejection w/o a code
      gtxResultSeen = true;
      logEvent("GSM", "GSM", "TX_ERR|GENERIC");
    } else if (expectBody && line.length() > 0) {
      logEvent("GSM", "GSM", "RX|" + line);
      handleSms(line);                              // replies enqueue with replyTarget (the sender)
      replyTarget = "";                             // back to owner PHONE_NUMBER for autonomous msgs
      expectBody = false;
      wakeBacklight();
    }
    line = "";
  } else if (c != '\r') {
    line += c;
    if (line.length() > 180) line = "";
  }
}

// Periodic GSM health poll for PAGE_GSM (modeled on the GSM-WITH-LCD bench). Issues the
// three queries only when no outbound send is in flight; replies parse in gsmFeedInbound.
void gsmHealthTick() {
  if (gtx != GTX_IDLE) return;                       // never interleave with an outbound send
  if (millis() - lastGsmHealthMs < GSM_HEALTH_INTERVAL_MS) return;
  lastGsmHealthMs = millis();
  simSerial.println("AT+CSQ");                        // -> +CSQ: rssi,ber
  simSerial.println("AT+CREG?");                      // -> +CREG: n,stat
  simSerial.println("AT+CPIN?");                      // -> +CPIN: READY
}

// Parse compact comma commands (spec sec.12.2). Keyword match is case-insensitive.
void handleSms(const String &body) {
  String b = body; b.trim();
  String U = b; U.toUpperCase();

  // Owner-gate EVERY command: only the configured owner number may command the system (STATUS/NET/
  // SUMMARY included). senderIsOwner() returns true for internally-generated calls (replyTarget empty,
  // e.g. the deferred-config replay), so those still pass. Non-owner -> ERR,AUTH.
  if (!senderIsOwner()) { sendSMS("ERR,AUTH"); return; }

  // Defer a remote config-write that collides with an open LOCAL edit (companion spec §B.3.1):
  // apply it once the operator finishes the edit, so neither silently overwrites the other.
  if ((uiMode == UI_EDIT || editConfirm) &&
      (U.startsWith("SET") || U.startsWith("MODE") || U.startsWith("NAME"))) {
    pendingCfgSms = b;
    sendSMS("BUSY,local edit; applied after");
    return;
  }

  // ---- ESP2 fault recovery (only while a fault is HELD, sec.19.4.8.2) ----
  // While held, STOP means acknowledge & keep holding (NOT the global emergency stop); the
  // other three resolve the held tank to the column.
  if (esp2Held) {
    if (U == "STOP")     { issueRecovery(0); return; }   // hold / acknowledge
    if (U == "RELEASE")  { issueRecovery(1); return; }   // dump tank to its column as-is
    if (U == "IRRIGATE") { issueRecovery(2); return; }   // top up water + deliver (no dosing)
    if (U == "NORMAL")   { issueRecovery(3); return; }   // resume the paused sequence
  }

  // CANCEL,OFF | CANCEL,30 | CANCEL,10  -- abort the run and suppress the column. Works whether or
  // not a fault is held, so it also stops a healthy-but-unwanted run without the global e-stop.
  if (U.startsWith("CANCEL")) {
    int c1 = U.indexOf(',');
    String a = (c1 >= 0) ? U.substring(c1 + 1) : "30";
    a.trim();
    int mode;
    if      (a == "OFF") mode = 0;
    else if (a == "30")  mode = 1;
    else if (a == "10")  mode = 2;
    else { sendSMS("ERR,CANCEL,FORMAT (use CANCEL,OFF | CANCEL,30 | CANCEL,10)"); return; }
    if (!wo.active && !pendingRun.active && !esp2Held) { sendSMS("ERR,CANCEL,NO_RUN"); return; }
    cancelRun(mode, "SMS");
    return;
  }

  // STOP,ALL  (global emergency stop when nothing is held)
  if (U.startsWith("STOP")) {
    enterEmergencyStop(false);
    sendSMS("ACK,STOP,ALL");
    return;
  }
  // SUPA,<projectUrl>,<serviceKey>  -- set Supabase creds (case-sensitive; use raw body, not U).
  // SUPA,CLEAR wipes them. Stored in NVS only -- never committed. Owner-gated by senderIsOwner() above.
  if (U.startsWith("SUPA")) {
    int c1 = b.indexOf(',');
    String rest = (c1 >= 0) ? b.substring(c1 + 1) : "";
    rest.trim();
    if (rest.equalsIgnoreCase("CLEAR")) {
      xSemaphoreTake(netMux, portMAX_DELAY); supaUrl = ""; supaKey = ""; xSemaphoreGive(netMux);
      saveSupa(); sendSMS("ACK,SUPA,CLEAR"); return;
    }
    int c2 = rest.indexOf(',');                          // URL , KEY (key may not contain a comma; JWTs don't)
    if (c2 <= 0) { sendSMS("ERR,SUPA,FORMAT"); return; }
    String u = rest.substring(0, c2); u.trim();
    String k = rest.substring(c2 + 1); k.trim();
    u = supaNormalizeUrl(u);                        // collapse any pasted path down to the origin
    if (!u.startsWith("http") || k.length() < 20) { sendSMS("ERR,SUPA,FORMAT"); return; }
    xSemaphoreTake(netMux, portMAX_DELAY); supaUrl = u; supaKey = k; xSemaphoreGive(netMux);
    saveSupa();
    logEvent("ESP1", "CFG", "SUPA|SET");                 // note: NO url/key in the log
    sendSMS("ACK,SUPA,SET");
    return;
  }
  // FBASE,<rtdbUrl>  -- set the Firebase RTDB base URL (raw body, not U: the URL is case-sensitive).
  // FBASE,CLEAR wipes it; FBASE,ON / FBASE,OFF toggle without touching the URL. NVS only, owner-gated.
  if (U.startsWith("FBASE")) {
    int c1 = b.indexOf(',');
    String rest = (c1 >= 0) ? b.substring(c1 + 1) : "";
    rest.trim();
    if (rest.equalsIgnoreCase("CLEAR")) {
      xSemaphoreTake(netMux, portMAX_DELAY); fbUrl = ""; xSemaphoreGive(netMux);
      saveFb(); sendSMS("ACK,FBASE,CLEAR"); return;
    }
    if (rest.equalsIgnoreCase("ON") || rest.equalsIgnoreCase("OFF")) {
      bool en = rest.equalsIgnoreCase("ON");
      xSemaphoreTake(netMux, portMAX_DELAY); firebaseEnabled = en; xSemaphoreGive(netMux);
      saveFb();
      logEvent("ESP1", "CFG", String("FBASE|") + (en ? "ON" : "OFF"));
      sendSMS(en ? "ACK,FBASE,ON" : "ACK,FBASE,OFF");
      return;
    }
    // FBASE,SIGNOUT -- wipe every credential and the live token. The dashboard goes read-dead
    // until re-provisioned; use this if the device is lost or the account is rotated.
    if (rest.equalsIgnoreCase("SIGNOUT")) {
      xSemaphoreTake(netMux, portMAX_DELAY);
      fbPassword = ""; fbRefresh = ""; fbIdToken = "";
      xSemaphoreGive(netMux);
      fbTokenExpiryMs = 0; fbAuthOk = false; fbUid = ""; fbAuthErr = "";
      saveFbAuth();
      logEvent("ESP1", "CFG", "FBASE|SIGNOUT");
      sendSMS("ACK,FBASE,SIGNOUT");
      return;
    }
    // FBASE,STATUS -- why is it not connecting? Reports the blocking precondition, the last
    // Google error, and the UID. Safe to send: no secret is ever echoed.
    if (rest.equalsIgnoreCase("STATUS")) {
      String m = "FB,";
      m += fbAuthOk ? "SIGNEDIN" : "NOAUTH";
      m += ",http=" + String(fbAuthHttp);
      if (!firebaseEnabled)                                 m += ",OFF";
      else if (fbApiKey.length() < 8)                       m += ",NOKEY";
      else if (!fbEmail.length())                           m += ",NOMAIL";
      else if (!fbRefresh.length() && !fbPassword.length()) m += ",NOPASS";
      else if (!wifiConnected)                              m += ",NOWIFI";
      else if (fbUrl.length() < 8)                          m += ",NOURL";
      m += fbPinCa ? ",TLS=pin" : ",TLS=insecure";
      if (fbUid.length())     m += ",uid=" + fbUid;
      if (fbAuthErr.length()) m += "," + fbAuthErr.substring(0, 60);
      sendSMS(m);
      return;
    }
    // FBASE,TLS,ON|OFF -- toggle certificate validation. OFF is a DIAGNOSTIC: if sign-in only
    // works with it off, the CA pin is the problem, not your credentials. Put it back on.
    if (rest.startsWith("TLS,") || rest.startsWith("tls,")) {
      String v = rest.substring(4); v.trim();
      if (!v.equalsIgnoreCase("ON") && !v.equalsIgnoreCase("OFF")) { sendSMS("ERR,FBASE,FORMAT"); return; }
      xSemaphoreTake(netMux, portMAX_DELAY); fbPinCa = v.equalsIgnoreCase("ON"); xSemaphoreGive(netMux);
      fbResetTls();
      fbSignInPending = true; fbAuthFails = 0; fbAuthNextTryMs = 0;
      saveFbAuth();
      logEvent("ESP1", "CFG", String("FBASE|TLS|") + (fbPinCa ? "PIN" : "INSECURE"));
      sendSMS(fbPinCa ? "ACK,FBASE,TLS,ON" : "ACK,FBASE,TLS,OFF");
      return;
    }
    // FBASE,APIKEY,<webApiKey>
    if (rest.startsWith("APIKEY,") || rest.startsWith("apikey,")) {
      String k = rest.substring(7); k.trim();
      if (k.length() < 8) { sendSMS("ERR,FBASE,FORMAT"); return; }
      xSemaphoreTake(netMux, portMAX_DELAY); fbApiKey = k; xSemaphoreGive(netMux);
      saveFbAuth();
      logEvent("ESP1", "CFG", "FBASE|APIKEY");          // note: NO key in the log
      sendSMS("ACK,FBASE,APIKEY");
      return;
    }
    // FBASE,AUTH,<email>,<password> -- provisioning. Password is used once to mint a refresh
    // token, then FBASE,SIGNOUT or the portal's Forget button can remove it.
    if (rest.startsWith("AUTH,") || rest.startsWith("auth,")) {
      String rem = rest.substring(5);
      int c2 = rem.indexOf(',');
      if (c2 <= 0) { sendSMS("ERR,FBASE,FORMAT"); return; }
      String em = rem.substring(0, c2); em.trim();
      String pw = rem.substring(c2 + 1); pw.trim();
      if (em.indexOf('@') < 0 || pw.length() < 6) { sendSMS("ERR,FBASE,FORMAT"); return; }
      xSemaphoreTake(netMux, portMAX_DELAY);
      fbEmail = em; fbPassword = pw; fbRefresh = "";   // new creds void any stored refresh token
      xSemaphoreGive(netMux);
      saveFbAuth();
      fbSignInPending = true; fbAuthFails = 0; fbAuthNextTryMs = 0;
      logEvent("ESP1", "CFG", "FBASE|AUTH");           // note: NO email/password in the log
      sendSMS("ACK,FBASE,AUTH");
      return;
    }
    String u = supaNormalizeUrl(rest);                   // same origin-only reduction as SUPA
    if (!u.startsWith("http")) { sendSMS("ERR,FBASE,FORMAT"); return; }
    xSemaphoreTake(netMux, portMAX_DELAY); fbUrl = u; firebaseEnabled = true; xSemaphoreGive(netMux);
    saveFb();
    logEvent("ESP1", "CFG", "FBASE|SET");                // note: NO url in the log
    sendSMS("ACK,FBASE,SET");
    return;
  }
  // UPLOAD             -> queue yesterday's CSV.  UPLOAD,YYYYMMDD -> queue that specific day.
  if (U.startsWith("UPLOAD")) {
    long stamp = 0;
    int c1 = b.indexOf(',');
    if (c1 >= 0) { String d = b.substring(c1 + 1); d.trim(); if (d.length() == 8) stamp = d.toInt(); }
    else if (rtcOk && currentDayStamp > 0) {             // no arg -> yesterday
      DateTime y = rtc.now() - TimeSpan(1, 0, 0, 0);
      stamp = dayStamp(y);
    }
    if (supaUrl.length() == 0) { sendSMS("ERR,UPLOAD,NOCREDS"); return; }
    if (stamp <= 0)                    { sendSMS("ERR,UPLOAD,DAY"); return; }
    if (rtcOk && stamp == currentDayStamp) { sendSMS("ERR,UPLOAD,TODAY"); return; }  // never the open file
    char path[16]; snprintf(path, sizeof(path), "/%08ld.CSV", stamp);
    bool exists;
    { bool got = sdTake(200); exists = got && SD.exists(path); if (got) sdGive(); }
    if (!exists) { sendSMS(String("ERR,UPLOAD,NOFILE,") + stamp); return; }
    manualUploadStamp = stamp;                           // uploadTick prioritizes this
    sendSMS(String("ACK,UPLOAD,") + stamp);
    return;
  }
  // STATUS
  if (U == "STATUS") {
    if (sysState == ACTIVE_STATE) {
      String prog = (wo.active && wo.colIdx >= 0)
        ? String("ACK,BUSY,") + (wo.fertigate ? "FERTIGATION" : "IRRIGATION") + ",COL_" + COL_TAG[wo.colIdx]
        : String("ACK,BUSY,ACTIVE");
      sendSMS(prog);
      reportPending = true;                 // deferred full report (sec.12.1.3.1)
    } else {
      sendDailyReport();
    }
    return;
  }
  // NET -- WiFi/ThingSpeak link status (Part B)
  if (U == "NET") {
    String ss; xSemaphoreTake(netMux, portMAX_DELAY); ss = wifiSsid; xSemaphoreGive(netMux);
    String r = "NET,WiFi:" + String(portalActive ? "PORTAL" : !wifiEnabled ? "OFF" : wifiConnected ? "OK" : "DOWN");
    if (portalActive) r += ",AP:" + String(AP_SSID) + ",IP" + String(portalApIp);
    else if (wifiConnected) r += ",RSSI" + String(wifiRssiVal) + ",IP" + String(wifiIpStr);
    r += ",TS:" + String(lastTsOk ? "ok" : "--");
    if (lastTsUploadMs) r += ",age" + String((millis() - lastTsUploadMs) / 1000) + "s";
    r += ",SSID:" + (ss.length() ? ss : String("-"));
    sendSMS(r);
    return;
  }
  // SUMMARY [,day]  and  FULL SUMMARY [,day]  -- SD-log digest (extends sec.12.2).
  //   SUMMARY      -> day's AVERAGES (NPK + moisture per column, power) for the paper.
  //   FULL SUMMARY -> uncapped HOURLY record + deduped errors + run/dose events + peaks.
  //   day = (none)=today | YESTERDAY | YYYYMMDD | NODATE (RTC-dead 00:00:00 file).
  // Same deferred behaviour as STATUS during ACTIVE (immediate ack, run when idle).
  {
    bool isFull = U.startsWith("FULL");
    bool isSum  = isFull || U == "SUMMARY" || U.startsWith("SUMMARY,") || U.startsWith("SUMMARY ");
    if (isSum) {
      String arg = U;                       // strip the keyword(s), leave the optional day token
      if (isFull) arg.replace("FULL", " ");
      arg.replace("SUMMARY", " ");
      arg.replace(",", " ");
      arg.trim();
      long target = resolveSummaryDay(arg);
      if (target == -2) { sendSMS("ERR,SUMDATE"); return; }
      SumMode mode = isFull ? SUM_FULL : SUM_SHORT;
      const char *ackName = isFull ? "FULLSUMMARY" : "SUMMARY";
      if (sysState == ACTIVE_STATE || portalActive) {   // portal owns the SD -> defer, run when it closes
        sendSMS(String("ACK,BUSY,") + ackName);
        summaryReplyTo = replyTarget;
        pendingSumMode = mode; pendingSumTarget = target;
        summaryPending = true;
      } else if (summaryStage != SUM_IDLE) {
        sendSMS(String("ACK,") + ackName + ",BUSY");   // a job is already running -> don't stack
      } else {
        summaryReplyTo = replyTarget;
        startSummaryJob(mode, target);
      }
      return;
    }
  }
  // tokenize
  const int MAXT = 14; String tok[MAXT]; int n = 0, start = 0;
  for (int i = 0; i <= b.length() && n < MAXT; i++) {
    if (i == b.length() || b[i] == ',') { tok[n++] = b.substring(start, i); start = i + 1; }
  }
  if (n < 2) { sendSMS("ERR,PARSE"); return; }
  String k0 = tok[0]; k0.toUpperCase();

  // resolve column index from COL_A/B/C
  auto colIdx = [&](const String &s) -> int {
    String t = s; t.toUpperCase();
    if (t == "COL_A") return 0; if (t == "COL_B") return 1; if (t == "COL_C") return 2;
    return -1;
  };

  if (k0 == "MODE") {
    int c = colIdx(tok[1]); String m = tok[2]; m.toUpperCase();
    if (c < 0) { sendSMS("ERR,COL"); return; }
    if (m == "AUTO")            col[c].mode = MODE_AUTO;
    else if (m == "IRRIGATION_ONLY") col[c].mode = MODE_IRRIGATION_ONLY;
    else { sendSMS("ERR,MODE"); return; }
    saveColumn(c);
    sendSMS(String("ACK,MODE,COL_") + COL_TAG[c] + "," + m);
    return;
  }
  if (k0 == "NAME") {
    int c = colIdx(tok[1]);
    if (c < 0 || n < 3) { sendSMS("ERR,NAME"); return; }
    strncpy(col[c].name, tok[2].c_str(), sizeof(col[c].name) - 1);
    col[c].name[sizeof(col[c].name) - 1] = 0;
    saveColumn(c);
    sendSMS(String("ACK,NAME,COL_") + COL_TAG[c] + "," + col[c].name);
    return;
  }
  if (k0 == "SET") {
    int c = colIdx(tok[1]);
    if (c < 0) { sendSMS("ERR,COL"); return; }
    String t2 = tok[2]; t2.toUpperCase();
    if (t2 == "PRESET") {                   // SET,COL_A,PRESET,NAME
      String pn = tok[3]; pn.toUpperCase();
      for (int p = 0; p < NUM_PRESETS; p++) {
        if (pn == CROP_PRESETS[p].name) {
          col[c].targetN = CROP_PRESETS[p].N; col[c].targetP = CROP_PRESETS[p].P;
          col[c].targetK = CROP_PRESETS[p].K; col[c].targetPH = CROP_PRESETS[p].pH;
          saveColumn(c);
          sendSMS(String("ACK,SET,COL_") + COL_TAG[c] + ",PRESET," + CROP_PRESETS[p].name);
          return;
        }
      }
      sendSMS("ERR,PRESET"); return;
    }
    // explicit: SET,COL_A,N,150,P,40,K,200,pH,5.8  (key/value pairs)
    for (int i = 2; i + 1 < n; i += 2) {
      String key = tok[i]; key.toUpperCase(); float v = tok[i + 1].toFloat();
      if (key == "N") col[c].targetN = v;
      else if (key == "P") col[c].targetP = v;
      else if (key == "K") col[c].targetK = v;
      else if (key == "PH") col[c].targetPH = v;
    }
    saveColumn(c);
    sendSMS(String("ACK,SET,COL_") + COL_TAG[c]);
    return;
  }
  // WIFI,<ssid>,<pass>  -- set WiFi credentials (owner-only). Password is the remainder
  // after the 2nd comma, so it may contain commas/spaces (SSID may not). (Part B)
  if (k0 == "WIFI") {
    if (!senderIsOwner()) { sendSMS("ERR,AUTH"); return; }
    // WIFI,ON / WIFI,OFF -- master switch (mirrors the Settings > WiFi toggle). Checked before the
    // ssid/pass parse; a real SSID of literally "ON"/"OFF" with no password is not a valid pair anyway.
    if (n == 2 && (tok[1] == "ON" || tok[1] == "OFF")) {
      wifiEnabled = (tok[1] == "ON"); saveWifiEn();
      sendSMS(String("ACK,WIFI,") + tok[1]);
      return;
    }
    int p1 = b.indexOf(','), p2 = (p1 >= 0) ? b.indexOf(',', p1 + 1) : -1;
    if (p1 < 0 || p2 < 0) { sendSMS("ERR,WIFI"); return; }
    String ss = b.substring(p1 + 1, p2), pw = b.substring(p2 + 1);
    xSemaphoreTake(netMux, portMAX_DELAY); wifiSsid = ss; wifiPass = pw; xSemaphoreGive(netMux);
    saveWifi(); wifiCredsChanged = true;
    sendSMS("ACK,WIFI," + ss);
    return;
  }
  // WIFIPORTAL [,STOP] -- start/stop the SoftAP WiFi-setup portal (owner-only). Join AP_SSID from a
  // phone, browse to the AP IP, pick a network + type the password, Save. (Part B)
  if (k0 == "WIFIPORTAL") {
    if (!senderIsOwner()) { sendSMS("ERR,AUTH"); return; }
    if (n >= 2 && tok[1] == "STOP") { portalCancel = true; sendSMS("ACK,WIFIPORTAL,STOP"); return; }
    portalRequested = true;
    sendSMS(String("ACK,WIFIPORTAL,join ") + AP_SSID + " pw " + AP_PASS + " -> http://192.168.4.1");
    return;
  }
  // TSKEY,<1|2>,<writeApiKey>  -- set a ThingSpeak channel write key (owner-only). (Part B)
  if (k0 == "TSKEY") {
    if (!senderIsOwner()) { sendSMS("ERR,AUTH"); return; }
    if (n < 3) { sendSMS("ERR,TSKEY"); return; }
    int ch = tok[1].toInt(); String key = tok[2];
    if (ch < 1 || ch > 3) { sendSMS("ERR,TSCH"); return; }
    xSemaphoreTake(netMux, portMAX_DELAY);
    if (ch == 1) tsKey1 = key; else if (ch == 2) tsKey2 = key; else tsKey3 = key;
    xSemaphoreGive(netMux);
    saveTsKey();
    sendSMS("ACK,TSKEY," + String(ch));
    return;
  }
  // THRESH,<start%>,<stop%>,<gap>  -- soil start/stop thresholds + fertigation gap (also on the LCD
  // Settings > Thresholds editor). Clamps stop>start like the editor. (Owner-gated above.)
  if (k0 == "THRESH") {
    if (n < 4) { sendSMS("ERR,THRESH"); return; }
    int s  = clampi(tok[1].toInt(), 0, 100);
    int st = clampi(tok[2].toInt(), 0, 100);
    int g  = clampi((int)tok[3].toFloat(), 0, 500);
    if (st <= s) st = clampi(s + 1, 0, 100);
    soilStartPct = s; soilStopPct = st; fertGap = g;
    saveThresholds();
    sendSMS("ACK,THRESH," + String(s) + "," + String(st) + "," + String(g));
    return;
  }
  sendSMS("ERR,CMD");
}

// Enqueue only -- non-blocking. The AT exchange is driven by gsmTxTick() so alert
// and fault paths never stall the loop.
void sendSMS(const String &msg) {
  if (smsMute) return;                                 // suppress ACK/ERR for portal-replayed config commands
  if (smsCount >= SMS_QUEUE_SIZE) {                     // queue full: drop oldest, keep newest alert
    smsHead = (smsHead + 1) % SMS_QUEUE_SIZE;
    smsCount--;
    logEvent("GSM", "GSM", "TX|DROP_QUEUE_FULL");
  }
  smsQueue[smsTail] = msg;
  // Reply to the inbound sender when one is set (STATUS/ACK); else the owner number.
  smsTo[smsTail]    = replyTarget.length() ? replyTarget : PHONE_NUMBER;
  smsTail = (smsTail + 1) % SMS_QUEUE_SIZE;
  smsCount++;
}

// Non-blocking outbound SMS state machine. Owns the SIM UART only while a send is
// in progress; pollGSM() yields during that window (see its guard).
void gsmTxTick() {
  switch (gtx) {
    case GTX_IDLE:
      if (smsCount == 0) return;
      gtxMsg  = smsQueue[smsHead];
      gtxTo   = smsTo[smsHead];
      smsHead = (smsHead + 1) % SMS_QUEUE_SIZE;
      smsCount--;
      while (simSerial.available()) gsmFeedInbound((char)simSerial.read());  // parse, don't drop (B5)
      simSerial.println("AT+CMGF=1");                    // text mode
      gtxMs = millis();
      gtx = GTX_CMGF_WAIT;
      break;

    case GTX_CMGF_WAIT:
      while (simSerial.available()) gsmFeedInbound((char)simSerial.read());  // drain inbound (B5)
      if (millis() - gtxMs < GSM_CMGF_SETTLE_MS) return;
      simSerial.print("AT+CMGS=\""); simSerial.print(gtxTo.length() ? gtxTo : PHONE_NUMBER); simSerial.println("\"");
      gtxMs = millis();
      gtx = GTX_PROMPT_WAIT;
      break;

    case GTX_PROMPT_WAIT:
      while (simSerial.available()) {
        char c = (char)simSerial.read();
        if (c == '>') {                                 // module ready for body
          simSerial.print(gtxMsg);
          simSerial.write(26);                          // CTRL+Z -> send
          logEvent("GSM", "GSM", "TX|" + gtxMsg);
          gtxResultSeen = false;                        // arm result capture for this send (+CMGS / +CMS ERROR)
          gtxMs = millis();
          gtx = GTX_BODY_SETTLE;
          return;
        }
        gsmFeedInbound(c);                              // non-prompt bytes may be an inbound +CMT (B5)
      }
      if (millis() - gtxMs > GSM_PROMPT_TIMEOUT_MS) {    // no '>' -> abandon this message
        logEvent("GSM", "GSM", "TX|FAIL_NO_PROMPT");
        gtx = GTX_IDLE;
      }
      break;

    case GTX_BODY_SETTLE:                                // wait for the network's send result, then next send
      while (simSerial.available()) gsmFeedInbound((char)simSerial.read());  // parse +CMGS/+CMS ERROR (B5)
      if (gtxResultSeen) { gtx = GTX_IDLE; break; }     // +CMGS / +CMS ERROR already logged the outcome
      if (millis() - gtxMs >= GSM_SEND_RESULT_TIMEOUT_MS) {   // hard cap: silent modem can't stall the queue
        logEvent("GSM", "GSM", "TX_UNKNOWN");
        gtx = GTX_IDLE;
      }
      break;
  }
}

void sendDailyReport() {
  long ds = (currentDayStamp > 0) ? currentDayStamp : 0;
  String r = "RPT," + String(ds);
  for (int c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c]) continue;
    r += String(",") + COL_TAG[c] + ":" + (strlen(col[c].name) ? col[c].name : "-");
    r += "|N" + String((int)col[c].targetN) + "P" + String((int)col[c].targetP) + "K" + String((int)col[c].targetK);
    r += "|pH" + String(col[c].targetPH, 1);
    r += "|W" + String(waterUsedToday[c], 1) + "|Nu" + String(nutrientUsedToday[c], 1);
  }
  r += ",FLT,C" + String(faultsToday[0]) + "M" + String(faultsToday[1]) + "m" + String(faultsToday[2]);
  r += ",RST," + String(nanoResetsToday);
  r += ",ST," + String(stateName(sysState));
  r += ",BAT," + String(battV, 1) + "V";
  // Daily energy (sec.12.1.3): integrated discharge (CONS) + charge (CHG), Wh.
  r += ",CONS," + String((int)(energyConsumedWh + 0.5)) + "Wh";
  r += ",CHG," + String((int)(energyChargedWh + 0.5)) + "Wh";
  sendSMS(r);
  reportPending = false;
}

/* =============================================================================
 *  SUMMARY  --  today's-activity digest from the SD log (incremental, sec.25)
 * ========================================================================== */
// Split a CSV detail field by '|' into out[]; returns the token count.
static int splitPipe(const String &s, String *out, int maxn) {
  int n = 0, start = 0;
  for (int i = 0; i <= (int)s.length() && n < maxn; i++) {
    if (i == (int)s.length() || s[i] == '|') { out[n++] = s.substring(start, i); start = i + 1; }
  }
  return n;
}
static int colFromTag(const String &t) {
  for (int j = 0; j < NUM_COLUMNS; j++) if (t == String(COL_TAG[j])) return j;
  return -1;
}
// Record an error code once (with its first timestamp) for the FULL dedup list.
static void sumAddError(const String &code, const String &hhmm) {
  for (int i = 0; i < sumErrN; i++) if (sumErrCode[i] == code) return;
  if (sumErrN < SUM_MAX_ERRORS) { sumErrCode[sumErrN] = code; sumErrTs[sumErrN] = hhmm; sumErrN++; }
}

// Parse one CSV row (timestamp,source,type,detail). Numeric SENSOR/PWR rows feed the
// day-average accumulators AND the 24 hourly buckets (FULL); FAULT/RESET/DOSE/ACT/config
// rows feed counts, the deduped error list, and the run/dose event list.
static void summaryParseLine(const String &ln) {
  if (ln.length() < 12) return;
  int c1 = ln.indexOf(',');            if (c1 < 0) return;
  int c2 = ln.indexOf(',', c1 + 1);    if (c2 < 0) return;
  int c3 = ln.indexOf(',', c2 + 1);    if (c3 < 0) return;
  String ts     = ln.substring(0, c1);
  String type   = ln.substring(c2 + 1, c3);
  String detail = ln.substring(c3 + 1);
  String hhmm   = (ts.length() >= 16) ? (ts.substring(11, 13) + ts.substring(14, 16)) : "----";
  int hh = (ts.length() >= 13) ? ts.substring(11, 13).toInt() : 0;
  if (hh < 0 || hh > 23) hh = 0;          // NODATE rows (00:00:00) -> hour 0

  // ---- numeric rows: averages + hourly buckets ----
  if (type == "SENSOR") {
    String f[10]; int nf = splitPipe(detail, f, 10);
    if (nf >= 9 && f[0] == "NPK") {                 // NPK|tag|moist|temp|EC|pH|N|P|K
      int c = colFromTag(f[1]);
      if (c >= 0 && f[6] != "-1") {
        float N = f[6].toFloat(), P = f[7].toFloat(), K = f[8].toFloat();
        sumNsum[c] += N; sumPsum[c] += P; sumKsum[c] += K; sumNPKc[c]++;
        hrNsum[hh][c] += N; hrPsum[hh][c] += P; hrKsum[hh][c] += K; hrNPKc[hh][c]++;
      }
      return;
    }
    if (nf >= 2 && f[0] == "SOIL") {                // SOIL|A=41|B=53|C=DISABLED
      for (int i = 1; i < nf; i++) {
        int eq = f[i].indexOf('=');
        if (eq < 1) continue;
        int c = colFromTag(f[i].substring(0, eq));
        String v = f[i].substring(eq + 1);
        if (c < 0 || v == "DISABLED" || v == "-1") continue;
        float m = v.toFloat();
        sumMoistSum[c] += m; sumMoistC[c]++;
        hrMoistSum[hh][c] += m; hrMoistC[hh][c]++;
      }
      return;
    }
    if (nf >= 2 && f[0] == "ENV" && f[1] != "-1") { // ENV|temp|hum  (peak temp)
      float t = f[1].toFloat(); if (t > sumMaxTemp) sumMaxTemp = t;
    }
    return;                                          // TANK/LIGHT not aggregated
  }
  if (type == "PWR") {                               // INA226|V=..|I=..|P=..|CONS=..Wh|CHG=..Wh
    String f[8]; int nf = splitPipe(detail, f, 8);
    float v = 0, p = 0; bool haveV = false, haveP = false;
    for (int i = 1; i < nf; i++) {
      if      (f[i].startsWith("V="))   { v = f[i].substring(2).toFloat(); haveV = true; }
      else if (f[i].startsWith("P="))   { p = f[i].substring(2).toFloat(); haveP = true; }
      else if (f[i].startsWith("CONS=")) sumConsWh = f[i].substring(5).toFloat();
      else if (f[i].startsWith("CHG="))  sumChgWh  = f[i].substring(4).toFloat();
    }
    if (haveV) { sumBattVsum += v; if (v < sumMinBattV) sumMinBattV = v; hrBattVsum[hh] += v; }
    if (haveP) { sumBattWsum += p; if (p > sumPeakW)    sumPeakW    = p; hrBattWsum[hh] += p; }
    if (haveV || haveP) { sumBattC++; hrBattC[hh]++; }
    return;
  }

  // ---- event / significant rows ----
  String code;
  if (type == "FAULT") {
    if      (detail.startsWith("CRIT")) sumFltC++;
    else if (detail.startsWith("MAJ"))  sumFltM++;
    else if (detail.startsWith("MIN"))  sumFltm++;
    else return;
    { String f[4]; int nf = splitPipe(detail, f, 4); if (nf >= 2) sumAddError(f[1], hhmm); }  // dedup code
    code = "F:" + detail;
  } else if (type == "RESET") { sumRst++; { String f[4]; int nf = splitPipe(detail, f, 4); if (nf >= 1) sumAddError(String("RST_") + f[0], hhmm); } code = "R:" + detail; }
  else if   (type == "DOSE")  {                      // NUT_A|target|measured|COL_A
    sumDose++;
    String f[4]; int nf = splitPipe(detail, f, 4);
    if (nf >= 4) { int c = colFromTag(f[3].substring(f[3].length() - 1)); if (c >= 0) sumNutTot[c] += f[2].toFloat(); }
    code = "D:" + detail;
  }
  else if   (type == "ACT") {                        // IRRIGATION|START|COL_A or ...|STOP|COL_A|W=3.2
    if      (detail.startsWith("FERTIGATION") && detail.indexOf("START") >= 0) sumFert++;
    else if (detail.startsWith("IRRIGATION")  && detail.indexOf("START") >= 0) sumIrr++;
    int wi = detail.indexOf("W=");
    if (wi >= 0) {                                    // STOP carries delivered litres (log enhancement)
      String f[6]; int nf = splitPipe(detail, f, 6);
      int c = -1; for (int i = 0; i < nf; i++) if (f[i].startsWith("COL_")) c = colFromTag(f[i].substring(f[i].length() - 1));
      if (c >= 0) sumWaterTot[c] += detail.substring(wi + 2).toFloat();
    }
    code = "A:" + detail;
  } else if (type == "STATE" && detail.indexOf("_SET") >= 0) {
    code = "C:" + detail;                            // config change
  } else {
    return;
  }

  code.replace("|", ":");
  if (code.length() > 26) code = code.substring(0, 26);
  uint8_t cap = (sumMode == SUM_FULL) ? FULL_MAX_EVENTS : SUMMARY_MAX_EVENTS;
  if (sumEvtCount < cap) { sumEvents += "|" + hhmm + " " + code; sumEvtCount++; }
  else                   { sumTruncated = true; }
}

// Resolve a SUMMARY day token to a yyyymmdd stamp (0 = NODATE file, -2 = invalid).
static long resolveSummaryDay(const String &arg) {
  String a = arg; a.trim(); a.toUpperCase();
  if (a.length() == 0 || a == "TODAY") return (rtcOk && currentDayStamp > 0) ? currentDayStamp : 0;
  if (a == "NODATE")                   return 0;
  if (a == "YESTERDAY") {
    if (!rtcOk) return 0;
    DateTime y = rtc.now() - TimeSpan(1, 0, 0, 0);     // correct month/year rollover
    return dayStamp(y);
  }
  if (a.length() == 8) { long v = a.toInt(); if (v > 20000000L) return v; }   // explicit YYYYMMDD
  return -2;                                            // unrecognized
}

// SD cross-core lock (sdMux). Timed take so the core-1 loop never blocks past the task WDT while the
// core-0 portal holds the card; NULL sdMux (pre-init) -> proceed unlocked (single-threaded at that point).
static bool sdTake(uint32_t ms) { return (!sdMux) || xSemaphoreTake(sdMux, pdMS_TO_TICKS(ms)) == pdTRUE; }
static void sdGive()            { if (sdMux) xSemaphoreGive(sdMux); }

// Enqueue an SMS-format config command from the core-0 portal for the core-1 loop to replay via
// handleSms (muted). Returns false if the 6-slot ring is full. Producer = core 0, consumer = core 1.
static bool portalCfgEnq(const String &cmd) {
  // Publish under netMux so the String content is barriered for the core-1 drain (matches the
  // WiFi-creds hand-off pattern). The drain reads portalCfgQ under netMux too.
  xSemaphoreTake(netMux, portMAX_DELAY);
  uint8_t next = (uint8_t)((cfgTail + 1) % 6);
  bool ok = (next != cfgHead);
  if (ok) { portalCfgQ[cfgTail] = cmd; cfgTail = next; }
  xSemaphoreGive(netMux);
  return ok;
}

void startSummaryJob(SumMode mode, long targetStamp) {
  if (summaryStage != SUM_IDLE) return;  // one job at a time
  if (portalActive) return;              // portal owns the SD; the deferred trigger retries when it closes
  sumMode = mode;
  summaryTargetStamp = targetStamp;
  summaryStage = SUM_OPEN;
}

static String sumDayLabel() {
  return (summaryTargetStamp > 0) ? String(summaryTargetStamp) : String("NODATE");
}

// Clear every accumulator + hourly bucket before a fresh parse.
static void sumResetAccumulators() {
  sumFltC = sumFltM = sumFltm = 0; sumRst = sumFert = sumIrr = sumDose = 0;
  sumEvtCount = 0; sumTruncated = false; sumEvents = ""; sumPartial = ""; sumErrN = 0;
  sumBattVsum = sumBattWsum = 0; sumBattC = 0; sumConsWh = sumChgWh = 0;
  sumMaxTemp = -1000; sumMinBattV = 100000; sumPeakW = 0;
  for (int c = 0; c < NUM_COLUMNS; c++) {
    sumNsum[c] = sumPsum[c] = sumKsum[c] = 0; sumNPKc[c] = 0;
    sumMoistSum[c] = 0; sumMoistC[c] = 0; sumWaterTot[c] = 0; sumNutTot[c] = 0;
  }
  for (int h = 0; h < 24; h++) {
    hrBattVsum[h] = hrBattWsum[h] = 0; hrBattC[h] = 0;
    for (int c = 0; c < NUM_COLUMNS; c++) {
      hrNsum[h][c] = hrPsum[h][c] = hrKsum[h][c] = 0; hrNPKc[h][c] = 0;
      hrMoistSum[h][c] = 0; hrMoistC[h][c] = 0;
    }
  }
}

// SHORT SUMMARY = the day's averages (paper-ready), into sumReport.
static void buildShortReport() {
  String s = "SUM," + sumDayLabel();
  for (int c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c]) continue;
    s += String(" ") + COL_TAG[c] + ":";
    if (sumNPKc[c]) s += "N" + String((int)(sumNsum[c] / sumNPKc[c] + 0.5)) + "P" + String((int)(sumPsum[c] / sumNPKc[c] + 0.5)) + "K" + String((int)(sumKsum[c] / sumNPKc[c] + 0.5));
    else            s += "N-P-K-";
    if (sumMoistC[c]) s += "M" + String((int)(sumMoistSum[c] / sumMoistC[c] + 0.5));
    else              s += "M-";
  }
  if (sumBattC) s += " BAT" + String(sumBattVsum / sumBattC, 1) + "V " + String(sumBattWsum / sumBattC, 1) + "W";
  s += " CONS" + String((int)(sumConsWh + 0.5)) + " CHG" + String((int)(sumChgWh + 0.5)) + "Wh";
  float wtot = 0, ntot = 0; for (int c = 0; c < NUM_COLUMNS; c++) { wtot += sumWaterTot[c]; ntot += sumNutTot[c]; }
  s += " H2O" + String(wtot, 1) + "L NUT" + String(ntot, 0) + "mL";
  s += " FLT C" + String(sumFltC) + "M" + String(sumFltM) + "m" + String(sumFltm);
  s += " RST" + String(sumRst) + " IRR" + String(sumIrr) + " FERT" + String(sumFert) + " DOSE" + String(sumDose);
  if (!sdOk) s += " (noSD)";
  sumReport = s;
}

// FULL SUMMARY = hourly nutrient/moisture/power record + deduped errors + events + peaks.
static void buildFullReport() {
  String s = "FULL," + sumDayLabel() + " hourly";
  for (int h = 0; h < 24; h++) {
    bool any = (hrBattC[h] > 0);
    for (int c = 0; c < NUM_COLUMNS; c++) if (hrNPKc[h][c] || hrMoistC[h][c]) any = true;
    if (!any) continue;                                // skip hours with no samples
    char hb[6]; snprintf(hb, sizeof(hb), "%02dh", h);
    s += String("\n") + hb;
    for (int c = 0; c < NUM_COLUMNS; c++) {
      if (!COLUMN_ENABLED[c]) continue;
      s += String(" ") + COL_TAG[c];
      if (hrNPKc[h][c]) s += " N" + String((int)(hrNsum[h][c] / hrNPKc[h][c] + 0.5)) + " P" + String((int)(hrPsum[h][c] / hrNPKc[h][c] + 0.5)) + " K" + String((int)(hrKsum[h][c] / hrNPKc[h][c] + 0.5));
      if (hrMoistC[h][c]) s += " M" + String((int)(hrMoistSum[h][c] / hrMoistC[h][c] + 0.5));
    }
    if (hrBattC[h]) s += " | " + String(hrBattVsum[h] / hrBattC[h], 1) + "V " + String(hrBattWsum[h] / hrBattC[h], 1) + "W";
  }
  if (sumErrN) { s += "\nERR:"; for (int i = 0; i < sumErrN; i++) s += " " + sumErrTs[i] + " " + sumErrCode[i]; }
  else         { s += "\nERR: none"; }
  if (sumEvents.length()) { String e = sumEvents; e.replace("|", "\n"); s += "\nEVT:" + e; if (sumTruncated) s += "\n+more evt@SD"; }
  float wtot = 0, ntot = 0; for (int c = 0; c < NUM_COLUMNS; c++) { wtot += sumWaterTot[c]; ntot += sumNutTot[c]; }
  s += "\nPEAK Tmax" + (sumMaxTemp > -999 ? String(sumMaxTemp, 1) : String("-"))
     + " Vmin" + (sumMinBattV < 99999 ? String(sumMinBattV, 1) : String("-"))
     + " Wpk" + String(sumPeakW, 1);
  s += " H2O" + String(wtot, 1) + "L NUT" + String(ntot, 0) + "mL FLT" + String(sumFltC + sumFltM + sumFltm) + " RST" + String(sumRst);
  if (!sdOk) s += "\n(noSD)";
  sumReport = s;
}

// Non-blocking FSM: open -> incremental read (bounded/tick) -> build -> stream SMS paced
// to the queue so an uncapped FULL report never overflows the 8-slot queue or stalls the WDT.
void summaryTick() {
  const int SEG = 140;                                  // each SMS under the 160-char GSM7 limit
  switch (summaryStage) {
    case SUM_IDLE: return;

    case SUM_OPEN: {
      sumResetAccumulators();
      if (!sdOk) { summaryStage = SUM_BUILD; return; }   // no SD -> build an empty report
      char fname[16];
      if (summaryTargetStamp > 0) snprintf(fname, sizeof(fname), "/%08ld.CSV", summaryTargetStamp);
      else                        strcpy(fname, "/NODATE.CSV");
      if (!sdTake(200)) return;                          // portal holds SD -> retry this stage next tick
      summaryFile = SD.open(fname, FILE_READ);
      sdGive();
      summaryStage = SUM_READ;                           // SUM_READ tolerates a failed open
      return;
    }

    case SUM_READ: {
      if (!summaryFile) { summaryStage = SUM_BUILD; return; }
      if (!sdTake(200)) return;                          // portal holds SD -> retry next tick (handle stays open)
      uint16_t lines = 0;
      while (summaryFile.available() && lines < SUMMARY_LINES_PER_TICK) {
        char c = (char)summaryFile.read();
        if (c == '\n')      { summaryParseLine(sumPartial); sumPartial = ""; lines++; }
        else if (c != '\r') { if (sumPartial.length() < 200) sumPartial += c; }
      }
      bool eof = !summaryFile.available();
      sdGive();
      if (eof) {                                         // EOF: flush a trailing unterminated line
        if (sumPartial.length()) { summaryParseLine(sumPartial); sumPartial = ""; }
        summaryStage = SUM_BUILD;
      }
      return;
    }

    case SUM_BUILD: {
      if (summaryFile) { if (!sdTake(200)) return; summaryFile.close(); sdGive(); }
      if (sumMode == SUM_FULL) buildFullReport(); else buildShortReport();
      int len = sumReport.length();
      sumSegTotal = (len + SEG - 1) / SEG; if (sumSegTotal < 1) sumSegTotal = 1;
      uint8_t capN = (sumMode == SUM_FULL) ? FULL_SUMMARY_MAX_SMS : SUMMARY_MAX_SMS;
      if (capN > 0 && sumSegTotal > capN) sumSegTotal = capN;   // 0 = unlimited (FULL by request)
      sumSegIdx = 0;
      summaryStage = SUM_STREAM;
      return;
    }

    case SUM_STREAM: {
      if (sumSegIdx >= sumSegTotal) {                    // done
        sumReport = ""; summaryPending = false; summaryStage = SUM_IDLE; return;
      }
      if (smsCount >= SMS_QUEUE_SIZE - 1) return;        // queue nearly full -> wait (paced, no drops)
      int len = sumReport.length();
      int from = sumSegIdx * SEG;
      String seg = sumReport.substring(from, min(len, from + SEG));
      if (sumSegIdx == sumSegTotal - 1 && len > sumSegTotal * SEG) {   // capped, more text remains
        if ((int)seg.length() > SEG - 9) seg = seg.substring(0, SEG - 9);
        seg += "+more@SD";
      }
      String pfx = (sumSegTotal > 1) ? ("(" + String(sumSegIdx + 1) + "/" + String(sumSegTotal) + ")") : String("");
      replyTarget = summaryReplyTo; sendSMS(pfx + seg); replyTarget = "";
      sumSegIdx++;
      return;
    }
  }
}

/* =============================================================================
 *  STATE MACHINE
 * ========================================================================== */
void stateMachineTick() {
  static unsigned long syncStart = 0;
  switch (sysState) {
    case STARTUP_SYNC: {
      // If we got here via a power-cycle, hold the relay OFF long enough for a real cycle.
      if (esp2OffMs != 0 && millis() - esp2OffMs < POWER_CYCLE_OFF_MS) break;
      if (syncStart == 0) {
        syncStart = millis();
        esp2SetPower(true);                          // power ESP2 to validate READY (sec.10.7.2)
        esp2OffMs = 0;
        esp2Serial.print(FRAME_START); esp2Serial.print(",STATUS_REQ,"); esp2Serial.println(FRAME_END);
      }
      bool nanoFresh = (millis() - sensor.lastNanoMs < HEARTBEAT_TIMEOUT_MS);
      if (esp2Available && nanoFresh) {
        syncStart = 0;
        esp2CommRecovered();                         // comm validated -> clear cap counter + any COMM_LOST latch
        pushAllCalToEsp2();                          // baseline calibration after any reset (§A.5.1 #2)
        esp2SetPower(false);                         // validated -> ESP2 OFF for idle (sec.18.8)
        setState(IDLE_STATE);
      } else if (millis() - syncStart > STARTUP_SYNC_TIMEOUT_MS) {
        syncStart = 0;
        if (!esp2Available) raiseFault('M', "ESP2_NO_READY", "STARTUP");  // raiseFault already logs
        esp2SetPower(false);                         // give up powering; a run will re-power on demand
        setState(IDLE_STATE);
      }
      break;
    }
    case ACTIVE_STATE: {
      // OFF-during-idle: a scheduled run parks here while ESP2 boots; its READY dispatches
      // the work order (handleEsp2Response). This is the warm-up timeout safety net.
      if (pendingRun.active && !wo.active) {
        if (esp2Available) dispatchPendingRun();
        else if (millis() - esp2WarmupMs > ESP2_WARMUP_TIMEOUT_MS) {
          raiseFault('M', "ESP2_NO_READY", "RUN");   // executor never came up for the run
          pendingRun.active = false; pendingRun.colIdx = -1;
          esp2SetPower(false);
          setState(IDLE_STATE);
        }
      }
      // Same pattern for a preventive exercise: warm up -> EXERCISE -> wait for DONE.
      else if (pendingExercise.active) {
        if (!pendingExercise.sent) {
          if (esp2Available) dispatchPendingExercise();
          else if (millis() - esp2WarmupMs > ESP2_WARMUP_TIMEOUT_MS) {  // never booted: skip, defer 2 days
            int k = pendingExercise.idx; if (k >= 0 && k < 3) lastPumpUseMs[k] = millis();
            logEvent("ESP1", "ACT", String("EXERCISE|NOBOOT|") + (k >= 0 ? EX_NAME[k] : "?"));  // ESP2 never powered up
            pendingExercise.active = false; pendingExercise.idx = -1; pendingExercise.acked = false;
            firebaseRemoteExerciseDone("failed", "ESP2 never powered up");   // releases the remote slot
            esp2SetPower(false); setState(IDLE_STATE);
          }
        } else if (millis() - esp2WarmupMs > PUMP_EXERCISE_TIMEOUT_MS) {  // no DONE,EXERCISE in time
          int k = pendingExercise.idx; if (k >= 0 && k < 3) lastPumpUseMs[k] = millis();
          // Distinguish "ESP2 never confirmed receipt" (NOACK) from "acked but never finished" (NODONE).
          logEvent("ESP1", "ACT", String("EXERCISE|") + (pendingExercise.acked ? "NODONE|" : "NOACK|") + (k >= 0 ? EX_NAME[k] : "?"));
          firebaseRemoteExerciseDone("failed", pendingExercise.acked ? "ESP2 never reported done" : "ESP2 never acknowledged");
          pendingExercise.active = false; pendingExercise.idx = -1; pendingExercise.sent = false; pendingExercise.acked = false;
          esp2SetPower(false); setState(IDLE_STATE);
        }
      }
      break;
    }
    case RECOVERY_STATE: {
      // Fast loop is disabled once ESP2_COMM_LOST is latched -- the 20-min slow-retry owns recovery now,
      // so any trigger that lands us here just falls back to idle (no relay hammering). (sec.18.9)
      if (esp2CommLost) { setState(IDLE_STATE); break; }
      // ESP2 recovery escalation ladder (sec.18.9 / CLAUDE.md): try the cheap soft reset
      // (RESET_SELF) first; fall back to the GPIO4 power-cycle only if ESP2 does not come back.
      if (!esp2SoftResetTried) {
        esp2SoftResetTried = true;
        esp2ReinitUart();                          // reset ESP1's own UART first (RESET_SELF reply lands clean)
        if (esp2Powered) {
          sendEsp2("RESET_SELF");                  // ESP2 ACKs, goes SAFE, reboots, re-emits READY,ESP2
          logEvent("ESP1", "RESET", "ESP2|SOFT|RESET_SELF");
          esp2Available = false;                   // success = READY (handleEsp2Response), not the ACK
          esp2RecoverMs = millis();
        } else {
          esp2Escalate();                          // unpowered: soft reset impossible -> capped cycle re-powers
        }
        break;
      }
      // Soft reset issued: success is detected in the READY branch of handleEsp2Response.
      // If ESP2 never returns within the budget, escalate to the (capped) power-cycle.
      if (millis() - esp2RecoverMs > ESP2_SOFT_RESET_TIMEOUT_MS) {
        logEvent("ESP1", "RESET", "ESP2|SOFT_FAILED|POWERCYCLE");
        esp2Escalate();
      }
      break;
    }
    default: break;
  }
}

/* =============================================================================
 *  CONTROL LOGIC  --  windowed schedule + soil threshold + NPK gap
 * ========================================================================== */
// Format minutes-of-day as HH:MM for the control-decision log.
static String hhmm(uint16_t m) {
  char b[6]; snprintf(b, sizeof(b), "%02u:%02u", (unsigned)(m / 60), (unsigned)(m % 60)); return String(b);
}
// Log a per-column control decision, but only when it CHANGES (or every 10 min) so the SD log
// explains WHY a column is/ isn't serviced without flooding (answers "why it never irrigates").
static void ctrlNote(int c, const String &r) {
  if (ctrlReason[c] == r && millis() - ctrlReasonMs[c] < 600000UL) return;
  ctrlReason[c] = r; ctrlReasonMs[c] = millis();
  logEvent("ESP1", "CTRL", String("COL_") + COL_TAG[c] + "|" + r);
}

void controlTick() {
  if (uiMode != UI_DATA) return;          // operator in Settings/Testing -> pause automation
  if (sysState != IDLE_STATE) return;     // only dispatch new work from IDLE
  if (wo.active) return;                   // ESP2 busy -> sequential (sec.14.2.0.1)
  if (pendingRun.active) return;           // a run is already warming ESP2 up
#if BATTERY_SAFETY_ENABLED
  if (batteryCritical) return;             // battery-critical -> hold all runs (READY, not active)
#endif
  if (!rtcOk) return;
  // NOTE: ESP2 is OFF during idle (sec.18.8) -- do NOT gate on esp2Available here; a due
  // column powers ESP2 up on demand below and the work order is sent once it reports READY.

  uint16_t mod = minuteOfDay();

  for (int c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c]) continue;                              // config, not a fault -> no CTRL log spam
    // Operator (or the auto-cancel breaker) suppressed this column for a while. Logged like every
    // other skip reason so the CSV shows why a due column was passed over.
    if (colSnoozeUntil[c] && (long)(millis() - colSnoozeUntil[c]) < 0) {
      ctrlNote(c, "SNOOZED|" + String((colSnoozeUntil[c] - millis()) / 60000 + 1) + "min");
      continue;
    }
    if (col[c].lastServicedStamp == currentDayStamp) { ctrlNote(c, "SERVICED_TODAY"); continue; }
    // schedule axis: AUTO uses the default window, MANUAL uses the operator-set one (sec.18.10.7.2)
    uint16_t ws = (colSchedMode[c] == SCHED_MANUAL) ? COL_WIN_START[c] : DEF_WIN_START[c];
    uint16_t we = (colSchedMode[c] == SCHED_MANUAL) ? COL_WIN_END[c]   : DEF_WIN_END[c];
    if (mod < ws || mod > we) {                                    // outside service window
      ctrlNote(c, "OUTSIDE_WINDOW|now=" + hhmm(mod) + "|win=" + hhmm(ws) + "-" + hhmm(we));
      continue;
    }
    if (sensor.soil[c] < 0) { ctrlNote(c, "SOIL_INVALID"); continue; }   // invalid soil (faulted elsewhere)
    if (sensor.soil[c] >= soilStartPct) {                          // not dry enough
      ctrlNote(c, "NOT_DRY|soil=" + String(sensor.soil[c]) + ">=" + String(soilStartPct));
      continue;
    }
    if (sensor.tankValid && sensor.resLevel < RES_LOW_PCT) {        // reservoir guard (sec.14.4.1)
      // A1: latch so the fault/SMS fires ONCE per low episode (cleared on recovery in powerTick-
      // style check below), not every loop while a column sits due over a low reservoir.
      ctrlNote(c, "RES_LOW|res=" + String((int)sensor.resLevel));
      if (!resLowLatched) { resLowLatched = true; raiseFault('M', "RES_LOW", "RESERVOIR"); }
      continue;
    }
    bool fert = decideFertigate(c);
    ctrlNote(c, fert ? "DUE->FERTIGATE" : "DUE->IRRIGATE");        // column is being serviced now
#if BATTERY_SAFETY_ENABLED
    if (batteryLow) fert = false;                                  // low battery -> irrigation only (READY, not active)
#endif
    setState(ACTIVE_STATE);                                        // nanoPaceTick sends ACTIVE on this transition
    // OFF-during-idle (sec.18.8): queue the run, power ESP2 up, and let its READY dispatch the
    // work order (stateMachineTick ACTIVE_STATE is the warm-up timeout safety net).
    pendingRun.active = true; pendingRun.colIdx = c; pendingRun.fertigate = fert;
    runUiBegin(c, fert);                                           // pre-run receipt (shows during warm-up)
    esp2WarmupMs = millis();
    if (esp2Available && esp2Powered) dispatchPendingRun();        // already up -> send immediately
    else                              esp2SetPower(true);          // warm up; READY will dispatch
    return;                                                        // one column at a time
  }
}

bool decideFertigate(int c) {
  if (col[c].mode == MODE_IRRIGATION_ONLY) return false;
  if (!sensor.npkValid[c]) {
    raiseFault('M', "NPK_FAULT", c == 0 ? "COL_A" : c == 1 ? "COL_B" : "COL_C");
    // Fertigation-capable column falls back to irrigation-only because NPK is invalid. Log the DOWNGRADE
    // as its own event so the thesis dataset can tell "fertigated" from "watered only" (sec.M-3); the
    // NPK_FAULT above marks the sensor, this marks the decision consequence.
    logEvent("ESP1", "CTRL", String("COL_") + COL_TAG[c] + "|FERT_DOWNGRADE|reason=NPK_INVALID");
    return false;
  }
  float n = sensor.npk[c][4], p = sensor.npk[c][5], k = sensor.npk[c][6];
  float gapN = col[c].targetN - n, gapP = col[c].targetP - p, gapK = col[c].targetK - k;
  bool fert = (gapN >= fertGap) || (gapP >= fertGap) || (gapK >= fertGap);
  // Log the affirmative decision with the per-nutrient gaps so every fertigation choice is traceable
  // (the DOWNGRADE path above logs the other branch). Low-frequency: only at the start of a serviced run.
  logEvent("ESP1", "CTRL", String("COL_") + COL_TAG[c] + "|FERT_DECIDE|fert=" + String(fert ? 1 : 0) +
           "|gapN=" + String(gapN, 1) + "|gapP=" + String(gapP, 1) + "|gapK=" + String(gapK, 1));
  return fert;
}

/* =============================================================================
 *  MODULE HEALTH LOGGING  --  edge (state change) + periodic HEALTH snapshot so the SD log
 *  shows WHEN each module/link/sensor failed (GSM, WiFi/ThingSpeak, RTC/LCD/SD, Nano/ESP2
 *  links, per-column NPK). All logging is on core 1 (SD is core-1-only); netTask snapshot
 *  flags (wifiConnected / lastTsOk) are just read here.
 * ========================================================================== */
const unsigned long NANO_LINK_STALE_MS = 180000;   // Nano considered stale if no packet in 3 min
void healthTick() {
  static unsigned long lastCheckMs = 0, lastSnapMs = 0;
  if (millis() - lastCheckMs < 5000) return;       // edge scan every 5 s
  lastCheckMs = millis();

  // GSM registration
  static int lastReg = -1;
  int reg = netRegistered ? 1 : 0;
  if (reg != lastReg) { lastReg = reg; logEvent("ESP1", "NET", reg ? ("GSM|REG|rssi=" + String(lastRssi)) : "GSM|NOREG"); }

  // WiFi (off / up / down) + ThingSpeak upload result
  static int lastWifi = -1;
  int wf = !wifiEnabled ? 2 : (wifiConnected ? 1 : 0);
  if (wf != lastWifi) { lastWifi = wf; logEvent("ESP1", "NET", wf == 2 ? "WIFI|OFF" : wf == 1 ? ("WIFI|UP|rssi=" + String(wifiRssiVal)) : "WIFI|DOWN"); }
  static int lastTs = -1;
  if (wifiEnabled && wifiConnected) { int ts = lastTsOk ? 1 : 0; if (ts != lastTs) { lastTs = ts; logEvent("ESP1", "NET", ts ? "TS|OK" : "TS|FAIL"); } }

  // I2C/SD device presence (probe at this cadence)
  static int8_t dprev[3] = { -1, -1, -1 };
  bool dnow[3] = { i2cPresent(0x68), i2cPresent(LCD_ADDR), sdOk };
  const char *dname[3] = { "RTC", "LCD", "SD" };
  for (int k = 0; k < 3; k++) { int v = dnow[k] ? 1 : 0; if (v != dprev[k]) { dprev[k] = v; logEvent("ESP1", "DEV", String(dname[k]) + "|" + (v ? "OK" : "ABSENT")); } }

  // Nano link + ESP2 availability
  static int lastNano = -1;
  int nano = (sensor.lastNanoMs && millis() - sensor.lastNanoMs < NANO_LINK_STALE_MS) ? 1 : 0;
  if (nano != lastNano) { lastNano = nano; logEvent("ESP1", "LINK", nano ? "NANO|OK" : "NANO|STALE"); }
  static int lastE2 = -1;
  int e2 = esp2Available ? 1 : 0;
  if (e2 != lastE2) { lastE2 = e2; logEvent("ESP1", "LINK", e2 ? "ESP2|OK" : "ESP2|SILENT"); }

  // Per-column NPK sensor validity (flaps on -1) -- the one sensor most prone to intermittent failure
  static int8_t npkPrev[NUM_COLUMNS] = { -1, -1, -1 };
  for (int c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c]) continue;
    int v = sensor.npkValid[c] ? 1 : 0;
    if (v != npkPrev[c]) { npkPrev[c] = v; logEvent("ESP1", "SENSOR", String("NPK_") + COL_TAG[c] + "|" + (v ? String("OK") : ("FAIL|reason=" + sensor.npkReason[c]))); }
  }

  // Periodic snapshot every 5 min: one greppable line with every module state.
  if (millis() - lastSnapMs >= 300000) {
    lastSnapMs = millis();
    // Bounded (every 5 min) GSM link telemetry: raw CSQ rssi (0-31, 99=unknown), raw CREG stat
    // (2=searching, 3=denied -- not just the reg bool), SIM state. Mirrors the WIFI|UP|rssi pattern so
    // "GSM failing" can be told apart from "no signal" / "not registered" in the logs.
    logEvent("ESP1", "NET", "GSM|rssi=" + String(lastRssi) + "|creg=" + String(lastCreg) +
                            "|cpin=" + String(simReady ? "READY" : "NOTREADY"));
    String s  = "GSM:" + String(netRegistered ? "reg" : "no") + "(rssi=" + String(lastRssi) + ")";
    s += "|WIFI:" + String(!wifiEnabled ? "off" : wifiConnected ? "up" : "dn");
    s += "|RTC:" + String(rtcOk ? "ok" : "x") + "|SD:" + String(sdOk ? "ok" : "x");
    s += "|NANO:" + String((sensor.lastNanoMs && millis() - sensor.lastNanoMs < NANO_LINK_STALE_MS) ? "ok" : "stale");
    s += "|ESP2:" + String(esp2Powered ? (esp2Available ? "on" : "boot") : "off");
    s += "|SOILA:" + String(sensor.soil[0]) + "|SOILB:" + String(sensor.soil[1]);
    logEvent("ESP1", "HEALTH", s);
  }
}

/* =============================================================================
 *  POWER  (INA226 battery monitoring, spec sec.21.1)
 * ========================================================================== */
// Trimmed mean of 64 ADC samples (0..4095) -- rejects spikes. (mirrors the bench tool)
static double readAdcTrimmed(int pin) {
  const int N = 64, TRIM = 8;
  static uint16_t buf[64];
  for (int i = 0; i < N; i++) { buf[i] = analogRead(pin); delayMicroseconds(150); }
  for (int i = 1; i < N; i++) { uint16_t k = buf[i]; int j = i - 1; while (j >= 0 && buf[j] > k) { buf[j + 1] = buf[j]; j--; } buf[j + 1] = k; }
  long sum = 0; int cnt = 0;
  for (int i = TRIM; i < N - TRIM; i++) { sum += buf[i]; cnt++; }
  return cnt ? (double)sum / cnt : 0.0;
}
// value = a0 + a1*u + ... + aN*u^N,  u = raw/4095 (end-to-end: captures the ADC + optocoupler curve).
static double applyPoly(const AdcCal &c, double raw) {
  double u = raw / 4095.0, v = 0, up = 1;
  for (int k = 0; k <= c.degree && k < 4; k++) { v += c.coef[k] * up; up *= u; }
  return v;
}
// NVS "adccal" (bench-tool layout) overrides the hardcoded defaults if present + valid.
void loadBattCal() {
  struct CalRec { uint32_t magic; int32_t degree; double coef[4]; };
  const uint32_t CAL_MAGIC = 0xADC0CA11;
  Preferences p; p.begin("adccal", true);
  CalRec r;
  if (p.getBytes("v", &r, sizeof(r)) == sizeof(r) && r.magic == CAL_MAGIC && r.degree >= 1 && r.degree <= 3) {
    calBattV.degree = r.degree; for (int k = 0; k < 4; k++) calBattV.coef[k] = r.coef[k]; battCalLoaded = true;
  }
  if (p.getBytes("i", &r, sizeof(r)) == sizeof(r) && r.magic == CAL_MAGIC && r.degree >= 1 && r.degree <= 3) {
    calBattI.degree = r.degree; for (int k = 0; k < 4; k++) calBattI.coef[k] = r.coef[k]; battCalLoaded = true;
  }
  p.end();
  Serial.printf("Battery ADC cal: %s\n", battCalLoaded ? "loaded from NVS" : "using firmware defaults (calibrate!)");
}
/* ADS1115 in CONTINUOUS-conversion mode, so reading it costs ONE 2-byte I2C read and never waits.
 * A single-shot read would have to poll the OS bit for the ~7.8 ms conversion, and polling it with
 * delay() inside powerTick() breaks the "no delay() in any main loop" rule (CLAUDE.md / sec.10.6.5).
 * Continuous mode sidesteps that entirely: the ADC free-runs at 128 SPS and we simply take the
 * latest sample, which is at most 7.8 ms old -- far fresher than the powerTick cadence needs.
 * Configure once; re-configure automatically after any I2C failure so a power glitch on the ADC
 * recovers on its own. Registers per the datasheet. */
static bool acsConfigured = false;

static bool acsConfigure() {
  const uint16_t MODE_CONT = 0x0000, DR_128SPS = 0x0080, COMP_OFF = 0x0003;
  uint16_t cfg = MODE_CONT | DR_128SPS | COMP_OFF;
  cfg |= (uint16_t)(0x4000 | ((ACS_CHANNEL & 0x03) << 12));    // MUX 1xx = AINx vs GND
  cfg |= (uint16_t)((ACS_PGA & 0x07) << 9);
  Wire.beginTransmission(ACS_ADDR);
  Wire.write(0x01);                                            // config register
  Wire.write((uint8_t)(cfg >> 8)); Wire.write((uint8_t)(cfg & 0xFF));
  if (Wire.endTransmission() != 0) return false;
  Wire.beginTransmission(ACS_ADDR);
  Wire.write(0x00);                                            // leave the pointer on conversion,
  if (Wire.endTransmission() != 0) return false;               // so steady-state reads are 1 xfer
  acsConfigured = true;
  return true;
}

// Volts AT THE ADC PIN (the caller undoes the divider). Returns false on any I2C fault, so a
// missing or unpowered ADC surfaces as a fault flag rather than a plausible-looking 0 A.
static bool acsReadVolts(float &v) {
  if (!acsConfigured) {
    // Configure, but report no sample this tick: the first conversion is still ~7.8 ms away, so the
    // conversion register holds whatever preceded it (0 on a fresh ADC). Publishing that would log
    // and upload a bogus current as if it were real. The next powerTick gets a genuine reading.
    acsConfigure();
    return false;
  }
  if (Wire.requestFrom((int)ACS_ADDR, 2) != 2) { acsConfigured = false; return false; }
  int16_t raw = (int16_t)(((uint16_t)Wire.read() << 8) | Wire.read());
  const float FS[6] = { 6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f };
  v = raw * (FS[ACS_PGA] / 32768.0f);
  return true;
}

void readBatteryAdc() {
  battV = (float)applyPoly(calBattV, readAdcTrimmed(PIN_BATT_V));
  // Current now comes from the ACS758 via the ADS1115, NOT the opto ADC on GPIO34 (which read 0.00 A
  // on every sample for 16 days). On an I2C failure hold the last value and clear acsOk so the fault
  // is visible on the Diag page -- reporting 0 A would repeat exactly the failure this replaces.
  float vAdc;
  acsOk = acsReadVolts(vAdc);
  if (acsOk) {
    battIsigned = (vAdc * ACS_DIV - ACS_ZERO_V) / ACS_SENS_V_PER_A;   // signed: 050B is bidirectional
    // Magnitude only, per CLAUDE.md: all energy counts as consumption, no charge/discharge split.
    // The sensor CAN now tell the two apart (battIsigned) -- restoring that split is a separate
    // decision against a documented design choice, so it is deliberately not taken here.
    battI = fabsf(battIsigned);
  }
  battP = battV * battI;
}

// Battery protections tied to the (removed) INA226 -- READY, NOT IMPLEMENTED. The real actions live inside
// BATTERY_SAFETY_ENABLED so flipping that one define re-enables them (see also controlTick / exerciseTick).
void batterySafetyTick() {
#if BATTERY_SAFETY_ENABLED
  static bool wasLow = false, wasCrit = false;
  if (batteryCritical && !wasCrit) raiseFault('C', "BATTERY_CRITICAL", "BATT");   // -> enterEmergencyStop
  else if (batteryLow && !wasLow)  raiseFault('M', "BATTERY_LOW", "BATT");
  wasLow = batteryLow; wasCrit = batteryCritical;
#endif
}

void powerTick() {
  if (millis() - lastInaMs < INA_READ_INTERVAL_MS) return;
  unsigned long nowMs = millis();
  float dtH = (lastInaMs == 0) ? 0.0f : (nowMs - lastInaMs) / 3600000.0f;   // hours since last read
  lastInaMs = nowMs;
  readBatteryAdc();                                    // GPIO35/34 -> polynomial -> battV/battI/battP

  // Energy (B2, sec.12.1.3): opto current is magnitude only, so all energy counts as consumption.
  energyConsumedWh += (double)battP * dtH;

  // Battery state flags -- DISPLAY/LOG (and, if BATTERY_SAFETY_ENABLED, the protective actions).
  batteryCritical = (battV > 0 && battV < BATT_CRIT_V);
  batteryLow      = (battV > 0 && battV < BATT_LOW_V);
  batterySafetyTick();                                 // ready-but-disabled protections

  // B3: log a PWR snapshot at most ~1/min, OR immediately on a battery-state change (sec.25.2.1).
  static unsigned long lastPwrLogMs = 0;
  bool battStateChanged = (batteryLow != prevBattStateLow) || (batteryCritical != prevBattStateCrit);
  if (battStateChanged || lastPwrLogMs == 0 || millis() - lastPwrLogMs >= PWR_LOG_INTERVAL_MS) {
    lastPwrLogMs = millis();
    prevBattStateLow = batteryLow; prevBattStateCrit = batteryCritical;
    logEvent("ESP1", "PWR", "BATT|V=" + String(battV, 2) + "|I=" + String(battI, 2) + "|P=" + String(battP, 1)
             + "|CONS=" + String((int)(energyConsumedWh + 0.5)) + "Wh");
  }
}

/* =============================================================================
 *  SCHEDULE  (daily report, daily Nano reset, day rollover)
 * ========================================================================== */
void scheduleTick() {
  if (!rtcOk) return;
  DateTime now = rtc.now();
  long ds = dayStamp(now);
  if (ds != currentDayStamp) {                 // midnight rollover -> reset daily counters
    currentDayStamp = ds;
    faultsToday[0] = faultsToday[1] = faultsToday[2] = 0;
    nanoResetsToday = 0; esp2PowerCycles = 0;
    reportSentToday = false; nanoDailyResetDone = false;
    energyConsumedWh = energyChargedWh = 0.0;  // reset daily energy totals (B2)
    for (int c = 0; c < NUM_COLUMNS; c++) { waterUsedToday[c] = nutrientUsedToday[c] = 0; }
  }
  uint16_t mod = now.hour() * 60 + now.minute();

  if (!reportSentToday && mod >= DAILY_REPORT_MIN) {
    sendDailyReport(); reportSentToday = true;
  }
  if (!nanoDailyResetDone && mod >= DAILY_NANO_RESET_MIN) {     // sec.18.9.5.0.3 (exempt from HW limit)
    sendNanoCommand("RESET_REQ");
    logEvent("ESP1", "RESET", "NANO|RESET_REQ|DAILY");
    nanoDailyResetDone = true;
  }
  if (reportPending && sysState != ACTIVE_STATE) sendDailyReport();   // deferred STATUS
  // Deferred SUMMARY: operation finished -> run the (non-blocking) SD parse now (sec.12.1.3.1).
  if (summaryPending && sysState != ACTIVE_STATE && summaryStage == SUM_IDLE && !portalActive) {
    summaryPending = false;            // cleared again in SUM_STREAM; prevents re-trigger mid-job
    startSummaryJob(pendingSumMode, pendingSumTarget);
  }
}

/* =============================================================================
 *  SUPABASE UPLOAD SCHEDULING  (core 1 owns "which day"; netTask does the network)
 * ========================================================================== */
// Oldest completed daily CSV on the SD with supaLast < stamp < today (skips NODATE + today's open file).
// 0 = nothing to send. Requires a valid clock so "today" is known -- no auto catch-up on a dead RTC.
static long uploadNextCatchupDay() {
  if (!rtcOk || currentDayStamp <= 0) return 0;
  if (!sdTake(200)) return 0;                          // SD busy -> try next cycle
  long best = 0;
  File root = SD.open("/");
  if (root) {
    for (File e = root.openNextFile(); e; e = root.openNextFile()) {
      String n = e.name(); e.close();
      int sl = n.lastIndexOf('/'); if (sl >= 0) n = n.substring(sl + 1);
      if (n.length() != 12 || !n.endsWith(".CSV")) continue;     // want exactly NNNNNNNN.CSV
      bool digits = true; for (int i = 0; i < 8; i++) if (!isDigit(n[i])) digits = false;
      if (!digits) continue;                                     // skips NODATE.CSV and other names
      long stamp = n.substring(0, 8).toInt();
      if (stamp > supaLast && stamp < currentDayStamp && (best == 0 || stamp < best)) best = stamp;
    }
    root.close();
  }
  sdGive();
  return best;
}

// Drain a finished upload result, then queue the next day (manual request first, else catch-up).
// Exponential backoff on failure; the SD file is never deleted, so a failed upload only defers -- no data loss.
void uploadTick() {
  static unsigned long lastAttemptMs = 0;
  static int attempts = 0;
  static const unsigned long BACKOFF[4] = { 30000UL, 120000UL, 480000UL, 1800000UL };  // 30s,2m,8m,30m

  // SoftAP admin "Upload logs" button: resolve yesterday HERE (RTC lives on core 1) into a manual request.
  if (adminUploadReq) {
    adminUploadReq = false;
    if (rtcOk && currentDayStamp > 0) {
      long y = dayStamp(rtc.now() - TimeSpan(1, 0, 0, 0));
      char path[16]; snprintf(path, sizeof(path), "/%08ld.CSV", y);
      bool got = sdTake(200), exists = got && SD.exists(path); if (got) sdGive();
      if (exists) { manualUploadStamp = y; attempts = 0; }
    }
  }

  // 1) publish a finished result (netTask -> here)
  if (uploadResult != 0) {
    long s = uploadResStamp; int r = uploadResult, code = uploadHttp;
    uploadResult = 0; lastAttemptMs = millis();
    if (r == 1) {
      logEvent("ESP1", "UPLOAD", "OK|" + String(s) + ".CSV");
      if (s > supaLast) { supaLast = s; saveSupaLast(); }
      if (manualUploadStamp == s) manualUploadStamp = 0;
      attempts = 0;
    } else {
      attempts++;
      logEvent("ESP1", "UPLOAD", "FAIL|" + String(s) + ".CSV|http=" + String(code) + "|try=" + String(attempts));
      if (manualUploadStamp == s && attempts >= 5) { manualUploadStamp = 0; attempts = 0; }  // give up a stuck manual send
    }
    return;
  }

  if (uploadReqStamp != 0 || uploadBusy) return;                 // an attempt is in flight
  if (supaUrl.length() == 0 || supaKey.length() == 0) return;    // not configured
  if (!wifiConnected) return;
  unsigned long wait = (attempts > 0) ? BACKOFF[attempts < 4 ? attempts - 1 : 3] : 5000UL;
  if (millis() - lastAttemptMs < wait) return;                   // backoff / gentle SD-scan throttle

  long target = (manualUploadStamp != 0) ? manualUploadStamp : uploadNextCatchupDay();
  if (target == 0) return;
  uploadReqStamp = target;                                       // hand to netTask
  lastAttemptMs = millis();
}

/* =============================================================================
 *  HEARTBEAT  (freshness; Nano silence does NOT trigger reset, sec.18.9.5.0.1)
 * ========================================================================== */
void heartbeatTick() {
  // No silence alerts / ESP2 recovery while manually Testing -- a heartbeat lapse must not power-cycle
  // ESP2 or alert mid-test (ESP2 heartbeats continuously in test mode; the Testing header shows link state).
  if (sysState == TEST_MODE) return;
  // Nano silence: alert/count ONCE per outage (Nano's own WDT self-recovers, sec.18.9.5.0.1).
  // Not a reset trigger; the flag clears when fresh data resumes (pollNano).
  if (!nanoSilent && millis() - sensor.lastNanoMs > NANO_SILENCE_TIMEOUT_MS) {
    nanoSilent = true;
    faultsToday[2]++;                                          // minor (now actually tallied)
    logEvent("ESP1", "FAULT", "MIN|NANO_SILENCE|NANO");
    sendSMS("ALERT,MIN,NANO_SILENCE,NANO");                    // one-shot (queued, non-blocking)
  }
  // ESP2 silence: major fault + recovery. Inherently one-shot (esp2Available latches false
  // until ESP2 speaks again). RECOVERY_STATE drives the power-cycle (no longer a dead end).
  if (esp2Available && millis() - lastEsp2Ms > HEARTBEAT_TIMEOUT_MS) {
    // Don't declare silence on missed heartbeats alone -- actively probe first. A working ESP2 whose
    // frames were dropped answers a STATUS_REQ; each reply refreshes lastEsp2Ms + clears silenceProbes.
    if (silenceProbes < SILENCE_PROBE_MAX) {
      if (millis() - silenceProbeMs >= SILENCE_PROBE_INTERVAL_MS) {
        esp2Serial.print(FRAME_START); esp2Serial.print(",STATUS_REQ,"); esp2Serial.println(FRAME_END);
        silenceProbeMs = millis(); silenceProbes++;
        logEvent("ESP1", "PROBE", "ESP2_SILENCE_CHECK|" + String(silenceProbes));
      }
      return;                                                 // wait out the confirm window before deciding
    }
    esp2Available = false; silenceProbes = 0;
    raiseFault('M', "ESP2_SILENCE", "ESP2");                  // counts + queued SMS (sec.12.1.1)
    // Start the recovery ladder fresh (soft reset first, then power-cycle).
    if (sysState != EMERGENCY_STOP) { esp2SoftResetTried = false; setState(RECOVERY_STATE); }
  }
}

/* =============================================================================
 *  NANO INTERVAL PACING  (A2: own the ACTIVE/DAY/NIGHT command; send only on change)
 * -----------------------------------------------------------------------------
 *  The Nano boots ACTIVE (10 s) and only changes interval when ESP1 tells it to.
 *  This is the single owner of that command: ACTIVE during runs, else DAY/NIGHT by
 *  the RTC clock. Sending only on a state change avoids spamming the SoftwareSerial
 *  link. NIGHT window wraps midnight (NIGHT_START_MIN..NIGHT_END_MIN).             */
void nanoPaceTick() {
  NanoPace desired;
  if (sysState == ACTIVE_STATE || wo.active || pendingRun.active) {
    desired = PACE_ACTIVE;
  } else if (!rtcOk) {
    desired = PACE_DAY;                                  // no clock -> default to day idle
  } else {
    uint16_t mod = minuteOfDay();
    bool night = (mod >= NIGHT_START_MIN) || (mod < NIGHT_END_MIN);
    desired = night ? PACE_NIGHT : PACE_DAY;
  }
  if (desired == lastNanoPace) return;
  lastNanoPace = desired;
  if      (desired == PACE_ACTIVE) sendNanoCommand("ACTIVE");
  else if (desired == PACE_DAY)    sendNanoCommand("DAY");
  else if (desired == PACE_NIGHT)  sendNanoCommand("NIGHT");
}

/* =============================================================================
 *  DEVICE HEALTH  (Part C: present-at-boot -> absent-at-runtime => daily self-reset)
 * -----------------------------------------------------------------------------
 *  ESP1's RTC/LCD/INA226/SD can wedge (suspected I2C lock-up). If a device that was
 *  present at boot stops responding at runtime, reboot ESP1 -- but at most ONCE per
 *  calendar day. Boot-loop-safe: only a present->absent TRANSITION triggers a reset,
 *  so a device that is simply absent from boot just runs degraded (no loop).        */
static bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}
void deviceHealthTick() {
  if (millis() - lastDevHealthMs < DEV_HEALTH_INTERVAL_MS) return;
  lastDevHealthMs = millis();

  // Re-probe current presence. SD has no cheap probe: on a flush failure logFlush sets
  // sdOk=false after a failed re-init, so we trust that flag here.
  bool nowPresent[DEV_COUNT];
  nowPresent[DEV_RTC] = i2cPresent(0x68);
  nowPresent[DEV_LCD] = i2cPresent(LCD_ADDR);
  nowPresent[DEV_INA] = i2cPresent(INA226_ADDR);
  nowPresent[DEV_SD]  = sdOk;

  const char *DEV_NAME[DEV_COUNT] = { "RTC", "LCD", "INA226", "SD" };
  int lost = -1;
  for (int d = 0; d < DEV_COUNT; d++) {
    if (d == DEV_INA) continue;                                  // INA226 dropout no longer reboots (user request)
    if (bootPresent[d] && !nowPresent[d]) { lost = d; break; }   // dropout transition
  }
  if (lost < 0) return;

  // Once per calendar day (RTC-RAM stamp survives the software reset). If the RTC has no
  // valid date (today==0, e.g. RTC itself wedged) this still allows exactly ONE reset, then
  // matches (0==0) and stops -- no boot loop -- until a real day stamp re-arms it.
  uint32_t today = (currentDayStamp > 0) ? (uint32_t)currentDayStamp : 0;
  if (g_lastSelfResetDay == today) return;                      // already reset for this day stamp

  logEvent("ESP1", "RESET", String("SELF|DEVICE_LOST|") + DEV_NAME[lost]);
  sendSMS(String("ALERT,MAJ,SELF_RESET,") + DEV_NAME[lost]);    // best-effort (may not flush before reboot)
  logFlush(true);                                               // persist what we can
  g_lastSelfResetDay = today;
  delay(50);                                                    // let the last UART/SD bytes drain
  ESP.restart();
}

/* =============================================================================
 *  WiFi + THINGSPEAK TELEMETRY  (Part A: core-0 task, non-blocking uplink)
 * ========================================================================== */
// Core 1: snapshot the latest readings into the shared struct (POD -> spinlock). Throttled.
void telemetryCollect() {
  if (millis() - lastTelemCollectMs < 1000) return;
  lastTelemCollectMs = millis();
  portENTER_CRITICAL(&telemMux);
  telem.temp = sensor.temp; telem.hum = sensor.hum; telem.lux = sensor.lux;
  telem.resLevel = sensor.resLevel; telem.mixLevel = sensor.mixLevel; telem.flow = sensor.flow;
  for (int c = 0; c < NUM_COLUMNS; c++) {
    telem.soil[c] = sensor.soil[c];
    telem.npkValid[c] = sensor.npkValid[c];
    telem.npkN[c] = sensor.npk[c][4]; telem.npkP[c] = sensor.npk[c][5]; telem.npkK[c] = sensor.npk[c][6];
    telem.npkEC[c] = sensor.npk[c][2]; telem.npkPH[c] = sensor.npk[c][3];
  }
  telem.battV = battV; telem.battP = battP; telem.battI = battI;
  // Mirror the per-group validity + coarse system state so core-0 uploaders read only from the snapshot.
  telem.envValid = sensor.envValid; telem.tankValid = sensor.tankValid; telem.lightValid = sensor.lightValid;
  telem.inaValid = inaOk;
  telem.state = sysState; telem.woActive = wo.active;
  telem.valid = true;
  portEXIT_CRITICAL(&telemMux);
}

// Core 0: one ThingSpeak update GET. Returns true on HTTP 200.
static bool tsGet(const String &url) {
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  if (!http.begin(url)) return false;
  int code = http.GET();
  http.end();
  return code == 200;
}

// Core 0: push channel 1 (per-column moisture+NPK) and channel 2 (system) from the snapshot.
static void tsUpload() {
  TelemetrySnapshot t;
  portENTER_CRITICAL(&telemMux); t = telem; portEXIT_CRITICAL(&telemMux);
  if (!t.valid) return;

  String k1, k2, k3;
  xSemaphoreTake(netMux, portMAX_DELAY); k1 = tsKey1; k2 = tsKey2; k3 = tsKey3; xSemaphoreGive(netMux);
  bool ok = false;

  if (k1.length()) {                                  // Channel 1: columns (moisture + NPK)
    String url = "http://" + String(TS_HOST) + "/update?api_key=" + k1;
    int fld = 1;
    for (int c = 0; c < NUM_COLUMNS && fld <= 8; c++) {
      if (!COLUMN_ENABLED[c]) continue;
      url += "&field" + String(fld++) + "=" + String(t.soil[c]);
      if (t.npkValid[c] && fld + 2 <= 8) {
        url += "&field" + String(fld++) + "=" + String(t.npkN[c], 1);
        url += "&field" + String(fld++) + "=" + String(t.npkP[c], 1);
        url += "&field" + String(fld++) + "=" + String(t.npkK[c], 1);
      } else fld += 3;
    }
    ok = tsGet(url) || ok;
  }
  if (k2.length()) {                                  // Channel 2: system
    String url = "http://" + String(TS_HOST) + "/update?api_key=" + k2;
    url += "&field1=" + String(t.temp, 1);
    url += "&field2=" + String(t.hum, 1);
    url += "&field3=" + String(t.lux, 0);
    url += "&field4=" + String(t.resLevel, 1);
    url += "&field5=" + String(t.mixLevel, 1);
    url += "&field6=" + String(t.battV, 2);
    url += "&field7=" + String(t.battP, 1);
    url += "&field8=" + String(t.flow, 1);
    ok = tsGet(url) || ok;
  }
  if (k3.length()) {                                  // Channel 3: chemistry (per-column EC + pH)
    String url = "http://" + String(TS_HOST) + "/update?api_key=" + k3;
    int fld = 1;
    for (int c = 0; c < NUM_COLUMNS && fld <= 8; c++) {
      if (!COLUMN_ENABLED[c]) continue;
      if (t.npkValid[c]) {
        url += "&field" + String(fld++) + "=" + String(t.npkEC[c], 2);
        url += "&field" + String(fld++) + "=" + String(t.npkPH[c], 2);
      } else fld += 2;
    }
    // Battery current (ACS758) rides Ch3 because Ch2 "System" has all 8 fields used.
    // CONSTRAINT: the loop above consumes 2 fields per ENABLED column -- 4 today with A and B.
    // Enabling column C would take fields 1-6 and collide with field 5. Move this constant if so.
    url += "&field" + String(TS3_CURRENT_FIELD) + "=" + String(t.battI, 3);
    ok = tsGet(url) || ok;
  }
  lastTsOk = ok; lastTsUploadMs = millis();
}

/* =============================================================================
 *  FIREBASE RTDB LIVE SNAPSHOT  (core 0 / netTask)
 *  PUT <fbUrl>/irrigation/live.json -- replaces the node, so storage stays flat.
 * ========================================================================== */
/* ---- Shared TLS client (keep-alive) ---------------------------------------
 * ONE WiFiClientSecure + HTTPClient reused by every Firebase request. Without this, the
 * 3 s command poll would open a fresh mbedTLS session ~28,800 times a day: ~1-2 s and
 * ~40-50 KB of heap churn each, which would saturate netTask and starve the ThingSpeak
 * and Supabase uploads. setReuse(true) keeps the socket open, so handshakes happen only
 * on first use and after the server drops us -- fbHandshakes is the metric that proves it.
 * Only netTask touches these; they are never used from core 1.                          */
WiFiClientSecure fbClient;
HTTPClient       fbHttp;
bool             fbTlsReady = false;                // TLS options applied once

// Apply the TLS trust policy. Pinning matters more here than for the Supabase CSV push
// because the provisioning sign-in carries the device PASSWORD. FB_ROOT_CA is the Google
// GTS Root R1 PEM; when it is empty we fall back to setInsecure() (encrypted, unauthenticated)
// and say so, rather than silently pretending the connection is verified.
static void fbApplyTls() {
  if (fbTlsReady) return;
  if (fbPinCa && FB_ROOT_CA[0]) fbClient.setCACert(FB_ROOT_CA);
  else                          fbClient.setInsecure();   // [HARDENING TODO] no cert validation
  fbHttp.setReuse(true);                            // <-- the whole point: keep the socket alive
  fbTlsReady = true;
}
// Force the next request to re-apply the trust policy (and drop any open socket), so toggling
// pinning takes effect immediately instead of at the next reboot.
static void fbResetTls() {
  fbHttp.end();
  fbClient.stop();
  fbTlsReady = false;
}

// Every Firebase HTTP call goes through here so the handshake counter and timeouts stay honest.
static bool fbBegin(const String &url) {
  fbApplyTls();
  if (!fbClient.connected()) fbHandshakes++;        // a fresh TCP+TLS session is about to happen
  fbHttp.setConnectTimeout(6000);
  fbHttp.setTimeout(6000);
  return fbHttp.begin(fbClient, url);
}

// Single seam for the RTDB path + auth. Every request carries the device account's ID token;
// lock the RTDB rules to that UID or the token buys you nothing.
static String fbBuildUrl(const String &base, const char *path) {
  return base + path + "?auth=" + fbIdToken;
}

/* ---- Token lifecycle -------------------------------------------------------
 * Two paths, deliberately asymmetric:
 *   PROVISIONING (rare)  signInWithPassword -> idToken + refreshToken. Needs the password.
 *   STEADY STATE (hourly) securetoken refresh -> idToken. Needs only the refresh token.
 * Once a refresh token exists the password is dead weight and can be wiped, so a stolen
 * device yields a revocable token instead of reusable account credentials.               */

// Turn a failed auth response into something a human can act on. Google returns
// {"error":{"message":"REASON"}}; the bare HTTP number alone is not diagnosable.
// A negative code is an HTTPClient transport error (no TLS/TCP), not an API rejection.
static void fbNoteAuthError(int code, const String &resp) {
  String reason;
  if (code < 0) {
    reason = "transport error (TLS/TCP) code " + String(code);
  } else {
    StaticJsonDocument<512> e;
    if (!deserializeJson(e, resp)) reason = String(e["error"]["message"] | "");
    if (!reason.length()) reason = "HTTP " + String(code);
  }
  // The handful of reasons that actually happen, translated into the fix.
  String hint;
  if      (reason.startsWith("OPERATION_NOT_ALLOWED"))   hint = " -> enable Email/Password in Firebase Console > Authentication > Sign-in method";
  else if (reason.startsWith("EMAIL_NOT_FOUND"))         hint = " -> that user does not exist in THIS project; create it under Authentication > Users";
  else if (reason.startsWith("INVALID_LOGIN_CREDENTIALS")
        || reason.startsWith("INVALID_PASSWORD"))        hint = " -> wrong password (or wrong project's API key)";
  else if (reason.startsWith("API_KEY_INVALID")
        || reason.startsWith("INVALID_API_KEY"))         hint = " -> Web API key is wrong; copy it from Project settings > General";
  else if (reason.startsWith("INVALID_EMAIL"))           hint = " -> email is malformed";
  else if (reason.startsWith("USER_DISABLED"))           hint = " -> the device account is disabled";
  else if (reason.startsWith("TOO_MANY_ATTEMPTS"))       hint = " -> rate-limited by Google; wait, then retry";
  else if (reason.startsWith("TOKEN_EXPIRED")
        || reason.startsWith("USER_NOT_FOUND"))          hint = " -> refresh token no longer valid; re-enter the password to re-provision";
  else if (code < 0)                                     hint = " -> no TLS/TCP to Google: check WiFi, DNS, and try FBASE,TLS,OFF to rule out cert pinning";
  fbAuthErr = reason + hint;
  fbAuthErrMs = millis();
  Serial.print(F("[FIREBASE] auth failed: ")); Serial.println(fbAuthErr);
  // Hand the reason to core 1 for the SD log. Without this the only witness is the serial
  // monitor -- and you cannot read the portal's copy without suspending the very connection
  // attempt that produces it.
  strlcpy(fbAuthErrLog, fbAuthErr.c_str(), sizeof(fbAuthErrLog));
  fbAuthErrLogPending = true;
}

// A locally-detected auth blocker (no network involved). Same reporting path as a Google
// rejection, because "FAIL|http=0|n=0" with no reason is exactly the dead end this avoids.
// Deduped: these conditions persist for many ticks and must not flood the CSV.
static void fbNoteAuthLocal(const char *why) {
  if (fbAuthErr == why) return;
  fbAuthErr = why;
  fbAuthErrMs = millis();
  Serial.print(F("[FIREBASE] auth blocked: ")); Serial.println(fbAuthErr);
  strlcpy(fbAuthErrLog, fbAuthErr.c_str(), sizeof(fbAuthErrLog));
  fbAuthErrLogPending = true;
}

// Shared tail: parse a token response, store it, reset the backoff. Returns true on success.
static bool fbStoreToken(const String &idToken, const String &refresh, unsigned long expSec) {
  if (!idToken.length()) return false;
  xSemaphoreTake(netMux, portMAX_DELAY);
  fbIdToken = idToken;
  if (refresh.length()) fbRefresh = refresh;
  xSemaphoreGive(netMux);
  fbAuthErr = "";
  // Renew 5 min early so a slow link never uses an expired token mid-request.
  fbTokenExpiryMs = millis() + ((expSec > 300 ? expSec - 300 : 60) * 1000UL);
  fbAuthFails = 0; fbAuthNextTryMs = 0; fbAuthOk = true;
  return true;
}

// Exchange the stored refresh token for a fresh ID token. No password involved.
static bool fbRefreshToken(const String &apiKey, const String &refresh) {
  String body = "grant_type=refresh_token&refresh_token=" + refresh;
  if (!fbBegin("https://securetoken.googleapis.com/v1/token?key=" + apiKey)) {
    fbAuthHttp = -1; fbNoteAuthError(-1, ""); return false;
  }
  fbHttp.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = fbHttp.POST(body);
  // ALWAYS drain the body: it carries the reason on failure, and an unread body would
  // leave the kept-alive socket dirty for the next request.
  String resp = fbHttp.getString();
  fbHttp.end();
  fbAuthHttp = code;
  if (code != 200) { fbNoteAuthError(code, resp); return false; }
  // 4096, not 1536. A real securetoken response is ~1.5 KB -- the ID token alone is a ~980-char JWT
  // and the refresh token another ~270 -- and deserializeJson() from a String COPIES every string
  // into the document's pool, then adds ~16 B per member on top. Measured need is ~1594 B, so 1536
  // fell ~60 B short: deserializeJson returned NoMemory and this reported "malformed" on a
  // perfectly good HTTP 200, meaning the device could never hold a token. THIS is why Firebase
  // never connected. Do not shrink it back.
  StaticJsonDocument<4096> r;
  // Report WHICH parse failure. Calling a NoMemory "malformed" is what disguised the undersized
  // pool as a server problem for so long -- c.f() names it (NoMemory / IncompleteInput / ...).
  DeserializationError de = deserializeJson(r, resp);
  if (de) { fbNoteAuthLocal((String("refresh parse: ") + de.c_str() + " (body " + resp.length() + "B)").c_str()); return false; }
  String uid = r["user_id"] | "";
  if (uid.length()) fbUid = uid;
  return fbStoreToken(r["id_token"] | "", r["refresh_token"] | "", (r["expires_in"] | String("3600")).toInt());
}

// Provisioning sign-in. This is the only request that carries the device password.
static bool fbPasswordSignIn(const String &apiKey, const String &email, const String &pass) {
  StaticJsonDocument<384> req;
  req["email"] = email; req["password"] = pass; req["returnSecureToken"] = true;
  String body; serializeJson(req, body);
  if (!fbBegin("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + apiKey)) {
    fbAuthHttp = -1; fbNoteAuthError(-1, ""); return false;
  }
  fbHttp.addHeader("Content-Type", "application/json");
  int code = fbHttp.POST(body);
  String resp = fbHttp.getString();                       // always drain -- see fbRefreshToken
  fbHttp.end();
  fbAuthHttp = code;
  if (code != 200) { fbNoteAuthError(code, resp); return false; }
  // 4096 for the same reason as fbRefreshToken: an Email/Password success body measured 1504 B
  // (982 B idToken + 268 B refreshToken + the rest), needing ~1594 B of document. See that note.
  StaticJsonDocument<4096> r;
  DeserializationError de = deserializeJson(r, resp);      // name the failure -- see fbRefreshToken
  if (de) { fbNoteAuthLocal((String("sign-in parse: ") + de.c_str() + " (body " + resp.length() + "B)").c_str()); return false; }
  // localId is the account's UID. The firmware never needs it (the token carries it, and rules
  // compare auth.uid server-side) but you need it to WRITE those rules -- so surface it.
  String uid = r["localId"] | "";
  if (uid.length()) fbUid = uid;
  return fbStoreToken(r["idToken"] | "", r["refreshToken"] | "", (r["expiresIn"] | String("3600")).toInt());
}

// Core 0: guarantee a usable ID token, or fail fast under backoff. Called before every request.
static bool fbEnsureToken() {
  if (fbIdToken.length() && (long)(millis() - fbTokenExpiryMs) < 0) return true;   // still valid
  if (fbAuthNextTryMs && (long)(millis() - fbAuthNextTryMs) < 0) return false;     // backing off

  String apiKey, email, pass, refresh;
  bool force;
  xSemaphoreTake(netMux, portMAX_DELAY);
  apiKey = fbApiKey; email = fbEmail; pass = fbPassword; refresh = fbRefresh;
  xSemaphoreGive(netMux);
  force = fbSignInPending; fbSignInPending = false;
  if (apiKey.length() < 8) {
    fbAuthOk = false;
    fbNoteAuthLocal("no Web API key stored (portal form, or FBASE,APIKEY,<key>)");
    return false;
  }

  bool ok = false, tried = false;
  // Prefer the refresh token; only fall back to the password when provisioning or when the
  // refresh token has been revoked/expired (a forced sign-in from the portal also lands here).
  if (!force && refresh.length()) { tried = true; ok = fbRefreshToken(apiKey, refresh); }
  if (!ok && email.length() && pass.length()) {
    tried = true;
    ok = fbPasswordSignIn(apiKey, email, pass);
    if (ok) fbCredsPersistPending = true;                                          // persist the new refresh token
  }
  if (!ok) {
    fbAuthOk = false;
    // Nothing was even attempted: the key is present but there is no password and no refresh
    // token, so there is no credential to exchange. Previously this fell straight into the
    // backoff and reported nothing at all.
    if (!tried) fbNoteAuthLocal("have API key but no password and no refresh token");
    fbIdToken = "";                                                                // don't reuse a dead token
    // Escalate: 30 s -> 2 m -> 8 m -> 30 m. Retrying a wrong password every poll is
    // credential hammering and Google will rate-limit or lock the account.
    fbAuthNextTryMs = millis() + FB_AUTH_BACKOFF[fbAuthFails < 4 ? fbAuthFails : 3];
    if (fbAuthFails < 4) fbAuthFails++;
  }
  return ok;
}

// Core 0: publish the latest verified snapshot. Reads ONLY from the telem snapshot (never core-1's
// `sensor`/`wo`/`sysState`) so the dashboard can't show a torn mix of old and new readings.
static bool firebaseUploadLive() {
  // Stamp the attempt FIRST, unconditionally: every early return below still consumes this tick.
  // Stamping only on success would leave the netTask cadence gate permanently open and retry the
  // whole path every 500 ms loop iteration (worst at boot, before the first telemetryCollect()).
  fbLastUploadMs = millis();

  String base;
  bool enabled;
  xSemaphoreTake(netMux, portMAX_DELAY); base = fbUrl; enabled = firebaseEnabled; xSemaphoreGive(netMux);
  if (!enabled || base.length() < 8) { fbLastOk = false; return false; }

  TelemetrySnapshot t;
  portENTER_CRITICAL(&telemMux); t = telem; portEXIT_CRITICAL(&telemMux);
  if (!t.valid || WiFi.status() != WL_CONNECTED) { fbLastOk = false; return false; }

  // An mbedTLS session needs ~40-50 KB. Skipping a dashboard tick is much cheaper than a failed
  // alloc mid-handshake, and fbLastHeap makes the long-run fragmentation trend visible on PAGE_GSM.
  uint32_t heap = ESP.getFreeHeap();
  fbLastHeap = heap;
  if (heap < FIREBASE_MIN_HEAP) { fbLastOk = false; return false; }

  if (!fbEnsureToken()) { fbLastOk = false; return false; }   // no token -> no request (respects backoff)

  // 1536 B covers all NUM_COLUMNS zones with headroom. Overflow is CHECKED below, not assumed: an
  // over-capacity document serializes silently truncated, which would PUT malformed JSON.
  StaticJsonDocument<1536> doc;
  JsonObject meta = doc.createNestedObject("meta");
  meta.createNestedObject("updatedAt")[".sv"] = "timestamp";   // server clock; millis() is not wall time
  meta["deviceOnline"] = true;
  // Publish the cadence so the dashboard can compute its own staleness threshold and grey out the
  // control buttons when this snapshot goes cold. With remote e-stop in scope, a page that cannot
  // tell "idle" from "device offline" is the dangerous failure mode.
  meta["refreshMs"] = (sysState == ACTIVE_STATE) ? FIREBASE_UPLOAD_ACTIVE_MS : FIREBASE_UPLOAD_IDLE_MS;

  JsonObject system = doc.createNestedObject("system");
  system["state"] = stateName(t.state);
  system["masterWorkOrderActive"] = t.woActive;

  JsonObject sensors = doc.createNestedObject("sensors");
  if (t.tankValid)  { sensors["reservoirLevel"] = t.resLevel; sensors["mixingLevel"] = t.mixLevel; sensors["flowRate"] = t.flow; }
  if (t.envValid)   { sensors["temperature"] = t.temp; sensors["humidity"] = t.hum; }
  if (t.lightValid) sensors["lightLevel"] = t.lux;
  if (t.inaValid)   sensors["batteryVoltage"] = t.battV;

  JsonObject zones = sensors.createNestedObject("zones");
  for (int c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c]) continue;
    JsonObject z = zones.createNestedObject(String(COL_TAG[c]));
    if (t.soil[c] >= 0) z["moisture"] = t.soil[c];
    if (t.npkValid[c]) {
      z["nitrogen"] = t.npkN[c]; z["phosphorus"] = t.npkP[c]; z["potassium"] = t.npkK[c];
      z["ec"] = t.npkEC[c]; z["ph"] = t.npkPH[c];
    }
  }

  // Past every skip condition -- this tick is a real attempt, so count it before anything that can fail.
  fbAttempts++;
  if (doc.overflowed()) { fbLastOk = false; fbLastHttp = -100; fbFailures++; return false; }  // never PUT truncated JSON
  String payload;
  serializeJson(doc, payload);

  if (!fbBegin(fbBuildUrl(base, "/irrigation/live.json"))) { fbLastOk = false; fbLastHttp = 0; fbFailures++; return false; }
  fbHttp.addHeader("Content-Type", "application/json");
  int code = fbHttp.PUT(payload);
  fbHttp.end();                                            // setReuse(true): keeps the socket, frees the request
  fbLastHttp = code;
  fbLastOk = (code >= 200 && code < 300);
  if (!fbLastOk) fbFailures++;
  if (code == 401) { fbIdToken = ""; fbTokenExpiryMs = 0; }  // token rejected -> force a renew next tick
  return fbLastOk;
}

/* =============================================================================
 *  FIREBASE REMOTE COMMANDS  (core 0 transports, core 1 decides)
 * ========================================================================== */
// Core 1 -> core 0: queue a result for the dashboard. Safe to call from any control path.
void firebaseQueueStatus(const char *id, const char *status, const char *detail) {
  if (!fbStatusQueue || !id || !*id) return;
  FirebaseCommandStatus u = {};
  strlcpy(u.id, id, sizeof(u.id));
  strlcpy(u.status, status, sizeof(u.status));
  strlcpy(u.detail, detail, sizeof(u.detail));
  xQueueSend(fbStatusQueue, &u, 0);                        // never block the control loop
}

// Core 1: a remote pump test reached a terminal state. Report it and release the slot.
// EVERY terminal path must call this: fbRemoteExerciseId is part of the idle gate, so leaving
// it set after a failure would reject all future remote commands as "not safely idle".
void firebaseRemoteExerciseDone(const char *status, const char *detail) {
  if (!fbRemoteExerciseId[0]) return;
  firebaseQueueStatus(fbRemoteExerciseId, status, detail);
  fbRemoteExerciseId[0] = 0;
}

// Core 0: fetch at most one queued command. One per poll keeps operation strictly sequential.
static void firebasePollCommands() {
  if (millis() - fbLastCommandPollMs < FIREBASE_COMMAND_POLL_MS) return;
  fbLastCommandPollMs = millis();

  String base; bool enabled;
  xSemaphoreTake(netMux, portMAX_DELAY); base = fbUrl; enabled = firebaseEnabled; xSemaphoreGive(netMux);
  if (!enabled || base.length() < 8 || WiFi.status() != WL_CONNECTED) return;
  if (!fbCommandQueue || !fbEnsureToken()) return;

  // Fetch only the NEWEST few commands, not the whole node. The firmware never deletes processed
  // commands (the dashboard renders their status/detail), so this node grows without bound -- measured
  // at 28 nodes / 4.6 KB on the live rig. Parsing all of that overflowed the document below, and
  // deserializeJson's NoMemory made EVERY command invisible from then on: press the web button a few
  // times and the device stops acknowledging forever. Firebase push keys are chronological, so the
  // newest entries are always inside this window, and $key ordering needs no index rule. Making the
  // response size independent of history is what actually fixes that.
  // fbBuildUrl already appended "?auth=<token>", hence the leading '&'.
  // limitToLast=10, not 5: the poll takes ONE command per 3 s tick (sequential by design), so the
  // window has to outlast a burst of clicks. Anything that scrolls out of it is never seen and stays
  // "queued" forever -- the same invisible-loss failure this whole change exists to remove, just at a
  // higher threshold. 10 nodes is ~550 B filtered, comfortably inside the 2048 B document.
  if (!fbBegin(fbBuildUrl(base, "/irrigation/commands.json") + "&orderBy=%22%24key%22&limitToLast=10")) return;
  int code = fbHttp.GET();
  String resp = (code == 200) ? fbHttp.getString() : "";
  fbHttp.end();
  if (code == 401) { fbIdToken = ""; fbTokenExpiryMs = 0; return; }
  if (code != 200 || resp.length() == 0 || resp == "null") return;

  // Filter: we only ever read these four fields, so ArduinoJson can skip the rest of the
  // document instead of allocating it. Keeps this off the netTask stack (was 6 KB in the fork).
  StaticJsonDocument<256> filter;
  JsonObject any = filter.createNestedObject("*");
  any["status"] = true; any["type"] = true;
  JsonObject pf = any.createNestedObject("payload");
  pf["pump"] = true;
  pf["columns"] = true; pf["liters"] = true;      // FORCE_RUN: which columns, TOTAL litres
  pf.createNestedObject("doseMl");                 // FORCE_RUN: {"A":mL,"B":mL,"C":mL}

  // 2048 as belt-and-braces on top of the limitToLast bound above. The failure is now REPORTED
  // rather than swallowed: a bare `return` here is what let a NoMemory look like "the device just
  // stopped responding" for days -- the same trap as the auth path.
  StaticJsonDocument<2048> doc;
  DeserializationError de = deserializeJson(doc, resp, DeserializationOption::Filter(filter));
  if (de) {
    static unsigned long lastParseLogMs = 0;                  // dedupe: this path polls every 3 s
    if (millis() - lastParseLogMs > 60000) {
      lastParseLogMs = millis();
      logEvent("FIREBASE", "POLL", String("parse ") + de.c_str() + "|body=" + resp.length() + "B");
    }
    return;
  }
  if (!doc.is<JsonObject>()) return;

  for (JsonPair item : doc.as<JsonObject>()) {
    JsonObject req = item.value().as<JsonObject>();
    if (String(req["status"] | "") != "queued") continue;
    FirebaseCommand c = {};
    strlcpy(c.id,   item.key().c_str(),          sizeof(c.id));
    strlcpy(c.type, req["type"] | "",            sizeof(c.type));
    strlcpy(c.pump, req["payload"]["pump"] | "", sizeof(c.pump));
    // FORCE_RUN extras. "columns" accepts "AB" or ["A","B"]; anything else leaves cols empty and
    // core 1 rejects the request rather than guessing which column to water.
    JsonVariant jc = req["payload"]["columns"];
    if (jc.is<const char *>()) strlcpy(c.cols, jc.as<const char *>(), sizeof(c.cols));
    else if (jc.is<JsonArray>()) {
      uint8_t k = 0;
      for (JsonVariant e : jc.as<JsonArray>()) {
        const char *s = e.as<const char *>();
        if (s && *s && k < sizeof(c.cols) - 1) c.cols[k++] = s[0];
      }
      c.cols[k] = '\0';
    }
    c.liters    = req["payload"]["liters"] | 0.0f;
    c.doseMl[0] = req["payload"]["doseMl"]["A"] | 0.0f;
    c.doseMl[1] = req["payload"]["doseMl"]["B"] | 0.0f;
    c.doseMl[2] = req["payload"]["doseMl"]["C"] | 0.0f;
    FirebaseCommandStatus ack = {};
    strlcpy(ack.id, c.id, sizeof(ack.id));
    // Mark it non-"queued" immediately, so the next poll cannot hand the same request to
    // core 1 twice and start two pump tests from one dashboard click.
    if (xQueueSend(fbCommandQueue, &c, 0) == pdTRUE) {
      strlcpy(ack.status, "received", sizeof(ack.status));
      strlcpy(ack.detail, "master validating", sizeof(ack.detail));
    } else {
      strlcpy(ack.status, "rejected", sizeof(ack.status));
      strlcpy(ack.detail, "master busy", sizeof(ack.detail));
    }
    xQueueSend(fbStatusQueue, &ack, 0);
    break;                                                 // exactly one command per poll
  }
}

// Core 0: PATCH queued results back. Separated from control work so a dead link never
// stalls the actuator side -- the queue simply drains when connectivity returns.
static void firebaseFlushStatus() {
  if (!fbStatusQueue) return;
  String base; bool enabled;
  xSemaphoreTake(netMux, portMAX_DELAY); base = fbUrl; enabled = firebaseEnabled; xSemaphoreGive(netMux);
  if (!enabled || base.length() < 8 || WiFi.status() != WL_CONNECTED) return;

  FirebaseCommandStatus u;
  while (xQueuePeek(fbStatusQueue, &u, 0) == pdTRUE) {
    if (!fbEnsureToken()) return;                          // leave it queued; retry next tick
    StaticJsonDocument<192> body;
    body["status"] = u.status; body["detail"] = u.detail;
    body.createNestedObject("updatedAt")[".sv"] = "timestamp";
    String json; serializeJson(body, json);
    String path = String("/irrigation/commands/") + u.id + ".json";
    if (!fbBegin(fbBuildUrl(base, path.c_str()))) return;
    fbHttp.addHeader("Content-Type", "application/json");
    int code = fbHttp.PATCH(json);
    fbHttp.end();
    if (code == 401) { fbIdToken = ""; fbTokenExpiryMs = 0; return; }
    if (code < 200 || code >= 300) return;                 // keep it queued and retry later
    xQueueReceive(fbStatusQueue, &u, 0);                   // confirmed delivered -> drop it
  }
}

// Core 1: validate and dispatch. This is the ONLY place a remote request can touch the rig,
// and it goes through the same gates and the same bounded flows the local UI uses.
void firebaseCommandTick() {
  if (!fbCommandQueue) return;
  FirebaseCommand c;
  while (xQueueReceive(fbCommandQueue, &c, 0) == pdTRUE) {
    // ---- FORCE_RUN: operator-specified irrigation/fertigation -------------------------------
    // Unlike the other remote commands this actuates a REAL run, so every input is bounded here on
    // the control core before anything is sent. "Force" overrides the schedule window, the soil
    // threshold and serviced-today -- it does NOT override hardware safety.
    if (!strcmp(c.type, "FORCE_RUN")) {
      uint8_t mask = 0;
      for (const char *p = c.cols; *p; p++) {
        int b = (*p == 'A') ? 0 : (*p == 'B') ? 1 : (*p == 'C') ? 2 : -1;
        if (b >= 0 && COLUMN_ENABLED[b]) mask |= (uint8_t)(1 << b);
      }
      if (!mask) {
        firebaseQueueStatus(c.id, "rejected", "no valid enabled column in 'columns'");
        logEvent("FIREBASE", "REJECT", "FORCE|NO_COLUMN"); continue;
      }
      if (c.liters <= 0.0f || c.liters > FORCE_MAX_LITERS) {
        firebaseQueueStatus(c.id, "rejected", "liters out of range");
        logEvent("FIREBASE", "REJECT", "FORCE|LITERS=" + String(c.liters, 1)); continue;
      }
      bool overDose = false;
      for (int i = 0; i < 3; i++) if (c.doseMl[i] < 0.0f || c.doseMl[i] > FORCE_MAX_DOSE_ML) overDose = true;
      if (overDose) {
        firebaseQueueStatus(c.id, "rejected", "nutrient mL out of range");
        logEvent("FIREBASE", "REJECT", "FORCE|DOSE_RANGE"); continue;
      }
      // Same hardware gate the local UI honours. Schedule/soil are deliberately NOT checked.
      if (sysState != IDLE_STATE || uiMode != UI_DATA || wo.active || pendingRun.active ||
          pendingExercise.active || esp2Held || fbRemoteExerciseId[0]) {
        firebaseQueueStatus(c.id, "rejected", "system is not safely idle");
        logEvent("FIREBASE", "REJECT", "FORCE|NOT_IDLE"); continue;
      }
      if (sensor.tankValid && sensor.resLevel < RES_LOW_PCT) {
        firebaseQueueStatus(c.id, "rejected", "reservoir too low");
        logEvent("FIREBASE", "REJECT", "FORCE|RES_LOW"); continue;
      }
      sendForceWorkOrder(mask, c.liters, c.doseMl, c.id);
      continue;
    }

    if (!strcmp(c.type, "EMERGENCY_STOP")) {
      enterEmergencyStop(true);                            // existing system-wide physical stop
      logEvent("FIREBASE", "CMD", "ESTOP");
      firebaseQueueStatus(c.id, "completed", "emergency stop executed");
      continue;
    }

    // START_PUMP is accepted for dashboard compatibility but is deliberately interpreted as the
    // firmware's existing 5 s preventive exercise -- never an open-ended remote run.
    int pump = -1;
    if (!strcmp(c.type, "RUN_PUMP_TEST") || !strcmp(c.type, "START_PUMP")) {
      if      (!strcmp(c.pump, "transfer")) pump = 0;
      else if (!strcmp(c.pump, "booster"))  pump = 1;
      else if (!strcmp(c.pump, "mixer"))    pump = 2;
    } else if (!strcmp(c.type, "TOGGLE_MIXER")) pump = 2;

    if (pump < 0) {
      firebaseQueueStatus(c.id, "rejected", "not a remotely safe control");
      logEvent("FIREBASE", "REJECT", String(c.type));
      continue;
    }
    // Same gate the local exerciseTick() honours, plus the UI modes: never preempt a run,
    // a held fault, Testing/Calibration, or another in-flight remote test.
    if (sysState != IDLE_STATE || uiMode != UI_DATA || wo.active || pendingRun.active ||
        pendingExercise.active || esp2Held || fbRemoteExerciseId[0]) {
      firebaseQueueStatus(c.id, "rejected", "system is not safely idle");
      logEvent("FIREBASE", "REJECT", "NOT_IDLE");
      continue;
    }

    // Rate-limit only ACTUAL actuations, and only AFTER every other check has passed. Starting the
    // cooldown earlier (before validation, as a naive placement does) means a command rejected for
    // an unrelated reason -- not idle, unknown pump -- still arms a 10 s block, so the operator's
    // legitimate retry is refused too and the button looks permanently dead.
    if (fbLastRemoteActionMs && millis() - fbLastRemoteActionMs < FB_REMOTE_MIN_GAP_MS) {
      unsigned long waitS = (FB_REMOTE_MIN_GAP_MS - (millis() - fbLastRemoteActionMs) + 999) / 1000;
      firebaseQueueStatus(c.id, "rejected", (String("wait ") + waitS + "s between commands").c_str());
      logEvent("FIREBASE", "REJECT", "THROTTLE");
      continue;
    }
    fbLastRemoteActionMs = millis();

    // Arm exactly as exerciseTick() does -- including acked=false, which the ACK/START log and
    // the NOACK-vs-NODONE timeout distinction both depend on.
    pendingExercise.active = true; pendingExercise.idx = pump;
    pendingExercise.sent = false;  pendingExercise.acked = false;
    strlcpy(fbRemoteExerciseId, c.id, sizeof(fbRemoteExerciseId));
    esp2WarmupMs = millis();
    setState(ACTIVE_STATE);
    if (esp2Available && esp2Powered) dispatchPendingExercise();   // already up
    else                              esp2SetPower(true);          // warm up; READY dispatches
    firebaseQueueStatus(c.id, "accepted", "5 second pump test started");
    logEvent("FIREBASE", "CMD", String("PUMP_TEST|") + EX_NAME[pump]);
  }
}

// Core 1: publish Firebase health to the SD log. Logs only on an ok<->fail TRANSITION (same
// log-on-change discipline as the battery PWR logs) -- a per-upload line would be ~1440 rows/day.
// Carries free heap and netTask stack headroom, which is how the long-run TLS fragmentation risk
// (a fresh mbedTLS session every ~60 s) becomes visible in the CSV instead of only on the LCD.
void firebaseLogTick() {
  // Auth failures land in the CSV so you can reproduce the fault with the portal CLOSED
  // (the only state in which Firebase is actually attempted) and read the reason afterwards.
  if (fbAuthErrLogPending) {
    fbAuthErrLogPending = false;
    logEvent("ESP1", "FBASE", String("AUTH|") + fbAuthErrLog);
  }
  static int  lastState = -1;                       // -1 unknown, 0 fail, 1 ok
  static unsigned long lastSeenUploadMs = 0;
  if (fbLastUploadMs == lastSeenUploadMs) return;   // no new attempt since the last check
  lastSeenUploadMs = fbLastUploadMs;
  // netMux: the core-0 admin form may be rewriting these while this core-1 tick reads them.
  // Unconfigured/disabled is not a failure -- don't log a FAIL line for a feature that is simply off.
  bool configured;
  xSemaphoreTake(netMux, portMAX_DELAY); configured = (firebaseEnabled && fbUrl.length() >= 8); xSemaphoreGive(netMux);
  if (!configured) { lastState = -1; return; }      // re-arm so the first real result still logs
  int now = fbLastOk ? 1 : 0;
  if (now == lastState) return;
  lastState = now;
  uint32_t stackFree = netTaskHandle ? uxTaskGetStackHighWaterMark(netTaskHandle) : 0;
  // auth= and url= make an n=0 row self-explanatory: with no attempt counted, the cause is
  // always upstream of the request, and it is one of these two.
  logEvent("ESP1", "FBASE", String(now ? "OK" : "FAIL") + "|http=" + String(fbLastHttp) +
                            "|auth=" + String(fbAuthOk ? "ok" : "no") +
                            "|url=" + String(fbUrl.length() >= 8 ? "set" : "unset") +
                            "|heap=" + String(fbLastHeap) + "|netstk=" + String(stackFree) +
                            "|n=" + String(fbAttempts) + "|fail=" + String(fbFailures));
}

/* =============================================================================
 *  SUPABASE STORAGE UPLOAD  (core 0 / netTask)  --  stream a completed daily CSV to a private bucket.
 *  Endpoint: POST <supaUrl>/storage/v1/object/CSV-Logs/<objName>  (x-upsert:true = idempotent retries).
 *  The body is STREAMED straight from the SD File (HTTPClient pulls ~1.4 KB at a time with a fixed
 *  Content-Length), so heap stays flat no matter how big the file is. sdMux is held for the transfer;
 *  the core-1 logger's logFlush uses a timed take and just buffers meanwhile (never blocks the WDT).
 * ========================================================================== */
bool supaUploadFile(const char *path, const char *objName) {
  String base, key;
  xSemaphoreTake(netMux, portMAX_DELAY); base = supaUrl; key = supaKey; xSemaphoreGive(netMux);
  if (base.length() == 0 || key.length() == 0) { uploadHttp = 0; return false; }   // not configured
  if (!wifiConnected) { uploadHttp = 0; return false; }

  if (!sdTake(4000)) { uploadHttp = -1; return false; }            // SD busy (portal/summary) -> retry later
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) { if (f) f.close(); sdGive(); uploadHttp = -2; return false; }
  size_t len = f.size();

  WiFiClientSecure client;
  client.setInsecure();                                            // encrypted; no cert pinning (thesis) [HARDENING TODO]
  client.setTimeout(15000);
  HTTPClient http;
  String url = base + "/storage/v1/object/" + SUPA_BUCKET + "/" + objName;
  bool ok = false; int code = 0;
  if (http.begin(client, url)) {
    http.setConnectTimeout(8000);
    http.setTimeout(20000);
    http.addHeader("Authorization", "Bearer " + key);
    http.addHeader("apikey", key);
    http.addHeader("Content-Type", "text/csv");
    http.addHeader("x-upsert", "true");                            // overwrite if it already exists
    code = http.sendRequest("POST", &f, len);                      // streams from SD, no full-file buffer
    http.end();
    ok = (code == 200 || code == 201);
  }
  f.close();
  sdGive();
  uploadHttp = code;
  return ok;
}

/* ---- WiFi provisioning portal (SoftAP + captive web form) -------------------
 * Lives on core 0 (serviced from netTask). Builds an HTML form listing scanned
 * SSIDs; POST /save stages creds under netMux and flags the core-1 loop to persist
 * (no NVS write from core 0). All Serial-only logging here (logBuf is core-1 only). */
WebServer     portalServer(80);
DNSServer     portalDns;
IPAddress     portalApIpAddr;
unsigned long portalStartMs = 0;
bool          portalSaved   = false;
String        portalScanOpts;                                // cached <option> list (scan once, not per GET /)

// Escape a string for safe insertion into portal HTML (attributes + text). Prevents a hostile nearby
// SSID (or a column name) with quotes/angle-brackets from breaking the markup or injecting script.
static String htmlEscape(const String &s) {
  String o; o.reserve(s.length() + 8);
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

// Scan nearby networks ONCE (blocking ~2 s) and cache the <option> list. Repeating this on every page
// load would keep taking the STA off-channel in AP_STA mode and bump the phone off the AP.
static void portalDoScan() {
  int n = WiFi.scanNetworks(false, true);                    // sync scan, include hidden
  String o;
  for (int i = 0; i < n && i < 30; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    String e = htmlEscape(s);                                // SSID is attacker-controlled -> escape
    o += "<option value='" + e + "'>" + e + "  (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();
  portalScanOpts = o;
}

static String portalFormHtml() {
  String o = F("<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Irrigation WiFi Setup</title></head>"
               "<body style='font-family:sans-serif;max-width:420px;margin:16px auto;padding:0 12px'>"
               "<h2>Irrigation WiFi Setup</h2><form method=POST action=/save>"
               "<p>Nearby network (<a href=/rescan>rescan</a>):<br><select name=ssid style='width:100%;padding:8px'>");
  o += portalScanOpts;                                       // cached at portalStart / /rescan
  o += F("</select></p>"
         "<p>...or type a hidden SSID:<br><input name=ssid_manual style='width:100%;padding:8px'></p>"
         "<p>Password:<br><input name=pass type=password style='width:100%;padding:8px'></p>"
         "<p><button type=submit style='width:100%;padding:12px;font-size:1em'>Save &amp; Connect</button></p>"
         "</form>"
         "<hr><h2>Admin</h2><form method=POST action=/admin>"      // PIN-gated: logs / owner / format
         "<p>Admin PIN:<br><input name=pin type=password style='width:100%;padding:8px'></p>"
         "<p><button type=submit style='width:100%;padding:12px'>Unlock admin</button></p></form>"
         "</body></html>");
  return o;
}
static void portalHandleRoot() { portalServer.send(200, "text/html", portalFormHtml()); }
static void portalHandleRescan() {                            // GET /rescan -- refresh the cached SSID list
  if (portalActive) portalDoScan();
  portalServer.sendHeader("Location", String("http://") + portalApIpAddr.toString(), true);
  portalServer.send(302, "text/plain", "");
}
static void portalHandleSave() {
  String ss = portalServer.arg("ssid_manual"); ss.trim();
  if (ss.length() == 0) ss = portalServer.arg("ssid");
  String pw = portalServer.arg("pass");
  if (ss.length() == 0) {
    portalServer.send(200, "text/html", F("<html><body style='font-family:sans-serif'>"
                                          "<h3>No SSID selected.</h3><a href=/>Go back</a></body></html>"));
    return;
  }
  xSemaphoreTake(netMux, portMAX_DELAY); wifiSsid = ss; wifiPass = pw; xSemaphoreGive(netMux);
  wifiEnabled = true;                                          // saving creds implies "connect" -> ensure radio on
  wifiPersistPending = true; portalSaved = true;
  portalServer.send(200, "text/html", "<html><body style='font-family:sans-serif'><h3>Saved.</h3>"
                    "<p>Connecting to <b>" + htmlEscape(ss) + "</b>&hellip; you can close this page.</p></body></html>");
}
static void portalHandleNotFound() {                          // captive-portal: redirect probes to the form
  portalServer.sendHeader("Location", String("http://") + portalApIpAddr.toString(), true);
  portalServer.send(302, "text/plain", "");
}

/* ---- Portal admin (PIN-gated): SD logs (list/view/download/format) + owner-number edit --------
 * Runs on core 0. SD access takes sdMux; the owner-number write is staged to core 1 (pendingOwner).
 * Path params are strictly sanitized to a root-level *.CSV basename (no traversal). */
static String portalHead(const String &title) {
  return "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>" + title + "</title></head>"
         "<body style='font-family:sans-serif;max-width:460px;margin:16px auto;padding:0 12px'>";
}
// Allow only "/<BASENAME>.CSV" (letters/digits/_), no path separators or "..". "" = reject.
static String portalSafeCsv(const String &f) {
  if (f.length() < 5 || f.length() > 20) return "";
  String u = f; u.toUpperCase();
  if (!u.endsWith(".CSV")) return "";
  for (unsigned i = 0; i < f.length() - 4; i++) {
    char c = f[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return "";
  }
  return "/" + f;
}
static bool portalRequireAdmin() {                            // gate: send a "locked" page + return false
  if (portalAdminUnlocked) return true;
  portalServer.send(200, "text/html", portalHead("Admin") + "<h3>Locked.</h3><p>Enter the admin PIN.</p><a href=/>Back</a></body></html>");
  return false;
}
// Validate the ?col= form field to a single column tag A/B/C (0 = invalid).
static char portalColArg() {
  String cv = portalServer.arg("col"); cv.toUpperCase();
  return (cv.length() == 1 && cv[0] >= 'A' && cv[0] <= 'C') ? cv[0] : 0;
}
// Config forms: ThingSpeak keys, per-column mode/name/targets/preset, thresholds. Each posts structured
// fields; the handler builds an SMS-format command and enqueues it for the core-1 handleSms replay.
static String portalConfigHtml() {
  String o;
  bool k1, k2, k3;
  xSemaphoreTake(netMux, portMAX_DELAY); k1 = tsKey1.length() > 0; k2 = tsKey2.length() > 0; k3 = tsKey3.length() > 0; xSemaphoreGive(netMux);
  o += "<h3>ThingSpeak keys</h3><form method=POST action=/tskey>"
       "<p>Channel: <select name=ch>"
       "<option value=1>1 Columns (" + String(k1 ? "set" : "unset") + ")</option>"
       "<option value=2>2 System (" + String(k2 ? "set" : "unset") + ")</option>"
       "<option value=3>3 Chem (" + String(k3 ? "set" : "unset") + ")</option></select></p>"
       "<p>Write key:<br><input name=key style='width:100%;padding:8px'></p>"
       "<p><button type=submit style='width:100%;padding:10px'>Save key</button></p></form>";
  for (int c = 0; c < NUM_COLUMNS; c++) {
    String tag = String(COL_TAG[c]);
    bool irr = (col[c].mode == MODE_IRRIGATION_ONLY);
    o += "<h3>Column " + tag + "</h3>";
    o += "<form method=POST action=/colmode><input type=hidden name=col value=" + tag + ">"
         "<p>Mode: <label><input type=radio name=mode value=AUTO " + String(irr ? "" : "checked") + ">AUTO</label> "
         "<label><input type=radio name=mode value=IRRIGATION_ONLY " + String(irr ? "checked" : "") + ">Irrig-only</label> "
         "<button type=submit>Set</button></p></form>";
    o += "<form method=POST action=/colname><input type=hidden name=col value=" + tag + ">"
         "<p>Name: <input name=name value='" + htmlEscape(String(col[c].name)) + "' style='padding:6px'> <button type=submit>Rename</button></p></form>";
    o += "<form method=POST action=/coltargets><input type=hidden name=col value=" + tag + ">"
         "<p>N<input name=n type=number value=" + String((int)col[c].targetN) + " style='width:56px'> "
         "P<input name=p type=number value=" + String((int)col[c].targetP) + " style='width:56px'> "
         "K<input name=k type=number value=" + String((int)col[c].targetK) + " style='width:56px'> "
         "pH<input name=ph type=number step=0.1 value=" + String(col[c].targetPH, 1) + " style='width:64px'> "
         "<button type=submit>Save targets</button></p></form>";
    o += "<form method=POST action=/colpreset><input type=hidden name=col value=" + tag + ">"
         "<p>Preset: <select name=preset>";
    for (int p = 0; p < NUM_PRESETS; p++) o += "<option>" + String(CROP_PRESETS[p].name) + "</option>";
    o += "</select> <button type=submit>Apply</button></p></form>";
  }
  o += "<h3>Thresholds</h3><form method=POST action=/thresh>"
       "<p>Start&lt;<input name=start type=number value=" + String(soilStartPct) + " style='width:56px'>% "
       "Stop&gt;<input name=stop type=number value=" + String(soilStopPct) + " style='width:56px'>% "
       "gap<input name=gap type=number value=" + String((int)fertGap) + " style='width:56px'>mg/kg "
       "<button type=submit>Save</button></p></form>";
  return o;
}
static String portalAdminHtml(const String &msg) {            // the unlocked admin dashboard
  String o = portalHead("Irrigation Admin") + "<h2>Admin</h2>";
  if (msg.length()) o += "<p style='color:#0a0'><b>" + msg + "</b></p>";
  String owner; xSemaphoreTake(netMux, portMAX_DELAY); owner = PHONE_NUMBER; xSemaphoreGive(netMux);
  o += "<h3>Owner number</h3><form method=POST action=/owner>"
       "<p>Current: <b>" + htmlEscape(owner) + "</b><br><input name=owner value='" + htmlEscape(owner) + "' style='width:100%;padding:8px'></p>"
       "<p><button type=submit style='width:100%;padding:10px'>Save owner</button></p></form>";
  o += portalConfigHtml();                                     // ThingSpeak keys + columns + thresholds
  o += "<h3>Logs</h3><table style='width:100%;border-collapse:collapse'>";
  if (sdTake(1000)) {
    File root = SD.open("/");
    if (root) {
      for (File e = root.openNextFile(); e; e = root.openNextFile()) {
        String nm = String(e.name()); int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
        String up = nm; up.toUpperCase();
        if (up.endsWith(".CSV"))
          o += "<tr><td>" + nm + "</td><td align=right>" + String((uint32_t)e.size()) + "B</td>"
               "<td><a href='/view?f=" + nm + "'>view</a> &middot; <a href='/download?f=" + nm + "'>get</a></td></tr>";
      }
      root.close();
    }
    sdGive();
  } else o += "<tr><td>(SD busy, reload)</td></tr>";
  o += "</table>";
  o += "<h3 style='color:#c33'>Format SD</h3><form method=POST action=/format "
       "onsubmit=\"return confirm('Erase ALL files on the SD card?')\">"
       "<p><label><input type=checkbox name=confirm value=yes> Yes, delete every log file.</label></p>"
       "<p><button type=submit style='width:100%;padding:10px;background:#c33;color:#fff'>Erase SD</button></p></form>";
  o += "<h3>Change admin PIN</h3><form method=POST action=/pin>"
       "<p><input name=pin type=password placeholder='new PIN (4-12)' style='width:100%;padding:8px'></p>"
       "<p><button type=submit style='width:100%;padding:10px'>Change PIN</button></p></form>";
  o += "<h3>Supabase cloud logs</h3><form method=POST action=/supa>"
       "<p>Project URL<br><input name=url style='width:100%' placeholder='https://<ref>.supabase.co' value='" + htmlEscape(supaUrl) + "'></p>"
       "<p>Service key (service_role JWT)<br><textarea name=key rows=3 style='width:100%' placeholder='" + String(supaKey.length() ? "(set - leave blank to keep)" : "paste the full key") + "'></textarea></p>"
       "<p><button type=submit style='width:100%;padding:10px'>Save Supabase creds</button></p></form>";
  o += "<form method=POST action=/upload>"
       "<p>Push yesterday's CSV to cloud storage" + String(supaUrl.length() ? "" : " (set the URL + key above first)") + ".</p>"
       "<p><button type=submit style='width:100%;padding:10px'>Upload logs now</button></p></form>";
  o += "<h3>Firebase live dashboard</h3><form method=POST action=/fbase>"
       "<p>RTDB URL<br><input name=url style='width:100%' placeholder='https://<ref>-default-rtdb.<region>.firebasedatabase.app' value='" + htmlEscape(fbUrl) + "'></p>"
       "<p><label><input type=checkbox name=en value=yes" + String(firebaseEnabled ? " checked" : "") + "> Enabled (live snapshot + remote commands)</label></p>"
       "<p>Last upload: " + String(fbUrl.length() == 0 ? "not configured"
                                   : (fbLastUploadMs == 0 ? "none yet"
                                      : String((millis() - fbLastUploadMs) / 1000) + " s ago, " + (fbLastOk ? "OK" : "FAILED"))) + "</p>"
       "<p><button type=submit style='width:100%;padding:10px'>Save Firebase settings</button></p></form>";
  // Device account. The password is write-only from here: it is never rendered back, and once a
  // refresh token exists it is no longer needed at all (Forget password below).
  o += "<h3>Firebase device account</h3><form method=POST action=/fbauth>"
       "<p>Web API key<br><input name=key style='width:100%' placeholder='" +
         String(fbApiKey.length() ? "(set - leave blank to keep)" : "AIza...") + "'></p>"
       "<p>Device email<br><input name=email style='width:100%' value='" + htmlEscape(fbEmail) + "'></p>"
       "<p>Device password<br><input name=pass type=password style='width:100%' placeholder='" +
         String(fbPassword.length() ? "(set - leave blank to keep)" : "used once, then discardable") + "'></p>"
       "<p>Token: <b>" + String(fbAuthOk ? "signed in" : "NOT signed in") + "</b>" +
         String(fbAuthHttp ? " (HTTP " + String(fbAuthHttp) + ")" : "") +
         " &middot; refresh token " + String(fbRefresh.length() ? "stored" : "none") +
         " &middot; TLS handshakes " + String(fbHandshakes) + "</p>";
  // Blocked-reason readout. Without this the page just says "not signed in" and gives you
  // nothing to act on -- which is exactly the state that makes provisioning feel broken.
  {
    // NOTE: while this page is open the setup AP owns netTask, so NO Firebase request is being
    // made right now. Everything below is the state from before the portal opened -- say so,
    // instead of reporting "WiFi is not connected", which is trivially true of every portal session.
    o += "<p><small><b>This page suspends Firebase.</b> The setup AP takes over the network task, so "
         "nothing is being attempted while you read this. The status below is from before you opened it. "
         "For a live view use the USB serial monitor at 115200, or send the SMS <code>FBASE,STATUS</code>. "
         "Auth failures are also written to the SD log as <code>FBASE,AUTH|...</code>.</small></p>";
    String why;
    if (!firebaseEnabled)           why = "Firebase is disabled (tick Enabled above)";
    else if (fbApiKey.length() < 8) why = "no Web API key set";
    else if (!fbEmail.length())     why = "no device email set";
    else if (!fbRefresh.length() && !fbPassword.length()) why = "no password and no refresh token";
    else if (fbUrl.length() < 8)    why = "no RTDB URL set -- sign-in will work, but nothing will publish";
    if (why.length())      o += "<p style='color:#c33'>Blocked: " + htmlEscape(why) + "</p>";
    if (fbAuthErr.length()) {
      o += "<p style='color:#c33'>Last error (" + String((millis() - fbAuthErrMs) / 1000) + " s ago): "
           + htmlEscape(fbAuthErr) + "</p>";
    }
    if (fbUid.length())
      o += "<p>Device UID: <code>" + htmlEscape(fbUid) + "</code><br>"
           "<small>Use this in your RTDB rules, e.g. <code>\".write\": \"auth.uid === '" + htmlEscape(fbUid) + "'\"</code>. "
           "The firmware itself never needs the UID.</small></p>";
  }
  o += "<p><label><input type=checkbox name=pin value=yes" + String(fbPinCa ? " checked" : "") + "> "
       "Validate server certificate (uncheck ONLY to test whether pinning is the problem)</label></p>"
       "<p><label><input type=checkbox name=signin value=yes> Sign in now (uses the password)</label></p>"
       "<p><button type=submit style='width:100%;padding:10px'>Save device account</button></p></form>";
  o += "<form method=POST action=/fbauth>"
       "<input type=hidden name=forget value=pass>"
       "<p>Once \"refresh token: stored\" appears, the password is no longer needed.</p>"
       "<p><button type=submit style='width:100%;padding:10px'>Forget password (keep signed in)</button></p></form>";
  o += "<h3 style='color:#c33'>Reboot</h3><form method=POST action=/reboot "
       "onsubmit=\"return confirm('Reboot the controller now?')\">"
       "<p><label><input type=checkbox name=confirm value=yes> Yes, reboot ESP1 (config kept).</label></p>"
       "<p><button type=submit style='width:100%;padding:10px;background:#c33;color:#fff'>Reboot ESP1</button></p></form>";
  o += "<hr><p><a href=/>&larr; WiFi setup</a></p></body></html>";
  return o;
}
static void portalHandleUpload() {                            // POST /upload -- queue the log push (core-1 resolves the day)
  if (!portalAdminUnlocked) { portalServer.send(403, "text/html", "locked"); return; }
  adminUploadReq = true;                                      // core-1 uploadTick resolves yesterday + queues it
  portalServer.send(200, "text/html", portalAdminHtml("Upload queued (sends when WiFi is up)."));
}
static void portalHandleAdmin() {                             // POST /admin -- verify PIN
  String pin = portalServer.arg("pin");
  String want; xSemaphoreTake(netMux, portMAX_DELAY); want = adminPin; xSemaphoreGive(netMux);
  if (pin.length() && pin == want) { portalAdminUnlocked = true; portalServer.send(200, "text/html", portalAdminHtml("Unlocked.")); }
  else portalServer.send(200, "text/html", portalHead("Admin") + "<h3>Wrong PIN.</h3><a href=/>Back</a></body></html>");
}
static void portalHandleOwner() {                             // POST /owner -- stage new owner number
  if (!portalRequireAdmin()) return;
  String o = portalServer.arg("owner"); o.trim();
  int digits = 0; for (unsigned i = 0; i < o.length(); i++) if (isDigit(o[i])) digits++;
  if (digits < 7 || o.length() > 15) { portalServer.send(200, "text/html", portalAdminHtml("Invalid number (7-15 digits).")); return; }
  xSemaphoreTake(netMux, portMAX_DELAY); pendingOwner = o; xSemaphoreGive(netMux);  // barrier for core-1 read
  ownerPersistPending = true;                                 // core-1 loop applies + persists
  portalServer.send(200, "text/html", portalAdminHtml("Owner saved: " + htmlEscape(o)));
}
static void portalHandlePin() {                               // POST /pin -- stage new admin PIN
  if (!portalRequireAdmin()) return;
  String p = portalServer.arg("pin"); p.trim();
  if (p.length() < 4 || p.length() > 12) { portalServer.send(200, "text/html", portalAdminHtml("PIN must be 4-12 chars.")); return; }
  xSemaphoreTake(netMux, portMAX_DELAY); pendingAdminPin = p; xSemaphoreGive(netMux);  // barrier for core-1 read
  adminPinPersistPending = true;
  portalServer.send(200, "text/html", portalAdminHtml("Admin PIN changed."));
}
static void portalHandleView() {                              // GET /view?f= -- inline tail (last 16 KB)
  if (!portalRequireAdmin()) return;
  String path = portalSafeCsv(portalServer.arg("f"));
  if (path == "") { portalServer.send(400, "text/plain", "bad file"); return; }
  if (!sdTake(2000)) { portalServer.send(503, "text/plain", "SD busy, retry"); return; }
  File f = SD.open(path, FILE_READ);
  if (!f) { sdGive(); portalServer.send(404, "text/plain", "not found"); return; }
  const uint32_t TAIL = 16384;
  uint32_t sz = f.size();
  bool tailed = sz > TAIL;
  if (tailed) f.seek(sz - TAIL);
  portalServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  portalServer.send(200, "text/plain", "");
  if (tailed) portalServer.sendContent("...(tail; full file via download)...\n");
  char buf[513];
  while (f.available()) { int r = f.read((uint8_t*)buf, 512); if (r <= 0) break; buf[r] = 0; portalServer.sendContent(buf); }
  f.close(); sdGive();
  portalServer.sendContent("");                               // finalize chunked response
}
static void portalHandleDownload() {                          // GET /download?f= -- whole file as attachment
  if (!portalRequireAdmin()) return;
  String path = portalSafeCsv(portalServer.arg("f"));
  if (path == "") { portalServer.send(400, "text/plain", "bad file"); return; }
  if (!sdTake(2000)) { portalServer.send(503, "text/plain", "SD busy, retry"); return; }
  File f = SD.open(path, FILE_READ);
  if (!f) { sdGive(); portalServer.send(404, "text/plain", "not found"); return; }
  portalServer.sendHeader("Content-Disposition", "attachment; filename=" + path.substring(1));
  portalServer.streamFile(f, "application/octet-stream");
  f.close(); sdGive();
}
// Recursively delete everything under `dir` (files removed, sub-dirs emptied then rmdir'd). Collects each
// level's names BEFORE deleting (deleting during openNextFile can invalidate the walk) and SKIPS -- never
// stops on -- an entry it can't remove, so one un-deletable item (e.g. a "System Volume Information" dir)
// no longer halts the whole erase. Caller holds sdMux. Returns the count of files removed.
static int portalRmrf(const String &dir, int depth) {
  int removed = 0;
  File d = SD.open(dir);
  if (!d) return 0;
  const int MAXN = 48;
  String child[MAXN]; bool isdir[MAXN]; int cnt = 0;
  for (File e = d.openNextFile(); e && cnt < MAXN; e = d.openNextFile()) {
    String nm = String(e.name());                             // normalize to basename (core may return a full path)
    int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
    if (nm.length()) { child[cnt] = (dir == "/") ? "/" + nm : dir + "/" + nm; isdir[cnt] = e.isDirectory(); cnt++; }
    e.close();
  }
  d.close();
  for (int i = 0; i < cnt; i++) {
    if (isdir[i]) { if (depth < 4) removed += portalRmrf(child[i], depth + 1); SD.rmdir(child[i]); }
    else if (SD.remove(child[i])) removed++;
  }
  return removed;
}
static void portalHandleFormat() {                            // POST /format -- delete all files (chosen "format")
  if (!portalRequireAdmin()) return;
  if (portalServer.arg("confirm") != "yes") { portalServer.send(200, "text/html", portalAdminHtml("Cancelled (box unchecked).")); return; }
  if (summaryStage != SUM_IDLE) { portalServer.send(200, "text/html", portalAdminHtml("SD busy (report reading). Retry.")); return; }
  int removed = 0;
  if (sdTake(8000)) {
    // Repeat passes so a root with >MAXN files (or dirs freed this pass) is fully cleared; stops when a
    // pass removes nothing (only un-deletable entries left) -- never spins.
    int r, guard = 0;
    do { r = portalRmrf("/", 0); removed += r; } while (r > 0 && ++guard < 100);
    SD.begin(SD_CS);                                           // ensure remounted; logging resumes into a fresh file
    sdGive();
  }
  portalServer.send(200, "text/html", portalAdminHtml("Erased " + String(removed) + " file(s). SD ready."));
}
static void portalHandleReboot() {                            // POST /reboot -- software-reset ESP1
  if (!portalRequireAdmin()) return;
  if (portalServer.arg("confirm") != "yes") { portalServer.send(200, "text/html", portalAdminHtml("Cancelled (box unchecked).")); return; }
  portalServer.send(200, "text/html", portalHead("Reboot") +
                    "<h3>Rebooting the controller&hellip;</h3><p>The setup AP will drop. Reconnect to your "
                    "network (or reopen Setup AP) in ~15 s.</p></body></html>");
  rebootPending = true;                                       // core-1 loop flushes logs, then ESP.restart()
}
// ---- config forms: build an SMS-format command, enqueue for the core-1 handleSms replay -----------
static void portalCfgReply(const String &cmd, const String &okMsg) {
  portalServer.send(200, "text/html", portalAdminHtml(portalCfgEnq(cmd) ? okMsg : "Busy, retry."));
}
static void portalHandleFbase() {                             // POST /fbase -- set Firebase RTDB URL + enable
  if (!portalRequireAdmin()) return;
  String url = portalServer.arg("url"); url.trim();
  bool   en  = (portalServer.arg("en") == "yes");             // unchecked box = arg absent = disabled
  if (url.equalsIgnoreCase("CLEAR")) {
    xSemaphoreTake(netMux, portMAX_DELAY); fbUrl = ""; xSemaphoreGive(netMux);
    fbPersistPending = true;
    portalServer.send(200, "text/html", portalAdminHtml("Firebase URL cleared (feature off)."));
    return;
  }
  if (url.length()) {
    url = supaNormalizeUrl(url);                              // same origin-only reduction as the Supabase form
    if (!url.startsWith("http")) {
      portalServer.send(200, "text/html", portalAdminHtml("URL must start with https://")); return;
    }
  }
  // Set directly under netMux (like the WiFi/TSKEY/SUPA creds); core-1 loop persists to NVS (fbPersistPending).
  xSemaphoreTake(netMux, portMAX_DELAY); fbUrl = url; firebaseEnabled = en; xSemaphoreGive(netMux);
  fbPersistPending = true;
  portalServer.send(200, "text/html", portalAdminHtml(String("Firebase settings saved (") + (en ? "enabled" : "disabled") + ")."));
}
static void portalHandleFbauth() {                            // POST /fbauth -- Firebase device account
  if (!portalRequireAdmin()) return;
  // "Forget password": drop the password but keep the refresh token, so the device stays signed in
  // with no reusable account credential left on it. This is the intended end state after provisioning.
  if (portalServer.arg("forget") == "pass") {
    xSemaphoreTake(netMux, portMAX_DELAY); fbPassword = ""; xSemaphoreGive(netMux);
    fbCredsPersistPending = true;
    portalServer.send(200, "text/html", portalAdminHtml("Password forgotten (refresh token kept)."));
    return;
  }
  String key   = portalServer.arg("key");   key.trim();
  String email = portalServer.arg("email"); email.trim();
  String pass  = portalServer.arg("pass");                    // NOT trimmed: spaces can be legitimate
  bool   signin = (portalServer.arg("signin") == "yes");
  bool   pin    = (portalServer.arg("pin") == "yes");
  // Blank means "keep what's stored" for the two secrets, mirroring the Supabase service-key form.
  xSemaphoreTake(netMux, portMAX_DELAY);
  if (key.length())  fbApiKey = key;
  if (pass.length()) fbPassword = pass;
  if (email != fbEmail) { fbEmail = email; fbRefresh = ""; }  // new identity -> old refresh token is void
  if (pin != fbPinCa) { fbPinCa = pin; fbResetTls(); }        // drop the socket so it re-handshakes
  xSemaphoreGive(netMux);
  fbCredsPersistPending = true;
  if (signin) { fbSignInPending = true; fbAuthFails = 0; fbAuthNextTryMs = 0; }  // clear backoff, retry now
  portalServer.send(200, "text/html", portalAdminHtml(signin ? "Saved. Signing in on the next network tick."
                                                             : "Device account saved."));
}
static void portalHandleSupa() {                              // POST /supa -- set Supabase URL + service key
  if (!portalRequireAdmin()) return;
  String url = portalServer.arg("url"); url.trim();
  String key = portalServer.arg("key"); key.trim();
  if (url.equalsIgnoreCase("CLEAR")) {
    xSemaphoreTake(netMux, portMAX_DELAY); supaUrl = ""; supaKey = ""; xSemaphoreGive(netMux);
    supaPersistPending = true;
    portalServer.send(200, "text/html", portalAdminHtml("Supabase creds cleared."));
    return;
  }
  url = supaNormalizeUrl(url);                                 // collapse any pasted path down to the origin
  if (!url.startsWith("http")) {
    portalServer.send(200, "text/html", portalAdminHtml("URL must start with https://")); return;
  }
  bool keepKey = (key.length() == 0 && supaKey.length() > 0);  // blank key + one already stored -> URL-only edit
  if (!keepKey && key.length() < 20) {                          // a service_role JWT is ~200+ chars
    portalServer.send(200, "text/html", portalAdminHtml("Paste the full service key (or leave blank to keep the current one).")); return;
  }
  // Set directly under netMux (like the WiFi/TSKEY creds); core-1 loop persists to NVS (supaPersistPending).
  // The key is never echoed back to the browser or written to the log.
  xSemaphoreTake(netMux, portMAX_DELAY); supaUrl = url; if (!keepKey) supaKey = key; xSemaphoreGive(netMux);
  supaPersistPending = true;
  portalServer.send(200, "text/html", portalAdminHtml(keepKey ? "Supabase URL saved (key kept)." : "Supabase URL + key saved."));
}
static void portalHandleTskey() {                             // POST /tskey
  if (!portalRequireAdmin()) return;
  int ch = portalServer.arg("ch").toInt();
  String key = portalServer.arg("key"); key.trim();
  bool bad = (key.length() == 0);
  for (unsigned i = 0; i < key.length(); i++) if (!isalnum((unsigned char)key[i])) bad = true;
  if (ch < 1 || ch > 3 || bad) { portalServer.send(200, "text/html", portalAdminHtml("Invalid key (letters/digits).")); return; }
  // tsKey1-3 are netMux-guarded RAM read by tsUpload on core 0 -> set directly here (like WiFi creds),
  // so it applies immediately (label shows "set", uploads use it) and doesn't depend on the config drain.
  xSemaphoreTake(netMux, portMAX_DELAY);
  if (ch == 1) tsKey1 = key; else if (ch == 2) tsKey2 = key; else tsKey3 = key;
  xSemaphoreGive(netMux);
  tsKeyPersistPending = true;                                 // core-1 loop saves it to NVS
  portalServer.send(200, "text/html", portalAdminHtml("ThingSpeak Ch" + String(ch) + " key saved."));
}
static void portalHandleColMode() {                           // POST /colmode
  if (!portalRequireAdmin()) return;
  char cc = portalColArg(); String m = portalServer.arg("mode"); m.toUpperCase();
  if (!cc || (m != "AUTO" && m != "IRRIGATION_ONLY")) { portalServer.send(200, "text/html", portalAdminHtml("Bad mode.")); return; }
  portalCfgReply("MODE,COL_" + String(cc) + "," + m, "Column " + String(cc) + " mode saved (reload to confirm).");
}
static void portalHandleColName() {                           // POST /colname
  if (!portalRequireAdmin()) return;
  char cc = portalColArg(); String nm = portalServer.arg("name"); nm.trim(); nm.replace(",", " ");
  if (!cc || nm.length() == 0) { portalServer.send(200, "text/html", portalAdminHtml("Bad name.")); return; }
  if (nm.length() > 15) nm = nm.substring(0, 15);
  portalCfgReply("NAME,COL_" + String(cc) + "," + nm, "Column " + String(cc) + " name saved (reload to confirm).");
}
static void portalHandleColTargets() {                        // POST /coltargets
  if (!portalRequireAdmin()) return;
  char cc = portalColArg();
  if (!cc) { portalServer.send(200, "text/html", portalAdminHtml("Bad column.")); return; }
  String cmd = "SET,COL_" + String(cc) +
               ",N," + String(portalServer.arg("n").toInt()) +
               ",P," + String(portalServer.arg("p").toInt()) +
               ",K," + String(portalServer.arg("k").toInt()) +
               ",pH," + String(portalServer.arg("ph").toFloat(), 1);
  portalCfgReply(cmd, "Column " + String(cc) + " targets saved (reload to confirm).");
}
static void portalHandleColPreset() {                         // POST /colpreset
  if (!portalRequireAdmin()) return;
  char cc = portalColArg(); String pr = portalServer.arg("preset"); pr.trim();
  if (!cc || pr.length() == 0) { portalServer.send(200, "text/html", portalAdminHtml("Bad preset.")); return; }
  portalCfgReply("SET,COL_" + String(cc) + ",PRESET," + pr, "Column " + String(cc) + " preset saved (reload to confirm).");
}
static void portalHandleThresh() {                            // POST /thresh
  if (!portalRequireAdmin()) return;
  portalCfgReply("THRESH," + String(portalServer.arg("start").toInt()) + "," +
                 String(portalServer.arg("stop").toInt()) + "," +
                 String(portalServer.arg("gap").toInt()), "Thresholds saved (reload to confirm).");
}
static void portalStart() {
  WiFi.mode(WIFI_AP_STA);                                     // AP_STA so scanNetworks() works while AP is up
  WiFi.softAP(AP_SSID, AP_PASS);
  vTaskDelay(pdMS_TO_TICKS(100));
  portalApIpAddr = WiFi.softAPIP();
  portalApIpAddr.toString().toCharArray(portalApIp, sizeof(portalApIp));
  portalDns.start(53, "*", portalApIpAddr);                   // wildcard DNS -> AP IP
  // Register routes ONCE: WebServer::stop() doesn't free handlers, so re-registering each portal
  // session would grow the handler list unbounded (heap leak). begin()/stop() run per session.
  static bool routesRegistered = false;
  if (!routesRegistered) {
    portalServer.on("/", portalHandleRoot);
    portalServer.on("/save", HTTP_POST, portalHandleSave);
    portalServer.on("/rescan", portalHandleRescan);            // GET -> re-scan + redirect
    portalServer.on("/admin", HTTP_POST, portalHandleAdmin);   // PIN unlock
    portalServer.on("/owner", HTTP_POST, portalHandleOwner);
    portalServer.on("/pin", HTTP_POST, portalHandlePin);
    portalServer.on("/view", portalHandleView);                // GET ?f=
    portalServer.on("/download", portalHandleDownload);        // GET ?f=
    portalServer.on("/format", HTTP_POST, portalHandleFormat);
    portalServer.on("/reboot", HTTP_POST, portalHandleReboot); // software-reset ESP1
    portalServer.on("/upload", HTTP_POST, portalHandleUpload); // queue a Supabase CSV push
    portalServer.on("/supa", HTTP_POST, portalHandleSupa);     // set Supabase URL + service key (NVS)
    portalServer.on("/fbase", HTTP_POST, portalHandleFbase);   // set Firebase RTDB URL + enable flag (NVS)
    portalServer.on("/fbauth", HTTP_POST, portalHandleFbauth); // set Firebase device account creds (NVS)
    portalServer.on("/tskey", HTTP_POST, portalHandleTskey);   // config forms (staged -> core-1 handleSms)
    portalServer.on("/colmode", HTTP_POST, portalHandleColMode);
    portalServer.on("/colname", HTTP_POST, portalHandleColName);
    portalServer.on("/coltargets", HTTP_POST, portalHandleColTargets);
    portalServer.on("/colpreset", HTTP_POST, portalHandleColPreset);
    portalServer.on("/thresh", HTTP_POST, portalHandleThresh);
    portalServer.onNotFound(portalHandleNotFound);
    routesRegistered = true;
  }
  portalDoScan();                                             // cache the SSID list once (not per GET /)
  portalServer.begin();
  portalSaved = false; portalStartMs = millis(); portalActive = true;
  Serial.printf("[PORTAL] up: AP=%s IP=%s\n", AP_SSID, portalApIp);
}
static void portalStop() {
  portalServer.stop();
  portalDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalActive = false; portalRequested = false; portalCancel = false;
  portalAdminUnlocked = false;                                // re-lock admin every session
  portalScanOpts = "";                                        // free the cached SSID list
  Serial.println(F("[PORTAL] down"));
}

// Core-0 task: keep the WiFi STA link up and upload on a cadence. NOT on the task WDT
// (it may legitimately block on the network); the core-1 loop is never disturbed.
void netTask(void *pv) {
  (void)pv;
  unsigned long lastUpload = 0;
  for (;;) {
    // WiFi provisioning portal takes over the radio when requested (SoftAP + web form).
    if (portalRequested && !portalActive) portalStart();
    if (portalActive) {
      wifiConnected = false;
      portalDns.processNextRequest();
      portalServer.handleClient();
      if (portalSaved || portalCancel || millis() - portalStartMs > PORTAL_TIMEOUT_MS) {
        bool saved = portalSaved;
        portalStop();
        if (saved) wifiCredsChanged = true;                  // reconnect STA to the freshly-saved creds
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // WiFi radio master switch (Settings > WiFi). Off = drop the radio + skip telemetry.
    if (!wifiEnabled) {
      if (WiFi.getMode() != WIFI_OFF) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
      wifiConnected = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    char ssid[33] = "", pass[65] = "";
    xSemaphoreTake(netMux, portMAX_DELAY);
    strncpy(ssid, wifiSsid.c_str(), sizeof(ssid) - 1);
    strncpy(pass, wifiPass.c_str(), sizeof(pass) - 1);
    xSemaphoreGive(netMux);

    if (ssid[0] == '\0') { wifiConnected = false; vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    if (wifiCredsChanged) { wifiCredsChanged = false; WiFi.disconnect(); vTaskDelay(pdMS_TO_TICKS(200)); }

    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, pass);
      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_MS) vTaskDelay(pdMS_TO_TICKS(250));
      if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_MS)); continue; }
      WiFi.localIP().toString().toCharArray(wifiIpStr, sizeof(wifiIpStr));
    }
    wifiConnected = true;
    wifiRssiVal = WiFi.RSSI();

    unsigned long interval = (sysState == ACTIVE_STATE) ? TS_UPLOAD_ACTIVE_MS : TS_UPLOAD_IDLE_MS;
    if (millis() - lastUpload >= interval) { lastUpload = millis(); tsUpload(); }

    // Firebase live snapshot for the web dashboard. Same core-0 cadence discipline as tsUpload above:
    // it may block on the network, but never touches the core-1 control loop. Additive -- ThingSpeak
    // (graphs) and the Supabase daily CSV (archive) are unaffected and all three serialize here, so
    // only one TLS session is ever open at a time.
    // Sign in / refresh INDEPENDENTLY of whether an RTDB URL is configured. Signing in needs
    // only the API key + creds, so gating it behind the URL (as the upload/poll paths must be)
    // would make provisioning impossible: "Sign in now" would silently do nothing and the portal
    // would sit at "not signed in" forever. fbEnsureToken() self-gates on a valid token, the
    // backoff timer, and a missing API key, so calling it every loop is cheap.
    {
      bool fbOn;
      xSemaphoreTake(netMux, portMAX_DELAY); fbOn = firebaseEnabled; xSemaphoreGive(netMux);
      if (fbOn) fbEnsureToken();
    }

    unsigned long fbInterval = (sysState == ACTIVE_STATE) ? FIREBASE_UPLOAD_ACTIVE_MS : FIREBASE_UPLOAD_IDLE_MS;
    if (millis() - fbLastUploadMs >= fbInterval) firebaseUploadLive();
    // Remote-command transport, both directions. Cheap: they ride the SAME keep-alive TLS
    // socket as the upload above, so a 3 s poll costs one small GET, not a handshake.
    firebaseFlushStatus();                                 // results first: report before fetching more
    firebasePollCommands();

    // Supabase CSV push: core 1 sets uploadReqStamp; do one attempt here and publish the result.
    if (uploadReqStamp != 0 && !uploadBusy) {
      uploadBusy = true;
      long stamp = uploadReqStamp;
      char path[16], obj[13];
      snprintf(path, sizeof(path), "/%08ld.CSV", stamp);
      snprintf(obj,  sizeof(obj),  "%08ld.CSV", stamp);
      bool ok = supaUploadFile(path, obj);
      uploadResStamp = stamp; uploadResult = ok ? 1 : 2;
      uploadReqStamp = 0; uploadBusy = false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// Loose owner check for config commands: compare the last 9 digits of sender vs owner.
static bool senderIsOwner() {
  if (replyTarget.length() == 0) return true;           // internally generated
  String da = "", db = "";
  for (int i = replyTarget.length() - 1; i >= 0 && da.length() < 9; i--) if (isDigit(replyTarget[i])) da = String(replyTarget[i]) + da;
  for (int i = PHONE_NUMBER.length() - 1; i >= 0 && db.length() < 9; i--) if (isDigit(PHONE_NUMBER[i])) db = String(PHONE_NUMBER[i]) + db;
  return da.length() >= 9 && da == db;
}

/* =============================================================================
 *  FAULTS  (3-tier classification + response, spec sec.23)
 * ========================================================================== */
void raiseFault(char tier, const char *code, const char *loc) {
  // Testing is a manual, operator-present, dead-man + hard-cap guarded mode: raise NO automatic fault
  // (no UI-preempt, no SMS, no emergency-stop) -- just record it. The manual MODE+BACK e-stop (which
  // calls enterEmergencyStop directly) still works.
  if (sysState == TEST_MODE) {
    logEvent("ESP1", "FAULT", String("TEST_SUPPRESSED|") + code + "|" + loc);
    return;
  }
  const char *t = (tier == 'C') ? "CRIT" : (tier == 'M') ? "MAJ" : (tier == 'W') ? "WARN" : "MIN";
  if (tier == 'C') faultsToday[0]++; else if (tier == 'M') faultsToday[1]++; else if (tier == 'm') faultsToday[2]++;

  String detail = String(t) + "|" + code + "|" + loc;
  logEvent("ESP1", "FAULT", detail);
  // Capture problems seen during a service run so the post-run error table can list them.
  if (runPhase == RUN_PRE || runPhase == RUN_LIVE)
    runNoteErr(String(code) + " " + loc + " @" + run.stage);
  lastFaultMsg = String(code) + " " + loc;
  lastFaultTime = tsString();
  // Faults preempt the Settings/Testing UI; never leave a manually-held relay ON. Also drop
  // ESP2 power (OFF-during-idle) so the executor doesn't sit powered after a Testing abort.
  if (uiMode == UI_TEST) {
    sendEsp2("TEST,EXIT"); testArmPending = false; esp2SetPower(false);
    if (sysState == TEST_MODE) setState(IDLE_STATE);
  }
  if (uiMode == UI_DIAG) {                          // fault while in Sensor Diag: stop streams + power ESP2 off
    diagStopStream(); esp2SetPower(false);
  }
  // Non-critical faults wait BEHIND an open confirm dialog (companion spec §B.3.1); Critical
  // force-dismisses (handled in enterEmergencyStop below).
  if (tier == 'C' || !(editConfirm || restoreConfirm)) { uiMode = UI_DATA; lcdPage = PAGE_FAULT; }
  wakeBacklight();

  // GSM alert for all tiers (sec.12.1.1)
  sendSMS(String("ALERT,") + t + "," + code + "," + loc);

  if (tier == 'C') enterEmergencyStop(true);     // critical -> shutdown (sec.14.7.1)
}

void enterEmergencyStop(bool cutPower) {
  // stop all actuators (sec.22.3) then optionally cut ESP2 power (sec.23.1.1)
  esp2Serial.print(FRAME_START); esp2Serial.print(",STOP_ALL,"); esp2Serial.println(FRAME_END);
  logEvent("ESP1", "CMD", "ESP2|STOP_ALL");
  esp2Held = false;                                // a hard E-stop supersedes any held-fault recovery
  wo.active = false; wo.stage = WO_IDLE;
  pendingRun.active = false; pendingRun.colIdx = -1;
  pendingExercise.active = false; pendingExercise.idx = -1; pendingExercise.sent = false;
  firebaseRemoteExerciseDone("failed", "aborted by emergency stop");   // release the remote slot
  if (cutPower) esp2SetPower(false, true);        // (sec.23.1.1) -- FORCE immediate cut (bypass min-on hold)
  runUiAbort("EMERGENCY_STOP");                   // close the run UI so the E-stop prompt owns the screen
  editConfirm = false; restoreConfirm = false; resetConfirm = false; editDirty = false;   // force-dismiss + auto-discard (§B.3.1)
  // Take over the screen with the recovery prompt (Part B): leave any Settings/Testing UI,
  // default the cursor to "Return to normal".
  uiMode = UI_DATA; lcdPage = PAGE_FAULT; estopSel = 0; wakeBacklight();
  setState(EMERGENCY_STOP);
}

/* =============================================================================
 *  ESP2 FAULT HOLD  --  hard ESP2 fault: ESP2 is holding (P17 dropped) WITH the mix-tank
 *  volume retained, so ESP1 keeps it POWERED (no GPIO4 cut) and waits for the user's
 *  GSM/LCD recovery choice. Alerts carry the operation + column (sec.19.4.8 / user req).
 * ========================================================================== */
void enterFaultHold(const char *code, const char *loc) {
  faultsToday[0]++;                                    // critical-tier count
  String op   = wo.fertigate ? "FERTIGATION" : "IRRIGATION";
  String colS = (wo.colIdx >= 0 && wo.colIdx < NUM_COLUMNS) ? String("COL_") + COL_TAG[wo.colIdx] : "COL_?";
  String detail = String("CRIT|") + code + "|" + loc + "|" + op + "|" + colS + "|HELD";
  logEvent("ESP1", "FAULT", detail);
  lastFaultMsg = String(code) + " " + loc + " " + colS;
  lastFaultTime = tsString();
  // GSM alert WITH operation + column, and the reply menu (sec.12.1.1 + user req).
  sendSMS(String("ALERT,CRIT,") + code + "," + loc + "," + op + "," + colS +
          ",reply STOP/RELEASE/IRRIGATE/NORMAL");
  // Count consecutive identical holds; after 3 FLOW_FAILs steer to Release (flow-independent on ESP2).
  if (lastHoldCode == code) sameHoldN++; else { lastHoldCode = code; sameHoldN = 1; }
  // Circuit breaker. Steering only moves the cursor -- an operator who keeps picking "Resume normal"
  // against dead hardware re-holds forever, which is exactly the state that forced a physical reset
  // with the pumps still live. Give up on our own and snooze the column instead.
  if (sameHoldN >= HOLD_AUTOCANCEL_N) {
    logEvent("ESP1", "FAULT", String("CRIT|") + code + "|" + loc + "|AUTOCANCEL|after=" + String(sameHoldN));
    sendSMS(String("ALERT,CANCEL,") + code + "," + colS + ",repeated holds - run cancelled, column snoozed 30m");
    cancelRun(1, code);                                // 30-min snooze; clears the hold + stops ESP2
    return;
  }
  steerRelease = (sameHoldN >= 3 && String(code) == "FLOW_FAIL");
  esp2Held = true; faultRecovSel = steerRelease ? 1 : 0;   // default cursor: Release when steering, else Hold
  runUiAbort(code);                                    // close the run UI so the recovery menu is reachable
  editConfirm = false; restoreConfirm = false; resetConfirm = false; editDirty = false;   // force-dismiss any open dialog (§B.3.1)
  // ESP2 STAYS POWERED (it holds the tank) -- do NOT esp2SetPower(false) here.
  uiMode = UI_DATA; lcdPage = PAGE_FAULT; wakeBacklight();
  setState(EMERGENCY_STOP);                            // halted; the held UI/buttons branch on esp2Held
}

/* Operator abort. This is the exit the recovery menu never had: Hold/Release/Irrigate/Normal all
 * try to CONTINUE the run, so an unrecoverable fault (dead flow sensor) left a physical reset as
 * the only way to stop the pumps.
 *   mode 0 = turn the column OFF (persistent)   1 = snooze 30 min   2 = snooze 10 min
 * Safe to call whether or not ESP2 is currently held. */
void cancelRun(int mode, const char *why) {
  int c = (wo.colIdx >= 0) ? wo.colIdx : pendingRun.colIdx;      // whichever run we are aborting

  // STOP_ALL is what actually kills the pumps: on ESP2 it clears faultHeld, the work order, the
  // step and the timed-recovery latch, so nothing can resume behind our back.
  sendEsp2("STOP_ALL");

  wo.active = false; wo.stage = WO_IDLE; wo.colIdx = -1;
  pendingRun.active = false; pendingRun.colIdx = -1;
  esp2Held = false;
  // A cancelled FORCE run must not leave the dashboard sitting on "accepted", nor let a later
  // scheduled run inherit this one's column mask.
  if (forceCmdId[0]) { firebaseQueueStatus(forceCmdId, "failed", why); forceCmdId[0] = 0; }
  forceMask = 0;
  lastHoldCode = ""; sameHoldN = 0; steerRelease = false;        // a cancel resolves the re-hold loop
  cancelPrompt = false; cancelSel = 0;

  String scope;
  if (c >= 0 && c < NUM_COLUMNS) {
    if (mode == 0)      { COLUMN_ENABLED[c] = false; saveColEnable(c); scope = "OFF"; }
    else if (mode == 1) { colSnoozeUntil[c] = millis() + SNOOZE_LONG_MS;  scope = "SNOOZE30"; }
    else                { colSnoozeUntil[c] = millis() + SNOOZE_SHORT_MS; scope = "SNOOZE10"; }
  } else scope = "NOCOL";                                        // nothing was running -- still a clean stop

  String colS = (c >= 0 && c < NUM_COLUMNS) ? String("COL_") + COL_TAG[c] : String("COL_?");
  logEvent("ESP1", "ACT", String("CANCEL|") + scope + "|" + colS + "|" + why);
  sendSMS(String("ACK,CANCEL,") + scope + "," + colS);

  runUiAbort("CANCELLED");                                       // release the run screen
  esp2SetPower(false);                                           // OFF-during-idle
  setState(IDLE_STATE);
  uiMode = UI_DATA; lcdPage = PAGE_HOME; wakeBacklight();
}

// Apply a recovery choice (from LCD ENTER or an SMS reply): 0 Hold, 1 Release, 2 OnlyIrr, 3 Normal.
void issueRecovery(int sel) {
  if (!esp2Held) return;
  if (sel == 0) {                                      // Hold / acknowledge: stay held, push nothing
    sendSMS("ACK,STOP,HELD");
    // Visible acknowledgement. Staying held is correct behaviour, but with no feedback at all this
    // button was indistinguishable from a dead one.
    lastFaultMsg = String("HELD - waiting");
    wakeBacklight();
    return;
  }
  const char *mode = (sel == 1) ? "RELEASE" : (sel == 2) ? "IRRIGATE" : "NORMAL";
  sendEsp2(String("RESUME,") + mode);                  // ESP2 re-energizes P17 and resumes/rewrites the run
  logEvent("ESP1", "ACT", String("RESUME|") + mode + "|COL_" +
           (wo.colIdx >= 0 ? String(COL_TAG[wo.colIdx]) : String("?")));
  sendSMS(String("ACK,RESUME,") + mode);
  esp2Held = false;
  if (wo.active) { wo.stage = WO_ACKED; wo.sentMs = millis(); }   // restart the DONE-timeout supervision
  setState(ACTIVE_STATE);                              // supervising the resumed run again
}

/* =============================================================================
 *  CALIBRATION MODE UI  (companion spec §A)  -- IDLE-only, automation suspended
 * ========================================================================== */
static bool calTgtVisible(int idx) {
  const CalTgt &t = CAL_TGTS[idx];
  if (t.kind == CK_SOIL || t.kind == CK_NPK) return COLUMN_ENABLED[t.col];  // hide disabled columns (§14.1.4.1)
  return true;
}
static bool calIsNano(uint8_t kind) {                    // Nano-owned sensors (ESP1 applies their cal)
  return kind == CK_SOIL || kind == CK_LEVEL_RES || kind == CK_LEVEL_MIX || kind == CK_OFFSET ||
         kind == CK_NPK || kind == CK_FLOW_RES;
}
static const char *calPrimeLine(const char *id, int *col) {
  *col = -1;
  if (!strcmp(id, "FLOW_MIXIRR")) { *col = 0; return "MIXIRR"; }   // prime via column A
  if (!strcmp(id, "FLOW_RESMIX")) return "RESMIX";
  if (!strcmp(id, "FLOW_NUTA"))   return "NUTA";
  if (!strcmp(id, "FLOW_NUTB"))   return "NUTB";
  if (!strcmp(id, "FLOW_NUTC"))   return "NUTC";
  return "";
}

void enterCal() {
  if (sysState != IDLE_STATE) return;            // idle-only lockout (§A.6)
  uiMode = UI_CAL; calSel = 0; calOpen = false; calStep = 0;
  esp2SetPower(true);                            // power ESP2 so its sensors/pumps are reachable
  logEvent("ESP1", "CAL", "ENTER");
}

static void calOpenTarget() {
  const CalTgt &t = CAL_TGTS[calSel];
  calOpen = true; calStep = 0; calCap[0] = calCap[1] = 0; calFlowPulses = 0; calVolL = 0.50f;
  calRunIdx = 0; calKRuns[0] = calKRuns[1] = calKRuns[2] = 0; calMsg = "";
  calRxId = ""; calRxValid = false;
  if (t.kind == CK_NPK) { for (int j = 0; j < 3; j++) calNpkEdit[j] = calNpkOff[t.col][j]; }  // seed current
  String cs = String("CAL_START,") + t.id;
  if (calIsNano(t.kind)) sendNanoCommand(cs.c_str()); else sendEsp2(cs);
}

static void calCloseTarget() {
  const CalTgt &t = CAL_TGTS[calSel];
  String cs = String("CAL_STOP,") + t.id;
  if (calIsNano(t.kind)) sendNanoCommand(cs.c_str());
  else {
    sendEsp2(cs);
    int col; const char *ln = calPrimeLine(t.id, &col);
    if (*ln) sendEsp2(String("PRIME_STOP,") + ln);   // make sure any prime is stopped
  }
  calOpen = false;
}

// Compute the captured points for the open target, save to NVS, push ESP2-owned to ESP2 (§A.5.1).
// Returns false if the capture was rejected (e.g. degenerate pH/EC points) -- nothing is saved.
static bool calCommit() {
  const CalTgt &t = CAL_TGTS[calSel];
  char buf[40];
  switch (t.kind) {
    case CK_SOIL: {
      int air = (int)calCap[0], wat = (int)calCap[1];
      String k = "s" + String(t.col);
      prefsCal.putInt((k + "a2").c_str(), air); prefsCal.putInt((k + "w2").c_str(), wat);
      snprintf(buf, sizeof(buf), "SAVE|SOIL_%c|%d/%d", COL_TAG[t.col], air, wat);
      calSoilAir[t.col] = air; calSoilWater[t.col] = wat;
      logEvent("ESP1", "CAL", buf);
      break;
    }
    case CK_PH: {
      if (fabs(calCap[0] - calCap[1]) < CAL_MIN_SPAN_ADC) return false;   // degenerate -> reject (no div-by-0)
      float m = (7.0f - 4.0f) / (calCap[0] - calCap[1]);   // (raw7,7.0),(raw4,4.0)
      float b = 7.0f - m * calCap[0];
      calPhM = m; calPhB = b;
      prefsCal.putFloat("phM", m); prefsCal.putFloat("phB", b);
      setCalPushBlocking("PH", String(m, 6) + "," + String(b, 4));
      logEvent("ESP1", "CAL", "SAVE|PH");
      break;
    }
    case CK_EC: {
      if (fabs(calCap[1] - calCap[0]) < CAL_MIN_SPAN_ADC) return false;   // degenerate -> reject (no div-by-0)
      float m = EC_STD_MSCM / (calCap[1] - calCap[0]);     // (raw0,0),(rawstd,EC_STD_MSCM)
      float b = 0.0f - m * calCap[0];
      calEcM = m; calEcB = b;
      prefsCal.putFloat("ecM", m); prefsCal.putFloat("ecB", b);
      setCalPushBlocking("EC", String(m, 6) + "," + String(b, 4));
      logEvent("ESP1", "CAL", "SAVE|EC");
      break;
    }
    case CK_ACS: {
      calAcs712Zero = calCap[0] / 4095.0f * 3.3f;
      prefsCal.putFloat("acs", calAcs712Zero);
      setCalPushBlocking("ACS712", String(calAcs712Zero, 4));
      logEvent("ESP1", "CAL", "SAVE|ACS712");
      break;
    }
    case CK_LEVEL_RES: calResEmptyCm = calCap[0]; prefsCal.putFloat("reCm", calResEmptyCm); logEvent("ESP1","CAL","SAVE|ULTRA_RES"); break;
    case CK_LEVEL_MIX: calMixEmptyCm = calCap[0]; prefsCal.putFloat("meCm", calMixEmptyCm); logEvent("ESP1","CAL","SAVE|ULTRA_MIX"); break;
    case CK_FLOW: {
      // 3-run result (§A.4.1.7): average if the runs agree, median if an outlier remains.
      float a = calKRuns[0], b = calKRuns[1], c = calKRuns[2];
      float med = a + b + c - max(a, max(b, c)) - min(a, min(b, c));   // middle value
      bool outlier = (fabs(a - med) > CAL_OUTLIER_PCT / 100.0f * med) ||
                     (fabs(b - med) > CAL_OUTLIER_PCT / 100.0f * med) ||
                     (fabs(c - med) > CAL_OUTLIER_PCT / 100.0f * med);
      float kf = outlier ? med : (a + b + c) / 3.0f;
      if (kf <= 0) break;
      const char *id = t.id;
      if      (!strcmp(id, "FLOW_RESMIX")) { calKResMix = kf; prefsCal.putFloat("kRM", kf); }
      else if (!strcmp(id, "FLOW_MIXIRR")) { calKMixIrr = kf; prefsCal.putFloat("kMI", kf); }
      else if (!strcmp(id, "FLOW_NUTA"))   { calKNut[0] = kf; prefsCal.putFloat("kNA", kf); }
      else if (!strcmp(id, "FLOW_NUTB"))   { calKNut[1] = kf; prefsCal.putFloat("kNB", kf); }
      else if (!strcmp(id, "FLOW_NUTC"))   { calKNut[2] = kf; prefsCal.putFloat("kNC", kf); }
      setCalPushBlocking(id, String(kf, 1));
      snprintf(buf, sizeof(buf), "SAVE|%s|K=%d|%s", id, (int)kf, outlier ? "MED" : "AVG3");
      logEvent("ESP1", "CAL", buf);
      break;
    }
    case CK_OFFSET: {     // offset = reference reading (calVolL) - captured raw (calCap[0]); Nano-applied
      float off = calVolL - calCap[0];
      if (t.col == 0)      { calTempOff = off; prefsCal.putFloat("tOff", off); }
      else if (t.col == 1) { calHumOff  = off; prefsCal.putFloat("hOff", off); }
      else                 { calLuxOff  = off; prefsCal.putFloat("lOff", off); }
      snprintf(buf, sizeof(buf), "SAVE|%s|off=%s", t.id, String(off, 2).c_str());
      logEvent("ESP1", "CAL", buf);
      break;
    }
    case CK_NPK: {        // per-column N/P/K offset trim (guarded, §A.4.3); Nano stays raw
      String k = "s" + String(t.col);
      prefsCal.putFloat((k + "N").c_str(), calNpkEdit[0]);
      prefsCal.putFloat((k + "P").c_str(), calNpkEdit[1]);
      prefsCal.putFloat((k + "K").c_str(), calNpkEdit[2]);
      for (int j = 0; j < 3; j++) calNpkOff[t.col][j] = calNpkEdit[j];
      snprintf(buf, sizeof(buf), "SAVE|NPK_%c|%d/%d/%d", COL_TAG[t.col],
               (int)calNpkEdit[0], (int)calNpkEdit[1], (int)calNpkEdit[2]);
      logEvent("ESP1", "CAL", buf);
      break;
    }
    case CK_FLOW_RES: {   // Nano reservoir fill-flow: single run -> correction scale on the Nano's L/min
      float kMeas = (calVolL > 0.01f) ? (float)calFlowPulses / calVolL : 0;
      if (kMeas <= 0) return false;
      calFlowResScale = NANO_FLOW_K / kMeas;          // Nano reports L/min with NANO_FLOW_K; this rescales it
      prefsCal.putFloat("frS", calFlowResScale);
      snprintf(buf, sizeof(buf), "SAVE|FLOW_RES|x%s", String(calFlowResScale, 3).c_str());
      logEvent("ESP1", "CAL", buf);
      break;
    }
  }
  return true;
}

void calButton(int i) {
  // i: 0=UP 1=DOWN 2=ENTER 3=BACK  (MODE handled in handleButtons)
  if (!calOpen) {                                  // browsing the target list
    if (i == 0)      { do { calSel = (calSel + CAL_TGT_COUNT - 1) % CAL_TGT_COUNT; } while (!calTgtVisible(calSel)); }
    else if (i == 1) { do { calSel = (calSel + 1) % CAL_TGT_COUNT; } while (!calTgtVisible(calSel)); }
    else if (i == 3) { esp2SetPower(false); uiMode = UI_MENU; logEvent("ESP1", "CAL", "EXIT"); }
    else if (i == 2) calOpenTarget();
    return;
  }
  const CalTgt &t = CAL_TGTS[calSel];
  if (i == 3) { calCloseTarget(); return; }        // BACK = abandon this target (no save)

  if (t.kind == CK_FLOW) {
    if (calStep == 0) {                            // run step: ENTER held = pump (calHoldTick); UP = proceed
      if (i == 0) { calFlowPulses = (calRxValid ? (unsigned long)calRxRaw : 0); calStep = 1; }
    } else if (calStep == 1) {                      // volume entry for this run
      if (i == 0) calVolL += 0.01f;
      else if (i == 1) { calVolL -= 0.01f; if (calVolL < 0.0f) calVolL = 0.0f; }
      else if (i == 2) {                            // store this run's K, then next run or review
        calKRuns[calRunIdx] = (calVolL > 0.01f) ? (float)calFlowPulses / calVolL : 0;
        if (++calRunIdx < 3) { calStep = 0; calVolL = 0.50f; calFlowPulses = 0;
                               sendEsp2(String("CAL_START,") + t.id); }   // re-zero ISR for the next run
        else calStep = 2;                           // review the 3 runs
      }
    } else {                                        // review step: UP = redo worst run, ENTER = save
      if (i == 2) { calCommit(); calCloseTarget(); }
      else if (i == 0) {                            // jump back to the run furthest from the median
        float a = calKRuns[0], b = calKRuns[1], c = calKRuns[2];
        float med = a + b + c - max(a, max(b, c)) - min(a, min(b, c));
        int worst = 0; float wd = fabs(a - med);
        if (fabs(b - med) > wd) { worst = 1; wd = fabs(b - med); }
        if (fabs(c - med) > wd) { worst = 2; }
        calRunIdx = worst; calStep = 0; calVolL = 0.50f; calFlowPulses = 0;
        sendEsp2(String("CAL_START,") + t.id);
      }
    }
    return;
  }

  if (t.kind == CK_OFFSET) {
    if (calStep == 0) {                            // capture the raw sensor reading
      if (i == 2) { if (calRxValid) calCap[0] = calRxRaw; calVolL = calCap[0]; calStep = 1; }
    } else {                                        // enter the true reference reading -> offset
      if (i == 0) calVolL += 0.1f;
      else if (i == 1) calVolL -= 0.1f;
      else if (i == 2) { calCommit(); calCloseTarget(); }
    }
    return;
  }

  if (t.kind == CK_NPK) {                          // guarded N/P/K offset editor (§A.4.3)
    if (calStep == 0) {                            // warning gate
      if (i == 2) calStep = 1;                      // ENTER = proceed to edit
      else if (i == 1) { calNpkEdit[0] = calNpkEdit[1] = calNpkEdit[2] = 0; calCommit(); calCloseTarget(); }  // DOWN = reset to 0
    } else {                                        // edit N (1), P (2), K (3)
      int idx = calStep - 1;
      if (i == 0) calNpkEdit[idx] += 1.0f;
      else if (i == 1) calNpkEdit[idx] -= 1.0f;
      else if (i == 2) { if (++calStep > 3) { calCommit(); calCloseTarget(); } }
    }
    return;
  }

  if (t.kind == CK_FLOW_RES) {                    // Nano reservoir flow: manual external fill, no pump
    if (calStep == 0) {                            // watch pulses climb during the fill; UP = done
      if (i == 0) { calFlowPulses = (calRxValid ? (unsigned long)calRxRaw : 0); calStep = 1; }
    } else {                                        // volume entry
      if (i == 0) calVolL += 0.01f;
      else if (i == 1) { calVolL -= 0.01f; if (calVolL < 0.0f) calVolL = 0.0f; }
      else if (i == 2) { calCommit(); calCloseTarget(); }
    }
    return;
  }

  // capture kinds: ENTER captures current raw; 2-pt (soil/pH/EC) needs 2, others 1
  int needed = (t.kind == CK_PH || t.kind == CK_EC || t.kind == CK_SOIL) ? 2 : 1;
  if (i == 2) {
    calMsg = "";
    if (calRxValid) calCap[calStep] = calRxRaw;
    if (++calStep >= needed) {
      if (calCommit()) calCloseTarget();
      else { calStep = 0; calMsg = "pts too close"; }   // degenerate pH/EC -> recapture (§B audit fix)
    }
  }
}

// Flow dead-man: while ENTER is physically held in the flow run step, stream PRIME_START as the
// keep-alive (ESP2 caps locally); release -> PRIME_STOP. Mirrors testHoldTick (sec.18.10.8).
void calHoldTick() {
  if (uiMode != UI_CAL || !calOpen) { calEnterWasDown = false; return; }
  const CalTgt &t = CAL_TGTS[calSel];
  if (t.kind != CK_FLOW || calStep != 0) { calEnterWasDown = false; return; }
  static unsigned long lastHold = 0;
  int col; const char *ln = calPrimeLine(t.id, &col);
  bool down = (digitalRead(BTN_ENTER) == LOW);
  if (down) {
    if (millis() - lastHold >= 150) {
      lastHold = millis();
      String c = String("PRIME_START,") + ln;
      if (col >= 0) c += String(",") + COL_TAG[col];
      sendEsp2(c);
    }
    calEnterWasDown = true;
  } else if (calEnterWasDown) {
    sendEsp2(String("PRIME_STOP,") + ln);
    calEnterWasDown = false;
  }
}

void lcdRenderCal() {
  char l[21];
  if (!calOpen) {
    lcd.setCursor(0, 0); lcd.print("CALIBRATION  IDLE   ");
    int top = calSel - 1; if (top < 0) top = 0; if (top > CAL_TGT_COUNT - 3) top = CAL_TGT_COUNT - 3; if (top < 0) top = 0;
    for (int r = 0; r < 3; r++) {
      int idx = top + r;
      lcd.setCursor(0, r + 1);
      if (idx < CAL_TGT_COUNT) { snprintf(l, 21, "%c%-19s", idx == calSel ? '>' : ' ', CAL_TGTS[idx].name); lcd.print(l); }
      else lcd.print("                    ");
    }
    return;
  }
  const CalTgt &t = CAL_TGTS[calSel];
  bool fresh = calRxValid && (millis() - calRxMs < CAL_STALE_MS);
  lcd.setCursor(0, 0); snprintf(l, 21, "CAL %-16s", t.name); lcd.print(l);
  if (t.kind == CK_FLOW) {
    if (calStep == 0) {
      lcd.setCursor(0, 1); snprintf(l, 21, "Run %d/3 hold ENTER  ", calRunIdx + 1); lcd.print(l);
      lcd.setCursor(0, 2); snprintf(l, 21, "pulses:%-13ld", fresh ? (long)calRxRaw : 0L); lcd.print(l);
      lcd.setCursor(0, 3); lcd.print("UP=done  BACK=cancel");
    } else if (calStep == 1) {
      lcd.setCursor(0, 1); snprintf(l, 21, "Run %d pulses=%-7ld", calRunIdx + 1, (long)calFlowPulses); lcd.print(l);
      lcd.setCursor(0, 2); snprintf(l, 21, "vol: %s L         ", String(calVolL, 2).c_str()); lcd.print(l);
      lcd.setCursor(0, 3); lcd.print("UP/DN vol  ENT=next ");
    } else {                                          // review the 3 runs
      float a = calKRuns[0], b = calKRuns[1], c = calKRuns[2];
      float med = a + b + c - max(a, max(b, c)) - min(a, min(b, c));
      bool out = (fabs(a - med) > CAL_OUTLIER_PCT / 100.0f * med) || (fabs(b - med) > CAL_OUTLIER_PCT / 100.0f * med) ||
                 (fabs(c - med) > CAL_OUTLIER_PCT / 100.0f * med);
      lcd.setCursor(0, 1); snprintf(l, 21, "K %d %d %d   ", (int)a, (int)b, (int)c); lcd.print(l);
      lcd.setCursor(0, 2); snprintf(l, 21, "%s K=%d        ", out ? "OUTLIER med" : "avg", (int)(out ? med : (a + b + c) / 3.0f)); lcd.print(l);
      lcd.setCursor(0, 3); lcd.print(out ? "UP=redo  ENT=save  " : "ENT=save  BACK=esc ");
    }
    return;
  }
  if (t.kind == CK_OFFSET) {
    if (calStep == 0) {
      lcd.setCursor(0, 1); lcd.print("Read reference, then");
      lcd.setCursor(0, 2); snprintf(l, 21, "raw:%-9s ENTER ", fresh ? String(calRxRaw, 1).c_str() : "--"); lcd.print(l);
      lcd.setCursor(0, 3); lcd.print("ENT=capture BACK=esc");
    } else {
      lcd.setCursor(0, 1); snprintf(l, 21, "raw was %s     ", String(calCap[0], 1).c_str()); lcd.print(l);
      lcd.setCursor(0, 2); snprintf(l, 21, "ref: %s   off %s", String(calVolL, 1).c_str(), String(calVolL - calCap[0], 1).c_str()); lcd.print(l);
      lcd.setCursor(0, 3); lcd.print("UP/DN ref  ENT=save ");
    }
    return;
  }
  if (t.kind == CK_NPK) {
    if (calStep == 0) {
      lcd.setCursor(0, 1); lcd.print("WARN: biases data!  ");
      lcd.setCursor(0, 2); lcd.print("Not needed normally ");
      lcd.setCursor(0, 3); lcd.print("ENT=edit DN=zero/sav");
    } else {
      const char *el = (calStep == 1) ? "N" : (calStep == 2) ? "P" : "K";
      lcd.setCursor(0, 1); snprintf(l, 21, "NPK trim %c  edit %s ", COL_TAG[t.col], el); lcd.print(l);
      lcd.setCursor(0, 2); snprintf(l, 21, "N%d P%d K%d        ", (int)calNpkEdit[0], (int)calNpkEdit[1], (int)calNpkEdit[2]); lcd.print(l);
      lcd.setCursor(0, 3); lcd.print("UP/DN  ENT=next/save");
    }
    return;
  }
  if (t.kind == CK_FLOW_RES) {                      // Nano reservoir flow: manual fill (no pump)
    if (calStep == 0) {
      lcd.setCursor(0, 1); lcd.print("Run fill; watch pul ");
      lcd.setCursor(0, 2); snprintf(l, 21, "pulses:%-13ld", fresh ? (long)calRxRaw : 0L); lcd.print(l);
      lcd.setCursor(0, 3); lcd.print("UP=done  BACK=cancel");
    } else {
      lcd.setCursor(0, 1); snprintf(l, 21, "pulses=%-13ld", (long)calFlowPulses); lcd.print(l);
      lcd.setCursor(0, 2); snprintf(l, 21, "vol: %s L         ", String(calVolL, 2).c_str()); lcd.print(l);
      lcd.setCursor(0, 3); lcd.print("UP/DN vol  ENT=save ");
    }
    return;
  }
  int needed = (t.kind == CK_PH || t.kind == CK_EC || t.kind == CK_SOIL) ? 2 : 1;
  const char *prompt = "Steady, then ENTER";
  if      (t.kind == CK_SOIL) prompt = (calStep == 0) ? "Probe in AIR, ENTER" : "Probe in WATER, ENT";
  else if (t.kind == CK_PH)   prompt = (calStep == 0) ? "In pH7 buf, ENTER"   : "In pH4 buf, ENTER";
  else if (t.kind == CK_EC)   prompt = (calStep == 0) ? "Dry/air, ENTER"      : "In std soln, ENTER";
  else if (t.kind == CK_ACS)  prompt = "Motor OFF, ENTER";
  else                        prompt = "Tank EMPTY, ENTER";
  lcd.setCursor(0, 1); snprintf(l, 21, "%-20s", prompt); lcd.print(l);
  lcd.setCursor(0, 2); snprintf(l, 21, "raw:%-8s pt%d/%d ", fresh ? String(calRxRaw, 1).c_str() : "--", calStep + 1, needed); lcd.print(l);
  lcd.setCursor(0, 3);
  if (calMsg.length()) { snprintf(l, 21, "%-20s", calMsg.c_str()); lcd.print(l); }   // e.g. "pts too close"
  else lcd.print("ENT=capture BACK=esc");
}

/* =============================================================================
 *  SENSOR DIAG  (Settings > Sensor Diag)  --  read-only RAW viewer, all 3 controllers.
 *  Nano + ESP1 raw are already on hand (SensorData raw fields incl. per-probe soil, battery ADC).
 *  ESP2's sensors are NOT streamed in normal operation, so the ESP2 pages power ESP2 up and
 *  CAL-stream each sensor's raw value one-at-a-time (pH/EC/ACS/PZEM/flow), reusing the calibration
 *  transport. Purely diagnostic: no actuators driven, ESP2 powered only while the screen is open.
 * ========================================================================== */
// Stop the ESP2 CAL sweep (close the open id) without touching power / uiMode.
void diagStopStream() {
  if (diagEsp2Streaming && diagEsp2Idx >= 0 && diagEsp2DwellMs != 0)
    sendEsp2(String("CAL_STOP,") + DIAG_ESP2_ID[diagEsp2Idx]);
  diagEsp2Streaming = false; diagEsp2Idx = -1; diagEsp2DwellMs = 0;
}

void enterDiag() {
  uiMode = UI_DIAG; diagPage = 0;
  for (int k = 0; k < DIAG_ESP2_N; k++) { diagEsp2Raw[k] = 0; diagEsp2Valid[k] = false; diagEsp2Ms[k] = 0; }
  diagEsp2Idx = -1; diagEsp2DwellMs = 0; diagEsp2Streaming = false;
  // ESP2 sensors aren't sent normally -> power ESP2 so its pages can CAL-stream (idle-only; a run /
  // held fault makes it reply BUSY). Automation is already paused in any Settings mode. The sweep
  // self-starts in diagTick. (Soil is per-probe in the normal packet -- no sweep needed.)
  if (sysState == IDLE_STATE && !wo.active && !esp2Held) esp2SetPower(true);
  logEvent("ESP1", "DIAG", "ENTER");
}

void exitDiag() {
  diagStopStream();
  esp2SetPower(false);                        // respects the min-on / deferred-off logic
  uiMode = UI_MENU;
  logEvent("ESP1", "DIAG", "EXIT");
}

// Round-robin the ESP2 CAL stream (self-starting): arm one id, dwell, record raw, stop, advance.
void diagTick() {
  if (uiMode != UI_DIAG) return;
  if (sysState != IDLE_STATE || wo.active || esp2Held) { diagStopStream(); return; }   // ESP2 busy
  if (!diagEsp2Streaming) { diagEsp2Streaming = true; diagEsp2Idx = 0; diagEsp2DwellMs = 0; }

  if (diagEsp2DwellMs == 0) {                                   // arm the current id
    calRxId = "";                                              // discard any stale sample from the prior id
    sendEsp2(String("CAL_START,") + DIAG_ESP2_ID[diagEsp2Idx]);
    diagEsp2DwellMs = millis();
    return;
  }
  if (millis() - diagEsp2DwellMs < DIAG_ESP2_DWELL_MS) return;

  int k = diagEsp2Idx;                                          // dwell elapsed: record what arrived
  if (calRxId == DIAG_ESP2_ID[k] && millis() - calRxMs < DIAG_ESP2_DWELL_MS + 500) {
    diagEsp2Raw[k] = calRxRaw; diagEsp2Valid[k] = calRxValid; diagEsp2Ms[k] = millis();
  }
  sendEsp2(String("CAL_STOP,") + DIAG_ESP2_ID[k]);
  diagEsp2Idx = (diagEsp2Idx + 1) % DIAG_ESP2_N;
  diagEsp2DwellMs = 0;                                          // next loop arms the new id
}

void diagButton(int i) {
  // i: 0=UP 1=DOWN 2=ENTER 3=BACK  (read-only; ENTER is a no-op)
  if (i == 0)      diagPage = (diagPage + DIAG_PAGES - 1) % DIAG_PAGES;
  else if (i == 1) diagPage = (diagPage + 1) % DIAG_PAGES;
  else if (i == 3) exitDiag();
}

// Liveness flag from freshness + validity. Short (<=4 chars) so it right-fits a 20-col line.
static const char *diagNanoFlag(unsigned long ms, bool valid) {
  if (ms == 0) return "n/a";                        // never received this sensor
  if (millis() - ms > NANO_STALE_MS) return "OLD";  // link/sensor stopped updating
  return valid ? "ok" : "BAD";                      // BAD = a -1 sentinel (sensor faulted)
}
static const char *diagEsp2Flag(int k) {
  if (!diagEsp2Streaming) return (sysState == IDLE_STATE ? "..." : "BUSY");
  if (diagEsp2Ms[k] == 0) return "...";             // not yet answered this sweep
  if (millis() - diagEsp2Ms[k] > DIAG_ESP2_STALE_MS) return "OLD";
  return diagEsp2Valid[k] ? "ok" : "BAD";
}

void lcdRenderDiag() {
  char l[21];
  char hdr[21];
  snprintf(hdr, 21, "DIAG%d/%d", diagPage + 1, DIAG_PAGES);

  switch (diagPage) {
    case 0:   // Nano: DHT22 + BH1750
      snprintf(l, 21, "%-8s Nano Env", hdr);                              lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "%-6s%-9s%5s", "Temp", String(sensor.rawTemp, 1).c_str(), diagNanoFlag(sensor.msEnv, sensor.envValid));   lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "%-6s%-9s%5s", "Humid", String(sensor.rawHum, 1).c_str(), diagNanoFlag(sensor.msEnv, sensor.envValid));   lcd.setCursor(0, 2); lcd.print(l);
      snprintf(l, 21, "%-6s%-9s%5s", "Lux", String(sensor.rawLux, 0).c_str(), diagNanoFlag(sensor.msLight, sensor.lightValid)); lcd.setCursor(0, 3); lcd.print(l);
      break;
    case 1:   // Nano: ultrasonic tanks + reservoir flow (raw)
      snprintf(l, 21, "%-8s Nano Tank", hdr);                             lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "%-7scm:%-6s%4s", "Res", String(sensor.rawResCm, 0).c_str(), diagNanoFlag(sensor.msTank, sensor.tankValid)); lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "%-7scm:%-6s%4s", "Mix", String(sensor.rawMixCm, 0).c_str(), diagNanoFlag(sensor.msTank, sensor.tankValid)); lcd.setCursor(0, 2); lcd.print(l);
      snprintf(l, 21, "%-7sLpm:%-5s%4s", "Flow", String(sensor.rawFlow, 1).c_str(), diagNanoFlag(sensor.msTank, true));            lcd.setCursor(0, 3); lcd.print(l);
      break;
    case 2: {  // Nano: per-probe capacitive soil raw ADC (A1/A2 B1/B2 C1/C2) from the normal SOIL packet
      snprintf(l, 21, "%-8s Soil ch", hdr);                               lcd.setCursor(0, 0); lcd.print(l);
      for (int c = 0; c < NUM_COLUMNS && c < 3; c++) {
        if (!COLUMN_ENABLED[c]) { snprintf(l, 21, "%c: disabled", COL_TAG[c]); lcd.setCursor(0, c + 1); lcd.print(l); continue; }
        // mapped % (robust combine) next to the two raw probes, so raw-vs-% is visible at a glance.
        snprintf(l, 21, "%c %3d%% %4d/%-4d", COL_TAG[c], sensor.soil[c], sensor.rawSoil[c][0], sensor.rawSoil[c][1]);
        lcd.setCursor(0, c + 1); lcd.print(l);
      }
      break;
    }
    case 3: {  // Nano: NPK raw registers (N/P/K) per column
      snprintf(l, 21, "%-8s Nano NPK", hdr);                              lcd.setCursor(0, 0); lcd.print(l);
      for (int c = 0; c < NUM_COLUMNS && c < 3; c++) {
        String v;
        if (!COLUMN_ENABLED[c]) v = "-";
        else v = String((int)sensor.rawNpk[c][4]) + "/" + String((int)sensor.rawNpk[c][5]) + "/" + String((int)sensor.rawNpk[c][6]);
        const char *fl = COLUMN_ENABLED[c] ? diagNanoFlag(sensor.msNpk[c], sensor.npkValid[c]) : "off";
        snprintf(l, 21, "%c %-13s%5s", COL_TAG[c], v.c_str(), fl);
        lcd.setCursor(0, c + 1); lcd.print(l);
      }
      break;
    }
    case 4: {  // Nano: the NPK sensor's OWN EC + pH per column.
      // These were captured (sensor.npk[c][2]/[3]), uploaded to ThingSpeak Ch3 and published to
      // Firebase, but had no on-rig view at all -- page 3 shows only N/P/K. Distinct from page 6,
      // which is ESP2's separate EC/pH probes in the mixing tank.
      snprintf(l, 21, "%-8s NPK Chem", hdr);                              lcd.setCursor(0, 0); lcd.print(l);
      for (int c = 0; c < NUM_COLUMNS && c < 3; c++) {
        String v;
        if (!COLUMN_ENABLED[c]) v = "-";
        else v = "EC" + String(sensor.npk[c][2], 2) + " pH" + String(sensor.npk[c][3], 1);
        const char *fl = COLUMN_ENABLED[c] ? diagNanoFlag(sensor.msNpk[c], sensor.npkValid[c]) : "off";
        snprintf(l, 21, "%c %-13s%4s", COL_TAG[c], v.c_str(), fl);
        lcd.setCursor(0, c + 1); lcd.print(l);
      }
      break;
    }
    case 5:   // ESP1 local: battery raw ADC + device presence
      snprintf(l, 21, "%-8s ESP1 Loc", hdr);                              lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "BatV raw%-5d%5.1fV", (int)readAdcTrimmed(PIN_BATT_V), battV); lcd.setCursor(0, 1); lcd.print(l);
      // ACS758 via ADS1115 (replaces the dead GPIO34 opto read). Shows the SIGNED amps -- the 050B is
      // bidirectional -- plus an ok/X flag, so a missing ADC reads as a fault and never as "0.0 A".
      snprintf(l, 21, "Cur %7.2fA ADS:%s", battIsigned, acsOk ? "ok" : "X"); lcd.setCursor(0, 2); lcd.print(l);
      snprintf(l, 21, "RTC:%s LCD:%s SD:%s", i2cPresent(0x68) ? "ok" : "X", i2cPresent(LCD_ADDR) ? "ok" : "X", sdOk ? "ok" : "X"); lcd.setCursor(0, 3); lcd.print(l);
      break;
    case 6:   // ESP2: pH / EC / ACS712 raw ADC (live CAL stream) -- ESP2's OWN probes, not the NPK's
      snprintf(l, 21, "%-8s ESP2 Chem", hdr);                             lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "%-4sraw%-8ld%5s", "pH",  (long)diagEsp2Raw[0], diagEsp2Flag(0)); lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "%-4sraw%-8ld%5s", "EC",  (long)diagEsp2Raw[1], diagEsp2Flag(1)); lcd.setCursor(0, 2); lcd.print(l);
      snprintf(l, 21, "%-4sraw%-8ld%5s", "ACS", (long)diagEsp2Raw[2], diagEsp2Flag(2)); lcd.setCursor(0, 3); lcd.print(l);
      break;
    case 7:   // ESP2: PZEM AC power (V/I/P, engineering values from the CAL stream)
      snprintf(l, 21, "%-8s ESP2 Pwr", hdr);                              lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "Vac:%-8s%5s", String(diagEsp2Raw[3], 0).c_str(), diagEsp2Flag(3)); lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "Iac:%-8s%5s", String(diagEsp2Raw[4], 2).c_str(), diagEsp2Flag(4)); lcd.setCursor(0, 2); lcd.print(l);
      snprintf(l, 21, "Pw :%-8s%5s", String(diagEsp2Raw[5], 0).c_str(), diagEsp2Flag(5)); lcd.setCursor(0, 3); lcd.print(l);
      break;
    case 8:   // ESP2 flow (1/2): RESMIX / MIXIRR / NUT A  (pulse counts)
      snprintf(l, 21, "%-8s ESP2 Flw1", hdr);                             lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "%-7s p%-7ld%4s", "RESMIX", (long)diagEsp2Raw[6], diagEsp2Flag(6)); lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "%-7s p%-7ld%4s", "MIXIRR", (long)diagEsp2Raw[7], diagEsp2Flag(7)); lcd.setCursor(0, 2); lcd.print(l);
      snprintf(l, 21, "%-7s p%-7ld%4s", "NUT A",  (long)diagEsp2Raw[8], diagEsp2Flag(8)); lcd.setCursor(0, 3); lcd.print(l);
      break;
    default:  // case 9: ESP2 flow (2/2): NUT B / NUT C
      snprintf(l, 21, "%-8s ESP2 Flw2", hdr);                             lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "%-7s p%-7ld%4s", "NUT B", (long)diagEsp2Raw[9],  diagEsp2Flag(9));  lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "%-7s p%-7ld%4s", "NUT C", (long)diagEsp2Raw[10], diagEsp2Flag(10)); lcd.setCursor(0, 2); lcd.print(l);
      snprintf(l, 21, "UP/DN=page BACK=out"); lcd.setCursor(0, 3); lcd.print(l);
      break;
  }
}

/* =============================================================================
 *  LCD UI + BUTTONS  (spec sec.18.10)
 * ========================================================================== */
void wakeBacklight() {
  backlightOn = true; backlightMs = millis();
  lcd.backlight();
}

void handleButtons() {
  static bool last[5] = { HIGH, HIGH, HIGH, HIGH, HIGH };
  static unsigned long lastChange[5] = { 0, 0, 0, 0, 0 };
  const uint8_t pins[5] = { BTN_UP, BTN_DOWN, BTN_ENTER, BTN_BACK, BTN_MODE };

  // ---- Emergency-Off combo: MODE + BACK held TOGETHER (Part B) ----------------
  // Highest priority, fires regardless of lock (safety override). One-shot via latch.
  bool modeDown = (digitalRead(BTN_MODE) == LOW);
  bool backDown = (digitalRead(BTN_BACK) == LOW);
  if (modeDown && backDown) {
    if (!estopComboLatch) {
      estopComboLatch = true;
      if (uiMode == UI_TEST) { sendEsp2("TEST,EXIT"); testArmPending = false; }
      logEvent("ESP1", "CMD", "ESTOP|MODE+BACK");
      enterEmergencyStop(true);                    // stops actuators, cuts ESP2 power, shows recovery
    }
    for (int i = 0; i < 5; i++) last[i] = digitalRead(pins[i]);   // swallow these edges
    return;
  }
  estopComboLatch = false;                          // combo released -> re-arm

  // ---- WiFi portal active: BACK cancels it, other buttons are ignored ----------
  if (portalActive) {
    static bool portalBackLatch = false;
    if (digitalRead(BTN_BACK) == LOW) {
      if (!portalBackLatch) { portalBackLatch = true; portalCancel = true; wakeBacklight();
                              logEvent("ESP1", "CMD", "WIFI_PORTAL|CANCEL"); }
    } else portalBackLatch = false;
    for (int i = 0; i < 5; i++) last[i] = digitalRead(pins[i]);   // swallow edges while portal is up
    return;
  }

  // ---- Locked: ignore all buttons except the UP+DOWN unlock combo (Part D) -----
  if (lcdLocked) {
    static bool unlockLatch = false;
    bool upDown   = (digitalRead(BTN_UP)   == LOW);
    bool downDown = (digitalRead(BTN_DOWN) == LOW);
    if (upDown && downDown) {                                     // unlock = hold UP+DOWN
      if (!unlockLatch) {
        unlockLatch = true;
        lcdLocked = false; saveLock(); wakeBacklight();
        logEvent("ESP1", "STATE", "LCD_UNLOCK");
      }
      for (int i = 0; i < 5; i++) last[i] = digitalRead(pins[i]); // don't replay edges after unlock
      return;
    }
    unlockLatch = false;
    // Keep the unlock hint readable on a real (DEBOUNCED) press only. A raw level read here spammed
    // wakeBacklight() every loop on the noisy strapping-pin buttons, so the lock screen never dimmed --
    // the same reason the main button loop below debounces edges.
    for (int i = 0; i < 5; i++) {
      bool cur = digitalRead(pins[i]);
      if (cur != last[i] && millis() - lastChange[i] > BTN_DEBOUNCE_MS) {
        lastChange[i] = millis(); last[i] = cur;
        if (cur == LOW) wakeBacklight();                          // fresh press -> light to read the hint
      }
    }
    return;
  }

  // ---- Service run in progress: the process is uninterruptable ----------------
  // Only two inputs are honored: UP+DOWN together RELEASES the screen (the run keeps going, it is not
  // an abort), and ENTER advances a receipt / error page. Everything else is swallowed so nobody can
  // wander into a menu mid-run. The MODE+BACK emergency stop is handled above and always works.
  // (never swallows buttons during a held fault / E-stop -- the recovery selectors live below)
  if (runPhase != RUN_NONE && runLocked && !esp2Held && sysState != EMERGENCY_STOP) {
    static bool runUnlockLatch = false, runEnterLatch = false, runBackLatch = false;
    bool up = (digitalRead(BTN_UP) == LOW), dn = (digitalRead(BTN_DOWN) == LOW);
    bool ent = (digitalRead(BTN_ENTER) == LOW), bk = (digitalRead(BTN_BACK) == LOW);

    // Cancel confirm open: UP/DOWN pick, ENTER commits, BACK backs out. Takes priority over the
    // normal run-screen bindings so the prompt cannot be dismissed by accident.
    if (cancelPrompt) {
      if (up && !dn)      { if (!runUnlockLatch) { runUnlockLatch = true; cancelSel = (cancelSel + 2) % 3; wakeBacklight(); } }
      else if (dn && !up) { if (!runUnlockLatch) { runUnlockLatch = true; cancelSel = (cancelSel + 1) % 3; wakeBacklight(); } }
      else runUnlockLatch = false;
      if (ent) { if (!runEnterLatch) { runEnterLatch = true; cancelRun(cancelSel, "OPERATOR"); } }
      else runEnterLatch = false;
      if (bk)  { if (!runBackLatch)  { runBackLatch = true; cancelPrompt = false; wakeBacklight(); } }
      else runBackLatch = false;
      for (int i = 0; i < 5; i++) last[i] = digitalRead(pins[i]);
      return;
    }

    if (up && dn) {
      if (!runUnlockLatch) {
        runUnlockLatch = true; runLocked = false; wakeBacklight();
        logEvent("ESP1", "ACT", "RUN|SCREEN_RELEASED");
      }
    } else runUnlockLatch = false;
    if (ent && !up && !dn) {
      if (!runEnterLatch) { runEnterLatch = true; wakeBacklight(); runUiAdvance(); }
    } else runEnterLatch = false;
    // BACK during a live run opens the cancel confirm. ENTER is already the receipt-advance, and
    // BACK was simply swallowed here, so this adds a stop without taking a binding away.
    if (bk && !up && !dn && !ent) {
      if (!runBackLatch) { runBackLatch = true; cancelPrompt = true; cancelSel = 1; wakeBacklight(); }
    } else runBackLatch = false;
    for (int i = 0; i < 5; i++) last[i] = digitalRead(pins[i]);   // swallow every other edge
    return;
  }

  for (int i = 0; i < 5; i++) {
    bool cur = digitalRead(pins[i]);
    // Debounced edge only: a noisy/floating contact (esp. on strapping-pin MODE/GPIO2)
    // must not spam wakeBacklight()/I2C every loop iteration.
    if (cur != last[i] && millis() - lastChange[i] > BTN_DEBOUNCE_MS) {
      lastChange[i] = millis();
      last[i] = cur;
      if (cur == LOW) {                            // falling edge = press
        wakeBacklight();
        lastBtnMs = millis();                       // real activity -> reset the idle fail-safe timer

        // MODE (i==4): toggle into/out of the Settings menu (spec sec.18.10)
        if (i == 4) {
          if (sysState == EMERGENCY_STOP) continue;  // keep the recovery prompt up; use the combo to re-stop
          if (uiMode == UI_DATA) { uiMode = UI_MENU; setSel = 0; }
          else settingsLeaveToData();                // leave any menu -> clean up + resume normal operation
          continue;
        }

        // ESP2 fault-hold recovery selector (sec.19.4.8.2): UP/DOWN choose one of the 4
        // options, ENTER issues it (mirror of the GSM STOP/RELEASE/IRRIGATE/NORMAL reply).
        if (esp2Held && uiMode == UI_DATA) {
          if (cancelPrompt) {                                         // 3-way confirm is open
            if      (i == 0) cancelSel = (cancelSel + 2) % 3;         // UP
            else if (i == 1) cancelSel = (cancelSel + 1) % 3;         // DOWN
            else if (i == 2) cancelRun(cancelSel, lastHoldCode.length() ? lastHoldCode.c_str() : "OPERATOR");
            else if (i == 3) cancelPrompt = false;                    // BACK -> back to the recovery menu
            continue;
          }
          if      (i == 0) faultRecovSel = (faultRecovSel + RECOV_N - 1) % RECOV_N;   // UP
          else if (i == 1) faultRecovSel = (faultRecovSel + 1) % RECOV_N;             // DOWN
          else if (i == 2) {                                          // ENTER -> act
            if (faultRecovSel == 4) { cancelPrompt = true; cancelSel = 1; }  // open the confirm
            else                     issueRecovery(faultRecovSel);
          }
          continue;
        }

        // E-stop recovery selector (Part B): UP/DOWN choose, ENTER confirms. Takes over
        // the other buttons while stopped so the operator must deliberately return.
        if (sysState == EMERGENCY_STOP && uiMode == UI_DATA) {
          if (i == 0 || i == 1) estopSel ^= 1;                 // toggle Return / Stay
          else if (i == 2 && estopSel == 0) setState(STARTUP_SYNC);  // re-powers + re-validates ESP2
          continue;
        }

        // In any settings UI, the other 4 buttons drive the menu, not the data pages.
        if (uiMode != UI_DATA) { settingsButton(i); continue; }

        // ---- normal data-page navigation (UI_DATA) ----
        if (i == 0) lcdPage = (lcdPage + PAGE_COUNT - 1) % PAGE_COUNT;   // UP
        else if (i == 1) lcdPage = (lcdPage + 1) % PAGE_COUNT;           // DOWN
      }
    }
  }
}

// Leave any Settings/Testing/Calibration/Diag screen back to the data view, cleaning up whatever the
// mode held (ESP2 power, CAL streams, open dialogs, unsaved edit) so normal automation resumes.
void settingsLeaveToData() {
  if (uiMode == UI_TEST) {                     // leaving Testing: stop + power ESP2 off
    sendEsp2("TEST,EXIT"); testArmPending = false; esp2SetPower(false);
    if (sysState == TEST_MODE) setState(IDLE_STATE);
  } else if (uiMode == UI_CAL) {               // leaving Calibration: stop stream + power ESP2 off
    if (calOpen) calCloseTarget();
    esp2SetPower(false); logEvent("ESP1", "CAL", "EXIT");
  } else if (uiMode == UI_DIAG) {              // leaving Sensor Diag: stop streams + power ESP2 off
    diagStopStream(); esp2SetPower(false); logEvent("ESP1", "DIAG", "EXIT");
  }
  // Cancel any open dialog / discard an in-progress edit (nothing is committed on a fail-safe exit).
  editConfirm = false; restoreConfirm = false; resetConfirm = false;
  editDirty = false; editItem = -1;
  uiMode = UI_DATA;
}

// Fail-safe: if the operator wandered off inside a menu that pauses/hinders normal operation, auto-
// return to the data screen after UI_IDLE_TIMEOUT_MS of no button activity. Safety states (E-stop /
// held fault) run with uiMode == UI_DATA, so they are never auto-dismissed; the portal owns its own timeout.
void uiIdleTick() {
  if (uiMode == UI_DATA || portalActive) return;
  if (sysState == EMERGENCY_STOP || esp2Held) return;
  if (millis() - lastBtnMs < UI_IDLE_TIMEOUT_MS) return;
  logEvent("ESP1", "STATE", "UI_IDLE_TIMEOUT");
  settingsLeaveToData();
  lastBtnMs = millis();
}

/* =============================================================================
 *  SETTINGS / TESTING MENU  (driven by handleButtons when uiMode != UI_DATA)
 * ========================================================================== */
void sendEsp2(const String &body) {
  esp2Serial.print(FRAME_START); esp2Serial.print(",");
  esp2Serial.print(body);
  esp2Serial.print(","); esp2Serial.println(FRAME_END);
  logEvent("ESP1", "CMD", "ESP2|" + body);
}

// Derive the 3-state column mode (0=AUTO,1=IRRIGATION_ONLY,2=OFF) for the editor.
static int colMode3(int c) { return !COLUMN_ENABLED[c] ? 2 : (col[c].mode == MODE_AUTO ? 0 : 1); }

// Testing row helpers: rows 0..15 are single relays, 16 = Fill combo, 17+c = Push>Col c combo.
static bool testRowVisible(int r) {
  (void)r;
  return true;   // Testing is a manual hardware bench: show every relay, Fill, and all Push>Col rows
                 // (incl. columns disabled in the run config, e.g. Col C) so any wiring can be actuated.
}
static String testRowName(int r) {
  if (r < 16) return TEST_NAMES[r];
  if (r == TEST_FILL_ROW) return "Fill Res>Mix";
  return String("Push>Col ") + COL_TAG[r - (TEST_FILL_ROW + 1)];
}

// Load the per-column schedule fields into the working copy for editCol.
static void seedSchedule() {
  editTmp[0] = colSchedMode[editCol];                          // AUTO / MANUAL
  editTmp[1] = COL_WIN_START[editCol] / 60; editTmp[2] = COL_WIN_START[editCol] % 60;
  editTmp[3] = COL_WIN_END[editCol]   / 60; editTmp[4] = COL_WIN_END[editCol]   % 60;
}

static int clampi(int v, int lo, int hi);   // defined below; used by commitEditor's THRESH clamp

// Apply the working copy (editTmp) for the current editItem to live config + NVS. Called by the
// editor's final ENTER and by the three-way confirm dialog's SAVE (companion spec §B).
void commitEditor() {
  switch (editItem) {
    case SET_CLOCK:
      if (rtcOk) rtc.adjust(DateTime(editTmp[0], editTmp[1], editTmp[2], editTmp[3], editTmp[4], 0));
      logEvent("ESP1", "STATE", "CLOCK_SET");
      break;
    case SET_SCHEDULE:
      colSchedMode[editCol] = editTmp[0];
      if (editTmp[0] == SCHED_MANUAL) {
        COL_WIN_START[editCol] = editTmp[1] * 60 + editTmp[2];
        COL_WIN_END[editCol]   = editTmp[3] * 60 + editTmp[4];
      }
      saveSchedule(editCol);
      logEvent("ESP1", "STATE", String("SCHED_SET|COL_") + COL_TAG[editCol]);
      break;
    case SET_COLMODE:
      if (editTmp[0] == 2) { COLUMN_ENABLED[editCol] = false; }
      else { COLUMN_ENABLED[editCol] = true; col[editCol].mode = (editTmp[0] == 0) ? MODE_AUTO : MODE_IRRIGATION_ONLY; saveColumn(editCol); }
      saveColEnable(editCol);
      logEvent("ESP1", "STATE", String("MODE_SET|COL_") + COL_TAG[editCol]);
      break;
    case SET_PRESET:
      if (editTmp[0] == 0) { const CropPreset &p = CROP_PRESETS[editTmp[1]];
        col[editCol].targetN = p.N; col[editCol].targetP = p.P; col[editCol].targetK = p.K; col[editCol].targetPH = p.pH;
      } else {
        col[editCol].targetN = editTmp[2]; col[editCol].targetP = editTmp[3];
        col[editCol].targetK = editTmp[4]; col[editCol].targetPH = editTmp[5] / 10.0f;
      }
      saveColumn(editCol);
      logEvent("ESP1", "STATE", String("PRESET_SET|COL_") + COL_TAG[editCol]);
      break;
    case SET_THRESH:
      if (editTmp[1] <= editTmp[0]) editTmp[1] = clampi(editTmp[0] + 1, 0, 100);   // stop > start
      soilStartPct = editTmp[0]; soilStopPct = editTmp[1]; fertGap = editTmp[2]; saveThresholds();
      logEvent("ESP1", "STATE", "THRESH_SET");
      break;
  }
}

// Restore operational config to compiled defaults (companion spec §C). Resets automation settings;
// KEEPS calibration (separate NVS namespace), column-enable flags (physical wiring), and WiFi/TS
// connectivity setup. Idle-only; no reboot. EC/pH safe windows are compile constants (not in NVS,
// re-sent each work order), so there is nothing stale to ACK-push here.
void restoreDefaults() {
  for (int c = 0; c < NUM_COLUMNS; c++) {
    col[c].mode = MODE_AUTO;
    col[c].targetN = col[c].targetP = col[c].targetK = 0.0f; col[c].targetPH = 6.0f;
    col[c].name[0] = '\0';
    colSchedMode[c] = SCHED_AUTO;
    COL_WIN_START[c] = DEF_WIN_START[c]; COL_WIN_END[c] = DEF_WIN_END[c];
    saveColumn(c); saveSchedule(c);
  }
  soilStartPct = 35; soilStopPct = 45; fertGap = 20.0f; saveThresholds();
  logEvent("ESP1", "RESET", "CONFIG|DEFAULTS|kept cal+cols+wifi");
}

// Seed the working copy when an editor is opened.
void enterEditor(int item) {
  editItem = item; editField = 0; editCol = 0; editDirty = false;
  switch (item) {
    case SET_CLOCK: {
      DateTime n = rtcOk ? rtc.now() : DateTime(2026, 1, 1, 0, 0, 0);
      editTmp[0] = n.year(); editTmp[1] = n.month(); editTmp[2] = n.day();
      editTmp[3] = n.hour(); editTmp[4] = n.minute();
      break;
    }
    case SET_SCHEDULE: editCol = 0; seedSchedule(); break;
    case SET_COLMODE:  editCol = 0; editTmp[0] = colMode3(0); break;
    case SET_PRESET:   editCol = 0;                            // 0=src(0 named/1 manual),1=presetIdx,2..5=N,P,K,pH*10
      editTmp[0] = 0; editTmp[1] = 0;
      editTmp[2] = (int)col[0].targetN; editTmp[3] = (int)col[0].targetP;
      editTmp[4] = (int)col[0].targetK; editTmp[5] = (int)(col[0].targetPH * 10);
      break;
    case SET_THRESH:   editTmp[0] = soilStartPct; editTmp[1] = soilStopPct; editTmp[2] = (int)fertGap; break;
  }
  uiMode = UI_EDIT;
}

// Clamp helper for UI_EDIT field deltas.
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void settingsButton(int i) {
  // i: 0=UP 1=DOWN 2=ENTER 3=BACK  (MODE handled in handleButtons)
  if (uiMode == UI_CAL)  { calButton(i);  return; }
  if (uiMode == UI_DIAG) { diagButton(i); return; }

  // ---- Restore Defaults double-confirm (companion spec §C.3) ----
  if (restoreConfirm) {
    if (i == 0 || i == 1) restoreSel ^= 1;                 // toggle NO / YES
    else if (i == 3) restoreConfirm = false;               // BACK = abort
    else if (i == 2) {
      if (restoreSel == 1) restoreDefaults();              // YES -> wipe operational config
      restoreConfirm = false; uiMode = UI_DATA;
    }
    return;
  }

  // ---- Reboot ESP1 double-confirm ----
  if (resetConfirm) {
    if (i == 0 || i == 1) resetSel ^= 1;                   // toggle NO / YES
    else if (i == 3) resetConfirm = false;                 // BACK = abort
    else if (i == 2) {
      if (resetSel == 1) rebootPending = true;             // YES -> core-1 loop reboots (clean logFlush)
      resetConfirm = false; uiMode = UI_DATA;
    }
    return;
  }

  if (uiMode == UI_MENU) {
    if (i == 0) setSel = (setSel + SET_COUNT - 1) % SET_COUNT;
    else if (i == 1) setSel = (setSel + 1) % SET_COUNT;
    else if (i == 3) uiMode = UI_DATA;                         // BACK = exit
    else if (i == 2) {                                         // ENTER = open item
      if (setSel == SET_EXIT) { uiMode = UI_DATA; }
      else if (setSel == SET_LOCK) {                            // lock the LCD (Part D)
        lcdLocked = true; saveLock();
        logEvent("ESP1", "STATE", "LCD_LOCK");
        uiMode = UI_DATA;                                      // back to data view (automation unaffected)
      }
      else if (setSel == SET_TESTING) {
        // Idle-only (spec sec.18.10.8.1). Power ESP2 via the GPIO4 relay, PRIME it (let it
        // boot), then send TEST,ENTER repeatedly until it ACKs -- the single boot READY can be
        // lost on a cold relay power-up (see testHoldTick arming retry).
        if (sysState != IDLE_STATE) return;
        esp2SetPower(true);                                    // power ESP2 -> boot
        testArmPending = true;
        esp2TestArmed = false; testArmTries = 0; lastTestArmMs = 0;
        esp2WarmupMs = millis();                               // prime reference (TEST_PRIME_MS)
        setState(TEST_MODE);
        uiMode = UI_TEST; testSel = 0; testOnBit = -1;
        // No immediate send: the primed retry in testHoldTick owns arming (after TEST_PRIME_MS).
      }
      else if (setSel == SET_WIFI) {                            // toggle STA WiFi + telemetry (persisted)
        wifiEnabled = !wifiEnabled; saveWifiEn();
        logEvent("ESP1", "CMD", wifiEnabled ? "WIFI|ON" : "WIFI|OFF");   // netTask gate acts next iteration
      }
      else if (setSel == SET_SOFTAP) {                          // toggle the SoftAP provisioning portal
        if (portalActive || portalRequested) { portalCancel = true; logEvent("ESP1", "CMD", "WIFI_PORTAL|CANCEL"); }
        else { portalRequested = true; logEvent("ESP1", "CMD", "WIFI_PORTAL|START"); }   // netTask brings up the AP (banner)
      }
      else if (setSel == SET_CALIB) enterCal();                 // idle-only calibration (§A.6)
      else if (setSel == SET_DIAG)  enterDiag();                // read-only raw sensor diagnostics
      else if (setSel == SET_RESTORE) {                          // Restore Defaults (§C): idle-only, double-confirm
        if (sysState != IDLE_STATE) return;
        restoreConfirm = true; restoreSel = 0;                  // default cursor = NO
      }
      else if (setSel == SET_RESET) {                            // manual ESP1 reboot: double-confirm (anytime)
        resetConfirm = true; resetSel = 0;                       // default cursor = NO
      }
      else enterEditor(setSel);
    }
    return;
  }

  if (uiMode == UI_TEST) {
    // Dead-man: ENTER is NOT an edge action here (held = ON, handled by testHoldTick).
    if (i == 0)      { do { testSel = (testSel + TEST_ROWS - 1) % TEST_ROWS; } while (!testRowVisible(testSel)); }  // UP
    else if (i == 1) { do { testSel = (testSel + 1) % TEST_ROWS; } while (!testRowVisible(testSel)); }              // DOWN
    else if (i == 3) {                                         // BACK exit Testing
      sendEsp2("TEST,EXIT"); testOnBit = -1; testArmPending = false;
      esp2SetPower(false);                                     // power ESP2 back off (idle model)
      if (sysState == TEST_MODE) setState(IDLE_STATE);
      uiMode = UI_MENU;
    }
    return;
  }

  if (uiMode == UI_EDIT) {
    int delta = (i == 0) ? +1 : (i == 1) ? -1 : 0;

    // ---- three-way "unsaved changes" dialog (companion spec §B) ----
    if (editConfirm) {
      if (i == 0)      confirmSel = (confirmSel + 2) % 3;
      else if (i == 1) confirmSel = (confirmSel + 1) % 3;
      else if (i == 3) editConfirm = false;                    // BACK on dialog = CANCEL (stay)
      else if (i == 2) {
        if (confirmSel == 0)      { commitEditor(); editConfirm = false; editDirty = false; uiMode = UI_MENU; editItem = -1; }  // SAVE
        else if (confirmSel == 1) { editConfirm = false; editDirty = false; uiMode = UI_MENU; editItem = -1; }                  // DISCARD
        else                      { editConfirm = false; }                                                                     // CANCEL -> stay
      }
      return;
    }
    // BACK: silent exit if clean, else open the three-way dialog (catches accidental edits).
    if (i == 3) {
      if (editDirty) { editConfirm = true; confirmSel = 0; }
      else { uiMode = UI_MENU; editItem = -1; }
      return;
    }
    // Mark dirty on a value change (exclude the column-select field 0 of per-column editors).
    if (delta && !(editField == 0 && (editItem == SET_SCHEDULE || editItem == SET_COLMODE || editItem == SET_PRESET)))
      editDirty = true;

    switch (editItem) {
      case SET_CLOCK:
        if (delta) {
          if (editField == 0) editTmp[0] = clampi(editTmp[0] + delta, 2020, 2099);
          else if (editField == 1) editTmp[1] = clampi(editTmp[1] + delta, 1, 12);
          else if (editField == 2) editTmp[2] = clampi(editTmp[2] + delta, 1, 31);
          else if (editField == 3) editTmp[3] = clampi(editTmp[3] + delta, 0, 23);
          else if (editField == 4) editTmp[4] = clampi(editTmp[4] + delta, 0, 59);
        } else if (i == 2) {                                    // ENTER advances / commits
          if (++editField > 4) { commitEditor(); editDirty = false; uiMode = UI_MENU; editItem = -1; }
        }
        break;

      case SET_SCHEDULE:
        // field0 column, field1 AUTO/MANUAL; fields 2-5 (start/end H:M) only if MANUAL
        if (delta) {
          if (editField == 0) { editCol = clampi(editCol + delta, 0, NUM_COLUMNS - 1); seedSchedule(); }
          else if (editField == 1) editTmp[0] = (editTmp[0] == SCHED_AUTO) ? SCHED_MANUAL : SCHED_AUTO;
          else if (editField == 2) editTmp[1] = clampi(editTmp[1] + delta, 0, 23);
          else if (editField == 3) editTmp[2] = clampi(editTmp[2] + delta, 0, 59);
          else if (editField == 4) editTmp[3] = clampi(editTmp[3] + delta, 0, 23);
          else if (editField == 5) editTmp[4] = clampi(editTmp[4] + delta, 0, 59);
        } else if (i == 2) {
          int lastField = (editTmp[0] == SCHED_MANUAL) ? 5 : 1;
          if (++editField > lastField) { commitEditor(); editDirty = false; uiMode = UI_MENU; editItem = -1; }
        }
        break;

      case SET_COLMODE:                                         // AUTO / IRRIGATION_ONLY / OFF
        if (delta) {
          if (editField == 0) { editCol = clampi(editCol + delta, 0, NUM_COLUMNS - 1); editTmp[0] = colMode3(editCol); }
          else if (editField == 1) editTmp[0] = (editTmp[0] + (delta > 0 ? 1 : 2)) % 3;
        } else if (i == 2) {
          if (++editField > 1) { commitEditor(); editDirty = false; uiMode = UI_MENU; editItem = -1; }
        }
        break;

      case SET_PRESET:
        // field0 column, field1 source (NAMED/MANUAL); NAMED->field2 presetIdx; MANUAL->fields2-5 N/P/K/pH
        if (delta) {
          if (editField == 0) {
            editCol = clampi(editCol + delta, 0, NUM_COLUMNS - 1);
            editTmp[2] = (int)col[editCol].targetN; editTmp[3] = (int)col[editCol].targetP;
            editTmp[4] = (int)col[editCol].targetK; editTmp[5] = (int)(col[editCol].targetPH * 10);
          }
          else if (editField == 1) editTmp[0] = !editTmp[0];                 // NAMED <-> MANUAL
          else if (editTmp[0] == 0) editTmp[1] = (editTmp[1] + (delta > 0 ? 1 : NUM_PRESETS - 1)) % NUM_PRESETS;  // named idx
          else if (editField == 2) editTmp[2] = clampi(editTmp[2] + delta, 0, 999);   // N
          else if (editField == 3) editTmp[3] = clampi(editTmp[3] + delta, 0, 999);   // P
          else if (editField == 4) editTmp[4] = clampi(editTmp[4] + delta, 0, 999);   // K
          else if (editField == 5) editTmp[5] = clampi(editTmp[5] + delta, 0, 140);   // pH*10
        } else if (i == 2) {
          int lastField = (editTmp[0] == 0) ? 2 : 5;                          // named: up to idx; manual: up to pH
          if (++editField > lastField) { commitEditor(); editDirty = false; uiMode = UI_MENU; editItem = -1; }
        }
        break;

      case SET_THRESH:
        if (delta) {
          if (editField == 0) editTmp[0] = clampi(editTmp[0] + delta, 0, 100);
          else if (editField == 1) editTmp[1] = clampi(editTmp[1] + delta, 0, 100);
          else if (editField == 2) editTmp[2] = clampi(editTmp[2] + delta, 0, 500);
        } else if (i == 2) {
          if (++editField > 2) { commitEditor(); editDirty = false; uiMode = UI_MENU; editItem = -1; }
        }
        break;
    }
    return;
  }
}

/* ---- Dead-man relay hold: stream TEST,HOLD while ENTER is physically held ---
 * (spec sec.18.10.8.2). Runs every loop while in UI_TEST; ESP2 owns the timeout
 * and the 10 s hard cap. */
void testHoldTick() {
  if (uiMode != UI_TEST) return;

  // ---- Primed arming: prove ESP2 is actually in TEST mode before relying on HOLD ----
  // After the GPIO4 power-on prime (TEST_PRIME_MS, lets ESP2 finish booting), re-send
  // TEST,ENTER every TEST_ARM_RETRY_MS until ESP2 confirms with ACK,TEST,ENTER (esp2TestArmed).
  // This is the actual fix: the single boot-time READY is easily lost on a cold relay power-up.
  if (!esp2TestArmed && testArmTries < TEST_ARM_MAX_TRIES
      && (unsigned long)(millis() - esp2WarmupMs) >= TEST_PRIME_MS
      && (unsigned long)(millis() - lastTestArmMs) >= TEST_ARM_RETRY_MS) {
    lastTestArmMs = millis();
    testArmPending = false;
    testArmTries++;
    sendEsp2("TEST,ENTER");                                    // sent repeatedly until ACK (or cap)
  }

  static bool wasHeld = false;
  static unsigned long lastSendMs = 0;
  bool held = (digitalRead(BTN_ENTER) == LOW);

  if (held) {
    if (!wasHeld) {                                            // press edge
      wasHeld = true; lastSendMs = 0; testOnBit = testSel;
      logEvent("ESP1", "ACT", String("TEST|START|") + testRowName(testSel));
    }
    if (millis() - lastSendMs >= 150) {                        // keep-alive stream (< ESP2 timeout)
      lastSendMs = millis();
      sendEsp2(String("TEST,HOLD,") + testSel);
    }
  } else if (wasHeld) {                                        // release edge
    wasHeld = false; testOnBit = -1;
    sendEsp2("TEST,RELEASE");
    logEvent("ESP1", "ACT", "TEST|STOP");
  }
}

// Write exactly 20 columns at the start of a row: left-justify, space-pad, hard-truncate. Padding is
// what prevents "floating" leftover glyphs when a line's new text is shorter than what was there before.
static void lcdRow(uint8_t row, const char *s) {
  char buf[21];
  snprintf(buf, sizeof(buf), "%-20.20s", s);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void lcdTick() {
  if (millis() - lastLcdMs < LCD_REFRESH_MS) return;
  lastLcdMs = millis();

  // I2C hardening (stage 'L' WDT fix): probe the LCD with ONE bounded transaction before
  // issuing the page's many writes. If it doesn't ACK (wedged/missing bus), skip all writes
  // this tick so a dead display can't stall the loop past the WDT; re-init when it returns.
  Wire.beginTransmission(LCD_ADDR);
  bool lcdPresent = (Wire.endTransmission() == 0);
  static bool lcdWasPresent = true;
  if (!lcdPresent) { lcdWasPresent = false; return; }
  if (!lcdWasPresent) { lcd.init(); lcd.backlight(); lcdWasPresent = true; }   // recovered

  // Fault display stays lit a full minute (vs the 10 s button window) so a fault is noticed, then dims
  // to save battery/LCD. Any button press or fresh fault relights it (wakeBacklight resets backlightMs).
  unsigned long blWindow = (lcdPage == PAGE_FAULT || sysState == EMERGENCY_STOP)
                             ? LCD_FAULT_BACKLIGHT_MS : LCD_BACKLIGHT_MS;
  if (backlightOn && millis() - backlightMs > blWindow
      && uiMode == UI_DATA && !portalActive && sysState != ACTIVE_STATE) {
    backlightOn = false; lcd.noBacklight();
  }

  // LCD lock screen (Part D): takes over rendering only -- automation keeps running
  // (controlTick etc. gate on uiMode, which stays UI_DATA while locked). Shows the
  // unlock combo. E-stop still shows here so a held operator knows action is needed.
  static bool lastLocked = false;
  if (lcdLocked) {
    if (!lastLocked) { lcd.clear(); lastLocked = true; }
    lcd.setCursor(0, 0); lcd.print("***   LOCKED   ***  ");
    lcd.setCursor(0, 1); lcd.print(sysState == EMERGENCY_STOP ? "E-STOP active!      "
                                                              : "System running OK   ");
    lcd.setCursor(0, 2); lcd.print("Hold UP + DOWN      ");
    lcd.setCursor(0, 3); lcd.print("together to unlock  ");
    return;
  }
  if (lastLocked) { lcd.clear(); lastLocked = false; }    // just unlocked -> force a clean redraw

  // WiFi provisioning portal banner: takes over the screen while the SoftAP is up (automation runs on).
  static bool lcdInPortal = false;
  if (portalActive) {
    if (!lcdInPortal) { lcd.clear(); lcdInPortal = true; }
    char lb[21];
    lcd.setCursor(0, 0); lcd.print("WiFi Setup Mode     ");
    snprintf(lb, 21, "AP:%-16s", AP_SSID);          lcd.setCursor(0, 1); lcd.print(lb);
    snprintf(lb, 21, "http://%-13s", portalApIp);   lcd.setCursor(0, 2); lcd.print(lb);
    lcd.setCursor(0, 3); lcd.print("BACK = cancel       ");
    return;
  }
  if (lcdInPortal) { lcd.clear(); lcdInPortal = false; }  // just left portal -> clean redraw

  // Service-run UI takes over: receipt -> live stages -> result -> errors. Released by UP+DOWN
  // (runLocked=false), which only frees the screen -- the run itself keeps going.
  static uint8_t lastRunPhase = 255;
  // Never mask a held fault / E-stop: those screens carry the recovery choices the operator must answer.
  if (runPhase != RUN_NONE && runLocked && !esp2Held && sysState != EMERGENCY_STOP) {
    if (lastRunPhase != (uint8_t)runPhase) { lcd.clear(); lastRunPhase = (uint8_t)runPhase; }
    // BACK opened the cancel confirm over the live run screen.
    if (cancelPrompt) {
      char cl[21];
      lcdRow(0, "CANCEL THIS RUN?");
      for (int r = 0; r < 3; r++) {
        snprintf(cl, 21, "%c%-19s", r == cancelSel ? '>' : ' ', CANCEL_NAMES[r]); lcdRow(r + 1, cl);
      }
      return;
    }
    lcdRenderRun();
    return;
  }
  // Drop an orphaned confirm: only the held-fault menu and the locked run screen service it, so if
  // neither is showing (run ended, screen released, fault resolved) nothing could dismiss it.
  if (cancelPrompt && !esp2Held && !(runPhase != RUN_NONE && runLocked)) cancelPrompt = false;
  if (lastRunPhase != 255) { lcd.clear(); lastRunPhase = 255; }   // left the run UI -> clean redraw

  // Settings / Testing UI takes over the screen when active (spec sec.18.10).
  static uint8_t lastUi = 255;
  if (lastUi != uiMode) { lcd.clear(); lastUi = uiMode; }
  if (uiMode == UI_CAL)  { lcdRenderCal();  return; }
  if (uiMode == UI_DIAG) { lcdRenderDiag(); return; }
  if (uiMode != UI_DATA) { lcdRenderSettings(); return; }

  static uint8_t lastPage = 255;
  if (lastPage != lcdPage) { lcd.clear(); lastPage = lcdPage; }

  char l[21];
  switch (lcdPage) {
    case PAGE_HOME:
      lcdRow(0, tsString().substring(0, 19).c_str());
      snprintf(l, 21, "State:%-13s", stateName(sysState)); lcdRow(1, l);
      snprintf(l, 21, "Bat:%.1fV %s", battV, batteryCritical ? "CRIT" : batteryLow ? "LOW" : "OK"); lcdRow(2, l);
      snprintf(l, 21, "Res:%d%% Mix:%d%%", (int)sensor.resLevel, (int)sensor.mixLevel); lcdRow(3, l);
      break;
    case PAGE_SENSORS:
      snprintf(l, 21, "T:%.1fC H:%.0f%%", sensor.temp, sensor.hum); lcdRow(0, l);
      snprintf(l, 21, "Soil A%d B%d C%d", sensor.soil[0], sensor.soil[1], sensor.soil[2]); lcdRow(1, l);
      snprintf(l, 21, "Light:%.0f", sensor.lux); lcdRow(2, l);
      snprintf(l, 21, "Flow:%.1f L/min", sensor.flow); lcdRow(3, l);
      break;
    case PAGE_COLUMNS:
      for (int c = 0; c < 3; c++) {
        snprintf(l, 21, "%c:%s %s", COL_TAG[c],
                 COLUMN_ENABLED[c] ? (col[c].mode == MODE_AUTO ? "AUTO" : "IRR ") : "OFF ",
                 strlen(col[c].name) ? col[c].name : "-");
        lcdRow(c, l);
      }
      snprintf(l, 21, "ESP2:%s", esp2Available ? "OK" : "--"); lcdRow(3, l);
      break;
    case PAGE_POWER:
      lcdRow(0, "Power (ADC)");
      snprintf(l, 21, "V:%.2f", battV); lcdRow(1, l);
      snprintf(l, 21, "I:%.2fA P:%.1fW", battI, battP); lcdRow(2, l);
      snprintf(l, 21, "SD:%s RTC:%s", sdOk ? "OK" : "--", rtcOk ? "OK" : "--"); lcdRow(3, l);
      break;
    case PAGE_GSM:   // GSM (SIM800L) + WiFi/ThingSpeak link health
      lcdRow(0, "GSM + WiFi");
      snprintf(l, 21, "GSM:%-4s Sig:%-3s Q%d",
               netRegistered ? "REG" : "NO",
               (lastRssi < 0 || lastRssi == 99) ? "--" : String(lastRssi).c_str(),
               smsCount);
      lcdRow(1, l);
      if (portalActive)       snprintf(l, 21, "WiFi:PORTAL %-8s", portalApIp);
      else if (!wifiEnabled)  snprintf(l, 21, "WiFi:OFF");
      else if (wifiConnected) snprintf(l, 21, "WiFi:OK %ddBm", wifiRssiVal);
      else                    snprintf(l, 21, "WiFi:DOWN");
      lcdRow(2, l);
      {
        // Row 3 carries both uplinks: ThingSpeak (graphs) and Firebase (live dashboard). Ages are
        // clamped to 999 s so a long outage can't overflow the 20-char row.
        unsigned long tsAge = lastTsUploadMs ? (millis() - lastTsUploadMs) / 1000 : 0;
        unsigned long fbAge = fbLastUploadMs ? (millis() - fbLastUploadMs) / 1000 : 0;
        if (tsAge > 999) tsAge = 999;
        if (fbAge > 999) fbAge = 999;
        const char *fbTag = (fbUrl.length() == 0 || !firebaseEnabled) ? "--" : (fbLastOk ? "ok" : "!!");
        snprintf(l, 21, "TS:%s%3lus FB:%s%3lus", lastTsOk ? "ok" : "--", tsAge, fbTag, fbAge);
        lcdRow(3, l);
      }
      break;
    default: // PAGE_FAULT  (doubles as the E-stop / fault-hold recovery prompt)
      if (esp2Held && cancelPrompt) {
        // Cancel confirm, over the recovery menu: three ways to stop, BACK returns.
        lcdRow(0, "CANCEL RUN?");
        for (int r = 0; r < 3; r++) {
          snprintf(l, 21, "%c%-19s", r == cancelSel ? '>' : ' ', CANCEL_NAMES[r]); lcdRow(r + 1, l);
        }
      } else if (esp2Held) {
        // ESP2 fault HELD: 5-way recovery in a 3-row window over rows 1..3 (sec.19.4.8.2).
        // After repeated flow re-holds, the header steers to Release (the flow-independent escape).
        if (steerRelease) lcdRow(0, "FLOWx3: RELEASE/Cxl");
        else { snprintf(l, 21, "HELD:%-15s", lastFaultMsg.substring(0, 15).c_str()); lcdRow(0, l); }
        // Window follows the cursor across all RECOV_N entries (was hard-capped at 4).
        int top = faultRecovSel - 1;
        if (top < 0) top = 0;
        if (top > RECOV_N - 3) top = RECOV_N - 3;
        for (int r = 0; r < 3; r++) {
          int idx = top + r;
          snprintf(l, 21, "%c%-19s", idx == faultRecovSel ? '>' : ' ', RECOV_NAMES[idx]); lcdRow(r + 1, l);
        }
      } else if (sysState == EMERGENCY_STOP) {
        lcdRow(0, "** EMERGENCY OFF  **");
        snprintf(l, 21, "%cReturn to normal", estopSel == 0 ? '>' : ' '); lcdRow(1, l);
        snprintf(l, 21, "%cStay stopped", estopSel == 1 ? '>' : ' '); lcdRow(2, l);
        lcdRow(3, "UP/DN pick  ENT=ok");
      } else {
        lcdRow(0, "Last Fault:");
        snprintf(l, 21, "%-20s", lastFaultMsg.substring(0, 20).c_str()); lcdRow(1, l);
        snprintf(l, 21, "C%d M%d m%d today", faultsToday[0], faultsToday[1], faultsToday[2]); lcdRow(2, l);
        if (lastFaultTime.length()) { snprintf(l, 21, "at %-17s", lastFaultTime.substring(5).c_str()); lcdRow(3, l); }  // MM-DD HH:MM:SS
        else                        { lcdRow(3, ""); }
      }
      break;
  }
}

/* =============================================================================
 *  SETTINGS / TESTING RENDERING  (20x4; cleared on uiMode change by lcdTick)
 * ========================================================================== */
// Helper: draw a 3-row scrolling list window (rows 1..3) with a '>' cursor.
static void drawList(int sel, int count, const char *const *names) {
  int top = sel - 1; if (top < 0) top = 0;
  if (top > count - 3) top = (count > 3) ? count - 3 : 0;
  char l[21];
  for (int r = 0; r < 3; r++) {
    int idx = top + r;
    lcd.setCursor(0, r + 1);
    if (idx < count) snprintf(l, 21, "%c%-19s", idx == sel ? '>' : ' ', names[idx]);
    else             snprintf(l, 21, "%-20s", "");
    lcd.print(l);
  }
}

// Settings-row label with a live [ON]/[OFF] suffix for the two WiFi toggles.
static String setRowLabel(int i) {
  if (i == SET_WIFI)   return String("WiFi        ") + (wifiEnabled ? "[ON]" : "[OFF]");
  if (i == SET_SOFTAP) return String("Setup AP    ") + ((portalActive || portalRequested) ? "[ON]" : "[OFF]");
  return SET_NAMES[i];
}

void lcdRenderSettings() {
  char l[21];

  // ---- Restore-Defaults double-confirm (companion spec §C.3) ----
  if (restoreConfirm) {
    lcd.setCursor(0, 0); lcd.print("Restore defaults?   ");
    lcd.setCursor(0, 1); lcd.print("Keeps cal+col setup ");
    lcd.setCursor(0, 2); snprintf(l, 21, "%cNO                 ", restoreSel == 0 ? '>' : ' '); lcd.print(l);
    lcd.setCursor(0, 3); snprintf(l, 21, "%cYES, RESET         ", restoreSel == 1 ? '>' : ' '); lcd.print(l);
    return;
  }
  // ---- Reboot ESP1 double-confirm ----
  if (resetConfirm) {
    lcd.setCursor(0, 0); lcd.print("Reboot controller?  ");
    lcd.setCursor(0, 1); lcd.print("Config is kept      ");
    lcd.setCursor(0, 2); snprintf(l, 21, "%cNO                 ", resetSel == 0 ? '>' : ' '); lcd.print(l);
    lcd.setCursor(0, 3); snprintf(l, 21, "%cYES, REBOOT        ", resetSel == 1 ? '>' : ' '); lcd.print(l);
    return;
  }
  // ---- Edit "unsaved changes" three-way dialog (companion spec §B.3) ----
  if (editConfirm) {
    lcd.setCursor(0, 0); lcd.print("Unsaved changes     ");
    lcd.setCursor(0, 1); snprintf(l, 21, "%cSAVE               ", confirmSel == 0 ? '>' : ' '); lcd.print(l);
    lcd.setCursor(0, 2); snprintf(l, 21, "%cDISCARD            ", confirmSel == 1 ? '>' : ' '); lcd.print(l);
    lcd.setCursor(0, 3); snprintf(l, 21, "%cCANCEL             ", confirmSel == 2 ? '>' : ' '); lcd.print(l);
    return;
  }

  if (uiMode == UI_MENU) {
    lcd.setCursor(0, 0); lcd.print("=== SETTINGS ===    ");
    // Like drawList, but the two WiFi rows carry a live [ON]/[OFF] suffix.
    int top = setSel - 1; if (top < 0) top = 0;
    if (top > SET_COUNT - 3) top = (SET_COUNT > 3) ? SET_COUNT - 3 : 0;
    for (int r = 0; r < 3; r++) {
      int idx = top + r;
      lcd.setCursor(0, r + 1);
      if (idx < SET_COUNT) snprintf(l, 21, "%c%-19s", idx == setSel ? '>' : ' ', setRowLabel(idx).c_str());
      else                 snprintf(l, 21, "%-20s", "");
      lcd.print(l);
    }
    return;
  }

  if (uiMode == UI_TEST) {
    // Header reflects the REAL arming state, not just the heartbeat:
    //  ESP2 DOWN   = no heartbeat (ESP2 off / link both-ways or RX dead)
    //  ARMING...   = ESP2 alive, waiting for it to confirm TEST mode (priming/retrying)
    //  NO ACK!     = ESP2 alive but not ACKing TEST,ENTER after several tries (suspect ESP1->ESP2 TX)
    //  (dead-man)  = confirmed in TEST mode, relays controllable
    const char *hdr = !esp2Available     ? "TESTING: ESP2 DOWN  "
                    : esp2TestArmed       ? "TESTING (dead-man)  "
                    : (testArmTries >= TEST_ARM_NOACK_HINT) ? "TEST NO ACK-chk link"
                    :                        "TESTING: ARMING...  ";
    lcd.setCursor(0, 0); lcd.print(hdr);
    int top = testSel - 1; if (top < 0) top = 0; if (top > TEST_ROWS - 2) top = TEST_ROWS - 2;
    for (int r = 0; r < 2; r++) {
      int idx = top + r;
      lcd.setCursor(0, r + 1);
      if (idx < TEST_ROWS && testRowVisible(idx))
        snprintf(l, 21, "%c%-11s %s", idx == testSel ? '>' : ' ', testRowName(idx).c_str(),
                 idx == testOnBit ? "[ON]" : "[ -]");
      else snprintf(l, 21, "                    ");
      lcd.print(l);
    }
    lcd.setCursor(0, 3); lcd.print("HOLD ENT=on BACK=ext");
    return;
  }

  // UI_EDIT
  switch (editItem) {
    case SET_CLOCK:
      lcd.setCursor(0, 0); lcd.print("Set Clock           ");
      lcd.setCursor(0, 1);
      snprintf(l, 21, "%c%04d-%c%02d-%c%02d      ",
               editField==0?'>':' ', editTmp[0], editField==1?'>':' ', editTmp[1],
               editField==2?'>':' ', editTmp[2]);
      lcd.print(l);
      lcd.setCursor(0, 2);
      snprintf(l, 21, "%c%02d:%c%02d            ",
               editField==3?'>':' ', editTmp[3], editField==4?'>':' ', editTmp[4]);
      lcd.print(l);
      break;
    case SET_SCHEDULE:
      lcd.setCursor(0, 0); snprintf(l, 21, "Schedule            "); lcd.print(l);
      lcd.setCursor(0, 1);
      snprintf(l, 21, "%cCol %c   %cSched:%-4s ",
               editField==0?'>':' ', COL_TAG[editCol],
               editField==1?'>':' ', editTmp[0]==SCHED_AUTO ? "AUTO" : "MAN ");
      lcd.print(l);
      lcd.setCursor(0, 2);
      if (editTmp[0] == SCHED_MANUAL)
        snprintf(l, 21, "%cSt%02d:%02d %cEn%02d:%02d  ",
                 (editField==2||editField==3)?'>':' ', editTmp[1], editTmp[2],
                 (editField==4||editField==5)?'>':' ', editTmp[3], editTmp[4]);
      else
        snprintf(l, 21, "(AUTO uses default) ");
      lcd.print(l);
      break;
    case SET_COLMODE:
      lcd.setCursor(0, 0); lcd.print("Column Mode         ");
      lcd.setCursor(0, 1);
      snprintf(l, 21, "%cColumn: %c          ", editField==0?'>':' ', COL_TAG[editCol]);
      lcd.print(l);
      lcd.setCursor(0, 2);
      snprintf(l, 21, "%cMode: %s ", editField==1?'>':' ',
               editTmp[0]==0 ? "AUTO           " : editTmp[0]==1 ? "IRRIGATION_ONLY" : "OFF            ");
      lcd.print(l);
      break;
    case SET_PRESET:
      lcd.setCursor(0, 0);
      snprintf(l, 21, "%cPreset COL_%c %c%-6s",
               editField==0?'>':' ', COL_TAG[editCol],
               editField==1?'>':' ', editTmp[0]==0 ? "NAMED" : "MANUAL");
      lcd.print(l);
      if (editTmp[0] == 0) {                                    // NAMED
        lcd.setCursor(0, 1);
        snprintf(l, 21, "%c%-12s        ", editField==2?'>':' ', CROP_PRESETS[editTmp[1]].name);
        lcd.print(l);
        lcd.setCursor(0, 2);
        snprintf(l, 21, "N%dP%dK%d pH%.1f      ",
                 (int)CROP_PRESETS[editTmp[1]].N, (int)CROP_PRESETS[editTmp[1]].P,
                 (int)CROP_PRESETS[editTmp[1]].K, CROP_PRESETS[editTmp[1]].pH);
        lcd.print(l);
      } else {                                                 // MANUAL
        lcd.setCursor(0, 1);
        snprintf(l, 21, "%cN%3d %cP%3d %cK%3d  ",
                 editField==2?'>':' ', editTmp[2], editField==3?'>':' ', editTmp[3],
                 editField==4?'>':' ', editTmp[4]);
        lcd.print(l);
        lcd.setCursor(0, 2);
        snprintf(l, 21, "%cpH %.1f             ", editField==5?'>':' ', editTmp[5] / 10.0f);
        lcd.print(l);
      }
      break;
    case SET_THRESH:
      lcd.setCursor(0, 0); lcd.print("Thresholds          ");
      lcd.setCursor(0, 1);
      snprintf(l, 21, "%cStart<%3d%% %cStop>%3d%%", editField==0?'>':' ', editTmp[0], editField==1?'>':' ', editTmp[1]);
      lcd.print(l);
      lcd.setCursor(0, 2);
      snprintf(l, 21, "%cFert gap %3d mg/kg  ", editField==2?'>':' ', editTmp[2]); lcd.print(l);
      break;
  }
  lcd.setCursor(0, 3); lcd.print("UP/DN edit ENT next ");
}

/* =============================================================================
 *  LOGGING  (buffered CSV to microSD, spec sec.25)
 * ========================================================================== */
void logEvent(const char *source, const char *type, const String &detail) {
  // The schema is timestamp,source,event_type,detail with '|' as the secondary delimiter and NO
  // commas inside detail (sec.25.2 / CLAUDE.md). Several callers pass through raw framed payloads or
  // SMS text that legitimately contain commas -- GARBAGE RAW=, ESP2 RESP, ESP1 CMD, GSM TX -- which
  // silently split into extra CSV columns (~10% of rows). Our own summary parser tolerates it
  // (substring past the 3rd comma), but any external reader of the thesis CSV does not.
  String d = detail;
  d.replace(',', ';');
  String row = tsString() + "," + source + "," + type + "," + d + "\n";
  logBuf += row;
  logLineCount++;
  Serial.print(row);                          // mirror to USB debug
  bool critical = (strcmp(type, "FAULT") == 0 && detail.startsWith("CRIT"));
  if (logLineCount >= LOG_FLUSH_LINES || critical) logFlush(critical);
  // The flush above can legitimately decline (sdMux held by a core-0 portal download) and leave the
  // buffer intact. Cap it so that can never become unbounded heap growth, and leave a marker so the
  // gap is visible in the CSV instead of silently missing rows.
  if (logBuf.length() > LOG_BUF_MAX) {
    logBuf = tsString() + ",ESP1,LOG,OVERFLOW|buffer_dropped\n";
    logLineCount = 1;
  }
}

void logFlush(bool force) {
  if (logBuf.length() == 0) return;
  if (!force && millis() - lastLogFlushMs < LOG_FLUSH_INTERVAL_MS && logLineCount < LOG_FLUSH_LINES) return;
  lastLogFlushMs = millis();
  if (!sdOk) { logBuf = ""; logLineCount = 0; return; }   // degraded: drop (mirrored to Serial)

  char fname[16];
  if (rtcOk && currentDayStamp > 0) snprintf(fname, sizeof(fname), "/%08ld.CSV", currentDayStamp);
  else { strcpy(fname, "/NODATE.CSV"); }

  // Serialize SD access against the core-0 portal (SD admin). If the portal holds sdMux, don't block
  // the core-1 loop -- keep buffering and flush on a later tick (logBuf tolerates this like !sdOk).
  if (sdMux && xSemaphoreTake(sdMux, pdMS_TO_TICKS(100)) != pdTRUE) return;

  File f = SD.open(fname, FILE_APPEND);
  if (f) { f.print(logBuf); f.close(); logBuf = ""; logLineCount = 0; }
  else {
    // Write failed: try ONE re-init. If the card is really gone, mark it lost so
    // deviceHealthTick (Part C) sees the present->absent transition.
    if (!SD.begin(SD_CS)) { sdOk = false; Serial.println(F("WARN: microSD lost (write failed, re-init failed)")); }
    logBuf = ""; logLineCount = 0;            // avoid unbounded growth on write failure
  }
  if (sdMux) xSemaphoreGive(sdMux);
}
