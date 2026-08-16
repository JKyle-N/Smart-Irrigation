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
 *         Optional operator-FORCE fields, valid on either form (dashboard force-run):
 *                 COLS,<letters>  deliver into every listed ENABLED column AT ONCE, e.g. COLS,AB.
 *                                 Default = the single column named in the command. With more than
 *                                 one column the tank's single outlet + single flow meter can only
 *                                 measure a COMBINED total; the per-column split is not knowable.
 *                 WATER,<L>       TOTAL batch litres for this run, overriding WATER_BUDGET_L.
 *                                 Ignored unless 0 < L <= MIXING_TANK_MAX_L.
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
 *    OUT: ACK,<cmd> -> DONE,<cmd>[,WATER,<L>[,EST]];  DOSE,NUT_x,<target>,<measured>,COL_y
 *         (,EST = this water total includes a flow-blind timed stage: estimated, not metered)
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
 *  NON-BLOCKING: no delay() in normal operation (only a 20 ms UART flush right before a
 *  self-reboot); every stage is a millis()-based step in a FSM. Task WDT petted each loop.
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
#include <esp_system.h>        // esp_reset_reason() -- clean-reboot-once on cold power-on
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
#define OUT_NUT_D     12    // P14 nutrient D pump -- not dosed (only 3 nutrients); driven solely by Prime NUTD
#define OUT_PH_UP     13    // P15 pH up pump
#define OUT_PH_DN     14    // P16 pH down pump
#define OUT_MASTER_CUTOFF 15 // P17 master actuator-power cutoff (sec.19.4.8; was Nano RESET).
                             // Gates power to the whole P0-P16 bank. Fail-safe: de-energized = all OFF.
const uint8_t COL_VALVE_BIT[3] = { OUT_COL_A, OUT_COL_B, OUT_COL_C };
const uint8_t NUT_PUMP_BIT[3]  = { OUT_NUT_A, OUT_NUT_B, OUT_NUT_C };

/* ---- Flow sensors (interrupt, stage-based; spec sec.19.4.5) --------------- *
 * Pins CORRECTED to the actual wiring (identified with Calibration/ESP2/FlowPinFinder).  */
#define FLOW_RES_MIX 26
#define FLOW_MIX_IRR  5
#define FLOW_NUT_A   23
#define FLOW_NUT_B   19
#define FLOW_NUT_C   18
#define FLOW_NUT_D   27      // wired but not metered in normal operation
#define FLOW_PH_UP    4      // wired but not metered in normal operation
#define FLOW_PH_DN   25      // wired but not metered in normal operation
const uint8_t NUT_FLOW_PIN[3] = { FLOW_NUT_A, FLOW_NUT_B, FLOW_NUT_C };

/* ---- Analog sensors (ADC1; spec sec.19.4.6) ------------------------------ */
#define PIN_PH        32
#define PIN_EC        33
#define PIN_MIXER_I   36       // ACS712 mixer current

/* ---- Flow K-factors (pulses per LITER) [MEASURE] ------------------------- *
 * RUNTIME (not const): set on the fly by ESP1 Calibration Mode via SET_CAL, refreshed at
 * STARTUP_SYNC and carried in each work order (spec §A.5.1). Defaults are the bench values.  */
float K_RES_MIX = 450.0f;
float K_MIX_IRR = 450.0f;
float K_NUT[3]   = { 450.0f, 450.0f, 450.0f };   // small dosing sensors differ [MEASURE]
/* Flow K-factors are DIVISORS in litersSoFar(), and every metered stage decides when to stop its
 * pump from that result -- so a bad K is not a measurement error, it is an actuation fault:
 *   k == 0        -> pulses/0 = inf -> "target reached" on the first pulse. The stage completes
 *                    instantly, the run reports DONE, and no water ever moved.
 *   k far too big -> litres barely climb, so the pump runs to the 180 s hard cap. On SEQ_FILL that
 *                    is the mixing tank overflowing.
 * The ESP1 link has framing but no checksum, so a bit-flip inside a KMAIN/KNUT number survives as a
 * plausible float (toFloat() on a mangled token yields 0). Both intake paths are therefore range
 * checked, following the same "out of range -> ignore, keep the known-good value" convention the
 * WATER token already uses. 450 is the bench value; this band is wide enough for any real sensor. */
const float K_FLOW_MIN = 1.0f, K_FLOW_MAX = 100000.0f;
static bool kSane(float k) { return isfinite(k) && k >= K_FLOW_MIN && k <= K_FLOW_MAX; }

/* ---- Per-column water budget (liters per service) [TBD] ------------------ */
const float WATER_BUDGET_L[3] = { 5.0f, 5.0f, 5.0f };
bool COLUMN_ENABLED[3] = { true, true, false };        // [CONFIRM] mirror ESP1/Nano

