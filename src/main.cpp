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

// MQTT
#include <PubSubClient.h>

// JSON
#include <ArduinoJson.h>

// Filesystem
#include <LittleFS.h>

// NTP
#include <NTPClient.h>
#include <WiFiUdp.h>

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

// Application Manager
AppItem apps[MAX_APPS];
uint8_t appCount = 0;
int8_t currentAppIndex = -1;
unsigned long lastAppSwitch = 0;
bool appRotationEnabled = true;

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

// Timing
unsigned long lastStatsPublish = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastTimeUpdate = 0;

// Request body buffer for async handling
static char requestBodyBuffer[1024];
static size_t requestBodyLength = 0;

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
void displayClear();
void displaySetBrightness(uint8_t brightness);

bool loadSettings();
bool saveSettings();
void initDefaultSettings();
bool ensureDirectories();

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

        Serial.println("[INIT] Setting up NTP...");
        timeClient.setPoolServerName(settings.ntpServer);
        timeClient.setTimeOffset(settings.ntpOffset);
        timeClient.begin();

        displayShowIP();
        delay(2000);

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
    dma_display->setTextColor(dma_display->color565(0, 255, 0));
    dma_display->setTextSize(1);
    dma_display->setCursor(2, 20);
    dma_display->print("WiFi OK");

    dma_display->setTextColor(dma_display->color565(255, 255, 255));
    dma_display->setCursor(2, 36);
    String ip = WiFi.localIP().toString();
    dma_display->print(ip);

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

    // Custom apps
    dma_display->clearScreen();

    // Background color
    if (app->backgroundColor != 0) {
        uint8_t br = (app->backgroundColor >> 16) & 0xFF;
        uint8_t bg = (app->backgroundColor >> 8) & 0xFF;
        uint8_t bb = app->backgroundColor & 0xFF;
        dma_display->fillScreen(dma_display->color565(br, bg, bb));
    }

    // Text color
    uint8_t r = (app->textColor >> 16) & 0xFF;
    uint8_t g = (app->textColor >> 8) & 0xFF;
    uint8_t b = app->textColor & 0xFF;
    dma_display->setTextColor(dma_display->color565(r, g, b));
    dma_display->setTextSize(1);

    // Calculate text position (centered vertically)
    // Icon would be on left if present (TODO: implement icon rendering)
    int xPos = 2;
    int yPos = 28;

    // Simple text display for now
    dma_display->setCursor(xPos, yPos);
    dma_display->print(app->text);

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
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

    // Handle OPTIONS for CORS preflight
    webServer.on("/api/*", HTTP_OPTIONS, [](AsyncWebServerRequest *request) {
        request->send(200);
    });

    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html",
            "<!DOCTYPE html><html><head><title>PixelCast</title></head>"
            "<body><h1>ESP32-PixelCast</h1>"
            "<p>Version: " VERSION_STRING "</p>"
            "<p><a href='/api/stats'>API Stats</a></p>"
            "<p><a href='/api/apps'>Active Apps</a></p>"
            "</body></html>"
        );
    });

    webServer.on("/api/stats", HTTP_GET, handleApiStats);
    webServer.on("/api/settings", HTTP_GET, handleApiSettings);
    webServer.on("/api/apps", HTTP_GET, handleApiApps);

    // POST /api/brightness - Set brightness
    webServer.on("/api/brightness", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            // This handler should not be called if body handler sends response
        },
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            // Body handler - accumulate and process when complete
            if (index == 0) {
                requestBodyLength = 0;
            }
            if (requestBodyLength + len < sizeof(requestBodyBuffer)) {
                memcpy(requestBodyBuffer + requestBodyLength, data, len);
                requestBodyLength += len;
            }

            // Process when all data received
            if (index + len == total) {
                JsonDocument doc;
                DeserializationError error = deserializeJson(doc, requestBodyBuffer, requestBodyLength);

                if (error) {
                    request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                    return;
                }

                if (!doc["brightness"].isNull()) {
                    uint8_t brightness = doc["brightness"].as<uint8_t>();
                    displaySetBrightness(brightness);
                    settings.brightness = brightness;
                    saveSettings();
                    request->send(200, "application/json", "{\"success\":true}");
                } else {
                    request->send(400, "application/json", "{\"error\":\"Missing brightness\"}");
                }
            }
        });

    // POST /api/custom - Create/update custom app
    webServer.on("/api/custom", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            // This handler should not be called if body handler sends response
        },
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            // Body handler - accumulate and process when complete
            if (index == 0) {
                requestBodyLength = 0;
            }
            if (requestBodyLength + len < sizeof(requestBodyBuffer)) {
                memcpy(requestBodyBuffer + requestBodyLength, data, len);
                requestBodyLength += len;
            }

            // Process when all data received
            if (index + len == total) {
                JsonDocument doc;
                DeserializationError error = deserializeJson(doc, requestBodyBuffer, requestBodyLength);

                if (error) {
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
                    request->send(200, "application/json", "{\"success\":true}");
                } else {
                    request->send(500, "application/json", "{\"error\":\"Failed to add app\"}");
                }
            }
        });

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

    // POST /api/settings - Update settings
    webServer.on("/api/settings", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            // This handler should not be called if body handler sends response
        },
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            // Body handler - accumulate and process when complete
            if (index == 0) {
                requestBodyLength = 0;
            }
            if (requestBodyLength + len < sizeof(requestBodyBuffer)) {
                memcpy(requestBodyBuffer + requestBodyLength, data, len);
                requestBodyLength += len;
            }

            // Process when all data received
            if (index + len == total) {
                JsonDocument doc;
                DeserializationError error = deserializeJson(doc, requestBodyBuffer, requestBodyLength);

                if (error) {
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
                request->send(200, "application/json", "{\"success\":true}");
            }
        });

    // POST /api/reboot - Reboot device
    webServer.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Rebooting...\"}");
        delay(500);
        ESP.restart();
    });

    webServer.onNotFound([](AsyncWebServerRequest *request) {
        if (request->method() == HTTP_OPTIONS) {
            request->send(200);
        } else {
            request->send(404, "application/json", "{\"error\":\"Not found\"}");
        }
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

// ============================================================================
// Application Manager Functions
// ============================================================================

void setupApps() {
    // Initialize app array
    memset(apps, 0, sizeof(apps));
    appCount = 0;
    currentAppIndex = -1;

    // Add system apps
    if (settings.clockEnabled) {
        appAdd("clock", "Clock", "", settings.clockColor, 0x000000,
               settings.defaultDuration, 0, 0, true);
        Serial.println("[APPS] Clock app added");
    }

    if (settings.dateEnabled) {
        appAdd("date", "Date", "", settings.dateColor, 0x000000,
               settings.defaultDuration, 0, 0, true);
        Serial.println("[APPS] Date app added");
    }

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
        }
        return;
    }

    // Check if current app duration has elapsed
    if (appRotationEnabled && (now - lastAppSwitch > current->duration)) {
        current = appGetNext();
        if (current) {
            lastAppSwitch = now;
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

    // Update display based on current app
    if (millis() - lastDisplayUpdate > 1000) {
        AppItem* current = appGetCurrent();
        if (current) {
            displayShowApp(current);
        } else {
            // Fallback: show time if no apps
            displayShowTime();
        }
        lastDisplayUpdate = millis();
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

void logMemory() {
    Serial.printf("[MEM] Free heap: %d bytes, largest block: %d bytes\n",
        ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}
