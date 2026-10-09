// test_bootloader_recovery_update.c -- host tests for bootloader/
// recovery_update.c, the recovery-mode receiver that runs with no RTOS and
// decides whether a slot becomes bootable.
//
// The real recovery_update.c is #included (not linked) so the tests can call
// its static entry points (recovery_dispatch, recovery_periodic_status)
// directly instead of looping on a UART. Flash is a RAM array behind the
// XIP_BASE stub; erase/program have real flash semantics (erase -> 0xFF,
// program only clears bits). The real persist.c / metadata.c / crc32.c and
// the real pure decision modules (image_header, received_ranges,
// update_receiver, update_task_slot_linkage) are linked, so metadata is read
// back the way main.c's boot path reads it.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

// recovery_update.h declares recovery_update_run() with a GCC attribute MSVC
// does not know; the attribute is irrelevant on host.
#define __attribute__(x)
#include "../bootloader/recovery_update.c"
#undef __attribute__

// --- stub backends ---------------------------------------------------------

#define FAKE_FLASH_SIZE 0x00200000u
uint8_t g_fake_flash[FAKE_FLASH_SIZE];

uint64_t time_us_64(void) { return 0u; }

void flash_range_erase(uint32_t flash_offs, size_t count)
{
    memset(&g_fake_flash[flash_offs], 0xFF, count);
}

void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        g_fake_flash[flash_offs + i] &= data[i];
    }
}

static uint8_t g_tx[16384];
static size_t g_tx_len;

void uart_putc_raw(uart_inst_t *uart, char c)
{
    (void)uart;
    if (g_tx_len < sizeof(g_tx)) {
        g_tx[g_tx_len++] = (uint8_t)c;
    }
}
bool uart_is_readable(uart_inst_t *uart) { (void)uart; return false; }
char uart_getc(uart_inst_t *uart) { (void)uart; return 0; }

// --- TX decoding: last UPDATE_STATUS frame the receiver sent ---------------

typedef struct {
    bool seen;
    uint8_t state;
    uint8_t err;
    uint8_t gap_count;
    bool has_trailer;
    uint8_t active_slot;
    uint8_t target_slot;
} tx_status_t;

static tx_status_t tx_last_status(void)
{
    tx_status_t out;
    memset(&out, 0, sizeof(out));
    size_t i = 0;
    while (i < g_tx_len) {
        if (g_tx[i] != KILNLINK_FRAME_DELIM) {
            i++;
            continue;
        }
        size_t j = i + 1u;
        while (j < g_tx_len && g_tx[j] != KILNLINK_FRAME_DELIM) {
            j++;
        }
        if (j >= g_tx_len) {
            break;
        }
        if (j > i + 1u) {
            uint8_t raw[KILNLINK_FRAME_STUFFED_MAX];
            kilnlink_frame_status_t us;
            size_t rl = kilnlink_unstuff(&g_tx[i + 1u], j - i - 1u, raw, sizeof(raw), &us);
            kilnlink_frame_t f;
            if (rl > 0 && kilnlink_frame_decode(raw, rl, &f) == KILNLINK_FRAME_OK &&
                f.length >= STATUS_HEADER_LEN && f.payload[0] == RECOVERY_CMD_UPDATE_STATUS) {
                memset(&out, 0, sizeof(out));
                out.seen = true;
                out.state = f.payload[1];
                out.err = f.payload[2];
                out.gap_count = f.payload[15];
                size_t t = (size_t)STATUS_HEADER_LEN + (size_t)out.gap_count * 2u;
                if (f.length >= t + STATUS_SLOT_TRAILER_LEN) {
                    out.has_trailer = true;
                    out.active_slot = f.payload[t];
                    out.target_slot = f.payload[t + 1u];
                }
            }
        }
        i = j; // closing delimiter doubles as the next opening one
    }
    return out;
}

// --- harness ---------------------------------------------------------------

#define IMG_LEN 1000u
#define IMG_MAX 2048u
static uint8_t g_img[IMG_MAX];

