# Smart Irrigation — web dashboard

Static dashboard (`index.html`, `script.js`, `style.css`) served from GitHub Pages, talking to
Firebase Realtime Database. ESP1 is the only writer of live telemetry; the page reads it and queues
commands the firmware validates before acting on.

## Realtime Database layout

| Node | Written by | Purpose |
|---|---|---|
| `irrigation/live` | ESP1 only | Telemetry snapshot, faults, event log, `diagnostics.webManual` |
| `irrigation/commands/$id` | page (queued) → ESP1 (status) | One-shot commands; ESP1 validates every payload |
| `irrigation/config/zones` | page only | Which crop and stage each zone is growing. Dashboard-side notes — the numeric targets reach the rig through `SET_COLUMN`, not from here |
| `irrigation/manual` | page only | `{seq, want}` — the Manual/Test mutual-exclusion hold. ESP1 reads it and answers in `live` under `diagnostics.webManual` |

## Security rules

`firebase-rules.json` is the authoritative copy of the database rules. Rules are enforced
server-side, so keeping them in the repo costs nothing in security and makes changes reviewable.

**After editing it, paste the whole file into Firebase Console → Realtime Database → Rules and
publish.** The console is the live source; this file is only version control, and the two drift
apart silently if you change one without the other.

`irrigation/manual` needs its rule present or the Manual/Test tab does not work at all — RTDB denies
by default where no rule matches, so without it both the page's write and ESP1's read are rejected,
the hold is never granted, and the tab's Proceed button stays disabled.

Credentials and the device UID live in `Firebase infos.txt` at the repo root, which is gitignored.
