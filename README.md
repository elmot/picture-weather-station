# Picture Weather Station

An ESP32-based weather station with a colorful 320x240 LCD, built with
[ESP-IDF](https://docs.espressif.com/projects/esp-idf/) and the
[Slint](https://slint.dev/) UI framework. Aggregates weather data from multiple
sources and presents it on a small desktop display with fox mascot illustrations.

![Board: Unihiker K10](https://img.shields.io/badge/board-Unihiker_K10-blue?logo=espressif)
![Board: WaveShare PhotoPainter](https://img.shields.io/badge/board-WaveShare_PhotoPainter-yellow?logo=espressif)

![Framework: ESP-IDF](https://img.shields.io/badge/framework-ESP--IDF-red?logo=espressif)
![UI: Slint](https://img.shields.io/badge/UI-Slint-blue?logo=slint)

![IDE: CLion](https://img.shields.io/badge/IDE-CLion-black?logo=clion)
![AI: Claude](https://img.shields.io/badge/AI-Claude-orange?logo=claude)
## Data Sources

| Source          | What it provides                                             | Transport    |
|-----------------|--------------------------------------------------------------|--------------|
| **Open-Meteo**  | Outdoor temperature, feels-like, wind, weather code          | HTTPS REST   |
| **AHT20**       | Indoor temperature & humidity (with 96-sample history chart) | I2C          |
| **Ruuvi Tag**   | Temperature, humidity, barometric pressure, battery          | BLE (NimBLE) |
| **Adafruit IO** | Remote feed value (e.g. CO2 ppm) + chart history             | HTTPS REST   |

Ruuvi pressure is also published back to Adafruit IO every 5 minutes.

## Hardware

- **[DFRobot Unihiker K10](https://www.dfrobot.com/)** 
  - ESP32-S3, 16 MB flash, 8 MB octal PSRAM
  - 320x240 TFT LCD
  - **AHT20** temperature/humidity sensor
- _TBD_ migrate to **[ESP32-S3-PhotoPainter 7.3"](https://www.waveshare.com/esp32-s3-photopainter.htm)**
- **[Ruuvi Tag](https://ruuvi.com/)**

## UI

Weather icons follow the WMO weather interpretation codes with day/night
variants. The fox mascot changes between sun, rain, snow, and wind poses.

## Building

### Prerequisites

- ESP-IDF v6.x (with `idf.py` on PATH)

### Configuration

Create `sdkconfig.secrets` using [template](sdkconfig.secrets.template) in the project root with your credentials:

All options are also available via `idf.py menuconfig` under
**Picture Weather Station**.

### Build & Flash

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Tools & Resources

- **[CLion](https://www.jetbrains.com/clion/)** - C/C++ IDE used for development
- **[Slint Language Plugin](https://marketplace.visualstudio.com/items?itemName=Slint.slint)** - IDE extension for Slint
  UI framework
- **[Claude](https://claude.ai/)** - AI assistant used for development support
- **[Adafruit IO](https://io.adafruit.com/)** - Cloud service for IoT data feeds and visualization
- **[ESP-IDF 6](https://docs.espressif.com/projects/esp-idf/en/v6.0/)** - Espressif IoT Development Framework

## Architecture

The firmware runs four concurrent FreeRTOS tasks plus the Slint event loop:

