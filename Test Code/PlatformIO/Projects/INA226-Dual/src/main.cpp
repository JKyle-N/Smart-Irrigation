/*
  Dual INA226 (CJMCU-226) detect + read test
  ------------------------------------------------------------------
  Finds two INA226 modules on one I2C bus at two different addresses, proves each is really
  an INA226 from its ID registers, and reads both side by side. Only Wire is used.

  WIRING (both modules share the same bus)
    ESP32:  SDA -> GPIO21   SCL -> GPIO22   VCC -> 3.3V   GND -> GND
    Uno:    SDA -> A4       SCL -> A5       VCC -> 5V     GND -> GND
    Shunt R010 on each module. IN+ / IN- in series with the load, VBS tied to IN- (high side).
    Keep load and board grounds common, and stay under the INA226's 36 V limit.

  SETTING THE ADDRESS (set on the module, by its A0 / A1 pins or solder pads)
    A1 and A0 both to GND = 0x40  <- MODULE A (the default as shipped)
    A1 to GND, A0 to VS   = 0x41  <- MODULE B: this one must be changed
    Two modules left on the same address both answer and corrupt each other's replies.
    To try other addresses, change only the MODULES table below:

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
  uint16_t cal;        // 2048 = R010 at 0.25 mA/bit. Trim: cal x (meter A / INA A)  [MEASURE per module]
  float    offsetA;    // no-load reading, subtracted                                [MEASURE per module]
  float    busFactor;  // meter V / INA V                                            [MEASURE per module]
  bool     ready;
};

Module MODULES[] = {
  { "A", 0x40, 2048, 0.0f, 1.000f, false },
  { "B", 0x41, 2048, 0.0f, 1.000f, false },
};
const uint8_t MODULE_COUNT = sizeof(MODULES) / sizeof(MODULES[0]);

const float    CURRENT_LSB = 0.00025f;        // 0.25 mA/bit -> 8.192 A full scale on R010
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
  Serial.println(F("  With two modules connected and one address silent, both are probably on the"));
  Serial.println(F("  same address: set A0 to VS on module B for 0x41. Then check its SDA/SCL/VCC/GND."));
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
  float current_A = (int16_t)rCurr * CURRENT_LSB - m.offsetA;

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

  Serial.println(F("\n=== Dual INA226 test (CJMCU-226, R010) ==="));
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
