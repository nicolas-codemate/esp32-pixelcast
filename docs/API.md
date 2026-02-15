# API Reference

ESP32-PixelCast exposes a REST API and MQTT for controlling the display.

## Base URL

```
http://pixelcast.local/api/
```

## Authentication

By default, the API is not protected. Authentication can be enabled in settings.

## REST API

### Custom Apps

#### Create/Update an app

```http
POST /api/custom?name={appName}
Content-Type: application/json
```

**Payload:**

```json
{
  "text": "Hello World",
  "icon": "smiley",
  "label": "Subtitle",
  "color": [
    255,
    255,
    255
  ],
  "background": [
    0,
    0,
    0
  ],
  "duration": 10,
  "lifetime": 300,
  "progress": -1,
  "progressC": [
    0,
    255,
    0
  ],
  "progressBC": [
    50,
    50,
    50
  ],
  "bar": [],
  "effect": "none"
}
```

**Parameters:**

| Param      | Type    | Default       | Description                                        |
|------------|---------|---------------|----------------------------------------------------|
| text       | mixed   | ""            | Text to display (see colored segments below)       |
| icon       | string  | null          | Icon name (without extension)                      |
| label      | mixed   | ""            | Subtitle below text (see colored segments below)   |
| color      | [R,G,B] | [255,255,255] | Default text color                                 |
| background | [R,G,B] | [0,0,0]       | Background color                                   |
| duration   | int     | 10            | Display duration (seconds)                         |
| lifetime   | int     | 0             | Expiration if not updated (seconds, 0 = permanent) |
| progress   | int     | -1            | Progress bar (0-100, -1 = disabled)                |
| progressC  | [R,G,B] | [0,255,0]     | Progress color                                     |
| progressBC | [R,G,B] | [50,50,50]    | Progress background color                          |
| bar        | [int]   | []            | Data for bar chart                                 |
| effect     | string  | "none"        | Visual effect                                      |

**Available effects:**

- `none`: No effect
- `matrix`: Character rain
- `rainbow`: Rainbow
- `fade`: Fade
- `pulse`: Pulsation

**Colored text segments:**

The `text` and `label` fields accept three formats:

1. **Simple string** (backward compatible):
```json
"text": "22.5°C"
```

2. **String with custom color** (overrides `color`):
```json
"text": {"text": "22.5°C", "color": "#FF0000"}
```

3. **Array of colored segments** (per-group coloring):
```json
"text": [{"t": "22.5", "c": "#FF8800"}, {"t": "°C", "c": "#666666"}]
```

Each segment has:
- `t`: text content (concatenated in order)
- `c`: color for this segment (hex `"#FF8800"`, RGB `[255,136,0]`, or uint32)

Up to 8 segments per field. Works in both single-zone and multi-zone layouts.

**Response:**

```json
{
  "success": true
}
```

#### Multi-zone layout

Instead of single text/icon/color, send a `zones` array to create a dashboard-like multi-info screen. Layout is inferred from zone count.

```http
POST /api/custom?name={appName}
Content-Type: application/json
```

**Payload (2 zones - dual rows):**

```json
{
  "zones": [
    { "text": "22.5C", "icon": "thermo", "label": "Salon", "color": "#FF8800" },
    { "text": "58%", "icon": "humidity", "label": "Humidity", "color": "#00D4FF" }
  ],
  "duration": 10000
}
```

**Payload (4 zones - quad grid):**

```json
{
  "zones": [
    { "text": "22.5C", "icon": "thermo", "label": "Salon", "color": "#FF8800" },
    { "text": "58%", "icon": "humidity", "color": "#00D4FF" },
    { "text": "BTC", "icon": "btc", "color": "#FFD000" },
    { "text": "OK", "icon": "wifi", "color": "#22FF44" }
  ],
  "duration": 15000
}
```

**Zone object:**

| Param | Type   | Default       | Description                                           |
|-------|--------|---------------|-------------------------------------------------------|
| text  | mixed  | ""            | Text to display (string, {text,color}, or [{t,c},...])|
| icon  | string | null          | Icon name (without extension)                         |
| label | mixed  | ""            | Subtitle below text (same formats as text)            |
| color | mixed  | [255,255,255] | Default text color (hex, RGB, uint)                   |

