// test_bootloader_persist.c -- host tests for bootloader/persist.c's
// power-loss behaviour (AUDIT_PICO_UPDATE MED-1, LOW-3). Runs in the
// bootloader_recovery_update_tests exe: the real persist.c / metadata.c are
// linked against the fake flash in test_bootloader_recovery_update.c, whose
// erase/program stubs can tear the Nth operation and drop everything after it
// (g_flash_op_limit / g_power_lost).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "flash_layout.h"
#include "metadata.h"
#include "persist.h"

#define FAKE_FLASH_SIZE 0x00200000u
extern uint8_t g_fake_flash[FAKE_FLASH_SIZE];
extern int g_flash_op_limit;
extern int g_flash_ops;
extern bool g_power_lost;
extern bool g_corrupt_backup_program;
extern bool g_program_src_in_flash;

static const uint8_t *main_sector(void) { return &g_fake_flash[BOOTLOADER_METADATA_FLASH_OFFSET]; }
static const uint8_t *backup_sector(void)
{
    return &g_fake_flash[BOOTLOADER_METADATA_BACKUP_FLASH_OFFSET];
}

static bootloader_metadata_t base_meta(void)
{
    bootloader_metadata_t m;
    memset(&m, 0, sizeof(m));
    m.format_version = BOOTLOADER_METADATA_FORMAT_VERSION;
    m.active_slot = BOOTLOADER_SLOT_A;
    m.slots[BOOTLOADER_SLOT_A].state = BOOTLOADER_SLOT_VALID;
    m.slots[BOOTLOADER_SLOT_A].length = 1000;
    m.slots[BOOTLOADER_SLOT_A].crc32 = 0x1234u;
    m.slots[BOOTLOADER_SLOT_B].state = BOOTLOADER_SLOT_VALID;
    m.slots[BOOTLOADER_SLOT_B].length = 2000;
    m.slots[BOOTLOADER_SLOT_B].crc32 = 0x5678u;
    return m;
}

static void power_up(void)
{
    g_flash_op_limit = -1;
    g_flash_ops = 0;
    g_power_lost = false;
}

// Writes `n` records through the real persist path; returns the latest index.
static size_t fill_log(size_t n, bootloader_metadata_t *m)
{
    size_t latest = BOOTLOADER_METADATA_NO_SLOT;
    for (size_t i = 0; i < n; i++) {
        bool ok = bootloader_persist_metadata(m, latest);
        TEST_CHECK(ok, "healthy persist reports success");
        bootloader_metadata_t tmp;
        latest = bootloader_metadata_find_latest_ab(main_sector(), backup_sector(), &tmp);
    }
    return latest;
}

static void test_wrap_power_loss_every_step(void)
{
    TEST_SECTION("MED-1 -- power loss at every flash operation of the log wrap");

    // Count the flash operations of one healthy wrap write.
    memset(g_fake_flash, 0xFF, sizeof(g_fake_flash));
    power_up();
    bootloader_metadata_t m = base_meta();
    size_t latest = fill_log(BOOTLOADER_METADATA_SLOTS_PER_SECTOR, &m);
    TEST_CHECK(latest == BOOTLOADER_METADATA_SLOTS_PER_SECTOR - 1, "log filled to its last slot");
    uint32_t seq_before = m.seq;
    static uint8_t snapshot[FAKE_FLASH_SIZE];
    memcpy(snapshot, g_fake_flash, sizeof(snapshot));

    g_flash_ops = 0;
    bootloader_metadata_t w = m;
    TEST_CHECK(bootloader_persist_metadata(&w, latest), "healthy wrap write succeeds");
    int total_ops = g_flash_ops;
    TEST_CHECK(total_ops >= 4, "wrap = backup erase+program and main erase+program");

    for (int cut = 0; cut <= total_ops; cut++) {
        memcpy(g_fake_flash, snapshot, sizeof(snapshot));
        power_up();
        g_flash_op_limit = cut; // cut == total_ops: no tear, the write completes
        w = m;
        bool ok = bootloader_persist_metadata(&w, latest);
        power_up(); // reboot

        bootloader_metadata_t got;
        size_t idx = bootloader_metadata_find_latest_ab(main_sector(), backup_sector(), &got);
        TEST_CHECK(idx != BOOTLOADER_METADATA_NO_SLOT,
                   "a valid metadata record survives a power loss at every wrap step");
        TEST_CHECK(idx == BOOTLOADER_METADATA_NO_SLOT || got.seq >= seq_before,
                   "surviving record is not older than the pre-wrap latest");
        if (idx != BOOTLOADER_METADATA_NO_SLOT) {
            bootloader_boot_decision_t d = bootloader_decide_boot(&got);
            TEST_CHECK(d.bootable, "bootloader still finds a bootable slot (no spurious recovery)");
        }
        if (cut == total_ops) {
            TEST_CHECK(ok && got.seq == seq_before + 1u, "uncut wrap lands the new record");
        }

        // The next write from whatever state survived must work and verify.
        if (idx != BOOTLOADER_METADATA_NO_SLOT) {
            bootloader_metadata_t again = got;
            TEST_CHECK(bootloader_persist_metadata(&again, idx),
                       "next persist after the power loss succeeds");
            bootloader_metadata_t g2;
            size_t i2 = bootloader_metadata_find_latest_ab(main_sector(), backup_sector(), &g2);
            TEST_CHECK(i2 != BOOTLOADER_METADATA_NO_SLOT && g2.seq == again.seq,
                       "and it is the newest record afterwards");
        }
    }
}

