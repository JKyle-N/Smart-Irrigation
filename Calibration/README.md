# Calibration — standalone sensor-calibration bench tools

Standalone, per-sensor calibration firmware for the Smart Irrigation rig. Each tool runs on the
target controller **on its own** — no ESP32 #1, no UART link, no framed packets, no main firmware —
and talks to you over the USB serial monitor at **115200 baud**. You calibrate one sensor, the tool
prints a **paste-ready constant**, and you drop that value into the real firmware
(`Nano/Nano.ino`, `ESP2/src/main.cpp`). See `../Settings_Calibration_and_Safe_Edit_Spec.md` §A for
the methods these implement.

These are the bench basis for the eventual on-device Calibration Mode; they deliberately reuse the
**exact pins and formulas** of the real firmware so measured values transfer 1:1.

## Layout

```
Calibration/
  Nano/        Arduino .ino tools (open in Arduino IDE, or `pio run` per folder)
    Soil/         per-column capacitive air/water endpoints
    Ultrasonic/   empty-tank reference distances (2 tanks)
    DHT22/        temp + humidity offset trims
    BH1750/       lux offset trim
    NPK/          raw RS485 registers + guarded N/P/K offset trim
    Flow/         reservoir flow K-factor (pulses/L), 3-run + outlier
  ESP2/        PlatformIO projects (env esp32dev)
    pH/           linear pH fit -> PH_CAL_M / PH_CAL_B
    EC/           linear EC fit -> EC_CAL_M / EC_CAL_B
    ACS712/       mixer-current zero offset (+ optional sensitivity)
    Flow/         8x flow K-factors; DRIVES the paired pump/valve under a dead-man
    FlowPinFinder/ identify which flow sensor is on which GPIO (reads all flow pins)
```

Flow is separated from the other sensors on both controllers, as required.

## Building & flashing

**PlatformIO** (`pio` is at `"$HOME/.platformio/penv/Scripts/pio.exe"`, not on PATH here):

```bash
# ESP2 tools (from the tool folder)
cd ESP2/pH   && pio run -e esp32dev            # -t upload to flash (set upload_port first)
cd ESP2/Flow && pio run -e esp32dev

# Nano tools (each folder has a platformio.ini so it builds in CI / on a phone too)
cd Nano/Soil && pio run -e nanoatmega328       # -t upload to flash
```

**Arduino IDE (Nano):** open the folder's `.ino` (folder name matches the sketch), Board *Arduino
Nano*, Processor *ATmega328P* (Old Bootloader on older clones). Install libraries where noted:
DHT22 → *DHT sensor library* + *Adafruit Unified Sensor*; BH1750 → *BH1750* (Christopher Laws).
**Disconnect D0/D1** (the UART-to-ESP1 header) while flashing the Nano, then reconnect.

Every tool prints a **help banner** on boot; type `h` any time. All use a common command style:
`h` help · `r` read once · `live` toggle a ~2 Hz readout · plus per-sensor capture/compute verbs.

## What each tool measures → where it goes

| Tool | Method (spec §A) | Prints → paste into |
|---|---|---|
| Nano/Soil | 2-pt air/water per column | ESP32 #1 soil map (air_raw/water_raw) |
| Nano/Ultrasonic | 1-pt empty-tank reference | ESP32 #1 ultrasonic geometry |
| Nano/DHT22 | offset vs reference (default 0) | ESP32 #1 ENV temp/hum offset |
| Nano/BH1750 | offset vs reference (default 0) | ESP32 #1 LIGHT offset |
| Nano/NPK | raw registers + guarded trim (default 0) | ESP32 #1 NPK offset (bias-guarded) |
| Nano/Flow | K = pulses/L, 3-run + outlier | `Nano.ino` `FLOW_K_PULSES_PER_LITER` |
| ESP2/pH | linear least-squares (2–3 buffers) | `main.cpp` `PH_CAL_M` / `PH_CAL_B` |
| ESP2/EC | linear least-squares (zero + standard) | `main.cpp` `EC_CAL_M` / `EC_CAL_B` |
| ESP2/ACS712 | zero-offset (motor OFF) | `main.cpp` `ACS712_ZERO_V` |
| ESP2/Flow | K = pulses/L, 3-run + outlier, per channel | `main.cpp` `K_RES_MIX`/`K_MIX_IRR`/`K_NUT[]` |

## ⚠️ ESP2/Flow drives real actuators

The ESP2 flow tool opens the line's valve(s) and runs its paired pump (per the Prime pairing table,
spec §A.4.2.1) so water actually moves — but **only while you physically hold the ESP32's on-board
BOOT button (GPIO0)** (a hardware dead-man), and never past a hard per-class cap (~3 s main-line,
~15 s dosing). Releasing BOOT or hitting the cap **stops everything and returns all outputs to SAFE**;
after a cap-stop you must release and re-press to continue. Only run it with the rig plumbed and a
measuring vessel in place. `off` is a panic-to-SAFE at any time.

Flow procedure (one channel): `ch RESMIX` (or `ch MIXIRR A`) → **hold the BOOT button** to fill your
vessel → release → `vol 3.0` (the liters you caught) → repeat 3× → `calc` (median outlier check, then
the averaged K). Dosing channels accumulate pulses across several hold/release bursts within a run, so
you can fill a graduated cylinder to a clean mark.

## Don't know which flow sensor is on which pin? — `ESP2/FlowPinFinder`

Flash `ESP2/FlowPinFinder`, open serial 115200, then **spin / blow through one flow sensor at a time**.
It watches all 8 flow GPIOs at once and prints the pin whose pulse counter climbs — e.g.
`>>> THIS SENSOR IS ON GPIO18 (currently labeled NUT_A)`. It drives no actuators (read-only), so you
move the sensor by hand. Type `z` to zero the counters between sensors. Once you've mapped every
sensor, put the correct GPIO numbers into `ESP2/src/main.cpp` (`FLOW_*` at :120-126 + `flowPinForId()`
at :1145-1155) and into `ESP2/Flow`'s `CH[]` table. If a sensor never registers, its wire is on a pin
not in the list — add that GPIO to `PINS[]` in the sketch and re-flash.
