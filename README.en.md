# Candle Matrix

A smart ambient light device based on a 9×16 LED matrix that simulates a flickering candle flame. Built on the **ESP32-S3** microcontroller with Wi-Fi connectivity and a built-in web interface.

![alt text](doc/scr.png)

[YouTube shorts](https://youtube.com/shorts/UzqbhlGrOIw)

---

## Features

- **Candle animation** — smooth flickering light effect on a 9×16 LED matrix
- **Auto sun mode** — automatically turns on at dusk and off at sunrise based on GPS coordinates; sunrise/sunset and twilight times are calculated on-device using NOAA solar formulas (no external API required)
- **Moon phase display** — dedicated moon phase animation mode
- **Sun position visualization** — real-time sun position indicator mode
- **Adjustable brightness** — 0–100% via web UI or HTTP API
- **NTP time sync** — accurate local time using a configurable NTP server and POSIX timezone
- **Web interface** — responsive UI served from LittleFS (no cloud dependency)
- **HTTP REST API** — control and monitor the device programmatically
- **Prometheus metrics** — `/metrics` endpoint for integration with Grafana dashboards
- **Captive portal** — easy Wi-Fi setup on first boot
- **mDNS** — accessible by hostname (e.g. `http://candle.local`)

---

## Hardware

| Component           | Description                               |
| ------------------- | ----------------------------------------- |
| **ESP32-S3 Zero**   | Main microcontroller (Wi-Fi, USB-CDC)     |
| **IS31FL3731**      | Charlieplexed PWM LED matrix driver (I²C) |
| **9×16 LED matrix** | 144 individually controlled LEDs          |

- [9×16 LED matrix on AliExpress](https://m.aliexpress.ru/item/0_1005008306395274.html)
- [IS31FL3731 PWM controller on AliExpress](https://m.aliexpress.ru/item/0_1005008306395274.html)
- [Candle box](https://www.thingiverse.com/thing:7363252)

---

## Software Stack

- **Framework:** Arduino (via PlatformIO)
- **Web server:** ESPAsyncWebServer
- **Filesystem:** LittleFS
- **JSON:** ArduinoJson 7
- **LED driver:** Adafruit IS31FL3731 Library
