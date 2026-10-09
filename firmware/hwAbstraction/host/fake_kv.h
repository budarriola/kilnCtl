/* fake_kv.h -- host fake backend for hal_kv.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION.md "Host fakes (Phase 2 specs)" -- fake_kv
 * (must): RAM namespace/key store, reset between tests; error injection
 * (wrong-type, corruption, no-space).
 *
 * Handle storage: hal_kv_handle_t opaque storage is stamped with a
 * {magic, slot} tag, same as fake_i2c/fake_spi -- the namespace/partition
 * bookkeeping does not fit HAL_KV_HANDLE_STORAGE_BYTES on its own terms and
 * is easier to keep out-of-line.
 *
 * Durability model (hal_kv.h: a set MAY already be durable before commit;
 * commit guarantees durability of everything before it -- real NVS writes
 * to flash immediately and commit is close to a no-op on top of that): every
 * key slot carries a COMMITTED value (survives fake_kv_reset_partition / a
 * simulated reboot) and a PENDING value (visible to get_* on the same or any
 * other handle immediately, exactly like real NVS's in-RAM page cache).
 * hal_kv_commit() merges pending into committed for every key in that
 * handle's partition and clears pending -- matching nvs_commit() flushing
 * the whole partition's page, not just one handle's writes.
 * fake_kv_has_uncommitted_writes() reports whether any pending write is
 * outstanding on a partition.
 *
 * fake_kv_simulate_power_loss() DEFAULT behavior: pending writes are KEPT
 * (merged into committed, same as a real commit would do) -- this models
 * real NVS's immediate-write-to-flash behavior, where a set-then-crash
 * before an explicit hal_kv_commit() call very often still survives, since
 * the flash write already happened. A caller that wants the OLD
 * (pre-2026-09-05) lossy behavior -- pending writes discarded on power
 * loss, useful for over-approximating the worst case a caller must still
 * tolerate -- opts in with fake_kv_set_lossy_uncommitted(true). That mode is
 * explicitly labelled an OVER-approximation: real NVS does not actually
 * lose an uncommitted write this reliably, so a test passing only in lossy
 * mode is not proof of a bug, but a test that assumes pending writes are
 * ALWAYS lost (the old default) no longer matches either this fake or real
 * hardware.
 *
 * Type/error injection decisions (contract points hal_kv.h leaves to the
 * fake, since NVS's own byte-for-byte typing isn't part of the header):
 *  - get_blob accepts a key regardless of whether it was written via
 *    set_blob or set_str (both are just bytes on flash) -- this is the
 *    "blob size-probe" path kiln_cfg_store.c and touch_cal_store.c use on
 *    values that may have been written either way.
 *  - get_str requires the key was last written via set_str; calling it on
 *    a set_blob key returns HAL_INVALID_ARG (the plan's "wrong-type" error
 *    injection case) rather than silently reinterpreting the bytes.
 *  - fake_kv_script_corrupt_key() marks a COMMITTED value corrupted; any
 *    get_* on it (once no newer pending write shadows it) returns HAL_IO
 *    until overwritten by a fresh set_*+commit, modeling flash bit-rot
 *    independent of the wrong-type case above.
 *  - A handle opened HAL_KV_MODE_READ_ONLY returns HAL_INVALID_ARG from
 *    every write-shaped call (set_blob/set_str/erase_key) -- read calls are
 *    unaffected, matching hal_kv.h's "Read calls carry no such restriction"
 *    note (which is about the write-context stack contract, but the same
 *    shape applies to the read-only mode gate).
 */
