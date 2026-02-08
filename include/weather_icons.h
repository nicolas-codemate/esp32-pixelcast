#ifndef WEATHER_ICONS_H
#define WEATHER_ICONS_H

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// ============================================================
// Built-in PROGMEM weather icons (8x8 pixel art, RGB565)
// Eliminates filesystem dependency for weather dashboard
// ============================================================

// Compile-time RGB565 color conversion
#define WI_RGB565(r, g, b) (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

// Named palette for pixel art readability (WI_ prefix avoids ctype.h collisions)
#define WI__  0x0000                       // Transparent (black = skip)
#define WI_Y  WI_RGB565(255, 220, 50)      // Yellow (sun core)
#define WI_A  WI_RGB565(255, 180, 50)      // Amber (sun rays)
#define WI_W  WI_RGB565(220, 220, 220)     // White (cloud highlight)
#define WI_L  WI_RGB565(170, 170, 170)     // Light gray (cloud body)
#define WI_G  WI_RGB565(120, 120, 120)     // Gray (cloud shadow)
#define WI_D  WI_RGB565(80, 80, 80)        // Dark gray (cloud underside)
#define WI_B  WI_RGB565(50, 100, 220)      // Blue (rain drops)
#define WI_C  WI_RGB565(0, 180, 255)       // Cyan (heavy rain)
#define WI_P  WI_RGB565(140, 180, 255)     // Pale blue (moon)
#define WI_S  WI_RGB565(240, 240, 255)     // Snow white (snowflakes)
#define WI_F  WI_RGB565(150, 150, 140)     // Fog gray
#define WI_E  WI_RGB565(255, 240, 80)      // Lightning yellow

// ---- Clear day: sun with rays ----
const uint16_t w_clear_day[64] PROGMEM = {
    WI__, WI__, WI_A, WI__, WI__, WI_A, WI__, WI__,
    WI__, WI__, WI__, WI_Y, WI_Y, WI__, WI__, WI__,
    WI_A, WI__, WI_Y, WI_Y, WI_Y, WI_Y, WI__, WI_A,
    WI__, WI_Y, WI_Y, WI_Y, WI_Y, WI_Y, WI_Y, WI__,
    WI__, WI_Y, WI_Y, WI_Y, WI_Y, WI_Y, WI_Y, WI__,
    WI_A, WI__, WI_Y, WI_Y, WI_Y, WI_Y, WI__, WI_A,
    WI__, WI__, WI__, WI_Y, WI_Y, WI__, WI__, WI__,
    WI__, WI__, WI_A, WI__, WI__, WI_A, WI__, WI__,
};

// ---- Clear night: crescent moon ----
const uint16_t w_clear_night[64] PROGMEM = {
    WI__, WI__, WI__, WI_P, WI_P, WI__, WI__, WI__,
    WI__, WI__, WI_P, WI_P, WI__, WI__, WI__, WI__,
    WI__, WI_P, WI_P, WI__, WI__, WI__, WI__, WI__,
    WI__, WI_P, WI_P, WI__, WI__, WI__, WI__, WI__,
    WI__, WI_P, WI_P, WI__, WI__, WI__, WI__, WI__,
    WI__, WI_P, WI_P, WI__, WI__, WI__, WI__, WI__,
    WI__, WI__, WI_P, WI_P, WI__, WI__, WI__, WI__,
    WI__, WI__, WI__, WI_P, WI_P, WI__, WI__, WI__,
};

// ---- Partly cloudy day: small sun top-right + cloud bottom-left ----
const uint16_t w_partly_day[64] PROGMEM = {
    WI__, WI__, WI__, WI__, WI_A, WI__, WI_A, WI__,
    WI__, WI__, WI__, WI__, WI__, WI_Y, WI__, WI__,
    WI__, WI__, WI__, WI__, WI_Y, WI_Y, WI_Y, WI_A,
    WI__, WI__, WI_W, WI_W, WI_Y, WI_Y, WI_Y, WI__,
    WI__, WI_W, WI_W, WI_W, WI_W, WI__, WI__, WI__,
    WI_W, WI_W, WI_L, WI_L, WI_W, WI_W, WI__, WI__,
    WI_L, WI_L, WI_G, WI_G, WI_L, WI_L, WI__, WI__,
    WI__, WI_D, WI_D, WI_D, WI_D, WI__, WI__, WI__,
};

