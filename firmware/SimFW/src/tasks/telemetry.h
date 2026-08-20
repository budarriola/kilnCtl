// telemetry.h -- REAL BODY (docs/PLAN.md section 4.1 task map): "Periodic
// state frames to USB (temps, relay states, active faults, sim clock)."
// Owns no peripheral (single-owner-per-peripheral doctrine, PLAN.md section
// 4's opening paragraph) -- it reads the zone snapshot and event ring
// sim_engine publishes (src/sim/sim_snapshot.h), the instrumentation
// counters spi_emu_a/b and fault_sched expose, and the fault-line sense
// i2c_owner exposes, then hands frames to usb_owner; it never talks to USB
// CDC directly.
//
// Two unsolicited frame kinds ride the same benchproto BROADCAST channel,
// addressed from SIMFW_TASK_ID_EVT (cmd_ids.h; docs/PROTOCOL.md section 6
// says EVT is "unsolicited BROADCAST only... the actual outbound BROADCAST
// path is telemetry's own work") and disambiguated by a frame-kind byte 0
// (SIMFW_EVT_FRAME_KIND_* , cmd_ids.h) documented in docs/PROTOCOL.md
// section 6:
//   - TELEMETRY: periodic, default 2 Hz (PLAN.md section 5.3), rate settable
//     via telemetry_set_rate_hz() -- a future SYS or MODEL command lands
//     here once one exists.
//   - EVT: one frame per sim_engine event-ring entry, drained and forwarded
//     promptly (PLAN.md section 4.5/5.3: "unsolicited, immediate"; "nothing
//     ever blocks... a full queue toward them is a counted drop").
//
// Both frame kinds are sent via usb_owner_send_broadcast() (usb_owner.h) --
// a narrow addition to usb_owner made as part of this pass, see that
// header's own doc comment for why (usb_owner remains the CDC's sole
// owner; nothing here calls tud_cdc_write() or touches benchproto_link_t
// directly).
#ifndef SIMFW_TASKS_TELEMETRY_H
#define SIMFW_TASKS_TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// PLAN.md section 5.3: "default 2 Hz, rate settable."
#define TELEMETRY_DEFAULT_RATE_HZ 2u
// A future SYS/MODEL command validates against this before calling
// telemetry_set_rate_hz() -- 0 is rejected (would mean "never", which is
// what stopping the task would mean, not a rate); an upper bound keeps a
// pathological request from starving usb_owner's TX path or wave_owner/
// spi_emu_* on the same core (PLAN.md section 4.5's "nothing ever blocks"
// rule -- telemetry itself must stay a well-behaved, bounded producer).
#define TELEMETRY_MAX_RATE_HZ     50u

// Creates telemetry at SIMFW_PRIO_TELEMETRY, pinned to
// SIMFW_CORE_ELASTIC_PATH (task_priorities.h). Returns false if task
// creation failed.
bool telemetry_start(void);

// Sets the periodic TELEMETRY frame's rate. Takes effect at the start of
// the task's next period (a simple "read the current period, use it" loop --
// no queue needed, this is a single word write/read, safe from any task via
// a plain volatile per task_priorities.h/i2c_owner.h's own convention for
// single-word cross-task flags). Returns false if hz is 0 or exceeds
// TELEMETRY_MAX_RATE_HZ, leaving the previous rate in effect.
bool telemetry_set_rate_hz(uint32_t hz);
uint32_t telemetry_get_rate_hz(void);

// --- Instrumentation (read-only, mutex-free single-word reads like
// i2c_owner.h's own getters) -------------------------------------------------
// EVT/TELEMETRY frames are also embedded in the periodic TELEMETRY frame
// itself (docs/PROTOCOL.md section 6's byte layout) so loss is visible to
// any PC client without a separate query -- these getters exist for a
// future diagnostic command / host test to read the same counters directly.

// Count of sim_event_ring_drain() calls that observed a sequence gap
// (sim_snapshot.h: "the caller can detect the gap by comparing the
// returned first event's seq to the value it expected") -- PLAN.md 5.3:
// "the PC's report generator refuses to certify a run with a sequence gap."
uint32_t telemetry_get_evt_seq_gap_count(void);

// Count of BROADCAST sends (TELEMETRY or EVT frames) that
// usb_owner_send_broadcast() reported as failed (CDC TX path busy/short --
// PLAN.md 4.5's drop policy: "a full queue toward them is a counted drop
// plus event, never a stall"). Distinct from the seq-gap counter above:
// this one is "we tried to send N frames and M were lost on the wire out",
// the other is "the ring itself outran us before we ever tried."
uint32_t telemetry_get_send_drop_count(void);

// High-water mark of event-ring backlog telemetry has observed: the largest
// number of events drained from the ring in a single telemetry period since
// boot (sim_snapshot.h exposes no direct "current fill" reader, only a
// drain call, so this is the closest telemetry can measure without adding
// an API to sim_engine's own file -- PLAN.md 5.3 lists "event-ring
// high-water mark" as a telemetry-frame field, and this is that field's
// value).
uint32_t telemetry_get_evt_ring_high_water_mark(void);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_TELEMETRY_H
