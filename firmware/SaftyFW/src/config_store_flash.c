// config_store_flash.c -- the flash I/O and ARMED-check glue for
// config_store.h's pure record logic. Rebased (docs/HW_ABSTRACTION.md
// Phase 3 item 2, 2026-09-06) onto hal_flash.h: real flash access now goes
// through hal_flash_read()/hal_flash_erase()/hal_flash_program()/
// hal_flash_safe_execute() instead of pico-sdk's XIP_BASE pointer read /
// flash_range_erase() / flash_range_program() / flash_safe_execute()
// directly. This is what makes this file host-testable for the first time
// (see test/test_config_store_flash.c) -- the pico backend
// (hal_flash_pico.c) still wraps the exact same pico-sdk calls this file
// used to make itself; the host backend (fake_flash.c) models an in-memory
// sector image instead. No change to what is preserved: the ARMED gate
// (config_store_decide_write() against relay_owner_get_state()), the
// seq/CRC round-robin log, and the format-version REFUSE policy are all
// still expressed at this layer, untouched by the rebase -- only the flash
// primitive calls underneath moved.
//
// Region binding: this file's two hal_flash_region_t's are bound once,
// lazily, to the config store's A and B sectors -- base ==
// flash_layout.h's real SAFTYFW_CONFIG_STORE_FLASH_OFFSET /
// SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B, size == SAFTYFW_CONFIG_STORE_FLASH_
// SIZE for each (see ensure_region() below) -- so every offset used
// elsewhere in this file against one of them is 0-based within THAT sector,
// not a whole-device offset. Binding at the REAL flash_layout.h offsets
// (rather than 0 on host, matching pico) is deliberate: it lets this file be
// identical on both backends, at the cost of the host fake needing to be
// sized to accept those real offsets (see firmware/hwAbstraction/host/
// fake_flash.h's FAKE_FLASH_MAX_SIZE_BYTES comment, bumped for exactly this).
//
// --- A/B sectors (flash_endurance_review_2026-09-07.md R2) -----------------
//
// The ORIGINAL single-sector design's failure mode: config_store_write()'s
// 8th write into a sector must erase that whole sector before it can program
// slot 0 again (config_store_next_write_needs_erase()) -- and between that
// erase and the next successful program, the sector held ZERO valid copies
// of the safety config. A power cut there lost TC type, abs_max_temp_c, CT
// cal outright. Nothing about the CRC/seq scan itself was ever unsafe; the
// gap was structural -- there was only ever one place to look.
//
// The fix adds a second sector (B) and never lets there be a moment with no
// valid copy anywhere:
//
//   - WRITE: config_store_plan_write() (config_store.c, pure) decides, from
//     the currently-cached (sector, slot), whether the next write fits in
//     the SAME sector (7 out of 8 writes -- no erase, byte-identical to the
//     old behaviour) or must SWITCH to the other sector (the 8th write --
//     erase that other sector, then program its slot 0).
//   - ATOMICITY: a switch never touches the sector holding the current
//     record. config_store_write_cb() below is handed only ONE
//     hal_flash_region_t (`args.region`, the target sector) -- it has no
//     way to erase or reprogram the other one even if it wanted to. So at
//     every instant during the erase-then-program pair -- including a crash
//     mid-erase, mid-program, or anywhere between the two -- the sector that
//     was NOT targeted still holds its full, untouched, CRC-valid record.
//     There is no separate "which sector is active" pointer stored anywhere
//     to tear: see the next bullet.
//   - ARBITRATION: read_latest_or_default() reads BOTH sectors, every time,
//     and config_store_find_latest_multi_ex() (config_store.c, pure) keeps
//     the single highest-`seq`, CRC-valid record across all 16 slots. A
//     torn/interrupted write in the target sector simply fails its own CRC
//     check and is skipped, exactly like any other corrupt slot always was
//     -- the reader then finds the OTHER sector's still-valid, lower-seq
//     record instead, with no special-casing needed for "was this a
//     switch-in-progress". The corrupt/superseded sector is not logged as a
//     distinct case (it looks, and is treated, exactly like any other
//     partially-written slot config_store_unpack_ex() already understands).
//   - MIGRATION: sector A keeps its original offset (SAFTYFW_CONFIG_STORE_
//     FLASH_OFFSET, unmoved) and its original 8-slot append-only format
//     (unchanged) -- a board already running the old single-sector firmware
//     IS running "A/B with B still erased/empty" already, with no data to
//     move and no migration step to run. The first boot of this firmware on
//     such a board reads sector A's existing highest-seq record exactly as
//     before (config_store_find_latest_multi_ex() finds nothing valid in the
//     still-blank sector B, same as an ordinary fresh sector) and proceeds
//     unaffected; TC type and abs_max_temp_c are not defaulted or lost.
//   - WEAR: each sector now absorbs erases only half as often (an erase
//     happens once every 8 writes, alternating sectors), which is the free
//     side effect the endurance review named -- not the reason this exists.
//
// Caches the current record in a static, so config_store_get_tc_type()/
// config_store_is_calibration_missing() are cheap, lock-free reads for
// hot-path callers (main.c at boot, and eventually thermo_task) rather than
// re-scanning flash every call. config_store_boot_load() must run once,
// early in main()'s boot sequence, before anything reads the cache.
#include "config_store.h"
#include "discrete_pin_policy.h"

#include <stdio.h>
#include <string.h>

#include "hal_barrier.h"
#include "hal_flash.h"

#include "flash_layout.h" // bootloader/ -- SAFTYFW_CONFIG_STORE_FLASH_OFFSET/_SIZE
#include "max31856.h"      // MAX31856_TC_TYPE_K -- asserted to match CONFIG_STORE_DEFAULT_TC_TYPE
#include "tasks/console_uart.h" // console_uart_puts() -- the boot-time "record REJECTED" log line
#include "tasks/relay_owner.h" // relay_owner_get_state() -- the ARMED check

