/*
  Solar + main battery INA226 (CJMCU-226) detect + read test, for the rig's two isolated modules
  ------------------------------------------------------------------
  Finds both modules on one I2C bus at two different addresses, proves each is really an INA226
  from its ID registers, and reads both side by side. Only Wire is used. Same addresses, scaling and
  sign as ESP1/src/main.cpp, so a module that reads right here reads right there.

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
  uint8_t  addr;
  uint16_t cal;        // 0.00512 / (currentLsb x shunt ohms). Trim: cal x (meter A / INA A) [MEASURE]
  float    currentLsb; // amps per bit
  float    offsetA;    // no-load reading, subtracted                                [MEASURE per module]
  float    busFactor;  // meter V / INA V                                            [MEASURE per module]
  bool     ready;
};

Module MODULES[] = {
  { "SOLAR", 0x41, 2048, 0.00025f, 0.0f, 1.000f, false },   // R010: 0.25 mA/bit, 8.19 A full scale
  { "BATT",  0x44, 1707, 0.002f,   0.0f, 1.000f, false },   // 50 A / 75 mV: 2 mA/bit, 54.6 A full scale
};
const uint8_t MODULE_COUNT = sizeof(MODULES) / sizeof(MODULES[0]);

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
  float current_A = (int16_t)rCurr * m.currentLsb - m.offsetA;

  Serial.print(m.name);        Serial.print('\t');
  Serial.print(bus_V, 3);      Serial.print('\t');
  Serial.print(shunt_mV, 4);   Serial.print(F("\t\t"));
  Serial.print(current_A, 4);  Serial.print(F("\t\t"));
  Serial.println(bus_V * current_A, 3);
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

  Serial.println(F("\n=== Solar + battery INA226 test (CJMCU-226) ==="));
  tryMissingModules();
  lastRetryMs = lastReadMs = millis();
}

void loop() {
  unsigned long now = millis();

  bool anyMissing = false;
  for (uint8_t i = 0; i < MODULE_COUNT; i++) if (!MODULES[i].ready) anyMissing = true;
  if (anyMissing && now - lastRetryMs >= RETRY_MS) {
    lastRetryMs = now;
    tryMissingModules();
  }

  if (now - lastReadMs >= READ_MS) {
    lastReadMs = now;
    for (uint8_t i = 0; i < MODULE_COUNT; i++) if (MODULES[i].ready) readModule(MODULES[i]);
  }
}
