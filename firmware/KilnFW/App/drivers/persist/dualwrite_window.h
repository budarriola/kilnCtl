// dualwrite_window -- measures the owner-approved dual-write closure
// criterion for the cfg-filesystem migration (docs/FILESYSTEM_PLAN.md,
// "dual-write window"). The owner approved running BOTH the file-backed
// path and the legacy NVS path in parallel for a BOUNDED window, and wrote
// down the exit criterion:
//
//   20 consecutive clean boots with the filesystem as the primary path,
//   AND one complete firing run that ran file-backed,
//   AND one verified backup/restore round trip against the file path.
//
// Before this module, nothing measured any of the three, which meant the
// window could only ever be closed on vibes (a claim nobody could check) or
// never closed at all (nobody willing to make that unverifiable claim).
// This module makes the criterion checkable. It does NOT decide anything:
// see dualwrite_window_get_status()'s `window_may_close` field, which is a
// REPORT, never an action -- nothing in this codebase reads it back to
// decide whether to keep writing NVS. Dropping the NVS copies once the
// window closes is a deliberate, reviewed, owner-visible step performed by
// hand; automating that step is explicitly out of scope and must stay that
// way (see this header's "NOT AN AUTOMATION" note below and
// docs/FILESYSTEM_PLAN.md's "Dual-write window" section).
//
// WHY PLAIN NVS, NOT THE FILESYSTEM BEING EVALUATED
// -----------------------------------------------------------------------
// The whole point of this module is to answer "has the filesystem path
// proven itself yet?" -- so the measurement itself must not depend on the
// thing under evaluation. If this module's own counters lived in cfg_fs,
// a cfg_fs bug (the very thing the window exists to catch) could corrupt
// or lose the evidence needed to prove cfg_fs is NOT yet trustworthy --
// exactly backwards. Plain NVS (same "kiln_cfg" namespace / "kiln_nvs"
// partition as run_state.c/crash_report.c, own key) is the established,
// already-proven persistence path in this codebase, so it is the right
// place to keep score of a challenger.
//
// "RESET ONE SIDE OF A PAIR" -- READ BEFORE CHANGING THIS FILE
// -----------------------------------------------------------------------
// project memory documents four confirmed incidents of exactly this bug
// class: a counter or seed reset on one side of a producer/consumer pair
// while something derived from it, elsewhere, was not. This module's
// counter (consecutive_clean_boots) has exactly ONE thing derived from it:
// dualwrite_window_get_status()'s `window_may_close` field. That field is
// NEVER stored -- it is recomputed from the three source values (clean-boot
// count, firing_complete, restore_verified) on every single call to
// dualwrite_window_get_status(), read directly out of the same record this
// module's own persist_locked()/load_locked() already keep as the single
// source of truth. There is no second copy of "may it close" anywhere to
// fall out of sync with the counter that feeds it -- the reset-one-side bug
// class needs two copies to desync; this design keeps exactly one.
// (firing_complete and restore_verified are independent one-time
// achievements, not derived from the boot counter, so they are NOT reset by
// an unclean boot -- an unclean boot after a real firing already ran
// file-backed does not erase the fact that the firing happened.)
#ifndef DUALWRITE_WINDOW_H
#define DUALWRITE_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "hal_sysinfo.h" /* hal_reset_reason_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The exit criterion's boot-count target. 20, per the owner-approved
 * criterion this module exists to measure -- not tunable without going back
 * to the owner, so this is a #define documenting the number, not a runtime
 * setting. */
#define DUALWRITE_WINDOW_CLEAN_BOOTS_TARGET 20u

/* Bumped whenever dualwrite_window_record_t's layout changes. An older
 * record is discarded rather than migrated -- same reasoning as
 * run_state.h's RUN_STATE_RECORD_VERSION: mis-parsing an old layout would
 * print a confidently wrong answer about how close the window is to
 * closing, which is worse than restarting the count at zero. */
#define DUALWRITE_WINDOW_RECORD_VERSION 1u

/* Persisted verbatim as one fixed-size NVS blob. Explicit reserved bytes,
 * same discipline run_state_record_t/crash_report_record_t use, so adding a
 * field means bumping the version rather than silently changing the size
 * (a size change reads back as corrupt, per this module's loader). */
typedef struct {
    uint8_t  version;             /* DUALWRITE_WINDOW_RECORD_VERSION at write time */
    uint8_t  firing_complete;     /* 0/1 -- sticky once set; see header note above */
    uint8_t  restore_verified;    /* 0/1 -- sticky once set; see header note above */
    uint8_t  reserved0;
    uint32_t consecutive_clean_boots; /* reset to 0 on any unclean boot -- see .c for the
                                        * exact predicate (reset reason, unacknowledged
                                        * crash, cfg_fs mount state) */
    uint32_t reserved1[2];
} dualwrite_window_record_t;

/* The read-only, fully-derived view this module reports. `window_may_close`
 * is computed fresh every call from the three fields above it -- see this
 * header's "reset one side of a pair" note. Never persisted. */
typedef struct {
    uint32_t consecutive_clean_boots;
    uint32_t clean_boots_target;   /* DUALWRITE_WINDOW_CLEAN_BOOTS_TARGET, echoed back so a
                                     * caller never has to hardcode the criterion itself */
    bool     firing_complete;
    bool     restore_verified;
    bool     window_may_close;     /* REPORT ONLY -- see this header's top comment.
                                     * Nothing in this codebase may read this field and
                                     * decide to stop writing NVS on its own. */
} dualwrite_window_status_t;

