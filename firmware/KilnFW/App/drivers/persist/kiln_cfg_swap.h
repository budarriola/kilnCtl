// kiln_cfg_swap -- the two-processor apply transaction (docs/KILN_PROFILES_
// PLAN.md item 5, docs/audits/kiln_profiles_robustness_2026-09-14.md,
// docs/audits/kiln_swap_transaction_2026-09-14.md).
//
// Owner's rule, verbatim, twice: "It must swap out everything ... without
// human error" and "If a config doesn't land and match on both sides then
// alarm and dissable heaters." This module is the ONLY place that swaps a
// kiln_cfg_store slot's package onto BOTH processors as one transaction; it
// is deliberately separate from kiln_cfg_store.c (which still owns the
// ESP-only apply, kiln_cfg_store_apply(), used by every ordinary caller) so
// this file's own crash-recovery state machine and its tests don't bloat
// that already-large module.
//
// ORDERING (section 4.2, as revised by section 1a -- no disarm, no re-arm,
// the Pico never leaves ARMED):
//   0. interlock (ota_http_check_interlocks(), same predicate kiln_cfg_
//      store_apply() itself is built on) -- refuse outright, nothing written.
//   1. H17: refuse a half-package (pico_populated==0) outright.
//   2. snapshot the CURRENT live state (R) and persist it into the pending-
//      swap record, marker=STAGED. This is the crash-recovery anchor.
//   4. if P's ceiling is >= the Pico's CURRENT ceiling: raise it now, before
//      touching anything else, via safety_cfg_write_set_and_confirm_f32_
//      volatile() (item 15, SAFETY_CMD_APPLY_CONFIG_VOLATILE/0x2D -- NEVER
//      the flash-writing safety_cfg_write_set_and_confirm_f32() the standing
//      ceiling-reconcile loop still uses; see docs/audits/kiln_swap_
//      volatile_wiring_2026-09-14.md for why).
//   5. marker=PICO_OPEN, persist.
//   6. push every OTHER Pico param, also volatile (safety_cfg_write_apply_
//      package_and_confirm(..., volatile_install=true, ...)) -- the Pico
//      never leaves ARMED for the whole transaction, forward or rollback.
//   7. read back (that same call's confirm-by-readback) and compare field-
//      by-field against P. Mismatch -> ROLLBACK. Success -> marker=PICO_DONE.
//   8. commit the ESP half (zones_config_import_blob(), already all-or-
//      nothing). Failure -> ROLLBACK (Pico already on P's non-ceiling
//      fields; ESP never committed -- see rollback's own comment for why
//      this is still safe). Success -> marker=ESP_DONE.
//   9. read back the ESP half (zones_config_export_blob()) and compare
//      byte-for-byte against P's blob (both are the SAME canonical export,
//      so an exact compare is available and used -- stronger than the
//      plan's minimum "field-by-field", never hash-only). Mismatch ->
//      ROLLBACK.
//  10. reconcile/assert the ceiling identity by calling safety_ceiling_
//      sync_reconcile_on_link_up() (reuses the EXISTING divergence
//      primitive and its already-wired disable-heat hooks -- this module
//      adds no second detector) and checking safety_ceiling_sync_is_
//      diverged() afterward.
//  11. if diverged (ceiling identity, or the Pico is not reporting a
//      configured/known ceiling -- item 16's UNCONFIGURED flag is not
//      landed as of this module; see the .c file's own note on the
//      approximation used in its place): latch, heaters already disabled by
//      the hooks safety_ceiling_sync installed at bring-up, do NOT roll
//      back (both halves already agree on CONTENT; only the ceiling
//      cross-check failed) and do NOT finalize active_id -- leave the
//      pending record at ESP_DONE so a reboot's boot-recovery retries the
//      SAME "verify then finish" path (section 4.4's ESP_DONE row).
//  12. finalize: kiln_cfg_store_set_active_id_raw(P.id), clear the pending
//      record (marker=NONE).
//  13. best-effort Pico flash-fallback persist (persist_pico_flash_
//      fallback(), .c file) -- item 15's "persist what is now proven live"
//      case-1 write: re-pushes the SAME just-verified fields through the
//      flash-writing COMMIT_CONFIG path so they survive a Pico reboot too.
//      ALLOWED TO FAIL (an ARMED refusal here is ordinary and expected,
//      logged not alarmed) -- the swap's own result was already decided at
//      step 12, before this ever runs. A Pico reboot between a successful
//      step 6/7 volatile install and this step landing leaves the Pico on
//      its OLD flashed config while the ESP believes the swap succeeded;
//      this is a real divergence, and it is caught by the SAME standing
//      ceiling/arming check step 10/11 already uses (safety_ceiling_sync's
//      reconcile-on-link-up, which reacts to the reboot independently of
//      this module) -- see docs/audits/kiln_swap_volatile_wiring_2026-09-
//      14.md for the coverage argument and its one named caveat (a swap
//      whose target ceiling is IDENTICAL to the pre-swap ceiling has no
//      ceiling-side signal of a lost non-ceiling field).
//
// LOCKING (H6): kiln_cfg_store_lock()/_unlock() bracket ONLY the snapshot-R
// step (2) and the finalize/rollback-commit steps (8/9/12 and their
// rollback mirrors) -- i.e. exactly the steps that read or write
// kiln_cfg_store's own `s_store`. They are NEVER held across step 6/7's
// Pico round trip or step 10's reconcile call, both of which can block for
// hundreds of milliseconds on the UART link -- see kiln_cfg_store.h's own
// doc comment on kiln_cfg_store_lock() for the standing rule this follows
// (project_flash_worker_reentrancy's sibling hazard,
// project_screen_idle_brick_real_cause's cautionary precedent). A
// generation check (kiln_cfg_store_generation()) immediately before step 12
// catches an ordinary save/clone/rename/delete that landed on the store
// during the unlocked window; a change is treated as "someone else acted
// while a swap was in flight" and forces a ROLLBACK rather than blindly
// overwriting whatever is there now.
//
// TASK PLACEMENT: this function performs a 60+ round-trip UART exchange
// (68 params) plus at least one full flash write (zones_config_import_
// blob()) and must NOT run on the httpd worker (a watchdog hazard the plan
// calls out explicitly, section 8 item 5's acceptance criteria) -- callers
// dispatch kiln_cfg_swap_apply() onto a dedicated worker task (NOT the
// flash worker -- project_flash_worker_reentrancy: dispatching to the flash
// worker from a caller already on it deadlocks the board, and zones_
// config_import_blob() itself dispatches its own NVS write to the flash
// worker internally). Wiring that dispatch is integration work left to the
// HTTP/LCD call site (docs/audits/kiln_swap_transaction_2026-09-14.md names
// this explicitly as not yet done); this module's own entry point is
// synchronous and blocking by design so it is trivially unit-testable and
// trivially wrappable in whatever task-dispatch primitive the call site
// already uses (same shape as kiln_cfg_store_apply() itself, which every
// existing httpd handler already wraps in its own interlock pre-check
// before calling).
#ifndef KILN_CFG_SWAP_H
#define KILN_CFG_SWAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "kiln_package.h" /* kiln_pkg_safety_t */
#include "safety_link.h" /* SafetyLinkClass */
#include "zones_config_accessors.h" /* ZONES_CONFIG_BLOB_MAX_SIZE */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KILN_CFG_SWAP_MARKER_NONE = 0,
    KILN_CFG_SWAP_MARKER_STAGED = 1,
    KILN_CFG_SWAP_MARKER_PICO_OPEN = 2,
    KILN_CFG_SWAP_MARKER_PICO_DONE = 3,
    KILN_CFG_SWAP_MARKER_ESP_DONE = 4,
} kiln_cfg_swap_marker_t;

