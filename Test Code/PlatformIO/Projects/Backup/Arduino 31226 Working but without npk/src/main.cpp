/*
=========================================
ARDUINO NANO SENSOR HUB
Smart Irrigation Thesis Project
Author: Rara Maurice
=========================================
Sends sensor data to ESP32 Master via UART
Baud Rate: 115200
=========================================
*/

#include <DHT.h>

#define DHTPIN 11
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);

// Soil Sensors
#define SOIL_A1 A0
#define SOIL_A2 A1
#define SOIL_B1 A2
#define SOIL_B2 A3
#define SOIL_C1 A4
#define SOIL_C2 A5

// Light Sensor
#define LIGHT_SENSOR A6

// Ultrasonic Reservoir
#define RES_TRIG 4
#define RES_ECHO 5

// Ultrasonic Mixing Tank
#define MIX_TRIG 6
#define MIX_ECHO 7

// Flow sensor (optional)
#define FLOW_PIN 2

// Status LED
#define LED 13

// Calibration values for capacitive sensors
#define AIR_VALUE 520
#define WATER_VALUE 260

unsigned long lastSend = 0;
unsigned long interval = 3000;

float reservoirHeight = 30.0;   // cm tank height
float mixingHeight = 25.0;

void setup()
{
  Serial.begin(115200);

  pinMode(RES_TRIG, OUTPUT);
  pinMode(RES_ECHO, INPUT);

  pinMode(MIX_TRIG, OUTPUT);
  pinMode(MIX_ECHO, INPUT);

  pinMode(LED, OUTPUT);

  dht.begin();
}

float readUltrasonic(int trigPin, int echoPin)
{
  digitalWrite(trigPin, LOW);
  delayMicroseconds(5);

  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH);

  float distance = duration * 0.034 / 2;

  return distance;
}

int soilPercent(int raw)
{
  int percent = map(raw, AIR_VALUE, WATER_VALUE, 0, 100);

  if (percent > 100) percent = 100;
  if (percent < 0) percent = 0;

  return percent;
}

void loop()
{
  if (millis() - lastSend > interval)
  {
    lastSend = millis();

    digitalWrite(LED, HIGH);

    // ===== DHT22 =====
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();

    // ===== Soil Sensors =====
    int soilA = (soilPercent(analogRead(SOIL_A1)) +
                 soilPercent(analogRead(SOIL_A2))) / 2;

    int soilB = (soilPercent(analogRead(SOIL_B1)) +
                 soilPercent(analogRead(SOIL_B2))) / 2;

    int soilC = (soilPercent(analogRead(SOIL_C1)) +
                 soilPercent(analogRead(SOIL_C2))) / 2;

    // ===== Light Sensor =====
    int lightRaw = analogRead(LIGHT_SENSOR);
    int lightPercent = map(lightRaw, 0, 1023, 0, 100);

    // ===== Tank Levels =====
    float resDistance = readUltrasonic(RES_TRIG, RES_ECHO);
    float mixDistance = readUltrasonic(MIX_TRIG, MIX_ECHO);

    float reservoirLevel = 100 - ((resDistance / reservoirHeight) * 100);
    float mixingLevel = 100 - ((mixDistance / mixingHeight) * 100);

    if (reservoirLevel > 100) reservoirLevel = 100;
    if (reservoirLevel < 0) reservoirLevel = 0;

    if (mixingLevel > 100) mixingLevel = 100;
    if (mixingLevel < 0) mixingLevel = 0;

    // ===== SEND DATA =====

    Serial.print("ENV,");
    Serial.print(temperature);
    Serial.print(",");
    Serial.println(humidity);

    Serial.print("SOIL,");
    Serial.print(soilA);
    Serial.print(",");
    Serial.print(soilB);
    Serial.print(",");
    Serial.println(soilC);

    Serial.print("LIGHT,");
    Serial.println(lightPercent);

    Serial.print("TANK,");
    Serial.print(reservoirLevel);
    Serial.print(",");
    Serial.println(mixingLevel);

    digitalWrite(LED, LOW);
  }
}