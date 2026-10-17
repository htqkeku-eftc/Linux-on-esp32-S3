#include "terminal.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define PB_RASTER_ATTR IRAM_ATTR
#define PB_FONT_STORAGE DRAM_ATTR
#else
#define PB_RASTER_ATTR
#define PB_FONT_STORAGE
#endif
#include "font8x8_basic.h"

/*
 * JT24 TFT console (240x240 RGB565, ST7789V).
 *
 * Layout: 30 columns x 20 rows.  Each character cell is 8 px wide
 * and 12 px tall (8 px font glyph + 4 px vertical spacing).
 *
 * raster_rgb565(context, y, out):
 *   Fills out[] (PB_WIDTH bytes) with one scan-line of RGB565 pixels.
 *   y is 0..PB_HEIGHT-1.  For y >= PB_ROWS*12 the line is blank.
 *
 * pb_dirty_lines(r, dirty):
 *   Unchanged from the e-paper version -- compares cell row contents
 *   and cursor rows.
 */

/* Per-row RGB565 palette: white foreground on black background. */
#define FG_RGB565  0xFFFF
#define BG_RGB565  0x0000

/* Horizontal spacing: 8 px per column (matches 8x8 font width). */
#define CELL_W 8
/* Vertical: 12 px per row (8 px glyph + 4 px gap). */
#define CELL_H 12
/* Rows of glyph within one row cell: 8.  dy 0..11 -> glyph row dy%8 if dy<8 else gap. */

static void blank(PbCell* c) { c->ch = ' '; c->inverse = 0; }

void pb_init(PbTerminal* t) {
    memset(t, 0, sizeof(*t)); t->cursor_visible = true;
    for (unsigned y=0;y<PB_ROWS;y++)
        for (unsigned x=0;x<PB_COLS;x++)
            blank(&t->cells[y][x]);
}

static void lf(PbTerminal* t) {
    if (++t->y == PB_ROWS) {
        memmove(t->cells[0], t->cells[1], sizeof(t->cells) - sizeof(t->cells[0]));
        t->y = PB_ROWS - 1;
        for (unsigned x=0;x<PB_COLS;x++)
            blank(&t->cells[t->y][x]);
    }
}

static unsigned clamp(unsigned v, unsigned max) { return v>max ? max : v; }

static void erase(PbTerminal* t, unsigned first, unsigned last) {
    for (unsigned i=first;i<=last;i++) {
        PbCell* c = &t->cells[i/PB_COLS][i%PB_COLS];
        blank(c); c->inverse = t->inverse;
    }
}

static void csi(PbTerminal* t, uint8_t c) {
    unsigned n = t->params[0] ? t->params[0] : 1;
    unsigned pos = t->y*PB_COLS + t->x;
    if (t->private_seq) {
        if (t->params[0]==25 && (c=='h'||c=='l'))
            t->cursor_visible = (c=='h');
        return;
    }
    switch(c) {
    case 'A': t->y = n>t->y ? 0 : t->y-n; break;
    case 'B': t->y = clamp(t->y+n, PB_ROWS-1); break;
    case 'C': t->x = clamp(t->x+n, PB_COLS-1); break;
    case 'D': t->x = n>t->x ? 0 : t->x-n; break;
    case 'G': t->x = clamp(n-1, PB_COLS-1); break;
    case 'd': t->y = clamp(n-1, PB_ROWS-1); break;
    case 'H': case 'f':
        t->y = clamp(n-1, PB_ROWS-1);
        t->x = clamp(t->params[1] ? t->params[1]-1 : 0, PB_COLS-1);
        break;
    case 'J':
        if (t->params[0]==0)
            erase(t, pos, PB_ROWS*PB_COLS-1);
        else if (t->params[0]==1)
            erase(t, 0, pos);
        else if (t->params[0]==2)
            erase(t, 0, PB_ROWS*PB_COLS-1);
        break;
    case 'K':
        if (t->params[0]==0)
            erase(t, pos, (t->y+1)*PB_COLS-1);
        else if (t->params[0]==1)
            erase(t, t->y*PB_COLS, pos);
        else if (t->params[0]==2)
            erase(t, t->y*PB_COLS, (t->y+1)*PB_COLS-1);
        break;
    case 'm':
        for (unsigned i=0;i<=t->nparam;i++) {
            if (t->params[i]==0 || t->params[i]==27)
                t->inverse = false;
            else if (t->params[i]==7)
                t->inverse = true;
        }
        break;
    case 's': t->saved_x=t->x; t->saved_y=t->y; break;
    case 'u': t->x=t->saved_x; t->y=t->saved_y; break;
    }
    if (c!='m') t->wrap_pending = false;
}

