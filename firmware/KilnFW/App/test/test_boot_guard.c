// Host tests for App/drivers/persist/boot_guard.c -- the boot-failure counter and
// recovery-mode decision behind the owner's watchdog-recovery request
// (ROADMAP.md). #includes boot_guard.c directly (same convention as
// test_kiln_cfg_store.c/test_wifi_prov.c) to reach its static
// crc32_compute()/record_crc()/record_is_valid()/next_boot_count() helpers
// and its s_bg file-scope state directly -- there is no other seam into a
// module whose entire job is managing that state.
//
// Uses fake_kv.h's RAM-backed hal_kv fake to simulate actual persistence
// across simulated reboots (HW_ABSTRACTION.md Phase 3 item 3, the
// nvs.h -> hal_kv.h migration; this file previously used stubs/nvs.h's
// opt-in "real" blob store, nvs_test_enable(true)/nvs_test_clear()).
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "../drivers/persist/boot_guard.c"

// ---------------------------------------------------------------------------
// Simulated reboot: resets everything that would be lost on a real power
// cycle (s_bg's RAM state) while leaving the stubbed NVS blob (the flash
// stand-in) exactly as it was. Does NOT touch s_bg.lock -- a real reboot
// would get a freshly-created mutex too, but re-creating the stub's dummy
// handle buys nothing here and the stub's xSemaphoreTake/_Give are no-ops
// regardless (see stubs/freertos/semphr.h).
// ---------------------------------------------------------------------------
static void simulate_reboot(void)
{
    s_bg.initialized = false;
    s_bg.count = 0;
    s_bg.recovery_mode = false;
    s_bg.healthy_marked = false;
    /* s_bg_rtc is deliberately NOT touched here: on target it lives in RTC
     * slow memory (RTC_NOINIT_ATTR), which survives exactly this kind of
     * software reset. simulate_power_cycle() below is the one that clears
     * it, matching the only event that clears it on hardware. */
}

/* A power cycle: everything a software reset clears, PLUS RTC slow memory.
 * Used to start each test from a clean board, so the stuck-counter escape's
 * RTC flag never leaks from one test case into the next. */
static void simulate_power_cycle(void)
{
    simulate_reboot();
    memset(&s_bg_rtc, 0, sizeof(s_bg_rtc));
}

static void test_crc32_reference_vector(void)
{
    // Standard CRC32 (IEEE 802.3/zlib) test vector: crc32("123456789") == 0xCBF43926.
    // Pins that the local reimplementation (chosen over pulling in
    // esp_rom_crc.h -- see boot_guard.c's comment on crc32_compute()) is the
    // same algorithm esp_rom_crc32_le() implements on-target.
    uint32_t crc = crc32_compute("123456789", 9);
    TEST_CHECK(crc == 0xCBF43926u, "crc32_compute reference vector");
}

static void test_record_crc_detects_corruption(void)
{
    boot_guard_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = BOOT_GUARD_RECORD_VERSION;
    rec.boot_count = 7;
    rec.crc32 = record_crc(&rec);

    TEST_CHECK(record_is_valid(&rec), "freshly-computed CRC validates");

    // Flip one bit in boot_count -- the CRC must catch this. This is the
    // negative test for the CRC check itself: without it (e.g. if
    // record_is_valid() only checked `version`), this would incorrectly
    // pass and a corrupted-to-garbage count would be trusted.
    rec.boot_count ^= 0x01;
    TEST_CHECK(!record_is_valid(&rec), "a corrupted boot_count is rejected by the CRC");

    // Restore boot_count, corrupt the version instead.
    rec.boot_count ^= 0x01;
    rec.version = BOOT_GUARD_RECORD_VERSION + 1;
    TEST_CHECK(!record_is_valid(&rec), "a version mismatch is rejected");
}

static void test_next_boot_count_threshold(void)
{
    // Pins the EXACT count at which recovery_mode flips -- boot_guard.h
    // documents this as "loaded count >= RECOVERY_MODE_BOOT_THRESHOLD", i.e.
    // the (THRESHOLD+1)'th consecutive unconfirmed boot is the one that
    // actually enters recovery mode.
    bool recovery;
    uint32_t next;

    next = next_boot_count(0, &recovery);
    TEST_CHECK(next == 1u, "boot 1: count becomes 1");
    TEST_CHECK(!recovery, "boot 1: not recovery (loaded 0 < threshold)");

    next = next_boot_count(RECOVERY_MODE_BOOT_THRESHOLD - 1, &recovery);
    TEST_CHECK(next == (uint32_t)RECOVERY_MODE_BOOT_THRESHOLD, "boot at threshold-1: count becomes threshold");
    TEST_CHECK(!recovery, "loaded == threshold-1 is still NOT recovery mode yet");

    next = next_boot_count(RECOVERY_MODE_BOOT_THRESHOLD, &recovery);
    TEST_CHECK(next == (uint32_t)RECOVERY_MODE_BOOT_THRESHOLD + 1, "boot at threshold: count becomes threshold+1");
#if RECOVERY_MODE_ENABLED
    TEST_CHECK(recovery, "loaded == threshold IS recovery mode -- this is the exact crossing point");

    next = next_boot_count(RECOVERY_MODE_BOOT_THRESHOLD + 5, &recovery);
    TEST_CHECK(recovery, "well past threshold stays recovery mode");
#else
    /* Recovery mode is built but not armed (boot_guard.h's
     * RECOVERY_MODE_ENABLED). Assert the DISABLED contract just as strictly:
     * counting still works, but no count -- however large -- ever puts the
     * board into a mode that currently panic-loops. */
    TEST_CHECK(!recovery, "disabled build: loaded == threshold does NOT enter recovery mode");

    next = next_boot_count(RECOVERY_MODE_BOOT_THRESHOLD + 5, &recovery);
    TEST_CHECK(!recovery, "disabled build: no boot count, however large, enters recovery mode");
#endif
    (void)next;
}

