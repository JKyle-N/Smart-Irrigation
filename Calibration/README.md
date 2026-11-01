# Calibration

One small tool per component. Each runs on its own board with no other firmware, talks to you over USB at 115200 baud, and tells you the next step as you go. **Every tool here has been compiled, and the INA226, Flow, CalStore, pH and opto tools have run against a simulated board. None has run on real hardware yet.**

## Using a tool

From this folder, in PowerShell:

```powershell
.\cal.ps1                 # list the tools
.\cal.ps1 ina226          # flash it, then open its serial monitor
.\cal.ps1 ina226 -Monitor # only open the monitor
```

**Set your COM ports once, in `common.ini`.** On this PC auto-detect can pick a Bluetooth port, so the ports are named there. Close the monitor with Ctrl+C.

Every tool works the same way: `h` is help, `r` reads once, `live` switches a live readout on and off, and each result ends with a block to paste. Flashing a tool replaces that board's firmware until you flash the firmware back.

On the Nano, **disconnect D0/D1** (the link to ESP1) while flashing and while using the tool. The sketches also open in the Arduino IDE (board Arduino Nano, ATmega328P, Old Bootloader on older clones).

## Where a result goes

Almost every result goes to **ESP1's NVS**, using the `set` lines the tools print:

1. Run the tool. It ends with lines like `set kNA 452.3`.
2. `.\cal.ps1 calstore`, on ESP1, and paste those lines in.
3. Flash ESP1's main firmware back. It loads them at boot. Flashing does not erase NVS; only `pio run -t erase` does.

**Why not just edit the source?** ESP1 loads NVS over the constants in its source, and pushes its flow, pH, EC and mixer-zero values to ESP2 at startup and in every work order. So:

- A value pasted into ESP1's source loses to anything already in NVS, including an old LCD calibration.
- A value pasted into ESP2's `K_RES_MIX`, `K_NUT`, `PH_CAL_M`, `EC_CAL_M` or `ACS712_ZERO_V` **is overwritten and does nothing.** Earlier versions of these tools told you to do exactly that.

`calstore`'s `show` lists what is stored, `export` prints it as `set` lines for your records, and `clear <key>` goes back to the compiled default.

The LCD's Settings > Calibration menu is the other way in, and saves to the same place without any flashing. It covers soil, pH, EC, the mixer zero, tank-empty distance, flow K, offsets and NPK trim. It does not cover the INA226s, tank-full distance, pump rates, the mixer's sensitivity or the fallback sensors.

## The tools

| Tool | Board | Component | What you need | Result goes to |
|---|---|---|---|---|
| `ina226` | ESP1 | Solar, main battery and secondary INA226 | Clamp meter, multimeter | Its own `save`, straight to NVS |
| `calstore` | ESP1 | Stores everything else | The other tools' output | NVS |
| `opto` | ESP1 | Fallback battery voltage | Bench supply, multimeter | Its own `calc`, straight to NVS |
| `acs758` | ESP1 | Fallback battery current | Clamp meter, multimeter | `set acsDiv`, `acsZero`, `acsSens` |
| `flow` | ESP2 | 8 flow K-factors, **and pump rates** | Measuring jug or cylinder | `set kRM` `kMI` `kNA`..`kPD`, `pumpA`..`pumpC`. Mains pump rates go in ESP2's `TRANSFER_LPM`, `BOOSTER_LPM` |
| `pinfinder` | ESP2 | Which flow sensor is on which GPIO | Nothing | ESP2's `FLOW_*` defines |
| `ph` | ESP2 | pH probe | pH 7, pH 4 buffers | `set phM`, `phB` |
| `ec` | ESP2 | EC probe | A standard solution | `set ecM`, `ecB` |
| `acs712` | ESP2 | Mixer current | Clamp meter | `set acs`, and ESP2's `ACS712_SENS_V_PER_A` |
| `soil` | Nano | Soil dry and wet readings, per column | Water | `set s0a2` `s0w2` and so on |
| `ultrasonic` | Nano | Tank empty and full distances | Tape measure helps | `set reCm` `rfCm` `meCm` `mfCm` |
| `resflow` | Nano | Reservoir flow K | Measuring jug | `set frS` |
| `dht22` | Nano | Temperature, humidity offsets | Reference instrument | `set tOff`, `hOff` |
| `bh1750` | Nano | Light offset | Lux meter | `set lOff` |
| `npk` | Nano | NPK bus check, guarded offset | Lab result, if trimming at all | `set s0N` and so on |

## A good order

