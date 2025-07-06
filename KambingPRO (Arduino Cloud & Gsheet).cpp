// ----------------------------------------------------------------------------------
//  RAB001 – KambingPRO ESP32: Debug-Enhanced Arduino Cloud + Google Sheet Logger
//  Includes hourly sensor averaging, automatic flushing, and full serial diagnostics.
//  Revised by Anna for Aman – 04-07-2025
// ----------------------------------------------------------------------------------

#include "thingProperties.h" // Arduino Cloud variable declarations
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <time.h>            // NTP sync
#include <WiFiClientSecure.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <math.h>            // For M_PI in volume calculation

// ==== Project Configuration ====
const char* THING_UID_NAME = "RAB001"; // Unique identifier for this device

// ---- Google Apps Script Webhook ----
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
const int RELAY_PUMP_PIN     = 5;
const int RELAY_AUX_PIN      = 25;
const int RELAY_CCTV_PIN     = 33;
const int RELAY_SIREN_PIN    = 32;

const int DHT_SENSOR_PIN     = 15;
const int ULTRASONIC_TRIG_PIN = 13;
const int ULTRASONIC_ECHO_PIN = 14;
const int MQ137_ANALOG_PIN   = 34;   // ESP32 ADC1 pin

// ---- Sensor Specifics ----
#define DHT_SENSOR_TYPE DHT22
const float MQ137_LOAD_RESISTOR_KOHM = 22.0f;
const float ADC_VOLTAGE_REFERENCE    = 3.3f;
const float ADC_MAX_VALUE            = 4095.0f;
const float MQ137_AMMONIA_OFFSET_PPM = 7.0f;
const float MQ137_AMMONIA_SCALING_DIV= 10.0f;

const unsigned long ULTRASONIC_TIMEOUT_US = 30000UL; // 30 ms ≈ 5 m
const float SPEED_OF_SOUND_CM_PER_US = 0.0343f;

// ---- Tank Geometry (Frustum of a Cone) ----
const float TANK_HEIGHT_CM           = 38.0f;
const float TANK_RADIUS_TOP_CM       = 18.5f;
const float TANK_RADIUS_BOTTOM_CM = 14.0f;
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
HttpClient         googleSheetsClient = HttpClient(clientSecure, GOOGLE_SCRIPT_HOST, GOOGLE_SCRIPT_PORT);

// ---- Timing Variables ----
unsigned long lastNtpSyncMillis      = 0;
unsigned long lastSuccessfulSampleMillis = 0;
unsigned long pumpTurnedOnMillis     = 0; // Timestamp when the pump was turned on (for auto-off duration)
unsigned long lastAutoFlushMillis    = 0;
const long    PUMP_ON_DURATION_MS    = 20000L; // 20 seconds

// New variables for tracking relay ON durations
unsigned long totalPumpOnSeconds = 0;
unsigned long totalSirenOnSeconds = 0;
unsigned long totalCCTVOnSeconds = 0;
unsigned long totalAuxOnSeconds = 0; // For the auxiliary socket

// Variables to store the millis() timestamp when a relay last turned ON
unsigned long pumpLastOnMillis = 0;
unsigned long sirenLastOnMillis = 0;
unsigned long cctvLastOnMillis = 0;
unsigned long auxLastOnMillis = 0; // For the auxiliary socket

// prevStoragePumpState is no longer needed as logic is moved to callback
// bool prevStoragePumpState = false;


// ===================================================================================
//          SETUP
// ===================================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000); // wait for USB-CDC
  Serial.println("\n\nKambingPRO ESP32 booting…");

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
  lcd.print("KambingPRO IoT"); lcd.setCursor(0,1); lcd.print("Initializing…");

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
  
  // --- Initialize auto-flush timer ---
  lastAutoFlushMillis = millis(); 

  // --- Ready ---
  lcd.clear(); lcd.print(THING_UID_NAME); lcd.setCursor(0,1); lcd.print("System Ready");
  Serial.println("Setup complete. System is running.");

  // prevStoragePumpState is no longer needed
  // prevStoragePumpState = storagePump;
}

