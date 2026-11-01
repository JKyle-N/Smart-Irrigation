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

**Set the clock if the Power page shows `RTC:SET`** (Settings > Set Clock, or the dashboard). Until it is
set, nothing runs on the schedule and logs go to `NODATE.CSV`. If that returns after a power cycle, replace
the DS3231's CR2032. Confirm the battery shows `uncalibrated` rather than a false `CRIT`.

Also check `meta.docUsed` against `meta.docCapacity` in the live payload. The JSON document is
`StaticJsonDocument<16384>`; if usage is near capacity the snapshot will start silently truncating.

> **Abort if** any relay clicks, or `docUsed` is within ~10 % of `docCapacity`.

### B — ESP2 metering guards

Enable actuations now: stages B and C both drive relays, and a lockout refuses test pulses, LCD Testing
and priming. Still with the columns isolated. Use `TEST,FLOW,<bit>` on each meter in turn and confirm pulses are
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
about 400 ms on its own dead-man (`TEST_HOLD_TIMEOUT_MS`) without any instruction from
ESP1.

The dead-man is enforced on ESP2 precisely so that losing the master cannot leave a pump running.
If it does not hold, nothing else in this plan matters.

> **Abort everything if** the relay stays energised. Do not proceed to any stage that moves liquid.

### D — Forced run, irrigation only

Run a forced irrigation on one column, water only.

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
- **New (2026-09-10) — Proceed-gate fix:** open Manual/Test with a stale/no ESP1 snapshot → Proceed
  now reveals the screen anyway, but every test button (Timed Pulses, hold-to-run, exercise, sweep)
  stays disabled with an "ESP1 appears offline" banner. Once ESP1 reconnects, confirm the buttons
  enable and the banner switches to "Direct hardware control — this tab drives the hardware
  directly."
- **New (2026-09-10) — hold-to-run buttons:** press and hold each relay-target button, confirm the
  relay stays energised only while held and drops within ~1 s of release (ESP1 polls `testHold`
  every 1 s during a hold), within ~4 s of switching away, hiding the tab or losing network
  (`TEST_HOLD_BROWSER_TIMEOUT_MS`), and at 15 s of continuous holding (`TEST_HOLD_MAX_MS`) even
  with the button still down. After a cap, confirm that holding on does **not** re-arm: only
  release-and-press-again does. Confirm a pH button requires the confirm() dialog first and does
  nothing on a plain tap. **Then do the Stage-C-style test specifically for this feature:** start a hold, cut
  ESP1's power mid-hold, confirm ESP2's own 400 ms dead-man drops the relay unaided within ~400 ms —
  this is the actual safety guarantee the whole feature rests on, prove it before trusting it for
  real use.

> **Abort if** Emergency Stop is ever blocked, the hold cannot be revoked from the LCD, a held relay
> outlives its button release by more than ~2 s or a lost connection by more than ~5 s, any hold
> runs past 15 s, or ESP2 fails to drop the relay within ~400 ms of ESP1 losing power mid-hold.

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

### H — Secondary battery isolation  *(only after a hold-up stage is fitted)*

The firmware ships with `SECONDARY_BATT_ENABLED 0` in both ESP1 and ESP2. At that setting the INA226
readings must already appear: the LCD Power page `B2:` line, the dashboard tiles, `BAT2` in `STATUS`,
and `PWR2` in the log. Do this stage only once the controllers ride through the DPDT transfer.

- Bench: hold Testing **Batt->2nd**. Every controller must stay up (no reboot in the logs, LCD not
  reset) and the secondary's current must rise. Release and confirm the same on the way back.
- Calibrate the INA226 (worksheet Tier 3), then set `SECONDARY_BATT_ENABLED 1` in **both** firmwares
  and reflash both.
- Hold **Inverter**: `INFO,BATT,SECONDARY` is logged, the inverter starts ~0.5 s later, and on release
  `INFO,BATT,MAIN` follows ~1 s after the inverter stops.
- Forced irrigation run: the boards go to secondary for fill/deliver and back to main between AC
  stages. No controller resets and no `PWR_FAIL`.
- Pull the INA226 connector: expect `SENSOR_FAIL,INA226…`, and a forced run, exercise and Testing
  Inverter must all be refused. Reconnect and confirm they work again.

