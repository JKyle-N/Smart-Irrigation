/************************************************************
 * ARDUINO NANO – SENSOR HUB (FINAL)
 * Reads:
 *  - DHT22 (Air Temp & Humidity)
 *  - 2× Ultrasonic (Reservoir & Mixing Tank)
 *  - 6× Capacitive Soil Sensors (2 per column, averaged)
 *  - Photoresistor (Day / Cloudy / Night)
 *  - 3× RS485 NPK Sensors (Moisture, Temp, EC, pH, N, P, K)
 *
 * Sends:
 *  - Clean, processed data via UART to ESP32 #1
 *
 * NO actuators, NO decisions
 ************************************************************/

#include <DHT.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <BH1750.h>

/******************** PIN DEFINITIONS ********************/

// -------- DHT22 --------
#define DHTPIN 11
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// -------- Ultrasonic --------
#define TRIG_RES 4
#define ECHO_RES 5
#define TRIG_MIX 6
#define ECHO_MIX 7

// -------- Soil Sensors --------
// NOTE: COL_C moved to A6/A7 so the BH1750 can use I2C (A4=SDA, A5=SCL).
#define COL_A_S1 A0
#define COL_A_S2 A1
#define COL_B_S1 A2
#define COL_B_S2 A3
#define COL_C_S1 A6
#define COL_C_S2 A7

// -------- Light Sensor (BH1750, I2C @ 0x23) --------
BH1750 lightMeter;

// -------- Flow Sensor --------
#define FLOW_PIN 2   // D2 (INT0)
const float PULSES_PER_LITER = 450.0;
volatile unsigned long pulseCount = 0;
float totalLiters = 0.0;

void pulseCounter() {
  pulseCount++;
}

// -------- RS485 (NPK Sensors) --------
#define RS485_DE_RE 8
#define RS485_RX    9
#define RS485_TX    10
SoftwareSerial rs485(RS485_RX, RS485_TX);

/******************** CALIBRATION ********************/

// Soil ADC calibration
const int SOIL_DRY = 800;
const int SOIL_WET = 300;

// Ultrasonic calibration (meters)
const float RES_MIN_M = 0.03;
const float RES_MAX_M = 0.53;
const float MIX_MIN_M = 0.04;
const float MIX_MAX_M = 0.50;

const unsigned long US_TIMEOUT = 30000;

// Timing
const unsigned long SENSOR_INTERVAL = 2000;
const unsigned long NPK_INTERVAL    = 5000;

/******************** NPK STRUCT ********************/

struct NPKData {
  bool valid;
  float moisture;
  float temperature;
  uint16_t ec;
  float ph;
  uint16_t nitrogen;
  uint16_t phosphorus;
  uint16_t potassium;
};

NPKData npk[3];
byte npkResponse[19];

// Modbus query frames
const byte NPK_QUERIES[3][8] = {

  // NPK 1 (old address 3)
  {0x03, 0x03, 0x00, 0x00, 0x00, 0x07, 0x05, 0xEA},

  // NPK 2 (old address 1)
  {0x01, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x08},

  // NPK 3 (old address 2)
  {0x02, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x3B}
};

/******************** UTILITY FUNCTIONS ********************/

float readUltrasonicMeters(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, US_TIMEOUT);
  if (duration == 0) return -1.0;

  return (duration * 0.000001 * 343.0) / 2.0;
}

float distanceToPercent(float dist, float minM, float maxM) {
  if (dist < 0) return -1.0;
  float pct = (maxM - dist) / (maxM - minM) * 100.0;
  return constrain(pct, 0.0, 100.0);
}

int adcToMoisturePercent(int adc) {
  adc = constrain(adc, SOIL_WET, SOIL_DRY);
  return map(adc, SOIL_DRY, SOIL_WET, 0, 100);
}

int readSoilAverage(uint8_t p1, uint8_t p2) {
  int s1 = adcToMoisturePercent(analogRead(p1));
  int s2 = adcToMoisturePercent(analogRead(p2));
  if (abs(s1 - s2) > 30) return max(s1, s2);
  return (s1 + s2) / 2;
}

inline void rs485TX() { digitalWrite(RS485_DE_RE, HIGH); }
inline void rs485RX() { digitalWrite(RS485_DE_RE, LOW); }

/******************** NPK READ FUNCTION ********************/

