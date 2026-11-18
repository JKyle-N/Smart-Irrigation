# Calibration Worksheet

Every constant in the three firmwares still carrying a `[MEASURE]`, `[TBD]` or `[CONFIRM]` tag, with
what it physically is, how to pin it down, and what goes wrong if it is wrong. Ordered by **damage
if wrong**, not by where it sits in the file.

`Calibration/` holds a tool for most of these constants, and this worksheet cites the tool rather than
repeating its procedure. See `Calibration/README.md` for how to run them, and
`Settings_Calibration_and_Safe_Edit_Spec.md` §A for the methods.

> **Before using an ESP2 tool (found 19 Nov 2026):** `Calibration/ESP2/Flow`, `FlowPinFinder` and `ACS712` still
> carry ESP2's relay map from before the 15 Nov rewiring. They treat P4 as the battery switch (it is on P0 now, and P4
> is column B's valve), and their valve, pump and inverter bits point at the old relays. On today's wiring they would
> switch the wrong relays, so bring them in line with ESP2's `OUT_*` map first. The LCD's own calibration screens
> (Settings → Calibration) are current.

**Where a result goes matters.** ESP1 loads its NVS namespace `calib` over its own source constants, and
pushes flow K, pH, EC and the mixer zero to ESP2 at startup and in every work order. So store results through
`Calibration/ESP1/CalStore` (or the LCD's Calibration menu), never by editing ESP2's `K_RES_MIX`, `K_NUT`,
`PH_CAL_*`, `EC_CAL_*` or `ACS712_ZERO_V`, which are overwritten. The "Now" column below is the compiled default.

**A third of the entries below are not measurements at all.** They are agronomic decisions, datasheet
lookups, or design limits. Those are marked so you do not go looking for a tool that does not exist.

| Kind | Meaning |
|---|---|
| 🔧 **Tool** | A bench sketch in `Calibration/` prints a paste-ready value |
| ⏱️ **Bench** | Measure by hand — stopwatch, jug, scale, multimeter. No tool exists |
| 📄 **Datasheet** | Look it up; do not measure |
| 🌱 **Decide** | An agronomic or design choice, not a property of the hardware |

---

## Tier 1 — the dosing maths multiplies by these

Wrong values here do not throw an error. They produce confident, precise, wrong doses.

| Constant | File:line | Now | What it is | How | If wrong |
|---|---|---|---|---|---|
| `PUMP_FLOWRATE_MLPM[3]` | `ESP1`, CalStore `pumpA/B/C` | `{50, 50, 50}` guessed | Nutrient peristaltic pump delivery, mL per minute, per pump | 🔧 `Calibration/ESP2/Flow`: hold BOOT about 60 s into a measuring cylinder, 3×, `calc` | Sets both each dose's run time *and* its timeout ceiling. Over- or under-fertilises silently, with no alarm, because the firmware has no independent measure of what the nutrient pumps actually moved |
| Transfer and booster **pump lines** (litres = start + rate × seconds from the inverter switch-on) | `shared/pump_line.h` (`PL_XFER_READINGS`, `PL_BOOST_READINGS`); ESP1 NVS `rXfer`/`xStartL`, `rBoost`/`bStartL`; ESP2 NVS `xS` | Transfer **11.74 L/min, +0.47 L**; booster **13.20 L/min, −0.65 L** (fitted at boot from the user's 10 + 10 weighed runs of 17 Nov, 1 L = 1028 g) | How much water a run moves | ⏱️ LCD Settings → Calibration → **Transfer pump est** / **Booster pump est**: run 3–60 s, weigh, type the grams; at least 5 tries, vary the seconds | Every fill's run time is (need − start) ÷ rate: a rate 10 % high gives about 10 % less water, and the top-up only makes up a shortfall of more than 3 L. Full readings and fits: reference §5.3.2 |
| ~~`calKResMix`, `calKMixIrr`~~ | `ESP1`, CalStore `kRM2`, `kMI2` | — | Main flow meters (ResMix, MixIrr) | **Not used since web v.4 (16 Nov 2026):** the tank ultrasonics measure the water. ESP2 accepts and ignores `SET_CAL,FLOW_RESMIX/MIXIRR` | — |
| `calKNut[3]`, and the D / pH-up / pH-down meters | `ESP1`, CalStore `kNA`..`kNC`, `kND`, `kPU`, `kPD` (pushed to ESP2) | `450` generic | Small dosing flow meters | 🔧 `Calibration/ESP2/Flow` | As above. Small meters differ most from the generic figure, and these are the ones metering chemicals |
| `FLOW_K_PULSES_PER_LITER` | `Nano:139` | `450.0` | Reservoir flow meter | 🔧 `Calibration/Nano/Flow` (3-run with outlier rejection) | Reservoir volume accounting drifts. Easiest: CalStore `set frS` corrects it with no Nano reflash. Otherwise it **must equal ESP1's `NANO_FLOW_K`** |
| `WATER_BUDGET_L[3]` | `shared/rig_constants.h` (both ESP32s) | `{5, 5, 5}` | Litres delivered per column per service | 🌱 Decide from pot size and crop | One definition since web v.4 |
| `calSoilAir[3]` / `calSoilWater[3]` | `ESP1:157-158` | A/B measured, C default | Capacitive probe raw ADC, bone dry and saturated | 🔧 `Calibration/Nano/Soil` | Drives every irrigation start/stop decision *and* the NPK divergence gate. Column C is still on placeholder values |
| `SOIL_BASELINE_N/P/K` | `ESP1:336-338` | Lab values present | Soil's existing nutrient content, mg/kg | 📄 F.A.S.T. Labs report MC2812-6783 — already obtained | The dose is the *gap* between baseline and target. A wrong baseline offsets every dose by a constant amount |
| Stock concentrations `STOCK_A/B/C_*` | `ESP1:339+` | Manufacturer figures | mg/L of each nutrient in the stock tanks | ⏱️ **Dilute at commissioning** and record the actual dilution | Doses are computed from these. Undiluted stock means every dose is far too strong |

## Tier 2 — timeouts and safety limits

Wrong here and the rig either stops when it should not, or fails to stop when it should.

| Constant | File:line | Now | What it is | How | If wrong |
|---|---|---|---|---|---|
| `TRANSFER_LPM`, `XFER_START_L`, `XFER_MIN_RUN_S` | `ESP2` | `11.74` L/min, `0.47` L, `6` s | The transfer pump's default line (a fill is TIMED from it, web v.4) | ⏱️ the Transfer pump est screen (above), or LCD **Rate Transfer** | The fill gives the wrong litres; see the pump lines row |
| `BOOSTER_LPM` / `BOOSTER_OPEN_LPM` | `ESP2` | `1.0` (guess, with the drip pipe) / `13.2` (open valve) | The booster's rate: sizes how long a delivery may take (stage cap, no-flow and stall windows) / caps how fast a drain is believable | ⏱️ LCD **Rate Booster**: type the `seen:` value after a real delivery through the drip pipe | Too low: a slow failure is noticed late. Too high: a healthy delivery is cut short as a stall |
| `MIX_HIGH_PCT` (shared) / ESP1 backstop | `shared/rig_constants.h`; `ESP1` | `90` % / `97` % | The fill's overflow stop (ESP2) and ESP1's own last-line stop (web v.4; `MIX_TARGET_PCT` 70 / `MIX_MAX_PCT` 95 in ESP1 are older planning values) | 🌱 Decide from tank geometry, then verify against the ultrasonic reading | Too high and a ripple overfills; too low and big batches are capped (the planning ceiling `MIXCAP` is the litres at 90 %: 17.32 L with the shape taped on 18 Nov, 15.90 L before) |
| `RES_LOW_PCT` = `RES_FILTER_PCT` | `ESP1`, `shared/rig_constants.h` | `18` | Reservoir floor that blocks operations: the reservoir's filter zone, where its ultrasonic stops seeing water | 🌱 Decide — set above the pump intake | Too low and the transfer pump runs dry |
| `calResEmptyCm` / `calResFullCm` | `ESP1` (NVS) | `42.9` / `24.0` cm, measured 15 Nov | Ultrasonic geometry, reservoir (sensor to water) | 🔧 LCD **Ultra Res empty**, then **Tank Res liters** (up to 8 level → litres points) | Every tank percentage and every litre the reservoir reports |
| `calMixEmptyCm` / `calMixFullCm` | `ESP1` (NVS) | `42.9` / `24.0` cm, measured 15 Nov | Ultrasonic geometry, mixing tank | 🔧 LCD **Ultra Mix empty**, then **Tank Mix liters** | Every fill check and every delivery is measured on this tank (web v.4) |
| `TANK_SENSOR_H_CM` / `TANK_D_FLOOR_CM` / `TANK_TAPER_CM`; mixer insert | `ESP1`; `shared/tank_volume.h` | `47.5` cm / `34.60` cm / `0.1219` cm per cm, taped 18 Nov (replaced the guessed 31.0 / 39.4 cm cone); mixer Ø 6 × 11.4 cm | The tank shape between EMPTY and FULL, and the mixer's volume taken out of it | ✅ taped; check with the 2-minute ruler test (LCD distance + water depth = 47.5 cm); the Tank liters points override the shape | Litres wrong by the shape error, and since the two-step fill the delivered litres too |
| `FILL_FIRST_FRAC` / `FILL_FIRST_CUT_FRAC` | `ESP2` | `0.70` / `0.75` | The two-step fill (18 Nov): the first run aims for 70 % of the need and is cut once the live level has risen 75 % | 🌱 Decide (simulated 18 Nov for 8–22 L/min pumps: every fill within about 1 L) | Higher: a fast pump overfills, which cannot be taken back. Lower: more top-ups, more time |
| `FILL_SETTLE_MS` / `FILL_STEADY_L` / `FILL_TOPUP_MIN_L` / `FILL_TOPUP_MAX` / `FILL_SHORT_ALERT_L` | `ESP2` | `60` s / `0.3` L / `0.8` L (3 L before 18 Nov) / `2` / `3` L | Settle after a timed run, steadiness band, shortfall that earns a top-up, top-ups per fill, shortfall still alerted as `FILL_SHORT` | 🌱 Decide; confirm the settle on the rig | A short settle measures ripples. Under 0.8 L a top-up would overshoot further than it was short: the shortest run (6 s) moves 1.6–2.3 L |
| `FILL_CUT_LAG_L` / `FILL_CUT_SAMPLES` / `FILL_LEARN_MIN_S` | `ESP2` | `0.8` L / `3` / `10` s | A top-up stops this far below the goal (the water still running in after the stop); readings in a row needed to stop early; the shortest run whose real rate is used | ⏱️ compare a run's `FILL_CUT level=` with the next `FILL_CHECK measured=` | Too small a lag overfills; too large leaves a top-up short |
| `MIX_LOW_STOP_PCT` / `MIX_LOW_STOP_MS` | `ESP2` | `5` % / `10` s | A delivery ends this long after the mixing tank is under this level (user rule, 17 Nov) | 🌱 Decide | Too short leaves water behind; too long runs the booster dry |
| `ULTRA_MAX_SPREAD_CM` | `ESP1` | `4` cm (2 before 17 Nov) | How far the Nano's pings may disagree for a reading to be used for control | ⏱️ watch `NANO,LEVEL,NOT_USABLE` lines and the LCD `?` during runs | Too tight: readings dropped while the transfer pours in. Too loose: a mixer echo passes as a level |
| `MIXING_DURATION_MS` | `ESP1:136` | `30000` | Homogenise time after dosing | ⏱️ Observe when EC/pH readings settle | Too short and EC/pH are measured on an unmixed batch |
| `PRIME_CAP_MS` | `ESP2:215` | `45000` | Hard cap for purging air from a line | ⏱️ Time an actual prime | Too short and priming never completes |
| `BATT_LOW_V` / `BATT_CRIT_V` | `ESP1:198-199` | `11.8` / `11.2` | Fertigation cutoff and full stop | 📄 12 V lead-acid resting curve, then ⏱️ verify with a multimeter against the reading. **The INA226 also reads the sag during inverter runs.** A flag sets only after `BATT_FLAG_HOLD_MS` (60 s) below the line, which covers a start's dip but not a long run's sag, so measure that before enabling `BATTERY_SAFETY_ENABLED`. **Not usable on this rig yet:** the main-battery sensor is not fitted (reference §20.5), so nothing reads the main battery | Too low and the battery is damaged; too high and the rig stops needlessly |
| `SEC_BLOCK_PCT` / `SEC_UNBLOCK_PCT` / `SEC_BATT_CRIT_V`; `SEC_RINT_DEFAULT` | `ESP1` (secondary battery block) | `30` % / `35` % of the **resting** charge (≈ 14.0 / 14.2 V) / `12.8` V; `0.25` Ω learned into NVS `secR` | AC pumps are blocked at or below 30 % resting charge (resting V = V + I × R, so charging does not read high) (user, 17 Nov) | 📄 4S Li-ion curve; ⏱️ check the % against a rested pack | Too high blocks runs on a healthy pack; too low lets the inverter start on a weak one |
| `SEC_BATT_FULL_V` / `SEC_BATT_EMPTY_V` | `ESP1` | `16.8` / `12.8` | Secondary battery % scale (display only) | 📄 4.20 / 3.20 V per cell | Wrong % on the dashboard only |
| `BATT_SWITCH_SETTLE_MS` / `BATT_RETURN_DELAY_MS` | `ESP2` | `500` / `1000` | Settle after the DPDT moves the boards to the secondary before the inverter starts; wait after the inverter stops before returning to main | ⏱️ scope the board rail through a switch once the hold-up stage is fitted | Too short and the inverter starts (or the boards return) while the transfer is still settling |
| `BSW_RECHARGE_MS` | `ESP1` | `60000` | How long inverter use waits after a boot or a switch reset, so the hold-up can recharge | ⏱️ scope the hold-up voltage from a cold boot until it is full, then add a margin | Too short and the retry fails too, using up the second strike; too long only delays the re-run |
| `BACKUP_MAIN_RECOVER_V` | `ESP1` | `12.6` | Main voltage (held for `BACKUP_RECOVER_MS`, 5 min) at which the boards leave the backup. Entry uses `BATT_CRIT_V` | 📄 your main battery's resting curve; must sit well above `BATT_CRIT_V` | Too low and the boards return to a battery that drops again; too high and the pack runs down waiting |
| `INVERTER_WARMUP_MS` | `ESP2` | `5000` | Inverter settle before an AC pump starts (plus the settle above when switching is on) | ⏱️ watch the inverter output come up | Too short and pumps start on a ramping supply (PWR_FAIL) |

## Tier 3 — sensor scaling

These make readings wrong, which mostly shows up as bad decisions rather than bad actuation.

| Constant | File:line | Now | How | Notes |
|---|---|---|---|---|
| `calPhM` / `calPhB` | `ESP1`, CalStore `phM`, `phB` (pushed to ESP2) | Nominal | 🔧 `Calibration/ESP2/pH` | Two-point fit against buffer solutions |
| `SEC_INA_CAL` / `SEC_CURRENT_OFFSET_A` / `SEC_BUS_V_FACTOR` | `ESP1` (secondary battery), NVS `iSec*` | `2048` / `0.014` / `1.000` | 🔧 `Calibration/ESP1/INA226` | `zero sec`, `amps sec <A>`, `volts sec <V>`, `save sec`. `invert sec` if discharge reads negative |
| `SOLAR_SHUNT_OHM` / `SOLAR_OFFSET_A` / `SOLAR_BUS_FACTOR` | `ESP1` (solar INA226, behind the ISO1540), NVS `iSol*` | `0.010` / `0` / `1.000` | 🔧 `Calibration/ESP1/INA226` | **Skip: not fitted on this rig (reference §20.5).** If one is fitted: `zero solar` at night, `amps solar <A>` with a clamp meter in sun (at least 0.13 A, more is better), `volts solar <V>` by day, `save solar`. `invert solar` if charging reads negative |
| `MAIN_SHUNT_OHM` / `MAIN_OFFSET_A` / `MAIN_BUS_FACTOR` | `ESP1` (main INA226, behind the ISO1540), NVS `iMain*` | `0.0015` / `0` / `1.000` | 🔧 `Calibration/ESP1/INA226` | **Skip: not fitted on this rig (reference §20.5).** If one is fitted: `zero batt` with no current through the bar, `amps batt <A>` at a steady known load (at least 1 A, more is better), `volts batt <V>`, `save batt`. Confirm the module's own R010 is removed; the tool warns if it reads over 8 % low. `invert batt` if discharge reads negative |
| `calBattV` | `ESP1`, NVS `adccal` | Placeholder | 🔧 `Calibration/ESP1/BatteryOpto` | The main battery's fallback voltage, used only while the main INA226 is not answering. On this rig the opto input reads about 0 V (18 Nov log), so it is not connected either |
| `calEcM` / `calEcB` | `ESP1`, CalStore `ecM`, `ecB` (pushed to ESP2) | Nominal | 🔧 `Calibration/ESP2/EC` | Uses `EC_STD_MSCM` below |
| `EC_STD_MSCM` | `ESP1:1254` | `1.413` | 📄 Printed on the standard solution bottle | Confirm it matches the bottle you actually have |
| `ACS712_SENS_V_PER_A` | `ESP2` | `0.100` | 🔧 `Calibration/ESP2/ACS712` | Module rating: 20 A ≈ 100 mV/A, 30 A ≈ 66 mV/A. **Check which module is fitted** — the wrong one is a 50 % current error |
| `calAcs712Zero` | `ESP1`, CalStore `acs` (pushed to ESP2) | `1.65` | 🔧 `Calibration/ESP2/ACS712` | Mixer off |
| `ACS_DIV`, `ACS_ZERO_V`, `ACS_SENS_V_PER_A` | `ESP1`, CalStore `acsDiv`, `acsZero`, `acsSens` | `2.0`, `2.5`, `0.040` | 🔧 `Calibration/ESP1/ACS758` | `ACS_DIV` is the physical divider in front of A0 — measure the two resistors. Fallback sensor |
| `calNpkScale[7]` | `ESP1:167` | `{10,10,1,10,1,1,1}` | 🔧 `Calibration/Nano/NPK` | Raw register → engineering units, per channel. Now that all 7 channels are used, **all 7 need confirming**, not just N/P/K |
| `calNpkOff[3][3]` | `ESP1`, CalStore `s0N` and so on | all `0` | 🔧 `Calibration/Nano/NPK` | Per-column N/P/K offset trim against the lab result. Guarded: it biases the thesis data |
| `calTempOff`, `calHumOff`, `calLuxOff` | `ESP1:169` | `0` | 🔧 `Calibration/Nano/DHT22`, `Calibration/Nano/BH1750` | Offset trims only |
| `CAL_OUTLIER_PCT` | `ESP1:1253` | `8.0` | 🌱 Decide | How far three calibration runs may disagree before the tool rejects the set |

## Tier 4 — agronomic and operational choices

Nothing to measure. These are decisions to make deliberately and record in the thesis.

| Constant | File:line | Now | Notes |
|---|---|---|---|
| `soilStartPct` | `ESP1` | `35` | Irrigation starts below this soil moisture (the stop % was removed; a run gives the column's water budget). **`NPK_MIN_MOIST_PCT` must stay below `soilStartPct`** — the host test suite asserts this |
| `fertGap` | `ESP1:134` | `30.0` | mg/kg below target before fertigating. Directly sets how often fertigation runs |
| `NPK_MOIST_AGREE_PCT` | `ESP1/include/pure_math.h` | `15` | How far the NPK probe may sit from the capacitive pair before exclusion. Tighten once you know the probe's real spread |
| `NPK_MIN_MOIST_PCT` | `ESP1/include/pure_math.h` | `15` | Below this the probe's N/P/K registers are not trustworthy |
| `DEF_WIN_START` / `DEF_WIN_END` | `ESP1:123-124` | 06:00 / 08:00 | Default service windows, overridable per column from the web |
| `EC_MIN`/`EC_MAX`, `PH_MIN`/`PH_MAX` | `ESP1:139-140` | `0–3`, `5–7` | Safe windows for the mixed batch |
| `exerciseEnabled` / `exerciseRunS` | `ESP1:291-292` | `true` / `5 s` | Preventive pump exercise. Now settable from LCD and web |
| `FORCE_MAX_LITERS` / `FORCE_MAX_DOSE_ML` | `ESP1:657-658` | `20 L` / `500 mL` | Caps on a single forced run. Deliberate safety limits |
| `DAILY_REPORT_MIN` | `ESP1:317` | `18:00` | When the daily SMS summary goes out |
| `SECRET_OWNER_NUMBER` | `ESP1/include/secrets.h` (git-ignored; copy `secrets.example.h`) | — | 🌱 Set the owner phone before flashing, or on the Setup AP admin page. Never commit it |
| `COLUMN_ENABLED[3]` | `ESP1:104`, `ESP2:162`, `Nano:74` | `{true, true, false}` | **Triplicated across all three firmwares — change all three.** C is currently disabled |

## Tier 5 — protocol constants, confirm once

Proven on the bench; confirm they still match the hardware fitted, then stop thinking about them.

| Constant | File:line | Now | Notes |
|---|---|---|---|
| `NPK_BAUD` | `Nano:155` | `4800` | Proven working |
| `NPK_FUNCTION` | `Nano:156` | `0x03` | Read-holding. Some probes want `0x04` |
| `NPK_REG_START` / `NPK_REG_COUNT` | `Nano:157-158` | `0x0000` / `7` | Moisture, temp, EC, pH, N, P, K |
| `FLOW_INTERRUPT_EDGE` | `Nano:141` | `RISING` | Proven test value |
| `ACS_SENS_V_PER_A` | `ESP1:218` | `0.040` | 40 mV/A = ACS758LCB-050B. Confirm the part number |

---

## Cross-firmware values that must agree

These exist in more than one place, and nothing checks them at runtime. Getting them out of step is
silent, so check them as a set whenever you change one.

| Value | Lives in | Consequence of drift |
|---|---|---|
| ~~`WATER_BUDGET_L`~~ | one definition in `shared/rig_constants.h` since web v.4 | No longer able to drift |
| `COLUMN_ENABLED` | `ESP1:104`, `ESP2:162`, `Nano:74` | A column that one controller thinks exists and another does not |
| Reservoir flow K | `Nano:139` (`FLOW_K_PULSES_PER_LITER`), `ESP1` (`NANO_FLOW_K`) | Reservoir volume accounting disagrees between the two |
| `SECONDARY_BATT_ENABLED` | `ESP1`, `ESP2` | ESP2 switches the boards but ESP1 neither blocks a low secondary nor waits for the return (or the reverse: ESP1 blocks runs on a monitor ESP2 never uses) |
| `SECONDARY_BACKUP_ENABLED` | `ESP1`, `ESP2` | ESP1 alerts and asks for the backup but ESP2 answers `INVALID` and the boards never move, so it reports `BACKUP_SWITCH_FAILED` every minute |

## Suggested order

1. **The transfer and booster pump lines** (LCD pump est screens), then the **tank litres** (Tank Mix / Res liters):
   since web v.4 they are the water measurement. Then the dosing flow K-factors and nutrient pump rates
   (`Calibration/ESP2/Flow` measures both from the same runs, with a measuring jug).
2. **Soil endpoints**, including column C, which is still on defaults.
3. **pH / EC fits**, then the tank ultrasonic geometry.
4. **Currents and battery**, which only gate operations rather than dose.
5. **Tier 4 decisions**, recorded with reasoning — this is thesis methodology material, not tuning.

Dilute the stock solutions and record the real concentrations before any dosing test with anything
other than water. See `Commissioning_Flash_Plan.md` — stage G is the first stage that uses stock.
