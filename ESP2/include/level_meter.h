/* =============================================================================
 *  level_meter.h  --  ultrasonic water metering for the AC pump stages (pure, host-testable)
 * -----------------------------------------------------------------------------
 *  web v.4 "pure ultrasonic" (2026-11-16): the ResMix (reservoir -> mixing) and MixIrr (mixing -> column)
 *  flow meters are no longer used. Every litre the transfer and booster pumps move is read from the tank
 *  ultrasonics (in litres, converted by ESP1 from its tank calibration, see shared/tank_volume.h): a
 *  FILL is measured on the reservoir's drop with the mixing tank as the witness (xferEstimate), a
 *  delivery on the mixing tank's fall.
 *
 *  An ultrasonic level is a noisy, slow, occasionally wild signal: ripples, a splash under the transducer,
 *  a lost echo, a multipath echo off the tank wall. The control decisions taken from it here -- stop the
 *  fill, the tank is empty, nothing is flowing, the wrong pump is running -- are therefore never taken
 *  from one sample:
 *    - LevelFilt   : median of the last LF_N accepted samples, with a jump gate in front of it. A sample
 *                    more than `maxJumpL` away from the median is held back; only LF_JUMP_CONFIRM such
 *                    samples IN A ROW that agree with each other are believed (a real step, e.g. after a
 *                    sensor re-mount), and then the ring restarts from them.
 *    - LevelStage  : progress of one fill/drain from a SETTLED baseline, with "no flow at all" and
 *                    "flow stopped" (stall) judged on progress steps, never on a single reading.
 *    - wrong-way   : the level moves the opposite way to the selected pump (stuck pump selector).
 *  No Arduino types: `pio test -e native` runs test/test_level_meter on the host.
 * ========================================================================== */
#pragma once
#include <stdint.h>
#include <math.h>

#define LF_N 5                      // median window (samples arrive ~3 Hz -> ~1.7 s)
const uint8_t LF_MIN_VALID     = 3; // samples in the ring before the median is believed
const uint8_t LF_JUMP_CONFIRM  = 3; // consecutive agreeing out-of-gate samples that are a real step

struct LevelFilt {
  float    ring[LF_N];
  uint8_t  n, idx;
  float    med;            // current median (valid only when n >= LF_MIN_VALID)
  uint32_t lastMs;         // when the last sample was ACCEPTED (0 = never)
  uint32_t count;          // accepted samples since reset (lets callers wait for N fresh ones)
  uint8_t  rejectRun;      // consecutive samples held back by the jump gate
  float    rejectFirst;    // the first of them (a real step must agree with it)
  uint32_t rejects;        // samples held back since reset (noise statistics)
};

static inline void lfReset(LevelFilt &f) {
  for (uint8_t i = 0; i < LF_N; i++) f.ring[i] = 0.0f;
  f.n = 0; f.idx = 0; f.med = -1.0f; f.lastMs = 0; f.count = 0;
  f.rejectRun = 0; f.rejectFirst = 0.0f; f.rejects = 0;
}

static inline float lfMedianOf(const float *v, uint8_t n) {
  float s[LF_N];
  for (uint8_t i = 0; i < n; i++) {                 // insertion sort, n <= 5
    float x = v[i]; int8_t j = (int8_t)i - 1;
    while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; }
    s[j + 1] = x;
  }
  if (n == 0) return -1.0f;
  return (n & 1) ? s[n / 2] : 0.5f * (s[n / 2 - 1] + s[n / 2]);
}

static inline bool lfValid(const LevelFilt &f) { return f.n >= LF_MIN_VALID; }

// Fresh = believed, and the last accepted sample is younger than staleMs (wrap-safe).
static inline bool lfFresh(const LevelFilt &f, uint32_t now, uint32_t staleMs) {
  return lfValid(f) && f.lastMs != 0 && (uint32_t)(now - f.lastMs) < staleMs;
}

/* One sample, in litres. L < 0 = "no reading" (lost echo, noisy, Nano silent): ignored, so the stale clock
 * keeps running. Returns true if the sample was accepted into the median. */
