/* =============================================================================
 *  SMART IRRIGATION & FERTIGATION SYSTEM  --  ESP32 #2  ACTUATOR CONTROLLER
 *  Controller 3 of 3   (Nano  ->  ESP32 #1  ->  ESP32 #2)
 * -----------------------------------------------------------------------------
 *  ROLE (spec sec.10.2, 9.8.2 / CLAUDE.md): ACTUATOR EXECUTION SUBSYSTEM.
 *    - Receives a COMPLETE work order from ESP32 #1 and executes the entire
 *      valve/pump/mixer/dosing sequence autonomously (it owns HOW + timing).
 *    - Meters transferred/delivered WATER from the tank ULTRASONICS (litres, web v.4 "pure ultrasonic"
 *      2026-11-16 -- the ResMix/MixIrr flow meters are no longer used) and dosed NUTRIENTS from the
 *      small dosing flow sensors (pulses / K-factor).
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
 *         SERVO,<TRANSFER|BOOSTER>,<OK|FAIL>,<seq>  (ESP1 relays the Nano's pump-selector servo result
 *                 for our SERVO_REQ; only an OK for the CURRENT seq lets the inverter come on)
 *         LEVEL,<mixPct|-1>,<resPct|-1>,<mixL|-1>,<resL|-1>,*XX  (ESP1 relays the Nano's FAST ultrasonic
 *                 level ~3 Hz while an AC pump runs AND while we ask for it with LEVEL_FEED; -1 = no valid
 *                 reading. *XX = CRC-8 of the text before it -- REQUIRED: a garbled level is a metering
 *                 error now, so a line without a matching checksum is dropped. The % feeds the overflow
 *                 watch (level_guard.h); the litres meter every transfer/delivery (level_meter.h).)
 *         MIXCAP,<L> (work-order token) / SET_CAL,MIXCAP,<L>: litres the mixing tank holds at MIX_HIGH_PCT,
 *                 from ESP1's tank calibration -- the most a fill may plan to add.
 *         EXERCISE,<TRANSFER|BOOSTER|MIXER>  (preventive 5 s pump run, scheduled by
 *                 ESP1 sec.14.9.1 -> ACK,EXERCISE then DONE,EXERCISE,<pump>)
 *         CAL_START,<SENSOR_ID> / CAL_STOP,<SENSOR_ID>  (calibration raw stream, spec §A.3):
 *                 ESP2 streams CAL,<id>,<raw> ~1.6 Hz for pH/EC/ACS712/FLOW_*; idle-only.
 *         SET_CAL,<SENSOR_ID>,<VALUE>[,<VALUE2>]  (push a runtime cal const; ACK,SET_CAL,<id>)
 *                 PZEM_V,<low>,<high> = the AC supply window, kept in this board's NVS (2026-11-14)
 *                 PRATE,<transfer>,<booster> (L/min) / NRATE,<A>,<B>,<C> (mL/min) = the pump rates the operator
 *                 measured (LCD Calibration > Rate ...; 0 = not measured), kept in NVS too (2026-11-17, RATE_WEIGHT)
 *                 PSTART,<L> = the transfer's start litres (LCD Calibration > Transfer pump est), NVS "xS" (2026-11-17)
 *         PRIME_START,<LINE_ID>[,<COL>[,<run ms>]] / PRIME_STOP,<LINE_ID>  (purge a line: valve(s)+pump; with a run
 *                 time an AC line runs exactly that long from the inverter switch-on -> DONE,PRIME_TIMED,..., 2026-11-17;
 *                 under dead-man + generous cap, spec §A.4.2)
 *         TEST,ENTER / TEST,EXIT / TEST,HOLD,<bit> / TEST,RELEASE  (dead-man manual
 *                 relay test, sec.18.10.8): ESP1 streams HOLD while ENTER held; ESP2
 *                 keeps ONE relay ON only while HOLD arrives (timeout), with a 30s
 *                 hard cap. bit = PCF8575 OUT_* index 0..15, 16..19 = priming combos, 20/21 = servo
 *                 only (Transfer/Booster side, inverter stays off -> TEST,SERVO,<pump>,OK), 22 = mixer.
 *    OUT: ACK,<cmd> -> DONE,<cmd>[,WATER,<L>[,EST]];  DOSE,NUT_x,<target>,<measured>,COL_y
 *         (,EST = this water total includes a flow-blind timed stage: estimated, not metered)
 *         (per-nutrient result, sec.25.2.1); FLOW_FAIL/PWR_FAIL/EC_FAIL/PH_FAIL/
 *         SAFE_STOP/ERROR; DOSE_TIMEOUT,NUT_x (dosing pump past its ceiling -> ESP1 fault-hold);
 *         DEGRADED,<chan> (channel disabled, sequence continues ->
 *         ESP1 Major, no shutdown); BUSY,ACTIVE/TEST/HELD; INVALID,<reason>;
 *         PCF_FAIL,I2C (relay bus dead -> ESP1 Major) + PCF_OK (recovered);
 *         SENSOR_FAIL,EC / SENSOR_FAIL,PH (probe railed/disconnected -> ESP1 Major,
 *         distinct from EC_FAIL/PH_FAIL = out-of-window); STATUS,ESP2,OK; READY,ESP2;
 *         DONE,EXERCISE,<pump>; ACK,RESUME,<mode> (not DONE -- resumeWork() only ever sends ACK)
 *         SERVO_REQ,<TRANSFER|BOOSTER>,<seq>  (inverter is OFF and AC is dead: please move the pump
 *                 selector; ESP1 forwards it to the Nano and puts the Nano in its FAST state)
 *         INFO,INV_ON,<pump>  (the inverter relay was just switched ON for that pump: every timed AC run counts from
 *                 here -- the fill's run time, a timed prime, an exercise, ESP1's counters; 2026-11-17)
 *         PUMP,<TRANSFER|BOOSTER>,ON / PUMP,OFF  (an AC pump just got / lost inverter power -> ESP1
 *                 starts/stops the FAST level relay)
 *         SERVO_FAIL,<where>|<why>  (held fault: the selector could not be confirmed, the inverter never
 *                 came on -- or <why> = STUCK|<WRONG_WAY|NO_MOVE>|mix=..|res=..|t=..: the wrong pump kept
 *                 running after the re-seats -- a Critical hold with the choice to switch again or not);
 *                 LEVEL_BLIND,TRANSFER (held fault: filling with no level reading);
 *         SERVO_MISMATCH,<pump>|<WRONG_WAY|NO_MOVE>|mix=..|res=..|t=<ms>|try=<n>/<max>  (10 s after inverter-on
 *                 the levels say the other pump runs -- moved the wrong way, or a fill moved nothing with the
 *                 reservoir full: inverter off, servo swings twice, inverter on, judged again; alert only, no
 *                 halt -- the run continues unless it happens past <max>);
 *         INFO,PUMP_START_RETRY,<pump>|why=..|try=1/1 (a start step failed: retried once from the top);
         INFO,PUMP_RATE,<pump>|seen=..|cal=..|unit=<L/min|mL/min>|moved=..|s=.. (end of each stage: the rate the
                 levels / dose meter really saw, beside the measured one); DEGRADED,PUMP_WEAK|<pump>|levels=..|by_rate=..
                 (the levels saw under half of what the measured rate should have moved: alert only, the run goes on);
         INFO,FILL_RUN|need=..|run_s=..|q=..|start_l=..|base=..[|cut=..][|topup=n] (a transfer run, timed from the
                 inverter switch-on, 2026-11-17; q = the rate it is timed with, cut = the live level that ends it early,
                 2026-11-18 two-step fill); INFO,FILL_CUT|level=..|cut=..|t=..|run_s=..|by_time=.. (it ended there: the
                 pump was faster than q); INFO,FILL_CHECK|goal=..|measured=..|missing=..|settle_s=..|<OK|TOPUP>[|seen=..]
                 (after the ~1 min settle; seen = the rate the runs really gave, which times the top-up);
                 DEGRADED,FILL_SHORT|goal=..|measured=..|topups=n (still more than 3 L short after the
                 top-ups: the run goes on); DEGRADED,FILL_NOT_STEADY|level=..|band=.. (measured anyway);
                 DEGRADED,FILL_SETTLE_BLIND|goal=..|by_time=..[|rest=..] (no level after the settle: the run time's
                 figure, EST; rest = a two-step fill's remainder, run next on the saved rate);
         INFO,HELD,<work order>,<code>,<loc> (every 30 s while a fault is held: the hold reply again, in case it was
                 lost on the wire -- ESP1 acts on it only for its own acknowledged, not-yet-held work order);
         INFO,FILL_REPLAN|done=..|left=.. / INFO,DRAIN_REPLAN|done=..|left=.. (back on after a SERVO_MISMATCH re-seat:
                 the stage is measured on from the level now -- the wrong pump's water neither counts against it
                 nor goes uncounted);
 *         INFO,MIX_LOW_STOP,<MAIN|FLUSH>|mix=..%|delivered=..|left=.. (the 5 % / 10 s delivery low stop: done);
 *         RES_EMPTY,TRANSFER|..|by=<STALL|RES_EMPTY_WINDOW> (the reservoir ran dry: the fill ends, run goes on);
 *         LEVEL_HIGH,MIX|pct=..|metered=..|target=..  (overflow limit reached: fill stopped early, the
 *                 run carries on); DEGRADED,LEVEL_BLIND (draining with no level reading: the rest of the
 *                 drain runs on a timer, the water total is marked EST); LEVEL_FEED,<ON|OFF> (lease: please
 *                 keep relaying LEVEL even with no pump on -- we need settled baselines between stages);
 *         DEGRADED,LEVEL_DISAGREE|mix=..|res=.. (a fill's mixing-tank rise and reservoir drop disagree);
 *         TEST,LEVEL,<row>,<dMixL> (Testing Fill/Push combo released: how far the mixing tank moved)
 *
 *  P17 MASTER ACTUATOR-POWER CUTOFF & FAULT HOLD (sec.19.4.8): PCF8575 P17 (bit 15) is
 *  a master relay gating power to the whole P0-P16 actuator bank (fail-safe: loss of P17 =
 *  all actuators OFF). On a hard fault ESP2 drops P17, PAUSES the sequence, and STAYS ALIVE
 *  holding the mixing-tank volume (it does NOT power itself off -- that would forget the
 *  water and overfill on resume). Recovery is the user-gated RESUME,<mode> above. The mix-tank
 *  volume is MEASURED by the mixing-tank ultrasonic (web v.4) -- re-measured, settled, before
 *  every fill and every delivery -- and the last figure is persisted to NVS (survives a reboot).
 *
 *  NON-BLOCKING: no delay() in normal operation (only a 20 ms UART flush right before a
 *  self-reboot); every stage is a millis()-based step in a FSM. Task WDT petted each loop.
 *
 *  PLUMBING MODEL (documented assumption -- spec delegates HOW to ESP2 and does
 *  not step-specify the hydraulics; confirm against the real rig):
 *    Transfer pump (AC): reservoir -> mixing tank   (2026-11-17: TIMED -- its run time from the inverter switch-on,
 *                                                     from the pump line -- then measured on the mixing tank after a
 *                                                     ~1 min settle, with a top-up when more than 3 L short)
 *    Booster pump  (AC): mixing tank -> columns      (metered by the mixing-tank ultrasonic falling; runs until the
 *                                                     tank is under 5 % for 10 s)
 *    Both sit BELOW the tanks (flooded suction): always primed, water moves the moment the inverter is on.
 *    (web v.4: the GPIO26 / GPIO5 ResMix / MixIrr flow meters are not read at all any more.)
 *    Both are fed from the inverter (P2) through a changeover switch flipped by the Nano's D3 servo
 *    (LEFT = transfer, RIGHT = booster) -- they have NO relays of their own any more (P5/P11/P12 are spares,
 *    see the relay map). Order, always: inverter OFF -> AC dead -> servo -> confirmed -> inverter ON.
 *    Nutrient pumps (A = P1, B = P7, C = P13 since 2026-11-16): dose into mixing tank (metered by GPIO23/19/18)
 *    Mixer (P10): 12 V DC motor -- no inverter.
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
#include "flow_math.h"         // K guard + pulses->litres -- host-testable, see ESP2/test/
#include "level_guard.h"       // mixing-tank overflow / blind watch -- host-testable, see ESP2/test/
#include "servo_check.h"       // stuck pump-selector re-seat plan -- host-testable, see ESP2/test/
#include "source_low.h"        // reservoir nearly empty -> stop the transfer (web v.3 rule) -- host-testable
#include "level_meter.h"       // ultrasonic litres: filter, stage progress, stall, wrong-way -- host-testable
#include "../../shared/rig_constants.h"   // WATER_BUDGET_L, MIXING_TANK_MAX_L, MIX_HIGH_PCT: shared with ESP1
// A millis() stamp that is never 0 (0 = "not set") and never in the future (2026-11-17). The old `t | 1` made every even
// stamp 1 ms ahead of the clock, so `now - stamp` taken in the same millisecond wrapped to ~49 days and a timeout fired
// at once -- e.g. LEVEL_BLIND|NO_SETTLED_LEVEL the instant a forced run started.
static inline unsigned long nzMs(unsigned long t) { return t ? t : 1UL; }

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
// P0..P7 remapped 2026-11-14 to match the rewired relay board. Every user of these bits goes
// through the OUT_* names, EXCEPT ESP1's TEST_NAMES / pulseKForRow and pure_math.h's
// testRowUsesInverter, which hard-code the numbers -- change those together with this block.
// The pumps have had no relays since 2026-11-15 (P0/P1 were the transfer and booster pump relays).
//   WHY: with the AC pump load wired through these relay contacts, the AC induced voltage into the
//   relay-coil circuit, and that coupled onto the shared low-voltage wiring and disturbed EVERY flow
//   sensor input (false/missed pulses -> wrong litres on every metered stage). The pumps are now chosen
//   mechanically by the Nano's D3 servo changeover switch and powered only through the inverter relay
//   (P2) -- see the AC PUMP SELECTION block.
// Rewired again 2026-11-15 (user): P0 = 2nd-battery select (was P0), P5 = spare (was column C valve),
//   P6 = column C valve. 2026-11-16: nutrient A -> P1, nutrient B -> P7; P5/P11/P12 are spares (raw Testing hold only).
#define OUT_BATT_SEL   0    // P0  DPDT board-supply select (2nd battery; moved from P0 2026-11-15):
                            //     de-energized = main solar battery, energized = secondary battery
#define OUT_NUT_A      1    // P1  nutrient A pump (DC dosing). Moved off P11 on 2026-11-16 (user): the P11/P12 relays
                             //     never ran A/B -- P17 clicked, the pumps stayed idle. A/B swapped (P1/P7) later that day
#define OUT_INVERTER   2    // P2  inverter relay -- the ONLY AC switching relay; feeds the servo-selected pump
#define OUT_COL_A      3    // P3  solenoid valve, column A
#define OUT_COL_B      4    // P4  solenoid valve, column B
#define OUT_SPARE_P5   5    // P5  SPARE (was the column C valve until 2026-11-15)
#define OUT_COL_C      6    // P6  solenoid valve, column C (moved from P5 2026-11-15)
#define OUT_NUT_B      7    // P7  nutrient B pump (DC dosing). Moved off P12 on 2026-11-16 (user), same reason
#define OUT_MIXER      8    // P10 mixer motor   (12 V DC -- no inverter; ACS712-monitored)
#define OUT_SPARE_P11  9    // P11 SPARE (nutrient A until 2026-11-16 -- check that relay/wiring before reusing it)
#define OUT_SPARE_P12 10    // P12 SPARE (nutrient B until 2026-11-16 -- same)
#define OUT_NUT_C     11    // P13 nutrient C pump
#define OUT_NUT_D     12    // P14 nutrient D pump -- not dosed (only 3 nutrients); driven solely by Prime NUTD
#define OUT_PH_UP     13    // P15 pH up pump
#define OUT_PH_DN     14    // P16 pH down pump
#define OUT_MASTER_CUTOFF 15 // P17 master actuator-power cutoff (sec.19.4.8; was Nano RESET).
                             // Gates power to the whole P0-P16 bank. Fail-safe: de-energized = all OFF.
const uint8_t COL_VALVE_BIT[3] = { OUT_COL_A, OUT_COL_B, OUT_COL_C };

/* ---- Reservoir valve -- none fitted ---------------------------------------- *
 * The reservoir valve is no longer part of the plumbing: the transfer pump draws straight from
 * the reservoir. Set RES_VALVE_FITTED to 1 to put a valve back, on OUT_RES_VALVE (P5: P7 is nutrient B
 * since 2026-11-16); every fill path (SEQ_FILL, SEQ_FLUSH_FILL, Prime RESMIX, the Testing Fill combo)
 * then opens it again. At 0 nothing drives it except a direct Testing hold of the 'Spare P5' row.    */
#define RES_VALVE_FITTED 0
#define OUT_RES_VALVE    OUT_SPARE_P5

/* ---- Secondary battery: DPDT board-supply isolation (P0) ------------------------------------ *
 * While the inverter runs the AC motors, the DPDT on P0 moves ESP1, ESP2, the Nano, the sensors
 * and the relay coils off the main solar battery onto a secondary 4S Li-ion battery, breaking
 * BOTH + and - so there is no electrical link to the inverter's battery and the sensor readings
 * stay clean. Rule: inverter ON implies secondary. pcfOn(OUT_INVERTER) first selects the
 * secondary, waits BATT_SWITCH_SETTLE_MS, then starts the inverter (battPathTick); pcfOff(
 * OUT_INVERTER) stops it and returns to main BATT_RETURN_DELAY_MS later.
 * 2026-11-16 (user rule): an AC pump run asks for the secondary at its very START (acPumpHoldsSecondary),
 * before the servo moves: P0 on -> settle -> nothing rebooted (ESP2 survived, PCF8575 still holds, ESP1 and
 * the Nano confirm: BP_VERIFY_SEC) -> only then the servo and the inverter. The boards then STAY on the
 * secondary for the whole pump run -- stuck-selector re-seats and a straight restart included -- and return
 * to main BATT_RETURN_DELAY_MS after the run ends. A normal end keeps P17 up
 * until that return has happened (stopAllGraceful/cutoffDeenergizeGraceful); faults, STOP_ALL
 * and RESET_SELF still cut everything at once (fail-safe = main battery).
 * HARDWARE PREREQUISITE -- keep this 0 until it is fitted: a DPDT transfer leaves the boards
 * unpowered for ~10-20 ms, so a hold-up stage after the DPDT (and a coil supply for the P0
 * module/DPDT that rides through it) must exist, or every switch resets all three controllers
 * and the relay drops straight back to main. At 0, P0 is never driven automatically and the
 * inverter is switched exactly as before. ESP1 holds the matching flag (it owns the INA226 and
 * the low-secondary run block) -- set both together.                                          */
#define SECONDARY_BATT_ENABLED 1   // ON since 2026-11-16 (user: hold-up fitted). Must match ESP1.
/* Secondary battery as a BACKUP for a critically low main battery: READY, NOT ACTIVE. ESP1 decides
 * (backupTick) and sends BATTBK,ON|OFF; at 1 this holds P0 + P17 on with no inverter running, until
 * BATTBK,OFF or any hard stop (a fault or STOP_ALL still returns the boards to main). Needs
 * SECONDARY_BATT_ENABLED. Set this together with ESP1's flag of the same name.                  */
#define SECONDARY_BACKUP_ENABLED 0
#if SECONDARY_BACKUP_ENABLED && !SECONDARY_BATT_ENABLED
#error "SECONDARY_BACKUP_ENABLED needs SECONDARY_BATT_ENABLED"
#endif
const uint8_t NUT_PUMP_BIT[3]  = { OUT_NUT_A, OUT_NUT_B, OUT_NUT_C };

/* ---- Flow sensors (interrupt, stage-based; spec sec.19.4.5) --------------- *
 * Pins CORRECTED to the actual wiring (identified with Calibration/ESP2/FlowPinFinder).
 * web v.4 "pure ultrasonic" (2026-11-16): the two main-line meters -- ResMix (reservoir -> mixing, GPIO26)
 * and MixIrr (mixing -> column, GPIO5) -- are REMOVED from the firmware. Nothing attaches an interrupt to
 * GPIO26 / GPIO5 any more; the transfer and booster pumps are metered by the tank ultrasonics instead
 * (level_meter.h). Only the small dosing meters below are still read.                                    */
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
float K_NUT[3]   = { 450.0f, 450.0f, 450.0f };   // small dosing sensors differ [MEASURE]
// (K_RES_MIX / K_MIX_IRR are gone with their meters -- web v.4. A KMAIN token or a SET_CAL FLOW_RESMIX /
//  FLOW_MIXIRR from an older ESP1 is accepted and ignored.)
/* Flow K-factors are DIVISORS in litersSoFar(), and every metered stage decides when to stop its
 * pump from that result -- so a bad K is not a measurement error, it is an actuation fault:
 *   k == 0        -> pulses/0 = inf -> "target reached" on the first pulse. The stage completes
 *                    instantly, the run reports DONE, and no water ever moved.
 *   k far too big -> litres barely climb, so the pump runs to the 180 s hard cap. On SEQ_FILL that
 *                    is the mixing tank overflowing.
 * K_FLOW_MIN/MAX, kSane() and the pulses->litres division itself live in include/flow_math.h, so
 * `pio test -e native` can exercise the guard without an ESP32 -- see ESP2/test/. */

/* ---- Per-column water budget: WATER_BUDGET_L, in shared/rig_constants.h --- */
bool COLUMN_ENABLED[3] = { true, true, false };        // [CONFIRM] mirror ESP1/Nano

/* ---- Stage timings ------------------------------------------------------- */
const unsigned long VALVE_SWITCH_MS   = 1000;   // valve settle before pump (DC stages)
// Inverter ramp grace: since 2026-11-15 the selected pump runs the moment the inverter comes on, so this
// is no longer a wait before the pump -- it is how long after inverter-on the PZEM voltage/current checks
// hold off (a ramping inverter reads low).                                                     [MEASURE]
const unsigned long INVERTER_WARMUP_MS = 5000;
// Secondary-battery switch timing [TBD -- confirm at commissioning]: settle after the DPDT moves the
// boards to the secondary before the inverter starts, and the wait after the inverter stops before
// they go back to main.
const unsigned long BATT_SWITCH_SETTLE_MS = 500;
const unsigned long BATT_RETURN_DELAY_MS  = 1000;
// Before every switch ESP2 announces it (INFO,BATT,SWITCHING,<to>) and waits for ESP1's BATTSW,GO
// -- ESP1 flushes its SD log and journals the switch first -- or this long, whichever comes first,
// so a silent/absent ESP1 can never stall the run.
const unsigned long BATT_HANDSHAKE_MS     = 800;
// "Did anything reboot?" (rule, user 2026-11-16: P0 first -> wait -> nothing rebooted -> only then the
// inverter). Once ESP2 itself has survived BATT_SWITCH_SETTLE_MS it also checks that the PCF8575 still
// holds its outputs (a relay-board reset drops P0/P17 silently), then asks ESP1 (INFO,BATT,VERIFY, repeated
// every BATT_VERIFY_RESEND_MS). ESP1 answers BATTSW,OK only if it did not restart and the Nano answered
// after the switch without rebooting; otherwise BATTSW,FAIL,<why>. No answer in BATT_VERIFY_MS = fail.
// A failure never turns the inverter on: the pump start ends as BATT_SWITCH_FAIL and the boards return to main.
const unsigned long BATT_VERIFY_MS        = 5000;   // 3 s -> 5 s (2026-11-16): must outlast ESP1's BSW_VERIFY_NANO_MS (3 s)
const unsigned long BATT_VERIFY_RESEND_MS = 500;
// Longest an AC pump start waits for the boards to be confirmed on the secondary before giving up.
const unsigned long BATT_READY_MAX_MS     = BATT_HANDSHAKE_MS + BATT_SWITCH_SETTLE_MS + BATT_VERIFY_MS + 1000;
// Every AC warmup counts from the inverter REQUEST, so with the switch in the path the inverter
// itself starts BATT_SWITCH_SETTLE_MS late -- add it, so the inverter still gets its full warmup.
const unsigned long AC_WARMUP_MS = INVERTER_WARMUP_MS +
    (SECONDARY_BATT_ENABLED ? (BATT_HANDSHAKE_MS + BATT_SWITCH_SETTLE_MS) : 0);   // worst-case switch lead
// The part of AC_WARMUP_MS that is the battery switch (0 while SECONDARY_BATT_ENABLED is 0): the inverter
// is REQUESTED at pump start but only comes on after the switch, so AC no-flow/ramp windows add it.
const unsigned long AC_SWITCH_LEAD_MS = AC_WARMUP_MS - INVERTER_WARMUP_MS;

/* ---- AC pump selection: Nano D3 servo changeover (2026-11-15) ------------ *
 * Sequence per pump start (acPumpTick): inverter OFF -> INV_DECAY_MS -> PZEM says the AC is dead ->
 * SERVO_REQ (through ESP1) -> SERVO OK -> SERVO_POST_MS -> inverter ON. The pump runs from that moment. */
const unsigned long INV_DECAY_MS       = 2000;   // inverter off -> its output has collapsed      [MEASURE]
// PZEM dead-check before the servo may move: no reply (the PZEM-004T v3 is powered from the AC it measures)
// or a reading under AC_DEAD_V counts as dead. Set AC_DEAD_CHECK 0 ONLY if the PZEM is not on the
// inverter output (then the check would never pass); the decay wait alone then protects the switch.
#define AC_DEAD_CHECK 1                          //                                               [CONFIRM]
const float         AC_DEAD_V          = 50.0f;
const unsigned long AC_DEAD_WAIT_MS    = 15000;  // still live this long after the decay -> SERVO_FAIL AC_LIVE
                                                 // 5 s -> 15 s (2026-11-16): only more waiting -- the servo still never
                                                 // moves before the AC reads dead. An inverter output (or the PZEM on it)
                                                 // that lingers a few seconds no longer ends the start as a fault.
const unsigned long SERVO_REQ_RETRY_MS = 3000;   // re-send SERVO_REQ (ESP1 retries the Nano itself, every 1.5 s)
const unsigned long SERVO_TIMEOUT_MS   = 10000;  // no confirmed move -> SERVO_FAIL; the inverter never comes on
const unsigned long SERVO_POST_MS      = 300;    // after OK, before the inverter: lever fully seated
// Inverter confirmation (user, 2026-11-16): once the inverter relay (P2) is ON, wait up to INV_CONFIRM_MAX_MS
// for the PZEM to read voltage on the inverter output (>= INV_CONFIRM_V; the PZEM-004T is powered from that
// output, so no reply = no AC). The moment it does, the pump run counts as started and the inverter STAYS on.
// No voltage within the time -> the relay goes off again and the start ends as INVERTER_FAIL (NO_AC) -- not a
// servo fault, not a flow fault. The run clocks (stage cap, no-flow timeout, exercise length, the PZEM ramp grace
// and the cross-flow blank) only start once the voltage is there. Needs AC_DEAD_CHECK (the PZEM on that output).
const unsigned long INV_CONFIRM_MAX_MS = 6000;   //                                               [MEASURE]
const float         INV_CONFIRM_V      = AC_DEAD_V;   // any real reading: the same "live" line as the dead-check
// WHICH PUMP IS REALLY RUNNING (2026-11-16 user rule; replaces web v.4's continuous wrong-way watch). One verdict
// per inverter-on, SERVO_JUDGE_MS (10 s) after it, from the tank levels at inverter-on (level_meter.h pumpVerdict):
//   transfer expected: the mixing tank FALLS; or -- in a run's fill, with the reservoir clearly above its filter
//     zone -- neither tank moved at all = the BOOSTER is powered (it runs against its shut column valves);
//   booster expected: the reservoir FALLS (while the mixing tank does not), or the mixing tank RISES = the
//     TRANSFER pump is powered.
// The verdict must still stand SERVO_CONFIRM_MS later (one wild sample cannot do it). Then -- WITHOUT halting --
// the inverter goes off, the litres that moved are measured, the servo makes SERVO_SWINGS full swings (other side,
// back) and the inverter comes on again, judged afresh. After SERVO_RESEAT_MAX such recoveries in one start, the
// next verdict ends the start as SERVO_FAIL|STUCK: a run holds with a Critical alert and the operator's choice --
// switch the selector again (RESUME NORMAL: the restarted step pre-seats it once more) or not (CANCEL).
const unsigned long SERVO_JUDGE_MS     = 10000;  // user: "after 10 seconds of running"                 [CONFIRM]
const unsigned long SERVO_CONFIRM_MS   = 2000;   // the verdict must still stand this much later         [MEASURE]
const unsigned long SERVO_JUDGE_MAX_MS = 25000;  // no fresh levels to judge by within this: no verdict this time
const float         CROSS_LEVEL_MIN_L  = 0.6f;   // "moved the WRONG way": ~0.6 cm, twice the filtered noise [MEASURE]
// A pump START that fails on the way (a servo move not confirmed in time, the Nano refusing a move, the inverter
// giving no voltage) is tried once more from the top -- inverter off, decay, dead-check, pre-seat -- before it counts
// as failed (2026-11-16 hardening: one lost reply on the Nano link or a slow inverter must not halt a run).
// Testing's servo-only moves are not retried (the operator wants to see the failure).
const uint8_t       AP_START_RETRY_MAX = 1;
const uint8_t       SERVO_SWINGS       = 2;      // user: "initiate the servo twice" = two full swings
const uint8_t       SERVO_RESEAT_MAX   = 2;      // user: retry, retry once more, then hold
// 2026-11-16 (user, web v.3): EVERY AC pump start pre-seats the lever before the inverter comes on:
// SERVO_PRESEAT_SWINGS full swings (other side, back) ending on the target pump -- "3 times switching before turning
// on the inverter". The inverter is off and its AC confirmed dead for all of it. Testing's servo-only rows keep a
// single move. (web v.3 also greyed out its cross-FLOW stuck check, because the flow sensors read noise; web v.4's
// stuck check reads the tank LEVELS instead, so it stays on.)
const uint8_t       SERVO_PRESEAT_SWINGS = 3;
const uint8_t       AP_MOVES_MAX       = 8;      // the move queue: 3 swings = 6 moves

/* ---- Mixing-tank level guard (level_guard.h) ------------------------------ *
 * ESP1 relays the Nano's FAST ultrasonic level (LEVEL,<mix%>,<res%>,<mixL>,<resL>,<resInL>,<seq>, ~3 Hz) while an AC pump
 * runs and while we hold a LEVEL_FEED lease. Filling: LEVEL_HIGH_COUNT readings in a row at/over MIX_HIGH_PCT
 * (shared/rig_constants.h) -> stop the fill, keep the measured litres, alert (LEVEL_HIGH) and carry on.
 * The % comes from ESP1's tank geometry: measured 2026-11-15 as empty 42.9 cm / full 24.0 cm (both tanks), so
 * the 90 % stop is at ~25.9 cm sensor-to-water. ESP1 sends -1 only for no usable reading (lost echo, too
 * noisy, Nano silent). The level guard is no longer optional (web v.4): the ultrasonic IS the meter.      */
const uint8_t       LEVEL_HIGH_COUNT   = 3;
const unsigned long LEVEL_GRACE_MS     = 3000;   // after inverter-on, before a blind level is judged
const unsigned long LEVEL_BLIND_MS     = 10000;  // no ACCEPTED level sample this long while pumping = blind
                                                 // 3 s -> 10 s (2026-11-16, user): splashing / a lost echo is not a fault.
                                                 // ~1.3 L at 8 L/min; the 90 % stop and ESP1's 97 % backstop still apply.

