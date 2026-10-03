#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Wire.h>
#include <time.h>
#include "DHT.h"
#include "Adafruit_CCS811.h"

// ============================================================================
// NETWORK & FIREBASE CONFIGURATION
// Replace the placeholder values with your actual network and Firebase credentials
// ============================================================================
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Firebase Web API Key & Realtime Database URL
#define API_KEY "YOUR_FIREBASE_API_KEY"
#define DATABASE_URL "YOUR_FIREBASE_DATABASE_URL" // e.g., "https://<your-project>-default-rtdb.firebaseio.com/"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ============================================================================
// HARDWARE PIN DEFINITIONS
// ============================================================================
#define DHTPIN 4
#define DHTTYPE DHT22
#define I2C_SDA 8
#define I2C_SCL 9
#define DUST_LED_PIN 10
#define DUST_ADC_PIN 14
#define RELAY1_PIN 11   // Fan Relay (Active LOW)
#define RELAY2_PIN 12   // Humidifier Relay (Active LOW)

// ============================================================================
// SENSOR OBJECTS
// ============================================================================
DHT dht(DHTPIN, DHTTYPE);
Adafruit_CCS811 ccs;

// ============================================================================
// TIMING INTERVALS (Milliseconds)
// ============================================================================
unsigned long lastControlRead = 0;
unsigned long lastSensorRead = 0;
unsigned long lastLiveUpload = 0;
unsigned long lastHistoryUpload = 0;
unsigned long lastTimeSyncRetry = 0;

const unsigned long CONTROL_READ_INTERVAL = 300;     // Poll control commands every 300ms
const unsigned long SENSOR_READ_INTERVAL = 2000;    // Read sensors every 2s
const unsigned long LIVE_UPLOAD_INTERVAL = 2000;    // Update /iot_data live node every 2s
const unsigned long HISTORY_UPLOAD_INTERVAL = 600000; // Log to /iot_data_history every 10 mins
const unsigned long TIME_SYNC_RETRY_INTERVAL = 60000;
const unsigned long AUTO_TRIGGER_DELAY = 5000;       // Debounce threshold for auto activation (5s)

// ============================================================================
// STATE VARIABLES
// ============================================================================
String autoMode = "ON";
String fanCommand = "OFF";
String humidifierCommand = "OFF";
String fanStatus = "OFF";
String humidifierStatus = "OFF";

float temperature = 0;
float humidity = 0;
int co2 = -1;
int smog = 0;
bool hasValidSensorData = false;
bool timeSynced = false;

// Auto mode trigger timers
unsigned long fanHighSince = 0;
unsigned long lowHumiditySince = 0;

// NTP Time Configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 0;

// ============================================================================
// WIFI & CLOUD SETUP
// ============================================================================
void connectWiFi() {
  Serial.println("[1] Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }

  Serial.println("\n[1] WiFi connected successfully. IP: " + WiFi.localIP().toString());
}

void setupFirebase() {
  Serial.println("[2] Configuring Firebase...");
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("[2] Firebase anonymous signup OK");
  } else {
    Serial.println("[2] Firebase signup failed: " + String(config.signer.signupError.message.c_str()));
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("[2] Firebase initialized");
}

void setupTimeSync() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("[3] Synchronizing NTP time...");

  struct tm timeinfo;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeinfo)) {
      timeSynced = true;
      Serial.println("[3] NTP Time synchronized");
      return;
    }
    delay(500);
    Serial.print(".");
  }

  timeSynced = false;
  Serial.println("\n[3] NTP Time sync failed - will retry in background");
}

bool isTimeValid() {
  time_t now;
  time(&now);
  return now > 100000;
}

void retryTimeSyncIfNeeded() {
  unsigned long nowMs = millis();

  if (timeSynced) return;
  if (nowMs - lastTimeSyncRetry < TIME_SYNC_RETRY_INTERVAL) return;

  lastTimeSyncRetry = nowMs;

  Serial.println("[TIME] Retrying NTP sync...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 3000)) {
    timeSynced = true;
    Serial.println("[TIME] Time synchronized successfully");
  } else {
    Serial.println("[TIME] Retry failed");
  }
}

unsigned long getTimestampMillis() {
  time_t now;
  time(&now);
  if (now <= 1000) return 0;
  return (unsigned long)now * 1000;
}

// ============================================================================
// HARDWARE INITIALIZATION
// ============================================================================
void setupHardware() {
  dht.begin();
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!ccs.begin()) {
    Serial.println("[4] CCS811 CO2 sensor initialization failed!");
  } else {
    Serial.println("[4] CCS811 CO2 sensor initialized");
  }

  pinMode(DUST_LED_PIN, OUTPUT);
  digitalWrite(DUST_LED_PIN, HIGH);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);

  // Active-LOW Relays: HIGH = OFF, LOW = ON
  digitalWrite(RELAY1_PIN, HIGH);
  digitalWrite(RELAY2_PIN, HIGH);
}

