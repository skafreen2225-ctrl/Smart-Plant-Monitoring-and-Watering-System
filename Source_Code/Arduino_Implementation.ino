/*
Smart Plant Monitoring & Watering System

Implementation: Standalone ESP32 Version

Features:
- Automatic watering
- Soil moisture monitoring
- Water tank level monitoring
- OLED dashboard
- Touch interaction
- IR proximity detection
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#define TOUCH_ACTIVE_STATE   HIGH
#define IR_ACTIVE_STATE      LOW
#define SOIL_PIN     34
#define RELAY_PIN    19
#define TRIG_PIN     13
#define ECHO_PIN     12
#define BUZZER_PIN   27
#define DHTPIN       18
#define DHTTYPE      DHT11
#define IR_PIN       23
#define TOUCH_PIN    32 
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHTPIN, DHTTYPE);
// ─────────────────────────────────────────────
//  THRESHOLDS
// ─────────────────────────────────────────
int dryThreshold   = 3000;
int moistThreshold = 1800; // Increased from 1200 to accommodate typical ESP32 capacitive sensor ranges
int waterThreshold = 8;    // cm — distance above this = tank empty
// ─────────────────────────────────────────────
//  SENSOR VALUES & FILTER STATS
// ─────────────────────────────────────────────
int   soilValue     = 0;
float temperature   = 0.0;
float humidity      = 0.0;
float waterDistance = 0.0;
bool  pumpRunning   = false;
// Glitch-filtering state for HC-SR04
int consecutiveWaterFailures = 0;
#define WATER_FAILURE_LIMIT      3  // Require 3 consecutive failed/empty readings to alert/stop pump
#define ULTRASONIC_READ_SAMPLES  3  // Number of hardware pulses to average per cycle
// ─────────────────────────────────────────────
//  TIMING & DEBOUNCE
// ─────────────────────────────────────────────
unsigned long lastSensorRead = 0;
unsigned long lastDHTRead    = 0;
#define SENSOR_INTERVAL 2000
#define DHT_INTERVAL    4000
unsigned long lastTouchTime  = 0;
unsigned long lastIRTime     = 0;
#define TOUCH_DEBOUNCE_MS    300
#define IR_DEBOUNCE_MS       300
// Cooldown to lock out triggers after an animation finishes, guaranteeing dashboard visibility
unsigned long lastAnimationEndTime = 0;
bool          cooldownActive       = false;
#define COOLDOWN_DURATION    5000  // 5 seconds dashboard display guarantee
// ─────────────────────────────────────────────
//  EYE SEQUENCE STATE
// ─────────────────────────────────────────────
bool          eyeActive       = false;   // true while an eye sequence is playing
bool          showingEyes     = false;   // true during the 3s eye phase
bool          showingText     = false;   // true during the 2s text phase
unsigned long eyePhaseStart   = 0;
int           currentExprIdx  = 0;       // 0-2, which expression is playing
String        currentMessage  = "";
String        currentTrigger  = "";      // "touch" or "ir"
#define EYE_DURATION  3000
#define TEXT_DURATION 2000
// ─────────────────────────────────────────────
//  SHUFFLED DECK — shared structure for touch & IR
// ─────────────────────────────────────────────
struct Deck {
  int order[3];      // shuffled order of indices 0,1,2
  int pos;           // current position in deck (0,1,2)
  int lastPlayed;    // last expression index played (-1 = none yet)
};
Deck touchDeck;
Deck irDeck;
// Fisher-Yates shuffle, ensuring deck[0] != lastPlayed
void shuffleDeck(Deck &d) {
  d.order[0] = 0; d.order[1] = 1; d.order[2] = 2;
  for (int i = 2; i > 0; i--) {
    int j = random(0, i + 1);
    int tmp = d.order[i];
    d.order[i] = d.order[j];
    d.order[j] = tmp;
  }
  if (d.lastPlayed != -1 && d.order[0] == d.lastPlayed) {
    int swapWith = random(1, 3); // 1 or 2
    int tmp = d.order[0];
    d.order[0] = d.order[swapWith];
    d.order[swapWith] = tmp;
  }
  d.pos = 0;
}
int nextCard(Deck &d) {
  if (d.pos >= 3) shuffleDeck(d);
  int expr = d.order[d.pos];
  d.pos++;
  d.lastPlayed = expr;
  return expr;
}
// ─────────────────────────────────────────────
//  EXPRESSION DATA
// ─────────────────────────────────────────────
// Touch expressions: 0=Grumpy, 1=Dizzy, 2=Sleepy
const char* touchMessages[3] = {
  "Hey! I was asleep!",
  "Whoa... dizzy!",
  "Zzz... 5 more mins..."
};
// IR expressions: 0=Surprised, 1=Unimpressed, 2=Cool
const char* irMessages[3] = {
  "Oh! A visitor!",
  "Oh. It's you again.",
  "Hey, I'm watching you"
};
// ─────────────────────────────────────────────
//  SENSOR READS (With filtering)
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
  float sum = 0;
  int validSamples = 0;
 
  for (int i = 0; i < ULTRASONIC_READ_SAMPLES; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
   
    // 25ms timeout corresponds to approx 4.25 meters max distance
    long duration = pulseIn(ECHO_PIN, HIGH, 25000);
   
    if (duration > 0) {
      float dist = (duration * 0.0343) / 2.0;
      // Filter out physical outliers
      if (dist > 1.5 && dist < 250.0) {
        sum += dist;
        validSamples++;
      }
    }
    delay(15); // Short gap between pulses to prevent echo overlap interference
  }
 
  if (validSamples > 0) {
    waterDistance = sum / validSamples;
   
    // Water level is healthy if distance to water is less than empty threshold limit
    if (waterDistance < waterThreshold) {
      consecutiveWaterFailures = 0; // Reset consecutive failures
    } else {
      consecutiveWaterFailures++; // Water is too low
    }
  } else {
    waterDistance = -1; // Flag sensor measurement failure
    consecutiveWaterFailures++;
  }
}
// ─────────────────────────────────────────────
//  PUMP & BUZZER
// ─────────────────────────────────────────────
void startPump() {
  digitalWrite(RELAY_PIN, LOW);  // Active-LOW relay closes switch to start pump
  pumpRunning = true;  
}
void stopPump()  {
  digitalWrite(RELAY_PIN, HIGH); // Active-LOW relay opens switch to stop pump
  pumpRunning = false;
}
void controlPump() {
  // If water level failure limit has been reached, protect pump by forcing it OFF
  bool waterEmpty = (consecutiveWaterFailures >= WATER_FAILURE_LIMIT);
  if (waterEmpty) {
    stopPump();
    return;
  }
 
  // Normal soil moisture hysteresis control
  if      (soilValue > dryThreshold   && !pumpRunning) startPump();
  else if (soilValue < moistThreshold &&  pumpRunning) stopPump();
}
void alertBuzzer() {
  bool waterEmpty = (consecutiveWaterFailures >= WATER_FAILURE_LIMIT);
  if (waterEmpty) {
    digitalWrite(BUZZER_PIN, HIGH);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
}
// ─────────────────────────────────────────────
//  DASHBOARD DISPLAY
// ─────────────────────────────────────────────
void displayDashboard() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  // Soil
  display.setCursor(0, 0);
  display.print("Soil: ");
  display.print(soilValue);
  if      (soilValue > dryThreshold)   display.print(" DRY");
  else if (soilValue < moistThreshold) display.print(" GOOD");
  else                                 display.print(" MED");
  // Temperature
  display.setCursor(0, 16);
  display.print("Temp: ");
  display.print(temperature, 1);
  display.print(" C");
  // Humidity
  display.setCursor(0, 32);
  display.print("Hum:  ");
  display.print(humidity, 1);
  display.print("%");
  // Water level
  display.setCursor(0, 48);
  display.print("Water: ");
  if (consecutiveWaterFailures >= WATER_FAILURE_LIMIT) {
    if (waterDistance == -1) {
      display.print("No Echo!");
    } else {
      display.print("EMPTY!");
    }
  } else {
    if (waterDistance == -1) {
      display.print("Calc...");
    } else {
      display.print(waterDistance, 1);
      display.print("cm");
    }
  }
  // Pump status
  display.setCursor(90, 48);
  display.print(pumpRunning ? "PMP:ON" : "PMP:OF");
  display.display();
}
// ─────────────────────────────────────────────
//  EYE DRAWING
// ─────────────────────────────────────────────
#define EL_X 38
#define ER_X 90
#define E_Y  28
#define E_R  14
void drawGrumpy() {
  display.clearDisplay();
  display.fillRoundRect(EL_X - E_R, E_Y - 4, E_R * 2, 8, 3, WHITE);
  display.fillRoundRect(ER_X - E_R, E_Y - 4, E_R * 2, 8, 3, WHITE);
  display.drawLine(EL_X - E_R, E_Y - 12, EL_X + E_R, E_Y - 8, WHITE);
  display.drawLine(ER_X - E_R, E_Y - 8,  ER_X + E_R, E_Y - 12, WHITE);
  display.display();
}
void drawDizzy() {
  display.clearDisplay();
  display.drawLine(EL_X - 8, E_Y - 8, EL_X + 8, E_Y + 8, WHITE);
  display.drawLine(EL_X + 8, E_Y - 8, EL_X - 8, E_Y + 8, WHITE);
  display.drawCircle(EL_X, E_Y, E_R, WHITE);
  display.drawLine(ER_X - 8, E_Y - 8, ER_X + 8, E_Y + 8, WHITE);
  display.drawLine(ER_X + 8, E_Y - 8, ER_X - 8, E_Y + 8, WHITE);
  display.drawCircle(ER_X, E_Y, E_R, WHITE);
  display.display();
}
void drawSleepy() {
  display.clearDisplay();
  display.drawCircle(EL_X, E_Y, E_R, WHITE);
  display.fillRect(EL_X - E_R - 1, E_Y - E_R - 1, E_R * 2 + 2, E_R + 2, BLACK);
  display.drawFastHLine(EL_X - E_R, E_Y, E_R * 2, WHITE);
  display.drawCircle(ER_X, E_Y, E_R, WHITE);
  display.fillRect(ER_X - E_R - 1, E_Y - E_R - 1, E_R * 2 + 2, E_R + 2, BLACK);
  display.drawFastHLine(ER_X - E_R, E_Y, E_R * 2, WHITE);
  display.display();
}
void drawSurprised() {
  display.clearDisplay();
  display.drawCircle(EL_X, E_Y, E_R + 2, WHITE);
  display.fillCircle(EL_X, E_Y, 5, WHITE);
  display.drawCircle(ER_X, E_Y, E_R + 2, WHITE);
  display.fillCircle(ER_X, E_Y, 5, WHITE);
  display.drawFastHLine(EL_X - E_R, E_Y - E_R - 5, E_R * 2, WHITE);
  display.drawFastHLine(ER_X - E_R, E_Y - E_R - 5, E_R * 2, WHITE);
  display.display();
}
void drawUnimpressed() {
  display.clearDisplay();
  display.drawCircle(EL_X, E_Y, E_R, WHITE);
  display.fillRect(EL_X - E_R - 1, E_Y - E_R - 1, E_R * 2 + 2, (int)(E_R * 1.2), BLACK);
  display.drawFastHLine(EL_X - E_R, E_Y - 4, E_R * 2, WHITE);
  display.drawCircle(ER_X, E_Y, E_R, WHITE);
  display.fillRect(ER_X - E_R - 1, E_Y - E_R - 1, E_R * 2 + 2, (int)(E_R * 1.2), BLACK);
  display.drawFastHLine(ER_X - E_R, E_Y - 4, E_R * 2, WHITE);
  display.fillCircle(EL_X, E_Y + 4, 3, WHITE);
  display.fillCircle(ER_X, E_Y + 4, 3, WHITE);
  display.display();
}
void drawCool() {
  display.clearDisplay();
  display.drawCircle(EL_X, E_Y, E_R, WHITE);
  display.fillRect(EL_X - E_R - 1, E_Y - E_R - 1, E_R * 2 + 2, E_R / 2, BLACK);
  display.drawFastHLine(EL_X - E_R, E_Y - E_R / 2, E_R * 2, WHITE);
  display.fillCircle(EL_X, E_Y + 2, 4, WHITE);
  display.drawCircle(ER_X, E_Y, E_R, WHITE);
  display.fillRect(ER_X - E_R - 1, E_Y - E_R - 1, E_R * 2 + 2, E_R / 2, BLACK);
  display.drawFastHLine(ER_X - E_R, E_Y - E_R / 2, E_R * 2, WHITE);
  display.fillCircle(ER_X, E_Y + 2, 4, WHITE);
  display.display();
}
void drawEyes(String trigger, int exprIdx) {
  if (trigger == "touch") {
    switch (exprIdx) {
      case 0: drawGrumpy();     break;
      case 1: drawDizzy();      break;
      case 2: drawSleepy();     break;
    }
  } else {
    switch (exprIdx) {
      case 0: drawSurprised();  break;
      case 1: drawUnimpressed(); break;
      case 2: drawCool();       break;
    }
  }
}
// ─────────────────────────────────────────────
//  TEXT DISPLAY
// ─────────────────────────────────────────────
void displayMessage(String msg) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  int cx = (SCREEN_WIDTH  - w) / 2;
  int cy = (SCREEN_HEIGHT - h) / 2;
  display.setCursor(cx, cy);
  display.print(msg);
  display.display();
}
// ─────────────────────────────────────────────
//  TRIGGER EYE SEQUENCE
// ─────────────────────────────────────────────
void triggerSequence(String trigger) {
  if (eyeActive) return;
  eyeActive     = true;
  showingEyes   = true;
  showingText   = false;
  currentTrigger = trigger;
  eyePhaseStart = millis();
  if (trigger == "touch") {
    currentExprIdx  = nextCard(touchDeck);
    currentMessage  = String(touchMessages[currentExprIdx]);
  } else {
    currentExprIdx  = nextCard(irDeck);
    currentMessage  = String(irMessages[currentExprIdx]);
  }
  drawEyes(currentTrigger, currentExprIdx);
}
// ─────────────────────────────────────────────
//  UPDATE EYE SEQUENCE (called every loop)
// ─────────────────────────────────────────────
void updateEyeSequence() {
  if (!eyeActive) return;
  unsigned long elapsed = millis() - eyePhaseStart;
  if (showingEyes && elapsed >= EYE_DURATION) {
    showingEyes = false;
    showingText = true;
    eyePhaseStart = millis();
    displayMessage(currentMessage);
    return;
  }
  if (showingText && elapsed >= TEXT_DURATION) {
    showingEyes = false;
    showingText = false;
    eyeActive   = false;
   
    // Activate cooldown lockouts
    lastAnimationEndTime = millis();
    cooldownActive = true;
   
    // Instantly restore dashboard display when animation is complete
    displayDashboard();
  }
}
// ─────────────────────────────────────────────
//  TOUCH & IR POLLING (With active levels & debounce)
// ─────────────────────────────────────────────
bool lastTouchState = !TOUCH_ACTIVE_STATE; // Initialize as inactive
bool lastIRState    = !IR_ACTIVE_STATE;    // Initialize as inactive
void handleTouch() {
  bool touchState = digitalRead(TOUCH_PIN);
  unsigned long now = millis();
 
  // Edge detection: trigger on active state transition
  if (touchState == TOUCH_ACTIVE_STATE && lastTouchState != TOUCH_ACTIVE_STATE) {
    // Only trigger if idle, not in cooldown lockout, and debounce elapsed
    if (!eyeActive && !cooldownActive && (now - lastTouchTime >= TOUCH_DEBOUNCE_MS)) {
      lastTouchTime = now;
      triggerSequence("touch");
    }
  }
  lastTouchState = touchState;
}
void handleIR() {
  bool irState = digitalRead(IR_PIN);
  unsigned long now = millis();
 
  // Edge detection: trigger on active state transition
  if (irState == IR_ACTIVE_STATE && lastIRState != IR_ACTIVE_STATE) {
    // Only trigger if idle, not in cooldown lockout, and debounce elapsed
    if (!eyeActive && !cooldownActive && (now - lastIRTime >= IR_DEBOUNCE_MS)) {
      lastIRTime = now;
      triggerSequence("ir");
    }
  }
  lastIRState = irState;
}
// ─────────────────────────────────────────────
//  DEBUG SERIAL LOGGER
// ─────────────────────────────────────────────
void logStatus() {
  Serial.print("--- SYSTEM STATUS --- [");
  Serial.print(millis() / 1000);
  Serial.println("s]");
 
  Serial.print("Soil Moisture Value : ");
  Serial.print(soilValue);
  Serial.print(" (Dry Threshold: ");
  Serial.print(dryThreshold);
  Serial.print(", Moist Threshold: ");
  Serial.print(moistThreshold);
  Serial.println(")");
  Serial.print("Water Tank Distance : ");
  if (waterDistance == -1) {
    Serial.print("No Echo! / Failure (Count: ");
    Serial.print(consecutiveWaterFailures);
    Serial.println(")");
  } else {
    Serial.print(waterDistance, 1);
    Serial.print(" cm (Threshold: ");
    Serial.print(waterThreshold);
    Serial.print(" cm, Empty Failures: ");
    Serial.print(consecutiveWaterFailures);
    Serial.println(")");
  }
  Serial.print("Air Temperature     : ");
  Serial.print(temperature, 1);
  Serial.println(" C");
  Serial.print("Air Humidity        : ");
  Serial.print(humidity, 1);
  Serial.println(" %");
  Serial.print("Water Pump State    : ");
  Serial.println(pumpRunning ? "RUNNING (ON)" : "STOPPED (OFF)");
  Serial.print("Animation State     : ");
  if (eyeActive) {
    Serial.print("ACTIVE (Trigger: ");
    Serial.print(currentTrigger);
    Serial.println(")");
  } else if (cooldownActive) {
    Serial.print("COOLDOWN LOCKOUT (Remaining: ");
    Serial.print((COOLDOWN_DURATION - (now_diff())) / 1000.0, 1);
    Serial.println("s)");
  } else {
    Serial.println("READY / IDLE (Showing Dashboard)");
  }
  Serial.println("─────────────────────────────────────────────");
}
// Helper to calculate cooldown remaining time correctly
unsigned long now_diff() {
  return millis() - lastAnimationEndTime;
}
// ─────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("Booting Smart Plant System...");
  pinMode(RELAY_PIN,  OUTPUT); digitalWrite(RELAY_PIN,  HIGH); // Active-LOW relay starts OFF (HIGH)
  pinMode(TRIG_PIN,   OUTPUT);
  pinMode(ECHO_PIN,   INPUT);
  pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
 
  // Set up inputs. Enabling internal pullup for IR to prevent floating if open-drain/disconnected.
  pinMode(IR_PIN,     INPUT_PULLUP);
  pinMode(TOUCH_PIN,  INPUT);        
  dht.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found — check wiring!");
    while (true);
  }
  display.clearDisplay();
  display.display();
  // Wait for sensor voltage rails to stabilize after boot to prevent transient false reads
  Serial.println("Waiting 1.5s for sensor voltages to stabilize...");
  delay(1500);
  // Seed RNG using floating ADC input
  randomSeed(analogRead(35));
  // Initialize decks
  touchDeck.lastPlayed = -1; shuffleDeck(touchDeck);
  irDeck.lastPlayed    = -1; shuffleDeck(irDeck);
  // Initialize input states after stabilization to ensure edge trigger accuracy
  lastIRState = digitalRead(IR_PIN);
  lastTouchState = digitalRead(TOUCH_PIN);
  // Perform first sensor reads
  readDHTSensor();
  readSoilMoisture();
  readWaterLevel();
  controlPump();
  alertBuzzer();
  displayDashboard();
 
  Serial.println("Initialization complete! Running loop...");
  logStatus();
}
// ─────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────
void loop() {
  unsigned long now = millis();
  // Manage animation cooldown lockout duration
  if (cooldownActive) {
    if (now - lastAnimationEndTime >= COOLDOWN_DURATION) {
      cooldownActive = false;
      Serial.println("Dashboard cooldown lockout cleared. Sensor triggers re-enabled.");
    }
  }
  // Sensor reads & dashboard display update (Runs every 2 seconds)
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSoilMoisture();
    readWaterLevel();
    controlPump();
    alertBuzzer();
    if (!eyeActive) {
      displayDashboard();
    }
    // Output full diagnostic status to Serial Monitor
    logStatus();
  }
  // DHT sensor reading on its own 4s timer
  if (now - lastDHTRead >= DHT_INTERVAL) {
    lastDHTRead = now;
    readDHTSensor();
    if (!eyeActive) {
      displayDashboard();
    }
  }
  // Poll Touch and IR sensors with debounced edge detection
  handleTouch();
  handleIR();
  // Eye sequence animation state machine
  updateEyeSequence();
}

