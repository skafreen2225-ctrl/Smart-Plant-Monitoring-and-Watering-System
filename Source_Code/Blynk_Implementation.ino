/*
Smart Plant Monitoring & Watering System

Implementation: Blynk IoT Version

Features:
- Remote monitoring through Blynk
- Automatic watering
- Soil moisture monitoring
- Water level monitoring
- OLED dashboard
*/

// ── Blynk config — fill in your credentials ──
#define BLYNK_TEMPLATE_ID   "XXXXXX"
#define BLYNK_TEMPLATE_NAME "smart plant system"
#define BLYNK_AUTH_TOKEN    "XXXXXXX"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <DHT.h>

// ─────────────────────────────────────────────
//  Wi-Fi Credentials
// ─────────────────────────────────────────────
char ssid[]     = "XXXXXXX";
char password[] = "XXXXXXX";

// ─────────────────────────────────────────────
//  PIN DEFINITIONS
// ─────────────────────────────────────────────
#define SOIL_PIN     34
#define RELAY_PIN    19
#define TRIG_PIN     13
#define ECHO_PIN     12
#define BUZZER_PIN   27
#define DHTPIN       18
#define DHTTYPE      DHT11
#define IR_PIN       23
#define TOUCH_PIN    32   // GPIO4

// ─────────────────────────────────────────────
//  THRESHOLDS
// ─────────────────────────────────────────────
#define DRY_THRESHOLD    3000
#define MOIST_THRESHOLD  1200
#define WATER_THRESHOLD  8     // cm — above this = LOW
#define TANK_MED_CM      5     // cm — between 5–8 = MEDIUM, below 5 = HIGH
#define TOUCH_THRESHOLD  40

// ─────────────────────────────────────────────
//  DHT
// ─────────────────────────────────────────────
DHT dht(DHTPIN, DHTTYPE);

// ─────────────────────────────────────────────
//  SENSOR VALUES
// ─────────────────────────────────────────────
int   soilValue     = 0;
float temperature   = 0.0;
float humidity      = 0.0;
float waterDistance = 0.0;
bool  pumpRunning   = false;

// ─────────────────────────────────────────────
//  BLYNK TIMER
// ─────────────────────────────────────────────
BlynkTimer timer;

// ─────────────────────────────────────────────
//  SENSOR READS
// ─────────────────────────────────────────────
void readSoilMoisture() {
  soilValue = analogRead(SOIL_PIN);
}

void readDHTSensor() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity    = h;
}

void readWaterLevel() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  waterDistance = (duration == 0) ? -1 : (duration * 0.034) / 2.0;
}

// ─────────────────────────────────────────────
//  PUMP CONTROL
// ─────────────────────────────────────────────
void startPump() { digitalWrite(RELAY_PIN, LOW);  pumpRunning = true;  }
void stopPump()  { digitalWrite(RELAY_PIN, HIGH); pumpRunning = false; }

void controlPump() {
  bool waterOk = (waterDistance != -1 && waterDistance < WATER_THRESHOLD);
  if (!waterOk) { stopPump(); return; }
  if      (soilValue > DRY_THRESHOLD   && !pumpRunning) startPump();
  else if (soilValue < MOIST_THRESHOLD &&  pumpRunning) stopPump();
}

// ─────────────────────────────────────────────
//  BUZZER
// ─────────────────────────────────────────────
void alertBuzzer() {
  if (waterDistance == -1 || waterDistance >= WATER_THRESHOLD)
    digitalWrite(BUZZER_PIN, HIGH);
  else
    digitalWrite(BUZZER_PIN, LOW);
}

// ─────────────────────────────────────────────
//  SOIL STATUS (0=Good, 1=Medium, 2=Dry)
// ─────────────────────────────────────────────
int getSoilStatus() {
  if      (soilValue > DRY_THRESHOLD)   return 2; // Dry
  else if (soilValue < MOIST_THRESHOLD) return 0; // Good
  else                                  return 1; // Medium
}

// ─────────────────────────────────────────────
//  TANK STATUS (0=OK, 1=LOW)
// ─────────────────────────────────────────────
int getTankStatus() {
  if (waterDistance == -1 || waterDistance >= WATER_THRESHOLD) return 1; // LOW
  return 0; // OK
}

// ─────────────────────────────────────────────
//  SEND TO BLYNK — every 2s
// ─────────────────────────────────────────────
void sendSensorData() {
  readSoilMoisture();
  readWaterLevel();
  controlPump();
  alertBuzzer();

  // Send to Blynk virtual pins
  Blynk.virtualWrite(V0, soilValue);
  Blynk.virtualWrite(V1, waterDistance == -1 ? 0 : waterDistance);
  Blynk.virtualWrite(V4, pumpRunning ? 1 : 0);
  Blynk.virtualWrite(V5, getSoilStatus());
  Blynk.virtualWrite(V6, getTankStatus());

  // Debug
  Serial.print("Soil: "); Serial.print(soilValue);
  Serial.print(" | Water: "); Serial.print(waterDistance);
  Serial.print("cm | Pump: "); Serial.print(pumpRunning ? "ON" : "OFF");
  Serial.print(" | Soil Status: "); Serial.print(getSoilStatus());
  Serial.print(" | Tank Status: "); Serial.println(getTankStatus());
}

// ─────────────────────────────────────────────
//  SEND DHT TO BLYNK — every 4s
// ─────────────────────────────────────────────
void sendDHTData() {
  readDHTSensor();
  Blynk.virtualWrite(V2, temperature);
  Blynk.virtualWrite(V3, humidity);

  Serial.print("Temp: "); Serial.print(temperature);
  Serial.print("C | Humidity: "); Serial.println(humidity);
}

// ─────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN,  OUTPUT); digitalWrite(RELAY_PIN,  HIGH);
  pinMode(TRIG_PIN,   OUTPUT);
  pinMode(ECHO_PIN,   INPUT);
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
  pinMode(IR_PIN,     INPUT);

  dht.begin();

  // Connect to Blynk (handles Wi-Fi connection internally)
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, password);

  // Timer setup
  timer.setInterval(2000L, sendSensorData);
  timer.setInterval(4000L, sendDHTData);

  Serial.println("=== Smart Plant Blynk Ready ===");
}

// ─────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────
void loop() {
  Blynk.run();
  timer.run();
}
