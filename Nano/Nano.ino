/* =============================================================================
 *  SMART IRRIGATION & FERTIGATION SYSTEM  --  ARDUINO NANO SENSOR HUB
 *  Controller 1 of 3   (Nano  ->  ESP32 #1  ->  ESP32 #2)
 * -----------------------------------------------------------------------------
 *  ROLE (spec sec.10.3, 11.5 / CLAUDE.md): SENSOR ACQUISITION + ONE COMMANDED ACTUATOR.
 *    - Reads sensors, frames packets, transmits to ESP32 #1 over UART.
 *    - Obeys a small inbound command set (interval switch, reset, FAST, SERVO).
 *    - Drives ONE actuator, and only on command: the pump-selector servo on D3 (2026-11-15).
 *      It flips the changeover switch that routes the inverter's AC to the TRANSFER pump (LEFT)
 *      or the BOOSTER pump (RIGHT). ESP32 #2 asks for it (through ESP32 #1) only while its
 *      inverter is OFF, so the switch never makes/breaks load current and cannot weld. The Nano
 *      never moves it by itself -- not at boot, not after a reset, not on a timer.
 *    - Makes NO decisions, classifies NO faults.
 *      Invalid sensor reads are reported honestly with a -1 sentinel; ESP32 #1
 *      decides what that means. -1 is a valid "invalid read" flag, NOT garbage.
 *
 *  FAST STATE (2026-11-15): while ESP32 #2 moves water (transfer reservoir -> mixing tank, or
 *      drain mixing tank -> field), ESP32 #1 sends FAST. The Nano then puts the ultrasonics and the
 *      servo first: TANK every FAST_TANK_MS (~3 Hz) so an overflow cannot be missed, heartbeat every
 *      2 s, ENV/SOIL/LIGHT only once a minute, NPK skipped (it blocks up to 1 s per column). FAST is a
 *      LEASE: without a renewing FAST for FAST_LEASE_MS the Nano returns to its previous state on its
 *      own, so a dead ESP32 #1 can never leave it there. In every state the loop keeps polling the
 *      UART between single sensor reads, so a SERVO/FAST command is never stuck behind a slow cycle.
 *
 *  UART PROTOCOL (spec sec.9.9): framed, UPPERCASE, comma-separated, <=128 bytes
 *      Outbound frames also carry a checksum field before <END>, which the list below leaves out:
 *        <START>,ENV,28.1,65.0,*C0,<END>   (CRC-8 of "ENV,28.1,65.0"; see endPkt())
 *      ESP32 #1 rejects a frame with a bad or missing checksum, so flash the Nano and ESP32 #1 together.
 *      Outbound (Nano -> ESP32 #1):
 *        <START>,ENV,<temp>,<hum>,<END>
 *        <START>,TANK,<resCm>,<mixCm>,<flowLpm>,<resSpread>,<mixSpread>,<st>,<seq>,<END>  (reservoir/mixing-tank
 *              RAW distance in cm -- web v.4: the MEDIAN of the background pinger's last 5 pings, with the
 *              width of their middle three as the spread; -1 with fewer than 3 good pings. ESP32 #1 maps to
 *              % and litres using the stored geometry, per Calibration Mode's RAW-streaming convention below;
 *              flowLpm is the only already-scaled field here. 2026-11-16 link hardening: <st> = two letters,
 *              the pinger's status for the reservoir then the mixing tank (K ok, E no echo, R echo out of range,
 *              F too few good pings, S echo line stuck high, X no pings at all), and <seq> = this TANK's number,
 *              0..999, so ESP32 #1 can drop a repeat and count frames lost on the link. Flash ESP32 #1 with it:
 *              an older ESP32 #1 rejects the 8-field TANK.)
 *        <START>,SOIL,<COL>,<p1>,<p2>,...,<END>      (tag + 2 raw probes per enabled column; disabled omitted)
 *        <START>,LIGHT,<lux>,<END>
 *        <START>,NPK,<COL>,<moist>,<temp>,<EC>,<pH>,<N>,<P>,<K>,<reason>,<END>  (one per enabled column;
 *              reason = OK|TIMEOUT|BADADDR|BADLEN|BADCRC -- OK carries values, the rest carry -1s)
 *        <START>,STATUS,NANO,OK,<END>                (heartbeat, spec sec.10.8.2)
 *        <START>,CAL,<id>,<raw>,<END>                (Calibration Mode raw stream while CAL_START is
 *              active, ~cadence per calStreamTick(); <raw> is a real -1 on a failed/out-of-range read,
 *              never a plausible-looking value -- see calReadRaw())
 *        <START>,SERVO,<LEFT|RIGHT>,<MOVING|OK>,<seq>,<END>  pump-selector servo: MOVING on accept,
 *              OK once it sits at the target and has settled (seq echoes the request)
 *      Inbound (ESP32 #1 -> Nano, spec sec.9.7.1.1):
 *        Every inbound frame MAY carry the same ",*XX" CRC-8 before <END>; when present it must match
 *        (a corrupted command is dropped). SERVO REQUIRES it -- it is the one command that moves hardware.
 *        <START>,ACTIVE,<END>     active interval (10 s) -- also ends FAST
 *        <START>,DAY,<END>        day-idle interval (20 min)
 *        <START>,NIGHT,<END>      night-idle interval (1 hr)
 *        <START>,FAST,<END>       fast level-watch state (renew every <= FAST_LEASE_MS)
 *        <START>,SERVO,<LEFT|RIGHT>,<seq>,*XX,<END>  move the pump selector (LEFT=transfer, RIGHT=booster)
 *        <START>,RESET_REQ,<END>  clean software self-reset (short-timeout WDT)
 *        <START>,CAL_START,<id>,<END>  begin raw stream for one sensor id (Calibration Mode, idle-only)
 *        <START>,CAL_STOP,<id>,<END>   end it
 *
 *  RECOVERY LADDER (spec sec.17.4.1 / CLAUDE.md), 2 layers:
 *      1. Internal ATmega328 WDT, petted in loop()  <-- primary self-recovery
 *      2. ESP32 #1 RESET_REQ over UART              <-- handled here (self-reset)
 *      (No hardware reset: PCF8575 P17 became the master actuator cutoff, so RESET is not driven.)
 *
 *  NON-BLOCKING: no delay() in loop() except a single one-time settle in setup() and
 *      one bounded ~5 ms RS485 DE settle inside the NPK read (well under WDTO_4S).
 *      The servo jumps to its target (SERVO_STEP_MS 0) and is timed by millis() in servoTick, never by delay().
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
 *    (SoftwareSerial for the RS485/NPK bus, Wire for BH1750 I2C and EEPROM ship with the AVR core;
 *     Servo comes with the Arduino IDE, and PlatformIO pulls it via lib_deps;
 *     the NPK Modbus query is hand-rolled so a proven config can be pasted.)
 *    Servo uses Timer1, so D9/D10 lose analogWrite() -- they are SoftwareSerial pins, never PWM.
 * ========================================================================== */

