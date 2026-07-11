# ESP2 Hard-Reset Recovery + Protect Active Run + Checkpoint/Resume — HELD PLAN

> Status: **ON HOLD** (parked 2026-07-11). Not implemented. Saved so we can pick it up later.
> The urgent ESP2 boot bug that blocked comm was fixed separately (WDT-first + I²C bus-recovery +
> deferred PZEM in `ESP2 setup()`, and `POWER_CYCLE_OFF_MS` 1500→3500 in ESP1) — comm now works, so this
> plan is now about **robustness**, not a live outage.
> Cloud/Web dashboard is tracked separately in `Web_Dashboard_Firebase_Plan.md`.

## Context
Three linked requests:
1. **ESP2 recovery should hard-reset every time** (skip the soft `RESET_SELF`-first step).
2. **During an irrigation/fertigation run, nothing should accidentally power-off/reset ESP2** and lose the
   in-progress sequence.
3. **If ESP2 does reset mid-run, it should report the leftover work order** so the user decides to
   **push through (resume) or abort**.

Key insight: **Layer 3 (checkpoint) is what makes Layer 1 (hard reset) safe** — once ESP2 checkpoints its
progress, a hard reset mid-run no longer loses the sequence; it resumes from the checkpoint after the user OKs.

Findings from exploration (this session):
- **Recovery** (`ESP1 RECOVERY_STATE`, `esp2Escalate`/`esp2PowerCycle`) currently tries a **soft
  `RESET_SELF` first** (if ESP2 is powered), waits 10 s, then hard-cycles GPIO4.
- **During a run**, idle-poll / new-dispatch / min-on-off are gated on `sysState != IDLE_STATE`, but **three
  paths still hit ESP2 mid-run**: `heartbeatTick` ESP2-silence recovery (power-cycle), `deviceHealthTick`
  ESP1 self-reset (`ESP.restart` drops GPIO4), and `UART_DONE_TIMEOUT` (10 min) → `enterEmergencyStop`
  **force-cut**.
- **ESP2 forgets everything on reset** — only `mixTankL` persists in NVS (`tankSave/tankLoad`, ns `"esp2"`).
  Step/dose/targets are volatile, so a mid-run reset loses the batch today. But the RESUME command and
  resume-aware FILL/DELIVER (off `mixTankL`) already exist — so checkpointing the rest is contained.

## Recommended defaults (revisit at implementation)
Build **all three layers**; recovery = **always hard power-cycle**; on a mid-run reset **hold + ask**
(default cursor Hold) via LCD+SMS; add an explicit **ABORT** option.

## Layer 1 — recovery always hard-resets (`ESP1/src/main.cpp`)
- In `RECOVERY_STATE`: drop the soft-`RESET_SELF`-first branch; on entry go straight to `esp2Escalate()`
  (GPIO4 hard cycle). Keep the `esp2CommLost` bail, `esp2ReinitUart()`, the fast-cycle cap, 20-min
  `esp2CommRetryTick` slow-retry, and one/day `esp1SelfResetOncePerDay`. Retire the unused
  `ESP2_SOFT_RESET_TIMEOUT_MS` wait + soft-READY branch in `handleEsp2Response`. Leave ESP2's `RESET_SELF`
  handler harmless/unused.

