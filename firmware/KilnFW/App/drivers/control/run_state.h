// run_state -- the "what was running and how far it got" breadcrumb that
// survives a reboot. TODO.md 6A.3's "No auto-resume across reboot" bullet.
//
// WHY THIS IS NOT A RESUME FEATURE, AND MUST NEVER BECOME ONE
// -----------------------------------------------------------------------
// Half of that bullet was already satisfied before this module existed:
// kiln_io_init()'s latch ordering brings every relay up OFF, so a firing
// physically cannot restart itself after a brownout. That property is the
// safety guarantee and nothing here touches it. What was missing is the
// operator-facing half: after the power blinks at 3am, the person standing
// in front of the kiln has no way to answer "was it firing, which profile,
// which segment, how far in -- and did it stop because somebody stopped it,
// or because the power went out?" A kiln full of ware at cone temperature
// makes that a real question with a real cost attached.
//
// So this module persists a description of the run and nothing else. It
// exposes no API that commands a relay, starts a profile, or is consulted by
// the control loop. profile_executor.c writes to it; run_state_init() reads
// it back at boot and logs; dashboard_http.c serves it. That is the entire
// data flow, deliberately one-directional: nothing downstream of a load can
// influence what the kiln does. If a future change ever wants to feed this
// record back into profile_executor_run(), that is the change TODO.md 6A.3
// forbids -- an unattended element re-energizing itself into a chamber
// nobody has looked at, with a schedule whose thermal history is now a lie
// (the ware cooled during the outage; picking up "where it left off" is
// wrong even when it is safe).
//
// FLASH, NOT RAM: see run_state.c's cadence note. This is written to NVS, so
// it is written on transitions plus a slow periodic refresh -- never per
// control tick.
//
// NO WALL CLOCK: this board has no RTC and no guaranteed SNTP (see
// docs/HARDWARE.md), so the record carries uptime-at-write, not an absolute
// timestamp. "The firing was 4h12m into this boot when the record was
// written" is true and checkable; a fabricated date/time would look
// authoritative and be wrong, which is worse than no timestamp at all for
// the one question this record exists to answer.
#ifndef RUN_STATE_H
#define RUN_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "profiles_types.h" /* PROFILE_NAME_MAX_LEN */

