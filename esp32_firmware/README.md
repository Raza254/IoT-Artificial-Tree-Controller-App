# ESP32 Firmware - Smart IoT System

This folder contains the ESP32 C++/Arduino firmware that interfaces with the physical environmental sensors, handles autonomous relay actuation, synchronizes NTP time, and connects to Firebase Realtime Database.

## Hardware Components & Pinout

| Component | Sensor / Actuator Type | ESP32 GPIO Pin | Protocol / Interface |
|---|---|---|---|
| **DHT22 / AM2302** | Temperature & Relative Humidity | `GPIO 4` | 1-Wire Digital |
| **CCS811** | eCO2 & Total Volatile Organic Compounds (TVOC) | `GPIO 8` (SDA), `GPIO 9` (SCL) | I2C |
| **Optical Dust Sensor** | Smog / Airborne Particulate | `GPIO 10` (LED), `GPIO 14` (ADC) | Analog + Pulse Trigger |
| **Relay 1** | Exhaust Fan Actuator | `GPIO 11` | Digital Output (Active LOW) |
| **Relay 2** | Humidifier Actuator | `GPIO 12` | Digital Output (Active LOW) |

## Required Arduino Libraries

Install the following libraries via the **Arduino IDE Library Manager** (`Sketch` -> `Include Library` -> `Manage Libraries...`):
- `Firebase ESP Client` by Mobizt (v4.x.x)
- `DHT sensor library` by Adafruit
- `Adafruit CCS811 Library` by Adafruit
- `Adafruit Unified Sensor` by Adafruit

## Configuration

Before flashing the code to your ESP32 board, open `esp32_firmware.ino` and update the following configuration lines:

```cpp
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define API_KEY "YOUR_FIREBASE_WEB_API_KEY"
#define DATABASE_URL "https://<your-project-id>-default-rtdb.firebaseio.com/"
```

## Automation Logic Thresholds

- **Fan Trigger:** When sustained $\text{CO}_2 \ge 700\text{ ppm}$ for more than 5 seconds.
- **Humidifier Trigger:** When sustained $\text{Humidity} < 50\%$ for more than 5 seconds.
- **Manual Mode:** Flutter app sends commands to override autonomous thresholds.
