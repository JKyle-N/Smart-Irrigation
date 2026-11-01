/*
  A7670C SMS Test  -  ESP32
  ------------------------------------------------------------------
  Send an SMS from the Serial Monitor and read incoming SMS on it.
  The module baud rate is detected automatically (no AT+IPR needed).

  WIRING (module powered from its own 5-12 V / 2 A supply)
    Module TX  -> ESP32 GPIO16 (RX2)
    Module RX  -> ESP32 GPIO17 (TX2)
    Module GND -> ESP32 GND  (and supply GND)
    Module VIN -> external supply +
    PEN, NET, PWK -> leave open
    ESP32 is powered by its USB cable.

  ARDUINO IDE
    Board: "ESP32 Dev Module"
    Serial Monitor: 115200 baud, line ending Newline or Both NL & CR

  COMMANDS (type in Serial Monitor)
    send <number> <message>   e.g.  send +639171234567 Hello from ESP32
    read                      list all SMS stored in the module
    del                       delete all stored SMS
    status                    SIM / signal / network registration
    init                      re-run module detection and setup
    AT...                     anything starting with AT goes to the module
    help                      show this list

  Incoming SMS are shown automatically.
  Keep messages to 160 plain characters (one SMS, no emoji).
*/

#include <string.h>
#include <stdlib.h>
#include <strings.h>

// ---------------- Settings ----------------
const int      MODEM_RX_PIN = 16;      // ESP32 RX2 <- module TX
const int      MODEM_TX_PIN = 17;      // ESP32 TX2 -> module RX
const uint32_t PC_BAUD      = 115200;

const uint32_t BAUD_LIST[] = {115200, 9600, 57600, 38400, 19200, 230400, 460800};
const uint8_t  BAUD_COUNT  = sizeof(BAUD_LIST) / sizeof(BAUD_LIST[0]);

const uint16_t CMD_BUF_SIZE   = 256;
const uint16_t MODEM_BUF_SIZE = 256;
const uint16_t SMS_MAX_LEN    = 160;

// waitFor() results
const uint8_t RES_TIMEOUT = 0;
const uint8_t RES_OK      = 1;
const uint8_t RES_ERR     = 2;

HardwareSerial &modem = Serial2;

char     cmdBuf[CMD_BUF_SIZE];     // line typed in Serial Monitor
uint16_t cmdLen = 0;

char     mBuf[MODEM_BUF_SIZE];     // line coming from the module
uint16_t mLen = 0;

bool smsBodyNext = false;          // next modem line is an SMS body
bool inPoll      = false;          // guards against re-entry

// ---------------- Prototypes ----------------
uint8_t waitFor(const char* okStr, const char* errStr, uint32_t idleTimeoutMs);
uint8_t finishCmd(uint32_t timeoutMs);
uint8_t runAT(const char* cmd, uint32_t timeoutMs);
void    readSms(int idx);
void    handleModemLine(char* line);
void    pollModem();
void    pollUser();
void    handleCommand(char* s);
void    cmdSend(char* args);
bool    sendSms(const char* number, const char* text);
bool    probeAT();
bool    findBaud();
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
      if (strstr(tail, okStr))  return RES_OK;
      if (strstr(tail, errStr)) return RES_ERR;
    }
    delay(1);
  }
  return RES_TIMEOUT;
}

uint8_t finishCmd(uint32_t timeoutMs) {
  uint8_t r = waitFor("OK\r\n", "ERROR", timeoutMs);
  if (r == RES_TIMEOUT) Serial.println("[!] Timeout - no reply from module");
  return r;
}

// Send an AT command and print the reply.
uint8_t runAT(const char* cmd, uint32_t timeoutMs) {
  pollModem();
  Serial.print(">> ");
  Serial.println(cmd);
  modem.print(cmd);
  modem.print("\r\n");
  return finishCmd(timeoutMs);
}

// ======================================================================
//  Incoming data from the module
// ======================================================================

void readSms(int idx) {
  Serial.print(">> AT+CMGR=");
  Serial.println(idx);
  modem.print("AT+CMGR=");
  modem.print(idx);
  modem.print("\r\n");
  finishCmd(5000);
}