#include <Arduino.h>
#include <avr/wdt.h>
#include <Wire.h>
#include <BH1750.h>
#include <DHT.h>
#include <SoftwareSerial.h>
#include <Servo.h>
#include <EEPROM.h>

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
const uint8_t PIN_SERVO     = 3;    // D3  pump-selector servo signal (was reserved; 2026-11-15)
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
// Checked 2026-11-16 against Calibration/Nano/Soil (the tool that captures the dry/wet endpoints): same pins
// per column, same sampling (soilRead), same rails and column rule as ESP32 #1's soilCombineCal().
const uint8_t SOIL_SAMPLES = 64;               // = the calibration tool's SAMPLES
const int     SOIL_RAIL_LO = 8, SOIL_RAIL_HI = 1015;   // = ESP32 #1's SOIL_ADC_RAIL_LO/HI (disconnected/shorted)
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

/* ---- FAST level-watch state (pump moving water) -------------------------- *
 * Ultrasonics + servo first. HC-SR04 needs >= 60 ms between pings; the background pinger (usTick) pings
 * each sensor every ~70 ms and a TANK just reports the medians, ~55 bytes = ~57 ms of the 9600 baud link,
 * so 300 ms leaves the link and the loop plenty of room while giving ESP32 #2 three samples a second.
 * web v.4: ESP32 #1 also holds the Nano in FAST for the whole of a work order (ESP32 #2's LEVEL_FEED), not
 * only while a pump runs -- the levels between the pumps are the settled baselines of the metering.   */
const unsigned long FAST_TANK_MS  = 300;      // TANK cadence in FAST                       [CONFIRM]
const unsigned long FAST_HB_MS    = 2000;     // heartbeat in FAST
const unsigned long FAST_SLOW_MS  = 60000;    // ENV/SOIL/LIGHT in FAST (keeps ESP1's 5 min staleness away)
const unsigned long FAST_LEASE_MS = 15000;    // no renewing FAST this long -> back to the previous state

/* ---- Pump-selector servo (D3) -------------------------------------------- *
 * An SG90 flips the changeover switch that routes the inverter's AC output to ONE pump.
 * LEFT = transfer pump (reservoir -> mixing tank), RIGHT = booster pump (mixing tank -> field).
 * Angles and speed are the ones proven on the bench (sketch_nov15a).                  [CONFIRM]
 * Power it from its own 5 V supply (common GND), not the Nano's regulator: a stalled SG90 draws
 * enough to brown the Nano out.                                                                  */
// SWAPPED 2026-11-16 (user): in calibration the selector put the wrong pump on, so the transfer side is now 90 deg and
// the booster side 160 deg (they were 160 / 90 from sketch_nov15a). The protocol words keep their meaning -- "LEFT" =
// the TRANSFER side, "RIGHT" = the BOOSTER side, in ESP1, ESP2 and the website -- only the angle behind each changed.
const uint8_t       SERVO_LEFT_DEG  = 90;     // TRANSFER pump side                         [CONFIRM]
const uint8_t       SERVO_RIGHT_DEG = 160;    // BOOSTER pump side                          [CONFIRM]
// sketch_nov15a servoDelay. 0 = no ramp: the target pulse goes out at once and the SG90 swings at full
// speed, exactly like the bench sketch (servoDelay = 0, user 2026-11-15). >0 = ms per one-degree step.
const unsigned long SERVO_STEP_MS   = 0;
// Keep the pulse at the target this long before OK. With no ramp this is also the travel time: an SG90
// needs ~0.12 s for the 70 deg swing unloaded, more against the switch, so 500 ms leaves room.
// 2026-11-16: 500 -> 1000 ms. The PWM is cut (detach) when this ends, and against the switch's detent the SG90 can need
// longer than 0.5 s for the 70 deg swing: cut too early, the lever is left short of its position. The OK is only
// sent after this, so ESP2 never powers a pump on a half-thrown switch.
const unsigned long SERVO_SETTLE_MS = 1000;
// 1 = stop the PWM after each move. The switch's own detent holds the lever, and an un-driven servo
// cannot twitch when SoftwareSerial (NPK), DHT or pulseIn hold interrupts off -- a twitch while the
// inverter is live would be exactly the arcing this servo exists to prevent.
#define SERVO_DETACH_AFTER_MOVE 1
const int           SERVO_EE_ADDR   = 0;      // EEPROM: last angle, the ramp start after a reset
// 2026-11-16 (user): save only the FINAL position. ESP32 #2 now pre-seats the lever with 3 full swings (6 moves,
// under a second apart) before every pump start, and saving after each move wore the one EEPROM cell ~6x per start.
// The angle is saved once the servo has been still this long; EEPROM.update() skips it when unchanged, so a start
// that ends on the same pump as before writes nothing. A RESET_REQ flushes a pending save first.
const unsigned long SERVO_EE_SAVE_DELAY_MS = 10000;

/* ---- Watchdog ------------------------------------------------------------ *
 * Must comfortably exceed the worst-case single loop iteration (a full sensor
 * cycle, dominated by NPK Modbus timeouts). WDTO_4S leaves a wide margin.      */
#define WDT_TIMEOUT  WDTO_4S
const unsigned long WDT_TIMEOUT_MS = 4000;   // must match WDT_TIMEOUT above (WDTO_4S) -- kept as a
                                              // plain number too so NPK_RESPONSE_TIMEOUT_MS below can
                                              // be checked against it at compile time

