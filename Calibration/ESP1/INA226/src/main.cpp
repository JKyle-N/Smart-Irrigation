/* =============================================================================
 *  SMART IRRIGATION -- CALIBRATION TOOL -- INA226 POWER MONITORS (ESP32 #1)
 * -----------------------------------------------------------------------------
 *  Calibrates the three INA226 modules on ESP1's I2C bus against your meters:
 *    solar      0x41  behind the ISO1540   R010 on the module         full scale 8.19 A
 *    batt       0x44  behind the ISO1540   50 A / 75 mV bar, 1.5 mOhm full scale 54.6 A
 *    sec        0x40  on the boards' side  R010 on the module         full scale 8.19 A
 *  Same addresses, scaling and sign as ESP1/src/main.cpp, so a module that reads right here reads right there.
 *
 *  HOW TO USE (type + Enter, <m> = solar, batt or sec). The tool tells you the next step each time.
 *    1. zero <m>        No current through the shunt: measures the offset.
 *    2. amps <m> <A>    Steady known current from a clamp meter: trims the shunt value.
 *                       Positive = discharging (batt, sec) or producing (solar).
 *    3. volts <m> <V>   Multimeter across the module's VBS and GND: trims the bus voltage.
 *    4. save <m>        Stores the result in this board's NVS. Flash the main firmware back and it loads it.
 *    Also: invert <m> (current reads backwards), show (everything + paste lines), r, live, scan,
 *          bustest (is the bus through the isolator reliable?), defaults <m> (forget a module's saved trims).
 *
 *  THE ISOLATOR IS AN ISO1540 (a bidirectional I2C isolator). A part marked ISO1450 is an RS-485 transceiver
 *  and will not pass I2C at all: check the marking if nothing answers behind it.
 *
 *  WIRING THROUGH THE ISO1540 (side 2 faces the ESP32 on purpose, see reference sec.21.0)
 *    Side 2:  SDA2 -> GPIO21  SCL2 -> GPIO22  VCC2 -> the voltage ESP1's bus is pulled up to  GND2 -> board GND
 *    Side 1:  SDA1/SCL1 -> both modules  VCC1 + module VCC -> 3.3 V from the main-battery side
 *             GND1 + module GND -> main battery negative.   100 nF on each VCC.
 *    Current sensing is high side: IN+/IN- in series with the + line, VBS tied to IN-. Stay under 36 V.
 *    Main battery module: its onboard R010 must be REMOVED, or it parallels the bar (reads ~13 % low).
 *    Addresses: A1 GND + A0 VCC = 0x41, A1 VCC + A0 GND = 0x44, both GND = 0x40.
 *
 *  BENCH WITHOUT THE ISOLATOR: same SDA/SCL (Uno: A4/A5), VCC 3.3 V (Uno: 5 V), load ground common with the board.
 * ========================================================================== */
#include <Arduino.h>
#include <Wire.h>
#if defined(ESP32)
#include <Preferences.h>
#endif

struct Module {
  const char *name;       // what you type
  const char *paste;      // this module's constants in ESP1/src/main.cpp: <paste>_SHUNT_OHM and so on
  const char *nvs;        // NVS key prefix ESP1's loadCal reads: <nvs>O, A, B, I
  uint8_t  addr;
  float    nominalOhm;    // the shunt as fitted
  float    lsb;           // amps per bit: the same current LSB ESP1 uses for this module
  float    minV;          // a live bus reads above this. 0 = none: a solar line at night really is 0 V
  uint16_t cal;           // 0.00512 / (lsb x ohms). `amps` trims it
  float    offsetA;       // `zero` measures it. Stored after the sign, as in ESP1's inaAmps()
  float    busFactor;     // `volts` measures it
  bool     invert;
  bool     ready;
  bool     changed;       // measured since the last save
};

Module MODULES[] = {
  { "solar", "SOLAR", "iSol",  0x41, 0.010f,  0.00025f, 0.0f, 2048, 0.0f,   1.0f, false, false, false },
  { "batt",  "MAIN",  "iMain", 0x44, 0.0015f, 0.002f,   5.0f, 1707, 0.0f,   1.0f, false, false, false },
  { "sec",   "SEC",   "iSec",  0x40, 0.010f,  0.00025f, 5.0f, 2048, 0.014f, 1.0f, false, false, false },
};
const uint8_t MODULE_COUNT = sizeof(MODULES) / sizeof(MODULES[0]);

