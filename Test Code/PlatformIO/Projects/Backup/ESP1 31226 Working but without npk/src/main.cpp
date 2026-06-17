
#include <Arduino.h>

/*
=========================================
ESP32 MASTER SENSOR PARSER
Smart Irrigation Thesis Project
Author: Rara Maurice
=========================================
Receives sensor packets from Arduino Nano
and converts them to usable variables
=========================================
*/

#define RXD2 16
#define TXD2 17

String incomingData = "";

/* ===== SENSOR VARIABLES ===== */

float temperature = 0;
float humidity = 0;

int soilA = 0;
int soilB = 0;
int soilC = 0;

int lightLevel = 0;

float reservoirLevel = 0;
float mixingLevel = 0;


/* ===== PACKET PARSER ===== */

void parsePacket(String packet)
{
  packet.trim();

  if (packet.startsWith("ENV"))
  {
    sscanf(packet.c_str(), "ENV,%f,%f", &temperature, &humidity);
  }

  else if (packet.startsWith("SOIL"))
  {
    sscanf(packet.c_str(), "SOIL,%d,%d,%d", &soilA, &soilB, &soilC);
  }

  else if (packet.startsWith("LIGHT"))
  {
    sscanf(packet.c_str(), "LIGHT,%d", &lightLevel);
  }

  else if (packet.startsWith("TANK"))
  {
    sscanf(packet.c_str(), "TANK,%f,%f", &reservoirLevel, &mixingLevel);
  }
}


/* ===== DASHBOARD DISPLAY ===== */

void printDashboard()
{
  Serial.println();
  Serial.println("===== SENSOR STATUS =====");

  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.println(" C");

  Serial.print("Humidity: ");
  Serial.print(humidity);
  Serial.println(" %");

  Serial.println();

  Serial.print("Soil A: ");
  Serial.print(soilA);
  Serial.println(" %");

  Serial.print("Soil B: ");
  Serial.print(soilB);
  Serial.println(" %");

  Serial.print("Soil C: ");
  Serial.print(soilC);
  Serial.println(" %");

  Serial.println();

  Serial.print("Light Level: ");
  Serial.print(lightLevel);
  Serial.println(" %");

  Serial.println();

  Serial.print("Reservoir Level: ");
  Serial.print(reservoirLevel);
  Serial.println(" %");

  Serial.print("Mix Tank Level: ");
  Serial.print(mixingLevel);
  Serial.println(" %");

  Serial.println("==========================");
}


/* ===== SETUP ===== */

void setup()
{
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);

  Serial.println("ESP32 Master Sensor Parser Started");
}


/* ===== LOOP ===== */

void loop()
{
  while (Serial2.available())
  {
    char c = Serial2.read();

    if (c == '\n')
    {
      parsePacket(incomingData);
      printDashboard();

      incomingData = "";
    }
    else
    {
      incomingData += c;
    }
  }
}