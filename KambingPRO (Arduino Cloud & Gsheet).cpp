// ----------------------------------------------------------------------------------
//  AmanPRO ESP32 – Arduino Cloud + Google Sheet Hourly Logger (Time-Synced)
//  Description: This sketch monitors ammonia, temperature, humidity, and water level.
//  It sends hourly averaged data to a Google Sheet and allows for remote
//  control of a pump, siren, CCTV, and an auxiliary device via Arduino Cloud.
//
//  Wiring (2025-06-20 revision for new setup)
//  * MQ-137          -> GPIO 34 (ADC1_CH6)
//  * DHT22/AM2301      -> GPIO 15
//  * Relay #1 Pump     -> GPIO 5
//  * Relay #2 Aux      -> GPIO 25
//  * Relay #3 CCTV     -> GPIO 33
//  * Relay #4 Siren    -> GPIO 32
//  * HC-SR04 Trig      -> GPIO 13
//  * HC-SR04 Echo      -> GPIO 14
//  * I2C LCD SDA       -> GPIO 21 (Default)
//  * I2C LCD SCL       -> GPIO 22 (Default)
//
//  Aman - UniSZA, ©2025
// ----------------------------------------------------------------------------------

#include "thingProperties.h"      // Arduino Cloud variable declarations
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <time.h>                 // NTP sync
#include <WiFiClientSecure.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <math.h>                 // For M_PI in volume calculation

// ==== Project Configuration ====
const char* THING_UID_NAME = "RAB-001"; // Unique identifier for this device

// ---- Google Apps Script Webhook ----
// IMPORTANT: Replace with your own Google Apps Script Webhook URL
const char* GOOGLE_SHEET_WEBHOOK_URL = "https://script.google.com/macros/s/AKfycbxaygP3nPks_jBGWjEhmRce7UESrxxHb1cGK65Nhnxpc4L663tCWeSaVKkdExZya0oc/exec";
const char* GOOGLE_SCRIPT_HOST       = "script.google.com";
const int   GOOGLE_SCRIPT_PORT       = 443;  // HTTPS

// ---- NTP Configuration ----
const long  GMT_OFFSET_SECONDS       = 8L * 3600L; // GMT+8
const int   DAYLIGHT_OFFSET_SECONDS  = 0;
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";
const char* NTP_SERVER_3 = "sg.pool.ntp.org";
const unsigned long NTP_SYNC_INTERVAL_MS = 12UL * 3600UL * 1000UL; // every 12 h
const int NTP_SYNC_MAX_TRIES         = 20;
const int NTP_SYNC_RETRY_DELAY_MS    = 500;

// ---- Hardware Pin Definitions (New Setup) ----
const int RELAY_PUMP_PIN      = 5;
const int RELAY_AUX_PIN       = 25;
const int RELAY_CCTV_PIN      = 33;
const int RELAY_SIREN_PIN     = 32;

const int DHT_SENSOR_PIN      = 15;
const int ULTRASONIC_TRIG_PIN = 13;
const int ULTRASONIC_ECHO_PIN = 14;
const int MQ137_ANALOG_PIN    = 34;   // ESP32 ADC1 pin

// ---- Sensor Specifics ----
#define DHT_SENSOR_TYPE DHT22
const float MQ137_LOAD_RESISTOR_KOHM = 22.0f;
const float ADC_VOLTAGE_REFERENCE    = 3.3f;
const float ADC_MAX_VALUE            = 4095.0f;
const float MQ137_AMMONIA_OFFSET_PPM = 7.0f;
const float MQ137_AMMONIA_SCALING_DIV= 10.0f;

const unsigned long ULTRASONIC_TIMEOUT_US = 30000UL; // 30 ms ≈ 5 m
const float SPEED_OF_SOUND_CM_PER_US = 0.0343f;

// ---- Tank Geometry (Frustum of a Cone) ---- // -- REVISED SECTION --
const float TANK_HEIGHT_CM        = 38.0f;
const float TANK_RADIUS_TOP_CM    = 18.5f;
const float TANK_RADIUS_BOTTOM_CM = 14.0f;
// Calculate max volume for a frustum of a cone: V = (πh/3) * (R² + Rr + r²)
const float TANK_MAX_VOLUME_LITERS = (M_PI * TANK_HEIGHT_CM / 3.0f) *
                                     (pow(TANK_RADIUS_TOP_CM, 2) + TANK_RADIUS_TOP_CM * TANK_RADIUS_BOTTOM_CM + pow(TANK_RADIUS_BOTTOM_CM, 2)) / 1000.0f;

