/* test_fake_kv.c -- standalone MSVC host test for hwAbstraction/host/fake_kv.c.
 * Compiled and run by test_host_fakes.ps1.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "fake_kv.h"

static int g_pass = 0, g_fail = 0;

/* Mutation fence (hal_kv.h): refuse writes on the "kiln_nvs" partition only. */
static bool s_fence_on = false;
static bool fence_pred(const char *partition)
{
    return s_fence_on && partition != NULL && strcmp(partition, "kiln_nvs") == 0;
}

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

    /* --- durability: power loss before commit KEEPS the write by default
     * (hal_kv.h: a set MAY already be durable before commit -- real NVS
     * writes to flash immediately) --- */
    fake_kv_simulate_power_loss();
    CHECK(fake_kv_has_uncommitted_writes("kiln_nvs") == false);
    len = sizeof(got);
    CHECK(hal_kv_get_blob(&h, "cal", got, &len) == HAL_OK);
    CHECK(memcmp(got, val, sizeof(val)) == 0);

    /* --- opt-in lossy mode: the OLD over-approximating behavior, power
     * loss before commit discards the write. Reuses the already-committed
     * "cal" key (rather than a new one) so this does not consume one of
     * kiln_cfg's FAKE_KV_MAX_KEYS_PER_NS key slots, which the later
     * "nospace" write-failure-injection test below depends on having free. */
    uint8_t val2[4] = {9, 9, 9, 9};
    CHECK(hal_kv_set_blob(&h, "cal", val2, sizeof(val2)) == HAL_OK); /* pending overwrite */
    fake_kv_set_lossy_uncommitted(true);
    fake_kv_simulate_power_loss();
    CHECK(fake_kv_has_uncommitted_writes("kiln_nvs") == false);
    len = sizeof(got);
    CHECK(hal_kv_get_blob(&h, "cal", got, &len) == HAL_OK); /* reverts to the old committed value */
    CHECK(memcmp(got, val, sizeof(val)) == 0);
    fake_kv_set_lossy_uncommitted(false);

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
    CHECK(hal_kv_get_str(&h, "cal", strbuf, &len) == HAL_NOT_FOUND); /* as on target (IDF 6.0.2) */
    CHECK(hal_kv_key_exists(&h, "cal") == HAL_OK);
    CHECK(hal_kv_key_exists(&h, "no_such_key") == HAL_NOT_FOUND);
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

    /* --- fake_kv_set_lossy_uncommitted default is false (keep-on-power-loss),
     * and reset_all restores it --- */
    CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK);
    hal_kv_handle_t h2;
    memset(&h2, 0, sizeof(h2));
    CHECK(hal_kv_open(&h2, "kiln_cfg", HAL_KV_MODE_READ_WRITE, "kiln_nvs") == HAL_OK);
    CHECK(hal_kv_set_blob(&h2, "cal", val, sizeof(val)) == HAL_OK);
    fake_kv_simulate_power_loss(); /* default: kept */
    len = sizeof(got);
    CHECK(hal_kv_get_blob(&h2, "cal", got, &len) == HAL_OK);
    fake_kv_reset_all();

    /* --- overlong key (>= FAKE_KV_MAX_KEY_LEN, 16) is rejected up front on
     * every entry point, matching the real backend's ESP_ERR_NVS_KEY_TOO_LONG
     * -> HAL_IO mapping (hal_kv_esp_err_to_status() has no explicit case for
     * it, so it falls through to hal_esp_err_to_status()'s default). Before
     * this fix, do_set()/find_key() silently truncated the key via
     * copy_bounded(), so a set "succeeded" but every later get/erase on the
     * same overlong key compared the FULL key against the truncated stored
     * name and missed -- burning a fresh key slot per call. A 15-char key
     * (one under the limit) must still work normally. --- */
    CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK);
    hal_kv_handle_t h3;
    memset(&h3, 0, sizeof(h3));
    CHECK(hal_kv_open(&h3, "long_keys", HAL_KV_MODE_READ_WRITE, "kiln_nvs") == HAL_OK);
    const char *key16 = "0123456789abcdef";       /* 16 chars: at the limit, rejected */
    const char *key15 = "0123456789abcde";        /* 15 chars: fits, accepted */
    CHECK(strlen(key16) == 16);
    CHECK(strlen(key15) == 15);
    CHECK(hal_kv_set_blob(&h3, key16, "x", 1) == HAL_IO);
    len = sizeof(buf8);
    CHECK(hal_kv_get_blob(&h3, key16, buf8, &len) == HAL_IO);
    CHECK(hal_kv_erase_key(&h3, key16) == HAL_IO);
    CHECK(hal_kv_set_blob(&h3, key15, "y", 1) == HAL_OK);
    len = sizeof(buf8);
    CHECK(hal_kv_get_blob(&h3, key15, buf8, &len) == HAL_OK);
    CHECK(len == 1 && buf8[0] == 'y');
    fake_kv_reset_all();

    /* --- kiln_cfg's real worst case: PROFILES_MAX_COUNT=100 "profN" keys +
     * "prof_used" + "prof_favusr"/"prof_favbi" = 103 keys live at once in one
     * namespace, matching profiles_http.c/profiles_favorites.c's actual
     * NVS layout (see fake_kv.c's FAKE_KV_KNOWN_WORST_CASE_KILN_CFG_KEYS).
     * This is the scenario the 12-key cap used to strand silently partway
     * through (a caller looping profN writes without checking every return
     * would have just lost slots 12..99); with the cap now 128, every one of
     * these 103 keys must actually be stored, not merely accepted with a
     * later slot quietly overwritten. --- */
    CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK);
    hal_kv_handle_t hp;
    memset(&hp, 0, sizeof(hp));
    CHECK(hal_kv_open(&hp, "kiln_cfg", HAL_KV_MODE_READ_WRITE, "kiln_nvs") == HAL_OK);
    uint8_t used_bitmap[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    CHECK(hal_kv_set_blob(&hp, "prof_used", used_bitmap, sizeof(used_bitmap)) == HAL_OK);
    CHECK(hal_kv_set_blob(&hp, "prof_favusr", used_bitmap, sizeof(used_bitmap)) == HAL_OK);
    CHECK(hal_kv_set_blob(&hp, "prof_favbi", used_bitmap, sizeof(used_bitmap)) == HAL_OK);
    for (int i = 0; i < 100; i++) {
        char key[16];
        snprintf(key, sizeof(key), "prof%d", i);
        uint8_t val = (uint8_t)(i & 0xFF);
        CHECK(hal_kv_set_blob(&hp, key, &val, 1) == HAL_OK);
    }
    CHECK(hal_kv_commit(&hp) == HAL_OK);
    /* Read every one of the 100 profN keys back and confirm none were
     * silently dropped or overwritten by a later key colliding into a slot a
     * too-small cap had already recycled. */
    for (int i = 0; i < 100; i++) {
        char key[16];
        snprintf(key, sizeof(key), "prof%d", i);
        uint8_t val = 0xFF;
        size_t vlen = sizeof(val);
        CHECK(hal_kv_get_blob(&hp, key, &val, &vlen) == HAL_OK);
        CHECK(vlen == 1 && val == (uint8_t)(i & 0xFF));
    }
    size_t bmlen = sizeof(used_bitmap);
    uint8_t got_bitmap[4] = {0};
    CHECK(hal_kv_get_blob(&hp, "prof_used", got_bitmap, &bmlen) == HAL_OK);
    CHECK(memcmp(got_bitmap, used_bitmap, sizeof(used_bitmap)) == 0);
    hal_kv_stats_t prof_stats;
    CHECK(hal_kv_stats("kiln_nvs", &prof_stats) == HAL_OK);
    CHECK(prof_stats.used_entries == 103); /* 100 profN + prof_used + prof_favusr + prof_favbi */
    fake_kv_reset_all();

    /* --- mutation fence: refuses set/erase/commit on kiln_nvs while the predicate says so; reads, other
     * partitions and a cleared predicate are untouched --- */
    {
        hal_kv_handle_t fk, fo;
        CHECK(hal_kv_init_partition("kiln_nvs") == HAL_OK);
        CHECK(hal_kv_init_partition(NULL) == HAL_OK);
        CHECK(hal_kv_open(&fk, "fence_ns", HAL_KV_MODE_READ_WRITE, "kiln_nvs") == HAL_OK);
        CHECK(hal_kv_open(&fo, "fence_ns", HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK);
        CHECK(hal_kv_set_u32(&fk, "a", 1) == HAL_OK);
        CHECK(hal_kv_commit(&fk) == HAL_OK);
        hal_kv_set_write_refuse_hook(fence_pred);
        s_fence_on = true;
        CHECK(hal_kv_set_blob(&fk, "b", "x", 1) == HAL_NOT_READY);
        CHECK(hal_kv_set_str(&fk, "s", "x") == HAL_NOT_READY);
        CHECK(hal_kv_set_u32(&fk, "a", 2) == HAL_NOT_READY);
        CHECK(hal_kv_set_u8(&fk, "u", 2) == HAL_NOT_READY);
        CHECK(hal_kv_erase_key(&fk, "a") == HAL_NOT_READY);
        CHECK(hal_kv_commit(&fk) == HAL_NOT_READY);
        uint32_t got = 0;
        CHECK(hal_kv_get_u32(&fk, "a", &got) == HAL_OK && got == 1); /* nothing changed, reads allowed */
        CHECK(hal_kv_set_u32(&fo, "a", 7) == HAL_OK);                /* default partition not fenced */
        CHECK(hal_kv_commit(&fo) == HAL_OK);
        s_fence_on = false;
        CHECK(hal_kv_set_u32(&fk, "a", 3) == HAL_OK);
        CHECK(hal_kv_commit(&fk) == HAL_OK);
        hal_kv_set_write_refuse_hook(NULL);
        CHECK(hal_kv_close(&fk) == HAL_OK);
        CHECK(hal_kv_close(&fo) == HAL_OK);
        fake_kv_reset_all();
    }

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
