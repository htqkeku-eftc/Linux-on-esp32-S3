/*
 * JT24 TFT console (JLX130-026, 240x240 RGB565, ST7789V).
 *
 * Replaces the e-paper console task.  The terminal ANSI parser
 * (terminal.c) is unchanged; only the rendering backend differs.
 *
 * Layout: 30 columns x 20 rows, 8px glyph + 4px gap per row.
 *
 * Frame buffer: 240 * 240 * 2 = 115200 bytes of RGB565 in PSRAM.
 *   The frame buffer lives in PSRAM to save internal SRAM.
 *
 * Rendering path:
 *   1. pb_feed() updates the `current` terminal state (ANSI parse).
 *   2. console_task waits up to 20 ms for a chunk, throttles redraws
 *      to ~100 ms, then computes dirty scanlines with pb_dirty_lines().
 *   3. For each dirty scanline, raster_rgb565() fills 480 bytes of
 *      the row in the frame buffer.
 *   4. tft_draw_rect() pushes all changed rows to the panel in one
 *      SPI burst (CASET/RASET + RAMWR covering the full 240x240).
 *   5. shown = current, dirty lines are cleared.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "tft_driver.h"
#include "terminal.h"

static PbTerminal current, shown;
static bool dirty[PB_HEIGHT];

/* Frame buffer: 240 * 240 * 2 bytes = 115200.  PSRAM so it doesn't
   eat internal SRAM that the WiFi/BLE stacks need. */
static uint8_t *fb;   /* pointer, allocated in pb_console_init() */

struct chunk { unsigned len; uint8_t bytes[128]; };
static QueueHandle_t queue;

/*
 * Rasterise one 480-byte scanline into the frame buffer at row `y`.
 * The raster callback fills a local buffer, which is then copied into
 * the global frame buffer.
 */
static void raster_row(unsigned y) {
    uint8_t line[PB_WIDTH];  /* 480 bytes */
    PbRaster r = { &current, &shown };
    raster_rgb565(&r, (int)y, line);
    /* Copy into the frame buffer.  Row y = y * 480 bytes. */
    memcpy(fb + (size_t)y * PB_WIDTH, line, PB_WIDTH);
}

static void push_dirty_rows(void) {
    /* Collect the bounding rectangle of all dirty rows.  The ST7789
       CASET/RASET window can be set to the min/max dirty row so we
       only push what changed.  The panel's unused RAM locations
       retain their previous values. */
    unsigned min_y = PB_HEIGHT, max_y = 0;
    bool any = false;
    for (unsigned y = 0; y < PB_HEIGHT; y++) {
        if (dirty[y]) {
            if (!any) { min_y = y; any = true; }
            max_y = y;
        }
    }
    if (!any) return;

    /* Raster all dirty rows into the frame buffer. */
    for (unsigned y = min_y; y <= max_y; y++) {
        if (dirty[y]) raster_row(y);
    }

    /* Push the bounding rectangle to the panel. */
    uint16_t rows = (uint16_t)(max_y - min_y + 1);
    const uint16_t *fb16 = (const uint16_t *)(fb + (size_t)min_y * PB_WIDTH);
    if (tft_draw_rect(0, (uint16_t)min_y, TFT_WIDTH, rows, fb16) != ESP_OK) {
        ESP_LOGE("paperboard", "tft_draw_rect(%u..%u) failed",
                 (unsigned)min_y, (unsigned)max_y);
    }

    /* Clear dirty flags. */
    memset(dirty, 0, sizeof(dirty));
}

static void console_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    bool pending = false, failed = false;

    for (;;) {
        struct chunk c;
        if (xQueueReceive(queue, &c, pdMS_TO_TICKS(20)) == pdTRUE) {
            pb_feed(&current, c.bytes, c.len);
            pending = true;
        }
        if (!pending || failed ||
            xTaskGetTickCount() - last < pdMS_TO_TICKS(100))
            continue;

        /* Compute which scanlines changed. */
        PbRaster r = { &current, &shown };
        pb_dirty_lines(&r, dirty);
        push_dirty_rows();
        shown = current;
        last  = xTaskGetTickCount();
        pending = false;
    }
}

void pb_console_write(const uint8_t *data, size_t len) {
    if (!queue) return;
    while (len) {
        struct chunk c = { .len = len > 128 ? 128 : len };
        memcpy(c.bytes, data, c.len);
        xQueueSend(queue, &c, portMAX_DELAY);
        data += c.len;
        len  -= c.len;
    }
}

void pb_console_init(void) {
    /* Verify we are on core 0 (WiFi / BT radio ownership). */
    configASSERT(xPortGetCoreID() == 0);

    /* 1. Initialise the ST7789V panel. */
    esp_err_t err = tft_init();
    if (err != ESP_OK) {
        ESP_LOGE("paperboard", "tft_init failed: %s (serial console only)",
                 esp_err_to_name(err));
        printf("paperboard tft v1: display init failed; serial console only\n");
        /* Without a display we still want the queue + task to be
           created so that pb_console_write() doesn't spin forever.
           But the console task will just log a warning on first
           push and stop pushing.  For now, print and return so the
           shell still works over serial. */
        return;
    }

    /* 2. Allocate the frame buffer in PSRAM. */
    fb = heap_caps_malloc(PB_WIDTH * PB_HEIGHT * 2,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) {
        ESP_LOGE("paperboard", "PSRAM frame buffer alloc failed");
        printf("paperboard tft v1: PSRAM fb alloc failed; serial console only\n");
        return;
    }

    /* 3. Clear the panel to black. */
    tft_clear(0x0000);
    tft_backlight_on();

    /* 4. Initialise terminal state. */
    pb_init(&current);
    pb_init(&shown);
    shown.cursor_visible = false;

    const char *banner =
        "Paperboard / JT24\r\n"
        "Linux console: run epd-shell over serial.\r\n\r\n";
    pb_feed(&current, (const uint8_t *)banner, strlen(banner));

    /* 5. Create queue + task.  Pinned to core 0 (SPI is on the
       peripheral bus which is hard-wired to core 0).  Priority 2
       matches the old e-paper task. */
    queue = xQueueCreate(8, sizeof(struct chunk));
    configASSERT(queue);
    configASSERT(xTaskCreatePinnedToCore(console_task, "paperboard", 6144,
                                         NULL, 2, NULL, 0) == pdPASS);

    printf("paperboard tft v1: 240x240 RGB565 ST7789V, 30x20 console\n");
}
