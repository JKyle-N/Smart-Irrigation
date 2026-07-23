/* =============================================================================
 *  SMART IRRIGATION  --  CALIBRATION BENCH TOOL  --  SOIL MOISTURE (Arduino Nano)
 * -----------------------------------------------------------------------------
 *  Standalone. No ESP1, no UART link, no framed packets -- just a human over USB
 *  serial. Measures the per-column capacitive soil endpoints (air_raw / water_raw)
 *  that the REAL firmware uses to map raw ADC -> 0..100 %. Two-point calibration
 *  (companion spec Settings_Calibration_and_Safe_Edit_Spec.md sec.A.4): one point
 *  dry-in-air, one point in water / saturated soil, per column.
 *
 *  PINS (match Nano/Nano.ino:91-95 -- do NOT invent pins):
 *    Column A: A0, A1   Column B: A2, A3   Column C: A6, A7   (2 sensors/column,
 *    averaged). A6/A7 are analog-input-only. The firmware maps the COLUMN AVERAGE,
 *    so this tool captures the 64-sample mean of a column's two sensors.
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                     help
 *    r                     print raw ADC once (all 6 sensors + per-column average)
 *    live                  toggle the ~2 Hz live readout on/off
 *    soil <A|B|C> air      capture this column's AIR endpoint (probe dry in air)
 *    soil <A|B|C> water    capture this column's WATER endpoint (probe in water)
 *    soil <A|B|C> calc     print air_raw/water_raw + a sanity 0..100% for that column
 *    x <A|B|C>             clear captured endpoints for a column
 *
 *  RESULT: paste the printed endpoints into ESP32 #1's soil calibration (they live
 *  on ESP1 now -- the Nano streams RAW). Capacitive sensors usually read HIGHER in
 *  air (dry) than in water (wet); the map handles either ordering.
 *
 *  Build: Arduino IDE (Board: Arduino Nano, ATmega328P) or `pio run` in this folder.
 * ========================================================================== */
#include <Arduino.h>

const uint8_t NUM_COLUMNS = 3;
const char    COLUMN_TAG[NUM_COLUMNS] = { 'A', 'B', 'C' };
const uint8_t SOIL_PINS[NUM_COLUMNS][2] = {
  { A0, A1 },   // Column A
  { A2, A3 },   // Column B
  { A6, A7 },   // Column C
};

const uint8_t SAMPLES  = 64;          // per-reading average
const unsigned long LIVE_MS = 500;    // ~2 Hz live readout

int  airRaw[NUM_COLUMNS];             // -1 = not captured
int  waterRaw[NUM_COLUMNS];
bool liveOn = false;
unsigned long lastLiveMs = 0;

#define LINE_BUF 48
char    line[LINE_BUF];
uint8_t lineLen = 0;

void printHelp();
void printRaw();
int  sampleMean(uint8_t pin);
int  columnMean(uint8_t col);
void handleLine(char *s);

void setup() {
  Serial.begin(115200);
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) { airRaw[c] = -1; waterRaw[c] = -1; }
  Serial.println();
  Serial.println(F("=== SOIL MOISTURE CALIBRATION (Nano) ==="));
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
      lineLen = 0;                    // overflow -> drop
    }
  }

  if (liveOn && (unsigned long)(millis() - lastLiveMs) >= LIVE_MS) {
    lastLiveMs = millis();
    printRaw();
  }
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  h                  this help"));
  Serial.println(F("  r                  raw ADC once (6 sensors + column avg)"));
  Serial.println(F("  live               toggle ~2 Hz live readout"));
  Serial.println(F("  soil <A|B|C> air   capture AIR endpoint  (probe dry in air)"));
  Serial.println(F("  soil <A|B|C> water capture WATER endpoint (probe in water)"));
  Serial.println(F("  soil <A|B|C> calc  show endpoints + sanity 0..100%"));
  Serial.println(F("  x <A|B|C>          clear a column's endpoints"));
}

int sampleMean(uint8_t pin) {
  long sum = 0;
  for (uint8_t i = 0; i < SAMPLES; i++) sum += analogRead(pin);
  return (int)(sum / SAMPLES);
}

int columnMean(uint8_t col) {
  return (sampleMean(SOIL_PINS[col][0]) + sampleMean(SOIL_PINS[col][1])) / 2;
}

