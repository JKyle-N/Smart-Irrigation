#include <DHT.h>

// =================================================
// PIN DEFINITIONS
// =================================================

// DHT22
#define DHTPIN     11
#define DHTTYPE    DHT22
DHT dht(DHTPIN, DHTTYPE);

// Ultrasonic Sensors
#define TRIG_RES   4
#define ECHO_RES   5
#define TRIG_MIX   6
#define ECHO_MIX   7

// Soil Sensors
#define COL_A_S1   A0
#define COL_A_S2   A1
#define COL_B_S1   A2
#define COL_B_S2   A3
#define COL_C_S1   A4
#define COL_C_S2   A5

// Light Sensor
#define PHOTO_PIN  A6

// =================================================
// CALIBRATION CONSTANTS
// =================================================

// Soil ADC calibration
const int SOIL_DRY = 800;
const int SOIL_WET = 300;

// Photoresistor calibration
const int LIGHT_MAX = 1020;   // Bright
const int LIGHT_MIN = 250;    // Dark

// Ultrasonic calibration (meters)
const float RES_MIN_M = 0.03;
const float RES_MAX_M = 0.53;
const float MIX_MIN_M = 0.04;
const float MIX_MAX_M = 0.50;

// Ultrasonic timeout
const unsigned long US_TIMEOUT = 30000;

// Timing
const unsigned long SAMPLE_INTERVAL = 2000;
unsigned long lastSample = 0;

// =================================================
// UTILITY FUNCTIONS
// =================================================

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

float distanceToPercent(float distance, float minM, float maxM) {
  if (distance < 0) return -1.0;
  float pct = (maxM - distance) / (maxM - minM) * 100.0;
  return constrain(pct, 0.0, 100.0);
}

int adcToMoisturePercent(int adc) {
  adc = constrain(adc, SOIL_WET, SOIL_DRY);
  return map(adc, SOIL_DRY, SOIL_WET, 0, 100);
}

int readSoilAvg(uint8_t pin1, uint8_t pin2) {
  int s1 = adcToMoisturePercent(analogRead(pin1));
  int s2 = adcToMoisturePercent(analogRead(pin2));

  // Sanity check: discard outliers
  if (abs(s1 - s2) > 30) {
    return max(s1, s2);   // safer for irrigation logic
  }
  return (s1 + s2) / 2;
}

// =================================================
// SETUP
// =================================================

void setup() {
  Serial.begin(9600);
  delay(1500);

  pinMode(TRIG_RES, OUTPUT);
  pinMode(ECHO_RES, INPUT);
  pinMode(TRIG_MIX, OUTPUT);
  pinMode(ECHO_MIX, INPUT);

  dht.begin();

  Serial.println(F("\n=== ARDUINO NANO SENSOR HUB STARTED ==="));
}

// =================================================
// LOOP
// =================================================

void loop() {
  if (millis() - lastSample < SAMPLE_INTERVAL) return;
  lastSample = millis();

  // ---------------- DHT22 ----------------
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  // ---------------- Ultrasonic ----------------
  float resDist = readUltrasonicMeters(TRIG_RES, ECHO_RES);
  float mixDist = readUltrasonicMeters(TRIG_MIX, ECHO_MIX);

  float resPct = distanceToPercent(resDist, RES_MIN_M, RES_MAX_M);
  float mixPct = distanceToPercent(mixDist, MIX_MIN_M, MIX_MAX_M);

  // ---------------- Soil ----------------
  int soilA = readSoilAvg(COL_A_S1, COL_A_S2);
  int soilB = readSoilAvg(COL_B_S1, COL_B_S2);
  int soilC = readSoilAvg(COL_C_S1, COL_C_S2);

  // ---------------- Light ----------------
  int photoRaw = analogRead(PHOTO_PIN);
  photoRaw = constrain(photoRaw, LIGHT_MIN, LIGHT_MAX);
  int lightPct = map(photoRaw, LIGHT_MAX, LIGHT_MIN, 100, 0);

  // =================================================
  // OUTPUT (DEBUG FORMAT)
  // =================================================

  Serial.println(F("\n--- ENVIRONMENT ---"));
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println(F("DHT22 ERROR"));
  } else {
    Serial.print(F("Temp: ")); Serial.print(temperature, 1); Serial.println(F(" C"));
    Serial.print(F("Hum : ")); Serial.print(humidity, 1); Serial.println(F(" %"));
  }

  Serial.println(F("\n--- TANK LEVELS ---"));
  Serial.print(F("Reservoir: ")); Serial.print(resPct, 1); Serial.println(F(" %"));
  Serial.print(F("Mix Tank : ")); Serial.print(mixPct, 1); Serial.println(F(" %"));

  Serial.println(F("\n--- SOIL MOISTURE ---"));
  Serial.print(F("Column A: ")); Serial.print(soilA); Serial.println(F(" %"));
  Serial.print(F("Column B: ")); Serial.print(soilB); Serial.println(F(" %"));
  Serial.print(F("Column C: ")); Serial.print(soilC); Serial.println(F(" %"));

  Serial.println(F("\n--- LIGHT ---"));
  Serial.print(F("Light Level: "));
  Serial.print(lightPct);
  Serial.println(F(" %"));
}
