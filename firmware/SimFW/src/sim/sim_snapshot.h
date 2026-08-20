// sim_snapshot -- the shared data-flow contract between sim_engine (sole
// writer) and every reader task (spi_emu_a/b, wave_owner, telemetry,
// cmd_task). docs/PLAN.md section 4.5 is authoritative for the shape:
// a double-buffered zone/TC/CT snapshot with torn-read retry, plus a
// fixed-size sequence-numbered event ring. This header is a FIXED
// CONTRACT shared by multiple independently-developed task modules --
// extend it by adding fields, never by renaming or repurposing existing
// ones, and coordinate before removing anything.
//
// Ownership (PLAN.md section 4 doctrine): sim_engine.c is the ONLY writer
// of sim_snapshot_t and the ONLY producer into the event ring. Every other
// task only ever calls sim_snapshot_read() / sim_event_ring_drain() --
// never touches the underlying storage directly.
//
// Still pure/host-testable: no FreeRTOS or pico-sdk types appear here.
// sim_engine.c (the FreeRTOS task) is expected to wrap the pure
// thermal_model_t/fault_engine_t state in one of these snapshots each
// tick and publish it via a double-buffer + sequence counter (readers
// retry if the sequence number changes across their read, per PLAN.md
// 4.5 -- "double-buffered with a sequence counter, readers retry on torn
// read, no locks on the hot path"). The exact double-buffer/publish
// mechanism is sim_engine.c's business; this header only defines the
// struct shape both sides agree on.
#ifndef SIMFW_SIM_SIM_SNAPSHOT_H
#define SIMFW_SIM_SIM_SNAPSHOT_H

#include <stdbool.h>
#include <stdint.h>

#include "thermal_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Matches thermal_model.h's zone cap and PLAN.md 3.1/4.3's "1-4 zones
 * (default 3)". */
#define SIM_SNAPSHOT_MAX_ZONES THERMAL_MODEL_MAX_ZONES

/* PLAN.md 3.4: five relay-sense channels (K1/K2/K3/K5 main-side, K4 safety
 * pilot). Bit order fixed here so every reader agrees without re-deriving
 * it -- do not renumber. */
typedef enum {
    SIM_RELAY_BIT_K1 = 0,
    SIM_RELAY_BIT_K2 = 1,
    SIM_RELAY_BIT_K3 = 2,
    SIM_RELAY_BIT_K5 = 3,
    SIM_RELAY_BIT_K4 = 4, /* safety pilot */
    SIM_RELAY_BIT_COUNT
} sim_relay_bit_t;

/* One zone's worth of published state. "True" is the thermal model's
 * ground truth (never corrupted); "reported" is what an emulated
 * MAX31856 channel should encode -- i.e. TC lag already applied by
 * sim_engine, corruption knobs NOT yet applied (spi_emu_a/b's own
 * max31856_regs_t instance owns fault/corruption state and layers it on
 * top of T_tc_reported_c when it builds its LTCB bytes; sim_snapshot
 * carries the clean, physically-truthful signal only). */
typedef struct {
    float T_true_c;          /* thermal_model_state_t.T_zone[i] verbatim */
    float T_tc_reported_c;    /* thermal_model_state_t.T_tc[i] (main-side lag) */
    float T_safety_reported_c;/* safety-side blend + its own lag, PLAN.md 4.3 */
    float current_a;          /* CT amplitude target this zone should drive,
                                * PLAN.md 3.3: I = V_mains/R_element * duty *
                                * element_health when the zone's relay chain
                                * conducts, else 0 */
} sim_zone_snapshot_t;

/* The full published snapshot. Sequence-counter protocol: sim_engine
 * increments seq to an ODD value before writing, writes every field,
 * then increments seq to the next EVEN value when done. A reader takes
 * two copies of seq (before and after copying the rest of the struct)
 * and retries the whole read if either copy is odd or the two copies
 * differ (PLAN.md 4.5's "double-buffered with a sequence counter" --
 * sim_snapshot_read() below implements exactly this so callers never
 * hand-roll it). */
