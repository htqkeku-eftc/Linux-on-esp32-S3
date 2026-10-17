#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PB_COLS 30
#define PB_ROWS 20
#define PB_WIDTH 240
#define PB_HEIGHT 240
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint8_t ch, inverse; } PbCell;
typedef struct {
    PbCell cells[PB_ROWS][PB_COLS];
    unsigned x, y, saved_x, saved_y, state, nparam, params[8];
    bool inverse, wrap_pending, cursor_visible, private_seq;
} PbTerminal;
void pb_init(PbTerminal* t);
void pb_feed(PbTerminal* t, const uint8_t* data, size_t len);
typedef struct { const PbTerminal *next, *shown; } PbRaster;
/* Callback: emit one 240-pixel RGB565 scanline (480 bytes). */
void raster_rgb565(void* context, int y, uint8_t* out);
void pb_dirty_lines(const PbRaster* r, bool dirty[PB_HEIGHT]);
#ifdef __cplusplus
}
#endif
