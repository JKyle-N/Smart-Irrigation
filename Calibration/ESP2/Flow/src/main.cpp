/* =============================================================================
 *  SMART IRRIGATION  --  CALIBRATION BENCH TOOL  --  8x FLOW SENSORS (ESP32 #2)
 * -----------------------------------------------------------------------------
 *  Standalone. No ESP1, no work orders. Measures the flow-sensor K-factors
 *  (pulses per LITER) that the REAL firmware doses/meters with (K_RES_MIX /
 *  K_MIX_IRR / K_NUT[]). Flow is single-factor, zero-offset: K = pulses / liters,
 *  measured 3x + median outlier check (spec sec.A.4.1).
 *
 *  DRIVES THE PUMP UNDER A HARDWARE DEAD-MAN: to move water the tool opens the line's
 *  valve(s) + runs its paired pump via the PCF8575 -- but ONLY while you physically
 *  HOLD the ESP32's on-board BOOT button (GPIO0). Release BOOT -> pump stops instantly.
 *  A hard per-class time cap still applies (sec.A.4.1.3): if you hold past the cap the
 *  pump auto-stops and you must RELEASE and re-press to continue. `off` = panic-SAFE.
 *
 *  PINS / BITS (match ESP2/src/main.cpp): flow pins RES_MIX 26, MIX_IRR 5, NUT_A 23,
 *  NUT_B 19, NUT_C 18, NUT_D 27, PH_UP 4, PH_DN 25 (ISR FALLING, INPUT_PULLUP,
 *  main.cpp:120-126,601-606). PCF8575 I2C SDA21/SCL22 addr 0x20, ACTIVE-LOW
 *  (0xFFFF = all OFF); output bits from main.cpp:99-115. Master cutoff bit 15 (P17)
 *  gates power to the whole bank and is energized whenever a pump runs.
 *  Dead-man button: on-board BOOT = GPIO0 (active-LOW, pressed = LOW).
 *  Line->(valve,pump) pairing follows the Prime table (spec sec.A.4.2.1).
 *
 *  !!! SAFETY: this drives real pumps/valves. Only run with the rig plumbed and a
 *      vessel in place. Releasing BOOT or the hard cap stops it. `off` = panic-SAFE. !!!
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                    help
 *    ch <ID> [A|B|C]      select+arm a channel (col required for MIXIRR); zeros its ISR.
 *                         Then HOLD the on-board BOOT button to flow, release to stop.
 *    r / live             pulse count (live toggles ~2 Hz)
 *    vol <liters>         enter caught volume -> stores this run's K, re-zeros for next run
 *    runs                 list captured runs
 *    calc                 median/outlier check -> final K to paste
 *    x / clear            drop last run / all runs
 *    off                  force ALL outputs SAFE (panic)
 *    IDs: RESMIX MIXIRR NUTA NUTB NUTC NUTD PHUP PHDN
 *
 *  Build: PlatformIO `pio run -e esp32dev` in this folder.
 * ========================================================================== */
#include <Arduino.h>
#include <Wire.h>

/* ---- PCF8575 (match ESP2/src/main.cpp:94-115) ---------------------------- */
const int      I2C_SDA = 21, I2C_SCL = 22;
const uint8_t  PCF_ADDR = 0x20;
#define B_RES_VALVE (1u<<0)
#define B_COL_A     (1u<<1)
#define B_COL_B     (1u<<2)
#define B_COL_C     (1u<<3)
#define B_MIX_VALVE (1u<<4)
#define B_INVERTER  (1u<<5)
#define B_TRANSFER  (1u<<6)
#define B_BOOSTER   (1u<<7)
#define B_NUT_A     (1u<<9)
#define B_NUT_B     (1u<<10)
#define B_NUT_C     (1u<<11)
#define B_NUT_D     (1u<<12)
#define B_PH_UP     (1u<<13)
#define B_PH_DN     (1u<<14)
#define B_MASTER    (1u<<15)          // P17 master actuator-power cutoff

/* ---- flow pins (match ESP2/src/main.cpp:120-126,1145-1155) ---------------- */
struct Chan {
  const char *id;
  uint8_t     flowPin;
  uint16_t    relays;    // ON bits (besides master + column)
  bool        mainLine;  // true -> short main-line cap; false -> longer dosing cap
  bool        needCol;   // MIXIRR needs a column valve
  const char *paste;     // firmware constant this K feeds (or a note)
};
Chan CH[] = {
  { "RESMIX", 26,  B_INVERTER|B_RES_VALVE|B_TRANSFER, true,  false, "K_RES_MIX  (main.cpp:136)" },
  { "MIXIRR",  5,  B_INVERTER|B_MIX_VALVE|B_BOOSTER,  true,  true,  "K_MIX_IRR  (main.cpp:137)" },
  { "NUTA",   23,  B_NUT_A,                           false, false, "K_NUT[0]   (main.cpp:138)" },
  { "NUTB",   19,  B_NUT_B,                           false, false, "K_NUT[1]   (main.cpp:138)" },
  { "NUTC",   18,  B_NUT_C,                           false, false, "K_NUT[2]   (main.cpp:138)" },
  { "NUTD",   27,  B_NUT_D,                           false, false, "(NutD not metered in normal firmware)" },
  { "PHUP",    4,  B_PH_UP,                           false, false, "(pH Up flow not metered in normal firmware)" },
  { "PHDN",   25,  B_PH_DN,                           false, false, "(pH Dn flow not metered in normal firmware)" },
};
const int NCH = sizeof(CH) / sizeof(CH[0]);

