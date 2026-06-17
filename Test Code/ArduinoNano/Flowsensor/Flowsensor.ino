#define FLOW_PIN 2   // D2 (INT0)

const float PULSES_PER_LITER = 450.0;

volatile unsigned long pulseCount = 0;
unsigned long lastTime = 0;
float flowRate = 0.0;
float totalLiters = 0.0;

// Interrupt function (NO IRAM_ATTR here)
void pulseCounter() {
  pulseCount++;
}

void setup() {
  Serial.begin(9600);

  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, RISING);

  Serial.println("Flow Sensor Test Started...");
}

void loop() {
  unsigned long currentTime = millis();

  if (currentTime - lastTime >= 1000) {

    noInterrupts();
    unsigned long pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    flowRate = (pulses / PULSES_PER_LITER) * 60.0;
    totalLiters += (pulses / PULSES_PER_LITER);

    Serial.println("------");
    Serial.print("Pulses: ");
    Serial.println(pulses);

    Serial.print("Flow Rate: ");
    Serial.print(flowRate);
    Serial.println(" L/min");

    Serial.print("Total Volume: ");
    Serial.print(totalLiters, 3);
    Serial.println(" L");

    lastTime = currentTime;
  }
}