/* =============================================================================
 *  level_guard.h  --  mixing-tank overflow / blind watch (pure, host-testable)
 * -----------------------------------------------------------------------------
 *  While an AC pump runs, ESP1 relays the Nano's fast ultrasonic level to ESP2 as
 *  LEVEL,<mixPct>,<resPct> (~3 Hz; -1 = no valid reading: lost echo, Nano silent).
 *  This header decides, from those samples and the clock alone:
 *    - LG_HIGH  : filling, and `need` valid samples in a row at/over the high limit (overflow coming).
 *                 Consecutive, so one splash or one corrupted line cannot stop a fill.
 *    - blindness: no valid sample for `blindMs` -- counted from the last valid sample, or, before the
 *                 first one, from the pump start plus `graceMs` (the Nano needs a moment to go FAST).
 *  What ESP2 DOES with either (stop and carry on / hold / just warn) lives in main.cpp.
 *  No Arduino types: `pio test -e native` runs test/test_level_guard on the host.
 * ========================================================================== */
#pragma once
#include <stdint.h>

struct LevelGuard {
  bool     active;          // a pump is on and being watched
  bool     transfer;        // true = filling the mixing tank (the only direction that can overflow)
  uint32_t startMs;         // when the pump came on
  uint32_t lastValidMs;     // last valid mix sample (0 = none yet this run)
  uint8_t  highRun;         // consecutive valid samples at/over the high limit
  float    lastPct;         // last valid mix level (for the alert text)
  bool     blindReported;   // a drain's DEGRADED,LEVEL_BLIND has been sent (once per run)
};

enum LevelVerdict { LG_OK = 0, LG_HIGH = 1 };

static inline void lgStart(LevelGuard &g, bool transfer, uint32_t now) {
  g.active = true; g.transfer = transfer; g.startMs = now; g.lastValidMs = 0;
  g.highRun = 0; g.lastPct = -1.0f; g.blindReported = false;
}

static inline void lgStop(LevelGuard &g) { g.active = false; g.highRun = 0; }

// One LEVEL sample. mixPct < 0 is the honest "no reading" sentinel: it neither counts towards the
// high run nor refreshes the blind clock (and it does not reset the run either -- only a valid
// reading under the limit does).
static inline LevelVerdict lgSample(LevelGuard &g, float mixPct, uint32_t now, float highPct, uint8_t need) {
  if (!g.active || mixPct < 0.0f) return LG_OK;
  g.lastValidMs = now ? now : 1;            // 0 is reserved for "none yet"
  g.lastPct = mixPct;
  if (mixPct >= highPct) { if (g.highRun < 255) g.highRun++; }
  else g.highRun = 0;
  return (g.transfer && need > 0 && g.highRun >= need) ? LG_HIGH : LG_OK;
}

// Clock check (every loop pass): has the watch gone blind? Wrap-safe unsigned differences.
static inline bool lgBlind(const LevelGuard &g, uint32_t now, uint32_t graceMs, uint32_t blindMs) {
  if (!g.active) return false;
  if (g.lastValidMs == 0) return (uint32_t)(now - g.startMs) >= graceMs + blindMs;
  return (uint32_t)(now - g.lastValidMs) >= blindMs;
}