/* ---- dead-man = on-board BOOT button (GPIO0, active-LOW) ------------------ */
#define PIN_BOOT 0                             // ESP32 on-board BOOT button; pressed = LOW
const unsigned long BTN_DEBOUNCE_MS = 25;      // debounce the mechanical button
const unsigned long CAP_MAIN_MS     = 3000;    // main-line hard cap (high flow, ~1-3 s)
const unsigned long CAP_DOSING_MS   = 15000;   // dosing-line hard cap (low flow)
const float OUTLIER_TOL = 0.08f;               // +-8% from median flags a run
const uint8_t MAX_RUNS  = 6;

/* ---- state --------------------------------------------------------------- */
volatile unsigned long flowPulses = 0;
int     activeFlowPin = -1;
int     selCh  = -1;                            // selected channel index
int     selCol = -1;                            // 0/1/2 for MIXIRR
bool    running = false;
unsigned long runOnMs = 0;
// BOOT-button dead-man tracking
bool          btnRaw = false, btnStable = false;   // raw + debounced pressed state
unsigned long btnChangeMs = 0;                     // last raw-edge time (for debounce)
bool          capLatched = false;                  // hit the cap -> require a release before re-running

struct Run { unsigned long pulses; float liters; float k; };
Run     runs[MAX_RUNS];
uint8_t nRuns = 0;
bool    pendingVol = false;
unsigned long burstStartPulses = 0;            // pulses at the start of the current run's bursts

bool liveOn = false;
unsigned long lastLiveMs = 0;
String line;

void IRAM_ATTR flowISR() { flowPulses++; }

void pcfWrite(uint16_t bank);
void safeAll();
void energize();
void attachFlow(int pin);
void detachFlow();
unsigned long snapPulses();
void startRun();
void stopRun(const char *why);
void printHelp();
void printLive();
void listRuns();
void doCalc();
int  chFromId(const String &s);
void handleLine(const String &s);

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);
  analogReadResolution(12);
  pinMode(PIN_BOOT, INPUT_PULLUP);             // on-board BOOT button = hardware dead-man
  safeAll();                                   // outputs SAFE at boot (all OFF)
  Serial.println();
  Serial.println("=== 8x FLOW K-FACTOR CALIBRATION (ESP32 #2, BOOT-button dead-man) ===");
  Serial.println("!!! Pumps/valves run only while the BOOT button is HELD. Have a vessel ready. !!!");
  printHelp();
}

void loop() {
  // Serial: plain line parsing at all times (button is the dead-man now, not keystrokes).
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { if (line.length()) { handleLine(line); line = ""; } }
    else if (line.length() < 64) line += c;
  }

  // BOOT-button dead-man: debounce, then act on the edges.
  bool pressed = (digitalRead(PIN_BOOT) == LOW);     // active-LOW
  if (pressed != btnRaw) { btnRaw = pressed; btnChangeMs = millis(); }   // raw edge -> restart debounce
  if (pressed != btnStable && millis() - btnChangeMs > BTN_DEBOUNCE_MS) {
    btnStable = pressed;                             // debounced state change
    if (btnStable) {                                 // pressed: start flowing (if armed + allowed)
      if (selCh < 0)        Serial.println("no channel armed -- `ch <ID> [A|B|C]` first");
      else if (capLatched)  Serial.println("cap latched -- already released? re-press to run");
      else if (!running)    startRun();
    } else {                                         // released: stop immediately
      if (running) stopRun("BOOT released");
      capLatched = false;                            // a release clears the cap latch
    }
  }

  // Hard cap while running (per-channel), enforced regardless of the button.
  if (running) {
    unsigned long cap = CH[selCh].mainLine ? CAP_MAIN_MS : CAP_DOSING_MS;
    if (millis() - runOnMs > cap) { stopRun("hard cap"); capLatched = true; }   // release+re-press to continue
  }

  if (liveOn && millis() - lastLiveMs >= 500) { lastLiveMs = millis(); printLive(); }
}