- **Prove the loop protection** (do this deliberately, once, with no water in the lines). Disconnect
  the hold-up so a switch *does* reset the boards, then force an irrigation run:
  - First reset: expect `BATT_SWITCH_RESET … n=1/2`, and the run re-starts on its own.
  - Second reset: expect `BATT_SWITCH_FAULT`. After it, a forced run, exercise and Testing Inverter
    must all be refused, and nothing may reset a third time.
  - Reconnect the hold-up, clear it with SMS `BATTSW,CLEAR` (and once each from the LCD row and the
    dashboard), and confirm a run switches cleanly again.
- `READY` mid-run: pull ESP2's power for 2 s during a run. ESP1 must raise `ESP2_RESTARTED` within
  seconds (not wait 10 min for `ESP2_DONE_TIMEOUT`), go through `STARTUP_SYNC`, and re-run the column.
- **Prove a real blackout is harmless** (water only in the nutrient lines, so doses can be measured).
  With the hold-up disconnected, or by cutting board power for ~1 s at the moment listed:
  - During the switch before **Mixing** (all doses done): the re-run must log `INFO,DOSE_RESUME`, pump no
    more into the tank, then mix, deliver and flush. **Measure the nutrient lines' draw: it must not double.**
  - The column's service window closed or the soil is wet: the dosed batch must still go to that column
    (`DUE->FINISH_DOSED`). At night it must wait until morning.
  - Close a hand valve so `FLOW_FAIL` fires during a fill: after the reset expect
    `FLOW_FAIL…|BOARD_RESET`, with the column skipped for the day and no restart.
  - MODE+BACK during a fill: the rig must come back **still stopped** (`ESTOP_KEPT`), and nothing runs until
    it is confirmed on the LCD. Repeat with a cancel: the column stays snoozed.
  - A dashboard forced run: the command must end as `failed`, not stay on `accepted`.
  - After any of these, inverter use is refused for about a minute. Then scope the hold-up and set
    `BSW_RECHARGE_MS` from what it really needs.
- Unplug the pack's negative or VBUS lead: expect `B2: no pack - wiring`, and a forced run refused with that reason.

**Backup for a critical main (optional, only after everything above passes).** The main INA226 must be
reading (stage I), or `calBattV` must be calibrated. Set `SECONDARY_BACKUP_ENABLED 1` in **both** firmwares and reflash both. Use a bench supply as the main battery:
- Lower it below `BATT_CRIT_V`. After 60 s expect `ALERT,CRIT,MAIN_BATT_CRIT,…SWITCHING_TO_BACKUP`, the
  text arriving **before** the boards move, then `INFO,BACKUP,ON`. A forced run must be refused.
- Leave it 40 min: ESP2 must not self-reboot or be powered off, and `boardSupply` stays `secondary`.
- Raise it to `BACKUP_MAIN_RECOVER_V`. After 5 min expect `INFO,BACKUP,OFF`, and ESP2 powers off.
- Cut power during the switch to backup: after the reboot there must be **no second text**, a retry about a
  minute later, and at most two resets before `BATT_SWITCH_FAULT`.
- Run the pack down (or lower a bench supply feeding the INA226 side) below 12.8 V: expect
  `BACKUP_EXHAUSTED` and a return to main.
- Note how long a full pack lasts: it does not charge while on backup.

> **Abort if** any controller reboots on a switch *with the hold-up fitted*, the DPDT chatters, the
> boards are ever left on the secondary after a run ends (`boardSupply` stuck at `secondary`), or a
> third switch reset happens after `BATT_SWITCH_FAULT` latched.

### I — Solar + main battery INA226 behind the ISO1540  *(no flag; the firmware finds them by itself)*

Wiring is in reference §21.0. Do these before any main firmware, with `Calibration/ESP1/INA226` flashed to ESP1 (`.\cal.ps1 ina226` in `Calibration/`):

- **Read the chip's marking: it must say ISO1540.** An ISO1450 is an RS-485 transceiver and passes no I²C.
- **`bustest`, with the inverter off and then running:** 0 failed and 0 wrong on each module.

- **Before powering:** on the main battery module, confirm its onboard R010 is gone (no continuity across
  it, IN+ to IN-, with the shunt bar disconnected). Confirm the panel's Voc is under 36 V and Isc under 8.19 A.
