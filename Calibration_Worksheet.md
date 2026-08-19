# Calibration Worksheet

Every constant in the three firmwares still carrying a `[MEASURE]`, `[TBD]` or `[CONFIRM]` tag, with
what it physically is, how to pin it down, and what goes wrong if it is wrong. Ordered by **damage
if wrong**, not by where it sits in the file.

`Calibration/` already holds a standalone bench tool for most of the sensor constants — this
worksheet cites the tool rather than repeating its procedure. See `Calibration/README.md` for how to
build and run them, and `Settings_Calibration_and_Safe_Edit_Spec.md` §A for the methods.

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
| `PUMP_FLOWRATE_MLPM[3]` | `ESP1:348` | `{50, 50, 50}` guessed | Nutrient peristaltic pump delivery, mL per minute, per pump | ⏱️ Run each pump 60 s into a measuring cylinder, 3×, average. **No tool exists for this — build one or do it by hand** | Sets both each dose's run time *and* its timeout ceiling. Over- or under-fertilises silently, with no alarm, because the firmware has no independent measure of what the nutrient pumps actually moved |
| `K_RES_MIX`, `K_MIX_IRR` | `ESP2:148-149` | `450` generic | Main flow meters, pulses per litre | 🔧 `Calibration/ESP2/Flow` (drives the paired pump under a dead-man) | Wrong delivered volume everywhere. Far too high and a stage runs to its 180 s cap — on `SEQ_FILL` that is the mixing tank overflowing |
| `K_NUT[3]`, and the D / pH-up / pH-down meters | `ESP2:150` | `450` generic | Small dosing flow meters | 🔧 `Calibration/ESP2/Flow` | As above. Small meters differ most from the generic figure, and these are the ones metering chemicals |
| `FLOW_K_PULSES_PER_LITER` | `Nano:139` | `450.0` | Reservoir flow meter | 🔧 `Calibration/Nano/Flow` (3-run with outlier rejection) | Reservoir volume accounting drifts. **Must equal ESP1's `NANO_FLOW_K`** |
| `WATER_BUDGET_L[3]` | `ESP1:353` **and** `ESP2:161` | `{5, 5, 5}` | Litres delivered per column per service | 🌱 Decide from pot size and crop | **Duplicated in two firmwares. Change both.** If they disagree, ESP1 plans a batch ESP2 will not deliver, and the mismatch is silent |
| `calSoilAir[3]` / `calSoilWater[3]` | `ESP1:157-158` | A/B measured, C default | Capacitive probe raw ADC, bone dry and saturated | 🔧 `Calibration/Nano/Soil` | Drives every irrigation start/stop decision *and* the NPK divergence gate. Column C is still on placeholder values |
| `SOIL_BASELINE_N/P/K` | `ESP1:336-338` | Lab values present | Soil's existing nutrient content, mg/kg | 📄 F.A.S.T. Labs report MC2812-6783 — already obtained | The dose is the *gap* between baseline and target. A wrong baseline offsets every dose by a constant amount |
| Stock concentrations `STOCK_A/B/C_*` | `ESP1:339+` | Manufacturer figures | mg/L of each nutrient in the stock tanks | ⏱️ **Dilute at commissioning** and record the actual dilution | Doses are computed from these. Undiluted stock means every dose is far too strong |

## Tier 2 — timeouts and safety limits

Wrong here and the rig either stops when it should not, or fails to stop when it should.

| Constant | File:line | Now | What it is | How | If wrong |
|---|---|---|---|---|---|
| `TRANSFER_LPM` | `ESP2:176` | `8.0` | Reservoir→mix transfer pump, L/min | ⏱️ Time a known volume | Sets the flow-blind stage timeout. Too low and stages abort mid-fill; too high and a dead pump is not caught until the 180 s hard cap |
| `BOOSTER_LPM` | `ESP2:177` | `6.0` | Mix→column booster pump, L/min | ⏱️ Time a known volume | As above, on delivery |
| `MIX_TARGET_PCT` / `MIX_MAX_PCT` | `ESP1:194-195` | `70` / `95` | Mixing tank fill target and overflow guard | 🌱 Decide from tank geometry, then verify against the ultrasonic reading | `MIX_MAX_PCT` is the last guard before an overflow |
| `RES_LOW_PCT` | `ESP1:193` | `15` | Reservoir floor that blocks operations | 🌱 Decide — set above the pump intake | Too low and the transfer pump runs dry |
| `calResEmptyCm` / `calResFullCm` | `ESP1:164` | `38` / `11` measured | Ultrasonic geometry, reservoir | 🔧 `Calibration/Nano/Ultrasonic` | Every tank percentage. Note `loadCal()` overrides these from NVS — editing the constant alone does not take |
| `calMixEmptyCm` / `calMixFullCm` | `ESP1:165` | `50` / `4` | Ultrasonic geometry, mixing tank | 🔧 `Calibration/Nano/Ultrasonic` | As above |
| `MIXING_DURATION_MS` | `ESP1:136` | `30000` | Homogenise time after dosing | ⏱️ Observe when EC/pH readings settle | Too short and EC/pH are measured on an unmixed batch |
| `PRIME_CAP_MS` | `ESP2:215` | `45000` | Hard cap for purging air from a line | ⏱️ Time an actual prime | Too short and priming never completes |
| `BATT_LOW_V` / `BATT_CRIT_V` | `ESP1:198-199` | `11.8` / `11.2` | Fertigation cutoff and full stop | 📄 12 V lead-acid resting curve, then ⏱️ verify with a multimeter against the ADC reading | Too low and the battery is damaged; too high and the rig stops needlessly |

