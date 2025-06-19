// KambingPRO ESP32 - Arduino Cloud + Google Sheet Hourly Logger (Time-Synced, Power-Aware)
// Aligned to NTP (GMT+8). Posts averaged hourly data to Google Sheet via Apps Script Webhook.
// Aman & Anna, 2025-06-10 (Revised 2025-06-19 for best practices)

#include "thingProperties.h" // Contains Arduino Cloud variable declarations and connection logic
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <time.h>            // For NTP time synchronization
#include <WiFiClientSecure.h>  // For HTTPS communication
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>     // For creating JSON payloads

// ==== Project Configuration ====
const char* THING_UID_NAME = "RAB001"; // Unique identifier for this device/thing

// ---- Google Apps Script Webhook ----
const char* GOOGLE_SHEET_WEBHOOK_URL = "https://script.google.com/macros/s/AKfycbxaygP3nPks_jBGWjEhmRce7UESrxxHb1cGK65Nhnxpc4L663tCWeSaVKkdExZya0oc/exec";
const char* GOOGLE_SCRIPT_HOST = "script.google.com";
const int   GOOGLE_SCRIPT_PORT = 443; // HTTPS port

// ---- NTP Configuration ----
const long  GMT_OFFSET_SECONDS    = 8 * 3600; // GMT+8 for Kuala Terengganu
const int   DAYLIGHT_OFFSET_SECONDS = 0;
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";
const char* NTP_SERVER_3 = "sg.pool.ntp.org";
const unsigned long NTP_SYNC_INTERVAL_MS = 12UL * 3600UL * 1000UL; // Sync every 12 hours
const int NTP_SYNC_MAX_TRIES = 20;
const int NTP_SYNC_RETRY_DELAY_MS = 500;

// ---- Hardware Pin Definitions ----
const int RELAY_PUMP_PIN      = 5;
const int RELAY_AUX_PIN       = 25;
const int RELAY_CCTV_PIN      = 33;
const int RELAY_SIREN_PIN     = 32;
const int DHT_SENSOR_PIN      = 15;
const int ULTRASONIC_TRIG_PIN = 13;
const int ULTRASONIC_ECHO_PIN = 14;
const int MQ137_ANALOG_PIN    = 34; // ESP32 ADC1 pin

// ---- Sensor Specifics ----
#define DHT_SENSOR_TYPE DHT22
const float MQ137_LOAD_RESISTOR_KOHM = 22.0f; // kOhms
const float ADC_VOLTAGE_REFERENCE    = 3.3f;  // ESP32 ADC reference voltage
const float ADC_MAX_VALUE            = 4095.0f; // ESP32 12-bit ADC
const float MQ137_AMMONIA_OFFSET_PPM   = 7.0f;
const float MQ137_AMMONIA_SCALING_DIV  = 10.0f;

const unsigned long ULTRASONIC_TIMEOUT_US  = 30000UL; // Microseconds
const float SPEED_OF_SOUND_CM_PER_US = 0.0343f;

// ---- Tank Geometry (Frustum Shape) ----
const float TANK_HEIGHT_CM        = 38.0f;
const float TANK_RADIUS_TOP_CM    = 18.5f;
const float TANK_RADIUS_BOTTOM_CM = 14.0f;
const float TANK_MAX_VOLUME_LITERS = (M_PI * TANK_HEIGHT_CM / 3.0f) *
                                     (TANK_RADIUS_TOP_CM * TANK_RADIUS_TOP_CM +
                                      TANK_RADIUS_TOP_CM * TANK_RADIUS_BOTTOM_CM +
                                      TANK_RADIUS_BOTTOM_CM * TANK_RADIUS_BOTTOM_CM) / 1000.0f;

// ---- Data Sampling & Averaging ----
// IMPROVEMENT: Using constexpr for a true compile-time constant.
constexpr int SAMPLES_PER_HOUR = 6; // One sample every 10 minutes for an hour
constexpr int SAMPLING_INTERVAL_MINUTES = 10;
float hourlyAmmoniaSamples[SAMPLES_PER_HOUR];
float hourlyTemperatureSamples[SAMPLES_PER_HOUR];
float hourlyHumiditySamples[SAMPLES_PER_HOUR];
float hourlyStorageTankSamples[SAMPLES_PER_HOUR];
int   currentHourlySampleCount = 0;