**Layout types:**

| Zones | Layout                                        |
|-------|-----------------------------------------------|
| 2     | Two horizontal rows (64x31 each)              |
| 3     | Top full-width (64x31) + bottom 2 cols (31x31)|
| 4     | Four quadrants (31x31 each)                   |

- Full-width zones: icon left + text right
- Half-width zones: icon top centered + text below
- 1px dark separator lines between zones
- Text is truncated (no scrolling in multi-zone)
- The `zones` array must have 2, 3, or 4 elements (1 or >4 is rejected)
- Top-level `duration`, `lifetime`, `priority` still apply
- Backward compatible: omitting `zones` keeps the single-zone layout

#### Delete an app

```http
DELETE /api/custom?name={appName}
```

#### List apps

```http
GET /api/apps
```

**Response:**

```json
{
  "apps": [
    {
      "name": "weather",
      "text": "72°F",
      "lifetime": 300,
      "remaining": 245
    }
  ]
}
```

---

### Notifications

#### Send a notification

```http
POST /api/notify
Content-Type: application/json
```

**Payload:**

```json
{
  "text": "New message!",
  "icon": "mail",
  "color": [
    0,
    150,
    255
  ],
  "duration": 5,
  "stack": false,
  "hold": false
}
```

**Specific parameters:**

| Param | Type | Default | Description                    |
|-------|------|---------|--------------------------------|
| stack | bool | false   | Stack with other notifications |
| hold  | bool | false   | Hold until acknowledgment      |

#### Acknowledge notifications

```http
POST /api/dismiss
```

---

### Indicators

#### Control an indicator

```http
POST /api/indicator{1-3}
Content-Type: application/json
```

**Payload:**

```json
{
  "color": [
    255,
    0,
    0
  ],
  "blink": 500,
  "fade": 0
}
```

**Parameters:**

| Param | Type    | Default | Description                       |
|-------|---------|---------|-----------------------------------|
| color | [R,G,B] | -       | Indicator color                   |
| blink | int     | 0       | Blink interval (ms, 0 = disabled) |
| fade  | int     | 0       | Fade duration (ms, 0 = disabled)  |

#### Disable an indicator

```http
POST /api/indicator{1-3}
Content-Type: application/json

{}
```

An empty payload disables the indicator.

---

### Settings

#### Read settings

```http
GET /api/settings
```

#### Modify settings

```http
POST /api/settings
Content-Type: application/json
```

---

### System

#### Statistics

```http
GET /api/stats
```

**Response:**

```json
{
  "version": "0.1.0",
  "uptime": 3600,
  "freeHeap": 120000,
  "brightness": 128,
  "wifi": {
    "ssid": "MyNetwork",
    "rssi": -45,
    "ip": "192.168.1.100"
  },
  "display": {
    "width": 64,
    "height": 64
  },
  "mqtt": {
    "connected": true
  }
}
```

#### Brightness

```http
POST /api/brightness
Content-Type: application/json

{
    "brightness": 200
}
```

#### Reboot

```http
POST /api/reboot
```

---

### Weather

The WeatherClock is a built-in system app that displays current conditions and a multi-day forecast on the matrix. It replaces the default clock when weather data is available and falls back to a simple time display when data is stale (older than 1 hour).

The forecast supports up to **7 days**, displayed in pages of 3 columns with automatic scrolling. Page timing is proportional to the app duration (default 10s).

#### Update weather data

```http
POST /api/weather
Content-Type: application/json
```

**Payload:**

```json
{
  "current": {
    "icon": "w_clear_day",
    "temp": 22,
    "temp_min": 16,
    "temp_max": 29,
    "humidity": 50
  },
  "forecast": [
    { "day": "LUN", "icon": "w_partly_day", "temp_min": 14, "temp_max": 23 },
    { "day": "MAR", "icon": "w_rain", "temp_min": 10, "temp_max": 17 },
    { "day": "MER", "icon": "w_snow", "temp_min": -2, "temp_max": 3 }
  ]
}
```

**Parameters:**

