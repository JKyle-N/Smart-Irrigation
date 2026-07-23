/* =============================================================================
 *  SMART IRRIGATION  --  CALIBRATION BENCH TOOL  --  ULTRASONIC (Arduino Nano)
 * -----------------------------------------------------------------------------
 *  Standalone. No ESP1, no UART. Measures the empty-tank REFERENCE DISTANCE
 *  (mounting / zero offset) for the two HC-SR04 tank sensors. One-point cal per
 *  spec sec.A.4: with the tank empty, the sensor->surface distance is the zero
 *  reference the firmware subtracts to derive fill level.
 *
 *  PINS (match Nano/Nano.ino:80-83):
 *    Reservoir : TRIG D4, ECHO D5      Mixing : TRIG D6, ECHO D7
 *  Distance formula matches the firmware exactly (Nano.ino:131-132,462-469):
 *    distance_cm = echo_us * (0.0343 / 2),  pulseIn timeout 25000 us (~4.3 m).
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    r                 print both distances once (median of a few pings)
 *    live              toggle ~2 Hz live readout
 *    zero res          capture reservoir empty-tank reference distance
 *    zero mix          capture mixing-tank empty-tank reference distance
 *    show              print both captured references
 *
 *  Measure the EMPTY tank (or hold a tape to the known surface) and `zero` it.
 *  Paste the reference distances into ESP32 #1's ultrasonic geometry.
 *
 *  Build: Arduino IDE (Arduino Nano, ATmega328P) or `pio run` in this folder.
 * ========================================================================== */
#include <Arduino.h>

const uint8_t PIN_TRIG_RES = 4, PIN_ECHO_RES = 5;
const uint8_t PIN_TRIG_MIX = 6, PIN_ECHO_MIX = 7;

const float         SOUND_CM_PER_US       = 0.0343f / 2.0f;   // round-trip -> one way
const unsigned long ULTRASONIC_TIMEOUT_US = 25000UL;         // ~4.3 m cap, bounds pulseIn
const unsigned long LIVE_MS = 500;

float zeroRes = -1.0f, zeroMix = -1.0f;   // captured empty-tank references (cm)
bool  liveOn = false;
unsigned long lastLiveMs = 0;

#define LINE_BUF 48
char    line[LINE_BUF];
uint8_t lineLen = 0;

void printHelp();
float readDistanceCm(uint8_t trig, uint8_t echo);
float readMedianCm(uint8_t trig, uint8_t echo);
void  printRaw();
void  handleLine(char *s);

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG_RES, OUTPUT); digitalWrite(PIN_TRIG_RES, LOW); pinMode(PIN_ECHO_RES, INPUT);
  pinMode(PIN_TRIG_MIX, OUTPUT); digitalWrite(PIN_TRIG_MIX, LOW); pinMode(PIN_ECHO_MIX, INPUT);
  Serial.println();
  Serial.println(F("=== ULTRASONIC CALIBRATION (Nano) ==="));
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
  Serial.println(F("  h            this help"));
  Serial.println(F("  r            distances once"));
  Serial.println(F("  live         toggle ~2 Hz live readout"));
  Serial.println(F("  zero res     capture reservoir empty reference"));
  Serial.println(F("  zero mix     capture mixing empty reference"));
  Serial.println(F("  show         print captured references"));
}

// Raw one-way distance (cm) or -1 on timeout -- identical to the firmware.
float readDistanceCm(uint8_t trig, uint8_t echo) {
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long dur = pulseIn(echo, HIGH, ULTRASONIC_TIMEOUT_US);
  if (dur == 0) return -1.0f;
  return (float)dur * SOUND_CM_PER_US;
}

// Median of 5 pings (drops the odd dropout/echo) for a steadier capture.
float readMedianCm(uint8_t trig, uint8_t echo) {
  float v[5];
  uint8_t got = 0;
  for (uint8_t i = 0; i < 5; i++) {
    float d = readDistanceCm(trig, echo);
    if (d >= 0.0f) v[got++] = d;
    delay(60);                  // ~60 ms between pings (bounded, one-shot capture; not in the hot loop)
  }
  if (got == 0) return -1.0f;
  // simple insertion sort of the valid samples
  for (uint8_t i = 1; i < got; i++) {
    float key = v[i]; int8_t j = i - 1;
    while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; j--; }
    v[j + 1] = key;
  }
  return v[got / 2];
}

void printRaw() {
  float res = readMedianCm(PIN_TRIG_RES, PIN_ECHO_RES);
  float mix = readMedianCm(PIN_TRIG_MIX, PIN_ECHO_MIX);
  Serial.print(F("RES = "));
  if (res < 0) Serial.print(F("timeout")); else { Serial.print(res, 1); Serial.print(F(" cm")); }
  Serial.print(F("   MIX = "));
  if (mix < 0) Serial.print(F("timeout")); else { Serial.print(mix, 1); Serial.print(F(" cm")); }
  Serial.println();
}

void handleLine(char *s) {
  char *t1 = strtok(s, " ");
  if (!t1) return;

  if (!strcasecmp(t1, "h"))    { printHelp(); return; }
  if (!strcasecmp(t1, "r"))    { printRaw();  return; }
  if (!strcasecmp(t1, "live")) { liveOn = !liveOn; Serial.print(F("live=")); Serial.println(liveOn ? F("ON") : F("OFF")); return; }
  if (!strcasecmp(t1, "show")) {
    Serial.print(F("zero_res = ")); if (zeroRes < 0) Serial.println(F("(uncaptured)")); else { Serial.print(zeroRes, 1); Serial.println(F(" cm")); }
    Serial.print(F("zero_mix = ")); if (zeroMix < 0) Serial.println(F("(uncaptured)")); else { Serial.print(zeroMix, 1); Serial.println(F(" cm")); }
    return;
  }
  if (!strcasecmp(t1, "zero")) {
    char *t2 = strtok(NULL, " ");
    if (!t2) { Serial.println(F("usage: zero <res|mix>")); return; }
    if (!strcasecmp(t2, "res")) {
      zeroRes = readMedianCm(PIN_TRIG_RES, PIN_ECHO_RES);
      Serial.print(F("reservoir empty reference = ")); Serial.print(zeroRes, 1); Serial.println(F(" cm"));
      return;
    }
    if (!strcasecmp(t2, "mix")) {
      zeroMix = readMedianCm(PIN_TRIG_MIX, PIN_ECHO_MIX);
      Serial.print(F("mixing empty reference = ")); Serial.print(zeroMix, 1); Serial.println(F(" cm"));
      return;
    }
    Serial.println(F("usage: zero <res|mix>"));
    return;
  }
  Serial.println(F("? unknown command -- type h"));
}
