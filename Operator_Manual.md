# Smart Irrigation & Fertigation — Operator Manual

A complete field reference for **running the system** and **reading everything it sends you**.
It has three interfaces:

- **GSM / SMS** — remote control. Text commands to the controller's SIM card; it texts replies,
  alerts, and reports back.
- **LCD + 5 buttons** — local control on the panel (UP, DOWN, ENTER, BACK, MODE).
- **Website dashboard** — a browser-based view with live telemetry, history, and (for approved
  accounts) remote controls. See §8.

**Mental model:** the Arduino Nano *reads sensors*, ESP32 #1 (the master) *decides and talks to you*,
ESP32 #2 *runs the pumps and valves*. You only ever talk to ESP32 #1 — by SMS or at the LCD.

> Conventions for SMS: commands are **case-insensitive**, fields are **comma-separated, no spaces
> inside a field**. Columns are **A / B / C** (written `COL_A`, `COL_B`, `COL_C`). Some commands are
> **owner-only** (see [Owner gating](#owner-gating)).

---

## 1. Quick start

1. Put the recipient/owner phone number on the controller (set in firmware at commissioning).
2. Text **`STATUS`** to the controller's SIM — you should get a daily-report line back. That confirms
   two-way SMS works.
3. To change a plant's nutrient target: **`SET,COL_A,N,150,P,40,K,200,pH,5.8`**.
4. The system irrigates/fertigates **automatically** on its schedule. You can also start a run yourself:
   **Settings → Force Run** on the LCD (§6.8) or **Force Run** on the website. You get a **`RUN: …`** text
   when a column is serviced, and an alert (`CRITICAL: …` / `ALERT: …`) if anything goes wrong.

---

## 2. Commands you SEND (SMS)

| Command | Example | What it does | The reply starts with |
|---|---|---|---|
| `HELP` / `HELP,RUN` / `HELP,CROPS` / `HELP,SYSTEM` | `HELP,RUN` | The command list, one text per topic. | `HELP: text one command` |
| `STATUS` | `STATUS` | The report now. During a run you get a busy note first, then the report when it ends. | `DAILY REPORT <date> 1/N` (or `BUSY: Irrigation running on Col A.`) |
| `SUMMARY[,<day>]` | `SUMMARY,YESTERDAY` | The day's **averages** (per-column NPK and moisture, power, water and nutrients, counts), from the SD log. | `SUMMARY <date> 1/N` |
| `FULL SUMMARY[,<day>]` | `FULL SUMMARY,20261117` | The day **hour by hour**, each error once, run and dose events, peaks. Several texts. | `FULL SUMMARY <date> 1/N` |
| `STOP` (or `STOP,ALL`) | `STOP` | **Emergency stop.** While a fault is held, `STOP` instead means "stay on hold" (§5). | `OK: EMERGENCY STOP` |
| `CANCEL,30` / `CANCEL,10` / `CANCEL,OFF` | `CANCEL,30` | End the current run, and skip its column for 30 / 10 minutes, or switch the column **off** (saved; switch it back on in Settings → Column Mode or on the website). | `OK: Run cancelled, Col A` |
| `TANKCHECK` / `TANKCHECK,OK` | `TANKCHECK,OK` | Is a tank check needed / you have looked at the mixing tank (idle only, §5). | `TANK CHECK NEEDED …` / `OK: Tank check confirmed.` |
| `SET,COL_x,N,..,P,..,K,..,pH,..` | `SET,COL_A,N,150,P,40,K,200,pH,5.8` | Nutrient targets (mg/kg) and pH. Any subset; nothing is saved if one value is out of range. | `OK: Col A targets saved` |
| `SET,COL_x,PRESET,<crop>` | `SET,COL_B,PRESET,PECHAY` | A built-in crop preset (§7). | `OK: Col B uses preset PECHAY` |
| `MODE,COL_x,AUTO` / `MODE,COL_x,IRRIGATION_ONLY` | `MODE,COL_A,AUTO` | AUTO = nutrients when needed; IRRIGATION_ONLY = water only. | `OK: Col A mode is AUTO …` |
| `NAME,COL_x,<name>` | `NAME,COL_A,Lettuce` | The plant name (shown in reports; saved). | `OK: Col A plant name is now Lettuce` |
| `THRESH,<start %>,<gap>` | `THRESH,35,30` | Watering starts below `start` % soil moisture; nutrients are added when `gap` mg/kg or more below target. (The old `THRESH,<start>,<stop>,<gap>` still works; the stop is ignored.) | `OK: Watering starts below 35% …` |
| `NET` | `NET` | WiFi and ThingSpeak status. | `NETWORK` |
| `WIFI,<ssid>,<pass>` / `WIFI,ON` / `WIFI,OFF` | `WIFI,MyNet,my pass,word` | WiFi network (the password is everything after the 2nd comma) / the WiFi switch. | `OK: WiFi set to MyNet. Connecting now.` |
| `WIFIPORTAL` / `WIFIPORTAL,STOP` | `WIFIPORTAL` | The **Setup AP** portal (§6.6). | `OK: WiFi setup portal is ON …` |
| `TSKEY,<1\|2\|3>,<key>` | `TSKEY,1,ABCD1234EFGH5678` | A ThingSpeak channel write key (1 Columns, 2 System, 3 Chem). | `OK: ThingSpeak key for channel 1 saved.` |
| `FBASE,STATUS` (and `FBASE,ON` / `OFF`, `FBASE,<url>`, `FBASE,SIGNOUT`, `FBASE,TLS,ON` / `OFF`, `FBASE,APIKEY,<key>`, `FBASE,AUTH,<email>,<password>`) | `FBASE,STATUS` | Website sync: why it is or is not connecting, and its setup. Nothing secret is ever sent back. | `FIREBASE: signed in` … |
| `UPLOAD` / `UPLOAD,YYYYMMDD` | `UPLOAD,20261117` | Send that day's SD log (default yesterday) to the website, today included as far as it is written. Download it there: Diagnostics → SD card logs. | `OK: The log for … is queued for upload.` |
| `BATTSW` / `BATTSW,CLEAR` | `BATTSW,CLEAR` | Battery-switch status / clear a latched switch fault. | `BATTERY SWITCH` / `OK: Battery switch fault cleared.` |
| `SIMTEST` | `SIMTEST` | The SIM and modem check. Also sent by itself every Sunday with the 18:00 report: **if a Sunday passes with no SIM check text, check the SIM's load and the modem.** | `SIM CHECK (weekly)` |
| `USSD,<code>` / `USSD,CLEAR` / `USSD` | `USSD,*143#` | Save / clear / show your carrier's balance code. | `Balance code: *143#` |
| `BAL` | `BAL` | Ask the carrier for the SIM balance (needs the USSD code). | the carrier's text |
| `ALERTNUM,<number>` / `ALERTNUM,CLEAR` / `ALERTNUM` | `ALERTNUM,09171234567` | A second phone that also gets alerts. It only receives: its commands are refused. | the number |
| `SUPA,<url>,<key>` / `SUPA,CLEAR` | | The old Supabase log upload (that project expired on 17 Nov 2026; logs go to the website now). | `OK: Supabase login saved.` |

**`<day>` token** (for SUMMARY / FULL SUMMARY): *omit* = today · `YESTERDAY` · `YYYYMMDD` (e.g.
`20261117`).

**Fault-recovery replies** — see §5. The words `STOP`, `RELEASE`, `IRRIGATE`, `NORMAL` and `SERVO` mean
something *only while a fault is being held*; the rest of the time `STOP` is the emergency stop.

---

## 3. Owner gating

**All commands are owner-only**: the sender's number must match the configured owner number
(compared on the last 9 digits). This includes STATUS/NET/SUMMARY, the recovery replies
(STOP/RELEASE/IRRIGATE/NORMAL/SERVO), and all config. A text from any other number gets *"Sorry, this
number is not allowed to control the irrigation system."*, at most once every 10 minutes, and short codes
or promo senders get no reply at all. Set the owner number in `ESP1/include/secrets.h` before flashing, or
later on the Setup AP admin page (§6.6). A second alert phone (`ALERTNUM`) receives alerts only.

---

## 4. Messages you RECEIVE (how to read them)

### 4.1 Replies
Every reply is **one plain-English text** (at most 160 characters). Its first word tells you what happened:
- **`OK:`** — done.
- **`NOT DONE:`** — refused, with the reason and what to send instead.
- **`BUSY:`** — queued or deferred.
- **A heading** for a report: `DAILY REPORT`, `SUMMARY`, `NETWORK`, `BATTERY SWITCH`, `SIM CHECK`, `FIREBASE`.

```
OK: Col A targets saved
N150 P40 K200 mg/kg, pH 5.8
```
```
NOT DONE: Unknown column. Use COL_A, COL_B or COL_C.
```
```
BUSY: Someone is editing settings on the LCD.
Your change will be applied when they finish.
```

### 4.2 Run notice
One text per column service, when it starts:
```
RUN: Col A (Lettuce)
Irrigation: water only
Started 06:00
```

### 4.3 Alerts
Line 1 is the kind and the headline, then the detail, then what to do, then `(time CODE)`. The code is the same
one the LCD, the website and the SD log show.

The kind words are **`CRITICAL`** (Critical tier), **`ALERT`** (Major), **`WARNING`** and **`NOTICE`** (minor).
Alerts also go to the second alert phone. A real one from 17 Nov:
```
CRITICAL: Tank level sensor not reading
Col A irrigation; transfer; no settled level; level=blind; bad li..
Check the mixing tank ultrasonic
(17:40 LEVEL_BLIND)
```
When the run is **held**, a second text follows (owner only):
```
ON HOLD (Col A). Reply:
NORMAL = resume run
IRRIGATE = water only, no dose
RELEASE = deliver tank as is
STOP = stay on hold
CANCEL,30 = end run, skip 30m
```

### 4.4 Fault codes
| Code | Kind | The text says | What to do |
|---|---|---|---|
| `FLOW_FAIL` | CRITICAL, held | Tank level not moving, pump stopped | Check pump, valves, tank sensors, then reply (§5) |
| `LEVEL_BLIND` | CRITICAL, held | Tank level sensor not reading | Check the mixing tank ultrasonic, then reply |
| `SERVO_FAIL` | CRITICAL, held | Pump selector failed or stuck | Check the servo and its switch; reply `SERVO` to try again |
| `PWR_FAIL` | CRITICAL, held | Pump power fault, pump stopped | Check pump, wiring and AC supply |
| `INVERTER_FAIL` | CRITICAL, held | Inverter gave no AC power | Check the inverter and its wiring |
| `BATT_SWITCH_FAIL` | CRITICAL, held | Switch to backup battery failed | Check the backup battery and relay |
| `SAFE_STOP` | CRITICAL, held | Safety stop | Check the mixer and pumps |
| `DOSE_TIMEOUT` | CRITICAL, held | Nutrient pump ran too long | Prime the nutrient line |
| `ESP2_ERROR` | CRITICAL, held | Pump controller stopped the run | Check the pump controller board |
| `MIX_OVERFLOW` | CRITICAL (emergency stop) | Mixing tank overfilling | Pumps stopped. Check tank, clear on LCD |
| `ESP2_DONE_TIMEOUT` | CRITICAL | Pump controller never finished | All stopped. Check tank, clear on LCD |
| `ESTOP_KEPT` | CRITICAL | Emergency stop kept after restart | Clear it on the LCD to resume |
| `BATTERY_CRITICAL` | CRITICAL | Main battery critically low | All stopped. Charge it, clear on LCD |
| `ESP2_RESTARTED` / `ESP2_SILENCE` / `ESP2_COMM_LOST` / `ESP2_NO_READY` | ALERT | Pump controller restarted / not answering / link lost / did not start | Usually recovers by itself; check its power and wiring if it repeats |
| `ESP2_DEGRADED` | ALERT | Pump controller disabled a part | Run continues. Check it after (a weak pump, a fill short, a mixer with no load …) |
| `PCF_FAIL` | ALERT | Relay board not responding | Check the relay board wiring |
| `COL_MISMATCH` | ALERT | Column is off on pump controller | Skipped today. Match column settings |
| `NANO_GARBAGE` | ALERT | Sensor board sending bad data | Check the sensor board wiring |
| `SELF_RESET` | ALERT | Main controller restarting itself | Max once a day. Check if it repeats |
| `RTC_MISSING` / `RTC_INVALID` | ALERT | Clock module not found / Clock time is wrong | Nothing runs on the schedule until fixed |
| `SERVO_MISMATCH` | ALERT | Wrong pump ran, retrying | Check the pump selector servo |
| `LEVEL_HIGH` | ALERT | Mixing tank full before target | Run goes on. Check tank litres cal |
| `RES_EMPTY` / `RES_LOW` | ALERT | Water reservoir ran dry / is low | Refill it |
| `TANK_CHECK` | ALERT | Check the mixing tank | Runs wait. Then text `TANKCHECK,OK` (§5) |
| `EC_FAIL` / `PH_FAIL` | ALERT | Mixed batch EC / pH out of range | Batch still given. Check dosing / the pH probe |
| `DOSE_BATCH_LOW` | ALERT | Batch too small to dose | Watering only. Check volume settings |
| `DOSE_CARRY` | ALERT | Dosed batch went to other column | Note it for the crop records |
| `WO_OVERSIZE` | ALERT | Run command too long, not sent | Run skipped. Check column settings |
| `SENSOR_FAIL` | ALERT | Sensor not working (EC, pH, PZEM or a battery monitor) | Check its wiring |
| `SOIL_MISSING` / `NPK_FAULT` | ALERT | Soil moisture reading missing / NPK sensor not reading | Check that column's probes; watering only |
| `BATTERY_LOW` | ALERT | Main battery low | Check charging and the solar panel |
| `BATT2_LOW` / `BATT2_CRIT` | ALERT | Backup battery low / critically low | AC pumps are blocked while its resting charge is 30 % or less (until 35 %) |
| `BATT_SWITCH_RESET` / `BATT_SWITCH_FAULT` | ALERT | Battery switch reset the boards / Battery switching locked | Locked: clear on the LCD, the website or `BATTSW,CLEAR` (auto in 24 h) |
| `MAIN_BATT_CRIT` / `BACKUP_EXHAUSTED` / `BACKUP_SWITCH_FAILED` | CRITICAL | (backup feature only) | Check the batteries |
| `NANO_SILENCE` / `FORCE_ABORT` / `MANUAL_REVOKED` | NOTICE | Sensor board not answering / Manual run cancelled (and why) / Website control ended | Informational |
| `NANO_RESET` / `NET_STALL` / `THRESH_NO_FERT` / `RESUME_NO_REPLY` | WARNING | Sensor board was restarted / Internet task stuck / Soil start % too low to fertilize / No answer to your resume | Informational, or raise the start % / resend the reply |

**Why a forced run was cancelled** (`FORCE_ABORT`, after its countdown):
- **The rig was busy or locked:** `NOT_IDLE`, `IN_MENU` (someone in a menu), `BUSY`, `HELD`, `WEB_MANUAL`, `ACTUATIONS_OFF`.
- **Battery:** `BATT2_LOW`, `BATT2_MONITOR` / `BATT2_NOT_SEEN` (its monitor is not answering), `BATTSW_FAULT`,
  `BATTSW_RECHARGE` (pumps wait 1 minute after every restart: arm again after a minute), `MAIN_BATT_CRIT`.
- **Water or column:** `RES_LOW`, `COL_DISABLED` (the column was switched off).

### 4.5 Daily report — `DAILY REPORT <date> i/N`
Sent automatically once a day (18:00) and in reply to `STATUS`, in several pages. For example:
```
DAILY REPORT 2026-11-17 1/4
State: idle, ready
Faults: 0 critical, 2 major, 1 minor
Sensor board resets: 1
Battery 12.6V, backup 15.9V
```
The pages that follow give solar and energy (used / charged), the water and nutrients each column got today,
and each column's targets. `LOW` after a battery means it is under its low mark; `--` means its monitor isn't
answering.

### 4.6 Summary — `SUMMARY <date> i/N`
The day's averages from the SD log: each column's N, P, K and soil moisture, the battery and power, total water
and nutrients, fault counts, and the numbers of irrigations, fertigations and doses. If the SD card couldn't be
read, the text says so.

### 4.7 Full summary — `FULL SUMMARY <date> i/N`
Hour by hour (each column's NPK and moisture, battery and power), then each distinct error once with its first
time, the day's run and dose events, and the peaks and totals. Long reports go over several texts. When there is
more than fits, the text says the rest is on the SD card.

### 4.8 Network status — `NETWORK`
```
NETWORK
WiFi: connected to MyNet
Signal -67 dBm, IP 192.168.1.42
ThingSpeak: last upload OK, 12 s ago
```
While the setup portal is up it says so, with the network to join. `turned OFF` = the WiFi switch is off (§6.6).

---

## 5. Faults & recovery (the held-fault flow)

When the actuator controller hits a **hard fault during a run** (FLOW_FAIL, LEVEL_BLIND, SERVO_FAIL, PWR_FAIL,
SAFE_STOP, DOSE_TIMEOUT …), it
**stops safely and holds**: it cuts power to the actuator bank but **stays alive remembering how much
liquid is already in the mixing tank** (so it can't overfill later). You get a CRIT alert naming the
operation and column, and the system **waits for your reply**. Reply with **one word**:

| Reply | Effect |
|---|---|
| **`STOP`** | Acknowledge & keep holding — nothing is delivered; you can decide later. |
| **`RELEASE`** | Dump whatever is currently in the mixing tank to that column **as-is** (no top-up, no dosing). |
| **`IRRIGATE`** | Finish as **irrigation only** — top the tank up to the column's water amount with plain water and deliver; skip remaining nutrients. |
| **`NORMAL`** | **Resume** the paused job from where it stopped (fill only the remaining litres, no re-dose), then deliver and flush. |
| **`SERVO`** | (pump-selector faults) Switch the pump selector again, then resume — the same as `NORMAL`. |
| **`CANCEL,30`** / **`CANCEL,10`** / **`CANCEL,OFF`** | End the run; skip the column for 30 / 10 minutes or switch it off. Whatever is in the mixing tank stays there and goes to the next run's column. |

After **three identical holds in a row**, a `FLOW_FAIL` or `LEVEL_BLIND` menu marks RELEASE as *(best)*: the hardware is
not recovering. On the **fourth**, the system cancels the run by itself and skips the column for 30 minutes.

You can also choose these on the LCD fault screen (UP/DOWN to pick, ENTER to confirm).

> **Mind the tank.** The mixing tank's only outlet is the booster pump into a column (there's no drain), so held
> liquid is eventually delivered to a column. A held fault occupies the actuator engine: **other columns won't run
> until you resolve it**.

**Tank check after a restart.** If the master controller restarts in the middle of a run (a power dip), it no
longer knows what is in the mixing tank. It raises **`TANK_CHECK`**, and **scheduled runs wait**. To clear it:
1. Look at the mixing tank. If it holds water, empty it with Testing (Push>Col).
2. Text **`TANKCHECK,OK`**, or use **Settings → TankCheck >confirm**.

A forced run is never blocked by the check; starting one confirms it.

**`STOP,ALL`** (any time) is the hard emergency stop: it halts everything immediately. Recovery from a
full emergency stop is done at the LCD fault screen.

---

## 6. LCD + buttons (local control)

**Buttons:** UP, DOWN, ENTER, BACK, MODE.

- **MODE** toggles between the live data pages and the **Settings** menu (and exits Testing/Calibration).
- **Data pages** (UP/DOWN to cycle): **Home**, **Sensors**, **Columns**, **Power** (main battery V/I, where a minus current means it is charging; a `B2:` line for the secondary battery, with `*` marking the battery the controllers are running from; and a `Sol:` line for solar. SD card and clock status are on the Columns page's last line),
  **GSM + WiFi** health, **Fault** (last fault / recovery prompts).

### 6.1 Settings menu
MODE → menu; UP/DOWN to move, ENTER to open, BACK to leave.

| Item | What it does |
|---|---|
| **Enable Actuations** | Only shown while actuations are disabled (from the website or SMS): switches them back on. |
| **Set Clock** | Set RTC date/time. |
| **Schedule** | Per-column service window (AUTO default window, or MANUAL start/end H:M). |
| **Column Mode** | Per column: AUTO / IRRIGATION_ONLY / OFF (OFF disables the column). |
| **Preset** | Per column: pick a named crop preset or enter N/P/K/pH manually. |
| **Thresholds** | Soil start % and the fertigation gap. |
| **AC Voltage** / **AC Current** | The inverter's accepted AC window (default 215–240 V, 0.5–3.0 A). |
| **Pump Exercise** | The every-2-days preventive pump run: on/off and its length. |
| **Force Run** | Start a run now (§6.8). Idle only. |
| **Calibration** | Calibrate sensors and pumps (see §6.3). Idle only. |
| **Sensor Diag** | Raw sensor values (read only). |
| **Testing** | Manually run each relay (dead-man, see §6.4). Idle only. |
| **WiFi [ON/OFF]** / **Setup AP [ON/OFF]** | The WiFi switch / the setup portal (§6.6). |
| **Restore Defaults** | Reset operational settings to factory (see §6.5). Idle only. |
| **Lock Screen** | Lock the buttons (see §6.7). |
| **Reboot ESP1** | Restart the master controller (NO/YES confirm; refused during a run). |
| **Batt Switch** | Only shown while a battery-switch fault is latched: clears it. |
| **TankCheck >confirm** | Only shown while a tank check is needed: confirms you have looked at the mixing tank (§5). |
| **Exit** | Back to the data pages. |

### 6.2 Unsaved-changes dialog
If you change a value in an editor and press **BACK** with unsaved changes, you get a three-way prompt:
**SAVE** (commit), **DISCARD** (revert), **CANCEL** (stay and keep editing). Walking away (backlight
timeout) never auto-saves. A critical fault dismisses the dialog and discards the edit.

### 6.3 Calibration Mode (Settings → Calibration; idle only)
Pick a target from the list (disabled columns are hidden); the screen shows the **live raw** sensor
value. Targets and how to capture them:

- **Soil A/B/C** (2-point): probe in **air** → ENTER, probe in **water** → ENTER.
- **pH** (2-point): in **pH 7** buffer → ENTER, in **pH 4** buffer → ENTER.
- **EC** (2-point): **dry/air** → ENTER, in the **standard solution** → ENTER.
- **ACS712 zero**: motor OFF → ENTER.
- **Ultra Res / Mix empty**: tank empty → ENTER.
- **Temp / Hum / Lux offset**: capture raw → ENTER, then dial in the **true reference reading** → ENTER.
- **NPK trim A/B/C** (guarded): a warning appears (trimming biases your data — not needed normally);
  ENTER to edit N/P/K offsets, or **DOWN to reset them to 0**.
- **Tank Mix liters / Tank Res liters** (since 16 Nov): up to 8 "level → litres" points per tank, so the
  ultrasonics read litres correctly. The water itself is measured by the tanks now; the ResMix / MixIrr flow
  meters are no longer used.
- **Rate Transfer / Rate Booster / Rate Nut A–C**: a pump's measured rate (L/min or mL/min). `not measured` = the
  built-in default. The screen also shows the rate the last run actually saw (`seen:`).
- **Transfer pump est / Booster pump est** (since 17 Nov): set the seconds (3–60) and ENTER to run the pump; the
  time counts from the inverter switch-on. Weigh the water and enter the grams. From the 5th try the screen shows
  the fitted line (L/min and start litres); ENTER saves it. Until a pump is re-measured, the system uses the
  readings taken on 17 Nov: transfer 11.74 L/min + 0.47 L, booster 13.20 L/min − 0.65 L (reference §5.3.2).
- **Flow Reservoir**: the reservoir's inflow meter (on the sensor board).
- **Flow Nut A/B/C** (3 runs): **hold ENTER to run the pump** (live pulse count shown), UP when done, enter the
  measured **volume**, ENTER. Repeat 3×; the system checks the three K-factors for an **outlier** and lets you
  redo the worst run or saves the median/average.
- **Prime Lines** are run the same dead-man way (hold ENTER opens the line's valve + runs its pump to
  purge air) before flow calibration.

Saving stores the value on the master controller and (for pump/EC/pH/current sensors) pushes it to the
actuator controller and confirms it. **No reflashing needed.**

### 6.4 Testing (Settings → Testing; idle only)
Manually exercise the actuators. **Hold ENTER** to energize the selected item (dead-man — it drops the
instant you release; a single relay stops after **30 s**, and the Fill / Push combos may run **1 minute**,
counted only while the inverter is on). UP/DOWN to choose. After you release, the next press waits **1.5 s**
(the header says `TOO FAST: WAIT 1.5s`): pressing again and again switched the relay board too fast. The
**Master Cutoff** entry is reversed here (holding it drops bank power) so you can test it without killing
the others.

**Testing raises no automatic faults and never emergency-stops** — it won't kick you out or reset ESP2 on
a fault while you're testing (they're logged only). You are the safety here: the dead-man and 30 s cap
bound relay-on time, and **MODE + BACK** still force-stops everything if you need it.

The single-relay rows, in order: Batt->2nd, Nut A, Inverter, Col A Vlv, Col B Vlv, Spare P5, Col C Vlv,
Nut B, Mixer, Spare P11, Spare P12, Nut C, Nut D, pH Up, pH Down, Mast Cutoff.

The list has the 16 single relays, then **priming combos** that run a valve + pump together so water
actually moves through the pipes (1 minute of inverter time; re-hold to keep going):

| Combo row | Energizes | Use |
|---|---|---|
| **Fill Res>Mix** | Inverter + Transfer pump (draws straight from the reservoir) | Prime/fill reservoir → mixing tank. |
| **Push>Col A / B / C** | Inverter + that Column's valve + Booster pump | Prime mixing tank → the chosen column. **All** columns (A/B/C) are shown here regardless of the run config, so you can bench-test any column's wiring — scroll (UP/DOWN) to the one you want. |

| **Servo>Transfer / Servo>Booster** | The pump selector only (inverter stays off) | Check the selector moves; `[OK]` when confirmed. |

Hold ENTER to run a combo; release (or the cap) stops the pump and closes the valves immediately. When the
Fill or Push combo ends, the header shows how far the mixing tank moved. This replaces the idea of a separate
"Prime Lines" menu.

**Batt->2nd** (P0) energizes the DPDT and moves the controllers onto the secondary battery. **Until the
hold-up stage is fitted, this resets all three controllers** — use it only to verify the wiring on the bench.

Any row that raises the inverter (**Inverter**, **Fill**, **Push>Col**) is refused when the backup battery
blocks the AC pumps:
- its resting charge is **30 % or less** (allowed again from 35 %);
- its monitor is not answering;
- a switch fault is latched;
- or the system restarted less than a minute ago.

The header names the reason, for example `BATT2 low: 28%` or `Restarted: wait 40s`.

### 6.4.1 Batt Switch (Settings → Batt Switch; idle only)
Since 16 Nov the row appears **only while a switch fault is latched** (after two switch resets in 24 h); the
switch's normal status is on the LCD **Power** page. **ENTER** on the row clears the latch and re-arms switching
(also: the website, or SMS `BATTSW,CLEAR`; it clears by itself after 24 h). The LCD **Power** page shows `SWFLT` on its last line while
the fault is latched.

### 6.5 Restore Defaults (Settings → Restore Defaults; idle only)
Double-confirm (NO/YES). Resets operational settings (modes, targets, names, schedules, thresholds) to
factory defaults. **Keeps**: all calibration, which columns are enabled (physical wiring), and WiFi /
ThingSpeak setup. Does not reboot.

**Reboot ESP1** (Settings → **Reboot ESP1**): software-restarts the controller without the physical
reset button — a NO/YES confirm, then it flushes logs and restarts. Config is kept (nothing is erased);
irrigation resumes on boot. Handy if something seems stuck. (Also available on the SoftAP admin page.)

### 6.6 WiFi controls (Settings menu)
The MODE-button menu has two live WiFi toggles (each shows `[ON]`/`[OFF]`):

- **`WiFi [ON/OFF]`** — master switch for the WiFi connection + ThingSpeak uploads. Turn it **OFF** to
  stop using WiFi entirely (e.g. to save power or when no network is available); **ON** reconnects. The
  setting is remembered across reboots. `NET` / the GSM+WiFi page then read `WiFi:OFF`. (SMS equivalent:
  **`WIFI,ON`** / **`WIFI,OFF`**, owner-only.)
- **`Setup AP [ON/OFF]`** — starts/stops the provisioning portal below.

**Setup AP (provisioning portal)** — the easiest way to set the WiFi network the controller joins, with
no SMS or button-typing:

1. Start it: **Settings → Setup AP** (toggle to `[ON]`), or text **`WIFIPORTAL`** (owner-only). The LCD
   shows a **WiFi Setup Mode** banner with the AP name and address. (Works even if `WiFi` is `[OFF]`.)
2. On your phone's WiFi list, join **`Irrig-Setup`** (its password is set before flashing as `SECRET_AP_PASS` in `ESP1/include/secrets.h`; ask whoever set up the unit).
3. Open a browser to **`http://192.168.4.1`** (most phones pop this up automatically). You'll see a
   form listing **nearby networks** with signal strength.
4. Pick your network (or type a hidden SSID), enter its **password**, tap **Save & Connect**.
5. The controller leaves the setup AP and connects to your network; check with the **GSM+WiFi** LCD
   page or by texting **`NET`** (shows `WiFi:OK`, signal, IP). Credentials are remembered across reboots.

Cancel any time with the **BACK** button, by texting **`WIFIPORTAL,STOP`**, or just wait — it closes
itself after **10 minutes** if unused. Irrigation keeps running normally the whole time. *(The AP name
is fixed in firmware; its password comes from `secrets.h`.)*

**Admin area (on the same portal page).** Below the WiFi form is an **Admin** box. Enter the admin **PIN**
(the first-boot PIN is `SECRET_ADMIN_PIN` in `secrets.h` — change it here at commissioning) and tap **Unlock admin**. Then you can:

- **Owner number** — see the current SMS owner number and change it. The new number is what all SMS
  commands are gated to (and where alerts/reports go). Saved to memory, survives reboot.
- **Logs** — every day's `.CSV` is listed with its size. **view** shows the last ~16 KB in your browser
  (quick check); **get** downloads the whole file to your phone (for the thesis / Excel).
- **Format SD** — tick the confirm box and tap **Erase SD** to **delete all files** on the card (frees
  space / clears history; logging resumes into a fresh file). *This is a full erase, not recoverable.*
- **ThingSpeak keys** — pick a channel (1 Columns / 2 System / 3 Chem) and paste its **Write API key**
  (shown only as set/unset, never displayed back).
- **Columns (A/B/C)** — set each column's **mode** (AUTO vs Irrigation-only), **name**, **nutrient
  targets** (N/P/K/pH), or **apply a crop preset**.
- **Thresholds** — soil **Start<** / **Stop>** % and the **fertigation gap**.
- **Reboot ESP1** — tick the box and tap **Reboot ESP1** to software-restart the controller (config is
  kept; the AP drops — reconnect after ~15 s).
- **Change admin PIN** — set a new 4–12-char PIN (saved to memory).

*Config edits show "queued" and take effect within a moment (they don't text you a confirmation). The
same settings are still available by SMS.*

Admin re-locks itself when the portal closes. Note: the portal AP password and PIN travel over a local
Wi-Fi link in plain text — fine for setup while you're standing at the unit, but change the default PIN
and don't leave the portal running unattended.

### 6.7 Lock screen & emergency stop
- **Lock:** Settings → Lock Screen ignores all buttons except the unlock combo. **Press UP+DOWN
  together** to unlock. Automation keeps running while locked.
- **Emergency-Off combo:** press **MODE + BACK together** any time to force an emergency stop (works
  even when locked). The fault screen then offers `> Return to normal` / `Stay stopped` (UP/DOWN +
  ENTER).

### 6.8 Force Run (Settings → Force Run; idle only)
Start a run now, outside the schedule.

**Set it up:**
- **Mode:** Col A, Col B, A+B together, or A then B (two batches).
- **For each batch:** litres (0.5 L steps, up to 20 L), nutrient A / B / C in mL (25 mL steps, up to 500 mL; 0 =
  water only), and the start delay (20–300 s).
- **Arm it:** ENTER after the last field. During the countdown, BACK cancels it.

**Before it starts:**
- **A column that is OFF:** arming switches it back on; the bottom line warns first, `Col A   OFF, ARM=ON`.
- **When the countdown ends**, the system checks again that it is idle, the batteries allow AC pumps, and the
  reservoir is not low. If not, the run is cancelled with a `Manual run cancelled` notice that says why (§4.4).
- **No limits otherwise:** forced runs have no daily limit, and the tank check does not block them.

### 6.9 How the water is measured (since 16–17 Nov)
- **The tanks are the meters:** the ultrasonic sensors measure the water; the two main-line flow meters are no
  longer used.
- **A fill is timed:** the transfer pump runs for the time the litres need, counted from the inverter switch-on.
  The tank then settles for about a minute and is measured, with a top-up run if more than 3 L is missing.
- **A delivery:** the booster runs until the mixing tank is under 5 %, then 10 s more.
- **On the LCD:** the home page shows each tank's % and litres. A `?` after it (`Mix:40%?`) means the reading was
  too noisy to be used for control (the pings disagreed, often the inflow or the mixer).

---

## 7. Reference

### Crop presets (for `SET,COL_x,PRESET,<CROP>`)
| Name | N | P | K | pH |
|---|---|---|---|---|
| `PECHAY` | 180 | 60 | 120 | 6.0 |
| `TOMATO_S1` | 200 | 80 | 160 | 6.0 |
| `TOMATO_S2` | 240 | 100 | 200 | 6.0 |
| `TOMATO_S3` | 150 | 80 | 300 | 6.0 |

### Columns
A / B / C. A column set to **OFF** (or physically unwired) is hidden from menus/reports and never
serviced. Column-enable reflects **wiring**, so Restore Defaults never changes it.

### Abbreviation glossary
N, P, K = nutrient mg/kg · M = soil moisture % · W/H2O = water (L) · Nu/NUT = nutrient (mL) ·
BAT = battery V (from the opto-isolated ADC sensor) · CONS = energy consumed (Wh); CHG stays 0 (the current sensor reads magnitude only) · FLT C/M/m = Critical/Major/minor faults ·
RST = resets · IRR/FERT/DOSE = irrigation/fertigation/dose counts · EC = conductivity · pH = acidity.

### Commissioning note
Values tagged `[MEASURE]` / `[TBD]` / `[CONFIRM]` in the firmware (flow K-factors, soil/EC/pH
calibration, tank geometry, thresholds, battery limits, the owner phone number) are set per-rig — most
can now be set from the **Calibration** and **Thresholds** menus without reflashing.

---

## 8. Website dashboard

A browser-based third interface, separate from SMS and the LCD. It talks to the controller through
Firebase, not directly — so it can be open from anywhere with internet, not just standing at the rig
or in SMS range. *(Ask whoever manages the system for the dashboard's actual web address — it isn't
repeated here since it can change independently of this manual.)*

### 8.1 Getting an account
1. Open the dashboard link and click **Create account** — name, email, password.
2. Your new account starts as **pending**: you can see the read-only Live dashboard immediately, but
   every control that would touch real hardware stays blocked until an operator approves you.
3. Ask an operator to approve you in **User Management**. Once approved, the blocked controls unlock
   automatically — no need to sign out and back in.
4. Forgot your password? Use **Forgot password?** on the sign-in screen; a reset link is emailed to
   you the same way any normal account recovery works.

### 8.2 Account levels
The dashboard has more levels than SMS/LCD access, because unlike a single owner phone number, several
people may need to see or use it at once:

| Level | Who | Can see | Can touch hardware |
|---|---|---|---|
| **Pending / Restricted / Rejected** | A brand-new signup, or an approved account an operator has limited, or one an operator reviewed and did not approve | Every tab except Manual/Test, System, and User Management — but every actuating button on them is disabled, and Zone Profiles is read-only text. Only the account-status banner's wording tells these three apart | No |
| **Approved** | A normal approved account | Same tabs as above, all now actually usable | Yes — Controls, Zone Profiles, Force Run, Emergency Stop, recovery. **Not** Manual/Test or System (see below) |
| **Sub-operator** | An approved account an operator has separately elevated, no time limit | Everything Approved sees, plus **Manual/Test** and **System** | Yes, everything Approved has, plus direct relay tests and remote Set Clock/Thresholds/Restore Defaults. Stays in effect until an operator takes it back |
| **Temp access (1h)** | An approved account an operator has elevated for exactly one hour | Same reach as Sub-operator | Same as Sub-operator, but it expires on its own after 1 hour — no need for an operator to remember to take it back |
| **Operator** (Main / Creator) | The two fixed accounts that administer the system | Everything, including **User Management** | Everything, always |
| **Blocked** | An account an operator has disabled | Nothing — a full-screen "access blocked" takeover replaces the entire dashboard | No |

Sub-operator and the 1-hour temp grant unlock the *exact same* things — the only difference is whether
it expires by itself. An operator grants or withholds either from that account's row in **User
Management**; hovering the small **"i"** next to each button explains which is which if it's not
obvious at a glance.

### 8.3 Tabs
| Tab | Who sees it | What it's for |
|---|---|---|
| **Live dashboard** | Everyone signed in | Read-only telemetry: reservoir/mixing levels, flow, battery, per-zone NPK/pH/EC/moisture, run progress, command history. |
| **Zone profiles** | Everyone signed in | Crop/growth-stage reference notes per physical zone (A/B/C). Editable only once approved; read-only text otherwise. Purely a dashboard planning note — the rig only actually changes when you press "Send to ESP1" on the firmware-settings form underneath. |
| **Controls** | Approved and above | Disable/re-enable actuations (while disabled, LCD Testing and calibration priming are refused too), reboot Nano/ESP2/ESP1, Emergency Stop, Force Run (a real one-off irrigation/fertigation outside the schedule). |
| **Manual/Test** | Sub-operator / temp-access / operator only | **Direct hardware control** — every button here energises a real relay immediately, with none of the checks a scheduled run goes through. Opening it takes the rig out of automatic until you leave. Covers preventive pump exercise, Timed Pulses (run a pump/valve combo for a set number of seconds), and Hold-to-run (press-and-hold buttons that mirror the LCD Testing screen's own dead-man button — energised only while held. Because the press travels through Firebase, release is not instant: the relay drops about 1 s after you let go, within about 4 s if the tab or connection is lost, and after 15 s of continuous holding at the latest. After that cap, let go and press again to restart). The pH pumps dispense a corrosive adjuster and ask you to confirm first. |
| **Diagnostics** | Everyone signed in | The same raw/pre-conversion sensor values the LCD's Sensor Diag pages show, plus recent ESP1 events. |
| **System** | Sub-operator / temp-access / operator only | Remote equivalents of LCD Settings-menu items not covered elsewhere: Set Clock, Thresholds, Restore Defaults, LCD Lock status (read-only). |
| **User Management** | Operators only | Approve/reject/restrict/block/delete accounts, force a sign-out ("Kick," only offered while that account is actually online), see who's online/offline and for how long, grant/withhold Sub-operator or the 1h temp grant, and see each account's most recent actions (last 15, click to expand). |

### 8.4 "Waiting for a current ESP1 snapshot"
The dashboard only ever shows what ESP1 last reported — if that snapshot goes stale (ESP1 offline, no
WiFi, etc.), every actuating control on the page disables itself rather than let you send a command
into the dark. You can still **open** Manual/Test with a stale snapshot (Proceed always works for an
account with the right access level) — you just won't be able to press anything inside it until the
connection is confirmed current again, at which point a **"Direct hardware control"** banner reappears
as a reminder before you touch anything.

### 8.4.1 Live updates, Refresh and the SD card logs (17 Nov)
- **Live updates:** while a dashboard page is open and visible, the controller sends its status every **2 s during a run**
  and every **4 s** otherwise (every 20 / 60 s when nobody is watching). **Refresh** asks for a new status at
  once, and the page says how long the answer took.
- **Diagnostics → SD card logs:** the last 14 days of the controller's CSV log. **Download** any day.
  **Upload from the rig** asks for any day on the card, today included as far as it is written.
- **Manual/Test → SD card log (CSV):** **Update today's log**, then **Download today's log**. The button works
  only while the controller is reporting and idle, with nothing running; otherwise it says why. A day's log takes
  about 2–3 minutes per megabyte to arrive.
- **By SMS:** `UPLOAD` / `UPLOAD,YYYYMMDD` sends a day the same way.

### 8.5 Pop-up notifications
Since 2026-09, most actions (command results, account-management outcomes) also show as a small pop-up
in the corner of the screen — bottom-right on a computer, top of the screen on a phone — fading out on
its own after a few seconds. If several happen close together they clear out faster than usual rather
than piling up; click one to dismiss it early. This is in addition to, not instead of, the normal status
text elsewhere on the page, which stays there for you to read back later.

---

*This manual reflects the firmware in this repository (ESP32 #1 `handleSms` / reports / alerts +
the LCD menu) and the website as of 2026-11-17 (web v.4 "pure ultrasonic"). The full technical detail is in
`Smart_Irrigation_System_Reference_FINAL.md`: SMS §12.2.5, fault codes §23.4, LCD §18.10.7.5, website §29.11,
calibration defaults §5.3. If a command, message, or dashboard control looks
different on your unit, the firmware or website version may have moved on since this was written.*
