/* flow_math.h -- ESP2 flow-meter maths, extracted so it can be compiled and tested on a host PC.
 *
 * Pure functions only: no globals, no interrupts, no Arduino headers. litersSoFar() in main.cpp
 * still owns reading the interrupt-driven pulse counter; it hands the count here for the division.
 *
 * Do not add anything that touches hardware or global state to this file.
 */
#ifndef FLOW_MATH_H
#define FLOW_MATH_H

#include <math.h>

/* The ESP1 link has framing but no checksum, so a bit-flip inside a KMAIN/KNUT number survives as a
 * plausible float (toFloat() on a mangled token yields 0). Both intake paths are range checked,
 * following the same "out of range -> ignore, keep the known-good value" convention the WATER token
 * already uses. 450 is the bench value; this band is wide enough for any real sensor. */
const float K_FLOW_MIN = 1.0f, K_FLOW_MAX = 100000.0f;

static inline bool kSane(float k) { return isfinite(k) && k >= K_FLOW_MIN && k <= K_FLOW_MAX; }

/* Pulses -> litres for one meter.
 *
 * Refuses an out-of-range K independently of the intake checks, because this is the line that would
 * actually produce the damage. Returning 0 makes an unusable K look like NO FLOW, which the stage
 * already handles safely (FLOW_TIMEOUT_MS -> FLOW_FAIL -> hold for the operator). Dividing by a zero
 * K instead yields inf, which reads as "target reached" and completes the stage instantly with
 * nothing delivered -- a silent failure, and the worst possible answer. */
static inline float pulsesToLiters(unsigned long pulses, float k) {
  if (!kSane(k)) return 0.0f;
  return (float)pulses / k;
}

/* Hard cap for one fill/deliver stage: at least `baseMs`, and longer when the batch itself needs it.
 * A flat 180 s cap faulted a legal 20 L FORCE delivery (200 s at 6 L/min) with FLOW_FAIL at ~18 L. The
 * margin leaves room for a pump below its nominal rate; `baseMs` keeps a small batch's cap unchanged. */
static inline unsigned long stageCapMs(float targetL, float lpm, unsigned long baseMs, float margin) {
  if (!(targetL > 0.0f) || !(lpm > 0.0f)) return baseMs;
  unsigned long needMs = (unsigned long)(targetL / lpm * 60000.0f * margin);
  return needMs > baseMs ? needMs : baseMs;
}

/* Litres a flow-blind (timed) stage may credit after running `elapsedMs`. Crediting the whole target
 * regardless made an interrupted or capped stage look finished: a delivery stopped early zeroed the
 * tank's bookkeeping while water was still in it, and the next fill overfilled. */
static inline float timedLiters(float targetL, float lpm, unsigned long elapsedMs) {
  if (!(targetL > 0.0f) || !(lpm > 0.0f)) return 0.0f;
  float moved = lpm * (float)elapsedMs / 60000.0f;
  return moved < targetL ? moved : targetL;
}

/* Flow started, then stopped. Only AC stages: a dosing pump's pulses can legitimately be seconds apart,
 * and those stages have their own per-dose ceiling. Without this a tank that ran out left the transfer
 * or booster pump running dry until the stage's hard cap. */
static inline bool flowStalled(bool sawFlow, bool isAC, unsigned long msSinceLastPulse, unsigned long limitMs) {
  return sawFlow && isAC && msSinceLastPulse > limitMs;
}

/* Dose journal. A controller reset between dosing and delivery leaves a dosed tank and a column that is
 * still due, and the re-run would dose it all over again. The journal holds what is already in the tank:
 * a dose counts as fully delivered from the moment it starts (a reset mid-pour then under-doses, which
 * the next cycle corrects, instead of over-dosing, which nothing can correct), and is trimmed to the
 * measured amount when it ends. */
static inline float doseStillOwed(float wantMl, float alreadyMl) {
  if (!(wantMl > 0.0f)) return 0.0f;
  if (!(alreadyMl > 0.0f)) return wantMl;
  return wantMl > alreadyMl ? wantMl - alreadyMl : 0.0f;
}
static inline float doseJournalTrim(float journalMl, float targetMl, float measuredMl) {
  float v = journalMl - targetMl + measuredMl;
  return v > 0.0f ? v : 0.0f;
}

#endif // FLOW_MATH_H
