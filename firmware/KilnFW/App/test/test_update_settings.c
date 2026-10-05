// Host tests for App/drivers/update/update_settings.c -- the persisted
// "update repo" setting (docs/GITHUB_RELEASE_UPDATE_PLAN.md WP9). The driver
// is linked once as a real object (test_backup_import.c needs it too), so this
// file reaches it only through its public header plus the fake NVS/cfg_fs.
//
// Load-bearing properties: the validator refuses everything outside
// owner/name; an unset or unreadable setting always reads as the default,
// never garbage; a set survives a simulated reboot; an invalid set changes
// nothing in RAM or flash.
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TUS_MKDIR(p) _mkdir(p)
#define TUS_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TUS_MKDIR(p) mkdir((p), 0755)
#define TUS_RMDIR(p) rmdir(p)
#endif

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "cfg_fs.h"
#include "pref_cfg_fs.h"
#include "update_settings.h"

static const char *TUS_SCRATCH_BASE = "cfg_fs_test_update_settings";

// Mirrors update_settings.c's private blob layout and keys (64 bytes).
typedef struct {
    uint8_t version;
    char repo[63];
} tus_blob_t;

static void tus_cfg_fs_reset(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", TUS_SCRATCH_BASE, UPDATE_SETTINGS_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", TUS_SCRATCH_BASE, UPDATE_SETTINGS_FILE_PATH);
    remove(path);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", TUS_SCRATCH_BASE);
    TUS_RMDIR(tmp);
    TUS_RMDIR(TUS_SCRATCH_BASE);
    TUS_MKDIR(TUS_SCRATCH_BASE);
    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
}

static void tus_boot(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition("kiln_nvs");
    update_settings_reset_ram_for_test();
}

static void tus_reboot(void)
{
    update_settings_reset_ram_for_test();
    update_settings_start();
}

static void tus_put_nvs_blob(const void *bytes, size_t len, uint32_t rev)
{
    hal_kv_handle_t h;
    hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_WRITE, "kiln_nvs");
    hal_kv_set_blob(&h, "update_repo", bytes, len);
    hal_kv_set_u32(&h, "upd_repo_rev", rev);
    hal_kv_commit(&h);
    hal_kv_close(&h);
}

static void test_validator(void)
{
    TEST_CHECK(update_settings_repo_is_valid("budarriola/kilnCtl"), "default repo is valid");
    TEST_CHECK(update_settings_repo_is_valid("a/b"), "shortest owner/name is valid");
    TEST_CHECK(update_settings_repo_is_valid("A-b.c_d/e.f-g_h"), "mixed allowed characters are valid");
    TEST_CHECK(update_settings_repo_is_valid("a-b/-name"), "a name may start with '-' (owner may not)");

    static const char *const bad[] = {
        "", "a", "ab", "/", "a/", "/b", "a//b", "a/b/c", "noslash", "-a/b", "a-/b", "a b/c", "a/b c", "a/b?x=1",
        "a/b#frag", "a/b\\c", "a:b/c", "http://x/y", "a/../b", "a/b..c", "..a/b", "a/b%2F", "own\xc3\xa9r/n",
        "a/b\n", "a/b\"", "a/b'", "a@b/c", "a/b;c",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "validator refuses bad input #%u", (unsigned)i);
        TEST_CHECK(!update_settings_repo_is_valid(bad[i]), msg);
    }
    TEST_CHECK(!update_settings_repo_is_valid(NULL), "NULL is invalid");

    // Length bounds: owner 39, whole string 62.
    char owner40[64];
    memset(owner40, 'o', 40);
    strcpy(owner40 + 40, "/n");
    TEST_CHECK(!update_settings_repo_is_valid(owner40), "40-character owner is refused (GitHub max 39)");
    char owner39[64];
    memset(owner39, 'o', 39);
    strcpy(owner39 + 39, "/n");
    TEST_CHECK(update_settings_repo_is_valid(owner39), "39-character owner is accepted");

    char s62[80];
    memset(s62, 'n', sizeof(s62));
    s62[0] = 'o';
    s62[1] = '/';
    s62[62] = '\0';
    TEST_CHECK(update_settings_repo_is_valid(s62), "62-character total length is accepted");
    s62[62] = 'n';
    s62[63] = '\0';
    TEST_CHECK(!update_settings_repo_is_valid(s62), "63-character total length is refused");
}

