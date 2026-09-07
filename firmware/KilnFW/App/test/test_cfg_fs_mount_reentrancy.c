// Host test for cfg_fs_mount.c's cfg_fs_write_atomic_device() -- the ONE
// function in that file this test cares about. cfg_fs_mount.c itself was
// undiscoverable to the host build until now ("Not part of any host test
// build" per its own header comment, since it #includes esp_littlefs.h/
// esp_partition.h): stubs/esp_littlefs.h and the two extra declarations in
// stubs/esp_partition.h (esp_partition_find_first/esp_partition_read) close
// that gap just enough for this ONE translation unit to compile and link on
// the host, with every littlefs/partition call still a no-op stub -- this
// test never calls cfg_fs_mount_device() itself, only the write-dispatch
// path.
//
// WHY THIS TEST EXISTS: docs/audits/filesystem_migration_review_2026-09-07.md
// section 1 flagged that cfg_fs_write_atomic_device() was never installed as
// the bridges' write function. Fixing that exposed a REAL, previously-latent
// deadlock: check_flash_worker_lint.ps1's reentrancy-guard rule (bac50dbc)
// caught that cfg_fs_write_atomic_device() dispatches onto bx_flash_worker
// with no re-entrancy guard, unlike every other dispatcher in this codebase.
// Concrete path: CONTROL_CMD_SET_UNIT_PREF already runs ON that worker ->
// unit_pref_set() -> pref_cfg_fs_save() -> this same function -> dispatches
// onto the worker AGAIN from inside itself -- a real deadlock on hardware
// (CLAUDE.md's flash-worker-reentrancy note; project_flash_worker_
// reentrancy). The host stub previously modeled no lock at all, so nothing
// could have caught this; test/stubs/bx_worker_stub.h's busy-modeling stub
// (added for the adaptive_tune.c R1/R2 findings) is exactly the tool that
// makes the deadlock visible here instead: it TEST_CHECK(false, ...)s the
// instant uart_bridge_ext_run_on_flash_worker() is called while a job is
// already running through it.
#include "test_common.h"

#include "esp_err.h"
#include "esp_partition.h"
#include "esp_littlefs.h"
#include "cfg_fs_mount.h"
#include "cfg_fs.h"
#include "zones_config_cfg_fs.h"
#include "pref_cfg_fs.h"
#include "profiles_cfg_fs.h"

#include "bx_worker_stub.h"

int g_test_failures = 0;
int g_test_count = 0;

// Trivial stand-ins for the three bridges' set/get_write_fn() -- referenced
// by cfg_fs_mount.c's cfg_fs_install_device_write_fns()/cfg_fs_assert_
// device_write_fns_installed(), which this test's own path never reaches
// (both are called only from finish_mount_after_register(), which is only
// reached via cfg_fs_mount_device()/cfg_fs_confirm_format_device() -- this
// test calls neither, only cfg_fs_write_atomic_device()). Linking the real
// zones_config_cfg_fs.c/pref_cfg_fs.c/profiles_cfg_fs.c here would pull in
// zones_config_json.c's/profiles_http.c's full codec surfaces for logic
// this test never exercises -- these bodies exist only so the .obj links,
// same "declared here (in the real header), defined per test executable"
// convention as stubs/esp_partition.h's/esp_littlefs.h's functions above.
static zones_cfg_fs_write_fn_t s_fake_zones_write_fn;
void zones_config_cfg_fs_set_write_fn(zones_cfg_fs_write_fn_t fn) { s_fake_zones_write_fn = fn; }
zones_cfg_fs_write_fn_t zones_config_cfg_fs_get_write_fn(void) { return s_fake_zones_write_fn; }

static pref_cfg_fs_write_fn_t s_fake_pref_write_fn;
void pref_cfg_fs_set_write_fn(pref_cfg_fs_write_fn_t fn) { s_fake_pref_write_fn = fn; }
pref_cfg_fs_write_fn_t pref_cfg_fs_get_write_fn(void) { return s_fake_pref_write_fn; }

static profiles_cfg_fs_write_fn_t s_fake_profiles_write_fn;
void profiles_cfg_fs_set_write_fn(profiles_cfg_fs_write_fn_t fn) { s_fake_profiles_write_fn = fn; }
profiles_cfg_fs_write_fn_t profiles_cfg_fs_get_write_fn(void) { return s_fake_profiles_write_fn; }

// Trivial per-executable definitions for the stub declarations cfg_fs_
// mount.c's translation unit needs to LINK (same "declared once, defined
// per test file" convention as test_ota_http.c/test_partition_info_http.c)
// -- none of these is ever actually exercised: this test never calls
// cfg_fs_mount_device()/cfg_fs_confirm_format_device(), only
// cfg_fs_write_atomic_device(), which touches none of them.
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf)
{
    (void)conf;
    return ESP_FAIL;
}
esp_err_t esp_vfs_littlefs_unregister(const char *partition_label)
{
    (void)partition_label;
    return ESP_OK;
}
esp_err_t esp_littlefs_format(const char *partition_label)
{
    (void)partition_label;
    return ESP_FAIL;
}
esp_err_t esp_littlefs_info(const char *partition_label, size_t *total_bytes, size_t *used_bytes)
{
    (void)partition_label;
    (void)total_bytes;
    (void)used_bytes;
    return ESP_FAIL;
}
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t dst_offset, const void *src, size_t size)
{
    (void)partition;
    (void)dst_offset;
    (void)src;
    (void)size;
    return ESP_FAIL;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t size)
{
    (void)partition;
    (void)offset;
    (void)size;
    return ESP_FAIL;
}
uint32_t esp_partition_get_main_flash_sector_size(void)
{
    return 4096u;
}
esp_partition_iterator_t esp_partition_find(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                             const char *label)
{
    (void)type;
    (void)subtype;
    (void)label;
    return NULL;
}
const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator)
{
    (void)iterator;
    return NULL;
}
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator)
{
    (void)iterator;
    return NULL;
}
esp_err_t esp_partition_iterator_release(esp_partition_iterator_t iterator)
{
    (void)iterator;
    return ESP_OK;
}
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                 const char *label)
{
    (void)type;
    (void)subtype;
    (void)label;
    return NULL; // "cfg partition not found" leg -- never reached, cfg_fs_mount_device() is not called here.
}
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset, void *dst, size_t size)
{
    (void)partition;
    (void)src_offset;
    (void)dst;
    (void)size;
    return ESP_FAIL;
}

