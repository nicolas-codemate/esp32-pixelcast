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
// Global Objects
// ============================================================================

// Display
MatrixPanel_I2S_DMA *display = nullptr;

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
uint8_t currentBrightness = DEFAULT_BRIGHTNESS;

// Timing
unsigned long lastStatsPublish = 0;
unsigned long lastDisplayUpdate = 0;

// ============================================================================
// Function Prototypes
// ============================================================================

// Setup functions
void setupDisplay();
void setupWiFi();
void setupMDNS();
void setupWebServer();
void setupMQTT();
void setupFilesystem();

// Loop functions
void loopWiFi();
void loopMQTT();
void loopDisplay();
void loopTime();

// Display functions
void displayShowBoot();
void displayShowIP();
void displayClear();
void displaySetBrightness(uint8_t brightness);

// MQTT functions
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttReconnect();
void mqttPublishStats();
void mqttPublishStatus(bool online);

// API handlers
void handleApiCustom(AsyncWebServerRequest *request, JsonVariant &json);
void handleApiNotify(AsyncWebServerRequest *request, JsonVariant &json);
void handleApiSettings(AsyncWebServerRequest *request);
void handleApiStats(AsyncWebServerRequest *request);

// Utility functions
uint32_t colorFromRGB(uint8_t r, uint8_t g, uint8_t b);
void logMemory();

// ============================================================================
// Setup
// ============================================================================

