#include <Arduino.h>
#include <Wire.h>


void setPCF();
void activateChannel();
void stopAll();

// ===== UART from ESP32 #1 =====
HardwareSerial esp1(2);

// ===== PCF8575 =====
#define PCF_ADDR 0x20

uint16_t pcfState = 0xFFFF; // all OFF (active LOW)

// ===== STATES =====
enum State {
  STATE_IDLE,
  STATE_RUNNING,
  STATE_STOPPING
};

State currentState = STATE_IDLE;

// ===== SEQUENCE =====
int currentIndex = 0;
bool sequenceUp = true;

unsigned long stepTimer = 0;
const int stepDelay = 1000; // 1 sec per step

// ===== FUNCTION DECLARATIONS =====
void handleSerial();
void updateSequence();
void setPCF(uint16_t state);
void activateChannel(int ch);
void stopAll();

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);

  esp1.begin(9600, SERIAL_8N1, 16, 17);

  setPCF(0xFFFF); // all OFF
}

// ================= LOOP =================
void loop() {
  handleSerial();
  updateSequence();
}

void handleSerial() {
  static String buffer = "";

  while (esp1.available()) {
    char c = esp1.read();

    if (c == '\n') {
      buffer.trim();

      Serial.println("CMD: " + buffer);

      if (buffer == "SEQ_UP") {
        sequenceUp = true;
        currentIndex = 0;
        currentState = STATE_RUNNING;
        stepTimer = millis();
      }
      else if (buffer == "SEQ_DOWN") {
        sequenceUp = false;
        currentIndex = 7;
        currentState = STATE_RUNNING;
        stepTimer = millis();
      }
      else if (buffer == "STOP") {
        currentState = STATE_STOPPING;
      }

      buffer = "";
    }
    else if (c != '\r') {
      buffer += c;
    }
  }
}

void updateSequence() {

  if (currentState == STATE_RUNNING) {

    if (millis() - stepTimer >= stepDelay) {
      stepTimer = millis();

      activateChannel(currentIndex);

      Serial.print("Step: ");
      Serial.println(currentIndex);

      // Move index
      if (sequenceUp) {
        currentIndex++;
        if (currentIndex > 7) {
          stopAll();
          esp1.println("DONE");
          currentState = STATE_IDLE;
        }
      } else {
        currentIndex--;
        if (currentIndex < 0) {
          stopAll();
          esp1.println("DONE");
          currentState = STATE_IDLE;
        }
      }
    }
  }

  else if (currentState == STATE_STOPPING) {
    Serial.println("Stopping safely...");

    stopAll();

    esp1.println("DONE");

    currentState = STATE_IDLE;
  }
}


void setPCF(uint16_t state) {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(lowByte(state));
  Wire.write(highByte(state));
  Wire.endTransmission();

  pcfState = state;
}

void activateChannel(int ch) {

  uint16_t state = 0xFFFF; // all OFF

  // Active LOW → turn ON selected channel
  state &= ~(1 << ch);

  setPCF(state);
}

void stopAll() {
  setPCF(0xFFFF); // all OFF
}