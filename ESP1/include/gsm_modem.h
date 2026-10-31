/* A7670C (SIMCom A76xx) SMS driver for ESP32 #1. No Arduino dependency, so the whole state machine
 * runs on the host against a simulated modem:  pio test -e native
 *
 * Non-blocking: main.cpp feeds UART bytes through feed() and calls tick() from loop(). Nothing here
 * waits, allocates or touches a global; every effect goes out through Callbacks. One AT command is in
 * flight at a time, and every result is checked -- the manual's limits below are what set the timeouts.
 *
 * AT behaviour follows the A76XX Series AT Command Manual V1.12. Anything the manual left open is
 * handled defensively and listed in Commissioning_Flash_Plan.md stage M for checking on the module.
 *
 * Inbound owner-spoof defence: AT+CSDH=1 puts the body length in the +CMT/+CMGR header and exactly that
 * many characters are consumed as body. Otherwise a multi-line SMS could carry a forged "+CMT:" line
 * with the owner's number followed by a command, and the parser would treat that text as the modem's.
 */
#ifndef GSM_MODEM_H
#define GSM_MODEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const uint32_t GSM_BAUDS[] = { 115200, 9600, 57600, 38400, 19200, 230400, 460800 };
static const uint8_t  GSM_BAUD_COUNT = sizeof(GSM_BAUDS) / sizeof(GSM_BAUDS[0]);

static const unsigned long GSM_PROBE_TIMEOUT_MS  = 700;
static const uint8_t       GSM_PROBE_TRIES       = 3;
static const unsigned long GSM_SWEEP_GAP_MS      = 5000;     // between full baud sweeps (modem absent/booting)
static const unsigned long GSM_CMD_TIMEOUT_MS    = 9000;     // manual: max response of the commands used here
static const unsigned long GSM_CMGF_TIMEOUT_MS   = 3000;     // per-send re-assert of text mode
static const unsigned long GSM_PROMPT_TIMEOUT_MS = 10000;    // wait for '>' after AT+CMGS
static const unsigned long GSM_RESULT_TIMEOUT_MS = 45000;    // manual: AT+CMGS max response is 40 s
static const unsigned long GSM_RESULT_OK_GRACE_MS = 1000;    // +CMGS seen; wait this long for its OK
static const unsigned long GSM_RETRY_BACKOFF_MS  = 30000;    // x attempt number, so 30 s then 60 s
static const uint8_t       GSM_SEND_RETRIES      = 2;        // re-sends after the first failure
static const unsigned long GSM_HEALTH_STEP_MS    = 3750;     // one query per step: a full round takes ~15 s
static const uint8_t       GSM_SILENT_LIMIT      = 3;        // consecutive query timeouts -> re-detect
static const unsigned long GSM_SETUP_RETRY_MS    = 1000;     // between tries of one setup step
static const uint8_t       GSM_SETUP_STEP_TRIES  = 3;
static const unsigned long GSM_SETUP_WAIT_MS     = 10000;    // a required step failed (SMS not up yet)
static const uint8_t       GSM_SETUP_MAX_ROUNDS  = 3;        // then assume the baud/modem changed -> re-detect
static const unsigned long GSM_BODY_TIMEOUT_MS   = 2000;     // a promised SMS body stopped arriving
static const unsigned long GSM_UNREG_HOLD_MS     = 300000UL; // keep the queue through a short outage (5 min)
static const unsigned long GSM_UNREG_RESET_MS    = 900000UL; // unregistered this long -> soft reset (15 min)
static const uint8_t       GSM_GIVEUP_RESET      = 2;        // consecutive abandoned messages -> soft reset
static const unsigned long GSM_RESET_MIN_GAP_MS  = 3600000UL;// at most one soft reset per hour
static const unsigned long GSM_RESET_BOOT_MS     = 10000;    // leave the modem alone after AT+CFUN=1,1
static const unsigned long GSM_USSD_TIMEOUT_MS   = 30000;

static const size_t GSM_MSG_MAX    = 170;    // one queued segment (160 septets + spare, NUL)
static const size_t GSM_NUM_MAX    = 24;
static const size_t GSM_LINE_MAX   = 256;    // +CUSD can carry up to 182 characters plus its fields
static const size_t GSM_BODY_MAX   = 200;
static const uint8_t GSM_QUEUE_SIZE = 12;    // 2 recipients per alert, and report segments
static const uint8_t GSM_CMTI_SLOTS = 4;

/* ---- Pure helpers -------------------------------------------------------------------------------- */

