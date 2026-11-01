# Test Code

Older bench sketches and diagnostics. **To calibrate anything, use `../Calibration/`.** Everything below was compile-checked on 2026-11-02 against the pinned toolchain and read against the current firmware's pins; none of it has been run on hardware in this check.

## Still useful

| Program | Board | Use it for | Note |
|---|---|---|---|
| `PlatformIO/Projects/I2CScanner` | ESP32 | Listing an I²C bus, every 5 s | Safe on any board with SDA 21, SCL 22 |
| `PlatformIO/Projects/BH1750` | ESP32 | One I²C scan at boot | Misnamed: it is a scanner, not a light test |
| `ESP2_PCF8575_SEQ` | ESP2 | Walking the relays to prove the wiring | Now skips P4, the battery DPDT. Disconnect AC loads first |
| `PlatformIO/Projects/A7670C-SMS-ESP32` | ESP32 | Proving the A7670C modem alone | Uses GPIO16/17, which on ESP1 is the Nano link. ESP1's modem is on 26/27 |
| `A7670C_SMS_ESP32.ino` | ESP32 | The same, for the Arduino IDE | |
| `Arduino R3/A7670C_SMS_Test` | Uno | The same, on an Uno | |
| `PlatformIO/Projects/Nano-Debug` | Nano | A fake Nano, to bench-test ESP1 with no sensors | Check its packets against `Nano/Nano.ino` before trusting a result: it predates raw packets |

## Replaced by a tool in `Calibration/`

These still build and do no harm. Their results are not wrong, but the new tool is easier and goes straight into ESP1.

| Program | Use instead | Why |
|---|---|---|
| `PlatformIO/Projects/INA226-Dual` | `ina226` | Also covers the secondary, and saves. Keep this one for an Uno bench with no ESP32 |
| `Arduino R3/INA226_R010` | `ina226` (`sec`) | This one is calibrated by editing constants, and takes the first INA226 it finds |
| `ESP1_ADC_OPTO_CAL` | `opto` | Its GPIO34 current channel is dead: the firmware no longer reads it |
| `ESP1_ADS1115_VI` | `acs758` | Still handy to prove the ADS1115 answers. `acs758` adds `vcc`, `variant` and CalStore lines |

## Do not flash these to the rig

They do not match the current hardware. Kept as history.

| Program | Problem |
|---|---|
| `PlatformIO/Projects/SEQUENTIALESP2` | Walks P0 to P7, which includes **P4, the battery DPDT**, and takes commands from an old protocol |
| `PlatformIO/Projects/SEQUENTIAL` | The ESP1 half of that old protocol, on a different UART2 to the current wiring |
| `PlatformIO/Projects/ESP2-Debug` | Old flow pins (`FLOW_RES_MIX` 4, where the rig has 26) and the old relay map |
| `PlatformIO/Projects/ESP1-Debug` | SIM800L-era firmware with the old main-battery INA226 |
| `PlatformIO/Projects/GSM WITH LCD` | SIM800L, replaced by the A7670C |
| `PlatformIO/Projects/Backup/*` | March 2026 snapshots |
| `ArduinoNano/*.ino` | Early single-sensor sketches. Most share the rig's pins, but `NPK` uses D2/D3 where the rig has D9/D10, and `ANALOGSENSORS` reads A4/A5 where the rig has A6/A7 and the I²C bus. Print-only, so no result to carry over |
| `ArduinoNano/DEBUG_I2C_RTC_INA_LCD` | Assumes a 2 mΩ shunt. No current module uses that |
| `ArduinoNano/*.txt` | Notes copied from the sketches above. `Flowsensor.txt` is empty |
