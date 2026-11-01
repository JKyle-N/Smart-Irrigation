/* =============================================================================
 *  SMART IRRIGATION -- CALIBRATION TOOL -- ULTRASONIC TANK LEVEL (Arduino Nano)
 * -----------------------------------------------------------------------------
 *  ESP1 works out a tank's % from two distances, sensor to water surface: one when EMPTY and one when FULL.
 *  The LCD's Calibration menu only captures empty. This captures both.
 *
 *  HOW TO USE (<t> = res or mix; the tool tells you the next step)
 *    1. Tank empty.                  empty res
 *    2. Tank at your full level.     full res
 *       (No need to fill it: `full res 11` takes a distance you measured with a tape.)
 *    3. result                       prints the lines for CalStore
 *    Also: r, live, h.
 *
 *  PINS and distance formula as in Nano/Nano.ino: reservoir TRIG D4, ECHO D5; mixing TRIG D6, ECHO D7.
 *  Disconnect D0/D1 (the link to ESP1) while flashing and while using this tool.
 * ========================================================================== */
#include <Arduino.h>

const uint8_t PIN_TRIG[2] = { 4, 6 }, PIN_ECHO[2] = { 5, 7 };
const char *const TANK_NAME[2] = { "res", "mix" };

const float         SOUND_CM_PER_US = 0.0343f / 2.0f;
const unsigned long TIMEOUT_US = 25000UL;
const unsigned long LIVE_MS = 1000;
const float MIN_RANGE_CM = 2.0f;        // an HC-SR04 cannot range closer
const float MIN_SPAN_CM  = 3.0f;        // empty and full closer than this make a useless scale

float emptyCm[2] = { -1.0f, -1.0f }, fullCm[2] = { -1.0f, -1.0f };
bool  liveOn = false;
unsigned long lastLiveMs = 0;

#define LINE_BUF 32
char    line[LINE_BUF];
uint8_t lineLen = 0;

float readDistanceCm(uint8_t t) {
  digitalWrite(PIN_TRIG[t], LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG[t], HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG[t], LOW);
  unsigned long dur = pulseIn(PIN_ECHO[t], HIGH, TIMEOUT_US);
  if (dur == 0) return -1.0f;
  return (float)dur * SOUND_CM_PER_US;
}

// Median of up to 9 pings. -1 unless at least 5 came back, so a flaky echo cannot become a calibration.
float readMedianCm(uint8_t t) {
  float v[9];
  uint8_t got = 0;
  for (uint8_t i = 0; i < 9; i++) {
    float d = readDistanceCm(t);
    if (d >= 0.0f) v[got++] = d;
    delay(60);
  }
  if (got < 5) return -1.0f;
  for (uint8_t i = 1; i < got; i++) { float key = v[i]; int8_t j = i - 1; while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; j--; } v[j + 1] = key; }
  return v[got / 2];
}

void printHelp() {
  Serial.println(F("Commands (<t> = res or mix):"));
  Serial.println(F("  empty <t>        tank empty: capture the distance"));
  Serial.println(F("  full <t> [cm]    tank full: capture it, or give a distance you measured"));
  Serial.println(F("  result           lines to paste into CalStore"));
  Serial.println(F("  r / live         distances now, and live on/off"));
}

void printRaw() {
  for (uint8_t t = 0; t < 2; t++) {
    float d = readMedianCm(t);
    Serial.print(TANK_NAME[t]); Serial.print(F(" = "));
    if (d < 0) Serial.print(F("no echo")); else { Serial.print(d, 1); Serial.print(F(" cm")); }
    Serial.print(F("   "));
  }
  Serial.println();
}

int tankFrom(const char *s) {
  if (!s) return -1;
  if (!strcasecmp(s, "res")) return 0;
  if (!strcasecmp(s, "mix")) return 1;
  return -1;
}

