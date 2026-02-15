# ESP32-PixelCast - Development Roadmap

## Overview

This document details the development phases of the ESP32-PixelCast project, from proof of concept to stable release.

---

## Phase 0: Preparation ✅

### 0.1 Environment Setup
- [x] Create GitHub repository
- [x] Configure PlatformIO
- [x] Test hardware (Trinity + 64x64 panel)
- [x] Validate wiring and power supply

### 0.2 Hardware Validation
- [x] Flash a basic ESP32-HUB75-MatrixPanel-DMA example
- [x] Verify display (colors, orientation, scan type)
- [x] Test brightness and power consumption
- [x] Identify exact pinout (E_PIN for 64x64)

> **Note**: Trinity board uses **GPIO 18** for E_PIN (not GPIO 32).
> Panel model: P3(2121)64X64-32S-T (1/32 scan)

### 0.3 Final Technical Choices
- [x] Validate LittleFS vs SD card → LittleFS (built into ESP32 core)
- [x] Choose color depth (5-6 bits recommended) → 6 bits
- [ ] Define target memory limits

**Deliverables:**
- [x] Working panel with test display
- [x] Validated PlatformIO configuration

---

## Phase 1: Foundations ✅

### 1.1 Base Architecture
- [x] Project structure (src/, include/, lib/)
- [x] Centralized configuration file (`config.h`)
- [x] Logging system (Serial)
- [x] Basic error handling

### 1.2 Display Driver
- [x] Wrapper around ESP32-HUB75-MatrixPanel-DMA
- [x] Dynamic configuration (resolution, pins)
- [x] Double buffering (configurable)
- [x] Brightness control
- [x] Clear/fill functions

### 1.3 WiFi Connectivity
- [x] WiFiManager for initial configuration
- [x] Captive portal
- [x] Credential storage in NVS
- [x] Automatic reconnection
- [x] mDNS (`pixelcast.local`)

### 1.4 LittleFS Filesystem
- [x] LittleFS initialization
- [x] Folder structure (`/icons`, `/gifs`, `/config`)
- [x] JSON configuration read/write
- [x] Available space management

**Deliverables:**
- [x] Working WiFi with captive portal
- [x] Basic text display (time via NTP)
- [x] Persistent configuration

---

## Phase 2: Application System ✅

### 2.1 App Manager
- [x] `AppItem` structure (id, text, icon, color, duration, lifetime, priority)
- [x] Circular application queue (max 16)
- [x] Add/remove/update apps
- [x] Lifetime management (automatic expiration)

### 2.2 Automatic Rotation
- [x] Rotation timer
- [ ] Transitions between apps (cut only for now)
- [x] Respect configured durations
- [x] Skip expired apps

### 2.3 Application Rendering
- [x] Layout: vertical (icon top centered + text below)
- [x] Scrolling text if too long
- [ ] Progress bar (optional)
- [ ] Bar chart (optional)

### 2.4 Built-in System Apps
- [x] **Clock**: Clock with NTP (format 24h/12h configurable, seconds optional)
- [x] **Date**: Current date (multiple formats)
- [x] **IP**: IP display at startup

### 2.5 Persistence
- [x] `saveApps()` / `loadApps()` for custom apps in LittleFS
- [ ] Enable loadApps() at startup (currently disabled)

**Deliverables:**
- [x] Working app rotation
- [x] NTP clock
- [x] Persistence mechanism implemented (activation pending)

---

## Phase 2b: Data Dashboards ✅

### 2b.1 WeatherClock System App
- [x] `WeatherData` structure (current conditions + forecast)
- [x] Up to 7-day forecast support (`MAX_FORECAST_DAYS`)
- [x] REST API: `POST /api/weather` (update), `GET /api/weather` (read)
- [x] Built-in PROGMEM weather icons (no filesystem dependency)
- [x] Dashboard layout: current temp/icon + forecast grid
- [x] Forecast pagination with auto-scroll (3 columns per page)
- [x] Dynamic centering based on column count (1/2/3 columns)
- [x] Page indicator dots (right edge, vertical)
- [x] Stale data detection (>1h fallback to clock)
- [x] Registered as system app (always available)

