/* =============================================================================
 *  SMART IRRIGATION  --  BENCH TOOL  --  ADS1115 DETECT + ACS758 CURRENT (ESP32 #1)
 * -----------------------------------------------------------------------------
 *  Standalone. No main firmware, no UART to the Nano/ESP2. Two jobs:
 *    1. DETECT  -- scan the I2C bus, confirm the ADS1115 answers at 0x48, and identify
 *                  every other device so a wiring mistake is obvious immediately.
 *    2. MEASURE -- read the ACS758 hall current sensor on A0 and show raw code, sensor
 *                  volts and amps. CURRENT ONLY: the ACS758 measures the current flowing
 *                  in a wire and says nothing about voltage, so there is no voltage
 *                  channel here. A1/A2/A3 are unused.
 *
 *  WHY THIS EXISTS: the shipping firmware read battery current from GPIO34 through an
 *  optocoupler with a fitted polynomial. Sixteen days of SD logs show that path reporting
 *  I=0.00 on all 44,303 samples, so battery %, energy Wh and the low/critical thresholds
 *  never had real data. The ADS1115 gives 125 uV/LSB against a stable internal reference
 *  instead of the ESP32's nonlinear ADC. Prove the front-end here first.
 *
 *  ############################################################################
 *  #  WIRING WARNING -- READ BEFORE POWERING UP                               #
 *  #                                                                          #
 *  #  1. POWER THE ADS1115 FROM 3.3 V, not 5 V. At VDD=5 V its logic-high     #
 *  #     threshold is 0.7 x VDD = 3.5 V, which the ESP32's 3.3 V I2C lines    #
 *  #     cannot reliably reach -- the bus then works intermittently, which is #
 *  #     far worse to debug than not working at all.                          #
 *  #                                                                          #
 *  #  2. A 2:1 DIVIDER ON A0 IS REQUIRED. The ACS758 runs on 5 V; its output  #
 *  #     rests at VCC/2 (~2.5 V) and swings 0.5..4.5 V across +/-50 A. No     #
 *  #     ADS1115 input may exceed VDD + 0.3 V (~3.6 V), so wired direct,      #
 *  #     anything above about +27 A drives the input past its limit.          #
 *  #                                                                          #
 *  #       VIOUT --[ R1 10k ]--+--[ R2 10k ]-- GND     ratio = (R1+R2)/R2     #
 *  #                           |                                              #
 *  #                           +--> ADS1115 A0                                #
 *  #                                                                          #
 *  #     With 10k/10k: zero -> 1.25 V, +/-50 A -> 0.25..2.25 V, inside the    #
 *  #     +/-4.096 V PGA and under 3.3 V. Resolution ~6 mA/LSB.                #
 *  #     MEASURE your resistors and set the real ratio with `div`.            #
 *  #                                                                          #
 *  #  3. Common ground between sensor, divider and ADS1115. 100 nF across the #
 *  #     ADS1115 supply; keep the sense lead short.                           #
 *  ############################################################################
 *
 *  SENSOR: ACS758LCB-050B -- BIDIRECTIONAL, +/-50 A, 40 mV/A, output rests at VCC/2.
 *  The sign is meaningful (charge vs discharge). If `zero` reports ~0.6 V instead of
 *  ~2.5 V (or ~1.25 V behind a 2:1 divider) you have a 050U (unidirectional, 60 mV/A)
 *  and must change `sens` + re-run `zero`.
 *
 *  PINS (match ESP1/src/main.cpp:62-63): I2C SDA=21, SCL=22.  ADS1115 at 0x48 (ADDR->GND).
 *  0x48 does not collide with the rig's LCD 0x27 / EEPROM 0x57 / RTC 0x68.
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    scan              I2C bus scan (identifies every device found)
 *    r                 read once: raw code, sensor volts, amps
 *    live              toggle ~2 Hz live readout
 *    stats [n]         n samples (default 200): mean/min/max/ripple + amps
 *    zero              NO CURRENT flowing: capture the quiescent output -> ACS_ZERO_V
 *    sens <mV/A>       sensitivity: 40 = 050B (default), 60 = 050U, 20 = 100B
 *    cal <amps>        known current from a clamp meter -> derive mV/A
 *    div <ratio>       divider ratio in front of A0; 1.0 = direct (see warning above)
 *    gain <0-5>        PGA full-scale: 0=6.144 1=4.096 2=2.048 3=1.024 4=0.512 5=0.256 V
 *    show              every setting + the paste-ready firmware constants
 *
 *  Build: PlatformIO `pio run -e esp32dev` in this folder.
 * ========================================================================== */