/* ---- Ultrasonic water metering (web v.4 "pure ultrasonic", level_meter.h) ------------------------------- *
 * Every delivery (DELIVER / FLUSH_DELIVER) is metered on the MIXING TANK's fall, in litres. A transfer (FILL /
 * FLUSH_FILL) is timed by the transfer pump's RUN TIME and then measured on the mixing tank once the water has
 * settled (2026-11-17 user rule, see TRANSFER BY RUN TIME; it replaces the 2026-11-16 reservoir-drop estimate --
 * the reservoir is refilled while the transfer runs, so its drop is no measure). Rules, in the order a stage meets
 * them:
 *  1. BASELINE. Before the pump starts, wait for a SETTLED level: LVL_BASE_SAMPLES accepted samples, all taken
 *     at least LVL_SETTLE_MS after the last pump stopped and MIX_SETTLE_MS after the mixer stopped (the surface
 *     must be still). That reading replaces the bookkeeping (mixTankL), so a fill always tops up from what is
 *     REALLY in the tank. No settled level within LVL_BASE_MAX_MS: a FILL holds LEVEL_BLIND (it is never planned
 *     blind); a DELIVERY plans from the last known figure (DEGRADED,LEVEL_BASE_EST) and carries on -- it cannot
 *     overflow anything, and the level (or, blind, the timed drain) still ends it.
 *  2. FILL ends when the transfer pump's RUN TIME is reached (timed from the inverter switch-on), at the overflow
 *     limit (LEVEL_HIGH), or when the reservoir is found empty (RES_EMPTY, below); it is then settled, measured and
 *     topped up when more than FILL_TOPUP_MIN_L short. DELIVER ends MIX_LOW_STOP_MS after the mixing tank is under
 *     MIX_LOW_STOP_PCT (the low stop, below), or MIX_LOW_STOP_MS after it reached the booster's learned floor
 *     (mixDeadL + MIX_EMPTY_MARGIN_L) -- whichever comes first.
 *  3. NO FLOW: the level has not moved LVL_MOVED_MIN_L within LVL_NOFLOW_MS of the pump coming on -> FLOW_FAIL
 *     (hold; same recovery choices as before).
 *  4. STALL: water moved, then no further LVL_STALL_STEP_L of progress for LVL_STALL_MS:
 *       fill  -> the reservoir is empty (RES_EMPTY, carry on with what came in) -- unless the reservoir
 *                ultrasonic still shows water above its filter zone, then the pump lost its prime: FLOW_FAIL
 *                (hold);
 *       drain -> the booster is sucking air: the tank is empty for it (INFO,MIX_EMPTY, done). The level it
 *                stopped at is learned as mixDeadL (water the booster cannot reach), so the next fill adds it.
 *  5. BLIND (no accepted sample for LEVEL_BLIND_MS after LEVEL_GRACE_MS): a FILL holds (LEVEL_BLIND -- it
 *     could overflow); a DRAIN finishes on a timer for the litres that were left (DEGRADED,LEVEL_BLIND, total
 *     marked EST) -- it cannot overflow anything, and the timer keeps the booster from running dry long.
 *  6. When a stage ends, the tank figure is the measured level, not a sum of increments.
 * The noise floor is ~0.3 cm (~0.3 L here) after the Nano's and our medians, so every threshold below sits
 * well above it. All [MEASURE] at commissioning with the real tanks and pumps.                           */
const uint8_t       LVL_BASE_SAMPLES   = 5;
const unsigned long LVL_SETTLE_MS      = 3000;   // still water after a pump stops                     [MEASURE]
const unsigned long LVL_BASE_MAX_MS    = 60000;  // no settled baseline this long -> fill: LEVEL_BLIND hold (30 -> 60 s,
                                                 // 2026-11-17: room for a noisy surface to give its 5 samples);
                                                 // delivery: plan from the bookkeeping (20 -> 30 s, 2026-11-16)
const unsigned long MIX_SETTLE_MS      = 8000;   // the swirl after the mixer stops (12 V DC, SEQ_MIX)     [MEASURE]
const float         LVL_JUMP_L         = 1.5f;   // a sample this far from the median is held back (spike) [MEASURE]
const float         MIX_EMPTY_MARGIN_L = 0.15f;  // a drain is done this close to the pump's floor      [MEASURE]
const float         LVL_MOVED_MIN_L    = 0.4f;   // "water is really moving" (well above the noise)     [MEASURE]
const float         LVL_STALL_STEP_L   = 0.25f;  // progress is counted in steps of this                [MEASURE]
const unsigned long LVL_NOFLOW_MS      = 20000;  // pump on, level not moving this long -> FLOW_FAIL    [MEASURE]
const unsigned long LVL_STALL_MS       = 12000;  // moving, then no progress step this long -> stall    [MEASURE]
const float         MIX_DEAD_MAX_L     = 3.0f;   // a drain floor above this is not learned (blockage?)  [MEASURE]
const float         XCHECK_MIN_L       = 1.0f;   // fill cross-check: mixing rise vs reservoir drop ...
const float         XCHECK_FRAC        = 0.35f;  // ... may differ by max(1 L, 35 %) before DEGRADED     [MEASURE]
// The two tanks' fill figure (reservoir drop + mixing rise), LOGGED beside every fill (INFO,LEVEL_XCHECK) since the
// 2026-11-17 run-time rule: weight of the reservoir's drop while they agree. Never a measure any more.
const float         FILL_RES_WEIGHT    = 0.75f;
// RESERVOIR EMPTY (2026-11-16 user rule): the reservoir has a filter at the bottom and its ultrasonic stops at
// ~15 % -- it cannot see whether water is left below that. So on a transfer, SOURCE_LOW_COUNT readings in a row at
// or under RES_FILTER_PCT (18 %, shared/rig_constants.h) open a RES_EMPTY_WIN_MS window (the same number above it
// close it). If the MIXING TANK has not risen by RES_EMPTY_RISE_L across the window, the reservoir is empty: the
// transfer stops (RES_EMPTY -- refill it; the run carries on with what came in). If it rose, the next window opens.
// In that zone the reservoir's drop is no basis for the litres either: the rest of the fill is metered on the
// mixing tank alone (INFO,FILL_MIX_ONLY).
const unsigned long RES_EMPTY_WIN_MS   = 10000;  // user: the 10 s countdown                            [CONFIRM]
const float         RES_EMPTY_RISE_L   = 0.3f;   // less than this across a window = nothing came in   [MEASURE]
// DELIVERY LOW STOP (2026-11-16 user rule): SOURCE_LOW_COUNT mixing-tank readings in a row under MIX_LOW_STOP_PCT
// start a MIX_LOW_STOP_MS countdown (the same number at/over it cancel it). When it runs out the booster stops and
// the delivery is COMPLETE (INFO,MIX_LOW_STOP), even with that little mixture left in the tank. The level it stopped
// at is remembered (mixHeelL, NVS "mixHeel"), and the next fill plans its batch above it -- so the column still gets
// its full litres while the heel stays in the tank. (web v.3's 15 % rule is replaced by these two.)
const float         MIX_LOW_STOP_PCT   = 5.0f;   //                                                    [CONFIRM]
const unsigned long MIX_LOW_STOP_MS    = 10000;  // user 2026-11-17: 10 s more once under 5 % (was 5 s)       [CONFIRM]
const uint8_t       SOURCE_LOW_COUNT   = 3;      // ~1 s of readings at the Nano's FAST ~3 Hz
const unsigned long LEVEL_FEED_RENEW_MS = 4000;  // LEVEL_FEED,ON lease renewal (ESP1 expires it after 12 s)

// Dosing stages (DC nutrient pumps, still flow-metered): no pulse at all this long after the pump -> fault.
const unsigned long FLOW_TIMEOUT_MS   = 10000;
const unsigned long STAGE_MAX_MS      = 180000; // hard safety cap per metered stage (longer for a big batch, see stageCapMs)
const float         STAGE_CAP_MARGIN  = 1.5f;   // cap = this x the batch's expected time at the pump's nominal rate
const unsigned long MIX_DEFAULT_MS    = 30000;  // used if work order omits MIX
// Outer defensive cap on the MIX token, not a commissioning value: a generous multiple of the current
// [TBD] 30s placeholder above, so a corrupted/oversized token can't leave the mixer running with no
// backstop (SEQ_MIX has none of stagePoll's FLOW_TIMEOUT_MS/sgCapMs -- only this and overcurrent).
// Revisit once ESP1's MIXING_DURATION_MS is finalized at commissioning.
const unsigned long MIX_MAX_MS = 10 * MIX_DEFAULT_MS;  // 300000 (5 min)

/* ---- Level-independent (timed) recovery -- escape hatch when the ULTRASONIC metering failed -------- *
 * A held FLOW_FAIL (no level change) or LEVEL_BLIND can never clear via a level-metered resume (the stage
 * re-detects it and re-holds -> lockout). On RESUME,IRRIGATE / RESUME,RELEASE after such a fault, the
 * FILL/DELIVER stages run the pump on a TIMER instead, sized from the last known volume and pump rate. The
 * PZEM power check and (if the sensor came back) the overflow stop still run. Rates are [MEASURE].     */
// 2026-11-17, from the user's bucket measurements (1 L = 1028 g; time counted from the inverter SWITCH-ON, its start-up
// included): the transfer pump gives V = XFER_START_L + Q x t with Q = 11.74 L/min (line fit of 10 runs of 6.0-8.7 s;
// the web v.3 log's own 26 s fill agrees). The booster gave 11.5-13 L/min through an open column valve (no drip pipe);
// with the drip pipe it is LOWER and not measured yet -- BOOSTER_LPM is a cautious guess for it, used only to size how
// long a delivery may take to show progress, never to count litres.
const float TRANSFER_LPM = 11.74f;              // transfer pump, steady rate (the line's slope): DEFAULT until the LCD rate
const float BOOSTER_LPM  = 1.0f;                // booster WITH the drip pipe (a guess, see above): DEFAULT until the LCD rate
const float BOOSTER_OPEN_LPM = 13.2f;           // booster through an open valve: the fastest the mixing tank can drain
const float TIMED_MARGIN = 1.3f;                // run a bit longer than the estimate to fully move the volume
const unsigned long TIMED_STAGE_CAP_MS = 120000; // hard cap on any single timed stage (safety)

/* ---- TRANSFER BY RUN TIME, then SETTLE and MEASURE (2026-11-17 user rule) ------------------------------------- *
 * The transfer pump's run time is the basis of a fill; the mixing tank's ultrasonic checks it once the water has
 * settled. The reservoir is no basis at all: it is refilled while the transfer runs.
 *   1. Plan from a settled level (rule 1): need = the batch - what the booster can already reach, capped at the
 *      overflow limit.
 *   2. Run the transfer pump for xferRunS(need) seconds counted from the moment the inverter is SWITCHED ON (its
 *      start-up included, as it was measured): (need - XFER_START_L) / Q, at least XFER_MIN_RUN_S. Inverter off.
 *   3. Wait FILL_SETTLE_MS (~1 min) while watching the mixing tank: the reading must hold within FILL_STEADY_L for
 *      FILL_STEADY_WIN_MS.
 *   4. Measure. More than FILL_TOPUP_MIN_L short of the plan: run again for what is missing (at most FILL_TOPUP_MAX
 *      times), settle and measure again. FILL_TOPUP_MIN_L or less short: carry on with the run.
 *   (2026-11-18: the first run aims for FILL_FIRST_FRAC of the need, any run may end early on the live level, and a
 *    top-up is timed with the rate the runs really gave -- see TWO-STEP FILL below.)
 * Every run is still watched on the mixing tank's own rise: the overflow stop, no level change, stall, the empty-
 * reservoir window, the pump verdict and blind. A run that ended on the overflow limit or an empty reservoir is
 * measured but never topped up.                                                                                    */
const float         XFER_START_L       = 0.47f;  // the start's extra litres (the line's intercept)           [MEASURE]
// ... RUNTIME: ESP1 sends the re-measured start litres (LCD Calibration > Transfer pump est) as SET_CAL,PSTART,<L>,
// kept in NVS "xS"; the rate is SET_CAL,PRATE's transfer value (rateXferLpm). XFER_START_L is the default.
const float         PSTART_MAX_L       = 3.0f;   // = ESP1's PL_START_L_MAX
float               xferStartL         = XFER_START_L;
const float         XFER_MIN_RUN_S     = 6.0f;   // shortest run the measurements cover                        [MEASURE]
const unsigned long FILL_SETTLE_MS     = 60000;  // user: ~1 minute                                           [CONFIRM]
const unsigned long FILL_STEADY_WIN_MS = 10000;  // the reading holds this long ...
const float         FILL_STEADY_L      = 0.3f;   // ... within this (the level noise)                          [MEASURE]
const unsigned long FILL_SETTLE_MAX_MS = 150000; // never steady by then: measured anyway (DEGRADED,FILL_NOT_STEADY)
// 3.0 -> 0.8 L (2026-11-18, user approved the two-step fill below): any real shortfall is topped up. Not lower: the
// shortest run (XFER_MIN_RUN_S) gives ~1.6-2.3 L, so a top-up for less would land further off than it started.
const float         FILL_TOPUP_MIN_L   = 0.8f;   //                                                             [MEASURE]
const uint8_t       FILL_TOPUP_MAX     = 2;      // top-up runs per fill (the two-step fill's second run is the first)
const float         FILL_SHORT_ALERT_L = 3.0f;   // still this short with the top-ups used up -> DEGRADED,FILL_SHORT
/* TWO-STEP FILL (2026-11-18 user request: a 10 L forced run gave 14.9 L). The transfer pump's rate is not the same from
 * run to run -- 18 Nov: ~10.8 L/min at 15:33, ~17.8 L/min at 17:34, same inverter power both times -- so one run timed
 * with the saved rate can land far off, and an overfill can never be taken back. So:
 *   1. The first run aims for FILL_FIRST_FRAC of the litres needed (all of them when the rest would be under
 *      FILL_TOPUP_MIN_L), and ends early once the mixing tank's live level has risen FILL_FIRST_CUT_FRAC of them
 *      (a pump much faster than its saved rate).
 *   2. Settle and measure (as before). The rate that run really gave is worked out from the measurement
 *      (level_meter.h fillSeenLpm; runs of FILL_LEARN_MIN_S or more).
 *   3. The top-up is timed with that rate, for whatever is still missing, and ends early once the live level is
 *      within FILL_CUT_LAG_L of the goal. Settle and measure again; one more top-up if still short.
 * Every early end is a normal end (INFO,FILL_CUT) -- never a fault. It needs FILL_CUT_SAMPLES readings in a row at
 * the cut level and a believable pace (levelPaceOk), so one noisy echo cannot end a run. A level-blind recovery
 * (resumeTimed) keeps the single run: it could not measure the first.                                              */
const float         FILL_FIRST_FRAC     = 0.70f;
const float         FILL_FIRST_CUT_FRAC = 0.75f; // 0.80 left ~0.8 L for the top-up on a fast pump: at its threshold
                                                 // (simulated 18 Nov, 8-22 L/min: 0.75 kept every fill within ~1 L)
const float         FILL_CUT_LAG_L      = 0.8f;  // 17:34 run: settled 0.65 L above the level at pump-off, + the
                                                 // FILL_CUT_SAMPLES readings                                    [MEASURE]
const uint8_t       FILL_CUT_SAMPLES    = 3;     // ~1 s at the Nano's FAST rate
const float         FILL_LEARN_MIN_S    = 10.0f; // a run this long (from the switch-on) gives a usable rate
const float         FILL_LEARN_LO       = 0.4f;  // a seen rate outside 0.4-2.5x the saved one is not used
const float         FILL_LEARN_HI       = 2.5f;
// DELIVERY (2026-11-17 user rule): the booster empties the tank to under MIX_LOW_STOP_PCT and runs MIX_LOW_STOP_MS more.
// Its rate is no measure of the litres (the drip pipe slows it, by an amount not measured yet); it only sizes how long
// a delivery may take to show progress: the no-flow and stall windows and the stage cap allow a tank that drains
// DRAIN_SLOW_X times slower than BOOSTER_LPM (or the LCD rate).
const float         DRAIN_SLOW_X       = 0.5f;
// A FILL that loses its level mid-run (2026-11-17: forced runs held LEVEL_BLIND while the sensors read fine). The run time
// is the fill's measure, so it carries on blind -- but the overflow stop cannot see, so the run is cut to leave this share
// of the room below the overflow limit (from the settled base). The settle afterwards measures, and a top-up makes up
// the rest when the level is back. ESP1's own 97 % backstop judges every echo, noisy or not.
const float         FILL_BLIND_MARGIN  = 0.25f;

/* ---- Calibrated pump rates (2026-11-17 user request) ------------------------------------------------------ *
 * The operator measures each pump's constant flow by hand and types it on the LCD (Calibration > Rate Transfer /
 * Rate Booster in L/min, Rate Nut A-C in mL/min). ESP1 sends SET_CAL,PRATE,<transfer>,<booster> and
 * SET_CAL,NRATE,<a>,<b>,<c> (0 = not measured); kept in this board's NVS too, like the PZEM window.
 * The transfer and booster pumps sit BELOW the tanks (flooded suction): ALWAYS PRIMED. So there is no priming lag
 * in any of this -- water moves from the moment the inverter's AC is confirmed, and rate x run time counts from
 * that moment (re-seat pauses excluded). A measured rate is used for (2026-11-17, later the same day: the transfer's rate is the BASIS of a fill -- see
 * TRANSFER BY RUN TIME -- and the booster's only sizes a delivery's windows; neither is blended into the levels):
 *   - a dose's litres: rate x run time blended with its flow meter (level_meter.h rateBlend) -- weight RATE_WEIGHT
 *     while they agree, fading to nothing as they drift apart;
 *   - the stage cap and the timed (level-blind) recovery, instead of the defaults above;
 *   - the end of a stage whose level was lost: what was measured last plus rate x the time since;
 *   - a weak-pump alert (DEGRADED,PUMP_WEAK): after RATE_CHECK_MS the levels saw under RATE_WEAK_FRAC of the rate;
 *   - INFO,PUMP_RATE at the end of every stage: the rate the levels / meter actually saw (shown on the LCD screen).
 * Not measured (0) = the levels / meter alone, and the defaults above for caps and timers.                    */
const float RATE_WEIGHT        = 0.4f;    // pull towards rate x time while the two agree                  [MEASURE]
const float RATE_XCHECK_MIN_L  = 1.0f;    // water: agreement band max(1 L, 25 %)                           [MEASURE]
const float RATE_XCHECK_FRAC   = 0.25f;
const float RATE_DOSE_MIN_L    = 0.003f;  // dose: agreement band max(3 mL, 25 %)                           [MEASURE]
const float RATE_DOSE_FRAC     = 0.25f;
const unsigned long RATE_CHECK_MS = 25000;   // weak-pump check: past the 10 s pump verdict + its re-seat time
const float RATE_CHECK_MIN_L   = 1.5f;    // ... and once the rate says this much should have moved
const float RATE_WEAK_FRAC     = 0.5f;    // the levels saw under half of it -> DEGRADED,PUMP_WEAK           [MEASURE]
const unsigned long RATE_REPORT_MIN_MS = 15000;   // INFO,PUMP_RATE only for a run at least this long (water)
const float RATE_LPM_MIN  = 0.5f, RATE_LPM_MAX  = 60.0f;     // accepted transfer / booster rates (L/min)
const float RATE_MLPM_MIN = 1.0f, RATE_MLPM_MAX = 5000.0f;   // accepted nutrient-pump rates (mL/min), as ESP1

/* ---- PZEM AC validation (spec sec.23.1.2.1) ------------------------------ */
// AC pump current limits. RUNTIME since 2026-11-15 (were const 0.5 / 3.0): ESP1 owns them (LCD Settings >
// AC Current) and sends SET_CAL,PZEM_I,<min>,<max> in TENTHS of an amp; kept in this board's NVS too, like
// the voltage window. Limits + gap must match ESP1's acAmpWindowOk() (pure_math.h).
const int   PZEM_A10_DEFAULT_MIN = 5, PZEM_A10_DEFAULT_MAX = 30;   // 0.5 A / 3.0 A
const int   PZEM_A10_LIMIT_LO = 1, PZEM_A10_LIMIT_HI = 100;        // 0.1 A .. 10.0 A (PZEM-004T v3 10 A)
const int   PZEM_A10_MIN_GAP  = 2;                                  // min at least 0.2 A below max
float PZEM_MIN_CURRENT_A = PZEM_A10_DEFAULT_MIN / 10.0f;   // pump ON but below this = no draw (NO_CURRENT)
float PZEM_OVERCURRENT_A = PZEM_A10_DEFAULT_MAX / 10.0f;   // above this = OVERCURRENT
// AC supply window. RUNTIME since 2026-11-14 (was const 215/240): ESP1 owns it (LCD Settings > AC Voltage,
// website System tab) and sends SET_CAL,PZEM_V,<low>,<high> -- see applySetCal(). Kept in this board's own
// NVS too (pzemWindowLoad/Save), because ESP2 is powered off at idle and would otherwise wake on the
// compiled defaults. Limits + gap must match ESP1's acVoltWindowOk() (pure_math.h) and the website rules.
const float PZEM_V_DEFAULT_MIN = 215.0f, PZEM_V_DEFAULT_MAX = 240.0f;
const float PZEM_V_LIMIT_LO = 80.0f, PZEM_V_LIMIT_HI = 260.0f;   // PZEM-004T v3 measuring range
const float PZEM_V_MIN_GAP  = 5.0f;                               // low at least this far below high
float PZEM_V_MIN = PZEM_V_DEFAULT_MIN, PZEM_V_MAX = PZEM_V_DEFAULT_MAX;
const unsigned long PZEM_NO_CURRENT_MS = 30000;   // tolerate before PWR_FAIL
const unsigned long PZEM_POLL_MS       = 500;     // throttle Modbus reads (sec.19.2.1; non-blocking)

/* ---- ACS712 mixer current (spec sec.21.3 / 23.2.2.4) --------------------- */
const float ACS712_SENS_V_PER_A = 0.100f; // 20A module ~100 mV/A; 30A ~66mV/A [MEASURE]
float       ACS712_ZERO_V       = 1.65f;  // quiescent output at 0 A -- RUNTIME (SET_CAL ACS712)
// The mixer is a 12 V DC motor (2026-11-15), not AC: re-measure both limits on the real motor -- a 12 V
// mixer can draw more than 2.5 A, which would trip MIXER_OC on every healthy mix.              [MEASURE]
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
const unsigned long PUMP_EXERCISE_RUN_MS = 5000;             // default when ESP1 sends no length
const unsigned long PUMP_EX_RUN_MIN_MS = 1000, PUMP_EX_RUN_MAX_MS = 10000;  // clamp for an operator-set length

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
const unsigned long PRIME_HOLD_TIMEOUT_AC_MS = 1200; // ... for a transfer / booster line: see TEST_HOLD_TIMEOUT_AC_MS

/* ---- Mixing-tank volume tolerance ---------------------------------------- *
 * A FILL stage whose remaining target is below this is treated as "already full"
 * (no pump run) -- avoids a zero/negative-target pump cycle when resuming.       */
const float TANK_EPS_L = 0.05f;
// Mixing-tank safety ceiling: MIXING_TANK_MAX_L, in shared/rig_constants.h. Second line of defence on
// an operator-supplied WATER,<L>: ESP1 caps it too, but ESP2 must never accept a volume its own tank
// cannot hold just because the value arrived over the wire.

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
 * a HARD 30 s cap regardless of ESP1. The cap lives SOLELY here on ESP2.        */
bool     testMode      = false;
int      testHeldBit   = -1;          // relay currently driven by the dead-man (-1 = none)
unsigned long idleSinceMs = 0;        // when ESP2 became idle (0 = not idle now); drives IDLE_RESET_MS self-heal
bool     testCapped    = false;       // 30 s hard cap reached -> stay OFF until release
unsigned long testRelayOnMs = 0;      // when the current relay went ON (for the cap)
unsigned long testLastHoldMs = 0;     // last TEST,HOLD received (for the release timeout)
bool          testInvAcOk    = false; // bare Inverter row: voltage seen since its relay came on (2026-11-16)
unsigned long testInvRelayMs = 0;     // when that relay was first seen ON (0 = not yet; the battery switch may lead)
unsigned long testInvPzemMs  = 0;     // PZEM read pace for that check
const unsigned long TEST_HOLD_TIMEOUT_MS = 400;     // no HOLD within this -> release (fail-safe)
// ... for the inverter rows (Fill, Push, the servo moves) 1.2 s (2026-11-17 log). Starting a pump reads the PZEM while the
// inverter is still off, and an unpowered PZEM costs a ~108 ms Modbus timeout per read (two or three in a row), so this
// loop can miss ESP1's 150 ms keep-alives for longer than 400 ms and the dead-man dropped the run mid-start. A real
// release still ends it at once (TEST,RELEASE); the dead-man is only for a lost link.
const unsigned long TEST_HOLD_TIMEOUT_AC_MS = 1200;
// Re-arm gap (user, 2026-11-17): a new Testing selection within TEST_REARM_MS of the last TEST,RELEASE is ignored, and
// so is a new prime within PRIME_REARM_MS of the last PRIME_STOP -- rapid presses toggled the bank and P17 until the
// PCF8575 faulted. Only a real release starts it: a hold whose keep-alive was late picks up again at once.
// ESP1 keeps the same gap on its buttons (ACT_REARM_MS); this is the backstop (the website's hold-to-run too).
const unsigned long TEST_REARM_MS = 1500, PRIME_REARM_MS = 1500;
unsigned long testReleasedMs = 0;     // the last Testing selection ended (0 = none yet)
unsigned long primeStoppedMs = 0;     // the last prime ended
const unsigned long TEST_HARD_CAP_MS     = 30000;   // max continuous ON, single relay (sec.18.10.8.3)
const unsigned long TEST_COMBO_CAP_MS    = 60000;   // longer cap for valve+pump PRIMING combos (30 -> 60 s, 2026-11-16:
                                                    // the servo pre-seat, battery switch and inverter confirm come first)
// Fill Res>Mix and Push>Col (16..19, the inverter combos; user 2026-11-17): 1 minute of RUNNING, counted only while the
// inverter is switched on -- the servo pre-seat, the battery switch and a re-seat's swings do not use it up. The start
// itself is bounded separately: the inverter never came on within TEST_AC_START_MAX_MS -> off (as ESP1's web pulses).
const unsigned long TEST_AC_RUN_CAP_MS   = 60000;
const unsigned long TEST_AC_START_MAX_MS = 90000;
unsigned long testInvOnMs   = 0;      // inverter-on time this combo has used (TEST_AC_RUN_CAP_MS)
unsigned long testInvTickMs = 0;      // last tick that counted it

/* ---- Testing valve+pump priming combos (TEST,HOLD idx > 15) --------------- *
 * idx 16 = Fill (Reservoir -> Mixing: [reservoir valve if fitted] + transfer pump),
 * 17/18/19 = Push (Mixing -> Column A/B/C: column valve + booster pump), 20/21 = Servo>Transfer /
 * Servo>Booster (pump selector only, the inverter stays OFF), 22 = Mixer (DC relay only).
 * The pumps go through acPumpStart (servo first, then the inverter) -- see testComboOn().         */

/* ---- Calibration: raw stream + Prime (companion spec §A) ----------------- */
String   calId = "";                  // active CAL sensor id ("" = none)
unsigned long lastCalMs = 0;
// Prime dead-man (valve+pump pairing, §A.4.2): one line at a time, generous cap.
String   primeLine = "";              // active prime line ("" = none)
int      primeValveA = -1, primeValveB = -1, primePump = -1;   // primePump: DC relay bit, or AcPumpSel when primeIsAC
bool     primeIsAC = false, primeCapped = false;
unsigned long primeOnMs = 0, primeLastMs = 0;
// A prime that ENDED ON ITS OWN (45 s cap, selector not confirmed, overflow limit) stays off until the
// operator lets go. ESP1's keep-alive keeps arriving while ENTER is still held, and without this latch the
// very next PRIME_START re-armed the line at once -- defeating the cap, and retrying a failed selector
// (and alerting) every ~10 s. Cleared by PRIME_STOP, or once the keep-alives stop for 1 s.
bool     primeLatched = false;
unsigned long primeLatchKeepMs = 0;
// A TIMED prime (2026-11-17, LCD Calibration > Transfer / Booster pump est): PRIME_START,<line>,<col>,<run ms> runs an
// AC line for exactly that long from the inverter SWITCH-ON (the way the pumps were measured), then ends by itself:
// DONE,PRIME_TIMED,<line>,<ran ms>,<re-seat 0/1>,<ms from the switch-on to AC, -1 = no AC>. ESP1's keep-alive still
// feeds the dead-man meanwhile; the cap becomes the run time + 10 s.
unsigned long primeRunMs = 0, primeArmMs = 0, primeRelayMs = 0;
uint32_t      primeGen0 = 0;
bool          primeAcSeen = false;    // the inverter gave AC in this timed prime (its switch-on is then fixed)
const unsigned long PRIME_TIMED_MIN_MS = 1000, PRIME_TIMED_MAX_MS = 120000;

/* ---- Flow ISR (one active DOSING sensor at a time) ----------------------- */
volatile unsigned long flowPulses = 0;
// Edge glitch filter (2026-11-17). The web v.3 log of 17 Nov 08:01 shows a 5 L delivery "metered" within a second of
// the booster's inverter coming on: switching noise on the meter cable counted as pulses. No meter here can pulse
// faster than ~600 Hz (K 450-6000 pulses/L at dosing flows well under 1 L/min is 4-100 Hz), so edges closer than
// FLOW_MIN_EDGE_US are ringing, not water, and are not counted (flowGlitches keeps the tally for diagnostics).
const uint32_t FLOW_MIN_EDGE_US = 1500;
volatile uint32_t flowLastEdgeUs = 0;
volatile unsigned long flowGlitches = 0;
int activeFlowPin = -1;
void IRAM_ATTR flowISR() {
  uint32_t t = micros();
  if (t - flowLastEdgeUs < FLOW_MIN_EDGE_US) { flowGlitches++; return; }
  flowLastEdgeUs = t;
  flowPulses++;
}

/* ---- Ultrasonic levels relayed by ESP1 (web v.4, level_meter.h) ----------- *
 * lfMix / lfRes: median-filtered litres, mixing tank / reservoir. lvMixPct / lvResPct: the last raw % (for
 * logs). lvBad: LEVEL lines dropped for a bad/missing checksum or unparsable numbers (diagnostics).        */
LevelFilt lfMix, lfRes;
float     lvMixPct = -1.0f, lvResPct = -1.0f;
float     lvResInL = -1.0f;                  // ESP1's running count of litres poured INTO the reservoir (-1 = none)
uint32_t  lvBad = 0;
// web v.4 link hardening: ESP1 numbers its LEVEL lines (0..9999). A repeat is dropped; a gap = lines lost on the
// ESP1 -> ESP2 link (counted, shown in the fault text). -1 = none seen yet (or ESP1 restarted).
int       lvSeqLast = -1;
uint32_t  lvGap = 0;
bool      lvFeedWanted = false;              // our LEVEL_FEED lease is held
unsigned long lvFeedSentMs = 0;
// Settled-baseline gate (rule 1): when it started, and the accepted-sample count it counts from.
unsigned long lvGateMs = 0;
uint32_t      lvGateCount0 = 0;
float     mixCapL  = MIXING_TANK_MAX_L;      // litres at MIX_HIGH_PCT, from ESP1 (MIXCAP); planning ceiling
float     mixDeadL = 0.0f;                   // learned: water the booster cannot reach (persisted, "mixDead")
float     mixHeelL = 0.0f;                   // learned: what a low-stopped delivery leaves (persisted, "mixHeel")
unsigned long mixerOffMs = 0;                // when SEQ_MIX last switched the mixer off (baseline settle)

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
 * mixTankL is the liters currently in the mixing tank. web v.4: it is MEASURED by the
 * mixing-tank ultrasonic (settled baseline before every AC stage, measured level after it);
 * between measurements the dosing stages add their metered mL. It is persisted to NVS so a
 * reboot/power-down can never make ESP2 assume an empty tank (and a level-blind recovery
 * still has the last figure). faultHeld pauses the sequence after a hard fault until the
 * user-gated RESUME,<mode> arrives; the work order + step are kept (not cleared).      */
