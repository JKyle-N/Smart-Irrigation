/* =============================================================================
 *  GSM + LCD BENCH DEBUG  (SIM800L on ESP32, spec sec.12 / sec.18.7 buttons)
 * -----------------------------------------------------------------------------
 *  Purpose: prove out SIM800L send + receive with VERBOSE serial diagnostics and
 *  on-LCD display of received SMS.
 *
 *  Send a test SMS:  press ENTER.
 *  Receive an SMS:   text the module -> sender + body shown on the LCD, full
 *                    decode printed to serial.
 *
 *  WHY SMS FAILS -- this build now reports it explicitly at boot and every 15 s:
 *    - SIM not READY            (AT+CPIN?)   -> SIM not seated / PIN locked
 *    - No / weak signal         (AT+CSQ)     -> antenna / location / power dip
 *    - Not registered to network(AT+CREG?)   -> can't send OR receive
 *    - Send rejected            (+CMS ERROR) -> decoded with the error number
 *  SIM800L needs a solid 3.7-4.2 V supply able to source ~2 A bursts; brownouts
 *  during transmit are the #1 cause of "no response / failed to send".
 * ========================================================================== */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>

// ===== LCD + RTC =====
LiquidCrystal_I2C lcd(0x27, 20, 4);
RTC_DS3231 rtc;

// ===== GSM =====
HardwareSerial sim800(1);
#define SIM_RX_PIN 26          // ESP32 RX  <- SIM800L TX
#define SIM_TX_PIN 27          // ESP32 TX  -> SIM800L RX
#define SIM_BAUD   9600

// ===== BUTTONS (match main ESP1 layout, spec sec.18.7) =====
#define BTN_UP    0
#define BTN_DOWN  12
#define BTN_ENTER 13
#define BTN_BACK  15
#define BTN_MODE  2
const uint8_t BTN_PINS[5]  = { BTN_UP, BTN_DOWN, BTN_ENTER, BTN_BACK, BTN_MODE };
const char *  BTN_NAMES[5] = { "UP", "DOWN", "ENTER", "BACK", "MODE" };

// ===== PHONE =====
String phoneNumber = "09150424784";   // <-- set to YOUR phone for the ENTER test send

// ===== STATES =====
enum State { STATE_DEFAULT, STATE_SENDING, STATE_RECEIVED };
State currentState = STATE_DEFAULT;

// ===== DATA =====
String receivedMsg  = "";
String receivedFrom = "";
unsigned long stateTimer = 0;

// ===== GSM health (refreshed by gsmHealthCheck) =====
int  lastRssi       = -1;       // raw AT+CSQ RSSI (0..31, 99 = unknown/no signal)
bool netRegistered  = false;    // AT+CREG stat == 1 (home) or 5 (roaming)
bool simReady       = false;    // AT+CPIN? == READY

// ===== FUNCTION DECLARATIONS =====
void handleButton();
void sendSMS(String msg);
void handleSIM800();
void updateLCD();
String sendAT(const String &cmd, unsigned long timeout, const char *label);
void gsmHealthCheck(bool verbose);
const char *cmsErrorText(int code);

