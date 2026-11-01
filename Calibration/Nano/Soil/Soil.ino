/* =============================================================================
 *  SMART IRRIGATION -- CALIBRATION TOOL -- SOIL MOISTURE (Arduino Nano)
 * -----------------------------------------------------------------------------
 *  Measures each column's dry and wet readings. ESP1 uses them to turn the probes' raw ADC into 0..100 %.
 *
 *  HOW TO USE (for each column; the tool tells you the next step)
 *    1. Both probes clean and dry, in air.     air a
 *    2. Both probes in water.                  water a
 *    3. result                                 prints the lines for CalStore
 *    Also: r, live, x a (forget a column), h. The old `soil a air` form still works.
 *
 *  PINS, as in Nano/Nano.ino: column A = A0, A1; B = A2, A3; C = A6, A7.
 *  It works out a column the way the firmware does: the average of its two probes, or the one good probe
 *  if the other is at a rail (disconnected or shorted). ESP1 ignores a pair unless dry is above wet by at
 *  least 20 counts, so this refuses one that is not.
 *
 *  Disconnect D0/D1 (the link to ESP1) while flashing and while using this tool.
 * ========================================================================== */
#include <Arduino.h>

const uint8_t NUM_COLUMNS = 3;
const char    COLUMN_TAG[NUM_COLUMNS] = { 'A', 'B', 'C' };
const uint8_t SOIL_PINS[NUM_COLUMNS][2] = { { A0, A1 }, { A2, A3 }, { A6, A7 } };

const uint8_t SAMPLES = 64;
const unsigned long LIVE_MS = 1000;
const int RAIL_LO = 8, RAIL_HI = 1015;     // ESP1's SOIL_ADC_RAIL_LO/HI
const int MIN_SPAN = 20;                   // ESP1's CAL_MIN_SPAN_ADC
const int PROBES_DIFFER = 150;             // two probes of a column further apart than this are worth a look

int  airRaw[NUM_COLUMNS], waterRaw[NUM_COLUMNS];   // -1 = not captured
bool liveOn = false;
unsigned long lastLiveMs = 0;

#define LINE_BUF 32
char    line[LINE_BUF];
uint8_t lineLen = 0;

int sampleMean(uint8_t pin) {
  long sum = 0;
  analogRead(pin);                         // the first read after switching channels is unreliable
  for (uint8_t i = 0; i < SAMPLES; i++) sum += analogRead(pin);
  return (int)(sum / SAMPLES);
}

bool railed(int v) { return v <= RAIL_LO || v >= RAIL_HI; }

// The column's value as the firmware works it out, or -1 if both probes are at a rail.
int columnValue(uint8_t c, int s0, int s1) {
  if (!railed(s0) && !railed(s1)) return (s0 + s1) / 2;
  if (!railed(s0)) return s0;
  if (!railed(s1)) return s1;
  return -1;
}

void printHelp() {
  Serial.println(F("Commands (<c> = a, b or c):"));
  Serial.println(F("  air <c>     probes dry, in air: capture the dry reading"));
  Serial.println(F("  water <c>   probes in water: capture the wet reading"));
  Serial.println(F("  result      lines to paste into CalStore"));
  Serial.println(F("  r / live    readings now, and live on/off"));
  Serial.println(F("  x <c>       forget a column"));
}

void printRaw() {
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) {
    int s0 = sampleMean(SOIL_PINS[c][0]), s1 = sampleMean(SOIL_PINS[c][1]);
    int v = columnValue(c, s0, s1);
    Serial.print(F("Col ")); Serial.print(COLUMN_TAG[c]);
    Serial.print(F(": ")); Serial.print(s0);
    if (railed(s0)) Serial.print(F("(rail)"));
    Serial.print(F(" ")); Serial.print(s1);
    if (railed(s1)) Serial.print(F("(rail)"));
    Serial.print(F(" -> ")); Serial.print(v);
    if (v >= 0 && airRaw[c] >= 0 && waterRaw[c] >= 0 && airRaw[c] != waterRaw[c]) {
      long pct = (long)(v - airRaw[c]) * 100L / (waterRaw[c] - airRaw[c]);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      Serial.print(F(" = ")); Serial.print(pct); Serial.print('%');
    }
    Serial.println();
  }
}

int colFromChar(char ch) {
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) if (COLUMN_TAG[c] == ch) return c;
  return -1;
}