// Compile-time cross-check: config_store.h's CONFIG_STORE_DEFAULT_TC_TYPE is
// duplicated as a literal rather than #including max31856.h (see
// config_store.h's comment on why), so this catches the two ever drifting
// apart instead of silently defaulting to the wrong type.
typedef char config_store_default_tc_type_matches_max31856
    [(CONFIG_STORE_DEFAULT_TC_TYPE == MAX31856_TC_TYPE_K) ? 1 : -1];

// Same cross-check pattern for the other tc_type literal config_store.h
// duplicates: CONFIG_STORE_TC_TYPE_MAX_REAL must track MAX31856_TC_TYPE_T
// (the highest real, linearized thermocouple type) or config_store_unpack()'s
// voltage-mode clamp would silently protect the wrong boundary.
typedef char config_store_tc_type_max_real_matches_max31856
    [(CONFIG_STORE_TC_TYPE_MAX_REAL == MAX31856_TC_TYPE_T) ? 1 : -1];

// config_store.h's CONFIG_STORE_FLASH_RC_* literals used to be cross-checked
// here at compile time against pico/error.h's `enum pico_error_codes`
// directly. That check was deleted, not moved: no caller feeds a raw pico rc
// to config_store_flash_rc_reason() any more, so numeric agreement with the
// SDK enum is no longer a requirement anywhere. This file only sees the
// hal_status_t hal_flash_safe_execute() returns, translated to a
// CONFIG_STORE_FLASH_RC_* value by hal_status_to_config_store_flash_rc()
// below so config_store_flash_rc_reason()'s existing three named strings
// keep working unchanged.
static hal_status_t hal_status_to_config_store_flash_rc(hal_status_t status, int *out_rc)
{
    switch (status) {
        case HAL_OK:
            *out_rc = CONFIG_STORE_FLASH_RC_OK;
            break;
        case HAL_TIMEOUT:
            *out_rc = CONFIG_STORE_FLASH_RC_TIMEOUT;
            break;
        case HAL_NOT_READY:
            *out_rc = CONFIG_STORE_FLASH_RC_NOT_PERMITTED;
            break;
        default:
            // Catch-all, matching hal_flash_pico.c's own HAL_IO default case
            // ("PICO_ERROR_INSUFFICIENT_RESOURCES and anything not named
            // above") -- symmetric with that mapping so the round trip
            // through hal_status_t loses no distinction config_store_flash_
            // rc_reason() itself makes.
            *out_rc = CONFIG_STORE_FLASH_RC_INSUFFICIENT_RESOURCES;
            break;
    }
    return status;
}

// Lazily bound the first time either read_latest_or_default() or
// config_store_write() needs it -- see the file header comment above for
// what this region covers and why the offset is the same on both backends.
//
// A/B sectors (flash_endurance_review_2026-09-07.md R2): s_region is sector
// A (the legacy single sector, unmoved -- SAFTYFW_CONFIG_STORE_FLASH_OFFSET),
// s_region_b is sector B (flash_layout.h's new
// SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B, immediately following it in the same
// 64K reserved region). Indexed as s_regions[0]/[1] wherever code needs to
// pick "the sector config_store_plan_write()/find_latest_multi_ex() named",
// so the sector-index values those pure functions return map directly to an
// array index here with no translation.
static hal_flash_region_t s_region;
static hal_flash_region_t s_region_b;
static hal_flash_region_t *s_regions[SAFTYFW_CONFIG_STORE_NUM_SECTORS];
static bool s_region_ready = false;

static bool ensure_region(void)
{
    if (s_region_ready) {
        return true;
    }
    if (hal_flash_region_init(&s_region, SAFTYFW_CONFIG_STORE_FLASH_OFFSET,
                               SAFTYFW_CONFIG_STORE_FLASH_SIZE) != HAL_OK) {
        return false;
    }
    if (hal_flash_region_init(&s_region_b, SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B,
                               SAFTYFW_CONFIG_STORE_FLASH_SIZE) != HAL_OK) {
        return false;
    }
    s_regions[0] = &s_region;
    s_regions[1] = &s_region_b;
    s_region_ready = true;
    return true;
}

static config_store_record_t s_cached_record;

// --- Seqlock for s_cached_record (2026-09-09) -------------------------------
//
// Why: config_store_write() (called only from link_task, pinned to
// SAFTYFW_CORE_LINK_PATH / core 0) updates s_cached_record with a plain
// struct assignment; every getter below (config_store_get_full_record(),
// config_store_get_tc_type(), etc.) reads it with no synchronisation at
// all, and those getters are called from safety_core/thermo_task/
// current_task, all pinned to SAFTYFW_CORE_TRIP_PATH / core 1 -- including
// the guard/trip path. hal_flash_safe_execute() only fences both cores
// during the actual erase/program; `s_cached_record = to_write;` itself
// runs AFTER that fence is released, fully unguarded. Confirmed live, not
// theoretical: `safety_config_version` was observed moving 131 -> 132
// (config_crc changing under it) while a heating run was reading guard
// thresholds from this same cache.
//
// The owner-chosen fix is a seqlock, specifically BECAUSE the trip path
// must never block waiting on the writer: the writer bumps a counter to
// odd before touching the struct and to the next even value after: readers
// snapshot the counter, copy the struct, then re-check the counter --
// retrying if it changed (writer overlapped the copy) or was odd (writer
// mid-update). No reader ever waits on the writer, and the writer never
// waits on a reader.
//
// s_seq_counter must be volatile (stop the compiler reordering/caching its
// own accesses) AND paired with HAL_DMB() (stop the CPU/bus reordering
// what either core's OTHER core observes) -- volatile alone is a compiler-
// level guarantee only, and this is a cross-core, not just cross-task,
// hazard on RP2040's two independent Cortex-M0+ cores. See hal_barrier.h
// for why a barrier is needed here at all and which backend is used.
static volatile uint32_t s_seq_counter = 0u; // even == stable, odd == write in progress

