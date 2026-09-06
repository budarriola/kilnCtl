/* test_fake_kv.c -- standalone MSVC host test for hwAbstraction/host/fake_kv.c.
 * Compiled and run by test_host_fakes.ps1.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "fake_kv.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

int main(void)
{
    fake_kv_reset_all();

    /* --- open before init_partition -> HAL_NOT_READY --- */
    hal_kv_handle_t h;
    memset(&h, 0, sizeof(h));
    CHECK(hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_WRITE, "kiln_nvs") == HAL_NOT_READY);

    /* --- uninitialized/garbage handle: every call -> HAL_NOT_READY --- */
    hal_kv_handle_t garbage;
    memset(&garbage, 0xCD, sizeof(garbage));
    CHECK(fake_kv_handle_is_live(&garbage) == false);
    CHECK(hal_kv_close(&garbage) == HAL_NOT_READY);
    CHECK(hal_kv_commit(&garbage) == HAL_NOT_READY);
    size_t len = 0;
    CHECK(hal_kv_get_blob(&garbage, "k", NULL, &len) == HAL_NOT_READY);
    CHECK(hal_kv_set_blob(&garbage, "k", "v", 1) == HAL_NOT_READY);
    CHECK(hal_kv_erase_key(&garbage, "k") == HAL_NOT_READY);

    /* --- init_partition is idempotent --- */
    CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK);
    CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK);

    /* --- open: bad args --- */
    CHECK(hal_kv_open(NULL, "kiln_cfg", HAL_KV_MODE_READ_WRITE, "kiln_nvs") == HAL_INVALID_ARG);
    CHECK(hal_kv_open(&h, NULL, HAL_KV_MODE_READ_WRITE, "kiln_nvs") == HAL_INVALID_ARG);

    /* --- READ_ONLY open on a namespace that doesn't exist yet -> HAL_NOT_FOUND --- */
    CHECK(hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_ONLY, "kiln_nvs") == HAL_NOT_FOUND);

    /* --- open READ_WRITE creates the namespace --- */
    CHECK(hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_WRITE, "kiln_nvs") == HAL_OK);
    CHECK(fake_kv_handle_is_live(&h) == true);

    /* --- get on absent key -> HAL_NOT_FOUND --- */
    len = 8;
    uint8_t buf8[8];
    CHECK(hal_kv_get_blob(&h, "missing", buf8, &len) == HAL_NOT_FOUND);

    /* --- set_blob then get_blob WITHOUT commit is visible (real-NVS shape:
     * uncommitted writes are visible to reads, only not durable) --- */
    uint8_t val[4] = {1, 2, 3, 4};
    CHECK(hal_kv_set_blob(&h, "cal", val, sizeof(val)) == HAL_OK);
    CHECK(fake_kv_has_uncommitted_writes("kiln_nvs") == true);

    /* size probe (buf == NULL) */
    len = 0;
    CHECK(hal_kv_get_blob(&h, "cal", NULL, &len) == HAL_OK);
    CHECK(len == sizeof(val));

    /* buffer too small -> HAL_INVALID_SIZE */
    uint8_t small[2];
    len = sizeof(small);
    CHECK(hal_kv_get_blob(&h, "cal", small, &len) == HAL_INVALID_SIZE);

    /* correctly sized read */
    uint8_t got[4] = {0};
    len = sizeof(got);
    CHECK(hal_kv_get_blob(&h, "cal", got, &len) == HAL_OK);
    CHECK(len == sizeof(val) && memcmp(got, val, sizeof(val)) == 0);

    /* --- durability: power loss before commit discards the write --- */
    fake_kv_simulate_power_loss();
    CHECK(fake_kv_has_uncommitted_writes("kiln_nvs") == false);
    len = sizeof(got);
    CHECK(hal_kv_get_blob(&h, "cal", got, &len) == HAL_NOT_FOUND);

    /* --- durability: commit before power loss survives it --- */
    CHECK(hal_kv_set_blob(&h, "cal", val, sizeof(val)) == HAL_OK);
    CHECK(hal_kv_commit(&h) == HAL_OK);
    CHECK(fake_kv_has_uncommitted_writes("kiln_nvs") == false);
    fake_kv_simulate_power_loss();
    len = sizeof(got);
    CHECK(hal_kv_get_blob(&h, "cal", got, &len) == HAL_OK);
    CHECK(memcmp(got, val, sizeof(val)) == 0);

    /* --- set_str / get_str round trip --- */
    CHECK(hal_kv_set_str(&h, "name", "kiln1") == HAL_OK);
    CHECK(hal_kv_commit(&h) == HAL_OK);
    char strbuf[16];
    len = sizeof(strbuf);
    CHECK(hal_kv_get_str(&h, "name", strbuf, &len) == HAL_OK);
    CHECK(strcmp(strbuf, "kiln1") == 0);
    CHECK(len == strlen("kiln1") + 1);

    /* size probe for str */
    len = 0;
    CHECK(hal_kv_get_str(&h, "name", NULL, &len) == HAL_OK);
    CHECK(len == strlen("kiln1") + 1);

    /* --- wrong-type error injection: get_str on a blob key, and vice versa --- */
    CHECK(hal_kv_get_str(&h, "cal", strbuf, &len) == HAL_INVALID_ARG);
    len = sizeof(strbuf);
    CHECK(hal_kv_get_blob(&h, "name", strbuf, &len) == HAL_OK); /* blob accepts either */
    CHECK(len == strlen("kiln1") + 1 && memcmp(strbuf, "kiln1", len) == 0);

    /* --- set_str(NULL) -> HAL_INVALID_ARG --- */
    CHECK(hal_kv_set_str(&h, "name", NULL) == HAL_INVALID_ARG);

    /* --- erase_key --- */
    CHECK(hal_kv_erase_key(&h, "name") == HAL_OK);
    CHECK(fake_kv_has_uncommitted_writes("kiln_nvs") == true); /* tombstone pending */
    len = sizeof(strbuf);
    CHECK(hal_kv_get_str(&h, "name", strbuf, &len) == HAL_NOT_FOUND); /* visible pre-commit */
    CHECK(hal_kv_erase_key(&h, "name") == HAL_NOT_FOUND); /* already gone */
    CHECK(hal_kv_commit(&h) == HAL_OK);
    len = sizeof(strbuf);
    CHECK(hal_kv_get_str(&h, "name", strbuf, &len) == HAL_NOT_FOUND); /* still gone after commit */

    /* --- corruption injection --- */
    CHECK(hal_kv_set_blob(&h, "corrupt_me", val, sizeof(val)) == HAL_OK);
    CHECK(hal_kv_commit(&h) == HAL_OK);
    CHECK(fake_kv_script_corrupt_key("kiln_nvs", "kiln_cfg", "corrupt_me") == true);
    len = sizeof(got);
    CHECK(hal_kv_get_blob(&h, "corrupt_me", got, &len) == HAL_IO);
    /* corrupting a nonexistent key is a no-op, reports false */
    CHECK(fake_kv_script_corrupt_key("kiln_nvs", "kiln_cfg", "never_written") == false);
    /* overwriting + committing clears corruption */
    CHECK(hal_kv_set_blob(&h, "corrupt_me", val, sizeof(val)) == HAL_OK);
    CHECK(hal_kv_commit(&h) == HAL_OK);
    len = sizeof(got);
    CHECK(hal_kv_get_blob(&h, "corrupt_me", got, &len) == HAL_OK);

    /* --- injected write failure (HAL_NO_MEM / HAL_IO on next write) --- */
    fake_kv_script_next_write_status(HAL_NO_MEM);
    CHECK(hal_kv_set_blob(&h, "nospace", val, 1) == HAL_NO_MEM);
    /* one-shot: reverted afterward */
    CHECK(hal_kv_set_blob(&h, "nospace", val, 1) == HAL_OK);
    fake_kv_script_next_write_status(HAL_IO);
    CHECK(hal_kv_commit(&h) == HAL_IO);
    CHECK(hal_kv_commit(&h) == HAL_OK); /* reverted, actually commits now */

    /* --- READ_ONLY handle rejects writes, allows reads --- */
    hal_kv_handle_t ro;
    memset(&ro, 0, sizeof(ro));
    CHECK(hal_kv_open(&ro, "kiln_cfg", HAL_KV_MODE_READ_ONLY, "kiln_nvs") == HAL_OK);
    CHECK(hal_kv_set_blob(&ro, "cal", val, sizeof(val)) == HAL_INVALID_ARG);
    CHECK(hal_kv_erase_key(&ro, "cal") == HAL_INVALID_ARG);
    len = sizeof(got);
    CHECK(hal_kv_get_blob(&ro, "cal", got, &len) == HAL_OK);
    CHECK(hal_kv_close(&ro) == HAL_OK);

    /* --- key pool exhaustion within one namespace --- */
    fake_kv_reset_all();
    CHECK(hal_kv_init_partition("wifi_nvs") == HAL_OK);
    hal_kv_handle_t wh;
    memset(&wh, 0, sizeof(wh));
    CHECK(hal_kv_open(&wh, "wifi_cfg", HAL_KV_MODE_READ_WRITE, "wifi_nvs") == HAL_OK);
    int ok = 0;
    for (int i = 0; i < FAKE_KV_MAX_KEYS_PER_NS + 2; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%d", i);
        hal_status_t st = hal_kv_set_blob(&wh, key, "x", 1);
        if (st == HAL_OK) ok++;
        else { CHECK(st == HAL_NO_MEM); }
    }
    CHECK(ok == FAKE_KV_MAX_KEYS_PER_NS);

    /* --- stats --- */
    hal_kv_stats_t stats;
    CHECK(hal_kv_stats("wifi_nvs", &stats) == HAL_OK);
    CHECK(stats.used_entries == (size_t)FAKE_KV_MAX_KEYS_PER_NS);
    CHECK(stats.total_entries == stats.used_entries + stats.free_entries);
    CHECK(hal_kv_stats("does_not_exist", &stats) == HAL_NOT_FOUND);
    CHECK(hal_kv_stats("wifi_nvs", NULL) == HAL_INVALID_ARG);

    /* --- erase_partition wipes data but leaves the partition usable --- */
    CHECK(hal_kv_erase_partition("wifi_nvs") == HAL_OK);
    CHECK(hal_kv_stats("wifi_nvs", &stats) == HAL_OK);
    CHECK(stats.used_entries == 0);
    CHECK(hal_kv_erase_partition("never_initialized") == HAL_NOT_FOUND);

    /* namespace was cleared by erase_partition -- reopening READ_WRITE
     * recreates it, and the old data is gone */
    hal_kv_handle_t wh2;
    memset(&wh2, 0, sizeof(wh2));
    CHECK(hal_kv_open(&wh2, "wifi_cfg", HAL_KV_MODE_READ_WRITE, "wifi_nvs") == HAL_OK);
    len = sizeof(got);
    CHECK(hal_kv_get_blob(&wh2, "k0", got, &len) == HAL_NOT_FOUND);

    /* --- default partition (NULL) is distinct from a named one --- */
    fake_kv_reset_all();
    CHECK(hal_kv_open(&h, "boot_guard", HAL_KV_MODE_READ_WRITE, NULL) == HAL_NOT_READY);
    CHECK(hal_kv_init_partition(NULL) == HAL_OK);
    CHECK(hal_kv_open(&h, "boot_guard", HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK);
    CHECK(hal_kv_set_blob(&h, "flag", "1", 1) == HAL_OK);
    CHECK(hal_kv_commit(&h) == HAL_OK);
    CHECK(fake_kv_has_uncommitted_writes(NULL) == false);
    CHECK(fake_kv_has_uncommitted_writes("kiln_nvs") == false); /* different, uninitialized partition */

    /* --- write_safe_here test hook --- */
    CHECK(hal_kv_write_safe_here() == true);
    fake_kv_set_write_safe_here(false);
    CHECK(hal_kv_write_safe_here() == false);
    fake_kv_reset_all();
    CHECK(hal_kv_write_safe_here() == true); /* reset restores default */

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
