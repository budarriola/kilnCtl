/* hal_kv_esp.c -- ESP-IDF backend for interface/hal_kv.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against ESP-IDF
 * v6.0.2 (C:\esp\v6.0.2\esp-idf, components/nvs_flash/include/nvs.h and
 * nvs_flash.h), grounded in the real KilnFW consumers named in hal_kv.h's
 * own header comment and docs/HW_ABSTRACTION_PLAN.md's "hal_kv" section:
 * kiln_cfg_store.c (nvs_open_from_partition + get/set_blob size-probe
 * pattern, KILN_NVS_PARTITION), zones_config_*.c and profiles_*.c (the
 * same blob pattern, versioned migration chains built on top of
 * hal_kv_get_blob -- that call-site logic is unchanged by this backend),
 * backup_*.c (blob export/import), crash_report.c and profiles_http.c
 * (nvs_erase_key), boot_guard.c/crash_report.c (the erase-and-retry
 * nvs_flash_init_partition idiom), factory_reset.c (nvs_flash_erase_partition
 * scoped erase) and ui_page_diagnostics.c (nvs_get_stats). Not wired into
 * any CMakeLists yet -- see
 * firmware/hwAbstraction/test/compile_esp_backends.ps1 for the syntax-only
 * compile check standing in for that until Phase 1a's real move lands.
 *
 * Handle storage: hal_kv_handle_t reserves HAL_KV_HANDLE_STORAGE_BYTES (64)
 * bytes; this backend stores only a single nvs_handle_t (a uint32_t on
 * ESP-IDF today) plus an "open" flag, far under the reservation -- see the
 * _Static_assert below. nvs_close()/nvs_commit() take only the handle, not
 * the namespace/partition string, so nothing else needs to be carried.
 *
 * hal_kv_commit() durability: nvs_set_blob()/nvs_set_str()/nvs_erase_key()
 * write into NVS's in-RAM/staged representation immediately and are visible
 * to a subsequent nvs_get_* on the SAME handle or a fresh nvs_open() before
 * any commit -- NVS itself does not buffer writes in a way a crash could
 * lose independently of flash wear-leveling; nvs_commit() forces those
 * pages to be written out now rather than leaving it to NVS's own internal
 * flushing. Per hal_kv.h's own contract comment, a hal_kv_set_blob/_str call
 * MAY already be durable before hal_kv_commit() returns -- real NVS's
 * nvs_set_* writes to flash immediately, so "nothing is durable until
 * commit" does NOT hold for this backend. What hal_kv_commit() DOES
 * guarantee: every write issued on that handle before the call is durable,
 * unconditionally, once it returns. Callers must not rely on a
 * set-then-crash (power loss / reset before commit) losing the write --
 * that behavior is backend- and even call-specific (NVS page write timing),
 * not something this interface promises either way. This backend's
 * hal_kv_commit() does exactly what nvs_commit() does today -- no
 * additional buffering is introduced or removed.
 *
 * INTERFACE MISMATCH -- ESP_ERR_NVS_NOT_FOUND vs the shared esp_err_t
 * mapper: hal_esp_common.c's hal_esp_err_to_status() only maps the
 * generic-driver code ESP_ERR_NOT_FOUND (0x105), not NVS's own distinct
 * ESP_ERR_NVS_NOT_FOUND (0x1102, "key not found" from nvs_get_* /
 * nvs_erase_key). Falling through hal_esp_err_to_status() alone would
 * therefore map "key not found" to the HAL_IO catch-all, which is wrong:
 * kiln_cfg_store.c and profiles_http.c both branch on this exact code
 * ("nothing stored yet" / "already gone, not an error" -- see
 * profiles_http.c:597's `erase_err != ESP_ERR_NVS_NOT_FOUND` check) and
 * hal_status.h already has a value for exactly this case, HAL_NOT_FOUND
 * ("probe loops continue-on-NOT_FOUND... must not collapse into HAL_IO").
 * Per task instructions this is reported rather than widening hal_kv.h or
 * hal_esp_common.h (which is shared by every esp/ backend, not kv-specific):
 * this file maps the NVS-specific codes itself, in hal_kv_esp_err_to_status()
 * below, before falling back to the shared mapper for anything generic.
 * The other NVS-specific codes get the same treatment for the same reason
 * (none of them exist in the shared driver-facing switch, and several are
 * branched on today): ESP_ERR_NVS_INVALID_LENGTH -> HAL_INVALID_SIZE
 * (mirrors the shared table's ESP_ERR_INVALID_SIZE case; this is the
 * get_blob "buffer too small for the size-probe caller's real buffer"
 * code), ESP_ERR_NVS_NOT_ENOUGH_SPACE -> HAL_NO_MEM (out-of-space on the
 * partition, not a driver malloc failure, but the closest existing
 * meaning), ESP_ERR_NVS_INVALID_HANDLE -> HAL_INVALID_ARG (a closed/bad
 * handle passed back in, i.e. caller error), ESP_ERR_NVS_READ_ONLY ->
 * HAL_INVALID_ARG (a write attempted on a HAL_KV_MODE_READ_ONLY handle --
 * caller error, not a transport failure), ESP_ERR_NVS_NO_FREE_PAGES and
 * ESP_ERR_NVS_NEW_VERSION_FOUND are NOT mapped here because they are only
 * ever consumed inside hal_kv_init_partition()'s own erase-and-retry loop
 * (see boot_guard.c/crash_report.c's identical idiom) and never surface to
 * a caller as a return value.
 *
 * Write-context safety (hal_kv_write_safe_here(), hal_kv.h's contract):
 * implements today's caller_stack_is_external() predicate verbatim, copied
 * from kiln_cfg_store.c/profiles_http.c/relay_cycles.c/run_state.c/
 * safety_cfg_store.c (five independent copies of the identical function --
 * this backend is the first shared home for it). It checks exactly one
 * thing: whether the ADDRESS of a local (stack) variable in the CURRENTLY
 * EXECUTING task's own stack frame lies in external RAM (PSRAM), via
 * esp_ptr_external_ram() -- the same check flash_worker.h's HAZARD comment
 * and uart_bridge_ext.c's flash worker exist to make unnecessary for their
 * own callers. It does NOT check task identity (e.g. "is this the flash
 * worker task"); it is a pure "would a flash/NVS write from HERE, RIGHT
 * NOW abort the board" probe, because that is the only thing
 * esp_task_stack_is_sane_cache_disabled() itself cares about --a
 * PSRAM-stacked task's stack becomes unreachable the instant a flash op
 * disables the cache, independent of which task it is. A task with an
 * internal-DRAM stack that happens not to be the flash worker still passes
 * this check (e.g. init-time code before any PSRAM-stacked task exists, per
 * hal_kv.h's contract comment) -- that is intentional, matching every one
 * of the five existing copies, not a gap introduced here.
 *
 * Compile-time size pin: nvs_handle_t is a uint32_t handle on ESP-IDF
 * today; the struct below adds one bool for "is this handle open" bookkeeping
 * (nvs_close()/nvs_commit() misuse guard) and is far under the 64-byte
 * reservation.
 */