bool readNPK(uint8_t index) {
  if (index > 2) return false;

  rs485TX();
  delay(5);
  rs485.write(NPK_QUERIES[index], 8);
  rs485.flush();
  rs485RX();

  unsigned long start = millis();
  while (rs485.available() < 19) {
    if (millis() - start > 1000) {
      npk[index].valid = false;
      return false;
    }
  }

  rs485.readBytes(npkResponse, 19);

  npk[index].moisture    = ((npkResponse[3] << 8) | npkResponse[4]) / 10.0;
  npk[index].temperature = ((npkResponse[5] << 8) | npkResponse[6]) / 10.0;
  npk[index].ec          = (npkResponse[7] << 8) | npkResponse[8];
  npk[index].ph          = ((npkResponse[9] << 8) | npkResponse[10]) / 10.0;
  npk[index].nitrogen    = (npkResponse[11] << 8) | npkResponse[12];
  npk[index].phosphorus  = (npkResponse[13] << 8) | npkResponse[14];
  npk[index].potassium   = (npkResponse[15] << 8) | npkResponse[16];

  npk[index].valid = true;
  return true;
}

/******************** SETUP ********************/

void setup() {
  Serial.begin(9600);

  pinMode(TRIG_RES, OUTPUT);
  pinMode(ECHO_RES, INPUT);
  pinMode(TRIG_MIX, OUTPUT);
  pinMode(ECHO_MIX, INPUT);

  pinMode(RS485_DE_RE, OUTPUT);
  rs485RX();
  rs485.begin(4800);

  dht.begin();

  Wire.begin();
  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23)) {
    Serial.println(F("BH1750 init failed!"));
  }

  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, RISING);

  Serial.println(F("\n=== ARDUINO NANO SENSOR HUB READY ==="));
}

/******************** LOOP ********************/

void loop() {
  static unsigned long lastSensor = 0;
  static unsigned long lastNPK = 0;
  static uint8_t npkIndex = 0;

  // -------- General Sensors --------
  if (millis() - lastSensor >= SENSOR_INTERVAL) {
    lastSensor = millis();

    float temp = dht.readTemperature();
    float hum  = dht.readHumidity();

    float resPct = distanceToPercent(
      readUltrasonicMeters(TRIG_RES, ECHO_RES),
      RES_MIN_M, RES_MAX_M
    );

    float mixPct = distanceToPercent(
      readUltrasonicMeters(TRIG_MIX, ECHO_MIX),
      MIX_MIN_M, MIX_MAX_M
    );

    int soilA = readSoilAverage(COL_A_S1, COL_A_S2);
    int soilB = readSoilAverage(COL_B_S1, COL_B_S2);
    int soilC = readSoilAverage(COL_C_S1, COL_C_S2);

    float lux = lightMeter.readLightLevel();

    // -------- Flow Sensor (atomic read) --------
    noInterrupts();
    unsigned long pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    float litersThisWindow = pulses / PULSES_PER_LITER;
    // SENSOR_INTERVAL is in ms; flow rate in L/min
    float flowRate = litersThisWindow * (60000.0 / SENSOR_INTERVAL);
    totalLiters += litersThisWindow;

    Serial.println(F("\n--- ENV ---"));
    Serial.print(F("T:")); Serial.print(temp);
    Serial.print(F(" H:")); Serial.println(hum);

    Serial.println(F("--- TANK ---"));
    Serial.print(F("RES:")); Serial.print(resPct);
    Serial.print(F(" MIX:")); Serial.println(mixPct);

    Serial.println(F("--- SOIL ---"));
    Serial.print(F("A:")); Serial.print(soilA);
    Serial.print(F(" B:")); Serial.print(soilB);
    Serial.print(F(" C:")); Serial.println(soilC);

    Serial.print(F("LIGHT:")); Serial.print(lux); Serial.println(F(" lx"));

    Serial.println(F("--- FLOW ---"));
    Serial.print(F("RATE:")); Serial.print(flowRate); Serial.print(F(" L/min"));
    Serial.print(F(" TOTAL:")); Serial.print(totalLiters, 3); Serial.println(F(" L"));
  }

  // -------- NPK Sensors --------
  if (millis() - lastNPK >= NPK_INTERVAL) {
    lastNPK = millis();

    if (readNPK(npkIndex)) {
      Serial.print(F("\nNPK "));
      Serial.println(npkIndex + 1);

      Serial.print(F("M:")); Serial.print(npk[npkIndex].moisture);
      Serial.print(F(" T:")); Serial.print(npk[npkIndex].temperature);
      Serial.print(F(" EC:")); Serial.print(npk[npkIndex].ec);
      Serial.print(F(" pH:")); Serial.println(npk[npkIndex].ph);

      Serial.print(F("N:")); Serial.print(npk[npkIndex].nitrogen);
      Serial.print(F(" P:")); Serial.print(npk[npkIndex].phosphorus);
      Serial.print(F(" K:")); Serial.println(npk[npkIndex].potassium);
    } else {
      Serial.print(F("\nNPK "));
      Serial.print(npkIndex + 1);
      Serial.println(F(" ERROR"));
    }

    npkIndex = (npkIndex + 1) % 3;
  }
}