/* ---- One-time sensor settle (setup() only) ------------------------------- */
const unsigned long SENSOR_SETTLE_MS = 2000;   // DHT22 warm-up + ultrasonic settle

/* ---- Conversion constants moved to ESP32 #1 (companion spec §A) ----------- *
 * This Nano now streams RAW signals (raw soil ADC, raw ultrasonic distance cm, raw NPK
 * registers); ESP32 #1 owns and applies all calibration via its Calibration menu. So the soil
 * dry/wet ADC endpoints, the ultrasonic empty/full tank geometry, and the NPK scale factors live
 * on ESP32 #1 (NVS) -- do NOT tune them here, it would have no effect.                          */

/* ---- Ultrasonic raw read (HC-SR04 style) --------------------------------- *
 * distance(cm) = echo_us * SOUND_CM_PER_US. Sent raw; ESP32 #1 maps to % and litres (and corrects for the
 * air temperature -- this constant is the 20 degC speed of sound on purpose, ESP32 #1 owns the correction). */
const float         SOUND_CM_PER_US      = 0.0343f / 2.0f;  // round-trip -> one way
// Back to web v.3's 25 ms (2026-11-17 log): with 12 ms EVERY ping of both sensors timed out from the moment the v.4
// Nano was flashed (TANK st=EE, 14:09-14:59), while v.3's 25 ms read 23-25 cm / 33-42 cm all morning. These
// waterproof modules start their echo pulse late after the trigger, so the wait must cover that delay as well as
// the echo itself. 25 ms still bounds one lost echo.                                         [MEASURED]
const unsigned long ULTRASONIC_TIMEOUT_US = 25000UL;

/* ---- Ultrasonic hardening (web v.4 "pure ultrasonic", 2026-11-16) ---------- *
 * The mixing-tank ultrasonic now METERS every litre the transfer and booster pumps move (the ResMix / MixIrr
 * flow meters are gone), so one ping is no longer good enough: a splash, a ripple or an echo off the tank
 * wall must not become a level. A background pinger fires ONE ping per loop pass at most, alternating the
 * two sensors every US_PING_GAP_MS (each sensor every 2 x 35 = 70 ms -- an HC-SR04 / JSN-SR04T wants >= 60 ms
 * between its own pings), and keeps the last US_WIN pings of each. TANK then sends, per sensor:
 *   - the MEDIAN of the fresh pings (needs >= US_MIN_GOOD of them, else -1 = no reading), and
 *   - their SPREAD: the width of the middle three (one wild ping on either side does not count), so ESP32 #1
 *     can refuse a median taken from a churning surface.
 * Pings outside US_MIN_CM..US_MAX_CM are not pings (dead zone / nothing in range) and count as lost.
 * No delay(): each ping is one bounded pulseIn (<= ULTRASONIC_TIMEOUT_US), between which the loop runs.      */
#define US_WIN 5
// Why a ping failed (stored per ping, reported as the TANK status letter): a stuck echo line is told apart from
// "no echo", so a hung transducer shows up as such instead of as an empty tank or a calm surface.
enum UsWhy { US_OK = 0, US_TIMEOUT = 1, US_RANGE = 2, US_STUCK = 3 };
const uint8_t       US_MIN_GOOD    = 3;        // fresh valid pings needed for a median
const unsigned long US_PING_GAP_MS = 50;       // between any two pings: each sensor every 100 ms (35 -> 50,
                                               // 2026-11-17: these modules want ~100 ms between their own pings)
const unsigned long US_STALE_MS    = 1000;     // a ping older than this is not part of the median
const float         US_MIN_CM      = 2.0f;     // closer = inside the transducer's dead zone      [MEASURE]
const float         US_MAX_CM      = 200.0f;   // farther = not this tank                         [MEASURE]

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
// Coupled to WDT_TIMEOUT_MS: this wait, plus loop()'s other per-iteration work (ENV/TANK/SOIL/LIGHT
// reads + UART writes), all happen between one wdt_reset() and the next. A future edit that raises
// this in isolation (e.g. to accommodate a slower NPK sensor) could erode that margin without
// anything flagging the coupling -- catches the most obvious case (this wait alone approaching the
// WDT period) at compile time; doesn't replace hand-verifying the full worst-case chain.
static_assert(NPK_RESPONSE_TIMEOUT_MS * 2 < WDT_TIMEOUT_MS,
              "NPK_RESPONSE_TIMEOUT_MS leaves too little WDT margin -- see WDT_TIMEOUT_MS");
// Print precision per field (decimals) for the RAW register outbound packet (moist,temp,EC,pH,N,P,K).
// Sized off NPK_REG_COUNT (not a bare 7) so a future commissioning change to that [CONFIRM]ed count
// can't desync this array -- or the two loop bounds below -- from it and read past a shrunk buffer.
const uint8_t NPK_PREC[NPK_REG_COUNT]  = { 1, 1, 0, 1, 0, 0, 0 };
/* ---- end NPK editable block ---------------------------------------------- */

/* =============================================================================
 *  GLOBALS  (no further user tuning below this line)
 * ========================================================================== */
DHT          dht(PIN_DHT, DHT22);
BH1750       lightMeter;                                  // I2C on A4/A5
SoftwareSerial npkSerial(PIN_RS485_RX, PIN_RS485_TX);     // RX=D9, TX=D10

enum TxState { TX_ACTIVE, TX_DAY, TX_NIGHT, TX_FAST };
TxState txState = BOOT_TX_STATE;
TxState fastReturn = BOOT_TX_STATE;            // state to go back to when the FAST lease ends
unsigned long lastFastRenewMs = 0;             // last FAST received
unsigned long lastFastTankMs  = 0;             // last FAST-cadence TANK

/* ---- Sensor cycle, one read per loop() pass ------------------------------ *
 * The old cycle sent ENV..NPK back to back (NPK alone up to 1 s per column), so a command could
 * wait seconds in the UART buffer. Now each pass does one step and pollUart()/servoTick() run in
 * between; FAST drops what is left of a cycle, and NPK waits while the servo moves.            */
enum CycStep { CY_IDLE, CY_ENV, CY_TANK, CY_SOIL, CY_LIGHT, CY_NPK };
CycStep cycStep   = CY_IDLE;
uint8_t cycNpkCol = 0;