// ===================================================================================
//                      LOOP
// ===================================================================================
void loop() {
  ArduinoCloud.update();
  unsigned long nowMillis = millis();

  // Calculate intervalMillis here to use in the debug print
  unsigned long intervalMillis = (unsigned long)flushInterval * 60UL * 1000UL;

  Serial.printf("\n[Loop] Time: %lu | LastFlush: %lu | Interval: %d (%lu ms) | Diff: %lu | PumpCloud: %s | PumpPhysical: %s | PumpAutoOffTimer: %lu | PumpLastOn: %lu\n",
                  nowMillis, lastAutoFlushMillis, flushInterval, intervalMillis, (nowMillis - lastAutoFlushMillis), storagePump ? "ON" : "OFF", (digitalRead(RELAY_PUMP_PIN) == HIGH ? "ON" : "OFF"), pumpTurnedOnMillis, pumpLastOnMillis);
  Serial.printf("[Loop] PumpDur: %lu s | SirenDur: %lu s | CCTV_Dur: %lu s | AuxDur: %lu s\n",
                  totalPumpOnSeconds, totalSirenOnSeconds, totalCCTVOnSeconds, totalAuxOnSeconds);


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
      Serial.printf("[Hourly Report] Sending data for %02d:00\n", tmNow.tm_hour);

      // --- Before reporting, add any ongoing durations to the total ---
      if (storagePump && pumpLastOnMillis != 0) { // If pump is currently ON
          totalPumpOnSeconds += (nowMillis - pumpLastOnMillis) / 1000;
          pumpLastOnMillis = nowMillis; // Reset start time for the next hour
          Serial.printf("[Hourly Report] Pump was ON at hour change, added %lu s. New start time: %lu\n", (nowMillis - pumpLastOnMillis) / 1000, pumpLastOnMillis);
      }
      if (siren && sirenLastOnMillis != 0) { // If siren is currently ON
          totalSirenOnSeconds += (nowMillis - sirenLastOnMillis) / 1000;
          sirenLastOnMillis = nowMillis; // Reset start time for the next hour
          Serial.printf("[Hourly Report] Siren was ON at hour change, added %lu s. New start time: %lu\n", (nowMillis - sirenLastOnMillis) / 1000, sirenLastOnMillis);
      }
      if (cCTV && cctvLastOnMillis != 0) { // If CCTV is currently ON
          totalCCTVOnSeconds += (nowMillis - cctvLastOnMillis) / 1000;
          cctvLastOnMillis = nowMillis; // Reset start time for the next hour
          Serial.printf("[Hourly Report] CCTV was ON at hour change, added %lu s. New start time: %lu\n", (nowMillis - cctvLastOnMillis) / 1000, cctvLastOnMillis);
      }
      if (auxilliarySocket && auxLastOnMillis != 0) { // If Aux is currently ON
          totalAuxOnSeconds += (nowMillis - auxLastOnMillis) / 1000;
          auxLastOnMillis = nowMillis; // Reset start time for the next hour
          Serial.printf("[Hourly Report] Aux was ON at hour change, added %lu s. New start time: %lu\n", (nowMillis - auxLastOnMillis) / 1000, auxLastOnMillis);
      }

      // Calculate averages of collected samples.
      float aTemp = averageArray(hourlyTemperatureSamples, currentHourlySampleCount);
      float aHum  = averageArray(hourlyHumiditySamples,     currentHourlySampleCount);
      float aNH3  = averageArray(hourlyAmmoniaSamples,      currentHourlySampleCount);
      float aTank = averageArray(hourlyStorageTankSamples, currentHourlySampleCount);
      
      StaticJsonDocument<512> doc;
      doc["thing"]           = THING_UID_NAME;
      char iso[25]; strftime(iso, sizeof iso, "%Y-%m-%dT%H:00:00", &tmNow); doc["timestamp"] = iso;
      if (!isnan(aNH3 )) doc["ammonia"]     = round(aNH3  * 10) / 10.0f;
      if (!isnan(aTemp)) doc["temperature"] = round(aTemp * 10) / 10.0f;
      if (!isnan(aHum )) doc["humidity"]    = round(aHum  * 10) / 10.0f;
      if (!isnan(aTank)) doc["storageTank"] = round(aTank * 10) / 10.0f;
      doc["flushInterval"]   = flushInterval;
      // Add new duration fields
      doc["pumpDuration"] = totalPumpOnSeconds;
      doc["sirenDuration"] = totalSirenOnSeconds;
      doc["cctvDuration"] = totalCCTVOnSeconds;
      doc["auxDuration"] = totalAuxOnSeconds; // Add auxiliary socket duration

      String out; serializeJson(doc, out);
      Serial.printf("[Hourly Report] JSON Payload: %s\n", out.c_str());

      if (WiFi.status() == WL_CONNECTED) {
        clientSecure.setInsecure();
        googleSheetsClient.post(String(GOOGLE_SHEET_WEBHOOK_URL), "application/json", out);
        Serial.printf("Google Sheet POST status code: %d\n", googleSheetsClient.responseStatusCode());
        Serial.printf("Google Sheet POST response: %s\n", googleSheetsClient.responseBody().c_str());
      } else {
        Serial.println("WiFi down – hourly report skipped");
      }
      clearHourlySampleArrays();
      lastHour = tmNow.tm_hour;

      // --- Reset duration counters for the new hour ---
      Serial.println("[Hourly Report] Resetting relay duration counters.");
      totalPumpOnSeconds = 0;
      totalSirenOnSeconds = 0;
      totalCCTVOnSeconds = 0;
      totalAuxOnSeconds = 0;
    }
  }

  // ---------- Automatic Flushing & Pump Control Logic ----------
  // 1. Automatic flush trigger: This code decides WHEN to flush.
  if (flushInterval > 0 && !storagePump) {
    // intervalMillis is already calculated above for the debug print
    if (nowMillis - lastAutoFlushMillis >= intervalMillis) {
      Serial.printf("TIMER: Auto-flush triggered by %d minute interval. Current millis: %lu\n", flushInterval, nowMillis);
      // Directly perform the turn-on actions.
      digitalWrite(RELAY_PUMP_PIN, HIGH);
      // Use nowMillis for pumpTurnedOnMillis to ensure consistency
      pumpTurnedOnMillis = nowMillis; // Start the 20-second auto-off timer
      storagePump = true; // Update the cloud variable to reflect the new state.
      Serial.printf("  >> ACTION: Relay ON. pumpTurnedOnMillis set to %lu. storagePump set to TRUE. Calling ArduinoCloud.update()\n", pumpTurnedOnMillis);
      lastAutoFlushMillis = nowMillis; // Reset the timer for the next flush
      ArduinoCloud.update(); // Immediately update Cloud to reflect pump status.

      // Also record the start time for pump duration tracking
      pumpLastOnMillis = nowMillis;
      Serial.println("[AUTO-FLUSH] Pump ON, recording start time for duration tracking.");
    }
  }

  // 2. Automatic pump turn-off timer: This code decides WHEN to stop.
  if (storagePump && (nowMillis - pumpTurnedOnMillis >= PUMP_ON_DURATION_MS)) {
    Serial.printf("TIMER: Auto-off condition met! nowMillis: %lu, pumpTurnedOnMillis: %lu, Diff: %lu, PUMP_ON_DURATION_MS: %lu\n",
                   nowMillis, pumpTurnedOnMillis, (nowMillis - pumpTurnedOnMillis), PUMP_ON_DURATION_MS);
    digitalWrite(RELAY_PUMP_PIN, LOW);
    storagePump = false; // Update the cloud variable.
    Serial.println("  >> ACTION: Relay OFF. storagePump set to FALSE. Calling ArduinoCloud.update()");
    ArduinoCloud.update(); // Immediately update Cloud to reflect pump status.

    totalPumpOnSeconds += PUMP_ON_DURATION_MS / 1000;
    pumpLastOnMillis = 0; // Reset as pump is now off
    Serial.printf("[AUTO-FLUSH] Added %lu seconds to total pump ON duration. Total: %lu s\n", PUMP_ON_DURATION_MS / 1000, totalPumpOnSeconds);
  }

  // --- Handle manual storagePump changes from Cloud Dashboard ---
  // This block ensures that if storagePump is changed from the dashboard,
  // the physical relay is updated and the auto-off timer is correctly started.
  // Removed prevStoragePumpState variable as it's no longer needed for pump control.
  // The previous logic was: if (storagePump != prevStoragePumpState) { ... }
  // Now, we directly check the desired state (storagePump) against the physical state.

  // The logic below is removed as it was causing conflicts and the direct callback approach is preferred.
  // if (storagePump && digitalRead(RELAY_PUMP_PIN) == LOW) {
  //     Serial.println("[Pump Control] Cloud commanded Pump ON. Turning ON physical relay.");
  //     digitalWrite(RELAY_PUMP_PIN, HIGH); // Turn physical pump ON
  //     pumpTurnedOnMillis = nowMillis;    // Start the auto-off timer
  //     pumpLastOnMillis = nowMillis;      // Start duration tracking
  //     Serial.printf("[Pump Control] Pump ON. Timer started at %lu ms.\n", pumpTurnedOnMillis);
  // }
  // else if (!storagePump && digitalRead(RELAY_PUMP_PIN) == HIGH) {
  //     Serial.println("[Pump Control] Cloud commanded Pump OFF. Turning OFF physical relay.");
  //     digitalWrite(RELAY_PUMP_PIN, LOW); // Turn physical pump OFF
  //     // Calculate and add duration if it was running
  //     if (pumpLastOnMillis != 0) {
  //         totalPumpOnSeconds += (nowMillis - pumpLastOnMillis) / 1000;
  //         Serial.printf("[Pump Control] Added %lu seconds to total pump ON duration. Total: %lu s\n", (nowMillis - pumpLastOnMillis) / 1000, totalPumpOnSeconds);
  //         pumpLastOnMillis = 0; // Reset duration tracking
  //     }
  //     pumpTurnedOnMillis = 0; // Reset auto-off timer
  // }


  delay(200);
}

