/*
  SUPERSEDED by Calibration/ESP1/INA226, which does this and adds the secondary battery and saving to
  ESP1. Kept because it runs on an Uno bench with nothing else connected and no ESP32 at all.

  Solar + main battery INA226 (CJMCU-226) detect, read + calibrate, for the rig's two isolated modules
  ------------------------------------------------------------------
  Finds both modules on one I2C bus at two different addresses, proves each is really an INA226
  from its ID registers, reads both side by side, and calibrates each against your meters. Only Wire
  is used. Same addresses, scaling and sign as ESP1/src/main.cpp, so a module that reads right here
  reads right there.

  CALIBRATING (type + Enter; <module> is solar or batt). Do it in this order, per module:
    1. zero <module>           No current through the shunt: captures the no-load offset.
    2. amps <module> <A>       Steady known current, from a clamp meter: trims CAL. Positive = discharging
                               (batt) or producing (solar). Use at least 0.2 A on solar, 1.5 A on batt.
    3. volts <module> <V>      Multimeter across the module's VBS and GND: trims the bus voltage.
    4. show                    Prints the values to paste into ESP1/src/main.cpp and the table below.
    invert <module>            Swaps the current's sign, if it reads backwards.
    Also: h help, r read once, live on/off, scan.
  Nothing is stored on the board: a reboot goes back to the MODULES table, so paste what `show` prints.

  SHUNTS
    SOLAR    R010 on the module. Full scale 8.19 A. Keep panel Voc under 36 V.
    BATTERY  50 A / 75 mV shunt bar (1.5 mOhm). Full scale 54.6 A. REMOVE the module's own R010, or it
             sits in parallel with the bar: readings come out ~13 % low and it carries ~6.5 A at 50 A.

  WIRING ON THE RIG (through the ISO1540)
    ESP32 side:   SDA2 -> GPIO21   SCL2 -> GPIO22   VCC2 -> the voltage ESP1's bus is pulled up to
                  GND2 -> board GND                 100 nF on VCC2
    Module side:  SDA1 / SCL1 -> both modules' SDA / SCL   VCC1 -> 3.3 V from the main-battery side
                  GND1 -> main battery negative     100 nF on VCC1
    Modules:      VCC -> that same 3.3 V   GND -> main battery negative
    Side 2 faces the ESP32 on purpose. Side 1 pulls low to only 0.57-0.8 V, and an ESP32 needs below
    0.825 V at 3.3 V, so that way round leaves almost no margin. Side 2 reaches 0.4 V. Side 1's pull-ups
    must sink 0.5-3.5 mA: the two modules' 10 k pull-ups in parallel (0.66 mA at 3.3 V) are enough.
    Current sensing is high side: IN+ / IN- in series with the + line, VBS tied to IN-.
    Stay under the INA226's 36 V limit.

  ON THE BENCH (no isolator): SDA -> GPIO21 (Uno A4), SCL -> GPIO22 (Uno A5), VCC -> 3.3V (Uno 5V),
    GND -> GND, with the load's ground common to the board's.

  SETTING THE ADDRESS (set on the module, by its A0 / A1 pins or solder pads)
    A1 to GND, A0 to VCC  = 0x41  <- SOLAR
    A1 to VCC, A0 to GND  = 0x44  <- BATTERY
    The chip's address pins are named A0 and A1. If yours are marked differently and the two names
    come out swapped (unplug SOLAR: BATTERY goes LOST), swap the two addresses in the MODULES table,
    and in ESP1's SOLAR_INA_ADDR / MAIN_INA_ADDR.
    Two modules left on the same address both answer and corrupt each other's replies.
    To try other addresses, change only the MODULES table below (VS = the module's VCC):

      A1\A0   GND    VS    SDA   SCL
      GND     0x40  0x41  0x42  0x43
      VS      0x44  0x45  0x46  0x47
      SDA     0x48  0x49  0x4A  0x4B     0x48 is the ADS1115 on ESP1's bus: avoid it there
      SCL     0x4C  0x4D  0x4E  0x4F

  PLATFORMIO
    pio run -t upload                  ESP32 (default)
    pio run -e uno -t upload           Arduino Uno
    pio device monitor                 115200 baud

  Unplugging a module is reported as LOST, and plugging it back in as DETECTED, so each
  module can be identified by hand.
*/