#include "hal_kv.h"

#include <string.h>

#include "esp_memory_utils.h" /* esp_ptr_external_ram() -- hal_kv_write_safe_here() */
#include "nvs.h"
#include "nvs_flash.h"

#include "hal_esp_common.h"

struct hal_kv_esp_impl {
    nvs_handle_t handle;
    bool is_open;
};

_Static_assert(sizeof(struct hal_kv_esp_impl) <= sizeof(((hal_kv_handle_t *)0)->storage),
               "hal_kv_esp_impl exceeds HAL_KV_HANDLE_STORAGE_BYTES reservation");

static struct hal_kv_esp_impl *hal_kv_esp_impl(hal_kv_handle_t *h) {
    return (struct hal_kv_esp_impl *)(void *)h->storage;
}

/* See the INTERFACE MISMATCH note above: NVS-specific codes this backend's
 * consumers actually branch on, checked before falling back to the shared,
 * driver-generic mapper in hal_esp_common.c. */
static hal_status_t hal_kv_esp_err_to_status(esp_err_t err) {
    switch (err) {
        case ESP_ERR_NVS_NOT_FOUND:         return HAL_NOT_FOUND;
        case ESP_ERR_NVS_INVALID_LENGTH:    return HAL_INVALID_SIZE;
        case ESP_ERR_NVS_NOT_ENOUGH_SPACE:  return HAL_NO_MEM;
        case ESP_ERR_NVS_INVALID_HANDLE:    return HAL_INVALID_ARG;
        case ESP_ERR_NVS_READ_ONLY:         return HAL_INVALID_ARG;
        default:                            return hal_esp_err_to_status(err);
    }
}

