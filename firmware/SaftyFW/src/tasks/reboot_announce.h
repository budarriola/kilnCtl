// reboot_announce.h -- tiny producer/consumer fact for SAFETY_CMD_
// ANNOUNCE_REBOOT (0x18), KilnFW/TODO.md's "SAFETY_CMD_ANNOUNCE_REBOOT sent
// before the ESP reboots" line. link_task.c writes the local (Pico) uptime
// at which the last ANNOUNCE_REBOOT frame was received; safety_core.c reads
// it to compute S6b's bounded grace-window fact.
//
// Deliberately its OWN header, not part of link_task.h -- safety_core.c/.h
// must never #include a header whose path contains "link" or "uart"
// (docs/ARCHITECTURE.md section 2, enforced by tools/check_isolation.ps1's
// literal substring grep on #include lines), so this fact needs a neutral
// name to cross that boundary, same reason context_snapshot_t/thermo_
// snapshot_t/current_snapshot_t all live in snapshots.h rather than in the
// producing task's own header. reboot_announce_get() hands back a plain
// timestamp, not a bool -- SAFETY_MODEL.md's own discipline (see safety_
// guards.h's doc comment on link_up) is that a pure guard-evaluation module
// never does its own time arithmetic; the elapsed-time computation belongs
// to the caller (safety_core.c), which already owns to_ms_since_boot(get_
// absolute_time()) for its other fields. This header carries no opinion
// about what window length counts as "recent" -- that constant lives in
// safety_core.c, next to where the comparison happens.
#ifndef SAFTYFW_TASKS_REBOOT_ANNOUNCE_H
#define SAFTYFW_TASKS_REBOOT_ANNOUNCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called by link_task.c's link_task_handle_announce_reboot() the instant a
// well-formed ANNOUNCE_REBOOT frame is decoded, with the local (Pico)
// uptime_ms at that instant (to_ms_since_boot(get_absolute_time()), the same
// clock base link_task.c/safety_core.c already use for every other
// timestamp field -- never the ESP's own uptime, which this frame does not
// even carry). Safe to call from link_task's own thread only -- single
// writer, matching s_last_context_rx_tick's own documented convention in
// link_task.c.
void reboot_announce_mark(uint32_t now_ms);

// Returns true and fills *out_at_ms with the local uptime_ms of the most
// recent ANNOUNCE_REBOOT frame, or returns false (leaving *out_at_ms
// unmodified) if none has ever been received this boot. Safe to call from
// any task -- backed by a single volatile read of a value only link_task.c
// writes, same "no lock needed for one scalar" reasoning as discrete_task.h's
// discrete_task_estop_pressed()/discrete_task_main_fault().
bool reboot_announce_get(uint32_t *out_at_ms);

// Test/reset hook only -- host tests call this between cases so
// reboot_announce_get() starts each test "never announced" rather than
// leaking state from a previous test in the same process. Not used by any
// embedded call site.
void reboot_announce_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_REBOOT_ANNOUNCE_H