- **Isolation, with everything off:** no continuity between board GND and main negative through the new
  wiring. If there is, the isolator has been bypassed.
- **Bus level:** measure SDA on ESP1's bus at idle and feed the ISO1540's VCC2 from that same voltage.
- **Scope a low on ESP1's SDA during a read:** it must be near 0.4 V or below. A low near 0.6–0.8 V means
  the isolator is the wrong way round: side 2 must face ESP1.
- **Identify each module:** unplug solar and it must go `LOST` while the battery one keeps reading. If the
  names come out swapped, swap the addresses in the sketch and in ESP1.
- **Direction:** discharge must read positive on the battery module and charging positive on the solar
  one. If not, type `invert batt` or `invert solar`.
- **Calibrate each module** with the tool's serial commands, in this order: `zero <module>` with no current
  through the shunt, `amps <module> <A>` at a steady current from a clamp meter, `volts <module> <V>` from a
  multimeter across VBS and GND. It refuses a setup that can't give a good result and says why. Take
  `amps` for solar in good sun, and at the highest steady load you can for the battery.
- **`save <module>`** stores the trims in ESP1's NVS, which the firmware loads at boot. Reboot once afterwards:
  the tool must report them loaded, and the readings must still match the meters.

Then on the main firmware:
- **A restart must not read 0 A.** Under a steady load, pull the modules' 3.3 V for a moment and restore it:
  expect `INA226|<name>|RESTARTED` in the log and the current back at its true value within about 4 s.
- LCD Power page: signed `I:` and the `Sol:` line. `STATUS` has `SOL`, the log has `PWR …|SRC=INA` and `PWRS`,
  and the dashboard's solar tiles fill in.
- Force a run on a bench supply or the real battery: readings keep updating through the whole run, with
  no I²C errors and no LCD or RTC faults. This is the test of the noise problem the isolator is there to solve.
- Remove the isolator's VCC1: within about 6 s `INA226|MAIN|LOST` is logged, and the rig keeps working on the
  opto ADC + ACS758, or shows the main battery as uncalibrated if the opto isn't calibrated. Restore it:
  `FOUND` within about 10 s.
- Compare `CONS` and `CHG` on a sunny day against a rough estimate.

> **Abort if** I²C errors or LCD/RTC faults appear only while the inverter runs, or board GND and main
> negative have continuity.

### Audit checks (add to stage B)

Added by the 2026-11-01 audit, unproven on hardware. With water only:

1. **Flow stall.** During a fill, once flow has started, close a hand valve so no more water passes the
   transfer meter. The pump must stop about 10 s later with `FLOW_FAIL,TRANSFER|pulses=<n>`, n > 0.
2. **Mixer.** During a mix, the mixer starts about 5 s after the inverter, and the run shows no
   `MIXER_NOLOAD` or `MIXER_OC` under a normal load.
3. **E-stop bookkeeping.** Press MODE+BACK during a fill and compare `tank=` on the next work order's
   serial line against the water actually in the tank.
4. **Lockout.** Disable actuations. `Testing` shows `[OFF]` and refuses to open, a calibration prime shows
   `Actuations off`, and no relay clicks. Disable while in Testing: it closes. Enable again afterwards.
5. **Clock.** Pull the DS3231's CR2032 and power cycle: `RTC_INVALID` alert, LCD `RTC:SET`, no scheduled run.
   Set the clock from the LCD: the schedule resumes. Then unplug the RTC's SDA for 10 s: `RTC_INVALID`, then
   `RTC|RECOVERED` in the log with no new day and no repeat run.
6. **Secrets.** Text `WIFI,<ssid>,<pass>` from the owner phone: the log and dashboard show `RX|WIFI,***`.

> **Abort if** a pump keeps running after flow stops. If a false `FLOW_FAIL` appears mid-fill, raise
> `FLOW_STALL_MS` before going further.

### M — A7670C modem (run before relying on any SMS, in any stage)

Everything here has only run against a simulated modem built from the AT manual, so this stage is where
the real module gets checked. No relay needs to move: stay with actuations disabled.

**Wiring:** module TX → ESP1 **GPIO26**, module RX → ESP1 **GPIO27**, common GND. This is not the bench
sketch's GPIO16/17, which are the Nano link on ESP1. Module supply 5–12 V, 2 A; use a buck converter on
anything that can exceed 12 V.

