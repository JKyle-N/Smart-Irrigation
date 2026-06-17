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
 *    - Monitors battery via INA226; classifies faults (3 tiers, sec.23).
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
#include <INA226.h>
#include <SoftwareSerial.h>      // EspSoftwareSerial (plerup) -- Nano link
#include <esp_task_wdt.h>

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
float fertGap = 20.0f;                        // mg/kg below target -> fertigate (NVS) [TBD]
const float FLUSH_PCT             = 20.0f;    // post-fertigation flush % (sec.14.2.0.2)
const unsigned long MIXING_DURATION_MS = 30000UL;  // homogenize time           [TBD]

/* ---- EC / pH safe window (pushed to ESP2, spec sec.14.2.4) --------------- */
const float EC_MIN = 0.0f,  EC_MAX = 3.0f;    // [TBD]
const float PH_MIN = 5.0f,  PH_MAX = 7.0f;    // [TBD]

/* ---- Tank levels (%) (spec sec.14.4) ------------------------------------- */
const float RES_LOW_PCT      = 15.0f;   // reservoir too low -> block ops  [TBD]
const float MIX_TARGET_PCT   = 70.0f;   // mixing tank fill target          [TBD]
const float MIX_MAX_PCT      = 95.0f;   // mixing tank overflow guard       [TBD]

/* ---- Battery (INA226) (spec sec.21.1) ------------------------------------ */
const float BATT_LOW_V       = 11.8f;   // disable fertigation below        [TBD]
const float BATT_CRIT_V      = 11.2f;   // stop all below                   [TBD]
const float INA226_SHUNT_OHMS   = 0.002f;   // shunt resistor value         [MEASURE]
const float INA226_MAX_CURRENT_A = 10.0f;   // expected max current         [MEASURE]

/* ---- Timing / supervisory constants (spec sec.10.14) --------------------- */
const unsigned long UART_ACK_TIMEOUT_MS   = 3000;    // ESP2 ACK wait
const unsigned long UART_DONE_TIMEOUT_MS  = 600000;  // ESP2 sequence completion (10 min)
const uint8_t       MAX_UART_RETRY        = 3;
const unsigned long RECOVERY_COOLDOWN_MS  = 30000;
const unsigned long STARTUP_SYNC_TIMEOUT_MS = 15000;
const unsigned long HEARTBEAT_TIMEOUT_MS  = 120000;  // subsystem freshness
const unsigned long INA_READ_INTERVAL_MS  = 5000;
const unsigned long LCD_REFRESH_MS        = 500;
const unsigned long LCD_BACKLIGHT_MS      = 10000;   // button backlight timeout
const unsigned long BTN_DEBOUNCE_MS       = 40;      // per-button debounce window
const unsigned long LOG_FLUSH_INTERVAL_MS = 5000;    // batched SD flush
const uint16_t      LOG_FLUSH_LINES       = 20;
const uint8_t       GARBAGE_LIMIT         = 5;       // consecutive (spec sec.18.9.5)
const uint8_t       MAX_NANO_HW_RESET_PER_DAY = 1;   // spec sec.18.9.5.1

/* ---- Daily schedule (minutes since midnight) ----------------------------- */
const uint16_t DAILY_REPORT_MIN     = 18*60;   // 18:00 daily summary (sec.12.1.3) [TBD]
const uint16_t DAILY_NANO_RESET_MIN = 3*60;    // 03:00 fresh-start RESET_REQ (sec.18.9.5.0.3)
const uint16_t NIGHT_START_MIN      = 18*60;   // night idle begins (Nano NIGHT interval)
const uint16_t NIGHT_END_MIN        = 6*60;    // night idle ends

/* ---- Nano semantic-range limits (garbage Tier 2, spec sec.18.9.5.0) ------ */
const float TEMP_MIN = -10.0f, TEMP_MAX = 70.0f;

/* =============================================================================
 *  DOSING -- DELEGATED STUB (spec sec.12.4 / CLAUDE.md)  -- EDIT TABLES ONLY
 * -----------------------------------------------------------------------------
 *  The mg/kg -> mL conversion is intentionally NOT implemented. Below are the
 *  editable tables the future calculation will consume; calcDoseML() returns a
 *  documented placeholder so the rest of the system compiles and runs.
 * ========================================================================== */
// Element contribution per mL of each nutrient bottle (mg of N/P/K per mL) [MEASURE]
struct NutrientConc { float nPerML, pPerML, kPerML; };
const NutrientConc NUTRIENT_CONC[3] = {   // A=CalNitrate, B=MAP, C=KNO3
  { 0.0f, 0.0f, 0.0f },   // Nutrient A  [MEASURE]
  { 0.0f, 0.0f, 0.0f },   // Nutrient B  [MEASURE]
  { 0.0f, 0.0f, 0.0f },   // Nutrient C  [MEASURE]
};
// Lab-measured existing soil N-P-K baseline per column (mg/kg) [MEASURE]
float LAB_SOIL_BASELINE[NUM_COLUMNS][3] = {
  { 0.0f, 0.0f, 0.0f },   // Column A {N,P,K}
  { 0.0f, 0.0f, 0.0f },   // Column B
  { 0.0f, 0.0f, 0.0f },   // Column C
};
// Built-in crop presets: name -> target N,P,K (mg/kg) + pH. Expandable.
struct CropPreset { const char* name; float N, P, K, pH; };
const CropPreset CROP_PRESETS[] = {
  { "LETTUCE", 150, 40, 200, 5.8f },
  { "CARROT",  120, 50, 250, 6.2f },
  { "TOMATO",  180, 45, 300, 6.0f },
};
const uint8_t NUM_PRESETS = sizeof(CROP_PRESETS) / sizeof(CROP_PRESETS[0]);

// STUB: target mg/kg -> mL per bottle. DEFINED-BUT-PENDING (separate task).
void calcDoseML(int colIdx, float &mlA, float &mlB, float &mlC) {
  (void)colIdx;
  mlA = 0.0f; mlB = 0.0f; mlC = 0.0f;   // placeholder; real open-loop calc pending
}

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
INA226            ina(INA226_ADDR);
Preferences       prefs;
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
  bool  envValid, tankValid, lightValid;
  unsigned long lastNanoMs;
};
SensorData sensor;

