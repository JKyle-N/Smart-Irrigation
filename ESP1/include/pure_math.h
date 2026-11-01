/* pure_math.h -- ESP1 sensor maths, extracted so it can be compiled and tested on a host PC.
 *
 * Everything here is a pure function: no globals, no Arduino headers, no millis(). Calibration
 * endpoints arrive as arguments rather than being read from calSoilAir[]/calSoilWater[], which is
 * the only change made when these were lifted out of main.cpp -- main.cpp keeps thin wrappers that
 * pass the per-column values, so no call site had to change.
 *
 * Do not add anything that touches hardware, time, or global state to this file. The point of it is
 * that `pio test -e native` can exercise it without an ESP32 attached.
 */
#ifndef PURE_MATH_H
#define PURE_MATH_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/* ---- policy constants owned by the maths below ---------------------------- */

// A capacitive probe reading <=LO or >=HI is treated as disconnected/shorted and dropped.
const int SOIL_ADC_RAIL_LO = 8, SOIL_ADC_RAIL_HI = 1015;
// How far the NPK probe's moisture may sit from the capacitive pair before it is excluded. [TBD]
const int NPK_MOIST_AGREE_PCT = 15;
// Below this the NPK probe's N/P/K registers are not trustworthy at all. [TBD] must be < soilStartPct
const int NPK_MIN_MOIST_PCT   = 15;

/* ---- helpers -------------------------------------------------------------- */

