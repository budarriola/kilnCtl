// iter_tune_store -- persistence for the redesigned iterative-tuning
// decision core (docs/ITER_TUNE_REDESIGN_PLAN.md step 7).
//
// A NEW, INDEPENDENT NVS namespace ("iter_tune"), deliberately never the old
// module's "adap_tune" namespace -- the plan's step 7 row names this
// explicitly, and this repo has already been bitten once by a stale
// namespace surviving a redesign (zones_config_accessors.h's own note next
// to the OLD adaptive-tune-enable flag). Dual-written to the `cfg` LittleFS
// partition following the SAME shape kiln_cfg_store_cfg_fs.c/
// zones_config_cfg_fs.c use for their own whole-document stores: a 4-byte
// little-endian rev counter immediately followed by a byte-for-byte copy of
// the current-version blob, NVS authoritative, file wins only on a
// STRICTLY higher rev.
//
// THIS FILE HAS NO KNOWLEDGE OF THE LIVE DECISION CORE (iter_tune.c/.h) and
// never includes it -- it stores exactly the fields a future in-RAM
// iter_tune_zone_state_t needs to survive a reboot (enabled/anchor/
// baseline/status/stop_reason), as plain bytes, and nothing else. Per plan
// step 7 ("still proposes nothing on hardware"), nothing in this module
// calls zones_config_set_pid() or any other hardware setter -- that
// remains the HTTP layer's (iter_tune_http.c) job for exactly the one
// sanctioned action (the restore-commissioned-gains control route).
//
// OWN SCHEMA VERSION, NOT A ZONES_CFG_VERSION BUMP: this is a brand-new,
// independent store (same governance class as KILN_CFG_STORE_VERSION). It
// owns ITER_TUNE_STORE_VERSION and has its own row ("ESP iterative-tuning
// persistence") in docs/CONFIG_MIGRATION_CHAIN_PLAN.md sec 0.1's per-store
// table -- it does not participate in ZONES_CFG_VERSION's migration chain
// at all.
//
// PER-ZONE OPT-IN, DEFAULT OFF: a zeroed blob (version set, everything else
// zero) is a fully valid, fully-disabled starting state for every zone --
// same convention iter_tune_zone_state_t itself uses.
#ifndef ITER_TUNE_STORE_H
#define ITER_TUNE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Matches MAX31856_CHANNEL_COUNT elsewhere in this tree (3 zones on this
// board). Kept as its own constant, not an #include of
// zones_config_accessors.h, so this persistence module stays free of the
// zones-config header chain -- the _Static_assert in iter_tune_http.c (the
// one file that already includes both this header and
// zones_config_accessors.h) holds the two together.
#define ITER_TUNE_STORE_MAX_ZONES 3u

// Bumped only if the stored layout changes. A blob whose version this build
// does not recognize as either the CURRENT version or a known OLD version
// below is treated exactly like a missing key (every zone reads as
// never-enabled), never partially trusted -- same rule ct_verify_store.c and
// display_power_cfg.c apply to their own blobs. A version NEWER than
// ITER_TUNE_STORE_VERSION is additionally reported loudly (ESP_LOGE plus
// iter_tune_store_schema_refused(), surfaced by GET /api/iter_tune/status)
// rather than silently folded into the same "nothing persisted" bucket a
// truncated or corrupt blob gets -- a newer-than-known version means a
// downgrade happened, which is worth a boot-time banner, not silence.
//
// v1 -> v2 (2026-09-23, plan step 7 acceptance gap 2): BYTE-COMPATIBLE.
// v1 never used the third `reserved` byte (always zero, per this file's own
// convention for every reserved field in this tree); v2 names that byte
// `carry_count` and otherwise changes nothing. A v1 blob is therefore already
// valid v2 content -- no field shuffling is needed, only re-tagging the
// version byte -- see migrate_v1_to_current() in iter_tune_store.c. This is
// the smallest real instance of the general mechanism (read old layout,
// migrate forward, refuse newer-than-known loudly) the plan's acceptance
// criteria asked to see exercised; it is deliberately NOT a
// ZONES_CFG_VERSION bump (this store never participated in that chain, see
// this file's top-of-file comment and CONFIG_MIGRATION_CHAIN_PLAN.md sec
// 0.1), so it carries none of that chain's rollback hazard.
//
// The migration itself is IN-RAM ONLY on a bare load -- iter_tune_store_start()
// does not re-persist a migrated blob to NVS/cfg_fs by itself (step 7 review,
// 2026-09-23, finding 1). This is deliberate: re-tagging the on-disk copy to
// v2 before any real v2 writer exists would make a rollback to v1 firmware
// read the store as version-mismatched and treat every zone as
// never-enabled. On-disk bytes stay at v1 until iter_tune_store_set_zone()
// performs a real write (which always persists the current, in-RAM-migrated
// blob, so it lands on disk as v2 from then on) -- a v1 rollback with no
// intervening write is therefore fully lossless, and a v1 rollback losing a
// write that happened after that first v2 write is expected/unavoidable,
// same as any other store in this tree.
#define ITER_TUNE_STORE_VERSION_V1 1u
#define ITER_TUNE_STORE_VERSION 2u