/* ---- Nano garbage tracking / recovery ------------------------------------ */
uint8_t garbageCount = 0;
uint8_t nanoHwResetsToday = 0;
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
uint8_t esp2PowerCycles = 0;
unsigned long lastRecoveryMs = 0;
unsigned long esp2OffMs = 0;     // timestamp ESP2 power was cut, for the power-cycle OFF hold
const unsigned long POWER_CYCLE_OFF_MS = 1500;   // hold ESP2 relay OFF this long for a real cycle

/* ---- GSM ----------------------------------------------------------------- */
bool reportPending = false;     // deferred STATUS report (sec.12.1.3.1)

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
const unsigned long GSM_CMGF_SETTLE_MS    = 300;
const unsigned long GSM_PROMPT_TIMEOUT_MS = 5000;
const unsigned long GSM_BODY_SETTLE_MS    = 1500;

/* ---- Power --------------------------------------------------------------- */
float battV = 0, battI = 0, battP = 0;
bool  batteryLow = false, batteryCritical = false;
unsigned long lastInaMs = 0;

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

/* ---- LCD UI -------------------------------------------------------------- */
enum LcdPage { PAGE_HOME, PAGE_SENSORS, PAGE_COLUMNS, PAGE_POWER, PAGE_FAULT, PAGE_COUNT };
uint8_t   lcdPage = PAGE_HOME;
bool      backlightOn = true;
unsigned long backlightMs = 0;
unsigned long lastLcdMs = 0;
String    lastFaultMsg = "none";

/* ---- Settings / Testing UI (MODE button, spec sec.18.10) ----------------- */
enum UiMode { UI_DATA, UI_MENU, UI_EDIT, UI_TEST };
UiMode    uiMode = UI_DATA;
// Top-level Settings menu rows
enum SetItem { SET_CLOCK, SET_SCHEDULE, SET_COLMODE, SET_PRESET, SET_THRESH, SET_TESTING, SET_EXIT, SET_COUNT };
const char *SET_NAMES[SET_COUNT] = { "Set Clock", "Schedule", "Column Mode", "Preset", "Thresholds", "Testing", "Exit" };
int  setSel    = 0;          // selected settings row
int  editItem  = -1;         // SetItem currently being edited
int  editField = 0;          // field index within the editor
int  editCol   = 0;          // column index for per-column editors
int  editTmp[6] = { 0 };     // working copy of the field values being edited
// Testing submenu: PCF8575 OUT_* bit -> short name (MUST match ESP2 OUT_* numbering)
const char *TEST_NAMES[16] = {
  "ResValve", "Col A Vlv", "Col B Vlv", "Col C Vlv", "Mix Valve", "Inverter",
  "Transfer", "Booster", "Mixer", "Nut A", "Nut B", "Nut C", "Nut D",
  "pH Up", "pH Down", "Nano RST"
};
int  testSel   = 0;          // selected component row
int  testOnBit = -1;         // which bit is currently ON (-1 = none)

/* ---- Heartbeat ----------------------------------------------------------- */
bool nanoSilent = false;        // one-shot latch so silence alerts/counts fire once per outage

/* ---- Forward declarations ------------------------------------------------ */
void wdtSetup();
void feedWDT();
long dayStamp(const DateTime &dt);
uint16_t minuteOfDay();
String tsString();
void setState(SystemState s);
void loadConfig();
void saveColumn(int c);
void saveSchedule(int c);
void saveColEnable(int c);
void saveThresholds();
void testHoldTick();

void pollNano();
bool classifyAndApply(const String &payload, const String &raw);
void escalateNanoRecovery();
void sendNanoCommand(const char *cmd);

void pollESP2();
void sendWorkOrder(int c, bool fertigate);
void handleEsp2Response(const String &payload);
void esp2PowerCycle();

void pollGSM();
void gsmTxTick();
void handleSms(const String &body);
void sendSMS(const String &msg);
void sendDailyReport();

void handleButtons();
void settingsButton(int i);
void enterEditor(int item);
void lcdRenderSettings();
void sendEsp2(const String &body);
void lcdTick();
void wakeBacklight();

void stateMachineTick();
void controlTick();
bool decideFertigate(int c);
void powerTick();
void scheduleTick();
void heartbeatTick();

void raiseFault(char tier, const char *code, const char *loc);
void enterEmergencyStop(bool cutPower);

void logEvent(const char *source, const char *type, const String &detail);
void logFlush(bool force);

/* =============================================================================
 *  SETUP
 * ========================================================================== */
void setup() {
  Serial.begin(DEBUG_BAUD);
  delay(50);

  // ---- Soft I2C bus reset: release stuck slaves (deadlock recovery) ----
  // If the previous run crashed mid-I2C transaction, a slave can hold SDA low and
  // deadlock the bus (LCD/INA/RTC dead until a full power cycle). Parking SDA/SCL
  // as pulled-up inputs briefly lets a stuck slave finish its byte and release.
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delay(100);

  Serial.println(F("\n=== ESP32 #1 Master Controller boot [DEBUG: watchdog OFF, no auto-reset; I2C soft-reset done] ==="));

  // Reboot breadcrumb: if the last run died in the WDT, name the stuck section.
  if (g_stageMagic == STAGE_MAGIC) {
    Serial.print(F("!! Previous run rebooted while in loop stage '"));
    Serial.print(g_lastStage); Serial.println(F("' (likely the blocking call)"));
  }
  g_stageMagic = STAGE_MAGIC;
  g_lastStage  = 'S';   // 'S' = still in setup()

  pinMode(ESP2_PWR_PIN, OUTPUT);
  digitalWrite(ESP2_PWR_PIN, LOW);          // ESP2 powered off until STARTUP_SYNC

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

  // ---- INA226 init with validation + retry ----
  inaOk = ina.begin();
  if (!inaOk) {
    Serial.println(F("WARN: INA226 init failed; retrying..."));
    delay(500);
    inaOk = ina.begin();
  }
  if (inaOk) {
    ina.setMaxCurrentShunt(INA226_MAX_CURRENT_A, INA226_SHUNT_OHMS);
    // Validate first reading: an implausibly low bus voltage (~0.01V) means the
    // I2C read is garbage, not a real flat battery. Disable INA so it can't raise
    // a fake BATTERY_CRITICAL that blocks the whole system (see powerTick).
    delay(100);
    float testV = ina.getBusVoltage();
    if (testV < 0.1f) {
      Serial.println(F("WARN: INA226 voltage implausible (~0.01V). I2C bus issue; disabling INA until next boot."));
      inaOk = false;
    }
  } else {
    Serial.println(F("CRIT: INA226 permanently failed"));
  }

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

  // ---- Config (NVS) ----
  prefs.begin("irrig", false);
  loadConfig();

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
  for (int i = 0; i < NUM_COLUMNS; i++) { sensor.soil[i] = -1; sensor.npkValid[i] = false; }
  sensor.lastNanoMs = millis();
  lastEsp2Ms = millis();

  logBuf.reserve(1024);
  logEvent("SYS", "STATE", "BOOT_STATE");

  wdtSetup();                               // arm task watchdog
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

  g_lastStage = 'M'; stateMachineTick();
  g_lastStage = 'C'; controlTick();
  g_lastStage = 'P'; powerTick();          // INA226 I2C read
  g_lastStage = 'D'; scheduleTick();
  g_lastStage = 'H'; heartbeatTick();

  g_lastStage = 'B'; handleButtons();
  g_lastStage = 'T'; testHoldTick();
  g_lastStage = 'L'; lcdTick();            // LCD I2C writes
  g_lastStage = 'F'; logFlush(false);      // microSD (SPI) write
  g_lastStage = '.';                       // idle: loop completed cleanly
}

