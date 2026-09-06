/* hal_kv_pico.c -- pico-sdk backend for interface/hal_kv.h.
 *
 * ============================================================================
 * INTERFACE MISMATCH -- read before touching this file.
 * ============================================================================
 * hal_kv.h's own header comment already says this plainly: "hal_kv -- ESP-
 * only, wraps NVS. Pico explicitly excluded" and docs/HW_ABSTRACTION_PLAN.md
 * ("hal_kv -- ESP-only, wraps NVS. Pico explicitly excluded") gives the full
 * reasoning: SaftyFW's real persistent store, config_store.c/
 * config_store_flash.c, is NOT a key/value store. It is:
 *   - ONE fixed-layout 512-byte C struct (config_store_record_t), not an
 *     arbitrary set of named blobs/strings.
 *   - seq-numbered and CRC'd, with an 8-slot round-robin append-only log in
 *     one 4 KB sector (config_store.h's own header comment on why an
 *     append-only log, not NVS-style key wear-levelling, is the only scheme
 *     safe on NOR flash within one erase-granularity sector).
 *   - Gated by a hard safety interlock: config_store_write() unconditionally
 *     refuses while the relay is ARMED (config_store_decide_write(),
 *     checked against relay_owner_get_state() in config_store_flash.c),
 *     regardless of which field would change. hal_kv.h has no ARMED concept
 *     at all -- NVS has no such interlock on the ESP side either.
 *   - format-versioned with an explicit REFUSE-not-reinterpret policy for
 *     anything newer than this firmware understands (config_store.h's
 *     "Load-time rejection diagnostics" section) -- there is no hal_kv.h
 *     concept of "this get_blob call failed because the record's
 *     format_version is from the future," only the generic HAL_IO/
 *     HAL_NOT_FOUND vocabulary NVS failures use.
 *   - written through flash_safe_execute() (RP2040 multicore XIP lockout,
 *     config_store_flash.c's config_store_write_cb()), not the NVS
 *     wear-levelled flash driver hal_kv.h's write-context-safety contract
 *     (hal_kv_write_safe_here(), "PSRAM-stacked task panics") was written
 *     against -- that specific hazard (a PSRAM-backed task stack) does not
 *     exist on the RP2040 at all; SaftyFW has no external RAM, so every task
 *     stack is plain SRAM and hal_kv_write_safe_here()'s ESP-specific
 *     predicate has no pico equivalent to report.
 *
 * The plan's own conclusion: "Forcing it through hal_kv would either strip
 * the ARMED gate to a caller-side check or bloat the interface with
 * slot/seq/ARMED concepts one platform needs. It sits on hal_flash instead
 * (separate interface, pico backend for config_store_flash.c)." hal_flash.h
 * does not exist in interface/ yet (docs/HW_ABSTRACTION_PLAN.md: "fake_flash
 * remains unstarted: interface/ still has no hal_flash.h") -- config_store's
 * real home is a header this pass is not authorized to add (task scope is
 * pico/uart, pico/time, pico/kv only; interface/ is off-limits).
 *
 * What this file does instead, since a hal_kv_pico.c was asked for: rather
 * than silently doing nothing, or fabricating a general-purpose namespace/
 * key store config_store.c does not have, this backend implements the ONE
 * mapping that is honest and does not widen or reinterpret either side:
 *   - Exactly one blessed (namespace, key) pair is accepted:
 *     namespace "config", key "record". Every other namespace/key is refused
 *     with HAL_NOT_SUPPORTED -- not HAL_NOT_FOUND, because "not found" would
 *     imply a real per-key store that simply lacks that entry, which is not
 *     what is happening here.
 *   - hal_kv_get_blob on that pair returns the CURRENT config_store_record_t,
 *     packed via config_store_pack() into hal_kv.h's buf/out_len shape
 *     (including the size-probe convention, buf == NULL). This is read-only
 *     information config_store_flash.c already exposes
 *     (config_store_get_full_record()); no new read path is invented.
 *   - hal_kv_set_blob on that pair requires EXACTLY CONFIG_STORE_RECORD_LEN
 *     bytes, unpacks them with config_store_unpack() (the real, validated
 *     record parser -- a bad blob is refused with HAL_INVALID_ARG, matching
 *     config_store_unpack()'s own "leaves *out completely untouched" refusal
 *     contract) and writes the result through the REAL config_store_write(),
 *     so the ARMED refusal, the seq bump, and the flash_safe_execute()
 *     multicore lockout all still happen exactly as they do for every other
 *     config_store_write() caller. ARMED refusal maps to HAL_BUSY (a
 *     recoverable, retry-later condition -- disarm and retry -- not a
 *     caller-argument error); a flash_safe_execute() failure maps to
 *     HAL_TIMEOUT or HAL_IO depending on which pico_error_codes reason
 *     config_store_flash_rc_reason() names, since hal_status_t already has a
 *     dedicated HAL_TIMEOUT and nothing more specific than HAL_IO for
 *     "NOT_PERMITTED"/"INSUFFICIENT_RESOURCES" (hal_flash.h, when it exists,
 *     is where those three distinct reasons per the plan's hal_flash section
 *     belong -- not here).
 *   - hal_kv_commit() on an open handle is a documented no-op returning
 *     HAL_OK: config_store_write() commits atomically in one call (pack,
 *     flash_safe_execute, cache-update) with no separate pending-vs-
 *     committed staging the way NVS's nvs_set_blob()-then-nvs_commit() two-
 *     step has. There is no "pending write" for a later hal_kv_commit() to
 *     flush -- see MISMATCH 3 below.
 *   - Every other call (get/set_str, erase_key, init_partition,
 *     erase_partition, stats) returns HAL_NOT_SUPPORTED -- config_store has
 *     no string fields, no scoped-key erase (it is refuse-and-rewrite-whole-
 *     record or nothing), and no partition concept at all (one fixed flash
 *     region, SAFTYFW_CONFIG_STORE_FLASH_OFFSET/_SIZE from flash_layout.h).
 *
 * Enumerated mismatches, for anyone deciding what a real hal_flash-based
 * replacement must cover:
 *   1. Namespace/key model vs. one fixed struct. hal_kv.h's whole surface is
 *      shaped around "many named entries per namespace"; config_store has
 *      exactly one entry. The single-blessed-pair mapping above is a
 *      deliberate narrowing, not a generalization -- a second real record
 *      type would need a second blessed pair added by hand, not a generic
 *      key lookup.
 *   2. ARMED refusal collapses to a bare hal_status_t. The real refusal
 *      carries a human-readable reason string (config_store_write_decision_
 *      reason()) that a UI would want to surface verbatim; hal_kv_set_blob's
 *      hal_status_t-only return cannot carry it. Widening hal_kv.h to add an
 *      out-string parameter was considered and rejected -- that is
 *      widening the interface for one backend's benefit, against this
 *      pass's constraint.
 *   3. No pending-vs-committed durability. hal_kv.h's own contract says
 *      "nothing is durable until hal_kv_commit()" (matching NVS's real
 *      two-step model, and fake_kv's pending-vs-committed simulation per the
 *      plan's Host fakes section). This backend's set_blob is durable
 *      IMMEDIATELY (config_store_write() has already called
 *      flash_safe_execute() and returned before set_blob returns) --
 *      hal_kv_commit() has nothing left to do. A caller relying on hal_kv.h's
 *      documented "set-then-crash loses the write" semantics would be wrong
 *      about this backend specifically.
 *   4. format_version REFUSE-not-reinterpret has no hal_status_t of its own.
 *      A newer-than-this-firmware record byte blob handed to hal_kv_set_blob
 *      is refused via config_store_unpack()'s ordinary false return, which
 *      this backend maps to the same HAL_INVALID_ARG a plain CRC failure or
 *      wrong-length buffer gets -- callers cannot distinguish "your bytes
 *      were garbage" from "your bytes were a valid, too-new record" through
 *      this interface, though config_store_unpack_ex()'s richer
 *      config_store_reject_info_t exists and could be threaded through a
 *      hal_flash-based interface that has room for it.
 *   5. hal_kv_write_safe_here() -- the ESP-specific PSRAM-stacked-task-panic
 *      predicate hal_kv.h documents this function as exposing has no RP2040
 *      analog (SaftyFW has no external/PSRAM RAM at all, so no task stack on
 *      this target can be in the failure mode NVS writes guard against).
 *      This backend returns `true` unconditionally, documented below as "not
 *      a real predicate, just always safe on this target" rather than a
 *      meaningful safety check -- a caller must not treat pico's `true` as
 *      having verified anything analogous to the ESP backend's real check.
 * ============================================================================
 *
 * Not wired into any CMakeLists yet -- see
 * firmware/hwAbstraction/test/compile_pico_backends.ps1 for the syntax-only
 * compile check that stands in for that until Phase 1a's real move lands.
 */
