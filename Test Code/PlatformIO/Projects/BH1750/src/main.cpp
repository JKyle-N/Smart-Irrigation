#include <Arduino.h>
#include <Wire.h>

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("I2C Scanner");
    Serial.println("----------------");

    Wire.begin(21, 22);

    byte count = 0;

    for (byte address = 1; address < 127; address++)
    {
        Wire.beginTransmission(address);

        byte error = Wire.endTransmission();

        if (error == 0)
        {
            Serial.print("Found device at 0x");

            if (address < 16)
                Serial.print("0");

            Serial.println(address, HEX);

            count++;
        }
        else if (error == 4)
        {
            Serial.print("Unknown error at 0x");

            if (address < 16)
                Serial.print("0");

            Serial.println(address, HEX);
        }
    }

    Serial.println("----------------");

    if (count == 0)
    {
        Serial.println("No I2C devices found");
    }
    else
    {
        Serial.print("Devices found: ");
        Serial.println(count);
    }
}

void loop()
{
}