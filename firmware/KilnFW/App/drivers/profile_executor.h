// profile_executor -- TODO.md section 6 + 6A: the task that drives a
// profile's ramp/dwell schedule across the zone(s) it targets, through one
// of three control modes per zone (zones_http.h's zone_control_mode_t),
// with the full thermal-protection guard suite (thermal_guard.h) armed on
// every active zone.
//
// Module layout (TODO.md 6A.7): this is the only module in the set that
// talks to FreeRTOS, kiln_io, relay_authority, or the HTTP layer. The
// control math (pid.h), the safety guards (thermal_guard.h), and the
// duty-to-relay rendering (heater_output.h) are all pure and live in their
// own files specifically so they can be tested without this one.
//
// TODO.md 6A.5: a profile can drive more than one zone concurrently,
// sharing a single ramp/dwell schedule -- profile_t.zone_mask, not a single
// zone_index. Each active zone runs its own independent PID/guard/relay
// ("detuned decentralized PID", 6A.5(a)) against one shared setpoint, and
// that shared setpoint's ramp only advances while every active,
// not-already-per-zone-faulted zone is within PROFILE_EXECUTOR_RAMP_LOCK_BAND_C
// of it -- "ramp-lock" / setpoint governor, 6A.5(d), "the slowest zone sets
// the pace". A per-zone guard trip (guards 1/2/4/7) drops only that zone and
// the run continues for the rest; a global guard trip (guards 3/5/6) drops
// every zone and faults the whole run, same escalation split as before this
// pass, just now applied per-zone within one run instead of being the whole
// run's only zone.
//
// TODO.md 6A.3, "No auto-resume across reboot": this module writes a small
// fixed-size breadcrumb to NVS (run_state.h) on every meaningful transition
// -- run start, segment change, pause/resume, fault, done, operator halt --
// plus a slow periodic refresh while running. profile_executor_start() loads
// the previous boot's record and logs it, loudly at WARN if that firing was
// never cleanly ended. It is loaded, logged, and served over HTTP, and that
// is all: no path exists from a stored record back into
// profile_executor_run(), and the executor always comes up IDLE with relays
// off (kiln_io_init()'s latch ordering). A firing must never restart itself
// -- see run_state.h for why "resume where it left off" is wrong even when
// it would be safe.
//
// Scope of what's built in this pass vs. TODO.md 6A's full plan:
//   - Built: multi-zone concurrent execution sharing one ramp/dwell
//     schedule, ramp-lock (6A.5(d)), per-zone guard escalation within a
//     multi-zone run, all three control modes per zone, a second watchdog
//     task for guard 9, calibration applied to the control-facing reading.
//   - Built elsewhere since this pass: the coupling-matrix capture itself
//     (6A.5(b)) landed 2026-08-11 in autotune_engine.c
//     (autotune_engine_get_coupling_matrix(), served as GET
//     /api/autotune/matrix), so guard 8's threshold design is no longer
//     blocked on a missing matrix -- only on nobody having written the
//     guard yet.
//   - NOT built (see TODO.md 6A for the full list): guard 8 (cross-zone
//     plausibility), the RGA display (6A.5(c)), the static decoupler
//     (6A.5(e), explicitly gated on the RGA saying it's warranted),
//     electrical load staggering, and PID autotune's own multi-zone
//     awareness (autotune still runs one zone at a time, unaffected by this
//     pass -- see autotune_engine.h).
//
// HARDWARE STATUS AS WRITTEN: built and logic-verified against the absent-
// hardware code paths (io/thermo_bus NULL, matching every other driver's
// non-fatal convention) but NOT verified against a real relay board or
// thermocouple daughterboard, and specifically NOT verified against a real
// *multi-zone* firing (no way to attach more than the absent hardware at
// all right now) -- neither a relay board nor a thermocouple daughterboard
// is attached to this bench unit as of this writing. Treat every
// control/guard/ramp-lock behavior here as unverified until it has actually
// driven real relays against real readings across more than one zone at
// once. See docs/PROJECT_STATUS.md.
#ifndef PROFILE_EXECUTOR_H
#define PROFILE_EXECUTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "MAX31856.h"
#include "kiln_io.h"
#include "pid.h"
#include "profiles_http.h"
#include "safety_link.h"
#include "thermal_guard.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROFILE_EXEC_IDLE = 0,   /* nothing running */
    PROFILE_EXEC_RUNNING,
    PROFILE_EXEC_PAUSED,
    PROFILE_EXEC_DONE,       /* ran to completion; relays off; call profile_executor_run() to go again */
    PROFILE_EXEC_FAULTED,    /* a GLOBAL thermal guard tripped (or every active zone individually
                              * faulted -- nothing left to run); relays off across the whole run;
                              * stays here until profile_executor_halt() explicitly acknowledges it
                              * (latching, TODO.md 6A.3) */
} profile_exec_state_t;

