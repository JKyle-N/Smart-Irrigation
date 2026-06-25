# Settings: Calibration Mode, Edit-Confirmation, and Restore-Defaults

**Companion spec to** `Smart_Irrigation_System_Reference_FINAL.md`.
Covers three remaining-to-code features, all living under the ESP32 #1 LCD menu and NVS config authority. Nothing here changes locked architecture; it extends the existing Source-of-Truth (§10.1, §10.10) and command patterns.

Status legend: **LOCKED** = decided, **PLACEHOLDER** = editable constant set at commissioning, **PENDING** = downstream task.

---

## A. Calibration Mode

### A.1. Purpose & Principle

Calibrate every field sensor **from the LCD, without reflashing any microcontroller**. Today, changing a soil-moisture map or a flow K-factor means editing code and reflashing the Nano or ESP32 #2. Calibration Mode replaces that with an on-device workflow whose results are stored in ESP32 #1 NVS and pushed to the owning subsystem at startup sync — exactly the pattern already used for flow K-factors and EC/pH safe windows (§10.10.1, §14.2.2).

Core principles (LOCKED):
- ESP32 #1 owns the UI and is the **only** persistent store for calibration constants. The Nano stores no persistent config (§10.3); ESP32 #2 holds runtime copies only (§10.2).
- Calibration reads and stores **RAW** sensor signals (raw ADC counts, raw pulse counts, raw Modbus registers), never the already-corrected value.
- Calibration constants are a **separate NVS namespace** from operational config (see §C: Restore-Defaults explicitly excludes calibration).
- Entry is **IDLE_STATE only**, behind a hard lockout (§A.6).

### A.2. Entry & Sub-Menu

