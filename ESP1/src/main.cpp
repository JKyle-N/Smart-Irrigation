/* =============================================================================
 *  SMART IRRIGATION & FERTIGATION SYSTEM  --  ESP32 #1  MASTER CONTROLLER
 *  Controller 2 of 3   (Nano  ->  ESP32 #1  ->  ESP32 #2)
 * -----------------------------------------------------------------------------
 *  ROLE (spec sec.10.1 / CLAUDE.md): SINGLE DECISION-MAKING AUTHORITY.
 *    - Ingests framed sensor packets from the Nano (sec.9.9).
 *    - Decides per-column irrigation vs. fertigation (windowed schedule + soil
 *      threshold + NPK gap), and hands COMPLETE work orders to ESP32 #2
 *      (supervisor / work-order model, sec.9.8.1.1) -- then monitors.
 *    - Talks to the user over A7670C SMS (alerts, run notices, daily report).
 *    - Logs every event to microSD (CSV, RTC-stamped, sec.25).
 *    - Monitors solar and the main battery via two INA226 behind an ISO1540 (opto ADC + ACS758 as the
 *      main battery's fallback; display/log/telemetry only -- no battery-triggered stops/alerts) and
 *      the secondary battery via its own INA226; classifies OTHER faults (3 tiers, sec.23).
 *    - Drives the LCD UI; owns all recovery escalation (sec.18.9).
 *
 *  AUTHORITY BOUNDARIES: ESP32 #1 decides; ESP32 #2 executes; Nano senses.
 *  This file implements ESP32 #1's side of every protocol. ESP32 #2 (controller
 *  3) is built last, so the ESP2-facing work-order path is implemented to spec
 *  here but can only be end-to-end tested once ESP32 #2 exists.
 *
 *  NON-BLOCKING: no delay() in loop(); the A7670C driver (include/gsm_modem.h) brings
 *  the modem up from loop() and keeps retrying. Task watchdog petted each loop iteration.
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
#include "pure_math.h"           // soil/level/pulse maths -- host-testable, see ESP1/test/
#include "gsm_modem.h"           // A7670C SMS driver -- host-tested against a simulated modem
#include "run_plan.h"            // a column's Operation (Automatic / Manual) and its day plan (2026-11-18)
#include "../../shared/rig_constants.h"   // WATER_BUDGET_L, MIXING_TANK_MAX_L, MIX_HIGH_PCT: shared with ESP2
// A millis() stamp that is never 0 (0 = "not set") and never in the future (2026-11-17). The old `t | 1` made every even
// stamp 1 ms ahead of the clock, so `now - stamp` taken in the same millisecond wrapped to ~49 days and a timeout fired
// at once -- e.g. LEVEL_BLIND|NO_SETTLED_LEVEL the instant a forced run started.
static inline unsigned long nzMs(unsigned long t) { return t ? t : 1UL; }
#include "../../shared/tank_volume.h"     // ultrasonic cm -> litres (frustum + calibration points), web v.4
#include "../../shared/pump_line.h"       // a pump's litres from its run time (LCD "... pump est", 2026-11-17)
#if __has_include("secrets.h")            // owner number, AP password, admin PIN: git-ignored, this repo is public
#include "secrets.h"
#else
#include "secrets.example.h"              // placeholders, so CI and a fresh clone still build
#endif
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
#define SIM_RX_PIN    26        // HW UART2 RX  (<- A7670C TX)
#define SIM_TX_PIN    27        // HW UART2 TX  (-> A7670C RX)
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
#define PIN_BATT_I    34        // old opto current channel: read 0.00 A on every sample, no longer used
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
// The A7670C's baud is auto-detected (GSM_BAUDS in gsm_modem.h, 115200 first).
#define SIM_RX_BUFFER  2048     // ~180 ms of 115200 traffic; the default 256 B is only ~22 ms of loop stall
// Nano link (EspSoftwareSerial). 2026-11-16 hardening: 128 -> 512 B, and the bit-edge buffer the receive interrupt
// fills is sized explicitly. The ISR timestamps every edge; the bytes are only decoded when the loop next polls, so a
// loop pass stretched by an SD write, an LCD redraw or an NVS write used to overflow the 1280-edge default (~0.2 s of
// traffic) and corrupt lines -- the rig logs showed 11-28 % of Nano lines failing their checksum. 5120 edges = ~0.6 s
// even for the busiest bytes (~20 KB of RAM; there is plenty). Overflows are counted (nanoRxOverflows).
#define NANO_RX_BUFFER      512
#define NANO_RX_ISR_BUFFER  (NANO_RX_BUFFER * 10)

/* ---- I2C addresses ------------------------------------------------------- */
#define LCD_ADDR     0x27
#define INA226_ADDR  0x40       // secondary battery, on this bus directly
#define SOLAR_INA_ADDR 0x41     // solar, behind the ISO1540 (A1 GND, A0 VCC)
#define MAIN_INA_ADDR  0x44     // main battery, behind the ISO1540 (A1 VCC, A0 GND)
// DS3231 0x68 and EEPROM 0x57 are auto-addressed by their libraries / unused.

/* ---- GSM ----------------------------------------------------------------- */
String PHONE_NUMBER = SECRET_OWNER_NUMBER;   // [CONFIRM] recipient for alerts/reports (secrets.h)
String ALERT_NUMBER2 = "";              // second recipient, ALERT texts only and no command rights (SMS ALERTNUM, NVS)
String USSD_CODE     = "";              // carrier balance code for BAL / the weekly check (SMS USSD, NVS); carrier-specific
const uint8_t SIM_CHECK_WEEKDAY = 0;    // weekly SIMTEST text goes with this day's report (0 = Sunday)
const unsigned long SMS_AUTH_REPLY_GAP_MS = 600000UL;   // at most one ERR,AUTH reply per 10 min

/* ---- Columns (spec sec.14.1.4.1) ----------------------------------------- */
#define NUM_COLUMNS 3
// Runtime + NVS (Column Mode OFF/ON on the LCD flips this, sec.18.10.7). NOTE: this
// is ESP1-SIDE ONLY -- the Nano keeps its own compiled COLUMN_ENABLED, so toggling a
// column here does not change which columns the Nano physically reads (sec.18.10.7.4).
bool COLUMN_ENABLED[NUM_COLUMNS] = { true, true, false };   // A, B, C [CONFIRM]
const char COL_TAG[NUM_COLUMNS] = { 'A', 'B', 'C' };

/* ---- Irrigation thresholds & windows (spec sec.14.1.3, windowed) --------- *
 * Windowed scheduling: a column is serviced only inside its RTC window, and
 * within the window a soil reading below soilStartPct triggers the run. A run delivers a metered
 * volume and then ends, so there is no stop threshold (decision D11).          */
// Editable at commissioning AND on-device via Settings menu (persisted to NVS).
int soilStartPct = 35;   // start irrigation below this %   [TBD]
// 18 Nov 2026 (the owner): an Overwatered line, and both lines per column. soilWetPct is the default "Too wet" line,
// a warning only (pure_math.h: wetEffPct keeps it above soil start). A column's own value, -1 = follow the default;
// set from the website's Edit settings (SET_COLUMN soilStart / soilWet), kept in NVS c<n>ss / c<n>sw.
int    soilWetPct = SOIL_WET_DEFAULT_PCT;
int8_t colSoilStart[NUM_COLUMNS] = { -1, -1, -1 };
int8_t colSoilWet[NUM_COLUMNS]   = { -1, -1, -1 };
static int colStartPct(int c) { return effPct(colSoilStart[c], soilStartPct); }
static int colWetPct(int c)   { return wetEffPct(effPct(colSoilWet[c], soilWetPct), colStartPct(c)); }
/* ---- NPK 7-in-1 trust thresholds ----------------------------------------- *
 * The probe reads moisture and N/P/K unreliably in unsaturated soil, so neither is trusted blindly:
 *  AGREE  how far its moisture may sit from the capacitive pair before it is dropped from the
 *         column average (percentage points).
 *  MIN    below this column moisture its N/P/K are not usable, so fertigation downgrades to
 *         irrigation-only until the soil is wet enough to read.
 * MIN must stay BELOW soilStartPct: irrigation only starts below 35 %, so fertigation is always
 * decided on dry-ish soil, and a floor at or above that would disable fertigation permanently.  */
// NPK_MOIST_AGREE_PCT and NPK_MIN_MOIST_PCT now live in include/pure_math.h, beside the soil
// blending that enforces them, so the host tests exercise the same numbers the firmware runs.
// DEFAULT (AUTO) per-column service window, minutes since midnight {start, end}  [TBD]
const uint16_t DEF_WIN_START[NUM_COLUMNS] = { 6*60, 8*60, 6*60 };   // 06:00 / 08:00 / 06:00
const uint16_t DEF_WIN_END[NUM_COLUMNS]   = { 8*60, 10*60, 8*60 };  // 08:00 / 10:00 / 08:00
// MANUAL (operator-set) window, used when colSchedMode==SCHED_MANUAL (sec.18.10.7.2).
uint16_t COL_WIN_START[NUM_COLUMNS] = { 6*60, 8*60, 6*60 };
uint16_t COL_WIN_END[NUM_COLUMNS]   = { 8*60, 10*60, 8*60 };
// Schedule axis per column: AUTO = use DEF_* window; MANUAL = use COL_WIN_* (perpetual).
enum SchedMode { SCHED_AUTO = 0, SCHED_MANUAL = 1 };
uint8_t colSchedMode[NUM_COLUMNS] = { SCHED_AUTO, SCHED_AUTO, SCHED_AUTO };
// Several waterings a day (2026-11-05, asked for by the project owner): on MANUAL a column may have up to
// MAX_RUNS_PER_DAY windows. Window 1 is COL_WIN_START/END above, unchanged; windows 2.. are COL_EXTRA_*.
// The column is watered at most once in each window, still only while its soil is drier than soilStartPct,
// and nutrients go in at most once a day (col[].fertStamp). AUTO always has the one default window.
// Windows are kept in order and apart (end of one < start of the next) by every path that sets them.
const uint8_t MAX_RUNS_PER_DAY = 4;
uint8_t  colRuns[NUM_COLUMNS] = { 1, 1, 1 };                     // MANUAL windows in use, 1..MAX_RUNS_PER_DAY
uint16_t COL_EXTRA_START[NUM_COLUMNS][MAX_RUNS_PER_DAY - 1] = { { 12*60, 16*60, 18*60 }, { 12*60, 16*60, 18*60 }, { 12*60, 16*60, 18*60 } };
uint16_t COL_EXTRA_END[NUM_COLUMNS][MAX_RUNS_PER_DAY - 1]   = { { 13*60, 17*60, 19*60 }, { 13*60, 17*60, 19*60 }, { 13*60, 17*60, 19*60 } };

/* ---- Fertigation decision (spec sec.14.2.0) ------------------------------ */
float fertGap = 30.0f;                        // mg/kg below target -> fertigate (FERTIGATE_TRIGGER_GAP, §6; NVS) [TBD]

/* ---- AC supply window for ESP2's PZEM check (2026-11-14) ------------------ *
 * Whole volts; LCD Settings > AC Voltage, website System tab (SET_AC_VOLT); NVS "acvlo"/"acvhi". ESP1 is
 * the owner and sends it to ESP2 as SET_CAL,PZEM_V on every ESP2 power-up and on every change (see
 * setAcVoltWindow / pushAcVoltToEsp2). Defaults and the accepted range live in pure_math.h.            */
int acVMin = AC_V_DEFAULT_LOW, acVMax = AC_V_DEFAULT_HIGH;
// AC pump current limits, tenths of an amp (LCD Settings > AC Current, website System tab (SET_AC_AMP, 2026-11-16);
// NVS "acalo"/"acahi"; sent to ESP2 as SET_CAL,PZEM_I with the voltage window). Defaults and the accepted range
// live in pure_math.h.
int acAMin10 = AC_A_DEFAULT_MIN, acAMax10 = AC_A_DEFAULT_MAX;
float acLastI = -1.0f;                        // last AC pump current ESP2 measured (TELE / a current trip)
unsigned long acLastIMs = 0;                  // when acLastI was measured (0 = none since ESP1 started)
// The last AC voltage ESP2 measured (TELE during a run, or the reading in a VOLTAGE fault), shown next to
// the setting on the LCD and the website so the window is chosen from a real reading. -1 = none yet.
float acLastV = -1.0f;
unsigned long acLastMs = 0;
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
// Per-PROBE endpoints (2026-11-16), [column][probe 0 = "1", 1 = "2"], probe n = the Nano's SOIL_PINS[c][n]
// (A1 = pin A0, A2 = A1, B1 = A2, B2 = A3, C1 = A6, C2 = A7). These are what the moisture % uses
// (soilCombineProbes). Captured per probe on the LCD: Settings > Calibration > Soil A1 / A2 / B1 / B2 / ..,
// saved as NVS "s<c>p<n>d" / "s<c>p<n>w" (n = 1 or 2). A probe never calibrated that way starts from its
// column's endpoints above (or CalStore's s<c>a2/s<c>w2), so behaviour is unchanged until it is.
int   calProbeDry[NUM_COLUMNS][2] = { { 656, 656 }, { 707, 707 }, { 800, 800 } };   // = the column pairs until
int   calProbeWet[NUM_COLUMNS][2] = { { 524, 524 }, { 391, 391 }, { 300, 300 } };   //   loadCal() runs
// LCD capture: the probe's live raw must sit within SOIL_CAL_STEADY_ADC over the last SOIL_CAL_WIN samples
// (~0.6 s apart from the Nano) to show STEADY; ENTER locks the window's mean.              [MEASURE]
const int SOIL_CAL_WIN        = 8;
const int SOIL_CAL_MIN_SAMPLES = 5;
const int SOIL_CAL_STEADY_ADC  = 4;
int  soilCalRing[SOIL_CAL_WIN];
int  soilCalN = 0, soilCalIdx = 0;
void soilCalReset() { soilCalN = 0; soilCalIdx = 0; }
void soilCalPush(int v) {
  soilCalRing[soilCalIdx] = v;
  soilCalIdx = (soilCalIdx + 1) % SOIL_CAL_WIN;
  if (soilCalN < SOIL_CAL_WIN) soilCalN++;
}
// The window's mean (what ENTER locks), its spread, and whether it is steady. False with no samples yet.
bool soilCalStats(int *mean, int *lo, int *hi, bool *steady) {
  if (soilCalN == 0) return false;
  long sum = 0; int mn = 1 << 14, mx = -1;
  for (int i = 0; i < soilCalN; i++) { int v = soilCalRing[i]; sum += v; if (v < mn) mn = v; if (v > mx) mx = v; }
  *mean = (int)((sum + soilCalN / 2) / soilCalN); *lo = mn; *hi = mx;
  *steady = soilCalSteady(soilCalN, SOIL_CAL_MIN_SAMPLES, mn, mx, SOIL_CAL_STEADY_ADC);
  return true;
}
// SOIL_ADC_RAIL_LO/HI live in include/pure_math.h beside soilCombineCal(), which applies them.
// Ultrasonic geometry, sensor-to-water distance. Measured 2026-11-15 (user's calibration run), the SAME
// for both tanks: 42.9 cm = empty (0 %), 24.0 cm = full (100 %). An 18.9 cm span, so 1 cm ~= 5.3 %;
// on the mixing tank the 90 % fill stop is at ~25.9 cm and ESP1's 97 % backstop at ~24.6 cm.
// (Before: reservoir 38/11 and mixing 50/4 -- the mixing pair was never measured.)
// The Nano only sends raw cm; this conversion lives here on ESP1, not on the Nano.
// loadCal() reads the `calib` NVS keys reCm2/rfCm2/meCm2/mfCm2 over these. The keys were BUMPED from
// reCm/rfCm/meCm/mfCm (same reason as the soil "a2"/"w2" keys): an older on-device value no longer
// overrides these measured defaults after flashing. Calibration > "Ultra ... empty" writes the new keys.
// Tape check (user, 2026-11-18): the sensor head is 47.5 cm above the inside floor, so 42.9 cm is 4.6 cm of water
// (~4.3 L) above the floor and 24.0 cm is 23.5 cm of water. That is NOT a sensor offset: on 17 Nov the reservoir,
// pumped dry, read 47.1-47.6 cm. 0 % is where deliveries leave the mixing tank (it read 42.8-43.2 cm after every
// delivery), so the marks stay. 100 % cannot move up to the tank's 35.8 cm body line: water there is 11.7 cm from
// the sensor, inside the waterproof sensor's ~20 cm dead zone (no reading under 19.8 cm all day on 17 Nov), and the
// 90 % stop / 97 % backstop would go blind. Raise the sensors >= 12 cm first if 100 % must be the body line.
float calResEmptyCm = 42.9f, calResFullCm = 24.0f;     // ultrasonic geometry (moved from Nano) [MEASURED]
float calMixEmptyCm = 42.9f, calMixFullCm = 24.0f;     //                                        [MEASURED]
const float CAL_LEVEL_MIN_SPAN_CM = 2.0f;               // empty must sit this far beyond full, or it is ignored

/* ---- Tank litres (web v.4 "pure ultrasonic", shared/tank_volume.h) ------------------------------------ *
 * The ResMix / MixIrr flow meters are gone: ESP2 meters every transfer and delivery on the mixing-tank level
 * in LITRES, which this board works out from the raw cm and sends with each LEVEL line. Each tank is a
 * truncated cone, corrected by the operator's own "level -> litres" points (Calibration > "Tank Mix liters" /
 * "Tank Res liters", up to 8 each, NVS blobs "tvM" / "tvR"). The shape is the same for both tanks.
 * SHAPE (tape, user, 2026-11-18; inner circumference, wall already deducted): 108.7 cm at the floor, 117.4 cm at
 * 22.6 cm, 122.4 cm at 35.8 cm -> 34.60 / 37.37 / 38.96 cm across, a straight taper of 0.1219 cm per cm (the middle
 * point sits 0.1 mm off the line). Above 35.8 cm it flares to 41.7 cm at the rim (46.0 cm deep inside); no water
 * goes there. tankVolSyncGeom() turns this into the diameters at the 0 % and 100 % marks, so a re-captured EMPTY
 * keeps the litres right. (Was a guessed 31.0 / 39.4 cm at the marks: ~20 % too few litres near 0 %.)       */
const float TANK_SENSOR_H_CM = 47.5f;                  // sensor head above the inside floor            [MEASURED]
const float TANK_D_FLOOR_CM  = 34.60f;                 // inner diameter at the floor (108.7 cm around)  [MEASURED]
const float TANK_TAPER_CM    = 0.1219f;                // inner diameter gained per cm of height          [MEASURED]
// The mixer standing on the mixing tank's bottom (user, 2026-11-17): 6 cm across, 11.4 cm tall = 322.3 cm^3 the water
// cannot fill. tank_volume.h takes it out of the shape; the ultrasonic is not in its way.
const float MIXER_DIAM_CM = 6.0f, MIXER_H_CM = 11.4f;   //                                            [MEASURED]
// Inner diameter at the water level a sensor distance means (the floor diameter for a distance past the floor).
static float tankDiamAtCm(float distCm) {
  float h = TANK_SENSOR_H_CM - distCm;
  return TANK_D_FLOOR_CM + TANK_TAPER_CM * (h > 0.0f ? h : 0.0f);
}
// tank_volume.h stands the insert on the 0 % mark; the part of the mixer below that mark holds no counted water.
static float mixerHAboveEmpty(float emptyCm) {
  float h = MIXER_H_CM - (TANK_SENSOR_H_CM - emptyCm);
  return h > 0.0f ? h : 0.0f;
}
TankVolCal calMixVol = { 42.9f, 24.0f, tankDiamAtCm(42.9f), tankDiamAtCm(24.0f), 0, {0}, {0}, MIXER_DIAM_CM,
                         mixerHAboveEmpty(42.9f) };
TankVolCal calResVol = { 42.9f, 24.0f, tankDiamAtCm(42.9f), tankDiamAtCm(24.0f), 0, {0}, {0} };
// Hardening of the reading itself (the Nano sends the MEDIAN of its last 5 pings and their spread):
// a median whose pings spread more than this is a splash / ripple / multipath, not a level -- it is shown but
// NOT used for control (sent to ESP2 as -1, which ESP2 treats as "no reading").                      [MEASURE]
// 2.0 -> 4.0 cm (2026-11-17): with the transfer pouring in, the mixing tank's surface ripples and its pings spread past
// 2 cm, so ESP2 got "no reading" (-1) for second after second and held forced runs as LEVEL_BLIND while the LCD (which
// shows every echo) read fine. ESP2 takes a 5-sample median with a jump gate on top of the Nano's own median.
const float ULTRA_MAX_SPREAD_CM = 4.0f;
// Physically possible distances for these tanks: closer than the sensor's dead zone, or far past the empty
// level, is an echo off something else.                                                              [MEASURE]
const float ULTRA_MIN_CM = 3.0f;
const float ULTRA_MAX_OVER_EMPTY_CM = 15.0f;
// Speed-of-sound temperature compensation (tvTempCorrectCm). The geometry above was measured near this air
// temperature; distances are scaled to it using the Nano's DHT22 reading. 0 = off.                 [CONFIRM]
#define ULTRA_TEMP_COMP 1
const float ULTRA_TEMP_REF_C = 30.0f;
// Recompute both tanks' geometry from the empty/full cm (they live in calResEmptyCm.. for the % maths). The
// calibration points belong to a sensor DISTANCE, so when the empty mark moves their heights move with it.
static void tankVolSyncGeom() {
  float dM = calMixEmptyCm - calMixVol.emptyCm, dR = calResEmptyCm - calResVol.emptyCm;
  for (uint8_t i = 0; i < calMixVol.n && i < TV_MAX_PTS; i++) calMixVol.hCm[i] += dM;
  for (uint8_t i = 0; i < calResVol.n && i < TV_MAX_PTS; i++) calResVol.hCm[i] += dR;
  calMixVol.emptyCm = calMixEmptyCm; calMixVol.fullCm = calMixFullCm;
  calResVol.emptyCm = calResEmptyCm; calResVol.fullCm = calResFullCm;
  // The measured shape, at wherever the marks now are (2026-11-18).
  calMixVol.dBottomCm = tankDiamAtCm(calMixEmptyCm); calMixVol.dTopCm = tankDiamAtCm(calMixFullCm);
  calResVol.dBottomCm = tankDiamAtCm(calResEmptyCm); calResVol.dTopCm = tankDiamAtCm(calResFullCm);
  calMixVol.insHCm = mixerHAboveEmpty(calMixEmptyCm);
}
// Litres the mixing tank holds at ESP2's overflow stop: the most a fill may plan to put in it (MIXCAP).
static float mixCapLiters() {
  float v = tvLitersAtPct(calMixVol, MIX_HIGH_PCT);
  if (!(v > 0.5f)) v = MIXING_TANK_MAX_L;
  if (v > MIXING_TANK_MAX_L) v = MIXING_TANK_MAX_L;
  return v;
}

float calFlowResScale = 1.0f;                          // reservoir flow correction factor
float calNpkScale[7] = { 10, 10, 1, 10, 1, 1, 1 };     // raw register -> engineering (moved from Nano)
float calNpkOff[NUM_COLUMNS][3] = { {0,0,0}, {0,0,0}, {0,0,0} };  // N/P/K offset trim (default 0, §A.4.3)
float calTempOff = 0, calHumOff = 0, calLuxOff = 0;    // env/light offset trims
// ESP2-owned (ESP1 holds authoritative copy, pushes to ESP2). web v.4: no ResMix / MixIrr K-factors any more.
float calKNut[3] = { 450, 450, 450 };
float calKNutD = 450, calKPhUp = 450, calKPhDn = 450;
float calPhM = 0.0036621f, calPhB = 0, calEcM = 0.0009766f, calEcB = 0;
float calAcs712Zero = 1.65f;
// Runtime: live raw stream + SET_CAL ack capture (block-until-ACK).
String lastSetCalAck = "";              // arg of the last ACK,SET_CAL,<id>
String calRxId = "";                    // sensor id of the last CAL sample received
float  calRxRaw = 0; bool calRxValid = false; unsigned long calRxMs = 0;

/* The soil maths itself lives in include/pure_math.h so it can be compiled and tested on a host PC
 * without an ESP32 -- see ESP1/test/test_pure_math.cpp. This wrapper is the only difference between
 * the firmware and the tested code: it looks up this column's calibration endpoints and hands them
 * over as plain arguments. levelPct() and pulseRowForTarget() were already pure and moved across
 * unchanged. Behaviour is identical; imap() reproduces Arduino map()'s integer truncation. */
static int soilCombine(int col, int v1, int v2, int npkPct, SoilBlend *why) {
  return soilCombineProbes(v1, v2, npkPct, calProbeDry[col][0], calProbeWet[col][0],
                           calProbeDry[col][1], calProbeWet[col][1], why);
}

/* ---- Tank levels (%) (spec sec.14.4) ------------------------------------- */
// Reservoir too low -> block new runs (+ RES_LOW alert). 2026-11-16: = its filter zone (RES_FILTER_PCT, 18 %,
// shared/rig_constants.h) -- its ultrasonic stops at ~15 % and cannot see the water left below. Was 15 %, which an
// empty reservoir sitting on its filter reads, so it never tripped.                                   [TBD]
const float RES_LOW_PCT      = RES_FILTER_PCT;
// The reservoir pause (18 Nov 2026, the owner: "adjust the Watering pauses if it falls below 18 %, so the pumps never
// run dry"): new runs and forced runs wait while the reservoir reads under it. RES_LOW_PCT is now its floor: it can be
// raised to RES_PAUSE_MAX_PCT (LCD Settings > Thresholds, website System tab: SET_THRESH thRes; NVS "rpause"), never
// set lower, because the ultrasonic can't see the water under the filter. ESP2's own empty check during a fill
// (RES_EMPTY, at RES_FILTER_PCT) is unchanged.
int resPausePct = (int)RES_LOW_PCT;
static float resPauseLevel() { return (float)clampi(resPausePct, (int)RES_LOW_PCT, RES_PAUSE_MAX_PCT); }
const float MIX_TARGET_PCT   = 70.0f;   // mixing tank fill target          [TBD]
const float MIX_MAX_PCT      = 95.0f;   // mixing tank overflow guard       [TBD]

/* ---- Battery (INA226) (spec sec.21.1) ------------------------------------ */
const float BATT_LOW_V       = 11.8f;   // disable fertigation below        [TBD]
const float BATT_CRIT_V      = 11.2f;   // stop all below                   [TBD]
const unsigned long BATT_FLAG_HOLD_MS = 60000;  // below the line this long before a flag sets, so an inverter
                                                // start's sag cannot flicker it (battFlagHeld)  [CONFIRM]

/* ---- SECONDARY battery: INA226 (R010 shunt) on this I2C bus at INA226_ADDR ------------------- *
 * A 4S Li-ion battery (16.8 V full) that powers ESP1, ESP2, the Nano, the sensors and the relay
 * coils while the inverter runs the AC motors. ESP2's P0 (the old transfer-pump relay; P6 until 2026-11-15) drives a DPDT
 * that breaks BOTH + and - from the main solar battery for that time, so no electrical link to
 * the inverter's battery remains and the sensor readings stay clean; idle, the boards run from
 * the main battery, which also charges the secondary. ESP1 owns the monitor and the policy:
 * while SECONDARY_BATT_ENABLED, no inverter use may START if the secondary is low or its INA226
 * is not answering (secBattBlocksAc), and both raise Major alerts. ESP2 holds the matching flag
 * (it drives the switch) -- set both together, and only once the hold-up stage that rides the
 * boards through the ~10-20 ms DPDT transfer is fitted. At 0 the INA226 is still read, logged
 * (PWR2) and published, but nothing is blocked or alerted.
 * Calibrated with Calibration/ESP1/INA226, whose results load from NVS over these defaults (loadCal). */
#define SECONDARY_BATT_ENABLED 1   // ON since 2026-11-16 (user: hold-up fitted). Must match ESP2.
/* Secondary battery as a BACKUP for a critically low main battery: READY, NOT ACTIVE. At 1: when main
 * stays below BATT_CRIT_V for BACKUP_TRIGGER_MS with nothing running, ESP1 texts a critical alert (it
 * does NOT use raiseFault('C'): that E-stops, and the GPIO4 cut would drop the boards straight back to
 * main), then has ESP2 hold the boards on the secondary. All inverter use is blocked from the alert
 * until main recovers. It returns to main once main holds BACKUP_MAIN_RECOVER_V for BACKUP_RECOVER_MS,
 * or when the pack reaches SEC_BATT_CRIT_V or stops being monitored. See backupTick. Needs
 * SECONDARY_BATT_ENABLED and a trustworthy main voltage (the main INA226 answering, or a calibrated
 * calBattV), since without one it never triggers. ESP2 holds the matching flag -- set both together.  */
#define SECONDARY_BACKUP_ENABLED 0
#if SECONDARY_BACKUP_ENABLED && !SECONDARY_BATT_ENABLED
#error "SECONDARY_BACKUP_ENABLED needs SECONDARY_BATT_ENABLED"
#endif
const float BACKUP_MAIN_RECOVER_V = 12.6f;              // main has recovered at or above this        [CONFIRM]
const unsigned long BACKUP_TRIGGER_MS  = 60000;         // main critical this long before acting      [CONFIRM]
const unsigned long BACKUP_RECOVER_MS  = 300000;        // main recovered this long before returning  [CONFIRM]
const unsigned long BACKUP_ALERT_WAIT_MS = 60000;       // longest wait for the text before switching anyway
const unsigned long BACKUP_SWITCH_TIMEOUT_MS = 30000;   // ESP2 boot + handshake + settle, with margin
const unsigned long BACKUP_RESEND_MS   = 3000;          // repeat BATTBK,ON|OFF until ESP2 confirms
const unsigned long BACKUP_RETRY_MS    = 60000;         // pause before trying again after a failed or lost switch
const float         SEC_NO_PACK_V      = 5.0f;          // below this it is a wiring fault, not a flat pack
const float    SEC_SHUNT_OHM        = 0.010f;    // R010 on the module
const float    SEC_CURRENT_LSB      = 0.00025f;  // 0.25 mA/bit -> 8.192 A full scale
const uint16_t SEC_INA_CAL          = 2048;      // 0.00512/(LSB*0.010 R); trim x (meter A/INA A) [MEASURE]
const float    SEC_CURRENT_OFFSET_A = 0.014f;    // no-load reading, subtracted                 [MEASURE]
const float    SEC_BUS_V_FACTOR     = 1.000f;    // meter V / INA V                             [MEASURE]
const bool     SEC_INVERT_CURRENT   = false;     // true if IN+/IN- are wired reversed
const uint16_t SEC_INA_CONFIG       = 0x4527;    // AVG16, 1.1 ms bus/shunt, continuous
const float SEC_BATT_FULL_V    = 16.8f;          // 4.20 V/cell -> 100 %                         [CONFIRM]
const float SEC_BATT_EMPTY_V   = 12.8f;          // 3.20 V/cell ->   0 %                         [CONFIRM]
// Inverter use is blocked at or below SEC_BLOCK_PCT and allowed again from SEC_UNBLOCK_PCT (user, 2026-11-17: "do not
// attempt it at 30 % and below"), judged on the pack's RESTING charge (secRestV) -- not on the raw voltage, which the
// charger lifts and the boards' load pulls down. (Was 13.6 V raw to block, 14.0 V to allow.)
const float SEC_BLOCK_PCT      = 30.0f;          // = 14.0 V resting                               [CONFIRM]
const float SEC_UNBLOCK_PCT    = 35.0f;          // = 14.2 V resting (hysteresis)
const float SEC_BATT_CRIT_V    = 12.8f;          // 3.20 V/cell: critical alert                  [CONFIRM]
const unsigned long SEC_READ_MS  = 2000;         // read cadence once found
const unsigned long SEC_RETRY_MS = 10000;        // re-probe cadence while missing

/* ---- SOLAR + MAIN BATTERY: two INA226 behind an ISO1540 I2C isolator -------------------------- *
 * An INA226 must share a ground with what it measures, while these boards must not share one with
 * the main battery (inverter noise, and the P0 DPDT breaks both + and -). So the two modules live on
 * the main-battery side of an ISO1540: GND = main negative, VCC = a 3.3 V supply on that side. They stay
 * readable through every inverter run and battery switch, and nothing but the isolator crosses over.
 * The main INA226 is the first choice for main battery V/I/P; if it stops answering, the opto ADC and
 * ACS758 take over by themselves. Neither module blocks a run or sends a text: they only measure.
 * Sign: + = main battery discharging, + = solar producing (swap with *_INVERT if the leads are reversed).
 * Main shunt: 50 A / 75 mV = 1.5 mOhm. The INA226 tops out at 81.92 mV, so it clips at 54.6 A, harmlessly. */
const float    SOLAR_SHUNT_OHM   = 0.010f;       // R010 on the module; clips at 8.19 A           [CONFIRM]
const float    SOLAR_CURRENT_LSB = 0.00025f;     // 0.25 mA/bit
const float    SOLAR_OFFSET_A    = 0.0f;         // no-load reading, subtracted                   [MEASURE]
const float    SOLAR_BUS_FACTOR  = 1.000f;       // meter V / INA V                               [MEASURE]
const bool     SOLAR_INVERT      = false;        // true if IN+/IN- are wired reversed
const float    MAIN_SHUNT_OHM    = 0.075f / 50.0f;  // shunt bar; the module's own R010 must be REMOVED [CONFIRM]
const float    MAIN_CURRENT_LSB  = 0.002f;       // 2 mA/bit
const float    MAIN_OFFSET_A     = 0.0f;         // no-load reading, subtracted                   [MEASURE]
const float    MAIN_BUS_FACTOR   = 1.000f;       // meter V / INA V                               [MEASURE]
const bool     MAIN_INVERT       = false;        // true if IN+/IN- are wired reversed
const unsigned long PWR_INA_READ_MS = 2000;      // read cadence once found; re-probe uses SEC_RETRY_MS
const unsigned long ESP2_BATT_RETURN_WAIT_MS = 4000;  // keep ESP2 powered for its secondary->main return
                                                      // (1 s delay + <=0.8 s handshake + 0.5 s settle + margin)
/* Battery-switch reset protection. A switch whose hold-up fails resets the controllers; each such
 * reset is a STRIKE. BSW_LATCH_STRIKES within BSW_WINDOW_S latches the switch fault, which blocks
 * every inverter use (secBattBlocksAc) until cleared from the LCD, SMS BATTSW,CLEAR, the dashboard,
 * or automatically BSW_WINDOW_S after the last strike. Bounded: at most BSW_LATCH_STRIKES resets per
 * window, so a hold-up that cannot carry the boards can never reset the rig in a loop.          */
const uint8_t       BSW_LATCH_STRIKES = 2;
const uint32_t      BSW_WINDOW_S      = 86400;      // 24 h (RTC time)
const unsigned long BSW_QUIET_MS      = 3000;       // SD/radio/GSM quiet window around one switch
const unsigned long BSW_PENDING_MAX_MS = 10000;     // an unconfirmed switch with no reboot = survived
// After a boot or a counted reset the hold-up is flat, and retrying at once would be sure to fail and
// burn the second strike. Time it needs to recharge -- measure it on the rig.             [MEASURE]
const unsigned long BSW_RECHARGE_MS    = 60000;
const unsigned long BSW_CUT_SURVIVE_MS = 5000;      // alive this long after a stop = the stop did not reset us

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
float ACS_DIV          = 2.0f;          // (R1+R2)/R2 in front of A0                    [MEASURE]
float ACS_ZERO_V       = 2.5f;          // sensor output at 0 A = VCC/2 (050B)          [MEASURE]
float ACS_SENS_V_PER_A = 0.040f;        // 40 mV/A = ACS758LCB-050B                     [CONFIRM]
                                        // Not const: NVS "calib" overrides these three (loadCal)
const uint8_t ACS_PGA        = 1;       // 1 = +/-4.096 V full scale (125 uV/LSB)
// ThingSpeak Ch3 ("Chem") slot for the battery current. Ch2 "System" is full (all 8 fields).
// Must stay ABOVE 2 x (enabled columns), since the EC/pH loop claims 2 fields per enabled column.
// Ch3 layout since 2026-11-15 (user: "sacrifice EC, replace with solar"):
//   1 solar current (A) | 2 pH col A | 3 solar energy today (Wh) | 4 pH col B | 5 battery current (A)
//   6 pH col C (if enabled) | 7 solar voltage (V) | 8 solar power (W)
// Rename fields 1 and 3 in the ThingSpeak channel settings; their old history is soil EC.
const int TS3_SOLAR_I_FIELD  = 1;       // was column A soil EC
const int TS3_SOLAR_WH_FIELD = 3;       // was column B soil EC
const int TS3_CURRENT_FIELD  = 5;       // was column C's EC slot -- free now that EC is not uploaded
const int TS3_SOLAR_V_FIELD  = 7;       // free even with all 3 columns on (pH takes 2/4/6)
const int TS3_SOLAR_W_FIELD  = 8;
bool  acsOk = false;                   // last ADS1115 read succeeded -- a dead ADC must be VISIBLE,
                                        // not silently read as 0 A, which is how the opto failure hid
float battIsigned = 0;                  // signed amps, + = discharging; battI stays magnitude
// Mixing-tank chemistry, captured from ESP2's TELE during a run (the tank is empty at idle, so these
// describe the LAST batch). -1 / 0 stamp = never measured. Published as sensors.waterEC / waterPH.
float tankEC = -1, tankPH = -1;
unsigned long tankChemMs = 0;
// Battery percent needs a full/empty voltage pair; BATT_LOW_V/BATT_CRIT_V are alarm points, not
// endpoints. 12 V lead-acid resting voltages.                                          [MEASURE]
const float BATT_FULL_V  = 12.70f;
const float BATT_EMPTY_V = 11.20f;

/* ---- Timing / supervisory constants (spec sec.10.14) --------------------- */
const unsigned long UART_ACK_TIMEOUT_MS   = 3000;    // ESP2 ACK wait
const unsigned long UART_DONE_TIMEOUT_MS  = 600000;  // ESP2 silent this long mid-run (each PROG restarts it)
const unsigned long RESUME_ACK_TIMEOUT_MS = 5000;    // recovery RESUME: bounded wait for ACK/INVALID
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
/* ---- Preventive pump exercise: operator switch + run length --------------- *
 * The exercise exists to stop a pump seizing between runs, but it is not always wanted -- it spins
 * the mixer in an empty tank, and on a battery rig every unnecessary AC start costs charge. Both
 * settings are NVS-backed and reachable from the LCD (Settings > Pump Exercise) and the dashboard.
 * The run length lives here rather than on ESP2 so one operator change covers every pump; it is sent
 * with each EXERCISE command, and ESP2 clamps it again on arrival.                                 */
bool exerciseEnabled = true;    // master on/off                                          [TBD]
int  exerciseRunS    = 5;       // seconds per exercise, EX_RUN_MIN..EX_RUN_MAX           [TBD]
const int EX_RUN_MIN = 1, EX_RUN_MAX = 10;
void saveExercise();
// From dispatch. 2026-11-15: an AC pump now waits for the Nano servo first (ESP2 gives it up to 10 s, plus the
// PZEM dead-check) before the inverter and the run (<=10 s) start -- 20 s no longer covered the worst case.
// Stuck-selector re-seats (up to 2, each ~11 s: decay + 4 servo moves) can add ~22 s more.
const unsigned long PUMP_EXERCISE_TIMEOUT_MS  = 150000;  // 60 -> 150 s (2026-11-16): from ESP2 power-up it covers boot,
                                                          // the servo pre-seat (3 swings) and the rest below;
                                                          // boot, battery switch, AC dead wait, servo, inverter confirm and the run

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
float PUMP_FLOWRATE_MLPM[3] = { 50.0f, 50.0f, 50.0f };   // pre-cal default mL/min, NVS "calib" overrides [MEASURE]
const float PUMP_FLOWRATE_DEFAULT_MLPM = 50.0f;          // what a cleared ("not set") nutrient rate goes back to
// ---- Mixing tank (§1.1): batch volume is VARIABLE; MAX is a ceiling, not the formula input ----
const float MIXING_TANK_MAX_VOLUME = MIXING_TANK_MAX_L;   // L (safety ceiling, shared/rig_constants.h)
const float MIXING_TANK_SAFE_MIN   = 5.0f;    // L (refuse dosing below)
// Per-column water budget per service: WATER_BUDGET_L, in shared/rig_constants.h with ESP2 [TBD]
static_assert(NUM_COLUMNS == sizeof(WATER_BUDGET_L) / sizeof(WATER_BUDGET_L[0]), "one budget per column");

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

bool rtcOk = false, sdOk = false;   // rtcOk: the DS3231 answers AND its time can be trusted (see rtcNow)
bool rtcChipOk = false;             // the DS3231 answered at boot, so it can at least be set
bool rtcTimeLost = false;           // it lost power or holds an impossible date: no clock until someone sets it

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

/* Display label. The actuation lockout is a FLAG (actuationsDisabled), not a system state: parking
 * SAFE_MODE in sysState looked tidy but there are 17 setState(IDLE_STATE) sites and no SAFE_MODE
 * case in stateMachineTick, so any run completion or recovery silently cleared it and the LCD then
 * read "IDLE_STATE" on a locked-out rig. Deriving the label from the flag makes that impossible.
 * The flag is passed in rather than read as a global so the core-0 uploader can hand over its
 * snapshot copy instead of reaching across cores. */
static const char *stateLabel(SystemState s, bool lockedOut) {
  if (lockedOut && s == IDLE_STATE) return "SAFE_MODE";
  return stateName(s);
}

/* ---- Per-column configuration (NVS-persisted) ---------------------------- */
enum ColMode { MODE_AUTO = 0, MODE_IRRIGATION_ONLY = 1 };
struct ColumnConfig {
  uint8_t mode;
  float   targetN, targetP, targetK, targetPH;
  char    name[17];
  long    lastServicedStamp;     // yyyymmdd that servicedMask belongs to
  uint8_t servicedMask;          // bit w = window w watered that day; 0xFF = the whole day is done or skipped
  long    fertStamp;             // yyyymmdd nutrients last went in (at most once a day)
};
ColumnConfig col[NUM_COLUMNS];
// Operation (2026-11-18, website request): Automatic = waters on its watering times (as before); Manual = readings
// only, ESP1 starts no scheduled run for the column, the operator uses forced runs. colOpManual is the column's
// default; colPlan marks single days that override it (run_plan.h). Both persist in NVS ("c<n>op", "c<n>pl"), set by
// SET_COLUMN {operation, plan, planFrom} from the website, cleared by Restore Defaults.
uint8_t colOpManual[NUM_COLUMNS] = { 0, 0, 0 };
RunPlan colPlan[NUM_COLUMNS];

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
  // web v.4: litres from the tank calibration (shared/tank_volume.h), -1 = no usable reading; the Nano's
  // median spread per sensor (cm, -1 = an older Nano that does not send it); the temperature-corrected cm.
  float resL, mixL;
  float resSpreadCm, mixSpreadCm;
  float resCm, mixCm;
  bool  resOk, mixOk;                    // usable for CONTROL (valid echo, calm enough, sane geometry)
  char  resSt, mixSt;                    // the Nano's pinger status per tank (K E R F S X; '?' = not sent)
  int   rawSoil[NUM_COLUMNS][2];         // raw per-probe soil ADC (0..1023); [c][0]=probe1 [c][1]=probe2
  float rawNpk[NUM_COLUMNS][7];          // raw Modbus registers per column
  // Per-branch last-seen so a single dead Nano sensor is detectable (lastNanoMs is whole-link).
  unsigned long msEnv, msTank, msSoil, msLight, msNpk[NUM_COLUMNS];
};
SensorData sensor;

/* ---- Nano TANK link statistics (web v.4 hardening, 2026-11-16) ------------ *
 * The Nano numbers its TANK frames 0..999. A repeat is ignored (it was applied once); a gap is frames lost on the
 * way -- dropped on a bad checksum, or eaten by a receive-buffer overflow. tankRxN / tankLostN are a decaying
 * window (halved every 200 frames), so tankLinkPct() shows the link of the last minute or so.                  */
int      tankSeqLast = -1;               // -1 = none yet (or the Nano restarted)
uint32_t tankRxN = 0, tankLostN = 0;
uint32_t tankLostTotal = 0;              // since boot
unsigned long lastTankMs = 0;            // the last TANK accepted (FAST gap watchdog)
const unsigned long NANO_FIRST_DATA_RETRY_MS = 30000;   // no TANK since boot: ask for a cycle again this often
bool     tankGapLogged = false;          // the current FAST gap has been logged
unsigned long fastSinceMs = 0;           // FAST was (re)entered here (the gap watchdog's start)
uint16_t levelSeqOut = 0;                // our LEVEL line number to ESP2 (0..9999): ESP2 drops repeats, counts gaps
uint32_t nanoRxOverflows = 0;            // EspSoftwareSerial buffer overflows (the loop stalled too long)
const unsigned long NANO_TANK_GAP_MS = 2500;   // FAST sends a TANK every ~0.3 s: this much silence = re-send FAST
const char *TANK_ST_CHARS = "KERFSX";    // the Nano's per-tank pinger status letters
static bool tankSeqAccept(int seq) {
  if (tankSeqLast >= 0) {
    int d = (seq - tankSeqLast + 1000) % 1000;
    if (d == 0) return false;                         // the same frame twice
    if (d > 1 && d < 500) { tankLostN += d - 1; tankLostTotal += d - 1; }   // a big jump = the Nano restarted
  }
  tankSeqLast = seq; tankRxN++;
  if (tankRxN + tankLostN >= 200) { tankRxN /= 2; tankLostN /= 2; }
  return true;
}
static int tankLinkPct() { uint32_t t = tankRxN + tankLostN; return t ? (int)(tankRxN * 100UL / t) : -1; }

/* ---- Nano garbage tracking / recovery ------------------------------------ */
uint8_t garbageCount = 0;
long    lastResetDayStamp = -1;
bool    nanoResetReqInFlight = false;
unsigned long nanoResetReqMs = 0;

/* ---- ESP2 work-order FSM ------------------------------------------------- */
// WO_PENDING = built and owned, but NOT yet transmitted because ESP2 is still cold-booting.
// A scheduled run parks in pendingRun for that wait; a FORCE run has no such slot, so before this
// stage existed sendForceWorkOrder() wrote the frame into a powered-down ESP2 and it was simply
// lost -- the "force run never initiates" fault. Only WO_SENT arms the ACK-timeout supervisor,
// so a pending order cannot burn its retries while the executor is still booting.
enum WoStage { WO_IDLE, WO_PENDING, WO_SENT, WO_ACKED };
struct WorkOrder {
  bool     active;
  WoStage  stage;
  int      colIdx;
  bool     fertigate;
  String   cmd;            // full framed command (for retries)
  uint8_t  retries;
  unsigned long sentMs;
  int8_t   window;         // the column's window this run counts for (-1 = none); set after sending
};
WorkOrder wo = { false, WO_IDLE, -1, false, "", 0, 0, -1 };
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
struct PendingRun { bool active; int colIdx; bool fertigate; int8_t window; };
PendingRun pendingRun = { false, -1, false, -1 };
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

/* ---- Remote push-to-column pulse (dashboard counterpart to LCD Testing) ---- *
 * State only; the machine itself lives next to testHoldTick(), whose arming pattern it mirrors.   */
// pulseRowForTarget() moved to include/pure_math.h -- pure, and tested on host.
// Litres-per-pulse divisor for the meter watching a TEST row, or 0 when that row has no meter.
// Mirrors ESP2's flowPinForBit(); kept here so the pulse result can be reported as a volume.
static float pulseKForRow(int row) {
  switch (row) {
    // Fill (16) / Push (17..19) combos: no meter since web v.4 -- ESP2 reports the mixing-tank level change
    // (TEST,LEVEL), handled in pulseFinish.
    case 1:  return calKNut[0];          // nutrient A on P1 (2026-11-16, swapped with B)
    case 7:  return calKNut[1];          // nutrient B on P7 (2026-11-16)
    case 11: return calKNut[2];
    case 12: return calKNutD;
    case 13: return calKPhUp;
    case 14: return calKPhDn;
    default: return 0.0f;                // mixer, valves, inverter, cutoff -- no meter
  }
}

enum PulsePhase { PU_NONE, PU_ARM, PU_RUN, PU_REPORT };
PulsePhase    puPhase   = PU_NONE;
int           puRow     = -1;              // ESP2 TEST row: 17 + column index
int           puSeconds = 0;
unsigned long puEndMs = 0, puArmMs = 0, puLastSendMs = 0;
char          puCmdId[48] = "";
const int           PULSE_MAX_S          = 15;    // hard cap, well inside ESP2's own 30 s
const unsigned long PULSE_ARM_TIMEOUT_MS = 14000; // ESP2 boot + TEST,ENTER ACK budget
// After TEST,RELEASE, ESP2 answers TEST,FLOW,<bit>,<pulses>. Wait briefly for it so the pulse can be
// reported as "fluid moved" or "nothing moved" -- the whole point of the test. Bounded: an ESP2 that
// never answers must not leave the command unresolved.
const unsigned long PULSE_REPORT_WAIT_MS = 800;
// Longest a Fill/Push combo may take to power its pump (see PU_RUN): 45 -> 90 s (2026-11-16), ESP2 pre-seats the servo
// (3 swings) before every pump start.
const unsigned long PULSE_AC_START_MAX_MS = 90000;
bool          puFlowSeen = false;
unsigned long puFlowPulses = 0, puReportMs = 0;
bool          puLevelSeen = false;         // web v.4: TEST,LEVEL,<row>,<dMixL> for a Fill / Push combo
bool          puInvOnSeen = false;         // ESP2 said INV_ON for this pulse: its seconds count from that (2026-11-17)
float         puLevelL = 0.0f;
// A timed pulse on a servo row (servoL / servoR, the website's pump-selector switch) ends as soon as the move is
// settled either way (2026-11-16): the Nano's confirmation (testServoDoneRow), or ESP2's SERVO_FAIL,TEST|<why>,
// kept here. Before, it held ESP2 in Testing for the whole of its seconds and then answered.
char          puServoFailWhy[24] = "";
// Web "hold-to-run" (2026-09-10, irrigation/testHold): press-and-hold from the Manual/Test tab,
// mirrors the LCD's own dead-man testHoldTick() but driven by a periodically-refreshed Firebase flag
// instead of a physical button, since a browser tab cannot hold a physical dead-man switch. Reuses
// the EXACT same PU_ARM/PU_RUN/PU_REPORT machinery as TEST_PULSE (same ESP2 arming, same 150ms
// TEST,HOLD keep-alive stream) -- puHoldMode just changes PU_RUN's stop condition from a fixed timer
// (puEndMs) to "the browser stopped refreshing" (testHoldLastFreshMs). ESP2's OWN TEST_HOLD_TIMEOUT_MS
// (400ms) is the real safety net either way, exactly as the block comment above pulseFinish() already
// explains for TEST_PULSE.
bool          puHoldMode = false;
int64_t       testHoldSeq = -1;                    // last seq seen from the page, core 0 only
unsigned long testHoldLastFreshMs = 0;
// While a hold is running, testHold is polled every TEST_HOLD_POLL_MS (not the 3 s command cadence),
// so a release or a vanished browser is noticed within ~1 s. The timeout must still comfortably exceed
// that poll AND the page's own 1.2 s refresh: testHoldLastFreshMs only advances on a poll that sees a
// new seq, so a timeout too close to either would false-trip while the browser is still holding.
const unsigned long TEST_HOLD_POLL_MS            = 1000;
const unsigned long TEST_HOLD_BROWSER_TIMEOUT_MS = 4000;  // no fresh want:true seen within this -> release
// Hard cap on one continuous hold -- the same limit a Timed Pulse gets. A browser keeps refreshing
// want:true for as long as the button is down (or the tab is wedged), so without a cap the keep-alive
// alone could run a relay until ESP2's own 30 s TEST_HARD_CAP_MS.
const unsigned long TEST_HOLD_MAX_MS = (unsigned long)PULSE_MAX_S * 1000UL;
unsigned long testHoldRunMs = 0;                   // when the current hold entered PU_RUN (core 1)
// Set whenever a hold ends for any reason other than an explicit want:false (cap, browser timeout,
// safety abort). While set, further want:true refreshes are ignored -- otherwise the very next
// keep-alive would arm a fresh hold and defeat the cap. Cleared only by a want:false (button up).
// Core 1 only.
bool          testHoldLatched = false;
// Deep-scan finding (2026-09-10, HIGH, fixed): firebasePollTestHold() runs on core 0 (netTask) and
// must NEVER call setState()/logEvent()/sendEsp2() itself -- those mutate core-1-owned state (the
// logBuf String, the ESP2 UART, sysState/uiMode) with no lock, the exact bug class this file already
// paid to fix twice for fbAuthErrLog/fbPollErrLog (see their own comments). The fix mirrors
// firebasePollManual()/webManualTick()'s own split exactly: core 0 writes only these three plain
// scalars (a bool, a small fixed char buffer -- never a String across cores -- and a bool flag);
// pulseTick() (core 1, called every loop() alongside testHoldTick() at the same call site) is the
// ONLY place that reads them and performs the actual arm/release.
bool          testHoldReqWant = false;
char          testHoldReqBit[12] = {0};            // "transfer" is the longest target (8 chars + NUL)
bool          testHoldReqFresh = false;
void pulseTick();
static void pulseFinish(bool ok, const char *why);

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

/* ---- GSM live health, mirrored from the driver each loop (LCD PAGE_GSM, logs, dashboard) ---------- */
int  lastRssi      = -1;        // AT+CSQ RSSI 0..31 (99/-1 = no signal)
bool netRegistered = false;     // registered on CREG or CEREG (LTE); see gsmRegStatOk
int  lastCreg      = -1;        // the registered one of CEREG/CREG, else raw CREG (2=searching, 3=denied, etc.)
int  lastCereg     = -1;        // raw AT+CEREG stat, for the logs
bool simReady      = false;     // AT+CPIN? == READY
unsigned long lastGsmHealthMs = 0;

// The driver owns the modem, its outbound queue and every AT exchange (include/gsm_modem.h).
// sendSMS() only enqueues, so fault/alert paths never stall the loop.
GsmModem gsm;
// Reply target for messages generated while handling an INBOUND command: set to the
// sender's number in gsmOnSms so STATUS/ACK replies go back to whoever texted. Empty
// => autonomous alerts/reports go to the configured owner PHONE_NUMBER.
String   replyTarget = "";

/* ---- WiFi + ThingSpeak telemetry (Part A) -------------------------------- *
 * ESP32 #1 joins the nearby WiFi and uploads a numeric snapshot to ThingSpeak,
 * which draws a live graph per field (viewable from anywhere). All network work
 * runs in a core-0 FreeRTOS task so blocking HTTP never disturbs the core-1 loop
 * or the 8 s task WDT. Credentials + write keys are set by SMS (Part B), NVS-kept.
 * Channel 1 "Columns" = per-column moisture + N,P,K. Channel 2 "System" = temp,
 * hum, lux, reservoir%, mixing%, battV, battW, flow. Channel 3 "Chem" = solar I,
 * pH A, solar Wh today, pH B, battery A, pH C, solar V, solar W (EC dropped 2026-11-15). */
const char *TS_HOST = "api.thingspeak.com";
const unsigned long TS_UPLOAD_IDLE_MS   = 60000;   // upload cadence, every mode (20 s during a run before 18 Nov 2026 e:
                                                   // the rig now comes first while busy -- see LINK PACE below)
const unsigned long WIFI_RETRY_MS       = 15000;   // reconnect backoff
const unsigned long WIFI_CONNECT_MS     = 12000;   // per-attempt connect budget
const uint16_t      TS_TIMEOUT_MS       = 4000;    // connect / answer budget per ThingSpeak send (6 s before 18 Nov 2026 c)

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
/* ---- LINK PACE (18 Nov 2026 e; "link lite" 18 Nov 2026 g, the owner) --------------------------------------------- *
 * e: "Don't show realtime; a Refresh button for live data; still refresh every 20-30 s or longer, only while idle.
 *    During an operation, or when a fertigation/irrigation is coming up, focus on the operation, not Firebase."
 * g: "Make the website communication back to before, no longer high realtime ... still it automatically refreshes to
 *    request data from the system, and a refresh button. To lessen the load of ESP1." So the link is back to its pre-17 Nov
 *    weight: one report a minute, no "watched" pace (the page no longer touches irrigation/viewer/at), no automatic
 *    upload of old SD logs (the 17 Nov catch-up of up to 14 days at ~1.2 MB each, never finished on this link), and the
 *    two polls added on 17 Nov slowed down. The SD card's own record (D:\, 10-18 Nov) shows why: at -84..-89 dBm ESP1's
 *    reports failed 1-8 % a day from 10 to 17 Nov, and 97 % on 18 Nov with the extra traffic.
 *
 * ESP1 is in one of two link modes (linkModeNow(), core 0):
 *   BUSY    a run, its warm-up, a pump exercise, an armed forced run, a dosed tank waiting, or a watering window that
 *           is open (and not yet served) or opens within LINK_PRE_RUN_MIN -- see linkBusyFlag, set on core 1.
 *           A Refresh at most every FB_REFRESH_GAP_BUSY_MS, no SD log upload, no quick retries. Commands (and every
 *           stop) are still read every FIREBASE_COMMAND_POLL_MS, and a fault still pushes at once.
 *   IDLE    everything else.
 * The website reads the pace from meta.refreshMs and the mode from meta.linkMode, and works out "late" and "lost" from
 * them, so these numbers can change here without a website change. Keep it that way: one table, here. */
const unsigned long FB_PACE_IDLE_MS          = 60000;   // one report a minute (the pace before 17 Nov)
const unsigned long FB_PACE_BUSY_MS          = 60000;   // a run or one coming up: the rig comes first (20 s before 17 Nov)
const unsigned long FB_REFRESH_GAP_IDLE_MS   = 10000;   // the Refresh button / page open, idle
const unsigned long FB_REFRESH_GAP_BUSY_MS   = 30000;   // the Refresh button / page open, busy
const unsigned long FB_VIEWER_POLL_MS        = 6000;    // irrigation/viewer (Refresh requests); 3 s on 17-18 Nov f
const unsigned long FB_LOGREQ_POLL_MS        = 30000;   // irrigation/logRequest ("Upload from the rig"); 10 s on 17-18 Nov f
const uint16_t      LINK_PRE_RUN_MIN         = 10;      // BUSY this many minutes before a watering window opens
// Recovery (18 Nov 2026 e; g: 3 / 10, was 2 / 8): the reports that failed in a row with a network error (code -1..-99).
// After every FB_RESET_AFTER_FAILS of them the TLS session is dropped and the next request opens a fresh one; after
// FB_REJOIN_AFTER_FAILS, WiFi is left and joined again. On 18 Nov 398 reports failed on one session without either.
const uint8_t       FB_RESET_AFTER_FAILS     = 3;
const uint8_t       FB_REJOIN_AFTER_FAILS    = 10;
// Firebase request budgets (18 Nov 2026 f). On 18 Nov 18:32 (18e, -87 dBm) Firebase STORED a report while ESP1 gave up
// waiting for the 204 (http=-11): 6 s was too short for the reply on this link. Connect stays 6 s.
const uint16_t      FB_CONNECT_TIMEOUT_MS    = 6000;
const uint16_t      FB_REPLY_TIMEOUT_MS      = 12000;
enum LinkMode : uint8_t { LINK_IDLE = 0, LINK_BUSY = 2 };   // 1 was WATCHED (30 s while a page was open), 18 Nov e-f
volatile bool       linkBusyFlag = false;               // core 1 (telemetryCollect) -> core 0
volatile uint32_t   fbLinkResets = 0, fbWifiRejoins = 0;   // recovery counters since this start (diagnostics)
volatile uint32_t   fbTextFixes = 0;                    // reports whose text needed cleaning before the PUT (18 Nov 2026 g)
// Firebase's own words when it refuses a report (18 Nov 2026 g): core 0 -> core 1 (logEvent is core-1 only), as
// FBASE,REPLY|... and FBASE,TEXT|... lines. On 18 Nov evening every report that reached Firebase got HTTP 400, and the
// reason was thrown away with the body.
char                fbReplyLog[176] = "";
volatile bool       fbReplyLogPending = false;
const unsigned long FB_PUSH_MIN_GAP_MS        = 5000;   // floor between error-triggered out-of-band pushes
const unsigned long FB_PUSH_FRESH_WAIT_MS     = 2500;   // longest a push waits for a snapshot taken after its event
// Command poll cadence. The TLS connection is REUSED between polls (fbClient/fbHttp below),
// so this is one small GET on an open socket -- not a handshake every 3 s.
const unsigned long FIREBASE_COMMAND_POLL_MS  = 3000;
// Refresh (2026-11-17; LINK PACE since 18 Nov 2026 e). The website's Refresh button, and the page opening or coming back
// to the front, touch irrigation/viewer/refreshAt. ESP1 polls that node every FB_VIEWER_POLL_MS and answers with a
// report at once (at most every FB_REFRESH_GAP_IDLE_MS / _BUSY_MS). irrigation/viewer/at (a page being open, 17 Nov to
// 18 Nov f) is no longer used: since 18 Nov g the pace does not change with viewers. Core 0 (netTask) only.
const unsigned long FB_LOG_STEP_MIN_MS             = 500;     // the SD log upload: one piece a pass at most
// A live report that fails is tried again after FB_UPLOAD_RETRY_MS, up to FB_UPLOAD_RETRIES times in a row (18 Nov 2026 c),
// instead of at the next tick: the website calls ESP1 lost at 90 s. Not while BUSY (18 Nov 2026 e): the next tick will do.
const unsigned long FB_UPLOAD_RETRY_MS             = 3000;
const uint8_t       FB_UPLOAD_RETRIES              = 3;
// FBASE,OK/FAIL lines at most this often (18 Nov 2026 c). Nothing is lost -- every line carries the running totals
// n= (attempts) and fail= (failures).
const unsigned long FB_LOG_TRANSITION_MIN_MS       = 10000;
bool          fbRefreshPending = false;
static LinkMode linkModeNow() { return linkBusyFlag ? LINK_BUSY : LINK_IDLE; }
static unsigned long linkPaceMs(LinkMode m) { return m == LINK_BUSY ? FB_PACE_BUSY_MS : FB_PACE_IDLE_MS; }
static const char *linkModeName(LinkMode m) { return m == LINK_BUSY ? "busy" : "idle"; }
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
/* ---- MEMORY for the network (18 Nov 2026 h) -------------------------------------------------------------------------
 * ESP.getFreeHeap() / ESP.getMaxAllocHeap() count MALLOC_CAP_INTERNAL, which on the ESP32 includes ~40 KB of spare IRAM
 * that only 32-bit loads can use: never a String, a JSON text or an mbedTLS buffer. That is why the "largest block" read
 * 40,948 all day on 18 Nov (it is that IRAM), and why "68 KB free" was really ~27 KB of usable memory. The 60 / 32 KB
 * floors of 17-18 Nov were measured against that number and protected nothing. From 18 Nov h every decision uses
 * byte-addressable memory (MALLOC_CAP_8BIT): heap8Free() and heap8Block().
 *   FB_TLS_MIN_BLOCK / _FREE  a NEW TLS session needs one block for mbedTLS's 16 KB input buffer and ~24 KB in all.
 *                             Below that the handshake can only fail (http=-1) after churning the heap, so it is not
 *                             tried (fbBegin) and the next pass tries again.
 *   FB_REPORT_MIN_BLOCK       the report's JSON text (~5.1 KB) in one block.
 * 18 Nov h also stopped HTTPClient copying every body: PUT(String) / POST(String) / PATCH(String) take the String BY
 * VALUE, a second 5-7 KB block. When that copy failed the request went out with NO body and Firebase answered HTTP 400
 * "No data supplied." -- logged by 18 Nov g (FBASE,REPLY), the cause of every 400 that evening. */
const uint32_t FB_TLS_MIN_BLOCK    = 17000;
const uint32_t FB_TLS_MIN_FREE     = 24000;
const uint32_t FB_REPORT_MIN_BLOCK = 6144;
static inline uint32_t heap8Free()  { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
static inline uint32_t heap8Block() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }
volatile uint32_t fbTlsSkips = 0;                  // new TLS sessions not tried for want of memory (diagnostics)
// URL is NVS-backed (SoftAP admin form / FBASE SMS) -- never hardcoded in tracked source,
// same rule as the Supabase creds. Empty URL or firebaseEnabled=false disables the feature.
String   fbUrl = "";                               // e.g. "https://<ref>-default-rtdb.<region>.firebasedatabase.app"
bool     firebaseEnabled = true;                   // master switch (NVS "fben")
volatile bool fbPersistPending = false;            // core0 admin form -> core1: persist fbUrl/firebaseEnabled
volatile bool fbLastOk = false;                    // last PUT result (diagnostics page)
volatile unsigned long fbLastUploadMs = 0;         // cadence gate + "last seen" age
volatile uint32_t fbLastHeap = 0;                  // byte-addressable heap free at the last attempt (18 Nov h: was INTERNAL)
volatile uint32_t fbLastBlock = 0;                 // ...and its largest block (18 Nov 2026 h)
volatile int  fbLastHttp = 0;                      // last HTTP status (0 = never attempted / skipped)
volatile uint32_t fbAttempts = 0, fbFailures = 0;  // lifetime counters (logged on transition)
// 18 Nov 2026 (c): counts FINISHED firebaseUploadLive() calls, so firebaseLogTick() (core 1) reads fbLastOk/fbLastHttp only
// once they describe the call that just ended. It used fbLastUploadMs, which is stamped when a call STARTS, so each
// FBASE line described the attempt before, and the "FAIL|http=0" just after every restart was a request still on the wire.
volatile uint32_t fbUploadDone = 0;
const char * volatile fbLastSkip = "";             // why the last call sent nothing ("" = it sent); string literals only

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
// Same hand-off for the command-poll diagnostics. firebasePollCommands() also runs on netTask and
// was calling logEvent() directly, which appends to logBuf -- a core-1 String. Reallocating that
// from core 0 is a heap race; rare (both paths are deduped to once per 60 s) but not something to
// leave in. firebaseLogTick() drains this on core 1 like the auth message above.
char     fbPollErrLog[96] = "";
volatile bool fbPollErrLogPending = false;
bool     fbPinCa = true;                           // validate the server cert against FB_ROOT_CA (NVS "fbpin")
unsigned long fbTokenExpiryMs = 0;                 // millis() deadline for the current ID token
unsigned long fbTokenUsableMs = 0;                 // when that token really stops working (18 Nov 2026 c, fbEnsureToken)
volatile bool fbAuthOk = false;                    // last token operation succeeded (diagnostics)
volatile int  fbAuthHttp = 0;                      // last auth HTTP status
volatile bool fbSignInPending = false;             // portal/SMS -> netTask: force a password sign-in now
volatile bool fbCredsPersistPending = false;       // core0 admin form -> core1: persist auth creds
volatile bool fbTlsResetPending = false;           // SMS (core 1) -> netTask: re-apply the TLS trust policy
// Auth backoff: a wrong password must NOT be retried every poll (that is credential
// hammering and Google will rate-limit the account). Same ladder shape as uploadTick().
const unsigned long FB_AUTH_BACKOFF[4] = { 30000UL, 120000UL, 480000UL, 1800000UL };
// 18 Nov 2026 (c): a renewal that never reached Google (no WiFi, DNS or TLS: HTTP code <= 0) is the network, not the
// credentials, so it is tried again this soon and does not climb that ladder. It used to: on 17 Nov a short drop during
// a renewal (09:43-09:47, 14:53-14:59) meant 30 s, then 2 min, then 8 min with no reports at all.
const unsigned long FB_AUTH_NET_RETRY_MS = 10000;
uint8_t  fbAuthFails = 0;
unsigned long fbAuthNextTryMs = 0;

/* ---- Firebase remote commands --------------------------------------------- *
 * Core 0 (netTask) only transports; core 1 validates and dispatches. FreeRTOS queues
 * carry POD structs across the core boundary -- no Strings, no shared mutable state.  */
// FORCE_RUN carries an operator-chosen batch: which columns, how many litres TOTAL, and explicit
// per-nutrient mL. POD only -- this crosses a core boundary through a FreeRTOS queue.
struct FirebaseCommand       { char id[48]; char type[32]; char pump[16];
                               char cols[4]; float liters; float doseMl[3];
                               uint16_t delayS;      // FORCE_RUN start delay (s), 0..FORCE_MAX_DELAY_S
                               char arg[16];         // RECOVER action / REBOOT target / SET_COLUMN col / TEST_PULSE zone
                               int   seconds;        // TEST_PULSE duration
                               int8_t exEnabled;     // SET_EXERCISE: -1 absent, 0 off, 1 on
                               int   exSeconds;      // SET_EXERCISE: -1 absent
                               // SET_COLUMN, all optional: -1 means "field absent, leave unchanged".
                               char  colMode[20];    // "AUTO" | "IRRIGATION_ONLY" ("" = absent)
                               int8_t enabled, schedMode;
                               int   winStart, winEnd;
                               // SET_COLUMN windows[] (2026-11-05): -1 absent, else how many {start,end} pairs
                               // came (MAX_RUNS_PER_DAY + 1 = too many); -1 in a pair = not a whole number.
                               int8_t  winCount;
                               int16_t wins[MAX_RUNS_PER_DAY][2];
                               float tgt[4];         // N, P, K, pH  (-1 = absent)
                               char  preset[16];     // SET_COLUMN: crop preset name ("" = absent)
                               // SET_COLUMN Operation and day plan (2026-11-18, run_plan.h). operation: -1 absent,
                               // 0 AUTOMATIC, 1 MANUAL, 2 not recognised. planState: 0 absent, 1 copied whole into
                               // plan[], 2 not a string or longer than RUN_PLAN_DAYS. planFrom: yyyymmdd of plan[0]
                               // (-1 absent = ESP1's today).
                               int8_t operation, planState;
                               char  plan[RUN_PLAN_DAYS + 2];
                               long  planFrom;
                               // SET_COLUMN own Soil start / Too wet % (2026-11-18): COL_PCT_ABSENT, -1 = back to
                               // the default, 0..100 = its own; COL_PCT_BAD = not a whole number. Range-checked on core 1.
                               int   soilStart, soilWet;
                               // SET_CLOCK, all required together (not optional-absent like above).
                               int16_t clkY; int8_t clkMo, clkD, clkH, clkMi;
                               // SET_THRESH: start + gap required together; Too wet and the reservoir pause
                               // (2026-11-18) optional, -1 = absent (an older page sends only the first two).
                               int   thStart; float thGap; int thWet, thRes;
                               // SET_AC_VOLT, both required (-1 = absent or not a whole number).
                               int   acVMin, acVMax;
                               // SET_AC_AMP, both required, whole tenths of an amp (-1 = absent or not whole).
                               int   acAMin10, acAMax10; };
// A forced run arms a countdown before it fires, so the operator can see the rig accepted it and
// can still stop it. The default is deliberately non-zero: an immediate start leaves no window to
// catch a mistyped volume. 0 is still allowed for a deliberate fire-now.
const uint16_t FORCE_DEFAULT_DELAY_S = 30;
const uint16_t FORCE_MAX_DELAY_S     = 300;
const int COL_PCT_ABSENT = -1000, COL_PCT_BAD = -1001;   // FirebaseCommand.soilStart / soilWet sentinels
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

/* Google Trust Services roots R1..R4 (https://pki.goog/repo/certs/gtsr{1,2,3,4}.pem, all valid to
 * 2036-06-22) -- the CAs behind *.googleapis.com and *.firebasedatabase.app. With these set, the
 * Firebase client VALIDATES the server certificate, so the provisioning sign-in cannot be MITM'd for
 * the device password. Blank it to fall back to setInsecure() (encrypted but unauthenticated).
 *
 * WHY ALL FOUR (2026-11-09 fix for the rig's FB:!! / "transport error code -1"): this used to pin
 * R1 alone. Google's sign-in hosts (identitytoolkit + securetoken.googleapis.com) now serve an ECDSA
 * chain, leaf -> WE2 -> GTS Root R4, so with R1 alone EVERY sign-in failed the handshake and the
 * device could never get a token, while the RTDB host itself (-> WR1 -> R1) still verified fine.
 * Google can serve any of its four roots depending on host and on which key type the client offers,
 * so trusting only the one in use today is what broke this. Checked 2026-11-09 with
 * openssl s_client -CAfile <these four>: identitytoolkit, securetoken and the project's RTDB host all
 * verify (code 0), whether the client prefers ECDSA or RSA. SHA-256 fingerprints, to re-check:
 *   R1 D9:47:43:2A:BD:E7:B7:FA:90:FC:2E:6B:59:10:1B:12:80:E0:E1:C7:E4:E4:0F:A3:C6:88:7F:FF:57:A7:F4:CF
 *   R2 8D:25:CD:97:22:9D:BF:70:35:6B:DA:4E:B3:CC:73:40:31:E2:4C:F0:0F:AF:CF:D3:2D:C7:6E:B5:84:1C:7E:A8
 *   R3 34:D8:A7:3E:E2:08:D9:BC:DB:0D:95:65:20:93:4B:4E:40:E6:94:82:59:6E:8B:6F:73:C8:42:6B:01:0A:6F:48
 *   R4 34:9D:FA:40:58:C5:E2:63:12:3B:39:8A:E7:95:57:3C:4E:13:13:C8:3F:E6:8F:93:55:6C:D5:E8:03:1B:3C:7D
 * mbedTLS parses a concatenated PEM bundle like this one in a single setCACert() call.
 * If TLS ever starts failing again (PAGE_GSM FB:!!, portal "transport error code -1"), compare
 * the hosts' chains against this list first.  */
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
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlrsWNBCUaqxElqjANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjIwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjIwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQDO3v2m++zsFDQ8BwZabFn3GTXd98GdVarTzTukk3LvCvpt
nfbwhYBboUhSnznFt+4orO/LdmgUud+tAWyZH8QiHZ/+cnfgLFuv5AS/T3KgGjSY
6Dlo7JUle3ah5mm5hRm9iYz+re026nO8/4Piy33B0s5Ks40FnotJk9/BW9BuXvAu
MC6C/Pq8tBcKSOWIm8Wba96wyrQD8Nr0kLhlZPdcTK3ofmZemde4wj7I0BOdre7k
RXuJVfeKH2JShBKzwkCX44ofR5GmdFrS+LFjKBC4swm4VndAoiaYecb+3yXuPuWg
f9RhD1FLPD+M2uFwdNjCaKH5wQzpoeJ/u1U8dgbuak7MkogwTZq9TwtImoS1mKPV
+3PBV2HdKFZ1E66HjucMUQkQdYhMvI35ezzUIkgfKtzra7tEscszcTJGr61K8Yzo
dDqs5xoic4DSMPclQsciOzsSrZYuxsN2B6ogtzVJV+mSSeh2FnIxZyuWfoqjx5RW
Ir9qS34BIbIjMt/kmkRtWVtd9QCgHJvGeJeNkP+byKq0rxFROV7Z+2et1VsRnTKa
G73VululycslaVNVJ1zgyjbLiGH7HrfQy+4W+9OmTN6SpdTi3/UGVN4unUu0kzCq
gc7dGtxRcw1PcOnlthYhGXmy5okLdWTK1au8CcEYof/UVKGFPP0UJAOyh9OktwID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQUu//KjiOfT5nK2+JopqUVJxce2Q4wDQYJKoZIhvcNAQEMBQADggIBAB/Kzt3H
vqGf2SdMC9wXmBFqiN495nFWcrKeGk6c1SuYJF2ba3uwM4IJvd8lRuqYnrYb/oM8
0mJhwQTtzuDFycgTE1XnqGOtjHsB/ncw4c5omwX4Eu55MaBBRTUoCnGkJE+M3DyC
B19m3H0Q/gxhswWV7uGugQ+o+MePTagjAiZrHYNSVc61LwDKgEDg4XSsYPWHgJ2u
NmSRXbBoGOqKYcl3qJfEycel/FVL8/B/uWU9J2jQzGv6U53hkRrJXRqWbTKH7QMg
yALOWr7Z6v2yTcQvG99fevX4i8buMTolUVVnjWQye+mew4K6Ki3pHrTgSAai/Gev
HyICc/sgCq+dVEuhzf9gR7A/Xe8bVr2XIZYtCtFenTgCR2y59PYjJbigapordwj6
xLEokCZYCDzifqrXPW+6MYgKBesntaFJ7qBFVHvmJ2WZICGoo7z7GJa7Um8M7YNR
TOlZ4iBgxcJlkoKM8xAfDoqXvneCbT+PHV28SSe9zE8P4c52hgQjxcCMElv924Sg
JPFI/2R80L5cFtHvma3AH/vLrrw4IgYmZNralw4/KBVEqE8AyvCazM90arQ+POuV
7LXTWtiBmelDGDfrs7vRWGJB82bSj6p4lVQgw1oudCvV0b4YacCs1aTPObpRhANl
6WLAYv7YTVWW4tAR+kg0Eeye7QUd5MjWHYbL
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPluILrIPglJ209ZjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjMwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjMwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AAQfTzOHMymKoYTey8chWEGJ6ladK0uFxh1MJ7x/JlFyb+Kf1qPKzEUURout736G
jOyxfi//qXGdGIRFBEFVbivqJn+7kAHjSxm65FSWRQmx1WyRRK2EE46ajA2ADDL2
4CejQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBTB8Sa6oC2uhYHP0/EqEr24Cmf9vDAKBggqhkjOPQQDAwNpADBmAjEA9uEglRR7
VKOQFhG/hMjqb2sXnh5GmCCbn9MN2azTL818+FsuVbu/3ZL3pAzcMeGiAjEA/Jdm
ZuVDFhOD3cffL74UOO0BzrEXGhF16b0DjyZ+hOXJYKaV11RZt+cRLInUue4X
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
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
// CSV logs to the dashboard (2026-11-17, user: "Supabase has expired"). Each day's SD CSV now goes to the Firebase RTDB
// that already serves the website, as text pieces: logs/files/<yyyymmdd>/<nnnn> (LOG_CHUNK_BYTES each) plus
// logs/index/<yyyymmdd> = {state, size, chunks, partial, at}. The website lists them and joins the pieces into a download.
// Completed days go up by themselves, oldest first after fbLogLast; a day asked for on the website (irrigation/logRequest,
// today included -- as far as it is written) goes first. Only days from logKeepFrom on (LOG_KEEP_DAYS) are sent or kept.
// One piece per netTask pass, so commands and the live report keep flowing during an upload. Supabase stays the target
// only while Firebase is off.
// 12288 -> 6144 (2026-11-17 evening): the 12 KB piece buffer was a static array, so it took 12 KB of heap for good. ESP1 then
// ran at 55-60 KB free during runs, under FIREBASE_MIN_HEAP, and skipped every Firebase request (no FBASE,OK after any
// restart from 17:58). Now a piece is read in small blocks into its JSON text, which is freed after each request.
const uint16_t LOG_CHUNK_BYTES = 6144;
const uint16_t LOG_READ_BLOCK  = 512;              // bytes read from the SD card at a time (netTask stack)
// Why the last Firebase log upload failed, for core 1's UPLOAD,FAIL line (2026-11-17: "http=-1" alone was a dead end).
char     fbLogFailStep[24] = "";
uint32_t fbLogFailHeap = 0, fbLogFailBlock = 0;
const int      LOG_KEEP_DAYS   = 14;
long          fbLogLast = 0;                       // last completed day uploaded to Firebase (NVS "fblogl")
volatile long logKeepFrom = 0;                     // core 1 -> core 0: the oldest day kept (yyyymmdd; 0 = unknown)
volatile long webLogReqStamp = 0;                  // core 0 -> core 1: a day the website asked for (0 = none)
volatile bool uploadWasFb = false;                 // the attempt in flight / just done went to Firebase
volatile bool uploadReqPartial = false;            // core 1 -> core 0: the requested day is today (still growing)
volatile bool adminUploadReq    = false;           // SoftAP admin "Upload logs" button (core0 -> core1)
volatile bool wifiEnabled   = true;                // Settings > WiFi master switch (STA + telemetry); NVS-kept
volatile bool wifiConnected = false;
volatile int  wifiRssiVal   = 0;
char    wifiIpStr[16] = "0.0.0.0";                 // fixed buffer (written core 0, read core 1; no String race)
volatile bool wifiCredsChanged = false;            // SMS sets new creds -> task reconnects
volatile unsigned long lastTsUploadMs = 0;
volatile bool lastTsOk = false;
TaskHandle_t  netTaskHandle = NULL;
/* ---- netTask liveness ------------------------------------------------------ *
 * esp_task_wdt_add(NULL) in wdtSetup() registers only the CALLING task, so the core-1 control loop
 * is watchdogged and netTask is not. A wedged netTask (mbedTLS, the WiFi stack, a driver) kills
 * telemetry, remote commands and remote recovery silently and permanently -- irrigation carries on
 * as normal, so nothing signals that the dashboard has died. That was tolerable when the dashboard
 * was read-only; it is now the primary remote-control surface, emergency stop included.
 *
 * netTask bumps this each pass; netHealthTick() on core 1 watches it. Deliberately NOT an
 * esp_task_wdt_add on netTask itself: a panic reboot would be a far blunter response than an alarm
 * plus an idle-only reset, and a slow TLS handshake is not a hang.                                */
volatile uint32_t netBeat = 0;
const unsigned long NET_STALL_WARN_MS  = 300000;   // 5 min without a beat -> fault + SMS
const unsigned long NET_STALL_RESET_MS = 600000;   // 10 min -> self-reset, but only when idle

// ---- WiFi provisioning portal (SoftAP + captive web form, edit SSID/pass from a phone) ----
// The ESP32 briefly hosts its own WPA2 AP + a web form so creds can be set without SMS/buttons.
// Runs entirely in netTask (core 0); NVS is written by the core-1 loop (wifiPersistPending).
const char *AP_SSID = "Irrig-Setup";               // provisioning AP name
const char *AP_PASS = SECRET_AP_PASS;              // WPA2 (>=8 chars) -- NOT open (secrets.h)
const unsigned long PORTAL_TIMEOUT_MS = 600000;    // auto-close after 10 min unused
volatile bool portalRequested    = false;          // core1/SMS -> netTask: start the portal
volatile bool portalActive       = false;          // netTask -> core1: AP is up (LCD banner)
volatile bool portalCancel       = false;          // core1/SMS -> netTask: tear down now
volatile bool wifiPersistPending = false;          // netTask -> core1: persist staged creds to NVS
char portalApIp[16] = "192.168.4.1";               // softAP IP, shown on LCD/SMS

// ---- Portal admin (PIN-gated: SD format/download/review + owner-number edit) ----
// Admin actions run on core 0 (portal) but touch core-1-owned resources: SD (guarded by sdMux) and
// PHONE_NUMBER (staged to core 1 via pendingOwner). PIN + owner persist in NVS on core 1.
const char *ADMIN_PIN_DEFAULT = SECRET_ADMIN_PIN;  // change at commissioning (secrets.h)
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

// ESP2 sensors are pulled live one-at-a-time via a CAL round-robin (not streamed in normal
// operation). Declared here rather than with the rest of the diag globals because
// TelemetrySnapshot below sizes its mirror array from DIAG_ESP2_N.
// web v.4: FLOW_RESMIX / FLOW_MIXIRR are gone (no meters); the tank litres come from the Nano instead.
const char *DIAG_ESP2_ID[] = { "PH", "EC", "ACS712", "PZEM_V", "PZEM_I", "PZEM_P",
                               "FLOW_NUTA", "FLOW_NUTB", "FLOW_NUTC",
                               // NUTD / pH up / pH down are wired but were never swept, so nothing
                               // read them at all. Needed for the Manual/Test flow table to cover
                               // every meter. ~3 s more per full sweep, well inside the 60 s window.
                               "FLOW_NUTD", "FLOW_PHUP", "FLOW_PHDN" };
const uint8_t DIAG_ESP2_N = sizeof(DIAG_ESP2_ID) / sizeof(DIAG_ESP2_ID[0]);

// Inter-core telemetry snapshot: filled on core 1 (telemetryCollect, POD only -> spinlock),
// read on core 0 by the upload task.
struct TelemetrySnapshot {
  float temp, hum, lux, resLevel, mixLevel, flow;
  float resL, mixL;                                  // web v.4: tank litres (-1 = not usable)
  int   soil[NUM_COLUMNS];
  float npkN[NUM_COLUMNS], npkP[NUM_COLUMNS], npkK[NUM_COLUMNS];
  float npkEC[NUM_COLUMNS], npkPH[NUM_COLUMNS];
  // The 7-in-1's other two channels. Both were read and scaled, then thrown away: root-zone
  // temperature is genuinely useful, and its moisture now votes in the column average.
  float npkMoist[NUM_COLUMNS], npkTemp[NUM_COLUMNS];
  bool  npkValid[NUM_COLUMNS];
  float battV, battP, battI;   // battI added for the ACS758 ThingSpeak field (core-0 uploader)
  float battIsigned_;          // + = discharging (meaningful only with battFromIna_)
  bool  battIOk_, battFromIna_;              // a current sensor answered / the main INA226 is the source
  bool  solarOk_;              // solar INA226 answering (0 V at night is still a valid reading)
  float solarV_, solarI_, solarP_;
  double chargedWh_;
  // Which pumps are energised, derived on CORE 1 from ESP2's reported stage. Derived here rather
  // than read from run.stage in the uploader: run[] is core-1 state and a char array read from
  // core 0 can tear -- the same cross-core mistake this snapshot exists to prevent.
  bool pumpTransfer, pumpBooster, pumpMixer;
  float tankEC, tankPH;        // last mixing-tank chemistry from ESP2's TELE (-1 = never measured)
  // Live run progress, mirrored from RunUi so the dashboard can show a real progress bar instead of
  // just "a run is active". Copied under the spinlock (stage included) for the same tearing reason.
  bool     runActive, runFert;
  char     runStage[18];
  uint8_t  runOrd, runTotal, runPhaseId;
  int      runCol;
  uint8_t  runMask;            // every column the run waters (a FORCE run may water several): published as "A+B"
  float    runStageL, runStageTgt, runWaterGot, runWaterTgt;
  float    runDoseTgt[3], runDoseGot[3];
  // Diagnostics mirrored for the dashboard's diagnostics panel. All core-1 owned, so none of it may
  // be read directly by the core-0 uploader -- lastFault especially: it is a String on core 1, and
  // reading one across cores can catch a freed buffer mid-reassign.
  bool     esp2Avail, esp2Pwr, esp2Lost;
  unsigned long esp2AgeMs, nanoAgeMs, gsmHealthAgeMs;
  bool     pendRun, pendEx;
  int      pendExIdx;
  bool     rtcOk_, sdOk_, acsOk_;
  bool     secOk_, secLow_, onSecondary_;           // secondary battery (INA226) + supply in use
  bool     bswLatched_; uint8_t bswStrikes_;        // battery-switch fault latch + strike count
  int16_t  acVMin_, acVMax_;                        // AC supply window ESP2 enforces (PZEM)
  float    acLastV_; uint32_t acLastAgeMs_;         // last AC voltage measured (-1 / 0xFFFFFFFF = none)
  int16_t  acAMin10_, acAMax10_;                    // AC pump current limits, tenths of an amp (2026-11-16)
  float    acLastI_; uint32_t acLastIAgeMs_;        // last AC pump current measured (-1 / 0xFFFFFFFF = none)
  uint8_t  servoSide_; uint32_t servoSideAgeMs_;    // pump selector's confirmed side (0 = unknown; age 0xFFFFFFFF = from NVS)
  uint8_t  bkState_;                                // backup state (BkState)
  float    secV_, secI_, secP_, secRest_;
  bool     simOk, netReg;
  int      rssi, creg;
  bool     battLow, battCrit;
  // lastFault was 24 bytes -- too small for several real lastFaultMsg values ("AUTO-CANCELLED ...",
  // "Resume attempt got no reply - still held", "Resume already in progress - waiting for ESP2"),
  // each truncated to a meaningless fragment (losing exactly the words that message existed to
  // convey) right before publishing to the fault banner's own title. strlcpy is memory-safe either
  // way, so the truncation was silent -- 96 gives real headroom without meaningfully affecting RAM.
  char     lastFault[96], lastFaultAt[20];
  char     heldClearReason[16];   // why esp2Held last cleared -- see lastHeldClearReason
  char     deviceTime[20];        // read on core 1: the RTC shares its I2C bus with core 1's sensors
  // Fault / recovery, so the dashboard can offer the same choices the LCD menu does.
  bool     held, steerRel;
  uint8_t  holdRepeats;
  long     ackLeftS;              // >0 = operator asked to be re-prompted in this many seconds
  bool     actDisabled;
  bool     exEnabled;      // preventive pump exercise on/off
  // Today's tally per column, and each pump's minutes to its next exercise (2026-11-06), for the website. Copied on
  // core 1 like everything else here.
  float    dayWaterL[NUM_COLUMNS], dayNutMl[NUM_COLUMNS];
  uint8_t  dayWaterings[NUM_COLUMNS], dayFeedings[NUM_COLUMNS];
  long     exNextMin[3];
  // Operation + day plan per column (2026-11-18): the default, today's, and the plan text from today. planYmd is the
  // date of plan[c][0]; -1 while ESP1's date isn't known, and then no plan is published.
  uint8_t  opManual[NUM_COLUMNS], opManualToday[NUM_COLUMNS];
  char     plan[NUM_COLUMNS][RUN_PLAN_DAYS + 1];
  long     planYmd;
  bool     wmHeld;         // dashboard manual-mode hold
  long     wmLeftS;
  char     wmState[10], wmWhy[40];
  int      exRunS;         // its run length, seconds
  // Armed forced run. secondsLeft is computed HERE: fcFireAtMs is a millis() value and means
  // nothing to a browser on a different clock.
  bool     fcArmed, fcWeb;
  long     fcLeftS;
  float    fcLiters, fcDose[3];
  char     fcCols[4];
  // Raw sensor block for the web diagnostics tab -- the same values the LCD Sensor Diag pages show.
  float    rTemp, rHum, rLux, rResCm, rMixCm, rFlow;
  char     rResSt, rMixSt;                        // the Nano's pinger status per tank (K E R F S X, '?' = not sent)
  int      rSoil[NUM_COLUMNS][2];
  float    rNpk[NUM_COLUMNS][7];
  unsigned long agEnv, agTank, agSoil, agLight, agNpk[NUM_COLUMNS];
  // ESP2 raws, only meaningful after a sweep (LCD Sensor Diag or a remote DIAG_SWEEP).
  float    e2Raw[DIAG_ESP2_N];
  bool     e2Valid[DIAG_ESP2_N];
  unsigned long e2Age[DIAG_ESP2_N];
  bool     e2Any, sweepActive;
  long     sweepLeftS;
  // Validity/state mirrored here so the core-0 uploaders never touch core-1's `sensor`/`wo`/`sysState`
  // directly (a torn read there publishes e.g. tankValid against a half-updated level).
  bool  envValid, tankValid, lightValid, battVCalValid;
  SystemState state;
  bool  woActive;
  uint32_t pushSeq;     // the newest fbRequestPush() this snapshot was taken after (see fbPushSeq)
  bool  valid;
};
TelemetrySnapshot telem = {};
portMUX_TYPE telemMux = portMUX_INITIALIZER_UNLOCKED;
unsigned long lastTelemCollectMs = 0;

/* ---- Web event ring (dashboard "Recent ESP1 events") ---------------------- *
 * The SD log records everything; this keeps only the handful of events an operator actually needs
 * to see remotely (see webWorthy). Bounded and republished inside each snapshot rather than appended
 * to a Firebase list -- an unbounded node is exactly what broke the command queue.
 *
 * Has its OWN spinlock, not telemMux: telemetryCollect() holds telemMux while it copies, and if a
 * log call could ever happen underneath that lock the two would deadlock. Separate locks make that
 * impossible by construction. The critical section is one fixed-size struct copy.               */
const uint8_t WEB_EVT_N = 12;
// detail is 128, not 96: the fertigation PRE receipt reaches 117 chars in the worst case --
// "PRE|COL_A|FERTIGATION|PLANT=" + a 16-char col[].name + WATER/FLUSH/STEPS + N/P/K at 4 digits
// + "|A=500.0|B=500.0|C=500.0". At 96 the doses fell off the end, which is the most useful part of
// the one row the operator actually asked to see. Sized against the worst case, not the typical.
struct WebEvent { char at[20]; char src[8]; char type[12]; char detail[128]; };
WebEvent      webEvt[WEB_EVT_N] = {};
uint8_t       webEvtHead = 0;        // next slot to write
uint8_t       webEvtCount = 0;       // entries filled (saturates at WEB_EVT_N)
portMUX_TYPE  evtMux = portMUX_INITIALIZER_UNLOCKED;
// Raised when an event the operator should not wait for lands (fault / rejection). netTask uses it as
// an extra upload trigger so an error surfaces in seconds instead of at the next 60 s tick.
// A counter, not a flag (2026-11-05). The upload reads the telemetry snapshot, which telemetryCollect()
// refreshes only once a second, so a push sent the moment the flag was seen could carry the state from
// BEFORE the event that asked for it -- and the next regular upload is a minute away. Seen on the host
// build: a held FLOW_FAIL took 45 s to reach the dashboard, an LCD take-over of manual mode 15 s. Core 1
// numbers every request, each snapshot records the newest number already raised when it was taken, and
// netTask pushes only a snapshot that includes the request. A counter also cannot lose a request the way
// netTask clearing a shared flag could, when a new one landed between its check and its clear.
volatile uint32_t fbPushSeq = 0;                     // written by core 1 only
static inline void fbRequestPush() { fbPushSeq++; }  // core 1 only

/* ---- Web manual-mode hold ------------------------------------------------- *
 * While the dashboard's Manual/Test tab is open it holds the rig out of automatic, so a scheduled
 * run cannot start underneath someone with their hands on the relays. Mutual exclusion is strict:
 * nothing may start a run from ANY source while the hold is up -- including the LCD's own Force Run
 * and SMS -- which is only safe because whoever is at the rig can revoke it (UP+DOWN on the banner).
 *
 * ESP1 owns the deadline; the page only proves it is still there. The page writes an increasing
 * `seq` to /irrigation/manual and the lease is refreshed only when that value CHANGES -- so a stale
 * want:true left behind by a closed browser stops refreshing and expires, which a plain boolean
 * would not. 60 s outlives a network hiccup without stranding the rig out of automatic.            */
unsigned long webManualUntilMs = 0;          // 0 = not held; else the millis() deadline
const unsigned long WEB_MANUAL_LEASE_MS = 60000;
int64_t       webManualSeq   = -1;           // last seq seen from the page. Written ONLY by core 0
                                             // (firebasePollManual); core 1 must never reset it, or a
                                             // revoke would look like a fresh request on the next poll.
                                             // MUST be 64-bit: the page sends JS Date.now() (~1.7e12
                                             // today), which does not fit a 32-bit long -- ArduinoJson's
                                             // `| default` idiom silently returns the default instead of
                                             // truncating, so a 32-bit read here comes back -1 forever.
// Latched by a revoke: the page keeps pinging until it notices, and without this the very next poll
// would re-grant the hold within ~3 s -- silently undoing the operator override this feature exists
// for. Cleared only when the page actually stops asking.
bool          webManualBlocked = false;
char          webManualState[10] = "idle";   // idle | held | refused | revoked  (published)
char          webManualWhy[40]   = "";       // why it was refused / revoked
// core0 poll -> core1: what the page is asking for, and whether its keep-alive advanced this poll.
// Core 1 owns every decision; core 0 only reports what it read.
volatile bool webManualReq  = false;
volatile bool webManualPing = false;
static bool webManualHeld() { return webManualUntilMs && (long)(webManualUntilMs - millis()) > 0; }
// LCD side of the hold: the banner owns the screen, UP+DOWN opens a two-item chooser, and "view
// sensors" is a PEEK -- it hides the banner without releasing the hold.
bool wmChooser = false;      // chooser open
uint8_t wmSel  = 0;          // 0 = view sensors, 1 = interrupt web
bool wmPeek    = false;      // operator is paging the data screens while the hold stands
bool lcdInWebManual = false; // banner currently owns the screen (drives the clear-on-enter/exit)

/* ---- Power (battery via opto-isolated ADC, polynomial-calibrated) --------- *
 * Battery V/I now come from GPIO35/34 through an optocoupler (nonlinear), fitted with a least-squares
 * polynomial: value = a0 + a1*u + a2*u^2 + a3*u^3, u = raw/4095. Calibrate with the bench tool
 * Test Code/ESP1_ADC_OPTO_CAL, then PASTE its printed a0..a3 into the defaults below (or populate NVS
 * namespace "adccal"). The tool's NVS record layout is mirrored so a matched-partition tool save also works. */
struct AdcCal { int degree; double coef[4]; };
// TODO: paste a0..a3 from the bench tool. Defaults are PLACEHOLDERS (raw fraction 0..1) until calibrated.
AdcCal calBattV = { 1, { 0.0, 1.0, 0.0, 0.0 } };   // voltage channel (GPIO35)
// Audit fix (2026-09-08): the current channel's opto-ADC polynomial path (calBattI, GPIO34,
// NVS "i" blob) was removed here -- current has come entirely from the ACS758-via-ADS1115 I2C path
// (acsReadVolts(), see below) since that migration, so calBattI was loaded from NVS on boot and then
// never read again by anything. An operator recalibrating "current" via the bench tool would see
// "loaded from NVS" reported with zero actual effect -- a silent no-op that looked like a working
// calibration.
bool   battCalLoaded = false;                       // true once NVS "adccal" overrode the defaults
bool   battVCalLoaded = false;                       // see loadBattCal() and the battery-publish gate below
float battV = 0, battI = 0, battP = 0;
bool  battOptoCal = false;                          // the opto channel holds a real calibration (see loadBattCal)
bool  battFromIna = false;                          // battV/battI came from the main INA226 this tick
bool  battIOk = false;                              // battI is real: from the main INA226, or from the ACS758
// The two INA226 behind the ISO1540. Core 1 only; the snapshot carries copies to core 0.
struct InaMon {
  const char *name; uint8_t addr; uint16_t cal; float lsb, offsetA, busFactor; bool invert;
  float minV;                // a reading below this is a wiring fault, not a measurement
  bool  ok, haveV, noV;      // answering / have a usable reading / below minV (logged once)
  uint8_t failN;
  unsigned long lastTryMs, lastReadMs;
  float v, i, p;
};
enum { INA_SOLAR = 0, INA_MAIN = 1, INA_COUNT = 2 };
InaMon powerIna[INA_COUNT] = {
  // Solar has no minV: 0 V at night is real.
  { "SOLAR", SOLAR_INA_ADDR, inaCalFor(SOLAR_SHUNT_OHM, SOLAR_CURRENT_LSB), SOLAR_CURRENT_LSB, SOLAR_OFFSET_A,
    SOLAR_BUS_FACTOR, SOLAR_INVERT, 0.0f },
  { "MAIN",  MAIN_INA_ADDR,  inaCalFor(MAIN_SHUNT_OHM,  MAIN_CURRENT_LSB),  MAIN_CURRENT_LSB,  MAIN_OFFSET_A,
    MAIN_BUS_FACTOR,  MAIN_INVERT,  SEC_NO_PACK_V },
};
static inline bool inaUsable(const InaMon &m) { return m.ok && m.haveV; }
bool  batteryLow = false, batteryCritical = false;
unsigned long lastInaMs = 0;                         // ADC read throttle (kept name to avoid churn)
// Secondary battery (INA226). Core 1 only; the snapshot carries copies to core 0.
uint16_t secInaCal = SEC_INA_CAL;                  // these four: compiled defaults, NVS "calib" overrides (loadCal)
float    secOffsetA = SEC_CURRENT_OFFSET_A, secBusFactor = SEC_BUS_V_FACTOR;
bool     secInvert = SEC_INVERT_CURRENT;
bool  secInaOk = false, secHaveV = false;          // monitor answering / have a reading
float secV = 0, secI = 0, secP = 0;                // filtered V, signed current, power
// Resting charge (2026-11-17, user: "while charging the level reads higher; the real % shows once the system switches
// to the 2nd battery"). The pack reads HIGH while the charger pushes current in and LOW while the boards draw from it,
// by I x the pack's resistance. secRestV = secV + secI x secRint (+ = discharging) is the voltage it would rest at; the %
// (LCD B2, dashboard) and the 30 % block are worked out from it. secRint is LEARNED at every switch to the secondary:
// the last reading on main (charging) and the first steady one on the secondary (discharging) are the same charge, so
// (V before - V after) / (I after - I before) is the resistance (NVS "secR"). Until then SEC_RINT_DEFAULT.
const float SEC_RINT_DEFAULT = 0.25f, SEC_RINT_MIN = 0.03f, SEC_RINT_MAX = 1.5f;   // ohm               [MEASURE]
float secRint = SEC_RINT_DEFAULT;
float secRestV = 0.0f;
bool  secRintDirty = false;                        // learned on the secondary: saved once back on main
// Secondary battery charge %, linear between SEC_BATT_EMPTY_V and SEC_BATT_FULL_V (the LCD B2 row and Firebase).
static int secBattPct(float v) {
  float p = (v - SEC_BATT_EMPTY_V) * 100.0f / (SEC_BATT_FULL_V - SEC_BATT_EMPTY_V);
  if (!(p > 0.0f)) p = 0.0f;
  if (p > 100.0f) p = 100.0f;
  return (int)roundf(p);
}
bool  secLow = false, secCrit = false;             // low latch (hysteresis) / critical
bool  secNoPack = false;                           // the chip answers but sees no pack: wiring or no common ground
BkState bkState = BK_IDLE;                         // secondary-as-backup state (see backupTick)
uint8_t tankDosedMask = 0;                         // ESP2 reports a dosed tank left for these columns (INFO,TANK,DOSED)
bool  bswEstopRestore = false;                     // boot: an E-stop's own cut reset us, so go back into E-stop
unsigned long bswCutMs = 0;                        // a stop was journaled (bswNoteCut); cleared once we survive it
char  bswCmdSaved[48] = "";                        // dashboard command id on flash while the boards can be reset
bool  esp2OnSecondary = false;                     // ESP2 reported INFO,BATT,SECONDARY (boards isolated)
unsigned long esp2BattWaitUntil = 0;               // ESP2 power-off held for the secondary->main return
uint8_t  bswPendingDir = 0;        // ESP2 announced a P0 switch not yet confirmed (1 to secondary, 2 to main)
unsigned long bswPendingMs = 0;
// "Did anything reboot in the switch?" (2026-11-16, see INFO,BATT,VERIFY). ESP2 only turns the inverter on
// after BATTSW,OK: ESP1 still running since it said GO, and the Nano answered after the switch without a BOOT.
unsigned long bswGoMs = 0;         // when this boot answered BATTSW,GO (~ when P0 moved); 0 = never this boot
uint8_t       bswGoDir = 0;        // its direction (1 to secondary, 2 to main)
unsigned long bswVerifyStartMs = 0;  // ESP2 asked INFO,BATT,VERIFY (0 = no verification running)
const char   *bswVerdict = nullptr;  // "OK" or the reason; re-sent on every repeat of VERIFY
unsigned long nanoBootMs = 0;      // last STATUS,NANO,BOOT (the Nano restarted)
// 2026-11-16 (run buffers): 1.5 s -> 3 s. The Nano may be inside one NPK read (NPK_RESPONSE_TIMEOUT_MS = 1 s) when
// FAST arrives, then needs two pings and ~50 ms of link: ~1.3 s worst case left 0.2 s of margin, and a miss ends the
// pump start as BATT_SWITCH_FAIL|NANO_SILENT. A real Nano reset is still caught by its BOOT line (NANO_RESET).
// ESP2's BATT_VERIFY_MS (5 s) must stay longer than this.
const unsigned long BSW_VERIFY_NANO_MS      = 3000;    // the Nano must answer FAST within this after VERIFY
const unsigned long BSW_NANO_BOOT_WINDOW_MS = 30000;   // a Nano BOOT this soon after a switch = a switch reset
volatile unsigned long battQuietUntilMs = 0;   // quiet window end (core 1 writes, core 0 reads; 0 = none)
bool     bswLatched = false;       // battery-switch fault latched: no inverter use until cleared
uint8_t  bswStrikes = 0;           // switch-caused resets in the current window
uint32_t bswLastStrikeT = 0;       // RTC unix time of the last strike (0 = unknown)
unsigned long bswLastStrikeMs = 0; // millis() of the last strike counted THIS boot (report dedupe)
bool     bswBootStruck = false;    // this boot already counted ESP1's own switch reset: ESP2's report of
                                   // the same event (it was cut by our boot) must not count again
// A run was lost in a way that can leave ESP2's saved tank volume too low, so the next fill could overfill.
// No run starts until the operator has looked at the tank and confirmed. Kept in NVS across reboots.
volatile bool tankCheckNeeded = false;
// For the website (18 Nov 2026): why the tank check was raised ("" = carried over a restart), this boot's reset
// reason, and how many times ESP1 has started today (NVS "bootDay"/"bootN", counted once the date is known).
char tankCheckWhy[40] = "";
const char *bootReasonStr = "OTHER";
uint16_t bootsToday = 0;
static inline bool battQuietActive() {
  unsigned long u = battQuietUntilMs;
  return u != 0 && (long)(millis() - u) < 0;
}
// Energy integration for the daily report (sec.12.1.3). See powerTick: discharge only with the shunt bar,
// every amp on the ACS758 fallback, which is not trusted for direction.
double energyConsumedWh = 0.0;   // energy today (Wh)
double energyChargedWh  = 0.0;   // solar energy today (Wh), from the solar INA226
bool   prevBattStateLow = false, prevBattStateCrit = false;  // PWR-log-on-change (B3)

/* ---- Daily counters (for report) ----------------------------------------- */
uint16_t faultsToday[3] = { 0, 0, 0 };   // CRIT, MAJ, MIN
uint16_t nanoResetsToday = 0;
float    waterUsedToday[NUM_COLUMNS]    = { 0 };
float    nutrientUsedToday[NUM_COLUMNS] = { 0 };
// Runs that reached each column today, and how many of them added nutrients (2026-11-06, asked for by the project
// owner for the website's column cards). Kept in NVS with the litres and mL above and the day they belong to, so a
// restart in the middle of the day no longer zeroes today's totals (the STATUS SMS reads them too).
uint8_t  wateringsToday[NUM_COLUMNS] = { 0 };
uint8_t  feedingsToday[NUM_COLUMNS]  = { 0 };
bool     reportSentToday = false;
bool     nanoDailyResetDone = false;
long     currentDayStamp = -1;

// NVS: "tdd" = the day stored; per column "c<n>tw" litres, "c<n>tn" nutrient mL, "c<n>tr" runs, "c<n>tf" feedings.
// Written once per finished run, so a few times a day.
static void saveDayTotals() {
  if (currentDayStamp < 0) return;                 // no clock: today unknown, nothing worth keeping
  prefs.putLong("tdd", currentDayStamp);
  for (int c = 0; c < NUM_COLUMNS; c++) {
    String k = String("c") + c;
    prefs.putFloat((k + "tw").c_str(), waterUsedToday[c]);
    prefs.putFloat((k + "tn").c_str(), nutrientUsedToday[c]);
    prefs.putUChar((k + "tr").c_str(), wateringsToday[c]);
    prefs.putUChar((k + "tf").c_str(), feedingsToday[c]);
  }
}
// After a restart: today's totals come back if they were saved today; anything older stays zero.
static void restoreDayTotals(long day) {
  if (day < 0 || prefs.getLong("tdd", -1) != day) return;
  for (int c = 0; c < NUM_COLUMNS; c++) {
    String k = String("c") + c;
    waterUsedToday[c]    = prefs.getFloat((k + "tw").c_str(), 0.0f);
    nutrientUsedToday[c] = prefs.getFloat((k + "tn").c_str(), 0.0f);
    wateringsToday[c]    = prefs.getUChar((k + "tr").c_str(), 0);
    feedingsToday[c]     = prefs.getUChar((k + "tf").c_str(), 0);
  }
}

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
int      sumPos = 0;                        // where the next page starts in sumReport (pages end at line breaks)

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
// Why esp2Held most recently cleared -- set at each of the four clear sites (confirmed RESUME ACK,
// EMERGENCY_STOP, cancelRun, the ESP2-restarted INVALID,NOT_HELD path). esp2Held itself only says
// held/not-held; the website's fault banner otherwise can't tell "the rig confirmed recovery" from
// "the safety circuit breaker gave up" from "an operator cancelled it" -- they all just look like the
// banner disappearing. One of: "resumed" / "auto_cancelled" / "cancelled" / "estop" / "esp2_restarted".
String    lastHeldClearReason = "";

/* ---- Settings / Testing UI (MODE button, spec sec.18.10) ----------------- */
enum UiMode { UI_DATA, UI_MENU, UI_EDIT, UI_TEST, UI_CAL, UI_DIAG };
UiMode    uiMode = UI_DATA;
// Top-level Settings menu rows
/* ---- Actuation lockout (operator "disable actuations") -------------------- *
 * Distinct from lcdLocked (which only gates the screen) and from EMERGENCY_STOP (which is a fault
 * response with its own recovery prompt). This is a standing operator decision: keep MONITORING the
 * farm -- sensors, logging, telemetry, SMS, uploads -- but never energize anything.
 *
 * Persisted, so a crash-and-reboot cannot silently re-arm the pumps while nobody is watching. That
 * is the safe default, but it means a bug here could strand the rig -- which is why
 * ENABLE_ACTUATIONS is reachable from a deliberately wider gate than every other remote command,
 * and why the LCD grows the SET_ENABLE row below.                                                  */
bool actuationsDisabled = false;
void setActuationsEnabled(bool on, const char *why);

// SET_ENABLE is FIRST and exists ONLY while actuationsDisabled -- so the moment an operator opens
// Settings on a locked-out rig, "Enable Actuations" is the row already under the cursor. The LCD can
// only RE-ENABLE, never disable: a stray button press must not be able to take the farm offline, so
// disabling stays a deliberate web/SMS action. See setRowVisible/setRowAt.
enum SetItem { SET_ENABLE, SET_CLOCK, SET_SCHEDULE, SET_COLMODE, SET_PRESET, SET_THRESH, SET_ACVOLT, SET_ACAMP, SET_EXERCISE, SET_FORCE, SET_CALIB, SET_DIAG, SET_TESTING, SET_WIFI, SET_SOFTAP, SET_RESTORE, SET_LOCK, SET_RESET, SET_BATTSW, SET_TANKCHK, SET_EXIT, SET_COUNT };
// SET_WIFI and SET_SOFTAP show a live [ON]/[OFF] suffix via setRowLabel(); their base names here are placeholders.
const char *SET_NAMES[SET_COUNT] = { "Enable Actuations", "Set Clock", "Schedule", "Column Mode", "Preset", "Thresholds", "AC Voltage", "AC Current", "Pump Exercise", "Force Run", "Calibration", "Sensor Diag", "Testing", "WiFi", "Setup AP", "Restore Defaults", "Lock Screen", "Reboot ESP1", "Batt Switch", "TankCheck >confirm", "Exit" };
// Row visibility + position<->item mapping, so the windowed 3-row render and the UP/DOWN wrap both
// operate on what is actually on screen rather than on the raw enum.
static bool setRowVisible(int i)  {
  if (i == SET_ENABLE) return actuationsDisabled;
  // 2026-11-16 (user): the switch's OK status lives on the LCD Power page only. The row shows up here just when a
  // latched switch fault needs clearing -- the one action it carries.
  if (i == SET_BATTSW) return bswLatched;
  if (i == SET_TANKCHK) return tankCheckNeeded;                       // only when there is something to confirm
  return true;
}
static int  setRowCount()         { int k = 0; for (int i = 0; i < SET_COUNT; i++) if (setRowVisible(i)) k++; return k; }
static int  setRowAt(int pos) {                       // visible position -> SetItem
  for (int i = 0, k = 0; i < SET_COUNT; i++) if (setRowVisible(i)) { if (k == pos) return i; k++; }
  return SET_EXIT;
}
static int  setPosOf(int item) {                      // SetItem -> visible position
  int k = 0;
  for (int i = 0; i < SET_COUNT; i++) { if (i == item) return k; if (setRowVisible(i)) k++; }
  return 0;
}
int  setSel    = 0;          // selected settings row
int  editItem  = -1;         // SetItem currently being edited
int  editField = 0;          // field index within the editor
int  editCol   = 0;          // column index for per-column editors
int  editTmp[6] = { 0 };     // working copy of the field values being edited

/* ---- Settings > Force Run (operator-initiated irrigation/fertigation) ------ *
 * Its own state rather than editTmp[6]: A-then-B needs eleven fields. Values are held as integers
 * so the existing clampi()/delta editor convention applies unchanged (litres are x10).
 * The armed run outlives the menu -- the operator sets it up, confirms, and walks away while a
 * countdown runs on the data screen.                                                              */
struct ForceSeq {
  uint16_t dL;        // litres x10   (5..200 = 0.5..20.0 L)
  uint16_t ml[3];     // nutrient A/B/C mL (0..FORCE_MAX_DOSE_ML)
  uint16_t delayS;    // start delay 20..300 s
  // Column mask, resolved at ARM time rather than derived from fcMode at fire time. The LCD editor
  // only offers A/B/AB/A-then-B, but a dashboard FORCE_RUN can name any column including C, and
  // both now share this one countdown. Storing it per slot also lets A-then-B carry a different
  // mask in each slot without special-casing the fire path.
  uint8_t  mask;
};
ForceSeq fcSeq[2] = { { 20, {0,0,0}, 30, 0 }, { 20, {0,0,0}, 30, 0 } };  // slot0 = A/B/AB, slot1 = B (A-then-B)
uint8_t  fcMode  = 0;        // 0=A  1=B  2=AB-together  3=A-then-B
uint8_t  fcField = 0;        // editor cursor (0 = mode, then 5 per active slot)
const char *FC_MODE_NAMES[4] = { "Col A", "Col B", "A+B together", "A then B" };
// The columns a Force Run mode waters (bit 0 = A, bit 1 = B).
static inline uint8_t fcModeColumns(uint8_t mode) { return (mode == 0) ? 0b001 : (mode == 1) ? 0b010 : 0b011; }
// Editor steps. handleButtons is edge-triggered (no hold-repeat), so every press is one step --
// these are coarse on purpose to keep a full entry to a sane number of presses.
const uint16_t FC_STEP_DL = 5, FC_STEP_ML = 25, FC_STEP_DELAY = 10;
const uint16_t FC_DELAY_MIN = 20, FC_DELAY_MAX = 300;

enum ForcePhase { FP_NONE, FP_WAIT, FP_RUNNING };
ForcePhase    fcPhase = FP_NONE;
uint8_t       fcSlot  = 0;         // which slot is counting down / running
// Race fix: the LCD's own Force Run entry used to arm unconditionally, silently overwriting an
// already-armed run (web or LCD) with no notice. commitEditor()'s SET_FORCE case now checks
// fcPhase first and, if blocked, flashes this instead of arming -- see the render block near the
// existing "FORCE RUN ARMED" screen.
bool          forceBlockedFlash   = false;
bool          forceBlockedLow     = false;            // the flash is for a dose on too small a batch
unsigned long forceBlockedUntilMs = 0;
unsigned long fcFireAtMs = 0;      // countdown deadline for FP_WAIT
// A dashboard-initiated force run now arms this same countdown instead of dispatching immediately,
// so the LCD "FORCE RUN ARMED" screen shows it, LCD BACK can abort it, and forceTick() re-checks the
// safety gate at FIRE time. fcCmdId carries the command id through the wait so the dashboard entry
// still resolves; fcFromWeb only labels the source in telemetry.
char          fcCmdId[48] = "";
bool          fcFromWeb   = false;
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
// Remote reboot is deferred, not immediate: loop() waits for fbStatusQueue to drain so the "completed"
// PATCH actually lands before we restart. 0 = no remote reboot pending.
unsigned long rebootReqMs = 0;
const unsigned long REBOOT_FLUSH_WAIT_MS = 10000;   // give up waiting and reboot anyway (NVS guard covers us)
// Second half of the same guard: the command id we rebooted for, loaded from NVS at boot. The core-0
// poller consumes it once; core 1 owns the NVS erase (core 0 must never write NVS).
String fbSkipCmdId = "";
volatile bool fbSkipCmdClear = false;
// "Do nothing for now" from the dashboard fault prompt: a UI snooze only, it changes no rig behaviour.
unsigned long faultAckUntilMs = 0;
const unsigned long FAULT_REPROMPT_MS = 120000;     // 2 minutes, per the operator's request
// Remote SMS config-write deferral while a local edit is open (companion spec §B.3.1).
String pendingCfgSms = "";   // queued config SMS, applied when the local edit exits
// Testing submenu: PCF8575 OUT_* bit -> short name (MUST match ESP2 OUT_* numbering)
// P0 = 2nd-battery select, P5/P11/P12 = spares, P6 = column C valve (rewired 2026-11-15). Nutrient A = P1 and
// nutrient B = P7 since 2026-11-16 (swapped the same day) (user: the P11/P12 relays did not run them). The pumps have no relays: AC on the
// old pump-relay contacts upset every flow sensor; they are picked by the Nano D3 servo and run only through the Fill / Push
// combos. "Inverter": bare P2 relay, allowed by user override (2026-11-15) -- it runs whichever pump the servo is on.
const char *TEST_NAMES[16] = {
  "Batt->2nd", "Nut A", "Inverter", "Col A Vlv", "Col B Vlv", "Spare P5", "Col C Vlv",
  "Nut B", "Mixer", "Spare P11", "Spare P12", "Nut C", "Nut D",
  "pH Up", "pH Down", "Mast Cutoff"
};
int  testSel   = 0;          // selected component row
int  testOnBit = -1;         // which bit is currently ON (-1 = none)
unsigned long testBlockedUntilMs = 0;   // header shows the secondary-battery refusal until this
// Re-arm gap (user, 2026-11-17): once a Testing or Calibration button that ran something is released, the next press
// is ignored for ACT_REARM_MS -- pressing again and again switched the relay bank and the master cut-off (P17) so fast
// that the PCF8575 faulted. The press must be made again after the gap. ESP2 keeps the same gap (TEST/PRIME_REARM_MS).
const unsigned long ACT_REARM_MS = 1500;
unsigned long actRearmUntilMs = 0;
unsigned long actWaitMsgUntilMs = 0;    // the Testing header says "too fast" until this
static inline bool actRearmWaiting() { return (long)(actRearmUntilMs - millis()) > 0; }
static inline void actRearmStart()   { actRearmUntilMs = millis() + ACT_REARM_MS; }
// Hold-to-run buttons (Testing rows, the tank-litres pumps, hold-to-prime): a RELEASE counts only after the button has
// been up for BTN_RELEASE_MS (2026-11-17). Read raw, a one-pass blip -- contact bounce, or the noise a relay coil or the
// inverter makes as it switches -- ended the hold, and the re-arm gap then refused the press still being held.
const unsigned long BTN_RELEASE_MS = 120;
struct HoldBtn { bool held; unsigned long upSinceMs; };
static bool holdBtnRead(HoldBtn &b, uint8_t pin) {
  bool down = (digitalRead(pin) == LOW);
  unsigned long now = millis();
  if (down) { b.held = true; b.upSinceMs = 0; }
  else if (b.held) {
    if (b.upSinceMs == 0) b.upSinceMs = nzMs(now);
    if (now - b.upSinceMs >= BTN_RELEASE_MS) b.held = false;
  }
  return b.held;
}
unsigned long testUseComboUntilMs = 0;  // header: an older ESP2 refused the bare Inverter row (use Fill / Push)
// Header: a Testing servo/Fill/Push step failed (SERVO_FAIL,TEST|<why>). raiseFault() is silent in TEST_MODE,
// so without this the operator saw nothing at all when the selector did not move.
unsigned long testServoFailUntilMs = 0;
bool          testServoFailAcLive  = false;   // the reason was AC still live (not the Nano link)
bool          testBattSwitchFail   = false;   // it was the battery switch (BATT_SWITCH_FAIL), not the servo
bool          testInverterNoAc     = false;   // the inverter gave no voltage within 6 s (INVERTER_FAIL)
// Testing rows: 16 single relays (0..15) + Fill combo (16) + one Push>Col combo per column (17+c)
// + the two pump-selector servo rows (20 = Servo>Transfer, 21 = Servo>Booster; 2026-11-15). These extra
// indices double as the TEST,HOLD value; ESP2 treats idx>15 as a combo. The servo rows only MOVE the
// selector (ESP2 runs the inverter-off/dead-check path and leaves the inverter OFF) and answer
// TEST,SERVO,<pump>,OK. The Mixer combo (TEST_ROWS = 22) is dashboard-only, not on the LCD list.
// MUST match ESP2's testComboOn() and pure_math.h's pulseRowForTarget().
const int TEST_FILL_ROW = 16;
const int TEST_SERVO_L_ROW = 17 + NUM_COLUMNS;    // = 20: servo to the transfer pump (LEFT)
const int TEST_SERVO_R_ROW = 18 + NUM_COLUMNS;    // = 21: servo to the booster pump (RIGHT)
const int TEST_ROWS = 16 + 1 + NUM_COLUMNS + 2;    // = 22
int  testServoDoneRow = -1;    // servo row whose move ESP2 confirmed (TEST,SERVO,..,OK) -- LCD [OK], pulse result

/* ---- Calibration UI (companion spec §A) ---------------------------------- */
enum CalKind { CK_SOIL, CK_PH, CK_EC, CK_ACS, CK_LEVEL_RES, CK_LEVEL_MIX, CK_FLOW, CK_OFFSET, CK_NPK, CK_FLOW_RES,
               CK_VOL_MIX, CK_VOL_RES,     // web v.4: "level -> litres" points per tank (shared/tank_volume.h)
               CK_RATE,                    // 2026-11-17: a pump's constant flow rate, measured by hand and typed in
               CK_PEST };                  // 2026-11-17: run a water pump for N s, weigh it, >= 5 tries -> its line
struct CalTgt { const char *name; const char *id; uint8_t kind; int col; };
const CalTgt CAL_TGTS[] = {
  // One row per capacitive probe (2026-11-16): WET (in water) is locked first, then DRY. The probe number is
  // the id's last digit; the Nano streams that single channel (SOIL_A1 = pin A0 ... SOIL_C2 = pin A7).
  { "Soil A1 pin A0", "SOIL_A1",  CK_SOIL,      0 },
  { "Soil A2 pin A1", "SOIL_A2",  CK_SOIL,      0 },
  { "Soil B1 pin A2", "SOIL_B1",  CK_SOIL,      1 },
  { "Soil B2 pin A3", "SOIL_B2",  CK_SOIL,      1 },
  { "Soil C1 pin A6", "SOIL_C1",  CK_SOIL,      2 },
  { "Soil C2 pin A7", "SOIL_C2",  CK_SOIL,      2 },
  { "pH (7 then 4)", "PH",        CK_PH,       -1 },
  { "EC (0 then std)", "EC",      CK_EC,       -1 },
  { "ACS712 zero", "ACS712",      CK_ACS,      -1 },
  { "Ultra Res empty", "ULTRA_RES", CK_LEVEL_RES, -1 },
  { "Ultra Mix empty", "ULTRA_MIX", CK_LEVEL_MIX, -1 },
  // web v.4 (pure ultrasonic): lock the water level, type the litres really in the tank. Up to 8 points per tank;
  // the tank's cone shape fills in between. Mix: hold UP = transfer pump in, hold DOWN = booster out to column A.
  { "Tank Mix liters", "ULTRA_MIX", CK_VOL_MIX,   -1 },
  { "Tank Res liters", "ULTRA_RES", CK_VOL_RES,   -1 },
  // 2026-11-17 (user): each pump's constant flow rate, measured by hand (a bucket and a stopwatch) and typed in.
  // ESP2 uses rate x run time as an estimate of its own beside the ultrasonics (water) or the flow meter (doses).
  // col = which pump: 0 transfer, 1 booster (L/min), 2..4 nutrient A..C (mL/min). Nothing streams for these.
  { "Rate Transfer",  "RATE_XFER",  CK_RATE,      0 },
  { "Rate Booster",   "RATE_BOOST", CK_RATE,      1 },
  { "Rate Nut A",     "RATE_NUTA",  CK_RATE,      2 },
  { "Rate Nut B",     "RATE_NUTB",  CK_RATE,      3 },
  { "Rate Nut C",     "RATE_NUTC",  CK_RATE,      4 },
  // 2026-11-17 (user): run the transfer (col 0) or the booster (col 1) for a chosen number of seconds, timed by ESP2
  // from the inverter switch-on, weigh the water and type the grams; at least 5 tries give the pump's line (rate +
  // start litres). Until then the user's own readings of 17 Nov are the line (shared/pump_line.h).
  { "Transfer pump est", "PEST_XFER",  CK_PEST,   0 },
  { "Booster pump est",  "PEST_BOOST", CK_PEST,   1 },
  { "Flow Reservoir", "FLOW",      CK_FLOW_RES,  -1 },   // Nano fill-flow: manual run, scale cal
  { "Temp offset", "DHT_T",       CK_OFFSET,    0 },   // col selects tOff/hOff/lOff (0/1/2)
  { "Hum offset",  "DHT_H",       CK_OFFSET,    1 },
  { "Lux offset",  "LUX",         CK_OFFSET,    2 },
  { "NPK trim A",  "NPK_A",       CK_NPK,       0 },
  { "NPK trim B",  "NPK_B",       CK_NPK,       1 },
  { "NPK trim C",  "NPK_C",       CK_NPK,       2 },
  // ("Flow ResMix" / "Flow MixIrr" removed in web v.4: the main-line meters are no longer used.)
  { "Flow Nut A",  "FLOW_NUTA",   CK_FLOW,      0 },
  { "Flow Nut B",  "FLOW_NUTB",   CK_FLOW,      1 },
  { "Flow Nut C",  "FLOW_NUTC",   CK_FLOW,      2 },
  // 2026-11-16 (owner's request, overriding "Nut D unused"): the three meters ESP2 already counts but that had no
  // LCD calibration, so calKNutD/calKPhUp/calKPhDn (NVS kND/kPU/kPD) could never be set. Same 3-run dead-man flow
  // cal as Nut A-C; ESP2 already accepts CAL_START/PRIME_START/SET_CAL for them. Their K only converts pulses to
  // volume on ESP1 (pulseKForRow: Testing/website pulse results); no automated sequence doses with them.
  { "Flow Nut D",  "FLOW_NUTD",   CK_FLOW,     -1 },
  { "Flow pH Up",  "FLOW_PHUP",   CK_FLOW,     -1 },
  { "Flow pH Down","FLOW_PHDN",   CK_FLOW,     -1 },
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
// Pump rates (CK_RATE, 2026-11-17). Transfer / booster in L/min, kept in NVS "rXfer" / "rBoost"; 0 = not measured
// (ESP2 then meters on the levels alone and uses its default rates for caps and timers). The nutrient rates are
// PUMP_FLOWRATE_MLPM (NVS "pumpA".."pumpC", shared with the bench tools); pumpRateSet = one was really measured.
// Accepted ranges MUST match ESP2 (RATE_LPM_* / RATE_MLPM_*): out of range there refuses the whole SET_CAL.
const float RATE_LPM_MIN  = 0.5f, RATE_LPM_MAX  = 60.0f;
const float RATE_MLPM_MIN = 1.0f, RATE_MLPM_MAX = 5000.0f;
float calRateXfer = 0.0f, calRateBoost = 0.0f;
bool  pumpRateSet[3] = { false, false, false };
float rateSeen[5] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };   // last INFO,PUMP_RATE "seen" per pump (-1 = none yet)
bool  calRatePushOk = false;         // the saved rate reached ESP2 (else it gets it at its next power-up)
// Pump lines (CK_PEST, 2026-11-17): litres = start + rate x seconds from the inverter switch-on. The rate is the Rate
// Transfer / Rate Booster value above; the start litres are kept here (NVS "xStartL" / "bStartL", NAN = not measured).
// Not measured -> the user's readings (plDef*, fitted at boot from shared/pump_line.h).
PlFit plDefXfer  = { false, 0.0f, 0.0f, 0 }, plDefBoost = { false, 0.0f, 0.0f, 0 };
float calXferStartL = NAN, calBoostStartL = NAN;
// The estimate screens' tries and state (see pestButton).
const int     PEST_SECS_MIN = 3, PEST_SECS_MAX = 60;
const uint8_t PEST_MAX_TRIES = 10;
float   pestT[PEST_MAX_TRIES], pestV[PEST_MAX_TRIES];   // seconds from the switch-on, litres weighed
uint8_t pestN = 0;
int     pestSecs = 10, pestCol = 0, pestGrams = 0;
bool    pestRunning = false, pestReseat = false, pestPushOk = false;
unsigned long pestStartMs = 0, pestKeepMs = 0;
unsigned long pestInvOnMs = 0;                 // ESP2's INFO,INV_ON for this run: the counter starts here (0 = not yet)
long    pestRanMs = -1;                        // ESP2's run time from the switch-on (-1 = none yet)
PlFit   pestFit = { false, 0.0f, 0.0f, 0 };
static float pestRate(bool xfer) {
  float typed = xfer ? calRateXfer : calRateBoost;
  if (typed > 0.0f) return typed;
  return xfer ? plDefXfer.rateLpm : plDefBoost.rateLpm;
}
static float pestStart(bool xfer) {
  float s = xfer ? calXferStartL : calBoostStartL;
  if (isfinite(s)) return s;
  return xfer ? plDefXfer.startL : plDefBoost.startL;
}
static String rateNutWire() {        // SET_CAL,NRATE payload: 0 = not measured
  String s;
  for (int i = 0; i < 3; i++) s += String(pumpRateSet[i] ? PUMP_FLOWRATE_MLPM[i] : 0.0f, 1) + (i < 2 ? "," : "");
  return s;
}
static String rateWaterWire() { return String(calRateXfer, 2) + "," + String(calRateBoost, 2); }
// Tank litres capture (CK_VOL_*): the level is locked as the median of the last few streamed raw cm, and only
// when they agree (a moving or rippling surface is refused) -- the same idea as the soil steadiness window.
const int   VOL_CAL_WIN = 7;
const int   VOL_CAL_MIN_SAMPLES = 5;
const float VOL_CAL_STEADY_CM = 0.6f;      // max - min of the window                             [MEASURE]
float volCalRing[VOL_CAL_WIN];
int   volCalN = 0, volCalIdx = 0;
float volCalLockCm = 0;                    // the locked (temperature-corrected) distance
float volCalLockH = 0;                     // its height above the 0 % level
const float VOL_CAL_STEP_L = 0.1f;         // UP/DOWN step when typing the litres (held = x10 after 1.5 s)
unsigned long volCalHeldSinceMs = 0;
int   volCalHeldBtn = -1;                  // the UP/DOWN button pressed in the litres step (auto-repeat)
unsigned long volCalRepeatMs = 0;
bool  volCalPumpWasOn = false;             // Mix target, step 0: a hold-to-run pump is going
static void volCalReset() { volCalN = 0; volCalIdx = 0; }
static void volCalPush(float cm) {
  if (!(cm >= 0.0f)) return;
  volCalRing[volCalIdx] = cm; volCalIdx = (volCalIdx + 1) % VOL_CAL_WIN;
  if (volCalN < VOL_CAL_WIN) volCalN++;
}
// The window's median (what ENTER locks), its spread, and whether it is steady. False with too few samples.
static bool volCalStats(float *med, float *lo, float *hi, bool *steady) {
  if (volCalN < 1) return false;
  float s[VOL_CAL_WIN];
  for (int i = 0; i < volCalN; i++) s[i] = volCalRing[i];
  for (int i = 1; i < volCalN; i++) { float x = s[i]; int j = i - 1; while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; } s[j + 1] = x; }
  *med = (volCalN & 1) ? s[volCalN / 2] : 0.5f * (s[volCalN / 2 - 1] + s[volCalN / 2]);
  *lo = s[0]; *hi = s[volCalN - 1];
  *steady = volCalN >= VOL_CAL_MIN_SAMPLES && (*hi - *lo) <= VOL_CAL_STEADY_CM;
  return true;
}

/* ---- Ultrasonic reading -> control value (web v.4) ------------------------ */
// Speed-of-sound correction with the Nano's air temperature (tank_volume.h). No valid temperature = raw.
static float ultraCorrectCm(float rawCm) {
#if ULTRA_TEMP_COMP
  if (sensor.envValid && sensor.msEnv != 0) return tvTempCorrectCm(rawCm, sensor.temp, ULTRA_TEMP_REF_C);
#endif
  return rawCm;
}
// Usable for CONTROL: a real echo, inside the tank (past the sensor's dead zone, not far beyond the empty
// level), pings that agree (spread within ULTRA_MAX_SPREAD_CM; -1 = an older Nano that does not send it --
// the web v.4 Nano sends -1 only together with a -1 distance), and a geometry that gives litres.
// Why a tank reading is not used for control, for the log (2026-11-17): "ok" when it is.
static const char *ultraWhy(bool invalid, float cm, float spreadCm, float emptyCm, float liters, char st) {
  if (invalid) return "no_reading";
  if (st != '?' && st != 'K') return st == 'E' ? "status_E_no_echo" : st == 'S' ? "status_S_stuck" : st == 'R' ? "status_R_range"
                                   : st == 'F' ? "status_F_few_pings" : "status_X_not_pinging";
  if (!(cm >= ULTRA_MIN_CM)) return "too_close";
  if (cm > emptyCm + ULTRA_MAX_OVER_EMPTY_CM) return "beyond_empty";
  if (spreadCm > ULTRA_MAX_SPREAD_CM) return "pings_spread";
  if (!(liters >= 0.0f)) return "no_litres_geometry";
  return "ok";
}
static bool ultraUsable(bool invalid, float cm, float spreadCm, float emptyCm, float liters) {
  if (invalid || !(cm >= ULTRA_MIN_CM) || cm > emptyCm + ULTRA_MAX_OVER_EMPTY_CM) return false;
  if (spreadCm > ULTRA_MAX_SPREAD_CM) return false;
  return liters >= 0.0f;
}
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
// 0 Env,1 Tank,2 Soil,3 NPK N/P/K raw,4..6 NPK A/B/C full,7 ESP1,8 ESP2 chem,9 ESP2 pwr,10 flow1,11 flow2
// Pages 4..6 replaced the old single "NPK chem" row-per-column page: the 7-in-1 reports moisture,
// temperature, EC, pH, N, P and K, and all seven fit one 20x4 page per column. One page each keeps
// the values in engineering units at a readable size -- the raw registers are on the web page.
const uint8_t DIAG_PAGES = 12;
const uint8_t DIAG_NPK_FIRST = 4;          // first per-column NPK detail page
// A Nano sensor is "stale" after this. Matches the silence alarm: the Nano sends TANK and SOIL at most every
// 120 s by night and ESP1 misses frames, so 90 s blocked the scheduler on a healthy link (decision D5).
const unsigned long NANO_STALE_MS = NANO_SILENCE_TIMEOUT_MS;
float         diagEsp2Raw[DIAG_ESP2_N];
bool          diagEsp2Valid[DIAG_ESP2_N];
unsigned long diagEsp2Ms[DIAG_ESP2_N];
int           diagEsp2Idx = -1;            // index currently CAL-streaming (-1 = not streaming)
unsigned long diagEsp2DwellMs = 0;         // when the current id started streaming
bool          diagEsp2Streaming = false;   // sweep active (idle + ESP2 powered)
// Non-zero while a dashboard-requested sweep is running: the deadline in millis(). ESP2 is normally
// OFF at idle, so a remote sweep must be time-boxed and must hand the power back.
unsigned long diagRemoteUntilMs = 0;
const unsigned long DIAG_REMOTE_DEFAULT_MS = 60000, DIAG_REMOTE_MAX_MS = 120000;
bool diagRemoteStart(unsigned long ms, const char **why = nullptr);
void diagRemoteTick();
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
// Indexed by RunPhase for the published run node -- keep in step with the enum above.
static const char *const RUN_PHASE_NAME[5] = { "idle", "starting", "running", "finishing", "error" };
RunPhase runPhase = RUN_NONE;
struct RunUi {
  bool     fert;
  int      col;
  uint8_t  mask;                    // every column this run waters (bit 0 = A): a FORCE run may water several at once
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
// One-shot per column, same pattern as resLowLatched: calcDose()'s BATCH_LOW abort (dosing silently
// no-ops to water-only) previously had no operator-visible signal beyond a quiet SD log line -- with
// this project's shipped WATER_BUDGET_L/FLUSH_PCT/MIXING_TANK_SAFE_MIN combination it fires on EVERY
// fertigation-eligible run for a column, so an un-latched alert would spam; latched, it fires once
// until the underlying constants are retuned and it actually recovers.
bool dosingBatchLowLatched[NUM_COLUMNS] = { false, false, false };

/* ---- Control-decision trace: log WHY a column is/ isn't serviced (debug the "never irrigates") --- *
 * Logged on change (or re-emitted every 10 min) so the SD log explains itself without spamming.      */
String        ctrlReason[NUM_COLUMNS];
unsigned long ctrlReasonMs[NUM_COLUMNS] = { 0 };

/* ---- Nano interval pacing (A2: ACTIVE / DAY / NIGHT, sent only on change) -- */
enum NanoPace { PACE_NONE, PACE_ACTIVE, PACE_DAY, PACE_NIGHT, PACE_FAST };
NanoPace lastNanoPace = PACE_NONE;

/* ---- Pump-selector servo relay + FAST level watch (2026-11-15) ------------- *
 * The transfer/booster pumps lost their relays: ESP2 selects one with the Nano's D3 servo (LEFT = transfer,
 * RIGHT = booster) while its inverter is OFF, then turns the inverter on. ESP2 cannot reach the Nano, so:
 *   ESP2 SERVO_REQ,<pump>,<seq> -> here -> Nano SERVO,<side>,<seq>,*CRC (resent until OK, or FAIL)
 *   Nano SERVO,<side>,OK,<seq>  -> here -> ESP2 SERVO,<pump>,OK,<seq>
 *   ESP2 PUMP,<pump>,ON .. PUMP,OFF: the Nano runs FAST (TANK ~3 Hz) and every valid TANK is relayed to
 *   ESP2 as LEVEL,<mix%>,<res%> -- ESP2 stops a fill at its overflow limit and holds a blind fill.
 * ESP1 adds its own last line: mix >= MIX_HIGHHIGH_PCT on MIX_HIGHHIGH_COUNT readings while the transfer
 * pump is still on after MIX_HIGHHIGH_GRACE_MS = ESP2 did not stop it -> Critical MIX_OVERFLOW (E-stop,
 * which also cuts ESP2's power and with it the whole relay bank).                                       */
bool          levelWatch = false;          // an AC pump is powered (PUMP,<p>,ON .. PUMP,OFF)
bool          levelWatchTransfer = false;  // ... and it is the transfer pump (filling the mixing tank)
unsigned long levelWatchSinceMs = 0;
// web v.4: ESP2's LEVEL_FEED lease -- relay the levels (and keep the Nano FAST) with no pump on as well, for the
// whole of a run: ESP2 takes a settled baseline before every fill and delivery. 0 = no lease.
unsigned long levelFeedUntilMs = 0;
// Litres the Nano's reservoir flow meter has counted INTO the reservoir since boot (web v.4, 2026-11-16). Sent
// with every LEVEL: ESP2 meters a fill on the reservoir's drop, and water poured in during it would otherwise
// read as "less transferred". Integrated from each TANK's L/min over the gap since the previous TANK.
float resInflowL = 0.0f;
unsigned long resInflowMs = 0;
const unsigned long RES_INFLOW_MAX_GAP_MS = 10000;   // a longer gap is not integrated (unknown flow meanwhile)
const unsigned long LEVEL_FEED_LEASE_MS = 12000;   // ESP2 renews every 4 s
static inline bool levelFeedActive() { return levelFeedUntilMs != 0 && (long)(levelFeedUntilMs - millis()) > 0; }
// Is the LEVEL relay running (a pump on, or a feed lease)? The Nano is then FAST and must not be reset.
static inline bool levelRelayActive() { return levelWatch || levelFeedActive(); }
bool          servoReqActive = false;      // a SERVO_REQ is being carried to the Nano
String        servoReqPump = "", servoReqSeq = "";
unsigned long servoReqStartMs = 0, servoReqSentMs = 0;
// 2026-11-16 (run buffers): the last Nano OK relayed to ESP2. Between it and PUMP,ON (SERVO_POST_MS + the inverter
// confirm, up to INV_CONFIRM_MAX_MS) neither the level relay nor servoReqActive is true, so nanoPaceTick() used to drop
// the Nano back to ACTIVE for those seconds and send FAST again at PUMP,ON. FAST is kept through that bridge now.
unsigned long servoOkRelayMs = 0;
const unsigned long SERVO_TO_PUMP_FAST_MS = 20000;   // > SERVO_POST_MS + INV_CONFIRM_MAX_MS, with margin
// Where the selector is, for the website's pump-selector switch (2026-11-16): the side the Nano last confirmed
// (SERVO,<side>,OK), for any move -- a run's, a Testing row's or the website's. NVS "svside" keeps it over an
// ESP1 restart; the Nano keeps its own copy in EEPROM, and only ESP1 ever sends it a move. It is the commanded
// side the Nano drove the servo to: a lever that did not follow shows up as SERVO_MISMATCH instead.
const uint8_t SERVO_SIDE_NONE = 0, SERVO_SIDE_TRANSFER = 1, SERVO_SIDE_BOOSTER = 2;
uint8_t       servoSideNow = SERVO_SIDE_NONE;
unsigned long servoSideMs  = 0;            // when the Nano confirmed it since this boot (0 = from NVS, or never)
unsigned long lastFastSentMs = 0;          // FAST is a lease on the Nano: renewed every NANO_FAST_RENEW_MS
uint8_t       mixHighHighRun = 0;
unsigned long mixHighHighSinceMs = 0;      // 0 = not over the backstop limit
unsigned long lastFastTankLogMs = 0;
bool          nanoResetDeferred = false;   // a Nano RESET_REQ waits until no pump is running
const unsigned long SERVO_NANO_RETRY_MS  = 1500;   // re-send SERVO to the Nano (lost line / still moving)
const unsigned long SERVO_NANO_GIVEUP_MS = 8000;   // no OK -> tell ESP2 FAIL (its own limit is 10 s)
const unsigned long NANO_FAST_RENEW_MS   = 5000;   // the Nano's lease is 15 s
// 2026-11-16: the steady ACTIVE/DAY/NIGHT pace is re-sent this often even when unchanged. A Nano that reset
// on its own (WDT/brown-out) boots ACTIVE (10 s) and otherwise stayed there until the next DAY/NIGHT switch.
const unsigned long NANO_PACE_REFRESH_MS = 1800000UL;
const unsigned long LEVEL_WATCH_MAX_MS   = 1800000UL;   // a lost PUMP,OFF cannot keep the Nano FAST forever
const unsigned long FAST_TANK_LOG_MS     = 5000;   // TANK log rows while FAST (3 Hz would flood SD + Supabase)
const float         MIX_HIGHHIGH_PCT     = 97.0f;  // above ESP2's 90 % stop                        [TBD]
const uint8_t       MIX_HIGHHIGH_COUNT   = 3;
const unsigned long MIX_HIGHHIGH_GRACE_MS = 3000;

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
// A RESUME was sent and we are waiting for ESP2's own ACK,RESUME (success) or INVALID,NOT_HELD
// (rejected) reply before esp2Held actually clears (see issueRecovery/handleEsp2Response). Clearing
// esp2Held on SEND instead of on CONFIRMATION used to make the rejection path unreachable.
bool resumeAwaitingAck = false;
unsigned long resumeSentMs = 0;
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
static DateTime rtcNow();
static void rtcClockWasSet();
void rtcHealthTick();
void loadConfig();
void loadCal();
void pushAllCalToEsp2();
void pushAcVoltToEsp2();
void setAcVoltWindow(int lo, int hi, const char *src);
void setAcAmpWindow(int min10, int max10, const char *src);
bool setCalPushBlocking(const String &id, const String &payload);
void pestOnDone(const String &arg);
void saveColumn(int c);
void saveSchedule(int c);
void saveColEnable(int c);
void saveThresholds();
void testHoldTick();

void pollNano();
bool classifyAndApply(const String &payload, const String &raw);
void escalateNanoRecovery();
void sendNanoCommand(const char *cmd, bool log = true);
void servoRelayTick();
void levelWatchStop(const char *why);

void pollESP2();
void sendWorkOrder(int c, bool fertigate);
void sendForceWorkOrder(uint8_t mask, float liters, const float doseMl[3], const char *fbCmdId);
void dispatchPendingWorkOrder();
void handleEsp2Response(const String &payload);
bool esp2PowerCycle();
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
void runUiBegin(int c, bool fert, float forceL = -1.0f, const float *forceDose = nullptr, uint8_t mask = 0);
void runUiFinish(bool ok);
void runUiTick();
void runUiAdvance();
void runUiAbort(const char *why);
void runNoteErr(const String &e);
void lcdRenderRun();
static void lcdRow(uint8_t row, const char *s);   // full-width (20-col) padded row writer
void exerciseTick();
void forceTick();
void forceAbort(const char *why);
void i2cBusRecover();

void pollGSM();
void gsmTxTick();
void gsmBegin();
void simHealthCheck();
void handleSms(const String &body);
void sendSMS(const String &msg);                        // reply/notice -> the sender, else the owner
void smsAlert(const String &msg);                       // alert -> the owner AND the second alert number
void smsErr(const char *logCode, const String &msg);    // refused command -> sender; logs SMS_ERR|<logCode>
void smsFault(char tier, const char *code, const String &rawLoc);   // plain-language fault alert
void smsSendPages(const String &title, const String &body);         // multi-SMS text, "TITLE i/N" pages
const int SMS_MAX = 160;                                // one SMS, in GSM-7 septets
static String smsPlain(const String &raw);
static String smsClock();
static String smsDate(long stamp);
static String smsCol(int c);
static String smsCut(const String &s, int maxSeptets);
static String smsFaultText(char tier, const char *code, const String &detail);
void smsEnqueueSplit(const String &msg, const String &to, uint8_t retries);
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
static void calCloseTarget();
void calHoldTick();
void lcdRenderCal();
void enterDiag();
void exitDiag();
void diagTick();
void diagStopStream();
void diagButton(int i);
void lcdRenderDiag();
void sendEsp2(const String &body);
void sendEsp2Quiet(const String &body);
void sendEsp2KeepAlive(const String &body, bool first);
void lcdTick();
void wakeBacklight();

void stateMachineTick();
void controlTick();
bool decideFertigate(int c);
void powerTick();
void secBattTick();
void powerInaTick();
bool secBattBlocksAc(const char **why = nullptr);
const char *secBlockLcd();      // what blocks inverter use, in 20 LCD characters ("" = nothing)
const char *secBlockTag();      // the same as a log / SMS token
void bswBoot();
void bswTick();
void bswVerifyAnswer();
static void bswStrike(uint8_t dir, const char *by);
void bswNoteCut(uint8_t kind, int c, int mode);
void bswCmdJournal();
void backupTick();
bool backupHoldsEsp2();
const char *bkName(BkState s);
void bswForgetPending();
const char *rebootBusyReason();
void tankCheckSet(const char *why);
void tankCheckClear(const char *by);
void tankCheckBoot();
void tankCheckTick();
void bswClear(const char *by);
static void bswSavePending(uint8_t dir);
static void bswStrike(uint8_t dir, const char *by);
void loadBattCal();
void readBatteryAdc();
void batterySafetyTick();
void scheduleTick();
void heartbeatTick();
void nanoPaceTick();
void deviceHealthTick();
void netHealthTick();
void webManualTick();
void webManualRevoke();
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
void cancelRun(int mode, const char *why, bool isAuto = false);

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
  // Only the voltage channel (PIN_BATT_V) is actually read this way; current comes from the
  // ACS758-via-ADS1115 I2C path (see acsReadVolts()), so PIN_BATT_I's attenuation config was
  // removed here as dead setup for a channel nothing reads (audit fix, 2026-09-08).
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATT_V, ADC_11db);
  loadBattCal();                                   // NVS "adccal" overrides the hardcoded defaults if present

  // ---- RTC init ----
  rtcChipOk = rtc.begin();
  if (rtcChipOk) {
    DateTime n = rtc.now();
    rtcTimeLost = rtc.lostPower() || !rtcPlausible(n.year(), n.month(), n.day(), n.hour(), n.minute(), n.second());
    rtcOk = !rtcTimeLost;
    if (rtcTimeLost) {
      // No trusted clock: scheduling stops and logs go to NODATE until it is set (rtcHealthTick alerts).
      Serial.println(F("WARN: RTC lost its time -- backup battery may be dead. Set the clock to resume scheduling."));
      Serial.println(F("ACTION: Replace the DS3231 CR2032 if this repeats, then Settings > Set Clock or the dashboard."));
    }
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
  nanoSerial.begin(NANO_BAUD, SWSERIAL_8N1, NANO_RX_PIN, NANO_TX_PIN, false, NANO_RX_BUFFER, NANO_RX_ISR_BUFFER);
  // 1 KB receive buffer for ESP2's lines (2026-11-17; default 256 B = ~0.27 s at 9600 baud): an SD flush, a GSM
  // exchange or an LCD redraw that stretches one loop pass must not cost a fault line. Kept by esp2ReinitUart().
  esp2Serial.setRxBufferSize(1024);
  esp2Serial.begin(ESP2_BAUD, SERIAL_8N1, ESP2_RX_PIN, ESP2_TX_PIN);
  gsmBegin();                               // opens UART2; the modem itself comes up from loop()

  // ---- sensor snapshot defaults (invalid until first packet) ----
  sensor.envValid = sensor.tankValid = sensor.lightValid = false;
  // rawSoil MUST start at -1, not the zero-init a global gets: 0 is below SOIL_ADC_RAIL_LO, so an
  // uninitialised pair reads as "both probes railed". An NPK packet arriving before the first valid
  // SOIL packet (at boot, or when a SOIL packet is rejected as garbage but the NPK one parses) would
  // then log SOIL_FALLBACK|NPK_ONLY and drive the column from the NPK alone, on probes that are
  // perfectly healthy and simply have not reported yet.
  for (int i = 0; i < NUM_COLUMNS; i++) {
    sensor.soil[i] = -1; sensor.npkValid[i] = false; sensor.npkReason[i] = "INIT";
    sensor.rawSoil[i][0] = -1; sensor.rawSoil[i][1] = -1;
  }
  sensor.lastNanoMs = millis();
  lastEsp2Ms = millis();
  lastPumpUseMs[0] = lastPumpUseMs[1] = lastPumpUseMs[2] = millis();   // exercise clock starts at boot

  logBuf.reserve(1024);
  logEvent("SYS", "STATE", "BOOT_STATE");
  // Why this board restarted (2026-11-17: the log had 14 restarts in the afternoon and no way to tell a flash or a
  // power-up from a crash, a watchdog or a brown-out).
  { esp_reset_reason_t rr = esp_reset_reason();
    const char *why = rr == ESP_RST_POWERON ? "POWER_ON" : rr == ESP_RST_EXT ? "EXT_PIN" : rr == ESP_RST_SW ? "SOFTWARE"
                    : rr == ESP_RST_PANIC ? "PANIC" : rr == ESP_RST_INT_WDT ? "INT_WDT" : rr == ESP_RST_TASK_WDT ? "TASK_WDT"
                    : rr == ESP_RST_WDT ? "WDT" : rr == ESP_RST_BROWNOUT ? "BROWNOUT" : rr == ESP_RST_DEEPSLEEP ? "DEEPSLEEP"
                    : "OTHER";
    logEvent("SYS", "BOOT", String("reason=") + why + "|rr=" + (int)rr);
    bootReasonStr = why; }                                // a string literal: safe to keep the pointer

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
  // 24 KB (28 KB until 18 Nov 2026 h). Its deepest use, through TLS handshakes and the portal, was ~11.5 KB on 18 Nov
  // (FBASE netstk= never under 17,128 free of 28,672), and a task stack comes out of the same byte-addressable heap the
  // network was starving for (see MEMORY for the network).
  xTaskCreatePinnedToCore(netTask, "netTask", 24576, NULL, 1, &netTaskHandle, 0);

  bswBoot();                                // battery-switch latch + was THIS boot caused by a switch?
  wdtSetup();                               // arm task watchdog (core-1 loop only)
  setState(STARTUP_SYNC);
  tankCheckBoot();                          // was a run live when this boot happened? (after bswBoot)
  if (bswEstopRestore) {                    // an operator or critical stop must not end because it reset us
    logEvent("ESP1", "PWR2", "BATTSW|ESTOP_RESTORED");
    smsFault('C', "ESTOP_KEPT", "");
    enterEmergencyStop(false);
  }
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
  g_lastStage = 'g'; gsmTxTick();          // bring-up, sends, stored SMS, health queries, watchdog

  g_lastStage = 'y'; rtcHealthTick();      // clock trust: reject bad reads, alert when it needs setting

  g_lastStage = 'M'; stateMachineTick();
  g_lastStage = 'C'; controlTick();
  g_lastStage = 'X'; exerciseTick();       // preventive pump exercise (sec.14.9.1)
  g_lastStage = 'f'; forceTick();          // Settings force run: countdown -> gate re-check -> fire
  g_lastStage = 'i'; powerInaTick();       // solar + main battery INA226 behind the ISO1540
  g_lastStage = 'P'; powerTick();          // main battery: main INA226, else opto ADC + ACS758
  g_lastStage = 'b'; secBattTick();        // secondary battery: INA226 I2C read + run-block policy
  g_lastStage = 'w'; bswTick();            // battery-switch journal expiry + 24 h auto re-arm
  tankCheckTick();                         // journal whether a run is live, for tankCheckBoot()
  g_lastStage = 'b'; backupTick();         // secondary as backup for a critical main (compiled out at 0)
  g_lastStage = 'D'; scheduleTick();
  g_lastStage = 'H'; heartbeatTick();
  g_lastStage = 'p'; esp2PowerTick();      // ESP2 min-on hold + periodic idle heartbeat poll (sec.18.8)
  g_lastStage = 'r'; esp2CommRetryTick();  // gentle 20-min retry while ESP2_COMM_LOST is latched (sec.18.9)
  g_lastStage = 'v'; servoRelayTick();     // ESP2 SERVO_REQ -> Nano pump selector; overflow backstop
  g_lastStage = 'n'; nanoPaceTick();       // drive Nano ACTIVE/DAY/NIGHT/FAST interval (A2)
  g_lastStage = 'd'; deviceHealthTick();   // device-loss watchdog -> daily self-reset (Part C)
  g_lastStage = 'h'; netHealthTick();      // netTask liveness: core 0 has no task watchdog
  g_lastStage = 'w'; webManualTick();      // grant/expire the dashboard's manual-mode hold
  g_lastStage = 'w'; telemetryCollect();   // snapshot for the WiFi/ThingSpeak uplink task (Part A)
  g_lastStage = 'U'; summaryTick();        // incremental SD parse for SUMMARY (bounded)

  g_lastStage = 'B'; handleButtons();
  g_lastStage = 'T'; testHoldTick();
  g_lastStage = 'u'; pulseTick();          // remote push-to-column pulse (bounded TEST,HOLD stream)
  g_lastStage = 'C'; calHoldTick();        // flow-cal dead-man (prime pump while ENTER held)
  g_lastStage = 'D'; diagTick();           // Sensor Diag: round-robin CAL sweep of ESP2 sensors
  g_lastStage = 'S'; diagRemoteTick();     // close a dashboard-requested sweep + hand ESP2 back
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
  // cleanly (config persists in NVS). NOTE: setup() drives ESP2_PWR_PIN LOW, so an ESP1 reboot also cuts
  // ESP2's power, which is why a manual reboot is refused while a run, test or held fault is live
  // (decision D12). Stop or cancel it first. Checked again here: the portal asks from the other core.
  if (rebootPending) {
    rebootPending = false;
    const char *busy = rebootBusyReason();
    if (busy) {
      logEvent("ESP1", "RESET", String("MANUAL|REFUSED|") + busy);
    } else {
      logEvent("ESP1", "RESET", "MANUAL|REBOOT");
      logFlush(true); delay(60);
      bswForgetPending(); ESP.restart();
    }
  }
  // Remote reboot: hold off until core 0 has PATCHed the "completed" status, so the command cannot
  // still read "queued" after we come back and trigger another reboot. Bounded -- if the link is
  // down the flush will never happen, and the NVS "rbcmd" guard is what stops the loop then.
  // Core 0 consumed the reboot-guard id; erase the NVS key here (all NVS writes stay on core 1).
  if (fbSkipCmdClear) { fbSkipCmdClear = false; prefs.remove("rbcmd"); }
  if (rebootReqMs) {
    bool flushed = (fbStatusQueue == NULL) || (uxQueueMessagesWaiting(fbStatusQueue) == 0);
    if (flushed || millis() - rebootReqMs > REBOOT_FLUSH_WAIT_MS) {
      logEvent("ESP1", "RESET", flushed ? "REMOTE|REBOOT" : "REMOTE|REBOOT|STATUS_UNFLUSHED");
      logFlush(true); delay(60);
      bswForgetPending(); ESP.restart();
    }
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
/* The only way the firmware reads the clock (core 1 only; I2C is not for core 0). An impossible date is
 * refused, and one that is possible but far from where the clock should be is believed only if a second
 * read agrees, because garbled bytes can still form a valid date. A refused read answers with the last
 * good time plus the elapsed millis(), so one bad read cannot look like a new day to the scheduler.
 * No logging in here: logEvent() stamps its rows through this function. rtcHealthTick() reports. */
const uint32_t RTC_JUMP_MAX_S = 120;        // further than this from the expected time needs a second read
const uint8_t  RTC_BAD_LIMIT  = 5;          // consecutive refused reads before the clock stops being trusted
uint32_t      rtcRefUnix = 0;               // last good reading, and when it was taken
unsigned long rtcRefMs = 0;
uint8_t       rtcBadReads = 0;
uint32_t      rtcRejected = 0;              // refused reads since boot
bool          rtcJumpPending = false;       // a real jump was accepted; rtcHealthTick logs it

static inline bool rtcDtPlausible(const DateTime &d) {
  return rtcPlausible(d.year(), d.month(), d.day(), d.hour(), d.minute(), d.second());
}
static DateTime rtcNow() {
  DateTime n = rtc.now();
  bool good = rtcDtPlausible(n);
  uint32_t expected = rtcRefUnix ? rtcRefUnix + (millis() - rtcRefMs) / 1000 : 0;
  if (good && rtcRefUnix && rtcJumped(n.unixtime(), expected, RTC_JUMP_MAX_S)) {
    DateTime again = rtc.now();
    good = rtcDtPlausible(again) && !rtcJumped(again.unixtime(), n.unixtime(), 2);
    if (good) { n = again; rtcJumpPending = true; }
  }
  if (good) {
    rtcRefUnix = n.unixtime(); rtcRefMs = millis(); rtcBadReads = 0;
    return n;
  }
  rtcRejected++;
  if (rtcBadReads < 255) rtcBadReads++;
  return rtcRefUnix ? DateTime(expected) : n;
}
// Called after every rtc.adjust(). adjust() also clears the DS3231's lost-power flag.
static void rtcClockWasSet() {
  rtcTimeLost = false; rtcRefUnix = 0; rtcBadReads = 0;   // a deliberate change is not a jump
  rtcOk = rtcChipOk;
}
// Core 1, ~1 Hz: re-checks the clock even when nothing is logging, and does the reporting rtcNow() cannot.
void rtcHealthTick() {
  static unsigned long lastMs = 0, lastBadLogMs = 0, lastAlertMs = 0;
  static uint32_t loggedRejected = 0;
  static bool bootReported = false;
  if (millis() - lastMs < 1000) return;
  lastMs = millis();

  if (!bootReported) {                       // setup() runs before alerts can go out
    bootReported = true;
    if (!rtcChipOk)        raiseFault('M', "RTC_MISSING", "DS3231|no schedule until fixed");
    else if (rtcTimeLost)  raiseFault('M', "RTC_INVALID", "LOST_TIME|set the clock to resume the schedule");
  }
  if (!rtcChipOk || rtcTimeLost) return;

  rtcNow();
  bool trusted = rtcBadReads < RTC_BAD_LIMIT;
  if (trusted != rtcOk) {                    // set first: the fault below stamps its row through tsString()
    rtcOk = trusted;
    if (trusted) {
      rtcRefUnix = 0;
      logEvent("ESP1", "STATE", "RTC|RECOVERED");
    } else if (!lastAlertMs || millis() - lastAlertMs >= 3600000UL) {   // a flapping bus must not storm SMS
      lastAlertMs = millis();
      raiseFault('M', "RTC_INVALID", "BAD_READS|no schedule until it reads correctly");
    } else {
      logEvent("ESP1", "STATE", "RTC|UNTRUSTED");
    }
  }
  if (rtcJumpPending) {
    rtcJumpPending = false;
    logEvent("ESP1", "STATE", "RTC|JUMP_CONFIRMED|" + tsString());
  }
  if (rtcRejected != loggedRejected && millis() - lastBadLogMs >= 60000UL) {
    lastBadLogMs = millis();
    logEvent("ESP1", "STATE", "RTC|BAD_READS|total=" + String(rtcRejected));
    loggedRejected = rtcRejected;
  }
}

uint16_t minuteOfDay() {
  if (!rtcOk) return 0;
  DateTime now = rtcNow();
  return now.hour() * 60 + now.minute();
}
String tsString() {
  if (!rtcOk) return String("0000-00-00 00:00:00");
  DateTime n = rtcNow();
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
/* ---- Watering windows (2026-11-05): up to MAX_RUNS_PER_DAY a day on MANUAL, one on AUTO ---------------- */
static uint8_t windowCount(int c) {
  if (colSchedMode[c] != SCHED_MANUAL) return 1;
  return (colRuns[c] >= 1 && colRuns[c] <= MAX_RUNS_PER_DAY) ? colRuns[c] : 1;
}
static uint16_t windowStart(int c, int w) {
  if (colSchedMode[c] != SCHED_MANUAL) return DEF_WIN_START[c];
  return w == 0 ? COL_WIN_START[c] : COL_EXTRA_START[c][w - 1];
}
static uint16_t windowEnd(int c, int w) {
  if (colSchedMode[c] != SCHED_MANUAL) return DEF_WIN_END[c];
  return w == 0 ? COL_WIN_END[c] : COL_EXTRA_END[c][w - 1];
}
// The window minute-of-day `mod` falls in, or -1. Windows are inclusive at both ends, as the original check was.
static int windowAt(int c, uint16_t mod) {
  for (int w = 0; w < windowCount(c); w++) if (mod >= windowStart(c, w) && mod <= windowEnd(c, w)) return w;
  return -1;
}
// Windows as {start,end} pairs are valid if each lies in the day, starts before it ends, and ends before the next
// one starts (so no minute belongs to two windows, and a column can't be watered twice in one).
static bool windowsValid(const uint16_t (*win)[2], int n) {
  if (n < 1 || n > MAX_RUNS_PER_DAY) return false;
  for (int w = 0; w < n; w++) {
    if (win[w][0] > 1439 || win[w][1] > 1439 || win[w][0] >= win[w][1]) return false;
    if (w > 0 && win[w - 1][1] >= win[w][0]) return false;
  }
  return true;
}
static bool windowDoneToday(int c, int w) {
  return col[c].lastServicedStamp == currentDayStamp && (col[c].servicedMask & (1 << w));
}
static bool dayDoneToday(int c) {
  return col[c].lastServicedStamp == currentDayStamp && col[c].servicedMask == 0xFF;
}
// Marks window w watered today (w < 0: the whole day, as a STOP reply or a lost fault does). `persist` keeps it
// across a reboot, so a column that finished can't be re-run, or re-dosed, by a restart inside the same window.
static void markServiced(int c, int w, bool persist) {
  if (col[c].lastServicedStamp != currentDayStamp) { col[c].lastServicedStamp = currentDayStamp; col[c].servicedMask = 0; }
  col[c].servicedMask |= (w < 0) ? 0xFF : (uint8_t)(1 << w);
  if (!persist) return;
  prefs.putLong((String("c") + c + "ls").c_str(), col[c].lastServicedStamp);
  prefs.putUChar((String("c") + c + "lm").c_str(), col[c].servicedMask);
}
static uint8_t runsDoneToday(int c) {
  if (col[c].lastServicedStamp != currentDayStamp) return 0;
  if (col[c].servicedMask == 0xFF) return windowCount(c);
  return (uint8_t)__builtin_popcount(col[c].servicedMask & ((1 << MAX_RUNS_PER_DAY) - 1));
}

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
    // Persisted: a reboot inside the window must not re-run (and, for fertigation, re-dose) a column that
    // already finished today. An interrupted run never reaches DONE, so it still re-runs when due.
    col[c].lastServicedStamp = prefs.getLong((String(key) + "ls").c_str(), -1);
    // Which windows of that day are done. 0xFF when absent: firmware before 2026-11-05 saved only the day, and
    // that meant the whole day. The day nutrients last went in, so a reboot can't dose twice in one day.
    col[c].servicedMask = prefs.getUChar((String(key) + "lm").c_str(), 0xFF);
    col[c].fertStamp    = prefs.getLong((String(key) + "fd").c_str(), -1);
    // per-column schedule (manual window + AUTO/MANUAL axis) and enable flag
    COL_WIN_START[c] = prefs.getUShort((String(key) + "ws").c_str(), COL_WIN_START[c]);
    COL_WIN_END[c]   = prefs.getUShort((String(key) + "we").c_str(), COL_WIN_END[c]);
    colSchedMode[c]  = prefs.getUChar((String(key) + "sm").c_str(), colSchedMode[c]);
    colRuns[c]       = prefs.getUChar((String(key) + "r").c_str(), 1);
    for (int x = 0; x < MAX_RUNS_PER_DAY - 1; x++) {
      COL_EXTRA_START[c][x] = prefs.getUShort((String(key) + "x" + (x + 1) + "s").c_str(), COL_EXTRA_START[c][x]);
      COL_EXTRA_END[c][x]   = prefs.getUShort((String(key) + "x" + (x + 1) + "e").c_str(), COL_EXTRA_END[c][x]);
    }
    {   // a stored set that doesn't hold together falls back to the one window, rather than none
      uint16_t win[MAX_RUNS_PER_DAY][2];
      int n = (colRuns[c] >= 1 && colRuns[c] <= MAX_RUNS_PER_DAY) ? colRuns[c] : 0;
      for (int w = 0; w < n; w++) {
        win[w][0] = w == 0 ? COL_WIN_START[c] : COL_EXTRA_START[c][w - 1];
        win[w][1] = w == 0 ? COL_WIN_END[c]   : COL_EXTRA_END[c][w - 1];
      }
      if (!windowsValid(win, n)) colRuns[c] = 1;
    }
    COLUMN_ENABLED[c] = prefs.getBool((String(key) + "en").c_str(), COLUMN_ENABLED[c]);
    // Operation + day plan (2026-11-18). Absent on older NVS: Automatic, no plan -- exactly the old behaviour.
    colOpManual[c] = prefs.getUChar((String(key) + "op").c_str(), 0) ? 1 : 0;
    planClear(colPlan[c]);
    {
      String pk = String(key) + "pl";
      RunPlan stored;
      if (prefs.isKey(pk.c_str()) && prefs.getBytes(pk.c_str(), &stored, sizeof(stored)) == sizeof(stored)) colPlan[c] = stored;
    }
    // Own Soil start / Too wet (18 Nov 2026). Absent on older NVS: -1, the defaults -- exactly the old behaviour.
    colSoilStart[c] = (int8_t)clampi(prefs.getChar((String(key) + "ss").c_str(), -1), -1, 100);
    colSoilWet[c]   = (int8_t)clampi(prefs.getChar((String(key) + "sw").c_str(), -1), -1, 100);
  }
  // global thresholds (settable on-device, sec.10.10.1)
  soilStartPct = prefs.getInt("sstart", soilStartPct);
  fertGap      = prefs.getFloat("fgap", fertGap);
  soilWetPct   = clampi(prefs.getInt("swet", soilWetPct), 0, 100);                               // 18 Nov 2026
  resPausePct  = clampi(prefs.getInt("rpause", resPausePct), (int)RES_LOW_PCT, RES_PAUSE_MAX_PCT); // never under the filter
  { // AC supply window (PZEM): a missing or unusable pair keeps the compiled defaults.
    int lo = prefs.getInt("acvlo", AC_V_DEFAULT_LOW), hi = prefs.getInt("acvhi", AC_V_DEFAULT_HIGH);
    if (acVoltWindowOk(lo, hi)) { acVMin = lo; acVMax = hi; }
  }
  { // AC pump current limits (PZEM), tenths of an amp: same rule.
    int lo = prefs.getInt("acalo", AC_A_DEFAULT_MIN), hi = prefs.getInt("acahi", AC_A_DEFAULT_MAX);
    if (acAmpWindowOk(lo, hi)) { acAMin10 = lo; acAMax10 = hi; }
  }
  { // The pump selector's last confirmed side (website switch): anything but 1/2 = not known.
    uint8_t s = prefs.getUChar("svside", SERVO_SIDE_NONE);
    servoSideNow = (s == SERVO_SIDE_TRANSFER || s == SERVO_SIDE_BOOSTER) ? s : SERVO_SIDE_NONE;
  }
  lcdLocked    = prefs.getBool("lock", false);   // LCD lock persists across reboot (Part D)
  // Deliberately survives a reboot: see the actuationsDisabled comment. A power blip must not
  // re-arm the actuators behind the operator's back.
  actuationsDisabled = prefs.getBool("actdis", false);
  exerciseEnabled    = prefs.getBool("exen", true);
  exerciseRunS       = clampi(prefs.getInt("exrun", exerciseRunS), EX_RUN_MIN, EX_RUN_MAX);
  fbSkipCmdId = prefs.getString("rbcmd", "");     // remote-reboot bootloop guard (consumed once)
  // WiFi + ThingSpeak (Part A/B): creds + write keys persist in NVS.
  wifiEnabled = prefs.getBool("wifien", true);   // WiFi master switch (Settings > WiFi)
  PHONE_NUMBER = prefs.getString("owner", PHONE_NUMBER);   // owner number (portal-editable; SMS gating)
  ALERT_NUMBER2 = prefs.getString("owner2", "");           // second alert recipient (SMS ALERTNUM)
  USSD_CODE     = prefs.getString("ussd", "");             // carrier balance code (SMS USSD)
  adminPin     = prefs.getString("apin", ADMIN_PIN_DEFAULT); // portal admin PIN
  wifiSsid = prefs.getString("wssid", "");
  wifiPass = prefs.getString("wpass", "");
  tsKey1   = prefs.getString("tsk1", "");
  tsKey2   = prefs.getString("tsk2", "");
  tsKey3   = prefs.getString("tsk3", "");
  supaUrl  = prefs.getString("supaurl", "");        // Supabase project URL + service_role key (NVS-only)
  supaKey  = prefs.getString("supakey", "");
  supaLast = prefs.getLong("supalast", 0);          // last day (yyyymmdd) successfully uploaded
  fbLogLast = prefs.getLong("fblogl", 0);           // ... to Firebase (2026-11-17)
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
static void loadPowerCal();

/* ---- Tank "level -> litres" points in NVS (web v.4) ----------------------- *
 * Stored as sensor DISTANCES (cm, temperature-corrected), not heights, so a later "Ultra ... empty"
 * re-capture moves the 0 % mark without moving the points: height = emptyCm - distance is worked out on
 * load and again by tankVolSyncGeom(). Keys: "tvM" (mixing tank), "tvR" (reservoir).                    */
struct TankVolBlob { uint8_t n; float dCm[TV_MAX_PTS]; float L[TV_MAX_PTS]; };
static void tankVolSave(bool mix) {
  const TankVolCal &c = mix ? calMixVol : calResVol;
  TankVolBlob b; memset(&b, 0, sizeof(b));
  b.n = c.n;
  for (uint8_t i = 0; i < c.n && i < TV_MAX_PTS; i++) { b.dCm[i] = c.emptyCm - c.hCm[i]; b.L[i] = c.L[i]; }
  prefsCal.putBytes(mix ? "tvM" : "tvR", &b, sizeof(b));
}
static void tankVolLoadOne(bool mix) {
  TankVolCal &c = mix ? calMixVol : calResVol;
  c.n = 0;
  const char *key = mix ? "tvM" : "tvR";
  if (!prefsCal.isKey(key)) return;
  TankVolBlob b;
  if (prefsCal.getBytes(key, &b, sizeof(b)) != sizeof(b) || b.n > TV_MAX_PTS) return;
  TankVolCal t = c;
  t.n = b.n;
  for (uint8_t i = 0; i < b.n; i++) { t.hCm[i] = t.emptyCm - b.dCm[i]; t.L[i] = b.L[i]; }
  if (tvPointsSane(t)) c = t;
  else Serial.printf("CAL: tank litres points %s in NVS are not usable, using the cone only\n", key);
}
// After the empty/full cm are final (loadCal reads them first).
static void tankVolLoad() {
  tankVolSyncGeom();
  tankVolLoadOne(true);
  tankVolLoadOne(false);
}

void loadCal() {
  prefsCal.begin("calib", false);                 // separate namespace; Restore-Defaults never touches it
  for (int c = 0; c < NUM_COLUMNS; c++) {
    String k = "s" + String(c);
    // "a2"/"w2" keys (bumped from "a"/"w"): a stale pre-remap on-device soil cal no longer overrides the
    // measured compiled defaults -- the correct endpoints apply on flash until a fresh cal is saved.
    int savedAir   = prefsCal.getInt((k + "a2").c_str(), calSoilAir[c]);
    int savedWater = prefsCal.getInt((k + "w2").c_str(), calSoilWater[c]);
    if (soilCalOk(savedAir, savedWater, (int)CAL_MIN_SPAN_ADC)) { calSoilAir[c] = savedAir; calSoilWater[c] = savedWater; }
    // Per-probe endpoints: each probe's own LCD calibration if it has one, else its column's pair.
    for (int p = 0; p < 2; p++) {
      String pk = k + "p" + String(p + 1);
      int d = prefsCal.getInt((pk + "d").c_str(), calSoilAir[c]);
      int w = prefsCal.getInt((pk + "w").c_str(), calSoilWater[c]);
      if (!soilCalOk(d, w, (int)CAL_MIN_SPAN_ADC)) { d = calSoilAir[c]; w = calSoilWater[c]; }
      calProbeDry[c][p] = d; calProbeWet[c][p] = w;
    }
    calNpkOff[c][0] = prefsCal.getFloat((k + "N").c_str(), 0);
    calNpkOff[c][1] = prefsCal.getFloat((k + "P").c_str(), 0);
    calNpkOff[c][2] = prefsCal.getFloat((k + "K").c_str(), 0);
  }
  // Tank geometry: "2" keys (bumped 2026-11-15, see calResEmptyCm). A saved pair is used only if it is
  // sane -- empty clearly farther than full -- otherwise the measured defaults stay.
  { float re = prefsCal.getFloat("reCm2", calResEmptyCm), rf = prefsCal.getFloat("rfCm2", calResFullCm);
    if (re - rf >= CAL_LEVEL_MIN_SPAN_CM) { calResEmptyCm = re; calResFullCm = rf; }
    float me = prefsCal.getFloat("meCm2", calMixEmptyCm), mf = prefsCal.getFloat("mfCm2", calMixFullCm);
    if (me - mf >= CAL_LEVEL_MIN_SPAN_CM) { calMixEmptyCm = me; calMixFullCm = mf; } }
  calFlowResScale = prefsCal.getFloat("frS", calFlowResScale);
  calTempOff = prefsCal.getFloat("tOff", 0);
  calHumOff  = prefsCal.getFloat("hOff", 0);
  calLuxOff  = prefsCal.getFloat("lOff", 0);
  // (kRM / kMI -- the ResMix / MixIrr K-factors -- are no longer read: web v.4 has no main-line meters.)
  tankVolLoad();                                   // web v.4: tank "level -> litres" points (tvM / tvR)
  calKNut[0] = prefsCal.getFloat("kNA", calKNut[0]);
  calKNut[1] = prefsCal.getFloat("kNB", calKNut[1]);
  calKNut[2] = prefsCal.getFloat("kNC", calKNut[2]);
  calKNutD   = prefsCal.getFloat("kND", calKNutD);
  calKPhUp   = prefsCal.getFloat("kPU", calKPhUp);
  calKPhDn   = prefsCal.getFloat("kPD", calKPhDn);
  calPhM = prefsCal.getFloat("phM", calPhM); calPhB = prefsCal.getFloat("phB", calPhB);
  calEcM = prefsCal.getFloat("ecM", calEcM); calEcB = prefsCal.getFloat("ecB", calEcB);
  calAcs712Zero = prefsCal.getFloat("acs", calAcs712Zero);
  loadPowerCal();
}

// One INA226's trims from Calibration/ESP1/INA226. A set that is not believable is ignored whole, so a
// corrupted value leaves the compiled defaults rather than a silently wrong current.
static void loadInaTrim(const char *key, float nominalOhm, float lsb, uint16_t &cal, float &offsetA, float &busFactor, bool &invert) {
  String k(key);
  float ohm = prefsCal.getFloat((k + "O").c_str(), 0.00512f / (cal * lsb));
  float off = prefsCal.getFloat((k + "A").c_str(), offsetA);
  float bus = prefsCal.getFloat((k + "B").c_str(), busFactor);
  if (!inaTrimOk(ohm, nominalOhm, lsb, off, bus)) {
    Serial.printf("CAL: INA226 %s trims in NVS are out of range, using compiled values\n", key);
    return;
  }
  cal = inaCalFor(ohm, lsb); offsetA = off; busFactor = bus;
  invert = prefsCal.getBool((k + "I").c_str(), invert);
}

// Everything the bench tools save for the power side and the dosing pumps. Keys are listed in
// Calibration/README.md, and CalStore can read or clear them.
static void loadPowerCal() {
  InaMon &sol = powerIna[INA_SOLAR], &mn = powerIna[INA_MAIN];
  loadInaTrim("iSol",  SOLAR_SHUNT_OHM, sol.lsb, sol.cal, sol.offsetA, sol.busFactor, sol.invert);
  loadInaTrim("iMain", MAIN_SHUNT_OHM,  mn.lsb,  mn.cal,  mn.offsetA,  mn.busFactor,  mn.invert);
  loadInaTrim("iSec",  SEC_SHUNT_OHM, SEC_CURRENT_LSB, secInaCal, secOffsetA, secBusFactor, secInvert);
  { float r = prefsCal.getFloat("secR", SEC_RINT_DEFAULT);       // the secondary pack's learned resistance
    if (r >= SEC_RINT_MIN && r <= SEC_RINT_MAX) secRint = r; }

  float div  = prefsCal.getFloat("acsDiv",  ACS_DIV);
  float zero = prefsCal.getFloat("acsZero", ACS_ZERO_V);
  float sens = prefsCal.getFloat("acsSens", ACS_SENS_V_PER_A);
  if (calInRange(div, 0.999f, 20.0f) && calInRange(zero, 0.1f, 5.5f) && calInRange(sens, 0.002f, 0.2f)) {
    ACS_DIV = div; ACS_ZERO_V = zero; ACS_SENS_V_PER_A = sens;
  } else Serial.println("CAL: ACS758 values in NVS are out of range, using compiled values");

  const char *PUMP_KEY[3] = { "pumpA", "pumpB", "pumpC" };
  for (int i = 0; i < 3; i++) {
    pumpRateSet[i] = false;
    if (!prefsCal.isKey(PUMP_KEY[i])) continue;    // never measured: the default, and ESP2 doses on its meter alone
    float r = prefsCal.getFloat(PUMP_KEY[i], PUMP_FLOWRATE_MLPM[i]);
    // A divisor in every dose's timed ceiling, so an unusable value must never get this far.
    if (calInRange(r, RATE_MLPM_MIN, RATE_MLPM_MAX)) { PUMP_FLOWRATE_MLPM[i] = r; pumpRateSet[i] = true; }
    else Serial.printf("CAL: %s in NVS is out of range, using %.1f mL/min\n", PUMP_KEY[i], PUMP_FLOWRATE_MLPM[i]);
  }
  // Transfer / booster rates (LCD Calibration > Rate ..., 2026-11-17). A bad stored value = not measured.
  calRateXfer  = prefsCal.isKey("rXfer")  ? prefsCal.getFloat("rXfer", 0.0f)  : 0.0f;
  calRateBoost = prefsCal.isKey("rBoost") ? prefsCal.getFloat("rBoost", 0.0f) : 0.0f;
  if (!calInRange(calRateXfer, RATE_LPM_MIN, RATE_LPM_MAX))  calRateXfer = 0.0f;
  if (!calInRange(calRateBoost, RATE_LPM_MIN, RATE_LPM_MAX)) calRateBoost = 0.0f;
  // Pump lines (2026-11-17): the user's readings are the default; a re-measured start litres replaces it.
  plDefXfer  = plFitReadings(PL_XFER_READINGS, 10);
  plDefBoost = plFitReadings(PL_BOOST_READINGS, 10);
  calXferStartL  = prefsCal.isKey("xStartL") ? prefsCal.getFloat("xStartL", NAN) : NAN;
  calBoostStartL = prefsCal.isKey("bStartL") ? prefsCal.getFloat("bStartL", NAN) : NAN;
  if (!(fabsf(calXferStartL) <= PL_START_L_MAX))  calXferStartL = NAN;
  if (!(fabsf(calBoostStartL) <= PL_START_L_MAX)) calBoostStartL = NAN;
}

// Push every ESP2-owned calibration constant (baseline after any ESP2 reset, §A.5.1 #2).
// Best-effort fire (not block-until-ACK); the work order re-carries job-critical values anyway.
void pushAllCalToEsp2() {
  sendEsp2("SET_CAL,MIXCAP," + String(mixCapLiters(), 2));   // web v.4: litres at the overflow stop
  sendEsp2("SET_CAL,FLOW_NUTA," + String(calKNut[0], 1));
  sendEsp2("SET_CAL,FLOW_NUTB," + String(calKNut[1], 1));
  sendEsp2("SET_CAL,FLOW_NUTC," + String(calKNut[2], 1));
  sendEsp2("SET_CAL,ACS712," + String(calAcs712Zero, 4));
  sendEsp2("SET_CAL,PH," + String(calPhM, 6) + "," + String(calPhB, 4));
  sendEsp2("SET_CAL,EC," + String(calEcM, 6) + "," + String(calEcB, 4));
  sendEsp2("SET_CAL,PRATE," + rateWaterWire());   // the pumps' measured rates (2026-11-17; 0 = not measured)
  sendEsp2("SET_CAL,NRATE," + rateNutWire());
  sendEsp2("SET_CAL,PSTART," + String(pestStart(true), 3));   // the transfer's start litres (2026-11-17)
  pushAcVoltToEsp2();                     // not calibration, but ESP2-enforced the same way (PZEM window)
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

/* Single owner of the actuation lockout, so the LCD row, the Firebase command and any future SMS
 * path cannot drift. Called from core 1 only (it writes NVS). */
void setActuationsEnabled(bool on, const char *why) {
  if (actuationsDisabled == !on) {                       // already in the requested state
    logEvent("ESP1", "ACT", String("ACTUATION|NOCHANGE|") + (on ? "ON" : "OFF") + "|" + why);
    return;
  }
  actuationsDisabled = !on;
  prefs.putBool("actdis", actuationsDisabled);
  logEvent("ESP1", "ACT", String("ACTUATION|") + (on ? "ENABLED" : "DISABLED") + "|" + why);
  sendSMS(on ? "OK: Pumps and valves ENABLED.\nWatering can run again."
             : "OK: Pumps and valves DISABLED.\nNothing waters until enabled again. Sensors, logs and texts keep working.");
  if (!on) {
    // Nothing in flight survives the lockout, or "disabled" would be a lie until the current run
    // finished. The armed force countdown would otherwise fire into a system that has just been
    // told not to actuate; a LIVE run needs the real stop, which cancelRun() already does properly
    // (STOP_ALL to ESP2, run UI closed, forceCmdId/forceMask cleared, dashboard command resolved).
    forceAbort("ACTUATIONS_DISABLED");
    if (wo.active || pendingRun.active) cancelRun(-1, "ACTUATIONS_DISABLED");
    if (uiMode == UI_TEST) settingsLeaveToData(); // LCD Testing drives relays directly: end it (Calibration
                                                  // stays open for its sensors; calHoldTick refuses the prime)
    if (pendingExercise.active) {                 // a pump exercise is an actuation too (cancelRun does not own it)
      sendEsp2("STOP_ALL");
      pendingExercise.active = false; pendingExercise.idx = -1; pendingExercise.sent = false; pendingExercise.acked = false;
      firebaseRemoteExerciseDone("failed", "actuations disabled");
      if (sysState == ACTIVE_STATE) setState(IDLE_STATE);
      esp2SetPower(false);
    }
  }
  // Note: sysState is deliberately NOT touched here. The lockout is reported via stateLabel(), so
  // it cannot be cleared by any of the many setState(IDLE_STATE) sites. See stateLabel().
  wakeBacklight();
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
  prefs.putUChar((String(key) + "r").c_str(), colRuns[c]);
  for (int x = 0; x < MAX_RUNS_PER_DAY - 1; x++) {
    prefs.putUShort((String(key) + "x" + (x + 1) + "s").c_str(), COL_EXTRA_START[c][x]);
    prefs.putUShort((String(key) + "x" + (x + 1) + "e").c_str(), COL_EXTRA_END[c][x]);
  }
}
void saveColEnable(int c) {
  char key[8];
  snprintf(key, sizeof(key), "c%d", c);
  prefs.putBool((String(key) + "en").c_str(), COLUMN_ENABLED[c]);
}
// Operation + day plan (2026-11-18, run_plan.h).
void saveColumnPlan(int c) {
  char key[8];
  snprintf(key, sizeof(key), "c%d", c);
  prefs.putUChar((String(key) + "op").c_str(), colOpManual[c]);
  prefs.putBytes((String(key) + "pl").c_str(), &colPlan[c], sizeof(colPlan[c]));
}
// A column's own Soil start / Too wet (18 Nov 2026), -1 = the default.
void saveColumnThresh(int c) {
  char key[8];
  snprintf(key, sizeof(key), "c%d", c);
  prefs.putChar((String(key) + "ss").c_str(), colSoilStart[c]);
  prefs.putChar((String(key) + "sw").c_str(), colSoilWet[c]);
  // The same warning saveThresholds() gives for the default: under the NPK floor, this column never gets nutrients.
  if (colSoilStart[c] >= 0 && colSoilStart[c] <= NPK_MIN_MOIST_PCT) {
    logEvent("ESP1", "FAULT", String("WARN|THRESH_NO_FERT|col=") + COL_TAG[c] + "|start=" + colSoilStart[c]
                              + "|npkMin=" + NPK_MIN_MOIST_PCT);
    smsAlert(smsFaultText('W', "THRESH_NO_FERT", "Column " + String(COL_TAG[c]) + " start is " + String(colSoilStart[c]) +
                                                 "%, the NPK sensor needs over " + String(NPK_MIN_MOIST_PCT) + "%"));
  }
}
// Today's day number by ESP1's own clock, or -1 while the date isn't known (RTC not set yet / dead).
static long planToday() {
  return currentDayStamp > 0 ? planDayFromYmd(currentDayStamp) : -1;
}
// Whether column c is in Manual today: its plan's mark for today, else its default. With no date known, the
// default alone -- a dead clock already stops every scheduled run (controlTick returns on !rtcOk).
static bool colManualToday(int c) {
  long today = planToday();
  return today < 0 ? colOpManual[c] != 0 : planManualOn(colPlan[c], colOpManual[c] != 0, today);
}

/* Bounds for an operator-supplied nutrient target. The SMS "SET,COL_A,N,150,..." path historically
 * assigned whatever float arrived -- a typo'd negative or a stray extra digit went straight into
 * NVS and then into calcDose()'s gap maths. One shared validator so the SMS and Firebase paths
 * cannot drift, and so neither can write a target the doser cannot honour.       [CONFIRM bounds] */
const float TGT_PPM_MAX = 2000.0f;      // well above any CROP_PRESETS value; catches typos, not use
const float TGT_PH_MIN  = 3.0f, TGT_PH_MAX = 9.0f;

// Pure range check, no mutation -- lets a caller validate every field in a multi-field command
// BEFORE applying any of them, so a late-failing field can't leave earlier ones live-but-unsaved.
static bool columnTargetInRange(const String &keyIn, float v, String &err) {
  String key = keyIn; key.toUpperCase();
  if (key == "PH") {
    if (!(v >= TGT_PH_MIN && v <= TGT_PH_MAX)) { err = "pH out of range 3.0-9.0"; return false; }
    return true;
  }
  if (!(v >= 0.0f && v <= TGT_PPM_MAX)) { err = key + " out of range 0-2000 ppm"; return false; }
  if (key == "N" || key == "P" || key == "K") return true;
  err = "unknown field " + key; return false;
}
// key: "N" | "P" | "K" | "PH" (case-insensitive). Returns false + a reason on a rejected value.
bool applyColumnTarget(int c, const String &keyIn, float v, String &err) {
  if (c < 0 || c >= NUM_COLUMNS) { err = "bad column"; return false; }
  if (!columnTargetInRange(keyIn, v, err)) return false;
  String key = keyIn; key.toUpperCase();
  if      (key == "N")  col[c].targetN  = v;
  else if (key == "P")  col[c].targetP  = v;
  else if (key == "K")  col[c].targetK  = v;
  else if (key == "PH") col[c].targetPH = v;
  return true;
}
void saveExercise() {
  prefs.putBool("exen", exerciseEnabled);
  prefs.putInt("exrun", exerciseRunS);
  logEvent("ESP1", "CFG", String("EXERCISE|") + (exerciseEnabled ? "ON" : "OFF") + "|" + exerciseRunS + "s");
}
void saveThresholds() {
  prefs.putInt("sstart", soilStartPct);
  prefs.putFloat("fgap", fertGap);
  prefs.putInt("swet", soilWetPct);       // 18 Nov 2026: the default Too wet line
  prefs.putInt("rpause", resPausePct);    // 18 Nov 2026: the reservoir pause
  // Warn about a combination that silently disables fertigation. Irrigation only starts below
  // soilStartPct, so if that sits at or under the NPK trust floor the probe will always be too dry
  // to read when the decision is made, and every run downgrades to irrigation-only forever. Not
  // clamped -- irrigation-only operation at a low threshold is a legitimate choice -- but it must
  // not be a silent one. Reaches the LCD Thresholds editor, the THRESH SMS and Restore Defaults,
  // because all three come through here.
  if (soilStartPct <= NPK_MIN_MOIST_PCT) {
    logEvent("ESP1", "FAULT", String("WARN|THRESH_NO_FERT|start=") + soilStartPct
                              + "|npkMin=" + NPK_MIN_MOIST_PCT);
    smsAlert(smsFaultText('W', "THRESH_NO_FERT", "Start is " + String(soilStartPct) + "%, the NPK sensor needs over " +
                                                 String(NPK_MIN_MOIST_PCT) + "%"));
  }
}

// AC supply window (PZEM on ESP2). ESP2 keeps its own copy in NVS, but only what reaches it: so it is
// re-sent at startup sync, on every READY (each power-up), and on every change. Only while ESP2 is
// powered -- a powered-off ESP2 gets it from the READY of its next power-up.
void saveAcVolt() {
  prefs.putInt("acvlo", acVMin);
  prefs.putInt("acvhi", acVMax);
}
// Also carries the AC current limits (SET_CAL,PZEM_I), so every place that refreshes ESP2's voltage window
// (startup sync, each READY, a change) refreshes both.
void pushAcVoltToEsp2() {
  if (!esp2Powered) return;
  sendEsp2("SET_CAL,PZEM_V," + String(acVMin) + "," + String(acVMax));
  sendEsp2("SET_CAL,PZEM_I," + String(acAMin10) + "," + String(acAMax10));
}
// AC pump current limits: the one way they change (LCD editor, website, Restore Defaults).
void setAcAmpWindow(int min10, int max10, const char *src) {
  if (!acAmpWindowOk(min10, max10)) return;
  int oldLo = acAMin10, oldHi = acAMax10;
  acAMin10 = min10; acAMax10 = max10;
  prefs.putInt("acalo", acAMin10);
  prefs.putInt("acahi", acAMax10);
  pushAcVoltToEsp2();
  fbRequestPush();                       // the website shows the new limits without waiting a minute
  char d[48];
  snprintf(d, sizeof(d), "ACAMP|%.1f-%.1f|was=%.1f-%.1f", min10 / 10.0f, max10 / 10.0f, oldLo / 10.0f, oldHi / 10.0f);
  logEvent(src, "CFG", d);
}
// The one way the window changes (LCD editor, website, Restore Defaults). Callers check acVoltWindowOk()
// first; this checks again so a bad pair can never be saved.
void setAcVoltWindow(int lo, int hi, const char *src) {
  if (!acVoltWindowOk(lo, hi)) return;
  int oldLo = acVMin, oldHi = acVMax;
  acVMin = lo; acVMax = hi;
  saveAcVolt();
  pushAcVoltToEsp2();
  fbRequestPush();                       // the website shows the new window without waiting a minute
  logEvent(src, "CFG", String("ACVOLT|") + lo + "-" + hi + "|was=" + oldLo + "-" + oldHi);
}

/* =============================================================================
 *  NANO LINK  --  framed parse, garbage classification (spec sec.18.9.5.0)
 * ========================================================================== */
void pollNano() {
  static String line;
  if (nanoSerial.overflow()) {                     // the loop stalled longer than the receive buffers hold
    nanoRxOverflows++;
    static unsigned long lastOvfLogMs = 0;
    if (millis() - lastOvfLogMs > 10000) { lastOvfLogMs = millis(); logEvent("NANO", "LINK", "RX_OVERFLOW|n=" + String(nanoRxOverflows)); }
  }
  while (nanoSerial.available()) {
    char c = (char)nanoSerial.read();
    if (c == '\n' || c == '\r') {
      if (line.length() > 0) {
        String raw = line;
        line = "";
        // structural: framing
        int s = raw.indexOf(FRAME_START);
        int e = raw.indexOf(FRAME_END);
        bool clean = false, badCrc = false;
        if (s >= 0 && e > s && raw.length() <= 128) {
          String payload = raw.substring(s + 7, e);   // between markers
          payload.trim();
          if (payload.startsWith(",")) payload = payload.substring(1);
          if (payload.endsWith(","))   payload = payload.substring(0, payload.length() - 1);
          // Checksum first: on this software-serial link a flipped bit can still look like a plausible
          // number. A frame with a bad or missing checksum counts as garbage (decision D4).
          size_t bodyLen = 0;
          if (frameCheckCrc(payload.c_str(), payload.length(), &bodyLen)) {
            clean = classifyAndApply(payload.substring(0, bodyLen), raw);
          } else {
            badCrc = true;
          }
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
            String d = "COUNT=" + String(garbageCount) + (badCrc ? "|CRC" : "") + "|RAW=" + raw.substring(0, 64);
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
// the parser a token like "ENV" or "<START>", which String::toFloat() silently turns into 0.0, or a
// byte lost inside a number. Rig logs show the Nano link corrupting 11-28 % of its lines, and a 0 cm
// TANK distance would read as a full reservoir.
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

// The column's NPK moisture as a percent, or -1 when there is no usable reading. Stale counts as
// unusable: a probe that stopped answering must not keep voting with its last value forever.
static int npkMoistPct(int c) {
  if (!sensor.npkValid[c] || sensor.npk[c][0] < 0) return -1;
  if (!sensor.msNpk[c] || millis() - sensor.msNpk[c] > NANO_STALE_MS) return -1;
  int p = (int)(sensor.npk[c][0] + 0.5f);
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return p;
}

/* Recompute one column's blended moisture from the retained capacitive raws plus the current NPK
 * reading, and log whichever exceptional outcome applied. Called from BOTH the SOIL and the NPK
 * packet handlers: the Nano sends SOIL immediately before NPK in the same burst, so blending only at
 * SOIL time would always use the PREVIOUS cycle's NPK. Re-running it when NPK lands keeps
 * sensor.soil[] as fresh as the data allows, with one implementation of the policy. */
static SoilBlend soilRecompute(int c) {
  if (c < 0 || c >= NUM_COLUMNS) return SB_NONE;
  if (sensor.rawSoil[c][0] < 0 && sensor.rawSoil[c][1] < 0) return SB_NONE;   // no capacitive packet yet
  SoilBlend why = SB_CAP_ONLY;
  int npk = npkMoistPct(c);                    // sampled ONCE: the log must report the value actually used
  sensor.soil[c] = soilCombine(c, sensor.rawSoil[c][0], sensor.rawSoil[c][1], npk, &why);
  // Log only the exceptional outcomes, and only on a CHANGE of outcome: these fire per packet, and a
  // permanently-diverged probe would otherwise fill the SD card and the 12-slot web ring.
  static SoilBlend last[NUM_COLUMNS] = { SB_CAP_ONLY, SB_CAP_ONLY, SB_CAP_ONLY };
  if (why != last[c]) {
    last[c] = why;
    if (why == SB_NPK_DIVERGED)
      logEvent("NANO", "SOIL_NPK_DIVERGE", String(COL_TAG[c]) + "|cap=" + sensor.soil[c] + "|npk=" + npk);
    else if (why == SB_NPK_FALLBACK)
      logEvent("NANO", "SOIL_FALLBACK", String("NPK_ONLY|") + COL_TAG[c]);
  }
  return why;
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
    // LCD soil capture: only samples of the probe being calibrated feed its steadiness window.
    if (calRxValid && uiMode == UI_CAL && calOpen && CAL_TGTS[calSel].kind == CK_SOIL && calRxId == CAL_TGTS[calSel].id)
      soilCalPush((int)calRxRaw);
    // LCD tank-litres capture (web v.4): the level window, in temperature-corrected cm.
    if (calRxValid && uiMode == UI_CAL && calOpen && calRxId == CAL_TGTS[calSel].id &&
        (CAL_TGTS[calSel].kind == CK_VOL_MIX || CAL_TGTS[calSel].kind == CK_VOL_RES))
      volCalPush(ultraCorrectCm(calRxRaw));
    sensor.lastNanoMs = millis();
    return true;                              // expected calibration traffic, not garbage
  }
  if (cmd == "ENV" && n == 3) {
    if (!isNumericToken(tok[1]) || !isNumericToken(tok[2])) return false;
    float t = tok[1].toFloat(), h = tok[2].toFloat();
    bool tInv = (tok[1] == "-1"), hInv = (tok[2] == "-1");
    // Exactly 0.0 C / 0.0 % is a failed DHT read (all-zero bits pass its checksum), not weather (2026-11-17 log): not
    // used -- the tank distances are corrected for the air temperature, and 0 C reads every tank ~5 % wrong.
    bool dhtZero = !tInv && !hInv && t == 0.0f && h == 0.0f;
    if (dhtZero) { tInv = true; hInv = true; }
    if (!tInv && (t < TEMP_MIN || t > TEMP_MAX)) return false;   // Tier 2 (raw range)
    if (!hInv && (h < 0 || h > 100)) return false;
    sensor.temp = tInv ? t : t + calTempOff;     // apply calibration offset (§A)
    sensor.hum  = hInv ? h : h + calHumOff;
    sensor.envValid = !(tInv || hInv);
    sensor.rawTemp = t; sensor.rawHum = h; sensor.msEnv = millis();   // Sensor Diag: raw + last-seen
    sensor.lastNanoMs = millis();
    logEvent("NANO", "SENSOR", "ENV|" + tok[1] + "|" + tok[2] + (dhtZero ? "|DHT_ZERO_NOT_USED" : ""));
    return true;
  }
  if (cmd == "TANK" && (n == 4 || n == 6 || n == 8)) {
    // RAW distances in cm (companion spec §A); ESP1 maps to % AND LITRES with the stored geometry (web v.4).
    // web v.4 Nano: TANK,<resCm>,<mixCm>,<flowLpm>,<resSpread>,<mixSpread> -- each cm is the MEDIAN of the
    // Nano's last 5 pings, the spread the width of their middle three (-1 = fewer than 3 good pings). An
    // older Nano sends the first three fields only (spread unknown = not judged).
    if (!isNumericToken(tok[1]) || !isNumericToken(tok[2]) || !isNumericToken(tok[3])) return false;
    if (n >= 6 && (!isNumericToken(tok[4]) || !isNumericToken(tok[5]))) return false;
    // web v.4 link hardening (2026-11-16): TANK,...,<st>,<seq>. <st> = one pinger status letter per tank (K ok,
    // E no echo, R out of range, F too few good pings, S echo line stuck high, X no pings); <seq> = the Nano's TANK
    // counter 0..999 -- a repeat is ignored, a gap counted as frames lost (tankLinkPct).
    char rSt = '?', mSt = '?';
    if (n == 8) {
      if (tok[6].length() != 2 || !strchr(TANK_ST_CHARS, tok[6][0]) || !strchr(TANK_ST_CHARS, tok[6][1])) return false;
      const String &q = tok[7];
      if (q.length() < 1 || q.length() > 3) return false;
      for (unsigned i = 0; i < q.length(); i++) if (!isDigit(q[i])) return false;
      if (!tankSeqAccept(q.toInt())) { sensor.lastNanoMs = millis(); return true; }   // the same frame twice
      rSt = tok[6][0]; mSt = tok[6][1];
    }
    lastTankMs = millis(); tankGapLogged = false;
    float rd = tok[1].toFloat(), md = tok[2].toFloat();
    bool rInv = (tok[1] == "-1"), mInv = (tok[2] == "-1");
    if (!rInv && (rd < 0 || rd > 500)) return false;   // raw distance plausibility (Tier 2)
    if (!mInv && (md < 0 || md > 500)) return false;
    float rs = (n >= 6) ? tok[4].toFloat() : -1.0f, ms = (n >= 6) ? tok[5].toFloat() : -1.0f;
    // Temperature-corrected distances: the % and the litres both come from these, so ESP2's overflow stop and
    // its metering read the same level.
    float rc = rInv ? -1.0f : ultraCorrectCm(rd), mc = mInv ? -1.0f : ultraCorrectCm(md);
    sensor.resLevel = rInv ? -1 : levelPct(rc, calResEmptyCm, calResFullCm);
    sensor.mixLevel = mInv ? -1 : levelPct(mc, calMixEmptyCm, calMixFullCm);
    sensor.resL = rInv ? -1.0f : tvLiters(calResVol, rc);
    sensor.mixL = mInv ? -1.0f : tvLiters(calMixVol, mc);
    sensor.resCm = rc; sensor.mixCm = mc;
    sensor.resSpreadCm = rs; sensor.mixSpreadCm = ms;
    // The Nano's own verdict first: anything but K (or '?' from an older Nano) is not a level to steer by.
    sensor.resSt = rSt; sensor.mixSt = mSt;
    sensor.resOk = ultraUsable(rInv, rc, rs, calResEmptyCm, sensor.resL) && (rSt == '?' || rSt == 'K');
    sensor.mixOk = ultraUsable(mInv, mc, ms, calMixEmptyCm, sensor.mixL) && (mSt == '?' || mSt == 'K');
    sensor.flow = tok[3].toFloat() * calFlowResScale;
    {
      unsigned long nowMs = millis();
      if (tok[3] != "-1" && sensor.flow > 0.0f && resInflowMs != 0 && nowMs - resInflowMs <= RES_INFLOW_MAX_GAP_MS)
        resInflowL += sensor.flow * (float)(nowMs - resInflowMs) / 60000.0f;
      resInflowMs = nowMs;
    }
    sensor.tankValid = !(rInv || mInv);
    sensor.rawResCm = rInv ? -1 : rd; sensor.rawMixCm = mInv ? -1 : md;   // Sensor Diag: raw cm + L/min
    sensor.rawFlow = tok[3].toFloat(); sensor.msTank = millis();
    // A1: clear the reservoir-low latch once the level recovers (with hysteresis).
    if (!rInv && sensor.resLevel >= resPauseLevel() + 5.0f) resLowLatched = false;
    sensor.lastNanoMs = millis();
    // While an AC pump runs, or ESP2 holds a feed lease, relay every reading to ESP2 at once (web v.4: it meters
    // the water with it). A reading not usable for control -- lost echo, pings too scattered (splash, ripple,
    // multipath), a distance outside the tank, an unusable geometry -- goes as -1, which ESP2 treats as "no
    // reading". The line carries a CRC-8: ESP2 drops it otherwise, so a bit flip can never become a level.
    if (levelRelayActive()) {
      // A reading ESP2 will not get (-1 below), and why -- at most every 10 s (2026-11-17: LEVEL_BLIND was untraceable).
      if (!sensor.mixOk || !sensor.resOk) {
        static unsigned long lastWhyMs = 0;
        if (millis() - lastWhyMs >= 10000UL) {
          lastWhyMs = millis();
          logEvent("NANO", "LEVEL", String("NOT_USABLE|mix=") + ultraWhy(mInv, mc, ms, calMixEmptyCm, sensor.mixL, mSt) +
                   "|res=" + ultraWhy(rInv, rc, rs, calResEmptyCm, sensor.resL, rSt) +
                   "|cm=" + String(mc, 1) + "/" + String(rc, 1) + "|s=" + String(ms, 1) + "/" + String(rs, 1));
        }
      }
      float mixOut = sensor.mixOk ? sensor.mixLevel : -1.0f;
      float resOut = sensor.resOk ? sensor.resLevel : -1.0f;
      String body = "LEVEL," + String(mixOut, 1) + "," + String(resOut, 1) + "," +
                    String(sensor.mixOk ? sensor.mixL : -1.0f, 2) + "," + String(sensor.resOk ? sensor.resL : -1.0f, 2) +
                    "," + String(resInflowL, 2) + "," + String(levelSeqOut);
      levelSeqOut = (uint16_t)((levelSeqOut + 1) % 10000);
      char crc[5]; crcFieldFor(body.c_str(), body.length(), crc);
      sendEsp2Quiet(body + crc);
      // ESP1's own backstop on top of ESP2's stop (see servoRelayTick). It judges every real echo, noisy or
      // not -- it is the last line, and three readings in a row at 97 % are not a ripple.
      float mixBack = mInv ? -1.0f : sensor.mixLevel;
      if (levelWatchTransfer && mixBack >= MIX_HIGHHIGH_PCT) {
        if (mixHighHighRun < 255) mixHighHighRun++;
        if (mixHighHighRun >= MIX_HIGHHIGH_COUNT && !mixHighHighSinceMs) mixHighHighSinceMs = nzMs(millis());
      } else if (mixBack >= 0.0f) { mixHighHighRun = 0; mixHighHighSinceMs = 0; }
    }
    // FAST sends ~3 TANK a second: log every FAST_TANK_LOG_MS then, not every packet.
    if (!levelRelayActive() || millis() - lastFastTankLogMs >= FAST_TANK_LOG_MS) {
      lastFastTankLogMs = millis();
      logEvent("NANO", "SENSOR", "TANK|" + tok[1] + "|" + tok[2] + "|" + tok[3] +
               "|L=" + String(sensor.resL, 2) + "/" + String(sensor.mixL, 2) +
               (n >= 6 ? "|s=" + tok[4] + "/" + tok[5] : String("")) +
               (n == 8 ? "|st=" + tok[6] + "|lnk=" + String(tankLinkPct()) + "%" : String("")) +
               ((sensor.resOk && sensor.mixOk) ? String("") : String("|NOT_USABLE")));
    }
    return true;
  }
  if (cmd == "SERVO" && n == 4) {         // pump selector: SERVO,<LEFT|RIGHT>,<MOVING|OK>,<seq>
    if (tok[1] != "LEFT" && tok[1] != "RIGHT") return false;
    if (tok[2] != "MOVING" && tok[2] != "OK") return false;
    sensor.lastNanoMs = millis();
    // Only the OK for the request in flight is passed on -- the side AND the seq must match, so a late
    // OK for an older request can never let ESP2 power the wrong pump.
    if (tok[2] == "OK" && servoReqActive && tok[3] == servoReqSeq &&
        tok[1] == String(servoSideForPump(servoReqPump.c_str()))) {
      sendEsp2("SERVO," + servoReqPump + ",OK," + servoReqSeq);
      logEvent("NANO", "SERVO", tok[1] + "|OK|" + servoReqPump + "|seq=" + servoReqSeq);
      servoReqActive = false;
      servoOkRelayMs = nzMs(millis());       // keep the Nano FAST until PUMP,ON (see SERVO_TO_PUMP_FAST_MS)
    }
    // Any OK, whichever request it answers, means the Nano drove the servo to that side: that is where the
    // selector is now (the website's switch). After ESP2 has its OK; NVS only when the side changes (wear).
    if (tok[2] == "OK") {
      uint8_t side = (tok[1] == "LEFT") ? SERVO_SIDE_TRANSFER : SERVO_SIDE_BOOSTER;
      servoSideMs = nzMs(millis());
      if (side != servoSideNow) { servoSideNow = side; prefs.putUChar("svside", side); }
      fbRequestPush();                     // the switch shows it within seconds, not at the next report
    }
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
      if (!isNumericToken(tok[i + 1]) || !isNumericToken(tok[i + 2])) return false;
      int v1 = tok[i + 1].toInt(), v2 = tok[i + 2].toInt();   // RAW per-probe ADC (companion spec §A)
      if (v1 < 0 || v1 > 1023 || v2 < 0 || v2 > 1023) return false;   // Tier 2: 10-bit ADC range
      rawTmp[c][0] = v1; rawTmp[c][1] = v2;              // keep each raw probe (log + Sensor Diag)
      // Blend is applied below via soilRecompute(), once the raws are stored -- it needs them in
      // sensor.rawSoil[] so the NPK packet can re-run the same combine a moment later.
      tmp[c] = 0;                                        // placeholder; real value set by soilRecompute
      if (abs(v1 - v2) > 400) logEvent("NANO", "SOIL_DIVERGE", String(COL_TAG[c]) + "|" + v1 + "|" + v2);
    }
    for (int c = 0; c < NUM_COLUMNS; c++) {
      sensor.rawSoil[c][0] = rawTmp[c][0]; sensor.rawSoil[c][1] = rawTmp[c][1];
      // Store the raws FIRST, then blend from them -- soilRecompute() reads sensor.rawSoil[], which
      // is also what lets the NPK packet re-run the same combine when its reading lands.
      SoilBlend why = SB_NONE;
      if (rawTmp[c][0] < 0 && rawTmp[c][1] < 0) sensor.soil[c] = -1;   // column absent from the packet
      else why = soilRecompute(c);
      tmp[c] = sensor.soil[c];
      // A column ESP1 has ENABLED but the Nano did not report, OR both its capacitive probes are
      // dead. SB_NPK_FALLBACK still counts as broken hardware even though the NPK is covering the
      // reading -- the whole point of the fallback is to keep watering WHILE the fault is visible,
      // not to hide it. Without this the column just logs SOIL_INVALID in controlTick and never
      // irrigates, with nothing raised (the failure mode when the two COLUMN_ENABLED tables disagree).
      bool broken = (tmp[c] < 0) || (why == SB_NPK_FALLBACK);
      if (COLUMN_ENABLED[c] && broken) {
        if (!soilMissingLatched[c]) {
          soilMissingLatched[c] = true;
          // Diagnostic enrichment (same pattern as holdFault()'s "|pulses=" for FLOW_FAIL): fold the
          // specific broken-reason and the two raw probe values into loc so it reaches the dashboard
          // and SMS, not just the local log -- "both probes read nothing" and "probes disagree, NPK
          // covering" point at different physical checks, and the raw numbers save a Diagnostics-tab
          // round trip.
          const char *reason = (why == SB_NPK_FALLBACK) ? "PROBE_DIVERGE" : "NO_READING";
          String locDetail = String(c == 0 ? "COL_A" : c == 1 ? "COL_B" : "COL_C") + "|" + reason +
                              "|raw=" + String(rawTmp[c][0]) + "," + String(rawTmp[c][1]);
          raiseFault('M', "SOIL_MISSING", locDetail.c_str());
        }
      } else if (!broken && tmp[c] >= 0) soilMissingLatched[c] = false;   // healthy again -> re-arm
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
    if (!isNumericToken(tok[1])) return false;
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
    // Re-blend now that this column's moisture is current. The Nano sends SOIL immediately before
    // NPK in the same burst, so the combine done at SOIL time used the PREVIOUS cycle's NPK -- this
    // is the point at which both sensors are genuinely from the same moment.
    soilRecompute(c);
    String d = "NPK";                     // pipe-delimited, CSV-safe (sec.25.2 -- no commas in detail)
    for (int i = 1; i < n; i++) d += "|" + tok[i];
    logEvent("NANO", "SENSOR", d);
    return true;
  }
  if (cmd == "STATUS" && n >= 3) {        // heartbeat: STATUS,NANO,OK  /  boot notice: STATUS,NANO,BOOT
    if (tok[2] == "BOOT") {               // the Nano restarted (2026-11-16): it boots ACTIVE (10 s)
      nanoBootMs = nzMs(millis());
      lastNanoPace = PACE_NONE;           // so nanoPaceTick sends our pace (or FAST) again right away
      tankSeqLast = -1;                   // its TANK counter restarts at 0: not a gap
      logEvent("NANO", "BOOT", "");
      // Rebooted by a battery switch: a strike, like an ESP1 or ESP2 reset (bswStrike folds duplicates).
      if (bswGoMs && millis() - bswGoMs < BSW_NANO_BOOT_WINDOW_MS) bswStrike(bswGoDir ? bswGoDir : 1, "NANO_RESET");
      return true;                        // not a sign of life from before the reset: lastNanoMs untouched
    }
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
  // Not while an AC pump runs or its selector is being moved: a reset blinds the overflow watch for the
  // Nano's ~2 s boot (ESP2 would then hold a fill as LEVEL_BLIND) and drops a servo move half-way. The
  // Nano never moves the servo at boot, so this is about the level watch, not the switch. Run it after.
  if (!nanoResetReqInFlight && (levelRelayActive() || servoReqActive)) {
    if (!nanoResetDeferred) logEvent("ESP1", "RESET", "NANO|RESET_REQ|DEFERRED_PUMP_RUNNING");
    nanoResetDeferred = true;
    garbageCount = 0;
    return;
  }
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

// Every command carries the same ",*XX" checksum the Nano puts on its own frames (2026-11-15). The Nano
// drops a command whose checksum is wrong, and refuses SERVO -- the one that moves hardware -- without one.
void sendNanoCommand(const char *cmd, bool log) {
  char crc[5];
  crcFieldFor(cmd, strlen(cmd), crc);
  nanoSerial.print(FRAME_START);
  nanoSerial.print(",");
  nanoSerial.print(cmd);
  nanoSerial.print(crc);
  nanoSerial.print(",");
  nanoSerial.println(FRAME_END);
  if (log) logEvent("ESP1", "CMD", String("NANO|") + cmd);
}

// End the level relay (PUMP,OFF, ESP2 restart/power-off, safety timeout). nanoPaceTick then returns the
// Nano to its normal ACTIVE/DAY/NIGHT pace on its next pass.
void levelWatchStop(const char *why) {
  if (levelWatch || servoReqActive) logEvent("ESP1", "PUMP", String("LEVEL_WATCH|OFF|") + why);
  levelWatch = false; levelWatchTransfer = false;
  servoReqActive = false;
  mixHighHighRun = 0; mixHighHighSinceMs = 0;
}

// Carry ESP2's SERVO_REQ to the Nano until it answers OK (classifyAndApply forwards that), or give up and
// tell ESP2 FAIL -- it then holds the run with the inverter never having come on. Also: the overflow
// backstop timer, the level-watch safety timeout, and a Nano reset that was deferred while pumping.
void servoRelayTick() {
  unsigned long now = millis();
  if (servoReqActive) {
    if (now - servoReqStartMs >= SERVO_NANO_GIVEUP_MS) {
      sendEsp2("SERVO," + servoReqPump + ",FAIL," + servoReqSeq);
      logEvent("ESP1", "SERVO", "FAIL|" + servoReqPump + "|seq=" + servoReqSeq + "|NO_NANO_OK");
      servoReqActive = false;
    } else if (servoReqSentMs == 0 || now - servoReqSentMs >= SERVO_NANO_RETRY_MS) {
      bool first = (servoReqSentMs == 0);
      servoReqSentMs = now ? now : 1;
      String c = String("SERVO,") + servoSideForPump(servoReqPump.c_str()) + "," + servoReqSeq;
      sendNanoCommand(c.c_str(), first);           // log the first send only; retries are routine
    }
  }
  if (levelWatch && now - levelWatchSinceMs >= LEVEL_WATCH_MAX_MS) levelWatchStop("TIMEOUT");
  // Backstop: ESP2 stops a fill at its own limit (90 %). Still filling well past it means ESP2 did not.
  if (levelWatch && levelWatchTransfer && mixHighHighSinceMs && now - mixHighHighSinceMs >= MIX_HIGHHIGH_GRACE_MS) {
    String loc = "MIX|pct=" + String(sensor.mixLevel, 0) + "|ESP2_DID_NOT_STOP";
    levelWatchStop("MIX_OVERFLOW");
    // raiseFault() is suppressed entirely in TEST_MODE (a Testing Fill combo), so stop ESP2 directly too;
    // outside Testing the E-stop below sends its own STOP_ALL as well -- a duplicate is harmless.
    sendEsp2("STOP_ALL");
    raiseFault('C', "MIX_OVERFLOW", loc.c_str());   // Critical -> E-stop: STOP_ALL + ESP2 power cut
  }
  if (nanoResetDeferred && !levelRelayActive() && !servoReqActive) {
    nanoResetDeferred = false;
    logEvent("ESP1", "RESET", "NANO|RESET_REQ|RESUMING_DEFERRED");
    escalateNanoRecovery();
  }
}

/* =============================================================================
 *  ESP2 LINK  --  work-order supervisor (spec sec.9.8.1.1)
 * ========================================================================== */
const unsigned int ESP2_LINE_MAX = 256;     // longest ESP2 line accepted (see the frame check below)
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
        // Require both markers, in order, within the accumulation cap -- mirrors pollNano()'s structural
        // gate and the LOCKED rule "never execute a packet unless both START and END markers validate".
        // Liveness bookkeeping above still counts a garbled line as evidence ESP2 is alive; only the
        // dispatch below is gated, so a dropped/garbled frame can no longer be acted on as if it were real.
        // Cap 256 (was 140, 2026-11-17 bug check): a FILL's FLOW_FAIL carries the level diagnosis (moved/level/base/
        // link counts/res+mix) and runs ~150 characters. At 140 it was dropped as garbage, so ESP1 never saw the hold
        // and -- with PROG still refreshing the DONE timeout -- showed the run as live forever. Same cap as ESP2's.
        if (s < 0 || e <= s || raw.length() > ESP2_LINE_MAX) {
          static unsigned long lastEsp2GarbageLogMs = 0;
          if (millis() - lastEsp2GarbageLogMs > 1000) {
            lastEsp2GarbageLogMs = millis();
            logEvent("ESP2", "GARBAGE", "RAW=" + raw.substring(0, 64));
          }
        } else {
          String payload = raw.substring(s + 7, e);
          payload.trim();
          if (payload.startsWith(",")) payload = payload.substring(1);
          if (payload.endsWith(","))   payload = payload.substring(0, payload.length() - 1);
          handleEsp2Response(payload);
        }
      }
    } else if (line.length() < ESP2_LINE_MAX) line += c; else line = "";
  }

  // ACK / DONE timeout supervision (suspended while a fault is HELD -- ESP2 is paused, not hung)
  if (esp2Held) {
    // A RESUME we sent may never get any reply at all (link glitch, ESP2 wedged) -- without this,
    // the fault-hold menu would sit unchanged forever with no sign the button press did anything.
    // Bounded, and deliberately does NOT itself clear esp2Held or resume anything: it only reports
    // that this attempt got no answer, so the operator's next choice is still what actually recovers.
    if (resumeAwaitingAck && millis() - resumeSentMs > RESUME_ACK_TIMEOUT_MS) {
      resumeAwaitingAck = false;
      logEvent("ESP1", "RESP", "RESUME|NO_REPLY");
      smsFault('W', "RESUME_NO_REPLY", "");
      // Reuses the existing lastFaultMsg -> diagnostics.system.lastFault / diagnostics.fault.code
      // plumbing (already rendered on the dashboard's Diagnostics tab) so the website has SOME way
      // to distinguish "still held, last attempt got no reply" from a silently-stuck banner.
      lastFaultMsg = "Resume attempt got no reply - still held";
    }
    return;
  }
  if (wo.active && wo.stage == WO_SENT && millis() - wo.sentMs > UART_ACK_TIMEOUT_MS) {
    if (wo.retries < MAX_UART_RETRY) {
      wo.retries++;
      esp2Serial.println(wo.cmd);
      wo.sentMs = millis();
      logEvent("ESP1", "RESP", "RETRY|" + wo.cmd + "|" + String(wo.retries));
    } else {
      logEvent("ESP1", "RESP", "TIMEOUT|" + wo.cmd);
      wo.active = false; wo.stage = WO_IDLE;
      // A Settings force sequence only advances A->B from the DONE handler, so an abnormal end has
      // to release it explicitly. Without this fcPhase stays FP_RUNNING forever: column B never
      // fires, and the stale non-idle phase blocks the next arm. Same for the DONE timeout below.
      forceAbort("ESP2_TIMEOUT");
      // Tell the dashboard too, or a web-initiated FORCE_RUN sits on "accepted" for good.
      if (forceCmdId[0]) firebaseQueueStatus(forceCmdId, "failed", "ESP2 never acknowledged the work order");
      forceCmdId[0] = '\0'; forceMask = 0;   // a later stray DONE must not be attributed to this run
      esp2SoftResetTried = false;            // fresh ladder: soft reset (RESET_SELF) before power-cycle
      setState(RECOVERY_STATE);              // RECOVERY_STATE tick owns the escalation now
    }
  }
  if (wo.active && wo.stage == WO_ACKED && millis() - wo.sentMs > UART_DONE_TIMEOUT_MS) {
    logEvent("ESP1", "RESP", "TIMEOUT|" + wo.cmd);
    tankCheckSet("ESP2_DONE_TIMEOUT");     // a frozen ESP2 never recorded what it filled, and this cut ends it
    raiseFault('C', "ESP2_DONE_TIMEOUT", "ESP2");
    wo.active = false; wo.stage = WO_IDLE;
    forceAbort("ESP2_NO_DONE");
    if (forceCmdId[0]) firebaseQueueStatus(forceCmdId, "failed", "ESP2 never reported the run finished");
    forceCmdId[0] = '\0'; forceMask = 0;
  }
}

/* ESP2 rebooted under a live job. Same release as the INVALID,NOT_HELD path: the work order is gone,
 * nothing may keep waiting on it. mixTankL survived in ESP2's NVS, so a still-due column re-runs on
 * its own and ESP2 fills only what the tank is missing. If the reboot was not a battery switch, the
 * saved volume may be behind what is really in the tank, so the operator has to check it first. */
static void esp2LostJob(bool tankCheck) {
  int c = wo.colIdx;
  bool hadHold = esp2Held, hadRun = wo.active, hadEx = pendingExercise.active && pendingExercise.sent;
  esp2Held = false; resumeAwaitingAck = false;
  if (hadHold) lastHeldClearReason = "esp2_restarted";
  lastHoldCode = ""; sameHoldN = 0; steerRelease = false;
  wo.active = false; wo.stage = WO_IDLE; wo.colIdx = -1;
  forceAbort("ESP2_RESTARTED");
  if (forceCmdId[0]) { firebaseQueueStatus(forceCmdId, "failed", "ESP2 restarted during the run"); forceCmdId[0] = 0; }
  forceMask = 0;
  if (hadEx) {
    pendingExercise.active = false; pendingExercise.idx = -1; pendingExercise.sent = false;
    firebaseRemoteExerciseDone("failed", "ESP2 restarted during the test");
  }
  if (hadRun || hadHold) runUiAbort("ESP2_RESTARTED");
  String loc = (c >= 0 && c < NUM_COLUMNS) ? String("COL_") + COL_TAG[c] : String(hadEx ? "EXERCISE" : "-");
  logEvent("ESP1", "RESP", String("JOB_LOST|ESP2_RESTARTED|") + loc + (hadHold ? "|WAS_HELD" : ""));
  raiseFault('M', "ESP2_RESTARTED", (loc + (tankCheck ? "|run lost" : "|tank kept, column re-runs when due")).c_str());
  if (tankCheck) tankCheckSet("ESP2_RESTARTED");
}

void handleEsp2Response(const String &payload) {
  // payload e.g. "DONE,SEQ_IRRIGATION_A" or "ACK,..." or "FLOW_FAIL,MAIN"
  int comma = payload.indexOf(',');
  String resp = (comma < 0) ? payload : payload.substring(0, comma);
  String arg  = (comma < 0) ? ""      : payload.substring(comma + 1);

  // web v.4: ESP2 meters its water on the tank levels, and needs them BETWEEN pump runs too (settled
  // baselines before a fill / a delivery). LEVEL_FEED,ON is a lease, renewed every ~4 s; it expires on its
  // own after LEVEL_FEED_LEASE_MS. Not logged per renewal (it would flood the SD log).
  if (resp == "LEVEL_FEED") {
    bool on = (arg == "ON");
    if (on != levelFeedActive()) logEvent("ESP2", "LEVEL_FEED", on ? "ON" : "OFF");
    levelFeedUntilMs = on ? nzMs(millis() + LEVEL_FEED_LEASE_MS) : 0;
    return;
  }
  if (resp == "CAL") {                                    // live raw sample from ESP2 (§A.3): id,raw
    int c2 = arg.indexOf(',');
    if (c2 > 0) { calRxId = arg.substring(0, c2); calRxRaw = arg.substring(c2 + 1).toFloat();
                  calRxValid = (calRxRaw != -1); calRxMs = millis(); }
    return;
  }
  if (resp == "TELE") {                                   // power telemetry during a run: PZEM,<v>,<i>,<p>,ACS,<a>
    // arg = "PZEM,228.0,1.40,319.0,ACS,0.80,ECPH,1.42,6.30" -> log verbatim as pipe-delimited.
    String d = arg; d.replace(",", "|");                  // CSV-safe (sec.25.2: no commas in detail)
    logEvent("ESP2", "SENSOR", d);
    // The AC voltage, for the AC Voltage setting's "last reading". -1 (PZEM unread, e.g. the inverter
    // off between AC stages) leaves the previous reading in place.
    if (arg.startsWith("PZEM,")) {
      float v = arg.substring(5).toFloat();         // toFloat() stops at the next comma
      if (v > 0.0f) { acLastV = v; acLastMs = millis(); }
      int c1 = arg.indexOf(',', 5);                 // PZEM,<v>,<i>,...: the current, for AC Current's "last"
      if (c1 > 0) { float a = arg.substring(c1 + 1).toFloat(); if (a >= 0.0f && v > 0.0f) { acLastI = a; acLastIMs = millis(); } }
    }
    // Capture the mixing-tank EC/pH for telemetry. ESP2 only sends these during a run (the tank is
    // empty otherwise), so they are the LAST BATCH's chemistry, not a live idle reading -- which is
    // the only honest thing a single-outlet tank can report. -1 = probe railed / not measured.
    int ei = arg.indexOf("ECPH,");
    if (ei >= 0) {
      String chem = arg.substring(ei + 5);          // not `t` -- that is the token array in this scope
      int c1 = chem.indexOf(',');
      if (c1 > 0) {
        float ec = chem.substring(0, c1).toFloat();
        float ph = chem.substring(c1 + 1).toFloat();
        if (ec >= 0) { tankEC = ec; tankChemMs = millis(); }
        if (ph >= 0) { tankPH = ph; tankChemMs = millis(); }
      }
    }
    return;
  }
  // Pump selector (2026-11-15): ESP2 has its inverter OFF and wants the servo on <pump>. servoRelayTick()
  // carries it to the Nano; nanoPaceTick() already puts the Nano in FAST for the run that follows. ESP2
  // re-sends the same seq every 3 s while it waits -- that is the same request, not a new one.
  if (resp == "SERVO_REQ") {                                // SERVO_REQ,<TRANSFER|BOOSTER>,<seq>
    int c2 = arg.indexOf(',');
    String pump = (c2 < 0) ? arg : arg.substring(0, c2);
    String seq  = (c2 < 0) ? "0" : arg.substring(c2 + 1);
    if (!servoSideForPump(pump.c_str())) { logEvent("ESP2", "SERVO", "REQ_BAD|" + arg); return; }
    if (servoReqActive && pump == servoReqPump && seq == servoReqSeq) return;
    servoReqActive = true; servoReqPump = pump; servoReqSeq = seq;
    servoReqStartMs = millis(); servoReqSentMs = 0;
    logEvent("ESP2", "SERVO", "REQ|" + pump + "|seq=" + seq);
    return;
  }
  if (resp == "PUMP") {                                     // PUMP,<TRANSFER|BOOSTER>,ON  /  PUMP,OFF
    if (arg.endsWith(",ON")) {
      levelWatch = true; levelWatchTransfer = arg.startsWith("TRANSFER");
      levelWatchSinceMs = millis(); mixHighHighRun = 0; mixHighHighSinceMs = 0;
      lastFastTankLogMs = 0;
      logEvent("ESP2", "PUMP", arg);
      // A timed dashboard pulse on a Fill/Push combo: the servo step took the first seconds, so the pulse's
      // own length starts now, when the pump is actually powered.
      // (Only for an ESP2 without INFO,INV_ON: from 2026-11-17 the seconds count from the inverter switch-on.)
      if (puPhase == PU_RUN && !puHoldMode && puRow >= 16 && puRow <= 19 && !puInvOnSeen)
        puEndMs = millis() + (unsigned long)puSeconds * 1000UL;
    } else {
      levelWatchStop("PUMP_OFF");
    }
    return;
  }
  if (resp == "READY") {
    // ESP2 finished booting (OFF-during-idle model). Fire whatever was waiting on it.
    logEvent("ESP2", "RESP", payload);
    levelWatchStop("ESP2_READY");                                // a fresh ESP2 has every output off
    esp2CommRecovered();                                         // any READY = link alive -> clear COMM_LOST latch
    // First thing after a boot, ahead of any work order or exercise it is about to get (the UART keeps
    // order): the AC supply window, in case a change was made while ESP2 was off.
    pushAcVoltToEsp2();
    esp2OnSecondary = false;                                     // a fresh ESP2 boots with P0 off
    tankDosedMask = 0;                                           // its INFO,TANK,DOSED follows if one is left
    bool switchReset = bswPendingDir != 0;                       // read before the next lines clear it
    if (bswPendingDir) {                                         // rebooted mid-switch: the switch reset it
      uint8_t dir = bswPendingDir;
      bswPendingDir = 0; bswSavePending(0); battQuietUntilMs = 0;
      bswStrike(dir, "ESP2_REBOOT");
    }
    // A READY in the middle of a live job means ESP2 rebooted under it (switch brown-out, WDT, power
    // glitch). Without this the run would sit until UART_DONE_TIMEOUT_MS (10 min) and then end as an
    // ESP2_DONE_TIMEOUT emergency stop; a held fault or a sent exercise would be stranded likewise.
    bool lostJob = (wo.active && (wo.stage == WO_SENT || wo.stage == WO_ACKED)) || esp2Held ||
                   (pendingExercise.active && pendingExercise.sent);
    // A held ESP2 had already stopped and saved its volume, so only a run still going needs the tank check.
    if (lostJob) esp2LostJob(runLossNeedsTankCheck(wo.active && wo.stage == WO_ACKED && !esp2Held, switchReset));
    // Only a session ESP2 had already CONFIRMED is lost by a reboot. Every pulse/Testing session starts
    // by powering ESP2 up, so the READY of that very boot is expected and must not end it (PU_ARM and
    // an unconfirmed Testing arm simply carry on arming).
    if (puPhase == PU_RUN || puPhase == PU_REPORT) pulseFinish(false, "ESP2 restarted");
    else if (sysState == TEST_MODE && esp2TestArmed) {          // armed Testing lost its TEST mode: re-arm
      esp2TestArmed = false; testArmTries = 0; lastTestArmMs = 0; esp2WarmupMs = millis();
      logEvent("ESP1", "RESP", "TEST|ESP2_RESTARTED|REARM");
    }
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
    if (lostJob) { setState(STARTUP_SYNC); return; }            // re-validate + re-push cal; due work re-runs
    // Testing arming is handled by the primed retry in testHoldTick (the single READY here
    // can be lost on a cold relay boot), so we do NOT send TEST,ENTER from this one message.
    dispatchPendingWorkOrder();                                 // FORCE run parked in WO_PENDING
    if (pendingRun.active && !wo.active) dispatchPendingRun();  // scheduled-run warm-up complete
    if (pendingExercise.active && !pendingExercise.sent) dispatchPendingExercise();  // exercise warm-up done
    return;
  }
  if (resp == "TEST") {                                   // TEST,FLOW,<bit>,<pulses> after a pulse release
    if (arg.startsWith("FLOW,")) {
      int c1 = arg.indexOf(',', 5);
      if (c1 > 0) { puFlowPulses = (unsigned long)arg.substring(c1 + 1).toInt(); puFlowSeen = true; }
    }
    // web v.4: TEST,LEVEL,<row>,<dMixL> -- a Fill / Push combo has no meter; the mixing-tank level change since
    // the pump came on answers "did water move" (-99 = no level to compare).
    if (arg.startsWith("LEVEL,")) {
      int c1 = arg.indexOf(',', 6);
      if (c1 > 0) { puLevelL = arg.substring(c1 + 1).toFloat(); puLevelSeen = true; }
    }
    // TEST,SERVO,<TRANSFER|BOOSTER>,OK: a Servo>Transfer / Servo>Booster Testing row finished its move
    // (the Nano confirmed it; the inverter stayed off). Shown as [OK] on the LCD and in a pulse result.
    if (arg.startsWith("SERVO,") && arg.endsWith(",OK")) {
      testServoDoneRow = (arg.indexOf("TRANSFER") > 0) ? TEST_SERVO_L_ROW
                       : (arg.indexOf("BOOSTER") > 0)  ? TEST_SERVO_R_ROW : -1;
      wakeBacklight();
    }
    logEvent("ESP2", "RESP", payload);
    return;
  }
  if (resp == "STATUS") return;                                // heartbeat
  if (resp == "INFO") {                                       // e.g. INFO,IDLE_RESET (self-heal notice)
    // Repeated each heartbeat while it is true, so handled before the log line and logged on change only.
    if (arg.startsWith("TANK,DOSED,")) {
      uint8_t m = (uint8_t)(arg.substring(11).toInt() & 0x07);
      if (m != tankDosedMask) {
        tankDosedMask = m;
        logEvent("ESP2", "RESP", payload + "|finishing this batch first");
      }
      return;
    }
    // ESP2 re-asserts a held fault every 30 s (2026-11-17 bug check): INFO,HELD,<work order>,<code>,<loc>. Normally we
    // already hold it and this is ignored (not logged -- a long hold would fill the log). If ESP2's own hold reply was
    // lost on the wire, this is how the hold reaches the operator: only for the work order ESP2 acknowledged from us.
    if (arg.startsWith("HELD,")) {
      if (esp2Held || !wo.active || wo.stage != WO_ACKED) return;
      int c1 = arg.indexOf(',', 5), c2 = (c1 > 0) ? arg.indexOf(',', c1 + 1) : -1;
      if (c2 < 0) return;
      String name = arg.substring(5, c1);
      if (wo.cmd.indexOf("," + name + ",") < 0) return;          // not the work order we are running
      String code = arg.substring(c1 + 1, c2), loc = arg.substring(c2 + 1);
      logEvent("ESP2", "RESP", payload + "|hold_reply_was_lost");
      enterFaultHold(code.c_str(), loc.c_str());
      return;
    }
    logEvent("ESP2", "RESP", payload);
    // INFO,PUMP_RATE,<pump>|seen=<rate>|... (2026-11-17): the rate a stage's levels / dose meter really saw. Kept for
    // the LCD's Calibration > Rate screen, beside the value typed in.
    // INFO,INV_ON,<pump> (2026-11-17): ESP2 switched the inverter relay ON. A timed run's seconds count from here, the
    // way the pumps were measured: the LCD pump estimate's counter, and a dashboard Fill/Push pulse's length.
    if (arg.startsWith("INV_ON,")) {
      if (pestRunning) pestInvOnMs = nzMs(millis());
      if (puPhase == PU_RUN && !puHoldMode && puRow >= 16 && puRow <= 19) {
        puEndMs = millis() + (unsigned long)puSeconds * 1000UL;
        puInvOnSeen = true;
      }
      return;
    }
    if (arg.startsWith("PUMP_RATE,")) {
      int bar = arg.indexOf('|'), sp = arg.indexOf("|seen=");
      if (bar > 10 && sp > 0) {
        String pump = arg.substring(10, bar);
        float v = arg.substring(sp + 6).toFloat();
        int k = (pump == "TRANSFER") ? 0 : (pump == "BOOSTER") ? 1 : (pump == "NUT_A") ? 2 :
                (pump == "NUT_B") ? 3 : (pump == "NUT_C") ? 4 : -1;
        if (k >= 0 && v > 0.0f && v < 100000.0f) rateSeen[k] = v;
      }
      return;
    }
    if (arg.startsWith("DOSE_CARRY,")) {                      // a batch dosed for one column went to another
      raiseFault('M', "DOSE_CARRY", (String("mask=") + arg.substring(11) + "|no extra dose added").c_str());
      return;
    }
    // A fault whose own P0 drop reset the boards before its reply got out. The hold is gone, so report
    // it here and skip the column for today, as a STOP reply does: re-running into the same fault would
    // only burn the next strike. A forced run can still retry it.
    if (arg.startsWith("FAULT_LOST,")) {
      sendEsp2("BATTSW,FACK");
      static String lastLost = "";
      static unsigned long lastLostMs = 0;
      if (arg == lastLost && millis() - lastLostMs < 120000UL) return;   // only our FACK was lost
      lastLost = arg; lastLostMs = millis();
      int c1 = arg.indexOf(',', 11), c2 = (c1 > 0) ? arg.indexOf(',', c1 + 1) : -1;
      if (c2 > 0) {
        uint8_t m = (uint8_t)(arg.substring(c1 + 1, c2).toInt() & 0x07);
        for (int b = 0; b < NUM_COLUMNS; b++) if (m & (1 << b)) markServiced(b, -1, false);   // the whole day; RAM only
        raiseFault('M', arg.substring(11, c1).c_str(), (arg.substring(c2 + 1) + "|BOARD_RESET|skipped today").c_str());
      }
      return;
    }
    // Secondary-battery switch notices: track which battery the boards are on. MAIN also releases a
    // power-off that was being held for exactly this return (see esp2SetPower).
    if (arg.startsWith("BATT,SWITCHING,")) {
      // ESP2 is about to move P0. Get ready for the rails to drop: log on the card, switch journaled,
      // quiet window, then let it go.
      uint8_t dir = arg.endsWith("SECONDARY") ? 1 : 2;
      logFlush(true);
      if (bswPendingDir != dir) bswSavePending(dir);
      bswPendingDir = dir; bswPendingMs = millis();
      bswCmdJournal();                                        // before GO: the switch may be the end of this boot
      battQuietUntilMs = millis() + BSW_QUIET_MS;
      bswGoMs = nzMs(millis()); bswGoDir = dir;
      sendEsp2("BATTSW,GO");
      return;
    }
    if (arg == "BATT,VERIFY") {
      // ESP2 is on the secondary and survived it; is everyone else still up? The verdict is reached in bswTick();
      // a repeat (our answer was lost) just gets the same answer again.
      if (!bswVerifyStartMs) {
        bswVerifyStartMs = nzMs(millis()); bswVerdict = nullptr;
        sendNanoCommand("FAST", false);          // the Nano answers FAST with a TANK at once -- if it is alive
      }
      bswVerifyAnswer();
      return;
    }
    if (arg.startsWith("BATT,RESET,")) {                    // ESP2's own journal: a switch reset it
      sendEsp2("BATTSW,ACK");
      if (bswBootStruck) {                                    // same event ESP1 already counted at boot
        bswBootStruck = false;
        logEvent("ESP1", "PWR2", "BATTSW|RESET_DUP|by=ESP2_JOURNAL|same_as_ESP1_boot");
      } else {
        bswStrike(arg.endsWith("SECONDARY") ? 1 : 2, "ESP2_JOURNAL");
      }
      return;
    }
    if (arg == "BATT,SECONDARY" || arg == "BATT,MAIN") {   // switch survived (or was called off)
      esp2OnSecondary = (arg == "BATT,SECONDARY");
      if (bswPendingDir) { bswPendingDir = 0; bswSavePending(0); }
      bswVerifyStartMs = 0; bswVerdict = nullptr;              // that switch's verification is over
      battQuietUntilMs = 0;
      if (!esp2OnSecondary && esp2OffPending && esp2BattWaitUntil) esp2OffAt = millis();
    }
    return;
  }

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
    // Proof of life, so the DONE timeout means "ESP2 went silent" and not "this run is long": a legal
    // FORCE dose of 500 mL takes ~10 min at the pump's rate, and that alone used to end in an
    // ESP2_DONE_TIMEOUT emergency stop. ESP2's own per-stage caps bound how long a run can go on.
    if (wo.active && wo.stage == WO_ACKED) wo.sentMs = millis();
    return;
  }

  if (resp == "DOSE") {
    // Per-nutrient dose result: DOSE,NUT_x,target_mL,measured_mL,COL_y (sec.25.2.1).
    // Accumulate measured nutrient volume for the daily report (B1); log as DOSE.
    String t[5]; int nt = 0, st = 0;
    for (int i = 0; i <= (int)arg.length() && nt < 5; i++)
      if (i == (int)arg.length() || arg[i] == ',') { t[nt++] = arg.substring(st, i); st = i + 1; }
    if (nt >= 4) {
      float measuredMl = t[2].toFloat();
      if (forceMask && __builtin_popcount(forceMask) > 1) {
        // Same reasoning as the WATER split in the DONE handler below: one dosing pump, one measured
        // volume, but a multi-column FORCE run's colLoc tag ("COL_AB") never matches this loop's
        // single-letter comparison, so t[3] here never resolves to a column and the whole measured
        // amount was silently dropped from nutrientUsedToday[] for every FORCE run touching 2+ columns.
        int nc = __builtin_popcount(forceMask);
        for (int b = 0; b < NUM_COLUMNS; b++) if (forceMask & (1 << b)) nutrientUsedToday[b] += measuredMl / nc;
      } else {
        int dc = -1;
        for (int j = 0; j < NUM_COLUMNS; j++) if (t[3] == String("COL_") + COL_TAG[j]) dc = j;
        if (dc >= 0) nutrientUsedToday[dc] += measuredMl;     // measured mL
      }
      if (runPhase != RUN_NONE && t[0].length() >= 5) {         // NUT_A/B/C -> the post-run receipt
        int ni = t[0].charAt(4) - 'A';
        if (ni >= 0 && ni < 3) run.doseGot[ni] = measuredMl;   // per-nutrient for the whole run, not per-column -- not split
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
    if (arg.startsWith("RESUME") && resumeAwaitingAck) {       // ESP2 actually confirmed the recovery RESUME
      resumeAwaitingAck = false;
      esp2Held = false;
      lastHeldClearReason = "resumed";
      String mode = arg.substring(arg.indexOf(',') + 1);
      logEvent("ESP1", "ACT", String("RESUME|CONFIRMED|") + mode + "|COL_" +
               (wo.colIdx >= 0 ? String(COL_TAG[wo.colIdx]) : String("?")));
      sendSMS("OK: Run resumed (" + mode + ")\n" +
              (mode == "IRRIGATE" ? "Water only, no more nutrients." : mode == "RELEASE" ? "The tank goes to the column as it is." : "The paused run continues.") +
              "\nConfirmed by the pump controller.");
      lastFaultMsg = "Resumed (" + mode + ") - confirmed by ESP2";   // see diagnostics.system.lastFault
      setState(ACTIVE_STATE);                                  // supervising the resumed run again
      // D-M2 fix: enterFaultHold() -> runUiAbort() forced runPhase to RUN_NONE when the fault was
      // raised, so the dashboard's Run Progress panel (and the LCD run screen) went dark even though
      // wo/ESP2 stayed mid-sequence the whole time. A confirmed RESUME means the same work order is
      // live again, so re-arm the same phase runUiAdvance() would already be in (RUN_LIVE) WITHOUT
      // touching run.* (targets/doses/step count) -- this is a resume, not a new run, so calling
      // runUiBegin() here would wrongly reseed the doses and reset progress to "Starting".
      if (runPhase == RUN_NONE && wo.active) { runPhase = RUN_LIVE; runPhaseMs = millis(); runLocked = true; }
    }
    // Only ESP2's ACK of the work order itself (or of a resume) counts. ESP2 also ACKs SET_CAL, CAL_STOP and
    // TEST frames, and one of those would otherwise mark a parked WO_PENDING order as accepted without it
    // ever being sent -- leaving the run to die 10 minutes later in ESP2_DONE_TIMEOUT.
    if (wo.active && (arg.startsWith("SEQ_") || arg.startsWith("RESUME"))) { wo.stage = WO_ACKED; wo.sentMs = millis(); }
  } else if (resp == "DONE") {
    // LCD "... pump est" run (2026-11-17): its timed prime ended (PRIME_TIMED), or stopped early (PRIME / PRIME_CAP).
    if (pestRunning && (arg.startsWith("PRIME_TIMED,") || arg == "PRIME" || arg == "PRIME_CAP")) pestOnDone(arg);
    if (arg.startsWith("EXERCISE")) {                          // preventive exercise finished
      int k = pendingExercise.idx;
      if (k >= 0 && k < 3) lastPumpUseMs[k] = millis();        // reset that pump's 2-day clock
      logEvent("ESP1", "ACT", String("EXERCISE|STOP|") + (k >= 0 ? EX_NAME[k] : "?"));
      pendingExercise.active = false; pendingExercise.idx = -1; pendingExercise.sent = false;
      firebaseRemoteExerciseDone("completed", "pump test finished");   // no-op if locally initiated
      if (sysState == ACTIVE_STATE) setState(IDLE_STATE);
      esp2SetPower(false);
    } else if (wo.active && arg.startsWith("SEQ_")) {
      // SEQ_ only: ESP2 also sends DONE,STOP_ALL / DONE,TEST / DONE,PRIME / DONE,TEST_CAP, and any of those
      // landing during a run used to be taken for its completion -- run closed, column marked serviced,
      // ESP2 powered off mid-sequence.
      int c = wo.colIdx;
      if (c >= 0 && c < NUM_COLUMNS) {
        // The window this run counts for (several a day since 2026-11-05). A run outside every window -- a
        // forced run then, or a dosed batch finished early -- uses none up: the soil check is what keeps the
        // next window from watering a column that is still wet. Saved, so a reboot can't re-run it.
        if (wo.window >= 0) markServiced(c, wo.window, true);
        // Nutrients went in: no more today, for every column this batch reached (A + B for a joint forced run).
        if (wo.fertigate) {
          uint8_t fed = forceMask ? forceMask : (uint8_t)(1 << c);
          for (int b = 0; b < NUM_COLUMNS; b++) if (fed & (1 << b)) {
            col[b].fertStamp = currentDayStamp;
            prefs.putLong((String("c") + b + "fd").c_str(), currentDayStamp);
          }
        }
        // Today's tally (2026-11-06): one watering for every column this run reached, and one feeding when it added
        // nutrients. Saved with the litres just below (saveDayTotals()).
        {
          uint8_t reached = forceMask ? forceMask : (uint8_t)(1 << c);
          for (int b = 0; b < NUM_COLUMNS; b++) if (reached & (1 << b)) {
            if (wateringsToday[b] < 255) wateringsToday[b]++;
            if (wo.fertigate && feedingsToday[b] < 255) feedingsToday[b]++;
          }
        }
        // B1: optional measured water volume reported as ...,WATER,<liters>[,EST] on DONE. EST means
        // ESP2 hit a flow-blind (timed, not metered) stage somewhere in this run -- String::toFloat()
        // already safely stops at the trailing comma so `liters` itself is unaffected either way, but
        // this distinction was previously read off the wire and then silently dropped: neither the
        // log nor waterUsedToday[] ever recorded that a total was estimated rather than truly metered.
        int wi = payload.indexOf("WATER,");
        float liters = (wi >= 0) ? payload.substring(wi + 6).toFloat() : 0.0f;
        bool waterEstimated = payload.indexOf(",EST") >= 0;
        // The post-run receipt's water figure (2026-11-05). run.waterGot was only ever set from PROG, but
        // ESP2 adds a Release stage's litres to its total when that stage ENDS, and DONE follows at once,
        // so no PROG carried them: every irrigation receipt read WATER=0.00of<target> (LCD, log and the
        // dashboard's run panel), and fertigation ones lost the last release. DONE's WATER is the same
        // running total as PROG's third field, only final, so it is the right number for the receipt.
        // The finished stage's own litres are zeroed with it: the dashboard adds a Release stage's litres
        // to the total while that stage is still moving water, and would otherwise count them twice now.
        if (wi >= 0 && runPhase != RUN_NONE) { run.waterGot = liters; run.stageL = 0; run.stageTgt = 0; }
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
                                    + "|SPLIT=EST/" + String(nc) + (waterEstimated ? "|TIMED" : ""));
        } else {
          if (wi >= 0) waterUsedToday[c] += liters;
          // Log the litres in the ACT row (|W=) so SUMMARY/FULL can recover per-day water totals.
          logEvent("ESP1", "ACT", String(wo.fertigate ? "FERTIGATION" : "IRRIGATION")
                                    + "|STOP|" + (forceMask ? "FORCE|" : "") + "COL_" + COL_TAG[c]
                                    + "|W=" + String(liters, 2) + (waterEstimated ? "|TIMED" : ""));
        }
        saveDayTotals();                           // so a restart later today keeps them (2026-11-06)
        if (forceCmdId[0]) {                       // report the run back to the dashboard
          firebaseQueueStatus(forceCmdId, "completed", (String("delivered ") + String(liters, 2) + " L").c_str());
          forceCmdId[0] = 0;
        }
        forceMask = 0;
        // A-then-B: column A has finished, so start B's countdown now. Its delay is measured from
        // THIS moment, not from arming -- ESP2 runs one work order at a time, so the two sequences
        // are inherently back-to-back and each gets its own fully metered batch.
        if (fcPhase == FP_RUNNING) {
          if (fcMode == 3 && fcSlot == 0) {
            fcSlot = 1; fcPhase = FP_WAIT;
            fcFireAtMs = millis() + (unsigned long)fcSeq[1].delayS * 1000UL;
            logEvent("ESP1", "ACT", String("FORCE|NEXT|COL_B|L=") + String(fcSeq[1].dL / 10.0f, 1) +
                                    "|in=" + String(fcSeq[1].delayS) + "s");
          } else fcPhase = FP_NONE;                // sequence complete
        }
      }
      // A real run exercises transfer+booster (and mixer if fertigating) -> reset their clocks.
      lastPumpUseMs[0] = millis(); lastPumpUseMs[1] = millis();
      if (wo.fertigate) lastPumpUseMs[2] = millis();
      wo.active = false; wo.stage = WO_IDLE; wo.colIdx = -1;
      tankDosedMask = 0;               // ESP2 cleared its dose journal when it finished this run
      if (sysState == ACTIVE_STATE) setState(IDLE_STATE);
      esp2SetPower(false);             // run complete -> ESP2 off (idle power-saving, sec.18.8)
      lastHoldCode = ""; sameHoldN = 0; steerRelease = false;   // run completed -> clear the re-hold guard
      runUiFinish(true);               // -> post-run receipt (+ error table if the run logged problems)
    }
  } else if (resp == "BUSY") {
    // ESP2 busy; leave work order pending, it will retry/await. EXCEPT a retry answered BUSY,ACTIVE: our
    // first copy got through and only its ACK was lost, so the run is live. Treating that as "no answer"
    // ran out the retries and sent RECOVERY_STATE's RESET_SELF into a healthy run.
    if (arg == "ACTIVE" && wo.active && wo.stage == WO_SENT && wo.retries > 0) {
      wo.stage = WO_ACKED; wo.sentMs = millis();
      logEvent("ESP1", "RESP", "WO|ACK_LOST|ESP2_ALREADY_RUNNING");
    }
  } else if (resp == "DEGRADED") {
    // ESP2 disabled one channel (nutrient/mixer) but is STILL running the sequence:
    // Major, NOT Critical -- do not abort or cut power, just log+alert (sec.23.2.2.1/.4).
    raiseFault('M', "ESP2_DEGRADED", arg.c_str());
  } else if (resp == "ERROR") {
    enterFaultHold("ESP2_ERROR", arg.c_str());   // ESP2 holds (P17 dropped); keep wo for user-gated RESUME
  } else if (resp == "FLOW_FAIL") {
    enterFaultHold("FLOW_FAIL", arg.c_str());
  } else if (resp == "LEVEL_BLIND") {
    // ESP2 stopped a fill because the mixing-tank ultrasonic gave no usable reading (Nano silent, lost echo,
    // too noisy) -- filling blind could overflow -- or (web v.4) it could not get a SETTLED level to plan a
    // fill / delivery from (|NO_SETTLED_LEVEL). Held like FLOW_FAIL; RELEASE / IRRIGATE then run on a timer.
    enterFaultHold("LEVEL_BLIND", arg.c_str());
  } else if (resp == "SERVO_FAIL") {
    // The pump selector could not be confirmed (Nano did not answer, or the inverter's AC was still live),
    // so ESP2 never turned the inverter on. In a run ESP2 holds it; for an exercise / prime / Testing combo
    // ESP2 has already ended that action (its DONE comes separately), so it is an alert only.
    if (uiMode == UI_TEST || sysState == TEST_MODE) {      // raiseFault() below is suppressed here: show it
      testServoFailUntilMs = millis() + 4000;
      testServoFailAcLive  = (arg.indexOf("AC_LIVE") >= 0);
      testBattSwitchFail   = false;
      testInverterNoAc     = false;
      wakeBacklight();
    }
    // A website servo move (a timed pulse on a servo row) ends now, with ESP2's reason (see pulseTick).
    if (puPhase == PU_RUN && !puHoldMode && (puRow == TEST_SERVO_L_ROW || puRow == TEST_SERVO_R_ROW)) {
      String why = arg.startsWith("TEST|") ? arg.substring(5) : arg;
      strlcpy(puServoFailWhy, why.length() ? why.c_str() : "?", sizeof(puServoFailWhy));
    }
    if (wo.active) enterFaultHold("SERVO_FAIL", arg.c_str());
    else raiseFault('M', "SERVO_FAIL", arg.c_str());
  } else if (resp == "BATT_SWITCH_FAIL" || resp == "INVERTER_FAIL") {
    // 2026-11-16, both handled like SERVO_FAIL under their own names:
    //  BATT_SWITCH_FAIL: the boards were moved to the secondary (P0) but did not all come through it -- a board or
    //    the relay bank rebooted, or nobody confirmed (|NANO_RESET, |NANO_SILENT, |PCF_RESET, |NO_CONFIRM,
    //    |TIMEOUT). ESP2 never turned the inverter on and is returning to main.
    //  INVERTER_FAIL: the inverter relay was ON for INV_CONFIRM_MAX_MS (6 s) and the PZEM read no voltage on the
    //    inverter output (|NO_AC|v=none = no PZEM reply at all). ESP2 switched the relay off again.
    if (uiMode == UI_TEST || sysState == TEST_MODE) {
      testServoFailUntilMs = millis() + 4000;
      testServoFailAcLive  = false;
      testBattSwitchFail   = (resp == "BATT_SWITCH_FAIL");
      testInverterNoAc     = (resp == "INVERTER_FAIL");
      wakeBacklight();
    }
    if (wo.active) enterFaultHold(resp.c_str(), arg.c_str());
    else raiseFault('M', resp.c_str(), arg.c_str());
  } else if (resp == "SERVO_MISMATCH") {
    // 10 s after the inverter came on, the tank levels said the OTHER pump is running (2026-11-16 user rule:
    // |WRONG_WAY = they moved the wrong way; |NO_MOVE = a fill moved nothing although the reservoir holds water,
    // i.e. the booster is powered against its shut valves). ESP2 has already stopped the inverter and is swinging
    // the servo twice before trying again (try=n/2); the run continues -- no halt. Alert only: a third verdict
    // comes back as SERVO_FAIL|STUCK, held above (Critical, with the choice to switch it again or not).
    raiseFault('M', "SERVO_MISMATCH", arg.c_str());
  } else if (resp == "LEVEL_HIGH") {
    // The mixing tank reached ESP2's overflow limit (%) before the fill's litres target: the fill was stopped
    // and the run carries on with the measured litres (user decision). Major: the % (empty/full geometry) and
    // the litres (tank calibration points) disagree, which is worth a look -- re-check "Tank Mix liters".
    raiseFault('M', "LEVEL_HIGH", arg.c_str());
  } else if (resp == "RES_EMPTY") {
    // The reservoir ran dry during a fill, so ESP2 stopped the transfer pump and the run carries on with the litres
    // that came in (user decision 2026-11-15). Judged on the MIXING TANK (2026-11-16): the reservoir's ultrasonic
    // cannot see below its filter (~15 %), so once it reads <= 18 % a 10 s window in which the mixing tank did not
    // rise = empty (|by=RES_EMPTY_WINDOW); or the transfer stalled for 12 s (|by=STALL). Major + SMS: the reservoir
    // needs refilling, and a fertigation batch may be stronger than planned (RES_EMPTY,TRANSFER|metered=<L>|
    // target=<L>|res=<%>|by=<rule>). The mixing tank emptying on a drain is not a fault -- INFO,MIX_EMPTY (the
    // booster sucked air) or INFO,MIX_LOW_STOP (the 5 % low stop) -- only logged.
    raiseFault('M', "RES_EMPTY", arg.c_str());
  } else if (resp == "PWR_FAIL") {
    // A voltage trip carries the reading (VOLTAGE|v=205.3|LOW|min=215): keep it as the last AC reading,
    // the only one an exercise or AC prime ever reports (they send no TELE).
    if (arg.startsWith("VOLTAGE|v=")) {
      float v = arg.substring(10).toFloat();
      if (v > 0.0f) { acLastV = v; acLastMs = millis(); }
    }
    // A current trip carries its reading too (OVERCURRENT|i=4.12|max=3.0, NO_CURRENT|i=0.02|min=0.5).
    if (arg.startsWith("OVERCURRENT|i=") || arg.startsWith("NO_CURRENT|i=")) {
      float a = arg.substring(arg.indexOf("i=") + 2).toFloat();
      if (a >= 0.0f) { acLastI = a; acLastIMs = millis(); }
    }
    if (wo.active) enterFaultHold("PWR_FAIL", arg.c_str());
    else {
      // Pump exercise or AC prime: ESP2 stopped the load itself and is NOT holding anything. Treating this
      // as a hold offered recovery choices that could only fail, and stranded pendingExercise (no DONE
      // ever comes), which blocked all later exercises and tests until a reboot.
      raiseFault('M', "PWR_FAIL", arg.c_str());
      if (pendingExercise.active) {
        int k = pendingExercise.idx; if (k >= 0 && k < 3) lastPumpUseMs[k] = millis();   // next try in 2 days
        pendingExercise.active = false; pendingExercise.idx = -1; pendingExercise.sent = false; pendingExercise.acked = false;
        firebaseRemoteExerciseDone("failed", "power fault during the test");
        if (sysState == ACTIVE_STATE) setState(IDLE_STATE);
        esp2SetPower(false);
      }
    }
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
    // An ESP2 flashed before 2026-11-15 refuses the bare Inverter row; the current one allows it (user override).
    if (arg == "TEST_USE_COMBO") { testUseComboUntilMs = millis() + 3000; return; }
    // ESP2 refuses this column outright: the COLUMN_ENABLED tables disagree (ESP1's is editable at runtime,
    // ESP2's is compiled in). Retrying cannot help, and letting the retries run out would reset a healthy
    // ESP2 for a config mismatch. Say what is wrong and leave the column alone until tomorrow.
    if (wo.active && wo.stage == WO_SENT && (arg.startsWith("COL_DISABLED") || arg.startsWith("COLUMN"))) {
      int c = wo.colIdx;
      wo.active = false; wo.stage = WO_IDLE; wo.colIdx = -1;
      if (c >= 0 && c < NUM_COLUMNS) markServiced(c, -1, false);   // the whole day; RAM only: a reboot may retry
      forceAbort("COL_MISMATCH");
      if (forceCmdId[0]) { firebaseQueueStatus(forceCmdId, "failed", "ESP2 has this column disabled"); forceCmdId[0] = 0; }
      forceMask = 0;
      runUiAbort("COL_MISMATCH");
      String loc = (c >= 0 && c < NUM_COLUMNS) ? String("COL_") + COL_TAG[c] + "|ESP2 has it disabled" : String("COL_?");
      raiseFault('M', "COL_MISMATCH", loc.c_str());
      if (sysState == ACTIVE_STATE) setState(IDLE_STATE);
      esp2SetPower(false);
      return;
    }
    // A recovery RESUME was refused. The usual cause is that ESP2 restarted during the hold and
    // lost its paused sequence (the mix-tank volume still survives in its NVS). Clear the held UI
    // and let the still-due column re-run normally -- ESP2 will fill only the remaining liters.
    if (esp2Held && arg.startsWith("NOT_HELD")) {
      esp2Held = false; wo.active = false; wo.stage = WO_IDLE;
      esp2SetPower(false);                       // nothing left to hold: back to OFF-during-idle
      lastHeldClearReason = "esp2_restarted";
      resumeAwaitingAck = false;
      // Same re-hold-guard reset cancelRun() and the normal DONE-success path already do -- this is
      // one of the four documented esp2Held-clear sites but previously missed it, so an unrelated
      // LATER fault that happens to share a code with whatever was held here would inherit a stale
      // sameHoldN instead of starting its own count at 1.
      lastHoldCode = ""; sameHoldN = 0; steerRelease = false;
      // Same release as the other abnormal endings: this work order is gone, so nothing may keep
      // waiting on it -- an armed A-then-B sequence or a dashboard command most of all.
      forceAbort("ESP2_RESTARTED");
      if (forceCmdId[0]) { firebaseQueueStatus(forceCmdId, "failed", "ESP2 restarted during the run"); forceCmdId[0] = 0; }
      forceMask = 0;
      logEvent("ESP1", "RESP", "RESUME_REFUSED|ESP2_RESTARTED");
      sendSMS("INFO: Pump controller restarted during the hold.\nTank volume kept. The column will re-run on its own.");
      if (sysState == EMERGENCY_STOP) setState(IDLE_STATE);
    }
  }
}

// Open-loop gap dose (§3): see the forward-declared comment in the dosing tables block.
void calcDose(int c, float mL[3], float ceilS[3]) {
  for (int i = 0; i < 3; i++) { mL[i] = 0; ceilS[i] = 0; }
  float V = WATER_BUDGET_L[c] * (1.0f - FLUSH_PCT / 100.0f);   // planned batch volume this run (§1.1)
  if (V < MIXING_TANK_SAFE_MIN) {
    logEvent("ESP1", "DOSE", "BATCH_LOW|water-only");
    // This is a commissioning/constant-tuning question (WATER_BUDGET_L vs. FLUSH_PCT vs.
    // MIXING_TANK_SAFE_MIN) -- CLAUDE.md says do not invent final values here, so this does not
    // change any of them. But the abort itself was previously silent beyond the log line above;
    // one-shot alert so it's actually noticed rather than discovered later as "nutrients never dose."
    if (!dosingBatchLowLatched[c]) {
      dosingBatchLowLatched[c] = true;
      // New-F9 (audit): same "diagnostic enrichment" pattern as the FLOW_FAIL/PWR_FAIL/etc fixes --
      // V and MIXING_TANK_SAFE_MIN are both already in scope; include them instead of just the column.
      String colLoc = String("COL_") + COL_TAG[c] + "|V=" + String(V, 2) + "|min=" + String(MIXING_TANK_SAFE_MIN, 2);
      raiseFault('M', "DOSE_BATCH_LOW", colLoc.c_str());
    }
    return;
  }
  dosingBatchLowLatched[c] = false;   // recovered (constants retuned) -- allow it to alert again if it recurs
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

/* ESP2 rejects any frame over 256 B as INVALID,FRAMING (ESP2/src/main.cpp pollEsp1). A worst-case
 * FORCE fertigation already runs ~230 B, so there is not much room left -- and the failure is ugly:
 * ESP1 would retry three times and then drop into RECOVERY_STATE, with nothing in the log naming
 * the length as the cause. Refuse to emit a frame that cannot be parsed, and say why.
 * WO_FRAME_MAX is below ESP2's 256 so a marginal frame is caught here rather than on the wire. */
const size_t WO_FRAME_MAX = 240;
static bool woFrameFits(const String &cmd, const char *what) {
  if (cmd.length() <= WO_FRAME_MAX) return true;
  logEvent("ESP1", "FAULT", String("WO|OVERSIZE|") + what + "|" + cmd.length() + "B>" + WO_FRAME_MAX);
  // ALERT, not ERR: "ERR," is the reply convention for a REJECTED INBOUND command, and sendSMS now
  // logs those as SMS_ERR -- using it here would file an outbound alarm as an inbound parse failure.
  smsAlert(smsFaultText('M', "WO_OVERSIZE", smsPlain(what) + ", " + String(cmd.length()) + " bytes"));
  return false;
}

void sendWorkOrder(int c, bool fertigate) {
  // Last line of defence. controlTick() already gates, but this is the single function that can put
  // a run on the wire, so it refuses independently rather than trusting every future caller.
  if (actuationsDisabled) { logEvent("ESP1", "ACT", "WO|BLOCKED|ACTUATIONS_OFF"); return; }
  if (webManualHeld())    { logEvent("ESP1", "ACT", "WO|BLOCKED|WEB_MANUAL"); return; }
  if (tankCheckNeeded)    { logEvent("ESP1", "ACT", "WO|BLOCKED|TANK_CHECK"); return; }
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
    // Same MIXING_TANK_MAX_VOLUME ceiling calcDose() applies -- this recomputes the identical
    // formula independently, and previously did so without the clamp, so a future WATER_BUDGET_L
    // raised past ~62.5L would size the nutrient dose for one (capped) volume while telling ESP2 to
    // target a larger (uncapped) one here, silently diluting the dose. Dormant today (5L << 50L).
    float wireBatchV = WATER_BUDGET_L[c] * (1.0f - FLUSH_PCT / 100.0f);
    if (wireBatchV > MIXING_TANK_MAX_VOLUME) wireBatchV = MIXING_TANK_MAX_VOLUME;
    cmd += ",BATCHV," + String(wireBatchV, 1);                                                        // planned batch L (§1.1)
    cmd += ",EC," + String(EC_MIN, 1) + "," + String(EC_MAX, 1);
    cmd += ",PH," + String(PH_MIN, 1) + "," + String(PH_MAX, 1);
    cmd += ",MIX," + String(MIXING_DURATION_MS);
  }
  // Job-critical calibration (companion spec §A.5.1 #3): make the job self-contained so a reset
  // moments before it runs can never execute on stale K-factors / EC-pH cal.
  cmd += ",MIXCAP," + String(mixCapLiters(), 2);   // web v.4: no KMAIN (no main-line meters); the fill ceiling instead
  if (fertigate) {
    cmd += ",KNUT," + String(calKNut[0], 1) + "," + String(calKNut[1], 1) + "," + String(calKNut[2], 1);
    cmd += ",ECCAL," + String(calEcM, 6) + "," + String(calEcB, 4);
    cmd += ",PHCAL," + String(calPhM, 6) + "," + String(calPhB, 4);
  }
  cmd += "," + String(FRAME_END);
  // Refusing is only safe if we also unwind. By here the caller has cleared pendingRun and we are in
  // ACTIVE_STATE with the pre-run receipt on screen, so a bare `return` would strand the rig in
  // ACTIVE_STATE with no work order and nothing supervising it -- worse than the frame we refused.
  if (!woFrameFits(cmd, "SCHED")) {
    wo.active = false; wo.stage = WO_IDLE; wo.colIdx = -1;
    runUiAbort("WO_OVERSIZE");                 // release the LCD takeover
    esp2SetPower(false);
    setState(IDLE_STATE);
    return;
  }

  wo.active = true; wo.stage = WO_SENT; wo.colIdx = c; wo.fertigate = fertigate;
  wo.window = -1;                                    // dispatchPendingRun() sets the scheduled run's own window
  wo.cmd = cmd; wo.retries = 0; wo.sentMs = millis();
  esp2Serial.println(cmd);
  logEvent("ESP1", "CMD", "ESP2|" + name);
  logEvent("ESP1", "ACT", String(fertigate ? "FERTIGATION" : "IRRIGATION")
                            + "|START|COL_" + COL_TAG[c]);

  // Consolidated run notice (one per column service; sec.12.1.2)
  { String t = smsClock();
    sendSMS(String("RUN: ") + smsCol(c) + "\n" + (fertigate ? "Fertigation: water + nutrients" : "Irrigation: water only") +
            (t.length() ? String("\nStarted ") + t : String())); }
}

/* Abandon a pending/running Settings force sequence and say why. */
void forceAbort(const char *why) {
  if (fcPhase == FP_NONE) return;
  fcPhase = FP_NONE;
  logEvent("ESP1", "ACT", String("FORCE|ABORT|") + why);
  smsFault('m', "FORCE_ABORT", why);
  // A web-armed run that is aborted during its countdown must not leave the dashboard command
  // sitting on "accepted" -- it never ran, and the operator needs to see why.
  if (fcCmdId[0]) {
    firebaseQueueStatus(fcCmdId, "failed", (String("cancelled before start: ") + why).c_str());
    fcCmdId[0] = '\0';
  }
  fcFromWeb = false;
}

/* Core 1: run the armed Settings force sequence. The safety gate is re-checked HERE, at fire time,
 * not when the operator armed it -- a scheduled run, a fault, or a dropping reservoir during a
 * 5-minute countdown must stop it rather than let it fire late into a changed system. */
void forceTick() {
  if (fcPhase != FP_WAIT) return;
  if ((long)(millis() - fcFireAtMs) < 0) return;          // still counting down

  const char *bad = NULL;
  if      (actuationsDisabled)                                  bad = "ACTUATIONS_OFF";
  else if (sysState != IDLE_STATE)                              bad = "NOT_IDLE";
  else if (uiMode != UI_DATA)                                   bad = "IN_MENU";
  else if (webManualHeld())                                     bad = "WEB_MANUAL";
  else if (wo.active || pendingRun.active || pendingExercise.active) bad = "BUSY";
  else if (esp2Held)                                            bad = "HELD";
  // (No TANK_CHECK here since 2026-11-17: a forced run is never refused for it -- see sendForceWorkOrder.)
  else if (secBattBlocksAc())                                   bad = secBlockTag();   // which reason (2026-11-17)
  else if (reservoirLow(sensor.msTank != 0, sensor.resLevel, resPauseLevel())) bad = "RES_LOW";
  if (bad) { forceAbort(bad); return; }

  const ForceSeq &q = fcSeq[fcSlot];
  uint8_t mask = q.mask;                                  // resolved at arm time (LCD fcMode or web columns)
  for (int b = 0; b < NUM_COLUMNS; b++) if ((mask & (1 << b)) && !COLUMN_ENABLED[b]) mask &= ~(1 << b);
  if (!mask) { forceAbort("COL_DISABLED"); return; }

  float doseMl[3] = { (float)q.ml[0], (float)q.ml[1], (float)q.ml[2] };
  fcPhase = FP_RUNNING;
  // fcCmdId is "" for an LCD-armed run; sendForceWorkOrder treats that as "not from the dashboard".
  sendForceWorkOrder(mask, q.dL / 10.0f, doseMl, fcCmdId);
  // Ownership of the dashboard command passes to forceCmdId now that the run is live -- the DONE
  // and failure paths resolve it from there. Leaving a copy here would let a LATER abort (e.g. the
  // A-then-B second leg) report against a command that has already finished.
  fcCmdId[0] = '\0';
  // Refused (too small a batch to dose, say): no run exists, so nothing would ever move this off
  // FP_RUNNING, and every later force run, pulse and web manual hold would be refused until a reboot.
  if (!wo.active && fcPhase == FP_RUNNING) forceAbort("REFUSED");
}

/* Operator FORCE run: build a work order from explicit litres + explicit per-nutrient mL instead of
 * the schedule and calcDose(). Everything is already bounds-checked by the caller.
 * `mask` may name several columns -- ESP2 opens them together, so `liters` is the TOTAL batch.
 * fbCmdId is echoed to the dashboard when the run finishes ("" for an SMS-initiated force). */
void sendForceWorkOrder(uint8_t mask, float liters, const float doseMl[3], const char *fbCmdId) {
  if (webManualHeld()) {                             // web operator holds the rig -- no run may start
    logEvent("ESP1", "ACT", "FORCE|BLOCKED|WEB_MANUAL");
    if (fbCmdId && *fbCmdId) firebaseQueueStatus(fbCmdId, "rejected", "web manual mode is active");
    return;
  }
  { const char *bw;
    if (secBattBlocksAc(&bw)) {                      // every run uses the AC pumps -> needs the secondary
      logEvent("ESP1", "ACT", String("FORCE|BLOCKED|") + secBlockTag());
      if (fbCmdId && *fbCmdId) firebaseQueueStatus(fbCmdId, "rejected", bw);
      return;
    } }
  if (actuationsDisabled) {                          // last line of defence, as in sendWorkOrder
    logEvent("ESP1", "ACT", "FORCE|BLOCKED|ACTUATIONS_OFF");
    if (fbCmdId && *fbCmdId) firebaseQueueStatus(fbCmdId, "rejected", "actuations are disabled");
    return;
  }
  // Forced runs are not limited (user, 2026-11-17): the tank check no longer refuses them. The operator is starting the
  // run at the rig or on the dashboard, and in web v.4 every fill plans from the mixing tank's MEASURED, settled level
  // (ESP2 waterStageGate; with no level it holds LEVEL_BLIND instead of filling blind) -- so a saved volume that went
  // wrong in a lost run can no longer make it overfill. Starting a forced run counts as the confirmation.
  if (tankCheckNeeded) tankCheckClear("FORCE_RUN");
  int primary = 0;                                   // lowest column in the mask: budget + logging anchor
  while (primary < NUM_COLUMNS && !(mask & (1 << primary))) primary++;
  if (primary >= NUM_COLUMNS) return;

  bool fert = (doseMl[0] > 0.0f || doseMl[1] > 0.0f || doseMl[2] > 0.0f);
  // Same floor calcDose() enforces for the scheduled path (MIXING_TANK_SAFE_MIN) -- previously
  // unchecked on this path, so a FORCE run (dashboard, or the LCD editor which clamps down to
  // 0.5L) could dose nutrient into a batch both firmwares' own constants call unsafe, with no
  // refusal anywhere in the protocol.
  if (!forceBatchOk(liters, FLUSH_PCT, MIXING_TANK_SAFE_MIN, doseMl[0], doseMl[1], doseMl[2])) {
    logEvent("ESP1", "ACT", "FORCE|BLOCKED|BATCH_LOW");
    if (fbCmdId && *fbCmdId) firebaseQueueStatus(fbCmdId, "rejected", "batch too small to dose safely -- raise liters or remove nutrients");
    return;
  }
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
    // Same MIXING_TANK_MAX_VOLUME ceiling as the scheduled path's calcDose() -- see the identical
    // comment at sendWorkOrder's BATCHV. Dormant given FORCE_MAX_LITERS=20 << 50L today.
    float wireBatchV = liters * (1.0f - FLUSH_PCT / 100.0f);
    if (wireBatchV > MIXING_TANK_MAX_VOLUME) wireBatchV = MIXING_TANK_MAX_VOLUME;
    cmd += ",BATCHV," + String(wireBatchV, 1);
    cmd += ",EC," + String(EC_MIN, 1) + "," + String(EC_MAX, 1);
    cmd += ",PH," + String(PH_MIN, 1) + "," + String(PH_MAX, 1);
    cmd += ",MIX," + String(MIXING_DURATION_MS);
  }
  cmd += ",MIXCAP," + String(mixCapLiters(), 2);   // web v.4: no KMAIN (no main-line meters); the fill ceiling instead
  if (fert) {
    cmd += ",KNUT," + String(calKNut[0], 1) + "," + String(calKNut[1], 1) + "," + String(calKNut[2], 1);
    cmd += ",ECCAL," + String(calEcM, 6) + "," + String(calEcB, 4);
    cmd += ",PHCAL," + String(calPhM, 6) + "," + String(calPhB, 4);
  }
  cmd += "," + String(FRAME_END);
  // Safe to return early here -- unlike the scheduled path, nothing has been committed yet (no
  // wo, no ACTIVE_STATE, no run UI). forceTick() has set fcPhase = FP_RUNNING though, so release
  // it: forceAbort logs, alerts and resolves the dashboard command in one place.
  if (!woFrameFits(cmd, "FORCE")) { forceAbort("WO_OVERSIZE"); return; }

  wo.active = true; wo.colIdx = primary; wo.fertigate = fert;
  // A forced run counts for the window it falls in, if any, so that window doesn't water the column again.
  // Before several windows a day it used the whole day up; outside a window it now uses none (see DONE).
  wo.window = (int8_t)windowAt(primary, minuteOfDay());
  wo.cmd = cmd; wo.retries = 0;
  forceMask = mask;                                  // remembered so DONE can be attributed honestly
  strlcpy(forceCmdId, fbCmdId ? fbCmdId : "", sizeof(forceCmdId));

  setState(ACTIVE_STATE);
  runUiBegin(primary, fert, liters, doseMl, mask);   // force targets, NOT the scheduled budget; every column it waters
  esp2WarmupMs = millis();
  // The FORCE gate requires the system to be idle, and idle means ESP2 is POWERED OFF
  // (OFF-during-idle, sec.18.8) -- so this else branch is the normal case, not the exception.
  // Parking in WO_PENDING and letting READY transmit is the same wait a scheduled run does via
  // pendingRun; writing the frame here would push it into a UART nobody is listening on yet.
  if (esp2Available && esp2Powered) {
    wo.stage = WO_SENT; wo.sentMs = millis();
    esp2Serial.println(cmd);                         // already up -> send now
  } else {
    wo.stage = WO_PENDING;                           // READY (or the ACTIVE_STATE net) owns it now
    esp2SetPower(true);
  }

  logEvent("ESP1", "CMD", "ESP2|" + name + "|FORCE");
  logEvent("ESP1", "ACT", String(fert ? "FERTIGATION" : "IRRIGATION") + "|START|FORCE|COL_" + cols +
                          "|W=" + String(liters, 2) +
                          (fert ? "|mL=" + String(doseMl[0], 0) + "/" + String(doseMl[1], 0) + "/" + String(doseMl[2], 0) : ""));
  { String cl, t = smsClock();
    for (unsigned i = 0; i < cols.length(); i++) cl += String(i ? "+" : "") + cols[i];
    sendSMS(String("RUN (manual): Col ") + cl + "\n" + (fert ? "Fertigation: water + nutrients, " : "Irrigation: water only, ") +
            String(liters, 1) + "L" + (t.length() ? String("\nStarted ") + t : String())); }
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
// forceL >= 0 / forceDose != nullptr override the schedule for an operator FORCE run: the receipt,
// the LCD progress bars and the published run node must show what was actually ordered, not the
// scheduled budget the run is deliberately bypassing.
// The columns this run waters: "A", or "A+B" for a FORCE run that waters several at once (2026-11-18: the screens and
// the receipts named only the first). sep "+" on the LCD; "" in the log, "COL_AB" as the ACT rows already write it.
static String runCols(const char *sep) {
  uint8_t m = run.mask ? run.mask : (uint8_t)(1 << run.col);
  String s;
  for (int b = 0; b < NUM_COLUMNS; b++) if (m & (1 << b)) { if (s.length()) s += sep; s += COL_TAG[b]; }
  return s;
}
static bool runMulti() { return __builtin_popcount(run.mask) > 1; }
// Water out to the column(s) so far: the finished Release stages plus the one still moving water. ESP2 adds a Release
// stage's litres to its total only when the stage ends, so the irrigation bar used to sit at zero all run (2026-11-18);
// the dashboard already counts it the same way.
static bool runReleasing() { return !strcmp(run.stage, "Release") || !strcmp(run.stage, "Flush Release"); }
static float runWaterNow() { return run.waterGot + (runReleasing() ? run.stageL : 0.0f); }

void runUiBegin(int c, bool fert, float forceL, const float *forceDose, uint8_t mask) {
  memset(&run, 0, sizeof(run));
  run.fert = fert; run.col = c; run.ok = true;
  run.mask = mask ? mask : (uint8_t)(1 << c);
  run.total = fert ? ((FLUSH_PCT > 0) ? 7 : 5) : 2;
  run.ord = 0;
  strncpy(run.stage, "Starting", sizeof(run.stage) - 1);
  float budget = (forceL >= 0.0f) ? forceL : WATER_BUDGET_L[c];
  run.waterTgt = budget;
  run.batchV   = budget * (1.0f - FLUSH_PCT / 100.0f);
  if (run.batchV > MIXING_TANK_MAX_VOLUME) run.batchV = MIXING_TANK_MAX_VOLUME;   // same ceiling calcDose() applies
  if (fert) {
    if (forceDose) {
      // Mirror sendForceWorkOrder's DCEIL maths so the receipt and the work order agree.
      for (int i = 0; i < 3; i++) {
        run.doseTgt[i] = forceDose[i];
        run.ceilS[i]   = (forceDose[i] > 0.0f)
                       ? forceDose[i] / PUMP_FLOWRATE_MLPM[i] * 60.0f * CEILING_MARGIN : 0.0f;
      }
    } else {
      calcDose(c, run.doseTgt, run.ceilS);                    // computed once; sendWorkOrder reuses these
    }
  }
  for (int i = 0; i < 3; i++) run.npkBefore[i] = sensor.npk[c][4 + i];
  run.startMs = millis();
  runErrN = 0; runPage = 0;
  runPhase = RUN_PRE; runPhaseMs = millis(); runLocked = true;
  wakeBacklight();

  String d = String("PRE|COL_") + runCols("") + "|" + (fert ? "FERTIGATION" : "IRRIGATION")
           + "|PLANT=" + (runMulti() ? "-" : runPlant(c))
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

  String d = String("POST|COL_") + runCols("") + "|" + (run.fert ? "FERTIGATION" : "IRRIGATION")
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

// Progress bar of `cells` cells (8 by default), e.g. "[####----]". frac is clamped 0..1.
static String runBar(float frac, int cells = 8) {
  if (!(frac > 0)) frac = 0;
  if (frac > 1) frac = 1;
  int on = (int)(frac * cells + 0.5f);
  String s = "[";
  for (int i = 0; i < cells; i++) s += (i < on) ? '#' : '-';
  return s + "]";
}

// The run screens. Layout: header (operation / column / plant), stage line with the step counter,
// then the targets and a live detail row. Same shape for irrigation, minus the nutrient rows.
void lcdRenderRun() {
  char l[21];
  int  c = run.col;

  if (runPhase == RUN_PRE) {
    if (runMulti()) snprintf(l, 21, "RECEIPT  %s", runCols("+").c_str());               // A+B: one batch, both columns
    else            snprintf(l, 21, "RECEIPT  %c  %s", COL_TAG[c], runPlant(c));
    lcdRow(0, l);
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
    const char *op = run.fert ? "FERTIGATE" : "IRRIGATE";
    if (runMulti()) snprintf(l, 21, "%s %s", op, runCols("+").c_str());
    else            snprintf(l, 21, "%s %c %s", op, COL_TAG[c], runPlant(c));
    lcdRow(0, l);
    snprintf(l, 21, "%d/%d %s", run.ord, run.total, run.stage);                          lcdRow(1, l);
    if (run.fert) {
      snprintf(l, 21, "Tgt N%d P%d K%d", (int)col[c].targetN, (int)col[c].targetP, (int)col[c].targetK); lcdRow(2, l);
      // Detail row: during dosing show the nutrient mL, otherwise the water out so far (with the live Release).
      float frac = (run.stageTgt > 0) ? (run.stageL / run.stageTgt) : 0;
      if (run.ord == 2) snprintf(l, 21, "Nut %s %s", String(run.doseGot[0] + run.doseGot[1] + run.doseGot[2], 0).c_str(), runBar(frac).c_str());
      else              snprintf(l, 21, "%.1fL %s", runWaterNow(), runBar(frac).c_str());
      lcdRow(3, l);
    } else {
      snprintf(l, 21, "Target %.1f L", run.waterTgt);                                    lcdRow(2, l);
      if (run.ord == 1 && run.stageTgt > 0) {
        // Transfer Water: the fill into the mixing tank, over all its runs and settles (ESP2 reports the whole fill).
        snprintf(l, 21, "Fill %.1fL %s", run.stageL, runBar(run.stageL / run.stageTgt, 7).c_str());
      } else {
        // Release: the water out to the column(s), counting the stage still under way (2026-11-18: the bar sat at 0).
        float got = runWaterNow();
        float frac = (run.waterTgt > 0) ? (got / run.waterTgt) : 0;
        snprintf(l, 21, "%.1fL %s", got, runBar(frac).c_str());
      }
      lcdRow(3, l);
    }
    return;
  }

  if (runPhase == RUN_POST) {
    if (runMulti()) snprintf(l, 21, "RESULT %s %s", runCols("+").c_str(), run.ok ? "OK" : "!!");
    else            snprintf(l, 21, "RESULT %c %s %s", COL_TAG[c], runPlant(c), run.ok ? "OK" : "!!");
    lcdRow(0, l);
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
  smsFault('M', "SELF_RESET", reason);                 // best-effort (may not flush before reboot)
  logFlush(true);
  g_lastSelfResetDay = today;
  delay(60);                                            // let the last UART/SD bytes drain
  bswForgetPending(); ESP.restart();
}

/* Core 1: watch netTask's heartbeat (see netBeat). Alarm first, and only self-reset when the rig is
 * IDLE -- rebooting mid-run would abandon a live work order to fix a problem that costs nothing
 * while the run finishes. ESP2 keeps executing either way, so waiting is free. */
/* Core 1: own the manual-mode hold. Core 0 only reports what the page wrote; every decision --
 * grant, refuse, expire, revoke -- is made here, on the core that owns the run state being
 * protected. Each outcome calls fbRequestPush() so the page learns it in ~1 s instead of waiting up to a
 * minute for the next scheduled snapshot. */
void webManualTick() {
  bool ping = webManualPing; webManualPing = false;

  if (!webManualReq) {                                   // page released it (or never asked)
    webManualBlocked = false;                            // page stopped asking -> a fresh request may grant
    if (webManualUntilMs) {
      webManualUntilMs = 0;
      wmPeek = false; wmChooser = false;          // never resume a later hold mid-peek
      strlcpy(webManualState, "idle", sizeof(webManualState)); webManualWhy[0] = '\0';
      logEvent("ESP1", "ACT", "MANUAL|RELEASED|WEB");
      fbRequestPush();
    }
    return;
  }

  // Revoked at the LCD and the page has not caught up yet: refuse every ping until it releases.
  if (webManualBlocked) return;

  if (webManualHeld()) {                                 // already held: a fresh keep-alive extends it
    if (ping) webManualUntilMs = millis() + WEB_MANUAL_LEASE_MS;
    return;
  }

  // Not currently held. A repeat of the SAME seq must not re-ask: otherwise a page that was refused
  // would hammer the gate every poll and refill the log. Only a new keep-alive re-evaluates.
  if (!ping) {
    if (!strcmp(webManualState, "held")) {               // lease ran out with the page gone
      strlcpy(webManualState, "idle", sizeof(webManualState));
      wmPeek = false; wmChooser = false;
      logEvent("ESP1", "ACT", "MANUAL|EXPIRED");
      fbRequestPush();
    }
    return;
  }

  // Grant only from a genuinely idle rig -- the same gate the timed pulse uses. The reason is
  // published so the page can say "wait for the run to finish" rather than a bare failure.
  const char *why = NULL;
  if      (sysState != IDLE_STATE || wo.active || pendingRun.active) why = "a run is in progress";
  else if (esp2Held)            why = "a fault is held";
  else if (fcPhase != FP_NONE)  why = "a forced run is armed";
  else if (pendingExercise.active || puPhase != PU_NONE) why = "a test is already running";
  // Someone is USING the LCD. sysState alone does not catch this: Calibration and Sensor Diag both
  // leave sysState == IDLE_STATE, so without this the web could seize control mid-calibration --
  // freezing the operator's screen and swallowing their buttons while a prime pump ran under the
  // dead-man. Exclusion has to work in both directions to be worth anything.
  else if (uiMode != UI_DATA)   why = "someone is using the LCD";
  if (why) {
    strlcpy(webManualState, "refused", sizeof(webManualState));
    strlcpy(webManualWhy, why, sizeof(webManualWhy));
    logEvent("ESP1", "ACT", String("MANUAL|REFUSED|") + why);
    fbRequestPush();
    return;
  }
  webManualUntilMs = millis() + WEB_MANUAL_LEASE_MS;
  strlcpy(webManualState, "held", sizeof(webManualState)); webManualWhy[0] = '\0';
  logEvent("ESP1", "ACT", "MANUAL|GRANTED|WEB");
  wakeBacklight();                                       // the banner is about to appear
  fbRequestPush();
}

/* The person at the rig taking control back. Ends any pulse in flight through the normal path so a
 * relay is never left energised by the takeover. */
void webManualRevoke() {
  if (!webManualHeld()) return;
  if (puPhase != PU_NONE) pulseFinish(false, "web manual mode revoked at the LCD");
  webManualUntilMs = 0;
  wmPeek = false; wmChooser = false;
  webManualBlocked = true;                     // do not re-grant until the page stops asking
  strlcpy(webManualState, "revoked", sizeof(webManualState));
  strlcpy(webManualWhy, "the rig operator took control", sizeof(webManualWhy));
  logEvent("ESP1", "ACT", "MANUAL|REVOKED|LCD");
  smsFault('m', "MANUAL_REVOKED", "");
  fbRequestPush();
}

void netHealthTick() {
  static uint32_t      lastBeat   = 0;
  static unsigned long lastBeatMs = 0;
  static bool          warned     = false;

  if (!lastBeatMs) lastBeatMs = millis();          // first call: start the clock, don't judge yet
  uint32_t beat = netBeat;
  if (beat != lastBeat) {                          // alive -> reset the timer and the alarm latch
    lastBeat = beat; lastBeatMs = millis();
    if (warned) { warned = false; logEvent("ESP1", "NET", "NET_STALL|RECOVERED"); }
    return;
  }
  // netTask parks on vTaskDelay in several normal paths, so only judge it stalled when it has had
  // every opportunity to tick. The 5 min threshold is far longer than any legitimate wait.
  unsigned long stalled = millis() - lastBeatMs;
  if (!warned && stalled >= NET_STALL_WARN_MS) {
    warned = true;
    // Log + SMS directly rather than via raiseFault(). raiseFault preempts the LCD to PAGE_FAULT and
    // tears down an in-progress Testing session (TEST,EXIT + power off) for EVERY tier, and a purely
    // network problem must not do that: it affects no actuator, and dumping an operator out of
    // Calibration or a relay test to report that the dashboard is offline would be worse than the
    // fault. The FAULT type still puts it in the SD log and the web event ring, and the SMS goes out
    // over the GSM path, which is independent of the task that just stalled.
    logEvent("ESP1", "FAULT", "WARN|NET_STALL|NETTASK");
    smsFault('W', "NET_STALL", "");
    return;
  }
  // Reboot only when there is genuinely nothing to lose: no run, no held fault, no armed force
  // countdown, and nobody part-way through a menu (a reboot mid-calibration would discard their
  // work to fix a problem that only costs remote visibility). Anything else simply waits -- the
  // stall persists, and the next tick re-checks.
  if (warned && stalled >= NET_STALL_RESET_MS &&
      sysState == IDLE_STATE && !wo.active && !pendingRun.active && !esp2Held &&
      fcPhase == FP_NONE && uiMode == UI_DATA && !portalActive) {
    esp1SelfResetOncePerDay("NET_STALL");           // once-per-day capped -> cannot boot-loop
  }
}

/* ---- Dead-RTC daily reboot ------------------------------------------------ *
 * When the RTC is unreadable, tsString() stamps every row "0000-00-00 00:00:00" and logs land in
 * NODATE.CSV -- scheduling is dead too (controlTick bails on !rtcOk), so the rig silently does nothing.
 * A reboot re-runs the I2C/RTC init and often clears a wedged bus. Reboot at most once every 24 h of
 * uptime while the timestamp stays all-zero, and ONLY when truly idle with nothing pending -- never
 * mid-run, mid-fault-hold, or while the operator is in a menu / the WiFi portal.
 * The 24 h clock is uptime-based on purpose: with a dead RTC there is no calendar day to key off, and
 * millis() resets on reboot, so the next attempt is naturally ~24 h later (no boot loop).
 * Not for a clock that merely lost its time: the chip works, so only setting it helps.               */
const unsigned long RTC_DEAD_REBOOT_MS = 86400000UL;   // 24 h of continuous all-zero timestamp
unsigned long rtcDeadSinceMs = 0;                       // 0 = timestamp currently valid
void rtcDeadRebootTick() {
  if (rtcOk || rtcTimeLost) { rtcDeadSinceMs = 0; return; }   // timestamp valid, or a reboot cannot fix it
  if (rtcDeadSinceMs == 0) { rtcDeadSinceMs = millis(); return; }   // start the 24 h clock
  if (millis() - rtcDeadSinceMs < RTC_DEAD_REBOOT_MS) return;
  // Idle-only, nothing pending, operator not busy -> otherwise wait and retry on a later tick.
  if (sysState != IDLE_STATE || wo.active || pendingRun.active || pendingExercise.active
      || esp2Held || uiMode != UI_DATA || portalActive) return;
  logEvent("ESP1", "RESET", "SELF|RTC_TS_ZERO|24h_idle");
  logFlush(true);
  delay(60);                                            // let the last UART/SD bytes drain
  bswForgetPending(); ESP.restart();
}

// Returns whether it actually cycled ESP2 (false = suppressed by cooldown). esp2Escalate() uses this so
// a suppressed call can never be miscounted as one of the ESP2_MAX_FAST_CYCLES attempts, and can never
// leave sysState stranded in RECOVERY_STATE re-triggering the escalate-timeout branch on every loop tick.
bool esp2PowerCycle() {
  if (millis() - lastRecoveryMs < RECOVERY_COOLDOWN_MS) return false;
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
  return true;
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
  if (!esp2PowerCycle()) {
    // Suppressed by cooldown: don't count it, and re-arm the soft-reset-timeout clock so RECOVERY_STATE's
    // timeout branch (and its logEvent) waits another ESP2_SOFT_RESET_TIMEOUT_MS instead of re-firing on
    // every loop tick until the cooldown clears on its own.
    esp2RecoverMs = millis();
    return;
  }
  esp2FastCycles++;
}

// ESP2 comm confirmed again (READY / STARTUP_SYNC validate): drop the episode counter and clear the latch.
void esp2CommRecovered() {
  esp2FastCycles = 0;
  if (esp2CommLost) {
    esp2CommLost = false;
    logEvent("ESP1", "RESET", "ESP2|COMM_RECOVERED");
    String t = smsClock();
    smsAlert(String("OK: Pump controller link is back.\nIt is answering again.\n(") + (t.length() ? t + " " : String()) + "ESP2_COMM_OK)");
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
    esp2BattWaitUntil = 0;
    if (esp2Powered) return;
    digitalWrite(ESP2_PWR_PIN, HIGH);
    esp2Powered = true; esp2OnMs = millis();
    return;
  }
  if (!esp2Powered) { esp2OffPending = false; return; }
  // ESP2 is holding the boards on the backup battery. Turning it off now is what would put them on the
  // flat main battery; backupTick decides when that is allowed. force (E-stop) still cuts.
  if (!force && backupHoldsEsp2()) { esp2OffPending = false; return; }
#if SECONDARY_BATT_ENABLED
  // ESP2 has the boards on the secondary and returns them to main ~1 s after the inverter stops.
  // Cutting its power now would drop the DPDT at once instead, so hold a graceful off until it
  // reports INFO,BATT,MAIN, bounded by ESP2_BATT_RETURN_WAIT_MS. force (E-stop) never waits.
  if (!force && esp2OnSecondary) {
    if (!esp2BattWaitUntil) esp2BattWaitUntil = millis() + ESP2_BATT_RETURN_WAIT_MS;
    if ((long)(millis() - esp2BattWaitUntil) < 0) { esp2OffPending = true; esp2OffAt = esp2BattWaitUntil; return; }
  }
#endif
  if (!force && millis() - esp2OnMs < ESP2_MIN_ON_MS) {    // hold the minimum on-time before turning off
    esp2OffPending = true; esp2OffAt = esp2OnMs + ESP2_MIN_ON_MS;
    return;
  }
  digitalWrite(ESP2_PWR_PIN, LOW);
  esp2Powered = false; esp2OffPending = false;
  levelWatchStop("ESP2_OFF");                      // no inverter, no pump: the Nano goes back to normal
  esp2OnSecondary = false; esp2BattWaitUntil = 0;   // PCF unpowered -> P0 drops -> boards on main
  bswForgetPending();                              // our own cut, not a switch failure
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
  // Signed difference, not `millis() >= esp2OffAt`. esp2OffAt is esp2OnMs + ESP2_MIN_ON_MS, so if
  // ESP2 is powered on within 10 s of the millis() wrap that sum wraps to a small number and the
  // plain comparison is immediately true -- cutting ESP2's power during the very minimum-on hold
  // that exists to stop it being cut before it can boot. Once per 49.7 days, and this rig is meant
  // to run unattended for months.
  if (esp2OffPending && esp2Powered && (long)(millis() - esp2OffAt) >= 0) {
    // Something may have started using ESP2 during this deferred-off window -- controlTick(),
    // sendForceWorkOrder(), and exerciseTick() all have an "already up, dispatch now" fast path that
    // sends straight over the wire without going through esp2SetPower(true) (the only place that
    // normally clears esp2OffPending). If that happened, cutting power now would be a raw,
    // ungraceful GPIO4 cut mid-actuation that bypasses the fault-hold protocol entirely -- no P17
    // drop through ESP2's own holdFault(), no mixTankL persistence -- reached with no fault involved
    // at all. Cancel the stale deferred cut instead; every real completion path (DONE/exercise-done)
    // already calls esp2SetPower(false) directly once the work genuinely finishes.
    if (wo.active || pendingRun.active || pendingExercise.active || esp2Held) esp2OffPending = false;
    else esp2SetPower(false);
  }
  // uiMode + diagRemoteUntilMs: Calibration, Sensor Diag and a remote sweep all use ESP2 at IDLE_STATE. A
  // poll finishing underneath one would power ESP2 off in the middle of it.
  if (sysState != IDLE_STATE || pendingRun.active || pendingExercise.active || esp2Held || esp2CommLost ||
      uiMode != UI_DATA || diagRemoteUntilMs || backupHoldsEsp2()) { esp2PollActive = false; return; }
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

// Warm-up complete (ESP2 READY): transmit a work order that was built while ESP2 was still off.
// Only FORCE runs park in WO_PENDING -- scheduled runs wait in pendingRun and are built after READY.
// Deliberately does NOT wait on the pre-run receipt the way dispatchPendingRun() does: a force run
// is operator-initiated from the dashboard, so there may be nobody at the LCD to acknowledge it,
// and the 20 s receipt timeout would outlast ESP2's 8 s warm-up budget and abort the run.
void dispatchPendingWorkOrder() {
  if (!wo.active || wo.stage != WO_PENDING) return;
  wo.stage = WO_SENT; wo.sentMs = millis();
  esp2Serial.println(wo.cmd);
  logEvent("ESP1", "CMD", "ESP2|FORCE_DISPATCH");   // frame already logged at build time
}

// Warm-up complete (ESP2 READY): send the queued scheduled run, then clear the pending slot.
void dispatchPendingRun() {
  if (!pendingRun.active) return;
  if (runPhase == RUN_PRE) return;      // hold until the pre-run receipt is acknowledged / times out
  int c = pendingRun.colIdx; bool f = pendingRun.fertigate;
  int8_t w = pendingRun.window;
  pendingRun.active = false; pendingRun.colIdx = -1; pendingRun.window = -1;
  sendWorkOrder(c, f);
  if (wo.active && wo.colIdx == c) wo.window = w;   // DONE marks this window watered (markServiced)
}

// Warm-up complete: send the EXERCISE command for the selected pump. esp2WarmupMs is
// repurposed as the run-completion timeout once sent (see stateMachineTick ACTIVE_STATE).
void dispatchPendingExercise() {
  if (!pendingExercise.active || pendingExercise.sent) return;
  pendingExercise.sent = true;
  esp2WarmupMs = millis();                       // now timing warmup + run -> DONE,EXERCISE
  // sendEsp2 logs the CMD,ESP2|EXERCISE intent line. START is logged only once ESP2 confirms receipt
  // (ACK,EXERCISE, handleEsp2Response), so the log reflects a genuine command reception, not just dispatch.
  // Carry the operator-set run length with the command so one setting covers every pump; ESP2
  // clamps it again on arrival rather than trusting the wire.
  sendEsp2(String("EXERCISE,") + EX_NAME[pendingExercise.idx] + "," + String(exerciseRunS * 1000));
}

// Preventive pump exercise (sec.14.9.1): when fully idle, if a pump has not run for
// PUMP_EXERCISE_INTERVAL_MS, power ESP2 up and have it run that pump briefly. Lowest
// priority -- controlTick runs first, so a due column always wins.
void exerciseTick() {
  if (webManualHeld()) return;            // web operator holds the rig; nothing auto-starts
  if (!exerciseEnabled) return;           // operator switched the preventive exercise off
  if (actuationsDisabled) return;         // operator lockout: no preventive pump runs either
  if (sysState != IDLE_STATE) return;
  if (uiMode != UI_DATA) return;
  if (wo.active || pendingRun.active || pendingExercise.active) return;
  bool acBlocked = secBattBlocksAc();      // transfer/booster run the inverter -> need the secondary
#if BATTERY_SAFETY_ENABLED
  if (batteryLow || batteryCritical) return;   // don't exercise on a weak battery (READY, not active)
#endif
  for (int k = 0; k < 3; k++) {
    if (k != 2 && acBlocked) continue;     // k 2 = the 12 V DC mixer: no inverter, no secondary needed
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
 *  GSM  (A7670C; the modem logic lives in include/gsm_modem.h, this is the wiring)
 * ========================================================================== */
static void gsmWrite(const char *data, size_t len) { simSerial.write((const uint8_t *)data, len); }
static void gsmSetBaud(uint32_t baud) { simSerial.updateBaudRate(baud); }
static void gsmLog(const char *detail) { logEvent("GSM", "GSM", detail); }

static void gsmOnSms(const char *sender, const char *body) {
  char safe[GSM_BODY_MAX];
  gsmSafeLogBody(body, safe, sizeof(safe));          // this row reaches the SD, Supabase and the dashboard
  logEvent("GSM", "GSM", String("RX|") + safe);
  replyTarget = sender;
  handleSms(body);                                   // replies enqueue with replyTarget (the sender)
  replyTarget = "";                                  // back to owner PHONE_NUMBER for autonomous msgs
  wakeBacklight();
}

static void gsmOnUssd(bool ok, const char *text) {
  // ERROR here usually means the carrier/IMS refused USSD. A long carrier answer is split by smsEnqueueSplit.
  sendSMS(ok ? String("SIM BALANCE:\n") + text
             : String("SIM BALANCE check failed: ") + text + "\nThe carrier refused it or the code is wrong (USSD).");
}

void gsmBegin() {
  static const GsmModem::Callbacks cb = { gsmWrite, gsmSetBaud, gsmOnSms, gsmOnUssd, gsmLog };
  gsm.begin(cb, millis());
  simSerial.setRxBufferSize(SIM_RX_BUFFER);          // must precede begin()
  simSerial.begin(gsm.baud(), SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
}

void pollGSM() {
  while (simSerial.available()) gsm.feed((char)simSerial.read(), millis());
}

// Weekly (and on demand): prove the SIM can still reach the owner. The text itself is the check -- a
// week without one means the SIM, its load or the modem needs looking at.
void simHealthCheck() {
  bool known = lastRssi >= 0 && lastRssi != 99;
  String m = "SIM CHECK (weekly)\nSignal " + (known ? String(lastRssi) + "/31 " : String()) +
             (!known ? "unknown" : lastRssi >= 20 ? "excellent" : lastRssi >= 15 ? "good" : lastRssi >= 10 ? "fair" : "weak");
  m += netRegistered ? ", network OK" : ", NO network (" + String(lastCreg) + ")";
  m += "\nTexts sent " + String(gsm.txOk()) + ", failed " + String(gsm.txGaveUp());
  m += "\nDelivered " + String(gsm.delivered()) + ", not delivered " + String(gsm.undelivered());
  m += "\nModem restarts: " + String(gsm.resets());
  if (gsmSeptets(m.c_str()) + 40 <= SMS_MAX) m += "\nNo text next week = check the SIM load.";
  sendSMS(m);
  if (USSD_CODE.length()) gsm.requestUssd(USSD_CODE.c_str());
}

// Parse compact comma commands (spec sec.12.2). Keyword match is case-insensitive.
void handleSms(const String &body) {
  String b = body; b.trim();
  String U = b; U.toUpperCase();

  // Owner-gate EVERY command: only the configured owner number may command the system (STATUS/NET/
  // SUMMARY included). senderIsOwner() returns true for internally-generated calls (replyTarget empty,
  // e.g. the deferred-config replay), so those still pass. Non-owner -> ERR,AUTH.
  if (!senderIsOwner()) {
    // Every reply costs load. Short codes and promo senders get none (a failed reply to one also counts
    // against the modem watchdog), and anyone else at most one per SMS_AUTH_REPLY_GAP_MS, so a stranger
    // cannot drain the SIM by texting it. A mistyped owner number still gets told.
    static unsigned long lastAuthReplyMs = 0;
    int digits = 0;
    for (unsigned i = 0; i < replyTarget.length(); i++) if (isDigit(replyTarget[i])) digits++;
    if (digits >= 9 && (!lastAuthReplyMs || millis() - lastAuthReplyMs >= SMS_AUTH_REPLY_GAP_MS)) {
      lastAuthReplyMs = millis();
      smsErr("AUTH", "Sorry, this number is not allowed to control the irrigation system.");
    } else logEvent("GSM", "SMS_ERR", "AUTH|NO_REPLY");
    return;
  }

  // Defer a remote config-write that collides with an open LOCAL edit (companion spec §B.3.1):
  // apply it once the operator finishes the edit, so neither silently overwrites the other.
  if ((uiMode == UI_EDIT || editConfirm) &&
      (U.startsWith("SET") || U.startsWith("MODE") || U.startsWith("NAME"))) {
    pendingCfgSms = b;
    sendSMS("BUSY: Someone is editing settings on the LCD.\nYour change will be applied when they finish.");
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
    if (U == "SERVO")    { issueRecovery(3); return; }   // pump-selector hold: switch it again (= NORMAL, re-seats)
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
    else { smsErr("CANCEL|FORMAT", "NOT DONE: Unknown CANCEL option.\nCANCEL,30 or CANCEL,10 = end the run, skip the column 30/10 min\nCANCEL,OFF = end it, column off"); return; }
    if (!wo.active && !pendingRun.active && !esp2Held) { smsErr("CANCEL|NO_RUN", "NOT DONE: Nothing is running, so there is nothing to cancel."); return; }
    cancelRun(mode, "SMS");
    return;
  }

  // STOP,ALL  (global emergency stop when nothing is held)
  if (U.startsWith("STOP")) {
    enterEmergencyStop(false);
    sendSMS("OK: EMERGENCY STOP\nAll pumps and valves are stopped.\nClear it on the LCD to resume.");
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
      saveSupa(); sendSMS("OK: Supabase login cleared. Log uploads are off."); return;
    }
    int c2 = rest.indexOf(',');                          // URL , KEY (key may not contain a comma; JWTs don't)
    if (c2 <= 0) { smsErr("SUPA|FORMAT", "NOT DONE: Wrong format.\nUse SUPA,<project url>,<service key>\nor SUPA,CLEAR"); return; }
    String u = rest.substring(0, c2); u.trim();
    String k = rest.substring(c2 + 1); k.trim();
    u = supaNormalizeUrl(u);                        // collapse any pasted path down to the origin
    if (!u.startsWith("http") || k.length() < 20) { smsErr("SUPA|FORMAT", "NOT DONE: Wrong format.\nUse SUPA,<project url>,<service key>\nor SUPA,CLEAR"); return; }
    xSemaphoreTake(netMux, portMAX_DELAY); supaUrl = u; supaKey = k; xSemaphoreGive(netMux);
    saveSupa();
    logEvent("ESP1", "CFG", "SUPA|SET");                 // note: NO url/key in the log
    sendSMS("OK: Supabase login saved.");
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
      saveFb(); sendSMS("OK: Firebase address cleared. The website stops updating."); return;
    }
    if (rest.equalsIgnoreCase("ON") || rest.equalsIgnoreCase("OFF")) {
      bool en = rest.equalsIgnoreCase("ON");
      xSemaphoreTake(netMux, portMAX_DELAY); firebaseEnabled = en; xSemaphoreGive(netMux);
      saveFb();
      logEvent("ESP1", "CFG", String("FBASE|") + (en ? "ON" : "OFF"));
      sendSMS(en ? "OK: Website sync (Firebase) is ON." : "OK: Website sync (Firebase) is OFF.");
      return;
    }
    // FBASE,SIGNOUT -- wipe every credential and the live token. The dashboard goes read-dead
    // until re-provisioned; use this if the device is lost or the account is rotated.
    if (rest.equalsIgnoreCase("SIGNOUT")) {
      xSemaphoreTake(netMux, portMAX_DELAY);
      // fbUid/fbAuthErr moved inside this section too -- both are Strings also read/written from
      // core 0 (netTask) without netMux (internally consistent there, single task), so writing them
      // from this core-1 SMS handler unguarded was a genuine cross-core String data race (heap
      // pointer/length), the exact hazard class this file's own comments warn about elsewhere.
      fbPassword = ""; fbRefresh = ""; fbIdToken = ""; fbUid = ""; fbAuthErr = "";
      xSemaphoreGive(netMux);
      fbTokenExpiryMs = 0; fbAuthOk = false;
      saveFbAuth();
      logEvent("ESP1", "CFG", "FBASE|SIGNOUT");
      sendSMS("OK: Signed out of Firebase. Saved logins erased.\nThe website stops updating until you sign in again.");
      return;
    }
    // FBASE,STATUS -- why is it not connecting? Reports the blocking precondition, the last
    // Google error, and the UID. Safe to send: no secret is ever echoed.
    if (rest.equalsIgnoreCase("STATUS")) {
      // netMux: these 7 Strings are also written from core-0 netTask (fbRefreshToken/
      // fbPasswordSignIn/fbNoteAuthError/fbNoteAuthLocal/the portal admin form, etc.). Copy them
      // under the lock first, then build the reply from the local copies -- fbUid/fbAuthErr
      // especially: reading their live buffer content (not just .length()) while core 0 is
      // concurrently reassigning either is a genuine use-after-free/torn-read hazard, not just a
      // stale value. This block previously took no lock at all.
      String uKey, uMail, uRefresh, uPass, uUrl, uUid, uErr;
      xSemaphoreTake(netMux, portMAX_DELAY);
      uKey = fbApiKey; uMail = fbEmail; uRefresh = fbRefresh; uPass = fbPassword;
      uUrl = fbUrl; uUid = fbUid; uErr = fbAuthErr;
      xSemaphoreGive(netMux);
      String m = String("FIREBASE: ") + (fbAuthOk ? "signed in" : "NOT signed in");
      const char *block = !firebaseEnabled                      ? "sync is OFF (text FBASE,ON)"
                        : uKey.length() < 8                     ? "no API key (FBASE,APIKEY)"
                        : !uMail.length()                       ? "no email (FBASE,AUTH)"
                        : (!uRefresh.length() && !uPass.length()) ? "no password (FBASE,AUTH)"
                        : !wifiConnected                        ? "no WiFi (text NET)"
                        : uUrl.length() < 8                     ? "no address (FBASE,<url>)" : nullptr;
      if (block) m += String("\nBlocked: ") + block;
      // The last snapshot upload's HTTP code (the sign-in code below is only the SIGN-IN's). Without it a
      // signed-in device whose every upload is refused (wrong account vs the rules -> 401) reported
      // as healthy here. 204/200 = ok, 401 = rules refused this account, <0 = no TLS/TCP, 0 = none yet.
      int up = fbLastHttp;
      m += "\nLast upload: " + ((up == 200 || up == 204) ? String("OK")
                             : up == 401 ? String("refused by the rules (401)")
                             : up < 0    ? "no connection (" + String(up) + ")"
                             : up == 0   ? String("none yet") : "code " + String(up));
      m += "\nSign-in code: " + String(fbAuthHttp);
      m += fbPinCa ? "\nCertificate check: ON" : "\nCertificate check: OFF (test only)";
      if (uErr.length()) m += "\n" + smsCut("Error: " + uErr, SMS_MAX - gsmSeptets(m.c_str()) - 1);
      if (uUid.length() && gsmSeptets(m.c_str()) + 6 + (int)uUid.length() <= SMS_MAX) m += "\nUID: " + uUid;
      sendSMS(m);
      return;
    }
    // FBASE,TLS,ON|OFF -- toggle certificate validation. OFF is a DIAGNOSTIC: if sign-in only
    // works with it off, the CA pin is the problem, not your credentials. Put it back on.
    if (rest.startsWith("TLS,") || rest.startsWith("tls,")) {
      String v = rest.substring(4); v.trim();
      if (!v.equalsIgnoreCase("ON") && !v.equalsIgnoreCase("OFF")) { smsErr("FBASE|FORMAT", "NOT DONE: Wrong format. Use one of:\nFBASE,<url>  FBASE,ON / OFF\nFBASE,STATUS  FBASE,SIGNOUT\nFBASE,TLS,ON / OFF  FBASE,APIKEY,<key>\nFBASE,AUTH,<email>,<password>"); return; }
      xSemaphoreTake(netMux, portMAX_DELAY); fbPinCa = v.equalsIgnoreCase("ON"); xSemaphoreGive(netMux);
      fbTlsResetPending = true;                         // netTask owns the TLS client; tearing it down from
                                                        // here could free it in the middle of a request
      fbSignInPending = true; fbAuthFails = 0; fbAuthNextTryMs = 0;
      saveFbAuth();
      logEvent("ESP1", "CFG", String("FBASE|TLS|") + (fbPinCa ? "PIN" : "INSECURE"));
      sendSMS(fbPinCa ? "OK: Firebase certificate check is ON."
                      : "OK: Firebase certificate check is OFF.\nFor testing only. Turn it back on with FBASE,TLS,ON");
      return;
    }
    // FBASE,APIKEY,<webApiKey>
    if (rest.startsWith("APIKEY,") || rest.startsWith("apikey,")) {
      String k = rest.substring(7); k.trim();
      if (k.length() < 8) { smsErr("FBASE|FORMAT", "NOT DONE: Wrong format. Use one of:\nFBASE,<url>  FBASE,ON / OFF\nFBASE,STATUS  FBASE,SIGNOUT\nFBASE,TLS,ON / OFF  FBASE,APIKEY,<key>\nFBASE,AUTH,<email>,<password>"); return; }
      xSemaphoreTake(netMux, portMAX_DELAY); fbApiKey = k; xSemaphoreGive(netMux);
      saveFbAuth();
      logEvent("ESP1", "CFG", "FBASE|APIKEY");          // note: NO key in the log
      sendSMS("OK: Firebase API key saved.");
      return;
    }
    // FBASE,AUTH,<email>,<password> -- provisioning. Password is used once to mint a refresh
    // token, then FBASE,SIGNOUT or the portal's Forget button can remove it.
    if (rest.startsWith("AUTH,") || rest.startsWith("auth,")) {
      String rem = rest.substring(5);
      int c2 = rem.indexOf(',');
      if (c2 <= 0) { smsErr("FBASE|FORMAT", "NOT DONE: Wrong format. Use one of:\nFBASE,<url>  FBASE,ON / OFF\nFBASE,STATUS  FBASE,SIGNOUT\nFBASE,TLS,ON / OFF  FBASE,APIKEY,<key>\nFBASE,AUTH,<email>,<password>"); return; }
      String em = rem.substring(0, c2); em.trim();
      String pw = rem.substring(c2 + 1); pw.trim();
      if (em.indexOf('@') < 0 || pw.length() < 6) { smsErr("FBASE|FORMAT", "NOT DONE: Wrong format. Use one of:\nFBASE,<url>  FBASE,ON / OFF\nFBASE,STATUS  FBASE,SIGNOUT\nFBASE,TLS,ON / OFF  FBASE,APIKEY,<key>\nFBASE,AUTH,<email>,<password>"); return; }
      xSemaphoreTake(netMux, portMAX_DELAY);
      fbEmail = em; fbPassword = pw; fbRefresh = "";   // new creds void any stored refresh token
      xSemaphoreGive(netMux);
      saveFbAuth();
      fbSignInPending = true; fbAuthFails = 0; fbAuthNextTryMs = 0;
      logEvent("ESP1", "CFG", "FBASE|AUTH");           // note: NO email/password in the log
      sendSMS("OK: Firebase email and password saved. Signing in now.\nText FBASE,STATUS in a minute to check.");
      return;
    }
    String u = supaNormalizeUrl(rest);                   // same origin-only reduction as SUPA
    if (!u.startsWith("http")) { smsErr("FBASE|FORMAT", "NOT DONE: Wrong format. Use one of:\nFBASE,<url>  FBASE,ON / OFF\nFBASE,STATUS  FBASE,SIGNOUT\nFBASE,TLS,ON / OFF  FBASE,APIKEY,<key>\nFBASE,AUTH,<email>,<password>"); return; }
    xSemaphoreTake(netMux, portMAX_DELAY); fbUrl = u; firebaseEnabled = true; xSemaphoreGive(netMux);
    saveFb();
    logEvent("ESP1", "CFG", "FBASE|SET");                // note: NO url in the log
    sendSMS("OK: Firebase address saved. Website sync is ON.");
    return;
  }
  // UPLOAD             -> queue yesterday's CSV.  UPLOAD,YYYYMMDD -> queue that specific day.
  // 2026-11-17: through Firebase when website sync is on (the Supabase project expired) -- today included, sent as far as
  // it is written, like the website's "Upload from the rig". Supabase only while Firebase is off, completed days only.
  if (U.startsWith("UPLOAD")) {
    long stamp = 0;
    int c1 = b.indexOf(',');
    if (c1 >= 0) { String d = b.substring(c1 + 1); d.trim(); if (d.length() == 8) stamp = d.toInt(); }
    else if (rtcOk && currentDayStamp > 0) {             // no arg -> yesterday
      DateTime y = rtcNow() - TimeSpan(1, 0, 0, 0);
      stamp = dayStamp(y);
    }
    xSemaphoreTake(netMux, portMAX_DELAY);
    bool suHave = supaUrl.length() > 0, fbHave = firebaseEnabled && fbUrl.length() >= 8;
    xSemaphoreGive(netMux);
    if (!suHave && !fbHave) { smsErr("UPLOAD|NOCREDS", "NOT DONE: Log upload is not set up.\nTurn on website sync (FBASE,ON) first."); return; }
    if (stamp <= 0) { smsErr("UPLOAD|DAY", "NOT DONE: Unknown day.\nUse UPLOAD (= yesterday) or UPLOAD,YYYYMMDD e.g. UPLOAD,20261115"); return; }
    if (rtcOk && stamp == currentDayStamp && !fbHave) {  // Supabase: never the open file
      smsErr("UPLOAD|TODAY", "NOT DONE: Today's log is still being written.\nOnly past days can be uploaded."); return;
    }
    char path[16]; snprintf(path, sizeof(path), "/%08ld.CSV", stamp);
    bool exists;
    { bool got = sdTake(200); exists = got && SD.exists(path); if (got) sdGive(); }
    if (!exists) { smsErr((String("UPLOAD|NOFILE|") + stamp).c_str(), "NOT DONE: There is no log for " + smsDate(stamp) + " on the SD card."); return; }
    manualUploadStamp = stamp;                           // uploadTick prioritizes this
    sendSMS("OK: The log for " + smsDate(stamp) + " is queued for upload." +
            (fbHave ? String("\nDownload it on the website: Diagnostics > SD card logs.") : String()));
    return;
  }
  // STATUS
  if (U == "STATUS") {
    if (sysState == ACTIVE_STATE) {
      String prog = (wo.active && wo.colIdx >= 0)
        ? String("BUSY: ") + (wo.fertigate ? "Fertigation" : "Irrigation") + " running on " + smsCol(wo.colIdx) + "."
        : String("BUSY: The system is running.");
      sendSMS(prog + "\nThe full report will follow when it finishes.");
      reportPending = true;                 // deferred full report (sec.12.1.3.1)
    } else {
      sendDailyReport();
    }
    return;
  }
  // TANKCHECK -- is a tank check outstanding? TANKCHECK,OK -- the operator has looked at the mixing tank.
  if (U == "TANKCHECK" || U == "TANKCHECK,OK") {
    if (U == "TANKCHECK,OK") {
      if (sysState != IDLE_STATE) { smsErr("TANKCHECK|NOT_IDLE", "NOT DONE: Wait until the system is idle, then send TANKCHECK,OK again."); return; }
      bool was = tankCheckNeeded;
      tankCheckClear("SMS");
      sendSMS(was ? "OK: Tank check confirmed. Runs can start again." : "OK: No tank check was needed.");
      return;
    }
    sendSMS(tankCheckNeeded ? "TANK CHECK NEEDED\nLook at the mixing tank. If it holds water, empty it with Testing (booster).\nThen text TANKCHECK,OK"
                            : "TANK CHECK: not needed. All good.");
    return;
  }
  // BATTSW -- battery-switch status; BATTSW,CLEAR -- clear a latched switch fault (re-arms switching).
  if (U == "BATTSW" || U == "BATTSW,CLEAR") {
    if (U == "BATTSW,CLEAR") {
      bool was = bswLatched;
      bswClear("SMS");
      sendSMS(was ? "OK: Battery switch fault cleared. AC pumps are allowed again." : "OK: There was no battery switch fault to clear.");
      return;
    }
    String r = String("BATTERY SWITCH\nBackup switching: ") + (SECONDARY_BATT_ENABLED ? "on" : "off");
    r += bswLatched ? "\nFault lock: ON, AC pumps blocked" : "\nFault lock: none";
    r += "\nSwitch resets: " + String(bswStrikes) + " (locks at " + String(BSW_LATCH_STRIKES) + ")";
    r += String("\nRunning on: ") + (esp2OnSecondary ? "backup battery" : "main battery");
    if (bswLatched) r += "\nText BATTSW,CLEAR to unlock";
    sendSMS(r);
    return;
  }
  // SIMTEST -- the weekly SIM check, on demand.  BAL -- carrier balance via USSD.
  // HELP [,RUN | ,CROPS | ,SYSTEM] -- the command list, one SMS per topic (2026-11-16). Owner-gated like
  // everything above. Anything after HELP that is not a topic gets the main page.
  if (U == "HELP" || U == "?" || U == "COMMANDS" || U.startsWith("HELP,") || U.startsWith("HELP ")) {
    String t = U.startsWith("HELP") ? U.substring(4) : String();
    t.replace(",", " "); t.trim();
    if (t == "RUN")
      sendSMS("HELP RUN\n"
              "CANCEL,30 / CANCEL,10 = end run, skip col 30/10 min\n"
              "CANCEL,OFF = end run, col off\n"
              "On hold: NORMAL, IRRIGATE, RELEASE, STOP\n"
              "TANKCHECK,OK = tank checked");
    else if (t == "CROPS" || t == "CROP")
      sendSMS("HELP CROPS\n"
              "SET,COL_A,N,150,P,40,K,200,pH,5.8\n"
              "SET,COL_A,PRESET,PECHAY\n"
              "NAME,COL_A,Lettuce\n"
              "MODE,COL_A,AUTO or IRRIGATION_ONLY\n"
              "THRESH,35,30 = start %, gap mg/kg");
    else if (t == "SYSTEM")
      sendSMS("HELP SYSTEM\n"
              "NET = WiFi status\n"
              "WIFI,<name>,<pass>\n"
              "WIFIPORTAL = setup page\n"
              "BATTSW = battery switch\n"
              "BAL = SIM balance\n"
              "ALERTNUM,<phone> = 2nd phone\n"
              "FBASE,STATUS");
    else
      sendSMS("HELP: text one command\n"
              "STATUS = report now\n"
              "SUMMARY = day averages\n"
              "FULL SUMMARY = hourly\n"
              "STOP = emergency stop\n"
              "More: HELP,RUN  HELP,CROPS  HELP,SYSTEM");
    return;
  }
  if (U == "SIMTEST") { simHealthCheck(); return; }
  if (U == "BAL") {
    if (!USSD_CODE.length()) { smsErr("BAL|NOCODE", "NOT DONE: No balance code saved.\nSend USSD,<code> first, e.g. USSD,*143#"); return; }
    if (!gsm.requestUssd(USSD_CODE.c_str())) smsErr("BAL|BUSY", "NOT DONE: The modem is busy. Try BAL again in a minute.");
    return;                                 // the answer arrives through gsmOnUssd, to the owner
  }
  // USSD,<code> | USSD,CLEAR -- carrier balance code for BAL and the weekly check. USSD = show it.
  if (U == "USSD" || U.startsWith("USSD,")) {
    if (U != "USSD") {
      String code = b.substring(5); code.trim();
      if (code.equalsIgnoreCase("CLEAR")) code = "";
      else if (!gsmUssdCodeOk(code.c_str())) { smsErr("USSD|FORMAT", "NOT DONE: Use digits, * and # only, e.g. USSD,*143#\nUSSD,CLEAR removes it."); return; }
      USSD_CODE = code;
      prefs.putString("ussd", USSD_CODE);
    }
    sendSMS(USSD_CODE.length() ? "Balance code: " + USSD_CODE + "\nText BAL to check the SIM balance."
                               : String("Balance code: none\nSet one with USSD,<code>"));
    return;
  }
  // ALERTNUM,<number> | ALERTNUM,CLEAR -- second recipient for ALERT texts. It receives only: its
  // commands are still refused, because owner-gating looks at PHONE_NUMBER alone. ALERTNUM = show it.
  if (U == "ALERTNUM" || U.startsWith("ALERTNUM,")) {
    if (U != "ALERTNUM") {
      String num = b.substring(9); num.trim();
      if (num.equalsIgnoreCase("CLEAR")) num = "";
      else if (!gsmNumberOk(num.c_str())) { smsErr("ALERTNUM|FORMAT", "NOT DONE: Use ALERTNUM,<phone number> (digits, + allowed)\nor ALERTNUM,CLEAR"); return; }
      ALERT_NUMBER2 = num;
      prefs.putString("owner2", ALERT_NUMBER2);
    }
    sendSMS(ALERT_NUMBER2.length() ? "2nd alert number: " + ALERT_NUMBER2 + "\nIt gets alerts only and cannot send commands."
                                   : String("2nd alert number: none\nAdd one with ALERTNUM,<phone number>"));
    return;
  }
  // NET -- WiFi/ThingSpeak link status (Part B)
  if (U == "NET") {
    String ss; xSemaphoreTake(netMux, portMAX_DELAY); ss = wifiSsid; xSemaphoreGive(netMux);
    String r = "NETWORK\nWiFi: ";
    if (portalActive)       r += String("setup portal ON\nJoin ") + AP_SSID + ", open http://" + portalApIp;
    else if (!wifiEnabled)  r += "turned OFF (text WIFI,ON)";
    else if (wifiConnected) r += "connected to " + ss + "\nSignal " + String(wifiRssiVal) + " dBm, IP " + String(wifiIpStr);
    else                    r += "NOT connected" + (ss.length() ? " (network " + ss + ")" : String(" (no network set)"));
    if (lastTsUploadMs) {
      unsigned long ago = (millis() - lastTsUploadMs) / 1000;
      r += String("\nThingSpeak: last upload ") + (lastTsOk ? "OK, " : "FAILED, ") +
           (ago < 120 ? String(ago) + " s ago" : String(ago / 60) + " min ago");
    } else {
      r += "\nThingSpeak: no upload yet";
    }
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
      if (target == -2) { smsErr("SUMDATE", "NOT DONE: Unknown day.\nUse SUMMARY, SUMMARY,YESTERDAY or SUMMARY,YYYYMMDD (same for FULL SUMMARY)"); return; }
      SumMode mode = isFull ? SUM_FULL : SUM_SHORT;
      if (sysState == ACTIVE_STATE || portalActive) {   // portal owns the SD -> defer, run when it closes
        if (summaryPending) {
          // A request is already queued and waiting for the system to idle. summaryReplyTo/
          // pendingSumMode/pendingSumTarget are single, non-queued globals -- a second request here
          // used to silently overwrite the first one, so the first requester's report simply never
          // arrived, with no error, and both requests got an ACK,BUSY that looked like a queued
          // success.
          sendSMS("BUSY: A summary is already waiting.\nIt will be sent when the system is free.");
          return;
        }
        sendSMS(String("BUSY: The system is running.\nYour ") + (isFull ? "full summary" : "summary") + " will be sent when it is free.");
        summaryReplyTo = replyTarget;
        pendingSumMode = mode; pendingSumTarget = target;
        summaryPending = true;
      } else if (summaryStage != SUM_IDLE) {
        sendSMS("BUSY: A summary is being sent right now.\nTry again in a few minutes.");   // a job is already running -> don't stack
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
  String k0 = tok[0]; k0.toUpperCase();
  if (n < 2 && k0 != "WIFIPORTAL") { smsErr("PARSE", "NOT DONE: Command not understood.\nText HELP for the list of commands."); return; }   // the only keyword valid on its own here

  // resolve column index from COL_A/B/C
  auto colIdx = [&](const String &s) -> int {
    String t = s; t.toUpperCase();
    if (t == "COL_A") return 0;
    if (t == "COL_B") return 1;
    if (t == "COL_C") return 2;
    return -1;
  };

  if (k0 == "MODE") {
    int c = colIdx(tok[1]); String m = tok[2]; m.toUpperCase();
    if (c < 0) { smsErr("COL", "NOT DONE: Unknown column. Use COL_A, COL_B or COL_C."); return; }
    if (m == "AUTO")            col[c].mode = MODE_AUTO;
    else if (m == "IRRIGATION_ONLY") col[c].mode = MODE_IRRIGATION_ONLY;
    else { smsErr("MODE", "NOT DONE: Mode must be AUTO or IRRIGATION_ONLY.\nExample: MODE,COL_A,AUTO"); return; }
    saveColumn(c);
    sendSMS(String("OK: Col ") + COL_TAG[c] + " mode is " +
            (m == "AUTO" ? "AUTO (nutrients when needed)" : "IRRIGATION ONLY (water only)"));
    return;
  }
  if (k0 == "NAME") {
    int c = colIdx(tok[1]);
    if (c < 0 || n < 3) { smsErr("NAME", "NOT DONE: Use NAME,COL_A,<plant name>"); return; }
    strncpy(col[c].name, tok[2].c_str(), sizeof(col[c].name) - 1);
    col[c].name[sizeof(col[c].name) - 1] = 0;
    saveColumn(c);
    sendSMS(String("OK: Col ") + COL_TAG[c] + " plant name is now " + col[c].name);
    return;
  }
  if (k0 == "SET") {
    int c = colIdx(tok[1]);
    if (c < 0) { smsErr("COL", "NOT DONE: Unknown column. Use COL_A, COL_B or COL_C."); return; }
    if (n < 4) {                                         // or the reply would say OK with nothing changed
      smsErr("SET|FORMAT", "NOT DONE: Use SET,COL_A,N,150,P,40,K,200,pH,5.8\nor SET,COL_A,PRESET,<crop>. Text HELP for more."); return;
    }
    String t2 = tok[2]; t2.toUpperCase();
    if (t2 == "PRESET") {                   // SET,COL_A,PRESET,NAME
      String pn = tok[3]; pn.toUpperCase();
      for (int p = 0; p < NUM_PRESETS; p++) {
        if (pn == CROP_PRESETS[p].name) {
          col[c].targetN = CROP_PRESETS[p].N; col[c].targetP = CROP_PRESETS[p].P;
          col[c].targetK = CROP_PRESETS[p].K; col[c].targetPH = CROP_PRESETS[p].pH;
          saveColumn(c);
          sendSMS(String("OK: ") + smsCol(c) + " uses preset " + CROP_PRESETS[p].name + "\nN" + String((int)col[c].targetN) +
                  " P" + String((int)col[c].targetP) + " K" + String((int)col[c].targetK) + " mg/kg, pH " + String(col[c].targetPH, 1));
          return;
        }
      }
      String names;
      for (int p = 0; p < NUM_PRESETS; p++) names += String(p ? ", " : "") + CROP_PRESETS[p].name;
      smsErr("PRESET", "NOT DONE: Unknown crop preset.\nChoose one of: " + names); return;
    }
    // explicit: SET,COL_A,N,150,P,40,K,200,pH,5.8  (key/value pairs)
    // Shares applyColumnTarget()/columnTargetInRange() with the dashboard's SET_COLUMN, now via the
    // same two-pass validate-then-apply shape too (Pass 1: every field checked, nothing mutated;
    // Pass 2: apply, only reached if all passed). Previously this validated-and-mutated one field at
    // a time via applyColumnTarget() alone, so a late-failing field (e.g. N valid, P out of range)
    // left N already live in RAM from the earlier successful iteration -- unpersisted (saveColumn()
    // never reached on the error return) and silently reverting on next reboot, while the ERR reply
    // implied nothing had changed at all.
    String err;
    for (int i = 2; i + 1 < n; i += 2) {
      if (!columnTargetInRange(tok[i], tok[i + 1].toFloat(), err)) {
        smsErr((String("SET|") + err).c_str(), "NOT DONE: " + err + ".\nNothing was changed."); return;
      }
    }
    for (int i = 2; i + 1 < n; i += 2) {
      applyColumnTarget(c, tok[i], tok[i + 1].toFloat(), err);   // already validated -- cannot fail here
    }
    saveColumn(c);
    sendSMS(String("OK: ") + smsCol(c) + " targets saved\nN" + String((int)col[c].targetN) + " P" + String((int)col[c].targetP) +
            " K" + String((int)col[c].targetK) + " mg/kg, pH " + String(col[c].targetPH, 1));
    return;
  }
  // WIFI,<ssid>,<pass>  -- set WiFi credentials (owner-only). Password is the remainder
  // after the 2nd comma, so it may contain commas/spaces (SSID may not). (Part B)
  if (k0 == "WIFI") {
    if (!senderIsOwner()) { smsErr("AUTH", "Sorry, this number is not allowed to control the irrigation system."); return; }
    // WIFI,ON / WIFI,OFF -- master switch (mirrors the Settings > WiFi toggle). Checked before the
    // ssid/pass parse; a real SSID of literally "ON"/"OFF" with no password is not a valid pair anyway.
    // Audit fix (2026-09-08): compare against an uppercased copy, not the raw token -- tok[] is built
    // from the un-uppercased body, unlike k0, so "WIFI,on"/"WIFI,off" previously fell through to the
    // ssid/pass parser and silently returned ERR,WIFI instead of toggling.
    if (n == 2) {
      String t1u = tok[1]; t1u.toUpperCase();
      if (t1u == "ON" || t1u == "OFF") {
        wifiEnabled = (t1u == "ON"); saveWifiEn();
        sendSMS(t1u == "ON" ? "OK: WiFi is ON.\nText NET in a minute to check the connection." : "OK: WiFi is OFF.");
        return;
      }
    }
    int p1 = b.indexOf(','), p2 = (p1 >= 0) ? b.indexOf(',', p1 + 1) : -1;
    if (p1 < 0 || p2 < 0) { smsErr("WIFI", "NOT DONE: Use WIFI,<network name>,<password>\nor WIFI,ON / WIFI,OFF"); return; }
    String ss = b.substring(p1 + 1, p2), pw = b.substring(p2 + 1);
    xSemaphoreTake(netMux, portMAX_DELAY); wifiSsid = ss; wifiPass = pw; xSemaphoreGive(netMux);
    saveWifi(); wifiCredsChanged = true;
    sendSMS("OK: WiFi set to " + ss + ". Connecting now.\nText NET in a minute to check.");
    return;
  }
  // WIFIPORTAL [,STOP] -- start/stop the SoftAP WiFi-setup portal (owner-only). Join AP_SSID from a
  // phone, browse to the AP IP, pick a network + type the password, Save. (Part B)
  if (k0 == "WIFIPORTAL") {
    if (!senderIsOwner()) { smsErr("AUTH", "Sorry, this number is not allowed to control the irrigation system."); return; }
    // Audit fix (2026-09-09): same case-sensitivity bug class as the WIFI,ON/OFF fix -- tok[] is
    // built from the un-uppercased body, so "WIFIPORTAL,stop" previously fell through to the
    // (re-)start branch below instead of stopping the portal, replying with the AP's default
    // password as if a stop had never been requested.
    if (n >= 2) { String t1u = tok[1]; t1u.toUpperCase(); if (t1u == "STOP") { portalCancel = true; sendSMS("OK: WiFi setup portal closed."); return; } }
    portalRequested = true;
    sendSMS(String("OK: WiFi setup portal is ON (closes after 10 min unused).\nOn your phone join WiFi ") + AP_SSID +
            ", password " + AP_PASS + ", then open http://192.168.4.1");
    return;
  }
  // TSKEY,<1|2>,<writeApiKey>  -- set a ThingSpeak channel write key (owner-only). (Part B)
  if (k0 == "TSKEY") {
    if (!senderIsOwner()) { smsErr("AUTH", "Sorry, this number is not allowed to control the irrigation system."); return; }
    if (n < 3) { smsErr("TSKEY", "NOT DONE: Use TSKEY,<channel 1-3>,<write key>"); return; }
    int ch = tok[1].toInt(); String key = tok[2];
    if (ch < 1 || ch > 3) { smsErr("TSCH", "NOT DONE: The ThingSpeak channel must be 1, 2 or 3."); return; }
    // Same alnum-only check the portal's /tskey route already enforces -- this SMS path had none, so
    // a key containing '&' (e.g. "ABC&status=x") would inject extra query parameters into the
    // outbound ThingSpeak GET (tsUpload() concatenates it unescaped into the URL).
    bool badKey = (key.length() == 0);
    for (unsigned ki = 0; ki < key.length(); ki++) if (!isalnum((unsigned char)key[ki])) badKey = true;
    if (badKey) { smsErr("TSKEY_CHARS", "NOT DONE: The key must be letters and numbers only."); return; }
    xSemaphoreTake(netMux, portMAX_DELAY);
    if (ch == 1) tsKey1 = key; else if (ch == 2) tsKey2 = key; else tsKey3 = key;
    xSemaphoreGive(netMux);
    saveTsKey();
    sendSMS("OK: ThingSpeak key for channel " + String(ch) + " saved.");
    return;
  }
  // THRESH,<start%>,<gap>  -- soil start threshold + fertigation gap (also on the LCD Settings >
  // Thresholds editor). The old THRESH,<start%>,<stop%>,<gap> is still accepted, and its stop value
  // ignored, so a saved phone message keeps working. (Owner-gated above.)
  if (k0 == "THRESH") {
    if (n < 3) { smsErr("THRESH", "NOT DONE: Use THRESH,<start %>,<gap mg/kg>\nExample: THRESH,35,30"); return; }
    int s = clampi(tok[1].toInt(), 0, 100);
    int g = clampi((int)tok[n >= 4 ? 3 : 2].toFloat(), 0, 500);
    soilStartPct = s; fertGap = g;
    saveThresholds();
    sendSMS("OK: Watering starts below " + String(s) + "% soil moisture.\nNutrients are added when " + String(g) +
            " mg/kg or more below target.");
    return;
  }
  smsErr("CMD", "NOT DONE: Unknown command \"" + smsCut(tok[0], 20) + "\".\nText HELP for the list of commands.");
}

/* =============================================================================
 *  SMS TEXT  (2026-11-16, owner's request: every text easy to read, one SMS = 160 characters)
 *
 *  Every outbound text is written for a person, not a parser, and is laid out the same way:
 *    line 1   KIND: headline        CRITICAL: / ALERT: / WARNING: / NOTICE: (faults)
 *                                   OK: / NOT DONE: / BUSY: / INFO: / RUN: (replies + notices)
 *                                   or a report title (DAILY REPORT, SUMMARY, HELP ...)
 *    middle   the detail, in words  "Col A irrigation, transfer, pulses=0"
 *    then     what to do next       "Refill the reservoir" / "Reply NORMAL, ..."
 *    last     (hh:mm CODE)          alerts only: the code the LCD, the website and the SD log use
 *  Only GSM-7 characters are used, so one SMS really holds 160: no emoji or degree sign (they switch
 *  the whole text to UCS-2 = 70 characters), and '|' '[' ']' '{' '}' '~' '^' '\' are avoided (2 each).
 *  A text that would run over is trimmed in its detail line ("..") rather than split. Reports and
 *  HELP are written as lines and sent as complete pages, each headed "TITLE i/N".
 *  Inbound command syntax is unchanged (the portal replays it through handleSms too).
 * ========================================================================== */
// Technical detail -> words: "COL_A|NO_READING|raw=12,0" -> "Col A, no reading, raw=12,0".
// All-capital words of 2+ letters are lower-cased, except the abbreviations in KEEP; anything with a
// digit (ESP2, INA226) is kept as it is.
static String smsPlain(const String &raw) {
  String s = raw;
  s.trim();
  s.replace("MIXER_OC", "MIXER_OVERCURRENT");
  s.replace("COL_", "Col ");
  s.replace("|", ", ");
  s.replace("_", " ");
  static const char *const KEEP[] = { "RTC", "LCD", "NPK", "PZEM", "PCF", "GSM", "SMS", "USB", "UART", "ESP", "LED",
                                      "AC", "DC", "EC", "SD", "OK", "IP", "ID", "UID" };
  int n = s.length();
  for (int i = 0; i < n; ) {
    if (!isalnum((unsigned char)s[i])) { i++; continue; }
    int j = i; bool caps = true;
    while (j < n && isalnum((unsigned char)s[j])) { if (!isupper((unsigned char)s[j])) caps = false; j++; }
    String w = s.substring(i, j);
    bool keep = false;
    for (size_t k = 0; k < sizeof(KEEP) / sizeof(KEEP[0]); k++) if (w == KEEP[k]) keep = true;
    if (w == "PH") { s[i + 1] = 'H'; s[i] = 'p'; }                // pH
    else if (caps && !keep && j - i >= 2) for (int k = i; k < j; k++) s[k] = (char)tolower((unsigned char)s[k]);
    i = j;
  }
  return s;
}

// "14:05" from the RTC, or "" while the clock is not trusted.
static String smsClock() {
  if (!rtcOk) return String();
  DateTime n = rtcNow();
  char b[6]; snprintf(b, sizeof(b), "%02d:%02d", n.hour(), n.minute());
  return String(b);
}

// 20261116 -> "2026-11-16" (0 -> "no-date file").
static String smsDate(long stamp) {
  if (stamp <= 0) return String("no-date file");
  char b[12]; snprintf(b, sizeof(b), "%04ld-%02ld-%02ld", stamp / 10000, (stamp / 100) % 100, stamp % 100);
  return String(b);
}

static String smsCol(int c) {                                   // "Col A (Lettuce)" / "Col A"
  if (c < 0 || c >= NUM_COLUMNS) return String("Col ?");
  String s = String("Col ") + COL_TAG[c];
  if (strlen(col[c].name)) s += String(" (") + col[c].name + ")";
  return s;
}

// Trim s to maxSeptets, ending in ".." when anything was cut.
static String smsCut(const String &s, int maxSeptets) {
  if (gsmSeptets(s.c_str()) <= maxSeptets) return s;
  if (maxSeptets < 4) return String();
  String o; int used = 0;
  for (unsigned i = 0; i < s.length(); i++) {
    int w = gsmIsExtended(s[i]) ? 2 : 1;
    if (used + w > maxSeptets - 2) break;
    o += s[i]; used += w;
  }
  o.trim();
  while (o.length() && (o[o.length() - 1] == ',' || o[o.length() - 1] == '.')) o.remove(o.length() - 1);
  return o + "..";
}

// head / detail / todo / foot, one per line (empty parts skipped). If it runs past one SMS the detail
// line is trimmed first, then the todo line: the headline and the reference always survive.
static String smsCompose(const String &head, String detail, String todo, const String &foot) {
  auto total = [&]() -> int {
    const String *p[4] = { &head, &detail, &todo, &foot };
    int len = 0, parts = 0;
    for (int i = 0; i < 4; i++) if (p[i]->length()) { len += gsmSeptets(p[i]->c_str()); parts++; }
    return len + (parts ? parts - 1 : 0);
  };
  int over = total() - SMS_MAX;
  if (over > 0 && detail.length()) { detail = smsCut(detail, gsmSeptets(detail.c_str()) - over); over = total() - SMS_MAX; }
  if (over > 0 && todo.length())   { todo   = smsCut(todo,   gsmSeptets(todo.c_str())   - over); }
  String m = head;
  if (detail.length()) m += "\n" + detail;
  if (todo.length())   m += "\n" + todo;
  if (foot.length())   m += "\n" + foot;
  return m;
}

/* Fault code -> headline + what to do. Codes not listed still go out, with the code itself spelled
 * out as the headline. HIDE_LOC: the location is only an internal tag (ESP2, LINK, BATT ...) or is
 * already said better by the todo line, so it is not printed. */
const uint8_t SMS_HIDE_LOC = 1;
struct SmsFaultInfo { const char *code; const char *what; const char *todo; uint8_t flags; };
static const SmsFaultInfo SMS_FAULTS[] = {
  // ---- held faults: the run waits for a reply (a second text lists the replies) ----
  // web v.4: water is metered by the tank ultrasonics -- FLOW_FAIL now means "the level did not move".
  { "FLOW_FAIL",          "Tank level not moving, pump stopped", "Check pump, valves, tank sensors",        0 },
  { "ESP2_ERROR",         "Pump controller stopped the run",    "Check the pump controller board",          0 },
  { "LEVEL_BLIND",        "Tank level sensor not reading",      "Check the mixing tank ultrasonic",         0 },
  { "SERVO_FAIL",         "Pump selector failed or stuck",      "Check the servo and its switch",           0 },
  { "BATT_SWITCH_FAIL",   "Switch to backup battery failed",    "Check the backup battery and relay",       0 },
  { "INVERTER_FAIL",      "Inverter gave no AC power",          "Check the inverter and its wiring",        0 },
  { "PWR_FAIL",           "Pump power fault, pump stopped",     "Check pump, wiring and AC supply",         0 },
  { "SAFE_STOP",          "Safety stop",                        "Check the mixer and pumps",                0 },
  { "DOSE_TIMEOUT",       "Nutrient pump ran too long",         "Prime the nutrient line",                  0 },
  // ---- critical: everything stopped, cleared on the LCD ----
  { "MIX_OVERFLOW",       "Mixing tank overfilling",            "Pumps stopped. Check tank, clear on LCD",  0 },
  { "ESP2_DONE_TIMEOUT",  "Pump controller never finished",     "All stopped. Check tank, clear on LCD",    SMS_HIDE_LOC },
  { "BATTERY_CRITICAL",   "Main battery critically low",        "All stopped. Charge it, clear on LCD",     SMS_HIDE_LOC },
  { "ESTOP_KEPT",         "Emergency stop kept after restart",  "Clear it on the LCD to resume",            SMS_HIDE_LOC },
  // ---- controllers and links ----
  { "ESP2_RESTARTED",     "Pump controller restarted",          "",                                         0 },
  { "ESP2_SILENCE",       "Pump controller not answering",      "Restarting it automatically",              SMS_HIDE_LOC },
  { "ESP2_COMM_LOST",     "Lost link to pump controller",       "Retrying every 20 min. Check wiring",      SMS_HIDE_LOC },
  { "ESP2_NO_READY",      "Pump controller did not start",      "Check its power and wiring",               0 },
  { "ESP2_DEGRADED",      "Pump controller disabled a part",    "Run continues. Check it after",            0 },
  { "PCF_FAIL",           "Relay board not responding",         "Check the relay board wiring",             0 },
  { "COL_MISMATCH",       "Column is off on pump controller",   "Skipped today. Match column settings",     0 },
  { "NANO_GARBAGE",       "Sensor board sending bad data",      "Check the sensor board wiring",            0 },
  { "NANO_RESET",         "Sensor board was restarted",         "",                                         0 },
  { "NANO_SILENCE",       "Sensor board not answering",         "Check its power and wiring",               SMS_HIDE_LOC },
  { "SELF_RESET",         "Main controller restarting itself",  "Max once a day. Check if it repeats",      0 },
  { "NET_STALL",          "Internet task stuck",                "Website paused. Watering not affected",    SMS_HIDE_LOC },
  { "RTC_MISSING",        "Clock module not found",             "",                                         0 },
  { "RTC_INVALID",        "Clock time is wrong",                "",                                         0 },
  // ---- water, tanks, dosing ----
  { "SERVO_MISMATCH",     "Wrong pump ran, retrying",           "Check the pump selector servo",            0 },
  { "LEVEL_HIGH",         "Mixing tank full before target",     "Run goes on. Check tank litres cal",       0 },
  { "RES_EMPTY",          "Water reservoir ran dry",            "Refill it. Run went on with less water",   0 },
  { "RES_LOW",            "Water reservoir is low",             "Refill the reservoir",                     SMS_HIDE_LOC },
  { "TANK_CHECK",         "Check the mixing tank",              "Runs wait. Then text TANKCHECK,OK",        0 },
  { "EC_FAIL",            "Mixed batch EC out of range",        "Batch still given. Check dosing",          0 },
  { "PH_FAIL",            "Mixed batch pH out of range",        "Batch still given. Check pH probe",        0 },
  { "DOSE_BATCH_LOW",     "Batch too small to dose",            "Watering only. Check volume settings",     0 },
  { "DOSE_CARRY",         "Dosed batch went to other column",   "",                                         0 },
  { "THRESH_NO_FERT",     "Soil start % too low to fertilize",  "Only watering will run. Raise it",         0 },
  { "RESUME_NO_REPLY",    "No answer to your resume",           "Still on hold. Check link, try again",     SMS_HIDE_LOC },
  { "WO_OVERSIZE",        "Run command too long, not sent",     "Run skipped. Check column settings",       0 },
  { "FORCE_ABORT",        "Manual run cancelled",               "",                                         0 },
  { "MANUAL_REVOKED",     "Website control ended",              "The operator at the rig took over",        SMS_HIDE_LOC },
  // ---- sensors ----
  { "SENSOR_FAIL",        "Sensor not working",                 "Check its wiring",                         0 },
  { "SOIL_MISSING",       "Soil moisture reading missing",      "Check that column's soil probes",          0 },
  { "NPK_FAULT",          "NPK sensor not reading",             "Watering only. Check NPK sensor",          0 },
  // ---- batteries ----
  { "BATTERY_LOW",        "Main battery low",                   "Check charging and the solar panel",       SMS_HIDE_LOC },
  { "BATT2_LOW",          "Backup battery low",                 "AC pumps blocked until it charges",        0 },
  { "BATT2_CRIT",         "Backup battery critically low",      "Charge the backup battery",                0 },
  { "BATT_SWITCH_RESET",  "Battery switch reset the boards",    "",                                         0 },
  { "BATT_SWITCH_FAULT",  "Battery switching locked",           "AC pumps blocked. Clear on LCD, web or text BATTSW,CLEAR (auto in 24h)", SMS_HIDE_LOC },
  { "MAIN_BATT_CRIT",     "Main battery critically low",        "",                                         0 },
  { "BACKUP_EXHAUSTED",   "Backup battery used up",             "",                                         0 },
  { "BACKUP_SWITCH_FAILED", "Switch to backup failed",          "",                                         0 },
};
static const SmsFaultInfo *smsFaultInfo(const char *code) {
  for (size_t i = 0; i < sizeof(SMS_FAULTS) / sizeof(SMS_FAULTS[0]); i++)
    if (!strcmp(SMS_FAULTS[i].code, code)) return &SMS_FAULTS[i];
  return nullptr;
}
static const char *smsTierWord(char tier) {
  return tier == 'C' ? "CRITICAL" : tier == 'M' ? "ALERT" : tier == 'W' ? "WARNING" : "NOTICE";
}
static String smsFaultWhat(const char *code) {
  const SmsFaultInfo *fi = smsFaultInfo(code);
  return fi ? String(fi->what) : smsPlain(code);
}
// The alert text for one fault. detail is already in words; "" = none.
static String smsFaultText(char tier, const char *code, const String &detail) {
  const SmsFaultInfo *fi = smsFaultInfo(code);
  String what = fi ? String(fi->what) : smsPlain(code);
  String todo = fi ? String(fi->todo) : String();
  String t = smsClock();
  String foot = "(" + (t.length() ? t + " " : String()) + code + ")";
  return smsCompose(String(smsTierWord(tier)) + ": " + what, detail, todo, foot);
}
void smsFault(char tier, const char *code, const String &rawLoc) {
  const SmsFaultInfo *fi = smsFaultInfo(code);
  bool hide = fi && (fi->flags & SMS_HIDE_LOC);
  String d = hide ? String() : smsPlain(rawLoc);
  if (d.length() && d[0] >= 'a' && d[0] <= 'z') d[0] = (char)(d[0] - 'a' + 'A');   // its own line: capitalise
  smsAlert(smsFaultText(tier, code, d));
}

// The reply menu sent (to the owner) right after a held-fault alert.
static String smsHoldMenu(int colIdx, bool steerRelease) {
  // A pump-selector hold (2026-11-16, user): ESP2 already re-switched it on its own and it is still wrong. The choice
  // is to switch it again (the step restarts with the 3-swing pre-seat) or not.
  if (lastHoldCode == "SERVO_FAIL")
    return String("ON HOLD") + (colIdx >= 0 && colIdx < NUM_COLUMNS ? String(" (Col ") + COL_TAG[colIdx] + ")" : String()) +
           ". Pump selector still wrong. Reply:\n"
           "SERVO = switch it again and go on\n"
           "CANCEL,30 = do not, end run (skip 30m)\n"
           "STOP = stay on hold";
  String m = String("ON HOLD") + (colIdx >= 0 && colIdx < NUM_COLUMNS ? String(" (Col ") + COL_TAG[colIdx] + ")" : String()) +
             ". Reply:\n"
             "NORMAL = resume run\n"
             "IRRIGATE = water only, no dose\n";
  m += steerRelease ? "RELEASE = deliver tank as is (best)\n" : "RELEASE = deliver tank as is\n";
  m += "STOP = stay on hold\n"
       "CANCEL,30 = end run, skip 30m";
  return m;
}

// Send to the sender when one is set (a reply), else the owner; alerts also to the second number.
static void smsDispatch(const String &msg, uint8_t retries, bool alsoAlertNumber) {
  if (smsMute) return;                                 // suppress replies for portal-replayed config commands
  if (replyTarget.length() || PHONE_NUMBER.length())    // no owner number yet: nowhere to send it
    smsEnqueueSplit(msg, replyTarget.length() ? replyTarget : PHONE_NUMBER, retries);
  if (alsoAlertNumber && !replyTarget.length() && ALERT_NUMBER2.length())
    smsEnqueueSplit(msg, ALERT_NUMBER2, retries);
}

// Enqueue only -- non-blocking. The AT exchange is driven by gsmTxTick() so alert
// and fault paths never stall the loop.
void sendSMS(const String &msg) { smsDispatch(msg, GSM_SEND_RETRIES, false); }
void smsAlert(const String &msg) { smsDispatch(msg, GSM_SEND_RETRIES, true); }

// A refused command. The log keeps the short machine code (SMS_ERR|<logCode>, the same codes the
// old "ERR,<code>" replies carried) so the dashboard and SD log read exactly as before; the person
// gets the explanation. AUTH goes to strangers, so it gets no retries: a dead-end number must not
// hold up real alerts behind it.
void smsErr(const char *logCode, const String &msg) {
  if (smsMute) return;
  logEvent("GSM", "SMS_ERR", logCode);
  smsDispatch(msg, strcmp(logCode, "AUTH") ? GSM_SEND_RETRIES : 0, false);
}

// ---- multi-page texts: each page is a whole SMS that starts with "TITLE i/N" ----
static int smsSkipBreak(const String &s, int pos) {
  while (pos < (int)s.length() && (s[pos] == '\n' || s[pos] == ' ')) pos++;
  return pos;
}
// End (exclusive) of the page starting at from that fits room septets. A blank line marks a group
// (one column of the daily report, one section of FULL SUMMARY): a page ends at one when it is in the
// page's second half, so a group is not split; else at a line break; else at a space/comma.
static int smsPageEnd(const String &s, int from, int room) {
  const char *p = s.c_str(); int len = s.length();
  int used = 0, i = from;
  while (i < len) { int w = gsmIsExtended(p[i]) ? 2 : 1; if (used + w > room) break; used += w; i++; }
  if (i >= len) return len;
  int half = from + (i - from) / 2;
  for (int j = i; j > half; j--) if (p[j] == '\n' && p[j - 1] == '\n') return j - 1;
  for (int j = i; j > from; j--) if (p[j] == '\n') return j;
  return gsmSplitEnd(p, from, len, room);
}
static int smsPageRoom(const String &title) {             // worst case header "TITLE 99/99\n"
  return SMS_MAX - gsmSeptets(title.c_str()) - 7;
}
static int smsPageCount(const String &body, int room) {
  int n = 0, pos = smsSkipBreak(body, 0), len = body.length();
  while (pos < len) { pos = smsSkipBreak(body, smsPageEnd(body, pos, room)); n++; }
  return n ? n : 1;
}
void smsSendPages(const String &title, const String &body) {
  String one = title + "\n" + body;
  if (gsmSeptets(one.c_str()) <= SMS_MAX) { sendSMS(one); return; }
  int room = smsPageRoom(title);
  int total = smsPageCount(body, room);
  int pos = smsSkipBreak(body, 0);
  for (int i = 1; i <= total && pos < (int)body.length(); i++) {
    int end = smsPageEnd(body, pos, room);
    sendSMS(title + " " + i + "/" + total + "\n" + body.substring(pos, end));
    pos = smsSkipBreak(body, end);
  }
}

static const char *smsStateWords(SystemState s, bool lockedOut) {
  if (lockedOut && s == IDLE_STATE) return "safe mode, pumps locked out";
  switch (s) {
    case BOOT_STATE:     return "starting";
    case STARTUP_SYNC:   return "starting up";
    case IDLE_STATE:     return "idle, ready";
    case ACTIVE_STATE:   return "running";
    case RECOVERY_STATE: return "recovering pump controller";
    case SAFE_MODE:      return "safe mode";
    case TEST_MODE:      return "testing";
    default:             return "EMERGENCY STOP";
  }
}

// Safety net: one SMS holds 160 septets. A text that still runs over (a long carrier USSD answer) goes
// out as "(i/N)" parts, so it arrives whole instead of cut off or refused by the modem.
void smsEnqueueSplit(const String &msg, const String &to, uint8_t retries) {
  const char *s = msg.c_str();
  int len = msg.length();
  if (gsmSeptets(s) <= 160) { gsm.enqueue(s, to.c_str(), retries); return; }
  const int SEG = 153;                                  // leaves room for a "(10/12)" prefix
  int total = 0;
  for (int pos = 0; pos < len; total++) pos = gsmSplitEnd(s, pos, len, SEG);
  int pos = 0;
  for (int i = 1; i <= total; i++) {
    int end = gsmSplitEnd(s, pos, len, SEG);
    String part = "(" + String(i) + "/" + String(total) + ")" + msg.substring(pos, end);
    gsm.enqueue(part.c_str(), to.c_str(), retries);
    pos = end;
  }
}

// Run the driver and mirror its status into the globals the LCD, logs and dashboard read.
void gsmTxTick() {
  gsm.tick(millis(), battQuietActive());                // quiet: no 2 A burst during a battery switch
  lastRssi        = gsm.rssi();
  netRegistered   = gsm.registered();
  lastCreg        = gsm.regStat();
  lastCereg       = gsm.ceregStat();
  simReady        = gsm.simReady();
  lastGsmHealthMs = gsm.lastHealthMs();
}

// Daily / STATUS report, in words (sec.12.1.3). System lines first, then two lines per column; sent as
// "DAILY REPORT <date> i/N" pages that each break at a line.
void sendDailyReport() {
  long ds = (currentDayStamp > 0) ? currentDayStamp : 0;
  String r = String("State: ") + smsStateWords(sysState, actuationsDisabled);
  r += (faultsToday[0] || faultsToday[1] || faultsToday[2])
       ? "\nFaults: " + String(faultsToday[0]) + " critical, " + String(faultsToday[1]) + " major, " + String(faultsToday[2]) + " minor"
       : String("\nFaults: none");
  if (nanoResetsToday) r += "\nSensor board resets: " + String(nanoResetsToday);
  r += "\nBattery " + (battVCalLoaded ? String(battV, 1) + "V" : String("n/a")) +
       ", backup " + ((secInaOk && secHaveV) ? String(secV, 1) + "V" + (secLow ? " LOW" : "") : String("n/a"));
  r += "\nSolar " + (inaUsable(powerIna[INA_SOLAR]) ? String(powerIna[INA_SOLAR].v, 1) + "V " + String(powerIna[INA_SOLAR].i, 1) + "A"
                                                     : String("n/a"));
#if SECONDARY_BACKUP_ENABLED
  if (bkState != BK_IDLE) r += String("\nBackup supply: ") + bkName(bkState);
#endif
  // Daily energy (sec.12.1.3): integrated discharge + charge, Wh.
  r += "\nEnergy: used " + String((int)(energyConsumedWh + 0.5)) + "Wh, charged " + String((int)(energyChargedWh + 0.5)) + "Wh";
  // Two groups (a blank line keeps each on one page): what each column used today, then its targets.
  String used = "\n\nUsed today:", tgt = "\n\nTargets (N P K mg/kg, pH):";
  for (int c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c]) continue;
    used += String("\nCol ") + COL_TAG[c] + ": " + String(waterUsedToday[c], 1) + "L water, " + String(nutrientUsedToday[c], 0) + "mL nutrients";
    tgt  += "\n" + smsCol(c) + ": " + String((int)col[c].targetN) + " " + String((int)col[c].targetP) + " " +
            String((int)col[c].targetK) + ", " + String(col[c].targetPH, 1);
  }
  r += used + tgt;
  smsSendPages("DAILY REPORT " + (ds > 0 ? smsDate(ds) : String("(clock not set)")), r);
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
// Add a run's total to the columns named by "COL_A" / "COL_AB". One tank and one meter feed them all, so
// a shared total is split evenly -- the same estimate the live daily tally makes in handleEsp2Response.
static void sumAddToCols(float *totals, const String &tag, float value) {
  int n = (int)tag.length() - 4;
  if (n <= 0) return;
  for (int i = 0; i < n; i++) {
    int c = colFromTag(String(tag[4 + i]));
    if (c >= 0) totals[c] += value / n;
  }
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
  if (type == "PWR") {                               // BATT|V=..|I=..|P=..|SRC=..|CONS=..Wh|CHG=..Wh
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
    code = "fault " + detail;
  } else if (type == "RESET") {
    sumRst++;
    // Was keyed on f[0] alone (just the device, e.g. "ESP2") -- every distinct reset REASON for the
    // same device (UART_REINIT, SLOW_RETRY, HW|POWERCYCLE, SOFT|RESET_SELF, ...) collapsed into one
    // dedup bucket, so FULL SUMMARY's ERR list showed only the day's FIRST ESP2 reset, silently
    // swallowing every later, possibly-unrelated one. f[1] (the reason) is now part of the key too.
    String f[4]; int nf = splitPipe(detail, f, 4);
    if (nf >= 1) sumAddError(String("RST_") + f[0] + (nf >= 2 ? ("_" + f[1]) : ""), hhmm);
    code = "reset " + detail;
  }
  else if   (type == "DOSE")  {                      // NUT_A|target|measured|COL_A
    // Only ESP2's measured results count. ESP1 logs its plan under the same type (NUT_A|int=..|del=..,
    // SUBRES, BATCH_LOW), and counting those reported doses that were skipped or never pumped.
    String f[4]; int nf = splitPipe(detail, f, 4);
    if (nf < 4 || !f[3].startsWith("COL_")) return;
    sumDose++;
    sumAddToCols(sumNutTot, f[3], f[2].toFloat());
    code = "dose " + detail;
  }
  else if   (type == "ACT") {                        // IRRIGATION|START|COL_A or ...|STOP|COL_A|W=3.2
    if      (detail.startsWith("FERTIGATION") && detail.indexOf("START") >= 0) sumFert++;
    else if (detail.startsWith("IRRIGATION")  && detail.indexOf("START") >= 0) sumIrr++;
    int wi = detail.indexOf("W=");
    // STOP only: it carries the delivered litres. A force run's START row has a W= too, the volume it
    // was asked for, and counting both reported that run's water twice.
    if (wi >= 0 && detail.indexOf("|STOP|") >= 0) {
      String f[6]; int nf = splitPipe(detail, f, 6);
      for (int i = 0; i < nf; i++) if (f[i].startsWith("COL_")) sumAddToCols(sumWaterTot, f[i], detail.substring(wi + 2).toFloat());
    }
    code = detail;
  } else if (type == "STATE" && detail.indexOf("_SET") >= 0) {
    code = "setting " + detail;                      // config change
  } else {
    return;
  }

  // Stored already in words ("06:05 fault: ALERT flow fail, transfer.."), one event per '|'-separated
  // entry; FULL SUMMARY prints one per line.
  code = smsPlain(code);                               // also turns every '|' into ", "
  if (code.length() > 38) code = code.substring(0, 36) + "..";
  String at = (hhmm.length() == 4) ? hhmm.substring(0, 2) + ":" + hhmm.substring(2) : hhmm;
  uint8_t cap = (sumMode == SUM_FULL) ? FULL_MAX_EVENTS : SUMMARY_MAX_EVENTS;
  if (sumEvtCount < cap) { sumEvents += "|" + at + " " + code; sumEvtCount++; }
  else                   { sumTruncated = true; }
}

// Resolve a SUMMARY day token to a yyyymmdd stamp (0 = NODATE file, -2 = invalid).
static long resolveSummaryDay(const String &arg) {
  String a = arg; a.trim(); a.toUpperCase();
  if (a.length() == 0 || a == "TODAY") return (rtcOk && currentDayStamp > 0) ? currentDayStamp : 0;
  if (a == "NODATE")                   return 0;
  if (a == "YESTERDAY") {
    if (!rtcOk) return 0;
    DateTime y = rtcNow() - TimeSpan(1, 0, 0, 0);      // correct month/year rollover
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
  return smsDate(summaryTargetStamp);
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
static String sumTitle() {
  return String(sumMode == SUM_FULL ? "FULL SUMMARY " : "SUMMARY ") + sumDayLabel();
}

static void buildShortReport() {
  String s = "Day averages:";
  for (int c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c]) continue;
    s += String("\nCol ") + COL_TAG[c] + ": ";
    if (sumNPKc[c]) s += "N" + String((int)(sumNsum[c] / sumNPKc[c] + 0.5)) + " P" + String((int)(sumPsum[c] / sumNPKc[c] + 0.5)) + " K" + String((int)(sumKsum[c] / sumNPKc[c] + 0.5)) + " mg/kg";
    else            s += "NPK n/a";
    s += ", moisture " + (sumMoistC[c] ? String((int)(sumMoistSum[c] / sumMoistC[c] + 0.5)) + "%" : String("n/a"));
  }
  if (sumBattC) s += "\nBattery " + String(sumBattVsum / sumBattC, 1) + "V, " + String(sumBattWsum / sumBattC, 1) + "W";
  s += "\nEnergy used " + String((int)(sumConsWh + 0.5)) + "Wh, charged " + String((int)(sumChgWh + 0.5)) + "Wh";
  float wtot = 0, ntot = 0; for (int c = 0; c < NUM_COLUMNS; c++) { wtot += sumWaterTot[c]; ntot += sumNutTot[c]; }
  s += "\nWater " + String(wtot, 1) + "L, nutrients " + String(ntot, 0) + "mL";
  s += "\nRuns: " + String(sumIrr) + " irrigation, " + String(sumFert) + " fertigation, " + String(sumDose) + " doses";
  s += "\nFaults: " + String(sumFltC) + " critical, " + String(sumFltM) + " major, " + String(sumFltm) + " minor";
  s += "\nResets: " + String(sumRst);
  if (!sdOk) s += "\n(No SD card: nothing logged)";
  sumReport = s;
}

// FULL SUMMARY = hourly nutrient/moisture/power record + deduped errors + events + peaks.
static void buildFullReport() {
  String s = "Hourly: N P K mg/kg, M moisture %, battery V W";
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
    if (hrBattC[h]) s += "; " + String(hrBattVsum[h] / hrBattC[h], 1) + "V " + String(hrBattWsum[h] / hrBattC[h], 1) + "W";
  }
  if (sumErrN) {
    s += "\n\nProblems (first seen):";
    for (int i = 0; i < sumErrN; i++) {
      String at = (sumErrTs[i].length() == 4) ? sumErrTs[i].substring(0, 2) + ":" + sumErrTs[i].substring(2) : sumErrTs[i];
      const String &ec = sumErrCode[i];
      s += "\n" + at + " " + (smsFaultInfo(ec.c_str()) ? smsFaultWhat(ec.c_str()) + " (" + ec + ")"
                             : ec.startsWith("RST_") ? "restart: " + smsPlain(ec.substring(4)) : smsPlain(ec));
    }
  } else {
    s += "\n\nProblems: none";
  }
  if (sumEvents.length()) {
    String e = sumEvents; e.replace("|", "\n");
    s += "\n\nEvents:" + e;
    if (sumTruncated) s += "\n(more events on the SD card)";
  }
  float wtot = 0, ntot = 0; for (int c = 0; c < NUM_COLUMNS; c++) { wtot += sumWaterTot[c]; ntot += sumNutTot[c]; }
  s += "\n\nPeaks: max temp " + (sumMaxTemp > -999 ? String(sumMaxTemp, 1) + "C" : String("n/a"))
     + ", min battery " + (sumMinBattV < 99999 ? String(sumMinBattV, 1) + "V" : String("n/a"))
     + ", max power " + String(sumPeakW, 1) + "W";
  s += "\nTotals: water " + String(wtot, 1) + "L, nutrients " + String(ntot, 0) + "mL, faults " +
       String(sumFltC + sumFltM + sumFltm) + ", resets " + String(sumRst);
  if (!sdOk) s += "\n(No SD card: nothing logged)";
  sumReport = s;
}

// Non-blocking FSM: open -> incremental read (bounded/tick) -> build -> stream SMS paced
// to the queue so an uncapped FULL report never overflows the SMS queue or stalls the WDT.
void summaryTick() {
  switch (summaryStage) {
    case SUM_IDLE: return;

    case SUM_OPEN: {
      sumResetAccumulators();
      if (!sdOk) { summaryStage = SUM_BUILD; return; }   // no SD -> build an empty report
      char fname[16];
      if (summaryTargetStamp > 0) snprintf(fname, sizeof(fname), "/%08ld.CSV", summaryTargetStamp);
      else                        strlcpy(fname, "/NODATE.CSV", sizeof(fname));
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
      // Pages break at line ends, each a whole SMS headed "SUMMARY <date> i/N" (see smsSendPages).
      sumSegTotal = smsPageCount(sumReport, smsPageRoom(sumTitle()));
      uint8_t capN = (sumMode == SUM_FULL) ? FULL_SUMMARY_MAX_SMS : SUMMARY_MAX_SMS;
      if (capN > 0 && sumSegTotal > capN) sumSegTotal = capN;   // 0 = unlimited (FULL by request)
      sumSegIdx = 0;
      sumPos = smsSkipBreak(sumReport, 0);
      summaryStage = SUM_STREAM;
      return;
    }

    case SUM_STREAM: {
      int len = sumReport.length();
      if (sumSegIdx >= sumSegTotal || sumPos >= len) {   // done
        sumReport = ""; summaryPending = false; summaryStage = SUM_IDLE; return;
      }
      if (gsm.queued() >= GSM_QUEUE_SIZE - 1) return;    // queue nearly full -> wait (paced, no drops)
      String title = sumTitle();
      int room = smsPageRoom(title);
      int end = smsPageEnd(sumReport, sumPos, room);
      String msg;
      if (sumSegTotal == 1 && smsSkipBreak(sumReport, end) >= len) {
        msg = title + "\n" + sumReport.substring(sumPos);           // the whole report fits one SMS
      } else {
        String page;
        if (sumSegIdx == sumSegTotal - 1 && smsSkipBreak(sumReport, end) < len) {   // capped, more text remains
          end = smsPageEnd(sumReport, sumPos, room - 19);
          page = sumReport.substring(sumPos, end) + "\n(more on SD card)";
        } else {
          page = sumReport.substring(sumPos, end);
        }
        msg = title + " " + String(sumSegIdx + 1) + "/" + String(sumSegTotal) + "\n" + page;
      }
      replyTarget = summaryReplyTo; sendSMS(msg); replyTarget = "";
      sumPos = smsSkipBreak(sumReport, end);
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
      // A FORCE run holds its frame in WO_PENDING instead of a pendingRun slot, so it needs the
      // same net: without one it would sit in ACTIVE_STATE forever with ESP2 dark, and the
      // dashboard command would stay "accepted" and never resolve.
      if (wo.active && wo.stage == WO_PENDING) {
        if (esp2Available) dispatchPendingWorkOrder();
        else if (millis() - esp2WarmupMs > ESP2_WARMUP_TIMEOUT_MS) {
          raiseFault('M', "ESP2_NO_READY", "FORCE");    // executor never came up for the forced run
          if (forceCmdId[0]) firebaseQueueStatus(forceCmdId, "failed", "ESP2 never powered up");
          forceAbort("ESP2_NO_READY");                  // release an A-then-B sequence (see pollESP2)
          forceCmdId[0] = '\0'; forceMask = 0;
          wo.active = false; wo.stage = WO_IDLE; wo.colIdx = -1;
          runUiAbort("ESP2_NO_READY");                  // release the LCD takeover (runLocked)
          esp2SetPower(false);
          setState(IDLE_STATE);
        }
      }
      else if (pendingRun.active && !wo.active) {
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
  if (webManualHeld()) return;            // web operator holds the rig; no scheduled run may start
  if (actuationsDisabled) return;         // operator lockout: monitor only, never energize
  if (uiMode != UI_DATA) return;          // operator in Settings/Testing -> pause automation
  if (sysState != IDLE_STATE) return;     // only dispatch new work from IDLE
  if (wo.active) return;                   // ESP2 busy -> sequential (sec.14.2.0.1)
  if (pendingRun.active) return;           // a run is already warming ESP2 up
#if BATTERY_SAFETY_ENABLED
  if (batteryCritical) return;             // battery-critical -> hold all runs (READY, not active)
#endif
  if (!rtcOk) return;
  {                                       // secondary battery low / unmonitored: no inverter use may start
    // Logged once per reason (2026-11-17): a scheduled run that does not start must say why.
    static const char *lastTag = nullptr;
    const char *tag = secBattBlocksAc() ? secBlockTag() : nullptr;
    if (tag != lastTag) { if (tag) logEvent("ESP1", "CTRL", String("AC_BLOCKED|") + tag); lastTag = tag; }
    if (tag) return;
  }
  // NOTE: ESP2 is OFF during idle (sec.18.8) -- do NOT gate on esp2Available here; a due
  // column powers ESP2 up on demand below and the work order is sent once it reports READY.

  uint16_t mod = minuteOfDay();

  // A dosed tank left behind by a reset goes first, to the column it was mixed for, whatever its window
  // or soil reading says now: that batch has nowhere else to go (the tank has no drain) and the next
  // column would otherwise inherit it. ESP2 doses only what is still owed. Snoozed, disabled and
  // skipped-today columns still pass: the operator's choice wins over finishing the batch. Not at
  // night: nobody expects pumps then, and the batch keeps until morning.
  uint8_t dosedNow = isNight() ? 0 : tankDosedMask;
  int order[NUM_COLUMNS], orderN = 0;
  for (int c = 0; c < NUM_COLUMNS; c++) if (dosedNow & (1 << c)) order[orderN++] = c;
  for (int c = 0; c < NUM_COLUMNS; c++) if (!(dosedNow & (1 << c))) order[orderN++] = c;

  for (int oi = 0; oi < orderN; oi++) {
    int c = order[oi];
    bool finishDosed = dosedNow & (1 << c);
    if (!COLUMN_ENABLED[c]) continue;                              // config, not a fault -> no CTRL log spam
    // Manual (2026-11-18): the operator waters this column by hand today (forced runs), so no scheduled run --
    // nor the finishing of a dosed batch, the same as a snoozed column. Its readings carry on as usual.
    if (colManualToday(c)) {
      long today = planToday();
      ctrlNote(c, (today >= 0 && planMarkFor(colPlan[c], today) == PLAN_MANUAL) ? "MANUAL|plan" : "MANUAL|default");
      continue;
    }
    // Operator (or the auto-cancel breaker) suppressed this column for a while. Logged like every
    // other skip reason so the CSV shows why a due column was passed over.
    if (colSnoozeUntil[c] && (long)(millis() - colSnoozeUntil[c]) < 0) {
      ctrlNote(c, "SNOOZED|" + String((colSnoozeUntil[c] - millis()) / 60000 + 1) + "min");
      continue;
    }
    if (dayDoneToday(c)) { ctrlNote(c, "SERVICED_TODAY"); continue; }   // the whole day (a STOP reply, a lost fault)
    // schedule axis: AUTO uses the default window, MANUAL the operator-set ones (sec.18.10.7.2) -- up to
    // MAX_RUNS_PER_DAY of them since 2026-11-05. A column is watered at most once in each window.
    int win = windowAt(c, mod);
    if (!finishDosed && win < 0) {                                 // outside every service window
      String wins;
      for (int w = 0; w < windowCount(c); w++) wins += (w ? "," : "") + hhmm(windowStart(c, w)) + "-" + hhmm(windowEnd(c, w));
      ctrlNote(c, "OUTSIDE_WINDOW|now=" + hhmm(mod) + "|win=" + wins);
      continue;
    }
    if (!finishDosed && windowDoneToday(c, win)) {
      ctrlNote(c, String("SERVICED_TODAY|win=") + (win + 1) + "/" + windowCount(c));
      continue;
    }
    // Whole-Nano-link staleness: sensor.soil[c]/npkValid[c] only reflect the last successfully parsed
    // packet -- neither is automatically invalidated by the LINK going down (only a per-field -1
    // sentinel is caught below), so a Nano outage previously let the schedule irrigate/fertigate on
    // arbitrarily old data for as long as the outage lasted. heartbeatTick's NANO_SILENCE alert is
    // informational only and nothing here gated on it. Same NANO_STALE_MS threshold npkMoistPct()
    // already uses for the analogous per-sensor case. Its value is the silence timeout (decision D5).
    if (!sensor.lastNanoMs || millis() - sensor.lastNanoMs > NANO_STALE_MS) {
      ctrlNote(c, "NANO_STALE");
      continue;
    }
    if (tankCheckNeeded) { ctrlNote(c, "TANK_CHECK"); continue; }  // the operator must confirm the tank first
    if (!finishDosed && sensor.soil[c] < 0) { ctrlNote(c, "SOIL_INVALID"); continue; }   // invalid soil (faulted elsewhere)
    if (!finishDosed && sensor.soil[c] >= colStartPct(c)) {        // not dry enough (its own line, 18 Nov 2026)
      ctrlNote(c, "NOT_DRY|soil=" + String(sensor.soil[c]) + ">=" + String(colStartPct(c)));
      continue;
    }
    if (reservoirLow(sensor.msTank != 0, sensor.resLevel, resPauseLevel())) {   // reservoir guard (sec.14.4.1)
      // A1: latch so the fault/SMS fires ONCE per low episode (cleared on recovery in powerTick-
      // style check below), not every loop while a column sits due over a low reservoir.
      ctrlNote(c, "RES_LOW|res=" + String((int)sensor.resLevel) + "<" + String((int)resPauseLevel()));
      if (!resLowLatched) { resLowLatched = true; raiseFault('M', "RES_LOW", "RESERVOIR"); }
      continue;
    }
    // Nutrients at most once a day (2026-11-05): with several waterings a day every due window could otherwise
    // dose. decideFertigate() isn't asked once they've gone in today, so its NPK checks don't log for nothing.
    bool fedToday = col[c].fertStamp == currentDayStamp;
    bool fert = finishDosed || (!fedToday && decideFertigate(c));  // a dosed batch still needs its mix and flush
    ctrlNote(c, finishDosed ? "DUE->FINISH_DOSED" : fert ? "DUE->FERTIGATE"
                : fedToday ? "DUE->IRRIGATE|FED_TODAY" : "DUE->IRRIGATE");   // column is being serviced now
#if BATTERY_SAFETY_ENABLED
    if (batteryLow) fert = false;                                  // low battery -> irrigation only (READY, not active)
#endif
    setState(ACTIVE_STATE);                                        // nanoPaceTick sends ACTIVE on this transition
    // OFF-during-idle (sec.18.8): queue the run, power ESP2 up, and let its READY dispatch the
    // work order (stateMachineTick ACTIVE_STATE is the warm-up timeout safety net).
    pendingRun.active = true; pendingRun.colIdx = c; pendingRun.fertigate = fert;
    pendingRun.window = (int8_t)win;                               // -1 for a dosed batch finished outside a window
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
    // New-F10 (audit): sensor.npkReason[c] (TIMEOUT/BADADDR/BADLEN/BADCRC, set at the SOIL/NPK packet
    // parse) already distinguishes "dead sensor/wiring" from "bus noise" for the LCD health line a few
    // hundred lines below -- it was never carried into this fault, unlike every other enrichment fixed
    // this pass series.
    String npkLoc = String(c == 0 ? "COL_A" : c == 1 ? "COL_B" : "COL_C") + "|reason=" + sensor.npkReason[c];
    raiseFault('M', "NPK_FAULT", npkLoc.c_str());
    // Fertigation-capable column falls back to irrigation-only because NPK is invalid. Log the DOWNGRADE
    // as its own event so the thesis dataset can tell "fertigated" from "watered only" (sec.M-3); the
    // NPK_FAULT above marks the sensor, this marks the decision consequence.
    logEvent("ESP1", "CTRL", String("COL_") + COL_TAG[c] + "|FERT_DOWNGRADE|reason=NPK_INVALID");
    return false;
  }
  // The probe needs moist soil for its N/P/K to mean anything -- in dry soil it returns numbers that
  // look plausible and are not. Water first, measure next cycle: this is self-correcting, because
  // the irrigation this triggers is exactly what makes the reading trustworthy. NPK_MIN_MOIST_PCT
  // sits below soilStartPct on purpose; at or above it, fertigation could never run at all.
  // Judge this on the PROBE'S OWN moisture, not the blended column figure. What determines whether
  // these N/P/K numbers are readable is the moisture at THIS probe -- and the blend deliberately
  // excludes the NPK when it disagrees with the capacitive pair, so using sensor.soil[c] would let
  // bad nutrient data through in exactly the case the gate exists for: probe sitting dry at 5 %,
  // capacitive probes elsewhere in the column reading 40 %, blend says 40 %, gate passes.
  // npk[c][0] is guaranteed real here -- npkValid is false if ANY of the seven channels is -1, and
  // that was already checked above.
  float probeMoist = sensor.npk[c][0];
  if (probeMoist < NPK_MIN_MOIST_PCT) {
    logEvent("ESP1", "CTRL", String("COL_") + COL_TAG[c] + "|FERT_DOWNGRADE|reason=DRY_SOIL"
                             + "|npkMoist=" + String(probeMoist, 1)
                             + "|col=" + String(sensor.soil[c]) + "|min=" + String(NPK_MIN_MOIST_PCT));
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
    logEvent("ESP1", "NET", "GSM|rssi=" + String(lastRssi) + "|creg=" + String(gsm.cregStat()) +
                            "|cereg=" + String(lastCereg) + "|cpin=" + String(simReady ? "READY" : "NOTREADY") +
                            "|modem=" + String(gsm.ready() ? "READY" : gsm.absent() ? "ABSENT" : "INIT") +
                            "|baud=" + String(gsm.baud()) + "|dr=" + String(gsm.deliveryReports() ? "on" : "off"));
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
    calBattV.degree = r.degree; for (int k = 0; k < 4; k++) calBattV.coef[k] = r.coef[k]; battVCalLoaded = true;
  }
  // The "i" (current-channel) NVS blob is intentionally no longer read here -- see the calBattV
  // comment above (audit fix, 2026-09-08). A pre-existing "i" blob from an older firmware/bench-tool
  // run is simply ignored now, same net effect as before (it was never applied to anything).
  p.end();
  battCalLoaded = battVCalLoaded;
  // Coefficients pasted into calBattV from the bench tool are a real calibration too (CLAUDE.md
  // documents both routes), so only the untouched placeholder -- degree 1, {0,1,0,0}, which just
  // returns the raw ADC fraction -- keeps voltage unpublished.
  bool placeholderV = (calBattV.degree == 1 && calBattV.coef[0] == 0.0 && calBattV.coef[1] == 1.0 &&
                       calBattV.coef[2] == 0.0 && calBattV.coef[3] == 0.0);
  if (!placeholderV) battVCalLoaded = true;
  battOptoCal = battVCalLoaded;                        // battVCalLoaded also goes true for the main INA226
  Serial.printf("Battery ADC cal: %s\n", battCalLoaded ? "loaded from NVS"
                                       : battVCalLoaded ? "using compiled coefficients"
                                                        : "using placeholder defaults (calibrate!)");
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
  // The ACS758 is read either way, so a dead ADC still shows on the Diag page while the INA226 is in use.
  float vAdc = 0;
  acsOk = acsReadVolts(vAdc);
  const InaMon &m = powerIna[INA_MAIN];
  battFromIna = inaUsable(m);
  if (battFromIna) {
    // The shunt bar behind the ISO1540: isolated, signed, and needs no opto calibration.
    battV = m.v; battIsigned = m.i;
    battI = fabsf(battIsigned);
    battIOk = true; battVCalLoaded = true;
  } else {
    battVCalLoaded = battOptoCal;
    battV = (float)applyPoly(calBattV, readAdcTrimmed(PIN_BATT_V));
    // Current comes from the ACS758 via the ADS1115, NOT the opto ADC on GPIO34 (which read 0.00 A on
    // every sample for 16 days). On an I2C failure hold the last value and clear acsOk so the fault is
    // visible on the Diag page -- reporting 0 A would repeat exactly the failure this replaces.
    if (acsOk) {
      battIsigned = (vAdc * ACS_DIV - ACS_ZERO_V) / ACS_SENS_V_PER_A;   // signed: 050B is bidirectional
      battI = fabsf(battIsigned);
    }
    battIOk = acsOk;
  }
  battP = battV * battI;
}

// Battery protections tied to the (removed) INA226 -- READY, NOT IMPLEMENTED. The real actions live inside
// BATTERY_SAFETY_ENABLED so flipping that one define re-enables them (see also controlTick / exerciseTick).
void batterySafetyTick() {
#if BATTERY_SAFETY_ENABLED
  static bool wasLow = false, wasCrit = false;
#if !SECONDARY_BACKUP_ENABLED
  // With the backup live, backupTick owns a critical main: this E-stop's GPIO4 cut would drop the boards
  // onto the flat main battery at the very moment the backup is meant to take over.
  if (batteryCritical && !wasCrit) raiseFault('C', "BATTERY_CRITICAL", "BATT");   // -> enterEmergencyStop
  else
#endif
  if (batteryLow && !wasLow)       raiseFault('M', "BATTERY_LOW", "BATT");
  wasLow = batteryLow; wasCrit = batteryCritical;
#endif
}

void powerTick() {
  if (millis() - lastInaMs < INA_READ_INTERVAL_MS) return;
  unsigned long nowMs = millis();
  float dtH = (lastInaMs == 0) ? 0.0f : (nowMs - lastInaMs) / 3600000.0f;   // hours since last read
  lastInaMs = nowMs;
  readBatteryAdc();                                    // main INA226, else opto + ACS758 -> battV/battI/battP

  // Energy (B2, sec.12.1.3). The shunt bar knows the direction, so only discharge counts as consumption.
  // The ACS758 fallback keeps its documented rule: every amp counts, as it is not trusted for direction.
  // Needs real volts: an uncalibrated opto battV is just the raw ADC fraction (0..1).
  if (battVCalLoaded && battIOk && (!battFromIna || battIsigned > 0)) energyConsumedWh += (double)battP * dtH;
  // Charged = what the solar module measured. A small negative reading is offset, not generation.
  const InaMon &sol = powerIna[INA_SOLAR];
  if (inaUsable(sol) && sol.i > 0) energyChargedWh += (double)(sol.v * sol.i) * dtH;

  // Battery state flags -- DISPLAY/LOG (and, if BATTERY_SAFETY_ENABLED, the protective actions).
  // Calibrated only: the placeholder's 0..1 "volts" would otherwise read as a critically flat battery,
  // and with the safety flag on that would be an emergency stop.
  // Held for BATT_FLAG_HOLD_MS, because the main INA226 reads the sag of every inverter start.
  static uint32_t critSinceMs = 0, lowSinceMs = 0;
  batteryCritical = battFlagHeld(battVCalLoaded && battV > 0 && battV < BATT_CRIT_V, nowMs, &critSinceMs, BATT_FLAG_HOLD_MS);
  batteryLow      = battFlagHeld(battVCalLoaded && battV > 0 && battV < BATT_LOW_V,  nowMs, &lowSinceMs,  BATT_FLAG_HOLD_MS);
  batterySafetyTick();                                 // ready-but-disabled protections

  // B3: log a PWR snapshot at most ~1/min, OR immediately on a battery-state change (sec.25.2.1).
  static unsigned long lastPwrLogMs = 0;
  bool battStateChanged = (batteryLow != prevBattStateLow) || (batteryCritical != prevBattStateCrit);
  if (battStateChanged || lastPwrLogMs == 0 || millis() - lastPwrLogMs >= PWR_LOG_INTERVAL_MS) {
    lastPwrLogMs = millis();
    prevBattStateLow = batteryLow; prevBattStateCrit = batteryCritical;
    // Uncalibrated rows stay in the log for bench work, under names the SUMMARY parser does not average.
    String vp = battVCalLoaded ? "|V=" + String(battV, 2) + "|I=" + String(battI, 2) + "|P=" + String(battP, 1)
                               : "|VRAW=" + String(battV, 3) + "|I=" + String(battI, 2);
    logEvent("ESP1", "PWR", "BATT" + vp + "|SRC=" + (battFromIna ? "INA" : "OPTO") +
             "|CONS=" + String((int)(energyConsumedWh + 0.5)) + "Wh|CHG=" + String((int)(energyChargedWh + 0.5)) + "Wh");
    // PWRS, not PWR: the SUMMARY parser averages every PWR row as the main battery.
    if (inaUsable(sol))
      logEvent("ESP1", "PWRS", "SOLAR|V=" + String(sol.v, 2) + "|I=" + String(sol.i, 3) + "|P=" + String(sol.p, 1));
  }
}

/* =============================================================================
 *  SECONDARY BATTERY  (INA226 at INA226_ADDR, R010 shunt) -- see SECONDARY_BATT_ENABLED
 * ========================================================================== */
static bool inaWrite(uint8_t addr, uint8_t reg, uint16_t v) {
  Wire.beginTransmission(addr); Wire.write(reg); Wire.write(v >> 8); Wire.write(v & 0xFF);
  return Wire.endTransmission() == 0;
}
static bool inaRead(uint8_t addr, uint8_t reg, uint16_t &out) {
  Wire.beginTransmission(addr); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)2) != 2) { while (Wire.available()) Wire.read(); return false; }
  out = ((uint16_t)Wire.read() << 8) | Wire.read();
  return true;
}
// Probe + configure. The manufacturer/die IDs prove it is really an INA226 (0x40 is a common
// address); the calibration read-back proves the bus carries writes. No reset/delay(): writing
// CONFIG and CAL sets every register this code reads.
static bool inaInit(uint8_t addr, uint16_t cal) {
  uint16_t mfg, die, calBack;
  if (!cal) return false;
  if (!inaRead(addr, 0xFE, mfg) || !inaRead(addr, 0xFF, die)) return false;
  if (mfg != 0x5449 || die != 0x2260) return false;
  if (!inaWrite(addr, 0x00, SEC_INA_CONFIG) || !inaWrite(addr, 0x05, cal)) return false;
  return inaRead(addr, 0x05, calBack) && calBack == cal;
}
static bool secInaRead(uint8_t reg, uint16_t &out) { return inaRead(INA226_ADDR, reg, out); }
static bool secInaInit() { return inaInit(INA226_ADDR, secInaCal); }

/* Solar + main battery modules behind the ISO1540. Their power comes from the main-battery side, so a
 * module going quiet says the isolator or its supply needs attention; it says nothing about the boards.
 *
 * A module that loses its supply for a moment comes back with CAL = 0 and goes on answering, with the right
 * voltage and a current of exactly 0 A: an inverter start can do that to a regulator on the main battery,
 * and the wrong reading would look perfectly healthy. So CAL is read back on every pass, like the first
 * time the module was found. */
void powerInaTick() {
  unsigned long now = millis();
  for (uint8_t k = 0; k < INA_COUNT; k++) {
    InaMon &m = powerIna[k];
    if (!m.ok) {
      if (m.lastTryMs && now - m.lastTryMs < SEC_RETRY_MS) continue;
      m.lastTryMs = now ? now : 1;
      m.ok = inaInit(m.addr, m.cal);
      if (!m.ok) continue;
      m.failN = 0; m.lastReadMs = 0; m.noV = false;
      logEvent("ESP1", "PWRS", String("INA226|") + m.name + "|FOUND");
    }
    if (m.lastReadMs && now - m.lastReadMs < PWR_INA_READ_MS) continue;
    m.lastReadMs = now ? now : 1;
    uint16_t rBus, rCur, rCal;
    if (!inaRead(m.addr, 0x02, rBus) || !inaRead(m.addr, 0x04, rCur) || !inaRead(m.addr, 0x05, rCal)) {
      if (++m.failN >= 3) {                                 // debounced: one NAK is bus noise, three is gone
        m.ok = false; m.haveV = false; m.lastTryMs = now ? now : 1;
        logEvent("ESP1", "PWRS", String("INA226|") + m.name + "|LOST");
      }
      continue;
    }
    m.failN = 0;
    if (rCal != m.cal) {                                    // it restarted: this current reading is not real
      m.ok = inaInit(m.addr, m.cal);
      logEvent("ESP1", "PWRS", String("INA226|") + m.name + (m.ok ? "|RESTARTED" : "|LOST"));
      m.haveV = false; m.lastTryMs = now ? now : 1;
      continue;
    }
    float v = inaBusVolts(rBus, m.busFactor);
    if (v < m.minV) {                                       // a loose VBUS lead, not a 12 V battery at 3 V
      if (!m.noV) logEvent("ESP1", "PWRS", String("INA226|") + m.name + "|NO_BUS_VOLTAGE|V=" + String(v, 2));
      m.noV = true; m.haveV = false;
      continue;
    }
    m.noV = false;
    m.v = m.haveV ? m.v + 0.3f * (v - m.v) : v;             // light filter, like the secondary's
    m.haveV = true;
    m.i = inaAmps(rCur, m.lsb, m.invert, m.offsetA);
    m.p = m.v * m.i;
  }
}

// Low latch with hysteresis, critical flag, and (only when switching is live) the alerts. One-shot
// per episode, like resLowLatched, so a sagging battery can't storm SMS.
static void secBattPolicy() {
  bool valid = secInaOk && secHaveV;
  if (valid) {
    float pct = (float)secBattPct(secRestV);       // the resting charge, not the raw voltage (2026-11-17)
    if (!secLow && pct <= SEC_BLOCK_PCT) secLow = true;
    else if (secLow && pct >= SEC_UNBLOCK_PCT) secLow = false;
    secCrit = (secRestV < SEC_BATT_CRIT_V);
  }
#if SECONDARY_BATT_ENABLED
  // raiseFault sends nothing during Testing. Setting these one-shots there would lose the alert for the
  // whole episode, so hold them until Testing ends.
  if (sysState == TEST_MODE) return;
  static bool missAlerted = false, lowAlerted = false, critAlerted = false;
  if (!valid) {
    if (!missAlerted) {
      missAlerted = true;
      raiseFault('M', "SENSOR_FAIL", secNoPack ? "INA226|NO_PACK_CHECK_WIRING|AC_BLOCKED" : "INA226|SECONDARY_BATT|AC_BLOCKED");
    }
    return;
  }
  missAlerted = false;
  if (secLow && !lowAlerted) {
    lowAlerted = true;
    raiseFault('M', "BATT2_LOW", (String("V=") + String(secV, 2) + "|rest=" + String(secRestV, 2) + "|pct=" +
                                  String(secBattPct(secRestV)) + "|AC_BLOCKED").c_str());
  } else if (!secLow && lowAlerted) {
    lowAlerted = false; logEvent("ESP1", "PWR2", "BATT2|RECOVERED|V=" + String(secV, 2));
  }
  if (secCrit && !critAlerted) { critAlerted = true; raiseFault('M', "BATT2_CRIT", (String("V=") + String(secV, 2)).c_str()); }
  else if (!secCrit) critAlerted = false;
#endif
}

// Learn secRint at a switch to the secondary (see secRestV): the last reading on main against the second one on the
// secondary (the first can still carry the transfer), taken within 15 s of each other, the current having risen by
// at least 0.1 A. Smoothed (30 % of each new step); saved to NVS once back on main, never during the switch.
static void secRintLearn(float v, float i, unsigned long now) {
  static float mainV = 0.0f, mainI = 0.0f;
  static unsigned long mainMs = 0;
  static bool done = false;
  static uint8_t secN = 0;
  if (!esp2OnSecondary) {
    mainV = v; mainI = i; mainMs = now; done = false; secN = 0;
    if (secRintDirty) { secRintDirty = false; prefsCal.putFloat("secR", secRint); }
    return;
  }
  if (done || ++secN < 2) return;
  done = true;
  if (mainMs == 0 || now - mainMs > 15000UL) return;
  float dI = i - mainI, dV = mainV - v;
  if (dI < 0.1f || !(dV > 0.0f)) return;
  float r = dV / dI;
  if (r < SEC_RINT_MIN || r > SEC_RINT_MAX) return;
  float was = secRint;
  secRint += 0.3f * (r - secRint);
  secRintDirty = true;
  logEvent("ESP1", "PWR2", "BATT2|RINT|step=" + String(r, 3) + "|was=" + String(was, 3) + "|now=" + String(secRint, 3) +
           "|main=" + String(mainV, 2) + "V/" + String(mainI, 3) + "A|sec=" + String(v, 2) + "V/" + String(i, 3) + "A");
}

void secBattTick() {
  static unsigned long lastTryMs = 0, lastReadMs = 0, lastLogMs = 0;
  static uint8_t failN = 0;
  static bool prevLow = false;
  unsigned long now = millis();
  if (!secInaOk) {
    if (lastTryMs == 0 || now - lastTryMs >= SEC_RETRY_MS) {
      lastTryMs = now;
      secInaOk = secInaInit();
      if (secInaOk) { failN = 0; lastReadMs = 0; logEvent("ESP1", "PWR2", "INA226|FOUND"); }
    }
    if (!secInaOk) { secBattPolicy(); return; }
  }
  if (lastReadMs && now - lastReadMs < SEC_READ_MS) return;
  lastReadMs = now;
  uint16_t rBus, rCur, rCal;
  if (!secInaRead(0x02, rBus) || !secInaRead(0x04, rCur) || !secInaRead(0x05, rCal)) {
    if (++failN >= 3) {                                   // debounced: one NAK is bus noise, three is gone
      secInaOk = false; secHaveV = false; secNoPack = false; lastTryMs = now;
      logEvent("ESP1", "PWR2", "INA226|LOST");
    }
    secBattPolicy(); return;
  }
  failN = 0;
  if (rCal != secInaCal) {                                // it restarted, so its current reads 0 A (see powerInaTick)
    secInaOk = secInaInit();
    logEvent("ESP1", "PWR2", secInaOk ? "INA226|RESTARTED" : "INA226|LOST");
    if (!secInaOk) { secHaveV = false; secNoPack = false; lastTryMs = now; secBattPolicy(); }
    return;                                               // the next pass reads through the restored CAL
  }
  float v = rBus * 0.00125f * secBusFactor;               // 1.25 mV/bit
  // No 4S pack reads this low. The chip is fine, so this is a loose VBUS lead or a pack whose negative is
  // not common with board ground -- report it as that, not as a flat battery.
  if (v < SEC_NO_PACK_V) {
    if (!secNoPack) logEvent("ESP1", "PWR2", "INA226|NO_PACK|V=" + String(v, 2));
    secNoPack = true; secHaveV = false;
    secBattPolicy(); return;
  }
  secNoPack = false;
  float i = (int16_t)rCur * SEC_CURRENT_LSB;
  if (secInvert) i = -i;
  i -= secOffsetA;
  secV = secHaveV ? secV + 0.3f * (v - secV) : v;        // light filter so a load step can't flap the latch
  secHaveV = true; secI = i; secP = secV * i;
  secRestV = secV + secI * secRint;                       // resting voltage: the charger's lift / the load's sag out
  secRintLearn(v, i, now);
  secBattPolicy();
  if (secLow != prevLow || lastLogMs == 0 || now - lastLogMs >= PWR_LOG_INTERVAL_MS) {
    lastLogMs = now; prevLow = secLow;
    logEvent("ESP1", "PWR2", "BATT2|V=" + String(secV, 2) + "|I=" + String(secI, 3) + "|P=" + String(secP, 1) +
             "|LOW=" + String(secLow ? 1 : 0) + "|SRC=" + (esp2OnSecondary ? "SECONDARY" : "MAIN") +
             "|REST=" + String(secRestV, 2) + "|PCT=" + String(secBattPct(secRestV)) + "|R=" + String(secRint, 3));
  }
}

// True when no inverter use may START: switching is live and the secondary is low or unmonitored.
// A run already in progress is never aborted by this -- only new work is refused.
#if SECONDARY_BATT_ENABLED
static bool bswRecharging() {
  // Latched once passed: a plain millis() < X comes true again for a minute when millis() wraps at 49.7 days.
  static bool bootWaitOver = false;
  if (!bootWaitOver && millis() >= BSW_RECHARGE_MS) bootWaitOver = true;
  return !bootWaitOver || (bswLastStrikeMs && millis() - bswLastStrikeMs < BSW_RECHARGE_MS);
}
#endif

bool secBattBlocksAc(const char **why) {
#if SECONDARY_BACKUP_ENABLED
  if (bkState != BK_IDLE)      { if (why) *why = "the main battery is critical"; return true; }
#endif
#if SECONDARY_BATT_ENABLED
  if (!secInaOk || !secHaveV) { if (why) *why = secNoPack ? "the secondary battery is not seen (check its wiring and ground)"
                                                          : "the secondary battery monitor (INA226) is not responding"; return true; }
  if (secLow)                  { if (why) *why = "the secondary battery is low"; return true; }
  if (bswLatched)              { if (why) *why = "battery-switch fault: a switch reset the controllers (clear it to retry)"; return true; }
  if (bswRecharging())         { if (why) *why = "the controllers just restarted: waiting for the hold-up to recharge"; return true; }
#endif
  (void)why;
  return false;
}

// Which of secBattBlocksAc's reasons it is (2026-11-17): "BATT2 low" used to be shown for all of them -- a monitor
// that is not answering, the battery-switch fault latch, or the minute after a restart all looked like a flat pack.
static unsigned long secRechargeLeftMs() {
#if SECONDARY_BATT_ENABLED
  unsigned long now = millis(), left = 0;
  if (now < BSW_RECHARGE_MS) left = BSW_RECHARGE_MS - now;
  if (bswLastStrikeMs && now - bswLastStrikeMs < BSW_RECHARGE_MS) {
    unsigned long l2 = BSW_RECHARGE_MS - (now - bswLastStrikeMs);
    if (l2 > left) left = l2;
  }
  return left;
#else
  return 0;
#endif
}
const char *secBlockLcd() {
  static char b[21];
  if (!secBattBlocksAc()) return "";
#if SECONDARY_BACKUP_ENABLED
  if (bkState != BK_IDLE) return "Main batt critical";
#endif
#if SECONDARY_BATT_ENABLED
  if (!secInaOk || !secHaveV) return secNoPack ? "BATT2 not seen: wire" : "BATT2 monitor off";
  if (secLow) { snprintf(b, sizeof(b), "BATT2 low: %d%%", secBattPct(secRestV)); return b; }
  if (bswLatched) return "Batt switch fault";
  if (bswRecharging()) { snprintf(b, sizeof(b), "Restarted: wait %lus", (secRechargeLeftMs() + 999UL) / 1000UL); return b; }
#endif
  return "AC pumps blocked";
}
const char *secBlockTag() {
  if (!secBattBlocksAc()) return "";
#if SECONDARY_BACKUP_ENABLED
  if (bkState != BK_IDLE) return "MAIN_BATT_CRIT";
#endif
#if SECONDARY_BATT_ENABLED
  if (!secInaOk || !secHaveV) return secNoPack ? "BATT2_NOT_SEEN" : "BATT2_MONITOR";
  if (secLow) return "BATT2_LOW";
  if (bswLatched) return "BATTSW_FAULT";
  if (bswRecharging()) return "BATTSW_RECHARGE";
#endif
  return "BATT2";
}

/* ---- Battery-switch reset protection (see BSW_LATCH_STRIKES) ------------------------------- *
 * ESP2 announces every P0 switch (INFO,BATT,SWITCHING,<to>) before it moves it. ESP1 then:
 * flushes the SD log, journals the switch in NVS ("battsw"/"pend"), opens a short quiet window
 * (no SD write, no new WiFi request, no SMS -- less load on the hold-up, nothing half-written if
 * the rails do drop) and answers BATTSW,GO. ESP2 confirms with INFO,BATT,SECONDARY|MAIN once the
 * boards survived. A strike is counted when:
 *   - ESP1 boots and finds its own journal set (ESP1 itself was reset by the switch), or
 *   - ESP2 sends READY while a switch is pending (ESP2 was reset; ESP1 survived), or
 *   - ESP2 reports INFO,BATT,RESET,<to> from its own journal (covers a lost announcement).
 * Reports of the same event are folded together (2-minute dedupe within one boot).             */
static void bswSavePending(uint8_t dir) {
  Preferences p; p.begin("battsw", false); p.putUChar("pend", dir); p.end();
}
static void bswSaveState() {
  Preferences p; p.begin("battsw", false);
  p.putUChar("n", bswStrikes); p.putUInt("t", bswLastStrikeT); p.putBool("lat", bswLatched);
  p.end();
}
static uint32_t bswNow() { return rtcOk ? rtcNow().unixtime() : 0; }

static void bswStrike(uint8_t dir, const char *by) {
  if (bswLastStrikeMs && millis() - bswLastStrikeMs < 120000UL) {
    logEvent("ESP1", "PWR2", String("BATTSW|RESET_DUP|by=") + by);
    return;
  }
  bswLastStrikeMs = millis() ? millis() : 1;
  uint32_t now = bswNow();
  // Unknown time / a clock that went backwards counts as inside the window (fail towards latching).
  bswStrikes = bswNextStrikes(bswStrikes, bswLastStrikeT, now, BSW_WINDOW_S);
  bswLastStrikeT = now;
  bool newlyLatched = !bswLatched && bswStrikes >= BSW_LATCH_STRIKES;
  if (newlyLatched) bswLatched = true;
  bswSaveState();
  String d = String(dir == 1 ? "TO_SECONDARY" : "TO_MAIN") + "|by=" + by + "|n=" + bswStrikes + "/" + BSW_LATCH_STRIKES;
  logEvent("ESP1", "PWR2", "BATTSW|RESET|" + d);
  raiseFault('M', "BATT_SWITCH_RESET", d.c_str());
  if (newlyLatched) {
    logEvent("ESP1", "PWR2", "BATTSW|LATCHED|AC_BLOCKED");
    raiseFault('M', "BATT_SWITCH_FAULT", "AC_BLOCKED|clear: LCD, SMS BATTSW,CLEAR, web, or auto 24h");
  }
}

// Why a manual or remote reboot must wait, or NULL. An ESP1 reboot also cuts ESP2 (setup() drives GPIO4 LOW),
// so it would lose a run or a held fault. An E-stop clears all of these, so this can never trap the operator.
const char *rebootBusyReason() {
  if (wo.active || pendingRun.active || fcPhase == FP_RUNNING || sysState == ACTIVE_STATE) return "a run is in progress";
  if (pendingExercise.active) return "a pump test is running";
  if (esp2Held) return "a fault is held";
  return NULL;
}

/* Tank check (decision D12). Set when a run is lost in a way that can leave ESP2's saved volume too low. (web v.4 measures
 * the mixing tank before every fill, so this now only holds back SCHEDULED runs; a forced run clears it, 2026-11-17.) No run starts until the operator has
 * looked at the tank and confirmed by SMS TANKCHECK,OK or the LCD Settings row. Confirming does not change ESP2's
 * saved volume, so if the tank holds unexpected water, empty it first with Testing (the booster to the column). */
void tankCheckSet(const char *why) {
  if (!tankCheckNeeded) {
    tankCheckNeeded = true;
    prefs.putBool("tankchk", true);
  }
  strlcpy(tankCheckWhy, why ? why : "", sizeof(tankCheckWhy));   // published for the website (18 Nov 2026)
  raiseFault('M', "TANK_CHECK", why);
}

void tankCheckClear(const char *by) {
  if (!tankCheckNeeded) return;
  tankCheckNeeded = false;
  tankCheckWhy[0] = '\0';
  prefs.putBool("tankchk", false);
  logEvent("ESP1", "STATE", String("TANK_CHECK|CONFIRMED|by=") + by);
}

void tankCheckBoot() {
  tankCheckNeeded = prefs.getBool("tankchk", false);
  bool wasLive = prefs.getBool("runlive", false);
  if (wasLive) prefs.putBool("runlive", false);
  if (runLossNeedsTankCheck(wasLive, bswBootStruck)) tankCheckSet("ESP1_RESET_DURING_RUN");
  else if (tankCheckNeeded) logEvent("ESP1", "STATE", "TANK_CHECK|STILL_NEEDED_AT_BOOT");
}

// Written only when it changes, so about twice per run.
void tankCheckTick() {
  static bool saved = false;
  bool live = wo.active && wo.stage == WO_ACKED && !esp2Held;
  if (live == saved) return;
  saved = live;
  prefs.putBool("runlive", live);
}

// A deliberate cut/restart while a switch is pending is not the switch's fault -- forget it so the
// next boot / READY does not count a strike.
void bswForgetPending() {
  if (!bswPendingDir) return;
  bswPendingDir = 0; battQuietUntilMs = 0;
  bswSavePending(0);
}

/* A stop while the boards are on the secondary drops P0 at once, and that transfer can reset ESP1 before
 * the stop has finished. Unjournaled, the rig would come back as if nothing had been asked of it: an E-stop
 * forgotten and the schedule running again, or a cancelled column re-run at once. So it is written down
 * first. Surviving BSW_CUT_SURVIVE_MS clears it (bswTick), so a later unrelated reset can't replay it.
 *   kind 1 = E-stop   kind 2 = cancelRun(mode) of column c                                             */
void bswNoteCut(uint8_t kind, int c, int mode) {
  if (!esp2OnSecondary && !bswPendingDir) return;
  Preferences p; p.begin("battsw", false);
  p.putUChar("cutk", kind); p.putChar("cutc", (int8_t)c); p.putChar("cutm", (int8_t)mode);
  p.end();
  bswCutMs = millis() ? millis() : 1;
}

/* A dashboard command in flight when the boards reset would sit on "accepted" for good, since its id is
 * only in RAM. Keep it on flash while a reset is possible; a boot that finds one marks it failed. */
void bswCmdJournal() {
  const char *id = forceCmdId[0] ? forceCmdId : fbRemoteExerciseId[0] ? fbRemoteExerciseId : puCmdId;
  if (!esp2OnSecondary && !bswPendingDir) id = "";
  if (!strcmp(id, bswCmdSaved)) return;
  strlcpy(bswCmdSaved, id, sizeof(bswCmdSaved));
  Preferences p; p.begin("battsw", false);
  if (bswCmdSaved[0]) p.putString("cmd", bswCmdSaved); else p.remove("cmd");
  p.end();
}

void bswClear(const char *by) {
  bool was = bswLatched;
  bswLatched = false; bswStrikes = 0; bswLastStrikeT = 0;
  bswSaveState();
  logEvent("ESP1", "PWR2", String("BATTSW|CLEARED|by=") + by + (was ? "" : "|was_not_latched"));
}

// Late in setup(): restore the latch, and count a strike if this very boot was caused by a switch.
void bswBoot() {
  Preferences p; p.begin("battsw", true);
  uint8_t pend = p.getUChar("pend", 0);
  bswStrikes = p.getUChar("n", 0); bswLastStrikeT = p.getUInt("t", 0); bswLatched = p.getBool("lat", false);
  uint8_t cutKind = p.getUChar("cutk", 0);
  int cutCol = p.getChar("cutc", -1), cutMode = p.getChar("cutm", -1);
  String lostCmd = p.isKey("cmd") ? p.getString("cmd", "") : String("");
  p.end();
  if (cutKind || lostCmd.length()) {            // clear first: this boot can never replay them twice
    Preferences w; w.begin("battsw", false);
    w.remove("cutk"); w.remove("cutc"); w.remove("cutm"); w.remove("cmd");
    w.end();
  }
  if (cutKind == 1) bswEstopRestore = true;     // setup() goes back into E-stop once it has finished
  if (cutKind == 2 && cutCol >= 0 && cutCol < NUM_COLUMNS) {
    if (cutMode == 0) { COLUMN_ENABLED[cutCol] = false; saveColEnable(cutCol); }
    else if (cutMode > 0) colSnoozeUntil[cutCol] = millis() + SNOOZE_LONG_MS;
    logEvent("ESP1", "PWR2", String("BATTSW|CANCEL_RESTORED|COL_") + COL_TAG[cutCol] + "|mode=" + cutMode);
  }
  if (lostCmd.length()) firebaseQueueStatus(lostCmd.c_str(), "failed", "the controllers reset during a battery switch");
  if (bswLatched) logEvent("ESP1", "PWR2", "BATTSW|LATCHED_AT_BOOT|AC_BLOCKED");
  if (pend == 1 || pend == 2) {
    bswSavePending(0);                          // clear first: this boot can never count twice
    String by = String("ESP1_RESET|rr=") + (int)esp_reset_reason();
    bswStrike(pend, by.c_str());
    bswBootStruck = true;
  } else if (pend != 0) bswSavePending(0);
}

void bswVerifyAnswer() {
  if (!bswVerdict) return;
  if (!strcmp(bswVerdict, "OK")) sendEsp2("BATTSW,OK");
  else sendEsp2(String("BATTSW,FAIL,") + bswVerdict);
}

void bswTick() {
  // INFO,BATT,VERIFY (2026-11-16): decide whether the switch was survived by every board.
  if (bswVerifyStartMs && !bswVerdict) {
    // ESP1 never saw this switch announced (it restarted since, or the announcement was lost): it cannot vouch
    // for it, and a reset would not have been journaled -- refuse.
    if (!bswGoMs || millis() - bswGoMs > 15000UL)               bswVerdict = "ESP1_NO_GO";
    else if (nanoBootMs && (long)(nanoBootMs - bswGoMs) >= 0)    bswVerdict = "NANO_RESET";
    else if (sensor.lastNanoMs && (long)(sensor.lastNanoMs - bswVerifyStartMs) > 0) bswVerdict = "OK";
    else if (millis() - bswVerifyStartMs >= BSW_VERIFY_NANO_MS) bswVerdict = "NANO_SILENT";
    if (bswVerdict) {
      logEvent("ESP1", "PWR2", String("BATTSW|VERIFY|") + bswVerdict);
      bswVerifyAnswer();
    }
  }
  if (bswVerifyStartMs && millis() - bswVerifyStartMs > 10000UL) { bswVerifyStartMs = 0; bswVerdict = nullptr; }
  // A switch that was announced but neither confirmed nor followed by a reboot: ESP2 survived and
  // only the confirmation was lost. Drop the journal so a much later, unrelated READY can't count.
  if (bswPendingDir && millis() - bswPendingMs > BSW_PENDING_MAX_MS) {
    logEvent("ESP1", "PWR2", "BATTSW|UNCONFIRMED|assumed_survived");
    bswForgetPending();
  }
  if (battQuietUntilMs && !battQuietActive()) battQuietUntilMs = 0;
  bswCmdJournal();
  if (bswCutMs && millis() - bswCutMs >= BSW_CUT_SURVIVE_MS) {      // alive: that stop did not reset us
    bswCutMs = 0;
    Preferences p; p.begin("battsw", false);
    p.remove("cutk"); p.remove("cutc"); p.remove("cutm");
    p.end();
  }
  // ESP2's journal report of an ESP1-boot strike arrives within seconds of STARTUP_SYNC; after 5 min
  // any report is a new event again.
  if (bswBootStruck && millis() > 300000UL) bswBootStruck = false;
  // Auto re-arm BSW_WINDOW_S after the last strike (needs the RTC; without it only a manual clear).
  static unsigned long lastCheckMs = 0;
  if (millis() - lastCheckMs < 60000UL) return;
  lastCheckMs = millis();
  if ((bswLatched || bswStrikes) && bswLastStrikeT != 0) {
    uint32_t now = bswNow();
    if (bswAutoClearDue(bswLastStrikeT, now, BSW_WINDOW_S)) {
      bool was = bswLatched;
      bswClear("AUTO_24H");
      if (was) sendSMS("INFO: Battery switching re-armed.\n24 h passed since the last switch reset. AC pumps are allowed again.");
    }
  }
}

/* =============================================================================
 *  SECONDARY AS BACKUP  (see SECONDARY_BACKUP_ENABLED, backupNext in pure_math.h)
 * -----------------------------------------------------------------------------
 *  The decisions are in backupNext(), which is host-tested. This wires it up: debounce the main battery,
 *  send the alerts, and ask ESP2 to hold or release P0. Reset-proof by construction: no state survives
 *  a reset except "the alert already went out" (NVS battsw/bka), so a reset lands on BK_IDLE with the
 *  boards on main, and the same steps run again -- without a second text, after the hold-up has
 *  recharged, and at most BSW_LATCH_STRIKES times, because each switch-caused reset is a strike.
 * ========================================================================== */
bool backupHoldsEsp2() {
  return bkState == BK_SWITCHING || bkState == BK_ACTIVE || bkState == BK_RETURNING;
}
const char *bkName(BkState s) {
  switch (s) {
    case BK_ALERT:     return "alert";
    case BK_WAIT:      return "waiting";
    case BK_SWITCHING: return "switching";
    case BK_ACTIVE:    return "active";
    case BK_RETURNING: return "returning";
    default:           return "idle";
  }
}
#if SECONDARY_BACKUP_ENABLED
// Critical alert WITHOUT raiseFault('C'), which would E-stop and cut ESP2. Also not suppressed in Testing.
static void bkCritAlert(const char *code, const String &loc) {
  faultsToday[0]++;
  logEvent("ESP1", "FAULT", String("CRIT|") + code + "|" + loc);
  lastFaultMsg = String(code) + " " + loc;
  lastFaultTime = tsString();
  wakeBacklight();
  smsFault('C', code, loc);
}
static String bkVolts() {
  return String("MAIN=") + String(battV, 2) + "|SEC=" + ((secInaOk && secHaveV) ? String(secV, 2) : String("--"));
}
static void bkSaveAlerted(bool on) {
  Preferences p; p.begin("battsw", false); p.putBool("bka", on); p.end();
}

void backupTick() {
  static bool alerted = false, booted = false;
  static unsigned long lowSince = 0, okSince = 0, stateMs = 0, lastSendMs = 0, retryAt = 0;
  unsigned long now = millis();
  if (!booted) {
    booted = true;
    Preferences p; p.begin("battsw", true); alerted = p.getBool("bka", false); p.end();
    stateMs = now;
  }
  bool idle = sysState == IDLE_STATE && !wo.active && !pendingRun.active && !pendingExercise.active && !esp2Held &&
              uiMode == UI_DATA && !actuationsDisabled && puPhase == PU_NONE && fcPhase == FP_NONE &&
              !webManualHeld() && !diagRemoteUntilMs;
  bool known = battVCalLoaded && battV > 0;
  // A run's sag is not a flat battery: below the line only counts with nothing drawing on main.
  if (known && battV < BATT_CRIT_V && (idle || bkState != BK_IDLE)) { if (!lowSince) lowSince = now ? now : 1; }
  else lowSince = 0;
  if (known && battV >= BACKUP_MAIN_RECOVER_V) { if (!okSince) okSince = now ? now : 1; }
  else okSince = 0;

  BackupIn in;
  in.idle         = idle;
  in.mainLowHeld  = lowSince && now - lowSince >= BACKUP_TRIGGER_MS;
  in.mainOkHeld   = okSince && now - okSince >= BACKUP_RECOVER_MS;
  in.secUsable    = secInaOk && secHaveV && !secLow && !bswLatched && !bswRecharging() &&
                    (!retryAt || (long)(now - retryAt) >= 0);
  in.secExhausted = !(secInaOk && secHaveV) || secCrit;
  in.alertDone    = alerted || gsm.queued() == 0 || now - stateMs >= BACKUP_ALERT_WAIT_MS;
  in.onSecondary  = esp2OnSecondary;
  in.timedOut     = now - stateMs >= BACKUP_SWITCH_TIMEOUT_MS;

  // A main that recovers ends the episode whichever state it finds, including a reboot's BK_IDLE.
  if (in.mainOkHeld && alerted) { alerted = false; bkSaveAlerted(false); }

  BkState from = bkState, to = backupNext(from, in);
  if (to != from) {
    bkState = to; stateMs = now; lastSendMs = 0;
    logEvent("ESP1", "PWR2", String("BACKUP|") + bkName(from) + "->" + bkName(to) + "|" + bkVolts());
    switch (to) {
      case BK_ALERT:
        // Only if this episode's text has not gone out yet: a reset must not send it again.
        if (!alerted)
          bkCritAlert("MAIN_BATT_CRIT", bkVolts() + (in.secUsable ? "|SWITCHING_TO_BACKUP" : "|BACKUP_NOT_AVAILABLE"));
        break;
      case BK_WAIT:
        if (from == BK_ALERT) {
          if (!alerted && gsm.queued() == 0) { alerted = true; bkSaveAlerted(true); }   // it left the modem
        } else {
          retryAt = now + BACKUP_RETRY_MS;                  // failed or lost: not straight back on
          esp2SetPower(false);
        }
        break;
      case BK_SWITCHING:
        esp2SetPower(true);
        break;
      case BK_ACTIVE:
        sendSMS(String("INFO: Now running on the BACKUP battery.\n") + smsPlain(bkVolts()));
        break;
      case BK_RETURNING:
        if (from == BK_ACTIVE && !in.mainOkHeld)
          bkCritAlert("BACKUP_EXHAUSTED", bkVolts() + "|RETURNED_TO_MAIN");
        else if (from == BK_SWITCHING && !in.mainOkHeld)
          bkCritAlert("BACKUP_SWITCH_FAILED", bkVolts() + "|STAYING_ON_MAIN");
        break;
      case BK_IDLE:
        retryAt = 0;
        if (from != BK_ALERT && from != BK_WAIT) esp2SetPower(false);
        sendSMS(String("INFO: Back on the MAIN battery.\n") + smsPlain(bkVolts()));
        break;
    }
  }
  // ESP2 boots first, and a frame can be lost: repeat until it confirms. Both commands are idempotent.
  if ((bkState == BK_SWITCHING || bkState == BK_RETURNING) && esp2Available && esp2Powered &&
      (!lastSendMs || now - lastSendMs >= BACKUP_RESEND_MS)) {
    lastSendMs = now ? now : 1;
    sendEsp2(bkState == BK_SWITCHING ? "BATTBK,ON" : "BATTBK,OFF");
  }
}
#else
void backupTick() {}
#endif

/* =============================================================================
 *  SCHEDULE  (daily report, daily Nano reset, day rollover)
 * ========================================================================== */
void scheduleTick() {
  if (!rtcOk) return;
  DateTime now = rtcNow();
  long ds = dayStamp(now);
  if (ds != currentDayStamp) {                 // midnight rollover -> reset daily counters
    bool firstAfterBoot = currentDayStamp < 0;
    currentDayStamp = ds;
    logKeepFrom = dayStamp(now - TimeSpan(LOG_KEEP_DAYS, 0, 0, 0));   // CSV logs kept in Firebase from this day on
    faultsToday[0] = faultsToday[1] = faultsToday[2] = 0;
    nanoResetsToday = 0; esp2PowerCycles = 0;
    reportSentToday = false; nanoDailyResetDone = false;
    energyConsumedWh = energyChargedWh = 0.0;  // reset daily energy totals (B2)
    for (int c = 0; c < NUM_COLUMNS; c++) {
      waterUsedToday[c] = nutrientUsedToday[c] = 0;
      wateringsToday[c] = feedingsToday[c] = 0;
    }
    // Not a new day, only a restart: today's water, nutrients and run counts carry on (2026-11-06).
    if (firstAfterBoot) restoreDayTotals(ds);
    // ...and so do the once-a-day jobs (2026-11-17 log): every restart after 03:00 reset the Nano again, and one after
    // 18:00 would send the daily report again. Their day is kept in NVS.
    if (firstAfterBoot) {
      nanoDailyResetDone = (prefs.getLong("nrsDay", -1) == ds);
      reportSentToday    = (prefs.getLong("rptDay", -1) == ds);
      // Starts today, for the website (18 Nov 2026: 40 boots on 17 Nov, most of them power loss). One NVS write a boot.
      bootsToday = (prefs.getLong("bootDay", -1) == ds) ? (uint16_t)(prefs.getUShort("bootN", 0) + 1) : 1;
      prefs.putLong("bootDay", ds); prefs.putUShort("bootN", bootsToday);
    } else {
      bootsToday = 0;                            // a new day began while running: no start today yet
    }
  }
  uint16_t mod = now.hour() * 60 + now.minute();

  if (!reportSentToday && mod >= DAILY_REPORT_MIN) {
    sendDailyReport(); reportSentToday = true; prefs.putLong("rptDay", ds);
    if (now.dayOfTheWeek() == SIM_CHECK_WEEKDAY) simHealthCheck();
  }
  // Not while a pump runs or the selector moves: the reset would blind the overflow watch mid-fill (ESP2
  // would hold the run as LEVEL_BLIND). It simply happens on a later pass, once the pump is off.
  if (!nanoDailyResetDone && mod >= DAILY_NANO_RESET_MIN && !levelRelayActive() && !servoReqActive) {     // sec.18.9.5.0.3 (exempt from HW limit)
    sendNanoCommand("RESET_REQ");
    logEvent("ESP1", "RESET", "NANO|RESET_REQ|DAILY");
    nanoDailyResetDone = true; prefs.putLong("nrsDay", ds);
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
static long uploadNextCatchupDay(long after, long notBefore) {
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
      if (stamp > after && stamp >= notBefore && stamp < currentDayStamp && (best == 0 || stamp < best)) best = stamp;
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
      long y = dayStamp(rtcNow() - TimeSpan(1, 0, 0, 0));
      char path[16]; snprintf(path, sizeof(path), "/%08ld.CSV", y);
      bool got = sdTake(200), exists = got && SD.exists(path); if (got) sdGive();
      if (exists) { manualUploadStamp = y; attempts = 0; }
    }
  }

  // The website's "Upload from the rig" (irrigation/logRequest, polled on core 0): first in line, today included.
  if (webLogReqStamp != 0) {
    long d = webLogReqStamp; webLogReqStamp = 0;
    manualUploadStamp = d; attempts = 0;
    logEvent("ESP1", "UPLOAD", "WEB_REQUEST|" + String(d) + ".CSV");
  }

  // 1) publish a finished result (netTask -> here)
  if (uploadResult != 0) {
    long s = uploadResStamp; int r = uploadResult, code = uploadHttp;
    bool fb = uploadWasFb;
    uploadResult = 0; lastAttemptMs = millis();
    const char *to = fb ? "|FIREBASE" : "|SUPABASE";
    if (r == 1) {
      logEvent("ESP1", "UPLOAD", "OK|" + String(s) + ".CSV" + to);
      if (fb) {                                   // only a COMPLETED day moves the mark (today is still growing)
        if (s > fbLogLast && s < currentDayStamp) { fbLogLast = s; prefs.putLong("fblogl", fbLogLast); }
      } else if (s > supaLast) { supaLast = s; saveSupaLast(); }
      if (manualUploadStamp == s) manualUploadStamp = 0;
      attempts = 0;
    } else if (code == -2) {                      // not on the SD card: nothing to retry
      logEvent("ESP1", "UPLOAD", "MISSING|" + String(s) + ".CSV" + to);
      if (manualUploadStamp == s) manualUploadStamp = 0;
      attempts = 0;
    } else {
      attempts++;
      String why = String("FAIL|") + s + ".CSV|http=" + code + "|try=" + attempts + to;
      if (fb && fbLogFailStep[0]) why += String("|step=") + fbLogFailStep + "|heap=" + fbLogFailHeap + "|blk=" + fbLogFailBlock;
      logEvent("ESP1", "UPLOAD", why);
      if (manualUploadStamp == s && attempts >= 5) { manualUploadStamp = 0; attempts = 0; }  // give up a stuck manual send
    }
    return;
  }

  if (uploadReqStamp != 0 || uploadBusy) return;                 // an attempt is in flight
  // netMux: this runs on core 1 nearly every loop() iteration; supaUrl/supaKey are also written
  // from core-0 netTask (handleSms's SUPA command, portalHandleSupa()).
  xSemaphoreTake(netMux, portMAX_DELAY);
  bool suConfigured = supaUrl.length() > 0 && supaKey.length() > 0;
  bool toFb = firebaseEnabled && fbUrl.length() >= 8;             // 2026-11-17: Firebase first (Supabase expired)
  xSemaphoreGive(netMux);
  if (!toFb && !suConfigured) return;    // no destination
  if (!wifiConnected) return;
  unsigned long wait = (attempts > 0) ? BACKOFF[attempts < 4 ? attempts - 1 : 3] : 5000UL;
  if (millis() - lastAttemptMs < wait) return;                   // backoff / gentle SD-scan throttle

  // Firebase: only a day someone ASKED for (the website's "Upload from the rig", SMS, the portal). The automatic catch-up of
  // every completed day (17 Nov 2026) is off since 18 Nov 2026 g: up to 14 days at ~1.2 MB each in 6 KB pieces never
  // finished on this link (logs/index on 18 Nov: nothing completed) and took the one TLS session down with every failed
  // piece. The SD card keeps every day; Supabase (only while Firebase is off) keeps its old catch-up.
  long target = (manualUploadStamp != 0) ? manualUploadStamp
              : toFb ? 0 : uploadNextCatchupDay(supaLast, 0);
  if (target == 0) return;
  uploadReqPartial = (target == currentDayStamp);                // today's file: sent as far as it is written
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
    smsFault('m', "NANO_SILENCE", "");                         // one-shot (queued, non-blocking)
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
  // FAST (2026-11-15): from ESP2's servo request until its PUMP,OFF -- the whole time water is moving
  // through the transfer or booster pump. FAST is a lease on the Nano (15 s), so it is renewed here every
  // NANO_FAST_RENEW_MS; the renewals are not logged.
  bool pumpStarting = servoOkRelayMs && millis() - servoOkRelayMs < SERVO_TO_PUMP_FAST_MS;   // servo OK -> PUMP,ON
  if (levelRelayActive() || servoReqActive || pumpStarting) {
    // TANK gap watchdog (2026-11-16 hardening): in FAST a TANK comes every ~0.3 s. None for NANO_TANK_GAP_MS (counted
    // from the later of the last TANK and entering FAST) = the Nano may have lost FAST (a reset it could not report,
    // a dropped command): send FAST again now instead of at the next renewal, and log the gap once.
    unsigned long now = millis();
    unsigned long ref = (lastTankMs && (long)(lastTankMs - fastSinceMs) > 0) ? lastTankMs : fastSinceMs;
    bool gap = (lastNanoPace == PACE_FAST) && ref && now - ref >= NANO_TANK_GAP_MS;
    if (gap && !tankGapLogged) {
      tankGapLogged = true;
      logEvent("NANO", "LINK", "TANK_GAP|ms=" + String(now - ref) + "|lnk=" + String(tankLinkPct()) + "%|ovf=" + String(nanoRxOverflows));
    }
    if (lastNanoPace != PACE_FAST || now - lastFastSentMs >= (gap ? NANO_TANK_GAP_MS : NANO_FAST_RENEW_MS)) {
      if (lastNanoPace != PACE_FAST) fastSinceMs = nzMs(now);
      sendNanoCommand("FAST", lastNanoPace != PACE_FAST);
      lastNanoPace = PACE_FAST; lastFastSentMs = now;
    }
    return;
  }
  if (sysState == ACTIVE_STATE || wo.active || pendingRun.active) {
    desired = PACE_ACTIVE;
  } else if (!rtcOk) {
    desired = PACE_DAY;                                  // no clock -> default to day idle
  } else {
    uint16_t mod = minuteOfDay();
    bool night = (mod >= NIGHT_START_MIN) || (mod < NIGHT_END_MIN);
    desired = night ? PACE_NIGHT : PACE_DAY;
  }
  static unsigned long lastPaceSentMs = 0;
  bool changed = (desired != lastNanoPace);
  // No TANK accepted since this boot (2026-11-17: the tanks read -1 after every restart). The pace command after a
  // restart is what makes the Nano send a full cycle at once; it goes out once and can be lost on this line, and in
  // the day the Nano's own cycle is 20 min. So until a TANK is in, send it again every NANO_FIRST_DATA_RETRY_MS.
  if (lastTankMs == 0 && lastPaceSentMs != 0 && millis() - lastPaceSentMs >= NANO_FIRST_DATA_RETRY_MS) changed = true;
  if (!changed && millis() - lastPaceSentMs < NANO_PACE_REFRESH_MS) return;   // refresh: see NANO_PACE_REFRESH_MS
  lastNanoPace = desired; lastPaceSentMs = millis();
  const char *paceCmd = (desired == PACE_ACTIVE) ? "ACTIVE" : (desired == PACE_DAY) ? "DAY" : "NIGHT";
  sendNanoCommand(paceCmd, changed);                     // only a real change is logged
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
  smsFault('M', "SELF_RESET", String(DEV_NAME[lost]) + " not responding"); // best-effort (may not flush before reboot)
  logFlush(true);                                               // persist what we can
  g_lastSelfResetDay = today;
  delay(50);                                                    // let the last UART/SD bytes drain
  bswForgetPending(); ESP.restart();
}

/* =============================================================================
 *  WiFi + THINGSPEAK TELEMETRY  (Part A: core-0 task, non-blocking uplink)
 * ========================================================================== */
// Core 1: BUSY for the link (LINK PACE, 18 Nov 2026 e) -- the rig is working, or about to. A watering window counts
// while it is open and not yet served, and from LINK_PRE_RUN_MIN before it opens: a run can start at any minute of it.
// Same column filters as controlTick(): enabled, not Manual today, not snoozed, not done for the day.
static bool linkBusyNow() {
  if (sysState == ACTIVE_STATE || wo.active || pendingRun.active || pendingExercise.active || fcPhase == FP_WAIT) return true;
  if (tankDosedMask && !isNight()) return true;                  // a dosed batch goes out at the next chance
  if (!rtcOk || actuationsDisabled) return false;
  uint16_t mod = minuteOfDay();
  for (int c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c] || colManualToday(c) || dayDoneToday(c)) continue;
    if (colSnoozeUntil[c] && (long)(millis() - colSnoozeUntil[c]) < 0) continue;
    for (int w = 0; w < windowCount(c); w++) {
      if (windowDoneToday(c, w)) continue;
      if (windowOpenOrSoon(mod, windowStart(c, w), windowEnd(c, w), LINK_PRE_RUN_MIN)) return true;   // pure_math.h
    }
  }
  return false;
}

// Core 1: snapshot the latest readings into the shared struct (POD -> spinlock). Throttled.
void telemetryCollect() {
  if (millis() - lastTelemCollectMs < 1000) return;
  lastTelemCollectMs = millis();
  linkBusyFlag = linkBusyNow();                                  // read by netTask (core 0), LINK PACE
  String deviceTime = tsString();                  // before the lock: an I2C read cannot run with interrupts off
  portENTER_CRITICAL(&telemMux);
  strlcpy(telem.deviceTime, deviceTime.c_str(), sizeof(telem.deviceTime));
  telem.temp = sensor.temp; telem.hum = sensor.hum; telem.lux = sensor.lux;
  telem.resLevel = sensor.resLevel; telem.mixLevel = sensor.mixLevel; telem.flow = sensor.flow;
  telem.resL = (sensor.msTank && sensor.resOk) ? sensor.resL : -1.0f;
  telem.mixL = (sensor.msTank && sensor.mixOk) ? sensor.mixL : -1.0f;
  for (int c = 0; c < NUM_COLUMNS; c++) {
    telem.soil[c] = sensor.soil[c];
    telem.npkValid[c] = sensor.npkValid[c];
    telem.npkN[c] = sensor.npk[c][4]; telem.npkP[c] = sensor.npk[c][5]; telem.npkK[c] = sensor.npk[c][6];
    telem.npkEC[c] = sensor.npk[c][2]; telem.npkPH[c] = sensor.npk[c][3];
    telem.npkMoist[c] = sensor.npk[c][0]; telem.npkTemp[c] = sensor.npk[c][1];
  }
  telem.battV = battV; telem.battP = battP; telem.battI = battI;
  telem.battIsigned_ = battIsigned; telem.battIOk_ = battIOk; telem.battFromIna_ = battFromIna;
  telem.solarOk_ = inaUsable(powerIna[INA_SOLAR]);
  telem.solarV_ = powerIna[INA_SOLAR].v; telem.solarI_ = powerIna[INA_SOLAR].i; telem.solarP_ = powerIna[INA_SOLAR].p;
  telem.chargedWh_ = energyChargedWh;
  telem.tankEC = tankEC; telem.tankPH = tankPH;
  // Mirror the per-group validity + coarse system state so core-0 uploaders read only from the snapshot.
  telem.envValid = sensor.envValid; telem.tankValid = sensor.tankValid; telem.lightValid = sensor.lightValid;
  telem.battVCalValid = battVCalLoaded;
  telem.state = sysState; telem.woActive = wo.active;
  // Diagnostics mirror. Ages are computed HERE (core 1) rather than published as raw millis(),
  // which is meaningless to a browser on a different clock.
  telem.esp2Avail = esp2Available; telem.esp2Pwr = esp2Powered; telem.esp2Lost = esp2CommLost;
  telem.esp2AgeMs = lastEsp2Ms ? (millis() - lastEsp2Ms) : 0xFFFFFFFF;
  telem.nanoAgeMs = sensor.lastNanoMs ? (millis() - sensor.lastNanoMs) : 0xFFFFFFFF;
  telem.gsmHealthAgeMs = lastGsmHealthMs ? (millis() - lastGsmHealthMs) : 0xFFFFFFFF;
  telem.pendRun = pendingRun.active; telem.pendEx = pendingExercise.active;
  telem.pendExIdx = pendingExercise.idx;
  telem.rtcOk_ = rtcOk; telem.sdOk_ = sdOk; telem.acsOk_ = acsOk;
  telem.secOk_ = secInaOk && secHaveV; telem.secLow_ = secLow; telem.onSecondary_ = esp2OnSecondary;
  telem.secV_ = secV; telem.secI_ = secI; telem.secP_ = secP; telem.secRest_ = secRestV;
  telem.bswLatched_ = bswLatched; telem.bswStrikes_ = bswStrikes;
  telem.acVMin_ = (int16_t)acVMin; telem.acVMax_ = (int16_t)acVMax;
  telem.acLastV_ = acLastV; telem.acLastAgeMs_ = acLastMs ? (uint32_t)(millis() - acLastMs) : 0xFFFFFFFF;
  telem.acAMin10_ = (int16_t)acAMin10; telem.acAMax10_ = (int16_t)acAMax10;
  telem.acLastI_ = acLastIMs ? acLastI : -1.0f;
  telem.acLastIAgeMs_ = acLastIMs ? (uint32_t)(millis() - acLastIMs) : 0xFFFFFFFF;
  telem.servoSide_ = servoSideNow;
  telem.servoSideAgeMs_ = servoSideMs ? (uint32_t)(millis() - servoSideMs) : 0xFFFFFFFF;
  telem.bkState_ = (uint8_t)bkState;
  telem.simOk = simReady; telem.netReg = netRegistered;
  telem.rssi = lastRssi; telem.creg = lastCreg;
  telem.battLow = batteryLow; telem.battCrit = batteryCritical;
  strlcpy(telem.lastFault,   lastFaultMsg.c_str(),  sizeof(telem.lastFault));
  strlcpy(telem.lastFaultAt, lastFaultTime.c_str(), sizeof(telem.lastFaultAt));
  strlcpy(telem.heldClearReason, lastHeldClearReason.c_str(), sizeof(telem.heldClearReason));
  // Fault / recovery + lockout.
  telem.held        = esp2Held;
  telem.steerRel    = steerRelease;
  telem.holdRepeats = (uint8_t)(sameHoldN > 255 ? 255 : sameHoldN);
  telem.ackLeftS    = (faultAckUntilMs && (long)(faultAckUntilMs - millis()) > 0)
                    ? (long)((faultAckUntilMs - millis()) / 1000) : 0;
  telem.actDisabled = actuationsDisabled;
  telem.exEnabled = exerciseEnabled; telem.exRunS = exerciseRunS;
  for (int c = 0; c < NUM_COLUMNS; c++) {
    telem.dayWaterL[c] = waterUsedToday[c];   telem.dayNutMl[c] = nutrientUsedToday[c];
    telem.dayWaterings[c] = wateringsToday[c]; telem.dayFeedings[c] = feedingsToday[c];
  }
  {                                             // Operation + day plan (2026-11-18)
    long today = planToday();
    telem.planYmd = today >= 0 ? currentDayStamp : -1;
    for (int c = 0; c < NUM_COLUMNS; c++) {
      telem.opManual[c]      = colOpManual[c];
      telem.opManualToday[c] = colManualToday(c) ? 1 : 0;
      if (today >= 0) planText(colPlan[c], today, telem.plan[c]);
      else            telem.plan[c][0] = '\0';
    }
  }
  for (int k = 0; k < 3; k++) {                // exerciseTick()'s own test: due once idle this long
    unsigned long since = millis() - lastPumpUseMs[k];
    telem.exNextMin[k] = since >= PUMP_EXERCISE_INTERVAL_MS ? 0 : (long)((PUMP_EXERCISE_INTERVAL_MS - since) / 60000UL);
  }
  telem.wmHeld  = webManualHeld();
  telem.wmLeftS = telem.wmHeld ? (long)((webManualUntilMs - millis()) / 1000) : 0;
  strlcpy(telem.wmState, webManualState, sizeof(telem.wmState));
  strlcpy(telem.wmWhy,   webManualWhy,   sizeof(telem.wmWhy));
  // Armed forced run: seconds resolved here, never a raw millis() deadline.
  telem.fcArmed = (fcPhase == FP_WAIT);
  telem.fcWeb   = fcFromWeb;
  telem.fcLeftS = telem.fcArmed ? (long)(((long)(fcFireAtMs - millis())) / 1000) : 0;
  if (telem.fcLeftS < 0) telem.fcLeftS = 0;
  {
    const ForceSeq &q = fcSeq[fcSlot];
    telem.fcLiters = q.dL / 10.0f;
    for (int i = 0; i < 3; i++) telem.fcDose[i] = (float)q.ml[i];
    uint8_t k = 0;
    for (int b = 0; b < NUM_COLUMNS && k < sizeof(telem.fcCols) - 1; b++)
      if (q.mask & (1 << b)) telem.fcCols[k++] = COL_TAG[b];
    telem.fcCols[k] = '\0';
  }
  // Raw sensor mirror for the web diagnostics tab (same values the LCD Sensor Diag pages show).
  telem.rTemp = sensor.rawTemp; telem.rHum = sensor.rawHum; telem.rLux = sensor.rawLux;
  telem.rResCm = sensor.rawResCm; telem.rMixCm = sensor.rawMixCm; telem.rFlow = sensor.rawFlow;
  telem.rResSt = sensor.resSt; telem.rMixSt = sensor.mixSt;
  for (int cc = 0; cc < NUM_COLUMNS; cc++) {
    telem.rSoil[cc][0] = sensor.rawSoil[cc][0]; telem.rSoil[cc][1] = sensor.rawSoil[cc][1];
    for (int r = 0; r < 7; r++) telem.rNpk[cc][r] = sensor.rawNpk[cc][r];
    telem.agNpk[cc] = sensor.msNpk[cc] ? (millis() - sensor.msNpk[cc]) : 0xFFFFFFFF;
  }
  telem.agEnv   = sensor.msEnv   ? (millis() - sensor.msEnv)   : 0xFFFFFFFF;
  telem.agTank  = sensor.msTank  ? (millis() - sensor.msTank)  : 0xFFFFFFFF;
  telem.agSoil  = sensor.msSoil  ? (millis() - sensor.msSoil)  : 0xFFFFFFFF;
  telem.agLight = sensor.msLight ? (millis() - sensor.msLight) : 0xFFFFFFFF;
  telem.e2Any = false;
  for (int k = 0; k < DIAG_ESP2_N; k++) {
    telem.e2Raw[k]   = diagEsp2Raw[k];
    telem.e2Valid[k] = diagEsp2Valid[k];
    telem.e2Age[k]   = diagEsp2Ms[k] ? (millis() - diagEsp2Ms[k]) : 0xFFFFFFFF;
    if (diagEsp2Ms[k]) telem.e2Any = true;
  }
  telem.sweepActive = (diagRemoteUntilMs != 0) || (uiMode == UI_DIAG);
  telem.sweepLeftS  = diagRemoteUntilMs ? (long)(((long)(diagRemoteUntilMs - millis())) / 1000) : 0;
  if (telem.sweepLeftS < 0) telem.sweepLeftS = 0;
  // Live run progress. Same reason as the pump flags: run[] belongs to core 1.
  telem.runActive  = (runPhase != RUN_NONE);
  telem.runFert    = run.fert;
  telem.runCol     = run.col;
  telem.runMask    = run.mask;
  telem.runOrd     = run.ord;      telem.runTotal   = run.total;
  telem.runPhaseId = (uint8_t)runPhase;
  telem.runStageL  = run.stageL;   telem.runStageTgt = run.stageTgt;
  telem.runWaterGot = run.waterGot; telem.runWaterTgt = run.waterTgt;
  for (int i = 0; i < 3; i++) { telem.runDoseTgt[i] = run.doseTgt[i]; telem.runDoseGot[i] = run.doseGot[i]; }
  memcpy(telem.runStage, run.stage, sizeof(telem.runStage));
  // Pump flags from ESP2's reported stage name, resolved here on core 1 so the uploader never
  // touches run.stage. Anything outside a live run is off, which is the truth at idle.
  {
    bool live = (runPhase == RUN_PRE || runPhase == RUN_LIVE);
    telem.pumpTransfer = live && (!strcmp(run.stage, "Transfer Water") || !strcmp(run.stage, "Flush Fill"));
    telem.pumpBooster  = live && (!strcmp(run.stage, "Release")        || !strcmp(run.stage, "Flush Release"));
    telem.pumpMixer    = live && !strcmp(run.stage, "Mixing");
  }
  // Every push request raised so far happened on this core, before this point, so this snapshot already
  // shows the events behind them (see fbPushSeq).
  telem.pushSeq = fbPushSeq;
  telem.valid = true;
  portEXIT_CRITICAL(&telemMux);
}

// Core 0: one ThingSpeak update GET. Returns the HTTP status; negative = no connection or no answer (HTTPClient's own
// codes), 0 = the URL would not parse. TS_TIMEOUT_MS each way (18 Nov 2026 c: 6 s before).
static int tsGet(const String &url) {
  HTTPClient http;
  http.setConnectTimeout(TS_TIMEOUT_MS);
  http.setTimeout(TS_TIMEOUT_MS);
  if (!http.begin(url)) return 0;
  int code = http.GET();
  http.end();
  return code;
}

// Core 0: push channel 1 (per-column moisture+NPK) and channel 2 (system) from the snapshot.
static void tsUpload() {
  TelemetrySnapshot t;
  portENTER_CRITICAL(&telemMux); t = telem; portEXIT_CRITICAL(&telemMux);
  if (!t.valid) return;

  String k1, k2, k3;
  xSemaphoreTake(netMux, portMAX_DELAY); k1 = tsKey1; k2 = tsKey2; k3 = tsKey3; xSemaphoreGive(netMux);
  // down (18 Nov 2026 c): ThingSpeak did not answer at all, so the channels after it are skipped this round instead of
  // each waiting out its own timeouts -- up to 36 s with netTask blocked and no live report going up.
  bool ok = false, down = false;

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
    int code = tsGet(url);
    ok = (code == 200) || ok;
    down = code < 0;
  }
  if (k2.length() && !down) {                         // Channel 2: system
    String url = "http://" + String(TS_HOST) + "/update?api_key=" + k2;
    // Audit fix (2026-09-09): this upload previously wrote every field unconditionally, unlike the
    // Firebase live snapshot which gates each on its own validity flag (envValid/tankValid/
    // lightValid/battVCalValid). A field simply omitted from the URL is left untouched on the
    // ThingSpeak channel for that update (fields are addressed by name, not position), matching
    // Firebase's "absent, not fabricated" behavior instead of graphing a placeholder/stale number
    // as if it were real -- battV especially, which ships with an explicit placeholder polynomial
    // until the bench calibration is actually performed.
    if (t.envValid)   { url += "&field1=" + String(t.temp, 1); url += "&field2=" + String(t.hum, 1); }
    if (t.lightValid) url += "&field3=" + String(t.lux, 0);
    if (t.tankValid)  { url += "&field4=" + String(t.resLevel, 1); url += "&field5=" + String(t.mixLevel, 1); url += "&field8=" + String(t.flow, 1); }
    if (t.battVCalValid) url += "&field6=" + String(t.battV, 2);
    if (t.battVCalValid && t.battIOk_) url += "&field7=" + String(t.battP, 1);   // power needs both V and I valid
    int code = tsGet(url);
    ok = (code == 200) || ok;
    down = code < 0;
  }
  if (k3.length() && !down) {                         // Channel 3: pH + solar + battery current (EC dropped)
    String url = "http://" + String(TS_HOST) + "/update?api_key=" + k3;
    int fld = 1;
    // 2026-11-15 (user): soil EC is no longer sent to ThingSpeak -- its fields carry SOLAR instead. Each
    // enabled column still owns a field PAIR, but only the pair's second field (pH) is written here; the
    // first (old EC) fields 1 and 3 take the solar current and today's solar energy below. Soil EC itself
    // is still read, logged on the SD card and published to Firebase -- only this upload dropped it.
    for (int c = 0; c < NUM_COLUMNS && fld <= 8; c++) {
      if (!COLUMN_ENABLED[c]) continue;
      if (t.npkValid[c] && fld + 1 <= 8) url += "&field" + String(fld + 1) + "=" + String(t.npkPH[c], 2);
      fld += 2;
    }
    // Battery current rides Ch3 because Ch2 "System" has all 8 fields used. With EC gone, column C's
    // pair (5/6) only writes pH on 6, so enabling C no longer collides with this field 5.
    if (t.battIOk_) url += "&field" + String(TS3_CURRENT_FIELD) + "=" + String(t.battI, 3);
    if (t.solarOk_) {
      url += "&field" + String(TS3_SOLAR_I_FIELD)  + "=" + String(t.solarI_, 3);      // was column A EC
      url += "&field" + String(TS3_SOLAR_WH_FIELD) + "=" + String(t.chargedWh_, 1);   // was column B EC
      url += "&field" + String(TS3_SOLAR_V_FIELD)  + "=" + String(t.solarV_, 2);
      url += "&field" + String(TS3_SOLAR_W_FIELD)  + "=" + String(t.solarP_, 1);
    }
    ok = (tsGet(url) == 200) || ok;
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
// The host the open TLS session belongs to ("" = none). HTTPClient switches hosts by closing the session, so a request to
// another host (the token renewal at securetoken.googleapis.com) means a new handshake for the next RTDB request too.
static String fbSessionHost;
static String fbHostOf(const String &url) {
  int a = url.indexOf("://"); a = (a < 0) ? 0 : a + 3;
  int b = url.indexOf('/', a);
  return url.substring(a, b < 0 ? url.length() : b);
}
// True when a request to url needs a NEW TLS session: none is open, or the open one is to another host.
static bool fbNeedsHandshake(const String &url) { return !fbClient.connected() || fbHostOf(url) != fbSessionHost; }

// Force the next request to re-apply the trust policy (and drop any open socket), so toggling
// pinning takes effect immediately instead of at the next reboot.
static void fbResetTls() {
  fbHttp.end();
  fbClient.stop();
  fbTlsReady = false;
  fbSessionHost = "";
}

// Every Firebase HTTP call goes through here so the handshake counter and timeouts stay honest.
static bool fbBegin(const String &url) {
  fbApplyTls();
  if (fbNeedsHandshake(url)) {                      // a fresh TCP+TLS session is about to happen
    // Not without the memory it needs (18 Nov 2026 h): it would fail with http=-1 anyway, after fragmenting the heap.
    if (heap8Block() < FB_TLS_MIN_BLOCK || heap8Free() < FB_TLS_MIN_FREE) { fbTlsSkips++; return false; }
    fbHandshakes++;
  }
  fbSessionHost = fbHostOf(url);
  fbHttp.setConnectTimeout(FB_CONNECT_TIMEOUT_MS);
  fbHttp.setTimeout(FB_REPLY_TIMEOUT_MS);              // 6 s before 18 Nov 2026 f (LINK PACE table)
  return fbHttp.begin(fbClient, url);
}

// Single seam for the RTDB path + auth. Every request carries the device account's ID token;
// lock the RTDB rules to that UID or the token buys you nothing.
static String fbBuildUrl(const String &base, const char *path, const char *suffix = nullptr) {
  // netMux: fbIdToken is also written from core-1's FBASE,SIGNOUT SMS handler -- the same
  // cross-core String race already guarded at the FBASE,STATUS handler and fbEnsureToken(), just
  // not previously extended to this read site (audit fix, 2026-09-08).
  String tok;
  xSemaphoreTake(netMux, portMAX_DELAY); tok = fbIdToken; xSemaphoreGive(netMux);
  // One allocation (18 Nov 2026 c): with the ~1 KB token, `base + path + "?auth=" + tok` regrew the String several
  // times per request, five or six requests a second at the live pace -- churn that fragments the heap.
  String url;
  url.reserve(base.length() + strlen(path) + 6 + tok.length() + (suffix ? strlen(suffix) : 0));
  url = base; url += path; url += "?auth="; url += tok;
  if (suffix) url += suffix;                       // "&print=silent" etc., inside the same block (18 Nov 2026 h)
  return url;
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
  // netMux: fbAuthErr is read (buffer content, not just .length()) from core-1's FBASE,STATUS
  // handler -- an unlocked write here raced that read (use-after-free/torn-read hazard, not just
  // a stale value).
  xSemaphoreTake(netMux, portMAX_DELAY);
  fbAuthErr = reason + hint;
  xSemaphoreGive(netMux);
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
  // netMux covers the compare-and-set together, same reasoning as fbNoteAuthError above.
  xSemaphoreTake(netMux, portMAX_DELAY);
  bool same = (fbAuthErr == why);
  if (!same) fbAuthErr = why;
  xSemaphoreGive(netMux);
  if (same) return;
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
  fbAuthErr = "";
  xSemaphoreGive(netMux);
  // Renew 5 min early so a slow link never uses an expired token mid-request.
  fbTokenExpiryMs = millis() + ((expSec > 300 ? expSec - 300 : 60) * 1000UL);
  // ...but it keeps working until 1 min before it really expires, so a renewal that fails in those 5 minutes no longer
  // stops the reports (18 Nov 2026 c, fbEnsureToken).
  fbTokenUsableMs = nzMs(millis() + ((expSec > 120 ? expSec - 60 : expSec / 2) * 1000UL));
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
  int code = fbHttp.POST((uint8_t *)body.c_str(), body.length());     // by pointer (18 Nov 2026 h)
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
  if (uid.length()) { xSemaphoreTake(netMux, portMAX_DELAY); fbUid = uid; xSemaphoreGive(netMux); }
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
  int code = fbHttp.POST((uint8_t *)body.c_str(), body.length());     // by pointer (18 Nov 2026 h)
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
  if (uid.length()) { xSemaphoreTake(netMux, portMAX_DELAY); fbUid = uid; xSemaphoreGive(netMux); }
  return fbStoreToken(r["idToken"] | "", r["refreshToken"] | "", (r["expiresIn"] | String("3600")).toInt());
}

// Core 0: guarantee a usable ID token, or fail fast under backoff. Called before every request.
static bool fbEnsureToken() {
  // netMux: fbIdToken is also cleared from core-1's FBASE,SIGNOUT SMS handler (audit fix, 2026-09-08).
  String curTok; xSemaphoreTake(netMux, portMAX_DELAY); curTok = fbIdToken; xSemaphoreGive(netMux);
  if (curTok.length() && (long)(millis() - fbTokenExpiryMs) < 0) return true;   // still valid
  // Due for renewal, but still accepted by Firebase until fbTokenUsableMs (18 Nov 2026 c). While that holds, a renewal
  // that fails or waits out its backoff does not stop the live report and the command polls: they keep the old token.
  bool usable = curTok.length() && fbTokenUsableMs && (long)(millis() - fbTokenUsableMs) < 0;
  if (fbAuthNextTryMs && (long)(millis() - fbAuthNextTryMs) < 0) return usable;    // backing off

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

  // refused (18 Nov 2026 c): Google answered and said no (any HTTP status, or a 200 that would not parse). Only that is a
  // credential problem; a request that never got an answer (fbAuthHttp <= 0) is the network.
  bool ok = false, tried = false, refused = false;
  // Prefer the refresh token; only fall back to the password when provisioning or when the
  // refresh token has been revoked/expired (a forced sign-in from the portal also lands here).
  if (!force && refresh.length()) { tried = true; ok = fbRefreshToken(apiKey, refresh); refused = !ok && fbAuthHttp > 0; }
  // The password only when Google REFUSED the refresh token (or there is none, or a forced sign-in): a refresh that never
  // reached Google would fail the same way with the password, one more handshake for nothing (18 Nov 2026 c).
  if (!ok && email.length() && pass.length() && (force || !refresh.length() || refused)) {
    tried = true;
    ok = fbPasswordSignIn(apiKey, email, pass);
    if (!ok && fbAuthHttp > 0) refused = true;
    if (ok) fbCredsPersistPending = true;                                          // persist the new refresh token
  }
  if (!ok) {
    fbAuthOk = false;
    // Nothing was even attempted: the key is present but there is no password and no refresh
    // token, so there is no credential to exchange. Previously this fell straight into the
    // backoff and reported nothing at all.
    if (!tried) fbNoteAuthLocal("have API key but no password and no refresh token");
    if (!usable) { xSemaphoreTake(netMux, portMAX_DELAY); fbIdToken = ""; xSemaphoreGive(netMux); } // don't reuse a dead token
    if (tried && !refused) {
      // No request got an answer from Google: the network, not the credentials. Soon again, no escalation (18 Nov 2026 c).
      fbAuthNextTryMs = nzMs(millis() + FB_AUTH_NET_RETRY_MS);
    } else {
      // Escalate: 30 s -> 2 m -> 8 m -> 30 m. Retrying a wrong password every poll is
      // credential hammering and Google will rate-limit or lock the account.
      fbAuthNextTryMs = millis() + FB_AUTH_BACKOFF[fbAuthFails < 4 ? fbAuthFails : 3];
      if (fbAuthFails < 4) fbAuthFails++;
    }
    return usable;
  }
  return ok;
}

// Text for an SD log line (18 Nov 2026 g): printable ASCII only, line breaks and tabs as single spaces. Core 0.
static void fbCopyPrintable(char *dst, size_t cap, const String &src) {
  size_t j = 0;
  for (size_t i = 0; i < src.length() && j + 1 < cap; i++) {
    char c = src[i];
    if (c == 0x0A || c == 0x0D || c == 0x09) c = ' ';   // line feed, carriage return, tab
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) c = '?';
    if (c == ' ' && (j == 0 || dst[j - 1] == ' ')) continue;   // no leading or repeated spaces
    dst[j++] = c;
  }
  dst[j] = 0;
}

// Core 0: publish the latest verified snapshot. Reads ONLY from the telem snapshot (never core-1's
// `sensor`/`wo`/`sysState`) so the dashboard can't show a torn mix of old and new readings.
// Called only through firebaseUploadLive() below, which counts it as finished (fbUploadDone).
static bool firebaseUploadLiveOnce() {
  // Stamp the attempt FIRST, unconditionally: every early return below still consumes this tick.
  // Stamping only on success would leave the netTask cadence gate permanently open and retry the
  // whole path every 500 ms loop iteration (worst at boot, before the first telemetryCollect()).
  fbLastUploadMs = millis();

  String base;
  bool enabled;
  // The diagnostics node reports whether each service is CONFIGURED, never the credential itself.
  // Derived here, under the lock that owns those strings -- not read bare from the payload builder.
  bool credsCfg, tsCfg, supaCfg;
  // signedIn/authErr: also read here (audit fix, 2026-09-08) -- fbIdToken/fbAuthErr are written
  // from core-1's FBASE,SIGNOUT SMS handler, so the two unguarded reads further below (previously
  // bare fbIdToken.length()/fbAuthErr.length()) were the same cross-core String race already fixed
  // elsewhere in this file.
  bool signedIn; String authErr;
  xSemaphoreTake(netMux, portMAX_DELAY);
  base = fbUrl; enabled = firebaseEnabled;
  credsCfg = fbApiKey.length() > 0 && (fbRefresh.length() > 0 || fbPassword.length() > 0);
  tsCfg    = tsKey1.length() > 0 || tsKey2.length() > 0 || tsKey3.length() > 0;
  supaCfg  = supaUrl.length() > 0 && supaKey.length() > 0;
  signedIn = fbIdToken.length() > 0;
  authErr  = fbAuthErr;
  xSemaphoreGive(netMux);
  if (!enabled || base.length() < 8) { fbLastOk = false; fbLastSkip = "off"; return false; }

  TelemetrySnapshot t;
  portENTER_CRITICAL(&telemMux); t = telem; portEXIT_CRITICAL(&telemMux);
  if (!t.valid || WiFi.status() != WL_CONNECTED) { fbLastOk = false; fbLastSkip = t.valid ? "wifi" : "no-snapshot"; return false; }

  // An mbedTLS session needs ~40-50 KB. Skipping a dashboard tick is much cheaper than a failed
  // alloc mid-handshake, and fbLastHeap makes the long-run fragmentation trend visible on PAGE_GSM.
  // 18 Nov 2026 (c): that floor only before a NEW session; an open one needs room for this report alone. connected()
  // first: it frees a session the server has closed, so the heap read after it is the one a handshake would get.
  // 18 Nov 2026 (h): byte-addressable memory (see MEMORY for the network). With a session open to the database only the
  // report's text has to fit; without one the handshake must fit as well, next to that text -- otherwise the report waits
  // and the next command poll (no body) opens the session for it.
  bool tls = fbNeedsHandshake(base);
  uint32_t free8 = heap8Free(), block8 = heap8Block();
  fbLastHeap = free8; fbLastBlock = block8;
  bool roomy = tls ? (block8 >= FB_TLS_MIN_BLOCK + FB_REPORT_MIN_BLOCK && free8 >= FB_TLS_MIN_FREE + FB_REPORT_MIN_BLOCK)
                   : (block8 >= FB_REPORT_MIN_BLOCK);
  if (!roomy) { fbLastOk = false; fbLastSkip = "memory"; return false; }

  if (!fbEnsureToken()) { fbLastOk = false; fbLastSkip = "login"; return false; }   // no token -> no request (respects backoff)
  fbLastSkip = "";

  // 10240 B: zones + run node + the diagnostics tree + fault/forceArmed + the raw sensor block
  // (1536 -> 2048 for powerSource/battery/chemistry/actuators, -> 6144 for diagnostics, -> 10240 for
  // the raw NPK registers, which are 7 values per enabled column). Overflow is CHECKED below, not
  // assumed: an over-capacity document serializes silently TRUNCATED, which would PUT malformed
  // JSON.
  //
  // STATIC, not a stack local: at 10 KB this would eat a third of netTask's 28 KB stack, which it
  // shares with the mbedTLS handshake frames -- the deepest consumer here and the one that would
  // fail first, mid-TLS, in a way that looks like a network fault rather than a stack overflow.
  // In .bss it costs the same RAM without competing for stack, and re-entrancy is not a concern:
  // this function is only ever called from netTask.
  // 12 KB (16 KB until 18 Nov 2026 h: the 4 KB goes back to the heap). A full report with all 12 events uses ~9 KB.
  static StaticJsonDocument<12288> doc;          // margin for the event ring, which is fitted to
  doc.clear();                                   // whatever is left (see recentEvents below)
  JsonObject meta = doc.createNestedObject("meta");
  meta.createNestedObject("updatedAt")[".sv"] = "timestamp";   // server clock; millis() is not wall time
  meta["deviceOnline"] = true;
  // Previous tick's pool usage against capacity. The payload has grown steadily and an overflow
  // aborts the ENTIRE PUT (reported as http=-100, which reads like a network fault), so make the
  // headroom a number you can watch rather than a cliff you discover. Previous tick, because
  // measuring the current one would change what it measures.
  static size_t lastDocUsed = 0;
  meta["docUsed"] = (uint32_t)lastDocUsed;
  meta["docCapacity"] = (uint32_t)doc.capacity();
  // Publish the cadence so the dashboard can compute its own staleness threshold and grey out the
  // control buttons when this snapshot goes cold. With remote e-stop in scope, a page that cannot
  // tell "idle" from "device offline" is the dangerous failure mode.
  // t.state, not sysState: this runs on core 0 and the snapshot is the only sanctioned view of
  // core-1 state. The enum read happened to be atomic, but mixing the two invites the next field
  // added here to be one that is not.
  // LINK PACE (18 Nov 2026 e): the pace of the mode ESP1 is in, and the mode itself ("idle" / "watched" / "busy").
  // The website derives its "late" / "lost" thresholds from refreshMs, so a pace change here needs no website change.
  // meta.live (the 1 s realtime flag of 17-18 Nov) is no longer sent.
  LinkMode lm = linkModeNow();
  meta["refreshMs"] = linkPaceMs(lm);
  meta["linkMode"]  = linkModeName(lm);

  JsonObject system = doc.createNestedObject("system");
  system["state"] = stateLabel(t.state, t.actDisabled);
  system["firmware"] = "web v.4 (2026-11-18h)";          // the dashboard names the firmware it is talking to
  // 18 Nov 2026 (website parity): this boot's reset reason, starts today, seconds since this start, and the two
  // watering thresholds, which the website could only ask for before.
  system["bootReason"]   = bootReasonStr;
  system["bootsToday"]   = bootsToday;
  system["uptimeS"]      = (uint32_t)(millis() / 1000UL);
  system["soilStartPct"] = soilStartPct;
  system["fertGap"]      = fertGap;
  // 18 Nov 2026: the default Too wet line and the reservoir pause (each column's own lines are under its zone).
  system["soilWetPct"]   = soilWetPct;
  system["resPausePct"]  = (int)resPauseLevel();
  system["masterWorkOrderActive"] = t.woActive;
  // The dashboard has a Power Source tile; without this key it reads "--". The rig runs from the
  // battery bank, with the inverter raised only for AC pumps during a run.
  system["powerSource"] = t.woActive ? "Battery + inverter (run)" : "Battery";
  // Which battery the boards are on right now (ESP2's INFO,BATT reports) and whether the switch is live.
  system["boardSupply"] = t.onSecondary_ ? "secondary" : "main";
  system["secondaryBattSwitching"] = (bool)SECONDARY_BATT_ENABLED;
  system["battSwitchFault"]   = t.bswLatched_;
  system["battSwitchStrikes"] = t.bswStrikes_;
  // The AC supply window ESP2's PZEM check enforces (System tab, 2026-11-14), and the last AC voltage it
  // measured, with its age at this snapshot. No reading since ESP1 started = the two last* keys are absent.
  system["acVoltMin"] = t.acVMin_;
  system["acVoltMax"] = t.acVMax_;
  if (t.acLastV_ > 0.0f) {
    system["acVoltLast"]      = t.acLastV_;
    system["acVoltLastAgeMs"] = t.acLastAgeMs_;
  }
  // The AC pump current limits (System tab, 2026-11-16) in whole tenths of an amp, as stored (5 = 0.5 A): a float
  // here would print as 0.300000012. The last AC pump current measured, with its age, only once there is one.
  system["acAmpMin10"] = t.acAMin10_;
  system["acAmpMax10"] = t.acAMax10_;
  if (t.acLastI_ >= 0.0f) {
    system["acAmpLast"]      = t.acLastI_;
    system["acAmpLastAgeMs"] = t.acLastIAgeMs_;
  }
  // The pump selector's side as the Nano last confirmed it (Manual/Test switch, 2026-11-16). Absent = never
  // confirmed; no age = confirmed before ESP1's last restart (kept in NVS).
  if (t.servoSide_ != SERVO_SIDE_NONE) {
    system["servoSide"] = (t.servoSide_ == SERVO_SIDE_TRANSFER) ? "TRANSFER" : "BOOSTER";
    if (t.servoSideAgeMs_ != 0xFFFFFFFF) system["servoSideAgeMs"] = t.servoSideAgeMs_;
  }
  system["backupEnabled"]     = (bool)SECONDARY_BACKUP_ENABLED;
  system["backupState"]       = bkName((BkState)t.bkState_);
  // For the new Set Clock control: show the operator what ESP1 currently reads before they decide to
  // change it. From the snapshot, never tsString() here: that is an I2C read, and Wire hands its receive
  // buffer back unlocked, so a read from this core can overwrite a sensor reading in progress on core 1.
  system["rtcOk"] = rtcOk;
  system["deviceTime"] = t.deviceTime;

  JsonObject sensors = doc.createNestedObject("sensors");
  if (t.tankValid)  { sensors["reservoirLevel"] = t.resLevel; sensors["mixingLevel"] = t.mixLevel; sensors["flowRate"] = t.flow; }
  // web v.4: the litres each tank holds, from the tank calibration (absent when the reading is not usable).
  if (t.resL >= 0.0f) sensors["reservoirLiters"] = roundf(t.resL * 100.0f) / 100.0f;
  if (t.mixL >= 0.0f) sensors["mixingLiters"]    = roundf(t.mixL * 100.0f) / 100.0f;
  if (t.envValid)   { sensors["temperature"] = t.temp; sensors["humidity"] = t.hum; }
  if (t.lightValid) sensors["lightLevel"] = t.lux;
  // Voltage/percent only publish from the main INA226 or a REAL opto calibration -- otherwise there is
  // no honest way to tell "sensor not installed" from "installed but uncalibrated, reading noise", so
  // this correctly shows Unavailable rather than a meaningless polynomial-of-noise number. Current is
  // gated on battIOk_: the source in use answered on I2C just now.
  if (t.battVCalValid) {
    sensors["batteryVoltage"] = t.battV;
    // Percent from resting voltage. BATT_LOW_V/BATT_CRIT_V are alarm points, not endpoints, so the
    // scale uses its own full/empty pair. Clamped, and only published when the reading is valid --
    // a made-up 0 % would look like a flat battery rather than a missing sensor.
    float pct = (t.battV - BATT_EMPTY_V) * 100.0f / (BATT_FULL_V - BATT_EMPTY_V);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    sensors["batteryPercent"] = (int)(pct + 0.5f);
  }
  if (t.battIOk_) {
    sensors["batteryCurrent"] = t.battI;                 // magnitude (see readBatteryAdc)
    if (t.battFromIna_) sensors["batteryCurrentSigned"] = t.battIsigned_;   // + = discharging; shunt bar only
  }
  // Solar INA226. Absent, not fabricated, while it is not answering. 0 V at night is a real reading.
  if (t.solarOk_) {
    sensors["solarVoltage"] = t.solarV_;
    sensors["solarCurrent"] = t.solarI_;
    sensors["solarPower"]   = t.solarP_;
    sensors["solarEnergyTodayWh"] = (int)(t.chargedWh_ + 0.5);
  }
  // Secondary battery (INA226). Absent, not fabricated, while the monitor is not answering.
  if (t.secOk_) {
    sensors["secondaryBatteryVoltage"] = t.secV_;
    sensors["secondaryBatteryCurrent"] = t.secI_;        // signed: + = discharging (SEC_INVERT_CURRENT)
    sensors["secondaryBatteryPower"]   = t.secP_;
    // The RESTING charge (secRestV), not the raw voltage: the charger lifts it, the load pulls it down (2026-11-17).
    float sp = (t.secRest_ - SEC_BATT_EMPTY_V) * 100.0f / (SEC_BATT_FULL_V - SEC_BATT_EMPTY_V);
    if (sp < 0) sp = 0;
    if (sp > 100) sp = 100;
    sensors["secondaryBatteryPercent"] = (int)(sp + 0.5f);
    sensors["secondaryBatteryLow"]     = t.secLow_;
  }
  // Mixing-tank chemistry from ESP2's TELE. Only ever measured during a run, so this is the last
  // batch -- published only if we have actually seen a value, never as a placeholder zero.
  if (t.tankEC >= 0) sensors["waterEC"] = t.tankEC;
  if (t.tankPH >= 0) sensors["waterPH"] = t.tankPH;

  // Which pumps are energised right now, derived on core 1 from the stage ESP2 reports
  // (STAGE,<ord>,<n>,<name>) -- no extra ESP2 traffic needed.
  JsonObject act = doc.createNestedObject("actuators");
  act["transferRunning"] = t.pumpTransfer;
  act["boosterRunning"]  = t.pumpBooster;
  act["mixerRunning"]    = t.pumpMixer;

  /* ---- diagnostics ------------------------------------------------------------------------
   * The dashboard's Diagnostics tab and Run-progress panel read everything below. None of it was
   * ever published, which is why that whole tab said "no diagnostics have been published by ESP1".
   * All values come from the snapshot (core 1), never from live globals. */
  JsonObject dg = doc.createNestedObject("diagnostics");

  JsonObject dnet = dg.createNestedObject("network");
  dnet["wifiEnabled"]   = (WiFi.getMode() & WIFI_MODE_STA) != 0;
  dnet["wifiConnected"] = WiFi.status() == WL_CONNECTED;
  dnet["wifiRssi"]      = WiFi.RSSI();

  JsonObject dfb = dg.createNestedObject("firebase");
  dfb["enabled"]        = enabled;
  dfb["urlConfigured"]  = base.length() >= 8;
  dfb["deviceCredentialsConfigured"] = credsCfg;
  dfb["signedIn"]       = signedIn;
  dfb["attempts"]       = fbAttempts;
  dfb["lastUploadOk"]   = fbLastOk;
  dfb["lastHttp"]       = fbLastHttp;
  // Link health (18 Nov 2026 c), for the website's Diagnostics: failed reports and new TLS sessions since this start (a
  // steady link keeps both low), and the free heap and largest free block just before this report.
  dfb["failures"]       = fbFailures;
  dfb["handshakes"]     = fbHandshakes;
  dfb["freeHeap"]       = fbLastHeap;              // byte-addressable (18 Nov 2026 h; INTERNAL before, ~40 KB too high)
  dfb["maxAllocHeap"]   = fbLastBlock;             // its largest block (18 Nov h; was the IRAM block, always 40,948)
  dfb["tlsSkips"]       = fbTlsSkips;              // new sessions not tried for want of memory (18 Nov 2026 h)
  dfb["linkResets"]     = fbLinkResets;            // fresh TLS sessions forced after failed reports (18 Nov 2026 e)
  dfb["wifiRejoins"]    = fbWifiRejoins;           // WiFi left and joined again after more of them
  dfb["textFixes"]      = fbTextFixes;             // reports whose text was cleaned before sending (18 Nov 2026 g)
  dfb["tlsValidationEnabled"] = fbPinCa;
  if (authErr.length()) dfb["lastAuthError"] = authErr;   // Google's reason string, never a secret

  JsonObject dts = dg.createNestedObject("thingspeak");
  dts["configured"]   = tsCfg;
  dts["attempted"]    = lastTsUploadMs != 0;
  dts["lastUploadOk"] = lastTsOk;

  JsonObject dlu = dg.createNestedObject("logUpload");     // 2026-11-17: the CSV logs go to Firebase
  dlu["target"]          = (firebaseEnabled && fbUrl.length() >= 8) ? "firebase" : (supaCfg ? "supabase" : "none");
  dlu["busy"]            = uploadBusy;
  dlu["lastUploadedDay"] = fbLogLast;
  dlu["keepFromDay"]     = (long)logKeepFrom;

  JsonObject dsb = dg.createNestedObject("supabase");
  dsb["configured"]          = supaCfg;
  dsb["uploadBusy"]          = uploadBusy;
  dsb["lastUploadAttempted"] = supaLast != 0;
  dsb["lastUploadedDay"]     = supaLast;

  JsonObject dsy = dg.createNestedObject("system");
  dsy["workOrderActive"] = t.woActive;
  dsy["pendingRun"]      = t.pendRun;
  if (t.lastFault[0])   dsy["lastFault"]     = t.lastFault;
  if (t.lastFaultAt[0]) dsy["lastFaultTime"] = t.lastFaultAt;
  // Lock Screen status (Part 2, item M). The website shows it, and since 2026-11-05 can set it with LCD_LOCK.
  dsy["lcdLocked"] = lcdLocked;

  // Tank check (18 Nov 2026): scheduled runs wait while it is needed; the website shows it and can confirm it
  // (TANK_CHECK_OK), as SMS TANKCHECK,OK and the LCD row do. Read across cores like lcdLocked: a bool and a short
  // string that change about twice a day.
  JsonObject dtk = dg.createNestedObject("tankCheck");
  dtk["needed"] = (bool)tankCheckNeeded;
  if (tankCheckNeeded) dtk["why"] = tankCheckWhy[0] ? tankCheckWhy : "CARRIED_OVER_RESTART";

  // Calibration, read-only for the website (18 Nov 2026): what the LCD's Calibration menu has set. Only real numbers
  // go in: a NaN would serialize as bare NaN and Firebase would refuse the whole report.
  JsonObject dca = dg.createNestedObject("calibration");
  auto putNum = [&](JsonObject o, const char *k, float v) { if (!isnan(v) && !isinf(v)) o[k] = v; };
  JsonObject dcr = dca.createNestedObject("rates");                  // 0 = not measured: the default lines
  putNum(dcr, "transferLpm", calRateXfer);  putNum(dcr, "boosterLpm", calRateBoost);
  putNum(dcr, "transferStartL", calXferStartL); putNum(dcr, "boosterStartL", calBoostStartL);
  JsonArray dcn = dcr.createNestedArray("nutMlpm");                 // Nut A/B/C pump mL/min (the dose ceilings)
  for (int i = 0; i < 3; i++) dcn.add(PUMP_FLOWRATE_MLPM[i]);
  JsonArray dcs = dcr.createNestedArray("nutMeasured");
  for (int i = 0; i < 3; i++) dcs.add(pumpRateSet[i]);
  JsonObject dck = dca.createNestedObject("flowK");                  // flow meters, pulses per litre
  putNum(dck, "nutA", calKNut[0]); putNum(dck, "nutB", calKNut[1]); putNum(dck, "nutC", calKNut[2]);
  putNum(dck, "nutD", calKNutD);   putNum(dck, "phUp", calKPhUp);   putNum(dck, "phDn", calKPhDn);
  putNum(dck, "reservoirScale", calFlowResScale);
  JsonObject dct = dca.createNestedObject("tanks");                  // ultrasonic: sensor to water at empty / full
  putNum(dct, "resEmptyCm", calResEmptyCm); putNum(dct, "resFullCm", calResFullCm);
  putNum(dct, "mixEmptyCm", calMixEmptyCm); putNum(dct, "mixFullCm", calMixFullCm);
  JsonObject dcp = dca.createNestedObject("probes");
  putNum(dcp, "phM", calPhM); putNum(dcp, "phB", calPhB); putNum(dcp, "ecM", calEcM); putNum(dcp, "ecB", calEcB);
  putNum(dcp, "acs712Zero", calAcs712Zero);
  putNum(dcp, "tempOff", calTempOff); putNum(dcp, "humOff", calHumOff); putNum(dcp, "luxOff", calLuxOff);

  JsonObject de2 = dg.createNestedObject("esp2");
  de2["available"]         = t.esp2Avail;
  de2["powered"]           = t.esp2Pwr;
  de2["communicationLost"] = t.esp2Lost;
  de2["lastResponseAgeMs"] = t.esp2AgeMs;

  JsonObject dna = dg.createNestedObject("nano");
  dna["lastSampleAgeMs"]  = t.nanoAgeMs;
  dna["environmentValid"] = t.envValid;
  dna["tankValid"]        = t.tankValid;
  dna["lightValid"]       = t.lightValid;

  JsonObject dpe = dg.createNestedObject("peripherals");
  dpe["rtcOk"] = t.rtcOk_; dpe["sdOk"] = t.sdOk_; dpe["batterySensorOk"] = t.battIOk_;
  dpe["batteryAdsOk"] = t.acsOk_;                  // the ACS758 fallback, separately: useful while INA226 is in use
  dpe["mainInaOk"] = t.battFromIna_;
  dpe["solarSensorOk"] = t.solarOk_;
  dpe["secondaryBatterySensorOk"] = t.secOk_;

  JsonObject dgs = dg.createNestedObject("gsm");
  dgs["simReady"]          = t.simOk;
  dgs["networkRegistered"] = t.netReg;
  dgs["rssi"]              = t.rssi;
  dgs["creg"]              = t.creg;
  dgs["lastHealthAgeMs"]   = t.gsmHealthAgeMs;

  JsonObject dpw = dg.createNestedObject("power");
  dpw["batteryLow"]      = t.battLow;
  dpw["batteryCritical"] = t.battCrit;
  dpw["batteryCurrent"]  = t.battI;
  if (t.battVCalValid) dpw["batteryPower"] = t.battP;   // needs real volts, same gate as the ThingSpeak field

  // Fault + recovery. The dashboard renders its prompt from this and offers exactly the choices the
  // LCD menu does, so the two surfaces cannot disagree about what is available.
  JsonObject dfa = dg.createNestedObject("fault");
  dfa["held"]  = t.held;
  dfa["state"] = stateLabel(t.state, t.actDisabled);
  if (t.lastFault[0])   dfa["code"] = t.lastFault;
  if (t.lastFaultAt[0]) dfa["at"]   = t.lastFaultAt;
  if (t.heldClearReason[0]) dfa["lastClearReason"] = t.heldClearReason;
  if (t.ackLeftS > 0)   dfa["ackSecondsLeft"] = t.ackLeftS;   // "do nothing" snooze still running
  if (t.held) {
    // Surface the re-hold guard: after 3 identical FLOW_FAIL holds the firmware steers toward
    // Release, and self-cancels the run at the 4th. A prompt that hid that would keep offering
    // "Resume normal" against dead hardware.
    dfa["repeats"]      = t.holdRepeats;
    dfa["steerRelease"] = t.steerRel;
    dfa["autoCancelAt"] = HOLD_AUTOCANCEL_N;
    JsonArray ra = dfa.createNestedArray("recovery");
    ra.add("hold"); ra.add("release"); ra.add("irrigate"); ra.add("normal");
  }
  dg["actuationsDisabled"] = t.actDisabled;
  JsonObject dwm = dg.createNestedObject("webManual");
  dwm["state"] = t.wmState;
  dwm["held"]  = t.wmHeld;
  if (t.wmLeftS > 0) dwm["secondsLeft"] = t.wmLeftS;
  if (t.wmWhy[0])    dwm["reason"] = t.wmWhy;
  JsonObject dex = dg.createNestedObject("pumpExercise");
  dex["enabled"] = t.exEnabled;
  dex["seconds"] = t.exRunS;
  dex["intervalHours"] = (int)(PUMP_EXERCISE_INTERVAL_MS / 3600000UL);
  // Minutes until each pump's exercise falls due (transfer, booster, mixer), counted from its last real run or, after
  // a restart, from the restart: the clock is kept in RAM (2026-11-06). 0 = due now, done once the rig is idle.
  JsonArray dnx = dex.createNestedArray("nextDueMin");
  for (int k = 0; k < 3; k++) dnx.add(t.exNextMin[k]);

  // Armed forced run -- the operator's proof the rig received the request and is counting down.
  JsonObject dfc = dg.createNestedObject("forceArmed");
  dfc["armed"] = t.fcArmed;
  if (t.fcArmed) {
    dfc["secondsLeft"] = t.fcLeftS;
    dfc["columns"]     = t.fcCols;
    dfc["liters"]      = t.fcLiters;
    dfc["source"]      = t.fcWeb ? "web" : "lcd";
    JsonObject fd = dfc.createNestedObject("doseMl");
    fd["A"] = t.fcDose[0]; fd["B"] = t.fcDose[1]; fd["C"] = t.fcDose[2];
  }

  JsonObject dac = dg.createNestedObject("actuator");
  dac["relayFeedback"]   = "notReported";      // no relay sense hardware -- say so rather than imply OK
  dac["workOrderActive"] = t.woActive;
  dac["pumpTestActive"]  = t.pendEx;
  dac["pumpUnderTest"]   = (t.pendEx && t.pendExIdx >= 0 && t.pendExIdx < 3) ? EX_NAME[t.pendExIdx] : "none";

  /* Raw sensor values -- the web equivalent of the LCD Sensor Diag pages. These are PRE-conversion
   * readings with an honest -1 on fault, which is exactly what makes them useful for diagnosing a
   * sensor: a converted value can look plausible while the raw one is railed. Each group carries an
   * age so a dead branch is visible rather than showing its last good number forever. */
  JsonObject draw = dg.createNestedObject("sensorsRaw");
  JsonObject rEnv = draw.createNestedObject("env");
  rEnv["tempC"] = t.rTemp; rEnv["humidity"] = t.rHum; rEnv["ageMs"] = t.agEnv;
  JsonObject rLig = draw.createNestedObject("light");
  rLig["lux"] = t.rLux; rLig["ageMs"] = t.agLight;
  JsonObject rTk = draw.createNestedObject("tank");
  rTk["reservoirCm"] = t.rResCm; rTk["mixingCm"] = t.rMixCm; rTk["flowLpm"] = t.rFlow;
  // Why a tank has no reading (2026-11-17): the dashboard says "no echo" etc. instead of just "Unavailable".
  rTk["reservoirStatus"] = String(t.rResSt ? t.rResSt : '?');
  rTk["mixingStatus"]    = String(t.rMixSt ? t.rMixSt : '?');
  rTk["ageMs"] = t.agTank;
  JsonObject rSo = draw.createNestedObject("soil");
  rSo["ageMs"] = t.agSoil;
  JsonObject rNp = draw.createNestedObject("npk");
  for (int cc = 0; cc < NUM_COLUMNS; cc++) {
    if (!COLUMN_ENABLED[cc]) continue;
    String tag = String(COL_TAG[cc]);
    JsonArray sa = rSo.createNestedArray(tag);          // both probes, so a single dead probe shows
    sa.add(t.rSoil[cc][0]); sa.add(t.rSoil[cc][1]);
    JsonObject nc = rNp.createNestedObject(tag);
    JsonArray na = nc.createNestedArray("regs");        // moist,temp,EC,pH,N,P,K as read over Modbus
    for (int r = 0; r < 7; r++) na.add(t.rNpk[cc][r]);
    nc["ageMs"] = t.agNpk[cc];
  }
  // ESP2 raws only exist after a sweep (LCD Sensor Diag or a remote DIAG_SWEEP), so they are
  // omitted entirely until one has run -- an all-zero block would read as "everything is 0.00".
  JsonObject rE2 = draw.createNestedObject("esp2");
  rE2["sweepActive"] = t.sweepActive;
  if (t.sweepLeftS > 0) rE2["sweepSecondsLeft"] = t.sweepLeftS;
  if (t.e2Any) {
    JsonObject vals = rE2.createNestedObject("values");
    for (int k = 0; k < DIAG_ESP2_N; k++) {
      if (t.e2Age[k] == 0xFFFFFFFF) continue;           // never answered this sweep
      JsonObject one = vals.createNestedObject(DIAG_ESP2_ID[k]);
      one["raw"] = t.e2Raw[k]; one["valid"] = t.e2Valid[k]; one["ageMs"] = t.e2Age[k];
    }
  }

  // Live run progress. The dashboard's run panel needs stage ordinal + litres delivered/target to
  // draw a bar; publishing only "a run is active" left it blank. Always emitted (with active=false
  // at idle) so the panel can distinguish "idle" from "field missing".
  JsonObject rn = dg.createNestedObject("runProgress");
  rn["active"] = t.runActive;
  if (t.runActive) {
    rn["operation"] = t.runFert ? "fertigation" : "irrigation";
    rn["phase"]     = RUN_PHASE_NAME[t.runPhaseId < 5 ? t.runPhaseId : 0];
    {  // "A", or "A+B" for a FORCE run that waters several columns at once (2026-11-18; the page only displays it)
      int c0 = (t.runCol >= 0 && t.runCol < NUM_COLUMNS) ? t.runCol : 0;
      uint8_t m = t.runMask ? t.runMask : (uint8_t)(1 << c0);
      String z;
      for (int b = 0; b < NUM_COLUMNS; b++) if (m & (1 << b)) { if (z.length()) z += '+'; z += COL_TAG[b]; }
      rn["zone"] = z;
    }
    rn["stage"]     = t.runStage;
    rn["stageOrdinal"] = t.runOrd;
    rn["stageTotal"]   = t.runTotal;
    rn["stageLiters"]       = t.runStageL;
    rn["stageTargetLiters"] = t.runStageTgt;
    rn["waterDeliveredLiters"] = t.runWaterGot;
    rn["waterTargetLiters"]    = t.runWaterTgt;
    if (t.runFert) {
      JsonObject dm = rn.createNestedObject("dosesMl");
      for (int i = 0; i < 3; i++) {
        JsonObject d = dm.createNestedObject(String((char)('A' + i)));
        d["target"] = t.runDoseTgt[i]; d["delivered"] = t.runDoseGot[i];
      }
    }
  }

  JsonObject zones = sensors.createNestedObject("zones");
  for (int c = 0; c < NUM_COLUMNS; c++) {
    // Column Mode website parity: every column gets an entry now, even a disabled one, so the
    // dashboard can show real enabled/disabled + mode/schedule/target state for all 3 physical
    // columns (previously a disabled column had no entry at all here). Sensor readings below are
    // still gated on COLUMN_ENABLED -- unchanged behaviour for every already-published field.
    JsonObject z = zones.createNestedObject(String(COL_TAG[c]));
    z["enabled"]   = COLUMN_ENABLED[c];
    z["mode"]      = (col[c].mode == MODE_AUTO) ? "AUTO" : "IRRIGATION_ONLY";
    z["schedMode"] = colSchedMode[c];
    z["winStart"]  = (colSchedMode[c] == SCHED_MANUAL) ? COL_WIN_START[c] : DEF_WIN_START[c];
    z["winEnd"]    = (colSchedMode[c] == SCHED_MANUAL) ? COL_WIN_END[c]   : DEF_WIN_END[c];
    // Several waterings a day (2026-11-05): every window in use, how many are done today, and whether nutrients
    // already went in today. winStart/winEnd above stay window 1, for dashboards that predate this.
    z["runs"] = windowCount(c);
    JsonArray zw = z.createNestedArray("windows");
    for (int w = 0; w < windowCount(c); w++) {
      JsonArray pair = zw.createNestedArray();
      pair.add(windowStart(c, w)); pair.add(windowEnd(c, w));
    }
    z["runsToday"] = runsDoneToday(c);
    z["fedToday"]  = col[c].fertStamp == currentDayStamp;
    // Today's tally (2026-11-06): runs that reached the column (scheduled and forced), how many added nutrients, and
    // the litres and mL they delivered. Survives a restart; resets at midnight.
    JsonObject zt = z.createNestedObject("today");
    zt["waterings"] = t.dayWaterings[c];
    zt["feedings"]  = t.dayFeedings[c];
    zt["liters"]    = roundf(t.dayWaterL[c] * 100.0f) / 100.0f;
    zt["ml"]        = roundf(t.dayNutMl[c] * 10.0f) / 10.0f;
    z["targetN"]   = col[c].targetN; z["targetP"] = col[c].targetP;
    z["targetK"]   = col[c].targetK; z["targetPH"] = col[c].targetPH;
    // Operation + day plan (2026-11-18, run_plan.h): the column's default, what applies today, and the plan from
    // today (one character a day: '-' default, 'M' Manual, 'A' Automatic). A char* (not const), so it is copied.
    z["operation"]      = t.opManual[c]      ? "MANUAL" : "AUTOMATIC";
    z["operationToday"] = t.opManualToday[c] ? "MANUAL" : "AUTOMATIC";
    if (t.planYmd > 0) { z["plan"] = (char *)t.plan[c]; z["planFrom"] = t.planYmd; }
    // Why a due column may be passed over (18 Nov 2026): resting after repeated faults (minutes left), or the whole
    // day closed by a STOP reply or a lost fault. Absent / false = neither.
    if (colSnoozeUntil[c] && (long)(millis() - colSnoozeUntil[c]) < 0) z["restMin"] = (uint32_t)((colSnoozeUntil[c] - millis()) / 60000UL + 1);
    if (dayDoneToday(c)) z["skippedToday"] = true;
    // The column's watering lines as they apply (18 Nov 2026): Soil start (waters below it) and Too wet (a warning
    // above it). *Own = its own value, not the default.
    z["soilStartPct"] = colStartPct(c);
    z["soilWetPct"]   = colWetPct(c);
    if (colSoilStart[c] >= 0) z["soilStartOwn"] = true;
    if (colSoilWet[c]   >= 0) z["soilWetOwn"]   = true;
    if (!COLUMN_ENABLED[c]) continue;                   // no sensor data for a disabled column
    if (t.soil[c] >= 0) z["moisture"] = t.soil[c];
    if (t.npkValid[c]) {
      z["nitrogen"] = t.npkN[c]; z["phosphorus"] = t.npkP[c]; z["potassium"] = t.npkK[c];
      z["ec"] = t.npkEC[c]; z["ph"] = t.npkPH[c];
      // The probe's own moisture, published alongside the blended `moisture` above so the two can be
      // compared -- that difference is exactly what the divergence gate acts on.
      z["npkMoisture"] = t.npkMoist[c];
      z["soilTemperature"] = t.npkTemp[c];
    }
  }

  /* Recent events -- built LAST, and only into the capacity that is genuinely left over.
   *
   * This ordering is the point. Everything above is telemetry the dashboard needs; the log is the
   * least important thing in the payload. Appending it blindly meant a long fertigation receipt
   * could tip the document over capacity, and doc.overflowed() then aborts the ENTIRE PUT -- so the
   * log could take the sensors, the fault state and the run progress down with it, reporting
   * http=-100 that reads like a network fault. Fitting events to the remaining room means the log
   * loses its oldest rows instead, which is the right thing to sacrifice.
   *
   * Read from the ring under its own lock (it is shared state with a lock built for exactly this),
   * copied out and released before touching JSON -- building the array with interrupts disabled
   * would be far worse than the copy. Emitted oldest-first so the dashboard's reverse() puts the
   * newest at the top; trimming therefore drops the oldest, which is what you want to lose. */
  {
    static WebEvent snap[WEB_EVT_N];
    uint8_t n, head;
    portENTER_CRITICAL(&evtMux);
    n = webEvtCount; head = webEvtHead;
    memcpy(snap, webEvt, sizeof(snap));
    portEXIT_CRITICAL(&evtMux);

    // Per-entry cost: 4 members plus their copied strings, with slack for ArduinoJson's own slots.
    const size_t EVT_COST = sizeof(WebEvent) + 96;
    const size_t RESERVE  = 512;                       // leave room for serialization headroom
    size_t room = (doc.capacity() > doc.memoryUsage() + RESERVE)
                ? (doc.capacity() - doc.memoryUsage() - RESERVE) : 0;
    uint8_t fit = (uint8_t)(room / EVT_COST);
    if (fit > n) fit = n;
    if (fit) {
      JsonArray ev = dg.createNestedArray("recentEvents");
      uint8_t start = (n < WEB_EVT_N) ? 0 : head;      // not yet wrapped -> slot 0 is oldest
      for (uint8_t i = n - fit; i < n; i++) {          // keep the NEWEST `fit` entries
        const WebEvent &e = snap[(start + i) % WEB_EVT_N];
        JsonObject o = ev.createNestedObject();
        o["at"] = e.at; o["source"] = e.src; o["type"] = e.type; o["detail"] = e.detail;
      }
    }
  }

  // Past every skip condition -- this tick is a real attempt, so count it before anything that can fail.
  fbAttempts++;
  lastDocUsed = doc.memoryUsage();
  if (doc.overflowed()) { fbLastOk = false; fbLastHttp = -100; fbFailures++; return false; }  // never PUT truncated JSON
  // One block for the whole text (18 Nov 2026 c): serializing into an empty String grew it a few bytes at a time --
  // hundreds of reallocations per report, every second at the live pace, the churn that fragments a heap.
  // -101 = no free block that size (counted as a failed report, like -100 above).
  // 18 Nov 2026 (h): the text needs ONE byte-addressable block of its length; checked before asking, with 1 KB to spare
  // for the request around it. Not enough: this report waits (skip "memory", not a failure) and the next tick tries.
  size_t need = measureJson(doc) + 1;
  if (heap8Block() < need + 1024) { fbAttempts--; fbLastOk = false; fbLastSkip = "memory"; return false; }
  String payload;
  if (!payload.reserve(need)) { fbLastOk = false; fbLastHttp = -101; fbFailures++; return false; }
  serializeJson(doc, payload);

  // Text safety (18 Nov 2026 g). This report is printable ASCII by construction -- every literal in it is -- so a byte
  // under 0x20 or over 0x7E can only be line noise that reached a text field (the Nano, ESP2 and modem links all carry
  // GARBAGE on this rig). Raw, such a byte makes the JSON invalid and Firebase refuses the WHOLE report with HTTP 400,
  // every minute, for as long as the text stays. Cleaned to '?', the report goes through; the first one is logged (with
  // the JSON just before it, which names the field) at most every 10 minutes.
  {
    int firstBad = -1; uint16_t nBad = 0;
    for (size_t i = 0; i < payload.length(); i++) {
      uint8_t c = (uint8_t)payload[i];
      if (c < 0x20 || c > 0x7E) { if (firstBad < 0) firstBad = (int)i; payload.setCharAt(i, '?'); nBad++; }
    }
    if (nBad) {
      fbTextFixes++;
      static unsigned long lastTextLogMs = 0;
      if (!fbReplyLogPending && (!lastTextLogMs || millis() - lastTextLogMs > 600000UL)) {
        lastTextLogMs = nzMs(millis());
        char near[72];
        fbCopyPrintable(near, sizeof(near), payload.substring(firstBad > 64 ? firstBad - 64 : 0, firstBad + 1));
        snprintf(fbReplyLog, sizeof(fbReplyLog), "TEXT|fixed=%u|near=%s", (unsigned)nBad, near);
        fbReplyLogPending = true;
      }
    }
  }

  // print=silent (2026-11-09): without it RTDB answers a PUT by echoing the whole ~6-10 KB snapshot
  // back, and that body was never read. WiFiClientSecure::flush() is a no-op in core 2.0.17, so
  // fbHttp.end() left it sitting on the kept-alive socket, where the NEXT request (a command poll)
  // could read it as its own status line. With print=silent a success is a bodiless 204 (still
  // inside the 2xx test below), and the drain covers the small error body of a rejection.
  if (!fbBegin(fbBuildUrl(base, "/irrigation/live.json", "&print=silent"))) { fbLastOk = false; fbLastHttp = 0; fbFailures++; return false; }
  fbHttp.addHeader("Content-Type", "application/json");
  int code = fbHttp.PUT((uint8_t *)payload.c_str(), payload.length());   // by pointer: PUT(String) copied it (18 Nov h)
  String reply = (code > 0) ? fbHttp.getString() : String();   // drain: never leave a body on the reused socket
  fbHttp.end();                                            // setReuse(true): keeps the socket, frees the request
  fbLastHttp = code;
  fbLastOk = (code >= 200 && code < 300);
  if (!fbLastOk) fbFailures++;
  // Firebase refused it (an HTTP status, not a network error): keep its reason for the SD log (18 Nov 2026 g). Once per
  // status, and again at most every 10 minutes while it lasts.
  if (code > 0 && !fbLastOk) {
    static int lastReplyCode = 0;
    static unsigned long lastReplyLogMs = 0;
    if (!fbReplyLogPending && (code != lastReplyCode || millis() - lastReplyLogMs > 600000UL)) {
      lastReplyCode = code; lastReplyLogMs = millis();
      char why[112];
      fbCopyPrintable(why, sizeof(why), reply);
      snprintf(fbReplyLog, sizeof(fbReplyLog), "REPLY|http=%d|len=%u|%s", code, (unsigned)payload.length(), why);
      fbReplyLogPending = true;
    }
  }
  if (code == 401) { xSemaphoreTake(netMux, portMAX_DELAY); fbIdToken = ""; xSemaphoreGive(netMux); fbTokenExpiryMs = 0; }  // token rejected -> force a renew next tick
  return fbLastOk;
}
// Every call counts once it has FINISHED (18 Nov 2026 c), so firebaseLogTick() never reads a result still on the wire.
static bool firebaseUploadLive() {
  bool ok = firebaseUploadLiveOnce();
  fbUploadDone++;
  return ok;
}

/* =============================================================================
 *  CSV LOGS TO FIREBASE  (core 0 / netTask, 2026-11-17)  --  see LOG_CHUNK_BYTES
 * ========================================================================== */
static bool fbOk(int code) { return code >= 200 && code < 300; }
static int fbLogRequest(const String &base, const String &path, const char *method, const String &body) {
  if (!fbBegin(fbBuildUrl(base, path.c_str(), "&print=silent"))) return 0;
  int code;
  if (!strcmp(method, "PUT")) { fbHttp.addHeader("Content-Type", "application/json"); code = fbHttp.PUT((uint8_t *)body.c_str(), body.length()); }
  else code = fbHttp.sendRequest(method);
  if (code > 0) fbHttp.getString();                        // drain: never leave a body on the reused socket
  fbHttp.end();
  if (code == 401) { xSemaphoreTake(netMux, portMAX_DELAY); fbIdToken = ""; xSemaphoreGive(netMux); fbTokenExpiryMs = 0; }
  return code;
}
// A piece of the CSV as a JSON string. RTDB needs valid UTF-8 and this log is ASCII; the only other bytes in it are line
// noise logged as GARBAGE, so those become '?'.
static void fbJsonEscapeAppend(String &out, const uint8_t *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = b[i];
    if      (c == '"')  out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c < 0x20 || c >= 0x7F) out += '?';
    else out += (char)c;
  }
}
struct FbLogJob {
  bool active; uint8_t phase; long stamp; char day[9]; char path[16];
  size_t size, off; uint16_t n; bool partial; unsigned long startMs;
  uint8_t netTries;                                         // connection failures on the current step
};
const uint8_t FB_LOG_NET_TRIES = 3;                         // a dropped connection: this many fresh sockets per step
static FbLogJob fbLogJob = {};
const unsigned long FB_LOG_JOB_MAX_MS = 900000UL;          // 15 min: a stuck upload gives up (and is retried later)

static void fbLogJobFinish(bool ok, int code) {
  if (!ok) {
    const char *ph = fbLogJob.phase == 0 ? "start" : fbLogJob.phase == 1 ? "piece" : "done";
    if (fbLogJob.phase == 1) snprintf(fbLogFailStep, sizeof(fbLogFailStep), "piece%u", (unsigned)fbLogJob.n);
    else strlcpy(fbLogFailStep, ph, sizeof(fbLogFailStep));
    fbLogFailHeap = heap8Free();                   // byte-addressable (18 Nov 2026 h)
    fbLogFailBlock = heap8Block();
  }
  fbLogJob.active = false;
  uploadHttp = code;
  uploadResStamp = fbLogJob.stamp; uploadResult = ok ? 1 : 2;
  uploadReqStamp = 0; uploadBusy = false;
}
static void fbLogJobBegin(long stamp, const char *path, bool partial) {
  fbLogJob = FbLogJob();
  fbLogJob.active = true; fbLogJob.phase = 0; fbLogJob.stamp = stamp; fbLogJob.partial = partial;
  snprintf(fbLogJob.day, sizeof(fbLogJob.day), "%08ld", stamp);
  strlcpy(fbLogJob.path, path, sizeof(fbLogJob.path));
  fbLogJob.startMs = millis();
}
// Days older than logKeepFrom leave Firebase (their pieces and their index entry). A few per upload at most.
static void fbLogRetention(const String &base) {
  long keepFrom = logKeepFrom;
  if (keepFrom <= 0) return;
  if (!fbBegin(fbBuildUrl(base, "/logs/index.json", "&shallow=true"))) return;
  int code = fbHttp.GET();
  String resp = (code > 0) ? fbHttp.getString() : "";
  fbHttp.end();
  if (code != 200 || resp.length() == 0 || resp == "null") return;
  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, resp)) return;
  long old[8]; int nOld = 0;
  for (JsonPair kv : doc.as<JsonObject>()) {
    const char *k = kv.key().c_str();
    if (strlen(k) != 8) continue;
    long d = atol(k);
    if (d > 0 && d < keepFrom && nOld < 8) old[nOld++] = d;
  }
  for (int i = 0; i < nOld; i++) {
    char p[40];
    snprintf(p, sizeof(p), "/logs/files/%08ld.json", old[i]); fbLogRequest(base, String(p), "DELETE", "");
    snprintf(p, sizeof(p), "/logs/index/%08ld.json", old[i]); fbLogRequest(base, String(p), "DELETE", "");
  }
}
// A failed request: a dropped or refused connection (code < 0) is retried on a fresh socket, up to FB_LOG_NET_TRIES
// times per step; anything else (or the last try) ends the upload. Returns true when the step should just be retried.
static bool fbLogNetRetry(int code) {
  if (code >= 0 || ++fbLogJob.netTries >= FB_LOG_NET_TRIES) return false;
  fbResetTls();                                             // drop a half-open TLS session; the next request re-handshakes
  return true;
}
// One step per netTask pass: 0 = check the file, clear an older copy, mark "uploading"; 1 = one piece; 2 = mark "done".
static void fbLogJobStep() {
  String base; bool en;
  xSemaphoreTake(netMux, portMAX_DELAY); base = fbUrl; en = firebaseEnabled; xSemaphoreGive(netMux);
  if (!en || base.length() < 8) { fbLogJobFinish(false, 0); return; }
  // LINK PACE (18 Nov 2026 e): while BUSY the upload waits where it is, and its time limit waits with it.
  if (linkBusyFlag) { fbLogJob.startMs = millis(); return; }
  if (millis() - fbLogJob.startMs > FB_LOG_JOB_MAX_MS) { fbLogJobFinish(false, -5); return; }
  if (WiFi.status() != WL_CONNECTED || !fbEnsureToken()) return;        // wait (bounded by FB_LOG_JOB_MAX_MS)
  if (!fbLastOk) return;          // the live report goes first: start or go on only while Firebase answers (2026-11-17)
  // The 60 KB floor only before a new session, as for the live report (18 Nov 2026 c); a piece's own block is checked below.
  if (heap8Block() < FB_REPORT_MIN_BLOCK) return;  // byte-addressable (18 Nov 2026 h); a new session is gated in fbBegin()
  String idx = String("/logs/index/") + fbLogJob.day + ".json";
  if (fbLogJob.phase == 0) {
    if (!sdTake(2000)) return;
    File f = SD.open(fbLogJob.path, FILE_READ);
    bool exists = f && !f.isDirectory();
    fbLogJob.size = exists ? f.size() : 0;
    if (f) f.close();
    sdGive();
    if (!exists) {
      fbLogRequest(base, idx, "PUT", "{\"state\":\"missing\",\"at\":{\".sv\":\"timestamp\"}}");
      fbLogJobFinish(false, -2); return;
    }
    int code = fbLogRequest(base, String("/logs/files/") + fbLogJob.day + ".json", "DELETE", "");   // an older copy
    if (!fbOk(code)) { if (!fbLogNetRetry(code)) fbLogJobFinish(false, code); return; }
    code = fbLogRequest(base, idx, "PUT", String("{\"state\":\"uploading\",\"size\":") + fbLogJob.size +
                                          ",\"partial\":" + (fbLogJob.partial ? "true" : "false") +
                                          ",\"at\":{\".sv\":\"timestamp\"}}");
    if (!fbOk(code)) { if (!fbLogNetRetry(code)) fbLogJobFinish(false, code); return; }
    fbLogJob.phase = 1; fbLogJob.off = 0; fbLogJob.n = 0; fbLogJob.netTries = 0;
    return;
  }
  if (fbLogJob.phase == 1) {
    if (fbLogJob.off >= fbLogJob.size) { fbLogJob.phase = 2; return; }
    size_t want = fbLogJob.size - fbLogJob.off;
    if (want > LOG_CHUNK_BYTES) want = LOG_CHUNK_BYTES;
    // The piece's JSON text is the only big allocation, and it is freed when this pass ends. It needs one block of
    // about want x 1.15; without that, wait for the next pass instead of fragmenting the heap further.
    if (heap8Block() < want + want / 4 + 1024) return;   // byte-addressable (18 Nov 2026 h)
    String body;
    if (!body.reserve(want + want / 8 + 16)) return;
    body += '"';
    if (!sdTake(2000)) return;
    size_t got = 0;
    File g = SD.open(fbLogJob.path, FILE_READ);
    if (g && g.seek(fbLogJob.off)) {
      uint8_t blk[LOG_READ_BLOCK];                         // small and on netTask's stack: no static buffer (see LOG_CHUNK_BYTES)
      while (got < want) {
        size_t n = want - got; if (n > sizeof(blk)) n = sizeof(blk);
        size_t r = g.read(blk, n);
        if (r == 0) break;
        fbJsonEscapeAppend(body, blk, r);
        got += r;
      }
    }
    if (g) g.close();
    sdGive();
    if (got == 0) { fbLogJob.size = fbLogJob.off; fbLogJob.phase = 2; return; }   // the file ended early
    body += '"';
    char p[40]; snprintf(p, sizeof(p), "/logs/files/%s/%04u.json", fbLogJob.day, (unsigned)fbLogJob.n);
    int code = fbLogRequest(base, String(p), "PUT", body);
    if (!fbOk(code)) {
      if (fbLogNetRetry(code)) return;                     // the same piece again, on a fresh socket
      fbLogRequest(base, idx, "PUT", "{\"state\":\"failed\",\"at\":{\".sv\":\"timestamp\"}}");
      fbLogJobFinish(false, code); return;
    }
    fbLogJob.off += got; fbLogJob.n++; fbLogJob.netTries = 0;
    return;
  }
  int code = fbLogRequest(base, idx, "PUT", String("{\"state\":\"done\",\"size\":") + fbLogJob.size +
                                         ",\"chunks\":" + fbLogJob.n + ",\"partial\":" + (fbLogJob.partial ? "true" : "false") +
                                         ",\"at\":{\".sv\":\"timestamp\"}}");
  if (!fbOk(code) && fbLogNetRetry(code)) return;
  if (fbOk(code)) fbLogRetention(base);
  fbLogJobFinish(fbOk(code), code);
}
// irrigation/logRequest = {day, at}: the website's "Upload from the rig". Only a CHANGE of `at` counts (see the viewer poll).
static void firebasePollLogRequest() {
  static unsigned long lastPollMs = 0;
  static int64_t lastAt = -1;
  static bool primed = false;
  if (millis() - lastPollMs < FB_LOGREQ_POLL_MS) return;     // every 30 s (10 s on 17-18 Nov f; LINK PACE table)
  lastPollMs = millis();
  String base; bool enabled;
  xSemaphoreTake(netMux, portMAX_DELAY); base = fbUrl; enabled = firebaseEnabled; xSemaphoreGive(netMux);
  if (!enabled || base.length() < 8 || WiFi.status() != WL_CONNECTED) return;
  if (!fbEnsureToken()) return;
  if (!fbBegin(fbBuildUrl(base, "/irrigation/logRequest.json"))) return;
  int code = fbHttp.GET();
  String resp = (code > 0) ? fbHttp.getString() : "";
  if (code != 200) resp = "";
  fbHttp.end();
  if (code == 401) { xSemaphoreTake(netMux, portMAX_DELAY); fbIdToken = ""; xSemaphoreGive(netMux); fbTokenExpiryMs = 0; return; }
  if (code != 200 || resp.length() == 0 || resp == "null") { primed = true; return; }
  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, resp)) return;
  int64_t at = doc["at"] | (int64_t)-1;
  long day = doc["day"] | 0L;
  if (primed && at > 0 && at != lastAt && day >= 20200101L && day <= 20991231L) webLogReqStamp = day;
  primed = true; lastAt = at;
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
/* Core 0: read the dashboard's manual-mode hold from a SINGLE node it overwrites with set().
 * Deliberately not a queue entry -- a 20 s keep-alive through /irrigation/commands would grow that
 * node without bound (the failure that once broke command delivery) and bury the real commands in
 * the history the operator reads.
 *
 * The lease is refreshed only when `seq` CHANGES, so a stale want:true left behind by a closed
 * browser stops refreshing and expires on its own. ESP1 keeps the deadline in millis(), so no clock
 * agreement with the browser is needed. */
static void firebasePollManual() {
  // Throttled to the same cadence as the command poll. netTask loops every 500 ms, so without this
  // the hold node was fetched twice a SECOND -- roughly 173k requests a day, competing with the
  // uploads on the same socket and keeping the radio awake on a battery rig. A 60 s lease pinged
  // every 20 s needs nothing faster than this.
  static unsigned long lastManualPollMs = 0;
  if (millis() - lastManualPollMs < FIREBASE_COMMAND_POLL_MS) return;
  lastManualPollMs = millis();
  String base; bool enabled;
  xSemaphoreTake(netMux, portMAX_DELAY); base = fbUrl; enabled = firebaseEnabled; xSemaphoreGive(netMux);
  if (!enabled || base.length() < 8 || WiFi.status() != WL_CONNECTED) return;
  if (!fbEnsureToken()) return;
  if (!fbBegin(fbBuildUrl(base, "/irrigation/manual.json"))) return;
  int code = fbHttp.GET();
  String resp = (code > 0) ? fbHttp.getString() : "";      // read error bodies too: an unread one poisons the reused socket
  if (code != 200) resp = "";
  fbHttp.end();
  if (code == 401) { xSemaphoreTake(netMux, portMAX_DELAY); fbIdToken = ""; xSemaphoreGive(netMux); fbTokenExpiryMs = 0; return; }
  if (code != 200 || resp.length() == 0 || resp == "null") return;

  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, resp)) return;              // malformed -> leave the hold exactly as it is
  // Must read as int64_t, not long/int: `doc["seq"] | -1` returns the literal default whenever the
  // stored value does not fit the requested type (ArduinoJson's is<T>()-gated `|` operator, not a
  // truncating cast) -- and a JS Date.now() value never fits a 32-bit long, so a 32-bit read here
  // silently came back exactly -1 on every single poll, so `seq` always equalled the initial
  // webManualSeq and webManualPing was never set, leaving webManualTick() stuck taking its "not
  // held, no new ping" early return forever. This is why
  // Proceed never lit up even though the page's write to /irrigation/manual was correct the whole time.
  int64_t seq = doc["seq"] | (int64_t)-1;
  bool want = doc["want"] | false;
  webManualReq = want;                                 // core 1 applies it (grant/refuse/release)
  if (seq != webManualSeq) { webManualSeq = seq; webManualPing = true; }
}

// irrigation/viewer/refreshAt (a server timestamp, written by the dashboard's Refresh button and when a page opens). Only
// a CHANGE counts -- ESP1's clock is not Firebase's, so the value is never compared with a time. viewer/at is ignored since
// 18 Nov 2026 g (it set the "watched" pace). Core 0 only: it sets a plain flag that the same netTask loop reads.
static void firebasePollViewer() {
  static unsigned long lastPollMs = 0;
  static int64_t lastRefresh = -1;
  static bool primed = false;
  if (millis() - lastPollMs < FB_VIEWER_POLL_MS) return;
  lastPollMs = millis();
  String base; bool enabled;
  xSemaphoreTake(netMux, portMAX_DELAY); base = fbUrl; enabled = firebaseEnabled; xSemaphoreGive(netMux);
  if (!enabled || base.length() < 8 || WiFi.status() != WL_CONNECTED) return;
  if (!fbEnsureToken()) return;
  if (!fbBegin(fbBuildUrl(base, "/irrigation/viewer.json"))) return;
  int code = fbHttp.GET();
  String resp = (code > 0) ? fbHttp.getString() : "";      // read error bodies too: an unread one poisons the reused socket
  if (code != 200) resp = "";
  fbHttp.end();
  if (code == 401) { xSemaphoreTake(netMux, portMAX_DELAY); fbIdToken = ""; xSemaphoreGive(netMux); fbTokenExpiryMs = 0; return; }
  if (code != 200 || resp.length() == 0 || resp == "null") return;
  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, resp)) return;
  int64_t rf = doc["refreshAt"] | (int64_t)-1;              // int64: a JS timestamp (see PollManual)
  if (primed && rf > 0 && rf != lastRefresh) fbRefreshPending = true;
  primed = true; lastRefresh = rf;
}

/* Web hold-to-run (2026-09-10, Website Manual/Test tab, mousedown/mouseup) -- irrigation/testHold =
 * {seq, bit, want}. Mirrors firebasePollManual()'s own seq/want polling exactly. A browser cannot
 * hold a physical dead-man switch, so it refreshes this node every ~1.2s while the button is held
 * instead; TEST_HOLD_BROWSER_TIMEOUT_MS is ESP1's own "the browser vanished" timeout (checked in
 * pulseTick()'s PU_RUN branch for hold-mode) -- ESP2's OWN 400ms dead-man is what actually protects
 * the relay if ESP1 itself goes away, exactly as it already does for TEST_PULSE.
 * Deep-scan finding (2026-09-10, HIGH, fixed): this runs on core 0 (netTask) and must ONLY ever write
 * the three plain scalars declared above (testHoldReqWant/Bit/Fresh) -- never call setState()/
 * logEvent()/sendEsp2()/pulseFinish() from here. Those mutate core-1-owned state (the logBuf String,
 * the ESP2 UART, sysState/uiMode) with no lock; pulseTick() (core 1, runs every loop()) is the only
 * place that reads these scalars and performs the actual arm/release -- same split this file already
 * uses for firebasePollManual()/webManualTick(). */
static void firebasePollTestHold() {
  static unsigned long lastPollMs = 0;
  // puHoldMode is a plain bool written by core 1; a stale read only shifts one poll by a cadence step.
  unsigned long pollMs = puHoldMode ? TEST_HOLD_POLL_MS : FIREBASE_COMMAND_POLL_MS;
  if (millis() - lastPollMs < pollMs) return;
  lastPollMs = millis();
  String base; bool enabled;
  xSemaphoreTake(netMux, portMAX_DELAY); base = fbUrl; enabled = firebaseEnabled; xSemaphoreGive(netMux);
  if (!enabled || base.length() < 8 || WiFi.status() != WL_CONNECTED) return;
  if (!fbEnsureToken()) return;
  if (!fbBegin(fbBuildUrl(base, "/irrigation/testHold.json"))) return;
  int code = fbHttp.GET();
  String resp = (code > 0) ? fbHttp.getString() : "";      // read error bodies too: an unread one poisons the reused socket
  if (code != 200) resp = "";
  fbHttp.end();
  if (code == 401) { xSemaphoreTake(netMux, portMAX_DELAY); fbIdToken = ""; xSemaphoreGive(netMux); fbTokenExpiryMs = 0; return; }
  if (code != 200 || resp.length() == 0 || resp == "null") return;

  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, resp)) return;
  int64_t seq = doc["seq"] | (int64_t)-1;
  bool want = doc["want"] | false;
  const char *bit = doc["bit"] | "";
  if (seq == testHoldSeq) return;                      // no new write since last poll -- nothing to do
  testHoldSeq = seq;

  if (want) testHoldLastFreshMs = millis();   // plain unsigned long, same cross-core convention as esp2WarmupMs etc.
  testHoldReqWant = want;
  strncpy(testHoldReqBit, bit, sizeof(testHoldReqBit) - 1);
  testHoldReqBit[sizeof(testHoldReqBit) - 1] = '\0';
  testHoldReqFresh = true;                    // pulseTick() (core 1) picks this up next loop() iteration
}

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
  if (!fbBegin(fbBuildUrl(base, "/irrigation/commands.json", "&orderBy=%22%24key%22&limitToLast=10"))) return;
  int code = fbHttp.GET();
  String resp = (code > 0) ? fbHttp.getString() : "";      // read error bodies too: an unread one poisons the reused socket
  if (code != 200) resp = "";
  fbHttp.end();
  if (code == 401) { xSemaphoreTake(netMux, portMAX_DELAY); fbIdToken = ""; xSemaphoreGive(netMux); fbTokenExpiryMs = 0; return; }
  if (code != 200 || resp.length() == 0 || resp == "null") return;

  // Filter: we only ever read these four fields, so ArduinoJson can skip the rest of the
  // document instead of allocating it. Keeps this off the netTask stack (was 6 KB in the fork).
  // 1024, raised from 256 when the recovery/config commands added ~15 payload keys. A filter that
  // OVERFLOWS silently drops the keys that did not fit, and the fields behind them then parse as
  // absent -- indistinguishable from the operator not sending them. That is the same silent-drop
  // class as the doseMl empty-object bug, so it is checked below rather than assumed.
  StaticJsonDocument<1024> filter;
  JsonObject any = filter.createNestedObject("*");
  any["status"] = true; any["type"] = true;
  JsonObject pf = any.createNestedObject("payload");
  pf["pump"] = true;
  pf["columns"] = true; pf["liters"] = true;      // FORCE_RUN: which columns, TOTAL litres
  // `true`, NOT createNestedObject(): in an ArduinoJson filter an object means "keep only the
  // members listed inside me", so an EMPTY object keeps nothing. doseMl parsed as {} and every
  // dose read back 0.0 -- which made `fert` false, so a requested fertigation silently ran as
  // plain irrigation. `true` allows the whole subtree. (Filter.hpp: allow() on a null member.)
  pf["doseMl"] = true;                             // FORCE_RUN: {"A":mL,"B":mL,"C":mL}
  pf["delaySeconds"] = true;                       // FORCE_RUN: countdown before it fires
  // One shared scalar for the single-argument commands: RECOVER{action}, REBOOT{target},
  // SET_COLUMN{col}, TEST_PULSE{zone}. Keeping them one key keeps this filter small, and the
  // filter is what bounds the parse buffer.
  pf["action"] = true; pf["target"] = true; pf["col"] = true; pf["zone"] = true;
  pf["seconds"] = true;
  pf["exerciseEnabled"] = true; pf["exerciseSeconds"] = true;
  pf["mode"] = true; pf["enabled"] = true; pf["schedMode"] = true;
  pf["winStart"] = true; pf["winEnd"] = true;
  pf["windows"] = true;                            // SET_COLUMN: [[start,end], ...] own watering times (2026-11-05)
  pf["targetN"] = true; pf["targetP"] = true; pf["targetK"] = true; pf["targetPH"] = true;
  pf["preset"] = true;                             // SET_COLUMN: crop preset name
  pf["operation"] = true; pf["plan"] = true; pf["planFrom"] = true;   // SET_COLUMN: Operation + day plan (2026-11-18)
  pf["soilStart"] = true; pf["soilWet"] = true;    // SET_COLUMN: own Soil start / Too wet (2026-11-18)
  pf["clkY"] = true; pf["clkMo"] = true; pf["clkD"] = true; pf["clkH"] = true; pf["clkMi"] = true;
  pf["thStart"] = true; pf["thGap"] = true;
  pf["thWet"] = true; pf["thRes"] = true;          // SET_THRESH: Too wet + the reservoir pause (2026-11-18)
  pf["vMin"] = true; pf["vMax"] = true;            // SET_AC_VOLT: the AC supply window (2026-11-14)
  pf["aMin10"] = true; pf["aMax10"] = true;        // SET_AC_AMP: the AC pump current limits (2026-11-16)

  // A filter that did not fit would silently drop keys; say so loudly instead of mis-parsing.
  if (filter.overflowed()) {
    static unsigned long lastFiltLogMs = 0;
    if (millis() - lastFiltLogMs > 60000) {
      lastFiltLogMs = millis();
      strlcpy(fbPollErrLog, "filter overflow -- payload keys dropped", sizeof(fbPollErrLog));
      fbPollErrLogPending = true;                             // core 1 logs it (no String from core 0)
    }
    return;
  }

  // 4096, raised from 2048 alongside the filter: a SET_COLUMN node retains ~12 values, and
  // limitToLast=10 of those is well past what 2048 held. Belt-and-braces on top of the limitToLast
  // bound above; the failure is REPORTED rather than swallowed, because a bare `return` here is
  // what let a NoMemory look like "the device just stopped responding" for days.
  StaticJsonDocument<4096> doc;
  DeserializationError de = deserializeJson(doc, resp, DeserializationOption::Filter(filter));
  if (de) {
    static unsigned long lastParseLogMs = 0;                  // dedupe: this path polls every 3 s
    if (millis() - lastParseLogMs > 60000) {
      lastParseLogMs = millis();
      snprintf(fbPollErrLog, sizeof(fbPollErrLog), "parse %s|body=%uB", de.c_str(), (unsigned)resp.length());
      fbPollErrLogPending = true;                             // core 1 logs it (no String from core 0)
    }
    return;
  }
  if (!doc.is<JsonObject>()) return;

  for (JsonPair item : doc.as<JsonObject>()) {
    JsonObject req = item.value().as<JsonObject>();
    if (String(req["status"] | "") != "queued") continue;
    // Bootloop guard, second half. A REBOOT esp1 whose "completed" PATCH never landed comes back
    // still marked "queued" and would reboot us again, forever. The id was written to NVS before
    // restarting; consume it once here and re-mark the node so the cycle cannot repeat.
    if (fbSkipCmdId.length() && fbSkipCmdId == item.key().c_str()) {
      fbSkipCmdId = "";
      fbSkipCmdClear = true;                               // core 1 clears the NVS key (no NVS from core 0)
      FirebaseCommandStatus done = {};
      strlcpy(done.id, item.key().c_str(), sizeof(done.id));
      strlcpy(done.status, "completed", sizeof(done.status));
      strlcpy(done.detail, "esp1 rebooted", sizeof(done.detail));
      xQueueSend(fbStatusQueue, &done, 0);
      continue;
    }
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
    // Absent delaySeconds -> the safe default, not 0. Clamped again on core 1.
    c.delayS    = (uint16_t)constrain((int)(req["payload"]["delaySeconds"] | (int)FORCE_DEFAULT_DELAY_S),
                                      0, (int)FORCE_MAX_DELAY_S);
    // One scalar shared by the single-argument commands, first match wins.
    JsonVariant ja = req["payload"]["action"];
    if (ja.isNull()) ja = req["payload"]["target"];
    if (ja.isNull()) ja = req["payload"]["col"];
    if (ja.isNull()) ja = req["payload"]["zone"];
    strlcpy(c.arg, ja.is<const char *>() ? ja.as<const char *>() : "", sizeof(c.arg));
    c.seconds = req["payload"]["seconds"] | 0;
    c.exEnabled = req["payload"]["exerciseEnabled"].isNull() ? -1 : (req["payload"]["exerciseEnabled"] ? 1 : 0);
    c.exSeconds = req["payload"]["exerciseSeconds"] | -1;
    // SET_COLUMN: -1 / "" mean "not supplied", so a partial update leaves the rest alone.
    strlcpy(c.colMode, req["payload"]["mode"] | "", sizeof(c.colMode));
    c.enabled   = req["payload"]["enabled"].isNull()   ? -1 : (req["payload"]["enabled"]   ? 1 : 0);
    c.schedMode = req["payload"]["schedMode"].isNull() ? -1 : (int8_t)(req["payload"]["schedMode"] | 0);
    c.winStart  = req["payload"]["winStart"] | -1;
    c.winEnd    = req["payload"]["winEnd"]   | -1;
    // windows: the column's own watering times, [[start, end], ...] in minutes of the day (2026-11-05). Copied as
    // sent and checked on core 1; more pairs than MAX_RUNS_PER_DAY are marked too many, not cut short.
    c.winCount = -1;
    JsonVariant jw = req["payload"]["windows"];
    if (!jw.isNull()) {
      c.winCount = 0;
      if (jw.is<JsonArray>()) {
        for (JsonVariant pair : jw.as<JsonArray>()) {
          if (c.winCount >= MAX_RUNS_PER_DAY) { c.winCount = MAX_RUNS_PER_DAY + 1; break; }
          c.wins[c.winCount][0] = pair[0].is<int>() ? (int16_t)pair[0].as<int>() : -1;
          c.wins[c.winCount][1] = pair[1].is<int>() ? (int16_t)pair[1].as<int>() : -1;
          c.winCount++;
        }
      }
    }
    c.tgt[0]    = req["payload"]["targetN"]  | -1.0f;
    c.tgt[1]    = req["payload"]["targetP"]  | -1.0f;
    c.tgt[2]    = req["payload"]["targetK"]  | -1.0f;
    c.tgt[3]    = req["payload"]["targetPH"] | -1.0f;
    strlcpy(c.preset, req["payload"]["preset"] | "", sizeof(c.preset));
    // SET_COLUMN Operation + day plan (2026-11-18). Checked on core 1 like every other SET_COLUMN field; here only
    // copied, with "absent", "not recognised" and "too long" kept apart so core 1 can refuse instead of guessing.
    {
      JsonVariant jo = req["payload"]["operation"];
      if (jo.isNull()) c.operation = -1;
      else {
        const char *o = jo.is<const char *>() ? jo.as<const char *>() : "";
        c.operation = !strcmp(o, "AUTOMATIC") ? 0 : !strcmp(o, "MANUAL") ? 1 : 2;
      }
      JsonVariant jp = req["payload"]["plan"];
      c.planState = 0;
      if (!jp.isNull()) {
        const char *p = jp.is<const char *>() ? jp.as<const char *>() : nullptr;
        c.planState = (p && strlcpy(c.plan, p, sizeof(c.plan)) <= (size_t)RUN_PLAN_DAYS) ? 1 : 2;
      }
      c.planFrom = req["payload"]["planFrom"] | -1L;
    }
    // SET_COLUMN own Soil start / Too wet (2026-11-18): absent, a whole number (range-checked on core 1), or refused.
    {
      JsonVariant js = req["payload"]["soilStart"], jw2 = req["payload"]["soilWet"];
      c.soilStart = js.isNull()  ? COL_PCT_ABSENT : js.is<int>()  ? constrain(js.as<int>(),  -999, 999) : COL_PCT_BAD;
      c.soilWet   = jw2.isNull() ? COL_PCT_ABSENT : jw2.is<int>() ? constrain(jw2.as<int>(), -999, 999) : COL_PCT_BAD;
    }
    // SET_CLOCK: all required together; -1/0 sentinels simply fail the range check if omitted.
    c.clkY  = req["payload"]["clkY"]  | -1;
    c.clkMo = req["payload"]["clkMo"] | -1;
    c.clkD  = req["payload"]["clkD"]  | -1;
    c.clkH  = req["payload"]["clkH"]  | -1;
    c.clkMi = req["payload"]["clkMi"] | -1;
    // SET_THRESH: all required together.
    c.thStart = req["payload"]["thStart"] | -1;
    c.thGap   = req["payload"]["thGap"]   | -1.0f;
    c.thWet   = req["payload"]["thWet"]   | -1;      // optional (2026-11-18); a decimal reads as absent
    c.thRes   = req["payload"]["thRes"]   | -1;
    // SET_AC_VOLT: both required, whole volts (a decimal reads as absent and is refused).
    c.acVMin  = req["payload"]["vMin"] | -1;
    c.acVMax  = req["payload"]["vMax"] | -1;
    // SET_AC_AMP: both required, whole tenths of an amp (5 = 0.5 A); a decimal reads as absent and is refused.
    c.acAMin10 = req["payload"]["aMin10"] | -1;
    c.acAMax10 = req["payload"]["aMax10"] | -1;
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
    // print=silent + drain: same reason as the live PUT -- an unread echo poisons the kept-alive socket.
    if (!fbBegin(fbBuildUrl(base, path.c_str(), "&print=silent"))) return;
    fbHttp.addHeader("Content-Type", "application/json");
    int code = fbHttp.PATCH((uint8_t *)json.c_str(), json.length());     // by pointer (18 Nov 2026 h)
    if (code > 0) fbHttp.getString();
    fbHttp.end();
    if (code == 401) { xSemaphoreTake(netMux, portMAX_DELAY); fbIdToken = ""; xSemaphoreGive(netMux); fbTokenExpiryMs = 0; return; }
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
      // The run is stored in tenths of a litre (fcSeq[].dL below), so anything under 0.05 L rounded to an
      // empty WATER,0.00 work order for ESP2. Refused instead (2026-11-05); the dashboard's own floor is 0.1 L.
      if (c.liters <= 0.0f || c.liters > FORCE_MAX_LITERS || lroundf(c.liters * 10.0f) < 1) {
        firebaseQueueStatus(c.id, "rejected", "liters out of range");
        logEvent("FIREBASE", "REJECT", "FORCE|LITERS=" + String(c.liters, 1)); continue;
      }
      { const char *bw;
        if (secBattBlocksAc(&bw)) { firebaseQueueStatus(c.id, "rejected", bw); logEvent("FIREBASE", "REJECT", "FORCE|BATT2"); continue; } }
      bool overDose = false;
      for (int i = 0; i < 3; i++) if (c.doseMl[i] < 0.0f || c.doseMl[i] > FORCE_MAX_DOSE_ML) overDose = true;
      if (overDose) {
        firebaseQueueStatus(c.id, "rejected", "nutrient mL out of range");
        logEvent("FIREBASE", "REJECT", "FORCE|DOSE_RANGE"); continue;
      }
      // Tell the operator now, not after the countdown: sendForceWorkOrder would refuse this at fire time.
      if (!forceBatchOk(c.liters, FLUSH_PCT, MIXING_TANK_SAFE_MIN, c.doseMl[0], c.doseMl[1], c.doseMl[2])) {
        firebaseQueueStatus(c.id, "rejected", "batch too small to dose safely -- raise liters or remove nutrients");
        logEvent("FIREBASE", "REJECT", "FORCE|BATCH_LOW"); continue;
      }
      // Same hardware gate the local UI honours. Schedule/soil are deliberately NOT checked.
      // actuationsDisabled/webManualHeld() were previously missing here: forceTick()/
      // sendForceWorkOrder() both re-check them at fire time so no work order is actually sent
      // while locked out, but without this the dashboard was told "accepted, armed" for the full
      // countdown (up to 300s) before silently failing -- directly contradicting the disabled-
      // actuations banner's own claim that forced runs are blocked immediately. Checking here too
      // gives an honest, immediate rejection instead.
      if (sysState != IDLE_STATE || uiMode != UI_DATA || wo.active || pendingRun.active ||
          pendingExercise.active || esp2Held || fbRemoteExerciseId[0] ||
          actuationsDisabled || webManualHeld()) {
        // Named reason, not a flat "not idle" -- mirrors webManualTick()'s own wording so the two
        // gates describe the same conditions the same way instead of one being specific and the
        // other opaque.
        const char *why = (sysState != IDLE_STATE || wo.active || pendingRun.active) ? "a run is in progress"
                         : esp2Held ? "a fault is held"
                         : (pendingExercise.active || fbRemoteExerciseId[0]) ? "a test is already running"
                         : actuationsDisabled ? "actuations are disabled"
                         : webManualHeld() ? "the Manual/Test tab has direct control"
                         : "someone is using the LCD";
        firebaseQueueStatus(c.id, "rejected", why);
        logEvent("FIREBASE", "REJECT", String("FORCE|NOT_IDLE|") + why); continue;
      }
      if (reservoirLow(sensor.msTank != 0, sensor.resLevel, resPauseLevel())) {
        firebaseQueueStatus(c.id, "rejected", "reservoir too low");
        logEvent("FIREBASE", "REJECT", "FORCE|RES_LOW"); continue;
      }
      // ARM the same countdown the LCD uses rather than dispatching now. The operator gets visible
      // proof the rig received the request (LCD "FORCE RUN ARMED" + the published countdown), either
      // surface can abort it, and forceTick() re-checks the safety gate at FIRE time -- so a fault or
      // a dropping reservoir during the delay cancels it instead of firing late into a changed rig.
      if (fcPhase != FP_NONE) {
        firebaseQueueStatus(c.id, "rejected", "another forced run is already armed");
        logEvent("FIREBASE", "REJECT", "FORCE|ALREADY_ARMED"); continue;
      }
      fcSlot = 0; fcMode = 0;                       // single batch; the mask below is authoritative
      fcSeq[0].mask = mask;
      fcSeq[0].dL   = (uint16_t)lroundf(c.liters * 10.0f);
      for (int i = 0; i < 3; i++) fcSeq[0].ml[i] = (uint16_t)lroundf(c.doseMl[i]);
      fcSeq[0].delayS = c.delayS;
      strlcpy(fcCmdId, c.id, sizeof(fcCmdId));
      fcFromWeb = true;
      fcPhase = FP_WAIT;
      fcFireAtMs = millis() + (unsigned long)c.delayS * 1000UL;
      wakeBacklight();
      firebaseQueueStatus(c.id, "accepted",
        (String("armed: starts in ") + c.delayS + "s").c_str());
      logEvent("FIREBASE", "CMD", String("FORCE|ARM|WEB|COLS=") + c.cols +
                                  "|L=" + String(c.liters, 1) + "|in=" + String(c.delayS) + "s");
      continue;
    }

    if (!strcmp(c.type, "EMERGENCY_STOP")) {
      enterEmergencyStop(true);                            // existing system-wide physical stop
      logEvent("FIREBASE", "CMD", "ESTOP");
      firebaseQueueStatus(c.id, "completed", "emergency stop executed");
      continue;
    }

    /* ---- Recovery + lockout ---------------------------------------------------------------
     * These are the counterpart to EMERGENCY_STOP, which until now could stop the rig remotely
     * with no remote way back: the only exit from EMERGENCY_STOP was the physical LCD prompt.
     * They deliberately do NOT require the system to be idle -- the whole point is that it is not. */

    // ENABLE_ACTUATIONS is checked FIRST and gated on nothing. It is the escape hatch: if some other
    // bug leaves the rig stuck, this must still get through, so it never consults sysState, uiMode,
    // wo.active or esp2Held.
    if (!strcmp(c.type, "ENABLE_ACTUATIONS")) {
      setActuationsEnabled(true, "WEB");
      firebaseQueueStatus(c.id, "completed", "actuations enabled");
      continue;
    }
    if (!strcmp(c.type, "DISABLE_ACTUATIONS")) {
      setActuationsEnabled(false, "WEB");
      firebaseQueueStatus(c.id, "completed", "actuations disabled; monitoring continues");
      continue;
    }
    if (!strcmp(c.type, "CANCEL_FORCE")) {
      if (fcPhase == FP_NONE) { firebaseQueueStatus(c.id, "rejected", "no forced run is armed"); continue; }
      // Only a countdown can be cancelled (2026-11-05), the same rule as the LCD's BACK. Once forceTick()
      // has fired (FP_RUNNING) the work order is live on ESP2, and forceAbort() would only clear the phase:
      // this replied "armed forced run cancelled" while the pumps carried on, and a cancel takes up to 3 s
      // to be picked up, so it was easy to hit at the end of a countdown. Stopping a live run is
      // EMERGENCY_STOP's job, and the reply says so.
      if (fcPhase != FP_WAIT) { firebaseQueueStatus(c.id, "rejected", "run already started; use emergency stop"); continue; }
      forceAbort("WEB");                                   // resolves the armed run's own command too
      firebaseQueueStatus(c.id, "completed", "armed forced run cancelled");
      continue;
    }
    if (!strcmp(c.type, "ACK_FAULT")) {
      // "Do nothing, but ask me again." Purely a UI snooze -- it changes no rig behaviour, it only
      // tells the dashboard when to re-raise the prompt.
      faultAckUntilMs = millis() + FAULT_REPROMPT_MS;
      logEvent("FIREBASE", "CMD", "ACK_FAULT");
      firebaseQueueStatus(c.id, "completed", "acknowledged; will re-prompt in 2 minutes");
      continue;
    }
    if (!strcmp(c.type, "RECOVER")) {
      if (!esp2Held) { firebaseQueueStatus(c.id, "rejected", "no held fault to recover from"); continue; }
      int sel = !strcmp(c.arg, "hold")     ? 0 : !strcmp(c.arg, "release")  ? 1
              : !strcmp(c.arg, "irrigate") ? 2 : !strcmp(c.arg, "normal")   ? 3 : -1;
      if (sel < 0) { firebaseQueueStatus(c.id, "rejected", "action must be hold/release/irrigate/normal"); continue; }
      issueRecovery(sel);                                  // the same call the LCD menu and SMS make
      logEvent("FIREBASE", "CMD", String("RECOVER|") + c.arg);
      firebaseQueueStatus(c.id, "completed", (String("recovery: ") + c.arg).c_str());
      continue;
    }
    if (!strcmp(c.type, "ESTOP_RECOVER")) {
      if (sysState != EMERGENCY_STOP) { firebaseQueueStatus(c.id, "rejected", "not in emergency stop"); continue; }
      if (esp2Held) { firebaseQueueStatus(c.id, "rejected", "a held fault is pending; use RECOVER first"); continue; }
      // Exactly what LCD ENTER on "Return to normal" does: re-power and re-validate ESP2 before
      // trusting it again, rather than jumping straight to IDLE.
      setState(STARTUP_SYNC);
      logEvent("FIREBASE", "CMD", "ESTOP_RECOVER");
      firebaseQueueStatus(c.id, "completed", "returning to normal via startup sync");
      continue;
    }
    // ---- DIAG_SWEEP: pull ESP2's raw sensor values for the dashboard diagnostics tab ------------
    // Reads only -- CAL_START/CAL_STOP sample sensors, they actuate nothing. It does power ESP2 up,
    // which is why it is idle-only and time-boxed.
    if (!strcmp(c.type, "DIAG_SWEEP")) {
      unsigned long ms = (c.seconds > 0) ? (unsigned long)c.seconds * 1000UL : DIAG_REMOTE_DEFAULT_MS;
      if (ms > DIAG_REMOTE_MAX_MS) ms = DIAG_REMOTE_MAX_MS;
      if (diagRemoteUntilMs) { firebaseQueueStatus(c.id, "rejected", "a sweep is already running"); continue; }
      const char *dsWhy = "the controller is not idle";
      if (!diagRemoteStart(ms, &dsWhy)) {
        firebaseQueueStatus(c.id, "rejected", dsWhy);
        logEvent("FIREBASE", "REJECT", String("DIAG_SWEEP|NOT_IDLE|") + dsWhy); continue;
      }
      firebaseQueueStatus(c.id, "completed", (String("sweeping esp2 sensors for ") + (ms / 1000) + "s").c_str());
      continue;
    }

    // ---- TEST_PULSE: bounded booster-to-column push (the web answer to LCD Testing) -------------
    if (!strcmp(c.type, "TEST_PULSE")) {
      int row = pulseRowForTarget(c.arg);
      if (row < 0) { firebaseQueueStatus(c.id, "rejected", "unknown pulse target"); continue; }
      // A push-to-column combo on a disabled column would open a valve the rig does not service.
      if (row >= 17 && row <= 19 && !COLUMN_ENABLED[row - 17]) {
        firebaseQueueStatus(c.id, "rejected", "that column is disabled"); continue;
      }
      int secs = (c.seconds > 0) ? c.seconds : 5;
      if (secs < 1 || secs > PULSE_MAX_S) {
        firebaseQueueStatus(c.id, "rejected", (String("seconds must be 1-") + PULSE_MAX_S).c_str()); continue;
      }
      if (actuationsDisabled) { firebaseQueueStatus(c.id, "rejected", "actuations are disabled"); continue; }
      if (puPhase != PU_NONE) { firebaseQueueStatus(c.id, "rejected", "a pulse is already running"); continue; }
      { const char *bw;
        if (testRowUsesInverter(row) && secBattBlocksAc(&bw)) { firebaseQueueStatus(c.id, "rejected", bw); continue; } }
      if (sysState != IDLE_STATE || uiMode != UI_DATA || wo.active || pendingRun.active ||
          pendingExercise.active || esp2Held || fcPhase != FP_NONE || fbRemoteExerciseId[0]) {
        const char *why = (sysState != IDLE_STATE || wo.active || pendingRun.active) ? "a run is in progress"
                         : esp2Held ? "a fault is held"
                         : (fcPhase != FP_NONE) ? "a forced run is armed"
                         : (pendingExercise.active || fbRemoteExerciseId[0]) ? "a test is already running"
                         : "someone is using the LCD";
        firebaseQueueStatus(c.id, "rejected", why);
        logEvent("FIREBASE", "REJECT", String("PULSE|NOT_IDLE|") + why); continue;
      }
      puRow = row;
      puSeconds = secs;
      strlcpy(puCmdId, c.id, sizeof(puCmdId));
      esp2SetPower(true);                    // power ESP2 -> boot -> TEST,ENTER retries in pulseTick
      esp2TestArmed = false; testArmPending = false; testArmTries = 0; lastTestArmMs = 0;
      esp2WarmupMs = millis();               // prime reference (TEST_PRIME_MS)
      puArmMs = millis();
      puPhase = PU_ARM; testServoDoneRow = -1; puServoFailWhy[0] = '\0';
      setState(TEST_MODE);                   // stops controlTick/exerciseTick starting a run underneath
      firebaseQueueStatus(c.id, "accepted", (String("pulse ") + c.arg + " for " + secs + "s").c_str());
      logEvent("FIREBASE", "CMD", String("PULSE|") + c.arg + "|" + secs + "s");
      continue;
    }

    // ---- SET_EXERCISE: preventive pump exercise on/off + run length ---------------------------
    // Config only, so it is allowed while held, stopped or locked out -- turning the exercise OFF is
    // something you may well want to do precisely when the rig is misbehaving.
    if (!strcmp(c.type, "SET_EXERCISE")) {
      String changed;
      if (c.exEnabled >= 0) { exerciseEnabled = (c.exEnabled == 1); changed += String("state=") + (exerciseEnabled ? "ON " : "OFF "); }
      if (c.exSeconds >= 0) {
        if (c.exSeconds < EX_RUN_MIN || c.exSeconds > EX_RUN_MAX) {
          firebaseQueueStatus(c.id, "rejected",
            (String("seconds must be ") + EX_RUN_MIN + "-" + EX_RUN_MAX).c_str()); continue;
        }
        exerciseRunS = c.exSeconds; changed += String("run=") + exerciseRunS + "s ";
      }
      if (!changed.length()) { firebaseQueueStatus(c.id, "rejected", "nothing to change"); continue; }
      saveExercise();
      changed.trim();
      firebaseQueueStatus(c.id, "completed", (String("pump exercise: ") + changed).c_str());
      continue;
    }

    // ---- SET_COLUMN: irrigation/fertigation mode, schedule window, target ppm ------------------
    // Every field is optional so the page can send partial updates; -1 / "" mean "leave alone".
    // Config only -- it actuates nothing, so it is allowed while held, stopped or locked out.
    if (!strcmp(c.type, "SET_COLUMN")) {
      int ci = (c.arg[0] == 'A') ? 0 : (c.arg[0] == 'B') ? 1 : (c.arg[0] == 'C') ? 2 : -1;
      if (ci < 0 || ci >= NUM_COLUMNS) {
        firebaseQueueStatus(c.id, "rejected", "col must be A, B or C"); continue;
      }
      String err;

      // ---- Pass 1: validate every supplied field WITHOUT mutating anything. A command that has
      // one bad field must reject cleanly, not leave earlier fields applied-live-but-unsaved
      // (review-fix: SET_COLUMN used to mutate col[]/COLUMN_ENABLED[]/schedule in place field-by-field,
      // so a late failure stranded the earlier fields live in RAM with none of it persisted to NVS).
      uint8_t  newOp    = colOpManual[ci];       bool setOp    = false;
      RunPlan  newPlan  = colPlan[ci];           bool setPlan  = false;  int planMarked = 0;
      uint8_t  newMode  = col[ci].mode;          bool setMode  = false;
      bool     newEn    = COLUMN_ENABLED[ci];    bool setEn    = false;
      uint8_t  newSched = colSchedMode[ci];      bool setSched = false;
      uint16_t newWs    = COL_WIN_START[ci];
      uint16_t newWe    = COL_WIN_END[ci];       bool setWin   = false;
      static const char *TGT_KEY[4] = { "N", "P", "K", "PH" };
      float newTgt[4] = { col[ci].targetN, col[ci].targetP, col[ci].targetK, col[ci].targetPH };
      bool  setTgt[4] = { false, false, false, false };
      int8_t   newSs    = colSoilStart[ci];      bool setSs    = false;   // own Soil start / Too wet (2026-11-18)
      int8_t   newSw    = colSoilWet[ci];        bool setSw    = false;

      if (c.colMode[0]) {
        if      (!strcmp(c.colMode, "AUTO"))            { newMode = MODE_AUTO;            setMode = true; }
        else if (!strcmp(c.colMode, "IRRIGATION_ONLY")) { newMode = MODE_IRRIGATION_ONLY; setMode = true; }
        else { firebaseQueueStatus(c.id, "rejected", "mode must be AUTO or IRRIGATION_ONLY"); continue; }
      }
      if (c.enabled >= 0) { newEn = (c.enabled == 1); setEn = true; }
      // Operation + day plan (2026-11-18, run_plan.h). The plan replaces the column's whole plan; "" clears it.
      if (c.operation >= 0) {
        if (c.operation > 1) { firebaseQueueStatus(c.id, "rejected", "operation must be AUTOMATIC or MANUAL"); continue; }
        newOp = (uint8_t)c.operation; setOp = true;
      }
      if (c.planState) {
        if (c.planState != 1 || !planTextValid(c.plan)) {
          firebaseQueueStatus(c.id, "rejected", "plan: up to 62 days of - M A"); continue;
        }
        long today = planToday();
        if (!rtcOk || today < 0) { firebaseQueueStatus(c.id, "rejected", "set ESP1's clock before a day plan"); continue; }
        long from = (c.planFrom == -1) ? today : planDayFromYmd(c.planFrom);
        if (from < 0) { firebaseQueueStatus(c.id, "rejected", "planFrom must be a date, yyyymmdd"); continue; }
        // More than a day off ESP1's own date is not a clock difference but a wrong clock on one side.
        if (from < today - 1 || from > today + 1) {
          firebaseQueueStatus(c.id, "rejected", "plan date is not ESP1's date: check clock"); continue;
        }
        planMarked = planReplace(newPlan, today, from, c.plan); setPlan = true;
      }
      if (c.schedMode >= 0) {
        if (c.schedMode > 1) { firebaseQueueStatus(c.id, "rejected", "schedMode must be 0 (auto) or 1 (manual)"); continue; }
        newSched = (uint8_t)c.schedMode; setSched = true;
      }
      // Minutes-of-day. Start == end would be a zero-length window that never opens, so reject it
      // rather than quietly disabling the column's schedule.
      if (c.winStart >= 0 || c.winEnd >= 0) {
        int ws = (c.winStart >= 0) ? c.winStart : COL_WIN_START[ci];
        int we = (c.winEnd   >= 0) ? c.winEnd   : COL_WIN_END[ci];
        if (ws < 0 || ws > 1439 || we < 0 || we > 1439) {
          firebaseQueueStatus(c.id, "rejected", "window minutes must be 0-1439"); continue;
        }
        // Audit fix (2026-09-09): controlTick()'s window check (`mod < ws || mod > we`) has no
        // midnight-wraparound support -- ws > we would be true for every minute of the day, silently
        // and permanently disabling the column with only a routine, non-alerting OUTSIDE_WINDOW log
        // line. Reject it here rather than let it through, same reasoning as the ws==we check.
        if (ws >= we) { firebaseQueueStatus(c.id, "rejected", "window start must be before window end"); continue; }
        // With several windows a day, window 1 must still end before window 2 starts.
        if (colRuns[ci] > 1 && we >= COL_EXTRA_START[ci][0]) {
          firebaseQueueStatus(c.id, "rejected", "window 1 must end before window 2 starts"); continue;
        }
        newWs = (uint16_t)ws; newWe = (uint16_t)we; setWin = true;
      }
      // windows[] (2026-11-05): all of the column's own watering times at once, 1..MAX_RUNS_PER_DAY of them, in
      // order. Each must lie in the day, start before it ends, and end before the next starts. Not together with
      // winStart/winEnd, which is the one-window form older dashboards send.
      uint8_t  newRuns = colRuns[ci];
      uint16_t newWins[MAX_RUNS_PER_DAY][2] = {};
      bool     setWins = false;
      if (c.winCount >= 0) {
        if (c.winStart >= 0 || c.winEnd >= 0) { firebaseQueueStatus(c.id, "rejected", "send windows or winStart/winEnd, not both"); continue; }
        if (c.winCount < 1 || c.winCount > MAX_RUNS_PER_DAY) { firebaseQueueStatus(c.id, "rejected", "windows: 1 to 4 start/end pairs"); continue; }
        bool bad = false;
        for (int w = 0; w < c.winCount; w++) {
          if (c.wins[w][0] < 0 || c.wins[w][1] < 0) { bad = true; break; }
          newWins[w][0] = (uint16_t)c.wins[w][0]; newWins[w][1] = (uint16_t)c.wins[w][1];
        }
        if (bad || !windowsValid(newWins, c.winCount)) {
          firebaseQueueStatus(c.id, "rejected", "windows: in order, apart, minutes 0-1439"); continue;
        }
        newRuns = (uint8_t)c.winCount; setWins = true;
      }
      // Preset (additive): resolve against the real firmware preset table BEFORE the explicit
      // per-field loop below, so an explicit targetN/P/K/PH sent in the SAME command can still
      // override one field of the preset -- same precedence a human filling in the form after
      // picking a preset would expect. Website-only crop database is a separate concept and never
      // reaches here.
      String presetApplied;
      if (c.preset[0]) {
        String pn = c.preset; pn.toUpperCase();
        bool found = false;
        for (int p = 0; p < NUM_PRESETS; p++) {
          if (pn == CROP_PRESETS[p].name) {
            newTgt[0] = CROP_PRESETS[p].N; newTgt[1] = CROP_PRESETS[p].P;
            newTgt[2] = CROP_PRESETS[p].K; newTgt[3] = CROP_PRESETS[p].pH;
            setTgt[0] = setTgt[1] = setTgt[2] = setTgt[3] = true;
            presetApplied = CROP_PRESETS[p].name;
            found = true; break;
          }
        }
        if (!found) { firebaseQueueStatus(c.id, "rejected", "unknown preset name"); continue; }
      }
      // Exact-sentinel check, not "< 0": the parser's absent-field default is exactly -1.0f (see
      // firebasePollCommands), and no valid N/P/K/pH value is ever negative -- so a genuinely bad
      // value like -5 must still reach the real range check below instead of being read as "absent".
      bool tgtBad = false;
      for (int t = 0; t < 4; t++) {
        if (c.tgt[t] == -1.0f) continue;                  // absent
        if (!columnTargetInRange(TGT_KEY[t], c.tgt[t], err)) {
          firebaseQueueStatus(c.id, "rejected", err.c_str()); tgtBad = true; break;
        }
        newTgt[t] = c.tgt[t]; setTgt[t] = true;
      }
      if (tgtBad) continue;
      // Own Soil start / Too wet (2026-11-18), -1 = back to the default. The pair that will then apply must keep
      // Too wet at least THRESH_MIN_GAP_PCT over Soil start, or the column could be "dry enough" and "too wet" at once.
      if (c.soilStart != COL_PCT_ABSENT) {
        if (c.soilStart < -1 || c.soilStart > 100) { firebaseQueueStatus(c.id, "rejected", "soilStart must be -1 or 0-100"); continue; }
        newSs = (int8_t)c.soilStart; setSs = true;
      }
      if (c.soilWet != COL_PCT_ABSENT) {
        if (c.soilWet < -1 || c.soilWet > 100) { firebaseQueueStatus(c.id, "rejected", "soilWet must be -1 or 0-100"); continue; }
        newSw = (int8_t)c.soilWet; setSw = true;
      }
      if ((setSs || setSw) && !wetAboveStart(effPct(newSs, soilStartPct), effPct(newSw, soilWetPct))) {
        firebaseQueueStatus(c.id, "rejected", "too wet must be 5 % over soil start"); continue;
      }
      if (!(setOp || setPlan || setSs || setSw || setMode || setEn || setSched || setWin || setWins || setTgt[0] || setTgt[1] || setTgt[2] || setTgt[3])) {
        firebaseQueueStatus(c.id, "rejected", "no recognised field to change"); continue;
      }

      // ---- Pass 2: every field validated -- apply and persist together.
      String changed;
      if (presetApplied.length()) changed += "preset=" + presetApplied + " ";
      bool touchedCol = false, touchedSched = false, touchedEn = false, touchedPlan = false, touchedTh = false;
      // First in the reply, which holds only 47 characters: these are what the website checks for.
      if (setOp)    { colOpManual[ci] = newOp; changed += newOp ? "ops=MANUAL " : "ops=AUTO "; touchedPlan = true; }
      if (setPlan)  { colPlan[ci] = newPlan; changed += "plan=" + String(planMarked) + "d "; touchedPlan = true; }
      if (setSs)    { colSoilStart[ci] = newSs; changed += "start="; changed += (newSs < 0 ? String("def") : String(newSs)); changed += " "; touchedTh = true; }
      if (setSw)    { colSoilWet[ci] = newSw;   changed += "wet=";   changed += (newSw < 0 ? String("def") : String(newSw)); changed += " "; touchedTh = true; }
      if (setMode)  { col[ci].mode = newMode; changed += (newMode == MODE_AUTO) ? "mode=AUTO " : "mode=IRRIGATION_ONLY "; touchedCol = true; }
      if (setEn)    { COLUMN_ENABLED[ci] = newEn; changed += String("enabled=") + (newEn ? "1 " : "0 "); touchedEn = true; }
      if (setSched) { colSchedMode[ci] = newSched; changed += String("sched=") + (newSched ? "MANUAL " : "AUTO "); touchedSched = true; }
      if (setWin)   { COL_WIN_START[ci] = newWs; COL_WIN_END[ci] = newWe; changed += "win=" + String(newWs) + "-" + String(newWe) + " "; touchedSched = true; }
      if (setWins) {
        COL_WIN_START[ci] = newWins[0][0]; COL_WIN_END[ci] = newWins[0][1];
        for (int x = 0; x < newRuns - 1; x++) { COL_EXTRA_START[ci][x] = newWins[x + 1][0]; COL_EXTRA_END[ci][x] = newWins[x + 1][1]; }
        colRuns[ci] = newRuns;
        changed += "runs=" + String(newRuns) + " ";
        touchedSched = true;
      }
      for (int t = 0; t < 4; t++) {
        if (!setTgt[t]) continue;
        applyColumnTarget(ci, TGT_KEY[t], newTgt[t], err);   // already validated -- cannot fail here
        changed += String(TGT_KEY[t]) + "=" + String(newTgt[t], 1) + " ";
        touchedCol = true;
      }
      if (touchedCol)   saveColumn(ci);                   // only write the NVS blocks we touched
      if (touchedSched) saveSchedule(ci);
      if (touchedEn)    saveColEnable(ci);
      if (touchedPlan)  saveColumnPlan(ci);
      if (touchedTh)    saveColumnThresh(ci);
      changed.trim();
      logEvent("FIREBASE", "CFG", String("COL_") + COL_TAG[ci] + "|" + changed);
      firebaseQueueStatus(c.id, "completed", (String("COL_") + COL_TAG[ci] + ": " + changed).c_str());
      continue;
    }

    // ---- SET_CLOCK: set the RTC, the same call the LCD's own Set Clock editor makes. No NVS --
    // the DS3231's own battery-backed register is the only store, same as the local path.
    if (!strcmp(c.type, "SET_CLOCK")) {
      // rtcChipOk, not rtcOk: a clock that lost its time is exactly the one that needs setting.
      if (!rtcChipOk) { firebaseQueueStatus(c.id, "rejected", "RTC not present"); continue; }
      // Audit fix (2026-09-08): day-of-month was previously validated as a flat 1..31 regardless of
      // month/leap-year (e.g. "Feb 30" could be set), which the DS3231 has no validation of its own
      // to catch -- it would silently store the impossible date.
      if (c.clkY < RTC_YEAR_MIN || c.clkY > RTC_YEAR_MAX || c.clkMo < 1 || c.clkMo > 12 || c.clkD < 1 ||
          c.clkD > daysInMonth(c.clkY, c.clkMo) ||
          c.clkH < 0 || c.clkH > 23 || c.clkMi < 0 || c.clkMi > 59) {
        firebaseQueueStatus(c.id, "rejected", "clock fields out of range"); continue;
      }
      rtc.adjust(DateTime((uint16_t)c.clkY, (uint8_t)c.clkMo, (uint8_t)c.clkD, (uint8_t)c.clkH, (uint8_t)c.clkMi, 0));
      rtcClockWasSet();
      logEvent("ESP1", "STATE", "CLOCK_SET");
      firebaseQueueStatus(c.id, "completed", (String("clock set to ") + tsString()).c_str());
      continue;
    }

    // ---- SET_THRESH: same fields, same clamps, same save function as the LCD Thresholds editor
    // and the SMS THRESH command.
    if (!strcmp(c.type, "SET_THRESH")) {
      // Audit fix (2026-09-09): thGap uses the same -1 absent-field sentinel as thStart (set at parse
      // time), but was previously only clamped, never rejected when missing/negative -- a partial
      // payload silently zeroed fertGap instead of being refused, turning "fertigate only past a
      // meaningful shortfall" into "fertigate on any positive shortfall at all" with no log distinction
      // from a deliberate 0. Now rejected exactly like thStart. An older dashboard may still send
      // thStop; the payload filter drops it (decision D11).
      if (c.thStart < 0 || c.thGap < 0.0f) { firebaseQueueStatus(c.id, "rejected", "missing threshold fields"); continue; }
      int s  = clampi(c.thStart, 0, 100);
      float g = (c.thGap > 500.0f) ? 500.0f : c.thGap;
      // Too wet + the reservoir pause (2026-11-18): optional, so an older page still sets the first two alone. Checked
      // before anything changes: a refused field leaves all four as they were.
      int w = soilWetPct, r = resPausePct;
      if (c.thWet >= 0) {
        if (!wetAboveStart(s, c.thWet)) { firebaseQueueStatus(c.id, "rejected", "too wet must be 5 % over soil start, max 100"); continue; }
        w = c.thWet;
      }
      if (c.thRes >= 0) {
        if (!resPauseOk(c.thRes, (int)RES_LOW_PCT)) { firebaseQueueStatus(c.id, "rejected", "reservoir pause must be 18-50 %"); continue; }
        r = c.thRes;
      }
      soilStartPct = s; fertGap = g; soilWetPct = w; resPausePct = r;
      saveThresholds();
      logEvent("FIREBASE", "CFG", String("THRESH|start=") + s + "|gap=" + String(g, 1) + "|wet=" + w + "|res=" + r);
      firebaseQueueStatus(c.id, "completed",
        (String("thresholds: start=") + s + " gap=" + String(g, 1) + " wet=" + w + " res=" + r).c_str());
      continue;
    }

    // ---- SET_AC_VOLT: the AC supply window ESP2's PZEM check enforces. Same check and same save as the
    // LCD's AC Voltage editor. Who may send it is the rules' job (operators and Sub-operators only).
    if (!strcmp(c.type, "SET_AC_VOLT")) {
      if (c.acVMin < 0 || c.acVMax < 0) { firebaseQueueStatus(c.id, "rejected", "missing voltage fields"); continue; }
      if (!acVoltWindowOk(c.acVMin, c.acVMax)) {
        firebaseQueueStatus(c.id, "rejected", (String("need ") + AC_V_LIMIT_LOW + "-" + AC_V_LIMIT_HIGH +
                            " V, low " + AC_V_MIN_GAP + " V under high").c_str());
        continue;
      }
      // Someone has it open on the LCD: their Save would silently overwrite this, so refuse instead.
      if ((uiMode == UI_EDIT || editConfirm) && editItem == SET_ACVOLT) {
        firebaseQueueStatus(c.id, "rejected", "being edited on the LCD"); continue;
      }
      setAcVoltWindow(c.acVMin, c.acVMax, "FIREBASE");
      firebaseQueueStatus(c.id, "completed", (String("AC window ") + acVMin + "-" + acVMax + " V").c_str());
      continue;
    }

    // ---- SET_AC_AMP (2026-11-16): the AC pump current limits ESP2's PZEM check enforces -- no-current (min) and
    // overcurrent (max), whole tenths of an amp. Same check and same save as the LCD's AC Current editor. Who may
    // send it is the rules' job (operators and Sub-operators only, like SET_AC_VOLT).
    if (!strcmp(c.type, "SET_AC_AMP")) {
      if (c.acAMin10 < 0 || c.acAMax10 < 0) { firebaseQueueStatus(c.id, "rejected", "missing current fields"); continue; }
      char msg[48];
      if (!acAmpWindowOk(c.acAMin10, c.acAMax10)) {
        snprintf(msg, sizeof(msg), "need %.1f-%.1f A, min %.1f A under max",
                 AC_A_LIMIT_LOW / 10.0f, AC_A_LIMIT_HIGH / 10.0f, AC_A_MIN_GAP / 10.0f);
        firebaseQueueStatus(c.id, "rejected", msg);
        continue;
      }
      // Someone has it open on the LCD: their Save would silently overwrite this, so refuse instead.
      if ((uiMode == UI_EDIT || editConfirm) && editItem == SET_ACAMP) {
        firebaseQueueStatus(c.id, "rejected", "being edited on the LCD"); continue;
      }
      setAcAmpWindow(c.acAMin10, c.acAMax10, "FIREBASE");
      snprintf(msg, sizeof(msg), "AC current %.1f-%.1f A", acAMin10 / 10.0f, acAMax10 / 10.0f);
      firebaseQueueStatus(c.id, "completed", msg);
      continue;
    }

    // ---- RESTORE_DEFAULTS: idle-only, same gate as the LCD, calls the same reset function directly.
    if (!strcmp(c.type, "RESTORE_DEFAULTS")) {
      if (sysState != IDLE_STATE) { firebaseQueueStatus(c.id, "rejected", "system must be idle"); continue; }
      restoreDefaults();
      logEvent("FIREBASE", "CMD", "RESTORE_DEFAULTS");
      firebaseQueueStatus(c.id, "completed", "defaults restored");
      continue;
    }

    // ---- CLEAR_BATT_SWITCH: clear a latched battery-switch fault (re-arms switching). Same effect as
    // the LCD row and SMS BATTSW,CLEAR; privileged tier only (RTDB rules).
    if (!strcmp(c.type, "CLEAR_BATT_SWITCH")) {
      bool was = bswLatched;
      bswClear("WEB");
      firebaseQueueStatus(c.id, "completed", was ? "battery-switch fault cleared" : "no battery-switch fault was latched");
      continue;
    }

    // ---- TANK_CHECK_OK (18 Nov 2026): the website's "I've checked the tank". Same rule and effect as SMS
    // TANKCHECK,OK: only while idle, and it doesn't change ESP2's saved volume. Control accounts (RTDB rules).
    if (!strcmp(c.type, "TANK_CHECK_OK")) {
      if (sysState != IDLE_STATE) { firebaseQueueStatus(c.id, "rejected", "wait until the rig is idle"); continue; }
      bool was = tankCheckNeeded;
      tankCheckClear("WEB");
      fbRequestPush();                                   // the banner goes as soon as the next report lands
      firebaseQueueStatus(c.id, "completed", was ? "tank check confirmed: runs can start" : "no tank check was needed");
      continue;
    }

    // ---- LCD_LOCK { action: "lock" | "unlock" }: the website's lock switch (added 2026-11-05). The same flag,
    // NVS key and log as Settings > Lock Screen and the UP+DOWN unlock, so the LCD and the website show one lock.
    // Automation, the MODE+BACK emergency stop and the UP+DOWN unlock at the rig work exactly as before.
    // Locking is refused while someone at the rig is in a menu, an editor, Testing, Calibration or Sensor Diag:
    // the lock screen would take their buttons mid-edit or mid-hold, and automation waits while uiMode isn't
    // UI_DATA. Unlocking is never refused. Privileged accounts only (RTDB rules).
    if (!strcmp(c.type, "LCD_LOCK")) {
      bool lock = !strcmp(c.arg, "lock");
      if (!lock && strcmp(c.arg, "unlock")) { firebaseQueueStatus(c.id, "rejected", "action must be lock or unlock"); continue; }
      if (lock && uiMode != UI_DATA) { firebaseQueueStatus(c.id, "rejected", "someone is using the LCD menu"); continue; }
      if (lcdLocked != lock) {
        lcdLocked = lock; saveLock();
        logEvent("FIREBASE", "STATE", lock ? "LCD_LOCK|WEB" : "LCD_UNLOCK|WEB");
      }
      fbRequestPush();                                   // the website shows the lock from ESP1's snapshot
      firebaseQueueStatus(c.id, "completed", lock ? "screen locked" : "screen unlocked");
      continue;
    }

    if (!strcmp(c.type, "REBOOT")) {
      if (!strcmp(c.arg, "nano")) {
        if (levelRelayActive() || servoReqActive) {     // mid-pump: the Nano is the overflow watch and the servo
          firebaseQueueStatus(c.id, "rejected", "a pump is running or the pump selector is moving"); continue;
        }
        sendNanoCommand("RESET_REQ");
        nanoResetReqInFlight = true; nanoResetReqMs = millis(); nanoResetsToday++;
        logEvent("FIREBASE", "CMD", "REBOOT|NANO");
        firebaseQueueStatus(c.id, "completed", "nano soft reset requested");
      } else if (!strcmp(c.arg, "esp2")) {
        { const char *busy = rebootBusyReason(); if (busy) { firebaseQueueStatus(c.id, "rejected", busy); continue; } }
        sendEsp2("RESET_SELF");                          // soft first; the ladder power-cycles if it stays dark
        esp2SoftResetTried = true; esp2RecoverMs = millis();
        logEvent("FIREBASE", "CMD", "REBOOT|ESP2");
        firebaseQueueStatus(c.id, "completed", "esp2 soft reset sent");
      } else if (!strcmp(c.arg, "esp1")) {
        { const char *busy = rebootBusyReason(); if (busy) { firebaseQueueStatus(c.id, "rejected", busy); continue; } }
        // BOOTLOOP GUARD. firebaseFlushStatus() is core-0 and queued, so restarting now can beat the
        // status PATCH: the node would stay "queued", the poller would see it again after boot and
        // reboot forever. Persist the id so the post-boot poll skips it even if the PATCH never
        // lands, THEN mark it, and let rebootReqMs wait for the queue to drain before restarting.
        prefs.putString("rbcmd", c.id);
        firebaseQueueStatus(c.id, "completed", "rebooting esp1");
        logEvent("FIREBASE", "CMD", "REBOOT|ESP1");
        rebootReqMs = millis();                            // loop() restarts once the status is flushed
      } else {
        firebaseQueueStatus(c.id, "rejected", "target must be nano/esp1/esp2");
      }
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
    // a held fault, Testing/Calibration, or another in-flight remote test. actuationsDisabled and
    // fcPhase mirror TEST_PULSE's own gate. Deliberately NOT checking webManualHeld() here, unlike
    // FORCE_RUN/sendWorkOrder: the website's transferPumpBtn/boosterPumpBtn/mixerBtn (the ONLY UI
    // path that ever sends this command) live inside the Manual/Test tab's #mtControls, which is
    // only reachable once ESP1 has already granted that exact hold -- so requiring !webManualHeld()
    // here made this command permanently unreachable through the only client that sends it. (Traced
    // and confirmed via a full DOM audit before removing this -- see the plan file's L2/M1 findings.)
    if (sysState != IDLE_STATE || uiMode != UI_DATA || wo.active || pendingRun.active ||
        pendingExercise.active || esp2Held || fbRemoteExerciseId[0] ||
        actuationsDisabled || fcPhase != FP_NONE) {
      const char *why = (sysState != IDLE_STATE || wo.active || pendingRun.active) ? "a run is in progress"
                       : esp2Held ? "a fault is held"
                       : (pendingExercise.active || fbRemoteExerciseId[0]) ? "a test is already running"
                       : actuationsDisabled ? "actuations are disabled"
                       : fcPhase != FP_NONE ? "a forced run is armed"
                       : "someone is using the LCD";
      firebaseQueueStatus(c.id, "rejected", why);
      logEvent("FIREBASE", "REJECT", String("NOT_IDLE|") + why);
      continue;
    }
    { const char *bw;                                   // the pump exercise always runs the inverter
      if (secBattBlocksAc(&bw)) { firebaseQueueStatus(c.id, "rejected", bw); logEvent("FIREBASE", "REJECT", "EXERCISE|BATT2"); continue; } }

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
    firebaseQueueStatus(c.id, "accepted", (String(exerciseRunS) + " second pump test started").c_str());
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
  if (fbReplyLogPending) {                          // Firebase refused a report / its text was cleaned (18 Nov 2026 g)
    logEvent("ESP1", "FBASE", fbReplyLog);           // copied before the flag clears: core 0 writes only while it is clear
    fbReplyLogPending = false;
  }
  if (fbPollErrLogPending) {                        // command-poll diagnostics, deferred from core 0
    fbPollErrLogPending = false;
    logEvent("FIREBASE", "POLL", fbPollErrLog);
  }
  static int  lastState = -1;                       // -1 unknown, 0 fail, 1 ok
  static unsigned long lastLineMs = 0;              // 0 = none yet
  // fbUploadDone, not fbLastUploadMs (18 Nov 2026 c): the latter is stamped when a call STARTS, so this used to read the
  // previous call's result while the new request was still on the wire.
  static uint32_t lastSeenDone = 0;
  uint32_t done = fbUploadDone;
  if (done == lastSeenDone) return;                 // no call has finished since the last check
  lastSeenDone = done;
  // netMux: the core-0 admin form may be rewriting these while this core-1 tick reads them.
  // Unconfigured/disabled is not a failure -- don't log a FAIL line for a feature that is simply off.
  bool configured;
  const char *urlLabel;   // captured under the same lock so the log line below needs no second read
  xSemaphoreTake(netMux, portMAX_DELAY);
  configured = (firebaseEnabled && fbUrl.length() >= 8);
  urlLabel = (fbUrl.length() >= 8) ? "set" : "unset";
  xSemaphoreGive(netMux);
  if (!configured) { lastState = -1; return; }      // re-arm so the first real result still logs
  int now = fbLastOk ? 1 : 0;
  if (now == lastState) return;
  // At most one line per FB_LOG_TRANSITION_MIN_MS (18 Nov 2026 c). lastState is left as it was, so a change that lasts
  // is still written once the gap is over, and n= / fail= below count everything in between.
  if (lastLineMs && millis() - lastLineMs < FB_LOG_TRANSITION_MIN_MS) return;
  lastLineMs = nzMs(millis());
  lastState = now;
  uint32_t stackFree = netTaskHandle ? uxTaskGetStackHighWaterMark(netTaskHandle) : 0;
  // auth= and url= make an n=0 row self-explanatory: with no attempt counted, the cause is
  // always upstream of the request, and it is one of these two. skip= (18 Nov 2026 c) says which,
  // when nothing was sent: off, wifi, no-snapshot, memory or login.
  const char *skip = fbLastSkip;
  logEvent("ESP1", "FBASE", String(now ? "OK" : "FAIL") + "|http=" + String(fbLastHttp) +
                            (skip[0] ? String("|skip=") + skip : String("")) +
                            "|auth=" + String(fbAuthOk ? "ok" : "no") +
                            "|url=" + String(urlLabel) +
                            "|heap=" + String(fbLastHeap) + "|blk=" + String(fbLastBlock) + "|netstk=" + String(stackFree) +
                            "|n=" + String(fbAttempts) + "|fail=" + String(fbFailures));
}

/* ISRG Root X1 (Let's Encrypt's root; https://letsencrypt.org/certs/isrgrootx1.pem, valid to
 * 2035-06-04) -- extracted live from supabase.co's own TLS chain during the 2026-09-05 security
 * audit (leaf CN=supabase.co -> Let's Encrypt R-series intermediate -> this root), not typed from
 * memory.
 *
 * NOT currently wired in (see setInsecure() below, not setCACert(SUPA_ROOT_CA)) -- kept here,
 * unused, for a future pass. Final pre-flash review (2026-09-07) could not verify this chain
 * against this project's OWN <ref>.supabase.co storage subdomain: that hostname lives only in NVS,
 * set by SMS/portal, never in source, so there was nothing to test against from this environment.
 * Pinning a guessed root on a one-shot flash risks silently breaking the Supabase upload outright
 * (any mismatch = every upload fails TLS verification, not a graceful fallback) for a feature
 * (optional SD-log cloud backup) that has zero effect on irrigation/fertigation safety either way --
 * so this reverts to the same setInsecure() the currently-flashed firmware has always used here,
 * rather than trade a real, working, unauthenticated upload for a maybe-broken authenticated one.
 * To safely enable later: read the device's actual configured Supabase URL (LCD/portal/NVS dump),
 * open a TLS connection to that exact host, print the chain it presents, confirm it matches this
 * constant (or replace it with whatever the real chain is), THEN switch the call below. */
const char SUPA_ROOT_CA[] = R"EOF(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

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
  // Close Firebase's kept-alive TLS session first, so only one is ever open (~40-50 KB each); its next
  // request simply re-handshakes. Then the same heap floor Firebase uses: a skipped upload just retries.
  fbResetTls();
  if (heap8Free() < FB_TLS_MIN_FREE || heap8Block() < FB_TLS_MIN_BLOCK) { uploadHttp = -3; return false; }   // 18 Nov h

  if (!sdTake(4000)) { uploadHttp = -1; return false; }            // SD busy (portal/summary) -> retry later
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) { if (f) f.close(); sdGive(); uploadHttp = -2; return false; }
  size_t len = f.size();

  WiFiClientSecure client;
  client.setInsecure();   // encrypted, unauthenticated -- see the SUPA_ROOT_CA comment above for why
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
        if (up.endsWith(".CSV")) {
          // htmlEscape() everywhere this is rendered, matching every other dynamic string in this
          // portal -- '&' and '\'' are valid FAT filename characters (unlike '<'/'>'/'"'), so a
          // crafted on-card filename like "x' onmouseover='..." could otherwise break out of the
          // href attribute. This was the one gap; every other rendered string here already escapes.
          String enm = htmlEscape(nm);
          o += "<tr><td>" + enm + "</td><td align=right>" + String((uint32_t)e.size()) + "B</td>"
               "<td><a href='/view?f=" + enm + "'>view</a> &middot; <a href='/download?f=" + enm + "'>get</a></td></tr>";
        }
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
  // netMux: supaUrl/supaKey are also written from core-1 handleSms(); this admin page runs on
  // core 0. Copy once, use the locals for all three reads below.
  String suUrl, suKey;
  xSemaphoreTake(netMux, portMAX_DELAY); suUrl = supaUrl; suKey = supaKey; xSemaphoreGive(netMux);
  o += "<h3>Supabase cloud logs</h3><form method=POST action=/supa>"
       "<p>Project URL<br><input name=url style='width:100%' placeholder='https://<ref>.supabase.co' value='" + htmlEscape(suUrl) + "'></p>"
       "<p>Service key (service_role JWT)<br><textarea name=key rows=3 style='width:100%' placeholder='" + String(suKey.length() ? "(set - leave blank to keep)" : "paste the full key") + "'></textarea></p>"
       "<p><button type=submit style='width:100%;padding:10px'>Save Supabase creds</button></p></form>";
  o += "<form method=POST action=/upload>"
       "<p>Push yesterday's CSV to cloud storage" + String(suUrl.length() ? "" : " (set the URL + key above first)") + ".</p>"
       "<p><button type=submit style='width:100%;padding:10px'>Upload logs now</button></p></form>";
  // netMux: fbUrl/fbApiKey/fbEmail/fbPassword/fbRefresh/fbAuthErr/fbUid are also written from
  // core-1's FBASE,... SMS handlers (SIGNOUT clears several of them) while this admin page render
  // runs on core 0 -- the same cross-core String race already guarded at the FBASE,STATUS SMS
  // handler, just not previously extended to this render path (audit fix, 2026-09-08).
  String fUrl, fApiKey, fEmail, fPass, fRefresh, fAuthErr, fUid;
  xSemaphoreTake(netMux, portMAX_DELAY);
  fUrl = fbUrl; fApiKey = fbApiKey; fEmail = fbEmail; fPass = fbPassword;
  fRefresh = fbRefresh; fAuthErr = fbAuthErr; fUid = fbUid;
  xSemaphoreGive(netMux);
  o += "<h3>Firebase live dashboard</h3><form method=POST action=/fbase>"
       "<p>RTDB URL<br><input name=url style='width:100%' placeholder='https://<ref>-default-rtdb.<region>.firebasedatabase.app' value='" + htmlEscape(fUrl) + "'></p>"
       "<p><label><input type=checkbox name=en value=yes" + String(firebaseEnabled ? " checked" : "") + "> Enabled (live snapshot + remote commands)</label></p>"
       "<p>Last upload: " + String(fUrl.length() == 0 ? "not configured"
                                   : (fbLastUploadMs == 0 ? "none yet"
                                      : String((millis() - fbLastUploadMs) / 1000) + " s ago, " + (fbLastOk ? "OK" : "FAILED")
                                         + (fbLastHttp ? " (HTTP " + String(fbLastHttp) + ")" : ""))) + "</p>"
       "<p><button type=submit style='width:100%;padding:10px'>Save Firebase settings</button></p></form>";
  // Device account. The password is write-only from here: it is never rendered back, and once a
  // refresh token exists it is no longer needed at all (Forget password below).
  o += "<h3>Firebase device account</h3><form method=POST action=/fbauth>"
       "<p>Web API key<br><input name=key style='width:100%' placeholder='" +
         String(fApiKey.length() ? "(set - leave blank to keep)" : "AIza...") + "'></p>"
       "<p>Device email<br><input name=email style='width:100%' value='" + htmlEscape(fEmail) + "'></p>"
       "<p>Device password<br><input name=pass type=password style='width:100%' placeholder='" +
         String(fPass.length() ? "(set - leave blank to keep)" : "used once, then discardable") + "'></p>"
       "<p>Token: <b>" + String(fbAuthOk ? "signed in" : "NOT signed in") + "</b>" +
         String(fbAuthHttp ? " (HTTP " + String(fbAuthHttp) + ")" : "") +
         " &middot; refresh token " + String(fRefresh.length() ? "stored" : "none") +
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
    else if (fApiKey.length() < 8)  why = "no Web API key set";
    else if (!fEmail.length())      why = "no device email set";
    else if (!fRefresh.length() && !fPass.length()) why = "no password and no refresh token";
    else if (fUrl.length() < 8)     why = "no RTDB URL set -- sign-in will work, but nothing will publish";
    if (why.length())      o += "<p style='color:#c33'>Blocked: " + htmlEscape(why) + "</p>";
    if (fAuthErr.length()) {
      o += "<p style='color:#c33'>Last error (" + String((millis() - fbAuthErrMs) / 1000) + " s ago): "
           + htmlEscape(fAuthErr) + "</p>";
    }
    if (fUid.length())
      o += "<p>Device UID: <code>" + htmlEscape(fUid) + "</code><br>"
           "<small>Use this in your RTDB rules, e.g. <code>\".write\": \"auth.uid === '" + htmlEscape(fUid) + "'\"</code>. "
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
  // 9+ digits: senderIsOwner() compares the last 9, so fewer locks the owner out. Dialable only: anything
  // else (spaces, dashes) is refused by the modem driver, and every alert to the owner would be dropped.
  if (digits < 9 || o.length() > 15 || !gsmNumberOk(o.c_str())) { portalServer.send(200, "text/html", portalAdminHtml("Invalid number (9-15 digits, optional leading +).")); return; }
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
  // Was a C-style for-loop (`for (File e = ...; e && cnt < MAXN; e = d.openNextFile())`) -- the
  // increment clause runs BEFORE the condition is re-checked, so once cnt reached MAXN the loop
  // still opened one more (49th) File before the condition failed and the loop exited, and that
  // File was never closed (the body, with its e.close(), never ran for it). A >48-entry directory
  // is a realistic outcome after ~7 weeks of daily logging, and Format SD calls this in a loop
  // specifically to walk large directories, so this was reachable in normal long-term use. Rewritten
  // to never call openNextFile() again once the buffer is full.
  for (File e = d.openNextFile(); e; ) {
    String nm = String(e.name());                             // normalize to basename (core may return a full path)
    int sl = nm.lastIndexOf('/'); if (sl >= 0) nm = nm.substring(sl + 1);
    if (nm.length() && cnt < MAXN) { child[cnt] = (dir == "/") ? "/" + nm : dir + "/" + nm; isdir[cnt] = e.isDirectory(); cnt++; }
    e.close();
    if (cnt >= MAXN) break;
    e = d.openNextFile();
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
  { const char *busy = rebootBusyReason();
    if (busy) { portalServer.send(200, "text/html", portalAdminHtml(String("Not rebooted: ") + busy + ". Stop or cancel it first.")); return; } }
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
  xSemaphoreTake(netMux, portMAX_DELAY); bool haveKey = supaKey.length() > 0; xSemaphoreGive(netMux);
  bool keepKey = (key.length() == 0 && haveKey);  // blank key + one already stored -> URL-only edit
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
                 String(portalServer.arg("gap").toInt()), "Thresholds saved (reload to confirm).");
}
static void portalStart() {
  // Drop the Firebase TLS session first (18 Nov 2026 g): its ~40 KB of mbedTLS buffers stayed allocated through the whole
  // portal session, next to the AP, the DNS and web servers and the WiFi scan -- on a heap at ~68 KB free with a 40 KB
  // largest block. On 18 Nov evening the portal was opened ~40 times and ESP1 rebooted with PANIC / TASK_WDT 7 times,
  // each within two minutes of opening (or cancelling) it. Nothing uses the session while the portal is up.
  fbResetTls();
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
// WiFi power save: the core's default (modem sleep) again since 18 Nov 2026 g. 18 Nov f kept the radio awake
// (WiFi.setSleep(false), ~80-100 mA more) without a measurable gain; the SD card shows 1-8 % failed reports a day on
// 10-17 Nov with the default at -84..-89 dBm. Back to what was proven.
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
    netBeat++;                 // liveness for netHealthTick() on core 1 -- see the WDT note there
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

    // Battery-switch quiet window (set by core 1): start no new WiFi/TLS request while P0 moves.
    if (battQuietActive()) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

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
    if (fbTlsResetPending) { fbTlsResetPending = false; fbResetTls(); }

    // ThingSpeak: one pace in every mode (18 Nov 2026 e; it was every 20 s during a run).
    if (millis() - lastUpload >= TS_UPLOAD_IDLE_MS) { lastUpload = millis(); tsUpload(); }
    // The link mode for this pass (LINK PACE). linkBusyFlag is a plain bool written by core 1 once a second.
    const LinkMode lmode = linkModeNow();
    const bool     lbusy = lmode == LINK_BUSY;

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

    unsigned long fbInterval = linkPaceMs(lmode);          // LINK PACE: 60 s idle, 30 s watched, 60 s busy
    // An error should not sit unseen for up to a minute, so core 1 can force a push out of band.
    // Rate-limited: a burst of rejections must not turn into a burst of TLS uploads on a battery.
    // It waits for a snapshot taken after the request (fbPushSeq), at most FB_PUSH_FRESH_WAIT_MS, so
    // a stalled core 1 cannot hold it back for good. Requests that land after that snapshot stay
    // pending, and get their own push once the gap allows.
    static uint32_t      pushedSeq  = 0;               // the newest request a push has already covered
    static unsigned long pushWaitMs = 0;               // when the wait for a fresh snapshot began (0 = none)
    bool urgent = false;
    uint32_t wantedSeq = fbPushSeq;
    if (wantedSeq != pushedSeq && millis() - fbLastUploadMs >= FB_PUSH_MIN_GAP_MS) {
      uint32_t coveredSeq;
      portENTER_CRITICAL(&telemMux); coveredSeq = telem.pushSeq; portEXIT_CRITICAL(&telemMux);
      if (!pushWaitMs) pushWaitMs = nzMs(millis());
      // Newer, not just different: after a timed-out wait pushedSeq can be ahead of the snapshot in hand.
      bool fresh = (int32_t)(coveredSeq - pushedSeq) > 0;
      if (fresh || millis() - pushWaitMs >= FB_PUSH_FRESH_WAIT_MS) {
        urgent = true;
        pushedSeq = fresh ? coveredSeq : wantedSeq;
        pushWaitMs = 0;
      }
    }
    // The dashboard's Refresh button: a fresh report now (the snapshot core 1 refreshes every second). While BUSY at
    // most every FB_REFRESH_GAP_BUSY_MS; a request inside the gap stays pending and is answered when the gap is over.
    if (fbRefreshPending && millis() - fbLastUploadMs >= (lbusy ? FB_REFRESH_GAP_BUSY_MS : FB_REFRESH_GAP_IDLE_MS)) {
      fbRefreshPending = false; urgent = true;
    }
    // A report that FAILED goes again after FB_UPLOAD_RETRY_MS, up to FB_UPLOAD_RETRIES times in a row (18 Nov 2026 c),
    // instead of at the next tick; not while BUSY (18 Nov 2026 e). A call that sent nothing (no WiFi, no token, low
    // memory) is not retried early: its own cause has to clear first, and the regular tick tries again.
    static uint8_t       upRetries = 0;
    static unsigned long upRetryAt = 0;                    // 0 = none due
    static uint8_t       netFails  = 0;                    // reports failed in a row with a network error (recovery)
    bool retryDue = upRetryAt && (long)(millis() - upRetryAt) >= 0;
    if (urgent || retryDue || millis() - fbLastUploadMs >= fbInterval) {
      uint32_t triedBefore = fbAttempts;
      bool sent = firebaseUploadLive();
      bool tried = fbAttempts != triedBefore;
      upRetryAt = 0;
      if (sent) upRetries = 0;
      else if (tried && !lbusy && upRetries < FB_UPLOAD_RETRIES) { upRetries++; upRetryAt = nzMs(millis() + FB_UPLOAD_RETRY_MS); }
      // Recovery (18 Nov 2026 e). A network error is -1..-99 (HTTPClient; -100/-101 are this side's own: an
      // over-full document or no free block). Drop the session every FB_RESET_AFTER_FAILS of them, so the next request
      // opens a fresh one, and leave WiFi after FB_REJOIN_AFTER_FAILS: the top of this loop joins it again.
      int code = fbLastHttp;
      if (sent) netFails = 0;
      else if (tried && code < 0 && code > -100) {
        netFails++;
        if (netFails >= FB_REJOIN_AFTER_FAILS) {
          netFails = 0; fbWifiRejoins++;
          fbResetTls();
          WiFi.disconnect();
          vTaskDelay(pdMS_TO_TICKS(500));
          continue;                                        // the top of the loop reconnects
        }
        if (netFails % FB_RESET_AFTER_FAILS == 0) { fbLinkResets++; fbResetTls(); }
      }
    }
    // Remote-command transport, both directions. Cheap: they ride the SAME keep-alive TLS
    // socket as the upload above, so a 3 s poll costs one small GET, not a handshake.
    firebaseFlushStatus();                                 // results first: report before fetching more
    firebasePollCommands();
    firebasePollManual();                                  // the Manual/Test tab's hold + keep-alive
    firebasePollTestHold();                                // the Manual/Test tab's hold-to-run buttons
    firebasePollViewer();                                  // someone watching / a Refresh (2026-11-17)

    // CSV log push: core 1 sets uploadReqStamp. Firebase (2026-11-17) goes one piece per pass (fbLogJobStep below);
    // Supabase, only while Firebase is off, is one streamed attempt here.
    if (uploadReqStamp != 0 && !uploadBusy) {
      uploadBusy = true;
      long stamp = uploadReqStamp;
      char path[16], obj[13];
      snprintf(path, sizeof(path), "/%08ld.CSV", stamp);
      snprintf(obj,  sizeof(obj),  "%08ld.CSV", stamp);
      bool toFb;
      xSemaphoreTake(netMux, portMAX_DELAY); toFb = firebaseEnabled && fbUrl.length() >= 8; xSemaphoreGive(netMux);
      uploadWasFb = toFb;
      if (toFb) fbLogJobBegin(stamp, path, uploadReqPartial);
      else {
        bool ok = supaUploadFile(path, obj);
        uploadResStamp = stamp; uploadResult = ok ? 1 : 2;
        uploadReqStamp = 0; uploadBusy = false;
      }
    }
    {   // one log piece per FB_LOG_STEP_MIN_MS at most, as when every pass ended with a 500 ms wait
      static unsigned long lastLogStepMs = 0;
      if (fbLogJob.active && millis() - lastLogStepMs >= FB_LOG_STEP_MIN_MS) { lastLogStepMs = millis(); fbLogJobStep(); }
    }
    firebasePollLogRequest();                              // the website's "Upload from the rig" (every 10 s)
    vTaskDelay(pdMS_TO_TICKS(500));                        // one pass every 500 ms in every mode (100 ms while watched, 18 Nov b-d)
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
  // Same for Calibration. It has no confirm dialogs, so the preempt below always closes it; left running,
  // ESP2 stays powered with its CAL stream live, and it then refuses the next work order as BUSY,CAL until
  // the retries run out and RECOVERY_STATE resets it.
  if (uiMode == UI_CAL) { if (calOpen) calCloseTarget(); esp2SetPower(false); }
  // Non-critical faults wait BEHIND an open confirm dialog (companion spec §B.3.1); Critical
  // force-dismisses (handled in enterEmergencyStop below). resetConfirm (the Reboot-ESP1 double-
  // confirm) is handled identically to restoreConfirm everywhere else in settingsButton() and is
  // force-cleared alongside it on a Critical fault / fault-hold -- it belongs in this condition too,
  // or a non-critical fault preempts the screen without clearing it, and the stale dialog silently
  // reappears out of context the next time Settings is reopened.
  if (tier == 'C' || !(editConfirm || restoreConfirm || resetConfirm)) { uiMode = UI_DATA; lcdPage = PAGE_FAULT; }
  wakeBacklight();

  // GSM alert for all tiers (sec.12.1.1), in words (smsFault)
  smsFault(tier, code, loc);

  if (tier == 'C') enterEmergencyStop(true);     // critical -> shutdown (sec.14.7.1)
}

void enterEmergencyStop(bool cutPower) {
  // stop all actuators (sec.22.3) then optionally cut ESP2 power (sec.23.1.1)
  bswNoteCut(1, -1, 0);                                // before STOP_ALL: it may be the last thing we do
  if (uiMode == UI_CAL && calOpen) calCloseTarget();   // the Nano keeps its CAL stream otherwise
  esp2Serial.print(FRAME_START); esp2Serial.print(",STOP_ALL,"); esp2Serial.println(FRAME_END);
  logEvent("ESP1", "CMD", "ESP2|STOP_ALL");
  // Let STOP_ALL actually reach ESP2 before the cut below (~20 ms on the wire, then ESP2 credits the running
  // stage's litres to its NVS tank volume). Written straight to UART and cut at once, it never arrived, and
  // the next fill could overfill. One-shot, relays are cut right after, and it is far inside the 8 s WDT.
  if (cutPower) { esp2Serial.flush(); delay(40); }
  esp2Held = false;                                // a hard E-stop supersedes any held-fault recovery
  lastHeldClearReason = "estop";
  // One of the four documented esp2Held-clear sites; previously the only one (besides the
  // ESP2-restarted branch, now also fixed) that left the re-hold guard stale for a later,
  // unrelated fault sharing the same code to inherit.
  lastHoldCode = ""; sameHoldN = 0; steerRelease = false;
  wo.active = false; wo.stage = WO_IDLE;
  pendingRun.active = false; pendingRun.colIdx = -1;
  pendingExercise.active = false; pendingExercise.idx = -1; pendingExercise.sent = false;
  firebaseRemoteExerciseDone("failed", "aborted by emergency stop");   // release the remote slot
  if (fcPhase != FP_NONE) { fcPhase = FP_NONE; logEvent("ESP1", "ACT", "FORCE|ABORT|ESTOP"); }
  // An E-stop is exactly when the dashboard must not keep showing "accepted" for a run that has
  // just been killed, and a stale forceMask would mis-attribute the next run's DONE. The cancel
  // path already does this; the E-stop path did not.
  if (forceCmdId[0]) { firebaseQueueStatus(forceCmdId, "failed", "aborted by emergency stop"); forceCmdId[0] = 0; }
  forceMask = 0;
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
  // Count consecutive identical holds; after 3 FLOW_FAILs steer to Release (flow-independent on ESP2).
  if (lastHoldCode == code) sameHoldN++; else { lastHoldCode = code; sameHoldN = 1; }
  // SMS detail in words: "Col A fertigation, transfer, pulses=0".
  String smsColOp = (wo.colIdx >= 0 && wo.colIdx < NUM_COLUMNS ? String("Col ") + COL_TAG[wo.colIdx] : String("Col ?")) +
                    " " + (wo.fertigate ? "fertigation" : "irrigation");
  String smsDetail = smsColOp + ", " + smsPlain(loc);
  // Circuit breaker. Steering only moves the cursor -- an operator who keeps picking "Resume normal"
  // against dead hardware re-holds forever, which is exactly the state that forced a physical reset
  // with the pumps still live. Give up on our own and snooze the column instead.
  if (sameHoldN >= HOLD_AUTOCANCEL_N) {
    logEvent("ESP1", "FAULT", String("CRIT|") + code + "|" + loc + "|AUTOCANCEL|after=" + String(sameHoldN));
    { String t = smsClock();
      smsAlert(smsCompose("ALERT: Run cancelled automatically",
                          smsColOp + ": " + smsFaultWhat(code) + ", " + String(sameHoldN) + " times in a row",
                          "Column skipped for 30 min. Fix it before the next run",
                          String("(") + (t.length() ? t + " " : String()) + code + ")")); }
    // Overwrite the plain fault line above with the actual outcome: esp2Held is about to go false and
    // the banner will simply disappear, so this is the only trace (besides SMS) of "the rig gave up on
    // its own" vs. a real operator-confirmed recovery -- see diagnostics.system.lastFault.
    lastFaultMsg = String("AUTO-CANCELLED ") + code + " " + loc + " " + colS + " (repeated " + sameHoldN + "x, snoozed 30m)";
    cancelRun(1, code, true);                          // 30-min snooze; clears the hold + stops ESP2; isAuto
    return;
  }
  // web v.4: a dead tank ultrasonic (LEVEL_BLIND) is the same dead-meter case -- Release runs on a timer.
  steerRelease = (sameHoldN >= 3 && (String(code) == "FLOW_FAIL" || String(code) == "LEVEL_BLIND"));
  // GSM alert WITH operation + column (sec.12.1.1 + user req), then the reply menu as its own text to the
  // owner (the second alert number cannot send commands).
  smsAlert(smsFaultText('C', code, smsDetail));
  sendSMS(smsHoldMenu(wo.colIdx, steerRelease));
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
void cancelRun(int mode, const char *whyIn, bool isAuto) {
  // A copy: callers pass lastHoldCode.c_str(), which is cleared below (the log said "CANCEL|OFF|COL_A|", 2026-11-17).
  const String whyS = whyIn ? whyIn : "";
  const char *why = whyS.c_str();
  int c = (wo.colIdx >= 0) ? wo.colIdx : pendingRun.colIdx;      // whichever run we are aborting
  bswNoteCut(2, c, mode);                                        // before STOP_ALL: it may be the last thing we do

  // STOP_ALL is what actually kills the pumps: on ESP2 it clears faultHeld, the work order, the
  // step and the timed-recovery latch, so nothing can resume behind our back.
  sendEsp2("STOP_ALL");
  esp2Serial.flush(); delay(40);                                 // see enterEmergencyStop: the power-off below may be immediate

  wo.active = false; wo.stage = WO_IDLE; wo.colIdx = -1;
  pendingRun.active = false; pendingRun.colIdx = -1;
  esp2Held = false;
  lastHeldClearReason = isAuto ? "auto_cancelled" : "cancelled";
  // A cancelled FORCE run must not leave the dashboard sitting on "accepted", nor let a later
  // scheduled run inherit this one's column mask.
  if (forceCmdId[0]) { firebaseQueueStatus(forceCmdId, "failed", why); forceCmdId[0] = 0; }
  forceMask = 0;
  if (fcPhase != FP_NONE) { fcPhase = FP_NONE; logEvent("ESP1", "ACT", "FORCE|CANCELLED"); }
  lastHoldCode = ""; sameHoldN = 0; steerRelease = false;        // a cancel resolves the re-hold loop
  cancelPrompt = false; cancelSel = 0;

  String scope;
  if (c >= 0 && c < NUM_COLUMNS) {
    // mode < 0: stop the run WITHOUT penalising the column. Used by the actuation lockout, which is
    // a system-wide decision -- the column did nothing wrong, and a snooze would outlive the lockout
    // and silently skip it for another 10 minutes after the operator re-enables.
    if      (mode < 0)  { scope = "NOPENALTY"; }
    else if (mode == 0) { COLUMN_ENABLED[c] = false; saveColEnable(c); scope = "OFF"; }
    else if (mode == 1) { colSnoozeUntil[c] = millis() + SNOOZE_LONG_MS;  scope = "SNOOZE30"; }
    else                { colSnoozeUntil[c] = millis() + SNOOZE_SHORT_MS; scope = "SNOOZE10"; }
  } else scope = "NOCOL";                                        // nothing was running -- still a clean stop

  String colS = (c >= 0 && c < NUM_COLUMNS) ? String("COL_") + COL_TAG[c] : String("COL_?");
  logEvent("ESP1", "ACT", String("CANCEL|") + scope + "|" + colS + "|" + why);
  if (!isAuto) {                                                 // an auto-cancel already sent its own alert
    bool hasCol = (c >= 0 && c < NUM_COLUMNS);
    const char *what = !hasCol ? "Nothing was running. All stopped."
                     : mode < 0 ? "Stopped because pumps were disabled."
                     : mode == 0 ? "Column turned OFF. Turn it on again on the LCD or website."
                     : mode == 1 ? "Column skipped for 30 min." : "Column skipped for 10 min.";
    sendSMS(String("OK: Run cancelled") + (hasCol ? ", " + smsCol(c) : String()) + "\n" + what);
  }

  runUiAbort("CANCELLED");                                       // release the run screen
  esp2SetPower(false);                                           // OFF-during-idle
  setState(IDLE_STATE);
  uiMode = UI_DATA; lcdPage = PAGE_HOME; wakeBacklight();
}

// Apply a recovery choice (from LCD ENTER or an SMS reply): 0 Hold, 1 Release, 2 OnlyIrr, 3 Normal.
void issueRecovery(int sel) {
  if (!esp2Held) return;
  if (sel == 0) {                                      // Hold / acknowledge: stay held, push nothing
    sendSMS("OK: Still on hold, tank kept.\nReply NORMAL, IRRIGATE or RELEASE when ready, or CANCEL,30 to end the run.");
    // Visible acknowledgement. Staying held is correct behaviour, but with no feedback at all this
    // button was indistinguishable from a dead one.
    lastFaultMsg = String("HELD - waiting");
    wakeBacklight();
    return;
  }
  const char *mode = (sel == 1) ? "RELEASE" : (sel == 2) ? "IRRIGATE" : "NORMAL";
  // A RESUME already sent is still awaiting ESP2's reply -- don't send a second one on top of it.
  // A duplicate (double button press, a retried SMS) racing the first would make ESP2 correctly
  // reject the second as INVALID,NOT_HELD once the first has already been accepted, which used to
  // make ESP1 wrongly conclude "ESP2 restarted, lost the sequence" even though ESP2 was still
  // executing the successfully-resumed run. resumeAwaitingAck already clears on ACK, on a rejected
  // INVALID,NOT_HELD, and on the RESUME_ACK_TIMEOUT_MS no-reply timeout below, so this can't wedge.
  if (resumeAwaitingAck && millis() - resumeSentMs <= RESUME_ACK_TIMEOUT_MS) {
    lastFaultMsg = "Resume already in progress - waiting for ESP2";
    return;
  }
  // Every resume mode drives the AC pumps. Stay held (the tank volume is kept) until the secondary
  // recovers; Hold and Cancel stay available.
  { const char *bw;
    if (secBattBlocksAc(&bw)) {
      lastFaultMsg = String("Resume refused: ") + bw;
      smsErr("RESUME|BATT2", String("NOT DONE: Can't resume yet: ") + bw + ".\nStill on hold, tank kept.");
      logEvent("ESP1", "ACT", String("RESUME|BLOCKED|BATT2|") + mode);
      return;
    } }
  sendEsp2(String("RESUME,") + mode);                  // ESP2 re-energizes P17 and resumes/rewrites the run
  logEvent("ESP1", "ACT", String("RESUME|SENT|") + mode + "|COL_" +
           (wo.colIdx >= 0 ? String(COL_TAG[wo.colIdx]) : String("?")));
  // Do NOT clear esp2Held (or restart DONE-timeout supervision, or claim success by SMS) here --
  // that must wait for ESP2's actual reply: ACK,RESUME (handleEsp2Response, confirmed) or
  // INVALID,NOT_HELD (rejected). Clearing esp2Held on SEND instead of on CONFIRMATION used to make
  // the INVALID,NOT_HELD rejection handler unreachable, silently re-arming the 10-min DONE-timeout
  // supervision and escalating to an unexplained Emergency Stop ~10 minutes later. The fault-hold UI
  // (and this menu) now stays up until ESP2 actually confirms one way or the other, or the bounded
  // RESUME_ACK_TIMEOUT_MS wait in pollESP2() reports that nothing came back at all.
  resumeAwaitingAck = true;
  resumeSentMs = millis();
}

/* =============================================================================
 *  CALIBRATION MODE UI  (companion spec §A)  -- IDLE-only, automation suspended
 * ========================================================================== */
static bool calTgtVisible(int idx) {
  const CalTgt &t = CAL_TGTS[idx];
  if (t.kind == CK_SOIL || t.kind == CK_NPK) return COLUMN_ENABLED[t.col];  // hide disabled columns (§14.1.4.1)
  return true;
}
// Soil rows are one probe each: "SOIL_A1" -> probe 0, "SOIL_A2" -> probe 1 (the Nano's SOIL_PINS[col][probe]).
static int calSoilProbe(const CalTgt &t) { return (t.id[6] == '2') ? 1 : 0; }
static bool calIsNano(uint8_t kind) {                    // Nano-owned sensors (ESP1 applies their cal)
  return kind == CK_SOIL || kind == CK_LEVEL_RES || kind == CK_LEVEL_MIX || kind == CK_OFFSET ||
         kind == CK_NPK || kind == CK_FLOW_RES || kind == CK_VOL_MIX || kind == CK_VOL_RES;
}
// (FLOW_RESMIX / FLOW_MIXIRR primes went with their calibration rows in web v.4. The RESMIX / MIXIRR prime
//  lines themselves still exist on ESP2: the "Tank Mix liters" screen runs them, see calHoldTick.)
static const char *calPrimeLine(const char *id, int *col) {
  *col = -1;
  if (!strcmp(id, "FLOW_NUTA"))   return "NUTA";
  if (!strcmp(id, "FLOW_NUTB"))   return "NUTB";
  if (!strcmp(id, "FLOW_NUTC"))   return "NUTC";
  if (!strcmp(id, "FLOW_NUTD"))   return "NUTD";      // ESP2 setupPrime(): OUT_NUT_D, DC pump, no valve
  if (!strcmp(id, "FLOW_PHUP"))   return "PHUP";      // OUT_PH_UP
  if (!strcmp(id, "FLOW_PHDN"))   return "PHDN";      // OUT_PH_DN
  return "";
}

// ---- Pump rate screens (CK_RATE, 2026-11-17) ----
// t.col: 0 transfer, 1 booster (L/min), 2..4 nutrient A..C (mL/min). 0 = not measured.
static bool rateIsWater(const CalTgt &t) { return t.col < 2; }
static float rateCurrent(const CalTgt &t) {
  if (t.col == 0) return calRateXfer;
  if (t.col == 1) return calRateBoost;
  int n = t.col - 2;
  return pumpRateSet[n] ? PUMP_FLOWRATE_MLPM[n] : 0.0f;
}
// One UP (+1) / DOWN (-1) step. Below the smallest accepted rate is 0 = "not measured"; UP from there starts at it.
static void rateNudge(const CalTgt &t, int dir, bool fast) {
  bool w = rateIsWater(t);
  float lo = w ? RATE_LPM_MIN : RATE_MLPM_MIN, hi = w ? RATE_LPM_MAX : RATE_MLPM_MAX;
  float step = w ? (fast ? 0.5f : 0.05f) : (fast ? 10.0f : 1.0f);
  float v = calVolL;
  if (dir > 0) v = (v < lo) ? lo : v + step;
  else { v -= step; if (v < lo - 0.001f) v = 0.0f; }
  if (v > hi) v = hi;
  float q = w ? 100.0f : 10.0f;
  calVolL = roundf(v * q) / q;
}
// UP/DOWN held: repeat after 0.6 s, and big steps after 2.5 s (same feel as the tank-litres entry).
static void rateHoldTick(const CalTgt &t) {
  if (calStep != 0) { volCalHeldBtn = -1; return; }
  bool up = (digitalRead(BTN_UP) == LOW), dn = (digitalRead(BTN_DOWN) == LOW);
  unsigned long now = millis();
  int b = up ? 0 : dn ? 1 : -1;
  if (b < 0 || (up && dn)) { volCalHeldBtn = -1; return; }
  if (b != volCalHeldBtn) { volCalHeldBtn = b; volCalHeldSinceMs = now; volCalRepeatMs = now; return; }
  if (now - volCalHeldSinceMs < 600 || now - volCalRepeatMs < 120) return;
  volCalRepeatMs = now;
  rateNudge(t, b == 0 ? +1 : -1, now - volCalHeldSinceMs >= 2500);
}

// ---- Pump estimate screens (CK_PEST, 2026-11-17 user request) ----
// "Transfer pump est" (t.col 0) / "Booster pump est" (t.col 1). Run the pump for a chosen number of seconds -- ESP2
// times it from the inverter SWITCH-ON, the way the user measured -- while the screen shows what the pump's line says
// that run gives. Weigh the water and type the grams (1 L = PL_G_PER_L g). From PL_MIN_TRIES tries on, the screen fits
// the pump's line; ENTER saves it: the rate as Rate Transfer / Rate Booster, the start litres here (and, for the
// transfer, on ESP2 as SET_CAL,PSTART -- every fill is timed by this line). Vary the seconds between tries.
// Steps: 0 booster valve (A/B/C), 1 seconds + run, 2 running, 3 type the grams, 4 the fit, 5 saved.
static bool pestIsXfer(const CalTgt &t) { return t.col == 0; }
static float pestEstL(const CalTgt &t, float tS) {
  bool x = pestIsXfer(t);
  return plLitresAt(tS, pestRate(x), pestStart(x));
}
static String pestPrimeCmd(const CalTgt &t) {
  String c = pestIsXfer(t) ? String("PRIME_START,RESMIX,") : String("PRIME_START,MIXIRR,") + COL_TAG[pestCol];
  return c + "," + String((unsigned long)pestSecs * 1000UL);
}
static void pestStop(const CalTgt &t) {
  if (!pestRunning) return;
  sendEsp2(pestIsXfer(t) ? "PRIME_STOP,RESMIX" : "PRIME_STOP,MIXIRR");
  pestRunning = false;
  actRearmStart();                                     // the next run waits ACT_REARM_MS
}
static void pestRun(const CalTgt &t) {
  if (actRearmWaiting())  { calMsg = "Too fast: wait 1.5s"; return; }
  if (actuationsDisabled) { calMsg = "Actuations off"; return; }
  if (secBattBlocksAc())  { calMsg = secBlockLcd(); return; }
  pestRunning = true; pestStartMs = millis(); pestKeepMs = 0; pestRanMs = -1; pestReseat = false; calMsg = "";
  pestInvOnMs = 0;                                     // the counter waits for the inverter's switch-on
  calStep = 2;
  logEvent("ESP1", "CAL", String("PEST_RUN|") + t.id + "|s=" + pestSecs +
                          (pestIsXfer(t) ? String("") : String("|col=") + COL_TAG[pestCol]));
}
// ESP2's answer: DONE,PRIME_TIMED,<line>,<ran ms>,<re-seat 0/1>,<ms to AC, -1 = none> -- or an early end.
void pestOnDone(const String &arg) {
  if (!pestRunning) return;
  pestRunning = false;
  actRearmStart();                                     // the next run waits ACT_REARM_MS
  if (uiMode != UI_CAL || !calOpen || CAL_TGTS[calSel].kind != CK_PEST) return;
  const CalTgt &t = CAL_TGTS[calSel];
  if (!arg.startsWith("PRIME_TIMED,")) { calStep = 1; calMsg = "Stopped early"; return; }
  int p1 = arg.indexOf(','), p2 = arg.indexOf(',', p1 + 1), p3 = arg.indexOf(',', p2 + 1), p4 = arg.indexOf(',', p3 + 1);
  if (p2 < 0 || p3 < 0) { calStep = 1; calMsg = "Bad answer"; return; }
  pestRanMs  = arg.substring(p2 + 1, p3).toInt();
  pestReseat = arg.substring(p3 + 1, p4 < 0 ? (int)arg.length() : p4).toInt() == 1;
  long ac    = (p4 > 0) ? arg.substring(p4 + 1).toInt() : 0;
  if (ac < 0) { calStep = 1; calMsg = "No AC: try again"; return; }   // the inverter never gave power: no water
  float est = pestEstL(t, pestRanMs / 1000.0f);
  pestGrams = (int)(roundf(est * PL_G_PER_L / 10.0f) * 10.0f);      // start from the estimate, rounded to 10 g
  calStep = 3; calMsg = pestReseat ? "Re-seated: BACK" : "";
  logEvent("ESP1", "CAL", String("PEST_RAN|") + t.id + "|ms=" + pestRanMs + "|ac_ms=" + ac + "|est=" + String(est, 2) +
                          (pestReseat ? "|RESEAT" : ""));
}
static void pestKeep(const CalTgt &t) {
  if (pestN >= PEST_MAX_TRIES) {                       // full: the oldest try makes room
    for (uint8_t i = 1; i < PEST_MAX_TRIES; i++) { pestT[i - 1] = pestT[i]; pestV[i - 1] = pestV[i]; }
    pestN = PEST_MAX_TRIES - 1;
  }
  pestT[pestN] = pestRanMs / 1000.0f;
  pestV[pestN] = pestGrams / PL_G_PER_L;
  pestN++;
  logEvent("ESP1", "CAL", String("PEST_TRY|") + t.id + "|n=" + pestN + "|s=" + String(pestT[pestN - 1], 2) +
                          "|g=" + pestGrams + "|L=" + String(pestV[pestN - 1], 3) +
                          "|est=" + String(pestEstL(t, pestT[pestN - 1]), 3));
  calMsg = "";
  if (pestN >= PL_MIN_TRIES) { pestFit = plFit(pestT, pestV, pestN); calStep = 4; }
  else calStep = 1;
}
static bool pestSave(const CalTgt &t) {
  float q = roundf(pestFit.rateLpm * 100.0f) / 100.0f, a = roundf(pestFit.startL * 1000.0f) / 1000.0f;
  if (!pestFit.ok || !calInRange(q, RATE_LPM_MIN, RATE_LPM_MAX)) { calMsg = "rate out of range"; return false; }
  if (pestIsXfer(t)) {
    calRateXfer = q; prefsCal.putFloat("rXfer", q);
    calXferStartL = a; prefsCal.putFloat("xStartL", a);
    pestPushOk = setCalPushBlocking("PRATE", rateWaterWire());
    pestPushOk = setCalPushBlocking("PSTART", String(a, 3)) && pestPushOk;
  } else {
    calRateBoost = q; prefsCal.putFloat("rBoost", q);
    calBoostStartL = a; prefsCal.putFloat("bStartL", a);
    pestPushOk = setCalPushBlocking("PRATE", rateWaterWire());
  }
  logEvent("ESP1", "CAL", String("PEST_SAVE|") + t.id + "|rate=" + String(q, 2) + "|start=" + String(a, 3) +
                          "|n=" + pestFit.n + (pestPushOk ? "" : "|ESP2_LATER"));
  return true;
}
static void pestButton(const CalTgt &t, int i) {      // i: 0=UP 1=DOWN 2=ENTER 3=BACK
  switch (calStep) {
    case 0:                                            // the booster's column valve
      if (i == 0)      pestCol = (pestCol + NUM_COLUMNS - 1) % NUM_COLUMNS;
      else if (i == 1) pestCol = (pestCol + 1) % NUM_COLUMNS;
      else if (i == 2) { calStep = 1; calMsg = ""; }
      else             calCloseTarget();
      break;
    case 1:                                            // seconds, then run
      if (i == 0)      { if (pestSecs < PEST_SECS_MAX) pestSecs++; calMsg = ""; }
      else if (i == 1) { if (pestSecs > PEST_SECS_MIN) pestSecs--; calMsg = ""; }
      else if (i == 2) pestRun(t);
      else             calCloseTarget();
      break;
    case 2:                                            // running: BACK stops it
      if (i == 3) { pestStop(t); calStep = 1; calMsg = "Stopped"; }
      break;
    case 3:                                            // the grams weighed
      if (i == 0)      { pestGrams += 10; if (pestGrams > 30000) pestGrams = 30000; }
      else if (i == 1) { pestGrams -= 10; if (pestGrams < 0) pestGrams = 0; }
      else if (i == 2) { if (!pestReseat) pestKeep(t); }
      else             { calStep = 1; calMsg = "Try dropped"; }
      break;
    case 4:                                            // the fit
      if (i == 0)      { calStep = 1; calMsg = ""; }   // more tries
      else if (i == 2) { if (pestFit.ok && pestSave(t)) calStep = 5; }
      else if (i == 3) calCloseTarget();
      break;
    default:                                           // 5: saved
      if (i == 2 || i == 3) calCloseTarget();
      break;
  }
}
// Keep-alive while a run is on (ESP2 times it; this only feeds its dead-man), a bound on a run that never answers,
// and UP/DOWN held for the seconds and the grams (repeat after 0.6 s, big steps after 2.5 s).
static void pestHoldTick(const CalTgt &t) {
  unsigned long now = millis();
  if (pestRunning) {
    if (now - pestKeepMs >= 150) { bool first = (pestKeepMs == 0); pestKeepMs = now; sendEsp2KeepAlive(pestPrimeCmd(t), first); }
    if (now - pestStartMs > (unsigned long)pestSecs * 1000UL + 100000UL) { pestStop(t); calStep = 1; calMsg = "No answer"; }
  }
  if (calStep != 1 && calStep != 3) { volCalHeldBtn = -1; return; }
  bool up = (digitalRead(BTN_UP) == LOW), dn = (digitalRead(BTN_DOWN) == LOW);
  int b = up ? 0 : dn ? 1 : -1;
  if (b < 0 || (up && dn)) { volCalHeldBtn = -1; return; }
  if (b != volCalHeldBtn) { volCalHeldBtn = b; volCalHeldSinceMs = now; volCalRepeatMs = now; return; }
  if (now - volCalHeldSinceMs < 600 || now - volCalRepeatMs < 120) return;
  volCalRepeatMs = now;
  bool fast = (now - volCalHeldSinceMs >= 2500);
  int dir = (b == 0) ? 1 : -1;
  if (calStep == 1) {
    pestSecs += dir * (fast ? 5 : 1);
    if (pestSecs < PEST_SECS_MIN) pestSecs = PEST_SECS_MIN;
    if (pestSecs > PEST_SECS_MAX) pestSecs = PEST_SECS_MAX;
  } else {
    pestGrams += dir * (fast ? 100 : 10);
    if (pestGrams < 0) pestGrams = 0;
    if (pestGrams > 30000) pestGrams = 30000;
  }
}
static void pestRender(const CalTgt &t) {
  char l[21];
  bool x = pestIsXfer(t);
  lcd.setCursor(0, 0);
  if (x || calStep == 0) snprintf(l, 21, "%-20s", t.name);
  else                   snprintf(l, 21, "Booster pump est  %c ", COL_TAG[pestCol]);
  lcd.print(l);
  const char *r1 = "", *r2 = "", *r3 = "";
  char b1[21], b2[21];
  switch (calStep) {
    case 0:
      snprintf(b1, 21, "Valve: column %c     ", COL_TAG[pestCol]); r1 = b1;
      r2 = "UP/DN = column      "; r3 = "ENT=next  BACK=exit ";
      break;
    case 1:
      snprintf(b1, 21, "Run %2d s  est %5.2fL", pestSecs, pestEstL(t, (float)pestSecs)); r1 = b1;
      snprintf(b2, 21, "Tries %d/%d  %-8s", pestN, PL_MIN_TRIES,
               isfinite(x ? calXferStartL : calBoostStartL) ? "saved" : "default"); r2 = b2;
      r3 = "UP/DN s  ENT=run    ";
      break;
    case 2: {
      // The counter starts at the inverter's switch-on (INFO,INV_ON), not at ENTER: the servo pre-seat and the battery
      // switch come first. It stops at the set seconds -- ESP2 switches the inverter off then, exactly.
      if (pestInvOnMs == 0) {
        snprintf(b1, 21, "Starting  0.0/%2d s ", pestSecs); r1 = b1;
        r2 = "servo + inverter... ";
        r3 = "BACK = stop         ";
      } else {
        float el = (millis() - pestInvOnMs) / 1000.0f;
        bool done = el >= (float)pestSecs;
        if (done) el = (float)pestSecs;
        snprintf(b1, 21, "Running %5.1f/%2d s ", el, pestSecs); r1 = b1;
        snprintf(b2, 21, "est %5.2f L         ", pestEstL(t, el)); r2 = b2;
        r3 = done ? "Stopped: wait...    " : "BACK = stop         ";
      }
      break;
    }
    case 3:
      snprintf(b1, 21, "Ran %4.1fs est %5.2fL", pestRanMs / 1000.0f, pestEstL(t, pestRanMs / 1000.0f)); r1 = b1;
      snprintf(b2, 21, "Water %5d g %5.2fL", pestGrams, pestGrams / PL_G_PER_L); r2 = b2;
      r3 = "ENT=keep BACK=drop  ";
      break;
    case 4:
      if (pestFit.ok) {
        snprintf(b1, 21, "Fit %5.2f L/min n=%-2d", pestFit.rateLpm, pestN); r1 = b1;
        snprintf(b2, 21, "start %+5.2f L      ", pestFit.startL); r2 = b2;
        r3 = "ENT=save UP=more    ";
      } else {
        snprintf(b1, 21, "No line yet: n=%-2d  ", pestN); r1 = b1;
        r2 = "vary the seconds    ";
        r3 = "UP=more  BACK=exit  ";
      }
      break;
    default:
      snprintf(b1, 21, "SAVED %5.2f L/min  ", pestFit.rateLpm); r1 = b1;
      snprintf(b2, 21, "start %+5.2f L      ", pestFit.startL); r2 = b2;
      r3 = pestPushOk ? "ESP2 has it         " : "ESP2: at next start ";
      break;
  }
  lcd.setCursor(0, 1); snprintf(l, 21, "%-20s", r1); lcd.print(l);
  lcd.setCursor(0, 2); snprintf(l, 21, "%-20s", r2); lcd.print(l);
  lcd.setCursor(0, 3);
  if (calMsg.length()) snprintf(l, 21, "%-20s", calMsg.c_str()); else snprintf(l, 21, "%-20s", r3);
  lcd.print(l);
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
  soilCalReset();                                // a soil probe's steadiness window starts empty
  volCalReset(); volCalPumpWasOn = false; volCalHeldBtn = -1;
  if (t.kind == CK_NPK) { for (int j = 0; j < 3; j++) calNpkEdit[j] = calNpkOff[t.col][j]; }  // seed current
  if (t.kind == CK_RATE) { calVolL = rateCurrent(t); return; }   // typed in: nothing to stream
  if (t.kind == CK_PEST) {                                         // no stream: the runs are timed primes
    pestN = 0; pestRunning = false; pestRanMs = -1; pestReseat = false; pestFit.ok = false; pestFit.n = 0;
    calStep = pestIsXfer(t) ? 1 : 0;
    return;
  }
  String cs = String("CAL_START,") + t.id;
  if (calIsNano(t.kind)) sendNanoCommand(cs.c_str()); else sendEsp2(cs);
}

static void calCloseTarget() {
  const CalTgt &t = CAL_TGTS[calSel];
  if (t.kind == CK_RATE) { calOpen = false; return; }             // nothing was streaming
  if (t.kind == CK_PEST) { pestStop(t); calOpen = false; return; } // a run must not outlive the screen
  String cs = String("CAL_STOP,") + t.id;
  if (t.kind == CK_VOL_MIX && volCalPumpWasOn) {  // a hold-to-run pump must not outlive the screen
    sendEsp2("PRIME_STOP,RESMIX"); sendEsp2("PRIME_STOP,MIXIRR"); volCalPumpWasOn = false;
  }
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
    case CK_SOIL: {                 // one probe: calCap[0] = WET (locked first), calCap[1] = DRY
      int p = calSoilProbe(t);
      int wat = (int)calCap[0], dry = (int)calCap[1];
      if (!soilCalOk(dry, wat, (int)CAL_MIN_SPAN_ADC)) return false;      // too close, or dry/wet reversed
      String pk = "s" + String(t.col) + "p" + String(p + 1);
      prefsCal.putInt((pk + "d").c_str(), dry); prefsCal.putInt((pk + "w").c_str(), wat);
      calProbeDry[t.col][p] = dry; calProbeWet[t.col][p] = wat;
      snprintf(buf, sizeof(buf), "SAVE|%s|dry=%d|wet=%d", t.id, dry, wat);
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
    // Empty capture (the tank drained). Rejected unless it sits clearly beyond the FULL distance -- an
    // empty "captured" with water in the tank would invert the % and move the overflow stop. Both values
    // of the pair are saved, under the bumped "2" keys loadCal() reads (see calResEmptyCm).
    // web v.4: the captured cm is temperature-corrected like every live reading, and the tank litres
    // geometry follows the new empty mark (its calibration points stay at their sensor distances).
    case CK_LEVEL_RES: {
      float cm = ultraCorrectCm(calCap[0]);
      if (cm - calResFullCm < CAL_LEVEL_MIN_SPAN_CM) return false;
      calResEmptyCm = cm;
      prefsCal.putFloat("reCm2", calResEmptyCm); prefsCal.putFloat("rfCm2", calResFullCm);
      tankVolSyncGeom();
      snprintf(buf, sizeof(buf), "SAVE|ULTRA_RES|%s/%s", String(calResEmptyCm, 1).c_str(), String(calResFullCm, 1).c_str());
      logEvent("ESP1", "CAL", buf); break;
    }
    case CK_LEVEL_MIX: {
      float cm = ultraCorrectCm(calCap[0]);
      if (cm - calMixFullCm < CAL_LEVEL_MIN_SPAN_CM) return false;
      calMixEmptyCm = cm;
      prefsCal.putFloat("meCm2", calMixEmptyCm); prefsCal.putFloat("mfCm2", calMixFullCm);
      tankVolSyncGeom();
      sendEsp2("SET_CAL,MIXCAP," + String(mixCapLiters(), 2));   // the fill ceiling moved with it
      snprintf(buf, sizeof(buf), "SAVE|ULTRA_MIX|%s/%s", String(calMixEmptyCm, 1).c_str(), String(calMixFullCm, 1).c_str());
      logEvent("ESP1", "CAL", buf); break;
    }
    // Tank litres point (web v.4): the locked level (volCalLockH) + the litres the operator typed (calVolL).
    // Refused when the points would not climb with the level (tvAddPoint) -- nothing is saved then.
    case CK_VOL_MIX: case CK_VOL_RES: {
      bool mix = (t.kind == CK_VOL_MIX);
      TankVolCal &tv = mix ? calMixVol : calResVol;
      if (!tvAddPoint(tv, volCalLockH, calVolL)) return false;
      tankVolSave(mix);
      if (mix) sendEsp2("SET_CAL,MIXCAP," + String(mixCapLiters(), 2));
      snprintf(buf, sizeof(buf), "SAVE|VOL_%s|%scm=%sL|n=%d", mix ? "MIX" : "RES",
               String(volCalLockCm, 1).c_str(), String(calVolL, 1).c_str(), (int)tv.n);
      logEvent("ESP1", "CAL", buf);
      break;
    }
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
      if      (!strcmp(id, "FLOW_NUTA"))   { calKNut[0] = kf; prefsCal.putFloat("kNA", kf); }
      else if (!strcmp(id, "FLOW_NUTB"))   { calKNut[1] = kf; prefsCal.putFloat("kNB", kf); }
      else if (!strcmp(id, "FLOW_NUTC"))   { calKNut[2] = kf; prefsCal.putFloat("kNC", kf); }
      else if (!strcmp(id, "FLOW_NUTD"))   { calKNutD   = kf; prefsCal.putFloat("kND", kf); }   // keys loadCal() reads
      else if (!strcmp(id, "FLOW_PHUP"))   { calKPhUp   = kf; prefsCal.putFloat("kPU", kf); }
      else if (!strcmp(id, "FLOW_PHDN"))   { calKPhDn   = kf; prefsCal.putFloat("kPD", kf); }
      setCalPushBlocking(id, String(kf, 1));             // ESP2 ACKs these three as a no-op (it doesn't meter them)
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
    case CK_PEST: return false;   // saved by pestSave() (its own screen flow)
    case CK_RATE: {       // a pump's measured constant rate (2026-11-17); 0 = not measured
      float v = calVolL;
      if (rateIsWater(t)) {
        if (v != 0.0f && !calInRange(v, RATE_LPM_MIN, RATE_LPM_MAX)) return false;
        const char *key = (t.col == 0) ? "rXfer" : "rBoost";
        if (t.col == 0) calRateXfer = v; else calRateBoost = v;
        if (v > 0.0f) prefsCal.putFloat(key, v); else if (prefsCal.isKey(key)) prefsCal.remove(key);
        calRatePushOk = setCalPushBlocking("PRATE", rateWaterWire());
      } else {
        int n = t.col - 2;
        if (v != 0.0f && !calInRange(v, RATE_MLPM_MIN, RATE_MLPM_MAX)) return false;
        const char *key = (n == 0) ? "pumpA" : (n == 1) ? "pumpB" : "pumpC";   // the bench tools' keys
        if (v > 0.0f) { prefsCal.putFloat(key, v); PUMP_FLOWRATE_MLPM[n] = v; pumpRateSet[n] = true; }
        else {                                        // back to the default for the dose ceiling only
          if (prefsCal.isKey(key)) prefsCal.remove(key);
          PUMP_FLOWRATE_MLPM[n] = PUMP_FLOWRATE_DEFAULT_MLPM; pumpRateSet[n] = false;
        }
        calRatePushOk = setCalPushBlocking("NRATE", rateNutWire());
      }
      snprintf(buf, sizeof(buf), "SAVE|%s|%s%s", t.id, v > 0.0f ? String(v, 2).c_str() : "off",
               calRatePushOk ? "" : "|ESP2_LATER");
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
  if (t.kind == CK_PEST) { pestButton(t, i); return; }   // its own steps, BACK included (2026-11-17)
  if (i == 3) { calCloseTarget(); return; }        // BACK = abandon this target (no save)

  // ---- Pump rate (2026-11-17): step 0 type it (UP/DOWN, held = faster; below the minimum = not measured),
  //      ENTER saves and sends it to ESP2; step 1 = saved. ----
  if (t.kind == CK_RATE) {
    if (calStep == 0) {
      if (i == 0) rateNudge(t, +1, false);
      else if (i == 1) rateNudge(t, -1, false);
      else if (i == 2) { if (calCommit()) { calStep = 1; calMsg = ""; } else calMsg = "out of range"; }
    } else if (i == 2) calCloseTarget();
    return;
  }

  // ---- Tank litres (web v.4): step 0 lock the level, 1 type the litres, 2 saved, 3 confirm "clear all" ----
  if (t.kind == CK_VOL_MIX || t.kind == CK_VOL_RES) {
    TankVolCal &tv = (t.kind == CK_VOL_MIX) ? calMixVol : calResVol;
    if (calStep == 0) {                            // UP / DOWN are the hold-to-run pumps here (calHoldTick)
      if (i != 2) return;
      float med, lo, hi; bool steady;
      if (!volCalStats(&med, &lo, &hi, &steady) || millis() - calRxMs >= CAL_STALE_MS) { calMsg = "no reading yet"; return; }
      if (!steady) { calMsg = "level moving - wait"; return; }
      float h = tvHeightCm(tv, med);
      if (h < -TV_ANCHOR_CM || h > tvSpanCm(tv) * TV_OVER_FULL) { calMsg = "outside 0..200 %"; return; }
      volCalLockCm = med; volCalLockH = h < 0.0f ? 0.0f : h;
      calVolL = roundf(tvLitersAtH(tv, volCalLockH) * 10.0f) / 10.0f;   // start from the current estimate
      calStep = 1; calMsg = "";
      logEvent("ESP1", "CAL", String("LOCK|") + t.id + "|cm=" + String(med, 1) + "|h=" + String(volCalLockH, 1));
    } else if (calStep == 1) {                     // litres: UP/DOWN 0.1 L (held: faster), ENTER = save
      if (i == 0) calVolL += VOL_CAL_STEP_L;
      else if (i == 1) { calVolL -= VOL_CAL_STEP_L; if (calVolL < 0.0f) calVolL = 0.0f; }
      else if (i == 2) {
        if (calCommit()) { calStep = 2; calMsg = ""; }
        else calMsg = "must rise with level";      // e.g. fewer litres than a lower point: re-type
      }
    } else if (calStep == 2) {                     // saved: ENTER = add another point, DOWN = clear all
      if (i == 2) { calStep = 0; calMsg = ""; volCalReset(); }
      else if (i == 1 && tv.n > 0) { calStep = 3; calMsg = ""; }
    } else {                                       // confirm clear: ENTER = yes, UP = no
      if (i == 2) {
        tvClearPoints(tv); tankVolSave(t.kind == CK_VOL_MIX);
        if (t.kind == CK_VOL_MIX) sendEsp2("SET_CAL,MIXCAP," + String(mixCapLiters(), 2));
        logEvent("ESP1", "CAL", String("CLEAR|") + (t.kind == CK_VOL_MIX ? "VOL_MIX" : "VOL_RES"));
        calStep = 2; calMsg = "points cleared";
      } else if (i == 0) { calStep = 2; calMsg = ""; }
    }
    return;
  }

  if (t.kind == CK_FLOW) {
    if (calStep == 0) {                            // run step: ENTER held = pump (calHoldTick); UP = proceed
      // Require an actual sample before letting UP advance -- previously a missing/no-reading-yet
      // case silently substituted 0 pulses and still proceeded, same class of gap as the generic
      // capture path above (see its comment). A genuine "0 pulses so far, pump just started" reading
      // still has calRxValid true, so this only blocks the "never got a reading at all" case.
      if (i == 0) {
        if (!calRxValid) { calMsg = "no reading yet"; return; }
        calFlowPulses = (unsigned long)calRxRaw; calStep = 1;
      }
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
      if (i == 2) {
        if (!calRxValid) { calMsg = "no reading yet"; return; }
        calCap[0] = calRxRaw; calVolL = calCap[0]; calStep = 1;
      }
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

  if (t.kind == CK_SOIL) {
    // One probe, two locks (2026-11-16): step 0 = SET 100% WET (probe in water) -> ENTER locks the live raw;
    // step 1 = SET DRY 0% (dry the probe, watch the raw settle) -> ENTER locks it, checks and saves; step 2 =
    // saved, showing the probe's live % on its new calibration. ENTER locks the mean of the steadiness window
    // (the last few streamed readings), not one sample. UP in the dry step goes back to redo the wet lock.
    if (calStep >= 2) { if (i == 2) calCloseTarget(); return; }        // done screen: ENTER (or BACK) leaves
    if (i == 0 && calStep == 1) { calStep = 0; calMsg = ""; soilCalReset(); return; }
    if (i != 2) return;
    int mean, lo, hi; bool steady;
    if (!soilCalStats(&mean, &lo, &hi, &steady) || millis() - calRxMs >= CAL_STALE_MS) { calMsg = "no reading yet"; return; }
    if (mean <= SOIL_ADC_RAIL_LO || mean >= SOIL_ADC_RAIL_HI) { calMsg = "probe not connected"; return; }
    calCap[calStep] = (float)mean;
    if (calStep == 0) {
      calStep = 1; calMsg = ""; soilCalReset();                       // the dry reading starts a fresh window
      logEvent("ESP1", "CAL", String("LOCK|") + t.id + "|wet=" + mean + (steady ? "" : "|unsteady"));
    } else if (calCommit()) {
      calStep = 2; calMsg = "";
    } else {
      calMsg = "dry must be > wet+20";                                 // stay in the dry step; UP = redo wet
    }
    return;
  }

  if (t.kind == CK_FLOW_RES) {                    // Nano reservoir flow: manual external fill, no pump
    if (calStep == 0) {                            // watch pulses climb during the fill; UP = done
      if (i == 0) {
        if (!calRxValid) { calMsg = "no reading yet"; return; }
        calFlowPulses = (unsigned long)calRxRaw; calStep = 1;
      }
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
    // Was: capture if calRxValid, but advance calStep unconditionally either way -- pressing ENTER
    // before the first raw sample has streamed back from the Nano/ESP2 (calRxValid is explicitly
    // reset false on calOpenTarget()) silently captured a stale/zero value and still advanced the
    // wizard. The LCD does show "--" for a stale reading as a visual cue, but nothing blocked the
    // advance itself. Now requires an actual reading before this step counts.
    if (!calRxValid) { calMsg = "no reading yet"; return; }
    calMsg = "";
    calCap[calStep] = calRxRaw;
    if (++calStep >= needed) {
      if (calCommit()) calCloseTarget();
      else { calStep = 0; calMsg = "pts too close"; }   // degenerate pH/EC -> recapture (§B audit fix)
    }
  }
}

// Flow dead-man: while ENTER is physically held in the flow run step, stream PRIME_START as the
// keep-alive (ESP2 caps locally); release -> PRIME_STOP. Mirrors testHoldTick (sec.18.10.8).
// Tank litres screen (web v.4). Step 0 on the MIXING tank: hold UP = transfer pump in (Prime RESMIX, with ESP2's
// overflow stop), hold DOWN = booster out through column A (Prime MIXIRR) -- the same dead-man keep-alive as
// the flow calibration, so the level can be walked through the points without leaving the screen. Step 1:
// UP/DOWN held for >0.6 s repeat the 0.1 L step, and after 2.5 s step a whole litre.
static void volCalHoldTick(const CalTgt &t) {
  static unsigned long lastHold = 0;
  static HoldBtn upBtn = { false, 0 }, dnBtn = { false, 0 };
  bool up = holdBtnRead(upBtn, BTN_UP), dn = holdBtnRead(dnBtn, BTN_DOWN);   // a release only after BTN_RELEASE_MS
  unsigned long now = millis();
  if (calStep == 1) {
    int b = up ? 0 : dn ? 1 : -1;
    if (b < 0 || (up && dn)) { volCalHeldBtn = -1; return; }
    if (b != volCalHeldBtn) { volCalHeldBtn = b; volCalHeldSinceMs = now; volCalRepeatMs = now; return; }
    if (now - volCalHeldSinceMs < 600 || now - volCalRepeatMs < 120) return;
    volCalRepeatMs = now;
    float step = (now - volCalHeldSinceMs >= 2500) ? 1.0f : VOL_CAL_STEP_L;
    calVolL += (b == 0) ? step : -step;
    if (calVolL < 0.0f) calVolL = 0.0f;
    if (calVolL > TV_L_MAX) calVolL = TV_L_MAX;
    return;
  }
  volCalHeldBtn = -1;
  if (t.kind != CK_VOL_MIX || calStep != 0) {
    if (volCalPumpWasOn) { sendEsp2("PRIME_STOP,RESMIX"); sendEsp2("PRIME_STOP,MIXIRR"); volCalPumpWasOn = false; }
    return;
  }
  static bool volWasHeld = false, volTooSoon = false;
  if (up != dn && !volWasHeld) {                   // press edge: too soon after the last run -> ignore this press
    volTooSoon = actRearmWaiting();
    if (volTooSoon) calMsg = "Too fast: wait 1.5s";
  }
  volWasHeld = (up != dn);
  if (up != dn && volTooSoon) return;              // release and press again
  if (up == dn) volTooSoon = false;
  if (up != dn) {                                  // exactly one held
    if (now - lastHold >= 150) {
      lastHold = now;
      if (actuationsDisabled)      calMsg = "Actuations off";
      else if (secBattBlocksAc())  calMsg = secBlockLcd();
      else {
        sendEsp2KeepAlive(up ? "PRIME_START,RESMIX" : "PRIME_START,MIXIRR,A", !volCalPumpWasOn);
        volCalPumpWasOn = true;
        volCalReset();                             // the level is moving: the lock window starts again
      }
    }
  } else if (volCalPumpWasOn) {
    sendEsp2("PRIME_STOP,RESMIX"); sendEsp2("PRIME_STOP,MIXIRR");
    volCalPumpWasOn = false;
    actRearmStart();                               // the next press waits ACT_REARM_MS
    volCalReset();
  }
}

void calHoldTick() {
  if (uiMode != UI_CAL || !calOpen) { calEnterWasDown = false; return; }
  const CalTgt &t = CAL_TGTS[calSel];
  if (t.kind == CK_VOL_MIX || t.kind == CK_VOL_RES) { calEnterWasDown = false; volCalHoldTick(t); return; }
  if (t.kind == CK_RATE) { calEnterWasDown = false; rateHoldTick(t); return; }
  if (t.kind == CK_PEST) { calEnterWasDown = false; pestHoldTick(t); return; }
  if (t.kind != CK_FLOW || calStep != 0) { calEnterWasDown = false; return; }
  static unsigned long lastHold = 0;
  static bool primeTooSoon = false;
  int col; const char *ln = calPrimeLine(t.id, &col);
  static HoldBtn primeBtn = { false, 0 };
  static bool primeLogged = false;                           // this hold's first keep-alive went into the log
  bool down = holdBtnRead(primeBtn, BTN_ENTER);              // a release only after BTN_RELEASE_MS up
  if (down) {
    if (!calEnterWasDown) {                        // press edge: too soon after the last prime -> ignore this press
      primeTooSoon = actRearmWaiting();
      if (primeTooSoon) calMsg = "Too fast: wait 1.5s";
    }
    if (!primeTooSoon && millis() - lastHold >= 150) {
      lastHold = millis();
      bool acLine = !strcmp(ln, "RESMIX") || !strcmp(ln, "MIXIRR");   // transfer / booster = inverter
      if (actuationsDisabled) {
        calMsg = "Actuations off";
      } else if (acLine && secBattBlocksAc()) {
        calMsg = secBlockLcd();
      } else {
        String c = String("PRIME_START,") + ln;
        if (col >= 0) c += String(",") + COL_TAG[col];
        sendEsp2KeepAlive(c, !primeLogged);
        primeLogged = true;
      }
    }
    calEnterWasDown = true;
  } else if (calEnterWasDown) {
    if (!primeTooSoon) { sendEsp2(String("PRIME_STOP,") + ln); actRearmStart(); }
    primeTooSoon = false; primeLogged = false;
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
  if (t.kind == CK_PEST) { pestRender(t); return; }       // its own title row (2026-11-17)
  lcd.setCursor(0, 0); snprintf(l, 21, "CAL %-16s", t.name); lcd.print(l);
  if (t.kind == CK_RATE) {                                  // pump rate (2026-11-17)
    bool w = rateIsWater(t);
    const char *u = w ? "L/min" : "mL/min";
    char v[16];
    if (calStep == 0) {
      lcd.setCursor(0, 1);
      if (calVolL > 0.0f) { snprintf(v, sizeof(v), w ? "%.2f" : "%.1f", calVolL); snprintf(l, 21, "set: %8s %-6s", v, u); }
      else snprintf(l, 21, "set: not measured   ");
      lcd.print(l);
      // What the last run really saw (INFO,PUMP_RATE from ESP2): the levels for water, the flow meter for a dose.
      lcd.setCursor(0, 2);
      float seen = rateSeen[t.col];
      if (seen > 0.0f) { snprintf(v, sizeof(v), w ? "%.2f" : "%.1f", seen); snprintf(l, 21, "seen:%8s %-6s", v, u); }
      else snprintf(l, 21, "seen: no run yet    ");
      lcd.print(l);
      lcd.setCursor(0, 3);
      if (calMsg.length()) { snprintf(l, 21, "%-20s", calMsg.c_str()); lcd.print(l); }
      else lcd.print("UP/DN ENT=save BACK ");
    } else {
      lcd.setCursor(0, 1);
      if (calVolL > 0.0f) { snprintf(v, sizeof(v), w ? "%.2f" : "%.1f", calVolL); snprintf(l, 21, "SAVED %7s %-6s", v, u); }
      else snprintf(l, 21, "SAVED: not measured ");
      lcd.print(l);
      lcd.setCursor(0, 2); lcd.print(calRatePushOk ? "ESP2 has it         " : "ESP2: at next start ");
      lcd.setCursor(0, 3); lcd.print("ENT/BACK = done     ");
    }
    return;
  }
  if (t.kind == CK_VOL_MIX || t.kind == CK_VOL_RES) {       // tank litres (web v.4)
    bool mix = (t.kind == CK_VOL_MIX);
    const TankVolCal &tv = mix ? calMixVol : calResVol;
    float med = 0, lo = 0, hi = 0; bool steady = false;
    bool have = volCalStats(&med, &lo, &hi, &steady) && fresh;
    float span = tvSpanCm(tv);
    if (calStep == 0) {
      // "41.2cm 9% ~1.5L" / "STEADY pts 2/8" -- the level now, its % and the litres the curve gives it.
      if (have) {
        float h = tvHeightCm(tv, med);
        int pct = (span > 0.0f) ? (int)roundf(h * 100.0f / span) : 0;
        snprintf(l, 21, "%5scm %3d%% ~%sL      ", String(med, 1).c_str(), pct, String(tvLitersAtH(tv, h), 1).c_str());
      } else snprintf(l, 21, "level --  waiting    ");
      lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "%-7s pts %d/%d      ", have ? (steady ? "STEADY" : "moving") : "", (int)tv.n, (int)TV_MAX_PTS);
      lcd.setCursor(0, 2); lcd.print(l);
      lcd.setCursor(0, 3);
      if (calMsg.length()) { snprintf(l, 21, "%-20s", calMsg.c_str()); lcd.print(l); }
      else lcd.print(mix ? "ENT=lock UP/DN=pump " : "ENT=lock  BACK=done ");
    } else if (calStep == 1) {
      snprintf(l, 21, "at %scm h%scm     ", String(volCalLockCm, 1).c_str(), String(volCalLockH, 1).c_str());
      lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "litres: %-7s     ", String(calVolL, 1).c_str());
      lcd.setCursor(0, 2); lcd.print(l);
      lcd.setCursor(0, 3);
      if (calMsg.length()) { snprintf(l, 21, "%-20s", calMsg.c_str()); lcd.print(l); }
      else lcd.print("UP/DN L  ENT=save   ");
    } else if (calStep == 2) {
      snprintf(l, 21, "SAVED  pts %d/%d      ", (int)tv.n, (int)TV_MAX_PTS);
      lcd.setCursor(0, 1); lcd.print(l);
      { char t[32];                                        // "full=18.5L 90%=16.2L": 20 (it was 21, the last L cut off)
        snprintf(t, sizeof(t), "full=%sL %d%%=%sL", String(tvLitersAtPct(tv, 100.0f), 1).c_str(),
                 (int)MIX_HIGH_PCT, String(tvLitersAtPct(tv, MIX_HIGH_PCT), 1).c_str());
        snprintf(l, 21, "%-20s", t); }
      lcd.setCursor(0, 2); lcd.print(l);
      lcd.setCursor(0, 3);
      if (calMsg.length()) { snprintf(l, 21, "%-20s", calMsg.c_str()); lcd.print(l); }
      else lcd.print("ENT=add DN=clr BACK ");
    } else {
      snprintf(l, 21, "Clear all %d points?", (int)tv.n); lcd.setCursor(0, 1); lcd.print(l);
      lcd.setCursor(0, 2); lcd.print("(cone shape only)   ");
      lcd.setCursor(0, 3); lcd.print("ENT=yes  UP=no      ");
    }
    return;
  }
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
  if (t.kind == CK_SOIL) {                          // one capacitive probe: WET lock, then DRY lock
    int p = calSoilProbe(t);
    int mean = 0, lo = 0, hi = 0; bool steady = false;
    bool have = soilCalStats(&mean, &lo, &hi, &steady) && fresh;
    if (calStep >= 2) {                             // saved: show the result and the live % on it
      lcd.setCursor(0, 1); snprintf(l, 21, "SAVED W%4d D%4d   ", calProbeWet[t.col][p], calProbeDry[t.col][p]); lcd.print(l);
      lcd.setCursor(0, 2);
      if (have) { snprintf(l, 21, "raw %4d = %3d%%     ", mean, soilPctCal(mean, calProbeDry[t.col][p], calProbeWet[t.col][p])); lcd.print(l); }
      else lcd.print("raw   --            ");
      lcd.setCursor(0, 3); lcd.print("ENT/BACK = done     ");
      return;
    }
    lcd.setCursor(0, 1); lcd.print(calStep == 0 ? "SET 100% WET: water " : "SET DRY 0%: dry it  ");
    lcd.setCursor(0, 2);
    if (!have) lcd.print("raw   --  waiting   ");
    else if (calStep == 0) { snprintf(l, 21, "raw%4d %-6s +-%-3d", mean, steady ? "STEADY" : "moving", (hi - lo + 1) / 2); lcd.print(l); }
    else { snprintf(l, 21, "raw%4d %-6s W%-4d", mean, steady ? "STEADY" : "moving", (int)calCap[0]); lcd.print(l); }
    lcd.setCursor(0, 3);
    if (calMsg.length()) { snprintf(l, 21, "%-20s", calMsg.c_str()); lcd.print(l); }
    else lcd.print(calStep == 0 ? "ENT=lock WET BACK=x " : "ENT=lock DRY UP=wet ");
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

/* Start a headless sweep for the dashboard. Same round-robin the LCD page uses -- diagTick() now
 * runs whenever EITHER the LCD Sensor Diag screen is open or this window is live, so there is one
 * implementation rather than two that can disagree. Bounded: it powers ESP2 up, sweeps, and hands
 * it back, because ESP2 is normally OFF at idle and must not be left running by a browser. */
bool diagRemoteStart(unsigned long ms, const char **why) {
  // New-F1 (audit): split out so DIAG_SWEEP's Firebase rejection can name which condition actually
  // failed, matching the FORCE_RUN/TEST_PULSE idle-gates instead of the one flat "not safely idle"
  // string this used to leave the caller with. Order picks the most specific/actionable reason first
  // when more than one is true at once. Behaviour (when this returns false) is unchanged.
  if (esp2Held)               { if (why) *why = "a fault is held";                  return false; }
  if (wo.active)              { if (why) *why = "a run is in progress";             return false; }
  if (uiMode != UI_DATA)      { if (why) *why = "someone is using the LCD";         return false; }
  if (sysState != IDLE_STATE) { if (why) *why = "the controller is not idle";       return false; }
  for (int k = 0; k < DIAG_ESP2_N; k++) { diagEsp2Raw[k] = 0; diagEsp2Valid[k] = false; diagEsp2Ms[k] = 0; }
  diagEsp2Idx = -1; diagEsp2DwellMs = 0; diagEsp2Streaming = false;
  diagRemoteUntilMs = millis() + ms;
  esp2SetPower(true);
  logEvent("ESP1", "DIAG", String("REMOTE|START|") + (ms / 1000) + "s");
  return true;
}

// Close a remote sweep and power ESP2 back down. Safe to call when none is running.
void diagRemoteTick() {
  if (!diagRemoteUntilMs) return;
  bool expired = (long)(millis() - diagRemoteUntilMs) >= 0;
  // A run, a fault or the operator opening the LCD diag page all take ESP2 back off us.
  bool preempted = (sysState != IDLE_STATE || wo.active || esp2Held || uiMode != UI_DATA);
  if (!expired && !preempted) return;
  diagStopStream();
  diagRemoteUntilMs = 0;
  // HAND ESP2 OVER rather than switching it off when something preempted us. If the LCD diag page is
  // what preempted, enterDiag() has already powered ESP2 up for it earlier in this same loop --
  // powering down here would be deferred by the min-on hold and then executed ~10 s later, killing
  // the page the operator just opened. exitDiag() owns the power-down in that case, and a run or a
  // fault likewise owns ESP2 once it has taken it. Only a clean expiry powers it down here.
  if (!preempted) esp2SetPower(false);
  logEvent("ESP1", "DIAG", expired ? "REMOTE|DONE" : "REMOTE|PREEMPTED");
}

// Round-robin the ESP2 CAL stream (self-starting): arm one id, dwell, record raw, stop, advance.
void diagTick() {
  // Driven by the LCD page OR a bounded remote sweep -- one implementation, two triggers.
  if (uiMode != UI_DIAG && !diagRemoteUntilMs) return;
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
// Sensor Diag tank rows: ' ' = usable for control; the Nano's status letter when it gave a reason; '!' otherwise.
static char diagUsChar(bool ok, char st) { if (ok) return ' '; return (st == '?' || st == 'K') ? '!' : st; }
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
    case 1:   // Nano: ultrasonic tanks (raw cm, litres, median spread) + reservoir flow (raw)
      snprintf(l, 21, "%-8s Nano Tank", hdr);                             lcd.setCursor(0, 0); lcd.print(l);
      // "R 31.2  12.4L s0.3" -- raw cm, litres from the tank calibration, the Nano's 5-ping spread (cm);
      // a trailing '!' = not usable for control (no echo, too noisy, or outside the tank).
      // The last character: ' ' = usable; else the Nano's reason (E no echo, R out of range, F few pings, S echo
      // stuck, X no pings) or '!' (ESP1 refused it: too scattered, outside the tank, no geometry).
      snprintf(l, 21, "R%5s %5sL s%-3s%c", String(sensor.rawResCm, 1).c_str(),
               sensor.resL >= 0 ? String(sensor.resL, 1).c_str() : "--",
               sensor.resSpreadCm >= 0 ? String(sensor.resSpreadCm, 1).c_str() : "-", diagUsChar(sensor.resOk, sensor.resSt));
      lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "M%5s %5sL s%-3s%c", String(sensor.rawMixCm, 1).c_str(),
               sensor.mixL >= 0 ? String(sensor.mixL, 1).c_str() : "--",
               sensor.mixSpreadCm >= 0 ? String(sensor.mixSpreadCm, 1).c_str() : "-", diagUsChar(sensor.mixOk, sensor.mixSt));
      lcd.setCursor(0, 2); lcd.print(l);
      // Flow (raw L/min), the TANK link (% of the Nano's frames that arrived lately), freshness.
      { int q = tankLinkPct();
        snprintf(l, 21, "Flw%5s Lk%-4s%5s", String(sensor.rawFlow, 1).c_str(), q >= 0 ? (String(q) + "%").c_str() : "--",
                 diagNanoFlag(sensor.msTank, true)); }
      lcd.setCursor(0, 3); lcd.print(l);
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
    case 4: case 5: case 6: {  // Nano: ALL SEVEN channels of ONE column's 7-in-1 probe.
      // The probe is not just N/P/K -- it also reports moisture, soil temperature, EC and pH, all of
      // which were being measured and then discarded. This replaced a single shared "NPK Chem" row
      // per column, which could only fit EC/pH. Engineering units here; the raw Modbus registers are
      // on the dashboard's raw-sensor table. Distinct from the ESP2 chem page, which is the separate
      // EC/pH pair in the MIXING TANK rather than in the soil.
      int c = diagPage - DIAG_NPK_FIRST;
      // These three cases assume one page per column. If NUM_COLUMNS is ever reduced without
      // shrinking DIAG_PAGES to match, bail rather than index past sensor.npk[].
      if (c < 0 || c >= NUM_COLUMNS) { lcdRow(0, "NPK page n/a"); lcdRow(1, ""); lcdRow(2, ""); lcdRow(3, ""); break; }
      snprintf(l, 21, "%-8s NPK %c", hdr, COL_TAG[c]);                    lcd.setCursor(0, 0); lcd.print(l);
      if (!COLUMN_ENABLED[c]) { lcdRow(1, "column disabled"); lcdRow(2, ""); lcdRow(3, ""); break; }
      snprintf(l, 21, "M%5.1f%%  T%5.1fC", sensor.npk[c][0], sensor.npk[c][1]);  lcdRow(1, l);
      snprintf(l, 21, "EC%-7.2f pH%4.1f", sensor.npk[c][2], sensor.npk[c][3]);   lcdRow(2, l);
      snprintf(l, 21, "N%-4d P%-4d K%-4d%3s", (int)sensor.npk[c][4], (int)sensor.npk[c][5],   // 20 cols (was 21)
               (int)sensor.npk[c][6], diagNanoFlag(sensor.msNpk[c], sensor.npkValid[c]));
      lcdRow(3, l);
      break;
    }
    case 7:   // ESP1 local: battery raw ADC + device presence
      snprintf(l, 21, "%-8s ESP1 Loc", hdr);                              lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "BatV raw%-5d%5.1fV", (int)readAdcTrimmed(PIN_BATT_V), battV); lcd.setCursor(0, 1); lcd.print(l);
      // Shows the SIGNED amps from the source in use, plus the ACS758 fallback's own health (ok/X), so a
      // missing sensor reads as a fault and never as "0.0 A". M/S = main and solar INA226 answering.
      snprintf(l, 21, "%7.2fA M%c S%c ADS%c", battIsigned, inaUsable(powerIna[INA_MAIN]) ? '+' : 'X',
               inaUsable(powerIna[INA_SOLAR]) ? '+' : 'X', acsOk ? '+' : 'X'); lcd.setCursor(0, 2); lcd.print(l);
      snprintf(l, 21, "RTC:%s LCD:%s SD:%s", i2cPresent(0x68) ? "ok" : "X", i2cPresent(LCD_ADDR) ? "ok" : "X", sdOk ? "ok" : "X"); lcd.setCursor(0, 3); lcd.print(l);
      break;
    case 8:   // ESP2: pH / EC / ACS712 raw ADC (live CAL stream) -- ESP2's OWN probes, not the NPK's
      snprintf(l, 21, "%-8s ESP2 Chem", hdr);                             lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "%-4sraw%-8ld%5s", "pH",  (long)diagEsp2Raw[0], diagEsp2Flag(0)); lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "%-4sraw%-8ld%5s", "EC",  (long)diagEsp2Raw[1], diagEsp2Flag(1)); lcd.setCursor(0, 2); lcd.print(l);
      snprintf(l, 21, "%-4sraw%-8ld%5s", "ACS", (long)diagEsp2Raw[2], diagEsp2Flag(2)); lcd.setCursor(0, 3); lcd.print(l);
      break;
    case 9:   // ESP2: PZEM AC power (V/I/P, engineering values from the CAL stream)
      snprintf(l, 21, "%-8s ESP2 Pwr", hdr);                              lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "Vac:%-8s%5s", String(diagEsp2Raw[3], 0).c_str(), diagEsp2Flag(3)); lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "Iac:%-8s%5s", String(diagEsp2Raw[4], 2).c_str(), diagEsp2Flag(4)); lcd.setCursor(0, 2); lcd.print(l);
      snprintf(l, 21, "Pw :%-8s%5s", String(diagEsp2Raw[5], 0).c_str(), diagEsp2Flag(5)); lcd.setCursor(0, 3); lcd.print(l);
      break;
    case 10:   // ESP2 dosing flow (1/2): NUT A / B / C (pulse counts). web v.4: no RESMIX / MIXIRR meters.
      snprintf(l, 21, "%-8s ESP2 Flw1", hdr);                             lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "%-7s p%-7ld%4s", "NUT A", (long)diagEsp2Raw[6], diagEsp2Flag(6)); lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "%-7s p%-7ld%4s", "NUT B", (long)diagEsp2Raw[7], diagEsp2Flag(7)); lcd.setCursor(0, 2); lcd.print(l);
      snprintf(l, 21, "%-7s p%-7ld%4s", "NUT C", (long)diagEsp2Raw[8], diagEsp2Flag(8)); lcd.setCursor(0, 3); lcd.print(l);
      break;
    default:  // case 11: ESP2 dosing flow (2/2): NUT D / pH up / pH down
      snprintf(l, 21, "%-8s ESP2 Flw2", hdr);                             lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "%-7s p%-7ld%4s", "NUT D", (long)diagEsp2Raw[9],  diagEsp2Flag(9));  lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "%-7s p%-7ld%4s", "PH UP", (long)diagEsp2Raw[10], diagEsp2Flag(10)); lcd.setCursor(0, 2); lcd.print(l);
      snprintf(l, 21, "%-7s p%-7ld%4s", "PH DN", (long)diagEsp2Raw[11], diagEsp2Flag(11)); lcd.setCursor(0, 3); lcd.print(l);
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

  // ---- Force-run countdown: BACK cancels it -----------------------------------
  // Only while WAITING. Once the work order is away, stopping it is the Cancel-run path (which
  // must also stop ESP2), not a keypress here.
  if (fcPhase == FP_WAIT && uiMode == UI_DATA) {
    static bool fcBackLatch = false;
    if (digitalRead(BTN_BACK) == LOW) {
      if (!fcBackLatch) { fcBackLatch = true; wakeBacklight(); forceAbort("OPERATOR"); }
    } else fcBackLatch = false;
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
        fbRequestPush();                                          // the website unlocks with it (2026-11-05)
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

  // ---- Web manual mode: the banner owns the screen ---------------------------
  // Same shape as the run-lock branch below: swallow everything so nobody wanders into a menu while
  // a remote operator has the relays, except UP+DOWN which opens the chooser. Sits ABOVE the run
  // branch only in code order; it cannot both be true, because the banner render requires
  // runPhase == RUN_NONE. MODE+BACK e-stop is handled higher up and always works.
  if (webManualHeld() && !wmPeek && !esp2Held && sysState != EMERGENCY_STOP && runPhase == RUN_NONE) {
    static bool wmComboLatch = false, wmEntLatch = false, wmBackLatch = false;
    bool up = (digitalRead(BTN_UP) == LOW), dn = (digitalRead(BTN_DOWN) == LOW);
    bool ent = (digitalRead(BTN_ENTER) == LOW), bk = (digitalRead(BTN_BACK) == LOW);

    if (!wmChooser) {
      if (up && dn) {                                     // UP+DOWN opens the options
        if (!wmComboLatch) { wmComboLatch = true; wmChooser = true; wmSel = 0; wakeBacklight(); }
      } else wmComboLatch = false;
    } else {
      if (up && !dn)      { if (!wmComboLatch) { wmComboLatch = true; wmSel ^= 1; wakeBacklight(); } }
      else if (dn && !up) { if (!wmComboLatch) { wmComboLatch = true; wmSel ^= 1; wakeBacklight(); } }
      else if (!up && !dn) wmComboLatch = false;
      if (ent) {
        if (!wmEntLatch) {
          wmEntLatch = true; wmChooser = false; wakeBacklight();
          if (wmSel == 0) { wmPeek = true; lcdPage = PAGE_HOME; logEvent("ESP1", "ACT", "MANUAL|PEEK|LCD"); }
          else            webManualRevoke();
        }
      } else wmEntLatch = false;
      if (bk) { if (!wmBackLatch) { wmBackLatch = true; wmChooser = false; wakeBacklight(); } }
      else wmBackLatch = false;
    }
    for (int i = 0; i < 5; i++) last[i] = digitalRead(pins[i]);   // swallow every other edge
    return;
  }
  // Peeking at the sensors while the hold stands: BACK returns to the banner. Everything else falls
  // through to the normal data-screen bindings, so the pages can be stepped as usual.
  if (webManualHeld() && wmPeek) {
    static bool peekBackLatch = false;
    if (digitalRead(BTN_BACK) == LOW) {
      if (!peekBackLatch) { peekBackLatch = true; wmPeek = false; wakeBacklight(); }
    } else peekBackLatch = false;
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
          if (uiMode == UI_DATA) { uiMode = UI_MENU; setSel = setRowAt(0); }   // never start on a hidden row
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
// Same frame, no log row: for high-rate traffic (LEVEL, ~3 per second while a pump runs) that would
// otherwise put hundreds of ESP1,CMD rows into the SD log and the Supabase upload per run.
void sendEsp2Quiet(const String &body) {
  esp2Serial.print(FRAME_START); esp2Serial.print(",");
  esp2Serial.print(body);
  esp2Serial.print(","); esp2Serial.println(FRAME_END);
}
// A hold's keep-alive (TEST,HOLD / PRIME_START every 150 ms): only the first of a hold goes into the log. The 17 Nov log
// had 3619 TEST;HOLD and 779 PRIME_START rows -- ~6 SD writes a second while a button was held, which also slowed the
// very loop that sends them.
void sendEsp2KeepAlive(const String &body, bool first) {
  if (first) sendEsp2(body); else sendEsp2Quiet(body);
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
  if (r >= TEST_ROWS) return "Mixer (DC)";           // row 22: dashboard only, not on the LCD list
  if (r == TEST_SERVO_L_ROW) return "Servo>Transfer";   // selector only, inverter stays off
  if (r == TEST_SERVO_R_ROW) return "Servo>Booster";
  return String("Push>Col ") + COL_TAG[r - (TEST_FILL_ROW + 1)];
}

// Load the per-column schedule fields into the working copy for editCol.
static void seedSchedule() {
  editTmp[0] = colSchedMode[editCol];                          // AUTO / MANUAL
  editTmp[1] = COL_WIN_START[editCol] / 60; editTmp[2] = COL_WIN_START[editCol] % 60;
  editTmp[3] = COL_WIN_END[editCol]   / 60; editTmp[4] = COL_WIN_END[editCol]   % 60;
}


// Apply the working copy (editTmp) for the current editItem to live config + NVS. Called by the
// editor's final ENTER and by the three-way confirm dialog's SAVE (companion spec §B).
void commitEditor() {
  switch (editItem) {
    case SET_CLOCK:
      if (rtcChipOk) { rtc.adjust(DateTime(editTmp[0], editTmp[1], editTmp[2], editTmp[3], editTmp[4], 0)); rtcClockWasSet(); }
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
    case SET_EXERCISE:
      exerciseEnabled = (editTmp[0] != 0);
      exerciseRunS    = clampi(editTmp[1], EX_RUN_MIN, EX_RUN_MAX);
      saveExercise();
      break;
    case SET_THRESH:
      soilStartPct = editTmp[0]; fertGap = editTmp[1];
      soilWetPct  = wetEffPct(editTmp[2], editTmp[0]);                              // 18 Nov 2026
      resPausePct = clampi(editTmp[3], (int)RES_LOW_PCT, RES_PAUSE_MAX_PCT);
      saveThresholds();
      logEvent("ESP1", "STATE", "THRESH_SET");
      break;
    case SET_ACVOLT:
      setAcVoltWindow(editTmp[0], editTmp[1], "ESP1");    // saves, sends to ESP2 if on, logs old -> new
      break;
    case SET_ACAMP:                                       // editTmp[0] = max (overcurrent), [1] = min
      setAcAmpWindow(editTmp[1], editTmp[0], "ESP1");     // saves, sends to ESP2 if on, logs old -> new
      break;
    case SET_FORCE:
      // Race fix: refuse to clobber an already-armed/running Force Run (web-armed or LCD-armed) --
      // fcPhase is the one shared authoritative state; the Firebase FORCE_RUN handler already
      // checks it, this was the missing side.
      if (fcPhase != FP_NONE) {
        logEvent("ESP1", "STATE", "FORCE_BLOCKED|ALREADY_ARMED");
        forceBlockedLow = false;
        forceBlockedFlash = true; forceBlockedUntilMs = millis() + 3000;
        uiMode = UI_DATA; lcdPage = PAGE_HOME; wakeBacklight();
        break;
      }
      // Say so now, not when the countdown ends: sendForceWorkOrder would refuse this at fire time.
      // The editor's 2.0 L default with any nutrient lands here.
      { bool batchOk = true;
        for (int s = 0; s < ((fcMode == 3) ? 2 : 1); s++)
          if (!forceBatchOk(fcSeq[s].dL / 10.0f, FLUSH_PCT, MIXING_TANK_SAFE_MIN, fcSeq[s].ml[0], fcSeq[s].ml[1], fcSeq[s].ml[2])) batchOk = false;
        if (!batchOk) {
          logEvent("ESP1", "STATE", "FORCE_BLOCKED|BATCH_LOW");
          forceBlockedLow = true;
          forceBlockedFlash = true; forceBlockedUntilMs = millis() + 4000;
          uiMode = UI_DATA; lcdPage = PAGE_HOME; wakeBacklight();
          break;
        } }
      // A column that is OFF (e.g. "Turn OFF column" in the cancel menu) is switched back ON (2026-11-17). The
      // operator picked it here by name, and the editor said so before ENTER ("Col A OFF, ARM=ON"). Before, the
      // run was armed, counted down, and was then dropped as COL_DISABLED -- and the column stayed off for the schedule.
      { uint8_t want = fcModeColumns(fcMode);
        for (int b = 0; b < NUM_COLUMNS; b++)
          if ((want & (1 << b)) && !COLUMN_ENABLED[b]) {
            COLUMN_ENABLED[b] = true; saveColEnable(b);
            logEvent("ESP1", "ACT", String("FORCE|COL_ON|COL_") + COL_TAG[b]);
          } }
      // ARM, don't run. forceTick() fires it after the countdown and re-checks safety THEN, because
      // conditions can change during a delay of up to 5 minutes. Return to UI_DATA so automation
      // (and the fire check, which requires uiMode == UI_DATA) is live while the clock runs.
      fcSlot = 0; fcPhase = FP_WAIT;
      // Resolve both slot masks from fcMode now (see ForceSeq::mask). Slot 1 is only used by A-then-B.
      fcSeq[0].mask = (fcMode == 0) ? 0b001 : (fcMode == 1) ? 0b010 : (fcMode == 2) ? 0b011 : 0b001;
      fcSeq[1].mask = 0b010;
      fcCmdId[0] = '\0'; fcFromWeb = false;        // armed at the LCD, not from the dashboard
      fcFireAtMs = millis() + (unsigned long)fcSeq[0].delayS * 1000UL;
      uiMode = UI_DATA; lcdPage = PAGE_HOME; wakeBacklight();
      logEvent("ESP1", "ACT", String("FORCE|ARM|") + FC_MODE_NAMES[fcMode] +
                              "|L=" + String(fcSeq[0].dL / 10.0f, 1) +
                              "|mL=" + String(fcSeq[0].ml[0]) + "/" + String(fcSeq[0].ml[1]) + "/" + String(fcSeq[0].ml[2]) +
                              "|in=" + String(fcSeq[0].delayS) + "s");
      break;
  }
}

// Restore operational config to compiled defaults (companion spec §C). Resets automation settings;
// KEEPS calibration (separate NVS namespace), column-enable flags (physical wiring), and WiFi/TS
// connectivity setup. Idle-only; no reboot. EC/pH safe windows are compile constants (not in NVS,
// re-sent each work order), so there is nothing stale to ACK-push here. The AC supply window (PZEM) is
// reset too; ESP2 is off while idle and gets it from its next READY.
void restoreDefaults() {
  for (int c = 0; c < NUM_COLUMNS; c++) {
    col[c].mode = MODE_AUTO;
    col[c].targetN = col[c].targetP = col[c].targetK = 0.0f; col[c].targetPH = 6.0f;
    col[c].name[0] = '\0';
    colSchedMode[c] = SCHED_AUTO;
    COL_WIN_START[c] = DEF_WIN_START[c]; COL_WIN_END[c] = DEF_WIN_END[c];
    // One watering a day again, and the later windows back to their compiled values (2026-11-05).
    static const uint16_t EXTRA_DEF[MAX_RUNS_PER_DAY - 1][2] = { { 12*60, 13*60 }, { 16*60, 17*60 }, { 18*60, 19*60 } };
    colRuns[c] = 1;
    for (int x = 0; x < MAX_RUNS_PER_DAY - 1; x++) { COL_EXTRA_START[c][x] = EXTRA_DEF[x][0]; COL_EXTRA_END[c][x] = EXTRA_DEF[x][1]; }
    colOpManual[c] = 0; planClear(colPlan[c]);   // Automatic, no day plan (2026-11-18)
    colSoilStart[c] = colSoilWet[c] = -1;        // the default Soil start / Too wet again (2026-11-18)
    saveColumn(c); saveSchedule(c); saveColumnPlan(c); saveColumnThresh(c);
  }
  // Audit fix (2026-09-10): restoreDefaults() is documented as "reset to compiled defaults" -- this
  // was hardcoding 20.0f while the actual compiled global default (line 134, FERTIGATE_TRIGGER_GAP)
  // is 30.0f. soilStartPct here already matches its own compiled default; fertGap was the one silent
  // outlier, making Restore Defaults leave a materially more aggressive fertigation trigger than the
  // function's own contract promises.
  soilStartPct = 35; fertGap = 30.0f;
  soilWetPct = SOIL_WET_DEFAULT_PCT; resPausePct = (int)RES_LOW_PCT;   // 18 Nov 2026: 90 % and 18 %
  saveThresholds();
  setAcVoltWindow(AC_V_DEFAULT_LOW, AC_V_DEFAULT_HIGH, "ESP1");   // AC supply window back to 215-240 V
  setAcAmpWindow(AC_A_DEFAULT_MIN, AC_A_DEFAULT_MAX, "ESP1");     // AC current limits back to 0.5 / 3.0 A
  logEvent("ESP1", "RESET", "CONFIG|DEFAULTS|kept cal+cols+wifi");
}

// Seed the working copy when an editor is opened.
void enterEditor(int item) {
  editItem = item; editField = 0; editCol = 0; editDirty = false;
  switch (item) {
    case SET_CLOCK: {
      DateTime n = rtcOk ? rtcNow() : DateTime(2026, 1, 1, 0, 0, 0);
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
    case SET_THRESH:   editTmp[0] = soilStartPct; editTmp[1] = (int)fertGap;
                       editTmp[2] = soilWetPct; editTmp[3] = (int)resPauseLevel(); break;   // + Too wet, pause (18 Nov)
    case SET_ACVOLT:   editTmp[0] = acVMin; editTmp[1] = acVMax; break;
    case SET_ACAMP:    editTmp[0] = acAMax10; editTmp[1] = acAMin10; break;   // overcurrent first
    case SET_EXERCISE: editTmp[0] = exerciseEnabled ? 1 : 0; editTmp[1] = exerciseRunS; break;
  }
  uiMode = UI_EDIT;
}

// clampi() -- the UI_EDIT / THRESH clamp helper -- moved to include/pure_math.h.

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
    // Navigate in VISIBLE-position space so the wrap skips a hidden SET_ENABLE row.
    if (i == 0 || i == 1) {
      int n = setRowCount(), pos = setPosOf(setSel);
      pos = (i == 0) ? (pos + n - 1) % n : (pos + 1) % n;
      setSel = setRowAt(pos);
    }
    else if (i == 3) uiMode = UI_DATA;                         // BACK = exit
    else if (i == 2) {                                         // ENTER = open item
      if (setSel == SET_EXIT) { uiMode = UI_DATA; }
      else if (setSel == SET_ENABLE) {                          // the escape hatch (re-enable only)
        setActuationsEnabled(true, "LCD");
        uiMode = UI_DATA; setSel = setRowAt(0);
      }
      else if (setSel == SET_LOCK) {                            // lock the LCD (Part D)
        lcdLocked = true; saveLock();
        fbRequestPush();                                       // the website locks with it (2026-11-05)
        logEvent("ESP1", "STATE", "LCD_LOCK");
        uiMode = UI_DATA;                                      // back to data view (automation unaffected)
      }
      else if (setSel == SET_TESTING) {
        // Idle-only (spec sec.18.10.8.1). Power ESP2 via the GPIO4 relay, PRIME it (let it
        // boot), then send TEST,ENTER repeatedly until it ACKs -- the single boot READY can be
        // lost on a cold relay power-up (see testHoldTick arming retry).
        if (sysState != IDLE_STATE) return;
        if (actuationsDisabled) {                              // the lockout means nothing energizes; the row says so
          logEvent("ESP1", "ACT", "TEST|BLOCKED|ACTUATIONS_OFF");
          return;
        }
        esp2SetPower(true);                                    // power ESP2 -> boot
        testArmPending = true;
        esp2TestArmed = false; testArmTries = 0; lastTestArmMs = 0;
        esp2WarmupMs = millis();                               // prime reference (TEST_PRIME_MS)
        setState(TEST_MODE);
        uiMode = UI_TEST; testSel = 0; testOnBit = -1;
        // No immediate send: the primed retry in testHoldTick owns arming (after TEST_PRIME_MS).
      }
      else if (setSel == SET_BATTSW) {                          // clear a latched battery-switch fault
        if (sysState != IDLE_STATE || !bswLatched) return;      // idle-only; nothing to clear otherwise
        bswClear("LCD");
      }
      else if (setSel == SET_TANKCHK) {                         // the operator has looked at the mixing tank
        if (sysState != IDLE_STATE) return;                     // idle-only, like clearing the switch fault
        tankCheckClear("LCD");
        uiMode = UI_DATA; setSel = setRowAt(0);                 // this row has just disappeared
      }
      else if (setSel == SET_WIFI) {                            // toggle STA WiFi + telemetry (persisted)
        wifiEnabled = !wifiEnabled; saveWifiEn();
        logEvent("ESP1", "CMD", wifiEnabled ? "WIFI|ON" : "WIFI|OFF");   // netTask gate acts next iteration
      }
      else if (setSel == SET_SOFTAP) {                          // toggle the SoftAP provisioning portal
        if (portalActive || portalRequested) { portalCancel = true; logEvent("ESP1", "CMD", "WIFI_PORTAL|CANCEL"); }
        else { portalRequested = true; logEvent("ESP1", "CMD", "WIFI_PORTAL|START"); }   // netTask brings up the AP (banner)
      }
      else if (setSel == SET_FORCE) {                            // operator force run: idle-only
        // webManualHeld covers the race where the hold lands while the operator is already in the
        // menu -- normally the banner has swallowed the buttons long before they get here.
        if (sysState != IDLE_STATE || wo.active || pendingRun.active || esp2Held || webManualHeld()) return;
        fcField = 0; editItem = SET_FORCE; editDirty = false; uiMode = UI_EDIT;
      }
      else if (setSel == SET_CALIB) enterCal();                 // idle-only calibration (§A.6)
      else if (setSel == SET_DIAG)  enterDiag();                // read-only raw sensor diagnostics
      else if (setSel == SET_RESTORE) {                          // Restore Defaults (§C): idle-only, double-confirm
        if (sysState != IDLE_STATE) return;
        restoreConfirm = true; restoreSel = 0;                  // default cursor = NO
      }
      else if (setSel == SET_RESET) {                            // manual ESP1 reboot: double-confirm
        if (rebootBusyReason()) return;                          // the row reads [BUSY]; loop() refuses too
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
        if (confirmSel == 0) {                                  // SAVE
          // SET_FORCE's own commitEditor() case sets uiMode=UI_DATA itself, specifically so
          // forceTick()'s fire-time gate (which requires UI_DATA) doesn't spuriously abort the
          // just-armed run. Every other editor relies on THIS generic dialog to set uiMode, so
          // the blanket UI_MENU below must not stomp on that one case's own choice.
          bool wasForce = (editItem == SET_FORCE);
          commitEditor();
          editConfirm = false; editDirty = false; editItem = -1;
          if (!wasForce) uiMode = UI_MENU;
        }
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
          if (editField == 0) editTmp[0] = clampi(editTmp[0] + delta, RTC_YEAR_MIN, RTC_YEAR_MAX);
          else if (editField == 1) editTmp[1] = clampi(editTmp[1] + delta, 1, 12);
          else if (editField == 2) editTmp[2] = clampi(editTmp[2] + delta, 1, 31);
          else if (editField == 3) editTmp[3] = clampi(editTmp[3] + delta, 0, 23);
          else if (editField == 4) editTmp[4] = clampi(editTmp[4] + delta, 0, 59);
          // Audit fix (2026-09-08): day-of-month was clamped to a flat 1..31 regardless of the
          // year/month also being edited here, so e.g. Feb 30 could be dialed in and committed
          // straight to the DS3231, which has no validation of its own to catch it.
          if (editField == 0 || editField == 1 || editField == 2)
            editTmp[2] = clampi(editTmp[2], 1, daysInMonth(editTmp[0], editTmp[1]));
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
          // Audit fix (2026-09-09): controlTick()'s window check (`mod < ws || mod > we`) has no
          // midnight-wraparound support, so a start>=end window silently and permanently disables
          // the column with no error surfaced on this 3-line display. Auto-correct in real time
          // (same approach as the SET_CLOCK day/month re-clamp above) rather than let an invalid
          // window ever be committed.
          if (editField >= 2 && editField <= 5) {
            int startMin = editTmp[1] * 60 + editTmp[2];
            int endMin   = editTmp[3] * 60 + editTmp[4];
            if (startMin >= endMin) {
              if (editField == 2 || editField == 3) {          // start just edited -> push end forward
                int e = clampi(startMin + 1, 0, 1439);
                editTmp[3] = e / 60; editTmp[4] = e % 60;
              } else {                                          // end just edited -> pull start back
                int s = clampi(endMin - 1, 0, 1439);
                editTmp[1] = s / 60; editTmp[2] = s % 60;
              }
            }
            // Several windows a day (2026-11-05): window 1, the one edited here, stays clear of window 2, which
            // is set on the website. Same real-time correction as above, so an overlap can never be committed.
            if (editTmp[0] == SCHED_MANUAL && colRuns[editCol] > 1) {
              int limit = COL_EXTRA_START[editCol][0] - 1;     // the last minute window 1 may reach
              if (editTmp[3] * 60 + editTmp[4] > limit) { editTmp[3] = limit / 60; editTmp[4] = limit % 60; }
              if (editTmp[1] * 60 + editTmp[2] >= limit) { int s = limit - 1; editTmp[1] = s / 60; editTmp[2] = s % 60; }
            }
          }
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

      case SET_FORCE: {
        // field 0 = mode; then 5 fields per active slot: litres, mlA, mlB, mlC, delay.
        int slots    = (fcMode == 3) ? 2 : 1;
        int lastField = slots * 5;                       // fields 1..slots*5
        if (delta) {
          if (fcField == 0) {
            fcMode = (uint8_t)((fcMode + (delta > 0 ? 1 : 3)) % 4);
            if (fcMode != 3) fcField = 0;                // mode change re-anchors the cursor
          } else {
            int s = (fcField - 1) / 5, f = (fcField - 1) % 5;
            ForceSeq &q = fcSeq[s];
            if      (f == 0) q.dL     = (uint16_t)clampi(q.dL + delta * FC_STEP_DL, 5, (int)(FORCE_MAX_LITERS * 10));
            else if (f <= 3) q.ml[f-1]= (uint16_t)clampi(q.ml[f-1] + delta * FC_STEP_ML, 0, (int)FORCE_MAX_DOSE_ML);
            else             q.delayS = (uint16_t)clampi(q.delayS + delta * FC_STEP_DELAY, FC_DELAY_MIN, FC_DELAY_MAX);
          }
        } else if (i == 2) {                             // ENTER advances; past the last field = ARM
          if ((int)++fcField > lastField) {
            commitEditor();                              // arms the countdown + returns to UI_DATA
            editDirty = false; editItem = -1;
          }
        }
        break;
      }

        case SET_EXERCISE:
          // field 0 = on/off, field 1 = seconds. The seconds field is still editable while OFF so
          // the length can be set before switching it back on, rather than in two separate visits.
          if (delta) {
            if (editField == 0) editTmp[0] = !editTmp[0];
            else                editTmp[1] = clampi(editTmp[1] + delta, EX_RUN_MIN, EX_RUN_MAX);
          } else if (i == 2) {
            if (++editField > 1) { commitEditor(); editDirty = false; uiMode = UI_MENU; editItem = -1; }
          }
          break;
      case SET_THRESH:
        // 18 Nov 2026: + Too wet (kept THRESH_MIN_GAP_PCT over soil start) and the reservoir pause (18-50 %).
        if (delta) {
          if (editField == 0) editTmp[0] = clampi(editTmp[0] + delta, 0, 100);
          else if (editField == 1) editTmp[1] = clampi(editTmp[1] + delta, 0, 500);
          else if (editField == 2) editTmp[2] = clampi(editTmp[2] + delta, editTmp[0] + THRESH_MIN_GAP_PCT, 100);
          else if (editField == 3) editTmp[3] = clampi(editTmp[3] + delta, (int)RES_LOW_PCT, RES_PAUSE_MAX_PCT);
        } else if (i == 2) {
          if (editField == 1) editTmp[2] = wetEffPct(editTmp[2], editTmp[0]);   // a raised start pushes Too wet up
          if (++editField > 3) { commitEditor(); editDirty = false; uiMode = UI_MENU; editItem = -1; }
        }
        break;
      case SET_ACVOLT:
        // 1 V a press. Each limit stops AC_V_MIN_GAP short of the other, so the pair is always valid.
        if (delta) {
          if (editField == 0) editTmp[0] = clampi(editTmp[0] + delta, AC_V_LIMIT_LOW, editTmp[1] - AC_V_MIN_GAP);
          else                editTmp[1] = clampi(editTmp[1] + delta, editTmp[0] + AC_V_MIN_GAP, AC_V_LIMIT_HIGH);
        } else if (i == 2) {
          if (++editField > 1) { commitEditor(); editDirty = false; uiMode = UI_MENU; editItem = -1; }
        }
        break;
      case SET_ACAMP:
        // 0.1 A a press. field 0 = overcurrent (max), field 1 = no-current (min); each stops AC_A_MIN_GAP
        // short of the other, so the pair is always valid.
        if (delta) {
          if (editField == 0) editTmp[0] = clampi(editTmp[0] + delta, editTmp[1] + AC_A_MIN_GAP, AC_A_LIMIT_HIGH);
          else                editTmp[1] = clampi(editTmp[1] + delta, AC_A_LIMIT_LOW, editTmp[0] - AC_A_MIN_GAP);
        } else if (i == 2) {
          if (++editField > 1) { commitEditor(); editDirty = false; uiMode = UI_MENU; editItem = -1; }
        }
        break;
    }
    return;
  }
}

/* ---- Dead-man relay hold: stream TEST,HOLD while ENTER is physically held ---
 * (spec sec.18.10.8.2). Runs every loop while in UI_TEST; ESP2 owns the timeout
 * and the 30 s hard cap. */
/* =============================================================================
 *  REMOTE PUSH-TO-COLUMN PULSE  --  bounded, no dead-man needed
 * ==========================================================================
 * The LCD Testing screen is a dead-man: testHoldTick() streams TEST,HOLD only while the physical
 * ENTER button is down. A browser cannot hold a dead-man switch, so this drives the SAME ESP2 combo
 * rows (17/18/19 = inverter + column valve + booster) from a timer instead.
 *
 * That is safe because the dead-man is enforced on ESP2, not here: ESP2's TEST_HOLD_TIMEOUT_MS (400 ms)
 * drops the relay if the HOLD stream stops for any reason -- ESP1 crashing, WiFi dying, the loop
 * stalling -- and TEST_HARD_CAP_MS (30 s) caps it regardless. So the worst case of a lost link is a
 * relay that switches off ~400 ms later, which is exactly what the physical button does on release.
 *
 * Deliberately leaves uiMode == UI_DATA: testHoldTick() returns early unless UI_TEST, so the two
 * never drive the relay at the same time. sysState goes to TEST_MODE, which is what stops
 * controlTick()/exerciseTick() from starting a run underneath us.                                   */
// Shut the pulse down and hand ESP2 back. `ok` distinguishes a clean finish from an abort.
static void pulseFinish(bool ok, const char *why) {
  if (puPhase == PU_NONE) return;
  if (puPhase == PU_RUN) sendEsp2("TEST,RELEASE");     // belt: ESP2 would also time out on its own
  sendEsp2("TEST,EXIT");
  esp2TestArmed = false; testArmPending = false;
  int  row = puRow;
  bool sawFlow = puFlowSeen;
  unsigned long pulses = puFlowPulses;
  bool sawLevel = puLevelSeen;
  float levelL = puLevelL;
  bool wasHold = puHoldMode;
  puPhase = PU_NONE; puRow = -1; puFlowSeen = false; puFlowPulses = 0; puHoldMode = false;
  puLevelSeen = false; puLevelL = 0.0f;
  if (wasHold) testHoldLatched = true;     // no re-arm until the page sends want:false (see its declaration)
  esp2SetPower(false);
  if (sysState == TEST_MODE) setState(IDLE_STATE);

  /* Turn the meter count into the answer the operator actually wants: did fluid move? A row with no
   * meter (mixer, a bare valve) says so explicitly -- "no flow sensor" must never be mistaken for
   * "nothing moved", which is a hardware fault. Volume is reported in mL below a litre because the
   * dosing pumps deliver ~12 mL in a 15 s test and "0.01 L" tells you nothing. */
  String detail = why;
  float k = pulseKForRow(row);
  bool servoRow = (row == TEST_SERVO_L_ROW || row == TEST_SERVO_R_ROW);
  bool servoMissed = false;                // a servo move that was not confirmed answers "failed", not "completed"
  if (ok && servoRow) {
    // A servo row moves the pump selector only (inverter off): the answer is whether the Nano confirmed it. It
    // replaces "pulse complete" so it fits the 47 characters of the website's reply field (2026-11-16: the old
    // "pulse complete; SERVO OK - selector moved to the TRANSFER side" arrived cut off before the side's name).
    if (testServoDoneRow == row) {
      detail = String("SERVO OK: selector on the ") + (row == TEST_SERVO_L_ROW ? "TRANSFER" : "BOOSTER") + " side";
    } else {
      servoMissed = true;
      detail = puServoFailWhy[0] ? String("SERVO FAIL: ") + puServoFailWhy
                                 : String("SERVO NOT CONFIRMED: check the Nano and servo");
    }
  } else if (ok && row >= TEST_FILL_ROW && row <= TEST_FILL_ROW + NUM_COLUMNS) {
    // Fill / Push combo (web v.4): the mixing-tank ultrasonic answers. Fill should raise it, Push lower it.
    bool fill = (row == TEST_FILL_ROW);
    if (!sawLevel || levelL <= -98.0f) detail += "; no tank level to compare";
    else {
      float moved = fill ? levelL : -levelL;
      detail += (moved >= 0.3f) ? ("; LEVEL OK " + String(fill ? "+" : "-") + String(fabsf(levelL), 2) + " L")
                                : ("; NO LEVEL CHANGE (" + String(levelL, 2) + " L)");
    }
  } else if (ok) {
    if (k <= 0.0f) {
      detail += "; no flow sensor on this output";
    } else if (!sawFlow) {
      detail += "; no flow report from ESP2";
    } else {
      float litres = (float)pulses / k;
      String vol = (litres < 1.0f) ? (String(litres * 1000.0f, 1) + " mL") : (String(litres, 2) + " L");
      detail += pulses ? ("; FLOW OK " + String(pulses) + " pulses ~" + vol)
                       : String("; NO FLOW - 0 pulses, check pump, line or sensor");
    }
  }
  logEvent("ESP1", "ACT", String(wasHold ? "TESTHOLD|" : "PULSE|") + (ok ? "DONE|" : "ABORT|") + detail);
  if (puCmdId[0]) {
    firebaseQueueStatus(puCmdId, (ok && !servoMissed) ? "completed" : "failed", detail.c_str());
    puCmdId[0] = '\0';
  }
}

void pulseTick() {
  // Web hold-to-run hand-off (2026-09-10): firebasePollTestHold() (core 0) only ever writes
  // testHoldReqWant/Bit/Fresh; this function runs on core 1 (called from loop(), same call site as
  // testHoldTick()), so THIS is the safe place to actually arm/release -- setState()/logEvent()/
  // pulseFinish() must never be called from core 0. Checked before the PU_NONE early-return below,
  // since a fresh start request arrives exactly while puPhase IS PU_NONE.
  if (testHoldReqFresh) {
    testHoldReqFresh = false;
    if (!testHoldReqWant) {
      if (puHoldMode) pulseFinish(true, "web hold released");   // explicit release -- don't wait out the timeout
      testHoldLatched = false;                                  // button is up: the next press may arm again
    } else if (puPhase == PU_NONE && !testHoldLatched) {
      // Same idle/safety gate TEST_PULSE's own handler already uses, so a hold request is silently
      // ignored (not armed) unless the rig is genuinely free to take it -- there is no queued
      // command/status entry for this node the way there is for TEST_PULSE, so there is nothing to
      // reject "into"; the website's own UI only offers this button once its normal freshness/
      // approval checks already pass.
      int row = pulseRowForTarget(testHoldReqBit);
      if (row >= 0 && !(row >= 17 && row <= 19 && !COLUMN_ENABLED[row - 17]) &&
          !actuationsDisabled && sysState == IDLE_STATE && uiMode == UI_DATA && !wo.active &&
          !pendingRun.active && !pendingExercise.active && !esp2Held && fcPhase == FP_NONE && !fbRemoteExerciseId[0] &&
          !(testRowUsesInverter(row) && secBattBlocksAc())) {
        puRow = row; puHoldMode = true; puCmdId[0] = '\0';
        esp2SetPower(true);
        esp2TestArmed = false; testArmPending = false; testArmTries = 0; lastTestArmMs = 0;
        esp2WarmupMs = millis();
        puArmMs = millis();
        puPhase = PU_ARM; testServoDoneRow = -1; puServoFailWhy[0] = '\0';
        setState(TEST_MODE);
        logEvent("FIREBASE", "CMD", String("TESTHOLD|START|") + testHoldReqBit);
      }
    }
    // else: already holding (or something else is running) -- this was just a keep-alive refresh, or
    // a start-request arriving while busy; ignored either way, same as before.
  }

  if (puPhase == PU_NONE) return;

  // Anything that makes actuation unsafe ends the pulse immediately. Checked every tick, not just
  // at start: a fault or an operator lockout during a 15 s pulse must cut it short.
  if (actuationsDisabled)                 { pulseFinish(false, "actuations disabled"); return; }
  if (esp2Held || sysState == EMERGENCY_STOP) { pulseFinish(false, "fault or emergency stop"); return; }
  if (uiMode != UI_DATA)                  { pulseFinish(false, "operator opened a menu"); return; }

  if (puPhase == PU_ARM) {
    // Same primed-arming pattern as testHoldTick: the single boot READY is easily lost on a cold
    // relay power-up, so re-send TEST,ENTER until ESP2 ACKs.
    if (esp2TestArmed) {
      puPhase = PU_RUN;
      if (puHoldMode) {
        testHoldLastFreshMs = millis();      // start the browser keep-alive clock fresh, not stale from PU_ARM
        testHoldRunMs = millis();            // TEST_HOLD_MAX_MS counts from the relay actually running
        logEvent("ESP1", "ACT", String("TESTHOLD|RUN|") + testRowName(puRow));
      } else {
        puEndMs = millis() + (unsigned long)puSeconds * 1000UL;
        puInvOnSeen = false;
        // Fill/Push combos (2026-11-16): the pump only runs after the battery switch, AC dead wait, servo pre-seat and
        // inverter confirm. PUMP,ON restarts the seconds; until then this is the budget, so a 5 s test no longer ends
        // before its pump has started. ESP2's own combo cap still bounds it.
        if (puRow >= 16 && puRow <= 19) puEndMs += PULSE_AC_START_MAX_MS;
        logEvent("ESP1", "ACT", String("PULSE|START|") + testRowName(puRow) + "|" + puSeconds + "s");
      }
      puLastSendMs = 0;
      return;
    }
    if (millis() - puArmMs > PULSE_ARM_TIMEOUT_MS) { pulseFinish(false, "ESP2 never entered test mode"); return; }
    if ((unsigned long)(millis() - esp2WarmupMs) >= TEST_PRIME_MS
        && (unsigned long)(millis() - lastTestArmMs) >= TEST_ARM_RETRY_MS
        && testArmTries < TEST_ARM_MAX_TRIES) {
      lastTestArmMs = millis(); testArmTries++;
      sendEsp2("TEST,ENTER");
    }
    return;
  }

  if (puPhase == PU_REPORT) {
    // Waiting on ESP2's TEST,FLOW answer so the result can say whether fluid actually moved --
    // the whole point of the test. Bounded, so a silent ESP2 cannot strand the command.
    if (puFlowSeen || puLevelSeen || millis() - puReportMs > PULSE_REPORT_WAIT_MS) pulseFinish(true, "pulse complete");
    return;
  }

  // PU_RUN: keep-alive stream at 150 ms, comfortably inside ESP2's 400 ms release timeout.
  // Hold-mode has no fixed puEndMs -- it ends when the browser stops refreshing testHoldLastFreshMs
  // (button released, tab closed, network dropped), bounded by TEST_HOLD_BROWSER_TIMEOUT_MS, and in
  // any case by TEST_HOLD_MAX_MS of continuous running. Either way ESP2's OWN 400 ms dead-man is what
  // actually protects the relay if ESP1 itself vanishes.
  // A servo row's move is over once the Nano confirmed it or ESP2 reported it failed (2026-11-16): end the pulse
  // then rather than holding ESP2 in Testing for the rest of its seconds. The seconds stay the upper bound.
  bool servoSettled = !puHoldMode && (puRow == TEST_SERVO_L_ROW || puRow == TEST_SERVO_R_ROW) &&
                      (testServoDoneRow == puRow || puServoFailWhy[0]);
  bool timeUp = puHoldMode
    ? (millis() - testHoldLastFreshMs > TEST_HOLD_BROWSER_TIMEOUT_MS ||
       millis() - testHoldRunMs >= TEST_HOLD_MAX_MS)
    : ((long)(millis() - puEndMs) >= 0 || servoSettled);
  if (timeUp) {
    sendEsp2("TEST,RELEASE");                  // ask for the meter count; ESP2 answers TEST,FLOW
    puPhase = PU_REPORT; puReportMs = millis();
    return;
  }
  if (millis() - puLastSendMs >= 150) {
    bool first = (puLastSendMs == 0);
    puLastSendMs = millis();
    sendEsp2KeepAlive(String("TEST,HOLD,") + puRow, first);
  }
}

// The Testing header while an inverter row was refused: which reason, padded to the 20 columns (2026-11-17).
static const char *testBlockedHdr() {
  static char b[21];
  const char *w = secBlockLcd();
  snprintf(b, sizeof(b), "%-20s", *w ? w : "AC pumps blocked");
  return b;
}

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

  static bool wasHeld = false, pressBlocked = false, pressTooSoon = false;
  static unsigned long lastSendMs = 0;
  static HoldBtn enterBtn = { false, 0 };
  bool held = holdBtnRead(enterBtn, BTN_ENTER);              // a release only after BTN_RELEASE_MS up

  if (held) {
    if (!wasHeld) {                                            // press edge
      wasHeld = true; lastSendMs = 0;
      // Too soon after the last release (ACT_REARM_MS): ignore this press entirely -- release and press again.
      pressTooSoon = actRearmWaiting();
      if (pressTooSoon) { actWaitMsgUntilMs = millis() + 2000; return; }
      pressBlocked = testRowUsesInverter(testSel) && secBattBlocksAc();
      if (pressBlocked) {                                      // inverter row on a low/unmonitored secondary
        testBlockedUntilMs = millis() + 3000;
        logEvent("ESP1", "ACT", String("TEST|BLOCKED|") + secBlockTag() + "|" + testRowName(testSel));
      } else {
        testOnBit = testSel; testServoDoneRow = -1;   // a new press: forget the last servo confirmation
        logEvent("ESP1", "ACT", String("TEST|START|") + testRowName(testSel));
      }
    }
    if (pressBlocked || pressTooSoon) return;                  // nothing was started, nothing to keep alive
    if (millis() - lastSendMs >= 150) {                        // keep-alive stream (< ESP2 timeout)
      bool first = (lastSendMs == 0);
      lastSendMs = millis();
      sendEsp2KeepAlive(String("TEST,HOLD,") + testSel, first);
    }
  } else if (wasHeld) {                                        // release edge
    wasHeld = false; testOnBit = -1;
    if (pressTooSoon) { pressTooSoon = false; return; }        // it never started: no new gap
    if (pressBlocked) { pressBlocked = false; return; }
    sendEsp2("TEST,RELEASE");
    actRearmStart();                                           // the next press waits ACT_REARM_MS
    logEvent("ESP1", "ACT", "TEST|STOP");
  }
}

// One tank's level for the home page: "62%", or why there is none (see PAGE_HOME).
static void tankLcdText(char *b, size_t n, float pct, char st, bool usable) {
  // "62%?" = an echo, but not one ESP2 is given (pings too spread, past the empty mark, ...): see NANO,LEVEL in the log.
  if (pct >= 0.0f) { snprintf(b, n, usable ? "%d%%" : "%d%%?", (int)pct); return; }
  const char *w = (lastTankMs == 0) ? "wait"
                : st == 'E' ? "echo?" : st == 'S' ? "stuck" : st == 'R' ? "range"
                : st == 'F' ? "noisy" : st == 'X' ? "noPng" : "--";
  snprintf(b, n, "%s", w);
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
    // A locked screen must not claim "running OK" while the rig is stopped or locked out -- this is
    // the one line a passer-by reads, and it is exactly the state they need to know is not normal.
    lcd.setCursor(0, 1); lcd.print(sysState == EMERGENCY_STOP ? "E-STOP active!      "
                                 : actuationsDisabled        ? "Actuations DISABLED "
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

  // Force-run countdown banner: the operator armed a run and walked away, so the pending action has
  // to be visible from across the room -- and cancellable without hunting through the menu.
  static bool lcdInForce = false;
  // Never mask a held fault or an E-stop prompt -- those carry choices the operator must answer.
  // Same guard the run UI uses below. forceTick() also refuses to fire in either state.
  /* Web manual-mode banner. Sits below the run screen and the fault/E-stop screens in this ladder --
   * a held fault or a stopped rig still outranks it, because those carry decisions the operator must
   * answer. wmPeek lets them page the sensors without giving up the hold. */
  if (webManualHeld() && !wmPeek && !esp2Held && sysState != EMERGENCY_STOP && runPhase == RUN_NONE) {
    if (!lcdInWebManual) { lcd.clear(); lcdInWebManual = true; }
    if (wmChooser) {
      lcdRow(0, "WEB MANUAL: OPTIONS");
      char b[21];
      snprintf(b, 21, "%cView sensors", wmSel == 0 ? '>' : ' ');   lcdRow(1, b);
      snprintf(b, 21, "%cInterrupt web", wmSel == 1 ? '>' : ' ');  lcdRow(2, b);
      lcdRow(3, "ENT=pick BACK=back");
    } else {
      lcdRow(0, "MANUAL TEST AT WEB");
      lcdRow(1, "Web operator has");
      lcdRow(2, "control of the rig");
      lcdRow(3, "UP+DOWN = options");
    }
    return;
  }
  if (lcdInWebManual) { lcd.clear(); lcdInWebManual = false; }

  // Race-fix feedback: briefly shown instead of arming when the LCD's own Force Run entry was
  // blocked because a run (web- or LCD-armed) is already armed/running. Auto-clears after 3 s and
  // falls through to whatever screen is actually current (e.g. the still-armed web run below).
  if (forceBlockedFlash) {
    if ((long)(millis() - forceBlockedUntilMs) < 0) {
      if (!lcdInForce) { lcd.clear(); lcdInForce = true; }
      char fb[21];
      snprintf(fb, 21, "%-20s", "FORCE RUN BLOCKED");   lcd.setCursor(0, 0); lcd.print(fb);
      if (forceBlockedLow) {
        snprintf(fb, 21, "%-20s", "Too little water");  lcd.setCursor(0, 1); lcd.print(fb);
        snprintf(fb, 21, "Dosing needs %4.2f L", MIXING_TANK_SAFE_MIN / (1.0f - FLUSH_PCT / 100.0f)); lcd.setCursor(0, 2); lcd.print(fb);
        snprintf(fb, 21, "%-20s", "or set nutrients 0"); lcd.setCursor(0, 3); lcd.print(fb);
        return;
      }
      snprintf(fb, 21, "%-20s", "A run is already");    lcd.setCursor(0, 1); lcd.print(fb);
      snprintf(fb, 21, "%-20s", "armed/running.");      lcd.setCursor(0, 2); lcd.print(fb);
      snprintf(fb, 21, "%-20s", "");                    lcd.setCursor(0, 3); lcd.print(fb);
      return;
    }
    forceBlockedFlash = false;
  }

  if (fcPhase == FP_WAIT && !esp2Held && sysState != EMERGENCY_STOP) {
    if (!lcdInForce) { lcd.clear(); lcdInForce = true; }
    char fb[21];
    const ForceSeq &q = fcSeq[fcSlot];
    long left = ((long)fcFireAtMs - (long)millis()) / 1000; if (left < 0) left = 0;
    const char *tag = (fcMode == 3) ? (fcSlot == 0 ? "A" : "B")
                                    : (fcMode == 0 ? "A" : fcMode == 1 ? "B" : "AB");
    lcd.setCursor(0, 0); lcd.print("FORCE RUN ARMED     ");
    snprintf(fb, 21, "COL_%-3s %4.1fL        ", tag, q.dL / 10.0f);       lcd.setCursor(0, 1); lcd.print(fb);
    snprintf(fb, 21, "N%3u P%3u K%3u mL   ", q.ml[0], q.ml[1], q.ml[2]);  lcd.setCursor(0, 2); lcd.print(fb);
    snprintf(fb, 21, "starts %3lds  BACK=X ", left);                      lcd.setCursor(0, 3); lcd.print(fb);
    return;
  }
  if (lcdInForce) { lcd.clear(); lcdInForce = false; }

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
      snprintf(l, 21, "State:%-13s", stateLabel(sysState, actuationsDisabled)); lcdRow(1, l);
      if (battVCalLoaded) snprintf(l, 21, "Bat:%.1fV %s", battV, batteryCritical ? "CRIT" : batteryLow ? "LOW" : "OK");
      else                snprintf(l, 21, "Bat: uncalibrated");
      lcdRow(2, l);
      // A tank with no reading says why instead of "-1%" (2026-11-17): wait = nothing from the Nano yet since this
      // boot; otherwise the Nano's pinger status (E no echo, S echo line stuck, R out of range, F too few good pings,
      // X not pinging) -- the same letters as the log's TANK st=.
      { char rb[8], mb[8];
        tankLcdText(rb, sizeof(rb), sensor.resLevel, sensor.resSt, sensor.resOk);
        tankLcdText(mb, sizeof(mb), sensor.mixLevel, sensor.mixSt, sensor.mixOk);
        snprintf(l, 21, "Res:%s Mix:%s", rb, mb); lcdRow(3, l); }
      break;
    case PAGE_SENSORS:
      snprintf(l, 21, "T:%.1fC H:%.0f%%", sensor.temp, sensor.hum); lcdRow(0, l);
      snprintf(l, 21, "Soil A%d B%d C%d", sensor.soil[0], sensor.soil[1], sensor.soil[2]); lcdRow(1, l);
      snprintf(l, 21, "Light:%.0f", sensor.lux); lcdRow(2, l);
      snprintf(l, 21, "Flow:%.1f L/min", sensor.flow); lcdRow(3, l);
      break;
    case PAGE_COLUMNS:
      for (int c = 0; c < 3; c++) {
        snprintf(l, 21, "%c:%s %s", COL_TAG[c],      // MAN = Manual today: no scheduled run (2026-11-18)
                 COLUMN_ENABLED[c] ? (colManualToday(c) ? "MAN " : col[c].mode == MODE_AUTO ? "AUTO" : "IRR ") : "OFF ",
                 strlen(col[c].name) ? col[c].name : "-");
        lcdRow(c, l);
      }
      snprintf(l, 21, "E2:%s SD:%s RTC:%s", esp2Available ? "OK" : "--", sdOk ? "OK" : "--",
               rtcOk ? "OK" : rtcTimeLost ? "SET" : "--"); lcdRow(3, l);
      break;
    case PAGE_POWER:
      if (battVCalLoaded) {
        snprintf(l, 21, "Main V:%.2f%s", battV, esp2OnSecondary ? "" : " *"); lcdRow(0, l);
        // Signed from the shunt bar (- = charging); the ACS758 fallback shows a plain magnitude.
        if (battFromIna) snprintf(l, 21, "I:%+.2fA P:%.1fW", battIsigned, battP);
        else             snprintf(l, 21, "I:%.2fA P:%.1fW", battI, battP);
        lcdRow(1, l);
      } else {
        snprintf(l, 21, "Main V:uncal%s", esp2OnSecondary ? "" : " *"); lcdRow(0, l);
        snprintf(l, 21, "I:%.2fA P:--", battI); lcdRow(1, l);
      }
      // B2 (2026-11-16, user): the secondary battery's charge % and its power only -- watts NEGATIVE while it charges
      // (secP = V x signed I, + = discharging). No reading: dashes.
      if (secInaOk && secHaveV) snprintf(l, 21, "B2:%4d%%  %+.1fW", secBattPct(secRestV), secP);   // resting charge
      else                      snprintf(l, 21, "B2:  --%%   --W");
      lcdRow(2, l);
      // The battery-switch status sits here (it left the Settings menu): SWok, or SW!! while a fault is latched.
      { const char *sw = SECONDARY_BATT_ENABLED ? (bswLatched ? " SW!!" : " SWok") : (bswLatched ? " SW!!" : "");
        if (inaUsable(powerIna[INA_SOLAR]))          // SD and RTC moved to the Columns page for this row
          snprintf(l, 21, "Sol:%.1fV %.1fA%s", powerIna[INA_SOLAR].v, powerIna[INA_SOLAR].i, sw);
        else
          snprintf(l, 21, "Sol: no INA226%s", sw); }
      lcdRow(3, l);
      break;
    case PAGE_GSM:   // GSM (A7670C) + WiFi/ThingSpeak link health
      lcdRow(0, "GSM + WiFi");
      snprintf(l, 21, "GSM:%-4s Sig:%-3s Q%d",
               gsm.absent() ? "DEAD" : !gsm.ready() ? "INIT" : netRegistered ? "REG" : "NO",
               (lastRssi < 0 || lastRssi == 99) ? "--" : String(lastRssi).c_str(),
               gsm.queued());
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
        // netMux: fbUrl is written from core-0 netTask (portal admin form); this LCD tick runs on
        // core 1. .length()-only, so the risk was a torn/stale glyph for one refresh, not memory
        // unsafety -- fixed anyway since every other fbUrl access in the file is correctly locked.
        xSemaphoreTake(netMux, portMAX_DELAY); bool fbUrlSet = (fbUrl.length() != 0); xSemaphoreGive(netMux);
        const char *fbTag = (!fbUrlSet || !firebaseEnabled) ? "--" : (fbLastOk ? "ok" : "!!");
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
          // A pump-selector hold offers "switch it again" where the others say "Resume normal" (same action).
          const char *nm = (idx == 3 && lastHoldCode == "SERVO_FAIL") ? "Switch servo again" : RECOV_NAMES[idx];
          snprintf(l, 21, "%c%-19s", idx == faultRecovSel ? '>' : ' ', nm); lcdRow(r + 1, l);
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
// Settings-row label with a live [ON]/[OFF] suffix for the two WiFi toggles.
static String setRowLabel(int i) {
  if (i == SET_WIFI)   return String("WiFi        ") + (wifiEnabled ? "[ON]" : "[OFF]");
  if (i == SET_SOFTAP) return String("Setup AP    ") + ((portalActive || portalRequested) ? "[ON]" : "[OFF]");
  if (i == SET_BATTSW) return String("BattSw FAULT>clear");
  if (i == SET_RESET && rebootBusyReason()) return String("Reboot ESP1 [BUSY]");
  if (i == SET_TESTING && actuationsDisabled) return String("Testing     [OFF]");
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
    // The web can re-enable actuations while this menu is open, hiding the row under the cursor.
    if (!setRowVisible(setSel)) setSel = setRowAt(0);
    // A 3-row scrolling list; the two WiFi rows carry a live [ON]/[OFF] suffix.
    // Window over VISIBLE rows, not raw enum indices (SET_ENABLE is hidden unless locked out).
    int n = setRowCount();
    int top = setPosOf(setSel) - 1; if (top < 0) top = 0;
    if (top > n - 3) top = (n > 3) ? n - 3 : 0;
    for (int r = 0; r < 3; r++) {
      int pos = top + r;
      lcd.setCursor(0, r + 1);
      if (pos < n) {
        int idx = setRowAt(pos);
        snprintf(l, 21, "%c%-19s", idx == setSel ? '>' : ' ', setRowLabel(idx).c_str());
      } else snprintf(l, 21, "%-20s", "");
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
    const char *hdr = ((long)(millis() - actWaitMsgUntilMs) < 0) ? "TOO FAST: WAIT 1.5s "
                    : ((long)(millis() - testBlockedUntilMs) < 0) ? testBlockedHdr()
                    : ((long)(millis() - testUseComboUntilMs) < 0) ? "INV: USE FILL/PUSH  "
                    : ((long)(millis() - testServoFailUntilMs) < 0)
                        ? (testInverterNoAc    ? "INVERTER: NO AC     "
                         : testBattSwitchFail  ? "BATT SWITCH FAILED  "
                         : testServoFailAcLive ? "SERVO: AC STILL LIVE" : "SERVO FAIL-chk Nano ")
                    : !esp2Available     ? "TESTING: ESP2 DOWN  "
                    : esp2TestArmed       ? "TESTING (dead-man)  "
                    : (testArmTries >= TEST_ARM_NOACK_HINT) ? "TEST NO ACK-chk link"
                    :                        "TESTING: ARMING...  ";
    lcd.setCursor(0, 0); lcd.print(hdr);
    int top = testSel - 1; if (top < 0) top = 0; if (top > TEST_ROWS - 2) top = TEST_ROWS - 2;
    for (int r = 0; r < 2; r++) {
      int idx = top + r;
      lcd.setCursor(0, r + 1);
      // Fixed layout, full width (2026-11-16): name in exactly 14 columns, the mark always at 16-19, and lcdRow()
      // pads to 20, so scrolling from a long name (Servo>Transfer) to a short one leaves no old characters behind.
      if (idx < TEST_ROWS && testRowVisible(idx))
        snprintf(l, 21, "%c%-14.14s %s", idx == testSel ? '>' : ' ', testRowName(idx).c_str(),
                 idx == testServoDoneRow ? "[OK]" : idx == testOnBit ? "[ON]" : "[ -]");
      else l[0] = 0;                          // empty row: lcdRow() pads it to 20 spaces
      lcdRow(r + 1, l);
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
    case SET_SCHEDULE: {
      // Row 0 lists the column's later windows when it has several a day (set on the website, 2026-11-05).
      // Only window 1 is edited here, and the correction above keeps it clear of window 2.
      String top = "Schedule";
      if (editTmp[0] == SCHED_MANUAL && colRuns[editCol] > 1) {
        top = colRuns[editCol] > 3 ? "+" : "Also ";
        for (int x = 0; x < colRuns[editCol] - 1; x++) top += (x ? " " : "") + hhmm(COL_EXTRA_START[editCol][x]);
      }
      lcd.setCursor(0, 0); snprintf(l, 21, "%-20s", top.c_str()); lcd.print(l);
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
    }
    case SET_COLMODE:
      lcd.setCursor(0, 0); lcd.print("Column Mode         ");
      lcd.setCursor(0, 1);
      snprintf(l, 21, "%cColumn: %c          ", editField==0?'>':' ', COL_TAG[editCol]);
      lcd.print(l);
      lcd.setCursor(0, 2);
      // 20 columns: "IRRIGATION_ONLY" after the 7-char prefix used to clip to "IRRIGATION_ON".
      snprintf(l, 21, "%cMode: %-13s", editField==1?'>':' ',
               editTmp[0]==0 ? "AUTO" : editTmp[0]==1 ? "IRRIG ONLY" : "OFF");
      lcd.print(l);
      break;
    case SET_PRESET:
      lcd.setCursor(0, 0);
      snprintf(l, 21, "%cPreset COL_%c%c%-6s",           // 20 columns ("MANUAL" lost its L at 21)
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
      case SET_EXERCISE:
        lcd.setCursor(0, 0); lcd.print("Pump Exercise       ");
        lcd.setCursor(0, 1);
        snprintf(l, 21, "%cState    %-9s", editField==0?'>':' ', editTmp[0] ? "ON" : "OFF"); lcd.print(l);
        lcd.setCursor(0, 2);
        snprintf(l, 21, "%cRun each %2ds        ", editField==1?'>':' ', editTmp[1]); lcd.print(l);
        lcd.setCursor(0, 3);
        lcd.print(editTmp[0] ? "fires every 2 days  " : "disabled            ");
        break;

    case SET_THRESH:
      // One field per row, no title. Row 3 is the shared edit hint below.
      lcd.setCursor(0, 0);
      snprintf(l, 21, "%cSoil start<%3d%%    ", editField==0?'>':' ', editTmp[0]); lcd.print(l);
      lcd.setCursor(0, 1);
      snprintf(l, 21, "%cFert gap %3d mg/kg  ", editField==1?'>':' ', editTmp[1]); lcd.print(l);
      lcd.setCursor(0, 2);                                  // 18 Nov 2026: Too wet over / reservoir pause under
      snprintf(l, 21, "%cWet>%3d%% %cRes<%2d%%  ", editField==2?'>':' ', editTmp[2], editField==3?'>':' ', editTmp[3]);
      lcd.print(l);
      break;

    case SET_ACVOLT: {
      // Row 0 shows the last AC voltage ESP2 measured, so the limits are set against a real reading.
      char last[6];
      if (acLastV > 0.0f) snprintf(last, sizeof(last), "%dV", (int)(acLastV + 0.5f));
      else                strcpy(last, "---");
      snprintf(l, 21, "AC Voltage last %-4s", last); lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "%cLow  limit %3d V   ", editField==0?'>':' ', editTmp[0]); lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "%cHigh limit %3d V   ", editField==1?'>':' ', editTmp[1]); lcd.setCursor(0, 2); lcd.print(l);
      break;
    }

    case SET_ACAMP: {
      // Row 0 = the last AC pump current ESP2 measured, so the limits are set against a real reading.
      char last[8];
      if (acLastI >= 0.0f) snprintf(last, sizeof(last), "%.1fA", acLastI);
      else                 strcpy(last, "---");
      snprintf(l, 21, "AC Current last%5s", last);                                 lcd.setCursor(0, 0); lcd.print(l);
      snprintf(l, 21, "%cOvercurrent %4.1f A ", editField==0?'>':' ', editTmp[0] / 10.0f); lcd.setCursor(0, 1); lcd.print(l);
      snprintf(l, 21, "%cNo-current  %4.1f A ", editField==1?'>':' ', editTmp[1] / 10.0f); lcd.setCursor(0, 2); lcd.print(l);
      break;
    }

    case SET_FORCE: {
      // Row 0 = mode. Rows 1-2 show the slot the cursor is in, so A-then-B's eleven fields fit a
      // 4-row display without paging: you always see the slot you are editing.
      int slot = (fcField == 0) ? 0 : (fcField - 1) / 5;
      int f    = (fcField == 0) ? -1 : (fcField - 1) % 5;
      const ForceSeq &q = fcSeq[slot];
      snprintf(l, 21, "%cForce: %-12s", fcField==0?'>':' ', FC_MODE_NAMES[fcMode]); lcd.setCursor(0,0); lcd.print(l);
      // Which column this slot actually waters (A-then-B: slot0=A, slot1=B).
      const char *tag = (fcMode == 3) ? (slot == 0 ? "A" : "B")
                                      : (fcMode == 0 ? "A" : fcMode == 1 ? "B" : "AB");
      // 20 columns exactly (2026-11-17: "AB" and the 3-space indent made 21, cutting the last digit / the "s").
      snprintf(l, 21, "%-2s%c%4.1fL %cN%3u %cP%3u", tag,
               f==0?'>':' ', q.dL / 10.0f, f==1?'>':' ', q.ml[0], f==2?'>':' ', q.ml[1]);
      lcd.setCursor(0,1); lcd.print(l);
      snprintf(l, 21, "  %cK%3u  %cstart %3us", f==3?'>':' ', q.ml[2], f==4?'>':' ', q.delayS);
      lcd.setCursor(0,2); lcd.print(l);
      break;
    }
  }
  lcd.setCursor(0, 3);
  if (editItem == SET_FORCE) {
    uint8_t off = 0, want = fcModeColumns(fcMode);
    for (int b = 0; b < NUM_COLUMNS; b++) if ((want & (1 << b)) && !COLUMN_ENABLED[b]) off |= (uint8_t)(1 << b);
    char h[21];
    if (off) snprintf(h, 21, "Col %-3s OFF, ARM=ON ", off == 0b011 ? "A+B" : off == 0b001 ? "A" : "B");
    else     snprintf(h, 21, "%-20s", "UP/DN set ENT next");
    lcd.print(h);
  } else lcd.print("UP/DN edit ENT next ");
}

/* =============================================================================
 *  LOGGING  (buffered CSV to microSD, spec sec.25)
 * ========================================================================== */
/* Which of the ~150 log rows the rig emits deserve a place in the 12-entry dashboard ring.
 * Everything still goes to the SD card; this is purely about what is worth looking at remotely:
 * errors, the start of a run and its receipt, testing from either surface, and SMS traffic.
 * Deliberately excludes the high-volume housekeeping -- CFG, CAL, STATE, SENSOR, POLL, NET -- which
 * would push the interesting rows out of a 12-slot window within minutes. */
static bool webWorthy(const char *type, const String &d) {
  if (!strcmp(type, "FAULT") || !strcmp(type, "RECEIPT") ||
      !strcmp(type, "REJECT") || !strcmp(type, "SMS_ERR")) return true;
  if (!strcmp(type, "ACT")) {
    // Run starts (the matching finish is the POST receipt, so |STOP| is left out as a duplicate).
    if (d.startsWith("IRRIGATION|START") || d.startsWith("FERTIGATION|START")) return true;
    // Testing from BOTH surfaces. TEST| is the LCD dead-man and PULSE| the dashboard push-to-column,
    // but the dashboard's plain "Test pump (5 sec)" button runs the preventive exercise -- so
    // EXERCISE| is website testing too, and omitting it left that button invisible here.
    // TESTHOLD| is the dashboard's own press-and-hold dead-man (2026-09-10) -- omitting it left every
    // arm/finish/abort of that feature invisible here too, the same gap EXERCISE| was added to close.
    if (d.startsWith("TEST|") || d.startsWith("PULSE|") || d.startsWith("EXERCISE|") || d.startsWith("TESTHOLD|")) return true;
    // Operator decisions and anything refused: "why did nothing happen?" is the question this
    // panel exists to answer, so the BLOCKED refusals matter as much as the actions.
    if (d.startsWith("ACTUATION|") || d.startsWith("FORCE|") || d.startsWith("WO|BLOCKED") ||
        d.startsWith("CANCEL|")    || d.startsWith("RESUME|")) return true;
    return false;
  }
  if (!strcmp(type, "GSM"))  return d.startsWith("RX|") || d.startsWith("TX_ERR|") || d.startsWith("TX|GAVE_UP") ||
                                    d.startsWith("TX_UNDELIVERED|") || d.startsWith("MODEM_SILENT") ||
                                    d.startsWith("MODEM_ABSENT") || d.startsWith("MODEM_RESET");
  if (!strcmp(type, "RESP")) return d.startsWith("TIMEOUT|");
  return false;
}

// An event the operator should not have to wait up to a minute to see.
static bool webUrgent(const char *type) {
  return !strcmp(type, "FAULT") || !strcmp(type, "REJECT") || !strcmp(type, "SMS_ERR");
}

void logEvent(const char *source, const char *type, const String &detail) {
  // The schema is timestamp,source,event_type,detail with '|' as the secondary delimiter and NO
  // commas inside detail (sec.25.2 / CLAUDE.md). Several callers pass through raw framed payloads or
  // SMS text that legitimately contain commas -- GARBAGE RAW=, ESP2 RESP, ESP1 CMD, GSM TX -- which
  // silently split into extra CSV columns (~10% of rows). Our own summary parser tolerates it
  // (substring past the 3rd comma), but any external reader of the thesis CSV does not.
  String d = detail;
  d.replace(',', ';');
  // Line breaks too (2026-11-16): outbound SMS are now multi-line, and the driver logs each one as
  // TX_SUBMIT|<text>. A raw '\n' would end the CSV row mid-detail (an inbound multi-line SMS did too).
  d.replace("\r", "");
  d.replace("\n", " / ");
  String stamp = tsString();

  // Dashboard ring first: it must capture the event even if the SD path below declines or the
  // buffer is being dropped. Uses the sanitized `d`, so the web view matches the CSV exactly.
  if (webWorthy(type, d)) {
    WebEvent e = {};
    strlcpy(e.at,     stamp.c_str(),  sizeof(e.at));
    strlcpy(e.src,    source,         sizeof(e.src));
    strlcpy(e.type,   type,           sizeof(e.type));
    strlcpy(e.detail, d.c_str(),      sizeof(e.detail));
    portENTER_CRITICAL(&evtMux);
    webEvt[webEvtHead] = e;
    webEvtHead = (webEvtHead + 1) % WEB_EVT_N;
    if (webEvtCount < WEB_EVT_N) webEvtCount++;
    portEXIT_CRITICAL(&evtMux);
    if (webUrgent(type)) fbRequestPush();
  }

  String row = stamp + "," + source + "," + type + "," + d + "\n";
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
  // Battery-switch quiet window: never be mid-write if the rails drop. Keep buffering (<= 3 s).
  if (battQuietActive()) return;
  if (!force && millis() - lastLogFlushMs < LOG_FLUSH_INTERVAL_MS && logLineCount < LOG_FLUSH_LINES) return;
  lastLogFlushMs = millis();
  if (!sdOk) { logBuf = ""; logLineCount = 0; return; }   // degraded: drop (mirrored to Serial)

  char fname[16];
  if (rtcOk && currentDayStamp > 0) snprintf(fname, sizeof(fname), "/%08ld.CSV", currentDayStamp);
  else strlcpy(fname, "/NODATE.CSV", sizeof(fname));

  // Serialize SD access against the core-0 portal (SD admin). If the portal holds sdMux, don't block
  // the core-1 loop -- keep buffering and flush on a later tick (logBuf tolerates this like !sdOk).
  if (sdMux && xSemaphoreTake(sdMux, pdMS_TO_TICKS(100)) != pdTRUE) return;

  File f = SD.open(fname, FILE_APPEND);
  // A full card opens fine and then writes short, so the open alone proves nothing.
  size_t want = logBuf.length(), wrote = 0;
  if (f) { wrote = f.print(logBuf); f.close(); }
  if (wrote == want) { logBuf = ""; logLineCount = 0; }
  else {
    // Write failed: try ONE re-init. If the card is really gone, mark it lost so
    // deviceHealthTick (Part C) sees the present->absent transition.
    bool cardBack = SD.begin(SD_CS);
    if (!cardBack) { sdOk = false; Serial.println(F("WARN: microSD lost (write failed, re-init failed)")); }
    Serial.printf("WARN: SD write failed, %u of %u log bytes lost\n", (unsigned)(want - wrote), (unsigned)want);
    // Avoid unbounded growth, and leave a marker: otherwise the gap looks like a quiet period in the CSV.
    logBuf = cardBack ? tsString() + ",ESP1,LOG,WRITE_FAILED|lost_bytes=" + String(want - wrote) + "\n" : String("");
    logLineCount = cardBack ? 1 : 0;
  }
  if (sdMux) xSemaphoreGive(sdMux);
}