void setup() {
    // Serial
    Serial.begin(115200);
    delay(100);
    
    Serial.println();
    Serial.println("╔═══════════════════════════════════════════╗");
    Serial.println("║         ESP32-PixelCast v" VERSION_STRING "          ║");
    Serial.println("║     HUB75 LED Matrix Display Firmware     ║");
    Serial.println("╚═══════════════════════════════════════════╝");
    Serial.println();
    
    logMemory();
    
    // Initialize subsystems
    Serial.println("[INIT] Setting up display...");
    setupDisplay();
    displayShowBoot();
    
    Serial.println("[INIT] Setting up filesystem...");
    setupFilesystem();
    
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
        timeClient.begin();
        
        displayShowIP();
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
    
    // Pin configuration
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
    mxconfig.gpio.e = E_PIN;  // Required for 64x64
    mxconfig.gpio.lat = LAT_PIN;
    mxconfig.gpio.oe = OE_PIN;
    mxconfig.gpio.clk = CLK_PIN;
    
    // Display options
    mxconfig.clkphase = false;
    mxconfig.driver = HUB75_I2S_CFG::FM6124;
    
    #if DOUBLE_BUFFER
        mxconfig.double_buff = true;
    #endif
    
    // Create display instance
    display = new MatrixPanel_I2S_DMA(mxconfig);
    
    if (!display->begin()) {
        Serial.println("[ERROR] Display initialization failed!");
        while (true) { delay(1000); }
    }
    
    display->setBrightness8(currentBrightness);
    display->clearScreen();
    
    Serial.printf("[DISPLAY] Initialized %dx%d panel\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

void displayShowBoot() {
    display->clearScreen();
    display->setTextColor(display->color565(0, 150, 255));
    display->setTextSize(1);
    display->setCursor(4, 20);
    display->print("PixelCast");
    display->setCursor(4, 35);
    display->setTextColor(display->color565(100, 100, 100));
    display->print("v" VERSION_STRING);
    
    #if DOUBLE_BUFFER
        display->flipDMABuffer();
    #endif
}

void displayShowIP() {
    delay(2000);  // Show boot screen for 2s
    
    display->clearScreen();
    display->setTextColor(display->color565(0, 255, 0));
    display->setTextSize(1);
    display->setCursor(4, 10);
    display->print("WiFi OK");
    
    display->setTextColor(display->color565(255, 255, 255));
    display->setCursor(4, 25);
    display->print(WiFi.localIP().toString());
    
    display->setTextColor(display->color565(100, 100, 100));
    display->setCursor(4, 45);
    display->print(MDNS_NAME ".local");
    
    #if DOUBLE_BUFFER
        display->flipDMABuffer();
    #endif
    
    delay(3000);  // Show IP for 3s
}

void displayClear() {
    display->clearScreen();
    #if DOUBLE_BUFFER
        display->flipDMABuffer();
    #endif
}

void displaySetBrightness(uint8_t brightness) {
    currentBrightness = constrain(brightness, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
    display->setBrightness8(currentBrightness);
    Serial.printf("[DISPLAY] Brightness set to %d\n", currentBrightness);
}

// ============================================================================
// WiFi Functions
// ============================================================================

void setupWiFi() {
    wifiManager.setConfigPortalTimeout(180);  // 3 minutes timeout
    wifiManager.setAPCallback([](WiFiManager *myWiFiManager) {
        Serial.println("[WIFI] Config portal started");
        display->clearScreen();
        display->setTextColor(display->color565(255, 165, 0));
        display->setCursor(4, 20);
        display->print("WiFi Setup");
        display->setTextColor(display->color565(255, 255, 255));
        display->setCursor(4, 35);
        display->print(WIFI_AP_NAME);
        #if DOUBLE_BUFFER
            display->flipDMABuffer();
        #endif
    });
    
    wifiConnected = wifiManager.autoConnect(WIFI_AP_NAME, WIFI_AP_PASS);
    
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
    // CORS headers
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");
    
    // Root
    webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", 
            "<!DOCTYPE html><html><head><title>PixelCast</title></head>"
            "<body><h1>ESP32-PixelCast</h1>"
            "<p>Version: " VERSION_STRING "</p>"
            "<p><a href='/api/stats'>API Stats</a></p>"
            "</body></html>"
        );
    });
    
    // API: Stats
    webServer.on("/api/stats", HTTP_GET, handleApiStats);
    
    // API: Settings GET
    webServer.on("/api/settings", HTTP_GET, handleApiSettings);
    
    // API: Brightness
    webServer.on("/api/brightness", HTTP_POST, [](AsyncWebServerRequest *request) {
        // Handled by body handler
    }, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, data, len);
        
        if (error) {
            request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }
        
        if (doc.containsKey("brightness")) {
            displaySetBrightness(doc["brightness"].as<uint8_t>());
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            request->send(400, "application/json", "{\"error\":\"Missing brightness\"}");
        }
    });
    
    // API: Reboot
    webServer.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Rebooting...\"}");
        delay(500);
        ESP.restart();
    });
    
    // 404 handler
    webServer.onNotFound([](AsyncWebServerRequest *request) {
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
    doc["brightness"] = currentBrightness;
    doc["wifi"]["ssid"] = WiFi.SSID();
    doc["wifi"]["rssi"] = WiFi.RSSI();
    doc["wifi"]["ip"] = WiFi.localIP().toString();
    doc["display"]["width"] = DISPLAY_WIDTH;
    doc["display"]["height"] = DISPLAY_HEIGHT;
    doc["mqtt"]["connected"] = mqttConnected;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void handleApiSettings(AsyncWebServerRequest *request) {
    JsonDocument doc;
    
    doc["brightness"] = currentBrightness;
    doc["display"]["width"] = DISPLAY_WIDTH;
    doc["display"]["height"] = DISPLAY_HEIGHT;
    doc["mqtt"]["prefix"] = MQTT_PREFIX;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// ============================================================================
// MQTT Functions
// ============================================================================

void setupMQTT() {
    // TODO: Load from config
    // mqttClient.setServer(mqtt_server, mqtt_port);
    // mqttClient.setCallback(mqttCallback);
    // mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
    
    Serial.println("[MQTT] Not configured (TODO: implement config)");
}

void loopMQTT() {
    if (!wifiConnected) return;
    
    // Reconnect if needed
    // if (!mqttClient.connected()) {
    //     mqttReconnect();
    // }
    // mqttClient.loop();
    
    // Publish stats periodically
    if (millis() - lastStatsPublish > MQTT_STATS_INTERVAL) {
        mqttPublishStats();
        lastStatsPublish = millis();
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[MQTT] Message on topic: %s\n", topic);
    
    // Parse JSON payload
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    
    if (error) {
        Serial.printf("[MQTT] JSON parse error: %s\n", error.c_str());
        return;
    }
    
    // TODO: Route to appropriate handler based on topic
}

void mqttReconnect() {
    // TODO: Implement reconnection logic
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
    
    // mqttClient.publish((String(MQTT_PREFIX) + MQTT_TOPIC_STATS).c_str(), payload.c_str());
}

void mqttPublishStatus(bool online) {
    // mqttClient.publish((String(MQTT_PREFIX) + MQTT_TOPIC_STATUS).c_str(), online ? "online" : "offline", true);
}

// ============================================================================
// Filesystem Functions
// ============================================================================

void setupFilesystem() {
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed!");
        return;
    }
    
    Serial.printf("[FS] LittleFS mounted, total: %d bytes, used: %d bytes\n",
        LittleFS.totalBytes(), LittleFS.usedBytes());
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
    // TODO: Implement app rotation and rendering
    
    // For now, just show time
    static unsigned long lastTimeUpdate = 0;
    if (millis() - lastTimeUpdate > 1000) {
        lastTimeUpdate = millis();
        
        if (wifiConnected) {
            display->clearScreen();
            
            // Draw time
            display->setTextColor(display->color565(255, 255, 255));
            display->setTextSize(2);
            display->setCursor(8, 24);
            display->print(timeClient.getFormattedTime());
            
            #if DOUBLE_BUFFER
                display->flipDMABuffer();
            #endif
        }
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

uint32_t colorFromRGB(uint8_t r, uint8_t g, uint8_t b) {
    return display->color565(r, g, b);
}

void logMemory() {
    Serial.printf("[MEM] Free heap: %d bytes, largest block: %d bytes\n",
        ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}
