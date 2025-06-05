// KambingPRO ESP32 - Arduino Cloud + Google Sheet Hourly Logger (Time-Synced, Power-Aware)
// Aligned to NTP (GMT+8). Posts averaged hourly data to Google Sheet via Apps Script Webhook.
// Aman & Anna, 2025-06-xx (Revised 2025-06-02 for best practices)

#include "thingProperties.h" // Contains Arduino Cloud variable declarations and connection logic
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <time.h>               // For NTP time synchronization
#include <WiFiClientSecure.h>   // For HTTPS communication
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>        // For creating JSON payloads

// ==== Project Configuration ====
const char* THING_UID_NAME = "RAB001"; // Unique identifier for this device/thing

// ---- Google Apps Script Webhook ----
// Note: Ensure your Google Apps Script is deployed to accept POST requests
const char* GOOGLE_SHEET_WEBHOOK_URL = "https://script.google.com/macros/s/AKfycbxaygP3nPks_jBGWjEhmRce7UESrxxHb1cGK65Nhnxpc4L663tCWeSaVKkdExZya0oc/exec";
const char* GOOGLE_SCRIPT_HOST = "script.google.com";
const int   GOOGLE_SCRIPT_PORT = 443; // HTTPS port

// ---- NTP Configuration ----
const long  GMT_OFFSET_SECONDS    = 8 * 3600; // GMT+8
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
// MQ137 Ammonia calculation parameters (empirical, adjust as needed)
const float MQ137_AMMONIA_OFFSET_PPM   = 7.0f;
const float MQ137_AMMONIA_SCALING_DIV  = 10.0f;

const float ULTRASONIC_TIMEOUT_US    = 30000UL; // Microseconds
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
#define MAX_HOURLY_SAMPLES 6 // E.g., one sample every 10 minutes for an hour
float hourlyAmmoniaSamples[MAX_HOURLY_SAMPLES];
float hourlyTemperatureSamples[MAX_HOURLY_SAMPLES];
float hourlyHumiditySamples[MAX_HOURLY_SAMPLES];
float hourlyStorageTankSamples[MAX_HOURLY_SAMPLES];
int   currentHourlySampleCount = 0;

// ---- Global Objects ----
LiquidCrystal_I2C lcd(0x27, 16, 2); // I2C address 0x27, 16 column and 2 rows
DHT dht(DHT_SENSOR_PIN, DHT_SENSOR_TYPE);

WiFiClientSecure clientForGoogleSheetsPOST_secure; // Use WiFiClientSecure for HTTPS
HttpClient googleSheetsHttpClient = HttpClient(clientForGoogleSheetsPOST_secure, GOOGLE_SCRIPT_HOST, GOOGLE_SCRIPT_PORT);

// ---- Timing Variables ----
unsigned long lastNtpSyncMillis = 0;
unsigned long lastSuccessfulSampleMillis = 0; // To avoid multiple samples in the same target second

// ---- Cloud Variables (assumed to be defined in thingProperties.h) ----
// extern float temperature;
// extern float humidity;
// extern float ammonia;
// extern float storageTank;
// extern bool storagePump;
// extern bool siren;
// extern bool cCTV;
// extern bool auxilliarySocket;
// extern int flushInterval; // Assuming this is defined if onFlushIntervalChange exists

// =======================================================================================
//                                   SETUP FUNCTION
// =======================================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000); // Wait for serial port to connect (for debugging)
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

  // --- Initialize Arduino Cloud Properties and Connection ---
  initProperties(); // Links variables to Arduino Cloud
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  setDebugMessageLevel(2); // 0 (errors only), 1 (info), 2 (debug), 3 (trace)
  ArduinoCloud.printDebugInfo();

  Serial.println("Waiting for Arduino Cloud connection...");
  while (ArduinoCloud.connected() != 1) {
    ArduinoCloud.update(); // Must be called frequently
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nArduino Cloud connected!");

  // --- Time Synchronization ---
  synchronizeNTPTime();

  // --- Initialize Data Storage ---
  clearHourlySampleArrays();

  // --- Final LCD Ready Message ---
  lcd.clear();
  lcd.print(THING_UID_NAME);
  lcd.setCursor(0,1);
  lcd.print("System Ready");
  Serial.println("System Initialization Complete.");
}

