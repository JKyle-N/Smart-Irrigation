# Nutrient Dosing Mechanism — Firmware Spec

**Companion to** `Smart_Irrigation_System_Reference_FINAL.md` and `Nutrient_Dosing_Complete_Guide.md`.
Turns the dosing *math* (the guide) into a *firmware mechanism* that fits the locked architecture: ESP32 #1 calculates, the work order carries mL targets, ESP32 #2 executes by flow-meter volume with a timed safety ceiling.

Status legend: **LOCKED** = decided, **PLACEHOLDER** = set at commissioning, **DERIVED** = computed from another value.

---

## 1. Authority & Data Flow (fits locked supervisor model)

Dosing spans both ESP32s along existing authority lines (§9.8.1.1, §10.1):

- **ESP32 #1 (decides):** at the fertigation decision point (§14.2.0) it computes the per-nutrient mL doses from the gap formula (§3), then composes the work order containing each pump's **target mL** plus its **timed-ceiling seconds** (§4.2). It does NOT micro-step the pumps.
- **ESP32 #2 (executes):** runs each nutrient pump, counts flow pulses, stops at target volume, and enforces the timed ceiling locally. Reports `DONE` / `DEGRADED` / fault per §9.7.3.
- **Open-loop:** the NPK sensor is the recorded OUTCOME, never a mid-dose control input (§12.4). Dose amount is fixed before the pump starts.

```
ESP32 #1: determine column water budget → batch_volume_L (this run)
          measure gap → calc mL per A/B/C (using batch_volume_L) → build work order (mL + ceiling_s)
                                   │ UART work order (§9.8.1.1)
                                   ▼
ESP32 #2: for each active nutrient → run pump
             stop when (flow volume == target mL)   [normal]
             OR        (runtime  >  ceiling_s)       [DOSE_TIMEOUT fault]
```

### 1.1. Batch Volume is Variable (LOCKED)

The mixing-tank batch volume is **not fixed at 50L** — it varies per run, set by the fertigating column's water budget. This is structural for the dose math, because `dose_mL = gap × batch_volume_L ÷ stock_conc` scales linearly with volume.

Consequences (LOCKED):
- ESP32 #1 computes `batch_volume_L` for the run **first** (from the column water budget, §14.2.0.2), **then** calculates doses against that exact volume. Water and dose are coupled — dose cannot be precomputed against a constant.
- The volume used in the formula is the **actual batched volume**, which ESP32 #2 meters from its own fill flow sensor (reservoir→mix) and persists (§19.4.8.1). Dose calc and fault-hold reference the **same** metered number, not an assumption.
- `MIXING_TANK_MAX_VOLUME = 50.0` (§6) is therefore a **safety ceiling**, NOT the formula input. `MIXING_TANK_SAFE_MIN` is the lower bound below which dosing is refused.
- **Accuracy lever (see §3.5):** because dose scales with batch volume and the thesis gaps are small, biasing the fertigation batch toward the high end of the working range yields larger, more-meterable doses.

---

## 2. Dosing Execution Method (LOCKED) — flow-primary, timed-ceiling backstop

This is the core safety decision. Two methods exist; they are NOT alternatives — they run together with distinct roles.

### 2.1. Primary: flow-meter volume (source of truth)
Per §19.4.4 / §14.2.2: count the nutrient line's flow pulses, convert with the calibrated K-factor (§A.4.1 of the calibration spec), stop when delivered volume reaches target mL. This is the accurate path and the ONLY thing that measures the dose.

### 2.2. Backstop: timed runtime as a hard ceiling (NOT a measurement)
For every dose, ESP32 #2 also computes a **maximum allowed runtime** and stops the pump if it is exceeded — regardless of pulse count. Timed runtime never *measures* the dose; it only *bounds* it, exactly like the TEST_MODE / flow-cal dead-man hard caps.

**Why both (the failure modes each one alone misses):**
- Flow-only, sensor fails **stuck-low** (no pulses) → pump never sees "target reached" → **runs forever, overdoses, drains the stock bottle into the mix.** The timed ceiling stops this.
- Timed-only, line **not primed** → pump pushes air, timer expires "satisfied" → **under-doses.** The flow meter catches this (real volume never reached).

Neither is safe alone; together the accurate method measures and the time limit bounds a stuck pump.

### 2.3. Stop conditions (whichever first)
| Stop trigger | Meaning | Action |
|---|---|---|
| Delivered volume == target mL | Normal completion | Stop pump, next nutrient |
| Runtime > ceiling (`2× expected`) | Flow sensor under-counting (failed stuck-low, or line unprimed) | Stop pump, raise **`DOSE_TIMEOUT`** (§5), do NOT silently continue |