// Bounded retry count for config_store_seqlock_read() below. The trip path
// (S1/S6a/etc via safety_core.c) must never spin unboundedly on a writer
// that is, by construction, a rare, deliberate, ARMED-refused-anyway
// commissioning commit -- so this is small on purpose, not tuned against
// any measured worst case.
#define CONFIG_STORE_SEQLOCK_MAX_RETRIES 4u

// Fallback used when config_store_seqlock_read() exhausts its retries (see
// that function's own comment for why a value one commit "behind" is safe
// for a guard threshold specifically).
//
// FIX (2026-09-09, opus review of b202fe56/5671ee03): this was originally a
// single s_last_good_record struct that EVERY successful reader -- on
// EITHER core -- wrote to right after taking its own stable copy. That
// premise ("only ever touched from inside config_store_seqlock_read(),
// which already holds a stable, freshly-copied struct") is true of the
// WRITE's *source*, but says nothing about the write's *destination*:
// s_last_good_record itself was unsynchronised, so a core-1 trip-path
// reader landing on the exhausted-retries path could read it while a
// core-0 reader was mid-write to that same ~512 B struct -- a torn read on
// the one path this whole seqlock exists to protect. Multiple concurrent
// readers were never exercised by the original host test (single reader
// thread), which is how this got through.
//
// Fix shape: make the fallback WRITER-owned, so no reader ever writes to
// it -- readers only ever read config_store_seqlock_write()'s output, never
// each other's. It is a double buffer, not a single struct.
//
// SECOND FIX (2026-09-09, opus review of the fix above): the paragraph this
// replaces claimed "whichever buffer a reader is pointed at by
// s_fallback_active is, by construction, never being written while that
// reader is copying it" -- true for ONE writer commit during a reader's
// copy, but false across TWO. Scenario: reader latches idx==0 (buffer 0
// active), begins copying s_fallback_buf[0], and is preempted. Writer
// commit #1 targets the OTHER slot (1), then flips active to 1. Writer
// commit #2 now targets fb_other = 1 - 1 = 0 -- the exact slot the
// suspended reader is still mid-copy of -- and flips active back to 0. The
// reader resumes and finishes copying a struct that was torn by commit #2,
// and a naive "is s_fallback_active still == idx?" recheck would not catch
// it either: the index legitimately returned to the same value (classic
// ABA), even though the buffer underneath was overwritten in between.
//
// A bare 0/1 index cannot be recheckable against a torn read of the slot it
// names; a monotonically-increasing generation counter can, because a
// SECOND writer commit during a reader's window necessarily increments it
// (there is no way for the generation counter itself to return to a
// previously-seen value the way the 0/1 index can), so a mismatch is a
// reliable signal to retry no matter how many commits happened during the
// read.  This is a seqlock over the fallback double buffer, using exactly
// the same protocol as s_seq_counter/s_cached_record above -- odd means
// "writer touching the fallback buffer/index right now", even means
// stable -- with its own small bounded retry count (writes here are rare
// commissioning commits, same rationale as CONFIG_STORE_SEQLOCK_MAX_RETRIES
// above). If even this second-level retry is exhausted (only reachable
// under a pathologically fast, continuous stream of writer commits -- never
// the real ARMED-refused-anyway commissioning-commit shape this store
// actually sees), config_store_seqlock_read() reports "no stable snapshot"
// exactly like the never-loaded case, and every caller already has a safe
// default for that (see each getter below) -- fail closed, never hand out a
// possibly-torn struct.
static config_store_record_t s_fallback_buf[2];
static volatile uint32_t s_fallback_active = 0u; // index into s_fallback_buf currently stable/readable
static volatile uint32_t s_fallback_gen = 0u; // even == stable, odd == fallback buffer/index write in progress
static bool s_fallback_valid = false; // see config_store_boot_load(): seeded true at boot once a
                                       // confirmed-safe boot record exists, not left false until
                                       // the first commissioning write (opus review finding B)
#define CONFIG_STORE_FALLBACK_SEQLOCK_MAX_RETRIES 4u

// Test-only instrumentation (opus review 2026-09-09): the multi-reader race
// test asserted zero torn reads but had no way to prove the exhausted-
// primary-retries fallback path in config_store_seqlock_read() ever actually
// ran -- "0 torn" is equally consistent with "the fallback was exercised and
// never tore" and "the fallback was never reached at all", and only the
// former is evidence the test claims to be. Incremented every time a reader
// falls through to the fallback branch below; a test asserting this is
// non-zero is the difference between a real regression test and a vacuous
// one. Not gated behind a test-only build flag: a plain counter increment is
// negligible cost even in production, and keeping it live means it is always
// available for a future diagnostics hook too.
static volatile uint32_t s_fallback_taken_count = 0u;

uint32_t config_store_test_fallback_taken_count(void)
{
    return s_fallback_taken_count;
}

void config_store_test_fallback_taken_count_reset(void)
{
    s_fallback_taken_count = 0u;
}

// TEST-ONLY deterministic race injection (2026-09-09): the real ABA window
// this fix closes is a handful of instructions wide on real hardware, and
// proved impractical to hit reliably even with real, heavily-loaded OS
// threads on a fast host (the multi-reader race test above still only
// demonstrates "no torn read seen in this run", never a guarantee the
// narrowest interleaving was tried). This hook is called, if set, from
// inside config_store_seqlock_read()'s fallback branch AFTER the index has
// been read but BEFORE the struct copy -- exactly where a real writer
// commit landing "during" the copy would have to land. A test can install a
// hook that itself calls config_store_write() (single-threaded, fully
// deterministic) to force exactly the two-commits-during-one-copy scenario
// the fix above exists to survive. NULL (a single branch on a well-predicted
// pointer) in production; never wired to anything at runtime outside tests.
static void (*s_fallback_test_hook)(void) = NULL;

void config_store_test_set_fallback_hook(void (*hook)(void))
{
    s_fallback_test_hook = hook;
}