static void reset_board(bool with_metadata)
{
    memset(g_fake_flash, 0xFF, sizeof(g_fake_flash));
    g_tx_len = 0;
    s_transfer_active = false;
    s_retransmit_round_count = 0;
    s_gap_cursor = 0;
    s_pass_had_gap = false;
    if (with_metadata) {
        bootloader_metadata_t m;
        memset(&m, 0, sizeof(m));
        m.format_version = BOOTLOADER_METADATA_FORMAT_VERSION;
        m.active_slot = BOOTLOADER_SLOT_A;
        m.slots[BOOTLOADER_SLOT_A].state = BOOTLOADER_SLOT_VALID;
        m.slots[BOOTLOADER_SLOT_B].state = BOOTLOADER_SLOT_EMPTY;
        bootloader_persist_metadata(&m, BOOTLOADER_METADATA_NO_SLOT);
    }
}

static size_t read_meta(bootloader_metadata_t *m)
{
    return bootloader_metadata_find_latest(
        (const uint8_t *)(XIP_BASE + BOOTLOADER_METADATA_FLASH_OFFSET), m);
}

// Fills g_img with `len` bytes of pattern; vector table linked for the slot at
// flash offset `linked_for_offset`.
static void make_image(uint32_t len, uint32_t linked_for_offset)
{
    memset(g_img, 0, sizeof(g_img));
    for (uint32_t i = 0; i < len; i++) {
        g_img[i] = (uint8_t)(i * 7u + 3u);
    }
    if (len >= 8u) {
        put_u32_le(&g_img[0], 0x20041000u); // initial SP, inside SRAM
        put_u32_le(&g_img[4], RECOVERY_LINKAGE_XIP_BASE + linked_for_offset + 0x101u); // Thumb reset
    }
}

static void send(const uint8_t *payload, uint8_t length)
{
    kilnlink_frame_t f;
    memset(&f, 0, sizeof(f));
    f.msg_type = KILNLINK_MSG_BROADCAST;
    f.length = length;
    f.payload = payload;
    recovery_dispatch(&f);
}

static void send_begin(uint32_t len, uint32_t crc)
{
    update_image_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = UPDATE_IMAGE_HEADER_MAGIC;
    h.target = UPDATE_IMAGE_TARGET_RP2040;
    h.header_version = UPDATE_IMAGE_HEADER_VERSION;
    h.protocol_version = 5;
    h.min_compatible = 3;
    h.requested_slot = 1;
    h.length = len;
    h.crc32 = crc;
    memcpy(h.version, "9.9.9", 5);
    uint8_t p[1 + UPDATE_IMAGE_HEADER_WIRE_LEN];
    p[0] = RECOVERY_CMD_UPDATE_BEGIN;
    update_image_header_pack(&h, &p[1]);
    send(p, (uint8_t)sizeof(p));
}

static void send_data_chunk(uint32_t offset, uint32_t len)
{
    uint8_t p[1 + 4 + UPDATE_CHUNK_LEN];
    uint32_t n = len - offset;
    if (n > UPDATE_CHUNK_LEN) {
        n = UPDATE_CHUNK_LEN;
    }
    p[0] = RECOVERY_CMD_UPDATE_DATA;
    put_u32_le(&p[1], offset);
    memcpy(&p[5], &g_img[offset], n);
    send(p, (uint8_t)(5u + n));
}

static void send_all_data(uint32_t len)
{
    for (uint32_t off = 0; off < len; off += UPDATE_CHUNK_LEN) {
        send_data_chunk(off, len);
    }
}

static void send_end(uint32_t crc)
{
    uint8_t p[5];
    p[0] = RECOVERY_CMD_UPDATE_END;
    put_u32_le(&p[1], crc);
    send(p, 5);
}

static void send_abort(void)
{
    uint8_t p[1] = { RECOVERY_CMD_UPDATE_ABORT };
    send(p, 1);
}

// BEGIN + all DATA + END for the image currently in g_img; TX buffer holds
// only what END produced.
static void run_full_transfer(uint32_t len, uint32_t crc)
{
    send_begin(len, crc);
    send_all_data(len);
    g_tx_len = 0;
    send_end(crc);
}

// --- tests -----------------------------------------------------------------