Calibration Mode is a Settings entry, reachable only from IDLE_STATE. On entry the system transitions to a dedicated `CALIBRATION_STATE` (new local UI state on ESP32 #1; automation suspended, see §A.6).

The user picks **which component** to calibrate. Disabled columns (§14.1.4.1) are hidden from the per-column lists.

```
SETTINGS > CALIBRATION
  ├─ Prime Lines        (separate menu, §A.4.2 — purge air before any flow cal)
  ├─ Soil Moisture      (per channel: A1 A2 B1 B2 [C1 C2 if enabled])
  ├─ pH
  ├─ EC
  ├─ Flow Sensors       (Reservoir→Mix, Mix→Irrig, Nut A, Nut B, Nut C, [Nut D], pH Up, pH Down)
  ├─ Ultrasonic         (Reservoir, Mixing tank)
  ├─ NPK (offset trim)  (per enabled column)
  ├─ Environmental      (DHT22 offset, BH1750 offset)
  ├─ Current Sensors    (ACS712 zero; INA226/PZEM factory-trimmed, view-only)
  └─ EXIT
```

### A.3. Live Raw Streaming (the CAL channel)

While a sensor is selected for calibration, ESP32 #1 needs its **raw** value at interactive speed. Reusing the normal 10s / 20min / 1hr sensor cadence (§17.4.4) is unusable for calibration.

**LOCKED mechanism — dedicated CAL streaming:**
- On selecting a sensor, ESP32 #1 sends `<START>,CAL_START,<SENSOR_ID>,<END>` to the owning subsystem (Nano or ESP32 #2).
- That subsystem streams **only that sensor's raw value** at ~1–2 Hz using a new framed packet:
  `<START>,CAL,<SENSOR_ID>,<RAW_VALUE>,<END>`
- On exit/next-sensor, ESP32 #1 sends `<START>,CAL_STOP,<SENSOR_ID>,<END>`; the subsystem returns to normal cadence.
- CAL streaming is exempt from the garbage-counter logic for cadence (it is expected traffic), but malformed CAL packets still follow framing rules (§9.9).

New commands to register in the UART catalog:

| Command   | Direction        | Purpose                                  |
|-----------|------------------|------------------------------------------|
| CAL_START | ESP1 → Nano/ESP2 | Begin fast raw stream for one sensor      |
| CAL_STOP  | ESP1 → Nano/ESP2 | End raw stream, resume normal cadence      |
| CAL       | Nano/ESP2 → ESP1 | One raw-value sample for the active sensor |
| SET_CAL   | ESP1 → ESP2      | Push a calibration constant to ESP32 #2 runtime (Save-time + sync + work-order); ESP32 #2 ACKs (§A.5.1) |
| PRIME_START | ESP1 → ESP2    | Open line's valve(s) + run paired pump for priming; dead-man + generous cap enforced locally (§A.4.2) |
| PRIME_STOP  | ESP1 → ESP2    | Close valve(s), stop pump, return outputs to SAFE |

Ownership of each `SENSOR_ID` (which subsystem answers CAL_START) follows the existing read-ownership table (§5.5): soil/NPK/ultrasonic/DHT22/BH1750/reservoir-fill-flow → Nano; pH/EC/ACS712/PZEM/8× flow → ESP32 #2.

### A.4. Calibration Methods (accuracy recommendation)

**Recommendation: 2-point linear calibration as the default workhorse (3-point for pH), storing the RAW endpoints — not just the derived slope.** Storing endpoints lets the system re-derive slope/offset and lets you sanity-check drift later. Linear (`real = scale * raw + offset`) covers soil, pH, EC, flow, and ultrasonic well within thesis accuracy needs.

| Sensor | Method | Stored constants |
|---|---|---|
| **Soil moisture** (capacitive, per channel) | 2-point: (1) sensor dry in air, (2) sensor in water / saturated soil | `air_raw`, `water_raw` → 0–100% map per channel |
| **pH** | 2-point buffers pH 7.0 then 4.0 (3-point adds 10.0 for high-pH range) | slope, offset (+ raw points) |
| **EC** | 2-point: dry/air zero + known standard (e.g. 1.413 mS/cm) | cell-constant/scale, offset |
| **Flow ×8** | **1-point K-factor (pulses/L), measured 3× and averaged** — NOT multi-point (flow is zero-offset by nature). Full procedure in §A.4.1 | pulses-per-liter K-factor per sensor |
| **Ultrasonic** | 1-point: measured reference distance to empty-tank surface (tape measure) | mounting/zero offset |
| **NPK (RS485)** | Factory-calibrated. Per-element **offset trim** ships in v1 but **defaults to 0** and is **guarded** (§A.4.3) — present for completeness, intentionally not used in normal operation | N/P/K offsets (default 0) |
| **DHT22 / BH1750** | Offset trim against a reference thermo-hygrometer / lux meter | temp/humidity/lux offset (default 0) |
| **ACS712** (mixer current) | Zero-offset: read with motor OFF, store as zero point | zero-offset |
| **INA226 / PZEM** | Factory-trimmed; **view-only** raw vs. computed, no field cal | none (display only) |

Per-sensor workflow on screen:
1. Prompt the physical setup ("Place soil probe in air, press ENTER").
2. Show live raw value (CAL stream) until stable.
3. Capture endpoint on ENTER.
4. Repeat for the second/third point.
5. Show resulting computed value to confirm sanity.
6. **Save is explicit** (see §A.5) — nothing is written to NVS until the user saves.

### A.4.2. Prime Lines (dedicated menu)

A standalone menu under Calibration that lets the user purge air and fill pump lines before flow calibration (or any time a line has been opened/serviced). Priming a dosing pump against an empty line gives a false low pulse count and ruins flow cal — so Prime exists to fill lines first. **LOCKED: separate menu listing all lines** (not buried per-channel), so the user can prime everything in one pass before calibrating.

#### A.4.2.1. Valve-opens-with-pump (the key behavior)

A pump cannot move fluid into a closed path. So Prime does **not** just run a pump — for each line it **opens the correct solenoid valve AND runs the paired pump together**, both held under the dead-man. ESP32 #2 owns this pairing (it owns all PCF8575 outputs, §19.4.7).

Line → (valve, pump) pairing for Prime:

| Prime target | Valve(s) opened | Pump run | Path |
|---|---|---|---|
| Reservoir → Mixing | Reservoir Valve (P0) | Transfer Pump (P6) | Reservoir into mixing tank |
| Mixing → Irrigation | Mixing Tank Valve (P4) + a column valve (P1/P2/P3) | Booster Pump (P7) | Mixing tank out to a column |
| Nutrient A / B / C | (doses into mixing tank — no column valve) | Nut A/B/C Pump (P11/P12/P13) | Bottle → mixing tank |
| pH Up / pH Down | (doses into mixing tank) | pH Up/Down Pump (P15/P16) | Bottle → mixing tank |
| [Nutrient D if used] | (doses into mixing tank) | Nut D Pump (P14) | Bottle → mixing tank |

Notes:
- For **Mixing→Irrigation** priming the user picks which column valve to open (only one column path is primed at a time; disabled columns hidden, §14.1.4.1).
- Dosing/pH pumps discharge **into the mixing tank** (confirmed against the physical build) — no column solenoid is involved, so priming these runs just the bottle's pump. Ensure the mixing tank can accept the primed volume (it is small — mL-scale per prime).
- On stopping a prime, ESP32 #2 returns ALL touched outputs to SAFE (valve closed, pump off) per §19.5.4.

#### A.4.2.2. Control & Safety (dead-man + generous cap)

Same dead-man pattern as TEST_MODE and flow cal, with a **longer cap** because purging air legitimately takes longer than a calibration burst:

- **Hold ENTER → ESP32 #2 opens the line's valve(s) and starts the paired pump together.** Live flow pulses (if that line has a flow sensor) shown on screen to confirm fluid is actually moving.
- **Release ENTER → valve closes and pump stops immediately** (full dead-man; both drop together).
- **Generous safety cap (PLACEHOLDER, ~30–60 s, LOCKED as dead-man + cap):** ESP32 #2 stops and closes the line if the cap elapses even while ENTER is held. User may re-hold to continue priming in another cap-bounded run. This bounds a stuck-button / walk-away failure while still allowing long purges.
- **Idle-only**, inside CALIBRATION_STATE (§A.6); automation suspended.
- Entry/exit and each prime run logged (§A.7 / §25).

#### A.4.2.3. UART

Prime reuses the framed command pattern. ESP32 #1 tells ESP32 #2 which line to prime; ESP32 #2 performs the valve+pump pairing and enforces dead-man + cap locally:

`<START>,PRIME_START,<LINE_ID>[,<COL>],<END>`  → arm valve+pump for that line
`<START>,PRIME_STOP,<LINE_ID>,<END>`           → close valve, stop pump, return SAFE

(ENTER hold/release is sensed on ESP32 #1's button and conveyed as the start/stop, OR — preferred for tightest safety — ESP32 #1 sends PRIME_START on hold and ESP32 #2 self-stops on cap; ESP32 #1 sends PRIME_STOP on release. The hard cap is always enforced locally by ESP32 #2 so a lost UART packet can never leave a pump running.)



Flow is the only calibration that moves water, spans two controllers (UI on ESP32 #1, sensor+pump on ESP32 #2), and needs the dead-man safety. It is also the only one that is **single-factor**, not multi-point.

#### A.4.1.1. Why flow is single-point (not 2-point)

A flow sensor produces pulses proportional to volume, through the origin: zero flow = zero pulses, with no offset to correct. So the calibration is one number — **K = total_pulses ÷ known_liters** (pulses per liter). A 2-point line with an offset (as used for pH/EC) is wrong here. Accuracy comes not from more points but from **repeating the same measurement and averaging** (§A.4.1.4).

#### A.4.1.2. Two physical classes (different flow rates → different setup)

The 8 sensors split into two very different regimes:

| Class | Sensors | Pump | Flow rate | Reference vessel | Run style |
|---|---|---|---|---|---|
| **Main-line** | Reservoir→Mix, Mix→Irrigation | AC transfer / booster | High (L/s) | Large (≥5 L bucket) | Short burst (~1–3 s) — 10 s would overfill |
| **Dosing-line** | Nut A, Nut B, Nut C, pH Up, pH Down (Nut D if used) | Small dosing pumps | Low (mL/s) | Graduated cylinder (e.g. 250–500 mL) | Accumulate across bursts — a single short run passes too little to count accurately |

This is why a single fixed 10 s cap can't serve both: main-line overfills, dosing-line under-fills. Resolved in §A.4.1.3.

#### A.4.1.3. Per-channel safety cap (generalized TEST_MODE pattern)

The locked TEST_MODE safety (dead-man ENTER held + hard cap enforced **solely by ESP32 #2**) is kept. The only change: the **cap value becomes a per-channel calibration constant** instead of one hardcoded 10 s.

- **Dead-man (universal):** the pump runs **only while ENTER is physically held**. Release → pump stops instantly. ESP32 #2 enforces this locally.
- **Per-channel hard cap (PLACEHOLDER, set at commissioning):** e.g. main-line ~3 s, dosing-line ~10–15 s. ESP32 #2 stops the pump when the cap elapses even if ENTER is still held.
- **Accumulate-across-bursts (dosing class):** the pulse counter is **not** reset between bursts within one run — the user can press-hold-release several times to fill the cylinder to a readable mark, and pulses sum across those bursts. ESP32 #2 holds the running total; ESP32 #1 displays it live.

#### A.4.1.4. Procedure (one channel)

Idle-only, in CALIBRATION_STATE (§A.6). Pulse counting lives on ESP32 #2; volume entry and the K math live on ESP32 #1.

1. **Select channel.** ESP32 #1 → `<START>,CAL_START,FLOW_<ch>,<END>`. ESP32 #2 arms only that channel's pump + flow ISR, zeroes that channel's pulse counter, and begins streaming live pulse count via `CAL` packets.
2. **Choose reference method (per sensor, user-selectable):**
   - **Catch:** run the pump, catch output in the vessel, then read the caught volume. (Good for main-line into a bucket.)
   - **Drain:** pre-fill a marked known volume on the supply side, run until it's drawn down by that marked amount. (Good when catching is awkward.)
3. **Run (dead-man):** user holds ENTER; pump runs; live pulse count climbs on screen. Main-line: one short burst. Dosing-line: repeat bursts until the cylinder reaches a clean graduation. ESP32 #2 enforces dead-man + per-channel cap throughout.
4. **Enter the known volume** read off the vessel (e.g. `0.500 L`) via the LCD/buttons.
5. ESP32 #1 computes this run's **K = pulses ÷ liters** and shows it.
6. **Repeat 3× total** (LOCKED). The system stores three independent K values.
7. **Outlier check (LOCKED):** if the three K-factors disagree beyond a tolerance band (PLACEHOLDER, e.g. ±5–10 % from their median), ESP32 #1 **flags it** and offers to redo the worst run rather than averaging bad data. If they agree, it averages them into the final K.
8. **Confirm & Save** (§A.5): final averaged K is written to NVS and pushed to ESP32 #2 with blocking ACK (§A.5.1), since ESP32 #2 uses K live during dosing.
9. ESP32 #1 → `CAL_STOP`; channel returns to normal.

#### A.4.1.5. Notes
- **Counting authority:** ESP32 #2 owns the pulse count (it owns the flow ISRs, §19.4.5); ESP32 #1 owns the volume entry, the K math, the averaging, the outlier check, and NVS. Clean split along existing authority lines.
- **Priming:** before the first counted run, fill/purge the line using **Prime Lines (§A.4.2)** so air slugs don't corrupt the count. Prime opens the line's solenoid + runs its pump together under dead-man. For dosing lines especially, prime until live pulses confirm steady fluid before counting.
- **Placeholders to set at commissioning:** per-channel hard cap, outlier tolerance band, recommended reference volume per class.

### A.4.3. NPK Offset Trim (guarded, default zero)

The RS485 7-in-1 NPK sensors are factory-calibrated. A per-element offset trim **ships in v1 for completeness**, but it is deliberately constrained because misusing it would corrupt thesis data:

- **Default is 0** for N, P, and K. Out of the box the system records **raw factory NPK output** — the honest, independent measured OUTCOME (§12.4, §14.2.0). Dosing amount is open-loop off the lab baseline, so the NPK reading must stay an unbiased observation, not something tuned to match an assumption.
- **Guarded edit:** the NPK trim screen shows an explicit warning before any change — that trimming toward the lab baseline biases the very data the thesis reports, and that it is not needed for normal operation. The user must confirm past this warning to edit.
- **Restore-Defaults note:** although calibration generally survives Restore-Defaults (§C), the NPK offset is the one calibration value a user is most likely to have set in error. It still lives in the calibration namespace (untouched by Restore-Defaults), but the guard above is the primary protection. Resetting NPK offset to 0 is available **inside** the NPK trim screen itself.
- Stored as N/P/K offset constants in the calibration NVS namespace; applied by ESP32 #1 when interpreting the Nano's raw NPK packet (Nano stays raw, §A.5.1).


- New endpoints/offsets are held in a **scratch (RAM) copy** during calibration.
- Data is committed to NVS **only** when the user chooses **Save** for that component.
- On Save: ESP32 #1 writes the calibration namespace in NVS, then distributes updated runtime constants to the owning subsystem per §A.5.1 (Nano vs. ESP32 #2 differ — this matters).
- Abandoning (back/exit without Save) discards the scratch copy — the previous calibration stands.
- Each component saves independently; calibrating pH does not touch soil endpoints.

### A.5.1. Distribution & Reset-Resilience (Nano vs. ESP32 #2)

The two subsystems are NOT symmetric, because of the supervisor/work-order model (§9.8.1.1):

**Nano-owned sensors (soil, NPK, ultrasonic, DHT22, BH1750, reservoir-fill flow):**
The Nano streams RAW values and never applies calibration itself. ESP32 #1 applies the constants when it interprets the packet. Therefore the Nano needs **no** calibration data and is **immune to its own resets** w.r.t. calibration — a Nano reboot loses nothing relevant. Save simply updates ESP32 #1's NVS; nothing is pushed to the Nano.

**ESP32 #2-owned sensors (flow K-factors ×8, EC, pH, ACS712 zero):**
ESP32 #2 applies these constants **live, autonomously, during job execution** — it counts pulses and decides "target volume reached," and it converts raw EC/pH to compare against the safe window for local protective stops (§9.8.1.2, §14.2.4). So the actual constants MUST be present in ESP32 #2 RAM at execution time. Because ESP32 #2 stores runtime copies only (§10.2), **any ESP32 #2 reset (watchdog, power-cycle recovery, brownout) wipes them.** This is closed with three layered deliveries:

1. **Save-time push (immediacy, blocking ACK).** On Save of an ESP32 #2-owned calibration, ESP32 #1:
   - writes its own NVS, then
   - pushes the new constant: `<START>,SET_CAL,<SENSOR_ID>,<VALUE>,<END>`, and
   - **waits for ESP32 #2 ACK.** The calibration is NOT considered active/saved-as-live until the ACK returns. **No ACK → the save is blocked**: ESP32 #1 keeps the value in NVS but marks ESP32 #2 as not-yet-confirmed, shows an LCD warning, logs it, and retries the push (e.g. on next idle or next startup sync). Operation does not proceed as if calibration succeeded. *(User decision: block-until-ACK.)*

2. **Startup-sync delivery (post-reset baseline).** Calibration constants — flow K-factors, EC cal, pH cal, ACS712 zero — are **explicitly part of the runtime parameters ESP32 #1 distributes to ESP32 #2 during STARTUP_SYNC** (§10.7.2 step that distributes runtime params; §10.10.3). This covers every ESP32 #2 reset, since recovery re-runs startup sync (§10.7.7). **Spec gap fixed:** the master reference's startup-distribution payload must name calibration constants, not only thresholds/safe-windows.

3. **Work-order delivery (execution self-containment — the real guarantee).** The complete work order ESP32 #1 hands ESP32 #2 (§9.8.1.1) already carries dosing targets and EC/pH safe windows. It is extended to also carry the **job-critical calibration**: the flow K-factors for the pumps used in THIS job, plus EC/pH calibration. Result: ESP32 #2 never relies on remembered state to run a job — even a reset moments before the job is harmless, because the job sheet is self-sufficient. *(User decision: re-push in every work order, layered on top of Save-time push.)*

**Why all three:** Save-push gives immediate effect without waiting for a reset or job; startup-sync gives a correct baseline after any reset; work-order makes each job execution immune to a reset between sync and job. Defense in depth against the core risk: ESP32 #2 executing a dose on stale or missing calibration.

### A.6. Safety Lockout (LOCKED)

- Calibration Mode is enterable **only from IDLE_STATE**.
- On entry, automation is suspended: no scheduled service runs start; ESP32 #1 will not issue work orders.
- **Non-actuator calibrations** (soil, pH, EC read, ultrasonic, NPK, env, ACS712 zero) require **no** actuator motion — pure read + capture.
- **Flow calibration is the one exception that must move a pump.** It reuses the **already-validated TEST_MODE safety pattern**: dead-man ENTER button held by the user, plus a hard cap **enforced solely by ESP32 #2**. The cap is **per-channel** (not a single flat 10 s) because main-line and dosing-line flow rates differ by orders of magnitude — see §A.4.1.3. Releasing ENTER or hitting the cap stops the pump immediately.
- A GSM alert/log entry (`ACT`/`STATE`) marks calibration entry/exit so the event is in the SD log (§25).
- Exiting Calibration Mode returns to IDLE_STATE and resumes normal cadence on all subsystems (CAL_STOP broadcast as needed).

### A.7. Logging

Calibration entry, each Save (which component, old→new key constants), and exit are logged by ESP32 #1 to the daily CSV (§25.2.1). **LOCKED: a dedicated `CAL` event_type is added to the §25.2.1 enum** (rather than overloading `CMD`), because calibration is distinct — it is cross-controller, it changes dosing accuracy, and it is the one namespace Restore-Defaults never touches, so it should be independently greppable in the logs.

Proposed detail format for the `CAL` event_type:

| event_type | source | detail format | example detail |
|---|---|---|---|
| CAL | ESP1 | ACTION\|SENSOR\|old->new (ACTION = ENTER/SAVE/EXIT) | SAVE\|FLOW_NUTA\|448.0->451.2 |

- ENTER / EXIT mark calibration-mode boundaries; SAVE carries the per-component old→new constant(s).
- For multi-value saves (e.g. soil air_raw + water_raw), pack both with the secondary `|` delimiter, commas avoided per §25.2 rules.

---

## B. Edit-Confirmation in Mode Settings (anti-accidental-overwrite)

### B.1. Purpose

Protect user-editable values from accidental overwrite. Any Settings screen that **changes a stored value** must confirm intent before persisting, and must catch the "I wandered in and changed something, then tried to leave" case.

### B.2. Scope (LOCKED)

Applies to **all editable Mode Settings EXCEPT Lock Screen and Testing**:
- **Covered:** presets/targets (N-P-K, pH), per-column mode (AUTO / IRRIGATION_ONLY), plant names, thresholds/hysteresis, schedules, FLUSH_PCT, FERTIGATE_TRIGGER_GAP, EC/pH safe windows, mixing duration, retry limits, battery thresholds, daily-summary time, etc.
- **Excluded:** Lock Screen (no persistent values to protect) and Testing/TEST_MODE (transient, already dead-man-gated).
- **Calibration Mode** has its own explicit per-component Save (§A.5), so it does not also use this dialog — they are parallel, not nested.

### B.3. Behavior (LOCKED)

The trigger is **leaving an edit screen that has unsaved changes** (a "dirty" flag set the moment any value is modified):

```
On BACK / exit from a Settings edit screen:
  IF no values changed (not dirty):
     → exit silently
  IF values changed (dirty):
     → prompt three-way dialog:
        ┌─────────────────────────────┐
        │  Unsaved changes             │
        │   > SAVE                     │
        │     DISCARD                  │
        │     CANCEL                   │
        └─────────────────────────────┘
```

- **SAVE** → commit to NVS (and distribute to subsystems if the value is used at runtime, §10.10.4), then exit.
- **DISCARD** → revert to the last-saved value, then exit.
- **CANCEL** → stay on the edit screen, changes still pending (user changed their mind about leaving).

Edits are staged in a **RAM working copy**; NVS is written only on SAVE. This mirrors the Calibration save model, so the codebase has one consistent "scratch → confirm → commit" pattern.

### B.3.1. Concurrency & Interrupt Edge Cases (LOCKED)

The dialog must not let a local edit collide unsafely with remote commands or faults:

- **Remote SMS config-write during an unsaved local edit of the same item** → the remote write is **deferred (queued), applied when the local edit exits** (save or discard). Rationale: a remote `SET`/`MODE` silently overwriting a value you are mid-edit is exactly the accidental-overwrite this feature prevents — so the protection is symmetric (local and remote). The system may text back a brief "busy, applied after local edit" ack. *(User decision.)*
- **Critical / EMERGENCY_STOP fault fires while the dialog is open** → the dialog is **force-dismissed and the pending edit auto-discarded**; the UI jumps straight to the fault display. Safety overrides the unsaved edit — this is the §14.11 / §24 fail-safe priority applied to the UI. *(User decision.)*
- **Minor / Major (non-critical) fault while dialog open** → does not force-dismiss; it waits behind the dialog (backlight may signal per §18.10.4). The user answers the dialog, then sees the fault.
- **Backlight timeout / walk-away with dialog open** → treated as **CANCEL, never auto-save**. The edit state persists in RAM (un-committed) until the user returns or a higher-priority event clears it. A value nobody confirmed is never written to NVS.

### B.4. Notes

- The dirty flag is per-screen and cleared on SAVE or DISCARD.
- Backlight wake/fault behavior (§18.10) is unaffected.
- This is purely an ESP32 #1 UI concern; no new UART traffic.

---

## C. Restore Defaults ("Set All to Default")

### C.1. Purpose (LOCKED)

One action to wipe **custom operational configuration** back to firmware defaults — useful after misconfiguration or for a clean redeploy.

### C.2. Scope (LOCKED)

- **Resets:** presets/targets, per-column modes, plant names, thresholds/hysteresis, schedules, FLUSH_PCT, FERTIGATE_TRIGGER_GAP, EC/pH safe windows, mixing duration, retry limits, battery thresholds, daily-summary time, user preferences — i.e. the operational-config NVS namespace.
- **Explicitly EXCLUDES calibration** (§A). Calibration lives in a separate NVS namespace and is **never** touched by Restore Defaults. Rationale: calibration reflects physical sensor reality and is expensive to redo; wiping it on a config reset would be destructive and surprising.
- **Explicitly EXCLUDES column-enable flags** (`COLUMN_ENABLED`, §14.1.4.1). These reflect **physical wiring**, not user preference — Column C is disabled because it is physically unwired. Resetting them could re-enable Column C and make the system read floating pins and try to service a nonexistent column. Same logic as excluding calibration: hardware reality is not "settings." *(User decision.)*

This separation is the main reason §A mandates a distinct calibration NVS namespace.

### C.3. Behavior (LOCKED)

- Lives under Settings; **double-confirm** because it is destructive and broad:
  ```
  Restore all settings to default?
  Keeps calibration + column setup.
     > NO
       YES, RESET
  ```
- On confirm: ESP32 #1 overwrites the operational-config namespace with the compiled-in defaults, then **distributes the defaulted runtime params to subsystems using the §A.5.1 ACK-confirmed push** (NOT fire-and-forget). Any ESP32 #2-held value — notably the **EC/pH safe windows** — must be confirmed by ACK, exactly like calibration, or ESP32 #2 could keep enforcing the *old* safe window after a reset-to-defaults. If ESP32 #2 does not ACK, ESP32 #1 flags it and retries (same handling as §A.5.1). *(User decision: reuse calibration's ACK push.)*
- **Idle-only:** Restore-Defaults is enterable only from IDLE_STATE. Wiping thresholds and safe windows mid-job is unsafe; the action is blocked while not idle. *(Consistent with calibration's idle-only rule, §A.6.)*
- Logged via a `RESET`-class entry (§25.2.1) recording the reset event.
- Defaults come from the **clearly-labeled editable default constants** already mandated for thresholds, presets, FLUSH_PCT, etc. (so "default" always means "the commissioning defaults in code," single source).
- Does **not** force a reboot; it reinitializes runtime config in place and returns to IDLE_STATE.

### C.4. Relationship to the Other Two Features

- Restore-Defaults is itself a destructive edit → it uses a **dedicated double-confirm** (§C.3), not the generic three-way dialog (§B), because there is no "edit screen" to stay on.
- After a reset, every value reads back as its default; the dirty/confirm machinery (§B) applies normally to any subsequent edits.

---

## D. Cross-Feature Summary

| Concern | Calibration (A) | Mode-Settings edits (B) | Restore Defaults (C) |
|---|---|---|---|
| NVS namespace | calibration (separate) | operational-config | operational-config |
| Commit trigger | explicit per-component Save | three-way dialog on dirty exit | double-confirm |
| Touched by Restore Defaults? | **No** | Yes (is reset) | n/a |
| Entry restriction | IDLE only + lockout | none (idle nav) | IDLE only |
| New UART traffic | CAL_START / CAL / CAL_STOP / SET_CAL / PRIME_START / PRIME_STOP | none | ACK-confirmed param push (reuses §A.5.1 path) |
| Actuator motion | only flow cal + Prime (dead-man + per-channel/generous cap) | none | none |

### D.1. Open / PENDING items
- ~~SD-log event_type for calibration~~ → **resolved: dedicated `CAL` event_type** (§A.7); add to §25.2.1 enum.
- Confirm full `SENSOR_ID` enumeration for CAL packets (one per calibratable channel).
- ~~NPK offset-trim v1 vs defer~~ → **resolved: ships in v1, defaults 0, guarded** (§A.4.3).
- ~~Flow-cal target volume / container~~ → resolved in §A.4.1.2 (per-class); commissioning sets exact per-channel reference volume, hard cap, and outlier tolerance band.
- ~~Dosing-pump prime routing~~ → **confirmed: into mixing tank, no column valve** (§A.4.2.1).
- **Master-reference edits required (§A.5.1):** (a) add calibration constants to the STARTUP_SYNC distribution payload (§10.7.2 / §10.10.3); (b) extend the work-order contract (§9.8.1.1) to carry job-critical flow K-factors + EC/pH calibration; (c) register `SET_CAL`, `CAL_START`, `CAL_STOP`, `CAL`, `PRIME_START`, `PRIME_STOP` in the UART catalog (§9.7); (d) add the `CAL` event_type to the logging enum (§25.2.1).
