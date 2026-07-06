# Smart Irrigation & Fertigation — Operator Manual

A complete field reference for **running the system** and **reading everything it sends you**.
It has two interfaces:

- **GSM / SMS** — remote control. Text commands to the controller's SIM card; it texts replies,
  alerts, and reports back.
- **LCD + 5 buttons** — local control on the panel (UP, DOWN, ENTER, BACK, MODE).

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
4. The system irrigates/fertigates **automatically** on its schedule; you don't trigger runs manually.
   You'll get a **`RUN,…`** text when a column is serviced and an **`ALERT,…`** if anything goes wrong.

---

## 2. Commands you SEND (SMS)

| Command | Example | What it does | You get back |
|---|---|---|---|
| `STATUS` | `STATUS` | Live status + today's report. If a run is active, you get a quick busy-ack first, then the report when idle. | `RPT,…` (see §4) or `ACK,BUSY,…` |
| `SUMMARY[,<day>]` | `SUMMARY` / `SUMMARY,YESTERDAY` | The day's **averages** (per-column NPK + moisture, power, water/nutrient totals, counts), parsed from the SD log. | `SUM,…` (see §4) |
| `FULL SUMMARY[,<day>]` | `FULL SUMMARY,20260625` | The day's **hour-by-hour** record + de-duplicated errors + run/dose events + peaks. Can span several texts. | `FULL,… (i/N)` |
| `NET` | `NET` | WiFi / ThingSpeak link status. | `NET,WiFi:…` (see §4) |
| `SET,COL_x,N,..,P,..,K,..,pH,..` | `SET,COL_A,N,150,P,40,K,200,pH,5.8` | Set per-column nutrient targets (mg/kg) + pH. Any subset of N/P/K/pH may be given. | `ACK,SET,COL_A` |
| `SET,COL_x,PRESET,<CROP>` | `SET,COL_B,PRESET,LETTUCE` | Apply a built-in crop preset (see §7). | `ACK,SET,COL_B,PRESET,LETTUCE` |
| `MODE,COL_x,AUTO` | `MODE,COL_A,AUTO` | Column decides irrigate-vs-fertigate automatically (by the nutrient gap). | `ACK,MODE,COL_A,AUTO` |
| `MODE,COL_x,IRRIGATION_ONLY` | `MODE,COL_C,IRRIGATION_ONLY` | Water only for this column — never doses nutrients. | `ACK,MODE,COL_C,IRRIGATION_ONLY` |
| `NAME,COL_x,<name>` | `NAME,COL_A,Lettuce` | Give a column a name (shown in reports; saved). | `ACK,NAME,COL_A,Lettuce` |
| `STOP,ALL` | `STOP,ALL` | **Emergency stop** — halt all actuators now. | `ACK,STOP,ALL` |
| `WIFI,<ssid>,<pass>` | `WIFI,MyNet,my pass,word` | Set WiFi credentials (**owner-only**). Password is everything after the 2nd comma, so it may contain commas/spaces; the SSID may not. | `ACK,WIFI,MyNet` |
| `TSKEY,<1\|2\|3>,<key>` | `TSKEY,1,ABCD1234EFGH5678` | Set a ThingSpeak channel **write** key (**owner-only**). Ch1=Columns, Ch2=System, Ch3=Chem. | `ACK,TSKEY,1` |

**`<day>` token** (for SUMMARY / FULL SUMMARY): *omit* = today · `YESTERDAY` · `YYYYMMDD` (e.g.
`20260625`) · `NODATE` (the file used when the clock/RTC is dead).

**Fault-recovery replies** — see §5. The words `STOP`, `RELEASE`, `IRRIGATE`, `NORMAL` mean something
*only while a fault is being held*; the rest of the time `STOP` is treated as the emergency stop.

---

## 3. Owner gating

`WIFI` and `TSKEY` are **owner-only**: the sender's number must match the configured owner number
(compared on the last digits). If someone else sends them, you get **`ERR,AUTH`**. All other commands
are accepted from any number that can reach the SIM, and replies go back to whoever texted.