/* ---- Pump-selector servo state ------------------------------------------- */
Servo   pumpServo;
enum SvState { SV_IDLE, SV_RAMP, SV_SETTLE };
SvState svState  = SV_IDLE;
uint8_t svPos    = SERVO_LEFT_DEG;             // believed angle (loaded from EEPROM in setup)
uint8_t svTarget = SERVO_LEFT_DEG;
char    svName[6] = "";                        // "LEFT" / "RIGHT" of the current target
char    svSeq[8]  = "";                        // request id echoed in the replies
unsigned long svStepMs = 0;
bool          svEePending = false;             // a final angle waits to be saved (SERVO_EE_SAVE_DELAY_MS)
unsigned long svEeIdleMs  = 0;                 // when the servo last became still

volatile unsigned long flowPulseCount = 0;     // incremented in ISR
unsigned long lastFlowCalcMs = 0;

/* ---- Ultrasonic background pinger (web v.4) ------------------------------- */
struct UsRing { float v[US_WIN]; unsigned long t[US_WIN]; uint8_t why[US_WIN]; uint8_t idx; };
UsRing usRes, usMix;                           // last US_WIN pings per sensor (-1 = lost echo)
uint8_t usNext = 0;                            // 0 = reservoir next, 1 = mixing tank next
unsigned long usLastPingMs = 0;
uint16_t tankSeq = 0;                          // TANK frame number, 0..999 (ESP32 #1 counts the gaps)

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
int  soilRead(uint8_t pin);
int  soilColumnValue(uint8_t c);
void sendLight();
void sendNpkColumn(uint8_t c);
void sendStatus();
void sendBoot();
void cycleStep();
void enterFast();
void servoCommand(bool left, const char *seq);
void servoTick();
void servoReply(const char *status);
static uint8_t crc8Text(const char *s);
bool  readEnv(float &t, float &h);
float readDistanceCm(uint8_t trig, uint8_t echo);
void  usTick();
bool  usMedian(const UsRing &r, float &med, float &spread);
char  usStatus(const UsRing &r);
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
  sendBoot();                       // "I just restarted" -- before the 2 s sensor settle below
  Wire.begin();                     // I2C for BH1750 (A4/A5)
  // AVR Wire waits forever on a stuck bus by default, which turns one bad BH1750 lead into a watchdog
  // reset every 4 s and no data from any sensor. 25 ms lets the read fail and report -1 instead.
  Wire.setWireTimeout(25000, true);
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
  for (uint8_t i = 0; i < US_WIN; i++) {
    usRes.v[i] = -1.0f; usRes.t[i] = 0; usRes.why[i] = US_TIMEOUT;
    usMix.v[i] = -1.0f; usMix.t[i] = 0; usMix.why[i] = US_TIMEOUT;
  }
  usRes.idx = 0; usMix.idx = 0;

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // Pump-selector servo: deliberately NOT attached here, and no pulse is sent. This boot may be a WDT or
  // RESET_REQ reset in the middle of a run with the inverter live; moving the switch now would break load
  // current (the contact welding this servo exists to prevent). It only moves on a SERVO command. The
  // stored angle is just where the next ramp starts from (an erased EEPROM reads 0xFF -> assume LEFT).
  uint8_t ee = EEPROM.read(SERVO_EE_ADDR);
  svPos = (ee <= 180) ? ee : SERVO_LEFT_DEG;
  svTarget = svPos;
  pinMode(PIN_SERVO, INPUT);        // no drive on the signal line until the first command

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

  pollUart();                       // handle inbound ACTIVE/DAY/NIGHT/FAST/SERVO/RESET_REQ/CAL_*
  servoTick();                      // pump-selector settle/detach (and ramp, if SERVO_STEP_MS > 0)
  usTick();                         // web v.4: one ultrasonic ping (alternating tanks) when due
  calStreamTick();                  // §A.3: stream the selected sensor's raw value when active

  unsigned long now = millis();

  if (txState == TX_FAST) {
    // Lease: ESP32 #1 renews FAST every few seconds while a pump runs. Silence means it is gone or the
    // run is over and the "back to normal" command was lost -- either way, return on our own.
    if ((unsigned long)(now - lastFastRenewMs) >= FAST_LEASE_MS) {
      txState = fastReturn;
      forceTx = true;               // a full cycle right away, as on any interval switch
    } else if ((unsigned long)(now - lastFastTankMs) >= FAST_TANK_MS) {
      lastFastTankMs = now;
      sendTank();                   // the priority read: tank levels for the overflow watch
    }
  }

  if (cycStep == CY_IDLE && (forceTx || (unsigned long)(now - lastTxMs) >= getTxInterval())) {
    lastTxMs = now;
    forceTx  = false;
    cycStep  = CY_ENV;              // start a cycle; cycleStep() does one read per pass
  }
  cycleStep();

  if ((unsigned long)(now - lastHbMs) >= getHbInterval()) {
    lastHbMs = now;
    sendStatus();
    // The tank levels with every heartbeat (2026-11-17): idle, the full cycle is 20 min (day) / 1 h (night), so ESP32 #1
    // showed -1 for that long after any restart. TANK only reports the background pinger's medians: no blocking read.
    if (txState != TX_FAST) sendTank();
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

  // Optional checksum ",*XX" before <END>: CRC-8 of the payload in front of it, the same function and
  // layout this Nano uses on its own frames (endPkt). Present + wrong = the line was corrupted -> drop it.
  bool crcOk = false;
  char *star = strstr(p, ",*");
  if (star && star < e) {
    if (!isxdigit(star[2]) || !isxdigit(star[3])) return;
    char hx[3] = { star[2], star[3], 0 };
    uint8_t want = (uint8_t)strtoul(hx, NULL, 16);
    *star = '\0';                   // payload ends here (the line buffer is ours to cut)
    crcOk = (crc8Text(p) == want);
    if (!crcOk) return;
  }

  char cmd[16];
  uint8_t n = 0;
  while (*p && *p != ',' && *p != '<' && n < (uint8_t)(sizeof(cmd) - 1)) {
    cmd[n++] = *p++;
  }
  cmd[n] = '\0';

  if      (strcmp(cmd, "ACTIVE")    == 0) setTxState(TX_ACTIVE);
  else if (strcmp(cmd, "DAY")       == 0) setTxState(TX_DAY);
  else if (strcmp(cmd, "NIGHT")     == 0) setTxState(TX_NIGHT);
  else if (strcmp(cmd, "FAST")      == 0) enterFast();
  else if (strcmp(cmd, "SERVO")     == 0) {
    // The one command that moves hardware: it must carry a matching checksum. ESP32 #2 only asks for a
    // move while its inverter is off, so a move is always dead-switching.
    if (!crcOk) return;
    char pos[8], seq[8];
    uint8_t k = 0;
    if (*p == ',') p++;
    while (*p && *p != ',' && *p != '<' && k < (uint8_t)(sizeof(pos) - 1)) pos[k++] = *p++;
    pos[k] = '\0';
    k = 0;
    if (*p == ',') p++;
    while (*p && *p != ',' && *p != '<' && k < (uint8_t)(sizeof(seq) - 1)) seq[k++] = *p++;
    seq[k] = '\0';
    if      (!strcmp(pos, "LEFT"))  servoCommand(true, seq);
    else if (!strcmp(pos, "RIGHT")) servoCommand(false, seq);
  }
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
  else if (strcmp(cmd, "CAL_STOP")  == 0) {
    // A FLOW calibration accumulates pulses without consuming them, and computeFlowLpm() has been
    // returning 0 (not touching lastFlowCalcMs) for the whole run. Zero both here, or the first
    // normal TANK packet afterwards divides a full calibration's pulses by a stale interval and
    // reports one spurious flow value. Scoped to FLOW specifically, matching CAL_START's own
    // equivalent reset above -- this used to fire unconditionally for every sensor's CAL_STOP.
    if (!strcmp(calId, "FLOW")) {
      noInterrupts(); flowPulseCount = 0; interrupts();
      lastFlowCalcMs = millis();
    }
    calActive = false; calId[0] = '\0';
  }
  // else: ignore
}

