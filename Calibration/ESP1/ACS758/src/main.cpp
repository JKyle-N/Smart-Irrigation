/* =============================================================================
 *  SMART IRRIGATION  --  CALIBRATION BENCH TOOL  --  ACS758 CURRENT via ADS1115 (ESP32 #1)
 * -----------------------------------------------------------------------------
 *  Standalone. No ESP2, no Nano, no UART. Reads an ACS758 hall-effect current
 *  sensor through an ADS1115 16-bit I2C ADC and captures the two constants the
 *  real firmware needs:
 *      amps = (v_sensor - ACS758_ZERO_V) / ACS758_SENS_V_PER_A
 *  Same shape as the existing ESP2 ACS712 tool (Calibration/ESP2/ACS712), so the
 *  method and the paste-ready output transfer 1:1.
 *
 *  WHY AN EXTERNAL ADC: the ESP32's own ADC is nonlinear and noisy at the few-mV
 *  level an ACS758 resolves (10-40 mV/A). The ADS1115 gives 125 uV/LSB on the
 *  +/-4.096 V range -- ~0.003 A of resolution on a 40 mV/A part -- plus a stable
 *  internal reference, which is what makes a hall sensor usable at all.
 *
 *  ############################################################################
 *  #  WIRING WARNING -- READ BEFORE POWERING UP                               #
 *  #  The ACS758 runs on 5 V and its output swings up to ~5 V. The ADS1115's  #
 *  #  absolute maximum input is VDD + 0.3 V. Feeding a 5 V signal into an     #
 *  #  ADS1115 powered from 3.3 V WILL damage the ADC.                         #
 *  #                                                                          #
 *  #  Use a resistive divider on the sensor output (default assumes 2:1):     #
 *  #                                                                          #
 *  #    ACS758 VIOUT --[ R1 10k ]--+--[ R2 10k ]-- GND                        #
 *  #                               |                                          #
 *  #                               +--> ADS1115 AIN0                          #
 *  #                                                                          #
 *  #  Divider ratio = (R1 + R2) / R2 = 2.0 for 10k/10k. Tell the tool the     #
 *  #  ratio with `div <ratio>`; it multiplies the measured voltage back up to #
 *  #  reconstruct the true sensor output. MEASURE your resistors and use the  #
 *  #  real ratio -- divider error becomes current error 1:1.                  #
 *  #                                                                          #
 *  #  Power the ADS1115 from 3.3 V (same rail as the ESP32). Do NOT power it  #
 *  #  from 5 V: its logic-high threshold is 0.7 x VDD = 3.5 V, which the      #
 *  #  ESP32's 3.3 V I2C lines cannot reliably reach.                          #
 *  #  Add a 100 nF cap across the ADS1115 supply and keep the sensor lead     #
 *  #  short -- this is a high-impedance, few-mV measurement.                  #
 *  ############################################################################
 *
 *  PINS (match ESP1/src/main.cpp:62-63): I2C SDA=21, SCL=22.
 *  ADS1115 ADDR pin -> GND = 0x48 (default here). VDD=0x49, SDA=0x4A, SCL=0x4B.
 *  0x48 does not collide with the rig's LCD 0x27 / INA226 0x40 / EEPROM 0x57 / RTC 0x68.
 *
 *  RATIOMETRIC SENSOR: the ACS758's zero point and sensitivity both scale with its
 *  supply. Zero = VCC/2 (bidirectional "B" parts) or 0.12 x VCC (unidirectional "U").
 *  Measure the actual 5 V rail with a DMM and enter it via `vcc <volts>` -- a rail
 *  that is really 4.85 V shifts the zero point by 75 mV, which on a 40 mV/A part is
 *  a phantom 1.9 A. This is the single most common ACS758 calibration mistake.
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    scan              I2C bus scan (find the ADS1115)
 *    r                 read once: raw code, volts, amps
 *    live              toggle ~2 Hz live readout
 *    stats [n]         n samples (default 200): mean/min/max/ripple + amps
 *    zero              load OFF: capture the quiescent output -> ACS758_ZERO_V
 *    sens <amps>       load at a KNOWN current: derive V/A -> ACS758_SENS_V_PER_A
 *    variant <name>    datasheet defaults, e.g. `variant 50B` / `100U` (see `variant ?`)
 *    vcc <volts>       measured ACS758 supply rail (ratiometric correction)
 *    div <ratio>       divider ratio (R1+R2)/R2 in front of the ADC; 1.0 = direct
 *    ch <0-3>          ADS1115 single-ended input channel (default 0)
 *    addr <0x48-0x4B>  ADS1115 I2C address
 *    gain <n>          PGA full-scale: 0=6.144 1=4.096 2=2.048 3=1.024 4=0.512 5=0.256 V
 *    show              print every current setting + the paste-ready constants
 *
 *  Build: PlatformIO `pio run -e esp32dev` in this folder.
 * ========================================================================== */
