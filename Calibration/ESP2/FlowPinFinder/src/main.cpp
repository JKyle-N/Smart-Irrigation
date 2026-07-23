/* =============================================================================
 *  SMART IRRIGATION  --  BENCH TOOL  --  FLOW SENSOR PIN FINDER (ESP32 #2)
 * -----------------------------------------------------------------------------
 *  Identify which flow sensor is on which GPIO. It watches ALL flow-sensor pins at
 *  once and counts pulses on each. Make ONE sensor pulse -- the GPIO whose counter
 *  climbs is that sensor's pin. Two ways to make a sensor pulse:
 *    (1) spin / blow / pour through it by hand, OR
 *    (2) DRIVE the paired pump/valve so real water flows through it (see below).
 *
 *  RELAY / COMBO DRIVE (dead-man): arm a single relay (`sel <idx|name>`) or a valve+
 *  pump COMBO (`combo fill|pusha|pushb|pushc`, same combos as the ESP1 Testing menu),
 *  then HOLD the on-board BOOT button (GPIO0) to energize it, release to stop. A hard
 *  30 s cap applies (release + re-press to continue). `off` = panic-to-SAFE. Nothing
 *  runs until something is armed AND BOOT is held.
 *
 *  !!! SAFETY: once a relay/combo is armed this drives REAL pumps/valves. Only run
 *      with the rig plumbed. Releasing BOOT, the 30 s cap, or `off` returns all
 *      outputs to SAFE. Without an armed selection it is read-only (pins only). !!!
 *
 *  PINS (ESP2/src/main.cpp:120-126, CORRECTED wiring):
 *      RES_MIX 26   MIX_IRR 5    NUT_A 23   NUT_B 19
 *      NUT_C  18    NUT_D  27    pH_UP 4    pH_DN 25
 *  Each flow pin is INPUT_PULLUP + FALLING-edge counted (matches the firmware ISR).
 *  PCF8575 relays: I2C SDA21/SCL22 addr 0x20, ACTIVE-LOW (0xFFFF = all OFF); bits +
 *  combos from ESP2/src/main.cpp:99-115,240-243. Master cutoff bit 15 (P17) powers
 *  the bank and is energized whenever a relay/combo runs. BOOT button = GPIO0.
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                  help / re-print the pin + relay list
 *    z                  zero all flow counters (start a fresh identification)
 *    list               list single relays (0-15) + combos
 *    sel <idx|name>     arm a single relay (e.g. `sel 6`, `sel transfer`)
 *    combo <name>       arm a combo: fill | pusha | pushb | pushc
 *    off                force ALL outputs SAFE (panic)
 *    <HOLD BOOT btn>    energize the armed relay/combo while held; release = stop
 *
 *  Build: PlatformIO `pio run -e esp32dev` in this folder.
 * ========================================================================== */
#include <Arduino.h>
#include <Wire.h>

/* ---- Flow pins to watch (EDIT ME to add/rename; labels = firmware channel) - */
struct Fp { uint8_t gpio; const char *label; };
Fp PINS[] = {
  { 26, "RES_MIX" },
  {  5, "MIX_IRR" },
  { 23, "NUT_A"   },
  { 19, "NUT_B"   },
  { 18, "NUT_C"   },
  { 27, "NUT_D"   },
  {  4, "PH_UP"   },
  { 25, "PH_DN"   },
};
const int NPINS = sizeof(PINS) / sizeof(PINS[0]);

/* ---- PCF8575 relays (match ESP2/src/main.cpp:94-115) --------------------- */
const int      I2C_SDA = 21, I2C_SCL = 22;
const uint8_t  PCF_ADDR = 0x20;
#define B_RES_VALVE (1u<<0)
#define B_COL_A     (1u<<1)
#define B_COL_B     (1u<<2)
#define B_COL_C     (1u<<3)
#define B_MIX_VALVE (1u<<4)
#define B_INVERTER  (1u<<5)
#define B_TRANSFER  (1u<<6)
#define B_BOOSTER   (1u<<7)
#define B_MIXER     (1u<<8)
#define B_NUT_A     (1u<<9)
#define B_NUT_B     (1u<<10)
#define B_NUT_C     (1u<<11)
#define B_NUT_D     (1u<<12)
#define B_PH_UP     (1u<<13)
#define B_PH_DN     (1u<<14)
#define B_MASTER    (1u<<15)          // P17 master actuator-power cutoff