// ---- Global Objects ----
LiquidCrystal_I2C lcd(0x27, 16, 2); // I2C address 0x27, 16 column and 2 rows
DHT dht(DHT_SENSOR_PIN, DHT_SENSOR_TYPE);

WiFiClientSecure clientSecure; // Use WiFiClientSecure for HTTPS
HttpClient googleSheetsClient = HttpClient(clientSecure, GOOGLE_SCRIPT_HOST, GOOGLE_SCRIPT_PORT);

// ---- Timing & State Variables ----
unsigned long lastNtpSyncMillis        = 0;
unsigned long pumpTurnedOnMillis       = 0;
// CRITICAL FIX: This constant was used in the loop but never declared.
const long PUMP_ON_DURATION_MS         = 20000L; // 20 seconds auto-off for pump

// IMPROVEMENT: State variables for robust, non-blocking timed events
int lastSampleMinute = -1;
int lastReportHour   = -1;


// --- Forward declarations for refactored functions ---
void handleSensorsAndDisplay();
void handleHourlyTasks(unsigned long currentMillis);
void readAllSensors();
void updateLcd();
void handleDataSampling(const tm& timeinfo);
void handleHourlyReporting(const tm& timeinfo);
void postToGoogleSheets();


// =======================================================================================
//                                  SETUP FUNCTION
// =======================================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000); // Wait for serial port to connect
  Serial.println("KambingPRO ESP32 Initializing...");

  // --- Initialize Relays (Output, Low initial state) ---
  pinMode(RELAY_PUMP_PIN, OUTPUT); digitalWrite(RELAY_PUMP_PIN, LOW);
  pinMode(RELAY_AUX_PIN, OUTPUT);  digitalWrite(RELAY_AUX_PIN, LOW);
  pinMode(RELAY_CCTV_PIN, OUTPUT); digitalWrite(RELAY_CCTV_PIN, LOW);
  pinMode(RELAY_SIREN_PIN, OUTPUT);digitalWrite(RELAY_SIREN_PIN, LOW);

  // --- Initialize Sensors ---
  pinMode(ULTRASONIC_TRIG_PIN, OUTPUT);
  pinMode(ULTRASONIC_ECHO_PIN, INPUT);
  dht.begin();

  // --- Initialize LCD ---
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("KambingPRO IoT");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");

  // --- Initialize Arduino Cloud ---
  initProperties();
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  setDebugMessageLevel(2);
  ArduinoCloud.printDebugInfo();

  Serial.println("Waiting for Arduino Cloud connection...");
  // IMPROVEMENT: Using the more common boolean check idiom.
  while (!ArduinoCloud.connected()) {
    ArduinoCloud.update();
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nArduino Cloud connected!");

  // --- Time Synchronization ---
  synchronizeNTPTime();

  // --- Initialize Data Storage ---
  clearHourlySampleArrays();
  // Initialize state variables with current time info
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  lastReportHour = timeinfo.tm_hour;
  lastSampleMinute = timeinfo.tm_min;

  // --- Final LCD Ready Message ---
  lcd.clear();
  lcd.print(THING_UID_NAME);
  lcd.setCursor(0,1);
  lcd.print("System Ready");
  Serial.println("System Initialization Complete.");
}

// =======================================================================================
//                                   MAIN LOOP
// =======================================================================================
// IMPROVEMENT: The loop is now much cleaner and delegates tasks to other functions.
void loop() {
  unsigned long currentMillis = millis();

  ArduinoCloud.update(); // Essential for Arduino Cloud functionality

  // --- Periodic NTP Time Synchronization ---
  if (currentMillis - lastNtpSyncMillis > NTP_SYNC_INTERVAL_MS || lastNtpSyncMillis == 0) {
    synchronizeNTPTime();
    lastNtpSyncMillis = currentMillis;
  }

  handleSensorsAndDisplay();
  handleHourlyTasks(currentMillis);
  
  // ---------- Pump Auto-Off Timer Logic ----------
  if (storagePump && (currentMillis - pumpTurnedOnMillis >= PUMP_ON_DURATION_MS)) {
    Serial.println("Pump -> OFF (20s timer elapsed)");
    digitalWrite(RELAY_PUMP_PIN, LOW); // Turn off the relay
    storagePump = false; // IMPORTANT: Update the cloud variable
  }
  
  // A small non-blocking delay is acceptable to prevent a tight loop from starving other tasks.
  delay(200);
}