static void test_counter_increments_and_enters_recovery(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    // Boots 1..RECOVERY_MODE_BOOT_THRESHOLD: never marked healthy. Boot N's
    // recovery decision is based on what boot N-1 LEFT BEHIND, so the first
    // RECOVERY_MODE_BOOT_THRESHOLD boots are all NOT recovery mode, and the
    // very next one (boot RECOVERY_MODE_BOOT_THRESHOLD + 1) is.
    for (uint32_t i = 1; i <= RECOVERY_MODE_BOOT_THRESHOLD; i++) {
        simulate_reboot();
        esp_err_t err = boot_guard_init();
        TEST_CHECK(err == ESP_OK, "boot_guard_init returns ESP_OK");
        TEST_CHECK(boot_guard_get_boot_count() == i, "count matches boot number");
        TEST_CHECK(!boot_guard_is_recovery_mode(), "not yet at the threshold");
    }

    simulate_reboot();
    boot_guard_init();
    TEST_CHECK(boot_guard_get_boot_count() == (uint32_t)RECOVERY_MODE_BOOT_THRESHOLD + 1,
               "count keeps incrementing");
#if RECOVERY_MODE_ENABLED
    TEST_CHECK(boot_guard_is_recovery_mode(),
               "boot after RECOVERY_MODE_BOOT_THRESHOLD unconfirmed boots enters recovery mode");
#else
    TEST_CHECK(!boot_guard_is_recovery_mode(),
               "disabled build: the counter still counts past the threshold, but the board does "
               "not enter recovery mode");
#endif
}

static void test_mark_healthy_clears_counter(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    // Run it up close to the threshold, then confirm healthy, then prove the
    // NEXT boot starts fresh instead of continuing to climb.
    for (uint32_t i = 0; i < (uint32_t)RECOVERY_MODE_BOOT_THRESHOLD - 1; i++) {
        simulate_reboot();
        boot_guard_init();
    }
    TEST_CHECK(!boot_guard_is_recovery_mode(), "precondition: not yet in recovery mode");

    TEST_CHECK(boot_guard_mark_healthy(), "boot_guard_mark_healthy() reports true when the clear "
               "actually verifies (the honest-hardware case)");

    simulate_reboot();
    boot_guard_init();
    TEST_CHECK(boot_guard_get_boot_count() == 1u,
               "boot_guard_mark_healthy() reset the persisted count to 0, so the next boot sees count 1");
    TEST_CHECK(!boot_guard_is_recovery_mode(), "nowhere near the threshold after a healthy mark");
}