// ---- Partly cloudy night: small moon top-right + cloud bottom-left ----
const uint16_t w_partly_night[64] PROGMEM = {
    WI__, WI__, WI__, WI__, WI__, WI_P, WI_P, WI__,
    WI__, WI__, WI__, WI__, WI_P, WI_P, WI__, WI__,
    WI__, WI__, WI__, WI__, WI_P, WI_P, WI__, WI__,
    WI__, WI__, WI_W, WI_W, WI__, WI_P, WI_P, WI__,
    WI__, WI_W, WI_W, WI_W, WI_W, WI__, WI__, WI__,
    WI_W, WI_W, WI_L, WI_L, WI_W, WI_W, WI__, WI__,
    WI_L, WI_L, WI_G, WI_G, WI_L, WI_L, WI__, WI__,
    WI__, WI_D, WI_D, WI_D, WI_D, WI__, WI__, WI__,
};

// ---- Cloudy: full cloud ----
const uint16_t w_cloudy[64] PROGMEM = {
    WI__, WI__, WI__, WI__, WI__, WI__, WI__, WI__,
    WI__, WI__, WI_W, WI_W, WI__, WI__, WI__, WI__,
    WI__, WI_W, WI_W, WI_W, WI_W, WI_W, WI__, WI__,
    WI_W, WI_W, WI_L, WI_L, WI_W, WI_W, WI_W, WI__,
    WI_W, WI_L, WI_L, WI_G, WI_L, WI_L, WI_W, WI__,
    WI_L, WI_L, WI_G, WI_G, WI_G, WI_L, WI_L, WI__,
    WI__, WI_G, WI_D, WI_D, WI_D, WI_G, WI__, WI__,
    WI__, WI__, WI__, WI__, WI__, WI__, WI__, WI__,
};

// ---- Rain: cloud + rain drops ----
const uint16_t w_rain[64] PROGMEM = {
    WI__, WI__, WI_W, WI_W, WI__, WI__, WI__, WI__,
    WI__, WI_W, WI_W, WI_W, WI_W, WI_W, WI__, WI__,
    WI_W, WI_W, WI_L, WI_L, WI_W, WI_W, WI_W, WI__,
    WI_L, WI_L, WI_G, WI_G, WI_L, WI_L, WI_L, WI__,
    WI__, WI_G, WI_D, WI_D, WI_D, WI_G, WI__, WI__,
    WI__, WI_B, WI__, WI__, WI_B, WI__, WI__, WI__,
    WI__, WI__, WI__, WI_B, WI__, WI__, WI_B, WI__,
    WI__, WI__, WI__, WI__, WI__, WI__, WI__, WI__,
};

// ---- Heavy rain: cloud + many drops ----
const uint16_t w_heavy_rain[64] PROGMEM = {
    WI__, WI__, WI_W, WI_W, WI__, WI__, WI__, WI__,
    WI__, WI_W, WI_W, WI_W, WI_W, WI_W, WI__, WI__,
    WI_W, WI_W, WI_L, WI_L, WI_W, WI_W, WI_W, WI__,
    WI_L, WI_L, WI_G, WI_G, WI_L, WI_L, WI_L, WI__,
    WI__, WI_G, WI_D, WI_D, WI_D, WI_G, WI__, WI__,
    WI_C, WI__, WI_C, WI__, WI_C, WI__, WI_C, WI__,
    WI__, WI_C, WI__, WI_C, WI__, WI_C, WI__, WI__,
    WI__, WI__, WI__, WI__, WI__, WI__, WI__, WI__,
};

// ---- Thunderstorm: cloud + lightning bolt ----
const uint16_t w_thunder[64] PROGMEM = {
    WI__, WI__, WI_W, WI_W, WI__, WI__, WI__, WI__,
    WI__, WI_W, WI_W, WI_W, WI_W, WI_W, WI__, WI__,
    WI_W, WI_W, WI_L, WI_L, WI_W, WI_W, WI_W, WI__,
    WI_L, WI_L, WI_G, WI_G, WI_L, WI_L, WI_L, WI__,
    WI__, WI_G, WI_D, WI_E, WI_D, WI_G, WI__, WI__,
    WI__, WI__, WI_E, WI_E, WI__, WI__, WI__, WI__,
    WI__, WI__, WI__, WI_E, WI_E, WI__, WI__, WI__,
    WI__, WI__, WI__, WI_E, WI__, WI__, WI__, WI__,
};

