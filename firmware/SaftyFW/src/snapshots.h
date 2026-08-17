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
typedef struct {
    uint32_t timestamp_ms;
    float    amps[3];
    bool     clipped[3];
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

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_SNAPSHOTS_H