#include <Arduino.h>
#include <Wire.h>

// ---------------- Settings ----------------
struct Module {
  const char *name;
  const char *espPrefix;   // this module's constants in ESP1/src/main.cpp: <prefix>_SHUNT_OHM and so on
  uint8_t  addr;
  uint16_t cal;        // 0.00512 / (currentLsb x shunt ohms). `amps` trims it        [MEASURE per module]
  float    currentLsb; // amps per bit
  float    offsetA;    // no-load reading, subtracted. `zero` measures it             [MEASURE per module]
  float    busFactor;  // meter V / INA V. `volts` measures it                        [MEASURE per module]
  bool     invert;     // IN+/IN- wired reversed. Applied before the offset, as in ESP1
  bool     ready;
};

Module MODULES[] = {
  { "SOLAR", "SOLAR", 0x41, 2048, 0.00025f, 0.0f, 1.000f, false, false },   // R010: 0.25 mA/bit, 8.19 A full scale
  { "BATT",  "MAIN",  0x44, 1707, 0.002f,   0.0f, 1.000f, false, false },   // 50 A / 75 mV: 2 mA/bit, 54.6 A full scale
};
const uint8_t MODULE_COUNT = sizeof(MODULES) / sizeof(MODULES[0]);

// Calibration limits. Each refusal is a wrong setup that would otherwise be saved as a calibration.
const uint8_t CAL_SAMPLES     = 32;      // averaged for each measurement, about 1.3 s
const uint8_t CAL_CONV_MS     = 40;      // one AVG16 conversion takes 35.2 ms, so every sample is new
const float   ZERO_MAX_FS     = 0.02f;   // `zero` refused above 2 % of full scale: current is still flowing
const float   AMPS_MIN_LSBS   = 500.0f;  // `amps` refused below this reading: too small to trim from
const float   AMPS_RATIO_MIN  = 0.5f;    // `amps` refused beyond these: a wrong shunt or a mistyped number
const float   AMPS_RATIO_MAX  = 2.0f;
const float   VOLTS_MIN       = 1.0f;    // `volts` refused below this: no bus voltage to trim from
const float   VOLTS_RATIO_MAX = 1.10f;   // `volts` refused beyond +/-10 %: the chip is good to about 0.1 %
const int16_t SHUNT_CLIP_RAW  = 32000;   // of +/-32767 (81.92 mV): the current register is no longer linear

const uint16_t CONFIG_VAL  = 0x4527;          // AVG16, 1.1 ms bus/shunt, continuous
const uint16_t MFG_ID      = 0x5449;          // "TI"
const uint16_t DIE_ID      = 0x2260;          // INA226
const unsigned long READ_MS  = 1000;
const unsigned long RETRY_MS = 3000;

// ---------------- Registers ----------------
#define REG_CONFIG   0x00
#define REG_SHUNT_V  0x01
#define REG_BUS_V    0x02
#define REG_CURRENT  0x04
#define REG_CAL      0x05
#define REG_MFG_ID   0xFE
#define REG_DIE_ID   0xFF

uint8_t lastErr = 0;
unsigned long lastReadMs = 0, lastRetryMs = 0;
bool liveOn = true;
char cmdLine[48];
uint8_t cmdLen = 0;

// ---------------- I2C helpers ----------------
const char *i2cErrText(uint8_t e) {
  switch (e) {
    case 0:  return "OK";
    case 2:  return "no ACK on address";
    case 3:  return "no ACK on data";
    case 4:  return "bus error (SDA/SCL shorted or stuck?)";
    case 5:  return "bus timeout (SDA/SCL held low?)";
    case 10: return "short read";
    default: return "I2C error";
  }
}

void printHex(uint8_t a) {
  Serial.print(F("0x"));
  if (a < 16) Serial.print('0');
  Serial.print(a, HEX);
}

bool present(uint8_t addr) {
  Wire.beginTransmission(addr);
  lastErr = Wire.endTransmission();
  return lastErr == 0;
}

bool writeReg(uint8_t addr, uint8_t reg, uint16_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write((uint8_t)(val >> 8));
  Wire.write((uint8_t)(val & 0xFF));
  lastErr = Wire.endTransmission();
  return lastErr == 0;
}

