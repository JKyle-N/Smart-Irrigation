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

/* ---- soil moisture -------------------------------------------------------- */

// Map a raw soil ADC to 0..100 % using this column's endpoints (dry = high ADC, wet = low ADC).
static inline int soilPctCal(int raw, int air, int water) {
  long p = imap(raw, air, water, 0, 100);
  return (int)clampi((int)p, 0, 100);
}

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
 * Transfer maps to the FILL COMBO (16), not the bare transfer relay: running that pump against a
 * shut reservoir valve dead-heads it. The push targets are the combos that open the mixing valve,
 * the column valve and the booster together -- the same rows the LCD Testing screen offers. */
static inline int pulseRowForTarget(const char *t) {
  if (!t)                     return -1;
  if (!strcmp(t, "colA"))     return 17;
  if (!strcmp(t, "colB"))     return 18;
  if (!strcmp(t, "colC"))     return 19;
  if (!strcmp(t, "transfer")) return 16;   // Fill combo: inverter + reservoir valve + transfer pump
  if (!strcmp(t, "mixer"))    return 8;
  if (!strcmp(t, "nutA"))     return 9;
  if (!strcmp(t, "nutB"))     return 10;
  if (!strcmp(t, "nutC"))     return 11;
  if (!strcmp(t, "nutD"))     return 12;
  if (!strcmp(t, "phUp"))     return 13;
  if (!strcmp(t, "phDn"))     return 14;
  return -1;
}

#endif // PURE_MATH_H