/* =============================================================================
 *  WATCHDOG
 * ========================================================================== */
// DEBUG BUILD: the task watchdog is intentionally DISABLED so ESP32 #1 never
// auto-reboots while bench-testing (a stalled / garbage-flooded loop keeps
// running instead of panic-resetting in a boot loop, as seen on the real rig).
// The production build (ESP1/) arms an 8 s panic watchdog -- restore that for
// deployment.
void wdtSetup() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_deinit();                 // make sure no panic watchdog is armed
#endif
  // no esp_task_wdt_init / esp_task_wdt_add  ->  no watchdog  ->  no auto-reset
}
void feedWDT() { /* no-op in debug: watchdog disabled (see wdtSetup) */ }

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

// Returns true if packet is clean+plausible (and applies it); false = garbage.
bool classifyAndApply(const String &payload, const String &raw) {
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

  if (cmd == "ENV" && n == 3) {
    float t = tok[1].toFloat(), h = tok[2].toFloat();
    bool tInv = (tok[1] == "-1"), hInv = (tok[2] == "-1");
    if (!tInv && (t < TEMP_MIN || t > TEMP_MAX)) return false;   // Tier 2
    if (!hInv && (h < 0 || h > 100)) return false;
    sensor.temp = t; sensor.hum = h;
    sensor.envValid = !(tInv || hInv);
    sensor.lastNanoMs = millis();
    logEvent("NANO", "SENSOR", "ENV|" + tok[1] + "|" + tok[2]);
    return true;
  }
  if (cmd == "TANK" && n == 4) {
    float r = tok[1].toFloat(), m = tok[2].toFloat();
    bool rInv = (tok[1] == "-1"), mInv = (tok[2] == "-1");
    if (!rInv && (r < 0 || r > 100)) return false;
    if (!mInv && (m < 0 || m > 100)) return false;
    sensor.resLevel = r; sensor.mixLevel = m; sensor.flow = tok[3].toFloat();
    sensor.tankValid = !(rInv || mInv);
    sensor.lastNanoMs = millis();
    logEvent("NANO", "SENSOR", "TANK|" + tok[1] + "|" + tok[2] + "|" + tok[3]);
    return true;
  }
  if (cmd == "SOIL") {
    // Tag-based, variable-length: SOIL,<tag>,<val>,... (enabled columns only; disabled omitted)
    if (n < 3 || ((n - 1) & 1)) return false;          // need >=1 pair, even token count (Tier 1)
    int tmp[NUM_COLUMNS];
    for (int c = 0; c < NUM_COLUMNS; c++) tmp[c] = -1;  // columns absent from packet stay -1
    for (int i = 1; i + 1 < n; i += 2) {
      int c = -1;
      for (int j = 0; j < NUM_COLUMNS; j++) if (tok[i] == String(COL_TAG[j])) c = j;
      if (c < 0) return false;                          // unknown tag = Tier 1 garbage
      int v = tok[i + 1].toInt();
      if (v < 0 || v > 100) return false;               // Tier 2 (no -1 in SOIL anymore)
      tmp[c] = v;
    }
    for (int c = 0; c < NUM_COLUMNS; c++) sensor.soil[c] = tmp[c];
    sensor.lastNanoMs = millis();
    // Log all columns, pipe-delimited (CSV-safe, sec.25.2.1); disabled columns -> DISABLED
    // token (Nano omits them, but ESP1 knows from COLUMN_ENABLED) to distinguish off vs 0/fault.
    String d = "SOIL";
    for (int c = 0; c < NUM_COLUMNS; c++) {
      d += "|" + String(COL_TAG[c]) + "=";
      d += COLUMN_ENABLED[c] ? String(tmp[c]) : String("DISABLED");
    }
    logEvent("NANO", "SENSOR", d);
    return true;
  }
  if (cmd == "LIGHT" && n == 2) {
    float l = tok[1].toFloat();
    if (tok[1] != "-1" && l < 0) return false;
    sensor.lux = l; sensor.lightValid = (tok[1] != "-1");
    sensor.lastNanoMs = millis();
    logEvent("NANO", "SENSOR", "LIGHT|" + tok[1]);
    return true;
  }
  if (cmd == "NPK" && n == 9) {
    int c = -1;
    for (int i = 0; i < NUM_COLUMNS; i++) if (tok[1] == String(COL_TAG[i])) c = i;
    if (c < 0) return false;
    bool anyInvalid = false;
    for (int i = 0; i < 7; i++) {
      if (tok[i + 2] == "-1") anyInvalid = true;
      sensor.npk[c][i] = tok[i + 2].toFloat();
    }
    sensor.npkValid[c] = !anyInvalid;     // -1 sentinel is honest, NOT garbage
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
  // Layer 2: software RESET_REQ first (sec.18.9.5.0.2)
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
  // Layer 3: hardware reset via ESP2 (last resort, rate-limited)
  if (millis() - nanoResetReqMs > 10000) {   // software path didn't restore valid data
    if (nanoHwResetsToday < MAX_NANO_HW_RESET_PER_DAY && esp2Available) {
      esp2Serial.print(FRAME_START); esp2Serial.print(",RESET_NANO,"); esp2Serial.println(FRAME_END);
      nanoHwResetsToday++;
      logEvent("ESP1", "RESET", "NANO|HW|GARBAGE5");
      raiseFault('W', "NANO_RESET", "HW");
    }
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
        int s = raw.indexOf(FRAME_START), e = raw.indexOf(FRAME_END);
        String payload = (s >= 0 && e > s) ? raw.substring(s + 7, e) : raw;
        payload.trim();
        if (payload.startsWith(",")) payload = payload.substring(1);
        if (payload.endsWith(","))   payload = payload.substring(0, payload.length() - 1);
        handleEsp2Response(payload);
      }
    } else if (line.length() < 140) line += c; else line = "";
  }

  // ACK / DONE timeout supervision
  if (wo.active && wo.stage == WO_SENT && millis() - wo.sentMs > UART_ACK_TIMEOUT_MS) {
    if (wo.retries < MAX_UART_RETRY) {
      wo.retries++;
      esp2Serial.println(wo.cmd);
      wo.sentMs = millis();
      logEvent("ESP1", "RESP", "RETRY|" + wo.cmd + "|" + String(wo.retries));
    } else {
      logEvent("ESP1", "RESP", "TIMEOUT|" + wo.cmd);
      setState(RECOVERY_STATE);
      esp2PowerCycle();
      wo.active = false; wo.stage = WO_IDLE;
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

  if (resp == "STATUS" || resp == "READY") {
    // DEBUG: if ESP2 (re)booted while the operator is in Testing -- e.g. it was
    // just powered by the GPIO4 relay on Testing entry and missed the first
    // TEST,ENTER -- re-arm TEST mode now that it is up.
    if (resp == "READY" && uiMode == UI_TEST) sendEsp2("TEST,ENTER");
    return;                                                   // heartbeat / startup
  }
  logEvent("ESP2", "RESP", payload);

  if (resp == "ACK") {
    if (wo.active) { wo.stage = WO_ACKED; wo.sentMs = millis(); }
  } else if (resp == "DONE") {
    if (wo.active) {
      int c = wo.colIdx;
      if (c >= 0 && c < NUM_COLUMNS) {
        col[c].lastServicedStamp = currentDayStamp;
        logEvent("ESP1", "ACT", String(wo.fertigate ? "FERTIGATION" : "IRRIGATION")
                                  + "|STOP|COL_" + COL_TAG[c]);
      }
      wo.active = false; wo.stage = WO_IDLE; wo.colIdx = -1;
      if (sysState == ACTIVE_STATE) setState(IDLE_STATE);
    }
  } else if (resp == "BUSY") {
    // ESP2 busy; leave work order pending, it will retry/await
  } else if (resp == "DEGRADED") {
    // ESP2 disabled one channel (nutrient/mixer) but is STILL running the sequence:
    // Major, NOT Critical -- do not abort or cut power, just log+alert (sec.23.2.2.1/.4).
    raiseFault('M', "ESP2_DEGRADED", arg.c_str());
  } else if (resp == "ERROR") {
    raiseFault('C', "ESP2_ERROR", arg.c_str());
    wo.active = false; wo.stage = WO_IDLE;
  } else if (resp == "FLOW_FAIL") {
    raiseFault('C', "FLOW_FAIL", arg.c_str());
    wo.active = false; wo.stage = WO_IDLE;
  } else if (resp == "PWR_FAIL") {
    raiseFault('C', "PWR_FAIL", arg.c_str());
    wo.active = false; wo.stage = WO_IDLE;
  } else if (resp == "EC_FAIL") {
    raiseFault('M', "EC_FAIL", arg.c_str());
  } else if (resp == "PH_FAIL") {
    raiseFault('M', "PH_FAIL", arg.c_str());
  } else if (resp == "SAFE_STOP") {
    raiseFault('C', "SAFE_STOP", arg.c_str());
    wo.active = false; wo.stage = WO_IDLE;
  }
}

void sendWorkOrder(int c, bool fertigate) {
  float mlA = 0, mlB = 0, mlC = 0;
  if (fertigate) calcDoseML(c, mlA, mlB, mlC);   // stub -> 0 (sec.12.4)

  // Complete work order: column, water/flush split, dose mL, EC/pH window (sec.9.8.1.1)
  String name = String("SEQ_") + (fertigate ? "FERTIGATION_" : "IRRIGATION_") + COL_TAG[c];
  String cmd = String(FRAME_START) + "," + name +
               ",FLUSH," + String((int)FLUSH_PCT);
  if (fertigate) {
    cmd += ",DOSE," + String(mlA, 1) + "," + String(mlB, 1) + "," + String(mlC, 1);
    cmd += ",EC," + String(EC_MIN, 1) + "," + String(EC_MAX, 1);
    cmd += ",PH," + String(PH_MIN, 1) + "," + String(PH_MAX, 1);
    cmd += ",MIX," + String(MIXING_DURATION_MS);
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

void esp2PowerCycle() {
  if (millis() - lastRecoveryMs < RECOVERY_COOLDOWN_MS) return;
  lastRecoveryMs = millis();
  esp2PowerCycles++;
  logEvent("ESP1", "RESET", "ESP2|HW|POWERCYCLE");
  digitalWrite(ESP2_PWR_PIN, LOW);
  esp2OffMs = millis();                  // STARTUP_SYNC holds OFF for POWER_CYCLE_OFF_MS, then powers on
  esp2Available = false;
  setState(STARTUP_SYNC);
}

/* =============================================================================
 *  GSM  (inbound SMS parse + outbound, proven SIM800L pattern)
 * ========================================================================== */
void pollGSM() {
  if (gtx != GTX_IDLE) return;          // an outbound send owns the SIM UART right now
  static String line;
  static bool expectBody = false;
  while (simSerial.available()) {
    char c = (char)simSerial.read();
    if (c == '\n') {
      line.trim();
      if (line.startsWith("+CMT:")) {
        // Extract the sender number (first quoted field) so replies go back to it.
        int q1 = line.indexOf('"');
        int q2 = (q1 >= 0) ? line.indexOf('"', q1 + 1) : -1;
        replyTarget = (q1 >= 0 && q2 > q1) ? line.substring(q1 + 1, q2) : "";
        expectBody = true;                  // next line is the message body
      } else if (expectBody && line.length() > 0) {
        logEvent("GSM", "GSM", "RX|" + line);
        handleSms(line);                    // replies enqueue with replyTarget (the sender)
        replyTarget = "";                   // back to owner PHONE_NUMBER for autonomous msgs
        expectBody = false;
        wakeBacklight();
      }
      line = "";
    } else if (c != '\r') {
      line += c;
      if (line.length() > 180) line = "";
    }
  }
}

// Parse compact comma commands (spec sec.12.2). Keyword match is case-insensitive.
void handleSms(const String &body) {
  String b = body; b.trim();
  String U = b; U.toUpperCase();

  // STOP,ALL
  if (U.startsWith("STOP")) {
    enterEmergencyStop(false);
    sendSMS("ACK,STOP,ALL");
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
  sendSMS("ERR,CMD");
}

// Enqueue only -- non-blocking. The AT exchange is driven by gsmTxTick() so alert
// and fault paths never stall the loop.
void sendSMS(const String &msg) {
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
      while (simSerial.available()) simSerial.read();   // clear stale bytes
      simSerial.println("AT+CMGF=1");                    // text mode
      gtxMs = millis();
      gtx = GTX_CMGF_WAIT;
      break;

    case GTX_CMGF_WAIT:
      if (millis() - gtxMs < GSM_CMGF_SETTLE_MS) return;
      simSerial.print("AT+CMGS=\""); simSerial.print(gtxTo.length() ? gtxTo : PHONE_NUMBER); simSerial.println("\"");
      gtxMs = millis();
      gtx = GTX_PROMPT_WAIT;
      break;

    case GTX_PROMPT_WAIT:
      while (simSerial.available()) {
        if ((char)simSerial.read() == '>') {            // module ready for body
          simSerial.print(gtxMsg);
          simSerial.write(26);                          // CTRL+Z -> send
          logEvent("GSM", "GSM", "TX|" + gtxMsg);
          gtxMs = millis();
          gtx = GTX_BODY_SETTLE;
          return;
        }
      }
      if (millis() - gtxMs > GSM_PROMPT_TIMEOUT_MS) {    // no '>' -> abandon this message
        logEvent("GSM", "GSM", "TX|FAIL_NO_PROMPT");
        gtx = GTX_IDLE;
      }
      break;

    case GTX_BODY_SETTLE:                                // let the module finish before next send
      if (millis() - gtxMs >= GSM_BODY_SETTLE_MS) gtx = GTX_IDLE;
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
  sendSMS(r);
  reportPending = false;
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
        digitalWrite(ESP2_PWR_PIN, HIGH);            // power ESP2 (sec.10.7.2)
        esp2OffMs = 0;
        esp2Serial.print(FRAME_START); esp2Serial.print(",STATUS_REQ,"); esp2Serial.println(FRAME_END);
      }
      bool nanoFresh = (millis() - sensor.lastNanoMs < HEARTBEAT_TIMEOUT_MS);
      if (esp2Available && nanoFresh) { syncStart = 0; setState(IDLE_STATE); }
      else if (millis() - syncStart > STARTUP_SYNC_TIMEOUT_MS) {
        syncStart = 0;
        if (!esp2Available) { logEvent("ESP1", "FAULT", "MAJ|ESP2_NO_READY|STARTUP"); raiseFault('M', "ESP2_NO_READY", "STARTUP"); }
        setState(IDLE_STATE);                         // proceed degraded; controlTick gates on esp2Available
      }
      break;
    }
    case RECOVERY_STATE:
      // Drive ESP2 recovery: power-cycle (cooldown-gated) then re-sync. esp2PowerCycle()
      // transitions to STARTUP_SYNC when it fires, so this is no longer a dead end.
      esp2PowerCycle();
      break;
    default: break;
  }
}

/* =============================================================================
 *  CONTROL LOGIC  --  windowed schedule + soil threshold + NPK gap
 * ========================================================================== */
void controlTick() {
  if (uiMode != UI_DATA) return;          // operator in Settings/Testing -> pause automation
  if (sysState != IDLE_STATE) return;     // only dispatch new work from IDLE
  if (wo.active) return;                   // ESP2 busy -> sequential (sec.14.2.0.1)
  if (!esp2Available) return;             // no executor available
  if (batteryCritical) return;            // sec.21.1: stop all
  if (!rtcOk) return;

  uint16_t mod = minuteOfDay();

  for (int c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c]) continue;
    if (col[c].lastServicedStamp == currentDayStamp) continue;     // once/day per window
    // schedule axis: AUTO uses the default window, MANUAL uses the operator-set one (sec.18.10.7.2)
    uint16_t ws = (colSchedMode[c] == SCHED_MANUAL) ? COL_WIN_START[c] : DEF_WIN_START[c];
    uint16_t we = (colSchedMode[c] == SCHED_MANUAL) ? COL_WIN_END[c]   : DEF_WIN_END[c];
    if (mod < ws || mod > we) continue;                            // outside service window
    if (sensor.soil[c] < 0) continue;                              // invalid soil (faulted elsewhere)
    if (sensor.soil[c] >= soilStartPct) continue;                 // not dry enough
    if (sensor.tankValid && sensor.resLevel < RES_LOW_PCT) {        // reservoir guard (sec.14.4.1)
      raiseFault('M', "RES_LOW", "RESERVOIR");
      continue;
    }
    bool fert = decideFertigate(c) && !batteryLow;                 // low batt -> irrigation only
    setState(ACTIVE_STATE);
    sendNanoCommand("ACTIVE");                                     // fast Nano interval
    sendWorkOrder(c, fert);
    return;                                                        // one column at a time
  }
}

