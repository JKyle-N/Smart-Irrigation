/*
  SUPERSEDED by Calibration/ESP1/INA226 (the "sec" module), which calibrates over serial instead of by
  editing constants. Note this one takes the FIRST INA226 it finds, so it can read the wrong module.

  INA226 current / voltage / power monitor - with detection diagnostics
  Shunt: R010 (0.010 ohm), VBS tied to IN- (high-side sensing)
  No external library needed - uses Wire only.

  Wiring (Arduino Uno / Nano):
    INA226 VCC -> 5V      INA226 GND -> GND
    INA226 SCL -> A5      INA226 SDA -> A4
  Mega: SDA = 20, SCL = 21   |   ESP32: SDA = 21, SCL = 22 (power INA226 from 3.3V)

  Serial Monitor: 115200 baud
*/

#include <Wire.h>

// ---------------- Settings ----------------
const float    R_SHUNT     = 0.010;               // R010
const float    CURRENT_LSB = 0.00025;             // 0.25 mA/bit -> max 8.192 A
const float    POWER_LSB   = 25.0 * CURRENT_LSB;  // 6.25 mW/bit
// ---- Calibration (adjust these 3) ----
const uint16_t CAL_VALUE      = 2048;   // Step 2: CAL x (meter A / INA A)
const float    CURRENT_OFFSET = 0.014;    // Step 1: no-load reading in A (subtracted)
const float    BUS_V_FACTOR   = 1.000;  // Step 3: meter V / INA V
const uint16_t CONFIG_VAL  = 0x4527;              // AVG16, 1.1ms/1.1ms, continuous
const unsigned long RETRY_MS = 2000;              // retry interval when not found
const unsigned long READ_MS  = 500;               // reading interval
const bool     INVERT_CURRENT = false;             // true = flip sign (IN+/IN- wired reversed)

// ---------------- Registers ----------------
#define REG_CONFIG    0x00
#define REG_SHUNT_V   0x01
#define REG_BUS_V     0x02
#define REG_POWER     0x03
#define REG_CURRENT   0x04
#define REG_CAL       0x05
#define REG_MFG_ID    0xFE
#define REG_DIE_ID    0xFF

uint8_t inaAddr = 0;          // 0 = not found yet
bool    inaReady = false;
uint8_t lastErr = 0;
unsigned long lastAction = 0;

// ---------------- I2C helpers ----------------
const char* i2cErrText(uint8_t e) {
  switch (e) {
    case 0: return "OK";
    case 1: return "data too long for buffer";
    case 2: return "no ACK on address (device not responding)";
    case 3: return "no ACK on data";
    case 4: return "other bus error (SDA/SCL shorted or stuck?)";
    case 5: return "bus timeout (SDA/SCL held low / missing pull-ups?)";
    case 10: return "requested 2 bytes, got fewer";
    default: return "unknown error";
  }
}

bool writeReg(uint8_t reg, uint16_t val) {
  Wire.beginTransmission(inaAddr);
  Wire.write(reg);
  Wire.write(val >> 8);
  Wire.write(val & 0xFF);
  lastErr = Wire.endTransmission();
  return lastErr == 0;
}

bool readReg(uint8_t reg, uint16_t &out) {
  Wire.beginTransmission(inaAddr);
  Wire.write(reg);
  lastErr = Wire.endTransmission(false);
  if (lastErr != 0) return false;

  if (Wire.requestFrom(inaAddr, (uint8_t)2) != 2) {
    lastErr = 10;
    while (Wire.available()) Wire.read();
    return false;
  }
  out = ((uint16_t)Wire.read() << 8) | Wire.read();
  return true;
}

// Lists every device on the bus - tells you if wiring works at all
uint8_t scanBus() {
  uint8_t count = 0;
  Serial.println(F("Scanning I2C bus..."));
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    uint8_t e = Wire.endTransmission();
    if (e == 0) {
      Serial.print(F("  Device at 0x"));
      if (a < 16) Serial.print('0');
      Serial.print(a, HEX);
      if (a >= 0x40 && a <= 0x4F) Serial.print(F("  <- possible INA226"));
      Serial.println();
      count++;
    } else if (e == 4 || e == 5) {
      Serial.print(F("  Bus fault while scanning: "));
      Serial.println(i2cErrText(e));
      return 0;
    }
  }
  if (count == 0) Serial.println(F("  No I2C devices found."));
  return count;
}

