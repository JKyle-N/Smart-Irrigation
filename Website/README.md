# Smart Irrigation — web dashboard

**Live dashboard:** https://jmhatake123.github.io/Website/

Remote monitoring and control for a real hydroponic fertigation rig — built for CIT-U EE Smart
Irrigation / Pinamungahan Automated Farm. Static site (`index.html`, `script.js`, `style.css`) on
GitHub Pages, backed by Firebase Realtime Database and Authentication. ESP1, the rig's master
controller, is the only writer of live telemetry; this page reads it and queues commands that the
firmware validates before acting on — the dashboard has no authority of its own over the hardware.

## About the system

Three controllers, split by role rather than duplicated logic:

- **Arduino Nano** — sensor hub only. Reads soil, NPK, and environment sensors, frames packets,
  sends them on. No decisions, no actuators.
- **ESP32 #1 (master)** — the single source of authority. Scheduling, fault classification, GSM
  alerts, logging, the on-device LCD UI, and everything this dashboard talks to.
- **ESP32 #2 (actuator executor)** — drives the pumps and valves, meters the water and the doses, and
  enforces its own local safety stops (dead-man release, overcurrent, mixing-tank hold) even if ESP1 goes
  silent.

Firmware source: https://github.com/JKyle-N/Smart-Irrigation

**Firmware generation: web v.4 "pure ultrasonic" (2026-11-17).** The reservoir → mix and mix → column
flow meters are gone: the water moved by the transfer and booster pumps is measured by the two tank
ultrasonics (each with its own litres calibration), and, once measured on the LCD (Calibration → Rate
…), by each pump's constant rate × its run time. The nutrient and pH pumps keep their flow meters. What
the page shows differently:

- The reservoir and mixing tank show litres beside the % (`sensors.reservoirLiters` /
  `sensors.mixingLiters`, absent from older firmware), and may read over 100 %.
- No run starts below **18 %** in the reservoir (was 15 %).
- **Reservoir refill** is the Nano's reservoir inflow meter.
- The flow meter check lists six meters (nutrients A–D, pH up and down).
- A timed test of Transfer or a column push answers with the mixing-tank level change (`LEVEL OK +x L` /
  `NO LEVEL CHANGE`).
- Fault explanations for the v.4 texts: `FLOW_FAIL <stage>|NO_LEVEL_CHANGE|STALL|CAP|moved=…`,
  `LEVEL_BLIND`, `SERVO_FAIL`, `ESP2_DEGRADED PUMP_WEAK` and `LEVEL_DISAGREE`. `LEVEL_BLIND` and
  `SERVO_FAIL` are named in the held-fault banner. The pre-v.4 `FLOW_FAIL …|pulses=` text is still
  explained.