/* The persisted pending-swap record -- section 4.2 step 2's crash-recovery
 * anchor. `crc32` is computed over every OTHER field (H10: the record must
 * detect its own corruption rather than a corrupt marker being silently
 * read as NONE or as some other state -- see kiln_cfg_swap_boot_recover()'s
 * own doc comment for what happens when this check fails). Persisted as one
 * opaque blob in kiln_nvs, a SEPARATE key from kiln_cfg_store's own blob
 * key -- this record's own corruption must never be confused with, or
 * capable of corrupting, the slot store itself. */
typedef struct {
    uint32_t crc32;
    uint8_t marker; /* kiln_cfg_swap_marker_t */
    int32_t target_id;
    int32_t previous_active_id;
    uint16_t rollback_blob_len;
    uint8_t rollback_blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    kiln_pkg_safety_t rollback_pico;
} kiln_cfg_swap_pending_t;

/* Reason buffer convention matches config_divergence.h's own
 * CONFIG_DIVERGENCE_REASON_MAX -- sized against the longest message this
 * module or anything it calls can produce, not the 96-byte ki_refusal_
 * reason mistake this codebase already has a name for. */
#define KILN_CFG_SWAP_REASON_MAX 200

/* Brings up whatever this module needs (today: nothing beyond kiln_nvs,
 * already brought up by kiln_cfg_store_init(); call this AFTER that, same
 * ordering plan section 4.4 specifies) and performs the interrupted-swap
 * boot recovery (section 4.4 / plan item 8's five-case table). Always
 * leaves the pending record in a state where heating is impossible until a
 * divergence check has actually confirmed the two sides agree -- never
 * "finishes" a swap by assuming the operator's intent, per section 4.4's
 * own explicit rule for the PICO_DONE case. Safe to call even if no swap
 * was ever attempted on this board (marker reads NONE, this is a no-op). */