// TEST-ONLY (2026-09-09): forces config_store_seqlock_read() straight past
// the primary seqlock into the fallback branch below, skipping its retry
// loop entirely. A single-threaded test has no concurrent writer to make
// the primary loop actually fail (it always sees a stable, even sequence
// immediately), so this is the only way to deterministically reach and
// exercise the fallback path's own ABA-closing seqlock without real thread
// races. False (no effect) in production.
static bool s_fallback_test_force = false;

void config_store_test_force_fallback_path(bool force)
{
    s_fallback_test_force = force;
}

// TEST-ONLY (2026-09-09): resets the fallback double buffer's state
// (validity, active index, generation counter, and both slots) back to its
// true cold-boot condition. Needed because these are static globals that,
// on real hardware, are reinitialised by a genuine power cycle but on this
// host test binary otherwise persist for the life of the whole test
// executable -- without this, a test running after ANY earlier test in the
// same binary has already called config_store_write() would find
// s_fallback_valid already true from that earlier call, masking a
// regression in config_store_boot_load()'s own seeding of it (opus review
// finding B; see test_fallback_seeded_at_boot_before_any_write()).
void config_store_test_reset_fallback_state(void)
{
    memset(s_fallback_buf, 0, sizeof(s_fallback_buf));
    s_fallback_active = 0u;
    s_fallback_gen = 0u;
    s_fallback_valid = false;
    s_fallback_taken_count = 0u;
}

// Snapshot s_cached_record into *out under the seqlock above, retrying up
// to CONFIG_STORE_SEQLOCK_MAX_RETRIES times if the writer is (or was)
// concurrently updating it. Returns true and fills *out on success --
// either a freshly stable snapshot, or (retries exhausted) the last
// snapshot this function ever confirmed stable. Returns false only when
// NEITHER a fresh nor a fallback snapshot exists yet (nothing has been read
// since boot) -- callers already handle that the same way they handle
// !s_loaded.
//
// Safety of the exhausted-retries fallback for a guard threshold
// specifically: a value one commissioning write "behind" is still a value
// that was fully committed and passed config_params_validate_ranges() --
// it is stale by at most one write, never torn/nonsensical, and the write
// that could make it stale is itself rare (an ARMED-refused-anyway
// commissioning commit, not a hot-path event). Blocking the trip path
// until the writer finishes, or handing it a torn struct, are both worse
// than reading a threshold that is briefly one write old.
static bool config_store_seqlock_read(config_store_record_t *out)
{
    for (unsigned attempt = 0;
         !s_fallback_test_force && attempt < CONFIG_STORE_SEQLOCK_MAX_RETRIES; attempt++) {
        uint32_t seq1 = s_seq_counter;
        // Barrier: this core must not read the struct below using a stale
        // cached view taken BEFORE it observed seq1 -- forces "read seq1"
        // to actually complete, as seen by this core, before the struct
        // read that follows.
        HAL_DMB();
        if (seq1 & 1u) {
            continue; // writer is mid-update -- retry rather than read torn data
        }
        config_store_record_t copy = s_cached_record;
        // Barrier: the struct copy above must be complete, as observed by
        // this core, before the re-read of the counter below -- otherwise
        // the CPU could reorder the seq2 read ahead of (part of) the struct
        // copy, defeating the whole "did the writer move during my copy"
        // check.
        HAL_DMB();
        uint32_t seq2 = s_seq_counter;
        if (seq1 == seq2) {
            *out = copy;
            return true;
        }
        // seq changed between the two reads (or is now odd): the copy may
        // be torn. Retry.
    }
    if (s_fallback_valid) {
        // Reader-side counterpart of the writer-owned double buffer above,
        // now its own seqlock (see s_fallback_gen's comment above for why a
        // bare index recheck cannot catch two writer commits landing during
        // one reader's copy -- classic ABA). Bounded retries, same rationale
        // as the primary seqlock's CONFIG_STORE_SEQLOCK_MAX_RETRIES.
        for (unsigned fb_attempt = 0; fb_attempt < CONFIG_STORE_FALLBACK_SEQLOCK_MAX_RETRIES;
             fb_attempt++) {
            uint32_t gen1 = s_fallback_gen;
            // Barrier: this core must not read the index/buffer below using a
            // stale cached view taken before it observed gen1.
            HAL_DMB();
            if (gen1 & 1u) {
                continue; // writer is mid-update of the fallback buffer -- retry
            }
            uint32_t idx = s_fallback_active;
            // Barrier: idx must be observed before the struct copy below --
            // otherwise the CPU could hoist part of the copy ahead of the
            // index read.
            HAL_DMB();
            // Copy split into two halves with the TEST-ONLY hook (see its
            // own comment above) run in between, purely so a test can
            // deterministically land a writer commit (or two, for ABA)
            // WHILE this copy is in progress -- exactly the window a real
            // concurrent writer would have to hit, rather than merely
            // before or after it. Functionally equivalent to one whole-
            // struct copy when no hook is installed (the production case).
            config_store_record_t copy;
            uint8_t *copy_bytes = (uint8_t *)&copy;
            const uint8_t *src_bytes = (const uint8_t *)&s_fallback_buf[idx];
            size_t split_at = offsetof(config_store_record_t, tc_type) <
                                       offsetof(config_store_record_t, estop_active_level)
                                   ? (offsetof(config_store_record_t, tc_type) +
                                      offsetof(config_store_record_t, estop_active_level)) /
                                         2u
                                   : sizeof(copy) / 2u;
            memcpy(copy_bytes, src_bytes, split_at);
            if (s_fallback_test_hook != NULL) {
                s_fallback_test_hook(); // TEST-ONLY, see its own comment above
            }
            memcpy(copy_bytes + split_at, src_bytes + split_at, sizeof(copy) - split_at);
            // Barrier: the struct copy above must be complete, as observed by
            // this core, before the re-read of the generation counter below.
            HAL_DMB();
            uint32_t gen2 = s_fallback_gen;
            if (gen1 == gen2) {
                *out = copy;
                s_fallback_taken_count++;
                return true;
            }
            // Generation moved (or is now odd): one or more writer commits
            // landed during this copy -- possibly into the very slot `idx`
            // named, even if s_fallback_active has since returned to a
            // value equal to idx again (ABA). Retry rather than trust it.
        }
        // Exhausted even the fallback's own retries -- only reachable under
        // continuous writer activity far outside this store's real usage
        // pattern (rare, deliberate, ARMED-refused-anyway commissioning
        // commits). Fail closed: report "no stable snapshot" rather than
        // risk handing out a torn struct; every caller already has a safe
        // default for exactly this return value.
    }
    return false; // never had a stable snapshot -- caller's !s_loaded path applies
}