/* ---- Stage timings ------------------------------------------------------- */
const unsigned long VALVE_SWITCH_MS   = 1000;   // valve settle before pump (DC stages)
const unsigned long INVERTER_WARMUP_MS = 2000;  // inverter+valve settle before AC pump
const unsigned long FLOW_TIMEOUT_MS   = 10000;  // no-flow after pump start -> FLOW_FAIL
const unsigned long STAGE_MAX_MS      = 180000; // hard safety cap per metered stage
const unsigned long MIX_DEFAULT_MS    = 30000;  // used if work order omits MIX

/* ---- Flow-independent (timed) recovery -- escape hatch when the FLOW SENSOR is dead -------- *
 * A held FLOW_FAIL can never clear via a flow-metered resume (the stage re-detects no-flow and
 * re-holds -> lockout). On RESUME,IRRIGATE / RESUME,RELEASE after a flow fault, the FILL/DELIVER
 * stages run the pump on a TIMER instead, sized from the known volume and pump rate. The PZEM
 * power check still runs, so this is flow-blind, not power-blind. Rates are [MEASURE] at commissioning. */
const float TRANSFER_LPM = 8.0f;                // reservoir->mix transfer pump flow rate [MEASURE]
const float BOOSTER_LPM  = 6.0f;                // mix->column booster pump flow rate     [MEASURE]
const float TIMED_MARGIN = 1.3f;                // run a bit longer than the estimate to fully move the volume
const unsigned long TIMED_STAGE_CAP_MS = 120000; // hard cap on any single timed stage (safety)

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
// Mixing-tank safety ceiling -- must mirror ESP1's MIXING_TANK_MAX_VOLUME. Second line of defence
// on an operator-supplied WATER,<L>: ESP1 caps it too, but ESP2 must never accept a volume its own
// tank cannot hold just because the value arrived over the wire.            [MEASURE]
const float MIXING_TANK_MAX_L = 50.0f;

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
const unsigned long TEST_HARD_CAP_MS     = 30000;   // max continuous ON, single relay (sec.18.10.8.3)
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
  int    col;            // 0..2 -- PRIMARY column (budget lookup, logging)
  // Bitmask of every column to deliver into (bit0=A, bit1=B, bit2=C). Normally just `col`, but an
  // operator FORCE run may open several at once. NOTE: the mixing tank has ONE outlet through ONE
  // flow meter, so with >1 bit set the delivered litres are a COMBINED total -- the per-column split
  // is decided by plumbing resistance and is NOT measurable. ESP1 marks such runs estimated.
  uint8_t colMask;
  // Operator-specified batch volume in litres (TOTAL, not per column). 0 = use WATER_BUDGET_L[col],
  // which is the normal scheduled-run behaviour.
  float  waterL;
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
unsigned long stepStart = 0;
float   woWaterL = 0.0f;        // metered water delivered to the column this work order (for DONE,WATER)
// Set if ANY stage in this work order ran flow-blind (timed recovery), so woWaterL is partly an
// ESTIMATE from the pump rate rather than a metered figure. DONE then carries an ,EST tag --
// otherwise the thesis dataset cannot tell a measured litre from a computed one.
bool    woEstimated = false;

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
bool  sgTimed = false;                   // flow-independent stage: complete on a timer, ignore the flow sensor
unsigned long sgTimedMs = 0;             // computed run time for a timed stage
bool  lastHoldFlow = false;              // the most recent hold was a FLOW_FAIL (enables timed recovery)
bool  resumeTimed  = false;              // this RESUME should run FILL/DELIVER timed (flow-blind)
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
void i2cBusRecover();
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
void teleTick();
void progTick();
int  flowPinForId(const String &id);
bool applySetCal(const String &id, const String *tok, int n, int i);
void setupPrime(const String &line, const String &col);
void stopPrime();
void primeSafety();

/* =============================================================================
 *  SETUP  --  SAFE boot: all outputs OFF before anything else (sec.19.5.4)
 * ========================================================================== */
// Power-on hard-reset one-shot guard: RTC-RAM survives a SW reset but is garbage on a true power loss, so
// this guarantees the clean-boot reset below fires at most once per power-up and can never loop.
RTC_NOINIT_ATTR uint32_t g_porResetMagic;
const uint32_t POR_RESET_DONE = 0xB007CE11;     // marker: already did the one clean power-on reset