| Param | Type | Required | Description |
|-------|------|----------|-------------|
| current | object | yes | Current weather conditions |
| current.icon | string | yes | Icon name (see built-in icons below) |
| current.temp | int | yes | Current temperature |
| current.temp_min | int | no | Today's min temperature |
| current.temp_max | int | no | Today's max temperature |
| current.humidity | int | no | Humidity percentage |
| forecast | array | no | Forecast days (up to 7) |
| forecast[].day | string | yes | Day abbreviation (3 chars, e.g. "LUN", "MAR") |
| forecast[].icon | string | yes | Icon name |
| forecast[].temp_min | int | yes | Min temperature |
| forecast[].temp_max | int | yes | Max temperature |

**Pagination behavior:**

| Forecast days | Pages | Layout |
|---------------|-------|--------|
| 1-3 | 1 | Single page, no indicator |
| 4-6 | 2 | Auto-scroll, 2 indicator dots |
| 7 | 3 | Auto-scroll, 3 indicator dots (last page centered) |

**Response:**

```json
{
  "success": true
}
```

#### Read weather data

```http
GET /api/weather
```

**Response:**

```json
{
  "valid": true,
  "age": 120,
  "stale": false,
  "current": {
    "icon": "w_clear_day",
    "temp": 22,
    "temp_min": 16,
    "temp_max": 29,
    "humidity": 50
  },
  "forecast": [
    { "day": "LUN", "icon": "w_partly_day", "temp_min": 14, "temp_max": 23 },
    { "day": "MAR", "icon": "w_rain", "temp_min": 10, "temp_max": 17 }
  ]
}
```

| Field | Description |
|-------|-------------|
| valid | Whether weather data has been received |
| age | Seconds since last update |
| stale | True if data is older than 1 hour (display falls back to clock) |

#### Built-in weather icons

These 8x8 pixel icons are stored in PROGMEM (no filesystem needed):

| Name | Description |
|------|-------------|
| `w_clear_day` | Sunny |
| `w_clear_night` | Clear night |
| `w_partly_day` | Partly cloudy (day) |
| `w_partly_night` | Partly cloudy (night) |
| `w_cloudy` | Cloudy |
| `w_rain` | Rain |
| `w_heavy_rain` | Heavy rain |
| `w_thunder` | Thunderstorm |
| `w_snow` | Snow |
| `w_fog` | Fog |

You can also use any icon uploaded to the filesystem (e.g. via `POST /api/icons`).

#### Examples

**curl - 3-day forecast (single page):**

```bash
curl -X POST http://pixelcast.local/api/weather \
  -H "Content-Type: application/json" \
  -d '{
    "current": {
      "icon": "w_clear_day",
      "temp": 22,
      "temp_min": 16,
      "temp_max": 29,
      "humidity": 50
    },
    "forecast": [
      { "day": "LUN", "icon": "w_partly_day", "temp_min": 14, "temp_max": 23 },
      { "day": "MAR", "icon": "w_rain", "temp_min": 10, "temp_max": 17 },
      { "day": "MER", "icon": "w_cloudy", "temp_min": 11, "temp_max": 18 }
    ]
  }'
```

**curl - 7-day forecast (3 pages with auto-scroll):**

```bash
curl -X POST http://pixelcast.local/api/weather \
  -H "Content-Type: application/json" \
  -d '{
    "current": {
      "icon": "w_clear_day",
      "temp": 22,
      "temp_min": 16,
      "temp_max": 29,
      "humidity": 50
    },
    "forecast": [
      { "day": "LUN", "icon": "w_clear_day", "temp_min": 15, "temp_max": 25 },
      { "day": "MAR", "icon": "w_partly_day", "temp_min": 14, "temp_max": 23 },
      { "day": "MER", "icon": "w_rain", "temp_min": 10, "temp_max": 17 },
      { "day": "JEU", "icon": "w_snow", "temp_min": -2, "temp_max": 3 },
      { "day": "VEN", "icon": "w_cloudy", "temp_min": 8, "temp_max": 14 },
      { "day": "SAM", "icon": "w_clear_day", "temp_min": 16, "temp_max": 28 },
      { "day": "DIM", "icon": "w_partly_day", "temp_min": 13, "temp_max": 22 }
    ]
  }'
```

