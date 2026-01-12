# Hardware Guide

## Required Hardware

### Recommended Configuration

| Component       | Model                     | Estimated Price |
|-----------------|---------------------------|-----------------|
| Microcontroller | ESP32 Trinity             | ~$30            |
| LED Panel       | HUB75 64x64 P3            | ~$25            |
| Power Supply    | Mean Well RS-25-5 (5V 5A) | ~$15            |
| **Total**       |                           | **~$70**        |

### ESP32 Trinity

Brian Lough's ESP32 Trinity board is designed specifically for HUB75 panels. It clips directly onto the panel connector.

**Features:**

- ESP32-WROOM-32
- Integrated HUB75 connector
- Terminal blocks for 5V power
- USB-C for programming
- Reset and Boot buttons

**Where to buy:**

- [Makerfabs](https://www.makerfabs.com/esp32-trinity.html)
- [Tindie](https://www.tindie.com/products/brianlough/esp32-trinity/)

### HUB75 64x64 Panel

**Recommended specifications:**

- Resolution: 64x64 pixels
- Pitch: P3 (3mm between LEDs)
- Scan: 1/32
- Interface: HUB75E (5 address lines)
- Dimensions: 192 x 192 mm

**Important notes:**

- Verify the panel is compatible (HUB75E, 1/32 scan)
- "Outdoor" panels may have different drivers
- Prefer tested vendors (Makerfabs, Waveshare)

### Power Supply

**Power consumption calculation:**

- 64x64 = 4096 RGB LEDs
- Max theoretical consumption: ~4A (100% white)
- Typical consumption: 1-2A (mixed content)

**Recommendations:**

- Mean Well RS-25-5 (5V 5A): compact, reliable
- Mean Well RS-50-5 (5V 10A): for chained panels
- Avoid USB power supplies (2A limit)

## Wiring Diagram

### ESP32 Trinity + 64x64 Panel

```
                     ┌─────────────────────────────────────────┐
                     │                                         │
                     │           HUB75 64x64 PANEL             │
                     │                                         │
                     │    ←←← Arrows indicate data flow ←←←    │
                     │                                         │
                     │  ┌──────────┐        ┌──────────┐       │
                     │  │  INPUT   │        │  OUTPUT  │       │
                     │  │ (HUB75E) │        │ (chain)  │       │
                     │  └────┬─────┘        └──────────┘       │
                     │       │                                 │
                     │       │                                 │
                     │  ┌────┴────────────────┐                │
                     │  │                     │                │
                     │  │    ESP32 TRINITY    │                │
                     │  │    (clips on)       │                │
                     │  │                     │                │
                     │  │  [5V+] [GND]        │                │
                     │  │    │     │          │                │
                     │  └────┼─────┼──────────┘                │
                     │       │     │                           │
                     │  ┌────┴─────┴────┐                      │
                     │  │ Panel power   │                      │
                     │  │   terminal    │                      │
                     │  └───────────────┘                      │
                     │                                         │
                     └─────────────────────────────────────────┘
                              │     │
                              │     │
                     ┌────────┴─────┴────────┐
                     │    POWER SUPPLY       │
                     │    Mean Well 5V 5A    │
                     │                       │
                     │   [L] [N] [⏚]  [+] [-]│
                     └───┬───┬───┬────┴───┴──┘
                         │   │   │
                      ───┴───┴───┴───
                        AC 110/220V
```

### Connections

1. **ESP32 Trinity → Panel**
    - Clip the Trinity onto the INPUT connector of the panel
    - Respect orientation (USB facing outward)

2. **Power Supply → Trinity**
    - Red (+5V) → Terminal [5V+]
    - Black (GND) → Terminal [GND]

3. **Power Supply → Mains** (open frame)
    - L (brown) → Live
    - N (blue) → Neutral
    - ⏚ (green/yellow) → Ground

## ESP32 Trinity Pinout

The Trinity uses the standard pinout from the ESP32-HUB75-MatrixPanel-DMA library:

| Signal | GPIO | Description           |
|--------|------|-----------------------|
| R1     | 25   | Red line 1            |
| G1     | 26   | Green line 1          |
| B1     | 27   | Blue line 1           |
| R2     | 14   | Red line 2            |
| G2     | 12   | Green line 2          |
| B2     | 13   | Blue line 2           |
| A      | 23   | Address bit 0         |
| B      | 19   | Address bit 1         |
| C      | 5    | Address bit 2         |
| D      | 17   | Address bit 3         |
| E      | 32   | Address bit 4 (64x64) |
| LAT    | 4    | Latch                 |
| OE     | 15   | Output Enable         |
| CLK    | 16   | Clock                 |

## Troubleshooting

### Screen doesn't turn on

1. Check power supply (5V at terminals)
2. Verify Trinity is properly connected
3. Measure voltage at panel terminals

### Corrupted display / missing lines

1. Check panel scan type (1/16 vs 1/32)
2. Adjust E_PIN if necessary
3. Reduce brightness (current overload)

### Flickering

1. Add a 1000µF capacitor on power supply
2. Check connections (cables too long)
3. Reduce color depth (COLOR_DEPTH)

### Unstable WiFi

1. ESP32 WiFi antenna can be affected by LEDs
2. Move antenna away from panel if possible
3. Use an extension cable if necessary
