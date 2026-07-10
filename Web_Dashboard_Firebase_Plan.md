# Firebase Web Dashboard — Feasibility Study & Implementation Plan

> Smart Irrigation & Fertigation System — remote website for logs, errors, and config over the internet.
> Status: **PLANNED, not implemented.** Saved for later. No firmware/docs were changed to produce this.
>
> **Before implementing this, back up the current firmware** (`ESP1/src/main.cpp` + `platformio.ini`,
> and ideally a full `pio run` build) so you can roll back — this feature adds a lot to the core-0 netTask.

---

## Context / Goal
A **website reachable from anywhere** that:
1. Downloads/previews the SD logs.
2. Mirrors the safe subset of SMS functions.
3. Shows system errors.
4. Edits per-column functions + thresholds (Irrigate_Only / AUTO / OFF, N/P/K/pH, soil start/stop/gap).
5. Is **active only when WiFi is OK**.

**Feasibility finding:** the ESP32 is **not reachable from the public internet** (it is an outbound HTTP
client; its web server binds only to the SoftAP at `192.168.4.1`). True off-site access therefore requires a
**cloud intermediary**. About **70%** of the feature logic already exists for the SoftAP portal and is reused.

## Confirmed decisions
| Decision | Choice |
|---|---|
| **Reach** | True internet via **Firebase** (Realtime Database + Hosting + Auth), **Spark/free** plan |
| **Scope** | **Reads + config only** — NO e-stop/recovery from web (STOP/RELEASE/IRRIGATE/NORMAL stay SMS/LCD-only) |
| **Logs** | **Batched new-rows** — per-day byte offset, upload every ~1–5 min into **RTDB**, pruned to **90 days**; SD keeps the full archive |
| **Telemetry** | **Keep ThingSpeak** for sensor graphs; Firebase handles logs/errors/config |
| **Sync** | Poll RTDB for pending config commands every **~30–60 s**; **backfill** missed logs/errors from SD offset on reconnect |
| **Auth** | Firebase Auth — **admin** (you: read+edit) + **read-only adviser**; ESP32 signs in as a dedicated **device account** (email/pass → ID token, auto-refreshed) |
| **Errors** | **Supplement SMS** (not replace) — SMS stays the offline fallback; web shows a live recent-fault list |

## Architecture
```
Browser (admin / adviser)  --Firebase Auth-->  Firebase RTDB  <--HTTPS REST-- ESP32 (core-0 netTask)
        Firebase Hosting (static SPA)               |                              |  (WiFi OK only)
                                              90-day pruned data            SD /YYYYMMDD.CSV, ThingSpeak (unchanged)
```
RTDB layout under `/devices/<deviceId>/`:
- `status/` — live snapshot + current config
- `errors/` — recent-faults ring
- `logs/<YYYYMMDD>/` — batched rows
- `config/` — authoritative config, device-published
- `commands/` — web-written pending edits; device applies then deletes

## Firmware changes — `ESP1/src/main.cpp` (all in the core-0 `netTask`; control logic untouched)
Transport: **lean raw HTTPS REST** with `WiFiClientSecure` + `HTTPClient` + `ArduinoJson` (avoids the heavy
mobizt library to control RAM). Pin Google **GTS Root R1** CA (fallback `setInsecure()` — documented as a con).
Gate every Firebase action on `wifiConnected && !portalActive && wifiEnabled && firebaseEnabled`.

New pieces (mirror existing `tsUpload` / portal patterns):
- **`firebaseTick()`** — new cadence branch in `netTask` (like `tsUpload`): refresh token, push status, push
  errors, batch-push logs, poll + apply commands.
- **`fbAuthEnsureToken()`** — Firebase Auth REST (`identitytoolkit`): sign in device account / refresh ID
  token (~55 min). Creds + refresh token in NVS.
- **`fbPushStatus()`** — snapshot `telem` under `telemMux` + current config → PUT `status/`.
- **In-RAM error ring** — small `struct FaultRec{ts,tier,code,loc}` circular buffer appended inside
  **`raiseFault()`** so SMS and Firebase share one source; `fbPushErrors()` PATCHes new entries.
- **`fbPushLogsBatched()`** — under `sdMux` (reuse `sdTake/sdGive` + the `portalHandleDownload` read pattern),
  read `/<daystamp>.CSV` from a persisted byte **offset** (NVS key per day), PATCH new rows to `logs/<day>/`,
  advance offset. Backfill = resume from stored offset after a WiFi gap.