// Map a column's raw average to 0..100% using its captured endpoints.
// Returns -1 if not yet calibrated. Handles air>water or water>air ordering.
int columnPercent(uint8_t col, int raw) {
  if (airRaw[col] < 0 || waterRaw[col] < 0 || airRaw[col] == waterRaw[col]) return -1;
  long pct = (long)(raw - airRaw[col]) * 100L / (waterRaw[col] - airRaw[col]);
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  return (int)pct;
}

void printRaw() {
  Serial.println(F("---- soil raw ----"));
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) {
    int s0 = sampleMean(SOIL_PINS[c][0]);
    int s1 = sampleMean(SOIL_PINS[c][1]);
    int avg = (s0 + s1) / 2;
    int pct = columnPercent(c, avg);
    Serial.print(F("Col ")); Serial.print(COLUMN_TAG[c]);
    Serial.print(F(": s1=")); Serial.print(s0);
    Serial.print(F(" s2="));  Serial.print(s1);
    Serial.print(F(" avg=")); Serial.print(avg);
    if (pct >= 0) { Serial.print(F(" (")); Serial.print(pct); Serial.print(F("%)")); }
    else          { Serial.print(F(" (uncal)")); }
    Serial.println();
  }
}

// Parse a single A/B/C letter to a column index, or -1.
int colFromChar(char ch) {
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) if (COLUMN_TAG[c] == ch) return c;
  return -1;
}

void handleLine(char *s) {
  // tokenize on spaces
  char *t1 = strtok(s, " ");
  if (!t1) return;

  if (!strcasecmp(t1, "h"))    { printHelp(); return; }
  if (!strcasecmp(t1, "r"))    { printRaw();  return; }
  if (!strcasecmp(t1, "live")) { liveOn = !liveOn; Serial.print(F("live=")); Serial.println(liveOn ? F("ON") : F("OFF")); return; }

  if (!strcasecmp(t1, "x")) {
    char *t2 = strtok(NULL, " ");
    int col = t2 ? colFromChar(t2[0]) : -1;
    if (col < 0) { Serial.println(F("usage: x <A|B|C>")); return; }
    airRaw[col] = -1; waterRaw[col] = -1;
    Serial.print(F("cleared column ")); Serial.println(COLUMN_TAG[col]);
    return;
  }

  if (!strcasecmp(t1, "soil")) {
    char *t2 = strtok(NULL, " ");
    char *t3 = strtok(NULL, " ");
    int col = t2 ? colFromChar(t2[0]) : -1;
    if (col < 0 || !t3) { Serial.println(F("usage: soil <A|B|C> <air|water|calc>")); return; }

    if (!strcasecmp(t3, "air")) {
      airRaw[col] = columnMean(col);
      Serial.print(F("Col ")); Serial.print(COLUMN_TAG[col]);
      Serial.print(F(" AIR raw = ")); Serial.println(airRaw[col]);
      return;
    }
    if (!strcasecmp(t3, "water")) {
      waterRaw[col] = columnMean(col);
      Serial.print(F("Col ")); Serial.print(COLUMN_TAG[col]);
      Serial.print(F(" WATER raw = ")); Serial.println(waterRaw[col]);
      return;
    }
    if (!strcasecmp(t3, "calc")) {
      if (airRaw[col] < 0 || waterRaw[col] < 0) {
        Serial.println(F("capture BOTH air and water first."));
        return;
      }
      int now = columnMean(col);
      int pct = columnPercent(col, now);
      Serial.print(F("== Column ")); Serial.print(COLUMN_TAG[col]); Serial.println(F(" ENDPOINTS =="));
      Serial.print(F("  air_raw   = ")); Serial.println(airRaw[col]);
      Serial.print(F("  water_raw = ")); Serial.println(waterRaw[col]);
      Serial.print(F("  (now raw=")); Serial.print(now);
      Serial.print(F(" -> ")); Serial.print(pct); Serial.println(F("%  sanity check)"));
      Serial.println(F("  Paste these endpoints into ESP32 #1's soil calibration for this column."));
      return;
    }
    Serial.println(F("usage: soil <A|B|C> <air|water|calc>"));
    return;
  }

  Serial.println(F("? unknown command -- type h"));
}