---

## 4. Messages you RECEIVE (how to read them)

### 4.1 Acknowledgements (`ACK,…`)
A successful command echoes back as `ACK,…`:

| Reply | Meaning |
|---|---|
| `ACK,SET,COL_A` | Targets saved for column A. |
| `ACK,SET,COL_A,PRESET,LETTUCE` | Preset applied. |
| `ACK,MODE,COL_A,AUTO` | Column mode changed. |
| `ACK,NAME,COL_A,Lettuce` | Column named. |
| `ACK,STOP,ALL` | Emergency stop accepted. |
| `ACK,STOP,HELD` | (During a held fault) you chose to keep holding. |
| `ACK,RESUME,NORMAL` / `…,IRRIGATE` / `…,RELEASE` | Your recovery choice was sent to the actuator controller. |
| `ACK,WIFI,<ssid>` / `ACK,TSKEY,<ch>` | WiFi / ThingSpeak setting saved. |
| `ACK,BUSY,FERTIGATION,COL_B` | The system is mid-run; your request (e.g. STATUS) is queued and answered when it finishes. |

### 4.2 Errors (`ERR,…`)
| Reply | Meaning / fix |
|---|---|
| `ERR,PARSE` | Command didn't parse — check commas/format. |
| `ERR,COL` | Bad column — use `COL_A` / `COL_B` / `COL_C`. |
| `ERR,MODE` | Mode must be `AUTO` or `IRRIGATION_ONLY`. |
| `ERR,NAME` | NAME needs a column and a name. |
| `ERR,PRESET` | Unknown preset name (see §7). |
| `ERR,AUTH` | Owner-only command from a non-owner number. |
| `ERR,WIFI` / `ERR,TSKEY` / `ERR,TSCH` | Bad WiFi/ThingSpeak syntax; `TSCH` = channel not 1–3. |
| `ERR,SUMDATE` | Bad `<day>` token for SUMMARY. |
| `ERR,CMD` | Unknown command keyword. |
| `BUSY,local edit; applied after` | Someone is editing the same setting on the LCD right now; your change is queued and applied when they finish. |

### 4.3 Run notice
`RUN,COL_A,FERTIGATION` or `RUN,COL_A,IRRIGATION` — one text per column service, sent when a run
starts. (No text per drop of telemetry — only run notices, alerts, and reports.)

### 4.4 Alerts (`ALERT,<tier>,<CODE>,<loc>[,<op>,COL_x,reply …]`)
Tiers: **CRIT** 🔴 (critical) · **MAJ** 🟠 (major) · **MIN** 🟡 (minor) · **WARN** (warning/info).
A hard actuator fault adds the current operation + column and a recovery menu, e.g.
`ALERT,CRIT,FLOW_FAIL,MAIN,FERTIGATION,COL_B,reply STOP/RELEASE/IRRIGATE/NORMAL`.

**Fault-code glossary:**

