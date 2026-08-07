# Claude Code Handoff — CSV Telemetry Logging Changes

**Scope:** logging/telemetry only. Do not change control logic, thresholds, timeouts, or actuator behaviour in this task.
**Derived from:** `Diagnostic Finding 0728 to 0805.md` (log review of 2026-07-28 → 2026-08-05)
**Target:** ESP32 #1 (`ESP1/src/main.cpp`), Arduino Nano (`Nano/Nano.ino`)

---

## Context You Need Before Starting

A nine-day log review found that **five subsystems capture diagnostic data in RAM and discard it before it reaches the SD card**. Every one of the project's currently-unresolved faults is un-diagnosable from logs for this reason. This task closes those gaps.

**Critical:** the repository is behind the deployed firmware. The logs contain `ESP1,HEALTH`, `ESP1,CTRL`, and `ESP1,SENSOR` events that do not exist in the committed source. **Before making any change, locate the current implementations by symbol name, not by the line numbers cited below** — line numbers refer to commit `7adb9b3` (2026-07-10) and will have shifted.

If you find the deployed source differs materially from what is described here, stop and report the difference rather than guessing.

---

## Design Rules (apply to every change)

1. **Log raw values alongside derived values, never instead of them.** The root failure pattern in this codebase is converting a raw reading to a percentage/boolean and discarding the raw input. Every fix below follows the same principle.
2. **Preserve existing event formats.** Do not rename or restructure existing `SENSOR`, `FAULT`, `ACT`, `CMD`, `STATE`, `PWR`, `CTRL`, or `HEALTH` events. Downstream analysis scripts and ~30 days of historical logs depend on current field ordering.
3. **Preserve the disabled-column convention.** Column C logs the literal token `DISABLED` — never `0`, never `-1`. This is correct and must survive all changes.
4. **Preserve the invalid-reading convention.** A failed sensor read logs `-1`, distinct from a valid zero.
5. **No blocking calls.** The Nano runs a watchdog (`WDTO_4S`) and both ESP32s run non-blocking state machines. Do not introduce `delay()` outside `setup()`. The single existing exception (the ~5 ms RS485 DE settle in `readNpkColumn()`) must remain the only one.
6. **Do not mask faults.** This is a thesis system where honest measurement outranks clean-looking data. Where a change could hide a real hardware failure rate, log the masking event explicitly. This is called out per-item below.
7. **Watch log volume.** Current volume is ~10,000–17,000 lines/day. Items 1 and 2 add fields to existing lines rather than new lines, which is intentional. Do not add new high-frequency periodic events.

---

## Item 1 — Log raw soil ADC (HIGHEST PRIORITY)

### Problem
`ESP1` receives a raw soil ADC value from the Nano, converts it to a percentage via `soilPct()`, stores only the percentage, and discards the raw input:

```cpp
// ESP1/src/main.cpp ~line 1329, in the SOIL packet handler
tmp[c] = soilPct(c, v);      // 'v' (raw ADC) is never stored or logged
```

`soilPct()` clamps to 0–100:
```cpp
static int soilPct(int col, int raw) {
  long p = map(raw, calSoilAir[col], calSoilWater[col], 0, 100);
  if (p < 0) p = 0; if (p > 100) p = 100;
  return (int)p;
}
```

With unmeasured placeholder endpoints (`calSoilAir = {800,800,800}`, `calSoilWater = {300,300,300}`, both still tagged `[MEASURE]`), any raw ≤ 300 clamps to exactly 100 and any raw ≥ 800 clamps to exactly 0. Column A has read exactly `100` on all 2,553 samples over nine days — indistinguishable from genuine saturation, a shorted probe, or wrong calibration endpoints.

### Required change
Log the raw ADC alongside the mapped percentage in the `SENSOR,SOIL` event.

**Current format:**
```
2026-08-05 08:00:00,NANO,SENSOR,SOIL|A=100|B=86|C=DISABLED
```

**Required format:**
```
2026-08-05 08:00:00,NANO,SENSOR,SOIL|A=100:287|B=86:412|C=DISABLED
```