// Registered? 1 home, 5 roaming, 6/7 "SMS only" (CREG on LTE), 9/10 "CSFB not preferred" (3GPP 27.007).
// One A7670C user report shows CREG sitting at 0,6, which must count -- SMS is what this rig needs.
static inline bool gsmRegStatOk(int stat) {
  return stat == 1 || stat == 5 || stat == 6 || stat == 7 || stat == 9 || stat == 10;
}

// GSM 7-bit length: these characters take an escape plus themselves. '|' is common in reports.
static inline bool gsmIsExtended(char c) { return c != 0 && strchr("^{}\\[~]|", c) != NULL; }

static inline int gsmSeptets(const char *s) {
  int n = 0;
  for (; *s; s++) n += gsmIsExtended(*s) ? 2 : 1;
  return n;
}

// End (exclusive) of the segment starting at start that fits maxSeptets. Prefers to cut just after a
// separator in the second half of the segment, so a report is not split in the middle of a value.
static inline int gsmSplitEnd(const char *s, int start, int len, int maxSeptets) {
  int used = 0, i = start;
  while (i < len) {
    int w = gsmIsExtended(s[i]) ? 2 : 1;
    if (used + w > maxSeptets) break;
    used += w;
    i++;
  }
  if (i >= len) return len;
  for (int j = i - 1; j > start + (i - start) / 2; j--) {
    if (s[j] == ',' || s[j] == '\n' || s[j] == ' ' || s[j] == '|') return j + 1;
  }
  return i > start ? i : start + 1;
}

// Dialable number only: it goes inside AT+CMGS="..." and senders can be alphanumeric ("GLOBE").
static inline bool gsmNumberOk(const char *s) {
  if (*s == '+') s++;
  int n = 0;
  for (; *s; s++, n++) if (*s < '0' || *s > '9') return false;
  return n >= 3 && n <= 20;
}

static inline bool gsmUssdCodeOk(const char *s) {
  int n = 0;
  for (; *s; s++, n++) if (!((*s >= '0' && *s <= '9') || *s == '*' || *s == '#')) return false;
  return n >= 2 && n <= 20;
}

// Copy an inbound SMS for logging, hiding anything secret. The log reaches the SD card, the Supabase
// upload and the dashboard's event list, and these commands carry passwords and API keys.
static inline void gsmSafeLogBody(const char *body, char *out, size_t outSize) {
  static const struct { const char *prefix; size_t keep; } SECRET[] = {
    { "WIFI,", 4 }, { "FBASE,AUTH", 10 }, { "FBASE,APIKEY", 12 }, { "SUPA,", 4 }, { "TSKEY,", 5 },
  };
  while (*body == ' ') body++;
  if (strcasecmp(body, "WIFI,ON") != 0 && strcasecmp(body, "WIFI,OFF") != 0) {
    for (size_t i = 0; i < sizeof(SECRET) / sizeof(SECRET[0]); i++) {
      if (strncasecmp(body, SECRET[i].prefix, strlen(SECRET[i].prefix)) == 0) {
        snprintf(out, outSize, "%.*s,***", (int)SECRET[i].keep, body);
        return;
      }
    }
  }
  snprintf(out, outSize, "%s", body);
}

// Copy the idx-th (0-based) double-quoted field of a line. False if the line has no such field.
static inline bool gsmQuotedField(const char *line, int idx, char *out, size_t outSize) {
  const char *p = line;
  for (int i = 0; ; i++) {
    const char *a = strchr(p, '"');
    if (!a) return false;
    const char *b = strchr(a + 1, '"');
    if (!b) return false;
    if (i == idx) {
      size_t n = (size_t)(b - a - 1);
      if (n >= outSize) n = outSize - 1;
      memcpy(out, a + 1, n);
      out[n] = 0;
      return true;
    }
    p = b + 1;
  }
}

// Body length from a +CMT/+CMGR header with AT+CSDH=1: the last, unquoted, comma-separated field. The
// short header (CSDH=0) ends in a quoted timestamp and returns -1 here.
static inline int gsmHeaderBodyLen(const char *line) {
  const char *c = strrchr(line, ',');
  if (!c || strchr(c, '"')) return -1;
  c++;
  while (*c == ' ') c++;
  if (*c < '0' || *c > '9') return -1;
  return atoi(c);
}

// "+CREG: 0,1" / "+CEREG: 0,1" -> 1. -1 if there is no <n>,<stat> pair.
static inline int gsmRegStat(const char *line) {
  const char *c = strchr(line, ',');
  return c ? atoi(c + 1) : -1;
}

/* ---- Driver -------------------------------------------------------------------------------------- */