// Each refusal is a wrong setup that would otherwise be saved as a calibration. ESP1's inaTrimOk() checks
// the same limits on what it loads, so what this tool saves is always accepted.
const uint8_t CAL_SAMPLES     = 32;      // averaged per measurement, about 1.3 s
const uint8_t CAL_CONV_MS     = 40;      // one AVG16 conversion takes 35.2 ms, so every sample is new
const float   ZERO_MAX_FS     = 0.02f;   // `zero` refused above 2 % of full scale: current is still flowing
const float   AMPS_MIN_LSBS   = 500.0f;  // `amps` refused below this reading: too small to trim from
const float   SHUNT_RATIO_MAX = 2.0f;    // the trimmed shunt stays within x0.5..x2 of what is fitted
const float   VOLTS_MIN       = 1.0f;    // `volts` refused below this: no bus voltage to trim from
const float   VOLTS_RATIO_MAX = 1.10f;   // `volts` refused beyond +/-10 %: the chip is good to about 0.1 %
const int16_t SHUNT_CLIP_RAW  = 32000;   // of +/-32767 (81.92 mV): the current register is no longer linear

const uint16_t CONFIG_VAL = 0x4527;      // AVG16, 1.1 ms bus/shunt, continuous. Same as ESP1
const unsigned long LIVE_MS = 1000, RETRY_MS = 3000;

#define REG_CONFIG  0x00
#define REG_SHUNT_V 0x01
#define REG_BUS_V   0x02
#define REG_CURRENT 0x04
#define REG_CAL     0x05
#define REG_MFG_ID  0xFE
#define REG_DIE_ID  0xFF

uint8_t lastErr = 0;
unsigned long lastLiveMs = 0, lastRetryMs = 0;
bool liveOn = true;
char cmdLine[48];
uint8_t cmdLen = 0;

/* ---- I2C ------------------------------------------------------------------ */
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
  return readReg(addr, REG_MFG_ID, mfg) && readReg(addr, REG_DIE_ID, die) && mfg == 0x5449 && die == 0x2260;
}

/* ---- Detection ------------------------------------------------------------ */
// Names everything on ESP1's bus, so a wrong address or a dead isolator is obvious at a glance.
void scanBus() {
  uint8_t found = 0;
  Serial.println(F("I2C bus:"));
  for (uint8_t a = 1; a < 127; a++) {
    bool answered = present(a);
    if (lastErr == 4 || lastErr == 5) {
      Serial.print(F("  Bus fault: ")); Serial.println(i2cErrText(lastErr));
      return;
    }
    if (!answered) continue;
    found++;
    Serial.print(F("  ")); printHex(a); Serial.print(F("  "));
    if      (a == 0x27) Serial.print(F("LCD"));
    else if (a == 0x48) Serial.print(F("ADS1115 (ACS758)"));
    else if (a == 0x57) Serial.print(F("EEPROM"));
    else if (a == 0x68) Serial.print(F("DS3231 RTC"));
    else {
      const Module *m = NULL;
      for (uint8_t i = 0; i < MODULE_COUNT; i++) if (MODULES[i].addr == a) m = &MODULES[i];
      Serial.print(isIna226(a) ? F("INA226") : F("not an INA226"));
      if (m) { Serial.print(F(" <- ")); Serial.print(m->name); }
    }
    Serial.println();
  }
  if (!found) Serial.println(F("  Nothing answered: check SDA/SCL, VCC and GND."));
}

