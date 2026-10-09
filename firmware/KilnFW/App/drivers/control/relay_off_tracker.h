// relay_off_tracker -- RAM-only record of when each physical relay last went
// from ON to OFF, so the on/off min_off_s hold survives every reset of the
// per-run hold accumulators.
//
// Why: the executor's hold accumulators (decide-state held_s and the actuation
// gate's held_s, zone and aux) are reset at run start, at an aux disable/
// re-enable and at the run-end aux funnel. Resetting them to "already held
// long enough" is right for a relay that has not been ON since boot (the first
// ON of a run must not wait min_off_s), but it also dropped min_off_s
// protection for a relay that WAS just switched OFF: a mid-run aux
// disable/re-enable, or a stop followed by a quick restart, could re-close a
// contactor immediately. Every reset site now seeds its accumulators from
// this tracker instead: held = time since the last ON-to-OFF transition when
// one is on record, ON_OFF_HOLD_SETTLED_S when the relay has not been ON since
// boot, 0 while it is physically ON.
//
// Fed from every OFF write: kiln_io_owner's relay funnel (manual, bridge,
// authorized and all-off writes) and the executor's own zone/aux writes.
// A write that turns OFF a relay the tracker never saw ON records nothing.
// Never persisted: a power cycle gives a relay time to rest on its own.
#ifndef RELAY_OFF_TRACKER_H
#define RELAY_OFF_TRACKER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Records a relay write: for every bit in mask, value's bit is the new state.
// Call after the write succeeded.
void relay_off_tracker_note_write(uint8_t mask, uint8_t value);

// Seconds the relays in mask have been OFF, the minimum over the mask's bits
// (a multi-relay zone is held by its most recently opened relay). 0 if any
// bit is currently ON; ON_OFF_HOLD_SETTLED_S (on_off_trigger_decide.h) if no
// bit has been ON since boot or mask is 0.
float relay_off_tracker_held_s(uint8_t mask);

// Test seam: forget everything (as after a power cycle).
void relay_off_tracker_reset_all(void);

#ifdef __cplusplus
}
#endif

#endif // RELAY_OFF_TRACKER_H
