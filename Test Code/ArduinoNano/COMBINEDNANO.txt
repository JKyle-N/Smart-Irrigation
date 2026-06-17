#include <DHT.h>

// ================= PIN DEFINITIONS =================

// DHT22
#define DHTPIN 11
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// Ultrasonic
#define TRIG_RES 4
#define ECHO_RES 5
#define TRIG_MIX 6
#define ECHO_MIX 7

// Soil Sensors
#define COL_A_S1 A0
#define COL_A_S2 A1
#define COL_B_S1 A2
#define COL_B_S2 A3
#define COL_C_S1 A4
#define COL_C_S2 A5
#define PHOTO_PIN A6

// ================= CALIBRATION =================
const int SOIL_DRY = 800;
const int SOIL_WET = 300;

const int Lightness = 1020;
const int Darkness  = 249;

const float RES_MIN_M = 0.03;
const float RES_MAX_M = 0.53;
const float MIX_MIN_M = 0.04;
const float MIX_MAX_M = 0.50;

const unsigned long TIMEOUT_US = 30000;

// ================= FUNCTIONS =================
float readUltrasonicMeters(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, TIMEOUT_US);
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

// ================= SETUP =================
void setup() {
  Serial.begin(9600);
  delay(2000);

  pinMode(TRIG_RES, OUTPUT);
  pinMode(ECHO_RES, INPUT);
  pinMode(TRIG_MIX, OUTPUT);
  pinMode(ECHO_MIX, INPUT);

  dht.begin();

  Serial.println("=== FULL SYSTEM DEBUG STARTED ===");
}

// ================= LOOP =================
void loop() {
  delay(2000);

  // -------- DHT22 --------
  float humidity = dht.readHumidity();
  float temperature = dht.readTemperature();

  Serial.println("\n--- DHT22 ---");
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("DHT22 ERROR");
  } else {
    Serial.print("Temperature: ");
    Serial.print(temperature);
    Serial.println(" °C");

    Serial.print("Humidity: ");
    Serial.print(humidity);
    Serial.println(" %");
  }

  // -------- ULTRASONIC --------
  float resDist = readUltrasonicMeters(TRIG_RES, ECHO_RES);
  float mixDist = readUltrasonicMeters(TRIG_MIX, ECHO_MIX);

  float resPct = distanceToPercent(resDist, RES_MIN_M, RES_MAX_M);
  float mixPct = distanceToPercent(mixDist, MIX_MIN_M, MIX_MAX_M);

  Serial.println("\n--- WATER LEVEL ---");
  Serial.print("Reservoir Tank: ");
  Serial.print(resPct);
  Serial.println(" %");

  Serial.print("Mix Tank: ");
  Serial.print(mixPct);
  Serial.println(" %");

  // -------- SOIL + LIGHT --------
  int a1 = adcToMoisturePercent(analogRead(COL_A_S1));
  int a2 = adcToMoisturePercent(analogRead(COL_A_S2));
  int b1 = adcToMoisturePercent(analogRead(COL_B_S1));
  int b2 = adcToMoisturePercent(analogRead(COL_B_S2));
  int c1 = adcToMoisturePercent(analogRead(COL_C_S1));
  int c2 = adcToMoisturePercent(analogRead(COL_C_S2));

  int avgA = (a1 + a2) / 2;
  int avgB = (b1 + b2) / 2;
  int avgC = (c1 + c2) / 2;

  int photoVal = analogRead(PHOTO_PIN);
  int photoPct = map(photoVal, Lightness, Darkness, 100, 0);

  Serial.println("\n--- SOIL MOISTURE ---");
  Serial.print("Column A S1: "); Serial.print(a1); Serial.println(" %");
  Serial.print("Column A S2: "); Serial.print(a2); Serial.println(" %");
  Serial.print("Column A AVG: "); Serial.print(avgA); Serial.println(" %\n");

  Serial.print("Column B S1: "); Serial.print(b1); Serial.println(" %");
  Serial.print("Column B S2: "); Serial.print(b2); Serial.println(" %");
  Serial.print("Column B AVG: "); Serial.print(avgB); Serial.println(" %\n");

  Serial.print("Column C S1: "); Serial.print(c1); Serial.println(" %");
  Serial.print("Column C S2: "); Serial.print(c2); Serial.println(" %");
  Serial.print("Column C AVG: "); Serial.print(avgC); Serial.println(" %");

  Serial.println("\n--- LIGHT ---");
  Serial.print("Photoresistor: ");
  Serial.print(photoPct);
  Serial.println(" %");
}