- Hardening from the web v.3 log of 17 Nov 2026 (a delivery reported done within a second, nothing reached
  the column): `FLOW_FAIL MAIN|STALL|left=…` (a delivery that stopped with most of its water still in the
  tank is held, not finished), `ESP2_DEGRADED LEVEL_JUMP` (a level "done" faster than the pump can move
  water is ignored) and `ESP2_DEGRADED FLOW_NOISE` (a nutrient meter's noise burst is left out of the dose)
  are explained in words.
- From 17 Nov 2026 (later the same day) a fill is timed by the transfer pump's run time from the inverter switch-on,
  then checked on the mixing tank after about a minute of settling and topped up when more than 3 L short; a delivery
  runs until the mixing tank is under 5 % and 10 s more. `ESP2_DEGRADED FILL_SHORT`, `FILL_NOT_STEADY` and
  `FILL_SETTLE_BLIND` are explained in words, and `PUMP_WEAK` now compares with the run time.

From 18 Nov 2026 each column has an **Operation**: **Automatic** (waters on its watering times, as before) or
**Manual** (readings only: ESP1 starts no scheduled run, and the operator waters or fertigates it with a forced run).
Edit settings has the default plus a **day plan** calendar: click a day to make that date Manual, Automatic, or back
to the default, up to about two months ahead. ESP1 keeps the plan (`run_plan.h`) and follows it by its own clock,
publishing `sensors.zones.<id>.operation`, `operationToday`, `plan` (one character a day: `-` default, `M`, `A`)
and `planFrom` (its date); `SET_COLUMN` takes `{operation, plan, planFrom}`. The plan shows under the column's
readings, and Up next says when a column is Manual. Firmware without it doesn't publish those fields, and the page
then leaves the feature out. The same day, the Manual/Test tab's SD card log panel was merged into Diagnostics → SD
card logs (today's state, idle-only requests, a stalled upload now says it stopped).

Firmware parity (18 Nov 2026, later): Manual/Test can test **Nutrient D** (the owner opened it on 16 Nov; it is still
never dosed by a run). Every one of ESP1's 52 alert codes now shows in plain words (its own SMS headline and advice) in
Recent events, the fault banner and Diagnostics. A column switched off on ESP1 can be switched on from the forced-run
form. With ESP1's firmware of 18 Nov 2026 the page also shows: a **tank check** banner (scheduled runs wait until
someone confirms the mixing tank, with an "I've checked the tank" button → `TANK_CHECK_OK`); ESP1's last start reason
and starts today (`system.bootReason`, `bootsToday`, `uptimeS`); the watering thresholds as ESP1 has them
(`system.soilStartPct`, `fertGap`); the LCD's calibration values, read-only (`diagnostics.calibration`); and why a due
column is passed over (`zones.<id>.restMin`, `skippedToday`). Older firmware just shows "not reported".

The page works with web v.3 firmware too (no litres shown).

## What the dashboard does

- **Simple and Complete modes** (18 Nov 2026) — a switch at the top changes how much the site shows; it
  changes nothing on the rig. Both share one farm-green design. **Simple** (the default) shows the farm at a
  glance in big, plain words: whether it is running, how much water is left, how wet each zone's soil is,
  a drawn picture of the system, what it is doing now, when it waters next, and the watering plan. Its tabs
  are Dashboard, Zone profiles, Controls and Help (plus User Management for operators), with the technical
  parts folded away. **Complete** is the same cards with every reading and control in them, and every tab.
  Both use the same live report and the same safety checks; when ESP1's report is old, both say the state is
  unknown rather than guess (the pump lamps say UNKNOWN, never OFF). The choice, the theme (light by default)
  and whether the plan is shown are kept in this browser only.
- **The owner's second round** (18 Nov 2026):
  - *The picture*: blue water in both tanks whose surface is a small wave simulation (springs between points
    across the surface), stirred by what ESP1 reports (water pouring in or drawn off, the mixer, nutrient
    drops, a level change); still when the report is old, the page is hidden or reduced motion is asked for.
    Over the mixing tank: calcium nitrate (A), ammonium phosphate (B), potassium nitrate (C), pH Up and pH Down.
  - *Times* on the 12-hour clock everywhere (6:00 AM – 8:00 AM); *who is signed in* in the top bar (the name on
    the account's record).
  - *Watering plan* on the dashboard (a month, each column Automatic or Manual each day), hidden or shown with
    the Plan button in Next watering.
  - *Zone cards*: the zone's own name on top, the column under it; the moisture over a bar marking Soil start
    and Too wet; the readings shown unless hidden; this week (Sun–Sat, A or M, today ringed) with today's
    times, progress, feeding and targets; Operation as one word, its meaning in the (i). Edit settings has
    Operation as two buttons and **Soil moisture lines**: the column's own Soil start and Too wet, by slider or
    box, or the System tab's defaults.
  - *System, Watering thresholds*: Too wet % (default 90, a warning only) and the **reservoir pause** % (18 to
    50; never under 18, where the reservoir's sensor stops seeing water).
  - Needs ESP1's firmware of 18 Nov 2026 (b) (`system.firmware` "web v.4 (2026-11-18b)") for the own lines,
    Too wet and the pause; on older firmware those parts say so and stay off.
- **Link lite, no realtime** (18 Nov 2026 g, the owner: "make the website communication back to before, no longer
  high realtime update ... but still it automatically refreshes to request data from the system, and a refresh
  button. To lessen the load of ESP1"). ESP1 (web v.4 of 18 Nov g) reports **once a minute** by itself, as before
  17 Nov; it is *busy* during a run, its warm-up, a pump exercise, an armed forced run, a dosed tank waiting, or a
  watering window that is open and not yet served or opens within 10 minutes, and then answers a refresh at most every
  30 s (10 s otherwise) and holds any SD log upload. Commands and every stop are still read every 3 s. The page asks
  ESP1 for one report (`irrigation/viewer/refreshAt`) when it opens or comes back to the front, by itself when the
  regular report is overdue (at most every 2 minutes), and when **Refresh** is pressed. It no longer touches
  `viewer/at` (the 17 Nov – 18 Nov f "watched" pace), ESP1 no longer uploads old SD logs by itself (only a day asked
  for with "Upload from the rig"), and ESP1's two polls from 17 Nov are slower (refresh every 6 s, log requests every
  30 s). Why: the SD card shows ESP1's reports failing 1–8 % a day from 10 to 17 Nov at −84 to −89 dBm, and 97 % on
  18 Nov with the extra traffic. ESP1 publishes its pace and mode as `meta.refreshMs` / `meta.linkMode` ("idle" /
  "busy"); the page takes every threshold from them (`linkPaceText()`, `deviceDelayMs()`, `deviceIsFresh()`),
  so a pace change in the firmware needs no website change. After failed reports ESP1 opens a fresh connection
  (every 3 failures) and rejoins Wi-Fi (after 10); Diagnostics shows both counts, and how many reports needed their
  text cleaned (noise bytes swapped for "?", which Firebase would otherwise refuse with HTTP 400).
- **Connection line** (18 Nov 2026) — the connection line in the top bar
  (and the Controller chip in Simple mode) is green while ESP1's reports arrive on time, **amber "Delayed"**
  once a report is one interval plus at least 5 s late (`deviceDelayMs()`: over 60 s at the 30 s pace, over 120 s
  at the 60 s pace), and red when this browser is offline or ESP1 has stopped reporting (`deviceIsFresh()`, as
  before). While on screen and offline for 20 s, the page restarts Firebase's reconnect attempt instead of waiting
  out the SDK's growing delay (up to 5 minutes), and again 2 s after the page comes back to the front.
  Diagnostics shows the Wi-Fi signal with what it means for the reports, and, with ESP1's firmware of
  18 Nov 2026 (c), the reports sent and failed, new connections and free memory since ESP1's last start.
- **Rig lock** — one switch, at the top of every tab and on System, that locks this website's controls
  and the buttons on the rig's LCD together. It is ESP1's own Lock Screen flag, so locking or unlocking at
  the rig does the same to the website. Automatic watering carries on; Emergency stop, switching pumps and
  valves off and cancelling a forced run still work. Operators and privileged accounts can lock and unlock.
- **Live Dashboard** — the picture of the system (the same in both modes since 18 Nov 2026) with its tanks, pumps and columns as ESP1 reports them,
  readings grouped by what they're about (water, air and light, main battery, solar and the second
  battery), the current run with its pumps, **Up next** beside it (each column's watering times today and
  tomorrow with dates, a forced run counting down, each pump's next exercise), and the recent commands with
  ESP1's replies, read straight from ESP1's telemetry snapshot.
- **Zone Profiles** — one card per column: its soil readings (compared with a crop profile you pick),
  what ESP1 does with it in plain words (what it does, when it runs, its nutrient targets), what it got
  today, an ON/OFF switch, a website-only name, and **Edit settings** (up to 4 watering times a day), which
  sends ESP1 only what you changed and shows the change first. Below them, **Crop profiles**: the built-in
  crops and your own, each with its growth stages.
- **Controls** — emergency stop, a Pumps and valves ON/OFF switch (ESP1's actuation lockout), a forced
  run ("Water a column now") with its countdown and Cancel, and restarting one of the three boards. The
  stops are only sent while ESP1 is reporting: a stop is never left waiting in the queue to fire later,
  so while ESP1 is silent the page says to use the physical cutoff, MODE + BACK on the unit, or SMS STOP,ALL.
- **Manual/Test** *(privileged accounts)* — direct hardware control: the pump selector switch (which pump
  the Nano's servo is on, with Transfer and Booster buttons that move it while the inverter stays off), a
  timed test, a hold-to-run test, the pump exercise (setting and run-now), and a flow meter check, all
  under ESP1/ESP2's own dead-man timers and safety limits.
- **Diagnostics** — a summary, in words, of anything ESP1 reports as wrong; then ESP1's diagnostic values
  item by item, sensor readings before conversion, an on-demand ESP2 sensor sweep, and recent events.
- **System** *(privileged accounts)* — remote versions of the LCD's Settings menu: the rig lock, clock,
  watering thresholds, AC voltage limits and AC current limits,
  battery-switch fault, restore factory settings.
- **User Management** *(operators only)* — the requests waiting for an answer, then accounts grouped by
  status, waiting-for-approval first; approve, reject, restrict, or block them; give or take back control
  and zone editing; give 1-hour access or make a Sub-operator; see each account's online/offline state and
  recent activity in plain words.
- **Help** — how the rig decides when to water and add nutrients, step-by-step guides for each task,
  and a definition of every word the site uses, searchable. The small "i" icons across the site give
  the short version and link to the matching Help entry.

Access is tiered — pending / approved / restricted / rejected / blocked, plus the operator accounts, two
separate grants an approved account can ask for (control, and zone editing), and an optional temporary or
Sub-operator elevation on top of "approved" — and enforced server-side by `firebase-rules.json`, not just
by what the page shows or hides. Only approved accounts and operators can see readings: pending,
restricted, rejected and blocked accounts see only a page to ask for access, and an account restricted
while its page is open loses them immediately.

## Tech stack

Vanilla HTML/CSS/JS, no framework or build step · Firebase Realtime Database + Authentication ·
GitHub Pages, auto-deployed on push to `main` (see `.github/workflows/static.yml`) · a small Vercel
function (`delete-user-api`) for the one operation Firebase Auth can't do client-side — permanently
deleting a user account.

## Contributors

CIT-U EE Smart Irrigation / Pinamungahan Automated Farm

- Christian Jay A. Tibon
- Mark Bon Q. Compuesto
- Johnbert Kyle T. Nacor
- John Michael N. Sugian
- John Ray C. Lagamayo
- John Christopher S. Cabrillos

---

## Realtime Database layout

| Node | Written by | Purpose |
|---|---|---|
| `irrigation/live` | ESP1 only | Telemetry snapshot, faults, event log, `diagnostics.webManual` |
| `irrigation/commands/$id` | page (queued) → ESP1 (status) | One-shot commands; ESP1 validates every payload. The page may also change a row from `queued` to `withdrawn` (not `EMERGENCY_STOP`, `DISABLE_ACTUATIONS` or `CANCEL_FORCE`), because ESP1 runs a queued row however old it is. While `irrigation/live/diagnostics/system/lcdLocked` is true (the rig lock), a new row must be `EMERGENCY_STOP`, `DISABLE_ACTUATIONS`, `CANCEL_FORCE`, `ACK_FAULT` or `LCD_LOCK`. `LCD_LOCK` needs the privileged tier, and an optional `byName` must be the sender's own name (or `Operator` for an operator account). Since 2026-11-05 a new row needs `controlAccess` (every type but `SET_COLUMN`, the stops included) or `zoneEditAccess` (`SET_COLUMN`), or the privileged tier; withdrawing follows the same split. `SET_COLUMN` may carry `windows` (2026-11-05): every own watering time of the column as `[[start, end], ...]`, 1 to 4 pairs of minutes 0–1439 (the rule checks the shape and range; ESP1 checks the order). The page sends it only to firmware whose snapshot reports each zone's `runs`, and `winStart`/`winEnd` to older firmware. Since 2026-11-06 a new `EMERGENCY_STOP`, `DISABLE_ACTUATIONS` or `CANCEL_FORCE` row is refused when `irrigation/live/meta/updatedAt` is more than 3 minutes old (ESP1 isn't reporting), so a stop never waits in the queue to fire hours later. Since 2026-11-14 `SET_AC_VOLT` (`{ vMin, vMax }`, the AC voltage limits ESP2's PZEM check enforces) needs the privileged tier (operator, approved Sub-operator, or, since 2026-11-16, 1-hour access) and both values as numbers: `vMin` 80–255, `vMax` 85–260, `vMax` at least 5 above `vMin`. Since 2026-11-16 `SET_AC_AMP` (`{ aMin10, aMax10 }`, the AC no-current and overcurrent limits in tenths of an amp) needs the same accounts and both values as numbers: `aMin10` 1–98, `aMax10` 3–100, `aMax10` at least 2 above `aMin10` |
| `irrigation/viewer` | page (approved accounts) → ESP1 | `{at, refreshAt}`, server timestamps (2026-11-17). Until 18 Nov f an open, visible page touched `at` every 15 s; since 18 Nov g the page leaves `at` alone and ESP1 no longer reads it: it uploads `live` once a minute whoever watches, and reports the pace as `meta.refreshMs` / `meta.linkMode` (firmware of 17 Nov to 18 Nov d: every 1–4 s while watched and `meta.live`; 18 Nov e–f: 30 s while watched). The page touches only `refreshAt`: when it opens, when a report is overdue, and on Refresh; ESP1 polls it every 6 s and answers with an upload at once (at most every 10 s, 30 s while busy). The rule takes only those two fields, each within the last minute, and `refreshAt` at most every 2 s. ESP1's device account reads it |
| `irrigation/logRequest` | page (privileged tier) → ESP1 | `{day, at}` (2026-11-17): "Upload from the rig" for one day's SD CSV (`day` yyyymmdd, `at` a server timestamp at most every 5 s). ESP1 polls it every 10 s |
| `logs/index/$day`, `logs/files/$day/$n` | ESP1 only | The daily SD CSV logs (2026-11-17, replacing the expired Supabase upload): the file in ~12 KB text pieces, and `{state: uploading/done/missing/failed, size, chunks, partial, at}`. Read by operators and the privileged tier (the logs hold SMS texts and phone numbers); Diagnostics > SD card logs joins the pieces into a download. ESP1 keeps the last 14 days |
| `irrigation/config/zones` | page only | Each column's website name (`name`, 1 to 60 characters, checked by the rule; the page allows 40), the crop and stage it is growing, and its saved setups (`savedSettings`, whose `windows` hold every watering time as `{from, to}`). Written by accounts with zone editing (or Sub-operator, 1-hour access, operators) since 2026-11-05; every approved account reads it. Dashboard-side notes — the numeric targets reach the rig through `SET_COLUMN`, not from here, and ESP1 keeps its own column names (LCD, SMS) |
| `irrigation/config/crops/$crop` | page only, privileged tier | The crop library's changes (2026-11-05): `{name, variety?, notes?, stages: {$stage: {name, order, n, p, k, ph, ec, moisture, days?, notes?}}, updatedAt, updatedBy}` replaces the built-in crop of the same key or adds a new one; `{removed: true}` hides a built-in until it is restored (deleting the key restores it). Written by operators and the privileged tier (1-hour access, Sub-operator), read by every approved account. The rule checks every value (N/P/K 0–2000 ppm, pH 3–9, EC 0–20 mS/cm, moisture 0–100 %, days 0–365, `order` 0–19; the page keeps a crop to 10 stages) and refuses any other field. ESP1 never reads it |
| `irrigation/manual` | page only | `{seq, want}` — the Manual/Test mutual-exclusion hold. ESP1 reads it and answers in `live` under `diagnostics.webManual`. Acquiring (`want:true`) requires the privileged tier (operator/temp-access/sub-operator, see `users/$uid` below) and an unlocked rig; releasing (`want:false`) is open to anyone signed in |
| `irrigation/testHold` | page only | `{seq, bit, want}` — Manual/Test's press-and-hold buttons. ESP1's `firebasePollTestHold()` reads it and drives ESP2's existing `TEST,HOLD`/`TEST,RELEASE` dead-man; `bit` is restricted (by the rule) to the same 10-target whitelist as `TEST_PULSE`'s `target` field (transfer/colA/colB/colC/mixer/nutA/nutB/nutC/phUp/phDn). Starting a hold (`want:true`) needs the privileged tier on an account that is still approved, and an unlocked rig, the same as `irrigation/manual`; releasing is open to anyone signed in. The page registers an `onDisconnect` release while a button is held, because ESP1 acts on a leftover `want:true` when it boots. ESP1 never answers a hold it won't run, so the page switches off any hold button ESP1 would ignore and says why |
| `presence/$uid` | every signed-in session, its own | Since 2026-11-05 one entry per open page, `sessions/$id = {since, lastActive, visible}`, removed by the server (`onDisconnect`) when the page goes; `lastActive` moves only on real use (a tap, a key, scrolling) while the page is on screen, at most once a minute; `lastActive` and `lastSeen` at the top keep the latest for when no page is open. The old `{online, lastChanged}` is still accepted from a page that hasn't reloaded, and is cleared by the new one. Read-restricted to the operator accounts (plus self) — powers User Management's "Active now / Page open but idle / Offline, last active" |
| `activity/$uid` | every signed-in session, its own | `{what, at}` entries: since 2026-11-05 every change made on the website (sign-in and sign-out, rig commands, column and crop edits, requests, account decisions, Manual/Test), not only commands. Read-restricted to **only** the operator accounts, not even the account's own owner — powers User Management's recent actions |
| `users/$uid` | see `status`/`tempAccessUntil`/`subOperator`/`controlAccess`/`zoneEditAccess` | `tempAccessUntil` (ms timestamp) is a 1-hour, operator-granted Manual/Test+System elevation; `subOperator` (boolean) is the same elevation but persistent until an operator withholds it again. `controlAccess` and `zoneEditAccess` (2026-11-05, booleans): an approved account on its own only looks (Live dashboard, Zone profiles, Help); `controlAccess` opens Controls, Diagnostics, the Emergency stop and every rig command except a column's settings, `zoneEditAccess` opens the column cards and `SET_COLUMN`. All four settable only by the operator accounts, never at sign-up, and cleared by User Management the moment status leaves `'approved'`; the rules also ignore them on an account that isn't approved |
| `accessRequests/$uid/$kind` | the account, its own; operators | 2026-11-05. `{state: 'waiting' \| 'declined', at, decidedAt?}`. `$kind` is `approval` (pending or rejected accounts), `unrestrict` (restricted), `zones` or `control` (approved without that access, not Sub-operators); blocked accounts can't ask. An account can ask once at a time (again after a decline), with the server's time, and take a waiting request back; operators read all of them and answer (accept = the grant or status plus removing the request in one write; decline = `state: 'declined'`) |

## Security rules

`firebase-rules.json` is the authoritative copy of the database rules. Rules are enforced
server-side, so keeping them in the repo costs nothing in security and makes changes reviewable.

**After editing it, publish it**: dry-run with `firebase deploy --only database --dry-run`, then run
`firebase deploy --only database`, or paste the whole file into Firebase Console → Realtime Database →
Rules. The deployed copy is the live source; this file is only version control, and the two drift apart
silently if you change one without the other.

`irrigation/manual` needs its rule present or the Manual/Test tab does not work at all — RTDB denies
by default where no rule matches, so without it both the page's write and ESP1's read are rejected
and the hold is never granted. Proceed itself (2026-09-10 revision) no longer waits for that grant —
it reveals the panel for any privileged-tier account regardless of ESP1's connection state, so the
individual test buttons (gated on a fresh snapshot) are what actually stays unusable, not the screen.

Credentials and the device UID live in `Firebase infos.txt` at the repo root, which is gitignored.