**Home Assistant - Automation (daily weather update):**

```yaml
automation:
  - alias: "Update PixelCast weather"
    trigger:
      - platform: time_pattern
        minutes: "/15"
    action:
      - service: rest_command.pixelcast_weather

rest_command:
  pixelcast_weather:
    url: "http://pixelcast.local/api/weather"
    method: POST
    content_type: "application/json"
    payload: >
      {
        "current": {
          "icon": "w_clear_day",
          "temp": {{ states('sensor.outdoor_temperature') | int }},
          "temp_min": {{ state_attr('weather.home', 'temperature') | int }},
          "temp_max": {{ state_attr('weather.home', 'temperature') | int }},
          "humidity": {{ states('sensor.outdoor_humidity') | int }}
        },
        "forecast": [
          {% for f in state_attr('weather.home', 'forecast')[:7] %}
          {
            "day": "{{ as_timestamp(f.datetime) | timestamp_custom('%a') | upper | truncate(3, true, '') }}",
            "icon": "w_{{ f.condition }}",
            "temp_min": {{ f.templow | int }},
            "temp_max": {{ f.temperature | int }}
          }{% if not loop.last %},{% endif %}
          {% endfor %}
        ]
      }
```

---

### Tracker

The Tracker layout displays financial or metric data (crypto, stocks, currencies) with a dedicated 64x64 layout: symbol + icon, price, change %, sparkline chart, and optional footer text.

Each tracker is registered as a regular app in the rotation (ID prefixed with `tracker_`). Data is pushed externally and displayed until stale (>1 hour), at which point colors dim and a "STALE" badge appears.

#### Create/Update a tracker

```http
POST /api/tracker?name={trackerName}
Content-Type: application/json
```

**Payload:**

```json
{
  "symbol": "BTC",
  "icon": "bitcoin",
  "currency": "USD",
  "value": 98452.30,
  "change": 2.14,
  "sparkline": [92100, 89300, 93200, 91800, 95400, 94100, 97600, 96200, 98452],
  "symbolColor": "#FF8800",
  "sparklineColor": "#00D4FF",
  "bottomText": "Vol 24h: 42B",
  "duration": 10000
}
```

**Parameters:**

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| symbol | string | "" | Display symbol (max 7 chars, e.g. "BTC", "ETH") |
| icon | string | "" | Icon name from /icons folder |
| currency | string | "" | Currency label (max 7 chars, e.g. "USD", "EUR") |
| value | float | 0 | Current price/value |
| change | float | 0 | Change percentage (positive or negative) |
| sparkline | [float] | [] | Up to 24 data points (auto-scaled to chart) |
| symbolColor | color | white | Header color (hex `"#FF8800"`, RGB `[255,136,0]`, or uint32) |
| sparklineColor | color | cyan | Chart line color (same formats) |
| bottomText | string | "" | Optional footer text (max 31 chars) |
| duration | int | 10000 | Display duration in ms |

**Display layout (64x64):**

```
y=0-11   Icon (8x8) + symbol in symbolColor
y=14-22  Price value + currency (right-aligned, dimmed)
y=25-33  Arrow (up/down) + change% (green/red)
y=37     Separator line
y=39     "24h" label (right-aligned)
y=40-53  Sparkline chart (60x14px)
y=55     Separator line
y=57-63  Bottom text (centered)
```

**Response:**

```json
{
  "success": true
}
```

#### Get tracker data

```http
GET /api/tracker?name={trackerName}
```

**Response:**

```json
{
  "name": "btc",
  "symbol": "BTC",
  "icon": "bitcoin",
  "currency": "USD",
  "value": 98452.3,
  "change": 2.14,
  "symbolColor": 16746496,
  "sparklineColor": 54527,
  "bottomText": "Vol 24h: 42B",
  "age": 120,
  "stale": false,
  "sparkline": [20050, 0, 27926, 17901, 43680, 34371, 59434, 49409, 65535]
}
```

#### List all trackers

```http
GET /api/trackers
```

**Response:**

