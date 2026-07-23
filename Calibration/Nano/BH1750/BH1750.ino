/* =============================================================================
 *  SMART IRRIGATION  --  CALIBRATION BENCH TOOL  --  BH1750 LIGHT (Arduino Nano)
 * -----------------------------------------------------------------------------
 *  Standalone. No ESP1, no UART. Measures the lux OFFSET trim (spec sec.A.4:
 *  offset vs. a reference lux meter; default 0). ESP32 #1 applies it on the
 *  Nano's LIGHT packet.
 *
 *  BUS (match Nano/Nano.ino:96): BH1750 on I2C A4 (SDA) / A5 (SCL).
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    r                 print lux once
 *    live              toggle ~2 Hz live readout
 *    ref <value>       enter your reference lux-meter reading -> lux offset
 *    show              print current offset
 *
 *  offset = reference - sensor. A multiplicative scale is usually better for a
 *  lux meter, but the firmware model is an additive offset (default 0) -- capture
 *  at your typical working light level. Paste into ESP32 #1's LIGHT offset.
 *
 *  Library (Arduino IDE: install "BH1750" by Christopher Laws).
 *  Build: Arduino IDE (Arduino Nano, ATmega328P) or `pio run` in this folder.
 * ========================================================================== */
#include <Arduino.h>
#include <Wire.h>
#include <BH1750.h>

BH1750 lightMeter;

const unsigned long LIVE_MS = 500;
float luxOffset = 0.0f;
bool  liveOn = false;
unsigned long lastLiveMs = 0;

#define LINE_BUF 48
char    line[LINE_BUF];
uint8_t lineLen = 0;

void printHelp();
float readLux();
void  printRaw();
void  handleLine(char *s);

void setup() {
  Serial.begin(115200);
  Wire.begin();
  if (!lightMeter.begin()) Serial.println(F("WARN: BH1750 begin() failed (check A4/A5 wiring)"));
  Serial.println();
  Serial.println(F("=== BH1750 LIGHT CALIBRATION (Nano) ==="));
  printHelp();
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) { line[lineLen] = '\0'; handleLine(line); lineLen = 0; }
    } else if (lineLen < LINE_BUF - 1) {
      line[lineLen++] = c;
    } else {
      lineLen = 0;
    }
  }
  if (liveOn && (unsigned long)(millis() - lastLiveMs) >= LIVE_MS) {
    lastLiveMs = millis();
    printRaw();
  }
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  h             this help"));
  Serial.println(F("  r             lux once"));
  Serial.println(F("  live          toggle ~2 Hz live readout"));
  Serial.println(F("  ref <value>   reference lux meter -> lux offset"));
  Serial.println(F("  show          print current offset"));
}

float readLux() {
  float lux = lightMeter.readLightLevel();
  return (lux < 0.0f) ? -1.0f : lux;
}

void printRaw() {
  float lux = readLux();
  if (lux < 0) { Serial.println(F("BH1750 read FAILED")); return; }
  Serial.print(F("lux = ")); Serial.print(lux, 0);
  Serial.print(F("  (corr ")); Serial.print(lux + luxOffset, 0); Serial.println(F(")"));
}

void handleLine(char *s) {
  char *t1 = strtok(s, " ");
  if (!t1) return;

  if (!strcasecmp(t1, "h"))    { printHelp(); return; }
  if (!strcasecmp(t1, "r"))    { printRaw();  return; }
  if (!strcasecmp(t1, "live")) { liveOn = !liveOn; Serial.print(F("live=")); Serial.println(liveOn ? F("ON") : F("OFF")); return; }
  if (!strcasecmp(t1, "show")) { Serial.print(F("lux offset = ")); Serial.println(luxOffset, 1); return; }
  if (!strcasecmp(t1, "ref")) {
    char *t2 = strtok(NULL, " ");
    if (!t2) { Serial.println(F("usage: ref <value>")); return; }
    float ref = atof(t2);
    float lux = readLux();
    if (lux < 0) { Serial.println(F("BH1750 read FAILED")); return; }
    luxOffset = ref - lux;
    Serial.print(F("lux offset = ")); Serial.print(luxOffset, 1);
    Serial.print(F("  (ref ")); Serial.print(ref, 0); Serial.print(F(" - sensor ")); Serial.print(lux, 0); Serial.println(F(")"));
    return;
  }
  Serial.println(F("? unknown command -- type h"));
}