#ifndef KILNCTL_FAKE_KV_H
#define KILNCTL_FAKE_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_kv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_KV_MAX_PARTITIONS         4
#define FAKE_KV_MAX_NAMESPACES_PER_PART 4
/* 4 -> 12 (2026-09-06 flash-safety review) -> 128 (2026-09-19, docs/
 * PROFILE_SLOTS_100.md task 6 widened PROFILES_MAX_COUNT 8 -> 100):
 * profiles_http.c's "kiln_cfg" namespace is the worst known caller. At
 * PROFILES_MAX_COUNT=8 it held 9 live keys at once ("prof_used" plus up to
 * 8 "profN" slot keys), which sat exactly at the then-cap of 4 -- any test
 * staging more than 4 profile slots at once would have hit find_key()'s
 * "table full" HAL_NO_MEM on the later ones rather than actually exercising
 * the capacity it thought it had. With PROFILES_MAX_COUNT now 100, that same
 * namespace's worst case is "prof_used" + up to 100 "profN" keys +
 * profiles_favorites.c's "prof_favusr"/"prof_favbi" = 103 keys live at once
 * -- the OLD 12-key cap would silently strand any host test that drives
 * profiles through this real fake (rather than test_profiles_http.c's own
 * nvs_stub) somewhere around profile #10, long before reaching 100. 128
 * gives headroom above that 103 rather than matching it exactly, same
 * "loose headroom, not a tight fit" discipline stubs/nvs.h's own blob-size
 * bumps used -- and see fake_kv.c's FAKE_KV_KNOWN_WORST_CASE_KILN_CFG_KEYS
 * _Static_assert, which pins this cap against that 103 number at compile
 * time so the two can't drift apart silently again. (This header
 * deliberately does NOT include a firmware app header like profiles_types.h
 * to derive 100/103 automatically -- fake_kv.h/.c are shared, portable HAL
 * fakes used by SaftyFW's host tests too, and pulling an ESP32-app-specific
 * header into them would be a worse dependency than hand-pinning the
 * number and asserting it.) */
#define FAKE_KV_MAX_KEYS_PER_NS         128
#define FAKE_KV_MAX_KEY_LEN            16
#define FAKE_KV_MAX_NAME_LEN           16
/* 256 -> 8192 (HW_ABSTRACTION.md Phase 3 item 3, the nvs.h -> hal_kv.h
 * migration) -> 20000 (docs/KILN_PROFILES_PLAN.md items 1/2/12: KILN_CFG_
 * MAX_COUNT 8 -> 10 plus each entry's new Pico-half package pushed
 * sizeof(kiln_cfg_store_blob_t) to ~17.1KB): kiln_cfg_store.c's host test
 * (test_kiln_cfg_store.c) round-trips a REAL full-size kiln_cfg_store_
 * blob_t through this fake to exercise nvs_load_store()'s migration path --
 * the same value stubs/nvs.h's single-slot blob store (s_stub_nvs_blob) was
 * independently bumped to over several growth spurts, for the identical
 * reason: a slot too small to hold them just makes that migration path
 * silently untestable again (every set_blob call returns HAL_INVALID_SIZE
 * instead of actually storing anything). 20000 gives headroom above the
 * measured ~17.1KB rather than fitting it exactly, same "loose headroom"
 * discipline this constant's own history already follows. */
#define FAKE_KV_MAX_VALUE_BYTES      20000
#define FAKE_KV_MAX_HANDLES             8

/* Clears every partition/namespace/key, all handles, all injected faults,
 * and the write_safe_here() flag (back to true). Call between test cases. */
void fake_kv_reset_all(void);

bool fake_kv_handle_is_live(const hal_kv_handle_t *h);

/* Total hal_kv_get_blob()/hal_kv_get_str() calls since the last reset,
 * regardless of outcome. Snapshot before/after a call under test to prove
 * a read happened (or did not) at a specific point -- see fake_kv.c's doc
 * comment on the counter itself for the motivating case. */
unsigned fake_kv_get_call_count(void);

/* True if `partition` (NULL means the default partition, matching
 * hal_kv_open's shape) has at least one key whose pending write has not
 * been merged in by hal_kv_commit(). Returns false for a partition never
 * initialized via hal_kv_init_partition(). */
bool fake_kv_has_uncommitted_writes(const char *partition);

/* Simulates a reboot/power-loss between set_* and commit(). Does not close
 * handles (matching real NVS: handles remain valid across a device reset).
 * DEFAULT: every pending write is KEPT (merged into committed, like a real
 * commit) -- see the durability-model comment above for why this, not
 * discard, is the accurate default given real NVS's immediate-write
 * behavior. Call fake_kv_set_lossy_uncommitted(true) first to instead
 * discard every pending write (the old, over-approximating behavior). */