Preferences prefs;
float   mixTankL  = 0.0f;       // liters currently in the mixing tank (persisted)
bool    faultHeld = false;      // sequence paused after a fault, awaiting RESUME
String  heldReply = "";         // the held fault's reply "<code>,<loc>": re-sent as INFO,HELD while held (heartbeat)
unsigned long heldReplyMs = 0;
const unsigned long HELD_REASSERT_MS = 30000;

/* ---- Metered-stage state ------------------------------------------------- */
int   sgPump = -1, sgFlow = -1;   // sgPump: DC relay bit, or an AcPumpSel (1/2) when sgIsAC
uint32_t sgApGen = 0;             // apGen when this AC stage last saw its pump on (detects a finished re-seat)
float sgK = 1, sgTarget = 0;
int   sgTankSign = 0;          // +1 = this stage FILLS the mix tank, -1 = DRAINS it, 0 = neither (dose)
unsigned long sgCapMs = STAGE_MAX_MS;   // per-stage hard cap (dose stages override with the 2x ceiling)
bool  sgIsAC = false, sgPumpOn = false, sgSawFlow = false;
unsigned long sgT0 = 0;
bool  sgTimed = false;                   // flow-independent stage: complete on a timer, ignore the flow sensor
unsigned long sgTimedMs = 0;             // computed run time for a timed stage
float sgTimedLpm = 0;                    // nominal pump rate of a timed stage: what its credit is computed from
// Level-metered AC stage (web v.4): progress on the mixing tank from a settled baseline.
bool  sgLevel = false;                   // this stage is metered by the ultrasonic (FILL / DELIVER), not a meter
LevelStage ls = { 0, 0.0f, 0.0f, 0, false };
bool  sgBaseMeasured = false;            // baseL is a settled MEASUREMENT (not the bookkeeping of a timed recovery)
String    sgFailWhy = "";                // which stage rule fired (stagePoll), folded into the fault text
SourceLow srcLowRes;                     // fill: the reservoir in its filter zone (source_low.h; lowPct = the zone)
SourceLow srcLowMix;                     // drain: the mixing tank under MIX_LOW_STOP_PCT (the delivery low stop)
EmptyWin  resWin;                        // fill: the reservoir-empty window, judged on the mixing tank
unsigned long sgFlowT0 = 0;              // the pump last came on (first start, or back from a re-seat): no-flow clock
float sgMixBaseL = 0.0f;                 // mixing-tank level the stage is measured from (settled, or the bookkeeping)
float sgResBaseL = -1.0f;                // reservoir level when the pump came on (-1 = no reading): the fill's basis
float sgResInBaseL = -1.0f;              // ESP1's reservoir-inflow count then (-1 = not relayed)
bool  sgResLost = false;                 // this fill lost the reservoir reading: mixing tank only from then on
float sgLastMovedL = 0.0f;               // last measured progress (credited if the level is gone at the end)
unsigned long sgLastMovedMs = 0;         // when it was measured (0 = not yet)
// Calibrated rate x run time (see RATE_WEIGHT). sgRateLpm = this stage's pump rate in L/min (0 = not measured).
float sgRateLpm = 0.0f;
float sgRateBaseL = 0.0f;                // litres already counted when the rate clock (re)started
unsigned long sgRateT0 = 0;              // the rate clock: pump on (or back on after a re-seat, from `done`)
float sgLevelsL = 0.0f;                  // the levels' (or the dose meter's) own figure, before the rate blend
bool  sgRateChecked = false;             // the weak-pump check ran this stage
uint8_t woRateWarned = 0;                // DEGRADED,PUMP_WEAK sent this work order: bit0 transfer, bit1 booster, 2.. nuts
float rateXferLpm = 0.0f, rateBoostLpm = 0.0f;   // measured by the operator (L/min); 0 = not measured
float rateNutMlpm[3] = { 0.0f, 0.0f, 0.0f };     // nutrient pumps A-C (mL/min); 0 = not measured
static float xferLpm()  { return rateXferLpm  > 0.0f ? rateXferLpm  : TRANSFER_LPM; }
static float boostLpm() { return rateBoostLpm > 0.0f ? rateBoostLpm : BOOSTER_LPM; }
float sgTimedBaseL = 0.0f;               // a drain that went blind mid-way: litres measured before the timer took over
// Plausibility (2026-11-17, the web v.3 log of 17 Nov: a delivery "done" in under a second, nothing reached the column).
// A level stage may not finish faster than LEVEL_PACE_X times its pump's rate (beyond LEVEL_PACE_SLACK_L of level
// noise): such a "done" is a glitch, the stage runs on (DEGRADED,LEVEL_JUMP once). A dose meter's pulses are counted
// in METER_WIN_MS windows, and a window with more than ANY accepted pump could give (RATE_MLPM_MAX, not the measured
// rate: a real pulse is never dropped, so a dose can never overshoot because of this) is noise and is dropped
// (DEGRADED,FLOW_NOISE once per stage).
const float         LEVEL_PACE_X       = 3.0f;    //                                                    [MEASURE]
const float         LEVEL_PACE_SLACK_L = 0.6f;    // above the filtered level noise                     [MEASURE]
const unsigned long METER_WIN_MS       = 200;
bool          sgPaceWarned = false;
unsigned long sgMeterRaw = 0, sgMeterGood = 0, sgMeterDropped = 0, sgMeterWinMs = 0;
bool          sgNoiseWarned = false;
// A delivery that stalls with most of its water still in the mixing tank is a blocked line, not an empty tank: held
// as FLOW_FAIL (STALL|left=) instead of finishing "OK" with the column unwatered.
const float         DRAIN_STALL_DONE_FRAC = 0.5f; // delivered under this share of the plan ...
// ... and more than MIX_DEAD_MAX_L above the floor still in the tank -> hold.
unsigned long sgTimedT0 = 0;             // when the timed part began (shifted with sgT0 on a re-seat pause)
// Transfer by run time (2026-11-17, see XFER_START_L): this run's plan, timed from the inverter switch-on.
bool  sgXferTimed = false;               // this stage is a fill timed by the transfer pump's run time
float sgXferNeedL = 0.0f;                // litres this run should give from its switch-on
float sgXferDoneL = 0.0f;                // litres already in from before a re-seat (the mixing tank's rise)
float sgXferGoalL = 0.0f;                // the mixing-tank level the run aims for
unsigned long sgRelayOnMs = 0;           // the inverter switch-on this run is timed from (0 = not on yet)
unsigned long sgXferRunMs = 0;           // its run time from that switch-on
bool  sgXferStopped = false;             // the water stopped (no level change / stall / empty): credit the levels
// Two-step fill (2026-11-18, see FILL_FIRST_FRAC): the rate this run is timed with and the live level that ends it early.
float sgXferLpm = 0.0f;                  // L/min this run is timed with (0 = the saved rate, xferLpm())
float sgXferCutL = -1.0f;                // live mixing-tank level that ends the run early (<0 = none)
bool  sgXferCut = false;                 // the run ended there: credited with the level's rise, not the clock's
uint8_t  sgCutHits = 0;                  // readings in a row at the cut level
uint32_t sgCutSeenN = 0;                 // the last level sample judged (lfMix.count)
bool  sgXferReplanned = false;           // a selector re-seat replanned this run (FILL_REPLAN): its rate says nothing
unsigned long sgFloorSinceMs = 0;        // delivery: the level reached the booster's floor (its MIX_LOW_STOP_MS count)
bool  sgFillBlind = false;               // this fill lost its level and runs on its (capped) run time
String lastStageDiag = "";               // "moved=..|level=..|base=.." of the last level stage (fault text)
bool  mixRunning = false;                // SEQ_MIX: warmup finished, the mixer relay is on
bool  lastHoldFlow = false;              // the most recent hold lost the water metering (FLOW_FAIL / LEVEL_BLIND):
                                         // enables the timed recovery
bool  resumeTimed  = false;              // this RESUME should run FILL/DELIVER timed (flow-blind)
unsigned long acNoCurrentSince = 0;
// 2026-11-16 (user): a single PZEM reading outside the window no longer stops the pump. A trip needs this many
// readings IN A ROW (one per PZEM_POLL_MS = 0.5 s): ~1.5 s for the voltage, ~1 s for overcurrent.
const uint8_t PZEM_V_CONFIRM = 3, PZEM_OC_CONFIRM = 2;
uint8_t pzVLowRun = 0, pzVHighRun = 0, pzOcRun = 0;
unsigned long lastPzemMs = 0;     // PZEM read throttle (see PZEM_POLL_MS)

/* ---- Misc state ---------------------------------------------------------- */
unsigned long lastHeartbeat = 0;
// ESP1-commanded preventive pump exercise (sec.14.9.1). The schedule lives on ESP1
// (always-on, RTC); ESP2 just runs the named pump briefly when told to. Non-blocking:
// turn the pump on, note the off-time, and complete it in loop() (like the Nano-RST pulse).
int  exRunBit = -1;                        // DC relay being exercised (the mixer), -1 = none
int  exRunAc  = 0;                         // AC pump being exercised (AcPumpSel), 0 = none
// Nonzero = an exercise is pending/active, covering the servo/inverter lead-in AND the run (every busy
// guard tests `exRunOffAt != 0`). Since 2026-11-15 it is only that flag (the start time | 1): the run
// itself is timed by exRunEndMs, set once the pump is really on (an AC pump's lead-in varies with the
// servo round trip).
unsigned long exRunOffAt = 0;
unsigned long exRunMs = 0, exRunEndMs = 0; // run length, and when the running pump stops
String exRunName;
// Audit fix (2026-09-10): EXERCISE previously energized OUT_INVERTER and the pump bit in the same
// call, giving it none of the INVERTER_WARMUP_MS settle time every automated work-order stage gets
// via stageBegin()/stagePoll() -- exactly the protection that constant exists for, and EXERCISE is
// the one AC path that runs fully unattended (ESP1's own 2-day preventive schedule, no operator
// present). exRunOffAt now spans the WHOLE warmup+run window (so the busy-slot/mutual-exclusion
// guards and safetyMonitor()'s exerciseActive correctly cover warmup too); these two track whether
// the pump bit itself has been energized yet.
bool exRunPumpOn = false;
unsigned long exRunPumpAtMs = 0;
String rxLine;

/* ---- Forward declarations ------------------------------------------------ */
void wdtSetup();
void feedWDT();
void i2cBusRecover();
bool pcfWrite(uint16_t s);
extern uint8_t bswBootReport;          // battery-switch reset journal (power-path block)
extern uint8_t bswNv;
static void bswJournal(uint8_t v);
void doseJLoad();
void faultJLoad();
void bootReports();
void pcfOn(uint8_t bit);
void pcfOff(uint8_t bit);
void acPumpTick();
void acPumpStop();
bool acPumpOn();
bool acPumpFailed();
const char *apFailType();         // "SERVO_FAIL" or "BATT_SWITCH_FAIL" for the current AP_ERROR
void levelAuxTick();
void levelFeedTick();
int  levelBaseline(float &out);
float stageMovedL();
void exerciseTick();
extern unsigned long apRelayOnMs;   // the inverter's last switch-on (defined with the acPump state)
void exerciseEnd();
extern String apErr;
void tankApply(float deltaL);
void stopAll();
void stopKeepBank();
void cutoffEnergize();
void cutoffDeenergize();
void cutoffDeenergizeGraceful();
void stopAllGraceful();
void battPathTick();
void tankSave();
void tankLoad();
void pzemWindowLoad();
void pumpRateLoad();
void holdFault(const char *resp, const char *loc);
void resumeWork(const String &mode);
bool pcfPresent();
void pcfHealth();
void reply(const String &body);
void attachFlow(int pin);
void detachFlow();
float litersSoFar(float k);
int  flowPinForBit(int bit);   // TEST pulse: which meter watches a relay (-1 = none)
void pollEsp1();
void dispatch(const String &payload);
void startWorkOrder();
void runSequence();
void stageBegin(int pumpBit, bool isAC, int flowPin, float k, float targetL);
void levelStageBegin(int pump, int8_t dir, float targetL, float baseL, bool baseMeasured);
int  stagePoll();          // 1 done, 0 running, -1 no-flow fail
void stageEnd();
void goStep(SeqStep s);
void finishOk();
float endMeteredStage();
void creditRunningStage();
void safetyMonitor();
float readMixerCurrent();
int  adcAvg(uint8_t pin);
void  resetMixerCurrentRing();
bool  mixerCurrentOver(float rmsA);
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

  // 1 KB receive buffer (2026-11-16 hardening; the default is 256 B = ~0.27 s at 9600 baud): a loop pass stretched by
  // a PZEM Modbus timeout, an NVS write or a relay-bank retry must not drop the LEVEL lines the metering runs on.
  esp1Serial.setRxBufferSize(1024);
  esp1Serial.begin(ESP1_BAUD, SERIAL_8N1, ESP1_RX_PIN, ESP1_TX_PIN);

  wo.active = false;
  lastHeartbeat = millis();
  rxLine.reserve(260);   // headroom for the longest (fertigation) work order (~200B)

  tankLoad();                               // restore persisted mixing-tank volume (overfill guard)
  lfReset(lfMix); lfReset(lfRes);           // no level believed until ESP1 relays fresh ones
  pzemWindowLoad();                         // AC supply window ESP1 last sent (PZEM voltage check)
  pumpRateLoad();                           // the pump rates the operator measured (LCD Calibration > Rate ...)
  // Battery-switch journal (see the power-path block): set means the last boot ended in the middle
  // of a P0 switch, i.e. the hold-up did not carry the boards through it. Clear it now (so this
  // boot can never count twice) and keep reporting it until ESP1 acknowledges.
  bswNv = prefs.getUChar("bsw", 0);
  if (bswNv == 1 || bswNv == 2) { bswBootReport = bswNv; bswJournal(0); }
  else if (bswNv != 0) bswJournal(0);
  doseJLoad();                              // what the tank already holds, so a re-run cannot dose it twice
  faultJLoad();                             // a fault whose own P0 drop reset the boards before it was sent
  Serial.printf("Restored mixing-tank volume: %.2f L\n", mixTankL);

  // Announce READY as early as possible -- BEFORE the (non-critical) PZEM Modbus probe, which is only
  // used during a run and could otherwise slow/block the boot handshake if the meter is absent/slow.
  feedWDT();
  reply("READY,ESP2");                      // tell ESP32 #1 we are up & SAFE (sec.10.7.4)
  bootReports();

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
  battPathTick();              // secondary-battery switch: delayed inverter start / return to main
  acPumpTick();                // AC pump selection: inverter off -> AC dead -> servo -> inverter on
  levelAuxTick();              // overflow stop for the non-work-order transfer runs (exercise/prime/test)
  levelFeedTick();             // LEVEL_FEED lease: ESP1 keeps relaying the tank levels while we need them
  pcfHealth();                 // PCF8575 bus watchdog -> PCF_FAIL / PCF_OK
  heartbeat();
  idleResetTick();             // self-heal: reboot after prolonged idle (30 min)
  if (exRunOffAt != 0) exerciseTick();   // non-blocking preventive exercise (commanded by ESP1)
}

/* ---- Preventive exercise run (ESP1 sec.14.9.1), non-blocking --------------- *
 * AC pumps: acPumpStart() already runs the inverter-off/servo/inverter-on sequence; the run length
 * counts from the moment the pump is actually powered. The DC mixer gets the plain relay after the
 * usual DC settle (exRunPumpAtMs). Signed differences: these are millis() sums (49.7-day wrap).       */