static inline bool lfPush(LevelFilt &f, float L, uint32_t now, float maxJumpL) {
  if (!(L >= 0.0f) || !isfinite(L)) return false;
  if (lfValid(f) && fabsf(L - f.med) > maxJumpL) {
    // Out of the gate. Believe it only when LF_JUMP_CONFIRM in a row say the same thing.
    if (f.rejectRun == 0 || fabsf(L - f.rejectFirst) > maxJumpL) { f.rejectRun = 1; f.rejectFirst = L; }
    else f.rejectRun++;
    f.rejects++;
    if (f.rejectRun < LF_JUMP_CONFIRM) return false;
    // A genuine step: restart the window from the new level.
    f.n = 0; f.idx = 0; f.rejectRun = 0;
    for (uint8_t i = 0; i < LF_JUMP_CONFIRM && i < LF_N; i++) { f.ring[f.idx] = L; f.idx = (uint8_t)((f.idx + 1) % LF_N); f.n++; }
  } else {
    f.rejectRun = 0;
    f.ring[f.idx] = L; f.idx = (uint8_t)((f.idx + 1) % LF_N);
    if (f.n < LF_N) f.n++;
  }
  f.med = lfMedianOf(f.ring, f.n);
  f.lastMs = now ? now : 1;                         // 0 is reserved for "never"
  f.count++;
  return true;
}

/* ---- one fill (+1) or drain (-1) stage, measured on the mixing tank -------------------------------- */
struct LevelStage {
  int8_t   dir;            // +1 = the level should rise (transfer), -1 = it should fall (booster)
  float    baseL;          // settled level when the stage began
  float    bestL;          // furthest level reached in `dir` (advanced in steps of stepL)
  uint32_t bestMs;         // when bestL last advanced
  bool     moved;          // progress reached minL at least once ("water is really moving")
};

static inline void lsBegin(LevelStage &s, int8_t dir, float baseL, uint32_t now) {
  s.dir = dir; s.baseL = baseL; s.bestL = baseL; s.bestMs = now; s.moved = false;
}

// Litres moved in the stage's own direction (negative = the wrong way).
static inline float lsMoved(const LevelStage &s, float curL) { return (float)s.dir * (curL - s.baseL); }

// Feed the current (filtered) level. Progress is only counted in steps of `stepL`, so ripples of less
// than a step can neither fake progress nor hide a stall.
static inline void lsUpdate(LevelStage &s, float curL, uint32_t now, float stepL, float minL) {
  if (lsMoved(s, curL) >= minL) s.moved = true;
  if ((float)s.dir * (curL - s.bestL) >= stepL) { s.bestL = curL; s.bestMs = now; }
}

// Nothing moved at all since the pump came on (dead pump, closed line, air lock, wrong pump with its line shut).
static inline bool lsNoFlow(const LevelStage &s, uint32_t elapsedMs, uint32_t noFlowMs) {
  return !s.moved && elapsedMs >= noFlowMs;
}

// Water moved, then the level stopped changing for stallMs: the tank feeding the pump ran dry (or the
// pump lost its prime). The caller decides which, from the reservoir level.
static inline bool lsStalled(const LevelStage &s, uint32_t now, uint32_t stallMs) {
  return s.moved && (uint32_t)(now - s.bestMs) >= stallMs;
}

// A paused stage (stuck-selector re-seat: the inverter is off while the servo swings) must not count the
// pause as "no progress".
static inline void lsShift(LevelStage &s, uint32_t pauseMs) { s.bestMs += pauseMs; }

/* ---- stuck pump selector, from the levels (replaces the old cross-flow sensor check) --------------- *
 * dir = the pump that SHOULD run (+1 transfer: mixing tank rises; -1 booster: mixing tank falls).
 * dMixL / dResL = level change since the check was armed. Evidence of the OTHER pump, in litres:
 *   - the mixing tank moves the wrong way (transfer selected but it falls = booster running; booster
 *     selected but it rises = transfer running);
 *   - booster selected and the RESERVOIR falls = the transfer pump is the one running.
 * Returns the larger of the two (0 when neither). resOk = the reservoir reading is usable. */
static inline float levelWrongWayL(int8_t dir, float dMixL, float dResL, bool resOk) {
  float ev = -(float)dir * dMixL;
  if (ev < 0.0f) ev = 0.0f;
  if (dir < 0 && resOk && -dResL > ev) ev = -dResL;
  return ev;
}

/* Confirm it: the evidence has lasted confirmMs AND kept growing by growthL since it first crossed the
 * floor -- water from the wrong pump keeps moving; a slosh or a wave after the inverter start does not. */
static inline bool levelWrongConfirmed(float evNow, float evAtStart, uint32_t heldMs,
                                       uint32_t confirmMs, float growthL) {
  return heldMs >= confirmMs && evNow - evAtStart >= growthL;
}

