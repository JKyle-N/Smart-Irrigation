/* =============================================================================
 *  SMART IRRIGATION -- CALIBRATION TOOL -- FLOW K-FACTORS + PUMP RATES (ESP32 #2)
 * -----------------------------------------------------------------------------
 *  Runs a line's pump while you HOLD the on-board BOOT button, into a measuring vessel. From the pulses,
 *  the volume you caught and the time the pump ran it works out two things:
 *    K     pulses per litre, for that line's flow sensor
 *    rate  how fast the pump really moves water: the only tool for PUMP_FLOWRATE_MLPM, TRANSFER_LPM, BOOSTER_LPM
 *
 *  HOW TO USE (the tool tells you the next step each time)
 *    1. ch NUTA              Pick a line. MIXIRR also needs a column: ch MIXIRR A
 *    2. Hold BOOT            Fill the vessel. An AC line waits 5 s for the inverter first. Release to stop.
 *    3. ml 120  or  vol 3.0  What you caught. Repeat 2-3 at least 3 times.
 *    4. calc                 Checks the runs agree within 8 %, and prints the lines for CalStore.
 *    Also: r, live, runs, drop <n>, clear, off (stop everything now), h.
 *
 *  For a pump rate, hold BOOT in ONE go for about a minute: a stop and restart spoils the timing, not the K.
 *
 *  SAFETY
 *    - WATER ONLY, with the rig plumbed and a vessel in place.
 *    - Releasing BOOT, the hard cap, `off`, or (on an AC line) 10 s with no flow stops everything.
 *    - P4 is never driven. It is the battery-select DPDT now, not a valve: energizing it moves the boards
 *      to the secondary, which is why this tool's old MIXIRR run could reset the controllers.
 *    - ESP2's PCF8575 is on the rail ESP1 switches with GPIO4. If it is not found, that rail is off.
 *
 *  PINS, BITS, SIGN: same as ESP2/src/main.cpp. Flow is INPUT_PULLUP, FALLING. PCF8575 at 0x20, active LOW.
 * ========================================================================== */
#include <Arduino.h>
#include <Wire.h>

const int     I2C_SDA = 21, I2C_SCL = 22;
const uint8_t PCF_ADDR = 0x20;
#define B_COL_A    (1u << 1)
#define B_COL_B    (1u << 2)
#define B_COL_C    (1u << 3)
#define B_INVERTER (1u << 5)
#define B_TRANSFER (1u << 6)
#define B_BOOSTER  (1u << 7)
#define B_NUT_A    (1u << 9)
#define B_NUT_B    (1u << 10)
#define B_NUT_C    (1u << 11)
#define B_NUT_D    (1u << 12)
#define B_PH_UP    (1u << 13)
#define B_PH_DN    (1u << 14)
#define B_MASTER   (1u << 15)          // P17 master actuator-power cutoff
#define B_BATT_SEL (1u << 4)           // P4: the battery DPDT. Masked out of every write below

struct Chan {
  const char *id;
  uint8_t     flowPin;
  uint16_t    pump;
  bool        ac;        // needs the inverter + warmup
  bool        needCol;   // MIXIRR: opens a column valve
  const char *kKey;      // CalStore key for the K
  const char *rateKey;   // CalStore key for the rate, or NULL
  const char *rateConst; // ESP2/src/main.cpp constant for the rate, or NULL
};
const Chan CH[] = {
  { "RESMIX", 26, B_TRANSFER, true,  false, "kRM", NULL,    "TRANSFER_LPM" },
  { "MIXIRR",  5, B_BOOSTER,  true,  true,  "kMI", NULL,    "BOOSTER_LPM"  },
  { "NUTA",   23, B_NUT_A,    false, false, "kNA", "pumpA", NULL },
  { "NUTB",   19, B_NUT_B,    false, false, "kNB", "pumpB", NULL },
  { "NUTC",   18, B_NUT_C,    false, false, "kNC", "pumpC", NULL },
  { "NUTD",   27, B_NUT_D,    false, false, "kND", NULL,    NULL },
  { "PHUP",    4, B_PH_UP,    false, false, "kPU", NULL,    NULL },
  { "PHDN",   25, B_PH_DN,    false, false, "kPD", NULL,    NULL },
};
const int NCH = sizeof(CH) / sizeof(CH[0]);