void fake_kv_simulate_power_loss(void);

/* Selects fake_kv_simulate_power_loss()'s behavior: false (default) keeps
 * pending writes across a simulated power loss; true discards them instead.
 * The `true` mode is a deliberate OVER-approximation of real NVS (which
 * does not reliably lose an uncommitted write) -- label any test relying on
 * it accordingly. Reset to false by fake_kv_reset_all(). */
void fake_kv_set_lossy_uncommitted(bool lossy);

/* Marks the COMMITTED value at (partition, namespace, key) corrupted: every
 * get_blob/get_str on it (once no pending write shadows it) returns HAL_IO
 * until the key is overwritten by a fresh set_*() + commit(). No-op
 * (returns false) if the key has no committed value yet. */
bool fake_kv_script_corrupt_key(const char *partition, const char *namespace_name,
                                 const char *key);

/* Forces the NEXT hal_kv_set_blob/set_str/erase_key/commit call (on any
 * handle) to return `status` instead of performing the operation, then
 * reverts to normal behavior. Intended for HAL_NO_MEM / HAL_IO write-failure
 * injection per the plan's "error injection ... no-space" spec. */
void fake_kv_script_next_write_status(hal_status_t status);

/* Like fake_kv_script_next_write_status(), but lets the first `skip` write-class
 * calls (set_blob/set_str/erase_key/commit, any handle) succeed and fails the
 * one after, once. Lets a test fail a multi-write sequence at every point. */
void fake_kv_script_write_status_after(unsigned skip, hal_status_t status);

/* Forces the NEXT hal_kv_open() call for `namespace_name` specifically to
 * return `status` instead of its normal result, then reverts to normal
 * behavior; other namespaces' opens are unaffected. Returns false (no-op)
 * for a NULL or too-long namespace_name. Added for load_count_strict()'s
 * legacy-namespace-open-error path: a genuine non-HAL_NOT_FOUND open failure
 * (e.g. HAL_NOT_READY, HAL_IO) has no other test-triggerable path for a
 * fixed, short, hardcoded namespace name. */
bool fake_kv_script_next_open_status(const char *namespace_name, hal_status_t status);

/* Arms `count` subsequent hal_kv_erase_key() calls to report HAL_OK while
 * leaving the key exactly where it was -- a write that LIES about having
 * succeeded, rather than one that fails honestly (which is what
 * fake_kv_script_next_write_status() models). Added 2026-09-09: this is the
 * shape real hardware showed in docs/audits/boot_guard_recovery_loop_
 * 2026-09-08.md, where an NVS write returned HAL_OK on a board whose
 * persisted value never changed, and it is the ONLY way to exercise a
 * caller's read-back verification -- an honest failure exits on the return
 * code long before the read-back runs. Consumed one call at a time; cleared
 * by fake_kv_reset_all(). */
void fake_kv_script_silent_erase_noops(unsigned count);

/* Same lie as fake_kv_script_silent_erase_noops(), for the NEXT `count`
 * hal_kv_set_blob()/hal_kv_set_str()/hal_kv_set_u32()/hal_kv_set_u8() calls
 * instead of hal_kv_erase_key(): reports HAL_OK while staging nothing, so
 * the key's persisted value does not change even after a commit. Needed
 * separately from the erase-noop version because not every "clear to a
 * known value" caller erases first -- boot_guard.c's persist_count()
 * overwrites via hal_kv_set_blob() directly, so only this variant can model
 * a lying write on that path (see boot_guard_reset_counter()'s host tests).
 * Consumed one call at a time; cleared by fake_kv_reset_all(). */
void fake_kv_script_silent_set_noops(unsigned count);

/* hal_kv_get_u32/set_u32 (profiles_builtin.c's NVS_KEY_HIDDEN mask) are
 * modeled as a plain 4-byte blob under the same key-slot storage
 * hal_kv_get/set_blob use -- no separate scalar storage needed. */

/* Test-controllable override for hal_kv_write_safe_here() -- see
 * hal_kv.h's write-context safety contract. Defaults to true after
 * fake_kv_reset_all() (the host has no PSRAM-stack concept of its own). */
void fake_kv_set_write_safe_here(bool safe);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_KV_H */
