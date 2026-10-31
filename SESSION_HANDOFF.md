# Session Handoff — start here

Read this first in a new Claude Code session, **after** `CLAUDE.md` (which is auto-loaded and holds
the locked architecture — do not repeat or re-derive it here).

Last updated: **2026-11-01** — SIM800L replaced by the A7670C (`ESP1/include/gsm_modem.h`); see §4 and
`CLAUDE.md` "LOCKED: GSM behavior". Commissioning stage M covers it.
Previous: 2026-10-30, toolchain pinned after ESP1 stopped linking on the newest platform; LCD
Thresholds/Column Mode truncation fixed; new open decisions logged in §5 (items 15–18).

---

## 1. The single most important fact

**Nothing in the backlog has been flashed to hardware.** The rig is running firmware from around the
`pre-firebase-baseline` tag (`bc571a8`, 2026-07-10). Everything since — the Firebase dashboard, fault
recovery, NPK blending, the Manual/Test tab, the manual-mode hold, and all of Sugian's 2026-09 fixes
and features merged on 2026-10-26 — exists only in the repository.

Every audit run against that backlog has found real defects in the newest code. Treat unflashed work
as unproven no matter how carefully it was reviewed, and do not describe any of it to the user as
"working" — it compiles, the host tests pass, and it is reasoned; that is all.

`Commissioning_Flash_Plan.md` stages the testing (A–G, each with an abort condition). Follow it
rather than inventing an order. Stage C (ESP2's own 400 ms dead-man dropping a relay when ESP1 loses
power mid-hold) and Stage F (Manual/Test, including the web hold-to-run cap) must not be skipped.

---

## 2. State of the cloud side (shared Firebase project `smart-irrigation-web2`)

- **RTDB rules are LIVE** and identical to `Website/firebase-rules.json` (Sugian deployed them
  2026-09-10/11). They include `irrigation/manual`, `irrigation/testHold`, `presence`, `activity`,
  `users`, and the command-type whitelist with privileged-tier gating.
- **Operators are hardcoded UIDs** in the rules and in `delete-user-api/api/delete-user.js`:
  `espoperator32@gmail.com` and Sugian's own account. The user's account is **not** an operator; it
  needs approval by one of them, or a rules edit + redeploy to add it. The user chose to adopt the
  website as-is (2026-10-25).