#include <Arduino.h>
#include <Wire.h>

#define I2C_SDA 21                      // ESP1/src/main.cpp:62
#define I2C_SCL 22                      // ESP1/src/main.cpp:63

/* ---- ADS1115 registers --------------------------------------------------- */
#define ADS_REG_CONVERT  0x00
#define ADS_REG_CONFIG   0x01

// Config bit fields (datasheet Table 8). Assembled in adsReadRaw().
#define ADS_OS_SINGLE    0x8000         // bit15: start a single conversion
#define ADS_MODE_SINGLE  0x0100         // bit8 : single-shot (not continuous)
#define ADS_DR_128SPS    0x0080         // bits7-5: 128 SPS -> ~7.8 ms/conversion
#define ADS_COMP_OFF     0x0003         // bits1-0: disable the comparator

uint8_t  adsAddr = 0x48;                // ADDR -> GND
uint8_t  adsChan = 0;                   // single-ended AIN0..AIN3
uint8_t  adsGain = 1;                   // index into PGA_FS below; 1 = +/-4.096 V

// PGA full-scale voltages, indexed by the `gain` command.
const float PGA_FS[6] = { 6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f };

/* ---- Front-end + sensor model -------------------------------------------- */
double divRatio = 2.0;                  // (R1+R2)/R2 -- 10k/10k. 1.0 if wired direct.
double acsVcc   = 5.0;                  // MEASURE this rail; the sensor is ratiometric
bool   biDir    = true;                 // "B" part (zero at VCC/2) vs "U" (zero at 0.12*VCC)
double zeroV    = 2.5;                  // ACS758_ZERO_V  (recomputed by `variant` / `vcc`)
double sensVperA = 0.040;               // ACS758_SENS_V_PER_A (50 A bidirectional default)
double sens5V   = 0.040;                // datasheet sensitivity AT 5.0 V, before ratiometric scaling

bool liveOn = false;
unsigned long lastLiveMs = 0;
String line;

/* ---- forward decls -------------------------------------------------------- */
bool   adsPresent();
bool   adsReadRaw(int16_t &raw);
bool   voltsMean(int samples, double &vSensor, double *vMin = nullptr,
                 double *vMax = nullptr, double *raw = nullptr);
double ampsFrom(double vSensor);
void   applyRatiometric();
void   i2cBusRecover();
void   printHelp();
void   printVariants();
void   printShow();
void   printRead();
void   doScan();
void   doStats(int n);
void   handleLine(const String &s);

/* =============================================================================
 *  SETUP / LOOP
 * ========================================================================== */
