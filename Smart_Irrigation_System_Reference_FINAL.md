# 1. 📄 Smart Irrigation & Fertigation System (Final Reference)

> **web v.4 "pure ultrasonic" (2026-11-16).** This copy of the firmware no longer uses the two main-line flow
> meters (**ResMix**, reservoir → mixing tank, GPIO26; **MixIrr**, mixing tank → column, GPIO5). Every litre that
> the transfer and booster pumps move is measured by the **ultrasonic sensor of each tank**, converted to litres
> with a calibrated tank shape. The nutrient / pH dosing meters are unchanged. The full description is in
> **§19.4.9**; wherever an older paragraph below still talks about the ResMix / MixIrr meters or about the
> ultrasonic being "display-only", §19.4.9 is what the firmware does.
>
> **State on 17 Nov 2026 (flashed; forced runs checked on the rig, §19.4.9.27).**
> - **Water:** a fill is a timed transfer run counted from the inverter switch-on, then a 1-minute settle, a
>   measurement, and a top-up when more than 3 L short. A delivery runs the booster until the mixing tank is under
>   5 % for 10 s (§19.4.9.13).
> - **Pumps:** pump lines come from the user's own measurements, re-measured on the LCD pump-estimate screens (§19.4.9.15).
> - **Testing:** 1 minute of inverter time for Fill/Push, 1.5 s between button runs, and a 120 ms release filter
>   (§19.4.9.14, §19.4.9.16–18).
> - **Forced runs:** no daily limit and no tank-check block; a switched-off column is switched back on (§19.4.9.21,
>   §19.4.9.26).
> - **BATT2:** blocks AC pumps at a resting charge of 30 % or less (§19.4.7.1).
> - **Website:** live updates every 2–4 s while someone watches, a Refresh button, and SD card logs through Firebase,
>   since the Supabase project expired (§19.4.9.22–23).
>
> Where these differ from older text, they win.

---

# 2. 🎯 Thesis Objective

This project aims to develop a **solar-powered automated irrigation and fertigation system** that:

* Maintains optimal soil moisture and nutrient levels
* Reduces manual labor in watering and fertilization
* Uses sensor-based decision-making for precision agriculture
* Operates efficiently using renewable energy
* Enables remote monitoring and control via GSM communication (A7670C)

---

# 3. 🧪 Research Goals

The system evaluates whether it can:

* Improve irrigation efficiency
* Maintain plant health through controlled nutrient dosing
* Reduce water and fertilizer waste

---

# 4. 🔗 Objective → System Mapping

| Objective               | System Component                            |
| ----------------------- | ------------------------------------------- |
| Maintain soil moisture  | Capacitive soil sensors + irrigation valves |
| Control nutrient levels | Flow sensors + dosing pumps                 |
| Reduce manual labor     | ESP32 automation logic                      |
| Monitor environment     | DHT22 + BH1750                              |
| Ensure safety           | Flow validation + protection logic          |
| Monitor power usage     | PZEM-004T energy meter                      |

---

# 5. 🏗️ System Overview

The system uses a **distributed architecture**:

| Controller   | Role                                     |
| ------------ | ---------------------------------------- |
| Arduino Nano | Sensor Hub                               |
| ESP32 #1     | Master Controller (decision-making)      |
| ESP32 #2     | Actuator Controller (execution & safety) |

---

## 5.1. 🔹 System Resilience Architecture

The system is designed with multiple recovery layers:

1. Software Recovery
   - Watchdog timers on ESP32 controllers
   - Automatic restart on failure

2. Communication Recovery
   - Retry mechanism for UART commands
   - Timeout detection

3. Hardware Recovery
   - Power cycling of ESP32 #2 via relay

This multi-layer approach ensures:
- Continuous operation
- Fault tolerance
- Minimal downtime


## 5.2. ⚠️ System Limitations

- GSM communication depends on network availability
- Dosing flow sensors provide estimated volume (calibration required)
- Water volumes come from the tank ultrasonics (web v.4): resolution ~0.3 cm ≈ 0.3 L after filtering, so a
  5 L batch is metered to roughly ±5–10 % — the tank litres calibration (§19.4.9.4) sets the accuracy
- Sensor accuracy may vary with environmental conditions
- The mixing-tank ultrasonic sees the mixer and the inflow when the tank is low or filling. Those readings are held
  back from control (§19.4.9.24), so a fill relies on its run time and on the measurement after it settles
- ESP32 #1 restarts when its supply dips (`SYS,BOOT,reason=POWER_ON`). A restart during a run stops the run and asks
  for a tank check (§19.4.9.21, §19.4.9.27)

---

## 5.3. ⚙️ Calibration Notes

- Dosing flow sensors must be calibrated for accurate volume measurement
- Each tank's "level → litres" curve must be calibrated (LCD Calibration → "Tank Mix liters" / "Tank Res
  liters", §19.4.9.4): the water metering rests on it
- pH and EC sensors require periodic calibration
- Sensor drift should be considered in long-term use

### 5.3.1. Capacitive Soil-Probe Calibration (measured)

Raw ADC readings per probe (normal polarity — **dry = high, wet = low**), captured on-site:

| Probe | 100% Wet | Dry |
| ----- | -------- | --- |
| A1    | 392      | 748 |
| A2    | 518      | 661 |
| B1    | 390      | 666 |
| B2    | 529      | 650 |

**Cross-wiring correction:** the two ADC channels per column were found physically swapped, so the
real grouping is **Column A = {B2, A2}** and **Column B = {B1, A1}**.

Averaging within each corrected column gives the baked per-column endpoints (in
`ESP1/src/main.cpp` → `calSoilAir` / `calSoilWater`, commit `ea9e280`):

| Column        | Dry (air, high ADC) | Wet (water, low ADC) |
| ------------- | ------------------- | -------------------- |
| A = {B2, A2}  | 656                 | 524                  |
| B = {B1, A1}  | 707                 | 391                  |
| C            | 800 (default)       | 300 (default)        |

The **NPK moisture field** reads 0 in unsaturated soil (hardware limitation) and is intentionally
kept **out** of the column moisture average — column moisture is **capacitive-only**.

---

## 5.4. 🔷 Microcontroller Responsibility Boundaries

The system uses strict role separation to reduce firmware complexity, avoid duplicated logic, and improve debugging reliability.

| Function                         | Arduino Nano | ESP32 #1 | ESP32 #2     |
| -------------------------------- | ------------ | -------- | ------------ |
| Read environmental sensors       | ✔            | ✘        | ✘            |
| Read soil sensors                | ✔            | ✘        | ✘            |
| Read ultrasonic sensors          | ✔            | ✘        | ✘            |
| Read NPK sensor                  | ✔            | ✘        | ✘            |
| Parse GSM commands               | ✘            | ✔        | ✘            |
| Decision-making                  | ✘            | ✔        | ✘            |
| Irrigation scheduling            | ✘            | ✔        | ✘            |
| Fault classification             | ✘            | ✔        | ✘            |
| LCD UI handling                  | ✘            | ✔        | ✘            |
| SD logging                       | ✘            | ✔        | ✘            |
| RTC scheduling                   | ✘            | ✔        | ✘            |
| Execute pumps/valves             | ✘            | ✘        | ✔            |
| Flow interrupt counting          | ✘            | ✘        | ✔            |
| Actuator-level safety validation | ✘            | ✘        | ✔            |
| Watchdog recovery                | ✘            | ✔        | ✔            |
| Nano reset authority (software RESET_REQ over UART) | ✘ | ✔ | ✘ |
| Master actuator-power cutoff (P17, sec.19.4.8)      | ✘ | ✘ | ✔ |

### 5.4.1. Design Philosophy

* ESP32 #1 is the supervisory controller and single decision-making authority.
* ESP32 #2 operates strictly as an actuator execution subsystem.
* Arduino Nano operates strictly as a sensor acquisition subsystem.
* No microcontroller may assume responsibilities outside its defined boundary.


---

# 6. 🔩 Hardware Components

## 6.1. Environmental Sensors

* DHT22 Temperature & Humidity
* BH1750 Light Sensor

## 6.2. Soil Monitoring

* RS485 7-in-1 NPK Sensors
* Capacitive Soil Moisture Sensors (6x)

## 6.3. Tank Monitoring

* Ultrasonic Reservoir Level Sensor — reservoir litres; cross-check of every fill (web v.4)
* Ultrasonic Mixing Tank Sensor — **meters every transfer and delivery** (web v.4)

## 6.4. Water System

* Flow Sensors (8x wired; web v.4 reads only the 6 dosing meters — ResMix / MixIrr are no longer used)
* Transfer Pump
* Booster Pump
* Mixer Motor

## 6.5. Nutrient System

* Nutrient A Pump (Calcium Nitrate)
* Nutrient B Pump (Mono Ammonium Phosphate)
* Nutrient C Pump (Potassium Nitrate)
* Nutrient D Pump (Magnesium Sulfate)
* pH Up Pump
* pH Down Pump

## 6.6. Monitoring Modules

