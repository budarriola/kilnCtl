// recovery_lcd_policy.h -- pure decisions of the recovery LCD (init retry
// schedule, boot_guard record classification), kept free of ESP-IDF types so a
// host test can exercise them (check_recovery_lcd_policy.ps1).
//
// Why: the SoftAP passphrase is shown ONLY on the LCD (owner decision
// 2026-10-02), so a panel that never comes up must be retried, reported and
// loudly logged rather than silently left blank.
#ifndef RECOVERY_LCD_POLICY_H
#define RECOVERY_LCD_POLICY_H

#include <stdbool.h>
#include <stdint.h>

// Init attempts per burst (boot, and each later retry pass).
#define RLCD_BURST_ATTEMPTS 3

// Total SX1509 hard re-resets allowed per boot, across every burst. A re-reset
// pulses ~RESET and floats the relay gates for ~10 ms, and the relay-hold
// watchdog cannot see it, so it is rare and bounded.
#define RLCD_MAX_RERESETS 3

// Re-reset the SX1509 before another attempt only when the previous attempt
// failed on an EXPANDER I2C error (D/C or ~RESET write), never on an SPI
// bus/device init or transmit error (a re-reset cannot fix those), and only
// while fewer than RLCD_MAX_RERESETS have been done this boot.
static inline bool rlcd_rereset_allowed(bool expander_err, int rereset_done)
{
    return expander_err && rereset_done < RLCD_MAX_RERESETS;
}

// After `attempts_done` attempts of this burst, the last of which succeeded
// (`ok`) or not, may another attempt run in the same burst?
static inline bool rlcd_burst_continue(int attempts_done, bool ok)
{
    return !ok && attempts_done < RLCD_BURST_ATTEMPTS;
}

typedef enum {
    RLCD_TICK_NONE = 0,        // panel ready: nothing to do
    RLCD_TICK_WARN_AND_RETRY = 1, // not ready: log the 1 Hz warning, run a burst
} rlcd_tick_t;

// Decision of the 1 Hz LCD task.
static inline rlcd_tick_t rlcd_tick_action(bool ready)
{
    return ready ? RLCD_TICK_NONE : RLCD_TICK_WARN_AND_RETRY;
}

// The one UART line logged at 1 Hz while the panel is not ready. It must
// never contain the passphrase.
#define RLCD_NOT_READY_MSG "LCD NOT READY, passphrase not displayed"

// A draw pass succeeded only if every line did; any failure marks the panel
// not ready so the retry path redraws.
static inline bool rlcd_draw_ok(int failed_lines)
{
    return failed_lines == 0;
}

// --- Wi-Fi join QR visibility ----------------------------------------------

// Which text block the status screen shows under the network rows. The text
// branch of draw_status() and the QR visibility below are BOTH derived from this
// one function, so the QR and the passphrase text cannot drift apart.
typedef enum {
    RLCD_TEXT_ERROR = 0,    // error banner or AP down: overrides every network state
    RLCD_TEXT_NET_FAIL = 1, // no network / Wi-Fi storage failure
    RLCD_TEXT_NET_SET = 2,  // AP up, network configured: SSID and passphrase shown
    RLCD_TEXT_STARTING = 3, // nothing known yet
} rlcd_text_mode_t;

static inline rlcd_text_mode_t rlcd_text_mode(bool err_head, bool ap_down, bool net_none, bool storage_fail,
                                              bool net_set)
{
    if (err_head || ap_down) {
        return RLCD_TEXT_ERROR;
    }
    if (net_none || storage_fail) {
        return RLCD_TEXT_NET_FAIL;
    }
    return net_set ? RLCD_TEXT_NET_SET : RLCD_TEXT_STARTING;
}

// The join QR is shown exactly where the passphrase text is: the NET_SET text
// mode, and a symbol actually encoded.
static inline bool rlcd_qr_visible(bool err_head, bool ap_down, bool net_none, bool storage_fail,
                                   bool net_set, int qr_size)
{
    return rlcd_text_mode(err_head, ap_down, net_none, storage_fail, net_set) == RLCD_TEXT_NET_SET &&
           qr_size > 0;
}

typedef enum {
    RLCD_QR_BEFORE_TEXT = 0,
    RLCD_QR_AFTER_TEXT = 1,
} rlcd_qr_phase_t;

// Paints the QR area for one phase of draw_status() through `paint(show)`
// (returns 0 on success; draw_qr_area in recovery_lcd.c). A visible symbol is
// painted AFTER the text; the hidden (solid background) area is painted BEFORE
// it, so a stale code never outlives its AP. Returns the number of failed
// paints (0 or 1); `paint` is not called when the phase has nothing to do.
static inline int rlcd_qr_paint(bool visible, rlcd_qr_phase_t phase, int (*paint)(bool show))
{
    if (phase == RLCD_QR_BEFORE_TEXT && !visible) {
        return paint(false) != 0;
    }
    if (phase == RLCD_QR_AFTER_TEXT && visible) {
        return paint(true) != 0;
    }
    return 0;
}

// Width of the text band whose rows are y..y+h-1: a visible QR owns the
// top-right qr_px square, so a band that reaches into it stops at its left edge
// (otherwise the QR would cut a fault band's background mid-line).
static inline int rlcd_band_width(int y, int h, bool qr_visible, int panel_w, int qr_px)
{
    return (qr_visible && y < qr_px && y + h > 0) ? panel_w - qr_px : panel_w;
}

// --- boot_guard record classification ------------------------------------

// Result of nvs_get_blob() for the "bootguard" key, mapped by the caller.
typedef enum {
    RLCD_GET_OK = 0,
    RLCD_GET_NOT_FOUND = 1,   // key (or namespace) absent
    RLCD_GET_BAD_LENGTH = 2,  // ESP_ERR_NVS_INVALID_LENGTH: a blob of another size exists
    RLCD_GET_OTHER = 3,       // any other NVS error
} rlcd_get_rc_t;

typedef enum {
    RLCD_BG_NONE = 0,       // no record: clean/just-cleared counter
    RLCD_BG_VALID = 1,      // record decoded (count is meaningful)
    RLCD_BG_INVALID = 2,    // a record exists but is wrong-size / bad version / bad CRC
    RLCD_BG_UNREADABLE = 3, // NVS could not be read
} rlcd_bg_state_t;

// `decoded` is ric_boot_guard_decode()'s result (only meaningful for GET_OK).
static inline rlcd_bg_state_t rlcd_classify_boot_guard(rlcd_get_rc_t rc, bool decoded)
{
    switch (rc) {
    case RLCD_GET_OK: return decoded ? RLCD_BG_VALID : RLCD_BG_INVALID;
    case RLCD_GET_NOT_FOUND: return RLCD_BG_NONE;
    case RLCD_GET_BAD_LENGTH: return RLCD_BG_INVALID;
    default: return RLCD_BG_UNREADABLE;
    }
}

static inline const char *rlcd_bg_state_name(rlcd_bg_state_t s)
{
    switch (s) {
    case RLCD_BG_NONE: return "none";
    case RLCD_BG_VALID: return "valid";
    case RLCD_BG_INVALID: return "invalid";
    default: return "unreadable";
    }
}

#endif // RECOVERY_LCD_POLICY_H
