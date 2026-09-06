/* fake_kv.h -- host fake backend for hal_kv.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION_PLAN.md "Host fakes (Phase 2 specs)" -- fake_kv
 * (must): RAM namespace/key store, reset between tests; error injection
 * (wrong-type, corruption, no-space).
 *
 * Handle storage: hal_kv_handle_t opaque storage is stamped with a
 * {magic, slot} tag, same as fake_i2c/fake_spi -- the namespace/partition
 * bookkeeping does not fit HAL_KV_HANDLE_STORAGE_BYTES on its own terms and
 * is easier to keep out-of-line.
 *
 * Durability model (hal_kv.h: "Nothing is durable until hal_kv_commit()"):
 * every key slot carries a COMMITTED value (survives fake_kv_reset_partition
 * / a simulated reboot) and a PENDING value (visible to get_* on the same or
 * any other handle immediately, exactly like real NVS's in-RAM page cache,
 * but discarded by fake_kv_simulate_power_loss() instead of being merged
 * into the committed value). hal_kv_commit() merges pending into committed
 * for every key in that handle's partition and clears pending -- matching
 * nvs_commit() flushing the whole partition's page, not just one handle's
 * writes. fake_kv_has_uncommitted_writes() reports whether any pending
 * write is outstanding on a partition.
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
#define FAKE_KV_MAX_KEYS_PER_NS         4
#define FAKE_KV_MAX_KEY_LEN            16
#define FAKE_KV_MAX_NAME_LEN           16
#define FAKE_KV_MAX_VALUE_BYTES       256
#define FAKE_KV_MAX_HANDLES             8

/* Clears every partition/namespace/key, all handles, all injected faults,
 * and the write_safe_here() flag (back to true). Call between test cases. */
void fake_kv_reset_all(void);

bool fake_kv_handle_is_live(const hal_kv_handle_t *h);

/* True if `partition` (NULL means the default partition, matching
 * hal_kv_open's shape) has at least one key whose pending write has not
 * been merged in by hal_kv_commit(). Returns false for a partition never
 * initialized via hal_kv_init_partition(). */
bool fake_kv_has_uncommitted_writes(const char *partition);

/* Discards every pending (uncommitted) write across every partition,
 * without touching committed values -- models a reboot/power-loss between
 * set_* and commit(). Does not close handles (matching real NVS: handles
 * remain valid across a device reset, only their uncommitted writes are
 * gone). */
void fake_kv_simulate_power_loss(void);

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

/* Test-controllable override for hal_kv_write_safe_here() -- see
 * hal_kv.h's write-context safety contract. Defaults to true after
 * fake_kv_reset_all() (the host has no PSRAM-stack concept of its own). */
void fake_kv_set_write_safe_here(bool safe);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_KV_H */