Format is `<pct>:<raw>`. Disabled columns remain the bare token `DISABLED` with no raw suffix. An invalid reading logs `-1:-1`.

If a colon separator conflicts with existing parsing, use `A=100/287` instead — but pick one and apply it consistently. State which you chose in your summary.

### Also required
Add the raw value to the `HEALTH` line's soil fields using the same separator, so the 5-minute health snapshot carries raw values too.

### Acceptance
- A log line shows both percentage and raw for every enabled column
- Column C still logs exactly `DISABLED`
- Failed reads log `-1:-1`
- Historical parsers that split on `|` and `=` still find the percentage in the expected position (the percentage must come first)

---

## Item 2 — Log per-probe soil values

### Problem
`Nano/Nano.ino::sendSoil()` averages two physical probes per column before transmission:

```cpp
// ~line 415
int raw = (analogRead(SOIL_PINS[c][0]) + analogRead(SOIL_PINS[c][1])) / 2;
pktAddIntOrInvalid(raw, true);
```

Pin map (`~line 91`):
```cpp
const uint8_t SOIL_PINS[NUM_COLUMNS][2] = {
  { A0, A1 },   // Column A
  { A2, A3 },   // Column B
  { A6, A7 },   // Column C
};
```

A single failed probe is invisible. If A0 reads 600 (plausible) and A1 is shorted at 100, the average of 350 maps to ~90% and silently blocks irrigation forever, with no fault and no diagnostic trace. Per-probe access exists **only** in the calibration path (`~line 489`).

### Required change — this is a coordinated two-file change

**Nano side** (`Nano/Nano.ino::sendSoil()`): emit both channel values per column instead of the average. The wire packet becomes:
```
<START>,SOIL,A,<raw1>,<raw2>,B,<raw1>,<raw2>,<END>
```
Keep the existing tag-based, variable-length structure. Disabled columns remain omitted entirely (an all-disabled state must still skip the packet rather than emit an empty one — preserve that existing guard).

**ESP1 side** (SOIL packet handler, `~line 1318`): parse two values per column tag. Compute the column average from them for `sensor.soil[c]` so **control logic behaviour is unchanged**, and log both probes.

**Required log format:**
```
2026-08-05 08:00:00,NANO,SENSOR,SOIL|A1=287|A2=294|A=100|B1=410|B2=414|B=86|C=DISABLED
```
Per-probe raws first, then the column percentage. This supersedes the `<pct>:<raw>` format from Item 1 for the `SENSOR,SOIL` line — implement Item 1 first (it is a one-line change and immediately useful), then supersede it here.

### Critical constraint
`sensor.soil[c]` must continue to hold the **averaged percentage** exactly as today. Control logic in `controlTick()` must see identical values to before this change. This is a logging change, not a behaviour change.

### Backward compatibility
The Nano and ESP1 must be flashed together. A Nano emitting the new packet to old ESP1 firmware will have every SOIL packet rejected as garbage. Note this prominently in your summary so the operator flashes both.

### Acceptance
- Both probes visible per enabled column
- `sensor.soil[c]` numerically identical to pre-change behaviour for the same inputs
- Column C omitted from the wire packet, logged as `DISABLED`
- Packet still parses if only one column is enabled

---

## Item 3 — Emit `SOIL_A2` raw calibration value

### Problem
Across the full log history, `CAL,SOIL_A1,<value>` lines appear (e.g. `CAL,SOIL_A1,399.0`) but **`CAL,SOIL_A2` never emits a value**, despite `CAL_START,SOIL_A2` commands being issued and acknowledged. The channel-2 calibration stream appears broken.

### Required change
Investigate the calibration stream path — `Nano/Nano.ino` `~line 482-491` (the `SOIL_` calId handler) and the `calStreamTick()` function. The parsing is:

```cpp
if (!strncmp(calId, "SOIL_", 5)) {
  if (calId[6] == '\0') {                    // "SOIL_A" -> column average
    raw = (float)((analogRead(SOIL_PINS[col][0]) + analogRead(SOIL_PINS[col][1])) / 2);
  }
  int ch = calId[6] - '1';                   // "SOIL_A1"/"A2" -> channel 0/1
  raw = (float)analogRead(SOIL_PINS[col][ch]);
}
```

