# Smart Irrigation & Fertigation System

A solar-powered automated irrigation and fertigation system for precision agriculture
(undergraduate thesis project). It maintains soil moisture and nutrient levels per plant
column using sensor-driven decisions, doses nutrients open-loop toward a target N-P-K, and
supports remote monitoring/control over GSM (SIM800L).

The full design specification is in
[`Smart_Irrigation_System_Reference_FINAL.md`](Smart_Irrigation_System_Reference_FINAL.md)
— the single source of truth — with locked conventions summarized in [`CLAUDE.md`](CLAUDE.md).

---

## Architecture

Three microcontrollers with strict role separation:

```
  Sensors → Arduino Nano ──UART──▶ ESP32 #1 (master) ──UART──▶ ESP32 #2 (actuators)
                                       │                          │
                                  GSM / LCD / SD              Pumps / Valves / Mixer
```

| Controller | Role | Source |
|------------|------|--------|
| **Arduino Nano** | Sensor hub only — reads sensors, frames packets, transmits. No decisions, no actuators, no fault classification. | [`Nano/Nano.ino`](Nano/Nano.ino) |
| **ESP32 #1** | Master controller — the single decision-making authority. Scheduling, per-column irrigate/fertigate logic, GSM, SD logging, LCD UI, fault classification, recovery. | [`ESP1/src/main.cpp`](ESP1/src/main.cpp) |
| **ESP32 #2** | Actuator executor — receives a complete work order and runs the full valve/pump/mixer/dosing sequence; meters flow; performs local protective stops. Executes, does not decide policy. | [`ESP2/src/main.cpp`](ESP2/src/main.cpp) |

**Build order:** Nano → ESP32 #1 → ESP32 #2.

---

## Key features

- **Framed UART protocol** `<START>,COMMAND,P1,P2,...,<END>` between all controllers — corrupt/partial packets are discarded, never executed.
- **Per-column AUTO / IRRIGATION_ONLY / OFF** modes; windowed RTC scheduling (AUTO or perpetual MANUAL).
- **Supervisor / work-order model** — ESP32 #1 hands ESP32 #2 a complete job and then monitors; ESP32 #2 owns the *how* and the timing.
- **Open-loop nutrient dosing** toward target N-P-K (mg/kg) + pH, with a post-fertigation flush that keeps total water roughly constant. *(The mg/kg → mL calculation is a delegated stub — see `CLAUDE.md`.)*
- **Non-blocking everywhere** — no `delay()` in any main loop; task watchdogs on both ESP32s, internal WDT on the Nano.
- **ESP2 powered off during idle** (power saving); ESP32 #1 powers it up on demand for each run and powers it back off.
- **GSM (SIM800L)** alerts for all fault tiers, consolidated scheduled-run notices, and on-demand reports.
- **SD logging** — RTC-timestamped CSV, one file per day (`YYYYMMDD.CSV`), buffered/batched writes.
- **Recovery ladder** — Nano internal WDT → ESP32 #1 `RESET_REQ` (after 5 consecutive garbage packets) → hardware reset via ESP32 #2.
- **Preventive pump exercise** (every 2 days) scheduled by the always-on ESP32 #1.

---

## Repository layout

```
ESP1/          ESP32 #1 master controller   (PlatformIO, env: esp32dev)
ESP2/          ESP32 #2 actuator controller (PlatformIO, env: esp32dev)
Nano/          Arduino Nano sensor hub      (Arduino IDE .ino)
Test Code/     Bench sketches & per-sensor test programs (Arduino IDE + PlatformIO)
Smart_Irrigation_System_Reference_FINAL.md   Full specification (source of truth)
CLAUDE.md      Locked architecture & conventions
```

`.pio/` build output and downloaded libraries are gitignored.

---

## Building & flashing

### ESP32 #1 and ESP32 #2 — PlatformIO

```bash
# ESP32 #1
cd ESP1 && pio run -e esp32dev -t upload    # set upload_port in platformio.ini first

# ESP32 #2
cd ESP2 && pio run -e esp32dev -t upload
```

Library dependencies are declared in each `platformio.ini` and fetched automatically
(LiquidCrystal_I2C, RTClib, EspSoftwareSerial, INA226 for ESP1; PZEM-004T-v30 for ESP2).

### Arduino Nano — Arduino IDE

Open `Nano/Nano.ino` (folder name must match the file name).

- **Board:** Tools → Board → Arduino AVR → Arduino Nano, Processor *ATmega328P*
  (use *Old Bootloader* on older clones).
- **Libraries** (Library Manager): *DHT sensor library* (Adafruit), *Adafruit Unified Sensor*, *BH1750* (Christopher Laws).
- **Upload:** disconnect D0/D1 (the UART link to ESP32 #1) while flashing, then reconnect.

---

## Pin / bus map (summary — see spec §17–19 for the authoritative version)

**ESP32 #1:** Nano link EspSoftwareSerial GPIO16/17 · ESP2 link HW UART1 GPIO25/33 ·
SIM800L HW UART2 GPIO27/26 · I2C GPIO21/22 (LCD 0x27, INA226 0x40, DS3231 0x68) ·
microSD SPI GPIO23/19/18/5 · buttons GPIO0/12/13/15/2 · ESP2 power relay GPIO4.

**ESP32 #2:** ESP1 link HW UART2 GPIO16/17 · PZEM-004T HW UART1 GPIO13/14 ·
PCF8575 actuator expander I2C GPIO21/22 (0x20) · flow sensors (interrupt, stage-based) ·
pH/EC/ACS712 analog inputs.

**Nano:** UART to ESP32 #1 on D0/D1 · DHT22 D11 · 2× ultrasonic D4–D7 · flow D2 ·
RS485/NPK D8/D9/D10 · soil A0–A3 + A6/A7 · BH1750 I2C A4/A5.

---

## GSM commands (inbound SMS)

| Command | Action |
|---------|--------|
| `SET,COL_A,N,150,P,40,K,200,pH,5.8` | Set per-column nutrient targets (mg/kg) + pH |
| `SET,COL_A,PRESET,CARROT` | Apply a built-in crop preset |
| `NAME,COL_A,Lettuce` | Name a column (persisted in NVS) |
| `MODE,COL_A,AUTO` / `MODE,COL_A,IRRIGATION_ONLY` | Per-column mode |
| `STATUS` | Live status + daily summary report |
| `SUMMARY` | Today's-activity digest, parsed from the SD log |
| `STOP,ALL` | Emergency stop |

Outbound: fault/warning alerts (`ALERT,...`), scheduled-run notices (`RUN,...`),
and compact daily/on-demand reports (`RPT,...` / `SUM,...`).

---

## Commissioning notes

Hardware-dependent values are tagged in the source as `[MEASURE]`, `[TBD]`, or `[CONFIRM]`
(soil ADC calibration, ultrasonic tank geometry, flow K-factors, NPK Modbus addresses/baud,
EC/pH safe windows, battery thresholds, INA226 shunt, ACS712 sensitivity, the GSM recipient
number, etc.). Set these for your rig before deployment. The nutrient mg/kg → mL dosing
calculation is intentionally left as an editable stub (see `CLAUDE.md`).

---

## Status

All three controllers compile cleanly (PlatformIO `esp32dev` / `nanoatmega328`). The
ESP32 #2 work-order execution path is implemented to spec and can be fully end-to-end
tested once the ESP32 #2 hardware is assembled. This is active thesis work — values marked
for commissioning are not yet final.

---

## License

Released under the [MIT License](LICENSE) — © 2026 John Kyle Nacor.
