/* =============================================================================
 *  SMART IRRIGATION  --  CALIBRATION BENCH TOOL  --  NPK 7-in-1 RS485 (Arduino Nano)
 * -----------------------------------------------------------------------------
 *  Standalone. No ESP1, no UART link. Reads the RAW Modbus registers from a
 *  column's 7-in-1 sensor (moist,temp,EC,pH,N,P,K) so you can (a) verify the bus
 *  config (address/baud/function/registers) and (b) set the GUARDED per-element
 *  N/P/K offset trim (spec sec.A.4.3 -- default 0; trimming toward the lab baseline
 *  BIASES the thesis data, so it ships at 0 and is warning-gated here too).
 *
 *  BUS (match Nano/Nano.ino:84-86,152-159): RS485 DE D8, RX D9, TX D10 (SoftwareSerial),
 *  per-column addr {0x01,0x02,0x03}, baud 4800, func 0x03, reg 0x0000 x 7.
 *  The Modbus query + CRC are copied VERBATIM from the firmware so the raw registers
 *  match 1:1 -- change the CONFIG block below to match your sensor, then paste the
 *  proven values back into Nano/Nano.ino.
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    r <A|B|C>         read a column once, print raw registers
 *    live <A|B|C>      toggle ~1 Hz live read of one column
 *    trim <N|P|K> <ref>  GUARDED: set an element offset = ref - raw (needs `unlock` first)
 *    unlock            acknowledge the bias warning, allow one trim
 *    reset             set all N/P/K offsets back to 0
 *    show              print current offsets
 *
 *  Libraries: SoftwareSerial ships with the AVR core -- no external libs.
 *  Build: Arduino IDE (Arduino Nano, ATmega328P) or `pio run` in this folder.
 * ========================================================================== */
#include <Arduino.h>
#include <SoftwareSerial.h>

/* ---- CONFIG (mirror Nano/Nano.ino -- change to match YOUR sensor) --------- */
const uint8_t  PIN_RS485_DE = 8, PIN_RS485_RX = 9, PIN_RS485_TX = 10;
const uint8_t  NPK_ADDR[3]   = { 0x03, 0x01, 0x02 };   // A=NPK1(0x03) B=NPK2(0x01) C=NPK3(0x02) -- from FINALTESTCODEWITHNPK
const unsigned long NPK_BAUD = 4800;
const uint8_t  NPK_FUNCTION  = 0x03;                   // 0x03 read-holding (some use 0x04)
const uint16_t NPK_REG_START = 0x0000;
const uint16_t NPK_REG_COUNT = 7;                      // moist,temp,EC,pH,N,P,K
const unsigned long NPK_RESPONSE_TIMEOUT_MS = 1000;
/* --------------------------------------------------------------------------- */

SoftwareSerial npkSerial(PIN_RS485_RX, PIN_RS485_TX);   // RX=D9, TX=D10
const char *FIELD[7] = { "moist", "temp", "EC", "pH", "N", "P", "K" };

// N/P/K element offsets (raw-register units). Default 0 (spec sec.A.4.3).
float offN = 0, offP = 0, offK = 0;
bool  trimUnlocked = false;

bool          liveOn = false;
int8_t        liveCol = -1;
unsigned long lastLiveMs = 0;
const unsigned long LIVE_MS = 1000;

#define LINE_BUF 48
char    line[LINE_BUF];
uint8_t lineLen = 0;

void printHelp();
bool readNpkColumn(uint8_t addr, float out[7]);
uint16_t modbusCRC(const uint8_t *buf, uint8_t len);
void printColumn(int col);
int  colFromChar(char ch);
void handleLine(char *s);

void setup() {
  Serial.begin(115200);
  npkSerial.begin(NPK_BAUD);
  pinMode(PIN_RS485_DE, OUTPUT);
  digitalWrite(PIN_RS485_DE, LOW);   // default receive
  Serial.println();
  Serial.println(F("=== NPK 7-in-1 CALIBRATION (Nano) ==="));
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
  if (liveOn && liveCol >= 0 && (unsigned long)(millis() - lastLiveMs) >= LIVE_MS) {
    lastLiveMs = millis();
    printColumn(liveCol);
  }
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  h                    this help"));
  Serial.println(F("  r <A|B|C>            read a column once (raw registers)"));
  Serial.println(F("  live <A|B|C>         toggle ~1 Hz live read of a column"));
  Serial.println(F("  unlock               ack bias warning, allow ONE trim"));
  Serial.println(F("  trim <N|P|K> <ref>   set offset = ref - raw (needs unlock)"));
  Serial.println(F("  reset                all N/P/K offsets -> 0"));
  Serial.println(F("  show                 print current offsets"));
}

int colFromChar(char ch) {
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  if (ch == 'A') return 0;
  if (ch == 'B') return 1;
  if (ch == 'C') return 2;
  return -1;
}

void printColumn(int col) {
  float v[7];
  bool ok = readNpkColumn(NPK_ADDR[col], v);
  Serial.print(F("Col ")); Serial.print((char)('A' + col));
  Serial.print(F(" (addr 0x")); Serial.print(NPK_ADDR[col], HEX); Serial.print(F("): "));
  if (!ok) { Serial.println(F("READ FAILED (no reply / bad CRC -- check config/wiring)")); return; }
  for (uint8_t i = 0; i < 7; i++) {
    Serial.print(FIELD[i]); Serial.print('='); Serial.print(v[i], 0);
    if (i == 4) { Serial.print(F("(+off ")); Serial.print(v[4] + offN, 0); Serial.print(')'); }
    if (i == 5) { Serial.print(F("(+off ")); Serial.print(v[5] + offP, 0); Serial.print(')'); }
    if (i == 6) { Serial.print(F("(+off ")); Serial.print(v[6] + offK, 0); Serial.print(')'); }
    Serial.print(' ');
  }
  Serial.println();
}

