/*
 * Plasma Demo for Waveshare ESP32-C6-Touch-AMOLED-1.32
 *
 * Hardware: ESP32-C6 + 1.32" AMOLED (466x466, CO5300/SH8601, QSPI)
 * Framework: ESP-IDF v5.5.2
 *
 * Key differences from the ESP32-S3-Touch-LCD-1.47 plasma:
 *   - ESP32-C6: single-core RISC-V @ 160 MHz (no PSRAM, no HW FPU)
 *   - AMOLED display via QSPI (4 data lines, no DC pin)
 *   - CO5300/SH8601 display controller (not JD9853/ST7789)
 *   - 466x466 resolution, X offset = 6
 *   - Brightness via AMOLED register 0x51 (no backlight GPIO)
 *   - Uses esp_lcd framework + esp_lcd_sh8601 component for QSPI
 *   - Integer-only inner loop (no per-pixel float) for C6 performance
 *
 * ONE-BUTTON UI (BOOT button GPIO9):
 *   Press cycles brightness: Max -> Med -> Dim
 *   From Dim, next press advances palette and resets to Max
 *
 * Palettes:
 *   0) HSV rainbow
 *   1) Cyclic grayscale
 *   2) Cyclic sunset
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "plasma";

/* ═══════════════════════════════════════════════════
 *  Pin Configuration (ESP32-C6-Touch-AMOLED-1.32)
 * ═══════════════════════════════════════════════════ */
#define PIN_LCD_CS     22
#define PIN_LCD_PCLK   18
#define PIN_LCD_DATA0  19
#define PIN_LCD_DATA1  20
#define PIN_LCD_DATA2  10
#define PIN_LCD_DATA3  11
#define PIN_LCD_RST    21
#define PIN_BTN         9    /* BOOT button (active-low) */
#define PIN_SYS_POWER   2    /* Power hold: drive HIGH to keep board on (battery) */
#define PIN_PWR_BTN     1    /* PWR button (active-low) */

/* ═══════════════════════════════════════════════════
 *  Display Geometry
 * ═══════════════════════════════════════════════════ */
#define TFT_W      466
#define TFT_H      466
#define X_OFFSET     6
#define Y_OFFSET     0

/* ═══════════════════════════════════════════════════
 *  SPI / Display
 * ═══════════════════════════════════════════════════ */
#define LCD_SPI_HOST   SPI2_HOST
#define LCD_SPI_HZ     (40 * 1000 * 1000)
#define LCD_BPP        16

static esp_lcd_panel_handle_t    panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle    = NULL;

/* ═══════════════════════════════════════════════════
 *  Plasma State
 * ═══════════════════════════════════════════════════ */
#define LINE_BUF_LINES  4   /* draw multiple lines per DMA transfer */
static uint16_t *lineBuf = NULL;

static uint8_t  sinLUT[256];
static uint16_t basePalette565[256];
static uint16_t palette565[256];

/* ─── Brightness (3 levels) ─── */
static uint8_t brightnessMode  = 2;
static const uint8_t BR_AMOLED[3] = { 30, 140, 255 };
static const uint8_t BR_SCALE[3]  = { 110, 170, 255 };
static uint8_t brightnessScale = 255;

/* ─── Palette ─── */
static uint8_t paletteMode = 0;

/* ═══════════════════════════════════════════════════
 *  SH8601 Init Commands (from official Waveshare demo)
 * ═══════════════════════════════════════════════════ */

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]) {0x00}, 1, 0},
    {0xC4, (uint8_t[]) {0x80}, 1, 0},       /* QSPI mode enable */
    {0x3A, (uint8_t[]) {0x55}, 1, 0},       /* RGB565 pixel format */
    {0x35, (uint8_t[]) {0x00}, 1, 0},       /* Tearing effect line ON */
    {0x53, (uint8_t[]) {0x20}, 1, 0},       /* Brightness control enable */
    {0x51, (uint8_t[]) {0xFF}, 1, 0},       /* Max brightness */
    {0x63, (uint8_t[]) {0xFF}, 1, 0},
    {0x2A, (uint8_t[]) {0x00, 0x06, 0x01, 0xD7}, 4, 0},  /* CASET: 6..471 */
    {0x2B, (uint8_t[]) {0x00, 0x00, 0x01, 0xD1}, 4, 0},  /* RASET: 0..465 */
    {0x11, (uint8_t[]) {0x00}, 0, 100},     /* Sleep Out */
    {0x29, (uint8_t[]) {0x00}, 0, 0},       /* Display ON */
};

