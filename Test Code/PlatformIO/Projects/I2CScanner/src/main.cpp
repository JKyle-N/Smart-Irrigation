#include <Arduino.h>
#include <Wire.h>

// ESP32 default I2C pins
#define I2C_SDA 21
#define I2C_SCL 22

// Re-scan interval (ms)
const unsigned long SCAN_INTERVAL = 5000;

void scanI2C()
{
    byte count = 0;

    Serial.println("Scanning I2C bus...");

    for (byte address = 1; address < 127; address++)
    {
        Wire.beginTransmission(address);
        byte error = Wire.endTransmission();

        if (error == 0)
        {
            Serial.print("  Found device at 0x");
            if (address < 16)
                Serial.print("0");
            Serial.println(address, HEX);
            count++;
        }
        else if (error == 4)
        {
            Serial.print("  Unknown error at 0x");
            if (address < 16)
                Serial.print("0");
            Serial.println(address, HEX);
        }
    }

    if (count == 0)
        Serial.println("  No I2C devices found");
    else
    {
        Serial.print("  Devices found: ");
        Serial.println(count);
    }

    Serial.println("----------------");
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("=== I2C Scanner ===");
    Serial.print("SDA=GPIO");
    Serial.print(I2C_SDA);
    Serial.print("  SCL=GPIO");
    Serial.println(I2C_SCL);
    Serial.println("----------------");

    Wire.begin(I2C_SDA, I2C_SCL);
}

void loop()
{
    static unsigned long lastScan = 0;

    if (millis() - lastScan >= SCAN_INTERVAL)
    {
        lastScan = millis();
        scanI2C();
    }
}