// ---- Data Sampling & Averaging ----
#define MAX_HOURLY_SAMPLES 6  // 1 sample / 10 min → six per hour
float hourlyAmmoniaSamples    [MAX_HOURLY_SAMPLES];
float hourlyTemperatureSamples[MAX_HOURLY_SAMPLES];
float hourlyHumiditySamples   [MAX_HOURLY_SAMPLES];
float hourlyStorageTankSamples[MAX_HOURLY_SAMPLES];
int   currentHourlySampleCount = 0;

// ---- Global Objects ----
LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht(DHT_SENSOR_PIN, DHT_SENSOR_TYPE);

WiFiClientSecure clientSecure;
HttpClient       googleSheetsClient = HttpClient(clientSecure, GOOGLE_SCRIPT_HOST, GOOGLE_SCRIPT_PORT);

// ---- Timing ----
unsigned long lastNtpSyncMillis          = 0;
unsigned long lastSuccessfulSampleMillis = 0;
unsigned long pumpTurnedOnMillis         = 0;
const long    PUMP_ON_DURATION_MS        = 20000L; // 20 seconds

// ===================================================================================
//                                     SETUP
// ===================================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000); // wait for USB-CDC
  Serial.println("AmanPRO ESP32 booting…");

  // --- Relays ---
  pinMode(RELAY_PUMP_PIN,  OUTPUT); digitalWrite(RELAY_PUMP_PIN,  LOW);
  pinMode(RELAY_AUX_PIN,   OUTPUT); digitalWrite(RELAY_AUX_PIN,   LOW);
  pinMode(RELAY_CCTV_PIN,  OUTPUT); digitalWrite(RELAY_CCTV_PIN,  LOW);
  pinMode(RELAY_SIREN_PIN, OUTPUT); digitalWrite(RELAY_SIREN_PIN, LOW);

  // --- Sensors ---
  pinMode(ULTRASONIC_TRIG_PIN, OUTPUT);
  pinMode(ULTRASONIC_ECHO_PIN, INPUT);
  dht.begin();

  // --- LCD ---
  lcd.init(); lcd.backlight();
  lcd.print("AmanPRO IoT"); lcd.setCursor(0,1); lcd.print("Initializing…");

  // --- Arduino Cloud ---
  initProperties();
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  setDebugMessageLevel(2);
  ArduinoCloud.printDebugInfo();
  Serial.print("Connecting to Arduino Cloud");
  while (!ArduinoCloud.connected()) { ArduinoCloud.update(); delay(500); Serial.print('.'); }
  Serial.println(" connected!");

  // --- Time sync ---
  synchronizeNTPTime();

  // --- Clear sample buffers ---
  clearHourlySampleArrays();

  // --- Ready ---
  lcd.clear(); lcd.print(THING_UID_NAME); lcd.setCursor(0,1); lcd.print("System Ready");
  Serial.println("Setup complete.");
}