/* ═══════════════════════════════════════════════════
 *  Display Init (QSPI + SH8601/CO5300)
 *  — Mirrors the official Waveshare demo exactly
 * ═══════════════════════════════════════════════════ */

static void lcd_init(void)
{
    ESP_LOGI(TAG, "Initializing QSPI bus...");

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num     = PIN_LCD_PCLK;
    buscfg.data0_io_num    = PIN_LCD_DATA0;
    buscfg.data1_io_num    = PIN_LCD_DATA1;
    buscfg.data2_io_num    = PIN_LCD_DATA2;
    buscfg.data3_io_num    = PIN_LCD_DATA3;
    buscfg.max_transfer_sz = TFT_W * TFT_H * LCD_BPP / 8;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Creating panel IO...");

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num       = PIN_LCD_CS;
    io_config.dc_gpio_num       = -1;
    io_config.spi_mode          = 0;
    io_config.pclk_hz           = LCD_SPI_HZ;
    io_config.trans_queue_depth  = 10;
    io_config.lcd_cmd_bits      = 32;
    io_config.lcd_param_bits    = 8;
    io_config.flags.quad_mode   = true;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Creating SH8601 panel...");

    sh8601_vendor_config_t vendor_config = {};
    vendor_config.init_cmds              = lcd_init_cmds;
    vendor_config.init_cmds_size         = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    vendor_config.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_LCD_RST;
    panel_config.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = LCD_BPP;
    panel_config.vendor_config  = &vendor_config;

    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    ESP_LOGI(TAG, "AMOLED initialized (%dx%d, QSPI, SH8601/CO5300)", TFT_W, TFT_H);
}

/* ═══════════════════════════════════════════════════
 *  Set rotation — called AFTER panel init
 *  Matches official: Lcd_SetRotation(0xC0)
 * ═══════════════════════════════════════════════════ */

static void lcd_set_rotation(uint8_t val)
{
    uint32_t lcd_cmd = 0x36;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    esp_lcd_panel_io_tx_param(io_handle, lcd_cmd, &val, 1);
}

/* ═══════════════════════════════════════════════════
 *  AMOLED Brightness (register 0x51)
 * ═══════════════════════════════════════════════════ */

static void set_amoled_brightness(uint8_t level)
{
    uint32_t lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    esp_lcd_panel_io_tx_param(io_handle, lcd_cmd, &level, 1);
}

/* ═══════════════════════════════════════════════════
 *  Color / Palette Helpers
 * ═══════════════════════════════════════════════════ */

static inline uint16_t rgb565_be(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (c >> 8) | (c << 8);
}

static void hsv2rgb(uint8_t h, uint8_t s, uint8_t v,
                    uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) { *r = *g = *b = v; return; }
    uint8_t region = h / 43;
    uint8_t rem    = (h - region * 43) * 6;
    uint16_t p = (uint16_t)v * (255 - s) / 255;
    uint16_t q = (uint16_t)v * (255 - ((uint16_t)s * rem) / 255) / 255;
    uint16_t t = (uint16_t)v * (255 - ((uint16_t)s * (255 - rem)) / 255) / 255;
    switch (region) {
        case 0:  *r = v;          *g = (uint8_t)t; *b = (uint8_t)p; break;
        case 1:  *r = (uint8_t)q; *g = v;          *b = (uint8_t)p; break;
        case 2:  *r = (uint8_t)p; *g = v;          *b = (uint8_t)t; break;
        case 3:  *r = (uint8_t)p; *g = (uint8_t)q; *b = v;          break;
        case 4:  *r = (uint8_t)t; *g = (uint8_t)p; *b = v;          break;
        default: *r = v;          *g = (uint8_t)p; *b = (uint8_t)q; break;
    }
}

