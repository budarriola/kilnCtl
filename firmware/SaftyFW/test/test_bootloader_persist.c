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

void run_test_bootloader_persist(void)
{
    test_wrap_power_loss_every_step();
    test_backup_failure_keeps_main();
    test_silently_corrupt_backup_keeps_main();
    test_persist_reports_unverified_program();
}