void printHints() {
  Serial.println(F("  Check:"));
  Serial.println(F("   - VCC to 5V (or 3.3V), GND to Arduino GND"));
  Serial.println(F("   - SDA -> A4, SCL -> A5 on Uno/Nano (not swapped)"));
  Serial.println(F("   - Loose jumper / bad solder on header pins"));
  Serial.println(F("   - A0/A1 address pads (default both GND = 0x40)"));
}

// Searches 0x40-0x4F, verifies chip ID, configures it
bool initINA226() {
  inaReady = false;
  inaAddr = 0;

  for (uint8_t a = 0x40; a <= 0x4F; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() != 0) continue;

    inaAddr = a;
    uint16_t mfg, die;
    if (!readReg(REG_MFG_ID, mfg) || !readReg(REG_DIE_ID, die)) {
      Serial.print(F("  0x")); Serial.print(a, HEX);
      Serial.print(F(" answered but ID read failed: "));
      Serial.println(i2cErrText(lastErr));
      continue;
    }
    if (mfg != 0x5449 || die != 0x2260) {
      Serial.print(F("  0x")); Serial.print(a, HEX);
      Serial.print(F(" is not an INA226 (MFG=0x")); Serial.print(mfg, HEX);
      Serial.print(F(" DIE=0x")); Serial.print(die, HEX);
      Serial.println(F(") - maybe INA219/other chip"));
      continue;
    }

    // Found it - reset and configure
    writeReg(REG_CONFIG, 0x8000);
    delay(5);
    if (!writeReg(REG_CONFIG, CONFIG_VAL) || !writeReg(REG_CAL, CAL_VALUE)) {
      Serial.print(F("  Config write failed: "));
      Serial.println(i2cErrText(lastErr));
      return false;
    }

    // Verify calibration stuck
    uint16_t calBack;
    if (!readReg(REG_CAL, calBack) || calBack != CAL_VALUE) {
      Serial.println(F("  Calibration readback mismatch - unstable bus?"));
      return false;
    }

    Serial.print(F("INA226 DETECTED at 0x")); Serial.println(a, HEX);
    Serial.println(F("Bus V\tShunt mV\tCurrent A\tPower W"));
    inaReady = true;
    return true;
  }

  inaAddr = 0;
  return false;
}

// ---------------- Main ----------------
void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000);   // for Leonardo / native USB boards

  Wire.begin();
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(25000, true);          // 25 ms - prevents freezing on a stuck bus
#endif

  Serial.println(F("\n=== INA226 monitor (R010) ==="));
  scanBus();

  if (!initINA226()) {
    Serial.println(F("INA226 NOT DETECTED."));
    printHints();
    Serial.println(F("Retrying every 2 s..."));
  }
  lastAction = millis();
}

void loop() {
  unsigned long now = millis();

  // --- Not detected: keep retrying and report ---
  if (!inaReady) {
    if (now - lastAction >= RETRY_MS) {
      lastAction = now;
#if defined(WIRE_HAS_TIMEOUT)
      if (Wire.getWireTimeoutFlag()) {
        Serial.println(F("  I2C timeout happened - bus was stuck, reset."));
        Wire.clearWireTimeoutFlag();
      }
#endif
      if (initINA226()) return;
      Serial.println(F("INA226 NOT DETECTED - still retrying..."));
    }
    return;
  }

  // --- Detected: read values ---
  if (now - lastAction < READ_MS) return;
  lastAction = now;

  uint16_t rShunt, rBus, rCurr, rPow;
  if (!readReg(REG_SHUNT_V, rShunt) || !readReg(REG_BUS_V, rBus) ||
      !readReg(REG_CURRENT, rCurr)  || !readReg(REG_POWER, rPow)) {
    Serial.print(F("INA226 LOST: "));
    Serial.println(i2cErrText(lastErr));
    printHints();
    inaReady = false;
    return;
  }

  float shunt_mV  = (int16_t)rShunt * 0.0025;   // 2.5 uV/bit, signed
  float bus_V     = rBus * 0.00125;             // 1.25 mV/bit
  float current_A = (int16_t)rCurr * CURRENT_LSB;
  float power_W   = rPow * POWER_LSB;           // always positive (chip uses magnitude)

  if (INVERT_CURRENT) {
    shunt_mV  = -shunt_mV;
    current_A = -current_A;
  }

  // Apply calibration corrections
  current_A -= CURRENT_OFFSET;
  bus_V     *= BUS_V_FACTOR;
  power_W    = bus_V * current_A;   // recompute so power matches corrected values

  Serial.print(bus_V, 3);      Serial.print('\t');
  Serial.print(shunt_mV, 4);   Serial.print(F("\t\t"));
  Serial.print(current_A, 4);  Serial.print(F("\t\t"));
  Serial.println(power_W, 3);
}
