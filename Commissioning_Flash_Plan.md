# Commissioning & Flash Plan

Eighty-plus commits have accumulated since the firmware currently on the rig was flashed. Every
audit run against that backlog has found real defects in the newest code, so the risk is
concentrated there rather than in the parts that have been running for months.

**A binary cannot be half-flashed.** This plan therefore stages *testing*, not flashing: one build to
the tip, then a fixed order of tests, each with an abort condition. Work down the stages. Stop at the
first surprise and fix it before continuing — the entire point is to avoid discovering four faults
at once and not knowing which caused what.

---

## Before you start

**1. Save what is on the rig now.** Rollback must be one command, not an archaeology exercise.

```bash
# Record the commit the running firmware was built from, if you know it:
git tag flashed-baseline <commit>
# And keep the binaries themselves:
cp ESP1/.pio/build/esp32dev/firmware.bin ~/rig-backup/ESP1-known-good.bin
cp ESP2/.pio/build/esp32dev/firmware.bin ~/rig-backup/ESP2-known-good.bin
```

There is already a `pre-firebase-baseline` tag. If that is what is running, say so in the tag message
now, while you still remember.

**2. Apply the Firebase rules.** `Website/firebase-rules.json` → Firebase Console → Realtime
Database → Rules → Publish. **Stage F cannot pass without this**; `/irrigation/manual` has no rule
today, and RTDB denies by default where no rule matches.

**3. Prime the nutrient lines with water, not stock solution.** Everything before stage G is about
timing and volume, and water measures both just as well. Nothing irreversible goes through the
system until the metering is proven.

**4. Have `Calibration_Worksheet.md` open.** Stages B and G produce numbers that belong in it.

---

## The stages

### A — Flash, actuations disabled

Flash both controllers. Boot with actuations disabled (`DISABLE_ACTUATIONS` from the web, or the LCD
lockout) **before** anything can move.

Check: telemetry reaching the dashboard; LCD paging normally; all five web tabs rendering; sensor
diagnostics on both the LCD and the web showing the same raw values, including the new NPK_A and
NPK_B pages with all seven channels.

Also check `meta.docUsed` against `meta.docCapacity` in the live payload. The JSON document is
`StaticJsonDocument<16384>`; if usage is near capacity the snapshot will start silently truncating.

> **Abort if** any relay clicks, or `docUsed` is within ~10 % of `docCapacity`.

### B — ESP2 metering guards

Still with the columns isolated. Use `TEST,FLOW,<bit>` on each meter in turn and confirm pulses are
counted and reported.

Then deliberately corrupt a K-factor (send a K outside `K_FLOW_MIN..K_FLOW_MAX`) and confirm ESP2
*rejects* it and keeps the previous value, rather than accepting it and completing the next stage
instantly. This is the failure the host test suite covers; this stage confirms the real intake path
enforces it too.

Record the measured K-factors in the worksheet as you go — this stage is also Tier 1 calibration.

> **Abort if** a stage completes with no flow counted, or a corrupt K is accepted.

### C — The power-pull test

**This is the one that must not be skipped.**

Start a `TEST_PULSE` on a relay, then cut ESP1's power mid-pulse. ESP2 must drop the relay within
about 400 ms on its own dead-man (`TEST_HOLD_TIMEOUT_MS`, `ESP2:268`) without any instruction from
ESP1.

The dead-man is enforced on ESP2 precisely so that losing the master cannot leave a pump running.
If it does not hold, nothing else in this plan matters.

> **Abort everything if** the relay stays energised. Do not proceed to any stage that moves liquid.

### D — Re-enable actuations; forced run, irrigation only

Enable actuations. Run a forced irrigation on one column, water only.

Then the specific regression this backlog exists to fix: **start a forced run with ESP2 powered
down.** The order must sit in `WO_PENDING`, ESP2 must be powered up, and the order must dispatch on
`READY` — previously it was written into a powered-down UART and vanished silently.

> **Abort if** the order is lost, or fires twice.

### E — Lockout, fault handling and recovery

Walk every state that can trap an operator:

