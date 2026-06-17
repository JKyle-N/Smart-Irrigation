/* =============================================================================
 *  SMART IRRIGATION & FERTIGATION SYSTEM  --  ARDUINO NANO SENSOR HUB (DEBUG)
 *  Controller 1 of 3   (Nano  ->  ESP32 #1  ->  ESP32 #2)
 * -----------------------------------------------------------------------------
 *  DEBUG / FAKE BUILD. Real sketch: Nano/Nano.ino.
 *
 *  Purpose: bench-test ESP32 #1 with NO sensors wired. This sketch speaks the
 *  EXACT same framed UART protocol (spec sec.9.9) and obeys the same inbound
 *  command set, but every sensor value is SYNTHESIZED in firmware (a slow random
 *  walk inside plausible ranges) instead of being read from hardware. To ESP32 #1
 *  this is indistinguishable from a healthy real Nano sending clean packets.
 *
 *  Outbound (Nano -> ESP32 #1), identical to production:
 *    <START>,ENV,<temp>,<hum>,<END>
 *    <START>,TANK,<res%>,<mix%>,<flowLpm>,<END>
 *    <START>,SOIL,<COL>,<val>,...,<END>     (tag,value per ENABLED column)
 *    <START>,LIGHT,<lux>,<END>
 *    <START>,NPK,<COL>,<m>,<t>,<EC>,<pH>,<N>,<P>,<K>,<END>  (one per enabled col)
 *    <START>,STATUS,NANO,OK,<END>            (heartbeat)
 *  Inbound (ESP32 #1 -> Nano), identical to production:
 *    <START>,ACTIVE,<END> / DAY / NIGHT / RESET_REQ
 *
 *  All values stay inside the ESP32 #1 plausibility limits (Tier-2 garbage check,
 *  spec sec.18.9.5.0) so every packet is accepted as clean. Flip COLUMN_ENABLED to
 *  test enabled/disabled-column handling; send RESET_REQ to test the self-reset.
 *
 *  No external libraries. Build: PlatformIO env nanoatmega328new (or Arduino IDE,
 *  board Arduino Nano). Disconnect D0/D1 before upload, reconnect after.
 * ========================================================================== */

#include <Arduino.h>
#include <avr/wdt.h>

/* =============================================================================
 *  CONFIG  (mirror the real Nano so ESP32 #1 sees the same packet shapes)
 * ========================================================================== */
const uint8_t NUM_COLUMNS = 3;
bool       COLUMN_ENABLED[NUM_COLUMNS] = { true, true, false };   // A, B, C
const char COLUMN_TAG[NUM_COLUMNS]     = { 'A', 'B', 'C' };

const uint8_t PIN_LED = 13;                 // D13 status LED (real hardware pin)
const unsigned long LINK_BAUD = 9600;       // ESP32 #1 link (CLAUDE.md locked)

/* ---- Transmission + heartbeat intervals (spec sec.17.4.4) ---------------- */
const unsigned long TX_INTERVAL_ACTIVE_MS = 10000UL;     // 10 s
const unsigned long TX_INTERVAL_DAY_MS    = 1200000UL;   // 20 min
const unsigned long TX_INTERVAL_NIGHT_MS  = 3600000UL;   // 1 hr
const unsigned long HB_INTERVAL_ACTIVE_MS = 10000UL;     // 10 s
const unsigned long HB_INTERVAL_DAY_MS    = 60000UL;     // 1 min
const unsigned long HB_INTERVAL_NIGHT_MS  = 120000UL;    // 2 min
#define BOOT_TX_STATE  TX_ACTIVE                          // fast data on boot

#define WDT_TIMEOUT  WDTO_4S

/* =============================================================================
 *  GLOBALS
 * ========================================================================== */
enum TxState { TX_ACTIVE, TX_DAY, TX_NIGHT };
TxState txState = BOOT_TX_STATE;

unsigned long lastTxMs = 0;
unsigned long lastHbMs = 0;
bool          forceTx  = false;

char pkt[140];                  // outbound packet builder (cap 128 enforced)

#define INBOUND_BUF 96
char    inBuf[INBOUND_BUF];
uint8_t inLen = 0;

bool ledState = false;