void setup() {
  Serial.begin(115200);
  delay(300);
  i2cBusRecover();                      // same trick as ESP1/src/main.cpp:1366 -- free a stuck slave
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);                // 100 kHz: this is a precision read, not a race

  Serial.println();
  Serial.println("=== ACS758 CURRENT CALIBRATION via ADS1115 (ESP32 #1) ===");
  Serial.println("!! ACS758 output is 0-5 V. The ADS1115 must NOT see more than VDD+0.3 V.");
  Serial.println("!! Use a divider (default assumes 2:1) and power the ADS1115 from 3.3 V.");
  Serial.println();

  applyRatiometric();

  if (!adsPresent()) {
    Serial.print("ADS1115 NOT FOUND at 0x"); Serial.println(adsAddr, HEX);
    Serial.println("  -> type `scan` to list the bus, `addr 0x49` to try another address.");
    Serial.println("  -> check VDD/GND, SDA=21, SCL=22, and that ADDR is tied to a rail.");
  } else {
    Serial.print("ADS1115 found at 0x"); Serial.println(adsAddr, HEX);
  }
  printHelp();
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { if (line.length()) { handleLine(line); line = ""; } }
    else if (line.length() < 64) line += c;
  }
  if (liveOn && millis() - lastLiveMs >= 500) { lastLiveMs = millis(); printRead(); }
}

/* =============================================================================
 *  ADS1115 ACCESS
 * ========================================================================== */
bool adsPresent() {
  Wire.beginTransmission(adsAddr);
  return Wire.endTransmission() == 0;
}

// One single-shot conversion on the selected channel. Returns false on any I2C fault
// so a missing/unpowered ADC reads as an error instead of a plausible-looking 0 A.
bool adsReadRaw(int16_t &raw) {
  uint16_t cfg = ADS_OS_SINGLE | ADS_MODE_SINGLE | ADS_DR_128SPS | ADS_COMP_OFF;
  cfg |= (uint16_t)(0x4000 | ((adsChan & 0x03) << 12));   // MUX 1xx = AINx vs GND
  cfg |= (uint16_t)((adsGain & 0x07) << 9);               // PGA

  Wire.beginTransmission(adsAddr);
  Wire.write(ADS_REG_CONFIG);
  Wire.write((uint8_t)(cfg >> 8));
  Wire.write((uint8_t)(cfg & 0xFF));
  if (Wire.endTransmission() != 0) return false;

  // Poll the OS bit rather than blind-delaying: at 128 SPS a conversion is ~7.8 ms,
  // but a slow bus or a different data rate would make a fixed delay read stale data.
  unsigned long t0 = millis();
  for (;;) {
    Wire.beginTransmission(adsAddr);
    Wire.write(ADS_REG_CONFIG);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((int)adsAddr, 2) != 2) return false;
    uint16_t st = ((uint16_t)Wire.read() << 8) | Wire.read();
    if (st & ADS_OS_SINGLE) break;                        // OS reads 1 when idle/done
    if (millis() - t0 > 200) return false;                // conversion never completed
    delay(1);
  }

  Wire.beginTransmission(adsAddr);
  Wire.write(ADS_REG_CONVERT);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)adsAddr, 2) != 2) return false;
  raw = (int16_t)(((uint16_t)Wire.read() << 8) | Wire.read());
  return true;
}