#include "hal_kv.h"

#include <string.h>

#include "config_store.h"

/* hal_kv_handle_t storage layout for this backend: just enough to remember
 * which (namespace, mode) this handle was opened with, so hal_kv_get/set_blob
 * can refuse anything but the one blessed "config"/"record" pair and so a
 * READ_ONLY handle can refuse writes at the HAL layer too (defense in depth
 * on top of config_store_write()'s own ARMED check, same "belt and
 * suspenders" spirit as hal_kv.h's write-context-safety contract). Comfortably
 * under HAL_KV_HANDLE_STORAGE_BYTES (64 B, see hal_kv.h).
 */
#define HAL_KV_PICO_NAMESPACE_MAX 16u

typedef struct {
    char          namespace_copy[HAL_KV_PICO_NAMESPACE_MAX];
    hal_kv_mode_t mode;
    bool          open;
} hal_kv_pico_handle_state_t;

_Static_assert(sizeof(hal_kv_pico_handle_state_t) <= HAL_KV_HANDLE_STORAGE_BYTES,
               "hal_kv_pico_handle_state_t must fit hal_kv_handle_t's opaque storage");

static const char HAL_KV_PICO_NAMESPACE[] = "config";
static const char HAL_KV_PICO_KEY[] = "record";

static hal_kv_pico_handle_state_t *hal_kv_pico_state(hal_kv_handle_t *h) {
    return (hal_kv_pico_handle_state_t *)(void *)h->storage;
}

