/* =============================================================================
 *  SMART IRRIGATION -- CALIBRATION TOOL -- EC PROBE (ESP32 #2)
 * -----------------------------------------------------------------------------
 *  Fits the line the firmware uses: EC = M x adc + B in mS/cm, on GPIO33 (12-bit ADC).
 *
 *  HOW TO USE
 *    1. Probe dry and clean, in air. Wait for `live` to settle.   cap 0
 *    2. Probe in the standard. Wait.                              cap 1.413   (use the number on YOUR bottle)
 *    3. calc   prints the lines for CalStore.
 *    Also: r, live, pts, drop <n>, x (clear all), h.
 *
 *  The result belongs on ESP1, not in ESP2's source: ESP1 pushes its own values to ESP2 at startup and in
 *  every work order, so an edit to ESP2's EC_CAL_M is overwritten. ESP1's LCD Calibration menu does the same
 *  job on the rig, without flashing, against its EC_STD_MSCM.
 *
 *  A dry probe reads close to 0, and ESP2 treats an ADC at or below 5 as a dead probe. So only the zero point
 *  may sit at the bottom rail; a standard reading down there is refused.
 * ========================================================================== */
#include <Arduino.h>

const int PIN_EC = 33;                 // ESP2's PIN_EC
const int SAMPLES = 64;
const int MAX_PTS = 8;
const unsigned long LIVE_MS = 1000;
const int ADC_FAULT_LO = 5, ADC_FAULT_HI = 4090;   // ESP2's limits
const int MIN_SPAN_ADC = 20;           // ESP1's CAL_MIN_SPAN_ADC
const int DRIFT_ADC = 15;              // two bursts a second apart further than this: still settling

double calM = 0.0009766, calB = 0.0;   // ESP1's defaults, for the readout until you fit your own
double px[MAX_PTS], py[MAX_PTS];
int nPts = 0;
bool liveOn = false;
unsigned long lastLiveMs = 0;
String line;

int adcMean() {
  long sum = 0;
  for (int i = 0; i < SAMPLES; i++) sum += analogRead(PIN_EC);
  return (int)(sum / SAMPLES);
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  cap <EC>   capture a point: cap 0 dry, cap 1.413 in the standard (mS/cm)");
  Serial.println("  calc       fit the points and print the result");
  Serial.println("  pts        list points      drop <n>  remove one      x  remove all");
  Serial.println("  r / live   reading now, and live on/off");
}

void printRaw() {
  int adc = adcMean();
  Serial.printf("adc=%d  EC=%.3f mS/cm%s\n", adc, calM * adc + calB, adc >= ADC_FAULT_HI ? "  AT THE TOP RAIL" : "");
}

void doCalc() {
  if (nPts < 2) { Serial.println("Need at least 2 points: cap 0 and cap 1.413"); return; }
  double sx = 0, sy = 0, sxx = 0, sxy = 0, loX = px[0], hiX = px[0];
  for (int i = 0; i < nPts; i++) {
    sx += px[i]; sy += py[i]; sxx += px[i] * px[i]; sxy += px[i] * py[i];
    if (px[i] < loX) loX = px[i];
    if (px[i] > hiX) hiX = px[i];
  }
  if (hiX - loX < MIN_SPAN_ADC) { Serial.println("REFUSED: the readings barely differ. Is the probe connected?"); return; }
  double denom = nPts * sxx - sx * sx;
  if (fabs(denom) < 1e-9) { Serial.println("REFUSED: the points are all the same reading. Recapture."); return; }
  double m = (nPts * sxy - sx * sy) / denom;
  double b = (sy - m * sx) / nPts;
  double rms = 0;
  for (int i = 0; i < nPts; i++) { double e = (m * px[i] + b) - py[i]; rms += e * e; }
  rms = sqrt(rms / nPts);
  calM = m; calB = b;
  Serial.println("======== RESULT ========");
  Serial.printf("Fit error %.4f mS/cm over %d points\n", rms, nPts);
  Serial.println("CalStore on ESP1 (flash Calibration/ESP1/CalStore, paste, flash the firmware back):");
  Serial.printf("  set ecM %.7f\n  set ecB %.4f\n", m, b);
  Serial.println("If the standard is not 1.413, also change EC_STD_MSCM in ESP1/src/main.cpp for the LCD's calibration.");
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
    for (int i = 0; i < nPts; i++) Serial.printf("  %d: adc=%.0f  EC=%.3f\n", i + 1, px[i], py[i]);
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
    if (!arg.length() || *end != '\0' || known < 0.0 || known > 20.0) { Serial.println("usage: cap <mS/cm from 0 to 20>   e.g. cap 1.413"); return; }
    if (nPts >= MAX_PTS) { Serial.println("Eight points is plenty: type calc, or drop one."); return; }
    int a = adcMean();
    delay(1000);
    int adc = adcMean();
    if (adc >= ADC_FAULT_HI) { Serial.printf("REFUSED: adc=%d is at the top rail. Check the probe's wiring and range.\n", adc); return; }
    if (known > 0.0 && adc <= ADC_FAULT_LO) { Serial.printf("REFUSED: adc=%d in a real solution means no signal. Is the probe connected?\n", adc); return; }
    if (abs(adc - a) > DRIFT_ADC) { Serial.printf("REFUSED: still drifting (%d then %d). Wait and try again.\n", a, adc); return; }
    px[nPts] = adc; py[nPts] = known; nPts++;
    Serial.printf("Point %d: adc=%d at %.3f mS/cm\n", nPts, adc, known);
    if (nPts >= 2) Serial.println("NEXT: type calc, or rinse and capture another standard.");
    else           Serial.println("NEXT: put the probe in the standard, wait, then cap <the number on the bottle>");
    return;
  }
  Serial.println("Unknown command: type h");
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  delay(300);
  Serial.println("\n=== EC PROBE CALIBRATION (ESP32 #2) ===");
  printHelp();
  Serial.println("START: probe dry and clean, in air. Type cap 0");
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { if (line.length()) { handleLine(line); line = ""; } }
    else if (line.length() < 64) line += c;
  }
  if (liveOn && millis() - lastLiveMs >= LIVE_MS) { lastLiveMs = millis(); printRaw(); }
}