void setTxState(TxState s) {
  txState = s;                      // ACTIVE/DAY/NIGHT also end FAST (ESP32 #1's "back to normal")
  forceTx = true;                   // send a fresh cycle right away on switch
}

// FAST (or a renewal of it). Entering drops the rest of a slow cycle's NPK reads, sends a TANK at once,
// and pushes the next ENV/SOIL/LIGHT a full FAST_SLOW_MS out.
void enterFast() {
  unsigned long now = millis();
  if (txState != TX_FAST) {
    fastReturn = txState;
    txState    = TX_FAST;
    lastTxMs   = now;
    lastFastTankMs = now - FAST_TANK_MS;   // due immediately
    if (cycStep == CY_NPK) cycStep = CY_IDLE;
  }
  lastFastRenewMs = now;
}

unsigned long getTxInterval() {
  switch (txState) {
    case TX_ACTIVE: return TX_INTERVAL_ACTIVE_MS;
    case TX_DAY:    return TX_INTERVAL_DAY_MS;
    case TX_FAST:   return FAST_SLOW_MS;
    default:        return TX_INTERVAL_NIGHT_MS;
  }
}

unsigned long getHbInterval() {
  switch (txState) {
    case TX_ACTIVE: return HB_INTERVAL_ACTIVE_MS;
    case TX_DAY:    return HB_INTERVAL_DAY_MS;
    case TX_FAST:   return FAST_HB_MS;
    default:        return HB_INTERVAL_NIGHT_MS;
  }
}

/* =============================================================================
 *  SENSOR CYCLE  --  one read per loop() pass
 * ========================================================================== */
void cycleStep() {
  switch (cycStep) {
    case CY_ENV:   sendEnv(); cycStep = CY_TANK; break;
    // In FAST, TANK has its own 300 ms cadence in loop(); don't add a second one here.
    case CY_TANK:  if (txState != TX_FAST) sendTank(); cycStep = CY_SOIL; break;
    case CY_SOIL:  sendSoil(); cycStep = CY_LIGHT; break;
    case CY_LIGHT:
      sendLight();
      cycNpkCol = 0;
      cycStep = (txState == TX_FAST) ? CY_IDLE : CY_NPK;   // FAST skips NPK entirely
      break;
    case CY_NPK:
      // An NPK read blocks up to NPK_RESPONSE_TIMEOUT_MS and its SoftwareSerial holds interrupts off,
      // which would stall the ramp and stretch the servo pulse. Wait until the servo is done.
      if (svState != SV_IDLE) break;
      while (cycNpkCol < NUM_COLUMNS && !COLUMN_ENABLED[cycNpkCol]) cycNpkCol++;
      if (cycNpkCol >= NUM_COLUMNS) { cycStep = CY_IDLE; break; }
      sendNpkColumn(cycNpkCol++);
      break;
    default: break;
  }
}

/* =============================================================================
 *  PUMP-SELECTOR SERVO  --  non-blocking ramp, commanded only
 * ========================================================================== */
// Accept a move. A repeat of the target already being ramped to is just acknowledged; a target equal
// to where the servo already is still re-drives it there (ESP32 #2 asks before EVERY inverter start, so
// a lever knocked off position or a reset in between is corrected rather than trusted).
void servoCommand(bool left, const char *seq) {
  uint8_t tgt = left ? SERVO_LEFT_DEG : SERVO_RIGHT_DEG;
  strlcpy(svSeq, seq, sizeof(svSeq));
  if (svState != SV_IDLE && tgt == svTarget) { servoReply("MOVING"); return; }
  svTarget = tgt;
  strlcpy(svName, left ? "LEFT" : "RIGHT", sizeof(svName));
  if (SERVO_STEP_MS == 0) {
    // No ramp (servoDelay 0): the very first pulse is the target, the same as sketch_nov15a. AVR Servo
    // keeps a value written before attach(), so no pulse at the old angle goes out first.
    pumpServo.write(tgt);
    if (!pumpServo.attached()) pumpServo.attach(PIN_SERVO);
    svPos    = tgt;
    svState  = SV_SETTLE;                           // the swing happens during the settle time
    svStepMs = millis();
    servoReply("MOVING");
    return;
  }
  if (!pumpServo.attached()) {
    // AVR Servo keeps the value written before attach(), so the first pulse is the believed position:
    // the ramp starts where the arm is instead of jumping.
    pumpServo.write(svPos);
    pumpServo.attach(PIN_SERVO);
  }
  svState  = SV_RAMP;
  svStepMs = millis();
  servoReply("MOVING");
}

