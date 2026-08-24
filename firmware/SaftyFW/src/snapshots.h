// snapshots.h -- shared producer/consumer snapshot structs, exactly as
// specified in docs/ARCHITECTURE.md section 6 ("Data flow and snapshots").
// Each producer task publishes a complete, self-consistent, timestamped
// snapshot; consumers take the newest whole snapshot, never a partial read.
//
// This file is shared across independently-developed tasks (thermo_task,
// current_task, ...). If you are adding a struct here and another one
// already exists, ADD to this file rather than replacing it, and re-read it
// immediately before editing in case another in-flight change landed first.
#ifndef SAFTYFW_SNAPSHOTS_H
#define SAFTYFW_SNAPSHOTS_H

#include <stdbool.h>
#include <stddef.h> // NULL, for context_reduce_zones()/current_any_present() below
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// thermo_snapshot_t -- thermo_task / max31856.c (docs/ARCHITECTURE.md
// section 6, docs/THERMOCOUPLE.md). valid == false implies tc_c and cj_c are
// NaN (never 0, never a cached reading -- the MAX31856 driver's "failure
// honesty" discipline, firmware/KilnFW/docs/MAX31856.md). While valid ==
// true, tc_c and/or cj_c can still independently be NaN when the part's own
// SR fault bits say that specific half of the reading is meaningless (e.g.
// OPEN leaves tc_c NaN while cj_c, from the same successful transaction, is
// still good) -- see max31856.c's MAX31856_TC_INVALIDATING_FAULTS/CJRANGE
// handling. fault_bits mirrors the MAX31856 SR register layout that
// safety_guards.h's SAFETY_THERMO_FAULT_* macros and max31856.h's
// MAX31856_FAULT_* macros both already document -- this struct just carries
// the raw SR byte, it does not redefine the bits a third time.
typedef struct {
    uint32_t timestamp_ms;
    bool     valid;
    float    tc_c, cj_c;      /* NaN when !valid */
    uint8_t  fault_bits;      /* THERMO_FAULT_* */
    bool     spi_failed;
} thermo_snapshot_t;

// current_snapshot_t -- current_task / adc_owner (docs/ARCHITECTURE.md
// section 6, docs/CURRENT_SENSE.md). `amps[n]` is the UNFILTERED,
// 16x-oversample-averaged reading a future guard must consume (S3/S4/S9,
// Phase 7) -- never the slow tau=0.5s reporting filter from
// docs/CURRENT_SENSE.md section 4. The filtered, reporting-only quantities
// (`i_conducting_a`, `conduction_fraction`, `p_avg_w`, docs/CURRENT_SENSE.md
// section 3b) live in current_sense_power_t (src/current_sense.h)
// deliberately OUTSIDE this struct, so nothing that reads current_snapshot_t
// can accidentally end up consuming a delayed value.
// `present[n]`, added 2026-08-24 alongside the current-sense-calibration-
// wiring fix: current_sense.c's own current_presence_policy.h computes this
// DECOUPLED from k_ct_v_per_a (whereas `amps[n]` is 0.0f, honestly, whenever
// k_ct_v_per_a is not commissioned -- see current_sense.c's cs_counts_to_
// amps()). current_any_present() below reads `present[n]`, not a
// recomputed amps-vs-threshold comparison, specifically so S3/S9/S11/S6b's
// presence fact survives an uncommissioned k_ct_v_per_a -- see current_
// presence_policy.h's header comment for the full safe-direction reasoning.
typedef struct {
    uint32_t timestamp_ms;
    float    amps[3];
    bool     clipped[3];
    bool     present[3];
    bool     calibrated;
} current_snapshot_t;

// context_snapshot_t -- link_task / SAFETY_CMD_PUSH_CONTEXT (0x07),
// CommonFW/docs/LINK_PROTOCOL.md section 4. Published by link_task.c on every
// well-formed context frame received from the ESP; consumed (once built) by
// the context-dependent guards (S2/S6/S10, TODO.md Phase 7) and by
// link_task's own DIAG frame (context_age_100ms etc). `valid == false` means
// no well-formed PUSH_CONTEXT has ever been parsed this boot -- every other
// field is then meaningless, not zeroed-and-trustworthy.
//
// Field layout mirrors the wire frame directly (LINK_PROTOCOL.md section 4's
// table), not a firmware-side reinterpretation of it -- link_frame.c's
// unpacker is the only place that translates wire offsets into these names.
#define CONTEXT_SNAPSHOT_MAX_ZONES 3u

