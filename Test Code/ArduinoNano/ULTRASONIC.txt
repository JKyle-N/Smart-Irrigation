// ================= CONFIG =================
const unsigned long TIMEOUT_US = 30000; // 30 ms timeout

// ----- Reservoir Tank Configuration -----
const float RES_MIN_M = 0.03;   // FULL
const float RES_MAX_M = 0.53;   // EMPTY

// ----- Mix Tank Configuration -----
const float MIX_MIN_M = 0.04;   // FULL
const float MIX_MAX_M = 0.50;   // EMPTY

// ---------------- PINS -------------------
// Reservoir Tank
#define TRIG_RES 4
#define ECHO_RES 5

// Mix Tank
#define TRIG_MIX 6
#define ECHO_MIX 7

// ================ FUNCTIONS ===============
float readUltrasonicMeters(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, TIMEOUT_US);
  if (duration == 0) return -1.0;  // timeout

  return (duration * 0.000001 * 343.0) / 2.0;
}

float distanceToPercent(float distance, float minM, float maxM) {
  if (distance < 0) return -1.0;

  float percent = (maxM - distance) / (maxM - minM) * 100.0;
  return constrain(percent, 0.0, 100.0);
}

// ================= SETUP ==================
void setup() {
  Serial.begin(9600);

  pinMode(TRIG_RES, OUTPUT);
  pinMode(ECHO_RES, INPUT);

  pinMode(TRIG_MIX, OUTPUT);
  pinMode(ECHO_MIX, INPUT);

  Serial.println("Dual Ultrasonic Tank Level (% - SI Units)");
}

// ================= LOOP ===================
void loop() {
  delay(1500);

  float distRes = readUltrasonicMeters(TRIG_RES, ECHO_RES);
  float distMix = readUltrasonicMeters(TRIG_MIX, ECHO_MIX);

  float resPercent = distanceToPercent(distRes, RES_MIN_M, RES_MAX_M);
  float mixPercent = distanceToPercent(distMix, MIX_MIN_M, MIX_MAX_M);

  Serial.println("-------------------------");

  if (resPercent < 0)
    Serial.println("Reservoir Tank: SENSOR ERROR");
  else {
    Serial.print("Reservoir Tank Level: ");
    Serial.print(resPercent, 1);
    Serial.println(" %");
  }

  if (mixPercent < 0)
    Serial.println("Mix Tank: SENSOR ERROR");
  else {
    Serial.print("Mix Tank Level: ");
    Serial.print(mixPercent, 1);
    Serial.println(" %");
  }
}