/* ---- litres transferred by a FILL: the reservoir is the basis, the mixing tank the witness ---------- *
 * (web v.4, 2026-11-16 user rule.) While the transfer pump fills the mixing tank, the water falls on that
 * surface: waves, a rising jet, foam -- its ultrasonic ripples by a few tenths of a litre. The reservoir is
 * only drawn from at the bottom, so its surface stays almost flat. The reservoir's DROP (plus any inflow its
 * flow meter counted during the stage) is therefore the main measure of what was transferred.
 * The mixing tank is never ignored:
 *   - within the agreement band the estimate is pulled towards the mixing-tank rise by (1 - wRes);
 *   - outside it the reading is a disagreement (reported) -- the estimate stays on the reservoir, but
 *     xferDone() also stops the fill as soon as the mixing tank ITSELF clearly shows the target, so a
 *     reservoir that under-reads can never overfill the mixing tank;
 *   - the overflow stop (LEVEL_HIGH, MIX_HIGH_PCT) is still judged on the mixing tank alone, elsewhere.
 * One source missing -> the other alone. Agreement band = max(minL, frac * the larger), as levelsDisagree(). */
enum XferSrc : uint8_t { XF_NONE = 0, XF_BOTH = 1, XF_RES = 2, XF_MIX = 3 };
struct XferEst {
  float   L;          // best estimate of the litres transferred so far
  float   tol;        // agreement band used (0 unless both sources were readable)
  uint8_t src;        // XferSrc
  bool    disagree;   // both readable and further apart than tol
};

static inline XferEst xferEstimate(float resDownL, bool resOk, float mixUpL, bool mixOk,
                                   float wRes, float minL, float frac) {
  XferEst e = { 0.0f, 0.0f, XF_NONE, false };
  if (!(wRes >= 0.0f)) wRes = 0.0f;
  if (wRes > 1.0f) wRes = 1.0f;
  if (resOk && mixOk) {
    float a = fabsf(resDownL), b = fabsf(mixUpL);
    float tol = frac * (a > b ? a : b); if (tol < minL) tol = minL;
    float d = mixUpL - resDownL;
    e.disagree = fabsf(d) > tol;
    if (d > tol) d = tol;
    if (d < -tol) d = -tol;
    e.L = resDownL + (1.0f - wRes) * d;        // = wRes*res + (1-wRes)*mix while they agree
    e.tol = tol; e.src = XF_BOTH;
  } else if (resOk) { e.L = resDownL; e.src = XF_RES; }
  else if (mixOk)   { e.L = mixUpL;   e.src = XF_MIX; }
  return e;
}

/* The fill has reached its target: the estimate says so (minus leadL for filter lag + pump run-on), OR the
 * mixing tank alone reads more than mixOverL (its ripple allowance) past the target -- its own reading says
 * the water is already there, whatever the reservoir thinks. A fixed allowance, not the agreement band: the
 * band grows with the volume, and the mixing tank's protection must not. */
static inline bool xferDone(const XferEst &e, float mixUpL, bool mixOk, float targetL, float leadL,
                            float mixOverL) {
  if (e.src != XF_NONE && e.L >= targetL - leadL) return true;
  return mixOk && e.src == XF_BOTH && mixUpL - mixOverL >= targetL;
}

/* ---- which pump is really running? (2026-11-16 user rule: judged once, 10 s after the inverter came on) --- *
 * dir = the pump that SHOULD run (+1 transfer, -1 booster). dMixL / dResL = the level change since the inverter
 * came on (litres). resOk = the reservoir was readable at both ends. resHasWater = it reads clearly above its
 * filter zone (below that its ultrasonic cannot see the water, so "the reservoir did not drop" means nothing).
 *   TRANSFER expected:
 *     the mixing tank rises, or the reservoir drops           -> PV_OK
 *     the mixing tank FALLS                                   -> PV_WRONG  (the booster runs)
 *     nothing moves although the reservoir holds water        -> PV_NOMOVE (the booster runs against its shut
 *                                                                           column valves -- user: "it might be
 *                                                                           that the booster is powered")
 *   BOOSTER expected:
 *     the mixing tank RISES, or the reservoir FALLS while the
 *     mixing tank does not                                    -> PV_WRONG  (the transfer pump runs)
 *     the mixing tank drains                                  -> PV_OK
 *   anything else                                             -> PV_UNSURE (no verdict: the stage's own no-flow /
 *                                                                           empty-reservoir rules decide)
 * moveL = "water really moved", wrongL = "water moved the wrong way" (both well above the filtered noise). */
