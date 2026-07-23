/* =============================================================================
 *  ESP1 NONLINEAR ADC CALIBRATION  --  opto-isolated current + voltage
 * -----------------------------------------------------------------------------
 *  Standalone bench tool (no main firmware / no UART / no other controllers).
 *  An optocoupler in front of the ESP32 ADC makes raw counts -> real-world units
 *  NONLINEAR (ESP32 ADC curve + opto CTR/saturation). This tool captures known
 *  (applied) reference points interactively and fits a least-squares POLYNOMIAL
 *  per channel -- capturing the whole chain end-to-end -- then prints it and
 *  SAVES it to NVS so the unit boots calibrated.
 *
 *  WIRING (ESP32 #1, ADC1 input-only pins):
 *    GPIO34 = CURRENT      GPIO35 = VOLTAGE
 *    Keep the ADC-side signal 0..3.3 V. ADC_11db reads ~0..3.1 V and CLIPS above,
 *    so scale the opto output to stay in range. Common ground on the ADC side.
 *
 *  MODEL:  value = a0 + a1*u + a2*u^2 + a3*u^3 ,  u = raw/4095   (degree 2 or 3)
 *          (u is normalized so the degree-3 normal-equations matrix stays sane.)
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    r                 print raw counts once
 *    i <value>         capture a CURRENT point: apply a known input, type its true value
 *    v <value>         capture a VOLTAGE point
 *    l                 list captured points
 *    x [i|v]           clear points (both, or one channel)
 *    d <2|3>           set polynomial degree (default 3)
 *    c                 compute the fit for both channels, print coeffs + error, SAVE to NVS
 *    s                 show the stored NVS calibration
 *    w                 wipe the stored NVS calibration
 *    live              toggle the ~2 Hz live readout on/off
 *
 *  CALIBRATION TIP: capture 6-8 points spread across the WHOLE range, including
 *  near both ends, for a good cubic fit. Watch the printed RMS / max error.
 * ========================================================================== */
#include <Arduino.h>
#include <Preferences.h>

/* ---- config ---- */
const int PIN_I = 34;                 // current channel (ADC1)
const int PIN_V = 35;                 // voltage channel (ADC1)
const int   ADC_MAX     = 4095;       // 12-bit
const int   SAMPLES     = 64;         // samples per reading
const int   TRIM        = 8;          // drop this many from each end (trimmed mean)
const int   MAX_PTS     = 16;         // captured points per channel
const int   MAX_DEG     = 3;          // max polynomial degree
const int   MAX_COEF    = MAX_DEG + 1;
const unsigned long LIVE_MS = 500;    // live readout cadence

/* ---- per-channel state ---- */
struct Channel {
  const char *name;
  int    pin;
  double px[MAX_PTS];                 // captured raw (averaged count)
  double py[MAX_PTS];                 // captured true value (operator units)
  int    n = 0;
  int    degree = 3;
  double coef[MAX_COEF] = {0,0,0,0};
  bool   haveFit = false;
  double rms = 0, maxErr = 0;
  Channel(const char *nm, int p) : name(nm), pin(p) {}
};
Channel chI("CURRENT", PIN_I);
Channel chV("VOLTAGE", PIN_V);

Preferences prefs;
bool liveOn = true;
unsigned long lastLiveMs = 0;

/* ---- NVS record (per channel) ---- */
struct CalRec { uint32_t magic; int32_t degree; double coef[MAX_COEF]; };
const uint32_t CAL_MAGIC = 0xADC0CA11;

/* =============================================================================
 *  ADC read -- trimmed mean of SAMPLES readings (rejects spikes)
 * ========================================================================== */
double readAdcTrimmed(int pin) {
  static uint16_t buf[SAMPLES];
  for (int i = 0; i < SAMPLES; i++) { buf[i] = analogRead(pin); delayMicroseconds(150); }
  // insertion sort (SAMPLES is small)
  for (int i = 1; i < SAMPLES; i++) {
    uint16_t k = buf[i]; int j = i - 1;
    while (j >= 0 && buf[j] > k) { buf[j + 1] = buf[j]; j--; }
    buf[j + 1] = k;
  }
  long sum = 0; int cnt = 0;
  for (int i = TRIM; i < SAMPLES - TRIM; i++) { sum += buf[i]; cnt++; }
  return cnt ? (double)sum / cnt : 0.0;
}

/* =============================================================================
 *  Least-squares polynomial fit (normal equations + Gaussian elimination)
 * ========================================================================== */
