/* hal_kv.h -- opaque key/value store interface. ESP-only, wraps NVS. Pico
 * explicitly excluded. See docs/HW_ABSTRACTION_PLAN.md "hal_kv -- ESP-only,
 * wraps NVS. Pico explicitly excluded."
 *
 * Why pico is excluded: SaftyFW's config_store is not a KV store -- it is a
 * fixed 512-byte record, seq-numbered, CRC'd, 8-slot round-robin log in one
 * 4K sector, with a hard safety interlock (write refused while relay ARMED).
 * Forcing it through hal_kv would either strip the ARMED gate to a
 * caller-side check or bloat this interface with slot/seq/ARMED concepts one
 * platform needs. It sits on hal_flash instead (separate interface, pico
 * backend for config_store_flash.c).
 *
 * Consumer census (KilnFW App/drivers/, function-by-function, re-done for
 * this header): plain nvs_open (default partition) alongside the far more
 * common nvs_open_from_partition -- both map to hal_kv_open, partition NULL
 * meaning the default; nvs_get/set_blob (including the size-probe form,
 * nvs_get_blob with a NULL buffer to learn the required length first) and
 * nvs_get/set_str; nvs_commit; nvs_erase_key (crash_report.c, profiles_http.c);
 * nvs_flash_init_partition / nvs_flash_erase_partition (idempotent
 * init-with-erase-retry pattern); nvs_get_stats (nvs_report's enumeration).
 * Zero production callers of the scalar nvs_get/set_u8/u32/i16/i32,
 * iterators, nvs_erase_all, nvs_find_key or nvs_flash_deinit -- none of
 * those are in this header. Namespaces seen: kiln_cfg, boot_guard,
 * touch_cal, watchdog_cfg, wifi_cfg. Partitions seen: kiln_nvs, wifi_nvs,
 * profiles_nvs (touch_cal_store.c has its own local definition of the
 * "kiln_nvs" partition name -- easy to miss when auditing string literals).
 *
 * Disagreement with the plan text: the plan's API sketch lists
 * hal_kv_get/set_str and _blob but originally omitted erase_key (corrected
 * in the plan doc itself, "The round-1 draft listed `_u8/_u32` and omitted
 * `erase_key`; both corrected") -- this header follows the corrected,
 * final plan shape, which already matches the census above. No further gap
 * found between the plan and the current NVS call-site census.
 *
 * Write-context safety is part of the contract, not an implementation
 * detail: NVS/flash writes issued from a task running on a PSRAM-backed
 * stack panic on this target every time (documented in project memory).
 * hal_kv_set_str/_blob/_erase_key/_commit and the partition-level erase/init
 * calls below are therefore legal ONLY from a task known to run on an
 * internal-DRAM stack -- in practice, only from the flash worker task (the
 * dispatch target every write-path caller already routes through today) or
 * from init-time code that runs before any PSRAM-stacked task is created.
 * hal_kv_write_safe_here() exposes today's caller_stack_is_external
 * predicate so the existing copy-pasted guards (worker dispatch / local
 * guard / init-time-only, plus wifi_prov's dedicated-writer-task variant)
 * and flash_worker_lint.py's sanctioned call patterns survive verbatim once
 * rekeyed onto hal_kv_set_*. Read calls (hal_kv_get_*) carry no such
 * restriction. This is a documented contract for callers and lint to
 * enforce -- hal_kv itself does not gate writes at runtime.
 *
 * Threading/ownership contract:
 *  - hal_kv_handle_t is single-owner: one open handle per (namespace,
 *    partition) pair per call site, matching today's nvs_open/nvs_close
 *    pairing. No implicit locking across handles or partitions.
 *  - Buffer-copy-in/out: get_blob with a NULL buffer is a size probe only
 *    (writes *out_len, touches no caller buffer) -- the same two-call
 *    pattern kiln_cfg_store.c uses today.
 *  - Durability: a hal_kv_set_blob/_str call MAY already be durable before
 *    hal_kv_commit() returns -- real NVS's nvs_set_* writes to flash
 *    immediately and nvs_commit() is close to a no-op on top of that, so
 *    "nothing is durable until commit" does not hold for the ESP backend.
 *    What IS guaranteed: hal_kv_commit() makes every write issued on that
 *    handle before the call durable, unconditionally. Callers must not rely
 *    on a set-then-crash (power loss / reset before commit) losing the
 *    write -- that behavior is backend- and even call-specific (NVS page
 *    write timing), not a portable guarantee this interface makes. A
 *    caller that needs "not visible after a crash unless committed" as an
 *    actual property must arrange it itself (e.g. a separate commit-flag
 *    key), not lean on this contract.
 */