void capture(int col, bool isAir) {
  int s0 = sampleMean(SOIL_PINS[col][0]), s1 = sampleMean(SOIL_PINS[col][1]);
  int v = columnValue(col, s0, s1);
  if (v < 0) {
    Serial.print(F("REFUSED: both probes are at a rail (")); Serial.print(s0); Serial.print(F(", ")); Serial.print(s1);
    Serial.println(F("). Check their wiring. A column with no sensor connected cannot be calibrated."));
    return;
  }
  if (railed(s0) || railed(s1)) Serial.println(F("WARNING: one probe is at a rail, so this uses the other alone. Fix it and recapture if you can."));
  else if (abs(s0 - s1) > PROBES_DIFFER) {
    Serial.print(F("WARNING: the two probes differ by ")); Serial.print(abs(s0 - s1));
    Serial.println(F(". Check that both are at the same dryness."));
  }
  if (isAir) airRaw[col] = v; else waterRaw[col] = v;
  Serial.print(F("Col ")); Serial.print(COLUMN_TAG[col]); Serial.print(isAir ? F(" dry = ") : F(" wet = ")); Serial.println(v);
  char t = COLUMN_TAG[col] + 32;
  if (airRaw[col] >= 0 && waterRaw[col] >= 0) {
    if (airRaw[col] - waterRaw[col] < MIN_SPAN) {
      Serial.println(F("REFUSED: dry must read at least 20 above wet, and it does not. Check the probes, then capture both again."));
      airRaw[col] = waterRaw[col] = -1;
      return;
    }
    Serial.println(F("NEXT: another column, or type result"));
  } else {
    Serial.print(isAir ? F("NEXT: put both probes in water, then type: water ") : F("NEXT: dry both probes, then type: air "));
    Serial.println(t);
  }
}

void printResult() {
  bool any = false;
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) if (airRaw[c] >= 0 && waterRaw[c] >= 0) any = true;
  if (!any) { Serial.println(F("Nothing complete yet. Start with: air a")); return; }
  Serial.println(F("======== RESULT ========"));
  Serial.println(F("CalStore on ESP1 (flash Calibration/ESP1/CalStore, paste, flash the firmware back):"));
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) {
    if (airRaw[c] < 0 || waterRaw[c] < 0) continue;
    Serial.print(F("  set s")); Serial.print(c); Serial.print(F("a2 ")); Serial.println(airRaw[c]);
    Serial.print(F("  set s")); Serial.print(c); Serial.print(F("w2 ")); Serial.println(waterRaw[c]);
  }
  Serial.println(F("========================"));
}

void handleLine(char *s) {
  char *t1 = strtok(s, " ");
  if (!t1) return;
  if (!strcasecmp(t1, "h") || !strcasecmp(t1, "help")) { printHelp(); return; }
  if (!strcasecmp(t1, "r"))      { printRaw(); return; }
  if (!strcasecmp(t1, "result")) { printResult(); return; }
  if (!strcasecmp(t1, "live"))   { liveOn = !liveOn; Serial.println(liveOn ? F("live ON") : F("live OFF")); return; }

  char *t2 = strtok(NULL, " ");
  int col = t2 ? colFromChar(t2[0]) : -1;
  if (!strcasecmp(t1, "air") || !strcasecmp(t1, "water")) {
    if (col < 0) { Serial.println(F("Name a column: air a")); return; }
    capture(col, !strcasecmp(t1, "air"));
    return;
  }
  if (!strcasecmp(t1, "x")) {
    if (col < 0) { Serial.println(F("usage: x <a|b|c>")); return; }
    airRaw[col] = waterRaw[col] = -1;
    Serial.print(F("Column ")); Serial.print(COLUMN_TAG[col]); Serial.println(F(" forgotten."));
    return;
  }
  if (!strcasecmp(t1, "soil")) {                 // the old form: soil a air
    char *t3 = strtok(NULL, " ");
    if (col >= 0 && t3 && !strcasecmp(t3, "air"))   { capture(col, true);  return; }
    if (col >= 0 && t3 && !strcasecmp(t3, "water")) { capture(col, false); return; }
    if (col >= 0 && t3 && !strcasecmp(t3, "calc"))  { printResult();       return; }
  }
  Serial.println(F("Unknown command: type h"));
}

void setup() {
  Serial.begin(115200);
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) { airRaw[c] = -1; waterRaw[c] = -1; }
  Serial.println();
  Serial.println(F("=== SOIL MOISTURE CALIBRATION (Nano) ==="));
  printHelp();
  Serial.println(F("START: column A's probes dry, in air. Type: air a"));
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