class GsmModem {
public:
  struct Callbacks {
    void (*write)(const char *data, size_t len);
    void (*setBaud)(uint32_t baud);
    void (*onSms)(const char *sender, const char *body);
    void (*onUssd)(bool ok, const char *text);
    void (*log)(const char *detail);
  };

  enum State { M_DETECT, M_SETUP, M_READY };

  void begin(const Callbacks &callbacks, unsigned long now) {
    cb = callbacks;
    enterDetect(now, 0);
  }

  uint32_t baud() const { return GSM_BAUDS[baudIdx]; }

  // Queue one segment for one recipient. A full queue drops the oldest and keeps the newest alert.
  void enqueue(const char *msg, const char *to, uint8_t retries) {
    if (qCount >= GSM_QUEUE_SIZE) {
      qHead = (qHead + 1) % GSM_QUEUE_SIZE;
      qCount--;
      cb.log("TX|DROP_QUEUE_FULL");
    }
    Item &it = q[qTail];
    size_t i = 0;
    for (; msg[i] && i < GSM_MSG_MAX - 1; i++) {
      char c = msg[i];                    // Ctrl+Z / ESC in a body would end or abort the AT+CMGS exchange
      it.msg[i] = ((unsigned char)c < 0x20 && c != '\n') ? ' ' : c;
    }
    it.msg[i] = 0;
    strncpy(it.to, to, GSM_NUM_MAX - 1);
    it.to[GSM_NUM_MAX - 1] = 0;
    it.retries = retries;
    qTail = (qTail + 1) % GSM_QUEUE_SIZE;
    qCount++;
  }

  // Start one USSD balance query; the answer arrives through onUssd. False if busy or not ready.
  bool requestUssd(const char *code) {
    if (state != M_READY || ussdActive || ussdWant || !gsmUssdCodeOk(code)) return false;
    strncpy(ussdCode, code, sizeof(ussdCode) - 1);
    ussdCode[sizeof(ussdCode) - 1] = 0;
    ussdWant = true;
    return true;
  }

  void feed(char c, unsigned long now) {
    nowMs = now;
    if (bodyRemain > 0) { feedBody(c); return; }
    if (tx == T_PROMPT && c == '>' && lineLen == 0 && !lineDrop) { txPrompt = true; return; }
    if (c == '\n') {
      line[lineLen] = 0;
      bool dropped = lineDrop;
      lineLen = 0;
      lineDrop = false;
      if (!dropped) handleLine(trim(line));
    } else if (c != '\r') {
      if (lineLen < GSM_LINE_MAX - 1) line[lineLen++] = c;
      else { lineLen = 0; lineDrop = true; }          // never parse the tail of an oversized line
    }
  }

  void tick(unsigned long now, bool quiet) {
    nowMs = now;
    if ((bodyRemain > 0 || expectBodyLine) && now - bodyAt >= GSM_BODY_TIMEOUT_MS) finishBody();
    if (ussdActive && now - ussdAt >= GSM_USSD_TIMEOUT_MS) { ussdActive = false; cb.onUssd(false, "TIMEOUT"); }
    if (tx != T_IDLE) { txTick(now); return; }
    if (cmdPending) {
      if (cmdResult == CMD_NONE && now - cmdAt < cmdTimeout) return;
      cmdPending = false;
      cmdDone(cmdResult, now);
      return;
    }
    if (setupWanted) { setupWanted = false; enterSetup(now); return; }
    if ((long)(now - nextAt) < 0) return;
    switch (state) {
      case M_DETECT: sendCmd("AT", C_PROBE, GSM_PROBE_TIMEOUT_MS, now); break;
      case M_SETUP:  setupStepSend(now); break;
      case M_READY:  readyTick(now, quiet); break;
    }
  }