// Top-level flags byte (wire offset 1), passed through raw rather than
// exploded into bools -- callers that only need one bit (e.g. SIM_PLANT)
// mask it directly, matching how the ESP-side spec documents the byte.
#define CONTEXT_FLAG_PROFILE_RUNNING  0x01u
#define CONTEXT_FLAG_ANY_ZONE_FAULTED 0x02u
#define CONTEXT_FLAG_HEAT_REQUESTED   0x04u
#define CONTEXT_FLAG_CONTEXT_VALID    0x08u
#define CONTEXT_FLAG_SIM_PLANT        0x10u

// Per-zone flags byte (wire offset 1 of each 14-byte zone block).
#define CONTEXT_ZONE_FLAG_MEASURED_VALID 0x01u
#define CONTEXT_ZONE_FLAG_ACTIVE         0x02u
#define CONTEXT_ZONE_FLAG_RELAY_ON       0x04u
#define CONTEXT_ZONE_FLAG_GUARD_TRIPPED  0x08u

typedef struct {
    uint8_t zone_index;
    uint8_t flags;          /* CONTEXT_ZONE_FLAG_* */
    float   setpoint_c;
    float   measured_c;     /* raw, uncalibrated -- LINK_PROTOCOL.md section 4 */
    uint8_t sample_counter; /* increments only on a fresh conversion -- S13 */
    uint8_t tc_type;        /* THERMO_TC_* */
    uint8_t tc_fault;       /* MAX31856 SR bits */
} context_zone_t;

typedef struct {
    uint32_t timestamp_ms; /* local (Pico) uptime this snapshot was published, not the ESP's */
    bool     valid;        /* a well-formed PUSH_CONTEXT has been parsed this boot */
    uint8_t  flags;        /* CONTEXT_FLAG_* */
    uint8_t  boot_id;      /* ESP boot_id -- a change resets every correlation window */
    uint32_t seq;
    uint32_t uptime_ms;         /* ESP-reported uptime */
    uint8_t  relay_now_mask;    /* bits 0-3, relays 1-4, as actually commanded */
    uint8_t  relay_recent_mask; /* relays commanded on at any point in recent_window_s */
    uint8_t  recent_window_s;
    uint8_t  zone_count; /* 0..CONTEXT_SNAPSHOT_MAX_ZONES */
    context_zone_t zones[CONTEXT_SNAPSHOT_MAX_ZONES];
} context_snapshot_t;

// --- Pure reduction helpers -------------------------------------------------
// safety_core.c (src/tasks/safety_core.c) needs these to turn the raw structs
// above into the handful of scalars safety_guards.h's safety_guard_input_t
// actually takes (S2/S10's "already reduced by the caller" contract, see that
// struct's own field comments) -- but safety_core.c is structurally forbidden
// from #include-ing any header whose name contains "link" or "uart"
// (docs/ARCHITECTURE.md section 2, tools/check_isolation.ps1), which rules out
// putting them in link_frame.h/.c alongside link_frame_unpack_context() where
// they would otherwise belong. snapshots.h already has neither substring in
// its name and is already included by both safety_core.c and link_task.c for
// the structs themselves, so it is the one legal shared home. `static inline`
// (not `static`) deliberately: GCC/Clang do not warn on an unused static
// inline function the way they would a plain unused static one, so
// thermo_task.c/current_task.c/link_frame.c -- which include this header for
// the structs but never call these two -- build clean under -Wall -Wextra
// -Werror without needing a suppression. Free host-test coverage too
// (test/test_snapshots.c, test/build_host_tests.ps1): no FreeRTOS/pico-sdk
// dependency, same reasoning link_frame.c's own pure functions already rely
// on.