static void test_backup_failure_keeps_main(void)
{
    TEST_SECTION("MED-1 -- a failed backup never erases the only copy");

    memset(g_fake_flash, 0xFF, sizeof(g_fake_flash));
    power_up();
    bootloader_metadata_t m = base_meta();
    size_t latest = fill_log(BOOTLOADER_METADATA_SLOTS_PER_SECTOR, &m);
    uint32_t seq_before = m.seq;

    g_flash_ops = 0;
    g_flash_op_limit = 1; // the backup program (2nd op) is torn
    bootloader_metadata_t w = m;
    bool ok = bootloader_persist_metadata(&w, latest);
    power_up();
    bootloader_metadata_t got;
    size_t idx = bootloader_metadata_find_latest_ab(main_sector(), backup_sector(), &got);
    TEST_CHECK(!ok, "persist reports failure when the backup did not verify");
    TEST_CHECK(idx == latest && got.seq == seq_before,
               "main sector left intact with its newest record");
}

static void test_silently_corrupt_backup_keeps_main(void)
{
    TEST_SECTION("MED-1 -- backup that silently fails read-back never erases main");

    memset(g_fake_flash, 0xFF, sizeof(g_fake_flash));
    power_up();
    bootloader_metadata_t m = base_meta();
    size_t latest = fill_log(BOOTLOADER_METADATA_SLOTS_PER_SECTOR, &m);
    uint32_t seq_before = m.seq;
    g_corrupt_backup_program = true;
    bootloader_metadata_t w = m;
    bool ok = bootloader_persist_metadata(&w, latest);
    g_corrupt_backup_program = false;
    bootloader_metadata_t got;
    size_t idx = bootloader_metadata_find_latest_ab(main_sector(), backup_sector(), &got);
    TEST_CHECK(!ok, "persist refuses when the backup read-back differs");
    TEST_CHECK(idx == latest && got.seq == seq_before, "main sector not erased");
}

static void test_persist_reports_unverified_program(void)
{
    TEST_SECTION("LOW-3 -- persist returns false when the record does not read back");

    memset(g_fake_flash, 0xFF, sizeof(g_fake_flash));
    power_up();
    bootloader_metadata_t m = base_meta();
    TEST_CHECK(bootloader_persist_metadata(&m, BOOTLOADER_METADATA_NO_SLOT),
               "first write verifies");

    bootloader_metadata_t w = m;
    g_flash_ops = 0;
    g_flash_op_limit = 0; // mid-log write: its only op (the program) is torn
    TEST_CHECK(!bootloader_persist_metadata(&w, 0), "torn program is reported as failure");
    power_up();
    bootloader_metadata_t got;
    TEST_CHECK(bootloader_metadata_find_latest_ab(main_sector(), backup_sector(), &got) == 0,
               "the previous record is still the valid one");
}

static void test_wrap_backup_source_is_ram(void)
{
    TEST_SECTION("PS2-01 -- wrap backup program never reads its source from XIP flash");

    memset(g_fake_flash, 0xFF, sizeof(g_fake_flash));
    power_up();
    g_program_src_in_flash = false;
    bootloader_metadata_t m = base_meta();
    size_t latest = fill_log(BOOTLOADER_METADATA_SLOTS_PER_SECTOR, &m);
    uint32_t seq_before = m.seq;
    bootloader_metadata_t w = m;
    bool ok = bootloader_persist_metadata(&w, latest);
    TEST_CHECK(!g_program_src_in_flash, "no flash_range_program source pointer inside XIP flash");
    TEST_CHECK(ok, "wrap persist reports success");
    bootloader_metadata_t got;
    size_t idx = bootloader_metadata_find_latest_ab(main_sector(), backup_sector(), &got);
    TEST_CHECK(idx == 0 && got.seq == seq_before + 1u, "wrapped record is the newest at slot 0");
    bootloader_metadata_t bk;
    TEST_CHECK(bootloader_metadata_find_latest(backup_sector(), &bk) != BOOTLOADER_METADATA_NO_SLOT &&
                   bk.seq == seq_before,
               "backup holds the previous newest record");
    g_program_src_in_flash = false;
}

