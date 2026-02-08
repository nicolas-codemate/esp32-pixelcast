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
| text       | string  | ""            | Text to display                                    |
| icon       | string  | null          | Icon name (without extension)                      |
| color      | [R,G,B] | [255,255,255] | Text color                                         |
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

**Response:**

```json
{
  "success": true
}
```

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

## MQTT API

### Configuration

| Parameter | Default value         |
|-----------|-----------------------|
| Prefix    | `pixelcast`           |
| QoS       | 0                     |
| Retain    | false (except status) |

### Topics

#### Commands (To Device)

| Topic                     | Payload | Description          |
|---------------------------|---------|----------------------|
| `pixelcast/custom/{name}` | JSON    | Create/Update an app |
| `pixelcast/notify`        | JSON    | Notification         |
| `pixelcast/dismiss`       | -       | Acknowledge          |
| `pixelcast/indicator1`    | JSON    | Indicator 1          |
| `pixelcast/indicator2`    | JSON    | Indicator 2          |
| `pixelcast/indicator3`    | JSON    | Indicator 3          |
| `pixelcast/settings`      | JSON    | Settings             |
| `pixelcast/brightness`    | int     | Brightness (0-255)   |
| `pixelcast/reboot`        | -       | Reboot               |

#### Status (From Device)

| Topic              | Payload        | Description           |
|--------------------|----------------|-----------------------|
| `pixelcast/status` | online/offline | LWT                   |
| `pixelcast/stats`  | JSON           | Statistics (periodic) |

### Examples

#### Home Assistant - Automation

```yaml
automation:
  - alias: "Temperature notification"
    trigger:
      platform: numeric_state
      entity_id: sensor.temperature
      above: 77
    action:
      service: mqtt.publish
      data:
        topic: "pixelcast/notify"
        payload: >
          {
            "text": "Temperature: {{ states('sensor.temperature') }}°F",
            "icon": "temperature",
            "color": [255, 100, 0]
          }
```

#### Node-RED

```json
{
  "topic": "pixelcast/custom/github",
  "payload": {
    "text": "5 PR",
    "icon": "github",
    "color": [
      100,
      200,
      255
    ]
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
