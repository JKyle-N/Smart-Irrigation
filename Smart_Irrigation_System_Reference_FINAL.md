# 1. 📄 Smart Irrigation & Fertigation System (Final Reference)

---

# 2. 🎯 Thesis Objective

This project aims to develop a **solar-powered automated irrigation and fertigation system** that:

* Maintains optimal soil moisture and nutrient levels
* Reduces manual labor in watering and fertilization
* Uses sensor-based decision-making for precision agriculture
* Operates efficiently using renewable energy
* Enables remote monitoring and control via GSM communication (SIM800L)

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
- Flow sensors provide estimated volume (calibration required)
- Sensor accuracy may vary with environmental conditions

---

## 5.3. ⚙️ Calibration Notes

- Flow sensors must be calibrated for accurate volume measurement
- pH and EC sensors require periodic calibration
- Sensor drift should be considered in long-term use

---

##  5.4. Microcontroller Responsibility Boundaries

## 5.5. 5.X. 🔷 Microcontroller Responsibility Boundaries

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
| Nano reset authority             | ✘            | ✔        | Execute only |
| Hardware reset execution         | ✘            | ✘        | ✔            |

### 5.5.1. Design Philosophy

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

* Ultrasonic Reservoir Level Sensor
* Ultrasonic Mixing Tank Sensor

## 6.4. Water System

* Flow Sensors (8x total)
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

* INA226 Current & Power Monitor (Battery-side)
* PZEM-004T Power Monitor
* DS3231 RTC
* microSD Logging Module
* LCD Display
* SIM800L GSM Module (remote communication)
* ACS712 Current Sensor (Mixer Motor Monitoring)

---

# 7. ⚙️ System Operation Flow

1. Sensors collect environmental and soil data
2. Arduino Nano sends data to ESP32 Master
3. ESP32 Master processes and decides actions
4. Commands sent to ESP32 Actuator
5. Pumps and valves execute irrigation/fertigation
6. Flow sensors validate operation in real-time
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
| ENV     | Environment data |
| SOIL    | Soil moisture    |
| TANK    | Tank levels      |
| NPK     | Nutrient values  |
| STATUS  | Nano health      |

### 9.7.1.1. ESP32 #1 to Nano Commands

| Command   | Purpose                                                  |
|-----------|----------------------------------------------------------|
| ACTIVE    | Switch Nano to active (fast) transmission interval       |
| DAY       | Switch Nano to day-idle interval                         |
| NIGHT     | Switch Nano to night-idle interval                       |
| RESET_REQ | Request Nano software self-reset (via short-timeout WDT) |

RESET_REQ is used both for fault recovery (after 5 consecutive garbage packets, Section 18.9.5) and for the once-per-day fresh-start reset.

### 9.7.2. ESP32 #1 to ESP32 #2 Execution Commands

| Command           | Purpose             |
|-------------------|---------------------|
| START_IRRIGATION  | Begin irrigation    |
| START_FERTIGATION | Begin fertigation   |
| STOP_ALL          | Emergency stop      |
| RESET_NANO        | Trigger Nano hardware reset (last resort) |
| RESET_SELF        | Soft reset ESP32 #2 |
| STATUS_REQ        | Request status      |
| TEST,ENTER        | Enter manual TEST mode (Settings>Testing, sec.18.10.8): ESP32 #2 forces SAFE (all OFF) and arms the dead-man. Idle-only (BUSY during a sequence). |
| TEST,HOLD,\<bit\> | Dead-man keep-alive: ESP32 #1 streams this (~every 150 ms) while the operator holds ENTER. ESP32 #2 keeps ONE relay ON (PCF8575 OUT_* index 0..15) only while these arrive; switching bit turns the previous OFF (one-at-a-time). |
| TEST,RELEASE      | Operator released ENTER — turn the held relay OFF immediately. |
| TEST,EXIT         | Leave TEST mode, all outputs OFF. |