static inline uint8_t clamp8i(int v)
{
    return (uint8_t)((v < 0) ? 0 : (v > 255 ? 255 : v));
}

/* ═══════════════════════════════════════════════════
 *  Sine LUT
 * ═══════════════════════════════════════════════════ */

static void build_sin_lut(void)
{
    for (int i = 0; i < 256; i++) {
        float a = (float)i * (2.0f * M_PI / 256.0f);
        int v = (int)(sinf(a) * 127.5f + 127.5f);
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        sinLUT[i] = (uint8_t)v;
    }
}

static inline uint8_t sin8(uint8_t a) { return sinLUT[a]; }

/* ═══════════════════════════════════════════════════
 *  Brightness Scaling
 * ═══════════════════════════════════════════════════ */

static inline uint16_t scale565_be(uint16_t c_be, uint8_t scale)
{
    uint16_t c = (c_be >> 8) | (c_be << 8);
    uint8_t r5 = (c >> 11) & 0x1F;
    uint8_t g6 = (c >> 5)  & 0x3F;
    uint8_t b5 =  c        & 0x1F;
    r5 = (uint8_t)(((uint16_t)r5 * scale + 127) / 255);
    g6 = (uint8_t)(((uint16_t)g6 * scale + 127) / 255);
    b5 = (uint8_t)(((uint16_t)b5 * scale + 127) / 255);
    if (r5 > 31) r5 = 31;
    if (g6 > 63) g6 = 63;
    if (b5 > 31) b5 = 31;
    uint16_t result = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    return (result >> 8) | (result << 8);
}

static void apply_brightness_to_palette(void)
{
    for (int i = 0; i < 256; i++) {
        palette565[i] = scale565_be(basePalette565[i], brightnessScale);
    }
}

/* ═══════════════════════════════════════════════════
 *  Palette Builders
 * ═══════════════════════════════════════════════════ */

static void build_base_palette(uint8_t mode)
{
    switch (mode % 3) {
    default:
    case 0: {
        for (int i = 0; i < 256; i++) {
            uint8_t r, g, b;
            hsv2rgb((uint8_t)i, 255, 255, &r, &g, &b);
            basePalette565[i] = rgb565_be(r, g, b);
        }
    } break;
    case 1: {
        for (int i = 0; i < 256; i++) {
            float t = (float)i / 255.0f;
            float a = 2.0f * M_PI * t;
            float v = 127.5f + 127.5f * sinf(a);
            uint8_t vv = clamp8i((int)(v + 0.5f));
            basePalette565[i] = rgb565_be(vv, vv, vv);
        }
    } break;
    case 2: {
        for (int i = 0; i < 256; i++) {
            float t = (float)i / 255.0f;
            float a = 2.0f * M_PI * t;
            float r = 185.0f + 70.0f * cosf(a - 0.35f) + 35.0f * cosf(2.0f * a + 0.90f);
            float g =  95.0f + 85.0f * cosf(a - 1.55f) + 25.0f * cosf(2.0f * a - 0.10f);
            float b =  85.0f + 75.0f * cosf(a + 1.35f) + 20.0f * cosf(2.0f * a + 1.20f);
            r += 15.0f; g += 10.0f; b -= 5.0f;
            basePalette565[i] = rgb565_be(
                clamp8i((int)(r + 0.5f)),
                clamp8i((int)(g + 0.5f)),
                clamp8i((int)(b + 0.5f)));
        }
    } break;
    }
}

/* ═══════════════════════════════════════════════════
 *  Button (debounced, active-low with pull-up)
 * ═══════════════════════════════════════════════════ */

static bool ui_button_pressed(void)
{
    static uint32_t lastMs   = 0;
    static bool     lastLevel = true;
    bool level = (gpio_get_level(PIN_BTN) != 0);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (level != lastLevel && (now - lastMs) > 30) {
        lastMs    = now;
        lastLevel = level;
        if (!level) return true;
    }
    return false;
}