- **The live website** is served from Sugian's repo `Jmhatake123/Website`
  (https://jmhatake123.github.io/Website/). Pushing `Website/` to `JKyle-N/Smart-Irrigation` does
  **not** publish anything.
- **`delete-user-api`** is deployed on Vercel (`DELETE_USER_API_URL` in `firebase-config.js`). Its
  `.env.local` and `.vercel/` are deliberately untracked — never commit them.
- **Before assuming ESP1 is reachable**, check `irrigation/live/meta.updatedAt` freshness first — a
  past session lost time blaming the rules when ESP1 simply was not connected.

---

## 3. Environment facts that will otherwise waste your time

| Thing | Reality |
|---|---|
| **PlatformIO** | Not on PATH. It is at `C:\Users\johnb\.platformio\penv\Scripts\pio.exe` — prepend that dir to `$env:PATH` |
| **Platform versions** | **Pinned** since 2026-10-30: ESP1/ESP2 `espressif32@6.9.0` (Arduino core 2.0.17), Nano `atmelavr@5.3.0`. Unpinned, PlatformIO pulled espressif32 8.0.0 / core 3.3.12 and ESP1 failed to link (EspSoftwareSerial: `dangerous relocation: l32r`). Do not unpin |
| **Git** | Not on this machine's PATH by default; it is at `E:\Program Files\Git\cmd\git.exe` |
| **Host compiler** | MinGW-w64 g++ 14.2.0 was installed on the *old* machine (`C:\` paths). On the current machine (project on `E:\`, 2026-10-30) **no gcc is installed**, so `pio test -e native` fails with "gcc is not recognized" — install MinGW (see the `platformio.ini` comment) to run the host tests |
| **`platformio.ini` (ESP1, ESP2)** | Both have the **skip-worktree** bit set. Git silently ignores your edits to them. Check with `git ls-files -v -- '*platformio.ini'` (an `S` means skip-worktree) |
| **Why that bit exists** | To keep the local `upload_port = COM5` from overwriting the committed `COM8`. Preserve both: clear the bit, commit with `COM8`, restore `COM5` locally, re-set the bit |
| **`Firebase infos.txt`** | Gitignored. Holds the RTDB URL, device UID, and the device password. Never commit or transmit it |
| **`Logs/`** | Gitignored |
| **Shell** | PowerShell is primary; a Bash tool exists too. They take different syntax — do not mix them |
| **Sugian's copies** | Sugian works on another machine (`C:\Users\johnm\...`) with only `Website/` under git. Their docs' paths and "no compiler" notes describe that machine, not this one |

## Build and test commands

```bash
# firmware (a bare `pio run` builds only the default env)
cd ESP1 && pio run
cd ESP2 && pio run
cd Nano && pio run

# host unit tests -- ~10 seconds each, no hardware needed
cd ESP1 && pio test -e native     # 80 cases (33 pure maths + 47 simulated A7670C)
cd ESP2 && pio test -e native     #  7 cases

# website
node --check Website/script.js
node --check delete-user-api/api/delete-user.js
node -e "JSON.parse(require('fs').readFileSync('Website/firebase-rules.json','utf8'))"
firebase deploy --only database --dry-run   # validates rules WITHOUT deploying (run from Website/)
```

Known-good build sizes after the A7670C swap (both `SECONDARY_BATT_ENABLED 0`) — a sharp deviation
means something unintended was picked up. ESP1 before the swap: 77,400 B RAM, 1,305,857 B flash. Host
tests: ESP1 80 cases, ESP2 7.

| | RAM | Flash |
|---|---|---|
| ESP1 | 24.6 % (80,600 B) | 41.8 % (1,313,949 B) |
| ESP2 | 7.1 % (23,236 B) | 25.9 % (339,081 B) |
| Nano | 52.0 % (1,064 B) | 48.2 % (14,792 B) |

---

## 4. What the last sessions added

**2026-11-01 — A7670C replaces the SIM800L on ESP1.** The user confirmed the module works with the bench
sketch, but the integrated firmware is **unflashed**: it is host-tested against a simulated modem only.
- **New module:** `ESP1/include/gsm_modem.h` + `ESP1/test/test_gsm_modem` (47 cases). `main.cpp` keeps
  `handleSms` and every SMS keyword; only the modem layer moved.
- **Add-ons the user chose:** send retries + delivery reports, modem watchdog (soft reset), weekly
  `SIMTEST` + USSD balance, second alert number. **Reserved, not built:** network time (`AT+CTZU`), to
  add if the RTC gives trouble.
- **Security fixes found on the way:** a multi-line SMS could forge a `+CMT:` line with the owner's
  number, and a sender-less SMS passed `senderIsOwner()`. Both are closed and tested.
- **Not checked against the real module:** boot-time messages (the manual does not list them; the driver
  watches `RDY`, `*ATREADY`, `SMS DONE` but also recovers by re-detecting), and `+CDS` / `+CSDH` formats.
- **Rewiring needed:** ESP1 uses GPIO26/27, not the bench sketch's GPIO16/17.

**2026-10-26 (later) — switch-reset protection + review fixes.** See CLAUDE.md "LOCKED: switch-reset
protection".
- **Handshake and journals:** every P4 switch is announced, journaled in both controllers' NVS, and
  confirmed. The handshake is `BATTSW,GO` / `ACK`, and the quiet window holds SD, WiFi and SMS.
- **Latch:** 2 switch-caused resets in 24 h latch `BATT_SWITCH_FAULT`, which blocks AC runs. It
  clears from the LCD, SMS, the website, or automatically after 24 h.
- **Fixed — `READY` during a run** (any ESP2 reboot) now releases the job at once. Before, it ended
  in a 10-min `ESP2_DONE_TIMEOUT` E-stop.
- **Fixed — 2 cases where a reset wasn't counted, or was counted when it shouldn't be:** an
  unjournaled P4 drop on a fault P17 cut, and a false strike after ESP2 `RESET_SELF`.
- **Corrected — CLAUDE.md:** an ESP1 reboot cuts ESP2 (GPIO4 LOW at boot); it does not keep running.
- **Rules not deployed yet:** `Website/firebase-rules.json` now whitelists `CLEAR_BATT_SWITCH`
  (privileged tier). Until it is deployed, the dashboard's Clear button is rejected by the live rules.
  See §5 item 12; the deploy needs the user's approval.

**2026-10-26 — Secondary battery isolation (P4 DPDT) + INA226 monitor.** See `CLAUDE.md` and reference
§19.4.7.1. The user's decisions:
- **Relays:** P4 (the condemned mix valve) drives the DPDT. The mix valve is removed from delivery,
  prime and combos (tank → booster → column valve). P0 (reservoir valve) is a spare, with
  `RES_VALVE_FITTED` (default 0) to restore it.
- **Switching:** every inverter use switches. Timing is 500 ms settle / 1000 ms return, with the P17
  drop deferred until the return on normal ends.
- **Secondary battery:** 4S Li-ion (16.8 V), charged from main while idle. INA226 at 0x40 on ESP1.
- **Block:** a low secondary (< 13.6 V, recover 14.0, critical 12.8) **or a missing INA226** blocks
  AC runs.
- **Readings shown on:** LCD, website and STATUS. Testing row P4 is `Batt->2nd`.
- **Compiled OFF** (`SECONDARY_BATT_ENABLED 0` in ESP1 and ESP2) because the controllers have **no
  hold-up** through the DPDT transfer yet.

**2026-10-26 — Sugian merge.** Sugian's copy forked `main` at `de4b079` (2026-08-19). It was merged
as reference, not source of truth, in reviewable pairs ("port as-is", then "adjust"):

- Nano, ESP2, ESP1 and the website were taken as-is, then adjusted:
  - **ESP2:** PZEM checks on EXERCISE / AC prime / SEQ_MIX wait for `INVERTER_WARMUP_MS`. Without
    this they false-trip on a ramping inverter.
  - **ESP1 hold-to-run** is capped at 15 s, polled at 1 s during a hold, with a 4 s browser timeout
    and a re-arm latch. Sugian's version had a 3 s poll and an 8 s timeout, with no cap.
  - **ESP1 `PHONE_NUMBER`** keeps its compiled default; Sugian had blanked it.
  - **ESP1 battery voltage** publishes for pasted `calBattV` coefficients too, not only for NVS.
- User decisions (2026-10-25): website as-is; hold-to-run with tighter limits; remote SET_CLOCK /
  SET_THRESH / RESTORE_DEFAULTS all allowed; owner number kept in source.
- Not taken: Sugian's blanked phone numbers in `Test Code/` and `Calibration_Worksheet.md`,
  `Firmware_Snapshot_2026-09-11/` (a duplicate), `New folder/`, `Website/.agents`, `.claude`,
  `skills-lock.json`, and every credential file.
- Sugian's dated reports are archived in **`Reports/Sugian_2026-09/`** (`START_HERE_Final_Status_2026-09-11.txt`
  is their summary; `HoldToRun_And_DeepScan_Fixes_Report_2026-09-10.txt` and
  `FINAL_DeepScan_Report_2026-09-11.txt` hold the most detail). They are history, not a to-do list.

**2026-08-19** — RTDB rules version-controlled, crop profiles reach the rig via `SET_COLUMN`, pure
maths in `ESP1/include/pure_math.h` / `ESP2/include/flow_math.h` with host tests,
`Calibration_Worksheet.md`, `Commissioning_Flash_Plan.md`. **2026-10-17** — A7670C SMS test
sketches under `Test Code/`.

---

## 5. What is still outstanding

Roughly in priority order.

1. **`PUMP_FLOWRATE_MLPM` is still a guess** (`ESP1/src/main.cpp`, `50` mL/min). It sets both a
   dose's run time and its timeout ceiling, and unlike the flow K-factors it has **no calibration
   tool** — it needs a jug and a stopwatch. Wrong here means silently wrong doses with no alarm.
2. **61 calibration constants** still carry `[MEASURE]` / `[TBD]` / `[CONFIRM]`. See
   `Calibration_Worksheet.md`, which ranks them by damage and marks which are genuine measurements
   versus agronomic decisions or datasheet lookups. `INVERTER_WARMUP_MS` (now 5 s) belongs on that list.
3. **Values duplicated across firmwares with nothing checking them at runtime** — `WATER_BUDGET_L`
   (ESP1 + ESP2) and `COLUMN_ENABLED` (all three). Drift is silent.
4. **The device password (see `Firebase infos.txt`) has never been rotated.**
5. ~~SMS Summary/Status never implemented~~ — **stale, corrected 2026-10-30:** `STATUS`,
   `SUMMARY[,<day>]`, `FULL SUMMARY[,<day>]` and the 18:00 daily report all exist in
   `ESP1/src/main.cpp` (`handleSms`, `sendDailyReport`). Unflashed, like everything else.
6. Test coverage stops at the pure functions. The FSMs, and anything touching `millis()`, are
   untested — that was a deliberate scope limit, not an oversight.
7. **`mixTankL` can go stale-low if an emergency stop (GPIO4 cut) interrupts `SEQ_FILL`/`SEQ_DOSE`**
   — documented as a known gap in `CLAUDE.md`'s P17/fault-hold section, deliberately left open pending
   a design decision.
8. **Website operators** — decide whether the user's own account should be an operator; that is a
   rules + `delete-user-api` edit and a redeploy, and needs explicit approval.
9. **Nothing has been flashed or hardware-validated** (§1).
10. **Secondary-battery switching needs hardware before it can be enabled.** Without a hold-up after the
    DPDT, every switch resets all three controllers, and the P4/DPDT coils drop the boards straight
    back to main. Fit a hold-up stage that also keeps the coils powered. Then bench-test with the
    Testing `Batt->2nd` row, calibrate the INA226 (worksheet Tier 3), and set
    `SECONDARY_BATT_ENABLED 1` in **both** ESP1 and ESP2.
11. **The dashboard's `mixer` Timed Pulse / hold drives TEST row 8 alone, without the inverter.** The
    mixer is an AC load, so it probably gets no power that way. This was found while mapping inverter
    rows for this change and left unchanged; confirm on the rig.
12. **Deploy the RTDB rules** (`firebase deploy --only database`, after a `--dry-run`) so the
    `CLEAR_BATT_SWITCH` dashboard command is accepted. The LCD and SMS clears work without it.
13. **An ESP1 reboot (manual, remote, or self-reset) during a run or held fault also cuts ESP2**
    (`setup()` drives GPIO4 LOW). The run is lost and the hold is forgotten; the tank volume survives
    in ESP2's NVS. This is documented, not changed. A fix would mean keeping GPIO4 high across a
    software reset, or refusing reboots while busy — a design decision.
14. **A dashboard command in flight when ESP1 itself resets** stays `accepted` forever: ESP1 keeps
    the command id in RAM only. Also documented, not changed.

Found in the 2026-10-30 review — **left open for the user to decide** (not changed):

15. **Nano WDT boot-loop risk (hardware check).** `RESET_REQ` resets the Nano with a 15 ms WDT
    (`softwareReset()`). The old non-Optiboot Nano bootloader (what `board = nanoatmega328` targets)
    does not clear the WDT, so it can reset forever after any WDT reset; `.init3` runs too late to
    help. Check which bootloader the Nano has (or send one `RESET_REQ` on the bench). If old: burn
    Optiboot (`nanoatmega328new`) or change the reset method.
16. **RTDB rules: `testHold` does not require `status === 'approved'`** on its privileged branch
    (`manual` does). The website clears `tempAccessUntil`/`subOperator` on any status change
    (`script.js` ~1017), so this is defense-in-depth. Also, any signed-in account — even pending or
    rejected — may write `want:false` to `manual`/`testHold` and so cancel someone's hold. Fixing
    either is a rules edit + redeploy (needs approval).
17. **Commissioning security:** the SoftAP password and default admin PIN `1234` are in this public
    repo, and the admin page can change the owner number (= full SMS control while the portal is
    open). Change the PIN at commissioning. The Supabase `service_role` key (full admin) lives on
    the device and can be set by plaintext SMS — consider a narrowly-scoped key / storage policy.
18. **Inert Sugian files:** `Website/.github/workflows/static.yml` and `Website/travis.yml` never run
    in this repo (GitHub only reads root `.github/`). Kept so `Website/` stays in step with Sugian's
    repo; delete only if that link no longer matters.

---

## 6. Working conventions that have been learned the hard way

- **The user's standing constraint:** work only in
  `E:\Users\JohnB\OneDrive\Documents\1RESEARCH\CLAUDE\Claude Code` (was `C:\Users\johnb\...` on the
  old machine) and the repo
  `https://github.com/JKyle-N/Smart-Irrigation`. Commit and push authority is standing.
- **Sugian's folders are reference, not source of truth.** Find the fork commit first (diff against
  historical blobs, ignoring CR), then port in "as-is" + "adjust" commit pairs, and ask the user
  before adopting anything that changes the architecture or a locked policy.
- **Plan before non-trivial edits** — `CLAUDE.md` requires it, and the architecture was deliberately
  reasoned. Do not silently make architectural choices.
- **Deploying rules or flashing hardware** needs its own explicit approval every time. Flashing is
  always the user's own action.
- **Firebase RTDB rules have no comment syntax.** A bare non-dot key (e.g. `"_comment"`) parses as an
  invalid child-node definition. `.write` cascades permissively from ancestors; `.validate` never
  cascade-skips, so every touched node's `.validate` must pass on multi-field `.update()` calls.
- **`sed`/`perl` surgery on `main.cpp` and `script.js` has corrupted files before** — prepending a
  replacement to line 1, appending a block to EOF and dropping the last line, and `perl` eating
  `${...}` template literals. Use the Edit tool for in-place edits to those two files; anchor on a
  verified-unique line.
- **After any structural edit, check hunk headers and head/tail against HEAD**, not just that the
  build passes.
- **Cross-core rule (ESP1):** core 0 (`netTask`) may only stage plain scalars or queue entries for
  core 1. It must never call `setState()`, `logEvent()` or `sendEsp2()`, or touch a core-1 String
  without `netMux`. This has been violated, found and fixed several times.
- **Verify before asserting.** Several confident claims in past sessions turned out to be wrong when
  checked against the source. Read the code; do not reason from memory of it. For relay safety,
  reuse the vetted target lists and dead-man paths (`pulseRowForTarget()`, ESP2 `TEST,HOLD`).
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
| `Operator_Manual.md` | How the rig is operated day to day, including the website (§8) |
| `Website/README.md` | RTDB node layout, access tiers and the rules workflow |
| `Calibration/README.md` | Per-sensor bench calibration tools |
| `Settings_Calibration_and_Safe_Edit_Spec.md` | §A — the calibration methods |
| `Nutrient_Dosing_Firmware_Spec.md` | The dosing maths |
| `ESP2_Recovery_Resume_Plan.md` | Fault hold and resume behaviour |
| `Reports/Sugian_2026-09/` | Sugian's audit and fix reports, 2026-09-05 → 09-11 |