## Layer 2 — shield an active run (`ESP1/src/main.cpp`)
Gate the three un-protected paths on `wo.active` / `sysState == ACTIVE_STATE`:
- **`deviceHealthTick`**: if a run is active, **defer** the `ESP.restart()` self-reset until the run finishes.
- **`heartbeatTick` ESP2-silence**: during a run, a confirmed silence must **not** casually power-cycle —
  route it into the safe hard-reset-and-resume path (Layer 3), not a blind cut. (Healthy runs heartbeat
  every 5 s, so this won't false-trip; only a genuinely dead ESP2 reaches it.)
- **`UART_DONE_TIMEOUT`**: during a run, convert the force-cut E-stop into the **hold + prompt** path (retain
  tank, ask the user) rather than `enterEmergencyStop(true)` wiping the batch.

## Layer 3 — checkpoint + resume-report
**ESP2 (`ESP2/src/main.cpp`)** — extend the existing `Preferences` ns `"esp2"` (today only `mixL`):
- **Checkpoint** the work order at each `goStep()`/dose boundary (not per-loop, to spare NVS): serialize
  `wo` (col, fertigate, flushPct, dose[3], doseCeilMs[3], ec/ph, mixMs, cmdName) + `step` + `doseIdx` +
  `woWaterL` + a `jobActive` flag. **Clear** it in `finishOk()` and `stopAll()`.
- **On `setup()`** after `tankLoad()`: if `jobActive`, rehydrate `wo`/`step`/`doseIdx`/`woWaterL`, set
  `faultHeld=true` (paused; keep P17 down / SAFE), and emit
  `INFO,RESUME_AVAILABLE,<cmd>,<step>,<remainingL>,<dosesLeft>` (in addition to `READY,ESP2`). `resumeWork()`
  already re-energizes P17 and continues from `step`.
- **New `ABORT` command**: clear checkpoint + `wo` + `step`, **keep `mixTankL`**, reply `ACK,ABORT`, go idle.

**ESP1 (`ESP1/src/main.cpp`)** — handle the report + user decision (reuse the fault-hold UI):
- Parse `INFO,RESUME_AVAILABLE,...` in `handleEsp2Response`. Enter a hold (like `enterFaultHold`): set
  `esp2Held`, `lcdPage=PAGE_FAULT`, default cursor Hold, and show the leftover (`COL_x`, op, remaining L /
  doses). SMS the same with `reply RESUME/ABORT/RELEASE/IRRIGATE`.
- Extend `issueRecovery()` / `faultRecovSel` menu (`RECOV_NAMES`, LCD render, SMS handler): add **RESUME**
  (→ `sendEsp2("RESUME,NORMAL")`, re-arm DONE supervision, `ACTIVE_STATE`) and **ABORT**
  (→ `sendEsp2("ABORT")`, clear `wo`, re-queue the column, `IDLE_STATE`). Keep Release / Irrigate-only / Hold.

## Related open discussion (parked, decide at implementation) — excludes Cloud/Web
- **Keep ESP2 always powered vs OFF-during-idle relay model.** The OFF-during-idle model forces repeated
  relay cold boots (the class of bug we just fixed). Keeping ESP2 always powered and using the PCF8575
  **P17 master cutoff** for actuator safety would remove that whole failure class, at a small idle-power
  cost. If pursued, it makes Layer 1 (hard reset) largely moot and changes Layer 2/3 assumptions.
- **Battery ADC calibration**: `calBattV`/`calBattI` in ESP1 are still placeholder coefficients — run the
  bench tool (`Test Code/ESP1_ADC_OPTO_CAL`) and paste real `a0..a3` before trusting battery V/I.
- **Harden the boot fix further** (optional): if the rig ever shows a *blank* ESP2 banner at relay power-on,
  that's brownout/EN-timing hardware — add a 1–10 µF cap on ESP2 EN→GND and ensure the relay/supply handles
  the inrush of ESP2 + PCF8575 + PZEM + relay coils.

## Docs to update when implemented
CLAUDE.md (recovery ladder → hard-only; run-shield; checkpoint/resume + ABORT), Operator_Manual (new
RESUME/ABORT recovery choices + a mid-run-reset resume prompt), FINAL spec §18.9 recovery + work-order resume.

## Verification (when built)
- Build both: `pio run -e esp32dev` in `ESP1/` and `ESP2/`.
- Bench traces: (1) force a recovery → confirm it hard-cycles immediately (no soft wait). (2) Start a run,
  simulate an RTC/SD dropout → ESP1 does NOT reboot until the run ends. (3) Mid-run, pull GPIO4 → on reboot
  ESP2 emits `RESUME_AVAILABLE`, ESP1 holds + prompts; **RESUME** continues from the checkpointed step and
  finishes with correct totals (no tank overfill via `mixTankL`); repeat and **ABORT** → run ends,
  `mixTankL` retained, column re-queued. (4) Confirm a clean run clears the checkpoint (no false
  RESUME_AVAILABLE on the next normal boot).