Verify: the column index derivation from `calId[5]`, the channel index from `calId[6]`, bounds checking on `ch`, and whether the `SOIL_A` (average) branch correctly falls through or should `return`/`else`. Note the average branch computes `raw` and then the channel branch **overwrites it unconditionally** — for `"SOIL_A"`, `calId[6]` is `'\0'` so `ch = -49`, producing an out-of-bounds array read. This looks like the bug.

Fix so that all three calIds work: `SOIL_A` (average), `SOIL_A1` (channel 0), `SOIL_A2` (channel 1) — and likewise for columns B and C.

### Acceptance
- `CAL_START,SOIL_A1` streams channel 0 raw ADC
- `CAL_START,SOIL_A2` streams channel 1 raw ADC
- `CAL_START,SOIL_A` streams the average
- No out-of-bounds array access for any valid calId

---

## Item 4 — Log GSM signal quality and registration state

### Problem
`ESP1` polls `AT+CSQ`, `AT+CREG?`, and `AT+CPIN?` every 15 seconds (`gsmHealthTick()`, `~line 1754`) and parses the replies into RAM (`gsmFeedInbound()`, `~line 1730`):

```cpp
} else if (line.startsWith("+CSQ:")) {
  lastRssi = line.substring(5).toInt();
} else if (line.indexOf("+CREG:") >= 0) {
  int stat = ...;
  netRegistered = (stat == 1 || stat == 5);
} else if (line.indexOf("+CPIN:") >= 0) {
  simReady = (line.indexOf("READY") >= 0);
```

`lastRssi`, `netRegistered`, and `simReady` are shown on the LCD but **never written to CSV**. The system polls signal strength 5,760 times a day and discards every answer. This makes it impossible to determine from logs whether GSM failures are signal-related.

WiFi already does this correctly (`ESP1,NET,WIFI|UP|rssi=-82`) — mirror that pattern.

### Required change
Add a periodic GSM network event. Suggested interval: every 5 minutes, or on change of `netRegistered`/`simReady`, whichever is less frequent. **Do not log every 15-second poll** — that would add 5,760 lines/day.

```
2026-08-05 08:00:00,ESP1,NET,GSM|rssi=14|creg=1|cpin=READY
```

Where `rssi` is the raw `AT+CSQ` value 0–31 (99 = unknown, log as `99`), `creg` is the raw `+CREG` stat integer (not the derived boolean — log the raw value so `stat=2` "searching" and `stat=3` "denied" are distinguishable), and `cpin` is `READY` / `NOTREADY`.

Also add the RSSI to the existing `HEALTH` line's GSM field so the 5-minute snapshot carries it:
```
GSM:no(rssi=6)
```
Keep the existing `GSM:yes`/`GSM:no` token intact for backward compatibility; append the parenthetical.

### Acceptance
- RSSI, registration stat, and SIM state appear in CSV at a bounded rate
- Raw `+CREG` stat integer preserved, not just the boolean
- Existing `HEALTH` line field ordering unchanged; only the GSM token is extended

---

## Item 5 — Log actual SMS send result (HIGH VALUE)

### Problem
This is the most misleading log in the system. `TX|ALERT` is written the instant the message body is handed to the modem, **before the network responds**:

```cpp
// ESP1/src/main.cpp ~line 2015, case GTX_PROMPT_WAIT
if (c == '>') {
  simSerial.print(gtxMsg);
  simSerial.write(26);                      // CTRL+Z -> send
  logEvent("GSM", "GSM", "TX|" + gtxMsg);   // logged HERE — before any network reply
  gtxMs = millis();
  gtx = GTX_BODY_SETTLE;
  return;
}
```

Then:
```cpp
case GTX_BODY_SETTLE:
  while (simSerial.available()) gsmFeedInbound((char)simSerial.read());
  if (millis() - gtxMs >= GSM_BODY_SETTLE_MS) gtx = GTX_IDLE;
```

