// ILI9488 480x320 SPI TFT driver (BIGTREETECH TFT35 SPI V2.1 on J2).
//
// Replaces the SSD1306 OLED driver from the unit-test fixture. Two things
// about this panel shape every design decision in here, and both are worth
// understanding before touching anything:
//
// 1. D/C AND ~RESET ARE NOT GPIOs. They are SX1509 expander pins (IO14/IO15,
//    see docs/HARDWARE.md), so every command/data transition costs a full I2C
//    round trip -- microseconds of SPI framed by ~100us of I2C. The driver
//    therefore batches: exactly ONE D/C toggle per command (low for the
//    opcode, high for that command's entire parameter or pixel payload), and
//    the payload goes out in as few spi_owner_transfer() calls as the SPI max
//    transfer size allows. D/C is NEVER toggled per byte, and it is left
//    parked in "data" for the whole duration of a fill or a blit. A shadow of
//    the last level lets back-to-back data pushes skip the I2C write
//    entirely. See ili9488_set_dc() in the .c file.
//
// 2. NO FRAMEBUFFER. 480 x 320 x 3 bytes = 460,800 bytes -- more than the
//    ESP32-S3's internal RAM. Every draw call therefore goes straight to the
//    panel's own frame memory, which is why (unlike SSD1306) there is no
//    ILI9488_display() flush step: by the time a draw call returns, the pixels
//    are already on the glass. The cost is that nothing can be read back or
//    composited; drawing is write-only and order-dependent.
//
// Colors on this API are RGB565 uint16_t, the same 16-bit form the UART
// protocol carries. The panel cannot accept RGB565 over 4-line SPI (see the
// RGB666 note in ILI9488.c); the conversion to 18-bit happens in exactly one
// place inside the driver.
#ifndef ILI9488_H
#define ILI9488_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_spi_owner.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kiln_io.h"

