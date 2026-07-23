/* =============================================================================
 *  SMART IRRIGATION  --  CALIBRATION BENCH TOOL  --  pH PROBE (ESP32 #2)
 * -----------------------------------------------------------------------------
 *  Standalone. No ESP1, no UART, no work orders -- just a human over USB serial.
 *  Fits the linear pH calibration the REAL firmware uses:
 *      value = PH_CAL_M * adc + PH_CAL_B          (ESP2/src/main.cpp:167)
 *  Two-point (buffers pH 7.0 then 4.0) is the default; add a 3rd (pH 10.0) for the
 *  high range and this tool least-squares-fits all captured points (spec sec.A.4).
 *
 *  PIN (match ESP2/src/main.cpp:129): pH analog input GPIO32 (ADC1). 12-bit ADC.
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    r                 raw ADC once + pH at the current fit
 *    live              toggle ~2 Hz live readout
 *    cap <pH>          capture a point: submerge in the known buffer, type its pH
 *    pts               list captured points
 *    calc              least-squares fit -> prints PH_CAL_M / PH_CAL_B to paste
 *    x                 clear all captured points
 *
 *  Rinse the probe between buffers; let the reading settle before `cap`.
 *  Build: PlatformIO `pio run -e esp32dev` in this folder.
 * ========================================================================== */
#include <Arduino.h>

const int PIN_PH   = 32;          // ESP2/src/main.cpp:129
const int SAMPLES  = 32;          // per-capture average
const int MAX_PTS  = 8;
const unsigned long LIVE_MS = 500;

// Defaults from the firmware (ESP2/src/main.cpp:167) -- used for the live readout
// until you compute a fresh fit.
double calM = 0.0036621, calB = 0.0;

double px[MAX_PTS];               // captured raw ADC (mean)
double py[MAX_PTS];               // captured known pH
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
  analogReadResolution(12);       // 0..4095, matches the firmware
  delay(300);
  Serial.println();
  Serial.println("=== pH PROBE CALIBRATION (ESP32 #2) ===");
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
  for (int i = 0; i < SAMPLES; i++) sum += analogRead(PIN_PH);
  return (int)(sum / SAMPLES);
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  h          this help");
  Serial.println("  r          raw ADC + pH once");
  Serial.println("  live       toggle ~2 Hz live readout");
  Serial.println("  cap <pH>   capture a point in a known buffer (e.g. cap 7.0)");
  Serial.println("  pts        list captured points");
  Serial.println("  calc       least-squares fit -> PH_CAL_M / PH_CAL_B");
  Serial.println("  x          clear captured points");
}

void printRaw() {
  int adc = adcMean();
  double ph = calM * adc + calB;
  Serial.print("adc="); Serial.print(adc);
  Serial.print("  pH="); Serial.print(ph, 2);
  Serial.println("  (current fit)");
}

void listPts() {
  if (!nPts) { Serial.println("(no points captured)"); return; }
  for (int i = 0; i < nPts; i++) {
    Serial.print("  pt "); Serial.print(i + 1);
    Serial.print(": adc="); Serial.print(px[i], 0);
    Serial.print(" -> pH="); Serial.println(py[i], 2);
  }
}

// Linear least-squares fit y = M*x + B over the captured points.
void doCalc() {
  if (nPts < 2) { Serial.println("need >=2 points (2 buffers)."); return; }
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (int i = 0; i < nPts; i++) { sx += px[i]; sy += py[i]; sxx += px[i]*px[i]; sxy += px[i]*py[i]; }
  double denom = nPts * sxx - sx * sx;
  if (fabs(denom) < 1e-9) { Serial.println("degenerate points (same ADC?) -- recapture."); return; }
  double m = (nPts * sxy - sx * sy) / denom;
  double b = (sy - m * sx) / nPts;
  calM = m; calB = b;                     // adopt for the live readout

  // residuals
  double rms = 0;
  for (int i = 0; i < nPts; i++) { double e = (m * px[i] + b) - py[i]; rms += e * e; }
  rms = sqrt(rms / nPts);

  Serial.println("---- fit ----");
  Serial.print("PH_CAL_M = "); Serial.println(m, 8);
  Serial.print("PH_CAL_B = "); Serial.println(b, 6);
  Serial.print("RMS pH error = "); Serial.println(rms, 3);
  Serial.println("// ESP2/src/main.cpp:167 -- paste:");
  Serial.print("float PH_CAL_M = "); Serial.print(m, 8); Serial.print("f, PH_CAL_B = "); Serial.print(b, 6); Serial.println("f;");
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
    if (arg.length() == 0) { Serial.println("usage: cap <pH>  (e.g. cap 7.0)"); return; }
    if (nPts >= MAX_PTS) { Serial.println("point buffer full -- `x` to clear"); return; }
    double known = arg.toFloat();
    int adc = adcMean();
    px[nPts] = adc; py[nPts] = known; nPts++;
    Serial.print("captured pt "); Serial.print(nPts);
    Serial.print(": adc="); Serial.print(adc); Serial.print(" @ pH="); Serial.println(known, 2);
    if (nPts >= 2) Serial.println("have >=2 points -- type `calc`");
    return;
  }
  Serial.println("? unknown command -- type h");
}
