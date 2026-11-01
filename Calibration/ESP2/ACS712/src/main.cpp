/* =============================================================================
 *  SMART IRRIGATION -- CALIBRATION TOOL -- ACS712 MIXER CURRENT (ESP32 #2)
 * -----------------------------------------------------------------------------
 *  The mixer is an AC motor, so the firmware works out an RMS current: (volts - zero) / sensitivity, over a
 *  whole mains cycle. This does the same. The old version averaged the AC signal, which lands on the zero
 *  point whatever the current is, so its `sens` could not work on this motor.
 *
 *  HOW TO USE (ADC on GPIO36)
 *    1. Mixer OFF:  zero        measures the zero point. Refused if the motor seems to be running.
 *    2. Hold BOOT:  the mixer runs after the inverter's 5 s warmup. Read its current on a clamp meter.
 *    3. Still holding:  sens <A>   works out the sensitivity. Release BOOT.
 *    4. show        prints the lines for CalStore and ESP2.
 *    Also: r, live, off (stop now), h.
 *
 *  SAFETY: the mixer runs only while BOOT is held, up to 60 s. Releasing it or `off` stops everything.
 *  P4 is never driven. ESP2's PCF8575 is on the rail ESP1 switches with GPIO4: if it is not found the mixer
 *  cannot run, but `zero` still works.
 * ========================================================================== */
#include <Arduino.h>
#include <Wire.h>

const int PIN_MIXER_I = 36;            // ESP2's PIN_MIXER_I (ADC1, input only)
const int I2C_SDA = 21, I2C_SCL = 22;
const uint8_t PCF_ADDR = 0x20;
#define B_INVERTER (1u << 5)
#define B_MIXER    (1u << 8)
#define B_MASTER   (1u << 15)
#define PIN_BOOT 0
const unsigned long BTN_DEBOUNCE_MS = 25, WARMUP_MS = 5000, CAP_MS = 60000, LIVE_MS = 1000;

const int RMS_SAMPLES = 200;           // 1 ms apart: 10 cycles at 50 Hz, 12 at 60 Hz
const float ZERO_MAX_RMS_A = 0.3f;     // ESP2's MIXER_MIN_CURRENT_A: above this the motor is really drawing
const float SENS_MIN_VRMS = 0.005f;    // below this the signal is lost in the noise

float zeroV = 1.65f;                   // ESP1's calAcs712Zero default
float sensVperA = 0.100f;              // ESP2's ACS712_SENS_V_PER_A default
bool zeroDone = false, sensDone = false;

enum Phase { IDLE, WARMUP, RUNNING };
Phase phase = IDLE;
unsigned long phaseMs = 0, btnChangeMs = 0, lastLiveMs = 0;
bool btnRaw = false, btnStable = false, capLatched = false, stopFailed = false, liveOn = false;
String line;

bool pcfWrite(uint16_t onBits) {
  uint16_t bank = 0xFFFF & ~onBits;
  for (uint8_t i = 0; i < 3; i++) {
    Wire.beginTransmission(PCF_ADDR);
    Wire.write(bank & 0xFF);
    Wire.write((bank >> 8) & 0xFF);
    if (Wire.endTransmission() == 0) return true;
  }
  return false;
}
void safeAll() {
  stopFailed = !pcfWrite(0);
  if (stopFailed) Serial.println("!!! COULD NOT REACH THE PCF8575: outputs may still be ON. Retrying.");
}

struct Reading { float meanV, vRms; };   // vRms is around the mean: the AC part alone

Reading sampleAc() {
  double sum = 0, sq = 0;
  static float v[RMS_SAMPLES];
  for (int i = 0; i < RMS_SAMPLES; i++) {
    unsigned long t = micros();
    v[i] = (analogRead(PIN_MIXER_I) / 4095.0f) * 3.3f;   // same scaling as the firmware
    sum += v[i];
    while (micros() - t < 1000) { }
  }
  float mean = sum / RMS_SAMPLES;
  for (int i = 0; i < RMS_SAMPLES; i++) { double d = v[i] - mean; sq += d * d; }
  return Reading{ mean, (float)sqrt(sq / RMS_SAMPLES) };
}

// The firmware's RMS about the stored zero, so what prints here is what ESP2 will report.
float firmwareAmps() {
  double sq = 0;
  for (int i = 0; i < RMS_SAMPLES; i++) {
    unsigned long t = micros();
    float v = (analogRead(PIN_MIXER_I) / 4095.0f) * 3.3f;
    float a = (v - zeroV) / sensVperA;
    sq += a * a;
    while (micros() - t < 1000) { }
  }
  return sqrt(sq / RMS_SAMPLES);
}

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  zero        mixer OFF: measure the zero point");
  Serial.println("  <hold BOOT> run the mixer (5 s inverter warmup first)");
  Serial.println("  sens <A>    while it runs: current from a clamp meter");
  Serial.println("  show        the result");
  Serial.println("  r / live    reading now, and live on/off      off  stop everything now");
}

void printRaw() {
  Reading r = sampleAc();
  Serial.printf("V=%.4f  ripple=%.1f mV rms  I=%.3f A rms  [%s]\n", r.meanV, r.vRms * 1000.0f, firmwareAmps(),
                phase == RUNNING ? "MIXER ON" : phase == WARMUP ? "INVERTER WARMUP" : "off");
}

