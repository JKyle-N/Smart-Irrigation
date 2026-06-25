# CLAUDE.md — Smart Irrigation & Fertigation System

This file defines the locked architecture and conventions for this project. These decisions were made deliberately and must NOT be changed without explicit instruction. The full specification lives in `docs/reference.md` (the single source of truth).

## Plan before editing
For any non-trivial change, explain the plan and wait for approval before editing. This project's architecture was carefully reasoned; do not silently make architectural choices.

---

## System overview
Three controllers, strict role separation:
- **Arduino Nano** — sensor hub ONLY. Reads sensors, frames packets, transmits. No decisions, no actuators, no fault classification.
- **ESP32 #1** — master controller. Decisions, scheduling, GSM, logging, UI, fault classification. Single source of authority.
- **ESP32 #2** — actuator executor. Drives pumps/valves, counts flow, performs local protective stops. Executes; does not decide policy.

Build order: Nano → ESP32 #1 → ESP32 #2.
Layout: PlatformIO, `src/main.cpp` per controller.

---

## LOCKED: UART assignment (do not change)

**ESP32 #1:**
- ESP32 #2 → hardware UART1 (GPIO25 TX / GPIO33 RX)
- SIM800L → hardware UART2 (GPIO27 TX / GPIO26 RX), proven working
- Arduino Nano → **SoftwareSerial** (EspSoftwareSerial / plerup), GPIO16 RX / GPIO17 TX, 9600
- UART0 reserved for USB serial monitor (debugging kept)

**ESP32 #2:**
- ESP32 #1 → hardware UART2 (GPIO16/17)
- PZEM-004T → hardware UART1 (GPIO13 TX / GPIO14 RX), 9600 — relocated here from ESP32 #1

Rationale: ESP32 has only 3 hardware UARTs. PZEM moved to ESP32 #2 (co-located with pump control). Nano demoted to software serial because lost sensor packets are recoverable; SIM and ESP2 kept on hardware because they're timing/safety critical.

Do NOT use legacy AVR SoftwareSerial on ESP32 — use EspSoftwareSerial.

---

## LOCKED: Communication protocol
- Framed packets: `<START>,COMMAND,P1,P2,...,<END>`
- UPPERCASE commands, comma-separated, no spaces, max 128 bytes
- Never execute a packet unless both START and END markers validate
- Non-blocking everywhere. No `delay()` in any main loop (one-time startup settle in `setup()` only). Protects watchdogs, UART, flow interrupts, GSM.

---

## LOCKED: Nano recovery / watchdog ladder (2 layers)
1. Nano internal WDT (ATmega328) — self-recovery from lockup, highest priority, automatic.
2. ESP32 #1 `RESET_REQ` over UART — software reset, triggered after **5 consecutive garbage packets**.

There is **no hardware-reset layer**: PCF8575 P17 was repurposed from the Nano-RESET line to the **master actuator-power cutoff** (see below), so the Nano RESET pin is no longer driven and `RESET_NANO` is retired. If `RESET_REQ` fails to restore valid data, ESP32 #1 logs/alerts (Major) and continues on last-valid data.

