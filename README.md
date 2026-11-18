# Smart Irrigation & Fertigation System

A solar-powered automated irrigation and fertigation system for precision agriculture
(undergraduate thesis project). It maintains soil moisture and nutrient levels per plant
column using sensor-driven decisions, doses nutrients open-loop toward a target N-P-K, and
supports remote monitoring/control over GSM (A7670C).

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
- **Open-loop, gap-based nutrient dosing** (`calcDose()`): mL = gap × batch litres ÷ stock, pulse-quantized, against a lab soil baseline, with a post-fertigation flush that keeps total water roughly constant. pH is validated, not dosed. *(With the shipped constants nothing doses yet: see "Status".)*
- **Non-blocking everywhere** — no `delay()` in normal loop flow (one 40 ms hand-off before an emergency power cut excepted); task watchdogs on both ESP32s, internal WDT on the Nano.
- **ESP2 powered off during idle** (power saving); ESP32 #1 powers it up on demand for each run and powers it back off.
- **GSM (A7670C)** alerts for all fault tiers, scheduled-run notices and on-demand reports, with send retries, delivery reports, a modem watchdog, a weekly `SIMTEST` text and an optional second alert number.
- **Pure ultrasonic water metering (web v.4)** — the two tank ultrasonics measure every litre the transfer and booster pumps move (the main-line flow meters are no longer used). A fill runs in two steps (since 18 Nov 2026): a timed transfer run for about 70 % of the litres needed, counted from the inverter switch-on; a 1-minute settle and a measurement; then a top-up timed with the rate that run really gave, when more than 0.8 L is missing. Any run ends early once the tank's live level shows enough. A delivery runs the booster until the mixing tank is under 5 % for 10 s. Pump lines come from bench measurements and can be re-measured on the LCD (Settings → Calibration → Transfer / Booster pump est).
- **Several columns in one run** — a forced run on A+B is one batch through both valves; the litres asked are the total. The LCD, the receipts and the website name every column, and the LCD's bar follows the fill and the delivery.
- **Web dashboard + cloud** — Firebase RTDB status once a minute, plus one report at once on the website's Refresh; the link goes quiet while a run is due or under way. Remote commands, ThingSpeak graphs, and the SD card's daily CSV logs sent to Firebase for a day someone asks for and downloaded on the website (Supabase only while Firebase is off), all from a core-0 task. SMS and the LCD remain the authoritative stop paths.
- **Kept away from the inverter's noise** — for every inverter use the boards move to a second battery (P0 DPDT, both + and − broken); a servo, not relays, chooses the AC pump; the inverter sits away from the sensor cables. The main-battery and solar sensors are not fitted: on the noisy 12 V side they disturbed the logic, and a clean board could not be built in time (reference §20.5).
- **SD logging** — RTC-timestamped CSV, one file per day (`YYYYMMDD.CSV`), buffered/batched writes.
- **Nano recovery ladder (2 layers)** — Nano internal WDT → ESP32 #1 `RESET_REQ` (after 5 consecutive garbage packets). The old hardware-reset layer was removed when PCF8575 **P17** became the master actuator-power cutoff.
- **P17 master cutoff + tank-aware fault hold** — on a hard ESP2 fault, ESP2 drops the P17 master power relay (bank goes dead, fail-safe), pauses, and *stays alive* holding the metered mixing-tank volume (persisted to NVS). ESP1 keeps ESP2 powered and alerts over GSM with the operation + column; recovery is user-gated: reply `STOP` / `RELEASE` / `IRRIGATE` / `NORMAL`.
- **Preventive pump exercise** (every 2 days) scheduled by the always-on ESP32 #1.

---

## Repository layout