// ---- Snow: cloud + snowflakes ----
const uint16_t w_snow[64] PROGMEM = {
    WI__, WI__, WI_W, WI_W, WI__, WI__, WI__, WI__,
    WI__, WI_W, WI_W, WI_W, WI_W, WI_W, WI__, WI__,
    WI_W, WI_W, WI_L, WI_L, WI_W, WI_W, WI_W, WI__,
    WI_L, WI_L, WI_G, WI_G, WI_L, WI_L, WI_L, WI__,
    WI__, WI_G, WI_D, WI_D, WI_D, WI_G, WI__, WI__,
    WI__, WI_S, WI__, WI__, WI_S, WI__, WI_S, WI__,
    WI__, WI__, WI_S, WI__, WI__, WI_S, WI__, WI__,
    WI__, WI__, WI__, WI_S, WI__, WI__, WI__, WI__,
};

// ---- Fog: horizontal dashed lines ----
const uint16_t w_fog[64] PROGMEM = {
    WI__, WI__, WI__, WI__, WI__, WI__, WI__, WI__,
    WI__, WI_F, WI_F, WI_F, WI_F, WI_F, WI_F, WI__,
    WI__, WI__, WI__, WI__, WI__, WI__, WI__, WI__,
    WI_F, WI_F, WI_F, WI_F, WI_F, WI_F, WI__, WI__,
    WI__, WI__, WI__, WI__, WI__, WI__, WI__, WI__,
    WI__, WI__, WI_F, WI_F, WI_F, WI_F, WI_F, WI__,
    WI__, WI__, WI__, WI__, WI__, WI__, WI__, WI__,
    WI_F, WI_F, WI_F, WI__, WI_F, WI_F, WI_F, WI__,
};

// Clean up palette macros
#undef WI__
#undef WI_Y
#undef WI_A
#undef WI_W
#undef WI_L
#undef WI_G
#undef WI_D
#undef WI_B
#undef WI_C
#undef WI_P
#undef WI_S
#undef WI_F
#undef WI_E
#undef WI_RGB565

// ============================================================
// Lookup table: API name -> PROGMEM icon pointer
// ============================================================

struct ProgmemWeatherIcon {
    const char* name;
    const uint16_t* pixels;
};

const ProgmemWeatherIcon PROGMEM_WEATHER_ICONS[] = {
    { "w_clear_day",    w_clear_day },
    { "w_clear_night",  w_clear_night },
    { "w_partly_day",   w_partly_day },
    { "w_partly_night", w_partly_night },
    { "w_cloudy",       w_cloudy },
    { "w_rain",         w_rain },
    { "w_heavy_rain",   w_heavy_rain },
    { "w_thunder",      w_thunder },
    { "w_snow",         w_snow },
    { "w_fog",          w_fog },
};

const uint8_t PROGMEM_WEATHER_ICON_COUNT = sizeof(PROGMEM_WEATHER_ICONS) / sizeof(PROGMEM_WEATHER_ICONS[0]);

// Lookup builtin icon by name, returns nullptr if not found
inline const uint16_t* getBuiltinWeatherIcon(const char* name) {
    if (!name) return nullptr;
    for (uint8_t i = 0; i < PROGMEM_WEATHER_ICON_COUNT; i++) {
        if (strcmp(name, PROGMEM_WEATHER_ICONS[i].name) == 0) {
            return PROGMEM_WEATHER_ICONS[i].pixels;
        }
    }
    return nullptr;
}

// Draw a PROGMEM 8x8 icon at position (x, y) with optional scale
// Reads pixels via pgm_read_word(); 0x0000 = transparent (skipped)
inline void drawProgmemIcon(MatrixPanel_I2S_DMA* display, const uint16_t* progmemPixels,
                            int16_t x, int16_t y, uint8_t scale) {
    if (!display || !progmemPixels) return;

    for (uint8_t py = 0; py < 8; py++) {
        for (uint8_t px = 0; px < 8; px++) {
            uint16_t pixel = pgm_read_word(&progmemPixels[py * 8 + px]);
            if (pixel != 0x0000) {
                if (scale == 2) {
                    int16_t dx = x + px * 2;
                    int16_t dy = y + py * 2;
                    display->drawPixel(dx, dy, pixel);
                    display->drawPixel(dx + 1, dy, pixel);
                    display->drawPixel(dx, dy + 1, pixel);
                    display->drawPixel(dx + 1, dy + 1, pixel);
                } else {
                    display->drawPixel(x + px, y + py, pixel);
                }
            }
        }
    }
}

#endif // WEATHER_ICONS_H