```json
{
  "trackers": [
    { "name": "btc", "symbol": "BTC", "value": 98452.3, "change": 2.14, "age": 120, "stale": false },
    { "name": "eth", "symbol": "ETH", "value": 3245.67, "change": -1.52, "age": 45, "stale": false }
  ],
  "count": 2
}
```

#### Delete a tracker

```http
DELETE /api/tracker?name={trackerName}
```

Removes the tracker data and its app from rotation.

#### Examples

**curl - BTC tracker with sparkline:**

```bash
curl -X POST "http://pixelcast.local/api/tracker?name=btc" \
  -H "Content-Type: application/json" \
  -d '{
    "symbol": "BTC",
    "icon": "bitcoin",
    "currency": "USD",
    "value": 98452.30,
    "change": 2.14,
    "sparkline": [92100, 89300, 93200, 91800, 95400, 94100, 97600, 96200, 98452],
    "symbolColor": "#FF8800",
    "sparklineColor": "#00D4FF",
    "bottomText": "Vol 24h: 42B"
  }'
```

**curl - ETH tracker (negative change):**

```bash
curl -X POST "http://pixelcast.local/api/tracker?name=eth" \
  -H "Content-Type: application/json" \
  -d '{
    "symbol": "ETH",
    "icon": "ethereum",
    "currency": "USD",
    "value": 3245.67,
    "change": -1.52,
    "sparkline": [3400, 3350, 3280, 3310, 3260, 3220, 3250, 3245],
    "symbolColor": "#627EEA",
    "sparklineColor": "#627EEA",
    "bottomText": "Vol 24h: 18B"
  }'
```

---

## MQTT API

All MQTT topics mirror the REST API endpoints. JSON payloads are identical to their REST counterparts described above.

### Setup

