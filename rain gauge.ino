#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Stepper.h>

// --- WiFi ---
const char* ssid     = "";
const char* password = "";

// --- MQTT: weather data (subscribe, port 1883, no auth) ---

// EDIT BELOW FOR YOUR MQTT BROKER 
const char* mqttServer = "";
const int   mqttPort   = ;
const char* mqttTopic  = "/loop";

// BELOW PUBLISHES AN ALIVE MESSAGE SO YOU KNOW ITS WORKING

// --- MQTT: heartbeat (publish, port 1884, with auth) ---
const int   mqttHBPort  = ;
const char* mqttHBUser  = "";
const char* mqttHBPass  = ";
const char* mqttHBTopic = "";

const float MIN_RAIN_DELTA_MM = 0.2f;
const int   MAX_TURNS_PER_MSG = 20;

// --- Stepper: D1=GPIO5, D2=GPIO4, D5=GPIO14, D6=GPIO12 ---
const int stepsPerRev = 2068;
Stepper myStepper(stepsPerRev, 5, 14, 4, 12);

WiFiClient   espClient;
PubSubClient client(espClient);

WiFiClient   hbEspClient;
PubSubClient hbClient(hbEspClient);

float         lastRainMM     = -1.0f;
unsigned long lastTurnMillis = 0;
bool          hasTurned      = false;
int           pendingTurns   = 0;

// -------------------------------------------------------

void cutPower() {
  digitalWrite(5,  LOW);
  digitalWrite(14, LOW);
  digitalWrite(4,  LOW);
  digitalWrite(12, LOW);
}

void stepWithWatchdog(int steps) {
  int count = abs(steps);
  int dir   = (steps > 0) ? 1 : -1;
  for (int i = 0; i < count; i++) {
    myStepper.step(-dir);
    if (i % 8 == 0) yield();
  }
}

void executeOneTurn() {
  stepWithWatchdog(stepsPerRev);
  cutPower();
  delay(300);
  lastTurnMillis = millis();
  hasTurned      = true;
}

float extractFloat(const String& json, const char* key) {
  String search = "\"";
  search += key;
  search += "\":";
  int idx = json.indexOf(search);
  if (idx == -1) return -999.0f;
  idx += search.length();
  while (idx < (int)json.length() && json.charAt(idx) == ' ') idx++;
  if (idx < (int)json.length() && json.charAt(idx) == '"') idx++;
  return json.substring(idx).toFloat();
}

// -------------------------------------------------------

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String json;
  json.reserve(length);
  for (unsigned int i = 0; i < length; i++) json += (char)payload[i];

  Serial.println("Payload received (" + String(length) + " bytes)");

  float currentRain = extractFloat(json, "dayRain_mm");

  if (currentRain <= -999.0f) {
    Serial.println("ERROR: dayRain_mm not found in payload");
    return;
  }

  Serial.print("dayRain_mm = ");
  Serial.println(currentRain);

  // --- THE ONLY CHANGES ARE BELOW THIS LINE ---

  if (lastRainMM < 0.0f) {
    lastRainMM = currentRain;
    Serial.println("First reading stored: " + String(currentRain));
    return;
  }

  // Midnight reset handler
  if (currentRain < lastRainMM) {
    lastRainMM = currentRain;
    Serial.println("Midnight reset detected.");
    return;
  }

  float diff = currentRain - lastRainMM;

  // Use 0.19f to handle slight float precision errors with 0.2
  if (diff >= 0.19f) {
    int turns = (int)(diff / 0.2f);
    
    if (turns > MAX_TURNS_PER_MSG) {
      Serial.print("WARN: clamped turns from ");
      Serial.print(turns);
      Serial.print(" to ");
      Serial.println(MAX_TURNS_PER_MSG);
      turns = MAX_TURNS_PER_MSG;
    }
    
    Serial.print("Rain +");
    Serial.print(diff);
    Serial.print("mm -> queuing ");
    Serial.print(turns);
    Serial.println(" turn(s)");
    
    pendingTurns += turns;
    
    // Accumulator: Only add what we used, keep the remainder
    lastRainMM += (turns * 0.2f);

  } else if (diff > 0.0f) {
    Serial.print("Sub-threshold ");
    Serial.print(diff);
    Serial.println("mm — holding until threshold reached");
  }
}

// -------------------------------------------------------

bool reconnectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.print("WiFi lost — reconnecting");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" ok. IP: " + WiFi.localIP().toString());
    return true;
  }
  Serial.println(" failed.");
  return false;
}