#define PIN_BOOT 0                              // on-board BOOT button; pressed = LOW
const unsigned long BTN_DEBOUNCE_MS = 25;
const unsigned long WARMUP_MS  = 5000;          // ESP2's INVERTER_WARMUP_MS: the pump waits for the inverter
const unsigned long CAP_AC_MS  = 45000;         // ESP2's PRIME_CAP_MS
const unsigned long CAP_DC_MS  = 90000;         // long enough for a one-minute rate run
const unsigned long NO_FLOW_MS = 10000;         // ESP2's FLOW_TIMEOUT_MS. AC only: dosing pulses can be seconds apart
const float OUTLIER_TOL = 0.08f;                // ESP1's CAL_OUTLIER_PCT
const unsigned long MIN_PULSES = 100;           // below this one pulse is over 1 %
const uint8_t MAX_RUNS = 6;
const float K_MIN = 1.0f, K_MAX = 100000.0f;    // what ESP2 will accept (kSane)

enum Phase { IDLE, WARMUP, PUMPING };
Phase phase = IDLE;
volatile unsigned long flowPulses = 0;
int  activeFlowPin = -1;
int  selCh = -1, selCol = -1;
unsigned long phaseMs = 0;                      // when this phase began
unsigned long pumpMs = 0;                       // pump-on time collected for this run, across bursts
unsigned long lastPulseMs = 0, lastPulseCount = 0;
bool btnRaw = false, btnStable = false;
unsigned long btnChangeMs = 0;
bool capLatched = false;                        // stopped by a limit: release BOOT before it can run again
bool stopFailed = false;                        // an all-off write did not reach the PCF: keep trying, loudly
unsigned long lastRetryMs = 0;

struct Run { unsigned long pulses; float liters; float k; float lpm; };
Run runs[MAX_RUNS];
uint8_t nRuns = 0;

bool liveOn = false;
unsigned long lastLiveMs = 0;
String line;

void IRAM_ATTR flowISR() { flowPulses++; }
unsigned long snapPulses() { noInterrupts(); unsigned long p = flowPulses; interrupts(); return p; }

/* ---- Outputs ---------------------------------------------------------------- */
bool pcfWriteOnce(uint16_t bank) {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(bank & 0xFF);
  Wire.write((bank >> 8) & 0xFF);
  return Wire.endTransmission() == 0;
}
bool pcfWrite(uint16_t onBits) {
  uint16_t bank = 0xFFFF & ~(onBits & ~(uint16_t)B_BATT_SEL);
  for (uint8_t i = 0; i < 3; i++) if (pcfWriteOnce(bank)) return true;
  return false;
}
void safeAll() {
  if (pcfWrite(0)) { stopFailed = false; return; }
  stopFailed = true;
  Serial.println("!!! COULD NOT REACH THE PCF8575: outputs may still be ON. Retrying. Cut the rig's power if a pump runs.");
}
uint16_t lineBits() {     // everything but the pump: the bank's power, the inverter, the column valve
  const Chan &c = CH[selCh];
  uint16_t on = B_MASTER;
  if (c.ac) on |= B_INVERTER;
  if (c.needCol) on |= (selCol == 0) ? B_COL_A : (selCol == 1) ? B_COL_B : B_COL_C;
  return on;
}

void attachFlow(int pin) {
  if (activeFlowPin >= 0) detachInterrupt(digitalPinToInterrupt(activeFlowPin));
  noInterrupts(); flowPulses = 0; interrupts();
  activeFlowPin = pin;
  pinMode(pin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(pin), flowISR, FALLING);
}