// ===================================================================================
//          Helper functions
// ===================================================================================

/**
 * @brief Synchronizes ESP32 time with NTP servers.
 * Retries multiple times until successful or max tries reached.
 */
void synchronizeNTPTime() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("NTP sync failed – WiFi down"); return; }
  Serial.print("Syncing time via NTP…");
  configTime(GMT_OFFSET_SECONDS, DAYLIGHT_OFFSET_SECONDS, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  time_t now = time(nullptr); int tries = 0;
  while (now < 946684800L && tries++ < NTP_SYNC_MAX_TRIES) { delay(NTP_SYNC_RETRY_DELAY_MS); now = time(nullptr); Serial.print('.'); }
  if (now < 946684800L) Serial.println(" failed!");
  else { struct tm tmNow; localtime_r(&now, &tmNow); Serial.printf(" OK. Current time: %s", asctime(&tmNow)); }
}

/**
 * @brief Clears all hourly sample arrays and resets the sample count.
 * Fills arrays with NAN (Not A Number) to ensure valid averaging.
 */
void clearHourlySampleArrays() {
  for (int i = 0; i < MAX_HOURLY_SAMPLES; i++) {
    hourlyAmmoniaSamples     [i] = NAN;
    hourlyTemperatureSamples[i] = NAN;
    hourlyHumiditySamples   [i] = NAN;
    hourlyStorageTankSamples[i] = NAN;
  }
  currentHourlySampleCount = 0;
  Serial.println("Hourly sample arrays cleared.");
}