The firmware **never parses `+CMGS:` (accepted) or `+CMS ERROR:` (rejected)** — neither string appears anywhere in the codebase. A message rejected for no credit, no registration, or invalid recipient logs identically to a delivered one. Over nine days, 729 messages logged as sent with zero confirmation that any reached the network.

### Required change
In `gsmFeedInbound()`, parse two additional responses:
- `+CMGS:` → log `GSM,GSM,TX_OK|<msgref>`
- `+CMS ERROR:` → log `GSM,GSM,TX_ERR|<code>`
- Bare `ERROR` received while `gtx == GTX_BODY_SETTLE` → log `GSM,GSM,TX_ERR|GENERIC`

Extend `GSM_BODY_SETTLE_MS` handling so the state machine waits for one of these responses rather than blindly timing out — but keep a hard timeout so a silent modem cannot stall the queue. If the timeout expires with no response, log `GSM,GSM,TX_UNKNOWN`.

**Do not remove or rename the existing `TX|<payload>` line** — it is the only record of message content, and 30 days of history depend on it. Add the result as a separate subsequent event.

### Why this matters
The operator has not received any SMS and suspects the SIM may have no load. With current logging, that hypothesis cannot be confirmed or refuted from logs. After this change it is a one-line answer.

### Acceptance
- Every `TX|<payload>` is followed by exactly one of `TX_OK`, `TX_ERR`, or `TX_UNKNOWN`
- `+CMS ERROR` numeric code preserved verbatim (it identifies no-credit vs. no-network vs. bad-recipient)
- Message queue cannot stall on a silent modem

---

## Item 6 — Log fertigation downgrade

### Problem
`decideFertigate()` (`~line 2493`) silently returns `false` when NPK data is invalid:

```cpp
bool decideFertigate(int c) {
  if (col[c].mode == MODE_IRRIGATION_ONLY) return false;
  if (!sensor.npkValid[c]) {
    raiseFault('M', "NPK_FAULT", c == 0 ? "COL_A" : ...);
    return false;                    // silently downgrades to irrigation-only
  }
```

NPK-B currently fails ~45% of reads. Once dosing begins, roughly half of Column B's fertigation decisions will downgrade to irrigation-only. A `NPK_FAULT` is raised, but **the downgrade itself is not logged as a distinct event**, so the resulting thesis dataset would contain a mix of fertigated and non-fertigated runs with no explicit marker distinguishing them.

### Required change
Log the downgrade explicitly at the point of decision:
```
2026-08-05 08:00:00,ESP1,CTRL,COL_B|FERT_DOWNGRADE|reason=NPK_INVALID
```

Also log the affirmative decision so both branches are traceable:
```
2026-08-05 08:00:00,ESP1,CTRL,COL_B|FERT_DECIDE|fert=1|gapN=42.0|gapP=8.0|gapK=15.0
```

Keep the existing `raiseFault('M', "NPK_FAULT", ...)` call unchanged.

### Acceptance
- Every fertigation decision produces exactly one `FERT_DECIDE` or `FERT_DOWNGRADE` line
- Existing `NPK_FAULT` behaviour unchanged

---

## Item 7 — Log NPK read failures distinctly (DO NOT MASK)

### Problem
`Nano/Nano.ino::readNpkColumn()` is a single-shot Modbus read with five failure exits and no retry. Column B fails ~45% of reads; Column A ~0%.

### Required change — logging only for now
**Do not add retries in this task.** A retry would mask a real hardware fault rate that is currently the primary evidence localising a Column B wiring problem.

Instead, make the failure reason visible. `readNpkColumn()` currently returns a bare `bool`. Change it to return a reason code (or set an out-parameter) distinguishing:
- `TIMEOUT` — `idx < expected`
- `BADADDR` — `resp[0] != addr || resp[1] != NPK_FUNCTION`
- `BADLEN` — `resp[2] != 2 * NPK_REG_COUNT`
- `BADCRC` — CRC mismatch