bool readReg(uint8_t addr, uint8_t reg, uint16_t &out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  lastErr = Wire.endTransmission(false);
  if (lastErr != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)2) != 2) {
    lastErr = 10;
    while (Wire.available()) Wire.read();
    return false;
  }
  uint16_t hi = Wire.read();
  out = (hi << 8) | (uint16_t)Wire.read();
  return true;
}

bool isIna226(uint8_t addr) {
  uint16_t mfg, die;
  return readReg(addr, REG_MFG_ID, mfg) && readReg(addr, REG_DIE_ID, die) &&
         mfg == MFG_ID && die == DIE_ID;
}

const Module *moduleAt(uint8_t addr) {
  for (uint8_t i = 0; i < MODULE_COUNT; i++) if (MODULES[i].addr == addr) return &MODULES[i];
  return NULL;
}

// ---------------- Detection ----------------
// Every device on the bus, naming the INA226s, so a wrong address or a wiring fault is obvious.
void scanBus() {
  uint8_t found = 0;
  Serial.println(F("Scanning I2C bus..."));
  for (uint8_t a = 1; a < 127; a++) {
    bool answered = present(a);
    if (lastErr == 4 || lastErr == 5) {
      Serial.print(F("  Bus fault: ")); Serial.println(i2cErrText(lastErr));
      return;
    }
    if (!answered) continue;
    found++;
    Serial.print(F("  ")); printHex(a);
    bool ina = isIna226(a);
    const Module *m = moduleAt(a);
    Serial.print(ina ? F("  INA226") : F("  other device"));
    if (m) { Serial.print(F("  <- module ")); Serial.print(m->name); }
    else if (ina) Serial.print(F("  <- not in the MODULES table"));
    Serial.println();
  }
  if (!found) Serial.println(F("  Nothing answered: check SDA/SCL, VCC and GND."));
}

// Reset, configure and read back one module. Reports the exact reason on failure.
bool initModule(Module &m) {
  if (!present(m.addr)) return false;
  uint16_t mfg = 0, die = 0;
  if (!readReg(m.addr, REG_MFG_ID, mfg) || !readReg(m.addr, REG_DIE_ID, die)) {
    Serial.print(F("  Module ")); Serial.print(m.name); Serial.print(F(" ID read failed: "));
    Serial.println(i2cErrText(lastErr));
    return false;
  }
  if (mfg != MFG_ID || die != DIE_ID) {
    Serial.print(F("  Module ")); Serial.print(m.name); Serial.print(F(": "));
    printHex(m.addr); Serial.print(F(" is not an INA226 (MFG=0x")); Serial.print(mfg, HEX);
    Serial.print(F(" DIE=0x")); Serial.print(die, HEX); Serial.println(')');
    return false;
  }
  writeReg(m.addr, REG_CONFIG, 0x8000);        // reset
  delay(5);
  uint16_t back = 0;
  if (!writeReg(m.addr, REG_CONFIG, CONFIG_VAL) || !writeReg(m.addr, REG_CAL, m.cal) ||
      !readReg(m.addr, REG_CAL, back) || back != m.cal) {
    Serial.print(F("  Module ")); Serial.print(m.name);
    Serial.println(F(" config failed (two modules on one address, or an unstable bus?)"));
    return false;
  }
  return true;
}

void printStatus() {
  Serial.print(F("DETECT:"));
  for (uint8_t i = 0; i < MODULE_COUNT; i++) {
    Serial.print(F("  ")); Serial.print(MODULES[i].name); Serial.print(' ');
    printHex(MODULES[i].addr);
    Serial.print(MODULES[i].ready ? F(" OK") : F(" MISSING"));
  }
  Serial.println();
}

void printMissingHints() {
  Serial.println(F("  Check that module's A0/A1 links (SOLAR 0x41: A1 GND, A0 VCC. BATT 0x44: A1 VCC, A0 GND),"));
  Serial.println(F("  that no address pin is left floating, then its SDA/SCL/VCC/GND."));
  Serial.println(F("  Through the ISO1540: both sides powered, GND1 on main negative, side 2 on the ESP32."));
}

