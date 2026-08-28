// safety_cfg_store -- the ESP-side cache of the RP2040 safety processor's
// commissioning parameter set (docs/COMMISSIONING.md sec 3, CONFIG_REFERENCE.md
// secs 1-5).
//
// WHY A CACHE EXISTS AT ALL: the owner's own decision (COMMISSIONING.md's
// opening quote) is "these should not be sent from the ESP every boot, but
// they must be set at least once, and the ESP should receive these sometime
// during boot up or on request." The Pico is the only place these values are
// ENFORCED (COMMISSIONING.md sec 1: "the processor that enforces a limit is
// the one that must hold it") -- this module never enforces anything, it only
// remembers the last set of values this ESP fetched, so:
//   - the GUI can render instantly on page load, and
//   - a dead safety link still shows *something* (clearly last-known, never
//     silently presented as current -- see safety_cfg_store_stale() below).
//
// FETCH-ON-CHANGE, NOT FETCH-ON-BOOT: every SAFETY_CMD_FW_VERSION frame the
// Pico sends carries its live config_crc (safety_link.h's
// safety_link_get_peer_build_status()). safety_cfg_store_maybe_refetch() is
// the one function that compares that live value against this cache's
// cached_config_crc and only pages through safety_link_get_config_page() when
// they differ -- a steady-state reboot of either processor, where both CRCs
// already agree, transfers nothing. safety_link.c's poll task is the intended
// caller (right after it applies a fresh FW_VERSION frame), mirroring the
// same "one module calls into another it doesn't otherwise depend on, because
// that's where the trigger naturally fires" precedent safety_link.c's
// safety_sync_tc_type() already set for zones_http.h.
//
// THE CRC IS THE AUTHORITY, NEVER THIS CACHE: safety_cfg_store_get_all() and
// safety_cfg_store_cached_crc() report exactly what was fetched and when --
// they never claim to know what the Pico is enforcing RIGHT NOW. Comparing
// this module's cached_config_crc against safety_link's live peer_config_crc
// is the caller's (safety_cfg_http.c's) job, every time, precisely so a
// caller can never mistake the cache for live truth (COMMISSIONING.md sec 3's
// "stale" flag).
//
// PERSISTENCE: kiln_nvs / namespace "kiln_cfg" (key "safetycfg") -- the SAME
// partition and namespace zones_http.c/kiln_cfg_store.c already use, for the
// identical reason documented in kiln_cfg_store.h's header comment: kiln_nvs
// is a `data` partition, untouched by an ordinary reflash/OTA of either
// application slot (verified against this project's actual flash args, same
// citation kiln_cfg_store.h makes) -- this cache survives a reprogram of the
// ESP exactly like every other operator-set parameter on this board does. A
// full NVS erase (not an ordinary reflash) does lose it, same as every other
// kiln_nvs consumer; the Pico's own config_store is the actual source of
// truth regardless (see this file's top paragraph), so losing the ESP's
// cache only costs one refetch on the next FW_VERSION frame, never a real
// commissioning value.
#ifndef SAFETY_CFG_STORE_H
#define SAFETY_CFG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "kilnlink/kilnlink_param_value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every CONFIG_REFERENCE.md secs 1-5 tunable, per COMMISSIONING.md sec 2.1's
 * param_id table -- 8 (sec 1) + 16 (sec 2) + 24 (sec 3) + 5 (sec 4) +
 * 4 (sec 5) = 57, plus safety_tc_installed (0x0211, 2026-08-23) = 58, plus
 * max_expected_power_w (0x0319, ROADMAP.md M12) = 59. Bump this (and
 * safety_cfg_store.c's SAFETY_CFG_PARAM_TABLE) only when CONFIG_REFERENCE.md
 * itself grows a field -- ids are permanent (COMMISSIONING.md sec 2.1: "a
 * field that is removed leaves its id burned, never reused"), so this count
 * only ever goes up. */
#define SAFETY_CFG_PARAM_COUNT 59u

/* One row of safety_cfg_store_get_by_index()'s output -- everything
 * safety_cfg_http.c's GET handler needs to emit one `params[]` entry.
 * `set == false` means "never fetched with a value" (the four sec-1 fields
 * with no compiled-in default, before commissioning, read this way) --
 * `value` is then meaningless and MUST NOT be treated as a real 0/false,
 * exactly per COMMISSIONING.md sec 3.1: "An unset parameter carries
 * `"set": false` and OMITS `value` entirely rather than sending a zero the
 * page might print." */
typedef struct {
    uint16_t param_id;
    const char *name;
    uint8_t type; /* KILNLINK_PARAM_TYPE_* */
    bool set;
    kilnlink_param_value_t value;
} safety_cfg_param_t;

/* SafetyLinkClass is an anonymous-struct typedef (safety_link.h), so it
 * cannot be forward-declared the way kiln_io_t/MAX31856BusClass are in that
 * same header -- #include it directly rather than fight the typedef. */
#include "safety_link.h"

/* Brings up kiln_nvs (harmless no-op if another module already has, same
 * convention as zones_http_start()/kiln_cfg_store_init()) and loads the
 * persisted cache. Three-outcome version load, same discipline
 * zones_http.c's nvs_load_from() documents:
 *   - stored version == current: load as-is.
 *   - stored version < current: no migration chain exists yet (this is the
 *     first version this cache has ever had, mirroring kiln_cfg_store.c's own
 *     "only version 1 exists today" state) -- unreachable until a future
 *     bump adds one.
 *   - stored version > current: REFUSE to load (this build is older than
 *     whatever wrote the blob -- a firmware-rollback case, same as
 *     zones_http.c/kiln_cfg_store.c) -- flash left untouched, this boot runs
 *     with an empty cache (every param reports set=false) rather than
 *     risking a byte-offset reinterpreted as a wrong threshold
 *     (COMMISSIONING.md sec 1's own reasoning for the identical rule on the
 *     Pico's own config_store).
 * Always returns ESP_OK once kiln_nvs itself is usable (matches
 * kiln_cfg_store_init()'s own contract) -- an unusable/corrupt/newer-than-
 * firmware blob is a fall-back-to-empty-cache case, not a startup failure:
 * every param simply reads set=false (equivalent to "never fetched yet")
 * until the next successful refetch. Returns the underlying NVS partition
 * error only if kiln_nvs itself could not be brought up at all. */
esp_err_t safety_cfg_store_init(void);

/* The live config_crc this cache's contents were fetched at, or 0 if nothing
 * has ever been fetched successfully (0 is never a real CRC -- CRC16/CCITT-
 * FALSE's all-zero-input result is a different value -- so 0 doubles as
 * "never fetched" the same way KILN_CFG_NO_ACTIVE_ID doubles as "no active
 * config" elsewhere in this codebase). Compare against safety_link's live
 * peer_config_crc (safety_link_get_peer_build_status()) to compute
 * COMMISSIONING.md sec 3.1's `stale` flag -- this module never computes that
 * comparison itself, since it has no reference to what "live" currently is. */
uint16_t safety_cfg_store_cached_crc(void);

/* Milliseconds since the last successful LIVE refetch (safety_cfg_store_
 * refetch(), an actual round trip to the Pico), or UINT32_MAX if nothing has
 * been fetched THIS BOOT. 2026-08-27 audit fix (defect c): loading a cache
 * from NVS at boot does NOT count as "just fetched" -- it never did anything
 * live, and stamping the clock for it used to make this function report
 * board uptime instead of fetch age, claiming "fetched 10 min ago" for a
 * value that could be a stale image days old off a Pico that has since been
 * reflashed. A board with a real NVS-loaded cache and no refetch yet this
 * boot correctly reads UINT32_MAX ("never", -> the API's `"fetched_ms_ago":
 * null`) even though safety_cfg_store_get_by_index() is already serving real
 * values for it -- "the values are known" and "how stale are they" are
 * deliberately answered by two different calls, and only the second one may
 * ever be null. */
uint32_t safety_cfg_store_fetched_ms_ago(void);

/* How many known parameters this cache tracks -- always SAFETY_CFG_PARAM_COUNT
 * today; exposed as a function (matching kiln_cfg_store_max_count()'s own
 * shape) so a caller iterating 0..count never hard-codes the constant twice. */
size_t safety_cfg_store_param_count(void);

/* Fills *out with param table row `index` (0 <= index < safety_cfg_store_
 * param_count()) plus whatever this cache currently holds for it. Returns
 * false (out untouched) for an out-of-range index or a NULL out. Table order
 * is fixed and matches COMMISSIONING.md sec 2.1's own ordering (sec 1 ids
 * first, then sec 2, 3, 4, 5) -- callers that want a specific field by id
 * should use safety_cfg_store_lookup() instead. */
bool safety_cfg_store_get_by_index(size_t index, safety_cfg_param_t *out);

/* Looks up param_id's type/name without touching this cache's stored value --
 * used by safety_cfg_http.c's POST handler to learn how to parse an
 * operator-submitted value BEFORE calling safety_link_send_set_param(), and
 * to refuse an id this build does not recognise (COMMISSIONING.md sec 2:
 * "version-tolerant... unknown ids are refused individually and named in the
 * reply"). Returns false (out_type/out_name untouched) for an unknown id. */
bool safety_cfg_store_lookup(uint16_t param_id, uint8_t *out_type, const char **out_name);

/* Live, blocking round trip: pages through safety_link_get_config_page()
 * (page 0, 1, 2, ... until a reply reports `more == 0`) and replaces this
 * cache's stored values with whatever came back, keyed by param_id -- an id
 * this build's table does not recognise (a newer Pico reporting a field this
 * ESP predates) is silently skipped, per COMMISSIONING.md sec 2's version-
 * tolerance rule; a param_id this table DOES recognise but that the Pico
 * never sent a page entry for is left as `set = false` (COMMISSIONING.md
 * sec 1: the four no-default fields read this way until actually
 * commissioned). On success, updates cached_config_crc to `config_crc` and
 * persists the whole cache to kiln_nvs. Returns false (cache left
 * untouched) if any page request fails (timeout, decode error) -- an
 * interrupted refetch must not leave the cache holding a MIX of old and new
 * pages with no way to tell which is which, so nothing is applied until
 * every page has been read successfully. `link` may not be NULL.
 *
 * SAFE TO CALL FROM MORE THAN ONE TASK (2026-08-27 audit fix, H5): this used
 * to be called only from safety_poll_task (via safety_cfg_store_maybe_
 * refetch()); safety_cfg_http.c's commissioning POST handler now also calls
 * it directly, from the httpd worker task, to force a live read-back after a
 * commit. This function takes an internal mutex around its whole body, so
 * two concurrent callers serialize rather than racing s_store/s_dirty/the
 * flash-worker flush -- see safety_cfg_store.c's s_store_lock/s_dirty
 * comments for the full story. A caller on the httpd worker should expect to
 * occasionally block here for as long as a concurrent safety_poll_task
 * refetch takes (bounded by SAFETY_CFG_STORE_REFETCH_BUDGET_MS). */
bool safety_cfg_store_refetch(SafetyLinkClass *link, uint16_t config_crc);

/* The fetch-on-change trigger (COMMISSIONING.md sec 3, this file's own top
 * comment): if `live_config_crc` differs from safety_cfg_store_cached_crc(),
 * calls safety_cfg_store_refetch() and returns whatever it returned;
 * otherwise does nothing (no UART traffic at all) and returns false --
 * *THIS* is what makes "a steady-state reboot of either processor transfers
 * nothing" true: the only way this function ever talks to the Pico is a CRC
 * mismatch. Intended caller: safety_link.c's poll task, right after it
 * applies a fresh FW_VERSION frame (see this header's top comment). */
bool safety_cfg_store_maybe_refetch(SafetyLinkClass *link, uint16_t live_config_crc);

/* 2026-08-23 panic fix: the actual NVS write safety_cfg_store_refetch() used
 * to make directly (nvs_save_store()) now only ever runs via this function,
 * which hands it to uart_bridge_ext.c's internal-SRAM-stack flash-safe
 * worker (uart_bridge_ext_run_on_flash_worker()) instead of executing it on
 * whatever task calls in -- safety_poll_task's own stack is PSRAM
 * (safety_link.c:1636-1638), and a flash/NVS write from a PSRAM-stacked task
 * aborts the whole board (ESP-IDF's esp_task_stack_is_sane_cache_disabled(),
 * not a constraint of this driver -- see safety_cfg_store.c's
 * caller_stack_is_external() comment). "Mark dirty, flush later on a safe
 * task": safety_cfg_store_refetch() marks the in-RAM cache dirty right after
 * updating it, then calls this. No-op (returns ESP_OK) if nothing is dirty.
 * Exported so a future caller on any task can trigger a flush without ever
 * risking a direct nvs_save_store() call of its own. */
esp_err_t safety_cfg_store_flush_if_dirty(void);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_CFG_STORE_H