// S2/S10's zone reduction (SAFETY_MODEL.md section 4). Only zones with both
// CONTEXT_ZONE_FLAG_ACTIVE and CONTEXT_ZONE_FLAG_MEASURED_VALID set are
// eligible -- an inactive or faulted zone must influence neither S2's ceiling
// nor S10's nearest-match search. `*out_zone_count` is the count of ELIGIBLE
// zones, not `ctx->zone_count` itself: safety_guard_input_t's own doc
// comment is explicit that "zone_count == 0" means "no ACTIVE zones this
// tick", not "no zones were on the wire". `*out_max_setpoint_c` is the
// highest `setpoint_c` among eligible zones (S2). `*out_nearest_measured_c`
// is the eligible zone's `measured_c` whose absolute difference from `tc_c`
// is smallest (S10: "compares against the *nearest* valid zone reading, not
// the mean -- kilns stratify by well over 100C top to bottom"). When
// `ctx == NULL`, `!ctx->valid`, or no zone is eligible, `*out_zone_count` is
// 0 and the two float outputs are left at 0.0f -- meaningless in that case,
// exactly like every other "count == 0" contract already in this codebase
// (the guards themselves gate on context_valid/zone_count before ever
// reading the floats, so this never needs to publish NaN to be safe).
static inline void context_reduce_zones(const context_snapshot_t *ctx, float tc_c,
                                         uint8_t *out_zone_count, float *out_max_setpoint_c,
                                         float *out_nearest_measured_c)
{
    uint8_t count = 0;
    float   max_setpoint = 0.0f;
    float   nearest_measured = 0.0f;
    float   nearest_diff = 0.0f; /* meaningful only once count > 0 */

    if (ctx != NULL && ctx->valid) {
        uint8_t n = ctx->zone_count;
        if (n > CONTEXT_SNAPSHOT_MAX_ZONES) {
            n = CONTEXT_SNAPSHOT_MAX_ZONES; /* defensive -- ctx is caller-trusted here, but never overrun */
        }
        for (uint8_t i = 0; i < n; i++) {
            const context_zone_t *z = &ctx->zones[i];
            bool eligible = (z->flags & CONTEXT_ZONE_FLAG_ACTIVE) != 0u &&
                            (z->flags & CONTEXT_ZONE_FLAG_MEASURED_VALID) != 0u;
            if (!eligible) {
                continue;
            }
            if (count == 0u || z->setpoint_c > max_setpoint) {
                max_setpoint = z->setpoint_c;
            }
            float diff = z->measured_c - tc_c;
            if (diff < 0.0f) {
                diff = -diff;
            }
            if (count == 0u || diff < nearest_diff) {
                nearest_diff = diff;
                nearest_measured = z->measured_c;
            }
            count++;
        }
    }

    if (out_zone_count) {
        *out_zone_count = count;
    }
    if (out_max_setpoint_c) {
        *out_max_setpoint_c = max_setpoint;
    }
    if (out_nearest_measured_c) {
        *out_nearest_measured_c = nearest_measured;
    }
}

// S3/S4/S6b/S11's "is the load actually drawing current right now" fact
// (SAFETY_MODEL.md section 3: presence/absence only, never a magnitude
// guard) -- OR across all three channels' precomputed `present[n]`.
// `cur == NULL` conservatively reads as "no current present", same "unknown
// means the conservative default" convention every other input in this
// codebase uses.
//
// 2026-08-24: this used to recompute `amps[n] > i_present_a` itself, taking
// i_present_a as a parameter. Changed to read the already-decided
// `present[n]` fact instead (current_sense.c computes it via current_
// presence_policy.h, decoupled from k_ct_v_per_a) so this function no
// longer needs -- and no longer CAN accidentally reintroduce -- the
// dependency on k_ct_v_per_a that silently disabled S3/S9/S11/S6b whenever
// a channel's CT scale factor was never commissioned. See current_
// presence_policy.h for the full reasoning.
static inline bool current_any_present(const current_snapshot_t *cur)
{
    if (cur == NULL) {
        return false;
    }
    return cur->present[0] || cur->present[1] || cur->present[2];
}

// S1's firing-ceiling gate (SAFETY_MODEL.md section 4, S1 /
// docs/GUARD_TEST_MATRIX.md section 6). `have_ceiling` is link_task_get_
// firing_ceiling()'s own already-bounds-checked fact (link_frame_ceiling_
// is_active() ran at RX time, in link_task.c, before the value was ever
// stored); `context_valid` is safety_core.c's own already-computed "stale
// context is no context" fact (SAFETY_MODEL.md section 5 rule 2,
// CONTEXT_MAX_AGE_MS in safety_core.c). SAFETY_CMD_SET_FIRING_CEILING is sent
// "repeated in every context frame's shadow" (CommonFW/docs/LINK_PROTOCOL.md
// section 4), so it shares PUSH_CONTEXT's own liveness contract rather than
// needing an independent staleness clock of its own.
//
// A pure one-line AND, factored out (rather than inlined at the one call
// site in safety_core.c) for the same reason context_reduce_zones()/
// current_any_present() are pure functions here rather than inline code in
// safety_core_build_input(): it is the isolation-legal, host-testable half of
// wiring SET_FIRING_CEILING, and it is short precisely because the actual
// bounds-checking work already happened elsewhere (link_frame_ceiling_
// is_active() at RX time) -- this only decides whether an already-valid fact
// is still fresh enough to act on.
//
// On link loss (or a version-mismatch DEGRADED_NO_CONTEXT, folded into
// context_valid the same way safety_core_build_input() already folds it for
// every other context-dependent input), this returns false and S1 reverts to
// abs_max_temp_c alone -- the same "goes inactive, not pessimistic" treatment
// every other context-consuming guard (S2/S3/S4/S10) already gets, rather
// than either trusting a stale ceiling forever (a hazard: a ceiling
// tightened for a bisque firing silently still capping a cone-10 firing hours
// later) or dropping S1 to some invented unclamped state -- abs_max_temp_c is
// never unclamped, it is the hard, always-commissioned backstop this falls
// back to.
static inline bool link_firing_ceiling_should_apply(bool have_ceiling, bool context_valid)
{
    return have_ceiling && context_valid;
}