bool reconnectMQTT() {
  if (client.connected()) return true;
  String id = "RainGauge_" + String(ESP.getChipId(), HEX);
  Serial.print("MQTT (data) reconnecting...");
  if (client.connect(id.c_str())) {
    client.subscribe(mqttTopic);
    Serial.println("ok, resubscribed");
    return true;
  }
  Serial.print("failed rc=");
  Serial.println(client.state());
  return false;
}

bool reconnectHB() {
  if (hbClient.connected()) return true;
  String id = "RainGauge_HB_" + String(ESP.getChipId(), HEX);
  Serial.print("MQTT (HB) reconnecting...");
  if (hbClient.connect(id.c_str(), mqttHBUser, mqttHBPass)) {
    Serial.println("ok");
    return true;
  }
  Serial.print("failed rc=");
  Serial.println(hbClient.state());
  return false;
}

void formatHMS(unsigned long totalSec, char* buf, size_t bufLen) {
  unsigned long h = totalSec / 3600;
  unsigned long m = (totalSec % 3600) / 60;
  unsigned long s = totalSec % 60;
  snprintf(buf, bufLen, "%luh %02lum %02lus", h, m, s);
}

void publishHeartbeat() {
  if (!reconnectHB()) return;

  unsigned long uptimeSec = millis() / 1000;
  char uptime[24];
  formatHMS(uptimeSec, uptime, sizeof(uptime));

  char msg[200];
  if (hasTurned) {
    char lastTurn[24];
    formatHMS((millis() - lastTurnMillis) / 1000, lastTurn, sizeof(lastTurn));
    snprintf(msg, sizeof(msg),
      "{\"alive\":true,\"uptime\":\"%s\",\"rssi\":%d,\"lastRain_mm\":%.2f"
      ",\"lastTurn_ago\":\"%s\",\"pendingTurns\":%d}",
      uptime, WiFi.RSSI(), lastRainMM, lastTurn, pendingTurns);
  } else {
    snprintf(msg, sizeof(msg),
      "{\"alive\":true,\"uptime\":\"%s\",\"rssi\":%d,\"lastRain_mm\":%.2f"
      ",\"lastTurn_ago\":null,\"pendingTurns\":%d}",
      uptime, WiFi.RSSI(), lastRainMM, pendingTurns);
  }

  if (hbClient.publish(mqttHBTopic, msg)) {
    Serial.println("HB: " + String(msg));
  } else {
    Serial.println("HB publish failed");
  }
}

// -------------------------------------------------------

void setup() {
  Serial.begin(9600);
  delay(3000);
  Serial.println("=== Ball Clock Rain Gauge ===");

  myStepper.setSpeed(2);

  client.setBufferSize(2048);
  client.setKeepAlive(300);

  hbClient.setBufferSize(512);
  hbClient.setKeepAlive(120);

  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 40) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nCannot connect to WiFi — rebooting in 5s");
    delay(5000);
    ESP.restart();
  }
  Serial.println(" ok. IP: " + WiFi.localIP().toString());

  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);
  String dataId = "RainGauge_" + String(ESP.getChipId(), HEX);
  while (!client.connected()) {
    Serial.print("MQTT (data)...");
    if (client.connect(dataId.c_str())) {
      Serial.println("ok");
      client.subscribe(mqttTopic);
      Serial.println("Subscribed to: " + String(mqttTopic));
    } else {
      Serial.print("failed rc=");
      Serial.print(client.state());
      Serial.println(" — retry in 3s");
      delay(3000);
    }
  }

  hbClient.setServer(mqttServer, mqttHBPort);
  reconnectHB();

  Serial.println("=== Setup complete, listening ===");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi dropped!");
    client.disconnect();
    hbClient.disconnect();
    if (!reconnectWiFi()) { delay(5000); return; }
  }

  if (hbClient.connected()) hbClient.loop();

  if (!client.connected()) {
    if (!reconnectMQTT()) { delay(5000); return; }
  }
  client.loop();

  if (pendingTurns > 0) {
    Serial.print("Turning (");
    Serial.print(pendingTurns);
    Serial.println(" remaining)");
    executeOneTurn();
    pendingTurns--;
  }

  static unsigned long lastHB = 0;
  if (millis() - lastHB >= 60000UL) {
    publishHeartbeat();
    lastHB = millis();
  }
}