- **`fbPollCommands()`** — GET `commands/`; for each, translate to the SMS-format string and feed the
  **existing `portalCfgEnq()` → `handleSms()` replay path** (`smsMute` on); then re-publish `config/` and
  delete the command. Only config verbs (MODE/NAME/SET/THRESH; optionally TSKEY/WIFI) are honored.
- **Config/creds** — new NVS keys (Firebase enable, device email/pass or refresh token, RTDB URL, deviceId,
  Web API key) via a new `FBCFG` SMS command + a SoftAP portal form (copy the `TSKEY` / `saveTsKey` pattern).
- Likely **bump `netTask` stack** (20 KB → ~28–32 KB) for the TLS handshake buffers; verify against RAM.

**Reuse (do not rebuild):** `handleSms()` interpreter, `portalCfgQ` / `portalCfgEnq` / `smsMute`,
`raiseFault()`, `sdTake/sdGive` + CSV read, `telem` / `telemMux`, `wifiConnected` / `wifiEnabled` /
`portalActive`, the `saveTsKey`-style NVS pattern, and the `TS_UPLOAD_*_MS` cadence structure.

## Web app — new deliverable (separate repo/folder, NOT the firmware)
Static SPA on **Firebase Hosting** + Firebase JS SDK. Pages:
- **Dashboard** — realtime `status/`
- **Logs** — pick a day from `logs/<day>`, render + rebuild CSV client-side for download
- **Errors** — realtime `errors/`
- **Config** — edit thresholds + per-column mode/name/targets/preset → write to `commands/`, show applied
  state from `config/`

Two roles via Auth custom claims: **admin** (write `commands/`), **viewer** (read-only).

## Firebase security rules
- `status/`, `errors/`, `logs/`, `config/`: read = authed users; write = **device UID only**.
- `commands/`: write = **admin only**; read = device + admin; device deletes after apply.
- Viewer = read-only everywhere. Test in the Rules simulator (viewer cannot write; device cannot escape its
  own `/devices/<id>` subtree).

## Cons / risks (for the thesis writeup)
- **Two auth layers + TLS upkeep** — device token refresh, CA cert, storing device creds in NVS.
- **RAM pressure** — `WiFiClientSecure` handshake buffers on top of an already-large firmware; must verify
  headroom and stack size.
- **WiFi-gated** — web control disappears when connectivity is bad → SMS remains the required fallback (web
  cannot fully replace SMS).
- **Free-tier limits** — Spark RTDB 1 GB store / 10 GB-month download; pruning to 90 days keeps well under.
- **No OTA** — `huge_app` has no OTA slot; firmware updates still over USB.
- **Cloud dependency** — a Firebase/Google outage or project-quota exhaustion disables the site (not the rig).

## Verification
- **Build:** `pio run -e esp32dev` (PATH: `$USERPROFILE/.platformio/penv/Scripts`); watch Flash/RAM, bump
  `netTask` stack if the TLS handshake starves; confirm ThingSpeak + SMS still work (Firebase is additive).
- **Bench** (test RTDB + throwaway device account): device signs in + refreshes token; `status/` updates on
  cadence; a raised fault appears in `errors/` and via SMS; `fbPushLogsBatched` advances the offset and new
  rows land in `logs/<day>`; a **config command from the web** (e.g. set COL_B IRRIGATION_ONLY, change soil
  start/stop) applies through `handleSms`, re-publishes `config/`, clears the command; drop WiFi for a few
  minutes → on reconnect the missed rows backfill from the SD offset.
- **Rules:** simulator confirms viewer read-only + device-subtree isolation.
- **Web:** admin edits config + downloads a day CSV; adviser account can view but not edit.

---

## Backup checklist (do this BEFORE implementing)
- [ ] Copy `ESP1/src/main.cpp` and `ESP1/platformio.ini` to a dated backup folder (or commit/tag in git).
- [ ] Optionally archive a known-good `.pio/build/esp32dev/firmware.bin`.
- [ ] Note the current build size (Flash ~37%, RAM ~16.5%) as the baseline to compare against after adding TLS.
- [ ] Record current NVS namespaces in use (`irrig`, `calib`, `adccal`) so new Firebase keys don't collide.

## Suggested phasing (lower risk than all-at-once)
1. **Prototype the device→Firebase status push** on real hardware first — proves the TLS/RAM cost and token
   refresh before committing to the rest.
2. Add **error ring + `fbPushErrors`** (small, high value).
3. Add **batched log upload** (offset + pruning).
4. Add **command poll → `handleSms` replay** (config edits).
5. Build the **web app** + security rules + adviser account.
