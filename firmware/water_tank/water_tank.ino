/*
 * Water tank monitoring & resilience to False Data Injection (FDI)
 * Arduino Uno — HC-SR04 level sensor, relay-driven pump, float switch,
 * serial link to a LabVIEW supervision VI.
 *
 * Control: hysteresis — pump ON below LEVEL_LOW, OFF above LEVEL_HIGH.
 *
 * Defences against a falsified level measurement:
 *   1. Range check            : the level must stay within 0..100 %.
 *   2. Physical consistency   : the level cannot rise or drop faster than
 *                               the pump and the outlet physically allow.
 *   3. Stuck-value detection  : with the pump ON the level must change.
 *   4. Cross-check            : the high-level float switch (independent
 *                               sensor) contradicts a "not full" reading.
 *   5. Hardware interlock     : the float switch (NC) is wired in series
 *                               with the pump supply and cuts it even if
 *                               the Arduino is compromised (see docs).
 * Any detection -> pump OFF (safe state) + activeAlarm, until reset by operator.
 *
 * LabVIEW protocol (9600 8N1, one line every 500 ms):
 *   L=<level %>;D=<distance cm>;P=<0|1>;F=<0|1>;M=<A|M>;A=<activeAlarm code>
 * Commands (end with '\n'):
 *   AUTO | MAN | PUMP=1 | PUMP=0 | RESET
 *   INJECT=<bias %>  -> lab demo of an FDI attack (adds a bias to the reading)
 *   STUCK=<level %>  -> lab demo: reported level frozen at a value (STUCK=-1 stops)
 */

// ---------------------------------------------------------------- pins
const uint8_t PIN_TRIG   = 9;
const uint8_t PIN_ECHO   = 10;
const uint8_t PIN_RELAY  = 7;   // pump relay (active HIGH)
const uint8_t PIN_FLOAT  = 2;   // float switch to GND, INPUT_PULLUP: LOW = tank full
const uint8_t PIN_BUZZER = 8;
const uint8_t PIN_LED_ALARM = 13;

// --------------------------------------------------------------- tank
const float TANK_HEIGHT_CM = 30.0;  // sensor to bottom
const float DEAD_ZONE_CM   = 3.0;   // HC-SR04 blind zone above max level
const float LEVEL_LOW      = 20.0;  // %
const float LEVEL_HIGH     = 85.0;  // %

// ----------------------------------------------- plausibility limits
const float MAX_RISE_PCT_S   = 4.0;   // fastest possible filling (measured on the prototype)
const float MAX_DROP_PCT_S   = 2.0;   // fastest possible draining
const unsigned long STUCK_MS = 8000;  // pump ON but no change for this long
const float STUCK_EPS        = 1.0;   // %

const unsigned long PERIOD_MS = 500;

enum Alarm : uint8_t { ALARM_NONE = 0, ALARM_RANGE, ALARM_RATE, ALARM_STUCK, ALARM_FLOAT, ALARM_SENSOR };
const char *ALARM_TEXT[] = {"NONE", "RANGE", "RATE", "STUCK", "FLOAT_MISMATCH", "NO_ECHO"};

bool autoMode = true;
bool pumpOn = false;
Alarm activeAlarm = ALARM_NONE;

float lastLevel = -1;
unsigned long lastSample = 0;
float stuckRef = 0;
unsigned long stuckSince = 0;

// lab attack simulation
float injectBias = 0;
float stuckValue = -1;

String rx;

// ------------------------------------------------------------ sensor
float readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, 30000UL);
  return us == 0 ? -1 : us / 58.0;
}

float distanceToLevel(float d) {
  float water = TANK_HEIGHT_CM - d;
  return 100.0 * water / (TANK_HEIGHT_CM - DEAD_ZONE_CM);
}

// ------------------------------------------------------------ actuators
void setPump(bool on) {
  pumpOn = on;
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
}

void triggerAlarm(Alarm a) {
  if (activeAlarm == ALARM_NONE) activeAlarm = a;
  setPump(false);
}

// --------------------------------------------------------- detection
void checkPlausibility(float level, bool floatFull, unsigned long now) {
  if (level < -5 || level > 105) { triggerAlarm(ALARM_RANGE); return; }

  if (lastLevel >= 0) {
    float dt = (now - lastSample) / 1000.0;
    float rate = (level - lastLevel) / dt;           // % per second
    if (rate > MAX_RISE_PCT_S * 1.5 || rate < -MAX_DROP_PCT_S * 1.5) { triggerAlarm(ALARM_RATE); return; }
  }

  if (pumpOn) {
    if (fabs(level - stuckRef) > STUCK_EPS) { stuckRef = level; stuckSince = now; }
    else if (now - stuckSince > STUCK_MS) { triggerAlarm(ALARM_STUCK); return; }
  } else {
    stuckRef = level;
    stuckSince = now;
  }

  // float switch says full but the measurement says otherwise
  if (floatFull && level < LEVEL_HIGH - 5) triggerAlarm(ALARM_FLOAT);
}

// ------------------------------------------------------------ commands
void handleCommand(String c) {
  c.trim();
  c.toUpperCase();
  if (c == "AUTO") autoMode = true;
  else if (c == "MAN") autoMode = false;
  else if (c == "PUMP=1" && !autoMode && activeAlarm == ALARM_NONE) setPump(true);
  else if (c == "PUMP=0") setPump(false);
  else if (c == "RESET") { activeAlarm = ALARM_NONE; lastLevel = -1; injectBias = 0; stuckValue = -1; }
  else if (c.startsWith("INJECT=")) injectBias = c.substring(7).toFloat();
  else if (c.startsWith("STUCK=")) stuckValue = c.substring(6).toFloat();
}

void readSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n') { handleCommand(rx); rx = ""; }
    else if (ch != '\r' && rx.length() < 32) rx += ch;
  }
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(9600);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_FLOAT, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_ALARM, OUTPUT);
  setPump(false);
}

void loop() {
  readSerial();
  unsigned long now = millis();
  if (now - lastSample < PERIOD_MS) return;

  float d = readDistanceCm();
  bool floatFull = digitalRead(PIN_FLOAT) == LOW;
  float level;

  if (d < 0) {
    triggerAlarm(ALARM_SENSOR);
    level = lastLevel;
  } else {
    level = distanceToLevel(d);
    // ---- lab attack simulation: what the controller receives is falsified
    if (stuckValue >= 0) level = stuckValue;
    level += injectBias;
    checkPlausibility(level, floatFull, now);
  }

  if (activeAlarm == ALARM_NONE && autoMode) {
    if (level < LEVEL_LOW) setPump(true);
    else if (level > LEVEL_HIGH) setPump(false);
  }
  if (floatFull) setPump(false);   // software copy of the hardware interlock

  digitalWrite(PIN_LED_ALARM, activeAlarm != ALARM_NONE);
  digitalWrite(PIN_BUZZER, activeAlarm != ALARM_NONE && (now / 250) % 2);

  Serial.print("L="); Serial.print(level, 1);
  Serial.print(";D="); Serial.print(d, 1);
  Serial.print(";P="); Serial.print(pumpOn ? 1 : 0);
  Serial.print(";F="); Serial.print(floatFull ? 1 : 0);
  Serial.print(";M="); Serial.print(autoMode ? 'A' : 'M');
  Serial.print(";A="); Serial.println(ALARM_TEXT[activeAlarm]);

  lastLevel = level;
  lastSample = now;
}
