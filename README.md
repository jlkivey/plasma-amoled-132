# Plasma Demo on Waveshare ESP32-C6-Touch-AMOLED-1.32

A real-time plasma animation effect running on the Waveshare ESP32-C6-Touch-AMOLED-1.32 development board, built entirely from the command line using ESP-IDF.

---

## Hardware

| Item | Detail |
|------|--------|
| **Board** | [Waveshare ESP32-C6-Touch-AMOLED-1.32](https://www.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-1.32) |
| **Chip** | ESP32-C6 — single-core RISC-V @ 160 MHz |
| **Memory** | 512 KB HP SRAM, 16 KB LP SRAM, 320 KB ROM, **no PSRAM** |
| **Flash** | 16 MB (W25Q128JVSI) |
| **Display** | 1.32" AMOLED, 466x466, 16.7M colors, circular |
| **Display Controller** | CO5300 / SH8601 (QSPI interface) |
| **Touch Controller** | CST820 (I2C) |
| **Connectivity** | 2.4 GHz Wi-Fi 6, BLE 5, Zigbee 3.0 / Thread |
| **Interface** | USB Type-C |

### QSPI Pin Mapping

| Function | GPIO |
|----------|------|
| LCD CS | 22 |
| LCD PCLK (SCLK) | 18 |
| LCD DATA0 | 19 |
| LCD DATA1 | 20 |
| LCD DATA2 | 10 |
| LCD DATA3 | 11 |
| LCD RST | 21 |
| BOOT Button | 9 |
| Backlight | N/A (AMOLED self-emitting, brightness via register 0x51) |

> **Key difference from TFT boards:** This AMOLED uses **QSPI** (4 data lines, no DC pin, 32-bit command encoding) via the `esp_lcd_sh8601` component. There is no backlight GPIO — brightness is controlled by writing to AMOLED register `0x51`.

---

## Prerequisites

- **ESP-IDF v5.5.x** installed (tested with v5.5.2)
- Board connected via USB-C (check port with `ls /dev/cu.usb*`)

---

## Quick Start

```bash
# 1. Activate ESP-IDF (required once per shell session)
source ~/.espressif/tools/activate_idf_v5.5.2.sh

# 2. Enter the project directory
cd ~/dev/plasma-amoled-132

# 3. Set target (only needed once after clean)
idf.py set-target esp32c6

# 4. Build
idf.py build

# 5. Flash (replace PORT with your device, e.g. /dev/cu.usbmodem101)
idf.py -p PORT flash

# 6. Monitor (Ctrl+] to exit)
idf.py -p PORT monitor

# Or all-in-one:
idf.py -p PORT flash monitor
```

---

## Step-by-Step: How This Was Built

### 1. Identify the Hardware

```bash
esptool.py --port /dev/cu.usbmodem101 chip_id
# Reports: ESP32-C6 (QFN40) revision v0.2
```

The board was identified as the **ESP32-C6-Touch-AMOLED-1.32** from the [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-1.32). Key specs discovered:

- Display controller: **CO5300** (compatible with SH8601 driver)
- Interface: **QSPI** (4 data lines, not regular SPI with MOSI + DC)
- Resolution: **466x466** with X offset of 6 pixels
- AMOLED: self-emitting, no backlight pin needed

### 2. Study the Official Demo

The existing demo at `ESP32-C6-Touch-AMOLED-1.32-Demo/02 ESP-IDF/06_LVGL_V8_Test/` was studied to extract:

- **Pin mapping** from `main/user_config.h`
- **QSPI bus config** from `components/lvgl_port_bsp/lvgl_port_bsp.cpp` — notably `data0_io_num` through `data3_io_num` instead of `mosi_io_num`
- **SH8601 init commands** — the exact vendor-specific sequence for CO5300
- **32-bit QSPI command encoding** — commands are encoded as `(0x02 << 24) | (cmd << 8)`
- **Brightness control** — register `0x51` via `esp_lcd_panel_io_tx_param`
- **Rotation** — register `0x36` with value `0xC0` (MY|MX for 180-degree rotation)
- **Component dependency** — `esp_lcd_sh8601` from ESP Component Registry
- **X offset handling** — the official demo adds offset manually in the flush callback, NOT via `esp_lcd_panel_set_gap`
- **SPI bus max_transfer_sz** — set to full framebuffer size (`466 * 466 * 2 = 434,312 bytes`)

### 3. Create the Project

```bash
mkdir -p ~/dev/plasma-amoled-132/main
```

Six files were created:

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Top-level project definition |
| `sdkconfig.defaults` | Target esp32c6, 8MB flash, QIO mode, perf optimization, USB console |
| `partitions.csv` | Custom partition table with 3MB app partition |
| `main/idf_component.yml` | Declares dependency on `esp_lcd_sh8601` |
| `main/CMakeLists.txt` | Component registration (single `main.c`) |
| `main/main.c` | All source code |

### 4. Key Design Decisions

- **`esp_lcd` framework** — used instead of raw SPI because QSPI protocol (32-bit commands, 4 data lines, no DC pin) is complex to implement manually
- **`esp_lcd_sh8601` component** — handles QSPI command encoding, panel init, and draw_bitmap internally; pulled from ESP Component Registry via `idf_component.yml`
- **Multi-line DMA buffer** (4 lines = 3,728 bytes) — trades a small amount of RAM for fewer SPI transactions per frame
- **Manual X offset** — `X_OFFSET = 6` is added directly to the `draw_bitmap` coordinates, matching the official demo approach (not via `set_gap`)
- **`max_transfer_sz` matches official demo** — set to full framebuffer size; smaller values can silently prevent QSPI transfers
- **Rotation set AFTER `esp_lcd_panel_init`** — order matters; the official demo sets `0x36` register after full init
- **AMOLED brightness** — via register 0x51 command, not PWM backlight
- **BOOT button on GPIO9** — standard ESP32-C6 BOOT pin
- **USB serial console** — `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` is required for serial output on C6 boards using USB-Serial/JTAG

### 5. Performance: Integer Fixed-Point Inner Loop

The ESP32-C6 has **no hardware FPU** — all floating-point is emulated in software. The initial implementation using `float` for the per-pixel plasma computation ran at only **0.7 FPS** across 466x466 = 217,156 pixels.

**Solution:** Convert the inner loop to **8.8 fixed-point integer math**:
- Per-frame warp parameters (sin, cos, scale) are computed once in float, then converted to `int32_t` fixed-point
- The inner loop uses only integer multiply, shift, add — no float
- Result: **10.9 FPS** (15x speedup)

### 6. Build and Flash

```bash
source ~/.espressif/tools/activate_idf_v5.5.2.sh
cd ~/dev/plasma-amoled-132
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

---

## How It Works

### Plasma Algorithm

Same algorithm as the ESP32-S3-Touch-LCD-1.47 version:

1. **256-entry sine LUT** precomputed at startup
2. **Per-pixel computation** — 4 overlapping sine waves, sampled via LUT
3. **Time-based warp** — sinusoidal rotation/stretch simulating accelerometer tilt
4. **Palette cycling** — index shifts each frame for smooth animation
5. **3 palettes** — HSV rainbow, cyclic grayscale, cyclic sunset

### QSPI Display Interface

Unlike regular SPI (1 data line + DC pin), QSPI uses:
- **4 parallel data lines** (DATA0-DATA3) for 4x throughput
- **No DC pin** — command vs. data is encoded in the 32-bit command word
- **32-bit commands** — format: `(0x02 << 24) | (register << 8)` for writes, `(0x32 << 24) | (register << 8)` for color data

The `esp_lcd_sh8601` component abstracts all of this, exposing the standard `esp_lcd_panel_draw_bitmap()` API.

### Performance

| Metric | Value |
|--------|-------|
| **Resolution** | 466 x 466 (217,156 pixels) |
| **FPS (float inner loop)** | ~0.7 |
| **FPS (fixed-point inner loop)** | ~10.9 |
| **Lines per DMA transfer** | 4 |
| **SPI clock** | 40 MHz QSPI |

Compared to the ESP32-S3-Touch-LCD-1.47 (20 FPS @ 172x320 = 55,040 pixels):
- ESP32-C6 processes **3.9x more pixels** per frame
- ESP32-C6 CPU is **~1.5x slower** per operation (160 MHz RISC-V no FPU vs 240 MHz Xtensa with FPU)
- Net result: ~10.9 FPS is reasonable given 466x466 resolution

### Button UI

| Press Count | Action |
|-------------|--------|
| 1st | Brightness: Max -> Med |
| 2nd | Brightness: Med -> Dim |
| 3rd | Palette advances, brightness resets to Max |

---

## Project Structure

```
plasma-amoled-132/
├── CMakeLists.txt              # Top-level project file
├── sdkconfig.defaults          # ESP32-C6 SDK config
├── partitions.csv              # Custom partition table
├── README.md                   # This file
├── main/
│   ├── CMakeLists.txt          # Component registration
│   ├── idf_component.yml       # esp_lcd_sh8601 dependency
│   └── main.c                  # All source code (~380 lines)
└── managed_components/         # Auto-downloaded by ESP Component Manager
    └── espressif__esp_lcd_sh8601/
```

---

## Lessons Learned

### Critical findings during development:

1. **`max_transfer_sz` must be large enough** — The official demo sets this to the full framebuffer size (`466*466*2`). Setting it too small (e.g., just one line) caused the display to remain black with no errors. The SPI bus uses this to allocate DMA descriptors.

2. **X offset must be added manually** — The display has a 6-pixel X offset. Using `esp_lcd_panel_set_gap()` didn't work as expected. The official demo adds the offset directly in the `draw_bitmap` coordinates.

3. **Rotation must be set AFTER `esp_lcd_panel_init()`** — Sending the `0x36` register command before full panel initialization has no effect. The official demo sets rotation after LVGL port init.

4. **USB serial console requires explicit config** — ESP32-C6 boards using USB-Serial/JTAG need `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in sdkconfig.defaults, otherwise no serial output appears.

5. **Software float is 15x slower than fixed-point** — On the FPU-less ESP32-C6, replacing `float` operations with 8.8 fixed-point integers in the inner loop boosted FPS from 0.7 to 10.9.

6. **AMOLED vs TFT brightness control** — No LEDC PWM needed. The SH8601/CO5300 controller accepts brightness via MIPI DCS command `0x51` (0=off, 255=max), sent using the QSPI write encoding `(0x02 << 24) | (0x51 << 8)`.

### Comparison: ESP32-S3-Touch-LCD-1.47 vs ESP32-C6-Touch-AMOLED-1.32

| Feature | S3-Touch-LCD-1.47 | C6-Touch-AMOLED-1.32 |
|---------|-------------------|----------------------|
| Chip | ESP32-S3 (Xtensa, 240 MHz) | ESP32-C6 (RISC-V, 160 MHz) |
| FPU | Yes (hardware) | No (software) |
| PSRAM | 8 MB | None |
| Display type | TFT (JD9853) | AMOLED (CO5300/SH8601) |
| Resolution | 172x320 | 466x466 |
| Interface | Standard SPI (MOSI+DC) | QSPI (4 data, no DC) |
| Backlight | GPIO46 + LEDC PWM | Register 0x51 |
| Display driver | Custom raw SPI | `esp_lcd_sh8601` component |
| Plasma FPS | ~20 | ~10.9 |

---

## Troubleshooting

### Port busy error
```bash
lsof /dev/cu.usbmodem101 | awk 'NR>1{print $2}' | xargs kill
```

### Verify chip identity
```bash
esptool.py --port /dev/cu.usbmodem101 chip_id
# Should report: ESP32-C6
```

### No serial output
Add to `sdkconfig.defaults` and rebuild from clean:
```
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
```
```bash
rm -rf build sdkconfig && idf.py build
```

### Black screen
1. Verify the board is the **ESP32-C6-Touch-AMOLED-1.32** (not another variant)
2. Flash the official factory firmware from `ESP32-C6-Touch-AMOLED-1.32-Demo/04 Firmware/Fac_V0.0.1.bin` to test hardware
3. Check serial output for init errors — the SH8601 driver logs all failures
4. Ensure `max_transfer_sz` in the SPI bus config is set large enough (at least `466 * 466 * 2`)
5. Ensure X offset (6) is added directly to `draw_bitmap` coordinates, not via `set_gap`
