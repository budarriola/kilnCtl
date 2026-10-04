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

// Attempt index is 0-based. Attempts after the first re-reset the SX1509 first
// (LCD D/C and ~RESET share it with the relay hold, so an expander fault hits
// both); the first attempt of a burst does not.
static inline bool rlcd_reset_expander_before(int attempt_index)
{
    return attempt_index > 0;
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
