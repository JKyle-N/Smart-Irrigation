#include <SoftwareSerial.h>

#define RE 8
#define DE 7

// Use the SAME pins that are known working for RS485
SoftwareSerial mod(2, 3);   // RO -> D2, DI -> D3

const byte querySensor2[8] = {
  0x02, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x3B
};

byte response[19];

void setup() {
  Serial.begin(9600);
  mod.begin(4800);

  pinMode(RE, OUTPUT);
  pinMode(DE, OUTPUT);

  digitalWrite(RE, LOW);
  digitalWrite(DE, LOW);

  Serial.println("=== SENSOR 2 TEST ===");
  Serial.println("Place sensor in AIR first");
  delay(2000);
}

void loop() {

  // ----- SEND QUERY -----
  digitalWrite(DE, HIGH);
  digitalWrite(RE, HIGH);
  delay(10);

  mod.write(querySensor2, 8);
  mod.flush();

  digitalWrite(DE, LOW);
  digitalWrite(RE, LOW);

  delay(800);

  // ----- READ RESPONSE -----
  if (mod.available() >= 19) {
    mod.readBytes(response, 19);

    // RAW DATA
    Serial.print("RAW: ");
    for (int i = 0; i < 19; i++) {
      if (response[i] < 0x10) Serial.print("0");
      Serial.print(response[i], HEX);
      Serial.print(" ");
    }
    Serial.println();

    // PARSED DATA
    float moisture = ((response[3] << 8) | response[4]) / 10.0;
    float temp     = ((response[5] << 8) | response[6]) / 10.0;
    uint16_t ec    = (response[7] << 8) | response[8];
    float ph       = ((response[9] << 8) | response[10]) / 10.0;
    uint16_t n     = (response[11] << 8) | response[12];
    uint16_t p     = (response[13] << 8) | response[14];
    uint16_t k     = (response[15] << 8) | response[16];

    Serial.println("--- Parsed Values ---");
    Serial.print("Moisture (%): "); Serial.println(moisture);
    Serial.print("Temperature: "); Serial.print(temp); Serial.println(" C");
    Serial.print("EC (uS/cm): "); Serial.println(ec);
    Serial.print("pH: "); Serial.println(ph);
    Serial.print("Nitrogen: "); Serial.println(n);
    Serial.print("Phosphorus: "); Serial.println(p);
    Serial.print("Potassium: "); Serial.println(k);
    Serial.println("---------------------\n");
  }
  else {
    Serial.println("No response from Sensor 2");
  }

  delay(3000);
}
