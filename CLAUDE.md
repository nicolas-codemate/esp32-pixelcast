# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-PixelCast is firmware for HUB75 LED matrix panels (64x64 and larger) controlled via REST API and MQTT. It runs on ESP32 (Trinity board recommended) and is inspired by AWTRIX3 but for larger displays.

## Build & Flash Commands

```bash
# Build firmware (default: trinity environment)
pio run

# Build for specific environment
pio run -e trinity    # ESP32 Trinity (recommended)
pio run -e devkit     # Generic ESP32 DevKit
pio run -e esp32s3    # ESP32-S3 with PSRAM
pio run -e debug      # Debug build with verbose logging
pio run -e release    # Release build with optimizations

# Flash firmware to device
pio run -t upload

# Flash filesystem (icons, config)
pio run -t uploadfs

# Monitor serial output (115200 baud)
pio device monitor

# Clean build
pio run -t clean
```

## Architecture

### Core Components

The firmware follows a single-file architecture (currently in `src/main.cpp`) with planned modular expansion:

- **Display**: ESP32-HUB75-MatrixPanel-DMA driver with double buffering
- **Network**: WiFiManager for captive portal config, ESPAsyncWebServer for REST API
- **MQTT**: PubSubClient for home automation integration
- **Storage**: LittleFS for icons, GIFs, and configuration

### Configuration System

Configuration is split between compile-time and runtime:

1. **Compile-time** (`platformio.ini` build_flags): Panel dimensions, pin mappings, limits
2. **Runtime** (`data/config/settings.json`): WiFi, MQTT, brightness preferences
3. **Defaults** (`include/config.h`): Fallback values with `#ifndef` guards

### Key Defines

Panel settings are configured in `platformio.ini`:
- `PANEL_WIDTH`, `PANEL_HEIGHT`, `PANEL_CHAIN` - Display dimensions
- `R1_PIN`, `G1_PIN`, etc. - HUB75 pin mapping (Trinity defaults provided)
- `MAX_APPS`, `MAX_NOTIFICATIONS`, `MAX_ICON_CACHE` - Resource limits

## Code Style

From CONTRIBUTING.md:
- **Indentation**: 4 spaces
- **Braces**: Allman style
- **Naming**: `camelCase` for variables/functions, `UPPER_SNAKE_CASE` for constants, `PascalCase` for classes
- **Commit format**: `Type: Description` where Type is Add/Fix/Update/Refactor/Docs/Style/Test

## Filesystem Structure

The `data/` directory is uploaded to LittleFS:
- `icons/` - PNG/GIF icons (8x8 to 64x64)
- `gifs/` - Animated GIFs
- `config/settings.json` - Runtime configuration

## Hardware Notes

- **Primary target**: ESP32 Trinity board (clips directly onto HUB75 panels)
- **Panel type**: 64x64 P3 HUB75E with 1/32 scan
- **Pin E (GPIO 32)**: Required for 64x64 panels (1/32 scan rate)
- **Power**: 5V 5A recommended (Mean Well RS-25-5)

## Versioning & Releases

Semantic versioning. Full process in CONTRIBUTING.md - read it before touching a version
number or cutting a release.

The two things to remember while working on a change:

- The version lives in **four** places that must always agree: `platformio.ini` (build
  flags, authoritative), `include/config.h` (`#ifndef` fallbacks), and `info.version` in
  both `docs/api/openapi.yaml` and `docs/api/asyncapi.yaml`. One number covers the firmware
  and the specs describing it.
- Bump it in the same PR as the change that warrants it. A PR that adds an API field and
  leaves the version untouched is incomplete - and so does a specs-only PR that corrects
  the published contract.

Releases are cut by pushing a `vX.Y.Z` tag; `.github/workflows/release.yml` refuses to
publish when the tag and the version in those files disagree.

## API Documentation

Full API specs in `docs/api/`:
- **REST API**: `docs/api/openapi.yaml` (OpenAPI 3.1) - interactive docs at `swagger-ui.html`
- **MQTT API**: `docs/api/asyncapi.yaml` (AsyncAPI 3.0) - interactive docs at `asyncapi.html`
- **Shared schemas**: `docs/api/schemas/` - reusable types referenced by both specs

Base URL: `http://pixelcast.local/api/` | MQTT prefix: `pixelcast/`
