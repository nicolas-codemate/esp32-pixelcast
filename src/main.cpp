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

struct AppItem {
    char id[24];
    char text[64];
    char icon[32];
    uint32_t textColor;
    uint32_t backgroundColor;
    uint16_t duration;          // Display duration in ms
    uint32_t lifetime;          // Expiration time (0 = permanent)
    uint32_t createdAt;         // Creation timestamp
    int8_t priority;            // -10 to 10 (higher = more important)
    bool active;
    bool isSystem;              // System apps cannot be deleted
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
struct WeatherData {
    char currentIcon[32];
    int16_t currentTemp;
    uint8_t currentHumidity;
    struct ForecastDay {
        char icon[32];
        int16_t tempMin;
        int16_t tempMax;
        char dayName[4];  // "LUN", "MAR", etc.
    } forecast[2];
    unsigned long lastUpdate;
    bool valid;
};
WeatherData weatherData;

// Timing
unsigned long lastStatsPublish = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastTimeUpdate = 0;
unsigned long lastScrollUpdate = 0;

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
void displayShowWeatherClock();
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
              uint32_t textColor, uint32_t bgColor,
              uint16_t duration, uint32_t lifetime, int8_t priority, bool isSystem);
bool appRemove(const char* id);
bool appUpdate(const char* id, const char* text, const char* icon,
               uint32_t textColor, uint32_t bgColor);
int8_t appFind(const char* id);
void appCleanExpired();
AppItem* appGetNext();
AppItem* appGetCurrent();

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
    dma_display->setCursor(11, 20);
    dma_display->print("WiFi OK");

    // IP address in compact font (TomThumb 3x5px), centered
    String ip = WiFi.localIP().toString();
    dma_display->setFont(&TomThumb);
    dma_display->setTextColor(dma_display->color565(255, 255, 255));
    int16_t ipWidth = ip.length() * 4;  // TomThumb: ~4px per char
    int16_t ipX = (DISPLAY_WIDTH - ipWidth) / 2;
    dma_display->setCursor(ipX, 38);
    dma_display->print(ip);

    // Reset to default font
    dma_display->setFont(NULL);

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