// Reset, configure, and read back. Reports the exact reason on failure.
bool initModule(Module &m) {
  if (!present(m.addr)) return false;
  uint16_t mfg = 0, die = 0;
  if (!readReg(m.addr, REG_MFG_ID, mfg) || !readReg(m.addr, REG_DIE_ID, die)) {
    Serial.print(m.name); Serial.print(F(": ID read failed: ")); Serial.println(i2cErrText(lastErr));
    return false;
  }
  if (mfg != 0x5449 || die != 0x2260) {
    Serial.print(m.name); Serial.print(F(": ")); printHex(m.addr); Serial.print(F(" is not an INA226 (MFG=0x"));
    Serial.print(mfg, HEX); Serial.print(F(" DIE=0x")); Serial.print(die, HEX); Serial.println(')');
    return false;
  }
  writeReg(m.addr, REG_CONFIG, 0x8000);        // reset
  delay(5);
  uint16_t back = 0;
  if (!writeReg(m.addr, REG_CONFIG, CONFIG_VAL) || !writeReg(m.addr, REG_CAL, m.cal) ||
      !readReg(m.addr, REG_CAL, back) || back != m.cal) {
    Serial.print(m.name); Serial.println(F(": config failed (two modules on one address, or an unstable bus?)"));
    return false;
  }
  return true;
}

void printHeader() { Serial.println(F("Module\tBus V\tShunt mV\tCurrent A\tPower W")); }

void tryMissingModules() {
  bool changed = false;
  for (uint8_t i = 0; i < MODULE_COUNT; i++) {
    Module &m = MODULES[i];
    if (m.ready) continue;
    m.ready = initModule(m);
    if (m.ready) {
      Serial.print(m.name); Serial.print(F(" DETECTED at ")); printHex(m.addr); Serial.println();
      changed = true;
    }
  }
  if (changed) printHeader();
}

void printMissing() {
  for (uint8_t i = 0; i < MODULE_COUNT; i++) {
    if (MODULES[i].ready) continue;
    Serial.print(MODULES[i].name); Serial.print(F(" not found at ")); printHex(MODULES[i].addr);
    Serial.println(i < 2 ? F(": check the ISO1540's two supplies, GND1 on main negative, side 2 on the ESP32, and A0/A1.")
                         : F(": check its A0/A1 (both GND), SDA/SCL, VCC and GND."));
  }
}

/* ---- Reading --------------------------------------------------------------- */
float signedAmps(const Module &m, int16_t raw) {   // before the offset, as in ESP1's inaAmps()
  float a = raw * m.lsb;
  return m.invert ? -a : a;
}

void lost(Module &m) {
  Serial.print(m.name); Serial.print(F(" LOST: ")); Serial.println(i2cErrText(lastErr));
  m.ready = false;
}

void readModule(Module &m) {
  uint16_t rShunt, rBus, rCurr, rCal;
  if (!readReg(m.addr, REG_SHUNT_V, rShunt) || !readReg(m.addr, REG_BUS_V, rBus) ||
      !readReg(m.addr, REG_CURRENT, rCurr) || !readReg(m.addr, REG_CAL, rCal)) { lost(m); return; }
  if (rCal != m.cal) {                         // its supply dropped: it answers, but reads 0 A
    Serial.print(m.name); Serial.println(F(" restarted (its supply dropped): re-configuring."));
    m.ready = initModule(m);
    return;
  }
  float busV = rBus * 0.00125f * m.busFactor;
  float amps = signedAmps(m, (int16_t)rCurr) - m.offsetA;
  Serial.print(m.name);                       Serial.print('\t');
  Serial.print(busV, 3);                      Serial.print('\t');
  Serial.print((int16_t)rShunt * 0.0025f, 4); Serial.print(F("\t\t"));
  Serial.print(amps, 4);                      Serial.print(F("\t\t"));
  Serial.print(busV * amps, 3);
  if (busV < m.minV) Serial.print(F("\tNO BUS VOLTAGE: check the VBS lead"));
  Serial.println();
}

/* ---- Calibration ------------------------------------------------------------ */
float fullScaleA(const Module &m) { return 16.0f * m.cal * m.lsb; }             // 81.92 mV / shunt ohms
float shuntOhms(const Module &m)  { return 0.00512f / (m.cal * m.lsb); }

struct Sample {
  float amps;    // mean, before the offset
  float busV;    // mean, before busFactor
  float spreadA; // highest - lowest
  bool  clipped; // the shunt voltage reached the chip's limit
};