/* Per-zone status within the current (or last) run. Only zones[i] with
 * .active == true participated in this run -- the rest are zeroed. */
typedef struct {
    bool     active;
    float    actual_c;        /* calibration-corrected; meaningless if !actual_valid */
    bool     actual_valid;
    bool     relay_commanded_on;
    float    duty;
    uint8_t  control_mode;    /* zone_control_mode_t */
    bool     faulted;         /* this zone's own per-zone guard tripped (guards 1/2/4/7) --
                               * the run continues for other active zones unless every
                               * active zone ends up faulted, see profile_exec_state_t. A
                               * GLOBAL trip (guards 3/5/6) instead sets the whole run to
                               * PROFILE_EXEC_FAULTED and every zone shows faulted here. */
    char     fault_reason[96];
    uint8_t  fault_guard;     /* thermal_guard_trip_t, only meaningful when faulted */
    /* TODO.md 6A.9's "/api/control... PID term breakdown", per zone.
     * Only meaningful when control_mode == ZONE_CONTROL_MODE_PID. */
    float    pid_p;
    float    pid_i;
    float    pid_d;
    /* TODO.md 6A.2's model feedforward, (T_sp - T_ambient)/K_dc +
     * (dT_sp/dt)*tau/K_dc, clamped to [0,1]. Exactly 0 for a zone with no
     * autotune-identified plant model, which is how a never-tuned zone keeps
     * running on feedback alone. Reading it against pid_i is the point of
     * publishing the split: a healthy tuned zone should show most of its duty
     * under ff with i small and steady, and a large i sitting under a
     * non-zero ff means the model is wrong, not that the loop is. */
    float    pid_ff;
    /* TODO.md 6A.2's "Output clamp [0,1]... the controller should report
     * 'cannot follow, cooling-limited' rather than sit at u=0 looking
     * healthy while the actual curve diverges." True once this zone's PID
     * output has sat at 0 (nothing to give -- there is no active cooling)
     * for PROFILE_EXECUTOR_COOLING_LIMITED_HOLD_S while still reading more
     * than PROFILE_EXECUTOR_COOLING_LIMITED_MARGIN_C above the ramp target
     * -- i.e. the kiln is losing heat slower than the profile asked it to,
     * and nothing the loop can command will close that gap. Only
     * meaningful when control_mode == ZONE_CONTROL_MODE_PID; always false
     * for BANGBANG (no continuous u=0 to observe the same way) and OFF. */
    bool     cooling_limited;
} profile_exec_zone_status_t;