You need an MQTT broker (e.g. [Mosquitto](https://mosquitto.org/)) and a client to test. Recommended tools:

- **[MQTTX](https://mqttx.app/)** - Desktop GUI client (open-source, JSON editor, topic management)
- **mosquitto_clients** - CLI tools (`mosquitto_pub` / `mosquitto_sub`) for scripting

```bash
# Install broker + CLI tools (Ubuntu/Debian)
sudo apt install mosquitto mosquitto-clients
```

### Configuration

MQTT is configured via `POST /api/settings` or the settings file. It must be explicitly enabled.

| Parameter | Default     | Description                      |
|-----------|-------------|----------------------------------|
| enabled   | `false`     | Enable/disable MQTT              |
| server    | `""`        | Broker hostname or IP            |
| port      | `1883`      | Broker port                      |
| user      | `""`        | Username (optional)              |
| password  | `""`        | Password (optional)              |
| prefix    | `pixelcast` | Topic prefix for all messages    |

**Example settings payload:**

```json
{
  "mqtt": {
    "enabled": true,
    "server": "192.168.1.10",
    "port": 1883,
    "prefix": "pixelcast"
  }
}
```

### Connection behavior

- Unique client ID derived from ESP32 MAC address
- LWT (Last Will and Testament): publishes `"offline"` (retained) on `{prefix}/status` on disconnect
- On connect: publishes `"online"` (retained) on `{prefix}/status`
- Subscribes to `{prefix}/#` wildcard
- Auto-reconnect every 5 seconds on disconnection
- Stats published every 60 seconds on `{prefix}/stats`

### Topics

All topics below are relative to the configured prefix (default: `pixelcast`).

#### Commands (To Device)

| Topic                      | Payload | Description                          |
|----------------------------|---------|--------------------------------------|
| `{prefix}/custom`          | JSON    | Create/update app (name in JSON)     |
| `{prefix}/custom/{name}`   | JSON    | Create/update app (name in topic)    |
| `{prefix}/notify`          | JSON    | Send notification                    |
| `{prefix}/dismiss`         | (empty) | Dismiss current notification         |
| `{prefix}/indicator{1-3}`  | JSON    | Set indicator (solid, blink, fade)   |
| `{prefix}/weather`         | JSON    | Update weather data                  |
| `{prefix}/tracker`         | JSON    | Create/update tracker (name in JSON) |
| `{prefix}/tracker/{name}`  | JSON    | Create/update tracker (name in topic)|
| `{prefix}/settings`        | JSON    | Update settings                      |
| `{prefix}/brightness`      | JSON    | Set brightness                       |
| `{prefix}/reboot`          | (empty) | Reboot device                        |

#### Status (From Device)

| Topic              | Payload          | Retained | Description                   |
|--------------------|------------------|----------|-------------------------------|
| `{prefix}/status`  | `online`/`offline` | yes    | Connection status (LWT)       |
| `{prefix}/stats`   | JSON             | no       | System stats (every 60s)      |

**Stats payload:**

```json
{
  "uptime": 3600,
  "freeHeap": 120000,
  "brightness": 128,
  "rssi": -45,
  "appCount": 3,
  "version": "0.1.0-dev",
  "currentApp": "weather_clock"
}
```

### Payload reference

JSON payloads are identical to the REST API. Key differences from REST:

- **No HTTP response**: errors are logged to Serial only
- **Name in topic or JSON**: for `/custom` and `/tracker`, the name can be in the topic path (`pixelcast/custom/myapp`) or in the JSON body (`{"name": "myapp", ...}`)
- **Delete via JSON flag**: send `{"delete": true}` (with optional `"name"` if using the base topic) instead of HTTP DELETE

### Examples

#### mosquitto CLI

```bash
# Monitor all messages from the device
mosquitto_sub -h localhost -t "pixelcast/#" -v

# Set brightness
mosquitto_pub -h localhost -t "pixelcast/brightness" \
  -m '{"brightness": 50}'

# Create a custom app
mosquitto_pub -h localhost -t "pixelcast/custom/hello" \
  -m '{"text": "Hello!", "icon": "smiley", "color": "#FF0000"}'

# Create a custom app with text override color (object format)
mosquitto_pub -h localhost -t "pixelcast/custom/alert" \
  -m '{"text": {"text": "CRITICAL", "color": "#FF0000"}, "icon": "warning"}'

# Create a custom app with colored segments (array format)
mosquitto_pub -h localhost -t "pixelcast/custom/temp" \
  -m '{"text": [{"t": "22.5", "c": "#FF8800"}, {"t": "°C", "c": "#666666"}], "icon": "thermo"}'

# Colored segments in both text and label
mosquitto_pub -h localhost -t "pixelcast/custom/server" \
  -m '{
    "text": [{"t": "CPU ", "c": "#AAAAAA"}, {"t": "87%", "c": "#FF2200"}],
    "label": [{"t": "web-", "c": "#666666"}, {"t": "prod", "c": "#00FF88"}],
    "icon": "server"
  }'

# Multi-zone with colored segments per zone
mosquitto_pub -h localhost -t "pixelcast/custom/dashboard" \
  -m '{"zones": [
    {
      "text": [{"t": "22.5", "c": "#FF8800"}, {"t": "°C", "c": "#666666"}],
      "icon": "thermo",
      "label": "Salon"
    },
    {
      "text": [{"t": "58", "c": "#00D4FF"}, {"t": "%", "c": "#666666"}],
      "icon": "humidity",
      "label": [{"t": "Hum ", "c": "#888888"}, {"t": "OK", "c": "#00FF44"}]
    }
  ]}'

# Delete a custom app
mosquitto_pub -h localhost -t "pixelcast/custom/hello" \
  -m '{"delete": true}'

# Send a notification (simple text)
mosquitto_pub -h localhost -t "pixelcast/notify" \
  -m '{"text": "Alert!", "icon": "warning", "color": "#FF0000", "urgent": true}'

# Send a notification with colored segments
mosquitto_pub -h localhost -t "pixelcast/notify" \
  -m '{"text": [{"t": "Door ", "c": "#FFFFFF"}, {"t": "OPEN", "c": "#FF0000"}], "icon": "door", "urgent": true}'

# Dismiss notification
mosquitto_pub -h localhost -t "pixelcast/dismiss" -n

# Set indicator 1 to blinking green
mosquitto_pub -h localhost -t "pixelcast/indicator1" \
  -m '{"mode": "blink", "color": "#00FF00", "blinkInterval": 500}'

# Turn off indicator 2
mosquitto_pub -h localhost -t "pixelcast/indicator2" \
  -m '{"mode": "off"}'

# Update weather
mosquitto_pub -h localhost -t "pixelcast/weather" \
  -m '{
    "current": {"icon": "w_clear_day", "temp": 22, "temp_min": 16, "temp_max": 29, "humidity": 50},
    "forecast": [
      {"day": "LUN", "icon": "w_partly_day", "temp_min": 14, "temp_max": 23},
      {"day": "MAR", "icon": "w_rain", "temp_min": 10, "temp_max": 17}
    ]
  }'

# Update BTC tracker
mosquitto_pub -h localhost -t "pixelcast/tracker/btc" \
  -m '{
    "symbol": "BTC", "icon": "bitcoin", "currency": "USD",
    "value": 98452.30, "change": 2.14,
    "sparkline": [92100, 89300, 93200, 91800, 95400, 94100, 97600, 96200, 98452],
    "symbolColor": "#FF8800", "sparklineColor": "#00D4FF",
    "bottomText": "Vol 24h: 42B"
  }'

# Delete a tracker
mosquitto_pub -h localhost -t "pixelcast/tracker/btc" \
  -m '{"delete": true}'

# Update settings
mosquitto_pub -h localhost -t "pixelcast/settings" \
  -m '{"brightness": 100, "autoRotate": true, "defaultDuration": 15000}'

# Reboot device
mosquitto_pub -h localhost -t "pixelcast/reboot" -n
```

#### Home Assistant - MQTT automations

```yaml
# Notification on high temperature (with colored segments)
automation:
  - alias: "PixelCast temperature alert"
    trigger:
      platform: numeric_state
      entity_id: sensor.temperature
      above: 30
    action:
      service: mqtt.publish
      data:
        topic: "pixelcast/notify"
        payload: >
          {
            "text": [
              {"t": "{{ states('sensor.temperature') }}", "c": "#FF4400"},
              {"t": "°C", "c": "#888888"}
            ],
            "icon": "thermo",
            "urgent": true
          }

# Custom app with colored value + dimmed unit
  - alias: "PixelCast indoor temperature"
    trigger:
      platform: state
      entity_id: sensor.indoor_temperature
    action:
      service: mqtt.publish
      data:
        topic: "pixelcast/custom/indoor"
        payload: >
          {
            "text": [
              {"t": "{{ states('sensor.indoor_temperature') }}", "c": "#FF8800"},
              {"t": "°C", "c": "#666666"}
            ],
            "label": {"text": "Salon", "color": "#888888"},
            "icon": "thermo"
          }

# Weather update every 15 minutes
  - alias: "PixelCast weather update"
    trigger:
      platform: time_pattern
      minutes: "/15"
    action:
      service: mqtt.publish
      data:
        topic: "pixelcast/weather"
        payload: >
          {
            "current": {
              "icon": "w_clear_day",
              "temp": {{ states('sensor.outdoor_temperature') | int }},
              "humidity": {{ states('sensor.outdoor_humidity') | int }}
            }
          }

# BTC price from sensor
  - alias: "PixelCast BTC tracker"
    trigger:
      platform: state
      entity_id: sensor.bitcoin_price
    action:
      service: mqtt.publish
      data:
        topic: "pixelcast/tracker/btc"
        payload: >
          {
            "symbol": "BTC",
            "value": {{ states('sensor.bitcoin_price') | float }},
            "change": {{ states('sensor.bitcoin_change_24h') | float }},
            "symbolColor": "#FF8800"
          }
```

#### Node-RED

```json
{
  "topic": "pixelcast/custom/github",
  "payload": {
    "text": "5 PR",
    "icon": "github",
    "color": "#64C8FF"
  }
}
```

---

## Error Codes

| Code | Description                |
|------|----------------------------|
| 200  | OK                         |
| 400  | Bad Request (invalid JSON) |
| 404  | Not Found                  |
| 500  | Internal Server Error      |

## Rate Limiting

No rate limiting by default. The ESP32 can handle ~10-20 requests/second.
