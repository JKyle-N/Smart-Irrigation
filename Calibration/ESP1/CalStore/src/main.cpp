/* =============================================================================
 *  SMART IRRIGATION -- CALIBRATION TOOL -- CALSTORE (ESP32 #1)
 * -----------------------------------------------------------------------------
 *  Every calibration tool ends by printing `set` lines. Flash this to ESP1, paste them in, flash the main
 *  firmware back. No source to edit, and it always takes effect: ESP1 loads the NVS namespace "calib" at
 *  boot OVER the constants in its source, so a value pasted into the source loses to any value saved here
 *  or by the LCD's Calibration menu.
 *
 *  Flashing the main firmware does not erase NVS. Only `pio run -t erase` does.
 *
 *    show               everything stored, next to the firmware's compiled default
 *    set <key> <value>  store a value. Refused if the key is unknown or the value is out of range
 *    clear <key>        forget one value: ESP1 goes back to its compiled default
 *    keys               list every key and what it is
 *    export             print everything stored as `set` lines, to keep with your calibration records
 *
 *  The limits below match what the firmware accepts when it loads a value. Keep this table in step with
 *  loadCal() and loadPowerCal() in ESP1/src/main.cpp.
 * ========================================================================== */
#include <Arduino.h>
#include <Preferences.h>

enum KeyType { K_FLOAT, K_INT, K_BOOL };
struct Key {
  const char *key;
  KeyType type;
  float lo, hi, def;      // def = the firmware's compiled default
  const char *what;
};

