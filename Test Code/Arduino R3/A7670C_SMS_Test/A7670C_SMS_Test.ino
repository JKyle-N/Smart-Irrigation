/*
  A7670C SMS Test  -  Arduino Uno R3
  ------------------------------------------------------------------
  Send an SMS from the Serial Monitor and read incoming SMS on it.

  WIRING (module powered from its own 5-12 V / 2 A supply, common GND)
    Module TX  -> Uno D2   (SoftwareSerial RX)
    Module RX  -> Uno D3   (SoftwareSerial TX)
    Module GND -> Uno GND  (and supply GND)
    Module VIN -> external supply +

  SERIAL MONITOR
    Baud: 115200      Line ending: Newline or Both NL & CR

  The module must already be set to 9600 baud:
    AT+IPR=9600  then  AT&W   (one-time step done over D0/D1)

  COMMANDS (type in Serial Monitor)
    send <number> <message>   e.g.  send +639171234567 Hello from Arduino
    read                      list all SMS stored in the module
    del                       delete all stored SMS
    status                    SIM / signal / network registration
    init                      re-run module setup (after a module reset)
    AT...                     anything starting with AT goes to the module
    help                      show this list

  Incoming SMS are shown automatically.
  Keep messages to 160 plain characters (one SMS, no emoji).
*/

#include <SoftwareSerial.h>
#include <avr/pgmspace.h>
#include <string.h>
#include <stdlib.h>

// ---------------- Settings ----------------
const uint8_t  MODEM_RX_PIN = 2;       // Uno RX  <- module TX
const uint8_t  MODEM_TX_PIN = 3;       // Uno TX  -> module RX
const uint32_t MODEM_BAUD   = 9600;
const uint32_t PC_BAUD      = 115200;

const uint8_t  CMD_BUF_SIZE   = 200;
const uint8_t  MODEM_BUF_SIZE = 200;
const uint8_t  SMS_MAX_LEN    = 160;

// waitFor() results
const uint8_t R_TIMEOUT = 0;
const uint8_t R_OK      = 1;
const uint8_t R_ERR     = 2;

SoftwareSerial modem(MODEM_RX_PIN, MODEM_TX_PIN);

char    cmdBuf[CMD_BUF_SIZE];     // line typed in Serial Monitor
uint8_t cmdLen = 0;

char    mBuf[MODEM_BUF_SIZE];     // line coming from the module
uint8_t mLen = 0;

bool smsBodyNext = false;         // next modem line is an SMS body
bool inPoll      = false;         // guards against re-entry

// ---------------- Prototypes ----------------
uint8_t waitFor(const char* okStr, const char* errStr, uint32_t idleTimeoutMs);
uint8_t finishCmd(uint32_t timeoutMs);
uint8_t runAT(const __FlashStringHelper* cmd, uint32_t timeoutMs);
uint8_t runATRam(const char* cmd, uint32_t timeoutMs);
void    readSms(int idx);
void    handleModemLine(char* line);
void    pollModem();
void    pollUser();
void    handleCommand(char* s);
void    cmdSend(char* args);
bool    sendSms(const char* number, const char* text);
void    showStatus();
bool    initModem();
void    printHelp();

// ======================================================================
//  Low-level helpers
// ======================================================================

// Read from the module (echoing everything to the Serial Monitor) until
// okStr or errStr shows up. Timeout is "idle" time: it restarts whenever
// a byte arrives.
uint8_t waitFor(const char* okStr, const char* errStr, uint32_t idleTimeoutMs) {
  char tail[12];
  memset(tail, ' ', sizeof(tail) - 1);
  tail[sizeof(tail) - 1] = 0;

  uint32_t last = millis();
  while (millis() - last < idleTimeoutMs) {
    while (modem.available()) {
      char c = modem.read();
      last = millis();
      Serial.write(c);
      memmove(tail, tail + 1, sizeof(tail) - 2);
      tail[sizeof(tail) - 2] = c;
      if (strstr(tail, okStr))  return R_OK;
      if (strstr(tail, errStr)) return R_ERR;
    }
  }
  return R_TIMEOUT;
}

uint8_t finishCmd(uint32_t timeoutMs) {
  uint8_t r = waitFor("OK\r\n", "ERROR", timeoutMs);
  if (r == R_TIMEOUT) Serial.println(F("[!] Timeout - no reply from module"));
  return r;
}

// Send an AT command stored in flash (F("...")) and print the reply.
uint8_t runAT(const __FlashStringHelper* cmd, uint32_t timeoutMs) {
  pollModem();
  Serial.print(F(">> "));
  Serial.println(cmd);
  modem.println(cmd);
  return finishCmd(timeoutMs);
}

