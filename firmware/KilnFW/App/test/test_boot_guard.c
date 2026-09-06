// Host tests for App/drivers/persist/boot_guard.c -- the boot-failure counter and
// recovery-mode decision behind the owner's watchdog-recovery request
// (ROADMAP.md). #includes boot_guard.c directly (same convention as
// test_kiln_cfg_store.c/test_wifi_prov.c) to reach its static
// crc32_compute()/record_crc()/record_is_valid()/next_boot_count() helpers
// and its s_bg file-scope state directly -- there is no other seam into a
// module whose entire job is managing that state.
//
// Uses fake_kv.h's RAM-backed hal_kv fake to simulate actual persistence
// across simulated reboots (HW_ABSTRACTION_PLAN.md Phase 3 item 3, the
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
    simulate_reboot();

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
    simulate_reboot();

    // Run it up close to the threshold, then confirm healthy, then prove the
    // NEXT boot starts fresh instead of continuing to climb.
    for (uint32_t i = 0; i < (uint32_t)RECOVERY_MODE_BOOT_THRESHOLD - 1; i++) {
        simulate_reboot();
        boot_guard_init();
    }
    TEST_CHECK(!boot_guard_is_recovery_mode(), "precondition: not yet in recovery mode");

    boot_guard_mark_healthy();

    simulate_reboot();
    boot_guard_init();
    TEST_CHECK(boot_guard_get_boot_count() == 1u,
               "boot_guard_mark_healthy() reset the persisted count to 0, so the next boot sees count 1");
    TEST_CHECK(!boot_guard_is_recovery_mode(), "nowhere near the threshold after a healthy mark");
}

static void test_corrupted_record_is_treated_as_count_zero(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();

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

void run_test_boot_guard(void)
{
    test_crc32_reference_vector();
    test_record_crc_detects_corruption();
    test_next_boot_count_threshold();
    test_counter_increments_and_enters_recovery();
    test_mark_healthy_clears_counter();
    test_corrupted_record_is_treated_as_count_zero();
    test_boot_confirm_is_healthy();
    test_boot_confirm_decide();
}