// Single-relay names by bit (match ESP1 TEST_NAMES; spaces stripped for `sel <name>`).
const char *RELAY_NAMES[16] = {
  "ResValve", "ColAVlv", "ColBVlv", "ColCVlv", "MixValve", "Inverter", "Transfer", "Booster",
  "Mixer", "NutA", "NutB", "NutC", "NutD", "pHUp", "pHDown", "MastCutoff"
};
// Valve+pump combos (identical to the ESP1 Testing menu, ESP2/src/main.cpp:240-243).
struct Combo { const char *name; uint16_t mask; };
Combo COMBOS[] = {
  { "FILL",  B_INVERTER | B_RES_VALVE | B_TRANSFER },              // Reservoir -> Mixing
  { "PUSHA", B_INVERTER | B_MIX_VALVE | B_COL_A | B_BOOSTER },     // Mixing -> Column A
  { "PUSHB", B_INVERTER | B_MIX_VALVE | B_COL_B | B_BOOSTER },     // Mixing -> Column B
  { "PUSHC", B_INVERTER | B_MIX_VALVE | B_COL_C | B_BOOSTER },     // Mixing -> Column C
};
const int NCOMBO = sizeof(COMBOS) / sizeof(COMBOS[0]);

/* ---- dead-man = on-board BOOT button (GPIO0, active-LOW) ------------------ */
#define PIN_BOOT 0
const unsigned long BTN_DEBOUNCE_MS = 25;
const unsigned long CAP_MS          = 30000;   // hard cap (matches Testing TEST_*_CAP_MS)

/* ---- state --------------------------------------------------------------- */
volatile uint32_t counts[16] = { 0 };          // per-pin pulse counters (index into PINS[])
uint32_t lastShown[16] = { 0 };

bool      armed = false;
uint16_t  armMask = 0;                          // ON bits for the armed relay/combo (excl. master)
String    armName = "";
bool      running = false;                      // relay/combo currently energized (BOOT held)
unsigned long runOnMs = 0;
bool      btnRaw = false, btnStable = false;
unsigned long btnChangeMs = 0;
bool      capLatched = false;                   // hit cap -> require a release before re-run

const unsigned long REFRESH_MS   = 400;
const unsigned long IDLE_HINT_MS = 5000;
unsigned long lastRefreshMs = 0, lastIdleMs = 0;
String line;

void IRAM_ATTR flowIsr(void *arg) { counts[(uint32_t)(uintptr_t)arg]++; }

/* ---- PCF8575 raw driver (active-LOW; 0xFFFF = all OFF) -------------------- */
void pcfWrite(uint16_t bank) {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(bank & 0xFF);          // P0..P7
  Wire.write((bank >> 8) & 0xFF);   // P8..P15
  Wire.endTransmission();
}
void safeAll() { pcfWrite(0xFFFF); }
void driveOn() { pcfWrite((uint16_t)(0xFFFF & ~(armMask | B_MASTER))); }   // armed bits + master cutoff ON

void stopDrive(const char *why) {
  safeAll();
  running = false;
  Serial.print("STOP ("); Serial.print(why); Serial.println("). all outputs SAFE.");
}

/* ---- help / listing ------------------------------------------------------ */
void printPins() {
  Serial.println("Flow pins watched (edit PINS[] to add/rename):");
  for (int i = 0; i < NPINS; i++) {
    Serial.print("  GPIO"); Serial.print(PINS[i].gpio);
    Serial.print("\t"); Serial.println(PINS[i].label);
  }
  Serial.println("Make ONE sensor pulse (spin it, or arm+run its pump) -> the GPIO that counts is its pin.");
  Serial.println("Cmds: z=zero  list=relays  sel <idx|name>  combo <fill|pusha|pushb|pushc>  off  h=help");
}

void listRelays() {
  Serial.println("Single relays (sel <idx> or sel <name>):");
  for (int i = 0; i < 16; i++) { Serial.print("  "); Serial.print(i); Serial.print("\t"); Serial.println(RELAY_NAMES[i]); }
  Serial.println("Combos (combo <name>):  FILL  PUSHA  PUSHB  PUSHC");
  Serial.println("Then HOLD the BOOT button to run, release to stop (30 s cap).");
}

void zeroAll() {
  for (int i = 0; i < NPINS; i++) { noInterrupts(); counts[i] = 0; interrupts(); lastShown[i] = 0; }
  Serial.println("-- all flow counters zeroed --");
}

// Normalize a token for name matching: lowercase, strip spaces.
String norm(const String &s) {
  String o; for (unsigned i = 0; i < s.length(); i++) { char c = s[i]; if (c != ' ') o += (char)tolower(c); }
  return o;
}

void armSingle(int bit) {
  if (running) stopDrive("re-arm");
  armed = true; armMask = (uint16_t)(1u << bit); armName = RELAY_NAMES[bit]; capLatched = false;
  Serial.print("armed relay "); Serial.print(bit); Serial.print(" ("); Serial.print(armName);
  Serial.println(") -- HOLD BOOT to run. NB: an AC pump also needs Inverter (use a combo).");
}
void armCombo(int idx) {
  if (running) stopDrive("re-arm");
  armed = true; armMask = COMBOS[idx].mask; armName = String("combo ") + COMBOS[idx].name; capLatched = false;
  Serial.print("armed "); Serial.print(armName); Serial.println(" -- HOLD BOOT to run water, release to stop.");
}