// Send an AT command held in RAM and print the reply.
uint8_t runATRam(const char* cmd, uint32_t timeoutMs) {
  pollModem();
  Serial.print(F(">> "));
  Serial.println(cmd);
  modem.println(cmd);
  return finishCmd(timeoutMs);
}

// ======================================================================
//  Incoming data from the module
// ======================================================================

void readSms(int idx) {
  Serial.print(F(">> AT+CMGR="));
  Serial.println(idx);
  modem.print(F("AT+CMGR="));
  modem.println(idx);
  finishCmd(5000);
}

void handleModemLine(char* line) {
  // Body line that follows a +CMT header
  if (smsBodyNext) {
    smsBodyNext = false;
    Serial.print(F("Message: "));
    Serial.println(line);
    Serial.println(F("===========================\n"));
    return;
  }

  // New SMS delivered directly:  +CMT: "<number>","","<timestamp>"
  if (strncmp_P(line, PSTR("+CMT:"), 5) == 0) {
    char*   q[6];
    uint8_t n = 0;
    char*   p = line;
    while (n < 6 && (p = strchr(p, '"')) != NULL) {
      q[n++] = p;
      p++;
    }
    Serial.println();
    Serial.println(F("========== NEW SMS =========="));
    if (n >= 2) {
      *q[1] = 0;
      Serial.print(F("From   : "));
      Serial.println(q[0] + 1);
    }
    if (n >= 4) {
      *q[n - 1] = 0;
      Serial.print(F("Time   : "));
      Serial.println(q[n - 2] + 1);
    }
    smsBodyNext = true;
    return;
  }

  // New SMS stored in the module:  +CMTI: "SM",<index>
  if (strncmp_P(line, PSTR("+CMTI:"), 6) == 0) {
    char* comma = strchr(line, ',');
    Serial.println();
    Serial.println(F("====== NEW SMS (stored in module) ======"));
    if (comma) readSms(atoi(comma + 1));
    return;
  }

  // Anything else (RDY, +CPIN: READY, SMS DONE, ...)
  Serial.print(F("[modem] "));
  Serial.println(line);
}

// Collect lines from the module while idle (handles incoming SMS).
void pollModem() {
  if (inPoll) return;
  inPoll = true;
  while (modem.available()) {
    char c = modem.read();
    if (c == '\r') continue;
    if (c == '\n') {
      mBuf[mLen] = 0;
      uint8_t n = mLen;
      mLen = 0;
      if (n) handleModemLine(mBuf);
    } else {
      mBuf[mLen++] = c;
      if (mLen >= MODEM_BUF_SIZE - 1) {      // overflow: flush what we have
        mBuf[mLen] = 0;
        mLen = 0;
        handleModemLine(mBuf);
      }
    }
  }
  inPoll = false;
}

// ======================================================================
//  Sending SMS
// ======================================================================

bool sendSms(const char* number, const char* text) {
  pollModem();
  Serial.print(F("[TX] To "));
  Serial.print(number);
  Serial.print(F(": "));
  Serial.println(text);

  modem.print(F("AT+CMGS=\""));
  modem.print(number);
  modem.print(F("\"\r"));

  uint8_t r = waitFor("> ", "ERROR", 10000);
  if (r != R_OK) {
    Serial.println(F("\n[!] No '>' prompt from module - cancelled"));
    modem.write((uint8_t)0x1B);               // ESC = cancel
    return false;
  }

  modem.print(text);
  modem.write((uint8_t)0x1A);                 // Ctrl+Z = send

  Serial.println(F("\n[..] Waiting for network (up to 60 s)..."));
  r = waitFor("OK\r\n", "ERROR", 60000);
  if (r == R_OK) {
    Serial.println(F("[OK] SMS sent"));
    return true;
  }
  if (r == R_ERR) {
    Serial.println(F("\n[FAIL] Module reported an error (see +CMS ERROR code above)"));
  } else {
    Serial.println(F("\n[FAIL] Timeout waiting for send confirmation"));
    modem.write((uint8_t)0x1B);
  }
  return false;
}

void cmdSend(char* args) {
  while (*args == ' ') args++;
  char* number = args;
  char* sp = strchr(args, ' ');
  if (!sp) {
    Serial.println(F("Usage: send <number> <message>"));
    return;
  }
  *sp = 0;
  char* text = sp + 1;
  while (*text == ' ') text++;

  if (!*number || !*text) {
    Serial.println(F("Usage: send <number> <message>"));
    return;
  }
  for (char* p = number; *p; p++) {
    bool digit = (*p >= '0' && *p <= '9');
    bool plus  = (*p == '+' && p == number);
    if (!digit && !plus) {
      Serial.println(F("[!] Number may only contain digits and a leading +"));
      return;
    }
  }
  if (strlen(text) > SMS_MAX_LEN) {
    Serial.println(F("[!] Message longer than 160 characters"));
    return;
  }
  sendSms(number, text);
}

