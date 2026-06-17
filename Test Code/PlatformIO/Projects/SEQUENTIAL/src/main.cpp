#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>

// ===== LCD + RTC =====
LiquidCrystal_I2C lcd(0x27, 20, 4);
RTC_DS3231 rtc;

// ===== UART to ESP32 #2 =====
HardwareSerial esp2(2); // UART2

// ===== BUTTONS =====
#define BTN_UP     0
#define BTN_DOWN   12
#define BTN_ENTER  13
#define BTN_BACK   15
#define BTN_MODE   2

// ===== OUTPUT =====
#define G4_PIN 4

// ===== STATES =====
enum State {
  STATE_IDLE,
  STATE_SELECT,
  STATE_CONFIRM,
  STATE_INIT,
  STATE_WAIT_DONE,
  STATE_FINISH,
  STATE_STOP_CONFIRM
};

State currentState = STATE_IDLE;

// ===== VARIABLES =====
bool sequenceUp = true;
unsigned long timer = 0;
bool processRunning = false;

// ===== FUNCTION DECLARATIONS =====
void handleButtons();
void updateLCD();
void handleESP2();

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_ENTER, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);

  pinMode(G4_PIN, OUTPUT);
  digitalWrite(G4_PIN, LOW);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();

  rtc.begin();

  esp2.begin(9600, SERIAL_8N1, 33, 25);

  lcd.clear();
}

// ================= LOOP =================
void loop() {
  handleButtons();
  handleESP2();
  updateLCD();
}

void handleButtons() {
  static bool lastEnter = HIGH, lastMode = HIGH, lastBack = HIGH;
  static bool lastUp = HIGH, lastDown = HIGH;

  bool enter = digitalRead(BTN_ENTER);
  bool mode  = digitalRead(BTN_MODE);
  bool back  = digitalRead(BTN_BACK);
  bool up    = digitalRead(BTN_UP);
  bool down  = digitalRead(BTN_DOWN);

  // MODE → enter selection
  if (lastMode == HIGH && mode == LOW && currentState == STATE_IDLE) {
    currentState = STATE_SELECT;
  }

  // UP selection
  if (lastUp == HIGH && up == LOW && currentState == STATE_SELECT) {
    sequenceUp = true;
  }

  // DOWN selection
  if (lastDown == HIGH && down == LOW && currentState == STATE_SELECT) {
    sequenceUp = false;
  }

  // ENTER logic
  if (lastEnter == HIGH && enter == LOW) {
    if (currentState == STATE_SELECT) {
      currentState = STATE_CONFIRM;
    }
    else if (currentState == STATE_CONFIRM) {
      currentState = STATE_INIT;
      timer = millis();
      processRunning = true;
    }
    else if (currentState == STATE_STOP_CONFIRM) {
      esp2.println("STOP"); // safe stop
      digitalWrite(G4_PIN, LOW);
      processRunning = false;
      currentState = STATE_IDLE;
    }
  }

  // BACK (interrupt)
  if (lastBack == HIGH && back == LOW && processRunning) {
    currentState = STATE_STOP_CONFIRM;
  }

  lastEnter = enter;
  lastMode  = mode;
  lastBack  = back;
  lastUp    = up;
  lastDown  = down;
}

void handleESP2() {
  static String buffer = "";

  while (esp2.available()) {
    char c = esp2.read();
    buffer += c;

    if (c == '\n') {
      buffer.trim();

      if (buffer == "DONE") {
        currentState = STATE_FINISH;
        timer = millis();
      }

      buffer = "";
    }
  }
}

void updateLCD() {
  static State lastState = STATE_IDLE;
  static unsigned long lastUpdate = 0;

  if (millis() - lastUpdate < 300) return;
  lastUpdate = millis();

  if (lastState != currentState) {
    lcd.clear();
    lastState = currentState;
  }

  switch (currentState) {

    case STATE_IDLE: {
      DateTime now = rtc.now();
      lcd.setCursor(0, 0);
      lcd.print("Time:");
      lcd.print(now.hour());
      lcd.print(":");
      lcd.print(now.minute());

      lcd.setCursor(0, 1);
      lcd.print("Date:");
      lcd.print(now.month());
      lcd.print("/");
      lcd.print(now.day());
    }
    break;

    case STATE_SELECT:
      lcd.setCursor(0, 0);
      lcd.print("Select Sequence:");

      lcd.setCursor(0, 1);
      lcd.print(sequenceUp ? "UP" : "DOWN");
    break;

    case STATE_CONFIRM:
      lcd.setCursor(0, 0);
      lcd.print("Initiate ");
      lcd.print(sequenceUp ? "UP" : "DOWN");
      lcd.print("?");

      lcd.setCursor(0, 1);
      lcd.print("Press ENTER");
    break;

    case STATE_INIT:
      lcd.setCursor(0, 0);
      lcd.print("Starting...");

      // Step 1: set G4 HIGH
      digitalWrite(G4_PIN, HIGH);

      // After 2 sec send sequence
      if (millis() - timer > 2000) {
        if (sequenceUp) {
          esp2.println("SEQ_UP");
        } else {
          esp2.println("SEQ_DOWN");
        }
        currentState = STATE_WAIT_DONE;
      }
    break;

    case STATE_WAIT_DONE:
      lcd.setCursor(0, 0);
      lcd.print("Running...");
      lcd.setCursor(0, 1);
      lcd.print("Waiting DONE...");
    break;

    case STATE_FINISH:
      lcd.setCursor(0, 0);
      lcd.print("Process Done");

      if (millis() - timer > 2000) {
        digitalWrite(G4_PIN, LOW);
        processRunning = false;
        currentState = STATE_IDLE;
      }
    break;

    case STATE_STOP_CONFIRM:
      lcd.setCursor(0, 0);
      lcd.print("STOP PROCESS?");
      lcd.setCursor(0, 1);
      lcd.print("Press ENTER");
    break;
  }
}