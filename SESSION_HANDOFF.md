# Session Handoff — start here

Read this first in a new Claude Code session, **after** `CLAUDE.md` (which is auto-loaded and holds
the locked architecture — do not repeat or re-derive it here).

Last updated: **2026-08-19**, at `main` = `988ce53`.

---

## 1. The single most important fact

**Nothing in the last ~71 commits has been flashed to hardware.** The rig is running firmware from
around the `pre-firebase-baseline` tag (`bc571a8`, 2026-07-10). Everything since — the Firebase
dashboard, the force-run fix, fault recovery, the NPK blending, the Manual/Test tab, the manual-mode
hold — exists only in the repository.

Every audit run against that backlog has found real defects in the newest code. Treat unflashed work
as unproven no matter how carefully it was reviewed, and do not describe any of it to the user as
"working" — it compiles and it is reasoned, that is all.

`Commissioning_Flash_Plan.md` stages the testing (A–G, each with an abort condition). Follow it
rather than inventing an order.

---

## 2. Blocking action the user must take

**Publish the Firebase RTDB rules.** `Website/firebase-rules.json` → Firebase Console → Realtime
Database → Rules → Publish.

Until this happens, `/irrigation/manual` has no rule, RTDB denies by default where no rule matches,
and **the entire Manual/Test tab is non-functional** — the page's write and ESP1's read are both
rejected, the hold is never granted, and Proceed stays disabled. Stage F of the commissioning plan
cannot be tested.

This is a console change; only the user can do it. Ask whether it has been done before spending time
on anything that depends on the manual hold.

---

## 3. Environment facts that will otherwise waste your time

| Thing | Reality |
|---|---|
| **PlatformIO** | Not on PATH. It is at `C:\Users\johnb\.platformio\penv\Scripts\pio.exe` — prepend that dir to `$env:PATH` |
| **Host compiler** | MinGW-w64 g++ 14.2.0 is installed and on the *persisted* PATH. A shell opened before the install still fails with "gcc is not recognized" — open a new one, do not debug it |
| **`platformio.ini` (ESP1, ESP2)** | Both have the **skip-worktree** bit set. Git silently ignores your edits to them. Check with `git ls-files -v -- '*platformio.ini'` (an `S` means skip-worktree) |
| **Why that bit exists** | To keep the local `upload_port = COM5` from overwriting the committed `COM8`. Preserve both: clear the bit, commit with `COM8`, restore `COM5` locally, re-set the bit |
| **`Firebase infos.txt`** | Gitignored. Holds the RTDB URL, device UID, and the device password `research123`. Never commit or transmit it |
| **`Logs/`** | Gitignored |
| **Shell** | PowerShell is primary; a Bash tool exists too. They take different syntax — do not mix them |

## Build and test commands

```bash
# firmware (a bare `pio run` builds only esp32dev, thanks to default_envs)
cd ESP1 && pio run
cd ESP2 && pio run

# host unit tests -- ~1 second each, no hardware needed
cd ESP1 && pio test -e native     # 24 cases
cd ESP2 && pio test -e native     #  7 cases

# website
node --check Website/script.js
```

Known-good build sizes at `988ce53` — a sharp deviation means something unintended was picked up:

| | RAM | Flash |
|---|---|---|
| ESP1 | 23.6 % (77,176 B) | 40.8 % (1,283,709 B) |
| ESP2 | 7.1 % (23,104 B) | 25.5 % (334,829 B) |

---

## 4. What the last session added

Four workstreams, merged as `3cbfd98` plus `988ce53`:

- **`Website/firebase-rules.json`** — the full ruleset, now version-controlled, including the
  previously missing `/irrigation/manual` rule. ESP1 only *reads* that node (`firebasePollManual()`,
  `ESP1/src/main.cpp:5682`) and answers through `/irrigation/live` as `diagnostics.webManual`
  (`:5475`), so the rule needs no device-write clause; it validates the page's write shape instead.
- **Crop profiles now reach the rig.** "Fill targets from crop profile" resolves `cropDatabase`
  crop+stage to N/P/K/pH, fills the column-settings inputs, and deliberately does *not* send — the
  operator reviews and presses the existing "Send to ESP1", reusing the validated `SET_COLUMN` path.
  No firmware change. `irrigation/config` keeps its existing dashboard-only job.
