/* =============================================================================
 *  SMART IRRIGATION  --  CALIBRATION BENCH TOOL  --  EC PROBE (ESP32 #2)
 * -----------------------------------------------------------------------------
 *  Standalone. No ESP1, no UART. Fits the linear EC calibration the REAL firmware
 *  uses:
 *      value = EC_CAL_M * adc + EC_CAL_B          (ESP2/src/main.cpp:168)
 *  Two-point: dry/air zero + a known standard (e.g. 1.413 mS/cm). Add more points
 *  and this tool least-squares-fits them all (spec sec.A.4).
 *
 *  PIN (match ESP2/src/main.cpp:130): EC analog input GPIO33 (ADC1). 12-bit ADC.
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    r                 raw ADC once + EC at the current fit
 *    live              toggle ~2 Hz live readout
 *    cap <EC>          capture a point in a known standard (e.g. cap 1.413; cap 0 dry)
 *    pts               list captured points
 *    calc              least-squares fit -> prints EC_CAL_M / EC_CAL_B to paste
 *    x                 clear all captured points
 *
 *  Rinse between standards; let it settle before `cap`. EC in mS/cm.
 *  Build: PlatformIO `pio run -e esp32dev` in this folder.
 * ========================================================================== */
#include <Arduino.h>

const int PIN_EC   = 33;          // ESP2/src/main.cpp:130
const int SAMPLES  = 32;
const int MAX_PTS  = 8;
const unsigned long LIVE_MS = 500;

// Defaults from the firmware (ESP2/src/main.cpp:168).
double calM = 0.0009766, calB = 0.0;

double px[MAX_PTS];               // raw ADC (mean)
double py[MAX_PTS];               // known EC (mS/cm)
int    nPts = 0;

bool liveOn = false;
unsigned long lastLiveMs = 0;
String line;

int    adcMean();
void   printHelp();
void   printRaw();
void   listPts();
void   doCalc();
void   handleLine(const String &s);

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  delay(300);
  Serial.println();
  Serial.println("=== EC PROBE CALIBRATION (ESP32 #2) ===");
  printHelp();
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { if (line.length()) { handleLine(line); line = ""; } }
    else if (line.length() < 64) line += c;
  }
  if (liveOn && millis() - lastLiveMs >= LIVE_MS) { lastLiveMs = millis(); printRaw(); }
}

int adcMean() {
  long sum = 0;
  for (int i = 0; i < SAMPLES; i++) sum += analogRead(PIN_EC);
  return (int)(sum / SAMPLES);
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  h          this help");
  Serial.println("  r          raw ADC + EC once");
  Serial.println("  live       toggle ~2 Hz live readout");
  Serial.println("  cap <EC>   capture a point in a known standard (cap 1.413; cap 0 dry)");
  Serial.println("  pts        list captured points");
  Serial.println("  calc       least-squares fit -> EC_CAL_M / EC_CAL_B");
  Serial.println("  x          clear captured points");
}

void printRaw() {
  int adc = adcMean();
  double ec = calM * adc + calB;
  Serial.print("adc="); Serial.print(adc);
  Serial.print("  EC="); Serial.print(ec, 3); Serial.println(" mS/cm  (current fit)");
}

void listPts() {
  if (!nPts) { Serial.println("(no points captured)"); return; }
  for (int i = 0; i < nPts; i++) {
    Serial.print("  pt "); Serial.print(i + 1);
    Serial.print(": adc="); Serial.print(px[i], 0);
    Serial.print(" -> EC="); Serial.println(py[i], 3);
  }
}

void doCalc() {
  if (nPts < 2) { Serial.println("need >=2 points (zero + a standard)."); return; }
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (int i = 0; i < nPts; i++) { sx += px[i]; sy += py[i]; sxx += px[i]*px[i]; sxy += px[i]*py[i]; }
  double denom = nPts * sxx - sx * sx;
  if (fabs(denom) < 1e-9) { Serial.println("degenerate points (same ADC?) -- recapture."); return; }
  double m = (nPts * sxy - sx * sy) / denom;
  double b = (sy - m * sx) / nPts;
  calM = m; calB = b;

  double rms = 0;
  for (int i = 0; i < nPts; i++) { double e = (m * px[i] + b) - py[i]; rms += e * e; }
  rms = sqrt(rms / nPts);

  Serial.println("---- fit ----");
  Serial.print("EC_CAL_M = "); Serial.println(m, 8);
  Serial.print("EC_CAL_B = "); Serial.println(b, 6);
  Serial.print("RMS EC error = "); Serial.print(rms, 4); Serial.println(" mS/cm");
  Serial.println("// ESP2/src/main.cpp:168 -- paste:");
  Serial.print("float EC_CAL_M = "); Serial.print(m, 8); Serial.print("f, EC_CAL_B = "); Serial.print(b, 6); Serial.println("f;");
}

void handleLine(const String &in) {
  String s = in; s.trim();
  int sp = s.indexOf(' ');
  String cmd = (sp < 0) ? s : s.substring(0, sp);
  String arg = (sp < 0) ? "" : s.substring(sp + 1);
  cmd.toLowerCase();

  if (cmd == "h")    { printHelp(); return; }
  if (cmd == "r")    { printRaw();  return; }
  if (cmd == "live") { liveOn = !liveOn; Serial.print("live="); Serial.println(liveOn ? "ON" : "OFF"); return; }
  if (cmd == "pts")  { listPts(); return; }
  if (cmd == "calc") { doCalc(); return; }
  if (cmd == "x")    { nPts = 0; Serial.println("points cleared"); return; }
  if (cmd == "cap") {
    if (arg.length() == 0) { Serial.println("usage: cap <EC>  (e.g. cap 1.413)"); return; }
    if (nPts >= MAX_PTS) { Serial.println("point buffer full -- `x` to clear"); return; }
    double known = arg.toFloat();
    int adc = adcMean();
    px[nPts] = adc; py[nPts] = known; nPts++;
    Serial.print("captured pt "); Serial.print(nPts);
    Serial.print(": adc="); Serial.print(adc); Serial.print(" @ EC="); Serial.println(known, 3);
    if (nPts >= 2) Serial.println("have >=2 points -- type `calc`");
    return;
  }
  Serial.println("? unknown command -- type h");
}
