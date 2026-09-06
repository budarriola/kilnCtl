/* test_fake_flash.c -- standalone MSVC host test for hwAbstraction/host/fake_flash.c.
 * Compiled and run by test_host_fakes.ps1.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "fake_flash.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static int s_safe_execute_calls = 0;
static void count_cb(void *arg) {
    (void)arg;
    s_safe_execute_calls++;
}

int main(void) {
    fake_flash_reset_all();

    /* --- geometry --- */
    hal_flash_geometry_t geo;
    memset(&geo, 0, sizeof(geo));
    CHECK(hal_flash_geometry(NULL, &geo) == HAL_OK);
    CHECK(geo.flash_total_size == FAKE_FLASH_DEFAULT_SIZE_BYTES);
    CHECK(geo.erase_size == HAL_FLASH_ERASE_SIZE);
    CHECK(geo.program_size == HAL_FLASH_PROGRAM_SIZE);
    CHECK(hal_flash_geometry(NULL, NULL) == HAL_INVALID_ARG);
    CHECK(fake_flash_get_total_size() == FAKE_FLASH_DEFAULT_SIZE_BYTES);

    /* --- erased state is 0xFF everywhere --- */
    uint8_t buf[HAL_FLASH_PROGRAM_SIZE];
    memset(buf, 0, sizeof(buf));
    CHECK(hal_flash_read(NULL, 0, buf, sizeof(buf)) == HAL_OK);
    {
        int all_ff = 1;
        for (size_t i = 0; i < sizeof(buf); i++) {
            if (buf[i] != 0xFF) { all_ff = 0; break; }
        }
        CHECK(all_ff);
    }

    /* --- read: bad args / bounds --- */
    CHECK(hal_flash_read(NULL, 0, NULL, 4) == HAL_INVALID_ARG);
    CHECK(hal_flash_read(NULL, 0, buf, 0) == HAL_OK); /* zero-length is a no-op OK */
    CHECK(hal_flash_read(NULL, (uint32_t)FAKE_FLASH_DEFAULT_SIZE_BYTES, buf, 1) == HAL_INVALID_ARG);
    CHECK(hal_flash_read(NULL, (uint32_t)FAKE_FLASH_DEFAULT_SIZE_BYTES - 1, buf, 2) == HAL_INVALID_ARG);

    /* --- erase: alignment / bounds / zero-length --- */
    CHECK(hal_flash_erase(NULL, 0, 0) == HAL_INVALID_SIZE);
    CHECK(hal_flash_erase(NULL, 1, HAL_FLASH_ERASE_SIZE) == HAL_INVALID_SIZE); /* offset misaligned */
    CHECK(hal_flash_erase(NULL, 0, HAL_FLASH_ERASE_SIZE - 1) == HAL_INVALID_SIZE); /* len misaligned */
    CHECK(hal_flash_erase(NULL, (uint32_t)FAKE_FLASH_DEFAULT_SIZE_BYTES, HAL_FLASH_ERASE_SIZE)
          == HAL_INVALID_ARG); /* out of bounds */

    /* --- program: alignment / bounds / bad args --- */
    CHECK(hal_flash_program(NULL, 0, NULL, HAL_FLASH_PROGRAM_SIZE) == HAL_INVALID_ARG);
    CHECK(hal_flash_program(NULL, 0, buf, 0) == HAL_INVALID_SIZE);
    CHECK(hal_flash_program(NULL, 1, buf, HAL_FLASH_PROGRAM_SIZE) == HAL_INVALID_SIZE);
    CHECK(hal_flash_program(NULL, 0, buf, HAL_FLASH_PROGRAM_SIZE - 1) == HAL_INVALID_SIZE);
    CHECK(hal_flash_program(NULL, (uint32_t)FAKE_FLASH_DEFAULT_SIZE_BYTES, buf, HAL_FLASH_PROGRAM_SIZE)
          == HAL_INVALID_ARG);

    /* --- normal erase + program + read round trip --- */
    CHECK(hal_flash_erase(NULL, 0, HAL_FLASH_ERASE_SIZE) == HAL_OK);
    CHECK(fake_flash_get_erase_count(0) == 1);
    CHECK(fake_flash_get_erase_count(1) == 0);

    uint8_t pattern[HAL_FLASH_PROGRAM_SIZE];
    for (size_t i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(0xA5 ^ i);
    CHECK(hal_flash_program(NULL, 0, pattern, sizeof(pattern)) == HAL_OK);

    uint8_t readback[HAL_FLASH_PROGRAM_SIZE];
    CHECK(hal_flash_read(NULL, 0, readback, sizeof(readback)) == HAL_OK);
    CHECK(memcmp(readback, pattern, sizeof(pattern)) == 0);

    /* erasing again bumps the count */
    CHECK(hal_flash_erase(NULL, 0, HAL_FLASH_ERASE_SIZE) == HAL_OK);
    CHECK(fake_flash_get_erase_count(0) == 2);

    /* --- erase spanning two sectors bumps both --- */
    CHECK(hal_flash_erase(NULL, 0, 2u * HAL_FLASH_ERASE_SIZE) == HAL_OK);
    CHECK(fake_flash_get_erase_count(0) == 3);
    CHECK(fake_flash_get_erase_count(1) == 1);

    /* --- out-of-range sector index reports 0, does not crash --- */
    CHECK(fake_flash_get_erase_count((uint32_t)FAKE_FLASH_MAX_SECTORS + 100u) == 0);

    /* --- AND semantics: programming a non-erased byte clears bits instead
     * of overwriting -- detectable by a caller who skipped the erase. --- */
    CHECK(hal_flash_erase(NULL, 0, HAL_FLASH_ERASE_SIZE) == HAL_OK);
    uint8_t all_0f[HAL_FLASH_PROGRAM_SIZE];
    memset(all_0f, 0x0F, sizeof(all_0f));
    CHECK(hal_flash_program(NULL, 0, all_0f, sizeof(all_0f)) == HAL_OK); /* 0xFF & 0x0F -> 0x0F */
    uint8_t rb1[HAL_FLASH_PROGRAM_SIZE];
    CHECK(hal_flash_read(NULL, 0, rb1, sizeof(rb1)) == HAL_OK);
    {
        int ok = 1;
        for (size_t i = 0; i < sizeof(rb1); i++) if (rb1[i] != 0x0F) { ok = 0; break; }
        CHECK(ok);
    }
    uint8_t all_f0[HAL_FLASH_PROGRAM_SIZE];
    memset(all_f0, 0xF0, sizeof(all_f0));
    /* Programming 0xF0 over already-0x0F flash WITHOUT erasing: real NOR
     * flash cannot set the bits 0xF0 needs that 0x0F already cleared -- AND
     * semantics leaves the result at 0x00, NOT 0xF0, proving the missing
     * erase is detectable. */
    CHECK(hal_flash_program(NULL, 0, all_f0, sizeof(all_f0)) == HAL_OK);
    uint8_t rb2[HAL_FLASH_PROGRAM_SIZE];
    CHECK(hal_flash_read(NULL, 0, rb2, sizeof(rb2)) == HAL_OK);
    {
        int ok = 1;
        for (size_t i = 0; i < sizeof(rb2); i++) if (rb2[i] != 0x00) { ok = 0; break; }
        CHECK(ok);
        /* Not equal to the naive "just overwrote" expectation. */
        CHECK(memcmp(rb2, all_f0, sizeof(all_f0)) != 0);
    }

    /* --- per-op failure injection: read --- */
    fake_flash_reset_all();
    fake_flash_script_next_op_status(FAKE_FLASH_OP_READ, HAL_TIMEOUT);
    CHECK(hal_flash_read(NULL, 0, buf, 4) == HAL_TIMEOUT);
    CHECK(hal_flash_read(NULL, 0, buf, 4) == HAL_OK); /* one-shot, reverted */

    /* --- per-op failure injection: erase --- */
    fake_flash_script_next_op_status(FAKE_FLASH_OP_ERASE, HAL_NOT_READY);
    CHECK(hal_flash_erase(NULL, 0, HAL_FLASH_ERASE_SIZE) == HAL_NOT_READY);
    CHECK(fake_flash_get_erase_count(0) == 0); /* injected failure performs nothing */
    CHECK(hal_flash_erase(NULL, 0, HAL_FLASH_ERASE_SIZE) == HAL_OK);
    CHECK(fake_flash_get_erase_count(0) == 1);

    /* --- per-op failure injection: program --- */
    fake_flash_script_next_op_status(FAKE_FLASH_OP_PROGRAM, HAL_IO);
    CHECK(hal_flash_program(NULL, 0, pattern, sizeof(pattern)) == HAL_IO);
    CHECK(hal_flash_read(NULL, 0, readback, sizeof(readback)) == HAL_OK);
    {
        int all_ff = 1;
        for (size_t i = 0; i < sizeof(readback); i++) if (readback[i] != 0xFF) { all_ff = 0; break; }
        CHECK(all_ff); /* injected failure performs nothing */
    }
    CHECK(hal_flash_program(NULL, 0, pattern, sizeof(pattern)) == HAL_OK);

    /* --- per-op failure injection: safe_execute --- */
    s_safe_execute_calls = 0;
    fake_flash_script_next_op_status(FAKE_FLASH_OP_SAFE_EXECUTE, HAL_TIMEOUT);
    CHECK(hal_flash_safe_execute(count_cb, NULL, 1000u) == HAL_TIMEOUT);
    CHECK(s_safe_execute_calls == 0); /* callback never runs on injected failure */
    CHECK(hal_flash_safe_execute(count_cb, NULL, 1000u) == HAL_OK);
    CHECK(s_safe_execute_calls == 1);
    CHECK(hal_flash_safe_execute(NULL, NULL, 1000u) == HAL_INVALID_ARG);

    /* --- power loss during program: half-written page --- */
    fake_flash_reset_all();
    CHECK(hal_flash_erase(NULL, 0, HAL_FLASH_ERASE_SIZE) == HAL_OK);
    {
        /* Program two pages' worth in one call so a "half-written" result is
         * observable: page 0 lands, page 1 does not. */
        uint8_t two_pages[2u * HAL_FLASH_PROGRAM_SIZE];
        memset(two_pages, 0x33, sizeof(two_pages));
        fake_flash_simulate_power_loss_during(FAKE_FLASH_OP_PROGRAM);
        CHECK(hal_flash_program(NULL, 0, two_pages, sizeof(two_pages)) == HAL_IO);

        uint8_t page0[HAL_FLASH_PROGRAM_SIZE];
        uint8_t page1[HAL_FLASH_PROGRAM_SIZE];
        CHECK(hal_flash_read(NULL, 0, page0, sizeof(page0)) == HAL_OK);
        CHECK(hal_flash_read(NULL, HAL_FLASH_PROGRAM_SIZE, page1, sizeof(page1)) == HAL_OK);

        int page0_written = 1, page1_erased = 1;
        for (size_t i = 0; i < sizeof(page0); i++) if (page0[i] != 0x33) { page0_written = 0; break; }
        for (size_t i = 0; i < sizeof(page1); i++) if (page1[i] != 0xFF) { page1_erased = 0; break; }
        CHECK(page0_written);
        CHECK(page1_erased);
    }
    /* one-shot: next program call is unaffected */
    {
        uint8_t two_pages[2u * HAL_FLASH_PROGRAM_SIZE];
        memset(two_pages, 0x44, sizeof(two_pages));
        CHECK(hal_flash_erase(NULL, 0, HAL_FLASH_ERASE_SIZE) == HAL_OK);
        CHECK(hal_flash_program(NULL, 0, two_pages, sizeof(two_pages)) == HAL_OK);
        uint8_t page1[HAL_FLASH_PROGRAM_SIZE];
        CHECK(hal_flash_read(NULL, HAL_FLASH_PROGRAM_SIZE, page1, sizeof(page1)) == HAL_OK);
        CHECK(page1[0] == 0x44);
    }

    /* --- power loss during erase: only the first sector actually clears --- */
    fake_flash_reset_all();
    {
        uint8_t some[HAL_FLASH_PROGRAM_SIZE];
        memset(some, 0x11, sizeof(some));
        CHECK(hal_flash_program(NULL, HAL_FLASH_ERASE_SIZE, some, sizeof(some)) == HAL_OK);
        fake_flash_simulate_power_loss_during(FAKE_FLASH_OP_ERASE);
        CHECK(hal_flash_erase(NULL, 0, 2u * HAL_FLASH_ERASE_SIZE) == HAL_IO);
        CHECK(fake_flash_get_erase_count(0) == 1);
        CHECK(fake_flash_get_erase_count(1) == 0); /* second sector never reached */
        uint8_t back[HAL_FLASH_PROGRAM_SIZE];
        CHECK(hal_flash_read(NULL, HAL_FLASH_ERASE_SIZE, back, sizeof(back)) == HAL_OK);
        CHECK(back[0] == 0x11); /* sector 1's earlier program survived the aborted erase */
    }

    /* --- power loss during safe_execute: callback never runs --- */
    fake_flash_reset_all();
    s_safe_execute_calls = 0;
    fake_flash_simulate_power_loss_during(FAKE_FLASH_OP_SAFE_EXECUTE);
    CHECK(hal_flash_safe_execute(count_cb, NULL, 1000u) == HAL_IO);
    CHECK(s_safe_execute_calls == 0);

    /* --- reset_all_sized: bad sizes rejected, good size applied --- */
    CHECK(fake_flash_reset_all_sized(0) == false);
    CHECK(fake_flash_reset_all_sized(1) == false); /* not erase-size aligned */
    CHECK(fake_flash_reset_all_sized((size_t)FAKE_FLASH_MAX_SIZE_BYTES + HAL_FLASH_ERASE_SIZE) == false);
    CHECK(fake_flash_reset_all_sized(2u * HAL_FLASH_ERASE_SIZE) == true);
    CHECK(fake_flash_get_total_size() == 2u * HAL_FLASH_ERASE_SIZE);
    hal_flash_geometry_t geo2;
    CHECK(hal_flash_geometry(NULL, &geo2) == HAL_OK);
    CHECK(geo2.flash_total_size == 2u * HAL_FLASH_ERASE_SIZE);
    CHECK(hal_flash_read(NULL, 2u * (uint32_t)HAL_FLASH_ERASE_SIZE, buf, 1) == HAL_INVALID_ARG);

    /* --- write_safe_here test hook --- */
    fake_flash_reset_all();
    CHECK(hal_flash_write_safe_here() == true);
    fake_flash_set_write_safe_here(false);
    CHECK(hal_flash_write_safe_here() == false);
    fake_flash_reset_all();
    CHECK(hal_flash_write_safe_here() == true); /* reset restores default */

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
