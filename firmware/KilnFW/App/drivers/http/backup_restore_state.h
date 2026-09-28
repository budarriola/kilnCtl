// backup_restore_state -- the one fact profile_executor_run.c/autotune_engine.c
// need out of backup_import.c: is a backup restore's commit pass in flight
// right now. Split out of backup_http.h deliberately (2026-09-28, A4 review
// follow-up A) rather than pulling in that whole header (and its esp_err.h
// dependency) from two control-module choke points that are otherwise plain
// C with no ESP-IDF HTTP surface in their own includes.
//
// backup_import_restore_in_flight() is a lock-free read (an _Atomic bool) --
// safe to call from any task, including one already holding s_exec.lock or
// s_at.lock (CLAUDE.md's lock-order note: s_exec.lock then s_at.lock,
// relay_authority a leaf; this reads no lock at all). Set true by
// backup_import.c's backup_import_job() as the job task starts (before the
// body read, its TOCTOU re-check and its tens-of-seconds commit pass) --
// see that function's comment for the store-then-read pairing with the
// start paths' commit-point re-read that closes the start-during-restore
// window.
#ifndef BACKUP_RESTORE_STATE_H
#define BACKUP_RESTORE_STATE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// True from the start of backup_import.c's import job task through
// the end of its (possibly tens-of-seconds) two-pass commit, false at every
// other time. Never blocks.
bool backup_import_restore_in_flight(void);

#ifdef __cplusplus
}
#endif

#endif // BACKUP_RESTORE_STATE_H