static void test_default_when_unset(void)
{
    tus_boot();
    TEST_CHECK(strcmp(update_settings_repo(), UPDATE_SETTINGS_DEFAULT_REPO) == 0,
               "before start(): the accessor returns the default");
    TEST_CHECK(update_settings_start() == ESP_OK, "start() against empty NVS succeeds");
    TEST_CHECK(strcmp(update_settings_repo(), UPDATE_SETTINGS_DEFAULT_REPO) == 0, "empty NVS: the default");
    TEST_CHECK(update_settings_repo_is_default(), "empty NVS: is_default is true");
    TEST_CHECK(update_settings_repo_is_valid(update_settings_repo()), "the default is itself a valid repo");
}

static void test_round_trip_and_reset(void)
{
    tus_boot();
    update_settings_start();
    TEST_CHECK(update_settings_set("someone/fork-of-kiln") == ESP_OK, "set(valid) succeeds");
    TEST_CHECK(strcmp(update_settings_repo(), "someone/fork-of-kiln") == 0, "in-RAM value updates immediately");
    TEST_CHECK(!update_settings_repo_is_default(), "non-default repo: is_default false");

    tus_reboot();
    TEST_CHECK(strcmp(update_settings_repo(), "someone/fork-of-kiln") == 0, "value survives a simulated reboot");

    TEST_CHECK(update_settings_set("") == ESP_OK, "set(\"\") resets to the default");
    TEST_CHECK(strcmp(update_settings_repo(), UPDATE_SETTINGS_DEFAULT_REPO) == 0, "reset: default in RAM");
    tus_reboot();
    TEST_CHECK(strcmp(update_settings_repo(), UPDATE_SETTINGS_DEFAULT_REPO) == 0, "reset: default after a reboot");
    TEST_CHECK(update_settings_set(NULL) == ESP_OK, "set(NULL) is also a reset");

    // Setting the default spelled out reads back as default too.
    TEST_CHECK(update_settings_set(UPDATE_SETTINGS_DEFAULT_REPO) == ESP_OK, "set(default string) succeeds");
    TEST_CHECK(update_settings_repo_is_default(), "default string: is_default true");
}

static void test_invalid_set_changes_nothing(void)
{
    tus_boot();
    update_settings_start();
    update_settings_set("keep/this-one");
    const char *bad[] = { "nope", "a/b/c", "http://evil/x", "-a/b", "a/../b", "a/b c" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_CHECK(update_settings_set(bad[i]) == ESP_ERR_INVALID_ARG, "invalid set is refused with INVALID_ARG");
        TEST_CHECK(strcmp(update_settings_repo(), "keep/this-one") == 0, "refused set leaves RAM untouched");
    }
    tus_reboot();
    TEST_CHECK(strcmp(update_settings_repo(), "keep/this-one") == 0, "refused sets left flash untouched too");
}

static void test_pointer_stays_valid_across_set(void)
{
    tus_boot();
    update_settings_start();
    update_settings_set("first/one");
    const char *p = update_settings_repo();
    update_settings_set("second/two");
    TEST_CHECK(strcmp(p, "first/one") == 0, "a pointer taken before one set still reads a whole string");
    TEST_CHECK(strcmp(update_settings_repo(), "second/two") == 0, "the accessor now returns the new value");
}