void setup() {
  Serial.begin(DEBUG_BAUD);
  delay(50);
  Serial.println(F("\n=== ESP32 #2 Actuator Controller boot ==="));

  // HARD RESET ON EVERY POWER-ON (user req): a relay power-up can leave the ESP32 in a marginal
  // power-on-reset state (the "silent until a manual EN press" boot). So on a genuine power-on we let
  // VCC settle briefly then do ONE clean software reset -- this mimics pressing EN after the rails are
  // stable and gives a reliable boot. The forced reset reports ESP_RST_SW, so the 2nd boot skips this
  // (no reboot loop); brownout/other reset reasons also skip it.
  esp_reset_reason_t rr = esp_reset_reason();
  if (rr == ESP_RST_POWERON) {
    if (g_porResetMagic == POR_RESET_DONE) {
      // We already forced our one clean reset this power cycle but still see POWERON (marginal-power
      // misreport) -> do NOT restart again; break any loop and just boot normally.
      g_porResetMagic = 0;
    } else {
      g_porResetMagic = POR_RESET_DONE;      // mark before the forced restart (survives the SW reset)
      Serial.println(F("Power-on detected -> one clean hard reset for a reliable boot"));
      Serial.flush();
      delay(200);                            // let VCC settle before the clean restart
      ESP.restart();                         // returns as ESP_RST_SW -> the else branch clears the mark
    }
  } else {
    g_porResetMagic = 0;                     // any non-POR reset (SW/WDT/brownout/RESET_SELF/idle) clears it
  }

  // WDT FIRST: arm the 8 s panic-reboot before touching any peripheral, so a stalled cold-boot init
  // (PCF8575/PZEM not yet ready when the GPIO4 relay applies power) auto-recovers instead of hanging
  // silent until a manual EN reset (sec.18.9 / boot-hang fix).
  wdtSetup();

  // I2C: recover a stuck bus, cap the clock, and give endTransmission a short timeout so the first
  // PCF8575 write can never block boot.
  i2cBusRecover();
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(50);                       // ms; bounds a not-ready/stuck-bus transaction
  pcfWrite(0xFFFF);                          // ALL actuators OFF + master cutoff de-energized (SAFE) -- must be first

  esp1Serial.begin(ESP1_BAUD, SERIAL_8N1, ESP1_RX_PIN, ESP1_TX_PIN);

  wo.active = false;
  lastHeartbeat = millis();
  rxLine.reserve(260);   // headroom for the longest (fertigation) work order (~200B)

  tankLoad();                               // restore persisted mixing-tank volume (overfill guard)
  Serial.printf("Restored mixing-tank volume: %.2f L\n", mixTankL);

  // Announce READY as early as possible -- BEFORE the (non-critical) PZEM Modbus probe, which is only
  // used during a run and could otherwise slow/block the boot handshake if the meter is absent/slow.
  feedWDT();
  reply("READY,ESP2");                      // tell ESP32 #1 we are up & SAFE (sec.10.7.4)

  analogReadResolution(12);                 // 0..4095
  pzem.setAddress(0x01);                    // default PZEM Modbus address (begins Serial1) -- deferred, post-READY

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
  teleTick();                  // power telemetry (PZEM V/I/P + ACS) while a run executes -> ESP1 logs it
  progTick();                  // live liters into the current stage -> ESP1 run-progress LCD
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
// Clear a stuck I2C bus (a slave -- e.g. the PCF8575 half-powered at a cold relay boot -- holding SDA low):
// clock SCL up to 9 times then issue a STOP, so the very first Wire transaction in setup() can't hang.
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
    pinMode(I2C_SDA, OUTPUT);                     // generate a STOP: SDA low->high while SCL high
    digitalWrite(I2C_SDA, LOW);  delayMicroseconds(5);
    digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5);
    digitalWrite(I2C_SDA, HIGH); delayMicroseconds(5);
  }
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delay(5);
}

bool pcfWrite(uint16_t s) {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(lowByte(s));
  Wire.write(highByte(s));
  bool ok = (Wire.endTransmission() == 0);          // 0 = ACKed; non-zero = bus/device fault
  // Only track the shadow when the write actually landed. A NAKed write leaves the hardware in its
  // OLD state, so recording the new one would desync shadow from reality and every later pcfOn/pcfOff
  // (which derive their mask FROM the shadow) would compute against a lie.
  if (ok) pcfShadow = s;
  if (!ok) { if (pcfFailCount < 250) pcfFailCount++; }   // consecutive-failure counter (debounce)
  else     { pcfFailCount = 0; }
  return ok;
}
void pcfOn(uint8_t bit)  { pcfWrite(pcfShadow & ~(1 << bit)); }   // clear bit = ON
void pcfOff(uint8_t bit) { pcfWrite(pcfShadow |  (1 << bit)); }   // set bit  = OFF
// Retry the two SAFE-ing writes. Every caller discards pcfWrite's status, so a single NAK here used
// to mean "emergency stop issued, relays still energized, nobody the wiser". No delay(): the I2C
// transaction is already bounded by Wire.setTimeOut(50), so 3 tries is <=150 ms -- far inside the 8 s
// WDT and still non-blocking per spec sec.10.6.5.
void stopAll()           { for (uint8_t a = 0; a < 3; a++) if (pcfWrite(0xFFFF)) return; }
// All actuators OFF but the master cutoff kept ENERGIZED (bank stays powered). One atomic write
// (bit15=0 -> P17 on), so switching Testing components never pulses P17 off->on (sec.19.4.8.4).
void stopKeepBank()      { pcfWrite(0x7FFF); }