| Code | Tier | Meaning | What to do |
|---|---|---|---|
| `FLOW_FAIL` | CRIT | A pump ran but no/!enough flow was metered (dry line, blockage, dead pump). The run is **held**. | Reply with a recovery choice (§5) after checking the line. |
| `PWR_FAIL` | CRIT | Power fault on an AC pump (no current / overcurrent / mains voltage out of range). Run **held**. | Check pump/inverter/mains, then recover. |
| `SAFE_STOP` | CRIT | Local protective stop (e.g. mixer overcurrent). Run **held**. | Check the mixer/load, then recover. |
| `DOSE_TIMEOUT` | CRIT | A nutrient pump ran past its time ceiling without reaching the target dose — the dosing line is unprimed or its flow sensor is stuck. Run **held**. | Prime that nutrient line (Testing combos / Prime), check the dosing flow sensor, then recover. |
| `ESP2_ERROR` | CRIT | Actuator controller reported a generic execution failure. Run **held**. | Recover or `STOP,ALL`. |
| `ESP2_DONE_TIMEOUT` | CRIT | A run didn't finish within the time budget. | Inspect; system has stopped supervising that run. |
| `BATTERY_CRITICAL` | CRIT | Battery below the critical threshold — everything stops. | Charge / check the solar/battery. |
| `EC_FAIL` / `PH_FAIL` | MAJ | EC or pH was outside the safe window during dosing (batch still delivered). | Check nutrient mix / probes. |
| `SENSOR_FAIL` | MAJ | An EC/pH probe read railed (disconnected/shorted) — distinct from out-of-window. | Inspect/replace the probe. |
| `PCF_FAIL` | MAJ | The actuator relay driver isn't responding (relay-bus fault). | Inspect ESP2 wiring/I²C; other safeties still apply. |
| `ESP2_DEGRADED` | MAJ | One channel was disabled (a nutrient no-flow or mixer no-load) but the run **continued**. | Note which channel; service it later. |
| `ESP2_SILENCE` | MAJ | The actuator controller went quiet; recovery is under way. | Usually self-recovers. |
| `ESP2_NO_READY` | MAJ | Actuator controller didn't come up for a run/startup. | Check ESP2 power (GPIO4 relay). |
| `NPK_FAULT` | MAJ | A column's NPK sensor read invalid; that column won't fertigate this cycle. | Check the RS485/NPK probe. |
| `RES_LOW` | MAJ | Reservoir below the low threshold — runs blocked until refilled. | Refill the reservoir. |
| `BATTERY_LOW` | MAJ | Battery low — fertigation disabled (irrigation still allowed). | Charge. |
| `NANO_GARBAGE` | MAJ | Sensor hub kept sending garbage after a software reset; running on last-valid data. | Check the Nano / sensor wiring. |
| `NANO_RESET` | WARN | The sensor hub was told to self-reset (after 5 bad packets, or the daily fresh-start). | Informational. |
| `NANO_SILENCE` | MIN | The sensor hub went quiet (its own watchdog restarts it). | Informational; check if it persists. |

### 4.5 Daily report — `RPT,<date>,…`
Sent automatically once a day (and in reply to `STATUS`). Example:

```
RPT,20260625,A:Lettuce|N150P40K200|pH5.8|W12.5|Nu30.0,B:-|N0P0K0|pH6.0|W5.0|Nu0.0,
FLT,C0M1m0,RST,1,ST,IDLE_STATE,BAT,12.6V,CONS,180Wh,CHG,240Wh
```

| Field | Meaning |
|---|---|
| `20260625` | Date (YYYYMMDD); `0` if the clock is unset. |
| `A:Lettuce` | Column letter : name (`-` if unnamed). Only **enabled** columns appear. |
| `\|N150P40K200` | Target N-P-K (mg/kg). |
| `\|pH5.8` | Target pH. |
| `\|W12.5` | Water delivered to that column today (litres). |
| `\|Nu30.0` | Nutrient dosed today (mL). |
| `FLT,C0M1m0` | Faults today: **C**ritical / **M**ajor / **m**inor counts. |
| `RST,1` | Sensor-hub resets today. |
| `ST,IDLE_STATE` | Current system state. |
| `BAT,12.6V` | Battery voltage. |
| `CONS,180Wh` / `CHG,240Wh` | Energy consumed / charged today. |

### 4.6 Summary — `SUM,<day> …` (averages)
Example: `SUM,2026-06-25 A:N148P39K198M42 B:N-P-K-M51 BAT12.5V 3.2W CONS180 CHG240Wh H2O17.5L NUT30mL FLT C0M1m0 RST1 IRR2 FERT1 DOSE3`

