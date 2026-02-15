/**
 * ESP32-PixelCast
 *
 * Firmware for HUB75 LED matrix displays
 * Compatible with ESP32 Trinity and 64x64 panels
 *
 * @author nicolas-codemate
 * @license MIT
 * @version 0.1.0
 */

#include <Arduino.h>
#include "config.h"
#include "weather_icons.h"

// Display
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// WiFi & Network
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>

// Web Server
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <AsyncJson.h>

// MQTT
#include <PubSubClient.h>

// JSON
#include <ArduinoJson.h>

// Filesystem
#include <LittleFS.h>

// PNG decoding
#include <PNGdec.h>

// Compact font for small text (IP address, status)
#include <Fonts/TomThumb.h>

// NTP
#include <NTPClient.h>
#include <WiFiUdp.h>

// HTTPS for LaMetric icon download
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// OTA updates
#include <ArduinoOTA.h>

// ============================================================================
// Application System - Structures
// ============================================================================

#define MAX_ZONES 4
#define MAX_TEXT_SEGMENTS 8

struct TextSegment {
    uint8_t offset;   // Visual char index where this color starts
    uint32_t color;   // 0xRRGGBB
};

struct AppZone {
    char text[32];
    char icon[32];
    char label[32];
    uint32_t textColor;
    TextSegment textSegments[MAX_TEXT_SEGMENTS];
    uint8_t textSegmentCount;
    TextSegment labelSegments[MAX_TEXT_SEGMENTS];
    uint8_t labelSegmentCount;
};

struct AppItem {
    char id[24];
    char text[64];
    char icon[32];
    char label[32];
    uint32_t textColor;
    uint16_t duration;          // Display duration in ms
    uint32_t lifetime;          // Expiration time (0 = permanent)
    uint32_t createdAt;         // Creation timestamp
    int8_t priority;            // -10 to 10 (higher = more important)
    uint8_t zoneCount;          // 0 or 1 = single layout, 2/3/4 = multi-zone
    bool active;
    bool isSystem;              // System apps cannot be deleted
    TextSegment textSegments[MAX_TEXT_SEGMENTS];
    uint8_t textSegmentCount;
    TextSegment labelSegments[MAX_TEXT_SEGMENTS];
    uint8_t labelSegmentCount;
    AppZone zones[3];           // zones 1-3 (zone 0 = main text/icon/textColor)
};

// ============================================================================
// Global Objects
// ============================================================================

// Display
MatrixPanel_I2S_DMA *dma_display = nullptr;

// Network
WiFiClient wifiClient;
WiFiManager wifiManager;

// Web Server
AsyncWebServer webServer(WEB_SERVER_PORT);

// MQTT
PubSubClient mqttClient(wifiClient);

// NTP
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER, NTP_OFFSET, NTP_UPDATE_INTERVAL);

// State
bool wifiConnected = false;
bool mqttConnected = false;
bool filesystemReady = false;
uint8_t currentBrightness = DEFAULT_BRIGHTNESS;
bool pendingReboot = false;
unsigned long rebootRequestTime = 0;

// Application Manager
AppItem apps[MAX_APPS];
uint8_t appCount = 0;
int8_t currentAppIndex = -1;
int8_t lastDisplayedAppIndex = -1;  // Track app switches for display clearing
unsigned long lastAppSwitch = 0;
bool appRotationEnabled = true;

// Scroll State
struct ScrollState {
    int16_t scrollOffset;
    unsigned long lastScrollTime;
    uint8_t scrollPhase;  // 0=pause_start, 1=scrolling, 2=pause_end
    bool needsScroll;
    int16_t textWidth;
    int16_t availableWidth;
};
ScrollState appScrollState;

// Icon Cache
struct CachedIcon {
    char name[32];
    uint16_t* pixels;  // RGB565 format
    uint8_t width;
    uint8_t height;
    bool valid;
    unsigned long lastUsed;
};
CachedIcon iconCache[MAX_ICON_CACHE];
PNG png;

// Failed icon download blacklist (prevents retry every frame)
#define MAX_FAILED_ICON_DOWNLOADS 8
#define FAILED_ICON_RETRY_DELAY 300000  // 5 minutes

struct FailedIconDownload {
    char name[32];
    unsigned long failedAt;
};

FailedIconDownload failedIconDownloads[MAX_FAILED_ICON_DOWNLOADS];

// Temporary buffer for PNG decode callback
uint16_t* pngDecodeTarget = nullptr;
uint8_t pngDecodeWidth = 0;

// Settings from JSON
struct Settings {
    uint8_t brightness;
    bool autoRotate;
    uint16_t defaultDuration;
    char ntpServer[48];
    int32_t ntpOffset;
    bool clockEnabled;
    bool clockFormat24h;
    bool clockShowSeconds;
    uint32_t clockColor;
    bool dateEnabled;
    char dateFormat[16];
    uint32_t dateColor;
    bool mqttEnabled;
    char mqttServer[64];
    uint16_t mqttPort;
    char mqttUser[32];
    char mqttPassword[32];
    char mqttPrefix[32];
} settings;

// Weather Data (populated by POST /api/weather)
#define MAX_FORECAST_DAYS 7    // Max storage (1 week)
#define FORECAST_COLUMNS  3    // Columns displayed simultaneously

struct WeatherData {
    char currentIcon[32];
    int16_t currentTemp;
    int16_t currentTempMin;
    int16_t currentTempMax;
    uint8_t currentHumidity;
    struct ForecastDay {
        char icon[32];
        int16_t tempMin;
        int16_t tempMax;
        char dayName[4];  // "LUN", "MAR", etc.
    } forecast[MAX_FORECAST_DAYS];
    uint8_t forecastCount;     // Number of forecast days received
    unsigned long lastUpdate;
    bool valid;
};
WeatherData weatherData;

// Tracker Data (populated by POST /api/tracker)
struct TrackerData {
    char name[16];            // Key: "btc", "eth", "aapl"
    char symbol[8];           // Display: "BTC", "ETH"
    char icon[32];            // Icon name (LittleFS)
    char currencySymbol[8];   // "USD", "EUR"
    float currentValue;       // Price/value
    float changePercent;      // +2.14 or -1.5
    uint16_t sparkline[MAX_SPARKLINE_POINTS];  // Scaled 0-65535
    uint8_t sparklineCount;
    uint32_t symbolColor;     // Header color (0xRRGGBB)
    uint32_t sparklineColor;  // Chart color
    char bottomText[32];      // Optional footer
    unsigned long lastUpdate;
    bool valid;
};
TrackerData trackers[MAX_TRACKERS];
uint8_t trackerCount = 0;

// Indicator Data
enum IndicatorMode : uint8_t {
    INDICATOR_OFF = 0,
    INDICATOR_SOLID = 1,
    INDICATOR_BLINK = 2,
    INDICATOR_FADE = 3
};

struct IndicatorData {
    IndicatorMode mode;
    uint32_t color;          // 0xRRGGBB
    uint16_t blinkInterval;  // ms (default INDICATOR_BLINK_INTERVAL)
    uint16_t fadePeriod;     // ms (default INDICATOR_FADE_PERIOD)
};
IndicatorData indicators[NUM_INDICATORS];

struct IndicatorAnimState {
    unsigned long lastToggle;
    bool blinkOn;
    unsigned long cycleStart;
};
IndicatorAnimState indicatorAnimState[NUM_INDICATORS];

// Notification Data
struct NotificationItem {
    char id[24];              // Unique ID ("notif_<millis>" or user-provided)
    char text[128];           // Notification text (longer than app's 64 chars)
    char icon[32];            // Icon filename
    uint32_t textColor;       // RGB color
    uint32_t backgroundColor; // RGB color for area outside card frame (0 = none)
    uint16_t duration;        // Display duration in ms (0 = hold mode)
    bool hold;                // Explicit hold flag (never auto-expires)
    bool urgent;              // Jumps to front of queue
    bool stack;               // Queue sequentially (true) vs replace current (false)
    bool active;              // Slot in use
    unsigned long displayedAt; // Timestamp when first displayed (0 = not yet shown)
};
NotificationItem notifications[MAX_NOTIFICATIONS];
uint8_t notificationCount = 0;
int8_t currentNotifIndex = -1;
int8_t savedAppIndex = -1;          // App to restore after notifications end
ScrollState notifScrollState;
unsigned long lastNotifScrollUpdate = 0;

// Timing
unsigned long lastStatsPublish = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastTimeUpdate = 0;
unsigned long lastScrollUpdate = 0;

// Forecast pagination
uint8_t forecastPage = 0;
unsigned long lastForecastPageSwitch = 0;

// Weather display cache (global so they can be reset on app switch)
int weatherLastDrawnMinute = -1;
unsigned long weatherLastUpdateDrawn = 0;

// Icon Upload State
File uploadFile;
String uploadIconName;
bool uploadValid = false;
size_t uploadSize = 0;