// Save the believed angle now if one is waiting (EEPROM.update writes only a changed value).
void servoEeFlush() {
  if (!svEePending) return;
  EEPROM.update(SERVO_EE_ADDR, svPos);
  svEePending = false;
}

void servoTick() {
  unsigned long now = millis();
  if (svState == SV_IDLE) {
    // Still for SERVO_EE_SAVE_DELAY_MS since the last move: that was the final position of the sequence.
    if (svEePending && (unsigned long)(now - svEeIdleMs) >= SERVO_EE_SAVE_DELAY_MS) servoEeFlush();
    return;
  }
  if (svState == SV_RAMP) {
    if ((unsigned long)(now - svStepMs) < SERVO_STEP_MS) return;
    svStepMs = now;
    if      (svPos < svTarget) svPos++;
    else if (svPos > svTarget) svPos--;
    pumpServo.write(svPos);
    if (svPos == svTarget) svState = SV_SETTLE;     // svStepMs now marks the settle start
    return;
  }
  if ((unsigned long)(now - svStepMs) < SERVO_SETTLE_MS) return;   // SV_SETTLE
#if SERVO_DETACH_AFTER_MOVE
  {
    // Detach only between pulses. AVR Servo's detach() just stops the timer ISR from touching the pin, so
    // a detach during the 0.5-2.4 ms HIGH part of a pulse leaves D3 stuck HIGH -- which an SG90 does not
    // read as "hold here" (it goes limp or runs to an end). With interrupts off the ISR cannot raise the
    // pin between the check and the detach; if it is mid-pulse, the next loop pass tries again.
    uint8_t sreg = SREG;
    cli();
    bool low = (digitalRead(PIN_SERVO) == LOW);
    if (low) pumpServo.detach();
    SREG = sreg;
    if (!low) return;
  }
#endif
  svEePending = true; svEeIdleMs = now;            // saved once still for SERVO_EE_SAVE_DELAY_MS (final position only)
  svState = SV_IDLE;
  servoReply("OK");
}

void servoReply(const char *status) {
  startPkt("SERVO");
  pktAddRaw(svName);
  pktAddRaw(status);
  pktAddRaw(svSeq);
  endPkt();
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

// RAW packet (companion spec §A): reservoir + mixing distances in cm (ESP32 #1 maps to % and litres
// using the stored geometry); flow in L/min (ESP1 applies its scale).
// web v.4: TANK,<resCm>,<mixCm>,<flowLpm>,<resSpread>,<mixSpread> -- each cm is the MEDIAN of the background
// pinger's fresh pings, each spread the width of their middle three (both -1 with fewer than US_MIN_GOOD).
// Printed to 0.1 cm: ~0.1 L on these tanks, below the real resolution but not the noise ESP32 #2 filters.
void sendTank() {
  float res, mix, rs, ms;
  bool rOk = usMedian(usRes, res, rs);
  bool mOk = usMedian(usMix, mix, ms);
  float flow = computeFlowLpm();    // always valid (0 if no flow)
  char st[3] = { usStatus(usRes), usStatus(usMix), 0 };
  char sq[5];
  utoa(tankSeq, sq, 10);
  tankSeq = (uint16_t)((tankSeq + 1) % 1000);
  startPkt("TANK");
  pktAddFloatOrInvalid(res, 1, rOk);
  pktAddFloatOrInvalid(mix, 1, mOk);
  pktAddFloatOrInvalid(flow, 1, true);
  pktAddFloatOrInvalid(rs, 1, rOk);
  pktAddFloatOrInvalid(ms, 1, mOk);
  pktAddRaw(st);                    // pinger status: reservoir, mixing tank (K/E/R/F/S/X)
  pktAddRaw(sq);                    // frame number: ESP32 #1 drops a repeat and counts the gaps
  endPkt();
}

// Tag-based, variable-length: one <tag>,<probe1>,<probe2> TRIPLET per ENABLED column; disabled
// columns are omitted entirely (their pins are never read). Both raw capacitive probes are sent
// individually (NOT pre-averaged) so ESP32 #1 can log/diagnose each probe; ESP32 #1 averages the
// two for the control value and maps to % with the stored air/water endpoints (companion spec §A).
//
// One probe, read EXACTLY as Calibration/Nano/Soil reads it when the dry/wet endpoints are captured
// (2026-11-16): one throw-away read after switching the ADC channel, then the mean of SOIL_SAMPLES.
// Before this the firmware sent a single analogRead() taken straight after the previous probe's channel,
// which carries that channel's charge and noise -- so live values were biased against the calibration.
// 1 + 64 reads x ~0.11 ms = ~7 ms per probe, ~30 ms for columns A+B: well inside the WDT and the UART buffer.
int soilRead(uint8_t pin) {
  analogRead(pin);                                  // first read after a channel switch is unreliable
  long sum = 0;
  for (uint8_t i = 0; i < SOIL_SAMPLES; i++) sum += analogRead(pin);
  return (int)(sum / SOIL_SAMPLES);
}
// A column's value the way the calibration tool and ESP32 #1 work it out: the average of both probes, or
// the one good probe if the other is at a rail (disconnected or shorted); -1 if both are.
int soilColumnValue(uint8_t c) {
  int s0 = soilRead(SOIL_PINS[c][0]), s1 = soilRead(SOIL_PINS[c][1]);
  bool ok0 = (s0 > SOIL_RAIL_LO && s0 < SOIL_RAIL_HI), ok1 = (s1 > SOIL_RAIL_LO && s1 < SOIL_RAIL_HI);
  if (ok0 && ok1) return (s0 + s1) / 2;
  if (ok0) return s0;
  if (ok1) return s1;
  return -1;
}
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
    pktAddIntOrInvalid(soilRead(SOIL_PINS[c][0]), true);     // probe 1 raw ADC (as Calibration/Nano/Soil reads it)
    pktAddIntOrInvalid(soilRead(SOIL_PINS[c][1]), true);     // probe 2 raw ADC
  }
  endPkt();
}