void kiln_cfg_swap_boot_recover(void);

/* Installs the SafetyLinkClass instance this module talks to -- same
 * "module holds a pointer handed to it once at bring-up" convention as
 * safety_cfg_http_start()/safety_ceiling_sync's own callers (there is no
 * global safety-link getter in this codebase; every module that needs one
 * is handed it explicitly). Call once, after safety_link_start(), before
 * kiln_cfg_swap_boot_recover()/_apply() can do anything useful. `link_or_
 * null` may be NULL (no safety processor this boot) -- both functions then
 * refuse/no-op with a reason rather than dereferencing NULL. */
void kiln_cfg_swap_set_link(SafetyLinkClass *link_or_null);

/* Performs the whole transaction described in this header's own top
 * comment, synchronously (see the TASK PLACEMENT note above for why the
 * caller, not this function, owns dispatching it off the httpd worker).
 * `link` may be NULL (no safety processor this boot) -- refused immediately
 * with a reason, same convention as every Pico-touching call in this
 * codebase; a package can never be swapped onto a controller with no safety
 * processor link.
 *
 * `ack_no_safety_processor` is forwarded verbatim to ota_http_check_
 * interlocks() -- same meaning as kiln_cfg_store_apply()'s own parameter of
 * that name.
 *
 * Returns true only once BOTH halves are committed, read back, and verified
 * matching, AND the ceiling identity/arming check (step 10/11) also passed
 * -- i.e. only once section 4.2's step 12 has actually run. Every other
 * outcome (interlock refusal, half-package, Pico push/readback failure, ESP
 * commit/readback failure, an unrecoverable rollback, or a ceiling-identity
 * failure after both halves already agree on content) returns false with a
 * reason in `reason_out`; `out_diverged` (may be NULL) is set true only for
 * the last of those -- the one case where the swap is NOT rolled back and
 * heaters are alarmed-and-disabled rather than merely refused, so a caller
 * building an operator message can distinguish "your swap was refused,
 * nothing changed" from "your swap partly landed and the board is now
 * alarmed -- see the divergence banner." */
bool kiln_cfg_swap_apply(int32_t target_id, bool ack_no_safety_processor, char *reason_out, size_t reason_cap,
                         bool *out_diverged);

/* Read-only accessor for a status page / test to report the pending-swap
 * marker without reaching into this module's NVS key directly. Returns
 * KILN_CFG_SWAP_MARKER_NONE (and target_id/previous_active_id left at -1)
 * if the persisted record is absent OR fails its own CRC check (H10) --
 * see kiln_cfg_swap_boot_recover()'s doc comment for why a CORRUPT record
 * is NOT reported identically to an absent one at the LOGGING/alarm layer,
 * even though this read-only accessor collapses both to NONE for callers
 * that only want "is a swap in flight". */