/* ═══════════════════════════════════════════════════
 *  Apply Current Brightness
 * ═══════════════════════════════════════════════════ */

static void apply_current_brightness(void)
{
    set_amoled_brightness(BR_AMOLED[brightnessMode]);
    brightnessScale = BR_SCALE[brightnessMode];
    apply_brightness_to_palette();
}

/* ═══════════════════════════════════════════════════
 *  Push scanlines via esp_lcd
 *  (adds X_OFFSET manually, matching official demo)
 * ═══════════════════════════════════════════════════ */

static void push_lines(int y, int num_lines, const uint16_t *buf, int w)
{
    esp_lcd_panel_draw_bitmap(
        panel_handle,
        X_OFFSET, y,
        X_OFFSET + w, y + num_lines,
        buf);
}

/* ═══════════════════════════════════════════════════
 *  Fixed-point warp coefficients (computed once/frame)
 *  Using 8.8 fixed-point for the inner loop to avoid
 *  software float on the FPU-less C6.
 * ═══════════════════════════════════════════════════ */

/* 10.6 fixed-point: multiply, keep upper bits */
#define FP_SHIFT  8
#define FP_ONE    (1 << FP_SHIFT)

static int32_t fp_ca, fp_sa;       /* cos/sin of warp angle, 8.8 */
static int32_t fp_sPerp, fp_sAlong;
static int32_t fp_tx, fp_ty;

static void compute_warp_params(void)
{
    float time_s = (float)esp_timer_get_time() / 1000000.0f;
    float gx = 0.5f * sinf(time_s * 0.37f);
    float gy = 0.5f * cosf(time_s * 0.29f);

    float gmag = sqrtf(gx * gx + gy * gy);
    if (gmag > 1.0f) gmag = 1.0f;

    float ang = atan2f(gy, gx);
    float ca  = cosf(-ang);
    float sa  = sinf(-ang);

    float sAlong = 1.0f - 0.22f * gmag;
    float sPerp  = 1.0f + 0.35f * gmag;

    float fdenom = gmag + 1e-6f;
    float push   = 10.0f * gmag;
    float ftx    = (gx / fdenom) * push;
    float fty    = (gy / fdenom) * push;

    /* Convert to 8.8 fixed-point */
    fp_ca     = (int32_t)(ca     * FP_ONE);
    fp_sa     = (int32_t)(sa     * FP_ONE);
    fp_sPerp  = (int32_t)(sPerp  * FP_ONE);
    fp_sAlong = (int32_t)(sAlong * FP_ONE);
    fp_tx     = (int32_t)(ftx    * FP_ONE);
    fp_ty     = (int32_t)(fty    * FP_ONE);
}