void sendLight() {
  float lux = readLux();
  startPkt("LIGHT");
  pktAddFloatOrInvalid(lux, 0, lux >= 0.0f);
  endPkt();
}

// One enabled column per call (cycleStep walks the columns, one per loop pass).
void sendNpkColumn(uint8_t c) {
  char tag[2] = { 0, 0 };
  wdt_reset();                                      // longest read -- keep WDT happy
  float vals[NPK_REG_COUNT];
  uint8_t rc = readNpkColumn(NPK_ADDR[c], vals);
  bool ok = (rc == NPK_OK);
  startPkt("NPK");
  tag[0] = COLUMN_TAG[c];
  pktAddRaw(tag);
  for (uint8_t i = 0; i < NPK_REG_COUNT; i++) {
    pktAddFloatOrInvalid(vals[i], NPK_PREC[i], ok);
  }
  pktAddRaw(npkReasonStr(rc));                       // trailing reason token (OK on success, else why it failed)
  endPkt();
}

void sendStatus() {
  startPkt("STATUS");
  pktAddRaw("NANO");
  pktAddRaw("OK");
  endPkt();
}

// Sent once, first thing in setup() (2026-11-16): tells ESP32 #1 this Nano just restarted. During a battery
// switch that is a switch reset (ESP32 #1 refuses the inverter and counts a strike); otherwise ESP32 #1 just
// re-sends its interval command. Older ESP32 #1 firmware treats it as an ordinary STATUS heartbeat.
void sendBoot() {
  startPkt("STATUS");
  pktAddRaw("NANO");
  pktAddRaw("BOOT");
  endPkt();
}

/* =============================================================================
 *  SENSOR READS  (all bounded, no delay())
 * ========================================================================== */
bool readEnv(float &t, float &h) {
  h = dht.readHumidity();
  t = dht.readTemperature();
  // Exactly 0.0 C and 0.0 % is a failed read, not weather (2026-11-17 log: 67 of them from 12:17 on): a data line held
  // LOW gives all-zero bits, and zeros pass the DHT checksum. Sent as -1 (no reading) -- ESP32 #1 corrects the tank
  // distances for the air temperature, and 0 C would read every tank ~5 % wrong.
  if (!isnan(h) && !isnan(t) && h == 0.0f && t == 0.0f) return false;
  return !(isnan(h) || isnan(t));
}

// Raw one-way distance in cm (echo timing), -1.0 on timeout, -2.0 when the echo line is already HIGH before the
// trigger (a hung transducer -- or the tail of the previous ping on a slow clone, hence judged over several pings).
float readDistanceCm(uint8_t trig, uint8_t echo) {
  if (digitalRead(echo) == HIGH) return -2.0f;
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long dur = pulseIn(echo, HIGH, ULTRASONIC_TIMEOUT_US);
  if (dur == 0) return -1.0f;
  return (float)dur * SOUND_CM_PER_US;
}

// One ping per call at most, every US_PING_GAP_MS, alternating reservoir / mixing tank (web v.4). A ping
// outside US_MIN_CM..US_MAX_CM is stored as lost (-1), so it can never become part of a median.
void usTick() {
  unsigned long now = millis();
  if ((unsigned long)(now - usLastPingMs) < US_PING_GAP_MS) return;
  usLastPingMs = now;
  UsRing &r = usNext ? usMix : usRes;
  float d = usNext ? readDistanceCm(PIN_TRIG_MIX, PIN_ECHO_MIX) : readDistanceCm(PIN_TRIG_RES, PIN_ECHO_RES);
  uint8_t why = US_OK;
  if (d <= -1.5f)                              { why = US_STUCK;   d = -1.0f; }
  else if (d < 0.0f)                           { why = US_TIMEOUT; }
  else if (d < US_MIN_CM || d > US_MAX_CM)     { why = US_RANGE;   d = -1.0f; }
  r.v[r.idx] = d; r.t[r.idx] = millis(); r.why[r.idx] = why; r.idx = (uint8_t)((r.idx + 1) % US_WIN);
  usNext ^= 1;
}

// Median of the fresh, valid pings and the width of their middle three. False (and -1s) with too few.
bool usMedian(const UsRing &r, float &med, float &spread) {
  float s[US_WIN];
  uint8_t n = 0;
  unsigned long now = millis();
  for (uint8_t i = 0; i < US_WIN; i++) {
    if (r.v[i] < 0.0f || r.t[i] == 0 || (unsigned long)(now - r.t[i]) > US_STALE_MS) continue;
    float x = r.v[i]; int8_t j = (int8_t)n - 1;                  // insertion sort, n <= 5
    while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; }
    s[j + 1] = x; n++;
  }
  if (n < US_MIN_GOOD) { med = -1.0f; spread = -1.0f; return false; }
  med = (n & 1) ? s[n / 2] : 0.5f * (s[n / 2 - 1] + s[n / 2]);
  spread = (n >= 5) ? (s[3] - s[1]) : (n == 4) ? (s[2] - s[1]) : (s[2] - s[0]);
  return true;
}