// ===================================================================================
//                                     LOOP
// ===================================================================================
void loop() {
  ArduinoCloud.update();
  unsigned long nowMillis = millis();

  // NTP resync every 12 h
  if (nowMillis - lastNtpSyncMillis > NTP_SYNC_INTERVAL_MS || lastNtpSyncMillis == 0) {
    synchronizeNTPTime();
    lastNtpSyncMillis = nowMillis;
  }

  // ---------- Sensor reads ----------
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity    = h;

  int   mqRaw   = analogRead(MQ137_ANALOG_PIN);
  float mqVolt  = mqRaw * (ADC_VOLTAGE_REFERENCE / ADC_MAX_VALUE);
  float rs_kOhm = mqVolt > 0.001f ? (ADC_VOLTAGE_REFERENCE - mqVolt) * MQ137_LOAD_RESISTOR_KOHM / mqVolt : 1e5f;
  ammonia = max(0.0f, MQ137_AMMONIA_OFFSET_PPM + (-rs_kOhm / MQ137_AMMONIA_SCALING_DIV));

  float dist_cm = measureDistanceCM(ULTRASONIC_TRIG_PIN, ULTRASONIC_ECHO_PIN);
  if (!isnan(dist_cm)) {
    // Calculate water height from the bottom of the tank
    float water_h = constrain(TANK_HEIGHT_CM - dist_cm, 0.0f, TANK_HEIGHT_CM);
    storageTank = calculateWaterVolumeLiters(water_h);
  }

  // LCD Update
  lcd.setCursor(0,0); lcd.printf("T:%.1fC H:%2.0f%%", temperature, humidity);
  lcd.setCursor(0,1); lcd.printf("NH3:%.1f S:%5.1fL", ammonia, storageTank);

  // ---------- Timed sampling (every 10 min, on the minute) ----------
  time_t epoch = time(nullptr);
  struct tm tmNow; localtime_r(&epoch, &tmNow);

  if (tmNow.tm_min % 10 == 0 && tmNow.tm_sec == 0 && (nowMillis - lastSuccessfulSampleMillis > 1000)) {
    if (currentHourlySampleCount < MAX_HOURLY_SAMPLES &&
        t > -40 && t < 80 && h >= 0 && h <= 100 && ammonia >= 0 && storageTank >= 0) {
      hourlyTemperatureSamples[currentHourlySampleCount] = t;
      hourlyHumiditySamples   [currentHourlySampleCount] = h;
      hourlyAmmoniaSamples    [currentHourlySampleCount] = ammonia;
      hourlyStorageTankSamples[currentHourlySampleCount] = storageTank;
      currentHourlySampleCount++;
      Serial.printf("Sample %d stored (%02d:%02d)\n", currentHourlySampleCount, tmNow.tm_hour, tmNow.tm_min);
    }
    lastSuccessfulSampleMillis = nowMillis;
  }

  // ---------- Hourly report to Google Sheet ----------
  if (tmNow.tm_min == 0 && tmNow.tm_sec == 0) {
    static int lastHour = -1;
    if (tmNow.tm_hour != lastHour && currentHourlySampleCount > 0) {
      float aTemp = averageArray(hourlyTemperatureSamples, currentHourlySampleCount);
      float aHum  = averageArray(hourlyHumiditySamples,    currentHourlySampleCount);
      float aNH3  = averageArray(hourlyAmmoniaSamples,     currentHourlySampleCount);
      float aTank = averageArray(hourlyStorageTankSamples, currentHourlySampleCount);

      // Create JSON payload for Google Sheet
      StaticJsonDocument<512> doc;
      doc["thing"]         = THING_UID_NAME;
      char iso[25]; strftime(iso, sizeof iso, "%Y-%m-%dT%H:00:00", &tmNow); doc["timestamp"] = iso;
      if (!isnan(aNH3 )) doc["ammonia"]     = round(aNH3  * 10) / 10.0f;
      if (!isnan(aTemp)) doc["temperature"] = round(aTemp * 10) / 10.0f;
      if (!isnan(aHum )) doc["humidity"]    = round(aHum  * 10) / 10.0f;
      if (!isnan(aTank)) doc["storageTank"] = round(aTank * 10) / 10.0f;
      // Also log the state of all relays
      doc["storagePump"]      = storagePump      ? 1 : 0;
      doc["auxilliarySocket"] = auxilliarySocket ? 1 : 0;
      doc["cctv"]             = cCTV             ? 1 : 0;
      doc["siren"]            = siren            ? 1 : 0;

      String out; serializeJson(doc, out);
      Serial.println("Hourly Report Payload:");
      Serial.println(out);

      // Send data to webhook
      if (WiFi.status() == WL_CONNECTED) {
        clientSecure.setInsecure(); // Use for script.google.com, but be aware of security implications
        String path = String(GOOGLE_SHEET_WEBHOOK_URL);
        
        googleSheetsClient.post(path, "application/json", out);
        
        int statusCode = googleSheetsClient.responseStatusCode();
        String response = googleSheetsClient.responseBody();

        Serial.print("Google Sheet POST status code: ");
        Serial.println(statusCode);
        Serial.print("Google Sheet POST response: ");
        Serial.println(response);

      } else {
        Serial.println("WiFi down – hourly report skipped");
      }

      clearHourlySampleArrays();
      lastHour = tmNow.tm_hour;
    }
  }

  // ---------- Pump Auto-Off Timer Logic ----------
  if (storagePump && (nowMillis - pumpTurnedOnMillis >= PUMP_ON_DURATION_MS)) {
    Serial.println("Pump -> OFF (20s timer elapsed)");
    digitalWrite(RELAY_PUMP_PIN, LOW); // Turn off the relay
    storagePump = false; // IMPORTANT: Update the cloud variable to reflect the new state
  }

  delay(200);
}