/* ---- P17 master actuator-power cutoff (sec.19.4.8) ----------------------- *
 * Active-LOW: clearing bit 15 ENERGIZES the master relay (bank powered); setting it
 * DE-energizes (whole P0-P16 bank goes hardware-dead, fail-safe). Energize before any
 * actuation; de-energize on fault / completion / idle.                                */
void cutoffEnergize()    { pcfOn(OUT_MASTER_CUTOFF); }
// De-energizing is the fail-safe direction -- retry it like stopAll() (see note above).
void cutoffDeenergize()  { for (uint8_t a = 0; a < 3; a++) if (pcfWrite(pcfShadow | (1 << OUT_MASTER_CUTOFF))) return; }

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
  // Clamp the top as well as the bottom. The bookkeeping should never exceed the tank -- SEQ_FILL
  // caps its target -- but if it ever did, an over-reading volume would make the next DELIVER run
  // longer than there is water for, which is how a booster ends up running dry.
  if (mixTankL > MIXING_TANK_MAX_L) mixTankL = MIXING_TANK_MAX_L;
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
  if (testMode) return;                             // no PCF_FAIL/PCF_OK while manually exercising relays (Testing)
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
  // Both intake paths range check K now, but this is the line that would actually produce the
  // damage, so it refuses independently. Returning 0 makes an unusable K look like NO FLOW, which
  // the stage already handles safely (FLOW_TIMEOUT_MS -> FLOW_FAIL -> hold for the operator).
  // Dividing by a zero K instead yields inf, which reads as "target reached" and completes the
  // stage instantly with nothing delivered -- a silent failure, and the worst possible answer.
  if (!kSane(k)) return 0.0f;
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
        // 256B (not 128): a full FERTIGATION work order carries the self-contained job cal
        // (KMAIN/KNUT/ECCAL/PHCAL, §A.5.1 #3) and runs ~200B. 128 would drop every fertigation order.
        if (s < 0 || e <= s || raw.length() > 256) { reply("INVALID,FRAMING"); continue; }
        String p = raw.substring(s + 7, e);
        p.trim();
        if (p.startsWith(",")) p = p.substring(1);
        if (p.endsWith(","))   p = p.substring(0, p.length() - 1);
        dispatch(p);
      }
    } else if (rxLine.length() < 256) rxLine += c; else rxLine = "";   // 256: fits the ~200B fertigation order
  }
}

