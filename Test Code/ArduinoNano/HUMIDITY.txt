#include <DHT.h>

#define DHTPIN 11        // DHT22 data pin connected to D11
#define DHTTYPE DHT22   // Define sensor type

DHT dht(DHTPIN, DHTTYPE);

void setup() {
  Serial.begin(9600);
  Serial.println("DHT22 Test Started...");
  dht.begin();
}

void loop() {
  // Wait a few seconds between measurements
  delay(2000);

  // Read humidity
  float humidity = dht.readHumidity();
  // Read temperature in Celsius
  float temperature = dht.readTemperature();
  // Read temperature in Fahrenheit (optional)
  float temperatureF = dht.readTemperature(true);

  // Check if any reads failed
  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Failed to read from DHT sensor!");
    return;
  }

  Serial.print("Humidity: ");
  Serial.print(humidity);
  Serial.print(" %\t");

  Serial.print("Temperature: ");
  Serial.print(temperature);
  Serial.print(" °C\t");

  Serial.print(temperatureF);
  Serial.println(" °F");
}