const Key KEYS[] = {
  // Soil: dry must stay above wet by 20 counts. `show` checks the pairs. Nano's 10-bit ADC.
  { "s0a2", K_INT,   0, 1023, 656, "soil column A, dry raw ADC" },
  { "s0w2", K_INT,   0, 1023, 524, "soil column A, wet raw ADC" },
  { "s1a2", K_INT,   0, 1023, 707, "soil column B, dry raw ADC" },
  { "s1w2", K_INT,   0, 1023, 391, "soil column B, wet raw ADC" },
  { "s2a2", K_INT,   0, 1023, 800, "soil column C, dry raw ADC" },
  { "s2w2", K_INT,   0, 1023, 300, "soil column C, wet raw ADC" },
  // Ultrasonic geometry
  { "reCm", K_FLOAT, 2, 400, 38, "reservoir: sensor to water when EMPTY, cm" },
  { "rfCm", K_FLOAT, 2, 400, 11, "reservoir: sensor to water when FULL, cm" },
  { "meCm", K_FLOAT, 2, 400, 50, "mixing tank: sensor to water when EMPTY, cm" },
  { "mfCm", K_FLOAT, 2, 400,  4, "mixing tank: sensor to water when FULL, cm" },
  // Environment offsets
  { "tOff", K_FLOAT, -20, 20, 0, "temperature offset, C" },
  { "hOff", K_FLOAT, -50, 50, 0, "humidity offset, %" },
  { "lOff", K_FLOAT, -100000, 100000, 0, "light offset, lux" },
  // Guarded NPK offsets (default 0: trimming toward the lab baseline biases the thesis data)
  { "s0N", K_FLOAT, -5000, 5000, 0, "NPK offset, column A, N" },
  { "s0P", K_FLOAT, -5000, 5000, 0, "NPK offset, column A, P" },
  { "s0K", K_FLOAT, -5000, 5000, 0, "NPK offset, column A, K" },
  { "s1N", K_FLOAT, -5000, 5000, 0, "NPK offset, column B, N" },
  { "s1P", K_FLOAT, -5000, 5000, 0, "NPK offset, column B, P" },
  { "s1K", K_FLOAT, -5000, 5000, 0, "NPK offset, column B, K" },
  { "s2N", K_FLOAT, -5000, 5000, 0, "NPK offset, column C, N" },
  { "s2P", K_FLOAT, -5000, 5000, 0, "NPK offset, column C, P" },
  { "s2K", K_FLOAT, -5000, 5000, 0, "NPK offset, column C, K" },
  // Flow, pushed to ESP2 at startup and in every work order
  { "frS", K_FLOAT, 0.05, 20, 1, "reservoir flow (Nano) correction scale" },
  { "kRM", K_FLOAT, 1, 100000, 450, "flow K: reservoir -> mixing, pulses/L" },
  { "kMI", K_FLOAT, 1, 100000, 450, "flow K: mixing -> irrigation, pulses/L" },
  { "kNA", K_FLOAT, 1, 100000, 450, "flow K: nutrient A, pulses/L" },
  { "kNB", K_FLOAT, 1, 100000, 450, "flow K: nutrient B, pulses/L" },
  { "kNC", K_FLOAT, 1, 100000, 450, "flow K: nutrient C, pulses/L" },
  { "kND", K_FLOAT, 1, 100000, 450, "flow K: nutrient D, pulses/L" },
  { "kPU", K_FLOAT, 1, 100000, 450, "flow K: pH up, pulses/L" },
  { "kPD", K_FLOAT, 1, 100000, 450, "flow K: pH down, pulses/L" },
  // Dosing pump rate: sets every dose's timed ceiling
  { "pumpA", K_FLOAT, 1, 5000, 50, "nutrient pump A, mL/min" },
  { "pumpB", K_FLOAT, 1, 5000, 50, "nutrient pump B, mL/min" },
  { "pumpC", K_FLOAT, 1, 5000, 50, "nutrient pump C, mL/min" },
  // pH, EC, mixer current zero, pushed to ESP2
  { "phM", K_FLOAT, -1, 1, 0.0036621, "pH slope (pH per ADC count)" },
  { "phB", K_FLOAT, -100, 100, 0, "pH offset" },
  { "ecM", K_FLOAT, -1, 1, 0.0009766, "EC slope (mS/cm per ADC count)" },
  { "ecB", K_FLOAT, -100, 100, 0, "EC offset" },
  { "acs", K_FLOAT, 0.1, 3.3, 1.65, "ACS712 mixer current zero, V at the ADC" },
  // ACS758 (the main battery's fallback current sensor)
  { "acsDiv",  K_FLOAT, 0.999, 20, 2, "ACS758 divider ratio" },
  { "acsZero", K_FLOAT, 0.1, 5.5, 2.5, "ACS758 output at 0 A, V" },
  { "acsSens", K_FLOAT, 0.002, 0.2, 0.04, "ACS758 sensitivity, V/A" },
  // INA226 trims: Calibration/ESP1/INA226 writes these. Changing one by hand is rarely right.
  { "iSolO",  K_FLOAT, 0.005, 0.02, 0.01, "solar INA226 shunt, ohm" },
  { "iSolA",  K_FLOAT, -0.17, 0.17, 0, "solar INA226 offset, A" },
  { "iSolB",  K_FLOAT, 0.909, 1.1, 1, "solar INA226 bus factor" },
  { "iSolI",  K_BOOL,  0, 1, 0, "solar INA226 current inverted, 0 or 1" },
  { "iMainO", K_FLOAT, 0.00075, 0.003, 0.0015, "main INA226 shunt, ohm" },
  { "iMainA", K_FLOAT, -1.1, 1.1, 0, "main INA226 offset, A" },
  { "iMainB", K_FLOAT, 0.909, 1.1, 1, "main INA226 bus factor" },
  { "iMainI", K_BOOL,  0, 1, 0, "main INA226 current inverted, 0 or 1" },
  { "iSecO",  K_FLOAT, 0.005, 0.02, 0.01, "secondary INA226 shunt, ohm" },
  { "iSecA",  K_FLOAT, -0.17, 0.17, 0.014, "secondary INA226 offset, A" },
  { "iSecB",  K_FLOAT, 0.909, 1.1, 1, "secondary INA226 bus factor" },
  { "iSecI",  K_BOOL,  0, 1, 0, "secondary INA226 current inverted, 0 or 1" },
};
const int KEY_COUNT = sizeof(KEYS) / sizeof(KEYS[0]);

Preferences cal;
char lineBuf[64];
uint8_t lineLen = 0;

const Key *findKey(const char *name) {
  for (int i = 0; i < KEY_COUNT; i++) if (!strcmp(KEYS[i].key, name)) return &KEYS[i];   // NVS keys are case-sensitive
  for (int i = 0; i < KEY_COUNT; i++) if (!strcasecmp(KEYS[i].key, name)) return &KEYS[i];
  return NULL;
}

float readValue(const Key &k) {
  switch (k.type) {
    case K_INT:  return (float)cal.getInt(k.key, (int)k.def);
    case K_BOOL: return cal.getBool(k.key, k.def != 0) ? 1.0f : 0.0f;
    default:     return cal.getFloat(k.key, k.def);
  }
}

void printValue(const Key &k, float v) {
  if (k.type == K_FLOAT) Serial.print(v, fabs(v) < 0.1f ? 7 : 4);
  else Serial.print((int)v);
}