#include <Arduino.h>
#include <Wire.h>

#define I2C_SDA 21                      // ESP1/src/main.cpp:62
#define I2C_SCL 22                      // ESP1/src/main.cpp:63
#define ADS_ADDR 0x48                   // ADDR -> GND. Fixed: this rig has exactly one ADS1115.
#define ACS_CH   0                      // ACS758 on A0; nothing else connected

/* ---- ADS1115 registers --------------------------------------------------- */
#define ADS_REG_CONVERT  0x00
#define ADS_REG_CONFIG   0x01
#define ADS_OS_SINGLE    0x8000         // bit15: start a conversion / reads 1 when idle
#define ADS_MODE_SINGLE  0x0100         // bit8 : single-shot
#define ADS_DR_128SPS    0x0080         // bits7-5: 128 SPS -> ~7.8 ms/conversion
#define ADS_COMP_OFF     0x0003         // bits1-0: comparator disabled

uint8_t adsGain = 1;                    // index into PGA_FS; 1 = +/-4.096 V
const float PGA_FS[6] = { 6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f };

/* ---- Front end ----------------------------------------------------------- */
double divRatio  = 2.0;                 // (R1+R2)/R2 in front of A0            [MEASURE]
double zeroV     = 2.5;                 // sensor output at 0 A = VCC/2 (050B)  [MEASURE]
double sensVperA = 0.040;               // 40 mV/A = ACS758LCB-050B             [CONFIRM]

bool liveOn = false;
unsigned long lastLiveMs = 0;
String line;

/* ---- forward decls -------------------------------------------------------- */
bool   adsPresent();
bool   adsReadRaw(int16_t &raw);
bool   senseVolts(int samples, double &v, double *vMin = nullptr,
                  double *vMax = nullptr, double *code = nullptr);
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
  Serial.println(F("=== ADS1115 DETECT + ACS758 CURRENT  (ESP32 #1 bench tool) ==="));
  Serial.println(F("!! ADS1115 on 3.3 V. A0 needs a 2:1 divider -- the ACS758 swings to ~4.5 V."));
  Serial.println(F("!! Current only: this sensor measures wire current, never voltage."));
  Serial.println();

  doScan();                             // detection is the primary job -- run it unprompted

  if (!adsPresent()) {
    Serial.println(F("ADS1115 NOT FOUND at 0x48"));
    Serial.println(F("  -> check VDD/GND, SDA=21, SCL=22, and that ADDR is tied to GND."));
  } else {
    Serial.println(F("ADS1115 responding at 0x48"));
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
  Wire.beginTransmission(ADS_ADDR);
  return Wire.endTransmission() == 0;
}

// One single-shot conversion on A0. Returns false on ANY I2C fault, so a missing or unpowered ADC
// surfaces as an error rather than a plausible-looking 0 A -- which is exactly how the opto-ADC
// failure this replaces stayed hidden for sixteen days.
bool adsReadRaw(int16_t &raw) {
  uint16_t cfg = ADS_OS_SINGLE | ADS_MODE_SINGLE | ADS_DR_128SPS | ADS_COMP_OFF;
  cfg |= (uint16_t)(0x4000 | ((ACS_CH & 0x03) << 12));    // MUX 1xx = AINx vs GND
  cfg |= (uint16_t)((adsGain & 0x07) << 9);               // PGA

  Wire.beginTransmission(ADS_ADDR);
  Wire.write(ADS_REG_CONFIG);
  Wire.write((uint8_t)(cfg >> 8));
  Wire.write((uint8_t)(cfg & 0xFF));
  if (Wire.endTransmission() != 0) return false;

  // Poll the OS bit rather than blind-delaying: a fixed delay silently returns stale data if the
  // bus is slow or the data rate is ever changed.
  unsigned long t0 = millis();
  for (;;) {
    Wire.beginTransmission(ADS_ADDR);
    Wire.write(ADS_REG_CONFIG);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((int)ADS_ADDR, 2) != 2) return false;
    uint16_t st = ((uint16_t)Wire.read() << 8) | Wire.read();
    if (st & ADS_OS_SINGLE) break;                        // 1 = conversion complete
    if (millis() - t0 > 200) return false;
    delay(1);
  }

  Wire.beginTransmission(ADS_ADDR);
  Wire.write(ADS_REG_CONVERT);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)ADS_ADDR, 2) != 2) return false;
  raw = (int16_t)(((uint16_t)Wire.read() << 8) | Wire.read());
  return true;
}

