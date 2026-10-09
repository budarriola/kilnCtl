// boot_reason.h -- latches the trip reason across a watchdog reset in two of
// the RP2040 watchdog's eight scratch registers (docs/ARCHITECTURE.md section
// 8, "Latch the trip reason in the watchdog scratch registers").
//
// Without this, a watchdog reset loses *why* the system was tripping and the
// board comes back up looking merely freshly-booted -- the difference between
// a diagnosable fault and a mystery, per the doc.
//
// Deliberately does not use scratch[4]: pico-sdk's own watchdog_enable() /
// watchdog_enable_caused_reboot() already use it as their own marker
// (hardware_watchdog/watchdog.c). scratch[0] and scratch[1] are unused by the
// SDK and free for this.
#ifndef SAFTYFW_BOOT_REASON_H
#define SAFTYFW_BOOT_REASON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// safety_trip_t values from safety_guards.h fit in a uint32_t; this module
// does not itself depend on safety_guards.h (main.c does the cast) so it
// stays usable from watchdog_task.c without pulling in the guard module.
typedef struct {
    bool     watchdog_caused_reboot;      // hardware_watchdog's own reason bit
    bool     watchdog_enable_caused_reboot;
    bool     trip_reason_valid;           // magic word matched -- trip_reason below is trustworthy
    uint32_t trip_reason;                 // safety_trip_t, valid only if trip_reason_valid
} saftyfw_boot_reason_t;

// Reads and validates the scratch registers. Call once, early in main(),
// BEFORE boot_reason_clear_trip() -- this is the only chance to see what the
// previous boot left behind. Does not itself call watchdog_caused_reboot();
// the caller passes those two SDK facts in, so this module has no pico-sdk
// watchdog.h dependency beyond the scratch registers it already needs.
//
// Also caches its result (see boot_reason_get_cached() below) -- main.c calls
// this exactly once, at step 3 of the boot sequence, and nothing else in the
// build has another opportunity to read the scratch registers before
// boot_reason_clear_trip() (also called once, from main.c) zeroes the magic
// word. Caching here, rather than main.c threading the struct through every
// task's start function, is what lets link_task's Frame B (SAFETY_CMD_DIAG,
// TODO.md Phase 8) report boot_reason without a new coupling.
saftyfw_boot_reason_t boot_reason_read(bool wd_caused_reboot, bool wd_enable_caused_reboot);

// Returns whatever the one boot_reason_read() call this boot passed in,
// zeroed/false in every field if boot_reason_read() has not run yet (should
// not happen in practice -- main.c calls it at step 3, before any task that
// could call this starts). Safe to call from any task: the cached struct is
// written once, at boot, before the scheduler starts, and never again.
saftyfw_boot_reason_t boot_reason_get_cached(void);

// Latches a trip reason into the scratch registers with the magic word, so it
// survives a subsequent watchdog reset. Called by safety_core when a guard
// newly trips -- see safety_guards_tick()'s newly-tripped return value.
void boot_reason_latch_trip(uint32_t trip_reason);

// Clears the latch (magic word zeroed) once the trip reason has been read and
// reported for this boot, so a *future* unrelated reset does not re-report a
// stale reason. Call after boot_reason_read(), once its result has been
// consumed (diagnostic frame / log line).
void boot_reason_clear_trip(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_BOOT_REASON_H