/* ---- PCF8575 raw driver (active-LOW; 0xFFFF = all OFF) -------------------- */
void pcfWrite(uint16_t bank) {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(bank & 0xFF);          // P0..P7
  Wire.write((bank >> 8) & 0xFF);   // P8..P15
  Wire.endTransmission();
}
void safeAll() { pcfWrite(0xFFFF); }

// Energize the selected channel's relays (+ master cutoff + column for MIXIRR).
void energize() {
  uint16_t on = CH[selCh].relays | B_MASTER;
  if (CH[selCh].needCol) {
    on |= (selCol == 0) ? B_COL_A : (selCol == 1) ? B_COL_B : B_COL_C;
  }
  pcfWrite((uint16_t)(0xFFFF & ~on));   // clear ON bits (active-LOW)
}

/* ---- flow ISR (match ESP2/src/main.cpp:601-608) -------------------------- */
void attachFlow(int pin) {
  detachFlow();
  flowPulses = 0;
  activeFlowPin = pin;
  pinMode(pin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(pin), flowISR, FALLING);
}
void detachFlow() {
  if (activeFlowPin >= 0) { detachInterrupt(digitalPinToInterrupt(activeFlowPin)); activeFlowPin = -1; }
}
unsigned long snapPulses() { noInterrupts(); unsigned long p = flowPulses; interrupts(); return p; }

void startRun() {
  if (selCh < 0) { Serial.println("select a channel first: ch <ID> [A|B|C]"); return; }
  if (CH[selCh].needCol && selCol < 0) { Serial.println("MIXIRR needs a column: ch MIXIRR <A|B|C>"); return; }
  running = true;
  runOnMs = millis();
  burstStartPulses = snapPulses();   // pulses carry across bursts within this run
  energize();
  Serial.print("RUN ["); Serial.print(CH[selCh].id); Serial.println("] -- flowing while BOOT held; release/cap = STOP.");
}

void stopRun(const char *why) {
  safeAll();
  running = false;
  unsigned long p = snapPulses();
  Serial.print("STOP ("); Serial.print(why); Serial.print("). pulses so far = "); Serial.println(p);
  Serial.println("hold BOOT again to add more, or `vol <liters>` to record this run.");
  pendingVol = true;
}

/* ---- reporting / K math -------------------------------------------------- */
void printHelp() {
  Serial.println("Commands:");
  Serial.println("  h                 this help");
  Serial.println("  ch <ID> [A|B|C]   select+arm channel (col only for MIXIRR); zeros ISR");
  Serial.println("  <HOLD BOOT btn>   flow while held; release/cap stops (hardware dead-man)");
  Serial.println("  r / live          pulse count (live toggles)");
  Serial.println("  vol <liters>      record run K = pulses/liters, re-zero for next run");
  Serial.println("  runs / calc       list runs / median-outlier final K");
  Serial.println("  x / clear         drop last run / all runs");
  Serial.println("  off               force ALL outputs SAFE");
  Serial.println("  IDs: RESMIX MIXIRR NUTA NUTB NUTC NUTD PHUP PHDN");
}

void printLive() {
  Serial.print("pulses="); Serial.print(snapPulses());
  if (selCh >= 0) { Serial.print("  ch="); Serial.print(CH[selCh].id); }
  Serial.println(running ? "  [RUNNING]" : "  [idle]");
}

void listRuns() {
  if (!nRuns) { Serial.println("(no runs yet)"); return; }
  for (uint8_t i = 0; i < nRuns; i++) {
    Serial.print("  run "); Serial.print(i + 1);
    Serial.print(": pulses="); Serial.print(runs[i].pulses);
    Serial.print(" liters="); Serial.print(runs[i].liters, 3);
    Serial.print(" K="); Serial.println(runs[i].k, 1);
  }
}

void doCalc() {
  if (nRuns < 3) { Serial.print("need >=3 runs (have "); Serial.print(nRuns); Serial.println(") -- spec sec.A.4.1.6"); return; }
  float ks[MAX_RUNS];
  for (uint8_t i = 0; i < nRuns; i++) ks[i] = runs[i].k;
  for (uint8_t i = 1; i < nRuns; i++) { float key = ks[i]; int j = i - 1; while (j >= 0 && ks[j] > key) { ks[j+1] = ks[j]; j--; } ks[j+1] = key; }
  float median = ks[nRuns / 2];

  int worst = -1; float worstDev = 0;
  Serial.println("---- calc ----");
  Serial.print("median K = "); Serial.println(median, 1);
  for (uint8_t i = 0; i < nRuns; i++) {
    float dev = fabs(runs[i].k - median) / median;
    Serial.print("  run "); Serial.print(i + 1); Serial.print(" K="); Serial.print(runs[i].k, 1);
    Serial.print(" dev="); Serial.print(dev * 100.0f, 1); Serial.print('%');
    if (dev > OUTLIER_TOL) Serial.print("  <-- OUTLIER");
    Serial.println();
    if (dev > worstDev) { worstDev = dev; worst = i; }
  }
  if (worstDev > OUTLIER_TOL) {
    Serial.print("Run "); Serial.print(worst + 1);
    Serial.println(" disagrees > 8%. Redo it (drop the worst, re-run) then `calc` again.");
    return;
  }
  float sum = 0; for (uint8_t i = 0; i < nRuns; i++) sum += runs[i].k;
  float avg = sum / nRuns;
  Serial.print("All runs within +-8%. FINAL K = "); Serial.println(avg, 1);
  Serial.print("// paste into "); Serial.println(CH[selCh].paste);
}

