// log_store_mount -- device-only glue between log_store.c (pure, portable,
// host-testable) and the real hardware: mounts the `logs` SPIFFS partition
// (partitions.csv, 0xCF0000, 3072K) at "/logs" and routes writes through
// uart_bridge_ext_run_on_flash_worker() so a PSRAM-stacked caller (telemetry_
// log_task, same task that already calls ESP_LOGI() for the live UART feed)
// never touches flash directly -- see uart_bridge_ext.c's own HAZARD comment
// for the "flash writes from a PSRAM-stacked task assert every time on this
// board" incident this sidesteps, and the project memory entry of the same
// name.
//
// Not part of any host test build -- it #includes esp_spiffs.h and
// uart_bridge.h, neither of which exists off-target.
#ifndef LOG_STORE_MOUNT_H
#define LOG_STORE_MOUNT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mounts the `logs` partition at "/logs" (format_if_mount_failed = true, same
 * convention as this project's other flash-backed stores recovering from a
 * corrupt/first-boot partition rather than refusing to come up) and calls
 * log_store_init("/logs"). Idempotent. Safe to call even if the partition is
 * missing/corrupt beyond recovery -- returns an error, logs it, and every
 * subsequent log_store_write_*() call below becomes a no-op (log_store_
 * is_init() stays false), never a boot-time hang. */
esp_err_t log_store_mount(void);

/* Thin wrappers that hand the line to log_store_append() on the flash
 * worker task (uart_bridge_ext_run_on_flash_worker()) rather than the
 * caller's own stack/task. Safe to call before log_store_mount() succeeds or
 * if it failed -- log_store_append() itself returns ESP_ERR_INVALID_STATE
 * in that case, which these wrappers simply propagate; nothing here
 * retries or blocks waiting for the mount to succeed later. */
esp_err_t log_store_write_firing(const char *line);
esp_err_t log_store_write_autotune(const char *line);

#ifdef __cplusplus
}
#endif

#endif // LOG_STORE_MOUNT_H
