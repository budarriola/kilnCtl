// Host test for App/drivers/persist/recovery_switch.c's boot_guard threshold
// path, recovery_switch_at_boot_threshold().
//
// The property under test: when the threshold is reached and selecting the
// recovery (factory-subtype) partition fails -- esp_ota_set_boot_partition()
// erroring after it may already have erased otadata, or a set that reports
// OK but reads back wrong -- the boot target is put back on the RUNNING
// partition, never left blank or pointing somewhere unintended. Also: the
// restore is one bounded attempt (no boot-path retry loop), nothing is
// written when the image does not verify, and the success path reboots.
//
// recovery_switch.c and boot_partition_verify.c are #included directly so
// the real set-and-read-back logic runs against the fake otadata below.
// esp_image_format.h comes from the private stubs_recovery_switch/ dir.
//
// NEGATIVE TEST (2026-10-08): with the restore call removed from
// recovery_switch_at_boot_threshold()'s SET_FAILED branch, the "restored"
// checks in test_set_fails_after_erase() and test_readback_mismatch() go RED;
// restored by hand and rebuilt from clean, all GREEN.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "boot_guard.h"
#include "esp_err.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

// ---------------------------------------------------------------------------
// Fake partition table and otadata.
// ---------------------------------------------------------------------------
static const esp_partition_t s_recovery = {
    .address = 0x20000u, .size = 0x180000u, .label = "recovery",
    .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_FACTORY,
};
static const esp_partition_t s_app = {
    .address = 0x200000u, .size = 0x300000u, .label = "app",
    .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0,
};

// What esp_ota_get_boot_partition() reports. NULL models blank otadata as a
// "no valid entry" read; the real IDF would then report the factory
// partition, which the tests also check is not what is left behind.
static const esp_partition_t *s_boot_part = &s_app;

typedef enum {
    SET_OK = 0,             // writes the request, returns ESP_OK
    SET_FAIL_AFTER_ERASE,   // erases otadata (boot target -> factory), then fails
    SET_OK_BUT_IGNORED,     // returns ESP_OK but otadata is unchanged
    SET_FAIL_NOTHING,       // fails before writing anything
} set_mode_t;

static set_mode_t s_set_mode_factory = SET_OK; // applied to a set of the recovery partition
static set_mode_t s_set_mode_other = SET_OK;   // applied to any other set (the restore)
static int s_set_calls = 0;
static const esp_partition_t *s_set_args[8];

static bool s_threshold = true;
static bool s_image_valid = true;
static int s_reboot_calls = 0;

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    if (s_set_calls < (int)(sizeof(s_set_args) / sizeof(s_set_args[0]))) {
        s_set_args[s_set_calls] = partition;
    }
    s_set_calls++;
    const set_mode_t mode = (partition == &s_recovery) ? s_set_mode_factory : s_set_mode_other;
    switch (mode) {
    case SET_OK:
        s_boot_part = partition;
        return ESP_OK;
    case SET_FAIL_AFTER_ERASE:
        s_boot_part = &s_recovery; // otadata erased: bootloader would fall through to factory
        return ESP_FAIL;
    case SET_OK_BUT_IGNORED:
        return ESP_OK;
    case SET_FAIL_NOTHING:
    default:
        return ESP_FAIL;
    }
}

const esp_partition_t *esp_ota_get_boot_partition(void) { return s_boot_part; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &s_app; }

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                 const char *label)
{
    (void)label;
    if (type == ESP_PARTITION_TYPE_APP && subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        return &s_recovery;
    }
    return NULL;
}

esp_err_t esp_image_verify(esp_image_load_mode_t mode, const esp_partition_pos_t *part,
                           esp_image_metadata_t *data)
{
    (void)mode;
    (void)data;
    if (!part || part->offset != s_recovery.address) {
        return ESP_FAIL;
    }
    return s_image_valid ? ESP_OK : ESP_FAIL;
}

void hal_wdt_reboot(void) { s_reboot_calls++; }

bool boot_guard_is_recovery_mode(void) { return s_threshold; }

// Same decision as boot_guard.c's pure boot_guard_decide_recovery_route()
// (tested directly in test_boot_guard.c); boot_guard.c itself is not linked
// here because it pulls in the whole NVS/hal_kv stack.
boot_recovery_route_t boot_guard_decide_recovery_route(bool threshold_reached, bool running_is_factory,
                                                       bool recovery_image_valid)
{
    if (!threshold_reached) return BOOT_RECOVERY_ROUTE_NORMAL;
    if (running_is_factory || !recovery_image_valid) return BOOT_RECOVERY_ROUTE_DEGRADED;
    return BOOT_RECOVERY_ROUTE_SWITCH_PARTITION;
}