void cmdShow() {
  int stored = 0;
  Serial.println(F("Stored in NVS (everything else is at the firmware's compiled default):"));
  for (int i = 0; i < KEY_COUNT; i++) {
    const Key &k = KEYS[i];
    if (!cal.isKey(k.key)) continue;
    stored++;
    Serial.printf("  %-8s ", k.key); printValue(k, readValue(k));
    Serial.print(F("   (default ")); printValue(k, k.def); Serial.print(F(")  ")); Serial.println(k.what);
  }
  if (!stored) Serial.println(F("  nothing yet"));
  // ESP1 ignores a soil pair that is too close or reversed, and that should not be a surprise later.
  for (int c = 0; c < 3; c++) {
    char a[6], w[6];
    snprintf(a, sizeof(a), "s%da2", c); snprintf(w, sizeof(w), "s%dw2", c);
    const Key *ka = findKey(a), *kw = findKey(w);
    if ((cal.isKey(a) || cal.isKey(w)) && readValue(*ka) - readValue(*kw) < 20)
      Serial.printf("  WARNING: column %c dry must be at least 20 above wet, or ESP1 ignores the pair.\n", 'A' + c);
  }
}

void cmdKeys() {
  for (int i = 0; i < KEY_COUNT; i++) {
    const Key &k = KEYS[i];
    Serial.printf("  %-8s %s  (", k.key, k.what); printValue(k, k.lo); Serial.print(F(" to ")); printValue(k, k.hi); Serial.println(')');
  }
}

void cmdExport() {
  Serial.println(F("# Paste these into CalStore to restore this calibration."));
  for (int i = 0; i < KEY_COUNT; i++) {
    const Key &k = KEYS[i];
    if (!cal.isKey(k.key)) continue;
    Serial.print(F("set ")); Serial.print(k.key); Serial.print(' '); printValue(k, readValue(k)); Serial.println();
  }
}

void cmdSet(const char *name, const char *val) {
  const Key *k = name ? findKey(name) : NULL;
  if (!k) { Serial.println(F("Unknown key: type keys. usage: set <key> <value>")); return; }
  char *end = NULL;
  float v = val ? strtof(val, &end) : NAN;
  if (!val || end == val || *end != '\0' || !(v >= k->lo && v <= k->hi)) {
    Serial.printf("REFUSED: %s needs a number from ", k->key); printValue(*k, k->lo);
    Serial.print(F(" to ")); printValue(*k, k->hi); Serial.println();
    return;
  }
  size_t wrote = 0;
  switch (k->type) {
    case K_INT:  wrote = cal.putInt(k->key, (int)v); break;
    case K_BOOL: wrote = cal.putBool(k->key, v != 0.0f); break;
    default:     wrote = cal.putFloat(k->key, v); break;
  }
  if (!wrote) { Serial.println(F("FAILED: NVS write error.")); return; }
  Serial.printf("%s = ", k->key); printValue(*k, readValue(*k)); Serial.print(F("  ")); Serial.println(k->what);
}

void cmdClear(const char *name) {
  const Key *k = name ? findKey(name) : NULL;
  if (!k) { Serial.println(F("Unknown key: type keys. usage: clear <key>")); return; }
  if (!cal.isKey(k->key)) { Serial.printf("%s is not stored.\n", k->key); return; }
  cal.remove(k->key);
  Serial.printf("%s cleared: ESP1 goes back to ", k->key); printValue(*k, k->def); Serial.println();
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  set <key> <value>   store a value (paste the lines the other tools print)"));
  Serial.println(F("  show                what is stored"));
  Serial.println(F("  clear <key>         forget one value"));
  Serial.println(F("  keys                every key and its range"));
  Serial.println(F("  export              print the stored values as set lines, for your records"));
  Serial.println(F("When done, flash ESP1's main firmware back. It loads these at boot."));
}

void handleLine(char *line) {
  char *cmd = strtok(line, " \t");
  if (!cmd) return;
  if (!strcasecmp(cmd, "h") || !strcasecmp(cmd, "help")) { printHelp(); return; }
  if (!strcasecmp(cmd, "show")) { cmdShow(); return; }
  if (!strcasecmp(cmd, "keys")) { cmdKeys(); return; }
  if (!strcasecmp(cmd, "export")) { cmdExport(); return; }
  if (!strcasecmp(cmd, "set")) { char *n = strtok(NULL, " \t"); char *v = strtok(NULL, " \t"); cmdSet(n, v); return; }
  if (!strcasecmp(cmd, "clear")) { cmdClear(strtok(NULL, " \t")); return; }
  Serial.println(F("Unknown command: type h"));
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("\n=== CALSTORE: ESP1's stored calibration ==="));
  if (!cal.begin("calib", false)) { Serial.println(F("FAILED: could not open NVS.")); }
  printHelp();
  cmdShow();
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (!lineLen) continue;
      lineBuf[lineLen] = '\0'; lineLen = 0;
      handleLine(lineBuf);
    } else if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
  }
}
