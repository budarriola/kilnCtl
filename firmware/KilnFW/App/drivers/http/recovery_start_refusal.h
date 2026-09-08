#ifndef RECOVERY_START_REFUSAL_H
#define RECOVERY_START_REFUSAL_H

/* UI aggregate review d89256fe (docs/audits/ui_aggregate_review_2026-09-08.md):
 * app.js's recovery banner tells the operator "Firing is NOT available" --
 * but no HTTP route actually checked boot_guard_is_recovery_mode(); the
 * Start control stayed enabled and a firing/autotune request would have been
 * accepted at this door.
 *
 * Established truth first (per that review's own instruction): a request
 * made today, in recovery mode, is NOT accepted and does NOT hang --
 * profile_executor_run()/autotune_begin_run_locked() already refuse it,
 * because profile_executor_start()/autotune_engine_start() are never called
 * in recovery mode (main_control_bringup.c), so s_exec.lock/s_at.lock is
 * still NULL and every public entry point tests that first (see
 * test_profile_executor_prestart.c / test_autotune_engine_prestart.c, and
 * boot_guard.h's own doc comment above RECOVERY_MODE_ENABLED, which records
 * a bench run with recovery mode forced true where a profile start came
 * back "profile executor not started", not a panic). So this was a missing
 * EARLY, EXPLICIT refusal, not a missing guard -- the board was never at
 * risk of actually firing in recovery mode.
 *
 * What WAS missing: that refusal never named recovery mode, so an operator
 * seeing "profile executor not started" has no way to tell "this boot is
 * degraded" apart from any other prestart refusal reading the same NULL
 * lock. This function is the deliberate, explicit, API-layer enforcement
 * that names the real reason, checked before either engine's own generic
 * guard ever runs -- so the refusal an operator sees matches the banner's
 * claim exactly.
 *
 * Deliberately does NOT touch anything recovery mode exists to make
 * reachable: POST /api/ota/esp/recovery_exit (ota_http_recovery.c) and the
 * rest of the OTA/update family never call this function, so the one
 * legitimate action recovery mode must still allow -- flashing a fix and
 * rebooting out of it -- is unaffected. */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "boot_guard.h"

static inline bool recovery_mode_refuses_start(char *err_msg, size_t err_cap)
{
    if (!boot_guard_is_recovery_mode()) {
        return false;
    }
    if (err_msg != NULL && err_cap > 0) {
        snprintf(err_msg, err_cap,
                 "refused -- board is in RECOVERY MODE this boot; firing and autotune are not "
                 "available (see the recovery banner, or \"Exit recovery mode & reboot now\" on "
                 "the Firmware update page)");
    }
    return true;
}

#endif
