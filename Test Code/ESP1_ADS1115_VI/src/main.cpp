/* =============================================================================
 *  SMART IRRIGATION  --  BENCH TOOL  --  ADS1115 I2C DETECT + BATTERY V/I (ESP32 #1)
 * -----------------------------------------------------------------------------
 *  Standalone. No main firmware, no UART to the Nano/ESP2. Two jobs:
 *    1. DETECT  -- scan the I2C bus, confirm the ADS1115 answers, and identify every
 *                  other device so a wiring mistake is obvious immediately.
 *    2. MEASURE -- continuously read A0 = CURRENT sensor, A1 = VOLTAGE sense, and
 *                  show raw code / ADC volts / engineering amps+volts+watts.
 *
 *  WHY THIS EXISTS: the shipping firmware reads the battery from GPIO34/35 through an
 *  optocoupler with a fitted polynomial (CLAUDE.md). Sixteen days of SD logs show that
 *  path reporting V=0.00 / I=0.00 on every single sample, so nothing downstream --
 *  battery %, energy Wh, low/critical thresholds -- has ever had real data. An ADS1115
 *  gives 125 uV/LSB against a stable internal reference instead of the ESP32's
 *  nonlinear ADC, so this tool exists to prove the replacement front-end before any
 *  firmware is asked to trust it.
 *
 *  ############################################################################
 *  #  WIRING WARNING -- READ BEFORE POWERING UP                               #
 *  #                                                                          #
 *  #  1. POWER THE ADS1115 FROM 3.3 V, not 5 V. At VDD=5 V its logic-high     #
 *  #     threshold is 0.7 x VDD = 3.5 V, which the ESP32's 3.3 V I2C lines    #
 *  #     cannot reliably reach -- the bus works intermittently, which is far  #
 *  #     worse to debug than not working at all.                              #
 *  #                                                                          #
 *  #  2. NEVER exceed VDD + 0.3 V on any analog input. Both channels here     #
 *  #     normally need a divider:                                             #
 *  #       A0 CURRENT: an ACS712/ACS758 runs on 5 V and swings to ~5 V.       #
 *  #       A1 VOLTAGE: a 12 V battery is ~4x over the limit on its own.       #
 *  #                                                                          #
 *  #       sense --[ R1 ]--+--[ R2 ]-- GND        ratio = (R1 + R2) / R2      #
 *  #                       |                                                  #
 *  #                       +--> ADS1115 AINx                                  #
 *  #                                                                          #
 *  #     MEASURE your resistors and set the real ratios (`divi` / `divv`).    #
 *  #     Divider error becomes reading error 1:1.                             #
 *  #                                                                          #
 *  #  3. Common ground between the sensor, the divider and the ADS1115.       #
 *  #     Add 100 nF across the ADS1115 supply; keep the sense leads short.    #
 *  ############################################################################
 *
 *  PINS (match ESP1/src/main.cpp:62-63): I2C SDA=21, SCL=22.
 *  ADS1115 ADDR -> GND = 0x48 (default here). VDD=0x49, SDA=0x4A, SCL=0x4B.
 *  0x48 does not collide with the rig's LCD 0x27 / INA226 0x40 / EEPROM 0x57 / RTC 0x68.
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    scan              I2C bus scan (identifies every device found)
 *    r                 read both channels once
 *    live              toggle ~2 Hz live readout (V, I, W)
 *    stats [n]         n samples (default 200) per channel: mean/min/max/ripple
 *    zero              NO LOAD: capture the current sensor's quiescent output
 *    sens <mV/A>       current-sensor sensitivity, e.g. 100 for ACS712-20A, 40 for ACS758-50B
 *    vset <volts>      1-point voltage cal: enter the TRUE voltage you measure with a DMM
 *    divi <ratio>      divider ratio in front of A0 (current);  1.0 = direct
 *    divv <ratio>      divider ratio in front of A1 (voltage);  1.0 = direct
 *    gain <0-5>        PGA full-scale: 0=6.144 1=4.096 2=2.048 3=1.024 4=0.512 5=0.256 V
 *    addr <0x48-0x4B>  ADS1115 I2C address
 *    show              every setting + the paste-ready constants
 *
 *  Build: PlatformIO `pio run -e esp32dev` in this folder.
 * ========================================================================== */