/* ═══════════════════════════════════════════════════
 *  Main Entry
 * ═══════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "Plasma AMOLED demo starting...");

    /* ── Power hold: latch system power ON for battery operation ──
     * The board has a power latch circuit controlled by GPIO 2.
     * When powered from battery, pressing PWR momentarily powers
     * the board, but firmware must drive GPIO 2 HIGH immediately
     * to keep power latched.  Without this, the board shuts off
     * as soon as the PWR button is released.
     * (USB power bypasses this circuit, so it's harmless when on USB.) */
    gpio_config_t pwr_conf = {
        .pin_bit_mask = (1ULL << PIN_SYS_POWER),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_conf);
    gpio_set_level(PIN_SYS_POWER, 1);
    ESP_LOGI(TAG, "Power hold latched (GPIO %d HIGH)", PIN_SYS_POWER);

    /* Button */
    gpio_set_direction(PIN_BTN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BTN, GPIO_PULLUP_ONLY);

    /* Allocate DMA-capable line buffer */
    size_t buf_sz = TFT_W * LINE_BUF_LINES * sizeof(uint16_t);
    lineBuf = (uint16_t *)heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    if (!lineBuf) {
        ESP_LOGE(TAG, "Failed to allocate line buffer (%u bytes)!", (unsigned)buf_sz);
        return;
    }
    ESP_LOGI(TAG, "Line buffer: %u bytes (%d lines)", (unsigned)buf_sz, LINE_BUF_LINES);

    /* Build LUTs and initial palette */
    build_sin_lut();
    build_base_palette(paletteMode);

    /* Init display */
    lcd_init();
    lcd_set_rotation(0xC0);
    apply_current_brightness();

    ESP_LOGI(TAG, "Entering plasma loop (466x466 AMOLED, QSPI)");

    /* ─── Plasma render loop ─── */
    uint32_t t        = 0;
    uint8_t  palShift = 0;

    /* Center in 8.8 fixed-point */
    const int32_t cx_fp = (TFT_W / 2) << FP_SHIFT;
    const int32_t cy_fp = (TFT_H / 2) << FP_SHIFT;

    uint32_t frame_count  = 0;
    int64_t  fps_timer    = esp_timer_get_time();

    while (1) {
        /* ── Button UI ── */
        if (ui_button_pressed()) {
            if (brightnessMode > 0) {
                brightnessMode--;
            } else {
                paletteMode = (paletteMode + 1) % 3;
                build_base_palette(paletteMode);
                brightnessMode = 2;
            }
            apply_current_brightness();
            ESP_LOGI(TAG, "Palette=%u  Brightness=%u", paletteMode, brightnessMode);
        }

        /* ── Compute warp (float, done once per frame) ── */
        compute_warp_params();

        t += 2;
        palShift += 1;

        uint8_t t0 = (uint8_t)t;
        uint8_t t1 = (uint8_t)(t >> 1);
        uint8_t t2 = (uint8_t)(t >> 2);

        int buf_line = 0;
        for (int y = 0; y < TFT_H; y++) {
            /* fy in 8.8 fixed-point */
            int32_t fy_fp = ((int32_t)y << FP_SHIFT) - cy_fp;
            uint16_t *row = &lineBuf[buf_line * TFT_W];

            for (int x = 0; x < TFT_W; x++) {
                int32_t fx_fp = ((int32_t)x << FP_SHIFT) - cx_fp;

                /* Rotate */
                int32_t rx = (fx_fp * fp_ca - fy_fp * fp_sa) >> FP_SHIFT;
                int32_t ry = (fx_fp * fp_sa + fy_fp * fp_ca) >> FP_SHIFT;

                /* Scale along axes */
                rx = (rx * fp_sPerp)  >> FP_SHIFT;
                ry = (ry * fp_sAlong) >> FP_SHIFT;

                /* Rotate back + translate */
                int32_t bx = ((rx * fp_ca + ry * fp_sa) >> FP_SHIFT) + fp_tx;
                int32_t by = ((-rx * fp_sa + ry * fp_ca) >> FP_SHIFT) + fp_ty;

                /* Convert back from 8.8 to pixel-ish coords, scale for plasma */
                uint8_t xx = (uint8_t)(((bx * 3) >> FP_SHIFT) & 0xFF);
                uint8_t yy = (uint8_t)(((by * 2) >> FP_SHIFT) & 0xFF);

                uint16_t sum =
                    sin8(xx + t0) +
                    sin8(yy + t1) +
                    sin8(xx + yy + t2) +
                    sin8(xx - yy - t1);

                row[x] = palette565[(uint8_t)((sum >> 2) + palShift)];
            }

            buf_line++;
            if (buf_line >= LINE_BUF_LINES || y == TFT_H - 1) {
                push_lines(y - buf_line + 1, buf_line, lineBuf, TFT_W);
                buf_line = 0;
            }
        }

        /* FPS counter */
        frame_count++;
        if (frame_count >= 10) {
            int64_t now  = esp_timer_get_time();
            float   elapsed = (float)(now - fps_timer) / 1000000.0f;
            ESP_LOGI(TAG, "FPS: %.1f  palette=%u  brightness=%u",
                     (float)frame_count / elapsed,
                     paletteMode, brightnessMode);
            frame_count = 0;
            fps_timer   = now;
        }

        vTaskDelay(1);
    }
}
