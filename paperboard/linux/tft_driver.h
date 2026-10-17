#pragma once
#include <stdint.h>
#include "esp_err.h"

/*
 * JT24 TFT driver (ST7789V, 240x240 RGB565, 4-wire SPI).
 *
 * Wiring (ESP32-S3):
 *   SCK  -> GPIO 6
 *   SDA  -> GPIO 7  (MOSI)
 *   CS   -> GPIO 5
 *   RS   -> GPIO 8  (A0: 0 = command, 1 = data)
 *   RST  -> GPIO 9  (low = reset pulse)
 *   LEDA -> GPIO 10 (backlight, active high)
 *   VDD  -> 3.3V
 *   VSS  -> GND
 */

/* 240x240 display */
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

esp_err_t tft_init(void);
esp_err_t tft_clear(uint16_t color);
esp_err_t tft_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const uint16_t *rgb565_pixels);
void      tft_backlight_on(void);
void      tft_backlight_off(void);

/* Pack RGB888 -> RGB565 (big-endian 16-bit word). */
static inline uint16_t tft_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