typedef struct {
    profile_exec_state_t state;
    uint8_t  profile_id;
    char     profile_name[PROFILE_NAME_MAX_LEN + 1];
    uint8_t  zone_mask;        /* which zones this run targets (profile_t.zone_mask) */
    uint8_t  segment_index;    /* 0-based; valid when state is RUNNING/PAUSED/DONE/FAULTED */
    uint8_t  segment_count;
    bool     dwelling;         /* false = ramping toward target_c, true = holding it */
    float    target_c;         /* current shared commanded target (interpolated during a ramp) */
    uint32_t segment_elapsed_s; /* only advances while ramp-lock is satisfied -- see this
                                 * header's top comment */
    uint32_t dwell_remaining_s; /* only meaningful while dwelling */
    bool     ramp_lock_held;    /* true if the shared setpoint is NOT advancing this tick
                                 * because at least one active, non-faulted zone is outside
                                 * PROFILE_EXECUTOR_RAMP_LOCK_BAND_C of target_c */
    uint8_t  ramp_lock_lagging_mask; /* which zone(s) are the reason, if ramp_lock_held */
    profile_exec_zone_status_t zones[MAX31856_CHANNEL_COUNT];

    /* Duration-model inputs for /api/profile_exec's total_planned_s/
     * elapsed_s/remaining_s and for /api/profile_plan's polyline against
     * the profile that is actually running (dashboard_http.c does the
     * arithmetic; this struct just hands out the two things it cannot get
     * any other way -- the segment data as run, and the temperature
     * segment 0's ramp actually started from).
     *
     * segments/segment_count: a COPY of the profile this run is executing
     * (s_exec.profile can differ from whatever profiles_http.c holds for
     * this id right now -- profile_executor_run() takes its own copy at
     * start, same reasoning as s_exec_state_t.profile in the .c file), so a
     * caller computing this run's planned duration is guaranteed to match
     * the schedule actually driving relays, not a schedule since edited
     * under it. */
    profile_segment_t segments[PROFILE_MAX_SEGMENTS];

    /* The temperature segment 0's ramp treated as its starting point --
     * profile_executor_run()'s baseline_target_c, captured once from the
     * first active zone's actual reading at firing start (or that segment's
     * own target if no reading was available yet). Fixed for the life of
     * the run so a caller's total-planned-seconds answer does not drift
     * tick to tick as target_c itself moves through the ramp -- only
     * meaningful when state != PROFILE_EXEC_IDLE. */
    float    run_start_c;

    /* Real seconds since this run started (profile_executor_run()), NOT
     * reset at a segment boundary -- unlike segment_elapsed_s above, this
     * only freezes across a PAUSE (the control task simply doesn't tick it
     * while PROFILE_EXEC_PAUSED, same discipline as segment_elapsed_s) and
     * otherwise keeps counting even while ramp-lock holds the shared
     * setpoint, because wall-clock time is genuinely passing then -- ramp-
     * lock is exactly the situation /api/profile_exec's
     * remaining_is_estimate exists to flag, not a reason to stop the clock.
     * This is "elapsed_s" in the API contract. */
    uint32_t total_elapsed_s;

    char     fault_reason[96];   /* only meaningful when state == PROFILE_EXEC_FAULTED (a GLOBAL trip) */
    uint8_t  fault_guard;        /* thermal_guard_trip_t, only meaningful when state == PROFILE_EXEC_FAULTED */
} profile_exec_status_t;

#define PROFILE_EXECUTOR_HYSTERESIS_C 2.0f /* BANGBANG mode's fixed band -- see profile_executor.c */

/* TODO.md 6A.2's cooling-limited diagnostic (see
 * profile_exec_zone_status_t::cooling_limited): how far above target and
 * how long before "duty pinned at 0" is reported as "cannot follow" rather
 * than just a normal, brief overshoot settling out. Firmware-wide constants
 * for now, like PROFILE_EXECUTOR_HYSTERESIS_C above -- not yet per-zone
 * config, since nothing has asked for that granularity here. */
#define PROFILE_EXECUTOR_COOLING_LIMITED_MARGIN_C 2.0f
#define PROFILE_EXECUTOR_COOLING_LIMITED_HOLD_S 60.0f

#define PROFILE_EXECUTOR_DEFAULT_SANITY_RATE_C_PER_MIN 0.5f

#define PROFILE_EXECUTOR_TICK_MS 1000u /* 1 Hz per TODO.md 6A.7 */

/* TODO.md 6A.5(d): "ramp_lock_band_c (default 25 C)". Not yet per-profile/
 * per-zone configurable -- a single firmware-wide default, same status as
 * PROFILE_EXECUTOR_HYSTERESIS_C above. */
#define PROFILE_EXECUTOR_RAMP_LOCK_BAND_C 25.0f

/* History ring buffer (TODO.md section 0 / 6A.9): one sample per
 * HISTORY_SAMPLE_PERIOD_S while a profile is RUNNING, oldest overwritten
 * once full -- "this firing's trend," not indefinite history, matching
 * section 0's RAM-only settled design. Scoped to a single representative
 * zone (the lowest-indexed active zone in the run's zone_mask) even though
 * multi-zone concurrent execution now exists -- true per-zone history would
 * multiply this buffer's 23KB (was ~58KB before the 2026-08-12 packing --
 * see profile_history_entry_t) by up to MAX31856_CHANNEL_COUNT, and 6A.9
 * never actually asked for per-zone history, only for the buffer to gain a
 * duty field (done). Revisit if a real multi-zone firing shows the
 * single-series view isn't enough. Resets to empty at the start of every
 * profile_executor_run(). */
#define HISTORY_SAMPLE_PERIOD_S 30u
#define HISTORY_MAX_SAMPLES 2880u /* 24h at 30s/sample, per section 0's settled sizing */

