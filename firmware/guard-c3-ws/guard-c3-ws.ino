// Anti-Theft Guard - ESP32-C3 Super Mini + WebSocket dashboard
// Library needed: "WebSockets" by Markus Sattler (Library Manager)
// Arduino IDE: Board = "ESP32C3 Dev Module", USB CDC On Boot = Enabled

#include <WiFi.h>
#include <WebSocketsClient.h>

// ================= EDIT THESE =================
const char* WIFI_SSID   = "YOUR_WIFI_SSID";
const char* WIFI_PASS   = "YOUR_WIFI_PASSWORD";
const char* SERVER_HOST = "anti-intruder.onrender.com";
const uint16_t SERVER_PORT = 443;
const bool USE_TLS = true;                // true only when using a tunnel (port 443)
const char* DEVICE_KEY  = "YOUR_DEVICE_KEY"; // same as DEVICE_KEY in server.js
const char* LOCATION    = "Bag-Area";       // no spaces
const bool LATCH_ALARM  = true;             // true = alarm stays on until you press DISARM on the dashboard
// ==============================================

// Pins (ESP32-C3 Super Mini)
const int TRIG = 3, ECHO = 4, BUZZER = 5, RED = 6, GREEN = 7;

const float MAX_RANGE_CM = 450;   // about the farthest an HC-SR04 can measure
const float NEAR_CM = 20;         // anything closer than this always triggers
const float CHANGE_CM = 10;       // getting this much closer than normal triggers
const unsigned long SENSOR_INTERVAL_MS = 60; // minimum interval recommended for HC-SR04
const unsigned long BEEP_MS = 10;

enum State { DISARMED, ARMED, ALARM };
State state = DISARMED;

WebSocketsClient ws;
String wsPath;
bool wsConnected = false;
volatile int pendingCmd = 0;      // 1 = arm, 2 = disarm (handled in loop)

float baseline = 0, lastCm = 0;
int clears = 0;
unsigned long lastSampleAt = 0;
unsigned long armBeepAt = 0;

void setLeds(bool green, bool red) {
  digitalWrite(GREEN, green);
  digitalWrite(RED, red);
}

float readCm() {
  digitalWrite(TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long d = pulseIn(ECHO, HIGH, (unsigned long)(MAX_RANGE_CM * 58.3));
  if (d == 0) return MAX_RANGE_CM;
  float cm = d * 0.0343 / 2;
  return cm > MAX_RANGE_CM ? MAX_RANGE_CM : cm;
}

// ---------- WebSocket messages ----------
const char* stateName() {
  if (state == DISARMED) return "disarmed";
  if (state == ARMED) return "armed";
  return "alarm";
}

void sendStatus() {
  if (!wsConnected) return;
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"type\":\"status\",\"state\":\"%s\"}", stateName());
  ws.sendTXT(buf);
}

void sendAlert() {
  if (!wsConnected) return;
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"type\":\"alert\",\"distance\":%.1f,\"baseline\":%.1f}", lastCm, baseline);
  ws.sendTXT(buf);
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    wsConnected = true;
    Serial.println("WebSocket connected to server");
    sendStatus();
  } else if (type == WStype_DISCONNECTED) {
    wsConnected = false;
    Serial.print("WebSocket disconnected; WiFi status=");
    Serial.println(WiFi.status());
    if (payload && length) {
      Serial.print("WebSocket disconnect detail: ");
      Serial.write(payload, length);
      Serial.println();
    }
  } else if (type == WStype_ERROR) {
    Serial.println("WebSocket connection error");
    if (payload && length) {
      Serial.print("WebSocket error detail: ");
      Serial.write(payload, length);
      Serial.println();
    }
  } else if (type == WStype_TEXT) {
    char buf[64];
    size_t n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = 0;
    if (strstr(buf, "\"cmd\":\"disarm\"")) pendingCmd = 2;
    else if (strstr(buf, "\"cmd\":\"arm\"")) pendingCmd = 1;
  }
}

// ---------- state changes ----------
void armNow() {
  digitalWrite(BUZZER, LOW);
  setLeds(true, false);                       // green ON
  baseline = readCm();                        // one immediate reading of the empty scene
  Serial.print("Baseline: ");
  Serial.println(baseline);
  clears = 0;
  state = ARMED;
  digitalWrite(BUZZER, HIGH);
  armBeepAt = millis();
  lastSampleAt = millis() - SENSOR_INTERVAL_MS;
  sendStatus();
}

void disarmNow() {
  state = DISARMED;
  armBeepAt = 0;
  digitalWrite(BUZZER, LOW);
  setLeds(false, false);                      // all off
  sendStatus();
}

void triggerAlarm() {
  state = ALARM;
  clears = 0;
  armBeepAt = 0;
  digitalWrite(BUZZER, HIGH);
  setLeds(false, true);                       // red ON
  sendAlert();
  sendStatus();
}

void clearAlarm() {
  state = ARMED;
  digitalWrite(BUZZER, LOW);
  setLeds(true, false);                       // back to green
  sendStatus();
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(RED, OUTPUT);
  pinMode(GREEN, OUTPUT);
  digitalWrite(BUZZER, LOW);
  setLeds(true, false);                       // green ON as soon as it powers on

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("Connecting to WiFi in the background");

  wsPath = String("/device?key=") + DEVICE_KEY + "&loc=" + LOCATION;
  Serial.print("Connecting to wss://");
  Serial.print(SERVER_HOST);
  Serial.print(":");
  Serial.println(SERVER_PORT);
  if (USE_TLS) ws.beginSSL(SERVER_HOST, SERVER_PORT, wsPath.c_str());
  else ws.begin(SERVER_HOST, SERVER_PORT, wsPath.c_str());
  ws.onEvent(onWsEvent);
  ws.setReconnectInterval(3000);
  ws.enableHeartbeat(15000, 3000, 2);

  armNow();
}

void loop() {
  ws.loop();

  if (pendingCmd) {
    int c = pendingCmd;
    pendingCmd = 0;
    if (c == 1 && state == DISARMED) armNow();
    else if (c == 2 && state != DISARMED) disarmNow();
  }

  if (armBeepAt && millis() - armBeepAt >= BEEP_MS) {
    armBeepAt = 0;
    if (state == ARMED) digitalWrite(BUZZER, LOW);
  }

  if (state == DISARMED || (state == ALARM && LATCH_ALARM)) return;
  if (millis() - lastSampleAt < SENSOR_INTERVAL_MS) return;
  lastSampleAt = millis();

  lastCm = readCm();
  bool closer = (lastCm < NEAR_CM) || (lastCm < baseline - CHANGE_CM);

  if (state == ARMED) {
    if (closer) triggerAlarm();               // trigger on the first detection
  } else if (state == ALARM && !LATCH_ALARM) {
    clears = closer ? 0 : clears + 1;
    if (clears >= 2) clearAlarm();
  }

  Serial.print(lastCm);
  Serial.print(" cm  ");
  Serial.println(stateName());
}
