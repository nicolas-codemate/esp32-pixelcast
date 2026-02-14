/**
 * ESP32-PixelCast
 * Global Configuration
 *
 * This file centralizes all configuration constants.
 * Values can be overridden via platformio.ini build_flags.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================================
// Version
// ============================================================================
#ifndef VERSION_MAJOR
    #define VERSION_MAJOR 0
#endif
#ifndef VERSION_MINOR
    #define VERSION_MINOR 1
#endif
#ifndef VERSION_PATCH
    #define VERSION_PATCH 0
#endif
#ifndef VERSION_STRING
    #define VERSION_STRING "0.1.0-dev"
#endif

// ============================================================================
// Panel Configuration
// ============================================================================
#ifndef PANEL_WIDTH
    #define PANEL_WIDTH 64
#endif
#ifndef PANEL_HEIGHT
    #define PANEL_HEIGHT 64
#endif
#ifndef PANEL_CHAIN
    #define PANEL_CHAIN 1
#endif

// Total resolution
#define DISPLAY_WIDTH  (PANEL_WIDTH * PANEL_CHAIN)
#define DISPLAY_HEIGHT PANEL_HEIGHT

// ============================================================================
// Display Options
// ============================================================================
#ifndef COLOR_DEPTH
    #define COLOR_DEPTH 6  // 2-8 bits per color channel
#endif
#ifndef DOUBLE_BUFFER
    #define DOUBLE_BUFFER 1
#endif
#ifndef DEFAULT_BRIGHTNESS
    #define DEFAULT_BRIGHTNESS 128  // 0-255
#endif
#define MIN_BRIGHTNESS 1
#define MAX_BRIGHTNESS 255

// ============================================================================
// Pin Definitions (ESP32 Trinity defaults)
// ============================================================================
// RGB Data pins
#ifndef R1_PIN
    #define R1_PIN 25
#endif
#ifndef G1_PIN
    #define G1_PIN 26
#endif
#ifndef B1_PIN
    #define B1_PIN 27
#endif
#ifndef R2_PIN
    #define R2_PIN 14
#endif
#ifndef G2_PIN
    #define G2_PIN 12
#endif
#ifndef B2_PIN
    #define B2_PIN 13
#endif

// Address pins
#ifndef A_PIN
    #define A_PIN 23
#endif
#ifndef B_PIN
    #define B_PIN 19
#endif
#ifndef C_PIN
    #define C_PIN 5
#endif
#ifndef D_PIN
    #define D_PIN 17
#endif
#ifndef E_PIN
    #define E_PIN 32  // Required for 64x64 (1/32 scan)
#endif

// Control pins
#ifndef LAT_PIN
    #define LAT_PIN 4
#endif
#ifndef OE_PIN
    #define OE_PIN 15
#endif
#ifndef CLK_PIN
    #define CLK_PIN 16
#endif

// ============================================================================
// Application Limits
// ============================================================================
#ifndef MAX_APPS
    #define MAX_APPS 16
#endif
#ifndef MAX_NOTIFICATIONS
    #define MAX_NOTIFICATIONS 10
#endif
#ifndef MAX_ICON_CACHE
    #define MAX_ICON_CACHE 8
#endif

// ============================================================================
// Icon Management
// ============================================================================
#ifndef MAX_ICON_SIZE
    #define MAX_ICON_SIZE 8192          // 8KB max per icon
#endif
#ifndef MAX_ICON_DIMENSION
    #define MAX_ICON_DIMENSION 64       // Max 64x64 pixels
#endif

// ============================================================================
// Tracker Layout
// ============================================================================
#ifndef MAX_TRACKERS
    #define MAX_TRACKERS 8
#endif
#ifndef MAX_SPARKLINE_POINTS
    #define MAX_SPARKLINE_POINTS 24
#endif
#define TRACKER_STALE_TIMEOUT 3600000   // 1 hour in ms
#define TRACKER_ID_PREFIX "tracker_"
#define LAMETRIC_API_HOST "developer.lametric.com"
#define LAMETRIC_ICON_PATH "/content/apps/icon_thumbs/"

// App defaults
#define DEFAULT_APP_DURATION    10000   // 10 seconds
#define DEFAULT_APP_LIFETIME    300000  // 5 minutes (0 = permanent)
#define DEFAULT_NOTIF_DURATION  5000    // 5 seconds

// Text scrolling
#define SCROLL_SPEED 50  // ms per pixel
#define SCROLL_PAUSE 2000  // Pause at start/end

// ============================================================================
// WiFi Configuration
// ============================================================================
#ifndef WIFI_AP_NAME
    #define WIFI_AP_NAME "PixelCast"
#endif
#ifndef WIFI_AP_PASS
    #define WIFI_AP_PASS "pixelcast"
#endif
#define WIFI_CONNECT_TIMEOUT 30000  // 30 seconds
#define WIFI_RECONNECT_DELAY 5000   // 5 seconds

// ============================================================================
// mDNS
// ============================================================================
#ifndef MDNS_NAME
    #define MDNS_NAME "pixelcast"
#endif

// ============================================================================
// MQTT Configuration
// ============================================================================
#ifndef MQTT_PREFIX
    #define MQTT_PREFIX "pixelcast"
#endif
#ifndef MQTT_BUFFER_SIZE
    #define MQTT_BUFFER_SIZE 1024
#endif
#define MQTT_RECONNECT_DELAY 5000  // 5 seconds
#define MQTT_KEEPALIVE 60  // seconds
#define MQTT_STATS_INTERVAL 60000  // Publish stats every minute

// MQTT Topics (relative to MQTT_PREFIX)
#define MQTT_TOPIC_CUSTOM       "/custom"
#define MQTT_TOPIC_NOTIFY       "/notify"
#define MQTT_TOPIC_DISMISS      "/dismiss"
#define MQTT_TOPIC_INDICATOR    "/indicator"
#define MQTT_TOPIC_SETTINGS     "/settings"
#define MQTT_TOPIC_BRIGHTNESS   "/brightness"
#define MQTT_TOPIC_REBOOT       "/reboot"
#define MQTT_TOPIC_STATS        "/stats"
#define MQTT_TOPIC_STATUS       "/status"

// ============================================================================
// Web Server
// ============================================================================
#define WEB_SERVER_PORT 80
#define WEBSOCKET_PORT 81

// ============================================================================
// NTP Configuration
// ============================================================================
#define NTP_SERVER "pool.ntp.org"
#define NTP_OFFSET 3600      // UTC+1 (France)
#define NTP_UPDATE_INTERVAL 3600000  // 1 hour

// ============================================================================
// Filesystem
// ============================================================================
#define FS_ICONS_PATH "/icons"
#define FS_GIFS_PATH "/gifs"
#define FS_CONFIG_PATH "/config"
#define FS_WWW_PATH "/www"
#define FS_CONFIG_FILE "/config/settings.json"
#define FS_APPS_FILE "/config/apps.json"

// ============================================================================
// Indicator Configuration
// ============================================================================
#define NUM_INDICATORS 3
#define INDICATOR_CORE_SIZE 3      // 3x3 colored core
#define INDICATOR_BORDER_SIZE 1    // 1px black contour
#define INDICATOR_FOOTPRINT 5      // CORE + 2*BORDER = 5x5 total
#define INDICATOR_BLINK_INTERVAL 500   // Default blink interval (ms)
#define INDICATOR_FADE_PERIOD 2000     // Default fade cycle period (ms)

// Indicator positions (corners)
// Indicator 1: Top-left
// Indicator 2: Top-right
// Indicator 3: Bottom-right

// ============================================================================
// Effects
// ============================================================================
typedef enum {
    EFFECT_NONE = 0,
    EFFECT_MATRIX,
    EFFECT_RAINBOW,
    EFFECT_FADE,
    EFFECT_PULSE,
    EFFECT_SPARKLE
} DisplayEffect;

// ============================================================================
// Transitions
// ============================================================================
typedef enum {
    TRANSITION_NONE = 0,
    TRANSITION_FADE,
    TRANSITION_SLIDE_LEFT,
    TRANSITION_SLIDE_UP
} TransitionType;

#define DEFAULT_TRANSITION TRANSITION_NONE
#define TRANSITION_DURATION 300  // ms

// ============================================================================
// Debug
// ============================================================================
#ifndef DEBUG_MODE
    #define DEBUG_MODE 0
#endif

#if DEBUG_MODE
    #define DEBUG_PRINT(x) Serial.print(x)
    #define DEBUG_PRINTLN(x) Serial.println(x)
    #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
    #define DEBUG_PRINT(x)
    #define DEBUG_PRINTLN(x)
    #define DEBUG_PRINTF(...)
#endif

// ============================================================================
// System
// ============================================================================
#define WATCHDOG_TIMEOUT 30  // seconds
#define TASK_STACK_SIZE 4096
#define LOOP_DELAY 10  // Main loop delay (ms)

#endif // CONFIG_H