bool decideFertigate(int c) {
  if (col[c].mode == MODE_IRRIGATION_ONLY) return false;
  if (!sensor.npkValid[c]) { raiseFault('M', "NPK_FAULT", c == 0 ? "COL_A" : c == 1 ? "COL_B" : "COL_C"); return false; }
  float n = sensor.npk[c][4], p = sensor.npk[c][5], k = sensor.npk[c][6];
  if ((col[c].targetN - n) >= fertGap) return true;
  if ((col[c].targetP - p) >= fertGap) return true;
  if ((col[c].targetK - k) >= fertGap) return true;
  return false;
}

/* =============================================================================
 *  POWER  (INA226 battery monitoring, spec sec.21.1)
 * ========================================================================== */
void powerTick() {
  if (!inaOk) {
    // INA226 is down (init failed or I2C garbage detected at boot). Do NOT read;
    // leave battV/battI/battP stale (0) so no fake BATTERY_CRITICAL is raised.
    return;
  }
  if (millis() - lastInaMs < INA_READ_INTERVAL_MS) return;
  lastInaMs = millis();
  battV = ina.getBusVoltage();
  battI = ina.getCurrent();
  battP = ina.getPower();

  bool wasLow = batteryLow, wasCrit = batteryCritical;
  // Only classify low/critical if INA is working AND the voltage is genuinely low.
  // If INA failed, battV stays 0 and we skip false faults.
  batteryCritical = inaOk && (battV > 0 && battV < BATT_CRIT_V);
  batteryLow      = inaOk && (battV > 0 && battV < BATT_LOW_V);

  if (batteryCritical && !wasCrit) {
    raiseFault('C', "BATTERY_CRITICAL", "BATT");   // raiseFault('C') already enters EMERGENCY_STOP
  } else if (batteryLow && !wasLow) {
    raiseFault('M', "BATTERY_LOW", "BATT");
  }
  logEvent("ESP1", "PWR", "INA226|V=" + String(battV, 2) + "|I=" + String(battI, 2) + "|P=" + String(battP, 1));
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
    nanoResetsToday = 0; nanoHwResetsToday = 0; esp2PowerCycles = 0;
    reportSentToday = false; nanoDailyResetDone = false;
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
}