// --- Link-task boundary getters ---------------------------------------------
// Declared here, not in link_task.h, for the same isolation reason as the
// pure functions above -- these three are implemented in link_task.c (their
// full doc comments live there / in link_task.h, which re-declares
// link_task_get_context_snapshot() and link_task_get_degraded_no_context()
// for link_task.c's own internal use and any other non-isolation-constrained
// caller). A matching prototype in two headers is ordinary, legal C -- the
// same trick reboot_announce.h already established for the ANNOUNCE_REBOOT
// grace-window fact (see safety_core.c's own header comment), applied here
// for context and link liveness, which need a snapshot-shaped and a
// boolean-shaped crossing respectively rather than reboot_announce.h's single
// timestamp.
bool link_task_get_context_snapshot(context_snapshot_t *out);
bool link_task_get_degraded_no_context(void);
// True iff link_task decoded a valid (CRC-passing, well-formed) kilnlink
// frame from the ESP within the last LINK_UP_RECENCY_MS (link_task.c) --
// a per-tick LEVEL, deliberately mirroring discrete_task_estop_pressed()'s
// own shape (safety_guard_input_t's own doc comment on link_up: "a level
// (true/false per tick), not a duration"). safety_guards.c's own
// s6b_link_down_elapsed_s accumulator is what turns this level into the
// real two-tier (10s soft / 120s hard) timeout -- this function only ever
// answers "is a frame arriving right now", never does the timeout math
// itself.
bool link_task_link_up(void);

// Milliseconds `relay_now_mask` (the context frame's instantaneous field, not
// `relay_recent_mask`'s already-ESP-computed OR-over-window) has been
// continuously nonzero across the sequence of PUSH_CONTEXT frames link_task
// has actually received, or 0 if it is not on right now. safety_core.c
// compares this against `correlation_window_s` (safety_guard_cfg_t) itself
// to produce S4's `relay_commanded_continuously` (SAFETY_MODEL.md section 4:
// "commanded on throughout the last correlation_window_s" -- an AND over the
// window, which neither wire mask alone answers: `relay_now_mask` is a single
// instant and `relay_recent_mask` is an OR, not an AND). Tracked in
// link_task.c because it is single-writer-safe there (link_task's own RX
// path is the only writer, matching s_context_frames_ok/bad's own
// documented reasoning) -- safety_core_build_input() is called from two
// different task contexts (safety_core_task's own tick, and
// safety_core_request_clear_trip() from link_task's CLEAR_TRIP handler), so
// it must never own a mutable time accumulator of its own.
uint32_t link_task_get_relay_on_continuous_ms(void);

// The most recently decoded SAFETY_CMD_SET_FIRING_CEILING (0x09) value that
// link_frame_ceiling_is_active() judged an active ceiling (finite, strictly
// positive) -- link_task.c's RX handler already applies that bounds check
// before ever storing a value here, so a caller never has to re-check it.
// Returns false (and leaves `*out_firing_max_c` at 0.0f) if no such frame has
// ever been decoded this boot, OR the most recent one carried 0/NaN/negative
// (the wire's own "no firing" state) -- both collapse to the same "no active
// ceiling" answer, matching context_snapshot_t's own "valid == false means
// every other field is meaningless" convention. Deliberately NOT gated on
// link liveness or context freshness in here: that combination is
// safety_core.c's own job (link_frame_firing_ceiling_should_apply(),
// src/tasks/link_frame.h) using its own already-computed context_valid, the
// same division of labour link_task_get_context_snapshot() above already
// establishes (link_task publishes the raw fact, safety_core decides what
// "stale" means for it).
bool link_task_get_firing_ceiling(float *out_firing_max_c);

// The most recently decoded SAFETY_CMD_SET_CLOCK (0x0C) epoch, in Unix
// milliseconds, if link_frame_clock_epoch_is_plausible() judged it plausible
// at RX time. Purely diagnostic (LINK_PROTOCOL.md section 4: "no guard may
// ever read this clock") -- no consumer exists in this codebase yet (nothing
// here is time-based in a way that needs wall-clock time; trip timestamps
// stay in milliseconds-since-boot, per Frame D's own fixed wire layout), so
// this is published for a future log/diag consumer the same "exposed but
// unconsumed" way link_task_get_context_snapshot() originally was. Returns
// false (and leaves `*out_epoch_ms` at 0) if no plausible SET_CLOCK has ever
// been decoded this boot.
bool link_task_get_wall_clock_epoch_ms(uint64_t *out_epoch_ms);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_SNAPSHOTS_H