## Tier 3 — sensor scaling

These make readings wrong, which mostly shows up as bad decisions rather than bad actuation.

| Constant | File:line | Now | How | Notes |
|---|---|---|---|---|
| `PH_CAL_M` / `PH_CAL_B` | `ESP2:197` | Nominal | 🔧 `Calibration/ESP2/pH` | Two-point fit against buffer solutions |
| `EC_CAL_M` / `EC_CAL_B` | `ESP2:198` | Nominal | 🔧 `Calibration/ESP2/EC` | Uses `EC_STD_MSCM` below |
| `EC_STD_MSCM` | `ESP1:1254` | `1.413` | 📄 Printed on the standard solution bottle | Confirm it matches the bottle you actually have |
| `ACS712_SENS_V_PER_A` | `ESP2:189` | `0.100` | 📄 Module rating: 20 A ≈ 100 mV/A, 30 A ≈ 66 mV/A | **Check which module is fitted** — the wrong one is a 50 % current error |
| `ACS_DIV`, `ACS_ZERO_V`, `ACS_SENS_V_PER_A` | `ESP1:216-218` | `2.0`, `2.5`, `0.040` | 🔧 `Calibration/ESP1/ACS758` | `ACS_DIV` is the physical divider in front of A0 — measure the two resistors |
| `INA226_SHUNT_OHMS` | `ESP1:200` | `0.002` | 📄 Printed on the shunt | Not a measurement — read the part |
| `INA226_MAX_CURRENT_A` | `ESP1:201` | `10.0` | 🌱 Decide from expected load | Only sets the LSB resolution |
| `calNpkScale[7]` | `ESP1:167` | `{10,10,1,10,1,1,1}` | 🔧 `Calibration/Nano/NPK` | Raw register → engineering units, per channel. Now that all 7 channels are used, **all 7 need confirming**, not just N/P/K |
| `calNpkOff[3][3]` | `ESP1:168` | all `0` | 🔧 `Calibration/Nano/NPK` | Per-column N/P/K offset trim against the lab result |
| `calTempOff`, `calHumOff`, `calLuxOff` | `ESP1:169` | `0` | 🔧 `Calibration/Nano/DHT22`, `Calibration/Nano/BH1750` | Offset trims only |
| `CAL_OUTLIER_PCT` | `ESP1:1253` | `8.0` | 🌱 Decide | How far three calibration runs may disagree before the tool rejects the set |

## Tier 4 — agronomic and operational choices

Nothing to measure. These are decisions to make deliberately and record in the thesis.

| Constant | File:line | Now | Notes |
|---|---|---|---|
| `soilStartPct` / `soilStopPct` | `ESP1:111-112` | `35` / `45` | Irrigation hysteresis band. **`NPK_MIN_MOIST_PCT` must stay below `soilStartPct`** — the host test suite asserts this |
| `fertGap` | `ESP1:134` | `30.0` | mg/kg below target before fertigating. Directly sets how often fertigation runs |
| `NPK_MOIST_AGREE_PCT` | `ESP1/include/pure_math.h` | `15` | How far the NPK probe may sit from the capacitive pair before exclusion. Tighten once you know the probe's real spread |
| `NPK_MIN_MOIST_PCT` | `ESP1/include/pure_math.h` | `15` | Below this the probe's N/P/K registers are not trustworthy |
| `DEF_WIN_START` / `DEF_WIN_END` | `ESP1:123-124` | 06:00 / 08:00 | Default service windows, overridable per column from the web |
| `EC_MIN`/`EC_MAX`, `PH_MIN`/`PH_MAX` | `ESP1:139-140` | `0–3`, `5–7` | Safe windows for the mixed batch |
| `exerciseEnabled` / `exerciseRunS` | `ESP1:291-292` | `true` / `5 s` | Preventive pump exercise. Now settable from LCD and web |
| `FORCE_MAX_LITERS` / `FORCE_MAX_DOSE_ML` | `ESP1:657-658` | `20 L` / `500 mL` | Caps on a single forced run. Deliberate safety limits |
| `DAILY_REPORT_MIN` | `ESP1:317` | `18:00` | When the daily SMS summary goes out |
| `PHONE_NUMBER` | `ESP1:97` | `09150424784` | 🌱 Confirm the recipient before commissioning |
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
| `WATER_BUDGET_L` | `ESP1:353`, `ESP2:161` | ESP1 plans a batch ESP2 will not deliver |
| `COLUMN_ENABLED` | `ESP1:104`, `ESP2:162`, `Nano:74` | A column that one controller thinks exists and another does not |
| Reservoir flow K | `Nano:139` (`FLOW_K_PULSES_PER_LITER`), `ESP1` (`NANO_FLOW_K`) | Reservoir volume accounting disagrees between the two |

## Suggested order

1. **Tier 1 flow K-factors and pump rates.** Everything downstream is scaled by them, and the
   nutrient pump rates have no tool — budget bench time for the jug-and-stopwatch work.
2. **Soil endpoints**, including column C, which is still on defaults.
3. **pH / EC fits**, then the tank ultrasonic geometry.
4. **Currents and battery**, which only gate operations rather than dose.
5. **Tier 4 decisions**, recorded with reasoning — this is thesis methodology material, not tuning.

Dilute the stock solutions and record the real concentrations before any dosing test with anything
other than water. See `Commissioning_Flash_Plan.md` — stage G is the first stage that uses stock.