#include <Arduino.h>
#include <Wire.h>

#define I2C_SDA 21                      // ESP1/src/main.cpp:62
#define I2C_SCL 22                      // ESP1/src/main.cpp:63

#define CH_CURRENT 0                    // ADS1115 A0
#define CH_VOLTAGE 1                    // ADS1115 A1

/* ---- ADS1115 registers --------------------------------------------------- */
#define ADS_REG_CONVERT  0x00
#define ADS_REG_CONFIG   0x01
#define ADS_OS_SINGLE    0x8000         // bit15: start a single conversion / 1 = idle when read
#define ADS_MODE_SINGLE  0x0100         // bit8 : single-shot
#define ADS_DR_128SPS    0x0080         // bits7-5: 128 SPS -> ~7.8 ms/conversion
#define ADS_COMP_OFF     0x0003         // bits1-0: comparator disabled

uint8_t adsAddr = 0x48;                 // ADDR -> GND
uint8_t adsGain = 1;                    // index into PGA_FS; 1 = +/-4.096 V
const float PGA_FS[6] = { 6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f };

/* ---- Front-end model ------------------------------------------------------ *
 * Two independent dividers: the current sensor and the voltage sense almost never share
 * a ratio (a 5 V sensor needs ~2:1, a 12 V battery needs ~6:1).                        */
double divI = 2.0;                      // (R1+R2)/R2 in front of A0   [MEASURE]
double divV = 6.0;                      // (R1+R2)/R2 in front of A1   [MEASURE]
double zeroV = 2.5;                     // current sensor output at 0 A (measured by `zero`)
double sensVperA = 0.100;               // 100 mV/A = ACS712-20A default             [CONFIRM]

bool liveOn = false;
unsigned long lastLiveMs = 0;
String line;

/* ---- forward decls -------------------------------------------------------- */
bool   adsPresent();
bool   adsReadRaw(uint8_t ch, int16_t &raw);
bool   chVolts(uint8_t ch, int samples, double ratio, double &v,
               double *vMin = nullptr, double *vMax = nullptr, double *code = nullptr);
void   i2cBusRecover();
void   printHelp();
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
  i2cBusRecover();                      // same trick as ESP1/src/main.cpp -- free a stuck slave
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);                // 100 kHz: this is a precision read, not a race

  Serial.println();
  Serial.println(F("=== ADS1115 DETECT + BATTERY V/I  (ESP32 #1 bench tool) ==="));
  Serial.println(F("!! ADS1115 must run on 3.3 V, and NO input may exceed VDD+0.3 V."));
  Serial.println(F("!! A0 = current sensor, A1 = voltage sense -- both normally via a divider."));
  Serial.println();

  doScan();                             // detect first: that is the primary job of this tool

  if (!adsPresent()) {
    Serial.print(F("ADS1115 NOT FOUND at 0x")); Serial.println(adsAddr, HEX);
    Serial.println(F("  -> check VDD/GND, SDA=21, SCL=22, and that ADDR is tied to a rail."));
    Serial.println(F("  -> if the scan above listed 0x49/0x4A/0x4B, select it with `addr`."));
  } else {
    Serial.print(F("ADS1115 responding at 0x")); Serial.println(adsAddr, HEX);
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

// One single-shot conversion on `ch` (single-ended vs GND). Returns false on ANY I2C fault so a
// missing or unpowered ADC reads as an error rather than a plausible-looking 0.
bool adsReadRaw(uint8_t ch, int16_t &raw) {
  uint16_t cfg = ADS_OS_SINGLE | ADS_MODE_SINGLE | ADS_DR_128SPS | ADS_COMP_OFF;
  cfg |= (uint16_t)(0x4000 | ((ch & 0x03) << 12));        // MUX 1xx = AINx vs GND
  cfg |= (uint16_t)((adsGain & 0x07) << 9);               // PGA

  Wire.beginTransmission(adsAddr);
  Wire.write(ADS_REG_CONFIG);
  Wire.write((uint8_t)(cfg >> 8));
  Wire.write((uint8_t)(cfg & 0xFF));
  if (Wire.endTransmission() != 0) return false;

  // Poll the OS bit rather than blind-delaying: a fixed delay would silently return stale data if
  // the bus is slow or the data rate is ever changed.
  unsigned long t0 = millis();
  for (;;) {
    Wire.beginTransmission(adsAddr);
    Wire.write(ADS_REG_CONFIG);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((int)adsAddr, 2) != 2) return false;
    uint16_t st = ((uint16_t)Wire.read() << 8) | Wire.read();
    if (st & ADS_OS_SINGLE) break;                        // OS reads 1 when the conversion is done
    if (millis() - t0 > 200) return false;
    delay(1);
  }

  Wire.beginTransmission(adsAddr);
  Wire.write(ADS_REG_CONVERT);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)adsAddr, 2) != 2) return false;
  raw = (int16_t)(((uint16_t)Wire.read() << 8) | Wire.read());
  return true;
}