int chFromId(const String &s) {
  for (int i = 0; i < NCH; i++) if (s.equalsIgnoreCase(CH[i].id)) return i;
  return -1;
}

void handleLine(const String &in) {
  String s = in; s.trim();
  int sp = s.indexOf(' ');
  String cmd = (sp < 0) ? s : s.substring(0, sp);
  String arg = (sp < 0) ? "" : s.substring(sp + 1);
  arg.trim();
  cmd.toLowerCase();

  if (cmd == "h")    { printHelp(); return; }
  if (cmd == "r")    { printLive(); return; }
  if (cmd == "live") { liveOn = !liveOn; Serial.print("live="); Serial.println(liveOn ? "ON" : "OFF"); return; }
  if (cmd == "off")  { safeAll(); running = false; capLatched = false; Serial.println("ALL OUTPUTS SAFE"); return; }
  if (cmd == "runs") { listRuns(); return; }
  if (cmd == "calc") { doCalc(); return; }
  if (cmd == "clear"){ nRuns = 0; pendingVol = false; Serial.println("all runs cleared"); return; }
  if (cmd == "x")    { if (nRuns) { nRuns--; Serial.println("last run dropped"); } else Serial.println("no runs"); return; }
  if (cmd == "run")  { Serial.println("`run` is gone -- HOLD the on-board BOOT button to flow, release to stop."); return; }

  if (cmd == "ch") {
    int sp2 = arg.indexOf(' ');
    String id  = (sp2 < 0) ? arg : arg.substring(0, sp2);
    String col = (sp2 < 0) ? ""  : arg.substring(sp2 + 1);
    int ci = chFromId(id);
    if (ci < 0) { Serial.println("unknown ID (RESMIX MIXIRR NUTA NUTB NUTC NUTD PHUP PHDN)"); return; }
    if (CH[ci].needCol) {
      col.toUpperCase();
      if (col == "A") selCol = 0; else if (col == "B") selCol = 1; else if (col == "C") selCol = 2;
      else { Serial.println("MIXIRR needs a column: ch MIXIRR <A|B|C>"); return; }
    } else selCol = -1;
    if (running) stopRun("channel changed");     // never leave a pump running across a channel switch
    selCh = ci;
    nRuns = 0; pendingVol = false;
    capLatched = false;                          // fresh channel -> clear any prior cap latch
    attachFlow(CH[ci].flowPin);                 // arm + zero this channel's ISR
    Serial.print("armed "); Serial.print(CH[ci].id);
    Serial.print(" (flow GPIO"); Serial.print(CH[ci].flowPin); Serial.print(")");
    if (CH[ci].needCol) { Serial.print(" col "); Serial.print((char)('A' + selCol)); }
    Serial.print(" cap "); Serial.print((CH[ci].mainLine ? CAP_MAIN_MS : CAP_DOSING_MS) / 1000); Serial.println("s");
    Serial.println("HOLD the on-board BOOT button to flow; release to stop.");
    return;
  }

  if (cmd == "vol") {
    if (selCh < 0) { Serial.println("select a channel first"); return; }
    if (arg.length() == 0) { Serial.println("usage: vol <liters>"); return; }
    float liters = arg.toFloat();
    if (liters <= 0.0f) { Serial.println("liters must be > 0"); return; }
    if (nRuns >= MAX_RUNS) { Serial.println("run buffer full -- `clear` first"); return; }
    unsigned long p = snapPulses();
    if (p == 0) { Serial.println("0 pulses -- run the pump (hold BOOT) before `vol`"); return; }
    float k = (float)p / liters;
    runs[nRuns].pulses = p; runs[nRuns].liters = liters; runs[nRuns].k = k;
    nRuns++;
    noInterrupts(); flowPulses = 0; interrupts();   // re-zero for the next independent run
    pendingVol = false;
    Serial.print("run "); Serial.print(nRuns); Serial.print(" K = "); Serial.print(k, 1); Serial.println(" pulses/L");
    if (nRuns >= 3) Serial.println("have >=3 runs -- type `calc`");
    return;
  }

  Serial.println("? unknown command -- type h");
}
