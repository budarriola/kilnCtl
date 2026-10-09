/* test_fake_sysinfo.c -- standalone MSVC host test for
 * hwAbstraction/host/fake_sysinfo.c. Compiled and run by test_host_fakes.ps1.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "fake_sysinfo.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

int main(void) {
    fake_sysinfo_reset_all();

    /* --- defaults --- */
    CHECK(hal_sysinfo_reset_reason() == HAL_RESET_POWERON);
    {
        hal_sysinfo_partition_info_t p;
        memset(&p, 0xAA, sizeof(p));
        CHECK(hal_sysinfo_get_running_partition(&p) == HAL_IO); /* never set */
    }
    {
        hal_sysinfo_build_info_t b;
        memset(&b, 0xAA, sizeof(b));
        hal_sysinfo_get_build_info(&b);
        CHECK(b.valid == false);
    }
    CHECK(!hal_sysinfo_coredump_present());
    CHECK(hal_sysinfo_random_u32() == 0xA5A5A5A5u); /* fixed fallback, unscripted */
    CHECK(hal_sysinfo_random_u32() == 0xA5A5A5A5u); /* stays fixed, not incrementing */

    /* --- reset reason scripting --- */
    fake_sysinfo_set_reset_reason(HAL_RESET_PANIC);
    CHECK(hal_sysinfo_reset_reason() == HAL_RESET_PANIC);
    fake_sysinfo_set_reset_reason(HAL_RESET_TASK_WDT);
    CHECK(hal_sysinfo_reset_reason() == HAL_RESET_TASK_WDT);

    /* --- get_running_partition: bad arg, then scripted value round-trips --- */
    CHECK(hal_sysinfo_get_running_partition(NULL) == HAL_INVALID_ARG);
    {
        hal_sysinfo_partition_info_t set;
        memset(&set, 0, sizeof(set));
        memcpy(set.label, "factory", sizeof("factory"));
        set.address = 0x10000;
        set.size = 0x300000;
        fake_sysinfo_set_running_partition(&set);

        hal_sysinfo_partition_info_t got;
        memset(&got, 0, sizeof(got));
        CHECK(hal_sysinfo_get_running_partition(&got) == HAL_OK);
        CHECK(strcmp(got.label, "factory") == 0);
        CHECK(got.address == 0x10000);
        CHECK(got.size == 0x300000);

        fake_sysinfo_set_running_partition(NULL); /* reverts to unset */
        CHECK(hal_sysinfo_get_running_partition(&got) == HAL_IO);
    }

    /* --- build info: valid round-trip, then explicit invalidation --- */
    {
        hal_sysinfo_build_info_t set;
        memset(&set, 0, sizeof(set));
        memcpy(set.version, "1.2.3", sizeof("1.2.3"));
        memcpy(set.date, "Sep  5 2026", sizeof("Sep  5 2026"));
        memcpy(set.time, "12:00:00", sizeof("12:00:00"));
        fake_sysinfo_set_build_info(&set);

        hal_sysinfo_build_info_t got;
        memset(&got, 0xAA, sizeof(got));
        hal_sysinfo_get_build_info(&got);
        CHECK(got.valid == true);
        CHECK(strcmp(got.version, "1.2.3") == 0);

        fake_sysinfo_set_build_info_invalid();
        hal_sysinfo_get_build_info(&got);
        CHECK(got.valid == false);
    }
    CHECK(hal_sysinfo_get_running_partition(NULL) == HAL_INVALID_ARG); /* NULL always invalid, regardless of state */

    /* --- temperature lifecycle: NOT_READY before init --- */
    {
        float c = -999.0f;
        CHECK(hal_sysinfo_temp_read_celsius(&c) == HAL_NOT_READY);
        CHECK(hal_sysinfo_temp_deinit() == HAL_NOT_READY);
    }

    /* --- temperature: scripted init failure --- */
    fake_sysinfo_script_temp_init_status(HAL_IO);
    CHECK(hal_sysinfo_temp_init() == HAL_IO);
    {
        float c;
        CHECK(hal_sysinfo_temp_read_celsius(&c) == HAL_NOT_READY); /* the failed init did not arm it */
    }

    /* --- temperature: normal init/read/deinit --- */
    CHECK(hal_sysinfo_temp_init() == HAL_OK); /* scripted failure was one-shot */
    CHECK(hal_sysinfo_temp_init() == HAL_OK); /* already-up is a no-op OK, matching board_temps.c */
    fake_sysinfo_set_temp_celsius(42.5f);
    {
        float c = 0;
        CHECK(hal_sysinfo_temp_read_celsius(NULL) == HAL_INVALID_ARG);
        CHECK(hal_sysinfo_temp_read_celsius(&c) == HAL_OK);
        CHECK(c == 42.5f);
    }

    /* --- temperature: scripted read failure, one-shot --- */
    fake_sysinfo_script_temp_read_status(HAL_WEDGED);
    {
        float c = 0;
        CHECK(hal_sysinfo_temp_read_celsius(&c) == HAL_WEDGED);
        CHECK(hal_sysinfo_temp_read_celsius(&c) == HAL_OK); /* one-shot: reverted */
        CHECK(c == 42.5f);
    }

    CHECK(hal_sysinfo_temp_deinit() == HAL_OK);
    CHECK(hal_sysinfo_temp_deinit() == HAL_NOT_READY); /* already down */

    /* --- random sequence: scripted values in order, then fallback --- */
    {
        uint32_t seq[3] = { 1u, 2u, 3u };
        fake_sysinfo_script_random_sequence(seq, 3);
        CHECK(hal_sysinfo_random_u32() == 1u);
        CHECK(hal_sysinfo_random_u32() == 2u);
        CHECK(hal_sysinfo_random_u32() == 3u);
        CHECK(hal_sysinfo_random_u32() == 0xA5A5A5A5u); /* exhausted -> fallback */
    }
    {
        /* count == 0 / NULL clears any armed sequence */
        uint32_t seq[1] = { 99u };
        fake_sysinfo_script_random_sequence(seq, 1);
        fake_sysinfo_script_random_sequence(NULL, 0);
        CHECK(hal_sysinfo_random_u32() == 0xA5A5A5A5u);
        fake_sysinfo_script_random_sequence(seq, 0);
        CHECK(hal_sysinfo_random_u32() == 0xA5A5A5A5u);
    }

    /* --- fill_random: draws from the same scripted sequence, whole words --- */
    {
        uint32_t seq[2] = { 0x11223344u, 0x55667788u };
        fake_sysinfo_script_random_sequence(seq, 2);
        uint8_t buf[8];
        memset(buf, 0xCC, sizeof(buf));
        hal_sysinfo_fill_random(buf, sizeof(buf));
        uint32_t got0, got1;
        memcpy(&got0, buf, sizeof(got0));
        memcpy(&got1, buf + 4, sizeof(got1));
        CHECK(got0 == 0x11223344u);
        CHECK(got1 == 0x55667788u);
    }
    /* --- fill_random: a trailing partial word takes only the low bytes of
     * the next scripted/fallback u32 (little-endian host, matching this
     * fake's memcpy-from-uint32_t implementation) --- */
    {
        fake_sysinfo_script_random_sequence(NULL, 0); /* fall back to 0xA5A5A5A5 */
        uint8_t buf[3];
        memset(buf, 0, sizeof(buf));
        hal_sysinfo_fill_random(buf, sizeof(buf));
        CHECK(buf[0] == 0xA5u);
        CHECK(buf[1] == 0xA5u);
        CHECK(buf[2] == 0xA5u);
    }
    /* --- fill_random: len == 0 is a safe no-op, even with buf == NULL --- */
    hal_sysinfo_fill_random(NULL, 0);

    /* --- coredump: presence set, erase clears it --- */
    fake_sysinfo_set_coredump_present(true);
    CHECK(hal_sysinfo_coredump_present());
    CHECK(hal_sysinfo_coredump_erase() == HAL_OK);
    CHECK(!hal_sysinfo_coredump_present());
    /* erasing when nothing is present is still HAL_OK (matches real
     * esp_core_dump_image_erase() being idempotent-safe to call) */
    CHECK(hal_sysinfo_coredump_erase() == HAL_OK);

    /* --- reset_all clears everything back to defaults --- */
    fake_sysinfo_set_coredump_present(true);
    fake_sysinfo_reset_all();
    CHECK(!hal_sysinfo_coredump_present());
    CHECK(hal_sysinfo_reset_reason() == HAL_RESET_POWERON);

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