// =======================================================================================
//                           REFACTORED HELPER FUNCTIONS
// =======================================================================================

/**
 * @brief Main handler for all sensor reads and LCD updates.
 */
void handleSensorsAndDisplay() {
  // This function can be expanded with its own timer to avoid reading sensors
  // on every single loop, e.g., only read every 2 seconds.
  // For now, it runs on every loop pass (every ~200ms).
  readAllSensors();
  updateLcd();
}

/**
 * @brief Reads all connected sensors and updates their respective cloud variables.
 */
void readAllSensors() {
  // DHT22 Temperature & Humidity
  float currentTemperature = dht.readTemperature();
  float currentHumidity    = dht.readHumidity();
  if (!isnan(currentTemperature)) temperature = currentTemperature;
  if (!isnan(currentHumidity))    humidity    = currentHumidity;

  // MQ-137 Ammonia Sensor
  int mq137_raw_adc = analogRead(MQ137_ANALOG_PIN);
  float mq137_voltage_rl = mq137_raw_adc * (ADC_VOLTAGE_REFERENCE / ADC_MAX_VALUE);
  float mq137_rs_kohm = (mq137_voltage_rl > 0.001f)
      ? (ADC_VOLTAGE_REFERENCE - mq137_voltage_rl) * MQ137_LOAD_RESISTOR_KOHM / mq137_voltage_rl
      : 100000.0f; // Assign high resistance if voltage is near zero
  ammonia = max(0.0f, MQ137_AMMONIA_OFFSET_PPM + (-mq137_rs_kohm / MQ137_AMMONIA_SCALING_DIV));

  // HC-SR04 Ultrasonic Sensor
  float distance_cm = measureDistanceCM(ULTRASONIC_TRIG_PIN, ULTRASONIC_ECHO_PIN);
  if (!isnan(distance_cm)) {
    float waterHeight_cm = constrain(TANK_HEIGHT_CM - distance_cm, 0.0f, TANK_HEIGHT_CM);
    storageTank = calculateWaterVolumeLiters(waterHeight_cm);
  }
}

/**
 * @brief Updates the 16x2 LCD with the latest sensor data.
 */
void updateLcd() {
  lcd.setCursor(0, 0);
  lcd.printf("T:%.1fC H:%.0f%% ", temperature, humidity);
  lcd.setCursor(0, 1);
  lcd.printf("NH3:%.1f S:%.1fL", ammonia, storageTank);
}

/**
 * @brief Main handler for time-dependent tasks (sampling and reporting).
 */
void handleHourlyTasks(unsigned long currentMillis) {
  time_t now = time(nullptr);
  if (now < 946684800L) return; // Don't run if time is not synced

  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  handleDataSampling(timeinfo);
  handleHourlyReporting(timeinfo);
}

/**
 * @brief Handles the logic for taking a data sample at specified intervals.
 * @param timeinfo A struct containing the current local time.
 */
void handleDataSampling(const tm& timeinfo) {
  // IMPROVEMENT: More robust trigger. Triggers once when the minute is a multiple of 10
  // and is different from the last minute we sampled.
  if (timeinfo.tm_min % SAMPLING_INTERVAL_MINUTES == 0 && timeinfo.tm_min != lastSampleMinute) {
    if (currentHourlySampleCount < SAMPLES_PER_HOUR) {
      if (temperature > -40.0f && temperature < 80.0f && humidity >= 0.0f && humidity <= 100.0f) {
        hourlyTemperatureSamples[currentHourlySampleCount] = temperature;
        hourlyHumiditySamples[currentHourlySampleCount]    = humidity;
        hourlyAmmoniaSamples[currentHourlySampleCount]     = ammonia;
        hourlyStorageTankSamples[currentHourlySampleCount] = storageTank;
        currentHourlySampleCount++;
        
        Serial.printf("Sample %d taken at %02d:%02d\n", currentHourlySampleCount, timeinfo.tm_hour, timeinfo.tm_min);
        lastSampleMinute = timeinfo.tm_min; // Mark this minute as sampled
      } else {
        Serial.println("Skipping sample due to invalid sensor data.");
      }
    }
  }
}

