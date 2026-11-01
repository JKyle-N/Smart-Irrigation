/* =============================================================================
 *  SMART IRRIGATION -- CALIBRATION TOOL -- DHT22 (Arduino Nano)
 * -----------------------------------------------------------------------------
 *  Temperature and humidity offsets against a reference instrument. They stay 0 unless a reference says
 *  otherwise. ESP1 adds them to what the Nano reports.
 *
 *  HOW TO USE
 *    Put the sensor beside your reference and give them 10 minutes.
 *    1. ref t 28.4      your thermometer's reading
 *    2. ref h 62        your hygrometer's reading
 *    3. result          prints the lines for CalStore
 *    Also: r, live, h.
 *
 *  PIN as in Nano/Nano.ino: DHT22 data on D11. Disconnect D0/D1 while flashing and while using this tool.
 * ========================================================================== */
#include <Arduino.h>
#include <DHT.h>

const uint8_t PIN_DHT = 11;
DHT dht(PIN_DHT, DHT22);

const unsigned long LIVE_MS = 2500;   // a DHT22 gives a new reading every 2 s at best
const uint8_t AVG_READS = 4;
const float MAX_T_OFF = 10.0f, MAX_H_OFF = 25.0f;   // further out, suspect the setup rather than the sensor

float tempOffset = 0.0f, humOffset = 0.0f;
bool  tempDone = false, humDone = false;
bool  liveOn = false;
unsigned long lastLiveMs = 0;

#define LINE_BUF 32
char    line[LINE_BUF];
uint8_t lineLen = 0;

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  ref t <value>   your thermometer: works out the temperature offset"));
  Serial.println(F("  ref h <value>   your hygrometer: works out the humidity offset"));
  Serial.println(F("  result          lines to paste into CalStore"));
  Serial.println(F("  r / live        reading now, and live on/off"));
}

void printRaw() {
  float t = dht.readTemperature(), h = dht.readHumidity();
  if (isnan(t) || isnan(h)) { Serial.println(F("DHT read FAILED: check D11 and its power.")); return; }
  Serial.print(F("T=")); Serial.print(t + tempOffset, 1); Serial.print(F(" C   H=")); Serial.print(h + humOffset, 1); Serial.println(F(" %  (offsets applied)"));
}

// Mean of a few fresh readings, about 8 s. NAN if any of them failed.
float averaged(bool temp) {
  float sum = 0;
  Serial.println(F("Reading, about 8 s..."));
  for (uint8_t i = 0; i < AVG_READS; i++) {
    delay(2100);
    float v = temp ? dht.readTemperature() : dht.readHumidity();
    if (isnan(v)) return NAN;
    sum += v;
  }
  return sum / AVG_READS;
}

void printResult() {
  if (!tempDone && !humDone) { Serial.println(F("Nothing measured yet. Start with: ref t <value>")); return; }
  Serial.println(F("======== RESULT ========"));
  Serial.println(F("CalStore on ESP1 (flash Calibration/ESP1/CalStore, paste, flash the firmware back):"));
  if (tempDone) { Serial.print(F("  set tOff ")); Serial.println(tempOffset, 2); }
  if (humDone)  { Serial.print(F("  set hOff ")); Serial.println(humOffset, 2); }
  Serial.println(F("========================"));
}

void handleLine(char *s) {
  char *t1 = strtok(s, " ");
  if (!t1) return;
  if (!strcasecmp(t1, "h") || !strcasecmp(t1, "help")) { printHelp(); return; }
  if (!strcasecmp(t1, "r"))      { printRaw(); return; }
  if (!strcasecmp(t1, "result") || !strcasecmp(t1, "show")) { printResult(); return; }
  if (!strcasecmp(t1, "live"))   { liveOn = !liveOn; Serial.println(liveOn ? F("live ON") : F("live OFF")); return; }
  if (!strcasecmp(t1, "ref")) {
    char *t2 = strtok(NULL, " "), *t3 = strtok(NULL, " ");
    bool isT = t2 && !strcasecmp(t2, "t"), isH = t2 && !strcasecmp(t2, "h");
    char *end = NULL;
    float ref = t3 ? (float)strtod(t3, &end) : 0.0f;
    if ((!isT && !isH) || !t3 || *end != '\0') { Serial.println(F("usage: ref t <value>  or  ref h <value>")); return; }
    float v = averaged(isT);
    if (isnan(v)) { Serial.println(F("DHT read FAILED: check D11 and its power.")); return; }
    float off = ref - v;
    if (fabs(off) > (isT ? MAX_T_OFF : MAX_H_OFF)) {
      Serial.print(F("REFUSED: the sensor reads ")); Serial.print(v, 1); Serial.print(F(" against your ")); Serial.print(ref, 1);
      Serial.println(F(". Too far apart to be a trim: check the number and let them settle together."));
      return;
    }
    if (isT) { tempOffset = off; tempDone = true; } else { humOffset = off; humDone = true; }
    Serial.print(isT ? F("Temperature offset = ") : F("Humidity offset = ")); Serial.println(off, 2);
    if (tempDone && humDone) Serial.println(F("NEXT: type result"));
    else Serial.println(isT ? F("NEXT: ref h <your hygrometer's reading>") : F("NEXT: ref t <your thermometer's reading>"));
    return;
  }
  Serial.println(F("Unknown command: type h"));
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  Serial.println();
  Serial.println(F("=== DHT22 CALIBRATION (Nano) ==="));
  printHelp();
  Serial.println(F("START: sensor beside your reference for 10 minutes, then: ref t <value>"));
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