// ---------------------------------------------------------------------------
// 2026-09-08 recovery-loop audit (docs/audits/boot_guard_recovery_loop_2026-09-08.md):
// on real hardware, boot_guard_mark_healthy()'s write call (persist_count(0))
// reported HAL_OK and set the old code's unconditional healthy_marked=true,
// yet the board came back up in recovery mode on every subsequent boot --
// the persisted "unconfirmed boot" count never actually reached 0 in flash,
// confirmed by a live JTAG read of s_bg mid-boot showing healthy_marked=true
// in RAM while the very next boot still loaded the pre-clear count. Trusting
// the write call's own return code was the bug. This test proves the NEW
// read-back verification (verify_persisted_count(), boot_guard.c) actually
// catches exactly that shape of lie: a committed record that a
// (real-hardware-observed) failure mode leaves NOT reading back as the value
// just written, despite the write path having reported success.
// ---------------------------------------------------------------------------
static void test_verify_persisted_count_catches_a_write_that_does_not_stick(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    // Get a real, valid, nonzero count on record (same as every other test
    // here) -- this is the STALE pre-clear value the real board's boot log
    // kept reporting boot after boot ("3 consecutive boots"), never
    // reaching 0 despite boot_guard_mark_healthy() reporting HAL_OK from its
    // write call every time.
    for (uint32_t i = 0; i < 2; i++) {
        simulate_reboot();
        boot_guard_init();
    }
    TEST_CHECK(boot_guard_get_boot_count() == 2u, "precondition: a real, valid, nonzero count is on record");

    // NEGATIVE TEST -- the exact real-hardware shape: the persisted record
    // is genuinely well-formed (valid version, valid CRC -- reading it back
    // does NOT fail or come back corrupt) but its boot_count is the STALE
    // pre-clear value, not the 0 a just-succeeded clear should show. Without
    // this function's boot_count==expected comparison (i.e. if it merely
    // checked "did the record read back and pass its CRC check", the same
    // mistake boot_guard_mark_healthy() used to make by trusting the
    // write's return code alone), this would be wrongly accepted as
    // verified. Deliberately does NOT call persist_count(0) first -- this
    // models the write claiming success while the flash content never
    // actually changed, which is exactly what could not be told apart from
    // a genuine clear without a value comparison.
    TEST_CHECK(!verify_persisted_count(0),
               "verify_persisted_count() refuses to confirm a clear when the persisted record is "
               "well-formed but its boot_count is still the stale nonzero value -- this is the "
               "exact gap that let the real board believe it had cleared the counter when it had "
               "not: a mere readability/CRC check would have wrongly passed this");

    // Now perform a genuine clear and confirm the SAME function correctly
    // accepts it -- proves the check is discriminating, not vacuously false.
    TEST_CHECK(persist_count(0) == HAL_OK, "a genuine clear write succeeds");
    TEST_CHECK(verify_persisted_count(0),
               "verify_persisted_count() DOES confirm a clear that genuinely reads back as 0");

    // Separately: a record that fails outright to read back (corrupted/
    // unreadable) must ALSO not be confused with "confirmed zero" -- see
    // boot_guard.c's own comment on why this function does not reuse
    // load_count()'s "unreadable collapses to 0" default.
    TEST_CHECK(fake_kv_script_corrupt_key(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_KEY_REC),
               "precondition: the just-written record can be corrupted for this test");
    TEST_CHECK(!verify_persisted_count(0),
               "verify_persisted_count() also refuses to confirm a clear whose read-back fails "
               "outright, rather than defaulting that to 'confirmed zero' the way load_count() would");
}