  State modemState() const { return state; }
  bool  ready()      const { return state == M_READY; }
  bool  absent()     const { return absentFlag; }      // a whole baud sweep got no answer
  bool  simReady()   const { return simOk; }
  bool  registered() const { return gsmRegStatOk(creg) || gsmRegStatOk(cereg); }
  int   rssi()       const { return csq; }
  int   cregStat()   const { return creg; }
  int   ceregStat()  const { return cereg; }
  int   regStat()    const { return gsmRegStatOk(cereg) ? cereg : creg; }  // what the dashboard's CREG row shows
  bool  deliveryReports() const { return drOn; }
  uint8_t queued()   const { return qCount + (hasCur ? 1 : 0); }
  unsigned long lastHealthMs() const { return healthOkAt; }
  uint16_t txOk()        const { return nTxOk; }
  uint16_t txGaveUp()    const { return nGaveUp; }
  uint16_t delivered()   const { return nDelivered; }
  uint16_t undelivered() const { return nUndelivered; }
  uint16_t resets()      const { return nResets; }

private:
  struct Item { char msg[GSM_MSG_MAX]; char to[GSM_NUM_MAX]; uint8_t retries; };
  enum Result { CMD_NONE, CMD_OK, CMD_ERR };
  enum CmdOwner { C_PROBE, C_SETUP, C_HEALTH, C_CMGR, C_CMGD, C_USSD, C_USSD_CANCEL, C_RESET, C_TX };
  enum TxState { T_IDLE, T_CMGF, T_PROMPT, T_RESULT };
  enum TxOutcome { TX_NONE, TX_ACCEPTED, TX_REJECTED };
  enum SetupStep { S_ECHO, S_TEXT, S_CSCS, S_CSDH, S_CNMI, S_CSMP, S_CLEAN, S_COUNT };

  Callbacks cb;
  State state = M_DETECT;
  unsigned long nowMs = 0, nextAt = 0;

  // Line assembly and body capture
  char line[GSM_LINE_MAX];
  size_t lineLen = 0;
  bool lineDrop = false;
  char bodySender[GSM_NUM_MAX];
  char body[GSM_BODY_MAX];
  size_t bodyLen = 0;
  int bodyRemain = 0;
  bool bodyStarted = false, bodyLineDone = false, expectBodyLine = false;
  unsigned long bodyAt = 0;
  bool skipNextLine = false;              // +CDS in PDU form: the next line is the PDU, not a result code

  // One AT command at a time
  bool cmdPending = false;
  Result cmdResult = CMD_NONE;
  CmdOwner cmdOwner = C_PROBE;
  unsigned long cmdAt = 0, cmdTimeout = 0;

  // Detect
  uint8_t baudIdx = 0, probeTries = 0, sweepCount = 0;
  bool absentFlag = false;

  // Setup
  uint8_t setupStep = 0, setupTries = 0, setupRounds = 0, cnmiOpt = 0;
  bool setupWanted = false, drOn = false;

  // Health
  uint8_t healthStep = 0, silentCount = 0;
  int csq = -1, creg = -1, cereg = -1;
  bool simOk = false;
  unsigned long healthOkAt = 0, lastRegAt = 0, lastHealthCmdAt = 0;
  bool healthDue = true;

  // Stored-message path
  int cmti[GSM_CMTI_SLOTS];
  uint8_t cmtiCount = 0;
  int cmgdIdx = -1, cmgrIdx = -1;

  // Send
  Item q[GSM_QUEUE_SIZE];
  uint8_t qHead = 0, qTail = 0, qCount = 0;
  Item cur;
  bool hasCur = false;
  uint8_t curAttempt = 0;
  unsigned long retryAt = 0;
  TxState tx = T_IDLE;
  TxOutcome txOutcome = TX_NONE;
  bool txPrompt = false, txOkLine = false;
  unsigned long txAt = 0, txAcceptedAt = 0;
  uint8_t giveUps = 0;

  // Watchdog
  bool everReset = false;
  unsigned long lastResetAt = 0;

  // USSD
  char ussdCode[24];
  char ussdBuf[GSM_LINE_MAX];
  bool ussdWant = false, ussdActive = false, ussdCancel = false, ussdCollecting = false;
  unsigned long ussdAt = 0;
  int ussdM = 0;

  uint16_t nTxOk = 0, nGaveUp = 0, nDelivered = 0, nUndelivered = 0, nResets = 0;

  /* ---- Output helpers ---- */
  void writeStr(const char *s) { cb.write(s, strlen(s)); }

  void sendCmd(const char *cmd, CmdOwner owner, unsigned long timeoutMs, unsigned long now) {
    writeStr(cmd);
    writeStr("\r\n");
    cmdPending = true;
    cmdResult = CMD_NONE;
    cmdOwner = owner;
    cmdAt = now;
    cmdTimeout = timeoutMs;
  }