static const hal_kv_pico_handle_state_t *hal_kv_pico_state_c(const hal_kv_handle_t *h) {
    return (const hal_kv_pico_handle_state_t *)(const void *)h->storage;
}

hal_status_t hal_kv_open(hal_kv_handle_t *h, const char *namespace_name,
                          hal_kv_mode_t mode, const char *partition) {
    if (h == NULL || namespace_name == NULL) {
        return HAL_INVALID_ARG;
    }
    /* config_store has no partition concept at all -- see MISMATCH list
     * above. A caller asking for a specific partition (non-NULL) is asking
     * for something this backend cannot honor; only the implicit single
     * region is available. */
    if (partition != NULL) {
        return HAL_NOT_SUPPORTED;
    }
    if (strncmp(namespace_name, HAL_KV_PICO_NAMESPACE, sizeof(HAL_KV_PICO_NAMESPACE)) != 0) {
        return HAL_NOT_SUPPORTED;
    }

    hal_kv_pico_handle_state_t *st = hal_kv_pico_state(h);
    memset(st, 0, sizeof(*st));
    strncpy(st->namespace_copy, namespace_name, sizeof(st->namespace_copy) - 1u);
    st->mode = mode;
    st->open = true;
    return HAL_OK;
}

hal_status_t hal_kv_close(hal_kv_handle_t *h) {
    if (h == NULL) {
        return HAL_INVALID_ARG;
    }
    hal_kv_pico_handle_state_t *st = hal_kv_pico_state(h);
    st->open = false;
    return HAL_OK;
}

hal_status_t hal_kv_commit(hal_kv_handle_t *h) {
    if (h == NULL || !hal_kv_pico_state(h)->open) {
        return HAL_NOT_READY;
    }
    /* No-op: see MISMATCH 3 above -- config_store_write() (called from
     * hal_kv_set_blob) is already fully durable by the time it returns. */
    return HAL_OK;
}

hal_status_t hal_kv_get_blob(hal_kv_handle_t *h, const char *key,
                              void *buf, size_t *out_len) {
    if (h == NULL || key == NULL || out_len == NULL || !hal_kv_pico_state(h)->open) {
        return HAL_INVALID_ARG;
    }
    if (strncmp(key, HAL_KV_PICO_KEY, sizeof(HAL_KV_PICO_KEY)) != 0) {
        return HAL_NOT_SUPPORTED;
    }

    /* buf == NULL is the documented size-probe convention (hal_kv.h). */
    if (buf == NULL) {
        *out_len = CONFIG_STORE_RECORD_LEN;
        return HAL_OK;
    }
    if (*out_len < CONFIG_STORE_RECORD_LEN) {
        *out_len = CONFIG_STORE_RECORD_LEN;
        return HAL_INVALID_SIZE;
    }

    config_store_record_t rec;
    config_store_get_full_record(&rec);
    config_store_pack(&rec, (uint8_t *)buf);
    *out_len = CONFIG_STORE_RECORD_LEN;
    return HAL_OK;
}