// Icons Web Interface HTML (stored in PROGMEM to save RAM)
const char ICONS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>PixelCast Icons</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: sans-serif; max-width: 800px; margin: 0 auto; padding: 20px; background: #1a1a2e; color: #eee; }
        h1 { color: #00aaff; }
        h2 { color: #888; border-bottom: 1px solid #333; padding-bottom: 8px; }
        .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(100px, 1fr)); gap: 15px; }
        .icon { text-align: center; padding: 15px; background: #16213e; border-radius: 8px; position: relative; }
        .icon img { width: 48px; height: 48px; image-rendering: pixelated; background: #000; }
        .icon .name { margin-top: 8px; font-size: 12px; word-break: break-all; }
        .icon .size { font-size: 10px; color: #666; }
        .icon button { position: absolute; top: 5px; right: 5px; background: #ff4444; border: none; color: white; width: 20px; height: 20px; border-radius: 50%; cursor: pointer; font-size: 12px; }
        .icon button:hover { background: #ff6666; }
        input, button { padding: 10px 15px; margin: 5px; border: none; border-radius: 4px; }
        input[type="text"], input[type="number"] { background: #0f3460; color: #eee; width: 150px; }
        input[type="file"] { background: #0f3460; color: #eee; }
        button { background: #00aaff; color: white; cursor: pointer; }
        button:hover { background: #0088cc; }
        button:disabled { background: #444; cursor: not-allowed; }
        section { margin-bottom: 30px; padding: 20px; background: #16213e; border-radius: 8px; }
        a { color: #00aaff; }
        .storage { font-size: 12px; color: #888; margin-top: 10px; }
        .msg { padding: 10px; border-radius: 4px; margin: 10px 0; display: none; }
        .msg.success { background: #1a4d1a; color: #4caf50; display: block; }
        .msg.error { background: #4d1a1a; color: #f44336; display: block; }
        .loading { opacity: 0.5; pointer-events: none; }
    </style>
</head>
<body>
    <h1>PixelCast Icons</h1>
    <div id="msg" class="msg"></div>

    <section>
        <h2>Upload Icon</h2>
        <input type="text" id="name" placeholder="Icon name (no extension)">
        <input type="file" id="file" accept=".png,.gif">
        <button onclick="upload()" id="uploadBtn">Upload</button>
    </section>

    <section>
        <h2>Download from LaMetric</h2>
        <input type="number" id="lmId" placeholder="Icon ID (e.g. 2867)">
        <input type="text" id="lmName" placeholder="Save as (optional)">
        <button onclick="downloadLM()" id="lmBtn">Download</button>
        <a href="https://developer.lametric.com/icons" target="_blank">Browse LaMetric Icons</a>
    </section>

    <section>
        <h2>Icon Gallery</h2>
        <div id="gallery" class="grid"></div>
        <div id="storage" class="storage"></div>
    </section>

    <script>
        function showMsg(text, isError) {
            const el = document.getElementById('msg');
            el.textContent = text;
            el.className = 'msg ' + (isError ? 'error' : 'success');
            setTimeout(() => el.className = 'msg', 3000);
        }

        async function load() {
            try {
                const r = await fetch('/api/icons');
                const d = await r.json();
                document.getElementById('gallery').innerHTML = d.icons.length ? d.icons.map(i => `
                    <div class="icon">
                        <button onclick="del('${i.name}')" title="Delete">X</button>
                        <img src="/api/icons/${i.name}" onerror="this.src='data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7'">
                        <div class="name">${i.name}</div>
                        <div class="size">${i.size}B</div>
                    </div>
                `).join('') : '<p style="color:#666">No icons uploaded yet</p>';
                document.getElementById('storage').innerHTML = `Storage: ${d.storage.used} / ${d.storage.total} bytes (${Math.round(d.storage.used/d.storage.total*100)}%)`;
            } catch(e) {
                showMsg('Failed to load icons: ' + e.message, true);
            }
        }

        async function upload() {
            const name = document.getElementById('name').value.trim();
            const file = document.getElementById('file').files[0];
            if (!name) { showMsg('Please enter icon name', true); return; }
            if (!file) { showMsg('Please select a file', true); return; }
            if (file.size > 8192) { showMsg('File too large (max 8KB)', true); return; }

            document.getElementById('uploadBtn').disabled = true;
            try {
                const fd = new FormData();
                fd.append('file', file);
                const r = await fetch('/api/icons?name=' + encodeURIComponent(name), {method: 'POST', body: fd});
                const d = await r.json();
                if (d.success) {
                    showMsg('Icon uploaded successfully', false);
                    document.getElementById('name').value = '';
                    document.getElementById('file').value = '';
                    load();
                } else {
                    showMsg(d.error || 'Upload failed', true);
                }
            } catch(e) {
                showMsg('Upload error: ' + e.message, true);
            }
            document.getElementById('uploadBtn').disabled = false;
        }

        async function downloadLM() {
            const id = parseInt(document.getElementById('lmId').value);
            const name = document.getElementById('lmName').value.trim() || String(id);
            if (!id) { showMsg('Please enter LaMetric icon ID', true); return; }

            document.getElementById('lmBtn').disabled = true;
            try {
                const r = await fetch('/api/icons/lametric', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({id: id, name: name})
                });
                const d = await r.json();
                if (d.success) {
                    showMsg('Icon downloaded from LaMetric', false);
                    document.getElementById('lmId').value = '';
                    document.getElementById('lmName').value = '';
                    load();
                } else {
                    showMsg(d.error || 'Download failed', true);
                }
            } catch(e) {
                showMsg('Download error: ' + e.message, true);
            }
            document.getElementById('lmBtn').disabled = false;
        }

        async function del(name) {
            if (!confirm('Delete icon "' + name + '"?')) return;
            try {
                const r = await fetch('/api/icons?name=' + encodeURIComponent(name), {method: 'DELETE'});
                const d = await r.json();
                if (d.success) {
                    showMsg('Icon deleted', false);
                    load();
                } else {
                    showMsg(d.error || 'Delete failed', true);
                }
            } catch(e) {
                showMsg('Delete error: ' + e.message, true);
            }
        }

        load();
    </script>
</body>
</html>
)rawliteral";

// ============================================================================
// Function Prototypes
// ============================================================================

void setupDisplay();
void setupWiFi();
void setupMDNS();
void setupWebServer();
void setupMQTT();
void setupFilesystem();
void setupApps();

void loopWiFi();
void loopMQTT();
void loopDisplay();
void loopTime();
void loopApps();

void displayShowBoot();
void displayShowIP();
void displayShowTime();
void displayShowDate();
void displayShowApp(AppItem* app);
void displayShowWeatherClock(uint16_t appDuration = 10000);
void drawDropIcon(int16_t x, int16_t y, uint16_t color);
void drawSeparatorLine(int16_t y, uint16_t color);
void drawIconAtScale(CachedIcon* icon, int16_t x, int16_t y, uint8_t scale);
void displayClear();
void displaySetBrightness(uint8_t brightness);

int16_t calculateTextWidth(const char* text);
bool textNeedsScroll(const char* text, int16_t availableWidth);
void resetScrollState();

int pngDrawCallback(PNGDRAW *pDraw);
CachedIcon* loadIcon(const char* name);
CachedIcon* getIcon(const char* name);
int8_t findLRUSlot();
void drawIcon(CachedIcon* icon, int16_t x, int16_t y);
void initIconCache();
void invalidateCachedIcon(const char* name);
bool validatePngHeader(const uint8_t* data, size_t len);
bool validateGifHeader(const uint8_t* data, size_t len);
bool downloadLaMetricIcon(uint32_t iconId, const char* saveName);
void handleApiIconsList(AsyncWebServerRequest *request);
void handleApiIconsServe(AsyncWebServerRequest *request, const String& name);
void handleApiIconsDelete(AsyncWebServerRequest *request);

bool loadSettings();
bool saveSettings();
void initDefaultSettings();
void printTextWithSpecialChars(const char* text, int16_t x, int16_t y);
bool ensureDirectories();
bool loadApps();
bool saveApps();

int8_t appAdd(const char* id, const char* text, const char* icon,
              uint32_t textColor, uint16_t duration,
              uint32_t lifetime, int8_t priority, bool isSystem);
bool appRemove(const char* id);
bool appUpdate(const char* id, const char* text, const char* icon,
               uint32_t textColor);
int8_t appFind(const char* id);
void appCleanExpired();
AppItem* appGetNext();
AppItem* appGetCurrent();
void appSetZones(int8_t appIndex, JsonArray zonesArray);
void displayShowMultiZone(AppItem* app);
void displayShowZone(AppZone* zone, int16_t x, int16_t y, int16_t w, int16_t h);

// Tracker management
TrackerData* trackerFind(const char* name);
TrackerData* trackerAllocate(const char* name);
bool trackerRemove(const char* name);
void trackerInit();
void displayShowTracker(TrackerData* tracker);
void drawSparkline(const uint16_t* data, uint8_t count, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void drawTrackerArrow(int16_t x, int16_t y, bool up, uint16_t color);
void formatTrackerValue(float value, char* buffer, size_t bufSize);
uint32_t parseColorValue(JsonVariant colorVar, uint32_t defaultColor);
void formatColorHex(uint32_t color, char* buffer, size_t bufSize);
void parseTextFieldWithSegments(JsonVariant field, char* textBuffer, size_t textBufferSize,
                                TextSegment* segments, uint8_t* segmentCount, uint32_t defaultColor);
void serializeTextField(JsonObject& obj, const char* fieldName, const char* text,
                        const TextSegment* segments, uint8_t segmentCount);
void printTextWithSegments(const char* text, int16_t x, int16_t y,
                           uint32_t defaultColor, const TextSegment* segments, uint8_t segmentCount);
void printLabelWithSegments(const char* text, int16_t x, int16_t y,
                            uint32_t defaultColor, const TextSegment* segments, uint8_t segmentCount,
                            bool dimDefault);

// Notification management
void notifInit();
int8_t notifAdd(const char* id, const char* text, const char* icon,
                uint32_t textColor, uint32_t bgColor, uint16_t duration,
                bool hold, bool urgent, bool stack);
bool notifDismiss();
void notifClearAll();
NotificationItem* notifGetCurrent();
NotificationItem* notifGetNext();
bool notifIsExpired(NotificationItem* notif);
void displayShowNotification(NotificationItem* notif);
void resetNotifScrollState();

// Indicator management
void indicatorInit();
void indicatorSet(uint8_t index, IndicatorMode mode, uint32_t color,
                  uint16_t blinkInterval, uint16_t fadePeriod);
void indicatorOff(uint8_t index);
void drawIndicators();
bool indicatorNeedsRedraw();
void handleIndicatorApi(AsyncWebServerRequest *request, JsonVariant &json, uint8_t index);

void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttReconnect();
void mqttPublishStats();

void handleApiStats(AsyncWebServerRequest *request);
void handleApiSettings(AsyncWebServerRequest *request);
void handleApiApps(AsyncWebServerRequest *request);

void logMemory();

// ============================================================================
// Setup
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(100);

    Serial.println();
    Serial.println("========================================");
    Serial.println("   ESP32-PixelCast v" VERSION_STRING);
    Serial.println("   HUB75 LED Matrix Display Firmware");
    Serial.println("========================================");
    Serial.println();

    logMemory();

    Serial.println("[INIT] Setting up display...");
    setupDisplay();
    displayShowBoot();

    Serial.println("[INIT] Setting up filesystem...");
    setupFilesystem();

    // Initialize weather data as empty
    memset(&weatherData, 0, sizeof(weatherData));

    // Initialize tracker system
    trackerInit();

    // Initialize notification system
    notifInit();

    // Initialize indicator system (defaults set before loadSettings overrides)
    indicatorInit();

    Serial.println("[INIT] Loading settings...");
    if (!loadSettings()) {
        Serial.println("[INIT] Using default settings");
        initDefaultSettings();
    }
    displaySetBrightness(settings.brightness);

    Serial.println("[INIT] Setting up WiFi...");
    setupWiFi();

    if (wifiConnected) {
        Serial.println("[INIT] Setting up mDNS...");
        setupMDNS();

        Serial.println("[INIT] Setting up web server...");
        setupWebServer();

        Serial.println("[INIT] Setting up MQTT...");
        setupMQTT();

        Serial.println("[INIT] Setting up OTA...");
        ArduinoOTA.setHostname(MDNS_NAME);
        ArduinoOTA.onStart([]() {
            Serial.println("[OTA] Update starting...");
            dma_display->fillScreen(0);
            dma_display->setCursor(4, 28);
            dma_display->setTextColor(dma_display->color565(255, 165, 0));
            dma_display->print("OTA UPDATE");
        });
        ArduinoOTA.onEnd([]() {
            Serial.println("[OTA] Update complete!");
        });
        ArduinoOTA.onError([](ota_error_t error) {
            Serial.printf("[OTA] Error[%u]\n", error);
        });
        ArduinoOTA.begin();

        Serial.println("[INIT] Setting up NTP...");
        timeClient.setPoolServerName(settings.ntpServer);
        timeClient.setTimeOffset(settings.ntpOffset);
        timeClient.begin();

        displayShowIP();
        delay(2000);

        Serial.println("[INIT] Initializing icon cache...");
        initIconCache();

        Serial.println("[INIT] Setting up apps...");
        setupApps();

        // Load demo weather data for development (6 days to test 2-page pagination)
        Serial.println("[INIT] Loading demo weather data (6 days)...");
        strncpy(weatherData.currentIcon, "w_clear_day", sizeof(weatherData.currentIcon));
        weatherData.currentTemp = 18;
        weatherData.currentTempMin = 12;
        weatherData.currentTempMax = 24;
        weatherData.currentHumidity = 65;
        strncpy(weatherData.forecast[0].icon, "w_partly_day", sizeof(weatherData.forecast[0].icon));
        weatherData.forecast[0].tempMin = 12;
        weatherData.forecast[0].tempMax = 22;
        strncpy(weatherData.forecast[0].dayName, "LUN", sizeof(weatherData.forecast[0].dayName));
        strncpy(weatherData.forecast[1].icon, "w_rain", sizeof(weatherData.forecast[1].icon));
        weatherData.forecast[1].tempMin = 8;
        weatherData.forecast[1].tempMax = 15;
        strncpy(weatherData.forecast[1].dayName, "MAR", sizeof(weatherData.forecast[1].dayName));
        strncpy(weatherData.forecast[2].icon, "w_snow", sizeof(weatherData.forecast[2].icon));
        weatherData.forecast[2].tempMin = 0;
        weatherData.forecast[2].tempMax = 6;
        strncpy(weatherData.forecast[2].dayName, "MER", sizeof(weatherData.forecast[2].dayName));
        strncpy(weatherData.forecast[3].icon, "w_clear_day", sizeof(weatherData.forecast[3].icon));
        weatherData.forecast[3].tempMin = 14;
        weatherData.forecast[3].tempMax = 26;
        strncpy(weatherData.forecast[3].dayName, "JEU", sizeof(weatherData.forecast[3].dayName));
        strncpy(weatherData.forecast[4].icon, "w_cloudy", sizeof(weatherData.forecast[4].icon));
        weatherData.forecast[4].tempMin = 10;
        weatherData.forecast[4].tempMax = 19;
        strncpy(weatherData.forecast[4].dayName, "VEN", sizeof(weatherData.forecast[4].dayName));
        strncpy(weatherData.forecast[5].icon, "w_partly_day", sizeof(weatherData.forecast[5].icon));
        weatherData.forecast[5].tempMin = 15;
        weatherData.forecast[5].tempMax = 28;
        strncpy(weatherData.forecast[5].dayName, "SAM", sizeof(weatherData.forecast[5].dayName));
        weatherData.forecastCount = 6;
        weatherData.lastUpdate = millis();
        weatherData.valid = true;
    }

    logMemory();
    Serial.println("[INIT] Setup complete!");
    Serial.println();
}

// ============================================================================
// Main Loop
// ============================================================================

void loop() {
    // Handle pending reboot (allow response to be sent first)
    if (pendingReboot && (millis() - rebootRequestTime > 500)) {
        Serial.println("[SYSTEM] Rebooting...");
        ESP.restart();
    }

    ArduinoOTA.handle();
    loopWiFi();
    loopMQTT();
    loopTime();
    loopApps();
    loopDisplay();

    delay(LOOP_DELAY);
}

// ============================================================================
// Display Functions
// ============================================================================

void setupDisplay() {
    HUB75_I2S_CFG mxconfig(
        PANEL_WIDTH,
        PANEL_HEIGHT,
        PANEL_CHAIN
    );

    // Trinity board pin configuration
    mxconfig.gpio.r1 = R1_PIN;
    mxconfig.gpio.g1 = G1_PIN;
    mxconfig.gpio.b1 = B1_PIN;
    mxconfig.gpio.r2 = R2_PIN;
    mxconfig.gpio.g2 = G2_PIN;
    mxconfig.gpio.b2 = B2_PIN;
    mxconfig.gpio.a = A_PIN;
    mxconfig.gpio.b = B_PIN;
    mxconfig.gpio.c = C_PIN;
    mxconfig.gpio.d = D_PIN;
    mxconfig.gpio.e = E_PIN;  // GPIO 18 on Trinity for 64x64 panels
    mxconfig.gpio.lat = LAT_PIN;
    mxconfig.gpio.oe = OE_PIN;
    mxconfig.gpio.clk = CLK_PIN;

    mxconfig.clkphase = false;
    mxconfig.driver = HUB75_I2S_CFG::SHIFTREG;

    #if DOUBLE_BUFFER
        mxconfig.double_buff = true;
    #endif

    dma_display = new MatrixPanel_I2S_DMA(mxconfig);

    if (!dma_display->begin()) {
        Serial.println("[ERROR] Display init failed!");
        while (true) { delay(1000); }
    }

    dma_display->setBrightness8(currentBrightness);
    dma_display->setTextWrap(false);  // Prevent ghost characters on text scroll
    dma_display->clearScreen();

    Serial.printf("[DISPLAY] Initialized %dx%d panel (E_PIN=%d)\n", PANEL_WIDTH, PANEL_HEIGHT, E_PIN);
}

void displayShowBoot() {
    dma_display->clearScreen();
    dma_display->setTextColor(dma_display->color565(0, 150, 255));
    dma_display->setTextSize(1);
    dma_display->setCursor(4, 24);
    dma_display->print("PixelCast");
    dma_display->setCursor(4, 36);
    dma_display->setTextColor(dma_display->color565(100, 100, 100));
    dma_display->print("v" VERSION_STRING);

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

void displayShowIP() {
    dma_display->clearScreen();

    // "WiFi OK" in default font, centered
    dma_display->setFont(NULL);
    dma_display->setTextSize(1);
    dma_display->setTextColor(dma_display->color565(0, 255, 0));
    dma_display->setCursor(11, 12);
    dma_display->print("WiFi OK");

    // IP address in default font, split across 2 lines for readability
    // e.g. "192.168" on line 1, "1.100" on line 2
    String ip = WiFi.localIP().toString();
    dma_display->setTextColor(dma_display->color565(255, 255, 255));

    // Find the second dot to split the IP into 2 halves
    int firstDot = ip.indexOf('.');
    int secondDot = ip.indexOf('.', firstDot + 1);
    String line1 = ip.substring(0, secondDot);
    String line2 = ip.substring(secondDot + 1);

    // Line 1: first two octets (NULL font, 6px per char)
    int16_t line1Width = line1.length() * 6;
    int16_t line1X = (DISPLAY_WIDTH - line1Width) / 2;
    dma_display->setCursor(line1X, 28);
    dma_display->print(line1);

    // Line 2: last two octets
    int16_t line2Width = line2.length() * 6;
    int16_t line2X = (DISPLAY_WIDTH - line2Width) / 2;
    dma_display->setCursor(line2X, 40);
    dma_display->print(line2);

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif

    delay(3000);
}

void displayShowTime() {
    dma_display->clearScreen();

    // Get time
    int hours = timeClient.getHours();
    int minutes = timeClient.getMinutes();
    int seconds = timeClient.getSeconds();

    // Apply 12h format if configured
    if (!settings.clockFormat24h && hours > 12) {
        hours -= 12;
    }

    // Format time string
    char timeStr[9];
    if (settings.clockShowSeconds) {
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", hours, minutes, seconds);
    } else {
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d", hours, minutes);
    }

    // Extract RGB from color
    uint8_t r = (settings.clockColor >> 16) & 0xFF;
    uint8_t g = (settings.clockColor >> 8) & 0xFF;
    uint8_t b = settings.clockColor & 0xFF;

    // Draw time centered
    dma_display->setTextColor(dma_display->color565(r, g, b));
    dma_display->setTextSize(1);

    // Center text based on format
    int textWidth = settings.clockShowSeconds ? 48 : 30;
    int xPos = (DISPLAY_WIDTH - textWidth) / 2;
    dma_display->setCursor(xPos, 28);
    dma_display->print(timeStr);

    drawIndicators();

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

void displayShowDate() {
    dma_display->clearScreen();

    // Get date from NTP epoch
    unsigned long epochTime = timeClient.getEpochTime();
    struct tm* timeinfo = gmtime((time_t*)&epochTime);

    uint8_t day = timeinfo->tm_mday;
    uint8_t month = timeinfo->tm_mon + 1;
    uint16_t year = timeinfo->tm_year + 1900;

    // Format date string based on settings
    char dateStr[16];
    if (strcmp(settings.dateFormat, "MM/DD/YYYY") == 0) {
        snprintf(dateStr, sizeof(dateStr), "%02u/%02u/%04u", month, day, year);
    } else if (strcmp(settings.dateFormat, "YYYY-MM-DD") == 0) {
        snprintf(dateStr, sizeof(dateStr), "%04u-%02u-%02u", year, month, day);
    } else {
        // Default: DD/MM/YYYY
        snprintf(dateStr, sizeof(dateStr), "%02u/%02u/%04u", day, month, year);
    }

    // Extract RGB from color
    uint8_t r = (settings.dateColor >> 16) & 0xFF;
    uint8_t g = (settings.dateColor >> 8) & 0xFF;
    uint8_t b = settings.dateColor & 0xFF;

    // Draw date centered
    dma_display->setTextColor(dma_display->color565(r, g, b));
    dma_display->setTextSize(1);

    int textWidth = 60;
    int xPos = (DISPLAY_WIDTH - textWidth) / 2;
    dma_display->setCursor(xPos, 28);
    dma_display->print(dateStr);

    drawIndicators();

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

// Draw icon at explicit scale (1 = native, 2 = upscale x2)
void drawIconAtScale(CachedIcon* icon, int16_t x, int16_t y, uint8_t scale) {
    if (!icon || !icon->valid || !icon->pixels) return;

    for (uint8_t py = 0; py < icon->height; py++) {
        for (uint8_t px = 0; px < icon->width; px++) {
            uint16_t pixel = icon->pixels[py * icon->width + px];
            if (pixel != 0) {
                if (scale == 2) {
                    int16_t dx = x + px * 2;
                    int16_t dy = y + py * 2;
                    dma_display->drawPixel(dx, dy, pixel);
                    dma_display->drawPixel(dx + 1, dy, pixel);
                    dma_display->drawPixel(dx, dy + 1, pixel);
                    dma_display->drawPixel(dx + 1, dy + 1, pixel);
                } else {
                    dma_display->drawPixel(x + px, y + py, pixel);
                }
            }
        }
    }
}

// Draw a small water drop icon (5px tall)
void drawDropIcon(int16_t x, int16_t y, uint16_t color) {
    //   .X.
    //   .X.
    //   XXX
    //   XXX
    //   .X.
    dma_display->drawPixel(x + 1, y,     color);
    dma_display->drawPixel(x + 1, y + 1, color);
    dma_display->drawPixel(x,     y + 2, color);
    dma_display->drawPixel(x + 1, y + 2, color);
    dma_display->drawPixel(x + 2, y + 2, color);
    dma_display->drawPixel(x,     y + 3, color);
    dma_display->drawPixel(x + 1, y + 3, color);
    dma_display->drawPixel(x + 2, y + 3, color);
    dma_display->drawPixel(x + 1, y + 4, color);
}

// Draw a thin horizontal separator line
void drawSeparatorLine(int16_t y, uint16_t color) {
    for (int16_t x = 4; x < DISPLAY_WIDTH - 4; x++) {
        dma_display->drawPixel(x, y, color);
    }
}

// ============================================================================
// Tracker Functions
// ============================================================================

TrackerData* trackerFind(const char* name) {
    for (uint8_t i = 0; i < MAX_TRACKERS; i++) {
        if (trackers[i].valid && strcmp(trackers[i].name, name) == 0) {
            return &trackers[i];
        }
    }
    return nullptr;
}

TrackerData* trackerAllocate(const char* name) {
    // Check if already exists
    TrackerData* existing = trackerFind(name);
    if (existing) return existing;

    // Find first free slot
    for (uint8_t i = 0; i < MAX_TRACKERS; i++) {
        if (!trackers[i].valid) {
            memset(&trackers[i], 0, sizeof(TrackerData));
            strlcpy(trackers[i].name, name, sizeof(trackers[i].name));
            trackers[i].symbolColor = 0xFFFFFF;    // Default white
            trackers[i].sparklineColor = 0x00D4FF;  // Default cyan
            trackers[i].valid = true;
            trackerCount++;
            return &trackers[i];
        }
    }
    return nullptr;
}

bool trackerRemove(const char* name) {
    TrackerData* tracker = trackerFind(name);
    if (!tracker) return false;

    tracker->valid = false;
    trackerCount--;

    // Remove corresponding app from rotation
    char appId[32];
    snprintf(appId, sizeof(appId), "%s%s", TRACKER_ID_PREFIX, name);
    appRemove(appId);

    Serial.printf("[TRACKER] Removed: %s\n", name);
    return true;
}

void trackerInit() {
    memset(trackers, 0, sizeof(trackers));
    trackerCount = 0;
    Serial.println("[TRACKER] Initialized");
}

// ============================================================================
// Notification Queue Management
// ============================================================================

void notifInit() {
    memset(notifications, 0, sizeof(notifications));
    notificationCount = 0;
    currentNotifIndex = -1;
    savedAppIndex = -1;
    memset(&notifScrollState, 0, sizeof(notifScrollState));
    Serial.println("[NOTIF] Initialized");
}

int8_t notifAdd(const char* id, const char* text, const char* icon,
                uint32_t textColor, uint32_t bgColor, uint16_t duration,
                bool hold, bool urgent, bool stack) {
    // Replace mode: clear all existing notifications first
    if (!stack) {
        notifClearAll();
    }

    // Find a free slot
    int8_t freeSlot = -1;
    for (uint8_t i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (!notifications[i].active) {
            freeSlot = i;
            break;
        }
    }

    if (freeSlot < 0) {
        Serial.println("[NOTIF] Queue full, dropping notification");
        return -1;
    }

    NotificationItem* notif = &notifications[freeSlot];
    memset(notif, 0, sizeof(NotificationItem));

    // Generate ID if not provided
    if (id && strlen(id) > 0) {
        strlcpy(notif->id, id, sizeof(notif->id));
    } else {
        snprintf(notif->id, sizeof(notif->id), "notif_%lu", millis());
    }

    strlcpy(notif->text, text, sizeof(notif->text));
    if (icon) {
        strlcpy(notif->icon, icon, sizeof(notif->icon));
    }

    notif->textColor = textColor;
    notif->backgroundColor = bgColor;
    notif->duration = duration;
    notif->hold = hold;
    notif->urgent = urgent;
    notif->stack = stack;
    notif->active = true;
    notif->displayedAt = 0;  // Not yet shown

    notificationCount++;

    // If urgent, force it to be picked up next
    if (urgent) {
        currentNotifIndex = freeSlot;
    }

    Serial.printf("[NOTIF] Added: %s (duration=%d, hold=%d, urgent=%d, stack=%d)\n",
                  notif->id, duration, hold, urgent, stack);
    return freeSlot;
}

bool notifDismiss() {
    if (currentNotifIndex < 0 || !notifications[currentNotifIndex].active) {
        return false;
    }

    Serial.printf("[NOTIF] Dismissed: %s\n", notifications[currentNotifIndex].id);
    notifications[currentNotifIndex].active = false;
    notificationCount--;
    currentNotifIndex = -1;
    return true;
}

void notifClearAll() {
    for (uint8_t i = 0; i < MAX_NOTIFICATIONS; i++) {
        notifications[i].active = false;
    }
    notificationCount = 0;
    currentNotifIndex = -1;
    Serial.println("[NOTIF] Cleared all");
}

NotificationItem* notifGetCurrent() {
    if (currentNotifIndex >= 0 && notifications[currentNotifIndex].active) {
        return &notifications[currentNotifIndex];
    }
    return nullptr;
}

NotificationItem* notifGetNext() {
    if (notificationCount == 0) return nullptr;

    // First pass: find urgent notifications not yet displayed
    for (uint8_t i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (notifications[i].active && notifications[i].urgent && notifications[i].displayedAt == 0) {
            currentNotifIndex = i;
            return &notifications[i];
        }
    }

    // Second pass: find any active notification not yet displayed
    for (uint8_t i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (notifications[i].active && notifications[i].displayedAt == 0) {
            currentNotifIndex = i;
            return &notifications[i];
        }
    }

    return nullptr;
}

bool notifIsExpired(NotificationItem* notif) {
    if (!notif || !notif->active) return true;
    if (notif->hold || notif->duration == 0) return false;
    if (notif->displayedAt == 0) return false;  // Not yet shown
    return (millis() - notif->displayedAt) > notif->duration;
}

// Parse color from JSON (hex string "#FF8800", RGB array [255,136,0], or raw uint32)
uint32_t parseColorValue(JsonVariant colorVar, uint32_t defaultColor) {
    if (colorVar.isNull()) return defaultColor;

    if (colorVar.is<JsonArray>()) {
        JsonArray arr = colorVar.as<JsonArray>();
        if (arr.size() == 3) {
            return ((uint32_t)arr[0].as<uint8_t>() << 16) |
                   ((uint32_t)arr[1].as<uint8_t>() << 8) |
                   (uint32_t)arr[2].as<uint8_t>();
        }
    } else if (colorVar.is<const char*>()) {
        const char* str = colorVar.as<const char*>();
        if (str[0] == '#') str++;
        return strtoul(str, NULL, 16);
    } else {
        return colorVar.as<uint32_t>();
    }
    return defaultColor;
}

// Format a uint32 color (0xRRGGBB) as hex string "#RRGGBB" into a buffer
void formatColorHex(uint32_t color, char* buffer, size_t bufSize) {
    snprintf(buffer, bufSize, "#%02X%02X%02X",
             (uint8_t)((color >> 16) & 0xFF),
             (uint8_t)((color >> 8) & 0xFF),
             (uint8_t)(color & 0xFF));
}

// Parse polymorphic text field: string, {text,color} object, or [{t,c},...] array
void parseTextFieldWithSegments(JsonVariant field, char* textBuffer, size_t textBufferSize,
                                TextSegment* segments, uint8_t* segmentCount, uint32_t defaultColor) {
    *segmentCount = 0;
    textBuffer[0] = '\0';

    if (field.isNull()) return;

    // Simple string: "text"
    if (field.is<const char*>()) {
        strlcpy(textBuffer, field.as<const char*>(), textBufferSize);
        return;
    }

    // Object with text and color: {"text": "hello", "color": "#FF0000"}
    if (field.is<JsonObject>()) {
        JsonObject obj = field.as<JsonObject>();
        strlcpy(textBuffer, obj["text"] | "", textBufferSize);
        if (!obj["color"].isNull()) {
            segments[0].offset = 0;
            segments[0].color = parseColorValue(obj["color"], defaultColor);
            *segmentCount = 1;
        }
        return;
    }

    // Array of segments: [{"t": "22.5", "c": "#FF8800"}, {"t": "C", "c": "#666666"}]
    if (field.is<JsonArray>()) {
        JsonArray arr = field.as<JsonArray>();
        size_t pos = 0;
        uint8_t count = 0;
        for (JsonObject seg : arr) {
            if (count >= MAX_TEXT_SEGMENTS) break;
            const char* t = seg["t"] | "";
            size_t tLen = strlen(t);
            if (pos + tLen >= textBufferSize) break;

            // Record segment offset and color
            segments[count].offset = (uint8_t)pos;
            segments[count].color = parseColorValue(seg["c"], defaultColor);
            count++;

            // Concatenate text
            memcpy(textBuffer + pos, t, tLen);
            pos += tLen;
        }
        textBuffer[pos] = '\0';
        *segmentCount = count;
        return;
    }
}

// Serialize text field in polymorphic format for JSON output
void serializeTextField(JsonObject& obj, const char* fieldName, const char* text,
                        const TextSegment* segments, uint8_t segmentCount) {
    if (segmentCount == 0) {
        obj[fieldName] = text;
        return;
    }

    // Build array of {t, c} segments
    JsonArray arr = obj[fieldName].to<JsonArray>();
    size_t textLen = strlen(text);
    for (uint8_t i = 0; i < segmentCount; i++) {
        JsonObject seg = arr.add<JsonObject>();
        // Extract substring from offset to next segment (or end)
        size_t start = segments[i].offset;
        size_t end = (i + 1 < segmentCount) ? segments[i + 1].offset : textLen;
        if (start >= textLen) break;
        if (end > textLen) end = textLen;

        // Copy substring
        char buf[64];
        size_t len = end - start;
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, text + start, len);
        buf[len] = '\0';
        seg["t"] = (const char*)buf;

        char colorHex[8];
        formatColorHex(segments[i].color, colorHex, sizeof(colorHex));
        seg["c"] = (const char*)colorHex;
    }
}

// Draw sparkline chart from scaled uint16 data
void drawSparkline(const uint16_t* data, uint8_t count, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (count < 2) return;

    // Find data min/max for vertical scaling
    uint16_t dataMin = 65535;
    uint16_t dataMax = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (data[i] < dataMin) dataMin = data[i];
        if (data[i] > dataMax) dataMax = data[i];
    }

    uint16_t dataRange = dataMax - dataMin;
    if (dataRange == 0) dataRange = 1;

    // Plot line segments between consecutive points
    for (uint8_t i = 0; i < count - 1; i++) {
        int16_t x0 = x + (int32_t)i * (w - 1) / (count - 1);
        int16_t x1 = x + (int32_t)(i + 1) * (w - 1) / (count - 1);
        // Invert Y: high value = top of chart
        int16_t y0 = y + h - 1 - (int32_t)(data[i] - dataMin) * (h - 1) / dataRange;
        int16_t y1 = y + h - 1 - (int32_t)(data[i + 1] - dataMin) * (h - 1) / dataRange;
        dma_display->drawLine(x0, y0, x1, y1, color);
    }
}

// Draw a small up/down arrow (5x5 pixels)
void drawTrackerArrow(int16_t x, int16_t y, bool up, uint16_t color) {
    if (up) {
        //   ..X..
        //   .XXX.
        //   XXXXX
        //   ..X..
        //   ..X..
        dma_display->drawPixel(x + 2, y,     color);
        dma_display->drawPixel(x + 1, y + 1, color);
        dma_display->drawPixel(x + 2, y + 1, color);
        dma_display->drawPixel(x + 3, y + 1, color);
        for (int16_t i = 0; i < 5; i++) dma_display->drawPixel(x + i, y + 2, color);
        dma_display->drawPixel(x + 2, y + 3, color);
        dma_display->drawPixel(x + 2, y + 4, color);
    } else {
        //   ..X..
        //   ..X..
        //   XXXXX
        //   .XXX.
        //   ..X..
        dma_display->drawPixel(x + 2, y,     color);
        dma_display->drawPixel(x + 2, y + 1, color);
        for (int16_t i = 0; i < 5; i++) dma_display->drawPixel(x + i, y + 2, color);
        dma_display->drawPixel(x + 1, y + 3, color);
        dma_display->drawPixel(x + 2, y + 3, color);
        dma_display->drawPixel(x + 3, y + 3, color);
        dma_display->drawPixel(x + 2, y + 4, color);
    }
}

// Smart value formatting with thousand separators
void formatTrackerValue(float value, char* buffer, size_t bufSize) {
    if (value >= 1000.0f) {
        // Integer with thousand separator
        uint32_t intVal = (uint32_t)value;
        // Build string with commas from right to left
        char tmp[20];
        int len = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)intVal);
        int commas = (len - 1) / 3;
        int outLen = len + commas;
        if ((size_t)outLen >= bufSize) {
            snprintf(buffer, bufSize, "%lu", (unsigned long)intVal);
            return;
        }
        buffer[outLen] = '\0';
        int srcIdx = len - 1;
        int dstIdx = outLen - 1;
        int digitCount = 0;
        while (srcIdx >= 0) {
            buffer[dstIdx--] = tmp[srcIdx--];
            digitCount++;
            if (digitCount % 3 == 0 && srcIdx >= 0) {
                buffer[dstIdx--] = ',';
            }
        }
    } else if (value >= 1.0f) {
        snprintf(buffer, bufSize, "%.2f", value);
    } else {
        snprintf(buffer, bufSize, "%.5f", value);
    }
}

// Display tracker layout on 64x64 matrix
void displayShowTracker(TrackerData* tracker) {
    if (!tracker) return;

    dma_display->clearScreen();

    unsigned long trackerAge = millis() - tracker->lastUpdate;
    bool isStale = (trackerAge > TRACKER_STALE_TIMEOUT);

    // Color helpers
    uint16_t white = dma_display->color565(255, 255, 255);
    uint16_t dimWhite = isStale ? dma_display->color565(60, 60, 60) : dma_display->color565(150, 150, 150);
    uint16_t dimGray = dma_display->color565(40, 40, 40);
    uint16_t green = isStale ? dma_display->color565(0, 60, 0) : dma_display->color565(0, 200, 0);
    uint16_t red = isStale ? dma_display->color565(60, 0, 0) : dma_display->color565(200, 0, 0);

    uint8_t symR = (tracker->symbolColor >> 16) & 0xFF;
    uint8_t symG = (tracker->symbolColor >> 8) & 0xFF;
    uint8_t symB = tracker->symbolColor & 0xFF;
    uint16_t symbolColor565 = isStale
        ? dma_display->color565(symR / 4, symG / 4, symB / 4)
        : dma_display->color565(symR, symG, symB);

    uint8_t spkR = (tracker->sparklineColor >> 16) & 0xFF;
    uint8_t spkG = (tracker->sparklineColor >> 8) & 0xFF;
    uint8_t spkB = tracker->sparklineColor & 0xFF;
    uint16_t sparklineColor565 = isStale
        ? dma_display->color565(spkR / 4, spkG / 4, spkB / 4)
        : dma_display->color565(spkR, spkG, spkB);

    uint16_t valueColor = isStale ? dma_display->color565(60, 60, 60) : white;

    // --- Row 1: Icon + Symbol (y=0..11) ---
    CachedIcon* icon = nullptr;
    if (strlen(tracker->icon) > 0) {
        icon = getIcon(tracker->icon);
    }
    if (icon && icon->valid) {
        // Draw icon at native 8x8 at (2, 2)
        drawIconAtScale(icon, 2, 2, 1);
    }

    // Symbol text at (13, 4) in symbolColor
    dma_display->setFont(NULL);  // Default 5x7 font
    dma_display->setTextSize(1);
    dma_display->setTextColor(symbolColor565);
    dma_display->setCursor(13, 4);
    dma_display->print(tracker->symbol);

    // --- Row 2: Price value (y=14..22) ---
    char valueBuf[20];
    formatTrackerValue(tracker->currentValue, valueBuf, sizeof(valueBuf));
    dma_display->setTextColor(valueColor);
    dma_display->setCursor(2, 16);
    dma_display->print(valueBuf);

    // Currency symbol right-aligned in TomThumb
    if (strlen(tracker->currencySymbol) > 0) {
        dma_display->setFont(&TomThumb);
        dma_display->setTextColor(dimWhite);
        int16_t currWidth = strlen(tracker->currencySymbol) * 4;
        dma_display->setCursor(62 - currWidth, 22);
        dma_display->print(tracker->currencySymbol);
        dma_display->setFont(NULL);  // Reset to default
    }

    // --- Row 3: Arrow + Change % (y=25..33) ---
    bool isPositive = (tracker->changePercent >= 0.0f);
    uint16_t changeColor = isPositive ? green : red;

    drawTrackerArrow(2, 27, isPositive, changeColor);

    char changeBuf[16];
    snprintf(changeBuf, sizeof(changeBuf), "%s%.2f%%",
             isPositive ? "+" : "", tracker->changePercent);
    dma_display->setTextColor(changeColor);
    dma_display->setCursor(9, 27);
    dma_display->print(changeBuf);

    // --- Separator line (y=37) ---
    drawSeparatorLine(37, dimGray);

    // --- "24h" label right-aligned (y=39) ---
    dma_display->setFont(&TomThumb);
    dma_display->setTextColor(dimWhite);
    dma_display->setCursor(51, 43);
    dma_display->print("24h");
    dma_display->setFont(NULL);

    // --- Sparkline chart (y=40..53, x=2..61) ---
    if (tracker->sparklineCount >= 2) {
        drawSparkline(tracker->sparkline, tracker->sparklineCount,
                      2, 40, 60, 14, sparklineColor565);
    }

    // --- Separator line (y=55) ---
    drawSeparatorLine(55, dimGray);

    // --- Bottom text centered (y=57..63) ---
    if (strlen(tracker->bottomText) > 0) {
        dma_display->setFont(&TomThumb);
        dma_display->setTextColor(dimWhite);
        int16_t textWidth = strlen(tracker->bottomText) * 4;
        int16_t textX = (DISPLAY_WIDTH - textWidth) / 2;
        dma_display->setCursor(textX, 62);
        dma_display->print(tracker->bottomText);
        dma_display->setFont(NULL);
    }

    // --- Stale badge ---
    if (isStale) {
        uint16_t staleRed = dma_display->color565(200, 0, 0);
        dma_display->setFont(&TomThumb);
        dma_display->setTextColor(staleRed);
        dma_display->setCursor(42, 6);
        dma_display->print("STALE");
        dma_display->setFont(NULL);
    }

    drawIndicators();

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

void displayShowWeatherClock(uint16_t appDuration) {
    // Fallback to time display if weather data is stale or missing
    unsigned long weatherAge = millis() - weatherData.lastUpdate;
    if (!weatherData.valid || weatherAge > 3600000) {
        displayShowTime();
        return;
    }

    // Use global weatherLastDrawnMinute / weatherLastUpdateDrawn
    // (reset by displayShowApp on app switch to force full redraw)

    int hours = timeClient.getHours();
    int minutes = timeClient.getMinutes();
    int seconds = timeClient.getSeconds();

    if (!settings.clockFormat24h && hours > 12) {
        hours -= 12;
    }

    bool needsFullRedraw = (weatherLastDrawnMinute != minutes) ||
                           (weatherLastUpdateDrawn != weatherData.lastUpdate);

    // Forecast pagination
    uint8_t forecastPageCount = max((uint8_t)1,
        (uint8_t)((weatherData.forecastCount + FORECAST_COLUMNS - 1) / FORECAST_COLUMNS));
    unsigned long pageInterval = (unsigned long)appDuration / forecastPageCount;

    bool pageChanged = false;
    if (forecastPageCount > 1) {
        unsigned long now = millis();
        if (now - lastForecastPageSwitch >= pageInterval) {
            forecastPage = (forecastPage + 1) % forecastPageCount;
            lastForecastPageSwitch = now;
            pageChanged = true;
        }
    }

    bool needsForecastRedraw = needsFullRedraw || pageChanged;

    uint16_t white = dma_display->color565(255, 255, 255);
    uint16_t dimGray = dma_display->color565(40, 40, 40);
    uint16_t cyan = dma_display->color565(0, 180, 255);
    uint16_t mintGreen = dma_display->color565(100, 255, 180);
    uint16_t gray = dma_display->color565(140, 140, 140);
    uint16_t coral = dma_display->color565(255, 140, 100);
    uint16_t coldBlue = dma_display->color565(80, 140, 255);
    uint16_t warmRed = dma_display->color565(255, 50, 30);
    uint16_t black = dma_display->color565(0, 0, 0);

    // ============================================================
    // Layout map (64x64 display)
    // NULL font: setCursor = top-left of glyph, char is 7px tall
    // TomThumb: setCursor = baseline, uppercase chars 5px above baseline
    // ============================================================
    // y=0-8:    current weather (icon 8x8 + temp + min/max)
    // y=10:     separator
    // y=13-19:  HH:MM (NULL font top=13) + :SS (TomThumb baseline=20)
    // y=22-28:  date (NULL font top=22)
    // y=31:     separator
    // y=32-63:  forecast (32px) - paginated by FORECAST_COLUMNS
    //   y=39:     day names (TomThumb baseline=39)
    //   y=41-48:  forecast icons (8x8)
    //   y=56:     min temps (TomThumb baseline=56)
    //   y=63:     max temps (TomThumb baseline=63)
    // ============================================================

    if (needsFullRedraw) {
        // Clear and redraw each section individually to avoid full-screen flicker

        // ---- Current weather (y=0-10) ----
        dma_display->fillRect(0, 0, DISPLAY_WIDTH, 11, black);
        int16_t weatherTextX = 2;
        const uint16_t* builtinCurrentIcon = getBuiltinWeatherIcon(weatherData.currentIcon);
        if (builtinCurrentIcon) {
            drawProgmemIcon(dma_display, builtinCurrentIcon, 1, 1, 1);
            weatherTextX = 11;
        } else {
            CachedIcon* currentIcon = getIcon(weatherData.currentIcon);
            if (currentIcon && currentIcon->valid) {
                drawIconAtScale(currentIcon, 1, 1, 1);
                weatherTextX = 11;
            }
        }

        // Temperature (NULL font, top at y=2 to align with icon)
        dma_display->setFont(NULL);
        dma_display->setTextSize(1);
        dma_display->setTextColor(white);

        char tempStr[8];
        snprintf(tempStr, sizeof(tempStr), "%d", weatherData.currentTemp);
        dma_display->setCursor(weatherTextX, 2);
        dma_display->print(tempStr);

        // Degree symbol (small circle, superscript position)
        int16_t degreeX = weatherTextX + strlen(tempStr) * 6;
        dma_display->drawPixel(degreeX + 1, 1, white);
        dma_display->drawPixel(degreeX,     2, white);
        dma_display->drawPixel(degreeX + 2, 2, white);
        dma_display->drawPixel(degreeX + 1, 3, white);

        // "C" after degree (NULL font, same top as temp)
        int16_t cX = degreeX + 4;
        dma_display->setCursor(cX, 2);
        dma_display->print("C");

        // Today's min/max on right side (NULL font, right-aligned)
        char todayMinStr[8], todayMaxStr[8];
        snprintf(todayMinStr, sizeof(todayMinStr), "%d", weatherData.currentTempMin);
        snprintf(todayMaxStr, sizeof(todayMaxStr), "%d", weatherData.currentTempMax);
        int16_t todayMinW = strlen(todayMinStr) * 4;
        int16_t todaySlashW = 4;
        int16_t todayMaxW = strlen(todayMaxStr) * 4;
        int16_t todayTotalW = todayMinW + todaySlashW + todayMaxW;
        int16_t todayX = DISPLAY_WIDTH - todayTotalW - 1;

        dma_display->setFont(&TomThumb);
        dma_display->setTextColor(coldBlue);
        dma_display->setCursor(todayX, 8);
        dma_display->print(todayMinStr);
        dma_display->setTextColor(gray);
        dma_display->setCursor(todayX + todayMinW, 8);
        dma_display->print("/");
        dma_display->setTextColor(warmRed);
        dma_display->setCursor(todayX + todayMinW + todaySlashW, 8);
        dma_display->print(todayMaxStr);

        // ---- Separator (y=10) ----
        dma_display->fillRect(0, 10, DISPLAY_WIDTH, 1, black);
        drawSeparatorLine(10, dimGray);

        // ---- Date (y=21-30) ----
        dma_display->fillRect(0, 21, DISPLAY_WIDTH, 10, black);
        unsigned long epochTime = timeClient.getEpochTime();
        struct tm* timeinfo = gmtime((time_t*)&epochTime);

        static const char* dayNamesFr[] = {"DIM", "LUN", "MAR", "MER", "JEU", "VEN", "SAM"};
        static const char* monthNamesFr[] = {"JAN", "FEV", "MAR", "AVR", "MAI", "JUN",
                                             "JUL", "AOU", "SEP", "OCT", "NOV", "DEC"};

        char dateStr[16];
        snprintf(dateStr, sizeof(dateStr), "%s %02d %s",
                 dayNamesFr[timeinfo->tm_wday],
                 timeinfo->tm_mday,
                 monthNamesFr[timeinfo->tm_mon]);

        dma_display->setFont(NULL);
        dma_display->setTextSize(1);
        dma_display->setTextColor(gray);

        int16_t dateWidth = strlen(dateStr) * 6;
        int16_t dateX = (DISPLAY_WIDTH - dateWidth) / 2;
        dma_display->setCursor(dateX, 22);
        dma_display->print(dateStr);

        // ---- Separator (y=31) ----
        drawSeparatorLine(31, dimGray);

        weatherLastDrawnMinute = minutes;
        weatherLastUpdateDrawn = weatherData.lastUpdate;
    }

    // ---- Forecast (y=33-63) - redrawn on full redraw or page change ----
    if (needsForecastRedraw) {
        dma_display->fillRect(0, 32, DISPLAY_WIDTH, 32, black);

        // Compute which forecast days to display on the current page
        uint8_t pageStart = forecastPage * FORECAST_COLUMNS;
        uint8_t displayCount = min((uint8_t)FORECAST_COLUMNS,
            (uint8_t)(weatherData.forecastCount - pageStart));

        for (int col = 0; col < displayCount; col++) {
            int forecastIndex = pageStart + col;

            // Dynamic centering based on number of columns on this page
            int16_t colCenter;
            if (displayCount == 1) {
                colCenter = 32;
            } else if (displayCount == 2) {
                colCenter = 16 + col * 32;
            } else {
                colCenter = 11 + col * 21;
            }

            // Day name (TomThumb baseline=39, glyphs y=34-38)
            dma_display->setFont(&TomThumb);
            dma_display->setTextColor(coral);
            int16_t dayNameWidth = strlen(weatherData.forecast[forecastIndex].dayName) * 4;
            dma_display->setCursor(colCenter - dayNameWidth / 2, 39);
            dma_display->print(weatherData.forecast[forecastIndex].dayName);

            // Forecast icon (8x8 native, y=41-48)
            const uint16_t* builtinForecastIcon = getBuiltinWeatherIcon(weatherData.forecast[forecastIndex].icon);
            if (builtinForecastIcon) {
                drawProgmemIcon(dma_display, builtinForecastIcon, colCenter - 4, 41, 1);
            } else {
                CachedIcon* forecastIcon = getIcon(weatherData.forecast[forecastIndex].icon);
                if (forecastIcon && forecastIcon->valid) {
                    drawIconAtScale(forecastIcon, colCenter - 4, 41, 1);
                }
            }

            // Min temp in blue (TomThumb baseline=56, glyphs y=51-55)
            char minStr[8];
            snprintf(minStr, sizeof(minStr), "%d", weatherData.forecast[forecastIndex].tempMin);
            dma_display->setFont(&TomThumb);
            dma_display->setTextColor(coldBlue);
            int16_t minWidth = strlen(minStr) * 4;
            dma_display->setCursor(colCenter - minWidth / 2, 56);
            dma_display->print(minStr);

            // Max temp in red (TomThumb baseline=63, glyphs y=58-62)
            char maxStr[8];
            snprintf(maxStr, sizeof(maxStr), "%d", weatherData.forecast[forecastIndex].tempMax);
            dma_display->setTextColor(warmRed);
            int16_t maxWidth = strlen(maxStr) * 4;
            dma_display->setCursor(colCenter - maxWidth / 2, 63);
            dma_display->print(maxStr);
        }

        // Page indicator squares (vertical, right edge, just below second separator)
        if (forecastPageCount > 1) {
            uint16_t activeDot = dma_display->color565(120, 60, 200);  // Dark violet
            int squareSize = 2;
            int gap = 1;
            int step = squareSize + gap;  // 3px per indicator
            int dotX = 61;               // 2px margin from right edge (x=63)
            int dotStartY = 33;          // Just below separator at y=31
            for (int d = 0; d < forecastPageCount; d++) {
                uint16_t dotColor = (d == forecastPage) ? activeDot : dimGray;
                dma_display->fillRect(dotX, dotStartY + d * step, squareSize, squareSize, dotColor);
            }
        }
    }

    // ---- Clock (y=13-20) - redrawn every second ----
    // Clear only the clock region (y=11 to y=20) to avoid full-screen flicker
    dma_display->fillRect(0, 11, DISPLAY_WIDTH, 10, black);

    dma_display->setTextColor(mintGreen);

    // HH:MM in NULL font (5 chars * 6px = 30px)
    char hmStr[6];
    snprintf(hmStr, sizeof(hmStr), "%02d:%02d", hours, minutes);
    dma_display->setFont(NULL);
    dma_display->setTextSize(1);

    int16_t hmX = (DISPLAY_WIDTH - 30) / 2 - 6;  // Shift left for seconds
    dma_display->setCursor(hmX, 13);
    dma_display->print(hmStr);

    // Seconds in TomThumb (baseline=20, bottom-aligned with NULL font y=13+6=19)
    dma_display->setFont(&TomThumb);
    char secStr[4];
    snprintf(secStr, sizeof(secStr), ":%02d", seconds);
    dma_display->setCursor(hmX + 31, 20);
    dma_display->print(secStr);

    // Reset font
    dma_display->setFont(NULL);

    drawIndicators();

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

void displayShowApp(AppItem* app) {
    if (!app) return;

    // Detect app switch and clear screen to prevent ghosting
    int8_t appIndex = appFind(app->id);
    if (appIndex != lastDisplayedAppIndex) {
        dma_display->clearScreen();
        #if DOUBLE_BUFFER
            dma_display->flipDMABuffer();
            dma_display->clearScreen();
        #endif
        lastDisplayedAppIndex = appIndex;
        // Reset weather display cache to force full redraw
        weatherLastDrawnMinute = -1;
        weatherLastUpdateDrawn = 0;
        // Reset forecast pagination to first page
        forecastPage = 0;
        lastForecastPageSwitch = millis();
    }

    // Handle system apps
    if (strcmp(app->id, "clock") == 0) {
        displayShowTime();
        return;
    }

    if (strcmp(app->id, "date") == 0) {
        displayShowDate();
        return;
    }

    if (strcmp(app->id, "weatherclock") == 0) {
        displayShowWeatherClock(app->duration);
        return;
    }

    // Tracker layout apps (ID starts with "tracker_")
    if (strncmp(app->id, TRACKER_ID_PREFIX, strlen(TRACKER_ID_PREFIX)) == 0) {
        const char* trackerName = app->id + strlen(TRACKER_ID_PREFIX);
        TrackerData* tracker = trackerFind(trackerName);
        if (tracker && tracker->valid) {
            displayShowTracker(tracker);
            return;
        }
        // Fallback to default custom app layout if no data
    }

    // Multi-zone layout apps
    if (app->zoneCount >= 2) {
        displayShowMultiZone(app);
        return;
    }

    // Custom apps (single-zone)
    dma_display->clearScreen();

    // Layout calculation - VERTICAL layout for 64x64 panel
    // +----------64px-----------+
    // |      Icon (8-16px)      |  <- centered, top
    // |                         |
    // |         Text            |  <- centered, below icon
    // |      (scrollable)       |
    // +-------------------------+

    int16_t textAreaX = 2;
    int16_t textAreaWidth = DISPLAY_WIDTH - 4;  // 2px margin each side
    int16_t textYPos = 28;  // Default Y position for text

    // Try to load icon if specified
    CachedIcon* icon = nullptr;
    if (strlen(app->icon) > 0) {
        icon = getIcon(app->icon);
    }

    // Adjust layout if icon is present - VERTICAL layout
    if (icon && icon->valid) {
        // Calculate displayed size (upscale x2 for 8x8 icons)
        uint8_t scale = (icon->width <= 8 && icon->height <= 8) ? 2 : 1;
        uint8_t displayWidth = icon->width * scale;
        uint8_t displayHeight = icon->height * scale;

        // Draw icon centered horizontally at top
        int16_t iconX = (DISPLAY_WIDTH - displayWidth) / 2;
        int16_t iconY = 2;  // 2px from top
        drawIcon(icon, iconX, iconY);

        // Text starts below icon with gap
        textYPos = iconY + displayHeight + 6;  // 6px gap below icon
    }

    dma_display->setTextSize(1);

    // Calculate text width and check if scrolling needed
    int16_t textWidth = calculateTextWidth(app->text);
    bool needsScroll = textWidth > textAreaWidth;

    // Update scroll state if this is new text or scroll requirements changed
    if (appScrollState.textWidth != textWidth || appScrollState.availableWidth != textAreaWidth) {
        appScrollState.textWidth = textWidth;
        appScrollState.availableWidth = textAreaWidth;
        appScrollState.needsScroll = needsScroll;
        if (!needsScroll) {
            appScrollState.scrollOffset = 0;
            appScrollState.scrollPhase = 0;
        }
    }

    // Calculate x position with scroll offset
    int16_t xPos = textAreaX;
    if (needsScroll) {
        xPos = textAreaX - appScrollState.scrollOffset;
    }

    // Draw text with segment-aware coloring
    printTextWithSegments(app->text, xPos, textYPos, app->textColor,
                          app->textSegments, app->textSegmentCount);

    // Draw label below text if present (TomThumb font, dimmed color)
    if (app->label[0] != '\0') {
        int16_t labelWidth = strlen(app->label) * 4;
        int16_t labelX = (DISPLAY_WIDTH - labelWidth) / 2;
        if (labelX < 2) labelX = 2;
        int16_t labelY = textYPos + 12;
        printLabelWithSegments(app->label, labelX, labelY, app->textColor,
                               app->labelSegments, app->labelSegmentCount, true);
    }

    drawIndicators();

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

// ============================================================================
// Multi-Zone Display Rendering
// ============================================================================

// Render a single zone within its bounding box
void displayShowZone(AppZone* zone, int16_t x, int16_t y, int16_t w, int16_t h) {
    if (!zone) return;

    dma_display->setTextSize(1);

    // Try to load icon
    CachedIcon* icon = nullptr;
    if (strlen(zone->icon) > 0) {
        icon = getIcon(zone->icon);
    }

    bool isFullWidth = (w >= 48);

    bool hasLabel = (zone->label[0] != '\0');

    // Layout constants for label positioning
    // NULL font: setCursor = top of glyph, 7px tall -> occupies textY to textY+6
    // TomThumb: setCursor = baseline, ~5px above -> occupies labelY-4 to labelY
    // Gap of 2px between text bottom and label top: labelY - 4 = textY + 6 + 2 -> labelY = textY + 12

    if (isFullWidth) {
        // Full-width zone (64x31): icon left, text+label right
        int16_t textX = x + 2;

        // With label: spread text (upper) and label (lower) across zone height
        // Without label: center text vertically
        // h=31 -> text at y+4 (top=y+4..y+10), label baseline at y+23 (top=y+19..y+23)
        int16_t textY = hasLabel ? y + 4 : y + (h / 2) - 3;

        if (icon && icon->valid) {
            // Icon at left, vertically centered in zone
            uint8_t scale = (icon->width <= 8 && icon->height <= 8) ? 2 : 1;
            uint8_t displayWidth = icon->width * scale;
            uint8_t displayHeight = icon->height * scale;
            int16_t iconX = x + 2;
            int16_t iconY = y + (h - displayHeight) / 2;
            drawIconAtScale(icon, iconX, iconY, scale);

            // Text starts after icon
            textX = iconX + displayWidth + 3;
        }

        // Truncate text to fit available width
        int16_t availableWidth = (x + w) - textX;
        int16_t maxChars = availableWidth / 6;  // 6px per char (5x7 font + 1px spacing)
        char truncatedText[32];
        strlcpy(truncatedText, zone->text, sizeof(truncatedText));
        if ((int16_t)strlen(truncatedText) > maxChars && maxChars > 0) {
            truncatedText[maxChars] = '\0';
        }

        printTextWithSegments(truncatedText, textX, textY, zone->textColor,
                              zone->textSegments, zone->textSegmentCount);

        // Draw label in lower portion of zone (TomThumb, dimmed)
        if (hasLabel) {
            int16_t labelY = y + h - 6;  // Near bottom of zone
            printLabelWithSegments(zone->label, textX, labelY, zone->textColor,
                                   zone->labelSegments, zone->labelSegmentCount, true);
        }
    } else {
        // Half-width zone (31x31): icon top-left, text beside icon, label at bottom
        bool hasIcon = (icon && icon->valid);

        int16_t textX = x;
        int16_t textY = hasLabel ? y + 3 : y + (h / 2) - 3;

        if (hasIcon) {
            // Icon at top-left, tight margins, native size (no upscale)
            int16_t iconX = x;
            int16_t iconY = y + 2;
            drawIconAtScale(icon, iconX, iconY, 1);

            // Text starts after icon with 1px gap
            textX = iconX + icon->width + 1;
        }

        // Check if text fits in default font (6px/char), fallback to TomThumb (4px/char)
        int16_t availableWidth = (x + w) - textX;
        int16_t textLen = (int16_t)strlen(zone->text);
        bool useCompactText = (textLen * 6 > availableWidth);
        int16_t charWidth = useCompactText ? 4 : 6;

        int16_t maxChars = availableWidth / charWidth;
        char truncatedText[32];
        strlcpy(truncatedText, zone->text, sizeof(truncatedText));
        if (textLen > maxChars && maxChars > 0) {
            truncatedText[maxChars] = '\0';
        }

        if (useCompactText) {
            // TomThumb: baseline positioning, adjust Y (+5px from top for baseline)
            int16_t compactY = hasLabel ? y + 8 : y + (h / 2) + 2;
            printLabelWithSegments(truncatedText, textX, compactY, zone->textColor,
                                   zone->textSegments, zone->textSegmentCount, false);
        } else {
            // Default font
            printTextWithSegments(truncatedText, textX, textY, zone->textColor,
                                  zone->textSegments, zone->textSegmentCount);
        }

        // Draw label at bottom of zone with good margin
        if (hasLabel) {
            int16_t labelWidth = strlen(zone->label) * 4;
            int16_t labelX = x + (w - labelWidth) / 2;
            if (labelX < x) labelX = x;
            int16_t labelY = y + h - 6;
            printLabelWithSegments(zone->label, labelX, labelY, zone->textColor,
                                   zone->labelSegments, zone->labelSegmentCount, true);
        }
    }
}

// Render multi-zone layout for an app
void displayShowMultiZone(AppItem* app) {
    if (!app || app->zoneCount < 2) return;

    dma_display->clearScreen();

    // Build array of all zones (zone 0 from main app fields, zones 1-3 from zones[])
    AppZone zone0;
    strlcpy(zone0.text, app->text, sizeof(zone0.text));
    strlcpy(zone0.icon, app->icon, sizeof(zone0.icon));
    strlcpy(zone0.label, app->label, sizeof(zone0.label));
    zone0.textColor = app->textColor;
    memcpy(zone0.textSegments, app->textSegments, sizeof(app->textSegments));
    zone0.textSegmentCount = app->textSegmentCount;
    memcpy(zone0.labelSegments, app->labelSegments, sizeof(app->labelSegments));
    zone0.labelSegmentCount = app->labelSegmentCount;

    AppZone* allZones[MAX_ZONES] = { &zone0, nullptr, nullptr, nullptr };
    for (uint8_t i = 1; i < app->zoneCount && i < MAX_ZONES; i++) {
        allZones[i] = &app->zones[i - 1];
    }

    // Separator line color (dark gray)
    uint16_t separatorColor = dma_display->color565(40, 40, 40);

    switch (app->zoneCount) {
        case 2: {
            // Two horizontal rows: zone0 top (64x31), zone1 bottom (64x31)
            // Separator at y=31
            dma_display->drawFastHLine(0, 31, 64, separatorColor);

            displayShowZone(allZones[0], 0, 0, 64, 31);
            displayShowZone(allZones[1], 0, 33, 64, 31);
            break;
        }
        case 3: {
            // Top row full-width (zone0, 64x31), bottom row split (zone1 + zone2, 31x31 each)
            // Horizontal separator at y=31
            dma_display->drawFastHLine(0, 31, 64, separatorColor);
            // Vertical separator in bottom half at x=31
            dma_display->drawFastVLine(31, 33, 31, separatorColor);

            displayShowZone(allZones[0], 0, 0, 64, 31);
            displayShowZone(allZones[1], 0, 33, 31, 31);
            displayShowZone(allZones[2], 33, 33, 31, 31);
            break;
        }
        case 4: {
            // Four quadrants (31x31 each)
            // Horizontal separator at y=31
            dma_display->drawFastHLine(0, 31, 64, separatorColor);
            // Vertical separator at x=31
            dma_display->drawFastVLine(31, 0, 64, separatorColor);

            displayShowZone(allZones[0], 0, 0, 31, 31);
            displayShowZone(allZones[1], 33, 0, 31, 31);
            displayShowZone(allZones[2], 0, 33, 31, 31);
            displayShowZone(allZones[3], 33, 33, 31, 31);
            break;
        }
    }

    drawIndicators();

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

void displayClear() {
    dma_display->clearScreen();
    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

void displaySetBrightness(uint8_t brightness) {
    currentBrightness = constrain(brightness, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
    dma_display->setBrightness8(currentBrightness);
    Serial.printf("[DISPLAY] Brightness set to %d\n", currentBrightness);
}

int16_t calculateTextWidth(const char* text) {
    // Default 5x7 font with 1px spacing = 6 pixels per character
    return strlen(text) * 6;
}

bool textNeedsScroll(const char* text, int16_t availableWidth) {
    return calculateTextWidth(text) > availableWidth;
}

void resetScrollState() {
    appScrollState.scrollOffset = 0;
    appScrollState.lastScrollTime = millis();
    appScrollState.scrollPhase = 0;  // Start with pause
    appScrollState.needsScroll = false;
    appScrollState.textWidth = 0;
    appScrollState.availableWidth = DISPLAY_WIDTH - 4;  // 2px margin each side
}

void resetNotifScrollState() {
    notifScrollState.scrollOffset = 0;
    notifScrollState.lastScrollTime = millis();
    notifScrollState.scrollPhase = 0;
    notifScrollState.needsScroll = false;
    notifScrollState.textWidth = 0;
    notifScrollState.availableWidth = DISPLAY_WIDTH - 4;  // Full width minus 2px padding each side
}

// ============================================================================
// Notification Display
// ============================================================================

void displayShowNotification(NotificationItem* notif) {
    if (!notif || !notif->active) return;

    // Mark display timestamp on first render
    if (notif->displayedAt == 0) {
        notif->displayedAt = millis();
    }

    // Layout: horizontal separators with background color margins
    // [bg margin 4px] [separator line] [content: icon + text] [separator line] [bg margin 4px]
    const int16_t marginHeight = 6;
    const int16_t separatorTopY = marginHeight;                            // y=4
    const int16_t separatorBottomY = DISPLAY_HEIGHT - marginHeight - 1;    // y=59
    const int16_t contentY = separatorTopY + 2;                            // y=6
    const int16_t contentH = separatorBottomY - contentY - 1;             // 52
    const int16_t textPadding = 2;                                         // Horizontal text padding
    const int16_t textAreaWidth = DISPLAY_WIDTH - textPadding * 2;         // 60

    // Colors
    uint8_t tr = (notif->textColor >> 16) & 0xFF;
    uint8_t tg = (notif->textColor >> 8) & 0xFF;
    uint8_t tb = notif->textColor & 0xFF;
    uint16_t lineColor = dma_display->color565(tr, tg, tb);
    uint16_t black = dma_display->color565(0, 0, 0);

    uint16_t bgFill = black;
    if (notif->backgroundColor != 0) {
        uint8_t br = (notif->backgroundColor >> 16) & 0xFF;
        uint8_t bg = (notif->backgroundColor >> 8) & 0xFF;
        uint8_t bb = notif->backgroundColor & 0xFF;
        bgFill = dma_display->color565(br, bg, bb);
    }

    // === Build frame (no clearScreen to avoid DMA flicker) ===

    // 1. Background color margins (top and bottom strips)
    dma_display->fillRect(0, 0, DISPLAY_WIDTH, marginHeight, bgFill);
    dma_display->fillRect(0, DISPLAY_HEIGHT - marginHeight, DISPLAY_WIDTH, marginHeight, bgFill);

    // 2. Content area (black)
    dma_display->fillRect(0, marginHeight, DISPLAY_WIDTH, DISPLAY_HEIGHT - marginHeight * 2, black);

    // 3. Separator lines
    uint16_t separatorColor = (bgFill != black) ? bgFill : lineColor;
    dma_display->drawFastHLine(0, separatorTopY, DISPLAY_WIDTH, separatorColor);
    dma_display->drawFastHLine(0, separatorBottomY, DISPLAY_WIDTH, separatorColor);

    // 4. Load icon
    CachedIcon* icon = nullptr;
    uint8_t iconDisplayW = 0;
    uint8_t iconDisplayH = 0;
    if (strlen(notif->icon) > 0) {
        icon = getIcon(notif->icon);
        if (icon && icon->valid) {
            uint8_t scale = (icon->width <= 8 && icon->height <= 8) ? 2 : 1;
            iconDisplayW = icon->width * scale;
            iconDisplayH = icon->height * scale;
        } else {
            icon = nullptr;
        }
    }

    // 5. Vertical centering of content (icon + text)
    const int16_t textHeight = 7;
    const int16_t iconTextGap = 4;
    int16_t totalContentH = textHeight;
    if (icon) {
        totalContentH = iconDisplayH + iconTextGap + textHeight;
    }
    int16_t contentStartY = contentY + (contentH - totalContentH) / 2;

    // 6. Draw icon centered horizontally
    int16_t textYPos;
    if (icon) {
        int16_t iconX = (DISPLAY_WIDTH - iconDisplayW) / 2;
        drawIcon(icon, iconX, contentStartY);
        textYPos = contentStartY + iconDisplayH + iconTextGap;
    } else {
        textYPos = contentStartY;
    }

    // 7. Draw text (full width, scrolls off-screen naturally - no clipping needed)
    dma_display->setTextColor(lineColor);
    dma_display->setTextSize(1);

    int16_t textWidth = calculateTextWidth(notif->text);
    bool needsScroll = textWidth > textAreaWidth;

    if (notifScrollState.textWidth != textWidth || notifScrollState.availableWidth != textAreaWidth) {
        notifScrollState.textWidth = textWidth;
        notifScrollState.availableWidth = textAreaWidth;
        notifScrollState.needsScroll = needsScroll;
        if (!needsScroll) {
            notifScrollState.scrollOffset = 0;
            notifScrollState.scrollPhase = 0;
        }
    }

    int16_t xPos;
    if (!needsScroll) {
        xPos = textPadding + (textAreaWidth - textWidth) / 2;
    } else {
        xPos = textPadding - notifScrollState.scrollOffset;
    }

    printTextWithSpecialChars(notif->text, xPos, textYPos);

    drawIndicators();

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

// ============================================================================
// Indicator Functions
// ============================================================================

void indicatorInit() {
    memset(indicators, 0, sizeof(indicators));
    memset(indicatorAnimState, 0, sizeof(indicatorAnimState));

    // Default colors: red, green, blue
    indicators[0].color = 0xFF0000;
    indicators[1].color = 0x00FF00;
    indicators[2].color = 0x0000FF;

    for (uint8_t i = 0; i < NUM_INDICATORS; i++) {
        indicators[i].blinkInterval = INDICATOR_BLINK_INTERVAL;
        indicators[i].fadePeriod = INDICATOR_FADE_PERIOD;
    }
}

void indicatorSet(uint8_t index, IndicatorMode mode, uint32_t color,
                  uint16_t blinkInterval, uint16_t fadePeriod) {
    if (index >= NUM_INDICATORS) return;

    indicators[index].mode = mode;
    indicators[index].color = color;
    indicators[index].blinkInterval = blinkInterval > 0 ? blinkInterval : INDICATOR_BLINK_INTERVAL;
    indicators[index].fadePeriod = fadePeriod > 0 ? fadePeriod : INDICATOR_FADE_PERIOD;

    // Reset animation state
    indicatorAnimState[index].lastToggle = millis();
    indicatorAnimState[index].blinkOn = true;
    indicatorAnimState[index].cycleStart = millis();
}

void indicatorOff(uint8_t index) {
    if (index >= NUM_INDICATORS) return;
    indicators[index].mode = INDICATOR_OFF;
}

bool indicatorNeedsRedraw() {
    for (uint8_t i = 0; i < NUM_INDICATORS; i++) {
        if (indicators[i].mode == INDICATOR_BLINK || indicators[i].mode == INDICATOR_FADE) {
            return true;
        }
    }
    return false;
}

void drawIndicators() {
    unsigned long now = millis();

    for (uint8_t i = 0; i < NUM_INDICATORS; i++) {
        if (indicators[i].mode == INDICATOR_OFF) continue;

        // Compute corner position
        int16_t x, y;
        switch (i) {
            case 0: x = 0; y = 0; break;                                             // Top-left
            case 1: x = DISPLAY_WIDTH - INDICATOR_FOOTPRINT; y = 0; break;           // Top-right
            case 2: x = DISPLAY_WIDTH - INDICATOR_FOOTPRINT;                          // Bottom-right
                    y = DISPLAY_HEIGHT - INDICATOR_FOOTPRINT; break;
            default: continue;
        }

        // Extract base color
        uint8_t r = (indicators[i].color >> 16) & 0xFF;
        uint8_t g = (indicators[i].color >> 8) & 0xFF;
        uint8_t b = indicators[i].color & 0xFF;

        // Apply mode effect
        switch (indicators[i].mode) {
            case INDICATOR_SOLID:
                // Full brightness, no change
                break;

            case INDICATOR_BLINK: {
                if (now - indicatorAnimState[i].lastToggle >= indicators[i].blinkInterval) {
                    indicatorAnimState[i].blinkOn = !indicatorAnimState[i].blinkOn;
                    indicatorAnimState[i].lastToggle = now;
                }
                if (!indicatorAnimState[i].blinkOn) continue;  // Skip drawing when off
                break;
            }

            case INDICATOR_FADE: {
                // Triangle wave: ramp up then ramp down, min brightness 10/255
                unsigned long elapsed = (now - indicatorAnimState[i].cycleStart) % indicators[i].fadePeriod;
                uint16_t halfPeriod = indicators[i].fadePeriod / 2;
                uint8_t brightness;
                if (elapsed < halfPeriod) {
                    brightness = 10 + (uint16_t)(245 * elapsed) / halfPeriod;
                } else {
                    brightness = 10 + (uint16_t)(245 * (indicators[i].fadePeriod - elapsed)) / halfPeriod;
                }
                r = (uint16_t)r * brightness / 255;
                g = (uint16_t)g * brightness / 255;
                b = (uint16_t)b * brightness / 255;
                break;
            }

            default:
                continue;
        }

        // Draw black border (full footprint)
        dma_display->fillRect(x, y, INDICATOR_FOOTPRINT, INDICATOR_FOOTPRINT,
                              dma_display->color565(0, 0, 0));

        // Draw colored core (inset by border size)
        dma_display->fillRect(x + INDICATOR_BORDER_SIZE, y + INDICATOR_BORDER_SIZE,
                              INDICATOR_CORE_SIZE, INDICATOR_CORE_SIZE,
                              dma_display->color565(r, g, b));
    }
}

void handleIndicatorApi(AsyncWebServerRequest *request, JsonVariant &json, uint8_t index) {
    if (index >= NUM_INDICATORS) {
        request->send(400, "application/json", "{\"error\":\"Invalid indicator index\"}");
        return;
    }

    JsonObject body = json.as<JsonObject>();

    // Parse mode string
    const char* modeStr = body["mode"] | "";
    IndicatorMode mode = INDICATOR_OFF;

    if (strlen(modeStr) > 0) {
        if (strcmp(modeStr, "solid") == 0) mode = INDICATOR_SOLID;
        else if (strcmp(modeStr, "blink") == 0) mode = INDICATOR_BLINK;
        else if (strcmp(modeStr, "fade") == 0) mode = INDICATOR_FADE;
        else if (strcmp(modeStr, "off") == 0) mode = INDICATOR_OFF;
        else {
            request->send(400, "application/json", "{\"error\":\"Invalid mode. Use: off, solid, blink, fade\"}");
            return;
        }
    } else if (!body["color"].isNull()) {
        // Default to solid if color provided but no mode
        mode = INDICATOR_SOLID;
    }

    if (mode == INDICATOR_OFF) {
        indicatorOff(index);
        saveSettings();
        Serial.printf("[API] Indicator %d turned off\n", index + 1);
        request->send(200, "application/json", "{\"success\":true,\"mode\":\"off\"}");
        return;
    }

    // Parse color (reuse parseColorValue helper)
    uint32_t color = parseColorValue(body["color"], indicators[index].color);

    // Parse optional timing parameters
    uint16_t blinkInterval = body["blinkInterval"] | (uint16_t)INDICATOR_BLINK_INTERVAL;
    uint16_t fadePeriod = body["fadePeriod"] | (uint16_t)INDICATOR_FADE_PERIOD;

    indicatorSet(index, mode, color, blinkInterval, fadePeriod);
    saveSettings();

    Serial.printf("[API] Indicator %d set: mode=%s color=0x%06X\n",
                  index + 1, modeStr[0] ? modeStr : "solid", color);

    char response[128];
    snprintf(response, sizeof(response),
             "{\"success\":true,\"indicator\":%d,\"mode\":\"%s\",\"color\":[%d,%d,%d]}",
             index + 1,
             mode == INDICATOR_SOLID ? "solid" : (mode == INDICATOR_BLINK ? "blink" : "fade"),
             (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    request->send(200, "application/json", response);
}

// Print text with special character handling
// Replaces non-ASCII characters with ASCII equivalents or draws them manually
void printTextWithSpecialChars(const char* text, int16_t x, int16_t y) {
    int16_t cursorX = x;
    const uint8_t charWidth = 6;  // 5x7 font + 1px spacing

    dma_display->setCursor(cursorX, y);

    const uint8_t* ptr = (const uint8_t*)text;
    while (*ptr) {
        uint8_t c = *ptr;

        // Handle UTF-8 degree symbol (C2 B0)
        if (c == 0xC2 && *(ptr + 1) == 0xB0) {
            // Draw degree symbol as small circle (3x3 at top)
            // Use white color - will inherit from setTextColor context
            int16_t dx = cursorX;
            int16_t dy = y - 6;  // Position at top of character
            dma_display->drawPixel(dx + 1, dy, 0xFFFF);
            dma_display->drawPixel(dx, dy + 1, 0xFFFF);
            dma_display->drawPixel(dx + 2, dy + 1, 0xFFFF);
            dma_display->drawPixel(dx + 1, dy + 2, 0xFFFF);
            cursorX += 4;  // Smaller width for degree
            ptr += 2;  // Skip both UTF-8 bytes
            dma_display->setCursor(cursorX, y);
            continue;
        }

        // Handle Latin-1 degree symbol (direct byte 0xB0)
        if (c == 0xB0) {
            int16_t dx = cursorX;
            int16_t dy = y - 6;
            dma_display->drawPixel(dx + 1, dy, 0xFFFF);
            dma_display->drawPixel(dx, dy + 1, 0xFFFF);
            dma_display->drawPixel(dx + 2, dy + 1, 0xFFFF);
            dma_display->drawPixel(dx + 1, dy + 2, 0xFFFF);
            cursorX += 4;
            ptr++;
            dma_display->setCursor(cursorX, y);
            continue;
        }

        // Handle UTF-8 accented characters (common French)
        if (c == 0xC3) {
            uint8_t next = *(ptr + 1);
            char replacement = '?';
            switch (next) {
                case 0xA0: case 0xA2: case 0xA4: replacement = 'a'; break;  // a grave, circumflex, umlaut
                case 0xA8: case 0xA9: case 0xAA: case 0xAB: replacement = 'e'; break;  // e variants
                case 0xAC: case 0xAE: case 0xAF: replacement = 'i'; break;  // i variants
                case 0xB2: case 0xB4: case 0xB6: replacement = 'o'; break;  // o variants
                case 0xB9: case 0xBB: case 0xBC: replacement = 'u'; break;  // u variants
                case 0xA7: replacement = 'c'; break;  // c cedilla
                case 0xB1: replacement = 'n'; break;  // n tilde
                case 0x80: case 0x89: replacement = 'E'; break;  // E accent
                case 0x87: replacement = 'C'; break;  // C cedilla
            }
            dma_display->print(replacement);
            cursorX += charWidth;
            ptr += 2;
            dma_display->setCursor(cursorX, y);
            continue;
        }

        // Standard ASCII character
        if (c >= 32 && c <= 126) {
            dma_display->print((char)c);
            cursorX += charWidth;
        }
        // Skip other non-printable characters

        ptr++;
        dma_display->setCursor(cursorX, y);
    }
}

// Draw text with per-segment coloring (NULL font, 6px/char)
// segmentCount==0: uses defaultColor and delegates to printTextWithSpecialChars
// segmentCount>0: switches color at segment boundaries
void printTextWithSegments(const char* text, int16_t x, int16_t y,
                           uint32_t defaultColor, const TextSegment* segments, uint8_t segmentCount) {
    if (segmentCount == 0) {
        uint8_t r = (defaultColor >> 16) & 0xFF;
        uint8_t g = (defaultColor >> 8) & 0xFF;
        uint8_t b = defaultColor & 0xFF;
        dma_display->setTextColor(dma_display->color565(r, g, b));
        printTextWithSpecialChars(text, x, y);
        return;
    }

    const uint8_t charWidth = 6;
    int16_t cursorX = x;
    dma_display->setCursor(cursorX, y);

    // Start with first segment color or default
    uint8_t currentSegment = 0;
    uint32_t currentColor = (segmentCount > 0) ? segments[0].color : defaultColor;
    uint8_t r = (currentColor >> 16) & 0xFF;
    uint8_t g = (currentColor >> 8) & 0xFF;
    uint8_t b = currentColor & 0xFF;
    uint16_t color565 = dma_display->color565(r, g, b);
    dma_display->setTextColor(color565);

    uint8_t charIndex = 0;  // Visual char index (UTF-8 multi-byte = 1 visual char)
    const uint8_t* ptr = (const uint8_t*)text;

    while (*ptr) {
        // Check if we need to switch to next segment color
        if (currentSegment + 1 < segmentCount && charIndex >= segments[currentSegment + 1].offset) {
            currentSegment++;
            currentColor = segments[currentSegment].color;
            r = (currentColor >> 16) & 0xFF;
            g = (currentColor >> 8) & 0xFF;
            b = currentColor & 0xFF;
            color565 = dma_display->color565(r, g, b);
            dma_display->setTextColor(color565);
        }

        uint8_t c = *ptr;

        // Handle UTF-8 degree symbol (C2 B0)
        if (c == 0xC2 && *(ptr + 1) == 0xB0) {
            int16_t dx = cursorX;
            int16_t dy = y - 6;
            dma_display->drawPixel(dx + 1, dy, color565);
            dma_display->drawPixel(dx, dy + 1, color565);
            dma_display->drawPixel(dx + 2, dy + 1, color565);
            dma_display->drawPixel(dx + 1, dy + 2, color565);
            cursorX += 4;
            ptr += 2;
            charIndex++;
            dma_display->setCursor(cursorX, y);
            continue;
        }

        // Handle Latin-1 degree symbol (direct byte 0xB0)
        if (c == 0xB0) {
            int16_t dx = cursorX;
            int16_t dy = y - 6;
            dma_display->drawPixel(dx + 1, dy, color565);
            dma_display->drawPixel(dx, dy + 1, color565);
            dma_display->drawPixel(dx + 2, dy + 1, color565);
            dma_display->drawPixel(dx + 1, dy + 2, color565);
            cursorX += 4;
            ptr++;
            charIndex++;
            dma_display->setCursor(cursorX, y);
            continue;
        }

        // Handle UTF-8 accented characters (common French)
        if (c == 0xC3) {
            uint8_t next = *(ptr + 1);
            char replacement = '?';
            switch (next) {
                case 0xA0: case 0xA2: case 0xA4: replacement = 'a'; break;
                case 0xA8: case 0xA9: case 0xAA: case 0xAB: replacement = 'e'; break;
                case 0xAC: case 0xAE: case 0xAF: replacement = 'i'; break;
                case 0xB2: case 0xB4: case 0xB6: replacement = 'o'; break;
                case 0xB9: case 0xBB: case 0xBC: replacement = 'u'; break;
                case 0xA7: replacement = 'c'; break;
                case 0xB1: replacement = 'n'; break;
                case 0x80: case 0x89: replacement = 'E'; break;
                case 0x87: replacement = 'C'; break;
            }
            dma_display->print(replacement);
            cursorX += charWidth;
            ptr += 2;
            charIndex++;
            dma_display->setCursor(cursorX, y);
            continue;
        }

        // Standard ASCII character
        if (c >= 32 && c <= 126) {
            dma_display->print((char)c);
            cursorX += charWidth;
            charIndex++;
        }

        ptr++;
        dma_display->setCursor(cursorX, y);
    }
}

// Draw label text with per-segment coloring (TomThumb font, baseline positioning)
// dimDefault=true + segmentCount==0: dims defaultColor 50% (standard label behavior)
// dimDefault=false: uses defaultColor as-is (compact text in half-width zones)
// segmentCount>0: uses segment colors at full brightness
void printLabelWithSegments(const char* text, int16_t x, int16_t y,
                            uint32_t defaultColor, const TextSegment* segments, uint8_t segmentCount,
                            bool dimDefault) {
    dma_display->setFont(&TomThumb);

    if (segmentCount == 0) {
        uint8_t r = (defaultColor >> 16) & 0xFF;
        uint8_t g = (defaultColor >> 8) & 0xFF;
        uint8_t b = defaultColor & 0xFF;
        if (dimDefault) {
            r = r * 3 / 4;
            g = g * 3 / 4;
            b = b * 3 / 4;
        }
        dma_display->setTextColor(dma_display->color565(r, g, b));
        dma_display->setCursor(x, y);
        dma_display->print(text);
        dma_display->setFont(NULL);
        return;
    }

    // Per-segment coloring - let GFX library handle cursor advancement
    dma_display->setCursor(x, y);

    uint8_t currentSegment = 0;
    uint32_t currentColor = segments[0].color;
    uint8_t r = (currentColor >> 16) & 0xFF;
    uint8_t g = (currentColor >> 8) & 0xFF;
    uint8_t b = currentColor & 0xFF;
    dma_display->setTextColor(dma_display->color565(r, g, b));

    uint8_t charIndex = 0;
    const char* ptr = text;

    while (*ptr) {
        // Check if we need to switch to next segment color
        if (currentSegment + 1 < segmentCount && charIndex >= segments[currentSegment + 1].offset) {
            currentSegment++;
            currentColor = segments[currentSegment].color;
            r = (currentColor >> 16) & 0xFF;
            g = (currentColor >> 8) & 0xFF;
            b = currentColor & 0xFF;
            dma_display->setTextColor(dma_display->color565(r, g, b));
        }

        dma_display->print(*ptr);
        charIndex++;
        ptr++;
    }

    dma_display->setFont(NULL);
}

// ============================================================================
// Icon Functions
// ============================================================================

void initIconCache() {
    for (uint8_t i = 0; i < MAX_ICON_CACHE; i++) {
        iconCache[i].name[0] = '\0';
        iconCache[i].pixels = nullptr;
        iconCache[i].width = 0;
        iconCache[i].height = 0;
        iconCache[i].valid = false;
        iconCache[i].lastUsed = 0;
    }
    Serial.println("[ICON] Cache initialized");
}

int pngDrawCallback(PNGDRAW *pDraw) {
    if (!pngDecodeTarget || pDraw->y >= 16) return 1;

    // Debug: log pixel type on first line
    if (pDraw->y == 0) {
        Serial.printf("[PNG] PixelType=%d, Width=%d, BPP=%d, HasAlpha=%d\n",
            pDraw->iPixelType, pDraw->iWidth, pDraw->iBpp, pDraw->iHasAlpha);
    }

    uint16_t* dest = pngDecodeTarget + (pDraw->y * pngDecodeWidth);
    uint16_t pixel;

    for (int x = 0; x < pDraw->iWidth && x < pngDecodeWidth; x++) {
        // Get RGBA values from source
        uint8_t r, g, b, a;
        if (pDraw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA) {
            // RGBA: 4 bytes per pixel
            uint8_t* src = pDraw->pPixels + (x * 4);
            r = src[0];
            g = src[1];
            b = src[2];
            a = src[3];
        } else if (pDraw->iPixelType == PNG_PIXEL_INDEXED) {
            // Indexed: use palette lookup
            uint8_t idx = pDraw->pPixels[x];
            if (pDraw->pPalette) {
                r = pDraw->pPalette[idx * 3];
                g = pDraw->pPalette[idx * 3 + 1];
                b = pDraw->pPalette[idx * 3 + 2];
                a = pDraw->iHasAlpha ? 255 : 255;  // TODO: handle alpha palette
            } else {
                r = g = b = idx;
                a = 255;
            }
        } else if (pDraw->iPixelType == PNG_PIXEL_TRUECOLOR) {
            // RGB: 3 bytes per pixel
            uint8_t* src = pDraw->pPixels + (x * 3);
            r = src[0];
            g = src[1];
            b = src[2];
            a = 255;
        } else {
            // Grayscale or other
            r = g = b = pDraw->pPixels[x];
            a = 255;
        }

        // Debug: log first non-black pixel on line 10
        if (pDraw->y == 10 && x < 20 && (r > 50 || g > 50 || b > 50)) {
            Serial.printf("[PNG] y=10 x=%d: R=%d G=%d B=%d A=%d\n", x, r, g, b, a);
        }

        // Convert to RGB565
        if (a < 128) {
            pixel = 0;  // Transparent = black
        } else {
            pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
        dest[x] = pixel;
    }
    return 1;
}

int8_t findLRUSlot() {
    int8_t lruIndex = -1;
    unsigned long oldestTime = UINT32_MAX;

    for (uint8_t i = 0; i < MAX_ICON_CACHE; i++) {
        // First check for empty slots
        if (!iconCache[i].valid) {
            return i;
        }
        // Then find least recently used
        if (iconCache[i].lastUsed < oldestTime) {
            oldestTime = iconCache[i].lastUsed;
            lruIndex = i;
        }
    }

    // Free the LRU slot
    if (lruIndex >= 0 && iconCache[lruIndex].pixels) {
        free(iconCache[lruIndex].pixels);
        iconCache[lruIndex].pixels = nullptr;
        iconCache[lruIndex].valid = false;
        Serial.printf("[ICON] Evicted icon: %s\n", iconCache[lruIndex].name);
    }

    return lruIndex;
}

CachedIcon* loadIcon(const char* name) {
    if (!name || strlen(name) == 0) return nullptr;
    if (!filesystemReady) return nullptr;

    // Build file path
    char filePath[64];
    snprintf(filePath, sizeof(filePath), "%s/%s.png", FS_ICONS_PATH, name);

    // Check if file exists
    if (!LittleFS.exists(filePath)) {
        Serial.printf("[ICON] File not found: %s\n", filePath);
        return nullptr;
    }

    // Find a cache slot
    int8_t slot = findLRUSlot();
    if (slot < 0) {
        Serial.println("[ICON] No cache slots available");
        return nullptr;
    }

    CachedIcon* cached = &iconCache[slot];

    // Open file
    File file = LittleFS.open(filePath, "r");
    if (!file) {
        Serial.printf("[ICON] Failed to open: %s\n", filePath);
        return nullptr;
    }

    // Read file into buffer
    size_t fileSize = file.size();
    uint8_t* fileBuffer = (uint8_t*)malloc(fileSize);
    if (!fileBuffer) {
        file.close();
        Serial.println("[ICON] Failed to allocate file buffer");
        return nullptr;
    }
    file.read(fileBuffer, fileSize);
    file.close();

    // Initialize PNG decoder
    int rc = png.openRAM(fileBuffer, fileSize, pngDrawCallback);
    if (rc != PNG_SUCCESS) {
        free(fileBuffer);
        Serial.printf("[ICON] PNG open failed: %d\n", rc);
        return nullptr;
    }

    // Get dimensions (limit to 32x32 to preserve RAM)
    uint8_t width = min((int)png.getWidth(), 32);
    uint8_t height = min((int)png.getHeight(), 32);

    // Allocate pixel buffer
    cached->pixels = (uint16_t*)malloc(width * height * sizeof(uint16_t));
    if (!cached->pixels) {
        png.close();
        free(fileBuffer);
        Serial.println("[ICON] Failed to allocate pixel buffer");
        return nullptr;
    }

    // Set up decode target
    pngDecodeTarget = cached->pixels;
    pngDecodeWidth = width;

    // Clear buffer
    memset(cached->pixels, 0, width * height * sizeof(uint16_t));

    // Decode PNG
    rc = png.decode(NULL, 0);
    png.close();
    free(fileBuffer);

    if (rc != PNG_SUCCESS) {
        free(cached->pixels);
        cached->pixels = nullptr;
        Serial.printf("[ICON] PNG decode failed: %d\n", rc);
        return nullptr;
    }

    // Update cache entry
    strlcpy(cached->name, name, sizeof(cached->name));
    cached->width = width;
    cached->height = height;
    cached->valid = true;
    cached->lastUsed = millis();

    Serial.printf("[ICON] Loaded: %s (%dx%d)\n", name, width, height);
    return cached;
}

bool isFailedIconDownload(const char* name) {
    unsigned long now = millis();
    for (uint8_t i = 0; i < MAX_FAILED_ICON_DOWNLOADS; i++) {
        if (failedIconDownloads[i].name[0] != '\0' &&
            strcmp(failedIconDownloads[i].name, name) == 0 &&
            (now - failedIconDownloads[i].failedAt) < FAILED_ICON_RETRY_DELAY) {
            return true;
        }
    }
    return false;
}

void addFailedIconDownload(const char* name) {
    // Find oldest entry to evict
    uint8_t oldestIndex = 0;
    unsigned long oldestTime = ULONG_MAX;
    for (uint8_t i = 0; i < MAX_FAILED_ICON_DOWNLOADS; i++) {
        if (failedIconDownloads[i].name[0] == '\0') {
            oldestIndex = i;
            break;
        }
        if (failedIconDownloads[i].failedAt < oldestTime) {
            oldestTime = failedIconDownloads[i].failedAt;
            oldestIndex = i;
        }
    }
    strlcpy(failedIconDownloads[oldestIndex].name, name, sizeof(failedIconDownloads[oldestIndex].name));
    failedIconDownloads[oldestIndex].failedAt = millis();
}

CachedIcon* getIcon(const char* name) {
    if (!name || strlen(name) == 0) return nullptr;

    // Search cache first
    for (uint8_t i = 0; i < MAX_ICON_CACHE; i++) {
        if (iconCache[i].valid && strcmp(iconCache[i].name, name) == 0) {
            iconCache[i].lastUsed = millis();
            return &iconCache[i];
        }
    }

    // Not in cache, try loading from filesystem
    CachedIcon* result = loadIcon(name);
    if (result) return result;

    // Auto-download LaMetric icons on demand
    if (strncmp(name, "lm_", 3) == 0) {
        const char* idStr = name + 3;
        // Validate that the rest is numeric
        bool isNumeric = (*idStr != '\0');
        for (const char* p = idStr; *p; p++) {
            if (*p < '0' || *p > '9') { isNumeric = false; break; }
        }
        if (isNumeric && !isFailedIconDownload(name)) {
            uint32_t iconId = strtoul(idStr, nullptr, 10);
            Serial.printf("[ICON] Auto-downloading LaMetric icon: %s (id=%u)\n", name, iconId);
            if (downloadLaMetricIcon(iconId, name)) {
                return loadIcon(name);
            } else {
                addFailedIconDownload(name);
                Serial.printf("[ICON] Download failed, blacklisted for %ds: %s\n",
                              FAILED_ICON_RETRY_DELAY / 1000, name);
            }
        }
    }

    return nullptr;
}

void drawIcon(CachedIcon* icon, int16_t x, int16_t y) {
    if (!icon || !icon->valid || !icon->pixels) return;

    // Upscale x2 for small icons (8x8 -> 16x16)
    uint8_t scale = (icon->width <= 8 && icon->height <= 8) ? 2 : 1;

    for (uint8_t py = 0; py < icon->height; py++) {
        for (uint8_t px = 0; px < icon->width; px++) {
            uint16_t pixel = icon->pixels[py * icon->width + px];
            if (pixel != 0) {  // Skip transparent/black pixels
                if (scale == 2) {
                    // Draw 2x2 block for each pixel
                    int16_t dx = x + px * 2;
                    int16_t dy = y + py * 2;
                    dma_display->drawPixel(dx, dy, pixel);
                    dma_display->drawPixel(dx + 1, dy, pixel);
                    dma_display->drawPixel(dx, dy + 1, pixel);
                    dma_display->drawPixel(dx + 1, dy + 1, pixel);
                } else {
                    dma_display->drawPixel(x + px, y + py, pixel);
                }
            }
        }
    }
}

void invalidateCachedIcon(const char* name) {
    if (!name || strlen(name) == 0) return;

    for (uint8_t i = 0; i < MAX_ICON_CACHE; i++) {
        if (iconCache[i].valid && strcmp(iconCache[i].name, name) == 0) {
            if (iconCache[i].pixels) {
                free(iconCache[i].pixels);
                iconCache[i].pixels = nullptr;
            }
            iconCache[i].valid = false;
            iconCache[i].name[0] = '\0';
            Serial.printf("[ICON] Invalidated cached icon: %s\n", name);
            return;
        }
    }
}

bool validatePngHeader(const uint8_t* data, size_t len) {
    if (len < 8) return false;
    // PNG magic bytes: 89 50 4E 47 0D 0A 1A 0A
    return (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
            data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A);
}

bool validateGifHeader(const uint8_t* data, size_t len) {
    if (len < 6) return false;
    // GIF magic: "GIF87a" or "GIF89a"
    return (data[0] == 'G' && data[1] == 'I' && data[2] == 'F' &&
            data[3] == '8' && (data[4] == '7' || data[4] == '9') && data[5] == 'a');
}

bool downloadLaMetricIcon(uint32_t iconId, const char* saveName) {
    if (!filesystemReady) {
        Serial.println("[LAMETRIC] Filesystem not ready");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();  // Skip certificate verification for simplicity

    HTTPClient https;
    bool isPng = true;

    // Try PNG first
    String url = "https://" LAMETRIC_API_HOST LAMETRIC_ICON_PATH + String(iconId) + ".png";
    Serial.printf("[LAMETRIC] Trying PNG: %s\n", url.c_str());

    if (!https.begin(client, url)) {
        Serial.println("[LAMETRIC] HTTPS begin failed");
        return false;
    }

    int httpCode = https.GET();

    // If PNG not found, try GIF
    if (httpCode != HTTP_CODE_OK) {
        https.end();
        url = "https://" LAMETRIC_API_HOST LAMETRIC_ICON_PATH + String(iconId) + ".gif";
        Serial.printf("[LAMETRIC] Trying GIF: %s\n", url.c_str());

        if (!https.begin(client, url)) {
            Serial.println("[LAMETRIC] HTTPS begin failed");
            return false;
        }

        httpCode = https.GET();
        isPng = false;
    }

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[LAMETRIC] HTTP error: %d\n", httpCode);
        https.end();
        return false;
    }

    // Check file size
    int contentLength = https.getSize();
    if (contentLength > MAX_ICON_SIZE) {
        Serial.printf("[LAMETRIC] Icon too large: %d bytes\n", contentLength);
        https.end();
        return false;
    }

    // Save file with appropriate extension
    String ext = isPng ? ".png" : ".gif";
    String path = String(FS_ICONS_PATH) + "/" + saveName + ext;

    File file = LittleFS.open(path, "w");
    if (!file) {
        Serial.printf("[LAMETRIC] Failed to create file: %s\n", path.c_str());
        https.end();
        return false;
    }

    // Stream response to file
    WiFiClient* stream = https.getStreamPtr();
    uint8_t buffer[256];
    size_t totalWritten = 0;

    while (https.connected() && (contentLength > 0 || contentLength == -1)) {
        size_t available = stream->available();
        if (available) {
            size_t toRead = min(available, sizeof(buffer));
            size_t bytesRead = stream->readBytes(buffer, toRead);
            file.write(buffer, bytesRead);
            totalWritten += bytesRead;
            if (contentLength > 0) {
                contentLength -= bytesRead;
            }
        } else {
            delay(1);
        }
    }

    file.close();
    https.end();

    Serial.printf("[LAMETRIC] Downloaded icon %d as %s (%d bytes)\n", iconId, path.c_str(), totalWritten);

    // Invalidate cache if icon with same name was cached
    invalidateCachedIcon(saveName);

    return true;
}

void handleApiIconsList(AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray icons = doc["icons"].to<JsonArray>();

    File root = LittleFS.open(FS_ICONS_PATH);
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            if (!file.isDirectory()) {
                JsonObject obj = icons.add<JsonObject>();
                String filename = String(file.name());
                // Remove path prefix if present
                int lastSlash = filename.lastIndexOf('/');
                if (lastSlash >= 0) {
                    filename = filename.substring(lastSlash + 1);
                }
                // Remove extension for the name
                int lastDot = filename.lastIndexOf('.');
                String name = lastDot > 0 ? filename.substring(0, lastDot) : filename;
                obj["name"] = name;
                obj["filename"] = filename;
                obj["size"] = file.size();
            }
            file = root.openNextFile();
        }
        root.close();
    }

    doc["count"] = icons.size();
    doc["storage"]["used"] = LittleFS.usedBytes();
    doc["storage"]["total"] = LittleFS.totalBytes();

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void handleApiIconsServe(AsyncWebServerRequest *request, const String& name) {
    // Try PNG first, then GIF
    String pngPath = String(FS_ICONS_PATH) + "/" + name + ".png";
    String gifPath = String(FS_ICONS_PATH) + "/" + name + ".gif";

    if (LittleFS.exists(pngPath)) {
        request->send(LittleFS, pngPath, "image/png");
    } else if (LittleFS.exists(gifPath)) {
        request->send(LittleFS, gifPath, "image/gif");
    } else {
        request->send(404, "application/json", "{\"error\":\"Icon not found\"}");
    }
}

void handleApiIconsDelete(AsyncWebServerRequest *request) {
    if (!request->hasParam("name")) {
        request->send(400, "application/json", "{\"error\":\"Missing name parameter\"}");
        return;
    }

    String name = request->getParam("name")->value();

    // Invalidate cache first
    invalidateCachedIcon(name.c_str());

    // Try to delete PNG or GIF
    String pngPath = String(FS_ICONS_PATH) + "/" + name + ".png";
    String gifPath = String(FS_ICONS_PATH) + "/" + name + ".gif";

    bool deleted = false;
    if (LittleFS.exists(pngPath)) {
        deleted = LittleFS.remove(pngPath);
    } else if (LittleFS.exists(gifPath)) {
        deleted = LittleFS.remove(gifPath);
    }

    if (deleted) {
        Serial.printf("[ICON] Deleted: %s\n", name.c_str());
        request->send(200, "application/json", "{\"success\":true}");
    } else {
        request->send(404, "application/json", "{\"error\":\"Icon not found\"}");
    }
}

// ============================================================================
// WiFi Functions
// ============================================================================

void setupWiFi() {
    wifiManager.setConfigPortalTimeout(180);
    wifiManager.setAPCallback([](WiFiManager *myWiFiManager) {
        Serial.println("[WIFI] Config portal started");
        dma_display->clearScreen();
        dma_display->setTextColor(dma_display->color565(255, 165, 0));
        dma_display->setCursor(4, 20);
        dma_display->print("WiFi Setup");
        dma_display->setTextColor(dma_display->color565(255, 255, 255));
        dma_display->setCursor(4, 35);
        dma_display->print(WIFI_AP_NAME);
        #if DOUBLE_BUFFER
            dma_display->flipDMABuffer();
        #endif
    });

    wifiConnected = wifiManager.autoConnect(WIFI_AP_NAME);

    if (wifiConnected) {
        Serial.print("[WIFI] Connected! IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("[WIFI] Failed to connect");
    }
}

void loopWiFi() {
    if (WiFi.status() != WL_CONNECTED && wifiConnected) {
        Serial.println("[WIFI] Connection lost, reconnecting...");
        wifiConnected = false;
        WiFi.reconnect();
    } else if (WiFi.status() == WL_CONNECTED && !wifiConnected) {
        Serial.println("[WIFI] Reconnected!");
        wifiConnected = true;
    }
}

// ============================================================================
// mDNS Functions
// ============================================================================

void setupMDNS() {
    if (MDNS.begin(MDNS_NAME)) {
        MDNS.addService("http", "tcp", WEB_SERVER_PORT);
        Serial.printf("[MDNS] Hostname: %s.local\n", MDNS_NAME);
    } else {
        Serial.println("[MDNS] Failed to start");
    }
}

// ============================================================================
// Web Server Functions
// ============================================================================

void setupWebServer() {
    // CORS headers via DefaultHeaders
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html",
            "<!DOCTYPE html><html><head><title>PixelCast</title></head>"
            "<body><h1>ESP32-PixelCast</h1>"
            "<p>Version: " VERSION_STRING "</p>"
            "<p><a href='/icons.html'>Icon Manager</a></p>"
            "<p><a href='/api/stats'>API Stats</a></p>"
            "<p><a href='/api/apps'>Active Apps</a></p>"
            "</body></html>"
        );
    });

    webServer.on("/api/stats", HTTP_GET, handleApiStats);
    webServer.on("/api/settings", HTTP_GET, handleApiSettings);
    webServer.on("/api/apps", HTTP_GET, handleApiApps);

    // POST /api/brightness - Set brightness (using AsyncCallbackJsonWebHandler)
    AsyncCallbackJsonWebHandler* brightnessHandler = new AsyncCallbackJsonWebHandler("/api/brightness",
        [](AsyncWebServerRequest *request, JsonVariant &json) {
            Serial.println("[API] /brightness handler called");
            JsonObject doc = json.as<JsonObject>();

            if (doc.isNull()) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            if (!doc["brightness"].isNull()) {
                uint8_t brightness = doc["brightness"].as<uint8_t>();
                displaySetBrightness(brightness);
                settings.brightness = brightness;
                saveSettings();
                Serial.printf("[API] Brightness set to %d\n", brightness);
                request->send(200, "application/json", "{\"success\":true}");
            } else {
                request->send(400, "application/json", "{\"error\":\"Missing brightness\"}");
            }
        });
    webServer.addHandler(brightnessHandler);

    // POST /api/custom - Create/update custom app (using AsyncCallbackJsonWebHandler)
    AsyncCallbackJsonWebHandler* customHandler = new AsyncCallbackJsonWebHandler("/api/custom",
        [](AsyncWebServerRequest *request, JsonVariant &json) {
            Serial.println("[API] /custom handler called");
            JsonObject doc = json.as<JsonObject>();

            if (doc.isNull()) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            // Get app name from query param or JSON
            String name;
            if (request->hasParam("name")) {
                name = request->getParam("name")->value();
            } else if (!doc["name"].isNull()) {
                name = doc["name"].as<String>();
            } else {
                request->send(400, "application/json", "{\"error\":\"Missing app name\"}");
                return;
            }

            // Check for multi-zone format
            JsonArray zonesArray = doc["zones"].as<JsonArray>();
            bool isMultiZone = !zonesArray.isNull() && zonesArray.size() > 0;

            if (isMultiZone) {
                uint8_t zoneCount = zonesArray.size();
                if (zoneCount == 1 || zoneCount > MAX_ZONES) {
                    request->send(400, "application/json",
                        "{\"error\":\"zones array must have 2, 3, or 4 elements\"}");
                    return;
                }
            }

            // For multi-zone, zone 0 provides the main fields; for single-zone, use top-level fields
            const char* icon = isMultiZone ? "" : (doc["icon"] | "");
            uint32_t textColor = isMultiZone ? 0xFFFFFF : parseColorValue(doc["color"], 0xFFFFFF);

            // Parse text field (may be string, {text,color} object, or [{t,c},...] array)
            char parsedText[64] = "";
            TextSegment textSegs[MAX_TEXT_SEGMENTS];
            uint8_t textSegCount = 0;
            if (!isMultiZone) {
                parseTextFieldWithSegments(doc["text"], parsedText, sizeof(parsedText),
                                           textSegs, &textSegCount, textColor);
            }

            uint16_t duration = doc["duration"] | settings.defaultDuration;
            uint32_t lifetime = doc["lifetime"] | 0;
            int8_t priority = doc["priority"] | 0;

            int8_t result = appAdd(name.c_str(), parsedText, icon, textColor,
                                   duration, lifetime, priority, false);

            if (result >= 0) {
                if (!isMultiZone) {
                    // Copy text segments
                    memcpy(apps[result].textSegments, textSegs, sizeof(textSegs));
                    apps[result].textSegmentCount = textSegCount;
                    // Parse label field
                    parseTextFieldWithSegments(doc["label"], apps[result].label,
                                               sizeof(apps[result].label),
                                               apps[result].labelSegments,
                                               &apps[result].labelSegmentCount, textColor);
                }
                // Apply multi-zone data if present
                if (isMultiZone) {
                    appSetZones(result, zonesArray);
                }
                Serial.printf("[API] Custom app '%s' created/updated\n", name.c_str());
                request->send(200, "application/json", "{\"success\":true}");
            } else {
                request->send(500, "application/json", "{\"error\":\"Failed to add app\"}");
            }
        });
    webServer.addHandler(customHandler);

    // DELETE /api/custom - Delete custom app
    webServer.on("/api/custom", HTTP_DELETE, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("name")) {
            request->send(400, "application/json", "{\"error\":\"Missing app name\"}");
            return;
        }

        String name = request->getParam("name")->value();
        if (appRemove(name.c_str())) {
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            request->send(404, "application/json", "{\"error\":\"App not found or is system app\"}");
        }
    });

    // POST /api/settings - Update settings (using AsyncCallbackJsonWebHandler)
    AsyncCallbackJsonWebHandler* settingsHandler = new AsyncCallbackJsonWebHandler("/api/settings",
        [](AsyncWebServerRequest *request, JsonVariant &json) {
            Serial.println("[API] /settings handler called");
            JsonObject doc = json.as<JsonObject>();

            if (doc.isNull()) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            // Update settings from JSON
            if (!doc["brightness"].isNull()) {
                settings.brightness = doc["brightness"].as<uint8_t>();
                displaySetBrightness(settings.brightness);
            }
            if (!doc["autoRotate"].isNull()) {
                settings.autoRotate = doc["autoRotate"].as<bool>();
                appRotationEnabled = settings.autoRotate;
            }
            if (!doc["defaultDuration"].isNull()) {
                settings.defaultDuration = doc["defaultDuration"].as<uint16_t>();
            }

            saveSettings();
            Serial.println("[API] Settings updated");
            request->send(200, "application/json", "{\"success\":true}");
        });
    webServer.addHandler(settingsHandler);

    // GET /api/weather - Return current weather data
    webServer.on("/api/weather", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;

        doc["valid"] = weatherData.valid;

        if (weatherData.valid) {
            unsigned long ageMs = millis() - weatherData.lastUpdate;
            doc["age"] = ageMs / 1000;
            doc["stale"] = (ageMs > 3600000);

            JsonObject current = doc["current"].to<JsonObject>();
            current["icon"] = weatherData.currentIcon;
            current["temp"] = weatherData.currentTemp;
            current["temp_min"] = weatherData.currentTempMin;
            current["temp_max"] = weatherData.currentTempMax;
            current["humidity"] = weatherData.currentHumidity;

            JsonArray forecastArr = doc["forecast"].to<JsonArray>();
            for (int i = 0; i < weatherData.forecastCount; i++) {
                JsonObject fc = forecastArr.add<JsonObject>();
                fc["day"] = weatherData.forecast[i].dayName;
                fc["icon"] = weatherData.forecast[i].icon;
                fc["temp_min"] = weatherData.forecast[i].tempMin;
                fc["temp_max"] = weatherData.forecast[i].tempMax;
            }
        }

        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    // POST /api/weather - Update weather data
    AsyncCallbackJsonWebHandler* weatherHandler = new AsyncCallbackJsonWebHandler("/api/weather",
        [](AsyncWebServerRequest *request, JsonVariant &json) {
            Serial.println("[API] /weather handler called");
            JsonObject doc = json.as<JsonObject>();

            if (doc.isNull()) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            // Parse current weather
            if (doc["current"].is<JsonObject>()) {
                JsonObject current = doc["current"];
                strlcpy(weatherData.currentIcon, current["icon"] | "", sizeof(weatherData.currentIcon));
                weatherData.currentTemp = current["temp"] | 0;
                weatherData.currentTempMin = current["temp_min"] | 0;
                weatherData.currentTempMax = current["temp_max"] | 0;
                weatherData.currentHumidity = current["humidity"] | 0;
            } else {
                request->send(400, "application/json", "{\"error\":\"Missing 'current' object\"}");
                return;
            }

            // Parse forecast (optional, up to MAX_FORECAST_DAYS days)
            if (doc["forecast"].is<JsonArray>()) {
                JsonArray forecastArr = doc["forecast"];
                int forecastSize = min((int)forecastArr.size(), (int)MAX_FORECAST_DAYS);
                for (int i = 0; i < forecastSize; i++) {
                    JsonObject fc = forecastArr[i];
                    strlcpy(weatherData.forecast[i].icon, fc["icon"] | "", sizeof(weatherData.forecast[i].icon));
                    weatherData.forecast[i].tempMin = fc["temp_min"] | 0;
                    weatherData.forecast[i].tempMax = fc["temp_max"] | 0;
                    strlcpy(weatherData.forecast[i].dayName, fc["day"] | "", sizeof(weatherData.forecast[i].dayName));
                }
                weatherData.forecastCount = forecastSize;
            } else {
                weatherData.forecastCount = 0;
            }

            // Reset forecast pagination on new data
            forecastPage = 0;
            lastForecastPageSwitch = millis();

            weatherData.lastUpdate = millis();
            weatherData.valid = true;

            Serial.printf("[WEATHER] Updated: %d C, %d%% humidity\n",
                         weatherData.currentTemp, weatherData.currentHumidity);
            request->send(200, "application/json", "{\"success\":true}");
        });
    webServer.addHandler(weatherHandler);

    // ========================================================================
    // Tracker API
    // ========================================================================

    // GET /api/trackers - List all active trackers
    webServer.on("/api/trackers", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        JsonArray arr = doc["trackers"].to<JsonArray>();

        for (uint8_t i = 0; i < MAX_TRACKERS; i++) {
            if (trackers[i].valid) {
                JsonObject t = arr.add<JsonObject>();
                t["name"] = trackers[i].name;
                t["symbol"] = trackers[i].symbol;
                t["value"] = trackers[i].currentValue;
                t["change"] = trackers[i].changePercent;
                unsigned long ageMs = millis() - trackers[i].lastUpdate;
                t["age"] = ageMs / 1000;
                t["stale"] = (ageMs > TRACKER_STALE_TIMEOUT);
            }
        }
        doc["count"] = trackerCount;

        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    // GET /api/tracker?name=btc - Get single tracker data
    webServer.on("/api/tracker", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("name")) {
            request->send(400, "application/json", "{\"error\":\"Missing tracker name\"}");
            return;
        }

        String name = request->getParam("name")->value();
        TrackerData* tracker = trackerFind(name.c_str());

        if (!tracker) {
            request->send(404, "application/json", "{\"error\":\"Tracker not found\"}");
            return;
        }

        JsonDocument doc;
        doc["name"] = tracker->name;
        doc["symbol"] = tracker->symbol;
        doc["icon"] = tracker->icon;
        doc["currency"] = tracker->currencySymbol;
        doc["value"] = tracker->currentValue;
        doc["change"] = tracker->changePercent;
        doc["symbolColor"] = tracker->symbolColor;
        doc["sparklineColor"] = tracker->sparklineColor;
        doc["bottomText"] = tracker->bottomText;

        unsigned long ageMs = millis() - tracker->lastUpdate;
        doc["age"] = ageMs / 1000;
        doc["stale"] = (ageMs > TRACKER_STALE_TIMEOUT);

        if (tracker->sparklineCount > 0) {
            JsonArray sparkArr = doc["sparkline"].to<JsonArray>();
            for (uint8_t i = 0; i < tracker->sparklineCount; i++) {
                sparkArr.add(tracker->sparkline[i]);
            }
        }

        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    // DELETE /api/tracker?name=btc - Remove tracker
    webServer.on("/api/tracker", HTTP_DELETE, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("name")) {
            request->send(400, "application/json", "{\"error\":\"Missing tracker name\"}");
            return;
        }

        String name = request->getParam("name")->value();
        if (trackerRemove(name.c_str())) {
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            request->send(404, "application/json", "{\"error\":\"Tracker not found\"}");
        }
    });

    // POST /api/tracker?name=btc - Create/update tracker
    AsyncCallbackJsonWebHandler* trackerHandler = new AsyncCallbackJsonWebHandler("/api/tracker",
        [](AsyncWebServerRequest *request, JsonVariant &json) {
            Serial.println("[API] /tracker handler called");
            JsonObject doc = json.as<JsonObject>();

            if (doc.isNull()) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            // Get tracker name from query param or JSON
            String name;
            if (request->hasParam("name")) {
                name = request->getParam("name")->value();
            } else if (!doc["name"].isNull()) {
                name = doc["name"].as<String>();
            } else {
                request->send(400, "application/json", "{\"error\":\"Missing tracker name\"}");
                return;
            }

            // Allocate or find existing tracker
            TrackerData* tracker = trackerAllocate(name.c_str());
            if (!tracker) {
                request->send(500, "application/json", "{\"error\":\"No tracker slot available\"}");
                return;
            }

            // Parse fields
            if (!doc["symbol"].isNull()) {
                strlcpy(tracker->symbol, doc["symbol"] | "", sizeof(tracker->symbol));
            }
            if (!doc["icon"].isNull()) {
                strlcpy(tracker->icon, doc["icon"] | "", sizeof(tracker->icon));
            }
            if (!doc["currency"].isNull()) {
                strlcpy(tracker->currencySymbol, doc["currency"] | "", sizeof(tracker->currencySymbol));
            }
            if (!doc["value"].isNull()) {
                tracker->currentValue = doc["value"].as<float>();
            }
            if (!doc["change"].isNull()) {
                tracker->changePercent = doc["change"].as<float>();
            }
            if (!doc["bottomText"].isNull()) {
                strlcpy(tracker->bottomText, doc["bottomText"] | "", sizeof(tracker->bottomText));
            }

            tracker->symbolColor = parseColorValue(doc["symbolColor"], tracker->symbolColor);
            tracker->sparklineColor = parseColorValue(doc["sparklineColor"], tracker->sparklineColor);

            // Parse sparkline data (float array -> scaled uint16)
            if (doc["sparkline"].is<JsonArray>()) {
                JsonArray sparkArr = doc["sparkline"];
                uint8_t count = min((int)sparkArr.size(), (int)MAX_SPARKLINE_POINTS);

                if (count >= 2) {
                    // Find min/max of float values
                    float minVal = sparkArr[0].as<float>();
                    float maxVal = minVal;
                    for (uint8_t i = 1; i < count; i++) {
                        float v = sparkArr[i].as<float>();
                        if (v < minVal) minVal = v;
                        if (v > maxVal) maxVal = v;
                    }

                    float range = maxVal - minVal;
                    if (range < 0.0001f) range = 1.0f;

                    // Scale to uint16 (0-65535)
                    for (uint8_t i = 0; i < count; i++) {
                        float normalized = (sparkArr[i].as<float>() - minVal) / range;
                        tracker->sparkline[i] = (uint16_t)(normalized * 65535.0f);
                    }
                    tracker->sparklineCount = count;
                }
            }

            tracker->lastUpdate = millis();

            // Register/update app in rotation
            char appId[32];
            snprintf(appId, sizeof(appId), "%s%s", TRACKER_ID_PREFIX, name.c_str());
            uint16_t duration = doc["duration"] | (uint16_t)DEFAULT_APP_DURATION;
            appAdd(appId, tracker->symbol, tracker->icon, 0xFFFFFF,
                   duration, 0, 0, false);

            Serial.printf("[TRACKER] Updated: %s (%s = %.2f)\n",
                         name.c_str(), tracker->symbol, tracker->currentValue);
            request->send(200, "application/json", "{\"success\":true}");
        });
    webServer.addHandler(trackerHandler);

    // ========================================================================
    // Notification API
    // ========================================================================

    // POST /api/notify/dismiss - Dismiss current notification
    // IMPORTANT: Must be registered BEFORE /api/notify JSON handler to avoid prefix match
    webServer.on("/api/notify/dismiss", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (notifDismiss()) {
            resetNotifScrollState();
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            request->send(404, "application/json", "{\"error\":\"No active notification\"}");
        }
    });

    // GET /api/notify/list - List all active notifications
    // IMPORTANT: Must be registered BEFORE /api/notify JSON handler to avoid prefix match
    webServer.on("/api/notify/list", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        doc["count"] = notificationCount;
        doc["currentIndex"] = currentNotifIndex;

        JsonArray arr = doc["notifications"].to<JsonArray>();
        for (uint8_t i = 0; i < MAX_NOTIFICATIONS; i++) {
            if (!notifications[i].active) continue;
            JsonObject obj = arr.add<JsonObject>();
            obj["id"] = notifications[i].id;
            obj["text"] = notifications[i].text;
            obj["icon"] = notifications[i].icon;
            obj["duration"] = notifications[i].duration;
            obj["hold"] = notifications[i].hold;
            obj["urgent"] = notifications[i].urgent;
            obj["stack"] = notifications[i].stack;
            obj["displayed"] = notifications[i].displayedAt > 0;
            obj["current"] = (i == (uint8_t)currentNotifIndex);
        }

        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    // POST /api/notify - Send a notification
    AsyncCallbackJsonWebHandler* notifyHandler = new AsyncCallbackJsonWebHandler("/api/notify",
        [](AsyncWebServerRequest *request, JsonVariant &json) {
            Serial.println("[API] /notify handler called");
            JsonObject doc = json.as<JsonObject>();

            if (doc.isNull()) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            // Text is required
            if (doc["text"].isNull() || strlen(doc["text"] | "") == 0) {
                request->send(400, "application/json", "{\"error\":\"Missing text\"}");
                return;
            }

            const char* text = doc["text"] | "";
            const char* id = doc["id"] | "";
            const char* icon = doc["icon"] | "";
            uint32_t textColor = parseColorValue(doc["color"], 0xFFFFFF);
            uint32_t bgColor = parseColorValue(doc["background"], 0x000000);
            uint16_t duration = doc["duration"] | (uint16_t)DEFAULT_NOTIF_DURATION;
            bool hold = doc["hold"] | false;
            bool urgent = doc["urgent"] | false;
            bool stack = doc["stack"] | true;

            int8_t slot = notifAdd(id, text, icon, textColor, bgColor,
                                   duration, hold, urgent, stack);

            if (slot < 0) {
                request->send(503, "application/json", "{\"error\":\"Notification queue full\"}");
                return;
            }

            // Return the assigned ID
            char response[128];
            snprintf(response, sizeof(response),
                     "{\"success\":true,\"id\":\"%s\"}", notifications[slot].id);
            request->send(200, "application/json", response);
        });
    webServer.addHandler(notifyHandler);

    // DELETE /api/indicator{1-3} - Turn off indicator
    webServer.on("/api/indicator1", HTTP_DELETE, [](AsyncWebServerRequest *request) {
        indicatorOff(0); saveSettings();
        Serial.println("[API] Indicator 1 turned off (DELETE)");
        request->send(200, "application/json", "{\"success\":true,\"mode\":\"off\"}");
    });
    webServer.on("/api/indicator2", HTTP_DELETE, [](AsyncWebServerRequest *request) {
        indicatorOff(1); saveSettings();
        Serial.println("[API] Indicator 2 turned off (DELETE)");
        request->send(200, "application/json", "{\"success\":true,\"mode\":\"off\"}");
    });
    webServer.on("/api/indicator3", HTTP_DELETE, [](AsyncWebServerRequest *request) {
        indicatorOff(2); saveSettings();
        Serial.println("[API] Indicator 3 turned off (DELETE)");
        request->send(200, "application/json", "{\"success\":true,\"mode\":\"off\"}");
    });

    // POST /api/indicator{1-3} - Set corner indicators
    for (uint8_t idx = 0; idx < NUM_INDICATORS; idx++) {
        String path = "/api/indicator" + String(idx + 1);
        AsyncCallbackJsonWebHandler* indicatorHandler = new AsyncCallbackJsonWebHandler(
            path.c_str(),
            [idx](AsyncWebServerRequest *request, JsonVariant &json) {
                handleIndicatorApi(request, json, idx);
            });
        indicatorHandler->setMethod(HTTP_POST);
        webServer.addHandler(indicatorHandler);
    }
    Serial.println("[WEB] Indicator API endpoints registered");

    // POST /api/reboot - Reboot device (deferred to allow response to be sent)
    webServer.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
        Serial.println("[API] Reboot requested");
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Rebooting...\"}");
        pendingReboot = true;
        rebootRequestTime = millis();
    });

    // ========================================================================
    // Icon Management API
    // ========================================================================

    // GET /icons.html - Web interface for icon management
    webServer.on("/icons.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", ICONS_HTML);
    });

    // GET /api/icons/{name} - Serve icon file (must be before /api/icons to avoid prefix match)
    webServer.on("^\\/api\\/icons\\/([a-zA-Z0-9_-]+)$", HTTP_GET, [](AsyncWebServerRequest *request) {
        String iconName = request->pathArg(0);
        handleApiIconsServe(request, iconName);
    });

    // GET /api/icons - List all icons
    webServer.on("/api/icons", HTTP_GET, handleApiIconsList);

    // DELETE /api/icons?name={name} - Delete an icon
    webServer.on("/api/icons", HTTP_DELETE, handleApiIconsDelete);

    // POST /api/icons/lametric - Download icon from LaMetric
    // IMPORTANT: Must be registered BEFORE /api/icons POST to avoid path conflict
    AsyncCallbackJsonWebHandler* lametricHandler = new AsyncCallbackJsonWebHandler(
        "/api/icons/lametric",
        [](AsyncWebServerRequest *request, JsonVariant &json) {
            JsonObject doc = json.as<JsonObject>();

            if (doc.isNull()) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            uint32_t iconId = doc["id"] | 0;
            if (iconId == 0) {
                request->send(400, "application/json", "{\"error\":\"Missing or invalid icon id\"}");
                return;
            }

            // Use provided name or icon ID as name
            String name = doc["name"] | String(iconId);

            Serial.printf("[API] LaMetric download request: id=%d, name=%s\n", iconId, name.c_str());

            if (downloadLaMetricIcon(iconId, name.c_str())) {
                request->send(200, "application/json", "{\"success\":true}");
            } else {
                request->send(500, "application/json", "{\"error\":\"Failed to download icon from LaMetric\"}");
            }
        });
    webServer.addHandler(lametricHandler);

    // POST /api/icons?name={name} - Upload icon (multipart/form-data)
    webServer.on("/api/icons", HTTP_POST,
        // Completion handler
        [](AsyncWebServerRequest *request) {
            if (uploadValid && uploadSize > 0) {
                Serial.printf("[ICON] Upload complete: %s (%d bytes)\n",
                              uploadIconName.c_str(), uploadSize);
                request->send(200, "application/json", "{\"success\":true}");
            } else {
                // Clean up failed upload
                if (uploadIconName.length() > 0) {
                    String path = String(FS_ICONS_PATH) + "/" + uploadIconName + ".png";
                    if (LittleFS.exists(path)) {
                        LittleFS.remove(path);
                    }
                    path = String(FS_ICONS_PATH) + "/" + uploadIconName + ".gif";
                    if (LittleFS.exists(path)) {
                        LittleFS.remove(path);
                    }
                }
                request->send(400, "application/json", "{\"error\":\"Upload failed - invalid file format or size\"}");
            }
            // Reset upload state
            uploadIconName = "";
            uploadValid = false;
            uploadSize = 0;
        },
        // Chunk handler for file upload
        [](AsyncWebServerRequest *request, String filename, size_t index,
           uint8_t *data, size_t len, bool final) {

            if (index == 0) {
                // First chunk - initialize upload
                if (!request->hasParam("name")) {
                    Serial.println("[ICON] Upload missing name parameter");
                    uploadValid = false;
                    return;
                }

                uploadIconName = request->getParam("name")->value();
                uploadSize = 0;

                // Validate file header
                bool isPng = validatePngHeader(data, len);
                bool isGif = validateGifHeader(data, len);

                if (!isPng && !isGif) {
                    Serial.println("[ICON] Invalid file format (not PNG or GIF)");
                    uploadValid = false;
                    return;
                }

                // Determine extension based on format
                String ext = isPng ? ".png" : ".gif";
                String path = String(FS_ICONS_PATH) + "/" + uploadIconName + ext;

                uploadFile = LittleFS.open(path, "w");
                if (!uploadFile) {
                    Serial.printf("[ICON] Failed to create file: %s\n", path.c_str());
                    uploadValid = false;
                    return;
                }

                uploadValid = true;
                Serial.printf("[ICON] Upload started: %s\n", path.c_str());
            }

            // Write data chunk
            if (uploadValid && uploadFile) {
                // Check size limit
                if (uploadSize + len > MAX_ICON_SIZE) {
                    Serial.println("[ICON] Upload exceeds size limit");
                    uploadFile.close();
                    uploadValid = false;
                    return;
                }

                size_t written = uploadFile.write(data, len);
                uploadSize += written;
            }

            // Final chunk
            if (final && uploadFile) {
                uploadFile.close();
                if (uploadValid) {
                    // Invalidate cached icon
                    invalidateCachedIcon(uploadIconName.c_str());
                }
            }
        }
    );

    // Handle dynamic routes not caught by static handlers
    webServer.onNotFound([](AsyncWebServerRequest *request) {
        // Handle CORS preflight
        if (request->method() == HTTP_OPTIONS) {
            request->send(200);
            return;
        }

        String url = request->url();
        WebRequestMethodComposite method = request->method();

        // Handle DELETE routes (fallback if static handler misses due to HTTP_DELETE enum conflict)
        const WebRequestMethodComposite HTTP_DELETE_METHOD = 0b00000100;
        if (method == HTTP_DELETE_METHOD && url == "/api/icons") {
            handleApiIconsDelete(request);
            return;
        }
        if (method == HTTP_DELETE_METHOD && url == "/api/tracker") {
            if (!request->hasParam("name")) {
                request->send(400, "application/json", "{\"error\":\"Missing tracker name\"}");
                return;
            }
            String name = request->getParam("name")->value();
            if (trackerRemove(name.c_str())) {
                request->send(200, "application/json", "{\"success\":true}");
            } else {
                request->send(404, "application/json", "{\"error\":\"Tracker not found\"}");
            }
            return;
        }
        if (method == HTTP_DELETE_METHOD && url.startsWith("/api/indicator")) {
            // Extract indicator number from URL (last char)
            char lastChar = url.charAt(url.length() - 1);
            uint8_t idx = lastChar - '1';  // '1'->0, '2'->1, '3'->2
            if (idx < NUM_INDICATORS) {
                indicatorOff(idx);
                saveSettings();
                Serial.printf("[API] Indicator %d turned off (DELETE)\n", idx + 1);
                request->send(200, "application/json", "{\"success\":true,\"mode\":\"off\"}");
            } else {
                request->send(400, "application/json", "{\"error\":\"Invalid indicator number\"}");
            }
            return;
        }
        if (method == HTTP_DELETE_METHOD && url == "/api/custom") {
            if (!request->hasParam("name")) {
                request->send(400, "application/json", "{\"error\":\"Missing app name\"}");
                return;
            }
            String name = request->getParam("name")->value();
            if (appRemove(name.c_str())) {
                request->send(200, "application/json", "{\"success\":true}");
            } else {
                request->send(404, "application/json", "{\"error\":\"App not found or is system app\"}");
            }
            return;
        }

        // Handle GET /api/icons/{name} for serving icon files (fallback)
        if (method == HTTP_GET && url.startsWith("/api/icons/")) {
            String iconName = url.substring(11);  // Remove "/api/icons/"
            // Sanitize: only allow alphanumeric, underscore, hyphen
            bool valid = true;
            for (size_t i = 0; i < iconName.length(); i++) {
                char c = iconName[i];
                if (!isalnum(c) && c != '_' && c != '-') {
                    valid = false;
                    break;
                }
            }
            if (valid && iconName.length() > 0) {
                handleApiIconsServe(request, iconName);
                return;
            }
        }

        request->send(404, "application/json", "{\"error\":\"Not found\"}");
    });

    webServer.begin();
    Serial.printf("[WEB] Server started on port %d\n", WEB_SERVER_PORT);
}

void handleApiStats(AsyncWebServerRequest *request) {
    JsonDocument doc;

    doc["version"] = VERSION_STRING;
    doc["uptime"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["maxAllocHeap"] = ESP.getMaxAllocHeap();
    doc["brightness"] = settings.brightness;
    doc["wifi"]["ssid"] = WiFi.SSID();
    doc["wifi"]["rssi"] = WiFi.RSSI();
    doc["wifi"]["ip"] = WiFi.localIP().toString();
    doc["display"]["width"] = DISPLAY_WIDTH;
    doc["display"]["height"] = DISPLAY_HEIGHT;
    doc["mqtt"]["connected"] = mqttConnected;
    doc["apps"]["count"] = appCount;
    doc["apps"]["current"] = currentAppIndex >= 0 ? apps[currentAppIndex].id : "";
    doc["apps"]["rotationEnabled"] = appRotationEnabled;
    doc["filesystem"]["ready"] = filesystemReady;
    if (filesystemReady) {
        doc["filesystem"]["total"] = LittleFS.totalBytes();
        doc["filesystem"]["used"] = LittleFS.usedBytes();
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void handleApiSettings(AsyncWebServerRequest *request) {
    JsonDocument doc;

    doc["brightness"] = settings.brightness;
    doc["autoRotate"] = settings.autoRotate;
    doc["defaultDuration"] = settings.defaultDuration;
    doc["display"]["width"] = DISPLAY_WIDTH;
    doc["display"]["height"] = DISPLAY_HEIGHT;
    doc["ntp"]["server"] = settings.ntpServer;
    doc["ntp"]["offset"] = settings.ntpOffset;
    doc["mqtt"]["enabled"] = settings.mqttEnabled;
    doc["mqtt"]["prefix"] = settings.mqttPrefix;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void handleApiApps(AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray appsArray = doc["apps"].to<JsonArray>();

    for (uint8_t i = 0; i < MAX_APPS; i++) {
        if (apps[i].active) {
            JsonObject appObj = appsArray.add<JsonObject>();
            appObj["id"] = apps[i].id;
            appObj["icon"] = apps[i].icon;
            appObj["duration"] = apps[i].duration;
            appObj["lifetime"] = apps[i].lifetime;
            appObj["priority"] = apps[i].priority;
            appObj["isSystem"] = apps[i].isSystem;
            appObj["isCurrent"] = (currentAppIndex == i);

            // Color as hex string
            char colorHex[8];
            formatColorHex(apps[i].textColor, colorHex, sizeof(colorHex));
            appObj["color"] = colorHex;

            // Text and label in polymorphic format
            serializeTextField(appObj, "text", apps[i].text,
                               apps[i].textSegments, apps[i].textSegmentCount);
            if (apps[i].label[0] != '\0') {
                serializeTextField(appObj, "label", apps[i].label,
                                   apps[i].labelSegments, apps[i].labelSegmentCount);
            }

            // Multi-zone data
            if (apps[i].zoneCount >= 2) {
                appObj["zoneCount"] = apps[i].zoneCount;
                JsonArray zonesArr = appObj["zones"].to<JsonArray>();
                // Zone 0 from main fields
                JsonObject z0 = zonesArr.add<JsonObject>();
                serializeTextField(z0, "text", apps[i].text,
                                   apps[i].textSegments, apps[i].textSegmentCount);
                z0["icon"] = apps[i].icon;
                if (apps[i].label[0] != '\0') {
                    serializeTextField(z0, "label", apps[i].label,
                                       apps[i].labelSegments, apps[i].labelSegmentCount);
                }
                char z0ColorHex[8];
                formatColorHex(apps[i].textColor, z0ColorHex, sizeof(z0ColorHex));
                z0["color"] = z0ColorHex;
                // Zones 1-N
                for (uint8_t z = 1; z < apps[i].zoneCount; z++) {
                    JsonObject zObj = zonesArr.add<JsonObject>();
                    serializeTextField(zObj, "text", apps[i].zones[z - 1].text,
                                       apps[i].zones[z - 1].textSegments,
                                       apps[i].zones[z - 1].textSegmentCount);
                    zObj["icon"] = apps[i].zones[z - 1].icon;
                    if (apps[i].zones[z - 1].label[0] != '\0') {
                        serializeTextField(zObj, "label", apps[i].zones[z - 1].label,
                                           apps[i].zones[z - 1].labelSegments,
                                           apps[i].zones[z - 1].labelSegmentCount);
                    }
                    char zColorHex[8];
                    formatColorHex(apps[i].zones[z - 1].textColor, zColorHex, sizeof(zColorHex));
                    zObj["color"] = zColorHex;
                }
            }
        }
    }

    doc["count"] = appCount;
    doc["currentIndex"] = currentAppIndex;
    doc["rotationEnabled"] = appRotationEnabled;

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// ============================================================================
// MQTT Functions
// ============================================================================

void setupMQTT() {
    Serial.println("[MQTT] Not configured (TODO: implement config)");
}

void loopMQTT() {
    if (!wifiConnected) return;

    if (millis() - lastStatsPublish > MQTT_STATS_INTERVAL) {
        mqttPublishStats();
        lastStatsPublish = millis();
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[MQTT] Message on topic: %s\n", topic);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error) {
        Serial.printf("[MQTT] JSON parse error: %s\n", error.c_str());
        return;
    }
}

void mqttPublishStats() {
    if (!mqttConnected) return;

    JsonDocument doc;
    doc["uptime"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["brightness"] = currentBrightness;
    doc["rssi"] = WiFi.RSSI();

    String payload;
    serializeJson(doc, payload);
}

// ============================================================================
// Filesystem Functions
// ============================================================================

void setupFilesystem() {
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed!");
        filesystemReady = false;
        return;
    }

    filesystemReady = true;
    Serial.printf("[FS] LittleFS mounted, total: %d bytes, used: %d bytes\n",
        LittleFS.totalBytes(), LittleFS.usedBytes());

    ensureDirectories();
}

bool ensureDirectories() {
    if (!filesystemReady) return false;

    const char* dirs[] = {FS_ICONS_PATH, FS_GIFS_PATH, FS_CONFIG_PATH};
    bool allOk = true;

    for (const char* dir : dirs) {
        if (!LittleFS.exists(dir)) {
            if (LittleFS.mkdir(dir)) {
                Serial.printf("[FS] Created directory: %s\n", dir);
            } else {
                Serial.printf("[FS] Failed to create directory: %s\n", dir);
                allOk = false;
            }
        }
    }

    return allOk;
}

void initDefaultSettings() {
    settings.brightness = DEFAULT_BRIGHTNESS;
    settings.autoRotate = true;
    settings.defaultDuration = DEFAULT_APP_DURATION;

    strlcpy(settings.ntpServer, NTP_SERVER, sizeof(settings.ntpServer));
    settings.ntpOffset = NTP_OFFSET;

    settings.clockEnabled = true;
    settings.clockFormat24h = true;
    settings.clockShowSeconds = true;
    settings.clockColor = 0xFFFFFF;

    settings.dateEnabled = true;
    strlcpy(settings.dateFormat, "DD/MM/YYYY", sizeof(settings.dateFormat));
    settings.dateColor = 0x6464FF;

    settings.mqttEnabled = false;
    settings.mqttServer[0] = '\0';
    settings.mqttPort = 1883;
    settings.mqttUser[0] = '\0';
    settings.mqttPassword[0] = '\0';
    strlcpy(settings.mqttPrefix, MQTT_PREFIX, sizeof(settings.mqttPrefix));
}

bool loadSettings() {
    if (!filesystemReady) {
        Serial.println("[SETTINGS] Filesystem not ready");
        return false;
    }

    File file = LittleFS.open(FS_CONFIG_FILE, "r");
    if (!file) {
        Serial.println("[SETTINGS] Config file not found");
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.printf("[SETTINGS] JSON parse error: %s\n", error.c_str());
        return false;
    }

    // Display settings
    settings.brightness = doc["display"]["brightness"] | DEFAULT_BRIGHTNESS;
    settings.autoRotate = doc["display"]["autoRotate"] | true;
    settings.defaultDuration = doc["display"]["defaultDuration"] | DEFAULT_APP_DURATION;

    // NTP settings
    const char* ntpSrv = doc["ntp"]["server"] | NTP_SERVER;
    strlcpy(settings.ntpServer, ntpSrv, sizeof(settings.ntpServer));
    settings.ntpOffset = doc["ntp"]["offset"] | NTP_OFFSET;

    // Clock app settings
    settings.clockEnabled = doc["apps"]["clock"]["enabled"] | true;
    settings.clockFormat24h = doc["apps"]["clock"]["format24h"] | true;
    settings.clockShowSeconds = doc["apps"]["clock"]["showSeconds"] | true;

    JsonArray clockColorArr = doc["apps"]["clock"]["color"];
    if (clockColorArr.size() == 3) {
        settings.clockColor = ((uint32_t)clockColorArr[0].as<uint8_t>() << 16) |
                              ((uint32_t)clockColorArr[1].as<uint8_t>() << 8) |
                              (uint32_t)clockColorArr[2].as<uint8_t>();
    } else {
        settings.clockColor = 0xFFFFFF;
    }

    // Date app settings
    settings.dateEnabled = doc["apps"]["date"]["enabled"] | true;
    const char* dateFmt = doc["apps"]["date"]["format"] | "DD/MM/YYYY";
    strlcpy(settings.dateFormat, dateFmt, sizeof(settings.dateFormat));

    JsonArray dateColorArr = doc["apps"]["date"]["color"];
    if (dateColorArr.size() == 3) {
        settings.dateColor = ((uint32_t)dateColorArr[0].as<uint8_t>() << 16) |
                             ((uint32_t)dateColorArr[1].as<uint8_t>() << 8) |
                             (uint32_t)dateColorArr[2].as<uint8_t>();
    } else {
        settings.dateColor = 0x6464FF;
    }

    // MQTT settings
    settings.mqttEnabled = doc["mqtt"]["enabled"] | false;
    const char* mqttSrv = doc["mqtt"]["server"] | "";
    strlcpy(settings.mqttServer, mqttSrv, sizeof(settings.mqttServer));
    settings.mqttPort = doc["mqtt"]["port"] | 1883;
    const char* mqttUsr = doc["mqtt"]["user"] | "";
    strlcpy(settings.mqttUser, mqttUsr, sizeof(settings.mqttUser));
    const char* mqttPwd = doc["mqtt"]["password"] | "";
    strlcpy(settings.mqttPassword, mqttPwd, sizeof(settings.mqttPassword));
    const char* mqttPfx = doc["mqtt"]["prefix"] | MQTT_PREFIX;
    strlcpy(settings.mqttPrefix, mqttPfx, sizeof(settings.mqttPrefix));

    // Indicator settings
    for (int i = 0; i < NUM_INDICATORS; i++) {
        String key = String(i + 1);
        JsonObject indObj = doc["indicators"][key];
        if (indObj.isNull()) continue;

        const char* modeStr = indObj["mode"] | "off";
        IndicatorMode mode = INDICATOR_OFF;
        if (strcmp(modeStr, "solid") == 0) mode = INDICATOR_SOLID;
        else if (strcmp(modeStr, "blink") == 0) mode = INDICATOR_BLINK;
        else if (strcmp(modeStr, "fade") == 0) mode = INDICATOR_FADE;

        // Backward compatibility: old format had "enabled" boolean
        if (mode == INDICATOR_OFF && indObj["mode"].isNull() && indObj["enabled"].as<bool>()) {
            mode = INDICATOR_SOLID;
        }

        uint32_t color = indicators[i].color;  // Keep default if not provided
        JsonArray colorArr = indObj["color"];
        if (colorArr.size() == 3) {
            color = ((uint32_t)colorArr[0].as<uint8_t>() << 16) |
                    ((uint32_t)colorArr[1].as<uint8_t>() << 8) |
                    (uint32_t)colorArr[2].as<uint8_t>();
        }

        uint16_t blinkInterval = indObj["blinkInterval"] | (uint16_t)INDICATOR_BLINK_INTERVAL;
        uint16_t fadePeriod = indObj["fadePeriod"] | (uint16_t)INDICATOR_FADE_PERIOD;

        indicatorSet(i, mode, color, blinkInterval, fadePeriod);
    }

    Serial.println("[SETTINGS] Configuration loaded successfully");
    Serial.printf("[SETTINGS] Brightness: %d, AutoRotate: %s\n",
                  settings.brightness, settings.autoRotate ? "true" : "false");

    return true;
}

bool saveSettings() {
    if (!filesystemReady) {
        Serial.println("[SETTINGS] Filesystem not ready");
        return false;
    }

    JsonDocument doc;

    // Display settings
    doc["display"]["brightness"] = settings.brightness;
    doc["display"]["autoRotate"] = settings.autoRotate;
    doc["display"]["defaultDuration"] = settings.defaultDuration;
    doc["display"]["colorDepth"] = COLOR_DEPTH;
    doc["display"]["transition"] = "none";

    // WiFi settings
    doc["wifi"]["hostname"] = MDNS_NAME;

    // NTP settings
    doc["ntp"]["server"] = settings.ntpServer;
    doc["ntp"]["offset"] = settings.ntpOffset;
    doc["ntp"]["daylightOffset"] = 3600;

    // Clock app settings
    doc["apps"]["clock"]["enabled"] = settings.clockEnabled;
    doc["apps"]["clock"]["format24h"] = settings.clockFormat24h;
    doc["apps"]["clock"]["showSeconds"] = settings.clockShowSeconds;
    char clockColorHex[8];
    formatColorHex(settings.clockColor, clockColorHex, sizeof(clockColorHex));
    doc["apps"]["clock"]["color"] = clockColorHex;

    // Date app settings
    doc["apps"]["date"]["enabled"] = settings.dateEnabled;
    doc["apps"]["date"]["format"] = settings.dateFormat;
    char dateColorHex[8];
    formatColorHex(settings.dateColor, dateColorHex, sizeof(dateColorHex));
    doc["apps"]["date"]["color"] = dateColorHex;

    // MQTT settings
    doc["mqtt"]["enabled"] = settings.mqttEnabled;
    doc["mqtt"]["server"] = settings.mqttServer;
    doc["mqtt"]["port"] = settings.mqttPort;
    doc["mqtt"]["user"] = settings.mqttUser;
    doc["mqtt"]["password"] = settings.mqttPassword;
    doc["mqtt"]["prefix"] = settings.mqttPrefix;

    // Indicators
    for (int i = 0; i < NUM_INDICATORS; i++) {
        String key = String(i + 1);
        const char* modeStr = "off";
        switch (indicators[i].mode) {
            case INDICATOR_SOLID: modeStr = "solid"; break;
            case INDICATOR_BLINK: modeStr = "blink"; break;
            case INDICATOR_FADE:  modeStr = "fade";  break;
            default: break;
        }
        doc["indicators"][key]["mode"] = modeStr;
        char indicatorColorHex[8];
        formatColorHex(indicators[i].color, indicatorColorHex, sizeof(indicatorColorHex));
        doc["indicators"][key]["color"] = indicatorColorHex;
        doc["indicators"][key]["blinkInterval"] = indicators[i].blinkInterval;
        doc["indicators"][key]["fadePeriod"] = indicators[i].fadePeriod;
    }

    File file = LittleFS.open(FS_CONFIG_FILE, "w");
    if (!file) {
        Serial.println("[SETTINGS] Failed to open config file for writing");
        return false;
    }

    serializeJsonPretty(doc, file);
    file.close();

    Serial.println("[SETTINGS] Configuration saved successfully");
    return true;
}

bool loadApps() {
    if (!filesystemReady) {
        Serial.println("[APPS] Filesystem not ready, cannot load apps");
        return false;
    }

    File file = LittleFS.open(FS_APPS_FILE, "r");
    if (!file) {
        Serial.println("[APPS] Apps file not found, starting fresh");
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.printf("[APPS] JSON parse error: %s\n", error.c_str());
        return false;
    }

    int loadedCount = 0;
    JsonArray appsArray = doc["apps"];
    for (JsonObject appObj : appsArray) {
        const char* id = appObj["id"] | "";
        const char* icon = appObj["icon"] | "";
        uint32_t textColor = appObj["textColor"] | 0xFFFFFF;
        uint16_t duration = appObj["duration"] | settings.defaultDuration;
        uint32_t lifetime = appObj["lifetime"] | 0;
        int8_t priority = appObj["priority"] | 0;

        // Parse text field (handles string and [{t,c},...] array from persistence)
        char parsedText[64] = "";
        TextSegment textSegs[MAX_TEXT_SEGMENTS];
        uint8_t textSegCount = 0;
        parseTextFieldWithSegments(appObj["text"], parsedText, sizeof(parsedText),
                                   textSegs, &textSegCount, textColor);

        if (strlen(id) > 0) {
            int8_t result = appAdd(id, parsedText, icon, textColor,
                                   duration, lifetime, priority, false);
            if (result >= 0) {
                // Restore text segments
                memcpy(apps[result].textSegments, textSegs, sizeof(textSegs));
                apps[result].textSegmentCount = textSegCount;
                // Restore label with segments
                parseTextFieldWithSegments(appObj["label"], apps[result].label,
                                           sizeof(apps[result].label),
                                           apps[result].labelSegments,
                                           &apps[result].labelSegmentCount, textColor);
                // Restore multi-zone data if present
                JsonArray zonesArr = appObj["zones"].as<JsonArray>();
                if (!zonesArr.isNull() && zonesArr.size() >= 2) {
                    appSetZones(result, zonesArr);
                }
                loadedCount++;
            }
        }
    }

    Serial.printf("[APPS] Loaded %d custom apps from storage\n", loadedCount);
    return loadedCount > 0;
}

bool saveApps() {
    if (!filesystemReady) {
        Serial.println("[APPS] Filesystem not ready, cannot save apps");
        return false;
    }

    JsonDocument doc;
    doc["version"] = 1;
    JsonArray appsArray = doc["apps"].to<JsonArray>();

    int savedCount = 0;
    for (uint8_t i = 0; i < MAX_APPS; i++) {
        if (apps[i].active && !apps[i].isSystem) {
            JsonObject appObj = appsArray.add<JsonObject>();
            appObj["id"] = apps[i].id;
            appObj["icon"] = apps[i].icon;
            appObj["textColor"] = apps[i].textColor;
            appObj["duration"] = apps[i].duration;
            appObj["lifetime"] = apps[i].lifetime;
            appObj["priority"] = apps[i].priority;
            // Serialize text and label in polymorphic format
            serializeTextField(appObj, "text", apps[i].text,
                               apps[i].textSegments, apps[i].textSegmentCount);
            if (apps[i].label[0] != '\0') {
                serializeTextField(appObj, "label", apps[i].label,
                                   apps[i].labelSegments, apps[i].labelSegmentCount);
            }

            // Serialize multi-zone data
            if (apps[i].zoneCount >= 2) {
                appObj["zoneCount"] = apps[i].zoneCount;
                JsonArray zonesArr = appObj["zones"].to<JsonArray>();
                // Zone 0 = main app fields (text/icon/textColor/label)
                JsonObject z0 = zonesArr.add<JsonObject>();
                serializeTextField(z0, "text", apps[i].text,
                                   apps[i].textSegments, apps[i].textSegmentCount);
                z0["icon"] = apps[i].icon;
                if (apps[i].label[0] != '\0') {
                    serializeTextField(z0, "label", apps[i].label,
                                       apps[i].labelSegments, apps[i].labelSegmentCount);
                }
                char z0ColorHex[8];
                formatColorHex(apps[i].textColor, z0ColorHex, sizeof(z0ColorHex));
                z0["color"] = z0ColorHex;
                // Zones 1..N stored in app->zones[0..N-1]
                for (uint8_t z = 1; z < apps[i].zoneCount; z++) {
                    JsonObject zObj = zonesArr.add<JsonObject>();
                    serializeTextField(zObj, "text", apps[i].zones[z - 1].text,
                                       apps[i].zones[z - 1].textSegments,
                                       apps[i].zones[z - 1].textSegmentCount);
                    zObj["icon"] = apps[i].zones[z - 1].icon;
                    if (apps[i].zones[z - 1].label[0] != '\0') {
                        serializeTextField(zObj, "label", apps[i].zones[z - 1].label,
                                           apps[i].zones[z - 1].labelSegments,
                                           apps[i].zones[z - 1].labelSegmentCount);
                    }
                    char zColorHex[8];
                    formatColorHex(apps[i].zones[z - 1].textColor, zColorHex, sizeof(zColorHex));
                    zObj["color"] = zColorHex;
                }
            }

            savedCount++;
        }
    }

    File file = LittleFS.open(FS_APPS_FILE, "w");
    if (!file) {
        Serial.println("[APPS] Failed to open apps file for writing");
        return false;
    }

    serializeJsonPretty(doc, file);
    file.close();

    Serial.printf("[APPS] Saved %d custom apps to storage\n", savedCount);
    return true;
}

// ============================================================================
// Application Manager Functions
// ============================================================================

void setupApps() {
    // Initialize app array
    memset(apps, 0, sizeof(apps));
    appCount = 0;
    currentAppIndex = -1;

    // Add system apps
    // NOTE: clock and date disabled while weatherclock is in development
    // if (settings.clockEnabled) {
    //     appAdd("clock", "Clock", "", settings.clockColor,
    //            settings.defaultDuration, 0, 0, true);
    //     Serial.println("[APPS] Clock app added");
    // }
    //
    // if (settings.dateEnabled) {
    //     appAdd("date", "Date", "", settings.dateColor,
    //            settings.defaultDuration, 0, 0, true);
    //     Serial.println("[APPS] Date app added");
    // }

    // WeatherClock system app (replaces clock+date when weather data is available)
    appAdd("weatherclock", "WeatherClock", "", settings.clockColor,
           settings.defaultDuration, 0, 1, true);
    Serial.println("[APPS] WeatherClock app added");

    // Load persisted custom apps
    // NOTE: disabled during weatherclock development to avoid old "weather" app interference
    // loadApps();

    Serial.printf("[APPS] Initialized with %d apps\n", appCount);
    appRotationEnabled = settings.autoRotate;
}

int8_t appAdd(const char* id, const char* text, const char* icon,
              uint32_t textColor, uint16_t duration,
              uint32_t lifetime, int8_t priority, bool isSystem) {

    // Check if app with same ID exists
    int8_t existingIndex = appFind(id);
    if (existingIndex >= 0) {
        // Update existing app
        AppItem* app = &apps[existingIndex];
        strlcpy(app->text, text, sizeof(app->text));
        if (icon) strlcpy(app->icon, icon, sizeof(app->icon));
        app->label[0] = '\0';  // Reset label (caller will set if needed)
        app->textColor = textColor;
        app->textSegmentCount = 0;
        app->labelSegmentCount = 0;
        app->duration = duration;
        app->lifetime = lifetime;
        app->priority = priority;
        app->createdAt = millis();
        app->active = true;
        // Reset zone data (caller will set via appSetZones if needed)
        app->zoneCount = 0;
        memset(app->zones, 0, sizeof(app->zones));
        Serial.printf("[APPS] Updated app: %s\n", id);
        // Persist non-system apps
        if (!app->isSystem) {
            saveApps();
        }
        return existingIndex;
    }

    // Find empty slot
    int8_t emptySlot = -1;
    for (uint8_t i = 0; i < MAX_APPS; i++) {
        if (!apps[i].active) {
            emptySlot = i;
            break;
        }
    }

    if (emptySlot < 0) {
        Serial.println("[APPS] No empty slot available");
        return -1;
    }

    // Create new app
    AppItem* app = &apps[emptySlot];
    strlcpy(app->id, id, sizeof(app->id));
    strlcpy(app->text, text, sizeof(app->text));
    if (icon) strlcpy(app->icon, icon, sizeof(app->icon));
    else app->icon[0] = '\0';
    app->label[0] = '\0';  // Initialize label (caller will set if needed)
    app->textColor = textColor;
    app->textSegmentCount = 0;
    app->labelSegmentCount = 0;
    app->duration = duration > 0 ? duration : settings.defaultDuration;
    app->lifetime = lifetime;
    app->createdAt = millis();
    app->priority = constrain(priority, -10, 10);
    app->active = true;
    app->isSystem = isSystem;
    // Initialize zone data (caller will set via appSetZones if needed)
    app->zoneCount = 0;
    memset(app->zones, 0, sizeof(app->zones));

    appCount++;
    Serial.printf("[APPS] Added app: %s (slot %d, total %d)\n", id, emptySlot, appCount);

    // Persist non-system apps
    if (!isSystem) {
        saveApps();
    }

    return emptySlot;
}

void appSetZones(int8_t appIndex, JsonArray zonesArray) {
    if (appIndex < 0 || appIndex >= MAX_APPS) return;

    AppItem* app = &apps[appIndex];
    uint8_t count = zonesArray.size();
    if (count < 2 || count > MAX_ZONES) return;

    app->zoneCount = count;

    // Zone 0 maps to the app's main text/icon/textColor/label fields
    JsonObject zone0 = zonesArray[0].as<JsonObject>();
    strlcpy(app->icon, zone0["icon"] | "", sizeof(app->icon));
    app->textColor = parseColorValue(zone0["color"], 0xFFFFFF);
    parseTextFieldWithSegments(zone0["text"], app->text, sizeof(app->text),
                               app->textSegments, &app->textSegmentCount, app->textColor);
    parseTextFieldWithSegments(zone0["label"], app->label, sizeof(app->label),
                               app->labelSegments, &app->labelSegmentCount, app->textColor);

    // Zones 1-3 map to app->zones[0..2]
    for (uint8_t i = 1; i < count && i < MAX_ZONES; i++) {
        JsonObject zoneObj = zonesArray[i].as<JsonObject>();
        strlcpy(app->zones[i - 1].icon, zoneObj["icon"] | "", sizeof(app->zones[0].icon));
        app->zones[i - 1].textColor = parseColorValue(zoneObj["color"], 0xFFFFFF);
        parseTextFieldWithSegments(zoneObj["text"], app->zones[i - 1].text,
                                   sizeof(app->zones[0].text),
                                   app->zones[i - 1].textSegments,
                                   &app->zones[i - 1].textSegmentCount,
                                   app->zones[i - 1].textColor);
        parseTextFieldWithSegments(zoneObj["label"], app->zones[i - 1].label,
                                   sizeof(app->zones[0].label),
                                   app->zones[i - 1].labelSegments,
                                   &app->zones[i - 1].labelSegmentCount,
                                   app->zones[i - 1].textColor);
    }

    Serial.printf("[APPS] Set %d zones for app: %s\n", count, app->id);

    // Persist non-system apps
    if (!app->isSystem) {
        saveApps();
    }
}

bool appRemove(const char* id) {
    int8_t index = appFind(id);
    if (index < 0) return false;

    AppItem* app = &apps[index];
    if (app->isSystem) {
        Serial.printf("[APPS] Cannot remove system app: %s\n", id);
        return false;
    }

    app->active = false;
    appCount--;

    // If removing current app, move to next
    if (currentAppIndex == index) {
        currentAppIndex = -1;
    }

    Serial.printf("[APPS] Removed app: %s\n", id);

    // Persist the removal
    saveApps();

    return true;
}

bool appUpdate(const char* id, const char* text, const char* icon,
               uint32_t textColor) {
    int8_t index = appFind(id);
    if (index < 0) return false;

    AppItem* app = &apps[index];
    if (text) strlcpy(app->text, text, sizeof(app->text));
    if (icon) strlcpy(app->icon, icon, sizeof(app->icon));
    if (textColor != 0) app->textColor = textColor;
    app->createdAt = millis();

    Serial.printf("[APPS] Updated app: %s\n", id);
    return true;
}

int8_t appFind(const char* id) {
    for (uint8_t i = 0; i < MAX_APPS; i++) {
        if (apps[i].active && strcmp(apps[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

void appCleanExpired() {
    unsigned long now = millis();
    for (uint8_t i = 0; i < MAX_APPS; i++) {
        if (apps[i].active && apps[i].lifetime > 0) {
            if (now - apps[i].createdAt > apps[i].lifetime) {
                Serial.printf("[APPS] App expired: %s\n", apps[i].id);
                apps[i].active = false;
                appCount--;
                if (currentAppIndex == i) {
                    currentAppIndex = -1;
                }
            }
        }
    }
}

AppItem* appGetNext() {
    if (appCount == 0) return nullptr;

    // Clean expired apps first
    appCleanExpired();
    if (appCount == 0) return nullptr;

    // Simple round-robin: find next active app after current
    int8_t startIndex = (currentAppIndex + 1) % MAX_APPS;

    for (uint8_t i = 0; i < MAX_APPS; i++) {
        int8_t idx = (startIndex + i) % MAX_APPS;
        if (apps[idx].active) {
            currentAppIndex = idx;
            return &apps[idx];
        }
    }

    return nullptr;
}

AppItem* appGetCurrent() {
    if (currentAppIndex >= 0 && currentAppIndex < MAX_APPS && apps[currentAppIndex].active) {
        return &apps[currentAppIndex];
    }
    return nullptr;
}

void loopApps() {
    if (!wifiConnected) return;

    // ---- Notification priority check (before app rotation) ----
    unsigned long now = millis();

    NotificationItem* currentNotif = notifGetCurrent();

    // Check if current notification has expired
    if (currentNotif && notifIsExpired(currentNotif)) {
        notifDismiss();
        resetNotifScrollState();
        currentNotif = notifGetNext();  // Try next in queue
        if (currentNotif) {
            resetNotifScrollState();
            // Draw twice to flush both DMA buffers
            displayShowNotification(currentNotif);
            displayShowNotification(currentNotif);
            lastDisplayUpdate = now;
        }
    }

    // If no current notification, check if new ones are queued
    if (!notifGetCurrent()) {
        currentNotif = notifGetNext();
        if (currentNotif) {
            // Save current app index to restore later (first time only)
            if (savedAppIndex < 0) {
                savedAppIndex = currentAppIndex;
            }
            resetNotifScrollState();
            // Draw twice to flush both DMA buffers (prevents weather ghosting)
            displayShowNotification(currentNotif);
            displayShowNotification(currentNotif);
            lastDisplayUpdate = now;
        }
    }

    // If a notification is active, skip app rotation
    if (notifGetCurrent()) {
        return;
    }

    // Just finished all notifications: restore app rotation
    if (savedAppIndex >= 0) {
        currentAppIndex = savedAppIndex;
        savedAppIndex = -1;
        lastAppSwitch = now;
        resetScrollState();
        // Reset weather clock cache to force full redraw (not just seconds update)
        weatherLastDrawnMinute = -1;
        weatherLastUpdateDrawn = 0;
        Serial.println("[NOTIF] All dismissed, resuming app rotation");
        // Clear both DMA buffers to remove any notification pixel remnants,
        // then force immediate app redraw on both buffers
        dma_display->clearScreen();
        #if DOUBLE_BUFFER
            dma_display->flipDMABuffer();
        #endif
        dma_display->clearScreen();
        #if DOUBLE_BUFFER
            dma_display->flipDMABuffer();
        #endif
        AppItem* restored = appGetCurrent();
        if (restored) {
            displayShowApp(restored);
            displayShowApp(restored);
            lastDisplayUpdate = now;
        }
    }

    // ---- Normal app rotation ----
    if (appCount == 0) return;

    AppItem* current = appGetCurrent();

    if (current == nullptr) {
        // No current app, get first one
        current = appGetNext();
        if (current) {
            lastAppSwitch = now;
            resetScrollState();
            // Force immediate redraw
            displayShowApp(current);
            lastDisplayUpdate = now;
        }
        return;
    }

    // Check if current app duration has elapsed
    if (appRotationEnabled && (now - lastAppSwitch > current->duration)) {
        current = appGetNext();
        if (current) {
            lastAppSwitch = now;
            resetScrollState();
            Serial.printf("[APPS] Switched to: %s\n", current->id);
            // Force immediate redraw on app switch
            displayShowApp(current);
            lastDisplayUpdate = now;
        }
    }
}

// ============================================================================
// Time Functions
// ============================================================================

void loopTime() {
    if (wifiConnected) {
        timeClient.update();
    }
}

// ============================================================================
// Display Loop
// ============================================================================

void loopDisplay() {
    if (!wifiConnected) return;

    unsigned long now = millis();
    bool needsRedraw = false;

    // ---- Notification display (priority over apps) ----
    NotificationItem* currentNotif = notifGetCurrent();
    if (currentNotif) {
        // Handle notification scroll animation
        if (notifScrollState.needsScroll) {
            if (now - lastNotifScrollUpdate >= SCROLL_SPEED) {
                lastNotifScrollUpdate = now;

                switch (notifScrollState.scrollPhase) {
                    case 0:  // pause_start
                        if (now - notifScrollState.lastScrollTime >= SCROLL_PAUSE) {
                            notifScrollState.scrollPhase = 1;
                            notifScrollState.lastScrollTime = now;
                        }
                        break;

                    case 1:  // scrolling
                        notifScrollState.scrollOffset++;
                        if (notifScrollState.scrollOffset >= notifScrollState.textWidth - notifScrollState.availableWidth + 10) {
                            notifScrollState.scrollPhase = 2;
                            notifScrollState.lastScrollTime = now;
                        }
                        needsRedraw = true;
                        break;

                    case 2:  // pause_end
                        if (now - notifScrollState.lastScrollTime >= SCROLL_PAUSE) {
                            notifScrollState.scrollOffset = 0;
                            notifScrollState.scrollPhase = 0;
                            notifScrollState.lastScrollTime = now;
                        }
                        break;
                }
            }
        }

        // Redraw notification on scroll, periodic update, or indicator animation
        bool indicatorRedraw = indicatorNeedsRedraw() && (now - lastDisplayUpdate > 50);
        if (now - lastDisplayUpdate > 1000 || needsRedraw || indicatorRedraw) {
            displayShowNotification(currentNotif);
            lastDisplayUpdate = now;
        }
        return;  // Skip app display while notification is active
    }

    // ---- Normal app display ----
    AppItem* current = appGetCurrent();
    needsRedraw = false;

    // Handle scroll animation (50ms updates for smooth scrolling)
    if (current && appScrollState.needsScroll) {
        if (now - lastScrollUpdate >= SCROLL_SPEED) {
            lastScrollUpdate = now;

            switch (appScrollState.scrollPhase) {
                case 0:  // pause_start
                    if (now - appScrollState.lastScrollTime >= SCROLL_PAUSE) {
                        appScrollState.scrollPhase = 1;
                        appScrollState.lastScrollTime = now;
                    }
                    break;

                case 1:  // scrolling
                    appScrollState.scrollOffset++;
                    // Check if text has scrolled completely
                    if (appScrollState.scrollOffset >= appScrollState.textWidth - appScrollState.availableWidth + 10) {
                        appScrollState.scrollPhase = 2;
                        appScrollState.lastScrollTime = now;
                    }
                    needsRedraw = true;
                    break;

                case 2:  // pause_end
                    if (now - appScrollState.lastScrollTime >= SCROLL_PAUSE) {
                        // Reset to beginning
                        appScrollState.scrollOffset = 0;
                        appScrollState.scrollPhase = 0;
                        appScrollState.lastScrollTime = now;
                    }
                    break;
            }
        }
    }

    // Regular display update (1000ms for non-scrolling, 50ms for indicator animation)
    bool indicatorRedraw = indicatorNeedsRedraw() && (now - lastDisplayUpdate > 50);
    if (now - lastDisplayUpdate > 1000 || needsRedraw || indicatorRedraw) {
        if (current) {
            displayShowApp(current);
        } else {
            // Fallback: show time if no apps
            displayShowTime();
        }
        lastDisplayUpdate = now;
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

void logMemory() {
    Serial.printf("[MEM] Free heap: %d bytes, largest block: %d bytes\n",
        ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}