// ===================================================================================
//                              Helper functions
// ===================================================================================
void synchronizeNTPTime() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("NTP sync failed – WiFi down"); return; }
  Serial.print("Syncing time via NTP…");
  configTime(GMT_OFFSET_SECONDS, DAYLIGHT_OFFSET_SECONDS, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  time_t now = time(nullptr); int tries = 0;
  while (now < 946684800L && tries++ < NTP_SYNC_MAX_TRIES) { delay(NTP_SYNC_RETRY_DELAY_MS); now = time(nullptr); Serial.print('.'); }
  if (now < 946684800L) Serial.println(" failed!");
  else { struct tm tmNow; localtime_r(&now, &tmNow); Serial.printf(" OK. Current time: %s", asctime(&tmNow)); }
}

void clearHourlySampleArrays() {
  for (int i = 0; i < MAX_HOURLY_SAMPLES; i++) {
    hourlyAmmoniaSamples[i]     = NAN;
    hourlyTemperatureSamples[i] = NAN;
    hourlyHumiditySamples[i]    = NAN;
    hourlyStorageTankSamples[i] = NAN;
  }
  currentHourlySampleCount = 0;
  Serial.println("Hourly sample arrays cleared.");
}

float averageArray(float* arr, int n) {
  if (n == 0) return NAN;
  float sum = 0; int valid_count = 0;
  for (int i = 0; i < n; i++) {
    if (!isnan(arr[i])) {
      sum += arr[i];
      valid_count++;
    }
  }
  return valid_count > 0 ? sum / valid_count : NAN;
}

float measureDistanceCM(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW); delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration_us = pulseIn(echoPin, HIGH, ULTRASONIC_TIMEOUT_US);
  return duration_us > 0 ? duration_us * SPEED_OF_SOUND_CM_PER_US / 2.0f : NAN;
}

// -- REVISED SECTION for Conical Frustum Tank --
float calculateWaterVolumeLiters(float h_cm) {
  // Calculates volume for a frustum of a cone
  h_cm = constrain(h_cm, 0.0f, TANK_HEIGHT_CM);
  if (h_cm <= 0.0f) return 0.0f;
  if (h_cm >= TANK_HEIGHT_CM) return TANK_MAX_VOLUME_LITERS;

  // Calculate the radius of the water's surface at height 'h' using linear interpolation
  float radius_at_h = TANK_RADIUS_BOTTOM_CM + (h_cm / TANK_HEIGHT_CM) * (TANK_RADIUS_TOP_CM - TANK_RADIUS_BOTTOM_CM);

  // Volume of a frustum: V = (1/3) * π * h * (r² + r*R + R²)
  float vol_cm3 = (M_PI * h_cm / 3.0f) * (pow(radius_at_h, 2) + radius_at_h * TANK_RADIUS_BOTTOM_CM + pow(TANK_RADIUS_BOTTOM_CM, 2));

  // Convert volume from cm^3 to Liters (1000 cm^3 = 1 L)
  return vol_cm3 / 1000.0f;
}

// ===================================================================================
//                        Cloud variable change callbacks
// ===================================================================================
void onStoragePumpChange() {
  // This function now only acts as a trigger to turn the pump ON
  if (storagePump) { // Only act if the command is to turn ON
    Serial.println("Pump -> ON (20s timer started)");
    digitalWrite(RELAY_PUMP_PIN, HIGH);
    pumpTurnedOnMillis = millis(); // Record the time it was turned on
  }
  // The pump is turned OFF automatically by the timer in the main loop
}

void onSirenChange() {
  Serial.printf("Siren -> %s\n", siren ? "ON" : "OFF");
  digitalWrite(RELAY_SIREN_PIN, siren ? HIGH : LOW);
}

void onCCTVChange() {
  Serial.printf("CCTV -> %s\n", cCTV ? "ON" : "OFF");
  digitalWrite(RELAY_CCTV_PIN, cCTV ? HIGH : LOW);
}

void onAuxilliarySocketChange()  {
  Serial.printf("Auxiliary Socket -> %s\n", auxilliarySocket ? "ON" : "OFF");
  digitalWrite(RELAY_AUX_PIN, auxilliarySocket ? HIGH : LOW);
}

void onFlushIntervalChange() {
  // This is a placeholder. You can add logic here if needed.
  Serial.printf("Flush interval set to %d minutes.\n", flushInterval);
}