void dispatch(const String &payload) {
  // tokenize
  // 40, not 24: a full FERTIGATION work order is 34 tokens
  // (SEQ_x,FLUSH,p,DOSE,a,b,c,DCEIL,a,b,c,BATCHV,v,EC,lo,hi,PH,lo,hi,MIX,ms,KMAIN,k,k,
  //  KNUT,a,b,c,ECCAL,m,b,PHCAL,m,b). At 24 the frame still arrived whole (207B < 256) but the
  // tokenizer dropped KNUT/ECCAL/PHCAL, so every fertigation ran on stale nutrient K-factors and
  // stale EC/pH calibration -- silently defeating the self-contained job cal of §A.5.1 #3.
  // A FORCE run adds COLS,<letters> and WATER,<L> -> 38 tokens. 48 leaves headroom.
  // Do NOT shrink this below the largest work order.
  const int MAXT = 48; String tok[MAXT]; int n = 0, start = 0;
  for (int i = 0; i <= payload.length() && n < MAXT; i++) {
    if (i == payload.length() || payload[i] == ',') { tok[n++] = payload.substring(start, i); start = i + 1; }
  }
  if (n == 0) { reply("INVALID,EMPTY"); return; }
  String cmd = tok[0];

  // ---- always-honored commands ----
  if (cmd == "STOP_ALL") {
    stopAll(); step = SEQ_NONE; wo.active = false; faultHeld = false; detachFlow();
    resumeTimed = false; lastHoldFlow = false;           // clear the timed-recovery latch
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
  wo.col = c; wo.colMask = (uint8_t)(1 << c); wo.waterL = 0.0f;
  wo.fertigate = isFert; wo.flushPct = 0;
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
    // COLS,<letters> -- deliver into every listed column at once (operator FORCE run). Disabled
    // columns are silently ignored so a stale dashboard cannot actuate unwired hardware.
    else if (tok[i] == "COLS" && i + 1 < n) {
      String s = tok[++i]; uint8_t m = 0;
      for (unsigned k = 0; k < s.length(); k++) {
        int b = (s[k] == 'A') ? 0 : (s[k] == 'B') ? 1 : (s[k] == 'C') ? 2 : -1;
        if (b >= 0 && COLUMN_ENABLED[b]) m |= (uint8_t)(1 << b);
      }
      if (m) wo.colMask = m;                 // ignore an empty/invalid mask -> keep the single column
    }
    // WATER,<L> -- operator-specified TOTAL batch volume, overriding WATER_BUDGET_L for this run.
    else if (tok[i] == "WATER" && i + 1 < n) {
      float v = tok[++i].toFloat();
      if (v > 0.0f && v <= MIXING_TANK_MAX_L) wo.waterL = v;   // out-of-range -> ignore, use the table
    }
    else if (tok[i] == "EC" && i + 2 < n)    { wo.ecLo = tok[++i].toFloat(); wo.ecHi = tok[++i].toFloat(); }
    else if (tok[i] == "PH" && i + 2 < n)    { wo.phLo = tok[++i].toFloat(); wo.phHi = tok[++i].toFloat(); }
    else if (tok[i] == "MIX" && i + 1 < n)   { wo.mixMs = (unsigned long)tok[++i].toInt(); }
    // Job-critical calibration carried in the work order (§A.5.1 #3): never run a dose on stale K/cal.
    // Range checked (see kSane): an implausible K is ignored so metering keeps the last known-good
    // value, and ESP1 is told, so a corrupted frame is visible instead of silently mis-metering.
    else if (tok[i] == "KMAIN" && i + 2 < n) {
      float a = tok[++i].toFloat(), b = tok[++i].toFloat();
      if (kSane(a)) K_RES_MIX = a; else reply("INVALID,KMAIN_RESMIX");
      if (kSane(b)) K_MIX_IRR = b; else reply("INVALID,KMAIN_MIXIRR");
    }
    else if (tok[i] == "KNUT" && i + 3 < n)  {
      for (int j = 0; j < 3; j++) {
        float k = tok[++i].toFloat();
        if (kSane(k)) K_NUT[j] = k; else reply(String("INVALID,KNUT_") + (char)('A' + j));
      }
    }
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
  doseIdx = 0;
  woWaterL = 0.0f;
  woEstimated = false;
  cutoffEnergize();              // power the actuator bank (sec.19.4.8); individual relays still gate each load
  Serial.printf("Work order: %s fert=%d flush=%.0f%% tank=%.2fL\n",
                wo.cmdName.c_str(), wo.fertigate, wo.flushPct, mixTankL);
  goStep(SEQ_FILL);
}

/* ---- Stage reporting to ESP1 (drives the LCD run-progress screen) --------- *
 * LCD-friendly names (<=16 chars so "N/M " + name fits one 20-col row). The enum value doubles as the
 * fertigation ordinal (FILL=1 .. FLUSH_DELIVER=7); irrigation only runs FILL then DELIVER, so it is
 * renumbered 1..2. ESP1 never has to know the sequence shape -- it just renders what it is told.      */
const char *SEQ_NAME[8] = { "", "Transfer Water", "Pump Nutrients", "Mixing",
                            "Check EC/pH", "Release", "Flush Fill", "Flush Release" };
static uint8_t stepOrdinal(SeqStep s) {
  if (wo.fertigate) return (uint8_t)s;                                   // 1..7 maps straight through
  return (s == SEQ_FILL) ? 1 : (s == SEQ_DELIVER) ? 2 : 0;               // irrigation: 2 visible steps
}
static uint8_t stepTotal() {
  if (!wo.fertigate) return 2;
  return (wo.flushPct > 0) ? 7 : 5;                                      // flush stages only when asked
}

void goStep(SeqStep s) {
  step = s; stepInit = false;
  if (s == SEQ_NONE) return;
  reply("STAGE," + String(stepOrdinal(s)) + "," + String(stepTotal()) + "," + SEQ_NAME[s]);
}

/* ---- Live progress (liters into the current metered stage) ---------------- *
 * ~1 Hz while a work order runs so ESP1 can draw a moving bar. Cheap: no extra hardware reads.       */
const unsigned long PROG_MS = 1000;
unsigned long lastProgMs = 0;
void progTick() {
  if (!wo.active) { lastProgMs = 0; return; }
  if (millis() - lastProgMs < PROG_MS) return;
  lastProgMs = millis();
  float L = (sgPump >= 0 && sgPumpOn) ? litersSoFar(sgK) : 0.0f;
  reply("PROG," + String(L, 3) + "," + String(sgTarget, 3) + "," + String(woWaterL, 2));
}

void stageBegin(int pumpBit, bool isAC, int flowPin, float k, float targetL) {
  sgPump = pumpBit; sgIsAC = isAC; sgFlow = flowPin; sgK = k; sgTarget = targetL;
  sgTankSign = 0;                // default: stage does not move tank water (set by caller for fill/deliver)
  sgCapMs = STAGE_MAX_MS;        // default cap (dose stages override with the per-dose ceiling)
  sgPumpOn = false; sgSawFlow = false; sgT0 = millis();
  sgTimed = false; sgTimedMs = 0;   // default: flow-metered; a timed-recovery stage sets these after this call
  acNoCurrentSince = 0;
  if (isAC) pcfOn(OUT_INVERTER);
}

// End a metered FILL/DELIVER stage, crediting the liters it moved to the persisted tank
// volume (and, for a DELIVER, to the per-order water-to-column tally). Reads before stageEnd
// so it works for both normal completion and a mid-stage fault (partial liters still count).
float endMeteredStage() {
  // A timed (flow-blind) stage has no usable pulse count, so credit the ESTIMATED target volume it was
  // sized to move -- otherwise the tank bookkeeping would think nothing moved and never empty/fill.
  float liters = sgTimed ? sgTarget : litersSoFar(sgK);
  if (sgTimed) woEstimated = true;            // this order's water total is no longer purely metered
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
  // Timed (flow-blind) recovery stage: the flow sensor is dead, so complete on the clock and NEVER
  // raise the no-flow fault. PZEM power validation (pwrValidate) still runs -> not power-blind.
  if (sgTimed) {
    unsigned long el = millis() - sgT0;
    if (el >= sgTimedMs) return 1;
    if (el > TIMED_STAGE_CAP_MS) return 1;              // absolute safety cap -> stop (do not re-fault)
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
  bool est = woEstimated;
  wo.active = false; faultHeld = false; step = SEQ_NONE;
  resumeTimed = false; lastHoldFlow = false;   // run finished -> clear the timed-recovery latch
  woEstimated = false;
  // Append measured water so ESP1 can tally per-column daily usage (sec.12.1.3 / B1). ,EST marks a
  // total that includes a flow-blind (timed) stage. ESP1 reads this with indexOf("WATER,")+toFloat(),
  // which stops at the comma, so the extra token is backward-compatible.
  reply("DONE," + name + ",WATER," + String(water, 2) + (est ? ",EST" : ""));
}

// Hard fault (sec.19.4.8.1): drop the master cutoff (bank hardware-dead), PAUSE the sequence
// but KEEP the work order + step + the persisted mix-tank volume, and STAY ALIVE awaiting a
// user-gated RESUME,<mode>. ESP2 does NOT power itself off -- that would forget the water and
// overfill on resume; ESP1 keeps it powered while held.
void holdFault(const char *resp, const char *loc) {
  bool isFlow = (strcmp(resp, "FLOW_FAIL") == 0);
  unsigned long fp = 0;
  if (isFlow) { noInterrupts(); fp = flowPulses; interrupts(); }   // snapshot pulses seen this stage BEFORE detach
  stageEnd();                        // pump off + flow detached (partial liters already credited by caller)
  stopAll();                         // all OFF, master cutoff de-energized
  faultHeld = true;
  stepInit  = false;                 // on RESUME the paused step re-inits with the updated tank volume
  lastHoldFlow = isFlow;             // a dead flow sensor enables timed IRRIGATE/RELEASE recovery
  // For FLOW_FAIL, append the pulse count: pulses=0 means the pump/sensor produced NOTHING (dead pump,
  // dead sensor, or no water); pulses>0 means flow started then stalled (air, weak pump, sensor half in
  // path). That single number tells the field team which hardware to inspect (diagnostic C-3).
  if (isFlow) reply(String(resp) + "," + loc + "|pulses=" + String(fp));
  else        reply(String(resp) + "," + loc);
}

// User-gated recovery (sec.19.4.8.2). NORMAL resumes the paused step; IRRIGATE finishes as
// irrigation-only (top up plain water to the column budget, no dosing); RELEASE dumps the
// current tank contents to the assigned column as-is. All re-energize P17 first.
void resumeWork(const String &mode) {
  faultHeld = false;
  cutoffEnergize();
  // Flow sensor dead + user chose a deliver/top-up recovery -> run those stages on a timer (flow-blind),
  // otherwise they would re-detect no-flow and re-hold forever. Metered recovery stays for other faults.
  resumeTimed = (lastHoldFlow && (mode == "IRRIGATE" || mode == "RELEASE"));
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

// Open / close every column valve in the work order's mask. Normally exactly one bit; an operator
// FORCE run may set several, delivering one batch into them simultaneously.
static void colValves(uint8_t mask, bool on) {
  for (int b = 0; b < 3; b++) {
    if (!(mask & (1 << b))) continue;
    if (on) pcfOn(COL_VALVE_BIT[b]); else pcfOff(COL_VALVE_BIT[b]);
  }
}
// "COL_A" / "COL_AB" -- names every column the batch actually went to, so the log and any fault
// reply cannot imply a single column when the water was shared.
static String colMaskName(uint8_t mask) {
  String s = "COL_";
  for (int b = 0; b < 3; b++) if (mask & (1 << b)) s += (char)('A' + b);
  return s;
}

void runSequence() {
  if (step == SEQ_NONE) return;
  if (faultHeld) return;                 // paused after a fault -> wait for RESUME (sec.19.4.8)
  int c = wo.col;
  String colLocS = colMaskName(wo.colMask);
  const char *colLoc = colLocS.c_str();

  switch (step) {

    /* ---- FILL: reservoir -> mixing tank (transfer pump, flow 4) ---------- *
     * Target is the REMAINING liters to reach the desired mix level minus what is
     * already in the tank (mixTankL) -- so a resume never re-fills from 0 and overfills. */
    case SEQ_FILL: {
      if (!stepInit) {
        // Operator FORCE runs carry their own TOTAL batch volume; scheduled runs use the table.
        float budget  = (wo.waterL > 0.0f) ? wo.waterL : WATER_BUDGET_L[c];
        float desired = wo.fertigate ? budget * (1.0f - wo.flushPct / 100.0f) : budget;
        float remaining = desired - mixTankL;
        // Never ask for more than the tank can still hold. `desired` is already bounded, but the
        // stage can overshoot its target: with a wrong-but-plausible K the metered litres under-read
        // and the pump runs to STAGE_MAX_MS instead -- 180 s at TRANSFER_LPM is ~24 L, which on top
        // of a partly-full tank would put water on the floor. Capping the request means the hard cap
        // is the only overshoot, not the target as well.
        float headroom = MIXING_TANK_MAX_L - mixTankL;
        if (remaining > headroom) remaining = headroom;
        if (remaining < TANK_EPS_L) {                 // already enough in the tank -> skip the fill
          goStep(wo.fertigate ? SEQ_DOSE : SEQ_DELIVER); break;
        }
        pcfOn(OUT_RES_VALVE);
        stageBegin(OUT_TRANSFER, true, FLOW_RES_MIX, K_RES_MIX, remaining);
        sgTankSign = +1;                              // this stage adds water to the mix tank
        if (resumeTimed) {                            // flow sensor dead -> run the transfer pump on a timer
          sgTimed = true;
          sgTimedMs = (unsigned long)(remaining / TRANSFER_LPM * 60000.0f * TIMED_MARGIN);
          if (sgTimedMs > TIMED_STAGE_CAP_MS) sgTimedMs = TIMED_STAGE_CAP_MS;
        }
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
        // Dosed liquid physically enters the mixing tank, so it must count toward mixTankL -- otherwise
        // a dose interrupted mid-pour leaves the tank bookkeeping short and a resumed FILL overfills.
        sgTankSign = +1;
        sgCapMs = wo.doseCeilMs[doseIdx] ? wo.doseCeilMs[doseIdx] : STAGE_MAX_MS;   // per-dose 2x ceiling (spec §2.4)
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1) {
        float measuredML = endMeteredStage() * 1000.0f;   // credits the tank, then ends the stage
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
        endMeteredStage();                   // credit whatever partial volume did make it into the tank
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
      if (phRaw <= PH_ADC_FAULT_LO || phRaw >= PH_ADC_FAULT_HI) { reply("SENSOR_FAIL,PH"); }
      else if (ph < wo.phLo || ph > wo.phHi)                    { reply(String("PH_FAIL,") + colLoc); }
      if (ecRaw <= EC_ADC_FAULT_LO || ecRaw >= EC_ADC_FAULT_HI) { reply("SENSOR_FAIL,EC"); }
      else if (ec < wo.ecLo || ec > wo.ecHi)                    { reply(String("EC_FAIL,") + colLoc); }
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
        pcfOn(OUT_MIX_VALVE); colValves(wo.colMask, true);
        stageBegin(OUT_BOOSTER, true, FLOW_MIX_IRR, K_MIX_IRR, mixTankL);
        sgTankSign = -1;                                     // this stage drains the mix tank
        if (resumeTimed) {                                   // flow sensor dead -> run the booster on a timer
          sgTimed = true;
          sgTimedMs = (unsigned long)(mixTankL / BOOSTER_LPM * 60000.0f * TIMED_MARGIN);
          if (sgTimedMs > TIMED_STAGE_CAP_MS) sgTimedMs = TIMED_STAGE_CAP_MS;
        }
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1) {
        endMeteredStage(); pcfOff(OUT_MIX_VALVE); colValves(wo.colMask, false);
        if (wo.fertigate && wo.flushPct > 0) goStep(SEQ_FLUSH_FILL);
        else finishOk();
      } else if (r == -1) {
        endMeteredStage(); pcfOff(OUT_MIX_VALVE); colValves(wo.colMask, false);
        holdFault("FLOW_FAIL", "MAIN");
      }
      break;
    }

    /* ---- FLUSH_FILL: plain reservoir water -> mixing tank ---------------- */
    case SEQ_FLUSH_FILL: {
      if (!stepInit) {
        float remaining = ((wo.waterL > 0.0f) ? wo.waterL : WATER_BUDGET_L[c]) * (wo.flushPct / 100.0f) - mixTankL;
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
        pcfOn(OUT_MIX_VALVE); colValves(wo.colMask, true);
        stageBegin(OUT_BOOSTER, true, FLOW_MIX_IRR, K_MIX_IRR, mixTankL);
        sgTankSign = -1;
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1)      { endMeteredStage(); pcfOff(OUT_MIX_VALVE); colValves(wo.colMask, false); finishOk(); }
      else if (r == -1){ endMeteredStage(); pcfOff(OUT_MIX_VALVE); colValves(wo.colMask, false); holdFault("FLOW_FAIL", "MAIN"); }
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
  // HARD CAP: force OFF regardless of ESP1. Both are 30 s (sec.18.10.8.3 / CLAUDE.md).
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
  if (id == "FLOW_NUTD")   return FLOW_NUT_D;   // wired but not metered in normal operation
  if (id == "FLOW_PHUP")   return FLOW_PH_UP;
  if (id == "FLOW_PHDN")   return FLOW_PH_DN;
  return -1;
}

// Stream the selected sensor's RAW value (~1.6 Hz) for interactive calibration / Sensor Diag (§A.3).
void calStreamTick() {
  if (calId == "") return;
  if (millis() - lastCalMs < CAL_STREAM_MS) return;
  lastCalMs = millis();
  // PZEM is factory-calibrated -> stream the ENGINEERING value (V/I/P), not a raw ADC.
  if (calId == "PZEM_V" || calId == "PZEM_I" || calId == "PZEM_P") {
    float v = (calId == "PZEM_V") ? pzem.voltage() : (calId == "PZEM_I") ? pzem.current() : pzem.power();
    if (isnan(v)) v = -1;                       // no PZEM / no reply -> -1 sentinel
    reply("CAL," + calId + "," + String(v, 2));
    return;
  }
  long raw;
  if      (calId == "PH")     raw = analogRead(PIN_PH);
  else if (calId == "EC")     raw = analogRead(PIN_EC);
  else if (calId == "ACS712") raw = analogRead(PIN_MIXER_I);
  else if (calId.startsWith("FLOW_")) { noInterrupts(); raw = (long)flowPulses; interrupts(); }
  else return;
  reply("CAL," + calId + "," + String(raw));
}

// Periodic power telemetry WHILE a work order runs (PZEM V/I/P + ACS712 mixer current). ESP1 logs it
// so the operator can see actual power draw during irrigation/fertigation. Idle = ESP2 off = nothing.
const unsigned long TELE_MS = 5000;
unsigned long lastTeleMs = 0;
void teleTick() {
  if (step == SEQ_NONE && !wo.active) { lastTeleMs = 0; return; }   // only during an active run
  if (millis() - lastTeleMs < TELE_MS) return;
  lastTeleMs = millis();
  float v = pzem.voltage(), i = pzem.current(), p = pzem.power();
  if (isnan(v)) v = -1; if (isnan(i)) i = -1; if (isnan(p)) p = -1;
  // Mixing-tank EC/pH ride along with the power telemetry. Until now these were only ever reported
  // on a FAULT (EC_FAIL / PH_FAIL), so ESP1 had no value to publish and the dashboard's Water pH and
  // Water EC tiles read "--" permanently. Same conversion the SEQ_ECPH check uses. A railed probe is
  // sent as -1 so ESP1 can tell "not measured" from a genuine reading.
  int phRaw = analogRead(PIN_PH), ecRaw = analogRead(PIN_EC);
  float ph = (phRaw <= PH_ADC_FAULT_LO || phRaw >= PH_ADC_FAULT_HI) ? -1.0f : (PH_CAL_M * phRaw + PH_CAL_B);
  float ec = (ecRaw <= EC_ADC_FAULT_LO || ecRaw >= EC_ADC_FAULT_HI) ? -1.0f : (EC_CAL_M * ecRaw + EC_CAL_B);
  reply("TELE,PZEM," + String(v, 1) + "," + String(i, 2) + "," + String(p, 1) +
        ",ACS," + String(readMixerCurrent(), 2) +
        ",ECPH," + String(ec, 2) + "," + String(ph, 2));
}

// Apply one pushed calibration constant to the matching runtime variable. pH/EC carry two
// values (slope M then offset B). Flow ids that ESP2 does not meter in normal operation
// (NUTD/PHUP/PHDN) are accepted as a no-op so the ESP1 block-until-ACK save still completes.
bool applySetCal(const String &id, const String *tok, int n, int i) {
  // Flow K-factors are range checked here too (see kSane) -- SET_CAL is the other way a bad divisor
  // can reach the metering path. Returning false makes ESP1 see INVALID,SET_CAL_ID rather than a
  // silent accept, so a mistyped or corrupted calibration cannot quietly take over the metering.
  if      (id == "FLOW_RESMIX") { if (!kSane(tok[i].toFloat())) return false; K_RES_MIX = tok[i].toFloat(); }
  else if (id == "FLOW_MIXIRR") { if (!kSane(tok[i].toFloat())) return false; K_MIX_IRR = tok[i].toFloat(); }
  else if (id == "FLOW_NUTA")   { if (!kSane(tok[i].toFloat())) return false; K_NUT[0]  = tok[i].toFloat(); }
  else if (id == "FLOW_NUTB")   { if (!kSane(tok[i].toFloat())) return false; K_NUT[1]  = tok[i].toFloat(); }
  else if (id == "FLOW_NUTC")   { if (!kSane(tok[i].toFloat())) return false; K_NUT[2]  = tok[i].toFloat(); }
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