typedef struct {
    uint32_t seq;

    uint64_t sim_time_us;     /* monotonic sim clock, PLAN.md 4.2 */
    uint32_t timescale_x100;  /* e.g. 1000 == 10.00x; 100 == 1.00x real time */

    uint8_t zone_count;       /* 1..SIM_SNAPSHOT_MAX_ZONES */
    sim_zone_snapshot_t zones[SIM_SNAPSHOT_MAX_ZONES];

    /* Bitmask over sim_relay_bit_t -- sim_engine's own idea of relay state
     * (fed FROM i2c_owner's debounced relay sense, PLAN.md 4.5's inbound
     * arrow "relay edges (i2c_owner) --> sim_engine"), republished here so
     * spi_emu/wave_owner/telemetry don't need to depend on i2c_owner
     * directly -- single point of truth for "what does the model think
     * the relays are doing right now." */
    uint16_t relay_mask;

    bool estop_open;
    bool dut_power_on;
} sim_snapshot_t;

/* Event catalog. PLAN.md 4.5's event ring carries relay edges, fault
 * fires/clears, threshold crossings, mode changes, DUT power switch,
 * protocol errors -- one enum value per PLAN.md 5.3's EVT frame
 * `event_type` field, so the wire encoding (owned by usb_owner/cmd_task)
 * can map 1:1 onto this list without a translation table. */
typedef enum {
    SIM_EVENT_RELAY_EDGE = 0,     /* a=sim_relay_bit_t, b=1 close/0 open */
    SIM_EVENT_FAULT_FIRED = 1,     /* a=fault slot id */
    SIM_EVENT_FAULT_CLEARED = 2,   /* a=fault slot id */
    SIM_EVENT_THRESHOLD_CROSSED = 3, /* a=zone, f0=value crossed */
    SIM_EVENT_MODE_CHANGED = 4,    /* a=target kind, b=MODEL(0)/MANUAL(1) */
    SIM_EVENT_DUT_POWER = 5,       /* b=1 on/0 off */
    SIM_EVENT_PROTOCOL_ERROR = 6,  /* a,b = implementation-defined error code */
} sim_event_type_t;

/* One ring entry. a/b/f0 are generic payload fields whose meaning depends
 * on `type` (documented per-value above) -- kept small and fixed-size so
 * the ring is a flat array, no per-event heap allocation. */
typedef struct {
    uint32_t seq;          /* monotonically increasing, PLAN.md 4.5/5.3:
                             * "sequence-numbered so the PC detects loss" */
    uint64_t sim_time_us;
    sim_event_type_t type;
    uint8_t a;
    uint8_t b;
    float f0;
} sim_event_t;

/* PLAN.md 4.5: "fixed-size (e.g. 256 entries)". Ring is owned/stored by
 * sim_engine.c; this constant is here (not private to that .c) because
 * readers may want to size their own drain buffers against it. */
#define SIM_EVENT_RING_SIZE 256u

/* Reader API -- implemented in sim_engine.c, called from any other task.
 * Both functions are safe to call from any task/core; sim_engine.c is
 * responsible for making them so (e.g. the sequence-counter protocol for
 * sim_snapshot_read, and whatever locking or lock-free scheme
 * sim_event_ring_drain needs for concurrent single-producer/
 * multi-consumer draining -- PLAN.md 4.5 says producers must never block,
 * consumers draining concurrently is an implementation detail sim_engine
 * owns). */

/* Copies the latest published snapshot into *out. Returns true once a
 * torn-free copy was obtained (retries internally); returns false only if
 * sim_engine has never published a snapshot yet (e.g. called before the
 * first tick). */
bool sim_snapshot_read(sim_snapshot_t *out);

/* Drains up to max_out events with seq >= *inout_next_seq, in seq order,
 * into out[]. Updates *inout_next_seq to one past the last event
 * returned (or leaves it unchanged if nothing new was available) so the
 * caller can poll incrementally without re-reading old events. Returns
 * the number of events written to out[]. If the ring wrapped past
 * *inout_next_seq before the caller could drain it (i.e. events were
 * lost), *inout_next_seq is advanced to the oldest still-available seq
 * and the caller can detect the gap by comparing the returned first
 * event's seq to the value it expected -- callers that care about loss
 * (PLAN.md 5.3: "the PC's report generator refuses to certify a run with
 * a sequence gap") must check for this themselves. */
uint32_t sim_event_ring_drain(sim_event_t *out, uint32_t max_out, uint32_t *inout_next_seq);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_SIM_SIM_SNAPSHOT_H