void handleLine(const String &in) {
  String s = in; s.trim();
  int sp = s.indexOf(' ');
  String cmd = (sp < 0) ? s : s.substring(0, sp);
  String arg = (sp < 0) ? "" : s.substring(sp + 1);
  arg.trim();
  String c = norm(cmd);

  if (c == "h")    { printPins(); return; }
  if (c == "z")    { zeroAll(); return; }
  if (c == "list") { listRelays(); return; }
  if (c == "off")  { safeAll(); running = false; capLatched = false; Serial.println("ALL OUTPUTS SAFE"); return; }

  if (c == "sel") {
    if (!arg.length()) { Serial.println("usage: sel <idx 0-15 | name>"); return; }
    // numeric index?
    bool numeric = true; for (unsigned i = 0; i < arg.length(); i++) if (!isdigit(arg[i])) numeric = false;
    if (numeric) { int b = arg.toInt(); if (b < 0 || b > 15) { Serial.println("idx must be 0-15"); return; } armSingle(b); return; }
    String na = norm(arg);
    for (int i = 0; i < 16; i++) if (norm(RELAY_NAMES[i]) == na) { armSingle(i); return; }
    Serial.println("unknown relay name -- type `list`");
    return;
  }
  if (c == "combo") {
    String na = norm(arg);
    for (int i = 0; i < NCOMBO; i++) if (norm(COMBOS[i].name) == na) { armCombo(i); return; }
    Serial.println("unknown combo -- use: fill | pusha | pushb | pushc");
    return;
  }
  if (s.length()) Serial.println("? h=help  list=relays  sel/combo to arm  off=SAFE");
}

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);
  pinMode(PIN_BOOT, INPUT_PULLUP);             // BOOT button = hardware dead-man
  safeAll();                                   // relays SAFE at boot
  for (int i = 0; i < NPINS; i++) {
    pinMode(PINS[i].gpio, INPUT_PULLUP);
    attachInterruptArg(digitalPinToInterrupt(PINS[i].gpio), flowIsr, (void *)(uintptr_t)i, FALLING);
  }
  delay(200);
  Serial.println();
  Serial.println("=== FLOW SENSOR PIN FINDER + RELAY DRIVE (ESP32 #2) ===");
  printPins();
}

void loop() {
  // serial commands (plain line parsing; BOOT is the dead-man, not keystrokes)
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') { if (line.length()) { handleLine(line); line = ""; } }
    else if (line.length() < 32) line += ch;
  }

  // BOOT-button dead-man for the armed relay/combo
  bool pressed = (digitalRead(PIN_BOOT) == LOW);
  if (pressed != btnRaw) { btnRaw = pressed; btnChangeMs = millis(); }
  if (pressed != btnStable && millis() - btnChangeMs > BTN_DEBOUNCE_MS) {
    btnStable = pressed;
    if (btnStable) {                             // pressed
      if (!armed)          Serial.println("nothing armed -- `sel <idx|name>` or `combo <name>` first");
      else if (capLatched) Serial.println("cap latched -- release already done? re-press to run");
      else if (!running)   { running = true; runOnMs = millis(); driveOn();
                             Serial.print("RUN "); Serial.print(armName); Serial.println(" -- release BOOT to stop."); }
    } else {                                     // released
      if (running) stopDrive("BOOT released");
      capLatched = false;
    }
  }
  if (running && millis() - runOnMs > CAP_MS) { stopDrive("30s hard cap"); capLatched = true; }

  // flow-pin activity print
  if (millis() - lastRefreshMs < REFRESH_MS) return;
  lastRefreshMs = millis();

  uint32_t snap[16]; bool anyActive = false;
  for (int i = 0; i < NPINS; i++) {
    noInterrupts(); snap[i] = counts[i]; interrupts();
    if (snap[i] != lastShown[i]) anyActive = true;
  }

  if (anyActive) {
    Serial.println("---------------------------------------------");
    int topIdx = -1; uint32_t topDelta = 0;
    for (int i = 0; i < NPINS; i++) {
      uint32_t d = snap[i] - lastShown[i];
      Serial.print("  GPIO"); Serial.print(PINS[i].gpio);
      Serial.print("\t"); Serial.print(PINS[i].label);
      Serial.print("\ttotal="); Serial.print(snap[i]);
      if (d > 0) { Serial.print("\t(+"); Serial.print(d); Serial.print(")  <-- ACTIVE"); }
      Serial.println();
      if (d > topDelta) { topDelta = d; topIdx = i; }
      lastShown[i] = snap[i];
    }
    if (topIdx >= 0) {
      Serial.print(">>> THIS SENSOR IS ON  GPIO"); Serial.print(PINS[topIdx].gpio);
      Serial.print("  (labeled "); Serial.print(PINS[topIdx].label); Serial.println(")");
    }
    lastIdleMs = millis();
  } else if (millis() - lastIdleMs >= IDLE_HINT_MS) {
    lastIdleMs = millis();
    Serial.print("(idle -- spin a sensor, or arm a relay/combo and hold BOOT");
    Serial.println(armed ? ("; armed: " + armName + ")").c_str() : ")");
  }
}
