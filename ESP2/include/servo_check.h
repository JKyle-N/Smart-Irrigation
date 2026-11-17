/* =============================================================================
 *  servo_check.h  --  stuck pump-selector detection (pure, host-testable)
 * -----------------------------------------------------------------------------
 *  The inverter feeds ONE pump through a changeover switch flipped by the Nano's D3 servo (LEFT =
 *  transfer, RIGHT = booster). If the servo sticks or its lever slips, the inverter powers the WRONG
 *  pump. Each pump has its own flow sensor (transfer -> FLOW_RES_MIX, booster -> FLOW_MIX_IRR), so the
 *  wrong pump shows up as flow on the OTHER sensor ("cross" flow).
 *  Rule (user, 2026-11-15): cross flow -> inverter off -> full servo swing twice (other side, back,
 *  other side, back) -> inverter on -> check again. Two such retries, then hold the run.
 *  web v.4 (no main-line flow meters): the wrong pump is now judged from the tank LEVELS (level_meter.h
 *  pumpVerdict). crossFlowMismatch / crossFlowConfirmed below are kept for their host tests only; the
 *  re-seat plan and the retry limit are still used as they are.
 *  No Arduino types: `pio test -e native` runs test/test_servo_check on the host.
 * ========================================================================== */
#pragma once
#include <stdint.h>

/* The wrong pump is running: the other sensor counted at least `minPulses` (a floor that keeps a few
 * noise pulses from triggering a re-seat) AND more than the expected sensor did. `expectPulses` is 0 when
 * the expected sensor is not being counted (an exercise or a prime has no meter attached). */
static inline bool crossFlowMismatch(uint32_t crossPulses, uint32_t expectPulses, uint32_t minPulses) {
  return crossPulses >= minPulses && crossPulses > expectPulses;
}

/* A cross-flow mismatch only counts once it has LASTED (2026-11-16, inverter-hold fix).
 * The watch used to count from the instant the inverter relay closed. That moment -- contact bounce/arc on
 * the AC relay plus the motor inrush -- induces a burst of false edges on the flow-sensor wiring (the same
 * coupling that got the pump relays removed), and the start surge can spin the idle turbine a few turns.
 * 20 pulses arrive in milliseconds, so the inverter was cut almost as soon as it came on, the servo swung,
 * and after two retries the run held as SERVO_FAIL|STUCK: a servo fault the servo never had.
 * main.cpp now ignores the first CROSS_BLANK_MS after inverter-on, and then calls this once the mismatch
 * has held `heldMs`: it is confirmed only if it held for `confirmMs` AND the other sensor kept counting
 * (at least `minGrowth` more pulses since the mismatch began). Water from the wrong pump keeps flowing;
 * a switching transient does not. */
static inline bool crossFlowConfirmed(uint32_t crossNow, uint32_t crossAtStart, uint32_t heldMs,
                                      uint32_t confirmMs, uint32_t minGrowth) {
  return heldMs >= confirmMs && crossNow >= crossAtStart && crossNow - crossAtStart >= minGrowth;
}

/* The servo moves for one re-seat: `swings` full swings away and back, ending on `target`. `other` is
 * the opposite side. Returns how many moves were written (at most maxOut). With swings = 2:
 * { other, target, other, target }. */
static inline uint8_t servoReseatPlan(int target, int other, uint8_t swings, int *out, uint8_t maxOut) {
  uint8_t n = 0;
  for (uint8_t s = 0; s < swings && n + 2 <= maxOut; s++) { out[n++] = other; out[n++] = target; }
  return n;
}

/* After the `detection`-th cross-flow detection (1-based) in one pump start: retry (re-seat) while
 * detection <= maxReseats, otherwise give up and hold. maxReseats = 2 -> retry, retry, hold. */
static inline bool reseatAllowed(uint8_t detection, uint8_t maxReseats) {
  return detection >= 1 && detection <= maxReseats;
}
