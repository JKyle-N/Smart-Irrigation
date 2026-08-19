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

#endif // FLOW_MATH_H