// Arduino's map() with the same integer-truncation behaviour, so results are bit-identical to what
// the firmware produced before the extraction. Local because Arduino.h is not available on host.
static inline long imap(long x, long inLo, long inHi, long outLo, long outHi) {
  if (inHi == inLo) return outLo;                    // degenerate span -> refuse to divide by 0
  return (x - inLo) * (outHi - outLo) / (inHi - inLo) + outLo;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Days in a given (year, month), Gregorian leap-year rule. month is 1-12; an out-of-range month
// returns 31 (the caller's own month bound already rejects it before this would matter -- this
// just never returns something smaller than any real month, so it can't over-reject a valid day).
static inline bool isLeapYear(int year) {
  return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}
static inline int daysInMonth(int year, int month) {
  static const int DAYS[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (month < 1 || month > 12) return 31;
  if (month == 2 && isLeapYear(year)) return 29;
  return DAYS[month - 1];
}

/* ---- real-time clock ------------------------------------------------------ */

/* Rig logs hold rows stamped 2000-01-01 (the DS3231 lost power) and 2000-00-00 45:161:0 (a garbled I2C
 * read). Neither may reach the scheduler: a bad date looks like a new day, which resets the daily
 * totals and the serviced-today check, and a wrong hour runs a column at the wrong time. */
const int RTC_YEAR_MIN = 2025, RTC_YEAR_MAX = 2099;

static inline bool rtcPlausible(int year, int month, int day, int hour, int minute, int second) {
  if (year < RTC_YEAR_MIN || year > RTC_YEAR_MAX || month < 1 || month > 12) return false;
  if (day < 1 || day > daysInMonth(year, month)) return false;
  return hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && second >= 0 && second < 60;
}

// Did the clock move further than maxS from where it should be? Garbled bytes can still form a valid date.
static inline bool rtcJumped(uint32_t got, uint32_t expected, uint32_t maxS) {
  uint32_t delta = (got > expected) ? got - expected : expected - got;
  return delta > maxS;
}

/* ---- soil moisture -------------------------------------------------------- */

// Map a raw soil ADC to 0..100 % using this column's endpoints (dry = high ADC, wet = low ADC).
static inline int soilPctCal(int raw, int air, int water) {
  long p = imap(raw, air, water, 0, 100);
  return (int)clampi((int)p, 0, 100);
}

/* A soil calibration must have dry above wet by a real margin. Equal endpoints map every reading to
 * 0 %, so the column would look bone dry forever and water on every service; a reversed pair reads
 * wet soil as dry. Applied when saving a capture and when loading saved endpoints from NVS. */
static inline bool soilCalOk(int air, int water, int minSpan) { return air - water >= minSpan; }

/* Combine a column's three moisture sensors into one 0..100 %, or -1 if none is usable.
 *
 * Capacitive first: drop a probe reading a rail (disconnected/shorted) so one dead probe cannot peg
 * the column, and average the rest in RAW ADC space before mapping (the two share endpoints, so the
 * average is meaningful there; the NPK does not, and is blended in PERCENT space below).
 *
 * The NPK moisture then gets ONE vote against the capacitive pair's two -- final = (2*cap + npk)/3 --
 * and only when it agrees with them to within NPK_MOIST_AGREE_PCT. Weighting it this way means a bad
 * NPK reading can move the column by at most a third of its own error, and a wildly wrong one is
 * excluded outright rather than averaged in. This is the whole reason the probe was previously
 * barred from the average: it is useful when the soil is wet enough for it, and misleading when not,
 * so the test is "does it agree", not "do we trust it in general".
 *
 * npkPct < 0 means no usable NPK reading (invalid, stale, or absent) -- caller decides that.
 * Caller-visible outcomes are reported through `why` so the SOIL packet handler can log them
 * without this function needing to know about logging. */
enum SoilBlend { SB_CAP_ONLY, SB_BLENDED, SB_NPK_DIVERGED, SB_NPK_FALLBACK, SB_NONE };

static inline int soilCombineCal(int v1, int v2, int npkPct, int air, int water, SoilBlend *why) {
  bool ok1 = (v1 > SOIL_ADC_RAIL_LO && v1 < SOIL_ADC_RAIL_HI);
  bool ok2 = (v2 > SOIL_ADC_RAIL_LO && v2 < SOIL_ADC_RAIL_HI);
  int raw;
  if      (ok1 && ok2) raw = (v1 + v2) / 2;   // both good -> average
  else if (ok1)        raw = v1;              // one railed -> use the good probe
  else if (ok2)        raw = v2;
  else {
    // BOTH capacitive probes look disconnected. Rather than declaring the column blind (which stops
    // it irrigating entirely), fall back to the NPK moisture if there is one. The caller still
    // raises SOIL_MISSING, so the dead probes get fixed -- this only keeps the plants watered in
    // the meantime, on the one sensor still reporting.
    if (npkPct >= 0) { if (why) *why = SB_NPK_FALLBACK; return npkPct; }
    if (why) *why = SB_NONE;
    return -1;
  }
  int capPct = soilPctCal(raw, air, water);
  if (npkPct < 0) { if (why) *why = SB_CAP_ONLY; return capPct; }
  int diff = npkPct - capPct; if (diff < 0) diff = -diff;
  if (diff > NPK_MOIST_AGREE_PCT) { if (why) *why = SB_NPK_DIVERGED; return capPct; }
  if (why) *why = SB_BLENDED;
  return (2 * capPct + npkPct) / 3;
}

/* ---- tank level ----------------------------------------------------------- */

// Map a raw ultrasonic distance (cm) to a WHOLE percent, 0..100, using empty/full geometry.
// Rounded to an integer on purpose: an ultrasonic ranger's real resolution is coarser than 1 % of
// a 27 cm span, so the decimals were noise being displayed and logged as if they meant something.
// The clamp is hard at both ends -- a reading past "full" reports exactly 100, never 101.
static inline float levelPct(float distCm, float emptyCm, float fullCm) {
  if (emptyCm == fullCm) return 0;                    // degenerate geometry -> refuse to divide by 0
  float pct = (emptyCm - distCm) * 100.0f / (emptyCm - fullCm);
  pct = roundf(pct);
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

/* ---- dashboard pulse target -> ESP2 TEST row ------------------------------ */

/* Map a dashboard TEST_PULSE target name to the ESP2 TEST row it drives. -1 = unknown.
 * Transfer maps to the FILL COMBO (16), not the bare transfer relay, so the inverter comes with it
 * (and the reservoir valve too, if ESP2's RES_VALVE_FITTED puts one back on P0). The push targets
 * are the combos that run the column valve and the booster together (there is no mixing valve any
 * more) -- the same rows the LCD Testing screen offers. */
static inline int pulseRowForTarget(const char *t) {
  if (!t)                     return -1;
  if (!strcmp(t, "colA"))     return 17;
  if (!strcmp(t, "colB"))     return 18;
  if (!strcmp(t, "colC"))     return 19;
  if (!strcmp(t, "transfer")) return 16;   // Fill combo: inverter + transfer pump
  if (!strcmp(t, "mixer"))    return 20;   // Mixer combo: an AC motor gets no power without the inverter
  if (!strcmp(t, "nutA"))     return 9;
  if (!strcmp(t, "nutB"))     return 10;
  if (!strcmp(t, "nutC"))     return 11;
  if (!strcmp(t, "nutD"))     return 12;
  if (!strcmp(t, "phUp"))     return 13;
  if (!strcmp(t, "phDn"))     return 14;
  return -1;
}

/* Does this ESP2 TEST row switch the inverter on? Row 5 is the bare inverter relay; 16..19 are the
 * Fill / Push>Col combos and 20 the dashboard's Mixer combo, which include it. Those are the Testing uses
 * that move the boards to the secondary battery, so they are the ones a low/unmonitored secondary must
 * refuse. (Single pump rows 6/7/8 do not raise the inverter on their own.) */
static inline bool testRowUsesInverter(int row) {
  return row == 5 || (row >= 16 && row <= 20);
}

/* ---- battery-switch reset strikes ------------------------------------------ */

/* Strike count after one more switch-caused reset at `now` (RTC unix seconds; 0 = unknown), given the
 * previous count and the time of the last strike. Counts towards the latch while inside `window`.
 * Anything that makes the window uncheckable -- unknown time on either side, or a clock that went
 * backwards -- is treated as INSIDE it: the safe error is latching early, never resetting forever. */
static inline uint8_t bswNextStrikes(uint8_t prev, uint32_t lastT, uint32_t now, uint32_t window) {
  bool inWindow = prev > 0 && (now == 0 || lastT == 0 || now < lastT || now - lastT < window);
  if (!inWindow) return 1;
  return (prev < 255) ? (uint8_t)(prev + 1) : prev;
}

/* Auto re-arm is due only when both times are known, the clock has not gone backwards, and a full
 * window has passed since the last strike. */
static inline bool bswAutoClearDue(uint32_t lastT, uint32_t now, uint32_t window) {
  return lastT != 0 && now != 0 && now >= lastT && now - lastT >= window;
}

/* ---- INA226 scaling ---------------------------------------------------------- */

/* CAL register for a shunt and a chosen current LSB (datasheet: 0.00512 / (LSB x R)). The two modules
 * behind the ISO1540 differ by a factor of 8 in LSB and nearly 7 in shunt, so getting this wrong is a
 * large silent error. 0 = refused: an INA226 with CAL 0 reports no current at all. */
static inline uint16_t inaCalFor(float shuntOhm, float lsbA) {
  if (!(shuntOhm > 0.0f) || !(lsbA > 0.0f)) return 0;
  float c = 0.00512f / (lsbA * shuntOhm) + 0.5f;
  if (!(c >= 1.0f) || c > 32767.0f) return 0;
  return (uint16_t)c;
}
static inline float inaBusVolts(uint16_t raw, float factor) { return raw * 0.00125f * factor; }   // 1.25 mV/bit
/* The current register is signed. + must mean discharging (battery) or producing (solar); `invert` is
 * for IN+/IN- wired the other way round. */
static inline float inaAmps(uint16_t raw, float lsbA, bool invert, float offsetA) {
  float a = (int16_t)raw * lsbA;
  if (invert) a = -a;
  return a - offsetA;
}

/* Is a calibration value read back from NVS usable? False for NaN too, because every compare fails. */
static inline bool calInRange(float v, float lo, float hi) { return v >= lo && v <= hi; }

/* Are trims for one INA226 believable? The same limits the bench tool (Calibration/ESP1/INA226) refuses
 * beyond, so a value it saved always passes and a corrupted one falls back to the compiled defaults:
 *   shunt   within x0.5..x2 of nominal (the bar with the module's own R010 left on is only 13 % out, so a
 *           real trim is far inside this; beyond it is a wrong shunt or a mistyped number)
 *   offset  at most 2 % of full scale (more than that is real current, not an offset)
 *   bus     within 10 % (the chip is good to about 0.1 %) */
static inline bool inaTrimOk(float shuntOhm, float nominalOhm, float lsbA, float offsetA, float busFactor) {
  if (!(nominalOhm > 0.0f) || !calInRange(shuntOhm, nominalOhm * 0.5f, nominalOhm * 2.0f)) return false;
  if (inaCalFor(shuntOhm, lsbA) == 0) return false;
  float limitA = 0.02f * (0.08192f / shuntOhm);       // 81.92 mV across the shunt is full scale
  if (!calInRange(offsetA, -limitA, limitA)) return false;
  return calInRange(busFactor, 1.0f / 1.10f, 1.10f);
}

/* ---- main battery low/critical flags ---------------------------------------- */

/* An inverter start sags the battery for a moment. The main INA226 sees that where the old opto path never
 * did, so without a hold the flags flicker on every pump start, and BATTERY_SAFETY_ENABLED would E-stop on
 * it. True once the reading has stayed below the line for holdMs; a reading back above it clears at once.
 * `*sinceMs` is the caller's: 0 means "not below". A long run can still sag past holdMs: that is a real
 * reading of a battery under load, and BATTERY_SAFETY_ENABLED still needs a decision on it. */
static inline bool battFlagHeld(bool below, uint32_t nowMs, uint32_t *sinceMs, uint32_t holdMs) {
  if (!below) { *sinceMs = 0; return false; }
  if (*sinceMs == 0) { *sinceMs = nowMs ? nowMs : 1; return false; }   // 0 is reserved for "not below"
  return nowMs - *sinceMs >= holdMs;
}

/* ---- force run ---------------------------------------------------------------- */

/* A force run that doses needs its batch (the water less the flush) at or above the dosing floor. A run
 * with no nutrients has no floor. Checked when arming, so the operator hears it before the countdown. */
static inline bool forceBatchOk(float liters, float flushPct, float safeMinL, float mlA, float mlB, float mlC) {
  if (!(mlA > 0.0f) && !(mlB > 0.0f) && !(mlC > 0.0f)) return true;
  return liters * (1.0f - flushPct / 100.0f) >= safeMinL;
}

/* ---- secondary battery as backup for a critically low main ---------------- */

/* BK_IDLE       main is fine.
 * BK_ALERT      main stayed critical: the alert is going out, and nothing moves until it has. A switch
 *               that resets ESP1 would lose a text still queued in RAM.
 * BK_WAIT       alert sent, not on backup: the secondary can't be used right now, or the rig is busy.
 * BK_SWITCHING  ESP2 was asked to hold the boards on the secondary.
 * BK_ACTIVE     on backup.
 * BK_RETURNING  ESP2 was asked to go back to main. */
enum BkState { BK_IDLE, BK_ALERT, BK_WAIT, BK_SWITCHING, BK_ACTIVE, BK_RETURNING };

struct BackupIn {
  bool idle;          // nothing running and no operator in a menu: the boards may be moved
  bool mainLowHeld;   // main below the critical level for the whole trigger time
  bool mainOkHeld;    // main at or above the recover level for the whole recover time
  bool secUsable;     // monitor answering, not low, no switch fault, hold-up recharged
  bool secExhausted;  // pack at its critical level, or no longer monitored
  bool alertDone;     // the critical text left the modem, or the wait for it ran out
  bool onSecondary;   // ESP2 confirmed the boards are on the secondary
  bool timedOut;      // the current switch request took too long
};

/* One step of the backup logic. Every path back to main ends in BK_IDLE (main recovered) or BK_WAIT
 * (main still critical), and a reset simply starts again from BK_IDLE, so no state can strand the boards. */
static inline BkState backupNext(BkState s, const BackupIn &in) {
  switch (s) {
    case BK_IDLE:      return (in.mainLowHeld && in.idle) ? BK_ALERT : BK_IDLE;
    case BK_ALERT:     return in.mainOkHeld ? BK_IDLE : in.alertDone ? BK_WAIT : BK_ALERT;
    case BK_WAIT:      return in.mainOkHeld ? BK_IDLE : (in.secUsable && in.idle) ? BK_SWITCHING : BK_WAIT;
    case BK_SWITCHING: if (in.onSecondary) return BK_ACTIVE;
                       return (in.mainOkHeld || in.timedOut || !in.secUsable) ? BK_RETURNING : BK_SWITCHING;
    case BK_ACTIVE:    if (!in.onSecondary) return BK_WAIT;                // lost it: a fault, E-stop or reset
                       return (in.secExhausted || in.mainOkHeld) ? BK_RETURNING : BK_ACTIVE;
    case BK_RETURNING: if (in.onSecondary && !in.timedOut) return BK_RETURNING;
                       return in.mainOkHeld ? BK_IDLE : BK_WAIT;
  }
  return BK_IDLE;
}

#endif // PURE_MATH_H
