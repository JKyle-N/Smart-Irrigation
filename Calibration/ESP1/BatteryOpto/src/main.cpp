/* =============================================================================
 *  SMART IRRIGATION -- CALIBRATION TOOL -- OPTO BATTERY VOLTAGE (ESP32 #1)
 * -----------------------------------------------------------------------------
 *  The main battery's FALLBACK voltage: ESP1 uses it only while the main INA226 is not answering. An opto in
 *  front of the ADC is not linear, so this fits a curve: volts = a0 + a1*u + a2*u^2 + a3*u^3, u = raw/4095.
 *
 *  HOW TO USE (flash to ESP1, GPIO35). Use a bench supply on the battery input if you can.
 *    1. Set a known voltage and read it on a multimeter.   v 12.50
 *    2. Repeat 6-8 times, across the WHOLE range you care about (say 10.5 V to 14.5 V), including both ends.
 *    3. calc     fits the curve, checks it, and saves it for the firmware.
 *    Also: r, live, list, drop <n>, x (clear all), d <1-3> (degree), show (what is saved), wipe, h.
 *
 *  The result is stored in NVS namespace "adccal", key "v". Flash the main firmware back and it loads it.
 *  The old GPIO34 current channel is gone: the firmware stopped reading it, so calibrating it did nothing.
 * ========================================================================== */
#include <Arduino.h>
#include <Preferences.h>

const int PIN_V = 35;                  // ESP1's PIN_BATT_V
const int SAMPLES = 64, TRIM = 8;      // trimmed mean, same as ESP1's readAdcTrimmed
const int MAX_PTS = 16, MAX_COEF = 4;
const unsigned long LIVE_MS = 1000;
const double ADC_RAIL_LO = 20, ADC_RAIL_HI = 4080;   // the opto or the ADC has run out of range
const double MIN_SPACING_ADC = 8;      // two points this close only repeat each other
const double BATT_V_MIN = 1.0, BATT_V_MAX = 40.0;

double px[MAX_PTS], py[MAX_PTS];
int nPts = 0, degree = 3;
double coef[MAX_COEF] = { 0, 1, 0, 0 };
int fitDegree = 1;
bool haveFit = false;

Preferences prefs;
bool liveOn = false;
unsigned long lastLiveMs = 0;
String line;

struct CalRec { uint32_t magic; int32_t degree; double coef[MAX_COEF]; };   // ESP1's loadBattCal reads this layout
const uint32_t CAL_MAGIC = 0xADC0CA11;

double readAdcTrimmed() {
  static uint16_t buf[SAMPLES];
  for (int i = 0; i < SAMPLES; i++) { buf[i] = analogRead(PIN_V); delayMicroseconds(150); }
  for (int i = 1; i < SAMPLES; i++) { uint16_t k = buf[i]; int j = i - 1; while (j >= 0 && buf[j] > k) { buf[j + 1] = buf[j]; j--; } buf[j + 1] = k; }
  long sum = 0; int cnt = 0;
  for (int i = TRIM; i < SAMPLES - TRIM; i++) { sum += buf[i]; cnt++; }
  return cnt ? (double)sum / cnt : 0.0;
}

double applyPoly(double raw) {
  double u = raw / 4095.0, val = 0, up = 1;
  for (int k = 0; k <= fitDegree; k++) { val += coef[k] * up; up *= u; }
  return val;
}

bool solveLinear(double A[MAX_COEF][MAX_COEF], double b[MAX_COEF], int n, double x[MAX_COEF]) {
  for (int col = 0; col < n; col++) {
    int piv = col; double best = fabs(A[col][col]);
    for (int r = col + 1; r < n; r++) if (fabs(A[r][col]) > best) { best = fabs(A[r][col]); piv = r; }
    if (best < 1e-12) return false;
    if (piv != col) {
      for (int k = 0; k < n; k++) { double t = A[col][k]; A[col][k] = A[piv][k]; A[piv][k] = t; }
      double t = b[col]; b[col] = b[piv]; b[piv] = t;
    }
    for (int r = 0; r < n; r++) {
      if (r == col) continue;
      double f = A[r][col] / A[col][col];
      for (int k = col; k < n; k++) A[r][k] -= f * A[col][k];
      b[r] -= f * b[col];
    }
  }
  for (int i = 0; i < n; i++) x[i] = b[i] / A[i][i];
  return true;
}