void handleLine(char *s) {
  char *t1 = strtok(s, " ");
  if (!t1) return;

  if (!strcasecmp(t1, "h")) { printHelp(); return; }
  if (!strcasecmp(t1, "r")) {
    char *t2 = strtok(NULL, " ");
    int col = t2 ? colFromChar(t2[0]) : -1;
    if (col < 0) { Serial.println(F("usage: r <A|B|C>")); return; }
    printColumn(col);
    return;
  }
  if (!strcasecmp(t1, "live")) {
    char *t2 = strtok(NULL, " ");
    int col = t2 ? colFromChar(t2[0]) : -1;
    if (col < 0) { Serial.println(F("usage: live <A|B|C>")); return; }
    if (liveOn && liveCol == col) { liveOn = false; Serial.println(F("live=OFF")); }
    else { liveOn = true; liveCol = col; Serial.print(F("live=ON col ")); Serial.println((char)('A' + col)); }
    return;
  }
  if (!strcasecmp(t1, "show")) {
    Serial.print(F("offN=")); Serial.print(offN, 0);
    Serial.print(F(" offP=")); Serial.print(offP, 0);
    Serial.print(F(" offK=")); Serial.println(offK, 0);
    return;
  }
  if (!strcasecmp(t1, "reset")) { offN = offP = offK = 0; Serial.println(F("offsets reset to 0")); return; }
  if (!strcasecmp(t1, "unlock")) {
    trimUnlocked = true;
    Serial.println(F("WARNING: trimming NPK toward a lab baseline BIASES the thesis data."));
    Serial.println(F("It is NOT needed for normal operation (default 0). One `trim` now allowed."));
    return;
  }
  if (!strcasecmp(t1, "trim")) {
    if (!trimUnlocked) { Serial.println(F("locked -- type `unlock` first (bias warning).")); return; }
    char *t2 = strtok(NULL, " ");
    char *t3 = strtok(NULL, " ");
    if (!t2 || !t3) { Serial.println(F("usage: trim <N|P|K> <ref>")); return; }
    // read current column? trim needs a raw sample -- use column A by default for the element.
    float v[7];
    if (!readNpkColumn(NPK_ADDR[0], v)) { Serial.println(F("read FAILED -- trim aborted")); return; }
    float ref = atof(t3);
    if (!strcasecmp(t2, "N"))      { offN = ref - v[4]; Serial.print(F("offN=")); Serial.println(offN, 0); }
    else if (!strcasecmp(t2, "P")) { offP = ref - v[5]; Serial.print(F("offP=")); Serial.println(offP, 0); }
    else if (!strcasecmp(t2, "K")) { offK = ref - v[6]; Serial.print(F("offK=")); Serial.println(offK, 0); }
    else { Serial.println(F("usage: trim <N|P|K> <ref>")); return; }
    trimUnlocked = false;   // re-lock after one use
    return;
  }
  Serial.println(F("? unknown command -- type h"));
}

/* ---- Modbus read + CRC copied VERBATIM from Nano/Nano.ino:551-610 ---------- */
bool readNpkColumn(uint8_t addr, float out[7]) {
  for (uint8_t i = 0; i < 7; i++) out[i] = -1.0f;

  uint8_t q[8];
  q[0] = addr;
  q[1] = NPK_FUNCTION;
  q[2] = highByte(NPK_REG_START);
  q[3] = lowByte(NPK_REG_START);
  q[4] = highByte(NPK_REG_COUNT);
  q[5] = lowByte(NPK_REG_COUNT);
  uint16_t crc = modbusCRC(q, 6);
  q[6] = lowByte(crc);
  q[7] = highByte(crc);

  while (npkSerial.available()) npkSerial.read();   // drop stale bytes

  digitalWrite(PIN_RS485_DE, HIGH);                 // transmit
  delay(5);                                         // bounded RS485 DE settle (proven value)
  npkSerial.write(q, 8);
  npkSerial.flush();
  digitalWrite(PIN_RS485_DE, LOW);                  // back to receive

  const uint8_t expected = 3 + 2 * NPK_REG_COUNT + 2;
  uint8_t resp[40];
  uint8_t idx = 0;
  unsigned long start = millis();
  while (idx < expected &&
         (unsigned long)(millis() - start) < NPK_RESPONSE_TIMEOUT_MS) {
    if (npkSerial.available()) resp[idx++] = (uint8_t)npkSerial.read();
  }

  if (idx < expected)                            return false;
  if (resp[0] != addr || resp[1] != NPK_FUNCTION) return false;
  if (resp[2] != 2 * NPK_REG_COUNT)              return false;

  uint16_t rcrc = modbusCRC(resp, expected - 2);
  if (lowByte(rcrc) != resp[expected - 2] ||
      highByte(rcrc) != resp[expected - 1])      return false;

  for (uint8_t i = 0; i < 7; i++) {
    uint16_t raw = ((uint16_t)resp[3 + 2 * i] << 8) | resp[4 + 2 * i];
    out[i] = (float)raw;
  }
  return true;
}

uint16_t modbusCRC(const uint8_t *buf, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= (uint16_t)buf[i];
    for (uint8_t b = 0; b < 8; b++) {
      if (crc & 0x0001) { crc >>= 1; crc ^= 0xA001; }
      else              { crc >>= 1; }
    }
  }
  return crc;
}