// One zone's persisted tuning state. `status`/`stop_reason` hold
// iter_tune_status_t/iter_tune_stop_reason_t values (iter_tune.h) narrowed
// to a byte -- stored as plain integers so this header does not have to
// pull iter_tune.h's decision-core header into the persist layer.
typedef struct {
    uint8_t enabled;
    uint8_t has_anchor;
    uint8_t has_baseline;
    uint8_t status;
    uint8_t stop_reason;
    uint8_t carry_count; // v2+: unscored-trial carry count (plan sec 4, "at
                          // most 3 such carries"); always 0 in a v1 record
                          // (it was v1's always-zero reserved[0]) and 0 here
                          // until a future producer writes it -- no writer
                          // exists yet, same as every other field in this
                          // struct per this file's top-of-file comment.
    uint8_t reserved[2]; // keeps the floats below naturally aligned; always 0
    float anchor_kp;
    float anchor_ki;
    float anchor_kd;
    float baseline_kp;
    float baseline_ki;
    float baseline_kd;
} iter_tune_store_zone_t;

typedef struct {
    uint8_t version; // ITER_TUNE_STORE_VERSION
    uint8_t zone_count; // how many entries in zone[] are meaningful
    uint8_t reserved[2]; // always 0
    iter_tune_store_zone_t zone[ITER_TUNE_STORE_MAX_ZONES];
} iter_tune_store_blob_t;

// Pinned sizes/offsets (step 7 review, 2026-09-23, advisory finding 6) -- a
// silent size or layout change here would be exactly the kind of drift
// note_schema_verdict()'s exact-length check (iter_tune_store.c) depends on,
// and offsetof(carry_count)==5 is the byte v1's always-zero reserved[0]
// occupied, which migrate_v1_to_current()'s byte-compatibility claim rests
// on.
_Static_assert(sizeof(iter_tune_store_zone_t) == 32, "iter_tune_store_zone_t size must stay pinned");
_Static_assert(offsetof(iter_tune_store_zone_t, carry_count) == 5,
               "carry_count must stay at v1's old reserved[0] byte offset");
_Static_assert(sizeof(iter_tune_store_blob_t) == 100, "iter_tune_store_blob_t size must stay pinned");

// PURE. Wrong size, unknown version, zone_count out of range, or any
// out-of-range enum field -> false, i.e. treat as no stored state at all.
// Shaped as validate(bytes, len) so it reads the same way every other blob
// store in this tree validates (ct_verify_blob_validate(),
// zones_config_json_decode_blob()'s own bounds checks).
bool iter_tune_store_blob_validate(const void *bytes, size_t len);

// Loads the persisted store into RAM (NVS + cfg file, resolved the same way
// kiln_cfg_store_cfg_fs_resolve() picks a winner). Non-fatal: a missing
// key, a failed partition init, or a blob that fails validation all leave
// the in-RAM state at "no zones persisted", which every reader treats as
// "every zone disabled, never anchored" -- never as a false PASS/anchor.
// Safe to call more than once.
esp_err_t iter_tune_store_start(void);

// True (and fills *out when non-NULL) if zone_index has a validated,
// persisted entry. False for an out-of-range index or an index >=
// the persisted zone_count (never enabled since this board's last
// iter_tune_store wipe).
bool iter_tune_store_get_zone(uint8_t zone_index, iter_tune_store_zone_t *out);

// Replaces zone_index's persisted entry wholesale and persists the WHOLE
// document (NVS + cfg file, rev bumped). Returns ESP_ERR_INVALID_ARG for an
// out-of-range zone_index. In-RAM truth updates first, so a failed NVS
// write means the entry will not survive a reboot, not that it failed to
// take effect now (same contract as ct_verify_store_save()).
//
// MUST NOT be called from a PSRAM-stacked task: the underlying NVS write
// refuses (and panics on real hardware) -- see hal_kv.h's write-context
// contract and safety_cfg_store.c's caller_stack_is_external() note.
esp_err_t iter_tune_store_set_zone(uint8_t zone_index, const iter_tune_store_zone_t *in);

// True if the most recent iter_tune_store_start() saw a blob (NVS or cfg
// file) whose version field was NEWER than ITER_TUNE_STORE_VERSION -- e.g.
// this build was downgraded after a newer build wrote the store. That blob
// is never trusted (every zone reads as never-enabled, same as any other
// invalid blob), but unlike a truncated/corrupt blob this case is reported:
// fills *out_version (when non-NULL) with the rejected version number.
// GET /api/iter_tune/status surfaces this so a downgrade-onto-newer-data
// situation is visible instead of silently indistinguishable from "never
// configured". Cleared by iter_tune_store_start()/iter_tune_store_reset_for_test().
bool iter_tune_store_schema_refused(uint8_t *out_version);

// Test-only: resets in-RAM state to "nothing persisted" without touching
// NVS/cfg_fs, so host tests get a clean slate between cases without a real
// partition erase.
void iter_tune_store_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif // ITER_TUNE_STORE_H
