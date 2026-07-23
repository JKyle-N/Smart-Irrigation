/* =============================================================================
 *  SMART IRRIGATION  --  CALIBRATION BENCH TOOL  --  DHT22 (Arduino Nano)
 * -----------------------------------------------------------------------------
 *  Standalone. No ESP1, no UART. Measures the temperature & humidity OFFSET trims
 *  (spec sec.A.4: offset vs. a reference thermo-hygrometer; default 0). ESP32 #1
 *  applies these offsets when it interprets the Nano's ENV packet.
 *
 *  PIN (match Nano/Nano.ino:87,165): DHT22 data D11.
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    r                 print temp/hum once
 *    live              toggle ~2 Hz live readout
 *    ref t <value>     enter your reference thermometer reading -> temp offset
 *    ref h <value>     enter your reference hygrometer reading   -> humidity offset
 *    show              print current offsets
 *
 *  offset = reference - sensor. Add it to the sensor reading to correct.
 *  Paste temp/hum offsets into ESP32 #1's ENV offset trims (default 0).
 *
 *  Libraries (Arduino IDE: install "DHT sensor library" + "Adafruit Unified Sensor").
 *  Build: Arduino IDE (Arduino Nano, ATmega328P) or `pio run` in this folder.
 * ========================================================================== */
#include <Arduino.h>
#include <DHT.h>

const uint8_t PIN_DHT = 11;
DHT dht(PIN_DHT, DHT22);

const unsigned long LIVE_MS = 2000;   // DHT22 updates ~0.5 Hz; don't poll faster
float tempOffset = 0.0f, humOffset = 0.0f;
bool  liveOn = false;
unsigned long lastLiveMs = 0;

#define LINE_BUF 48
char    line[LINE_BUF];
uint8_t lineLen = 0;

void printHelp();
void printRaw();
void handleLine(char *s);

void setup() {
  Serial.begin(115200);
  dht.begin();
  Serial.println();
  Serial.println(F("=== DHT22 CALIBRATION (Nano) ==="));
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
  Serial.println(F("  h               this help"));
  Serial.println(F("  r               temp/hum once"));
  Serial.println(F("  live            toggle ~0.5 Hz live readout"));
  Serial.println(F("  ref t <value>   reference thermometer -> temp offset"));
  Serial.println(F("  ref h <value>   reference hygrometer   -> humidity offset"));
  Serial.println(F("  show            print current offsets"));
}

void printRaw() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) { Serial.println(F("DHT read FAILED (check wiring/power)")); return; }
  Serial.print(F("T = ")); Serial.print(t, 1); Serial.print(F(" C"));
  Serial.print(F("  (corr ")); Serial.print(t + tempOffset, 1); Serial.print(F(" C)"));
  Serial.print(F("   H = ")); Serial.print(h, 1); Serial.print(F(" %"));
  Serial.print(F("  (corr ")); Serial.print(h + humOffset, 1); Serial.println(F(" %)"));
}

void handleLine(char *s) {
  char *t1 = strtok(s, " ");
  if (!t1) return;

  if (!strcasecmp(t1, "h"))    { printHelp(); return; }
  if (!strcasecmp(t1, "r"))    { printRaw();  return; }
  if (!strcasecmp(t1, "live")) { liveOn = !liveOn; Serial.print(F("live=")); Serial.println(liveOn ? F("ON") : F("OFF")); return; }
  if (!strcasecmp(t1, "show")) {
    Serial.print(F("temp offset = ")); Serial.println(tempOffset, 2);
    Serial.print(F("hum  offset = ")); Serial.println(humOffset, 2);
    return;
  }
  if (!strcasecmp(t1, "ref")) {
    char *t2 = strtok(NULL, " ");
    char *t3 = strtok(NULL, " ");
    if (!t2 || !t3) { Serial.println(F("usage: ref <t|h> <value>")); return; }
    float ref = atof(t3);
    if (!strcasecmp(t2, "t")) {
      float t = dht.readTemperature();
      if (isnan(t)) { Serial.println(F("DHT read FAILED")); return; }
      tempOffset = ref - t;
      Serial.print(F("temp offset = ")); Serial.print(tempOffset, 2);
      Serial.print(F("  (ref ")); Serial.print(ref, 1); Serial.print(F(" - sensor ")); Serial.print(t, 1); Serial.println(F(")"));
      return;
    }
    if (!strcasecmp(t2, "h")) {
      float h = dht.readHumidity();
      if (isnan(h)) { Serial.println(F("DHT read FAILED")); return; }
      humOffset = ref - h;
      Serial.print(F("hum offset = ")); Serial.print(humOffset, 2);
      Serial.print(F("  (ref ")); Serial.print(ref, 1); Serial.print(F(" - sensor ")); Serial.print(h, 1); Serial.println(F(")"));
      return;
    }
    Serial.println(F("usage: ref <t|h> <value>"));
    return;
  }
  Serial.println(F("? unknown command -- type h"));
}
