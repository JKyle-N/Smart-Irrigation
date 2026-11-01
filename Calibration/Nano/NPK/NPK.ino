/* =============================================================================
 *  SMART IRRIGATION  --  CALIBRATION TOOL  --  NPK 7-in-1 RS485 (Arduino Nano)
 * -----------------------------------------------------------------------------
 *  Reads each column's 7-in-1 sensor (moist,temp,EC,pH,N,P,K) so you can (a) check the
 *  bus config (address/baud/function/registers) and (b) set the GUARDED per-column N/P/K
 *  offset (spec sec.A.4.3 -- default 0; trimming toward the lab baseline BIASES the thesis
 *  data, so it ships at 0 and is warning-gated here too).
 *
 *  HOW TO USE: r a  shows column A. find  lists every sensor on the bus. Most columns need
 *  nothing more. Only if you mean to trim: unlock, then trim a n <ref>, then result.
 *  Disconnect D0/D1 (the link to ESP1) while flashing and while using this tool.
 *
 *  BUS (match Nano/Nano.ino:84-86,152-159): RS485 DE D8, RX D9, TX D10 (SoftwareSerial),
 *  per-column addr {0x01,0x02,0x03}, baud 4800, func 0x03, reg 0x0000 x 7.
 *  The Modbus query + CRC are copied VERBATIM from the firmware so the raw registers
 *  match 1:1 -- change the CONFIG block below to match your sensor, then paste the
 *  proven values back into Nano/Nano.ino.
 *
 *  SERIAL 115200 -- commands (type + Enter):
 *    h                 help
 *    r <A|B|C>         read a column once: registers, and values as ESP1 scales them
 *    live <A|B|C>      toggle ~1 Hz live read of one column
 *    find              list every sensor answering at addresses 1-16
 *    unlock            acknowledge the bias warning, allow one trim
 *    trim <A|B|C> <N|P|K> <ref>  GUARDED: that column's offset = ref - reading
 *    reset             set all N/P/K offsets back to 0
 *    result            lines to paste into CalStore
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
const char *const FIELD[7] = { "moist", "temp", "EC", "pH", "N", "P", "K" };
const float SCALE[7] = { 10, 10, 1, 10, 1, 1, 1 };      // ESP1's calNpkScale: register / this = the value

// Per-column N/P/K offsets, as in ESP1's calNpkOff, added after the scale. Default 0 (spec sec.A.4.3).
float npkOff[3][3] = { {0,0,0}, {0,0,0}, {0,0,0} };
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
  Serial.println(F("  r <A|B|C>            read a column once"));
  Serial.println(F("  live <A|B|C>         live read of a column, on/off"));
  Serial.println(F("  find                 list sensors at addresses 1-16"));
  Serial.println(F("  unlock               ack the bias warning, allow ONE trim"));
  Serial.println(F("  trim <col> <N|P|K> <ref>   offset = ref - reading (needs unlock)"));
  Serial.println(F("  reset                all offsets -> 0"));
  Serial.println(F("  result               lines to paste into CalStore"));
}

// Which addresses answer, so a wrong address or a second sensor at the same one is obvious.
void findSensors() {
  float v[7];
  uint8_t found = 0;
  Serial.println(F("Asking addresses 1-16, about 16 s if few answer..."));
  for (uint8_t a = 1; a <= 16; a++) {
    if (!readNpkColumn(a, v)) continue;
    found++;
    Serial.print(F("  0x")); Serial.print(a, HEX); Serial.print(F(" answers"));
    for (uint8_t c = 0; c < 3; c++) if (NPK_ADDR[c] == a) { Serial.print(F(" <- column ")); Serial.print((char)('A' + c)); }
    Serial.println();
  }
  if (!found) Serial.println(F("  Nothing answered: check A/B wiring, the sensor's supply, and the baud in this sketch."));
}

void printResult() {
  bool any = false;
  for (uint8_t c = 0; c < 3; c++) for (uint8_t e = 0; e < 3; e++) if (npkOff[c][e] != 0) any = true;
  if (!any) { Serial.println(F("All offsets are 0, which is the default. Nothing to store.")); return; }
  const char EL[3] = { 'N', 'P', 'K' };
  Serial.println(F("======== RESULT ========"));
  Serial.println(F("CalStore on ESP1 (flash Calibration/ESP1/CalStore, paste, flash the firmware back):"));
  for (uint8_t c = 0; c < 3; c++) for (uint8_t e = 0; e < 3; e++) {
    if (npkOff[c][e] == 0) continue;
    Serial.print(F("  set s")); Serial.print(c); Serial.print(EL[e]); Serial.print(' '); Serial.println(npkOff[c][e], 1);
  }
  Serial.println(F("========================"));
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
  for (uint8_t i = 0; i < 7; i++) {             // the value ESP1 will see: scaled, plus the offset
    float val = v[i] / SCALE[i];
    if (i >= 4) val += npkOff[col][i - 4];
    Serial.print(FIELD[i]); Serial.print('='); Serial.print(val, SCALE[i] > 1 ? 1 : 0);
    Serial.print(F(" (reg ")); Serial.print(v[i], 0); Serial.print(F(")  "));
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
  if (!strcasecmp(t1, "find")) { findSensors(); return; }
  if (!strcasecmp(t1, "show") || !strcasecmp(t1, "result")) { printResult(); return; }
  if (!strcasecmp(t1, "reset")) {
    for (uint8_t c = 0; c < 3; c++) for (uint8_t e = 0; e < 3; e++) npkOff[c][e] = 0;
    Serial.println(F("All offsets 0. If any were stored, `clear` them in CalStore (s0N and so on)."));
    return;
  }
  if (!strcasecmp(t1, "unlock")) {
    trimUnlocked = true;
    Serial.println(F("WARNING: trimming NPK toward a lab baseline BIASES the thesis data."));
    Serial.println(F("It is NOT needed for normal operation (default 0). One `trim` now allowed."));
    return;
  }
  if (!strcasecmp(t1, "trim")) {
    if (!trimUnlocked) { Serial.println(F("locked -- type `unlock` first (bias warning).")); return; }
    char *tc = strtok(NULL, " ");
    char *t2 = strtok(NULL, " ");
    char *t3 = strtok(NULL, " ");
    int col = tc ? colFromChar(tc[0]) : -1;
    int el = !t2 ? -1 : !strcasecmp(t2, "N") ? 0 : !strcasecmp(t2, "P") ? 1 : !strcasecmp(t2, "K") ? 2 : -1;
    char *end = NULL;
    float ref = t3 ? (float)strtod(t3, &end) : 0.0f;
    if (col < 0 || el < 0 || !t3 || *end != '\0') { Serial.println(F("usage: trim <A|B|C> <N|P|K> <ref>")); return; }
    float v[7];
    if (!readNpkColumn(NPK_ADDR[col], v)) { Serial.println(F("Read FAILED: nothing changed.")); return; }
    npkOff[col][el] = ref - v[4 + el] / SCALE[4 + el];
    Serial.print(F("Offset for column ")); Serial.print((char)('A' + col)); Serial.print(' '); Serial.print(t2);
    Serial.print(F(" = ")); Serial.println(npkOff[col][el], 1);
    Serial.println(F("NEXT: type result"));
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