/**
 * @brief Handles the logic for sending an averaged report at the top of the hour.
 * @param timeinfo A struct containing the current local time.
 */
void handleHourlyReporting(const tm& timeinfo) {
  // IMPROVEMENT: More robust trigger. Triggers once when the minute is 0
  // and the hour is different from the last hour we reported.
  if (timeinfo.tm_min == 0 && timeinfo.tm_hour != lastReportHour) {
    if (currentHourlySampleCount > 0) {
      Serial.printf("Initiating hourly report for hour: %d\n", timeinfo.tm_hour);
      postToGoogleSheets();
    } else {
      Serial.printf("Skipping report for hour %d: No samples taken.\n", timeinfo.tm_hour);
    }
    clearHourlySampleArrays(); // Reset for the next hour
    lastReportHour = timeinfo.tm_hour; // Mark this hour as reported
    // Reset lastSampleMinute to allow sampling in the new hour.
    lastSampleMinute = -1; 
  }
}

/**
 * @brief Prepares JSON payload and posts it to the configured Google Sheet webhook.
 */
void postToGoogleSheets() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Cannot send hourly report.");
    return;
  }
  
  float avg_temp = calculateFloatArrayAverage(hourlyTemperatureSamples, currentHourlySampleCount);
  float avg_hum  = calculateFloatArrayAverage(hourlyHumiditySamples, currentHourlySampleCount);
  float avg_nh3  = calculateFloatArrayAverage(hourlyAmmoniaSamples, currentHourlySampleCount);
  float avg_tank = calculateFloatArrayAverage(hourlyStorageTankSamples, currentHourlySampleCount);

  StaticJsonDocument<512> doc;
  doc["thing"] = THING_UID_NAME;

  char iso_timestamp[25];
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  strftime(iso_timestamp, sizeof(iso_timestamp), "%Y-%m-%dT%H:00:00", &timeinfo);
  doc["timestamp"] = iso_timestamp;

  if (!isnan(avg_nh3))  doc["ammonia"]     = round(avg_nh3 * 10.0f) / 10.0f;
  if (!isnan(avg_temp)) doc["temperature"] = round(avg_temp * 10.0f) / 10.0f;
  if (!isnan(avg_hum))  doc["humidity"]    = round(avg_hum * 10.0f) / 10.0f;
  if (!isnan(avg_tank)) doc["storageTank"] = round(avg_tank * 10.0f) / 10.0f;
  
  doc["storagePump"]    = storagePump ? 1 : 0;
  doc["siren"]          = siren ? 1 : 0;
  doc["cctv"]           = cCTV ? 1 : 0;
  doc["auxiliarySocket"]= auxilliarySocket ? 1 : 0;

  String payload;
  serializeJson(doc, payload);
  Serial.print("JSON Payload: "); Serial.println(payload);
  
  // IMPROVEMENT: For production, replace setInsecure() with a Root CA certificate.
  // This prevents man-in-the-middle attacks.
  // const char* google_root_ca = "-----BEGIN CERTIFICATE-----\n...";
  // clientSecure.setCACert(google_root_ca);
  clientSecure.setInsecure(); // Use with caution.

  Serial.println("Sending data to Google Sheet...");
  // IMPROVEMENT: Using substring on the URL constant directly.
  String path = String(GOOGLE_SHEET_WEBHOOK_URL).substring(String(GOOGLE_SCRIPT_HOST).length() + 8); // +8 for "https://"

  googleSheetsClient.post(path, "application/json", payload);
  
  int httpCode = googleSheetsClient.responseStatusCode();
  String httpResponse = googleSheetsClient.responseBody();
  Serial.print("Google Sheet POST - HTTP Code: "); Serial.println(httpCode);
  Serial.print("Google Sheet POST - Response: "); Serial.println(httpResponse);

  googleSheetsClient.stop();
}

