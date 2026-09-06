// ROADMAP.md M15 1500-line rule: move-only split of panel_spi.c. This file
// owns rect/pixel/line drawing and the 5x7 text renderer -- everything that
// composes a pixel stream on top of the transport primitives in panel_spi.c
// (panel_spi_begin_ram_write(), panel_spi_push_color_run(),
// panel_spi_tx()/panel_spi_chunk()). See panel_spi.c's header comment for
// the full file map and panel_spi_internal.h for the shared declarations.
//
// EXTREME CARE: the glyph renderer's opaque-cell fast path builds each row
// in disp->scratch via panel_spi_encode_pixel() and pushes it with
// panel_spi_tx() -- the same MSB-first RGB565->wire encoding used
// everywhere else in this driver. Preserve it exactly; do not touch pixel
// byte order as part of this move-only split.
#include "panel_spi.h"
#include "panel_spi_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

/* ===================================================================
 * Drawing
 * =================================================================== */

/* Caller holds the lock and has already bounds-checked. */
static esp_err_t ili9488_fill_rect_locked(ILI9488Class *disp, uint16_t x, uint16_t y,
                                          uint16_t w, uint16_t h, uint16_t color)
{
    esp_err_t err = panel_spi_begin_ram_write(disp, x, y, w, h);
    if (err != ESP_OK) return err;
    return panel_spi_push_color_run(disp, color, (uint32_t)w * h);
}

esp_err_t ILI9488_fill_rect(ILI9488Class *disp, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h, uint16_t color)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = panel_spi_reject_if_blitting(disp);
    if (err == ESP_OK) {
        err = panel_spi_rect_in_bounds(disp, x, y, w, h)
                  ? ili9488_fill_rect_locked(disp, x, y, w, h, color)
                  : ESP_ERR_INVALID_ARG;
    }
    panel_spi_unlock(disp);
    return err;
}

esp_err_t ILI9488_clear(ILI9488Class *disp, uint16_t color)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = panel_spi_reject_if_blitting(disp);
    if (err == ESP_OK) {
        err = ili9488_fill_rect_locked(disp, 0, 0, disp->width, disp->height, color);
        if (err == ESP_OK) {
            disp->cursor_x = 0;
            disp->cursor_y = 0;
        }
    }
    panel_spi_unlock(disp);
    return err;
}

esp_err_t ILI9488_draw_pixel(ILI9488Class *disp, uint16_t x, uint16_t y, uint16_t color)
{
    return ILI9488_fill_rect(disp, x, y, 1, 1, color);
}

esp_err_t ILI9488_draw_rect(ILI9488Class *disp, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h, uint16_t color)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = panel_spi_reject_if_blitting(disp);
    if (err != ESP_OK) {
        panel_spi_unlock(disp);
        return err;
    }
    if (!panel_spi_rect_in_bounds(disp, x, y, w, h)) {
        panel_spi_unlock(disp);
        return ESP_ERR_INVALID_ARG;
    }

    /* Four fills rather than a rect-shaped stream: each edge is one window
     * plus one contiguous run, and the two vertical edges skip the corners
     * the horizontal ones already covered. A 1- or 2-pixel-thin rectangle
     * degenerates correctly because the horizontal edges alone cover it. */
    err = ili9488_fill_rect_locked(disp, x, y, w, 1, color);
    if (err == ESP_OK && h > 1) {
        err = ili9488_fill_rect_locked(disp, x, (uint16_t)(y + h - 1), w, 1, color);
    }
    if (err == ESP_OK && h > 2) {
        err = ili9488_fill_rect_locked(disp, x, (uint16_t)(y + 1), 1, (uint16_t)(h - 2), color);
        if (err == ESP_OK && w > 1) {
            err = ili9488_fill_rect_locked(disp, (uint16_t)(x + w - 1), (uint16_t)(y + 1),
                                           1, (uint16_t)(h - 2), color);
        }
    }
    panel_spi_unlock(disp);
    return err;
}

