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

## LOCKED: Nano recovery / watchdog ladder
1. Nano internal WDT (ATmega328) — self-recovery from lockup, highest priority, automatic.
2. ESP32 #1 `RESET_REQ` over UART — software reset, triggered after **5 consecutive garbage packets**.
3. Hardware reset (ESP32 #1 → ESP32 #2 → PCF8575 P17 → Nano RESET) — last resort.

Garbage = structural (bad framing/parse) OR semantic (impossible values). NPK −1 sentinel is NOT garbage. Counter resets on one good packet. "UART silence" is NOT a reset trigger (Nano's own WDT handles that).
Daily fresh-start `RESET_REQ` once/day, exempt from the once-per-day hardware-reset limit.

---

## LOCKED: GSM behavior (SIM800L)
- Texts ONLY for: all-tier fault/warning alerts, scheduled-run notices, daily/on-demand reports. Not continuous telemetry.
- Scheduled-run notice: ONE consolidated SMS per run (all scheduled columns in one text).
- Daily summary + on-demand both via `STATUS` keyword. Compact encoded for manual expansion.
- Inbound: `SET,COL_A,N,150,P,40,K,200,pH,5.8` (explicit, default), `SET,COL_A,PRESET,CARROT` (named), `NAME,COL_A,Lettuce` (persistent NVS), `MODE,...`, `STOP,ALL`.

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

## Conventions
- EC/pH safe-window, hysteresis %, mixing duration = editable constants, values `<TBD>` (set at commissioning). Do not invent final values.
- All config (presets, names, calibration K-factors, thresholds) persists in ESP32 #1 NVS; distributed to ESP32 #2 at startup sync.
- Flow-to-volume uses editable per-sensor K-factor (pulses/liter).
- When in doubt about an architectural decision, check `docs/reference.md` or ask — do not guess.
