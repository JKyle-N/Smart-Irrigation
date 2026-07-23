/* =============================================================================
 *  ESP32 #2  --  STANDALONE PCF8575 RELAY SEQUENCE TEST  (wiring / I2C check)
 * -----------------------------------------------------------------------------
 *  Purpose: prove the PCF8575 + relay board wiring works, with NOTHING else in
 *  the loop -- no ESP1, no UART, no work orders. It walks ONE relay ON up the
 *  bank (P0 -> P15), one at a time, with a 1-second gap between each, forever.
 *  If a relay clicks here, the hardware path is good and the main ESP2 firmware
 *  uses the exact same I2C/active-LOW writes (pcfWrite/pcfOn).
 *
 *  WIRING (must match the real rig / ESP2/src/main.cpp):
 *    PCF8575 SDA -> GPIO21,  SCL -> GPIO22,  address 0x20 (A0..A2 = GND).
 *    Outputs are ACTIVE-LOW: writing 0xFFFF = all relays OFF; clearing a bit
 *    drives that output LOW = that relay ON.
 *
 *  >>> SAFETY: this drives EVERY output in turn, including the inverter (P5),
 *      the AC pumps (P6/P7/P10), and the Nano-RESET line (P17/bit15). For a dry
 *      wiring check, power the relay board but keep AC pumps/loads DISCONNECTED,
 *      or set FIRST_BIT / LAST_BIT below to skip those channels.
 *
 *  BUILD: PlatformIO -> env:esp32dev (only uses <Wire.h>, no extra libs).
 *         `pio run -t upload` then `pio device monitor` @ 115200.
 * ========================================================================== */

#include <Arduino.h>
#include <Wire.h>

/* ---- Config (match ESP2) ------------------------------------------------- */
#define I2C_SDA   21
#define I2C_SCL   22
#define PCF_ADDR  0x20

/* ---- Sequence options ---------------------------------------------------- */
const uint8_t  FIRST_BIT = 0;      // first PCF output to test (0..15)
const uint8_t  LAST_BIT  = 15;     // last  PCF output to test (0..15)
const uint16_t STEP_MS   = 1000;   // 1 second between each relay (gap requested)

/* ---- Channel names (mirror ESP2 OUT_* numbering) ------------------------- */
const char *CH_NAME[16] = {
  "ResValve",  // P0
  "Col A Vlv", // P1
  "Col B Vlv", // P2
  "Col C Vlv", // P3
  "Mix Valve", // P4
  "Inverter",  // P5  (AC source)
  "Transfer",  // P6  (AC pump)
  "Booster",   // P7  (AC pump)
  "Mixer",     // P8  (P10 AC motor)
  "Nut A",     // P9  (P11)
  "Nut B",     // P10 (P12)
  "Nut C",     // P11 (P13)
  "Nut D",     // P12 (P14, unused in main fw)
  "pH Up",     // P13 (P15)
  "pH Down",   // P14 (P16)
  "Nano RST"   // P15 (P17)
};

/* ---- PCF8575 raw I2C (active-LOW; identical pattern to ESP2 firmware) ----- */
uint16_t pcfShadow = 0xFFFF;       // all OFF

bool pcfWrite(uint16_t s) {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(lowByte(s));
  Wire.write(highByte(s));
  bool ok = (Wire.endTransmission() == 0);
  pcfShadow = s;
  return ok;
}
void allOff()            { pcfWrite(0xFFFF); }
void relayOn(uint8_t b)  { pcfWrite(0xFFFF & ~(uint16_t)(1 << b)); }   // exactly one ON

bool pcfPresent() {
  Wire.beginTransmission(PCF_ADDR);
  return (Wire.endTransmission() == 0);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== ESP32 #2 PCF8575 relay sequence test ==="));

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  allOff();                                  // SAFE: every relay OFF first

  if (!pcfPresent()) {
    Serial.printf("ERROR: no I2C ACK from PCF8575 at 0x%02X.\n", PCF_ADDR);
    Serial.println(F("Check SDA=21 / SCL=22, 3.3V/GND, address straps, and pull-ups."));
    // Slow blink on GPIO2 so a missing PCF is obvious even without the serial monitor.
    pinMode(2, OUTPUT);
    while (true) { digitalWrite(2, HIGH); delay(150); digitalWrite(2, LOW); delay(850); }
  }
  Serial.printf("PCF8575 found at 0x%02X. Walking P%u..P%u, %u ms gap, one ON at a time.\n",
                PCF_ADDR, FIRST_BIT, LAST_BIT, STEP_MS);
}

void loop() {
  for (uint8_t b = FIRST_BIT; b <= LAST_BIT && b < 16; b++) {
    relayOn(b);                              // exactly this relay ON (all others OFF)
    Serial.printf("P%-2u -> %-9s ON\n", b, CH_NAME[b]);
    delay(STEP_MS);                          // 1 s gap, then move up to the next
  }
  // wrap and repeat from the bottom, always going up
}