void startPump() {
  if (!pcfWrite(lineBits() | CH[selCh].pump)) { safeAll(); phase = IDLE; capLatched = true; Serial.println("FAILED: no PCF8575."); return; }
  phase = PUMPING; phaseMs = millis();
  lastPulseMs = phaseMs; lastPulseCount = snapPulses();
  Serial.println("Pump ON. Release BOOT to stop.");
}

void startLine() {
  if (selCh < 0) { Serial.println("Pick a line first: ch <ID>"); return; }
  capLatched = false;
  if (CH[selCh].ac) {
    if (!pcfWrite(lineBits())) { safeAll(); Serial.println("FAILED: no PCF8575. ESP2's rail is switched by ESP1's GPIO4: is it on?"); return; }
    phase = WARMUP; phaseMs = millis();
    Serial.println("Inverter starting, pump in 5 s. Keep holding BOOT.");
  } else startPump();
}

void stopLine(const char *why) {
  safeAll();
  if (phase == PUMPING) pumpMs += millis() - phaseMs;
  phase = IDLE;
  unsigned long p = snapPulses();
  Serial.print("STOP ("); Serial.print(why); Serial.print("). Pulses so far: "); Serial.println(p);
  Serial.println("NEXT: hold BOOT to add more, or type what you caught: ml <mL> or vol <L>");
}

/* ---- Results ----------------------------------------------------------------- */
void printHelp() {
  Serial.println("Commands:");
  Serial.println("  ch <ID> [A|B|C]  pick a line (column only for MIXIRR). IDs:");
  Serial.println("                   RESMIX MIXIRR NUTA NUTB NUTC NUTD PHUP PHDN");
  Serial.println("  <hold BOOT>      run the pump; release to stop");
  Serial.println("  ml <mL> / vol <L>  what you caught: records this run");
  Serial.println("  calc             check the runs and print the result");
  Serial.println("  runs             list runs       drop <n>  remove a run      clear  remove all");
  Serial.println("  r / live         pulses now      off       stop everything now");
}

void printLive() {
  unsigned long p = snapPulses();
  Serial.print("pulses="); Serial.print(p);
  if (selCh >= 0) { Serial.print("  line="); Serial.print(CH[selCh].id); }
  Serial.println(phase == PUMPING ? "  [PUMP ON]" : phase == WARMUP ? "  [INVERTER WARMUP]" : "  [off]");
}

void listRuns() {
  if (!nRuns) { Serial.println("No runs yet."); return; }
  for (uint8_t i = 0; i < nRuns; i++) {
    Serial.printf("  run %u: %lu pulses, %.3f L, K=%.1f, %.3f L/min\n", i + 1, runs[i].pulses, runs[i].liters, runs[i].k, runs[i].lpm);
  }
}

float medianOf(float *v, uint8_t n) {
  for (uint8_t i = 1; i < n; i++) { float key = v[i]; int j = i - 1; while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; j--; } v[j + 1] = key; }
  return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
}