// Average `samples` conversions and undo the input divider to get the voltage the
// ACS758 is actually producing. Optionally reports the spread (ripple) and mean code.
bool voltsMean(int samples, double &vSensor, double *vMin, double *vMax, double *rawOut) {
  if (samples < 1) samples = 1;
  double sum = 0, lo = 1e9, hi = -1e9, rawSum = 0;
  const double lsb = PGA_FS[adsGain] / 32768.0;

  for (int i = 0; i < samples; i++) {
    int16_t raw;
    if (!adsReadRaw(raw)) return false;
    double v = raw * lsb * divRatio;                      // ADC volts -> sensor volts
    sum += v; rawSum += raw;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  vSensor = sum / samples;
  if (vMin)   *vMin = lo;
  if (vMax)   *vMax = hi;
  if (rawOut) *rawOut = rawSum / samples;
  return true;
}

double ampsFrom(double vSensor) {
  return (vSensor - zeroV) / sensVperA;                   // signed: negative = reverse flow
}

// Re-derive the zero point and sensitivity for the ACS758's ACTUAL supply voltage.
// Both are ratiometric: a 3% low rail is a 3% error in both, and the zero-point shift
// alone is worth amps of phantom reading.
void applyRatiometric() {
  zeroV     = biDir ? (acsVcc * 0.5) : (acsVcc * 0.12);
  sensVperA = sens5V * (acsVcc / 5.0);
}

/* Bit-bang up to 9 SCL pulses to free a slave stuck holding SDA low, then STOP.
 * Lifted from ESP1/src/main.cpp:1366 -- same bus, same failure mode. Must run
 * BEFORE Wire.begin(). */
void i2cBusRecover() {
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(10);
  if (digitalRead(I2C_SDA) == LOW) {
    pinMode(I2C_SCL, OUTPUT);
    for (int i = 0; i < 9 && digitalRead(I2C_SDA) == LOW; i++) {
      digitalWrite(I2C_SCL, LOW);  delayMicroseconds(5);
      digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5);
    }
    pinMode(I2C_SDA, OUTPUT);
    digitalWrite(I2C_SDA, LOW);  delayMicroseconds(5);
    pinMode(I2C_SCL, INPUT_PULLUP); delayMicroseconds(5);
    pinMode(I2C_SDA, INPUT_PULLUP);                       // SDA low->high while SCL high = STOP
    Serial.println("[I2C] bus was stuck -- recovered");
  }
}

/* =============================================================================
 *  OUTPUT
 * ========================================================================== */
void printHelp() {
  Serial.println("Commands:");
  Serial.println("  h                 this help");
  Serial.println("  scan              I2C bus scan (find the ADS1115)");
  Serial.println("  r                 read once: raw code, volts, amps");
  Serial.println("  live              toggle ~2 Hz live readout");
  Serial.println("  stats [n]         n samples (default 200): mean/min/max/ripple");
  Serial.println("  zero              load OFF: capture quiescent -> ACS758_ZERO_V");
  Serial.println("  sens <amps>       load at a KNOWN current: derive V/A");
  Serial.println("  variant <name>    datasheet defaults (`variant ?` to list)");
  Serial.println("  vcc <volts>       measured ACS758 supply (ratiometric!)");
  Serial.println("  div <ratio>       input divider (R1+R2)/R2; 1.0 = direct");
  Serial.println("  ch <0-3>          ADS1115 channel; addr <0x48-0x4B>; gain <0-5>");
  Serial.println("  show              all settings + paste-ready constants");
}

void printVariants() {
  Serial.println("ACS758 variants (sensitivity @ 5.0 V):");
  Serial.println("  50B  40.0 mV/A   bidirectional, zero = VCC/2      (+/-50 A)");
  Serial.println("  100B 20.0 mV/A   bidirectional, zero = VCC/2      (+/-100 A)");
  Serial.println("  150B 13.3 mV/A   bidirectional, zero = VCC/2      (+/-150 A)");
  Serial.println("  200B 10.0 mV/A   bidirectional, zero = VCC/2      (+/-200 A)");
  Serial.println("  50U  60.0 mV/A   unidirectional, zero = 0.12*VCC  (0-50 A)");
  Serial.println("  100U 40.0 mV/A   unidirectional, zero = 0.12*VCC  (0-100 A)");
  Serial.println("  150U 26.7 mV/A   unidirectional, zero = 0.12*VCC  (0-150 A)");
  Serial.println("  200U 20.0 mV/A   unidirectional, zero = 0.12*VCC  (0-200 A)");
  Serial.println("The part number is printed on the sensor body, e.g. ACS758LCB-050B-PFF-T.");
}

