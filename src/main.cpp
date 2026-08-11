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
#include <time.h>

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

// Static RAM is the scarce resource, so a gauge field that shows six to ten glyphs at a time
// does not reserve room for eight colour changes.
#define MAX_GAUGE_TITLE_SEGMENTS 4
#define MAX_GAUGE_LABEL_SEGMENTS 3

// Packed: aligning the colour behind the offset would waste 3 bytes on each of the 64
// segments an app carries, and static RAM is the scarce resource on this board.
struct __attribute__((packed)) TextSegment {
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

enum StaleBehavior : uint8_t {
    STALE_HIDE = 0,             // Leave the rotation until the client speaks again
    STALE_DIM = 1,              // Stay visible, dimmed, with a STALE badge
    STALE_BADGE = 2,            // Stay visible at full colour, with a STALE badge
    STALE_NONE = 3              // Stay visible, unchanged
};

struct AppItem {
    char id[24];
    char text[64];
    char icon[32];
    char label[32];
    uint32_t textColor;
    uint16_t duration;          // Display duration in ms
    uint32_t staleAfter;        // Silence tolerated before the app is stale, in ms (0 = never)
    StaleBehavior staleBehavior;
    uint32_t lastUpdate;        // Refreshed by every push, so it dates the last client contact
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
    unsigned long lastScrollTime;
    int16_t scrollOffset;
    int16_t textWidth;
    int16_t availableWidth;
    uint8_t scrollPhase;  // 0=pause_start, 1=scrolling, 2=pause_end
    bool needsScroll;
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

struct SleepSlot {
    uint8_t startHour;
    uint8_t startMinute;
    uint8_t endHour;
    uint8_t endMinute;
};

struct SleepDay {
    bool allDay;
    uint8_t slotCount;
    SleepSlot slots[MAX_SLOTS_PER_DAY];
};

struct SleepSchedule {
    bool enabled;
    char displayMode[8];
    SleepDay days[7];
    uint32_t sleepUntilEpoch;
};

enum SleepReason {
    SLEEP_REASON_NONE,
    SLEEP_REASON_SCHEDULE,
    SLEEP_REASON_OVERRIDE,
    SLEEP_REASON_NTP_NOT_SYNCED
};

// Settings from JSON
struct Settings {
    uint8_t brightness;
    bool autoRotate;
    uint16_t defaultDuration;
    uint16_t weatherDuration;
    char ntpServer[48];
    char tzPosix[64];
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
    SleepSchedule sleep;
} settings;
SleepReason lastSleepReason = SLEEP_REASON_NONE;

// Weather Data (populated by POST /api/weather)
#define MAX_FORECAST_DAYS 7    // Max storage (1 week)
#define FORECAST_COLUMNS  3    // Columns displayed simultaneously
// Past three pages the rotation moves on before the last one has been read
#define MAX_CAROUSEL_PAGES 3
#define MAX_TODAY_HOURS   12   // Hourly window charted by weatherclock
#define MAX_TODAY_SEGMENTS 4   // Sky segments covering that window

struct TodayHour {
    uint8_t hour;              // Local hour 0-23
    int16_t temp;
    uint8_t precipProbability; // 0-100 percent
    uint8_t precipTenthsOfMm;  // 2 means 0.2 mm
};

struct TodaySegment {
    uint8_t fromHour;          // Inclusive, local hour 0-23
    uint8_t toHour;            // Inclusive, local hour 0-23
    char icon[32];
};

struct TodayWindow {
    TodayHour hours[MAX_TODAY_HOURS];
    TodaySegment segments[MAX_TODAY_SEGMENTS];
    uint8_t hourCount;
    uint8_t segmentCount;
};

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
    TodayWindow today;
    unsigned long lastUpdate;
    bool valid;
};
WeatherData weatherData;

// Which price the chart compares every column against: the first point of the series, so
// green means "up since the period opened", or the last one, so green means "higher than
// the price is now".
enum TrackerChartReference : uint8_t {
    TRACKER_REF_OPEN = 0,
    TRACKER_REF_LAST = 1
};

// Tracker Data (populated by POST /api/tracker)
struct TrackerData {
    char name[16];            // Key: "btc", "eth", "aapl"
    // Scrolls when it overflows, so the cap is storage rather than screen width: 64 bytes
    // hold 31 characters whatever their encoding, an accented one taking two.
    char symbol[64];
    char icon[32];            // Icon name (LittleFS)
    char currencySymbol[8];   // "USD", "EUR"
    float currentValue;       // Price/value
    float changePercent;      // +2.14 or -1.5
    uint16_t sparkline[MAX_SPARKLINE_POINTS];  // Scaled 0-65535
    uint8_t sparklineCount;
    // Bars behind the curve. Scaled against the largest value of the series rather than
    // between its extremes, because a volume bar is read from zero.
    uint8_t volumes[MAX_SPARKLINE_POINTS];     // Scaled 0-255
    uint8_t volumeCount;
    uint8_t sparklineRef;     // TrackerChartReference
    uint32_t symbolColor;     // Header color (0xRRGGBB)
    uint32_t sparklineColor;  // Deprecated: the curve now takes its colors from the reference
    // Same reasoning as symbol: 64 bytes hold 31 characters whatever their encoding
    char bottomText[64];      // Optional footer - scrolls when it overflows
    TextSegment bottomTextSegments[MAX_TEXT_SEGMENTS];
    uint8_t bottomTextSegmentCount;
    char sparklinePeriod[8];  // Chart period label, e.g. "24h", "7d"
    unsigned long lastUpdate;
    bool valid;
};
TrackerData trackers[MAX_TRACKERS];
uint8_t trackerCount = 0;

// Gauge Data (populated by POST /api/gauge)
struct GaugeRow {
    char label[12];
    char info[16];
    char value[8];
    char note[8];
    uint8_t percent;          // 0-100, clamped on parse
    uint32_t barColor;
    uint32_t noteColor;
    TextSegment labelSegments[MAX_GAUGE_LABEL_SEGMENTS];
    uint8_t labelSegmentCount;
};

struct GaugeData {
    char name[16];            // Key: "claude", "battery"
    // Scrolls when it overflows, so the cap is storage rather than screen width: 64 bytes
    // hold 31 characters whatever their encoding, an accented one taking two.
    char title[64];
    TextSegment titleSegments[MAX_GAUGE_TITLE_SEGMENTS];
    uint8_t titleSegmentCount;
    char icon[32];            // Icon name (LittleFS)
    GaugeRow rows[MAX_GAUGE_ROWS];
    uint8_t rowCount;
    unsigned long lastUpdate;
    bool valid;
};
GaugeData gauges[MAX_GAUGE_APPS];
uint8_t gaugeCount = 0;

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

// Timing
unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastStatsPublish = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastTimeUpdate = 0;

// Forecast pagination
uint8_t forecastPage = 0;
unsigned long lastForecastPageSwitch = 0;

// Weather display cache (global so they can be reset on app switch)
int weatherLastDrawnMinute = -1;
unsigned long weatherLastUpdateDrawn = 0;
uint8_t weatherFullRepaintsPending = 0;
uint8_t weatherForecastRepaintsPending = 0;

// Tracker display cache (global so they can be reset on app switch)
unsigned long trackerLastUpdateDrawn = 0;
bool trackerBadgeDrawn = false;
bool trackerDimDrawn = false;
bool trackerIconDrawn = false;
uint8_t trackerFullRepaintsPending = 0;

// Gauge display cache (global so they can be reset on app switch)
unsigned long gaugeLastUpdateDrawn = 0;
bool gaugeBadgeDrawn = false;
bool gaugeDimDrawn = false;
bool gaugeIconDrawn = false;
// Which of the two top indicators were in use at the last full paint: the header geometry
// dodges them, so their coming and going has to repaint it
uint8_t gaugeTopIndicatorsDrawn = 0;
uint8_t gaugeFullRepaintsPending = 0;
uint8_t gaugeRowRepaintsPending = 0;
uint8_t gaugePage = 0;
unsigned long lastGaugePageSwitch = 0;

// A screen arms one of these counters when its content changes and spends one unit per full
// paint, so the change reaches every buffer before a partial repaint is allowed to run.
bool consumePendingRepaint(uint8_t& pendingRepaints) {
    if (pendingRepaints == 0) return false;
    pendingRepaints--;
    return true;
}

// The symbol and the bottom text scroll independently, so they each carry their own state.
// Both are written by the render pass and read by loopDisplay; the REST handler can rewrite
// the underlying text from the web server task mid-frame, which costs at worst one torn
// frame, repaired 50 ms later.
ScrollState trackerSymbolScrollState;
ScrollState trackerBottomScrollState;

// Only the gauge title scrolls: its rows are clipped instead, so they need no state of their own.
ScrollState gaugeTitleScrollState;

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
void loopApps();
void loopSleepTransition();

void displayShowBoot();
void displayShowIP();
void displayShowTime();
void displayShowDate();
void displayShowApp(AppItem* app);
void displayShowWeatherClock(const AppItem* app);
void drawDropIcon(int16_t x, int16_t y, uint16_t color);
void drawSeparatorLine(int16_t y, uint16_t color);
void drawIconAtScale(CachedIcon* icon, int16_t x, int16_t y, uint8_t scale);
void displayClear();
void displayDrawOtaProgress(uint8_t percent);
void displaySetBrightness(uint8_t brightness);

uint32_t dimColorQuarter(uint32_t color);
uint16_t dimmedColor565(uint32_t color, bool dimColors);
char utf8FrenchToAscii(uint8_t leadByte, uint8_t continuationByte);
int16_t calculateTextWidth(const char* text);
int16_t calculateTomThumbTextWidth(const char* text);
int16_t tomThumbLeadingInkOffset(const char* text);
void truncateTomThumbTextToWidth(const char* text, char* buffer, size_t bufferSize,
                                 int16_t maxWidth);
void resetScrollState();
void resetTrackerScrollStates();
void resetGaugeDisplayState();
bool scrollStateAdvance(ScrollState& state, unsigned long now);
void scrollStateArm(ScrollState& state, int16_t textWidth, int16_t availableWidth);
void scrollStateReset(ScrollState& state);

int pngDrawCallback(PNGDRAW *pDraw);
CachedIcon* loadIcon(const char* name);
CachedIcon* getIcon(const char* name);
CachedIcon* getCachedIcon(const char* name);
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

void weatherParseTodayBlock(JsonObjectConst todayBlock);
void weatherClockApplyDuration(uint16_t durationMs);
void weatherClockApplyStalePolicy(uint32_t staleAfter, StaleBehavior staleBehavior);

int8_t appAdd(const char* id, const char* text, const char* icon,
              uint32_t textColor, uint16_t duration,
              uint32_t staleAfter, bool isSystem);
bool appRemove(const char* id);
bool appUpdate(const char* id, const char* text, const char* icon,
               uint32_t textColor);
int8_t appFind(const char* id);
void appHideStale();
AppItem* appGetNext();
AppItem* appGetCurrent();
void appSetZones(int8_t appIndex, JsonArray zonesArray);
void displayShowMultiZone(AppItem* app);
void displayShowZone(AppZone* zone, int16_t x, int16_t y, int16_t w, int16_t h);

// Tracker management
TrackerData* trackerFind(const char* name);
const AppItem* trackerFindApp(const char* name);
TrackerData* trackerAllocate(const char* name);
bool trackerRemove(const char* name);
void trackerInit();
void trackerApplyJsonFields(TrackerData* tracker, JsonObject doc);
void stripDegreeSign(char* text, TextSegment* segments, uint8_t segmentCount);
void displayShowTracker(TrackerData* tracker, const AppItem* app);
void displayDrawTrackerSymbol(TrackerData* tracker, bool showBadge, bool dimColors);
void displayDrawTrackerChart(TrackerData* tracker, bool dimColors, int16_t chartBottom);
void displayDrawTrackerPeriod(TrackerData* tracker, bool dimColors);
void drawTrackerArrow(int16_t x, int16_t y, bool up, uint16_t color);
void formatTrackerValue(float value, char* buffer, size_t bufSize, uint8_t maxChars);
void insertThousandSeparators(const char* digits, char* buffer, size_t bufSize);
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
void printTextWithinBounds(const char* text, int16_t x, int16_t y,
                           int16_t boundLeft, int16_t boundRight, uint32_t color,
                           const TextSegment* segments, uint8_t segmentCount);
const TextSegment* dimTextSegments(const TextSegment* segments, uint8_t segmentCount,
                                   bool dimColors, TextSegment* dimmedBuffer);
void drawCharacterWithinBounds(char character, int16_t x, int16_t y,
                               int16_t boundLeft, int16_t boundRight, uint16_t color);

// Gauge management
GaugeData* gaugeFind(const char* name);
const AppItem* gaugeFindApp(const char* name);
GaugeData* gaugeAllocate(const char* name);
bool gaugeRemove(const char* name);
void gaugeInit();
bool gaugeRowCountFits(JsonObject doc);
void gaugeApplyJsonFields(GaugeData* gauge, JsonObject doc);
void displayShowGauge(GaugeData* gauge, const AppItem* app);
void displayDrawGaugeTitle(GaugeData* gauge, bool showBadge, bool dimColors);

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
bool mqttConnect();
void mqttPublishStats();
void mqttHandleCustom(const char* name, JsonObject& doc);
void mqttHandleCustomDelete(const char* name);
void mqttHandleNotify(JsonObject& doc);
void mqttHandleDismiss();
void mqttHandleIndicator(uint8_t index, JsonObject& doc);
void mqttHandleWeather(JsonObject& doc);
void mqttHandleTracker(const char* name, JsonObject& doc);
void mqttHandleTrackerDelete(const char* name);
void mqttHandleGauge(const char* name, JsonObject& doc);
void mqttHandleGaugeDelete(const char* name);
void mqttHandleSettings(JsonObject& doc);
void mqttHandleBrightness(JsonObject& doc);
void mqttHandleReboot();

void handleApiStats(AsyncWebServerRequest *request);
void handleApiSettings(AsyncWebServerRequest *request);
void handleApiApps(AsyncWebServerRequest *request);

void logMemory();

bool sleepIsActive();
static bool dayIndexFromName(const char* name, uint8_t& outIndex);
static const char* dayNameFromIndex(uint8_t index);
static bool parseHourMinute(const char* text, uint8_t& outHour, uint8_t& outMinute);
static void formatHourMinute(uint8_t hour, uint8_t minute, char* out, size_t outSize);
static const char* sleepReasonToString(SleepReason reason);
static void buildSleepConfigJson(JsonObject root);
static bool applySleepUpdate(JsonObject body, String& errorOut);
static void wakeNow();

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

    // Initialize gauge system
    gaugeInit();

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
            displayDrawOtaProgress(0);
        });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            static uint8_t lastPercent = 255;
            uint8_t percent = (uint8_t)((progress * 100) / total);
            // Only redraw every 5% to avoid slowing down OTA transfer
            if (percent == lastPercent || (percent % 5 != 0 && percent != 100)) return;
            lastPercent = percent;
            displayDrawOtaProgress(percent);
        });
        ArduinoOTA.onEnd([]() {
            Serial.println("[OTA] Update complete!");
            dma_display->fillScreen(0);
            dma_display->setTextColor(dma_display->color565(0, 255, 0));
            dma_display->setCursor(13, 24);
            dma_display->print("DONE");
            dma_display->setFont(&TomThumb);
            dma_display->setTextColor(dma_display->color565(100, 100, 100));
            dma_display->setCursor(8, 38);
            dma_display->print("Rebooting...");
            dma_display->setFont(NULL);
            #if DOUBLE_BUFFER
                dma_display->flipDMABuffer();
            #endif
        });
        ArduinoOTA.onError([](ota_error_t error) {
            Serial.printf("[OTA] Error[%u]\n", error);
            dma_display->fillScreen(0);
            dma_display->setTextColor(dma_display->color565(255, 0, 0));
            dma_display->setCursor(7, 28);
            dma_display->print("OTA ERR");
            #if DOUBLE_BUFFER
                dma_display->flipDMABuffer();
            #endif
        });
        ArduinoOTA.begin();

        Serial.println("[INIT] Setting up NTP...");
        configTzTime(settings.tzPosix, settings.ntpServer);

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
        weatherClockApplyStalePolicy(WEATHER_DEFAULT_STALE_AFTER, STALE_HIDE);
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
    loopSleepTransition();
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

    time_t nowUtc = time(nullptr);
    struct tm localTm;
    localtime_r(&nowUtc, &localTm);
    int hours = localTm.tm_hour;
    int minutes = localTm.tm_min;
    int seconds = localTm.tm_sec;

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

    time_t nowUtc = time(nullptr);
    struct tm localTm;
    localtime_r(&nowUtc, &localTm);

    uint8_t day = localTm.tm_mday;
    uint8_t month = localTm.tm_mon + 1;
    uint16_t year = localTm.tm_year + 1900;

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
// Weather Data Functions
// ============================================================================

void weatherParseTodayBlock(JsonObjectConst todayBlock)
{
    weatherData.today.hourCount = 0;
    weatherData.today.segmentCount = 0;

    if (todayBlock.isNull())
    {
        return;
    }

    if (todayBlock["hours"].is<JsonArrayConst>())
    {
        JsonArrayConst hoursArray = todayBlock["hours"];
        int hourCount = min((int)hoursArray.size(), (int)MAX_TODAY_HOURS);
        for (int i = 0; i < hourCount; i++)
        {
            JsonObjectConst hourObject = hoursArray[i];
            TodayHour* hourSlot = &weatherData.today.hours[i];
            hourSlot->hour = constrain((int)(hourObject["h"] | 0), 0, 23);
            // No reading on Earth comes near +/-100 C, and a wider span would
            // overflow the int16_t arithmetic behind the curve
            hourSlot->temp = constrain((int)(hourObject["temp"] | 0), -100, 100);
            hourSlot->precipProbability = constrain((int)(hourObject["pop"] | 0), 0, 100);
            hourSlot->precipTenthsOfMm = constrain((int)(hourObject["precip"] | 0), 0, 255);
        }
        weatherData.today.hourCount = hourCount;
    }

    if (todayBlock["segments"].is<JsonArrayConst>())
    {
        JsonArrayConst segmentsArray = todayBlock["segments"];
        int segmentCount = min((int)segmentsArray.size(), (int)MAX_TODAY_SEGMENTS);
        for (int i = 0; i < segmentCount; i++)
        {
            JsonObjectConst segmentObject = segmentsArray[i];
            TodaySegment* segmentSlot = &weatherData.today.segments[i];
            segmentSlot->fromHour = constrain((int)(segmentObject["from"] | 0), 0, 23);
            segmentSlot->toHour = constrain((int)(segmentObject["to"] | 0), 0, 23);
            strlcpy(segmentSlot->icon, segmentObject["icon"] | "", sizeof(segmentSlot->icon));
        }
        weatherData.today.segmentCount = segmentCount;
    }
}

// ============================================================================
// Tracker Functions
// ============================================================================

#define TRACKER_BOTTOM_COLOR        0x969696
#define TRACKER_BOTTOM_COLOR_STALE  0x3C3C3C
#define TRACKER_SYMBOL_COLOR        0x00DC3C
#define TRACKER_CURRENCY_COLOR      0x787878
#define TRACKER_UP_COLOR            0x00E146
#define TRACKER_DOWN_COLOR          0xFF2D23
#define TRACKER_VOLUME_COLOR        0x242A48
#define TRACKER_REFERENCE_LINE_COLOR 0x646464
#define TRACKER_OTHER_LINE_COLOR    0x464646
#define TRACKER_RULE_COLOR          0x3C3C3C
#define TRACKER_PERIOD_COLOR        0x969696
#define TRACKER_SEPARATOR_COLOR     0x282828

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
            trackers[i].symbolColor = TRACKER_SYMBOL_COLOR;
            trackers[i].sparklineColor = 0x00D4FF;  // Default cyan
            trackers[i].sparklineRef = TRACKER_REF_OPEN;
            strlcpy(trackers[i].sparklinePeriod, "24h", sizeof(trackers[i].sparklinePeriod));
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

// A tracker's stale policy is stored on its rotation app, alongside every other app's.
const AppItem* trackerFindApp(const char* name) {
    char appId[32];
    snprintf(appId, sizeof(appId), "%s%s", TRACKER_ID_PREFIX, name);
    int8_t index = appFind(appId);
    return index >= 0 ? &apps[index] : nullptr;
}

// The footer font has no degree glyph, and the shared text printer draws the mark six rows
// above the row it belongs to - inside the chart, where nothing erases it between two scroll
// steps. It is dropped on the way in, which is what this field has always shown.
void stripDegreeSign(char* text, TextSegment* segments, uint8_t segmentCount) {
    char* readPosition = text;
    char* writePosition = text;

    while (*readPosition) {
        uint8_t degreeBytes = 0;
        if ((uint8_t)*readPosition == 0xC2 && (uint8_t)*(readPosition + 1) == 0xB0) {
            degreeBytes = 2;
        } else if ((uint8_t)*readPosition == 0xB0) {
            degreeBytes = 1;
        }

        if (degreeBytes == 0) {
            *writePosition++ = *readPosition++;
            continue;
        }

        // Segment offsets are byte positions in the string as it arrived, so they move by the
        // bytes removed ahead of them
        uint8_t originalOffset = (uint8_t)(readPosition - text);
        for (uint8_t i = 0; i < segmentCount; i++) {
            if (segments[i].offset > originalOffset) {
                segments[i].offset -= degreeBytes;
            }
        }
        readPosition += degreeBytes;
    }

    *writePosition = '\0';
}