```
ESP1/          ESP32 #1 master controller   (PlatformIO, env: esp32dev; host tests: env native)
ESP2/          ESP32 #2 actuator controller (PlatformIO, env: esp32dev; host tests: env native)
Nano/          Arduino Nano sensor hub      (Arduino IDE .ino, or PlatformIO)
shared/        Headers used by more than one board: rig constants, tank shape -> litres, pump lines
Calibration/   Bench calibration tools per board (each its own PlatformIO project)
Website/       The dashboard (static site + Firebase rules); published separately on GitHub Pages
delete-user-api/  The website's account-deletion function (Vercel); its credentials are environment variables
Test Code/     Bench sketches & per-sensor test programs (Arduino IDE + PlatformIO)
docs/          Diagrams for the two guides below
Smart_Irrigation_System_Reference_FINAL.md   Full specification (source of truth); §5.0 explains the system in brief
Accurate_Water_Delivery_Guide.md             The manual work and formulas for accurate litres
Nutrient_Dosing_Guide.md                     How a dose is decided, and the open questions on it
CLAUDE.md      Locked architecture & conventions
```

`.pio/` build output and downloaded libraries are gitignored. So is `ESP1/include/secrets.h` (owner phone
number, setup-AP password, admin PIN): copy `ESP1/include/secrets.example.h` to `secrets.h` and fill it in.
Without it the firmware builds with the placeholders.

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
(LiquidCrystal_I2C, RTClib, EspSoftwareSerial, ArduinoJson for ESP1; PZEM-004T-v30 for ESP2).

### Arduino Nano — PlatformIO or Arduino IDE

- **PlatformIO:** `cd Nano && pio run` (env `nanoatmega328`; `-t upload` to flash). Libraries
  (DHT, Adafruit Unified Sensor, BH1750) are declared in `Nano/platformio.ini` and fetched
  automatically. Use board `nanoatmega328new` for a 115200 (new) bootloader.
- **Arduino IDE:** open `Nano/Nano.ino` — Board *Arduino Nano*, Processor *ATmega328P* (use
  *Old Bootloader* on older clones); install *DHT sensor library*, *Adafruit Unified Sensor*,
  *BH1750* from Library Manager.