// Average `samples` conversions on one channel and undo its divider, so the value returned is the
// voltage present at the SENSE POINT, not at the ADC pin.
bool chVolts(uint8_t ch, int samples, double ratio, double &v,
             double *vMin, double *vMax, double *code) {
  if (samples < 1) samples = 1;
  double sum = 0, lo = 1e9, hi = -1e9, rawSum = 0;
  const double lsb = PGA_FS[adsGain] / 32768.0;
  for (int i = 0; i < samples; i++) {
    int16_t raw;
    if (!adsReadRaw(ch, raw)) return false;
    double x = raw * lsb * ratio;
    sum += x; rawSum += raw;
    if (x < lo) lo = x;
    if (x > hi) hi = x;
  }
  v = sum / samples;
  if (vMin) *vMin = lo;
  if (vMax) *vMax = hi;
  if (code) *code = rawSum / samples;
  return true;
}

/* Bit-bang up to 9 SCL pulses to free a slave stuck holding SDA low, then STOP. Lifted from
 * ESP1/src/main.cpp -- same bus, same failure mode. Must run BEFORE Wire.begin(). */
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
    Serial.println(F("[I2C] bus was stuck -- recovered"));
  }
}

/* =============================================================================
 *  OUTPUT
 * ========================================================================== */
void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  h                 this help"));
  Serial.println(F("  scan              I2C bus scan (identifies every device)"));
  Serial.println(F("  r                 read both channels once"));
  Serial.println(F("  live              toggle ~2 Hz live readout"));
  Serial.println(F("  stats [n]         n samples/channel: mean/min/max/ripple"));
  Serial.println(F("  zero              NO LOAD: capture the current sensor's 0 A output"));
  Serial.println(F("  sens <mV/A>       sensitivity (ACS712-20A=100, ACS758-50B=40)"));
  Serial.println(F("  vset <volts>      1-point voltage cal against a DMM reading"));
  Serial.println(F("  divi/divv <ratio> divider in front of A0 / A1; 1.0 = direct"));
  Serial.println(F("  gain <0-5>        PGA range; addr <0x48-0x4B>; show"));
}

void printShow() {
  Serial.println(F("--- ADC ---"));
  Serial.print(F("  addr = 0x")); Serial.print(adsAddr, HEX);
  Serial.print(F("   PGA = +/-")); Serial.print(PGA_FS[adsGain], 3);
  Serial.print(F(" V   LSB = ")); Serial.print(PGA_FS[adsGain] / 32768.0 * 1e6, 2);
  Serial.println(F(" uV"));
  Serial.println(F("--- A0 current ---"));
  Serial.print(F("  divider = ")); Serial.print(divI, 4);
  Serial.print(F("   zero = ")); Serial.print(zeroV, 5); Serial.print(F(" V"));
  Serial.print(F("   sens = ")); Serial.print(sensVperA * 1000.0, 2); Serial.println(F(" mV/A"));
  Serial.print(F("  resolution = "));
  Serial.print(PGA_FS[adsGain] / 32768.0 * divI / sensVperA * 1000.0, 2); Serial.println(F(" mA/LSB"));
  Serial.println(F("--- A1 voltage ---"));
  Serial.print(F("  divider = ")); Serial.print(divV, 4);
  Serial.print(F("   full-scale at this PGA = "));
  Serial.print(PGA_FS[adsGain] * divV, 2); Serial.println(F(" V"));
  Serial.println(F("--- paste-ready ---"));
  Serial.print(F("const float ADS_DIV_I   = ")); Serial.print(divI, 5);      Serial.println(F("f;"));
  Serial.print(F("const float ADS_DIV_V   = ")); Serial.print(divV, 5);      Serial.println(F("f;"));
  Serial.print(F("const float ADS_I_ZERO  = ")); Serial.print(zeroV, 5);     Serial.println(F("f;"));
  Serial.print(F("const float ADS_I_VPERA = ")); Serial.print(sensVperA, 5); Serial.println(F("f;"));
}