// The pinger's status letter for one tank (TANK <st>): K = a median can be taken; otherwise why not, from the
// fresh pings -- S every one found the echo line stuck HIGH, F some good but too few, R out of range, E no echo,
// X no fresh pings at all (the pinger is not running).
char usStatus(const UsRing &r) {
  uint8_t nOk = 0, nTo = 0, nRg = 0, nSt = 0, nFresh = 0;
  unsigned long now = millis();
  for (uint8_t i = 0; i < US_WIN; i++) {
    if (r.t[i] == 0 || (unsigned long)(now - r.t[i]) > US_STALE_MS) continue;
    nFresh++;
    if (r.why[i] == US_OK && r.v[i] >= 0.0f) nOk++;
    else if (r.why[i] == US_STUCK) nSt++;
    else if (r.why[i] == US_RANGE) nRg++;
    else nTo++;
  }
  if (nOk >= US_MIN_GOOD) return 'K';
  if (nFresh == 0) return 'X';
  if (nSt == nFresh) return 'S';
  if (nOk > 0) return 'F';
  return (nRg > nTo) ? 'R' : 'E';
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
    if (!COLUMN_ENABLED[col]) return false;          // a disabled column's pins float -- don't read them
    if (calId[6] == '\0') {                         // "SOIL_A" -> column value (ESP1 calibrates the 2-probe avg)
      int v = soilColumnValue(col);                 // a railed probe is left out, as in the tool and ESP1
      if (v < 0) return false;                      // both probes disconnected: nothing to calibrate
      raw = (float)v;
      return true;
    }
    int ch = calId[6] - '1';                        // "SOIL_A1"/"A2" -> single channel 0/1
    if (ch < 0 || ch > 1) return false;
    raw = (float)soilRead(SOIL_PINS[col][ch]);      // raw 10-bit ADC, sampled like the calibration tool
    return true;
  }
  if (!strncmp(calId, "NPK_", 4)) {                 // raw N register for that column (display-only, §A.3)
    int col = calId[4] - 'A';
    if (col < 0 || col >= NUM_COLUMNS) return false;
    if (!COLUMN_ENABLED[col]) return false;          // no sensor physically present on a disabled column
    float vals[NPK_REG_COUNT];                      // readNpkColumn fills NPK_REG_COUNT values, as in sendNpk()
    if (readNpkColumn(NPK_ADDR[col], vals) != NPK_OK) return false;
    raw = vals[4];                                  // N (index 4) raw register
    return true;
  }
  // web v.4: the calibration stream uses the same median as TANK, so what the LCD locks is what a run reads.
  if (!strcmp(calId, "ULTRA_RES")) { float v, s; if (!usMedian(usRes, v, s)) return false; raw = v; return true; }
  if (!strcmp(calId, "ULTRA_MIX")) { float v, s; if (!usMedian(usMix, v, s)) return false; raw = v; return true; }
  if (!strcmp(calId, "DHT_T")) { float t = dht.readTemperature(); if (isnan(t)) return false; raw = t; return true; }
  if (!strcmp(calId, "DHT_H")) { float h = dht.readHumidity();    if (isnan(h)) return false; raw = h; return true; }
  if (!strcmp(calId, "LUX"))   { float v = readLux(); if (v < 0.0f) return false; raw = v; return true; }
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
  float raw = -1.0f;
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
const char *npkReasonStr(uint8_t rc) {
  switch (rc) {
    case NPK_OK:      return "OK";
    case NPK_TIMEOUT: return "TIMEOUT";
    case NPK_BADADDR: return "BADADDR";
    case NPK_BADLEN:  return "BADLEN";
    default:          return "BADCRC";
  }
}

// Fills out[] on a valid CRC-checked reply and returns NPK_OK; otherwise returns why not, out[] all -1.
uint8_t readNpkColumn(uint8_t addr, float out[7]) {
  for (uint8_t i = 0; i < NPK_REG_COUNT; i++) out[i] = -1.0f;

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
  // Sized FROM the register count, not a fixed 40: NPK_REG_COUNT is an editable [CONFIRM] constant,
  // and the fill loop below is bounded by `expected`. A fixed 40 would overrun this global on a
  // 2 KB-SRAM part the moment someone set NPK_REG_COUNT >= 18 for a different sensor.
  uint8_t resp[3 + 2 * NPK_REG_COUNT + 2];
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
  for (uint8_t i = 0; i < NPK_REG_COUNT; i++) {
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
// Bounded concatenation: plain strcat() has no way to know pkt[]'s capacity, so an oversized field
// could write past the 140-byte buffer and silently corrupt adjacent globals -- unlike endPkt()'s own
// strlen(pkt)<=128 check below, which only runs AFTER every strcat() that built the string has
// already executed, so it can't prevent an earlier overflow. Hand-computed worst-case packet sizes
// never come close to 140 given the value ranges this code can currently produce (defense-in-depth,
// not a live bug today) -- but it's cheap insurance against a future field/constant change reopening it.
static void safeCat(const char *s) {
  size_t used = strlen(pkt);
  if (used >= sizeof(pkt) - 1) return;
  strncat(pkt, s, sizeof(pkt) - used - 1);
}

void startPkt(const char *cmd) {
  strlcpy(pkt, "<START>,", sizeof(pkt));
  safeCat(cmd);
}

void pktAddRaw(const char *s) {
  safeCat(",");
  safeCat(s);
}

void pktAddIntOrInvalid(long v, bool valid) {
  safeCat(",");
  if (!valid) { safeCat("-1"); return; }
  char t[12];
  ltoa(v, t, 10);
  safeCat(t);
}

void pktAddFloatOrInvalid(float v, uint8_t prec, bool valid) {
  safeCat(",");
  if (!valid) { safeCat("-1"); return; }
  char t[16];
  dtostrf(v, 0, prec, t);
  safeCat(t);
}

// CRC-8 (poly 0x07, init 0) of the payload text, sent as ",*XX" before <END>. ESP1 drops a frame that fails it.
// ESP1 reads this link with SoftwareSerial, and a flipped bit can still look like a plausible number.
// Same function as crc8Text() in ESP1/include/pure_math.h.
static uint8_t crc8Text(const char *s) {
  uint8_t c = 0;
  while (*s) {
    c ^= (uint8_t)*s++;
    for (uint8_t i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

void endPkt() {
  static const char HEXD[] = "0123456789ABCDEF";
  uint8_t crc = crc8Text(pkt + 8);  // the payload: everything after "<START>,"
  char t[3] = { HEXD[crc >> 4], HEXD[crc & 0x0F], 0 };
  safeCat(",*");
  safeCat(t);
  safeCat(",<END>");
  if (strlen(pkt) <= 128) {
    Serial.println(pkt);            // \r\n terminator; framing markers bound the payload
  }
  // oversize packets are dropped rather than sent truncated
}

/* =============================================================================
 *  RESET  --  RESET_REQ handler: clean self-reset via short-timeout WDT
 * ========================================================================== */
void softwareReset() {
  servoEeFlush();                   // a final servo angle still waiting to be saved goes to EEPROM first
  wdt_enable(WDTO_15MS);
  while (1) { }                     // let the watchdog fire -> true hardware reset
}

/* =============================================================================
 *  ISR  --  reservoir flow pulse counter
 * ========================================================================== */
void flowISR() {
  flowPulseCount++;
}
