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

### Initial Flash (USB)

```bash
# Clone the repository
git clone https://github.com/nicolas-codemate/esp32-pixelcast.git
cd esp32-pixelcast

# Build and flash via USB
pio run -t upload

# Upload filesystem (icons, config)
pio run -t uploadfs
```

### OTA Updates (WiFi)

After the initial USB flash, firmware can be updated over WiFi:

```bash
# Flash firmware over the air
pio run -e ota -t upload
```

The device advertises itself via mDNS as `pixelcast.local` on port 3232 (ArduinoOTA).
The display shows "OTA UPDATE" during the process.

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
    "color": "#FFC800",
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
    "color": "#0096FF",
    "duration": 5000,
    "hold": false,
    "urgent": false,
    "stack": true
  }'
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `text` | string | *required* | Notification text (max 127 chars) |
| `id` | string | auto | Unique ID (auto-generated `notif_<millis>` if omitted) |
| `icon` | string | - | Icon name from `/icons` folder |
| `color` | color | white | Text color and separator lines (`"#FF0000"`, `[255,0,0]`, or uint32) |
| `background` | color | black | Margin strip color above/below separators |
| `duration` | int | 5000 | Display duration in ms |
| `hold` | bool | false | If true, never auto-expires |
| `urgent` | bool | false | If true, jumps to front of queue |
| `stack` | bool | true | If false, clears queue before adding |

#### Dismiss / List Notifications
```bash
# Dismiss the current notification
curl -X POST "http://pixelcast.local/api/notify/dismiss"

# List all active notifications in queue
curl "http://pixelcast.local/api/notify/list"
```

#### Control an Indicator
```bash
# Solid red on top-left
curl -X POST "http://pixelcast.local/api/indicator1" \
  -H "Content-Type: application/json" \
  -d '{"color": "#FF0000"}'

# Blinking green on top-right
curl -X POST "http://pixelcast.local/api/indicator2" \
  -H "Content-Type: application/json" \
  -d '{"color": "#00FF00", "mode": "blink", "blinkInterval": 250}'

# Fading blue on bottom-right
curl -X POST "http://pixelcast.local/api/indicator3" \
  -H "Content-Type: application/json" \
  -d '{"color": "#0000FF", "mode": "fade"}'

# Turn off
curl -X DELETE "http://pixelcast.local/api/indicator1"
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `color` | color | *current* | RGB color (`"#FF0000"`, `[255,0,0]`, or uint32) |
| `mode` | string | `"solid"` | `"off"`, `"solid"`, `"blink"`, or `"fade"` |
| `blinkInterval` | int | 500 | Blink toggle interval in ms |
| `fadePeriod` | int | 2000 | Full fade cycle duration in ms |

3 indicators available: top-left (1), top-right (2), bottom-right (3). State persisted across reboots.

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
            "color": "#FF6400"
          }
```

## API Reference

Full interactive documentation: **[REST API](https://nicolas-codemate.github.io/esp32-pixelcast/swagger-ui.html)** (OpenAPI 3.1) | **[MQTT API](https://nicolas-codemate.github.io/esp32-pixelcast/asyncapi.html)** (AsyncAPI 3.0)

### Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | `/api/custom?name={name}` | Create/update custom app |
| `DELETE` | `/api/custom?name={name}` | Remove custom app |
| `POST` | `/api/notify` | Send notification |
| `POST` | `/api/notify/dismiss` | Dismiss current notification |
| `GET` | `/api/notify/list` | List active notifications |
| `POST` | `/api/weather` | Update weather data |
| `GET` | `/api/weather` | Read weather data |
| `POST` | `/api/tracker?name={name}` | Create/update tracker |
| `GET` | `/api/tracker?name={name}` | Read tracker data |
| `GET` | `/api/trackers` | List all trackers |
| `DELETE` | `/api/tracker?name={name}` | Remove tracker |
| `POST` | `/api/indicator{1-3}` | Set corner indicator |
| `DELETE` | `/api/indicator{1-3}` | Turn off corner indicator |
| `GET` | `/api/stats` | System statistics |
| `GET` | `/api/apps` | List all apps |
| `POST` | `/api/brightness` | Set brightness (0-255) |
| `POST` | `/api/reboot` | Restart device |

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
│   └── main.cpp              # Single-file firmware
├── include/
│   └── config.h              # Global configuration & defaults
├── lib/                      # Local libraries
├── data/                     # Filesystem (LittleFS)
│   ├── icons/                # PNG/GIF icons
│   ├── gifs/                 # Animations
│   └── config/               # Runtime settings (settings.json)
├── docs/api/                 # API specs (OpenAPI 3.1 + AsyncAPI 3.0)
├── api/                      # Bruno collection for API testing
├── platformio.ini            # PlatformIO configuration
├── ROADMAP.md                # Development roadmap
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