void printResult() {
  Serial.println("======== RESULT ========");
  Serial.printf("zero = %.4f V%s, sensitivity = %.1f mV/A%s\n", zeroV, zeroDone ? "" : " (default, not measured)",
                sensVperA * 1000.0f, sensDone ? "" : " (default, not measured)");
  if (zeroDone) {
    Serial.println("CalStore on ESP1 (flash Calibration/ESP1/CalStore, paste, flash the firmware back):");
    Serial.printf("  set acs %.4f\n", zeroV);
  }
  if (sensDone) {
    Serial.println("ESP2/src/main.cpp, then reflash ESP2:");
    Serial.printf("  const float ACS712_SENS_V_PER_A = %.4ff;\n", sensVperA);
  }
  Serial.println("========================");
}

void stopMixer(const char *why) {
  safeAll();
  phase = IDLE;
  Serial.printf("STOP (%s).\n", why);
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
  if (cmd == "show") { printResult(); return; }
  if (cmd == "off")  { stopMixer("off"); capLatched = true; return; }
  if (cmd == "zero") {
    if (phase != IDLE) { Serial.println("REFUSED: release BOOT first. The zero is measured with the mixer off."); return; }
    Reading r = sampleAc();
    if (r.vRms / sensVperA > ZERO_MAX_RMS_A) {
      Serial.printf("REFUSED: it shows about %.2f A of AC. Stop the mixer first.\n", r.vRms / sensVperA);
      return;
    }
    if (r.meanV < 0.05f || r.meanV > 3.25f) { Serial.printf("REFUSED: %.3f V is at the rail. Check the sensor's wiring and divider.\n", r.meanV); return; }
    zeroV = r.meanV; zeroDone = true;
    Serial.printf("Zero = %.4f V\n", zeroV);
    Serial.println("NEXT: hold BOOT to run the mixer, read it on a clamp meter, and type sens <A> while holding.");
    return;
  }
  if (cmd == "sens") {
    float amps = arg.toFloat();
    if (!(amps > 0.05f)) { Serial.println("usage: sens <amps from your clamp meter>, with the mixer running"); return; }
    if (phase != RUNNING) { Serial.println("REFUSED: the mixer is not running. Hold BOOT and wait for it to start."); return; }
    Reading r = sampleAc();
    if (r.vRms < SENS_MIN_VRMS) { Serial.printf("REFUSED: only %.1f mV of AC signal. Check the sensor and that the motor is really running.\n", r.vRms * 1000.0f); return; }
    sensVperA = r.vRms / amps; sensDone = true;
    Serial.printf("Sensitivity = %.1f mV/A at the ADC (a 20 A module is 100 before any divider, a 30 A 66)\n", sensVperA * 1000.0f);
    Serial.println("NEXT: release BOOT and type show");
    return;
  }
  Serial.println("Unknown command: type h");
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(50);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  delay(300);
  safeAll();
  Serial.println("\n=== ACS712 MIXER CURRENT CALIBRATION (ESP32 #2) ===");
  Wire.beginTransmission(PCF_ADDR);
  if (Wire.endTransmission() != 0) Serial.println("PCF8575 not found: the mixer cannot run from here (ESP1's GPIO4 switches its rail). zero still works.");
  printHelp();
  Serial.println("START: mixer off. Type zero");
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { if (line.length()) { handleLine(line); line = ""; } }
    else if (line.length() < 64) line += c;
  }
  static unsigned long lastRetryMs = 0;
  if (stopFailed && millis() - lastRetryMs >= 200) { lastRetryMs = millis(); safeAll(); }

  bool pressed = (digitalRead(PIN_BOOT) == LOW);
  if (pressed != btnRaw) { btnRaw = pressed; btnChangeMs = millis(); }
  if (pressed != btnStable && millis() - btnChangeMs > BTN_DEBOUNCE_MS) {
    btnStable = pressed;
    if (btnStable) {
      if (capLatched) Serial.println("Release BOOT, then press again.");
      else if (phase == IDLE) {
        if (pcfWrite(B_MASTER | B_INVERTER)) { phase = WARMUP; phaseMs = millis(); Serial.println("Inverter starting, mixer in 5 s. Keep holding BOOT."); }
        else Serial.println("FAILED: no PCF8575. ESP1's GPIO4 switches its rail: is it on?");
      }
    } else {
      if (phase != IDLE) stopMixer("BOOT released");
      capLatched = false;
    }
  }
  if (phase == WARMUP && millis() - phaseMs >= WARMUP_MS) {
    if (pcfWrite(B_MASTER | B_INVERTER | B_MIXER)) { phase = RUNNING; phaseMs = millis(); Serial.println("Mixer ON."); }
    else { stopMixer("no PCF8575"); capLatched = true; }
  }
  if (phase == RUNNING && millis() - phaseMs >= CAP_MS) { stopMixer("60 s limit"); capLatched = true; }

  if (liveOn && millis() - lastLiveMs >= LIVE_MS) { lastLiveMs = millis(); printRaw(); }
}
