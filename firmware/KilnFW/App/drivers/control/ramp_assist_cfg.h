// ramp_assist_cfg -- the single, KILN-WIDE (not per-zone) persisted on/off
// switch for the forthcoming "ramp assist" feature: during a real firing, if
// the kiln cannot keep up with a commanded ramp rate, ramp assist will (1)
// warn the user, (2) automatically stretch the schedule so every target
// temperature is still reached, and (3) reduce dwell time by a heat-work-
// weighted credit for time already spent within half a cone step of the
// target. A target above the kiln's permitted maximum stays a hard refusal
// either way -- ramp assist never stretches past that ceiling.
//
// THIS MODULE IS THE FLAG ONLY. It owns the persisted bool, its HTTP surface,
// and its MCP/tooling reachability -- it does NOT implement any ramp-
// stretching or dwell-credit behaviour. That lands separately in
// profile_executor.c (or wherever the executor's ramp/dwell math lives) and
// will read ramp_assist_cfg_enabled() at the point it decides whether to
// apply the stretch/credit; nothing here computes a stretched ramp or a
// credited dwell.
//
// WHY THIS TOGGLE IS LOAD-BEARING (the primary hazard this module exists to
// prevent): PID tuning runs and A/B controller comparisons on this project
// measure tracking error against a KNOWN, COMMANDED ramp/dwell shape. If ramp
// assist is enabled and silently stretches a ramp or shortens a dwell mid-run,
// the schedule the tuning pass THINKS it is running no longer matches the one
// that actually ran -- every IAE/overshoot/settle-time number computed from
// that run is comparing against the wrong target and becomes invalid, and two
// runs with the flag in different states are not an A/B comparison of the
// controller at all, only of whether ramp assist fired. This is why:
//   - the persisted DEFAULT is FALSE (off) -- a board that has never heard of
//     this setting, or whose stored value failed to load, always keeps today's
//     raw, unassisted behaviour, never opts a tuning run into assistance by
//     surprise;
//   - tools/PcTools' run-queue presets (config_presets/) PIN this flag
//     explicitly rather than leaving it to inherit whatever the board happens
//     to have -- see PIN_RAMP_ASSIST_KEY in the preset-apply code -- so an
//     automated experiment can never be silently invalidated by a value left
//     over from a previous session (an operator toggling it from the
//     diagnostics page, another experiment, a firmware default change, etc).
//
// PERSISTENCE. Same single-scalar-in-the-existing-namespace pattern
// unit_pref.c already established for a small, versionless, kiln-wide
// preference (see that module's header comment for why this shape, rather
// than kiln_cfg_store.c's versioned/migrated blob, is the right one for a
// single flag with no per-entry snapshot semantics): kiln_nvs partition,
// "kiln_cfg" namespace (shared with zones_http.c/unit_pref.c/profiles_
// builtin.c/kiln_cfg_store.c and friends -- distinguished by KEY, not
// namespace, same convention every one of those files documents). A missing
// key (fresh board, or a board that predates this feature) and a corrupt/
// out-of-range stored byte both fall back to the SAFE default (disabled),
// never the other way -- see ramp_assist_cfg.c's ramp_assist_cfg_start() for
// exactly how that is enforced.
#ifndef RAMP_ASSIST_CFG_H
#define RAMP_ASSIST_CFG_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The one warning sentence shown wherever a human is offered this toggle
// (diagnostics_page.html today) -- defined once, here, same
// "single source of truth the JS copy is checked against by App/test/
// lint_pages.js" convention as watchdog_cfg.h's WATCHDOG_CFG_FIRING_WARNING
// and ota_interlock.h's OTA_INTERLOCK_NO_SAFETY_WARNING. States the testing
// hazard (the reason this flag exists at all), not just the mechanism.
#define RAMP_ASSIST_CFG_PID_TEST_WARNING                                                          \
    "Ramp assist is for real firings, not PID tuning or A/B controller comparisons. When "         \
    "enabled, the executor may automatically stretch ramps and shorten dwells to keep up with "    \
    "the kiln's real capability -- a tuning or comparison run needs the RAW, unassisted "          \
    "schedule to produce comparable tracking-error numbers. Leave this OFF for tuning."

// Loads the persisted flag (or defaults to DISABLED if nothing valid is
// stored -- see this header's PERSISTENCE note). Non-fatal: a load failure
// leaves the in-RAM value at its safe default and never fails app_main, same
// convention as unit_pref_start()/watchdog_cfg_init(). Safe to call more than
// once; idempotent after the first call in a boot.
esp_err_t ramp_assist_cfg_start(void);

// Current in-RAM value. O(1), no NVS access -- the ramp-stretch/dwell-credit
// consumer (not yet written) and every status/diagnostics reader call this
// directly rather than re-reading flash.
bool ramp_assist_cfg_enabled(void);

// Persists `enabled` to NVS and applies it live immediately (in-RAM value
// updated before the NVS write is even attempted, same "in-RAM truth first"
// discipline unit_pref_set()/watchdog_cfg_set_panic_disabled() use) --
// returns the NVS write's esp_err_t; the live value takes effect for the rest
// of this boot regardless of whether persistence itself succeeded.
esp_err_t ramp_assist_cfg_set_enabled(bool enabled);

// Read-only dual-write status for GET /api/cfgfs -- see unit_pref.h's
// identical unit_pref_get_dualwrite_status() for the full contract (fresh
// re-read of both sides every call, no side effects, `diverged` computed
// via cfg_fs_status_item_diverged() from a real content compare).
void ramp_assist_cfg_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                           bool *diverged);

#ifdef __cplusplus
}
#endif

#endif // RAMP_ASSIST_CFG_H