void doCalc() {
  if (selCh < 0) { Serial.println("Pick a line first: ch <ID>"); return; }
  if (nRuns < 3) { Serial.printf("Need at least 3 runs, you have %u.\n", nRuns); return; }
  float ks[MAX_RUNS], rs[MAX_RUNS];
  for (uint8_t i = 0; i < nRuns; i++) { ks[i] = runs[i].k; rs[i] = runs[i].lpm; }
  float medK = medianOf(ks, nRuns), medRate = medianOf(rs, nRuns);
  int worst = -1; float worstDev = 0;
  for (uint8_t i = 0; i < nRuns; i++) {
    float dev = fabs(runs[i].k - medK) / medK;
    Serial.printf("  run %u: K=%.1f, %.1f %% from the median%s\n", i + 1, runs[i].k, dev * 100.0f, dev > OUTLIER_TOL ? "  <-- OUT" : "");
    if (dev > worstDev) { worstDev = dev; worst = i; }
  }
  if (worstDev > OUTLIER_TOL) {
    Serial.printf("Not averaging bad data. NEXT: type drop %d, do that run again, then calc.\n", worst + 1);
    return;
  }
  float sumK = 0, sumR = 0;
  for (uint8_t i = 0; i < nRuns; i++) { sumK += runs[i].k; sumR += runs[i].lpm; }
  float k = sumK / nRuns, lpm = sumR / nRuns;
  const Chan &c = CH[selCh];
  Serial.println("======== RESULT ========");
  Serial.printf("%s: K = %.1f pulses/L, pump = %.3f L/min (%.1f mL/min)\n", c.id, k, lpm, lpm * 1000.0f);
  if (k < K_MIN || k > K_MAX) { Serial.println("That K is outside what ESP2 accepts: check the sensor and the volumes."); return; }
  if (fabs(medRate - lpm) > OUTLIER_TOL * lpm)
    Serial.println("The rate varied between runs. Fine for K. For the rate, redo with one long hold per run.");
  Serial.println("CalStore on ESP1 (flash Calibration/ESP1/CalStore, paste, flash the firmware back):");
  Serial.printf("  set %s %.1f\n", c.kKey, k);
  if (c.rateKey) Serial.printf("  set %s %.1f\n", c.rateKey, lpm * 1000.0f);
  if (c.rateConst) {
    Serial.println("Rate goes in ESP2/src/main.cpp, then reflash ESP2:");
    Serial.printf("  const float %s = %.2ff;\n", c.rateConst, lpm);
  }
  Serial.println("========================");
}

int chFromId(const String &s) {
  for (int i = 0; i < NCH; i++) if (s.equalsIgnoreCase(CH[i].id)) return i;
  return -1;
}

void recordRun(float liters) {
  if (selCh < 0) { Serial.println("Pick a line first: ch <ID>"); return; }
  if (phase != IDLE) { Serial.println("Release BOOT first."); return; }
  if (!(liters > 0.0f)) { Serial.println("usage: ml <millilitres caught>  or  vol <litres caught>"); return; }
  if (nRuns >= MAX_RUNS) { Serial.println("Six runs is plenty: type calc, or drop one."); return; }
  unsigned long p = snapPulses();
  if (p == 0) { Serial.println("0 pulses: nothing to record. Hold BOOT first. If water moved, check the sensor with FlowPinFinder."); return; }
  float k = (float)p / liters;
  float lpm = pumpMs ? liters / (pumpMs / 60000.0f) : 0.0f;
  runs[nRuns++] = Run{ p, liters, k, lpm };
  noInterrupts(); flowPulses = 0; interrupts();
  pumpMs = 0;
  Serial.printf("Run %u: K = %.1f pulses/L, %.3f L/min\n", nRuns, k, lpm);
  if (p < MIN_PULSES) Serial.println("WARNING: under 100 pulses. Catch more water for a better K.");
  if (nRuns >= 3) Serial.println("NEXT: type calc");
  else Serial.printf("NEXT: empty the vessel and do run %u: hold BOOT\n", nRuns + 1);
}

