/* startup_faults -- a one-bit-per-subsystem record of "this boot, a task or
 * init step that an operator depends on FAILED TO START".
 *
 * ROADMAP.md M13's standing sweep (2026-10-03). Before this module, every one
 * of these failures ended in a single ESP_LOGE/ESP_LOGW on the debug UART and
 * nothing else: the board looked healthy on /api/status, /api/readiness, the
 * LCD and every PC tool while, for example, guard 9's profile-executor
 * watchdog task did not exist. `GET /api/readiness`'s "startup" item now
 * renders this table (readiness_http.c), naming each failed subsystem and what
 * it costs.
 *
 * Contract:
 *   - Latching and per-boot: a bit is only ever set, never cleared; a reboot
 *     starts clean, which is correct, since the next boot re-runs the start.
 *   - Safe from any task: every id owns its own byte, so two tasks noting two
 *     different faults at once cannot lose one (a shared bitmask OR can).
 *   - No allocation, no locks, no ESP-IDF dependency -- host-testable.
 *   - Noting a fault does NOT gate anything (see readiness_http.h's "ADDING AN
 *     ITEM HERE DOES NOT GATE ANYTHING"); promoting one to a firing interlock
 *     is an owner decision.
 *
 * Add an id only for a failure whose remedy is an operator's (reboot, reflash,
 * check hardware). Append before STARTUP_FAULT_COUNT and add both strings in
 * startup_faults.c; test_startup_faults.c fails if either table is short.
 */
#ifndef STARTUP_FAULTS_H
#define STARTUP_FAULTS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STARTUP_FAULT_EXEC_WATCHDOG = 0, /* profile_exec_wdt task (guard 9) did not start */
    STARTUP_FAULT_PC_LINK_WATCHDOG,  /* uart_bridge link watchdog did not start */
    STARTUP_FAULT_PROFILE_EXECUTOR,  /* profile_executor_start() failed */
    STARTUP_FAULT_AUTOTUNE_ENGINE,   /* autotune_engine_start() failed */
    STARTUP_FAULT_KILN_IO_OWNER,     /* kiln_io_owner_start() failed */
    STARTUP_FAULT_THERMO_OWNER,      /* thermo_owner_start() failed */
    STARTUP_FAULT_LOG_STORE,         /* log_store_mount() failed */
    STARTUP_FAULT_TELEMETRY_LOG,     /* telemetry_log_start() failed */
    STARTUP_FAULT_RELAY_CYCLES,      /* relay_cycles_init() failed */
    STARTUP_FAULT_OTA_CONFIRM,       /* OTA rollback-confirmation task did not start */
    STARTUP_FAULT_OTA_ROUTES,        /* ota_http_start() failed */
    STARTUP_FAULT_KILN_CFG_SWAP,     /* kiln_cfg_swap worker did not start */
    STARTUP_FAULT_LCD_UI,            /* LVGL display task did not start */
    STARTUP_FAULT_BOOT_GUARD_NVS, /* boot_guard NVS init/persist failed */
    STARTUP_FAULT_DANGER_MODE, /* danger_mode task did not start */
    STARTUP_FAULT_PC_BRIDGES, /* a PC-link bridge task did not start */
    STARTUP_FAULT_DNS_HIJACK, /* captive-portal DNS did not start */
    STARTUP_FAULT_HTTP_ROUTES, /* an HTTP page/API start failed */
    STARTUP_FAULT_WEB_AUTH_ROUTES, /* a login/security route start failed */
    STARTUP_FAULT_SETTINGS_STORE, /* a saved-settings store failed to load */
    STARTUP_FAULT_PICO_AUTO_UPDATE, /* pico auto-update start failed */
    STARTUP_FAULT_HEARTBEAT_MONITOR, /* heartbeat monitor task did not start */
    STARTUP_FAULT_TIME_SYNC, /* time_sync_start failed */
    STARTUP_FAULT_LCD_BACKLIGHT, /* screen idle/backlight start failed */
    STARTUP_FAULT_TOUCH, /* touch controller bring-up failed */
    STARTUP_FAULT_COUNT
} startup_fault_t;

/* Latch `id`. Out-of-range ids are ignored. Idempotent. */
void startup_fault_note(startup_fault_t id);

bool startup_fault_is_set(startup_fault_t id);

/* Number of latched faults. */
unsigned startup_fault_count(void);

/* Short subsystem label, e.g. "profile_exec_wdt (guard 9)". NULL if out of range. */
const char *startup_fault_name(startup_fault_t id);

/* What the operator loses and what to do, one sentence. NULL if out of range. */
const char *startup_fault_impact(startup_fault_t id);

/* Writes "<name>; <name>; ..." for the latched faults into `out` (always
 * NUL-terminated, truncating with "..." rather than overrunning). Returns the
 * number of faults named in full. */
unsigned startup_fault_summarize(char *out, size_t cap);

/* Test hook: clears every latch. Not for production code. */
void startup_fault_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif /* STARTUP_FAULTS_H */