void exerciseEnd() {
  if (exRunAc) acPumpStop();
  else if (exRunBit >= 0) pcfOff(exRunBit);
  cutoffDeenergizeGraceful();          // drop the master cutoff again (back to safe idle)
  exRunOffAt = 0; exRunBit = -1; exRunAc = 0; exRunPumpOn = false;
  reply("DONE,EXERCISE," + exRunName);
}
void exerciseTick() {
  unsigned long now = millis();
  if (!exRunPumpOn) {
    if (exRunAc) {
      if (acPumpFailed()) {                       // selector never confirmed: the pump never ran
        String m = String(apFailType()) + ",EXERCISE|" + apErr;
        exerciseEnd();
        reply(m);
        return;
      }
      if (!acPumpOn()) return;
    } else {
      if ((long)(now - exRunPumpAtMs) < 0) return;
      pcfOn(exRunBit);
    }
    exRunPumpOn = true;
    // An AC pump's run counts from the inverter switch-on (user, 2026-11-17), like every timed AC run.
    exRunEndMs = (exRunAc && apRelayOnMs && now - apRelayOnMs < 60000UL) ? apRelayOnMs + exRunMs : now + exRunMs;
    return;
  }
  if (exRunAc && acPumpFailed()) {                // stuck selector: re-seats used up mid-run
    String m = String(apFailType()) + ",EXERCISE|" + apErr;
    exerciseEnd();
    reply(m);
    return;
  }
  if ((long)(now - exRunEndMs) >= 0) exerciseEnd();
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

extern unsigned long apInvOffMs;   // AC pump decay clock (defined with the acPump state below)
bool pcfWrite(uint16_t s) {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(lowByte(s));
  Wire.write(highByte(s));
  bool ok = (Wire.endTransmission() == 0);          // 0 = ACKed; non-zero = bus/device fault
  // Only track the shadow when the write actually landed. A NAKed write leaves the hardware in its
  // OLD state, so recording the new one would desync shadow from reality and every later pcfOn/pcfOff
  // (which derive their mask FROM the shadow) would compute against a lie.
  if (ok) {
    // Inverter ON -> OFF by ANY path (bare Testing row, stopKeepBank, stopAll...): start the decay clock, so
    // acPumpStart never skips INV_DECAY_MS before the servo moves (relays are active-LOW: set bit = OFF).
    const uint16_t inv = (uint16_t)(1u << OUT_INVERTER);
    if (!(pcfShadow & inv) && (s & inv)) apInvOffMs = millis();
    pcfShadow = s;
  }
  if (!ok) { if (pcfFailCount < 250) pcfFailCount++; }   // consecutive-failure counter (debounce)
  else     { pcfFailCount = 0; }
  return ok;
}
void pcfRawOn(uint8_t bit)  { pcfWrite(pcfShadow & ~(1 << bit)); }   // clear bit = ON
void pcfRawOff(uint8_t bit) { pcfWrite(pcfShadow |  (1 << bit)); }   // set bit  = OFF
static inline bool pcfBitOn(uint8_t bit) { return !(pcfShadow & (1 << bit)); }

/* ---- Secondary-battery power path (see SECONDARY_BATT_ENABLED) ------------------------------ *
 * MAIN -> PRE_SEC -> SETTLE_SEC -> SEC -> RETURN_WAIT -> PRE_MAIN -> SETTLE_MAIN -> MAIN.
 * Reset-proofing, because a hold-up that cannot carry the boards through the DPDT transfer resets
 * every controller at exactly this point:
 *  1. Announce: INFO,BATT,SWITCHING,<to>. ESP1 flushes its SD log, journals the switch in its own
 *     NVS, pauses SD/radio/GSM bursts and answers BATTSW,GO (or BATT_HANDSHAKE_MS passes).
 *  2. Journal: NVS "bsw" = 1 (to secondary) / 2 (to main) and mixTankL saved, THEN P0 moves.
 *  3. Confirm: only after the boards survived BATT_SWITCH_SETTLE_MS -- journal cleared,
 *     INFO,BATT,SECONDARY|MAIN sent.
 * A boot that finds the journal set knows the switch reset it, and reports INFO,BATT,RESET,<to> until
 * ESP1 answers BATTSW,ACK. ESP1 counts those resets and latches the switch fault (no inverter use)
 * on the second within 24 h, so a failing hold-up can reset the rig at most twice -- never in a loop.
 * stopAll() (faults) still drops P0 at once, but journals it first so a reset there is counted too. */
// BP_VERIFY_SEC (2026-11-16): P0 is on and ESP2 survived; waiting for "nothing else rebooted" (ESP1's BATTSW,OK).
enum BattPhase { BP_MAIN, BP_PRE_SEC, BP_SETTLE_SEC, BP_VERIFY_SEC, BP_SEC, BP_RETURN_WAIT, BP_PRE_MAIN, BP_SETTLE_MAIN };
BattPhase     battPhase = BP_MAIN;
unsigned long battPhaseMs = 0;
bool          battGo = false;             // ESP1 answered BATTSW,GO for the announced switch
bool          battOk = false;             // ESP1 answered BATTSW,OK: no board rebooted in the switch
String        battFailWhy = "";           // the switch was not survived cleanly (consumed by acPumpTick)
unsigned long battVerifySentMs = 0;       // last INFO,BATT,VERIFY sent
bool          invWanted = false;          // inverter requested ON (may still be waiting on the switch)
bool          cutoffDropPending = false;  // a normal end is waiting for the return before dropping P17
bool          battBackupHold = false;     // ESP1 holds the boards on the secondary with no inverter (BATTBK,ON)
uint8_t       bswBootReport = 0;          // boot found the journal set: 1/2, reported until ESP1 ACKs
uint8_t       bswNv = 0xFF;               // last journal value written (skip redundant flash writes)
String        faultReport = "";           // boot found a fault journal: reported until ESP1 answers BATTSW,FACK
bool          faultJSet = false;          // a fault journal is on flash (skip redundant flash writes)

/* A hard fault drops P0 at once, so with no hold-up left the boards can die before the fault's own reply
 * is on the wire: ESP1 would then see only an unexplained reset and re-run into the same fault. Flash it
 * first. Survival clears it (battPathTick); a boot that finds it reports it (INFO,FAULT_LOST). */
static void faultJSave(const char *resp, uint8_t mask, const String &loc) {
#if SECONDARY_BATT_ENABLED
  if (battPhase == BP_MAIN || battPhase == BP_PRE_SEC || battPhase == BP_SETTLE_MAIN) return;   // P0 is off
  prefs.putString("faultj", String(resp) + "," + mask + "," + loc);
  faultJSet = true;
#else
  (void)resp; (void)mask; (void)loc;
#endif
}
__attribute__((unused)) static void faultJForget() {   // unused while SECONDARY_BATT_ENABLED is 0
  if (!faultJSet) return;
  prefs.remove("faultj"); faultJSet = false;
}
void faultJLoad() {
  if (!prefs.isKey("faultj")) return;
  faultReport = prefs.getString("faultj", "");
  prefs.remove("faultj");
}

static void bswJournal(uint8_t v) {
  if (v == bswNv) return;
  prefs.putUChar("bsw", v); bswNv = v;
}
static void battEnter(BattPhase ph) { battPhase = ph; battPhaseMs = millis(); }
__attribute__((unused)) static void battAnnounce(BattPhase ph, const char *to) {
  battEnter(ph); battGo = false;
  reply(String("INFO,BATT,SWITCHING,") + to);
}
// The only place P0 moves on the inverter path.
__attribute__((unused)) static void battToggle(bool toSecondary) {
  bswJournal(toSecondary ? 1 : 2);
  tankSave();                                  // tank bookkeeping on flash before the risky moment
  if (toSecondary) pcfRawOn(OUT_BATT_SEL); else pcfRawOff(OUT_BATT_SEL);
  battEnter(toSecondary ? BP_SETTLE_SEC : BP_SETTLE_MAIN);
}
// An announced switch that never happened (P0 did not move): tell ESP1 so it clears its journal.
static void battAbortAnnounce() { battEnter(BP_MAIN); reply("INFO,BATT,MAIN"); }

// Called by stopAll() just before its all-OFF write drops P0 unannounced (a fault).
static void battPathReset() {
  invWanted = false; cutoffDropPending = false;
  battBackupHold = false;                                      // a hard stop ends a backup hold; ESP1 must re-ask
  if (battPhase == BP_PRE_SEC) { battAbortAnnounce(); return; }
  if (battPhase != BP_MAIN && battPhase != BP_SETTLE_MAIN) {   // P0 is on: this write is a switch
    bswJournal(2); tankSave();
    battEnter(BP_SETTLE_MAIN);                                 // battPathTick confirms survival
  }
}
// A relay-board (PCF8575) reset leaves every output HIGH = OFF, P0 and P17 included, while the shadow still
// says ON. True when every output we hold ON still reads back LOW (quasi-bidirectional pins read their level).
__attribute__((unused)) static bool pcfStillHolds() {
  if (Wire.requestFrom((int)PCF_ADDR, 2) != 2) return false;
  uint16_t v = (uint16_t)Wire.read();
  v |= (uint16_t)Wire.read() << 8;
  return (v & (uint16_t)~pcfShadow) == 0;
}
bool acPumpHoldsSecondary();     // an AC pump run is in progress (defined with the acPump state below)
// The boards must be (or stay) on the secondary: the inverter is wanted, a backup hold, or an AC pump run.
__attribute__((unused)) static bool battNeedSec() { return invWanted || battBackupHold || acPumpHoldsSecondary(); }

// Inverter ON implies the secondary battery. Never blocks: battPathTick() switches first.
void inverterRequest(bool on) {
  if (on) {
    invWanted = true; cutoffDropPending = false;
    if      (battPhase == BP_SEC)         pcfRawOn(OUT_INVERTER);
    else if (battPhase == BP_RETURN_WAIT) { battEnter(BP_SEC); pcfRawOn(OUT_INVERTER); }
    else if (battPhase == BP_PRE_MAIN)    { battEnter(BP_SEC); reply("INFO,BATT,SECONDARY"); pcfRawOn(OUT_INVERTER); }
    // MAIN / PRE_SEC / SETTLE_SEC / SETTLE_MAIN: battPathTick() switches, then starts the inverter
  } else {
    invWanted = false;
    pcfRawOff(OUT_INVERTER);
    // An AC pump run keeps the boards on the secondary while its inverter is off (re-seat, restart).
    if (battPhase == BP_SEC && !battNeedSec()) battEnter(BP_RETURN_WAIT);
  }
}
void battPathTick() {
#if SECONDARY_BATT_ENABLED
  unsigned long dt = millis() - battPhaseMs;
  switch (battPhase) {
    case BP_MAIN:
      // A failed switch is consumed by acPumpTick in this same loop pass; never re-announce over it.
      if (battFailWhy.length()) { if (!acPumpHoldsSecondary()) battFailWhy = ""; break; }
      if (battNeedSec()) battAnnounce(BP_PRE_SEC, "SECONDARY");
      break;
    case BP_PRE_SEC:
      if (!battNeedSec()) { battAbortAnnounce(); break; }
      if (battGo || dt >= BATT_HANDSHAKE_MS) battToggle(true);
      break;
    case BP_SETTLE_SEC:
      if (dt >= BATT_SWITCH_SETTLE_MS) {                        // ESP2 itself survived the transfer
        bswJournal(0);
        if (!pcfStillHolds()) {
          // The relay board reset in the transfer: P0 (and P17) already dropped, so the boards are back on main.
          // Put the rest of the bank back as it was meant to be (P0 and the inverter off), and report it as a
          // switch reset: ESP1 counts the strike, exactly as for a board reset.
          pcfWrite(pcfShadow | (uint16_t)(1u << OUT_BATT_SEL) | (uint16_t)(1u << OUT_INVERTER));
          invWanted = false; battFailWhy = "PCF_RESET";
          bswBootReport = 1;                                    // INFO,BATT,RESET,SECONDARY until ESP1 ACKs
          reply("INFO,BATT,RESET,SECONDARY");
          battEnter(BP_MAIN); reply("INFO,BATT,MAIN");
          break;
        }
        battOk = false; battVerifySentMs = 0;
        battEnter(BP_VERIFY_SEC);                               // now: did ESP1 or the Nano reboot?
      }
      break;
    case BP_VERIFY_SEC:
      if (battOk) {                                             // nothing rebooted: the secondary is in use
        reply("INFO,BATT,SECONDARY");
        battEnter(BP_SEC);
        if (invWanted) pcfRawOn(OUT_INVERTER);
        else if (!battNeedSec()) battEnter(BP_RETURN_WAIT);
        break;
      }
      if (!battNeedSec()) {                                     // called off meanwhile: confirm where we are, go back
        reply("INFO,BATT,SECONDARY");
        battEnter(BP_RETURN_WAIT);
        break;
      }
      if (battFailWhy.length() || dt >= BATT_VERIFY_MS) {      // a board rebooted, or nobody confirmed
        if (!battFailWhy.length()) battFailWhy = "NO_CONFIRM";
        invWanted = false;                                      // the inverter does NOT come on
        reply("INFO,BATT,VERIFY_FAIL," + battFailWhy);
        reply("INFO,BATT,SECONDARY");                           // where the boards are; the return is announced next
        battEnter(BP_RETURN_WAIT);
        break;
      }
      if (!battVerifySentMs || millis() - battVerifySentMs >= BATT_VERIFY_RESEND_MS) {
        battVerifySentMs = nzMs(millis());
        reply("INFO,BATT,VERIFY");
      }
      break;
    case BP_SEC:
      if (!battNeedSec() && !pcfBitOn(OUT_INVERTER)) battEnter(BP_RETURN_WAIT);   // belt and braces
      break;
    case BP_RETURN_WAIT:
      // every normal end funnels through here; a new AC pump run (or a backup hold) keeps the secondary
      if (battNeedSec() && !battFailWhy.length()) { battEnter(BP_SEC); break; }
      if (dt >= BATT_RETURN_DELAY_MS) battAnnounce(BP_PRE_MAIN, "MAIN");
      break;
    case BP_PRE_MAIN:
      if (battGo || dt >= BATT_HANDSHAKE_MS) battToggle(false);
      break;
    case BP_SETTLE_MAIN:
      if (dt >= BATT_SWITCH_SETTLE_MS) {
        bswJournal(0);
        faultJForget();                                         // the fault's own reply got out: nothing was lost
        reply("INFO,BATT,MAIN");
        battEnter(BP_MAIN);
        if (cutoffDropPending) { cutoffDropPending = false; cutoffDeenergize(); }
      }
      break;
  }
#endif
}

void pcfOn(uint8_t bit) {
#if SECONDARY_BATT_ENABLED
  if (bit == OUT_INVERTER) { inverterRequest(true); return; }
#endif
  pcfRawOn(bit);
}
void pcfOff(uint8_t bit) {
#if SECONDARY_BATT_ENABLED
  if (bit == OUT_INVERTER) { inverterRequest(false); return; }
#endif
  pcfRawOff(bit);
}
// Reservoir valve, only if one is fitted again (RES_VALVE_FITTED); otherwise OUT_RES_VALVE (P5) stays a spare.
void resValveSet(bool on) {
#if RES_VALVE_FITTED
  if (on) pcfOn(OUT_RES_VALVE); else pcfOff(OUT_RES_VALVE);
#else
  (void)on;
#endif
}

/* =============================================================================
 *  AC PUMP SELECTION  --  Nano D3 servo changeover + inverter (2026-11-15)
 * -----------------------------------------------------------------------------
 *  The transfer and booster pumps have no relays any more (P1 is a spare, P0 the battery select -- AC on
 *  induced into the coil circuit and upset every flow sensor). The inverter's output goes through a
 *  changeover switch that a servo on the Nano flips: LEFT = transfer, RIGHT = booster. So running a
 *  pump means:
 *      inverter OFF -> INV_DECAY_MS -> PZEM says the AC is dead -> SERVO_REQ (ESP1 -> Nano)
 *      -> SERVO OK for this seq -> SERVO_POST_MS -> inverter ON (the pump runs) -> PUMP,<p>,ON
 *  The switch therefore never makes or breaks load current, so its contacts cannot weld. This block is
 *  the ONLY place the inverter is switched on for a pump, and the servo is asked again before EVERY
 *  start -- a "should already be there" is never trusted (Nano reset, lever knocked, lost reply).
 *  Non-blocking: acPumpTick() runs every loop pass. A failure leaves the inverter OFF and parks in
 *  AP_ERROR with the reason in apErr; the owner (stage / exercise / prime / test) turns that into its
 *  own fault and calls acPumpStop().
 *
 *  WRONG PUMP (2026-11-16 user rule, from the tank LEVELS -- see SERVO_JUDGE_MS): 10 s after the inverter comes
 *  on, the levels say which pump really runs (fill: the mixing tank falling, or nothing moving while the
 *  reservoir holds water = the booster; delivery: the reservoir falling or the mixing tank rising = the
 *  transfer). Confirmed 2 s later, then WITHOUT halting: inverter off, the moved litres measured, SERVO_MISMATCH
 *  reported, and after the usual decay + dead-check the servo makes SERVO_SWINGS full swings (other side, back)
 *  before the inverter comes on again and the next verdict is taken. SERVO_RESEAT_MAX recoveries per start; the
 *  next wrong verdict -> AP_ERROR "STUCK" (a run holds SERVO_FAIL: Critical, with the choice to switch again or
 *  not). While a re-seat runs, acPumpOn() is false and owners simply wait (apGen tells them when it is back on).
 *  PUMP,OFF is NOT sent for a re-seat, so ESP1 keeps the Nano in FAST throughout.
 *  A START that fails on the way (servo not confirmed, Nano refusal, no inverter voltage) is retried once from
 *  the top before it counts (AP_START_RETRY_MAX).
 * ========================================================================== */
enum AcPumpSel   { PUMP_NONE = 0, PUMP_TRANSFER = 1, PUMP_BOOSTER = 2 };
// AP_SELECTED: a Testing "servo only" move (acPumpSelectOnly) finished -- the selector sits on the pump,
// and the inverter stays OFF.
// AP_INV_CONFIRM (2026-11-16): the inverter relay is ON, waiting (<= INV_CONFIRM_MAX_MS) for voltage on its output.
enum AcPumpPhase { AP_IDLE, AP_INV_OFF, AP_SERVO_WAIT, AP_SETTLE, AP_INV_CONFIRM, AP_ON, AP_ERROR, AP_SELECTED };
AcPumpPhase apPhase = AP_IDLE;
int           apPump  = PUMP_NONE;
unsigned long apPhaseMs = 0;       // start of the current phase
unsigned long apReqMs   = 0;       // last SERVO_REQ sent
unsigned long apOnMs    = 0;       // inverter requested ON (pump start)
unsigned long apPzemMs  = 0;       // dead-check read throttle
bool          apReqSent = false;
uint16_t      apSeq     = 0;       // request id, echoed back by the Nano through ESP1
bool          apServoOk = false, apServoFail = false;
bool          apSelectOnly = false;   // Testing: move the servo and stop there -- never turn the inverter on
String        apErr     = "";      // why AP_ERROR (folded into the SERVO_FAIL text)
unsigned long apInvOffMs = 0;      // when a pump run last switched the inverter off (decay clock)
unsigned long apRelayOnMs = 0;     // when a pump run last switched the inverter relay ON (2026-11-17: a fill is timed from it)
// Servo move queue: a normal start is one move (the target); a re-seat is servoReseatPlan()'s swings.
int           apMoves[AP_MOVES_MAX] = { 0 };
uint8_t       apStartRetries = 0;     // AP_START_RETRY_MAX: failed start attempts retried in this start
uint8_t       apMoveN = 0, apMoveIdx = 0;
int           apReqPump = PUMP_NONE;  // the side being requested right now (== apPump except mid-swing)
uint8_t       apReseats = 0;          // wrong-way detections in this start
uint32_t      apGen = 0;              // +1 each time the inverter comes on (owners spot a finished re-seat)
unsigned long apPausedMs = 0;         // when a re-seat stopped the pump (owners shift their clocks by the pause)
// Which-pump verdict (level_meter.h pumpVerdict): reference levels at inverter-on (or the first fresh ones after
// it), one verdict SERVO_JUDGE_MS later, confirmed SERVO_CONFIRM_MS after that.
bool          apXSet   = false;       // the reference levels are taken
unsigned long apXRefMs = 0;           // when
float         apXMix0 = -1.0f;        // mixing-tank litres then
float         apXRes0 = -1.0f;        // reservoir litres then (-1 = no reading)
bool          apJudged = false;       // this inverter-on period has had its verdict (or gave up on one)
unsigned long apVerdictMs = 0;        // when a wrong-pump verdict was first seen (0 = none pending)
float         apOnMixL     = -1.0f;   // mixing-tank level at inverter-on (Testing combos report the change)

// Overflow / blind watch on the mixing tank (level_guard.h). Started when the inverter comes on for a
// pump, stopped with it. lgHighHit latches an LG_HIGH verdict from a LEVEL sample until its owner acts.
LevelGuard lg = { false, false, 0, 0, 0, -1.0f, false };
bool       lgHighHit = false;

static const char *acPumpName(int p) { return p == PUMP_TRANSFER ? "TRANSFER" : p == PUMP_BOOSTER ? "BOOSTER" : "NONE"; }

static void crossWatchStop() { apXSet = false; apJudged = false; apVerdictMs = 0; }

// Forget the pump run without touching the relays (stopAll() and friends write the whole bank anyway).
static void acPumpForget() {
  bool was = (apPhase != AP_IDLE);
  if (apPhase == AP_ON || apPhase == AP_INV_CONFIRM) apInvOffMs = millis();   // the inverter goes off now (here or in the caller's write)
  crossWatchStop();
  apPhase = AP_IDLE; apPump = PUMP_NONE; apSelectOnly = false;
  lgStop(lg); lgHighHit = false;
  if (was) reply("PUMP,OFF");               // ESP1: stop the level relay, Nano back to its normal pace
}
// Normal end of a pump run: inverter off, state cleared.
void acPumpStop() {
  if (apPhase != AP_IDLE) pcfOff(OUT_INVERTER);
  acPumpForget();
}
bool acPumpOn()     { return apPhase == AP_ON; }
bool acPumpFailed() { return apPhase == AP_ERROR; }
// From the start of an AC pump run to its end the boards belong on the secondary (user rule, 2026-11-16) --
// including a re-seat or a straight restart, so P0 is not toggled back and forth inside one run. A servo-only
// Testing move never powers anything, so it stays on main.
bool acPumpHoldsSecondary() {
  return !apSelectOnly && (apPhase == AP_INV_OFF || apPhase == AP_SERVO_WAIT || apPhase == AP_SETTLE ||
                           apPhase == AP_INV_CONFIRM || apPhase == AP_ON);
}
// How an AP_ERROR is reported: a battery switch that did not come through, or an inverter that gave no voltage,
// is not a servo fault.
const char *apFailType() {
  if (apErr.startsWith("BATT_SWITCH")) return "BATT_SWITCH_FAIL";
  if (apErr.startsWith("INVERTER"))    return "INVERTER_FAIL";
  return "SERVO_FAIL";
}
// The inverter has had its warm-up since the pump start, so PZEM voltage/current checks are meaningful.
static bool acPumpRamped() { return apPhase == AP_ON && millis() - apOnMs >= AC_WARMUP_MS; }

// Begin (or re-begin) a pump run. The inverter goes OFF first, always, and the servo may only move once
// INV_DECAY_MS has passed since it ACTUALLY went off -- an irrigation FILL turns it off a moment before its
// DELIVER starts here, so "it is already off" alone says nothing about its output having collapsed. Only an
// inverter that has been off for longer than the decay skips the wait. The PZEM dead-check runs regardless.
void acPumpStart(int p) {
  unsigned long now = millis();
  bool wasLive = pcfBitOn(OUT_INVERTER) || invWanted;
  pcfOff(OUT_INVERTER);
  if (apPhase == AP_ON) { apInvOffMs = now; reply("PUMP,OFF"); }   // switching sides mid-run: say so
  if (wasLive) apInvOffMs = now;
  lgStop(lg); lgHighHit = false;
  crossWatchStop();
  apPump = p; apServoOk = false; apServoFail = false; apErr = ""; apReqSent = false;
  apOnMixL = -1.0f;                                           // set again when this run's inverter comes on
  // Pre-seat (2026-11-16): SERVO_PRESEAT_SWINGS full swings ending on the target, all before the inverter comes on.
  apMoveN = servoReseatPlan(p, (p == PUMP_TRANSFER) ? PUMP_BOOSTER : PUMP_TRANSFER, SERVO_PRESEAT_SWINGS, apMoves, AP_MOVES_MAX);
  if (apMoveN == 0) { apMoves[0] = p; apMoveN = 1; }
  apMoveIdx = 0; apReseats = 0; apStartRetries = 0;
  apSelectOnly = false;
  apPhase = AP_INV_OFF;
  unsigned long sinceOff = now - apInvOffMs;              // decay already served since the inverter went off
  apPhaseMs = now - (sinceOff >= INV_DECAY_MS ? INV_DECAY_MS : sinceOff);
  apPzemMs = 0;
}

// Testing (LCD "Servo>Transfer" / "Servo>Booster", dashboard pulse servoL / servoR): move the selector to
// pump p through exactly the same safe path -- inverter off, decay, AC dead-check, SERVO_REQ, OK -- and then
// stop with the inverter still OFF (AP_SELECTED). Reports TEST,SERVO,<pump>,OK when the Nano confirms.
void acPumpSelectOnly(int p) {
  acPumpStart(p);
  apSelectOnly = true;
  apMoves[0] = p; apMoveN = 1;              // positioning only: one move, no pre-seat (the inverter never comes on)
}

static void apFail(const String &why) {
  pcfOff(OUT_INVERTER);                     // already off -- belt and braces
  crossWatchStop();
  apErr = why;
  apPhase = AP_ERROR;
}
// A start step failed (servo not confirmed, Nano refused, inverter gave no voltage). Try the whole start once more
// from the top -- inverter off, decay, dead-check, the same moves -- before it counts as failed (AP_START_RETRY_MAX).
static void apStartFailed(const String &why) {
  if (apSelectOnly || apStartRetries >= AP_START_RETRY_MAX) { apFail(why); return; }
  unsigned long now = millis();
  apStartRetries++;
  if (pcfBitOn(OUT_INVERTER)) apInvOffMs = now;
  pcfOff(OUT_INVERTER);
  reply(String("INFO,PUMP_START_RETRY,") + acPumpName(apPump) + "|why=" + why + "|try=" + apStartRetries + "/" +
        AP_START_RETRY_MAX);
  apMoveIdx = 0; apServoOk = false; apServoFail = false; apReqSent = false; apPzemMs = 0;
  apPhase = AP_INV_OFF; apPhaseMs = now;    // full decay + dead-check again
}

static bool mixLevelFresh() { return lfFresh(lfMix, millis(), LEVEL_BLIND_MS); }
static bool resLevelFresh() { return lfFresh(lfRes, millis(), LEVEL_BLIND_MS); }

// The wrong pump is running (pumpVerdict: the levels moved the wrong way, or nothing moved in a fill). Stop it,
// take the measured tank level, and -- without halting -- re-seat the servo and run again; past SERVO_RESEAT_MAX
// recoveries in this start, give up (AP_ERROR "STUCK": a run holds SERVO_FAIL, a Critical alert with the choice).
static void apCrossDetected(const String &why) {
  unsigned long now = millis();
  pcfOff(OUT_INVERTER);                     // first: the wrong pump stops now
  apInvOffMs = now; apPausedMs = now;
  crossWatchStop();
  lgStop(lg); lgHighHit = false;
  // That water really moved (a stuck booster drained the mixing tank, a stuck transfer filled it). The
  // tank figure is simply the measured level now -- no pulse arithmetic needed.
  if (mixLevelFresh()) { mixTankL = lfMix.med; tankSave(); }
  apReseats++;
  // t = how long the inverter had been on when the verdict was confirmed.
  String d = why + "|t=" + String(now - apOnMs);
  if (!reseatAllowed(apReseats, SERVO_RESEAT_MAX)) { apFail("STUCK|" + d); return; }
  reply(String("SERVO_MISMATCH,") + acPumpName(apPump) + "|" + d + "|try=" + apReseats + "/" + SERVO_RESEAT_MAX);
  int other = (apPump == PUMP_TRANSFER) ? PUMP_BOOSTER : PUMP_TRANSFER;
  apMoveN = servoReseatPlan(apPump, other, SERVO_SWINGS, apMoves, AP_MOVES_MAX);
  apMoveIdx = 0;
  apServoOk = false; apServoFail = false; apReqSent = false; apPzemMs = 0;
  apPhase = AP_INV_OFF; apPhaseMs = now;    // full decay + dead-check before the first swing
}

// The inverter's output is live: from here the selected pump is running and the run counts.
static void apAcOn(unsigned long now) {
  apPhase = AP_ON; apOnMs = now; apGen++;
  lgStart(lg, apPump == PUMP_TRANSFER, now); lgHighHit = false;
  crossWatchStop();                               // every inverter-on gets its own verdict (re-seats too)
  apOnMixL = mixLevelFresh() ? lfMix.med : -1.0f;
  if (apOnMixL >= 0.0f) {                         // references: the levels as the pump comes on
    apXSet = true; apXRefMs = now; apXMix0 = apOnMixL;
    apXRes0 = resLevelFresh() ? lfRes.med : -1.0f;
  }
  reply(String("PUMP,") + acPumpName(apPump) + ",ON");
}

void acPumpTick() {
  unsigned long now = millis();
  switch (apPhase) {
    case AP_INV_OFF: {
      if (now - apPhaseMs < INV_DECAY_MS) return;
#if SECONDARY_BATT_ENABLED
      // P0 first (2026-11-16): the switch started with this run (acPumpHoldsSecondary). The servo may not move,
      // and the inverter may not come on, until the boards are on the secondary and nothing rebooted.
      if (!apSelectOnly) {
        if (battFailWhy.length()) { String w = battFailWhy; battFailWhy = ""; apFail("BATT_SWITCH|" + w); return; }
        if (battPhase != BP_SEC) {
          if (now - apPhaseMs >= INV_DECAY_MS + BATT_READY_MAX_MS) apFail("BATT_SWITCH|TIMEOUT");
          return;
        }
      }
#endif
#if AC_DEAD_CHECK
      if (apPzemMs && now - apPzemMs < 250) return;   // a dead PZEM costs a Modbus timeout per read
      apPzemMs = now;
      float v = pzem.voltage();
      if (!isnan(v) && v >= AC_DEAD_V) {              // the inverter output is still live
        if (now - apPhaseMs >= INV_DECAY_MS + AC_DEAD_WAIT_MS) apFail("AC_LIVE|v=" + String(v, 0));
        return;
      }
#endif
      apSeq++;
      apMoveIdx = 0; apReqPump = apMoves[0];
      apPhase = AP_SERVO_WAIT; apPhaseMs = now;
      return;
    }
    case AP_SERVO_WAIT:
      if (apServoOk) {
        if (++apMoveIdx < apMoveN) {                  // re-seat swing: request the next move
          apSeq++; apReqPump = apMoves[apMoveIdx];
          apServoOk = false; apReqSent = false; apPhaseMs = now;
          return;
        }
        apPhase = AP_SETTLE; apPhaseMs = now;         // the last move ends on the target pump
        return;
      }
      if (apServoFail) { apStartFailed("NANO_FAIL"); return; }
      if (now - apPhaseMs >= SERVO_TIMEOUT_MS) { apStartFailed("TIMEOUT"); return; }
      if (!apReqSent || now - apReqMs >= SERVO_REQ_RETRY_MS) {
        apReqSent = true; apReqMs = now;
        reply(String("SERVO_REQ,") + acPumpName(apReqPump) + "," + apSeq);
      }
      return;
    case AP_SETTLE:
      if (now - apPhaseMs < SERVO_POST_MS) return;
      if (apSelectOnly) {                             // Testing: servo only -- the inverter stays off
        apPhase = AP_SELECTED;
        reply(String("TEST,SERVO,") + acPumpName(apPump) + ",OK");
        return;
      }
#if SECONDARY_BATT_ENABLED
      if (battPhase != BP_SEC) { apFail("BATT_SWITCH|LOST"); return; }   // left the secondary meanwhile: no AC
#endif
      pcfOn(OUT_INVERTER);                            // inverter relay ON (on the secondary: at once)
      apRelayOnMs = now;                              // a fill's run time counts from here (2026-11-17)
      // ...and so does every other timed AC run: ESP1's counters (LCD pump estimate, dashboard Fill/Push pulse)
      // start on this line, not when their button was pressed (the servo and the battery switch come first).
      reply(String("INFO,INV_ON,") + acPumpName(apPump));
#if AC_DEAD_CHECK
      apPhase = AP_INV_CONFIRM; apPhaseMs = now; apPzemMs = 0;   // now wait for its voltage (INV_CONFIRM_MAX_MS)
#else
      apAcOn(now);                                    // no PZEM on the output: nothing to wait for
#endif
      return;
    case AP_INV_CONFIRM: {
      // The relay is ON. Poll the PZEM (a dead one costs a Modbus timeout per read, hence the 250 ms pace). The
      // first reading of voltage confirms the inverter: the pump run starts and the inverter stays on. Nothing
      // here turns it off while voltage is being read -- only INV_CONFIRM_MAX_MS with none does.
      if (apPzemMs && now - apPzemMs < 250) return;
      apPzemMs = now;
      float v = pzem.voltage();
      if (!isnan(v) && v >= INV_CONFIRM_V) {
        reply(String("INFO,INVERTER,AC_OK,v=") + String(v, 0) + ",t=" + String(now - apPhaseMs));
        apAcOn(now);
        return;
      }
      if (now - apPhaseMs >= INV_CONFIRM_MAX_MS)
        apStartFailed(String("INVERTER|NO_AC|v=") + (isnan(v) ? String("none") : String(v, 0)) + "|t=" + String(now - apPhaseMs));
      return;
    }
    case AP_ON: {
      // Which pump is really running (2026-11-16 user rule): one verdict SERVO_JUDGE_MS after the reference levels
      // (taken at inverter-on, or at the first fresh reading after it), confirmed SERVO_CONFIRM_MS later. No fresh
      // levels within SERVO_JUDGE_MAX_MS = no verdict this time (the stage's blind / no-flow rules deal with that).
      if (apJudged) return;
      if (!apXSet) {
        if (!mixLevelFresh()) {
          if (now - apOnMs >= SERVO_JUDGE_MAX_MS) apJudged = true;
          return;
        }
        apXSet = true; apXRefMs = now; apXMix0 = lfMix.med;
        apXRes0 = resLevelFresh() ? lfRes.med : -1.0f;
        return;
      }
      unsigned long el = now - apXRefMs;
      if (el < SERVO_JUDGE_MS) return;
      if (el >= SERVO_JUDGE_MAX_MS || !mixLevelFresh()) {
        if (el >= SERVO_JUDGE_MAX_MS) apJudged = true;
        return;
      }
      int8_t dir = (apPump == PUMP_TRANSFER) ? +1 : -1;
      bool resOk = apXRes0 >= 0.0f && resLevelFresh();
      float dMix = lfMix.med - apXMix0, dRes = resOk ? lfRes.med - apXRes0 : 0.0f;
      // "Nothing moved = the booster" is only a verdict where water MUST move: a run's level-metered fill, with the
      // reservoir clearly above its filter zone. A Testing combo, an exercise or a prime gets the wrong-way rules only.
      bool runFill = (step != SEQ_NONE && sgLevel && sgIsAC && sgPumpOn && sgTankSign > 0 && !sgTimed);
      bool resHasWater = resOk && runFill && lvResPct > RES_FILTER_PCT;
      uint8_t v = pumpVerdict(dir, dMix, dRes, resOk, resHasWater, LVL_MOVED_MIN_L, CROSS_LEVEL_MIN_L);
      if (v == PV_OK) { apJudged = true; return; }  // the right pump: nothing more to judge until the next start
      if (v == PV_UNSURE) { apVerdictMs = 0; return; }   // keep looking until SERVO_JUDGE_MAX_MS
      if (apVerdictMs == 0) { apVerdictMs = nzMs(now); return; }   // first sight: it must still stand in 2 s
      if (now - apVerdictMs < SERVO_CONFIRM_MS) return;
      apCrossDetected(String(v == PV_NOMOVE ? "NO_MOVE" : "WRONG_WAY") + "|mix=" + String(dMix, 2) +
                      "|res=" + (resOk ? String(dRes, 2) : String("blind")));
      return;
    }
    default:
      return;
  }
}

// Retry the two SAFE-ing writes. Every caller discards pcfWrite's status, so a single NAK here used
// to mean "emergency stop issued, relays still energized, nobody the wiser". No delay(): the I2C
// transaction is already bounded by Wire.setTimeOut(50), so 3 tries is <=150 ms -- far inside the 8 s
// WDT and still non-blocking per spec sec.10.6.5.
void stopAll()           { acPumpForget(); battPathReset(); for (uint8_t a = 0; a < 3; a++) if (pcfWrite(0xFFFF)) return; }
// Normal-completion version of stopAll(): identical unless the inverter path is mid-switch or holding
// the boards on the secondary -- then every actuator goes off NOW, but P0 (while on) and P17 stay up
// until the return to main, after which battPathTick() drops P17. Faults keep using stopAll().
void stopAllGraceful() {
  acPumpForget();                                 // every actuator goes off below, the inverter with them
#if SECONDARY_BATT_ENABLED
  if (battPhase != BP_MAIN) {
    invWanted = false;
    if (battPhase == BP_PRE_SEC) { battAbortAnnounce(); stopAll(); return; }   // P0 never moved
    uint16_t keep = 0x7FFF;                                     // all actuators off, P17 on ...
    if (pcfBitOn(OUT_BATT_SEL)) keep &= ~(uint16_t)(1 << OUT_BATT_SEL);   // ... and P0 while it is on
    for (uint8_t a = 0; a < 3; a++) if (pcfWrite(keep)) break;
    if (battPhase == BP_SEC) battEnter(BP_RETURN_WAIT);         // SETTLE_SEC gets there via the tick
    cutoffDropPending = true;
    return;
  }
#endif
  stopAll();
}
// All actuators OFF but the master cutoff kept ENERGIZED (bank stays powered). One atomic write
// (bit15=0 -> P17 on), so switching Testing components never pulses P17 off->on (sec.19.4.8.4).
void stopKeepBank() {
  acPumpForget();                                 // a Testing combo's pump run ends with the selection
#if SECONDARY_BATT_ENABLED
  if (battPhase != BP_MAIN) {                     // inverter path busy: keep P0, return to main later
    invWanted = false;
    if (battPhase == BP_PRE_SEC) { battAbortAnnounce(); pcfWrite(0x7FFF); return; }
    uint16_t keep = 0x7FFF;
    if (pcfBitOn(OUT_BATT_SEL)) keep &= ~(uint16_t)(1 << OUT_BATT_SEL);
    pcfWrite(keep);
    if (battPhase == BP_SEC) battEnter(BP_RETURN_WAIT);
    return;
  }
#endif
  pcfWrite(0x7FFF);
}

/* ---- P17 master actuator-power cutoff (sec.19.4.8) ----------------------- *
 * Active-LOW: clearing bit 15 ENERGIZES the master relay (bank powered); setting it
 * DE-energizes (whole P0-P16 bank goes hardware-dead, fail-safe). Energize before any
 * actuation; de-energize on fault / completion / idle.                                */
void cutoffEnergize()    { cutoffDropPending = false; pcfOn(OUT_MASTER_CUTOFF); }   // new work: keep P17 up
// De-energizing is the fail-safe direction -- retry it like stopAll() (see note above).
void cutoffDeenergize() {
  uint16_t s = pcfShadow | (1 << OUT_MASTER_CUTOFF);
#if SECONDARY_BATT_ENABLED
  // Dropping P17 with the inverter path still holding P0 (a fault path, e.g. a PZEM trip during an
  // exercise) is a switch to main too -- P17 may gate the DPDT coil. Journal it like stopAll() does and
  // drop P0 in the same write, so the shadow says main and a resulting reset is still counted.
  if (battPhase != BP_MAIN && battPhase != BP_SETTLE_MAIN && pcfBitOn(OUT_BATT_SEL)) {
    battPathReset();
    s |= (1 << OUT_BATT_SEL);
  }
#endif
  for (uint8_t a = 0; a < 3; a++) if (pcfWrite(s)) return;
}
// Normal-end P17 drop: deferred until the secondary-battery return has happened (see stopAllGraceful).
void cutoffDeenergizeGraceful() {
#if SECONDARY_BATT_ENABLED
  if (battPhase != BP_MAIN) {
    if (battPhase == BP_PRE_SEC) { invWanted = false; battAbortAnnounce(); cutoffDeenergize(); return; }
    if (battPhase == BP_SEC) { invWanted = false; pcfRawOff(OUT_INVERTER); battEnter(BP_RETURN_WAIT); }
    cutoffDropPending = true;
    return;
  }
#endif
  cutoffDeenergize();
}

// Start a Testing combo (idx 16=Fill, 17/18/19=Push>Col A/B/C, 20/21=Servo>Transfer/Booster, 22=Mixer).
// Valves first; a pump then goes through acPumpStart (servo, then the inverter), so a combo takes a few
// seconds to actually run. The servo rows move the selector only: the inverter never comes on.
void testComboOn(int idx) {
  switch (idx) {
    case 16: resValveSet(true); acPumpStart(PUMP_TRANSFER); break;                  // reservoir -> mixing
    case 17: case 18: case 19:
      pcfOn(COL_VALVE_BIT[idx - 17]); acPumpStart(PUMP_BOOSTER); break;              // mixing -> column
    case 20: acPumpSelectOnly(PUMP_TRANSFER); break;                                 // servo LEFT, no AC
    case 21: acPumpSelectOnly(PUMP_BOOSTER);  break;                                 // servo RIGHT, no AC
    case 22: pcfOn(OUT_MIXER); break;                                                // 12 V DC mixer
    default: return;
  }
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

/* ---- Dose journal (see doseStillOwed) ------------------------------------- *
 * What the tank already holds and which column it was dosed for. Survives any reset, and is cleared only
 * when a run finishes. ESP1 hears about it on every boot (INFO,TANK,DOSED,<mask>) and sends that column
 * first, so an interrupted batch still reaches the crop it was mixed for. `stopped` marks a batch someone
 * or something stopped on purpose (STOP_ALL, or a fault lost in a reset): it is not offered for finishing,
 * since nobody wants that run to restart by itself, but its doses still count when the column next runs. */
struct DoseJournal { uint8_t mask; uint8_t stopped; float mL[3]; };
DoseJournal doseJ = { 0, 0, { 0.0f, 0.0f, 0.0f } };
static void doseJSave() { prefs.putBytes("dosej", &doseJ, sizeof(doseJ)); }
void doseJLoad() {
  if (!prefs.isKey("dosej")) return;
  if (prefs.getBytes("dosej", &doseJ, sizeof(doseJ)) != sizeof(doseJ)) doseJ = DoseJournal{ 0, 0, { 0.0f, 0.0f, 0.0f } };
}
static void doseJClear() {
  if (!doseJ.mask) return;
  doseJ = DoseJournal{ 0, 0, { 0.0f, 0.0f, 0.0f } };
  prefs.remove("dosej");
}
static void doseJMarkStopped() {
  if (!doseJ.mask || doseJ.stopped) return;
  doseJ.stopped = 1;
  doseJSave();
}
static void doseJStart(int i, uint8_t mask, float targetMl) {
  doseJ.mask |= mask;
  doseJ.stopped = 0;
  doseJ.mL[i] += targetMl;
  doseJSave();
}
static void doseJEnd(int i, float targetMl, float measuredMl) {
  doseJ.mL[i] = doseJournalTrim(doseJ.mL[i], targetMl, measuredMl);
  doseJSave();
}
void tankLoad() {
  prefs.begin("esp2", false);
  mixTankL = prefs.getFloat("mixL", 0.0f);
  if (mixTankL < 0.0f) mixTankL = 0.0f;
  mixDeadL = prefs.getFloat("mixDead", 0.0f);                 // the booster's learned floor (web v.4)
  if (!(mixDeadL >= 0.0f) || mixDeadL > MIX_DEAD_MAX_L) mixDeadL = 0.0f;
  mixHeelL = prefs.getFloat("mixHeel", 0.0f);                 // what a low-stopped delivery leaves (2026-11-16)
  if (!(mixHeelL >= 0.0f) || mixHeelL > MIX_DEAD_MAX_L) mixHeelL = 0.0f;
}
// AC supply window (see PZEM_V_MIN). Same rule as ESP1's acVoltWindowOk(); NaN and garbage fail it.
static bool pzemWindowSane(float lo, float hi) {
  return lo >= PZEM_V_LIMIT_LO && hi <= PZEM_V_LIMIT_HI && hi - lo >= PZEM_V_MIN_GAP;
}
// After tankLoad() (it opens the namespace). A missing or bad pair keeps the compiled defaults.
// AC current limits (see PZEM_OVERCURRENT_A), tenths of an amp. Same rule as ESP1's acAmpWindowOk().
static bool pzemAmpSane(long lo10, long hi10) {
  return lo10 >= PZEM_A10_LIMIT_LO && hi10 <= PZEM_A10_LIMIT_HI && hi10 - lo10 >= PZEM_A10_MIN_GAP;
}
static int pzemA10Min = PZEM_A10_DEFAULT_MIN, pzemA10Max = PZEM_A10_DEFAULT_MAX;
void pzemWindowLoad() {
  if (prefs.isKey("pzalo") && prefs.isKey("pzahi")) {
    int lo = prefs.getInt("pzalo", PZEM_A10_DEFAULT_MIN), hi = prefs.getInt("pzahi", PZEM_A10_DEFAULT_MAX);
    if (pzemAmpSane(lo, hi)) {
      pzemA10Min = lo; pzemA10Max = hi;
      PZEM_MIN_CURRENT_A = lo / 10.0f; PZEM_OVERCURRENT_A = hi / 10.0f;
    }
  }
  if (!prefs.isKey("pzvlo") || !prefs.isKey("pzvhi")) return;
  float lo = prefs.getFloat("pzvlo", PZEM_V_DEFAULT_MIN), hi = prefs.getFloat("pzvhi", PZEM_V_DEFAULT_MAX);
  if (pzemWindowSane(lo, hi)) { PZEM_V_MIN = lo; PZEM_V_MAX = hi; }
}
// Only called on a real change: ESP1 re-sends the window on every power-up, and flash wears.
static void pzemWindowSave() { prefs.putFloat("pzvlo", PZEM_V_MIN); prefs.putFloat("pzvhi", PZEM_V_MAX); }
static void pzemAmpSave()    { prefs.putInt("pzalo", pzemA10Min); prefs.putInt("pzahi", pzemA10Max); }
// Pump rates the operator measured (SET_CAL,PRATE / NRATE; see RATE_WEIGHT). 0 = not measured. A stored value out
// of range is dropped, never used (it scales every litres estimate).
static bool rateLpmOk(float v)  { return v >= RATE_LPM_MIN && v <= RATE_LPM_MAX; }
static bool rateMlpmOk(float v) { return v >= RATE_MLPM_MIN && v <= RATE_MLPM_MAX; }
static const char *RATE_NUT_KEY[3] = { "rNA", "rNB", "rNC" };
static float rateLoadOne(const char *key, bool ml) {
  if (!prefs.isKey(key)) return 0.0f;
  float v = prefs.getFloat(key, 0.0f);
  return (ml ? rateMlpmOk(v) : rateLpmOk(v)) ? v : 0.0f;
}
void pumpRateLoad() {                       // after tankLoad() (it opens the namespace)
  rateXferLpm = rateLoadOne("rX", false);
  rateBoostLpm = rateLoadOne("rB", false);
  for (int i = 0; i < 3; i++) rateNutMlpm[i] = rateLoadOne(RATE_NUT_KEY[i], true);
  if (prefs.isKey("xS")) {                    // the transfer's re-measured start litres (SET_CAL,PSTART)
    float a = prefs.getFloat("xS", XFER_START_L);
    if (a >= -PSTART_MAX_L && a <= PSTART_MAX_L) xferStartL = a;
  }
}
// Only on a real change (ESP1 re-sends them on every power-up). 0 removes the key: back to "not measured".
static void rateSaveOne(const char *key, float v) {
  if (v > 0.0f) prefs.putFloat(key, v);
  else if (prefs.isKey(key)) prefs.remove(key);
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

// CRC-8 (poly 0x07, init 0) of a text -- the same function ESP1 (pure_math.h crc8Text) and the Nano use. Only
// the LEVEL line carries it on this link (web v.4): it is the one ESP1 -> ESP2 line that meters water.
static uint8_t crc8Text(const char *s, size_t n) {
  uint8_t c = 0;
  for (size_t k = 0; k < n; k++) {
    c ^= (uint8_t)s[k];
    for (uint8_t i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}
// A clean decimal number: optional '-', digits, at most one '.', nothing else (toFloat() would turn garbage
// into 0, and 0 L is a perfectly believable level).
static bool numToken(const String &t) {
  int len = t.length();
  if (len == 0 || len > 9) return false;
  int i = (t[0] == '-') ? 1 : 0, digits = 0, dots = 0;
  if (i >= len) return false;
  for (; i < len; i++) {
    char ch = t[i];
    if (ch >= '0' && ch <= '9') digits++;
    else if (ch == '.') { if (++dots > 1) return false; }
    else return false;
  }
  return digits > 0;
}

/* =============================================================================
 *  FLOW METERING (stage-based interrupt)
 * ========================================================================== */
void attachFlow(int pin) {
  // A prior selection's interrupt must be detached before a new one is attached, or its ISR keeps
  // firing on the old pin forever, permanently contaminating flowPulses with an unrelated source.
  if (activeFlowPin >= 0 && activeFlowPin != pin) detachInterrupt(digitalPinToInterrupt(activeFlowPin));
  if (pin < 0) { activeFlowPin = -1; flowPulses = 0; return; }   // a level-metered stage has no meter
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
  return pulsesToLiters(p, k);   // the K guard lives with the division, in include/flow_math.h
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
    creditRunningStage();                    // read the meter before the stop detaches it
    doseJMarkStopped();                      // before stopAll: its P0 drop may be the last thing we do
    stopAll(); step = SEQ_NONE; wo.active = false; faultHeld = false; detachFlow();
    resumeTimed = false; lastHoldFlow = false;           // clear the timed-recovery latch
    testMode = false; testHeldBit = -1; testCapped = false;
    if (primeLine != "") stopPrime();                    // drop any prime
    calId = "";                                          // and any calibration stream

    // Cancel any in-flight one-shot timers so loop() can't emit a stray late DONE
    // (DONE,EXERCISE) after the stop. NOTE: mixTankL is kept (persisted) -- the tank may
    // still physically hold water, and the next run must account for it (no overfill).
    exRunOffAt = 0; exRunBit = -1; exRunAc = 0; exRunPumpOn = false;
    reply("DONE,STOP_ALL");
    return;
  }
  if (cmd == "STATUS_REQ") { reply("STATUS,ESP2,OK"); return; }
  // Pump-selector result relayed by ESP1: SERVO,<TRANSFER|BOOSTER>,<OK|FAIL>,<seq>. Only the answer to the
  // request in flight counts -- a late reply to an older request (other pump or older seq) is ignored.
  if (cmd == "SERVO") {
    if (n >= 4 && apPhase == AP_SERVO_WAIT && tok[1] == acPumpName(apReqPump) && (uint16_t)tok[3].toInt() == apSeq) {
      if (tok[2] == "OK") apServoOk = true; else apServoFail = true;
    }
    return;
  }
  // Tank levels relayed by ESP1: LEVEL,<mixPct|-1>,<resPct|-1>,<mixL|-1>,<resL|-1>[,<resInL>],*XX (web v.4).
  // The % may exceed 100 (water above the calibrated full mark); 0 is the floor.
  // These numbers now meter every litre of water, so the line must carry a matching CRC-8 and every field
  // must be a clean number; anything else is dropped whole (lvBad) and simply does not refresh the levels --
  // the blind rules then decide, which is the safe direction. A corrupted number can never become a level.
  if (cmd == "LEVEL") {
    int star = payload.lastIndexOf(",*");
    if (n < 6 || star < 0 || star + 4 > (int)payload.length()) { lvBad++; return; }
    uint8_t want = (uint8_t)strtoul(payload.substring(star + 2, star + 4).c_str(), NULL, 16);
    if (crc8Text(payload.c_str(), (size_t)star) != want) { lvBad++; return; }
    float v[4];
    for (int k = 0; k < 4; k++) {
      if (!numToken(tok[1 + k])) { lvBad++; return; }
      v[k] = tok[1 + k].toFloat();
    }
    // ESP1's line number (6th field, 2026-11-16): a repeat is dropped before it can count twice; a gap is lines
    // lost on the link. A big jump is ESP1 restarting its count, not a loss.
    if (n >= 8) {
      if (!numToken(tok[6])) { lvBad++; return; }
      int q = tok[6].toInt();
      if (q < 0 || q > 9999) { lvBad++; return; }
      if (lvSeqLast >= 0) {
        int d = (q - lvSeqLast + 10000) % 10000;
        if (d == 0) return;
        if (d > 1 && d < 5000) lvGap += (uint32_t)(d - 1);
      }
      lvSeqLast = q;
    }
    unsigned long now = millis();
    lvMixPct = v[0]; lvResPct = v[1];
    if (lgSample(lg, v[0], now, MIX_HIGH_PCT, LEVEL_HIGH_COUNT) == LG_HIGH) lgHighHit = true;
    lfPush(lfMix, v[2], now, LVL_JUMP_L);
    lfPush(lfRes, v[3], now, LVL_JUMP_L);
    // The tank FEEDING a running water stage (-1 = no usable reading: ignored): the reservoir on a fill (its filter
    // zone, whole percents: < zone + 0.5 means "at or under it"), the mixing tank on a drain (the low stop).
    if (sgLevel && sgIsAC && sgPumpOn) {
      if (sgTankSign > 0) slSample(srcLowRes, v[1], now, RES_FILTER_PCT + 0.5f, SOURCE_LOW_COUNT);
      if (sgTankSign < 0) slSample(srcLowMix, v[0], now, MIX_LOW_STOP_PCT, SOURCE_LOW_COUNT);
    }
    // Optional 5th number: litres the Nano's reservoir flow meter has counted INTO the reservoir since ESP1
    // booted. A fill adds what came in during it back to the reservoir's drop.
    if (n >= 7 && numToken(tok[5])) lvResInL = tok[5].toFloat();
    return;
  }
  if (cmd == "LEVEL_FEED") return;           // (ESP1 never sends this; ignore an echo)
  // Battery-switch handshake from ESP1: GO = prepared for the announced switch, ACK = it has
  // recorded our switch-caused reset report.
  if (cmd == "BATTSW") {
    String sub = (n >= 2) ? tok[1] : "";
    if (sub == "GO")  { battGo = true; return; }
    // Answer to INFO,BATT,VERIFY (2026-11-16). Only the switch being verified counts; a late answer is ignored.
    if (sub == "OK")   { if (battPhase == BP_VERIFY_SEC) battOk = true; return; }
    if (sub == "FAIL") { if (battPhase == BP_VERIFY_SEC) battFailWhy = (n >= 3 && tok[2].length()) ? tok[2] : String("ESP1"); return; }
    if (sub == "ACK") { bswBootReport = 0; return; }
    if (sub == "FACK") { faultReport = ""; return; }
    reply("INVALID,BATTSW"); return;
  }
#if SECONDARY_BACKUP_ENABLED
  // Backup hold: boards on the secondary with no inverter. ESP1 repeats ON until it hears
  // INFO,BATT,SECONDARY, so ON must be idempotent and re-confirm a confirmation that was lost.
  if (cmd == "BATTBK") {
    String sub = (n >= 2) ? tok[1] : "";
    if (sub == "ON") {
      if (wo.active || step != SEQ_NONE || faultHeld || testMode) { reply("BUSY,BATTBK"); return; }
      battBackupHold = true; cutoffDropPending = false;
      cutoffEnergize();                      // P17 may gate the DPDT coil, so the bank stays powered
      if (battPhase == BP_SEC) reply("INFO,BATT,SECONDARY");
      return;
    }
    if (sub == "OFF") {
      battBackupHold = false;
      if (battPhase == BP_PRE_SEC) { battAbortAnnounce(); cutoffDeenergize(); }
      else if (battPhase == BP_MAIN) { reply("INFO,BATT,MAIN"); cutoffDeenergize(); }
      else cutoffDropPending = true;         // drop P17 once the return to main has happened
      return;
    }
    reply("INVALID,BATTBK"); return;
  }
#endif
  if (cmd == "RESET_SELF") {                 // soft reboot on ESP1 request (spec sec.9.7.2)
    reply("ACK,RESET_SELF");
    // If genuinely still mid-sequence (reachable via a narrow ESP1-side race after a recovery-
    // confirmation ambiguity -- ESP1 can conclude "ESP2 restarted" and eventually escalate to
    // RESET_SELF while ESP2 is in fact still executing correctly), persist whatever mixTankL
    // already holds before the reboot discards RAM state. This only saves what's already been
    // metered up to this point, including the in-flight stage's litres. A raw GPIO4 cut with no STOP_ALL
    // ahead of it remains the documented gap (CLAUDE.md's P17 section).
    creditRunningStage();
    if (wo.active || step != SEQ_NONE) tankSave();
    stopAll();                               // force SAFE (all outputs OFF) before reboot
    bswJournal(0);                           // a deliberate reboot, not a battery-switch failure
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
    // Audit fix (2026-09-09): this guard checked wo.active/faultHeld/testMode but never
    // exRunOffAt (an EXERCISE run in progress) -- PRIME_START/SEQ_ dispatch already refuse each
    // other correctly; EXERCISE was the one command missing from every side of this mutual
    // exclusion, which could energize an EXERCISE pump concurrently with a calibration stream.
    if (exRunOffAt != 0) { reply("BUSY,EXERCISE"); return; }
    if (n < 2) { reply("INVALID,CAL_START"); return; }
    // Drop whatever flow ISR a previous calId may have left armed -- CAL_STOP only detaches when the
    // id being stopped starts with "FLOW_", so switching straight from a flow id to a non-flow id
    // (no intervening CAL_STOP) used to leave that ISR silently counting on the old pin forever.
    detachFlow();
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
    // Audit fix (2026-09-09): same missing exRunOffAt check as CAL_START above -- without it, an
    // EXERCISE pump run and a PRIME_START purge could energize two actuators simultaneously,
    // violating the locked sequential-never-simultaneous pump architecture.
    if (exRunOffAt != 0) { reply("BUSY,EXERCISE"); return; }
    if (n < 2) { reply("INVALID,PRIME"); return; }
    if (primeLatched) { primeLatchKeepMs = millis(); return; }   // ended on its own: wait for a release
    // Too soon after the last prime ended (PRIME_REARM_MS): ignore it (user, 2026-11-17).
    if (primeLine == "" && primeStoppedMs && millis() - primeStoppedMs < PRIME_REARM_MS) return;
    if (primeLine == "" || primeLine != tok[1]) {
      unsigned long runMs = (n >= 4) ? (unsigned long)tok[3].toInt() : 0;
      if (runMs && (runMs < PRIME_TIMED_MIN_MS || runMs > PRIME_TIMED_MAX_MS)) { reply("INVALID,PRIME_TIME"); return; }
      setupPrime(tok[1], (n >= 3) ? tok[2] : "");
      if (primeLine != "") { primeRunMs = primeIsAC ? runMs : 0; primeArmMs = millis(); primeRelayMs = 0; primeAcSeen = false; }
    }
    primeLastMs = millis();                   // (re)arm the dead-man keep-alive
    return;
  }
  if (cmd == "PRIME_STOP") {
    // The re-arm gap starts only at a real release (PRIME_STOP), not when a keep-alive was missed (2026-11-17).
    if (primeLine != "") primeStoppedMs = nzMs(millis());
    primeLatched = false; stopPrime(); reply("DONE,PRIME"); return;
  }

  // ---- manual relay test, dead-man (ESP1 Settings>Testing, spec sec.18.10.8) ----
  if (cmd == "TEST") {
    String sub = (n >= 2) ? tok[1] : "";
    if (sub == "ENTER") {                                  // enter TEST_MODE, force SAFE
      if (wo.active || step != SEQ_NONE) { reply(faultHeld ? "BUSY,HELD" : "BUSY,ACTIVE"); return; }
      // A Prime or Calibration session is still live (its own dead-man hasn't force-dropped P17 yet)
      // -- refuse rather than let Testing start under it and get its power pulled out from under it
      // by Prime's dead-man once ESP1 stops sending PRIME_START keep-alives.
      if (primeLine != "" || calId != "") { reply(primeLine != "" ? "BUSY,PRIME" : "BUSY,CAL"); return; }
      // Audit fix (2026-09-10): this force-clear previously discarded an in-flight EXERCISE with no
      // reply at all, leaving ESP1 believing the exercise was still outstanding until whatever
      // timeout it uses elapses. Report the same DONE,EXERCISE it would have gotten from a normal
      // completion, just early -- read exRunName BEFORE the reset below clears it.
      bool hadExercise = (exRunOffAt != 0);
      String discardedExName = exRunName;
      stopAll(); testMode = true; testHeldBit = -1; testCapped = false;
      cutoffEnergize();                                    // power the bank so manual relay control works (sec.19.4.8.4)
      exRunOffAt = 0; exRunBit = -1; exRunAc = 0; exRunPumpOn = false;                       // drop any pending one-shot timers
      if (hadExercise) reply("DONE,EXERCISE," + discardedExName);
      reply("ACK,TEST,ENTER"); return;
    }
    if (sub == "EXIT") {
      // A late EXIT must not reach a work order: it would drop the running pump and P17 while the FSM
      // carried on, and the stage would then report a FLOW_FAIL nothing had caused.
      if (!testMode) { reply("DONE,TEST"); return; }
      stopAllGraceful(); detachFlow(); testMode = false; testHeldBit = -1;  // all OFF incl. master cutoff de-energized
      exRunOffAt = 0; exRunBit = -1; exRunAc = 0; exRunPumpOn = false;                       // drop any pending one-shot timers
      reply("DONE,TEST"); return;
    }
    if (sub == "RELEASE") {                                // explicit button-up
      // Report what the matching flow meter saw BEFORE tearing the selection down. This is what
      // lets the operator confirm remotely that a pump actually moved fluid: a non-zero count means
      // the pump, line and sensor all work; zero means one of the three does not. Only sent when
      // the output has a meter at all -- see flowPinForBit.
      if (testHeldBit >= 0 && flowPinForBit(testHeldBit) >= 0) {
        noInterrupts(); unsigned long fp = flowPulses; interrupts();
        reply("TEST,FLOW," + String(testHeldBit) + "," + String(fp));
      }
      // Fill / Push combos (web v.4): no meter -- report how far the mixing tank moved since inverter-on.
      // -99 = it never got a level to compare (the pump never came on, or the ultrasonic was blind).
      if (testHeldBit >= 16 && testHeldBit <= 19) {
        float d = (apOnMixL >= 0.0f && mixLevelFresh()) ? lfMix.med - apOnMixL : -99.0f;
        reply("TEST,LEVEL," + String(testHeldBit) + "," + String(d, 2));
      }
      detachFlow();
      testOff();                                           // single relay OR combo -> off, bank stays powered
      if (testHeldBit >= 0) testReleasedMs = nzMs(millis());
      testHeldBit = -1; testCapped = false; return;
    }
    if (sub == "HOLD") {                                   // dead-man keep-alive for <bit/combo>
      if (!testMode || n < 3) return;
      int bit = tok[2].toInt();
      if (bit < 0 || bit > 22) { reply("INVALID,TEST_BIT"); return; }   // 0..15 relays, 16..22 combos
      // Bare Inverter row (P2): ALLOWED by user override (2026-11-15), knowing the constraint -- it powers
      // whichever pump the servo happens to be on, with no servo step, no valve, no level watch and no
      // stuck-servo check. Only the dead-man (400 ms) and the 30 s single-relay cap protect it. The Fill /
      // Push combos remain the safe way to run a pump. pcfWrite() starts the decay clock when it goes off,
      // so a servo move right after this row still waits INV_DECAY_MS.
      if (bit != testHeldBit) {                            // new selection -> one-at-a-time switch
        // Too soon after the last selection ended (TEST_REARM_MS): ignore it, keep-alive and all.
        if (testHeldBit < 0 && testReleasedMs && millis() - testReleasedMs < TEST_REARM_MS) return;
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
        // Arm the meter that watches this output, zeroed, so the count on RELEASE covers exactly
        // this pulse. attachFlow() resets the ISR counter; outputs without a meter detach instead,
        // so a stale count from a previous selection can never be reported against this one.
        int fpin = flowPinForBit(bit);
        if (fpin >= 0) attachFlow(fpin); else detachFlow();
        testHeldBit = bit; testRelayOnMs = millis(); testCapped = false;
        testInvOnMs = 0; testInvTickMs = millis();         // an inverter combo's run time starts at zero
        testInvAcOk = false; testInvRelayMs = 0;          // a new selection gets its own inverter confirmation
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
    // A previous EXERCISE is still running its own one-shot timer -- refuse a second one rather than
    // overwrite exRunBit/exRunOffAt and orphan the first pump's relay ON with nothing left tracking it.
    if (exRunOffAt != 0) { reply("BUSY,EXERCISE"); return; }
    // Audit fix (2026-09-09): this guard never checked primeLine/calId, so an EXERCISE run could be
    // started concurrently with an already-active PRIME_START purge or calibration stream, energizing
    // two actuators at once. PRIME_START/CAL_START/the SEQ_ dispatch already refuse each other and
    // now refuse EXERCISE too (see the exRunOffAt checks added there); this is the other half.
    if (primeLine != "" || calId != "") { reply(primeLine != "" ? "BUSY,PRIME" : "BUSY,CAL"); return; }
    if (n < 2) { reply("INVALID,EXERCISE"); return; }
    int ac = (tok[1] == "TRANSFER") ? PUMP_TRANSFER
           : (tok[1] == "BOOSTER")  ? PUMP_BOOSTER : PUMP_NONE;
    if (ac == PUMP_NONE && tok[1] != "MIXER") { reply("INVALID,EXERCISE_DEV"); return; }
    reply("ACK,EXERCISE");
    cutoffEnergize();                                  // power the bank (sec.19.4.8)
    exRunAc = ac; exRunBit = ac ? -1 : OUT_MIXER; exRunName = tok[1]; exRunPumpOn = false;
    // AC pump: servo to it with the inverter off, then the inverter (acPumpTick); exerciseTick() starts the
    // run clock once it is powered. DC mixer: just its relay after the usual DC settle.
    if (ac) acPumpStart(ac);
    // ESP1 may carry the operator-set run length as a third token. Clamped HERE too: ESP1 already
    // bounds it, but this is the side that actually energises the relay, so it does not take a
    // duration on trust from the wire.
    unsigned long runMs = (n >= 3) ? (unsigned long)tok[2].toInt() : PUMP_EXERCISE_RUN_MS;
    if (runMs < PUMP_EX_RUN_MIN_MS) runMs = PUMP_EX_RUN_MIN_MS;
    if (runMs > PUMP_EX_RUN_MAX_MS) runMs = PUMP_EX_RUN_MAX_MS;
    exRunMs = runMs;
    exRunPumpAtMs = millis() + VALVE_SWITCH_MS;         // DC mixer settle (unused for an AC pump)
    exRunOffAt = nzMs(millis());                          // nonzero = exercise active (busy guards); see exerciseTick()
    return;
  }

  // ---- sequence commands ----
  bool isIrr  = cmd.startsWith("SEQ_IRRIGATION_");
  bool isFert = cmd.startsWith("SEQ_FERTIGATION_");
  if (!isIrr && !isFert) { reply("INVALID,UNKNOWN_CMD"); return; }
  if (faultHeld) { reply("BUSY,HELD"); return; }                 // a held fault must be resolved (RESUME) first
  if (testMode) { reply("BUSY,TEST"); return; }                  // never auto-run during a manual test
  if (wo.active || step != SEQ_NONE) { reply("BUSY,ACTIVE"); return; }
  // Same reasoning as the TEST,ENTER guard above: a live Prime/Cal session must be stopped
  // (PRIME_STOP / CAL_STOP) before a new work order can safely start.
  if (primeLine != "" || calId != "") { reply(primeLine != "" ? "BUSY,PRIME" : "BUSY,CAL"); return; }
  // Audit fix (2026-09-09): this dispatch guard never checked exRunOffAt (an EXERCISE run in
  // progress) either -- ESP1's own unattended 2-day EXERCISE schedule landing at the same moment as
  // a scheduled/forced work order could energize both concurrently.
  if (exRunOffAt != 0) { reply("BUSY,EXERCISE"); return; }

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
    else if (tok[i] == "EC" && i + 2 < n) {
      float lo = tok[++i].toFloat(), hi = tok[++i].toFloat();
      if (lo < hi) { wo.ecLo = lo; wo.ecHi = hi; } else reply("INVALID,EC_WINDOW");   // keep the wide default set above
    }
    else if (tok[i] == "PH" && i + 2 < n) {
      float lo = tok[++i].toFloat(), hi = tok[++i].toFloat();
      if (lo < hi) { wo.phLo = lo; wo.phHi = hi; } else reply("INVALID,PH_WINDOW");   // keep the wide default set above
    }
    else if (tok[i] == "MIX" && i + 1 < n) {
      unsigned long m = (unsigned long)tok[++i].toInt();
      // Audit fix (2026-09-10): 0 now falls back to the same MIX_DEFAULT_MS already set above before
      // this loop runs, instead of literally meaning "mix for 0 ms" -- matches DCEIL's own "0 = use
      // the default" convention a few lines up, rather than silently skipping mixing on a malformed
      // or missing MIX token.
      wo.mixMs = (m == 0) ? MIX_DEFAULT_MS : (m <= MIX_MAX_MS) ? m : MIX_MAX_MS;
    }
    // Job-critical calibration carried in the work order (§A.5.1 #3): never run a dose on stale K/cal.
    // Range checked (see kSane): an implausible K is ignored so metering keeps the last known-good
    // value, and ESP1 is told, so a corrupted frame is visible instead of silently mis-metering.
    else if (tok[i] == "KMAIN" && i + 2 < n) { i += 2; }   // web v.4: no main-line meters -- an older ESP1's K is ignored
    // MIXCAP,<L>: litres at MIX_HIGH_PCT from ESP1's tank calibration -- the planning ceiling of a fill.
    else if (tok[i] == "MIXCAP" && i + 1 < n) {
      float v = tok[++i].toFloat();
      if (v > 0.5f && v <= MIXING_TANK_MAX_L) mixCapL = v; else reply("INVALID,MIXCAP");
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
  if (wo.flushPct < 0) wo.flushPct = 0;
  if (wo.flushPct > 90) wo.flushPct = 90;

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
  woRateWarned = 0;                  // a weak pump is reported once per work order
  // A dosed tank left by a reset or a stopped run. Same column: dose only what is still owed. Another
  // column: that batch goes there as it is, so add nothing on top, and ESP1 raises DOSE_CARRY.
  if (doseJ.mask) {
    if (doseJ.mask & wo.colMask) {
      if (wo.fertigate) {
        for (int i = 0; i < 3; i++) wo.dose[i] = doseStillOwed(wo.dose[i], doseJ.mL[i]);
        reply(String("INFO,DOSE_RESUME,") + String(doseJ.mL[0], 1) + "|" + String(doseJ.mL[1], 1) + "|" + String(doseJ.mL[2], 1));
      }
    } else if (mixTankL >= TANK_EPS_L) {
      if (wo.fertigate) for (int i = 0; i < 3; i++) wo.dose[i] = 0.0f;
      reply(String("INFO,DOSE_CARRY,") + doseJ.mask);
    } else {
      doseJClear();              // the tank is empty: that batch is long gone
    }
  }
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
  lvGateMs = 0;                      // every step that needs a settled level waits for its own (rule 1)
  if (s == SEQ_NONE) return;
  reply("STAGE," + String(stepOrdinal(s)) + "," + String(stepTotal()) + "," + SEQ_NAME[s]);
}

/* ---- Live progress (liters into the current metered stage) ---------------- *
 * ~1 Hz while a work order runs so ESP1 can draw a moving bar. Cheap: no extra hardware reads.       */
const unsigned long PROG_MS = 1000;
unsigned long lastProgMs = 0;
static bool fillProgress(float &L, float &T);   // with fillStep: a fill's progress across its runs and settles
void progTick() {
  if (!wo.active) { lastProgMs = 0; return; }
  if (millis() - lastProgMs < PROG_MS) return;
  lastProgMs = millis();
  float L = (sgPump >= 0 && sgPumpOn) ? stageMovedL() : 0.0f, T = sgTarget;
  // A fill is one or more runs with settles between them (two-step fill, 2026-11-18): report the whole fill, so the bar
  // does not drop back to zero for the settle or start again for the top-up.
  fillProgress(L, T);
  reply("PROG," + String(L, 3) + "," + String(T, 3) + "," + String(woWaterL, 2));
}

/* ---- Level feed lease (web v.4) ------------------------------------------- *
 * ESP1 relays LEVEL only while an AC pump is ON unless we ask. The metering needs the level BETWEEN pumps
 * too (settled baselines before a fill and before a delivery, the measured level after), so for the whole
 * of a live work order -- and around any AC pump run -- we hold a lease: LEVEL_FEED,ON every
 * LEVEL_FEED_RENEW_MS, LEVEL_FEED,OFF when done. A lost OFF costs nothing: ESP1 lets the lease expire.     */
void levelFeedTick() {
  bool want = (wo.active && !faultHeld) || apPhase != AP_IDLE || (primeLine != "" && primeIsAC);
  unsigned long now = millis();
  if (want) {
    if (!lvFeedWanted || now - lvFeedSentMs >= LEVEL_FEED_RENEW_MS) {
      lvFeedWanted = true; lvFeedSentMs = now;
      reply("LEVEL_FEED,ON");
    }
  } else if (lvFeedWanted) {
    lvFeedWanted = false;
    reply("LEVEL_FEED,OFF");
  }
}

/* Settled baseline for planning a fill or a delivery (rule 1). Counts only samples accepted at least
 * LVL_SETTLE_MS after the inverter last went off. 1 = ready (out = the median), 0 = still waiting,
 * -1 = no settled level within LVL_BASE_MAX_MS. goStep() restarts the gate for every step.               */
int levelBaseline(float &out) {
  unsigned long now = millis();
  if (lvGateMs == 0) { lvGateMs = nzMs(now); lvGateCount0 = lfMix.count; }
  bool settled = (pcfBitOn(OUT_INVERTER) == false) && (now - apInvOffMs >= LVL_SETTLE_MS || apInvOffMs == 0) &&
                 !pcfBitOn(OUT_MIXER) && (mixerOffMs == 0 || now - mixerOffMs >= MIX_SETTLE_MS);
  if (!settled) lvGateCount0 = lfMix.count;                 // samples taken on a moving surface do not count
  if (settled && lfMix.count - lvGateCount0 >= LVL_BASE_SAMPLES && mixLevelFresh()) {
    out = lfMix.med;
    return 1;
  }
  if (now - lvGateMs >= LVL_BASE_MAX_MS) return -1;
  return 0;
}

// Litres a running FILL has transferred (2026-11-16): the reservoir's drop -- plus what its flow meter saw
// poured in meanwhile -- as the basis, the mixing tank's rise as the witness (level_meter.h xferEstimate).
// resDownL / mixUpL (optional) return the two raw measurements (-1 = unreadable).
static XferEst fillEstimate(float *resDownL = nullptr, float *mixUpL = nullptr, float *inL = nullptr) {
  bool mixOk = mixLevelFresh();
  float mixUp = mixOk ? lfMix.med - sgMixBaseL : 0.0f;
  bool resOk = !sgResLost && sgResBaseL >= 0.0f && resLevelFresh();
  float resDown = 0.0f, in = 0.0f, inRaw = 0.0f;
  if (resOk) {
    resDown = sgResBaseL - lfRes.med;
    // ESP1's count only grows; a smaller one means ESP1 restarted -- nothing to add then.
    if (sgResInBaseL >= 0.0f && lvResInL > sgResInBaseL) in = lvResInL - sgResInBaseL;
    inRaw = in;
    // That count comes from the Nano's reservoir flow meter: a pulse counter, the kind of sensor the inverter's noise
    // upset on the main lines. It is believed only as far as the mixing tank can explain it (2026-11-17 hardening):
    // inflow beyond (mixing-tank rise - reservoir drop) + XCHECK_MIN_L is noise. Unbounded, a noisy meter would inflate
    // the estimate and end every fill early; bounded, it can shift it by at most that 1 L allowance.
    if (in > 0.0f && mixOk) {
      float room = (mixUp - resDown) + XCHECK_MIN_L;
      if (room < 0.0f) room = 0.0f;
      if (in > room) in = room;
    }
    resDown += in;
  }
  XferEst e = xferEstimate(resDown, resOk, mixUp, mixOk, FILL_RES_WEIGHT, XCHECK_MIN_L, XCHECK_FRAC);
  if (resDownL) *resDownL = resOk ? resDown : -1.0f;
  if (mixUpL)   *mixUpL   = mixOk ? mixUp : -1.0f;
  if (inL)      *inL      = inRaw;           // what the meter counted (the log shows it; resDownL holds what was used)
  return e;
}
/* ---- calibrated rate x run time (2026-11-17 user request, see RATE_WEIGHT) ---------------------------------- */
bool sgRateUse = false;        // this stage blends rate x time in (a measured rate, the pump running normally)
static const char *sgPumpName() {
  if (sgIsAC) return sgPump == PUMP_TRANSFER ? "TRANSFER" : "BOOSTER";
  for (int i = 0; i < 3; i++) if (sgPump == NUT_PUMP_BIT[i]) return i == 0 ? "NUT_A" : i == 1 ? "NUT_B" : "NUT_C";
  return "?";
}
// The rate clock starts with the pump (the AC pumps are always primed: no lag) -- or restarts from `doneL` after a
// selector re-seat, where the other pump's water was measured out.
static void rateClockStart(float doneL, unsigned long now) {
  sgRateBaseL = doneL; sgRateT0 = now;
  sgRateUse = sgRateLpm > 0.0f && !sgTimed && !sgLevel;   // doses only (2026-11-17)
}
// The source ran dry or the water stopped (stall / no flow / reservoir empty): the pump is no longer giving its rate,
// so the clock must not count on for the end figure. The stage's levels / meter alone from here.
static void rateClockStop() { sgRateUse = false; }
// What the calibrated rate says this stage has moved by now (-1 = no measured rate, pump off, timed or stopped).
static float rateMovedL(unsigned long now) {
  if (!sgRateUse || !sgPumpOn || sgTimed) return -1.0f;
  if (sgIsAC && !acPumpOn()) return -1.0f;            // a re-seat pause: nothing is pumping
  return sgRateBaseL + sgRateLpm * (float)(now - sgRateT0) / 60000.0f;
}
/* ---- Transfer by run time (2026-11-17 user rule, see XFER_START_L) ---- */
// The rate this run is timed with: the one the fill's first run really gave (two-step fill, 2026-11-18), else the
// saved one.
static float xferQLpm() { return sgXferLpm > 0.0f ? sgXferLpm : xferLpm(); }
static unsigned long xferRunMsFor(float needL) {
  return (unsigned long)(xferRunS(needL, xferQLpm() / 60.0f, xferStartL, XFER_MIN_RUN_S) * 1000.0f);
}
// Litres this fill has given by now: what was in before a re-seat, plus the line since this run's switch-on.
static float xferTimerL(unsigned long now) {
  if (sgRelayOnMs == 0) return sgXferDoneL;
  return sgXferDoneL + xferLitresAt((float)(now - sgRelayOnMs) / 1000.0f, xferQLpm() / 60.0f, xferStartL, XFER_MIN_RUN_S);
}
static bool xferDue(unsigned long now) { return sgRelayOnMs != 0 && now - sgRelayOnMs >= sgXferRunMs; }
// Weak transfer pump: RATE_CHECK_MS after the switch-on, once the line says RATE_CHECK_MIN_L should be in, the mixing
// tank rose by under RATE_WEAK_FRAC of it -> DEGRADED,PUMP_WEAK (once per work order; the run carries on -- the settle
// and the top-up make the litres up). Not in the reservoir's filter zone (a source running dry slows any pump).
static void xferWeakCheck(float mixUpL, float timerL, unsigned long now) {
  if (sgRateChecked || sgRelayOnMs == 0 || now - sgRelayOnMs < RATE_CHECK_MS) return;
  float should = timerL - sgXferDoneL, saw = mixUpL - sgXferDoneL;
  if (should < RATE_CHECK_MIN_L) return;
  sgRateChecked = true;
  if (srcLowRes.counting) return;
  if (!pumpWeak(saw, should, RATE_WEAK_FRAC)) return;
  if (woRateWarned & 1) return;
  woRateWarned |= 1;
  reply(String("DEGRADED,PUMP_WEAK|TRANSFER|levels=") + String(saw, 2) + "|by_rate=" + String(should, 2) +
        "|cal=" + String(xferLpm(), 2) + "|t=" + String((now - sgRelayOnMs) / 1000) + "s");
}
// How much: the stage's own measurement (levels, or a dose's meter) blended with rate x run time (rateBlend).
// Whether water moves at all is never asked of this -- callers judge that on `measuredL` alone.
static float rateBlendNow(float measuredL, unsigned long now) {
  sgLevelsL = measuredL;
  float t = rateMovedL(now);
  if (t < 0.0f) return measuredL;
  bool dose = !sgLevel;
  RateMix m = rateBlend(measuredL, t, RATE_WEIGHT, dose ? RATE_DOSE_MIN_L : RATE_XCHECK_MIN_L,
                        dose ? RATE_DOSE_FRAC : RATE_XCHECK_FRAC);
  return m.L;
}
// The stage's end reading is missing (the level went blind): what was measured last, plus the calibrated rate for
// the time since -- never less than measured, never more than the tank could give or take.
static float rateCarryL(unsigned long now) {
  float L = sgLastMovedL > 0.0f ? sgLastMovedL : 0.0f;
  if (!sgRateUse || !sgPumpOn || sgTimed) return L;
  unsigned long from = sgLastMovedMs ? sgLastMovedMs : sgRateT0;
  if ((long)(sgRateT0 - from) > 0) from = sgRateT0;   // a re-seat since: count from the restart
  float c = L + sgRateLpm * (float)(now - from) / 60000.0f;
  float cap = (sgTankSign < 0) ? sgMixBaseL - mixDeadL : MIXING_TANK_MAX_L - sgMixBaseL;
  if (c > cap) c = cap;
  return c > L ? c : L;
}
// INFO,PUMP_RATE,<pump>|seen=<rate>|cal=<rate|none>|unit=<L/min|mL/min>|moved=..|s=..: the rate this stage's levels
// (or dose meter) really saw -- the LCD rate screen shows it next to the value typed in. A water stage is timed up to
// its last real progress step, so a stage that ended on an empty source does not drag the figure down.
static void rateReport(unsigned long now) {
  if (!sgPumpOn || sgTimed || sgRateT0 == 0) return;
  bool dose = !sgLevel;
  float saw; unsigned long el;
  if (dose) { saw = sgLevelsL - sgRateBaseL; el = now - sgRateT0; }
  else      { saw = lsMoved(ls, ls.bestL) - sgRateBaseL; el = (unsigned long)(ls.bestMs - sgRateT0); }
  if ((long)el <= 0) return;
  float lpm = measuredLpm(saw, el, dose ? 5000UL : RATE_REPORT_MIN_MS);
  if (lpm <= 0.0f) return;
  float k = dose ? 1000.0f : 1.0f;                  // a dose in mL/min
  reply(String("INFO,PUMP_RATE,") + sgPumpName() + "|seen=" + String(lpm * k, dose ? 1 : 2) +
        "|cal=" + (sgRateLpm > 0.0f ? String(sgRateLpm * k, dose ? 1 : 2) : String("none")) +
        "|unit=" + (dose ? "mL/min" : "L/min") + "|moved=" + String(saw * k, dose ? 1 : 2) + "|s=" + String(el / 1000));
}

// The running dose's meter in litres, with noise bursts left out (see METER_WIN_MS). Pulses inside the open window
// count provisionally while they are within what the pump could give so far, so nothing lags behind the water.
static float doseMeterL(unsigned long now) {
  noInterrupts(); unsigned long raw = flowPulses; interrupts();
  if (raw < sgMeterRaw) sgMeterRaw = raw;                 // the counter was reset (a new attach)
  const float maxLpm = RATE_MLPM_MAX / 1000.0f;           // the fastest pump ESP2 accepts at all
  unsigned long win = now - sgMeterWinMs;
  unsigned long d = raw - sgMeterRaw;
  if (win >= METER_WIN_MS) {
    uint32_t lim = burstMaxPulses(sgK, maxLpm, (uint32_t)win);
    if (d > lim) {
      sgMeterDropped += d;
      if (!sgNoiseWarned) {
        sgNoiseWarned = true;
        reply(String("DEGRADED,FLOW_NOISE|") + sgPumpName() + "|dropped=" + String(d) + "|in_ms=" + String(win) +
              "|max=" + String(lim) + "|glitches=" + String(flowGlitches));
      }
    } else sgMeterGood += d;
    sgMeterRaw = raw; sgMeterWinMs = now;
    d = 0;
  } else if (d > burstMaxPulses(sgK, maxLpm, (uint32_t)(win > 0 ? win : 1))) d = 0;   // judged when the window closes
  return pulsesToLiters(sgMeterGood + d, sgK);
}

// Litres moved so far by the running stage (the progress bar): the last measured-and-blended figure (stagePoll keeps
// it current), or, on a timed stage, estimated.
float stageMovedL() {
  if (!sgLevel) return sgTimed ? litersSoFar(sgK) : rateBlendNow(doseMeterL(millis()), millis());
  if (sgXferTimed && !sgXferStopped) return sgPumpOn ? xferTimerL(millis()) : sgXferDoneL;   // a fill: its run time
  if (sgTimed) return sgTimedBaseL + timedLiters(sgTarget - sgTimedBaseL, sgTimedLpm, millis() - sgTimedT0);
  return sgLastMovedL > 0.0f ? sgLastMovedL : 0.0f;
}

// pumpBit: the DC relay bit for a dose stage, or PUMP_TRANSFER / PUMP_BOOSTER for an AC stage. Callers
// open the stage's valves BEFORE this, so an AC pump never starts against a closed line.
void stageBegin(int pumpBit, bool isAC, int flowPin, float k, float targetL) {
  sgPump = pumpBit; sgIsAC = isAC; sgFlow = flowPin; sgK = k; sgTarget = targetL;
  sgTankSign = 0;                // default: stage does not move tank water (set by caller for fill/deliver)
  slReset(srcLowRes); slReset(srcLowMix); ewReset(resWin);   // a new stage starts with no source watch running
  sgFailWhy = "";                // no reason carried over from the previous stage
  sgCapMs = STAGE_MAX_MS;        // default cap (dose stages override with the per-dose ceiling)
  sgPumpOn = false; sgSawFlow = false; sgT0 = millis();
  sgTimed = false; sgTimedMs = 0;   // default: metered; a timed-recovery stage sets these after this call
  sgLevel = false;
  sgRateLpm = 0.0f; sgRateUse = false; sgRateBaseL = 0.0f; sgRateT0 = 0;   // the caller sets the pump's rate
  sgLevelsL = 0.0f; sgRateChecked = false; sgLastMovedMs = 0;
  sgPaceWarned = false; sgNoiseWarned = false;
  sgXferTimed = false; sgXferStopped = false; sgRelayOnMs = 0; sgXferRunMs = 0; sgXferDoneL = 0.0f;
  sgXferLpm = 0.0f; sgXferCutL = -1.0f; sgXferCut = false; sgCutHits = 0; sgCutSeenN = 0; sgXferReplanned = false;
  sgFloorSinceMs = 0;
  sgFillBlind = false;
  sgMeterRaw = 0; sgMeterGood = 0; sgMeterDropped = 0; sgMeterWinMs = millis();
  acNoCurrentSince = 0;
  if (isAC) acPumpStart(pumpBit);   // inverter off -> AC dead -> servo -> inverter on (acPumpTick)
}

// An AC water stage, metered on the mixing-tank level (web v.4). dir +1 = fill (transfer), -1 = drain
// (booster). baseL = the settled level it starts from (or the bookkeeping, for a timed recovery).
void levelStageBegin(int pump, int8_t dir, float targetL, float baseL, bool baseMeasured) {
  stageBegin(pump, true, -1, 1.0f, targetL);
  sgLevel = true; sgTankSign = dir;
  sgBaseMeasured = baseMeasured;
  sgMixBaseL = baseL;
  // A drain's progress is the mixing tank's fall; a fill's is the transferred estimate, which starts at 0.
  lsBegin(ls, dir, dir > 0 ? 0.0f : baseL, millis());
  sgResBaseL = -1.0f; sgResInBaseL = -1.0f; sgResLost = false;
  sgLastMovedL = 0.0f; sgTimedBaseL = 0.0f; sgTimedT0 = millis();
  // Shown as cal= with INFO,PUMP_RATE. Water stages never blend it in (2026-11-17): a fill is timed by the transfer's
  // rate (xferRunMsFor), a delivery is the mixing tank's own fall.
  sgRateLpm = (pump == PUMP_TRANSFER) ? xferLpm() : boostLpm();
}

// End a FILL/DELIVER (or dose) stage, crediting what it moved: to the persisted tank volume and, for a
// DELIVER, to the per-order water-to-column tally. Works for a normal end and for a mid-stage fault.
// Level stages (web v.4): the tank figure becomes the MEASURED level when there is one; the litres moved
// are measured from the settled baseline. Only a timed stage (or a level lost at the very end) estimates.
float endMeteredStage() {
  float liters;
  if (sgLevel) {
    bool fresh = mixLevelFresh();
    bool fill = (sgTankSign > 0);
    float resDown = -1.0f, mixUp = -1.0f, inL = 0.0f;
    unsigned long nowMs = millis();
    if (fill && sgXferTimed) {
      // A FILL (2026-11-17 user rule): the transfer pump's run time is the measure -- the settle that follows re-measures
      // the tank on its ultrasonic. A run whose water stopped (no level change, a stall, an empty reservoir) is credited
      // with the mixing tank's own rise instead: the clock's litres never came.
      // A run ended early at its cut level (two-step fill, 2026-11-18) is credited with that rise too: the pump ran
      // faster than the clock assumes, so the clock's figure would be short.
      bool byLevel = sgXferStopped || sgXferCut;
      if (byLevel) { if (fresh) { sgLastMovedL = lfMix.med - sgMixBaseL; sgLastMovedMs = nowMs; } }
      else if (sgPumpOn) { sgLastMovedL = xferTimerL(nowMs); sgLastMovedMs = nowMs; }
      if (sgTimed || (byLevel && !fresh)) woEstimated = true;
      if (sgPumpOn) fillEstimate(&resDown, &mixUp, &inL);       // the two tanks, for the log only
      liters = sgLastMovedL > 0.0f ? sgLastMovedL : 0.0f;
      if (!sgTimed) rateReport(nowMs);
    } else if (sgTimed) {
      liters = stageMovedL();
      woEstimated = true;                       // this order's water total is no longer purely measured
    } else {
      // A DELIVERY: the mixing tank's own fall (2026-11-17: the booster's rate is no measure of the litres -- the drip
      // pipe slows it). No end reading: the last measured figure, marked EST.
      if (fresh) { sgLastMovedL = lsMoved(ls, lfMix.med); sgLastMovedMs = nowMs; }
      else woEstimated = true;
      liters = sgLastMovedL > 0.0f ? sgLastMovedL : 0.0f;
      rateReport(nowMs);
    }
    float cur = fresh ? lfMix.med : -1.0f;
    lastStageDiag = String("moved=") + String(liters, 2) + "|level=" + (fresh ? String(cur, 2) : String("blind")) +
                    "|base=" + String(sgMixBaseL, 2) + "|bad_lines=" + String(lvBad) + "|lost_lines=" + String(lvGap);
    if (fill && sgXferTimed) {
      lastStageDiag += String("|by_time=") + (sgXferStopped ? String("stopped") : sgXferCut ? String("cut") : String(liters, 2));
      if (mixUp != -1.0f) lastStageDiag += "|mix=+" + String(mixUp, 2);
      if (resDown != -1.0f) lastStageDiag += "|res=-" + String(resDown, 2);
    }
    if (sgTankSign < 0) woWaterL += liters;      // mix -> column
    if (fill && sgXferTimed && !sgXferStopped && !sgXferCut && sgBaseMeasured) {
      // The run time's figure: the settled level it started from plus what the line gives. The mixing tank still has the
      // inflow's waves on it; the settle that follows (fillStep) replaces this with a measured level.
      mixTankL = sgMixBaseL;
      tankApply(+liters);                       // base + litres, clamped like every other credit
    } else if (fresh) {
      mixTankL = cur;                           // the measured truth, even after a timed stage (clamped like tankApply)
      if (mixTankL > MIXING_TANK_MAX_L) mixTankL = MIXING_TANK_MAX_L;
      tankSave();
    } else {
      tankApply(sgTankSign > 0 ? +liters : -liters);   // from the last known figure
    }
    // Fill log: the run time's litres beside the mixing tank's rise and the reservoir's drop (the reservoir is refilled
    // during a transfer, so its drop is shown, never judged).
    if (fill && sgXferTimed && !sgTimed && liters >= XCHECK_MIN_L) {
      String x = String("by_time=+") + String(liters, 2) + "|mix=+" + String(mixUp, 2) + "|res=-" + String(resDown, 2);
      if (inL > 0.05f) x += "|res_in=+" + String(inL, 2);
      reply("INFO,LEVEL_XCHECK," + x);
    }
    sgLevel = false;
  } else {
    // Dose stage (DC nutrient pump, flow-metered, blended with the pump's measured mL/min while the two agree).
    // A timed stage credits an estimate from its run time.
    liters = sgTimed ? timedLiters(sgTarget, sgTimedLpm, millis() - sgT0) : rateBlendNow(doseMeterL(millis()), millis());
    if (sgTimed) woEstimated = true;
    else rateReport(millis());
    if      (sgTankSign > 0) tankApply(+liters);
    else if (sgTankSign < 0) { tankApply(-liters); woWaterL += liters; }
  }
  stageEnd();
  return liters;
}
// Keep the litres of a stage that is about to be cut short (STOP_ALL, RESET_SELF). Pump not on yet =
// nothing moved, and flowPulses would still hold the previous stage's count.
void creditRunningStage() {
  if (step != SEQ_NONE && sgPump >= 0 && sgPumpOn) endMeteredStage();
}
/* Returns 1 = target reached, 0 = running, -1 = no water moving (no level change / stall with the source
 *            still full / cap) or, on a dose stage, no flow / cap,
 *        -2 = AC pump could not be started (servo not confirmed or AC still live; the pump never ran),
 *        -3 = filling and the mixing tank reached the overflow limit (LEVEL_HIGH),
 *        -4 = filling with no level reading (LEVEL_BLIND),
 *        -5 = fill/drain: the source tank is empty (fill: the mixing tank stopped rising -- a stall, or a reservoir-
 *             empty window; drain: the booster is sucking air),
 *        -6 = drain: the mixing tank read under MIX_LOW_STOP_PCT for MIX_LOW_STOP_MS (the low stop: complete).
 * sgFailWhy (declared with the stage globals) says which rule fired (folded into the fault text).      */
// The floor a drain empties to: the level the booster can no longer reach (learned), plus a margin.
static float drainFloorL() { return mixDeadL + MIX_EMPTY_MARGIN_L; }
// The level a delivery is expected to end at: the booster's floor, or the heel a low stop leaves, whichever is
// higher. Fills plan their batch ABOVE it, so the column still gets its full litres (2026-11-16).
static float planFloorL() { return mixHeelL > mixDeadL ? mixHeelL : mixDeadL; }

// A delivery's windows (2026-11-17): it may drain DRAIN_SLOW_X times slower than the booster's rate (the drip pipe),
// so "no level change" and "stalled" wait for the time such a flow needs to show LVL_MOVED_MIN_L / LVL_STALL_STEP_L.
static unsigned long drainWindowMs(float stepL, unsigned long minMs) {
  float slow = boostLpm() * DRAIN_SLOW_X;
  if (!(slow > 0.0f)) return minMs;
  unsigned long ms = (unsigned long)(stepL / slow * 60000.0f * 1.5f);
  return ms > minMs ? ms : minMs;
}
static unsigned long drainNoFlowMs() { return drainWindowMs(LVL_MOVED_MIN_L, LVL_NOFLOW_MS); }
static unsigned long drainStallMs()  { return drainWindowMs(LVL_STALL_STEP_L, LVL_STALL_MS); }

// Rule 2's guard (2026-11-17): a level "done" faster than LEVEL_PACE_X times the pump's rate is a glitch. Reported
// once; the stage runs on, and the no-flow / stall / cap rules still apply.
static bool levelPaceOk(float levelsL, unsigned long now, float lpm, const char *what) {
  unsigned long el = now - sgT0;
  if (paceOk(levelsL, (uint32_t)el, lpm, LEVEL_PACE_X, LEVEL_PACE_SLACK_L)) return true;
  if (!sgPaceWarned) {
    sgPaceWarned = true;
    reply(String("DEGRADED,LEVEL_JUMP|") + what + "|moved=" + String(levelsL, 2) + "|s=" + String(el / 1000.0f, 1) +
          "|max_lpm=" + String(lpm * LEVEL_PACE_X, 1));
  }
  return false;
}

int stagePoll() {
  unsigned long now = millis();
  if (!sgPumpOn) {
    if (sgIsAC) {                           // AC: the pump runs the moment acPumpTick turns the inverter on
      if (acPumpFailed()) return -2;
      if (!acPumpOn()) return 0;
      sgPumpOn = true; sgT0 = now; sgTimedT0 = now; sgFlowT0 = now;
      // A timed stage's clock counts from the inverter switch-on, like every timed AC run (user, 2026-11-17).
      if (apRelayOnMs && now - apRelayOnMs < 60000UL) sgTimedT0 = apRelayOnMs;
      sgApGen = apGen;
      rateClockStart(0.0f, now);            // always primed (pumps below the tanks): the rate counts from now
      if (sgLevel) {
        ls.bestMs = now;                    // the stall clock starts with the pump, not with the stage
        // The reservoir as the pump comes on: logged beside a fill, never its measure (it is refilled meanwhile).
        sgResBaseL = resLevelFresh() ? lfRes.med : -1.0f;
        sgResInBaseL = lvResInL;
        // A fill is timed from the inverter SWITCH-ON, its start-up included -- the way the transfer was measured.
        if (sgXferTimed) sgRelayOnMs = (apRelayOnMs && now - apRelayOnMs < 60000UL) ? apRelayOnMs : now;
      }
    } else if (now - sgT0 >= VALVE_SWITCH_MS) {   // DC: valve settle, then start pump
      attachFlow(sgFlow); pcfOn(sgPump); sgPumpOn = true; sgT0 = now;
      sgMeterRaw = 0; sgMeterGood = 0; sgMeterDropped = 0; sgMeterWinMs = now;
      rateClockStart(0.0f, now);
    }
    return 0;
  }
  if (sgIsAC) {
    // Stuck-selector re-seat (acPump layer): the wrong pump was running, so the inverter is off while the
    // servo swings. The stage just waits -- no timeouts, no level verdicts -- and when the pump is back on,
    // every clock is shifted by the pause so the cap, the no-flow timeout and a timed stage exclude it.
    // A level-metered stage is then measured on from the level at the restart (FILL_REPLAN / DRAIN_REPLAN below).
    if (acPumpFailed()) return -2;          // re-seats used up: STUCK
    if (!acPumpOn()) return 0;
    if (apGen != sgApGen) {
      sgApGen = apGen;
      unsigned long pause = now - apPausedMs;
      sgT0 += pause; sgTimedT0 += pause; sgRateT0 += pause;
      lsShift(ls, pause);
      // The pump was off: the source watches start again, and the no-flow clock restarts -- the RIGHT pump now
      // gets its full LVL_NOFLOW_MS to show water moving.
      slReset(srcLowRes); slReset(srcLowMix); ewReset(resWin);
      sgFlowT0 = now;
      // A FILL whose selector stuck (2026-11-17 rework): the booster ran instead, so the run time so far says nothing.
      // Measure the mixing tank now, aim for the same final level, and time the rest from this switch-on (the start-up
      // is paid again). Without a level the plan is kept and timed again from the new switch-on.
      if (sgLevel && sgTankSign > 0 && sgXferTimed) {
        float level = mixLevelFresh() ? lfMix.med : sgMixBaseL + sgXferDoneL;   // where the tank stands now
        float done = level - sgMixBaseL;
        if (done < 0.0f) done = 0.0f;
        float left = sgXferGoalL - level;                // from the real level: the wrong pump may have lowered it
        float room = mixCapL - level;
        if (left > room) left = room;
        if (left < 0.0f) left = 0.0f;
        sgXferDoneL = done; sgXferNeedL = left; sgXferReplanned = true;
        sgRelayOnMs = (apRelayOnMs && now - apRelayOnMs < 60000UL) ? apRelayOnMs : now;
        sgXferRunMs = (left >= TANK_EPS_L) ? xferRunMsFor(left) : 0;   // nothing left: it stops at once
        lsBegin(ls, +1, done, now);                      // moving or not: judged afresh, for the right pump
        reply("INFO,FILL_REPLAN|done=" + String(done, 2) + "|left=" + String(left, 2) +
              "|run_s=" + String(sgXferRunMs / 1000.0f, 1));
      }
      // A DELIVERY whose selector stuck (2026-11-17 bug check): the TRANSFER pump ran instead and topped the mixing
      // tank up from the reservoir. Measured from the old baseline, the booster would first have to drain that extra
      // water before any progress showed -- ~2 L at 6 L/min is the whole LVL_NOFLOW_MS, so a healthy booster could be
      // held as FLOW_FAIL -- and the extra water it then delivers would never be counted. Keep the litres delivered so
      // far and measure the rest from the level now (it all goes to the column; the stage still drains to the floor).
      if (sgLevel && sgTankSign < 0 && !sgTimed && mixLevelFresh()) {
        float done = sgLastMovedL > 0.0f ? sgLastMovedL : 0.0f;
        float left = lfMix.med - planFloorL();
        if (left < 0.0f) left = 0.0f;
        sgMixBaseL = lfMix.med + done;
        lsBegin(ls, -1, sgMixBaseL, now);                // progress (and the stall/no-flow rules) from here
        sgTarget = done + left;
        rateClockStart(done, now);
        reply("INFO,DRAIN_REPLAN|done=" + String(done, 2) + "|left=" + String(left, 2));
      }
    }
    if (lgHighHit) return -3;               // only ever set while filling (lgSample)
    // Delivery low stop: the mixing tank under MIX_LOW_STOP_PCT for MIX_LOW_STOP_MS -> complete (user rule).
    if (sgLevel && sgTankSign < 0 && slDue(srcLowMix, now, MIX_LOW_STOP_MS) &&
        levelPaceOk(sgLastMovedL, now, BOOSTER_OPEN_LPM, "DRAIN")) { sgFailWhy = "MIX_LOW"; return -6; }
  }
  if (!sgLevel) {
    // ---- dose stage: DC nutrient pump, flow-metered; how much = the meter blended with the pump's measured
    // mL/min while the two agree. "Is it flowing" stays on the meter alone: a dead meter (or a dead pump) still
    // ends as DOSE_TIMEOUT -- the clock never doses on its own.
    if (sgTimed) {
      unsigned long el = now - sgT0;
      if (el >= sgTimedMs || el > TIMED_STAGE_CAP_MS) return 1;
      return 0;
    }
    float meterL = doseMeterL(now);                     // noise bursts left out (2026-11-17)
    if (meterL > 0.0005f) sgSawFlow = true;
    float L = rateBlendNow(meterL, now);
    if (L >= sgTarget) return 1;
    unsigned long el = now - sgT0;
    if (!sgSawFlow && el > FLOW_TIMEOUT_MS) return -1;   // dry run / no flow
    if (el > sgCapMs) return -1;                         // per-dose ceiling
    return 0;
  }

  // ---- level-metered AC stage (web v.4) ----
  // A fill stops at its run time, counted from the inverter switch-on, whatever the level reading is doing at that
  // moment: a gap in the LEVEL lines must never let the transfer run on past it (2026-11-17).
  if (sgTankSign > 0 && sgXferTimed && !sgXferStopped && xferDue(now)) {
    sgLastMovedL = xferTimerL(now); sgLastMovedMs = now;
    return 1;
  }
  bool fresh = mixLevelFresh();
  float cur = fresh ? lfMix.med : -1.0f;
  // Blind: no accepted sample for LEVEL_BLIND_MS, judged only after the start grace (rule 5).
  bool blind = !fresh && (now - sgT0 >= LEVEL_GRACE_MS) &&
               (lfMix.lastMs == 0 || now - lfMix.lastMs >= LEVEL_BLIND_MS);
  if (sgTimed) {
    // Timed: a level-blind recovery stage, or a drain that went blind mid-way. Completes on the clock and
    // never raises a no-flow fault (PZEM power validation still runs). If the level is back, a drain still
    // stops at the floor -- the booster never runs dry longer than it has to. A fill is timed like every fill.
    if (sgTankSign > 0 && sgXferTimed) {
      sgLastMovedL = xferTimerL(now); sgLastMovedMs = now;
      return (xferDue(now) || now - sgTimedT0 > TIMED_STAGE_CAP_MS) ? 1 : 0;
    }
    if (sgTankSign < 0 && fresh && cur <= drainFloorL()) return 1;
    unsigned long el = now - sgTimedT0;
    if (el >= sgTimedMs || el > TIMED_STAGE_CAP_MS) return 1;
    return 0;
  }
  if (blind) {
    if (sgTankSign > 0 && !sgXferTimed) { sgFailWhy = "BLIND"; return -4; }   // (a fill is always timed since 2026-11-17)
    if (sgTankSign > 0) {
      // FILL blind (2026-11-17): carry on with the run time, capped to leave FILL_BLIND_MARGIN of the room free.
      if (!sgFillBlind) {
        sgFillBlind = true;
        float room = mixCapL - sgMixBaseL;
        float allowedL = room * (1.0f - FILL_BLIND_MARGIN) - sgXferDoneL;   // litres this run may still give from its switch-on
        unsigned long capMs = (allowedL > 0.0f) ? xferRunMsFor(allowedL) : 0;
        if (allowedL <= 0.0f || capMs < sgXferRunMs) sgXferRunMs = (allowedL > 0.0f) ? capMs : 0;
        reply("DEGRADED,LEVEL_BLIND|FILL|base=" + String(sgMixBaseL, 2) + "|run_s=" + String(sgXferRunMs / 1000.0f, 1) +
              "|bad_lines=" + String(lvBad) + "|lost_lines=" + String(lvGap) + "|spikes=" + String(lfMix.rejects));
      }
      sgLastMovedL = xferTimerL(now); sgLastMovedMs = now;
      return xferDue(now) ? 1 : 0;
    }
    // Draining blind: finish the litres that were left on a timer (estimated, marked EST). What went out in the
    // LEVEL_BLIND_MS since the last reading is counted at the booster's measured rate (if it has one).
    sgTimedBaseL = rateCarryL(now);         // measured so far, plus the measured rate since the last reading
    float left = (sgMixBaseL - sgTimedBaseL) - drainFloorL();
    if (left < 0.0f) left = 0.0f;
    sgTimed = true; sgTimedLpm = boostLpm();   // the operator's measured rate (or the default)
    sgTarget = sgTimedBaseL + left;
    sgTimedT0 = now;
    sgTimedMs = (unsigned long)(left / sgTimedLpm * 60000.0f * TIMED_MARGIN);
    if (sgTimedMs > TIMED_STAGE_CAP_MS) sgTimedMs = TIMED_STAGE_CAP_MS;
    woEstimated = true;
    reply("DEGRADED,LEVEL_BLIND|DRAIN|left=" + String(left, 2) + "|timed_ms=" + String(sgTimedMs));
    return 0;
  }
  if (!fresh) return 0;                     // inside the grace / a short gap: wait for the next sample
  float levelsL;
  if (sgTankSign > 0) {
    // FILL (2026-11-17 user rule). HOW MUCH = the transfer pump's run time from the switch-on (the basis); the mixing
    // tank's own rise only says whether water is moving (no flow / stall / weak pump), and the settle after the run
    // measures it. The reservoir is no measure (it is refilled meanwhile); its level still tells an EMPTY reservoir:
    // in its filter zone, a RES_EMPTY_WIN_MS window in which the mixing tank did not rise.
    levelsL = cur - sgMixBaseL;
    if (ewTick(resWin, srcLowRes.counting, cur, now, RES_EMPTY_WIN_MS, RES_EMPTY_RISE_L)) {
      sgFailWhy = "RES_EMPTY_WINDOW";
      sgXferStopped = true; sgLastMovedL = levelsL;            // nothing came in: the run time is no measure now
      return -5;
    }
    lsUpdate(ls, levelsL, now, LVL_STALL_STEP_L, LVL_MOVED_MIN_L);
    sgLevelsL = levelsL;
    float byTime = xferTimerL(now);
    sgLastMovedL = byTime; sgLastMovedMs = now;
    xferWeakCheck(levelsL, byTime, now);
    if (xferDue(now)) return 1;                                // rule 2: the run time is reached
    // Two-step fill (2026-11-18): the live level reached this run's cut level before its run time -- the pump is faster
    // than the rate it was timed with. A normal end: the settle measures it and a top-up makes up the rest. Judged
    // once per new accepted sample, FILL_CUT_SAMPLES in a row, at a believable pace.
    if (sgXferCutL > 0.0f && lfMix.count != sgCutSeenN) {
      sgCutSeenN = lfMix.count;
      sgCutHits = (cur >= sgXferCutL) ? (uint8_t)(sgCutHits < 255 ? sgCutHits + 1 : 255) : 0;
      if (sgCutHits >= FILL_CUT_SAMPLES &&
          levelPaceOk(levelsL, now, xferQLpm() > xferLpm() ? xferQLpm() : xferLpm(), "FILL")) {
        sgXferCut = true;
        sgLastMovedL = levelsL; sgLastMovedMs = now;
        reply("INFO,FILL_CUT|level=" + String(cur, 2) + "|cut=" + String(sgXferCutL, 2) + "|t=" +
              String((now - sgRelayOnMs) / 1000.0f, 1) + "|run_s=" + String(sgXferRunMs / 1000.0f, 1) +
              "|by_time=" + String(byTime, 2));
        return 1;
      }
    }
  } else {
    // DELIVERY: the mixing tank's own fall is the measure. Rule 2 (2026-11-17 user rule): under MIX_LOW_STOP_PCT the
    // booster runs MIX_LOW_STOP_MS more (the low stop above); a tank its booster cannot get that low ends the same way,
    // MIX_LOW_STOP_MS after the level reached the learned floor.
    lsUpdate(ls, cur, now, LVL_STALL_STEP_L, LVL_MOVED_MIN_L);
    levelsL = lsMoved(ls, cur);
    sgLevelsL = levelsL;
    sgLastMovedL = levelsL; sgLastMovedMs = now;
    if (cur <= drainFloorL()) { if (sgFloorSinceMs == 0) sgFloorSinceMs = nzMs(now); }
    else if (cur > drainFloorL() + MIX_EMPTY_MARGIN_L) sgFloorSinceMs = 0;
    if (sgFloorSinceMs && now - sgFloorSinceMs >= MIX_LOW_STOP_MS &&
        levelPaceOk(levelsL, now, BOOSTER_OPEN_LPM, "DRAIN")) { sgFailWhy = "FLOOR"; return -6; }
  }
  unsigned long el = now - sgT0;
  if (lsNoFlow(ls, now - sgFlowT0, sgTankSign < 0 ? drainNoFlowMs() : LVL_NOFLOW_MS)) {   // rule 3 (from the last pump-on)
    rateClockStop(); sgXferStopped = true;
    sgLastMovedL = levelsL;                                   // nothing moved: the clock's litres never happened
    sgFailWhy = "NO_LEVEL_CHANGE|moved=" + String(levelsL, 2);
    return -1;
  }
  if (lsStalled(ls, now, sgTankSign < 0 ? drainStallMs() : LVL_STALL_MS)) {   // rule 4
    rateClockStop(); sgXferStopped = true;                    // the water stopped: the clock must not count on
    sgLastMovedL = levelsL;
    if (sgTankSign > 0 && resLevelFresh() && lvResPct > RES_FILTER_PCT) {
      // The transfer stopped (neither tank moving) but the reservoir still holds water: not a dry source. The pump
      // sits below the reservoir and cannot lose its prime, so the line or its filter is blocked, a valve is shut,
      // or the pump itself failed. Hold for the operator.
      sgFailWhy = "STALL|res=" + String(lvResPct, 0) + "%|moved=" + String(levelsL, 2);
      return -1;
    }
    if (sgTankSign < 0 && levelsL < DRAIN_STALL_DONE_FRAC * sgTarget && cur - drainFloorL() > MIX_DEAD_MAX_L) {
      // The delivery stopped with most of the batch still in the mixing tank: the booster is not sucking air, the
      // line, a column valve or the booster is the problem. Hold -- finishing "OK" would leave the column unwatered
      // (the web v.3 log of 17 Nov 08:01: a delivery reported done while nothing reached the field).
      sgFailWhy = "STALL|left=" + String(cur - drainFloorL(), 2) + "|moved=" + String(levelsL, 2);
      return -1;
    }
    sgFailWhy = "STALL";
    return -5;
  }
  if (el > sgCapMs) {                                         // far longer than the pump's rate allows: not its rate
    rateClockStop(); sgXferStopped = true; sgLastMovedL = levelsL;
    sgFailWhy = "CAP|moved=" + String(levelsL, 2);
    return -1;
  }
  return 0;
}
void stageEnd() {
  if (sgPump >= 0) {
    if (sgIsAC) acPumpStop();               // inverter off (the servo stays where it is -- dead switch)
    else        pcfOff(sgPump);
  }
  detachFlow();
  sgPump = -1; sgPumpOn = false;
}

// Shared outcome for an AC stage that cannot continue on its own terms:
//   -2 the pump never started (selector / AC-dead check failed) -> nothing moved, so NOTHING is credited;
//   -4 filling blind -> credit what the level showed before it went blind.
static void acStageHold(int r, const char *where) {
  if (r == -4) {
    endMeteredStage();
    String loc = String(where) + "|" + sgFailWhy;
    holdFault("LEVEL_BLIND", loc.c_str());
    return;
  }
  String loc = String(where) + "|" + apErr;   // read before the stage end below clears the pump state
  // A selector that stuck AFTER the pump had run (STUCK, from a re-seat) moved real water: the measured
  // level is credited. A pump that never started has nothing to credit.
  if (sgPumpOn) endMeteredStage();
  holdFault(apFailType(), loc.c_str());   // BATT_SWITCH_FAIL when the battery switch, not the servo, failed
}
// -1 on a level stage: no water moving (rule 3), a fill that stalled with water still in the reservoir, or
// the stage cap. Credit the measured level, then hold FLOW_FAIL (the wire code ESP1 already handles).
static void levelFlowFail(const char *where) {
  String loc = String(where) + "|" + sgFailWhy;
  endMeteredStage();
  holdFault("FLOW_FAIL", loc.c_str());
}
// -3: the mixing tank hit the overflow limit while filling. Keep the measured litres, alert, carry on.
static void fillStoppedHigh() {
  float pct = lg.lastPct, tgt = sgTarget;
  float L = endMeteredStage();
  reply("LEVEL_HIGH,MIX|pct=" + String(pct, 0) + "|metered=" + String(L, 2) + "|target=" + String(tgt, 2));
}
// -5 on a FILL: the mixing tank stopped rising and the reservoir reads empty (or cannot be read). Keep what
// came in, alert, carry on with the smaller batch (user decision 2026-11-15).
// -6 on a DELIVER (2026-11-16 user rule): the mixing tank read under MIX_LOW_STOP_PCT for MIX_LOW_STOP_MS. The delivery
// is COMPLETE even with a little mixture left. What is left is measured (kept in the tank figure) and remembered as
// the heel (mixHeelL), so the next fill plans its batch above it and the column still gets its full litres.
static void drainStoppedLow(const char *where) {
  float expected = sgTarget, pct = srcLowMix.lastPct;
  float L = endMeteredStage();
  float left = mixTankL;
  if (left <= MIX_DEAD_MAX_L && fabsf(left - mixHeelL) > 0.05f) { mixHeelL = left; prefs.putFloat("mixHeel", mixHeelL); }
  reply(String("INFO,MIX_LOW_STOP,") + where + "|mix=" + String(pct, 0) + "%|delivered=" + String(L, 2) +
        "|expected=" + String(expected, 2) + "|left=" + String(left, 2));
}
static void fillStoppedDry() {
  float tgt = sgTarget;
  String why = sgFailWhy.length() ? sgFailWhy : String("STALL");
  float L = endMeteredStage();
  reply("RES_EMPTY,TRANSFER|metered=" + String(L, 2) + "|target=" + String(tgt, 2) +
        "|res=" + (resLevelFresh() ? String(lvResPct, 0) + "%" : String("blind")) + "|by=" + why);
}
// -5 on a DELIVER: the level stopped falling -- the booster is sucking air, so the tank is empty FOR IT. The
// water left is real and stays in the tank (the measured level is kept, so the next fill adds less); it is
// learned as the pump's floor (mixDeadL), so later deliveries stop there instead of stalling. Logged, not a fault.
static void drainStoppedDry(const char *where) {
  float expected = sgTarget;
  float L = endMeteredStage();
  float left = mixTankL;
  if (left <= MIX_DEAD_MAX_L) {
    if (fabsf(left - mixDeadL) > 0.05f) { mixDeadL = left; prefs.putFloat("mixDead", mixDeadL); }
  } else {
    reply("DEGRADED,MIX_RESIDUAL|left=" + String(left, 2));   // too much left: a blocked intake, not a floor
  }
  reply(String("INFO,MIX_EMPTY,") + where + "|delivered=" + String(L, 2) + "|expected=" + String(expected, 2) +
        "|left=" + String(left, 2));
}

// Rule 1 for every water stage: plan from a settled MEASURED level. 1 = go (base = that level, and the tank
// figure is replaced by it), 0 = still waiting, -1 = no settled level in time -> held as LEVEL_BLIND. A timed
// recovery (resumeTimed) plans from the last known figure instead: the level is what failed.
static int waterStageGate(const char *where, float &base, bool &measured) {
  if (resumeTimed) { base = mixTankL; measured = false; return 1; }
  int g = levelBaseline(base);
  if (g == 0) return 0;
  if (g < 0) {
    lastStageDiag = String("level=") + (mixLevelFresh() ? String(lfMix.med, 2) : String("blind")) +
                    "|bad_lines=" + String(lvBad) + "|lost_lines=" + String(lvGap) + "|spikes=" + String(lfMix.rejects);
    // A delivery cannot overflow anything: plan it from the last known figure and carry on (2026-11-16 hardening).
    // The level -- or, if it stays blind, the timed drain -- still ends it. A fill is never planned blind.
    if (strcmp(where, "TRANSFER") != 0) {
      base = mixTankL; measured = false;
      reply(String("DEGRADED,LEVEL_BASE_EST|") + where + "|" + lastStageDiag + "|using=" + String(mixTankL, 2));
      lastStageDiag = "";
      return 1;
    }
    String loc = String(where) + "|NO_SETTLED_LEVEL";
    holdFault("LEVEL_BLIND", loc.c_str());
    return -1;
  }
  measured = true;
  if (fabsf(base - mixTankL) >= 0.5f)        // the bookkeeping drifted from the tank: say so (log only)
    reply("INFO,TANK_MEASURED," + String(base, 2) + "|was=" + String(mixTankL, 2));
  mixTankL = base; tankSave();
  return 1;
}

void finishOk() {
  stopAllGraceful();                 // all OFF + master cutoff de-energized (after any battery return)
  // web v.4: the tank figure is whatever the ultrasonic measured at the end of the last stage (the water
  // the booster could not reach stays counted). Without a fresh level, keep the bookkeeping -- never zero
  // it on faith, or the next fill would overfill.
  if (mixLevelFresh()) mixTankL = lfMix.med;
  tankSave();
  doseJClear();                      // the batch reached its column
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
  bool isFlow  = (strcmp(resp, "FLOW_FAIL") == 0);
  bool isBlind = (strcmp(resp, "LEVEL_BLIND") == 0);
  stageEnd();                        // pump off + flow detached (partial liters already credited by caller)
  // web v.4: a water-stage FLOW_FAIL / LEVEL_BLIND carries what the ultrasonic saw (moved / level / base)
  // instead of a pulse count -- moved=0.00 says nothing moved at all (dead pump, closed line, air lock).
  String faultLoc = String(loc);
  if ((isFlow || isBlind) && lastStageDiag.length()) faultLoc += "|" + lastStageDiag;
  faultJSave(resp, wo.colMask, faultLoc);   // before P0 drops: this reply may never get out otherwise
  doseJMarkStopped();                // a held fault waits for the operator, also if a reset ends the hold
  stopAll();                         // all OFF, master cutoff de-energized
  faultHeld = true;
  stepInit  = false;                 // on RESUME the paused step re-inits with the updated tank volume
  // Audit fix (2026-09-09): unconditionally overwriting this erased the "flow sensor is dead"
  // diagnosis if an unrelated fault (e.g. PWR_FAIL) occurred while already running a flow-blind
  // (resumeTimed) recovery -- the next RESUME would then retry a flow-metered stage against a
  // sensor that's still dead, immediately re-raise FLOW_FAIL, and self-correct, but that's one
  // avoidable extra operator RESUME cycle. Keep the prior true value across a same-hold, non-flow
  // fault instead of clobbering it.
  // Lost water metering (no level change, or no level at all) enables the timed IRRIGATE/RELEASE recovery.
  lastHoldFlow = isFlow || isBlind || (resumeTimed && lastHoldFlow);
  lastStageDiag = "";
  heldReply = String(resp) + "," + faultLoc; heldReplyMs = millis();   // re-asserted while held (heartbeat)
  reply(String(resp) + "," + faultLoc);
}

// User-gated recovery (sec.19.4.8.2). NORMAL resumes the paused step; IRRIGATE finishes as
// irrigation-only (top up plain water to the column budget, no dosing); RELEASE dumps the
// current tank contents to the assigned column as-is. All re-energize P17 first.
void resumeWork(const String &mode) {
  faultHeld = false;
  cutoffEnergize();
  // Level metering lost + user chose a deliver/top-up recovery -> run those stages on a timer (level-blind),
  // otherwise they would re-detect it and re-hold forever. Measured recovery stays for other faults.
  resumeTimed = (lastHoldFlow && (mode == "IRRIGATE" || mode == "RELEASE"));
  if (mode == "IRRIGATE") {
    wo.fertigate = false; wo.flushPct = 0;     // top-up FILL to budget, then DELIVER, no flush/dosing
    goStep(SEQ_FILL);
  } else if (mode == "RELEASE") {
    wo.fertigate = false; wo.flushPct = 0;     // skip fill/dose: just DELIVER whatever is in the tank
    goStep(SEQ_DELIVER);
  } else {                                     // NORMAL: continue the paused sequence as-is
    // Audit fix (2026-09-08): goStep(step) is a no-op on which step is active (s == step already)
    // but also re-sends the STAGE,... notice, matching what the IRRIGATE/RELEASE branches above
    // already do -- previously NORMAL alone left ESP1's run-progress LCD showing stale stage info
    // after a resume.
    goStep(step);                              // re-init the current step with the updated tank volume
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

/* ---- FILL / FLUSH_FILL (2026-11-17 user rule; two steps since 2026-11-18): run time, settle, measure, top up ----- *
 * A transfer run is timed from the inverter switch-on for the litres it should give (xferRunMsFor). Then the inverter
 * is off and the water settles for FILL_SETTLE_MS while the mixing tank is watched; once the reading has held within
 * FILL_STEADY_L for FILL_STEADY_WIN_MS it is the measurement. The first run aims short of the goal and every run may
 * end early at its cut level (TWO-STEP FILL, at FILL_FIRST_FRAC). More than FILL_TOPUP_MIN_L short after a settle:
 * another run for what is missing, timed with the rate the runs really gave (at most FILL_TOPUP_MAX), settled and
 * measured again; otherwise the run carries on.
 * Returns 1 when the fill is done (the caller moves on), 0 while it runs / settles / waits / holds.                */
enum FillPhase : uint8_t { FP_RUN = 0, FP_SETTLE = 1 };
FillPhase     fpPhase = FP_RUN;
float         fpGoalL = 0.0f;          // the mixing-tank level this fill aims for (settled base + batch, capped)
float         fpBase0L = 0.0f;         // the settled level the fill started from (zero on the progress bar)
uint8_t       fpTopups = 0;            // top-up runs so far
unsigned long fpStopMs = 0;            // the last run stopped (the settle clock)
bool          fpNoTopup = false;       // that run ended on the overflow limit or an empty reservoir
bool          fpSplit = false;         // the first run aimed short of the goal (two-step fill)
float         fpRunBaseL = 0.0f;       // the last run: the level it started from,
float         fpRunS = 0.0f;           //   how long it ran from the inverter switch-on (s),
bool          fpRunClean = false;      //   and whether its rise can tell the pump's rate (no re-seat, blind or timed part)
float         fpSeenLpm = 0.0f;        // the rate this fill's runs really gave (0 = not known: the saved rate)
SteadyWatch   fpSteady = { 0, 0.0f, 0.0f, false };

// One transfer run: needL from baseL, timed with qLpm (0 = the saved rate), ended early once the live level reaches
// cutL (<0 = only at its run time).
static void fillRunBegin(float needL, float baseL, bool measured, float qLpm, float cutL) {
  resValveSet(true);
  levelStageBegin(PUMP_TRANSFER, +1, needL, baseL, measured);
  sgXferTimed = true;
  sgXferLpm  = (qLpm > 0.0f) ? qLpm : 0.0f;
  sgXferCutL = resumeTimed ? -1.0f : cutL;       // a level-blind recovery has no level to end on
  sgXferNeedL = needL; sgXferDoneL = 0.0f; sgXferGoalL = baseL + needL;
  sgXferRunMs = xferRunMsFor(needL);
  // The cap stays well past the run time whichever rate times it: sized on the slower of the two.
  sgCapMs = stageCapMs(needL, xferQLpm() < xferLpm() ? xferQLpm() : xferLpm(), STAGE_MAX_MS, STAGE_CAP_MARGIN);
  if (resumeTimed) sgTimed = true;       // level metering lost: no level rules, the run time alone (no margin: it fills)
  fpPhase = FP_RUN;
  fpRunBaseL = baseL; fpRunS = 0.0f; fpRunClean = false;
  reply("INFO,FILL_RUN|need=" + String(needL, 2) + "|run_s=" + String(sgXferRunMs / 1000.0f, 1) + "|q=" +
        String(xferQLpm(), 2) + "|start_l=" + String(xferStartL, 2) + "|base=" + String(baseL, 2) +
        (sgXferCutL > 0.0f ? "|cut=" + String(sgXferCutL, 2) : String("")) +
        (fpTopups ? "|topup=" + String(fpTopups) : String("")));
}
static void fillSettleBegin(bool noTopup) {
  fpPhase = FP_SETTLE; fpStopMs = millis(); fpNoTopup = noTopup;
  swReset(fpSteady);
}
// A fill's progress across all its runs and settles (the PROG line, ESP1's progress bar): litres in since the fill
// began, out of the fill's whole plan. false = no fill under way (the caller keeps the stage's own figures).
static bool fillProgress(float &L, float &T) {
  if ((step != SEQ_FILL && step != SEQ_FLUSH_FILL) || !stepInit) return false;
  float sofar = (fpPhase == FP_RUN && sgPump >= 0 && sgPumpOn) ? (sgMixBaseL - fpBase0L) + stageMovedL()
                                                               : mixTankL - fpBase0L;
  L = sofar > 0.0f ? sofar : 0.0f;
  T = fpGoalL - fpBase0L;
  return true;
}
static int fillStep(int c, bool flush) {
  if (!stepInit) {
    float base; bool measured;
    int g = waterStageGate("TRANSFER", base, measured);
    if (g <= 0) return 0;                          // waiting for a settled level, or held LEVEL_BLIND
    // Operator FORCE runs carry their own TOTAL batch volume; scheduled runs use the table.
    float budget  = (wo.waterL > 0.0f) ? wo.waterL : WATER_BUDGET_L[c];
    float desired = flush ? budget * (wo.flushPct / 100.0f)
                          : (wo.fertigate ? budget * (1.0f - wo.flushPct / 100.0f) : budget);
    float usable  = mixTankL - planFloorL(); if (usable < 0.0f) usable = 0.0f;
    float remaining = desired - usable;
    // Never plan more than the tank holds at its overflow limit (mixCapL, from ESP1's tank calibration). The
    // LEVEL_HIGH stop is the hard guard; this keeps a plan from relying on it.
    float headroom = mixCapL - mixTankL;
    if (remaining > headroom) remaining = headroom;
    if (remaining < TANK_EPS_L) return 1;          // already enough in the tank -> skip the fill
    fpGoalL = mixTankL + remaining;
    fpBase0L = mixTankL;
    fpTopups = 0; fpSeenLpm = 0.0f;
    // Two-step fill (2026-11-18): the first run aims short and ends early on a fast pump; the measured top-up finishes
    // it. A fill too small to split, or a level-blind recovery (it could not measure the first run), is one run that
    // still ends early if the live level gets near the goal first.
    float first = resumeTimed ? remaining : fillFirstL(remaining, FILL_FIRST_FRAC, FILL_TOPUP_MIN_L);
    fpSplit = (first < remaining);
    float cut = fpSplit ? base + remaining * FILL_FIRST_CUT_FRAC
                        : fillCutLevel(fpGoalL, FILL_CUT_LAG_L, base, LVL_MOVED_MIN_L);
    fillRunBegin(first, base, measured, 0.0f, cut);
    stepInit = true;
    return 0;
  }
  if (fpPhase == FP_RUN) {
    int r = stagePoll();
    if (r == 0) return 0;
    resValveSet(false);
    if (r == 1) {                                                      // run time (or cut level) reached: settle, measure
      // The run's own record, for the rate it really gave (worked out once the settle has measured it).
      fpRunS = sgRelayOnMs ? (float)(millis() - sgRelayOnMs) / 1000.0f : 0.0f;
      fpRunClean = !sgTimed && !sgFillBlind && !sgXferReplanned && sgBaseMeasured;
      endMeteredStage(); fillSettleBegin(false);
    }
    else if (r == -3) { fillStoppedHigh(); fillSettleBegin(true); }    // at the overflow limit: measure, never top up
    else if (r == -5) { fillStoppedDry();  fillSettleBegin(true); }    // the reservoir is empty: the same
    else if (r == -1) levelFlowFail("TRANSFER");
    else              acStageHold(r, "TRANSFER");
    return 0;
  }
  // ---- FP_SETTLE: the inverter is off; let the water settle, watching the mixing tank, then measure ----
  unsigned long now = millis();
  bool fresh = mixLevelFresh();
  bool steady = fresh && swSample(fpSteady, lfMix.med, now, FILL_STEADY_L, FILL_STEADY_WIN_MS);
  if (now - fpStopMs < FILL_SETTLE_MS) return 0;   // the user's minute
  if (!steady) {
    if (now - fpStopMs < FILL_SETTLE_MAX_MS) return 0;
    if (!fresh) {
      // No level to check the fill with: carry on with the run time's figure, marked EST. A two-step fill's first run
      // stopped short on purpose, so the rest is run first on the saved rate, as the single run would have (the
      // blind-fill cap still keeps it off the overflow limit).
      woEstimated = true;
      float rest = fpGoalL - mixTankL;
      bool runRest = fpSplit && !fpNoTopup && rest >= TANK_EPS_L && fpTopups < FILL_TOPUP_MAX;
      reply("DEGRADED,FILL_SETTLE_BLIND|goal=" + String(fpGoalL, 2) + "|by_time=" + String(mixTankL, 2) +
            (runRest ? "|rest=" + String(rest, 2) : String("")));
      if (runRest) {
        fpSplit = false; fpTopups++;
        fillRunBegin(rest, mixTankL, false, 0.0f, -1.0f);
        return 0;
      }
      return 1;
    }
    reply("DEGRADED,FILL_NOT_STEADY|level=" + String(lfMix.med, 2) + "|band=" + String(fpSteady.hi - fpSteady.lo, 2));
  }
  float meas = lfMix.med;
  float missing = fpGoalL - meas;
  mixTankL = meas;                                   // the measured truth replaces the run time's figure
  if (mixTankL > MIXING_TANK_MAX_L) mixTankL = MIXING_TANK_MAX_L;
  tankSave();
  // The rate the run really gave (two-step fill): it times the top-up. A short or unclean run keeps the last one known.
  if (fpRunClean) {
    float q = fillSeenLpm(meas - fpRunBaseL, fpRunS, xferStartL, FILL_LEARN_MIN_S, xferLpm(), FILL_LEARN_LO, FILL_LEARN_HI);
    if (q > 0.0f) fpSeenLpm = q;
  }
  bool topup = !fpNoTopup && fillNeedsTopup(fpGoalL, meas, FILL_TOPUP_MIN_L, fpTopups, FILL_TOPUP_MAX);
  reply("INFO,FILL_CHECK|goal=" + String(fpGoalL, 2) + "|measured=" + String(meas, 2) + "|missing=" +
        String(missing, 2) + "|settle_s=" + String((now - fpStopMs) / 1000) + "|" + (topup ? "TOPUP" : "OK") +
        (fpSeenLpm > 0.0f ? "|seen=" + String(fpSeenLpm, 2) : String("")));
  if (topup) {
    float need = missing;
    float headroom = mixCapL - meas;
    if (need > headroom) need = headroom;
    if (need >= TANK_EPS_L) {
      fpTopups++;
      // A new run for what is missing, timed from its own switch-on with the rate seen, ended early near the goal.
      fillRunBegin(need, meas, true, fpSeenLpm, fillCutLevel(meas + need, FILL_CUT_LAG_L, meas, LVL_MOVED_MIN_L));
      return 0;
    }
  }
  if (!fpNoTopup && missing > FILL_SHORT_ALERT_L)     // still short with the top-ups used up: alert, carry on
    reply("DEGRADED,FILL_SHORT|goal=" + String(fpGoalL, 2) + "|measured=" + String(meas, 2) + "|topups=" +
          String(fpTopups));
  return 1;
}

void runSequence() {
  if (step == SEQ_NONE) return;
  if (faultHeld) return;                 // paused after a fault -> wait for RESUME (sec.19.4.8)
  int c = wo.col;
  String colLocS = colMaskName(wo.colMask);
  const char *colLoc = colLocS.c_str();

  switch (step) {

    /* ---- FILL: reservoir -> mixing tank (transfer pump) ---- *
     * Target = the batch the column should get, minus the water already in the tank that the booster can reach
     * (measured level - learned floor). Planned from a settled level (waterStageGate), timed by the transfer pump's
     * run time, then settled, measured and topped up (fillStep, 2026-11-17 user rule).                            */
    case SEQ_FILL: {
      if (fillStep(c, false) == 1) goStep(wo.fertigate ? SEQ_DOSE : SEQ_DELIVER);
      break;
    }

    /* ---- DOSE: nutrients A/B/C into mixing tank (sequential) ------------- */
    case SEQ_DOSE: {
      if (!stepInit) {
        pcfOff(OUT_INVERTER);                            // nutrient pumps are DC; drop the AC source
        // skip zero / disabled doses
        while (doseIdx < 3 && wo.dose[doseIdx] <= 0.0f) doseIdx++;
        if (doseIdx >= 3) { goStep(SEQ_MIX); break; }
#if SECONDARY_BATT_ENABLED
        // The inverter just stopped, so the boards are on their way back to main. No nutrient pump may
        // start until that switch is over: a reset mid-pour would leave an unknown amount in the tank.
        if (battPhase != BP_MAIN) break;
#endif
        float targetL = wo.dose[doseIdx] / 1000.0f;     // mL -> L
        doseJStart(doseIdx, wo.colMask, wo.dose[doseIdx]);
        stageBegin(NUT_PUMP_BIT[doseIdx], false, NUT_FLOW_PIN[doseIdx], K_NUT[doseIdx], targetL);
        sgRateLpm = rateNutMlpm[doseIdx] / 1000.0f;     // the pump's measured mL/min, in L/min (0 = meter alone)
        // Dosed liquid physically enters the mixing tank, so it must count toward mixTankL -- otherwise
        // a dose interrupted mid-pour leaves the tank bookkeeping short and a resumed FILL overfills.
        sgTankSign = +1;
        sgCapMs = wo.doseCeilMs[doseIdx] ? wo.doseCeilMs[doseIdx] : STAGE_MAX_MS;   // per-dose 2x ceiling (spec §2.4)
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1) {
        float measuredML = endMeteredStage() * 1000.0f;   // credits the tank, then ends the stage
        doseJEnd(doseIdx, wo.dose[doseIdx], measuredML);
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
        float deliveredML = endMeteredStage() * 1000.0f;  // credit whatever partial volume did make it into the tank
        doseJEnd(doseIdx, wo.dose[doseIdx], deliveredML);
        // Don't re-dose what already went in: a RESUME,NORMAL re-enters this step and re-reads
        // wo.dose[doseIdx] from scratch, so it must already reflect only what's still owed, not the
        // original full target (doseIdx itself isn't advanced on this path -- same step retries).
        wo.dose[doseIdx] = (wo.dose[doseIdx] > deliveredML) ? (wo.dose[doseIdx] - deliveredML) : 0.0f;
        // New-F5 (audit): same distinction FLOW_FAIL's pulse count already makes -- 0 mL delivered
        // means the dosing pump/sensor never moved anything; a partial mL means it started then
        // stalled. deliveredML was already computed above and previously discarded here.
        String nut = String("NUT_") + (char)('A' + doseIdx) + "|delivered=" + String(deliveredML, 1);
        holdFault("DOSE_TIMEOUT", nut.c_str());
      }
      break;
    }

    /* ---- MIX: homogenize; validate mixer current (ACS712) ---------------- */
    case SEQ_MIX: {
      // The mixer is a 12 V DC motor (2026-11-15): no inverter, no PZEM -- the usual DC settle, then its
      // relay. The mix window and the ACS712 current checks count from the mixer actually running.
      if (!stepInit) { mixRunning = false; stepStart = millis(); stepInit = true; }
      if (!mixRunning) {
        if (millis() - stepStart < VALVE_SWITCH_MS) break;
        pcfOn(OUT_MIXER); mixRunning = true; stepStart = millis(); resetMixerCurrentRing();
      }
      float mi = readMixerCurrent();
      if (mixerCurrentOver(mi)) {       // New-F4 (audit): same enrichment as PWR_FAIL -- include the reading
        pcfOff(OUT_MIXER);
        mixerOffMs = nzMs(millis());
        char b[32]; snprintf(b, sizeof(b), "MIXER_OC|i=%.2f", mi);
        holdFault("SAFE_STOP", b); break;
      }
      if (millis() - stepStart >= wo.mixMs) {
        // no-load check at end of mix window (spec sec.23.2.2.4). DEGRADED (not ERROR):
        // Major condition, ESP1 must not critical-stop -- the batch still delivers.
        if (mi < MIXER_MIN_CURRENT_A) { pcfOff(OUT_MIXER); reply("DEGRADED,MIXER_NOLOAD"); }
        pcfOff(OUT_MIXER);
        mixerOffMs = nzMs(millis());     // the delivery's baseline waits for the swirl to settle (MIX_SETTLE_MS)
        goStep(SEQ_ECPH);
      }
      break;
    }

    /* ---- EC/pH validation (local protective check, spec sec.14.2.4) ------ */
    case SEQ_ECPH: {
      // Averaged: this one reading decides the run's EC/pH verdict, and a single ESP32 ADC sample is noisy.
      int phRaw = adcAvg(PIN_PH), ecRaw = adcAvg(PIN_EC);
      float ph = PH_CAL_M * phRaw + PH_CAL_B;
      float ec = EC_CAL_M * ecRaw + EC_CAL_B;
      // HARDWARE fault first (railed ADC = probe disconnected/shorted) -> SENSOR_FAIL, distinct
      // from an in-range value that is merely outside the safe window (EC_FAIL/PH_FAIL). Both
      // are Major on ESP1; check EC and pH independently so a double-fault reports both.
      // New-F6/F7/F8 (audit): same "diagnostic enrichment" pattern as holdFault()'s "|pulses=" --
      // fold the measured value and which bound it crossed into the message instead of a bare code.
      // "too acidic" vs "too alkaline" (or railed-low vs railed-high) are different physical causes.
      if (phRaw <= PH_ADC_FAULT_LO || phRaw >= PH_ADC_FAULT_HI) {
        reply(String("SENSOR_FAIL,PH|") + (phRaw <= PH_ADC_FAULT_LO ? "RAILED_LOW" : "RAILED_HIGH"));
      } else if (ph < wo.phLo || ph > wo.phHi) {
        reply(String("PH_FAIL,") + colLoc + "|pH=" + String(ph, 2) + "|" + (ph < wo.phLo ? "LOW" : "HIGH"));
      }
      if (ecRaw <= EC_ADC_FAULT_LO || ecRaw >= EC_ADC_FAULT_HI) {
        reply(String("SENSOR_FAIL,EC|") + (ecRaw <= EC_ADC_FAULT_LO ? "RAILED_LOW" : "RAILED_HIGH"));
      } else if (ec < wo.ecLo || ec > wo.ecHi) {
        reply(String("EC_FAIL,") + colLoc + "|EC=" + String(ec, 2) + "|" + (ec < wo.ecLo ? "LOW" : "HIGH"));
      }
      // Plumbing has no drain: deliver the mixed batch regardless, fault already reported.
      goStep(SEQ_DELIVER);
      break;
    }

    /* ---- DELIVER: mixing tank -> column (booster pump, metered on the mixing-tank level) ---- *
     * Drains to the pump's floor: deliverable = measured level - learned floor. The delivered litres are
     * the level drop measured from the settled baseline to the end.                                    */
    case SEQ_DELIVER: {
      if (!stepInit) {
        float base; bool measured;
        int g = waterStageGate("MAIN", base, measured);
        if (g <= 0) break;
        float deliverable = mixTankL - planFloorL();
        if (deliverable < TANK_EPS_L) {                       // nothing the booster can reach
          if (wo.fertigate && wo.flushPct > 0) goStep(SEQ_FLUSH_FILL); else finishOk();
          break;
        }
        colValves(wo.colMask, true);
        levelStageBegin(PUMP_BOOSTER, -1, deliverable, base, measured);
        sgCapMs = stageCapMs(deliverable, boostLpm() * DRAIN_SLOW_X, STAGE_MAX_MS, STAGE_CAP_MARGIN);
        if (resumeTimed) {                                   // level metering lost -> run the booster on a timer
          sgTimed = true; sgTimedLpm = boostLpm();
          sgTimedMs = (unsigned long)(deliverable / sgTimedLpm * 60000.0f * TIMED_MARGIN);
          if (sgTimedMs > TIMED_STAGE_CAP_MS) sgTimedMs = TIMED_STAGE_CAP_MS;
        }
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1 || r == -5 || r == -6) {                    // -5 the booster sucked air / -6 the low stop = delivered
        if (r == -5) drainStoppedDry("MAIN"); else if (r == -6) drainStoppedLow("MAIN"); else endMeteredStage();
        colValves(wo.colMask, false);
        if (wo.fertigate && wo.flushPct > 0) goStep(SEQ_FLUSH_FILL);
        else finishOk();
      } else if (r == -1) {
        colValves(wo.colMask, false);
        levelFlowFail("MAIN");
      } else if (r < -1) {                                   // -2 only: a blind drain goes timed, never -4
        colValves(wo.colMask, false);
        acStageHold(r, "MAIN");
      }
      break;
    }

    /* ---- FLUSH_FILL: plain reservoir water -> mixing tank ---------------- */
    case SEQ_FLUSH_FILL: {
      if (fillStep(c, true) == 1) goStep(SEQ_FLUSH_DELIVER);
      break;
    }

    /* ---- FLUSH_DELIVER: plain water -> column (final flush step) --------- */
    case SEQ_FLUSH_DELIVER: {
      if (!stepInit) {
        float base; bool measured;
        int g = waterStageGate("MAIN", base, measured);
        if (g <= 0) break;
        float deliverable = mixTankL - planFloorL();
        if (deliverable < TANK_EPS_L) { finishOk(); break; }
        colValves(wo.colMask, true);
        levelStageBegin(PUMP_BOOSTER, -1, deliverable, base, measured);
        sgCapMs = stageCapMs(deliverable, boostLpm() * DRAIN_SLOW_X, STAGE_MAX_MS, STAGE_CAP_MARGIN);
        stepInit = true;
      }
      int r = stagePoll();
      if (r == 1)      { endMeteredStage(); colValves(wo.colMask, false); finishOk(); }
      else if (r == -5){ drainStoppedDry("FLUSH"); colValves(wo.colMask, false); finishOk(); }
      else if (r == -6){ drainStoppedLow("FLUSH"); colValves(wo.colMask, false); finishOk(); }
      else if (r == -1){ colValves(wo.colMask, false); levelFlowFail("MAIN"); }
      else if (r < -1) { colValves(wo.colMask, false); acStageHold(r, "MAIN"); }
      break;
    }

    default: break;
  }
}

/* =============================================================================
 *  SAFETY MONITOR  --  immediate local protective stops (spec sec.9.8.1.2)
 * ========================================================================== */
// Debounced report for a persistently unreadable PZEM (same counting shape as pcfFailCount/PCF_FAIL_LIMIT).
// No paired recovery token: PZEM validation only runs while an AC pump is active (like EC_FAIL/PH_FAIL),
// unlike PCF health which is continuously probed even idle, so there's no idle moment to announce
// recovery in. Reuses the existing SENSOR_FAIL wire token -- ESP1's handler is already generic on arg.
uint8_t pzemFailCount = 0;
unsigned long lastPzemReportMs = 0;
const uint8_t PZEM_FAIL_LIMIT      = 3;      // mirrors PCF_FAIL_LIMIT
const unsigned long PZEM_REPORT_MS = 10000;  // mirrors PCF_REPORT_MS

// Stops whichever AC load safetyMonitor() found the fault under. A work-order stage uses the
// existing holdFault()/RESUME machinery (tank bookkeeping applies); EXERCISE and an AC PRIME_START
// line are simple direct-energize actions with no tank/work-order state to preserve, so they just
// abort the actuator immediately and report -- there is nothing for an operator to RESUME.
// (The mixer used to be a fourth context here. It is a 12 V DC motor -- 2026-11-15 -- so the PZEM, which
// measures the inverter's AC, has nothing to say about it; its ACS712 checks in SEQ_MIX remain.)
static void auxPowerFaultStop(bool seqActive, bool exerciseActive, const char *msg) {
  if (seqActive) {
    endMeteredStage();
    holdFault("PWR_FAIL", msg);
  } else if (exerciseActive) {
    faultJSave("PWR_FAIL", 0, msg);          // this P17 drop takes P0 with it
    acPumpStop(); cutoffDeenergize();
    exRunOffAt = 0; exRunBit = -1; exRunAc = 0; exRunPumpOn = false;
    reply(String("PWR_FAIL,") + msg);
  } else {                                  // AC prime line
    stopPrime();
    reply(String("PWR_FAIL,") + msg);
  }
}

void safetyMonitor() {
  // Audit fix (2026-09-08): EXERCISE (ESP1's own unattended 2-day preventive-maintenance schedule,
  // sec.14.9.1) and an AC PRIME_START line (RESMIX/MIXIRR) both energize a pump directly via
  // pcfOn(), bypassing this monitor entirely because it previously gated only on the work-order
  // state machine's own step/sgPumpOn/sgIsAC bookkeeping. Neither path had ANY overcurrent/voltage
  // protection -- a locked-rotor pump or wiring short during an unattended overnight EXERCISE or a
  // PRIME purge could draw fault current for the full run (up to 10 s / 45 s) undetected. Now
  // covered by the same PZEM checks below.
  // (2026-09-09 added SEQ_MIX as a `mixActive` context on the belief the mixer was AC. It is a 12 V DC
  // motor, so that context is gone again -- see auxPowerFaultStop.)
  // Each AC context is only checked once the inverter has had its warm-up (AC_WARMUP_MS): a
  // still-ramping inverter reads below PZEM_V_MIN and would trip PWR_FAIL,VOLTAGE|LOW on every start.
  // 2026-11-15: an AC pump now starts the moment the inverter does (servo selector, no pump relay), so the
  // ramp grace counts from the inverter-on (acPumpRamped) instead of a separate warm-up before the pump.
  // The DC mixer -- SEQ_MIX and EXERCISE,MIXER -- is no AC context at all any more.
  bool seqActive      = (step != SEQ_NONE && sgPumpOn && sgIsAC && acPumpRamped());
  bool exerciseActive = (exRunOffAt != 0 && exRunAc && exRunPumpOn && acPumpRamped());
  bool primeAcActive  = (primeLine != "" && primeIsAC && acPumpRamped());
  if (!seqActive && !exerciseActive && !primeAcActive) { acNoCurrentSince = 0; pzVLowRun = pzVHighRun = pzOcRun = 0; return; }

  // Throttle the (blocking) Modbus reads so we don't poll every loop iteration.
  if (millis() - lastPzemMs < PZEM_POLL_MS) return;
  lastPzemMs = millis();

  float v = pzem.voltage();
  float i = pzem.current();
  if (isnan(v) || isnan(i)) {               // PZEM absent/unreadable -> skip actuation checks (graceful),
    if (pzemFailCount < 250) pzemFailCount++;   // but no longer silent: debounced alert past the limit
    if (pzemFailCount >= PZEM_FAIL_LIMIT && millis() - lastPzemReportMs >= PZEM_REPORT_MS) {
      lastPzemReportMs = millis();
      reply("SENSOR_FAIL,PZEM");            // ESP1 = Major (alert + log); other safeties still apply
    }
    return;
  }
  pzemFailCount = 0;

  // Credit the partial liters this metered stage moved before holding (overfill guard), then
  // PAUSE on the master cutoff awaiting RESUME (sec.19.4.8) -- ESP2 stays alive holding the tank.
  // New-F3 (audit): fold the actual PZEM reading into the fault message, same "diagnostic enrichment"
  // pattern as holdFault()'s own "|pulses=" for FLOW_FAIL -- a bare "VOLTAGE" didn't even say whether
  // the AC supply was over or under the safe window, which point at different physical causes.
  // Consecutive out-of-window readings (2026-11-16): one reading back in range resets its count.
  pzOcRun    = (i > PZEM_OVERCURRENT_A) ? (uint8_t)min(pzOcRun + 1, 255) : 0;
  pzVLowRun  = (v < PZEM_V_MIN)         ? (uint8_t)min(pzVLowRun + 1, 255) : 0;
  pzVHighRun = (v > PZEM_V_MAX)         ? (uint8_t)min(pzVHighRun + 1, 255) : 0;
  if (pzOcRun >= PZEM_OC_CONFIRM) {
    pzVLowRun = pzVHighRun = pzOcRun = 0;
    char b[40]; snprintf(b, sizeof(b), "OVERCURRENT|i=%.2f|max=%.1f", i, PZEM_OVERCURRENT_A);   // limit in force
    auxPowerFaultStop(seqActive, exerciseActive, b); return;
  }
  // The limit that tripped rides along (|min= / |max=), since the window is adjustable now: a trip
  // must say which setting was in force. The website's fault explanation matches on the prefix only.
  if (pzVLowRun >= PZEM_V_CONFIRM) {
    pzVLowRun = pzVHighRun = pzOcRun = 0;
    char b[40]; snprintf(b, sizeof(b), "VOLTAGE|v=%.1f|LOW|min=%.0f", v, PZEM_V_MIN);
    auxPowerFaultStop(seqActive, exerciseActive, b); return;
  }
  if (pzVHighRun >= PZEM_V_CONFIRM) {
    pzVLowRun = pzVHighRun = pzOcRun = 0;
    char b[40]; snprintf(b, sizeof(b), "VOLTAGE|v=%.1f|HIGH|max=%.0f", v, PZEM_V_MAX);
    auxPowerFaultStop(seqActive, exerciseActive, b); return;
  }

  if (i < PZEM_MIN_CURRENT_A) {             // pump ON but drawing no current
    if (acNoCurrentSince == 0) acNoCurrentSince = millis();
    else if (millis() - acNoCurrentSince > PZEM_NO_CURRENT_MS) {
      char b[40]; snprintf(b, sizeof(b), "NO_CURRENT|i=%.3f|min=%.1f", i, PZEM_MIN_CURRENT_A);
      auxPowerFaultStop(seqActive, exerciseActive, b); return;
    }
  } else {
    acNoCurrentSince = 0;
  }
}

// (2026-11-15: the mixer is 12 V DC after all. The RMS ring below still reads a DC current correctly -- the
// RMS of a steady value is that value -- and also smooths PWM/brush ripple, so it stays.)
// Audit fix (2026-09-08): the mixer is an AC (mains-frequency) load, so a single instantaneous
// analogRead() sample is essentially a random point on a sine wave -- it can read near zero on a
// perfectly healthy, loaded mixer purely because that one sample landed near a zero-crossing,
// causing spurious DEGRADED,MIXER_NOLOAD and an overcurrent trip point that depends on ADC-sampling
// luck rather than the physical load. Fixed with a small non-blocking ring buffer (one new sample
// per call -- no busy-wait, no delay(), keeps the file's "non-blocking everywhere" rule) that
// reports an RMS magnitude across the most recent samples.
// Samples are spaced at least MIXER_I_SAMPLE_US apart so the ring spans a whole mains cycle (24 ms against
// 16.7-20 ms). Taken every pass of a fast loop(), 24 samples cover only a few ms -- a slice of one half
// wave, whose "RMS" swings from near 0 to near peak, enough for a false MIXER_OC or MIXER_NOLOAD.
static const int MIXER_I_RING_N = 24;
static const unsigned long MIXER_I_SAMPLE_US = 1000;
static float mixerIRing[MIXER_I_RING_N] = {0};
static int   mixerIRingIdx = 0, mixerIRingCount = 0;
static unsigned long mixerILastUs = 0;

void resetMixerCurrentRing() { mixerIRingIdx = 0; mixerIRingCount = 0; }
bool mixerCurrentOver(float rmsA) { return mixerOvercurrent(rmsA, MIXER_OVERCURRENT_A, mixerIRingCount, MIXER_I_RING_N); }

float readMixerCurrent() {
  if (mixerIRingCount == 0 || micros() - mixerILastUs >= MIXER_I_SAMPLE_US) {
    mixerILastUs = micros();
    int raw = analogRead(PIN_MIXER_I);
    float v = (raw / 4095.0f) * 3.3f;
    float a = (v - ACS712_ZERO_V) / ACS712_SENS_V_PER_A;
    mixerIRing[mixerIRingIdx] = a;
    mixerIRingIdx = (mixerIRingIdx + 1) % MIXER_I_RING_N;
    if (mixerIRingCount < MIXER_I_RING_N) mixerIRingCount++;
  }
  double sumSq = 0.0;
  for (int k = 0; k < mixerIRingCount; k++) sumSq += (double)mixerIRing[k] * (double)mixerIRing[k];
  return sqrtf((float)(sumSq / mixerIRingCount));
}
// Mean of a short burst of ADC reads (~1 ms). EC/pH are read inline in the SEQ_ECPH stage.
int adcAvg(uint8_t pin) {
  const int N = 16;
  long sum = 0;
  for (int i = 0; i < N; i++) sum += analogRead(pin);
  return (int)(sum / N);
}

/* =============================================================================
 *  HEARTBEAT  (spec sec.10.8.3)
 * ========================================================================== */
void heartbeat() {
  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = millis();
    reply("STATUS,ESP2,OK");
    bootReports();
    // A held fault is re-asserted every HELD_REASSERT_MS (2026-11-17 bug check): if its one reply was lost on the wire,
    // ESP1 never knew the run was on hold -- and our PROG kept its run timeout fresh, so it waited forever. ESP1 acts on
    // this only for the work order it has running and not already held; otherwise it is ignored.
    if (faultHeld && wo.active && heldReply.length() && millis() - heldReplyMs >= HELD_REASSERT_MS) {
      heldReplyMs = millis();
      reply("INFO,HELD," + wo.cmdName + "," + heldReply);
    }
  }
}

// Sent with READY and repeated each heartbeat, because the first copy can be lost while ESP1 is itself
// still booting. The first two repeat until ESP1 acknowledges; the dosed tank until a run clears it.
void bootReports() {
  if (bswBootReport) reply(String("INFO,BATT,RESET,") + (bswBootReport == 1 ? "SECONDARY" : "MAIN"));
  if (faultReport.length()) reply("INFO,FAULT_LOST," + faultReport);
  if (doseJ.mask && !doseJ.stopped && !wo.active) reply(String("INFO,TANK,DOSED,") + doseJ.mask);
}

/* =============================================================================
 *  IDLE SELF-HEAL  --  reboot after prolonged idle so a wedged-but-working ESP2
 *  recovers on its own, even if ESP1 wrongly believes it is silent. Never fires
 *  during any real activity (a held fault MUST survive until the operator resolves
 *  it). mixTankL is NVS-persisted, so the tank volume survives the reboot.
 * ========================================================================== */
void idleResetTick() {
  // A backup hold counts as activity: this reboot would drop P0 and put the boards on the flat main battery.
  bool idle = (step == SEQ_NONE && !wo.active && !faultHeld && !testMode
               && calId == "" && primeLine == "" && exRunOffAt == 0 && !battBackupHold);
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
  // A Fill/Push combo whose pump selector could not be confirmed: the inverter never came on. Report it
  // once and park the selection off (testCapped) until the operator releases and presses again.
  if (testHeldBit >= 16 && testHeldBit <= 21 && !testCapped && acPumpFailed()) {   // Fill/Push + Servo rows
    String m = String(apFailType()) + ",TEST|" + apErr;
    testOff(); testCapped = true;
    reply(m);
  }
#if AC_DEAD_CHECK
  // Bare Inverter row (P2): the same confirmation as a pump start (2026-11-16) -- voltage on the inverter output
  // within INV_CONFIRM_MAX_MS of its relay coming on, or the relay goes off again (INVERTER_FAIL,TEST|NO_AC).
  // Once voltage is read it stays on (only the dead-man and the 30 s cap below apply).
  if (testHeldBit == OUT_INVERTER && !testCapped && !testInvAcOk && pcfBitOn(OUT_INVERTER)) {
    if (!testInvRelayMs) testInvRelayMs = nzMs(now);
    if (now - testInvPzemMs >= 250) {
      testInvPzemMs = now;
      float v = pzem.voltage();
      if (!isnan(v) && v >= INV_CONFIRM_V) {
        testInvAcOk = true;
        reply(String("INFO,INVERTER,AC_OK,v=") + String(v, 0) + ",t=" + String(now - testInvRelayMs));
      } else if (now - testInvRelayMs >= INV_CONFIRM_MAX_MS) {
        testOff(); testCapped = true;
        reply(String("INVERTER_FAIL,TEST|NO_AC|v=") + (isnan(v) ? String("none") : String(v, 0)));
      }
    }
  }
#endif
  // HARD CAP: force OFF regardless of ESP1. Single relays 30 s, priming combos 60 s (sec.18.10.8.3 / CLAUDE.md).
  bool acCombo = (testHeldBit >= 16 && testHeldBit <= 19);
  if (acCombo) {
    // Fill Res>Mix / Push>Col: only time with the inverter switched on counts (user 2026-11-17).
    if (pcfBitOn(OUT_INVERTER)) testInvOnMs += now - testInvTickMs;
    testInvTickMs = now;
    if (!testCapped && (testInvOnMs >= TEST_AC_RUN_CAP_MS ||
                        (testInvOnMs == 0 && now - testRelayOnMs >= TEST_AC_START_MAX_MS))) {
      testOff(); testCapped = true;                // back to resting (bank powered)
      reply(String("DONE,TEST_CAP|run_s=") + String(testInvOnMs / 1000));
    }
  } else {
    unsigned long cap = (testHeldBit > 15) ? TEST_COMBO_CAP_MS : TEST_HARD_CAP_MS;
    if (!testCapped && now - testRelayOnMs >= cap) {
      testOff(); testCapped = true;                // back to resting (bank powered)
      reply("DONE,TEST_CAP");
    }
  }
  // Dead-man: no HOLD within the timeout (button released, ESP1 hung, or link lost)
  // -> turn off and clear selection (fail-safe). Restore the resting bank-powered state.
  unsigned long deadMan = (testHeldBit >= 16 && testHeldBit <= 21) ? TEST_HOLD_TIMEOUT_AC_MS : TEST_HOLD_TIMEOUT_MS;
  if (now - testLastHoldMs > deadMan) {
    // A missed keep-alive is not a release: no re-arm gap here (2026-11-17), so the hold carries on with the next one.
    if (!testCapped) testOff();
    testHeldBit = -1; testCapped = false;
  }
}

/* =============================================================================
 *  CALIBRATION MODE  (companion spec §A)  --  raw stream, SET_CAL, Prime dead-man
 * ========================================================================== */
// Map a FLOW_* CAL/Prime id to its hardware pin (-1 if not a flow id).
/* Which flow sensor watches a given TEST relay/combo, or -1 if that output has no meter.
 * Used by the timed test pulse so the operator can tell, remotely, whether a pump that was told to
 * run actually moved fluid. -1 is meaningful and distinct from a zero count: "this output has no
 * flow sensor" must not read as "this pump moved nothing". */
int flowPinForBit(int bit) {
  switch (bit) {
    // The Fill (16) and Push (17..19) combos have no meter since web v.4: they report the mixing-tank level
    // change instead (TEST,LEVEL on release).
    case OUT_NUT_A:                             return FLOW_NUT_A;
    case OUT_NUT_B:                             return FLOW_NUT_B;
    case OUT_NUT_C:                             return FLOW_NUT_C;
    case OUT_NUT_D:                             return FLOW_NUT_D;
    case OUT_PH_UP:                             return FLOW_PH_UP;
    case OUT_PH_DN:                             return FLOW_PH_DN;
    default:                                    return -1;             // mixer, valves, inverter, cutoff
  }
}

int flowPinForId(const String &id) {
  // FLOW_RESMIX / FLOW_MIXIRR: no meter since web v.4 (-1: nothing is attached, CAL streams nothing).
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
  if (isnan(v)) v = -1;
  if (isnan(i)) i = -1;
  if (isnan(p)) p = -1;
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
  if      (id == "FLOW_RESMIX" || id == "FLOW_MIXIRR") { /* web v.4: meter removed -- accept, ignore */ }
  else if (id == "MIXCAP") {                // litres at MIX_HIGH_PCT (ESP1 tank calibration): planning ceiling
    float v = tok[i].toFloat();
    if (!(v > 0.5f && v <= MIXING_TANK_MAX_L)) return false;
    mixCapL = v;
  }
  else if (id == "FLOW_NUTA")   { if (!kSane(tok[i].toFloat())) return false; K_NUT[0]  = tok[i].toFloat(); }
  else if (id == "FLOW_NUTB")   { if (!kSane(tok[i].toFloat())) return false; K_NUT[1]  = tok[i].toFloat(); }
  else if (id == "FLOW_NUTC")   { if (!kSane(tok[i].toFloat())) return false; K_NUT[2]  = tok[i].toFloat(); }
  else if (id == "ACS712")      ACS712_ZERO_V = tok[i].toFloat();
  else if (id == "PH") { PH_CAL_M = tok[i].toFloat(); if (i + 1 < n) PH_CAL_B = tok[i + 1].toFloat(); }
  else if (id == "EC") { EC_CAL_M = tok[i].toFloat(); if (i + 1 < n) EC_CAL_B = tok[i + 1].toFloat(); }
  else if (id == "FLOW_NUTD" || id == "FLOW_PHUP" || id == "FLOW_PHDN") { /* not metered here: accept */ }
  // Pump rates the operator measured (2026-11-17, see RATE_WEIGHT): PRATE,<transfer L/min>,<booster L/min> and
  // NRATE,<A>,<B>,<C> in mL/min. 0 = not measured. Any value out of range refuses the whole line (nothing changes).
  else if (id == "PRATE") {
    if (i + 1 >= n) return false;
    float x = tok[i].toFloat(), b = tok[i + 1].toFloat();
    if ((x != 0.0f && !rateLpmOk(x)) || (b != 0.0f && !rateLpmOk(b))) return false;
    if (x != rateXferLpm)  { rateXferLpm = x;  rateSaveOne("rX", x); }
    if (b != rateBoostLpm) { rateBoostLpm = b; rateSaveOne("rB", b); }
  }
  else if (id == "PSTART") {               // the transfer's start litres (LCD Transfer pump est, 2026-11-17)
    float a = tok[i].toFloat();
    if (!(a >= -PSTART_MAX_L && a <= PSTART_MAX_L)) return false;
    if (a != xferStartL) { xferStartL = a; prefs.putFloat("xS", a); }
  }
  else if (id == "NRATE") {
    if (i + 2 >= n) return false;
    float v[3];
    for (int j = 0; j < 3; j++) {
      v[j] = tok[i + j].toFloat();
      if (v[j] != 0.0f && !rateMlpmOk(v[j])) return false;
    }
    for (int j = 0; j < 3; j++) if (v[j] != rateNutMlpm[j]) { rateNutMlpm[j] = v[j]; rateSaveOne(RATE_NUT_KEY[j], v[j]); }
  }
  else if (id == "PZEM_V") {                // AC supply window: low then high, whole volts from ESP1
    if (i + 1 >= n) return false;
    float lo = tok[i].toFloat(), hi = tok[i + 1].toFloat();
    if (!pzemWindowSane(lo, hi)) return false;   // ESP1 sees INVALID,SET_CAL_ID; the old window stays
    if (lo != PZEM_V_MIN || hi != PZEM_V_MAX) { PZEM_V_MIN = lo; PZEM_V_MAX = hi; pzemWindowSave(); }
  }
  else if (id == "PZEM_I") {                // AC current limits: min (no-current) then max (overcurrent), 0.1 A
    if (i + 1 >= n) return false;
    long lo = tok[i].toInt(), hi = tok[i + 1].toInt();
    if (!pzemAmpSane(lo, hi)) return false;      // ESP1 sees INVALID,SET_CAL_ID; the old limits stay
    if (lo != pzemA10Min || hi != pzemA10Max) {
      pzemA10Min = (int)lo; pzemA10Max = (int)hi;
      PZEM_MIN_CURRENT_A = lo / 10.0f; PZEM_OVERCURRENT_A = hi / 10.0f;
      pzemAmpSave();
    }
  }
  else return false;
  return true;
}

// Arm a prime line: open its valve(s) + run its paired pump together (§A.4.2.1). Energizes the
// master cutoff first so the bank has power. Switching lines stops the previous one.
void setupPrime(const String &line, const String &col) {
  if (primeLine != "") stopPrime();
  int valveA = -1, valveB = -1, pump = -1; bool isAC = false;
  if      (line == "RESMIX") { valveA = RES_VALVE_FITTED ? OUT_RES_VALVE : -1; pump = PUMP_TRANSFER; isAC = true; }
  else if (line == "MIXIRR") {
    pump = PUMP_BOOSTER; isAC = true;               // no mix valve any more: booster + column valve
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
  if (valveA >= 0) pcfOn(valveA);                   // valves first: an AC pump starts with its inverter
  if (valveB >= 0) pcfOn(valveB);
  if (isAC) acPumpStart(pump);                      // servo with the inverter off, then the inverter
  else      pcfOn(pump);
  primeLine = line; primeValveA = valveA; primeValveB = valveB; primePump = pump;
  primeIsAC = isAC; primeOnMs = millis(); primeCapped = false;
  reply("ACK,PRIME_START," + line);
}

void stopPrime() {
  if (primeLine == "") return;
  if (primeIsAC)            acPumpStop();           // inverter off; the selector stays put (dead switch)
  else if (primePump  >= 0) pcfOff(primePump);
  if (primeValveA >= 0) pcfOff(primeValveA);
  if (primeValveB >= 0) pcfOff(primeValveB);
  cutoffDeenergizeGraceful();
  primeLine = ""; primeValveA = primeValveB = primePump = -1; primeIsAC = false; primeCapped = false;
  primeRunMs = 0; primeRelayMs = 0;
}

// Prime dead-man + generous cap (§A.4.2.2): stop if no keep-alive arrives in time, or the cap elapses.
// End a prime by itself and keep it off until the operator releases (see primeLatched).
static void primeEndLatched() {
  stopPrime();
  primeLatched = true; primeLatchKeepMs = millis();
}
void primeSafety() {
  unsigned long now = millis();
  if (primeLatched && now - primeLatchKeepMs > 1000) primeLatched = false;   // keep-alives stopped = released
  if (primeLine == "") return;
  if (primeIsAC && acPumpFailed()) {                // selector not confirmed / stuck: no retry while held
    String m = String(apFailType()) + ",PRIME|" + apErr;
    primeEndLatched(); reply(m); reply("DONE,PRIME"); return;
  }
  // An AC line's cap counts from the pump actually running (2026-11-16): the battery switch, the servo pre-seat
  // (3 swings) and the inverter confirm come first and used to eat 10-20 s of a calibration run's 45 s.
  if (primeIsAC && primeRunMs) {                    // a timed prime: from the switch-on, exactly its run time
    // The run counts from the inverter switch-on. Until its AC is there, a start retry (no AC within
    // INV_CONFIRM_MAX_MS: relay off, servo, relay on again) moves the switch-on to the new one -- no water came before.
    if (!primeAcSeen && apRelayOnMs && apRelayOnMs != primeRelayMs && (long)(apRelayOnMs - primeArmMs) >= 0 &&
        pcfBitOn(OUT_INVERTER)) {
      primeRelayMs = apRelayOnMs; primeGen0 = apGen;
    }
    if (primeRelayMs && acPumpOn()) primeAcSeen = true;
    if (primeRelayMs && now - primeRelayMs >= primeRunMs) {
      String ln = primeLine;
      unsigned long ran = now - primeRelayMs;
      bool acOn = acPumpOn() || apGen != primeGen0;
      long ac = (acOn && (long)(apOnMs - primeRelayMs) >= 0) ? (long)(apOnMs - primeRelayMs) : -1;
      bool reseat = (apGen > primeGen0 + 1);          // the inverter came on more than once: a re-seat
      primeEndLatched();
      reply("DONE,PRIME_TIMED," + ln + "," + String(ran) + "," + (reseat ? "1" : "0") + "," + String(ac));
      return;
    }
  }
  if (primeIsAC && !acPumpOn()) primeOnMs = now;
  unsigned long primeCap = primeRunMs ? primeRunMs + 10000UL : PRIME_CAP_MS;
  if (now - primeOnMs >= primeCap) { primeEndLatched(); reply("DONE,PRIME_CAP"); return; }
  // 17 Nov log: four LCD pump-estimate runs ended "DONE,PRIME" mid-start while ESP1 kept sending -- the PZEM timeouts.
  if (now - primeLastMs > (primeIsAC ? PRIME_HOLD_TIMEOUT_AC_MS : PRIME_HOLD_TIMEOUT_MS)) { stopPrime(); reply("DONE,PRIME"); }
}

/* ---- Overflow stop for the transfer runs that are NOT work-order stages ---- *
 * A work-order FILL handles LEVEL_HIGH itself (stagePoll -> fillStoppedHigh). An EXERCISE, a Prime RESMIX
 * or a Testing Fill combo also runs the transfer pump into the mixing tank, so the same high limit stops
 * them here. (Blindness is not enforced on these: they are short, capped, and run by an operator or a
 * 10 s maximum exercise -- the work-order fill is the long unattended one.)                          */
void levelAuxTick() {
  if (!lgHighHit) return;
  if (step != SEQ_NONE && sgIsAC && sgPumpOn) return;   // a work-order stage acts on it in stagePoll()
  String m = "LEVEL_HIGH,MIX|pct=" + String(lg.lastPct, 0);
  lgHighHit = false;
  if (exRunOffAt != 0 && exRunAc)  { reply(m + "|EXERCISE"); exerciseEnd(); }
  else if (primeLine != "")        { reply(m + "|PRIME"); primeEndLatched(); reply("DONE,PRIME"); }
  else if (testMode && testHeldBit >= 0) { reply(m + "|TEST"); testOff(); testCapped = true; }
  else acPumpStop();
}