void handleModemLine(char* line) {
  // Body line that follows a +CMT header
  if (smsBodyNext) {
    smsBodyNext = false;
    Serial.print("Message: ");
    Serial.println(line);
    Serial.println("===========================\n");
    return;
  }

  // New SMS delivered directly:  +CMT: "<number>","","<timestamp>"
  if (strncmp(line, "+CMT:", 5) == 0) {
    char*   q[6];
    uint8_t n = 0;
    char*   p = line;
    while (n < 6 && (p = strchr(p, '"')) != NULL) {
      q[n++] = p;
      p++;
    }
    Serial.println();
    Serial.println("========== NEW SMS ==========");
    if (n >= 2) {
      *q[1] = 0;
      Serial.print("From   : ");
      Serial.println(q[0] + 1);
    }
    if (n >= 4) {
      *q[n - 1] = 0;
      Serial.print("Time   : ");
      Serial.println(q[n - 2] + 1);
    }
    smsBodyNext = true;
    return;
  }

  // New SMS stored in the module:  +CMTI: "SM",<index>
  if (strncmp(line, "+CMTI:", 6) == 0) {
    char* comma = strchr(line, ',');
    Serial.println();
    Serial.println("====== NEW SMS (stored in module) ======");
    if (comma) readSms(atoi(comma + 1));
    return;
  }

  // Anything else (RDY, +CPIN: READY, SMS DONE, ...)
  Serial.print("[modem] ");
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
      uint16_t n = mLen;
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
  Serial.print("[TX] To ");
  Serial.print(number);
  Serial.print(": ");
  Serial.println(text);

  modem.print("AT+CMGS=\"");
  modem.print(number);
  modem.print("\"\r");

  uint8_t r = waitFor("> ", "ERROR", 10000);
  if (r != RES_OK) {
    Serial.println("\n[!] No '>' prompt from module - cancelled");
    modem.write((uint8_t)0x1B);               // ESC = cancel
    return false;
  }

  modem.print(text);
  modem.write((uint8_t)0x1A);                 // Ctrl+Z = send

  Serial.println("\n[..] Waiting for network (up to 60 s)...");
  r = waitFor("OK\r\n", "ERROR", 60000);
  if (r == RES_OK) {
    Serial.println("[OK] SMS sent");
    return true;
  }
  if (r == RES_ERR) {
    Serial.println("\n[FAIL] Module reported an error (see +CMS ERROR code above)");
  } else {
    Serial.println("\n[FAIL] Timeout waiting for send confirmation");
    modem.write((uint8_t)0x1B);
  }
  return false;
}

void cmdSend(char* args) {
  while (*args == ' ') args++;
  char* number = args;
  char* sp = strchr(args, ' ');
  if (!sp) {
    Serial.println("Usage: send <number> <message>");
    return;
  }
  *sp = 0;
  char* text = sp + 1;
  while (*text == ' ') text++;

  if (!*number || !*text) {
    Serial.println("Usage: send <number> <message>");
    return;
  }
  for (char* p = number; *p; p++) {
    bool digit = (*p >= '0' && *p <= '9');
    bool plus  = (*p == '+' && p == number);
    if (!digit && !plus) {
      Serial.println("[!] Number may only contain digits and a leading +");
      return;
    }
  }
  if (strlen(text) > SMS_MAX_LEN) {
    Serial.println("[!] Message longer than 160 characters");
    return;
  }
  sendSms(number, text);
}

// ======================================================================
//  Baud detection / status / init / help
// ======================================================================

// Quiet "AT" probe: true if the module answers OK.
bool probeAT() {
  while (modem.available()) modem.read();
  modem.print("AT\r\n");

  char     buf[24];
  uint8_t  n = 0;
  buf[0] = 0;
  uint32_t t = millis();
  while (millis() - t < 700) {
    while (modem.available()) {
      char c = modem.read();
      if (n < sizeof(buf) - 1) {
        buf[n++] = c;
        buf[n] = 0;
      }
      if (strstr(buf, "OK")) return true;
    }
    delay(1);
  }
  return false;
}