void printShow() {
  Serial.println("--- front end ---");
  Serial.print("  ADS1115 addr = 0x"); Serial.print(adsAddr, HEX);
  Serial.print("   channel = AIN"); Serial.print(adsChan);
  Serial.print("   PGA = +/-"); Serial.print(PGA_FS[adsGain], 3); Serial.println(" V");
  Serial.print("  LSB = "); Serial.print(PGA_FS[adsGain] / 32768.0 * 1e6, 2); Serial.print(" uV");
  Serial.print("   divider = "); Serial.print(divRatio, 4);
  Serial.print("   -> resolution = ");
  Serial.print(PGA_FS[adsGain] / 32768.0 * divRatio / sensVperA * 1000.0, 2);
  Serial.println(" mA/LSB");
  Serial.println("--- sensor ---");
  Serial.print("  variant = "); Serial.print(biDir ? "bidirectional (B)" : "unidirectional (U)");
  Serial.print("   VCC = "); Serial.print(acsVcc, 3); Serial.println(" V (measured)");
  Serial.print("  datasheet sens @5V = "); Serial.print(sens5V * 1000.0, 2); Serial.println(" mV/A");
  Serial.println("--- constants (paste into the ESP1 firmware) ---");
  Serial.print("const float ACS758_ZERO_V        = "); Serial.print(zeroV, 5); Serial.println("f;");
  Serial.print("const float ACS758_SENS_V_PER_A  = "); Serial.print(sensVperA, 5); Serial.println("f;");
  Serial.print("const float ACS758_DIV_RATIO     = "); Serial.print(divRatio, 5); Serial.println("f;");
}

void printRead() {
  double v, raw;
  if (!voltsMean(16, v, nullptr, nullptr, &raw)) {
    Serial.println("READ FAILED -- ADS1115 not responding (check wiring / `scan`)");
    liveOn = false;                                       // don't spam a dead bus
    return;
  }
  Serial.print("code="); Serial.print(raw, 1);
  Serial.print("  Vadc="); Serial.print(v / divRatio, 5);
  Serial.print("  Vsensor="); Serial.print(v, 5);
  Serial.print("  amps="); Serial.print(ampsFrom(v), 3);
  Serial.println();
}

void doScan() {
  Serial.println("Scanning I2C 0x08-0x77 ...");
  int n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print("  found 0x"); Serial.print(a, HEX);
      if (a >= 0x48 && a <= 0x4B) Serial.print("   <-- ADS1115");
      else if (a == 0x27) Serial.print("   (LCD)");
      else if (a == 0x40) Serial.print("   (INA226)");
      else if (a == 0x57) Serial.print("   (EEPROM)");
      else if (a == 0x68) Serial.print("   (DS3231 RTC)");
      Serial.println();
      n++;
    }
  }
  if (!n) Serial.println("  nothing found -- check SDA=21 SCL=22, pull-ups, and power");
}

void doStats(int n) {
  double v, lo, hi;
  if (!voltsMean(n, v, &lo, &hi)) { Serial.println("READ FAILED -- ADS1115 not responding"); return; }
  Serial.print("n="); Serial.print(n);
  Serial.print("  mean="); Serial.print(v, 5); Serial.print(" V");
  Serial.print("  min="); Serial.print(lo, 5);
  Serial.print("  max="); Serial.print(hi, 5);
  Serial.print("  ripple="); Serial.print((hi - lo) * 1000.0, 2); Serial.println(" mVpp");
  Serial.print("  amps mean="); Serial.print(ampsFrom(v), 3);
  Serial.print("   +/-"); Serial.print((hi - lo) / 2.0 / sensVperA, 3); Serial.println(" A peak");
  // Ripple is the honest noise floor: any current smaller than this is not measurable.
  Serial.print("  -> smallest trustworthy current ~ ");
  Serial.print((hi - lo) / sensVperA, 3); Serial.println(" A");
}

/* =============================================================================
 *  COMMANDS
 * ========================================================================== */