**Critical rule (LOCKED):** a ceiling trip is a **fault, not a fallback.** The system never accepts the timed estimate and proceeds, because an unprimed line would make the timed value wrong too. It stops and hands the decision to the operator via the existing fault-hold (§19.4.8). Prime the line (§A.4.2 of calibration spec) and retry.

### 2.4. Ceiling computation (DERIVED)
```
expected_runtime_s = target_mL ÷ pump_flowrate_mLps
ceiling_s          = expected_runtime_s × CEILING_MARGIN   // CEILING_MARGIN = 2.0 (LOCKED)
```
- `pump_flowrate_mLps` comes from the **flow-cal K-factor** for that dosing channel (§A.4.1), NOT the guide's hardcoded `PUMP_x_FLOWRATE`. The guide's 50 mL/min is only a **pre-commissioning default** until the pump is flow-calibrated.
- `CEILING_MARGIN = 2.0` (LOCKED — user chose the more tolerant margin to avoid false trips on slightly slow pumps while still bounding a runaway).

---

## 3. Dose Calculation (from the dosing guide)

### 3.1. Gap-based open-loop formula
```
gap_X (mg/kg) = target_X − soil_baseline_X            (X = N, P, K)
dose_mL       = gap_X × tank_volume_L ÷ stock_conc_mgL
```
Only positive gaps dose; a non-positive gap means the soil already meets target → skip that nutrient.

### 3.2. Nutrient → element mapping (LOCKED)
The pumps dose salts, presets are elemental. The binding per the guide:
- **Nutrient B (MAP)** carries the **P** gap → `dose_B = gap_P × V ÷ STOCK_B_P`
- **Nutrient C (KNO₃)** carries the **K** gap → `dose_C = gap_K × V ÷ STOCK_C_K`
- **Nutrient A (CALCINIT)** carries the **N** gap → `dose_A = gap_N × V ÷ STOCK_A_N`, **gated on gap_N > 0** (§3.3).

(N is also incidentally supplied by B and C; the thesis treats N as out-of-scope for dosing since baseline N is already 5–6× target. The dose math drives B off P and C off K; A is the only N-driver and it is gated off.)

### 3.3. Nutrient A handling (LOCKED — kept, gap-gated)
Nutrient A stays in the firmware but **doses only if `gap_N > 0`**. For both crops the soil baseline N (1,060 mg/kg) far exceeds every target, so `gap_N` is always negative and A never fires in practice — **without hard-disabling it** (distinct from Nutrient D, which is hard-unused, §16). This keeps the calcium-supplement path available if a future crop preset ever has a positive N gap, while guaranteeing no nitrogen overdose for the thesis crops. *(User decision: gap-gated, not hard-disabled.)*

### 3.4. Sensor Resolution & Pulse-Quantized Dosing (LOCKED — from real hardware)