static void test_happy_path(void)
{
    TEST_SECTION("bootloader recovery: good image for the target slot is accepted");
    reset_board(true);
    make_image(IMG_LEN, BOOTLOADER_SLOT_B_FLASH_OFFSET);
    uint32_t crc = bootloader_crc32(g_img, IMG_LEN);

    send_begin(IMG_LEN, crc);
    tx_status_t st = tx_last_status();
    TEST_CHECK(st.seen && st.state == R_STATE_RECEIVING, "BEGIN accepted -> RECEIVING");
    TEST_CHECK(st.has_trailer, "status carries the slot trailer");
    TEST_CHECK(st.active_slot == BOOTLOADER_SLOT_A, "trailer active_slot = A (metadata)");
    TEST_CHECK(st.target_slot == BOOTLOADER_SLOT_B, "trailer target_slot = B (the inactive slot)");

    send_all_data(IMG_LEN);
    g_tx_len = 0;
    send_end(crc);
    st = tx_last_status();
    TEST_CHECK(st.state == R_STATE_COMPLETE && st.err == 0, "END with good CRC+linkage -> COMPLETE");

    bootloader_metadata_t m;
    TEST_CHECK(read_meta(&m) != BOOTLOADER_METADATA_NO_SLOT, "metadata readable");
    TEST_CHECK(m.slots[BOOTLOADER_SLOT_B].state == BOOTLOADER_SLOT_PENDING_VERIFY,
               "slot B marked PENDING_VERIFY");
    TEST_CHECK(m.active_slot == BOOTLOADER_SLOT_B, "active_slot flipped to B");
    TEST_CHECK(m.boot_attempts == 0u, "boot_attempts reset");
    TEST_CHECK(!s_transfer_active, "transfer closed after COMPLETE");
}

static void test_idle_status_slots(void)
{
    TEST_SECTION("bootloader recovery: idle status reports the metadata active slot");
    reset_board(true);
    g_tx_len = 0;
    recovery_periodic_status();
    tx_status_t st = tx_last_status();
    TEST_CHECK(st.seen && st.state == R_STATE_IDLE, "idle -> IDLE");
    TEST_CHECK(st.has_trailer && st.active_slot == BOOTLOADER_SLOT_A, "active_slot = A");
    TEST_CHECK(st.target_slot == STATUS_SLOT_UNKNOWN, "no transfer -> target unknown (0xFF)");

    reset_board(false); // blank metadata sector: nothing to report
    g_tx_len = 0;
    recovery_periodic_status();
    st = tx_last_status();
    TEST_CHECK(st.has_trailer && st.active_slot == STATUS_SLOT_UNKNOWN,
               "no valid metadata -> active_slot unknown, never a guessed A");
}

static void test_crc_mismatch(void)
{
    TEST_SECTION("bootloader recovery: CRC mismatch at END reverts to EMPTY");
    reset_board(true);
    make_image(IMG_LEN, BOOTLOADER_SLOT_B_FLASH_OFFSET);
    uint32_t crc = bootloader_crc32(g_img, IMG_LEN);

    run_full_transfer(IMG_LEN, crc ^ 0xDEADBEEFu); // header claims a CRC the data cannot match
    tx_status_t st = tx_last_status();
    TEST_CHECK(st.state == R_STATE_FAILED, "bad CRC -> FAILED");
    TEST_CHECK((st.err & STATUS_ERR_CRC_MISMATCH) != 0, "err has CRC_MISMATCH");

    bootloader_metadata_t m;
    TEST_CHECK(read_meta(&m) != BOOTLOADER_METADATA_NO_SLOT, "metadata readable");
    TEST_CHECK(m.slots[BOOTLOADER_SLOT_B].state == BOOTLOADER_SLOT_EMPTY, "slot B reverted to EMPTY");
    TEST_CHECK(m.active_slot == BOOTLOADER_SLOT_A, "active slot untouched");
    TEST_CHECK(!s_transfer_active, "transfer closed");
}

static void test_abort(void)
{
    TEST_SECTION("bootloader recovery: ABORT reverts the staged slot");
    reset_board(true);
    make_image(IMG_LEN, BOOTLOADER_SLOT_B_FLASH_OFFSET);
    uint32_t crc = bootloader_crc32(g_img, IMG_LEN);
    send_begin(IMG_LEN, crc);
    send_data_chunk(0, IMG_LEN);

    bootloader_metadata_t m;
    TEST_CHECK(read_meta(&m) != BOOTLOADER_METADATA_NO_SLOT &&
                   m.slots[BOOTLOADER_SLOT_B].state == BOOTLOADER_SLOT_STAGED,
               "slot B STAGED mid-transfer");

    g_tx_len = 0;
    send_abort();
    tx_status_t st = tx_last_status();
    TEST_CHECK(st.state == R_STATE_ABORTED, "ABORT -> ABORTED");
    TEST_CHECK(read_meta(&m) != BOOTLOADER_METADATA_NO_SLOT &&
                   m.slots[BOOTLOADER_SLOT_B].state == BOOTLOADER_SLOT_EMPTY,
               "slot B reverted to EMPTY");
    TEST_CHECK(!s_transfer_active, "transfer closed");
}