| Token | Meaning |
|---|---|
| `A:N148P39K198M42` | Column A day-average N, P, K, and **M**oisture %. `-` = no samples. |
| `BAT12.5V 3.2W` | Average battery volts / watts. |
| `CONS180 CHG240Wh` | Energy consumed / charged (Wh). |
| `H2O17.5L NUT30mL` | Total water / nutrient delivered. |
| `FLT C#M#m#` | Fault counts by tier. |
| `RST# IRR# FERT# DOSE#` | Counts of resets, irrigations, fertigations, dose events. |
| `(noSD)` | Appended if the SD card wasn't readable. |

### 4.7 Full summary — `FULL,<day> hourly …`
Multi-line / multi-text. Each populated hour: `02h A N148 P39 K198 M42 B … | 12.5V 3.1W`. Then:
- `ERR:` — each distinct error code **once**, with the time it first occurred (or `ERR: none`).
- `EVT:` — run/dose events of the day.
- `PEAK Tmax… Vmin… Wpk… H2O…L NUT…mL FLT… RST…` — daily peaks and totals.
- Long reports are paced over several texts tagged `(i/N)`; `+more@SD` / `+more evt@SD` means the rest
  is on the SD card.

### 4.8 Network status — `NET,WiFi:…`
Example: `NET,WiFi:OK,RSSI-58,IP192.168.1.42,TS:ok,age12s,SSID:MyNet`
- `WiFi:OK/DOWN`, `RSSI` (dBm), `IP`, `TS:ok/--` (last ThingSpeak upload), `age…s` since that upload,
  `SSID`.

---

## 5. Faults & recovery (the held-fault flow)

When the actuator controller hits a **hard fault during a run** (FLOW_FAIL / PWR_FAIL / SAFE_STOP), it
**stops safely and holds**: it cuts power to the actuator bank but **stays alive remembering how much
liquid is already in the mixing tank** (so it can't overfill later). You get a CRIT alert naming the
operation and column, and the system **waits for your reply**. Reply with **one word**:

| Reply | Effect |
|---|---|
| **`STOP`** | Acknowledge & keep holding — nothing is delivered; you can decide later. |
| **`RELEASE`** | Dump whatever is currently in the mixing tank to that column **as-is** (no top-up, no dosing). |
| **`IRRIGATE`** | Finish as **irrigation only** — top the tank up to the column's water amount with plain water and deliver; skip remaining nutrients. |
| **`NORMAL`** | **Resume** the paused job from where it stopped (fill only the remaining litres, no re-dose), then deliver and flush. |

You can also choose these on the LCD fault screen (UP/DOWN to pick, ENTER to confirm).

> **Why no "just cancel"?** The mixing tank's only outlet is the booster pump into a column (there's no
> drain). So held liquid is eventually delivered to a column via RELEASE / IRRIGATE / NORMAL.
> A held fault occupies the actuator engine, so **other columns won't run until you resolve it**.

**`STOP,ALL`** (any time) is the hard emergency stop: it halts everything immediately. Recovery from a
full emergency stop is done at the LCD fault screen.

---

## 6. LCD + buttons (local control)

**Buttons:** UP, DOWN, ENTER, BACK, MODE.

- **MODE** toggles between the live data pages and the **Settings** menu (and exits Testing/Calibration).
- **Data pages** (UP/DOWN to cycle): **Home**, **Sensors**, **Columns**, **Power** (battery/INA226),
  **GSM + WiFi** health, **Fault** (last fault / recovery prompts).

### 6.1 Settings menu
MODE → menu; UP/DOWN to move, ENTER to open, BACK to leave.

| Item | What it does |
|---|---|
| **Set Clock** | Set RTC date/time. |
| **Schedule** | Per-column service window (AUTO default window, or MANUAL start/end H:M). |
| **Column Mode** | Per column: AUTO / IRRIGATION_ONLY / OFF (OFF disables the column). |
| **Preset** | Per column: pick a named crop preset or enter N/P/K/pH manually. |
| **Thresholds** | Soil start/stop % and the fertigate trigger gap. |
| **Calibration** | Calibrate sensors (see §6.3). Idle-only. |
| **Testing** | Manually pulse each relay (dead-man, see §6.4). Idle-only. |
| **Restore Defaults** | Reset operational settings to factory (see §6.5). Idle-only. |
| **Lock Screen** | Lock the buttons (see §6.6). |
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
- **Flow ResMix / MixIrr / Nut A/B/C** (3 runs): **hold ENTER to run the pump** (live pulse count
  shown), UP when done, enter the measured **volume**, ENTER. Repeat 3×; the system checks the three
  K-factors for an **outlier** and lets you redo the worst run or saves the median/average.
