/*
 * ESP32 + Honeywell HPMA115 Compact + Adafruit IO
 *
 * - Reads PM1.0, PM2.5, PM4.0 and PM10 every 10 seconds
 * - Calculates five-minute average PM concentrations
 * - Calculates AQI from averaged PM2.5 and PM10 concentrations
 * - Publishes PM averages and overall AQI to Adafruit IO
 * - Publishes system events to the "log" feed
 * - Queues log messages while Adafruit IO is unavailable
 * - Reports the ESP32 reset reason after connecting
 *
 * Required libraries:
 *   1. PMSensor-HPMA115
 *   2. Adafruit IO Arduino
 *
 * Adafruit IO feed keys:
 *   pm1, pm25, pm4, pm10, aqi, log
 */

#include <WiFi.h>
#include <esp_system.h>
#include "AdafruitIO_WiFi.h"
#include "HPMA115_Compact.h"
#include "aqi/aqi.h"
#include "secrets.h"

// ============================================================
// USER CONFIGURATION in secrets.h
// Replace these placeholders before uploading.
// ============================================================
// #define WIFI_SSID   "YOUR_WIFI_SSID"
// #define WIFI_PASS   "YOUR_WIFI_PASSWORD"
// #define IO_USERNAME "YOUR_ADAFRUIT_IO_USERNAME"
// #define IO_KEY      "YOUR_NEW_ADAFRUIT_IO_KEY"

#define HPMA_RX_PIN 23
#define HPMA_TX_PIN 22
#define STATUS_LED_PIN 21

// ============================================================
// TIMING CONFIGURATION
// ============================================================
static const uint32_t SENSOR_INTERVAL_MS = 10000UL;
static const uint32_t UPLOAD_INTERVAL_MS = 300000UL;
static const uint32_t CONNECTION_WARNING_INTERVAL_MS = 60000UL;
static const uint32_t SENSOR_RESPONSE_TIMEOUT_MS = 3000UL;
static const uint32_t LED_BLINK_TIME_MS = 1000UL;
static const uint32_t LED_ERROR_BLINK_INTERVAL_MS = 200UL;

// ============================================================
// ADAFRUIT IO OBJECTS
// ============================================================
AdafruitIO_WiFi io(IO_USERNAME, IO_KEY, WIFI_SSID, WIFI_PASS);
AdafruitIO_Feed *pm1Feed  = io.feed("pm1");
AdafruitIO_Feed *pm25Feed = io.feed("pm25");
AdafruitIO_Feed *pm4Feed  = io.feed("pm4");
AdafruitIO_Feed *pm10Feed = io.feed("pm10");
AdafruitIO_Feed *aqiFeed  = io.feed("aqi");
AdafruitIO_Feed *logFeed  = io.feed("log");

// ============================================================
// HPMA115 OBJECT
// ============================================================
HPMA115_Compact hpm;

// ============================================================
// MEASUREMENT ACCUMULATORS
// ============================================================
uint64_t pm1Sum = 0;
uint64_t pm25Sum = 0;
uint64_t pm4Sum = 0;
uint64_t pm10Sum = 0;
uint32_t sampleCount = 0;
uint32_t failedSampleCount = 0;

// ============================================================
// SCHEDULER AND STATE
// ============================================================
uint32_t lastSensorReadMs = 0;
uint32_t lastUploadMs = 0;
uint32_t lastConnectionWarningMs = 0;

bool sensorInitialized = false;
bool previouslyConnected = false;
bool connectionWasLost = false;
bool firstCloudConnection = true;

bool ledBlinkActive = false;
bool ledState = false;
uint32_t ledBlinkStartMs = 0;
uint32_t lastErrorBlinkMs = 0;

// ============================================================
// OFFLINE LOG QUEUE
// ============================================================
static const uint8_t LOG_QUEUE_SIZE = 10;
String logQueue[LOG_QUEUE_SIZE];
uint8_t logQueueHead = 0;
uint8_t logQueueTail = 0;
uint8_t logQueueCount = 0;

