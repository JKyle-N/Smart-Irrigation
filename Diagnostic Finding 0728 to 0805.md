# Diagnostic Findings — 2026-07-28 to 2026-08-05

**System:** Smart Irrigation and Fertigation System
**Site:** Lamac Cooperative, Pinamungajan, Cebu
**Analysis date:** 2026-08-07
**Data reviewed:** 9 daily SD-card logs (`20260728.CSV` – `20260805.CSV`, 106,483 lines total) + 3 ThingSpeak channel exports (`System.csv`, `Columns.csv`, `Chem.csv`, covering 2026-07-10 → 2026-08-06)
**Firmware reference:** `JKyle-N/Smart-Irrigation` @ `7adb9b3` (2026-07-10)

---

## ✅ Remediation Status (updated 2026-08-07)

This section was verified against **current pushed firmware** (`main` @ `383a735`, 2026-08-07), not the stale `7adb9b3` the original analysis was written against. Several findings were **already resolved** in the ~4 weeks of firmware that has since been pushed; two more were **fixed today**.

| ID | Status | Note |
|---|---|---|
| C-1 | 🟡 Mostly fixed (firmware) | Measured calibration endpoints + raw-ADC-per-probe logging + railed-probe drop already in current firmware. Remaining: stuck-sensor watchdog (not yet), `soilStartPct` field re-eval (calibration). |
| C-2 | ✅ Fixed (firmware) | Per-probe SOIL packet, both raw probes logged (`X1=/X2=`), `soilCombine` drops a railed probe, `SOIL_DIVERGE` logged — all present in current firmware. |
| C-3 | ⛔ Not firmware | Genuine hardware flow fault — physical inspection required (do NOT touch `FLOW_FAIL` logic). |
| C-4 | ⛔ Blocked | Consequence of C-1/C-3; needs a real run. |
| **H-1** | ✅ **DONE today** | Added `NANO_SILENCE_TIMEOUT_MS = 300000` (separate from shared `HEARTBEAT_TIMEOUT_MS`); Nano silence now has 2.5× margin. |
| H-2 | ⛔ Not firmware | GSM/SIM800L hardware; re-measure after H-1. |
| H-3 | ⛔ Not straightforward | Hardware + deliberately-removed safety; needs decision, not a quick fix. |
| H-4 | ⛔ Needs observation | Requires physically watching the pump-exercise window. |
| M-1 | ⛔ Not firmware | Column-B RS485 hardware; retries deliberately NOT added (would mask fault rate). |
| M-2 | ⛔ Not firmware | EMI filter (hardware). |
| **M-3** | ✅ **DONE today** | `decideFertigate()` now logs `COL_x\|FERT_DOWNGRADE\|reason=NPK_INVALID` on the irrigation-only fallback. |
| M-4 | ⛔ Downstream | Resolves with M-1. |
| L-1 | 🟡 Partly fixed | Soil constants now measured (see C-1); NPK/column `[CONFIRM]` constants still open. |

Both today's fixes were mirrored to `Demo/ESP1-Force/src/main.cpp` and both firmwares build clean (`pio run -e esp32dev`). The per-finding sections below retain the **original analysis** (against `7adb9b3`); look for the **`✅ DONE`** / **verified** banners added inline.

---

## ⚠️ Read This First: Repository Sync — RESOLVED 2026-08-07

> **Update (2026-08-07):** the backlog has been pushed. `main` is now at `383a735` and includes the `HEALTH` / `CTRL` / per-sensor `SENSOR` telemetry, the per-probe soil work, and the measured calibration. The "out of sync" condition below is **historical**; it is what was true when the logs were analysed.

The repository contained **one commit**, dated **2026-07-10 09:27**. The logs from 2026-07-28 onward contain telemetry that **does not exist anywhere in that codebase**:

| Log string observed | Present in repo? |
|---|---|
| `ESP1,HEALTH,GSM:no\|WIFI:up\|RTC:ok\|SD:ok\|NANO:ok\|ESP2:off\|SOILA:100\|SOILB:99` | ❌ `SOILA` appears nowhere |
| `ESP1,CTRL,COL_B\|NOT_DRY\|soil=99>=35` | ❌ `NOT_DRY` appears nowhere |
| `ESP1,CTRL,COL_A\|OUTSIDE_WINDOW\|now=00:00\|win=11:22-12:00` | ❌ `OUTSIDE_WINDOW` appears nowhere |
| `ESP1,SENSOR,NPK_B\|FAIL` | ❌ Not in repo |

**Implication:** approximately four weeks of local firmware development has never been pushed. All code findings below are verified against the 2026-07-10 source. Where the deployed firmware may have diverged, this is flagged explicitly.

**Action required (highest priority, non-technical):** ✅ **DONE 2026-08-07** — current firmware pushed to `main` (`383a735`). Until this was done:
- The repo is not a valid source of truth for thesis documentation.
- Claude Code sessions will start from stale architecture and may reintroduce fixed bugs.
- No future log analysis can be reliably correlated to code.

---

## Executive Summary

| Metric | Value over 9 days |
|---|---|
| Successful irrigation runs | **0** |
| `DOSE` / `EVAL` events (fertigation logic) | **0** |
| `COL_A` blocked — outside window | 12,609 |
| `COL_B` blocked — outside window | 11,871 |
| `COL_B` blocked — soil not dry | 127 |
| `COL_A` blocked — soil not dry | 36 |
| Total faults logged | 2,223 (**100% `NANO_SILENCE`**, all assessed as false positives) |
| Serial frame corruption | 6.6% → 10.6% of daily log volume |

**Bottom line:** the system is running stably and logging cleanly, but has delivered **zero water and zero nutrients** across the entire period. Column A is blocked by a railed soil sensor that can never satisfy the dry threshold. Column B is blocked by the same soil gate and, behind it, an unresolved hardware flow fault. The fertigation decision path has still never executed once in the project's history.

---

## Severity Index