hal_status_t hal_kv_set_blob(hal_kv_handle_t *h, const char *key,
                              const void *buf, size_t len) {
    if (h == NULL || key == NULL || buf == NULL) {
        return HAL_INVALID_ARG;
    }
    hal_kv_pico_handle_state_t *st = hal_kv_pico_state(h);
    if (!st->open) {
        return HAL_NOT_READY;
    }
    if (st->mode != HAL_KV_MODE_READ_WRITE) {
        return HAL_INVALID_ARG;
    }
    if (strncmp(key, HAL_KV_PICO_KEY, sizeof(HAL_KV_PICO_KEY)) != 0) {
        return HAL_NOT_SUPPORTED;
    }
    if (len != CONFIG_STORE_RECORD_LEN) {
        return HAL_INVALID_SIZE;
    }

    config_store_record_t rec;
    if (!config_store_unpack((const uint8_t *)buf, &rec)) {
        /* Bad magic/CRC/format_version, OR a structurally-valid-but-too-new
         * record -- config_store_unpack() collapses all of these to false.
         * See MISMATCH 4 above: this interface cannot tell them apart. */
        return HAL_INVALID_ARG;
    }

    const char *reason = NULL;
    if (!config_store_write(&rec, &reason)) {
        /* config_store_write()'s only two failure shapes today: ARMED
         * refusal (config_store_decide_write()) and a flash_safe_execute()
         * failure (config_store_flash_rc_reason()). Neither is distinguished
         * any further than this by config_store_write()'s bool return, so
         * neither is by this mapping -- see MISMATCH 2 above for why the
         * human-readable `reason` string cannot be threaded back through
         * hal_status_t. HAL_BUSY: matches hal_status.h's own documented
         * "resource in use; caller may retry" -- ARMED is exactly a
         * recoverable-by-retry-after-disarming condition, not a caller
         * argument error (HAL_INVALID_ARG) or a hard IO fault (HAL_IO). A
         * genuine flash_safe_execute() failure is comparatively rare and
         * also recoverable by retrying, so it is reported the same way
         * rather than invented a status this table has no dedicated slot
         * for; a hal_flash-based replacement should surface
         * config_store_flash_rc_reason()'s three distinct reasons properly
         * (docs/HW_ABSTRACTION_PLAN.md's hal_flash section already
         * specifies TIMEOUT/NOT_PERMITTED/INSUFFICIENT_RESOURCES mapping
         * onto hal_status_t for exactly this reason). */
        (void)reason;
        return HAL_BUSY;
    }
    return HAL_OK;
}

hal_status_t hal_kv_get_str(hal_kv_handle_t *h, const char *key,
                             char *buf, size_t *out_len) {
    (void)h;
    (void)key;
    (void)buf;
    (void)out_len;
    /* config_store has no string fields at all. */
    return HAL_NOT_SUPPORTED;
}

hal_status_t hal_kv_set_str(hal_kv_handle_t *h, const char *key,
                             const char *value) {
    (void)h;
    (void)key;
    (void)value;
    return HAL_NOT_SUPPORTED;
}

hal_status_t hal_kv_erase_key(hal_kv_handle_t *h, const char *key) {
    (void)h;
    (void)key;
    /* config_store has no scoped-key erase -- it is "write a whole new
     * record" or nothing; there is no analog of NVS's per-key delete. */
    return HAL_NOT_SUPPORTED;
}

hal_status_t hal_kv_init_partition(const char *partition) {
    (void)partition;
    /* No partition concept -- see MISMATCH list above. */
    return HAL_NOT_SUPPORTED;
}

hal_status_t hal_kv_erase_partition(const char *partition) {
    (void)partition;
    /* config_store_flash.c exposes no sector-erase-only entry point
     * independent of writing a new record (config_store_write_cb() only
     * erases as part of a program cycle, gated by config_store_next_write_
     * needs_erase()) -- there is nothing this call could invoke without
     * either fabricating a new real function in config_store_flash.c (out
     * of scope: that file is not touched by this pass) or bypassing the
     * ARMED check entirely, which this backend will not do silently. */
    return HAL_NOT_SUPPORTED;
}

hal_status_t hal_kv_stats(const char *partition, hal_kv_stats_t *out) {
    (void)partition;
    (void)out;
    /* config_store has no NVS-style entry enumeration -- it is one fixed
     * record, not a variable set of key/value entries to count. */
    return HAL_NOT_SUPPORTED;
}

bool hal_kv_write_safe_here(void) {
    /* See MISMATCH 5 above: not a real predicate on this target. SaftyFW has
     * no PSRAM-stacked tasks (RP2040, no external RAM), so the specific
     * hazard hal_kv.h documents this function as reporting cannot occur
     * here -- every call site is unconditionally "safe" by the ESP
     * backend's own definition, which is why this is `true` always rather
     * than a check against something meaningful. */
    return true;
}