// Writer-side counterpart to config_store_seqlock_read() above: bump the
// counter to odd, write the struct, bump to the next even value. Called
// only from config_store_write(), which is itself only ever reached from
// link_task (core 0) -- there is exactly one writer, so this needs no
// writer-side mutual exclusion of its own, only the barriers that make the
// update visible to READERS on the other core in the right order.
static void config_store_seqlock_write(const config_store_record_t *rec)
{
    uint32_t seq = s_seq_counter;
    s_seq_counter = seq + 1u; // odd: tell readers a write is in progress
    // Barrier: the struct write below must not be reordered/hoisted ahead
    // of the counter going odd, as observed by another core -- otherwise a
    // reader could see the OLD (even) counter value while already reading
    // partially-updated struct bytes.
    HAL_DMB();
    s_cached_record = *rec;
    // Barrier: the struct write above must be complete, as observed by
    // another core, before the counter is published as even again --
    // otherwise a reader could see "even" and trust a struct that has not
    // actually finished landing in SRAM from this core's perspective.
    HAL_DMB();
    s_seq_counter = seq + 2u; // even again: stable, safe for readers

    // Writer-owned fallback double buffer (see s_fallback_buf's comment
    // above): commit *rec into the slot NOT currently marked active, then
    // flip the index -- now wrapped in its own seqlock (s_fallback_gen) so
    // a reader that catches TWO of these commits during one copy can
    // detect it (see s_fallback_gen's comment for the ABA this closes).
    // This function remains the sole writer (link_task, core 0 only) -- no
    // writer-side mutual exclusion is needed, only publishing the update to
    // readers on the other core in the right order, same as the primary
    // seqlock above.
    uint32_t fb_gen = s_fallback_gen;
    s_fallback_gen = fb_gen + 1u; // odd: fallback buffer/index write in progress
    HAL_DMB();
    uint32_t fb_idx = s_fallback_active;
    uint32_t fb_other = 1u - fb_idx;
    s_fallback_buf[fb_other] = *rec;
    // Barrier: the buffer write above must be complete, as observed by
    // another core, before the index flip below is published -- otherwise
    // a reader could see the new index and copy a fallback slot that has
    // not actually finished landing in SRAM yet.
    HAL_DMB();
    s_fallback_active = fb_other;
    // Barrier: the index flip above must be complete, as observed by
    // another core, before the generation counter is published as even
    // again -- otherwise a reader could see "even" and trust an index/
    // buffer pair that has not actually finished landing in SRAM yet.
    HAL_DMB();
    s_fallback_gen = fb_gen + 2u; // even again: stable, safe for readers
    s_fallback_valid = true;
}

static size_t s_cached_slot = CONFIG_STORE_NO_SLOT;
// Which of s_regions[0]/[1] (sector A/B) s_cached_slot indexes into. Only
// meaningful once s_cached_slot != CONFIG_STORE_NO_SLOT; config_store_plan_
// write() ignores it entirely in the NO_SLOT case (starts fresh at sector 0),
// same "don't trust a stale index against a sentinel" discipline as
// s_cached_slot's own callers already follow.
static size_t s_cached_sector = 0;
static bool s_loaded = false;
// True iff the sector held a structurally-intact (magic/CRC/format_version
// all valid) record that config_params_validate_ranges() refused, and no
// OTHER slot in the sector was good -- config_store.h's "Load-time rejection
// diagnostics" case 2, distinct from an ordinary never-committed board (case
// 1, this stays false). See config_store_is_config_rejected()'s own comment.
static bool s_load_rejected = false;

// Module-scope, not a local: SAFTYFW_CONFIG_STORE_FLASH_SIZE is 4096 bytes
// (one erase sector, bootloader/flash_layout.h), and read_latest_or_default()'s
// only caller, config_store_boot_load(), runs from main.c's boot sequence
// step 4 -- before vTaskStartScheduler() -- on core0's pre-scheduler boot
// stack. That stack is pico-sdk's default PICO_STACK_SIZE (0x800 == 2048
// bytes; not overridden anywhere in this project's CMakeLists.txt), so a
// 4096-byte local array here would be a guaranteed overflow of the ENTIRE
// boot stack on its own, independent of whatever else that stack frame
// holds -- not merely tight against a margin baseline. config_store_boot_load()
// is documented (config_store_flash.c's own header comment, "must run once,
// pre-scheduler") to run exactly once before any task exists, so there is no
// concurrent caller to serialize against; a single static buffer is safe.
static uint8_t s_read_sector[SAFTYFW_CONFIG_STORE_NUM_SECTORS][SAFTYFW_CONFIG_STORE_FLASH_SIZE];