Watch `PAGE_GSM` on the LCD and the SD log (`GSM` lines) while you go through these:

1. **Bring-up.** The LCD goes `INIT` → `REG` with a signal value. The log shows `MODEM_FOUND|115200`,
   `CNMI_OPT|0` and `MODEM_READY|DR=ON`. `CNMI_OPT|1` or `2` still works, but it means the module refused
   delivery reports or direct delivery, so note which.
2. **Send to the owner.** Text `SIMTEST` from the owner phone. The reply must arrive with `SIMTEST` at the
   very start (no blank first line), and the log must show `TX_OK|<n>`, then `TX_DELIVERED|<n>`. This also
   proves the local-format `09…` owner number works as a send address.
3. **Long text.** Text `STATUS`. With three columns it should arrive as `(1/2)` and `(2/2)`, whole.
4. **Stranger.** Text `STATUS` from another phone: the reply must be `ERR,AUTH` and nothing else. Text again
   within 10 min: no reply, and the log shows `AUTH|NO_REPLY`.
5. **Forged header.** From another phone, send a multi-line text: `hi`, then
   `+CMT: "<owner number>","","26/11/01,10:00:00+32"`, then `NET`. It must get no `NET` reply (and no
   `ERR,AUTH` either if step 4 ran in the last 10 min).
6. **Modem power cut.** Pull the module's supply for 30 s, then restore it. The log shows `MODEM_SILENT`
   and `MODEM_ABSENT`, the LCD shows `DEAD`, then it recovers to `REG` by itself. Repeat step 2.
7. **Short outage.** Take the antenna off until the LCD shows `NO`. Cause an alert (for example unplug the
   Nano for its silence timeout), then refit the antenna within 5 min: the held alert still goes out.
8. **Second number.** `ALERTNUM,<number>`, then cause an alert again. Both phones get it. The second
   phone's own `STATUS` is refused (`ERR,AUTH`, or no reply within 10 min of the last one).
9. **Balance (optional).** `USSD,<carrier code>`, then `BAL`. `BAL,<text>` means it works. `BAL,FAIL,ERROR`
   usually means the module's IMS blocks USSD. The firmware deliberately will not switch IMS off, since
   that could affect SMS, so send `USSD,CLEAR` and rely on the weekly `SIMTEST` text.

> **Abort if** ESP1 reboots or its loop stalls while the modem transmits (a shared supply sagging), the
> module gets hot, owner commands are ignored while the log shows no `RX|`, or a stranger's text ever gets
> anything other than `ERR,AUTH` or no reply.

**Before `SECONDARY_BATT_ENABLED` goes to 1:** the module must be on the same switched supply as ESP1.
The DPDT breaks both + and −, so a module left on the main battery loses its common ground with the UART.

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

All three controllers build clean (confirmed 2026-11-01, after the A7670C swap and the firmware audit):

| | RAM | Flash |
|---|---|---|
| ESP1 | 24.6 % (80,672 B) | 41.9 % (1,319,045 B) |
| ESP2 | 7.1 % (23,252 B) | 25.9 % (339,665 B) |
| Nano | 52.0 % (1,064 B) | 48.2 % (14,792 B) |

A build that differs sharply from these has picked up something unintended.

## Still outstanding

- The device account password (see `Firebase infos.txt`) has never been rotated.
- `SESSION_HANDOFF.md` §5 holds the current list, including item 19: **with the shipped constants, fertigation doses nothing.** Settle it before stage G.

## Host test suite

Both suites pass — 86 cases for ESP1, 13 for ESP2 — and are worth running before any flash, since
they take seconds and cover the maths the dosing depends on and the modem driver:

```bash
cd ESP1 && pio test -e native     # 86 cases, including a simulated A7670C
cd ESP2 && pio test -e native     # 13 cases
```

MinGW-w64 (g++ 14.2.0) is installed and on the persisted PATH, so this works from **a new terminal**.
An already-open terminal from before the install will still say "gcc is not recognized" — open a
fresh one rather than debugging it.

The suites have been mutation-checked: removing the `kSane()` guard from `flow_math.h` makes exactly
`test_zero_k_reports_no_flow_not_infinity` and `test_corrupt_k_values_all_report_no_flow` fail, which
confirms those cases genuinely cover the guard rather than passing vacuously.