// ================= SETUP =================
void setup() {
  pinMode(4, OUTPUT);
  Serial.begin(115200);

  for (int i = 0; i < 5; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("GSM debug booting...");

  rtc.begin();

  sim800.begin(SIM_BAUD, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  delay(5000);                  // network stabilize (module cold-start)

  Serial.println("\n=== GSM DEBUG SYSTEM READY ===");

  // ---- GSM init, each step checked ----
  String r = sendAT("AT", 1000, "AT(alive)");
  if (r.indexOf("OK") < 0)
    Serial.println("  !! No reply to AT -- check TX=27/RX=26 wiring, common GND, 9600 baud, module power.");

  sendAT("ATE0", 1000, "echo off");                 // quieter, easier to parse
  r = sendAT("AT+CMGF=1", 1000, "text mode");
  if (r.indexOf("OK") < 0) Serial.println("  !! AT+CMGF=1 failed (text mode not set).");
  sendAT("AT+CSCS=\"GSM\"", 1000, "charset");
  r = sendAT("AT+CNMI=2,2,0,0,0", 1000, "SMS push");
  if (r.indexOf("OK") < 0) Serial.println("  !! AT+CNMI failed -- incoming SMS will NOT be pushed (no +CMT).");
  sendAT("AT+CSMP=17,167,0,0", 1000, "SMS params");

  gsmHealthCheck(true);         // SIM / signal / registration summary

  digitalWrite(4, LOW);
}

// ================= LOOP =================
void loop() {
  handleSIM800();
  handleButton();
  updateLCD();

  // periodic GSM health refresh (so the LCD/serial show live signal + registration)
  static unsigned long lastHealth = 0;
  if (millis() - lastHealth >= 15000) {
    lastHealth = millis();
    gsmHealthCheck(true);
  }

  // button-state heartbeat once per second
  static unsigned long lastDbg = 0;
  if (millis() - lastDbg >= 1000) {
    lastDbg = millis();
    Serial.print("[DBG] ");
    for (int i = 0; i < 5; i++) {
      Serial.print(BTN_NAMES[i]); Serial.print("(");
      Serial.print(BTN_PINS[i]);  Serial.print(")=");
      Serial.print(digitalRead(BTN_PINS[i]) == LOW ? "LOW " : "HIGH ");
    }
    Serial.print(" state="); Serial.print(currentState);
    Serial.print(" net="); Serial.print(netRegistered ? "REG" : "--");
    Serial.print(" rssi="); Serial.println(lastRssi);
  }
}

// ----------------------------------------------------------------------------
//  AT helper: send a command, collect the whole response for `timeout` ms,
//  echo it to serial labeled, and return it (trimmed) for parsing.
// ----------------------------------------------------------------------------
String sendAT(const String &cmd, unsigned long timeout, const char *label) {
  while (sim800.available()) sim800.read();   // drop stale bytes
  sim800.println(cmd);

  String resp = "";
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (sim800.available()) resp += (char)sim800.read();
  }
  resp.trim();

  Serial.print("[GSM] ");
  Serial.print(label ? label : cmd.c_str());
  Serial.print(" -> ");
  if (resp.length() == 0) Serial.println("(NO RESPONSE)");
  else { String one = resp; one.replace("\r", " "); one.replace("\n", " | "); Serial.println(one); }
  return resp;
}

// ----------------------------------------------------------------------------
//  Query SIM / signal / network and update health flags (+ explain problems).
// ----------------------------------------------------------------------------
void gsmHealthCheck(bool verbose) {
  // SIM present / unlocked
  String r = sendAT("AT+CPIN?", 2000, "SIM status");
  simReady = (r.indexOf("READY") >= 0);
  if (verbose && !simReady)
    Serial.println("  !! SIM not READY -- reseat SIM, check holder contacts, or SIM is PIN-locked.");

  // Signal quality:  +CSQ: <rssi>,<ber>   rssi 0..31, 99 = no signal
  r = sendAT("AT+CSQ", 1000, "Signal");
  int idx = r.indexOf("+CSQ:");
  if (idx >= 0) {
    lastRssi = r.substring(idx + 5).toInt();
    if (verbose) {
      Serial.print("  Signal RSSI="); Serial.print(lastRssi);
      if (lastRssi == 99 || lastRssi < 0) Serial.println(" -> NO SIGNAL (antenna/location/power).");
      else {
        Serial.print(" (~"); Serial.print(-113 + 2 * lastRssi); Serial.print(" dBm) ");
        Serial.println(lastRssi < 8 ? "-> WEAK, SMS may fail." : "-> ok.");
      }
    }
  } else if (verbose) {
    Serial.println("  !! No +CSQ response.");
  }

  // Network registration:  +CREG: <n>,<stat>   stat 1=home, 5=roaming (registered)
  r = sendAT("AT+CREG?", 1000, "NetReg");
  idx = r.indexOf("+CREG:");
  int stat = -1;
  if (idx >= 0) {
    String s = r.substring(idx + 6); s.trim();
    int comma = s.indexOf(',');
    if (comma >= 0) stat = s.substring(comma + 1).toInt();
  }
  netRegistered = (stat == 1 || stat == 5);
  if (verbose) {
    Serial.print("  Network: ");
    switch (stat) {
      case 0:  Serial.println("NOT registered, not searching."); break;
      case 1:  Serial.println("registered (home)."); break;
      case 2:  Serial.println("searching for network..."); break;
      case 3:  Serial.println("registration DENIED (SIM/account issue)."); break;
      case 5:  Serial.println("registered (roaming)."); break;
      default: Serial.println("unknown."); break;
    }
    if (!netRegistered)
      Serial.println("  !! Not registered -- you can NOT send or receive until this clears.");
  }
}

// Decode the common SIM800L +CMS ERROR numbers seen during SMS sends.
const char *cmsErrorText(int code) {
  switch (code) {
    case 30:  return "unknown subscriber";
    case 38:  return "network out of order";
    case 41:  return "temporary failure";
    case 50:  return "requested facility not subscribed";
    case 69:  return "requested facility not implemented";
    case 193: return "no SMSC number set (AT+CSCA)";
    case 304: return "invalid PDU mode parameter";
    case 305: return "invalid text mode parameter";
    case 311: return "SIM PIN required";
    case 313: return "SIM failure";
    case 322: return "memory full";
    case 330: return "SMSC address unknown";
    case 500: return "unknown error (often: no signal / not registered)";
    case 512: return "send fail (network/SMSC)";
    default:  return "see SIM800L AT command manual";
  }
}

// ----------------------------------------------------------------------------
//  SEND  (ENTER button)
// ----------------------------------------------------------------------------
void sendSMS(String msg) {
  if (!netRegistered)
    Serial.println("[SEND] WARNING: not registered to network -- send will likely fail.");

  while (sim800.available()) sim800.read();

  sim800.println("AT+CMGF=1");
  delay(500);

  sim800.print("AT+CMGS=\"");
  sim800.print(phoneNumber);
  sim800.println("\"");

  // WAIT for '>' prompt, capturing everything for diagnostics
  unsigned long start = millis();
  String pre = "";
  bool gotPrompt = false;
  while (millis() - start < 7000) {
    if (sim800.available()) {
      char c = sim800.read();
      pre += c;
      if (c == '>') { gotPrompt = true; break; }
    }
  }

  if (!gotPrompt) {
    Serial.println("\n[SEND] ERROR: no '>' prompt from module.");
    String one = pre; one.replace("\r", " "); one.replace("\n", " | ");
    Serial.print("[SEND] module said: "); Serial.println(one.length() ? one : "(nothing)");
    if (pre.indexOf("ERROR") >= 0) Serial.println("  -> module rejected AT+CMGS (check text mode / SIM).");
    else Serial.println("  -> no reply: check power (brownout on TX), wiring, baud.");
    lcd.setCursor(0, 1); lcd.print("Send FAIL: no prompt");
    currentState = STATE_DEFAULT;
    return;
  }

  sim800.print(msg);
  delay(200);
  sim800.write(26);             // CTRL+Z -> send

  // capture the final result and classify it
  String resp = "";
  start = millis();
  while (millis() - start < 12000) {
    while (sim800.available()) resp += (char)sim800.read();
    if (resp.indexOf("+CMGS:") >= 0 || resp.indexOf("ERROR") >= 0) break;  // got a verdict
  }
  String one = resp; one.replace("\r", " "); one.replace("\n", " | ");
  Serial.print("\n[SEND] result: "); Serial.println(one.length() ? one : "(no response/timeout)");

  if (resp.indexOf("+CMGS:") >= 0) {
    Serial.println("[SEND] OK -- message accepted by network.");
    lcd.setCursor(0, 1); lcd.print("Sent OK            ");
  } else {
    int e = resp.indexOf("+CMS ERROR:");
    if (e >= 0) {
      int code = resp.substring(e + 11).toInt();
      Serial.print("[SEND] FAILED -- CMS ERROR "); Serial.print(code);
      Serial.print(" ("); Serial.print(cmsErrorText(code)); Serial.println(").");
      lcd.setCursor(0, 1); lcd.print("Fail CMS " + String(code) + "        ");
    } else if (resp.indexOf("+CME ERROR:") >= 0) {
      int c2 = resp.indexOf("+CME ERROR:");
      Serial.print("[SEND] FAILED -- CME ERROR "); Serial.println(resp.substring(c2 + 11).toInt());
      lcd.setCursor(0, 1); lcd.print("Fail CME           ");
    } else {
      Serial.println("[SEND] FAILED -- no confirmation (timeout). Likely no signal / brownout.");
      lcd.setCursor(0, 1); lcd.print("Send FAIL: timeout ");
    }
  }
  currentState = STATE_DEFAULT;
}

// ----------------------------------------------------------------------------
//  RECEIVE  --  parse +CMT push, extract sender + body, show on LCD
//    +CMT: "+639...","","26/06/17,10:00:00+32"
//    <message body>
// ----------------------------------------------------------------------------
void handleSIM800() {
  static String line = "";
  static bool   waitingForBody = false;

  while (sim800.available()) {
    char c = sim800.read();

    if (c == '\n') {
      line.trim();

      if (line.startsWith("+CMT:")) {
        // pull the sender number out of the first quoted field
        int q1 = line.indexOf('"');
        int q2 = (q1 >= 0) ? line.indexOf('"', q1 + 1) : -1;
        receivedFrom = (q1 >= 0 && q2 > q1) ? line.substring(q1 + 1, q2) : "(unknown)";
        waitingForBody = true;
      }
      else if (waitingForBody && line.length() > 0) {
        receivedMsg = line;
        waitingForBody = false;

        Serial.println("\n===== SMS RECEIVED =====");
        Serial.print("From: "); Serial.println(receivedFrom);
        Serial.print("Msg : "); Serial.println(receivedMsg);
        Serial.println("========================");

        currentState = STATE_RECEIVED;
        stateTimer = millis();
      }

      line = "";
    }
    else if (c != '\r') {
      line += c;
      if (line.length() > 250) line = "";   // guard runaway
    }
  }
}

// ----------------------------------------------------------------------------
//  BUTTONS  (ENTER sends the test SMS)
// ----------------------------------------------------------------------------
void handleButton() {
  static bool lastState[5]           = { HIGH, HIGH, HIGH, HIGH, HIGH };
  static unsigned long lastChange[5] = { 0, 0, 0, 0, 0 };

  for (int i = 0; i < 5; i++) {
    bool current = digitalRead(BTN_PINS[i]);
    if (current != lastState[i] && millis() - lastChange[i] > 40) {
      lastChange[i] = millis();
      lastState[i]  = current;

      if (current == LOW) {                       // press
        Serial.print("\n[BTN] "); Serial.print(BTN_NAMES[i]); Serial.println(" Pressed");
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("Btn: "); lcd.print(BTN_NAMES[i]);

        if (BTN_PINS[i] == BTN_ENTER) {
          lcd.setCursor(0, 1); lcd.print("Sending SMS...");
          currentState = STATE_SENDING;
          stateTimer = millis();
          sendSMS("Hello from Irrigation System");
        }
      } else {
        Serial.print("[BTN] "); Serial.print(BTN_NAMES[i]); Serial.println(" Released");
      }
    }
  }
}

// ----------------------------------------------------------------------------
//  LCD
// ----------------------------------------------------------------------------
void updateLCD() {
  static State lastState = STATE_DEFAULT;
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate < 500) return;
  lastUpdate = millis();

  if (lastState != currentState) { lcd.clear(); lastState = currentState; }

  if (currentState == STATE_DEFAULT) {
    DateTime now = rtc.now();
    char l[21];
    snprintf(l, 21, "Time %02d:%02d  %02d/%02d   ", now.hour(), now.minute(), now.month(), now.day());
    lcd.setCursor(0, 0); lcd.print(l);

    // live GSM health so you can SEE why receive/send may fail
    snprintf(l, 21, "Net:%-4s Sig:%-3s   ",
             netRegistered ? "REG" : "NO",
             (lastRssi < 0 || lastRssi == 99) ? "--" : String(lastRssi).c_str());
    lcd.setCursor(0, 1); lcd.print(l);
    lcd.setCursor(0, 2); lcd.print(simReady ? "SIM ready         " : "SIM NOT ready!    ");
    lcd.setCursor(0, 3); lcd.print("ENTER=send test SMS ");
  }

  else if (currentState == STATE_SENDING) {
    lcd.setCursor(0, 0); lcd.print("Sending SMS...      ");
  }

  else if (currentState == STATE_RECEIVED) {
    lcd.setCursor(0, 0); lcd.print("SMS from:           ");
    lcd.setCursor(0, 1); lcd.print((receivedFrom + "            ").substring(0, 20));
    // message body across the bottom two rows (up to 40 chars)
    lcd.setCursor(0, 2); lcd.print((receivedMsg + "                    ").substring(0, 20));
    lcd.setCursor(0, 3);
    if (receivedMsg.length() > 20) lcd.print((receivedMsg.substring(20) + "                    ").substring(0, 20));
    else                           lcd.print("                    ");

    if (millis() - stateTimer > 60000) currentState = STATE_DEFAULT;  // auto-return
  }
}