hal_status_t hal_kv_open(hal_kv_handle_t *h, const char *namespace_name,
                          hal_kv_mode_t mode, const char *partition) {
    if (!h || !namespace_name) {
        return HAL_INVALID_ARG;
    }
    struct hal_kv_esp_impl *impl = hal_kv_esp_impl(h);
    nvs_open_mode_t nvs_mode = (mode == HAL_KV_MODE_READ_WRITE) ? NVS_READWRITE : NVS_READONLY;

    esp_err_t err;
    if (partition) {
        err = nvs_open_from_partition(partition, namespace_name, nvs_mode, &impl->handle);
    } else {
        /* Default-partition form -- relay_cycles.c:96 and run_state.c:131's
         * plain nvs_open(). */
        err = nvs_open(namespace_name, nvs_mode, &impl->handle);
    }
    if (err != ESP_OK) {
        impl->is_open = false;
        return hal_kv_esp_err_to_status(err);
    }
    impl->is_open = true;
    return HAL_OK;
}

hal_status_t hal_kv_close(hal_kv_handle_t *h) {
    if (!h) {
        return HAL_INVALID_ARG;
    }
    struct hal_kv_esp_impl *impl = hal_kv_esp_impl(h);
    if (!impl->is_open) {
        return HAL_NOT_READY;
    }
    nvs_close(impl->handle);
    impl->is_open = false;
    return HAL_OK;
}

hal_status_t hal_kv_commit(hal_kv_handle_t *h) {
    if (!h) {
        return HAL_INVALID_ARG;
    }
    struct hal_kv_esp_impl *impl = hal_kv_esp_impl(h);
    if (!impl->is_open) {
        return HAL_NOT_READY;
    }
    /* See the durability comment at the top of this file: forces NVS's
     * staged writes on this handle out to flash now. */
    esp_err_t err = nvs_commit(impl->handle);
    return hal_kv_esp_err_to_status(err);
}

hal_status_t hal_kv_get_blob(hal_kv_handle_t *h, const char *key,
                              void *buf, size_t *out_len) {
    if (!h || !key || !out_len) {
        return HAL_INVALID_ARG;
    }
    struct hal_kv_esp_impl *impl = hal_kv_esp_impl(h);
    if (!impl->is_open) {
        return HAL_NOT_READY;
    }
    /* buf == NULL is the two-call size-probe pattern kiln_cfg_store.c uses
     * (nvs_get_blob's own NULL-buffer contract, passed straight through). */
    esp_err_t err = nvs_get_blob(impl->handle, key, buf, out_len);
    return hal_kv_esp_err_to_status(err);
}

hal_status_t hal_kv_set_blob(hal_kv_handle_t *h, const char *key,
                              const void *buf, size_t len) {
    if (!h || !key || (!buf && len != 0)) {
        return HAL_INVALID_ARG;
    }
    struct hal_kv_esp_impl *impl = hal_kv_esp_impl(h);
    if (!impl->is_open) {
        return HAL_NOT_READY;
    }
    esp_err_t err = nvs_set_blob(impl->handle, key, buf, len);
    return hal_kv_esp_err_to_status(err);
}