/**
 * @brief Calculates the average of an array of floats, ignoring NAN values.
 * @param arr Pointer to the float array.
 * @param n Number of elements to consider in the array.
 * @return The average of valid (non-NAN) numbers, or NAN if no valid numbers.
 */
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

/**
 * @brief Measures distance using an ultrasonic sensor.
 * @param trigPin The trigger pin of the ultrasonic sensor.
 * @param echoPin The echo pin of the ultrasonic sensor.
 * @return Distance in centimeters, or NAN if timeout occurs.
 */
float measureDistanceCM(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration_us = pulseIn(echoPin, HIGH, ULTRASONIC_TIMEOUT_US);
  return duration_us > 0 ? duration_us * SPEED_OF_SOUND_CM_PER_US / 2.0f : NAN;
}

/**
 * @brief Calculates the water volume in liters for a frustum-shaped tank.
 * Based on the height of the water.
 * @param h_cm Water height in centimeters.
 * @return Water volume in liters.
 */
float calculateWaterVolumeLiters(float h_cm) {
  h_cm = constrain(h_cm, 0.0f, TANK_HEIGHT_CM);
  if (h_cm <= 0.0f) return 0.0f;
  if (h_cm >= TANK_HEIGHT_CM) return TANK_MAX_VOLUME_LITERS;
  float radius_at_h = TANK_RADIUS_BOTTOM_CM + (h_cm / TANK_HEIGHT_CM) * (TANK_RADIUS_TOP_CM - TANK_RADIUS_BOTTOM_CM);
  float vol_cm3 = (M_PI * h_cm / 3.0f) * (pow(radius_at_h, 2) + radius_at_h * TANK_RADIUS_BOTTOM_CM + pow(TANK_RADIUS_BOTTOM_CM, 2));
  return vol_cm3 / 1000.0f;
}

