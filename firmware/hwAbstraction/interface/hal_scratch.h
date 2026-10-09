/* hal_scratch.h -- RP2040 watchdog scratch-register registry. Pico-only.
 * See docs/HW_ABSTRACTION.md "hal_scratch -- pico watchdog-scratch
 * registry".
 *
 * WHY THIS EXISTS. All 8 watchdog_hw->scratch[] registers are already
 * claimed today, poked directly by four separate files with no shared
 * ownership table -- see firmware/SaftyFW/src/startup_diag.h:13-28's own
 * budget comment, which is the only place the full map was written down
 * before this header. Real registers, real callers (re-verified against
 * every access site for this header):
 *   [0] trip reason            -- boot_reason.c (SAFTYFW_TRIP_REASON_SCRATCH)
 *   [1] trip reason magic      -- boot_reason.c (SAFTYFW_TRIP_MAGIC_SCRATCH)
 *   [2] startup diag           -- startup_diag.h (SAFTYFW_STARTUP_DIAG_SCRATCH),
 *                                  written main.c:439
 *   [3] startup diag magic     -- startup_diag.h (..._MAGIC_SCRATCH),
 *                                  written main.c:440
 *   [4] RESERVED -- pico-sdk's watchdog_enable() (hardware_watchdog/
 *                   watchdog.c:82) owns this register outright. Fenced by
 *                   comment only today (main.c:230-241); this header makes
 *                   the reservation a runtime-checked refusal instead
 *                   (claim/write/clear all reject slot 4 outright) --
 *                   not a compile-time check.
 *   [5] SHARED, two mutually-exclusive writers by construction (never both
 *       active in the same firmware build): watchdog_overdue_diag.c's
 *       overdue-checkin latch (tag 0xD9, watchdog_overdue_diag.c) and
 *       main.c's stack-overflow hook (tag 0xE3, main.c:127, written before
 *       the scheduler can schedule watchdog_overdue_diag's own writer --
 *       see main.c:84-89's own comment on why this is safe). Cross-
 *       documented in code at main.c:84-89.
 *   [6] boot-stage marker, overwrite-only -- main.c (no dedicated module;
 *       written directly at several boot_stage transition points).
 *   [7] CLEAR_TRIP crash checkpoint -- clear_trip_diag.c
 *       (CLEAR_TRIP_DIAG_SCRATCH), one packed word (magic tag + stage/
 *       reason/fault_bits/tc_valid/spi_failed/tc_c_is_nan/outcome bitfield,
 *       encoded/decoded by clear_trip_diag_codec.c -- this header does not
 *       touch that packing, only the raw scratch word beneath it).
 *
 * WHAT THIS HEADER OWNS: a runtime advisory claim registry (slot, owner
 * string, tag) plus typed read/write/clear accessors, and the hard slot-4
 * refusal. It does NOT own the packed encodings living inside slots
 * 2/3/5/7 (startup_diag_codec, watchdog_overdue_diag_codec,
 * clear_trip_diag_codec stay exactly where they are, pure and host-tested,
 * layered on top of hal_scratch_write_u32/read_u32 the same way
 * config_store's policy layer sits on top of hal_flash -- see that header's
 * identical split).
 *
 * REGISTRY / UNIQUENESS -- RUNTIME ONLY, NOT COMPILE-TIME. There is no
 * HAL_SCRATCH_CLAIM() macro and no compile-time table: a claiming module
 * calls hal_scratch_claim(slot, owner, tag) once, at boot, from ordinary
 * runtime code (mirroring where the four direct pokers already live), and
 * the backend maintains a small runtime array of accepted claims. A second
 * claim naming the same (slot, tag) pair is refused at that call
 * (HAL_INVALID_ARG) -- but only if and when both claims actually execute in
 * the same boot; nothing catches a collision at build time, and two
 * claiming modules that are never both linked into the same image are
 * never checked against each other at all. Slot 5 is the one deliberate
 * two-owner slot (co-owned by tag, not by slot number alone), expressed as
 * two claims sharing a slot but carrying distinct tags; the uniqueness
 * check keys on (slot, tag), not slot alone, so slot 5's two legitimate
 * co-owners register clean while a genuine slot collision (same slot, same
 * tag) does not.
 *
 * IMPORTANT: hal_scratch_write_u32()/hal_scratch_clear() do NOT consult the
 * claim table at all (beyond the slot-4 special case below) -- a write or
 * clear to any claimed-or-unclaimed slot 0-3/5-7 succeeds unconditionally.
 * The registry is advisory bookkeeping for a test or a future audit to walk
 * (see the host fake's own claim-table inspection helpers), not an
 * access-control layer; nothing here stops a module from writing a slot it
 * never claimed, or one another module claimed.
 *
 * SLOT 4 HARD RESERVATION. hal_scratch_claim()/hal_scratch_write_u32() must
 * refuse HAL_SCRATCH_SLOT_WATCHDOG_ENABLE (4) outright -- HAL_INVALID_ARG,
 * checked at the call, not just documented. Before this header, the
 * reservation was a comment only (main.c:235-241); making it a checked
 * refusal is this header's whole safety value per the plan
 * ("highest safety value, smallest diff").
 *
 * THE watchdog_reboot(pc, sp, delay) STOMP HAZARD. pico-sdk's
 * watchdog_reboot() writes scratch[4..7] whenever `pc != 0` (hardware_
 * watchdog/watchdog.c:90-112) -- it uses those four registers as its own
 * boot-vector handoff, silently overwriting the trip-checkpoint state slots
 * 5/6/7 hold (slot 4 is already off-limits to everything else). SaftyFW's
 * one real call site, update_task.c:998 (`watchdog_reboot(0, 0, 0)`),
 * passes pc == 0 today, so nothing is stomped in the current tree -- but
 * that safety is incidental to the call site, not structural. This header
 * makes it structural: hal_wdt.h's reboot wrapper (see that header) takes
 * NO pc/sp/delay_ms parameters at all, so the non-zero-pc path that would
 * stomp slots 5-7 is unreachable through the HAL, full stop -- a caller
 * cannot even ask for it. Any future genuine need for the pc/sp boot-vector
 * form must go through hal_scratch directly (a new, explicitly-named
 * escape hatch), never re-added to hal_wdt's plain reboot call.
 *
 * Threading/ownership contract:
 *  - Every slot's real writer runs once, at boot, before the scheduler
 *    starts (matching boot_reason.c/startup_diag.h/watchdog_overdue_diag.c/
 *    clear_trip_diag.c's existing single-writer-at-boot discipline) or, for
 *    slot 6's boot-stage marker, at a handful of well-known boot
 *    checkpoints on the one boot-time thread of execution. No slot is
 *    written concurrently from two tasks; this header does not add locking
 *    because none of the real call sites need it.
 *  - hal_scratch_read_u32() takes a magic word and reports validity via
 *    *magic_ok, matching the existing convention every one of the four
 *    direct pokers already uses (distinguishing "actually written this
 *    power cycle" from "reads zero because power-on left it
 *    uninitialised" -- boot_reason.c's own header comment states this
 *    explicitly, referencing docs/ARCHITECTURE.md section 8).
 */
