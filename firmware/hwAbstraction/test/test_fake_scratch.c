/* test_fake_scratch.c -- standalone MSVC host test for
 * hwAbstraction/host/fake_scratch.c. Compiled and run by
 * test_host_fakes.ps1.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "fake_scratch.h"

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) \
    do { \
        if (cond) { g_pass++; } \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

int main(void) {
    fake_scratch_reset_all();

    /* --- fresh state: all slots read 0, no claims --- */
    CHECK(fake_scratch_get_claim_count() == 0);
    for (uint8_t s = 0; s < HAL_SCRATCH_SLOT_COUNT; s++) {
        if (s == HAL_SCRATCH_SLOT_WATCHDOG_ENABLE) { continue; }
        uint32_t v = 0xFFFFFFFFu;
        bool magic_ok = true;
        CHECK(hal_scratch_read_u32(s, &v, s, 0, &magic_ok) == HAL_OK);
        CHECK(v == 0);
        CHECK(magic_ok == true); /* 0 == 0 */
    }

    /* --- out-of-range slot args --- */
    {
        uint32_t v;
        bool ok;
        CHECK(hal_scratch_write_u32(HAL_SCRATCH_SLOT_COUNT, 1) == HAL_INVALID_ARG);
        CHECK(hal_scratch_read_u32(HAL_SCRATCH_SLOT_COUNT, &v, 0, 0, &ok) == HAL_INVALID_ARG);
        CHECK(hal_scratch_read_u32(0, &v, HAL_SCRATCH_SLOT_COUNT, 0, &ok) == HAL_INVALID_ARG);
        CHECK(hal_scratch_read_u32(0, NULL, 0, 0, &ok) == HAL_INVALID_ARG);
        CHECK(hal_scratch_clear(HAL_SCRATCH_SLOT_COUNT) == HAL_INVALID_ARG);
        CHECK(hal_scratch_claim(HAL_SCRATCH_SLOT_COUNT, "x", 0) == HAL_INVALID_ARG);
        CHECK(hal_scratch_claim(0, NULL, 0) == HAL_INVALID_ARG);
    }

    /* --- slot 4 hard reservation: refused for claim/write/clear --- */
    CHECK(hal_scratch_claim(HAL_SCRATCH_SLOT_WATCHDOG_ENABLE, "watchdog_enable", 0) == HAL_INVALID_ARG);
    CHECK(hal_scratch_write_u32(HAL_SCRATCH_SLOT_WATCHDOG_ENABLE, 0x1234) == HAL_INVALID_ARG);
    CHECK(hal_scratch_clear(HAL_SCRATCH_SLOT_WATCHDOG_ENABLE) == HAL_INVALID_ARG);
    /* read is still allowed (real hardware reads are not refused, only
     * writes/claims -- watchdog_enable() itself writes it, a caller may
     * still legitimately inspect it) */
    {
        uint32_t v;
        bool ok;
        CHECK(hal_scratch_read_u32(HAL_SCRATCH_SLOT_WATCHDOG_ENABLE, &v, HAL_SCRATCH_SLOT_WATCHDOG_ENABLE, 0, &ok) == HAL_OK);
    }
    CHECK(fake_scratch_get_claim_count() == 0); /* the slot-4 claim attempt above did not register */

    /* --- normal claim, write, read-with-magic, clear --- */
    CHECK(hal_scratch_claim(0, "boot_reason", HAL_SCRATCH_TAG_NONE) == HAL_OK);
    CHECK(hal_scratch_claim(1, "boot_reason_magic", HAL_SCRATCH_TAG_NONE) == HAL_OK);
    CHECK(fake_scratch_get_claim_count() == 2);
    CHECK(fake_scratch_is_claimed(0, HAL_SCRATCH_TAG_NONE));
    CHECK(!fake_scratch_is_claimed(2, HAL_SCRATCH_TAG_NONE));

    {
        hal_scratch_claim_t c;
        CHECK(fake_scratch_get_claim(0, &c));
        CHECK(c.slot == 0);
        CHECK(strcmp(c.owner, "boot_reason") == 0);
        CHECK(!fake_scratch_get_claim(2, &c)); /* out of range */
        CHECK(!fake_scratch_get_claim(255, &c));
    }

    CHECK(hal_scratch_write_u32(0, 0xDEADBEEFu) == HAL_OK);
    CHECK(hal_scratch_write_u32(1, 0xB007FACEu) == HAL_OK); /* the "magic" word */
    {
        uint32_t v = 0;
        bool magic_ok = false;
        CHECK(hal_scratch_read_u32(0, &v, 1, 0xB007FACEu, &magic_ok) == HAL_OK);
        CHECK(v == 0xDEADBEEFu);
        CHECK(magic_ok == true);
        CHECK(hal_scratch_read_u32(0, &v, 1, 0x11111111u, &magic_ok) == HAL_OK);
        CHECK(magic_ok == false); /* wrong magic */
        CHECK(hal_scratch_read_u32(0, &v, 1, 0xB007FACEu, NULL) == HAL_OK); /* magic_ok optional */
    }

    CHECK(hal_scratch_clear(0) == HAL_OK);
    {
        uint32_t v = 0xFF;
        CHECK(hal_scratch_read_u32(0, &v, 0, 0, NULL) == HAL_OK);
        CHECK(v == 0);
    }

    /* --- (slot, tag) uniqueness: same slot+tag refused, same slot+different
     * tag (slot 5's two legitimate co-owners) accepted --- */
    CHECK(hal_scratch_claim(0, "someone_else", HAL_SCRATCH_TAG_NONE) == HAL_INVALID_ARG); /* collision */
    CHECK(hal_scratch_claim(5, "watchdog_overdue_diag", 0xD9) == HAL_OK);
    CHECK(hal_scratch_claim(5, "stack_overflow_hook", 0xE3) == HAL_OK); /* co-owner, different tag */
    CHECK(hal_scratch_claim(5, "watchdog_overdue_diag", 0xD9) == HAL_INVALID_ARG); /* re-claim same pair */
    CHECK(fake_scratch_get_claim_count() == 4);

    /* --- claim-table exhaustion --- */
    {
        int registered = 0;
        char names[32][16];
        int i;
        for (i = 0; i < 32; i++) {
            snprintf(names[i], sizeof(names[i]), "filler%02d", i);
            /* slot 6 accepts many distinct tags -- exercise pool exhaustion,
             * not the (slot,tag) collision path. */
            hal_status_t st = hal_scratch_claim(6, names[i], (uint8_t)(i + 1));
            if (st == HAL_OK) {
                registered++;
            } else {
                CHECK(st == HAL_NO_MEM);
                break;
            }
        }
        CHECK(registered > 0); /* at least some succeeded before the pool filled */
    }

    /* --- simulate_reset: claim table clears, slot VALUES survive --- */
    fake_scratch_reset_all();
    CHECK(hal_scratch_claim(0, "boot_reason", HAL_SCRATCH_TAG_NONE) == HAL_OK);
    CHECK(hal_scratch_write_u32(2, 0x77777777u) == HAL_OK);
    fake_scratch_simulate_reset();
    CHECK(fake_scratch_get_claim_count() == 0); /* claims wiped */
    {
        uint32_t v = 0;
        CHECK(hal_scratch_read_u32(2, &v, 2, 0, NULL) == HAL_OK);
        CHECK(v == 0x77777777u); /* scratch survives a watchdog reset -- the whole point */
    }
    /* re-claiming after a simulated reset must succeed again (no stale
     * "already claimed" state left over) */
    CHECK(hal_scratch_claim(0, "boot_reason", HAL_SCRATCH_TAG_NONE) == HAL_OK);

    /* --- simulate_power_loss: BOTH claims and slot values clear --- */
    CHECK(hal_scratch_write_u32(3, 0x99999999u) == HAL_OK);
    fake_scratch_simulate_power_loss();
    CHECK(fake_scratch_get_claim_count() == 0);
    {
        uint32_t v = 0xFF;
        CHECK(hal_scratch_read_u32(2, &v, 2, 0, NULL) == HAL_OK);
        CHECK(v == 0); /* power loss cleared what a watchdog reset would not */
        CHECK(hal_scratch_read_u32(3, &v, 3, 0, NULL) == HAL_OK);
        CHECK(v == 0);
    }

    fake_scratch_reset_all();

    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
