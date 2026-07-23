/* =============================================================================
 *  SMART IRRIGATION  --  CALIBRATION BENCH TOOL  --  RESERVOIR FLOW (Arduino Nano)
 * -----------------------------------------------------------------------------
 *  Standalone. No ESP1, no UART. Measures the reservoir flow sensor K-factor
 *  (pulses per LITER) for Nano/Nano.ino's FLOW_K_PULSES_PER_LITER. Flow is a
 *  single-factor, zero-offset calibration (spec sec.A.4.1): K = pulses / liters,
 *  measured 3x and averaged with a median outlier check.
 *
 *  The Nano is sensor-only (no pump), so YOU move the water by hand: `start` opens
 *  a counting window, run a known volume through the sensor, `stop`, then enter the
 *  measured volume with `vol`. Repeat 3x.
 *
 *  PIN (match Nano/Nano.ino:78,137-139): reservoir flow D2 (INT0), edge RISING,
 *  INPUT_PULLUP. Same wiring as Test Code/ArduinoNano/Flowsensor.
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    r                 print live pulse count + L/min at the current K
 *    start             zero the counter and begin a counting window
 *    stop              end the window, freeze the pulse count
 *    vol <liters>      enter the measured volume -> stores this run's K
 *    runs              list captured runs (pulses, liters, K)
 *    calc              median/outlier check over the runs -> final K to paste
 *    x                 drop the last run     clear   drop all runs
 *
 *  Build: Arduino IDE (Arduino Nano, ATmega328P) or `pio run` in this folder.
 * ========================================================================== */
#include <Arduino.h>

const uint8_t PIN_FLOW = 2;                 // D2, INT0
#define FLOW_INTERRUPT_EDGE  RISING
float  K_DEFAULT = 450.0f;                  // current firmware placeholder, for the live L/min readout
const float OUTLIER_TOL = 0.08f;            // +-8% from the median flags a run (spec sec.A.4.1.7)
const uint8_t MAX_RUNS = 6;

volatile unsigned long flowPulseCount = 0;

bool          windowOpen = false;
unsigned long windowPulses = 0;             // frozen at stop
unsigned long lastFlowMs = 0;

struct Run { unsigned long pulses; float liters; float k; };
Run   runs[MAX_RUNS];
uint8_t nRuns = 0;
bool  pendingVol = false;                    // a stopped run awaiting its volume

bool          liveOn = false;
unsigned long lastLiveMs = 0;
const unsigned long LIVE_MS = 1000;

#define LINE_BUF 48
char    line[LINE_BUF];
uint8_t lineLen = 0;

void flowISR() { flowPulseCount++; }

void printHelp();
void printLive();
void listRuns();
void doCalc();
void handleLine(char *s);

void setup() {
  Serial.begin(115200);
  pinMode(PIN_FLOW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), flowISR, FLOW_INTERRUPT_EDGE);
  lastFlowMs = millis();
  Serial.println();
  Serial.println(F("=== RESERVOIR FLOW K-FACTOR CALIBRATION (Nano) ==="));
  printHelp();
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) { line[lineLen] = '\0'; handleLine(line); lineLen = 0; }
    } else if (lineLen < LINE_BUF - 1) {
      line[lineLen++] = c;
    } else {
      lineLen = 0;
    }
  }
  if (liveOn && (unsigned long)(millis() - lastLiveMs) >= LIVE_MS) {
    lastLiveMs = millis();
    printLive();
  }
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  h              this help"));
  Serial.println(F("  r / live       pulse count + L/min at current K (live toggles)"));
  Serial.println(F("  start          zero counter + begin a counting window"));
  Serial.println(F("  stop           end window, freeze pulses"));
  Serial.println(F("  vol <liters>   enter measured volume -> store run K"));
  Serial.println(F("  runs           list captured runs"));
  Serial.println(F("  calc           median/outlier check -> final K"));
  Serial.println(F("  x / clear      drop last run / drop all runs"));
}

// Snapshot the pulse count without disturbing the running total.
unsigned long snapPulses() {
  noInterrupts(); unsigned long p = flowPulseCount; interrupts();
  return p;
}

void printLive() {
  unsigned long p = snapPulses();
  Serial.print(F("pulses=")); Serial.print(p);
  Serial.print(windowOpen ? F("  [window OPEN]") : F("  [idle]"));
  Serial.println();
}

void listRuns() {
  if (nRuns == 0) { Serial.println(F("(no runs yet)")); return; }
  for (uint8_t i = 0; i < nRuns; i++) {
    Serial.print(F("run ")); Serial.print(i + 1);
    Serial.print(F(": pulses=")); Serial.print(runs[i].pulses);
    Serial.print(F(" liters=")); Serial.print(runs[i].liters, 3);
    Serial.print(F(" K=")); Serial.println(runs[i].k, 1);
  }
}