### 2b.2 Tracker System App
- [x] `TrackerData` structure (symbol, value, change%, sparkline, colors, bottomText)
- [x] Up to 8 concurrent trackers (`MAX_TRACKERS`)
- [x] REST API: `POST /api/tracker`, `GET /api/tracker`, `GET /api/trackers`, `DELETE /api/tracker`
- [x] Dynamic app registration (auto-creates app in rotation on first POST)
- [x] App IDs prefixed with `tracker_` (e.g. `tracker_btc`)
- [x] Sparkline chart (24 points, float array scaled to uint16)
- [x] Layout: icon+symbol / price+currency / arrow+change% / separator / 24h+sparkline / separator / bottomText
- [x] Customizable colors (text, price, sparkline, change, bottomText)
- [x] Stale data detection (>1h: dims colors to 1/4, shows "STALE" badge)
- [x] `parseColorValue()` extracted as reusable helper

**Deliverables:**
- [x] Working weather dashboard with forecast
- [x] Working tracker display with sparkline
- [x] Bruno API collections for both features

---

## Phase 2c: Advanced Display Features ✅

### 2c.1 Multi-Zone Layouts
- [x] `AppZone` structure (text, icon, textColor per zone)
- [x] Up to 4 zones per app (`MAX_ZONES`)
- [x] Layout auto-inferred from zone count (2=dual rows, 3=top+2cols, 4=quad)
- [x] `POST /api/custom` accepts `zones` array (backward compatible)
- [x] Zone rendering: full-width = icon left + text right; half-width = icon top + text below
- [x] Separator lines between zones (dark gray)
- [x] Persistence support for multi-zone apps in saveApps/loadApps
- [x] `GET /api/apps` returns zoneCount + zones data

### 2c.2 Colored Text Segments
- [x] `TextSegment` struct (offset + color), up to 8 segments per text field
- [x] Polymorphic API: plain string, `{text, color}` object, or `[{t,c},...]` array
- [x] `parseTextFieldWithSegments()` / `serializeTextField()` for parse/serialize
- [x] `printTextWithSegments()` for default font with color switching
- [x] `printLabelWithSegments()` for TomThumb font with dimming support
- [x] Applied to both AppItem and AppZone text/label fields

**Deliverables:**
- [x] Multi-zone dashboard layouts
- [x] Per-segment colored text in apps and zones

---

## Phase 3: Notifications ✅

### 3.1 Notification Manager
- [x] FIFO notification queue (max 10)
- [x] Priorities (normal, urgent)
- [x] Stack mode vs replace
- [x] Hold mode (until acknowledgment)

### 3.2 Notification Display
- [x] Interrupt app flow
- [x] Horizontal separator layout
- [ ] Entry animation (slide down)
- [x] Return to apps after expiration
- [x] Dismiss via API (`POST /api/notify/dismiss`)
- [x] List active notifications (`GET /api/notify/list`)

### 3.3 Visual Indicators
- [x] 3 indicator zones (corners)
- [x] Solid color, blinking, fading modes
- [x] Configurable blink interval and fade period
- [x] Independent from main content
- [x] REST API: `POST /api/indicator{1-3}`, `DELETE /api/indicator{1-3}`

**Deliverables:**
- [x] Working push notifications with queue
- [x] 3 configurable indicators with multiple modes

---

## Phase 4: REST API ✅

### 4.1 Async Web Server
- [x] ESPAsyncWebServer setup
- [x] CORS for cross-origin access
- [ ] Basic authentication (optional)

### 4.2 REST Endpoints

| Method | Endpoint | Description | Status |
|--------|----------|-------------|--------|
| POST | `/api/custom` | Create/Update an app (single or multi-zone) | ✅ |
| DELETE | `/api/custom` | Delete an app | ✅ |
| POST | `/api/notify` | Send notification | ✅ |
| POST | `/api/notify/dismiss` | Dismiss current notification | ✅ |
| GET | `/api/notify/list` | List active notifications | ✅ |
| POST | `/api/indicator{1-3}` | Control indicator | ✅ |
| DELETE | `/api/indicator{1-3}` | Turn off indicator | ✅ |
| GET | `/api/apps` | List active apps | ✅ |
| GET | `/api/stats` | System statistics | ✅ |
| POST | `/api/settings` | Modify settings | ✅ |
| GET | `/api/settings` | Read settings | ✅ |
| POST | `/api/reboot` | Reboot | ✅ |
| POST | `/api/brightness` | Set brightness | ✅ |
| POST | `/api/weather` | Update weather data | ✅ |
| GET | `/api/weather` | Read weather data + stale status | ✅ |
| POST | `/api/tracker` | Create/Update a tracker | ✅ |
| GET | `/api/tracker` | Read single tracker data | ✅ |
| GET | `/api/trackers` | List all active trackers | ✅ |
| DELETE | `/api/tracker` | Remove tracker from rotation | ✅ |
| POST | `/api/icons` | Upload icon file | ✅ |
| POST | `/api/lametric` | Download LaMetric icon by ID | ✅ |