// ============================================================================
// FIREBASE COMMAND LISTENER
// ============================================================================
void readControlState() {
  if (!Firebase.RTDB.getJSON(&fbdo, "/iot_data")) {
    return;
  }

  FirebaseJson *json = fbdo.to<FirebaseJson *>();
  FirebaseJsonData result;

  if (json->get(result, "auto_mode") && result.success) {
    String value = result.to<String>();
    if (value == "ON" || value == "OFF") autoMode = value;
  }

  if (json->get(result, "fan_command") && result.success) {
    String value = result.to<String>();
    if (value == "ON" || value == "OFF") fanCommand = value;
  }

  if (json->get(result, "humidifier_command") && result.success) {
    String value = result.to<String>();
    if (value == "ON" || value == "OFF") humidifierCommand = value;
  }
}

// ============================================================================
// SENSOR READINGS
// ============================================================================
void readSensors() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (!isnan(h) && !isnan(t)) {
    humidity = h;
    temperature = t;
    hasValidSensorData = true;
  }

  if (ccs.available() && !ccs.readData()) {
    co2 = ccs.geteCO2();
  }

  // Optical Dust / Smog Sampling
  digitalWrite(DUST_LED_PIN, LOW);
  delayMicroseconds(280);
  smog = analogRead(DUST_ADC_PIN);
  delayMicroseconds(40);
  digitalWrite(DUST_LED_PIN, HIGH);
}

// ============================================================================
// AUTOMATION & RELAY LOGIC
// ============================================================================
void resetAutoTimers() {
  fanHighSince = 0;
  lowHumiditySince = 0;
}

void applyAutoLogicWithDelay() {
  unsigned long now = millis();

  // Fan threshold: CO2 >= 700 ppm sustained for 5s
  if (co2 >= 700) {
    if (fanHighSince == 0) {
      fanHighSince = now;
    }

    if (now - fanHighSince >= AUTO_TRIGGER_DELAY) {
      fanStatus = "ON";
    }
  } else {
    fanHighSince = 0;
    fanStatus = "OFF";
  }

  // Humidifier threshold: Humidity < 50% sustained for 5s
  if (humidity < 50) {
    if (lowHumiditySince == 0) {
      lowHumiditySince = now;
    }

    if (now - lowHumiditySince >= AUTO_TRIGGER_DELAY) {
      humidifierStatus = "ON";
    }
  } else {
    lowHumiditySince = 0;
    humidifierStatus = "OFF";
  }
}

void applyControlLogic() {
  if (autoMode == "ON") {
    applyAutoLogicWithDelay();
  } else {
    resetAutoTimers();
    fanStatus = fanCommand;
    humidifierStatus = humidifierCommand;
  }

  // Active-LOW Relays
  digitalWrite(RELAY1_PIN, fanStatus == "ON" ? LOW : HIGH);
  digitalWrite(RELAY2_PIN, humidifierStatus == "ON" ? LOW : HIGH);
}

// ============================================================================
// DATA UPLOADS
// ============================================================================
void uploadLiveData() {
  FirebaseJson json;
  json.set("temperature", temperature);
  json.set("humidity", humidity);
  json.set("co2", co2);
  json.set("smog", smog);
  json.set("auto_mode", autoMode);
  json.set("fan_status", fanStatus);
  json.set("humidifier_status", humidifierStatus);

  if (Firebase.RTDB.updateNode(&fbdo, "/iot_data", &json)) {
    Serial.println("[LIVE] /iot_data updated successfully");
  } else {
    Serial.println("[LIVE] Upload failed: " + fbdo.errorReason());
  }
}

void uploadHistoryData() {
  FirebaseJson historyJson;
  historyJson.set("temperature", temperature);
  historyJson.set("humidity", humidity);
  historyJson.set("co2", co2);
  historyJson.set("timestamp", getTimestampMillis());
  historyJson.set("time_valid", isTimeValid());

  if (Firebase.RTDB.pushJSON(&fbdo, "/iot_data_history", &historyJson)) {
    Serial.println("[HISTORY] /iot_data_history record appended");
  } else {
    Serial.println("[HISTORY] Upload failed: " + fbdo.errorReason());
  }
}

// ============================================================================
// MAIN SETUP & LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  connectWiFi();
  setupFirebase();
  setupTimeSync();
  setupHardware();
}

void loop() {
  unsigned long now = millis();

  retryTimeSyncIfNeeded();

  // 1. Poll incoming control commands from Flutter / Firebase
  if (now - lastControlRead >= CONTROL_READ_INTERVAL) {
    lastControlRead = now;
    readControlState();
    applyControlLogic();
  }

  // 2. Sample physical sensors
  if (now - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = now;
    readSensors();
    applyControlLogic();
  }

  // 3. Stream real-time telemetry to Firebase
  if (hasValidSensorData && now - lastLiveUpload >= LIVE_UPLOAD_INTERVAL) {
    lastLiveUpload = now;
    uploadLiveData();
  }

  // 4. Periodically append historical telemetry for analytics/charts
  if (hasValidSensorData && now - lastHistoryUpload >= HISTORY_UPLOAD_INTERVAL) {
    lastHistoryUpload = now;
    uploadHistoryData();
  }
}