void doCalc() {
  int D = degree, m = D + 1;
  if (nPts < m + 1) { Serial.printf("Need at least %d points for degree %d, you have %d. A curve through exactly as many points as it has terms proves nothing.\n", m + 1, D, nPts); return; }
  double A[MAX_COEF][MAX_COEF] = {{0}}, b[MAX_COEF] = {0}, x[MAX_COEF] = {0};
  for (int p = 0; p < nPts; p++) {
    double u = px[p] / 4095.0, up[7]; up[0] = 1;
    for (int k = 1; k <= 2 * D; k++) up[k] = up[k - 1] * u;
    for (int i = 0; i <= D; i++) { for (int j = 0; j <= D; j++) A[i][j] += up[i + j]; b[i] += py[p] * up[i]; }
  }
  if (!solveLinear(A, b, m, x)) { Serial.println("REFUSED: the points do not pin down a curve. Spread them across the range."); return; }
  for (int k = 0; k < MAX_COEF; k++) coef[k] = (k <= D) ? x[k] : 0.0;
  fitDegree = D;

  double se = 0, mx = 0;
  for (int p = 0; p < nPts; p++) { double e = applyPoly(px[p]) - py[p]; se += e * e; if (fabs(e) > mx) mx = fabs(e); }
  double rms = sqrt(se / nPts);

  // A cubic can wiggle between points. Walk the captured span: the curve must keep rising with the input.
  double loX = px[0], hiX = px[0];
  for (int i = 1; i < nPts; i++) { if (px[i] < loX) loX = px[i]; if (px[i] > hiX) hiX = px[i]; }
  bool up = applyPoly(hiX) > applyPoly(loX), mono = true;
  double prev = applyPoly(loX);
  for (int i = 1; i <= 40; i++) {
    double v = applyPoly(loX + (hiX - loX) * i / 40.0);
    if (up ? v < prev - 0.001 : v > prev + 0.001) mono = false;
    prev = v;
  }

  Serial.println("======== RESULT ========");
  Serial.printf("Degree %d: error %.3f V rms, %.3f V at worst\n", D, rms, mx);
  if (!mono) {
    Serial.println("REFUSED: the curve doubles back between your points, so one reading could mean two voltages.");
    Serial.println("NEXT: add points in the gaps, or try a lower degree: d 2");
    haveFit = false;
    return;
  }
  if (mx > 0.5) Serial.println("WARNING: over 0.5 V of error. Recheck the points (list) and drop any bad one.");
  CalRec r; r.magic = CAL_MAGIC; r.degree = D;
  for (int k = 0; k < MAX_COEF; k++) r.coef[k] = coef[k];
  haveFit = true;
  if (prefs.putBytes("v", &r, sizeof(r)) != sizeof(r)) { Serial.println("FAILED: could not save to NVS."); return; }
  Serial.println("Saved to NVS. Flash ESP1's main firmware back and it loads this.");
  Serial.println("Or paste into ESP1/src/main.cpp:");
  Serial.printf("  AdcCal calBattV = { %d, { %.8g, %.8g, %.8g, %.8g } };\n", D, coef[0], coef[1], coef[2], coef[3]);
  Serial.println("========================");
}

void cmdShow() {
  CalRec r;
  if (prefs.getBytes("v", &r, sizeof(r)) != sizeof(r) || r.magic != CAL_MAGIC) { Serial.println("Nothing saved in NVS."); return; }
  Serial.printf("Saved: degree %d, a0=%.8g a1=%.8g a2=%.8g a3=%.8g\n", (int)r.degree, r.coef[0], r.coef[1], r.coef[2], r.coef[3]);
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  v <volts>   capture a point at a known voltage");
  Serial.println("  calc        fit the curve, check it and save it");
  Serial.println("  list        points    drop <n>  remove one    x  remove all");
  Serial.println("  d <1-3>     curve degree (3 by default)");
  Serial.println("  r / live    reading now, and live on/off");
  Serial.println("  show        what is saved    wipe  forget it");
}