#include "../drivers/persist/boot_partition_verify.c"
#include "../drivers/persist/recovery_switch.c"

static void reset_fakes(void)
{
    s_boot_part = &s_app;
    s_set_mode_factory = SET_OK;
    s_set_mode_other = SET_OK;
    s_set_calls = 0;
    for (size_t i = 0; i < sizeof(s_set_args) / sizeof(s_set_args[0]); i++) {
        s_set_args[i] = NULL;
    }
    s_threshold = true;
    s_image_valid = true;
    s_reboot_calls = 0;
}

static void test_set_fails_after_erase(void)
{
    TEST_SECTION("threshold: set(recovery) erases otadata then fails -> boot target restored to running");
    reset_fakes();
    s_set_mode_factory = SET_FAIL_AFTER_ERASE;
    recovery_switch_at_boot_threshold();
    TEST_CHECK(s_boot_part == &s_app, "boot target restored to the running partition (not left erased)");
    TEST_CHECK(s_set_calls == 2, "exactly two sets: the failed select and one restore");
    TEST_CHECK(s_set_args[1] == &s_app, "the restore targets the running partition");
    TEST_CHECK(s_reboot_calls == 0, "no reboot after a failed select");
}

static void test_readback_mismatch(void)
{
    TEST_SECTION("threshold: set(recovery) reports OK but reads back wrong -> restored to running");
    reset_fakes();
    s_boot_part = NULL; // otadata already unreadable before the select
    s_set_mode_factory = SET_OK_BUT_IGNORED;
    recovery_switch_at_boot_threshold();
    TEST_CHECK(s_boot_part == &s_app, "boot target restored to the running partition");
    TEST_CHECK(s_set_calls == 2, "the select and one restore");
    TEST_CHECK(s_reboot_calls == 0, "no reboot after a read-back mismatch");
}

static void test_restore_also_fails(void)
{
    TEST_SECTION("threshold: select fails and the restore fails too -> returns, no retry loop, no reboot");
    reset_fakes();
    s_set_mode_factory = SET_FAIL_AFTER_ERASE;
    s_set_mode_other = SET_FAIL_NOTHING;
    recovery_switch_at_boot_threshold(); // must return (a hang here hangs the test)
    TEST_CHECK(s_set_calls == 2, "one restore attempt only, never a boot-path retry loop");
    TEST_CHECK(s_reboot_calls == 0, "no reboot");
    TEST_CHECK(s_boot_part == &s_recovery,
               "otadata left falling through to the just-verified recovery image, not anything else");
}

static void test_success_reboots(void)
{
    TEST_SECTION("threshold: select succeeds -> reboot, no restore");
    reset_fakes();
    recovery_switch_at_boot_threshold();
    TEST_CHECK(s_boot_part == &s_recovery, "boot target is recovery");
    TEST_CHECK(s_set_calls == 1, "no restore after a successful select");
    TEST_CHECK(s_reboot_calls == 1, "reboots into recovery");
}

static void test_invalid_image_writes_nothing(void)
{
    TEST_SECTION("threshold: recovery image does not verify -> nothing written, nothing restored");
    reset_fakes();
    s_image_valid = false;
    recovery_switch_at_boot_threshold();
    TEST_CHECK(s_set_calls == 0, "otadata untouched");
    TEST_CHECK(s_boot_part == &s_app, "boot target unchanged");
    TEST_CHECK(s_reboot_calls == 0, "no reboot");
}

static void test_below_threshold_noop(void)
{
    TEST_SECTION("below threshold -> nothing at all");
    reset_fakes();
    s_threshold = false;
    s_set_mode_factory = SET_FAIL_AFTER_ERASE;
    recovery_switch_at_boot_threshold();
    TEST_CHECK(s_set_calls == 0, "otadata untouched");
    TEST_CHECK(s_reboot_calls == 0, "no reboot");
}

int main(void)
{
    test_set_fails_after_erase();
    test_readback_mismatch();
    test_restore_also_fails();
    test_success_reboots();
    test_invalid_image_writes_nothing();
    test_below_threshold_noop();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