  void logf(const char *fmt, const char *a) {
    char buf[240];
    snprintf(buf, sizeof(buf), fmt, a);
    cb.log(buf);
  }
  void logi(const char *fmt, long a) {
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, a);
    cb.log(buf);
  }

  static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
  }

  /* ---- Link state ---- */
  void clearStatus() {
    csq = creg = cereg = -1;
    simOk = false;
  }

  void enterDetect(unsigned long now, unsigned long holdOffMs) {
    state = M_DETECT;
    probeTries = 0;
    sweepCount = 0;
    silentCount = 0;
    setupRounds = 0;
    setupWanted = false;
    clearStatus();
    nextAt = now + holdOffMs;
  }

  void enterSetup(unsigned long now) {
    state = M_SETUP;
    setupStep = 0;
    setupTries = 0;
    cnmiOpt = 0;
    nextAt = now;
  }

  void enterReady(unsigned long now) {
    state = M_READY;
    healthStep = 0;
    healthDue = true;
    silentCount = 0;
    lastRegAt = now;
    nextAt = now;
    logf("MODEM_READY|DR=%s", drOn ? "ON" : "OFF");
  }

  /* ---- Setup sequence ---- */
  void setupStepSend(unsigned long now) {
    static const char *const CNMI_OPTS[] = { "AT+CNMI=2,2,0,1,0", "AT+CNMI=2,2,0,0,0", "AT+CNMI=2,1,0,0,0" };
    const char *c = "AT";
    switch (setupStep) {
      case S_ECHO:  c = "ATE0"; break;
      case S_TEXT:  c = "AT+CMGF=1"; break;
      case S_CSCS:  c = "AT+CSCS=\"GSM\""; break;
      case S_CSDH:  c = "AT+CSDH=1"; break;
      case S_CNMI:  c = CNMI_OPTS[cnmiOpt]; break;
      case S_CSMP:  c = drOn ? "AT+CSMP=49,167,0,0" : "AT+CSMP=17,167,0,0"; break;   // 49 asks for a status report
      case S_CLEAN: c = "AT+CMGD=1,4"; break;        // a full store blocks new SMS; a backlog is never replayed
    }
    sendCmd(c, C_SETUP, GSM_CMD_TIMEOUT_MS, now);
  }

  void setupStepNext(unsigned long now) {
    setupStep++;
    setupTries = 0;
    nextAt = now;
    if (setupStep >= S_COUNT) { setupRounds = 0; enterReady(now); }
  }

  void setupRoundFailed(unsigned long now) {
    logi("SETUP_FAIL|step=%ld", setupStep);
    if (++setupRounds >= GSM_SETUP_MAX_ROUNDS) { enterDetect(now, 0); return; }
    setupStep = 0;
    setupTries = 0;
    cnmiOpt = 0;
    nextAt = now + GSM_SETUP_WAIT_MS;
  }

  void setupDone(Result r, unsigned long now) {
    if (r == CMD_OK) {
      if (setupStep == S_CNMI) {
        drOn = (cnmiOpt == 0);
        logi("CNMI_OPT|%ld", cnmiOpt);
      }
      setupStepNext(now);
      return;
    }
    if (setupStep == S_CNMI) {                       // walk the fallbacks, one try each
      if (++cnmiOpt < 3) { nextAt = now; return; }
      setupRoundFailed(now);
      return;
    }
    if (++setupTries < GSM_SETUP_STEP_TRIES) { nextAt = now + GSM_SETUP_RETRY_MS; return; }
    if (setupStep == S_TEXT) { setupRoundFailed(now); return; }   // text mode is the only other must-have
    logi("SETUP_SKIP|step=%ld", setupStep);
    setupStepNext(now);
  }

  /* ---- Command completion ---- */
  void cmdDone(Result r, unsigned long now) {
    switch (cmdOwner) {
      case C_PROBE: probeDone(r, now); break;
      case C_SETUP: setupDone(r, now); break;
      case C_HEALTH: healthDone(r, now); break;
      case C_CMGR:
        if (r == CMD_OK && cmgrIdx > 0) cmgdIdx = cmgrIdx;
        cmgrIdx = -1;
        break;
      case C_CMGD: case C_USSD_CANCEL: case C_TX: break;
      case C_USSD:
        if (r != CMD_OK) { ussdActive = false; cb.onUssd(false, "ERROR"); }
        break;
      case C_RESET:
        enterDetect(now, GSM_RESET_BOOT_MS);
        break;
    }
  }

  void probeDone(Result r, unsigned long now) {
    if (r != CMD_NONE) {                               // OK or ERROR: either way it speaks this baud
      absentFlag = false;
      logi("MODEM_FOUND|%ld", (long)GSM_BAUDS[baudIdx]);
      enterSetup(now);
      return;
    }
    if (++probeTries < GSM_PROBE_TRIES) return;
    probeTries = 0;
    baudIdx = (baudIdx + 1) % GSM_BAUD_COUNT;
    cb.setBaud(GSM_BAUDS[baudIdx]);
    lineLen = 0;
    lineDrop = false;
    if (++sweepCount >= GSM_BAUD_COUNT) {
      sweepCount = 0;
      if (!absentFlag) cb.log("MODEM_ABSENT");
      absentFlag = true;
      nextAt = now + GSM_SWEEP_GAP_MS;
    }
  }

  void healthDone(Result r, unsigned long now) {
    if (r == CMD_NONE) {
      if (++silentCount >= GSM_SILENT_LIMIT) {
        cb.log("MODEM_SILENT");
        enterDetect(now, 0);
      }
      return;
    }
    silentCount = 0;
    healthOkAt = now;
    if (healthStep == 3 && r == CMD_ERR) simOk = false;   // +CME ERROR: SIM not inserted, etc.
    healthStep = (healthStep + 1) % 4;
  }

  /* ---- Ready: one job per tick, highest priority first ---- */
  void readyTick(unsigned long now, bool quiet) {
    if (registered()) lastRegAt = now;
    char buf[48];
    if (cmgdIdx > 0) {
      snprintf(buf, sizeof(buf), "AT+CMGD=%d", cmgdIdx);
      cmgdIdx = -1;
      sendCmd(buf, C_CMGD, GSM_CMD_TIMEOUT_MS, now);
      return;
    }
    if (cmtiCount > 0) {
      cmgrIdx = cmti[0];
      for (uint8_t i = 1; i < cmtiCount; i++) cmti[i - 1] = cmti[i];
      cmtiCount--;
      snprintf(buf, sizeof(buf), "AT+CMGR=%d", cmgrIdx);
      sendCmd(buf, C_CMGR, GSM_CMD_TIMEOUT_MS, now);
      return;
    }
    if (ussdCancel) {
      ussdCancel = false;
      sendCmd("AT+CUSD=2", C_USSD_CANCEL, GSM_CMD_TIMEOUT_MS, now);
      return;
    }
    if (ussdWant) {
      ussdWant = false;
      ussdActive = true;
      ussdAt = now;
      snprintf(buf, sizeof(buf), "AT+CUSD=1,\"%s\",15", ussdCode);
      sendCmd(buf, C_USSD, GSM_CMD_TIMEOUT_MS, now);
      return;
    }
    if (trySend(now, quiet)) return;
    if (tryWatchdog(now, quiet)) return;
    if (now - lastHealthCmdAt >= GSM_HEALTH_STEP_MS || healthDue) {
      static const char *const Q[] = { "AT+CSQ", "AT+CREG?", "AT+CEREG?", "AT+CPIN?" };
      healthDue = false;
      lastHealthCmdAt = now;
      sendCmd(Q[healthStep], C_HEALTH, GSM_CMD_TIMEOUT_MS, now);
    }
  }

  bool tryWatchdog(unsigned long now, bool quiet) {
    if (quiet) return false;
    const char *why = NULL;
    if (giveUps >= GSM_GIVEUP_RESET) why = "TX_FAIL";
    else if (!registered() && now - lastRegAt >= GSM_UNREG_RESET_MS) why = "NOREG";
    if (!why) return false;
    if (everReset && now - lastResetAt < GSM_RESET_MIN_GAP_MS) return false;
    everReset = true;
    lastResetAt = now;
    nResets++;
    giveUps = 0;
    logf("MODEM_RESET|%s", why);
    sendCmd("AT+CFUN=1,1", C_RESET, GSM_CMD_TIMEOUT_MS, now);
    return true;
  }

  /* ---- Send ---- */
  bool trySend(unsigned long now, bool quiet) {
    if (quiet) return false;                         // no transmit burst during a battery switch
    if (!registered() && now - lastRegAt < GSM_UNREG_HOLD_MS) return false;   // hold, don't burn the retries
    if (hasCur) {
      if ((long)(now - retryAt) < 0) return false;   // backoff: later messages keep their order behind it
    } else {
      if (qCount == 0) return false;
      cur = q[qHead];
      qHead = (qHead + 1) % GSM_QUEUE_SIZE;
      qCount--;
      if (!gsmNumberOk(cur.to)) {                    // empty owner number, or an alphanumeric sender
        logf("TX|BAD_NUMBER|%s", cur.to);
        return false;
      }
      hasCur = true;
      curAttempt = 0;
    }
    curAttempt++;
    txOutcome = TX_NONE;
    txPrompt = false;
    txOkLine = false;
    sendCmd("AT+CMGF=1", C_TX, GSM_CMGF_TIMEOUT_MS, now);
    tx = T_CMGF;
    return true;
  }

  void txTick(unsigned long now) {
    switch (tx) {
      case T_IDLE: break;
      case T_CMGF:
        if (cmdResult == CMD_OK) {
          cmdPending = false;
          writeStr("AT+CMGS=\"");
          writeStr(cur.to);
          writeStr("\"\r");                          // CR only: an LF here can land in the message body
          txAt = now;
          tx = T_PROMPT;
        } else if (cmdResult == CMD_ERR || now - cmdAt >= GSM_CMGF_TIMEOUT_MS) {
          cmdPending = false;
          cb.log("TX|FAIL_CMGF");
          txFailed(now);
        }
        break;
      case T_PROMPT:
        if (txPrompt) {
          const char ctrlZ = 0x1A;
          writeStr(cur.msg);
          cb.write(&ctrlZ, 1);
          logf("TX|%s", cur.msg);
          txAt = now;
          tx = T_RESULT;
        } else if (txOutcome == TX_REJECTED) {
          txFailed(now);
        } else if (now - txAt >= GSM_PROMPT_TIMEOUT_MS) {
          abortSend();
          cb.log("TX|FAIL_NO_PROMPT");
          txFailed(now);
        }
        break;
      case T_RESULT:
        if (txOutcome == TX_REJECTED) {
          txFailed(now);
        } else if (txOutcome == TX_ACCEPTED) {
          if (txOkLine || now - txAcceptedAt >= GSM_RESULT_OK_GRACE_MS) txSucceeded();
        } else if (now - txAt >= GSM_RESULT_TIMEOUT_MS) {
          abortSend();
          cb.log("TX_UNKNOWN");
          txFailed(now);
        }
        break;
    }
  }

  void abortSend() {
    const char esc = 0x1B;
    cb.write(&esc, 1);
  }

  void txSucceeded() {
    tx = T_IDLE;
    hasCur = false;
    giveUps = 0;
    nTxOk++;
  }

  void txFailed(unsigned long now) {
    tx = T_IDLE;
    if (cur.retries > 0) {
      cur.retries--;
      retryAt = now + GSM_RETRY_BACKOFF_MS * curAttempt;
      logi("TX|RETRY|left=%ld", cur.retries);
      return;
    }
    char head[32];
    strncpy(head, cur.msg, sizeof(head) - 1);
    head[sizeof(head) - 1] = 0;
    logf("TX|GAVE_UP|%s", head);
    hasCur = false;
    nGaveUp++;
    if (giveUps < 255) giveUps++;
  }

  /* ---- Inbound ---- */
  void startBody(const char *hdr, int senderField) {
    if (!gsmQuotedField(hdr, senderField, bodySender, sizeof(bodySender))) bodySender[0] = 0;
    int n = gsmHeaderBodyLen(hdr);
    bodyLen = 0;
    bodyStarted = false;
    bodyLineDone = false;
    bodyAt = nowMs;
    if (n > 0) bodyRemain = n;
    else if (n < 0) expectBodyLine = true;           // CSDH refused: next line is the body, spoof guard weaker
  }

  void feedBody(char c) {
    bodyAt = nowMs;
    if (!bodyStarted && (c == '\r' || c == '\n')) return;    // framing before the text, not part of its length
    bodyStarted = true;
    if (c == '\r' || c == '\n') bodyLineDone = true;         // commands are one line; the rest is consumed unread
    else if (!bodyLineDone && bodyLen < GSM_BODY_MAX - 1) body[bodyLen++] = c;
    if (--bodyRemain == 0) finishBody();
  }

  void finishBody() {
    bodyRemain = 0;
    expectBodyLine = false;
    body[bodyLen] = 0;
    // No sender must never reach the handler: main.cpp reads an empty reply target as "internally
    // generated" and senderIsOwner() would pass it.
    if (bodyLen > 0 && bodySender[0]) cb.onSms(bodySender, body);
    else if (bodyLen > 0) cb.log("RX|NO_SENDER");
    bodyLen = 0;
  }

  void handleUssdLine(const char *s) {
    ussdM = atoi(s + 6);
    const char *a = strchr(s, '"');
    if (ussdM == 1) ussdCancel = true;               // a menu is waiting: close it, it holds the session open
    if (!a) {
      if (ussdActive) { ussdActive = false; cb.onUssd(false, ussdM >= 4 ? "UNSUPPORTED" : "NO_TEXT"); }
      return;
    }
    ussdBuf[0] = 0;
    strncat(ussdBuf, a + 1, sizeof(ussdBuf) - 1);
    char *b = strrchr(ussdBuf, '"');
    if (b) { *b = 0; finishUssd(); }
    else ussdCollecting = true;                      // the text spans several lines
  }

  void finishUssd() {
    ussdCollecting = false;
    if (!ussdActive) return;                         // network-initiated: not something we asked for
    ussdActive = false;
    cb.onUssd(true, ussdBuf);
  }

  void handleCds(const char *s) {
    if (!strchr(s, ',')) { skipNextLine = true; return; }    // PDU form: nothing readable here
    long mr = atol(strchr(s, ',') + 1);
    const char *lastComma = strrchr(s, ',');
    int st = atoi(lastComma + 1);
    char buf[48];
    if (st < 32) {                                   // 3GPP 23.040: 0-31 completed
      nDelivered++;
      snprintf(buf, sizeof(buf), "TX_DELIVERED|%ld", mr);
    } else if (st < 64) {                            // 32-63: the SMSC is still trying
      snprintf(buf, sizeof(buf), "TX_PENDING|%ld|st=%d", mr, st);
    } else {                                         // 64+: permanent; a re-send would fail the same way
      nUndelivered++;
      snprintf(buf, sizeof(buf), "TX_UNDELIVERED|%ld|st=%d", mr, st);
    }
    cb.log(buf);
  }

  void handleLine(char *s) {
    if (expectBodyLine) {
      if (*s == 0) return;
      strncpy(body, s, GSM_BODY_MAX - 1);
      body[GSM_BODY_MAX - 1] = 0;
      bodyLen = strlen(body);
      finishBody();
      return;
    }
    if (skipNextLine) {
      if (*s) skipNextLine = false;
      return;
    }
    if (ussdCollecting) {
      size_t have = strlen(ussdBuf);
      if (have < sizeof(ussdBuf) - 2) {
        ussdBuf[have] = ' ';
        ussdBuf[have + 1] = 0;
        strncat(ussdBuf, s, sizeof(ussdBuf) - have - 2);
      }
      char *b = strrchr(ussdBuf, '"');
      if (b) { *b = 0; finishUssd(); }
      return;
    }
    if (*s == 0) return;

    if      (!strncmp(s, "+CMT:", 5))   startBody(s, 0);
    else if (!strncmp(s, "+CMGR:", 6))  { if (cmdPending && cmdOwner == C_CMGR) startBody(s, 1); }
    else if (!strncmp(s, "+CMTI:", 6))  handleCmti(s);
    else if (!strncmp(s, "+CDS:", 5))   handleCds(s);
    else if (!strncmp(s, "+CUSD:", 6))  handleUssdLine(s);
    else if (!strncmp(s, "+CSQ:", 5))   csq = atoi(s + 5);
    else if (!strncmp(s, "+CREG:", 6))  creg = gsmRegStat(s);
    else if (!strncmp(s, "+CEREG:", 7)) cereg = gsmRegStat(s);
    else if (!strncmp(s, "+CPIN:", 6))  simOk = (strstr(s, "READY") != NULL);
    else if (!strncmp(s, "+CMGS:", 6))  handleAccepted(s);
    else if (!strcmp(s, "RDY") || !strncmp(s, "*ATREADY", 8) || !strcmp(s, "SMS DONE")) handleBoot(s);
    else if (!strcmp(s, "OK"))          handleOk();
    else if (!strcmp(s, "ERROR"))       handleError("GENERIC");
    else if (!strncmp(s, "+CMS ERROR:", 11)) handleError(trim(s + 11));
    else if (!strncmp(s, "+CME ERROR:", 11)) handleError(trim(s + 11));
  }

  void handleCmti(const char *s) {
    const char *c = strchr(s, ',');
    if (!c) return;
    if (cmtiCount >= GSM_CMTI_SLOTS) { cb.log("RX|CMTI_OVERFLOW"); return; }
    cmti[cmtiCount++] = atoi(c + 1);
  }

  void handleAccepted(char *s) {
    if (tx != T_RESULT) return;
    logf("TX_OK|%s", trim(s + 6));
    txOutcome = TX_ACCEPTED;
    txAcceptedAt = nowMs;
  }

  void handleBoot(const char *s) {
    logf("MODEM_BOOT|%s", s);
    if (state == M_DETECT) return;                   // detect goes on to setup anyway
    setupRounds = 0;
    setupWanted = true;
  }

  void handleOk() {
    if (cmdPending) cmdResult = CMD_OK;
    if (tx == T_RESULT && txOutcome == TX_ACCEPTED) txOkLine = true;
  }

  void handleError(const char *code) {
    if (tx == T_PROMPT || tx == T_RESULT) {
      logf("TX_ERR|%s", code);
      txOutcome = TX_REJECTED;
    } else if (cmdPending) {
      cmdResult = CMD_ERR;
    }
  }
};

#endif
