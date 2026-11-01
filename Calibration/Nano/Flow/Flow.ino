/* =============================================================================
 *  SMART IRRIGATION -- CALIBRATION TOOL -- RESERVOIR FLOW (Arduino Nano)
 * -----------------------------------------------------------------------------
 *  Measures the reservoir flow sensor's K-factor, pulses per litre. The Nano has no pump, so you move the
 *  water by hand. Use a good volume: a litre or more.
 *
 *  HOW TO USE (the tool tells you the next step)
 *    1. start            opens a counting window
 *    2. Pour a known volume through the sensor.
 *    3. stop
 *    4. vol 2.0          the litres you measured. Repeat 1-4 at least 3 times.
 *    5. calc             checks the runs agree within 8 % and prints the result
 *    Also: r, live, runs, drop <n>, clear, h.
 *
 *  PIN as in Nano/Nano.ino: D2 (INT0), RISING, INPUT_PULLUP. Disconnect D0/D1 while flashing and using this.
 * ========================================================================== */
#include <Arduino.h>

const uint8_t PIN_FLOW = 2;
#define FLOW_INTERRUPT_EDGE  RISING
const float NANO_K = 450.0f;                // what Nano.ino and ESP1's NANO_FLOW_K ship with
const float OUTLIER_TOL = 0.08f;            // ESP1's CAL_OUTLIER_PCT
const unsigned long MIN_PULSES = 100;       // below this one pulse is over 1 %
const uint8_t MAX_RUNS = 6;

volatile unsigned long flowPulseCount = 0;

bool          windowOpen = false;
unsigned long windowPulses = 0;
unsigned long windowStartMs = 0, windowMs = 0;

struct Run { unsigned long pulses; float liters; float k; };
Run     runs[MAX_RUNS];
uint8_t nRuns = 0;
bool    pendingVol = false;

bool          liveOn = false;
unsigned long lastLiveMs = 0, lastLivePulses = 0;
const unsigned long LIVE_MS = 1000;

#define LINE_BUF 32
char    line[LINE_BUF];
uint8_t lineLen = 0;

void flowISR() { flowPulseCount++; }

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  start / stop   open and close a counting window"));
  Serial.println(F("  vol <L>        the litres you measured: records this run"));
  Serial.println(F("  calc           check the runs and print the result"));
  Serial.println(F("  runs           list runs      drop <n>  remove one      clear  remove all"));
  Serial.println(F("  r / live       pulses now, and live on/off"));
}

unsigned long snapPulses() {
  noInterrupts(); unsigned long p = flowPulseCount; interrupts();
  return p;
}

void printLive() {
  unsigned long p = snapPulses();
  Serial.print(F("pulses=")); Serial.print(p);
  Serial.print(F("  about ")); Serial.print((float)(p - lastLivePulses) / NANO_K * 60000.0f / LIVE_MS, 2);
  Serial.print(F(" L/min at K=450"));
  Serial.println(windowOpen ? F("  [window OPEN]") : F(""));
  lastLivePulses = p;
}

void listRuns() {
  if (nRuns == 0) { Serial.println(F("No runs yet.")); return; }
  for (uint8_t i = 0; i < nRuns; i++) {
    Serial.print(F("  run ")); Serial.print(i + 1);
    Serial.print(F(": ")); Serial.print(runs[i].pulses);
    Serial.print(F(" pulses, ")); Serial.print(runs[i].liters, 3);
    Serial.print(F(" L, K=")); Serial.println(runs[i].k, 1);
  }
}

void doCalc() {
  if (nRuns < 3) { Serial.print(F("Need at least 3 runs, you have ")); Serial.println(nRuns); return; }
  float ks[MAX_RUNS];
  for (uint8_t i = 0; i < nRuns; i++) ks[i] = runs[i].k;
  for (uint8_t i = 1; i < nRuns; i++) { float key = ks[i]; int8_t j = i - 1; while (j >= 0 && ks[j] > key) { ks[j + 1] = ks[j]; j--; } ks[j + 1] = key; }
  float median = (nRuns % 2) ? ks[nRuns / 2] : (ks[nRuns / 2 - 1] + ks[nRuns / 2]) / 2.0f;

  int worst = -1; float worstDev = 0;
  for (uint8_t i = 0; i < nRuns; i++) {
    float dev = fabs(runs[i].k - median) / median;
    Serial.print(F("  run ")); Serial.print(i + 1); Serial.print(F(": K=")); Serial.print(runs[i].k, 1);
    Serial.print(F(", ")); Serial.print(dev * 100.0f, 1); Serial.print(F(" % from the median"));
    if (dev > OUTLIER_TOL) Serial.print(F("  <-- OUT"));
    Serial.println();
    if (dev > worstDev) { worstDev = dev; worst = i; }
  }
  if (worstDev > OUTLIER_TOL) {
    Serial.print(F("Not averaging bad data. NEXT: type drop ")); Serial.print(worst + 1);
    Serial.println(F(", do that run again, then calc."));
    return;
  }
  float sum = 0;
  for (uint8_t i = 0; i < nRuns; i++) sum += runs[i].k;
  float k = sum / nRuns;
  Serial.println(F("======== RESULT ========"));
  Serial.print(F("K = ")); Serial.print(k, 1); Serial.println(F(" pulses/L. Use ONE of these two, not both:"));
  Serial.println(F("1. Easiest, no Nano reflash. CalStore on ESP1 (flash Calibration/ESP1/CalStore, paste, flash the firmware back):"));
  Serial.print(F("     set frS ")); Serial.println(NANO_K / k, 4);
  Serial.println(F("2. Or both of these, then reflash the Nano and ESP1, and `clear frS` in CalStore if it is stored:"));
  Serial.print(F("     Nano/Nano.ino:     const float FLOW_K_PULSES_PER_LITER = ")); Serial.print(k, 1); Serial.println(F("f;"));
  Serial.print(F("     ESP1/src/main.cpp: const float NANO_FLOW_K = ")); Serial.print(k, 1); Serial.println(F("f;"));
  Serial.println(F("========================"));
}