- **Host-testable seam.** Pure maths moved (not copied) into `ESP1/include/pure_math.h` and
  `ESP2/include/flow_math.h`, with calibration passed as arguments instead of read from globals.
  `main.cpp` keeps thin wrappers, so no call site changed. `imap()` reproduces Arduino `map()`'s
  truncation exactly. 31 Unity cases, all passing, and **mutation-checked** — removing the `kSane()`
  guard fails exactly the two cases written for it.
- **`Calibration_Worksheet.md`** and **`Commissioning_Flash_Plan.md`**.

---

## 5. What is still outstanding

Roughly in priority order.

1. **`PUMP_FLOWRATE_MLPM` is still a guess** (`ESP1/src/main.cpp:348`, `50` mL/min). It sets both a
   dose's run time and its timeout ceiling, and unlike the flow K-factors it has **no calibration
   tool** — it needs a jug and a stopwatch. Wrong here means silently wrong doses with no alarm.
2. **61 calibration constants** still carry `[MEASURE]` / `[TBD]` / `[CONFIRM]`. See
   `Calibration_Worksheet.md`, which ranks them by damage and marks which are genuine measurements
   versus agronomic decisions or datasheet lookups.
3. **Values duplicated across firmwares with nothing checking them at runtime** — `WATER_BUDGET_L`
   (ESP1 + ESP2) and `COLUMN_ENABLED` (all three). Drift is silent.
4. **`research123` has never been rotated.**
5. **SMS Summary/Status** was specified but never implemented.
6. Test coverage stops at the pure functions. The FSMs, and anything touching `millis()`, are
   untested — that was a deliberate scope limit, not an oversight.

---

## 6. Working conventions that have been learned the hard way

- **The user's standing constraint:** work only in
  `C:\Users\johnb\OneDrive\Documents\1RESEARCH\CLAUDE\Claude Code` and the repo
  `https://github.com/JKyle-N/Smart-Irrigation`. Commit and push authority is standing.
- **Plan before non-trivial edits** — `CLAUDE.md` requires it, and the architecture was deliberately
  reasoned. Do not silently make architectural choices.
- **`sed`/`perl` surgery on `main.cpp` and `script.js` has corrupted files twice** — once prepending
  a replacement to line 1, once appending a block to EOF and dropping the last line. `script.js` also
  has `${...}` template literals that `perl` eats. Use the Edit tool for in-place edits to those two
  files; heredocs are fine for whole new files. Always anchor on a verified-unique line.
- **After any structural edit, check hunk headers and head/tail against HEAD**, not just that the
  build passes.
- **Verify before asserting.** Several confident claims in past sessions turned out to be wrong when
  checked against the source — an empty ArduinoJson filter object keeping nothing, a dry-soil gate
  reading the blended value it was meant to bypass, and the assumption that ESP1 wrote the manual
  node when it only reads it. Read the code; do not reason from memory of it.
- Line numbers in older docs (notably `CSV_Logging_Handoff_Prompt.md`) refer to old commits and have
  shifted. Locate code by symbol name.

---

## 7. Reference map

| Document | What it is for |
|---|---|
| `CLAUDE.md` | Locked architecture and conventions. Auto-loaded. Authoritative |
| `Smart_Irrigation_System_Reference_FINAL.md` | Full system specification |
| `Commissioning_Flash_Plan.md` | Stages A–G for putting the backlog on hardware |
| `Calibration_Worksheet.md` | Every untuned constant, ranked by damage |
| `Operator_Manual.md` | How the rig is operated day to day |
| `Website/README.md` | RTDB node layout and the rules workflow |
| `Calibration/README.md` | Per-sensor bench calibration tools |
| `Settings_Calibration_and_Safe_Edit_Spec.md` | §A — the calibration methods |
| `Nutrient_Dosing_Firmware_Spec.md` | The dosing maths |
| `ESP2_Recovery_Resume_Plan.md` | Fault hold and resume behaviour |
