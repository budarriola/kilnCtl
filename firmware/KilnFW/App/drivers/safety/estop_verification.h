// estop_verification -- persisted record of whether an operator has
// performed the bench verification procedure for the E-stop interlock
// (firmware/SaftyFW/README.md's "E-stop bench verification" section).
//
// WHY THIS EXISTS: 3b5ced00 made the firmware side of E-stop test-locked
// (relay_owner drives the pin low on TRIP, negative-tested) and documented
// the wiring (README.md/HARDWARE.md 5.1): pole 1, in series with the
// external line contactor's coil, is wiring the OWNER adds -- firmware
// cannot see it, cannot test it, and cannot know it was ever connected
// correctly. The honest coverage for a fact firmware structurally cannot
// observe is a documented manual procedure plus a durable, DELIBERATE record
// that a human performed it -- never something inferred from a GPIO read,
// because the whole point is that no GPIO read can tell pole 1 from an open
// circuit that happens to also read healthy.
//
// SAME NVS PATTERN AS crash_report.c/run_state.c, DELIBERATELY SIMPLER: a
// tiny versioned record in the same "kiln_cfg" namespace on KILN_NVS_PARTITION.
// No per-field CRC the way crash_report.c has one -- that module protects
// fields written from a fault path where memory could be half-written;
// this one is written only from a single explicit HTTP POST with one
// meaningful bit, so a version check on load is enough: a version mismatch or
// missing key both read as "unverified," which is the fail-safe direction
// (an operator who never confirmed reads exactly the same as one whose
// record failed to load).
//
// WHAT MUST CLEAR THIS RECORD -- read before adding a new invalidation path:
//   1. A change to param 0x0212 (estop_active_level) -- ANY commit that
//      includes that param, whether or not the value actually differs from
//      before, since a re-send is itself grounds to distrust a stale
//      verification made against a run this ESP cannot prove was the same
//      polarity. Hooked in safety_cfg_http.c's apply_pairs(), the one place
//      on this ESP a SET_PARAM/COMMIT_CONFIG for 0x0212 can land.
//   2. A factory reset of the "kiln" or "all" scope (factory_reset.c) --
//      handled for free, not by an explicit call: this record lives in
//      KILN_NVS_PARTITION under the same namespace crash_report.c/run_state.c
//      use, and both of those scopes already erase that whole partition.
//   3. NOT invalidated by a reboot, an OTA, or any other config change on
//      this board -- the procedure verifies WIRING, which does not change on
//      its own. It also does NOT invalidate on a Pico-side factory
//      reset/reflash that leaves estop_active_level unchanged: this ESP has
//      no way to observe that event happened at all (no "config changed
//      out from under me" signal exists for it), so widening the trigger
//      list to include it would require inventing detection this codebase
//      does not have -- see readiness_http.c's own item for "safety
//      processor commissioned" for the same class of asymmetry (cached_crc
//      only tracks what THIS ESP fetched, never a peer-side event it never
//      saw). If the safety processor is ever reflashed or reset independent
//      of a polarity change reaching this ESP through apply_pairs(), the
//      operator is relied upon to re-run the bench procedure and re-confirm
//      by hand, same as any other out-of-band hardware change this codebase
//      cannot detect.
#ifndef KILNCTL_ESTOP_VERIFICATION_H
#define KILNCTL_ESTOP_VERIFICATION_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call once at boot (main_boot_early.c, alongside crash_report_init()/
// run_state_init()) so the NVS partition is up before any HTTP handler asks.
void estop_verification_init(void);

// True only after estop_verification_confirm() has been called and has not
// since been invalidated by estop_verification_clear(). Any load failure
// (never confirmed, corrupt record, version mismatch) reads false -- the
// fail-safe direction, per this header's own reasoning above.
bool estop_verification_is_verified(void);

// The deliberate operator action ("I have verified the E-stop interlock").
// Persists a verified record. Returns the underlying NVS error on failure --
// callers should treat any non-ESP_OK as "not actually recorded" and say so,
// not report success optimistically.
esp_err_t estop_verification_confirm(void);

// Invalidates a standing verification. Call this from exactly the two sites
// documented above (the 0x0212 commit hook in safety_cfg_http.c; the
// partition erase in factory_reset.c needs no call, see point 2 above) --
// NOT from anywhere else, since every additional call site is one more place
// that has to independently reason about whether it invalidates the physical
// wiring fact this record stands in for. A missing/never-written record is
// already "unverified," so this is idempotent and its own failure is
// logged but not fatal to the caller.
esp_err_t estop_verification_clear(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_ESTOP_VERIFICATION_H