ESP32 #2 is the SOLE safety authority in TEST mode (sec.18.10.8.3): it turns the relay OFF if HOLD stops arriving within a short timeout (button released, ESP32 #1 hang, or link loss) and enforces a HARD 10-second cap on continuous ON regardless of ESP32 #1.

### 9.7.3. ESP32 #2 to ESP #1 Responses

| Response  | Meaning                  |
|-----------|--------------------------|
| DONE      | Command completed        |
| BUSY      | Operation active         |
| ERROR     | Execution failure (ESP32 #1 treats as Critical: stop + power-cut) |
| DEGRADED  | A single channel disabled (nutrient no-flow / mixer no-load) but the sequence CONTINUES; ESP32 #1 classifies Major (alert + log, no shutdown, work order stays active). Used instead of ERROR for recoverable per-channel faults (sec.23.2.2.1, .4) |
| FLOW_FAIL | Flow validation failed   |
| PWR_FAIL  | PZEM power validation failed (pump ON but no current / overcurrent / voltage out of range) |
| EC_FAIL   | EC outside safe window during dosing (local protective stop) |
| PH_FAIL   | pH outside safe window during dosing (local protective stop) |
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
* nutrient dosing targets (mL per bottle, from the dosing calculation)
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

<START>,START_IRRIGATION,A,<END>

<START>,FLOW_FAIL,MAIN,<END>

---

### 9.9.2. Packet Formatting Rules

The following rules apply to all controllers:

* All commands use UPPERCASE format
* Parameters are comma-separated
* No spaces allowed inside packets
* START and END markers are mandatory
* Packets must remain human-readable ASCII
* Maximum packet length: 128 bytes

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

<START>,RESET_NANO,<END>

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
* control actuators

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
* Nano software recovery may be attempted
* hardware reset may be triggered if required

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

* irrigation thresholds (hysteresis start/stop %, per column)
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
* normal protective safeties intentionally bypassed; only dead-man release + ESP32 #2's 10-second hard cap remain
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

# 12. 📡 GSM Communication System (SIM800L)

The SIM800L GSM module is connected to ESP32 #1 (hardware UART2, GPIO27 TX / GPIO26 RX, 9600 baud) and is used for outbound alerts/reports and inbound configuration/commands. SIM800L texts ONLY for errors, warnings, scheduled-run notices, and reports — it is not a continuous telemetry channel (the full event stream lives in the SD log, Section 25).

---

## 12.1. 🔹 Outbound Messages (System → User)

### 12.1.1. 🚨 Fault & Warning Alerts (All Three Tiers)

SIM800L sends an SMS for ALL fault tiers (Critical 🔴, Major 🟠, Minor 🟡, per Section 23).

Special-case alert: when ESP32 #1 issues a Nano reset after 5 consecutive garbage packets (Section 18.9.5), an alert is sent identifying which reset layer was used (software RESET_REQ or hardware reset).

Compact format examples:

ALERT,CRIT,PUMP_FAIL,TRANSFER
ALERT,MAJ,NPK_FAULT,COL_B
ALERT,MIN,ULTRASONIC,MIX
ALERT,WARN,NANO_RESET,SOFT,GARBAGE5

Note: Minor-tier alerts (e.g. ultrasonic ripple) may be frequent. Rate-limiting of Minor alerts can be added later if SMS volume becomes excessive; by current design all three tiers are sent.

---

### 12.1.2. 🚜 Scheduled Run Notice

When a scheduled irrigation/fertigation run begins, SIM800L sends ONE consolidated SMS covering all columns scheduled in that run (not one SMS per column, not per sequence).

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
- pH is a direct setpoint corrected by the pH up/down pumps
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

| Command                   | Function                                          |
|---------------------------|---------------------------------------------------|
| MODE,COL_A,AUTO           | Column decides irrigate/fertigate by nutrient gap |
| MODE,COL_A,IRRIGATION_ONLY| Disable fertigation for this column (water only)  |
| STOP,ALL                  | Emergency stop                                    |
| STATUS                    | Live status + daily report (both)                 |

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

Calculation status: DEFINED-BUT-PENDING. The conversion from target mg/kg → mL per bottle (accounting for mixing-tank batch volume, salt composition, and the lab soil baseline) is documented as a placeholder and will be implemented as a separate task. It is an open-loop approximation, not a closed-loop guarantee.

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
* Sequencing of nutrient dosing vs. pH correction is part of the dosing calculation topic and is documented as DEFINED-BUT-PENDING (Section 12.4)


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


IF mixing tank below target level
→ Open reservoir valve
→ Start transfer pump


---

### 14.4.3. Overflow Protection


IF mixing tank exceeds maximum level
→ Stop filling immediately


---

## 14.5. 🔷 Power-Aware Control Logic

System behavior adapts according to available power.

---

### 14.5.1. Battery Protection Logic

| Battery State | System Action       |
| ------------- | ------------------- |
| Normal        | Full operation      |
| Low           | Disable fertigation |
| Critical      | Stop all operations |

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
| D3  | Reserved                            |
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
3. ESP32 #1 hardware reset via ESP32 #2 → PCF8575 → Nano RESET pin — last resort

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

ESP32 has only three hardware UART peripherals (UART0, UART1, UART2). UART0 is the USB/boot/debug port and is KEPT FREE for the USB serial monitor (debugging is done via USB serial monitor and the LCD). The PZEM-004T has been relocated to ESP32 #2 (Section 19.2.1). With UART0 reserved for debug, two hardware UARTs remain for three devices (Nano, ESP32 #2, SIM800L), so the Arduino Nano runs on software serial.

| Device       | TX | RX | Peripheral                |
| ------------ | -- | -- | ------------------------- |
| ESP32 #2     | 25 | 33 | UART1 (hardware)          |
| SIM800L      | 27 | 26 | UART2 (hardware, proven)  |
| Arduino Nano | 17 | 16 | SoftwareSerial            |
| (USB debug)  | 1  | 3  | UART0 (reserved for USB)  |

Notes:
- SIM800L is kept on a hardware UART (proven working at GPIO27 TX / GPIO26 RX, 9600 baud) because its async incoming-SMS bytes (+CMT) are timing-sensitive.
- ESP32 #2 is kept on a hardware UART because the command/response link controls actuators and is safety-relevant.
- Arduino Nano is placed on software serial because lost sensor packets are recoverable (the next packet arrives within the interval, and the <START>...<END> framing discards corrupted packets cleanly).
- Software serial for the Nano uses the EspSoftwareSerial (plerup) library; the legacy AVR SoftwareSerial will not work on ESP32.
- A bench test of Nano-on-software-serial packet loss under load (flow interrupts active, SIM + ESP2 active) is recommended before final deployment. Pending.

### 18.3.1. UART Mapping (Actual Wiring)

ESP32 #1 → ESP32 #2 (hardware UART1):

- ESP#1 GPIO25 (TX) → ESP#2 GPIO16 (RX)
- ESP#1 GPIO33 (RX) ← ESP#2 GPIO17 (TX)
- GND must be common

ESP32 #1 → SIM800L (hardware UART2):

- ESP#1 GPIO27 (TX) → SIM800L RX
- ESP#1 GPIO26 (RX) ← SIM800L TX
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

- ESP32 #1 hosts ESP32 #2 and SIM800L on hardware UARTs; the Arduino Nano runs on software serial (EspSoftwareSerial)
- UART0 is reserved for the USB serial monitor (debugging)
- PZEM-004T is no longer on ESP32 #1 (moved to ESP32 #2, Section 19.2.1)
- Software must handle independent serial streams
- Avoid blocking reads to prevent data loss
- SIM800L AT-command exchanges must remain non-blocking and tolerant of unsolicited incoming-SMS bytes (+CMT)
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
* INA226 (0x40)
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

- Because ESP32 #2 and the PCF8575 are power-dependent on GPIO4 relay control, disabling ESP32 #2 also disables the Arduino Nano external hardware reset pathway until ESP32 #2 is restored.

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
2. ESP32 #1 software reset request (RESET_REQ over UART) — second priority, triggered after 5 consecutive garbage packets. Used because the Nano cannot detect its own garbage output.
3. ESP32 #1 hardware reset (RESET_NANO → ESP32 #2 → PCF8575 P17 → Nano RESET) — last resort, only if software reset fails to restore valid data.

#### 18.9.5.0.3. Daily Fresh-Start Reset

Independent of fault handling, ESP32 #1 sends RESET_REQ to the Nano once per day to ensure a clean daily restart (proactive hygiene, not fault-driven). This uses the software-reset path (Layer 2 mechanism) and is EXEMPT from the once-per-day hardware-reset limit.

---

#### 18.9.5.1. External Nano Hardware Reset

To improve long-term system reliability, the Arduino Nano reset pin may be controlled externally through ESP32-based recovery logic. This is the LAST-RESORT recovery layer, used only after the Nano's own watchdog and the RESET_REQ software path have failed.

Implementation concept:

ESP32 #1
↓ UART Command
ESP32 #2
↓ PCF8575 Output
Transistor Driver
↓
Arduino Nano RESET pin

Behavior:

* ESP32 #1 evaluates incoming Nano data validity (garbage counter, Section 18.9.5.0.1)
* If 5 consecutive garbage packets occur:
  → ESP32 #1 first issues a RESET_REQ software reset (second priority)
* If software reset fails to restore valid data:
  → ESP32 #1 escalates to hardware reset (last resort)
* ESP32 #2 briefly pulls Nano RESET pin LOW
* Arduino Nano restarts and resumes sensor acquisition

Protection Logic:

* Reset attempts are limited to prevent continuous reset loops
* Maximum one automatic fault-driven hardware reset per day
* The daily fresh-start software reset (Section 18.9.5.0.3) is separate and does NOT count against this limit
* Software-based recovery (RESET_REQ) is always attempted before hardware reset

Purpose:

* Recover from a Nano that is alive but stuck producing invalid data
* Improve autonomous operation
* Maintain monitoring reliability during long-term deployment


---

##### 18.9.5.1.1. Design Philosophy

The system prioritizes:
- Autonomous recovery
- Minimal manual intervention
- Reliable long-term operation

---

#### 18.9.5.2. Reset Dependency Chain

The Arduino Nano hardware reset path depends on ESP32 #2 operational availability.

Reset sequence:

ESP32 #1
↓ GPIO4 ACTIVE
Power enabled to ESP32 #2
↓
ESP32 #2 initializes
↓
PCF8575 initialized
↓
ESP32 #1 sends RESET_NANO command
↓
ESP32 #2 activates PCF8575 P17
↓
Transistor pulls Arduino Nano RESET LOW

Important Notes:

* ESP32 #1 does not directly control the Arduino Nano RESET pin
* Nano hardware reset depends on:

  * ESP32 #2 operational state
  * PCF8575 availability
  * I2C communication integrity
* ESP32 #2 watchdog recovery and external power-cycling mechanisms help maintain reset-path reliability

##### 18.9.5.2.1. Design Philosophy:

This distributed reset structure maintains centralized supervisory control while delegating hardware execution tasks to the actuator controller subsystem.


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
* HARD CAP: ESP32 #2 turns the relay OFF after a maximum of 10 seconds continuous ON, regardless of whether ESP32 #1 is still sending ON-hold. This caps a stuck-ON condition even if both the release and the link fail.

#### 18.10.8.4. Full Safety Bypass (operator is the safety)

In testing mode, normal protective logic is INTENTIONALLY bypassed — including no-flow, overcurrent, EC/pH, PZEM, and battery protections. The ONLY active protections are:

* the dead-man release (relay off when button released / signal lost), and
* the 10-second hard cap on ESP32 #2.

WARNING (documented operator responsibility): bypassing hardware-protection stops means manual testing can damage equipment (e.g. dry-running a pump, stalling a motor). Testing mode must never be left unattended; the operator holding the button is the safety. Use brief presses; do not hold a dry pump.

#### 18.10.8.5. Logging

All manual actuations in testing mode are logged to SD (relay/device, ON/OFF, duration), as ACT events (Section 25.2.1), so bench tests are recorded.

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

| Function                | GPIO |
| ----------------------- | ---- |
| Reservoir → Mixing Tank | 4    |
| Mixing → Irrigation     | 5    |
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

| Output | Device            |
| ------ | ----------------- |
| P0     | Reservoir Valve   |
| P1     | Column A Valve    |
| P2     | Column B Valve    |
| P3     | Column C Valve    |
| P4     | Mixing Tank Valve |
| P5     | Inverter Relay    |
| P6     | Transfer Pump     |
| P7     | Booster Pump      |
| P10    | Mixer Motor       |
| P11    | Nutrient A Pump   |
| P12    | Nutrient B Pump   |
| P13    | Nutrient C Pump   |
| P14    | Nutrient D Pump   |
| P15    | pH Up Pump        |
| P16    | pH Down Pump      |
| P17    | Nano RESET Control |

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

## 20.4. SIM800L Power Requirement

- Requires stable 3.7V–4.2V supply
- Peak current: up to 2A
- MUST NOT be powered directly from ESP32 5V pin
- Use dedicated buck converter or Li-ion source

---

# 21. 🔌 Power Monitoring System

## 21.1. Battery Monitoring (INA226)

- Measures battery voltage, current, and power
- Battery voltage is used to estimate remaining charge level
- Detects:
  - Charging / Discharging state
  - Idle vs active consumption
  - Low and critical battery levels

### 21.1.1. Control Usage:
- Low battery → Disable fertigation (irrigation only)
- Critical battery → Stop all operations
- Sends GSM alerts:
  - BATTERY_LOW
  - BATTERY_CRITICAL

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

  * Voltage present but current < 0.5A for 30 seconds
* Overcurrent > 3A
* Voltage outside 215–240V range
* Inverter ON but no voltage detected after 30 seconds

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
* All recovery actions: Nano watchdog events (as inferred), software RESET_REQ, hardware Nano reset, ESP32 #2 power-cycle, ESP32 #1 self-reset
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
| RESET      | ESP1           | TARGET|METHOD|reason  (METHOD = WDT/RESET_REQ/HW; reason = GARBAGE5/DAILY/TIMEOUT) | NANO|RESET_REQ|GARBAGE5 |
| DOSE       | ESP2           | NUTRIENT|target_mL|measured_mL|COL                         | NUT_A|50.0|49.2|COL_A                    |
| ACT        | ESP1/ESP2      | DEVICE|STATE|COL  (STATE = START/STOP)                     | IRRIGATION|START|COL_A                   |
| GSM        | GSM            | DIR|payload  (DIR = TX/RX)                                 | TX|ALERT|MAJ|NPK_FAULT|COL_B             |
| PWR        | ESP1           | SRC|fields  (SRC = INA226/PZEM)                            | INA226|V=12.6|I=1.2|P=15.1|CHG=150Wh    |

Notes:
* GARBAGE RAW is truncated to a safe length (e.g. 64 chars) to bound row size; the consecutive-garbage counter value is recorded as COUNT=n.
* DOSE logs both the target and the actual measured (flow-derived) volume, enabling dosing-accuracy analysis for the thesis.
* PWR snapshots are logged at a sensible interval (e.g. once per logging flush) and on significant battery-state changes, not on every reading, to limit volume.
* ACT logs each session start/stop; the consolidated scheduled-run GSM notice (Section 12.1.2) is logged separately as a GSM/TX event.
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
* Thesis data collection (the Arduino/sensor data is graphed manually on paper from these logs; graphing is not implemented in firmware)

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