static void test_corrupted_record_is_treated_as_count_zero(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    // Run up several real boots so the persisted count is unambiguously
    // nonzero and would (if read back correctly) already be in or near
    // recovery mode.
    for (uint32_t i = 0; i < (uint32_t)RECOVERY_MODE_BOOT_THRESHOLD + 2; i++) {
        simulate_reboot();
        boot_guard_init();
    }
#if RECOVERY_MODE_ENABLED
    TEST_CHECK(boot_guard_is_recovery_mode(), "precondition: genuinely in recovery mode");
#else
    TEST_CHECK(boot_guard_get_boot_count() > (uint32_t)RECOVERY_MODE_BOOT_THRESHOLD,
               "precondition: the persisted count is unambiguously past the threshold");
#endif

    // Corrupt the persisted blob by reading it back, flipping a byte, and
    // writing the flipped bytes back through the real hal_kv_get_blob/
    // set_blob/commit round trip -- simulates a brownout-mid-write / bit-flip
    // without needing fake_kv-internal storage access (fake_kv.h's
    // fake_kv_script_corrupt_key() makes the NEXT get_* fail outright with
    // HAL_IO instead, which would not exercise record_is_valid()'s CRC
    // check the way a still-readable-but-wrong-bytes record does).
    boot_guard_record_t corrupt_rec;
    size_t corrupt_len = sizeof(corrupt_rec);
    hal_kv_handle_t corrupt_h;
    TEST_CHECK(hal_kv_open(&corrupt_h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "precondition: the boot-guard namespace opens");
    TEST_CHECK(hal_kv_get_blob(&corrupt_h, NVS_KEY_REC, &corrupt_rec, &corrupt_len) == HAL_OK
                   && corrupt_len == sizeof(corrupt_rec),
               "precondition: something is actually stored");
    ((uint8_t *)&corrupt_rec)[4] ^= 0xFF;
    TEST_CHECK(hal_kv_set_blob(&corrupt_h, NVS_KEY_REC, &corrupt_rec, sizeof(corrupt_rec)) == HAL_OK
                   && hal_kv_commit(&corrupt_h) == HAL_OK,
               "corrupted bytes written back");
    hal_kv_close(&corrupt_h);

    simulate_reboot();
    esp_err_t err = boot_guard_init();
    TEST_CHECK(err == ESP_OK, "boot_guard_init still returns ESP_OK over a corrupt record");
    TEST_CHECK(boot_guard_get_boot_count() == 1u,
               "a corrupted record is treated as count 0, so this boot's count becomes 1");
    TEST_CHECK(!boot_guard_is_recovery_mode(),
               "a corrupted record must NOT be trusted to still say recovery mode -- this is the "
               "exact failure boot_guard.h's CRC design note warns about: a corrupted-to-garbage "
               "count could otherwise either falsely force or, worse, permanently suppress "
               "recovery mode");
}

// ---------------------------------------------------------------------------
// boot_confirm_is_healthy() -- the shared predicate that must drive BOTH
// OTA-rollback confirmation and boot_guard's counter clear (main.c). Pinned
// here because a regression that silently reintroduces a safety-link term
// would reintroduce the exact bug fixed 2026-08-22 (OTA updates reverting
// forever on a board with no safety processor answering).
// ---------------------------------------------------------------------------
static void test_boot_confirm_is_healthy(void)
{
    TEST_CHECK(boot_confirm_is_healthy(true, true, true),
               "nvs+web+ota all up -- healthy, WITHOUT needing a safety-link argument at all");
    TEST_CHECK(!boot_confirm_is_healthy(false, true, true), "nvs down -- not healthy");
    TEST_CHECK(!boot_confirm_is_healthy(true, false, true), "web down -- not healthy");
    TEST_CHECK(!boot_confirm_is_healthy(true, true, false), "ota routes down -- not healthy");

    // THE negative test that pins the actual bug fixed 2026-08-22: the OLD
    // logic was `nvs_ok && web_ok && link_up`, which -- on a board with no
    // safety processor answering (link_up permanently false) -- meant this
    // exact snapshot (everything else healthy) was WRONGLY treated as
    // unhealthy forever. boot_confirm_is_healthy() has no way to see link_up
    // at all (it is not even a parameter) -- that is the fix.
    bool old_buggy_logic_result = true && true && false; /* nvs_ok && web_ok && link_up, link_up=false */
    TEST_CHECK(!old_buggy_logic_result, "sanity: the OLD three-AND logic would call this unhealthy");
    TEST_CHECK(boot_confirm_is_healthy(true, true, true),
               "the NEW logic calls the identical nvs/web/ota-healthy snapshot healthy, regardless "
               "of what a safety link is doing -- this is what changed and why");
}

// ---------------------------------------------------------------------------
// boot_confirm_decide() -- main.c's ota_rollback_confirm_task() uses this to
// decide whether esp_ota_mark_app_valid_cancel_rollback() even applies this
// poll (2026-08-24: a factory-partition boot reliably fails that call, and
// used to log it as an ERROR every single boot despite being expected). See
// boot_guard.h's doc comment on boot_confirm_action_t for what each outcome
// means.
// ---------------------------------------------------------------------------
static void test_boot_confirm_decide(void)
{
    // Not healthy yet -- keep polling, regardless of partition type. This is
    // the exact three-flags-not-all-true case boot_confirm_is_healthy()
    // already rejects; boot_confirm_decide() must defer to it rather than
    // inventing a second definition of "healthy".
    TEST_CHECK(boot_confirm_decide(false, false, true, true) == BOOT_CONFIRM_SKIP_NOT_HEALTHY,
               "OTA slot, nvs down -- not healthy, keep polling");
    TEST_CHECK(boot_confirm_decide(true, false, true, true) == BOOT_CONFIRM_SKIP_NOT_HEALTHY,
               "factory partition, nvs down -- still not healthy, keep polling (factory alone "
               "does not shortcut the health bar)");

    // Healthy, running from an OTA slot -- the existing behavior: confirm
    // via esp_ota_mark_app_valid_cancel_rollback().
    TEST_CHECK(boot_confirm_decide(false, true, true, true) == BOOT_CONFIRM_CONFIRM_OTA_SLOT,
               "OTA slot, healthy -- confirm the rollback-cancel");

    // Healthy, running from the factory partition -- the new branch: skip
    // the rollback-cancel call (nothing to cancel there), but boot_guard's
    // counter still needs clearing (main.c calls boot_guard_mark_healthy()
    // in this branch too -- see the call site).
    TEST_CHECK(boot_confirm_decide(true, true, true, true) == BOOT_CONFIRM_SKIP_FACTORY,
               "factory partition, healthy -- skip the rollback-cancel call, not an error");
}

// ---------------------------------------------------------------------------
// STUCK-COUNTER ESCAPE -- docs/audits/boot_guard_recovery_loop_2026-09-08.md.
//
// The real-hardware failure this exists for, reproduced end to end through
// the production functions: the persisted record refuses to change in
// flash, while every in-boot call (open/set_blob/commit) returns HAL_OK AND
// a reopened READ_ONLY handle reads the new value straight back (NVS serves
// every handle on a partition from one in-RAM index built at mount time, so
// no same-boot read-back can see this). Modelled here by writing the stale
// value back into the fake store immediately after every clear -- i.e. the
// clear verifies inside the boot and is gone by the next one, exactly as
// observed.
//
// Before the fix the board is trapped forever. After it, the boot that
// follows a verified-but-lost clear refuses to enter recovery mode.
// ---------------------------------------------------------------------------
static void freeze_persisted_count_at(uint32_t stale)
{
    /* Re-write the stale record behind the caller's back, standing in for
     * "the flash content never actually changed". Uses the production
     * persist_count() so the record is byte-for-byte a real one. */
    boot_guard_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = BOOT_GUARD_RECORD_VERSION;
    rec.boot_count = stale;
    rec.crc32 = record_crc(&rec);
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "freeze helper can open the store");
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_REC, &rec, sizeof(rec)) == HAL_OK,
               "freeze helper can write the stale record back");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "freeze helper commits");
    hal_kv_close(&h);
}