void trackerApplyJsonFields(TrackerData* tracker, JsonObject doc) {
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
        parseTextFieldWithSegments(doc["bottomText"], tracker->bottomText,
                                   sizeof(tracker->bottomText),
                                   tracker->bottomTextSegments,
                                   &tracker->bottomTextSegmentCount,
                                   TRACKER_BOTTOM_COLOR);
        stripDegreeSign(tracker->bottomText, tracker->bottomTextSegments,
                        tracker->bottomTextSegmentCount);
    }
    if (!doc["sparklinePeriod"].isNull()) {
        strlcpy(tracker->sparklinePeriod, doc["sparklinePeriod"] | "", sizeof(tracker->sparklinePeriod));
    }
    if (!doc["sparklineRef"].isNull()) {
        const char* reference = doc["sparklineRef"] | "";
        if (strcmp(reference, "last") == 0) {
            tracker->sparklineRef = TRACKER_REF_LAST;
        } else if (strcmp(reference, "open") == 0) {
            tracker->sparklineRef = TRACKER_REF_OPEN;
        }
    }

    tracker->symbolColor = parseColorValue(doc["symbolColor"], tracker->symbolColor);
    tracker->sparklineColor = parseColorValue(doc["sparklineColor"], tracker->sparklineColor);

    // The API carries the sparkline as plain values, the panel stores them scaled to the full
    // uint16 range so the curve fills its box whatever the units are.
    if (doc["sparkline"].is<JsonArray>()) {
        JsonArray sparkArr = doc["sparkline"];
        uint8_t count = min((int)sparkArr.size(), (int)MAX_SPARKLINE_POINTS);

        if (count >= 2) {
            float minVal = sparkArr[0].as<float>();
            float maxVal = minVal;
            for (uint8_t i = 1; i < count; i++) {
                float v = sparkArr[i].as<float>();
                if (v < minVal) minVal = v;
                if (v > maxVal) maxVal = v;
            }

            float range = maxVal - minVal;
            bool flatSeries = (range < 0.0001f);

            for (uint8_t i = 0; i < count; i++) {
                // A series that never moves sits in the middle of its own range, and drawing
                // it along the floor of the chart would read as a crash instead of as calm.
                float normalized = flatSeries ? 0.5f
                                              : (sparkArr[i].as<float>() - minVal) / range;
                tracker->sparkline[i] = (uint16_t)(normalized * 65535.0f);
            }
            tracker->sparklineCount = count;
        }
    }

    if (doc["volumes"].is<JsonArray>()) {
        JsonArray volumeArr = doc["volumes"];
        uint8_t count = min((int)volumeArr.size(), (int)MAX_SPARKLINE_POINTS);

        float maxVolume = 0.0f;
        for (uint8_t i = 0; i < count; i++) {
            float v = volumeArr[i].as<float>();
            if (v > maxVolume) maxVolume = v;
        }

        for (uint8_t i = 0; i < count; i++) {
            float v = volumeArr[i].as<float>();
            if (v < 0.0f) v = 0.0f;
            tracker->volumes[i] = (maxVolume > 0.0f) ? (uint8_t)(v / maxVolume * 255.0f + 0.5f) : 0;
        }
        tracker->volumeCount = count;
    }
}

// ============================================================================
// Gauge Functions
// ============================================================================

// Up here rather than with the layout constants: each one is read at parse time, as the colour
// its field takes when the client sends none of its own.
#define GAUGE_NOTE_COLOR  0x969696
#define GAUGE_TITLE_COLOR 0xFFFFFF
#define GAUGE_LABEL_COLOR 0xC8C8C8

// The store keeps its own copy of the name while the rotation app id is built from the incoming
// one, so a name the store would have to shorten is refused instead: the two spellings drifting
// apart is what makes a gauge impossible to draw, to find and to delete.
bool gaugeNameFits(const char* name) {
    return strlen(name) < sizeof(GaugeData::name);
}

GaugeData* gaugeFind(const char* name) {
    for (uint8_t i = 0; i < MAX_GAUGE_APPS; i++) {
        if (gauges[i].valid && strcmp(gauges[i].name, name) == 0) {
            return &gauges[i];
        }
    }
    return nullptr;
}

GaugeData* gaugeAllocate(const char* name) {
    // Check if already exists
    GaugeData* existing = gaugeFind(name);
    if (existing) return existing;

    // Find first free slot
    for (uint8_t i = 0; i < MAX_GAUGE_APPS; i++) {
        if (!gauges[i].valid) {
            memset(&gauges[i], 0, sizeof(GaugeData));
            strlcpy(gauges[i].name, name, sizeof(gauges[i].name));
            gauges[i].valid = true;
            gaugeCount++;
            return &gauges[i];
        }
    }
    return nullptr;
}

bool gaugeRemove(const char* name) {
    GaugeData* gauge = gaugeFind(name);
    if (!gauge) return false;

    gauge->valid = false;
    gaugeCount--;

    // Remove corresponding app from rotation
    char appId[32];
    snprintf(appId, sizeof(appId), "%s%s", GAUGE_ID_PREFIX, name);
    appRemove(appId);

    Serial.printf("[GAUGE] Removed: %s\n", name);
    return true;
}

void gaugeInit() {
    memset(gauges, 0, sizeof(gauges));
    gaugeCount = 0;
    Serial.println("[GAUGE] Initialized");
}

// A gauge's stale policy is stored on its rotation app, alongside every other app's.
const AppItem* gaugeFindApp(const char* name) {
    char appId[32];
    snprintf(appId, sizeof(appId), "%s%s", GAUGE_ID_PREFIX, name);
    int8_t index = appFind(appId);
    return index >= 0 ? &apps[index] : nullptr;
}

// Asked before a slot is taken, so an oversized payload is refused without the caller having
// anything to undo. A payload without a rows array is accepted: it leaves the rows in place.
bool gaugeRowCountFits(JsonObject doc) {
    return !doc["rows"].is<JsonArray>() || doc["rows"].as<JsonArray>().size() <= MAX_GAUGE_ROWS;
}