- Raise a fault; confirm it appears on the web with the recovery choices, and on the LCD.
- Disable actuations, then confirm you can re-enable from **both** the web and the LCD escape hatch.
- Trigger SAFE_MODE and reboot; confirm it persists across the reboot and can still be exited.
- Emergency Stop from the web, then recover.

> **Abort if** any state cannot be exited from both the LCD and the web. A one-way door is a fault
> even when everything else works.

### F — Manual/Test hold  *(needs the Firebase rules from step 2)*

- Open Manual/Test → LCD shows "MANUAL TEST AT WEB" within ~3 s.
- A scheduled window passing during the hold starts no run — check the log for the skip reason.
- Start a run, then try to open Manual/Test → refused with "wait for the run to finish", controls
  stay hidden, LCD banner does not appear.
- **Confirm Controls still opens and Emergency Stop still works during that run.** This is the
  safety point of the whole design: only Manual/Test holds, and the e-stop is never blocked.
- Kill the browser with the hold active → within ~60 s the lease expires, the banner clears, and
  scheduled runs resume.
- With the hold active, press UP+DOWN on the LCD → chooser appears. "View sensors" pages the data
  screens with the hold intact; "Interrupt web" clears it, bounces the page to Dashboard, and stops
  any in-flight pulse.
- With the hold active, confirm LCD Settings → Force Run refuses and names the hold.

> **Abort if** Emergency Stop is ever blocked, or the hold cannot be revoked from the LCD.

### G — NPK blending, then real dosing

First the blending, water only. Test against a genuinely wet column and a genuinely dry one, and
watch that irrigation **start timing has not shifted** from what the rig did before — the blend
changes the moisture figure the schedule keys on, so a shift here is the expected place for a
regression to show.

Confirm the divergence gate excludes the NPK reading when the probe disagrees with the capacitive
pair, and that the dry-soil gate reads the *probe's own* moisture rather than the blended value.

Only then: dilute the stock solutions, record the real concentrations in the worksheet, and dose one
column. **Weigh or measure what actually comes out** and compare with what the firmware reported.

> **Abort if** measured volume is off by more than the worksheet's tolerance — go back to Tier 1
> calibration rather than adjusting anything downstream.

---

## After stage G

Unattended fertigation on all three columns is what stage G *graduates to*, not part of it. Run
attended for at least a full cycle first, with the SD log and the web event log both being checked
afterwards.

## If a stage fails

Fix forward if the cause is obvious. If it is not, bisect by flashing the merge commits in the order
the features landed:

```
a814efd  flow-metering safety guards        (stage B)
8ef00fa  full NPK probe utilisation         (stage G)
bb4aa48  NPK gate audit fixes               (stage G)
17d32c6  Manual/Test tab                    (stage F)
0a5ef5f  switchable pump exercise           (stage A)
dd942d0  web manual-mode hold               (stage F)
6f55405  manual-hold audit fixes            (stage F)
```

Bisecting costs about six flash cycles, which is why it is the fallback rather than the default.

## Known-good reference

The tip of `main` builds clean on both controllers:

| | RAM | Flash |
|---|---|---|
| ESP1 | 23.6 % (77,176 B) | 40.8 % (1,283,709 B) |
| ESP2 | 7.1 % (23,104 B) | 25.5 % (334,829 B) |

A build that differs sharply from these has picked up something unintended.

## Still outstanding

- `research123` (the device account password) has never been rotated.
- The SMS Summary/Status feature was specified but never implemented.

## Host test suite

Both suites pass — 24 cases for ESP1, 7 for ESP2 — and are worth running before any flash, since
they take about a second and cover the maths the dosing depends on:

```bash
cd ESP1 && pio test -e native     # 24 cases
cd ESP2 && pio test -e native     # 7 cases
```

MinGW-w64 (g++ 14.2.0) is installed and on the persisted PATH, so this works from **a new terminal**.
An already-open terminal from before the install will still say "gcc is not recognized" — open a
fresh one rather than debugging it.

The suites have been mutation-checked: removing the `kSane()` guard from `flow_math.h` makes exactly
`test_zero_k_reports_no_flow_not_infinity` and `test_corrupt_k_values_all_report_no_flow` fail, which
confirms those cases genuinely cover the guard rather than passing vacuously.