#ifdef __cplusplus
extern "C" {
#endif

// Panel geometry in its native (unrotated) orientation. The controller's
// frame memory is 320 columns x 480 rows regardless of how MADCTL is set;
// rotation only changes the scan order, never the memory size.
#define ILI9488_PANEL_WIDTH  320
#define ILI9488_PANEL_HEIGHT 480

// Font cell geometry, identical to the SSD1306 driver's: a 5x7 glyph plus a
// 1px inter-glyph gap = 6px wide, in an 8px-tall cell (7 glyph rows + 1 blank
// row below). At text size N every dimension is multiplied by N.
#define ILI9488_FONT_GLYPH_WIDTH  5
#define ILI9488_FONT_GLYPH_HEIGHT 7
#define ILI9488_FONT_CELL_WIDTH   6
#define ILI9488_FONT_CELL_HEIGHT  8
#define ILI9488_TEXT_SIZE_MAX     8

// Pixels held by the driver's reusable scratch buffer. One full landscape row
// (480 px = 1440 bytes of RGB666) is the natural unit: every fill, glyph and
// blit chunk is built here and pushed in one transfer, so no draw call ever
// allocates.
//
// The SPI bus this driver is added to MUST have been created with
// max_transfer_sz >= ILI9488_SCRATCH_BYTES (i.e. with DMA enabled -- a
// SPI_DMA_DISABLED bus caps transfers at 64 bytes). If the bus owner cannot
// promise that, lower disp->chunk_bytes after ILI9488_init(); correctness is
// unaffected, only throughput.
#define ILI9488_SCRATCH_PIXELS 480
#define ILI9488_SCRATCH_BYTES  (ILI9488_SCRATCH_PIXELS * 3)

// Reads are clocked much slower than writes -- the datasheet's serial clock
// cycle minimum is 50ns for a write but 150ns for a read (§17.4.3), i.e.
// 20 MHz vs 6.67 MHz. The driver keeps a second spi_device_handle_t on the
// same CS purely so ILI9488_read_id() can drop to a legal read clock without
// slowing everything else down.
#define ILI9488_READ_CLOCK_HZ 4000000

/* State of the streaming blit (DISPLAY_CMD_BLIT_BEGIN/DATA/END). A blit
 * deliberately spans multiple UART frames with the panel's pixel window left
 * open, so this state has to outlive any single call -- which also means
 * out-of-order commands have to be rejected explicitly rather than being
 * caught by the shape of the call sequence. */
typedef struct {
    bool active;            /* a window is open and awaiting pixels */
    uint16_t x, y, w, h;    /* the open window, in rotated coordinates */
    uint32_t pixels_total;  /* w * h -- how many the window will accept */
    uint32_t pixels_done;   /* how many have been streamed so far */
} ILI9488BlitState;

typedef struct {
    /* Borrowed, never owned: the SPI bus, its owner task and the expander
     * handle all belong to whoever created them (see main.c). Deinit must not
     * free any of them. */
    spi_owner_t *owner;
    kiln_io_t *io;                 /* SX1509, carries D/C and ~RESET -- unused for
                                     * whichever of the two dc_gpio/reset_gpio below
                                     * is >= 0 instead */
    spi_device_handle_t dev;       /* fast device config, used for all writes */
    spi_device_handle_t read_dev;  /* slow device config, used only for reads */
    int cs_gpio;
    int dc_gpio;    /* >= 0: drive D/C directly on this ESP32 GPIO instead of the
                      * expander (bench wiring); -1: use io/kiln_io_lcd_dc as normal */
    int reset_gpio; /* same idea as dc_gpio, for ~RESET */

    uint16_t panel_width;   /* native, unrotated -- 320 */
    uint16_t panel_height;  /* native, unrotated -- 480 */
    uint16_t width;         /* as currently rotated; what bounds checks use */
    uint16_t height;
    uint8_t rotation;       /* 0-3 */
    uint8_t madctl;         /* shadow of what was last written to 36h */

    /* D/C shadow. dc_valid is false until the first explicit set, so the
     * driver never assumes a level it has not itself written (the expander
     * may have been reset underneath it). */
    bool dc_is_data;
    bool dc_valid;

    /* Text state, mirroring DISPLAY_CMD_SET_TEXT_CURSOR / SET_TEXT_STYLE. */
    uint16_t cursor_x, cursor_y;
    uint16_t text_fg, text_bg;
    uint8_t text_size;      /* integer scale, 1..ILI9488_TEXT_SIZE_MAX */
    bool text_opaque;       /* false = draw only lit pixels, leave bg alone */

    ILI9488BlitState blit;

    /* Reusable DMA-capable scratch, allocated once in init. Guarded by lock,
     * like everything else that touches the wire. */
    uint8_t *scratch;
    size_t chunk_bytes;     /* max bytes per SPI transfer; <= ILI9488_SCRATCH_BYTES */

    /* Serializes anything that spans more than one transfer. A window set
     * followed by a pixel write is two-to-four transfers with the panel's
     * address counter live in between; a second caller landing in that gap
     * would write its pixels into this caller's window. */
    SemaphoreHandle_t lock;
} ILI9488Class;

/* Attaches the panel as a device on an already-initialized SPI bus and brings
 * it up: hardware reset (if a reset line is wired), the vendor power/gamma
 * sequence, COLMOD = RGB666, MADCTL for `rotation`, sleep out, display on.
 * The screen is left cleared to black.
 *
 * `owner` and `io` are borrowed. dc_gpio/reset_gpio select, independently,
 * whether D/C and ~RESET are driven straight off an ESP32 GPIO (>= 0, bench
 * wiring -- see settings.h's DISPLAY_DC_GPIO/DISPLAY_RESET_GPIO and the
 * KILNCTL_DISPLAY_DC_RESET_DIRECT_GPIO Kconfig option) or through the
 * expander (-1, the normal main-board wiring, via kiln_io_lcd_dc/
 * kiln_io_lcd_reset).
 *
 * `io` may be NULL only if dc_gpio >= 0 (D/C doesn't need the expander at
 * all in that case) -- with dc_gpio == -1 there is no way to send even a
 * command without it, so init fails rather than pretend otherwise. A NULL
 * `io` with reset_gpio == -1 just means no hardware reset line is available
 * (same as today: init falls back to SWRESET).
 *
 * width/height are the panel's NATIVE dimensions (320x480); the rotated
 * dimensions are derived. */
esp_err_t ILI9488_init(ILI9488Class *disp,
                       spi_owner_t *owner,
                       spi_host_device_t host,
                       kiln_io_t *io,
                       int cs_gpio,
                       int dc_gpio,
                       int reset_gpio,
                       uint16_t panel_width,
                       uint16_t panel_height,
                       uint8_t rotation,
                       int clock_hz);

/* Single-call bootstrap used by app_main: ILI9488_init with the
 * Kconfig-configured CS/clock/geometry/rotation, plus a one-line boot banner
 * so there is visible output before anything connects over UART. Logs and
 * returns the init error rather than aborting -- a dead panel is not worth
 * failing boot over, and the UART link is the real control path. */
esp_err_t ILI9488_start(ILI9488Class *disp, spi_owner_t *owner, spi_host_device_t host, kiln_io_t *io);

esp_err_t ILI9488_deinit(ILI9488Class *disp);

/* hard = true pulses ~RESET through the expander (10us minimum per the
 * datasheet, 120ms to recover); hard = false sends SWRESET (01h). Either way
 * the panel comes back at its power-on defaults, so this re-runs the whole
 * init sequence afterwards -- otherwise COLMOD would fall back to the
 * power-on 06h and every subsequent pixel would be misinterpreted. */
esp_err_t ILI9488_reset(ILI9488Class *disp, bool hard);

esp_err_t ILI9488_set_power(ILI9488Class *disp, bool on);       /* display off + sleep in when false */
esp_err_t ILI9488_set_rotation(ILI9488Class *disp, uint8_t rotation); /* 0-3; updates width/height */
esp_err_t ILI9488_set_invert(ILI9488Class *disp, bool invert);

/* Geometry as currently rotated. The UART READ_ID reply carries these, so the
 * PC side never has to track rotation itself. */
esp_err_t ILI9488_get_dimensions(ILI9488Class *disp, uint16_t *out_width, uint16_t *out_height);

/* Drawing. Colors are RGB565; coordinates are in the current rotation and are
 * bounds-checked, never clipped -- an out-of-range rectangle is a caller bug
 * (a truncated frame is much harder to notice than an error return), so these
 * return ESP_ERR_INVALID_ARG rather than drawing part of it. */
esp_err_t ILI9488_clear(ILI9488Class *disp, uint16_t color);
esp_err_t ILI9488_fill_rect(ILI9488Class *disp, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
esp_err_t ILI9488_draw_rect(ILI9488Class *disp, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color); /* 1px outline */
esp_err_t ILI9488_draw_line(ILI9488Class *disp, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);
esp_err_t ILI9488_draw_pixel(ILI9488Class *disp, uint16_t x, uint16_t y, uint16_t color);

/* Text. Same 5x7 ASCII font as the SSD1306 driver, integer-scaled by
 * text_size. Wraps at the right edge to the next text line and from the
 * bottom back to y = 0 (no scrolling -- scrolling would require reading the
 * panel back, which this wiring cannot do at any useful speed). */
esp_err_t ILI9488_set_text_cursor(ILI9488Class *disp, uint16_t x, uint16_t y);  /* pixels, glyph top-left */
esp_err_t ILI9488_set_text_style(ILI9488Class *disp, uint16_t fg, uint16_t bg, uint8_t size, bool opaque_background);
esp_err_t ILI9488_write_char(ILI9488Class *disp, char c);
esp_err_t ILI9488_write_str(ILI9488Class *disp, const char *str);
esp_err_t ILI9488_write(ILI9488Class *disp, const char *data, size_t len);  /* not null-terminated (the UART PRINT payload) */
esp_err_t ILI9488_printf(ILI9488Class *disp, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
esp_err_t ILI9488_vprintf(ILI9488Class *disp, const char *fmt, va_list args);

/* Streaming blit. BEGIN opens a pixel window and leaves it open across as
 * many DATA calls (i.e. as many UART frames) as it takes to fill it; END
 * closes it. Between calls the panel's address counter is live and D/C is
 * parked in "data", so no other drawing may happen -- any other draw call
 * while a blit is open aborts the blit and returns ESP_ERR_INVALID_STATE.
 *
 * `data` is RGB565, u16 little-endian, row-major, exactly as it arrives on
 * the wire. An odd length (a split pixel) or more pixels than the window
 * holds is ESP_ERR_INVALID_ARG/ESP_ERR_INVALID_SIZE and aborts the blit --
 * a desynchronized stream would otherwise smear the rest of the image. */
esp_err_t ILI9488_blit_begin(ILI9488Class *disp, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
esp_err_t ILI9488_blit_data(ILI9488Class *disp, const uint8_t *data, size_t len);
esp_err_t ILI9488_blit_end(ILI9488Class *disp);
esp_err_t ILI9488_blit_abort(ILI9488Class *disp);  /* drop an open window without error */
bool ILI9488_blit_active(ILI9488Class *disp);

/* RDDID (04h): 24 bits of manufacturer / version / driver ID. Clocked on the
 * slow read device (see ILI9488_READ_CLOCK_HZ). Returns ESP_ERR_NOT_FOUND if
 * the panel answers all-zero or all-ones, which is what an unwired or
 * high-impedance MISO looks like -- worth distinguishing from a bus error. */
esp_err_t ILI9488_read_id(ILI9488Class *disp, uint8_t out_id[3]);

#ifdef __cplusplus
}
#endif

#endif // ILI9488_H