/* ---------------------------------------------------------------------
 * Pure logic -- no NVS I/O, no ESP-IDF calls beyond the hal_reset_reason_t
 * type. Exercised directly by host tests (this module's .c is #included by
 * its test file, same convention as crash_report.c/test_crash_report.c) so
 * the reset-vs-increment decision can be negative-tested without any flash
 * behind it.
 * ------------------------------------------------------------------- */

/* True iff a boot counts as "clean" for the window's purposes: a boot the
 * operator (or an OTA) chose (POWERON/SW), no crash report sitting
 * unacknowledged from that boot's own reset, and the filesystem actually
 * mounted this boot (fs_available). Any other combination -- a panic, a
 * watchdog reset, brownout, or a boot where cfg_fs failed to mount even
 * though nothing crashed -- is NOT clean for this criterion, even though
 * some of those reasons (e.g. HAL_RESET_EXT, a deliberate button-press
 * reset) are perfectly benign for other purposes. This predicate answers
 * ONE question -- "does this boot count toward '20 consecutive boots with
 * the filesystem healthy and primary'" -- not "was this boot bad". */
bool dualwrite_window_boot_is_clean(hal_reset_reason_t reason, bool crash_pending, bool fs_available);

/* Applies one boot's clean/unclean verdict to `rec` in place: increments
 * consecutive_clean_boots on a clean boot, resets it to 0 otherwise. This
 * is the ONLY place consecutive_clean_boots is ever mutated -- see this
 * header's top comment for why nothing else may derive a second, separately
 * resettable copy of it. Does not touch firing_complete/restore_verified. */
void dualwrite_window_apply_boot(dualwrite_window_record_t *rec, bool clean);

/* Fills `out` from `rec`, computing window_may_close fresh (see header top
 * comment). Never fails -- `rec` is always a valid, in-range record by the
 * time this is called (load_locked() below never hands back anything else). */
void dualwrite_window_compute_status(const dualwrite_window_record_t *rec, dualwrite_window_status_t *out);

/* ---------------------------------------------------------------------
 * Stateful API -- NVS-backed, ESP-IDF/hal_kv underneath.
 * ------------------------------------------------------------------- */

/* Runs the once-per-boot check: reads the current reset reason
 * (hal_sysinfo_reset_reason()), whether an unacknowledged crash report
 * exists for THIS boot (crash_report_get()), and whether cfg_fs is mounted
 * (cfg_fs_is_available()), applies dualwrite_window_boot_is_clean() +
 * _apply_boot(), and persists the result. Safe to call more than once --
 * only the FIRST call in a boot does anything; later calls are no-ops. This
 * is deliberately NOT called from main_boot_early.c (that file's boot/mount
 * wiring is owned elsewhere as of this writing) -- see
 * docs/FILESYSTEM_PLAN.md for the current wiring and what still needs the
 * boot-mount owner's hand. Today it runs from dualwrite_window_http_start(),
 * i.e. once HTTP bring-up reaches it, which is after cfg_fs's mount attempt
 * and after crash_report_init() have both already run -- late enough that
 * this check sees their real, settled results, not a value from before they
 * ran. */
void dualwrite_window_boot_check(void);

/* Explicit hook for a MOUNT FAILURE THAT HAPPENS AFTER dualwrite_window_
 * boot_check() has already run and counted the boot clean (e.g. cfg_fs
 * unmounts or a write starts failing partway through a boot that looked
 * healthy at HTTP-bring-up time). Resets consecutive_clean_boots to 0 and
 * persists immediately, same as an unclean boot. NOT wired to any call site
 * by this module -- cfg_fs_mount.c is owned elsewhere as of this writing;
 * see this module's report / docs/FILESYSTEM_PLAN.md for the requested call
 * site. Idempotent to call more than once per boot. */
void dualwrite_window_note_mount_failure(void);

/* Marks "one complete firing run has executed with the filesystem live" --
 * sticky, never unset by this module. Call ONLY when the run reached a
 * genuine completion (PROFILE_EXEC_DONE) AND cfg_fs was the live primary
 * path for that run (cfg_fs_is_available() at the time of the call) --
 * both conditions are the caller's responsibility to check before calling;
 * this function does not re-verify them. Idempotent. */
void dualwrite_window_note_firing_complete(void);

/* Marks "a backup/restore round trip has been verified against the file
 * path" -- sticky, never unset by this module. This module does not perform
 * or verify the round trip itself (that lives in the backup-over-filesystem
 * code, owned elsewhere as of this writing) -- it only records that some
 * caller has attested the round trip succeeded. Idempotent. */
void dualwrite_window_note_restore_verified(void);

/* Loads the persisted record and computes the derived status (see
 * dualwrite_window_compute_status()). Returns true and fills *out on
 * success; returns true with a zeroed/default record (0 clean boots,
 * neither flag set) if nothing has been persisted yet or the stored blob is
 * unreadable/wrong-version -- same load-tolerant convention as
 * run_state.c/crash_report.c, because a corrupt breadcrumb here must cost
 * nothing worse than restarting the count, never a boot failure. Returns
 * false only for a NULL `out`. */
bool dualwrite_window_get_status(dualwrite_window_status_t *out);

#ifdef __cplusplus
}
#endif

#endif // DUALWRITE_WINDOW_H