// ============================================================
// FUNCTION DECLARATIONS
// ============================================================
void initializeSensor();
void readHPMA115();
void publishAverage();
void resetAccumulators();
void blinkStatusLED();
void updateStatusLED();
void sendLog(const String &message);
void queueLog(const String &message);
void flushLogQueue();
void monitorConnection();
bool cloudConnected();
String getResetReasonText();
String makeMeasurementLog(float avgPM1, float avgPM25, float avgPM4,
                          float avgPM10, uint32_t avgAQI);

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("HPMA115 Compact Air Quality Monitor");
  Serial.println("========================================");

  sendLog("BOOT: Device booting");
  sendLog("BOOT: Reset reason: " + getResetReasonText());

  initializeSensor();

  Serial.println();
  Serial.println("Connecting to Wi-Fi and Adafruit IO...");
  io.connect();

  uint32_t dotTimer = millis();
  while (io.status() < AIO_CONNECTED) {
    io.run();
    updateStatusLED();
    if (millis() - dotTimer >= 500UL) {
      dotTimer = millis();
      Serial.print(".");
    }
    delay(10);
  }

  Serial.println();
  Serial.print("Connected: ");
  Serial.println(io.statusText());

  previouslyConnected = true;
  firstCloudConnection = false;
  flushLogQueue();
  sendLog("NETWORK: Wi-Fi connected");
  sendLog("NETWORK: IP address " + WiFi.localIP().toString());
  sendLog("CLOUD: Adafruit IO connected");

  if (sensorInitialized) {
    sendLog("SENSOR: HPMA115 initialized successfully");
  } else {
    sendLog("CRITICAL: HPMA115 initialization failed");
  }

  resetAccumulators();
  lastSensorReadMs = millis();
  lastUploadMs = millis();
  sendLog("SYSTEM: Monitoring started; sample interval=10s, upload interval=5min");
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  io.run();
  monitorConnection();
  updateStatusLED();

  const uint32_t currentTimeMs = millis();

  if (currentTimeMs - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    lastSensorReadMs += SENSOR_INTERVAL_MS;
    readHPMA115();
  }

  if (currentTimeMs - lastUploadMs >= UPLOAD_INTERVAL_MS) {
    lastUploadMs += UPLOAD_INTERVAL_MS;
    publishAverage();
  }
}

// ============================================================
// SENSOR INITIALIZATION
// ============================================================
void initializeSensor() {
  Serial.println();
  Serial.println("Initializing HPMA115 Compact sensor...");

  Serial2.begin(9600, SERIAL_8N1, HPMA_RX_PIN, HPMA_TX_PIN);
  Serial2.setTimeout(SENSOR_RESPONSE_TIMEOUT_MS);
  hpm.begin(&Serial2);
  delay(1000);

  const uint32_t startupTimeMs = millis();
  while (millis() - startupTimeMs < 10000UL) {
    if (hpm.isNewDataAvailable()) {
      sensorInitialized = true;
      break;
    }
    updateStatusLED();
    delay(100);
  }

  if (sensorInitialized) {
    if (!hpm.stopAutoSend()) {
      Serial.println("WARNING: Failed to disable HPMA115 auto-send mode");
      sendLog("WARNING: Failed to disable HPMA115 auto-send mode");
    } else {
      Serial.println("HPMA115 initialized; automatic transmission disabled");
      sendLog("SENSOR: HPMA115 detected during startup");
    }
  } else {
    Serial.println("ERROR: HPMA115 startup timeout");
    sendLog("CRITICAL: HPMA115 startup timeout");
  }
}

// ============================================================
// READ SENSOR
// ============================================================
void readHPMA115() {
  if (!sensorInitialized) {
    failedSampleCount++;
    Serial.println("ERROR: Sensor read skipped because initialization failed");
    return;
  }

  if (hpm.readParticleMeasurementResults()) {
    const uint16_t pm1 = hpm.getPM1();
    const uint16_t pm25 = hpm.getPM25();
    const uint16_t pm4 = hpm.getPM4();
    const uint16_t pm10 = hpm.getPM10();
    const uint16_t instantaneousAQI = hpm.getAQI();

    blinkStatusLED();

    pm1Sum += pm1;
    pm25Sum += pm25;
    pm4Sum += pm4;
    pm10Sum += pm10;
    sampleCount++;

    Serial.println();
    Serial.print("Valid sample #");
    Serial.println(sampleCount);
    Serial.print("PM1.0 : ");
    Serial.print(pm1);
    Serial.println(" ug/m3");
    Serial.print("PM2.5 : ");
    Serial.print(pm25);
    Serial.println(" ug/m3");
    Serial.print("PM4.0 : ");
    Serial.print(pm4);
    Serial.println(" ug/m3");
    Serial.print("PM10  : ");
    Serial.print(pm10);
    Serial.println(" ug/m3");
    Serial.print("Instantaneous AQI: ");
    Serial.println(instantaneousAQI);
  } else {
    failedSampleCount++;
    Serial.println();
    Serial.println("ERROR: HPMA115 measurement request failed");

    if (failedSampleCount == 1) {
      sendLog("WARNING: HPMA115 measurement request failed");
    }
  }
}