hal_status_t hal_kv_get_str(hal_kv_handle_t *h, const char *key,
                             char *buf, size_t *out_len) {
    if (!h || !key || !out_len) {
        return HAL_INVALID_ARG;
    }
    struct hal_kv_esp_impl *impl = hal_kv_esp_impl(h);
    if (!impl->is_open) {
        return HAL_NOT_READY;
    }
    /* Same NULL-buffer size-probe contract as nvs_get_blob. */
    esp_err_t err = nvs_get_str(impl->handle, key, buf, out_len);
    return hal_kv_esp_err_to_status(err);
}

hal_status_t hal_kv_set_str(hal_kv_handle_t *h, const char *key,
                             const char *value) {
    if (!h || !key || !value) {
        return HAL_INVALID_ARG;
    }
    struct hal_kv_esp_impl *impl = hal_kv_esp_impl(h);
    if (!impl->is_open) {
        return HAL_NOT_READY;
    }
    esp_err_t err = nvs_set_str(impl->handle, key, value);
    return hal_kv_esp_err_to_status(err);
}

hal_status_t hal_kv_erase_key(hal_kv_handle_t *h, const char *key) {
    if (!h || !key) {
        return HAL_INVALID_ARG;
    }
    struct hal_kv_esp_impl *impl = hal_kv_esp_impl(h);
    if (!impl->is_open) {
        return HAL_NOT_READY;
    }
    esp_err_t err = nvs_erase_key(impl->handle, key);
    return hal_kv_esp_err_to_status(err);
}

hal_status_t hal_kv_init_partition(const char *partition) {
    if (!partition) {
        return HAL_INVALID_ARG;
    }
    /* Idempotent, erase-and-retry idiom copied verbatim from
     * boot_guard.c:115-124 / crash_report.c:41-51 (~13 call sites share this
     * shape at boot). nvs_flash_init_partition() is itself a no-op success
     * if the partition is already initialized. */
    esp_err_t err = nvs_flash_init_partition(partition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase_partition(partition);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(partition);
        }
    }
    return hal_kv_esp_err_to_status(err);
}

hal_status_t hal_kv_erase_partition(const char *partition) {
    if (!partition) {
        return HAL_INVALID_ARG;
    }
    /* factory_reset.c:125's scoped erase -- de-initializes the partition,
     * same as the real call; caller is responsible for re-init if it keeps
     * using the partition afterward (factory_reset.c does not). */
    esp_err_t err = nvs_flash_erase_partition(partition);
    return hal_kv_esp_err_to_status(err);
}

hal_status_t hal_kv_stats(const char *partition, hal_kv_stats_t *out) {
    if (!out) {
        return HAL_INVALID_ARG;
    }
    /* ui_page_diagnostics.c:570's nvs_get_stats(NULL, &stats) -- partition
     * NULL selects the default partition, same convention as hal_kv_open. */
    nvs_stats_t stats;
    esp_err_t err = nvs_get_stats(partition, &stats);
    if (err != ESP_OK) {
        return hal_kv_esp_err_to_status(err);
    }
    out->used_entries = stats.used_entries;
    out->free_entries = stats.free_entries;
    out->total_entries = stats.total_entries;
    return HAL_OK;
}

/* See the write-context-safety comment at the top of this file: identical
 * predicate to kiln_cfg_store.c/profiles_http.c/relay_cycles.c/run_state.c/
 * safety_cfg_store.c's five independent caller_stack_is_external() copies. */
bool hal_kv_write_safe_here(void) {
    volatile int stack_probe = 0; /* only its ADDRESS matters; volatile+initialised so
                                    * -Werror=maybe-uninitialized doesn't flag it and it
                                    * can't be optimised out of the frame. */
    return !esp_ptr_external_ram((void *)&stack_probe);
}