1. **`ina226`** first: solar, main and secondary. They are simple, and you then trust the battery readings for everything else.
2. **`pinfinder`** if the flow sensors have been touched, then **`flow`**, for every line: everything downstream is scaled by these. Water only. Pump rates come from the same runs.
3. **`soil`**, including column C when it is connected.
4. **`ph`** and **`ec`**, then **`ultrasonic`** and **`resflow`**.
5. **`acs712`**, then `dht22` and `bh1750` if you have reference instruments.
6. The fallbacks last, **`opto`** and **`acs758`**: they only matter if the main INA226 stops answering.

`Calibration_Worksheet.md` lists every constant and what goes wrong if it is wrong.

## Solar and battery INA226 modules

| Type | Address | Module | Where | Shunt | Full scale |
|---|---|---|---|---|---|
| `solar` | 0x41 | A1 GND, A0 VCC | Behind the isolator | R010 on the module | 8.19 A |
| `batt` | 0x44 | A1 VCC, A0 GND | Behind the isolator | 50 A / 75 mV bar | 54.6 A |
| `sec` | 0x40 | Both GND | Boards' side | R010 on the module | 8.19 A |

For each one: `zero <m>` with no current through the shunt, `amps <m> <A>` at a steady load, `volts <m> <V>` across VBS and GND, then `save <m>`. Flash the tool to **ESP1 itself**, because `save` writes to the board it runs on.

- **The isolator is an ISO1540**, a bidirectional I²C isolator. An ISO1450 is an RS-485 transceiver and will not pass I²C: if nothing answers behind it, read the marking on the chip.
- **`bustest`** counts failed reads. Run it with the inverter off, then on, to see if the bus through the isolator is really reliable.
- **Take `zero solar` at night and the rest in good sun.** Take `amps batt` at the highest steady load you can. It warns if the bar reads over 8 % low, which usually means the module's own R010 was left on.

## Tools that move things

`flow` and `acs712` run real pumps and motors, and `pinfinder` will if you arm a relay. **All three run only while you hold the ESP32's BOOT button.**

- Releasing BOOT stops everything. So does a hard time limit, or `off`. After a limit stop, release and press again.
- On an AC line, the inverter gets the same 5 s warmup as ESP2's stages before the pump starts. `flow` also stops an AC line after 10 s with no flow, so it cannot run dry.
- **P4 is never driven.** It used to be the mix valve; it is the battery-select DPDT now, and energizing it moves the boards to the secondary. The old `MIXIRR` and `PUSH` runs did exactly that.
- ESP2's PCF8575 is on the rail ESP1 switches with GPIO4. If a tool says it is not found, that rail is off.
- **Water only**, with the rig plumbed and a vessel in place.

For a **pump rate**, hold BOOT in one go for about a minute (90 s limit on dosing pumps, 45 s on AC lines). Stopping and restarting spoils the timing, though not the K.

## ⚠️ ACS758: the sensor can destroy the ADS1115

The ACS758 is a 5 V part whose output swings to about 5 V, and the ADS1115's maximum input is VDD + 0.3 V. Put a divider on the output (10k/10k, ratio 2.0) and power the ADS1115 from **3.3 V**. Tell the tool your real ratio with `div`.

Both its zero and its sensitivity follow its supply, so measure the rail and enter it with `vcc`. A rail of 4.85 V instead of 5.00 V reads as a phantom 1.9 A on a 40 mV/A part.

Order: `variant 50B`, `vcc 4.98`, `div 2.003`, load off: `zero`, known current: `sens 12.5`, then `show`.

## Keys CalStore accepts

`keys` in the tool lists all of these with their ranges. ESP1 ignores a value outside its range and uses its compiled default, printing a line on its serial monitor.

| Keys | Meaning |
|---|---|
| `s0a2` `s0w2` `s1a2` `s1w2` `s2a2` `s2w2` | Soil dry and wet, columns A, B, C |
| `reCm` `rfCm` `meCm` `mfCm` | Reservoir and mixing tank, empty and full distance |
| `tOff` `hOff` `lOff` | Temperature, humidity, light offsets |
| `s0N` `s0P` `s0K`, `s1...`, `s2...` | NPK offsets per column (default 0) |
| `frS` | Reservoir flow correction scale |
| `kRM` `kMI` `kNA` `kNB` `kNC` `kND` `kPU` `kPD` | Flow K-factors |
| `pumpA` `pumpB` `pumpC` | Nutrient pump rate, mL/min |
| `phM` `phB` `ecM` `ecB` `acs` | pH, EC, mixer current zero |
| `acsDiv` `acsZero` `acsSens` | ACS758 fallback |
| `iSolO/A/B/I` `iMainO/A/B/I` `iSecO/A/B/I` | INA226 shunt, offset, bus factor, invert. `ina226` writes these |

The table in `ESP1/CalStore/src/main.cpp` must stay in step with `loadCal()` and `loadPowerCal()` in `ESP1/src/main.cpp`.