void handleLine(const String &in) {
  String s = in; s.trim();
  int sp = s.indexOf(' ');
  String cmd = (sp < 0) ? s : s.substring(0, sp);
  String arg = (sp < 0) ? "" : s.substring(sp + 1);
  arg.trim();
  cmd.toLowerCase();

  if (cmd == "h" || cmd == "help") { printHelp(); return; }
  if (cmd == "r")    { printLive(); return; }
  if (cmd == "live") { liveOn = !liveOn; Serial.println(liveOn ? "live ON" : "live OFF"); return; }
  if (cmd == "off")  { if (phase != IDLE) stopLine("off"); safeAll(); capLatched = true; Serial.println("All outputs off."); return; }
  if (cmd == "runs") { listRuns(); return; }
  if (cmd == "calc") { doCalc(); return; }
  if (cmd == "clear") { nRuns = 0; noInterrupts(); flowPulses = 0; interrupts(); pumpMs = 0; Serial.println("All runs removed."); return; }
  if (cmd == "drop" || cmd == "x") {
    int n = arg.length() ? arg.toInt() : nRuns;
    if (n < 1 || n > nRuns) { Serial.println("usage: drop <run number>"); return; }
    for (int i = n - 1; i < nRuns - 1; i++) runs[i] = runs[i + 1];
    nRuns--;
    Serial.printf("Run %d removed. %u left.\n", n, nRuns);
    return;
  }
  if (cmd == "ml")  { recordRun(arg.toFloat() / 1000.0f); return; }
  if (cmd == "vol") { recordRun(arg.toFloat()); return; }
  if (cmd == "ch") {
    int sp2 = arg.indexOf(' ');
    String id  = (sp2 < 0) ? arg : arg.substring(0, sp2);
    String col = (sp2 < 0) ? ""  : arg.substring(sp2 + 1);
    col.trim(); col.toUpperCase();
    int ci = chFromId(id);
    if (ci < 0) { Serial.println("Unknown line. Use: RESMIX MIXIRR NUTA NUTB NUTC NUTD PHUP PHDN"); return; }
    int colIdx = -1;
    if (CH[ci].needCol) {
      colIdx = (col == "A") ? 0 : (col == "B") ? 1 : (col == "C") ? 2 : -1;
      if (colIdx < 0) { Serial.println("MIXIRR needs a column: ch MIXIRR A"); return; }
    }
    if (phase != IDLE) stopLine("line changed");          // never leave a pump running across a switch
    selCh = ci; selCol = colIdx;
    nRuns = 0; pumpMs = 0; capLatched = false;
    attachFlow(CH[ci].flowPin);
    Serial.printf("Line %s ready (flow GPIO%u%s), up to %lu s per hold.\n", CH[ci].id, CH[ci].flowPin,
                  CH[ci].ac ? ", AC: 5 s inverter warmup first" : "", (CH[ci].ac ? CAP_AC_MS : CAP_DC_MS) / 1000);
    Serial.println("NEXT: put a vessel in place, hold BOOT to run, release to stop.");
    return;
  }
  Serial.println("Unknown command: type h");
}

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(50);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  delay(300);
  safeAll();
  Serial.println();
  Serial.println("=== FLOW K + PUMP RATE CALIBRATION (ESP32 #2) ===");
  Serial.println("Pumps run only while the BOOT button is HELD. WATER ONLY, vessel in place.");
  Wire.beginTransmission(PCF_ADDR);
  if (Wire.endTransmission() != 0) Serial.println("PCF8575 NOT FOUND: no pump can run. Its rail is switched by ESP1's GPIO4, so that must be on.");
  printHelp();
  Serial.println("START: ch NUTA");
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { if (line.length()) { handleLine(line); line = ""; } }
    else if (line.length() < 64) line += c;
  }

  if (stopFailed && millis() - lastRetryMs >= 200) { lastRetryMs = millis(); safeAll(); }

  bool pressed = (digitalRead(PIN_BOOT) == LOW);
  if (pressed != btnRaw) { btnRaw = pressed; btnChangeMs = millis(); }
  if (pressed != btnStable && millis() - btnChangeMs > BTN_DEBOUNCE_MS) {
    btnStable = pressed;
    if (btnStable) {
      if (capLatched) Serial.println("Release BOOT, then press again to continue.");
      else if (phase == IDLE) startLine();
    } else {
      if (phase != IDLE) stopLine("BOOT released");
      capLatched = false;
    }
  }

  if (phase == WARMUP && millis() - phaseMs >= WARMUP_MS) startPump();
  if (phase == PUMPING) {
    unsigned long now = millis(), p = snapPulses();
    if (p != lastPulseCount) { lastPulseCount = p; lastPulseMs = now; }
    if (now - phaseMs >= (CH[selCh].ac ? CAP_AC_MS : CAP_DC_MS)) { stopLine("time limit"); capLatched = true; }
    else if (CH[selCh].ac && now - lastPulseMs >= NO_FLOW_MS) {
      stopLine("no flow for 10 s: dry line, or the wrong sensor");
      capLatched = true;
    }
  }

  if (liveOn && millis() - lastLiveMs >= 500) { lastLiveMs = millis(); printLive(); }
}