enum PumpVerdict : uint8_t { PV_UNSURE = 0, PV_OK = 1, PV_WRONG = 2, PV_NOMOVE = 3 };
static inline uint8_t pumpVerdict(int8_t dir, float dMixL, float dResL, bool resOk, bool resHasWater,
                                  float moveL, float wrongL) {
  if (dir > 0) {
    if (dMixL >= moveL) return PV_OK;
    if (resOk && -dResL >= moveL) return PV_OK;
    if (-dMixL >= wrongL) return PV_WRONG;
    if (resOk && resHasWater) return PV_NOMOVE;
    return PV_UNSURE;
  }
  if (dMixL >= wrongL) return PV_WRONG;
  if (-dMixL >= moveL) return PV_OK;
  if (resOk && -dResL >= wrongL) return PV_WRONG;
  return PV_UNSURE;
}

/* ---- reservoir empty, judged on the MIXING TANK (2026-11-16 user rule) ---------------------------------- *
 * The reservoir has a filter at the bottom: its ultrasonic stops at ~15 % and cannot see whether any water is
 * left below that. So once the reservoir reads in its filter zone (<= 18 %) during a transfer, a window opens:
 * if the mixing tank has not risen by riseL by the end of it (winMs, 10 s), the reservoir is empty. If it has,
 * the next window opens from there. Leaving the zone closes the window. ewTick returns true = empty. */
struct EmptyWin { bool on; uint32_t t0; float mix0; };
static inline void ewReset(EmptyWin &w) { w.on = false; w.t0 = 0; w.mix0 = 0.0f; }
static inline bool ewTick(EmptyWin &w, bool inZone, float mixL, uint32_t now, uint32_t winMs, float riseL) {
  if (!inZone) { w.on = false; return false; }
  if (!w.on) { w.on = true; w.t0 = now; w.mix0 = mixL; return false; }
  if ((uint32_t)(now - w.t0) < winMs) return false;
  if (mixL - w.mix0 < riseL) return true;
  w.t0 = now; w.mix0 = mixL;                          // still filling: judge the next window from here
  return false;
}

/* Reservoir cross-check for a finished fill: the mixing tank rose by mixUpL, the reservoir fell by resDownL.
 * Both are measured by different sensors; a big disagreement means one of them (or a tank calibration) is
 * wrong. Disagree = differ by more than max(minL, frac * the larger). */
static inline bool levelsDisagree(float mixUpL, float resDownL, float minL, float frac) {
  float a = fabsf(mixUpL), b = fabsf(resDownL);
  float big = a > b ? a : b;
  float tol = frac * big; if (tol < minL) tol = minL;
  return fabsf(a - b) > tol;
}

/* ---- the pump's calibrated rate x its run time: a third estimate (2026-11-17 user request) ---------------- *
 * The transfer and booster pumps sit BELOW the tanks (flooded suction): they are always primed, so water moves
 * the moment the inverter is on, at about the rate the operator measured by hand (LCD Calibration > Rate ...).
 * rateL = rate x run time is then an estimate of its own -- one with no ripple at all. It is blended into the
 * levels' (or a dosing meter's) estimate with weight w while the two agree, and the pull fades to nothing as they
 * drift apart: d = rateL - levelsL, tol = max(minL, frac x the larger),
 *     L = levelsL + w * d * (1 - |d| / tol)   while |d| < tol,   L = levelsL   beyond it.
 * Beyond the band the pump is not giving its calibrated rate (a blocked filter, a source running dry, a stale
 * calibration) and only the tanks know what really moved -- so a wrong rate can shift the estimate by at most
 * w * tol / 4, and never makes it jump. Whether water moves at all (no flow, stall, empty, wrong pump) is still
 * judged on the levels alone, never on the clock. */
struct RateMix { float L; float tol; bool disagree; };
static inline RateMix rateBlend(float levelsL, float rateL, float w, float minL, float frac) {
  RateMix m = { levelsL, 0.0f, false };
  if (!isfinite(levelsL) || !isfinite(rateL) || !(rateL >= 0.0f)) return m;
  float a = fabsf(levelsL), b = fabsf(rateL);
  float tol = frac * (a > b ? a : b);
  if (!(tol >= minL)) tol = minL;
  if (!(tol > 0.0f)) return m;
  float d = rateL - levelsL, ad = fabsf(d);
  m.tol = tol;
  m.disagree = ad > tol;
  if (!(w > 0.0f) || ad >= tol) return m;
  if (w > 1.0f) w = 1.0f;
  m.L = levelsL + w * d * (1.0f - ad / tol);
  return m;
}