* Battery voltage/current via a calibrated opto-isolated ADC read (GPIO35/34) — replaces the original INA226 (removed; kept only as a harmless boot-presence probe, see `CLAUDE.md`'s CHANGED section)
* PZEM-004T Power Monitor
* DS3231 RTC
* microSD Logging Module
* LCD Display
* A7670C GSM Module (remote communication)
* ACS712 Current Sensor (Mixer Motor Monitoring)

---

# 7. ⚙️ System Operation Flow

1. Sensors collect environmental and soil data
2. Arduino Nano sends data to ESP32 Master
3. ESP32 Master processes and decides actions
4. Commands sent to ESP32 Actuator
5. Pumps and valves execute irrigation/fertigation
6. Tank ultrasonics (water) and dosing flow sensors (nutrients) validate operation in real-time
7. GSM module sends alerts and receives user commands

---

# 8. 🔄 Data Flow

Sensors
↓
Arduino Nano
↓ UART
ESP32 #1 (Master)
↓ UART
ESP32 #2 (Actuator)
↓
Pumps / Valves

---

# 9. 🔄 Communication Protocol

Arduino Nano sends structured data via UART:

ENV → Temperature & Humidity
TANK → Reservoir & Mixing Level
SOIL → Soil moisture per column
LIGHT → Light intensity

## 9.1. Command Ownership Rules

- ESP32 #1 is the ONLY controller allowed to:
  - interpret GSM commands
  - change modes
  - modify configuration
  - classify faults
  - schedule operations

- ESP32 #2 MUST NOT independently make irrigation decisions.

- ESP32 #2 only:
  - executes commands
  - validates actuator-level safety
  - reports execution results

- Arduino Nano MUST NOT:
  - control actuators
  - classify faults
  - make automation decisions

## 9.2. Example Data

ENV,25.3,68.2
TANK,82.1,44.8,3.2
SOIL,A,41,B,53
LIGHT,72
NPK,A,38.5,24.1,1.2,6.4,150,40,200

(TANK third field = fill-flow L/min; SOIL is column-tagged and variable-length — only enabled columns appear, disabled columns omitted; NPK is per-enabled-column with a column tag, no packet for disabled columns. See Section 9.9.7.1 for the framed forms.)

## 9.3. Execution Flow

ESP#1 → Send command  
ESP#2 → Execute  
ESP#2 → Send DONE  

## 9.4. Reliability

- ESP#1 waits for response
- If no response:
  - Retry command
  - Optional power-cycle ESP#2

## 9.5. GSM Command Format

Commands follow a structured, compact format. See Section 12.2 for the full inbound command catalog.

Examples:

SET,COL_A,N,150,P,40,K,200,pH,5.8
SET,COL_A,PRESET,CARROT
NAME,COL_A,Lettuce
MODE,COL_A,IRRIGATION_ONLY
STATUS

## 9.6. 🔹 ESP32 Inter-Controller Command Protocol

ESP32 #1 sends command strings via UART:

Example:
SEQ_ALL

ESP32 #2:
- Receives command
- Executes full task sequence
- Responds with:

DONE

## 9.7. Authorized Uart Command List

### 9.7.1. Nano to ESP32 #1 Sensor Packets

| Command | Purpose          |
|---------|------------------|
| ENV     | Environment data (RAW; ESP1 applies offsets — companion spec §A) |
| SOIL    | Soil moisture (RAW averaged ADC; ESP1 maps to %) |
| TANK    | Tank levels (RAW ultrasonic distances cm + flow; ESP1 maps) |
| NPK     | Nutrient values (RAW Modbus registers; ESP1 applies scale + offset) |
| STATUS  | Nano health      |
| CAL     | One raw sample for the active calibration sensor (companion spec §A.3) |
| SERVO   | Pump-selector servo: `SERVO,<LEFT\|RIGHT>,<MOVING\|OK>,<seq>` — `OK` once it has settled at the target (2026-11-15, §19.4.7.2) |

Note (companion spec §A): the Nano streams RAW signals; ESP32 #1 owns and applies all calibration constants. The CAL packet (`CAL,<SENSOR_ID>,<RAW>`) carries the fast calibration stream.

### 9.7.1.1. ESP32 #1 to Nano Commands

| Command   | Purpose                                                  |
|-----------|----------------------------------------------------------|
| ACTIVE    | Switch Nano to active (10 s) transmission interval; also ends FAST |
| DAY       | Switch Nano to day-idle interval                         |
| NIGHT     | Switch Nano to night-idle interval                       |
| FAST      | Level-watch state while the transfer or booster pump runs: TANK about every 300 ms, heartbeat every 2 s, ENV/SOIL/LIGHT once a minute, NPK skipped. It is a 15 s lease: ESP32 #1 renews it every 5 s, and without a renewal the Nano returns to its previous state (2026-11-15) |
| SERVO,\<LEFT\|RIGHT\>,\<seq\> | Move the pump selector: LEFT (160°) = transfer pump, RIGHT (90°) = booster pump. Needs the checksum (see below). Sent only on ESP32 #2's `SERVO_REQ`, which ESP32 #2 sends only with its inverter OFF (§19.4.7.2) |
| RESET_REQ | Request Nano software self-reset (via short-timeout WDT) |

Since 2026-11-15, every ESP32 #1 → Nano command carries the same `,*XX` CRC-8 field as the Nano's own frames. The Nano drops a command whose checksum is wrong, and it refuses `SERVO` without a checksum.
| CAL_START,\<SENSOR_ID\> / CAL_STOP | Begin / end the fast raw calibration stream for one Nano sensor (companion spec §A.3). `CAL_STOP` takes no parameter — it unconditionally clears whichever stream is active (only one can be active at a time), matching the identical parameterless shape on ESP32 #2's own `CAL_STOP`. |

RESET_REQ is used both for fault recovery (after 5 consecutive garbage packets, Section 18.9.5) and for the once-per-day fresh-start reset.

### 9.7.2. ESP32 #1 to ESP32 #2 Execution Commands

| Command           | Purpose             |
|-------------------|---------------------|
| SEQ_IRRIGATION_\<X\>  | Begin irrigation of column X (A/B/C); carries FLUSH,\<pct\> |
| SEQ_FERTIGATION_\<X\> | Begin fertigation of column X; carries FLUSH/DOSE/EC/PH/MIX work-order params |
| STOP_ALL          | Emergency stop      |
| RESUME,\<NORMAL\|IRRIGATE\|RELEASE\> | User-gated fault recovery (sec.19.4.8.2). NORMAL resumes the paused sequence; IRRIGATE finishes as irrigation only (top up plain water, no dosing); RELEASE dumps the current mixing-tank contents to the assigned column as-is. |
| RESET_SELF        | Soft reset ESP32 #2 |
| STATUS_REQ        | Request status      |
| TEST,ENTER        | Enter manual TEST mode (Settings>Testing, sec.18.10.8): ESP32 #2 forces SAFE (all OFF) and arms the dead-man. Idle-only (BUSY during a sequence). |
| TEST,HOLD,\<idx\> | Dead-man keep-alive: ESP32 #1 streams this (~every 150 ms) while the operator holds ENTER. `idx` 0..15 = a single PCF8575 OUT_* relay (one-at-a-time). `idx` 16..19 = a **valve+pump priming combo** run together (16 = Fill: inverter+transfer pump; 17/18/19 = Push>Col A/B/C: inverter+column valve+booster pump; the inverter in each also moves the boards to the secondary battery when `SECONDARY_BATT_ENABLED`) with a longer ~30 s cap. Switching the selection turns the previous one OFF. |
| TEST,RELEASE      | Operator released ENTER — turn the held relay OFF immediately. |
| TEST,EXIT         | Leave TEST mode, all outputs OFF. |
| CAL_START,\<SENSOR_ID\> / CAL_STOP | Begin/end the fast raw calibration stream for an ESP2 sensor (pH/EC/ACS712/FLOW_*) — companion spec §A.3 |
| SET_CAL,\<SENSOR_ID\>,\<VALUE\>[,\<VALUE2\>] | Push a runtime calibration constant; ESP2 replies `ACK,SET_CAL,<id>` (block-until-ACK, §A.5.1) |
| PRIME_START,\<LINE_ID\>[,\<COL\>] / PRIME_STOP | Open a line's valve(s) + run its paired pump to purge air, dead-man + generous cap (§A.4.2) |

Work orders additionally carry job-critical calibration (`KMAIN/KNUT/ECCAL/PHCAL`) so a reset just before a job can never run on stale K-factors / EC-pH cal (companion spec §A.5.1 #3).

ESP32 #2 is the SOLE safety authority in TEST mode (sec.18.10.8.3): it turns the relay OFF if HOLD stops arriving within a short timeout (button released, ESP32 #1 hang, or link loss) and enforces a HARD **30-second** cap on continuous ON regardless of ESP32 #1. In TEST mode ESP32 #1 raises **no automatic faults and never emergency-stops** (they are logged only; the ESP2/Nano silence recovery and ESP2's PCF health reporting are suppressed) — the operator's MODE+BACK combo remains the manual e-stop.

### 9.7.3. ESP32 #2 to ESP #1 Responses

| Response  | Meaning                  |
|-----------|--------------------------|
| DONE      | Command completed        |
| BUSY      | Operation active (BUSY,ACTIVE / BUSY,TEST / BUSY,HELD = a held fault must be resolved via RESUME first, sec.19.4.8.2) |
| ERROR     | Execution failure. ESP32 #1 holds the work order on the master cutoff (ESP32 #2 stays alive, P17 dropped) and awaits a user-gated RESUME (sec.19.4.8) |
| DEGRADED  | A single channel disabled (nutrient no-flow / mixer no-load) but the sequence CONTINUES; ESP32 #1 classifies Major (alert + log, no shutdown, work order stays active). Used instead of ERROR for recoverable per-channel faults (sec.23.2.2.1, .4) |
| FLOW_FAIL | Flow validation failed. ESP32 #2 drops P17 and HOLDS with the mixing-tank volume retained; recovery is user-gated (sec.19.4.8) |
| PWR_FAIL  | PZEM power validation failed (pump ON but no current / overcurrent / voltage out of range). Triggers the same fault HOLD as FLOW_FAIL |
| DOSE_TIMEOUT,NUT_x | A nutrient pump ran past its 2× timed ceiling before reaching target mL (stuck-low dosing flow sensor or unprimed line). Hard dosing fault → ESP32 #1 enters the user-gated fault-hold (prime the line, then RELEASE/IRRIGATE/NORMAL). Distinct from FLOW_FAIL (main line) and SENSOR_FAIL. (Nutrient-dosing spec §5) |
| EC_FAIL   | EC outside safe window during dosing (local protective stop) |
| PH_FAIL   | pH outside safe window during dosing (local protective stop) |
| SENSOR_FAIL,EC / SENSOR_FAIL,PH | EC/pH probe railed (disconnected/shorted) during dosing — HARDWARE sensor fault, distinct from EC_FAIL/PH_FAIL (out-of-window). ESP32 #1 = Major (alert + log); batch still delivered (no drain). |
| PCF_FAIL,I2C | PCF8575 relay driver not ACKing (actuator bus fault), debounced over several reads. ESP32 #1 = Major (alert + log, no shutdown); other safeties still apply. Re-asserted ~10 s while faulted. |
| PCF_OK    | PCF8575 recovered (informational; clears the PCF fault) |
| SAFE_STOP | Emergency stop triggered |

## 9.8. 🔷 Command Granularity & Execution Model

The system uses a hybrid command architecture to balance centralized decision-making with distributed actuator execution.

---

### 9.8.1. High-Level Command Philosophy

ESP32 #1 transmits high-level sequence commands rather than individual actuator toggles.

Examples:

* SEQ_IRRIGATION_A
* SEQ_IRRIGATION_B
* SEQ_FERTIGATION_A
* SEQ_MIX_PREP
* SEQ_TRANSFER_FILL
* STOP_ALL

ESP32 #1 remains responsible for:

* deciding WHAT operation should occur
* scheduling operations
* validating global conditions
* fault classification
* retry strategy

### 9.8.1.1. Supervisor / Complete Work-Order Model

ESP32 #1 acts as a SUPERVISOR. For a normal operation it composes and sends a COMPLETE work order up front — everything ESP32 #2 needs to execute the entire sequence without asking ESP32 #1 for anything further during normal execution.

A work order includes (as applicable to the operation):

* the target column(s)
* the relay/actuator on-off sequence (which valves, pumps, mixer, in what order)
* pump assignments
* expected/target water volume (and the flush split, Section 14.2.0.2)
* nutrient dosing targets (`target_mL` per bottle, whole-pulse quantized, from the dosing calculation)
* per-dose `ceiling_s` (2× expected runtime — the DOSE_TIMEOUT backstop) and `batch_volume_L` (the variable batch the doses were computed against) — Nutrient-dosing spec §4
* EC/pH safe-window thresholds to enforce locally
* sequence timings and flow-timeout limits

Once the work order is sent:

* ESP32 #2 executes the FULL sequence autonomously (it owns HOW and the internal timing).
* ESP32 #1 STEPS BACK and primarily MONITORS — it watches heartbeats/responses, handles GSM (including deferred STATUS, Section 12.1.3.1), services its own watchdog, and waits for ESP32 #2's completion or fault reports.
* ESP32 #1 does NOT micro-step ESP32 #2 (no per-actuator toggling) during normal execution.

Analogy: ESP32 #1 is a supervisor who hands a complete job sheet to the worker (ESP32 #2) and then observes, rather than dictating each motion.

### 9.8.1.2. Fault Exception to the Supervisor Model

The "ESP32 #1 only listens" behavior applies to the NORMAL (happy-path) execution. Faults are the documented exception:

* ESP32 #2 performs IMMEDIATE local protective stops on its own authority for hardware safety (overcurrent, no-flow, EC/pH out of safe window) — it does not wait for ESP32 #1 to stop something dangerous.
* For POLICY-level decisions (continue in degraded mode? abort the session? retry?), ESP32 #2 reports the fault up and ESP32 #1 decides and responds (Section 11). ESP32 #2 does not classify global severity or decide long-term response.

So: supervisor hands off and monitors for normal runs; immediate safety stays local to ESP32 #2; policy-level fault decisions still escalate to ESP32 #1.

---

### 9.8.2. ESP32 #2 Execution Authority

ESP32 #2 is responsible for executing actuator sequences internally.

This includes:

* valve sequencing
* pump sequencing
* actuator timing
* flow timeout timing
* local protective stop logic
* execution acknowledgment

ESP32 #2 determines HOW a requested sequence is physically executed.

---

### 9.8.3. Design Benefits

This architecture:

* reduces UART traffic
* avoids duplicated timing logic
* simplifies firmware maintenance
* improves execution consistency
* isolates actuator complexity
* improves fault containment

---

### 9.8.4. Restriction

ESP32 #2 must not independently:

* schedule irrigation
* change system mode
* classify global faults
* alter configuration authority

All high-level operational authority remains under ESP32 #1 supervision.

---

## 9.9. 🔷 UART Packet Framing Standard

To improve communication reliability, parsing consistency, debugging simplicity, and fault recovery behavior, all UART communication follows a structured framed-packet protocol.

The protocol is designed for:

* readability
* robustness
* low parsing complexity
* distributed controller coordination

---

### 9.9.1. Standard Packet Structure

All UART packets follow this format:

<START>,COMMAND,PARAMETER_1,PARAMETER_2,<END>

Examples:

<START>,ENV,28.4,65.2,<END>

<START>,SOIL,A,41,B,53,C,48,<END>

<START>,SEQ_IRRIGATION_A,<END>

<START>,FLOW_FAIL,MAIN,<END>

---

### 9.9.2. Packet Formatting Rules

The following rules apply to all controllers:

* All commands use UPPERCASE format
* Parameters are comma-separated
* No spaces allowed inside packets
* START and END markers are mandatory
* Packets must remain human-readable ASCII
* Maximum packet length: 128 bytes for short frames (Nano↔ESP1, ESP2→ESP1 replies).
  EXCEPTION: the ESP1→ESP2 work order carries the self-contained job calibration
  (KMAIN/KNUT/ECCAL/PHCAL, §A.5.1) and a full fertigation order runs ~200 bytes, so ESP2's
  receiver accepts up to 256 bytes (buffer + frame check). A tighter cap drops fertigation orders.

---

### 9.9.3. START and END Markers

Packet framing markers are used to:

* detect packet boundaries
* recover from UART corruption
* ignore partial transmissions
* prevent malformed command execution

Markers:

| Marker  | Purpose           |
| ------- | ----------------- |
| <START> | Packet beginning  |
| <END>   | Packet completion |

Controllers must not execute commands unless both markers are validated.

---

### 9.9.4. Packet Validation Rules

Packets are considered valid only if:

* START marker detected
* END marker detected
* Command recognized
* Parameter count acceptable
* Packet length within limit

If validation fails:

* packet ignored
* optional error counter incremented
* no actuator action permitted

---

### 9.9.5. Invalid Packet Handling

Examples of invalid packets:

* missing START marker
* missing END marker
* corrupted command
* incomplete transmission
* excessive packet length
* invalid parameter count

Invalid packets must never trigger actuator execution.

---

### 9.9.6. Communication Safety Principle

Controllers must never:

* execute partial packets
* assume incomplete UART data is valid
* continue execution after malformed command parsing
* perform actuator operations from unverified packets

Safety always overrides communication convenience.

---

### 9.9.7. Recommended Packet Examples

#### 9.9.7.1. Arduino Nano → ESP32 #1

<START>,ENV,28.4,65.2,<END>

<START>,TANK,82.1,44.8,3.2,<END>

<START>,SOIL,A,41,B,53,<END>

<START>,LIGHT,72,<END>

<START>,NPK,A,38.5,24.1,1.2,6.4,150,40,200,<END>

<START>,NPK,B,40.0,23.8,1.1,6.5,148,38,195,<END>

<START>,STATUS,NANO,OK,<END>

Packet field notes:
- TANK: reservoir%, mixing%, fill-flow L/min (flow folded into TANK; ESP32 #1 parser expects 3 fields)
- SOIL: COLUMN-TAGGED and variable-length — only ENABLED columns appear, each as a (letter, value) pair. Disabled columns are OMITTED entirely (no field, no placeholder). Example above shows columns A and B active, C disabled. Tags (not position) identify the column, so omission is unambiguous even if the disabled column is not the last one.
- NPK: one packet PER enabled column, tagged with the column letter; disabled columns emit NO packet. Fields are moisture, temp, EC, pH, N, P, K from that column's 7-in-1 sensor (one sensor per column, unique Modbus addresses on a shared RS485 bus). Failed reads on an ENABLED column emit −1 per field (sensor-fault sentinel, distinct from "disabled").
- The Nano does NOT read sensors for a disabled column (avoids floating/unconnected inputs).

---

#### 9.9.7.2. ESP32 #1 → ESP32 #2

<START>,SEQ_IRRIGATION_A,<END>

<START>,STOP_ALL,<END>

<START>,RESUME,NORMAL,<END>

---

#### 9.9.7.3. ESP32 #2 → ESP32 #1

<START>,ACK,SEQ_IRRIGATION_A,<END>

<START>,DONE,SEQ_IRRIGATION_A,<END>

<START>,FLOW_FAIL,MAIN,<END>

---

### 9.9.8. Design Philosophy

The UART protocol prioritizes:

* reliability
* easy debugging
* deterministic parsing
* modular firmware development
* safer actuator handling
* easier recovery implementation

The protocol is intentionally lightweight and human-readable to simplify development and troubleshooting.


---

# 10. 🔷 Source of Truth Architecture

To prevent configuration conflicts and maintain consistent system behavior, the system follows a centralized “Source of Truth” architecture.

## 10.1. Primary Authority

ESP32 #1 is the single authoritative controller for:

* System mode selection
* Irrigation thresholds
* Fertigation configuration
* Scheduling and timing
* Fault classification state
* User configuration values
* Battery protection policies
* GSM command interpretation
* System-wide operational state

ESP32 #1 maintains the official runtime state of the entire system.

---

## 10.2. ESP32 #2 Authority Scope

ESP32 #2 does not maintain permanent system authority.

ESP32 #2 responsibilities are limited to:

* Actuator execution
* Flow interrupt monitoring
* Local actuator safety validation
* Command acknowledgment
* Temporary execution state reporting

ESP32 #2 only executes instructions provided by ESP32 #1.

---

## 10.3. Arduino Nano Authority Scope

The Arduino Nano operates strictly as a sensor acquisition subsystem.

The Arduino Nano:

* reads sensors
* formats data packets
* transmits measurements

The Arduino Nano does not:

* store persistent configuration
* make automation decisions
* classify faults
* control actuators on its own

Exception (2026-11-15): the Nano drives ONE actuator, and only on command. This is the pump-selector servo on D3 (§19.4.7.2). It moves only on a checksummed `SERVO` command. It is never moved at boot or after a reset, so a Nano reset in the middle of a run cannot switch the AC under load. The Nano keeps the last angle in EEPROM only as the starting point for the next ramp.

---

## 10.4. Configuration Synchronization Principle

Whenever configuration values are changed:

* GSM commands update ESP32 #1 only
* ESP32 #1 distributes required execution parameters to ESP32 #2
* ESP32 #2 does not permanently store configuration authority

This prevents:

* desynchronized thresholds
* conflicting operating modes
* inconsistent automation behavior

---

## 10.5. Design Philosophy

The system prioritizes centralized decision-making with distributed execution.

Benefits include:

* easier debugging
* predictable system behavior
* simplified firmware maintenance
* reduced logic duplication
* improved long-term reliability

## 10.6. ⏱️ Timing Ownership Architecture

To prevent duplicated timers, conflicting retries, and inconsistent execution behavior, the system uses explicit timing ownership rules.

Each microcontroller is responsible only for timing operations within its assigned authority scope.

---

### 10.6.1. Arduino Nano Timing Ownership

The Arduino Nano owns only sensor acquisition timing.

Responsibilities include:

* Sensor read intervals
* Sensor transmission intervals
* Dynamic interval adjustment
* Non-blocking sensor scheduling

Examples:

* ACTIVE state transmission interval
* Day idle interval
* Night idle interval

The Arduino Nano must not manage:

* actuator timing
* retry timing
* scheduling logic
* fault timeout timing

---

### 10.6.2. ESP32 #1 Timing Ownership

ESP32 #1 owns all global system timing and supervisory timing functions.

Responsibilities include:

* Irrigation scheduling (RTC-based)
* Fertigation scheduling
* Retry intervals
* GSM retry timing
* Fault escalation timing
* Daily report timing
* Long-term maintenance timing
* System heartbeat monitoring
* Recovery timeout monitoring
* Preventive maintenance scheduling

Examples:

* Retry command after timeout
* Daily GSM report at 6:00 PM
* Pump exercise every 2 days
* Fault persistence validation

ESP32 #1 determines WHEN operations should occur.

---

### 10.6.3. ESP32 #2 Timing Ownership

ESP32 #2 owns all actuator-local and execution-local timing.

Responsibilities include:

* Valve sequencing timing
* Pump sequencing timing
* Flow timeout timing
* Dosing timing
* Local actuator safety timing
* Sequence execution timing
* Temporary protective delay timing

Examples:

* Stop nutrient pump after target volume reached
* Detect no-flow within 10 seconds
* Delay between valve transitions
* Mixer stabilization timing

ESP32 #2 determines HOW operations are physically executed.

---

### 10.6.4. Watchdog Timing Ownership

Each ESP32 independently manages its own watchdog timing.

* ESP32 #1 watchdog protects supervisory logic
* ESP32 #2 watchdog protects actuator execution logic

Watchdog servicing must remain independent.

---

### 10.6.5. Non-Blocking Timing Principle

All timing operations must use non-blocking methods.

Examples:

* millis()-based scheduling
* hardware timers
* interrupt-safe timing

Blocking delays that interfere with:

* watchdog servicing
* UART communication
* flow interrupts
* GSM processing

must be avoided.

---

### 10.6.6. Timing Coordination Principle

Timing ownership follows system authority boundaries:

| Timing Category   | Owner            |
| ----------------- | ---------------- |
| Sensor intervals  | Arduino Nano     |
| System scheduling | ESP32 #1         |
| Retry timing      | ESP32 #1         |
| GSM timing        | ESP32 #1         |
| Flow timeout      | ESP32 #2         |
| Sequence timing   | ESP32 #2         |
| Dosing timing     | ESP32 #2         |
| Watchdog timing   | Local controller |

This prevents:

* duplicated timers
* race conditions
* conflicting retries
* inconsistent sequencing behavior

---

### 10.6.7. Design Philosophy

The system separates supervisory timing from execution timing.

Benefits include:

* cleaner firmware structure
* predictable behavior
* simplified debugging
* reduced synchronization complexity
* improved watchdog stability
* improved modularity

## 10.7. 🔄 Startup Synchronization Architecture

The system uses staged startup synchronization to prevent race conditions, unsafe actuator behavior, false fault detection, and communication instability during boot or recovery events.

ESP32 #1 acts as the startup coordinator and supervisory initialization authority.

---

### 10.7.1. Startup Design Philosophy

The system follows these startup principles:

* All controllers initialize into SAFE state
* No actuator operation is allowed during startup
* Controllers must explicitly report readiness
* ESP32 #1 controls operational authorization
* Communication links must stabilize before automation begins

This prevents:

* accidental actuator activation
* invalid sensor interpretation
* boot-time UART corruption
* premature fault classification

---

### 10.7.2. Startup Sequence Overview

Normal startup sequence:

1. ESP32 #1 boots
2. RTC, logging, and supervisory systems initialize
3. ESP32 #1 enables GPIO4 relay power to ESP32 #2
4. ESP32 #2 boots
5. ESP32 #2 initializes PCF8575 outputs to SAFE state (HIGH/OFF)
6. ESP32 #2 sends READY signal to ESP32 #1
7. ESP32 #1 validates ESP32 #2 communication
8. Arduino Nano communication enabled
9. Arduino Nano begins structured sensor transmission
10. ESP32 #1 validates initial sensor data
11. System enters IDLE state
12. Automation logic becomes active

---

### 10.7.3. SAFE Startup State

During startup:

* All pumps remain OFF
* All valves remain CLOSED
* Fertigation disabled
* Irrigation disabled
* No automatic execution permitted

SAFE state remains active until:

* ESP32 #2 READY confirmed
* Nano communication valid
* Critical sensor checks completed

---

### 10.7.4. ESP32 #2 Startup Responsibilities

Upon power-up or reset, ESP32 #2 must:

* initialize watchdog
* initialize UART
* initialize I2C bus
* initialize PCF8575 outputs HIGH
* verify actuator SAFE state
* avoid executing previous commands
* send READY message to ESP32 #1

ESP32 #2 must not autonomously begin actuator operation after reboot.

---

### 10.7.5. Arduino Nano Startup Responsibilities

Upon startup, Arduino Nano must:

* initialize sensors
* initialize UART communication
* stabilize sensor readings
* avoid blocking startup delays
* begin structured transmission only after initialization completes

Initial sensor packets are treated as stabilization packets and may be ignored temporarily by ESP32 #1.

---

### 10.7.6. Startup Communication Validation

ESP32 #1 validates:

* ESP32 #2 READY response
* Nano UART activity
* Valid packet formatting
* Sensor plausibility
* RTC availability
* SD logging initialization

Only after successful validation may:

* irrigation
* fertigation
* automation
* scheduled execution

be enabled.

---

### 10.7.7. Recovery Startup Behavior

The same startup synchronization process applies after:

* watchdog resets
* power cycling
* Nano recovery
* ESP32 #2 recovery
* communication recovery events

This ensures consistent recovery behavior.

---

### 10.7.8. Startup Fault Handling

If startup validation fails:

ESP32 #1 may:

* retry communication
* delay automation
* request subsystem recovery
* keep system in SAFE state
* send GSM alert if required

Automation must not begin during uncertain startup conditions.

---

### 10.7.9. Design Benefits

This architecture provides:

* predictable startup behavior
* improved safety
* reduced race conditions
* stable UART initialization
* cleaner recovery behavior
* safer actuator handling
* improved watchdog recovery stability

## 10.8. ❤️ Heartbeat & Health Monitoring System

The system uses heartbeat messages to monitor controller responsiveness, communication integrity, and subsystem operational health.

Heartbeat monitoring helps distinguish between:

* temporary inactivity
* communication failure
* software lockup
* subsystem restart
* watchdog recovery events

ESP32 #1 acts as the primary heartbeat supervisor.

---

### 10.8.1. Heartbeat Design Philosophy

Heartbeat packets are lightweight periodic status messages transmitted independently of normal operational commands.

Heartbeat messages confirm:

* controller responsiveness
* active firmware execution
* UART communication availability
* subsystem operational state

Heartbeat packets do not imply:

* sensor validity
* actuator correctness
* fault-free operation

They only confirm that the subsystem remains responsive.

---

### 10.8.2. Arduino Nano Heartbeat

Arduino Nano periodically transmits heartbeat packets to ESP32 #1.

Example:

STATUS,NANO,OK

Purpose:

* confirm Nano operational state
* validate UART activity
* detect Nano lockup
* detect stalled sensor loop

ESP32 #1 monitors heartbeat freshness.

If heartbeat timeout occurs:

* communication recovery may begin
* Nano software recovery may be attempted (RESET_REQ)
* (the Nano's own internal watchdog handles a true lockup; there is no external hardware reset)

---

### 10.8.3. ESP32 #2 Heartbeat

ESP32 #2 periodically transmits heartbeat packets to ESP32 #1.

Example:

STATUS,ESP2,OK

Purpose:

* confirm actuator controller responsiveness
* validate command subsystem availability
* detect execution lockup
* confirm watchdog recovery completion

Heartbeat transmission continues even during idle actuator state.

#### 10.8.3.1. ESP32 #2 silence is confirmed by active probing (not assumed)

A missed heartbeat window alone does NOT declare `ESP2_SILENCE`. After the freshness gap
(`HEARTBEAT_TIMEOUT_MS`, 120 s), ESP32 #1 first **actively probes** ESP32 #2 with `STATUS_REQ`
up to **5 times at 2 s spacing (~10 s)**. Any single frame received from ESP32 #2 during this
window — a heartbeat or a probe reply — cancels the probe and clears the condition. Only when all
5 probes go unanswered does ESP32 #1 raise `ESP2_SILENCE` (Major) and enter the recovery ladder
(soft `RESET_SELF` → power-cycle). This prevents a false silence declaration — caused by a
transient link glitch, an RX buffer drop, or a momentarily starved ESP32 #1 loop — from needlessly
resetting a healthy actuator controller.

#### 10.8.3.2. ESP32 #2 idle self-heal (self-reboot)

As an independent safety net, ESP32 #2 self-reboots (`ESP.restart()`) after **30 min of
continuous idle** — i.e. no active work order, no held fault, not in Testing, not calibrating, not
priming, and no pending pump exercise. It emits `INFO,IDLE_RESET` first (ESP32 #1 logs it) and
returns with `READY,ESP2`. This lets a wedged-but-working ESP32 #2 recover on its own even if
ESP32 #1 has (wrongly) concluded it is silent. The self-reboot **never** fires during a held fault
(the hold must persist until the operator resolves it), and the mixing-tank volume (`mixTankL`) is
NVS-persisted, so it survives the reboot.

---

### 10.8.4. Heartbeat Supervision Authority

ESP32 #1 supervises:

* Nano heartbeat freshness
* ESP32 #2 heartbeat freshness
* communication timeout persistence
* repeated subsystem loss

ESP32 #1 determines:

* whether retry is required
* whether recovery is required
* whether fault escalation is necessary

---

### 10.8.5. Heartbeat Timeout Handling

If heartbeat packets are not received within expected timing:

ESP32 #1 may:

* retry communication
* request subsystem status
* delay automation
* trigger software recovery
* trigger hardware recovery
* classify communication faults

Timeout behavior depends on:

* system operational state
* actuator activity
* fault persistence
* startup condition

---

### 10.8.6. Dynamic Heartbeat Behavior

Heartbeat intervals may change depending on system state.

Examples:

| System State     | Heartbeat Interval |
| ---------------- | ------------------ |
| IDLE Daytime     | Slower             |
| IDLE Night       | Slowest            |
| ACTIVE Operation | Faster             |
| Recovery State   | Fastest            |

This reduces unnecessary communication load while maintaining responsiveness during critical operation.

---

### 10.8.7. Heartbeat vs Sensor Data

Heartbeat packets are separate from sensor packets.

Sensor data:

* contains measurements

Heartbeat packets:

* confirm subsystem responsiveness

This separation prevents:

* communication ambiguity
* stale-state confusion
* false operational assumptions

---

### 10.8.8. Recovery Integration

Heartbeat monitoring integrates with:

* watchdog recovery
* Nano reset strategy
* ESP32 #2 power cycling
* startup synchronization
* fault classification

Heartbeat failure alone does not immediately imply critical system failure.

ESP32 #1 evaluates:

* timeout duration
* repeated failure persistence
* operational context
* current automation activity

before escalation.

---

### 10.8.9. Design Benefits

This architecture provides:

* improved subsystem monitoring
* earlier fault detection
* better recovery decisions
* reduced communication ambiguity
* improved watchdog supervision
* cleaner distributed coordination
* more reliable autonomous operation

---

## 10.9. 🔷 ACTIVE State Definition

The system uses an explicit ACTIVE state to coordinate monitoring behavior, communication frequency, timing sensitivity, and fault supervision intensity across all controllers.

---

### 10.9.1. ACTIVE State Meaning

ACTIVE state is defined as any condition where actuator-related operation is currently occurring or expected imminently.

Examples include:

* irrigation
* fertigation
* nutrient dosing
* transfer pumping
* mixing operation
* valve-controlled flow operation
* actuator sequence execution
* active recovery operation

ACTIVE state represents elevated operational activity requiring increased monitoring responsiveness.

---

### 10.9.2. ACTIVE State Ownership

ESP32 #1 is the only controller authorized to determine global ACTIVE state.

ESP32 #1 evaluates:

* current operation mode
* active sequence execution
* pending actuator activity
* fault recovery operations
* scheduled execution state

ESP32 #1 distributes ACTIVE state information to:

* Arduino Nano
* ESP32 #2 if required

This prevents inconsistent subsystem interpretation.

---

### 10.9.3. ACTIVE State Effects

When ACTIVE state is enabled:

#### 10.9.3.1. Arduino Nano:

* increases sensor transmission frequency
* increases heartbeat frequency
* prioritizes fresh sensor acquisition

#### 10.9.3.2. ESP32 #1:

* increases communication supervision sensitivity
* enables tighter timeout monitoring
* increases fault responsiveness
* enables active operation validation

#### 10.9.3.3. ESP32 #2:

* enables flow validation timing
* enables actuator safety timing
* enables execution monitoring
* increases watchdog sensitivity if required

---

### 10.9.4. IDLE State Definition

IDLE state exists when:

* no actuator activity is occurring
* no actuator sequence is pending
* no recovery operation is active
* no scheduled execution is imminent

During IDLE state:

* communication intervals may slow
* heartbeat intervals may slow
* sensor transmission intervals may slow
* monitoring remains active but less aggressive

---

### 10.9.5. Transitional ACTIVE State

The system may temporarily enter ACTIVE state before physical actuator activation.

Examples:

* pre-start validation
* safety verification
* sequence preparation
* startup stabilization

This allows tighter supervision before actuator engagement begins.

---

### 10.9.6. ACTIVE State and Fault Handling

Certain faults automatically force ACTIVE state.

Examples:

* recovery operations
* watchdog recovery
* communication retry sequences
* fault validation procedures

This ensures increased monitoring sensitivity during unstable conditions.

---

### 10.9.7. Design Philosophy

The ACTIVE state architecture allows the system to dynamically adapt monitoring intensity according to operational demand.

Benefits include:

* improved responsiveness during operation
* reduced unnecessary communication traffic
* lower idle power consumption
* improved fault supervision
* cleaner timing coordination
* consistent subsystem behavior

---

## 10.10. 💾 Persistent Configuration Storage Architecture

The system uses centralized persistent configuration storage to maintain consistent behavior across reboot events, watchdog recovery, power loss, and subsystem recovery.

ESP32 #1 is the only controller authorized to permanently store system configuration.

---

### 10.10.1. Configuration Authority

ESP32 #1 maintains the official persistent configuration for the entire system.

Persistent configuration includes:

* irrigation threshold (soil start %)
* fertigation settings
* per-column mode (AUTO / IRRIGATION_ONLY)
* per-column schedule
* fertigate trigger gap + hysteresis (nutrient-gap threshold for AUTO decision)
* post-fertigation flush percentage (FLUSH_PCT, default 20%)
* column enable flags (COLUMN_ENABLED)
* scheduling configuration (incl. daily-summary time)
* GSM settings
* calibration values (flow-sensor K-factors / pulses-per-liter, pH and EC calibration coefficients)
* EC/pH safe-window thresholds
* nutrient concentration constants and laboratory soil-nutrient baseline
* per-column nutrient presets (N-P-K mg/kg + pH) and built-in crop preset selection
* per-column plant names
* mixing/homogenization duration
* retry limits
* battery protection thresholds
* user preferences
* automation behavior settings

ESP32 #2 and Arduino Nano do not maintain permanent configuration authority.

---

### 10.10.2. Storage Technology

ESP32 #1 uses:

* ESP32 Preferences/NVS storage

for non-volatile configuration persistence.

Benefits include:

* built-in wear management
* reliable flash storage
* reboot persistence
* low implementation complexity
* improved long-term stability

Optional SD card backup may be implemented for redundancy or logging purposes.

---

### 10.10.3. Runtime Configuration Distribution

During startup synchronization:

* ESP32 #1 restores persistent configuration
* ESP32 #1 distributes required runtime parameters to ESP32 #2
* ESP32 #1 distributes operational state information if required

ESP32 #2 stores runtime copies only.

Arduino Nano does not store persistent automation configuration.

---

### 10.10.4. Configuration Synchronization Rules

Whenever configuration changes occur:

* GSM commands modify ESP32 #1 configuration only
* ESP32 #1 updates persistent storage
* ESP32 #1 distributes updated runtime parameters to subsystems

ESP32 #2 must not independently modify permanent configuration values.

This prevents:

* desynchronized thresholds
* conflicting automation behavior
* inconsistent recovery behavior

---

### 10.10.5. Reboot Behavior

After reboot or watchdog recovery:

* persistent configuration is restored
* runtime state is reinitialized
* startup synchronization process repeats
* actuator operations remain disabled until validation completes

The system must not automatically resume actuator execution solely from restored runtime state.

---

### 10.10.6. Non-Persistent Runtime State

The following runtime states are intentionally NOT persisted:

* ACTIVE state
* current actuator sequence
* temporary fault flags
* transient communication state
* watchdog status
* temporary execution timers
* pending deferred STATUS report flag (Section 12.1.3.1)

These states are recalculated after reboot.

---

### 10.10.7. Corruption Recovery Philosophy

If persistent configuration corruption is detected:

ESP32 #1 may:

* restore default safe configuration
* enter SAFE mode
* request user reconfiguration
* generate GSM alert
* disable automation until validation completes

Safety overrides configuration persistence.

---

### 10.10.8. Design Philosophy

The system uses centralized persistent storage with distributed runtime execution.

Benefits include:

* simplified synchronization
* cleaner authority boundaries
* easier debugging
* predictable reboot behavior
* reduced firmware complexity
* improved recovery consistency

---

## 10.11. 🧠 System State Machine Architecture

The system uses a centralized global state machine to coordinate operational behavior, recovery logic, automation permissions, timing behavior, and subsystem supervision.

ESP32 #1 is the only controller authorized to maintain and modify the global system state.

---

### 10.11.1. State Machine Design Philosophy

The global state machine defines:

* overall operational mode
* automation permission level
* monitoring intensity
* recovery behavior
* actuator authorization
* fault handling behavior

The state machine prevents:

* conflicting operational logic
* unsafe actuator behavior
* inconsistent recovery execution
* ambiguous automation behavior

Only one global system state may be active at any time.

---

### 10.11.2. Global System States

| State          | Description                                      |
| -------------- | ------------------------------------------------ |
| BOOT_STATE     | Initial firmware startup                         |
| STARTUP_SYNC   | Startup validation and subsystem synchronization |
| IDLE_STATE     | System operational but inactive                  |
| ACTIVE_STATE   | Actuator-related operation active                |
| RECOVERY_STATE | Recovery or retry procedures active              |
| SAFE_MODE      | Restricted operation due to persistent fault     |
| EMERGENCY_STOP | Critical shutdown state                          |
| TEST_MODE      | Manual bench testing; automation + safety bypassed (Section 18.10.8) |

---

### 10.11.3. BOOT_STATE

Purpose:

* initialize firmware
* initialize hardware
* initialize watchdogs
* initialize communication interfaces

Behavior:

* all actuators disabled
* no automation allowed
* no scheduling active

Transition:

* enters STARTUP_SYNC after initialization completes

---

### 10.11.4. STARTUP_SYNC

Purpose:

* validate subsystem availability
* synchronize communication
* confirm SAFE startup state
* restore persistent configuration

Validation includes:

* ESP32 #2 READY confirmation
* Nano communication validation
* RTC validation
* SD initialization validation
* sensor plausibility validation

Transition:

* enters IDLE_STATE after successful validation
* enters SAFE_MODE if validation repeatedly fails

---

### 10.11.5. IDLE_STATE

Purpose:

* maintain operational readiness
* monitor sensors
* supervise communication
* wait for scheduled or manual operation

Behavior:

* automation enabled
* no active actuator operation
* slower communication intervals allowed
* heartbeat monitoring active

Transition:

* enters ACTIVE_STATE when operation begins
* enters RECOVERY_STATE if recovery required

---

### 10.11.6. ACTIVE_STATE

Purpose:

* supervise actuator-related operation

Examples:

* irrigation
* fertigation
* nutrient dosing
* transfer pumping
* mixing operation

Behavior:

* tighter monitoring intervals
* faster heartbeat supervision
* flow validation enabled
* actuator safety monitoring active

Transition:

* enters IDLE_STATE after operation completes
* enters RECOVERY_STATE if abnormal condition detected
* enters EMERGENCY_STOP if critical fault occurs

---

### 10.11.7. RECOVERY_STATE

Purpose:

* supervise fault recovery procedures
* retry communication
* retry execution
* coordinate subsystem recovery

Examples:

* UART retry
* Nano recovery
* ESP32 #2 recovery
* communication validation
* startup re-synchronization

Behavior:

* automation temporarily restricted
* tighter monitoring enabled
* recovery escalation active

Transition:

* enters IDLE_STATE after successful recovery
* enters SAFE_MODE if recovery repeatedly fails
* enters EMERGENCY_STOP if critical safety fault occurs

---

### 10.11.8. SAFE_MODE

Purpose:

* maintain limited safe operation after persistent fault condition

Behavior:

* automation disabled
* actuator execution restricted
* monitoring continues
* logging continues
* GSM alerts permitted

SAFE_MODE allows:

* diagnostics
* communication
* user intervention

Transition:

* may return to IDLE_STATE after successful recovery and validation

---

### 10.11.9. EMERGENCY_STOP

Purpose:

* immediately halt dangerous operation

Triggers may include:

* critical electrical fault
* dangerous overcurrent
* unrecoverable actuator fault
* unsafe system condition

Behavior:

* all pumps OFF
* all valves CLOSED
* automation disabled
* actuator execution prohibited
* critical alert generation enabled

Recovery from EMERGENCY_STOP may require:

* manual acknowledgment
* restart sequence
* fault clearance validation

---

### 10.11.9.1. TEST_MODE

Purpose:

* manual bench testing of individual actuators (Section 18.10.8)

Entry / behavior:

* entered/exited via LCD menu; forces SAFE state on entry
* automation disabled; scheduled runs suppressed
* ESP32 #2 powered ON and aware of testing mode
* one relay at a time, momentary dead-man control via ENTER button
* normal protective safeties intentionally bypassed; only dead-man release + ESP32 #2's 30-second hard cap remain. No automatic faults/emergency-stop are raised in TEST mode (logged only); MODE+BACK stays as the manual e-stop
* GSM alerts remain active; all manual actuations logged

Transition:

* returns to IDLE_STATE on exit (after forcing SAFE state)
* TEST_MODE is operator-initiated only; the system never enters it automatically

---

### 10.11.10. Local Subsystem States

ESP32 #2 and Arduino Nano may maintain local temporary execution states internally.

Examples:

* sequence execution progress
* sensor acquisition phase
* local actuator timing

However:

* local states must not override global system state authority
* global authority always belongs to ESP32 #1

---

### 10.11.11. State Transition Authority

ESP32 #1 exclusively determines:

* state transitions
* recovery escalation
* automation permission
* safe mode activation
* emergency stop activation

Subsystems may request transitions by reporting:

* faults
* abnormal conditions
* recovery failures

---

### 10.11.12. Design Benefits

This architecture provides:

* predictable behavior
* centralized operational authority
* safer recovery logic
* cleaner firmware structure
* easier debugging
* consistent automation behavior
* reduced subsystem conflict


---

# 11. 🔷 Fault Authority Architecture

The system separates fault detection, fault classification, and fault response responsibilities to prevent conflicting recovery behavior between controllers.

---

## 11.1. Fault Detection Layer

ESP32 #2 is responsible for detecting actuator-level abnormalities during execution.

Examples:

* No flow detected
* Pump current abnormal
* Flow timeout
* Overcurrent condition
* Actuator execution failure

ESP32 #2 reports abnormalities to ESP32 #1 using structured fault messages.

ESP32 #2 may perform immediate local protective actions only when required for hardware safety.

Examples:

* stop pump during dangerous overcurrent
* disable actuator during short-circuit condition

---

## 11.2. Fault Classification Authority

ESP32 #1 is the single authority responsible for:

* final fault classification
* severity determination
* system-wide fault state
* recovery strategy selection
* operational restrictions
* GSM alert generation
* fail-safe state transitions

Fault classifications include:

* Minor Fault
* Major Fault
* Critical Fault

ESP32 #2 must not independently classify final system severity.

---

## 11.3. Fault Response Coordination

After fault classification:

ESP32 #1 determines:

* whether retry is allowed
* whether degraded operation is permitted
* whether irrigation/fertigation may continue
* whether emergency stop is required
* whether ESP32 #2 recovery actions are needed

ESP32 #1 then sends appropriate commands to ESP32 #2.

---

## 11.4. Local Protective Authority

ESP32 #2 is permitted to perform immediate temporary protective shutdowns only for actuator safety.

Examples:

* disabling a pump during dangerous current spike
* stopping actuator on impossible flow condition

However:

* final fault ownership still belongs to ESP32 #1
* ESP32 #1 determines long-term system behavior

---

## 11.5. Arduino Nano Fault Scope

The Arduino Nano does not classify faults.

The Nano may only:

* report invalid sensor readings
* report missing sensor status
* transmit raw measurement data

Fault interpretation belongs to ESP32 #1.

---

## 11.6. Design Philosophy

The system uses centralized fault authority with distributed fault detection.

Benefits:

* prevents conflicting shutdown logic
* simplifies debugging
* improves recovery consistency
* maintains predictable system behavior
* reduces firmware complexity
  
---

# 12. 📡 GSM Communication System (A7670C)

The A7670C GSM module is connected to ESP32 #1 (hardware UART2, GPIO27 TX / GPIO26 RX, baud auto-detected, 115200 first) and is used for outbound alerts/reports and inbound configuration/commands. A7670C texts ONLY for errors, warnings, scheduled-run notices, and reports — it is not a continuous telemetry channel (the full event stream lives in the SD log, Section 25).

> **2026-11-16 — outbound texts are now plain language (owner's request).** The compact comma formats in
> 12.1 (`ALERT,CRIT,...`, `RPT,...`, `ACK,...`, `ERR,...`, `SUM,...`) are no longer what the phone receives.
> Every outbound text fits ONE SMS (160 GSM-7 characters, no emoji/degree sign) and reads:
> line 1 `KIND: headline` (`CRITICAL` / `ALERT` / `WARNING` / `NOTICE` for faults; `OK` / `NOT DONE` / `BUSY` /
> `INFO` / `RUN` for replies and notices), then the detail in words, then what to do, then `(hh:mm CODE)` on
> alerts — the same code the LCD, website and SD log use. A held ESP2 fault is followed by a second text (owner
> only) listing the replies. The daily/STATUS report, SUMMARY and FULL SUMMARY go out as whole pages headed
> `TITLE i/N`. **Inbound command syntax is unchanged**; new: `HELP`, `HELP,RUN`, `HELP,CROPS`, `HELP,SYSTEM`.
> Refused commands still log `SMS_ERR|<code>` with the old codes (`AUTH`, `CANCEL|NO_RUN`, ...).
> Examples of every text: `Sugian Firm\SMS_Text_Samples_2026-11-16.md`. Code: ESP1 `main.cpp`, "SMS TEXT" block.

---

## 12.1. 🔹 Outbound Messages (System → User)

### 12.1.1. 🚨 Fault & Warning Alerts (All Three Tiers)

A7670C sends an SMS for ALL fault tiers (Critical 🔴, Major 🟠, Minor 🟡, per Section 23).

Special-case alert: when ESP32 #1 issues a Nano RESET_REQ software reset after 5 consecutive garbage packets (Section 18.9.5), an alert is sent. If RESET_REQ does not restore valid data, a Major alert is sent and ESP32 #1 continues on last-valid data (there is no hardware-reset layer).

Compact format examples:

ALERT,CRIT,FLOW_FAIL,MAIN,FERTIGATION,COL_B,reply STOP/RELEASE/IRRIGATE/NORMAL
ALERT,MAJ,NPK_FAULT,COL_B
ALERT,MIN,ULTRASONIC,MIX
ALERT,WARN,NANO_RESET,SOFT,GARBAGE5

A hard ESP32 #2 fault (FLOW_FAIL / PWR_FAIL / SAFE_STOP) carries the current operation (IRRIGATION/FERTIGATION) and column, and offers the user-gated recovery menu (Section 19.4.8.2): the system holds (ESP32 #2 stays alive with the mixing-tank volume retained) until the user replies STOP / RELEASE / IRRIGATE / NORMAL.

Note: Minor-tier alerts (e.g. ultrasonic ripple) may be frequent. Rate-limiting of Minor alerts can be added later if SMS volume becomes excessive; by current design all three tiers are sent.

---

### 12.1.2. 🚜 Scheduled Run Notice

When a scheduled irrigation/fertigation run begins, A7670C sends ONE consolidated SMS covering all columns scheduled in that run (not one SMS per column, not per sequence).

- If a single column is scheduled:
  RUN,COL_A,IRRIGATION
- If multiple columns are scheduled in the same run:
  RUN,COL_A+B+C,FERTIGATION

The notice fires once at the start of the scheduled run.

---

### 12.1.3. 📊 Daily Summary / On-Demand Report

A compact summary is sent automatically once per day at a configurable time (an editable constant in firmware; default unspecified — set during commissioning), and also on demand when the user texts STATUS (one keyword serves both live status and the report).

The summary is a compact encoded string intended for MANUAL expansion by the user (the full detail is in the SD log). Contents:

Per column (A, B, C):
- Plant name (if set, Section 12.2.3)
- N-P-K preset target (mg/kg) and pH target
- Total water used (today)
- Total nutrient volume dosed (mL, today)

System-wide:
- Faults today (count by tier)
- Nano resets today (count)
- Current system state (Section 10.11)
- Battery state, daily energy consumption, and charge (from INA226, Section 21.1)

Compact format example (encoding is expanded manually):
RPT,20260609,A:Lettuce,N150P40K200,pH5.8,W12.4,Nu85,B:...,C:...,FLT,C0M1m2,RST,1,ST,IDLE,BAT,78%,CONS,420Wh,CHG,150Wh

### 12.1.3.1. STATUS During Active Operation (Immediate Ack + Deferred Report)

ESP32 #1 must never block or stall an active irrigation/fertigation operation to answer a STATUS request, and must not lose the request. When STATUS is received while the system is actively operating (ACTIVE_STATE):

1. **Immediate brief acknowledgement** — ESP32 #1 replies right away with a short message confirming it is busy and reporting current progress (composing this reply only reads state; it does not interrupt the operation).
   Example: ACK,BUSY,FERTIGATION,COL_B,60%
2. **Deferred full report** — a "report pending" flag is set; once the operation completes, ESP32 #1 sends the full daily-style summary.

Rules:
- Duplicate STATUS requests during the same operation collapse to a SINGLE pending report (multiple texts do not stack multiple replies).
- The pending flag is non-persistent runtime state (Section 10.10.6); it is cleared after the report is sent, or dropped on reboot.
- Composing replies reads state only; it never pauses pumps, valves, dosing, or the work order in progress.

---

## 12.2. 🔹 Inbound Commands (User → System)

All inbound SMS use compact comma-separated formats. ESP32 #1 parses SMS into system actions and stores configuration in NVS (Section 10.10).

### 12.2.1. Nutrient Preset — Explicit (Default)

Sets the per-column target nutrient levels in mg/kg (N, P, K) plus target pH. This is the default preset mode.

SET,COL_A,N,150,P,40,K,200,pH,5.8

- Targets are in mg/kg to match the NPK sensor reading unit
- pH is stored per column and echoed in logs/receipts, but is **not** an automatically-corrected
  setpoint — no dosing logic reads it. Actual pH handling is validation-only against a fixed window
  (Section 14.2.4); the pH up/down pumps are manual/`TEST_PULSE`-only. See `CLAUDE.md`'s LOCKED
  nutrient dosing section.
- Preset applies per session, per column (each column may differ)

### 12.2.2. Nutrient Preset — Named Crop Preset

Selects a built-in crop preset stored in code. Used only when explicitly invoked with the PRESET keyword; otherwise the explicit format (12.2.1) is used.

SET,COL_A,PRESET,CARROT

- Built-in crop presets (name → target N-P-K mg/kg + pH) are stored in an editable, expandable table in the ESP32 #1 firmware
- The text does not need to carry mg/kg values when a named preset is used
- See Section 12.4 for the preset/calculation architecture

### 12.2.3. Plant Name

Assigns a persistent plant name to a column. The name remains until changed by the same command.

NAME,COL_A,Lettuce

- Stored in NVS; persists across reboots
- Used in logs and the daily summary

### 12.2.4. Mode & Emergency Commands

Mode is set PER COLUMN. There is no manual fertigation command (fertigation is automatic, Section 14.2.0).

**Owner-gating:** ALL inbound SMS commands (every command in the tables below, including STATUS/NET/
SUMMARY and the recovery replies STOP/RELEASE/IRRIGATE/NORMAL) require the owner number — `handleSms`
checks `senderIsOwner()` (sender's last 9 digits vs `PHONE_NUMBER`) first and replies `ERR,AUTH` to any
other sender. Internally-generated commands (empty reply target) pass.

| Command                   | Function                                          |
|---------------------------|---------------------------------------------------|
| MODE,COL_A,AUTO           | Column decides irrigate/fertigate by nutrient gap |
| MODE,COL_A,IRRIGATION_ONLY| Disable fertigation for this column (water only)  |
| THRESH,\<start\>,\<gap\> | Soil start % + fertigation gap mg/kg (also LCD + portal). The old form with a stop value is accepted and the stop value ignored |
| STOP,ALL                  | Emergency stop                                    |
| STOP                      | (while a fault is HELD, sec.19.4.8.2) acknowledge & keep holding — deliver nothing, wait |
| RELEASE                   | (held) dump the mixing-tank contents to the assigned column as-is (no top-up, no dosing) |
| IRRIGATE                  | (held) finish as irrigation only — top up plain water to budget, deliver, skip dosing |
| NORMAL                    | (held) resume the paused sequence from where it stopped (fill remainder, no re-dose) |
| STATUS                    | Live status + daily report (both)                 |
| SUMMARY[,\<day\>]         | Day-averages digest; day = (none/TODAY)\|YESTERDAY\|YYYYMMDD\|NODATE (Section 29.5) |
| FULL SUMMARY[,\<day\>]    | Hourly record + deduped errors + events + peaks (Section 29.5)        |
| NET                       | WiFi / ThingSpeak link status (Section 29.4)      |
| WIFI,\<ssid\>,\<pass\>    | Set WiFi credentials — owner-only, NVS (Section 29.4) |
| WIFIPORTAL                | Start the SoftAP WiFi-setup portal — owner-only (Section 29.4.1); `WIFIPORTAL,STOP` cancels |
| TSKEY,\<1\|2\|3\>,\<key\> | Set a ThingSpeak channel write key — owner-only (Section 29.4) |

(SUMMARY/FULL SUMMARY/NET/WIFI/TSKEY were added after the original catalog — full behavior in Section 29.4–29.5.)

---

## 12.3. 🔹 Configuration Handling

- SMS commands dynamically update system parameters
- Parameters (presets, plant names, mode) are stored in NVS and used by control logic
- Enables real-time tuning without reprogramming
- ESP32 #1 is the only controller that interprets GSM commands (Section 9.1, 10.1)

---

## 12.4. 🔹 Nutrient Preset & Dosing Calculation Architecture

The preset is expressed as a TARGET in N-P-K mg/kg (plus pH). ESP32 #1 calculates the required volume (mL) of each nutrient bottle to approach the target. This is OPEN-LOOP: the calculation uses known nutrient concentration constants and a pre-measured laboratory soil-nutrient baseline; it does not use live NPK sensor feedback during dosing. The NPK sensor reading is recorded as the measured OUTCOME (thesis data), not as a control setpoint.

Design requirements for the firmware:

* Nutrient concentration constants (how much N, P, K each bottle contributes per mL) MUST be defined as clearly-labeled editable constants at the top of the relevant source file, so they can be changed without touching logic.
* The laboratory-measured soil-nutrient baseline table (existing soil N-P-K, per column) MUST be editable in code.
* Built-in crop presets (name → target N-P-K mg/kg + pH) MUST be stored in an editable, expandable table.
* The system accepts BOTH the explicit mg/kg format (12.2.1) and the named-preset format (12.2.2); explicit is default, named is used only when PRESET is specified.

Calculation status: IMPLEMENTED (open-loop) per the companion `Nutrient_Dosing_Firmware_Spec.md` — gap formula `dose_mL = gap × batch_volume_L ÷ stock_conc`, whole-pulse quantization with a `MIN_DOSE_PULSES` floor, variable batch volume, and a 2× timed ceiling (`DOSE_TIMEOUT` backstop). The §3.1 arithmetic's exact unit/soil-mass basis and the diluted `STOCK_*` values are commissioning math. It is an open-loop approximation, not a closed-loop guarantee.

Note on nutrient bottles vs. elements: the physical system doses Nutrient A/B/C salts (Section 16; Nutrient D is unused). Each contributes multiple elements (Calcium Nitrate → N + Ca, MAP → N + P, Potassium Nitrate → N + K), and there is no pure-N source — so N is supplied jointly by A, B, and C. The preset is specified by ELEMENT (N-P-K, elemental, mg/kg) and the firmware computes the BOTTLE volumes; the concentration constants above define the element-per-mL contribution of each bottle. The dosing-calculation task must also handle the fertilizer-label reporting convention (P as P₂O₅, K as K₂O) versus the elemental mg/kg the NPK sensor reads.

---

## 12.5. 🔹 Design Principle

- Commands are short and compact for GSM efficiency
- Outbound messages are compact encoded strings for manual expansion
- ESP32 parses SMS into system actions
- Responses are structured for easy logging and interpretation
- GSM is reserved for alerts, scheduled-run notices, and reports — not continuous telemetry

---

# 13. ⚙️ System Modes

Irrigation vs. fertigation is decided PER COLUMN and AUTOMATICALLY, based on how far each column's measured soil nutrients are from its preset (Section 14.2.0). Fertigation is never a manual command — it happens only when the system determines the column's nutrients are sufficiently below target. A column can, however, be forced to irrigation-only.

Per-column mode flags:

| Per-Column Mode | Description                                                        |
|-----------------|-------------------------------------------------------------------|
| AUTO (default)  | System decides irrigate vs. fertigate by nutrient gap vs. preset  |
| IRRIGATION_ONLY | Fertigation disabled for this column; water only, regardless of gap |

System-wide operational modes (separate from the per-column flag above):

| Mode        | Description                |
|-------------|----------------------------|
| AUTO        | Fully automated operation  |
| MANUAL      | User-controlled via GSM    |

Per-column mode is set via SMS (applies to one column):
MODE,COL_A,AUTO
MODE,COL_A,IRRIGATION_ONLY

Note: there is no manual "force fertigation" command. Fertigation is always an automatic, sensor-driven decision (Section 14.2.0). The retained STOP,ALL emergency command and the system-wide AUTO/MANUAL modes are unchanged.

---

# 14. 🧠 Control Logic

The ESP32 Master Controller uses rule-based decision logic to determine irrigation, fertigation, safety actions, and power management behavior.

Control decisions are based on:

* Soil moisture readings
* Environmental conditions
* Tank levels
* Nutrient requirements
* Battery condition
* User GSM commands
* Time scheduling (RTC)
* Fault classification status

The system prioritizes safety, resource efficiency, and operational stability.

---

## 14.1. 🔷 Irrigation Decision Logic

The system continuously monitors soil moisture levels for each plant column.

---

### 14.1.1. Irrigation Trigger Logic


IF soil moisture < minimum threshold
→ Irrigation required


---

### 14.1.2. Irrigation Stop Logic


IF soil moisture > upper threshold
→ Stop irrigation


---

### 14.1.3. Hysteresis Control

To prevent rapid ON/OFF switching, irrigation uses separate start and stop thresholds. These are EDITABLE CONSTANTS in firmware (per column if needed); the values below are example defaults to be confirmed during commissioning, not final tuned values.

| Action           | Threshold (editable; example default) |
| ---------------- | ------------------------------------- |
| Start Irrigation | Below 35%                             |
| Stop Irrigation  | Above 45%                             |

This prevents:

* Relay chatter
* Pump stress
* Unstable watering cycles

---

### 14.1.4. Column-Based Operation

Each plant column operates independently.

Possible behaviors:

* Column A irrigating while Column B idle
* Fault in one column does not stop entire system

### 14.1.4.1. Per-Column Enable (3-Column Hardware, Subset Active)

The hardware supports 3 plant columns (A, B, C). The number of ACTIVE columns is an editable configuration (e.g. COLUMN_ENABLED[3]); columns may be enabled/disabled without code changes.

Current configuration: columns A and B active; column C DISABLED (hardware present on the board but column C sensors not yet connected).

Behavior of a disabled column:
* Skipped for irrigation, fertigation, and dosing
* Its sensors are NOT read by the Nano (avoids reading unconnected/floating inputs)
* OMITTED entirely from sensor packets — no SOIL field and no NPK packet for a disabled column (both SOIL and NPK are column-tagged, so omission is unambiguous)
* In the SD log, a disabled column is recorded as the literal token DISABLED in that column's field — NOT as 0 and NOT as the −1 sensor-fault sentinel, so "intentionally disabled" is distinguishable from "read zero" or "sensor failed"
* Re-enabling a column is a single configuration change (flip the enable flag); no structural code change required

All column-indexed structures (soil pins, valves P1/P2/P3, GSM presets COL_A/B/C, daily summary) remain defined for all 3 columns regardless of enable state.

---

## 14.2. 🔷 Fertigation Decision Logic

Fertigation is decided per column, automatically, and only allowed when all required conditions are valid.

---

### 14.2.0. Per-Column Irrigate-vs-Fertigate Decision

At a column's scheduled service time, ESP32 #1 decides irrigation vs. fertigation for that column:

```text
IF column mode = IRRIGATION_ONLY
  → Irrigate (water only)
ELSE (column mode = AUTO):
  Compare measured soil N-P-K (column 7-in-1, mg/kg) to the column preset
  IF any of N, P, K is below target by >= FERTIGATE_TRIGGER_GAP (with hysteresis)
    → Fertigate (dose toward preset; dosing amount per Section 12.4)
  ELSE
    → Irrigate (water only)
```

* FERTIGATE_TRIGGER_GAP and its hysteresis band are EDITABLE PLACEHOLDERS (<TBD>), set during commissioning. Hysteresis prevents a column flip-flopping between irrigate/fertigate near the boundary (same principle as irrigation hysteresis, Section 14.1.3).
* The soil NPK sensor is therefore BOTH a control trigger (decides whether to fertigate) AND the recorded outcome (thesis data). The dose AMOUNT remains open-loop (calculated from lab baseline + constants, Section 12.4), not corrected mid-dose; only the DECISION to fertigate is sensor-driven.
* Disabled columns (Section 14.1.4.1) are skipped entirely.

### 14.2.0.1. Per-Column Scheduling & Overlap

* Each column has its own schedule (when it is serviced).
* Schedules are independent; columns typically run at different times.
* If two columns' scheduled operations overlap, ESP32 #1 QUEUES them and runs them SEQUENTIALLY (one column completes before the next begins), because the mixing tank and delivery path are shared single-batch resources.

### 14.2.0.2. Post-Fertigation Flush (Water-Budget Split)

To clear nutrient remnants from the delivery lines and soil-surface discharge point while keeping total water added essentially unchanged, a fertigating column splits its water budget instead of adding extra water:

```text
Fertigating column water budget:
  - Deliver nutrient solution using (100% - FLUSH_PCT) of the column water budget
  - Then deliver FLUSH_PCT of the budget as PLAIN reservoir water (final flush step)

Irrigation-only column:
  - Deliver 100% of water budget, no flush (nothing to flush)
```

* FLUSH_PCT = EDITABLE CONSTANT, default 20% (percentage of the column's water budget).
* The flush is the LAST step so it pushes remnants through; plain water is sourced from the reservoir (no new hardware/valve).
* Total water delivered to a fertigating column is approximately unchanged versus irrigation (supports the water-conservation goal, Section 3).
* Flush sufficiency depends on line length/emitters; FLUSH_PCT is tuned at commissioning.
* Flush execution and timing are owned by ESP32 #2; the flush is logged as part of the fertigation session (CSV ACT/DOSE context, Section 25).

---

### 14.2.1. Fertigation Preconditions

```text id="jlwm2t"
IF:
- Column mode = AUTO and nutrient gap triggers fertigation (Section 14.2.0)
- Battery level acceptable
- Mixing tank available
- Required sensors operational
- No critical faults
THEN:
→ Allow fertigation
```

---

### 14.2.2. Nutrient Dosing Logic

Each nutrient is injected sequentially.

Process:

Start Nutrient Pump
↓
Measure Flow Pulses
↓
Convert Pulses → Volume
↓
Stop at Target Volume

Flow-to-volume conversion uses a per-sensor calibration constant (pulses-per-liter K-factor). These K-factors are EDITABLE CONSTANTS stored in ESP32 #1 NVS calibration values (Section 10.10.1) and used by ESP32 #2 during dosing; they MUST be easy to change without touching logic.

### 14.2.2.1. Mixing / Homogenization

After nutrients are dosed into the mixing tank, the mixer runs for a homogenization period before the booster pump delivers the mix to the columns, so the solution is uniform before EC/pH validation and delivery.

* Mixing duration: EDITABLE CONSTANT (placeholder — <TBD>, set during commissioning)
* EC/pH validation (Section 14.2.4) is performed after mixing stabilizes
* Mixer stabilization timing is owned by ESP32 #2 (Section 10.6.3)
* There is no automated pH correction to sequence against nutrient dosing: pH is validation-only
  (this section) and is never dosed. The "DEFINED-BUT-PENDING" status this line previously carried
  pointed at Section 12.4, which — checked directly — only ever defines the N/P/K gap formula and
  never defined a pH-correction dose; that pointer is now closed out as "not planned," not pending.
  See `CLAUDE.md`'s LOCKED nutrient dosing section for the current, authoritative statement.


---

### 14.2.3. Nutrient Fault Handling

IF nutrient channel fails
→ Disable affected nutrient only
→ Continue operation if safe


---

### 14.2.4. EC/pH Validation

During mixing:

IF EC or pH exceeds safe range
→ Stop dosing (executed locally by ESP32 #2 as a protective stop)
→ ESP32 #2 reports EC_FAIL / PH_FAIL to ESP32 #1
→ Continue irrigation only

Authority: ESP32 #2 holds LOCAL protective authority to stop dosing when EC/pH leaves the safe window (a hardware-safety stop, same class as overcurrent/no-flow). ESP32 #1 holds POLICY authority — it defines the safe-window thresholds and pushes them to ESP32 #2 at startup sync (Section 10.7); ESP32 #2 only enforces the limits it was given.

Safe-window thresholds (EDITABLE PLACEHOLDERS — set during commissioning, stored in ESP32 #1 NVS and distributed to ESP32 #2):

| Parameter | Lower Limit | Upper Limit |
| --------- | ----------- | ----------- |
| pH        | <TBD>       | <TBD>       |
| EC        | <TBD>       | <TBD>       |

Values intentionally left unset; they depend on the target crop and will be defined during commissioning.


---

## 14.3. 🔷 Environmental Adjustment Logic

Environmental sensors modify irrigation behavior dynamically.

---

### 14.3.1. DHT22 Usage

Temperature and humidity affect estimated water demand.

Example behavior:

* High temperature → increase irrigation duration
* High humidity → reduce irrigation duration

---

### 14.3.2. BH1750 Usage

Light intensity is used to estimate weather condition.

Example behavior:

* Low light/cloudy condition → reduce watering
* Very low light/rain indication → postpone irrigation

---

### 14.3.3. Sensor Failure Fallback


IF environmental sensor invalid
→ Use default irrigation parameters


---

## 14.4. 🔷 Tank Level Control Logic

The system manages both reservoir and mixing tank levels.

---

### 14.4.1. Reservoir Protection


IF reservoir level too low
→ Disable irrigation and fertigation


---

### 14.4.2. Mixing Tank Fill Logic

> **web v.4 (17 Nov):** the transfer runs for a time worked out from the litres needed, counted from the inverter
> switch-on. The tank then settles for a minute and is measured; more than 3 L short gives a top-up run. The 90 %
> overflow stop still guards the run. See §19.4.9.13.


IF mixing tank below target level
→ Start transfer pump (it draws straight from the reservoir; the reservoir valve is no longer fitted —
  P0 is a spare, and `RES_VALVE_FITTED` would open it here again)


---

### 14.4.3. Overflow Protection


IF mixing tank exceeds maximum level
→ Stop filling immediately


---

## 14.5. 🔷 Power-Aware Control Logic

System behavior adapts according to available power.

---

### 14.5.1. Battery Protection Logic

**REMOVED (operator request):** battery state no longer triggers any system action — no fertigation
disable, no stop, no emergency-stop, no alert, no exercise-skip, and INA226 dropout no longer reboots
ESP1. INA226 is retained for **monitoring only** (voltage/current/power on the LCD, `NET`, ThingSpeak,
and the daily energy Wh in reports). The table below is the historical behavior, now inactive:

| Battery State | System Action (INACTIVE) |
| ------------- | ------------------------ |
| Normal        | Full operation           |
| Low           | ~~Disable fertigation~~  |
| Critical      | ~~Stop all operations~~  |

---

### 14.5.2. Inverter Validation

```text id="j3hm43"
IF inverter ON but no AC detected
→ Trigger fault
```

---

### 14.5.3. Pump Validation via PZEM

PZEM is read by ESP32 #2 (Section 19.2.1). ESP32 #2 detects the abnormal-current condition locally and reports it to ESP32 #1, which classifies severity and decides the response.

```text id="42w3mf"
IF pump expected ON
AND current < 0.5A
FOR 30 seconds
→ Pump failure (ESP32 #2 detects → reports to ESP32 #1 → ESP32 #1 classifies)
```

---

## 14.6. 🔷 Flow Validation Logic

Flow sensors are used for closed-loop verification.

---

### 14.6.1. Main Irrigation Validation

```text id="1afm7l"
IF pump ON but no flow detected
→ Stop operation immediately
```

---

### 14.6.2. Impossible Flow Detection

```text id="b6jlwm"
IF flow exceeds expected range
→ Mark sensor or system fault
```

---

### 14.6.3. Dosing Precision Logic

Flow pulses are converted into estimated volume.

Purpose:

* Accurate nutrient injection
* Reduced over-fertilization
* Repeatable dosing behavior

---

## 14.7. 🔷 Fault-Aware Control Logic

Fault classification directly affects system behavior.

---

### 14.7.1. Critical Fault Logic (🔴)

→ Enter EMERGENCY_STOP
→ Disable ESP32 #2
→ Stop all outputs

---

### 14.7.2. Major Fault Logic (🟠)

→ Continue operation with restrictions
→ Disable affected subsystem

---

### 14.7.3. Minor Fault Logic (🟡)

→ Send GSM alert only
→ No operational interruption

---

## 14.8. 🔷 GSM Override Logic

GSM commands can modify system behavior remotely.

---

### 14.8.1. Remote Commands

Examples:


MODE,COL_A,IRRIGATION_ONLY
STOP,ALL
SET,COL_A,N,150,P,40,K,200,pH,5.8


---

### 14.8.2. Priority Rules

| Priority | Source               |
| -------- | -------------------- |
| Highest  | Emergency Stop       |
| High     | Critical Fault Logic |
| Medium   | GSM Manual Commands  |
| Lowest   | Automatic Logic      |

---

## 14.9. 🔷 Preventive Maintenance Logic

To improve long-term reliability:

---

### 14.9.1. Pump Exercise Routine

```text id="z89mjlwm"
IF pump inactive for 2 days
→ Run for 5 seconds
```

Purpose:

* Prevent seizure
* Prevent rust buildup
* Maintain readiness

---

## 14.10. 🔷  Data Logging Logic

Important system events are stored in microSD.

Logged data includes:

* Sensor readings
* Irrigation duration
* Nutrient usage
* Fault events
* Power statistics

All logs are timestamped using RTC.

---

## 14.11. 🔷  Fail-Safe Control Principle

Safety always overrides automation.

System behavior priority:

Safety
↓
Power Protection
↓
Fault Handling
↓
User Commands
↓
Automatic Operation

---

# 15. 🧠 Decision Logic (Overview)

The ESP32 Master determines system actions based on:

- Soil moisture thresholds
- Tank levels
- User-defined parameters (via GSM)
- Time scheduling (RTC)

Example:

IF soil moisture < threshold  
→ Start irrigation  

IF fertigation enabled  
→ Inject nutrients based on target concentration and flow measurement

---

# 16. 🧪 Nutrient System

| Label      | Chemical                | Status                          |
| ---------- | ----------------------- | ------------------------------- |
| Nutrient A | Calcium Nitrate         | Active (supplies N, +Ca)        |
| Nutrient B | Mono Ammonium Phosphate | Active (supplies N + P)         |
| Nutrient C | Potassium Nitrate       | Active (supplies N + K)         |
| Nutrient D | Magnesium Sulfate       | UNUSED — hardware retained, excluded from dosing logic (not part of NPK) |
| pH Up      | Alkaline solution       | Active                          |
| pH Down    | Acid solution           | Active                          |

Nutrient D (Magnesium Sulfate) supplies Mg/S, which are outside the N-P-K scope of this thesis. Its pump (PCF8575 P14) and flow sensor (GPIO25 on ESP32 #2) remain wired but are not driven by dosing logic. Only Nutrients A, B, and C participate in the N-P-K dosing calculation (Section 12.4).

Note: among the active stocks there is no pure-nitrogen source — N is supplied jointly by A, B, and C, so the dosing calculation must sum N contributions across all three.

---

# 17. 🔵 Arduino Nano — Sensor Hub

## 17.1. 🔹 Role

Handles all sensor readings and sends data via UART.

---

## 17.2. 🔹 Pin Configuration

### 17.2.1.  Communication

| Pin | Type    | Function      |
| --- | ------- | ------------- |
| D0  | UART RX | From ESP32 #1 |
| D1  | UART TX | To ESP32 #1   |

note:
UART lines between Nano and ESP32 #1 may require temporary disconnection during firmware upload or debugging.
---

### 17.3.1. Sensors (Digital)

| Pin | Function                            |
| --- | ----------------------------------- |
| D2  | Flow sensor (reservoir fill detect) |
| D3  | Pump-selector servo signal (SG90; LEFT 160° = transfer, RIGHT 90° = booster; §19.4.7.2). Power the servo from its own 5 V supply with a common GND. |
| D4  | Ultrasonic reservoir TRIG           |
| D5  | Ultrasonic reservoir ECHO           |
| D6  | Ultrasonic mixing TRIG              |
| D7  | Ultrasonic mixing ECHO              |
| D11 | DHT22                               |

---

### 17.3.2. RS485 (NPK Sensor)

| Pin | Function |
| --- | -------- |
| D8  | DE/RE    |
| D9  | RX       |
| D10 | TX       |

---

### 17.3.3. Analog Sensors

| Pin | Function       |
| --- | -------------- |
| A0  | Soil Column A1 |
| A1  | Soil Column A2 |
| A2  | Soil Column B1 |
| A3  | Soil Column B2 |
| A6  | Soil Column C1 |
| A7  | Soil Column C2 |

---

### 17.3.4. I2C (Corrected)

| Pin | Function     |
| --- | ------------ |
| A4  | SDA (BH1750) |
| A5  | SCL (BH1750) |

---

### 17.3.5. Other

| Pin | Function   |
| --- | ---------- |
| D12 | Reserved   |
| D13 | Status LED |

---

## 17.4. 🔹 System Reliability Considerations

The Arduino Nano functions as a sensor hub and operates continuously.

---

### 17.4.1. Internal Watchdog (Primary Self-Recovery)

The Arduino Nano uses its internal hardware watchdog (ATmega328 WDT) as its first and highest-priority recovery mechanism.

- Watchdog enabled with a short timeout (e.g. ~2–4 seconds)
- Watchdog is petted (reset) inside the non-blocking main loop
- If the main loop hangs or freezes, the Nano resets itself automatically with no involvement from ESP32 #1
- This handles true lockups/freezes locally and immediately

ESP32 #1-initiated recovery is intentionally LOWER priority than the Nano's own watchdog (see Section 18.9.5). ESP32 #1 only intervenes when the Nano is alive but transmitting persistently invalid ("garbage") data, which the Nano itself cannot detect.

Recovery priority order:

1. Nano internal watchdog (self-recovery from lockup) — highest
2. ESP32 #1 software reset request via UART (RESET_REQ) — after 5 consecutive garbage packets

(There is no third hardware-reset layer: PCF8575 P17 is now the master actuator-power cutoff, Section 19.4.8. If RESET_REQ fails, ESP32 #1 logs/alerts and continues on last-valid data.)

RESET_REQ mechanism: on receiving RESET_REQ over UART, the Nano enables its watchdog with a very short timeout and enters an infinite loop, causing the watchdog to perform a true hardware-level reset and restart the sketch from setup().

---

### 17.4.2. Dependency on ESP32 Master

- Arduino Nano does not control system flow
- ESP32 #1 validates incoming data
- Invalid or missing data is handled by ESP32 logic

---

### 17.4.3. UART Communication Stability

- Continuous data transmission to ESP32 #1
- Listens to ESP #1 for Day, Night, and Active.
- No blocking operations used
- Data sent at dynamic intervals

---

### 17.4.4. Data Intervals
- Data intervals goes every 10 seconds when actuator is active
- Morning when system is idle interval would be every 20 minutes
- Night when system is idle interval would be every 1 hour
- web v.4 (17 Nov): when idle, the Nano also sends a TANK line with every heartbeat (1 min by day, 2 min at night). Each
  ultrasonic waits up to 25 ms for its echo (`ULTRASONIC_TIMEOUT_US`), and each sensor is pinged every 100 ms. A DHT22
  read of exactly 0 °C / 0 % is sent as −1 (§19.4.9.19–20).

---

### 17.4.5. Design Philosophy

- Simple and stable operation
- Minimal processing load
- Delegates decision-making to ESP32 #1

---

# 18. 🟢 ESP32 #1 — Master Controller

## 18.1. 🔹 Role

Decision-making, communication, logging, UI, and power monitoring.

---

## 18.2. 🔹 Distributed Control with Power Management

ESP32 #1 not only sends commands but also:

- Controls power of ESP32 #2 via relay
- Can reset ESP#2 if communication fails
- Implements retry logic for command reliability

This enables:
- Fault recovery
- Energy-efficient operation
- Improved system robustness

---

## 18.3. 🔹 Communication (UART)

ESP32 has only three hardware UART peripherals (UART0, UART1, UART2). UART0 is the USB/boot/debug port and is KEPT FREE for the USB serial monitor (debugging is done via USB serial monitor and the LCD). The PZEM-004T has been relocated to ESP32 #2 (Section 19.2.1). With UART0 reserved for debug, two hardware UARTs remain for three devices (Nano, ESP32 #2, A7670C), so the Arduino Nano runs on software serial.

| Device       | TX | RX | Peripheral                |
| ------------ | -- | -- | ------------------------- |
| ESP32 #2     | 25 | 33 | UART1 (hardware)          |
| A7670C      | 27 | 26 | UART2 (hardware)  |
| Arduino Nano | 17 | 16 | SoftwareSerial            |
| (USB debug)  | 1  | 3  | UART0 (reserved for USB)  |

Notes:
- A7670C is kept on a hardware UART (proven working at GPIO27 TX / GPIO26 RX, baud auto-detected, 115200 first) because its async incoming-SMS bytes (+CMT) are timing-sensitive.
- ESP32 #2 is kept on a hardware UART because the command/response link controls actuators and is safety-relevant.
- Arduino Nano is placed on software serial because lost sensor packets are recoverable (the next packet arrives within the interval, and the <START>...<END> framing discards corrupted packets cleanly).
- Software serial for the Nano uses the EspSoftwareSerial (plerup) library; the legacy AVR SoftwareSerial will not work on ESP32.
- A bench test of Nano-on-software-serial packet loss under load (flow interrupts active, SIM + ESP2 active) is recommended before final deployment. Pending.

### 18.3.1. UART Mapping (Actual Wiring)

ESP32 #1 → ESP32 #2 (hardware UART1):

- ESP#1 GPIO25 (TX) → ESP#2 GPIO16 (RX)
- ESP#1 GPIO33 (RX) ← ESP#2 GPIO17 (TX)
- GND must be common

ESP32 #1 → A7670C (hardware UART2):

- ESP#1 GPIO27 (TX) → A7670C RX
- ESP#1 GPIO26 (RX) ← A7670C TX
- Firmware: HardwareSerial(2), begin(9600, SERIAL_8N1, 26, 27)

ESP32 #1 → Arduino Nano (software serial):

- ESP#1 GPIO17 (TX) → Nano D0 (RX)
- ESP#1 GPIO16 (RX) ← Nano D1 (TX)
- Firmware: EspSoftwareSerial instance on GPIO16 (RX) / GPIO17 (TX), 9600 baud

Note:
UART pins are configured via software and do not need to match physically.
UART0 (GPIO1/GPIO3) is intentionally left for the USB serial monitor; no device is wired to it.

---

## 18.4. ⚠️ UART Considerations

- ESP32 #1 hosts ESP32 #2 and A7670C on hardware UARTs; the Arduino Nano runs on software serial (EspSoftwareSerial)
- UART0 is reserved for the USB serial monitor (debugging)
- PZEM-004T is no longer on ESP32 #1 (moved to ESP32 #2, Section 19.2.1)
- Software must handle independent serial streams
- Avoid blocking reads to prevent data loss
- A7670C AT-command exchanges must remain non-blocking and tolerant of unsolicited incoming-SMS bytes (+CMT)
- Software-serial RX (Nano) is the weakest link under heavy interrupt load; rely on the <START>...<END> framing to discard corrupted packets and on the consecutive-garbage counter (Section 18.9.5) rather than reacting to single lost packets

---

## 18.5. 🔹 SPI (microSD)

| GPIO | Function |
| ---- | -------- |
| 23   | MOSI     |
| 19   | MISO     |
| 18   | SCK      |
| 5    | CS       |

---

## 18.6. 🔹 I2C Bus

| GPIO | Function |
| ---- | -------- |
| 21   | SDA      |
| 22   | SCL      |

I2C Devices with adress:

* LCD (0x27)
* INA226, secondary battery (0x40)
* INA226, solar (0x41), behind the ISO1540 (§21.0)
* INA226, main battery (0x44), behind the ISO1540 (§21.0)
* ADS1115 (0x48)
* DS3231 RTC (0x68)
* EEPROM (0x57)

---

## 18.7. 🔹 Buttons (Updated - No UART Conflict)

| GPIO | Button |
|------|--------|
| 0    | UP     |
| 12   | DOWN   |
| 13   | ENTER  |
| 15   | BACK   |
| 2    | MODE   |

- All buttons use INPUT_PULLUP configuration
- Button press connects GPIO to GND

---


## 18.8. 🔹 Relay Power Control (ESP32 #2)

| GPIO | Function                         |
|------|----------------------------------|
| 4    | Relay control for ESP32 #2 power |

- GPIO4 controls a relay that powers ESP32 #2
- Used for:
  - Power saving during idle
  - System recovery (power cycling ESP#2)
- Relay is active HIGH (via transistor driver)

- Because ESP32 #2 and the PCF8575 are power-dependent on GPIO4 relay control, cutting GPIO4 de-energizes the PCF8575 — including P17, the master actuator-power cutoff (Section 19.4.8). So cutting GPIO4 forces all actuators OFF as a hardware backstop. (The Nano hardware-reset pathway no longer exists; P17 was repurposed from Nano RESET to the master cutoff.)

### 18.8.1. Minimum on-time + idle heartbeat poll

- **Minimum on-time (10 s):** every ESP32 #2 power-up stays on at least 10 s. A graceful power-off while
  it has been on for less is deferred until the 10 s elapses (`esp2SetPower` defer + `esp2PowerTick`), so a
  too-brief power-up can never cut ESP32 #2 before it boots, ACKs, and heartbeats. Only the emergency stop
  cuts power immediately (`force`).
- **Idle heartbeat poll:** while idle (no scheduled irrigation/fertigation), if ESP32 #2 has not been heard
  for **10 minutes (day) / 1 hour (night)**, ESP32 #1 powers it up briefly for a health heartbeat, logs
  `IDLE_POLL|ALIVE`, then powers it back off (respecting the 10 s minimum). A poll with no reply runs the
  recovery ladder (RESET_SELF → power-cycle). This keeps ESP32 #2 proven-alive on a cadence without leaving
  it powered, and prevents false `ESP2_SILENCE` (which is gated on power state).

---

## 18.9. 🔹 System Reliability & Reset Strategy

ESP32 #1 is designed for continuous 24/7 operation with automatic recovery mechanisms.

### 18.9.1.  Software-Based Reset

The system can trigger a restart using:

ESP.restart()

Used when:
- Critical system errors occur
- Communication repeatedly fails
- System requires reinitialization after configuration updates

---

### 18.9.2.  Communication Failure Handling

ESP32 #1 monitors responses from ESP32 #2:

Flow:
- Send command → wait for response
- If no response within timeout:
  → Retry command (limited attempts)
- If still no response:
  → Trigger recovery action

---

### 18.9.3.  External Reset (Power Cycling ESP32 #2)

ESP32 #1 controls a relay connected to ESP32 #2 power supply.

Recovery process:
- Turn OFF relay (cut power to ESP#2)
- Wait for stabilization
- Turn ON relay (restart ESP#2)

Purpose:
- Recover from complete system freeze
- Handle communication lockups
- Improve system robustness

#### 18.9.3.1. Persistent link failure — ESP1-side re-init, cap, slow-retry, self-reset

Power-cycling ESP32 #2 only resets **ESP2's** state; it cannot fix a fault on **ESP32 #1's own UART**
(a wedged RX peripheral — framing-error/FIFO lockup) or a **broken physical link**. So repeated
power-cycles can loop indefinitely without restoring comm. The recovery ladder adds three defenses:

1. **ESP1 UART re-init on every attempt** (`esp2ReinitUart`): `esp2Serial.end()` → `begin()` → drain RX,
   run at the top of each power-cycle and before each `RESET_SELF`. This is the one link element that
   resetting ESP2 alone never touches.
2. **Fast-cycle cap → `ESP2_COMM_LOST` → slow-retry.** After **5** fast power-cycles in an episode
   (`ESP2_MAX_FAST_CYCLES`), ESP32 #1 stops hammering the relay: it latches `ESP2_COMM_LOST` (one Major
   alert), leaves the fast loop, and falls back to **one gentle re-init + power-cycle every 20 min**
   (`ESP2_SLOW_RETRY_MS`). A confirmed `READY` / startup validation clears the latch and sends
   `ESP2_COMM_OK`.
3. **Final escalation — one ESP1 self-reset/day.** Hitting the cap also triggers a full ESP32 #1
   `ESP.restart()` (shares the once-per-day RTC-RAM guard with the device-loss self-reset, so no boot
   loop), reinitializing every peripheral including UART1.

Hardware caveat: none of this fixes a physically broken link. Verify ESP1 TX(GPIO25)→ESP2 RX(GPIO16),
ESP1 RX(GPIO33)←ESP2 TX(GPIO17), a **common ground**, and 9600 baud.

---

### 18.9.4. Watchdog Timer

A watchdog timer be implemented to ensure ESP32 #1 resets if the main loop becomes unresponsive.

Purpose:
- Prevent system hang
- Ensure autonomous recovery

---

### 18.9.5. Reset Nano

ESP32 #1 resetting the Nano is a LOWER-priority recovery action. The Nano's own internal watchdog (Section 17.4.1) handles lockups/freezes first and automatically. ESP32 #1 only intervenes when the Nano is alive but persistently transmitting invalid ("garbage") data — a condition the Nano cannot detect about its own output.

#### 18.9.5.0. Garbage Data Definition

A received packet is classified as "garbage" if it fails EITHER tier:

Tier 1 — Structural garbage (malformed packet):
- Missing <START> or <END> marker
- Wrong field count for the command type
- Non-numeric characters where numbers are expected (random characters / line noise)
- Packet exceeds maximum length (128 bytes, Section 9.9.2)
- Unrecognized command keyword

Tier 2 — Semantic garbage (parses, but impossible values):
- Soil moisture < 0% or > 100%
- Temperature outside −10°C to 70°C, or humidity outside 0–100%
- Tank level < 0% or > 100%
- Negative light (lux) reading
- Stale or duplicate timestamps
- Impossible sensor combinations that persist

NOT garbage:
- The NPK −1 sentinel (and similar honest "invalid read" sentinels). These indicate the Nano is correctly reporting a failed sensor read, not malfunctioning. Resetting the Nano cannot fix a faulty sensor, so sentinels must not trigger reset.

#### 18.9.5.0.1. Consecutive Garbage Counter

- A counter increments on each garbage packet (Tier 1 or Tier 2)
- The counter resets to zero on a single cleanly-parsed, plausible packet
- Only when the counter reaches 5 CONSECUTIVE garbage packets does ESP32 #1 escalate to recovery

This prevents a single noise burst or transient bad reading from triggering a reset. A genuine Nano malfunction produces a sustained run of garbage; transient line noise recovers within one or two packets.

Note: "UART timeout persists" is NO LONGER a Nano-reset trigger. If the Nano goes silent, its own internal watchdog restarts it (Section 17.4.1). ESP32 #1 may still log/alert on silence, but does not reset on silence alone.

#### 18.9.5.0.2. Recovery Escalation Order

1. Nano internal watchdog — self-recovery from lockup (highest priority, automatic)
2. ESP32 #1 software reset request (RESET_REQ over UART) — triggered after 5 consecutive garbage packets. Used because the Nano cannot detect its own garbage output.

There is NO third (hardware-reset) layer. The former PCF8575 P17 → Nano RESET path was REMOVED when P17 was repurposed to the master actuator-power cutoff (Section 19.4.8). If RESET_REQ fails to restore valid data, ESP32 #1 logs/alerts (Major) and continues on the most recent valid data; there is no further automatic Nano reset (deliberate 2-layer ladder).

#### 18.9.5.0.3. Daily Fresh-Start Reset

Independent of fault handling, ESP32 #1 sends RESET_REQ to the Nano once per day to ensure a clean daily restart (proactive hygiene, not fault-driven). This uses the software-reset path (Layer 2 mechanism).

---

#### 18.9.5.1. External Nano Hardware Reset — REMOVED

The former external Nano hardware-reset path (ESP32 #1 → ESP32 #2 → PCF8575 P17 → transistor → Nano RESET pin) has been REMOVED. PCF8575 P17 is now the **master actuator-power cutoff** (Section 19.4.8), and the Nano RESET pin is no longer driven by the system. The `RESET_NANO` command is retired.

Consequence: the Nano recovery ladder is now TWO layers — its internal watchdog (lockups) and the RESET_REQ software reset over UART (garbage output). A Nano that is alive but persistently producing garbage AND does not respond to RESET_REQ can no longer be hardware-reset automatically; ESP32 #1 logs/alerts (Major) and continues on last-valid data. This reduction was accepted deliberately when P17 was repurposed.


---

## 18.10. 🔹 LCD User Interface & Backlight Behavior

The LCD interface is designed to improve usability, reduce unnecessary power consumption, and provide clear visual feedback to the user.

### 18.10.1. LCD Backlight Control

The LCD backlight operates dynamically based on system activity and user interaction.

Default behavior:

* Backlight remains OFF during idle state
* Backlight automatically activates during user interaction or important events
* Backlight automatically turns OFF after inactivity timeout

This behavior reduces:

* Unnecessary power consumption
* Night-time glare
* Continuous backlight wear

---

### 18.10.2. Backlight Wake Conditions

The LCD backlight automatically turns ON when:

* Any front-panel button is pressed
* GSM command is received
* Irrigation or fertigation sequence starts
* System mode changes
* Fault or warning condition occurs
* User enters menu navigation

---

### 18.10.3. Timeout Behavior

| Event Type         | Backlight Behavior       |
| ------------------ | ------------------------ |
| Button Interaction | ON for 10 seconds        |
| Active Operation   | ON during activity       |
| Fault/Warning      | Remains ON until cleared |
| Idle State         | OFF                      |

---

### 18.10.4. Fault Indication Behavior

For important system abnormalities:

* LCD backlight may blink slowly to attract attention
* Fault information is displayed immediately
* User acknowledgment clears visual alert state

Examples:

* Pump failure
* No flow detected
* Critical battery level
* Sensor failure

---

### 18.10.5. Night-Time User Experience

The system minimizes unnecessary light emission during night operation.

Possible strategies include:

* Shorter backlight timeout during night hours
* Reduced activation frequency
* Automatic backlight suppression during inactive nighttime periods

Night-time detection may utilize:

* RTC time reference
* BH1750 light intensity readings

---

### 18.10.6. Design Philosophy

The LCD interface prioritizes:

* Minimal power consumption
* Clear user feedback
* Professional embedded-system behavior
* Reduced user distraction
* Improved long-term usability

The interface is designed to emulate professional industrial control systems while remaining simple and intuitive for end users.

---

### 18.10.7. LCD Menu — Configuration Capabilities

The LCD + front-panel buttons allow on-device configuration without SMS. All values edited here write to the SAME ESP32 #1 NVS configuration used by SMS commands, so LCD and SMS stay in sync (last write wins).

#### 18.10.7.1. Clock & Date

* RTC (DS3231) clock and date are editable from the LCD.
* Correct date is important: daily log filenames (YYYYMMDD.CSV, Section 25.2) and the daily-summary scheduling depend on it.

#### 18.10.7.2. Per-Column Settings (two independent axes)

The LCD edits two SEPARATE per-column settings that do not interfere:

A. **Schedule axis** — `SCHEDULE_MODE` per column:
   * AUTO → use the default/original schedule
   * MANUAL → operator-set service time. A manual schedule is PERPETUAL: it does not reset after a run completes; it persists until set back to AUTO.

B. **Mode axis** — column `MODE`:
   * AUTO → system decides irrigate vs. fertigate by nutrient gap (Section 14.2.0)
   * IRRIGATION_ONLY → water only, fertigation disabled for this column
   * OFF → column disabled entirely (equivalent to COLUMN_ENABLED = false, Section 14.1.4.1)

These two axes are independent: e.g. a column may be MODE=IRRIGATION_ONLY with SCHEDULE_MODE=MANUAL at the same time.

#### 18.10.7.3. Per-Column Preset (named or manual)

* Named preset: select a built-in crop preset (e.g. CARROT) — mirrors SMS `SET,COL_x,PRESET,name`.
* Manual preset: set N, P, K (mg/kg) and pH targets, and the fertigate trigger threshold — mirrors SMS `SET,COL_x,N,..,P,..,K,..,pH,..`.

#### 18.10.7.4. Note on Enabling a Column via OFF/ON

Setting a column from OFF to an active mode sets COLUMN_ENABLED = true, which causes the Nano to begin reading that column's sensors. If the column's hardware is not physically connected (e.g. column C currently), enabling it will produce floating/garbage reads until the sensors are wired. This is an accepted operator responsibility; the LCD does not physically verify sensor presence.

> Implementation note (current firmware): the LCD OFF/ON toggle changes **ESP32 #1's** COLUMN_ENABLED only — it gates ESP32 #1 automation (irrigation/fertigation/dosing) and persists to NVS. The **Arduino Nano keeps its own compiled COLUMN_ENABLED**, so toggling here does not yet change which columns the Nano physically reads. Making the Nano follow at runtime requires a dedicated ESP32 #1 → Nano `ENABLE,<col>,<0|1>` command (not yet implemented).

> **Forced run (17 Nov):** arming a forced run on the LCD for a column that is OFF switches it back ON first. The
> editor warns before ENTER: `Col A   OFF, ARM=ON` (§19.4.9.26). The website refuses a forced run on an OFF column
> and says how to switch it on.

---

### 18.10.8. Testing Mode (Manual Bench Control)

Testing mode is a manual bench-test feature for exercising individual actuators directly. It is a DELIBERATE bypass of normal automation and safety, intended for use only while the operator is physically present and watching.

#### 18.10.8.1. Entry / Exit

* Entered and exited via an LCD menu item.
* On entry, the system first forces a SAFE state (all relays OFF, all valves closed).
* While in testing mode: automation is disabled, scheduled runs are suppressed, and no automatic irrigation/fertigation occurs.
* GSM alerts remain active.
* This is a distinct global state: `TEST_MODE` (Section 10.11).
* Both ESP32 #1 and ESP32 #2 are aware they are in testing mode; ESP32 #2 must be powered ON (GPIO4 relay enabled).

#### 18.10.8.2. Momentary (Dead-Man) Relay Control

* ONE relay is selected at a time via the LCD; ALL relays (pumps, valves, mixer) are reachable.
* The ENTER button acts as a momentary power button: relay turns ON while ENTER is held, OFF when released.
* Mechanism: while ENTER is held, ESP32 #1 continuously sends an "ON-hold" signal to ESP32 #2 for the selected relay; on release, ESP32 #1 stops sending it.

#### 18.10.8.3. ESP32 #2 Sole Safety Authority in Testing Mode

The dead-man safety lives ENTIRELY on ESP32 #2 and does not depend on ESP32 #1 behaving correctly:

* ESP32 #2 keeps the relay ON only while it is actively receiving the ON-hold signal from ESP32 #1.
* If the ON-hold signal stops (button released, OR ESP32 #1 hangs / link drops), ESP32 #2 turns the relay OFF within a short timeout (fail-safe).
* HARD CAP: ESP32 #2 turns the relay OFF after a maximum of **30 s** continuous ON for a single relay (`TEST_HARD_CAP_MS`; 10 s originally) and **60 s** for the valve + pump priming combos (`TEST_COMBO_CAP_MS`), regardless of whether ESP32 #1 is still sending ON-hold. The Fill Res>Mix and Push>Col combos get **1 minute of inverter-on time** (§19.4.9.14). This caps a stuck-ON condition even if both the release and the link fail.
* The ON-hold timeout is 400 ms (`TEST_HOLD_TIMEOUT_MS`), and 1.2 s for rows that switch the inverter (§19.4.9.20). After a release the next press is ignored for 1.5 s (§19.4.9.16), and ESP32 #1 counts a release only after the button has been up for 120 ms (§19.4.9.18).

#### 18.10.8.4. Full Safety Bypass (operator is the safety)

In testing mode, normal protective logic is INTENTIONALLY bypassed — including no-flow, overcurrent, EC/pH, PZEM, and battery protections. The ONLY active protections are:

* the dead-man release (relay off when button released / signal lost), and
* the hard caps on ESP32 #2 (§18.10.8.3).

WARNING (documented operator responsibility): bypassing hardware-protection stops means manual testing can damage equipment (e.g. dry-running a pump, stalling a motor). Testing mode must never be left unattended; the operator holding the button is the safety. Use brief presses; do not hold a dry pump.

#### 18.10.8.5. Logging

All manual actuations in testing mode are logged to SD (relay/device, ON/OFF, duration), as ACT events (Section 25.2.1), so bench tests are recorded.

---

# 19. 🟠 ESP32 #2 — Actuator Controller

## 19.1. 🔹 Role

Executes commands, controls pumps/valves, performs safety monitoring.

---

## 19.2. 🔹 Communication

| GPIO | Function         |
| ---- | ---------------- |
| 16   | RX from ESP32 #1 |
| 17   | TX to ESP32 #1   |


---

### 19.2.1. 🔹 PZEM-004T AC Power Monitor (Relocated from ESP32 #1)

The PZEM-004T was moved from ESP32 #1 to ESP32 #2 to resolve the ESP32 #1 four-UART conflict and to align power validation with the controller that actually drives the pumps.

| GPIO | Function       |
| ---- | -------------- |
| 13   | TX to PZEM     |
| 14   | RX from PZEM   |

- Runs on ESP32 #2 hardware UART1, remapped to GPIO13 (TX) / GPIO14 (RX), 9600 baud
- GPIO13 and GPIO14 are output-capable and not boot-critical (suitable for UART)
- ESP32 #2 hosts ESP32 #1 link on UART2 and PZEM on UART1 — both hardware UARTs, no software serial required

Rationale and authority:

- PZEM validates that pumps draw current when commanded ON; pumps are controlled by ESP32 #2, so detection now lives with the actuator controller (consistent with the fault-detection layer in Section 11.1)
- ESP32 #2 reads PZEM locally, detects electrical abnormalities (no current when pump ON, overcurrent, out-of-range voltage), and reports them to ESP32 #1
- ESP32 #1 retains fault classification and response authority (Section 11.2); it sets thresholds and decides system-wide response
- PZEM polling is performed on ESP32 #2's schedule, primarily during active pumping; PZEM UART is hardware, so it is unaffected by flow-interrupt timing

## 19.3. 🔹 I2C (PCF8575)

| GPIO | Function |
| ---- | -------- |
| 21   | SDA      |
| 22   | SCL      |

---

## 19.4. ⚙️ Control Strategy

### 19.4.1. 🔹 PCF8575 Control Behavior

- All outputs default HIGH (OFF)
- Active LOW logic:
  - LOW → ON
  - HIGH → OFF

- Used for:
  - Pumps
  - Valves
  - Mixer

### 19.4.2. Preventive Motor Exercise Routine

To prevent pump seizure due to inactivity:

- If pump idle for 2 days:
  → Run pump for 5 seconds

Applies to:
- AC Pumps (Transfer / Booster)
- Mixer Motor

Purpose:
- Prevent rust buildup
- Avoid mechanical seizure
- Improve long-term reliability

### 19.4.3. Pump & Valve Control

* All actuators are controlled via PCF8575
* ON/OFF control only (no PWM required)

### 19.4.4. Dosing Method (Flow-Based)

Start Pump
↓
Measure Flow Pulses
↓
Convert to Volume
↓
Stop at Target Volume

---

### 19.4.5. 🔹 Flow Sensors (Interrupt-Based)

web v.4: the two main-line meters below are **not read** (no interrupt is attached to their pins); the
transfer / booster water is metered by the tank ultrasonics (§19.4.9). In the firmware the pins are GPIO26
(ResMix) and GPIO5 (MixIrr); the table is the original plan.

| Function                | GPIO |
| ----------------------- | ---- |
| Reservoir → Mixing Tank | 4 (unused, web v.4) |
| Mixing → Irrigation     | 5 (unused, web v.4) |
| Nutrient A              | 18   |
| Nutrient B              | 19   |
| Nutrient C              | 23   |
| Nutrient D              | 25   |
| pH Up                   | 26   |
| pH Down                 | 27   |

* Flow sensors operate in sequence (not simultaneous) to avoid interrupt overload.
* Flow sensors are **stage-based (not simultaneous)**
---

### 19.4.6. 🔹 Analog Sensors

| Sensor                | GPIO |
| --------------------- | ---- |
| pH                    | 32   |
| EC                    | 33   |
| Mixer Motor Current   | 36   |
| Spare                 | 39   |

---

### 19.4.7. 🔹 PCF8575 Output Mapping

Current map (the relay board was rewired on 2026-11-14; the pump relays were retired and P0/P5/P6 moved on 2026-11-15; nutrient A and B moved to P1 and P7 on 2026-11-16). It matches `OUT_*` in `ESP2/src/main.cpp`.

| Output | Device            |
| ------ | ----------------- |
| P0     | DPDT board-supply select: de-energized = main battery, energized = secondary battery (was the transfer pump relay; the battery select was on P6 until 2026-11-15. §19.4.7.1 still says "P4" from before the remaps) |
| P1     | Nutrient A Pump (moved off P11 on 2026-11-16: the P11/P12 relays never ran A/B; was the booster pump relay before that) |
| P2     | Inverter Relay — the only AC switching relay; it powers whichever pump the servo has selected |
| P3     | Column A Valve    |
| P4     | Column B Valve    |
| P5     | **SPARE** — was the Column C Valve until 2026-11-15; `OUT_RES_VALVE` if `RES_VALVE_FITTED` puts a reservoir valve back |
| P6     | Column C Valve (moved from P5 on 2026-11-15) |
| P7     | Nutrient B Pump (moved off P12 on 2026-11-16, same reason; was the spare reservoir-valve relay) |
| P10    | Mixer Motor — **12 V DC**, no inverter |
| P11    | **SPARE** — Nutrient A until 2026-11-16; check that relay and its wiring before reusing it |
| P12    | **SPARE** — Nutrient B until 2026-11-16; same |
| P13    | Nutrient C Pump   |
| P14    | Nutrient D Pump   |
| P15    | pH Up Pump        |
| P16    | pH Down Pump      |
| P17    | **Master Actuator-Power Cutoff** (gates power to the P0–P16 relay bank; see Section 19.4.8) |

Note: P17 is no longer the Nano hardware-reset line. It is now a master cutoff that controls power to ALL control relays (P0–P16). Consequently the Nano no longer has a hardware-reset path — its recovery is limited to its internal watchdog and the RESET_REQ software reset (Section 18.9.5, recovery ladder reduced to 2 layers).

**Why the pump relays (P0/P1) were retired (2026-11-15).** The AC pump load was wired through these relays' contacts. The AC induced a voltage into the relay coil circuit, and that coupled onto the shared low-voltage wiring and disturbed **every** flow-sensor input, giving false or missed pulses and therefore wrong litres on every metered stage. The pumps now have no relay of their own. A changeover switch, flipped by the Nano's D3 servo, routes the inverter's output to one pump (§19.4.7.2). P0 has since become the battery select; nothing drives the spares (P5, and P11/P12 since 2026-11-16) except a raw Testing hold.

### 19.4.7.2. 🔹 Pump Selection by the Nano D3 Servo (2026-11-15)

**Rule:** the servo only ever moves with the inverter OFF and its output dead. The changeover contacts therefore never make or break load current and cannot weld. ESP32 #2 asks for the servo position again before **every** inverter start; it never trusts an "already there".

Sequence (ESP32 #2's `acPumpTick`, non-blocking; the Nano is reached through ESP32 #1):

1. Open the stage's valves (column valve for a drain, the reservoir valve if fitted for a fill).
2. Inverter (P2) OFF, then wait `INV_DECAY_MS` (2 s [MEASURE]) from the moment it actually went off.
3. PZEM dead-check: no reply, or under 50 V. If the AC is still live 5 s later → `SERVO_FAIL,…|AC_LIVE`.
4. ESP32 #2 → `SERVO_REQ,<TRANSFER|BOOSTER>,<seq>` → ESP32 #1 → Nano `FAST`, then `SERVO,<LEFT|RIGHT>,<seq>,*CRC` (resent every 1.5 s).
5. The Nano ramps at 1° per 20 ms, settles for 300 ms, detaches the servo and replies `SERVO,<side>,OK,<seq>`. ESP32 #1 forwards `SERVO,<pump>,OK,<seq>`; only the current seq counts.
6. After `SERVO_POST_MS` (300 ms): inverter ON, and the pump runs. ESP32 #2 sends `PUMP,<pump>,ON`.
7. When the stage ends: inverter OFF, then `PUMP,OFF`. The servo stays where it is.

If no OK arrives within 10 s, the run **holds** with `SERVO_FAIL`. The inverter never came on and nothing is credited to the tank. An exercise, Prime or Testing combo just ends and raises an alert instead.

**Level watch while a pump runs.** From `PUMP,ON` until `PUMP,OFF`, ESP32 #1 relays each FAST TANK reading to ESP32 #2 as `LEVEL,<mix%>,<res%>`. It sends -1 only when there is no valid reading (lost echo, Nano silent).

- **Filling** (transfer pump): 3 readings in a row at or over `MIX_HIGH_PCT` (90 % [TBD]) stop the fill. The metered litres are kept, `LEVEL_HIGH` is raised (Major), and the run **carries on**. No valid reading for 3 s (after a 3 s start grace) → the fill **holds** as `LEVEL_BLIND`.
- **Draining** (booster pump): a blind period only raises `DEGRADED,LEVEL_BLIND`; the run continues on the flow meter. *(web v.4: the rest of the drain runs on a timer instead, §19.4.9.2.)*
- **Source ran dry — the flow meter decides, not the ultrasonic (2026-11-15).** *(web v.4: the ultrasonic decides — the level stops moving, §19.4.9.2.)* On a fill or a drain, once at least `FLOW_DRY_MIN_L` (0.3 L [CONFIRM]) has flowed, `FLOW_DRY_MS` (5 s [CONFIRM]) with no flow pulse means the tank feeding the pump is empty. The pump stops even if the ultrasonic still shows water.
  - **Drain** (`DELIVER` / `FLUSH_DELIVER`): the mixing tank counts as empty (`mixTankL` = 0) and the run finishes normally, or goes on to the flush. ESP32 #2 sends `INFO,MIX_EMPTY,<MAIN|FLUSH>|delivered=<L>|expected=<L>`; it is logged, not a fault.
  - **Fill** (`FILL` / `FLUSH_FILL`): the litres that came in are kept, Major `RES_EMPTY,TRANSFER|metered=<L>|target=<L>` is raised (SMS), and the run **carries on** with the smaller batch (user decision). A fertigation batch can then be stronger than planned.
  - A trickle under `FLOW_DRY_MIN_L` that stops is still `FLOW_FAIL` after `FLOW_STALL_MS` (10 s), and no flow at all is `FLOW_FAIL` after `FLOW_TIMEOUT_MS` (10 s). Flow-blind (timed) recovery stages do not use this rule.
- **Backstop:** ESP32 #1 checks for itself. If the mixing tank reads 97 % or more on 3 readings and the transfer pump is still on 3 s later, it raises Critical `MIX_OVERFLOW`. That E-stops the rig: STOP_ALL, then ESP32 #2's power is cut.
- **Commissioning / tank geometry:** the percentages use ESP32 #1's tank geometry, measured on 2026-11-15 as **empty 42.9 cm, full 24.0 cm for both tanks** (sensor-to-water). That is an 18.9 cm span, so 1 cm is about 5.3 %. On the mixing tank the 90 % fill stop is at about 25.9 cm and ESP32 #1's 97 % backstop at about 24.6 cm. ESP32 #1 reads them from NVS keys `reCm2` / `rfCm2` / `meCm2` / `mfCm2`, which were renamed from `reCm`… so that older saved values no longer override the measured defaults. Calibration "Ultra … empty" re-captures EMPTY only; it is refused unless it is at least 2 cm farther than FULL. FULL has no calibration step. The Nano only sends raw cm. An uncalibrated sensor still gives a percentage; it does not read as blind.

**Stuck-servo check (2026-11-15; web v.4 replaces the sensors with the tank levels — see §19.4.9.3).** While the inverter powers a pump, ESP32 #2 also counts the OTHER pump's flow sensor:
- Transfer selected → it watches the irrigate sensor (FLOW_MIX_IRR, GPIO5).
- Booster selected → it watches the transfer sensor (FLOW_RES_MIX, GPIO26).

If that sensor reaches `CROSS_PULSE_MIN` pulses (20, about 44 mL [MEASURE]) and has counted more than the expected sensor, the servo stuck or slipped and the wrong pump is running. ESP32 #2 then does this:

1. Turns the inverter off at once.
2. Books the water the wrong pump moved to the mixing-tank volume: a stuck booster drained it, a stuck transfer filled it.
3. Reports `SERVO_MISMATCH,<pump>|cross=..|L=..|try=n/2` (Major alert).
4. After the normal decay and dead-check, makes two full servo swings (other side, back, other side, back).
5. Turns the inverter on again and checks once more.

This happens at most twice per pump start. A third detection holds the run as `SERVO_FAIL,…|STUCK|cross=..`, crediting the litres the expected sensor did meter. During a retry the stage is paused: its cap, no-flow timeout and timed-stage clocks leave out the pause. The Nano stays in FAST throughout.

Limit: during a fill the column valves are closed, so a stuck booster may barely move water and the irrigate sensor may see nothing. That case still ends as `FLOW_FAIL` (expected sensor at 0 pulses).

**Testing.** The bare Inverter row (P2) switches the inverter on directly (user override, 2026-11-15). It runs **whichever pump the servo happens to be on**, with no servo step, no valve, no level watch and no stuck-servo check; only the 400 ms dead-man and the 30 s single-relay cap protect it. Check the servo position before holding it. The Fill and Push>Col combos remain the safe way to run a pump: they include the servo step, so a pump starts a few seconds after the button. Turning the bare row off starts the inverter decay clock, so a servo move right after it still waits `INV_DECAY_MS`.

**Servo-only test (2026-11-15).** Two Testing rows move the pump selector with **no pump running**: `Servo>Transfer` (row 20, LEFT) and `Servo>Booster` (row 21, RIGHT). They are on the LCD Testing list and in the dashboard's timed test as `servoL` / `servoR`. **Website switch (2026-11-16):** Manual/Test has a **Pump selector switch** whose Transfer / Booster buttons send the same pulse (15 s). ESP1 ends a servo pulse as soon as the move is settled, answering `SERVO OK: selector on the <TRANSFER|BOOSTER> side` (completed) or `SERVO FAIL: <why>` / `SERVO NOT CONFIRMED: ...` (failed); the old answer was cut at 47 characters before the side's name. ESP1 publishes the side the Nano last confirmed (any move: a run's, a Testing row's or the website's) as `system.servoSide`, with `servoSideAgeMs`, and keeps it in NVS `svside` over a restart.

1. Holding the row makes ESP32 #2 run the normal safe path: inverter OFF, decay, AC dead-check, then `SERVO_REQ`.
2. It stops there with the inverter still off and replies `TEST,SERVO,<pump>,OK` once the Nano confirms.
3. The LCD then shows `[OK]` on that row. A dashboard pulse result reads `SERVO OK` or `SERVO NOT CONFIRMED`.

Hold the row, or pulse for, at least about 5 s: the decay wait and the move take that long. The Mixer combo moved from row 20 to row 22 (dashboard only).

**Calibration mode (verified 2026-11-15; web v.4: the "Flow ResMix" / "Flow MixIrr" rows are removed — the same RESMIX / MIXIRR prime is now run from "Tank Mix liters" with UP / DOWN, §19.4.9.4).** "Flow ResMix" and "Flow MixIrr" (MixIrr primes through column A) hold ENTER to run the pump. That sends `PRIME_START` to ESP32 #2, which runs the same `acPumpStart` path: inverter off, AC dead-check, servo, then inverter on. The pump therefore starts a few seconds after ENTER is pressed; keep holding.
- The flow sensor being calibrated keeps counting throughout.
- The stuck-servo check watches the other pump's sensor.
- The overflow stop applies to a ResMix prime.

A prime that ends on its own (the 45 s cap, a servo failure, or the overflow limit) now stays off until ENTER is released. Before this, ESP32 #1's keep-alive re-armed it straight away, so the cap never really held.

Two limits:
- Any Major alert during Calibration (`SERVO_MISMATCH`, `SERVO_FAIL`, `LEVEL_HIGH`) closes Calibration and powers ESP32 #2 off, as every fault does. Reopen the target and run again.
- Prime water is not added to ESP32 #2's mixing-tank volume. After a ResMix calibration, drain the tank or expect the next fill's 90 % stop to end it early.

**Nano resets while pumping.** A garbage-triggered RESET_REQ, the daily Nano reset and the dashboard's "Reboot Nano" all wait until no pump is running and the servo isn't moving. A Nano watchdog reset during a fill still blinds the level for its ~2 s boot plus up to 5 s until the next FAST, so the fill holds as `LEVEL_BLIND`. That is the safe direction; RESUME NORMAL continues the run.

**Mixer.** It is 12 V DC (P10 relay only). MIX no longer turns the inverter on, and the PZEM checks no longer apply to it; the ACS712 limits must be re-measured for the DC motor.

### 19.4.9. 🔹 Pure Ultrasonic Water Metering (web v.4, 2026-11-16)

**What changed.** The ResMix (reservoir → mixing tank) and MixIrr (mixing tank → column) flow meters are no longer used anywhere in the firmware: no interrupt on GPIO26 / GPIO5, no K-factors (`KMAIN`, `SET_CAL,FLOW_RESMIX/MIXIRR`, NVS `kRM` / `kMI` are gone or ignored), no "Flow ResMix" / "Flow MixIrr" calibration rows, and no ResMix / MixIrr pages in Sensor Diag. Every transfer and delivery is metered on the tank ultrasonics, **in litres**: a delivery on the mixing tank's fall; a transfer on the **reservoir's drop** with the mixing tank as the witness (§19.4.9.7). The nutrient A–D and pH dosing meters are unchanged.

#### 19.4.9.1. From an echo to litres (hardening of the reading)

1. **Nano — background pinger.** One ping per loop pass at most, alternating the two sensors every 35 ms (each sensor every ~70 ms). The last 5 pings of each are kept; a ping outside 2–200 cm counts as lost. `TANK` sends, per tank, the **median** of the fresh pings (≥ 3 needed, else -1) and their **spread** (width of the middle three), then (2026-11-16) a **status letter per tank** and a **frame number**: `TANK,<resCm>,<mixCm>,<flowLpm>,<resSpread>,<mixSpread>,<st>,<seq>`. Status: `K` ok, `E` no echo, `R` echo out of range, `F` too few good pings, `S` the echo line was already HIGH before every trigger (a hung transducer), `X` no pings at all. The ping timeout is 12 ms (≈ 2 m) so one lost echo cannot hold the loop. The calibration stream uses the same median. In FAST (the whole work order) the Nano puts the pinger first: a TANK every 0.3 s, NPK (which blocks up to 1 s) skipped, ENV / SOIL / LIGHT once a minute.
2. **ESP32 #1 — temperature and plausibility.** The distance is corrected for the speed of sound with the DHT22 air temperature (reference 30 °C, `ULTRA_TEMP_REF_C` [CONFIRM]). A reading is **usable for control** only if: it is a real echo; 3 cm ≤ distance ≤ empty + 15 cm; spread ≤ 2.0 cm (`ULTRA_MAX_SPREAD_CM`); and the tank geometry gives litres. It must also carry the Nano's `K`. Otherwise it is shown on the LCD (with the Nano's letter, or `!` when ESP32 #1 refused it) but sent to ESP32 #2 as -1.
3. **ESP32 #1 → ESP32 #2 — checksummed and numbered.** `LEVEL,<mix%>,<res%>,<mixL>,<resL>,<resInL>,<seq>,*XX` (`resInL` = litres the Nano's reservoir flow meter has counted **into** the reservoir since ESP32 #1 booted; `seq` = ESP32 #1's line number 0–9999 — ESP32 #2 drops a repeat and counts gaps as lines lost, shown as `lost_lines=` in a level fault; ESP32 #2 also accepts the line without the last two fields). The CRC-8 is **required**, and every field must be a clean number; a damaged line is dropped, never turned into a level. It is relayed while an AC pump runs **and** while ESP32 #2 holds a `LEVEL_FEED,ON` lease (renewed every 4 s, expires after 12 s), which it holds for the whole of a work order. The Nano stays in FAST for the same time.
4. **ESP32 #2 — filter.** Median of the last 5 accepted samples. A sample more than 1.5 L from the median is held back (splash, multipath); only **3 in a row that agree** are believed as a real step. Fresh = an accepted sample in the last 10 s (`LEVEL_BLIND_MS`: 3 s → 10 s on 2026-11-16, ported from web v.3 — splashing or a lost echo is not a fault).

**Link hardening (2026-11-16).**
- **Nano → ESP32 #1** (software serial, 9600 baud): the receive buffers are now 512 bytes and 5120 bit-edges (~0.6 s of traffic even with a stalled loop; they were ~0.2 s, and the rig logs showed 11–28 % of Nano lines failing their checksum). Overflows are counted and logged (`NANO|LINK|RX_OVERFLOW`). The TANK frame numbers give the link quality: Sensor Diag page 2 shows `Lk97%` (frames that arrived lately), the TANK log row `|lnk=97%`.
- **TANK gap watchdog (ESP32 #1):** in FAST a TANK comes every ~0.3 s. None for 2.5 s → ESP32 #1 sends FAST again at once (the Nano may have lost it) and logs `NANO|LINK|TANK_GAP|ms=…` once per gap; a Nano reboot (`STATUS,NANO,BOOT`) gets FAST immediately.
- **ESP32 #1 → ESP32 #2** (hardware UART): ESP32 #2's receive buffer is 1 KB (was 256 B), so a loop pass stretched by a PZEM timeout cannot drop LEVEL lines.
- **ESP32 #2 → ESP32 #1** (2026-11-17 bug check): ESP32 #1 accepts lines up to 256 characters (was 140) and its receive buffer is 1 KB (was 256 B). A fill's `FLOW_FAIL` with its level diagnosis (`moved|level|base|bad_lines|lost_lines|src|res|mix`) runs ~150 characters; at 140 it was dropped as garbage, so ESP32 #1 never saw the hold — and with `PROG` still refreshing the run timeout, it showed the run as live indefinitely.
- **Hold re-assertion (2026-11-17):** while a fault is held, ESP32 #2 repeats it every 30 s as `INFO,HELD,<work order>,<code>,<loc>`. ESP32 #1 ignores it when it is already holding (the normal case — not even logged); if the original hold line was lost, ESP32 #1 enters the hold from it (log `…|hold_reply_was_lost`), but only for the work order ESP32 #2 acknowledged from it.

#### 19.4.9.2. How a stage is metered (ESP32 #2)

| Rule | Fill (transfer) | Delivery (booster) |
| ---- | --------------- | ------------------ |
| 1. Baseline | Before the pump: 5 accepted samples, all ≥ 3 s after the last pump stopped and ≥ 8 s after the mixer stopped (`MIX_SETTLE_MS`). That **measured** level replaces the stored figure. None within 30 s → hold `LEVEL_BLIND,…|NO_SETTLED_LEVEL` | none within 30 s → **no hold**: plans from the stored figure (`DEGRADED,LEVEL_BASE_EST`) and carries on — a delivery cannot overflow anything |
| Target | batch − (level − planning floor), capped at `MIXCAP` (litres at the 90 % stop). Planning floor = the higher of the pump floor and the low-stop heel (`mixHeel`) | everything above the planning floor |
| 2. Done | transferred estimate (§19.4.9.7) ≥ target − 0.15 L (`FILL_LEAD_L`); or the mixing tank alone ≥ target + 1 L (`FILL_MIX_OVER_L`); or the 90 % overflow stop (`LEVEL_HIGH`, carries on); or the reservoir found empty (§19.4.9.8, `RES_EMPTY`, carries on) | level ≤ pump floor + 0.15 L; or the **low stop**: under 5 % for 5 s → complete (`INFO,MIX_LOW_STOP`, §19.4.9.8) |
| 3. No flow | level moved < 0.4 L within 20 s **of the last pump start** → `FLOW_FAIL,TRANSFER|NO_LEVEL_CHANGE` (hold). Before that, at 10 s, the which-pump check (§19.4.9.3) | `FLOW_FAIL,MAIN|NO_LEVEL_CHANGE` (hold) |
| 4. Stall (moved, then no 0.25 L step for 12 s) | reservoir ≤ 18 % (its filter zone) or unreadable → `RES_EMPTY` (carry on with what came in); reservoir above it → `FLOW_FAIL,…|STALL` (hold: a blocked line or filter, a shut valve or a failed pump — the pump sits below the reservoir and cannot lose its prime) | booster sucking air → `INFO,MIX_EMPTY` (done). The level it stopped at is **learned as the pump floor** (`mixDead`, ≤ 3 L; more → `DEGRADED,MIX_RESIDUAL`) |
| 5. Blind (no accepted mixing-tank sample for 10 s after a 3 s grace) | hold `LEVEL_BLIND` (overflow risk) | finish the remaining litres **on a timer** at `BOOSTER_LPM`, `DEGRADED,LEVEL_BLIND|DRAIN`, water total marked `,EST`; if the level returns, it still stops at the floor |
| 6. End | litres = the transferred estimate; the tank figure = baseline + those litres when the two tanks agreed, else the mixing tank's own reading | the tank figure = the measured level; `DONE,…,WATER,<L>` = the measured drop |
| Cross-check | `INFO,LEVEL_XCHECK,mix=+…|res=-…|used=…[|res_in=+…]`; differ by > max(1 L, 35 %) → `DEGRADED,LEVEL_DISAGREE` | — |
| Cap | `stageCapMs` (1.5 × the batch at the nominal pump rate, ≥ 180 s) → `FLOW_FAIL,…|CAP` | same |

`FLOW_FAIL` / `LEVEL_BLIND` text now carries `moved=…|level=…|base=…` instead of a pulse count (`moved=0.00` = nothing moved at all). After either, RESUME IRRIGATE / RELEASE runs the stages on a **timer** (as before for a dead meter); NORMAL retries with the ultrasonic. After 3 identical holds the LCD steers to Release for `LEVEL_BLIND` too.

> **Since 17 Nov (§19.4.9.13):** a fill's "Done" is its run time from the inverter switch-on, then a 1-minute settle
> and a measurement (top-up when more than 3 L short). A fill that loses its level carries on by its run time, capped
> (§19.4.9.24). A delivery ends 10 s after the tank is under 5 %. The rows above still describe the guards that run
> alongside.

#### 19.4.9.3. Which pump is really running (2026-11-16 user rule)

The levels at inverter-on are the reference. **10 s later** (`SERVO_JUDGE_MS`) one verdict (`pumpVerdict`, level_meter.h):

| Selected pump | The other pump is running when… | Fine when… |
| ------------- | ------------------------------- | ---------- |
| Transfer (fill) | the mixing tank **falls** ≥ 0.6 L; or — in a run's fill, with the reservoir clearly above its 18 % filter zone — **neither tank moved** (< 0.4 L): the booster is powered and pushes against its shut column valves | the mixing tank rises ≥ 0.4 L, or the reservoir drops ≥ 0.4 L |
| Booster (delivery) | the **reservoir falls** ≥ 0.6 L (and the mixing tank does not), or the mixing tank **rises** ≥ 0.6 L | the mixing tank falls ≥ 0.4 L |

Anything else is no verdict (the reservoir in its filter zone → the empty-reservoir window decides; nothing moving on a delivery → the no-flow rule). A verdict must still stand 2 s later (`SERVO_CONFIRM_MS`). Then **no halt**: `SERVO_MISMATCH,<pump>|<WRONG_WAY|NO_MOVE>|mix=…|res=…|t=…|try=n/2` (Major alert), inverter off, the moved litres measured, two full servo swings, inverter on, judged again (the no-flow clock restarts with it). A fill re-plans the rest to the same final mixing-tank level (`INFO,FILL_REPLAN|done=…|left=…`). A delivery is measured on from the level at the restart (`INFO,DRAIN_REPLAN|done=…|left=…`, 2026-11-17): the water the transfer pump wrongly added goes to the column too, so it is counted as delivered, and the booster no longer has to drain it back down before any progress shows. Measured from the old baseline, ~1.6 L of it at 6 L/min used up the whole 20 s no-flow window and could hold a healthy booster as `FLOW_FAIL`. After 2 such recoveries the next wrong verdict holds the run as `SERVO_FAIL,…|STUCK|…`: a **Critical** alert, and the hold offers the choice — **switch the selector again** (LCD `Switch servo again`, SMS `SERVO` or `NORMAL`: the step restarts with the 3-swing pre-seat) **or not** (LCD `Cancel run...`, SMS `CANCEL,30`). The SMS hold menu for this fault lists exactly those. Testing combos, an exercise and a prime get the wrong-way verdicts only.

**Pump start retry (2026-11-16 hardening).** A start that fails on the way — a servo move not confirmed within 10 s, the Nano refusing a move, or the inverter giving no voltage within 6 s — is retried **once** from the top (inverter off, decay, dead-check, pre-seat) before it counts (`INFO,PUMP_START_RETRY,<pump>|why=…`). Testing's servo-only rows are not retried.

**Pre-seat (2026-11-16, ported from web v.3).** Before **every** AC pump start the servo makes **3 full swings** (other side, back — 6 moves) ending on the target pump (`SERVO_PRESEAT_SWINGS`), with the inverter off and its AC confirmed dead the whole time. Testing's `Servo>Transfer` / `Servo>Booster` rows still make a single move. web v.3 also turned its cross-**flow** stuck check off (the flow sensors read noise); web v.4's check reads the tank **levels**, so it stays on. The Nano now uses **90° = transfer, 160° = booster** (swapped 2026-11-16; the words LEFT = transfer / RIGHT = booster are unchanged), keeps the PWM 1 s per move (was 0.5 s), and saves only the final angle to EEPROM (10 s after the last move). A stuck booster during a fill (column valves shut, nothing moves) is now caught too: the 10 s `NO_MOVE` verdict above. Both AC pumps sit **below** their tanks (flooded suction, 2026-11-17): they are always primed and move water from the first second, so "nothing moved in 10 s" can never be a pump still priming.

Testing Fill / Push combos report `TEST,LEVEL,<row>,<ΔL>` (mixing-tank change since inverter-on); the website pulse answers `LEVEL OK +1.23 L` / `NO LEVEL CHANGE`.

#### 19.4.9.4. Tank litres calibration (LCD → Settings → Calibration)

Each tank is modelled as a truncated cone: **31.0 cm across at the 0 % (EMPTY) level, 39.4 cm at 100 % (FULL)** (`TANK_D_BOTTOM_CM` / `TANK_D_TOP_CM` in ESP1, [MEASURE]). With empty 42.9 cm / full 24.0 cm that is **≈ 18.5 L at 100 %, ≈ 16.2 L at the 90 % stop**, ≈ 0.79 L/cm at the bottom and ≈ 1.18 L/cm at the top. The operator's own points correct it:

1. Open **"Tank Mix liters"** (or **"Tank Res liters"**). Line 1 shows the live level: `41.2cm 9% ~1.5L` (cm, %, litres on the current curve).
2. Bring the water to a level. On the mixing tank, **hold UP** = transfer pump in (with the 90 % overflow stop), **hold DOWN** = booster out through column A; or fill / empty by hand.
3. Wait for **STEADY** (the last 7 streamed readings within 0.6 cm), press **ENTER** to lock the level.
4. Type the litres really in the tank with **UP / DOWN** (0.1 L; hold to repeat, after 2.5 s 1 L steps). It starts at the curve's estimate. **ENTER** saves.
5. The SAVED screen shows the point count and the resulting litres at 100 % and at 90 %. **ENTER** = add another point, **DOWN** = clear all points (confirm with ENTER), **BACK** = done.

Up to **8 points per tank**. A point within 0.3 cm of an old one replaces it. A point that would make the curve fall (fewer litres at a higher level) is refused (`must rise with level`). Between points the curve follows the cone's shape, stretched through the points; a point at the very bottom (e.g. "2 L when the sensor reads empty") adds that water under the 0 % mark. Points are stored as sensor **distances** (NVS `calib`/`tvM`, `tvR`), so re-capturing "Ultra … empty" later moves the 0 % mark without moving them. A new mixing-tank point or empty capture pushes the new `MIXCAP` to ESP32 #2 at once (and every work order carries it).

Recommended: 3–4 points per tank (near empty, ~⅓, ~⅔, near full), measured with a marked bucket or jug.

#### 19.4.9.5. Where the litres show

LCD Sensor Diag page 2 (`R 31.2  12.4L s0.3`: cm, litres, spread; `!` = not usable for control), the SD/Supabase `TANK` log rows (`|L=res/mix|s=…`), and Firebase `sensors.reservoirLiters` / `sensors.mixingLiters`.

#### 19.4.9.6. Values to confirm at commissioning

`TANK_D_BOTTOM_CM`, `TANK_D_TOP_CM`, `ULTRA_TEMP_REF_C`, `ULTRA_MAX_SPREAD_CM` (ESP1); `RES_FILTER_PCT` (shared); `FILL_RES_WEIGHT`, `FILL_MIX_OVER_L`, `RES_EMPTY_WIN_MS`, `RES_EMPTY_RISE_L`, `MIX_LOW_STOP_PCT`, `MIX_LOW_STOP_MS`, `SERVO_JUDGE_MS`, `SERVO_CONFIRM_MS`, `MIX_SETTLE_MS`, `FILL_LEAD_L`, `MIX_EMPTY_MARGIN_L`, `LVL_MOVED_MIN_L`, `LVL_STALL_STEP_L`, `LVL_NOFLOW_MS`, `LVL_STALL_MS`, `LVL_SETTLE_MS`, `LVL_JUMP_L`, `CROSS_LEVEL_MIN_L`, `TRANSFER_LPM` / `BOOSTER_LPM` (defaults until the LCD rates are set), `RATE_WEIGHT`, `RATE_XCHECK_FRAC`, `RATE_DOSE_FRAC`, `RATE_WEAK_FRAC` (ESP2, §19.4.9.11); `US_PING_GAP_MS`, `US_MIN_CM`, `US_MAX_CM`, `ULTRASONIC_TIMEOUT_US` (Nano). The full mark (24.0 cm) sits near the dead zone of a waterproof JSN-SR04T-type sensor (~20–25 cm): if readings get erratic near full, raise the sensor mount.

**Flash all three boards together** — the Nano's 8-field `TANK`, ESP1's checksummed and numbered `LEVEL` and ESP2's `LEVEL_FEED` belong together; an older ESP1 rejects the new `TANK`.

#### 19.4.9.7. A fill is measured on the reservoir (2026-11-16)

> **Superseded on 17 Nov (§19.4.9.13):** a fill is now timed by the transfer pump's run time and measured on the
> mixing tank after a 1-minute settle. The reservoir is no longer the measure of a fill.

While the transfer pump runs, the water falls onto the mixing tank's surface: waves, a rising jet, foam — that ultrasonic ripples by a few tenths of a litre. The reservoir is only drawn from at the bottom, so its surface stays almost flat. So the litres **transferred** are taken from the **reservoir's drop** (plus any water its flow meter counted pouring **into** it during the fill, `resInL`), and the mixing tank is the witness — never ignored:

- While the two agree (within max(1 L, 35 %)), the estimate = 75 % reservoir drop + 25 % mixing-tank rise (`FILL_RES_WEIGHT` 0.75 [MEASURE]).
- When they disagree, the estimate stays on the reservoir (pulled by at most a quarter of the band), and the end reports `DEGRADED,LEVEL_DISAGREE` with both numbers (a leak or hose off between the tanks, or a tank calibration that is off).
- The mixing tank **alone** ends the fill when it reads 1 L or more past the target (`FILL_MIX_OVER_L` [MEASURE]): a reservoir that under-reads can never overfill the mixing tank. The 90 % overflow stop and ESP32 #1's 97 % backstop are judged on the mixing tank alone, as before.
- No reservoir reading at the start, or none for 10 s during the fill → `DEGRADED,LEVEL_RES_BLIND|TRANSFER|…|mixing_tank_only`; the rest of that fill runs on the mixing tank alone. The same happens, as a plain log line (`INFO,FILL_MIX_ONLY|why=filter_zone`), when the reservoir reaches its **filter zone** (≤ 18 %): below that its drop is no basis. No mixing-tank reading → the fill holds `LEVEL_BLIND` (overflow risk), as before.
- No-flow and stall (rules 3–4) are judged on the estimate: when the reservoir empties or the pump loses its prime, both tanks stop moving.
- The inflow (`resInL`) comes from the Nano's reservoir flow meter, a pulse counter like the main-line meters the inverter's noise upset. Since 2026-11-17 it is believed only as far as the mixing tank can explain it: at most (mixing-tank rise − reservoir drop) + 1 L (`XCHECK_MIN_L`). Unbounded, false pulses would inflate the estimate and end every fill early; bounded, they can move it by 1 L at most. `|res_in=+…` in `INFO,LEVEL_XCHECK` is what the meter counted (its `res=-…` already holds the part that was used). Check it after the first fills: it should read ~0 when nothing is pouring into the reservoir.

#### 19.4.9.8. Levels past 100 %, reservoir empty, delivery low stop (2026-11-16)

**Past 100 %.** 0 % (EMPTY) and 100 % (FULL) stay the calibrated distances (42.9 cm / 24.0 cm). Water **closer** than FULL now reads **past 100 %** instead of stopping at 100: each % is 0.189 cm, so **120 % = 3.78 cm above the full mark (20.2 cm from the sensor)**. A deep echo is still 0 %. The ceiling is 200 % (`LEVEL_PCT_MAX`, nonsense guard). The litres keep counting above FULL on the same cone taper (≈ 23.3 L at 120 % with the default shape), and calibration points may be taken up there (up to 200 %; was 115 %). The LCD, the website (`sensors.reservoirLevel` / `mixingLevel`), ThingSpeak and the logs show the real figure, e.g. `Res:120%`. The 90 % fill stop and the 97 % backstop are unchanged.

**Reservoir empty — judged on the mixing tank (user rule).** The reservoir has a filter at the bottom, and its ultrasonic stops at ~15 %: it cannot see whether any water is left below that. So on a transfer, 3 reservoir readings in a row at or under **18 %** (`RES_FILTER_PCT`, shared/rig_constants.h) open a **10 s window** (`RES_EMPTY_WIN_MS`); 3 above it close it. If the **mixing tank** has not risen by 0.3 L (`RES_EMPTY_RISE_L`) across the window, the reservoir is empty: the transfer stops, `RES_EMPTY,TRANSFER|…|by=RES_EMPTY_WINDOW` (Major + SMS "refill it"), and the run carries on with what came in. If it rose, the next 10 s window opens from there. ESP32 #1's reservoir-low guard (`RES_LOW_PCT`) is now the same 18 % (it was 15 %, which an empty reservoir sitting on its filter reads, so it never tripped): no new run starts below it, with the `RES_LOW` alert. This replaces web v.3's 15 % / 10 s rule.

**Delivery low stop (user rule).** During a delivery, 3 mixing-tank readings in a row under **5 %** (`MIX_LOW_STOP_PCT`) start a **10 s countdown** (`MIX_LOW_STOP_MS`; 5 s until 17 Nov); 3 at or over 5 % cancel it. When it runs out the booster stops and the delivery is **complete** even though that little mixture is left (`INFO,MIX_LOW_STOP,<MAIN|FLUSH>|mix=…%|delivered=…|expected=…|left=…`, logged, no alert). What was left is remembered as the **heel** (NVS `mixHeel`): the next fill plans its batch above it, so the column still gets its full litres while the heel stays in the tank.

#### 19.4.9.9. Also ported from web v.3 (2026-11-16, not ResMix / MixIrr related)

- **Relays:** nutrient A on **P1**, nutrient B on **P7**; P11 / P12 spare; a refitted reservoir valve would go on **P5** (§19.4.7). The LCD Testing list and the website pulse targets (`nutA` = row 1, `nutB` = row 7) follow.
- **Testing LCD:** fixed 20-column rows (14-character name, the `[OK]` / `[ON]` / `[ -]` mark always in columns 16–19), so scrolling from a long name leaves no old characters behind.
- **Run buffers:** ESP2 `AC_DEAD_WAIT_MS` 5 → 15 s, `BATT_VERIFY_MS` 3 → 5 s, Testing combo cap 30 → 60 s; ESP1 `BSW_VERIFY_NANO_MS` 1.5 → 3 s, pump exercise timeout 60 → 150 s, dashboard Fill / Push pulses get 90 s to start their pump; the Nano stays in FAST between the servo OK and `PUMP,ON`; an AC prime's 45 s cap counts from the pump really running.
- **PZEM:** a trip needs consecutive out-of-window readings (voltage 3 in a row ≈ 1.5 s, overcurrent 2 ≈ 1 s).

#### 19.4.9.10. LCD and Settings (2026-11-16)

- **Settings menu:** the `Batt Switch [OK]` status row is gone; the switch status is shown only on the LCD Power page. The Settings row reappears as `BattSw FAULT>clear` only while a battery-switch fault is latched, because clearing it is the one action it carries.
- **Power page:** `B2:  87%  -12.3W` — the secondary battery's charge % (linear 12.8 V = 0 % … 16.8 V = 100 %) and its power, **negative while it charges** (`B2:  --%   --W` with no reading). The battery-switch status moved to the end of the solar row: `SWok`, or `SW!!` while a fault is latched.
- **Sensor Diag page 2:** the last character of the R / M rows is blank when the reading is usable, otherwise the Nano's status letter (`E`, `R`, `F`, `S`, `X`) or `!` (ESP32 #1 refused it); the bottom row shows `Lk97%`, the TANK link quality.
- **Not ported (flow-meter only):** the ResMix / MixIrr K-factors 583.8 / 458.5 and their NVS keys `kRM2` / `kMI2`, and the cross-flow check switch — web v.4 has none of those meters.

#### 19.4.9.11. Pump rates: litres from how long a pump runs (2026-11-17 user request)

**The rule.** Each pump's constant flow rate is measured by hand (a marked bucket and a stopwatch) and typed in on the LCD. The firmware then estimates the water from **how long the pump runs**, not from the ultrasonics alone. The transfer and booster pumps sit **below** the tanks (flooded suction), so they are **always primed**: water moves the moment the inverter's AC is confirmed, and rate × run time counts from that moment with **no priming lag**. Re-seat pauses are left out.

**LCD → Settings → Calibration**, five new rows:

| Row | Unit | Step (held > 2.5 s) | Range | Stored in (ESP1 NVS `calib`) |
| --- | ---- | ------------------- | ----- | ---------------------------- |
| `Rate Transfer` | L/min | 0.05 (0.5) | 0.5–60 | `rXfer` |
| `Rate Booster` | L/min | 0.05 (0.5) | 0.5–60 | `rBoost` |
| `Rate Nut A` / `B` / `C` | mL/min | 1 (10) | 1–5000 | `pumpA` / `pumpB` / `pumpC` (the bench tools' keys) |

The screen shows `set:  7.85 L/min` (or `set: not measured`) and `seen: 7.62 L/min`: the rate the last run's levels, or the dose's flow meter, actually saw (`INFO,PUMP_RATE`). Use **UP / DOWN** to change the value; holding repeats after 0.6 s. Below the minimum is **not measured**. **ENTER** saves the value and sends it to ESP32 #2 straight away (`SET_CAL,PRATE,<transfer>,<booster>` / `SET_CAL,NRATE,<A>,<B>,<C>`, 0 = not measured). The SAVED screen says `ESP2 has it`, or `ESP2: at next start` when ESP32 #2 did not confirm; it then gets the values at its next power-up, like every other calibration. ESP32 #2 keeps its own copy in NVS (`rX`, `rB`, `rNA`–`rNC`). A nutrient rate also sizes that pump's 2× dose ceiling (`DCEIL`), as before. Clearing a nutrient rate puts the ceiling back on the 50 mL/min default.

**How ESP32 #2 uses a measured rate:**

> **Superseded for water the same day (§19.4.9.13).** A fill is now TIMED by the transfer pump's rate and checked on the mixing tank after it settles; a delivery is the mixing tank's own fall, with the booster's rate only sizing how long it may take. Neither water pump is blended into the levels any more. The nutrient pumps keep the blend described below.

- **How much moved.** rate × run time is blended into the ultrasonic estimate (`level_meter.h` `rateBlend`): `L = levels + w·d·(1 − |d|/tol)`, with `d` = rate estimate − levels, `w` = 0.4 (`RATE_WEIGHT`) and `tol` = max(1 L, 25 %) for water. The pull fades to nothing as the two drift apart, so a wrong rate can shift the figure by at most `w·tol/4`; beyond the band it is ignored. The blend drives the fill's stop, the litres credited to the tank and to the column, and the progress bar. For a dose the same blend works with the flow meter, with a band of max(3 mL, 25 %).
- **Whether water moves at all** is never asked of the clock. No-flow, stall, the empty-reservoir window, the wrong-pump verdict and the low stop are all judged on the levels alone. When one of them fires, the rate clock stops (`rateClockStop`), so water that never flowed is not counted. A dead nutrient meter still ends as `DOSE_TIMEOUT`: the clock never doses on its own.
- **No end reading.** If the level went blind, the stage is credited with the last measured figure plus rate × the time since (`rateCarryL`), marked `EST`. A delivery that goes blind hands over to its timer with those litres already counted.
- **Caps and timers.** The stage cap (1.5× the expected time) and the timed level-blind recovery use the measured rate instead of the 8 / 6 L/min defaults.
- **Weak pump.** 25 s after a pump comes on (past the 10 s verdict and its re-seat), once the rate says ≥ 1.5 L should have moved: if the levels saw under half of it, ESP32 #2 sends `DEGRADED,PUMP_WEAK|<pump>|levels=…|by_rate=…|cal=…|t=…s` (Major alert, once per pump per run). The run carries on. Likely causes: a blocked filter, a half-shut valve, a worn pump, or a rate measured under other conditions. The check is skipped while the reservoir is in its filter zone, because a source running dry slows any pump.
- **Logs.** `INFO,PUMP_RATE,<pump>|seen=…|cal=…|unit=…|moved=…|s=…` is sent at the end of every stage of 15 s or more (a dose: 5 s or more). A water stage is timed to its last real progress step, so a stage that ended on an empty source does not drag the figure down. `INFO,LEVEL_XCHECK` and the fault text carry `by_rate=` when a measured rate was in use.

**Not measured (0)** means the levels alone (a dose: its meter alone), as before.

**What it buys (simulation, `check_rate.py`, 10 L fill at 8 L/min, 300–400 runs each):**

| Ultrasonic noise | Levels only | + rate exact | + rate typed 10 % high |
| ---------------- | ----------- | ------------ | ---------------------- |
| quiet reservoir (0.12 L) | rms 0.09 L | 0.09 L | 0.31 L |
| typical (0.3 / 0.8 L) | 0.17 L | 0.13 L | 0.35 L |
| noisy (0.5 / 1.2 L) | 0.28 L | 0.19 L | 0.44 L |
| reservoir litres curve 8 % off | 0.67 L | 0.50 L | 0.78 L |

A well-measured rate helps most when the ultrasonics are noisy or their litres calibration is a little off. A rate typed 10 % wrong costs about 0.3 L on 10 L. Measure carefully, and compare with the `seen:` figure after the first runs. A rate 40 % off is outside the band and simply ignored.

**How to measure.** Time a full bucket from each hose, at the normal working levels. The transfer pump's discharge goes into the bucket instead of the mixing tank (Testing `Fill` combo, dead-man). The booster's goes out of a column line (Testing `Push>Col`). Divide litres by minutes. For a nutrient pump, time 100–200 mL into a measuring cylinder (Prime from the `Flow Nut` row) and convert to mL/min.

#### 19.4.9.12. Hardening from the web v.3 log of 17 Nov 2026 (2026-11-17)

**What the log shows (`20261117.CSV`, web v.3).** Column B's 08:00 irrigation filled the mixing tank: the transfer pump ran 23 s and metered 5.01 L. The delivery's booster was confirmed on at 08:01:08 (`AC_OK v=96`). In the same second ESP2 answered `DONE,SEQ_IRRIGATION_B,WATER,5.01`, and the receipt said `OK|WATER=5.01of5.0`. Nothing reached the field. No `MIX_LOW_STOP` or `MIX_EMPTY` line was sent, so the stage ended on its flow meter: the mix → column meter "counted" 5 L, about 2300 pulses, within a second of the inverter switching on. That was switching noise on the meter cable. The Nano link was just as noisy: 31 Nano frames were dropped that day for bad framing or a bad checksum, 14 of them in the minute of that run (08:00:20-08:01:24), while the pumps and the servo were switching. web v.4 has no mix → column meter, so this exact failure cannot happen. The same weaknesses are hardened here:

| Weakness | web v.4 guard (ESP2) | What the operator sees |
| --- | --- | --- |
| Noise counted as meter pulses (nutrient and pH meters) | `flowISR` ignores edges closer than `FLOW_MIN_EDGE_US` (1.5 ms); no meter here pulses faster than about 600 Hz. A dose's pulses are judged in 200 ms windows (`METER_WIN_MS`), and a window with more than the fastest accepted pump could give (`RATE_MLPM_MAX`, `burstMaxPulses`) is dropped. That ceiling is not the measured rate, so a real pulse is never dropped and a dose cannot overshoot because of the filter. | `DEGRADED,FLOW_NOISE\|<pump>\|dropped=…\|in_ms=…\|max=…\|glitches=…` (Major, once per stage); the dose carries on. |
| A level "done" in an impossible instant (a glitched ultrasonic reading) | Rule 2 (filled / drained to the floor) is accepted only at a believable pace: `paceOk` (level_meter.h), no faster than `LEVEL_PACE_X` (3×) the pump's rate (measured, or the default) beyond `LEVEL_PACE_SLACK_L` (0.6 L). Until then the stage runs on, and the no-flow, stall and cap rules still apply. | `DEGRADED,LEVEL_JUMP\|<FILL\|DRAIN>\|moved=…\|s=…\|max_lpm=…` (once per stage). |
| A delivery that stops early and is still reported OK | A delivery stall (rule 4) with under half of the plan delivered (`DRAIN_STALL_DONE_FRAC`) and more than `MIX_DEAD_MAX_L` (3 L) still above the floor is a blocked line, a shut column valve or a failed booster, not an empty tank. It is held as `FLOW_FAIL,MAIN\|STALL\|left=…\|moved=…`, so the operator chooses (Resume / Release / Irrigate) instead of the run finishing "OK" with the column dry. A smaller residue keeps the previous behaviour (`MIX_EMPTY`, `MIX_RESIDUAL`). | A held fault with the litres left; the website explains it in words. |
| A booster confirmed at 96 V | Not a fault: the PZEM read the inverter's output while it was ramping. The run's voltage check already has its ramp grace (`pzVLowRun`), and a sustained low voltage still ends as `PWR_FAIL,VOLTAGE\|LOW`. | — |

Not firmware: the many bad Nano frames point at the wiring. Route the Nano's serial and sensor cables away from the inverter and pump leads, use twisted pair or shielded cable with one ground point, and fit ferrites near the inverter. web v.4 checksums every frame and median-filters the levels, so a dropped frame costs a sample, not a wrong level.

#### 19.4.9.13. Transfer by run time, settle and top-up; delivery to under 5 % + 10 s; the mixer in the tank (2026-11-17 user rule)

**The rule (user, 17 Nov).** The transfer pump sits below the reservoir and is always primed, so it is accurate and critical: the water a fill needs is turned into a **run time**, counted from the moment the inverter is **switched on** (its start-up included, the way the pump was measured). When the time is up the inverter stops, the water settles for about **1 minute** while the mixing tank's ultrasonic is watched until it is steady, and the ultrasonic then measures the tank. More than **3 L** short: the transfer runs again for what is missing. 3 L or less: carry on. The **reservoir is no measure** of the water used, because it is refilled while the transfer runs. The booster is less critical: it empties the mixing tank to **under 5 %**, then runs **10 s more**. Its rate only says how fast the tank should drain, and it is lower once the drip pipe is connected.

**Measurements (user, 17 Nov; 1 L = 1028 g; grams minus the container; time from the inverter switch-on):**

| Pump | Runs | Fit | Notes |
| --- | --- | --- | --- |
| Transfer (reservoir → mixing tank) | 10, 6.0–8.7 s, 1.39–2.29 L | V = 0.47 L + 11.74 L/min × t (scatter 0.18 L) | The slope is uncertain on runs this short (± 3.7 L/min). The web v.3 log's own fill agrees: 5.01 L metered after about 26 s from the switch-on (the line gives 5.5 L with the run-on). |
| Booster, column A valve open (no drip pipe) | 5, 6.9–8.0 s, 0.78–1.30 L | about 13 L/min after about 3 s (scattered) | |
| Booster, column B valve open (no drip pipe) | 5, 7.4–8.6 s, 0.97–1.20 L | 11.5 L/min, water from 2.3 s (scatter 0.02 L) | |

`pumpfit.py` (session scratchpad) holds the numbers and the fits.

**A fill (FILL / FLUSH_FILL, ESP2 `fillStep`):**

1. Plan from a settled level (rule 1): need = the batch minus what the booster can already reach, capped at the 90 % limit (about 15.9 L with the mixer).
2. Run time = (need − `XFER_START_L` 0.47 L) ÷ Q, with Q = the LCD **Rate Transfer** (default **11.74 L/min** = `TRANSFER_LPM`), never under `XFER_MIN_RUN_S` (6 s, the shortest run measured). It is timed from the inverter relay switching on (`apRelayOnMs`). 5 L → 23.2 s, 10 L → 48.7 s, 15 L → 74.3 s.
3. During the run the mixing tank's own rise still guards it: the 90 % overflow stop, no level change in 20 s (FLOW_FAIL), a stall, the empty-reservoir window (18 % + 10 s), the 10 s pump verdict and blind. A weak pump (under half the run time's litres at 25 s) raises `DEGRADED,PUMP_WEAK|TRANSFER`. A selector re-seat measures the tank and times the rest from the new switch-on (`INFO,FILL_REPLAN`).
4. Time up → inverter off → **settle**: at least `FILL_SETTLE_MS` (60 s), and the reading must have held within `FILL_STEADY_L` (0.3 L) for `FILL_STEADY_WIN_MS` (10 s). Never steady by 150 s: measured anyway (`DEGRADED,FILL_NOT_STEADY`). No reading at all: the run time's figure is kept, marked EST (`DEGRADED,FILL_SETTLE_BLIND`).
5. **Measure** → `INFO,FILL_CHECK|goal=..|measured=..|missing=..|settle_s=..|OK/TOPUP`. More than `FILL_TOPUP_MIN_L` (3 L) short: a top-up run for what is missing (at most `FILL_TOPUP_MAX` = 2), then settle and measure again. Still short after them: `DEGRADED,FILL_SHORT`, and the run carries on. A run that ended on the overflow stop or an empty reservoir is measured but never topped up.

**A delivery (DELIVER / FLUSH_DELIVER):** the mixing tank's fall is the measure of the litres sent to the column. It ends `MIX_LOW_STOP_MS` (**10 s**, was 5 s) after the tank is under 5 % (three readings in a row; a reading back at 5 % or over cancels the count). A tank its booster cannot get that low ends the same way, 10 s after reaching the learned floor (`mixDeadL`). The booster's rate is no measure of the litres. The no-level-change and stall windows and the stage cap allow a tank draining at half the LCD **Rate Booster** (`DRAIN_SLOW_X`). The default `BOOSTER_LPM` is a cautious **1.0 L/min** (the drip pipe is not measured yet), so the windows are 72 s and 45 s and the cap is 15 min for 5 L. The open-valve 13.2 L/min (`BOOSTER_OPEN_LPM`) is only the fastest a tank may drain (the impossible-pace check of §19.4.9.12). There is no weak-pump alert for the booster.

**The mixer in the mixing tank:** a cylinder 6 cm across and 11.4 cm tall (322.3 cm³) stands on the bottom. `shared/tank_volume.h` now takes the part of it under water out of the tank's shape: 0.0283 L per cm of water, 0.322 L once it is covered. The calibration points still hold exactly, and between them the curve bends like the real tank. With the 31 / 39.4 cm cone over 18.9 cm: 5 % = 0.70 L, 50 % = 7.88 L, 90 % = 15.90 L, 100 % = 18.16 L. Assumption: the mixer stands on the 0 % level (the empty mark at 42.9 cm).

**Simulation** (`fillsim.py`: the user's 0.18 L run-to-run scatter, a 0.15 L reading noise, 2000 fills each):

| Real pump vs the model | 5 L batch | 10 L batch | 15 L batch |
| --- | --- | --- | --- |
| 20 % slower | 4.1 L, 1 run | 8.1 L, 1 run | 13.0 L, 1.4 runs |
| 10 % slower | 4.6 L | 9.0 L | 13.5 L |
| as measured | 5.0 L (4.4–5.7) | 10.0 L (9.4–10.8) | 15.0 L (14.4–15.6) |
| 10 % faster | 5.5 L | 11.0 L | 16.5 L |

The 3 L tolerance is wide for small batches: with a pump 20 % slower than its rate, a 5 L irrigation gets about 4.1 L and is not topped up. Re-measure the transfer rate carefully. Better: time two lengths, for example 10 s and 40 s, and take Q = (V40 − V10) ÷ 30 s, which leaves the start-up out.

**To confirm at commissioning:** `XFER_START_L` and Q (two run lengths), `FILL_TOPUP_MIN_L` (3 L, or a percentage for small batches), the booster's rate with the drip pipe connected (type the LCD `seen:` value after a real delivery into **Rate Booster**), `FILL_SETTLE_MS`, and the mixer's position.

#### 19.4.9.14. Testing, Fill Res>Mix and Push>Col: 1 minute of inverter time (user, 17 Nov)

A held Fill or Push combo may run **1 minute**, counted only while the inverter is switched on (`TEST_AC_RUN_CAP_MS`). The servo pre-seat, the battery switch and a re-seat's swings do not use it up. If the inverter never comes on, the combo still stops 90 s after the press (`TEST_AC_START_MAX_MS`). ESP2 answers `DONE,TEST_CAP|run_s=..`. Single relays keep their 30 s, and the other combos (servo moves, mixer) keep 60 s from the press. The website's timed test and hold-to-run keep their 15 s.

#### 19.4.9.15. Pump estimate screens: LCD → Settings → Calibration → "Transfer pump est" / "Booster pump est" (user, 17 Nov)

These screens re-measure a pump's line the way the user did.
- The booster screen first asks which column valve to open (UP/DOWN, ENTER).
- Set the seconds with UP/DOWN (3–60 s; holding repeats, in 5 s steps after 2.5 s). The screen shows what the current line says that run gives: `Run 10 s  est  2.43L`.
- ENTER runs the pump. ESP2 times it from the inverter **switch-on** (`PRIME_START,RESMIX,,<ms>` / `PRIME_START,MIXIRR,<col>,<ms>`, a timed prime) and answers `DONE,PRIME_TIMED,<line>,<ran ms>,<re-seat>,<ms to AC>`. BACK stops it.
- Weigh the water. The screen starts the entry at the estimate in grams; set the real net grams with UP/DOWN (10 g steps, 100 g when held). ENTER keeps the try; BACK drops it. A run with a selector re-seat, or with no AC, cannot be kept.
- From the **5th try** on (up to 10 kept; the oldest goes first), the screen shows the fitted line: `Fit 11.74 L/min n=5`, `start +0.47 L`. ENTER saves it; UP adds more tries. Vary the seconds between tries: run times less than 1.5 s apart cannot give a rate.
- Saved, the rate becomes **Rate Transfer** / **Rate Booster** and the start litres are kept (NVS `xStartL` / `bStartL`). The transfer's start litres also go to ESP2 (`SET_CAL,PSTART`, NVS `xS`), so every fill is timed by the new line. The booster's rate sizes a delivery's checks.
- **Defaults:** until a pump is re-measured, its line is fitted at boot from the user's own readings (`shared/pump_line.h`, 1 L = 1028 g): transfer **11.74 L/min, +0.47 L**; booster **13.20 L/min, −0.65 L** (both columns, open valve). The drain checks keep their cautious 1.0 L/min until a booster line or rate is saved.

#### 19.4.9.16. 1.5 s between button runs (user, 17 Nov)

Pressing a Testing or Calibration button again and again switched the relay bank and the master cut-off (P17) so fast that the PCF8575 faulted. After a button that ran something is released, the next press is ignored for 1.5 s (ESP1 `ACT_REARM_MS`). The Testing header shows `TOO FAST: WAIT 1.5s`, the Calibration screens `Too fast: wait 1.5s`, and that press must be made again after the gap. This covers the Testing dead-man rows, the tank-litres UP/DOWN pumps, the hold-to-prime rows and the pump-estimate runs. ESP2 keeps the same 1.5 s gap as a backstop (`TEST_REARM_MS`, `PRIME_REARM_MS`): it ignores a new Testing selection or a new prime that comes sooner, the website's hold-to-run included.

#### 19.4.9.17. Every timed pump run counts from the inverter switch-on (user, 17 Nov)

ESP2 sends `INFO,INV_ON,<pump>` the moment it switches the inverter relay on. Until then the servo pre-seat and the battery switch take their few seconds, and none of that counts.
- **LCD Transfer / Booster pump est:** the screen shows `Starting  0.0/ 7 s` until INV_ON, then counts up from 0. It holds at the set seconds; ESP2 switches the inverter off exactly then and reports the time it ran.
- **Irrigation fill:** the run time is from the switch-on. It now also stops on time when a level line is late at that moment.
- **Timed recovery stages and the pump exercise:** timed from the switch-on.
- **Dashboard Fill/Push timed test:** its seconds start at INV_ON (before, at the pump's AC).
- **Testing Fill/Push combos:** their 1-minute limit counts only while the inverter is on.
- **If the inverter gives no AC and the start is retried:** the count starts again at the new switch-on.

#### 19.4.9.18. Hold buttons and the 1.5 s gap, refined (17 Nov)

- **The fault:** relays pulsed and stopped while the button was still held. The Testing screen read ENTER raw, so a one-pass blip (contact bounce, or relay and inverter switching noise) counted as a release. The 1.5 s gap then refused the press that was still being held.
- **ESP1:** every hold-to-run button now counts a release only after it has been up for 120 ms (`BTN_RELEASE_MS`, `holdBtnRead`). This covers the Testing rows, the tank-litres UP/DOWN pumps and hold-to-prime.
- **ESP2:** it starts its gap only at a real `TEST,RELEASE` or `PRIME_STOP`. A late keep-alive no longer starts the gap, so the hold picks up again at once.

#### 19.4.9.19. Tank readings of -1 (17 Nov)

- **What -1 meant:** no TANK line from the Nano since ESP1 last started. ESP1 asked the Nano for a fresh cycle only once after a restart, that request can be lost on the noisy line, and in the day the Nano's own cycle is 20 min. On 17 Nov at 14:52 the dashboard showed the Nano's heartbeat arriving but no tank, temperature, light, soil or NPK data since ESP1's restart.
- **ESP1:** repeats the request every 30 s until a TANK arrives (`NANO_FIRST_DATA_RETRY_MS`).
- **Nano:** sends a TANK with every heartbeat when idle (1 min by day, 2 min at night).
- **LCD home page:** shows why a tank has no reading instead of `-1%`: `wait` (nothing yet since this restart), `echo?`, `stuck`, `range`, `noisy` or `noPng`. These are the Nano's pinger status letters, the same as `st=` in the log's TANK rows.

#### 19.4.9.20. Fixes from the 17 Nov afternoon log (`20261117.CSV`, 14:09–14:59)

- **Tanks -1: the ultrasonics timed out.** From the moment the v.4 Nano was flashed (14:09), every TANK line said `st=EE` (no echo on both sensors). The v.3 Nano had read 23–25 cm / 33–42 cm all morning. v.4 had cut the echo wait from v.3's 25 ms to 12 ms, and these modules start their echo late. Now 25 ms again, and each sensor is pinged every 100 ms (was 70 ms).
- **Temperature 0.0 °C / 0 %:** 67 DHT reads from 12:17 were exactly 0/0, a failed read that passes the checksum. That would have skewed the tanks' speed-of-sound correction by about 5 %. The Nano now sends −1 for it, and ESP1 refuses 0/0 from an older Nano (`ENV|…|DHT_ZERO_NOT_USED`). Check the DHT22 wiring.
- **Pump-estimate runs ending early (`DONE;PRIME` mid-start):** while a pump starts, ESP2 reads the PZEM with the inverter still off. An unpowered PZEM costs about 108 ms of Modbus timeout per read, and ESP2's loop then missed the 400 ms dead-man. Inverter rows and lines now use 1.2 s (`TEST_HOLD_TIMEOUT_AC_MS`, `PRIME_HOLD_TIMEOUT_AC_MS`). A real release still ends them at once.
- **Log flooding:** hold keep-alives were written to the SD card every 150 ms (3,619 `TEST;HOLD` and 779 `PRIME_START` rows). Only the first of each hold is logged now (`sendEsp2KeepAlive`).
- **The Nano reset at every ESP1 restart:** the once-a-day jobs (03:00 Nano reset, 18:00 report) forgot they were done when ESP1 restarted. Their day is now kept in NVS (`nrsDay`, `rptDay`).
- **Restart reason:** every ESP1 boot logs `SYS,BOOT,reason=POWER_ON|SOFTWARE|PANIC|TASK_WDT|BROWNOUT…`. The afternoon had 14 restarts and no way to tell a flash from a crash.
- **To check by hand:** the mixing-tank sensor read 104–108 cm from 10:07 to 14:09, far below the tank's 42.9 cm empty mark. Check its mounting and aim. ESP1 does not use readings that far out.

#### 19.4.9.21. Forced runs and the tank check: no limit (user, 17 Nov)

**The rule (user, 17 Nov):** no limit on forced runs.
- **What had blocked them:** not a daily limit, but the tank check. When ESP1 restarts in the middle of a run (it keeps `runlive` in NVS while one is live), it raises `TANK_CHECK|ESP1_RESET_DURING_RUN` (Major): the mixing tank may still hold that run's water, and ESP1 no longer knows how much. Runs then wait until the operator confirms the tank.
- **Now:** a forced run is never refused for it, and starting one confirms the check (`TANK_CHECK|CONFIRMED|by=FORCE_RUN`). Scheduled runs still wait. Confirm with SMS `TANKCHECK,OK` (while idle), on the LCD, or with a forced run.
- **What can still drop a forced run** at the end of its countdown (`FORCE|ABORT|<reason>`): `ACTUATIONS_OFF`, `NOT_IDLE`, `IN_MENU`, `WEB_MANUAL`, `BUSY`, `HELD`, the battery reasons (`BATT2_LOW`, `BATT2_MONITOR`, `BATT2_NOT_SEEN`, `BATTSW_FAULT`, `BATTSW_RECHARGE`, `MAIN_BATT_CRIT`), `RES_LOW`, and `COL_DISABLED` (a column switched off during the countdown, §19.4.9.26).
- **`BATTSW_RECHARGE`:** AC pumps wait `BSW_RECHARGE_MS` (60 s) after every ESP1 restart. A forced run armed in that first minute is dropped; arm it again after the minute.

#### 19.4.9.22. Live website and Refresh (17 Nov)

- **The viewer signal:** while a dashboard page is open and visible, it writes `irrigation/viewer/at` (a server timestamp) every 15 s. ESP1 polls that node with its commands (every 3 s). While it keeps changing, ESP1 uploads `irrigation/live` every 2 s during a run and every 4 s otherwise (`FIREBASE_UPLOAD_LIVE_*_MS`), instead of every 20 / 60 s. It returns to the slow pace 45 s after the last touch (`FB_VIEWER_HOLD_MS`).
- **The Refresh button** writes `irrigation/viewer/refreshAt`. ESP1 uploads at once (at most every 1.5 s), and the page says how long the answer took.
- **What the snapshot carries:** `meta.refreshMs` / `meta.live` (the pace), `system.firmware`, and the tanks' pinger status (`sensorsRaw.tank.reservoirStatus` / `mixingStatus`). A tank with no reading says why on the page.
- **Rules:** approved accounts and the operators may write only `at` and `refreshAt`, each within the last minute, and `refreshAt` at most every 2 s. ESP1's device account reads the node. Deployed 17 Nov.

#### 19.4.9.23. SD card logs through Firebase (17 Nov; the Supabase project expired)

- **What goes up:** each day's CSV goes to the Realtime Database that serves the website, as text pieces `logs/files/<yyyymmdd>/<nnnn>` of 6 KB (`LOG_CHUNK_BYTES`; 12 KB until the evening of 17 Nov), plus `logs/index/<yyyymmdd>` = `{state: uploading/done/missing/failed, size, chunks, partial, at}`.
- **How it is sent:** non-ASCII and control bytes (line noise) become `?`; everything else is unchanged. ESP1 sends one piece per network-task pass, so commands and the live report keep flowing. A 1 MB day is 171 pieces, about 2–3 minutes.
- **Which days:** finished days go up by themselves, oldest first after `fbLogLast` (NVS `fblogl`). The website's **Upload from the rig** (`irrigation/logRequest = {day, at}`, polled every 10 s) asks for any day on the card, today included as far as it is written (`partial`). Firebase keeps `LOG_KEEP_DAYS` (14). Older days are deleted there and stay on the SD card only.
- **Supabase** remains the target only while Firebase is off.
- **Who can see them:** operators and the privileged tier can read the logs (they hold SMS texts and phone numbers). Only ESP1's device account writes them. On the website: Diagnostics → SD card logs (list, Download, Upload from the rig).
- **Manual/Test → SD card log (CSV)** (user, 17 Nov: logs at any time of the morning). **Update today's log** asks for today's log so far, through the same `irrigation/logRequest`. It works only while ESP1 is reporting, idle (`IDLE_STATE` or `SAFE_MODE`), with no run, pump test, pump or mixer on; otherwise the button is off and says why. **Download today's log** works once it is sent. The badge follows `logs/index/<today>`: NOT SENT → ASKED → SENDING → SENT (SO FAR), or FAILED / NOT ON CARD.
- **Heap (fixed the evening of 17 Nov).** The first version kept the 12 KB piece in a `static` buffer, which took 12.4 KB of heap for good (static RAM 94,432 B, against 82,040 B before). During runs ESP1 then had 55–60 KB free, under `FIREBASE_MIN_HEAP` (60 KB), and skipped every Firebase request. After the 17:58 flash no restart reached `FBASE,OK`, and every log upload failed (`UPLOAD,FAIL|20261103.CSV|http=-1`). Now each piece is read in 512-byte blocks (`LOG_READ_BLOCK`) straight into its JSON text, which is freed after the request; static RAM is 82,184 B again.
- **Upload robustness (same evening).** A log upload starts or goes on only while the live report reaches Firebase (`fbLastOk`), and a piece waits for a free block of about 1.25× its size. A dropped or refused connection is retried on a fresh socket up to 3 times per step (`FB_LOG_NET_TRIES`). A failure logs where it stopped: `UPLOAD,FAIL|<day>.CSV|http=..|try=..|FIREBASE|step=start/pieceN/done|heap=..|blk=..`.

#### 19.4.9.24. LEVEL_BLIND on forced runs: ping spread, and a fill that loses its level (17 Nov)

- **First explanation (only part of it):** the LCD shows every echo, but ESP1 sends ESP2 only readings it trusts for control. While the transfer pours in, the mixing tank's ripples spread the Nano's pings past `ULTRA_MAX_SPREAD_CM` (2 cm). ESP1 then sent −1, and after 10 s with no accepted sample ESP2 held the fill as LEVEL_BLIND. The failure in the very first second was a timer bug (§19.4.9.25). The spread is real, though: on the evening runs the mixing tank's pings spread 5–9 cm through most of each fill and delivery (§19.4.9.27).
- **The fixes:** the spread limit is now 4 cm (ESP2's 5-sample median and jump gate still filter on top). A fill that loses its level mid-run no longer holds: it carries on by its run time, cut to leave `FILL_BLIND_MARGIN` (25 %) of the room below the overflow limit (`DEGRADED,LEVEL_BLIND|FILL|…`). The settle afterwards measures, and a top-up makes up the rest. The starting baseline gets 60 s (was 30 s) to collect its 5 settled samples.
- **Diagnosis:** while ESP1 relays levels, a reading it does not pass on is logged at most every 10 s as `NANO,LEVEL,NOT_USABLE|mix=<why>|res=<why>|cm=..|s=..`. The reasons are `pings_spread`, `beyond_empty`, `status_E_no_echo`, and so on. On the LCD home page such a reading shows with a `?` (for example `Mix:40%?`).

#### 19.4.9.25. LEVEL_BLIND, the real cause: a timer bug, `t | 1` (17 Nov, log 17:32:04 and 17:40:38)

- **What happened:** both forced runs held LEVEL_BLIND|NO_SETTLED_LEVEL in the same second they started, while the mixing tank read a steady 40.7 cm (spread 0 cm).
- **Why:** `levelBaseline()` stored its start as `now | 1` (so it is never 0). For an even `now`, that is `now + 1`, a stamp 1 ms in the future. The same call then computed `now - start`, which wrapped to about 49 days and exceeded `LVL_BASE_MAX_MS` at once. So about half of all fills failed instantly.
- **The fix:** every such stamp (10 in ESP2, 13 in ESP1) now uses `nzMs(t)` (`t ? t : 1`), which is never 0 and never ahead of the clock.
- **The same bug elsewhere:** the Testing Inverter row's 6 s AC check, the delivery's 10 s floor countdown, the wrong-pump verdict's 2 s confirmation, the hold-button release filter, ESP1's 97 % overflow backstop grace and the Nano FAST-gap watchdog could all have fired early in the same way.

#### 19.4.9.26. Forced runs on a switched-off column (17 Nov, 17:59–18:05: `FORCE|ABORT|COL_DISABLED` ×3)

- **What happened:** at 17:41:06, after the level-blind hold, "Turn OFF column" was chosen in the LCD cancel menu (`CANCEL|OFF|COL_A`). The LCD then armed each forced run on Column A, counted down, and dropped it at fire time.
- **The fix:** arming an LCD forced run switches a chosen column that is OFF back ON (`FORCE|COL_ON|COL_A`, saved). The editor's bottom row warns first: `Col A   OFF, ARM=ON`. `forceTick()` still drops a column switched off during the countdown. The website still refuses a forced run on an OFF column, but now says how to switch it on.
- **Also fixed:** `cancelRun()` logged its reason after clearing `lastHoldCode`, which the reason often points into. It now keeps a copy.

#### 19.4.9.27. Field check, 17 Nov evening (18:11–18:30)

With the 17 Nov fixes flashed, `20261117.CSV` from 18:11 to 18:30:

| Armed | Run | What happened | Receipt |
| --- | --- | --- | --- |
| 18:13:48 | Col A, 5 L | `FILL_RUN\|need=3.35\|run_s=14.7` (base 1.65 L). The inverter came on 11 s later (`INV_ON`: the selector and the battery switch go first). `FILL_CHECK\|goal=5.00\|measured=6.17\|missing=-1.17\|OK`. Booster on at 18:15:50. | none: ESP1 restarted at 18:16:41 (power) during the delivery → `TANK_CHECK` |
| 18:19:12 | Col A, 2 L | The tank still held the last run's water, so no fill: straight to Release. The booster emptied the tank to under 5 % + 10 s. | `OK\|WATER=5.79of2.0`, 81 s |
| 18:26:07 | Col B, 5 L | `FILL_RUN\|need=4.92\|run_s=22.7` (base 0.37 L). `FILL_CHECK\|goal=5.29\|measured=5.63\|missing=-0.34\|OK`. | `OK\|WATER=5.63of5.0`, 164 s |

**Works:** forced runs on both columns; the run-time fill with its settle and check; the delivery to under 5 % + 10 s; the `NANO,LEVEL` lines. No LEVEL_BLIND.

**Still open:**
- **ESP1 power resets.** `SYS,BOOT,reason=POWER_ON` at 18:11:03, 18:16:41 (during a delivery), 18:18:25, 18:24:39 and 18:30:24, and 14 times in the afternoon. POWER_ON means the supply dropped out, not a crash. Earlier logs tied them to SMS sends. Give ESP1 and the A7670C a 5 V supply of at least 2 A, and a 1000–2200 µF low-ESR capacitor at the modem's supply pins.
- **Two forced runs dropped as `BATTSW_RECHARGE`** (18:17:39, 18:25:33): armed within a minute of a restart (§19.4.9.21).
- **The mixing tank's pings spread 5–9 cm** during fills and deliveries (`NANO,LEVEL,NOT_USABLE|mix=pings_spread`, readings jumping between 26 and 42 cm). Near empty, the sensor also sees the mixer (its top is about 31.5 cm below the sensor) and the inflow jet. The runs still finished, but aim the sensor away from the mixer and the inflow.
- **The first fill overshot** by 1.17 L: 4.52 L came in for a 3.35 L plan (+35 %). The second came in 0.34 L over (+7 %). The line's start litres or rate may be low for a fuller reservoir (more head). Re-measure the transfer line with the reservoir at its usual level (two run lengths, or the LCD pump estimate, §19.4.9.15).
- **Water left after a restart goes to the next run's column.** The 18:19 run asked for 2.0 L on Col A and delivered the 5.79 L the interrupted run had left, because a delivery empties the tank. It was Col A's own water this time. It would matter if the next run were another column or a fertigation, which is what the tank check is for.
- **Firebase was down from the 17:58 flash on**: the log upload's static buffer (§19.4.9.23). Fixed that evening; ESP1 needs flashing again.

### 19.4.7.1. 🔹 Secondary Battery Isolation (P4 DPDT) — additional feature

**Purpose.** While the inverter runs the AC motors (transfer, booster, mixer), the controllers are momentarily moved off the main solar battery onto a **secondary battery**, so they share no electrical connection with the battery the inverter is loading. A **DPDT** breaks **both** the positive and the negative conductor, so no ground path remains either, and the sensor readings taken during a run stay clean.

**Hardware.**
- **Secondary battery:** 4S Li-ion, 16.8 V full (14.8 V nominal). It is charged from the main battery while the DPDT is in the main position (idle).
- **What moves to it:** ESP1, ESP2, the Nano, the sensors and the relay coils.
- **Switch:** P4, the relay of the condemned mixing-tank valve, drives the DPDT. De-energized = main battery, which is the fail-safe position. A fault, an E-stop, an ESP2 reboot, or ESP1 cutting ESP2's power (GPIO4) all return the boards to main.
- **Monitor:** an **INA226 with an R010 shunt** on **ESP1's I²C at 0x40** measures the secondary's voltage and current. It is calibrated with `Calibration/ESP1/INA226` (module `sec`).
- **Plumbing:** the mixing tank feeds the booster directly and the column valves select the column. The reservoir valve is no longer fitted, so P0 is a spare.

**Sequencing (ESP2).** The rule is that inverter ON implies the secondary battery, for every inverter use (run stages, mixing, exercise, AC prime, and Testing / Timed Pulse / hold-to-run):
1. P4 switches the boards to the secondary.
2. After `BATT_SWITCH_SETTLE_MS` (500 ms), the inverter starts.
3. The existing `INVERTER_WARMUP_MS` follows, extended by the settle time (`AC_WARMUP_MS`).
4. When the inverter stops, P4 returns the boards to main after `BATT_RETURN_DELAY_MS` (1000 ms). On a normal end, P17 stays up until that return; faults cut everything at once.

ESP2 reports `INFO,BATT,SECONDARY` and `INFO,BATT,MAIN` on each switch. ESP1 logs them, publishes them, and holds off cutting ESP2's power until the return (`ESP2_BATT_RETURN_WAIT_MS`, 4 s bound).

**Policy (ESP1).** Thresholds are [CONFIRM]:
- **Block:** with switching enabled, no inverter use may *start* when either of these holds. A run already in progress is never aborted.
  - The secondary is **low**: its **resting** charge is 30 % or less, released again at 35 % (user, 2026-11-17; was 13.6 V / 14.0 V raw).
    - **Resting charge.** The pack reads high while the charger pushes current in and low while the boards draw from it. ESP1 therefore uses resting V = V + I × R (+ = discharging) for the B2 % on the LCD, the dashboard % and this block.
    - **R is learned at every switch to the secondary:** (V just before − V just after) ÷ (I after − I before). It is smoothed, kept in NVS `secR`, starts at 0.25 Ω, and each change is logged as `PWR2,BATT2|RINT|…`. The minute-by-minute `PWR2` lines also carry `REST=`, `PCT=` and `R=`.
    - **The reason is named.** Every refusal names its cause on the LCD instead of a generic "BATT2 low": `BATT2 low: 28%`, `BATT2 monitor off`, `BATT2 not seen: wire`, `Batt switch fault`, `Restarted: wait 42s`. The logs carry `BATT2_LOW`, `BATT2_MONITOR`, `BATT2_NOT_SEEN`, `BATTSW_FAULT` and `BATTSW_RECHARGE`, and a scheduled run that is held back logs `CTRL,AC_BLOCKED|<reason>` once.
  - The INA226 is not answering.
- **Alerts (Major):** `BATT2_LOW`, `BATT2_CRIT` (below 12.8 V) and `SENSOR_FAIL,INA226`.
- **Readings:** published to the Firebase live snapshot (`sensors.secondaryBattery*`, `system.boardSupply`), the LCD power page (`B2:` line), the SMS `STATUS` report (`BAT2`) and the SD log (`PWR2`).

**Prerequisite — not yet fitted.** A DPDT transfer leaves the boards unpowered for about 10–20 ms. A hold-up stage after the DPDT (e.g. a supercapacitor or UPS module) must keep all three controllers, the P4 relay module's coil and the DPDT coil powered through the transfer. Without it, every switch resets the controllers and the relay falls back to main. For that reason the switching is compiled **off** in both firmwares (`SECONDARY_BATT_ENABLED 0`, ESP1 and ESP2 — set both together). Monitoring works regardless.

**When the hold-up fails anyway (reset-proofing).** Every switch is handled as a moment at which the controllers may reset:
1. **Announce:** ESP2 sends `INFO,BATT,SWITCHING,<to>`.
2. **ESP1 prepares:** it flushes its SD log, writes a switch journal to NVS, and stops SD writes, new WiFi/TLS requests and SMS sends for up to 3 s. That keeps the load on the hold-up low and leaves nothing half-written. Then it replies `BATTSW,GO`.
3. **ESP2 prepares:** it saves the mixing-tank volume and its own journal to NVS, and only then moves P4.
4. **Confirm:** once the boards have survived the 500 ms settle, both journals are cleared (`INFO,BATT,SECONDARY|MAIN`).

**Counting resets.** A boot that finds a journal still set knows the switch reset it. ESP2 reports `INFO,BATT,RESET,<to>` until ESP1 acknowledges. A `READY` from ESP2 while a switch is pending counts the same way. Each such event is one **strike**:
- **Two strikes within 24 h** latch `BATT_SWITCH_FAULT`, and every inverter use is refused. A failing hold-up can therefore reset the rig at most twice, never in a loop.
- **The interrupted run re-runs when still due.** ESP2's tank volume survived in NVS, so it only tops up what is missing.
- **Clearing the latch:** LCD Settings → Batt Switch, SMS `BATTSW,CLEAR`, or the dashboard System tab (`CLEAR_BATT_SWITCH`). It also clears itself 24 h after the last strike when the RTC is good.

Independently of the battery switch, any `READY` from ESP2 during a live run, held fault or exercise is now handled as "ESP2 restarted". The job is released at once (`ESP2_RESTARTED`, then `STARTUP_SYNC`). Before this, it waited for the 10-minute completion timeout and ended in an emergency stop.

**A real blackout during a switch.** If the boards actually lose power for a moment, the DPDT coil drops with them and all three controllers restart on main. Nothing decided before the reset is lost or repeated:
- **Recharge wait:** no inverter use for `BSW_RECHARGE_MS` (60 s, [MEASURE]) after a boot or strike, because the hold-up is flat.
- **No double dose:** ESP2 keeps what the tank already holds in NVS, and a re-run doses only what is still owed. A dose started but not finished counts as delivered, so the error is always towards under-dosing. No dose starts until a switch back to main has finished.
- **The batch reaches its column:** the column the tank was dosed for runs first, regardless of its window or soil reading, except at night or if the operator snoozed, disabled or skipped it. A batch stopped on purpose is not restarted automatically, but its doses still count when the column next runs. An order for a different column adds no dose and raises `DOSE_CARRY`.
- **Faults, stops and commands:** a fault is written to NVS before its own P4 drop and reported after the reset, and its column is skipped for the day. An E-stop or cancel is written down before its cut and restored after the reset. A dashboard command in flight is marked failed.

**Backup for a critical main battery — ready, not active** (`SECONDARY_BACKUP_ENABLED 0`, ESP1 and ESP2). When main stays below `BATT_CRIT_V` for 60 s with nothing running, ESP1 texts `ALERT,CRIT,MAIN_BATT_CRIT`, waits for the text to leave the modem, and has ESP2 hold the boards on the secondary (`BATTBK,ON`). ESP2 stays powered and holds P4 and P17; all inverter use is blocked from the alert onward, because the inverter would load the flat main battery. The boards return when main holds `BACKUP_MAIN_RECOVER_V` (12.6 V, [CONFIRM]) for 5 minutes, or when the pack reaches `SEC_BATT_CRIT_V` or stops being monitored (`ALERT,CRIT,BACKUP_EXHAUSTED`). Needs `SECONDARY_BATT_ENABLED` and a calibrated main voltage. **Limits:** the pack does not charge while it powers the boards; if the boards return to a main too weak to run them, the rig stays dark until main recovers; and the charger needs its own low-voltage cutoff so it does not drain a critical main.

### 19.4.8. 🔹 P17 Master Actuator-Power Cutoff & Fault Hold

P17 drives a master relay that supplies power to the entire P0–P16 control-relay bank. When P17 is de-energized, ALL actuators (pumps, valves, mixer) lose power regardless of their individual PCF output states. This is a single, reliable hardware cutoff for fault conditions.

**Wiring requirement (FAIL-SAFE):** the relay bank and loads MUST be wired so that loss of P17 power = all actuators OFF (loads on normally-open contacts). No load may sit on a normally-closed contact, or it would energize when P17 drops.

ESP32 #2 energizes P17 at the start of any actuation (work order, preventive exercise, testing entry) and de-energizes it on completion, on a fault, and at idle.

#### 19.4.8.1. Fault Hold (ESP32 #2 stays alive, tank-aware)

On a hard fault that warrants stopping actuation (FLOW_FAIL, PWR_FAIL, SAFE_STOP), ESP32 #2 on its own authority:

1. Sets all P0–P16 outputs to SAFE/OFF and **de-energizes P17** (bank hardware-dead).
2. **PAUSES** the sequence, keeping the work order, the current step, and the metered **mixing-tank volume**.
3. Reports the fault to ESP32 #1 (fault code + location).
4. **STAYS ALIVE and HOLDS** — ESP32 #2 does NOT power itself off, and ESP32 #1 keeps it powered (does NOT cut GPIO4). Powering it down would forget the water already in the mixing tank and overfill on resume.

The mixing-tank volume is **measured by the mixing-tank ultrasonic** (web v.4, §19.4.9) — a settled reading before every fill and every delivery, the measured level after — and the last figure is **persisted to NVS**, so even an unexpected reboot/brown-out cannot make ESP32 #2 assume an empty tank and overfill. (Before web v.4 it was metered by the ResMix / MixIrr flow sensors and the ultrasonic was display-only.) The mixing tank's only outlet is the booster pump → a column (no drain line), so held water is ultimately delivered to a column.

#### 19.4.8.2. User-Gated Recovery (GSM / LCD)

ESP32 #1 alerts the user over GSM with the fault, the operation (irrigation/fertigation), and the column, then waits for one of four replies (also selectable on the LCD fault screen):

* **STOP** — acknowledge & keep holding (delivers nothing; waits for a later RELEASE/IRRIGATE/NORMAL).
* **RELEASE** — re-energize P17 and dump the current mixing-tank contents to the assigned column as-is (no top-up, no dosing), emptying the tank.
* **ONLY IRRIGATE** — re-energize P17, top up plain reservoir water to the column's irrigation budget (minus what is already in the tank), deliver, skip remaining dosing.
* **NORMALLY** — re-energize P17 and resume the paused sequence from where it stopped (fill only the remaining liters, no re-dose), then deliver and flush.

There is no automatic resume after a fault; the user must decide. While held, ESP32 #2 refuses new sequence/exercise/test commands with `BUSY,HELD`.

#### 19.4.8.3. Hardware Backstop Retained (GPIO4)

ESP32 #1's GPIO4 relay (ESP32 #2 power) is retained as a backstop for a FROZEN ESP32 #2 and for the manual MODE+BACK emergency stop: cutting GPIO4 de-energizes the PCF8575 (which drives P17), so the actuator bank goes hardware-dead even if ESP32 #2's firmware has failed.

#### 19.4.8.4. Testing Mode Interaction

Because P17 gates all actuator power, testing mode (Section 18.10.8) energizes P17 on entry (so manual relay control works) and de-energizes it on exit. P17 stays energized as the resting state throughout testing so each relay under test actually receives power; the master-cutoff entry in the test list is therefore REVERSED — holding it de-energizes the bank (to verify the cutoff), and releasing restores power.

---


## 19.5. 🔹 System Reliability & Reset Strategy

ESP32 #2 is responsible for executing commands and must operate reliably under continuous conditions.

---

### 19.5.1. Watchdog Timer (Primary Protection)

ESP32 #2 uses a watchdog timer to automatically reset if the system becomes unresponsive.

Behavior:
- If the main loop fails to execute within a defined time:
  → System automatically resets

Purpose:
- Recover from software lockups
- Prevent actuator deadlock
- Ensure continuous operation

---

### 19.5.2.  Command Execution Safety

All actuator operations must remain non-blocking to ensure watchdog servicing and flow interrupt responsiveness.

- Receives command from ESP32 #1
- Executes full sequence
- Sends "DONE" upon completion

If system becomes unresponsive:
- Watchdog reset restores operation

---

### 19.5.3. External Recovery (Controlled by ESP32 #1)

ESP32 #2 can be reset externally via power cycling:

- ESP32 #1 disables relay → power OFF
- ESP32 #1 enables relay → power ON

Used when:
- No response to commands
- Watchdog recovery is insufficient
- Hardware-level reset is required

---

### 19.5.4. Startup Behavior

Upon reset or power-up:
- All outputs default to SAFE state (OFF)
- PCF8575 outputs initialized HIGH (inactive)
- System waits for valid command

---

### 19.5.5. Design Principle

- Fail-safe operation
- No actuator remains ON unintentionally
- Recovery handled automatically without user intervention

---

# 20. 🔌 Power Architecture

## 20.1. Logic Power

* ESP32 → 5V (VIN)
* Arduino Nano → 5V
* PCF8575 → 3.3V

## 20.2. Load Power

* Pumps and valves → 12V
* Relay modules → 5V

## 20.3. Important Rules

* All grounds must be COMMON
* Do NOT power loads from ESP32
* Use flyback diodes for pumps and valves

## 20.4. A7670C Power Requirement

- Breakout board takes 5–12 V from its own supply; common GND with ESP32 #1
- Never above 12 V: a 12 V battery charging or the 16.8 V secondary needs a buck converter
- Supply rated 2 A
- MUST NOT be powered from the ESP32 5V pin
- Must follow the same battery-switched rail as ESP32 #1 once SECONDARY_BATT_ENABLED is on: the DPDT breaks both + and −, and the UART needs that common ground

---

# 21. 🔌 Power Monitoring System

## 21.0. Solar + Main Battery Monitoring (two INA226 behind an ISO1540)

An INA226 must share a ground with what it measures, while the boards must not share one with the main battery. Two modules therefore sit on the main-battery side of an **ISO1540** I²C isolator and stay readable through every inverter run and battery switch. Nothing but the isolator crosses over. (The part number matters: an ISO1450 is an RS-485 transceiver and passes no I²C.)

| Module | Address | Address pins | Shunt | CAL | Current LSB | Clips at |
|---|---|---|---|---|---|---|
| Solar | 0x41 | A1 GND, A0 VCC | R010 on the module | 2048 | 0.25 mA | 8.19 A |
| Main battery | 0x44 | A1 VCC, A0 GND | 50 A / 75 mV bar (1.5 mΩ) | 1707 | 2 mA | 54.6 A |

**Wiring**

| ISO1540 side | Pins | Connects to |
|---|---|---|
| Side 2 (boards) | SDA2, SCL2 | ESP1 GPIO21, GPIO22 |
| | VCC2 | The voltage ESP1's bus is pulled up to (3–5.5 V). Measure SDA at idle: an LCD backpack may pull it to 5 V |
| | GND2 | Board GND |
| Side 1 (main battery) | SDA1, SCL1 | Both modules' SDA, SCL |
| | VCC1 + module VCC | 3.3 V from a regulator fed by the main battery |
| | GND1 + module GND | Main battery negative |

- Fit 100 nF on each VCC, close to the pins.
- **Why side 2 faces ESP1:** side 1 pulls low to only 0.57–0.8 V, against an ESP32 input limit of 0.825 V at 3.3 V. Side 2 reaches 0.4 V. An INA226 reads side 1's low comfortably (its limit is 0.3 × VCC, about 1 V).
- **Side 1 pull-ups** must sink 0.5–3.5 mA. The two modules' 10 kΩ resistors in parallel give 0.66 mA at 3.3 V, which is enough. If they are removed, fit 1–6.6 kΩ.
- **Main-side 3.3 V, not an isolated converter from the boards:** a converter's interwinding capacitance is the path the inverter's noise already takes, and this way the hold-up carries no extra load.
- **Current sensing is high side:** IN+/IN- in series with the + line, VBS tied to IN-. The module's own R010 must be **removed** on the main battery module. Left in, it parallels the bar: readings come out about 13 % low and it carries about 6.5 A at 50 A.
- **Solar limits:** the INA226 is rated to 36 V and the R010 clips at 8.19 A. Measure on the charge controller's battery side, whose negative is common with the main battery; panel negative may not be.

**Behaviour.** The main INA226 is the first choice for main battery V/I/P, and the opto ADC + ACS758 (§21.1) takes over automatically if it stops answering. A main reading below 5 V is treated as a wiring fault and not used; solar has no such floor, because 0 V at night is real. Neither module blocks a run or sends a text. With the shunt bar, current is signed (+ = discharging), so `CONS` counts discharge only; `CHG` is solar energy.

A module that loses its supply for a moment answers again with its calibration register at 0, which reads a healthy voltage and exactly 0 A. ESP1 reads that register back on every pass and restores it (`INA226|<name>|RESTARTED`). The secondary's monitor does the same.

**Calibration.** `Calibration/ESP1/INA226`, flashed to ESP1, calibrates all three modules and saves to NVS `calib`, which `loadCal()` applies over the compiled constants. A value outside what the tool itself would accept is ignored and the compiled value used.

## 21.1. Battery Monitoring Fallback (calibrated opto ADC)

- Battery **voltage (GPIO35)** is sensed through an optocoupler into the ESP32 ADC (ADC1, 12-bit, `ADC_11db`).
  The optocoupler makes the raw counts NONLINEAR, so it is fitted with a **least-squares polynomial**
  `value = a0 + a1·u + a2·u² + a3·u³`, `u = raw/4095`, capturing the ADC + opto curve end-to-end. Current comes
  from the ACS758 through the ADS1115; the old GPIO34 opto current channel is no longer read.
- **Calibration:** `Calibration/ESP1/BatteryOpto` (apply known voltages, capture points, compute) saves to NVS
  namespace `adccal`, which `loadBattCal()` reads at boot, and prints the coefficients for `calBattV`.
  `Calibration/ESP1/ACS758` covers the current sensor.
- On this fallback, current is **magnitude only**, so energy is counted purely as consumption (no
  charge/discharge split).

### 21.1.1. Control Usage: READY, NOT IMPLEMENTED
Battery voltage drives no control action by default. The protections below are **scaffolded** behind the
compile-time flag `BATTERY_SAFETY_ENABLED` (default `0`) — the code is present but compiled out; set it to
`1` to re-enable them (in `powerTick`/`batterySafetyTick`, `controlTick`, `exerciseTick`):
- Low battery → Disable fertigation (irrigation only)
- Critical battery → Stop all operations (emergency-stop)
- GSM alerts BATTERY_LOW / BATTERY_CRITICAL
- Low battery → skip the preventive pump exercise

(The old INA226 device-loss auto-reboot is moot — battery is now read from the ADC, not the INA226 — so it
is not scaffolded.)

Battery is otherwise **monitoring only**: voltage/current/power to the LCD, `NET`, ThingSpeak (System channel),
and the daily energy Wh in `SUMMARY`/`FULL SUMMARY`.

## 21.2. AC Monitoring (PZEM-004T)

- Installed at inverter output (220VAC side)
- Wired to ESP32 #2 on hardware UART1 (GPIO13 TX / GPIO14 RX), 9600 baud (see Section 19.2.1)
- Measures:
  - Voltage, current, power

### 21.2.1. Used to:
- Confirm pump operation
- Detect inverter failure
- Detect abnormal load behavior

### 21.2.2. Safety Logic:
- If inverter ON but no AC output → Error
- If pump expected ON but no power draw → Fault
- Detection occurs on ESP32 #2; classification and response decided by ESP32 #1 (Section 11)

## 21.3. Mixer Motor Monitoring (ACS712)

- Measures mixer motor current directly
- Installed inline with mixer motor power line
- Detects:
  - Motor running state
  - Overcurrent condition
  - No-load / failure condition

### 21.3.1. Safety:
- If mixer ON but no current → Stop + Alert
- If current exceeds threshold → Stop + Alert

---

# 22. ⚙️ Safety Logic

## 22.1. Pump Protection

* If pump ON and no flow detected → STOP
* Send error to ESP32 Master

## 22.2. Flow Validation

* Confirms correct dosing
* Detects:

  * Dry run
  * Blockage
  * Pump failure

## 22.3. Remote Safety Control

- User can issue emergency stop via SMS:
  STOP,ALL

- System will:
  - Disable all pumps
  - Close all valves
  - Send confirmation message

## 22.4. Energy-Based Protection

- System behavior adapts based on battery condition:

Battery Low:
→ Disable fertigation
→ Allow irrigation only

Battery Critical:
→ Stop all pumps
→ Enter power-saving mode
→ Send GSM alert

---

## 22.5. AC Power Validation

- If inverter enabled but no AC detected → Fault
- If pump command issued but no AC load → Pump failure

---

# 23. 🚨 Fault Classification and Response System

The system classifies faults into three levels based on severity and system impact. Each level defines the system’s response behavior, ensuring safe and predictable operation.

---

## 23.1. 🔴 Critical Faults (Immediate Shutdown)

### 23.1.1. Action:

* Send immediate GSM alert
* Set GPIO4 LOW of ESP #1 → Disable relay supplying ESP32 #2 power
* Stop all pumps and close all valves

### 23.1.2. Conditions:

#### 23.1.2.1. AC Power / PZEM Monitoring Faults**

* Pump commanded ON but:
  * Voltage present but current < the **No-current** limit (default 0.5 A) for 30 seconds
* Overcurrent > the **Overcurrent** limit (default 3.0 A)
* Voltage outside the **AC Voltage** window (default 215–240 V)
* Inverter ON but no voltage detected after 30 seconds

**Adjustable on the LCD (2026-11-15).** Settings > **AC Voltage** sets the low/high window (1 V a press, 80–260 V, at least 5 V apart). Settings > **AC Current** sets Overcurrent (max) and No-current (min), 0.1 A a press, 0.1–10.0 A, at least 0.2 A apart (10 A = the PZEM-004T v3 10 A model [CONFIRM]). Each screen's top row shows the last reading ESP2 measured. ESP1 saves both in NVS and sends them to ESP2 as `SET_CAL,PZEM_V,<low>,<high>` and `SET_CAL,PZEM_I,<min×10>,<max×10>` on every ESP2 power-up and every change; ESP2 keeps its own copy in NVS. Restore Defaults puts back 215–240 V and 0.5 / 3.0 A. A trip says which limit was in force: `PWR_FAIL,VOLTAGE|v=205.3|LOW|min=215`, `OVERCURRENT|i=4.12|max=3.0`, `NO_CURRENT|i=0.02|min=0.5`. It stops the pump, holds a run (or ends an exercise or prime) and sends an SMS alert. Not in Testing: Testing bypasses the PZEM checks (§ testing mode). The website's System tab edits both, for operators and Sub-operators only: `SET_AC_VOLT {vMin, vMax}` (2026-11-14) and `SET_AC_AMP {aMin10, aMax10}` in tenths of an amp (2026-11-16), with the same checks, refused while the matching LCD editor is open. ESP1 publishes them as `system.acVoltMin/acVoltMax` and `system.acAmpMin10/acAmpMax10`, plus the last readings (`acVoltLast`, `acAmpLast`) once a pump has run.

⚠️ *Note:* PZEM validation is only active when actuator operation is expected.

---

#### 23.1.2.2. Critical Flow Failure (Main Lines)**

* Transfer flow sensor failure
* Irrigation flow sensor failure
* No flow or impossible readings outside expected range

---

## 23.2. 🟠 Major Faults (Degraded Operation)

### 23.2.1. Action:

* Send immediate GSM alert
* Keep actuator running with restrictions
* Disable only affected subsystems

---

### 23.2.2. Conditions:

#### 23.2.2.1. Nutrient Dosing Flow Faults (6 Sensors)**

* No flow detected within 10 seconds
* Flow outside expected range

##### 23.2.2.1.1. Response:

* Disable affected nutrient channel
* Continue dosing using available nutrients
* If critical nutrient missing → disable fertigation for affected column

---

#### 23.2.2.2. NPK Sensor Fault

* No reading or unrealistic values

##### 23.2.2.2.1. Response:

* Disable fertigation for affected column
* Continue irrigation

---

#### 23.2.2.3. Soil Moisture Sensor Fault

* Average value > 100% or = 0%

##### 23.2.2.3.1. Response:

* Disable both irrigation and fertigation for affected column

---

#### 23.2.2.4. Mixer Motor / Bridge Pump Fault

* No current detected
* Overcurrent > 2.5A

##### 23.2.2.4.1. Response:

* Disable fertigation
* Allow irrigation only

---

#### 23.2.2.5. Environmental Sensor Fault (DHT22 / BH1750)

* No reading or abnormal values

##### 23.2.2.5.1. Response:

* Use default irrigation parameters
* Alert user via GSM

---

#### 23.2.2.6. EC Sensor Fault

* No reading or dangerous EC level during mixing

##### 23.2.2.6.1. Response:

* Stop dosing immediately
* Continue irrigation

---

#### 23.2.2.7. pH Sensor Fault

* No reading
* No change after dosing pH up/down

##### 23.2.2.7.1. Response:

* Stop dosing immediately
* Continue irrigation

---

## 23.3. 🟡 Minor Faults (Monitoring Only)

### 23.3.1. Action:

* Send GSM alert only
* No change to system operation

---

### 23.3.2. Conditions:

#### 23.3.2.1. Ultrasonic Sensor Instability

* Rapid fluctuations due to water surface ripple
* Ignore sudden changes within 10 minutes
* If instability persists beyond threshold → alert GSM
* web v.4 (17 Nov): ESP32 #1 passes a tank reading to ESP32 #2 only when the Nano's pings agree within
  `ULTRA_MAX_SPREAD_CM` (4 cm; 2 cm before 17 Nov). A reading it holds back is logged, at most every 10 s, as
  `NANO,LEVEL,NOT_USABLE|mix=<why>|res=<why>|cm=..|s=..` and shown on the LCD with a `?`. ESP32 #2 adds a 5-sample
  median and a jump gate (§19.4.9.1, §19.4.9.24).


---

# 24. 🛑 Fail-Safe Priority

1. Emergency Stop (SMS or system fault)
2. Pump protection (no flow detection)
3. Tank overflow/underflow protection
4. Sensor validation

Higher priority events override all operations.

---

# 25. 💾 Data Logging

The system logs as many events as possible to microSD for debugging and thesis data. All logging is performed by ESP32 #1 and timestamped from the DS3231 RTC. ESP32 #2 and Arduino Nano do not write to SD; their events are logged when ESP32 #1 hears about them, stamped with ESP32 #1's RTC time.

---

## 25.1. Logging Scope (Strategy C)

The system logs all distinct EVENTS, while avoiding redundant double-logging of routine traffic:

* Clean sensor packets are logged as their data line at the Nano's transmission rate (10s active / 20min day-idle / 1hr night-idle). A separate "received clean packet" line is NOT added — the data line itself is the record.
* Garbage packets are logged individually, including the RAW bytes (truncated to a safe length) so corruption can be diagnosed, plus the current consecutive-garbage counter value.

Logged event categories:

* Every Nano packet: clean (data line) or garbage (raw + counter)
* Every ESP32 #1 ↔ ESP32 #2 exchange: command sent, response received, retries, timeouts
* Every ESP32 #1 → Nano command: ACTIVE/DAY/NIGHT mode changes, RESET_REQ (fault and daily)
* Every global state change (Section 10.11)
* All faults (Critical / Major / Minor) and their classification
* All recovery actions: Nano watchdog events (as inferred), software RESET_REQ, ESP32 #2 soft reset (RESET_SELF) / power-cycle, fault hold + RESUME (sec.19.4.8), ESP32 #1 self-reset
* Dosing events (per nutrient: start, target, measured volume)
* Actuator start/stop (irrigation, fertigation, transfer, mixing)
* GSM messages sent and received
* Power statistics: INA226 (battery V/I/power, charge, consumption) and PZEM (AC V/I/power) snapshots

---

## 25.2. File Format & Rotation

* Format: CSV, one event per row
* Columns: timestamp, source, event_type, detail
  * timestamp — RTC date-time, format YYYY-MM-DD HH:MM:SS
  * source — NANO, ESP1, ESP2, GSM, SYS
  * event_type — SENSOR, GARBAGE, CMD, RESP, STATE, FAULT, RESET, DOSE, ACT, GSM, PWR
  * detail — compact payload defined per event type (Section 25.2.1)
* Rotation: one file per day, named YYYYMMDD.CSV (e.g. 20260609.CSV)
* RTC-failure fallback: if the RTC date is invalid/unavailable, log to NODATE.CSV and raise a Major fault alert so logging is never silently lost
* The detail field may itself contain values separated by a secondary delimiter (semicolon ; or pipe |) so it stays within one CSV cell; commas inside detail must be avoided or escaped to preserve CSV column alignment

### 25.2.1. Per-Event Detail Formats

The detail column content is defined per event_type below. Secondary delimiter is | (pipe) to avoid clashing with CSV commas.

| event_type | source(s)      | detail format                                              | example detail                          |
|------------|----------------|------------------------------------------------------------|-----------------------------------------|
| SENSOR     | NANO           | TYPE|values...  (TYPE = ENV/SOIL/TANK/LIGHT/NPK)            | ENV|25.3|68.2                           |
| GARBAGE    | NANO           | COUNT=n|RAW=<raw bytes, truncated to 64 chars>             | COUNT=3|RAW=<STA RT,SOI 41,,xx           |
| CMD        | ESP1           | TARGET|COMMAND|params  (TARGET = ESP2/NANO)                | ESP2|SEQ_IRRIGATION_A                    |
| RESP       | ESP2           | COMMAND|RESPONSE  (RESPONSE = DONE/BUSY/ERROR/FLOW_FAIL/PWR_FAIL/SAFE_STOP) | SEQ_IRRIGATION_A|DONE   |
| RESP       | ESP1           | RETRY|COMMAND|attempt_n   or   TIMEOUT|COMMAND             | RETRY|SEQ_IRRIGATION_A|2                 |
| STATE      | ESP1           | FROM->TO  (state names per Section 10.11.2)                | IDLE_STATE->ACTIVE_STATE                |
| FAULT      | ESP1           | TIER|CODE|location  (TIER = CRIT/MAJ/MIN)                  | MAJ|NPK_FAULT|COL_B                      |
| RESET      | ESP1           | TARGET|METHOD|reason  (METHOD = WDT/RESET_REQ; reason = GARBAGE5/DAILY/TIMEOUT) | NANO|RESET_REQ|GARBAGE5 |
| DOSE       | ESP2           | NUTRIENT|target_mL|measured_mL|COL                         | NUT_A|50.0|49.2|COL_A                    |
| ACT        | ESP1/ESP2      | DEVICE|STATE|COL  (STATE = START/STOP)                     | IRRIGATION|START|COL_A                   |
| GSM        | GSM            | DIR|payload  (DIR = TX/RX)                                 | TX|ALERT|MAJ|NPK_FAULT|COL_B             |
| PWR        | ESP1           | SRC|fields  (main battery; SRC=INA or OPTO)                | BATT|V=12.6|I=1.2|P=15.1|SRC=INA|CONS=90Wh|CHG=150Wh |
| PWR2       | ESP1           | secondary battery and switch events                        | BATT2|V=15.9|I=0.800|P=12.7|LOW=0|SRC=MAIN |
| PWRS       | ESP1           | solar, and both isolated modules' FOUND/LOST               | SOLAR|V=14.20|I=3.100|P=44.0            |

Notes:
* GARBAGE RAW is truncated to a safe length (e.g. 64 chars) to bound row size; the consecutive-garbage counter value is recorded as COUNT=n.
* DOSE logs both the target and the actual measured (flow-derived) volume, enabling dosing-accuracy analysis for the thesis.
* PWR snapshots are logged at a sensible interval (e.g. once per logging flush) and on significant battery-state changes, not on every reading, to limit volume.
* ACT logs each session start/stop; the consolidated scheduled-run GSM notice (Section 12.1.2) is logged separately as a GSM/TX event.
* Rows added in web v.4 (17 Nov), detail as it appears in the CSV:

| Row | Meaning |
| --- | --- |
| `SYS,BOOT,reason=<POWER_ON/SOFTWARE/PANIC/TASK_WDT/BROWNOUT…>\|rr=n` | every ESP32 #1 start, with its reset reason |
| `ESP2,RESP,INFO;FILL_RUN\|need=..\|run_s=..\|q=..\|start_l=..\|base=..` | a fill's plan: litres needed and the run time |
| `ESP2,RESP,INFO;INV_ON;<pump>` | the inverter relay switched on; timed runs count from here |
| `ESP2,RESP,INFO;FILL_CHECK\|goal=..\|measured=..\|missing=..\|settle_s=..\|OK/TOPUP` | the settled measurement after a fill |
| `ESP2,RESP,DEGRADED;LEVEL_BLIND\|FILL\|base=..\|run_s=..` | a fill that lost its level and carried on by its capped run time |
| `NANO,LEVEL,NOT_USABLE\|mix=<why>\|res=<why>\|cm=..\|s=..` | a tank reading ESP32 #1 did not pass to ESP32 #2, and why |
| `ESP1,ACT,FORCE\|COL_ON\|COL_x` | a forced run switched a column back on |
| `ESP1,UPLOAD,OK/FAIL/MISSING\|<day>.CSV\|..\|FIREBASE` | the SD log upload (a failure adds `step=..\|heap=..\|blk=..`) |
| `ESP1,UPLOAD,WEB_REQUEST\|<day>.CSV` | the website asked for a day's log |

* SENSOR detail for a per-column reading records DISABLED for any disabled column (e.g. SOIL|A=41|B=53|C=DISABLED), so the log distinguishes intentionally-disabled from a 0 reading or a −1 sensor fault. ESP32 #1 knows which columns are disabled from COLUMN_ENABLED and writes the token even though the Nano omits the column from the packet.

---

## 25.3. Write Strategy (SD / Timing Protection)

A single SD write can stall for tens of milliseconds, which would conflict with non-blocking operation and watchdog servicing. To protect timing:

* Events are written to a small RAM buffer first
* The buffer is flushed to SD in batches (every few seconds or every N events, whichever comes first)
* Batched writes avoid the many-tiny-writes pattern that strains SD cards and causes stalls
* On a critical fault or controlled shutdown, the buffer is flushed immediately
* Tradeoff documented: a crash may lose the last few un-flushed events; this is accepted in exchange for timing stability

Note: daily files keep individual files manageable and align with the daily-summary rhythm. A single rolling file is possible but grows unbounded and becomes hard to open over time; daily rotation is the chosen default.

---

## 25.4. Uses

* Debugging (primary purpose — full event visibility)
* Daily summary generation (via GSM, Section 12.1.3)
* Performance analysis
* Thesis data collection. Sensor data can be graphed two ways now: (a) automatically via the WiFi → ThingSpeak telemetry uplink (live per-field charts, Section 29.4), and (b) from the SD CSV logs / SMS digests for offline or paper graphing.

---

# 26. 📈 Expected Outcomes

* Stable soil moisture levels
* Accurate nutrient dosing
* Reduced water and fertilizer waste
* Improved plant health

---

# 27. ⭐ System Advantages

* Modular architecture (Nano + Dual ESP32)
* Scalable nutrient system (A–D expansion)
* Real-time flow validation
* Reduced calibration using PZEM
* Offline capable (RTC + SD logging)

---

# 28. 🚀 Usage Instruction

When starting a new chat:

Please read this project reference file first.
This is my irrigation thesis system.

---

# 29. 🆕 Firmware Feature Additions (Implemented)

These features were implemented after the original specification. They are part of the
build and supersede any earlier text that conflicts with them. Most live on ESP32 #1 (the
master); §29.7 (hardware-fault reporting) and §29.8 (Testing arming + wiring test) involve
ESP32 #2. The Nano is unchanged except where stated.

## 29.1. Emergency-Off + operator recovery

* **Trigger:** pressing **MODE + BACK at the same time** initiates an Emergency Off —
  STOP_ALL is sent to ESP32 #2, ESP32 #2 power is cut, and the global state becomes
  `EMERGENCY_STOP`. It fires even while the screen is locked (safety override).
* **Recovery is deliberate:** the LCD shows a selectable prompt — `> Return to normal` /
  `Stay stopped`. UP/DOWN choose, ENTER confirms. "Return to normal" re-enters
  `STARTUP_SYNC` (re-powers and re-validates ESP32 #2 and devices). There is no automatic
  exit from Emergency Off.

## 29.2. LCD Lock (anti-tamper)

* A **"Lock Screen"** item in the Settings menu locks the LCD. **Manual only — the system
  never auto-locks.** The lock state persists in NVS across reboots.
* While locked, all buttons are ignored **except**: **UP + DOWN together = unlock** (the
  lock screen shows this hint), and the MODE + BACK Emergency-Off combo (safety).
* **The lock does not affect operation.** Automation, irrigation/fertigation, logging, and
  telemetry continue normally while locked — only the UI is gated.

## 29.3. ESP32 #1 self-recovery (device-loss daily reset)

* Watches the core devices that answered **at boot**: RTC (0x68), LCD (0x27),
  INA226 (0x40), and the microSD card.
* If one **drops out at runtime** (the suspected "RTC/all devices stop responding" I2C
  wedge), ESP32 #1 reboots itself — **at most once per calendar day**.
* **Boot-loop-safe:** only a present→absent transition triggers a reset; a device absent
  from boot simply runs degraded. The daily limit uses an RTC-RAM day stamp that survives
  the software reset and self-limits even when the RTC itself is dead.

## 29.4. WiFi → ThingSpeak telemetry (live graphs)

* ESP32 #1 connects to a nearby WiFi network and uploads telemetry to **ThingSpeak**,
  which renders a live graph per field — viewable from anywhere over the internet. This
  is the "long-range monitoring / graphs" path. **SMS is retained** for alerts, commands,
  and on-demand reports; WiFi carries telemetry only (alerts still work if WiFi is down).
* **Non-blocking by design:** all WiFi/HTTP runs in a FreeRTOS task pinned to **core 0**,
  so blocking network calls never disturb the core-1 control loop or the 8 s task watchdog.
  Telemetry crosses cores via a spinlock-guarded snapshot; credentials via a mutex.
* **Channels (8 fields each; one write key per channel):**

  | Channel | Name    | Fields |
  | ------- | ------- | ------ |
  | 1       | Columns | per-enabled-column moisture + N, P, K |
  | 2       | System  | temp, humidity, lux, reservoir %, mixing %, battery V, battery W, flow |
  | 3       | Chem    | 1 solar current (A), 2 pH col A, 3 solar energy today (Wh), 4 pH col B, 5 battery current (A), 6 pH col C, 7 solar V, 8 solar W. Soil EC was dropped from ThingSpeak on 2026-11-15 to make room for solar (still on the SD log and Firebase); rename fields 1 and 3 in the channel settings. |

* **Setup is by SMS** (owner-only; the sender's last 9 digits must match the configured
  number), persisted to NVS:
  * `WIFI,<ssid>,<password>` — password is the remainder after the 2nd comma (may contain
    commas/spaces; SSID may not).
  * `TSKEY,<1|2|3>,<writeApiKey>` — set a channel's write key.
  * `NET` — reply with WiFi up/down, RSSI, IP, and last-upload status.
* Upload cadence ≈ 20 s during ACTIVE, 60 s when idle. Transport is plain HTTP
  (unencrypted) for simplicity; HTTPS is a possible future upgrade.
* Build note: ESP32 #1 uses the `huge_app` partition to fit WiFi + HTTPClient + SD + libs.

### 29.4.1. WiFi provisioning portal (SoftAP web form)

As an alternative to setting the SSID/password by SMS, ESP32 #1 can host a temporary
provisioning access point + captive web form so credentials can be entered from a phone
(no GSM, no on-device typing) — ideal for first-time commissioning at a site with no SIM.

* **Menu controls:** the MODE-button (Settings) menu has two live `[ON]/[OFF]` toggles — **`WiFi`**
  (`SET_WIFI`) is a persisted master switch for the STA radio + ThingSpeak telemetry (`wifiEnabled`,
  NVS `wifien`; when off, `netTask` powers the radio down and `NET`/LCD show `WiFi:OFF`), and
  **`Setup AP`** (`SET_SOFTAP`) starts/stops the provisioning portal. SMS equivalents: `WIFI,ON`/
  `WIFI,OFF` and `WIFIPORTAL`/`WIFIPORTAL,STOP` (all owner-only).
* **Start the portal:** Settings → **Setup AP**, or the SMS `WIFIPORTAL`. It runs even when the WiFi
  master switch is off; a successful **Save** turns the switch back on so the device connects.
* **Use:** join the WPA2 AP `Irrig-Setup` (password in `ESP1/include/secrets.h`), browse to
  `http://192.168.4.1`, pick a **scanned** nearby SSID (or type a hidden one), enter the
  password, **Save** → creds are staged and persisted to NVS, then the device reconnects in
  STA mode. The old `WIFI,<ssid>,<pass>` SMS remains as a fallback.
* **Close:** on Save, the LCD **BACK** button, `WIFIPORTAL,STOP`, or a 10-minute auto-timeout.
* **Isolation:** the portal runs entirely in the core-0 network task (built-in `WebServer` +
  captive `DNSServer`, no extra libraries). NVS is written only by the core-1 loop
  (`wifiPersistPending`), preserving the single-core-NVS invariant. Irrigation/fertigation
  (core 1 + ESP2) continue unaffected while the AP is up; only telemetry pauses. `NET` and the
  LCD GSM+WiFi page report `WiFi:PORTAL` during setup.

#### 29.4.1.1. Portal admin area (PIN-gated)

Below the WiFi form the portal exposes an **Admin** section, unlocked with a numeric PIN (NVS `apin`,
first value from `secrets.h`, re-locked on every portal close). It provides:

* **SD logs:** list every `/<daystamp>.CSV` with size; `/view?f=` streams the last 16 KB inline (quick
  check); `/download?f=` streams the whole file as an attachment. File params are sanitized to a
  root-level `*.CSV` basename (no path traversal).
* **Format SD:** confirm-gated **delete-all-files** (the installed core 2.0.17 / IDF 4.4 has no true
  FAT-format API; delete-all empties the card and logging resumes into a fresh file). Refused while a
  summary report is mid-read.
* **Owner number / admin PIN:** edit `PHONE_NUMBER` (the SMS owner-gate + alert/report recipient) and
  the admin PIN.
* **Config forms:** ThingSpeak write keys (`TSKEY`), per-column nutrient targets / crop preset (`SET`),
  per-column mode + name (`MODE`/`NAME`), and thresholds (`THRESH`). Each form builds an SMS-format
  command server-side and enqueues it (6-slot ring); the **core-1 loop** replays it through `handleSms()`
  with SMS replies muted — no parser duplication and no cross-core config write. `THRESH,<start>,<gap>`
  is a new SMS command (thresholds were previously LCD-only). ThingSpeak keys show set/unset only.

* **SD + owner concurrency:** a FreeRTOS `sdMux` serializes ALL SD access across cores — `logFlush` and
  the summary parser (core 1) and the portal handlers (core 0). `logFlush` uses a timed take and buffers
  on contention (never stalls the core-1 loop/WDT); summaries are deferred while the portal is active.
  The owner number and PIN are **staged to core 1** (`pendingOwner`/`pendingAdminPin`) and applied +
  persisted there, so `senderIsOwner`/`sendSMS` never race a cross-core write.
* **Security note:** the AP password and admin PIN travel in clear over the local Wi-Fi link (HTTP), and
  the unlock is a per-session RAM flag — acceptable for a physically-present operator, not internet-facing.
  Change the default PIN at commissioning.

## 29.5. SMS reports — day-selectable SUMMARY and new FULL SUMMARY

Extends Section 12 (GSM reports). **STATUS is unchanged** (always the current day).

* **`SUMMARY[,<day>]`** — the day's **averages**, formatted for the thesis paper:
  per-column NPK + moisture averages, power averages (avg V/W) + energy Wh, water and
  nutrient totals, and fault/reset/run/dose counts.
* **`FULL SUMMARY[,<day>]`** — an uncapped **hourly** record: per-column N/P/K + moisture +
  battery V/W for each hour that has data, plus a **deduplicated error list** (each error
  code once, with its first timestamp), run/dose events with volumes, and a daily
  peaks/totals line (max temp, min battery V, peak W, totals, counts). Segments are streamed
  paced to the SMS queue so none are dropped, each tagged `(i/N)`; a busy day can be many
  messages (tunable cap, default unlimited).
* **`<day>` selector** (both reports): omitted / `TODAY`, `YESTERDAY`, explicit `YYYYMMDD`,
  or `NODATE`.
* **NODATE / RTC-dead data:** when the RTC has stopped, logs are written to `/NODATE.CSV`
  with `0000-00-00 00:00:00` timestamps. Both reports resolve there automatically when the
  RTC is dead, and `,NODATE` selects that file explicitly (its rows bucket into hour 00).
  Both reports reuse the existing bounded, non-blocking incremental SD parser.

## 29.6. Review-fix behaviors (now part of the build)

* **Nano interval pacing:** ESP32 #1 now actively drives the Nano's `ACTIVE` / `DAY` /
  `NIGHT` transmission interval (single owner, sent only on change) — `ACTIVE` during runs,
  otherwise `DAY`/`NIGHT` by the RTC clock. Previously the Nano was left at the fast
  interval indefinitely.
* **Reservoir-low alert is one-shot:** raised once per low episode and cleared on recovery
  (was previously re-raised every loop, flooding SMS/log).
* **ESP32 #2 timer cleanup:** `STOP_ALL` and `TEST` cancel any in-flight preventive-exercise
  or Nano-reset pulse timers, so no stray late `DONE` is emitted; the inverter is dropped
  during DC nutrient dosing.
* **Water in the log:** irrigation/fertigation STOP log rows now include `|W=<liters>` so
  per-day water totals can be recovered from the CSV by the reports.

## 29.7. ESP2 hardware-fault reporting (PCF8575 / EC / pH)

ESP32 #2 now actively detects and reports hardware faults to ESP32 #1 (response codes in
Section 9.7.3). Both are classified **Major** on ESP32 #1 (alert + log, no shutdown).

* **PCF8575 relay bus (`PCF_FAIL,I2C` / `PCF_OK`):** every relay write checks the I2C result,
  and a periodic active probe (~2 s) feeds a consecutive-failure counter so a single glitch
  can't false-trip. After a few failures ESP32 #2 sends `PCF_FAIL,I2C`, re-asserted ~10 s while
  the bus stays dead (so a missed UART line still gets through), and `PCF_OK` on recovery.
  It does NOT force a stop (keep-running policy); the PZEM no-current and flow-timeout safeties
  still apply during a run.
* **EC / pH probe (`SENSOR_FAIL,EC` / `SENSOR_FAIL,PH`):** at the dosing EC/pH step the raw ADC
  is range-checked; a railed reading (probe disconnected/shorted) reports a SENSOR_FAIL — a
  HARDWARE fault, distinct from `EC_FAIL`/`PH_FAIL` which mean the value is merely outside the
  safe window. EC and pH are checked independently. The mixed batch still delivers (no drain).
* Editable thresholds on ESP32 #2: PCF probe cadence / fail-limit / re-assert interval, and the
  EC/pH rail ADC bounds (`[MEASURE]` at commissioning).

## 29.8. Testing-mode arming hardening + standalone PCF8575 wiring test

Extends the manual TEST mode (Section 18.10.8).

* **Robust arming:** opening Settings>Testing powers ESP32 #2 via the GPIO4 relay, **primes**
  it (~2 s boot settle), then **re-sends `TEST,ENTER` every ~1 s until ESP32 #2 confirms with
  `ACK,TEST,ENTER`**. This replaces the old reliance on the single boot-time `READY`, which is
  easily lost on a cold relay power-up — leaving ESP32 #2 out of test mode so every `TEST,HOLD`
  was silently dropped and no relay actuated.
* **Honest LCD feedback** (header): `ESP2 DOWN` (no heartbeat) / `ARMING...` (alive, awaiting
  confirmation) / `(dead-man)` (confirmed in test mode, relays controllable) /
  `NO ACK-chk link` (ESP32 #2 heartbeats but never ACKs after several tries → suspect the
  ESP32 #1 → ESP32 #2 TX link, GPIO25→GPIO16). `TEST,HOLD` keeps streaming so control engages
  the instant ESP32 #2 is armed.
* **Standalone wiring test:** `Test Code/ESP2_PCF8575_SEQ/` (PlatformIO, ESP32 #2 only) walks
  the PCF8575 outputs ON one at a time P0→P15 with a 1 s gap, active-LOW, naming each channel on
  the serial monitor, and probes the PCF at boot. It isolates relay/PCF wiring from the
  ESP32 #1↔#2 link: if relays click here but Testing still fails, the fault is the link, not the
  hardware.

## 29.9. Remote log retrieval — Supabase Storage upload

> **Replaced on 17 Nov 2026:** the Supabase project expired. Logs now go to Firebase and are downloaded on the
> website (§19.4.9.23). Supabase is used only while Firebase is switched off.

The site is ~50 km away with **outbound-only** internet (no port forwarding), so logs are pushed to
the cloud rather than pulled. ESP32 #1 uploads each completed daily CSV to a **private Supabase
Storage bucket**; the developer generates time-limited signed URLs from the dashboard to download.

* **What is uploaded:** the completed daily log files exactly as written to SD — `/<YYYYMMDD>.CSV`
  (4-column schema `timestamp,source,event_type,detail`). The **currently-open day and `/NODATE.CSV`
  are never uploaded** (no read/write conflict with the active SD writer).
* **Endpoint:** `POST <projectUrl>/storage/v1/object/CSV-Logs/<YYYYMMDD>.CSV`. Bucket name is
  **case-sensitive** — exactly `CSV-Logs` (capital C, capital L, hyphen); a mismatch returns 404.
  Headers: `Authorization: Bearer <service_role>`, `apikey: <service_role>`, `Content-Type: text/csv`,
  `x-upsert: true` (so a retry or re-send overwrites instead of failing "already exists").
* **Streamed, not buffered:** the body is streamed straight from the SD `File` via
  `HTTPClient::sendRequest("POST", &file, size)`, which pulls ~1.4 KB at a time with a fixed
  Content-Length. Heap stays flat regardless of file size (multi-MB day files upload safely).
  Runs on the **core-0 `netTask`** over `WiFiClientSecure` (TLS); SD access is held under `sdMux`
  while streaming, and the core-1 logger's buffered `logFlush` just defers meanwhile.
* **Triggers:** at the midnight day-rollover, yesterday's now-completed file becomes a candidate; on
  every WiFi (re)connect the firmware also **catches up** any completed day-file on the SD newer than
  the last success (NVS `supalast`), oldest-first, one per cycle — so nights missed due to the site's
  flaky WiFi drain automatically. Manual re-send: SMS **`UPLOAD`** (yesterday) / **`UPLOAD,YYYYMMDD`**
  (a specific day), or the **"Upload logs"** button in the SoftAP admin page.
* **Reliability:** failures retry with exponential backoff (30 s → 2 m → 8 m → cap 30 m). The SD file
  is never deleted, so a failed upload only defers — no data loss. Every attempt is logged as its own
  **`UPLOAD`** event_type: `UPLOAD|OK|<day>.CSV` or `UPLOAD|FAIL|<day>.CSV|http=<code>|try=<n>`.
* **Credentials:** the project URL + service_role key live **only in NVS** (like the WiFi/ThingSpeak
  creds). Set them at runtime via the **SoftAP admin form** (the PIN-gated admin page has a "Supabase
  cloud logs" URL+key form — the reliable path, since a service_role JWT (~220 chars) exceeds one SMS),
  or the owner-gated SMS `SUPA,<projectUrl>,<serviceKey>` (`SUPA,CLEAR` to wipe) for a short key. The key
  is never echoed back to the browser or written to the log. **Nothing secret is compiled into tracked
  source.** TLS currently uses
  `setInsecure()` (encrypted, no certificate pinning) — adequate for the deployment; pinning the
  Supabase CA is a future hardening step. Rotate any key that was exposed during manual testing.


---

## 29.10. 17 Nov 2026 additions (web v.4)

All of these are described in §19.4.9.11–§19.4.9.27 and §19.4.7.1. Flashed and checked on the rig the same evening,
except the heap fix of §19.4.9.23 (built, not yet flashed).

| Feature | Board(s) | Section |
| --- | --- | --- |
| Pump rates; fill by run time from the inverter switch-on, settle, measure, top-up; delivery to under 5 % + 10 s; the mixer in the tank's shape | ESP1, ESP2, shared | §19.4.9.11, §19.4.9.13 |
| Testing Fill/Push: 1 minute of inverter time | ESP2 | §19.4.9.14 |
| LCD Transfer / Booster pump estimate (at least 5 tries, fitted line, the user's readings as defaults) | ESP1, ESP2, shared | §19.4.9.15 |
| 1.5 s between button runs; 120 ms release filter | ESP1, ESP2 | §19.4.9.16, §19.4.9.18 |
| Every timed run counts from the inverter switch-on (`INFO,INV_ON`) | ESP1, ESP2 | §19.4.9.17 |
| BATT2 blocks AC pumps at a resting charge of 30 % or less (back at 35 %) | ESP1 | §19.4.7.1 |
| Tanks −1: Nano echo wait 25 ms, TANK with every heartbeat, first-data retry; DHT 0/0 refused; daily jobs kept in NVS; boot reason logged | Nano, ESP1 | §19.4.9.19–20 |
| Forced runs: no limit, no tank-check block; a switched-off column is switched back on | ESP1 | §19.4.9.21, §19.4.9.26 |
| Live website (2–4 s while watched) and Refresh | ESP1, website, rules | §19.4.9.22 |
| SD card logs through Firebase; Manual/Test "Update today's log" | ESP1, website, rules | §19.4.9.23 |
| LEVEL_BLIND: ping spread 4 cm, a fill carries on by run time, `NANO,LEVEL` lines, the `t \| 1` timer bug | ESP1, ESP2 | §19.4.9.24–25 |