// False on any I2C failure, so a dead module can never be calibrated from zeros.
bool takeSample(Module &m, Sample &s) {
  float sumA = 0, sumV = 0, lo = 0, hi = 0;
  s.clipped = false;
  for (uint8_t n = 0; n < CAL_SAMPLES; n++) {
    delay(CAL_CONV_MS);                        // also lets a just-written CAL reach the current register
    uint16_t rShunt, rBus, rCurr;
    if (!readReg(m.addr, REG_SHUNT_V, rShunt) || !readReg(m.addr, REG_BUS_V, rBus) ||
        !readReg(m.addr, REG_CURRENT, rCurr)) { lost(m); return false; }
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
  Serial.print(F("  Module ")); Serial.print(m.name);
  Serial.print(F(": shunt ")); Serial.print(shuntOhms(m) * 1000.0f, 4);
  Serial.print(F(" mOhm, offset ")); Serial.print(m.offsetA, 4);
  Serial.print(F(" A, bus factor ")); Serial.print(m.busFactor, 4);
  Serial.print(F(", invert ")); Serial.println(m.invert ? F("yes") : F("no"));
  Serial.println(F("  Or paste into ESP1/src/main.cpp:"));
  if (m.paste[0] == 'S' && m.paste[1] == 'E') {   // the secondary's constants carry CAL directly
    Serial.print(F("    const uint16_t SEC_INA_CAL          = ")); Serial.print(m.cal); Serial.println(';');
    Serial.print(F("    const float    SEC_CURRENT_OFFSET_A = ")); Serial.print(m.offsetA, 4); Serial.println(F("f;"));
    Serial.print(F("    const float    SEC_BUS_V_FACTOR     = ")); Serial.print(m.busFactor, 4); Serial.println(F("f;"));
    Serial.print(F("    const bool     SEC_INVERT_CURRENT   = ")); Serial.println(m.invert ? F("true;") : F("false;"));
    return;
  }
  Serial.print(F("    const float ")); Serial.print(m.paste); Serial.print(F("_SHUNT_OHM  = "));
  Serial.print(shuntOhms(m), 8); Serial.println(F("f;"));
  Serial.print(F("    const float ")); Serial.print(m.paste); Serial.print(F("_OFFSET_A   = "));
  Serial.print(m.offsetA, 4); Serial.println(F("f;"));
  Serial.print(F("    const float ")); Serial.print(m.paste); Serial.print(F("_BUS_FACTOR = "));
  Serial.print(m.busFactor, 4); Serial.println(F("f;"));
  Serial.print(F("    const bool  ")); Serial.print(m.paste); Serial.print(F("_INVERT     = "));
  Serial.println(m.invert ? F("true;") : F("false;"));
}

void next(const __FlashStringHelper *what, const Module &m) {
  Serial.print(F("NEXT: ")); Serial.print(what); Serial.println(m.name);
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
  m.offsetA = s.amps; m.changed = true;
  Serial.print(F("Offset = ")); Serial.print(m.offsetA, 4); Serial.println(F(" A."));
  Serial.print(F("NEXT: set a steady known load, then type: amps ")); Serial.print(m.name); Serial.println(F(" <A from your clamp meter>"));
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
  if (fabs(reading) < AMPS_MIN_LSBS * m.lsb) {
    Serial.print(F("REFUSED: it reads only ")); Serial.print(reading, 3); Serial.print(F(" A. Use at least "));
    Serial.print(AMPS_MIN_LSBS * m.lsb, 2); Serial.println(F(" A."));
    return;
  }
  if ((reading > 0) != (meterA > 0)) {
    Serial.print(F("REFUSED: it reads ")); Serial.print(reading, 3);
    Serial.print(F(" A, the opposite direction to your meter. If the leads are reversed type: invert ")); Serial.println(m.name);
    return;
  }
  float ratio = meterA / reading;
  float newCalF = m.cal * ratio + 0.5f;
  float newOhm = (newCalF >= 1.0f && newCalF <= 32767.0f) ? 0.00512f / ((uint16_t)newCalF * m.lsb) : 0.0f;
  if (newOhm < m.nominalOhm / SHUNT_RATIO_MAX || newOhm > m.nominalOhm * SHUNT_RATIO_MAX) {
    Serial.print(F("REFUSED: it reads ")); Serial.print(reading, 3); Serial.print(F(" A against your "));
    Serial.print(meterA, 3); Serial.println(F(" A. Too far apart to be a trim: check the shunt and the number."));
    return;
  }
  uint16_t oldCal = m.cal, newCal = (uint16_t)newCalF, back = 0;
  if (!writeReg(m.addr, REG_CAL, newCal) || !readReg(m.addr, REG_CAL, back) || back != newCal) {
    writeReg(m.addr, REG_CAL, oldCal);
    Serial.print(F("FAILED: CAL did not read back: ")); Serial.println(i2cErrText(lastErr));
    return;
  }
  m.cal = newCal; m.changed = true;
  m.offsetA *= (float)newCal / oldCal;         // the offset comes through the same CAL
  if (s.spreadA > 0.05f * fabs(reading)) {
    Serial.print(F("WARNING: the current moved by ")); Serial.print(s.spreadA, 3);
    Serial.println(F(" A during the measurement. Repeat with a steadier load."));
  }
  Serial.print(F("CAL ")); Serial.print(oldCal); Serial.print(F(" -> ")); Serial.print(newCal);
  Serial.print(F(", shunt = ")); Serial.print(shuntOhms(m) * 1000.0f, 4); Serial.println(F(" mOhm"));
  Sample after;                                // read through the new CAL: did the trim do what it should?
  if (!takeSample(m, after)) return;
  Serial.print(F("Now reads ")); Serial.print(after.amps - m.offsetA, 3); Serial.print(F(" A against your "));
  Serial.print(meterA, 3); Serial.println(F(" A"));
  if (ratio > 1.08f && m.nominalOhm < 0.005f)  // the bar with the module's R010 still in parallel reads ~13 % low
    Serial.println(F("WARNING: this module read over 8 % low. Check its onboard R010 was removed."));
  Serial.print(F("NEXT: put a multimeter across VBS and GND, then type: volts ")); Serial.print(m.name); Serial.println(F(" <V>"));
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
  m.busFactor = ratio; m.changed = true;
  Serial.print(F("Bus factor = ")); Serial.print(m.busFactor, 4); Serial.print(F(", now reads "));
  Serial.print(s.busV * m.busFactor, 3); Serial.println(F(" V"));
#if defined(ESP32)
  next(F("store it with: save "), m);
#else
  Serial.println(F("NEXT: type show and paste the values into ESP1/src/main.cpp (this board cannot store them)."));
#endif
}

void cmdInvert(Module &m) {
  m.invert = !m.invert; m.changed = true;
  m.offsetA = -m.offsetA;                      // the offset is stored after the sign, so it flips too
  Serial.print(m.name); Serial.print(F(" invert = ")); Serial.println(m.invert ? F("yes") : F("no"));
}

#if defined(ESP32)
/* ESP1's loadCal reads these from the namespace "calib". Same keys, same namespace. */
void nvsKey(char *k, const Module &m, char suffix) { snprintf(k, 12, "%s%c", m.nvs, suffix); }

void cmdSave(Module &m) {
  Preferences p;
  if (!p.begin("calib", false)) { Serial.println(F("FAILED: could not open NVS.")); return; }
  char k[12];
  nvsKey(k, m, 'O'); bool ok = p.putFloat(k, shuntOhms(m)) > 0;
  nvsKey(k, m, 'A'); ok = p.putFloat(k, m.offsetA) > 0 && ok;
  nvsKey(k, m, 'B'); ok = p.putFloat(k, m.busFactor) > 0 && ok;
  nvsKey(k, m, 'I'); ok = p.putBool(k, m.invert) > 0 && ok;
  p.end();
  if (!ok) { Serial.println(F("FAILED: NVS write error.")); return; }
  m.changed = false;
  Serial.print(F("Saved ")); Serial.print(m.name); Serial.println(F(" to this board's NVS."));
  printPaste(m);
  Serial.println(F("NEXT: next module, or flash ESP1's main firmware back: it loads these at boot."));
}

void cmdForget(Module &m) {
  Preferences p;
  if (!p.begin("calib", false)) { Serial.println(F("FAILED: could not open NVS.")); return; }
  char k[12];
  const char SUF[4] = { 'O', 'A', 'B', 'I' };
  for (uint8_t i = 0; i < 4; i++) { nvsKey(k, m, SUF[i]); p.remove(k); }
  p.end();
  Serial.print(m.name); Serial.println(F(": saved trims forgotten. ESP1 goes back to its compiled values. Reboot this tool to see them."));
}

// Start from what is stored, so the tool shows the values the firmware is really using.
void loadSaved() {
  Preferences p;
  if (!p.begin("calib", true)) return;
  char k[12];
  for (uint8_t i = 0; i < MODULE_COUNT; i++) {
    Module &m = MODULES[i];
    nvsKey(k, m, 'O');
    if (!p.isKey(k)) continue;
    float ohm = p.getFloat(k, shuntOhms(m));
    if (ohm >= m.nominalOhm / SHUNT_RATIO_MAX && ohm <= m.nominalOhm * SHUNT_RATIO_MAX)
      m.cal = (uint16_t)(0.00512f / (m.lsb * ohm) + 0.5f);
    nvsKey(k, m, 'A'); m.offsetA = p.getFloat(k, m.offsetA);
    nvsKey(k, m, 'B'); m.busFactor = p.getFloat(k, m.busFactor);
    nvsKey(k, m, 'I'); m.invert = p.getBool(k, m.invert);
    Serial.print(F("Loaded saved trims for ")); Serial.println(m.name);
  }
  p.end();
}
#endif

void cmdShow() {
  for (uint8_t i = 0; i < MODULE_COUNT; i++) {
    const Module &m = MODULES[i];
    Serial.print(F("--- ")); Serial.print(m.name); Serial.print(' '); printHex(m.addr);
    Serial.print(m.ready ? F(" OK") : F(" MISSING"));
    Serial.print(F(", full scale ")); Serial.print(fullScaleA(m), 2); Serial.print(F(" A"));
    Serial.println(m.changed ? F(", NOT SAVED ---") : F(" ---"));
    printPaste(m);
  }
}

// Through an isolator a marginal pull-up or low level shows up as the odd failed transfer long before
// anything stops working outright, so count them. Run it with the inverter on as well as off.
void cmdBusTest() {
  const uint16_t TRIES = 500;
  for (uint8_t i = 0; i < MODULE_COUNT; i++) {
    Module &m = MODULES[i];
    if (!m.ready) continue;
    uint16_t fails = 0, wrong = 0, v = 0;
    for (uint16_t n = 0; n < TRIES; n++) {
      if (!readReg(m.addr, REG_MFG_ID, v)) fails++;
      else if (v != 0x5449) wrong++;
    }
    Serial.print(m.name); Serial.print(F(": ")); Serial.print(TRIES); Serial.print(F(" reads, "));
    Serial.print(fails); Serial.print(F(" failed, ")); Serial.print(wrong); Serial.print(F(" wrong. "));
    Serial.println(fails || wrong ? F("NOT RELIABLE: check the supplies, the pull-ups and which side faces the ESP32.") : F("Good."));
  }
}

void printHelp() {
  Serial.println(F("Commands (<m> = solar, batt or sec):"));
  Serial.println(F("  zero <m>       no current through the shunt: measure the offset"));
  Serial.println(F("  amps <m> <A>   steady current from a clamp meter: trim the shunt value"));
  Serial.println(F("  volts <m> <V>  meter across VBS and GND: trim the bus voltage"));
#if defined(ESP32)
  Serial.println(F("  save <m>       store it for the firmware (flash the firmware back afterwards)"));
  Serial.println(F("  defaults <m>   forget a module's saved trims"));
#endif
  Serial.println(F("  invert <m>     swap the current's sign"));
  Serial.println(F("  show           all values + lines to paste into ESP1"));
  Serial.println(F("  r              read once       live   live readout on/off"));
  Serial.println(F("  scan           list the bus    bustest   count failed reads"));
}

// "b", "batt", "battery", "main" name the battery module, "sec", "secondary" the pack.
Module *findModule(const char *s) {
  if (!s || !*s) return NULL;
  if (!strncasecmp(s, "main", strlen(s))) return &MODULES[1];
  for (uint8_t i = 0; i < MODULE_COUNT; i++) {
    size_t nameLen = strlen(MODULES[i].name), n = strlen(s);
    if (!strncasecmp(s, MODULES[i].name, n < nameLen ? n : nameLen)) return &MODULES[i];
  }
  return NULL;
}

void handleCommand(char *line) {
  char *cmd = strtok(line, " ,\t");
  if (!cmd) return;
  if (!strcasecmp(cmd, "h") || !strcasecmp(cmd, "help")) { printHelp(); return; }
  if (!strcasecmp(cmd, "show")) { cmdShow(); return; }
  if (!strcasecmp(cmd, "scan")) { scanBus(); printMissing(); return; }
  if (!strcasecmp(cmd, "bustest")) { cmdBusTest(); lastLiveMs = millis(); return; }
  if (!strcasecmp(cmd, "live")) {
    liveOn = !liveOn;
    Serial.print(F("live = ")); Serial.println(liveOn ? F("ON") : F("OFF"));
    return;
  }
  if (!strcasecmp(cmd, "r")) {
    printHeader();
    for (uint8_t i = 0; i < MODULE_COUNT; i++) if (MODULES[i].ready) readModule(MODULES[i]);
    return;
  }
  bool isZero = !strcasecmp(cmd, "zero"), isAmps = !strcasecmp(cmd, "amps"), isVolts = !strcasecmp(cmd, "volts"),
       isInvert = !strcasecmp(cmd, "invert"), isSave = !strcasecmp(cmd, "save"), isForget = !strcasecmp(cmd, "defaults");
  if (!isZero && !isAmps && !isVolts && !isInvert && !isSave && !isForget) { Serial.println(F("Unknown command: type h")); return; }

  Module *m = findModule(strtok(NULL, " ,\t"));
  if (!m) { Serial.println(F("Name a module: solar, batt or sec")); return; }
#if defined(ESP32)
  if (isSave)   { cmdSave(*m);   return; }
  if (isForget) { cmdForget(*m); return; }
#else
  if (isSave || isForget) { Serial.println(F("This board cannot store them: type show and paste into ESP1.")); return; }
#endif
  if (!m->ready) { Serial.print(m->name); Serial.println(F(" is not detected: type scan.")); return; }
  const char *val = strtok(NULL, " ,\t");
  float number = val ? (float)atof(val) : 0.0f;   // atof gives 0 for junk, which amps and volts refuse
  if      (isZero)  cmdZero(*m);
  else if (isAmps)  cmdAmps(*m, number);
  else if (isVolts) cmdVolts(*m, number);
  else              cmdInvert(*m);
  lastLiveMs = millis();                       // a measurement blocks: no stale line right after it
}

void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen == 0) continue;
      cmdLine[cmdLen] = '\0';
      cmdLen = 0;
      handleCommand(cmdLine);
    } else if (cmdLen < sizeof(cmdLine) - 1) cmdLine[cmdLen++] = c;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