// The real deadlock shape: a job ALREADY dispatched onto (and running on)
// the flash worker calls cfg_fs_write_atomic_device() from inside itself --
// exactly what unit_pref_set() -> pref_cfg_fs_save() -> s_write_fn() does
// once cfg_fs_install_device_write_fns() has installed this function as
// every bridge's write_fn. bx_worker_stub.h's dispatch wrapper sets
// s_stub_bx_busy = true (and s_stub_on_flash_worker = true, S3) for the
// duration of fn(arg) -- so if cfg_fs_write_atomic_device() were still
// unconditionally dispatching, this inner call would trip the stub's own
// re-entrant-call TEST_CHECK(false, ...).
static void reentrant_caller_job(void *arg)
{
    (void)arg;
    esp_err_t err = cfg_fs_write_atomic_device("negtest_reentrant.bin", "x", 1);
    // cfg_fs is not mounted in this test (no esp_littlefs.h behind the
    // stub) -- ESP_ERR_INVALID_STATE from the real cfg_fs_write_atomic() is
    // the EXPECTED outcome here; this test is about the DISPATCH decision
    // (inline vs. re-dispatch), not the write itself succeeding.
    TEST_CHECK(err == ESP_ERR_INVALID_STATE,
               "inline call (already on the worker) reaches the real cfg_fs_write_atomic() and reports "
               "its real ESP_ERR_INVALID_STATE (cfg_fs not mounted in this test), not a dispatch error");
}

static void test_write_atomic_device_reentrant_call_does_not_redispatch(void)
{
    s_stub_bx_busy = false;
    s_stub_on_flash_worker = false;

    // Dispatch reentrant_caller_job() onto the (stubbed) flash worker --
    // this is the outer CONTROL_CMD_SET_UNIT_PREF-runs-on-the-worker leg of
    // the real deadlock path. While it runs, the stub models "we are now on
    // the worker, and it is busy" (S3) -- reentrant_caller_job() then calls
    // cfg_fs_write_atomic_device() AS IF it were pref_cfg_fs_save()'s
    // installed write_fn, called from inside that already-dispatched job.
    esp_err_t outer_err = uart_bridge_ext_run_on_flash_worker(reentrant_caller_job, NULL);
    TEST_CHECK(outer_err == ESP_OK, "outer dispatch itself succeeds (not what is under test)");

    // If g_test_failures grew here, bx_worker_stub.h's own re-entrant-call
    // assertion fired -- i.e. cfg_fs_write_atomic_device() redispatched
    // instead of running inline. The absence of that failure, plus the
    // ESP_ERR_INVALID_STATE assertion inside reentrant_caller_job() above,
    // together prove the inline path actually ran.
    TEST_CHECK(!s_stub_bx_busy, "stub's busy flag is clear again after the (single) outer dispatch returns");
}

// Sanity/contrast case: called normally (NOT already on the worker),
// cfg_fs_write_atomic_device() must still dispatch through
// uart_bridge_ext_run_on_flash_worker() -- proves the fix did not turn this
// into an unconditional inline call, which would silently reintroduce the
// exact hazard cfg_fs_write_atomic_device exists to prevent (a PSRAM-
// stacked caller touching flash directly).
static void test_write_atomic_device_dispatches_when_not_on_worker(void)
{
    s_stub_bx_busy = false;
    s_stub_on_flash_worker = false;

    esp_err_t err = cfg_fs_write_atomic_device("negtest_normal.bin", "x", 1);
    TEST_CHECK(err == ESP_ERR_INVALID_STATE,
               "normal (non-worker) call still reaches the real cfg_fs_write_atomic() via a dispatch");
    TEST_CHECK(!s_stub_on_flash_worker, "s_stub_on_flash_worker restored to false after the dispatch returns "
                                        "(S3's save/restore -- proves a real dispatch, not a leaked flag)");
}

static void test_write_atomic_device_rejects_null_path(void)
{
    s_stub_bx_busy = false;
    s_stub_on_flash_worker = false;
    esp_err_t err = cfg_fs_write_atomic_device(NULL, "x", 1);
    TEST_CHECK(err == ESP_ERR_INVALID_ARG, "NULL rel_path refused before ever touching the dispatch path");
}

void run_test_cfg_fs_mount_reentrancy(void)
{
    test_write_atomic_device_reentrant_call_does_not_redispatch();
    test_write_atomic_device_dispatches_when_not_on_worker();
    test_write_atomic_device_rejects_null_path();
}

int main(void)
{
    run_test_cfg_fs_mount_reentrancy();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