#ifndef KILNCTL_HAL_SCRATCH_H
#define KILNCTL_HAL_SCRATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RP2040 has exactly 8 watchdog scratch registers (hardware_watchdog);
 * every slot is already claimed -- see the map in this header's own
 * top comment. */
#define HAL_SCRATCH_SLOT_COUNT 8u

/* Slot 4 is pico-sdk's own -- watchdog_enable() (hardware_watchdog/
 * watchdog.c:82) writes it unconditionally. hal_scratch refuses any claim
 * or direct write naming this slot; see this header's top comment. */
#define HAL_SCRATCH_SLOT_WATCHDOG_ENABLE 4u

/* Opaque tag identifying one claimant, for the (slot, tag) uniqueness rule.
 * 0 is reserved to mean "no tag distinction" (an ordinary single-owner
 * slot); slot 5's two real co-owners use 0xD9 and 0xE3, matching the tag
 * values already burned into main.c/watchdog_overdue_diag.c today so a
 * caller reading a live register back can still recognise which owner
 * wrote it. */
#define HAL_SCRATCH_TAG_NONE 0u

/* One row of the runtime claim registry. Backends build this table entirely
 * at runtime, from each claiming module's own hal_scratch_claim() call at
 * boot (matching every real site's own single-writer-at-boot pattern) --
 * there is no compile-time table and no HAL_SCRATCH_CLAIM() macro; see this
 * header's "REGISTRY / UNIQUENESS" note above for what that does and does
 * not catch. */
typedef struct {
    uint8_t     slot;   /* 0..HAL_SCRATCH_SLOT_COUNT-1 */
    const char *owner;  /* e.g. "boot_reason", "clear_trip_diag" */
    uint8_t     tag;    /* HAL_SCRATCH_TAG_NONE, or a co-owner's distinguishing tag (slot 5) */
} hal_scratch_claim_t;

/* Registers one (slot, owner, tag) claim. Refuses HAL_SCRATCH_SLOT_
 * WATCHDOG_ENABLE outright (HAL_INVALID_ARG) -- see this header's slot-4
 * reservation note. A second claim on a slot already claimed with the same
 * tag is also HAL_INVALID_ARG (a real collision); a second claim on the
 * same slot with a DIFFERENT tag is accepted -- that is exactly slot 5's
 * two legitimate co-owners. Intended to be called once per claiming module,
 * at boot, before that module's first read/write. */
hal_status_t hal_scratch_claim(uint8_t slot, const char *owner, uint8_t tag);

/* Typed accessors. Every real writer today writes one raw uint32_t per
 * slot (the packed encodings above this layer are call-site logic, exactly
 * like hal_flash's config_store split) -- so these three primitives cover
 * 100% of the observed access shapes; no bitfield-aware variant is added
 * here. */
hal_status_t hal_scratch_write_u32(uint8_t slot, uint32_t value);

/* Reads back the raw word AND validates it against `magic`: *magic_ok is
 * set true only if the word at `magic_slot` currently equals `magic` --
 * matching every existing call site's own "trust this register only if its
 * paired magic word matches" discipline (boot_reason.c, startup_diag.h,
 * clear_trip_diag.c). Slots with no magic pairing of their own (slot 6's
 * overwrite-only boot-stage marker) may pass the same slot for both
 * `slot` and `magic_slot` with `magic` set to any value the caller does not
 * care about, and ignore *magic_ok. */
hal_status_t hal_scratch_read_u32(uint8_t slot, uint32_t *out_value,
                                   uint8_t magic_slot, uint32_t magic,
                                   bool *magic_ok);

/* Zeroes one slot. Matches every existing *_clear() function's shape
 * (boot_reason_clear_trip, watchdog_overdue_diag_clear,
 * clear_trip_diag_clear all just zero their scratch word). Refuses
 * HAL_SCRATCH_SLOT_WATCHDOG_ENABLE like the write path above. */
hal_status_t hal_scratch_clear(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_SCRATCH_H */