static void test_stuck_counter_escape(void)
{
    /* --- the pure predicate, every combination ------------------------- */
    TEST_CHECK(boot_guard_counter_is_stuck(BOOT_GUARD_RTC_MAGIC, 1u, 3u),
               "previous boot verified a clear and this boot still loaded 3 -> stuck");
    TEST_CHECK(!boot_guard_counter_is_stuck(BOOT_GUARD_RTC_MAGIC, 1u, 0u),
               "previous boot cleared and this boot loaded 0 -> the normal, working case");
    TEST_CHECK(!boot_guard_counter_is_stuck(BOOT_GUARD_RTC_MAGIC, 0u, 3u),
               "a nonzero count with NO verified clear behind it is an ordinary unconfirmed boot, "
               "not a stuck counter -- this is what keeps a genuinely reset-looping board in "
               "recovery mode");
    TEST_CHECK(!boot_guard_counter_is_stuck(0u, 1u, 3u),
               "garbage RTC memory (bad magic, i.e. after a power cycle) is never trusted");

    /* --- end to end, through boot_guard_init()/boot_guard_mark_healthy() */
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    /* Walk the counter up to the threshold so the next boot is a recovery
     * boot -- the state the real board was found in. */
    for (uint32_t i = 0; i < (uint32_t)RECOVERY_MODE_BOOT_THRESHOLD; i++) {
        simulate_reboot();
        boot_guard_init();
        freeze_persisted_count_at(i + 1u); /* init's own write sticks while the count climbs */
    }

    simulate_reboot();
    boot_guard_init();
#if RECOVERY_MODE_ENABLED
    TEST_CHECK(boot_guard_is_recovery_mode(), "precondition: the board is in recovery mode");
#endif
    /* A healthy boot: the clear verifies inside this boot... */
    TEST_CHECK(boot_guard_mark_healthy(), "the clear verifies within the boot (as it did on hardware)");
    /* ...and is then silently lost, which is the whole failure. */
    freeze_persisted_count_at((uint32_t)RECOVERY_MODE_BOOT_THRESHOLD);

    simulate_reboot();
    boot_guard_init();
    TEST_CHECK(!boot_guard_is_recovery_mode(),
               "the boot after a verified-but-lost clear does NOT enter recovery mode -- without "
               "the RTC-backed stuck-counter escape the board is trapped forever, which is exactly "
               "what happened on hardware");
    TEST_CHECK(boot_guard_get_boot_count() == 1u,
               "the stuck counter is treated as 0, so this boot counts as the first unconfirmed one");

    /* And a power cycle re-arms the guard from scratch rather than leaving
     * the escape latched: RTC memory is gone, so the stale count is
     * believed again until a fresh healthy boot re-proves it is stuck.
     * (The flash is still stuck at the same stale value -- freeze it again,
     * because the escape boot above wrote its own new count of 1, which on
     * this broken board would never have landed either.) */
    freeze_persisted_count_at((uint32_t)RECOVERY_MODE_BOOT_THRESHOLD);
    simulate_power_cycle();
    boot_guard_init();
#if RECOVERY_MODE_ENABLED
    TEST_CHECK(boot_guard_is_recovery_mode(),
               "after a power cycle the escape re-arms from scratch (one recovery boot at worst), "
               "rather than permanently disabling the boot guard");
#endif
}

// ---------------------------------------------------------------------------
// 2026-09-08 key move: NVS_KEY_REC went from "count" to "bootcnt2" because
// the item under the old key stopped changing in flash on this bench board
// (see boot_guard.c's comment on NVS_KEY_REC). An upgrading board must keep
// its counter history rather than silently restarting at 0 -- otherwise a
// genuinely reset-looping board would get RECOVERY_MODE_BOOT_THRESHOLD
// fresh chances every time the key name changed.
// ---------------------------------------------------------------------------
static void test_legacy_record_is_read_once_then_retired(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    /* An older firmware's record, under the OLD key only. */
    boot_guard_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = BOOT_GUARD_RECORD_VERSION;
    rec.boot_count = (uint32_t)RECOVERY_MODE_BOOT_THRESHOLD;
    rec.crc32 = record_crc(&rec);
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "legacy-record helper can open the store");
    hal_kv_close(&h);
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE_LEGACY, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "legacy-record helper can open the OLD namespace");
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_REC_LEGACY, &rec, sizeof(rec)) == HAL_OK,
               "legacy-record helper writes under the OLD namespace+key");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "legacy-record helper commits");
    hal_kv_close(&h);

    TEST_CHECK(load_count() == (uint32_t)RECOVERY_MODE_BOOT_THRESHOLD,
               "load_count() falls back to the legacy key, so an upgrade does not forget the "
               "counter history");

    simulate_reboot();
    boot_guard_init();
#if RECOVERY_MODE_ENABLED
    TEST_CHECK(boot_guard_is_recovery_mode(),
               "a board already at the threshold under the OLD key still enters recovery mode "
               "after the key move -- the move must not hand a looping board a fresh start");