// ===================================================================================
//          Cloud variable change callbacks
// ===================================================================================

/**
 * @brief Callback function when the 'storagePump' variable changes in the Arduino Cloud.
 * Controls the pump relay and records when it was turned on by the cloud.
 */
void onStoragePumpChange() {
    unsigned long nowMillis = millis();
    digitalWrite(RELAY_PUMP_PIN, storagePump ? HIGH : LOW); // Update physical relay immediately

    if (storagePump) { // Pump is turning ON via Cloud dashboard
        pumpTurnedOnMillis = nowMillis; // Start the auto-off timer
        pumpLastOnMillis = nowMillis; // Start duration tracking
        Serial.printf("[Cloud Callback] Pump ON. pumpTurnedOnMillis set to %lu. pumpLastOnMillis set to %lu.\n", pumpTurnedOnMillis, pumpLastOnMillis);
    } else { // Pump is turning OFF via Cloud dashboard
        if (pumpLastOnMillis != 0) { // Only calculate duration if it was previously ON
            totalPumpOnSeconds += (nowMillis - pumpLastOnMillis) / 1000;
            Serial.printf("[Cloud Callback] Pump OFF. Added %lu seconds. Total: %lu s\n", (nowMillis - pumpLastOnMillis) / 1000, totalPumpOnSeconds);
            pumpLastOnMillis = 0; // Reset start time for duration tracking
        }
        pumpTurnedOnMillis = 0; // Reset auto-off timer when pump goes off
    }
    Serial.printf("[Cloud] StoragePump now %s\n", storagePump ? "ON" : "OFF");
}

/**
 * @brief Callback function when the 'siren' variable changes in the Arduino Cloud.
 * Controls the siren relay and records its ON/OFF duration.
 */