Garbage = structural (bad framing/parse) OR semantic (impossible values). NPK −1 sentinel is NOT garbage. Counter resets on one good packet. "UART silence" is NOT a reset trigger (Nano's own WDT handles that).
Daily fresh-start `RESET_REQ` once/day.

## LOCKED: P17 master actuator-power cutoff + tank-aware fault hold (sec.19.4.8)
- **P17 (PCF8575 bit 15)** drives a master relay gating power to the entire P0–P16 actuator bank. Fail-safe wiring: loss of P17 = all actuators OFF (loads on normally-open contacts). ESP2 energizes P17 at the start of any actuation, de-energizes on fault/completion/idle.
- **On a hard fault (FLOW_FAIL / PWR_FAIL / SAFE_STOP):** ESP2 drops P17, **PAUSES** the sequence, and **STAYS ALIVE** holding the **mixing-tank volume** (metered from ESP2's OWN flow sensors — the Nano ultrasonic is display-only, NOT trusted for control — and persisted to NVS so a reboot can't overfill). ESP1 keeps ESP2 **powered** (does NOT cut GPIO4) and must NOT power it down at fault, or the tank volume is forgotten and the next fill overfills. The mixing tank's only outlet is the booster → a column (no drain).
- **Recovery is user-gated over GSM (or the LCD fault screen):** `STOP` (acknowledge & keep holding), `RELEASE` (dump tank to its column as-is), `IRRIGATE` (top up plain water to budget, deliver, no dosing), `NORMAL` (resume the paused sequence). ESP1 → ESP2 as `RESUME,<NORMAL|IRRIGATE|RELEASE>`. The fault SMS carries the operation (irrigation/fertigation) + column. A `STOP` reply skips the column for the rest of the day.
- GPIO4 power-cut is retained ONLY as the backstop for a **frozen** ESP2 and the manual **MODE+BACK** emergency stop (cutting GPIO4 de-energizes the PCF8575 → P17 drops → bank dead).

---

## LOCKED: GSM behavior (SIM800L)
- Texts ONLY for: all-tier fault/warning alerts, scheduled-run notices, daily/on-demand reports. Not continuous telemetry.
- Scheduled-run notice: ONE consolidated SMS per run (all scheduled columns in one text).
- Daily summary + on-demand both via `STATUS` keyword. Compact encoded for manual expansion.
- Inbound: `SET,COL_A,N,150,P,40,K,200,pH,5.8` (explicit, default), `SET,COL_A,PRESET,CARROT` (named), `NAME,COL_A,Lettuce` (persistent NVS), `MODE,...`, `STOP,ALL`.
- Inbound (added): `SUMMARY[,<day>]`, `FULL SUMMARY[,<day>]`, `NET`, `WIFI,<ssid>,<pass>`, `TSKEY,<1|2|3>,<key>` — see the report/telemetry sections below. `WIFI`/`TSKEY` are owner-gated (sender's last 9 digits must match `PHONE_NUMBER`).

---

## LOCKED: Logging
- All logging on ESP32 #1, RTC-timestamped. Daily CSV files `YYYYMMDD.CSV`, fallback `NODATE.CSV` + alert.
- Columns: `timestamp,source,event_type,detail`. Detail uses `|` secondary delimiter (no commas inside).
- Buffered batch-write (RAM buffer → periodic flush) to protect timing/SD. Flush immediately on critical fault.
- Log everything (packets, exchanges, state changes, faults, resets, dosing, actuator, GSM, power). Clean sensor packets logged once as data line; garbage logged with raw bytes + counter.

---

## LOCKED: Nutrient dosing — DELEGATED, leave as stub
- The mg/kg → mL dosing calculation is being done separately. Do NOT implement the formula.
- Leave concentration constants, lab soil baseline, and crop presets as clearly-labeled **editable constants/stubs** at the top of the relevant file.
- Preset input: target N-P-K in mg/kg (elemental) + pH. Output: mL per bottle. Open-loop (no live sensor feedback during dosing).
- Nutrients A, B, C only. **Nutrient D (P14 / GPIO25) is wired but UNUSED** — do not drive it.

---

## ADDED: Operator safety & UI (ESP32 #1)
These were added after the original spec; treat them as locked.
- **Emergency-Off combo:** pressing **MODE + BACK together** triggers `enterEmergencyStop(true)` — STOP_ALL to ESP2, ESP2 power cut, state `EMERGENCY_STOP`. Works even while the LCD is locked (safety override). Recovery is operator-explicit: the fault screen shows a selectable `> Return to normal` / `Stay stopped`; UP/DOWN choose, ENTER confirms → `STARTUP_SYNC` (re-powers + re-validates). The old "BACK clears E-STOP" shortcut was removed.
- **LCD Lock:** a "Lock Screen" item in the Settings menu sets `lcdLocked` (persisted to NVS; **manual only, never automatic**). While locked, all buttons are ignored **except** UP+DOWN (unlock) and the MODE+BACK emergency combo; the lock screen shows the unlock hint. Lock is independent of `uiMode`, so **automation keeps running** while locked (irrigation/fertigation/logging/telemetry unaffected).

## ADDED: ESP32 #1 self-recovery (device-loss daily reset)
- If a core device that was **present at boot** (RTC 0x68, LCD 0x27, INA226 0x40, or SD) **drops out at runtime** (suspected I2C wedge), ESP1 reboots itself via `ESP.restart()` — **at most once per calendar day**.
- Boot-loop-safe: only a present→absent **transition** triggers it; a device absent from boot just runs degraded. The once/day guard is an `RTC_NOINIT_ATTR` day stamp (survives the software reset); a cold-boot sentinel allows the first reset even with a dead RTC, then self-limits.

## ADDED: WiFi + ThingSpeak telemetry (graphs)
- ESP32 #1 joins WiFi and uploads telemetry to **ThingSpeak** (live per-field graphs, viewable anywhere). SMS is **kept** for alerts/commands; WiFi only carries telemetry. All network I/O runs in a **FreeRTOS task pinned to core 0** so blocking HTTP never disturbs the core-1 loop or the 8 s task WDT. Cross-core data via a spinlock-guarded snapshot; creds/keys via a mutex.
- **3 ThingSpeak channels** (write key per channel, set by SMS): **Ch1 "Columns"** = per-column moisture + N/P/K; **Ch2 "System"** = temp, hum, lux, reservoir%, mixing%, battV, battW, flow; **Ch3 "Chem"** = per-column EC + pH.
- Config by SMS (owner-gated, NVS-persisted): `WIFI,<ssid>,<pass>` (password = remainder after the 2nd comma, may contain commas/spaces), `TSKEY,<1|2|3>,<writeKey>`, `NET` (status). Upload is plain HTTP (unencrypted — acceptable for this rig).

## ADDED: SMS reports — day-selectable SUMMARY + FULL SUMMARY
- **STATUS is unchanged** (always current day).
- **`SUMMARY[,<day>]`** = the day's **averages** (per-column NPK + moisture, power avg + Wh, water/nutrient totals, counts). `<day>` = (none/`TODAY`) | `YESTERDAY` | `YYYYMMDD` | `NODATE`.
- **`FULL SUMMARY[,<day>]`** = uncapped **hourly** record (per-column N/P/K + moisture + battery V/W per hour) + **deduped errors** (each code once, first timestamp) + run/dose events + daily peaks/totals. Segments stream paced to the SMS queue (no drops), `(i/N)`-tagged; tunable `FULL_SUMMARY_MAX_SMS` (0 = unlimited).
- **NODATE path:** when the RTC is dead, logs go to `/NODATE.CSV` (timestamps `0000-00-00 00:00:00`); both reports resolve there automatically and `,NODATE` selects it explicitly. Both reuse the bounded, non-blocking incremental SD parser.

## ADDED: ESP2 hardware-fault reporting
- **PCF8575 health:** ESP2 checks every relay write + periodically probes the PCF; after a few consecutive failures (debounced) it sends `PCF_FAIL,I2C` (re-asserted ~10 s) and `PCF_OK` on recovery. ESP1 = **Major** (alert + log, no shutdown — user policy); other safeties (PZEM no-current, flow timeout) still apply.
- **EC/pH sensor fault:** in the dosing EC/pH step, a railed ADC (disconnected/shorted probe) sends `SENSOR_FAIL,EC` / `SENSOR_FAIL,PH` — distinct from `EC_FAIL`/`PH_FAIL` (out-of-window). ESP1 = **Major**; the mixed batch still delivers (no drain).

## ADDED: Testing-mode arming hardening
- Entering Settings>Testing powers ESP2 via the GPIO4 relay, **primes** it (~2 s boot settle), then **re-sends `TEST,ENTER` every ~1 s until ESP2 replies `ACK,TEST,ENTER`** (`esp2TestArmed`). Fixes the old reliance on a single boot `READY` that is easily lost on a cold relay power-up — which left ESP2 not in test mode so every `TEST,HOLD` was dropped (relays dead).
- The LCD Testing header shows the **real** state: `ESP2 DOWN` (no heartbeat) / `ARMING...` (alive, not yet confirmed) / `(dead-man)` (confirmed, relays controllable) / `NO ACK-chk link` (ESP2 heartbeats but never ACKs after several tries → suspect the ESP1→ESP2 TX link, GPIO25→GPIO16).
- Diagnostic tool: `Test Code/ESP2_PCF8575_SEQ/` (PlatformIO) flashes to ESP2 alone and walks the relays P0→P15 (1 s gap, active-LOW) — if relays click here but Testing still fails, the fault is the ESP1↔ESP2 link, not wiring/PCF.

## ADDED: review-fix behaviors (locked)
- **Nano interval pacing:** ESP1 now actively commands the Nano `ACTIVE`/`DAY`/`NIGHT` interval (single owner `nanoPaceTick()`, sent only on change) — ACTIVE during runs, else DAY/NIGHT by RTC (night = `NIGHT_START_MIN`..`NIGHT_END_MIN`).
- **RES_LOW one-shot:** the reservoir-low fault is latched (alerts once per low episode, cleared on recovery) — no per-loop fault/SMS storm.
- **ESP2 timer cancel:** `STOP_ALL`/`TEST` clear in-flight exercise / Nano-reset one-shot timers (no stray late `DONE`). Irrigation/fertigation STOP logs now include `|W=<liters>` so per-day water is recoverable from the CSV.

---

## ADDED: Calibration Mode (companion spec `Settings_Calibration_and_Safe_Edit_Spec.md` §A)
- On-device sensor calibration from the ESP32 #1 LCD, no reflashing. **Subsystems stream RAW; ESP32 #1 owns and applies all calibration constants.** The Nano now sends RAW packets (soil ADC, ultrasonic distance, NPK registers) and ESP1 converts on receipt; ENV/LUX get ESP1 offsets.
- Calibration lives in a **separate NVS namespace** (`calib`) — never touched by Restore-Defaults (Phase 2). ESP2-owned constants (8 flow K-factors, EC/pH cal, ACS712 zero) are runtime vars delivered three ways: Save-time `SET_CAL` block-until-ACK, STARTUP_SYNC baseline push, and inside every work order (`KMAIN/KNUT/ECCAL/PHCAL`).
- New UART: `CAL_START`/`CAL_STOP`/`CAL` (raw stream), `SET_CAL` (push+ACK), `PRIME_START`/`PRIME_STOP` (valve+pump purge, dead-man + generous cap). Calibration is **IDLE-only**; flow cal + Prime reuse the TEST dead-man pattern. SD log uses a dedicated `CAL` event_type.
- The LCD calibration UI covers soil (2-pt), pH/EC (2-pt), ACS712 zero, ultrasonic empty, env/hum/lux offset trims, the guarded NPK offset trim (default 0, warning gate, DOWN=reset), and **3-run flow K with a median outlier check** (redo-worst or save median/average).
## ADDED: Settings safe-edit (companion spec §B/§C)
- **Edit-Confirmation (B):** any Settings editor that changes a stored value tracks a dirty flag; leaving (BACK) a dirty editor shows a three-way **SAVE / DISCARD / CANCEL** dialog (edits stage in `editTmp`, commit via `commitEditor()`). A Critical/E-stop fault force-dismisses + auto-discards; non-critical faults wait behind the dialog; a remote `SET`/`MODE`/`NAME` SMS during a local edit is **deferred** and applied on exit. Excludes Lock Screen + Testing; Calibration has its own Save.
- **Restore Defaults (C):** Settings row, **idle-only**, double-confirm (NO/YES). `restoreDefaults()` resets the operational-config namespace to compiled defaults and **keeps calibration (separate namespace), `COLUMN_ENABLED` (physical wiring), and WiFi/TS setup**; logs a `RESET` event; no reboot.

## Conventions
- EC/pH safe-window, hysteresis %, mixing duration = editable constants, values `<TBD>` (set at commissioning). Do not invent final values.
- All config (presets, names, calibration K-factors, thresholds) persists in ESP32 #1 NVS; distributed to ESP32 #2 at startup sync.
- Flow-to-volume uses editable per-sensor K-factor (pulses/liter).
- When in doubt about an architectural decision, check `docs/reference.md` or ask — do not guess.