void printRead() {
  double vi, vv, ci, cv;
  if (!chVolts(CH_CURRENT, 16, divI, vi, nullptr, nullptr, &ci) ||
      !chVolts(CH_VOLTAGE, 16, divV, vv, nullptr, nullptr, &cv)) {
    Serial.println(F("READ FAILED -- ADS1115 not responding (try `scan`)"));
    liveOn = false;                                       // don't spam a dead bus
    return;
  }
  double amps  = (vi - zeroV) / sensVperA;                // signed: negative = reverse current
  double watts = vv * amps;
  Serial.print(F("A0 code=")); Serial.print(ci, 0);
  Serial.print(F(" sense=")); Serial.print(vi, 4); Serial.print(F("V"));
  Serial.print(F("  |  A1 code=")); Serial.print(cv, 0);
  Serial.print(F(" sense=")); Serial.print(vv, 4); Serial.print(F("V"));
  Serial.print(F("   =>  ")); Serial.print(vv, 2); Serial.print(F(" V  "));
  Serial.print(amps, 3); Serial.print(F(" A  "));
  Serial.print(watts, 2); Serial.println(F(" W"));
}

void doScan() {
  Serial.println(F("Scanning I2C 0x08-0x77 ..."));
  int n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  found 0x")); Serial.print(a, HEX);
      if      (a >= 0x48 && a <= 0x4B) Serial.print(F("   <-- ADS1115"));
      else if (a == 0x27) Serial.print(F("   (LCD)"));
      else if (a == 0x40) Serial.print(F("   (INA226)"));
      else if (a == 0x57) Serial.print(F("   (EEPROM)"));
      else if (a == 0x68) Serial.print(F("   (DS3231 RTC)"));
      Serial.println();
      n++;
    }
  }
  if (!n) Serial.println(F("  NOTHING FOUND -- check SDA=21, SCL=22, pull-ups and 3.3 V power"));
}