The dosing flow sensors resolve **~4 mL per pulse** (measured: the classmate's sketch computes `target_pulses = mL × SCALE` with `SCALE ≈ 0.25` on every channel, i.e. 0.25 pulses/mL). The sensor can only act on **whole pulses**, so the only volumes it can honestly deliver are integer multiples of ~4 mL (4, 8, 12 …). Sub-pulse targets (1.5 mL, 2.7 mL) cannot be metered — the firmware would stop at the first pulse and deliver ~4 mL, a large overdose.

**Consequence — dosing is quantized, with a whole-pulse floor (LOCKED):**

```
target_pulses_exact = dose_mL × SCALE              // SCALE ≈ 0.25 pulses/mL
target_pulses_whole = round(target_pulses_exact)   // nearest whole pulse (LOCKED)
IF target_pulses_whole < MIN_DOSE_PULSES:          // MIN_DOSE_PULSES = 3 (~12 mL) (LOCKED)
    → SKIP, log "sub-resolution dose, not delivered" + the intended mL/gap (thesis record)
ELSE:
    → dose to target_pulses_whole
    → delivered_mL = target_pulses_whole ÷ SCALE
    → log BOTH intended_mL AND delivered_mL (they differ; accuracy analysis needs the real one)
```

- **`MIN_DOSE_PULSES = 3` (~12 mL floor, LOCKED):** below 3 pulses, quantization error (±1 pulse) is too large a fraction to be honest, so the dose is skipped and logged rather than delivered coarsely. *(User decision: 3-pulse floor for better per-dose accuracy, fewer doses.)*
- **Round to nearest whole pulse (LOCKED)** for doses above the floor.
- **Log delivered (whole-pulse) volume, not intended mL** — the thesis dosing-accuracy analysis must use what was physically delivered.

### 3.5. Dilution is REQUIRED, not optional (consequence of §3.4)

With undiluted stock (guide concentrations) and the 3-pulse / 12 mL floor, **no realistic batch clears the floor:**

| Dose | gap | undiluted batch volume needed to reach 12 mL |
|---|---|---|
| Tomato S3 K (+100) | 100 mg/kg | **~406 L** (tank max is 50 L) |
| Pechay P boost (+40) | 40 mg/kg | ~401 L |
| Tomato S2 P (+20) | 20 mg/kg | ~803 L |

All far exceed the 50 L tank → at full-strength stock, nutrient C/B would **always** fall below the floor and never dose. To make a real dose clear 12 mL within a ≤50 L batch, the stock must be **diluted ~8×** (e.g. KNO₃ from 3,383 → ≤417 mg/L elemental K).

**Therefore (LOCKED): stock dilution is a required commissioning step, not an optional accuracy tweak.** Dilution factor is set so the expected doses for the target crop/stage land at **≥ MIN_DOSE_PULSES with margin** at the typical batch volume. Only the `STOCK_*` constants change; the mechanism is untouched.

- Bias fertigation **batch volume high** (§1.1) as secondary support — larger V → larger dose → more pulses.
- After dilution, doses still below the floor are honestly skipped-and-logged (§3.4). Given the thesis's "minimal dosing needed" finding (negative N gaps, small P/K gaps), some skips are expected and legitimately documented.
- **Rejected:** time-based dosing for small doses (unreliable unprimed, §2.2); leaving stock concentrated (guarantees no dosing ever clears the floor).

---

## 4. Work-Order Payload (extends §9.8.1.1)

The fertigation work order ESP32 #1 hands ESP32 #2 gains, per active nutrient:

| Field | Source | Purpose |
|---|---|---|
| target_mL | §3 calc | volume to deliver (flow-metered) |
| ceiling_s | §2.4 DERIVED | hard runtime cap (overdose backstop) |
| flow K-factor | calibration (§A.5.1) | pulse→mL for this channel |

This rides on the calibration self-containment already specified (§A.5.1 layer 3): the work order carries the job-critical flow K-factors so a mid-job ESP32 #2 reset can't lose them. Dosing simply adds `target_mL` and `ceiling_s` to that same payload.

---

## 5. Fault: `DOSE_TIMEOUT` (new code)

A new ESP32 #2 → ESP32 #1 response (register in §9.7.3 and the §25.2.1 `FAULT` log):

- **Trigger:** a nutrient pump's runtime exceeds its `ceiling_s` before reaching `target_mL`.
- **Meaning:** flow sensor under-counting — failed stuck-low, OR the line was not primed.
- **ESP32 #2 action:** stop that pump immediately (local protective stop, §11.1), de-energize per the fault-hold path, retain the mixing-tank volume metered so far (§19.4.8.1).
- **ESP32 #1 classification:** treat as a hard dosing fault that enters the user-gated fault-hold (§19.4.8.2) — alert carries operation + column + `DOSE_TIMEOUT` + which nutrient. Recovery options (STOP / RELEASE / IRRIGATE / NORMAL) apply as usual.
- **Distinct from** `FLOW_FAIL` (main-line flow validation) and the §29.7 `SENSOR_FAIL` — `DOSE_TIMEOUT` specifically means "dosing pump ran past its time ceiling," pointing the operator at priming or the dosing flow sensor.

*(User decision: dedicated `DOSE_TIMEOUT` rather than overloading `FLOW_FAIL`.)*

---

## 6. Editable Constants (corrected from the guide)

To live as clearly-labeled editable constants on ESP32 #1 (§10.10.1). Values from the dosing guide Part 4, **with corrections noted**.

```cpp
// ===== SOIL BASELINE (mg/kg) — F.A.S.T. Labs MC2812-6783 =====
const float SOIL_BASELINE_N = 1060.0;   // Kjeldahl
const float SOIL_BASELINE_P = 80.0;     // elemental (from P2O5)
const float SOIL_BASELINE_K = 200.0;    // elemental (from K2O)

// ===== STOCK CONCENTRATIONS (mg/L) — three 5L tanks =====
// NOTE 1: CALCINIT corrected to 1 kg / 5L = 3,100 mg/L N (the guide's
// worked examples that divide by 5,163 are STALE — old 1.613 kg prep).
// NOTE 2: These FULL-STRENGTH values CANNOT clear the 3-pulse (~12 mL)
// dosing floor within a ≤50 L batch (§3.5). Stock MUST be DILUTED at
// commissioning (≈8× for KNO₃) so real doses reach ≥ MIN_DOSE_PULSES.
// Update these constants to the DILUTED concentrations actually prepared.
const float STOCK_A_N  = 3100.0;        // CALCINIT N per L (1 kg/5L) — pre-dilution
const float STOCK_A_Ca = 3800.0;        // CALCINIT Ca per L
const float STOCK_B_P  = 1338.0;        // MAP elemental P per L — pre-dilution
const float STOCK_B_N  = 590.0;         // MAP N per L (incidental)
const float STOCK_C_K  = 3383.0;        // KNO3 elemental K per L — pre-dilution
const float STOCK_C_N  = 566.0;         // KNO3 N per L (incidental)

// ===== CROP PRESETS (mg/kg) =====
const float PECHAY_N = 180.0, PECHAY_P = 60.0, PECHAY_K = 120.0;
const float TOMATO_S1_N = 200.0, TOMATO_S1_P = 80.0,  TOMATO_S1_K = 160.0;
const float TOMATO_S2_N = 240.0, TOMATO_S2_P = 100.0, TOMATO_S2_K = 200.0;
const float TOMATO_S3_N = 150.0, TOMATO_S3_P = 80.0,  TOMATO_S3_K = 300.0;

// ===== DECISION / SAFETY =====
const float FERTIGATE_TRIGGER_GAP = 30.0;  // mg/kg (PLACEHOLDER, §14.2.0)
const float FERTIGATE_HYSTERESIS  = 10.0;  // mg/kg
const float CEILING_MARGIN        = 2.0;   // timed-ceiling = 2× expected (LOCKED)

// ===== DOSING SENSOR RESOLUTION (measured from dosing .ino) =====
// ~4 mL per pulse → 0.25 pulses/mL on all dosing channels.
const float SCALE_DOSE       = 0.25; // pulses per mL (per-channel SCALE_A..PHD ≈ this)
const int   MIN_DOSE_PULSES  = 3;    // ~12 mL floor; below this → skip+log (LOCKED)
// (Old DOSE_MIN_ML mL-based floor REMOVED — superseded by whole-pulse floor, §3.4)

// ===== MIXING TANK =====
// Batch volume is VARIABLE per run (set by column water budget, §1.1).
// MAX is a SAFETY CEILING, not the formula input.
const float MIXING_TANK_MAX_VOLUME = 50.0; // L (ceiling)
const float MIXING_TANK_SAFE_MIN   = 5.0;  // L (refuse dosing below)

// ===== PUMP FLOW (pre-commissioning default ONLY) =====
// Real value comes from flow-cal K-factor (§A.4.1); these are fallbacks
// until each dosing pump is calibrated.
const float PUMP_A_FLOWRATE_MLPM = 50.0;
const float PUMP_B_FLOWRATE_MLPM = 50.0;
const float PUMP_C_FLOWRATE_MLPM = 50.0;
```

---

## 7. Corrections / Reconciliations vs. the Dosing Guide

Captured so the guide and firmware don't silently diverge:

1. **CALCINIT concentration:** use **3,100 mg/L N** (1 kg/5L). The guide's Part 3 examples dividing by **5,163** are stale (old 1.613 kg prep) and must not be coded.
2. **Execution method:** the guide's "Step 3: convert mL → pump runtime (seconds)" is **demoted to the safety ceiling only** (§2.2). Flow-meter volume governs the actual dose. Timed seconds is never the measurement.
3. **Pump flow rate:** the guide's `50 mL/min` constants are **pre-commissioning defaults**; the live ceiling uses the flow-cal K-factor.
4. **Nutrient A:** the guide says "omit A"; firmware **keeps A but gap-gates it** (§3.3) — same practical result (never fires for these crops), but not hard-removed.
5. **EC as secondary validation** (guide Part 7): consistent with the spec's post-mix EC/pH check (§14.2.4) — EC remains a protective/validation layer, not a dosing control input. No change needed.
6. **Dosing sensor resolution (~4 mL/pulse, from the .ino):** the guide's small example doses (1.5–2.7 mL) are **below one pulse** and cannot be metered. Firmware uses a **whole-pulse floor (`MIN_DOSE_PULSES = 3`)** and **required stock dilution** (§3.4–3.5). The guide's `DOSE_MIN_ML = 0.5` floor is removed (it implied sub-pulse doses were deliverable; they are not).

---

## 8. Open / PENDING

- Register `DOSE_TIMEOUT` in the master `§9.7.3` response table and `§25.2.1` FAULT logging.
- Add `target_mL` + `ceiling_s` + `batch_volume_L` to the master `§9.8.1.1` work-order field list.
- **Stock dilution factor (§3.5):** set at commissioning so expected doses for the target crop/stage clear `MIN_DOSE_PULSES` with margin at the typical batch volume (≈8× for KNO₃ as a starting point); update `STOCK_*` constants to the diluted values actually prepared.
- Confirm `batch_volume_L` is sourced from ESP32 #2's metered fill (§1.1), not an assumed constant, in the work-order builder.
- Confirm the per-channel `SCALE_*` values from the dosing `.ino` (≈0.25) are carried into the calibration system (§A.4.1) so flow-cal can refine them per channel rather than assuming a shared 0.25.
- The mL→dose math implementation itself remains the classmate's subtask (open-loop calc); this spec defines the *mechanism and safety envelope* around it, not the arithmetic.