// =======================================================================================
//                                    MAIN LOOP
// =======================================================================================
void loop() {
  ArduinoCloud.update(); // Essential for Arduino Cloud functionality

  unsigned long currentMillis = millis();

  // --- Periodic NTP Time Synchronization ---
  if (currentMillis - lastNtpSyncMillis > NTP_SYNC_INTERVAL_MS || lastNtpSyncMillis == 0) {
    synchronizeNTPTime();
    lastNtpSyncMillis = currentMillis;
  }

  // --- Read Sensor Data ---
  float currentTemperature = dht.readTemperature(); // Celsius
  float currentHumidity    = dht.readHumidity();    // Percent
  
  // Update cloud variables if readings are valid
  if (!isnan(currentTemperature)) temperature = currentTemperature;
  if (!isnan(currentHumidity))    humidity    = currentHumidity;

  // MQ137 Ammonia Sensor Reading
  int mq137_raw_adc = analogRead(MQ137_ANALOG_PIN);
  float mq137_voltage_rl = mq137_raw_adc * (ADC_VOLTAGE_REFERENCE / ADC_MAX_VALUE);
  float mq137_rs_kohm = 0.0f;
  if (mq137_voltage_rl > 0.001f) { // Avoid division by zero or near-zero
      mq137_rs_kohm = (ADC_VOLTAGE_REFERENCE - mq137_voltage_rl) * MQ137_LOAD_RESISTOR_KOHM / mq137_voltage_rl;
  } else {
      mq137_rs_kohm = 100000.0f; // Effectively a very high resistance -> low/zero PPM
  }
  ammonia = max(0.0f, MQ137_AMMONIA_OFFSET_PPM + (-mq137_rs_kohm / MQ137_AMMONIA_SCALING_DIV)); // Simplified model

  // Ultrasonic Sensor - Water Tank Level
  float distance_cm = measureDistanceCM(ULTRASONIC_TRIG_PIN, ULTRASONIC_ECHO_PIN);
  if (!isnan(distance_cm)) {
    float waterHeight_cm = TANK_HEIGHT_CM - distance_cm;
    waterHeight_cm = constrain(waterHeight_cm, 0.0f, TANK_HEIGHT_CM); // Ensure height is within tank bounds
    storageTank = calculateWaterVolumeLiters(waterHeight_cm);
  } else {
    // storageTank remains its previous value or could be set to NAN if preferred
    // For now, let's assume it keeps last valid reading or cloud variable default
  }
  
  // --- LCD Update ---
  lcd.setCursor(0, 0);
  lcd.printf("T:%.1fC H:%.0f%% ", temperature, humidity); // Cloud variables shown
  lcd.setCursor(0, 1);
  lcd.printf("NH3:%.1f S:%.1fL", ammonia, storageTank); // Cloud variables shown

  // --- Time-Synced Data Sampling (e.g., every 10 minutes at xx:00, xx:10, xx:20...) ---
  time_t current_epoch_time = time(nullptr);
  struct tm timeinfo;
  localtime_r(&current_epoch_time, &timeinfo); // Get local time structure

  // Sample at the start of the 0th second of every 10th minute
  if (timeinfo.tm_min % 10 == 0 && timeinfo.tm_sec == 0) {
    // Ensure sampling happens only once per target second
    if (currentMillis - lastSuccessfulSampleMillis > 1000) { 
      if (currentHourlySampleCount < MAX_HOURLY_SAMPLES) {
        // Basic validation for sensor data before storing
        if (temperature > -40.0f && temperature < 80.0f && // Plausible temp range
            humidity >= 0.0f && humidity <= 100.0f &&   // Plausible humidity range
            ammonia >= 0.0f &&                          // Ammonia shouldn't be negative
            storageTank >= 0.0f) {                      // Volume shouldn't be negative
              
          hourlyTemperatureSamples[currentHourlySampleCount] = temperature;
          hourlyHumiditySamples[currentHourlySampleCount]    = humidity;
          hourlyAmmoniaSamples[currentHourlySampleCount]     = ammonia;
          hourlyStorageTankSamples[currentHourlySampleCount] = storageTank;
          currentHourlySampleCount++;
          
          Serial.printf("Sample %d taken at %02d:%02d:%02d\n", currentHourlySampleCount, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
          lastSuccessfulSampleMillis = currentMillis; 
        } else {
          Serial.println("Skipping sample due to invalid sensor data.");
        }
      }
    }
  }

  // --- Hourly Reporting to Google Sheet (at the start of a new hour, e.g., xx:00:00) ---
  if (timeinfo.tm_min == 0 && timeinfo.tm_sec == 0) {
    static int lastReportHour = -1; // Ensure reporting happens only once per hour change
    if (lastReportHour != timeinfo.tm_hour && currentHourlySampleCount > 0) { // Only report if there are samples
      Serial.printf("Initiating hourly report for hour: %d\n", timeinfo.tm_hour);

      float avg_temp = calculateFloatArrayAverage(hourlyTemperatureSamples, currentHourlySampleCount);
      float avg_hum  = calculateFloatArrayAverage(hourlyHumiditySamples, currentHourlySampleCount);
      float avg_nh3  = calculateFloatArrayAverage(hourlyAmmoniaSamples, currentHourlySampleCount);
      float avg_tank = calculateFloatArrayAverage(hourlyStorageTankSamples, currentHourlySampleCount);

      // --- Prepare JSON Payload for Google Sheets ---
      StaticJsonDocument<512> json_payload_doc; // Adjust size as needed
      json_payload_doc["thing"] = THING_UID_NAME;

      char iso_timestamp_str[25]; // Buffer for ISO8601 timestamp
      strftime(iso_timestamp_str, sizeof(iso_timestamp_str), "%Y-%m-%dT%H:00:00", &timeinfo); // Timestamp for the hour
      json_payload_doc["timestamp"] = iso_timestamp_str;

      // Only include fields in JSON if their average is a valid number
      if (!isnan(avg_nh3))  json_payload_doc["ammonia"]     = round(avg_nh3 * 10.0f) / 10.0f; // 1 decimal place
      if (!isnan(avg_temp)) json_payload_doc["temperature"] = round(avg_temp * 10.0f) / 10.0f;
      if (!isnan(avg_hum))  json_payload_doc["humidity"]    = round(avg_hum * 10.0f) / 10.0f;
      if (!isnan(avg_tank)) json_payload_doc["storageTank"] = round(avg_tank * 10.0f) / 10.0f;
      
      // Add status of cloud-controlled actuators (assuming these are global bools from thingProperties.h)
      json_payload_doc["storagePump"]    = storagePump ? 1 : 0;
      json_payload_doc["siren"]          = siren ? 1 : 0;
      json_payload_doc["cctv"]           = cCTV ? 1 : 0; // Variable name in sketch cCTV, JSON auxilliarySocket
      json_payload_doc["auxiliarySocket"]= auxilliarySocket ? 1 : 0;


      String json_output_str;
      serializeJson(json_payload_doc, json_output_str);
      Serial.print("JSON Payload: "); Serial.println(json_output_str);

      // --- Post to Google Sheets ---
      if (WiFi.status() == WL_CONNECTED) {
        // For ESP32, you might need to provide CA cert for script.google.com
        // clientForGoogleSheetsPOST_secure.setCACert(google_ca_cert); 
        // Or, for testing (less secure):
        clientForGoogleSheetsPOST_secure.setInsecure(); // Allows connection to hosts without verifying SSL certificate. Use with caution.

        Serial.println("Sending data to Google Sheet...");
        String path = GOOGLE_SHEET_WEBHOOK_URL;
        if (path.startsWith("https://script.google.com")) { // Ensure we only use the path part for the HttpClient
             path = path.substring(String("https://script.google.com").length());
        }

        googleSheetsHttpClient.beginRequest();
        int post_error = googleSheetsHttpClient.post(path.c_str());
        
        if (post_error == 0) { // Successfully started request
          googleSheetsHttpClient.sendHeader("Content-Type", "application/json");
          googleSheetsHttpClient.sendHeader("Content-Length", json_output_str.length());
          googleSheetsHttpClient.beginBody();
          googleSheetsHttpClient.print(json_output_str);
          googleSheetsHttpClient.endRequest();

          int http_response_code = googleSheetsHttpClient.responseStatusCode();
          String http_response_body = googleSheetsHttpClient.responseBody();
          Serial.print("Google Sheet POST - HTTP Response Code: "); Serial.println(http_response_code);
          Serial.print("Google Sheet POST - Response Body: "); Serial.println(http_response_body);
        } else {
          Serial.print("Error starting HTTP POST request to Google Sheet, error code: "); Serial.println(post_error);
        }
        googleSheetsHttpClient.stop(); // Close connection
      } else {
        Serial.println("WiFi not connected. Cannot send hourly data to Google Sheet.");
        // Optionally, implement data caching here to send later when connection is restored
      }
      
      clearHourlySampleArrays(); // Reset for the next hour's samples
      lastReportHour = timeinfo.tm_hour;
    }
  }
  delay(200); // General loop delay to yield for other tasks if any (e.g. WiFi stack)
}

// =======================================================================================
//                                 HELPER FUNCTIONS
// =======================================================================================

/**
 * @brief Synchronizes the ESP32's internal clock with an NTP server.
 */
void synchronizeNTPTime() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Synchronizing time with NTP server... ");
    configTime(GMT_OFFSET_SECONDS, DAYLIGHT_OFFSET_SECONDS, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    
    time_t now = time(nullptr);
    int tries = 0;
    // Wait until time is greater than a known past date (e.g., 2000-01-01 00:00:00 UTC)
    // Epoch for 2000-01-01 00:00:00 UTC is 946684800
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
      localtime_r(&now, &timeinfo); // Use localtime_r for thread safety
      Serial.print("\nNTP Time synchronized: ");
      Serial.println(asctime(&timeinfo));
    }
  } else {
    Serial.println("Cannot synchronize NTP: WiFi not connected.");
  }
}