/* Bresenham, but emitting RUNS rather than pixels.
 *
 * Every single-pixel write on this panel costs a CASET + PASET + RAMWR
 * (three commands, up to two D/C toggles) for three bytes of payload, so a
 * per-pixel line would be dominated by expander traffic. Grouping the
 * consecutive pixels that share a row (for shallow lines) or a column (for
 * steep ones) into one fill turns a 480px diagonal from 480 window setups
 * into a handful, and costs nothing for the degenerate 45-degree case where
 * every run is length 1. */
static esp_err_t ili9488_draw_line_locked(ILI9488Class *disp, int x0, int y0, int x1, int y1,
                                          uint16_t color)
{
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;

    if (dy == 0) {
        int left = (x0 < x1) ? x0 : x1;
        return ili9488_fill_rect_locked(disp, (uint16_t)left, (uint16_t)y0, (uint16_t)(dx + 1), 1, color);
    }
    if (dx == 0) {
        int top = (y0 < y1) ? y0 : y1;
        return ili9488_fill_rect_locked(disp, (uint16_t)x0, (uint16_t)top, 1, (uint16_t)(dy + 1), color);
    }

    if (dx >= dy) {
        /* Shallow: step x every iteration, y occasionally -> horizontal runs. */
        int err_acc = dx / 2;
        int y = y0;
        int run_start = x0;
        for (int x = x0;; x += sx) {
            bool last = (x == x1);
            int next_y = y;
            err_acc -= dy;
            if (err_acc < 0) {
                next_y = y + sy;
                err_acc += dx;
                /* The error term can round one step past the endpoint on the
                 * final iteration. Clamping here is what keeps a rounding
                 * artifact from becoming an out-of-bounds write. */
                if ((sy > 0 && next_y > y1) || (sy < 0 && next_y < y1)) next_y = y;
            }
            if (next_y != y || last) {
                int a = (sx > 0) ? run_start : x;
                int b = (sx > 0) ? x : run_start;
                esp_err_t err = ili9488_fill_rect_locked(disp, (uint16_t)a, (uint16_t)y,
                                                         (uint16_t)(b - a + 1), 1, color);
                if (err != ESP_OK) return err;
                run_start = x + sx;
                y = next_y;
            }
            if (last) break;
        }
    } else {
        /* Steep: step y every iteration -> vertical runs. */
        int err_acc = dy / 2;
        int x = x0;
        int run_start = y0;
        for (int y = y0;; y += sy) {
            bool last = (y == y1);
            int next_x = x;
            err_acc -= dx;
            if (err_acc < 0) {
                next_x = x + sx;
                err_acc += dy;
                if ((sx > 0 && next_x > x1) || (sx < 0 && next_x < x1)) next_x = x;
            }
            if (next_x != x || last) {
                int a = (sy > 0) ? run_start : y;
                int b = (sy > 0) ? y : run_start;
                esp_err_t err = ili9488_fill_rect_locked(disp, (uint16_t)x, (uint16_t)a,
                                                         1, (uint16_t)(b - a + 1), color);
                if (err != ESP_OK) return err;
                run_start = y + sy;
                x = next_x;
            }
            if (last) break;
        }
    }
    return ESP_OK;
}

esp_err_t ILI9488_draw_line(ILI9488Class *disp, uint16_t x0, uint16_t y0,
                            uint16_t x1, uint16_t y1, uint16_t color)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = panel_spi_reject_if_blitting(disp);
    if (err == ESP_OK) {
        /* Both endpoints must be on-screen. Clipping is deliberately not
         * implemented: a silently-clipped line looks like a drawing bug,
         * while a rejected one points straight at the caller. */
        if (x0 >= disp->width || x1 >= disp->width || y0 >= disp->height || y1 >= disp->height) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            err = ili9488_draw_line_locked(disp, x0, y0, x1, y1, color);
        }
    }
    panel_spi_unlock(disp);
    return err;
}

/* ===================================================================
 * Text
 * =================================================================== */

esp_err_t ILI9488_set_text_cursor(ILI9488Class *disp, uint16_t x, uint16_t y)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (x >= disp->width || y >= disp->height) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        disp->cursor_x = x;
        disp->cursor_y = y;
    }
    panel_spi_unlock(disp);
    return err;
}