void pb_feed(PbTerminal* t, const uint8_t* data, size_t len) {
    while (len--) {
        uint8_t c = *data++;
        if (t->state==3) {
            if (c==7) t->state=0;
            else if (c==27) t->state=4;
            continue;
        }
        if (t->state==4) { t->state = (c=='\\')?0:3; continue; }
        if (c==27) { t->state=1; continue; }
        if (t->state==1) {
            t->state=0;
            if (c=='[') {
                t->state=2; t->nparam=0; t->private_seq=false;
                memset(t->params, 0, sizeof(t->params));
            }
            else if (c==']') t->state=3;
            else if (c=='7') { t->saved_x=t->x; t->saved_y=t->y; }
            else if (c=='8') { t->x=t->saved_x; t->y=t->saved_y; t->wrap_pending=false; }
            else if (c=='c') pb_init(t);
            continue;
        }
        if (t->state==2) {
            if (c>='0'&&c<='9')
                t->params[t->nparam] = clamp(t->params[t->nparam]*10 + c-'0', 9999);
            else if (c==';') { if (t->nparam<7) ++t->nparam; else t->state=5; }
            else if (c=='?') t->private_seq = true;
            else if (c>=0x40&&c<=0x7e) { csi(t, c); t->state=0; }
            continue;
        }
        if (t->state==5) { if (c>=0x40&&c<=0x7e) t->state=0; continue; }
        if (c=='\r') { t->x=0; t->wrap_pending=false; }
        else if (c=='\n') { lf(t); t->wrap_pending=false; }
        else if (c=='\b') { if (t->x) --t->x; t->wrap_pending=false; }
        else if (c=='\t') { t->x=clamp((t->x/8+1)*8, PB_COLS-1); t->wrap_pending=false; }
        else if (c>=32 && c!=127) {
            if ((c&0xc0)==0x80) continue;
            if (c>=128) c='?';
            if (t->wrap_pending) { t->x=0; lf(t); t->wrap_pending=false; }
            t->cells[t->y][t->x] = (PbCell){ c, t->inverse };
            if (t->x==PB_COLS-1) t->wrap_pending=true; else ++t->x;
        }
    }
}

/*
 * raster_rgb565: emit one 240-pixel scanline as 480 bytes of RGB565.
 *
 * context = const PbRaster* (next + shown terminals).
 * out     = uint8_t[PB_WIDTH]  (480 bytes)
 *
 * For each column, look up the 8x8 font glyph for the character at
 * (row, col).  The glyph row (dy within 0..7) determines which of the
 * 8 font bytes to use.  dy 8..11 is vertical gap (all background).
 *
 * We render the "next" terminal state (current).  The "shown" state
 * is the last fully-pushed frame; pb_dirty_lines uses it to decide
 * which rows changed.
 */
void PB_RASTER_ATTR raster_rgb565(void* context, int y, uint8_t* out) {
    const PbRaster* r = context;
    if (y < 0 || y >= PB_HEIGHT) {
        /* Fill entire line with background colour. */
        uint16_t bg = BG_RGB565;
        for (unsigned p=0; p<PB_WIDTH; p++) {
            out[p*2]   = (uint8_t)(bg & 0xFF);
            out[p*2+1] = (uint8_t)(bg >> 8);
        }
        return;
    }

    unsigned row = (unsigned)y / CELL_H;
    unsigned dy  = (unsigned)y % CELL_H;   /* 0..11 */
    if (row >= PB_ROWS) {
        uint16_t bg = BG_RGB565;
        for (unsigned p=0; p<PB_WIDTH; p++) {
            out[p*2]   = (uint8_t)(bg & 0xFF);
            out[p*2+1] = (uint8_t)(bg >> 8);
        }
        return;
    }

    const PbTerminal* t = r->next;
    bool cursor_on = t->cursor_visible && (t->y == row);
    unsigned font_row = (dy < 8) ? dy : 8;  /* dy 8..11 -> all-background row */

    for (unsigned col=0; col<PB_COLS; col++) {
        PbCell cell = t->cells[row][col];
        uint8_t ch = (cell.ch < 128) ? cell.ch : '?';
        uint8_t glyph_byte = font8x8_basic[ch][font_row];
        uint16_t fg = FG_RGB565;
        uint16_t bg = BG_RGB565;
        if (cell.inverse) { fg = BG_RGB565; bg = FG_RGB565; }

        /* Cursor is shown as a solid box on the top-left cell of the
           cursor position.  We paint it as a filled rectangle of the
           foreground colour when the cursor is visible and this is
           the cursor row.  For simplicity we only draw the cursor
           in rows 0..7 of the cell (the glyph area). */
        bool cursor_here = cursor_on && col == t->x;

        for (unsigned px=0; px<CELL_W; px++) {
            bool on;
            if (dy >= 8) {
                on = false;  /* gap row: background only */
            } else if (cursor_here) {
                on = true;   /* cursor: solid block in glyph area */
            } else {
                on = (glyph_byte & (1u << (7 - px))) != 0;
            }

            unsigned out_idx = (col * CELL_W + px) * 2;
            uint16_t c = on ? fg : bg;
            out[out_idx]   = (uint8_t)(c & 0xFF);
            out[out_idx+1] = (uint8_t)(c >> 8);
        }
    }
}

/*
 * pb_dirty_lines: unchanged logic.
 * Compares next and shown cell rows and cursor rows.
 */
void pb_dirty_lines(const PbRaster* r, bool dirty[PB_HEIGHT]) {
    memset(dirty, 0, PB_HEIGHT * sizeof(bool));
    for (unsigned row=0; row<PB_ROWS; row++) {
        bool changed =
            memcmp(r->next->cells[row], r->shown->cells[row],
                   sizeof(r->next->cells[row])) != 0;
        if (r->next->cursor_visible  && (r->next->y  == row)) changed = true;
        if (r->shown->cursor_visible && (r->shown->y == row)) changed = true;
        if (changed)
            for (unsigned dy=0; dy<CELL_H; dy++)
                dirty[row * CELL_H + dy] = true;
    }
}