// ======================================================================
//  Status / init / help
// ======================================================================

void showStatus() {
  Serial.println(F("---- Module status ----"));
  runAT(F("ATI"),       2000);
  runAT(F("AT+CPIN?"),  3000);   // READY = SIM OK
  runAT(F("AT+CSQ"),    2000);   // 10-31 good, 99 = no signal
  runAT(F("AT+CEREG?"), 2000);   // LTE:  ,1 home  ,5 roaming
  runAT(F("AT+CREG?"),  2000);   // GSM:  ,1 home  ,5 roaming
  runAT(F("AT+COPS?"),  5000);   // operator name
  runAT(F("AT+CSCA?"),  3000);   // SMS center number (must not be empty)
  Serial.println(F("-----------------------"));
}

bool initModem() {
  Serial.println(F("Initialising module..."));

  bool alive = false;
  for (uint8_t i = 0; i < 5 && !alive; i++) {
    alive = (runAT(F("AT"), 1000) == R_OK);
    if (!alive) delay(500);
  }
  if (!alive) {
    Serial.println(F("\n[!] Module not answering. Check:"));
    Serial.println(F("    - external supply on, common GND with the Uno"));
    Serial.println(F("    - module TX -> D2, module RX -> D3 (not swapped)"));
    Serial.println(F("    - module baud is 9600 (AT+IPR=9600, AT&W)"));
    Serial.println(F("    - PWK pulse if the module is not powered on"));
    Serial.println(F("  Fix it, then type: init"));
    return false;
  }

  runAT(F("ATE0"),            1000);   // echo off
  runAT(F("AT+CMGF=1"),       2000);   // SMS text mode
  runAT(F("AT+CSCS=\"GSM\""), 2000);   // plain GSM character set

  // Deliver incoming SMS straight to the serial port (+CMT)
  if (runAT(F("AT+CNMI=2,2,0,0,0"), 2000) != R_OK) {
    Serial.println(F("[i] Falling back to CNMI=2,1 (store + notify)"));
    runAT(F("AT+CNMI=2,1,0,0,0"), 2000);
  }

  showStatus();
  Serial.println(F("Ready when CPIN is READY and CEREG/CREG ends in ,1 or ,5."));
  Serial.println(F("If not registered yet, wait 10-30 s and type: status\n"));
  return true;
}

void printHelp() {
  Serial.println(F("Commands (Serial Monitor: 115200 baud, Newline):"));
  Serial.println(F("  send <number> <message>  e.g. send +639171234567 Hello"));
  Serial.println(F("  read                     list stored SMS"));
  Serial.println(F("  del                      delete all stored SMS"));
  Serial.println(F("  status                   SIM, signal, registration"));
  Serial.println(F("  init                     re-run module setup"));
  Serial.println(F("  AT...                    raw AT command to the module"));
  Serial.println(F("  help                     show this list"));
  Serial.println(F("Incoming SMS appear automatically.\n"));
}

// ======================================================================
//  Serial Monitor input
// ======================================================================

void handleCommand(char* s) {
  while (*s == ' ') s++;
  if (!*s) return;

  if (strcasecmp_P(s, PSTR("help")) == 0 || strcmp(s, "?") == 0) {
    printHelp();
  } else if (strcasecmp_P(s, PSTR("status")) == 0) {
    showStatus();
  } else if (strcasecmp_P(s, PSTR("init")) == 0) {
    initModem();
  } else if (strcasecmp_P(s, PSTR("read")) == 0) {
    runAT(F("AT+CMGL=\"ALL\""), 10000);
  } else if (strcasecmp_P(s, PSTR("del")) == 0) {
    runAT(F("AT+CMGD=1,4"), 10000);
  } else if (strncasecmp_P(s, PSTR("send "), 5) == 0) {
    cmdSend(s + 5);
  } else if (strncasecmp_P(s, PSTR("AT"), 2) == 0) {
    runATRam(s, 10000);
  } else {
    Serial.println(F("Unknown command. Type: help"));
  }
}

void pollUser() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (cmdLen) {
        cmdBuf[cmdLen] = 0;
        cmdLen = 0;
        handleCommand(cmdBuf);
      }
    } else if (cmdLen < CMD_BUF_SIZE - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

// ======================================================================
//  Arduino entry points
// ======================================================================

void setup() {
  Serial.begin(PC_BAUD);
  modem.begin(MODEM_BAUD);
  delay(1000);

  Serial.println(F("\n=== A7670C SMS test (Arduino Uno) ==="));
  initModem();
  printHelp();
}

void loop() {
  pollUser();
  pollModem();
}
