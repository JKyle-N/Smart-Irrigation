/* =============================================================================
 *  SMART IRRIGATION -- CALIBRATION TOOL -- BH1750 LIGHT (Arduino Nano)
 * -----------------------------------------------------------------------------
 *  A lux offset against a reference lux meter. It stays 0 unless a reference says otherwise. ESP1 adds it to
 *  what the Nano reports. The firmware's model is an added offset, not a scale, so take it at the light
 *  level you care about most.
 *
 *  HOW TO USE
 *    1. Meter beside the sensor, steady light.    ref 12500
 *    2. result                                    prints the line for CalStore
 *    Also: r, live, h.
 *
 *  BUS as in Nano/Nano.ino: A4 (SDA), A5 (SCL). Disconnect D0/D1 while flashing and while using this tool.
 * ========================================================================== */
#include <Arduino.h>
#include <Wire.h>
#include <BH1750.h>

BH1750 lightMeter;
bool sensorOk = false;

const unsigned long LIVE_MS = 1000;
const uint8_t AVG_READS = 8;
float luxOffset = 0.0f;
bool  done = false;
bool  liveOn = false;
unsigned long lastLiveMs = 0;

#define LINE_BUF 32
char    line[LINE_BUF];
uint8_t lineLen = 0;

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  ref <value>   your lux meter's reading: works out the offset"));
  Serial.println(F("  result        line to paste into CalStore"));
  Serial.println(F("  r / live      reading now, and live on/off"));
}

// Mean of fresh readings, about 1.5 s. -1 if any failed.
float readLux() {
  float sum = 0;
  for (uint8_t i = 0; i < AVG_READS; i++) {
    delay(180);                        // a high-resolution conversion takes up to 180 ms
    float lux = lightMeter.readLightLevel();
    if (lux < 0.0f) return -1.0f;
    sum += lux;
  }
  return sum / AVG_READS;
}

void printRaw() {
  float lux = readLux();
  if (lux < 0) { Serial.println(F("BH1750 read FAILED: check A4/A5 and its power.")); return; }
  Serial.print(F("lux=")); Serial.print(lux + luxOffset, 0); Serial.println(F("  (offset applied)"));
}

void handleLine(char *s) {
  char *t1 = strtok(s, " ");
  if (!t1) return;
  if (!strcasecmp(t1, "h") || !strcasecmp(t1, "help")) { printHelp(); return; }
  if (!strcasecmp(t1, "r"))    { printRaw(); return; }
  if (!strcasecmp(t1, "live")) { liveOn = !liveOn; Serial.println(liveOn ? F("live ON") : F("live OFF")); return; }
  if (!strcasecmp(t1, "result") || !strcasecmp(t1, "show")) {
    if (!done) { Serial.println(F("Nothing measured yet: ref <your meter's reading>")); return; }
    Serial.println(F("======== RESULT ========"));
    Serial.println(F("CalStore on ESP1 (flash Calibration/ESP1/CalStore, paste, flash the firmware back):"));
    Serial.print(F("  set lOff ")); Serial.println(luxOffset, 1);
    Serial.println(F("========================"));
    return;
  }
  if (!strcasecmp(t1, "ref")) {
    char *t2 = strtok(NULL, " ");
    char *end = NULL;
    float ref = t2 ? (float)strtod(t2, &end) : -1.0f;
    if (!t2 || *end != '\0' || ref < 0.0f) { Serial.println(F("usage: ref <lux from your meter>")); return; }
    float lux = readLux();
    if (lux < 0) { Serial.println(F("BH1750 read FAILED: check A4/A5 and its power.")); return; }
    luxOffset = ref - lux; done = true;
    Serial.print(F("Sensor ")); Serial.print(lux, 0); Serial.print(F(" lux, offset = ")); Serial.println(luxOffset, 1);
    Serial.println(F("NEXT: type result"));
    return;
  }
  Serial.println(F("Unknown command: type h"));
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  sensorOk = lightMeter.begin();
  Serial.println();
  Serial.println(F("=== BH1750 LIGHT CALIBRATION (Nano) ==="));
  if (!sensorOk) Serial.println(F("BH1750 NOT FOUND: check A4/A5 and its power, then reset this board."));
  printHelp();
  Serial.println(F("START: lux meter beside the sensor in steady light, then: ref <value>"));
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) { line[lineLen] = '\0'; lineLen = 0; handleLine(line); }
    } else if (lineLen < LINE_BUF - 1) line[lineLen++] = c;
  }
  if (liveOn && (unsigned long)(millis() - lastLiveMs) >= LIVE_MS) { lastLiveMs = millis(); printRaw(); }
}