/* The rate the levels (or a dosing meter) actually saw: litres per minute over elapsedMs. -1 = too short to say. */
static inline float measuredLpm(float movedL, uint32_t elapsedMs, uint32_t minMs) {
  if (elapsedMs < minMs || elapsedMs == 0 || !isfinite(movedL)) return -1.0f;
  return movedL * 60000.0f / (float)elapsedMs;
}

/* A pump is WEAK when, over a stretch long enough to judge (the caller decides), the levels saw less than
 * weakFrac of what its calibrated rate should have moved. Not a fault by itself -- an alert (blocked filter, worn
 * pump, a valve half shut, or a calibration taken under other conditions). */
/* ---- Transfer by run time (2026-11-17 user rule) --------------------------------------------------------------
 * The user's bucket measurements give the transfer pump a straight line, with the time counted from the inverter
 * SWITCH-ON (its start-up included): V(t) = startL + q * t (q in L/s). Below minS (the shortest measured run) the line
 * means nothing -- the inverter is still starting -- so the run time is never shorter than minS, and the litres shown
 * for a run in progress ramp from 0 at the switch-on to the line at minS.                                           */
static inline float xferRunS(float needL, float qLps, float startL, float minS) {
  if (!(qLps > 0.0f) || !isfinite(needL)) return minS;
  float t = (needL - startL) / qLps;
  return (t > minS) ? t : minS;
}
static inline float xferLitresAt(float tS, float qLps, float startL, float minS) {
  if (!(tS > 0.0f) || !(qLps > 0.0f)) return 0.0f;
  float atMin = startL + qLps * minS;
  if (atMin < 0.0f) atMin = 0.0f;
  if (tS < minS) return (minS > 0.0f) ? atMin * tS / minS : atMin;
  float v = startL + qLps * tS;
  return v > 0.0f ? v : 0.0f;
}
/* Settle watch: true once the reading has stayed within bandL for winMs (a move past the band starts it again). */
struct SteadyWatch { uint32_t t0; float lo, hi; bool on; };
static inline void swReset(SteadyWatch &w) { w.on = false; w.t0 = 0; w.lo = 0.0f; w.hi = 0.0f; }
static inline bool swSample(SteadyWatch &w, float v, uint32_t now, float bandL, uint32_t winMs) {
  if (!isfinite(v)) return false;
  if (!w.on) { w.on = true; w.t0 = now; w.lo = v; w.hi = v; return winMs == 0; }
  if (v < w.lo) w.lo = v;
  if (v > w.hi) w.hi = v;
  if (w.hi - w.lo > bandL) { w.t0 = now; w.lo = v; w.hi = v; return false; }
  return (uint32_t)(now - w.t0) >= winMs;
}
/* After the settle: run the transfer again only when more than minShortL short, and only maxN times. */
static inline bool fillNeedsTopup(float goalL, float measuredL, float minShortL, uint8_t done, uint8_t maxN) {
  return isfinite(goalL) && isfinite(measuredL) && (goalL - measuredL) > minShortL && done < maxN;
}

/* ---- Plausibility (2026-11-17, from the web v.3 log of 17 Nov 08:01) --------------------------------------
 * A stage cannot have moved water faster than its pump can: paceOk() is false when movedL litres in elMs would
 * need more than x times the pump's rate (L/min), beyond slackL of level noise. A level that says "done" too early
 * is a glitch, not water -- the stage keeps running until the pace is believable (or a level rule ends it).     */
static inline bool paceOk(float movedL, uint32_t elMs, float lpm, float x, float slackL) {
  if (!isfinite(movedL) || movedL <= slackL) return true;
  if (!(lpm > 0.0f) || !(x > 0.0f)) return true;            // no rate to judge by
  return movedL <= lpm * x * ((float)elMs / 60000.0f) + slackL;
}
/* Flow-meter burst check: at most this many pulses can arrive in winMs from a meter of K pulses/L on a pump that
 * cannot exceed maxLpm (1.5x headroom, +3 pulses of edge quantisation). More is electrical noise (an inverter or
 * relay switching on next to the meter cable), and the window is not counted. */
static inline uint32_t burstMaxPulses(float k, float maxLpm, uint32_t winMs) {
  if (!(k > 0.0f) || !(maxLpm > 0.0f)) return 0xFFFFFFFFu;  // nothing to judge by: never drop
  float p = k * maxLpm * ((float)winMs / 60000.0f) * 1.5f + 3.0f;
  return p > 4.0e9f ? 0xFFFFFFFFu : (uint32_t)p;
}

static inline bool pumpWeak(float levelsL, float rateL, float weakFrac) {
  return rateL > 0.0f && isfinite(levelsL) && levelsL < weakFrac * rateL;
}