esp_err_t ILI9488_set_text_style(ILI9488Class *disp, uint16_t fg, uint16_t bg,
                                 uint8_t size, bool opaque_background)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;
    if (size < 1 || size > ILI9488_TEXT_SIZE_MAX) return ESP_ERR_INVALID_ARG;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    disp->text_fg = fg;
    disp->text_bg = bg;
    disp->text_size = size;
    disp->text_opaque = opaque_background;
    panel_spi_unlock(disp);
    return ESP_OK;
}

/* Draws one glyph cell. Two strategies, picked by whether the background is
 * being painted:
 *
 *  - Opaque: the whole cell is one window and one contiguous pixel stream
 *    (foreground and background both), built one source row at a time in the
 *    scratch and pushed `size` times. One window setup per glyph.
 *  - Transparent: only lit pixels are drawn, as horizontal runs of set bits.
 *    That is more window setups but it is the only way to leave the existing
 *    background untouched with no framebuffer to composite against.
 */
static esp_err_t ili9488_draw_glyph_locked(ILI9488Class *disp, uint16_t x, uint16_t y,
                                           unsigned char c)
{
    const uint8_t *glyph = panel_spi_font5x7[c - 0x20];
    uint8_t size = disp->text_size;
    uint8_t bpp = disp->panel->bytes_per_pixel;
    uint16_t cell_w = (uint16_t)(ILI9488_FONT_CELL_WIDTH * size);
    uint16_t cell_h = (uint16_t)(ILI9488_FONT_CELL_HEIGHT * size);
    size_t row_bytes = (size_t)cell_w * bpp;

    if (disp->text_opaque && row_bytes <= panel_spi_chunk(disp)) {
        esp_err_t err = panel_spi_begin_ram_write(disp, x, y, cell_w, cell_h);
        if (err != ESP_OK) return err;

        uint8_t fg[3], bg[3];
        panel_spi_encode_pixel(disp, disp->text_fg, fg);
        panel_spi_encode_pixel(disp, disp->text_bg, bg);

        for (int row = 0; row < ILI9488_FONT_CELL_HEIGHT; ++row) {
            size_t n = 0;
            for (int col = 0; col < ILI9488_FONT_CELL_WIDTH; ++col) {
                /* Column 5 is the inter-glyph gap and row 7 the blank row
                 * below, both always background -- the same 6x8 cell the
                 * SSD1306 driver renders. */
                bool lit = (col < ILI9488_FONT_GLYPH_WIDTH) &&
                           (row < ILI9488_FONT_GLYPH_HEIGHT) &&
                           ((glyph[col] >> row) & 0x01);
                const uint8_t *px = lit ? fg : bg;
                for (uint8_t s = 0; s < size; ++s) {
                    memcpy(&disp->scratch[n], px, bpp);
                    n += bpp;
                }
            }
            /* Vertical scaling is free: push the same row `size` times. */
            for (uint8_t s = 0; s < size; ++s) {
                err = panel_spi_tx(disp, disp->scratch, n);
                if (err != ESP_OK) return err;
            }
        }
        return ESP_OK;
    }

    /* Transparent (or a cell too wide for the configured chunk size). */
    if (disp->text_opaque) {
        esp_err_t err = ili9488_fill_rect_locked(disp, x, y, cell_w, cell_h, disp->text_bg);
        if (err != ESP_OK) return err;
    }
    for (int row = 0; row < ILI9488_FONT_GLYPH_HEIGHT; ++row) {
        int col = 0;
        while (col < ILI9488_FONT_GLYPH_WIDTH) {
            if (!((glyph[col] >> row) & 0x01)) {
                ++col;
                continue;
            }
            int run = 0;
            while (col + run < ILI9488_FONT_GLYPH_WIDTH &&
                   ((glyph[col + run] >> row) & 0x01)) {
                ++run;
            }
            esp_err_t err = ili9488_fill_rect_locked(disp,
                                                     (uint16_t)(x + col * size),
                                                     (uint16_t)(y + row * size),
                                                     (uint16_t)(run * size),
                                                     size, disp->text_fg);
            if (err != ESP_OK) return err;
            col += run;
        }
    }
    return ESP_OK;
}