// Try each baud rate until the module answers.
bool findBaud() {
  for (uint8_t b = 0; b < BAUD_COUNT; b++) {
    modem.end();
    modem.begin(BAUD_LIST[b], SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    delay(100);
    Serial.print("Trying ");
    Serial.print(BAUD_LIST[b]);
    Serial.println(" baud...");
    for (uint8_t t = 0; t < 3; t++) {
      if (probeAT()) {
        Serial.print("[OK] Module found at ");
        Serial.print(BAUD_LIST[b]);
        Serial.println(" baud\n");
        return true;
      }
    }
  }
  return false;
}

void showStatus() {
  Serial.println("---- Module status ----");
  runAT("ATI",       2000);
  runAT("AT+CPIN?",  3000);   // READY = SIM OK
  runAT("AT+CSQ",    2000);   // 10-31 good, 99 = no signal
  runAT("AT+CEREG?", 2000);   // LTE:  ,1 home  ,5 roaming
  runAT("AT+CREG?",  2000);   // GSM:  ,1 home  ,5 roaming
  runAT("AT+COPS?",  5000);   // operator name
  runAT("AT+CSCA?",  3000);   // SMS center number (must not be empty)
  Serial.println("-----------------------");
}

bool initModem() {
  Serial.println("Looking for the module...");

  if (!findBaud()) {
    Serial.println("\n[!] Module not answering at any baud rate. Check:");
    Serial.println("    - module TX -> GPIO16 and module RX -> GPIO17");
    Serial.println("      (if it still fails, swap those two wires)");
    Serial.println("    - module GND connected to ESP32 GND");
    Serial.println("    - supply on VIN is 5 V or more and steady");
    Serial.println("    - solder joints on the header pins");
    Serial.println("  Fix it, then type: init");
    return false;
  }

  runAT("ATE0",            1000);   // echo off
  runAT("AT+CMGF=1",       2000);   // SMS text mode
  runAT("AT+CSCS=\"GSM\"", 2000);   // plain GSM character set

  // Deliver incoming SMS straight to the serial port (+CMT)
  if (runAT("AT+CNMI=2,2,0,0,0", 2000) != RES_OK) {
    Serial.println("[i] Falling back to CNMI=2,1 (store + notify)");
    runAT("AT+CNMI=2,1,0,0,0", 2000);
  }

  showStatus();
  Serial.println("Ready when CPIN is READY and CEREG/CREG ends in ,1 or ,5.");
  Serial.println("If not registered yet, wait 10-30 s and type: status\n");
  return true;
}

void printHelp() {
  Serial.println("Commands (Serial Monitor: 115200 baud, Newline):");
  Serial.println("  send <number> <message>  e.g. send +639171234567 Hello");
  Serial.println("  read                     list stored SMS");
  Serial.println("  del                      delete all stored SMS");
  Serial.println("  status                   SIM, signal, registration");
  Serial.println("  init                     re-detect module and re-run setup");
  Serial.println("  AT...                    raw AT command to the module");
  Serial.println("  help                     show this list");
  Serial.println("Incoming SMS appear automatically.\n");
}

// ======================================================================
//  Serial Monitor input
// ======================================================================

void handleCommand(char* s) {
  while (*s == ' ') s++;
  if (!*s) return;

  if (strcasecmp(s, "help") == 0 || strcmp(s, "?") == 0) {
    printHelp();
  } else if (strcasecmp(s, "status") == 0) {
    showStatus();
  } else if (strcasecmp(s, "init") == 0) {
    initModem();
  } else if (strcasecmp(s, "read") == 0) {
    runAT("AT+CMGL=\"ALL\"", 10000);
  } else if (strcasecmp(s, "del") == 0) {
    runAT("AT+CMGD=1,4", 10000);
  } else if (strncasecmp(s, "send ", 5) == 0) {
    cmdSend(s + 5);
  } else if (strncasecmp(s, "AT", 2) == 0) {
    runAT(s, 10000);
  } else {
    Serial.println("Unknown command. Type: help");
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
  delay(1500);

  Serial.println("\n=== A7670C SMS test (ESP32) ===");
  initModem();
  printHelp();
}

void loop() {
  pollUser();
  pollModem();
  delay(1);
}