void doCalc() {
  if (nRuns < 3) { Serial.print(F("need >=3 runs (have ")); Serial.print(nRuns); Serial.println(F(") -- spec sec.A.4.1.6")); return; }
  // median of the K values
  float ks[MAX_RUNS];
  for (uint8_t i = 0; i < nRuns; i++) ks[i] = runs[i].k;
  for (uint8_t i = 1; i < nRuns; i++) { float key = ks[i]; int8_t j = i - 1; while (j >= 0 && ks[j] > key) { ks[j+1] = ks[j]; j--; } ks[j+1] = key; }
  float median = ks[nRuns / 2];

  // flag outliers vs median
  int worst = -1; float worstDev = 0;
  Serial.println(F("---- calc ----"));
  Serial.print(F("median K = ")); Serial.println(median, 1);
  for (uint8_t i = 0; i < nRuns; i++) {
    float dev = fabs(runs[i].k - median) / median;
    Serial.print(F("  run ")); Serial.print(i + 1); Serial.print(F(" K=")); Serial.print(runs[i].k, 1);
    Serial.print(F("  dev=")); Serial.print(dev * 100.0f, 1); Serial.print('%');
    if (dev > OUTLIER_TOL) Serial.print(F("  <-- OUTLIER"));
    Serial.println();
    if (dev > worstDev) { worstDev = dev; worst = i; }
  }
  if (worstDev > OUTLIER_TOL) {
    Serial.print(F("Run ")); Serial.print(worst + 1);
    Serial.println(F(" disagrees > 8%. Redo it (drop with `x`? no -- use `clear` and re-run the worst),"));
    Serial.println(F("then `calc` again. Not averaging bad data (spec sec.A.4.1.7)."));
    return;
  }
  float sum = 0; for (uint8_t i = 0; i < nRuns; i++) sum += runs[i].k;
  float avg = sum / nRuns;
  Serial.print(F("All runs within +-8%. FINAL K = ")); Serial.println(avg, 1);
  Serial.print(F("// Nano.ino:  const float FLOW_K_PULSES_PER_LITER = "));
  Serial.print(avg, 1); Serial.println(F("f;"));
}

void handleLine(char *s) {
  char *t1 = strtok(s, " ");
  if (!t1) return;

  if (!strcasecmp(t1, "h"))    { printHelp(); return; }
  if (!strcasecmp(t1, "r"))    { printLive(); return; }
  if (!strcasecmp(t1, "live")) { liveOn = !liveOn; Serial.print(F("live=")); Serial.println(liveOn ? F("ON") : F("OFF")); return; }
  if (!strcasecmp(t1, "runs")) { listRuns(); return; }
  if (!strcasecmp(t1, "calc")) { doCalc(); return; }
  if (!strcasecmp(t1, "clear")) { nRuns = 0; pendingVol = false; Serial.println(F("all runs cleared")); return; }
  if (!strcasecmp(t1, "x")) { if (nRuns > 0) { nRuns--; Serial.println(F("last run dropped")); } else Serial.println(F("no runs")); return; }

  if (!strcasecmp(t1, "start")) {
    noInterrupts(); flowPulseCount = 0; interrupts();
    windowOpen = true; pendingVol = false;
    Serial.println(F("window OPEN -- run your known volume of water now, then `stop`."));
    return;
  }
  if (!strcasecmp(t1, "stop")) {
    if (!windowOpen) { Serial.println(F("no open window -- `start` first")); return; }
    windowPulses = snapPulses();
    windowOpen = false; pendingVol = true;
    Serial.print(F("window CLOSED -- pulses = ")); Serial.println(windowPulses);
    Serial.println(F("enter measured volume: vol <liters>"));
    return;
  }
  if (!strcasecmp(t1, "vol")) {
    if (!pendingVol) { Serial.println(F("no stopped run -- do start/stop first")); return; }
    char *t2 = strtok(NULL, " ");
    if (!t2) { Serial.println(F("usage: vol <liters>")); return; }
    float liters = atof(t2);
    if (liters <= 0.0f) { Serial.println(F("liters must be > 0")); return; }
    if (nRuns >= MAX_RUNS) { Serial.println(F("run buffer full -- `clear` first")); return; }
    float k = (float)windowPulses / liters;
    runs[nRuns].pulses = windowPulses; runs[nRuns].liters = liters; runs[nRuns].k = k;
    nRuns++;
    pendingVol = false;
    Serial.print(F("run ")); Serial.print(nRuns);
    Serial.print(F(" K = ")); Serial.print(k, 1); Serial.println(F(" pulses/L"));
    if (nRuns >= 3) Serial.println(F("have >=3 runs -- type `calc`"));
    return;
  }
  Serial.println(F("? unknown command -- type h"));
}