// ============================================================
// CALCULATE AND PUBLISH FIVE-MINUTE AVERAGE
// ============================================================
void publishAverage() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("FIVE-MINUTE MEASUREMENT WINDOW");
  Serial.println("========================================");

  if (sampleCount == 0) {
    Serial.println("No valid samples available; measurement upload skipped");
    sendLog("CRITICAL: No valid HPMA115 samples in 5-minute window; failed reads=" +
            String(failedSampleCount));
    resetAccumulators();
    return;
  }

  const float avgPM1 = static_cast<float>(pm1Sum) / sampleCount;
  const float avgPM25 = static_cast<float>(pm25Sum) / sampleCount;
  const float avgPM4 = static_cast<float>(pm4Sum) / sampleCount;
  const float avgPM10 = static_cast<float>(pm10Sum) / sampleCount;

  // The AQI library accepts integer concentrations. Round each five-minute
  // average to the nearest whole ug/m3, calculate both sub-indices, and use
  // the worse pollutant as the overall AQI, matching the sensor library.
  const uint16_t avgPM25ForAQI = static_cast<uint16_t>(avgPM25 + 0.5f);
  const uint16_t avgPM10ForAQI = static_cast<uint16_t>(avgPM10 + 0.5f);
  const uint32_t aqi25 = aqi_pm25(avgPM25ForAQI);
  const uint32_t aqi10 = aqi_pm10(avgPM10ForAQI);
  const uint32_t avgAQI = (aqi25 > aqi10) ? aqi25 : aqi10;

  Serial.print("Valid samples  : ");
  Serial.println(sampleCount);
  Serial.print("Failed samples : ");
  Serial.println(failedSampleCount);
  Serial.print("Average PM1.0  : ");
  Serial.print(avgPM1, 2);
  Serial.println(" ug/m3");
  Serial.print("Average PM2.5  : ");
  Serial.print(avgPM25, 2);
  Serial.println(" ug/m3");
  Serial.print("Average PM4.0  : ");
  Serial.print(avgPM4, 2);
  Serial.println(" ug/m3");
  Serial.print("Average PM10   : ");
  Serial.print(avgPM10, 2);
  Serial.println(" ug/m3");
  Serial.print("PM2.5 AQI      : ");
  Serial.println(aqi25);
  Serial.print("PM10 AQI       : ");
  Serial.println(aqi10);
  Serial.print("Overall AQI    : ");
  Serial.println(avgAQI);

  if (cloudConnected()) {
    pm1Feed->save(avgPM1);
    pm25Feed->save(avgPM25);
    pm4Feed->save(avgPM4);
    pm10Feed->save(avgPM10);
    aqiFeed->save(avgAQI);

    sendLog(makeMeasurementLog(avgPM1, avgPM25, avgPM4, avgPM10, avgAQI));
    Serial.println("Five-minute PM averages and AQI submitted to Adafruit IO");
  } else {
    sendLog("CRITICAL: Measurement upload skipped because Adafruit IO is disconnected; valid samples=" +
            String(sampleCount));
    Serial.println("ERROR: Cloud disconnected; average not uploaded");
  }

  resetAccumulators();
}

// ============================================================
// CREATE MEASUREMENT LOG MESSAGE
// ============================================================
String makeMeasurementLog(float avgPM1, float avgPM25, float avgPM4,
                          float avgPM10, uint32_t avgAQI) {
  String message;
  message.reserve(220);
  message = "UPLOAD: 5-minute average submitted; valid=" + String(sampleCount) +
            ", failed=" + String(failedSampleCount) +
            ", PM1=" + String(avgPM1, 2) +
            ", PM2.5=" + String(avgPM25, 2) +
            ", PM4=" + String(avgPM4, 2) +
            ", PM10=" + String(avgPM10, 2) +
            ", AQI=" + String(avgAQI);
  return message;
}

