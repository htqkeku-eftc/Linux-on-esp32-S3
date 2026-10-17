/*
 * ST7789V TFT driver for JLX130-026 (240x240 RGB565, 4-wire SPI).
 *
 * Uses the ESP-IDF SPI master driver.  CS and RS are driven manually
 * via GPIO (the SPI bus's built-in CS is not used, to allow per-call
 * RS control).  All transactions run on core 0 before linux_boot().
 */
#include "tft_driver.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "TFT";

/* Hardware mapping. */
#define TFT_SCK_IO   6
#define TFT_SDA_IO   7   /* MOSI */
#define TFT_CS_IO    5
#define TFT_RS_IO    8   /* A0 */
#define TFT_RST_IO   9
#define TFT_LED_IO   10

/* ST7789V commands. */
#define CMD_SWRESET   0x01
#define CMD_SLPOUT    0x11
#define CMD_INVOFF    0x20
#define CMD_INVON     0x21
#define CMD_DISPOFF   0x28
#define CMD_DISPON    0x29
#define CMD_CASET     0x2A
#define CMD_RASET     0x2B
#define CMD_RAMWR     0x2C
#define CMD_MADCTL    0x36
#define CMD_COLMOD    0x3A

static spi_device_handle_t spi;
static bool inited;

/* --- low-level helpers ------------------------------------------------ */

/* Push a single 8-bit command byte over SPI.  RS is held LOW by the
   caller.  Uses SPI_TRANS_USE_TXDATA — tx_data[0] is read directly by
   the driver, so no separate tx_buffer is needed. */
static esp_err_t tft_cmd(uint8_t cmd) {
    struct spi_transaction_t tx = {
        .length = 8,
        .flags  = SPI_TRANS_USE_TXDATA,
    };
    tx.tx_data[0] = cmd;
    esp_err_t err = spi_device_transmit(spi, &tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cmd 0x%02X: %s", cmd, esp_err_to_name(err));
    }
    return err;
}

/*
 * Push `len` bytes of pixel data over SPI with RS HIGH.
 * Buffers are read from `buf` pointer directly (no USE_TXDATA needed
 * when `.data` is set).
 */
static esp_err_t tft_data(const uint8_t *buf, size_t len) {
    const size_t CHUNK = 480;  /* one scanline of 240px * 2 bytes */
    size_t off = 0;
    while (off < len) {
        size_t n = (len - off) > CHUNK ? CHUNK : (len - off);
        struct spi_transaction_t tx = {
            .length  = (int)n,
            .data    = buf + off,
        };
        esp_err_t err = spi_device_transmit(spi, &tx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "data @%u: %s", (unsigned)off, esp_err_to_name(err));
            return err;
        }
        off += n;
    }
    return ESP_OK;
}

/* RS pin state. */
static inline void rs_low(void)  { gpio_set_level(TFT_RS_IO, 0); }
static inline void rs_high(void) { gpio_set_level(TFT_RS_IO, 1); }

/* Set a CASET window (x_start .. x_end).  Values are 0..239, so the
   high byte is always 0x00. */
static esp_err_t tft_set_col_window(uint16_t x_start, uint16_t x_end) {
    rs_low();
    if (tft_cmd(CMD_CASET) != ESP_OK) { rs_high(); return ESP_FAIL; }
    rs_high();
    uint8_t d[4] = { 0x00, (uint8_t)x_start, 0x00, (uint8_t)x_end };
    rs_low();
    if (tft_data(d, 4) != ESP_OK) { rs_high(); return ESP_FAIL; }
    rs_high();
    return ESP_OK;
}

/* Set a RASET window (y_start .. y_end). */
static esp_err_t tft_set_row_window(uint16_t y_start, uint16_t y_end) {
    rs_low();
    if (tft_cmd(CMD_RASET) != ESP_OK) { rs_high(); return ESP_FAIL; }
    rs_high();
    uint8_t d[4] = { 0x00, (uint8_t)y_start, 0x00, (uint8_t)y_end };
    rs_low();
    if (tft_data(d, 4) != ESP_OK) { rs_high(); return ESP_FAIL; }
    rs_high();
    return ESP_OK;
}

/* Begin pixel write (sends CMD_RAMWR).  Caller must hold RS in the
   state that tft_data expects — data mode (RS HIGH). */
static esp_err_t tft_begin_write(void) {
    rs_low();
    esp_err_t err = tft_cmd(CMD_RAMWR);
    rs_high();
    if (err != ESP_OK) return ESP_FAIL;
    return ESP_OK;
}

/* --- public API -------------------------------------------------------- */