void gaugeApplyJsonFields(GaugeData* gauge, JsonObject doc) {
    // The shared parser fills up to MAX_TEXT_SEGMENTS; a gauge keeps fewer, so the colour
    // changes past its cap are dropped and the tail stays the last kept colour
    TextSegment parsedSegments[MAX_TEXT_SEGMENTS];
    uint8_t parsedCount = 0;

    if (!doc["title"].isNull()) {
        parseTextFieldWithSegments(doc["title"], gauge->title, sizeof(gauge->title),
                                   parsedSegments, &parsedCount, GAUGE_TITLE_COLOR);
        gauge->titleSegmentCount = min(parsedCount, (uint8_t)MAX_GAUGE_TITLE_SEGMENTS);
        memcpy(gauge->titleSegments, parsedSegments,
               gauge->titleSegmentCount * sizeof(TextSegment));
    }
    if (!doc["icon"].isNull()) {
        strlcpy(gauge->icon, doc["icon"] | "", sizeof(gauge->icon));
    }

    if (doc["rows"].is<JsonArray>()) {
        JsonArray rowsArray = doc["rows"];
        memset(gauge->rows, 0, sizeof(gauge->rows));
        gauge->rowCount = 0;

        for (JsonObject rowObject : rowsArray) {
            GaugeRow* row = &gauge->rows[gauge->rowCount];
            parseTextFieldWithSegments(rowObject["label"], row->label, sizeof(row->label),
                                       parsedSegments, &parsedCount, GAUGE_LABEL_COLOR);
            row->labelSegmentCount = min(parsedCount, (uint8_t)MAX_GAUGE_LABEL_SEGMENTS);
            memcpy(row->labelSegments, parsedSegments,
                   row->labelSegmentCount * sizeof(TextSegment));
            strlcpy(row->info, rowObject["info"] | "", sizeof(row->info));
            strlcpy(row->value, rowObject["value"] | "", sizeof(row->value));
            strlcpy(row->note, rowObject["note"] | "", sizeof(row->note));

            int percent = rowObject["percent"] | 0;
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;
            row->percent = (uint8_t)percent;

            row->barColor = parseColorValue(rowObject["color"], 0xFFFFFF);
            row->noteColor = parseColorValue(rowObject["noteColor"], GAUGE_NOTE_COLOR);
            gauge->rowCount++;
        }
    }
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

// Read a "hide" | "dim" | "badge" | "none" name. Returns false when the field is missing or
// holds an unknown name, leaving it to the caller to tell an absent field from a bad one.
bool parseStaleBehavior(JsonVariant value, StaleBehavior* behavior) {
    if (!value.is<const char*>()) return false;

    const char* name = value.as<const char*>();
    if (strcmp(name, "hide") == 0)  { *behavior = STALE_HIDE;  return true; }
    if (strcmp(name, "dim") == 0)   { *behavior = STALE_DIM;   return true; }
    if (strcmp(name, "badge") == 0) { *behavior = STALE_BADGE; return true; }
    if (strcmp(name, "none") == 0)  { *behavior = STALE_NONE;  return true; }
    return false;
}

const char* staleBehaviorName(StaleBehavior behavior) {
    switch (behavior) {
        case STALE_DIM:   return "dim";
        case STALE_BADGE: return "badge";
        case STALE_NONE:  return "none";
        default:          return "hide";
    }
}

// Clients express the tolerated silence in seconds; everything downstream compares against
// millis(). The cap keeps the conversion inside uint32 and well clear of the millis() wrap.
uint32_t staleAfterSecondsToMillis(uint32_t seconds) {
    if (seconds > MAX_STALE_AFTER_SECONDS) seconds = MAX_STALE_AFTER_SECONDS;
    return seconds * 1000UL;
}

bool appIsStale(const AppItem* app) {
    if (!app || app->staleAfter == 0) return false;
    return (millis() - app->lastUpdate) > app->staleAfter;
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

// Sized to a 5x7 cell so the arrow stands as tall as the change it labels
void drawTrackerArrow(int16_t x, int16_t y, bool up, uint16_t color) {
    //   ..X..     ..X..
    //   .XXX.     ..X..
    //   X.X.X     ..X..
    //   ..X..     ..X..
    //   ..X..     X.X.X
    //   ..X..     .XXX.
    //   ..X..     ..X..
    static const uint8_t upRows[7] = {0x04, 0x0E, 0x15, 0x04, 0x04, 0x04, 0x04};

    for (uint8_t row = 0; row < 7; row++) {
        uint8_t bits = up ? upRows[row] : upRows[6 - row];
        for (uint8_t column = 0; column < 5; column++) {
            if ((bits >> (4 - column)) & 1) {
                dma_display->drawPixel(x + column, y + row, color);
            }
        }
    }
}

// The default 5x7 font occupies a 6x8 cell: five columns of glyph plus one of spacing.
#define TEXT_CELL_WIDTH  6
#define TEXT_CELL_HEIGHT 8

// A float holds about seven significant digits. Printing more turns the binary rounding of
// the stored value into digits the caller never sent: 6.232 comes back as 6.2319999.
#define FLOAT_SIGNIFICANT_DIGITS 7

// Decimals kept even when they are zero, because a price reads as a price with its cents.
#define TRACKER_VALUE_MIN_DECIMALS 2

void insertThousandSeparators(const char* digits, char* buffer, size_t bufSize) {
    int digitCount = strlen(digits);
    int separatorCount = (digitCount - 1) / 3;
    int totalLength = digitCount + separatorCount;
    if ((size_t)totalLength >= bufSize) {
        strlcpy(buffer, digits, bufSize);
        return;
    }

    buffer[totalLength] = '\0';
    int sourceIndex = digitCount - 1;
    int destinationIndex = totalLength - 1;
    int writtenDigits = 0;
    while (sourceIndex >= 0) {
        buffer[destinationIndex--] = digits[sourceIndex--];
        writtenDigits++;
        if (writtenDigits % 3 == 0 && sourceIndex >= 0) {
            buffer[destinationIndex--] = ',';
        }
    }
}

// Fills maxChars with as many decimals as the value carries, so a fund quoted at 6.232 keeps
// its third decimal instead of being rounded to 6.23.
void formatTrackerValue(float value, char* buffer, size_t bufSize, uint8_t maxChars) {
    if (bufSize == 0) return;
    if ((size_t)maxChars >= bufSize) maxChars = bufSize - 1;

    bool isNegative = value < 0.0f;
    float magnitude = isNegative ? -value : value;

    int integerDigitCount = 1;
    for (uint32_t remaining = (uint32_t)magnitude; remaining >= 10; remaining /= 10) {
        integerDigitCount++;
    }

    uint8_t leadingZeros = 0;
    if (magnitude > 0.0f && magnitude < 1.0f) {
        for (float scaled = magnitude; scaled < 0.1f; scaled *= 10.0f) {
            leadingZeros++;
        }
    }

    // Leading zeros say nothing about the value, so they cost none of its significant digits.
    int decimals = magnitude >= 1.0f
        ? FLOAT_SIGNIFICANT_DIGITS - integerDigitCount
        : FLOAT_SIGNIFICANT_DIGITS + leadingZeros;
    if (decimals < 0) decimals = 0;

    // Rounding can carry into the integer part, so what fits is read off the formatted string
    // rather than predicted from the value: 9,999.99 at one decimal becomes 10,000.0.
    size_t length;
    do {
        char plainValue[24];
        snprintf(plainValue, sizeof(plainValue), "%.*f", decimals, magnitude);

        char* fraction = strchr(plainValue, '.');
        if (fraction) {
            *fraction++ = '\0';
            // Dropping every trailing zero would leave 0.00 for a value whose first
            // significant digit sits further right than the room allows.
            int minimumDecimals = leadingZeros + 1;
            if (minimumDecimals < TRACKER_VALUE_MIN_DECIMALS) minimumDecimals = TRACKER_VALUE_MIN_DECIMALS;
            if (minimumDecimals > decimals) minimumDecimals = decimals;

            int keptDecimals = strlen(fraction);
            while (keptDecimals > minimumDecimals && fraction[keptDecimals - 1] == '0') {
                fraction[--keptDecimals] = '\0';
            }
        }

        char separatedInteger[24];
        insertThousandSeparators(plainValue, separatedInteger, sizeof(separatedInteger));

        bool hasFraction = fraction && *fraction;
        snprintf(buffer, bufSize, "%s%s%s%s",
                 isNegative ? "-" : "",
                 separatedInteger,
                 hasFraction ? "." : "",
                 hasFraction ? fraction : "");
        length = strlen(buffer);
    } while (length > maxChars && decimals-- > 0);
}

// Tracker row geometry. The header and footer coordinates are shared by the full redraw and
// the scroll repaints.
#define TRACKER_HEADER_HEIGHT       10
#define TRACKER_ICON_X              1
#define TRACKER_ICON_Y              1
#define TRACKER_SYMBOL_X            12
#define TRACKER_SYMBOL_X_NO_ICON    1
#define TRACKER_SYMBOL_Y            2
#define TRACKER_STALE_BADGE_X       42
#define TRACKER_STALE_BADGE_BASELINE 7
#define TRACKER_VALUE_X             1
#define TRACKER_VALUE_Y             10
// Without it the last digit sits one pixel from the currency, the same gap that separates two
// digits, and the two read as one run: "1,234.57EUR".
#define TRACKER_VALUE_GUTTER        2
#define TRACKER_CHANGE_Y            19
#define TRACKER_CHANGE_TEXT_X       8
// TomThumb paints its glyphs on the five rows above the baseline it is given
#define TRACKER_PERIOD_BASELINE     31
#define TRACKER_CHART_RULE_Y        28
// Dark columns between the end of the rule and the first lit column of the period label
#define TRACKER_CHART_RULE_GAP      2
#define TRACKER_CHART_TOP_Y         31
#define TRACKER_CHART_BOTTOM_Y      50
// A tracker with no footer has no separator to draw either, so the chart takes the rows both
// would have used, one short of the bottom edge for the same air it has above the separator.
#define TRACKER_CHART_BOTTOM_Y_TALL 62
#define TRACKER_SEPARATOR_Y         52
#define TRACKER_BOTTOM_Y            55

// --- Row 1, the symbol alone (y=0..9) ---
// Runs every scroll step, and the DMA reads this framebuffer while it is being written, so it
// touches nothing outside the strip the name moves in: clearing the icon to paint it back
// identically twenty times a second is seen as flicker.
void displayDrawTrackerSymbol(TrackerData* tracker, bool showBadge, bool dimColors) {
    uint16_t black = dma_display->color565(0, 0, 0);

    // Taken from the last full redraw rather than the cache, so an icon evicted mid-scroll
    // cannot shift the name sideways
    int16_t symbolAreaX = trackerIconDrawn ? TRACKER_SYMBOL_X : TRACKER_SYMBOL_X_NO_ICON;
    int16_t symbolAreaEnd = showBadge ? TRACKER_STALE_BADGE_X - 1 : DISPLAY_WIDTH;

    dma_display->fillRect(symbolAreaX, 0, symbolAreaEnd - symbolAreaX, TRACKER_HEADER_HEIGHT, black);

    // drawIndicators skips a blinking indicator in its off phase instead of erasing it, so the
    // corners it owns are cleared here for it to blink at all - and only while it is in use,
    // since the top-left one sits over the icon
    for (uint8_t corner = 0; corner < 2; corner++) {
        if (indicators[corner].mode == INDICATOR_OFF) continue;
        int16_t cornerX = (corner == 0) ? 0 : DISPLAY_WIDTH - INDICATOR_FOOTPRINT;
        dma_display->fillRect(cornerX, 0, INDICATOR_FOOTPRINT, INDICATOR_FOOTPRINT, black);

        if (corner == 0 && trackerIconDrawn) {
            // Cache-only lookup: getIcon would fall back to a filesystem read, 20 times a second
            drawIconAtScale(getCachedIcon(tracker->icon), TRACKER_ICON_X, TRACKER_ICON_Y, 1);
        }
    }

    scrollStateArm(trackerSymbolScrollState, calculateTextWidth(tracker->symbol),
                   symbolAreaEnd - symbolAreaX);

    int16_t symbolX = symbolAreaX;
    if (trackerSymbolScrollState.needsScroll) {
        symbolX -= trackerSymbolScrollState.scrollOffset;
    }

    uint32_t symbolColor = dimColors ? dimColorQuarter(tracker->symbolColor) : tracker->symbolColor;

    dma_display->setFont(NULL);  // Default 5x7 font
    dma_display->setTextSize(1);
    printTextWithinBounds(tracker->symbol, symbolX, TRACKER_SYMBOL_Y,
                          symbolAreaX, symbolAreaEnd, symbolColor, nullptr, 0);
}

// --- Row 1: icon + symbol (y=0..9) ---
// The band starts at y=0 so a blinking indicator that went dark does not stay lit.
void displayDrawTrackerHeader(TrackerData* tracker, bool showBadge, bool dimColors, CachedIcon* icon) {
    dma_display->fillRect(0, 0, DISPLAY_WIDTH, TRACKER_HEADER_HEIGHT, dma_display->color565(0, 0, 0));

    trackerIconDrawn = (icon && icon->valid);
    if (trackerIconDrawn) {
        drawIconAtScale(icon, TRACKER_ICON_X, TRACKER_ICON_Y, 1);
    }

    if (showBadge) {
        dma_display->setFont(&TomThumb);
        dma_display->setTextColor(dma_display->color565(200, 0, 0));
        dma_display->setCursor(TRACKER_STALE_BADGE_X, TRACKER_STALE_BADGE_BASELINE);
        dma_display->print("STALE");
    }

    displayDrawTrackerSymbol(tracker, showBadge, dimColors);

    // Contract for the rows drawn after this one: default font, size 1
    dma_display->setFont(NULL);
    dma_display->setTextSize(1);
}

// --- Bottom text (y=55..61) ---
void displayDrawTrackerBottom(TrackerData* tracker, bool dimColors) {
    dma_display->fillRect(0, TRACKER_BOTTOM_Y, DISPLAY_WIDTH,
                          DISPLAY_HEIGHT - TRACKER_BOTTOM_Y, dma_display->color565(0, 0, 0));

    if (strlen(tracker->bottomText) == 0) {
        return;
    }

    int16_t textWidth = calculateTextWidth(tracker->bottomText);
    scrollStateArm(trackerBottomScrollState, textWidth, DISPLAY_WIDTH - 2);

    int16_t textX;
    if (trackerBottomScrollState.needsScroll) {
        textX = 1 - trackerBottomScrollState.scrollOffset;
    } else {
        textX = (DISPLAY_WIDTH - textWidth) / 2;
    }

    TextSegment dimmedSegments[MAX_TEXT_SEGMENTS];
    const TextSegment* segments = dimTextSegments(tracker->bottomTextSegments,
                                                  tracker->bottomTextSegmentCount,
                                                  dimColors, dimmedSegments);

    dma_display->setFont(NULL);
    dma_display->setTextSize(1);
    printTextWithSegments(tracker->bottomText, textX, TRACKER_BOTTOM_Y,
                          dimColors ? TRACKER_BOTTOM_COLOR_STALE : TRACKER_BOTTOM_COLOR,
                          segments, tracker->bottomTextSegmentCount);
}

// The chart plots one point per column, so a series shorter than the 63 columns is stretched
// over them: the shape and both endpoints are the same whether twelve points arrive or sixty.
uint16_t trackerSampleSeries(const uint16_t* series, uint8_t count, int16_t column) {
    if (count < 2) return series[0];

    uint32_t position = (uint32_t)column * (count - 1) * 256 / (TRACKER_CHART_COLUMNS - 1);
    uint8_t index = position >> 8;
    if (index + 1 >= count) return series[count - 1];

    int32_t step = (int32_t)series[index + 1] - (int32_t)series[index];
    return (uint16_t)((int32_t)series[index] + step * (int32_t)(position & 0xFF) / 256);
}

// Volumes take the nearest point instead of an interpolation: a bar stands for a measured
// volume, and averaging two of them would draw a bar nobody reported.
uint8_t trackerSampleVolume(const uint8_t* volumes, uint8_t count, int16_t column) {
    if (count < 2) return volumes[0];

    uint16_t index = ((uint32_t)column * (count - 1) + (TRACKER_CHART_COLUMNS - 1) / 2) /
                     (TRACKER_CHART_COLUMNS - 1);
    return volumes[index];
}

int16_t trackerChartOrdinate(uint16_t value, int16_t chartBottom) {
    const int16_t chartSpan = chartBottom - TRACKER_CHART_TOP_Y;
    return chartBottom - (int16_t)(((uint32_t)value * chartSpan + 32767) / 65535);
}

// --- The chart (top at y=31, columns x=0..62) ---
// Volumes first: they are a backdrop, and every line drawn after them stays readable over
// them because midnight blue has almost nothing in the red and green channels.
void displayDrawTrackerChart(TrackerData* tracker, bool dimColors, int16_t chartBottom) {
    if (tracker->volumeCount > 0) {
        // Three fifths of the chart, so the bars keep their share of it whatever its height
        int16_t volumeMaxHeight = (chartBottom - TRACKER_CHART_TOP_Y + 1) * 3 / 5;
        uint16_t volumeColor = dimmedColor565(TRACKER_VOLUME_COLOR, dimColors);
        for (int16_t column = 0; column < TRACKER_CHART_COLUMNS; column++) {
            uint8_t volume = trackerSampleVolume(tracker->volumes, tracker->volumeCount, column);
            int16_t barHeight = ((int32_t)volume * volumeMaxHeight + 127) / 255;
            if (barHeight < 1) barHeight = 1;
            dma_display->drawFastVLine(column, chartBottom - barHeight + 1,
                                       barHeight, volumeColor);
        }
    }

    if (tracker->sparklineCount < 2) return;

    uint16_t openValue = tracker->sparkline[0];
    uint16_t lastValue = tracker->sparkline[tracker->sparklineCount - 1];
    bool referenceIsLast = (tracker->sparklineRef == TRACKER_REF_LAST);
    uint16_t referenceValue = referenceIsLast ? lastValue : openValue;
    uint16_t otherValue = referenceIsLast ? openValue : lastValue;

    int16_t referenceY = trackerChartOrdinate(referenceValue, chartBottom);
    uint16_t referenceColor = dimmedColor565(TRACKER_REFERENCE_LINE_COLOR, dimColors);
    for (int16_t column = 0; column < TRACKER_CHART_COLUMNS; column += 2) {
        dma_display->drawPixel(column, referenceY, referenceColor);
    }

    // Where the two ends of the series meet, the second line would only thicken the first
    int16_t otherY = trackerChartOrdinate(otherValue, chartBottom);
    if (abs(otherY - referenceY) > 1) {
        uint16_t otherColor = dimmedColor565(TRACKER_OTHER_LINE_COLOR, dimColors);
        for (int16_t column = 0; column < TRACKER_CHART_COLUMNS; column += 4) {
            dma_display->drawPixel(column, otherY, otherColor);
            dma_display->drawPixel(column + 1, otherY, otherColor);
        }
    }

    uint16_t upColor = dimmedColor565(TRACKER_UP_COLOR, dimColors);
    uint16_t downColor = dimmedColor565(TRACKER_DOWN_COLOR, dimColors);

    int16_t previousY = 0;
    int16_t lastColumnY = 0;
    for (int16_t column = 0; column < TRACKER_CHART_COLUMNS; column++) {
        uint16_t value = trackerSampleSeries(tracker->sparkline, tracker->sparklineCount, column);
        int16_t columnY = trackerChartOrdinate(value, chartBottom);
        uint16_t columnColor = (value >= referenceValue) ? upColor : downColor;

        // The vertical run joining two columns takes the colour of the column it lands on, so
        // a curve crossing the reference changes colour on the crossing itself
        if (column > 0) {
            for (int16_t y = min(previousY, columnY) + 1; y < max(previousY, columnY); y++) {
                dma_display->drawPixel(column, y, columnColor);
            }
        }

        dma_display->drawPixel(column, columnY, columnColor);
        previousY = columnY;
        lastColumnY = columnY;
    }

    uint16_t todayColor = dimmedColor565(0xFFFFFF, dimColors);
    dma_display->drawPixel(TRACKER_CHART_COLUMNS - 2, lastColumnY, todayColor);
    dma_display->drawPixel(TRACKER_CHART_COLUMNS - 1, lastColumnY, todayColor);
}

// --- The period label set into the rule above the chart (y=26..30) ---
void displayDrawTrackerPeriod(TrackerData* tracker, bool dimColors) {
    bool hasPeriod = strlen(tracker->sparklinePeriod) > 0;
    int16_t periodWidth = hasPeriod ? calculateTomThumbTextWidth(tracker->sparklinePeriod) : 0;

    int16_t periodX = DISPLAY_WIDTH - periodWidth;
    int16_t ruleEnd = hasPeriod
        ? periodX + tomThumbLeadingInkOffset(tracker->sparklinePeriod) - TRACKER_CHART_RULE_GAP
        : DISPLAY_WIDTH;
    dma_display->drawFastHLine(0, TRACKER_CHART_RULE_Y, ruleEnd,
                               dimmedColor565(TRACKER_RULE_COLOR, dimColors));

    if (!hasPeriod) return;

    dma_display->setFont(&TomThumb);
    dma_display->setTextColor(dimmedColor565(TRACKER_PERIOD_COLOR, dimColors));
    dma_display->setCursor(periodX, TRACKER_PERIOD_BASELINE);
    dma_display->print(tracker->sparklinePeriod);
    dma_display->setFont(NULL);
}

// Display tracker layout on 64x64 matrix
void displayShowTracker(TrackerData* tracker, const AppItem* app) {
    if (!tracker) return;

    // Kept apart so "badge" can flag a closing price as frozen while leaving it readable
    bool isStale = appIsStale(app);
    bool showBadge = isStale && (app->staleBehavior == STALE_DIM || app->staleBehavior == STALE_BADGE);
    bool dimColors = isStale && app->staleBehavior == STALE_DIM;

    // A tracker screen shows a fixed snapshot, and a full paint costs an icon lookup plus the
    // whole layout, so it only runs when the data arrives or goes stale.
    bool contentChanged = (trackerLastUpdateDrawn != tracker->lastUpdate) ||
                          (trackerBadgeDrawn != showBadge) ||
                          (trackerDimDrawn != dimColors);

    if (contentChanged) {
        trackerLastUpdateDrawn = tracker->lastUpdate;
        trackerBadgeDrawn = showBadge;
        trackerDimDrawn = dimColors;
        trackerFullRepaintsPending = DISPLAY_BUFFER_COUNT;
    }

    if (!consumePendingRepaint(trackerFullRepaintsPending)) {
        if (trackerSymbolScrollState.needsScroll) {
            displayDrawTrackerSymbol(tracker, showBadge, dimColors);
        }
        if (trackerBottomScrollState.needsScroll) {
            displayDrawTrackerBottom(tracker, dimColors);
        }
        drawIndicators();
        #if DOUBLE_BUFFER
            dma_display->flipDMABuffer();
        #endif
        return;
    }

    dma_display->clearScreen();

    displayDrawTrackerHeader(tracker, showBadge, dimColors, getIcon(tracker->icon));

    // --- Row 2: price and currency (y=10..16) ---
    bool hasCurrency = strlen(tracker->currencySymbol) > 0;
    // Right-aligned so the last glyph column lands on x=62
    int16_t currencyX = DISPLAY_WIDTH - calculateTextWidth(tracker->currencySymbol);
    int16_t valueAreaEnd = hasCurrency ? currencyX - TRACKER_VALUE_GUTTER : DISPLAY_WIDTH;
    uint8_t valueMaxChars = (valueAreaEnd - TRACKER_VALUE_X) / TEXT_CELL_WIDTH;

    char valueBuf[24];
    formatTrackerValue(tracker->currentValue, valueBuf, sizeof(valueBuf), valueMaxChars);
    dma_display->setTextColor(dimmedColor565(0xFFFFFF, dimColors));
    dma_display->setCursor(TRACKER_VALUE_X, TRACKER_VALUE_Y);
    dma_display->print(valueBuf);

    if (hasCurrency) {
        dma_display->setTextColor(dimmedColor565(TRACKER_CURRENCY_COLOR, dimColors));
        dma_display->setCursor(currencyX, TRACKER_VALUE_Y);
        dma_display->print(tracker->currencySymbol);
    }

    // --- Row 3: arrow and change (y=19..25) ---
    bool isPositive = (tracker->changePercent >= 0.0f);
    uint16_t changeColor = dimmedColor565(isPositive ? TRACKER_UP_COLOR : TRACKER_DOWN_COLOR,
                                           dimColors);

    drawTrackerArrow(TRACKER_VALUE_X, TRACKER_CHANGE_Y, isPositive, changeColor);

    char changeBuf[16];
    snprintf(changeBuf, sizeof(changeBuf), "%s%.2f%%",
             isPositive ? "+" : "", tracker->changePercent);
    dma_display->setTextColor(changeColor);
    dma_display->setCursor(TRACKER_CHANGE_TEXT_X, TRACKER_CHANGE_Y);
    dma_display->print(changeBuf);

    bool hasBottomText = strlen(tracker->bottomText) > 0;

    displayDrawTrackerPeriod(tracker, dimColors);
    displayDrawTrackerChart(tracker, dimColors,
                            hasBottomText ? TRACKER_CHART_BOTTOM_Y : TRACKER_CHART_BOTTOM_Y_TALL);

    if (hasBottomText) {
        dma_display->drawFastHLine(0, TRACKER_SEPARATOR_Y, DISPLAY_WIDTH,
                                   dimmedColor565(TRACKER_SEPARATOR_COLOR, dimColors));
        displayDrawTrackerBottom(tracker, dimColors);
    }

    drawIndicators();

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

// ============================================================
// Layout map (64x64 display) - app "gauge"
// NULL font: setCursor = top-left of the glyph cell, 6px advance, 7px tall
// TomThumb:  setCursor = baseline, capitals sit 5px above it
//
// y=0-11:   header - 8x8 icon at x=2, title at x=13; without an icon the title starts at x=2,
//                    or at x=6 when the top-left indicator is lit. It scrolls when it overflows.
//                    Stale data takes the icon's place with a STALE badge at x=42, or at x=39
//                    when the top-right indicator is lit.
// y=12:     separator line
// y=14-55:  three 14px row bands; a page holding one or two rows is centred in the band instead
//           y=14-27:  row 1 - label/info/value baseline y=19, bar y=20-24
//           y=28-41:  row 2 - baseline y=33, bar y=34-38
//           y=42-55:  row 3 - baseline y=47, bar y=48-52
// y=59-60:  carousel dots, horizontal, centred
// ============================================================
#define GAUGE_HEADER_HEIGHT       12
#define GAUGE_TITLE_X             13
#define GAUGE_TITLE_X_NO_ICON     2
// drawIndicators paints the top-left corner after the header. With an icon under it the title
// already starts clear of the footprint; without one it has to be pushed past it by hand.
#define GAUGE_TITLE_X_PAST_INDICATOR (INDICATOR_FOOTPRINT + 1)
#define GAUGE_TITLE_Y             4
#define GAUGE_STALE_BADGE_X       42
#define GAUGE_BADGE_WIDTH         20   // "STALE" in TomThumb, five glyph advances
#define GAUGE_BADGE_GUTTER        4
// The top-right indicator is painted over the header after it, so while it is in use the badge
// sits entirely left of its footprint rather than losing the top rows of its last glyph. The
// blank column TomThumb carries in every advance is what keeps the two from touching.
#define GAUGE_BADGE_X_PAST_INDICATOR (DISPLAY_WIDTH - INDICATOR_FOOTPRINT - GAUGE_BADGE_WIDTH)
#define GAUGE_SEPARATOR_Y         12
#define GAUGE_ROWS_PER_PAGE       3
#define GAUGE_ROW_TOP_Y           14
#define GAUGE_ROW_HEIGHT          14
#define GAUGE_ROW_BASELINE_OFFSET 5
#define GAUGE_BAR_OFFSET_Y        6
#define GAUGE_BAR_HEIGHT          5
#define GAUGE_BAR_X               1
#define GAUGE_LABEL_X             1
#define GAUGE_RIGHT_X             62
#define GAUGE_FIELD_GUTTER        2
#define GAUGE_DOT_Y               59

// A field right-aligned on GAUGE_RIGHT_X stops its ink two columns earlier, because TomThumb
// carries a blank column inside every glyph advance. The bar ends on that same column so its
// right edge lines up with the values stacked above it.
#define GAUGE_BAR_RIGHT_X         (GAUGE_RIGHT_X - 2)
#define GAUGE_NOTE_WIDTH          20   // Five TomThumb glyphs, the widest note a row has room for
#define GAUGE_BAR_WIDTH_FULL      (GAUGE_BAR_RIGHT_X - GAUGE_BAR_X + 1)
#define GAUGE_BAR_WIDTH_WITH_NOTE (GAUGE_BAR_WIDTH_FULL - GAUGE_NOTE_WIDTH - GAUGE_FIELD_GUTTER)

// The client picks the bar and note colours and may colour the title and the label; the info
// and the value stay fixed so the value, the only figure being read, keeps the strongest
// contrast.
#define GAUGE_INFO_COLOR          0x969696
#define GAUGE_VALUE_COLOR         0xFFFFFF
#define GAUGE_BAR_OUTLINE_COLOR   0x505050
#define GAUGE_SEPARATOR_COLOR     0x282828
#define GAUGE_DOT_ACTIVE_COLOR    0x783CC8
#define GAUGE_DOT_IDLE_COLOR      0x282828

// The carousel is what the rows are shown through, and displayShowGauge clamps its page count
// to MAX_CAROUSEL_PAGES: rows past this product would be stored, returned by the API and never
// drawn, which is exactly the silent amputation the explicit refusal exists to avoid.
static_assert(MAX_GAUGE_ROWS <= GAUGE_ROWS_PER_PAGE * MAX_CAROUSEL_PAGES,
              "MAX_GAUGE_ROWS must not exceed what the carousel can display");

// The rotation app id is GAUGE_ID_PREFIX followed by the gauge name, and a name the store keeps
// whole must reach that id whole too, or the gauge can no longer be found, drawn or deleted.
static_assert(sizeof(GAUGE_ID_PREFIX) - 1 + sizeof(GaugeData::name) <= sizeof(AppItem::id),
              "GAUGE_ID_PREFIX plus a full-length gauge name must fit AppItem::id");

// Goes through the shared label primitive rather than print(): TomThumb has no glyph past 0x7E,
// and GFX drops such a byte without advancing the cursor, so a plain print() of an accented
// field would draw fewer glyphs than every width this layout measures for it. The primitive
// transliterates instead, exactly as calculateTomThumbTextWidth counts, and restores the
// default font on the way out.
void printGaugeRowField(const char* text, int16_t x, int16_t baselineY,
                        uint32_t color, bool dimColors,
                        const TextSegment* segments, uint8_t segmentCount) {
    // The label is the only row field that carries segments, so the scratch is sized on its cap
    TextSegment dimmedSegments[MAX_GAUGE_LABEL_SEGMENTS];
    printLabelWithSegments(text, x, baselineY, dimColors ? dimColorQuarter(color) : color,
                           dimTextSegments(segments, segmentCount, dimColors, dimmedSegments),
                           segmentCount, false);
}

enum GaugeFieldPlacement : uint8_t {
    GAUGE_FIELD_RIGHT_ALIGNED,
    GAUGE_FIELD_CENTRED
};

// Places a field between its two neighbours, cut on the last glyph that fits before it is
// placed: a gap narrower than the text can then neither push it off the panel nor let it paint
// over either neighbour. Right-aligned and centred differ only in where the leftover width
// goes - all of it on the left, or half on each side. Returns the x the field was drawn at.
int16_t printGaugeRowFieldWithinBounds(const char* text, int16_t leftBound, int16_t rightBound,
                                       GaugeFieldPlacement placement, int16_t baselineY,
                                       uint32_t color, bool dimColors) {
    char fitted[sizeof(GaugeRow::info)];
    truncateTomThumbTextToWidth(text, fitted, sizeof(fitted), rightBound - leftBound);

    int16_t leftoverWidth = rightBound - leftBound - calculateTomThumbTextWidth(fitted);
    int16_t x = leftBound + (placement == GAUGE_FIELD_CENTRED ? leftoverWidth / 2 : leftoverWidth);
    printGaugeRowField(fitted, x, baselineY, color, dimColors, nullptr, 0);
    return x;
}

// Read by the badge itself and by the title, which stops short of it: the two have to agree on
// where the badge sits or the title runs into it.
int16_t gaugeStaleBadgeX() {
    return indicators[1].mode != INDICATOR_OFF ? GAUGE_BADGE_X_PAST_INDICATOR
                                               : GAUGE_STALE_BADGE_X;
}

// --- Header, the title alone (y=0..11) ---
// Runs every scroll step, and the DMA reads this framebuffer while it is being written, so it
// touches nothing outside the strip the title moves in.
void displayDrawGaugeTitle(GaugeData* gauge, bool showBadge, bool dimColors) {
    uint16_t black = dma_display->color565(0, 0, 0);

    // Taken from the last full redraw rather than the cache, so an icon evicted mid-scroll
    // cannot shift the title sideways
    int16_t titleAreaX = gaugeIconDrawn ? GAUGE_TITLE_X : GAUGE_TITLE_X_NO_ICON;
    int16_t titleAreaEnd = showBadge ? GAUGE_STALE_BADGE_X - GAUGE_BADGE_GUTTER : DISPLAY_WIDTH;

    dma_display->fillRect(titleAreaX, 0, titleAreaEnd - titleAreaX, GAUGE_HEADER_HEIGHT, black);

    // A blinking indicator in its off phase is the only one drawIndicators leaves unpainted, so
    // it is the only one whose corner has to be cleared here for it to blink at all. Clearing it
    // in the other modes would black out the nine icon pixels the top-left corner covers, twenty
    // times a second, only for drawIndicators to paint over them again.
    for (uint8_t corner = 0; corner < 2; corner++) {
        if (indicators[corner].mode != INDICATOR_BLINK) continue;
        int16_t cornerX = (corner == 0) ? 0 : DISPLAY_WIDTH - INDICATOR_FOOTPRINT;
        dma_display->fillRect(cornerX, 0, INDICATOR_FOOTPRINT, INDICATOR_FOOTPRINT, black);

        if (corner == 0 && gaugeIconDrawn) {
            // Cache-only lookup: getIcon would fall back to a filesystem read, 20 times a second
            drawIconAtScale(getCachedIcon(gauge->icon), 2, 2, 1);
        }
    }

    // Both bounds close in only once the band above has been erased at its widest, so that an
    // indicator switched on mid-app cannot leave part of an old glyph behind at either end
    if (!gaugeIconDrawn && indicators[0].mode != INDICATOR_OFF) {
        titleAreaX = GAUGE_TITLE_X_PAST_INDICATOR;
    }
    if (showBadge) {
        titleAreaEnd = gaugeStaleBadgeX() - GAUGE_BADGE_GUTTER;
    }

    scrollStateArm(gaugeTitleScrollState, calculateTextWidth(gauge->title),
                   titleAreaEnd - titleAreaX);

    int16_t titleX = titleAreaX;
    if (gaugeTitleScrollState.needsScroll) {
        titleX -= gaugeTitleScrollState.scrollOffset;
    }

    TextSegment dimmedTitleSegments[MAX_GAUGE_TITLE_SEGMENTS];
    const TextSegment* titleSegments = dimTextSegments(gauge->titleSegments,
                                                       gauge->titleSegmentCount,
                                                       dimColors, dimmedTitleSegments);

    dma_display->setFont(NULL);  // Default 5x7 font
    dma_display->setTextSize(1);
    printTextWithinBounds(gauge->title, titleX, GAUGE_TITLE_Y, titleAreaX, titleAreaEnd,
                          dimColors ? dimColorQuarter(GAUGE_TITLE_COLOR) : GAUGE_TITLE_COLOR,
                          titleSegments, gauge->titleSegmentCount);
}

// --- Header: icon + title (y=0..11) ---
// Reached only from the full repaint, which clears the screen first, so the band arrives black
// and nothing is erased here.
void displayDrawGaugeHeader(GaugeData* gauge, bool showBadge, bool dimColors, CachedIcon* icon) {
    // The badge occupies 20 of the header's 64 pixels. Keeping the icon as well would leave the
    // title 25 px, less than the 36 a six-character title needs, so the badge takes the icon's
    // place: a stale screen says what it is rather than which icon it had.
    gaugeIconDrawn = (icon && icon->valid) && !showBadge;
    if (gaugeIconDrawn) {
        drawIconAtScale(icon, 2, 2, 1);
    }

    if (showBadge) {
        dma_display->setFont(&TomThumb);
        dma_display->setTextColor(dma_display->color565(200, 0, 0));
        dma_display->setCursor(gaugeStaleBadgeX(), 6);
        dma_display->print("STALE");
    }

    displayDrawGaugeTitle(gauge, showBadge, dimColors);

    // Contract for the rows drawn after this one: default font, size 1
    dma_display->setFont(NULL);
    dma_display->setTextSize(1);
}

// --- One row, two lines (14px band) ---
// Draws only its own ink: displayDrawGaugePage blacks the whole row band before calling this,
// and every page it lays out sits inside that band.
void displayDrawGaugeRow(const GaugeRow* row, int16_t topY, bool dimColors) {
    int16_t baselineY = topY + GAUGE_ROW_BASELINE_OFFSET;

    // The value owns the right edge and the label the left. The label is the only field allowed
    // to lose characters, so it takes what is left once the value, the info and their gutters
    // are reserved; the info is then centred in the gap those two actually leave open.
    int16_t columnRightBound = GAUGE_RIGHT_X;

    if (row->value[0]) {
        columnRightBound = printGaugeRowFieldWithinBounds(row->value, GAUGE_LABEL_X, GAUGE_RIGHT_X,
                                                          GAUGE_FIELD_RIGHT_ALIGNED, baselineY,
                                                          GAUGE_VALUE_COLOR, dimColors)
                           - GAUGE_FIELD_GUTTER;
    }

    int16_t labelMaxWidth = columnRightBound - GAUGE_LABEL_X;
    if (row->info[0]) {
        labelMaxWidth -= calculateTomThumbTextWidth(row->info) + GAUGE_FIELD_GUTTER;
    }

    int16_t labelWidth = 0;
    if (row->label[0]) {
        char clippedLabel[sizeof(row->label)];
        truncateTomThumbTextToWidth(row->label, clippedLabel, sizeof(clippedLabel), labelMaxWidth);
        // The cut lands on a byte boundary and keeps the head of the string, so every surviving
        // segment offset still points where it did
        printGaugeRowField(clippedLabel, GAUGE_LABEL_X, baselineY, GAUGE_LABEL_COLOR, dimColors,
                           row->labelSegments, row->labelSegmentCount);
        labelWidth = calculateTomThumbTextWidth(clippedLabel);
    }

    if (row->info[0]) {
        int16_t gapStart = labelWidth > 0 ? GAUGE_LABEL_X + labelWidth + GAUGE_FIELD_GUTTER
                                          : GAUGE_LABEL_X;
        printGaugeRowFieldWithinBounds(row->info, gapStart, columnRightBound, GAUGE_FIELD_CENTRED,
                                       baselineY, GAUGE_INFO_COLOR, dimColors);
    }

    int16_t barY = topY + GAUGE_BAR_OFFSET_Y;
    int16_t barWidth = row->note[0] ? GAUGE_BAR_WIDTH_WITH_NOTE : GAUGE_BAR_WIDTH_FULL;

    dma_display->drawRect(GAUGE_BAR_X, barY, barWidth, GAUGE_BAR_HEIGHT,
                          dimmedColor565(GAUGE_BAR_OUTLINE_COLOR, dimColors));

    // Rounded to the nearest pixel, with a floor of one so that any share above zero is visible
    // at all - the same arithmetic the hourly rain bars use.
    if (row->percent > 0) {
        int16_t fillWidth = (int16_t)max(1, (row->percent * (barWidth - 2) + 50) / 100);
        dma_display->fillRect(GAUGE_BAR_X + 1, barY + 1, fillWidth, GAUGE_BAR_HEIGHT - 2,
                              dimmedColor565(row->barColor, dimColors));
    }

    if (row->note[0]) {
        printGaugeRowFieldWithinBounds(row->note, GAUGE_BAR_X + barWidth + GAUGE_FIELD_GUTTER,
                                       GAUGE_RIGHT_X, GAUGE_FIELD_RIGHT_ALIGNED,
                                       barY + GAUGE_BAR_HEIGHT, row->noteColor, dimColors);
    }
}

// --- Carousel page: the rows it holds (y=14..55) plus the dots (y=59..60) ---
void displayDrawGaugePage(GaugeData* gauge, uint8_t page, uint8_t pageCount, bool dimColors) {
    uint16_t black = dma_display->color565(0, 0, 0);

    uint8_t firstRow = page * GAUGE_ROWS_PER_PAGE;
    uint8_t rowsOnPage = firstRow < gauge->rowCount
        ? min((uint8_t)(gauge->rowCount - firstRow), (uint8_t)GAUGE_ROWS_PER_PAGE)
        : 0;

    // The whole band is wiped first, so a page shorter than the one before it leaves none of its
    // rows behind, and the short page then sits in the middle of the band rather than at its top.
    dma_display->fillRect(0, GAUGE_ROW_TOP_Y, DISPLAY_WIDTH,
                          GAUGE_ROWS_PER_PAGE * GAUGE_ROW_HEIGHT, black);

    int16_t rowsTopY = GAUGE_ROW_TOP_Y +
                       ((GAUGE_ROWS_PER_PAGE - rowsOnPage) * GAUGE_ROW_HEIGHT) / 2;

    for (uint8_t slot = 0; slot < rowsOnPage; slot++) {
        displayDrawGaugeRow(&gauge->rows[firstRow + slot],
                            rowsTopY + slot * GAUGE_ROW_HEIGHT, dimColors);
    }

    const int16_t dotSize = 2;
    const int16_t dotPitch = 3;

    dma_display->fillRect(0, GAUGE_DOT_Y, DISPLAY_WIDTH, dotSize, black);

    if (pageCount > 1) {
        int16_t startX = (DISPLAY_WIDTH - (pageCount * dotPitch - 1)) / 2;
        for (uint8_t dot = 0; dot < pageCount; dot++) {
            uint32_t dotColor = (dot == page) ? GAUGE_DOT_ACTIVE_COLOR : GAUGE_DOT_IDLE_COLOR;
            // Never dimmed: a quarter of the idle grey is (8,8,8), which the panel does not
            // show, and stale data is exactly when the reader needs to know pages exist.
            dma_display->fillRect(startX + dot * dotPitch, GAUGE_DOT_Y, dotSize, dotSize,
                                  dimmedColor565(dotColor, false));
        }
    }
}

// Display gauge layout on 64x64 matrix
void displayShowGauge(GaugeData* gauge, const AppItem* app) {
    if (!gauge) return;

    bool isStale = appIsStale(app);
    bool showBadge = isStale && (app->staleBehavior == STALE_DIM || app->staleBehavior == STALE_BADGE);
    bool dimColors = isStale && app->staleBehavior == STALE_DIM;

    uint8_t pageCount = max((uint8_t)1,
                            (uint8_t)((gauge->rowCount + GAUGE_ROWS_PER_PAGE - 1) / GAUGE_ROWS_PER_PAGE));
    pageCount = min(pageCount, (uint8_t)MAX_CAROUSEL_PAGES);

    // Every page shows for its share of the app's slot, so the carousel never lengthens the rotation
    unsigned long pageInterval = (unsigned long)app->duration / pageCount;

    // The API handler runs on another task and can reset gaugePage mid-frame, so the whole
    // frame is drawn from one snapshot of it
    uint8_t currentPage = gaugePage;

    // A payload with fewer rows can leave the carousel parked on a page that no longer exists
    if (currentPage >= pageCount) {
        currentPage = 0;
    }

    bool pageChanged = false;
    if (pageCount > 1) {
        unsigned long now = millis();
        if (now - lastGaugePageSwitch >= pageInterval) {
            currentPage = (currentPage + 1) % pageCount;
            lastGaugePageSwitch = now;
            pageChanged = true;
        }
    }

    gaugePage = currentPage;

    // The title and the badge both step aside for the top indicators, so the header has to be
    // laid out again when one of them is switched on or off
    uint8_t topIndicatorsInUse = (indicators[0].mode != INDICATOR_OFF ? 1 : 0) |
                                 (indicators[1].mode != INDICATOR_OFF ? 2 : 0);

    // A gauge screen shows a fixed snapshot, and a full paint costs an icon lookup plus the
    // whole layout, so it only runs when the data arrives or goes stale.
    bool contentChanged = (gaugeLastUpdateDrawn != gauge->lastUpdate) ||
                          (gaugeBadgeDrawn != showBadge) ||
                          (gaugeDimDrawn != dimColors) ||
                          (gaugeTopIndicatorsDrawn != topIndicatorsInUse);

    if (contentChanged) {
        gaugeLastUpdateDrawn = gauge->lastUpdate;
        gaugeBadgeDrawn = showBadge;
        gaugeDimDrawn = dimColors;
        gaugeTopIndicatorsDrawn = topIndicatorsInUse;
        gaugeFullRepaintsPending = DISPLAY_BUFFER_COUNT;
    }
    // The carousel turning is the only thing that needs the rows redrawn on their own: new data
    // already arms the full repaint above, which draws the page as part of the whole screen.
    if (pageChanged) {
        gaugeRowRepaintsPending = DISPLAY_BUFFER_COUNT;
    }

    bool needsFullRedraw = consumePendingRepaint(gaugeFullRepaintsPending);
    bool needsRowRedraw = consumePendingRepaint(gaugeRowRepaintsPending);

    if (!needsFullRedraw) {
        if (needsRowRedraw) {
            displayDrawGaugePage(gauge, currentPage, pageCount, dimColors);
        }
        if (gaugeTitleScrollState.needsScroll) {
            displayDrawGaugeTitle(gauge, showBadge, dimColors);
        }
        drawIndicators();
        #if DOUBLE_BUFFER
            dma_display->flipDMABuffer();
        #endif
        return;
    }

    dma_display->clearScreen();

    displayDrawGaugeHeader(gauge, showBadge, dimColors, getIcon(gauge->icon));
    // Undimmed for the same reason as the dots: it frames the screen instead of carrying data
    drawSeparatorLine(GAUGE_SEPARATOR_Y, dimmedColor565(GAUGE_SEPARATOR_COLOR, false));
    displayDrawGaugePage(gauge, currentPage, pageCount, dimColors);

    drawIndicators();

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

#define TODAY_HEAVY_PRECIP_TENTHS 10
#define TODAY_MIN_PRECIP_PROBABILITY 15

// Hourly chart geometry, all of it inside the y=32-63 block of weatherclock.
// x=61-62 belong to the page indicator, so nothing here goes past x=59.
#define TODAY_HOUR_PITCH_X      5
#define TODAY_BAR_WIDTH         4
#define TODAY_BAR_MAX_HEIGHT    13
#define TODAY_CHART_LEFT_X      1
#define TODAY_CHART_RIGHT_X     59
// Centring the first bar on the margin plus half a bar is what keeps the whole
// chart, bars included, inside TODAY_CHART_LEFT_X without clamping anything
#define TODAY_FIRST_COLUMN_X    (TODAY_CHART_LEFT_X + TODAY_BAR_WIDTH / 2)
#define TODAY_SUMMARY_ROW_TOP_Y 33
#define TODAY_CURVE_TOP_Y       40
#define TODAY_CURVE_BOTTOM_Y    54
#define TODAY_AREA_FLOOR_Y      55
#define TODAY_BASELINE_Y        56
#define TODAY_HOUR_LABEL_TOP_Y  58
#define TODAY_HOUR_LABEL_STEP   4    // one hour label and one tick every four hours

// TomThumb carries neither a degree sign, nor an hour mark short enough to read
// as a unit, nor an mm ligature narrow enough to fit, so the three are painted
// pixel by pixel. Each row below is a bitmask drawn left to right from x, and
// topY is the top of the glyph box: a leading empty row means the glyph hangs
// lower than the digits it follows.
#define DEGREE_GLYPH_WIDTH 3
#define HOUR_UNIT_GLYPH_WIDTH 3
#define MILLIMETRE_GLYPH_WIDTH 7

void drawGlyphRows(int16_t x, int16_t topY, uint8_t width,
                   const uint8_t* rowBits, uint8_t rowCount, uint16_t color)
{
    for (uint8_t row = 0; row < rowCount; row++)
    {
        for (uint8_t column = 0; column < width; column++)
        {
            if (rowBits[row] & (1 << (width - 1 - column)))
            {
                dma_display->drawPixel(x + column, topY + row, color);
            }
        }
    }
}

void drawDegreeGlyph(int16_t x, int16_t topY, uint16_t color)
{
    static const uint8_t degreeRows[] = {
        0b010,
        0b101,
        0b010
    };
    drawGlyphRows(x, topY, DEGREE_GLYPH_WIDTH, degreeRows, sizeof(degreeRows), color);
}

void drawHourUnitGlyph(int16_t x, int16_t topY, uint16_t color)
{
    static const uint8_t hourUnitRows[] = {
        0b000,
        0b100,
        0b100,
        0b111,
        0b101
    };
    drawGlyphRows(x, topY, HOUR_UNIT_GLYPH_WIDTH, hourUnitRows, sizeof(hourUnitRows), color);
}

void drawMillimetreGlyph(int16_t x, int16_t topY, uint16_t color)
{
    static const uint8_t millimetreRows[] = {
        0b0000000,
        0b0000000,
        0b1111111,
        0b1010101,
        0b1010101
    };
    drawGlyphRows(x, topY, MILLIMETRE_GLYPH_WIDTH, millimetreRows, sizeof(millimetreRows), color);
}

// The ramp is anchored on absolute Celsius, not on the extremes of the window:
// anchored on the window, a -6 degree hour would take the warm end of the ramp
// simply for being the mildest hour of a freezing day. Outside these two bounds
// the ramp saturates.
#define TODAY_RAMP_COLD_CELSIUS (-10)
#define TODAY_RAMP_WARM_CELSIUS 35

// Tenths of a degree: the ramp is sampled once per pixel column, and whole degrees
// would break the gradient into five-pixel steps between two hours.
uint8_t weatherTemperatureWarmth(int16_t temperatureTenths)
{
    const int16_t coldTenths = TODAY_RAMP_COLD_CELSIUS * 10;
    const int16_t warmTenths = TODAY_RAMP_WARM_CELSIUS * 10;
    int16_t clampedTenths = constrain(temperatureTenths, coldTenths, warmTenths);
    return (uint8_t)((int32_t)(clampedTenths - coldTenths) * 255 / (warmTenths - coldTenths));
}

// The panel bleeds on saturated red, so the warm end stops at orange. Every stop
// keeps its red channel above 110, which is what separates the ramp from the rain
// bars: those are pure blues with no red at all.
uint16_t weatherTemperatureRampColor(uint8_t warmth)
{
    // { ramp position, red, green, blue }, one stop per named temperature:
    // -10 violet, 0 periwinkle, 10 teal, 18 yellow green, 26 gold, 35 orange
    static const uint8_t rampStops[][4] = {
        {   0, 140,  70, 240 },
        {  57, 130, 130, 245 },
        { 113, 120, 205, 200 },
        { 159, 160, 225, 110 },
        { 204, 235, 205,  80 },
        { 255, 255, 140,  20 }
    };
    const uint8_t rampStopCount = sizeof(rampStops) / sizeof(rampStops[0]);

    uint8_t upperStop = rampStopCount - 1;
    for (uint8_t stop = 1; stop < rampStopCount; stop++)
    {
        if (warmth <= rampStops[stop][0])
        {
            upperStop = stop;
            break;
        }
    }
    uint8_t lowerStop = upperStop - 1;

    int16_t stopDistance = (int16_t)rampStops[upperStop][0] - (int16_t)rampStops[lowerStop][0];
    int16_t positionInStop = (int16_t)warmth - (int16_t)rampStops[lowerStop][0];

    uint8_t channelValue[3];
    for (uint8_t channel = 0; channel < 3; channel++)
    {
        int16_t fromValue = (int16_t)rampStops[lowerStop][channel + 1];
        int16_t toValue = (int16_t)rampStops[upperStop][channel + 1];
        int16_t interpolated = fromValue +
            (int16_t)((int32_t)(toValue - fromValue) * positionInStop / stopDistance);
        channelValue[channel] = (uint8_t)interpolated;
    }

    return dma_display->color565(channelValue[0], channelValue[1], channelValue[2]);
}

int16_t todayColumnCenterX(uint8_t hourIndex)
{
    return TODAY_FIRST_COLUMN_X + TODAY_HOUR_PITCH_X * (int16_t)hourIndex;
}

// Called twice: once with fillArea before the rain bars, once without after them,
// so the crest stays readable where a bar crosses it.
void displayDrawTodayCurve(const TodayWindow* today, const int16_t* pointY,
                           int16_t lastColumnX, bool fillArea)
{
    uint16_t areaTint = dma_display->color565(26, 32, 50);
    uint8_t lastHourIndex = today->hourCount - 1;
    int16_t previousColumnY = -1;

    for (int16_t columnX = TODAY_CHART_LEFT_X; columnX <= lastColumnX; columnX++)
    {
        // Flat lead-in and lead-out: the first and last hours own the columns
        // outside their centres, so the crest spans the whole axis
        uint8_t anchorIndex = 0;
        int16_t offsetWithinHour = 0;
        if (columnX >= todayColumnCenterX(lastHourIndex))
        {
            anchorIndex = lastHourIndex;
        }
        else if (columnX > todayColumnCenterX(0))
        {
            anchorIndex = (uint8_t)((columnX - TODAY_FIRST_COLUMN_X) / TODAY_HOUR_PITCH_X);
            offsetWithinHour = columnX - todayColumnCenterX(anchorIndex);
        }

        int16_t columnY = pointY[anchorIndex];
        int16_t columnTemperatureTenths = (int16_t)today->hours[anchorIndex].temp * 10;
        if (offsetWithinHour > 0)
        {
            int16_t rowSpan = pointY[anchorIndex + 1] - pointY[anchorIndex];
            // The row is rounded as a whole rather than the offset alone, so a
            // half-row tie lands one row lower, like the reference render
            int32_t scaledRow = (int32_t)pointY[anchorIndex] * 2 * TODAY_HOUR_PITCH_X +
                                2 * (int32_t)rowSpan * offsetWithinHour + TODAY_HOUR_PITCH_X;
            columnY = (int16_t)(scaledRow / (2 * TODAY_HOUR_PITCH_X));

            int16_t temperatureSpanTenths =
                ((int16_t)today->hours[anchorIndex + 1].temp -
                 (int16_t)today->hours[anchorIndex].temp) * 10;
            columnTemperatureTenths +=
                (int16_t)((int32_t)temperatureSpanTenths * offsetWithinHour / TODAY_HOUR_PITCH_X);
        }

        uint16_t crestColor =
            weatherTemperatureRampColor(weatherTemperatureWarmth(columnTemperatureTenths));

        if (fillArea && columnY < TODAY_AREA_FLOOR_Y)
        {
            dma_display->drawFastVLine(columnX, columnY + 1,
                                       TODAY_AREA_FLOOR_Y - columnY, areaTint);
        }

        if (previousColumnY >= 0)
        {
            // A jump of more than one row would leave the crest as a dotted line
            int16_t rowsClimbed = abs(columnY - previousColumnY);
            if (rowsClimbed > 1)
            {
                dma_display->drawFastVLine(columnX, min(columnY, previousColumnY),
                                           rowsClimbed + 1, crestColor);
            }
        }
        dma_display->drawPixel(columnX, columnY, crestColor);

        previousColumnY = columnY;
    }
}

void displayDrawTodayRainTotal(const TodayWindow* today, uint16_t rainColor)
{
    uint16_t totalTenthsOfMm = 0;
    for (uint8_t i = 0; i < today->hourCount; i++)
    {
        totalTenthsOfMm += today->hours[i].precipTenthsOfMm;
    }

    if (totalTenthsOfMm == 0)
    {
        return;
    }

    // A tenth of a millimetre stops being worth a character above 10 mm
    char totalLabel[8];
    if (totalTenthsOfMm >= 100)
    {
        snprintf(totalLabel, sizeof(totalLabel), "%d", (totalTenthsOfMm + 5) / 10);
    }
    else
    {
        snprintf(totalLabel, sizeof(totalLabel), "%d.%d",
                 totalTenthsOfMm / 10, totalTenthsOfMm % 10);
    }

    int16_t numberWidth = calculateTomThumbTextWidth(totalLabel);
    int16_t labelWidth = numberWidth + 1 + MILLIMETRE_GLYPH_WIDTH;
    int16_t labelX = (DISPLAY_WIDTH - labelWidth) / 2;

    dma_display->setFont(&TomThumb);
    dma_display->setTextColor(rainColor);
    dma_display->setCursor(labelX, TODAY_SUMMARY_ROW_TOP_Y + 5);
    dma_display->print(totalLabel);
    drawMillimetreGlyph(labelX + numberWidth + 1, TODAY_SUMMARY_ROW_TOP_Y, rainColor);
    dma_display->setFont(NULL);
}

// Number plus degree diamond, tinted by the temperature ramp. topY is the top of
// the glyph box, not the TomThumb baseline. The caller restores the font.
void drawRampTemperature(int16_t labelX, int16_t topY, int16_t temperature)
{
    char label[8];
    snprintf(label, sizeof(label), "%d", temperature);
    uint16_t rampColor = weatherTemperatureRampColor(weatherTemperatureWarmth(temperature * 10));

    dma_display->setFont(&TomThumb);
    dma_display->setTextColor(rampColor);
    dma_display->setCursor(labelX, topY + 5);
    dma_display->print(label);
    drawDegreeGlyph(labelX + calculateTomThumbTextWidth(label), topY, rampColor);
}

int16_t calculateRampTemperatureWidth(int16_t temperature)
{
    char label[8];
    snprintf(label, sizeof(label), "%d", temperature);
    return calculateTomThumbTextWidth(label) + DEGREE_GLYPH_WIDTH;
}

void displayShowTodayChart(const TodayWindow* today)
{
    // ============================================================
    // Layout map of the hourly page (y=32-63 of weatherclock)
    // A TomThumb glyph paints from baseline-5 to baseline-1, so a band whose
    // top row is Y is written with setCursor(_, Y + 5).
    // ============================================================
    // y=33-37:  summary row - temperature of the first charted hour, rain total,
    //           temperature of the last one, each in its own ramp hue
    // y=40-54:  temperature crest, one pixel per column
    // y=43-55:  rain bars, hanging from the top of their height onto the axis
    // y=41-55:  area under the crest
    // y=56:     axis, brighter every four hours
    // y=58-62:  hour labels every four hours
    // ============================================================

    uint16_t labelGray = dma_display->color565(100, 100, 100);
    uint16_t axisGray = dma_display->color565(40, 40, 40);
    uint16_t lightRainBlue = dma_display->color565(0, 90, 180);
    uint16_t heavyRainBlue = dma_display->color565(0, 170, 255);

    // ---- Both ends of the window (y=33-37) ----
    int16_t firstHourTemperature = today->hours[0].temp;
    int16_t lastHourTemperature = today->hours[today->hourCount - 1].temp;

    drawRampTemperature(TODAY_CHART_LEFT_X, TODAY_SUMMARY_ROW_TOP_Y, firstHourTemperature);
    drawRampTemperature(
        TODAY_CHART_RIGHT_X + 1 - calculateRampTemperatureWidth(lastHourTemperature),
        TODAY_SUMMARY_ROW_TOP_Y, lastHourTemperature);
    dma_display->setFont(NULL);

    // ---- Curve, filled pass (y=40-55) ----
    // Scaled on the extremes so the crest uses the full height whatever the span
    int16_t temperatureMin = today->hours[0].temp;
    int16_t temperatureMax = today->hours[0].temp;
    for (uint8_t i = 1; i < today->hourCount; i++)
    {
        temperatureMin = min(temperatureMin, today->hours[i].temp);
        temperatureMax = max(temperatureMax, today->hours[i].temp);
    }

    const int16_t curveHeight = TODAY_CURVE_BOTTOM_Y - TODAY_CURVE_TOP_Y;
    int16_t temperatureRange = temperatureMax - temperatureMin;

    int16_t pointY[MAX_TODAY_HOURS];
    for (uint8_t i = 0; i < today->hourCount; i++)
    {
        if (temperatureRange == 0)
        {
            pointY[i] = (TODAY_CURVE_TOP_Y + TODAY_CURVE_BOTTOM_Y) / 2;
        }
        else
        {
            // The row is rounded as a whole rather than the offset alone, so a
            // half-row tie lands one row lower, like the reference render
            int32_t scaledRow = (int32_t)TODAY_CURVE_BOTTOM_Y * 2 * temperatureRange +
                                temperatureRange -
                                2 * (int32_t)(today->hours[i].temp - temperatureMin) * curveHeight;
            pointY[i] = (int16_t)(scaledRow / (2 * temperatureRange));
        }
    }

    // A window shorter than twelve hours keeps the same pitch and stops where its
    // data stops, rather than extending the last hour across the empty half
    int16_t lastColumnX = min((int16_t)TODAY_CHART_RIGHT_X,
                              (int16_t)(todayColumnCenterX(today->hourCount - 1) + 2));
    displayDrawTodayCurve(today, pointY, lastColumnX, true);

    // ---- Rain bars (y=43-55) ----
    for (uint8_t i = 0; i < today->hourCount; i++)
    {
        uint8_t probability = today->hours[i].precipProbability;
        if (probability < TODAY_MIN_PRECIP_PROBABILITY)
        {
            continue;
        }
        uint8_t barHeight = (uint8_t)max(1, (probability * TODAY_BAR_MAX_HEIGHT + 50) / 100);

        uint16_t barColor = (today->hours[i].precipTenthsOfMm >= TODAY_HEAVY_PRECIP_TENTHS)
            ? heavyRainBlue
            : lightRainBlue;
        dma_display->fillRect(todayColumnCenterX(i) - TODAY_BAR_WIDTH / 2,
                              TODAY_BASELINE_Y - barHeight,
                              TODAY_BAR_WIDTH, barHeight, barColor);
    }

    // ---- Curve, bare pass over the bars ----
    displayDrawTodayCurve(today, pointY, lastColumnX, false);

    // ---- Axis (y=56) and hour labels (y=58-62) ----
    dma_display->drawFastHLine(TODAY_CHART_LEFT_X, TODAY_BASELINE_Y,
                               TODAY_CHART_RIGHT_X - TODAY_CHART_LEFT_X + 1, axisGray);
    dma_display->setFont(&TomThumb);
    dma_display->setTextColor(labelGray);
    for (uint8_t i = 0; i < today->hourCount; i += TODAY_HOUR_LABEL_STEP)
    {
        int16_t columnCenterX = todayColumnCenterX(i);
        dma_display->drawPixel(columnCenterX, TODAY_BASELINE_Y, labelGray);

        char hourLabel[4];
        snprintf(hourLabel, sizeof(hourLabel), "%d", today->hours[i].hour);
        int16_t hourNumberWidth = calculateTomThumbTextWidth(hourLabel);
        int16_t hourLabelWidth = hourNumberWidth + 1 + HOUR_UNIT_GLYPH_WIDTH;
        int16_t hourLabelX = max(TODAY_CHART_LEFT_X, columnCenterX - hourLabelWidth / 2);

        dma_display->setCursor(hourLabelX, TODAY_HOUR_LABEL_TOP_Y + 5);
        dma_display->print(hourLabel);
        drawHourUnitGlyph(hourLabelX + hourNumberWidth + 1, TODAY_HOUR_LABEL_TOP_Y, labelGray);
    }
    dma_display->setFont(NULL);

    displayDrawTodayRainTotal(today, heavyRainBlue);
}

// Number plus degree diamond, the pair centred as a whole on centerX. baselineY is
// the TomThumb baseline, so the diamond hangs from baselineY - 5.
void drawCenteredTemperature(int16_t centerX, int16_t baselineY, int16_t temperature, uint16_t color)
{
    char label[8];
    snprintf(label, sizeof(label), "%d", temperature);
    int16_t labelWidth = calculateTomThumbTextWidth(label);
    int16_t labelX = centerX - (labelWidth + DEGREE_GLYPH_WIDTH) / 2;

    dma_display->setFont(&TomThumb);
    dma_display->setTextColor(color);
    dma_display->setCursor(labelX, baselineY);
    dma_display->print(label);
    drawDegreeGlyph(labelX + labelWidth, baselineY - 5, color);
}

// Dwell of the hourly carousel page, in day pages
#define HOURLY_PAGE_WEIGHT 2

void displayShowWeatherClock(const AppItem* app) {
    uint16_t appDuration = app ? app->duration : settings.weatherDuration;

    // A forecast that stopped being refreshed turns wrong rather than merely old, so it gives
    // way to the clock. The app itself stays in the rotation: it is the only system app left,
    // and dropping it would leave nothing to draw.
    bool hideWhenStale = !app || app->staleBehavior != STALE_NONE;
    if (!weatherData.valid || (hideWhenStale && appIsStale(app))) {
        displayShowTime();
        return;
    }

    // Use global weatherLastDrawnMinute / weatherLastUpdateDrawn
    // (reset by displayShowApp on app switch to force full redraw)

    time_t nowUtc = time(nullptr);
    struct tm localTm;
    localtime_r(&nowUtc, &localTm);
    int hours = localTm.tm_hour;
    int minutes = localTm.tm_min;
    int seconds = localTm.tm_sec;

    if (!settings.clockFormat24h && hours > 12) {
        hours -= 12;
    }

    bool contentChanged = (weatherLastDrawnMinute != minutes) ||
                          (weatherLastUpdateDrawn != weatherData.lastUpdate);

    // Forecast pagination. The hourly window, when there is one, takes the first
    // page of the same carousel and pushes the days one page to the right.
    bool hasHourlyPage = (weatherData.today.hourCount > 0);
    uint8_t dayPageCount =
        (weatherData.forecastCount + FORECAST_COLUMNS - 1) / FORECAST_COLUMNS;
    uint8_t forecastPageCount = max((uint8_t)1, (uint8_t)(dayPageCount + (hasHourlyPage ? 1 : 0)));
    forecastPageCount = min(forecastPageCount, (uint8_t)MAX_CAROUSEL_PAGES);

    // The hourly page holds the screen HOURLY_PAGE_WEIGHT times longer than a day
    // page, and the app's total time is unchanged: the day pages give up what the
    // chart takes.
    uint8_t totalPageWeight = forecastPageCount + (hasHourlyPage ? HOURLY_PAGE_WEIGHT - 1 : 0);
    unsigned long dayPageInterval = (unsigned long)appDuration / totalPageWeight;

    // The API handler runs on another task and can reset forecastPage mid-frame,
    // so the whole frame is drawn from one snapshot of it
    uint8_t currentPage = forecastPage;

    // A payload with fewer days, or with the hourly window dropped, can leave the
    // carousel parked on a page that no longer exists
    if (currentPage >= forecastPageCount) {
        currentPage = 0;
    }

    bool showingHourlyPage = (hasHourlyPage && currentPage == 0);
    unsigned long pageInterval = showingHourlyPage
        ? dayPageInterval * HOURLY_PAGE_WEIGHT
        : dayPageInterval;

    bool pageChanged = false;
    if (forecastPageCount > 1) {
        unsigned long now = millis();
        if (now - lastForecastPageSwitch >= pageInterval) {
            currentPage = (currentPage + 1) % forecastPageCount;
            lastForecastPageSwitch = now;
            pageChanged = true;
        }
    }

    forecastPage = currentPage;

    // A change has to reach every buffer before the seconds-only path is allowed to run
    if (contentChanged) {
        weatherLastDrawnMinute = minutes;
        weatherLastUpdateDrawn = weatherData.lastUpdate;
        weatherFullRepaintsPending = DISPLAY_BUFFER_COUNT;
    }
    if (contentChanged || pageChanged) {
        weatherForecastRepaintsPending = DISPLAY_BUFFER_COUNT;
    }

    bool needsFullRedraw = consumePendingRepaint(weatherFullRepaintsPending);
    bool needsForecastRedraw = consumePendingRepaint(weatherForecastRepaintsPending);

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
    // y=32-63:  carousel (32px) - hourly chart first when the data is there,
    //           then the forecast days FORECAST_COLUMNS at a time
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

        // Today's min/max on the right, chained left to right from the measured
        // widths so a one- or three-digit value keeps its diamond attached
        char todayMinStr[8], todayMaxStr[8];
        snprintf(todayMinStr, sizeof(todayMinStr), "%d", weatherData.currentTempMin);
        snprintf(todayMaxStr, sizeof(todayMaxStr), "%d", weatherData.currentTempMax);

        const int16_t degreeAdvance = DEGREE_GLYPH_WIDTH + 1;
        int16_t minAdvance = calculateTomThumbTextWidth(todayMinStr);
        int16_t maxAdvance = calculateTomThumbTextWidth(todayMaxStr);
        int16_t slashAdvance = calculateTomThumbTextWidth("/");
        int16_t pairWidth = minAdvance + degreeAdvance + slashAdvance + maxAdvance + DEGREE_GLYPH_WIDTH;
        int16_t minX = DISPLAY_WIDTH - 2 - pairWidth;

        char tempStr[8];
        snprintf(tempStr, sizeof(tempStr), "%d", weatherData.currentTemp);
        dma_display->setCursor(weatherTextX, 2);
        dma_display->print(tempStr);

        // Sub-zero on all three values runs the current temperature into the pair,
        // and the diamond is the first thing worth giving up
        int16_t degreeX = weatherTextX + strlen(tempStr) * 6;
        if (degreeX + DEGREE_GLYPH_WIDTH <= minX) {
            drawDegreeGlyph(degreeX, 1, white);
        }

        dma_display->setFont(&TomThumb);
        dma_display->setTextColor(coldBlue);
        dma_display->setCursor(minX, 8);
        dma_display->print(todayMinStr);
        drawDegreeGlyph(minX + minAdvance, 3, coldBlue);

        int16_t slashX = minX + minAdvance + degreeAdvance;
        dma_display->setTextColor(gray);
        dma_display->setCursor(slashX, 8);
        dma_display->print("/");

        int16_t maxX = slashX + slashAdvance;
        dma_display->setTextColor(warmRed);
        dma_display->setCursor(maxX, 8);
        dma_display->print(todayMaxStr);
        drawDegreeGlyph(maxX + maxAdvance, 3, warmRed);

        // ---- Separator (y=10) ----
        dma_display->fillRect(0, 10, DISPLAY_WIDTH, 1, black);
        drawSeparatorLine(10, dimGray);

        // ---- Date (y=21-30) ----
        dma_display->fillRect(0, 21, DISPLAY_WIDTH, 10, black);

        static const char* monthNamesFr[] = {"JAN", "FEV", "MAR", "AVR", "MAI", "JUN",
                                             "JUL", "AOU", "SEP", "OCT", "NOV", "DEC"};

        // Indexed by tm_wday, so Sunday first
        static const char* const dayNamesFr[] = {"DIM", "LUN", "MAR", "MER",
                                                 "JEU", "VEN", "SAM"};

        char dateStr[16];
        snprintf(dateStr, sizeof(dateStr), "%s %02d %s",
                 dayNamesFr[localTm.tm_wday],
                 localTm.tm_mday,
                 monthNamesFr[localTm.tm_mon]);

        dma_display->setFont(NULL);
        dma_display->setTextSize(1);
        dma_display->setTextColor(gray);

        int16_t dateWidth = strlen(dateStr) * 6;
        int16_t dateX = (DISPLAY_WIDTH - dateWidth) / 2;
        dma_display->setCursor(dateX, 22);
        dma_display->print(dateStr);

        // ---- Separator (y=31) ----
        drawSeparatorLine(31, dimGray);
    }

    // ---- Forecast (y=33-63) - redrawn on full redraw or page change ----
    if (needsForecastRedraw) {
        dma_display->fillRect(0, 32, DISPLAY_WIDTH, 32, black);

        if (hasHourlyPage && currentPage == 0) {
            displayShowTodayChart(&weatherData.today);
        } else {
            // Compute which forecast days to display on the current page
            uint8_t dayPage = hasHourlyPage ? (currentPage - 1) : currentPage;
            uint8_t pageStart = dayPage * FORECAST_COLUMNS;
            uint8_t displayCount = (pageStart < weatherData.forecastCount)
                ? min((uint8_t)FORECAST_COLUMNS,
                      (uint8_t)(weatherData.forecastCount - pageStart))
                : 0;

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
                int16_t dayNameWidth = calculateTomThumbTextWidth(weatherData.forecast[forecastIndex].dayName);
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
                drawCenteredTemperature(colCenter, 56,
                                        weatherData.forecast[forecastIndex].tempMin, coldBlue);

                // Max temp in red (TomThumb baseline=63, glyphs y=58-62)
                drawCenteredTemperature(colCenter, 63,
                                        weatherData.forecast[forecastIndex].tempMax, warmRed);
            }
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
                uint16_t dotColor = (d == currentPage) ? activeDot : dimGray;
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
        displayClear();
        lastDisplayedAppIndex = appIndex;
        // Reset weather display cache to force full redraw
        weatherLastDrawnMinute = -1;
        weatherLastUpdateDrawn = 0;
        // Reset tracker display cache to force full redraw
        trackerLastUpdateDrawn = 0;
        resetTrackerScrollStates();
        resetGaugeDisplayState();
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
        displayShowWeatherClock(app);
        return;
    }

    // Tracker layout apps (ID starts with "tracker_")
    if (strncmp(app->id, TRACKER_ID_PREFIX, strlen(TRACKER_ID_PREFIX)) == 0) {
        const char* trackerName = app->id + strlen(TRACKER_ID_PREFIX);
        TrackerData* tracker = trackerFind(trackerName);
        if (tracker && tracker->valid) {
            // The same app can have rendered through the custom layout below moments ago, while
            // its tracker data was still missing. That fallback arms appScrollState with the
            // symbol it printed as plain text, and nothing else disarms it: without this the
            // loop would keep asking for 20 redraws a second for a string no longer drawn.
            if (appScrollState.needsScroll) {
                resetScrollState();
            }
            displayShowTracker(tracker, app);
            return;
        }
        // Fallback to default custom app layout if no data
    }

    // Gauge layout apps (ID starts with "gauge_")
    if (strncmp(app->id, GAUGE_ID_PREFIX, strlen(GAUGE_ID_PREFIX)) == 0) {
        const char* gaugeName = app->id + strlen(GAUGE_ID_PREFIX);
        GaugeData* gauge = gaugeFind(gaugeName);
        if (gauge && gauge->valid) {
            // The custom layout below may have rendered this app moments ago, while its gauge
            // data was still missing, arming appScrollState with the title it printed as plain
            // text. Nothing else disarms it, and the loop would keep asking for 20 redraws a
            // second for a string no longer drawn.
            if (appScrollState.needsScroll) {
                resetScrollState();
            }
            displayShowGauge(gauge, app);
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
    scrollStateArm(appScrollState, calculateTextWidth(app->text), textAreaWidth);

    // Calculate x position with scroll offset
    int16_t xPos = textAreaX;
    if (appScrollState.needsScroll) {
        xPos = textAreaX - appScrollState.scrollOffset;
    }

    // Draw text with segment-aware coloring
    printTextWithSegments(app->text, xPos, textYPos, app->textColor,
                          app->textSegments, app->textSegmentCount);

    // Draw label below text if present (TomThumb font, dimmed color)
    if (app->label[0] != '\0') {
        int16_t labelWidth = calculateTomThumbTextWidth(app->label);
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
            int16_t labelWidth = calculateTomThumbTextWidth(zone->label);
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
    // Every buffer, so a later flip cannot bring back the frame that was on screen
    for (uint8_t remainingBuffers = DISPLAY_BUFFER_COUNT; remainingBuffers > 0; remainingBuffers--) {
        dma_display->clearScreen();
        #if DOUBLE_BUFFER
            dma_display->flipDMABuffer();
        #endif
    }
}

// Repainted whole on every step: with two buffers, a bar drawn on top of the previous frame
// would land on the frame before it.
void displayDrawOtaProgress(uint8_t percent) {
    dma_display->fillScreen(0);
    dma_display->setFont(NULL);
    dma_display->setTextSize(1);
    dma_display->setTextColor(dma_display->color565(255, 165, 0));
    // "OTA" default font, centered (3 chars x 6px = 18px)
    dma_display->setCursor(23, 4);
    dma_display->print("OTA");
    // "UPDATE" same font, centered (6 chars x 6px = 36px)
    dma_display->setCursor(14, 18);
    dma_display->print("UPDATE");
    // Progress bar frame near bottom
    dma_display->drawRect(4, 46, 56, 7, dma_display->color565(80, 80, 80));

    uint8_t barWidth = (uint8_t)(((uint16_t)percent * 54) / 100);
    if (barWidth > 0) {
        dma_display->fillRect(5, 47, barWidth, 5, dma_display->color565(255, 165, 0));
    }

    char percentText[8];
    snprintf(percentText, sizeof(percentText), "%d%%", percent);
    dma_display->setFont(&TomThumb);
    dma_display->setTextColor(dma_display->color565(150, 150, 150));
    int16_t textWidth = calculateTomThumbTextWidth(percentText);
    dma_display->setCursor((DISPLAY_WIDTH - textWidth) / 2, 60);
    dma_display->print(percentText);
    dma_display->setFont(NULL);

    #if DOUBLE_BUFFER
        dma_display->flipDMABuffer();
    #endif
}

void displaySetBrightness(uint8_t brightness) {
    currentBrightness = constrain(brightness, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
    dma_display->setBrightness8(currentBrightness);
    Serial.printf("[DISPLAY] Brightness set to %d\n", currentBrightness);
}

// A stale screen shows everything at a quarter brightness. Masking after the shift keeps
// each component's bits from bleeding into the one below it.
uint32_t dimColorQuarter(uint32_t color)
{
    return (color >> 2) & 0x3F3F3F;
}

uint16_t dimmedColor565(uint32_t color, bool dimColors) {
    uint32_t shown = dimColors ? dimColorQuarter(color) : color;
    return dma_display->color565((shown >> 16) & 0xFF, (shown >> 8) & 0xFF, shown & 0xFF);
}

// The segment printers dim the default colour only, so a dimmed screen has to hand them
// segment colours already quartered. Returns the buffer it filled, or the segments untouched.
const TextSegment* dimTextSegments(const TextSegment* segments, uint8_t segmentCount,
                                   bool dimColors, TextSegment* dimmedBuffer) {
    if (!dimColors || segmentCount == 0) {
        return segments;
    }

    for (uint8_t i = 0; i < segmentCount; i++) {
        dimmedBuffer[i] = segments[i];
        dimmedBuffer[i].color = dimColorQuarter(segments[i].color);
    }
    return dimmedBuffer;
}

// The panel has no accented glyphs in either font, so an accented name is drawn as its
// closest ASCII letter. Callers must pass both UTF-8 bytes: 'e' acute is C3 A9 and the
// copyright sign is C2 A9, so the continuation byte alone does not identify the character.
// An unknown C3 sequence becomes '?' and anything else is dropped, which is what the panel
// already did before this was a shared table.
char utf8FrenchToAscii(uint8_t leadByte, uint8_t continuationByte)
{
    if (leadByte != 0xC3)
    {
        return 0;
    }

    switch (continuationByte)
    {
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: return 'A';
        case 0x87: return 'C';
        case 0x88: case 0x89: case 0x8A: case 0x8B: return 'E';
        case 0x8C: case 0x8D: case 0x8E: case 0x8F: return 'I';
        case 0x91: return 'N';
        case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: return 'O';
        case 0x99: case 0x9A: case 0x9B: case 0x9C: return 'U';
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: return 'a';
        case 0xA7: return 'c';
        case 0xA8: case 0xA9: case 0xAA: case 0xAB: return 'e';
        case 0xAC: case 0xAD: case 0xAE: case 0xAF: return 'i';
        case 0xB1: return 'n';
        case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: return 'o';
        case 0xB9: case 0xBA: case 0xBB: case 0xBC: return 'u';
        default: return '?';
    }
}

// Must advance exactly like printTextWithSpecialChars, or a scrolling string is measured
// against a width it never occupies: 6 px per drawn character, 4 px for the degree glyph,
// nothing for bytes the panel drops.
int16_t calculateTextWidth(const char* text) {
    const uint8_t charWidth = 6;
    int16_t totalWidth = 0;

    const uint8_t* ptr = (const uint8_t*)text;
    while (*ptr) {
        uint8_t c = *ptr;

        if ((c == 0xC2 && *(ptr + 1) == 0xB0) || c == 0xB0) {
            totalWidth += 4;
            ptr += (c == 0xB0) ? 1 : 2;
            continue;
        }

        if (c == 0xC3 && *(ptr + 1)) {
            totalWidth += charWidth;
            ptr += 2;
            continue;
        }

        if (c >= 32 && c <= 126) {
            totalWidth += charWidth;
        }
        ptr++;
    }

    return totalWidth;
}

// TomThumb is not monospaced: '1' advances 3 px and space 2 px where most
// glyphs advance 4, so a strlen-based estimate misplaces right-aligned and
// centred strings and makes them jitter as their digits change.
int16_t calculateTomThumbTextWidth(const char* text)
{
    int16_t totalWidth = 0;
    const uint8_t* character = (const uint8_t*)text;

    while (*character)
    {
        uint8_t characterCode = *character;

        if (characterCode == 0xC3 && *(character + 1))
        {
            characterCode = (uint8_t)utf8FrenchToAscii(characterCode, *(character + 1));
            character++;
        }

        if (characterCode >= 0x20 && characterCode <= 0x7E)
        {
            totalWidth += (int16_t)pgm_read_byte(&TomThumbGlyphs[characterCode - 0x20].xAdvance);
        }
        character++;
    }

    return totalWidth;
}

// TomThumb insets some glyphs by a column: '1' starts one column right of the cursor where
// '7' starts on it. Anything measuring a gap in front of a label has to add this, or the gap
// is a column wider before some labels than before others.
int16_t tomThumbLeadingInkOffset(const char* text)
{
    uint8_t characterCode = (uint8_t)text[0];

    if (characterCode == 0xC3 && text[1])
    {
        characterCode = (uint8_t)utf8FrenchToAscii(characterCode, (uint8_t)text[1]);
    }

    if (characterCode < 0x20 || characterCode > 0x7E)
    {
        return 0;
    }

    return (int16_t)(int8_t)pgm_read_byte(&TomThumbGlyphs[characterCode - 0x20].xOffset);
}

// TomThumb advances differ per glyph, so text that overruns its column is cut on the last
// glyph that fits rather than on a character count.
void truncateTomThumbTextToWidth(const char* text, char* buffer, size_t bufferSize,
                                 int16_t maxWidth) {
    size_t writtenBytes = 0;
    int16_t usedWidth = 0;
    const uint8_t* character = (const uint8_t*)text;

    while (*character) {
        // An accented letter arrives as two bytes and is drawn as one transliterated glyph
        uint8_t glyphByteCount = (*character == 0xC3 && *(character + 1)) ? 2 : 1;
        char glyphBytes[3] = {(char)character[0], '\0', '\0'};
        if (glyphByteCount == 2) {
            glyphBytes[1] = (char)character[1];
        }

        int16_t glyphWidth = calculateTomThumbTextWidth(glyphBytes);
        if (usedWidth + glyphWidth > maxWidth) break;
        if (writtenBytes + glyphByteCount >= bufferSize) break;

        for (uint8_t i = 0; i < glyphByteCount; i++) {
            buffer[writtenBytes++] = (char)character[i];
        }
        usedWidth += glyphWidth;
        character += glyphByteCount;
    }

    buffer[writtenBytes] = '\0';
}

// One three-phase run for every scrolling text on screen: pause at the start, step one pixel
// per SCROLL_SPEED, pause at the end, rewind. Returns whether the caller has to repaint.
bool scrollStateAdvance(ScrollState& state, unsigned long now) {
    if (!state.needsScroll) return false;

    switch (state.scrollPhase) {
        case 0:  // pause_start
            if (now - state.lastScrollTime >= SCROLL_PAUSE) {
                state.scrollPhase = 1;
                state.lastScrollTime = now;
            }
            return false;

        case 1:  // scrolling
            if (now - state.lastScrollTime < SCROLL_SPEED) return false;
            state.lastScrollTime = now;
            state.scrollOffset++;
            if (state.scrollOffset >= state.textWidth - state.availableWidth + 10) {
                state.scrollPhase = 2;
            }
            return true;

        case 2:  // pause_end
            if (now - state.lastScrollTime >= SCROLL_PAUSE) {
                state.scrollOffset = 0;
                state.scrollPhase = 0;
                state.lastScrollTime = now;
            }
            return false;
    }

    return false;
}

// Feed a state from the text it has to show, and rewind it when the text no longer overflows.
void scrollStateArm(ScrollState& state, int16_t textWidth, int16_t availableWidth) {
    if (state.textWidth == textWidth && state.availableWidth == availableWidth) return;

    state.textWidth = textWidth;
    state.availableWidth = availableWidth;
    state.needsScroll = (textWidth > availableWidth);
    if (!state.needsScroll) {
        state.scrollOffset = 0;
        state.scrollPhase = 0;
    }
}

void scrollStateReset(ScrollState& state) {
    state.scrollOffset = 0;
    state.lastScrollTime = millis();
    state.scrollPhase = 0;  // Start with pause
    state.needsScroll = false;
    state.textWidth = 0;
    state.availableWidth = 0;
}

void resetScrollState() {
    scrollStateReset(appScrollState);
}

void resetNotifScrollState() {
    scrollStateReset(notifScrollState);
}

void resetTrackerScrollStates() {
    scrollStateReset(trackerSymbolScrollState);
    scrollStateReset(trackerBottomScrollState);
}

void resetGaugeDisplayState() {
    gaugeLastUpdateDrawn = 0;
    scrollStateReset(gaugeTitleScrollState);
    gaugePage = 0;
    lastGaugePageSwitch = millis();
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
    scrollStateArm(notifScrollState, textWidth, textAreaWidth);

    int16_t xPos;
    if (!notifScrollState.needsScroll) {
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

    char colorHex[8];
    formatColorHex(color, colorHex, sizeof(colorHex));
    char response[128];
    snprintf(response, sizeof(response),
             "{\"success\":true,\"indicator\":%d,\"mode\":\"%s\",\"color\":\"%s\"}",
             index + 1,
             mode == INDICATOR_SOLID ? "solid" : (mode == INDICATOR_BLINK ? "blink" : "fade"),
             colorHex);
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
        if (c == 0xC3 && *(ptr + 1)) {
            dma_display->print(utf8FrenchToAscii(c, *(ptr + 1)));
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
        if (c == 0xC3 && *(ptr + 1)) {
            dma_display->print(utf8FrenchToAscii(c, *(ptr + 1)));
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

// Adafruit_GFX draws a character whole or not at all, so one that straddles a bound goes
// through a stencil first.
void drawCharacterWithinBounds(char character, int16_t x, int16_t y,
                               int16_t boundLeft, int16_t boundRight, uint16_t color) {
    static GFXcanvas1 glyphStencil(TEXT_CELL_WIDTH, TEXT_CELL_HEIGHT);

    // Opaque draw, so every cell is written and nothing carries over from the last character
    glyphStencil.drawChar(0, 0, character, 1, 0, 1);

    for (int16_t column = 0; column < TEXT_CELL_WIDTH; column++) {
        int16_t panelX = x + column;
        if (panelX < boundLeft || panelX >= boundRight) continue;

        for (int16_t row = 0; row < TEXT_CELL_HEIGHT; row++) {
            if (glyphStencil.getPixel(column, row)) {
                dma_display->drawPixel(panelX, y + row, color);
            }
        }
    }
}

// Draws text the way printTextWithSpecialChars does, confined to [boundLeft, boundRight).
// The panel has no clip rectangle, and the alternative - painting the row whole and wiping
// what overflows - leaves those pixels lit for as long as the rest of the row takes to draw,
// which the DMA scans out as an artefact over whatever the row slides behind.
void printTextWithinBounds(const char* text, int16_t x, int16_t y,
                           int16_t boundLeft, int16_t boundRight, uint32_t color,
                           const TextSegment* segments, uint8_t segmentCount) {
    uint32_t currentColor = (segmentCount > 0) ? segments[0].color : color;
    uint16_t color565 = dma_display->color565((currentColor >> 16) & 0xFF,
                                              (currentColor >> 8) & 0xFF,
                                              currentColor & 0xFF);
    dma_display->setTextColor(color565);

    int16_t cursorX = x;
    const uint8_t* ptr = (const uint8_t*)text;

    // Segment offsets are byte positions, as parseTextFieldWithSegments records them, and the
    // counter runs over the whole string: a glyph scrolled off the left edge or clipped by the
    // bounds still moves the next colour boundary to where the client asked for it
    uint8_t byteIndex = 0;
    uint8_t currentSegment = 0;

    // Advances exactly like printTextWithSpecialChars, so a scrolling string is drawn against
    // the width calculateTextWidth measured for it
    while (*ptr && cursorX < boundRight) {
        if (currentSegment + 1 < segmentCount && byteIndex >= segments[currentSegment + 1].offset) {
            currentSegment++;
            currentColor = segments[currentSegment].color;
            color565 = dma_display->color565((currentColor >> 16) & 0xFF,
                                             (currentColor >> 8) & 0xFF,
                                             currentColor & 0xFF);
            dma_display->setTextColor(color565);
        }

        if ((*ptr == 0xC2 && *(ptr + 1) == 0xB0) || *ptr == 0xB0) {
            static const int8_t degreePixels[4][2] = {{1, 0}, {0, 1}, {2, 1}, {1, 2}};
            for (uint8_t i = 0; i < 4; i++) {
                int16_t pixelX = cursorX + degreePixels[i][0];
                if (pixelX >= boundLeft && pixelX < boundRight) {
                    dma_display->drawPixel(pixelX, y - 6 + degreePixels[i][1], color565);
                }
            }
            uint8_t degreeByteCount = (*ptr == 0xB0) ? 1 : 2;
            ptr += degreeByteCount;
            byteIndex += degreeByteCount;
            cursorX += 4;
            continue;
        }

        char character;
        if (*ptr == 0xC3 && *(ptr + 1)) {
            character = utf8FrenchToAscii(*ptr, *(ptr + 1));
            ptr += 2;
            byteIndex += 2;
        } else if (*ptr >= 32 && *ptr <= 126) {
            character = (char)*ptr;
            ptr++;
            byteIndex++;
        } else {
            ptr++;  // Dropped, and measured as dropped
            byteIndex++;
            continue;
        }

        if (cursorX >= boundLeft && cursorX + TEXT_CELL_WIDTH <= boundRight) {
            dma_display->setCursor(cursorX, y);
            dma_display->print(character);
        } else if (cursorX + TEXT_CELL_WIDTH > boundLeft) {
            drawCharacterWithinBounds(character, cursorX, y, boundLeft, boundRight, color565);
        }

        cursorX += TEXT_CELL_WIDTH;
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
    dma_display->setCursor(x, y);

    uint32_t currentColor = (segmentCount > 0) ? segments[0].color : defaultColor;
    uint8_t r = (currentColor >> 16) & 0xFF;
    uint8_t g = (currentColor >> 8) & 0xFF;
    uint8_t b = currentColor & 0xFF;
    if (segmentCount == 0 && dimDefault) {
        r = r * 3 / 4;
        g = g * 3 / 4;
        b = b * 3 / 4;
    }
    dma_display->setTextColor(dma_display->color565(r, g, b));

    // Segment offsets are byte positions, as parseTextFieldWithSegments records them
    uint8_t byteIndex = 0;
    uint8_t currentSegment = 0;
    const uint8_t* ptr = (const uint8_t*)text;

    while (*ptr) {
        if (currentSegment + 1 < segmentCount && byteIndex >= segments[currentSegment + 1].offset) {
            currentSegment++;
            currentColor = segments[currentSegment].color;
            r = (currentColor >> 16) & 0xFF;
            g = (currentColor >> 8) & 0xFF;
            b = currentColor & 0xFF;
            dma_display->setTextColor(dma_display->color565(r, g, b));
        }

        uint8_t c = *ptr;

        // TomThumb covers printable ASCII only, so an accented name arrives transliterated
        if (c == 0xC3 && *(ptr + 1)) {
            dma_display->print(utf8FrenchToAscii(c, *(ptr + 1)));
            byteIndex += 2;
            ptr += 2;
            continue;
        }

        if (c >= 0x20 && c <= 0x7E) {
            dma_display->print((char)c);
        }
        byteIndex++;
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

// Cache-only lookup, for callers that run every frame: a miss must not cost a filesystem
// read or a LaMetric download. Refreshes lastUsed so a displayed icon is not the first
// candidate for eviction.
CachedIcon* getCachedIcon(const char* name) {
    if (!name || strlen(name) == 0) return nullptr;

    for (uint8_t i = 0; i < MAX_ICON_CACHE; i++) {
        if (iconCache[i].valid && strcmp(iconCache[i].name, name) == 0) {
            iconCache[i].lastUsed = millis();
            return &iconCache[i];
        }
    }

    return nullptr;
}

CachedIcon* getIcon(const char* name) {
    if (!name || strlen(name) == 0) return nullptr;

    CachedIcon* cached = getCachedIcon(name);
    if (cached) return cached;

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

            // Former name of staleAfter, still accepted
            uint32_t staleAfterSeconds = doc["lifetime"] | 0;
            if (!doc["staleAfter"].isNull()) {
                staleAfterSeconds = doc["staleAfter"].as<uint32_t>();
            }

            StaleBehavior staleBehavior = STALE_HIDE;
            if (!doc["staleBehavior"].isNull() &&
                !parseStaleBehavior(doc["staleBehavior"], &staleBehavior)) {
                request->send(400, "application/json",
                    "{\"error\":\"staleBehavior must be hide, dim, badge or none\"}");
                return;
            }
            if (staleBehavior == STALE_DIM || staleBehavior == STALE_BADGE) {
                request->send(400, "application/json",
                    "{\"error\":\"staleBehavior dim and badge are only supported on tracker and gauge apps\"}");
                return;
            }

            int8_t result = appAdd(name.c_str(), parsedText, icon, textColor, duration,
                                   staleAfterSecondsToMillis(staleAfterSeconds), false);

            if (result >= 0) {
                apps[result].staleBehavior = staleBehavior;
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

            // Everything is validated before anything is applied: a payload that
            // gets a 400 must leave the panel and the stored settings untouched
            JsonObject ntpUpdate;
            if (doc["ntp"].is<JsonObject>()) {
                ntpUpdate = doc["ntp"].as<JsonObject>();
            }

            bool hasWeatherDuration = !doc["weatherDuration"].isNull();
            uint32_t requestedWeatherDuration = 0;
            if (hasWeatherDuration) {
                requestedWeatherDuration = doc["weatherDuration"].as<uint32_t>();
                if (requestedWeatherDuration < (uint32_t)MIN_WEATHER_DURATION ||
                    requestedWeatherDuration > (uint32_t)MAX_WEATHER_DURATION) {
                    char errorMessage[96];
                    snprintf(errorMessage, sizeof(errorMessage),
                             "{\"error\":\"weatherDuration must be between %d and %d ms\"}",
                             MIN_WEATHER_DURATION, MAX_WEATHER_DURATION);
                    request->send(400, "application/json", errorMessage);
                    return;
                }
            }

            const char* requestedTzPosix = nullptr;
            if (!ntpUpdate.isNull() && ntpUpdate["tz_posix"].is<const char*>()) {
                requestedTzPosix = ntpUpdate["tz_posix"].as<const char*>();
                size_t tzLen = strlen(requestedTzPosix);
                if (tzLen == 0 || tzLen >= sizeof(settings.tzPosix)) {
                    request->send(400, "application/json", "{\"error\":\"tz_posix length invalid\"}");
                    return;
                }
            }

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
            if (hasWeatherDuration) {
                settings.weatherDuration = (uint16_t)requestedWeatherDuration;
                weatherClockApplyDuration(settings.weatherDuration);
            }

            bool ntpChanged = false;
            if (!ntpUpdate.isNull()) {
                if (!ntpUpdate["offset"].isNull() || !ntpUpdate["daylight_offset"].isNull()) {
                    Serial.println("[NTP] Legacy ntp.offset/daylight_offset ignored, use ntp.tz_posix");
                }
                if (requestedTzPosix) {
                    strlcpy(settings.tzPosix, requestedTzPosix, sizeof(settings.tzPosix));
                    Serial.printf("[NTP] tz_posix updated to %s\n", settings.tzPosix);
                    ntpChanged = true;
                }
                if (ntpUpdate["server"].is<const char*>()) {
                    strlcpy(settings.ntpServer, ntpUpdate["server"].as<const char*>(), sizeof(settings.ntpServer));
                    Serial.printf("[NTP] server updated to %s\n", settings.ntpServer);
                    ntpChanged = true;
                }
            }
            if (ntpChanged) {
                configTzTime(settings.tzPosix, settings.ntpServer);
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
            int8_t weatherClockIndex = appFind("weatherclock");
            const AppItem* weatherApp = weatherClockIndex >= 0 ? &apps[weatherClockIndex] : nullptr;
            doc["age"] = ageMs / 1000;
            doc["stale"] = appIsStale(weatherApp);
            if (weatherApp) {
                doc["staleAfter"] = weatherApp->staleAfter / 1000;
                doc["staleBehavior"] = staleBehaviorName(weatherApp->staleBehavior);
            }

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

            if (weatherData.today.hourCount > 0) {
                JsonObject today = doc["today"].to<JsonObject>();

                JsonArray hoursArr = today["hours"].to<JsonArray>();
                for (uint8_t i = 0; i < weatherData.today.hourCount; i++) {
                    JsonObject hourObj = hoursArr.add<JsonObject>();
                    hourObj["h"] = weatherData.today.hours[i].hour;
                    hourObj["temp"] = weatherData.today.hours[i].temp;
                    hourObj["pop"] = weatherData.today.hours[i].precipProbability;
                    hourObj["precip"] = weatherData.today.hours[i].precipTenthsOfMm;
                }

                JsonArray segmentsArr = today["segments"].to<JsonArray>();
                for (uint8_t i = 0; i < weatherData.today.segmentCount; i++) {
                    JsonObject segmentObj = segmentsArr.add<JsonObject>();
                    segmentObj["from"] = weatherData.today.segments[i].fromHour;
                    segmentObj["to"] = weatherData.today.segments[i].toHour;
                    segmentObj["icon"] = weatherData.today.segments[i].icon;
                }
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

            weatherParseTodayBlock(doc["today"].as<JsonObjectConst>());

            uint32_t staleAfter = WEATHER_DEFAULT_STALE_AFTER;
            if (!doc["staleAfter"].isNull()) {
                staleAfter = staleAfterSecondsToMillis(doc["staleAfter"].as<uint32_t>());
            }

            StaleBehavior staleBehavior = STALE_HIDE;
            if (!doc["staleBehavior"].isNull() &&
                !parseStaleBehavior(doc["staleBehavior"], &staleBehavior)) {
                request->send(400, "application/json",
                    "{\"error\":\"staleBehavior must be hide or none\"}");
                return;
            }
            if (staleBehavior == STALE_DIM || staleBehavior == STALE_BADGE) {
                request->send(400, "application/json",
                    "{\"error\":\"staleBehavior dim and badge are only supported on tracker and gauge apps\"}");
                return;
            }
            weatherClockApplyStalePolicy(staleAfter, staleBehavior);

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
                const AppItem* trackerApp = trackerFindApp(trackers[i].name);
                t["age"] = ageMs / 1000;
                t["stale"] = appIsStale(trackerApp);
                if (trackerApp) {
                    t["staleAfter"] = trackerApp->staleAfter / 1000;
                    t["staleBehavior"] = staleBehaviorName(trackerApp->staleBehavior);
                }
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
        char symbolColorHex[8];
        formatColorHex(tracker->symbolColor, symbolColorHex, sizeof(symbolColorHex));
        doc["symbolColor"] = symbolColorHex;
        char sparklineColorHex[8];
        formatColorHex(tracker->sparklineColor, sparklineColorHex, sizeof(sparklineColorHex));
        doc["sparklineColor"] = sparklineColorHex;
        JsonObject trackerObj = doc.as<JsonObject>();
        serializeTextField(trackerObj, "bottomText", tracker->bottomText,
                           tracker->bottomTextSegments, tracker->bottomTextSegmentCount);
        doc["sparklinePeriod"] = tracker->sparklinePeriod;
        doc["sparklineRef"] = (tracker->sparklineRef == TRACKER_REF_LAST) ? "last" : "open";

        unsigned long ageMs = millis() - tracker->lastUpdate;
        const AppItem* trackerApp = trackerFindApp(tracker->name);
        doc["age"] = ageMs / 1000;
        doc["stale"] = appIsStale(trackerApp);
        if (trackerApp) {
            doc["staleAfter"] = trackerApp->staleAfter / 1000;
            doc["staleBehavior"] = staleBehaviorName(trackerApp->staleBehavior);
        }

        if (tracker->sparklineCount > 0) {
            JsonArray sparkArr = doc["sparkline"].to<JsonArray>();
            for (uint8_t i = 0; i < tracker->sparklineCount; i++) {
                sparkArr.add(tracker->sparkline[i]);
            }
        }

        if (tracker->volumeCount > 0) {
            JsonArray volumeArr = doc["volumes"].to<JsonArray>();
            for (uint8_t i = 0; i < tracker->volumeCount; i++) {
                volumeArr.add(tracker->volumes[i]);
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

            trackerApplyJsonFields(tracker, doc);

            tracker->lastUpdate = millis();

            uint32_t staleAfter = TRACKER_DEFAULT_STALE_AFTER;
            if (!doc["staleAfter"].isNull()) {
                staleAfter = staleAfterSecondsToMillis(doc["staleAfter"].as<uint32_t>());
            }

            StaleBehavior staleBehavior = STALE_DIM;
            if (!doc["staleBehavior"].isNull() &&
                !parseStaleBehavior(doc["staleBehavior"], &staleBehavior)) {
                request->send(400, "application/json",
                    "{\"error\":\"staleBehavior must be hide, dim, badge or none\"}");
                return;
            }

            // Register/update app in rotation
            char appId[32];
            snprintf(appId, sizeof(appId), "%s%s", TRACKER_ID_PREFIX, name.c_str());
            uint16_t duration = doc["duration"] | (uint16_t)DEFAULT_APP_DURATION;
            int8_t appIndex = appAdd(appId, tracker->symbol, tracker->icon, 0xFFFFFF,
                                     duration, staleAfter, false);
            if (appIndex >= 0) {
                apps[appIndex].staleBehavior = staleBehavior;
            }

            Serial.printf("[TRACKER] Updated: %s (%s = %.2f)\n",
                         name.c_str(), tracker->symbol, tracker->currentValue);
            request->send(200, "application/json", "{\"success\":true}");
        });
    webServer.addHandler(trackerHandler);

    // ========================================================================
    // Gauge API
    // ========================================================================

    // GET /api/gauges - List all active gauges
    webServer.on("/api/gauges", HTTP_GET, [](AsyncWebServerRequest *request) {
        JsonDocument doc;
        JsonArray arr = doc["gauges"].to<JsonArray>();

        for (uint8_t i = 0; i < MAX_GAUGE_APPS; i++) {
            if (gauges[i].valid) {
                JsonObject g = arr.add<JsonObject>();
                g["name"] = gauges[i].name;
                g["title"] = gauges[i].title;
                g["rowCount"] = gauges[i].rowCount;
                unsigned long ageMs = millis() - gauges[i].lastUpdate;
                const AppItem* gaugeApp = gaugeFindApp(gauges[i].name);
                g["age"] = ageMs / 1000;
                g["stale"] = appIsStale(gaugeApp);
                if (gaugeApp) {
                    g["staleAfter"] = gaugeApp->staleAfter / 1000;
                    g["staleBehavior"] = staleBehaviorName(gaugeApp->staleBehavior);
                }
            }
        }
        doc["count"] = gaugeCount;

        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    // GET /api/gauge?name=claude - Get single gauge data
    webServer.on("/api/gauge", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("name")) {
            request->send(400, "application/json", "{\"error\":\"Missing gauge name\"}");
            return;
        }

        String name = request->getParam("name")->value();
        GaugeData* gauge = gaugeFind(name.c_str());

        if (!gauge) {
            request->send(404, "application/json", "{\"error\":\"Gauge not found\"}");
            return;
        }

        JsonDocument doc;
        doc["name"] = gauge->name;
        JsonObject gaugeObject = doc.as<JsonObject>();
        serializeTextField(gaugeObject, "title", gauge->title,
                           gauge->titleSegments, gauge->titleSegmentCount);
        doc["icon"] = gauge->icon;

        JsonArray rowsArray = doc["rows"].to<JsonArray>();
        for (uint8_t i = 0; i < gauge->rowCount; i++) {
            const GaugeRow* row = &gauge->rows[i];
            JsonObject rowObject = rowsArray.add<JsonObject>();
            serializeTextField(rowObject, "label", row->label,
                               row->labelSegments, row->labelSegmentCount);
            rowObject["info"] = row->info;
            rowObject["value"] = row->value;
            rowObject["note"] = row->note;
            rowObject["percent"] = row->percent;
            char barColorHex[8];
            formatColorHex(row->barColor, barColorHex, sizeof(barColorHex));
            rowObject["color"] = barColorHex;
            char noteColorHex[8];
            formatColorHex(row->noteColor, noteColorHex, sizeof(noteColorHex));
            rowObject["noteColor"] = noteColorHex;
        }

        unsigned long ageMs = millis() - gauge->lastUpdate;
        const AppItem* gaugeApp = gaugeFindApp(gauge->name);
        doc["age"] = ageMs / 1000;
        doc["stale"] = appIsStale(gaugeApp);
        if (gaugeApp) {
            doc["staleAfter"] = gaugeApp->staleAfter / 1000;
            doc["staleBehavior"] = staleBehaviorName(gaugeApp->staleBehavior);
        }

        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    // DELETE /api/gauge?name=claude - Remove gauge
    webServer.on("/api/gauge", HTTP_DELETE, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("name")) {
            request->send(400, "application/json", "{\"error\":\"Missing gauge name\"}");
            return;
        }

        String name = request->getParam("name")->value();
        if (gaugeRemove(name.c_str())) {
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            request->send(404, "application/json", "{\"error\":\"Gauge not found\"}");
        }
    });

    // POST /api/gauge?name=claude - Create/update gauge
    AsyncCallbackJsonWebHandler* gaugeHandler = new AsyncCallbackJsonWebHandler("/api/gauge",
        [](AsyncWebServerRequest *request, JsonVariant &json) {
            Serial.println("[API] /gauge handler called");
            JsonObject doc = json.as<JsonObject>();

            if (doc.isNull()) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            // Get gauge name from query param or JSON
            String name;
            if (request->hasParam("name")) {
                name = request->getParam("name")->value();
            } else if (!doc["name"].isNull()) {
                name = doc["name"].as<String>();
            } else {
                request->send(400, "application/json", "{\"error\":\"Missing gauge name\"}");
                return;
            }

            // A name longer than the store holds would be truncated there but not in the app id
            // built below, and the two would stop matching: the gauge would never be found again,
            // neither to draw it nor to delete it, while still holding one of the two slots.
            char errorMessage[64];
            if (!gaugeNameFits(name.c_str())) {
                snprintf(errorMessage, sizeof(errorMessage),
                         "{\"error\":\"Gauge name too long (max %d)\"}",
                         (int)sizeof(GaugeData::name) - 1);
                request->send(400, "application/json", errorMessage);
                return;
            }

            if (!gaugeRowCountFits(doc)) {
                snprintf(errorMessage, sizeof(errorMessage),
                         "{\"error\":\"Too many rows (max %d)\"}", MAX_GAUGE_ROWS);
                request->send(400, "application/json", errorMessage);
                return;
            }

            // Everything the request can be refused on is checked before a slot is taken, so a
            // rejected POST leaves the device exactly as it found it
            StaleBehavior staleBehavior = STALE_DIM;
            if (!doc["staleBehavior"].isNull() &&
                !parseStaleBehavior(doc["staleBehavior"], &staleBehavior)) {
                request->send(400, "application/json",
                    "{\"error\":\"staleBehavior must be hide, dim, badge or none\"}");
                return;
            }

            uint32_t staleAfter = GAUGE_DEFAULT_STALE_AFTER;
            if (!doc["staleAfter"].isNull()) {
                staleAfter = staleAfterSecondsToMillis(doc["staleAfter"].as<uint32_t>());
            }

            // Allocate or find existing gauge
            GaugeData* gauge = gaugeAllocate(name.c_str());
            if (!gauge) {
                request->send(500, "application/json", "{\"error\":\"No gauge slot available\"}");
                return;
            }

            gaugeApplyJsonFields(gauge, doc);
            gauge->lastUpdate = millis();

            // Register/update app in rotation
            char appId[32];
            snprintf(appId, sizeof(appId), "%s%s", GAUGE_ID_PREFIX, name.c_str());
            uint16_t duration = doc["duration"] | (uint16_t)DEFAULT_APP_DURATION;
            int8_t appIndex = appAdd(appId, gauge->title, gauge->icon, 0xFFFFFF,
                                     duration, staleAfter, false);
            if (appIndex >= 0) {
                apps[appIndex].staleBehavior = staleBehavior;
            }

            Serial.printf("[GAUGE] Updated: %s (%s, %d rows)\n",
                         name.c_str(), gauge->title, gauge->rowCount);
            request->send(200, "application/json", "{\"success\":true}");
        });
    webServer.addHandler(gaugeHandler);

    // ========================================================================
    // Sleep API
    // ========================================================================

    webServer.on("/api/sleep", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        Serial.println("[API] /sleep GET");

        JsonDocument doc;
        bool active = sleepIsActive();
        doc["sleeping"] = active;
        if (active)
        {
            doc["reason"] = sleepReasonToString(lastSleepReason);
            if (lastSleepReason == SLEEP_REASON_OVERRIDE)
            {
                doc["until"] = settings.sleep.sleepUntilEpoch;
            }
        }
        else if (lastSleepReason == SLEEP_REASON_NTP_NOT_SYNCED)
        {
            doc["reason"] = sleepReasonToString(lastSleepReason);
        }
        buildSleepConfigJson(doc["config"].to<JsonObject>());

        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    AsyncCallbackJsonWebHandler *sleepHandler = new AsyncCallbackJsonWebHandler(
        "/api/sleep",
        [](AsyncWebServerRequest *request, JsonVariant &json)
        {
            Serial.println("[API] /sleep POST");

            JsonObject body = json.as<JsonObject>();
            if (body.isNull())
            {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }

            String errorMessage;
            if (!applySleepUpdate(body, errorMessage))
            {
                JsonDocument errorDoc;
                errorDoc["error"] = errorMessage;
                String output;
                serializeJson(errorDoc, output);
                request->send(400, "application/json", output);
                return;
            }
            request->send(200, "application/json", "{\"success\":true}");
        });
    webServer.addHandler(sleepHandler);

    webServer.on("/api/sleep/wake", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        Serial.println("[API] /sleep/wake POST");
        wakeNow();
        request->send(200, "application/json", "{\"success\":true}");
    });

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
        if (method == HTTP_DELETE_METHOD && url == "/api/gauge") {
            if (!request->hasParam("name")) {
                request->send(400, "application/json", "{\"error\":\"Missing gauge name\"}");
                return;
            }
            String name = request->getParam("name")->value();
            if (gaugeRemove(name.c_str())) {
                request->send(200, "application/json", "{\"success\":true}");
            } else {
                request->send(404, "application/json", "{\"error\":\"Gauge not found\"}");
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
    doc["weatherDuration"] = settings.weatherDuration;
    doc["display"]["width"] = DISPLAY_WIDTH;
    doc["display"]["height"] = DISPLAY_HEIGHT;
    doc["ntp"]["server"] = settings.ntpServer;
    doc["ntp"]["tz_posix"] = settings.tzPosix;
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
            appObj["staleAfter"] = apps[i].staleAfter / 1000;
            appObj["staleBehavior"] = staleBehaviorName(apps[i].staleBehavior);
            appObj["stale"] = appIsStale(&apps[i]);
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
    if (!settings.mqttEnabled || strlen(settings.mqttServer) == 0) {
        Serial.println("[MQTT] Disabled or no server configured");
        return;
    }

    mqttClient.setServer(settings.mqttServer, settings.mqttPort);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE);

    Serial.printf("[MQTT] Configured: %s:%d (prefix: %s)\n",
                  settings.mqttServer, settings.mqttPort, settings.mqttPrefix);

    mqttConnect();
}

bool mqttConnect() {
    if (!settings.mqttEnabled || strlen(settings.mqttServer) == 0) {
        return false;
    }

    // Build LWT topic
    char lwtTopic[96];
    snprintf(lwtTopic, sizeof(lwtTopic), "%s%s", settings.mqttPrefix, MQTT_TOPIC_STATUS);

    // Build unique client ID from MAC address
    char clientId[32];
    uint64_t mac = ESP.getEfuseMac();
    snprintf(clientId, sizeof(clientId), "pixelcast_%04X%08X",
             (uint16_t)(mac >> 32), (uint32_t)mac);

    Serial.printf("[MQTT] Connecting as %s...\n", clientId);

    bool connected = false;
    if (strlen(settings.mqttUser) > 0) {
        connected = mqttClient.connect(clientId, settings.mqttUser, settings.mqttPassword,
                                       lwtTopic, 0, true, "offline");
    } else {
        connected = mqttClient.connect(clientId, lwtTopic, 0, true, "offline");
    }

    if (connected) {
        mqttConnected = true;
        lastMqttReconnectAttempt = millis();

        // Publish online status (retained)
        mqttClient.publish(lwtTopic, "online", true);

        // Subscribe to all topics under prefix
        char subscribeTopic[96];
        snprintf(subscribeTopic, sizeof(subscribeTopic), "%s/#", settings.mqttPrefix);
        mqttClient.subscribe(subscribeTopic);

        Serial.printf("[MQTT] Connected, subscribed to %s\n", subscribeTopic);
        return true;
    } else {
        mqttConnected = false;
        Serial.printf("[MQTT] Connection failed, rc=%d\n", mqttClient.state());
        lastMqttReconnectAttempt = millis();
        return false;
    }
}

void loopMQTT() {
    if (!settings.mqttEnabled || !wifiConnected) return;

    if (mqttClient.connected()) {
        mqttClient.loop();

        // Periodic stats publish
        if (millis() - lastStatsPublish > MQTT_STATS_INTERVAL) {
            mqttPublishStats();
            lastStatsPublish = millis();
        }
    } else {
        mqttConnected = false;

        // Reconnect with delay
        if (millis() - lastMqttReconnectAttempt > MQTT_RECONNECT_DELAY) {
            Serial.println("[MQTT] Attempting reconnection...");
            mqttConnect();
        }
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[MQTT] Message on topic: %s (%d bytes)\n", topic, length);

    // Strip prefix to get relative topic
    size_t prefixLen = strlen(settings.mqttPrefix);
    if (strncmp(topic, settings.mqttPrefix, prefixLen) != 0) {
        Serial.println("[MQTT] Ignoring message outside prefix");
        return;
    }
    const char* relativeTopic = topic + prefixLen;

    // Ignore outgoing-only topics
    if (strcmp(relativeTopic, MQTT_TOPIC_STATS) == 0 ||
        strcmp(relativeTopic, MQTT_TOPIC_STATUS) == 0) {
        return;
    }

    // Topics that need no JSON payload
    if (strcmp(relativeTopic, MQTT_TOPIC_DISMISS) == 0) {
        mqttHandleDismiss();
        return;
    }
    if (strcmp(relativeTopic, MQTT_TOPIC_REBOOT) == 0) {
        mqttHandleReboot();
        return;
    }
    if (strcmp(relativeTopic, MQTT_TOPIC_WAKE) == 0) {
        wakeNow();
        return;
    }

    // Parse JSON payload for all other topics
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    if (error) {
        Serial.printf("[MQTT] JSON parse error: %s\n", error.c_str());
        return;
    }
    JsonObject obj = doc.as<JsonObject>();

    // Route to handler based on relative topic
    if (strcmp(relativeTopic, MQTT_TOPIC_NOTIFY) == 0) {
        mqttHandleNotify(obj);
    } else if (strcmp(relativeTopic, MQTT_TOPIC_BRIGHTNESS) == 0) {
        mqttHandleBrightness(obj);
    } else if (strcmp(relativeTopic, MQTT_TOPIC_SETTINGS) == 0) {
        mqttHandleSettings(obj);
    } else if (strcmp(relativeTopic, MQTT_TOPIC_WEATHER) == 0) {
        mqttHandleWeather(obj);
    } else if (strcmp(relativeTopic, MQTT_TOPIC_CUSTOM) == 0) {
        // /custom with name in JSON body
        const char* name = obj["name"] | "";
        if (strlen(name) > 0) {
            if (obj["delete"] | false) {
                mqttHandleCustomDelete(name);
            } else {
                mqttHandleCustom(name, obj);
            }
        } else {
            Serial.println("[MQTT] /custom missing name");
        }
    } else if (strncmp(relativeTopic, MQTT_TOPIC_CUSTOM "/", strlen(MQTT_TOPIC_CUSTOM) + 1) == 0) {
        // /custom/{name}
        const char* name = relativeTopic + strlen(MQTT_TOPIC_CUSTOM) + 1;
        if (strlen(name) > 0) {
            if (obj["delete"] | false) {
                mqttHandleCustomDelete(name);
            } else {
                mqttHandleCustom(name, obj);
            }
        }
    } else if (strcmp(relativeTopic, MQTT_TOPIC_TRACKER) == 0) {
        // /tracker with name in JSON body
        const char* name = obj["name"] | "";
        if (strlen(name) > 0) {
            if (obj["delete"] | false) {
                mqttHandleTrackerDelete(name);
            } else {
                mqttHandleTracker(name, obj);
            }
        } else {
            Serial.println("[MQTT] /tracker missing name");
        }
    } else if (strncmp(relativeTopic, MQTT_TOPIC_TRACKER "/", strlen(MQTT_TOPIC_TRACKER) + 1) == 0) {
        // /tracker/{name}
        const char* name = relativeTopic + strlen(MQTT_TOPIC_TRACKER) + 1;
        if (strlen(name) > 0) {
            if (obj["delete"] | false) {
                mqttHandleTrackerDelete(name);
            } else {
                mqttHandleTracker(name, obj);
            }
        }
    } else if (strcmp(relativeTopic, MQTT_TOPIC_GAUGE) == 0) {
        // /gauge with name in JSON body
        const char* name = obj["name"] | "";
        if (strlen(name) > 0) {
            if (obj["delete"] | false) {
                mqttHandleGaugeDelete(name);
            } else {
                mqttHandleGauge(name, obj);
            }
        } else {
            Serial.println("[MQTT] /gauge missing name");
        }
    } else if (strncmp(relativeTopic, MQTT_TOPIC_GAUGE "/", strlen(MQTT_TOPIC_GAUGE) + 1) == 0) {
        // /gauge/{name}
        const char* name = relativeTopic + strlen(MQTT_TOPIC_GAUGE) + 1;
        if (strlen(name) > 0) {
            if (obj["delete"] | false) {
                mqttHandleGaugeDelete(name);
            } else {
                mqttHandleGauge(name, obj);
            }
        }
    } else if (strncmp(relativeTopic, MQTT_TOPIC_INDICATOR, strlen(MQTT_TOPIC_INDICATOR)) == 0) {
        // /indicator1, /indicator2, /indicator3
        const char* indexStr = relativeTopic + strlen(MQTT_TOPIC_INDICATOR);
        int idx = atoi(indexStr);
        if (idx >= 1 && idx <= NUM_INDICATORS) {
            mqttHandleIndicator(idx - 1, obj);
        } else {
            Serial.printf("[MQTT] Invalid indicator index: %s\n", indexStr);
        }
    } else if (strcmp(relativeTopic, MQTT_TOPIC_SLEEP) == 0) {
        if (!obj["until"].is<unsigned long>() && !obj["until"].is<long>()) {
            Serial.println("[MQTT] /sleep payload missing or non-integer 'until'");
            return;
        }
        uint32_t requestedUntil = obj["until"].as<uint32_t>();
        JsonDocument overrideDoc;
        JsonObject overrideBody = overrideDoc.to<JsonObject>();
        overrideBody["sleep_until"] = requestedUntil;
        String errorMessage;
        if (!applySleepUpdate(overrideBody, errorMessage)) {
            Serial.printf("[MQTT] /sleep rejected (until=%lu): %s\n",
                          (unsigned long)requestedUntil, errorMessage.c_str());
        }
    } else {
        Serial.printf("[MQTT] Unknown topic: %s\n", relativeTopic);
    }
}

void mqttPublishStats() {
    if (!mqttConnected) return;

    JsonDocument doc;
    doc["uptime"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["brightness"] = currentBrightness;
    doc["rssi"] = WiFi.RSSI();
    doc["appCount"] = appCount;
    doc["version"] = VERSION_STRING;

    if (currentAppIndex >= 0 && currentAppIndex < appCount) {
        doc["currentApp"] = apps[currentAppIndex].id;
    }

    char fullTopic[96];
    snprintf(fullTopic, sizeof(fullTopic), "%s%s", settings.mqttPrefix, MQTT_TOPIC_STATS);

    String payload;
    serializeJson(doc, payload);
    mqttClient.publish(fullTopic, payload.c_str());
}

// --- MQTT Topic Handlers ---

void mqttHandleBrightness(JsonObject& doc) {
    if (doc["brightness"].isNull()) {
        Serial.println("[MQTT] /brightness missing brightness value");
        return;
    }

    uint8_t brightness = doc["brightness"].as<uint8_t>();
    displaySetBrightness(brightness);
    settings.brightness = brightness;
    saveSettings();
    Serial.printf("[MQTT] Brightness set to %d\n", brightness);
}

void mqttHandleCustom(const char* name, JsonObject& doc) {
    // Check for multi-zone format
    JsonArray zonesArray = doc["zones"].as<JsonArray>();
    bool isMultiZone = !zonesArray.isNull() && zonesArray.size() > 0;

    if (isMultiZone) {
        uint8_t zoneCount = zonesArray.size();
        if (zoneCount == 1 || zoneCount > MAX_ZONES) {
            Serial.println("[MQTT] /custom zones array must have 2, 3, or 4 elements");
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

    // Former name of staleAfter, still accepted
    uint32_t staleAfterSeconds = doc["lifetime"] | 0;
    if (!doc["staleAfter"].isNull()) {
        staleAfterSeconds = doc["staleAfter"].as<uint32_t>();
    }

    StaleBehavior staleBehavior = STALE_HIDE;
    if (!doc["staleBehavior"].isNull() &&
        !parseStaleBehavior(doc["staleBehavior"], &staleBehavior)) {
        Serial.println("[MQTT] Unknown staleBehavior, keeping hide");
    }
    if (staleBehavior == STALE_DIM || staleBehavior == STALE_BADGE) {
        Serial.println("[MQTT] staleBehavior dim and badge need a tracker or gauge app, keeping hide");
        staleBehavior = STALE_HIDE;
    }

    int8_t result = appAdd(name, parsedText, icon, textColor, duration,
                           staleAfterSecondsToMillis(staleAfterSeconds), false);

    if (result >= 0) {
        apps[result].staleBehavior = staleBehavior;
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
        Serial.printf("[MQTT] Custom app '%s' created/updated\n", name);
    } else {
        Serial.printf("[MQTT] Failed to add custom app '%s'\n", name);
    }
}

void mqttHandleCustomDelete(const char* name) {
    if (appRemove(name)) {
        Serial.printf("[MQTT] Custom app '%s' deleted\n", name);
    } else {
        Serial.printf("[MQTT] Custom app '%s' not found or is system app\n", name);
    }
}

void mqttHandleNotify(JsonObject& doc) {
    const char* text = doc["text"] | "";
    if (strlen(text) == 0) {
        Serial.println("[MQTT] /notify missing text");
        return;
    }

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

    if (slot >= 0) {
        Serial.printf("[MQTT] Notification added: '%s'\n", text);
    } else {
        Serial.println("[MQTT] Notification queue full");
    }
}

void mqttHandleDismiss() {
    if (notifDismiss()) {
        resetNotifScrollState();
        Serial.println("[MQTT] Notification dismissed");
    } else {
        Serial.println("[MQTT] No active notification to dismiss");
    }
}

void mqttHandleIndicator(uint8_t index, JsonObject& doc) {
    if (index >= NUM_INDICATORS) return;

    // Parse mode string
    const char* modeStr = doc["mode"] | "";
    IndicatorMode mode = INDICATOR_OFF;

    if (strlen(modeStr) > 0) {
        if (strcmp(modeStr, "solid") == 0) mode = INDICATOR_SOLID;
        else if (strcmp(modeStr, "blink") == 0) mode = INDICATOR_BLINK;
        else if (strcmp(modeStr, "fade") == 0) mode = INDICATOR_FADE;
        else if (strcmp(modeStr, "off") == 0) mode = INDICATOR_OFF;
        else {
            Serial.printf("[MQTT] Invalid indicator mode: %s\n", modeStr);
            return;
        }
    } else if (!doc["color"].isNull()) {
        // Default to solid if color provided but no mode
        mode = INDICATOR_SOLID;
    }

    if (mode == INDICATOR_OFF) {
        indicatorOff(index);
        saveSettings();
        Serial.printf("[MQTT] Indicator %d turned off\n", index + 1);
        return;
    }

    uint32_t color = parseColorValue(doc["color"], indicators[index].color);
    uint16_t blinkInterval = doc["blinkInterval"] | (uint16_t)INDICATOR_BLINK_INTERVAL;
    uint16_t fadePeriod = doc["fadePeriod"] | (uint16_t)INDICATOR_FADE_PERIOD;

    indicatorSet(index, mode, color, blinkInterval, fadePeriod);
    saveSettings();

    Serial.printf("[MQTT] Indicator %d set: mode=%s color=0x%06X\n",
                  index + 1, modeStr[0] ? modeStr : "solid", color);
}

void mqttHandleWeather(JsonObject& doc) {
    // Parse current weather
    if (!doc["current"].is<JsonObject>()) {
        Serial.println("[MQTT] /weather missing 'current' object");
        return;
    }

    JsonObject current = doc["current"];
    strlcpy(weatherData.currentIcon, current["icon"] | "", sizeof(weatherData.currentIcon));
    weatherData.currentTemp = current["temp"] | 0;
    weatherData.currentTempMin = current["temp_min"] | 0;
    weatherData.currentTempMax = current["temp_max"] | 0;
    weatherData.currentHumidity = current["humidity"] | 0;

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

    weatherParseTodayBlock(doc["today"].as<JsonObjectConst>());

    uint32_t staleAfter = WEATHER_DEFAULT_STALE_AFTER;
    if (!doc["staleAfter"].isNull()) {
        staleAfter = staleAfterSecondsToMillis(doc["staleAfter"].as<uint32_t>());
    }

    StaleBehavior staleBehavior = STALE_HIDE;
    if (!doc["staleBehavior"].isNull() &&
        !parseStaleBehavior(doc["staleBehavior"], &staleBehavior)) {
        Serial.println("[MQTT] Unknown staleBehavior, keeping hide");
    }
    if (staleBehavior == STALE_DIM || staleBehavior == STALE_BADGE) {
        Serial.println("[MQTT] staleBehavior dim and badge need a tracker or gauge app, keeping hide");
        staleBehavior = STALE_HIDE;
    }
    weatherClockApplyStalePolicy(staleAfter, staleBehavior);

    // Reset forecast pagination on new data
    forecastPage = 0;
    lastForecastPageSwitch = millis();

    weatherData.lastUpdate = millis();
    weatherData.valid = true;

    Serial.printf("[MQTT] Weather updated: %d C, %d%% humidity\n",
                  weatherData.currentTemp, weatherData.currentHumidity);
}

void mqttHandleTracker(const char* name, JsonObject& doc) {
    // Allocate or find existing tracker
    TrackerData* tracker = trackerAllocate(name);
    if (!tracker) {
        Serial.printf("[MQTT] No tracker slot available for '%s'\n", name);
        return;
    }

    trackerApplyJsonFields(tracker, doc);

    tracker->lastUpdate = millis();

    uint32_t staleAfter = TRACKER_DEFAULT_STALE_AFTER;
    if (!doc["staleAfter"].isNull()) {
        staleAfter = staleAfterSecondsToMillis(doc["staleAfter"].as<uint32_t>());
    }

    StaleBehavior staleBehavior = STALE_DIM;
    if (!doc["staleBehavior"].isNull() &&
        !parseStaleBehavior(doc["staleBehavior"], &staleBehavior)) {
        Serial.println("[MQTT] Unknown staleBehavior, keeping dim");
    }

    // Register/update app in rotation
    char appId[32];
    snprintf(appId, sizeof(appId), "%s%s", TRACKER_ID_PREFIX, name);
    uint16_t duration = doc["duration"] | (uint16_t)DEFAULT_APP_DURATION;
    int8_t appIndex = appAdd(appId, tracker->symbol, tracker->icon, 0xFFFFFF,
                             duration, staleAfter, false);
    if (appIndex >= 0) {
        apps[appIndex].staleBehavior = staleBehavior;
    }

    Serial.printf("[MQTT] Tracker updated: %s (%s = %.2f)\n",
                  name, tracker->symbol, tracker->currentValue);
}

void mqttHandleTrackerDelete(const char* name) {
    if (trackerRemove(name)) {
        Serial.printf("[MQTT] Tracker '%s' deleted\n", name);
    } else {
        Serial.printf("[MQTT] Tracker '%s' not found\n", name);
    }
}

void mqttHandleGauge(const char* name, JsonObject& doc) {
    if (!gaugeNameFits(name)) {
        Serial.printf("[MQTT] Gauge '%s' rejected: name longer than %d characters\n",
                      name, (int)sizeof(GaugeData::name) - 1);
        return;
    }

    if (!gaugeRowCountFits(doc)) {
        Serial.printf("[MQTT] Gauge '%s' rejected: more than %d rows\n", name, MAX_GAUGE_ROWS);
        return;
    }

    // Allocate or find existing gauge
    GaugeData* gauge = gaugeAllocate(name);
    if (!gauge) {
        Serial.printf("[MQTT] No gauge slot available for '%s'\n", name);
        return;
    }

    gaugeApplyJsonFields(gauge, doc);
    gauge->lastUpdate = millis();

    uint32_t staleAfter = GAUGE_DEFAULT_STALE_AFTER;
    if (!doc["staleAfter"].isNull()) {
        staleAfter = staleAfterSecondsToMillis(doc["staleAfter"].as<uint32_t>());
    }

    StaleBehavior staleBehavior = STALE_DIM;
    if (!doc["staleBehavior"].isNull() &&
        !parseStaleBehavior(doc["staleBehavior"], &staleBehavior)) {
        Serial.println("[MQTT] Unknown staleBehavior, keeping dim");
    }

    // Register/update app in rotation
    char appId[32];
    snprintf(appId, sizeof(appId), "%s%s", GAUGE_ID_PREFIX, name);
    uint16_t duration = doc["duration"] | (uint16_t)DEFAULT_APP_DURATION;
    int8_t appIndex = appAdd(appId, gauge->title, gauge->icon, 0xFFFFFF,
                             duration, staleAfter, false);
    if (appIndex >= 0) {
        apps[appIndex].staleBehavior = staleBehavior;
    }

    Serial.printf("[MQTT] Gauge updated: %s (%s, %d rows)\n",
                  name, gauge->title, gauge->rowCount);
}

void mqttHandleGaugeDelete(const char* name) {
    if (gaugeRemove(name)) {
        Serial.printf("[MQTT] Gauge '%s' deleted\n", name);
    } else {
        Serial.printf("[MQTT] Gauge '%s' not found\n", name);
    }
}

void mqttHandleSettings(JsonObject& doc) {
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
    if (!doc["weatherDuration"].isNull()) {
        uint32_t requestedWeatherDuration = doc["weatherDuration"].as<uint32_t>();
        settings.weatherDuration = constrain(requestedWeatherDuration,
                                             (uint32_t)MIN_WEATHER_DURATION,
                                             (uint32_t)MAX_WEATHER_DURATION);
        weatherClockApplyDuration(settings.weatherDuration);
    }

    saveSettings();
    Serial.println("[MQTT] Settings updated");
}

void mqttHandleReboot() {
    Serial.println("[MQTT] Reboot requested");
    pendingReboot = true;
    rebootRequestTime = millis();
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
    settings.weatherDuration = DEFAULT_WEATHER_DURATION;

    strlcpy(settings.ntpServer, NTP_SERVER, sizeof(settings.ntpServer));
    strlcpy(settings.tzPosix, DEFAULT_TZ_POSIX, sizeof(settings.tzPosix));

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

    settings.sleep.enabled = false;
    settings.sleep.sleepUntilEpoch = 0;
    strlcpy(settings.sleep.displayMode, "black", sizeof(settings.sleep.displayMode));
    for (int day = 0; day < 7; day++) {
        settings.sleep.days[day].allDay = false;
        settings.sleep.days[day].slotCount = 0;
    }
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

    if (doc["ntp"]["tz_posix"].is<const char*>()) {
        strlcpy(settings.tzPosix, doc["ntp"]["tz_posix"].as<const char*>(), sizeof(settings.tzPosix));
    } else {
        strlcpy(settings.tzPosix, DEFAULT_TZ_POSIX, sizeof(settings.tzPosix));
        if (!doc["ntp"]["offset"].isNull()) {
            Serial.println("[NTP] Legacy ntp.offset ignored, applying default tz_posix");
        }
    }

    // WeatherClock app settings
    uint32_t storedWeatherDuration = doc["apps"]["weatherclock"]["duration"] | (uint32_t)DEFAULT_WEATHER_DURATION;
    settings.weatherDuration = constrain(storedWeatherDuration,
                                         (uint32_t)MIN_WEATHER_DURATION,
                                         (uint32_t)MAX_WEATHER_DURATION);

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

    // Sleep settings
    settings.sleep.enabled = doc["sleep"]["enabled"] | false;
    const char* sleepDisplayMode = doc["sleep"]["displayMode"] | "black";
    strlcpy(settings.sleep.displayMode, sleepDisplayMode, sizeof(settings.sleep.displayMode));
    settings.sleep.sleepUntilEpoch = doc["sleep"]["sleepUntilEpoch"] | 0;

    for (int day = 0; day < 7; day++) {
        String dayKey = String(day);
        JsonObject dayObj = doc["sleep"]["days"][dayKey];

        settings.sleep.days[day].allDay = false;
        settings.sleep.days[day].slotCount = 0;

        if (dayObj.isNull()) continue;

        settings.sleep.days[day].allDay = dayObj["allDay"] | false;

        JsonArray slotsArr = dayObj["slots"];
        uint8_t loadedSlotCount = 0;
        for (JsonObject slotObj : slotsArr) {
            if (loadedSlotCount >= MAX_SLOTS_PER_DAY) break;
            uint8_t startHour   = slotObj["startHour"]   | 0;
            uint8_t startMinute = slotObj["startMinute"] | 0;
            uint8_t endHour     = slotObj["endHour"]     | 0;
            uint8_t endMinute   = slotObj["endMinute"]   | 0;
            if (startHour > 23 || endHour > 23 || startMinute > 59 || endMinute > 59) {
                Serial.printf("[SLEEP] Skipping invalid slot day=%d (%02u:%02u-%02u:%02u)\n",
                              day, startHour, startMinute, endHour, endMinute);
                continue;
            }
            SleepSlot& slot = settings.sleep.days[day].slots[loadedSlotCount];
            slot.startHour   = startHour;
            slot.startMinute = startMinute;
            slot.endHour     = endHour;
            slot.endMinute   = endMinute;
            loadedSlotCount++;
        }
        settings.sleep.days[day].slotCount = loadedSlotCount;
    }

    Serial.printf("[SLEEP] Loaded schedule: enabled=%d, displayMode=%s, override=%u\n",
                  settings.sleep.enabled,
                  settings.sleep.displayMode,
                  settings.sleep.sleepUntilEpoch);

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
    doc["ntp"]["tz_posix"] = settings.tzPosix;

    // WeatherClock app settings
    doc["apps"]["weatherclock"]["duration"] = settings.weatherDuration;

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

    // Sleep settings
    doc["sleep"]["enabled"] = settings.sleep.enabled;
    doc["sleep"]["displayMode"] = settings.sleep.displayMode;
    doc["sleep"]["sleepUntilEpoch"] = settings.sleep.sleepUntilEpoch;

    for (int day = 0; day < 7; day++) {
        String dayKey = String(day);
        doc["sleep"]["days"][dayKey]["allDay"] = settings.sleep.days[day].allDay;

        JsonArray slotsArr = doc["sleep"]["days"][dayKey]["slots"].to<JsonArray>();
        for (uint8_t slotIndex = 0; slotIndex < settings.sleep.days[day].slotCount; slotIndex++) {
            const SleepSlot& slot = settings.sleep.days[day].slots[slotIndex];
            JsonObject slotObj = slotsArr.add<JsonObject>();
            slotObj["startHour"]   = slot.startHour;
            slotObj["startMinute"] = slot.startMinute;
            slotObj["endHour"]     = slot.endHour;
            slotObj["endMinute"]   = slot.endMinute;
        }
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
    //            settings.defaultDuration, 0, true);
    //     Serial.println("[APPS] Clock app added");
    // }
    //
    // if (settings.dateEnabled) {
    //     appAdd("date", "Date", "", settings.dateColor,
    //            settings.defaultDuration, 0, true);
    //     Serial.println("[APPS] Date app added");
    // }

    // WeatherClock system app (replaces clock+date when weather data is available)
    appAdd("weatherclock", "WeatherClock", "", settings.clockColor,
           settings.weatherDuration, 0, true);
    Serial.println("[APPS] WeatherClock app added");

    Serial.printf("[APPS] Initialized with %d apps\n", appCount);
    appRotationEnabled = settings.autoRotate;
}

int8_t appAdd(const char* id, const char* text, const char* icon,
              uint32_t textColor, uint16_t duration,
              uint32_t staleAfter, bool isSystem) {

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
        app->staleAfter = staleAfter;
        app->lastUpdate = millis();
        app->active = true;
        // Reset zone data (caller will set via appSetZones if needed)
        app->zoneCount = 0;
        memset(app->zones, 0, sizeof(app->zones));
        Serial.printf("[APPS] Updated app: %s\n", id);
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
    app->staleAfter = staleAfter;
    // Slots are reused, so this has to be set rather than inherited from the previous tenant.
    app->staleBehavior = STALE_HIDE;
    app->lastUpdate = millis();
    app->active = true;
    app->isSystem = isSystem;
    // Initialize zone data (caller will set via appSetZones if needed)
    app->zoneCount = 0;
    memset(app->zones, 0, sizeof(app->zones));

    appCount++;
    Serial.printf("[APPS] Added app: %s (slot %d, total %d)\n", id, emptySlot, appCount);

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
    app->lastUpdate = millis();

    Serial.printf("[APPS] Updated app: %s\n", id);
    return true;
}

void weatherClockApplyDuration(uint16_t durationMs)
{
    int8_t weatherClockIndex = appFind("weatherclock");
    if (weatherClockIndex < 0) return;

    apps[weatherClockIndex].duration = durationMs;
    Serial.printf("[APPS] WeatherClock duration set to %u ms\n", durationMs);
}

// The weather client pushes to /api/weather, which never goes through appAdd, so the app
// carrying its stale policy has to be refreshed here on every push.
void weatherClockApplyStalePolicy(uint32_t staleAfter, StaleBehavior staleBehavior)
{
    int8_t weatherClockIndex = appFind("weatherclock");
    if (weatherClockIndex < 0) return;

    apps[weatherClockIndex].staleAfter = staleAfter;
    apps[weatherClockIndex].staleBehavior = staleBehavior;
    apps[weatherClockIndex].lastUpdate = millis();
}

int8_t appFind(const char* id) {
    for (uint8_t i = 0; i < MAX_APPS; i++) {
        if (apps[i].active && strcmp(apps[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

void appHideStale() {
    for (uint8_t i = 0; i < MAX_APPS; i++) {
        AppItem* app = &apps[i];
        // System apps have no client of their own to bring them back, and dropping the last
        // one would leave the panel with nothing to draw, so they never leave the rotation.
        if (!app->active || app->isSystem) continue;
        if (app->staleBehavior != STALE_HIDE) continue;
        if (!appIsStale(app)) continue;

        Serial.printf("[APPS] App went stale, leaving rotation: %s\n", app->id);
        app->active = false;
        appCount--;
        if (currentAppIndex == i) {
            currentAppIndex = -1;
        }
    }
}

AppItem* appGetNext() {
    if (appCount == 0) return nullptr;

    appHideStale();
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
    if (sleepIsActive()) return;

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
            for (uint8_t paint = DISPLAY_BUFFER_COUNT; paint > 0; paint--) {
                displayShowNotification(currentNotif);
            }
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
            // Every buffer, otherwise the app underneath shows through on the next flip
            for (uint8_t paint = DISPLAY_BUFFER_COUNT; paint > 0; paint--) {
                displayShowNotification(currentNotif);
            }
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
        // Reset tracker cache too, otherwise the restored tracker screen stays blank
        trackerLastUpdateDrawn = 0;
        resetTrackerScrollStates();
        resetGaugeDisplayState();
        Serial.println("[NOTIF] All dismissed, resuming app rotation");
        // Wipe the notification pixels, then paint the app into every buffer
        displayClear();
        AppItem* restored = appGetCurrent();
        if (restored) {
            for (uint8_t paint = DISPLAY_BUFFER_COUNT; paint > 0; paint--) {
                displayShowApp(restored);
            }
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
// Sleep Mode
// ============================================================================

static bool sleepSlotMatches(const SleepSlot& slot, uint8_t hour, uint8_t minute) {
    uint16_t nowMinutes = (uint16_t)hour * 60 + minute;
    uint16_t startMinutes = (uint16_t)slot.startHour * 60 + slot.startMinute;
    uint16_t endMinutes = (uint16_t)slot.endHour * 60 + slot.endMinute;

    if (startMinutes == endMinutes) {
        return false;
    }
    if (endMinutes > startMinutes) {
        return nowMinutes >= startMinutes && nowMinutes < endMinutes;
    }
    return nowMinutes >= startMinutes || nowMinutes < endMinutes;
}

static bool sleepScheduleSaysActive(uint8_t wday, uint8_t hour, uint8_t minute) {
    const SleepDay& day = settings.sleep.days[wday];
    if (day.allDay) return true;

    for (uint8_t slotIndex = 0; slotIndex < day.slotCount; slotIndex++) {
        if (sleepSlotMatches(day.slots[slotIndex], hour, minute)) {
            return true;
        }
    }
    return false;
}

// SLEEP_REASON_NTP_NOT_SYNCED wins over `enabled` so the diagnostic
// stays accurate when the clock is unreliable. sleepUntilEpoch is a
// standard UTC epoch.
bool sleepIsActive() {
    time_t nowUtc = time(nullptr);

    if (nowUtc < NTP_VALID_EPOCH_THRESHOLD) {
        lastSleepReason = SLEEP_REASON_NTP_NOT_SYNCED;
        return false;
    }

    if (!settings.sleep.enabled) {
        lastSleepReason = SLEEP_REASON_NONE;
        return false;
    }

    if ((uint32_t)nowUtc < settings.sleep.sleepUntilEpoch) {
        lastSleepReason = SLEEP_REASON_OVERRIDE;
        return true;
    }

    struct tm localTm;
    localtime_r(&nowUtc, &localTm);
    uint8_t wday   = (uint8_t)localTm.tm_wday;
    uint8_t hour   = (uint8_t)localTm.tm_hour;
    uint8_t minute = (uint8_t)localTm.tm_min;

    if (sleepScheduleSaysActive(wday, hour, minute)) {
        lastSleepReason = SLEEP_REASON_SCHEDULE;
        return true;
    }

    lastSleepReason = SLEEP_REASON_NONE;
    return false;
}

// ============================================================================
// Display Loop
// ============================================================================

void loopSleepTransition()
{
    static bool wasSleeping = false;
    static uint8_t previousBrightness = DEFAULT_BRIGHTNESS;
    bool isSleeping = sleepIsActive();

    if (isSleeping && !wasSleeping) {
        Serial.printf("[SLEEP] entering at %u\n", (unsigned)time(nullptr));
        previousBrightness = currentBrightness;
        if (strcmp(settings.sleep.displayMode, "black") == 0) {
            displaySetBrightness(0);
            displayClear();
        } else if (strcmp(settings.sleep.displayMode, "clock") == 0) {
            // Every buffer, so the clock replaces any leftover frame immediately on entry
            for (uint8_t paint = DISPLAY_BUFFER_COUNT; paint > 0; paint--) {
                displayShowTime();
            }
            lastDisplayUpdate = millis();
        }
    } else if (!isSleeping && wasSleeping) {
        Serial.printf("[SLEEP] exiting at %u\n", (unsigned)time(nullptr));
        displaySetBrightness(previousBrightness);
        lastDisplayUpdate = 0;
        // The sleep clock overwrote the app frame, so a tracker or gauge screen has to repaint
        trackerLastUpdateDrawn = 0;
        resetTrackerScrollStates();
        resetGaugeDisplayState();
    }
    wasSleeping = isSleeping;
}

static const char* const CANONICAL_DAY_NAMES[7] = {
    "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"
};

static bool dayIndexFromName(const char* name, uint8_t& outIndex)
{
    if (name == nullptr) return false;
    for (uint8_t i = 0; i < 7; i++) {
        if (strcmp(name, CANONICAL_DAY_NAMES[i]) == 0) {
            outIndex = i;
            return true;
        }
    }
    return false;
}

static const char* dayNameFromIndex(uint8_t index)
{
    if (index >= 7) return "unknown";
    return CANONICAL_DAY_NAMES[index];
}

static bool parseHourMinute(const char* text, uint8_t& outHour, uint8_t& outMinute)
{
    if (text == nullptr) return false;
    if (strlen(text) != 5) return false;
    if (text[2] != ':') return false;
    if (!isdigit((unsigned char)text[0]) || !isdigit((unsigned char)text[1])) return false;
    if (!isdigit((unsigned char)text[3]) || !isdigit((unsigned char)text[4])) return false;

    uint8_t hour = (uint8_t)((text[0] - '0') * 10 + (text[1] - '0'));
    uint8_t minute = (uint8_t)((text[3] - '0') * 10 + (text[4] - '0'));
    if (hour > 23 || minute > 59) return false;

    outHour = hour;
    outMinute = minute;
    return true;
}

static void formatHourMinute(uint8_t hour, uint8_t minute, char* out, size_t outSize)
{
    if (out == nullptr || outSize < 6) return;
    snprintf(out, outSize, "%02u:%02u", (unsigned)hour, (unsigned)minute);
}

static const char* sleepReasonToString(SleepReason reason)
{
    switch (reason) {
        case SLEEP_REASON_SCHEDULE:       return "schedule";
        case SLEEP_REASON_OVERRIDE:       return "override";
        case SLEEP_REASON_NTP_NOT_SYNCED: return "ntp_not_synced";
        case SLEEP_REASON_NONE:
        default:
            return "none";
    }
}

static void buildSleepConfigJson(JsonObject root)
{
    root["enabled"] = settings.sleep.enabled;
    root["display_mode"] = settings.sleep.displayMode;

    JsonObject schedule = root["schedule"].to<JsonObject>();
    for (uint8_t i = 0; i < 7; i++) {
        JsonObject dayObj = schedule[dayNameFromIndex(i)].to<JsonObject>();
        dayObj["all_day"] = settings.sleep.days[i].allDay;
        JsonArray slots = dayObj["slots"].to<JsonArray>();
        for (uint8_t s = 0; s < settings.sleep.days[i].slotCount; s++) {
            char startStr[6];
            char endStr[6];
            formatHourMinute(settings.sleep.days[i].slots[s].startHour,
                             settings.sleep.days[i].slots[s].startMinute,
                             startStr, sizeof(startStr));
            formatHourMinute(settings.sleep.days[i].slots[s].endHour,
                             settings.sleep.days[i].slots[s].endMinute,
                             endStr, sizeof(endStr));
            JsonObject slot = slots.add<JsonObject>();
            slot["start"] = startStr;
            slot["end"] = endStr;
        }
    }

    root["sleep_until"] = settings.sleep.sleepUntilEpoch;
}

static bool applySleepUpdate(JsonObject body, String& errorOut)
{
    SleepSchedule scratch = settings.sleep;

    if (body["enabled"].is<bool>()) {
        scratch.enabled = body["enabled"].as<bool>();
    }

    if (!body["display_mode"].isNull()) {
        if (!body["display_mode"].is<const char*>()) {
            errorOut = "display_mode must be a string";
            return false;
        }
        const char* mode = body["display_mode"].as<const char*>();
        if (strcmp(mode, "black") != 0 && strcmp(mode, "clock") != 0) {
            errorOut = "Invalid display_mode";
            return false;
        }
        strlcpy(scratch.displayMode, mode, sizeof(scratch.displayMode));
    }

    if (!body["sleep_until"].isNull()) {
        if (!body["sleep_until"].is<unsigned long>() && !body["sleep_until"].is<long>()) {
            errorOut = "sleep_until must be an integer";
            return false;
        }
        uint32_t requested = body["sleep_until"].as<uint32_t>();
        if (requested != 0 && requested <= (uint32_t)time(nullptr)) {
            errorOut = "sleep_until is in the past";
            return false;
        }
        scratch.sleepUntilEpoch = requested;
    }

    if (body["schedule"].is<JsonObject>()) {
        JsonObject schedule = body["schedule"].as<JsonObject>();
        for (JsonPair kv : schedule) {
            uint8_t dayIndex = 0;
            if (!dayIndexFromName(kv.key().c_str(), dayIndex)) {
                errorOut = String("Unknown day: ") + kv.key().c_str();
                return false;
            }

            if (!kv.value().is<JsonObject>()) {
                errorOut = String("Invalid day entry: ") + kv.key().c_str();
                return false;
            }
            JsonObject dayBody = kv.value().as<JsonObject>();
            SleepDay& day = scratch.days[dayIndex];

            if (dayBody["all_day"].is<bool>()) {
                day.allDay = dayBody["all_day"].as<bool>();
            }

            if (dayBody["slots"].is<JsonArray>()) {
                JsonArray slotsArray = dayBody["slots"].as<JsonArray>();
                day.slotCount = 0;
                for (JsonObject slotObj : slotsArray) {
                    if (day.slotCount >= MAX_SLOTS_PER_DAY) {
                        errorOut = "Too many slots";
                        return false;
                    }
                    const char* startText = slotObj["start"].as<const char*>();
                    const char* endText = slotObj["end"].as<const char*>();
                    uint8_t startHour = 0, startMinute = 0, endHour = 0, endMinute = 0;
                    if (!parseHourMinute(startText, startHour, startMinute) ||
                        !parseHourMinute(endText, endHour, endMinute)) {
                        errorOut = "Invalid time format";
                        return false;
                    }
                    SleepSlot& slot = day.slots[day.slotCount];
                    slot.startHour = startHour;
                    slot.startMinute = startMinute;
                    slot.endHour = endHour;
                    slot.endMinute = endMinute;
                    day.slotCount++;
                }
            }
        }
    }

    settings.sleep = scratch;
    saveSettings();
    loopSleepTransition();
    return true;
}

static void wakeNow()
{
    settings.sleep.sleepUntilEpoch = 0;
    saveSettings();
    loopSleepTransition();
    Serial.println("[SLEEP] Wake override cleared");
}

void loopDisplay() {
    if (!wifiConnected) return;

    if (sleepIsActive()) {
        if (strcmp(settings.sleep.displayMode, "clock") == 0) {
            unsigned long sleepNow = millis();
            if (sleepNow - lastDisplayUpdate > 1000) {
                displayShowTime();
                lastDisplayUpdate = sleepNow;
            }
        }
        return;
    }

    unsigned long now = millis();
    bool needsRedraw = false;

    // ---- Notification display (priority over apps) ----
    NotificationItem* currentNotif = notifGetCurrent();
    if (currentNotif) {
        needsRedraw = scrollStateAdvance(notifScrollState, now);

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
    if (current) {
        needsRedraw = scrollStateAdvance(appScrollState, now);
        needsRedraw |= scrollStateAdvance(trackerSymbolScrollState, now);
        needsRedraw |= scrollStateAdvance(trackerBottomScrollState, now);
        needsRedraw |= scrollStateAdvance(gaugeTitleScrollState, now);
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