void tryMissingModules() {
  bool changed = false, missing = false;
  for (uint8_t i = 0; i < MODULE_COUNT; i++) {
    Module &m = MODULES[i];
    if (m.ready) continue;
    m.ready = initModule(m);
    if (m.ready) {
      Serial.print(F("Module ")); Serial.print(m.name); Serial.print(F(" DETECTED at "));
      printHex(m.addr); Serial.println();
      changed = true;
    } else missing = true;
  }
  if (changed || missing) printStatus();
  if (missing) { scanBus(); printMissingHints(); }
  else if (changed) Serial.println(F("All modules detected.\nMod\tBus V\tShunt mV\tCurrent A\tPower W"));
}

// ---------------- Reading ----------------
// Before the offset, same order as ESP1's inaAmps().
float signedAmps(const Module &m, int16_t raw) {
  float a = raw * m.currentLsb;
  return m.invert ? -a : a;
}

void readModule(Module &m) {
  uint16_t rShunt, rBus, rCurr;
  if (!readReg(m.addr, REG_SHUNT_V, rShunt) || !readReg(m.addr, REG_BUS_V, rBus) ||
      !readReg(m.addr, REG_CURRENT, rCurr)) {
    Serial.print(F("Module ")); Serial.print(m.name); Serial.print(F(" LOST: "));
    Serial.println(i2cErrText(lastErr));
    m.ready = false;
    return;
  }
  float shunt_mV  = (int16_t)rShunt * 0.0025f;              // 2.5 uV/bit, signed
  float bus_V     = rBus * 0.00125f * m.busFactor;          // 1.25 mV/bit
  float current_A = signedAmps(m, (int16_t)rCurr) - m.offsetA;

  Serial.print(m.name);        Serial.print('\t');
  Serial.print(bus_V, 3);      Serial.print('\t');
  Serial.print(shunt_mV, 4);   Serial.print(F("\t\t"));
  Serial.print(current_A, 4);  Serial.print(F("\t\t"));
  Serial.println(bus_V * current_A, 3);
}

// ---------------- Calibration ----------------
float fullScaleA(const Module &m) { return 16.0f * m.cal * m.currentLsb; }   // 81.92 mV / shunt ohms
float shuntOhms(const Module &m)  { return 0.00512f / (m.cal * m.currentLsb); }

struct Sample {
  float amps;    // mean, before the offset
  float busV;    // mean, before busFactor
  float spreadA; // highest - lowest
  bool  clipped; // the shunt voltage reached the chip's limit
};

// Reports LOST and returns false on any I2C failure, so a dead module can never be calibrated from zeros.
bool takeSample(Module &m, Sample &s) {
  float sumA = 0, sumV = 0, lo = 0, hi = 0;
  s.clipped = false;
  for (uint8_t n = 0; n < CAL_SAMPLES; n++) {
    delay(CAL_CONV_MS);                        // also lets a just-written CAL reach the current register
    uint16_t rShunt, rBus, rCurr;
    if (!readReg(m.addr, REG_SHUNT_V, rShunt) || !readReg(m.addr, REG_BUS_V, rBus) ||
        !readReg(m.addr, REG_CURRENT, rCurr)) {
      Serial.print(F("Module ")); Serial.print(m.name); Serial.print(F(" LOST: "));
      Serial.println(i2cErrText(lastErr));
      m.ready = false;
      return false;
    }
    float a = signedAmps(m, (int16_t)rCurr);
    if (n == 0 || a < lo) lo = a;
    if (n == 0 || a > hi) hi = a;
    sumA += a;
    sumV += rBus * 0.00125f;
    int16_t sh = (int16_t)rShunt;
    if (sh >= SHUNT_CLIP_RAW || sh <= -SHUNT_CLIP_RAW) s.clipped = true;
  }
  s.amps = sumA / CAL_SAMPLES;
  s.busV = sumV / CAL_SAMPLES;
  s.spreadA = hi - lo;
  return true;
}