void handleLine(char *s) {
  char *t1 = strtok(s, " ");
  if (!t1) return;
  if (!strcasecmp(t1, "h") || !strcasecmp(t1, "help")) { printHelp(); return; }
  if (!strcasecmp(t1, "r"))    { lastLivePulses = snapPulses(); Serial.print(F("pulses=")); Serial.println(lastLivePulses); return; }
  if (!strcasecmp(t1, "live")) { liveOn = !liveOn; lastLivePulses = snapPulses(); Serial.println(liveOn ? F("live ON") : F("live OFF")); return; }
  if (!strcasecmp(t1, "runs")) { listRuns(); return; }
  if (!strcasecmp(t1, "calc")) { doCalc(); return; }
  if (!strcasecmp(t1, "clear")) { nRuns = 0; pendingVol = false; Serial.println(F("All runs removed.")); return; }
  if (!strcasecmp(t1, "drop") || !strcasecmp(t1, "x")) {
    char *t2 = strtok(NULL, " ");
    int n = t2 ? atoi(t2) : nRuns;
    if (n < 1 || n > nRuns) { Serial.println(F("usage: drop <run number>")); return; }
    for (int i = n - 1; i < nRuns - 1; i++) runs[i] = runs[i + 1];
    nRuns--;
    Serial.print(F("Run ")); Serial.print(n); Serial.println(F(" removed."));
    return;
  }
  if (!strcasecmp(t1, "start")) {
    noInterrupts(); flowPulseCount = 0; interrupts();
    windowOpen = true; pendingVol = false; windowStartMs = millis(); lastLivePulses = 0;
    Serial.println(F("Window OPEN. Pour your known volume through the sensor, then type stop."));
    return;
  }
  if (!strcasecmp(t1, "stop")) {
    if (!windowOpen) { Serial.println(F("No open window: type start first.")); return; }
    windowPulses = snapPulses();
    windowMs = millis() - windowStartMs;
    windowOpen = false; pendingVol = true;
    Serial.print(F("Window closed. ")); Serial.print(windowPulses); Serial.println(F(" pulses."));
    Serial.println(F("NEXT: vol <litres you measured>"));
    return;
  }
  if (!strcasecmp(t1, "vol")) {
    if (!pendingVol) { Serial.println(F("No finished window: type start, pour, stop.")); return; }
    char *t2 = strtok(NULL, " ");
    float liters = t2 ? atof(t2) : 0.0f;
    if (!(liters > 0.0f)) { Serial.println(F("usage: vol <litres>")); return; }
    if (windowPulses == 0) { Serial.println(F("0 pulses: nothing to record. Check D2, the sensor's power, and the flow direction arrow.")); pendingVol = false; return; }
    if (nRuns >= MAX_RUNS) { Serial.println(F("Six runs is plenty: type calc, or drop one.")); return; }
    float k = (float)windowPulses / liters;
    runs[nRuns].pulses = windowPulses; runs[nRuns].liters = liters; runs[nRuns].k = k;
    nRuns++;
    pendingVol = false;
    Serial.print(F("Run ")); Serial.print(nRuns); Serial.print(F(": K = ")); Serial.print(k, 1); Serial.println(F(" pulses/L"));
    if (windowPulses < MIN_PULSES) Serial.println(F("WARNING: under 100 pulses. Pour more water for a better K."));
    Serial.println(nRuns >= 3 ? F("NEXT: type calc") : F("NEXT: type start for the next run"));
    return;
  }
  Serial.println(F("Unknown command: type h"));
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_FLOW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), flowISR, FLOW_INTERRUPT_EDGE);
  Serial.println();
  Serial.println(F("=== RESERVOIR FLOW K-FACTOR CALIBRATION (Nano) ==="));
  printHelp();
  Serial.println(F("START: type start"));
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) { line[lineLen] = '\0'; lineLen = 0; handleLine(line); }
    } else if (lineLen < LINE_BUF - 1) line[lineLen++] = c;
  }
  if (liveOn && (unsigned long)(millis() - lastLiveMs) >= LIVE_MS) { lastLiveMs = millis(); printLive(); }
}
