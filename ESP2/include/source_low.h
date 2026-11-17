/* =============================================================================
 *  source_low.h  --  "the tank feeding the pump is nearly empty" stop (pure, host-testable)
 * -----------------------------------------------------------------------------
 *  User rule, 2026-11-16: the flow sensors pick up electrical noise, so a pump that moves no more water can
 *  still count pulses, and the flow-based dry stop (FLOW_DRY_MS) then never fires. So, on a transfer (the
 *  reservoir feeds the pump) or a drain (the mixing tank feeds it), the ULTRASONIC level of that source tank
 *  decides as well:
 *    - `need` valid readings in a row under `lowPct` start a countdown;
 *    - `need` valid readings in a row at or over `lowPct` cancel it (a splash or one bad echo cannot do it);
 *    - the countdown reaching `holdMs` = stop the pump, whatever the flow sensor reads.
 *  A reading < 0 (no echo, Nano silent, not calibrated) changes nothing.
 *  What ESP2 DOES on the stop (normal end, no fault, no alert) lives in main.cpp.
 *  No Arduino types: `pio test -e native` runs test/test_source_low on the host.
 * ========================================================================== */
#pragma once
#include <stdint.h>

struct SourceLow {
  uint8_t  lowRun;          // valid readings in a row under the limit
  uint8_t  okRun;           // valid readings in a row at/over the limit
  bool     counting;        // the countdown is running
  uint32_t sinceMs;         // when it started
  float    lastPct;         // last valid reading (for the log line), -1 = none
};

static inline void slReset(SourceLow &s) {
  s.lowRun = 0; s.okRun = 0; s.counting = false; s.sinceMs = 0; s.lastPct = -1.0f;
}

// One level sample of the SOURCE tank (percent; < 0 = no valid reading).
static inline void slSample(SourceLow &s, float pct, uint32_t now, float lowPct, uint8_t need) {
  if (pct < 0.0f) return;
  s.lastPct = pct;
  if (pct < lowPct) {
    s.okRun = 0;
    if (s.lowRun < 255) s.lowRun++;
    if (!s.counting && s.lowRun >= need) { s.counting = true; s.sinceMs = now; }
  } else {
    s.lowRun = 0;
    if (s.okRun < 255) s.okRun++;
    if (s.counting && s.okRun >= need) s.counting = false;
  }
}

// Clock check (every loop pass): has the countdown run out? Wrap-safe unsigned difference.
static inline bool slDue(const SourceLow &s, uint32_t now, uint32_t holdMs) {
  return s.counting && (uint32_t)(now - s.sinceMs) >= holdMs;
}