#endif

    /* boot_guard_init()'s own write went to the NEW key and retired the old
     * one, so the next load reads the new key with no legacy fallback. */
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "store still opens after the migration write");
    size_t len = sizeof(rec);
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_REC, &rec, &len) == HAL_OK && len == sizeof(rec),
               "the record now exists under the NEW key");
    TEST_CHECK(rec.boot_count == (uint32_t)RECOVERY_MODE_BOOT_THRESHOLD + 1u,
               "the migrated record carries the incremented count, not a reset one");
    hal_kv_close(&h);
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE_LEGACY, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "the legacy namespace still opens");
    len = sizeof(rec);
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_REC_LEGACY, &rec, &len) == HAL_NOT_FOUND,
               "the legacy record is erased once the new one is written, so the stuck item can "
               "never be read again");
    hal_kv_close(&h);
}

// ---------------------------------------------------------------------------
// 2026-09-08 post-flash recovery foot-gun
// (docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md):
// boot_confirm_is_healthy() -> boot_guard_mark_healthy() is gated on a
// snapshot (nvs_report_capture()) that main_network_http.c takes ONCE, early,
// and never re-checks. A board that samples that snapshot during a
// transient window -- plausibly right after flash_firmware() resets the
// chip -- never gets a second chance to confirm healthy that boot, even
// though boot_guard's own counter keeps incrementing and persisting fine
// (it owns its NVS handle independently -- see boot_guard_init()). Three
// ordinary development flashes in a row can walk a perfectly healthy board
// into recovery mode this way. boot_guard_reset_counter() is the fix: a
// tool that knows it just performed a deliberate flash can clear the
// counter directly, without waiting on that flaky auto-health snapshot.
// ---------------------------------------------------------------------------
static void test_reset_counter_keeps_normal_flashing_under_threshold(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    // Model "three normal flashes in a row": each boot increments (the app
    // is running, boot_guard_init() always counts first, unconditionally --
    // see boot_guard.h), but never reaches boot_confirm_is_healthy() this
    // boot (the auto-health snapshot happened to sample a transient NVS
    // state), so the ONLY thing that clears the counter is the tool calling
    // boot_guard_reset_counter() after confirming the new build is up.
    for (uint32_t i = 0; i < 5; i++) {
        simulate_reboot();
        boot_guard_init();
        TEST_CHECK(boot_guard_get_boot_count() == 1u,
                   "each 'flash' boot increments from a cleared counter, never accumulating");
        TEST_CHECK(!boot_guard_is_recovery_mode(),
                   "a run of ordinary flashes never reaches the threshold when the tool resets "
                   "the counter after each one");
        TEST_CHECK(boot_guard_reset_counter(),
                   "boot_guard_reset_counter() verifies its clear on ordinary (non-broken) hardware");
    }
}

// THE negative test: prove test_reset_counter_keeps_normal_flashing_under_threshold()
// actually exercises the fix rather than passing vacuously. Simulates a
// genuinely broken boot -- nothing ever calls boot_guard_reset_counter() or
// boot_guard_mark_healthy(), the same as a board that really cannot boot --
// and confirms recovery mode is STILL reached after
// RECOVERY_MODE_BOOT_THRESHOLD such boots. This is what pins that the fix
// above is additive (an explicit, tool-driven reset), not a weakening of
// the counter itself: a board nobody ever calls the reset for is exactly as
// protected as before this change.
static void test_a_genuinely_failing_boot_still_trips_recovery(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    for (uint32_t i = 1; i <= (uint32_t)RECOVERY_MODE_BOOT_THRESHOLD; i++) {
        simulate_reboot();
        boot_guard_init();
        TEST_CHECK(boot_guard_get_boot_count() == i,
                   "a boot nobody ever confirms/resets keeps accumulating, unaffected by the new "
                   "reset path existing");
    }
    simulate_reboot();
    boot_guard_init();
#if RECOVERY_MODE_ENABLED
    TEST_CHECK(boot_guard_is_recovery_mode(),
               "NEGATIVE-TEST TARGET: a genuinely failing boot (nothing ever resets or confirms it) "
               "still trips recovery mode after RECOVERY_MODE_BOOT_THRESHOLD boots -- proves "
               "boot_guard_reset_counter() is an additive escape hatch for a deliberate flash, not "
               "a general weakening of the counter");
#endif
}