static esp_err_t ili9488_write_char_locked(ILI9488Class *disp, char c)
{
    uint8_t size = disp->text_size;
    uint16_t cell_w = (uint16_t)(ILI9488_FONT_CELL_WIDTH * size);
    uint16_t cell_h = (uint16_t)(ILI9488_FONT_CELL_HEIGHT * size);

    if (c == '\n') {
        disp->cursor_x = 0;
        disp->cursor_y = (uint16_t)(disp->cursor_y + cell_h);
        if ((uint32_t)disp->cursor_y + cell_h > disp->height) disp->cursor_y = 0;
        return ESP_OK;
    }
    if (c == '\r') {
        disp->cursor_x = 0;
        return ESP_OK;
    }

    unsigned char uc = (unsigned char)c;
    if (uc < 0x20 || uc > 0x7F) {
        uc = '?';  /* same substitution the SSD1306 driver makes */
    }

    /* Wrap at the right edge, then from the bottom back to the top. No
     * scrolling: scrolling would mean reading the panel back, and read-back
     * on this interface is capped at 6.6 MHz for 3 bytes per pixel -- a full
     * screen would take the better part of a second. */
    if ((uint32_t)disp->cursor_x + cell_w > disp->width) {
        disp->cursor_x = 0;
        disp->cursor_y = (uint16_t)(disp->cursor_y + cell_h);
    }
    if ((uint32_t)disp->cursor_y + cell_h > disp->height) {
        disp->cursor_y = 0;
    }
    if ((uint32_t)disp->cursor_x + cell_w > disp->width ||
        (uint32_t)disp->cursor_y + cell_h > disp->height) {
        /* One glyph is bigger than the whole screen -- only reachable with an
         * absurd text size on a narrow rotation, but it must not write out
         * of bounds. The vertical half matters as much as the horizontal:
         * neither ili9488_draw_glyph_locked nor panel_spi_begin_ram_write
         * bounds-checks, they trust this. */
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ili9488_draw_glyph_locked(disp, disp->cursor_x, disp->cursor_y, uc);
    if (err != ESP_OK) return err;
    disp->cursor_x = (uint16_t)(disp->cursor_x + cell_w);
    return ESP_OK;
}

esp_err_t ILI9488_write_char(ILI9488Class *disp, char c)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = panel_spi_reject_if_blitting(disp);
    if (err == ESP_OK) {
        err = ili9488_write_char_locked(disp, c);
    }
    panel_spi_unlock(disp);
    return err;
}

/* Takes the lock once for the whole string rather than per character: a
 * half-printed string interleaved with another task's drawing is both ugly
 * and, because the cursor is shared state, wrong. */
esp_err_t ILI9488_write(ILI9488Class *disp, const char *data, size_t len)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;
    if (!data && len > 0) return ESP_ERR_INVALID_ARG;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = panel_spi_reject_if_blitting(disp);
    for (size_t i = 0; err == ESP_OK && i < len; ++i) {
        err = ili9488_write_char_locked(disp, data[i]);
    }
    panel_spi_unlock(disp);
    return err;
}

esp_err_t ILI9488_write_str(ILI9488Class *disp, const char *str)
{
    if (!str) return ESP_ERR_INVALID_ARG;
    return ILI9488_write(disp, str, strlen(str));
}

esp_err_t ILI9488_vprintf(ILI9488Class *disp, const char *fmt, va_list args)
{
    if (!fmt) return ESP_ERR_INVALID_ARG;
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;

    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (needed < 0) return ESP_FAIL;

    char *buf = malloc((size_t)needed + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    esp_err_t err = ILI9488_write(disp, buf, (size_t)needed);
    free(buf);
    return err;
}

esp_err_t ILI9488_printf(ILI9488Class *disp, const char *fmt, ...)
{
    if (!fmt) return ESP_ERR_INVALID_ARG;
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;
    va_list args;
    va_start(args, fmt);
    esp_err_t err = ILI9488_vprintf(disp, fmt, args);
    va_end(args);
    return err;
}