static void test_corrupt_storage_falls_back_to_default(void)
{
    tus_boot();
    tus_blob_t good;
    memset(&good, 0, sizeof(good));
    good.version = 1;
    strcpy(good.repo, "a/b");

    // wrong size
    tus_put_nvs_blob(&good, sizeof(good) - 1, 1);
    tus_reboot();
    TEST_CHECK(strcmp(update_settings_repo(), UPDATE_SETTINGS_DEFAULT_REPO) == 0, "short blob: default");

    // wrong version
    tus_blob_t v = good;
    v.version = 9;
    tus_put_nvs_blob(&v, sizeof(v), 1);
    tus_reboot();
    TEST_CHECK(strcmp(update_settings_repo(), UPDATE_SETTINGS_DEFAULT_REPO) == 0, "unknown version: default");

    // well-formed blob, invalid repo inside
    tus_blob_t inv = good;
    strcpy(inv.repo, "not a repo");
    tus_put_nvs_blob(&inv, sizeof(inv), 1);
    tus_reboot();
    TEST_CHECK(strcmp(update_settings_repo(), UPDATE_SETTINGS_DEFAULT_REPO) == 0, "invalid repo in blob: default");

    // no NUL terminator
    tus_blob_t nonul = good;
    memset(nonul.repo, 'a', sizeof(nonul.repo));
    tus_put_nvs_blob(&nonul, sizeof(nonul), 1);
    tus_reboot();
    TEST_CHECK(strcmp(update_settings_repo(), UPDATE_SETTINGS_DEFAULT_REPO) == 0, "unterminated blob: default");

    // garbage after the terminator
    tus_blob_t junk = good;
    junk.repo[10] = 'x';
    tus_put_nvs_blob(&junk, sizeof(junk), 1);
    tus_reboot();
    TEST_CHECK(strcmp(update_settings_repo(), UPDATE_SETTINGS_DEFAULT_REPO) == 0, "garbage after NUL: default");

    // sanity: the good blob is accepted by the same path
    tus_put_nvs_blob(&good, sizeof(good), 1);
    tus_reboot();
    TEST_CHECK(strcmp(update_settings_repo(), "a/b") == 0, "control: a well-formed blob loads");
}

static void test_dual_write_and_divergence(void)
{
    tus_cfg_fs_reset();
    tus_boot();
    TEST_CHECK(cfg_fs_init(TUS_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts against the scratch dir");
    update_settings_start();
    TEST_CHECK(update_settings_set("dual/write") == ESP_OK, "set() succeeds with cfg_fs mounted");
    bool exists = false;
    cfg_fs_exists(UPDATE_SETTINGS_FILE_PATH, &exists);
    TEST_CHECK(exists, "the cfg file was written alongside NVS");

    // NVS gets a newer value behind the file's back: higher rev wins and the file is resynced.
    tus_blob_t newer;
    memset(&newer, 0, sizeof(newer));
    newer.version = 1;
    strcpy(newer.repo, "newer/nvs");
    tus_put_nvs_blob(&newer, sizeof(newer), 9);
    tus_reboot();
    TEST_CHECK(strcmp(update_settings_repo(), "newer/nvs") == 0, "higher-rev NVS wins over the older file");

    // File-only value (NVS wiped, as after a lost NVS partition): the file is adopted.
    tus_reboot();
    hal_kv_handle_t h;
    hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_WRITE, "kiln_nvs");
    hal_kv_erase_key(&h, "update_repo");
    hal_kv_erase_key(&h, "upd_repo_rev");
    hal_kv_commit(&h);
    hal_kv_close(&h);
    tus_reboot();
    TEST_CHECK(strcmp(update_settings_repo(), "newer/nvs") == 0, "NVS empty: the (resynced) file value is adopted");
    cfg_fs_deinit();
}

static void test_mount_failed_is_nvs_only(void)
{
    tus_cfg_fs_reset();
    tus_boot();
    update_settings_start();
    update_settings_set("nvs/only");
    TEST_CHECK(cfg_fs_init("this_directory_does_not_exist_at_all", NULL) != ESP_OK, "mount fails as documented");
    tus_reboot();
    TEST_CHECK(strcmp(update_settings_repo(), "nvs/only") == 0, "unmounted cfg: NVS value still loads");
    cfg_fs_deinit();
}

void run_test_update_settings(void)
{
    test_validator();
    test_default_when_unset();
    test_round_trip_and_reset();
    test_invalid_set_changes_nothing();
    test_pointer_stays_valid_across_set();
    test_corrupt_storage_falls_back_to_default();
    test_dual_write_and_divergence();
    test_mount_failed_is_nvs_only();

    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
    update_settings_reset_ram_for_test();
    fake_kv_reset_all();
}