> **Note**: Avoid using wildcard patterns (`/api/*`) with HTTP_OPTIONS in ESPAsyncWebServer as it interferes with POST handlers.

### 4.3 JSON Parsing
- [x] ArduinoJson for parsing
- [x] Payload validation
- [x] Standardized error responses

**Deliverables:**
- [x] Complete and documented REST API
- [x] Bruno collections for testing

---

## Phase 5: MQTT

### 5.1 MQTT Client
- [ ] PubSubClient setup
- [ ] Broker configuration (host, port, user, pass)
- [ ] Automatic reconnection
- [ ] Last Will Testament (LWT) for status

### 5.2 Topics

```
pixelcast/
├── custom/{name}     # → Create/Update app
├── notify            # → Notification
├── dismiss           # → Acknowledge
├── indicator{1-3}    # → Indicators
├── weather           # → Update weather data
├── tracker/{name}    # → Create/Update tracker
├── settings          # → Settings
├── brightness        # → Brightness
├── reboot            # → Reboot
├── stats             # ← Statistics (publish)
└── status            # ← Online/Offline (LWT)
```

### 5.3 Home Assistant Integration
- [ ] MQTT Auto-discovery
- [ ] Entities: switch, light, sensor
- [ ] Integration documentation

**Deliverables:**
- Working MQTT
- Tested Home Assistant integration

---

## Phase 6: Media 🔄 (Partial)

### 6.1 Icon Management
- [x] PNG format 8x8 to 32x32
- [x] Loading from LittleFS
- [x] RAM cache (LRU, configurable MAX_ICON_CACHE)
- [x] On-the-fly color conversion (RGB565)
- [x] LaMetric icon download (8x8 native with x2 upscale to 16x16)
- [x] Indexed PNG palette support

### 6.2 Animated GIF Support
- [ ] AnimatedGIF library integration
- [ ] Reading from LittleFS
- [ ] Frame limitation (memory)
- [ ] Adaptive framerate

### 6.3 Media Upload
- [x] REST endpoint for upload (POST /api/icons)
- [x] Format/size validation (PNG/GIF)
- [x] Web interface for management (/icons.html)

### 6.4 Visual Effects
- [ ] Matrix (character rain)
- [ ] Rainbow
- [ ] Fade
- [ ] Pulse

**Deliverables:**
- [x] Working icons with cache and upload
- [ ] Animated GIF support
- [ ] Basic effects

---

## Phase 7: Web Interface

### 7.1 Dashboard
- [ ] Overview (active apps, notifications)
- [ ] Real-time preview (canvas)
- [ ] Quick controls (brightness, on/off)

### 7.2 Configuration
- [ ] WiFi settings
- [ ] MQTT settings
- [ ] Display settings
- [ ] Default apps

### 7.3 Media Management
- [ ] Icons/GIFs list
- [ ] Drag & drop upload
- [ ] Preview
- [ ] Deletion

### 7.4 Logs & Debug
- [ ] Real-time log console
- [ ] Memory/CPU statistics
- [ ] Diagnostic export

**Tech stack:**
- Vanilla HTML/CSS/JS (lightweight)
- WebSocket for real-time
- Stored in LittleFS `/www/`

**Deliverables:**
- Complete web interface
- Real-time preview

---

## Phase 8: Finalization

### 8.1 OTA Updates
- [x] ArduinoOTA for local updates (pio run -e ota -t upload)
- [x] OTA display screen with progress indicator
- [ ] HTTP OTA for web updates (ElegantOTA)
- [ ] Rollback on failure

### 8.2 Stability
- [ ] Load tests (many apps)
- [ ] Memory tests (leaks)
- [ ] Network tests (disconnections)
- [ ] Watchdog

