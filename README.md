# ESP32-PixelCast

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-Community-orange)](https://platformio.org/)
[![ESP32](https://img.shields.io/badge/ESP32-Trinity-blue)](https://github.com/witnessmenow/ESP32-Trinity)

**ESP32-PixelCast** is an open source firmware for driving HUB75 LED matrix panels (64x64 and larger) via REST API and MQTT. Inspired by [AWTRIX3](https://github.com/Blueforcer/awtrix3), it brings the same features to bigger and brighter displays.

![PixelCast Demo](docs/images/demo.gif)

## Features

### Display
- Support for HUB75/HUB75E panels (64x64, 128x64, etc.)
- 24-bit RGB colors with gamma correction
- Adjustable brightness (0-255)
- Double buffering for smooth animations

### Applications
- **Custom Apps**: Create your own information screens
- **Auto-rotation**: Configurable cycling between apps
- **Lifetime**: Automatic expiration of apps not updated
- **Priorities**: Display priority management

### Notifications
- Push notifications with stack
- Urgent mode with immediate interruption
- Hold mode until acknowledgment

### Indicators
- 3 visual indicators (screen corners)
- Customizable colors
- Effects: blinking, fading

### Connectivity
- Complete **REST API**
- **MQTT** for home automation integration
- Compatible with **Home Assistant**, **Jeedom**, **Node-RED**
- WiFi with captive portal for configuration
- **OTA**: Over-the-air updates

### Media
- PNG/GIF icons (8x8 to 64x64)
- GIF animations
- Text with scrolling
- Progress bars
- Bar charts

## Supported Hardware

### ESP32 Boards
| Board | Status | Notes |
|-------|--------|-------|
| **ESP32 Trinity** | Recommended | Plug & play on HUB75 |
| ESP32 DevKit | Supported | Manual wiring required |
| ESP32-S3 | Planned | Extended PSRAM support |

### LED Panels
| Resolution | Scan | Status |
|------------|------|--------|
| **64x64** | 1/32 | Supported |
| 64x32 | 1/16 | Supported |
| 128x64 | 1/32 | Planned |

## Installation

### Prerequisites
- [PlatformIO](https://platformio.org/install) (VS Code recommended)
- ESP32 Trinity or compatible
- HUB75 64x64 P3 panel

### Initial Flash

```bash
# Clone the repository
git clone https://github.com/nicolas-codemate/esp32-pixelcast.git
cd esp32-pixelcast

# Build and flash
pio run -t upload

# Upload filesystem (icons, config)
pio run -t uploadfs
```

### WiFi Configuration

On first boot, PixelCast creates a WiFi access point:
- **SSID**: `PixelCast-XXXX`
- **Password**: `pixelcast`

Connect and access `http://192.168.4.1` to configure your WiFi network.

## Usage

### REST API

#### Create/Update a Custom App
```bash
curl -X POST "http://pixelcast.local/api/custom?name=weather" \
  -H "Content-Type: application/json" \
  -d '{
    "text": "72°F",
    "icon": "weather_sunny",
    "color": [255, 200, 0],
    "duration": 10
  }'
```

#### Send a Notification
```bash
curl -X POST "http://pixelcast.local/api/notify" \
  -H "Content-Type: application/json" \
  -d '{
    "text": "New message!",
    "icon": "mail",
    "color": [0, 150, 255],
    "duration": 5
  }'
```

#### Control an Indicator
```bash
curl -X POST "http://pixelcast.local/api/indicator1" \
  -H "Content-Type: application/json" \
  -d '{
    "color": [255, 0, 0],
    "blink": 500
  }'
```

### MQTT

#### Available Topics
| Topic | Direction | Description |
|-------|-----------|-------------|
| `pixelcast/custom/{name}` | To Device | Create/Update an app |
| `pixelcast/notify` | To Device | Send notification |
| `pixelcast/indicator{1-3}` | To Device | Control indicator |
| `pixelcast/settings` | To Device | Modify settings |
| `pixelcast/stats` | From Device | Statistics (auto-publish) |
| `pixelcast/status` | From Device | Online/Offline (LWT) |

#### Home Assistant Example
```yaml
# configuration.yaml
mqtt:
  sensor:
    - name: "PixelCast Status"
      state_topic: "pixelcast/status"

# automations.yaml
- alias: "Temperature notification"
  trigger:
    - platform: numeric_state
      entity_id: sensor.living_room_temperature
      above: 77
  action:
    - service: mqtt.publish
      data:
        topic: "pixelcast/notify"
        payload: >
          {
            "text": "High temperature: {{ states('sensor.living_room_temperature') }}°F",
            "icon": "temperature",
            "color": [255, 100, 0]
          }
```

## Payload Reference

### Custom App / Notification
```json
{
  "text": "Text to display",
  "icon": "icon_name",
  "color": [255, 255, 255],
  "background": [0, 0, 0],
  "duration": 10,
  "lifetime": 300,
  "pos": 0,
  "pushIcon": 0,
  "progress": -1,
  "progressC": [0, 255, 0],
  "progressBC": [50, 50, 50],
  "bar": [],
  "effect": "none",
  "effectSettings": {},
  "stack": false,
  "hold": false
}
```

### Parameters
| Parameter | Type | Description |
|-----------|------|-------------|
| `text` | string | Text to display |
| `icon` | string | Icon name (without extension) |
| `color` | [R,G,B] | Text color |
| `duration` | int | Display duration (seconds) |
| `lifetime` | int | Expiration if not updated (seconds) |
| `progress` | int | Progress bar (0-100, -1 = disabled) |
| `bar` | [int] | Data for bar chart |
| `effect` | string | Visual effect (Matrix, Rainbow, etc.) |
| `stack` | bool | Stack notifications |
| `hold` | bool | Hold until acknowledgment |

## Configuration

### platformio.ini
Main options are configurable via `platformio.ini`:

```ini
build_flags =
  -DPANEL_WIDTH=64
  -DPANEL_HEIGHT=64
  -DMAX_APPS=16
  -DMAX_NOTIFICATIONS=10
```

### Web Interface
Accessible via `http://pixelcast.local/`:
- WiFi configuration
- MQTT settings
- Icon management
- Live preview
- System logs

## Project Structure

```
esp32-pixelcast/
├── src/
│   ├── main.cpp              # Entry point
│   ├── display/              # HUB75 display management
│   ├── apps/                 # Application manager
│   ├── notifications/        # Notification system
│   ├── api/                  # REST + MQTT handlers
│   ├── web/                  # Web interface
│   └── utils/                # Utilities
├── include/
│   ├── config.h              # Global configuration
│   └── pins.h                # Pin definitions
├── lib/                      # Local libraries
├── data/                     # Filesystem (LittleFS)
│   ├── icons/                # PNG/GIF icons
│   ├── gifs/                 # Animations
│   └── www/                  # Web interface
├── docs/                     # Documentation
├── platformio.ini            # PlatformIO configuration
└── README.md
```

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

This project is licensed under MIT. See [LICENSE](LICENSE) for details.

## Acknowledgments

- [AWTRIX3](https://github.com/Blueforcer/awtrix3) for the inspiration
- [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) for the HUB75 driver
- [ESP32 Trinity](https://github.com/witnessmenow/ESP32-Trinity) for the reference hardware
- [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF) for GIF decoding

## Support

- [GitHub Issues](https://github.com/nicolas-codemate/esp32-pixelcast/issues)
- [Discussions](https://github.com/nicolas-codemate/esp32-pixelcast/discussions)

---