// =======================================================================================
//                                  ORIGINAL HELPERS
// =======================================================================================

void synchronizeNTPTime() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Synchronizing time with NTP server... ");
    configTime(GMT_OFFSET_SECONDS, DAYLIGHT_OFFSET_SECONDS, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    
    time_t now = time(nullptr);
    int tries = 0;
    while (now < 946684800L && tries < NTP_SYNC_MAX_TRIES) {
      delay(NTP_SYNC_RETRY_DELAY_MS);
      now = time(nullptr);
      tries++;
      Serial.print(".");
    }
    
    if (now < 946684800L) {
      Serial.println("\nNTP Time synchronization failed!");
    } else {
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);
      Serial.print("\nNTP Time synchronized: ");
      Serial.print(asctime(&timeinfo));
    }
  } else {
    Serial.println("Cannot synchronize NTP: WiFi not connected.");
  }
}

void clearHourlySampleArrays() {
  for (int i = 0; i < SAMPLES_PER_HOUR; i++) {
    hourlyAmmoniaSamples[i]     = NAN;
    hourlyTemperatureSamples[i] = NAN;
    hourlyHumiditySamples[i]    = NAN;
    hourlyStorageTankSamples[i] = NAN;
  }
  currentHourlySampleCount = 0;
  Serial.println("Hourly sample arrays cleared.");
}

float calculateFloatArrayAverage(float* arr, int count) {
  if (count == 0) return NAN;
  
  float sum = 0.0f;
  int validSamples = 0;
  for (int i = 0; i < count; i++) {
    if (!isnan(arr[i])) {
      sum += arr[i];
      validSamples++;
    }
  }
  
  return (validSamples > 0) ? (sum / validSamples) : NAN;
}

float measureDistanceCM(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  
  long duration_us = pulseIn(echoPin, HIGH, ULTRASONIC_TIMEOUT_US); 
  
  return (duration_us > 0) ? (float)duration_us * SPEED_OF_SOUND_CM_PER_US / 2.0f : NAN;
}

float calculateWaterVolumeLiters(float h_water) {
  if (h_water <= 0.0f) return 0.0f;
  if (h_water >= TANK_HEIGHT_CM) return TANK_MAX_VOLUME_LITERS;
  
  float radius_at_h = TANK_RADIUS_BOTTOM_CM + 
                      (TANK_RADIUS_TOP_CM - TANK_RADIUS_BOTTOM_CM) * (h_water / TANK_HEIGHT_CM);
                      
  float volume_cm3 = (M_PI * h_water / 3.0f) * (radius_at_h * radius_at_h + 
                      radius_at_h * TANK_RADIUS_BOTTOM_CM + 
                      TANK_RADIUS_BOTTOM_CM * TANK_RADIUS_BOTTOM_CM);
                      
  return volume_cm3 / 1000.0f;
}

// =======================================================================================
//                       ARDUINO CLOUD VARIABLE CALLBACK FUNCTIONS
// =======================================================================================

void onAuxilliarySocketChange() {
  Serial.printf("Cloud Change: Auxiliary Socket -> %s\n", auxilliarySocket ? "ON" : "OFF");
  digitalWrite(RELAY_AUX_PIN, auxilliarySocket ? HIGH : LOW);
}

void onCCTVChange() {
  Serial.printf("Cloud Change: CCTV -> %s\n", cCTV ? "ON" : "OFF");
  digitalWrite(RELAY_CCTV_PIN, cCTV ? HIGH : LOW);
}

void onSirenChange() {
  Serial.printf("Cloud Change: Siren -> %s\n", siren ? "ON" : "OFF");
  digitalWrite(RELAY_SIREN_PIN, siren ? HIGH : LOW);
}

void onStoragePumpChange() {
  if (storagePump) {
    Serial.println("Cloud Change: Pump -> ON (20s timer started)");
    digitalWrite(RELAY_PUMP_PIN, HIGH);
    pumpTurnedOnMillis = millis();
  }
}

void onFlushIntervalChange() {
  Serial.printf("Cloud Change: Flush Interval set to %d\n", flushInterval);
}