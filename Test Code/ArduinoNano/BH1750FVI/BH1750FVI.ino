#include <Wire.h>
#include <BH1750.h>

BH1750 lightMeter;

void setup() {
Serial.begin(9600);
Wire.begin();

// Force address 0x23
if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23)) {
Serial.println("BH1750 initialized!");
} else {
Serial.println("BH1750 init failed!");
}
}

void loop() {
float lux = lightMeter.readLightLevel();

if (lux < 0) {
Serial.println("Sensor read error!");
} else {
Serial.print("Light: ");
Serial.print(lux);
Serial.println(" lx");
}

delay(1000);
}