esp_err_t tft_init(void) {
    if (inited) return ESP_OK;

    /* 1. Configure GPIOs. */
    gpio_config_t cs_conf = {
        .pin_bit_mask = (1ULL << TFT_CS_IO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs_conf);
    gpio_set_level(TFT_CS_IO, 1);  /* CS high = unselected */

    gpio_config_t ctrl_conf = {
        .pin_bit_mask = (1ULL << TFT_RS_IO) | (1ULL << TFT_RST_IO) | (1ULL << TFT_LED_IO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&ctrl_conf);
    gpio_set_level(TFT_RS_IO,  1);
    gpio_set_level(TFT_RST_IO, 1);
    gpio_set_level(TFT_LED_IO, 0);

    /* 2. Reset pulse. */
    gpio_set_level(TFT_RST_IO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TFT_RST_IO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* 3. SPI bus. */
    const spi_bus_config_t buscfg = {
        .mosi_io_num     = TFT_SDA_IO,
        .miso_io_num     = -1,
        .sclk_io_num     = TFT_SCK_IO,
        .quad_wp_io_num  = -1,
        .quad_hd_io_num  = -1,
        .max_transfer_sz  = 512,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, -1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_init: %s", esp_err_to_name(err));
        return err;
    }

    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,  /* 10 MHz */
        .mode           = 0,
        .spi2cv_half_bit = false,
        .queue_size     = 3,
        .flags          = 0,
        .cs_io          = -1,  /* manual CS via GPIO */
    };
    err = spi_bus_add_device(SPI2_HOST, &devcfg, &spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_add_device: %s", esp_err_to_name(err));
        return err;
    }

    /* 4. Software reset. */
    rs_low();
    if (tft_cmd(CMD_SWRESET) != ESP_OK) { rs_high(); return ESP_FAIL; }
    rs_high();
    vTaskDelay(pdMS_TO_TICKS(120));

    /* 5. Sleep out. */
    rs_low();
    if (tft_cmd(CMD_SLPOUT) != ESP_OK) { rs_high(); return ESP_FAIL; }
    rs_high();
    vTaskDelay(pdMS_TO_TICKS(120));

    /* 6. Colour mode: 16-bit RGB565. */
    rs_low();
    if (tft_cmd(CMD_COLMOD) != ESP_OK) { rs_high(); return ESP_FAIL; }
    rs_high();
    uint8_t colmode = 0x05;
    rs_low(); tft_data(&colmode, 1); rs_high();

    /* 7. Memory access: normal orientation. */
    rs_low();
    if (tft_cmd(CMD_MADCTL) != ESP_OK) { rs_high(); return ESP_FAIL; }
    rs_high();
    uint8_t madctl = 0x00;
    rs_low(); tft_data(&madctl, 1); rs_high();

    /* 8. Invert ON. */
    rs_low();
    if (tft_cmd(CMD_INVON) != ESP_OK) { rs_high(); return ESP_FAIL; }
    rs_high();

    /* 9. Display ON. */
    rs_low();
    if (tft_cmd(CMD_DISPON) != ESP_OK) { rs_high(); return ESP_FAIL; }
    rs_high();
    vTaskDelay(pdMS_TO_TICKS(20));

    inited = true;
    ESP_LOGI(TAG, "ST7789V initialised (240x240 RGB565)");
    return ESP_OK;
}

esp_err_t tft_clear(uint16_t color) {
    if (!inited) return ESP_ERR_INVALID_STATE;

    if (tft_set_col_window(0, TFT_WIDTH - 1) != ESP_OK)  return ESP_FAIL;
    if (tft_set_row_window(0, TFT_HEIGHT - 1) != ESP_OK) return ESP_FAIL;
    if (tft_begin_write() != ESP_OK)                      return ESP_FAIL;

    /* Push a solid rectangle of `color`.  240 rows × 480 bytes/row.
       The 480-byte buffer is static so it doesn't stress the 8 KB
       FreeRTOS task stack. */
    static uint8_t row[TFT_WIDTH * 2];
    for (int i = 0; i < TFT_WIDTH; i++) {
        row[i*2]   = (uint8_t)(color & 0xFF);
        row[i*2+1] = (uint8_t)(color >> 8);
    }
    for (int r = 0; r < TFT_HEIGHT; r++) {
        if (tft_data(row, sizeof(row)) != ESP_OK) return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t tft_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                        const uint16_t *rgb565_pixels) {
    if (!inited) return ESP_ERR_INVALID_STATE;
    if (w == 0 || h == 0) return ESP_OK;
    if (x + w > TFT_WIDTH || y + h > TFT_HEIGHT) return ESP_ERR_INVALID_ARG;

    if (tft_set_col_window(x, x + w - 1) != ESP_OK)  return ESP_FAIL;
    if (tft_set_row_window(y, y + h - 1) != ESP_OK)  return ESP_FAIL;
    if (tft_begin_write() != ESP_OK)                  return ESP_FAIL;

    size_t row_bytes = (size_t)w * 2;
    for (uint16_t r = 0; r < h; r++) {
        const uint8_t *row = (const uint8_t *)(rgb565_pixels + (size_t)r * w);
        if (tft_data(row, row_bytes) != ESP_OK) return ESP_FAIL;
    }
    return ESP_OK;
}

void tft_backlight_on(void)  { if (inited) gpio_set_level(TFT_LED_IO, 1); }
void tft_backlight_off(void)  { if (inited) gpio_set_level(TFT_LED_IO, 0); }