#if defined(ESP32)
  Wire.begin(21, 22);
  Wire.setTimeOut(50);                         // bound a stuck bus
#else
  Wire.begin();
  Wire.setWireTimeout(25000, true);            // 25 ms: a stuck bus cannot freeze the sketch
#endif
  Serial.println(F("\n=== INA226 CALIBRATION: solar, main battery, secondary battery ==="));
#if defined(ESP32)
  loadSaved();
#endif
  printHelp();
  Serial.println(F("START: pick a module and type: zero solar   (with no current through its shunt)"));
  tryMissingModules();
  scanBus();
  printMissing();
  lastRetryMs = lastLiveMs = millis();
}

void loop() {
  pollSerial();
  if (cmdLen > 0) return;                      // mid-line: hold the output until Enter
  unsigned long now = millis();

  bool anyMissing = false;
  for (uint8_t i = 0; i < MODULE_COUNT; i++) if (!MODULES[i].ready) anyMissing = true;
  if (anyMissing && now - lastRetryMs >= RETRY_MS) { lastRetryMs = now; tryMissingModules(); }

  if (liveOn && now - lastLiveMs >= LIVE_MS) {
    lastLiveMs = now;
    for (uint8_t i = 0; i < MODULE_COUNT; i++) if (MODULES[i].ready) readModule(MODULES[i]);
  }
}