static void test_retransmit_cap(void)
{
    TEST_SECTION("bootloader recovery: retransmit cap reverts to EMPTY");
    reset_board(true);
    make_image(IMG_LEN, BOOTLOADER_SLOT_B_FLASH_OFFSET);
    uint32_t crc = bootloader_crc32(g_img, IMG_LEN);
    send_begin(IMG_LEN, crc); // never send data: every pass finds every chunk missing

    bool failed = false;
    for (unsigned i = 0; i < UPDATE_MAX_RETRANSMIT_ROUNDS + 2u && !failed; i++) {
        g_tx_len = 0;
        recovery_periodic_status();
        tx_status_t st = tx_last_status();
        if (st.state == R_STATE_FAILED) {
            failed = true;
            TEST_CHECK((st.err & STATUS_ERR_RETRANSMIT_CAP) != 0, "err has RETRANSMIT_CAP");
        } else {
            TEST_CHECK(st.state == R_STATE_RECEIVING && st.gap_count > 0,
                       "below the cap: RECEIVING with gaps");
        }
    }
    TEST_CHECK(failed, "cap reached -> FAILED");
    bootloader_metadata_t m;
    TEST_CHECK(read_meta(&m) != BOOTLOADER_METADATA_NO_SLOT &&
                   m.slots[BOOTLOADER_SLOT_B].state == BOOTLOADER_SLOT_EMPTY,
               "slot B reverted to EMPTY");
    TEST_CHECK(!s_transfer_active, "transfer closed");
}

static void test_end_before_complete(void)
{
    TEST_SECTION("bootloader recovery: END before all chunks arrived is rejected");
    reset_board(true);
    make_image(IMG_LEN, BOOTLOADER_SLOT_B_FLASH_OFFSET);
    uint32_t crc = bootloader_crc32(g_img, IMG_LEN);
    send_begin(IMG_LEN, crc);
    send_data_chunk(0, IMG_LEN); // 1 of 5 chunks

    g_tx_len = 0;
    send_end(crc);
    tx_status_t st = tx_last_status();
    TEST_CHECK(st.state == R_STATE_RECEIVING, "incomplete END -> still RECEIVING, not VERIFYING/COMPLETE");

    bootloader_metadata_t m;
    TEST_CHECK(read_meta(&m) != BOOTLOADER_METADATA_NO_SLOT &&
                   m.slots[BOOTLOADER_SLOT_B].state == BOOTLOADER_SLOT_STAGED,
               "slot B still STAGED, not PENDING_VERIFY");
    TEST_CHECK(m.active_slot == BOOTLOADER_SLOT_A, "active slot untouched");
    TEST_CHECK(s_transfer_active, "transfer stays open so the sender can finish it");

    for (uint32_t off = UPDATE_CHUNK_LEN; off < IMG_LEN; off += UPDATE_CHUNK_LEN) {
        send_data_chunk(off, IMG_LEN);
    }
    g_tx_len = 0;
    send_end(crc);
    st = tx_last_status();
    TEST_CHECK(st.state == R_STATE_COMPLETE, "completes once every chunk is in");
}

// g_img is already built by the caller; runs it into target slot B with
// active = A and expects a linkage rejection.
static void expect_linkage_reject(const char *what, uint32_t len)
{
    char msg[200];
    uint32_t crc = bootloader_crc32(g_img, len);
    reset_board(true);
    run_full_transfer(len, crc);
    tx_status_t st = tx_last_status();

    snprintf(msg, sizeof(msg), "%s: CRC-good image rejected with state 8", what);
    TEST_CHECK(st.state == R_STATE_REJECTED_SLOT_LINKAGE, msg);
    snprintf(msg, sizeof(msg), "%s: err carries CRC_MISMATCH like update_task.c", what);
    TEST_CHECK((st.err & STATUS_ERR_CRC_MISMATCH) != 0, msg);

    bootloader_metadata_t m;
    snprintf(msg, sizeof(msg), "%s: slot B reverted to EMPTY, never PENDING_VERIFY", what);
    TEST_CHECK(read_meta(&m) != BOOTLOADER_METADATA_NO_SLOT &&
                   m.slots[BOOTLOADER_SLOT_B].state == BOOTLOADER_SLOT_EMPTY,
               msg);
    snprintf(msg, sizeof(msg), "%s: active slot still A", what);
    TEST_CHECK(m.active_slot == BOOTLOADER_SLOT_A, msg);
}