/* ---- Fake-sensor state (slow random walk inside plausible ranges) --------- */
float fTemp = 28.0f, fHum = 65.0f;
float fRes  = 80.0f, fMix = 45.0f;
float fSoil[NUM_COLUMNS] = { 42.0f, 38.0f, 50.0f };
float fLux  = 6000.0f;
// per column: moisture%, temp C, EC, pH, N, P, K
float fNpk[NUM_COLUMNS][7] = {
  { 30.0f, 26.0f, 200.0f, 6.5f, 120.0f, 35.0f, 180.0f },
  { 28.0f, 26.0f, 210.0f, 6.4f, 110.0f, 38.0f, 190.0f },
  { 32.0f, 26.0f, 205.0f, 6.6f, 130.0f, 40.0f, 175.0f },
};

/* ---- Forward declarations ------------------------------------------------ */
void pollUart();
void handleLine(char *line);
void setTxState(TxState s);
unsigned long getTxInterval();
unsigned long getHbInterval();
void sendEnv(); void sendTank(); void sendSoil(); void sendLight();
void sendNpk(); void sendStatus();
void startPkt(const char *cmd);
void pktAddRaw(const char *s);
void pktAddInt(long v);
void pktAddFloat(float v, uint8_t prec);
void endPkt();
void softwareReset();
float wander(float &s, float lo, float hi, float step);

/* ---- Early WDT disable (avoid reset boot-loop on older bootloaders) ------- */
void wdtEarlyDisable(void) __attribute__((naked, used, section(".init3")));
void wdtEarlyDisable(void) { MCUSR = 0; wdt_disable(); }

/* =============================================================================
 *  SETUP
 * ========================================================================== */
void setup() {
  MCUSR = 0;
  wdt_disable();

  Serial.begin(LINK_BAUD);          // hardware UART to ESP32 #1 (D0/D1)
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  randomSeed(analogRead(A0) ^ micros());   // seed the fake-value walk

  unsigned long now = millis();
  lastTxMs = now;
  lastHbMs = now;

  wdt_enable(WDT_TIMEOUT);          // self-recovery WDT (same as real Nano)
}

/* =============================================================================
 *  LOOP  (non-blocking)
 * ========================================================================== */
void loop() {
  wdt_reset();
  pollUart();                       // ACTIVE / DAY / NIGHT / RESET_REQ

  unsigned long now = millis();

  if (forceTx || (unsigned long)(now - lastTxMs) >= getTxInterval()) {
    lastTxMs = now;
    forceTx  = false;
    sendEnv();
    sendTank();
    sendSoil();
    sendLight();
    sendNpk();
  }

  if ((unsigned long)(now - lastHbMs) >= getHbInterval()) {
    lastHbMs = now;
    sendStatus();
    ledState = !ledState;
    digitalWrite(PIN_LED, ledState ? HIGH : LOW);
  }
}

/* =============================================================================
 *  INBOUND UART  (same framed command handling as the real Nano)
 * ========================================================================== */
void pollUart() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (inLen > 0) { inBuf[inLen] = '\0'; handleLine(inBuf); inLen = 0; }
    } else if (inLen < INBOUND_BUF - 1) {
      inBuf[inLen++] = c;
    } else {
      inLen = 0;                    // overflow: drop malformed line
    }
  }
}

void handleLine(char *line) {
  char *s = strstr(line, "<START>");
  char *e = strstr(line, "<END>");
  if (!s || !e || e < s) return;

  char *p = s + 7;
  if (*p == ',') p++;
  char cmd[16]; uint8_t n = 0;
  while (*p && *p != ',' && *p != '<' && n < (uint8_t)(sizeof(cmd) - 1)) cmd[n++] = *p++;
  cmd[n] = '\0';

  if      (strcmp(cmd, "ACTIVE")    == 0) setTxState(TX_ACTIVE);
  else if (strcmp(cmd, "DAY")       == 0) setTxState(TX_DAY);
  else if (strcmp(cmd, "NIGHT")     == 0) setTxState(TX_NIGHT);
  else if (strcmp(cmd, "RESET_REQ") == 0) softwareReset();
}

void setTxState(TxState s) { txState = s; forceTx = true; }