- **Prime Lines** are run the same dead-man way (hold ENTER opens the line's valve + runs its pump to
  purge air) before flow calibration.

Saving stores the value on the master controller and (for pump/EC/pH/current sensors) pushes it to the
actuator controller and confirms it. **No reflashing needed.**

### 6.4 Testing (Settings → Testing; idle only)
Manually exercise the actuators. **Hold ENTER** to energize the selected item (dead-man — it drops the
instant you release, with a hard time cap); UP/DOWN to choose. The **Master Cutoff** entry is reversed
here (holding it drops bank power) so you can test it without killing the others.

The list has the 16 single relays, then **priming combos** that run a valve + pump together so water
actually moves through the pipes (hard cap ~30 s; re-hold to keep going):

| Combo row | Energizes | Use |
|---|---|---|
| **Fill Res>Mix** | Inverter + Reservoir valve + Transfer pump | Prime/fill reservoir → mixing tank. |
| **Push>Col A / B / …** | Inverter + Mix valve + that Column's valve + Booster pump | Prime mixing tank → the chosen column. One row per **enabled** column — scroll (UP/DOWN) to the column you want. |

Hold ENTER to run a combo; release (or the 30 s cap) stops the pump and closes the valves immediately.
This replaces the idea of a separate "Prime Lines" menu.

### 6.5 Restore Defaults (Settings → Restore Defaults; idle only)
Double-confirm (NO/YES). Resets operational settings (modes, targets, names, schedules, thresholds) to
factory defaults. **Keeps**: all calibration, which columns are enabled (physical wiring), and WiFi /
ThingSpeak setup. Does not reboot.

### 6.6 Lock screen & emergency stop
- **Lock:** Settings → Lock Screen ignores all buttons except the unlock combo. **Press UP+DOWN
  together** to unlock. Automation keeps running while locked.
- **Emergency-Off combo:** press **MODE + BACK together** any time to force an emergency stop (works
  even when locked). The fault screen then offers `> Return to normal` / `Stay stopped` (UP/DOWN +
  ENTER).

---

## 7. Reference

### Crop presets (for `SET,COL_x,PRESET,<CROP>`)
| Name | N | P | K | pH |
|---|---|---|---|---|
| `LETTUCE` | 150 | 40 | 200 | 5.8 |
| `CARROT` | 120 | 50 | 250 | 6.2 |
| `TOMATO` | 180 | 45 | 300 | 6.0 |

### Columns
A / B / C. A column set to **OFF** (or physically unwired) is hidden from menus/reports and never
serviced. Column-enable reflects **wiring**, so Restore Defaults never changes it.

### Abbreviation glossary
N, P, K = nutrient mg/kg · M = soil moisture % · W/H2O = water (L) · Nu/NUT = nutrient (mL) ·
BAT = battery V · CONS/CHG = energy consumed/charged (Wh) · FLT C/M/m = Critical/Major/minor faults ·
RST = resets · IRR/FERT/DOSE = irrigation/fertigation/dose counts · EC = conductivity · pH = acidity.

### Commissioning note
Values tagged `[MEASURE]` / `[TBD]` / `[CONFIRM]` in the firmware (flow K-factors, soil/EC/pH
calibration, tank geometry, thresholds, battery limits, the owner phone number) are set per-rig — most
can now be set from the **Calibration** and **Thresholds** menus without reflashing.

---

*This manual reflects the firmware in this repository (ESP32 #1 `handleSms` / reports / alerts +
the LCD menu). If a command or message looks different on your unit, the firmware version may differ.*