static void test_wrong_linkage(void)
{
    TEST_SECTION("bootloader recovery: slot-linkage check rejects CRC-good wrong-slot images");

    make_image(IMG_LEN, BOOTLOADER_SLOT_A_FLASH_OFFSET); // linked for A, written into B
    expect_linkage_reject("image linked for the other slot", IMG_LEN);

    make_image(IMG_LEN, BOOTLOADER_SLOT_B_FLASH_OFFSET);
    put_u32_le(&g_img[0], 0x30000000u); // SP outside SRAM
    expect_linkage_reject("SP outside SRAM", IMG_LEN);

    make_image(IMG_LEN, BOOTLOADER_SLOT_B_FLASH_OFFSET);
    put_u32_le(&g_img[4], RECOVERY_LINKAGE_XIP_BASE + BOOTLOADER_SLOT_B_FLASH_OFFSET + 0x100u);
    expect_linkage_reject("reset vector without the Thumb bit", IMG_LEN);

    make_image(4u, BOOTLOADER_SLOT_B_FLASH_OFFSET); // too short to hold a vector table
    expect_linkage_reject("4-byte image (no room for vectors)", 4u);

    // Control: same builder, right linkage, is accepted -- so the rejections
    // above are about linkage and not about the harness.
    make_image(IMG_LEN, BOOTLOADER_SLOT_B_FLASH_OFFSET);
    uint32_t crc = bootloader_crc32(g_img, IMG_LEN);
    reset_board(true);
    run_full_transfer(IMG_LEN, crc);
    TEST_CHECK(tx_last_status().state == R_STATE_COMPLETE, "control: correctly linked image is accepted");

    // Other direction: active = B, so the target is A and an image linked for
    // B must be rejected.
    {
        reset_board(true);
        bootloader_metadata_t m;
        size_t latest = read_meta(&m);
        m.active_slot = BOOTLOADER_SLOT_B;
        m.slots[BOOTLOADER_SLOT_B].state = BOOTLOADER_SLOT_VALID;
        bootloader_persist_metadata(&m, latest);
        make_image(IMG_LEN, BOOTLOADER_SLOT_B_FLASH_OFFSET);
        crc = bootloader_crc32(g_img, IMG_LEN);
        run_full_transfer(IMG_LEN, crc);
        tx_status_t st = tx_last_status();
        TEST_CHECK(st.state == R_STATE_REJECTED_SLOT_LINKAGE,
                   "active=B: image linked for B rejected for target A");
    }
}

// --- pre-jump watchdog: source-text guards -----------------------------------
// bootloader/main.c and src/main.c need the real pico-sdk, so (like
// test_boot_checkin_coverage.c) these scan comment-stripped source. They pin
// the three properties docs/audits/recovery_bootloader_audit_2026-10-02.md
// verified by reading: the 8 s watchdog is armed only inside jump_to_app(),
// before the branch; main() clears any leftover enable bit before the first
// enter_recovery(); recovery mode (this file's recovery_update.c, including the
// state-8 slot-linkage rejection) never touches the watchdog; and the
// application re-arms it once, before the scheduler, with a shorter timeout.

static void wdsrc_strip_comments(char *s)
{
    char *out = s;
    while (*s) {
        if (s[0] == '/' && s[1] == '/') {
            while (*s && *s != '\n') {
                s++;
            }
        } else if (s[0] == '/' && s[1] == '*') {
            s += 2;
            while (*s && !(s[0] == '*' && s[1] == '/')) {
                s++;
            }
            if (*s) {
                s += 2;
            }
        } else {
            *out++ = *s++;
        }
    }
    *out = '\0';
}

static char *wdsrc_load(const char *anchor_rel, const char *alt1, const char *alt2)
{
    const char *cands[3] = {anchor_rel, alt1, alt2};
    char *t = test_read_source_anchored(__FILE__, anchor_rel, cands, 3);
    if (t) {
        wdsrc_strip_comments(t);
    }
    return t;
}