unsigned long getTxInterval() {
  switch (txState) {
    case TX_ACTIVE: return TX_INTERVAL_ACTIVE_MS;
    case TX_DAY:    return TX_INTERVAL_DAY_MS;
    default:        return TX_INTERVAL_NIGHT_MS;
  }
}
unsigned long getHbInterval() {
  switch (txState) {
    case TX_ACTIVE: return HB_INTERVAL_ACTIVE_MS;
    case TX_DAY:    return HB_INTERVAL_DAY_MS;
    default:        return HB_INTERVAL_NIGHT_MS;
  }
}

/* =============================================================================
 *  OUTBOUND PACKETS  --  all values FAKE but plausible (clean to ESP32 #1)
 * ========================================================================== */
void sendEnv() {
  startPkt("ENV");
  pktAddFloat(wander(fTemp, 18.0f, 38.0f, 0.3f), 1);    // temp C  (limit -10..70)
  pktAddFloat(wander(fHum,  40.0f, 90.0f, 0.5f), 1);    // hum %   (limit 0..100)
  endPkt();
}

void sendTank() {
  startPkt("TANK");
  pktAddFloat(wander(fRes, 20.0f, 100.0f, 0.4f), 1);    // reservoir %
  pktAddFloat(wander(fMix,  0.0f, 100.0f, 0.4f), 1);    // mixing %
  pktAddFloat(0.0f, 1);                                  // fill-flow L/min (idle)
  endPkt();
}

void sendSoil() {
  char tag[2] = { 0, 0 };
  startPkt("SOIL");
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c]) continue;                    // omit disabled column entirely
    tag[0] = COLUMN_TAG[c];
    pktAddRaw(tag);
    long v = (long)wander(fSoil[c], 10.0f, 90.0f, 0.6f); // moisture % (0..100)
    pktAddInt(v);
  }
  endPkt();
}

void sendLight() {
  startPkt("LIGHT");
  pktAddFloat(wander(fLux, 0.0f, 40000.0f, 200.0f), 0);  // lux (>=0)
  endPkt();
}

void sendNpk() {
  char tag[2] = { 0, 0 };
  const uint8_t prec[7] = { 1, 1, 0, 1, 0, 0, 0 };
  const float lo[7] = { 0,  0,   0,  3.0f,  0,  0,  0 };
  const float hi[7] = { 100, 50, 3000, 9.0f, 999, 999, 999 };
  const float st[7] = { 0.4f, 0.2f, 5.0f, 0.05f, 2.0f, 1.0f, 2.0f };
  for (uint8_t c = 0; c < NUM_COLUMNS; c++) {
    if (!COLUMN_ENABLED[c]) continue;                    // no packet for disabled column
    wdt_reset();
    startPkt("NPK");
    tag[0] = COLUMN_TAG[c];
    pktAddRaw(tag);
    for (uint8_t i = 0; i < 7; i++)
      pktAddFloat(wander(fNpk[c][i], lo[i], hi[i], st[i]), prec[i]);
    endPkt();
  }
}

void sendStatus() {
  startPkt("STATUS");
  pktAddRaw("NANO");
  pktAddRaw("OK");
  endPkt();
}

/* =============================================================================
 *  FAKE VALUE WALK  --  nudge state by +/- up to `step`, clamp to [lo,hi]
 * ========================================================================== */
float wander(float &s, float lo, float hi, float step) {
  s += ((float)random(-1000, 1001) / 1000.0f) * step;
  if (s < lo) s = lo;
  if (s > hi) s = hi;
  return s;
}

/* =============================================================================
 *  PACKET BUILDER  (frames <START>,...,<END>, enforces <=128 bytes)
 * ========================================================================== */
void startPkt(const char *cmd) { strcpy(pkt, "<START>,"); strcat(pkt, cmd); }
void pktAddRaw(const char *s)  { strcat(pkt, ","); strcat(pkt, s); }
void pktAddInt(long v)         { strcat(pkt, ","); char t[12]; ltoa(v, t, 10); strcat(pkt, t); }
void pktAddFloat(float v, uint8_t prec) {
  strcat(pkt, ",");
  char t[16]; dtostrf(v, 0, prec, t); strcat(pkt, t);
}
void endPkt() {
  strcat(pkt, ",<END>");
  if (strlen(pkt) <= 128) Serial.println(pkt);
}

/* =============================================================================
 *  RESET  --  RESET_REQ handler: clean self-reset via short-timeout WDT
 * ========================================================================== */
void softwareReset() {
  wdt_enable(WDTO_15MS);
  while (1) { }
}
