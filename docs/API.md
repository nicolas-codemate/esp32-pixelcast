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