/* =============================================================================
 *  HEARTBEAT  (freshness; Nano silence does NOT trigger reset, sec.18.9.5.0.1)
 * ========================================================================== */
void heartbeatTick() {
  // Nano silence: alert/count ONCE per outage (Nano's own WDT self-recovers, sec.18.9.5.0.1).
  // Not a reset trigger; the flag clears when fresh data resumes (pollNano).
  if (!nanoSilent && millis() - sensor.lastNanoMs > HEARTBEAT_TIMEOUT_MS) {
    nanoSilent = true;
    faultsToday[2]++;                                          // minor (now actually tallied)
    logEvent("ESP1", "FAULT", "MIN|NANO_SILENCE|NANO");
    sendSMS("ALERT,MIN,NANO_SILENCE,NANO");                    // one-shot (queued, non-blocking)
  }
  // ESP2 silence: major fault + recovery. Inherently one-shot (esp2Available latches false
  // until ESP2 speaks again). RECOVERY_STATE drives the power-cycle (no longer a dead end).
  if (esp2Available && millis() - lastEsp2Ms > HEARTBEAT_TIMEOUT_MS) {
    esp2Available = false;
    raiseFault('M', "ESP2_SILENCE", "ESP2");                  // counts + queued SMS (sec.12.1.1)
    if (sysState != EMERGENCY_STOP) setState(RECOVERY_STATE);
  }
}

