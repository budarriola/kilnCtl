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
// independent store (same governance class as KILN_CFG_STORE_VERSION,
// docs/CONFIG_MIGRATION_CHAIN_PLAN.md sec 0.1's per-store table), not part
// of the zones-config document. It owns ITER_TUNE_STORE_VERSION and is
// listed in that table as its own row -- it does not participate in
// ZONES_CFG_VERSION's migration chain at all.
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

// Matches ZONE_COUNT elsewhere in this tree (3 zones on this board). Kept as
// its own constant, not an #include of zones_config_accessors.h, so this
// persistence module stays free of the zones-config header chain -- the
// _Static_assert in iter_tune_store.c holds the two together.
#define ITER_TUNE_STORE_MAX_ZONES 3u

// Bumped only if the stored layout changes. A blob whose version this build
// does not recognize is treated exactly like a missing key (every zone
// reads as never-enabled), never partially trusted -- same rule
// ct_verify_store.c and display_power_cfg.c apply to their own blobs.
#define ITER_TUNE_STORE_VERSION 1u

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
    uint8_t reserved[3]; // keeps the floats below naturally aligned; always 0
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

// Test-only: resets in-RAM state to "nothing persisted" without touching
// NVS/cfg_fs, so host tests get a clean slate between cases without a real
// partition erase.
void iter_tune_store_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif // ITER_TUNE_STORE_H