// Average `samples` conversions and undo the divider, so the value returned is the voltage the
// ACS758 is actually producing at its output pin.
bool senseVolts(int samples, double &v, double *vMin, double *vMax, double *code) {
  if (samples < 1) samples = 1;
  double sum = 0, lo = 1e9, hi = -1e9, rawSum = 0;
  const double lsb = PGA_FS[adsGain] / 32768.0;
  for (int i = 0; i < samples; i++) {
    int16_t raw;
    if (!adsReadRaw(raw)) return false;
    double x = raw * lsb * divRatio;
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
  Serial.println(F("  r                 read once: code, sensor volts, amps"));
  Serial.println(F("  live              toggle ~2 Hz live readout"));
  Serial.println(F("  stats [n]         n samples: mean/min/max/ripple"));
  Serial.println(F("  zero              NO CURRENT: capture the 0 A output"));
  Serial.println(F("  sens <mV/A>       40 = 050B (default), 60 = 050U, 20 = 100B"));
  Serial.println(F("  cal <amps>        known current from a clamp meter -> derive mV/A"));
  Serial.println(F("  div <ratio>       divider in front of A0; 1.0 = direct"));
  Serial.println(F("  gain <0-5>        PGA range;  show"));
}

void printShow() {
  Serial.println(F("--- ADC ---"));
  Serial.print(F("  addr = 0x48   channel = A0   PGA = +/-"));
  Serial.print(PGA_FS[adsGain], 3);
  Serial.print(F(" V   LSB = ")); Serial.print(PGA_FS[adsGain] / 32768.0 * 1e6, 2); Serial.println(F(" uV"));
  Serial.println(F("--- ACS758 ---"));
  Serial.print(F("  divider = ")); Serial.print(divRatio, 4);
  Serial.print(F("   zero = ")); Serial.print(zeroV, 5); Serial.print(F(" V"));
  Serial.print(F("   sens = ")); Serial.print(sensVperA * 1000.0, 2); Serial.println(F(" mV/A"));
  Serial.print(F("  resolution = "));
  Serial.print(PGA_FS[adsGain] / 32768.0 * divRatio / sensVperA * 1000.0, 2); Serial.println(F(" mA/LSB"));
  Serial.print(F("  measurable span at this PGA = +/-"));
  Serial.print(PGA_FS[adsGain] * divRatio / sensVperA / 2.0, 1); Serial.println(F(" A (approx)"));
  Serial.println(F("--- paste into ESP1/src/main.cpp ---"));
  Serial.print(F("const float ACS_DIV          = ")); Serial.print(divRatio, 5);  Serial.println(F("f;"));
  Serial.print(F("const float ACS_ZERO_V       = ")); Serial.print(zeroV, 5);     Serial.println(F("f;"));
  Serial.print(F("const float ACS_SENS_V_PER_A = ")); Serial.print(sensVperA, 5); Serial.println(F("f;"));
}

void printRead() {
  double v, code;
  if (!senseVolts(16, v, nullptr, nullptr, &code)) {
    Serial.println(F("READ FAILED -- ADS1115 not responding (try `scan`)"));
    liveOn = false;                                       // don't spam a dead bus
    return;
  }
  double amps = (v - zeroV) / sensVperA;                  // signed: 050B is bidirectional
  Serial.print(F("code=")); Serial.print(code, 0);
  Serial.print(F("  Vadc=")); Serial.print(v / divRatio, 5);
  Serial.print(F("  Vsensor=")); Serial.print(v, 5);
  Serial.print(F("   =>  ")); Serial.print(amps, 3); Serial.println(F(" A"));
}

void doScan() {
  Serial.println(F("Scanning I2C 0x08-0x77 ..."));
  int n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  found 0x")); Serial.print(a, HEX);
      if      (a == 0x48) Serial.print(F("   <-- ADS1115"));
      else if (a >= 0x49 && a <= 0x4B) Serial.print(F("   <-- ADS1115 (wrong ADDR strap: expect 0x48)"));
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
  double v, lo, hi;
  if (!senseVolts(n, v, &lo, &hi)) { Serial.println(F("READ FAILED")); return; }
  Serial.print(F("n=")); Serial.print(n);
  Serial.print(F("  mean=")); Serial.print(v, 5); Serial.print(F(" V"));
  Serial.print(F("  ripple=")); Serial.print((hi - lo) * 1000.0, 2); Serial.println(F(" mVpp"));
  Serial.print(F("  amps = ")); Serial.print((v - zeroV) / sensVperA, 3);
  Serial.print(F("  +/-")); Serial.print((hi - lo) / 2.0 / sensVperA, 3); Serial.println(F(" A peak"));
  // Ripple is the honest noise floor: any current below this is not measurable on this rig.
  Serial.print(F("  -> smallest trustworthy current ~ "));
  Serial.print((hi - lo) / sensVperA, 3); Serial.println(F(" A"));
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
    if (!senseVolts(256, v, &lo, &hi)) { Serial.println(F("READ FAILED")); return; }
    zeroV = v;
    Serial.print(F("zero captured over 256 samples: ")); Serial.print(zeroV, 5); Serial.println(F(" V"));
    Serial.print(F("  ripple was ")); Serial.print((hi - lo) * 1000.0, 2); Serial.println(F(" mVpp"));
    // The zero point identifies the variant, so say so rather than let a 050U masquerade as a 050B.
    double vcc = zeroV * 2.0;
    if (zeroV > 2.0 && zeroV < 3.0)
      Serial.println(F("  ~VCC/2 -> 050B bidirectional, as expected (divider ratio already applied)"));
    else if (zeroV > 0.4 && zeroV < 0.9)
      Serial.println(F("  ~0.6 V -> this looks like a 050U (UNIdirectional): set `sens 60`"));
    else {
      Serial.print(F("  WARNING: expected ~2.5 V (050B) or ~0.6 V (050U). Implied VCC = "));
      Serial.print(vcc, 2); Serial.println(F(" V -- check the divider ratio (`div`) and the 5 V rail."));
    }
    return;
  }

  if (cmd == "sens") {
    double mv = arg.toFloat();
    if (mv <= 0.1 || mv > 1000.0) { Serial.println(F("usage: sens <mV/A>  40=050B  60=050U  20=100B")); return; }
    sensVperA = mv / 1000.0;
    Serial.print(F("sensitivity = ")); Serial.print(mv, 2); Serial.println(F(" mV/A"));
    return;
  }

  if (cmd == "cal") {
    // Two-point-ish: zero is already captured, so one known current gives the real mV/A.
    double amps = arg.toFloat();
    if (fabs(amps) < 0.05) { Serial.println(F("usage: cal <amps>   (a KNOWN current from a clamp meter)")); return; }
    double v;
    if (!senseVolts(256, v)) { Serial.println(F("READ FAILED")); return; }
    double delta = v - zeroV;
    if (fabs(delta) < 0.002) { Serial.println(F("output barely moved (<2 mV) -- is current actually flowing?")); return; }
    sensVperA = fabs(delta) / fabs(amps);
    Serial.print(F("delta = ")); Serial.print(delta * 1000.0, 2); Serial.print(F(" mV at "));
    Serial.print(amps, 3); Serial.println(F(" A"));
    Serial.print(F("sensitivity = ")); Serial.print(sensVperA * 1000.0, 2); Serial.println(F(" mV/A"));
    if (delta < 0) Serial.println(F("  (negative delta: the wire runs the other way through the sensor)"));
    return;
  }

  if (cmd == "div") {
    double x = arg.toFloat();
    if (x < 0.999 || x > 20.0) { Serial.println(F("usage: div <ratio>   (R1+R2)/R2; 2.0 for 10k/10k, 1.0 = direct")); return; }
    divRatio = x;
    Serial.print(F("divider ratio = ")); Serial.println(divRatio, 4);
    if (x < 1.5) Serial.println(F("  DANGER: <1.5 is almost no attenuation. The ACS758 swings to ~4.5 V; the ADS1115 dies above VDD+0.3 V."));
    return;
  }

  if (cmd == "gain") {
    int g = arg.toInt();
    if (g < 0 || g > 5) { Serial.println(F("usage: gain <0-5>  0=6.144 1=4.096 2=2.048 3=1.024 4=0.512 5=0.256 V")); return; }
    adsGain = (uint8_t)g;
    Serial.print(F("PGA = +/-")); Serial.print(PGA_FS[adsGain], 3); Serial.println(F(" V"));
    double maxIn = 5.0 / divRatio;                        // worst case the sensor can present
    if (maxIn > PGA_FS[adsGain]) {
      Serial.print(F("  WARNING: the divided signal can reach ")); Serial.print(maxIn, 3);
      Serial.println(F(" V, beyond this PGA range -- high currents will clip. Use a lower gain index."));
    }
    return;
  }

  Serial.println(F("? unknown command -- type h"));
}