void printPaste(const Module &m) {
  Serial.print(F("// ESP1/src/main.cpp\n"));
  Serial.print(F("const float    ")); Serial.print(m.espPrefix); Serial.print(F("_SHUNT_OHM  = "));
  Serial.print(shuntOhms(m), 8); Serial.println(F("f;"));
  Serial.print(F("const float    ")); Serial.print(m.espPrefix); Serial.print(F("_OFFSET_A   = "));
  Serial.print(m.offsetA, 4); Serial.println(F("f;"));
  Serial.print(F("const float    ")); Serial.print(m.espPrefix); Serial.print(F("_BUS_FACTOR = "));
  Serial.print(m.busFactor, 4); Serial.println(F("f;"));
  Serial.print(F("const bool     ")); Serial.print(m.espPrefix); Serial.print(F("_INVERT     = "));
  Serial.println(m.invert ? F("true;") : F("false;"));
  Serial.print(F("// this sketch's MODULES table\n{ \"")); Serial.print(m.name);
  Serial.print(F("\", \"")); Serial.print(m.espPrefix); Serial.print(F("\", "));
  printHex(m.addr); Serial.print(F(", ")); Serial.print(m.cal); Serial.print(F(", "));
  Serial.print(m.currentLsb, 5); Serial.print(F("f, ")); Serial.print(m.offsetA, 4); Serial.print(F("f, "));
  Serial.print(m.busFactor, 4); Serial.print(F("f, ")); Serial.print(m.invert ? F("true") : F("false"));
  Serial.println(F(", false },"));
}

void cmdZero(Module &m) {
  Serial.print(F("Measuring ")); Serial.print(m.name); Serial.println(F(" with no current..."));
  Sample s;
  if (!takeSample(m, s)) return;
  if (fabs(s.amps) > ZERO_MAX_FS * fullScaleA(m)) {
    Serial.print(F("REFUSED: it reads ")); Serial.print(s.amps, 3);
    Serial.println(F(" A, which is real current, not an offset. Stop all current through the shunt first."));
    return;
  }
  m.offsetA = s.amps;
  Serial.print(F("Offset = ")); Serial.print(m.offsetA, 4); Serial.println(F(" A. Next: amps"));
  printPaste(m);
}

void cmdAmps(Module &m, float meterA) {
  if (meterA == 0.0f) { Serial.println(F("usage: amps <module> <amps from your meter>")); return; }
  Serial.print(F("Measuring ")); Serial.print(m.name); Serial.println(F(": hold the current steady..."));
  Sample s;
  if (!takeSample(m, s)) return;
  float reading = s.amps - m.offsetA;
  if (s.clipped) {
    Serial.print(F("REFUSED: the shunt is at the chip's limit (about ")); Serial.print(fullScaleA(m), 1);
    Serial.println(F(" A). Use a smaller current."));
    return;
  }
  if (fabs(reading) < AMPS_MIN_LSBS * m.currentLsb) {
    Serial.print(F("REFUSED: it reads only ")); Serial.print(reading, 3); Serial.print(F(" A. Use at least "));
    Serial.print(AMPS_MIN_LSBS * m.currentLsb, 2); Serial.println(F(" A."));
    return;
  }
  if ((reading > 0) != (meterA > 0)) {
    Serial.print(F("REFUSED: it reads ")); Serial.print(reading, 3);
    Serial.println(F(" A, the opposite direction to your meter. Run invert if the leads are reversed."));
    return;
  }
  float ratio = meterA / reading;
  if (ratio < AMPS_RATIO_MIN || ratio > AMPS_RATIO_MAX) {
    Serial.print(F("REFUSED: it reads ")); Serial.print(reading, 3); Serial.print(F(" A against your "));
    Serial.print(meterA, 3); Serial.println(F(" A. Too far apart to be a trim: check the shunt and the number."));
    return;
  }
  float newCalF = m.cal * ratio + 0.5f;
  if (newCalF < 1.0f || newCalF > 32767.0f) {
    Serial.println(F("REFUSED: that CAL does not fit the register."));
    return;
  }
  uint16_t oldCal = m.cal, newCal = (uint16_t)newCalF, back = 0;
  if (!writeReg(m.addr, REG_CAL, newCal) || !readReg(m.addr, REG_CAL, back) || back != newCal) {
    writeReg(m.addr, REG_CAL, oldCal);
    Serial.print(F("FAILED: CAL did not read back: ")); Serial.println(i2cErrText(lastErr));
    return;
  }
  m.cal = newCal;
  m.offsetA *= (float)newCal / oldCal;         // the offset comes through the same CAL
  if (s.spreadA > 0.05f * fabs(reading)) {
    Serial.print(F("WARNING: the current moved by ")); Serial.print(s.spreadA, 3);
    Serial.println(F(" A during the measurement. Repeat with a steadier load."));
  }
  Serial.print(F("CAL ")); Serial.print(oldCal); Serial.print(F(" -> ")); Serial.print(newCal);
  Serial.print(F(", shunt = ")); Serial.print(shuntOhms(m) * 1000.0f, 4); Serial.println(F(" mOhm"));
  // Read back through the new CAL. This is the check that the trim did what it was meant to.
  Sample after;
  if (!takeSample(m, after)) return;
  Serial.print(F("Now reads ")); Serial.print(after.amps - m.offsetA, 3); Serial.print(F(" A against your "));
  Serial.print(meterA, 3); Serial.println(F(" A"));
  if (ratio > 1.08f && m.currentLsb > 0.001f)  // the bar with the module's R010 still in parallel reads ~13 % low
    Serial.println(F("WARNING: this module read over 8 % low. Check its onboard R010 was removed."));
  printPaste(m);
}