/* =============================================================================
 *  FAULTS  (3-tier classification + response, spec sec.23)
 * ========================================================================== */
void raiseFault(char tier, const char *code, const char *loc) {
  const char *t = (tier == 'C') ? "CRIT" : (tier == 'M') ? "MAJ" : (tier == 'W') ? "WARN" : "MIN";
  if (tier == 'C') faultsToday[0]++; else if (tier == 'M') faultsToday[1]++; else if (tier == 'm') faultsToday[2]++;

  String detail = String(t) + "|" + code + "|" + loc;
  logEvent("ESP1", "FAULT", detail);
  lastFaultMsg = String(code) + " " + loc;
  // Faults preempt the Settings/Testing UI; never leave a manually-held relay ON.
  if (uiMode == UI_TEST) { sendEsp2("TEST,EXIT"); if (sysState == TEST_MODE) setState(IDLE_STATE); }
  uiMode = UI_DATA;
  lcdPage = PAGE_FAULT; wakeBacklight();

  // GSM alert for all tiers (sec.12.1.1)
  sendSMS(String("ALERT,") + t + "," + code + "," + loc);

  if (tier == 'C') enterEmergencyStop(true);     // critical -> shutdown (sec.14.7.1)
}

void enterEmergencyStop(bool cutPower) {
  // stop all actuators (sec.22.3) then optionally cut ESP2 power (sec.23.1.1)
  esp2Serial.print(FRAME_START); esp2Serial.print(",STOP_ALL,"); esp2Serial.println(FRAME_END);
  logEvent("ESP1", "CMD", "ESP2|STOP_ALL");
  wo.active = false; wo.stage = WO_IDLE;
  if (cutPower) { digitalWrite(ESP2_PWR_PIN, LOW); esp2Available = false; }
  setState(EMERGENCY_STOP);
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
  for (int i = 0; i < 5; i++) {
    bool cur = digitalRead(pins[i]);
    // Debounced edge only: a noisy/floating contact (esp. on strapping-pin MODE/GPIO2)
    // must not spam wakeBacklight()/I2C every loop iteration.
    if (cur != last[i] && millis() - lastChange[i] > BTN_DEBOUNCE_MS) {
      lastChange[i] = millis();
      last[i] = cur;
      if (cur == LOW) {                            // falling edge = press
        wakeBacklight();

        // MODE (i==4): toggle into/out of the Settings menu (spec sec.18.10)
        if (i == 4) {
          if (uiMode == UI_DATA) { uiMode = UI_MENU; setSel = 0; }
          else {
            if (uiMode == UI_TEST) {
              sendEsp2("TEST,EXIT");
              digitalWrite(ESP2_PWR_PIN, LOW);                 // DEBUG: cut the ESP2 power relay back off
              if (sysState == TEST_MODE) setState(IDLE_STATE);
            }
            uiMode = UI_DATA;
          }
          continue;
        }

        // In any settings UI, the other 4 buttons drive the menu, not the data pages.
        if (uiMode != UI_DATA) { settingsButton(i); continue; }

        // ---- normal data-page navigation (UI_DATA) ----
        if (i == 0) lcdPage = (lcdPage + PAGE_COUNT - 1) % PAGE_COUNT;   // UP
        else if (i == 1) lcdPage = (lcdPage + 1) % PAGE_COUNT;           // DOWN
        else if (i == 3 && sysState == EMERGENCY_STOP) {                 // BACK clears E-STOP
          digitalWrite(ESP2_PWR_PIN, HIGH);
          setState(STARTUP_SYNC);
        }
      }
    }
  }
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

// Load the per-column schedule fields into the working copy for editCol.
static void seedSchedule() {
  editTmp[0] = colSchedMode[editCol];                          // AUTO / MANUAL
  editTmp[1] = COL_WIN_START[editCol] / 60; editTmp[2] = COL_WIN_START[editCol] % 60;
  editTmp[3] = COL_WIN_END[editCol]   / 60; editTmp[4] = COL_WIN_END[editCol]   % 60;
}

// Seed the working copy when an editor is opened.
void enterEditor(int item) {
  editItem = item; editField = 0; editCol = 0;
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
  if (uiMode == UI_MENU) {
    if (i == 0) setSel = (setSel + SET_COUNT - 1) % SET_COUNT;
    else if (i == 1) setSel = (setSel + 1) % SET_COUNT;
    else if (i == 3) uiMode = UI_DATA;                         // BACK = exit
    else if (i == 2) {                                         // ENTER = open item
      if (setSel == SET_EXIT) { uiMode = UI_DATA; }
      else if (setSel == SET_TESTING) {
        // DEBUG BUILD (production gate is `!esp2Available || sysState != IDLE_STATE`):
        //  - Power ESP32 #2 via its GPIO4 relay so it boots for the dead-man test.
        //  - Do NOT require ESP2 to be present/idle; allow entering Testing from any
        //    state except a running sequence (ACTIVE_STATE).
        digitalWrite(ESP2_PWR_PIN, HIGH);                      // turn ON the ESP2 power relay
        if (sysState == ACTIVE_STATE) return;                  // don't interrupt a live run
        sendEsp2("TEST,ENTER");                                // ESP2 forces SAFE + enters TEST_MODE (when it boots)
        setState(TEST_MODE);
        uiMode = UI_TEST; testSel = 0; testOnBit = -1;
      } else enterEditor(setSel);
    }
    return;
  }

  if (uiMode == UI_TEST) {
    // Dead-man: ENTER is NOT an edge action here (held = ON, handled by testHoldTick).
    if (i == 0) testSel = (testSel + 16 - 1) % 16;             // UP   select component
    else if (i == 1) testSel = (testSel + 1) % 16;            // DOWN select component
    else if (i == 3) {                                         // BACK exit Testing
      sendEsp2("TEST,EXIT"); testOnBit = -1;
      digitalWrite(ESP2_PWR_PIN, LOW);                         // DEBUG: cut the ESP2 power relay back off
      if (sysState == TEST_MODE) setState(IDLE_STATE);
      uiMode = UI_MENU;
    }
    return;
  }

  if (uiMode == UI_EDIT) {
    int delta = (i == 0) ? +1 : (i == 1) ? -1 : 0;
    if (i == 3) { uiMode = UI_MENU; editItem = -1; return; }    // BACK = cancel, no save

    switch (editItem) {
      case SET_CLOCK:
        if (delta) {
          if (editField == 0) editTmp[0] = clampi(editTmp[0] + delta, 2020, 2099);
          else if (editField == 1) editTmp[1] = clampi(editTmp[1] + delta, 1, 12);
          else if (editField == 2) editTmp[2] = clampi(editTmp[2] + delta, 1, 31);
          else if (editField == 3) editTmp[3] = clampi(editTmp[3] + delta, 0, 23);
          else if (editField == 4) editTmp[4] = clampi(editTmp[4] + delta, 0, 59);
        } else if (i == 2) {                                    // ENTER advances / commits
          if (++editField > 4) {
            if (rtcOk) rtc.adjust(DateTime(editTmp[0], editTmp[1], editTmp[2], editTmp[3], editTmp[4], 0));
            logEvent("ESP1", "STATE", "CLOCK_SET");
            uiMode = UI_MENU; editItem = -1;
          }
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
          if (++editField > lastField) {
            colSchedMode[editCol] = editTmp[0];
            if (editTmp[0] == SCHED_MANUAL) {
              COL_WIN_START[editCol] = editTmp[1] * 60 + editTmp[2];
              COL_WIN_END[editCol]   = editTmp[3] * 60 + editTmp[4];
            }
            saveSchedule(editCol);
            logEvent("ESP1", "STATE", String("SCHED_SET|COL_") + COL_TAG[editCol]);
            uiMode = UI_MENU; editItem = -1;
          }
        }
        break;

      case SET_COLMODE:                                         // AUTO / IRRIGATION_ONLY / OFF
        if (delta) {
          if (editField == 0) { editCol = clampi(editCol + delta, 0, NUM_COLUMNS - 1); editTmp[0] = colMode3(editCol); }
          else if (editField == 1) editTmp[0] = (editTmp[0] + (delta > 0 ? 1 : 2)) % 3;
        } else if (i == 2) {
          if (++editField > 1) {
            if (editTmp[0] == 2) { COLUMN_ENABLED[editCol] = false; }   // OFF
            else { COLUMN_ENABLED[editCol] = true; col[editCol].mode = (editTmp[0] == 0) ? MODE_AUTO : MODE_IRRIGATION_ONLY; saveColumn(editCol); }
            saveColEnable(editCol);
            logEvent("ESP1", "STATE", String("MODE_SET|COL_") + COL_TAG[editCol]);
            uiMode = UI_MENU; editItem = -1;
          }
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
          if (++editField > lastField) {
            if (editTmp[0] == 0) {                                            // NAMED preset
              const CropPreset &p = CROP_PRESETS[editTmp[1]];
              col[editCol].targetN = p.N; col[editCol].targetP = p.P; col[editCol].targetK = p.K; col[editCol].targetPH = p.pH;
            } else {                                                          // MANUAL
              col[editCol].targetN = editTmp[2]; col[editCol].targetP = editTmp[3];
              col[editCol].targetK = editTmp[4]; col[editCol].targetPH = editTmp[5] / 10.0f;
            }
            saveColumn(editCol);
            logEvent("ESP1", "STATE", String("PRESET_SET|COL_") + COL_TAG[editCol]);
            uiMode = UI_MENU; editItem = -1;
          }
        }
        break;

      case SET_THRESH:
        if (delta) {
          if (editField == 0) editTmp[0] = clampi(editTmp[0] + delta, 0, 100);
          else if (editField == 1) editTmp[1] = clampi(editTmp[1] + delta, 0, 100);
          else if (editField == 2) editTmp[2] = clampi(editTmp[2] + delta, 0, 500);
        } else if (i == 2) {
          if (++editField > 2) {
            if (editTmp[1] <= editTmp[0]) editTmp[1] = clampi(editTmp[0] + 1, 0, 100);  // stop > start
            soilStartPct = editTmp[0]; soilStopPct = editTmp[1]; fertGap = editTmp[2]; saveThresholds();
            logEvent("ESP1", "STATE", "THRESH_SET");
            uiMode = UI_MENU; editItem = -1;
          }
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
  static bool wasHeld = false;
  static unsigned long lastSendMs = 0;
  bool held = (digitalRead(BTN_ENTER) == LOW);

  if (held) {
    if (!wasHeld) {                                            // press edge
      wasHeld = true; lastSendMs = 0; testOnBit = testSel;
      logEvent("ESP1", "ACT", String("TEST|START|") + TEST_NAMES[testSel]);
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

void lcdTick() {
  if (millis() - lastLcdMs < LCD_REFRESH_MS) return;
  lastLcdMs = millis();

  if (backlightOn && millis() - backlightMs > LCD_BACKLIGHT_MS
      && uiMode == UI_DATA
      && sysState != ACTIVE_STATE && sysState != EMERGENCY_STOP && lcdPage != PAGE_FAULT) {
    backlightOn = false; lcd.noBacklight();
  }

  // Settings / Testing UI takes over the screen when active (spec sec.18.10).
  static uint8_t lastUi = 255;
  if (lastUi != uiMode) { lcd.clear(); lastUi = uiMode; }
  if (uiMode != UI_DATA) { lcdRenderSettings(); return; }

  static uint8_t lastPage = 255;
  if (lastPage != lcdPage) { lcd.clear(); lastPage = lcdPage; }

  char l[21];
  switch (lcdPage) {
    case PAGE_HOME:
      lcd.setCursor(0, 0); lcd.print(tsString().substring(0, 19));
      lcd.setCursor(0, 1); snprintf(l, 21, "State:%-13s", stateName(sysState)); lcd.print(l);
      lcd.setCursor(0, 2); snprintf(l, 21, "Bat:%.1fV %s", battV, batteryCritical ? "CRIT" : batteryLow ? "LOW" : "OK"); lcd.print(l);
      lcd.setCursor(0, 3); snprintf(l, 21, "Res:%d%% Mix:%d%%", (int)sensor.resLevel, (int)sensor.mixLevel); lcd.print(l);
      break;
    case PAGE_SENSORS:
      lcd.setCursor(0, 0); snprintf(l, 21, "T:%.1fC H:%.0f%%", sensor.temp, sensor.hum); lcd.print(l);
      lcd.setCursor(0, 1); snprintf(l, 21, "Soil A%d B%d C%d", sensor.soil[0], sensor.soil[1], sensor.soil[2]); lcd.print(l);
      lcd.setCursor(0, 2); snprintf(l, 21, "Light:%.0f", sensor.lux); lcd.print(l);
      lcd.setCursor(0, 3); snprintf(l, 21, "Flow:%.1f L/min", sensor.flow); lcd.print(l);
      break;
    case PAGE_COLUMNS:
      for (int c = 0; c < 3; c++) {
        lcd.setCursor(0, c);
        snprintf(l, 21, "%c:%s %s", COL_TAG[c],
                 COLUMN_ENABLED[c] ? (col[c].mode == MODE_AUTO ? "AUTO" : "IRR ") : "OFF ",
                 strlen(col[c].name) ? col[c].name : "-");
        lcd.print(l);
      }
      lcd.setCursor(0, 3); snprintf(l, 21, "ESP2:%s", esp2Available ? "OK" : "--"); lcd.print(l);
      break;
    case PAGE_POWER:
      lcd.setCursor(0, 0); lcd.print("Power (INA226)");
      lcd.setCursor(0, 1); snprintf(l, 21, "V:%.2f", battV); lcd.print(l);
      lcd.setCursor(0, 2); snprintf(l, 21, "I:%.2fA P:%.1fW", battI, battP); lcd.print(l);
      lcd.setCursor(0, 3); snprintf(l, 21, "SD:%s RTC:%s", sdOk ? "OK" : "--", rtcOk ? "OK" : "--"); lcd.print(l);
      break;
    default: // PAGE_FAULT
      lcd.setCursor(0, 0); lcd.print("Last Fault:");
      lcd.setCursor(0, 1); lcd.print(lastFaultMsg.substring(0, 20));
      lcd.setCursor(0, 2); snprintf(l, 21, "C%d M%d m%d today", faultsToday[0], faultsToday[1], faultsToday[2]); lcd.print(l);
      lcd.setCursor(0, 3); lcd.print(sysState == EMERGENCY_STOP ? "BACK=clear E-STOP" : "");
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

void lcdRenderSettings() {
  char l[21];

  if (uiMode == UI_MENU) {
    lcd.setCursor(0, 0); lcd.print("=== SETTINGS ===    ");
    drawList(setSel, SET_COUNT, SET_NAMES);
    return;
  }

  if (uiMode == UI_TEST) {
    lcd.setCursor(0, 0); lcd.print(esp2Available ? "TESTING (dead-man)  " : "TESTING: ESP2 DOWN  ");
    int top = testSel - 1; if (top < 0) top = 0; if (top > 16 - 2) top = 16 - 2;
    for (int r = 0; r < 2; r++) {
      int idx = top + r;
      lcd.setCursor(0, r + 1);
      snprintf(l, 21, "%c%-9s %s", idx == testSel ? '>' : ' ', TEST_NAMES[idx],
               idx == testOnBit ? "[ON] " : "[ - ]");
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
  String row = tsString() + "," + source + "," + type + "," + detail + "\n";
  logBuf += row;
  logLineCount++;
  Serial.print(row);                          // mirror to USB debug
  bool critical = (strcmp(type, "FAULT") == 0 && detail.startsWith("CRIT"));
  if (logLineCount >= LOG_FLUSH_LINES || critical) logFlush(critical);
}

void logFlush(bool force) {
  if (logBuf.length() == 0) return;
  if (!force && millis() - lastLogFlushMs < LOG_FLUSH_INTERVAL_MS && logLineCount < LOG_FLUSH_LINES) return;
  lastLogFlushMs = millis();
  if (!sdOk) { logBuf = ""; logLineCount = 0; return; }   // degraded: drop (mirrored to Serial)

  char fname[16];
  if (rtcOk && currentDayStamp > 0) snprintf(fname, sizeof(fname), "/%08ld.CSV", currentDayStamp);
  else { strcpy(fname, "/NODATE.CSV"); }

  File f = SD.open(fname, FILE_APPEND);
  if (f) { f.print(logBuf); f.close(); logBuf = ""; logLineCount = 0; }
  else   { logBuf = ""; logLineCount = 0; }   // avoid unbounded growth on write failure
}
