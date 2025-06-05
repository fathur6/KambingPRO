# KambingPRO 🐐📡

**KambingPRO** is an ESP32-based IoT system for intelligent livestock barn management. Designed to support traceable, efficient, and sustainable animal waste and environment monitoring, this project integrates real-time data streaming to Arduino Cloud, automated flushing systems, and data logging to Google Sheets.

---

## 🚀 Features

- 🌡️ **Sensor Monitoring**: Tracks temperature, humidity (DHT22), ammonia (MQ-137), and water level (ultrasonic sensor).
- ⏱️ **Scheduled and Reactive Flushing**: Activates barn pumps at intervals or when ammonia exceeds a threshold.
- 🧠 **Real-Time Decision Making**: ESP32 automates relays based on sensor logic and cloud input.
- 📊 **Cloud Sync**: 
  - Streams live data to [Arduino Cloud IoT](https://cloud.arduino.cc/).
  - Sends hourly-averaged data to **Google Sheets** via Google Apps Script.
- 📟 **Local LCD Display**: Shows current sensor readings for on-site operators.
- ⏰ **NTP Time Sync**: Keeps logs and control operations time-accurate.

---

## 📦 Hardware Requirements

- ESP32 Dev Board
- DHT22 / DHT21 Sensor
- MQ-137 Ammonia Gas Sensor
- Ultrasonic Sensor (HC-SR04 or JSN-SR04T)
- 4-channel relay module (for pump, CCTV, siren)
- I2C 16x2 or 20x4 LCD Display
- Water storage tank with pump
- Optional: Buzzer or LED indicators

---

## 🛠️ Software Requirements

- [Arduino IDE](https://www.arduino.cc/en/software)
- Arduino Cloud-connected `.ino` sketch
- `thingProperties.h` (auto-generated from Arduino IoT Cloud)
- [Google Apps Script](https://script.google.com/) Web App for Sheets logging

Required Libraries:
- `ArduinoIoTCloud`
- `DHT sensor library`
- `LiquidCrystal_I2C`
- `WiFi.h`, `HTTPClient.h`, etc.

---

## 📈 Data Flow

1. **Main Loop**: Continuously sends sensor data to Arduino Cloud.
2. **10-Min Interval Task**: Aggregates readings, sends cleaned data to Google Sheets.
3. **Flushing System**:
   - Time-controlled flush every X minutes
   - OR automatic flush if ammonia > threshold

---

## 🔒 License

This project is licensed under the [MIT License](LICENSE).

---

## 🤝 Contributing

Pull requests are welcome! Whether you're improving hardware setup, refactoring code, or extending cloud functionality — your contribution is appreciated.

---

## 📬 Contact

Maintained by [Fathurrahman Lananan](mailto:fathurrahman@unisza.edu.my)

---

## 🌾 Acknowledgements

This project is part of the **R.A.Barn** initiative toward sustainable, IoT-enabled agriculture and animal waste management.