// ---------------------------------------------------------------------------
// boot_guard_reset_counter() against a HAL that LIES about the write, not
// just an honest failure -- fake_kv_script_next_write_status() above only
// models an honest HAL_NO_MEM/HAL_IO (the caller's `err != HAL_OK` branch
// catches that long before clear_persisted_counter_verified_locked()'s
// read-back ever runs). persist_count() writes via hal_kv_set_blob(), not
// hal_kv_erase_key(), so fake_kv_script_silent_erase_noops() (already used
// elsewhere in this file's mark_healthy coverage) cannot model a lying
// write on THIS function's first attempt at all -- it needs the set-blob
// noop added to fake_kv.h/.c specifically for this
// (fake_kv_script_silent_set_noops()). This is the only way to actually
// reach the read-back check with a set_blob-shaped writer, per
// docs/audits/boot_guard_recovery_loop_2026-09-08.md's original hardware
// finding: hal_kv_set_blob()/hal_kv_commit() reported HAL_OK while the
// persisted count never changed.
// ---------------------------------------------------------------------------
static void test_reset_counter_recovers_from_a_single_lying_write(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    for (uint32_t i = 0; i < 2; i++) {
        simulate_reboot();
        boot_guard_init();
    }
    TEST_CHECK(boot_guard_get_boot_count() == 2u, "precondition: a real, valid, nonzero count is on record");

    // Only the FIRST attempt's hal_kv_set_blob() lies -- persist_count(0)
    // inside clear_persisted_counter_verified_locked()'s first try reports
    // HAL_OK but changes nothing, so verify_persisted_count(0) fails and the
    // bounded erase-then-retry (erase_then_persist_count()) fires; its own
    // hal_kv_set_blob() call is a genuine, un-noop'd write and lands for
    // real.
    fake_kv_script_silent_set_noops(1u);
    TEST_CHECK(boot_guard_reset_counter(),
               "the bounded retry recovers when only the first write lies -- the check is "
               "discriminating, not vacuously failing");

    // boot_guard_reset_counter() only ever affects the NEXT boot's loaded
    // count (see its own doc comment) -- s_bg.count this boot is untouched
    // by design, so the only honest way to confirm the clear actually
    // landed in flash is to simulate the next boot and read what it loads.
    simulate_reboot();
    boot_guard_init();
    TEST_CHECK(boot_guard_get_boot_count() == 1u,
               "the NEXT boot loads a genuinely-cleared persisted count (0, then this boot's own "
               "unconditional +1) -- not just a return value that claimed success");
}

static void test_reset_counter_refuses_success_when_every_write_lies(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    for (uint32_t i = 0; i < 2; i++) {
        simulate_reboot();
        boot_guard_init();
    }
    TEST_CHECK(boot_guard_get_boot_count() == 2u, "precondition: a real, valid, nonzero count is on record");

    // BOTH the first attempt's hal_kv_set_blob() and the bounded retry's own
    // hal_kv_set_blob() (inside erase_then_persist_count(), after a genuine
    // erase) lie -- the exact stuck-on-real-hardware shape the 2026-09-08
    // audit found, now reached through boot_guard_reset_counter() rather
    // than boot_guard_mark_healthy(). A return-code-only version of
    // clear_persisted_counter_verified_locked() would report success here;
    // the read-back must catch it.
    fake_kv_script_silent_set_noops(2u);
    bool verified = boot_guard_reset_counter();
    TEST_CHECK(!verified,
               "boot_guard_reset_counter() reports FAILURE when the counter still reads nonzero "
               "after both the write and its retry claimed HAL_OK -- a write-status-only version "
               "would have wrongly reported success here");

    // NOTE this is NOT "the old count survives": erase_then_persist_count()
    // (the bounded retry) genuinely erases the record first -- that part is
    // real, not noop'd -- and only the WRITE that would recreate it lies.
    // So the record this leaves behind is MISSING, not stale-at-2, and
    // load_count() collapses a missing record to 0 exactly the same as a
    // genuinely-cleared one (see its own doc comment) -- the next boot
    // loads 0, +1 for that boot's own unconditional increment. The
    // assertion that matters here is boot_guard_reset_counter()'s own
    // return value above: it must report failure (not verified) on THIS
    // call even though the persisted state happens to land somewhere
    // count-like afterward, because a caller trusting a `false` return as
    // "keep treating this as recovery-relevant" must not be told `true`
    // when the read-back could not confirm it in the boot that ran it.
    simulate_reboot();
    boot_guard_init();
    TEST_CHECK(boot_guard_get_boot_count() == 1u,
               "the doubly-lying retry leaves the record erased (not rewritten), which the next "
               "boot's load_count() reads as 0, +1 for that boot -- NOT the same as the write "
               "having verified in the call that made it");
}

// ---------------------------------------------------------------------------
// boot_guard_get_persisted_count() -- the read-only re-read added so a
// caller (the boot_guard_reset HTTP route/flash_firmware()'s post-flash
// step) can tell "the persisted counter was really cleared just now" apart
// from "this boot's own fixed in-RAM count, unchanged by the call" -- see
// boot_guard.h's doc comment on this function.
// ---------------------------------------------------------------------------
static void test_get_persisted_count_before_init_fails(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle(); /* s_bg.initialized == false, no lock yet */

    uint32_t out = 0xdeadbeefu;
    TEST_CHECK(!boot_guard_get_persisted_count(&out),
               "before boot_guard_init() has run this boot, there is no lock and nothing to read -- "
               "must report failure, not a fabricated 0");
    TEST_CHECK(out == 0xdeadbeefu, "a failed call must not touch *out_count");
}

static void test_get_persisted_count_null_arg_fails(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();
    boot_guard_init();
    TEST_CHECK(!boot_guard_get_persisted_count(NULL), "a NULL out-param is refused, not a crash");
}