### 8.3 Documentation
- [ ] Complete README
- [ ] Wiki with examples
- [ ] Wiring diagrams
- [ ] Video tutorials (optional)

### 8.4 Release
- [ ] Version 1.0.0
- [ ] Release notes
- [ ] Pre-compiled binaries
- [ ] Web flasher (optional)

**Deliverables:**
- Stable version 1.0.0
- Complete documentation

---

## Future Roadmap (Post v1.0)

### v1.1 - Improvements
- [ ] ESP32-S3 support with PSRAM
- [ ] Chained panels (128x64, etc.)
- [ ] Visual themes
- [ ] Custom fonts

### v1.2 - Integrations
- [ ] Native Jeedom plugin
- [ ] Node-RED nodes
- [ ] Prometheus metrics
- [ ] InfluxDB logging

### v2.0 - Advanced Features
- [ ] Interactive displays (touch/buttons)
- [ ] Audio reactive (VU meter)
- [ ] Camera + QR code
- [ ] Multi-display synchronized

---

## Success Metrics

| Metric | v1.0 Target |
|--------|-------------|
| Free memory | > 50KB |
| Boot time | < 5s |
| API latency | < 100ms |
| Uptime | > 7 days |
| Simultaneous apps | 16 |
| Display FPS | > 30 |

---

## Planned Tests

### Unit Tests
- [ ] App Manager (add, remove, lifetime)
- [ ] Notification queue
- [ ] JSON parsing
- [ ] Colors/gamma

### Integration Tests
- [ ] Complete REST API
- [ ] MQTT pub/sub
- [ ] WiFi reconnection
- [ ] OTA update

### Hardware Tests
- [ ] Different panels (P2, P3, P4)
- [ ] Resolutions (32x32, 64x32, 64x64)
- [ ] Extreme temperatures
- [ ] Power supply limits

---

## Technical Notes

### ESP32 Memory Constraints

```
Total SRAM:     ~320 KB
- System:       ~50 KB
- WiFi:         ~40 KB
- DMA buffer:   ~80 KB (64x64 double buffer)
- Application:  ~100 KB free

Application breakdown:
- App queue:    ~8 KB (16 apps x 512 bytes)
- Notif queue:  ~2 KB
- JSON buffer:  ~4 KB
- Icon cache:   ~16 KB
- GIF decode:   ~32 KB
- Web server:   ~8 KB
- Misc:         ~30 KB
```

### Current Build Stats (February 2026)
- RAM usage: ~37.7% (123 KB / 327 KB)
- Flash usage: ~78.5% (1.54 MB / 1.96 MB)

### Development Priorities

1. **Must have**: Display, WiFi, Apps, REST API
2. **Should have**: MQTT, Notifications, Indicators
3. **Nice to have**: GIFs, Effects, Complete Web UI
4. **Future**: Multi-display, Audio, Touch

---

## Hardware Notes

### Trinity Board Pinout (64x64 panels)

The ESP32-Trinity board has a **specific pinout** for the HUB75 E pin:

```
Pin     GPIO    Note
────────────────────────────────
R1      25
G1      26
B1      27
R2      14
G2      12
B2      13
A       23
B       19
C       5
D       17
E       18      ⚠️ Trinity-specific (not 32!)
LAT     4
OE      15
CLK     16
```

> **Important**: Most documentation shows E_PIN = GPIO 32, but on Trinity
> the HUB75 connector's E pin is wired to **GPIO 18**.

### Panel Compatibility

Tested with:
- **P3(2121)64X64-32S-T** - 64x64, 1/32 scan, 3mm pitch

### Power Supply Requirements

**IMPORTANT**: The HUB75 panel requires adequate power supply!

| Power Source | Current | Result |
|--------------|---------|--------|
| USB only | ~500mA | Only red LEDs work |
| 5V 5A PSU | 5A | All colors work ✅ |

A 64x64 panel at full white brightness can draw up to **8A**. USB power alone (500mA max) is insufficient and will cause color issues (only red channel works).

**Recommended**: Mean Well RS-25-5 (5V 5A) or equivalent.

---

## API Testing

A Bruno collection is available in `api/` folder for testing all REST endpoints.

---

*Last updated: February 15, 2026*
