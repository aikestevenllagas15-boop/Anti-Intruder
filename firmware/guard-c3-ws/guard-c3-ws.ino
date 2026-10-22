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
const int SAMPLES = 5;
const int BEEP_MS = 10;           // length of the "ready" beep (same as your code)

enum State { DISARMED, ARMED, ALARM };
State state = DISARMED;

WebSocketsClient ws;
String wsPath;
bool wsConnected = false;
volatile int pendingCmd = 0;      // 1 = arm, 2 = disarm (handled in loop)
bool arming = false;
bool cancelArming = false;

float baseline = 0, lastCm = 0;
int hits = 0, clears = 0;

// ---------- helpers ----------
void servicePendingDisarm();

void waitMs(unsigned long ms) {   // delay that keeps the WebSocket alive
  unsigned long t = millis();
  while (millis() - t < ms) {
    ws.loop();
    servicePendingDisarm();
    delay(2);
  }
}

void setLeds(bool green, bool red) {
  digitalWrite(GREEN, green);
  digitalWrite(RED, red);
}

float readCm() {
  digitalWrite(TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long d = pulseIn(ECHO, HIGH, 38000);
  if (d == 0) return MAX_RANGE_CM;
  float cm = d * 0.0343 / 2;
  return cm > MAX_RANGE_CM ? MAX_RANGE_CM : cm;
}

float readMedian() {              // 5 readings, takes the middle one
  float v[SAMPLES];
  for (int i = 0; i < SAMPLES; i++) { v[i] = readCm(); waitMs(60); }
  for (int i = 1; i < SAMPLES; i++) {
    float x = v[i]; int j = i - 1;
    while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
  return v[SAMPLES / 2];
}

float learnBaseline() {           // waits until two readings agree
  float a = readMedian();
  for (int i = 0; i < 5; i++) {
    float b = readMedian();
    if (fabs(a - b) < 5) return (a + b) / 2;
    a = b;
  }
  return a;
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
  arming = true;
  cancelArming = false;
  digitalWrite(BUZZER, LOW);
  setLeds(true, false);                       // green ON
  baseline = learnBaseline();                 // remembers the empty scene
  arming = false;
  if (cancelArming) return;
  Serial.print("Baseline: ");
  Serial.println(baseline);
  hits = 0; clears = 0;
  state = ARMED;
  digitalWrite(BUZZER, HIGH); waitMs(BEEP_MS); // one short beep = armed
  digitalWrite(BUZZER, LOW);
  sendStatus();
}

void disarmNow() {
  state = DISARMED;
  digitalWrite(BUZZER, LOW);
  setLeds(false, false);                      // all off
  sendStatus();
}

void servicePendingDisarm() {
  if (pendingCmd != 2) return;
  pendingCmd = 0;
  if (state != DISARMED) disarmNow();
  else if (arming) cancelArming = true;
}

void triggerAlarm() {
  state = ALARM;
  clears = 0;
  digitalWrite(BUZZER, HIGH);
  setLeds(false, true);                       // red ON
  sendAlert();
  sendStatus();
}

void clearAlarm() {
  state = ARMED;
  hits = 0;
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
  Serial.print("Connecting to WiFi");
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " connected" : " failed (alarm still works offline)");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
  }

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

  waitMs(3000);                               // step away, keep the area clear
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

  if (state == DISARMED) { delay(20); return; }
  if (state == ALARM && LATCH_ALARM) return;

  lastCm = readMedian();
  bool closer = (lastCm < NEAR_CM) || (lastCm < baseline - CHANGE_CM);

  if (state == ARMED) {
    hits = closer ? hits + 1 : 0;
    if (hits >= 2) triggerAlarm();            // 2 readings in a row, to avoid false alarms
  } else if (state == ALARM && !LATCH_ALARM) {
    clears = closer ? 0 : clears + 1;
    if (clears >= 2) clearAlarm();
  }

  Serial.print(lastCm);
  Serial.print(" cm  ");
  Serial.println(stateName());
}
