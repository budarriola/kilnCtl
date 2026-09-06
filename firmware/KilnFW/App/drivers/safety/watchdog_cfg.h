// watchdog_cfg -- the operator-facing, persisted switch for
// CONFIG_ESP_TASK_WDT_PANIC (sdkconfig.defaults' 2026-08-22 watchdog-recovery
// pass). That default is the right one for a shipped board: a task that
// starves both idle tasks of CPU for CONFIG_ESP_TASK_WDT_TIMEOUT_S (5 s,
// inherited IDF default) now panics -> resets instead of only logging, so a
// wedged task never leaves the relays silently latched in whatever state they
// were last commanded, forever. It is also exactly the thing that makes bench
// debugging painful: sit at a breakpoint, single-step past 5 s, and the panic
// reboots the board out from under you.
//
// This module is the dev-only escape hatch, not a config option meant to ship
// armed: an operator can disable the PANIC half of the task watchdog (the
// LOG half, and the watchdog's actual monitoring, keep running either way --
// see watchdog_cfg_set_panic_disabled()'s doc comment) from the UART bridge
// or the web debug page, and it survives a reboot because it is written to
// flash. It does NOT touch the RTC watchdog (rtc_watchdog.h) -- that one is
// deliberately independent of the scheduler and stays armed regardless, as
// the last-resort backstop for a lockup so total the task watchdog itself
// never got to run.
//
// PERSISTENCE: same NVS-struct-with-a-version-and-a-CRC32 convention as
// boot_guard.c (own namespace, "kiln_nvs" partition, load-tolerant: a
// missing/corrupt/wrong-version record is treated as "not set" -- see
// watchdog_cfg.c's record_is_valid()). Unlike boot_guard.c's counter, where a
// corrupted-to-zero record is merely a lost boot's worth of accuracy, a
// corrupted record HERE must resolve to the SAFE default (panic ENABLED),
// never the other way -- see watchdog_cfg.c's load_disabled_flag() for
// exactly how that is enforced.
#ifndef WATCHDOG_CFG_H
#define WATCHDOG_CFG_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The one warning sentence shown before starting a firing while the task-
// watchdog panic is disabled -- defined once, here, exactly the way
// ota_interlock.h's OTA_INTERLOCK_NO_SAFETY_WARNING is: main_page.html's JS
// copy of this text (WATCHDOG_CFG_FIRING_WARNING_JS) is compared against
// this macro by App/test/lint_pages.js so the two surfaces cannot drift
// apart, and ui_page_home.c includes this header directly for the LCD's
// copy of the same dialog.
//
// States the consequence, not the mechanism: an operator deciding whether to
// start a firing needs to know a hang leaves the kiln uncontrolled and
// running, not which Kconfig option is off.
#define WATCHDOG_CFG_FIRING_WARNING                                                             \
    "The task-watchdog panic is disabled for debugging. If a task hangs, the board will NOT "   \
    "reboot -- it will sit hung with the relays in whatever state they were last commanded. "    \
    "Start the firing anyway?"

// Loads the persisted setting (or the safe default, panic ENABLED, if
// nothing valid is stored) and, if the operator left it disabled last time,
// re-applies that immediately via esp_task_wdt_reconfigure() -- see
// watchdog_cfg.c for the exact call and where its timeout_ms/idle_core_mask
// come from. Must be called from app_main() AFTER the task watchdog already
// exists (it does, automatically, whenever CONFIG_ESP_TASK_WDT_EN=y -- no
// other call in this codebase creates it) -- calling this before that would
// have nothing to reconfigure. Logs an unmissable ESP_LOGE banner, in the
// same spirit as boot_guard.c's recovery-mode banner, whenever it finds the
// panic disabled at boot: a safety default that has been turned off must
// never be quietly true.
//
// Safe to call more than once; idempotent after the first call in a boot.
void watchdog_cfg_init(void);

// True if the operator has disabled the task-watchdog panic (persisted AND
// currently applied). Read by dashboard_http.c (GET /api/status, POST
// /api/profile_exec/start's loud log), diagnostics_http.c (GET
// /api/watchdog_cfg), the LCD's Start confirmation chain (ui_page_home.c),
// and uart_bridge.c's SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED.
bool watchdog_cfg_panic_disabled(void);

// Persists `disabled` to NVS AND applies it immediately via
// esp_task_wdt_reconfigure() -- no reboot needed either direction. Every
// transition is logged loudly (ESP_LOGW) with `source` describing who/where
// the change came from (e.g. "web debug page", "UART
// SYSTEM_CMD_SET_WATCHDOG_PANIC_DISABLED") for anyone reading the log later.
// Returns the NVS write's esp_err_t -- the in-RAM state (and therefore the
// live TWDT reconfigure) is applied regardless of whether the persist
// succeeded, so the toggle takes effect for the rest of this boot even if
// flash is somehow unwritable.
esp_err_t watchdog_cfg_set_panic_disabled(bool disabled, const char *source);

#ifdef __cplusplus
}
#endif

#endif // WATCHDOG_CFG_H