// Reads BOTH sectors and hands them to config_store_find_latest_multi_ex(),
// which is the actual arbiter (config_store.h's "A/B sector arbitration"
// comment) -- this function is now just the flash-I/O half of that: no
// erase/switch state lives here, only whatever the two sectors' own bytes
// say right now. `*out_sector` is filled with which sector the winning
// record came from (0 or 1), untouched on a CONFIG_STORE_NO_SLOT return.
static size_t read_latest_or_default(config_store_record_t *out_rec,
                                      size_t *out_sector,
                                      config_store_reject_info_t *out_reject)
{
    // ensure_region()/hal_flash_read() failing here is treated the same as
    // an unreadable/blank sector always was: "a missing part must not abort
    // boot" (max31856_configure()'s own doc comment) applies to
    // configuration exactly as much as to a missing sensor -- fall back to
    // config_store_default() rather than propagate the failure. A read
    // failure on EITHER sector is treated the same way -- there is no
    // partial-arbitration path that trusts one sector's bytes while
    // distrusting the other's read status.
    if (!ensure_region() ||
        hal_flash_read(&s_region, 0, s_read_sector[0], sizeof(s_read_sector[0])) != HAL_OK ||
        hal_flash_read(&s_region_b, 0, s_read_sector[1], sizeof(s_read_sector[1])) != HAL_OK) {
        config_store_default(out_rec);
        return CONFIG_STORE_NO_SLOT;
    }
    const uint8_t *sectors[SAFTYFW_CONFIG_STORE_NUM_SECTORS] = {s_read_sector[0], s_read_sector[1]};
    size_t sector_index = 0;
    size_t latest = config_store_find_latest_multi_ex(sectors, &sector_index, out_rec, out_reject);
    if (latest == CONFIG_STORE_NO_SLOT) {
        config_store_default(out_rec);
    } else {
        *out_sector = sector_index;
    }
    return latest;
}

// Reads the config store once, at boot, before the scheduler/other tasks
// start (same "call this before anything reads the cache" contract as
// spi_owner_init()/relay_owner_start() have in main.c's boot sequence). On a
// blank or corrupt sector this leaves the cache holding config_store_default()
// -- "a missing part must not abort boot" (max31856_configure()'s own doc
// comment) applies to configuration exactly as much as to a missing sensor.
//
// 2026-08-27 fail-open fix: that fallback is silent and correct for an
// ordinary fresh board, but WRONG to leave silent when the sector instead
// held a committed record that config_params_validate_ranges() just refused
// -- see config_store.h's "Load-time rejection diagnostics" block comment.
// That case gets a loud console_uart_puts() line naming the specific field
// and rule that failed (not just "validation failed" -- someone has to debug
// this from a log line alone, on a board with no other output channel this
// early in boot), and s_load_rejected latches so config_store_is_config_
// rejected() can report it to any later consumer. console_uart_puts() is
// safe to call here: console_uart_init() already ran (main.c step ~2, well
// before this function's own call site at step 4) and this is still
// pre-scheduler, the same context console_uart.h's own header comment
// documents as safe.
void config_store_boot_load(void)
{
    config_store_reject_info_t reject_info;
    memset(&reject_info, 0, sizeof(reject_info));

    s_cached_slot = read_latest_or_default(&s_cached_record, &s_cached_sector, &reject_info);
    s_load_rejected = (s_cached_slot == CONFIG_STORE_NO_SLOT) && reject_info.rejected;

    // Opus review finding B (2026-09-09): seed the writer-owned fallback
    // buffer from the boot-time record instead of leaving s_fallback_valid
    // false until the first-ever config_store_write(). Before this, a
    // core-1 reader that exhausted the primary seqlock's retries during the
    // window between boot and the first commissioning write got `false`
    // back from config_store_seqlock_read() and fell through to each
    // getter's own accessor default (calibration_missing=true, etc) instead
    // of the perfectly good record this function just loaded and validated
    // -- a real availability regression (nuisance-trip direction only,
    // since every one of those defaults is itself fail-safe) with no
    // matching justification: s_cached_record here is exactly as trustworthy
    // as any later config_store_write() commit (same validation path,
    // config_store_find_latest_multi_ex()/config_params_validate_ranges()
    // via read_latest_or_default() above, or config_store_default() when
    // nothing was ever committed). No concurrent reader exists yet at this
    // point (config_store_boot_load() runs pre-scheduler, single core, same
    // contract this function's header comment already documents), so a
    // plain assignment -- not the seqlock -- is correct and sufficient here.
    s_fallback_buf[0] = s_cached_record;
    s_fallback_active = 0u;
    s_fallback_valid = true;

    if (s_load_rejected) {
        // Two calls, not one. The single formatted line this replaced did not
        // fit: -Wformat-truncation proved at compile time that the fixed
        // consequence text alone needed 167 bytes of a buffer with 111-113
        // left after the field and rule strings, so the part an operator most
        // needs -- what the board is now DOING about it -- is exactly the part
        // that would have been cut off. The consequence text is a constant, so
        // it does not belong in a format buffer at all.
        char line[256];
        // %s on a possibly-NULL field/rule can't happen here: config_store_
        // unpack_ex() only ever sets rejected == true alongside non-NULL
        // field/rule (config_store.c's two RANGE-check call sites always
        // pass real out_field/out_rule pointers to config_params_validate_
        // ranges()), but "?" is printed instead of trusting that invariant
        // silently, matching this codebase's general preference for a
        // defensive fallback over an unverified assumption -- see e.g.
        // config_store.c's REC_OFF_SAFETY_TC_INSTALLED comment for the same
        // discipline applied to a wire byte instead of a pointer.
        snprintf(line, sizeof(line),
                 "SaftyFW: config_store REJECTED a committed record at load "
                 "(seq=%lu): field '%s' -- %s\r\n",
                 (unsigned long)reject_info.seq, reject_info.field ? reject_info.field : "?",
                 reject_info.rule ? reject_info.rule : "?");
        console_uart_puts(line);
        console_uart_puts("SaftyFW: falling back to compiled defaults: "
                          "abs_max_temp_c=0 (S1 will NOT trip until recommissioned), "
                          "calibration_missing stays true, config_crc reports 0 "
                          "(UNCOMMISSIONED).\r\n");
    }

    s_loaded = true;
}

bool config_store_is_config_rejected(void)
{
    return s_loaded && s_load_rejected;
}