static long wdsrc_define_value(const char *text, const char *name)
{
    char needle[96];
    snprintf(needle, sizeof(needle), "#define %s", name);
    const char *p = strstr(text, needle);
    if (!p) {
        return -1;
    }
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return strtol(p, NULL, 10);
}

static void test_prejump_watchdog_sources(void)
{
    TEST_SECTION("bootloader pre-jump watchdog: armed only before the jump, never in recovery");

    char *boot = wdsrc_load("../bootloader/main.c", "bootloader/main.c",
                            "firmware/SaftyFW/bootloader/main.c");
    char *app = wdsrc_load("../src/main.c", "src/main.c", "firmware/SaftyFW/src/main.c");
    char *rec = wdsrc_load("../bootloader/recovery_update.c", "bootloader/recovery_update.c",
                           "firmware/SaftyFW/bootloader/recovery_update.c");
    if (!boot || !app || !rec) {
        TEST_CHECK(false, "could not locate bootloader/main.c, src/main.c or "
                          "bootloader/recovery_update.c from the host test's working directory");
        free(boot);
        free(app);
        free(rec);
        return;
    }

    const char *wd = strstr(boot, "watchdog_enable(");
    const char *bx = strstr(boot, "\"bx");
    const char *main_fn = strstr(boot, "int main(void)");
    TEST_CHECK(wd && bx && main_fn, "bootloader/main.c: watchdog_enable, the bx branch and main() all found");
    if (wd && bx && main_fn) {
        TEST_CHECK(strstr(wd + 1, "watchdog_enable(") == NULL,
                   "bootloader/main.c: watchdog_enable() is called exactly once");
        TEST_CHECK(strstr(boot, "watchdog_enable(BOOTLOADER_APP_WATCHDOG_MS") == wd,
                   "bootloader/main.c: that one call uses BOOTLOADER_APP_WATCHDOG_MS");
        TEST_CHECK(wd < bx, "bootloader/main.c: the watchdog is armed BEFORE the bx jump");
        TEST_CHECK(bx < main_fn, "bootloader/main.c: the arm+jump lives in jump_to_app(), ahead of main()");

        const char *clr = strstr(main_fn, "hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS)");
        const char *first_rec = strstr(main_fn, "enter_recovery();");
        TEST_CHECK(clr && first_rec && clr < first_rec,
                   "bootloader/main.c: main() clears a leftover watchdog enable bit before the first enter_recovery()");
    }

    long boot_ms = wdsrc_define_value(boot, "BOOTLOADER_APP_WATCHDOG_MS");
    long app_ms = wdsrc_define_value(app, "SAFTYFW_WATCHDOG_TIMEOUT_MS");
    TEST_CHECK(boot_ms > 0 && boot_ms <= 8388, "BOOTLOADER_APP_WATCHDOG_MS fits the RP2040 hardware maximum (8388 ms)");
    TEST_CHECK(app_ms > 0 && boot_ms > app_ms,
               "the bootloader's pre-jump timeout is longer than the application's own, so re-arming only shortens it");

    const char *app_wd = strstr(app, "watchdog_enable(SAFTYFW_WATCHDOG_TIMEOUT_MS");
    const char *sched = strstr(app, "vTaskStartScheduler();");
    TEST_CHECK(app_wd && sched && app_wd < sched,
               "src/main.c: the application re-arms the watchdog before the scheduler starts");
    TEST_CHECK(app_wd && strstr(app_wd + 1, "watchdog_enable(") == NULL,
               "src/main.c: watchdog_enable() is called exactly once (shared by SaftyFW, slotA and slotB)");
    TEST_CHECK(strstr(app, "watchdog_disable") == NULL && strstr(app, "WATCHDOG_CTRL_ENABLE_BITS") == NULL,
               "src/main.c never disables the watchdog");

    TEST_CHECK(strstr(rec, "watchdog") == NULL,
               "bootloader/recovery_update.c never arms, feeds or disables a watchdog (state 8 included)");

    free(boot);
    free(app);
    free(rec);
}

void run_test_bootloader_recovery_update(void)
{
    test_prejump_watchdog_sources();
    test_happy_path();
    test_idle_status_slots();
    test_crc_mismatch();
    test_abort();
    test_retransmit_cap();
    test_end_before_complete();
    test_wrong_linkage();
}