static void test_get_persisted_count_tracks_live_nvs_value(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    boot_guard_init(); /* boot 1: persists count 1 */
    uint32_t out = 0;
    TEST_CHECK(boot_guard_get_persisted_count(&out), "reads back successfully once initialized");
    TEST_CHECK(out == 1u, "matches what boot_guard_init() just persisted");
    // THE distinction this function exists for: boot_guard_get_boot_count()
    // (this boot's fixed in-RAM value) does NOT move when the persisted
    // record is cleared out from under it mid-boot, but this function does,
    // because it re-reads flash every call rather than returning s_bg.count.
    TEST_CHECK(boot_guard_get_boot_count() == 1u, "this boot's in-RAM count is still 1");
    TEST_CHECK(boot_guard_mark_healthy(), "clear verifies");
    TEST_CHECK(boot_guard_get_boot_count() == 1u,
               "NEGATIVE-TEST CONTROL: boot_count is unchanged by the clear -- this is exactly the "
               "confusing behavior the new field exists to give a caller a way around");
    TEST_CHECK(boot_guard_get_persisted_count(&out), "reads back successfully after the clear");
    TEST_CHECK(out == 0u, "persisted_count DOES reflect the clear immediately, unlike boot_count");
}

static void test_get_persisted_count_unreadable_record_fails_not_fabricated_zero(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    boot_guard_init(); /* boot 1: persists a genuine, valid count 1 */
    uint32_t out = 0;
    TEST_CHECK(boot_guard_get_persisted_count(&out), "reads back successfully before corruption");
    TEST_CHECK(out == 1u, "sanity: matches what boot_guard_init() just persisted");

    // fake_kv_script_corrupt_key() makes the NEXT get_* on this key fail
    // outright with HAL_IO -- this is the exact "NVS unreadable" condition
    // load_count() would silently collapse to 0. load_count_strict() (and
    // therefore this accessor) must instead report failure, never a
    // fabricated 0 -- a caller (the boot_guard_reset HTTP route in
    // particular) must not be able to mistake "could not read the record" for
    // "confirmed cleared".
    TEST_CHECK(fake_kv_script_corrupt_key(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_KEY_REC),
               "precondition: the persisted record can be corrupted for this test");
    out = 0xdeadbeefu;
    TEST_CHECK(!boot_guard_get_persisted_count(&out),
               "an unreadable persisted record must report failure, not a fabricated 0 -- "
               "this is the negative-tested guard against the reviewed gap");
    TEST_CHECK(out == 0xdeadbeefu, "a failed call must not touch *out_count");
}

static void test_get_persisted_count_legacy_open_error_fails_not_fabricated_zero(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_power_cycle();

    boot_guard_init(); /* creates+persists a genuine record under NVS_NAMESPACE/NVS_KEY_REC */

    /* Erase the primary key so its own get_blob() reports HAL_NOT_FOUND (the
     * namespace itself still exists and opens fine) -- this is the "not
     * found at the current location, fall back to legacy" branch of
     * load_count_strict(). */
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "primary namespace still opens for the erase");
    TEST_CHECK(hal_kv_erase_key(&h, NVS_KEY_REC) == HAL_OK, "primary key erases cleanly");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "erase commits");
    hal_kv_close(&h);

    /* Script the legacy namespace's NEXT open to fail with something other
     * than HAL_NOT_FOUND -- e.g. HAL_NOT_READY, a real "could not open"
     * error distinct from "this namespace has genuinely never been written".
     * Before this fix, load_count_strict() treated ANY non-HAL_OK open
     * (including this one) as "never written" and fabricated a 0; it must
     * instead report failure. */
    TEST_CHECK(fake_kv_script_next_open_status(NVS_NAMESPACE_LEGACY, HAL_NOT_READY),
               "precondition: the legacy namespace's next open can be scripted to fail");
    uint32_t out = 0xdeadbeefu;
    TEST_CHECK(!boot_guard_get_persisted_count(&out),
               "a legacy-namespace open error other than HAL_NOT_FOUND must report failure, not a "
               "fabricated 0 -- this is the negative-tested guard against the reviewed gap");
    TEST_CHECK(out == 0xdeadbeefu, "a failed call must not touch *out_count");
}

void run_test_boot_guard(void)
{
    test_crc32_reference_vector();
    test_record_crc_detects_corruption();
    test_next_boot_count_threshold();
    test_counter_increments_and_enters_recovery();
    test_mark_healthy_clears_counter();
    test_verify_persisted_count_catches_a_write_that_does_not_stick();
    test_corrupted_record_is_treated_as_count_zero();
    test_boot_confirm_is_healthy();
    test_boot_confirm_decide();
    test_stuck_counter_escape();
    test_legacy_record_is_read_once_then_retired();
    test_reset_counter_keeps_normal_flashing_under_threshold();
    test_a_genuinely_failing_boot_still_trips_recovery();
    test_reset_counter_recovers_from_a_single_lying_write();
    test_reset_counter_refuses_success_when_every_write_lies();
    test_get_persisted_count_before_init_fails();
    test_get_persisted_count_null_arg_fails();
    test_get_persisted_count_tracks_live_nvs_value();
    test_get_persisted_count_unreadable_record_fails_not_fabricated_zero();
    test_get_persisted_count_legacy_open_error_fails_not_fabricated_zero();
}