kiln_cfg_swap_marker_t kiln_cfg_swap_get_marker(int32_t *out_target_id, int32_t *out_previous_active_id);

/* true iff a swap transaction has a pending record (marker != NONE) --
 * a plain wrapper over kiln_cfg_swap_get_marker(), meant to be registered
 * with kiln_cfg_store_set_swap_pending_source() (kiln_cfg_store.h) at
 * bring-up, matching the existing kiln_cfg_store_capture_expected_pico_
 * fields()/safety_ceiling_sync_set_expected_pico_fields_source() wiring
 * pattern in main_control_bringup.c. */
bool kiln_cfg_swap_is_pending(void);

/* M13 ("every fault says what was detected and what to do", ROADMAP.md
 * standing rule): kiln_cfg_swap_boot_recover() (section 4.4's five-case
 * table) used to report every one of its "could not recover, staying
 * alarmed" outcomes via ESP_LOGE only -- kiln_cfg_swap_is_pending() exists
 * and is TRUE for the whole time the board sits in this state (that is
 * literally what "leave the pending record" means, in every failing branch
 * below), but its only caller (main_control_bringup.c) uses it purely to
 * gate autosave, never to tell an operator anything. A board could sit with
 * heaters alarmed/disabled from an interrupted two-processor config swap
 * indefinitely with nothing on the dashboard or LCD naming why, or what to
 * do about it -- discoverable only by an ESP_LOGE line in a serial log an
 * operator is not watching.
 *
 * Latched once, the first time kiln_cfg_swap_boot_recover() reaches a
 * branch that leaves the pending record in place (i.e. does NOT reach
 * clear_pending()) -- covers: an unreadable/corrupt pending record (H10),
 * no safety-link available to recover a PICO_OPEN/PICO_DONE interruption,
 * that recovery's own rollback attempt failing, an ESP_DONE row whose
 * post-boot re-verification could not confirm both sides match (including
 * its own fallback-rollback failing), and an unrecognised marker byte.
 * Never latched by the STAGED case (a clean, harmless discard: nothing was
 * ever written to either processor) or by a recovery that succeeds. Not
 * cleared mid-boot, same "fixed for the boot" discipline as zones_cfg_load_
 * fault_t (zones_config_accessors.h) -- a fresh boot that recovers cleanly,
 * or that finds no pending record at all, re-evaluates to false. */
typedef enum {
    KILN_CFG_SWAP_BOOT_FAULT_NONE = 0,
    KILN_CFG_SWAP_BOOT_FAULT_UNREADABLE,           /* record present but unreadable or failed its CRC (H10) */
    KILN_CFG_SWAP_BOOT_FAULT_NO_LINK,              /* PICO_OPEN/PICO_DONE recovery needs a safety link that is not up */
    KILN_CFG_SWAP_BOOT_FAULT_ROLLBACK_FAILED,      /* the rollback-to-R attempt itself failed */
    KILN_CFG_SWAP_BOOT_FAULT_ESP_DONE_UNCONFIRMED, /* ESP_DONE row: could not re-confirm both sides match */
    KILN_CFG_SWAP_BOOT_FAULT_UNRECOGNISED_MARKER,  /* pending record's marker byte is not a known value */
} kiln_cfg_swap_boot_fault_kind_t;

typedef struct {
    bool occurred;
    kiln_cfg_swap_boot_fault_kind_t kind;
    int32_t target_id; /* the pending record's own target_id at the moment this latched */
    char reason[KILN_CFG_SWAP_REASON_MAX]; /* human-readable detail + what to do, copied verbatim */
} kiln_cfg_swap_boot_fault_t;

/* Returns the latched boot-recovery fault (see the type's own comment
 * above). *out (if given) is zeroed with occurred==false when nothing was
 * latched this boot. Cheap RAM read, safe from any task. */
bool kiln_cfg_swap_get_boot_fault(kiln_cfg_swap_boot_fault_t *out);

#ifdef __cplusplus
}
#endif

#endif // KILN_CFG_SWAP_H