void cmdVolts(Module &m, float meterV) {
  if (meterV <= 0.0f) { Serial.println(F("usage: volts <module> <volts from your meter>")); return; }
  Serial.print(F("Measuring ")); Serial.print(m.name); Serial.println(F(" bus voltage..."));
  Sample s;
  if (!takeSample(m, s)) return;
  if (s.busV < VOLTS_MIN) {
    Serial.print(F("REFUSED: it reads only ")); Serial.print(s.busV, 3);
    Serial.println(F(" V. Check the VBS lead. (A solar line at night has nothing to trim from.)"));
    return;
  }
  float ratio = meterV / s.busV;
  if (ratio > VOLTS_RATIO_MAX || ratio < 1.0f / VOLTS_RATIO_MAX) {
    Serial.print(F("REFUSED: it reads ")); Serial.print(s.busV, 3); Serial.print(F(" V against your "));
    Serial.print(meterV, 3); Serial.println(F(" V. Measure across the module's VBS and GND."));
    return;
  }
  m.busFactor = ratio;
  Serial.print(F("Bus factor = ")); Serial.print(m.busFactor, 4); Serial.print(F(", now reads "));
  Serial.print(s.busV * m.busFactor, 3); Serial.println(F(" V"));
  printPaste(m);
}

void cmdInvert(Module &m) {
  m.invert = !m.invert;
  m.offsetA = -m.offsetA;                      // the offset is stored after the sign, so it flips too
  Serial.print(m.name); Serial.print(F(" invert = ")); Serial.println(m.invert ? F("true") : F("false"));
}

void cmdShow() {
  for (uint8_t i = 0; i < MODULE_COUNT; i++) {
    const Module &m = MODULES[i];
    Serial.print(F("--- ")); Serial.print(m.name); Serial.print(' '); printHex(m.addr);
    Serial.print(m.ready ? F(" OK") : F(" MISSING"));
    Serial.print(F(", full scale ")); Serial.print(fullScaleA(m), 2); Serial.print(F(" A, shunt "));
    Serial.print(shuntOhms(m) * 1000.0f, 4); Serial.println(F(" mOhm ---"));
    printPaste(m);
  }
}

void printHelp() {
  Serial.println(F("Commands (<module> = solar or batt):"));
  Serial.println(F("  zero <module>        no current through the shunt: measure the offset"));
  Serial.println(F("  amps <module> <A>    steady known current, from a clamp meter: trim CAL"));
  Serial.println(F("  volts <module> <V>   meter across VBS and GND: trim the bus voltage"));
  Serial.println(F("  invert <module>      swap the current's sign"));
  Serial.println(F("  show                 values to paste into ESP1 and this sketch"));
  Serial.println(F("  r                    read once"));
  Serial.println(F("  live                 live readout on/off"));
  Serial.println(F("  scan                 list the I2C bus"));
  Serial.println(F("Order: zero, amps, volts, show. A reboot loses anything not pasted."));
}

