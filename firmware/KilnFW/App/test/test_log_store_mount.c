// Host test for log_store_mount.c -- docs/FILESYSTEM_PLAN.md step 2.
//
// log_store_mount.c was "not part of any host test build" before this file
// (its own header comment on log_store_mount.h says so) because it
// #includes esp_spiffs.h/esp_littlefs.h and flash_worker.h, none of which
// exist off-target. stubs/esp_spiffs.h (new, this change) and
// stubs/esp_littlefs.h (already existed, added earlier for cfg_fs_mount.c)
// close that gap; bx_worker_stub.h supplies uart_bridge_ext_run_on_flash_
// worker() the same way every other flash-worker-dispatching driver's host
// test does.
//
// This is its own SEPARATE executable, built TWICE by build_host_tests.ps1
// -- once with CONFIG_KILNCTL_LOGS_LITTLEFS left undefined (the SPIFFS
// branch, matching the bench's default) and once with it defined to 1 (the
// LittleFS branch) -- proving log_store_mount.c's #if CONFIG_KILNCTL_LOGS_
// LITTLEFS branch actually compiles under BOTH flag states, per the plan's
// step 2 test requirement. log_store.c itself needs no host test change --
// it is plain stdio and already covered by test_log_store.c -- so this file
// only exercises the mount-branch selection, not rotation logic again.
#include "test_common.h"

#include <string.h>

#include "esp_err.h"
#include "esp_partition.h"
#if CONFIG_KILNCTL_LOGS_LITTLEFS
#include "esp_littlefs.h"
#else
#include "esp_spiffs.h"
#endif

#include "log_store.h"
#include "log_store_mount.h"

#include "bx_worker_stub.h"

int g_test_failures = 0;
int g_test_count = 0;

// Which real ESP-IDF VFS registration call log_store_mount() actually made,
// recorded by the stub bodies below rather than assumed -- the thing this
// whole test exists to pin down as the flag flips.
static int s_register_calls = 0;
static int s_info_calls = 0;

#if CONFIG_KILNCTL_LOGS_LITTLEFS

esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf)
{
    TEST_CHECK(conf != NULL, "log_store_mount() passes a non-NULL littlefs conf");
    TEST_CHECK(conf && conf->partition_label && strcmp(conf->partition_label, "logs") == 0,
               "littlefs conf keeps the `logs` partition label unchanged");
    TEST_CHECK(conf && strcmp(conf->base_path, "/logs") == 0,
               "littlefs conf mounts at the same /logs base path as the SPIFFS branch");
    TEST_CHECK(conf && conf->format_if_mount_failed,
               "littlefs branch keeps format_if_mount_failed=true, same recovery policy as SPIFFS");
    s_register_calls++;
    return ESP_OK;
}
esp_err_t esp_vfs_littlefs_unregister(const char *partition_label)
{
    (void)partition_label;
    return ESP_OK;
}
esp_err_t esp_littlefs_format(const char *partition_label)
{
    (void)partition_label;
    return ESP_OK;
}
esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes)
{
    (void)partition_label;
    if (total_bytes) *total_bytes = 0;
    if (used_bytes) *used_bytes = 0;
    s_info_calls++;
    return ESP_OK;
}

#else

esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *conf)
{
    TEST_CHECK(conf != NULL, "log_store_mount() passes a non-NULL spiffs conf");
    TEST_CHECK(conf && conf->partition_label && strcmp(conf->partition_label, "logs") == 0,
               "spiffs conf keeps the `logs` partition label unchanged");
    TEST_CHECK(conf && strcmp(conf->base_path, "/logs") == 0, "spiffs conf mounts at /logs");
    TEST_CHECK(conf && conf->format_if_mount_failed, "spiffs branch's format_if_mount_failed stays true "
                                                       "(this test only ever exercised the pre-existing "
                                                       "default branch, never regressed by this change)");
    s_register_calls++;
    return ESP_OK;
}
esp_err_t esp_spiffs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes)
{
    (void)partition_label;
    if (total_bytes) *total_bytes = 0;
    if (used_bytes) *used_bytes = 0;
    s_info_calls++;
    return ESP_OK;
}

#endif

static void test_mount_selects_the_right_branch_and_is_idempotent(void)
{
    s_register_calls = 0;
    s_info_calls = 0;

    esp_err_t err1 = log_store_mount();
    TEST_CHECK(err1 == ESP_OK, "first log_store_mount() call succeeds");
    TEST_CHECK(s_register_calls == 1, "exactly one VFS register call happened on first mount");
    TEST_CHECK(log_store_is_init(), "log_store_init(/logs) ran as part of the mount");

    esp_err_t err2 = log_store_mount();
    TEST_CHECK(err2 == ESP_OK, "second log_store_mount() call also reports success (idempotent)");
    TEST_CHECK(s_register_calls == 1, "second call is a no-op -- does NOT re-register the VFS "
                                       "(s_mounted guard in log_store_mount.c)");
}

static void test_write_event_dispatches_through_the_flash_worker(void)
{
    unsigned before = s_stub_dispatch_count;
    const char payload[4] = { 1, 2, 3, 4 };
    esp_err_t err = log_store_write_event(LOG_STORE_KIND_FIRING, payload, sizeof(payload));
    TEST_CHECK(err == ESP_OK, "log_store_write_event() succeeds once mounted");
    TEST_CHECK(s_stub_dispatch_count == before + 1,
               "the append runs via uart_bridge_ext_run_on_flash_worker(), never the caller's own task "
               "-- same PSRAM-stack-safety reasoning as every other flash-touching driver here");
}

int main(void)
{
    test_mount_selects_the_right_branch_and_is_idempotent();
    test_write_event_dispatches_through_the_flash_worker();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