#ifdef __cplusplus
extern "C" {
#endif

/* How the run stood as of the last write. The only distinction that really
 * matters is "ended" vs. "did not end": a record loaded at boot still saying
 * RUNNING or PAUSED means the firmware stopped between one write and the
 * next without ever recording an ending -- brownout, watchdog reset, crash,
 * somebody pulling the plug. Everything else is an ending the firmware
 * itself recorded, so the operator knows the stop was intentional. */
typedef enum {
    RUN_STATE_PHASE_NONE = 0,  /* no run has ever been recorded */
    RUN_STATE_PHASE_RUNNING,   /* NOT an ending */
    RUN_STATE_PHASE_PAUSED,    /* NOT an ending -- a paused firing that never came back is still interrupted */
    RUN_STATE_PHASE_DONE,      /* ending: ran every segment to completion */
    RUN_STATE_PHASE_HALTED,    /* ending: an operator stopped it (profile_executor_halt()) */
    RUN_STATE_PHASE_FAULTED,   /* ending: a guard tripped -- fault_guard/fault_reason carry which */
} run_state_phase_t;

/* Truncation of profile_exec_status_t's 96-char fault_reason. 64 holds every
 * message thermal_guard.c actually formats today with room to spare, and the
 * whole point of a fixed-size record is that it stays small: this firmware
 * was found on 2026-08-12 booting with 7 KB of free heap because a static
 * buffer got sized without anybody adding it up (see profile_executor.h's
 * history-buffer note). A truncated reason plus the numeric fault_guard is
 * still an unambiguous answer to "why did it stop". */
#define RUN_STATE_REASON_MAX 64

/* Bumped whenever run_state_record_t's layout changes. An older record is
 * discarded rather than migrated: this is a breadcrumb, and one lost
 * breadcrumb across a firmware update costs nothing, whereas mis-parsing an
 * old layout would print a confident, wrong answer to "what was it doing". */
#define RUN_STATE_RECORD_VERSION 1

/* Persisted verbatim as one fixed-size NVS blob. Field order and the
 * explicit reserved[] are chosen so the struct has no implicit padding on
 * this toolchain and sizeof() is stable -- a blob whose size drifts between
 * builds reads back as corrupt (see run_state.c's loader), and while that
 * only costs a breadcrumb here, the same mistake in zones_http.c's config
 * blob has twice wiped an operator's zone setup. Adding a field means
 * bumping `version`, not silently changing the size. */
typedef struct {
    uint8_t  version;          /* RUN_STATE_RECORD_VERSION at write time */
    uint8_t  phase;            /* run_state_phase_t */
    uint8_t  profile_id;
    uint8_t  zone_mask;
    uint8_t  segment_index;    /* 0-based, as profile_exec_status_t reports it */
    uint8_t  segment_count;
    uint8_t  fault_guard;      /* thermal_guard_trip_t; 0 when not FAULTED */
    uint8_t  dwelling;         /* 0 = ramping toward target_c, 1 = holding it */
    uint8_t  acknowledged;     /* operator has seen this record (see run_state_acknowledge) */
    uint8_t  reserved[3];      /* explicit, so target_c below is 4-aligned with no implicit padding */
    float    target_c;         /* shared commanded setpoint at write time */
    uint32_t segment_elapsed_s;
    uint32_t uptime_s;         /* esp_timer uptime at write -- see this header's no-wall-clock note */
    char     profile_name[PROFILE_NAME_MAX_LEN + 1];
    char     fault_reason[RUN_STATE_REASON_MAX];
} run_state_record_t;

/* What profile_executor.c hands in on every update. Deliberately by pointer
 * to caller-owned strings rather than a second copy of the record: the
 * executor already holds these fields, and this module keeps exactly one
 * copy of the record in .bss. */
typedef struct {
    uint8_t     profile_id;
    const char *profile_name;   /* may be NULL */
    uint8_t     zone_mask;
    uint8_t     segment_index;
    uint8_t     segment_count;
    bool        dwelling;
    float       target_c;
    uint32_t    segment_elapsed_s;
    uint8_t     fault_guard;    /* thermal_guard_trip_t; 0 if none */
    const char *fault_reason;   /* may be NULL */
} run_state_snapshot_t;

/* Periodic-refresh interval while a firing is RUNNING. See run_state.c for
 * the flash-wear arithmetic behind this number. */
#define RUN_STATE_REFRESH_INTERVAL_S 300

/* Loads the stored record and logs it. If it describes a firing that was
 * never cleanly ended, this logs at WARN, loudly and repeatedly enough to be
 * visible in a boot log -- and then does nothing else. It commands no relay,
 * starts no profile, and hands nothing to profile_executor.c. Same
 * load-tolerant convention as relay_cycles.c/zones_http.c: a missing or
 * wrong-sized/wrong-version blob is treated as "no record", not as a reason
 * to fail bring-up. Safe to call more than once. */
esp_err_t run_state_init(void);

/* Records a state transition. ALWAYS writes to NVS -- these are the events
 * whose loss would make the record lie (a DONE that never landed reads back
 * as an interrupted firing, which is exactly the false alarm this module
 * must not produce). Call on run start, segment change, pause, resume,
 * fault, completion, and operator halt. snap may be NULL only for
 * RUN_STATE_PHASE_NONE. */
void run_state_note(run_state_phase_t phase, const run_state_snapshot_t *snap);

/* Periodic refresh of a run already noted as RUNNING: updates segment
 * progress so an interrupted record says roughly how far it got. Writes at
 * most once per RUN_STATE_REFRESH_INTERVAL_S and only while the live phase
 * is RUNNING, so it is cheap to call every control tick. */
void run_state_note_progress(const run_state_snapshot_t *snap);

/* Copies the record as it was loaded at boot -- NOT the live one this boot's
 * run has been writing. Returns false (and zeroes out) if there was no
 * record, or if it has already been acknowledged. This is the operator
 * breadcrumb; the live run's state is profile_executor_get_status()'s job. */
bool run_state_get_boot_record(run_state_record_t *out);

/* True if the boot record describes a firing that was never cleanly ended.
 * Purely informational -- no caller may branch control behaviour on it. */
bool run_state_boot_record_interrupted(void);

/* Operator has seen the boot record; stop reporting it. Persists the
 * acknowledgement only when no run has overwritten the stored record since
 * boot -- once a newer firing owns the key, the old record is gone from
 * flash anyway and dropping the RAM copy is the whole job. Returns false if
 * there was nothing to acknowledge. */
bool run_state_acknowledge(void);

/* Human-readable phase name, for logs and JSON. Never NULL. */
const char *run_state_phase_name(run_state_phase_t phase);

#ifdef __cplusplus
}
#endif

#endif // RUN_STATE_H