/**
 * @brief Clears all hourly sample arrays and resets the sample count.
 */
void clearHourlySampleArrays() {
  for (int i = 0; i < MAX_HOURLY_SAMPLES; i++) {
    hourlyAmmoniaSamples[i]     = NAN; // Not a Number, indicates no valid sample
    hourlyTemperatureSamples[i] = NAN;
    hourlyHumiditySamples[i]    = NAN;
    hourlyStorageTankSamples[i] = NAN;
  }
  currentHourlySampleCount = 0;
  Serial.println("Hourly sample arrays cleared.");
}

/**
 * @brief Calculates the average of valid (non-NAN) float values in an array.
 * @param arr Pointer to the float array.
 * @param count The number of elements to consider for averaging (typically currentHourlySampleCount).
 * @return The average value, or NAN if no valid samples or count is zero.
 */
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

/**
 * @brief Measures distance using an HC-SR04 or similar ultrasonic sensor.
 * @param trigPin The trigger pin for the ultrasonic sensor.
 * @param echoPin The echo pin for the ultrasonic sensor.
 * @return Measured distance in centimeters (cm), or NAN on timeout/error.
 */
float measureDistanceCM(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  
  // Reads the echoPin, returns the sound wave travel time in microseconds
  // Timeout ULTRASONIC_TIMEOUT_US (e.g., 30000 us for ~5 meters max range)
  long duration_us = pulseIn(echoPin, HIGH, ULTRASONIC_TIMEOUT_US); 
  
  if (duration_us > 0) {
    // Calculate the distance: Time * Speed_of_Sound / 2 (round trip)
    return (float)duration_us * SPEED_OF_SOUND_CM_PER_US / 2.0f;
  } else {
    return NAN; // Timeout or no echo received
  }
}