void onSirenChange() {
  unsigned long nowMillis = millis();
  if (siren) { // Siren is turning ON
    sirenLastOnMillis = nowMillis;
    Serial.printf("[Cloud Callback] Siren ON at %lu ms\n", sirenLastOnMillis);
  } else { // Siren is turning OFF
    if (sirenLastOnMillis != 0) { // Only calculate if it was previously ON
      totalSirenOnSeconds += (nowMillis - sirenLastOnMillis) / 1000;
      Serial.printf("[Cloud Callback] Siren OFF. Added %lu seconds. Total: %lu s\n", (nowMillis - sirenLastOnMillis) / 1000, totalSirenOnSeconds);
      sirenLastOnMillis = 0; // Reset start time
    }
  }
  digitalWrite(RELAY_SIREN_PIN, siren ? HIGH : LOW);
  Serial.printf("[Cloud] Siren now %s\n", siren ? "ON" : "OFF");
}

/**
 * @brief Callback function when the 'cctv' variable changes in the Arduino Cloud.
 * Controls the CCTV relay and records its ON/OFF duration.
 */
void onCCTVChange() {
  unsigned long nowMillis = millis();
  if (cCTV) { // CCTV is turning ON
    cctvLastOnMillis = nowMillis;
    Serial.printf("[Cloud Callback] CCTV ON at %lu ms\n", cctvLastOnMillis);
  } else { // CCTV is turning OFF
    if (cctvLastOnMillis != 0) { // Only calculate if it was previously ON
      totalCCTVOnSeconds += (nowMillis - cctvLastOnMillis) / 1000;
      Serial.printf("[Cloud Callback] CCTV OFF. Added %lu seconds. Total: %lu s\n", (nowMillis - cctvLastOnMillis) / 1000, totalCCTVOnSeconds);
      cctvLastOnMillis = 0; // Reset start time
    }
  }
  digitalWrite(RELAY_CCTV_PIN, cCTV ? HIGH : LOW);
  Serial.printf("[Cloud] CCTV now %s\n", cCTV ? "ON" : "OFF");
}

/**
 * @brief Callback function when the 'auxilliarySocket' variable changes in the Arduino Cloud.
 * Controls the auxiliary relay and records its ON/OFF duration.
 */
void onAuxilliarySocketChange() {
  unsigned long nowMillis = millis();
  if (auxilliarySocket) { // Auxiliary Socket is turning ON
    auxLastOnMillis = nowMillis;
    Serial.printf("[Cloud Callback] Aux Socket ON at %lu ms\n", auxLastOnMillis);
  } else { // Auxiliary Socket is turning OFF
    if (auxLastOnMillis != 0) { // Only calculate if it was previously ON
      totalAuxOnSeconds += (nowMillis - auxLastOnMillis) / 1000;
      Serial.printf("[Cloud Callback] Aux Socket OFF. Added %lu seconds. Total: %lu s\n", (nowMillis - auxLastOnMillis) / 1000, totalAuxOnSeconds);
      auxLastOnMillis = 0; // Reset start time
    }
  }
  digitalWrite(RELAY_AUX_PIN, auxilliarySocket ? HIGH : LOW);
  Serial.printf("[Cloud] Auxiliary Socket now %s\n", auxilliarySocket ? "ON" : "OFF");
}

/**
 * @brief Callback function when the 'flushInterval' variable changes in the Arduino Cloud.
 * Resets the auto-flush timer to apply the new interval immediately.
 */
void onFlushIntervalChange() {
  if (flushInterval > 0) {
    Serial.printf("[Cloud] Flush interval updated to %d minutes. Resetting auto-flush timer.\n", flushInterval);
    lastAutoFlushMillis = millis(); // Reset the timer to start the new interval countdown from now
  } else {
    Serial.println("[Cloud] Automatic flushing is now DISABLED.");
  }
}
