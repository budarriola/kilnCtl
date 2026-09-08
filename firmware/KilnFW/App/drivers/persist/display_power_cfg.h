// display_power_cfg -- persisted settings for the display brightness/idle-
// timeout/keep-on-while-firing/display-on-error feature (owner request
// 2026-09-04, docs/UI_PLAN.md). This module owns ONLY the persisted values
// and their NVS round trip -- it does not itself decide when the display
// turns on or off (that is display_power_policy.c's pure step function) and
// it does not itself drive the backlight (that is backlight_pwm.c -- see
// this header's BRIGHTNESS note below).
//
// PERSISTENCE: same single-scalar-in-the-existing-namespace pattern as
// unit_pref.c/ramp_assist_cfg.c -- kiln_nvs partition, "kiln_cfg" namespace
// (shared with zones_http.c/unit_pref.c/ramp_assist_cfg.c/profiles_builtin.c
// and friends, distinguished by KEY not namespace). Stored as one small
// versioned blob rather than four separate keys, since the four values are
// always read/written together by the settings page and gain a natural
// place to add a schema version if a field is ever added later. A missing
// key (fresh board) or a blob that fails validation (wrong size, or any
// field out of range) both fall back to the SAFE defaults below, never the
// other way -- see display_power_cfg.c's display_power_cfg_start().
//
// SAFE DEFAULTS, and why each one is the safe direction:
//   - brightness_percent = 100 -- a board that has never heard of this
//     setting keeps the panel at full brightness, same as today (no
//     regression for boards that predate this feature).
//   - timeout_setting = DISPLAY_TIMEOUT_NEVER -- today's shipped behaviour
//     (CONFIG_KILNCTL_TOUCH_IDLE_TIMEOUT_MS == 0, auto-blank disabled) is
//     "never blank"; this module's default must not silently start blanking
//     a board that never asked for it.
//   - keep_on_while_firing = true -- OWNER DECISION 2026-09-04. The earlier
//     reasoning (false, because it "only matters once a real timeout is
//     chosen") was about internal consistency with DISPLAY_TIMEOUT_NEVER,
//     not about what the operator wants. The owner wants a firing kiln to
//     keep its display up, so that this switch is already correct on the
//     day a timeout IS chosen rather than needing to be found and flipped.
//   - display_on_error = true -- OWNER DECISION 2026-09-04, same reasoning.
//     An error that nobody sees is the case this feature exists for; making
//     it opt-in defeats it. It self-dismisses on the next touch and resumes
//     the previous timeout, so the cost of it being on is one touch.
//
// BRIGHTNESS: no longer inert as of be02d34.
// CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE now defaults to y (Kconfig, "Backlight
// PWM (flying-wire bodge, DISPLAY_ST7796_PLAN.md 3.4.1)"), and
// backlight_pwm.c's on/idle duty follows this module's brightness_percent
// (via display_power_cfg_brightness_percent()) rather than the old fixed
// CONFIG_KILNCTL_BACKLIGHT_ON_PERCENT/IDLE_PERCENT constants. The flying wire
// from a spare ESP32 GPIO to the backlight LED input IS fitted on THIS bench
// unit -- GPIO15, confirmed with a meter (be02d34's commit message), which is
// why the default flipped to y for it. A different board that does NOT have
// that wire soldered must override this option off in its own local
// sdkconfig -- leaving the y default on such a board drives LEDC PWM onto
// whatever KILNCTL_BACKLIGHT_GPIO points at even though nothing is wired
// there, instead of the safe ESP_ERR_NOT_SUPPORTED no-op this option used to
// guarantee unconditionally (see KILNCTL_BACKLIGHT_PWM_ENABLE's own Kconfig
// help).
#ifndef DISPLAY_POWER_CFG_H
#define DISPLAY_POWER_CFG_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "display_power_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

// Loads the persisted settings from NVS (or leaves the safe defaults above
// in place if nothing valid is stored). Non-fatal: never fails app_main,
// same convention as unit_pref_start()/ramp_assist_cfg_start(). Safe to call
// more than once; idempotent after the first call in a boot.
esp_err_t display_power_cfg_start(void);

// Current in-RAM values. O(1), no NVS access.
uint8_t display_power_cfg_brightness_percent(void);
display_timeout_setting_t display_power_cfg_timeout_setting(void);
bool display_power_cfg_keep_on_while_firing(void);
bool display_power_cfg_display_on_error(void);

// Validates and persists all four settings together (the settings page
// posts them as one form) -- same "reject invalid outright, never partially
// apply" discipline as zones_http.c's zones_config_set_*() functions and
// unit_pref_set(). `brightness_percent` must be 0-100;
// `timeout_setting` must satisfy display_power_timeout_setting_is_valid().
// Returns ESP_ERR_INVALID_ARG (and leaves every persisted/in-RAM value
// untouched) if either fails -- the two bool switches have no invalid
// values. In-RAM values update first (live for the very next policy step),
// matching unit_pref_set()'s "in-RAM truth first" ordering; a save failure
// after that means the choice will not survive a reboot, not that it failed
// to take effect now.
// Read-only dual-write status for GET /api/cfgfs -- see unit_pref.h's
// unit_pref_get_dualwrite_status() for the full contract.
void display_power_cfg_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid,
                                             uint32_t *nvs_rev, bool *diverged);

esp_err_t display_power_cfg_set(uint8_t brightness_percent, display_timeout_setting_t timeout_setting,
                                bool keep_on_while_firing, bool display_on_error);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_POWER_CFG_H