void captured(uint8_t t, bool isEmpty, float d) {
  if (isEmpty) emptyCm[t] = d; else fullCm[t] = d;
  Serial.print(TANK_NAME[t]); Serial.print(isEmpty ? F(" empty = ") : F(" full = ")); Serial.print(d, 1); Serial.println(F(" cm"));
  if (emptyCm[t] >= 0 && fullCm[t] >= 0) {
    if (emptyCm[t] - fullCm[t] < MIN_SPAN_CM) {
      Serial.println(F("REFUSED: empty must be further from the sensor than full by at least 3 cm. Capture both again."));
      emptyCm[t] = fullCm[t] = -1.0f;
      return;
    }
    Serial.println(F("NEXT: the other tank, or type result"));
  } else {
    Serial.print(isEmpty ? F("NEXT: at the full level type: full ") : F("NEXT: with the tank empty type: empty "));
    Serial.println(TANK_NAME[t]);
  }
}

void printResult() {
  const char *const EK[2] = { "reCm", "meCm" }, *const FK[2] = { "rfCm", "mfCm" };
  bool any = false;
  for (uint8_t t = 0; t < 2; t++) if (emptyCm[t] >= 0 || fullCm[t] >= 0) any = true;
  if (!any) { Serial.println(F("Nothing captured yet. Start with: empty res")); return; }
  Serial.println(F("======== RESULT ========"));
  Serial.println(F("CalStore on ESP1 (flash Calibration/ESP1/CalStore, paste, flash the firmware back):"));
  for (uint8_t t = 0; t < 2; t++) {
    if (emptyCm[t] >= 0) { Serial.print(F("  set ")); Serial.print(EK[t]); Serial.print(' '); Serial.println(emptyCm[t], 1); }
    if (fullCm[t]  >= 0) { Serial.print(F("  set ")); Serial.print(FK[t]); Serial.print(' '); Serial.println(fullCm[t], 1); }
  }
  Serial.println(F("========================"));
}

void handleLine(char *s) {
  char *t1 = strtok(s, " ");
  if (!t1) return;
  if (!strcasecmp(t1, "h") || !strcasecmp(t1, "help")) { printHelp(); return; }
  if (!strcasecmp(t1, "r"))      { printRaw(); return; }
  if (!strcasecmp(t1, "result") || !strcasecmp(t1, "show")) { printResult(); return; }
  if (!strcasecmp(t1, "live"))   { liveOn = !liveOn; Serial.println(liveOn ? F("live ON") : F("live OFF")); return; }
  bool isEmpty = !strcasecmp(t1, "empty") || !strcasecmp(t1, "zero");
  bool isFull  = !strcasecmp(t1, "full");
  if (!isEmpty && !isFull) { Serial.println(F("Unknown command: type h")); return; }
  int t = tankFrom(strtok(NULL, " "));
  if (t < 0) { Serial.println(F("Name a tank: empty res  or  empty mix")); return; }
  char *given = strtok(NULL, " ");
  float d;
  if (given) {
    d = atof(given);
    if (d < MIN_RANGE_CM || d > 400.0f) { Serial.println(F("usage: full <res|mix> <cm from the sensor to the water, 2 to 400>")); return; }
  } else {
    d = readMedianCm(t);
    if (d < 0) { Serial.println(F("REFUSED: not enough echoes came back. Check the wiring and that the sensor faces the water squarely.")); return; }
    if (d < MIN_RANGE_CM) { Serial.println(F("REFUSED: closer than the sensor can range.")); return; }
  }
  captured(t, isEmpty, d);
}

void setup() {
  Serial.begin(115200);
  for (uint8_t t = 0; t < 2; t++) { pinMode(PIN_TRIG[t], OUTPUT); digitalWrite(PIN_TRIG[t], LOW); pinMode(PIN_ECHO[t], INPUT); }
  Serial.println();
  Serial.println(F("=== ULTRASONIC TANK LEVEL CALIBRATION (Nano) ==="));
  printHelp();
  Serial.println(F("START: reservoir empty. Type: empty res"));
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