void doStats(int n) {
  double vi, iLo, iHi, vv, vLo, vHi;
  if (!chVolts(CH_CURRENT, n, divI, vi, &iLo, &iHi) ||
      !chVolts(CH_VOLTAGE, n, divV, vv, &vLo, &vHi)) { Serial.println(F("READ FAILED")); return; }
  Serial.print(F("n=")); Serial.println(n);
  Serial.print(F("  A0 current: mean=")); Serial.print(vi, 5);
  Serial.print(F("V ripple=")); Serial.print((iHi - iLo) * 1000.0, 2); Serial.print(F("mVpp  -> "));
  Serial.print((vi - zeroV) / sensVperA, 3); Serial.print(F(" A +/-"));
  Serial.print((iHi - iLo) / 2.0 / sensVperA, 3); Serial.println(F(" A"));
  Serial.print(F("  A1 voltage: mean=")); Serial.print(vv, 5);
  Serial.print(F("V ripple=")); Serial.print((vHi - vLo) * 1000.0, 2); Serial.print(F("mVpp  -> "));
  Serial.print(vv, 3); Serial.println(F(" V"));
  // Ripple is the honest noise floor: anything smaller than this is not measurable on this rig.
  Serial.print(F("  -> smallest trustworthy current ~ "));
  Serial.print((iHi - iLo) / sensVperA, 3); Serial.println(F(" A"));
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

  if (cmd == "h")    { printHelp(); return; }
  if (cmd == "scan") { doScan();    return; }
  if (cmd == "r")    { printRead(); return; }
  if (cmd == "show") { printShow(); return; }
  if (cmd == "live") { liveOn = !liveOn; Serial.print(F("live=")); Serial.println(liveOn ? "ON" : "OFF"); return; }
  if (cmd == "stats") { doStats(arg.length() ? arg.toInt() : 200); return; }

  if (cmd == "zero") {
    double v, lo, hi;
    if (!chVolts(CH_CURRENT, 256, divI, v, &lo, &hi)) { Serial.println(F("READ FAILED")); return; }
    zeroV = v;
    Serial.print(F("zero captured over 256 samples: ")); Serial.print(zeroV, 5); Serial.println(F(" V"));
    Serial.print(F("  ripple was ")); Serial.print((hi - lo) * 1000.0, 2); Serial.println(F(" mVpp"));
    Serial.println(F("  (a hall sensor should sit near VCC/2; a shunt+amp near 0 V)"));
    return;
  }

  if (cmd == "sens") {
    double mv = arg.toFloat();
    if (mv <= 0.1 || mv > 1000.0) { Serial.println(F("usage: sens <mV/A>  e.g. 100 (ACS712-20A), 40 (ACS758-50B)")); return; }
    sensVperA = mv / 1000.0;
    Serial.print(F("sensitivity = ")); Serial.print(mv, 2); Serial.println(F(" mV/A"));
    return;
  }

  if (cmd == "vset") {
    // One-point divider calibration: trust the DMM, solve for the ratio actually present.
    double truth = arg.toFloat();
    if (truth <= 0.1) { Serial.println(F("usage: vset <volts>  (the TRUE voltage measured with a DMM)")); return; }
    double vAdc;
    if (!chVolts(CH_VOLTAGE, 256, 1.0, vAdc)) { Serial.println(F("READ FAILED")); return; }
    if (vAdc < 0.005) { Serial.println(F("A1 reads ~0 V -- check the divider and that the source is live")); return; }
    divV = truth / vAdc;
    Serial.print(F("A1 at the ADC = ")); Serial.print(vAdc, 5); Serial.println(F(" V"));
    Serial.print(F("divv = ")); Serial.print(divV, 5); Serial.println(F("  (calibrated against your DMM)"));
    return;
  }

  if (cmd == "divi" || cmd == "divv") {
    double x = arg.toFloat();
    if (x < 0.999 || x > 50.0) { Serial.println(F("usage: divi|divv <ratio>   (R1+R2)/R2; 1.0 = direct")); return; }
    if (cmd == "divi") divI = x; else divV = x;
    Serial.print(cmd); Serial.print(F(" = ")); Serial.println(x, 4);
    if (x < 1.5) Serial.println(F("  NOTE: <1.5 is almost no attenuation -- confirm the ADC never sees >VDD+0.3 V."));
    return;
  }

  if (cmd == "gain") {
    int g = arg.toInt();
    if (g < 0 || g > 5) { Serial.println(F("usage: gain <0-5>  0=6.144 1=4.096 2=2.048 3=1.024 4=0.512 5=0.256 V")); return; }
    adsGain = (uint8_t)g;
    Serial.print(F("PGA = +/-")); Serial.print(PGA_FS[adsGain], 3);
    Serial.print(F(" V   full-scale on A1 = ")); Serial.print(PGA_FS[adsGain] * divV, 2); Serial.println(F(" V"));
    return;
  }

  if (cmd == "addr") {
    long a = (arg.startsWith("0x") || arg.startsWith("0X")) ? strtol(arg.c_str(), nullptr, 16) : arg.toInt();
    if (a < 0x48 || a > 0x4B) { Serial.println(F("usage: addr <0x48-0x4B>  (ADDR pin: GND/VDD/SDA/SCL)")); return; }
    adsAddr = (uint8_t)a;
    Serial.print(F("addr = 0x")); Serial.print(adsAddr, HEX);
    Serial.println(adsPresent() ? F("  (responding)") : F("  (NO RESPONSE)"));
    return;
  }

  Serial.println(F("? unknown command -- type h"));
}
