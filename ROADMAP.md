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

## Phase 1: Foundations 🔄 (In Progress)

### 1.1 Base Architecture
- [x] Project structure (src/, include/, lib/)
- [x] Centralized configuration file (`config.h`)
- [x] Logging system (Serial)
- [ ] Error handling

### 1.2 Display Driver
- [x] Wrapper around ESP32-HUB75-MatrixPanel-DMA
- [x] Dynamic configuration (resolution, pins)
- [ ] Double buffering (disabled for now)
- [x] Brightness control
- [x] Clear/fill functions

```cpp
// Target interface
class Display {
public:
    void begin(uint8_t width, uint8_t height);
    void setBrightness(uint8_t brightness);
    void clear();
    void drawPixel(int16_t x, int16_t y, uint32_t color);
    void drawText(const char* text, int16_t x, int16_t y, uint32_t color);
    void drawIcon(const char* name, int16_t x, int16_t y);
    void swap(); // Double buffer swap
};
```

### 1.3 WiFi Connectivity
- [x] WiFiManager for initial configuration
- [x] Captive portal
- [x] Credential storage in NVS
- [x] Automatic reconnection
- [x] mDNS (`pixelcast.local`)

### 1.4 LittleFS Filesystem
- [x] LittleFS initialization
- [ ] Folder structure (`/icons`, `/gifs`, `/config`)
- [ ] JSON configuration read/write
- [ ] Available space management

**Deliverables:**
- [x] Working WiFi with captive portal
- [x] Basic text display (time via NTP)
- [ ] Persistent configuration

---

## Phase 2: Application System

### 2.1 App Manager
- [ ] `AppItem` structure (id, text, icon, color, duration, lifetime, priority)
- [ ] Circular application queue (max 16-24)
- [ ] Add/remove/update apps
- [ ] Lifetime management (automatic expiration)

```cpp
struct AppItem {
    String id;
    String text;
    String icon;
    uint32_t color;
    uint32_t background;
    uint16_t duration;      // Display duration (ms)
    uint32_t lifetime;      // Expiration (ms), 0 = permanent
    uint32_t lastUpdate;    // Last update timestamp
    int8_t priority;        // -10 to 10
    bool active;
};
```

### 2.2 Automatic Rotation
- [ ] Rotation timer
- [ ] Transitions between apps (cut, fade, slide)
- [ ] Respect configured durations
- [ ] Skip expired apps

### 2.3 Application Rendering
- [ ] Layout: icon (left) + text (right)
- [ ] Scrolling text if too long
- [ ] Progress bar (optional)
- [ ] Bar chart (optional)

### 2.4 Built-in System Apps
- [x] **Clock**: Clock with NTP (basic implementation)
- [ ] **Date**: Current date
- [x] **IP**: IP display at startup

**Deliverables:**
- Working app rotation
- NTP clock
- Apps persistent after reboot

---

## Phase 3: Notifications

### 3.1 Notification Manager
- [ ] FIFO notification queue (max 10)
- [ ] Priorities (normal, urgent)
- [ ] Stack mode vs replace
- [ ] Hold mode (until acknowledgment)

```cpp
struct Notification {
    String text;
    String icon;
    uint32_t color;
    uint16_t duration;
    bool urgent;        // Interrupts immediately
    bool hold;          // Stays displayed until dismiss
    bool stack;         // Stacks with others
};
```

### 3.2 Notification Display
- [ ] Interrupt app flow
- [ ] Entry animation (slide down)
- [ ] Return to apps after expiration
- [ ] Dismiss via API

### 3.3 Visual Indicators
- [ ] 3 zones of 4x4 pixels (corners)
- [ ] Fixed color, blinking, fading
- [ ] Independent from main content

**Deliverables:**
- Working push notifications
- 3 configurable indicators

---

## Phase 4: REST API 🔄 (Partial)

### 4.1 Async Web Server
- [x] ESPAsyncWebServer setup
- [x] CORS for cross-origin access
- [ ] Basic authentication (optional)

### 4.2 REST Endpoints

| Method | Endpoint | Description | Status |
|--------|----------|-------------|--------|
| POST | `/api/custom` | Create/Update an app | ❌ |
| DELETE | `/api/custom` | Delete an app | ❌ |
| POST | `/api/notify` | Send notification | ❌ |
| POST | `/api/dismiss` | Acknowledge notification | ❌ |
| POST | `/api/indicator{1-3}` | Control indicator | ❌ |
| GET | `/api/apps` | List active apps | ❌ |
| GET | `/api/stats` | System statistics | ✅ |
| POST | `/api/settings` | Modify settings | ❌ |
| GET | `/api/settings` | Read settings | ✅ |
| POST | `/api/reboot` | Reboot | ✅ |
| POST | `/api/brightness` | Set brightness | ✅ |

### 4.3 JSON Parsing
- [x] ArduinoJson for parsing
- [ ] Payload validation
- [ ] Standardized error responses

**Deliverables:**
- Complete and documented REST API
- Tests with curl/Postman

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

## Phase 6: Media

### 6.1 Icon Management
- [ ] PNG format 8x8 to 64x64
- [ ] Loading from LittleFS
- [ ] RAM cache (LRU)
- [ ] On-the-fly color conversion

### 6.2 Animated GIF Support
- [ ] AnimatedGIF library integration
- [ ] Reading from LittleFS
- [ ] Frame limitation (memory)
- [ ] Adaptive framerate

### 6.3 Media Upload
- [ ] REST endpoint for upload
- [ ] Format/size validation
- [ ] Web interface for management

### 6.4 Visual Effects
- [ ] Matrix (character rain)
- [ ] Rainbow
- [ ] Fade
- [ ] Pulse

**Deliverables:**
- Working icons and GIFs
- Upload via web
- Basic effects

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
- [ ] ArduinoOTA for local updates
- [ ] HTTP OTA for web updates
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
- App queue:    ~8 KB (16 apps × 512 bytes)
- Notif queue:  ~2 KB
- JSON buffer:  ~4 KB
- Icon cache:   ~16 KB
- GIF decode:   ~32 KB
- Web server:   ~8 KB
- Misc:         ~30 KB
```

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

---

*Last updated: January 31, 2026*