Log it on the ESP1 side, extending the existing per-sensor event:
```
2026-08-05 08:00:00,ESP1,SENSOR,NPK_B|FAIL|reason=TIMEOUT
```
Keep `NPK_B|OK` unchanged.

This distinguishes "device not responding at all" (timeout → dead device or broken wiring) from "responding but corrupted" (CRC → noise/EMI on the bus). That distinction determines whether the fix is a cable or an EMI filter.

### If you later add retries (separate task, not now)
Log retry-exhaustion as its own event so the underlying hardware failure rate stays visible in thesis data. Never let a retry silently convert a hardware fault into an apparent success.

### Acceptance
- Failure reason present on every `NPK_x|FAIL` line
- No retry behaviour added
- Read timing unchanged (`NPK_RESPONSE_TIMEOUT_MS` untouched)

---

## Item 8 — Log actuator commands on acknowledgement, not on dispatch

### Problem
The scheduled pump-exercise routine logs apparent success with no corresponding ESP2 acknowledgement anywhere in the log:

```
2026-07-29 19:17:46,ESP1,CMD,ESP2|EXERCISE,TRANSFER
2026-07-29 19:17:46,ESP1,ACT,EXERCISE|START|TRANSFER
2026-07-29 19:17:51,ESP1,ACT,EXERCISE|STOP|TRANSFER
```

Compare the 2026-07-24 irrigation attempt, which **did** log `ESP2,RESP,ACK,SEQ_IRRIGATION_B`. Meanwhile `ESP2:off` appears in 2,567 of 2,568 health checks. It is currently impossible to tell from logs whether the pumps physically ran — which matters, because if they did not, anti-seize maintenance has not occurred since deployment.

### Required change
Determine whether `ACT` lines are logged optimistically at dispatch or on confirmation. If optimistic:

- Keep the `CMD` line at dispatch time (it correctly records intent)
- Move the `ACT|START` log to fire on receipt of the ESP2 `ACK`
- If no ACK arrives within the existing timeout, log `ESP1,ACT,EXERCISE|NOACK|TRANSFER`

Do not change the warm-up or dispatch logic itself — only when the log line is written.

### Acceptance
- `ACT|START` appears only when ESP2 confirmed
- A missing ACK produces a visible `NOACK` line rather than silence
- Dispatch/warm-up timing unchanged

---

## Explicitly Out of Scope

Do **not** change these in this task, even though they appear in the diagnostic report:

- `HEARTBEAT_TIMEOUT_MS` (false `NANO_SILENCE` faults) — behaviour change, separate task
- `FLOW_TIMEOUT_MS` or any flow-fail logic — currently correct, reporting a real hardware fault
- `calSoilAir` / `calSoilWater` values — must be measured on hardware, not guessed in code
- `soilStartPct` / `soilStopPct` thresholds
- NPK retry logic (see Item 7)
- Battery safety run-block restoration
- GSM health-poll serialisation / SMS retry logic
- Any control-flow change in `controlTick()`

---

## Deliverables

1. Implement items **1, 3, 4, 5, 6, 7** first — these are additive and independently flashable.
2. Implement items **2 and 8** second — item 2 requires coordinated Nano + ESP1 flashing; item 8 requires verifying deployed behaviour first.
3. Update `CLAUDE.md` with any format decisions that become locked architecture (especially the SOIL packet format change in item 2).
4. Update the packet-format documentation comment block at the top of `Nano/Nano.ino` (currently documents `<START>,SOIL,<COL>,<val>,...,<END>`).
5. In your summary, state explicitly:
   - Which separator you chose for item 1
   - Whether the deployed source differed from the committed source, and how
   - Estimated change in daily log line volume
   - Which items require simultaneous Nano + ESP1 reflash

## Verification Before You Finish

- Both firmwares compile under PlatformIO
- Nano flash and SRAM usage checked — the Nano is an ATmega328P with 2 KB SRAM and the packet buffer is fixed-size; verify the wider SOIL packet still fits
- No `delay()` introduced outside `setup()`
- No new blocking loops in any `*Tick()` function
- Existing log consumers (ThingSpeak field mapping, any parsing scripts) reviewed against format changes