| ID | Severity | Issue | Root cause established? | Status (2026-08-07) |
|---|---|---|---|---|
| [C-1](#c-1) | 🔴 Critical | Column A soil sensor railed at 100% — irrigation structurally impossible | ✅ Code + calibration | 🟡 Mostly fixed (firmware) |
| [C-2](#c-2) | 🔴 Critical | Soil probe averaging hides single-probe failure | ✅ Code confirmed | ✅ Fixed (firmware) |
| [C-3](#c-3) | 🔴 Critical | Column B flow fault unresolved since 2026-07-24 | ⚠️ Hardware — inspection required | ⛔ Hardware (not firmware) |
| [C-4](#c-4) | 🔴 Critical | Zero fertigation events in 27 days of deployment | ✅ Consequence of C-1/C-3 | ⛔ Blocked on C-1/C-3 |
| [H-1](#h-1) | 🟠 High | `NANO_SILENCE` false positives — zero timing margin | ✅ Code confirmed | ✅ **DONE today** |
| [H-2](#h-2) | 🟠 High | GSM alerting ~68% failure rate | ⚠️ Partially — needs deployed source | ⛔ Hardware (not firmware) |
| [H-3](#h-3) | 🟠 High | Battery/power telemetry dead + safety disabled | ✅ Code + hardware | ⛔ Not straightforward |
| [H-4](#h-4) | 🟠 High | ESP2 reported `off`; pump exercise may be a no-op | ⚠️ Needs deployed source | ⛔ Needs observation |
| [M-1](#m-1) | 🟡 Medium | NPK-B ~45% read failure, no retry logic | ✅ Code + hardware | ⛔ Hardware (retries withheld by design) |
| [M-2](#m-2) | 🟡 Medium | Serial frame corruption ~10% and rising | ⚠️ EMI — known | ⛔ Hardware (EMI filter) |
| [M-3](#m-3) | 🟡 Medium | Fertigation silently downgrades on NPK fault | ✅ Code confirmed | ✅ **DONE today** |
| [M-4](#m-4) | 🟡 Medium | ThingSpeak Column B data 33–67% incomplete | ✅ Downstream of M-1 | ⛔ Downstream of M-1 |
| [L-1](#l-1) | 🟢 Low | Provisional constants still unmeasured (`[MEASURE]`/`[TBD]`/`[CONFIRM]`) | ✅ Code confirmed | 🟡 Soil measured; NPK still open |

---

# 🔴 CRITICAL

<a name="c-1"></a>
## C-1 — Column A soil sensor railed at 100%, irrigation structurally impossible

> 🟡 **VERIFIED vs current firmware (`383a735`, 2026-08-07) — mostly fixed.** The structural root cause is resolved: `calSoilAir = {656,707,800}` / `calSoilWater = {524,391,300}` are now **measured** (no longer `{800,800,800}`/`{300,300,300}` placeholders, ESP1 `:146-147`), and the raw ADC is now **logged per probe** (`SOIL|A=<pct>|A1=<raw>|A2=<raw>`, ESP1 `:1552-1558`) — required fixes 1 & 2 **done**. Still open: required fix 3 (stuck-sensor watchdog for zero-variance / rail-pinned readings — `soilCombine` only drops a *fully railed* probe, ESP1 `:176`) and fix 4 (`soilStartPct` field re-eval — a calibration decision, not firmware). The analysis below is the original `7adb9b3` finding.

### Evidence from logs

Across all 9 days and 2,571 `HEALTH` samples:

| Sensor | Distinct values observed |
|---|---|
| `SOILA` | `100` (2,553 times), `-1` (18 times) — **zero variance** |
| `SOILB` | `81, 84, 85, 86, 87, 90, 91, 92, 93, 94, 96, 97, 98, 99, 100` — responds normally |

Column B's probe tracks real drying behaviour. Column A has not moved once in nine days.

Control decisions logged:
```
2026-08-05 08:00:00,ESP1,CTRL,COL_B|NOT_DRY|soil=86>=35
```

### Root cause (three stacked code facts)

**(a) Calibration endpoints are unmeasured placeholder defaults** — `ESP1/src/main.cpp:128-129`

```cpp
int   calSoilAir[NUM_COLUMNS]   = { 800, 800, 800 };   // raw ADC dry (per column avg) [MEASURE]
int   calSoilWater[NUM_COLUMNS] = { 300, 300, 300 };   // raw ADC saturated            [MEASURE]
```

Both still carry the `[MEASURE]` marker — real bench values were never substituted.

**(b) The percentage mapping clamps hard, destroying all out-of-range information** — `ESP1/src/main.cpp:146-151`

```cpp
static int soilPct(int col, int raw) {
  long p = map(raw, calSoilAir[col], calSoilWater[col], 0, 100);
  if (p < 0) p = 0; if (p > 100) p = 100;
  return (int)p;
}
```

- Any raw ADC **≤ 300** → clamps to exactly **100**
- Any raw ADC **≥ 800** → clamps to exactly **0**

This explains the binary `A=100|B=0` pattern seen throughout earlier logs. Those were never real moisture readings — they were clamp rails. A saturated probe, a shorted probe, and a mis-calibrated probe are **indistinguishable** after this function runs.

**(c) The raw ADC value is discarded at conversion and never logged** — `ESP1/src/main.cpp:1329`

```cpp
tmp[c] = soilPct(c, v);      // raw value 'v' is never stored or logged
```

The Nano transmits raw ADC (correct design). ESP1 converts on receipt and stores only the mapped percentage into `sensor.soil[c]`. **No log file in the entire project contains a raw soil ADC value.**

### The blocking gate

`ESP1/src/main.cpp:2472-2473`, inside `controlTick()`:

```cpp
if (sensor.soil[c] < 0) continue;                // invalid soil (faulted elsewhere)
if (sensor.soil[c] >= soilStartPct) continue;    // not dry enough
```

With `soilStartPct = 35` (line 100, still tagged `[TBD]`), the evaluation `100 >= 35` is true on every cycle, forever. **Column A can never irrigate under any soil condition.**

Critically, this is a *silent* block. "Soil is wet" is a legitimate reason not to irrigate, so no fault is raised. The system has gone 9+ days delivering no water to Column A while reporting itself healthy.

### Required fixes

1. **Log raw soil ADC alongside the mapped percentage.** Highest diagnostic value per line of code changed. Without this, C-1 and C-2 cannot be diagnosed from logs at all.
2. **Measure real air/water endpoints per column** using the existing `CAL_START,SOIL_A1` / `SOIL_A2` calibration path, then replace the `{800,800,800}` / `{300,300,300}` defaults.
3. **Add a stuck-sensor watchdog:** if a soil reading shows zero variance for N hours (suggest 6h) or sits pinned at a clamp rail (0 or 100), raise a fault rather than silently reporting "not dry."
4. Re-evaluate `soilStartPct = 35` against measured field capacity once real readings exist.

---

<a name="c-2"></a>
## C-2 — Soil probe averaging hides single-probe failure

> ✅ **VERIFIED vs current firmware (`383a735`, 2026-08-07) — fixed.** The SOIL packet is now **per-probe** (`SOIL,<tag>,<probe1>,<probe2>`, parsed at ESP1 `:1528-1558`; both raw channels stored and logged as `X1=/X2=`). `soilCombine()` (ESP1 `:176`) drops a railed/disconnected probe instead of averaging it in, and a `SOIL_DIVERGE` event is logged when the two probes differ by >400 ADC (ESP1 `:1542`). Cross-wiring was also corrected (Column A = {B2,A2}, B = {B1,A1}). Note: divergence is currently **logged**, not raised as a hard fault, and the threshold is 400 (vs the 150 suggested here) — tighten if you want it to gate control. The analysis below is the original `7adb9b3` finding.

### Root cause

`Nano/Nano.ino:415`, inside `sendSoil()`:

```cpp
// RAW averaged ADC (companion spec §A): ESP32 #1 maps to % using the stored air/water endpoints.
int raw = (analogRead(SOIL_PINS[c][0]) + analogRead(SOIL_PINS[c][1])) / 2;
pktAddIntOrInvalid(raw, true);
```

Confirmed by the declaration comment at `Nano/Nano.ino:90`:
```cpp
// Soil capacitive sensors (2 per column, averaged). A6/A7 are analog-input only.
const uint8_t SOIL_PINS[NUM_COLUMNS][2] = {
  { A0, A1 },   // Column A
  { A2, A3 },   // Column B
  { A6, A7 },   // Column C
};
```

Two physical probes per column are averaged into a single value **before transmission**. Per-probe access exists **only** in the calibration path — `Nano/Nano.ino:482-491`:

```cpp
if (!strncmp(calId, "SOIL_", 5)) {
  if (calId[6] == '\0') {                    // "SOIL_A" -> column AVERAGE
    raw = (float)((analogRead(SOIL_PINS[col][0]) + analogRead(SOIL_PINS[col][1])) / 2);
  }
  int ch = calId[6] - '1';                   // "SOIL_A1"/"A2" -> single channel 0/1
  raw = (float)analogRead(SOIL_PINS[col][ch]);
}
```

### Compounding interaction with C-1

Worked example: if probe A0 reads a reasonable 600 and probe A1 is shorted reading 100:
- Average = 350
- `soilPct(0, 350)` with endpoints 800/300 → ≈ 90%
- 90 ≥ 35 → irrigation blocked
- **No log anywhere reveals that A1 is dead**

A single failed probe silently disables an entire column with no fault, no alert, and no diagnostic trace.

### Required fixes

1. **Change the SOIL packet format to carry per-probe values.** Suggested target format:
   ```
   SOIL|A1=<raw>|A2=<raw>|B1=<raw>|B2=<raw>|C=DISABLED
   ```
   This requires coordinated changes in `Nano/Nano.ino::sendSoil()` (emit both channels) and `ESP1/src/main.cpp:1318-1335` (parse the wider packet).
2. **Log the `SOIL_A2` raw calibration value.** Note: across the entire log history, `CAL,SOIL_A1,<value>` appears but **`CAL,SOIL_A2` never emits a raw value** despite `CAL_START,SOIL_A2` being issued. Verify the calibration stream handles channel index `2` correctly.
3. Add a **probe-divergence check**: if A1 and A2 differ by more than a threshold (suggest 150 ADC counts), raise a fault — this catches single-probe failure automatically.

---

<a name="c-3"></a>
## C-3 — Column B flow fault unresolved since 2026-07-24

### Evidence

The fault first appeared on the very first autonomous irrigation attempt:

```
2026-07-24 08:00:00,ESP1,CMD,ESP2|SEQ_IRRIGATION_B
2026-07-24 08:00:00,ESP1,ACT,IRRIGATION|START|COL_B
2026-07-24 08:00:12,ESP1,FAULT,CRIT|FLOW_FAIL|TRANSFER|IRRIGATION|COL_B|HELD
2026-07-24 08:00:12,ESP1,STATE,ACTIVE_STATE->EMERGENCY_STOP
```

Two manual recovery attempts on 2026-07-27 reproduced it identically:

```
2026-07-27 14:15:33  RESUME,NORMAL   -> ACTIVE_STATE -> 14:15:46 FLOW_FAIL -> EMERGENCY_STOP
2026-07-27 14:16:17  RESUME,IRRIGATE -> ACTIVE_STATE -> 14:16:30 FLOW_FAIL -> EMERGENCY_STOP
```

Same fault, same column, ~13 second latency, three consecutive times. **Fully reproducible.**

### The fault logic is correct — this is a real hardware condition

`ESP2/src/main.cpp:146`:
```cpp
const unsigned long FLOW_TIMEOUT_MS   = 10000;  // no-flow after pump start -> FLOW_FAIL
```

`ESP2/src/main.cpp:790`:
```cpp
if (!sgSawFlow && el > FLOW_TIMEOUT_MS) return -1;   // dry run / no flow
```

`ESP2/src/main.cpp:866`:
```cpp
else if (r == -1){ endMeteredStage(); pcfOff(OUT_RES_VALVE); holdFault("FLOW_FAIL", "TRANSFER"); }
```

The observed ~12–13s latency matches `FLOW_TIMEOUT_MS` (10,000 ms) plus `VALVE_SWITCH_MS` (1,000 ms) plus settle time. **No code bug.** The firmware is correctly reporting that no flow pulses were detected after the transfer pump started.

### Corroborating evidence: the fault is localised to Column B

| Day | NPK-A dead reads | NPK-B dead reads |
|---|---|---|
| 2026-07-24 | 0 / 5,009 (0%) | 2,236 / 5,373 (42%) |
| 2026-07-25 | 0 / 6,247 (0%) | 2,900 / 6,728 (43%) |
| 2026-07-26 | 0 / 6,275 (0%) | 3,240 / 6,788 (48%) |
| 2026-07-27 | 0 / 3,752 (0%) | 1,991 / 4,046 (49%) |

Column A is flawless. Column B fails roughly half the time and is trending worse. Both the NPK-B dropouts and the flow failure isolate to the same physical column — strongly indicating a **shared cable run or connector fault**, not system-wide EMI (which would degrade Column A equally).

### Required action — physical inspection (cannot be fixed in firmware)

Inspect, in order of likelihood:
1. **Flow sensor on the Column B transfer line** — connection, orientation, and whether it is physically in the flow path
2. **TRANSFER pump** — confirm it physically spins and moves water when commanded
3. **Column B cable run** — continuity and shielding, from the enclosure to the column
4. **Relay / PCF8575 output** for the Column B transfer valve
5. Reservoir level and inlet — confirm water is actually available to move

---

<a name="c-4"></a>
## C-4 — Zero fertigation events across the entire project history

Aggregate across **all** logs reviewed to date (2026-07-10 → 2026-08-05):

| Event class | Count |
|---|---|
| `DOSE` events | **0** |
| `EVAL` events | **0** |
| Completed irrigation runs | **0** |
| Manual `TEST` actuator commands | ~7,000+ |

The nutrient dosing decision pipeline (`decideFertigate()` → dose calculation → `SEQ_FERTIGATION_*`) has **never executed once**. All actuator activity to date has been manual commissioning tests or the scheduled pump-exercise routine.

**Thesis impact:** there is currently no dosing data of any kind. Every constant in the dosing chain (`fertGap`, per-crop `targetN/P/K`, stock dilution factor, the 12 mL dosing floor) remains unvalidated against real hardware behaviour. This is the single largest gap between current state and a defensible thesis dataset.

**Dependency chain:** C-4 cannot be resolved until C-1 (soil gate) and C-3 (flow fault) are both fixed, because the control logic never reaches the dosing branch.

---

# 🟠 HIGH

<a name="h-1"></a>
## H-1 — `NANO_SILENCE` false positives: zero timing margin

> ✅ **DONE 2026-08-07.** Confirmed still present in current firmware (`HEARTBEAT_TIMEOUT_MS = 120000` = Nano night heartbeat 120000 → zero margin). **Fix applied:** introduced a dedicated `NANO_SILENCE_TIMEOUT_MS = 300000` (5 min = 2.5× the slowest Nano heartbeat) used **only** for the Nano-silence fault (ESP1 `heartbeatTick()`), leaving the shared `HEARTBEAT_TIMEOUT_MS` on ESP2-silence and startup-sync paths untouched — so ESP2 detection is unaffected. Mirrored to `Demo/ESP1-Force`; both build clean. Expected to eliminate ~250 false faults + ~250 spurious SMS/day. The analysis below is the original finding.

### Evidence

`NANO_SILENCE` is the **only** fault type logged across all 9 days, at a near-constant rate with no trend:

| Date | Faults | All `NANO_SILENCE`? |
|---|---|---|
| 2026-07-28 | 175 | ✅ |
| 2026-07-29 | 260 | ✅ |
| 2026-07-30 | 259 | ✅ |
| 2026-07-31 | 259 | ✅ |
| 2026-08-01 | 260 | ✅ |
| 2026-08-02 | 255 | ✅ |
| 2026-08-03 | 256 | ✅ |
| 2026-08-04 | 245 | ✅ |
| 2026-08-05 | 254 | ✅ |

The near-perfect regularity (~1 per 5.5 min) indicates a systematic timing artifact, not random comms failure.

### Root cause — confirmed timing collision

| Constant | File / line | Value |
|---|---|---|
| `HEARTBEAT_TIMEOUT_MS` | `ESP1/src/main.cpp:176` | **120,000 ms** |
| `HB_INTERVAL_NIGHT_MS` | `Nano/Nano.ino:110` | **120,000 ms** |
| `HB_INTERVAL_DAY_MS` | `Nano/Nano.ino:109` | 60,000 ms |
| `HB_INTERVAL_ACTIVE_MS` | `Nano/Nano.ino:108` | 10,000 ms |

The detection logic — `ESP1/src/main.cpp:2585-2590`:

```cpp
if (!nanoSilent && millis() - sensor.lastNanoMs > HEARTBEAT_TIMEOUT_MS) {
  nanoSilent = true;
  faultsToday[2]++;
  logEvent("ESP1", "FAULT", "MIN|NANO_SILENCE|NANO");
  sendSMS("ALERT,MIN,NANO_SILENCE,NANO");
}
```

**The Nano's night heartbeat period exactly equals ESP1's silence timeout.** There is zero margin. Any scheduling jitter, any loop-timing variance, or any dropped frame (see M-2 — ~10% of frames are corrupted) trips the fault on a perfectly healthy Nano.

Day interval (60s) sits comfortably inside the 120s window, which is precisely why these faults cluster in the night window.

### Required fix

```cpp
// ESP1/src/main.cpp:176
const unsigned long HEARTBEAT_TIMEOUT_MS  = 300000;  // 5 min: 2.5x slowest Nano heartbeat (120s night)
```

Preferred alternative: derive the timeout from the current pace so it scales automatically —
```cpp
unsigned long nanoTimeoutMs() {
  switch (lastNanoPace) {
    case PACE_ACTIVE: return 3UL * HB_INTERVAL_ACTIVE_MS;   //  30 s
    case PACE_DAY:    return 3UL * HB_INTERVAL_DAY_MS;      // 180 s
    default:          return 3UL * HB_INTERVAL_NIGHT_MS;    // 360 s
  }
}
```

Note that `HEARTBEAT_TIMEOUT_MS` is also used for ESP2 silence detection (line 2593) and freshness checks (line 2380) — if changing the constant directly, verify those paths still behave correctly, or introduce a separate Nano-specific constant.

**Expected impact:** eliminates ~250 false faults and ~250 spurious SMS alert attempts per day.

---

<a name="h-2"></a>
## H-2 — GSM alerting is ~68% broken

### Evidence

| Date | `TX\|ALERT` (success) | `TX\|FAIL_NO_PROMPT` (failure) | Failure rate |
|---|---|---|---|
| 2026-07-28 | 58 | 118 | 67% |
| 2026-07-29 | 85 | 176 | 67% |
| 2026-07-30 | 85 | 175 | 67% |
| 2026-07-31 | 86 | 174 | 67% |
| 2026-08-01 | 88 | 172 | 66% |
| 2026-08-02 | 83 | 173 | 68% |
| 2026-08-03 | 83 | 174 | 68% |
| 2026-08-04 | 81 | 165 | 67% |
| 2026-08-05 | 80 | 175 | 69% |

Additionally, **`GSM:no` appears on every single `HEALTH` line across all 9 days** (2,571/2,571), and full status reports (`TX|RPT`) succeeded exactly **once** in nine days (2026-08-01).

`FAIL_NO_PROMPT` indicates the SIM800L is not returning the `>` prompt after `AT+CMGS` — typically SIM registration failure, insufficient signal, power brownout during transmit (SIM800L draws ~2A peak), or a module in an unrecovered error state.

### Why this matters beyond the alerts themselves

In the previous review period the system sat in `EMERGENCY_STOP` for **three full days** completely unnoticed. GSM alerting is the only out-of-band notification path in the architecture. With a 67% failure rate and a permanently-failing health check, the system is effectively operating blind.

### Note on interaction with H-1

Each of the ~250 daily false `NANO_SILENCE` faults calls `sendSMS(...)` (line 2589). Fixing H-1 will reduce GSM queue pressure by roughly 250 messages/day, which may itself improve GSM success rates. **Fix H-1 first, then re-measure H-2 before deep-diving the SIM800L.**

### Investigation steps

1. Confirm SIM registration status (`AT+CREG?`) and signal quality (`AT+CSQ`) at the site
2. Verify SIM800L supply can sustain 2A transient — check for brownout coinciding with `FAIL_NO_PROMPT`
3. Confirm SIM has credit / active data plan
4. Add exponential backoff and a retry ceiling so a failing modem doesn't re-attempt continuously

⚠️ The GSM handling code in the deployed firmware may differ from `7adb9b3` — verify against current source once pushed.

---

<a name="h-3"></a>
## H-3 — Battery/power telemetry dead and safety actions disabled

### Evidence

Every `PWR` line across all 9 days:
```
2026-07-28 00:00:00,ESP1,PWR,BATT|V=0.00|I=0.00|P=0.0|CONS=0Wh
```

| Date | `V=0.00` | `V=0.01` | Any valid reading |
|---|---|---|---|
| 2026-07-28 | 2,943 | 100 | **0** |
| 2026-08-01 | 2,720 | 205 | **0** |
| 2026-08-05 | 2,317 | 30 | **0** |

Confirmed independently in ThingSpeak `System.csv` — fields 6/7/8 read `0.00, 0.0, 0.0` continuously through 2026-08-06.

### Two independent failures stacked

**(a) Hardware:** the known PS817C voltage-sensing circuit missing its pull-up resistor. No valid voltage reading is physically possible.

**(b) Firmware:** battery safety actions were deliberately removed. Commit `7adb9b3` message:
> *"Remove INA226 battery safety actions; Testing: no auto-faults/e-stop, 30s cap"*

Confirmed in `ESP1/src/main.cpp:2457`:
```cpp
// (battery-critical run-block removed per user request -- battery no longer stops irrigation)
```

### Impact

The documented battery thresholds are **entirely inert**:

| Threshold | Configured | Actual behaviour |
|---|---|---|
| Normal | > 12.0 V | Never evaluated |
| Warning | 11.5 V | Never evaluated |
| Pump stop | 10.8 V | Never enforced |
| Critical shutdown | < 10.5 V | Never enforced |

On a solar/lead-acid system, deep discharge below 10.5V causes permanent capacity loss. There is currently no protection of any kind.

### Required fixes

1. Complete the **ADS1115 + isolated current sensor** migration already specified in the reference architecture (this supersedes both INA226 and the PS817C divider)
2. Restore the battery-critical run-block in `controlTick()` **before** any unattended deployment
3. Until then, treat the system as having no power protection and monitor battery voltage manually

---

<a name="h-4"></a>
## H-4 — ESP2 reported `off`; scheduled pump exercise may be a no-op

### Evidence

`ESP2:off` appears in **2,567 of 2,568** health checks across all 9 days. Exactly one `ESP2:on` (2026-07-31).

Yet a pump-exercise routine fires every 48 hours at ~19:17 and logs apparently-successful actuation:

```
2026-07-29 19:17:46,ESP1,STATE,IDLE_STATE->ACTIVE_STATE
2026-07-29 19:17:46,ESP1,CMD,ESP2|EXERCISE,TRANSFER
2026-07-29 19:17:46,ESP1,ACT,EXERCISE|START|TRANSFER
2026-07-29 19:17:51,ESP1,ACT,EXERCISE|STOP|TRANSFER
... (BOOSTER, MIXER follow identically)
```

This pattern repeats identically on 07-29, 07-31, 08-02, 08-04.

### Partial explanation — `ESP2:off` is expected by design

`esp2Available` is set true in exactly one place — `ESP1/src/main.cpp:1422` (on `READY`) — and latched false on power-down (line 1668) and silence (line 2604). ESP2 is intentionally powered off during idle per §18.8, confirmed at line 2460:

```cpp
// NOTE: ESP2 is OFF during idle (sec.18.8) -- do NOT gate on esp2Available here; a due
// column powers ESP2 up on demand below and the work order is sent once it reports READY.
```

So `ESP2:off` during idle is **correct behaviour**, not a fault.

### The unresolved concern

Dispatch is gated on both flags — `ESP1/src/main.cpp:1702`:
```cpp
if (esp2Available && esp2Powered) dispatchPendingExercise();   // already up
```

However, the exercise log entries contain **no corresponding ESP2 `ACK` or `DONE` responses**. Compare with the 2026-07-24 irrigation attempt, which *did* log `ESP2,RESP,ACK,SEQ_IRRIGATION_B`. Either:
- the warm-up path is silently failing and the `ACT` lines are logged optimistically before confirmation, or
- the deployed firmware logs differently than `7adb9b3`

**If the former, your pump anti-seize maintenance has not actually run since deployment** — a real risk for pumps sitting idle in a humid environment for weeks.

### Verification steps

1. **Physically observe the pumps at the next 19:17 exercise window** (next occurrence: check 48h cadence from 2026-08-04) — do they audibly run?
2. Confirm whether `ACT|START` is logged before or after ESP2 acknowledgement in the deployed source
3. If logged optimistically, change to log on `ACK` received, so the log reflects reality

---

# 🟡 MEDIUM

<a name="m-1"></a>
## M-1 — NPK-B ~45% read failure with no retry logic

### Evidence

New per-sensor telemetry (added in the unpushed firmware) makes this explicit:

| Date | `NPK_B\|FAIL` | `NPK_B\|OK` | `NPK_A\|FAIL` |
|---|---|---|---|
| 2026-07-28 | 359 | 359 | 1 |
| 2026-07-29 | 156 | 155 | 0 |
| 2026-07-30 | 134 | 134 | 0 |
| 2026-07-31 | 123 | 124 | 0 |
| 2026-08-01 | 100 | 99 | 0 |
| 2026-08-02 | 92 | 93 | 0 |
| 2026-08-03 | 96 | 95 | 0 |
| 2026-08-04 | 107 | 107 | 0 |
| 2026-08-05 | 81 | 82 | 0 |

NPK-A logged **one** failure in nine days. NPK-B toggles constantly.

### Code finding — single-shot read, no retry

`Nano/Nano.ino::readNpkColumn()` performs exactly one Modbus transaction with five independent failure exits and **no retry**:

```cpp
if (idx < expected)                             return false;  // timeout
if (resp[0] != addr || resp[1] != NPK_FUNCTION) return false;  // wrong responder
if (resp[2] != 2 * NPK_REG_COUNT)               return false;  // bad byte count
if (CRC mismatch)                               return false;  // corrupted
```

With `NPK_RESPONSE_TIMEOUT_MS = 1000` (line 157). Both columns share one RS485 bus (`SoftwareSerial npkSerial(D9, D10)`, line 167) and are polled back-to-back in `sendNpk()` (lines 428-443).

### Assessment

**The asymmetry is hardware, not code.** Same bus, same function, same timeout — Column A succeeds ~100%, Column B fails ~45%. Adding retries would *mask* the symptom without fixing the cause.

Note also `Nano/Nano.ino:152` — the slave addresses are still unverified:
```cpp
const uint8_t NPK_ADDR[NUM_COLUMNS] = { 0x01, 0x02, 0x03 };  // per-column slave addr [CONFIRM]
```

### Required fixes

1. **Physical:** inspect Column B RS485 wiring, connector, and bus termination (see C-3 — likely the same cable run)
2. **Firmware (secondary):** add 2–3 bounded retries so transient bus glitches don't produce dead reads, **but log retry-exhaustion as a distinct event** so the true hardware failure rate remains visible in thesis data. Do not let retries hide the fault.
3. Confirm the `0x02` slave address is correct for the Column B sensor

---

<a name="m-2"></a>
## M-2 — Serial frame corruption ~10% and slowly rising

### Evidence

| Date | Total lines | GARBAGE lines | % of volume |
|---|---|---|---|
| 2026-07-28 | 17,711 | 1,167 | 6.6% |
| 2026-07-29 | 12,341 | 1,099 | 8.9% |
| 2026-07-30 | 12,378 | 1,117 | 9.0% |
| 2026-07-31 | 11,731 | 1,178 | 10.0% |
| 2026-08-01 | 10,985 | 1,050 | 9.6% |
| 2026-08-02 | 10,959 | 1,088 | 9.9% |
| 2026-08-03 | 11,003 | 1,047 | 9.5% |
| 2026-08-04 | 10,414 | 1,090 | 10.5% |
| 2026-08-05 | 9,961 | 1,053 | 10.6% |

Absolute count is stable (~1,050–1,180/day); the rising percentage reflects reduced total logging, not worsening corruption.

Representative corruption signatures:
```
RAW=<START>,STATUS<START>,ENV,29.7,77.9,<END>     (concatenated frames)
RAW=<T>,LIGHT,4152,<END>                          (truncated start delimiter)
RAW=<START>,SOIL,A,3                              (truncated mid-packet)
```

Both truncation and concatenation are present — consistent with bit-level corruption on the UART line rather than a framing-logic bug.

### Assessment

Consistent with the established EMI diagnosis: the 500W inverter's ~50 kHz switching noise coupling capacitively across the Hi-Link converter's parasitic interwinding capacitance. The correct fix remains the **10A DC EMI filter (LC + common-mode choke)** on the input side, not a firmware change.

**Cross-impact:** this ~10% frame loss directly aggravates H-1 — dropped heartbeat frames are a likely trigger for the zero-margin `NANO_SILENCE` timeout.

---

<a name="m-3"></a>
## M-3 — Fertigation silently downgrades to irrigation-only on NPK fault

> ✅ **DONE 2026-08-07.** `decideFertigate()` now emits an explicit event on the NPK-invalid fallback: `ESP1,CTRL,COL_x|FERT_DOWNGRADE|reason=NPK_INVALID` (in addition to the existing `NPK_FAULT`). The thesis dataset can now distinguish a fertigated run from a watered-only one. Mirrored to `Demo/ESP1-Force`; both build clean. The analysis below is the original finding.

`ESP1/src/main.cpp:2493-2500`:

```cpp
bool decideFertigate(int c) {
  if (col[c].mode == MODE_IRRIGATION_ONLY) return false;
  if (!sensor.npkValid[c]) {
    raiseFault('M', "NPK_FAULT", c == 0 ? "COL_A" : c == 1 ? "COL_B" : "COL_C");
    return false;
  }
  ...
}
```

With NPK-B failing ~45% of reads (M-1), roughly **half of all Column B fertigation decisions would silently return `false`** and proceed as irrigation-only. A `NPK_FAULT` is raised, but the *downgrade itself* is not logged as a distinct event.

**Thesis impact:** once dosing begins, this will produce a dataset where some runs were fertigated and some were not, with no explicit marker distinguishing them. Given the project's stated priority on honest measurement over convenience, this needs an explicit log line.

**Required fix:** log the downgrade explicitly, e.g.
```
ESP1,CTRL,COL_B|FERT_DOWNGRADE|reason=NPK_INVALID
```

---

<a name="m-4"></a>
## M-4 — ThingSpeak Column B data 33–67% incomplete

`Columns.csv` fields 6–8 (Column B NPK) are blank on a large fraction of rows:

| Date | Populated / total | Blank rate |
|---|---|---|
| 2026-07-28 | 1,054 / 1,387 | 24% |
| 2026-07-30 | 491 / 1,415 | 65% |
| 2026-08-02 | 470 / 1,393 | 66% |
| 2026-08-04 | 455 / 1,380 | 67% |
| 2026-08-06 | 740 / 1,362 | 46% |

This mirrors the NPK-B hardware failure rate — a **downstream symptom of M-1**, not a separate upload bug. The ThingSpeak upload path itself is healthy (see below).

**Thesis impact:** the cloud dataset for Column B is unusable for continuous time-series analysis until M-1 is resolved.

---

<a name="l-1"></a>
# 🟢 LOW

## L-1 — Provisional constants still unmeasured

Constants still carrying provisional markers in `7adb9b3`:

| Constant | File:line | Value | Marker |
|---|---|---|---|
| `calSoilAir` | ESP1:128 | `{800,800,800}` | `[MEASURE]` → ✅ measured `{656,707,800}` (`383a735`) |
| `calSoilWater` | ESP1:129 | `{300,300,300}` | `[MEASURE]` → ✅ measured `{524,391,300}` (`383a735`) |
| `soilStartPct` | ESP1:100 | `35` | `[TBD]` |
| `soilStopPct` | ESP1:101 | `45` | `[TBD]` |
| `NPK_ADDR` | Nano:152 | `{0x01,0x02,0x03}` | `[CONFIRM]` |
| `NPK_BAUD` | Nano:153 | `4800` | `[CONFIRM]` |
| `NPK_FUNCTION` | Nano:154 | `0x03` | `[CONFIRM]` |
| `NPK_REG_START` | Nano:155 | `0x0000` | `[CONFIRM]` |
| `NPK_REG_COUNT` | Nano:156 | `7` | `[CONFIRM]` |
| `COLUMN_ENABLED` | ESP2:141 | `{true,true,false}` | `[CONFIRM]` |

The soil constants are directly implicated in C-1 and are the highest priority to resolve.

---

# ✅ Confirmed Healthy

| Subsystem | Status | Evidence |
|---|---|---|
| **DHT22 (replaced 2026-07-23)** | ✅ Fix holding | Zero `0.0\|0.0` stuck readings across all 9 days (vs. 223 on 07-23 pre-swap). Only 14–35 transient `-1` per day out of 4,000–7,300 reads. |
| **BH1750** | ✅ Stable | No recurrence of the July 10–11 `Device is not configured!` storm. |
| **RTC** | ✅ Stable | `RTC:ok` on all 2,571 health lines. Timestamps monotonic, zero backward jumps. |
| **SD logging** | ✅ Stable | `SD:ok` on all health lines. Clean midnight rollovers every day, no file corruption at boundaries. |
| **WiFi** | ✅ Good | `WIFI:up` on all days except 12 brief dropouts on 2026-08-05. |
| **ThingSpeak upload** | ✅ Reliable | Continuous through 2026-08-06 23:30. Only 20–41 gaps >10 min across 27 days, nearly all pre-2026-07-15. |
| **Telemetry design** | ✅ Major improvement | The `CTRL` / `HEALTH` / `SENSOR` events added ~2026-07-28 are why this diagnostic could reach code-level root causes. Continue this pattern. |
| **Safety architecture** | ✅ Behaved correctly | The 07-24 `FLOW_FAIL` → `EMERGENCY_STOP` → hold-for-`RESUME` sequence worked exactly as designed under a real fault. |

---

# Recommended Fix Order

Ordered by dependency and diagnostic payoff, not severity alone.

### Phase 0 — Unblock everything else
| # | Action | Type | Ref | Status |
|---|---|---|---|---|
| 0.1 | **Push current firmware to the repo** | Process | — | ✅ Done 2026-08-07 (`383a735`) |

### Phase 1 — Restore diagnostic visibility (cheap, high payoff)
| # | Action | Type | Ref | Status |
|---|---|---|---|---|
| 1.1 | Log **raw soil ADC** alongside mapped percentage | Firmware | C-1 | ✅ Already in firmware (`X1=/X2=`) |
| 1.2 | Give Nano silence its own wider timeout (`NANO_SILENCE_TIMEOUT_MS = 300000`) | Firmware | H-1 | ✅ Done 2026-08-07 |
| 1.3 | Log fertigation downgrade as an explicit event | Firmware | M-3 | ✅ Done 2026-08-07 |

### Phase 2 — Diagnose and fix the soil gate
| # | Action | Type | Ref |
|---|---|---|---|
| 2.1 | Bench-test probes A1 and A2 individually via `CAL_START,SOIL_A1`/`A2` | Hardware | C-1, C-2 |
| 2.2 | Measure and set real `calSoilAir` / `calSoilWater` per column | Calibration | C-1, L-1 |
| 2.3 | Change SOIL packet to per-probe format (Nano + ESP1 parser) | Firmware | C-2 |
| 2.4 | Add stuck-sensor watchdog + probe-divergence fault | Firmware | C-1, C-2 |
| 2.5 | Verify `CAL,SOIL_A2` raw value actually emits | Firmware | C-2 |

### Phase 3 — Column B hardware
| # | Action | Type | Ref |
|---|---|---|---|
| 3.1 | Physically inspect Column B: flow sensor, TRANSFER pump, cable run, relay | Hardware | C-3 |
| 3.2 | Inspect Column B RS485 wiring / connector / termination | Hardware | M-1 |
| 3.3 | Add bounded NPK retries with separate retry-exhaustion logging | Firmware | M-1 |

### Phase 4 — Infrastructure and safety
| # | Action | Type | Ref |
|---|---|---|---|
| 4.1 | Install 10A DC EMI filter (LC + common-mode choke) | Hardware | M-2 |
| 4.2 | Re-measure GSM success rate after H-1 fix; then debug SIM800L | Hardware | H-2 |
| 4.3 | Complete ADS1115 + isolated current sensor migration | Hardware | H-3 |
| 4.4 | Restore battery-critical run-block before unattended deployment | Firmware | H-3 |
| 4.5 | Verify pump exercise actually actuates; fix optimistic `ACT` logging | Both | H-4 |

### Phase 5 — First real fertigation run
| # | Action | Type | Ref |
|---|---|---|---|
| 5.1 | Confirm ~8× stock dilution commissioning step | Calibration | C-4 |
| 5.2 | Validate 12 mL dosing floor against real crop doses | Calibration | C-4 |
| 5.3 | Execute and log first `EVAL` → `DOSE` cycle | Operations | C-4 |

---

# Appendix A — Files Reviewed

## SD-card daily logs

| File | Lines | Coverage |
|---|---|---|
| `20260728.CSV` | 17,711 | 00:00:00 → 23:59:40 |
| `20260729.CSV` | 12,341 | 00:00:00 → 23:59:59 |
| `20260730.CSV` | 12,378 | 00:00:00 → 23:59:40 |
| `20260731.CSV` | 11,731 | 00:00:00 → 23:59:55 |
| `20260801.CSV` | 10,985 | 00:00:00 → 23:59:58 |
| `20260802.CSV` | 10,959 | 00:00:00 → 23:59:19 |
| `20260803.CSV` | 11,003 | 00:00:00 → 23:59:56 |
| `20260804.CSV` | 10,414 | 00:00:00 → 23:59:49 |
| `20260805.CSV` | 9,961 | 00:00:00 → 23:59:57 |

## ThingSpeak channel exports

| File | Rows | Coverage |
|---|---|---|
| `System.csv` | 20,331 | 2026-07-10 10:10 → 2026-08-06 19:17 |
| `Columns.csv` | 34,458 | 2026-07-10 09:55 → 2026-08-06 23:29 |
| `Chem.csv` | 34,608 | 2026-07-10 09:59 → 2026-08-06 23:30 |

Note: `System.csv` stops updating ~4 hours before the other two channels on 2026-08-06 — worth confirming whether the System channel upload path stalled or the export was taken at a different time.

---

# Appendix B — Code Reference Index

All line numbers refer to commit `7adb9b3` (2026-07-10).

| Finding | File | Lines | Symbol |
|---|---|---|---|
| C-1 | `ESP1/src/main.cpp` | 128–129 | `calSoilAir`, `calSoilWater` |
| C-1 | `ESP1/src/main.cpp` | 146–151 | `soilPct()` |
| C-1 | `ESP1/src/main.cpp` | 1329 | soil conversion, raw discarded |
| C-1 | `ESP1/src/main.cpp` | 2472–2473 | `controlTick()` soil gate |
| C-1 | `ESP1/src/main.cpp` | 100–101 | `soilStartPct`, `soilStopPct` |
| C-2 | `Nano/Nano.ino` | 90–95 | `SOIL_PINS[][2]` |
| C-2 | `Nano/Nano.ino` | 401–418 | `sendSoil()` |
| C-2 | `Nano/Nano.ino` | 482–491 | calibration per-channel read |
| C-2 | `ESP1/src/main.cpp` | 1318–1335 | SOIL packet parser |
| C-3 | `ESP2/src/main.cpp` | 146 | `FLOW_TIMEOUT_MS` |
| C-3 | `ESP2/src/main.cpp` | 790 | no-flow detection |
| C-3 | `ESP2/src/main.cpp` | 866, 953, 970 | `holdFault("FLOW_FAIL", ...)` |
| H-1 | `ESP1/src/main.cpp` | 176 | `HEARTBEAT_TIMEOUT_MS` |
| H-1 | `ESP1/src/main.cpp` | 2585–2590 | Nano silence detection |
| H-1 | `Nano/Nano.ino` | 108–110 | `HB_INTERVAL_*_MS` |
| H-3 | `ESP1/src/main.cpp` | 2457 | battery run-block removed |
| H-4 | `ESP1/src/main.cpp` | 1422, 1655, 1668 | `esp2Available` transitions |
| H-4 | `ESP1/src/main.cpp` | 1702 | `dispatchPendingExercise()` gate |
| H-4 | `ESP1/src/main.cpp` | 2460 | idle-power design note |
| M-1 | `Nano/Nano.ino` | 152–160 | NPK config block |
| M-1 | `Nano/Nano.ino` | 428–443 | `sendNpk()` |
| M-1 | `Nano/Nano.ino` | `readNpkColumn()` | single-shot Modbus read |
| M-3 | `ESP1/src/main.cpp` | 2493–2500 | `decideFertigate()` |

---

# Appendix C — Notes for Claude Code

When acting on this document:

- **Verify against current source first.** This analysis is based on commit `7adb9b3` (2026-07-10). The deployed firmware contains `HEALTH`, `CTRL`, and per-sensor `SENSOR` telemetry that does not exist in that commit. Line numbers and some logic may have shifted. Re-locate symbols by name, not line number.
- **Do not add NPK retries that mask the failure rate** (M-1). This project prioritises honest measurement for thesis data. Retry-exhaustion must be logged as a distinct event so the true hardware fault rate stays visible.
- **Do not "fix" the `FLOW_FAIL` logic** (C-3). It is behaving correctly and reporting a genuine hardware condition. Changing timeouts or bypassing the check would hide a real fault.
- **`HEARTBEAT_TIMEOUT_MS` is shared** between Nano silence (line 2585), ESP2 silence (line 2593), and freshness checks (line 2380). Changing the constant directly affects all three — prefer introducing a separate Nano-specific timeout, or verify all three paths.
- **Soil packet format change (C-2) is a coordinated two-file change.** `Nano/Nano.ino::sendSoil()` emits, `ESP1/src/main.cpp:1318` parses. Both must change together or the packet will be rejected as garbage.
- **Preserve the disabled-column convention.** Column C logs the literal token `DISABLED`, never `0` or `-1`. This is working correctly and must survive any packet format change.
- **Update `CLAUDE.md`** with any architectural decisions arising from these fixes, per the project's locked-decision workflow.
