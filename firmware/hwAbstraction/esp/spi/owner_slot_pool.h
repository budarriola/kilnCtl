// owner_slot_pool -- pure reference-count bookkeeping for a fixed-size pool
// of owner-task IPC result slots. Split out of kiln_io_owner.c/thermo_owner.c
// (both of which use it identically) so the one thing that actually needs
// proving -- that a slot is never handed back to the free pool while either
// side that was given it might still touch it -- can be host-tested directly,
// with no FreeRTOS/hardware stub layer, the same way
// firmware/SaftyFW/src/tasks/uart_owner_tx_policy.c pulls a pure decision out
// of a task loop that otherwise cannot run off-target.
//
// 2026-08-24 fix for the stack-lifetime bug in kiln_io_owner.c's/
// thermo_owner.c's post_and_wait(): both used to hand the owner task a
// pointer to a `owner_result_t` and a `SemaphoreHandle_t` that lived in the
// CALLING task's own stack frame, then gave up and returned after a bounded
// wait (KILN_IO_OWNER_WAIT_MS / THERMO_OWNER_WAIT_MS, 200ms) regardless of
// whether the owner task had actually finished. SX1509.c's own
// SX1509_LOCK_TIMEOUT_MS is 6000ms -- thirty times that client patience -- so
// a stalled I2C bus (exactly the condition this code exists to survive) let
// the client's stack frame get reused by its NEXT call while the owner task
// was still going to write through the old, now-dangling pointers: a
// cross-task write into whatever local variables or return address happened
// to occupy that stack slot afterward.
//
// The fix moves BOTH the result storage and the semaphore out of the
// caller's stack and into a small, fixed pool owned by the module itself
// (kiln_io_owner.c's/thermo_owner.c's own static `owner_slot_t s_slots[N]`,
// sized to the command queue depth -- see each file's own KILN_IO_OWNER_
// SLOT_COUNT/THERMO_OWNER_SLOT_COUNT). That alone already fixes the memory-
// safety half of the bug: static storage is never freed, so a late write
// from the owner task can never land on a stack frame that means something
// else by then. What is left is a SEMANTIC hazard, not a memory-safety one:
// if a freed slot were handed to a NEW command before the owner task
// finished writing the OLD command's late result into it, that late write
// would silently corrupt the new command's answer. This module exists to
// rule that out.
//
// INVARIANT: every slot returned by owner_slot_pool_alloc() starts held by
// BOTH parties that were given it -- the client that allocated it (which
// will either read the slot's result on success or give up on timeout) and
// the owner task (which will eventually process the command and write into
// the slot). Each side calls owner_slot_pool_release() EXACTLY ONCE, in
// either order:
//   - the client, right after it stops waiting -- whether that is because it
//     took the slot's "done" semaphore (success) or because its own
//     patience ran out (timeout);
//   - the owner task, right after it finishes writing the result and giving
//     the slot's semaphore.
// A slot is only actually returned to the free pool -- and only then may
// owner_slot_pool_alloc() hand it to a different command -- when the SECOND
// of those two release calls arrives, whichever side that turns out to be.
// This makes the order irrelevant: "client times out, owner finishes later"
// and "owner finishes, client times out a moment later" both end in exactly
// the same state (frees on the second call), and there is no window in
// between where the slot looks free while one side might still touch it.
//
// This header only defines the bookkeeping -- an array of small reference
// counts and two functions over it. It takes no lock itself: the real
// firmware wraps every call in its own module-owned mutex (see kiln_io_
// owner.c's/thermo_owner.c's s_slot_lock) since multiple client tasks and
// the one owner task all touch the same array; host tests exercise the pure
// functions directly, single-threaded, with no lock needed.
#ifndef DRIVERS_OWNER_SLOT_POOL_H
#define DRIVERS_OWNER_SLOT_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A freshly allocated slot is held by exactly two parties -- see this
 * header's top comment -- so its refcount starts at 2 and each release
 * ticks it down by one. */
#define OWNER_SLOT_POOL_HELD_BY_BOTH 2u

/* Scans refcounts[0..count) for a free slot (refcount == 0), reserves it
 * (sets its refcount to OWNER_SLOT_POOL_HELD_BY_BOTH), and returns its
 * index. Returns -1 if every slot is currently held by someone -- callers
 * treat that exactly like a full command queue: fail closed, do not block
 * waiting for one to free up (a command queue this size can never have more
 * than `count` commands in flight at once, so exhaustion here means the
 * pool is undersized relative to the queue it backs, not a transient
 * hiccup). */
int owner_slot_pool_alloc(uint8_t *refcounts, size_t count);

/* One side releases its hold on slot `index`. Returns true iff THIS call
 * brought the slot's refcount to zero -- i.e. this caller is the one
 * responsible for returning the slot to the free pool (draining any
 * semaphore "give" nobody collected, resetting the result storage, whatever
 * the real module needs before the slot can look brand new to the next
 * owner_slot_pool_alloc()). Returns false both when the OTHER side still
 * needs to release (refcount went from 2 to 1) and when `index` is out of
 * range or already fully released (refcount already 0) -- the latter is a
 * caller bug (a double release), and this pure function's only
 * responsibility on that path is to never underflow past zero, which would
 * otherwise wrap a uint8_t back up to 255 and make an already-free slot
 * look held again. */
bool owner_slot_pool_release(uint8_t *refcounts, size_t count, int index);

#ifdef __cplusplus
}
#endif

#endif // DRIVERS_OWNER_SLOT_POOL_H