- **Upload:** disconnect D0/D1 (the UART link to ESP32 #1) while flashing, then reconnect.

### Build without a laptop (GitHub Actions → phone)

Every push builds all three controllers in the cloud; you can also trigger it by hand from the
**Actions** tab → *Build firmware* → **Run workflow** (works from a phone browser). When the run
finishes, open it and download the artifacts — `ESP1-firmware` / `ESP2-firmware` (`firmware.bin`)
and `Nano-firmware` (`firmware.hex`) — then flash from the phone (e.g. an ESP32/AVR USB-OTG
flasher app or a web flasher). No compiler needed on the device.

---

## Pin / bus map (summary — see spec §17–19 for the authoritative version)

**ESP32 #1:** Nano link EspSoftwareSerial GPIO16/17 · ESP2 link HW UART1 GPIO25/33 ·
A7670C HW UART2 GPIO27/26 · I2C GPIO21/22 (LCD 0x27, secondary-battery INA226 0x40, ADS1115 0x48, DS3231 0x68; behind an ISO1540: solar INA226 0x41, main-battery INA226 0x44, supported but not fitted on the rig) ·
microSD SPI GPIO23/19/18/5 · buttons GPIO0/12/13/15/2 · ESP2 power relay GPIO4.

**ESP32 #2:** ESP1 link HW UART2 GPIO16/17 · PZEM-004T HW UART1 GPIO13/14 ·
PCF8575 actuator expander I2C GPIO21/22 (0x20) · dosing flow meters Nut A/B/C GPIO23/19/18 (Nut D 27, pH up 4,
pH down 25 wired, not used by the runs; the ResMix GPIO26 / MixIrr GPIO5 meters are no longer read in web v.4) ·
pH/EC/ACS712 analog inputs. The transfer and booster pumps share the inverter through a changeover switch moved by
the Nano's D3 servo.

**Nano:** UART to ESP32 #1 on D0/D1 · DHT22 D11 · 2× ultrasonic D4–D7 · reservoir inflow meter D2 ·
pump-selector servo D3 (LEFT = transfer, RIGHT = booster) ·
RS485/NPK D8/D9/D10 · soil A0–A3 + A6/A7 · BH1750 I2C A4/A5.

---

## GSM commands (inbound SMS)

| Command | Action |
|---------|--------|
| `HELP` / `HELP,RUN` / `HELP,CROPS` / `HELP,SYSTEM` | The command list, one text per topic |
| `STATUS` / `SUMMARY[,<day>]` / `FULL SUMMARY[,<day>]` | Report now / day averages / hour by hour, from the SD log |
| `SET,COL_A,N,150,P,40,K,200,pH,5.8` | Set per-column nutrient targets (mg/kg) + pH |
| `SET,COL_A,PRESET,PECHAY` | Apply a built-in crop preset (`PECHAY`, `TOMATO_S1`–`S3`) |
| `NAME,COL_A,Lettuce` / `MODE,COL_A,AUTO\|IRRIGATION_ONLY` / `THRESH,<start %>,<gap>` | Plant name / column mode / soil start % and fertigation gap |
| `STOP` (`STOP,ALL`) | Emergency stop |
| `NORMAL` / `IRRIGATE` / `RELEASE` / `SERVO` / `STOP` | Fault-hold replies (only while a fault is held): resume / finish water-only / deliver the tank as is / switch the pump selector again / stay on hold |
| `CANCEL,30` / `CANCEL,10` / `CANCEL,OFF` | End the run and skip the column 30 / 10 min, or switch it off |
| `TANKCHECK` / `TANKCHECK,OK` | Is a tank check needed (after a restart mid-run) / the tank was checked |
| `UPLOAD[,YYYYMMDD]` | Send a day's SD log to the website (today included) |
| `NET` / `WIFI,<ssid>,<pass>` / `WIFI,ON\|OFF` / `WIFIPORTAL[,STOP]` / `TSKEY,<ch>,<key>` | Network status and setup |
| `FBASE,STATUS` (and `FBASE,ON\|OFF\|<url>\|SIGNOUT\|TLS,…\|APIKEY,…\|AUTH,…`) | Website sync status and setup |
| `BATTSW[,CLEAR]` / `SIMTEST` / `BAL` / `USSD,<code>` / `ALERTNUM,<number>` | Battery switch, SIM check, balance, balance code, second alert phone |

The full list with replies: `Operator_Manual.md` §2 and the reference §12.2.5.

Outbound, one plain-English text each: alerts (`CRITICAL: …` / `ALERT: …` / `WARNING: …` / `NOTICE: …`, ending
with `(time CODE)`), run notices (`RUN: Col A …`) and paged reports (`DAILY REPORT <date> 1/N`, `SUMMARY …`).
Fault codes: reference §23.4.

---

## Commissioning notes

Hardware-dependent values are tagged in the source as `[MEASURE]`, `[TBD]`, or `[CONFIRM]`
(soil ADC calibration, ultrasonic tank geometry and litres, the transfer / booster pump lines, dosing flow K-factors, NPK Modbus addresses/baud,
EC/pH safe windows, battery thresholds, INA226 shunt, ACS712 sensitivity, the GSM recipient
number, etc.). Set these for your rig before deployment: `Calibration_Worksheet.md` ranks them
and `Commissioning_Flash_Plan.md` stages the testing.

---

## Status

**web v.4 "pure ultrasonic", 18 Nov 2026.** All three controllers compile cleanly (PlatformIO `esp32dev` /
`nanoatmega328`), and the host tests run in GitHub Actions on every push (`pio test -e native` on ESP1 and ESP2).

**On the rig:** ESP32 #1 runs `web v.4 (2026-11-18h)` and ESP32 #2 the two-step fill, both flashed on the evening of
18 Nov. The Nano runs its 17 Nov build. The first forced run with the two-step fill (A+B, 5.0 L) delivered 5.92 L;
single-run fills earlier that day had come in 27 % short to 53 % over.

**Where to read more:** the reference
[`Smart_Irrigation_System_Reference_FINAL.md`](Smart_Irrigation_System_Reference_FINAL.md):
- §5.0 explains the whole system in brief;
- §19.4.9.28–34 and §29.12 cover the 18 Nov changes;
- §20.5 covers the inverter noise.

**Most important open items:**
- ESP32 #1 restarts when its supply dips: give it and the A7670C a 2 A supply and a large capacitor at the modem.
- The Wi-Fi at ESP32 #1 is weak (about −80 to −90 dBm).

This is active thesis work; values marked for commissioning are not yet final.

---

## License

Released under the [MIT License](LICENSE) — © 2026 John Kyle Nacor.
