/* =============================================================================
 *  SMART IRRIGATION -- CALIBRATION TOOL -- pH PROBE (ESP32 #2)
 * -----------------------------------------------------------------------------
 *  Fits the line the firmware uses: pH = M x adc + B, on GPIO32 (12-bit ADC).
 *
 *  HOW TO USE
 *    1. Probe in pH 7.0 buffer. Wait for `live` to settle.   cap 7.0
 *    2. Rinse. Probe in pH 4.0 buffer. Wait.                 cap 4.0
 *    3. (Optional) rinse, pH 10.0 buffer.                    cap 10.0
 *    4. calc   prints the lines for CalStore.
 *    Also: r, live, pts, drop <n>, x (clear all), h.
 *
 *  The result belongs on ESP1, not in ESP2's source: ESP1 pushes its own values to ESP2 at startup and in
 *  every work order, so an edit to ESP2's PH_CAL_M is overwritten. ESP1's LCD Calibration menu does the
 *  same job on the rig, without flashing, for the two standard buffers.
 * ========================================================================== */
#include <Arduino.h>

const int PIN_PH = 32;                 // ESP2's PIN_PH
const int SAMPLES = 64;
const int MAX_PTS = 8;
const unsigned long LIVE_MS = 1000;
const int ADC_FAULT_LO = 5, ADC_FAULT_HI = 4090;   // ESP2 calls a reading out here a dead probe
const int MIN_SPAN_ADC = 20;           // ESP1's CAL_MIN_SPAN_ADC
const int DRIFT_ADC = 15;              // two bursts a second apart further than this: still settling

double calM = 0.0036621, calB = 0.0;   // ESP1's defaults, for the readout until you fit your own
double px[MAX_PTS], py[MAX_PTS];
int nPts = 0;
bool liveOn = false;
unsigned long lastLiveMs = 0;
String line;

int adcMean() {
  long sum = 0;
  for (int i = 0; i < SAMPLES; i++) sum += analogRead(PIN_PH);
  return (int)(sum / SAMPLES);
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  cap <pH>   capture a point in a known buffer (cap 7.0)");
  Serial.println("  calc       fit the points and print the result");
  Serial.println("  pts        list points      drop <n>  remove one      x  remove all");
  Serial.println("  r / live   reading now, and live on/off");
}

void printRaw() {
  int adc = adcMean();
  Serial.printf("adc=%d  pH=%.2f%s\n", adc, calM * adc + calB,
                (adc <= ADC_FAULT_LO || adc >= ADC_FAULT_HI) ? "  PROBE NOT CONNECTED?" : "");
}

void doCalc() {
  if (nPts < 2) { Serial.println("Need at least 2 points: cap 7.0 and cap 4.0"); return; }
  double sx = 0, sy = 0, sxx = 0, sxy = 0, loX = px[0], hiX = px[0];
  for (int i = 0; i < nPts; i++) {
    sx += px[i]; sy += py[i]; sxx += px[i] * px[i]; sxy += px[i] * py[i];
    if (px[i] < loX) loX = px[i];
    if (px[i] > hiX) hiX = px[i];
  }
  if (hiX - loX < MIN_SPAN_ADC) { Serial.println("REFUSED: the readings barely differ between buffers. Is the probe connected?"); return; }
  double denom = nPts * sxx - sx * sx;
  if (fabs(denom) < 1e-9) { Serial.println("REFUSED: the points are all the same reading. Recapture."); return; }
  double m = (nPts * sxy - sx * sy) / denom;
  double b = (sy - m * sx) / nPts;
  double rms = 0;
  for (int i = 0; i < nPts; i++) { double e = (m * px[i] + b) - py[i]; rms += e * e; }
  rms = sqrt(rms / nPts);
  calM = m; calB = b;
  Serial.println("======== RESULT ========");
  Serial.printf("Fit error %.3f pH over %d points%s\n", rms, nPts, rms > 0.2 ? ": POOR. Check the buffers and let it settle" : "");
  Serial.println("CalStore on ESP1 (flash Calibration/ESP1/CalStore, paste, flash the firmware back):");
  Serial.printf("  set phM %.7f\n  set phB %.4f\n", m, b);
  Serial.println("========================");
}

void handleLine(const String &in) {
  String s = in; s.trim();
  int sp = s.indexOf(' ');
  String cmd = (sp < 0) ? s : s.substring(0, sp);
  String arg = (sp < 0) ? "" : s.substring(sp + 1);
  arg.trim();
  cmd.toLowerCase();

  if (cmd == "h" || cmd == "help") { printHelp(); return; }
  if (cmd == "r")    { printRaw(); return; }
  if (cmd == "live") { liveOn = !liveOn; Serial.println(liveOn ? "live ON" : "live OFF"); return; }
  if (cmd == "calc") { doCalc(); return; }
  if (cmd == "x")    { nPts = 0; Serial.println("All points removed."); return; }
  if (cmd == "pts") {
    if (!nPts) Serial.println("No points yet.");
    for (int i = 0; i < nPts; i++) Serial.printf("  %d: adc=%.0f  pH=%.2f\n", i + 1, px[i], py[i]);
    return;
  }
  if (cmd == "drop") {
    int n = arg.toInt();
    if (n < 1 || n > nPts) { Serial.println("usage: drop <point number>"); return; }
    for (int i = n - 1; i < nPts - 1; i++) { px[i] = px[i + 1]; py[i] = py[i + 1]; }
    nPts--;
    Serial.printf("Point %d removed.\n", n);
    return;
  }
  if (cmd == "cap") {
    char *end = NULL;
    double known = strtod(arg.c_str(), &end);
    if (!arg.length() || *end != '\0' || known < 0.0 || known > 14.0) { Serial.println("usage: cap <pH from 0 to 14>   e.g. cap 7.0"); return; }
    if (nPts >= MAX_PTS) { Serial.println("Eight points is plenty: type calc, or drop one."); return; }
    int a = adcMean();
    delay(1000);
    int adc = adcMean();
    if (adc <= ADC_FAULT_LO || adc >= ADC_FAULT_HI) { Serial.printf("REFUSED: adc=%d is at the rail. Is the probe connected?\n", adc); return; }
    if (abs(adc - a) > DRIFT_ADC) { Serial.printf("REFUSED: still drifting (%d then %d). Wait and try again.\n", a, adc); return; }
    px[nPts] = adc; py[nPts] = known; nPts++;
    Serial.printf("Point %d: adc=%d at pH %.2f\n", nPts, adc, known);
    if (nPts >= 2) Serial.println("NEXT: type calc, or rinse and capture another buffer.");
    else           Serial.println("NEXT: rinse, put the probe in the next buffer, wait, then cap <pH>");
    return;
  }
  Serial.println("Unknown command: type h");
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);            // 0..4095, as in the firmware
  delay(300);
  Serial.println("\n=== pH PROBE CALIBRATION (ESP32 #2) ===");
  printHelp();
  Serial.println("START: probe in pH 7.0 buffer, type live, and when it settles type cap 7.0");
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { if (line.length()) { handleLine(line); line = ""; } }
    else if (line.length() < 64) line += c;
  }
  if (liveOn && millis() - lastLiveMs >= LIVE_MS) { lastLiveMs = millis(); printRaw(); }
}