// "b", "batt", "battery", "main" and "m" all name the battery module; likewise for solar.
Module *findModule(const char *s) {
  if (!s) return NULL;
  for (uint8_t i = 0; i < MODULE_COUNT; i++) {
    Module &m = MODULES[i];
    size_t n = strlen(s), nameLen = strlen(m.name);
    if (!strncasecmp(s, m.name, n) || !strncasecmp(s, m.espPrefix, n)) return &m;   // s is a prefix
    if (n > nameLen && !strncasecmp(s, m.name, nameLen)) return &m;                 // "battery"
  }
  return NULL;
}

void handleCommand(char *line) {
  char *cmd = strtok(line, " ,\t");
  if (!cmd) return;
  if (!strcasecmp(cmd, "h") || !strcasecmp(cmd, "help")) { printHelp(); return; }
  if (!strcasecmp(cmd, "show")) { cmdShow(); return; }
  if (!strcasecmp(cmd, "scan")) { scanBus(); return; }
  if (!strcasecmp(cmd, "live")) {
    liveOn = !liveOn;
    Serial.print(F("live = ")); Serial.println(liveOn ? F("ON") : F("OFF"));
    return;
  }
  if (!strcasecmp(cmd, "r")) {
    for (uint8_t i = 0; i < MODULE_COUNT; i++) if (MODULES[i].ready) readModule(MODULES[i]);
    return;
  }
  bool isZero = !strcasecmp(cmd, "zero"), isAmps = !strcasecmp(cmd, "amps"),
       isVolts = !strcasecmp(cmd, "volts"), isInvert = !strcasecmp(cmd, "invert");
  if (!isZero && !isAmps && !isVolts && !isInvert) { Serial.println(F("Unknown command: type h")); return; }

  Module *m = findModule(strtok(NULL, " ,\t"));
  if (!m) { Serial.println(F("Name a module: solar or batt")); return; }
  if (!m->ready) { Serial.print(F("Module ")); Serial.print(m->name); Serial.println(F(" is not detected.")); return; }
  const char *val = strtok(NULL, " ,\t");
  float number = val ? (float)atof(val) : 0.0f;   // atof gives 0 for junk, which amps and volts refuse
  if      (isZero)  cmdZero(*m);
  else if (isAmps)  cmdAmps(*m, number);
  else if (isVolts) cmdVolts(*m, number);
  else              cmdInvert(*m);
  lastReadMs = millis();                       // a measurement blocks: don't print a stale line right after
}

// Echoes what is typed (a serial monitor may not). Live output waits, so it can't split a half-typed line.
void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen == 0) continue;
      Serial.println();
      cmdLine[cmdLen] = '\0';
      cmdLen = 0;
      handleCommand(cmdLine);
    } else if (c == 8 || c == 127) {
      if (cmdLen > 0) { cmdLen--; Serial.print(F("\b \b")); }
    } else if (cmdLen < sizeof(cmdLine) - 1) {
      cmdLine[cmdLen++] = c;
      Serial.print(c);
    }
  }
}

// ---------------- Main ----------------
void setup() {
  Serial.begin(115200);
  delay(1500);

#if defined(ESP32)
  Wire.begin(21, 22);
  Wire.setTimeOut(50);                       // bound a stuck bus
#else
  Wire.begin();
  Wire.setWireTimeout(25000, true);          // 25 ms: a stuck bus cannot freeze the sketch
#endif

  Serial.println(F("\n=== Solar + battery INA226 test + calibration (CJMCU-226) ==="));
  printHelp();
  tryMissingModules();
  lastRetryMs = lastReadMs = millis();
}

void loop() {
  pollSerial();
  if (cmdLen > 0) return;                      // mid-typing: hold the output until Enter
  unsigned long now = millis();

  bool anyMissing = false;
  for (uint8_t i = 0; i < MODULE_COUNT; i++) if (!MODULES[i].ready) anyMissing = true;
  if (anyMissing && now - lastRetryMs >= RETRY_MS) {
    lastRetryMs = now;
    tryMissingModules();
  }

  if (liveOn && now - lastReadMs >= READ_MS) {
    lastReadMs = now;
    for (uint8_t i = 0; i < MODULE_COUNT; i++) if (MODULES[i].ready) readModule(MODULES[i]);
  }
}