void displayShowWeatherClock() {
    // Fallback to time display if weather data is stale or missing
    unsigned long weatherAge = millis() - weatherData.lastUpdate;
    if (!weatherData.valid || weatherAge > 3600000) {
        displayShowTime();
        return;
    }

    dma_display->clearScreen();

    uint16_t white = dma_display->color565(255, 255, 255);
    uint16_t dimGray = dma_display->color565(40, 40, 40);
    uint16_t cyan = dma_display->color565(0, 180, 255);
    uint16_t paleBlue = dma_display->color565(100, 160, 255);
    uint16_t gray = dma_display->color565(140, 140, 140);
    uint16_t amber = dma_display->color565(255, 180, 50);
    uint16_t coldBlue = dma_display->color565(80, 140, 255);
    uint16_t orange = dma_display->color565(255, 130, 0);

    // ============================================================
    // Layout map (64x64 display)
    // NULL font: setCursor = top-left of glyph, char is 7px tall
    // TomThumb: setCursor = baseline, uppercase chars 5px above baseline
    // ============================================================
    // y=1-8:    current weather (icon 8x8 + temp + humidity)
    // y=11:     separator
    // y=16-22:  HH:MM (NULL font top=16) + :SS (TomThumb baseline=23)
    // y=27-33:  date (NULL font top=27)
    // y=36:     separator
    // y=44:     day names (TomThumb baseline=44, glyphs y=39-43)
    // y=47-54:  forecast icons (8x8)
    // y=62:     temps (TomThumb baseline=62, glyphs y=57-61)
    // ============================================================

    // ---- Current weather (y=1-8) ----
    CachedIcon* currentIcon = getIcon(weatherData.currentIcon);
    int16_t weatherTextX = 2;
    if (currentIcon && currentIcon->valid) {
        drawIconAtScale(currentIcon, 1, 1, 1);  // Icon at (1,1), native 8x8
        weatherTextX = 11;  // After 8px icon + 2px gap
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

    // Drop icon + humidity on right side
    int16_t humidityX = DISPLAY_WIDTH - 18;
    drawDropIcon(humidityX, 2, cyan);

    // Humidity (TomThumb, baseline=8 to align bottom with temp bottom y=8)
    dma_display->setFont(&TomThumb);
    dma_display->setTextColor(cyan);
    char humStr[6];
    snprintf(humStr, sizeof(humStr), "%d%%", weatherData.currentHumidity);
    dma_display->setCursor(humidityX + 5, 8);
    dma_display->print(humStr);

    // ---- Separator (y=11) ----
    drawSeparatorLine(11, dimGray);

    // ---- Clock (y=16-22) ----
    int hours = timeClient.getHours();
    int minutes = timeClient.getMinutes();
    int seconds = timeClient.getSeconds();

    if (!settings.clockFormat24h && hours > 12) {
        hours -= 12;
    }

    dma_display->setTextColor(paleBlue);

    // HH:MM in NULL font (5 chars * 6px = 30px)
    char hmStr[6];
    snprintf(hmStr, sizeof(hmStr), "%02d:%02d", hours, minutes);
    dma_display->setFont(NULL);
    dma_display->setTextSize(1);

    int16_t hmX = (DISPLAY_WIDTH - 30) / 2 - 6;  // Shift left for seconds
    dma_display->setCursor(hmX, 16);
    dma_display->print(hmStr);

    // Seconds in TomThumb (baseline=23, bottom-aligned with NULL font y=16+6=22)
    dma_display->setFont(&TomThumb);
    char secStr[4];
    snprintf(secStr, sizeof(secStr), ":%02d", seconds);
    dma_display->setCursor(hmX + 31, 23);
    dma_display->print(secStr);

    // ---- Date (y=27-33) ----
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
    dma_display->setCursor(dateX, 27);
    dma_display->print(dateStr);

    // ---- Separator (y=36) ----
    drawSeparatorLine(36, dimGray);

    // ---- Forecast (y=39-61) ----
    // Two columns: left center x=16, right center x=48
    for (int i = 0; i < 2; i++) {
        int16_t colCenter = (i == 0) ? 16 : 48;

        // Day name (TomThumb, baseline=44, glyphs y=39-43)
        dma_display->setFont(&TomThumb);
        dma_display->setTextColor(amber);
        int16_t dayNameWidth = strlen(weatherData.forecast[i].dayName) * 4;
        dma_display->setCursor(colCenter - dayNameWidth / 2, 44);
        dma_display->print(weatherData.forecast[i].dayName);

        // Forecast icon (8x8 native, y=47-54)
        CachedIcon* forecastIcon = getIcon(weatherData.forecast[i].icon);
        if (forecastIcon && forecastIcon->valid) {
            drawIconAtScale(forecastIcon, colCenter - 4, 47, 1);
        }

        // Min temp in blue (TomThumb, baseline=62, glyphs y=57-61)
        char minStr[8];
        snprintf(minStr, sizeof(minStr), "%d", weatherData.forecast[i].tempMin);
        dma_display->setFont(&TomThumb);
        dma_display->setTextColor(coldBlue);
        int16_t minWidth = strlen(minStr) * 4;
        dma_display->setCursor(colCenter - minWidth - 2, 62);
        dma_display->print(minStr);

        // Slash
        dma_display->setTextColor(gray);
        dma_display->setCursor(colCenter - 2, 62);
        dma_display->print("/");

        // Max temp in orange
        char maxStr[8];
        snprintf(maxStr, sizeof(maxStr), "%d", weatherData.forecast[i].tempMax);
        dma_display->setTextColor(orange);
        dma_display->setCursor(colCenter + 2, 62);
        dma_display->print(maxStr);
    }

    // Reset font
    dma_display->setFont(NULL);

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

void displayShowApp(AppItem* app) {
    if (!app) return;

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
        displayShowWeatherClock();
        return;
    }

    // Custom apps
    dma_display->clearScreen();

    // Background color
    if (app->backgroundColor != 0) {
        uint8_t br = (app->backgroundColor >> 16) & 0xFF;
        uint8_t bg = (app->backgroundColor >> 8) & 0xFF;
        uint8_t bb = app->backgroundColor & 0xFF;
        dma_display->fillScreen(dma_display->color565(br, bg, bb));
    }

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

    // Text color
    uint8_t r = (app->textColor >> 16) & 0xFF;
    uint8_t g = (app->textColor >> 8) & 0xFF;
    uint8_t b = app->textColor & 0xFF;
    dma_display->setTextColor(dma_display->color565(r, g, b));
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

    // Draw text with special character handling
    printTextWithSpecialChars(app->text, xPos, textYPos);

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

CachedIcon* getIcon(const char* name) {
    if (!name || strlen(name) == 0) return nullptr;

    // Search cache first
    for (uint8_t i = 0; i < MAX_ICON_CACHE; i++) {
        if (iconCache[i].valid && strcmp(iconCache[i].name, name) == 0) {
            iconCache[i].lastUsed = millis();
            return &iconCache[i];
        }
    }

    // Not in cache, load it
    return loadIcon(name);
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

            const char* text = doc["text"] | "";
            const char* icon = doc["icon"] | "";

            // Parse color (can be hex string or RGB array)
            uint32_t textColor = 0xFFFFFF;
            if (!doc["color"].isNull()) {
                if (doc["color"].is<JsonArray>()) {
                    JsonArray arr = doc["color"];
                    if (arr.size() == 3) {
                        textColor = ((uint32_t)arr[0].as<uint8_t>() << 16) |
                                    ((uint32_t)arr[1].as<uint8_t>() << 8) |
                                    (uint32_t)arr[2].as<uint8_t>();
                    }
                } else if (doc["color"].is<const char*>()) {
                    textColor = strtoul(doc["color"].as<const char*>() + 1, NULL, 16);
                } else {
                    textColor = doc["color"].as<uint32_t>();
                }
            }

            uint32_t bgColor = 0;
            if (!doc["background"].isNull()) {
                if (doc["background"].is<JsonArray>()) {
                    JsonArray arr = doc["background"];
                    if (arr.size() == 3) {
                        bgColor = ((uint32_t)arr[0].as<uint8_t>() << 16) |
                                  ((uint32_t)arr[1].as<uint8_t>() << 8) |
                                  (uint32_t)arr[2].as<uint8_t>();
                    }
                } else {
                    bgColor = doc["background"].as<uint32_t>();
                }
            }

            uint16_t duration = doc["duration"] | settings.defaultDuration;
            uint32_t lifetime = doc["lifetime"] | 0;
            int8_t priority = doc["priority"] | 0;

            int8_t result = appAdd(name.c_str(), text, icon, textColor, bgColor,
                                   duration, lifetime, priority, false);

            if (result >= 0) {
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
            current["humidity"] = weatherData.currentHumidity;

            JsonArray forecastArr = doc["forecast"].to<JsonArray>();
            for (int i = 0; i < 2; i++) {
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
                weatherData.currentHumidity = current["humidity"] | 0;
            } else {
                request->send(400, "application/json", "{\"error\":\"Missing 'current' object\"}");
                return;
            }

            // Parse forecast (optional, up to 2 days)
            if (doc["forecast"].is<JsonArray>()) {
                JsonArray forecastArr = doc["forecast"];
                for (int i = 0; i < 2 && i < (int)forecastArr.size(); i++) {
                    JsonObject fc = forecastArr[i];
                    strlcpy(weatherData.forecast[i].icon, fc["icon"] | "", sizeof(weatherData.forecast[i].icon));
                    weatherData.forecast[i].tempMin = fc["temp_min"] | 0;
                    weatherData.forecast[i].tempMax = fc["temp_max"] | 0;
                    strlcpy(weatherData.forecast[i].dayName, fc["day"] | "", sizeof(weatherData.forecast[i].dayName));
                }
            }

            weatherData.lastUpdate = millis();
            weatherData.valid = true;

            Serial.printf("[WEATHER] Updated: %d C, %d%% humidity\n",
                         weatherData.currentTemp, weatherData.currentHumidity);
            request->send(200, "application/json", "{\"success\":true}");
        });
    webServer.addHandler(weatherHandler);

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

        // Handle DELETE /api/icons?name={name} (fallback if static handler misses)
        // Use explicit value 0b00000100 (4) for HTTP_DELETE due to enum conflicts with WebServer library
        const WebRequestMethodComposite HTTP_DELETE_METHOD = 0b00000100;
        if (method == HTTP_DELETE_METHOD && url == "/api/icons") {
            handleApiIconsDelete(request);
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
            appObj["text"] = apps[i].text;
            appObj["icon"] = apps[i].icon;
            appObj["duration"] = apps[i].duration;
            appObj["lifetime"] = apps[i].lifetime;
            appObj["priority"] = apps[i].priority;
            appObj["isSystem"] = apps[i].isSystem;
            appObj["isCurrent"] = (currentAppIndex == i);

            // Color as RGB array
            JsonArray color = appObj["color"].to<JsonArray>();
            color.add((apps[i].textColor >> 16) & 0xFF);
            color.add((apps[i].textColor >> 8) & 0xFF);
            color.add(apps[i].textColor & 0xFF);
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
    JsonArray clockColor = doc["apps"]["clock"]["color"].to<JsonArray>();
    clockColor.add((settings.clockColor >> 16) & 0xFF);
    clockColor.add((settings.clockColor >> 8) & 0xFF);
    clockColor.add(settings.clockColor & 0xFF);

    // Date app settings
    doc["apps"]["date"]["enabled"] = settings.dateEnabled;
    doc["apps"]["date"]["format"] = settings.dateFormat;
    JsonArray dateColor = doc["apps"]["date"]["color"].to<JsonArray>();
    dateColor.add((settings.dateColor >> 16) & 0xFF);
    dateColor.add((settings.dateColor >> 8) & 0xFF);
    dateColor.add(settings.dateColor & 0xFF);

    // MQTT settings
    doc["mqtt"]["enabled"] = settings.mqttEnabled;
    doc["mqtt"]["server"] = settings.mqttServer;
    doc["mqtt"]["port"] = settings.mqttPort;
    doc["mqtt"]["user"] = settings.mqttUser;
    doc["mqtt"]["password"] = settings.mqttPassword;
    doc["mqtt"]["prefix"] = settings.mqttPrefix;

    // Indicators (default values)
    for (int i = 1; i <= 3; i++) {
        String key = String(i);
        doc["indicators"][key]["enabled"] = false;
        JsonArray color = doc["indicators"][key]["color"].to<JsonArray>();
        if (i == 1) { color.add(255); color.add(0); color.add(0); }
        else if (i == 2) { color.add(0); color.add(255); color.add(0); }
        else { color.add(0); color.add(0); color.add(255); }
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
        const char* text = appObj["text"] | "";
        const char* icon = appObj["icon"] | "";
        uint32_t textColor = appObj["textColor"] | 0xFFFFFF;
        uint32_t bgColor = appObj["backgroundColor"] | 0;
        uint16_t duration = appObj["duration"] | settings.defaultDuration;
        uint32_t lifetime = appObj["lifetime"] | 0;
        int8_t priority = appObj["priority"] | 0;

        if (strlen(id) > 0) {
            int8_t result = appAdd(id, text, icon, textColor, bgColor,
                                   duration, lifetime, priority, false);
            if (result >= 0) {
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
            appObj["text"] = apps[i].text;
            appObj["icon"] = apps[i].icon;
            appObj["textColor"] = apps[i].textColor;
            appObj["backgroundColor"] = apps[i].backgroundColor;
            appObj["duration"] = apps[i].duration;
            appObj["lifetime"] = apps[i].lifetime;
            appObj["priority"] = apps[i].priority;
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
    //     appAdd("clock", "Clock", "", settings.clockColor, 0x000000,
    //            settings.defaultDuration, 0, 0, true);
    //     Serial.println("[APPS] Clock app added");
    // }
    //
    // if (settings.dateEnabled) {
    //     appAdd("date", "Date", "", settings.dateColor, 0x000000,
    //            settings.defaultDuration, 0, 0, true);
    //     Serial.println("[APPS] Date app added");
    // }

    // WeatherClock system app (replaces clock+date when weather data is available)
    appAdd("weatherclock", "WeatherClock", "", settings.clockColor, 0x000000,
           settings.defaultDuration, 0, 1, true);
    Serial.println("[APPS] WeatherClock app added");

    // Load persisted custom apps
    // NOTE: disabled during weatherclock development to avoid old "weather" app interference
    // loadApps();

    Serial.printf("[APPS] Initialized with %d apps\n", appCount);
    appRotationEnabled = settings.autoRotate;
}

int8_t appAdd(const char* id, const char* text, const char* icon,
              uint32_t textColor, uint32_t bgColor,
              uint16_t duration, uint32_t lifetime, int8_t priority, bool isSystem) {

    // Check if app with same ID exists
    int8_t existingIndex = appFind(id);
    if (existingIndex >= 0) {
        // Update existing app
        AppItem* app = &apps[existingIndex];
        strlcpy(app->text, text, sizeof(app->text));
        if (icon) strlcpy(app->icon, icon, sizeof(app->icon));
        app->textColor = textColor;
        app->backgroundColor = bgColor;
        app->duration = duration;
        app->lifetime = lifetime;
        app->priority = priority;
        app->createdAt = millis();
        app->active = true;
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
    app->textColor = textColor;
    app->backgroundColor = bgColor;
    app->duration = duration > 0 ? duration : settings.defaultDuration;
    app->lifetime = lifetime;
    app->createdAt = millis();
    app->priority = constrain(priority, -10, 10);
    app->active = true;
    app->isSystem = isSystem;

    appCount++;
    Serial.printf("[APPS] Added app: %s (slot %d, total %d)\n", id, emptySlot, appCount);

    // Persist non-system apps
    if (!isSystem) {
        saveApps();
    }

    return emptySlot;
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
               uint32_t textColor, uint32_t bgColor) {
    int8_t index = appFind(id);
    if (index < 0) return false;

    AppItem* app = &apps[index];
    if (text) strlcpy(app->text, text, sizeof(app->text));
    if (icon) strlcpy(app->icon, icon, sizeof(app->icon));
    if (textColor != 0) app->textColor = textColor;
    if (bgColor != 0xFFFFFFFF) app->backgroundColor = bgColor;
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
    if (appCount == 0) return;

    // Check if it's time to switch apps
    unsigned long now = millis();
    AppItem* current = appGetCurrent();

    if (current == nullptr) {
        // No current app, get first one
        current = appGetNext();
        if (current) {
            lastAppSwitch = now;
            resetScrollState();
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
    AppItem* current = appGetCurrent();
    bool needsRedraw = false;

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

    // Regular display update (1000ms for non-scrolling content like clock)
    if (now - lastDisplayUpdate > 1000 || needsRedraw) {
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