#ifndef KILNCTL_HAL_KV_H
#define KILNCTL_HAL_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reservation per docs/HW_ABSTRACTION_PLAN.md "Opaque handles": sized to
 * comfortably exceed an nvs_handle_t (a 32-bit integer on ESP-IDF today)
 * plus bookkeeping (namespace/partition identity for close/commit),
 * matching hal_uart's 64 B bus-scale reservation. */
#define HAL_KV_HANDLE_STORAGE_BYTES 64

typedef struct {
    HAL_ALIGNAS8 uint8_t storage[HAL_KV_HANDLE_STORAGE_BYTES];
} hal_kv_handle_t;

typedef enum {
    HAL_KV_MODE_READ_ONLY = 0,
    HAL_KV_MODE_READ_WRITE,
} hal_kv_mode_t;

/* partition == NULL selects the default partition (nvs_open's shape);
 * non-NULL selects a named partition, matching nvs_open_from_partition
 * (kiln_nvs / wifi_nvs / profiles_nvs seen today). */
hal_status_t hal_kv_open(hal_kv_handle_t *h, const char *namespace_name,
                          hal_kv_mode_t mode, const char *partition);
hal_status_t hal_kv_close(hal_kv_handle_t *h);

/* Commits pending writes on this handle to flash. See write-context safety
 * contract above -- legal only from the flash worker / init-time context. */
hal_status_t hal_kv_commit(hal_kv_handle_t *h);

/* buf == NULL is a size probe: writes the required length to *out_len and
 * returns HAL_OK, touching no caller buffer -- the two-call pattern
 * kiln_cfg_store.c already uses. */
hal_status_t hal_kv_get_blob(hal_kv_handle_t *h, const char *key,
                              void *buf, size_t *out_len);
hal_status_t hal_kv_set_blob(hal_kv_handle_t *h, const char *key,
                              const void *buf, size_t len);

hal_status_t hal_kv_get_str(hal_kv_handle_t *h, const char *key,
                             char *buf, size_t *out_len);
hal_status_t hal_kv_set_str(hal_kv_handle_t *h, const char *key,
                             const char *value);

/* Added for profiles_builtin.c's NVS_KEY_HIDDEN mask and unit_pref.c's/
 * ramp_assist_cfg.c's single-byte settings -- genuine production scalar
 * callers this header's original census (comment above) missed: they call
 * nvs_get_u32()/nvs_set_u32() and nvs_get_u8()/nvs_set_u8() directly, not
 * the blob/str forms. */
hal_status_t hal_kv_get_u32(hal_kv_handle_t *h, const char *key, uint32_t *out);
hal_status_t hal_kv_set_u32(hal_kv_handle_t *h, const char *key, uint32_t value);
hal_status_t hal_kv_get_u8(hal_kv_handle_t *h, const char *key, uint8_t *out);
hal_status_t hal_kv_set_u8(hal_kv_handle_t *h, const char *key, uint8_t value);

/* crash_report.c and profiles_http.c's scoped-key erase. */
hal_status_t hal_kv_erase_key(hal_kv_handle_t *h, const char *key);

/* Idempotent, with erase-retry -- matches today's nvs_flash_init_partition
 * fallback-to-erase-and-retry pattern used ~13 places at boot. */
hal_status_t hal_kv_init_partition(const char *partition);

/* Non-erasing mount probe -- nvs_report_capture()'s use (re-observing a
 * partition every OTHER module has already brought up at boot, per
 * main.c ordering) must never itself trigger an erase-and-retry: erasing a
 * partition just to report on it would silently destroy whatever data the
 * module that actually owns it was relying on. Same underlying
 * nvs_flash_init_partition() call hal_kv_init_partition() makes, but WITHOUT
 * the NO_FREE_PAGES/NEW_VERSION_FOUND erase-retry branch -- a partition that
 * needs erasing is reported not-ready, not silently fixed. Returns HAL_OK if
 * the partition is already mounted, HAL_NOT_READY if it is present but not
 * mountable as-is (the caller should NOT erase it itself -- that decision
 * belongs to whichever module owns the partition and calls
 * hal_kv_init_partition()), or the mapped error for any other failure. */
hal_status_t hal_kv_mount_probe(const char *partition);

/* factory_reset.c's scoped erase of one partition. */
hal_status_t hal_kv_erase_partition(const char *partition);

typedef struct {
    size_t used_entries;
    size_t free_entries;
    size_t total_entries;
} hal_kv_stats_t;

/* nvs_report's enumeration (ui_page_diagnostics.c). */
hal_status_t hal_kv_stats(const char *partition, hal_kv_stats_t *out);

/* Exposes today's caller_stack_is_external predicate so flash_worker_lint.py
 * and the existing copy-pasted guards can be rekeyed onto hal_kv_set_* /
 * hal_kv_commit / hal_kv_erase_* verbatim. See write-context safety
 * contract above -- this function reports the contract, it does not
 * enforce it. */
bool hal_kv_write_safe_here(void);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_KV_H */