/* The shape callers see. Storage is NOT this struct -- see
 * profile_executor.c's packed history_slot_t, which holds the same sample in
 * 8 bytes instead of 20. That packing is not premature: at 2880 samples this
 * struct cost 57.6 KB of .bss, and on 2026-08-12 the board was found booting
 * with 7 KB of free heap, unable to start tasks or serve HTTP, precisely
 * because this buffer and the autotune trace had eaten the DRAM before Wi-Fi
 * came up. The accessors below unpack on read, so this API is unchanged. */
typedef struct {
    uint32_t elapsed_s;   /* since this run started */
    float    actual_c;
    float    desired_c;
    float    duty;
    uint8_t  guard;       /* thermal_guard_trip_t; 0 = THERMAL_GUARD_TRIP_NONE */
} profile_history_entry_t;

/* Total number of valid history samples right now (0..HISTORY_MAX_SAMPLES),
 * for a caller that wants to page through profile_executor_get_history()
 * without allocating a HISTORY_MAX_SAMPLES-sized buffer up front. */
size_t profile_executor_get_history_count(void);

/* Copies up to max_entries of this run's history into out, starting at
 * chronological index start_index (0 = oldest), oldest-to-newest. Returns
 * the number actually written (less than max_entries at the tail of the
 * buffer). Safe to call from any state (an IDLE/DONE/FAULTED executor still
 * has the last run's buffer until the next profile_executor_run() clears
 * it). Paged rather than all-at-once so a caller streaming this out (e.g.
 * a CSV HTTP response) doesn't need a HISTORY_MAX_SAMPLES-sized buffer of
 * its own -- at 2880 entries that's real memory, worth not doubling up. */
size_t profile_executor_get_history(profile_history_entry_t *out, size_t start_index, size_t max_entries);

/* Brings the module up and starts its control task plus a second,
 * independent watchdog task (guard 9 -- "a control loop cannot be its own
 * watchdog", TODO.md 6A.3). Matches every other driver's
 * *_start(..., io_or_null, ...) non-fatal-bring-up convention. Safe to call
 * with io_or_null/thermo_bus_or_null/safety_or_null NULL -- the executor
 * still runs its state machine (ramp/dwell timing, the dashboard's status
 * view) but withholds heat and skips the guards it can't evaluate without
 * a reading, per the "no valid data -> the safe default is never command
 * heat" rule used throughout this codebase. */
esp_err_t profile_executor_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                                  SafetyLinkClass *safety_or_null);

/* Begins running profile_id from its first segment across every zone in its
 * zone_mask, each using that zone's currently configured control mode
 * (zones_config_get_control_mode()). Re-runs the TODO.md section 5
 * feasibility check against every participating zone's *current* max-ramp
 * ceiling. Refuses if already RUNNING/PAUSED (call profile_executor_halt()
 * first), or if any targeted zone's guard state is still FAULTED from a
 * previous run (same reason -- halt() first), or if an autotune run is
 * active on any zone this profile targets (TODO.md 6A.4/6A.5's "zones must
 * be autotuned one at a time" ordering constraint, enforced from both
 * directions -- see autotune_engine.h). */
bool profile_executor_run(uint8_t profile_id, char *err_msg, size_t err_cap);

/* Stops whatever is running/paused/faulted, drops every active zone's
 * relays (fail toward off), clears any fault this run asserted (global
 * safety_link source and/or each zone's relay_authority block), and resets
 * every active zone's thermal guard latch -- the one explicit operator
 * acknowledgement TODO.md 6A.3 requires to leave a tripped state. A no-op
 * from PROFILE_EXEC_IDLE. */
void profile_executor_halt(void);

/* Pause/resume: pause drops every active zone's relays and freezes the
 * shared ramp/dwell schedule; resume picks up exactly where it left off.
 * Both return false (no state change) if the executor isn't in a state
 * where the transition makes sense. */
bool profile_executor_pause(void);
bool profile_executor_resume(void);

/* Snapshot for the dashboard's status API -- never blocks on the executor
 * task. */
void profile_executor_get_status(profile_exec_status_t *out);

/* True if zone_index is one of the zones the currently RUNNING/PAUSED
 * profile (if any) targets -- autotune_engine.c uses this to refuse
 * starting a step test against a zone a profile is actively driving,
 * without having to refuse autotune globally just because *some* other
 * zone has a profile running (TODO.md 6A.5's multi-zone execution makes
 * that distinction meaningful in a way it wasn't before this pass). */
bool profile_executor_zone_is_active(uint8_t zone_index);

#ifdef __cplusplus
}
#endif

#endif // PROFILE_EXECUTOR_H