void resetAccumulators() {
  pm1Sum = 0;
  pm25Sum = 0;
  pm4Sum = 0;
  pm10Sum = 0;
  sampleCount = 0;
  failedSampleCount = 0;
}

// ============================================================
// CONNECTION MONITOR
// ============================================================
void monitorConnection() {
  const bool connectedNow = cloudConnected();

  if (!connectedNow && previouslyConnected) {
    previouslyConnected = false;
    connectionWasLost = true;
    lastConnectionWarningMs = millis();
    sendLog("CRITICAL: Wi-Fi or Adafruit IO connection lost");
  }

  if (connectedNow && !previouslyConnected) {
    previouslyConnected = true;
    Serial.println();
    Serial.println("Wi-Fi and Adafruit IO connection restored");
    flushLogQueue();

    if (connectionWasLost) {
      sendLog("NETWORK: Wi-Fi and Adafruit IO connection restored");
      sendLog("NETWORK: IP address " + WiFi.localIP().toString());
      connectionWasLost = false;
    } else if (!firstCloudConnection) {
      sendLog("NETWORK: Adafruit IO connection established");
    }
    firstCloudConnection = false;
  }

  if (!connectedNow &&
      millis() - lastConnectionWarningMs >= CONNECTION_WARNING_INTERVAL_MS) {
    lastConnectionWarningMs = millis();
    Serial.println("WARNING: Wi-Fi/Adafruit IO is still disconnected");
  }
}

bool cloudConnected() {
  return WiFi.status() == WL_CONNECTED && io.status() >= AIO_CONNECTED;
}

void sendLog(const String &message) {
  Serial.print("[LOG] ");
  Serial.println(message);

  if (cloudConnected()) {
    logFeed->save(message);
  } else {
    queueLog(message);
  }
}

void queueLog(const String &message) {
  if (logQueueCount >= LOG_QUEUE_SIZE) {
    logQueue[logQueueHead] = "";
    logQueueHead = (logQueueHead + 1) % LOG_QUEUE_SIZE;
    logQueueCount--;
  }

  logQueue[logQueueTail] = message;
  logQueueTail = (logQueueTail + 1) % LOG_QUEUE_SIZE;
  logQueueCount++;
}

void flushLogQueue() {
  if (!cloudConnected()) {
    return;
  }

  while (logQueueCount > 0) {
    String queuedMessage = logQueue[logQueueHead];
    logQueue[logQueueHead] = "";
    logQueueHead = (logQueueHead + 1) % LOG_QUEUE_SIZE;
    logQueueCount--;
    logFeed->save(queuedMessage);
    io.run();
    delay(100);
  }
}

// ============================================================
// ESP32 RESET REASON
// ============================================================
String getResetReasonText() {
  const esp_reset_reason_t resetReason = esp_reset_reason();

  switch (resetReason) {
    case ESP_RST_POWERON:   return "Power-on reset";
    case ESP_RST_EXT:       return "External reset";
    case ESP_RST_SW:        return "Software restart";
    case ESP_RST_PANIC:     return "Software exception or panic";
    case ESP_RST_INT_WDT:   return "Interrupt watchdog reset";
    case ESP_RST_TASK_WDT:  return "Task watchdog reset";
    case ESP_RST_WDT:       return "Watchdog reset";
    case ESP_RST_DEEPSLEEP: return "Wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "Brownout reset";
    case ESP_RST_SDIO:      return "SDIO reset";
    case ESP_RST_UNKNOWN:
    default:                return "Unknown reset reason";
  }
}

// ============================================================
// STATUS LED
// ============================================================
void blinkStatusLED() {
  if (!sensorInitialized) {
    return;
  }

  digitalWrite(STATUS_LED_PIN, HIGH);
  ledBlinkStartMs = millis();
  ledBlinkActive = true;
}

void updateStatusLED() {
  const uint32_t currentTimeMs = millis();

  if (!sensorInitialized) {
    if (currentTimeMs - lastErrorBlinkMs >= LED_ERROR_BLINK_INTERVAL_MS) {
      lastErrorBlinkMs = currentTimeMs;
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState ? HIGH : LOW);
    }
    return;
  }

  if (ledBlinkActive &&
      currentTimeMs - ledBlinkStartMs >= LED_BLINK_TIME_MS) {
    digitalWrite(STATUS_LED_PIN, LOW);
    ledBlinkActive = false;
    ledState = false;
  }
}