void handleLine(const String &in) {
  String s = in; s.trim();
  int sp = s.indexOf(' ');
  String cmd = (sp < 0) ? s : s.substring(0, sp);
  String arg = (sp < 0) ? "" : s.substring(sp + 1);
  arg.trim();
  cmd.toLowerCase();

  if (cmd == "h" || cmd == "help") { printHelp(); return; }
  if (cmd == "r") {
    double raw = readAdcTrimmed();
    Serial.printf("adc=%.1f", raw);
    if (haveFit) Serial.printf("  V=%.2f", applyPoly(raw));
    Serial.println();
    return;
  }
  if (cmd == "live") { liveOn = !liveOn; Serial.println(liveOn ? "live ON" : "live OFF"); return; }
  if (cmd == "calc") { doCalc(); return; }
  if (cmd == "show") { cmdShow(); return; }
  if (cmd == "wipe") { prefs.remove("v"); Serial.println("Forgotten. The firmware goes back to its compiled calBattV."); return; }
  if (cmd == "x")    { nPts = 0; Serial.println("All points removed."); return; }
  if (cmd == "list") {
    if (!nPts) Serial.println("No points yet.");
    for (int i = 0; i < nPts; i++) Serial.printf("  %d: adc=%.1f  %.3f V\n", i + 1, px[i], py[i]);
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
  if (cmd == "d") {
    int d = arg.toInt();
    if (d < 1 || d > 3) { Serial.println("usage: d <1, 2 or 3>"); return; }
    degree = d;
    Serial.printf("Degree %d. Type calc to refit.\n", d);
    return;
  }
  if (cmd == "v") {
    char *end = NULL;
    double volts = strtod(arg.c_str(), &end);
    if (!arg.length() || *end != '\0' || volts < BATT_V_MIN || volts > BATT_V_MAX) { Serial.println("usage: v <volts from your meter>   e.g. v 12.50"); return; }
    if (nPts >= MAX_PTS) { Serial.println("Sixteen points is plenty: type calc, or drop one."); return; }
    double raw = readAdcTrimmed();
    if (raw <= ADC_RAIL_LO || raw >= ADC_RAIL_HI) { Serial.printf("REFUSED: adc=%.0f is at the end of the ADC's range: this voltage cannot be read through the opto.\n", raw); return; }
    for (int i = 0; i < nPts; i++) {
      if (fabs(px[i] - raw) < MIN_SPACING_ADC && fabs(py[i] - volts) > 0.05) {
        Serial.printf("REFUSED: point %d read almost the same (adc=%.1f) at %.3f V. Check the meter and the wiring.\n", i + 1, px[i], py[i]);
        return;
      }
    }
    px[nPts] = raw; py[nPts] = volts; nPts++;
    Serial.printf("Point %d: adc=%.1f at %.3f V\n", nPts, raw, volts);
    if (nPts >= 6) Serial.println("NEXT: type calc, or add more points.");
    else Serial.printf("NEXT: change the voltage and capture again (%d more for a good curve).\n", 6 - nPts);
    return;
  }
  Serial.println("Unknown command: type h");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_V, ADC_11db);   // same as ESP1
  prefs.begin("adccal", false);
  Serial.println("\n=== OPTO BATTERY VOLTAGE CALIBRATION (ESP32 #1, fallback sensor) ===");
  printHelp();
  cmdShow();
  Serial.println("START: set a known voltage, then type v <volts>");
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { if (line.length()) { handleLine(line); line = ""; } }
    else if (line.length() < 48) line += c;
  }
  if (liveOn && millis() - lastLiveMs >= LIVE_MS) {
    lastLiveMs = millis();
    double raw = readAdcTrimmed();
    Serial.printf("adc=%.1f", raw);
    if (haveFit) Serial.printf("  V=%.2f", applyPoly(raw));
    Serial.println();
  }
}