/**
 * @brief Calculates the volume of water in a frustum-shaped tank.
 * @param h_water Current height of the water in cm, measured from the bottom.
 * @return Volume of water in Liters.
 */
float calculateWaterVolumeLiters(float h_water) {
  if (h_water <= 0.0f) return 0.0f;
  if (h_water >= TANK_HEIGHT_CM) return TANK_MAX_VOLUME_LITERS;
  
  // Calculate the radius of the water surface at height h_water using linear interpolation
  float radius_at_h = TANK_RADIUS_BOTTOM_CM + 
                      (TANK_RADIUS_TOP_CM - TANK_RADIUS_BOTTOM_CM) * (h_water / TANK_HEIGHT_CM);
                      
  // Volume of a frustum segment (from bottom up to h_water)
  // V = (pi * h / 3) * (R_top_segment^2 + R_top_segment * R_bottom_segment + R_bottom_segment^2)
  // Here, R_bottom_segment is TANK_RADIUS_BOTTOM_CM, and R_top_segment is radius_at_h
  float volume_cm3 = (M_PI * h_water / 3.0f) * (radius_at_h * radius_at_h + 
                      radius_at_h * TANK_RADIUS_BOTTOM_CM + 
                      TANK_RADIUS_BOTTOM_CM * TANK_RADIUS_BOTTOM_CM);
                      
  return volume_cm3 / 1000.0f; // Convert cm^3 to Liters
}

// =======================================================================================
//                      ARDUINO CLOUD VARIABLE CALLBACK FUNCTIONS
// =======================================================================================
// These functions are called when the E S P 32 receives an update for the 
// corresponding cloud variable from the Arduino Cloud dashboard or API.

void onAuxilliarySocketChange() {
  Serial.print("Auxiliary Socket state changed via Cloud: ");
  Serial.println(auxilliarySocket ? "ON" : "OFF");
  digitalWrite(RELAY_AUX_PIN, auxilliarySocket ? HIGH : LOW);
}

void onCCTVChange() {
  Serial.print("CCTV Relay state changed via Cloud: ");
  Serial.println(cCTV ? "ON" : "OFF");
  digitalWrite(RELAY_CCTV_PIN, cCTV ? HIGH : LOW);
}

void onSirenChange() {
  Serial.print("Siren Relay state changed via Cloud: ");
  Serial.println(siren ? "ON" : "OFF");
  digitalWrite(RELAY_SIREN_PIN, siren ? HIGH : LOW);
}

void onStoragePumpChange() {
  Serial.print("Storage Pump Relay state changed via Cloud: ");
  Serial.println(storagePump ? "ON" : "OFF");
  digitalWrite(RELAY_PUMP_PIN, storagePump ? HIGH : LOW);
}

void onFlushIntervalChange() {
  // Example: 'flushInterval' could be a cloud variable determining how often something happens.
  Serial.print("Flush Interval changed via Cloud to: ");
  Serial.println(flushInterval); // Assuming 'flushInterval' is an int or similar
  // Add logic here to act on the new flushInterval value
}

// Add other callbacks from thingProperties.h if any