uint8_t config_store_get_tc_type(void)
{
    if (!s_loaded) {
        // Defensive: a caller that runs before config_store_boot_load() gets
        // the safe default rather than uninitialised/zeroed memory -- same
        // "safe defaults over silence" discipline the rest of this module
        // follows. This is not the intended call order (main.c always loads
        // first) but a defensive default costs nothing and a wrong tc_type
        // is exactly the kind of silent hazard docs/THERMOCOUPLE.md section 2
        // warns about.
        return CONFIG_STORE_DEFAULT_TC_TYPE;
    }
    config_store_record_t snap;
    if (!config_store_seqlock_read(&snap)) {
        return CONFIG_STORE_DEFAULT_TC_TYPE;
    }
    return snap.tc_type;
}

float config_store_get_tc_offset_c(void)
{
    if (!s_loaded) {
        return 0.0f; // safe default: no correction until proven otherwise
    }
    config_store_record_t snap;
    if (!config_store_seqlock_read(&snap)) {
        return 0.0f;
    }
    return snap.tc_offset_c;
}

bool config_store_is_calibration_missing(void)
{
    if (!s_loaded) {
        return true; // safe default: missing until proven otherwise
    }
    config_store_record_t snap;
    if (!config_store_seqlock_read(&snap)) {
        return true;
    }
    return snap.calibration_missing;
}

bool config_store_is_tc_type_set(void)
{
    if (!s_loaded) {
        return false; // safe default: treat as uncommissioned until proven otherwise
    }
    config_store_record_t snap;
    if (!config_store_seqlock_read(&snap)) {
        return false;
    }
    return (snap.fields_set & CONFIG_STORE_SET_TC_TYPE) != 0u;
}

uint8_t config_store_get_estop_active_level(void)
{
    if (!s_loaded) {
        // Same "safe defaults over silence" discipline as
        // config_store_get_tc_type() above, and here the safe default is
        // not merely conventional: ACTIVE_HIGH is the only polarity under
        // which a broken E-stop line reads as STOP. A caller running before
        // the store is loaded must not be handed the polarity that cannot
        // detect a lost signal.
        return DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH;
    }
    config_store_record_t snap;
    if (!config_store_seqlock_read(&snap)) {
        return DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH;
    }
    return snap.estop_active_level;
}

void config_store_get_ct_cal(config_store_ct_channel_cal_t out[CONFIG_STORE_CT_CAL_NUM_CHANNELS])
{
    if (!s_loaded) {
        // Defensive, same reasoning as config_store_get_tc_type() above: a
        // caller that runs before config_store_boot_load() gets
        // config_store_default()'s shape (every channel calibrated ==
        // false) rather than uninitialised/zeroed memory that happens to
        // look the same today but is not guaranteed to.
        config_store_record_t def;
        config_store_default(&def);
        memcpy(out, def.ct_cal, sizeof(def.ct_cal));
        return;
    }
    config_store_record_t snap;
    if (!config_store_seqlock_read(&snap)) {
        config_store_record_t def;
        config_store_default(&def);
        memcpy(out, def.ct_cal, sizeof(def.ct_cal));
        return;
    }
    memcpy(out, snap.ct_cal, sizeof(snap.ct_cal));
}

// SAFETY_CMD_FW_VERSION's config_version/config_crc fields (LINK_PROTOCOL.md
// sec 4), wired to the real cache now that config_store exists -- see
// link_task.c's link_task_send_fw_version(), which used to hard-code both to
// 0 with a "no config_store yet" comment.
void config_store_get_full_record(config_store_record_t *out)
{
    if (!out) {
        return;
    }
    if (!s_loaded) {
        config_store_default(out);
        return;
    }
    if (!config_store_seqlock_read(out)) {
        config_store_default(out);
    }
}

uint8_t config_store_get_config_version(void)
{
    if (!s_loaded) {
        return 0;
    }
    config_store_record_t snap;
    if (!config_store_seqlock_read(&snap)) {
        return 0;
    }
    // config_store_seq_to_version() (config_store.c, pure/host-tested) is
    // the actual mapping -- see its own header comment in config_store.h for
    // why this can no longer be a bare truncating `& 0xFFu`: that collided
    // with the "never loaded" sentinel (0) every 256th commit.
    return config_store_seq_to_version(snap.seq);
}

uint16_t config_store_get_config_crc(void)
{
    if (!s_loaded) {
        return 0;
    }
    config_store_record_t snap;
    if (!config_store_seqlock_read(&snap)) {
        return 0;
    }
    // See config_store.h's own header comment on this function for the full
    // reasoning: seq == 0 is the same sentinel config_store_get_config_
    // version() already treats as "no CRC-verified record was ever
    // committed" (never written, OR committed-then-rejected-at-load), and
    // this getter must report the same "not confirmed" answer for the same
    // reason config_store_confirm_crc_ok() does -- KilnFW's safety_page.html/
    // diagnostics_page.html test THIS field, alone, for "UNCOMMISSIONED".
    // Returning the default record's own real (non-zero) packed CRC here,
    // as this function did before this fix, made a never-committed OR
    // rejected-at-load board read back as commissioned on both pages.
    if (snap.seq == 0u) {
        return 0u;
    }
    return (uint16_t)(config_store_record_crc(&snap) & 0xFFFFu);
}

typedef struct {
    hal_flash_region_t *region; // s_regions[plan.sector_index] -- the ONLY
                                 // sector this callback touches; the other
                                 // one (holding the still-current record
                                 // whenever needs_erase is true) is never
                                 // passed here at all, so there is no way for
                                 // this callback to erase or program the
                                 // sector a crash must still be able to fall
                                 // back on.
    size_t  next_write_slot;
    bool    needs_erase;
    uint8_t record[CONFIG_STORE_RECORD_LEN];
    hal_status_t result; // persist/save logging audit (2026-09-06): this file's
                          // own header comment used to argue the erase/program
                          // status could never be anything but HAL_OK here, and
                          // discarded it -- but update_task.c's near-identical
                          // update_metadata_write_cb() was fixed the same day
                          // ("discarding it would let a failed erase/program
                          // still report success up the call chain") for the
                          // exact same shape of call. hal_flash_safe_execute()'s
                          // own HAL_OK only means the callback RAN, not that the
                          // op it ran succeeded, so this must be captured and
                          // checked by config_store_write() too -- see that
                          // function below.
} config_store_write_args_t;

