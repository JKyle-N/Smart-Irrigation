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
- **ESP32 #2 (actuator executor)** — drives the pumps and valves, counts flow, and enforces its own
  local safety stops (dead-man release, overcurrent, mixing-tank hold) even if ESP1 goes silent.

Firmware source: https://github.com/JKyle-N/Smart-Irrigation

## What the dashboard does

- **Live Dashboard** — reservoir/mixing levels, flow rate, temperature, humidity, light, pH, EC, and
  battery voltage/percentage/current/power, read straight from ESP1's telemetry snapshot.
- **Zone Profiles** — per-column crop and growth-stage tracking, NPK/pH/EC/moisture readings against
  each crop's reference targets, and the firmware settings (mode, schedule, dosing targets) actually
  sent to ESP1.
- **Controls** — actuation enable/disable, reboot, emergency stop, and forced or queued irrigation
  runs.
- **Manual/Test** *(privileged accounts)* — direct hardware control: pump exercise cycles, timed
  pulses, and press-and-hold relay tests, all under ESP1/ESP2's own dead-man timers and safety
  limits.
- **Diagnostics** — ESP1's raw diagnostic values, pre-conversion sensor readings, an on-demand ESP2
  sensor sweep, and a recent-events feed.
- **System** *(privileged accounts)* — remote versions of the LCD's Settings menu: clock, thresholds,
  LCD-lock status, restore defaults.
- **User Management** *(operators only)* — approve, reject, restrict, or block accounts; grant
  1-hour temporary access or a persistent Sub-operator role; see each account's online/offline state
  and recent activity.

Access is tiered — pending / approved / restricted / rejected / blocked, plus two fixed operator
accounts and an optional temporary or Sub-operator elevation on top of "approved" — and enforced
server-side by `firebase-rules.json`, not just by what the page shows or hides.

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
| `irrigation/commands/$id` | page (queued) → ESP1 (status) | One-shot commands; ESP1 validates every payload |
| `irrigation/config/zones` | page only | Which crop and stage each zone is growing. Dashboard-side notes — the numeric targets reach the rig through `SET_COLUMN`, not from here |
| `irrigation/manual` | page only | `{seq, want}` — the Manual/Test mutual-exclusion hold. ESP1 reads it and answers in `live` under `diagnostics.webManual`. Acquiring (`want:true`) requires the privileged tier (operator/temp-access/sub-operator, see `users/$uid` below); releasing (`want:false`) is open to anyone signed in |
| `irrigation/testHold` | page only | `{seq, bit, want}` — Manual/Test's press-and-hold buttons. ESP1's `firebasePollTestHold()` reads it and drives ESP2's existing `TEST,HOLD`/`TEST,RELEASE` dead-man; `bit` is restricted (by the rule) to the same 10-target whitelist as `TEST_PULSE`'s `target` field (transfer/colA/colB/colC/mixer/nutA/nutB/nutC/phUp/phDn) |
| `presence/$uid` | every signed-in session, its own | `{online, lastChanged}` via `.info/connected`+`onDisconnect()`. Read-restricted to the two operator UIDs (plus self) — powers User Management's online/offline display |
| `activity/$uid` | every signed-in session, its own | `{what, at}` entries, one per command actually queued (see `activitySummary()`/`queueCommand()`). Read-restricted to **only** the two operator UIDs, not even the account's own owner — powers User Management's recent-activity list |
| `users/$uid` | see `status`/`tempAccessUntil`/`subOperator` | `tempAccessUntil` (ms timestamp) is a 1-hour, operator-granted Manual/Test+System elevation; `subOperator` (boolean) is the same elevation but persistent until an operator withholds it again. Both settable only by the two operator UIDs, both cleared automatically the moment status leaves `'approved'` (reject/block/restrict) |

## Security rules

`firebase-rules.json` is the authoritative copy of the database rules. Rules are enforced
server-side, so keeping them in the repo costs nothing in security and makes changes reviewable.

**After editing it, paste the whole file into Firebase Console → Realtime Database → Rules and
publish.** The console is the live source; this file is only version control, and the two drift
apart silently if you change one without the other.

`irrigation/manual` needs its rule present or the Manual/Test tab does not work at all — RTDB denies
by default where no rule matches, so without it both the page's write and ESP1's read are rejected
and the hold is never granted. Proceed itself (2026-09-10 revision) no longer waits for that grant —
it reveals the panel for any privileged-tier account regardless of ESP1's connection state, so the
individual test buttons (gated on a fresh snapshot) are what actually stays unusable, not the screen.

Credentials and the device UID live in `Firebase infos.txt` at the repo root, which is gitignored.
