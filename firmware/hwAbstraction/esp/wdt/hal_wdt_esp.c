/* hal_wdt_esp.c -- ESP-IDF backend for interface/hal_wdt.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against ESP-IDF
 * v6.0.2 (C:\esp\v6.0.2\esp-idf), grounded in hal_wdt.h's own consumer
 * census: watchdog_cfg.c is the SOLE production caller of esp_task_wdt_*
 * (build_twdt_config()/apply_panic_disabled() at watchdog_cfg.c:92-114,
 * which calls esp_task_wdt_reconfigure(&cfg) -- there is no
 * esp_task_wdt_init() call in this tree; the task-WDT is armed by IDF's own
 * boot-time init off Kconfig, and watchdog_cfg.c only ever *reconfigures*
 * it later to flip panic-vs-warn). esp_restart() sole real call sites:
 * factory_reset.c:90, ota_http_recovery.c:98, both unconditional no-args
 * never-returns calls. Compiled by
 * firmware/hwAbstraction/idf/hwabstraction_esp/CMakeLists.txt.
 *
 * INTERFACE MISMATCH: hal_wdt_init(timeout_ms, panic_disabled) is
 * esp_task_wdt_init()-shaped per hal_wdt.h's own doc comment, but this
 * tree's ESP side has no esp_task_wdt_init() call site at all -- the
 * task-WDT is armed by IDF's boot sequence from Kconfig
 * (CONFIG_ESP_TASK_WDT_*), and watchdog_cfg.c only ever *reconfigures* an
 * already-armed watchdog via esp_task_wdt_reconfigure(). This backend
 * therefore implements hal_wdt_init() as a reconfigure against the running
 * task-WDT (esp_task_wdt_reconfigure(), same call watchdog_cfg.c already
 * makes) rather than a fresh esp_task_wdt_init() -- calling
 * esp_task_wdt_init() a second time on an already-initialized task-WDT
 * returns ESP_ERR_INVALID_STATE on this IDF version, which would make a
 * caller that follows hal_wdt.h's own "arms the watchdog" doc comment
 * literally fail every time on ESP. Reported here rather than silently
 * papered over; hal_wdt.h's ESP-side doc comment should be revisited to
 * say "reconfigures" if a future pass tightens the header text.
 */
#include "hal_wdt.h"

#include "esp_task_wdt.h"
#include "esp_system.h"

#include "hal_esp_common.h"

/* Mirrors watchdog_cfg.c's own build_twdt_config() shape verbatim
 * (watchdog_cfg.c:92-104): idle_core_mask is derived from the same
 * CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1 Kconfig bits watchdog_cfg.c
 * reads directly -- there is no ESP_TASK_WDT_ALL_CORES macro in this IDF
 * version's esp_task_wdt.h, and watchdog_cfg.c's own comment is explicit
 * that idle_core_mask must always be Kconfig-derived, never hardcoded, so
 * the rest of the firmware's sizing around it cannot silently drift.
 * Only trigger_panic follows the caller's panic_disabled argument. */
static uint32_t hal_wdt_esp_idle_core_mask(void) {
    uint32_t mask = 0;
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    mask |= (1u << 0);
#endif
#if defined(CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1) && CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    mask |= (1u << 1);
#endif
    return mask;
}

/* Seeded from CONFIG_ESP_TASK_WDT_TIMEOUT_S, exactly matching
 * watchdog_cfg.c's own build_twdt_config() (watchdog_cfg.c:92-104): the
 * task-WDT is actually armed at boot by IDF off this Kconfig value, not by
 * an hal_wdt_init() call (see this file's header comment) -- so
 * hal_wdt_set_panic_disabled() must default to that same Kconfig-derived
 * timeout rather than 0 if it is ever called before hal_wdt_init() runs
 * (watchdog_cfg.c's sole future consumer only ever reconfigures the panic
 * bit, per that file's own header comment). */
static uint32_t s_last_timeout_ms = (uint32_t)CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000u;

hal_status_t hal_wdt_init(uint32_t timeout_ms, bool panic_disabled,
                           bool pause_on_debug) {
    /* No TWDT equivalent to pico's pause-while-halted-at-a-breakpoint knob
     * -- see hal_wdt.h's doc comment on this parameter. Ignored here, not
     * fabricated. */
    (void)pause_on_debug;
    esp_task_wdt_config_t cfg = {
        .timeout_ms = timeout_ms,
        .idle_core_mask = hal_wdt_esp_idle_core_mask(),
        .trigger_panic = !panic_disabled,
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err == ESP_OK) {
        s_last_timeout_ms = timeout_ms;
    }
    return hal_esp_err_to_status(err);
}

/* watchdog_cfg.c's apply_panic_disabled(): rebuild the config with the new
 * panic_disabled bit and reconfigure -- there is no lighter-weight IDF call
 * that flips only trigger_panic without restating timeout_ms, so this
 * backend needs the last-armed timeout_ms to reconfigure correctly. This
 * header's hal_wdt_set_panic_disabled() takes no timeout_ms argument (see
 * hal_wdt.h), so this backend remembers the value passed to hal_wdt_init()
 * and reuses it here -- matching watchdog_cfg.c's own real call sequence
 * (init once at boot with a fixed Kconfig-derived timeout, reconfigure only
 * the panic bit thereafter; watchdog_cfg.c never changes timeout_ms after
 * boot). */
hal_status_t hal_wdt_set_panic_disabled(bool panic_disabled) {
    esp_task_wdt_config_t cfg = {
        .timeout_ms = s_last_timeout_ms,
        .idle_core_mask = hal_wdt_esp_idle_core_mask(),
        .trigger_panic = !panic_disabled,
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    return hal_esp_err_to_status(err);
}

hal_status_t hal_wdt_feed(void) {
    esp_err_t err = esp_task_wdt_reset();
    return hal_esp_err_to_status(err);
}

/* Never returns on real hardware -- see hal_wdt.h's threading contract. */
void hal_wdt_reboot(void) {
    esp_restart();
}