static void config_store_write_cb(void *param)
{
    config_store_write_args_t *a = (config_store_write_args_t *)param;
    a->result = HAL_OK;
    if (a->needs_erase) {
        a->result = hal_flash_erase(a->region, 0, SAFTYFW_CONFIG_STORE_FLASH_SIZE);
        if (a->result != HAL_OK) {
            return; // do not attempt the program half over a failed erase
        }
    }
    a->result = hal_flash_program(a->region,
                                   (uint32_t)a->next_write_slot * CONFIG_STORE_RECORD_LEN,
                                   a->record, CONFIG_STORE_RECORD_LEN);
}

// Writes `rec` as the new current config record, refusing while ARMED
// (TODO.md Phase 9: "Config writes refused while ARMED") -- checked here,
// against relay_owner_get_state(), not left to the caller, so every write
// path gets the same guarantee regardless of who calls this. Returns false
// and sets `*out_reason` to a human-readable explanation on refusal (ARMED)
// or on a flash_safe_execute() failure; the caller is expected to surface
// that string over HTTP/PC UART the same way other refusal reasons in this
// codebase are (see config_store_write_decision_reason()).
//
// This is live, not dormant: link_task.c calls it from three wire command
// handlers -- SET_CONFIG (link_task.c:1473), SET_CT_CAL (link_task.c:1538)
// and COMMIT_CONFIG (link_task.c:2017) -- so this path runs on every
// commissioning commit, not just when TODO.md Phase 9's follow-on work
// eventually lands (docs/audits/unreviewed_changes_review_2026-09-08.md
// finding D4; this comment previously said "no caller exists yet," which
// was true only when Phase 9's first pass landed and has been stale since).
bool config_store_write(const config_store_record_t *rec, const char **out_reason)
{
    config_store_write_decision_t decision =
        config_store_decide_write(relay_owner_get_state() == RELAY_OWNER_STATE_ARMED);
    if (decision != CONFIG_STORE_WRITE_OK) {
        if (out_reason != NULL) {
            *out_reason = config_store_write_decision_reason(decision);
        }
        return false;
    }

    if (!ensure_region()) {
        // Same NOT_PERMITTED family as a flash_safe_execute() failure below
        // -- the region has never bound (e.g. this is somehow called before
        // any successful boot_load), which is exactly the "safe execution
        // isn't possible at all" class config_store_flash_rc_reason()
        // already names, not a transient timeout.
        if (out_reason != NULL) {
            *out_reason = config_store_flash_rc_reason(CONFIG_STORE_FLASH_RC_NOT_PERMITTED);
        }
        return false;
    }

    config_store_record_t to_write = *rec;
    to_write.format_version = CONFIG_STORE_FORMAT_VERSION;
    to_write.seq = s_cached_record.seq + 1u;

    // config_store.h's "A/B sector arbitration" -- this plan names which
    // sector this write actually targets, and whether that sector needs an
    // erase first. When it is a sector SWITCH (needs_erase == true), the
    // target is always the OTHER sector from s_cached_sector -- the one
    // holding the still-valid current record is passed to config_store_
    // write_cb() only as a value already captured in s_cached_record, never
    // as `args.region`, so this callback cannot erase or reprogram it no
    // matter when a crash interrupts it.
    config_store_write_plan_t plan = config_store_plan_write(s_cached_sector, s_cached_slot);

    config_store_write_args_t args;
    args.region = s_regions[plan.sector_index];
    args.next_write_slot = plan.slot_index;
    args.needs_erase = plan.needs_erase;
    args.result = HAL_NOT_READY; // overwritten by the callback if it ever runs
    config_store_pack(&to_write, args.record);

    hal_status_t status = hal_flash_safe_execute(config_store_write_cb, &args, 1000u);
    // Both must succeed: `status` reports whether the callback ran at all
    // (lockout handshake), args.result reports whether the erase/program it
    // ran actually landed -- see config_store_write_args_t's own result field
    // comment for why neither check alone is sufficient (a HAL_OK `status`
    // with a failed args.result used to be silently reported as success).
    if (status != HAL_OK || args.result != HAL_OK) {
        // Surface WHICH failure mode this was, not a single opaque string --
        // see config_store_flash_rc_reason()'s header comment (config_store.h)
        // for why "the other core never answered the lockout" (TIMEOUT) and
        // "safe execution isn't possible at all" (NOT_PERMITTED) must not be
        // reported identically: one is a transient/bench condition, the
        // other is a firmware init-order bug. hal_flash_safe_execute()
        // already made this same distinction (hal_status_t); translated back
        // to the legacy CONFIG_STORE_FLASH_RC_* value so config_store_flash_
        // rc_reason()'s existing strings need no change.
        hal_status_t failing_status = (status != HAL_OK) ? status : args.result;
        int rc;
        (void)hal_status_to_config_store_flash_rc(failing_status, &rc);
        if (out_reason != NULL) {
            *out_reason = config_store_flash_rc_reason(rc);
        }
        return false;
    }

    // Seqlock write, not a plain struct assignment (2026-09-09): every
    // reader above runs on the OTHER core (SAFTYFW_CORE_TRIP_PATH) with no
    // other synchronisation against this update -- see the seqlock block
    // near s_cached_record's declaration for the full reasoning.
    // s_cached_slot/s_cached_sector are NOT part of this: they are read
    // only by this same function (config_store_plan_write() above, next
    // call), never by a reader on the other core, so they need no seqlock
    // protection of their own.
    config_store_seqlock_write(&to_write);
    s_cached_slot = plan.slot_index;
    s_cached_sector = plan.sector_index;
    if (out_reason != NULL) {
        *out_reason = "ok";
    }
    return true;
}
