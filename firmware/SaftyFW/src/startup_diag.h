// startup_diag.h -- reset-surviving latches for the two failures that
// otherwise reboot this board silently once a second.
//
// Both live in RP2040 watchdog scratch registers, not in RAM, for one
// reason: they have to survive the reset they explain. A watchdog reset
// re-zeroes .bss, so a static variable holding "which task failed to start"
// is gone by the time anyone can read it, and this board has neither a log
// sink (Phase 8) nor a fitted console header on GP16/GP17 (TODO.md 0.5a) to
// have printed it on the way down. Scratch registers survive a watchdog
// reset and are readable over SWD at any time, including from inside a
// reboot loop, which is exactly the condition that needs diagnosing.
//
// Scratch register budget, so the next person adding one does not collide:
//   [0] trip reason        -- src/boot_reason.c
//   [1] trip reason magic  -- src/boot_reason.c
//   [2] startup diag       -- this file
//   [3] startup diag magic -- this file
//   [4] watchdog_enable()  -- pico-sdk, do not touch (see main.c step 3)
//   [5] last check-in mask -- this file
//   [6] [7] free
#ifndef SAFTYFW_STARTUP_DIAG_H
#define SAFTYFW_STARTUP_DIAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bitmask of tasks whose _start() returned false, written once by main()
// just before the scheduler runs. Bit positions are the
// watchdog_checkin_id_t values, so a set bit reads directly as "this
// check-in bit can never be set, and the feed gate can never close".
#define SAFTYFW_STARTUP_DIAG_SCRATCH       2u
#define SAFTYFW_STARTUP_DIAG_MAGIC_SCRATCH 3u

// watchdog_task has no check-in bit of its own (it is the feeder, not a
// participant), so its start failure gets a bit above every
// watchdog_checkin_id_t value. 31 is chosen to stay clear of the enum no
// matter how many check-ins are added later.
#define SAFTYFW_START_BIT_WATCHDOG_TASK 31u

// Distinguishes "main() wrote a zero because nothing failed" from "this
// register happens to read zero because nothing has written it this power
// cycle". Same magic-word discipline as boot_reason.c's trip latch, and a
// distinct word so a stale value from the other latch can never be mistaken
// for this one.
#define SAFTYFW_STARTUP_DIAG_MAGIC 0x53544152u // 'STAR'

// The check-in mask watchdog_task_fn() last observed, rewritten every
// period, OR'd with SAFTYFW_LAST_CHECKIN_WRITTEN. After a starvation reboot
// this holds the final pre-reset value: strip the flag bit and XOR with
// WATCHDOG_CHECKIN_ALL_MASK to get the bits that were missing.
//
// The flag bit is load-bearing, not decoration. Without it a reading of 0
// is ambiguous between the two most interesting cases -- "watchdog_task ran
// and saw no check-ins at all" and "watchdog_task never ran, so nothing has
// written this register since power-on" -- which are different faults with
// different fixes. With it, 0 means the latter and
// SAFTYFW_LAST_CHECKIN_WRITTEN alone means the former.
#define SAFTYFW_LAST_CHECKIN_MASK_SCRATCH 5u
#define SAFTYFW_LAST_CHECKIN_WRITTEN      (1u << 31)

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_STARTUP_DIAG_H