static void test_persist_failure_blocks_pending(void)
{
    TEST_SECTION("PS2-01 -- failed boot-time persist consumes a PENDING_VERIFY attempt");

    bootloader_metadata_t m = base_meta();
    m.slots[BOOTLOADER_SLOT_A].state = BOOTLOADER_SLOT_PENDING_VERIFY;
    TEST_CHECK(bootloader_metadata_persist_failure_blocks_boot(&m, BOOTLOADER_SLOT_A),
               "PENDING_VERIFY slot is not booted when its attempt could not be recorded");
    TEST_CHECK(!bootloader_metadata_persist_failure_blocks_boot(&m, BOOTLOADER_SLOT_B),
               "a VALID slot still boots when persist fails");
    TEST_CHECK(bootloader_metadata_persist_failure_blocks_boot(NULL, BOOTLOADER_SLOT_A),
               "NULL metadata fails closed");
}

static void test_torn_target_slot_wraps(void)
{
    TEST_SECTION("PS2-02 -- a torn, non-erased target slot takes the wrap path");

    memset(g_fake_flash, 0xFF, sizeof(g_fake_flash));
    power_up();
    bootloader_metadata_t m = base_meta();
    size_t latest = fill_log(3, &m); // slots 0..2 valid, latest == 2
    TEST_CHECK(latest == 2, "log holds three records");
    uint32_t seq_before = m.seq;
    // Slot 3: a record torn mid-program (CRC fails, bytes not erased).
    uint8_t *torn = &g_fake_flash[BOOTLOADER_METADATA_FLASH_OFFSET + 3u * BOOTLOADER_METADATA_RECORD_LEN];
    memset(torn, 0xFF, BOOTLOADER_METADATA_RECORD_LEN);
    memcpy(torn, main_sector() + 2u * BOOTLOADER_METADATA_RECORD_LEN, 12);
    torn[40] = 0x5A;
    bootloader_metadata_t chk;
    TEST_CHECK(bootloader_metadata_find_latest(main_sector(), &chk) == 2, "torn slot is skipped by find_latest");

    bootloader_metadata_t w = m;
    bool ok = bootloader_persist_metadata(&w, latest);
    TEST_CHECK(ok, "persist over a torn target slot reports success");
    bootloader_metadata_t got;
    size_t idx = bootloader_metadata_find_latest_ab(main_sector(), backup_sector(), &got);
    TEST_CHECK(idx == 0 && got.seq == seq_before + 1u, "new record landed at slot 0 after the erase");
    bootloader_metadata_t bk;
    TEST_CHECK(bootloader_metadata_find_latest(backup_sector(), &bk) != BOOTLOADER_METADATA_NO_SLOT &&
                   bk.seq == seq_before,
               "newest record was backed up before the erase");

    // Same with nothing valid in the main sector: erase, no backup.
    memset(g_fake_flash, 0xFF, sizeof(g_fake_flash));
    g_fake_flash[BOOTLOADER_METADATA_FLASH_OFFSET + 7] = 0x00;
    bootloader_metadata_t f = base_meta();
    TEST_CHECK(bootloader_persist_metadata(&f, BOOTLOADER_METADATA_NO_SLOT),
               "first write over a dirty slot 0 succeeds");
    TEST_CHECK(bootloader_metadata_find_latest(main_sector(), &got) == 0, "record readable at slot 0");
}

static void test_plan_write(void)
{
    TEST_SECTION("PS2-02 -- bootloader_metadata_plan_write");

    bootloader_metadata_write_plan_t p = bootloader_metadata_plan_write(2, true);
    TEST_CHECK(p.slot == 3 && !p.needs_erase && !p.needs_backup, "erased next slot: plain append");
    p = bootloader_metadata_plan_write(2, false);
    TEST_CHECK(p.slot == 0 && p.needs_erase && p.needs_backup, "dirty next slot: backup, erase, slot 0");
    p = bootloader_metadata_plan_write(BOOTLOADER_METADATA_SLOTS_PER_SECTOR - 1, true);
    TEST_CHECK(p.slot == 0 && p.needs_erase && p.needs_backup, "full log wraps with backup");
    p = bootloader_metadata_plan_write(BOOTLOADER_METADATA_NO_SLOT, false);
    TEST_CHECK(p.slot == 0 && p.needs_erase && !p.needs_backup, "dirty slot 0 on empty log: erase, no backup");
    p = bootloader_metadata_plan_write(BOOTLOADER_METADATA_FROM_BACKUP, false);
    TEST_CHECK(p.slot == 0 && p.needs_erase && !p.needs_backup, "newest in backup: erase, no new backup");
}

void run_test_bootloader_persist(void)
{
    test_wrap_backup_source_is_ram();
    test_persist_failure_blocks_pending();
    test_torn_target_slot_wraps();
    test_plan_write();
    test_wrap_power_loss_every_step();
    test_backup_failure_keeps_main();
    test_silently_corrupt_backup_keeps_main();
    test_persist_reports_unverified_program();
}