void handleLine(const String &in) {
  String s = in; s.trim();
  int sp = s.indexOf(' ');
  String cmd = (sp < 0) ? s : s.substring(0, sp);
  String arg = (sp < 0) ? "" : s.substring(sp + 1);
  arg.trim();
  cmd.toLowerCase();

  if (cmd == "h")    { printHelp();  return; }
  if (cmd == "scan") { doScan();     return; }
  if (cmd == "r")    { printRead();  return; }
  if (cmd == "show") { printShow();  return; }
  if (cmd == "live") { liveOn = !liveOn; Serial.print("live="); Serial.println(liveOn ? "ON" : "OFF"); return; }

  if (cmd == "stats") { doStats(arg.length() ? arg.toInt() : 200); return; }

  if (cmd == "zero") {
    double v, lo, hi;
    if (!voltsMean(256, v, &lo, &hi)) { Serial.println("READ FAILED -- check wiring"); return; }
    zeroV = v;
    Serial.print("zero captured over 256 samples: ACS758_ZERO_V = "); Serial.println(zeroV, 5);
    Serial.print("  ripple was "); Serial.print((hi - lo) * 1000.0, 2); Serial.println(" mVpp");
    double expect = biDir ? acsVcc * 0.5 : acsVcc * 0.12;
    double dev = fabs(zeroV - expect);
    // A measured zero far from the ratiometric prediction almost always means the
    // divider ratio or the VCC figure is wrong -- not that the sensor is off.
    if (dev > 0.05) {
      Serial.print("  WARNING: expected ~"); Serial.print(expect, 3);
      Serial.print(" V for this variant at VCC="); Serial.print(acsVcc, 2);
      Serial.print(" V; you are "); Serial.print(dev * 1000.0, 0); Serial.println(" mV off.");
      Serial.println("  Check `div` (measure R1/R2), `vcc` (measure the rail), and that the load is truly OFF.");
    }
    Serial.println("// paste:");
    Serial.print("const float ACS758_ZERO_V = "); Serial.print(zeroV, 5); Serial.println("f;");
    return;
  }

  if (cmd == "sens") {
    if (arg.length() == 0) { Serial.println("usage: sens <amps>   (a KNOWN load current, from a clamp meter)"); return; }
    double amps = arg.toFloat();
    if (fabs(amps) < 1e-3) { Serial.println("amps must be non-zero"); return; }
    double v;
    if (!voltsMean(256, v)) { Serial.println("READ FAILED -- check wiring"); return; }
    double delta = v - zeroV;
    if (fabs(delta) < 0.002) {
      Serial.println("output barely moved from zero (<2 mV) -- is the load actually drawing current?");
      return;
    }
    sensVperA = fabs(delta) / fabs(amps);
    sens5V    = sensVperA / (acsVcc / 5.0);               // back out the ratiometric scaling
    Serial.print("delta = "); Serial.print(delta * 1000.0, 2); Serial.print(" mV at ");
    Serial.print(amps, 3); Serial.println(" A");
    Serial.print("ACS758_SENS_V_PER_A = "); Serial.println(sensVperA, 5);
    Serial.print("  (= "); Serial.print(sens5V * 1000.0, 2); Serial.println(" mV/A normalised to 5.0 V)");
    Serial.println("// paste:");
    Serial.print("const float ACS758_SENS_V_PER_A = "); Serial.print(sensVperA, 5); Serial.println("f;");
    return;
  }

  if (cmd == "variant") {
    if (arg.length() == 0 || arg == "?") { printVariants(); return; }
    String v = arg; v.toUpperCase();
    bool ok = true;
    if      (v == "50B")  { sens5V = 0.0400; biDir = true;  }
    else if (v == "100B") { sens5V = 0.0200; biDir = true;  }
    else if (v == "150B") { sens5V = 0.0133; biDir = true;  }
    else if (v == "200B") { sens5V = 0.0100; biDir = true;  }
    else if (v == "50U")  { sens5V = 0.0600; biDir = false; }
    else if (v == "100U") { sens5V = 0.0400; biDir = false; }
    else if (v == "150U") { sens5V = 0.0267; biDir = false; }
    else if (v == "200U") { sens5V = 0.0200; biDir = false; }
    else ok = false;
    if (!ok) { Serial.println("unknown variant -- `variant ?` to list"); return; }
    applyRatiometric();
    Serial.print("variant "); Serial.print(v); Serial.println(" loaded (datasheet defaults):");
    Serial.print("  zero = "); Serial.print(zeroV, 4); Serial.print(" V   sens = ");
    Serial.print(sensVperA * 1000.0, 2); Serial.println(" mV/A");
    Serial.println("  -> now run `zero` with the load OFF to replace the theoretical zero with a measured one.");
    return;
  }

  if (cmd == "vcc") {
    double x = arg.toFloat();
    if (x < 3.0 || x > 6.0) { Serial.println("usage: vcc <3.0-6.0>   (measure the ACS758 supply with a DMM)"); return; }
    acsVcc = x;
    applyRatiometric();
    Serial.print("VCC = "); Serial.print(acsVcc, 3); Serial.println(" V");
    Serial.print("  zero -> "); Serial.print(zeroV, 4); Serial.print(" V   sens -> ");
    Serial.print(sensVperA * 1000.0, 2); Serial.println(" mV/A");
    return;
  }

  if (cmd == "div") {
    double x = arg.toFloat();
    if (x < 0.999 || x > 20.0) { Serial.println("usage: div <ratio>   (R1+R2)/R2; 2.0 for 10k/10k, 1.0 for direct"); return; }
    divRatio = x;
    Serial.print("divider ratio = "); Serial.println(divRatio, 4);
    if (divRatio < 1.5) Serial.println("  NOTE: <1.5 means little attenuation -- confirm the ADC never sees >VDD+0.3 V.");
    return;
  }

  if (cmd == "ch") {
    int c = arg.toInt();
    if (c < 0 || c > 3) { Serial.println("usage: ch <0-3>"); return; }
    adsChan = (uint8_t)c;
    Serial.print("channel = AIN"); Serial.println(adsChan);
    return;
  }

  if (cmd == "addr") {
    long a = (arg.startsWith("0x") || arg.startsWith("0X")) ? strtol(arg.c_str(), nullptr, 16) : arg.toInt();
    if (a < 0x48 || a > 0x4B) { Serial.println("usage: addr <0x48-0x4B>  (ADDR pin: GND/VDD/SDA/SCL)"); return; }
    adsAddr = (uint8_t)a;
    Serial.print("addr = 0x"); Serial.print(adsAddr, HEX);
    Serial.println(adsPresent() ? "  (responding)" : "  (NO RESPONSE)");
    return;
  }

  if (cmd == "gain") {
    int g = arg.toInt();
    if (g < 0 || g > 5) { Serial.println("usage: gain <0-5>  0=6.144 1=4.096 2=2.048 3=1.024 4=0.512 5=0.256 V"); return; }
    adsGain = (uint8_t)g;
    Serial.print("PGA = +/-"); Serial.print(PGA_FS[adsGain], 3); Serial.print(" V   LSB = ");
    Serial.print(PGA_FS[adsGain] / 32768.0 * 1e6, 2); Serial.println(" uV");
    // The divided sensor signal must stay inside the PGA window or readings clip flat.
    double maxIn = acsVcc / divRatio;
    if (maxIn > PGA_FS[adsGain]) {
      Serial.print("  WARNING: divided signal can reach "); Serial.print(maxIn, 3);
      Serial.print(" V, beyond this PGA range -- readings will clip. Use a lower gain index.");
      Serial.println();
    }
    return;
  }

  Serial.println("? unknown command -- type h");
}
