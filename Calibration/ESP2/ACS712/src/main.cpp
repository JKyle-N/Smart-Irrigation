/* =============================================================================
 *  SMART IRRIGATION  --  CALIBRATION BENCH TOOL  --  ACS712 CURRENT (ESP32 #2)
 * -----------------------------------------------------------------------------
 *  Standalone. No ESP1, no UART. Captures the ACS712 ZERO offset (quiescent output
 *  at 0 A) used by the REAL firmware's mixer-current read (spec sec.A.4):
 *      v = (adc/4095)*3.3;  amps = (v - ACS712_ZERO_V) / ACS712_SENS_V_PER_A
 *      (ESP2/src/main.cpp:1085-1090)
 *  With the mixer motor OFF, the sensor output should sit at its zero point; this
 *  tool averages it and prints ACS712_ZERO_V. Optionally refine the sensitivity
 *  (V/A) from a known current.
 *
 *  PIN (match ESP2/src/main.cpp:131,159-160): ACS712 output GPIO36 (ADC1, input-only).
 *  NOTE: scale the ACS712 5V output into the ESP32 0..3.3 V ADC range (divider),
 *  or the zero sits near 1.65 V by design of your front-end.
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    r                 raw ADC + volts + amps once (at current zero/sens)
 *    live              toggle ~2 Hz live readout
 *    zero              motor OFF: capture the zero-offset volts -> ACS712_ZERO_V
 *    sens <amps>       motor drawing a KNOWN current: derive V/A -> ACS712_SENS_V_PER_A
 *    show              print current zero + sensitivity
 *
 *  Build: PlatformIO `pio run -e esp32dev` in this folder.
 * ========================================================================== */
#include <Arduino.h>

const int PIN_MIXER_I = 36;          // ESP2/src/main.cpp:131
const int SAMPLES  = 64;
const unsigned long LIVE_MS = 500;

// Defaults from the firmware (ESP2/src/main.cpp:159-160).
double zeroV = 1.65;                  // ACS712_ZERO_V
double sensVperA = 0.100;             // ACS712_SENS_V_PER_A (20A module ~100mV/A)

bool liveOn = false;
unsigned long lastLiveMs = 0;
String line;

double voltsMean();
void   printHelp();
void   printRaw();
void   handleLine(const String &s);

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  delay(300);
  Serial.println();
  Serial.println("=== ACS712 MIXER-CURRENT CALIBRATION (ESP32 #2) ===");
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

double voltsMean() {
  long sum = 0;
  for (int i = 0; i < SAMPLES; i++) sum += analogRead(PIN_MIXER_I);
  double adc = (double)sum / SAMPLES;
  return (adc / 4095.0) * 3.3;
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  h            this help");
  Serial.println("  r            raw ADC + volts + amps once");
  Serial.println("  live         toggle ~2 Hz live readout");
  Serial.println("  zero         motor OFF: capture zero-offset -> ACS712_ZERO_V");
  Serial.println("  sens <amps>  motor at a KNOWN current: derive V/A");
  Serial.println("  show         print current zero + sensitivity");
}

void printRaw() {
  double v = voltsMean();
  double a = fabs((v - zeroV) / sensVperA);
  Serial.print("V="); Serial.print(v, 4);
  Serial.print("  amps="); Serial.print(a, 3);
  Serial.println("  (|(V-zero)/sens|)");
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
  if (cmd == "show") {
    Serial.print("ACS712_ZERO_V = "); Serial.println(zeroV, 4);
    Serial.print("ACS712_SENS_V_PER_A = "); Serial.println(sensVperA, 4);
    return;
  }
  if (cmd == "zero") {
    zeroV = voltsMean();
    Serial.print("zero captured. ACS712_ZERO_V = "); Serial.println(zeroV, 4);
    Serial.println("// ESP2/src/main.cpp:160 -- paste:");
    Serial.print("float ACS712_ZERO_V = "); Serial.print(zeroV, 4); Serial.println("f;");
    return;
  }
  if (cmd == "sens") {
    if (arg.length() == 0) { Serial.println("usage: sens <amps>  (a KNOWN load current)"); return; }
    double amps = arg.toFloat();
    if (fabs(amps) < 1e-3) { Serial.println("amps must be non-zero"); return; }
    double v = voltsMean();
    sensVperA = fabs(v - zeroV) / fabs(amps);
    Serial.print("ACS712_SENS_V_PER_A = "); Serial.println(sensVperA, 4);
    Serial.println("// ESP2/src/main.cpp:159 -- paste:");
    Serial.print("const float ACS712_SENS_V_PER_A = "); Serial.print(sensVperA, 4); Serial.println("f;");
    return;
  }
  Serial.println("? unknown command -- type h");
}