static bool solveLinear(double A[MAX_COEF][MAX_COEF], double b[MAX_COEF], int n, double x[MAX_COEF]) {
  for (int col = 0; col < n; col++) {
    int piv = col; double best = fabs(A[col][col]);
    for (int r = col + 1; r < n; r++) if (fabs(A[r][col]) > best) { best = fabs(A[r][col]); piv = r; }
    if (best < 1e-12) return false;                              // singular / degenerate
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

double applyPoly(const double *coef, int degree, double raw) {
  double u = raw / (double)ADC_MAX, val = 0, up = 1;
  for (int k = 0; k <= degree; k++) { val += coef[k] * up; up *= u; }
  return val;
}

bool fitChannel(Channel &c) {
  int D = c.degree, m = D + 1;
  if (c.n < m) { Serial.printf("  %s: need >= %d points, have %d\n", c.name, m, c.n); return false; }
  double A[MAX_COEF][MAX_COEF] = {{0}}, b[MAX_COEF] = {0};
  for (int p = 0; p < c.n; p++) {
    double u = c.px[p] / (double)ADC_MAX, y = c.py[p];
    double upow[2 * MAX_DEG + 1]; upow[0] = 1;
    for (int k = 1; k <= 2 * D; k++) upow[k] = upow[k - 1] * u;
    for (int i = 0; i <= D; i++) { for (int j = 0; j <= D; j++) A[i][j] += upow[i + j]; b[i] += y * upow[i]; }
  }
  double x[MAX_COEF] = {0};
  if (!solveLinear(A, b, m, x)) { Serial.printf("  %s: fit failed (singular -- add more/spread points)\n", c.name); return false; }
  for (int k = 0; k < MAX_COEF; k++) c.coef[k] = (k <= D) ? x[k] : 0.0;
  // error stats in real units
  double se = 0, mx = 0;
  for (int p = 0; p < c.n; p++) { double e = applyPoly(c.coef, D, c.px[p]) - c.py[p]; se += e * e; if (fabs(e) > mx) mx = fabs(e); }
  c.rms = sqrt(se / c.n); c.maxErr = mx; c.haveFit = true;
  Serial.printf("  %s deg=%d:", c.name, D);
  for (int k = 0; k <= D; k++) Serial.printf(" a%d=%.6g", k, c.coef[k]);
  Serial.printf("   RMS=%.4g  max=%.4g\n", c.rms, c.maxErr);
  return true;
}

/* =============================================================================
 *  NVS
 * ========================================================================== */
void saveChannel(const char *key, Channel &c) {
  CalRec r; r.magic = CAL_MAGIC; r.degree = c.degree;
  for (int k = 0; k < MAX_COEF; k++) r.coef[k] = c.coef[k];
  prefs.putBytes(key, &r, sizeof(r));
}
bool loadChannel(const char *key, Channel &c) {
  CalRec r;
  if (prefs.getBytes(key, &r, sizeof(r)) != sizeof(r) || r.magic != CAL_MAGIC) return false;
  if (r.degree < 1 || r.degree > MAX_DEG) return false;
  c.degree = r.degree; for (int k = 0; k < MAX_COEF; k++) c.coef[k] = r.coef[k]; c.haveFit = true;
  return true;
}
void showStored(const char *key, const char *name) {
  CalRec r;
  if (prefs.getBytes(key, &r, sizeof(r)) != sizeof(r) || r.magic != CAL_MAGIC) { Serial.printf("  %s: (none)\n", name); return; }
  Serial.printf("  %s deg=%d:", name, (int)r.degree);
  for (int k = 0; k <= r.degree; k++) Serial.printf(" a%d=%.6g", k, r.coef[k]);
  Serial.println();
}

/* =============================================================================
 *  UI
 * ========================================================================== */
void printHelp() {
  Serial.println(F("\n--- ESP1 ADC opto calibration ---"));
  Serial.println(F("  r            raw counts once"));
  Serial.println(F("  i <value>    capture CURRENT point (apply known input, type true value)"));
  Serial.println(F("  v <value>    capture VOLTAGE point"));
  Serial.println(F("  l            list points     x [i|v]  clear points"));
  Serial.println(F("  d <2|3>      polynomial degree (default 3)"));
  Serial.println(F("  c            compute fit + save to NVS"));
  Serial.println(F("  s  show NVS    w  wipe NVS    live  toggle readout    h  help"));
}

void capture(Channel &c, double val) {
  if (c.n >= MAX_PTS) { Serial.printf("  %s: point buffer full (%d)\n", c.name, MAX_PTS); return; }
  double raw = readAdcTrimmed(c.pin);
  c.px[c.n] = raw; c.py[c.n] = val; c.n++;
  Serial.printf("  %s captured #%d: adc=%.1f  value=%.4g\n", c.name, c.n, raw, val);
}

void listPoints(Channel &c) {
  Serial.printf("  %s (%d pts, deg %d):\n", c.name, c.n, c.degree);
  for (int i = 0; i < c.n; i++) Serial.printf("    #%d adc=%.1f  value=%.4g\n", i + 1, c.px[i], c.py[i]);
}

void handleLine(char *line) {
  while (*line == ' ') line++;
  char *sp = strchr(line, ' ');
  char *arg = sp ? sp + 1 : nullptr;
  if (sp) *sp = '\0';                                 // split command / arg
  String cmd = line;

  if      (cmd == "h" || cmd == "?") printHelp();
  else if (cmd == "r") Serial.printf("  raw: I(34)=%.1f  V(35)=%.1f\n", readAdcTrimmed(PIN_I), readAdcTrimmed(PIN_V));
  else if (cmd == "i") { if (arg) capture(chI, atof(arg)); else Serial.println(F("  usage: i <value>")); }
  else if (cmd == "v") { if (arg) capture(chV, atof(arg)); else Serial.println(F("  usage: v <value>")); }
  else if (cmd == "l") { listPoints(chI); listPoints(chV); }
  else if (cmd == "x") {
    if (arg && arg[0] == 'i') { chI.n = 0; Serial.println(F("  cleared CURRENT points")); }
    else if (arg && arg[0] == 'v') { chV.n = 0; Serial.println(F("  cleared VOLTAGE points")); }
    else { chI.n = chV.n = 0; Serial.println(F("  cleared all points")); }
  }
  else if (cmd == "d") {
    int d = arg ? atoi(arg) : 0;
    if (d < 1 || d > MAX_DEG) Serial.printf("  degree must be 1..%d\n", MAX_DEG);
    else { chI.degree = chV.degree = d; Serial.printf("  degree = %d\n", d); }
  }
  else if (cmd == "c") {
    Serial.println(F("  computing fit..."));
    bool okI = fitChannel(chI), okV = fitChannel(chV);
    if (okI) saveChannel("i", chI);
    if (okV) saveChannel("v", chV);
    if (okI || okV) Serial.println(F("  saved to NVS."));
  }
  else if (cmd == "s") { Serial.println(F("  stored calibration:")); showStored("i", "CURRENT"); showStored("v", "VOLTAGE"); }
  else if (cmd == "w") { prefs.remove("i"); prefs.remove("v"); chI.haveFit = chV.haveFit = false; Serial.println(F("  NVS calibration wiped")); }
  else if (cmd == "live") { liveOn = !liveOn; Serial.printf("  live readout %s\n", liveOn ? "ON" : "OFF"); }
  else if (cmd.length()) Serial.printf("  unknown '%s' (h for help)\n", cmd.c_str());
}

void pollSerial() {
  static char buf[48]; static uint8_t len = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') { if (len) { buf[len] = '\0'; handleLine(buf); len = 0; } }
    else if (len < sizeof(buf) - 1) buf[len++] = ch;
    else len = 0;                                     // overflow -> drop
  }
}

void liveTick() {
  if (!liveOn || millis() - lastLiveMs < LIVE_MS) return;
  lastLiveMs = millis();
  double rI = readAdcTrimmed(PIN_I), rV = readAdcTrimmed(PIN_V);
  Serial.printf("I raw=%4.0f", rI);
  if (chI.haveFit) Serial.printf(" cal=%.4g", applyPoly(chI.coef, chI.degree, rI)); else Serial.print(" cal=--");
  Serial.printf("   |   V raw=%4.0f", rV);
  if (chV.haveFit) Serial.printf(" cal=%.4g", applyPoly(chV.coef, chV.degree, rV)); else Serial.print(" cal=--");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_I, ADC_11db);
  analogSetPinAttenuation(PIN_V, ADC_11db);

  prefs.begin("adccal", false);
  bool li = loadChannel("i", chI), lv = loadChannel("v", chV);

  Serial.println(F("\n=== ESP1 ADC opto calibration tool ==="));
  Serial.printf("Loaded NVS cal: CURRENT=%s  VOLTAGE=%s\n", li ? "yes" : "no", lv ? "yes" : "no");
  printHelp();
}

void loop() {
  pollSerial();
  liveTick();
}
