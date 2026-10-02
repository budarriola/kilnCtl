// test_recovery_pico_proto.c -- host test for recovery_pico_proto.c (the pure
// half of the recovery image's Pico update relay): kilnlink framing with byte
// stuffing and CRC16, the UPDATE_* packers, the streaming deframer, UPDATE_STATUS
// and Frame A parsing, image vector/slot/CRC validation, target-slot resolution,
// the END/gap-round finish state machine and BEGIN wait driven against a fake
// bootloader/application receiver in virtual time, and the pacing helpers.
// Built and run by check_recovery_pico_proto.ps1 (MSVC). Prints
// "RESULT pass=N fail=N"; exit code is the fail count.
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recovery_pico_proto.h"

static int g_pass, g_fail;

#define CHECK(cond, name)                                            \
    do {                                                             \
        if (cond) {                                                  \
            g_pass++;                                                \
        } else {                                                     \
            g_fail++;                                                \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__); \
        }                                                            \
    } while (0)

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Decodes one stuffed frame (as rpp_build_frame produced it) back to a frame.
static kilnlink_frame_status_t decode_wire(const uint8_t *wire, size_t n, uint8_t *raw,
                                           size_t raw_cap, kilnlink_frame_t *f)
{
    kilnlink_frame_status_t st;
    size_t rl = kilnlink_unstuff(wire, n, raw, raw_cap, &st);
    if (rl == 0) {
        return st;
    }
    return kilnlink_frame_decode(raw, rl, f);
}

// Builds a Pico -> ESP frame (the direction the deframer accepts).
static size_t build_pico_frame(uint16_t idx, uint8_t src_dev, uint8_t dst_dev, const uint8_t *pl,
                               size_t pl_len, uint8_t *wire)
{
    kilnlink_frame_t f;
    f.msg_type = KILNLINK_MSG_BROADCAST;
    f.msg_index = idx;
    f.src_device = src_dev;
    f.src_task = RPP_TASK_SAFETY;
    f.dst_device = dst_dev;
    f.dst_task = RPP_TASK_SAFETY;
    f.length = (uint8_t)pl_len;
    f.payload = pl;
    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t st;
    size_t rl = kilnlink_frame_encode_raw(&f, raw, sizeof(raw), &st);
    return rl ? kilnlink_stuff(raw, rl, wire, KILNLINK_FRAME_STUFFED_MAX) : 0;
}

static bool feed(rpp_rx_t *rx, const uint8_t *w, size_t n, kilnlink_frame_t *out)
{
    bool got = false;
    for (size_t i = 0; i < n; i++) {
        if (rpp_rx_push(rx, w[i], out)) {
            got = true;
        }
    }
    return got;
}

static void test_crc32(void)
{
    CHECK(rpp_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u, "crc32 check value");
    CHECK(rpp_crc32((const uint8_t *)"", 0) == 0u, "crc32 empty");
    uint8_t z[4] = {0, 0, 0, 0};
    CHECK(rpp_crc32(z, 4) == 0x2144DF1Cu, "crc32 four zero bytes");
}

static void test_frames(void)
{
    uint8_t wire[KILNLINK_FRAME_STUFFED_MAX], raw[KILNLINK_FRAME_STUFFED_MAX];
    uint8_t pl[KILNLINK_FRAME_MAX_PAYLOAD];
    kilnlink_frame_t f;

    // DATA whose offset and body contain both reserved bytes.
    uint8_t chunk[RPP_CHUNK_LEN];
    memset(chunk, 0x7E, sizeof(chunk));
    chunk[1] = 0x7D;
    chunk[2] = 0x5E;
    chunk[247] = 0x7D;
    size_t pn = rpp_pack_data(pl, 0x7E7D0000u, chunk, sizeof(chunk));
    CHECK(pn == 5u + RPP_CHUNK_LEN, "pack_data length");
    CHECK(pl[0] == RPP_CMD_UPDATE_DATA, "pack_data command");
    CHECK(get_u32(pl + 1) == 0x7E7D0000u, "pack_data offset LE");
    CHECK(memcmp(pl + 5, chunk, sizeof(chunk)) == 0, "pack_data body");

    size_t wn = rpp_build_frame(0x7E7D, pl, pn, wire, sizeof(wire));
    CHECK(wn > pn, "build_frame wrote bytes");
    CHECK(wire[0] == 0x7E && wire[wn - 1] == 0x7E, "frame delimited");
    int interior = 0;
    for (size_t i = 1; i + 1 < wn; i++) {
        if (wire[i] == 0x7E) {
            interior++;
        }
    }
    CHECK(interior == 0, "no raw delimiter inside a stuffed frame");
    CHECK(decode_wire(wire, wn, raw, sizeof(raw), &f) == KILNLINK_FRAME_OK, "round trip decodes");
    CHECK(f.msg_type == KILNLINK_MSG_BROADCAST, "round trip type");
    CHECK(f.msg_index == 0x7E7D, "round trip msg_index (stuffed bytes)");
    CHECK(f.src_device == RPP_DEVICE_ESP && f.dst_device == RPP_DEVICE_SAFETY, "ESP -> Pico addressing");
    CHECK(f.src_task == RPP_TASK_SAFETY && f.dst_task == RPP_TASK_SAFETY, "task ids");
    CHECK(f.length == pn && memcmp(f.payload, pl, pn) == 0, "round trip payload");

    // A flipped payload bit must fail the CRC.
    uint8_t bad[KILNLINK_FRAME_STUFFED_MAX];
    memcpy(bad, wire, wn);
    bad[10] ^= 0x01;
    CHECK(decode_wire(bad, wn, raw, sizeof(raw), &f) != KILNLINK_FRAME_OK, "corrupted frame rejected");

    // Maximum-size payload, every byte reserved: worst-case stuffing fits.
    memset(pl, 0x7E, sizeof(pl));
    wn = rpp_build_frame(1, pl, KILNLINK_FRAME_MAX_PAYLOAD, wire, sizeof(wire));
    CHECK(wn > 0 && wn <= KILNLINK_FRAME_STUFFED_MAX, "worst-case frame fits");
    CHECK(decode_wire(wire, wn, raw, sizeof(raw), &f) == KILNLINK_FRAME_OK && f.length == 253,
          "worst-case frame round trip");
    CHECK(rpp_build_frame(1, pl, 254, wire, sizeof(wire)) == 0, "payload over 253 refused");
    CHECK(rpp_build_frame(1, pl, 4, wire, 16) == 0, "undersized output refused");

    // Small packers.
    uint8_t p[KILNLINK_FRAME_MAX_PAYLOAD];
    CHECK(rpp_pack_get_status(p) == 1 && p[0] == 0x01, "get_status");
    CHECK(rpp_pack_abort(p) == 1 && p[0] == 0x13, "abort");
    CHECK(rpp_pack_reboot(p) == 1 && p[0] == 0x29, "reboot");
    CHECK(rpp_pack_end(p, 0xDEADBEEFu) == 5 && p[0] == 0x12 && get_u32(p + 1) == 0xDEADBEEFu, "end");
    CHECK(rpp_pack_data(p, 0, chunk, 0) == 0, "empty data refused");
    CHECK(rpp_pack_data(p, 0, chunk, RPP_CHUNK_LEN + 1) == 0, "oversize data refused");

    // BEGIN header layout.
    size_t bn = rpp_pack_begin(p, 0xD0000u, 0x11223344u, "recovery");
    CHECK(bn == 37, "begin length 1 + 36");
    CHECK(p[0] == 0x10, "begin command");
    CHECK(get_u32(p + 1) == 0x53414655u, "begin magic SAFU");
    CHECK(p[5] == 1 && p[6] == 1, "begin target RP2040, header v1");
    CHECK(p[7] == 16 && p[8] == 0 && p[9] == 7 && p[10] == 0, "begin protocol 16 / min 7");
    CHECK(p[11] == 0 && p[12] == 0, "begin requested_slot and flags 0");
    CHECK(get_u32(p + 13) == 0xD0000u && get_u32(p + 17) == 0x11223344u, "begin length and crc");
    CHECK(memcmp(p + 21, "recovery        ", 16) == 0, "begin version space padded");
}

static void test_deframer(void)
{
    uint8_t wire[KILNLINK_FRAME_STUFFED_MAX], wire2[KILNLINK_FRAME_STUFFED_MAX];
    uint8_t pl[16] = {RPP_CMD_UPDATE_STATUS, 3, 0, 0x7E, 0x7D, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    rpp_rx_t rx;
    kilnlink_frame_t f;
    rpp_rx_init(&rx);

    size_t wn = build_pico_frame(5, RPP_DEVICE_SAFETY, RPP_DEVICE_ESP, pl, sizeof(pl), wire);
    CHECK(wn > 0, "pico frame built");
    bool early = false;
    for (size_t i = 0; i + 1 < wn; i++) {
        if (rpp_rx_push(&rx, wire[i], &f)) {
            early = true;
        }
    }
    CHECK(!early, "no frame before the closing delimiter");
    CHECK(rpp_rx_push(&rx, wire[wn - 1], &f), "frame completes on the closing delimiter");
    CHECK(f.length == sizeof(pl) && memcmp(f.payload, pl, sizeof(pl)) == 0, "deframed payload intact");
    CHECK(f.msg_index == 5, "deframed msg_index");

    // Noise, then two frames back to back, one of them corrupted in between.
    rpp_rx_init(&rx);
    uint8_t noise[] = {0x01, 0x02, 0x7D, 0x03};
    int got = 0;
    for (size_t i = 0; i < sizeof(noise); i++) {
        got += rpp_rx_push(&rx, noise[i], &f) ? 1 : 0;
    }
    size_t w2 = build_pico_frame(6, RPP_DEVICE_SAFETY, RPP_DEVICE_ESP, pl, 4, wire2);
    got += feed(&rx, wire2, w2, &f) ? 1 : 0; // delimiter closes the noise run as a bad frame
    CHECK(got == 1, "frame after leading noise still delivered");
    CHECK(rx.frames_bad >= 1, "leading noise counted as a bad frame");
    uint32_t ok_before = rx.frames_ok;
    uint8_t corrupt[KILNLINK_FRAME_STUFFED_MAX];
    memcpy(corrupt, wire2, w2);
    corrupt[6] ^= 0x10;
    CHECK(!feed(&rx, corrupt, w2, &f), "corrupt CRC frame not delivered");
    CHECK(feed(&rx, wire2, w2, &f), "next good frame delivered after a corrupt one");
    CHECK(rx.frames_ok == ok_before + 1, "frames_ok counts only good frames");

    // Direction filter: an ESP-sourced frame (our own echo, or wrong device) is ignored.
    rpp_rx_init(&rx);
    size_t w3 = build_pico_frame(7, RPP_DEVICE_ESP, RPP_DEVICE_SAFETY, pl, 4, wire2);
    CHECK(!feed(&rx, wire2, w3, &f), "ESP-sourced frame ignored");
    w3 = build_pico_frame(8, RPP_DEVICE_SAFETY, 1, pl, 4, wire2);
    CHECK(!feed(&rx, wire2, w3, &f), "frame for another device ignored");

    // Overflow: a runaway body never delivers and the next frame still works.
    rpp_rx_init(&rx);
    for (int i = 0; i < 2000; i++) {
        rpp_rx_push(&rx, 0x55, &f);
    }
    CHECK(!rpp_rx_push(&rx, 0x7E, &f), "overlong body discarded");
    w3 = build_pico_frame(9, RPP_DEVICE_SAFETY, RPP_DEVICE_ESP, pl, 4, wire2);
    CHECK(feed(&rx, wire2, w3, &f), "frame after an overflow delivered");
}

static void test_status(void)
{
    uint8_t p[16 + 2 * 40];
    rpp_status_t st;
    memset(p, 0, sizeof(p));
    p[0] = RPP_CMD_UPDATE_STATUS;
    p[1] = RPP_STATE_RECEIVING;
    p[2] = 0;
    put_u32(p + 3, 248u * 100u);
    put_u32(p + 7, 3400);
    put_u32(p + 11, 100);
    p[15] = 3;
    p[16] = 7;
    p[17] = 0;
    p[18] = 0x34;
    p[19] = 0x12; // 0x1234
    p[20] = 9;
    p[21] = 0;
    CHECK(rpp_parse_status(p, 22, &st), "status with 3 gaps parses");
    CHECK(st.state == RPP_STATE_RECEIVING && st.err == 0, "status state/err");
    CHECK(st.bytes_received == 24800 && st.total_chunks == 3400 && st.received_chunks == 100,
          "status counters");
    CHECK(st.gap_count == 3 && st.gaps[0] == 7 && st.gaps[1] == 0x1234 && st.gaps[2] == 9,
          "status gap list u16 LE");

    CHECK(rpp_parse_status(p, 21, &st) == false, "gap list truncated mid-entry rejected");
    CHECK(rpp_parse_status(p, 15, &st) == false, "short header rejected");
    CHECK(rpp_parse_status(p, 16, &st) == false, "header claiming gaps with none present rejected");
    p[15] = 0;
    CHECK(rpp_parse_status(p, 16, &st) && st.gap_count == 0, "zero-gap status parses");
    p[0] = 0x01;
    CHECK(rpp_parse_status(p, 16, &st) == false, "wrong command byte rejected");
    p[0] = RPP_CMD_UPDATE_STATUS;

    // Gap count above the cap is clamped to 32 (and needs 32 entries present).
    p[15] = 40;
    CHECK(rpp_parse_status(p, 16 + 2 * 40, &st) && st.gap_count == RPP_STATUS_MAX_GAPS,
          "gap count clamped to 32");
    CHECK(rpp_parse_status(p, 16 + 2 * 31, &st) == false, "clamped list still needs its entries");

    // Refusal bits.
    p[15] = 0;
    p[1] = RPP_STATE_REFUSED;
    p[2] = RPP_ERR_RELAY_CLOSED | RPP_ERR_TOO_HOT;
    CHECK(rpp_parse_status(p, 16, &st) && st.err == (RPP_ERR_RELAY_CLOSED | RPP_ERR_TOO_HOT),
          "refusal err bits carried");

    char t[256];
    rpp_describe_state(RPP_STATE_REFUSED, RPP_ERR_RELAY_CLOSED | RPP_ERR_TRIP_PENDING | RPP_ERR_TOO_HOT,
                       t, sizeof(t));
    CHECK(strstr(t, "refused") && strstr(t, "relay is closed") && strstr(t, "trip is pending") &&
              strstr(t, "temperature"),
          "refusal text names every reason");
    CHECK(strstr(t, "clear") == NULL && strstr(t, "CLEAR") == NULL, "refusal text never offers a trip clear");
    CHECK(strstr(t, "power-cycle") != NULL, "trip-pending refusal tells the operator to power-cycle");
    {
        char t2[256];
        rpp_describe_state(RPP_STATE_REFUSED, RPP_ERR_TOO_HOT, t2, sizeof(t2));
        CHECK(strstr(t2, "power-cycle") == NULL, "non-trip refusal has no power-cycle advice");
        // END outstanding + endless no-gap beacons must still time out (no stall).
        rpp_fin_t ff;
        rpp_status_t rs;
        memset(&rs, 0, sizeof(rs));
        rs.state = RPP_STATE_RECEIVING;
        rpp_fin_start(&ff);
        rpp_fin_end_sent(&ff);
        CHECK(rpp_fin_step(&ff, RPP_EV_STATUS, &rs, 100) == RPP_FIN_WAIT, "no-gap beacon right after END waits");
        CHECK(rpp_fin_step(&ff, RPP_EV_STATUS, &rs, RPP_END_REPLY_TIMEOUT_MS) == RPP_FIN_SEND_END,
              "no-gap beacons cannot stall past the END reply timeout");
    }
    rpp_describe_state(RPP_STATE_FAILED, RPP_ERR_CRC_MISMATCH, t, sizeof(t));
    CHECK(strstr(t, "failed") && strstr(t, "CRC"), "failed text names the CRC error");
    rpp_describe_state(RPP_STATE_REJECTED_SLOT_LINKAGE, 0, t, sizeof(t));
    CHECK(strstr(t, "slot"), "slot linkage rejection text");
    rpp_format_err_bits(0, t, sizeof(t));
    CHECK(strcmp(t, "none") == 0, "no err bits reads none");
    rpp_format_err_bits(0xFF, t, 24);
    CHECK(strlen(t) < 24, "err bit text respects a small buffer");
}

static void test_frame_a(void)
{
    uint8_t p[26];
    memset(p, 0, sizeof(p));
    p[0] = 0x01;
    CHECK(rpp_parse_frame_a_active_slot(p, 26) == RPP_SLOT_UNKNOWN, "ACTIVE_SLOT_KNOWN clear -> unknown");
    p[24] = 0x08;
    CHECK(rpp_parse_frame_a_active_slot(p, 26) == RPP_SLOT_A, "known, bit clear -> slot A");
    p[24] = 0x18;
    CHECK(rpp_parse_frame_a_active_slot(p, 26) == RPP_SLOT_B, "known, B bit -> slot B");
    p[24] = 0x10;
    CHECK(rpp_parse_frame_a_active_slot(p, 26) == RPP_SLOT_UNKNOWN, "B bit without KNOWN ignored");
    p[24] = 0x18;
    CHECK(rpp_parse_frame_a_active_slot(p, 25) == RPP_SLOT_UNKNOWN, "V2-length frame has no slot");
    p[0] = 0x14;
    CHECK(rpp_parse_frame_a_active_slot(p, 26) == RPP_SLOT_UNKNOWN, "non-Frame-A payload ignored");
    CHECK(rpp_parse_frame_a_active_slot(NULL, 26) == RPP_SLOT_UNKNOWN, "null payload");
}

static uint8_t *make_image(size_t len, uint32_t sp, uint32_t reset)
{
    uint8_t *img = (uint8_t *)malloc(len);
    for (size_t i = 0; i < len; i++) {
        img[i] = (uint8_t)(i * 31u + 7u);
    }
    put_u32(img, sp);
    put_u32(img + 4, reset);
    return img;
}

static void test_image_and_slots(void)
{
    const size_t len = 5000;
    int slot;
    uint32_t a_reset = 0x10011000u + 0x1C1u;
    uint32_t b_reset = 0x100E1000u + 0x1C1u;

    uint8_t *a = make_image(len, 0x20041F00u, a_reset);
    uint32_t a_crc = rpp_crc32(a, len);
    CHECK(rpp_check_image(a, len, a_crc, &slot) == RPP_IMG_OK && slot == RPP_SLOT_A, "slot A image accepted");
    uint8_t *b = make_image(len, 0x20041F00u, b_reset);
    uint32_t b_crc = rpp_crc32(b, len);
    CHECK(rpp_check_image(b, len, b_crc, &slot) == RPP_IMG_OK && slot == RPP_SLOT_B, "slot B image accepted");

    CHECK(rpp_check_image(a, len, a_crc ^ 1u, &slot) == RPP_IMG_CRC_MISMATCH && slot == RPP_SLOT_UNKNOWN,
          "CRC mismatch refused");
    a[100] ^= 0x01;
    CHECK(rpp_check_image(a, len, a_crc, &slot) == RPP_IMG_CRC_MISMATCH, "flipped body bit refused");
    a[100] ^= 0x01;

    put_u32(a, 0x30000000u);
    CHECK(rpp_check_image(a, len, rpp_crc32(a, len), &slot) == RPP_IMG_BAD_SP, "SP outside SRAM refused");
    put_u32(a, 0x1FFFFFFCu);
    CHECK(rpp_check_image(a, len, rpp_crc32(a, len), &slot) == RPP_IMG_BAD_SP, "SP below SRAM refused");
    put_u32(a, 0x20042000u);
    CHECK(rpp_check_image(a, len, rpp_crc32(a, len), &slot) == RPP_IMG_OK, "SP at SRAM end accepted");
    put_u32(a, 0x20041F00u);
    put_u32(a + 4, 0x10011000u + 0x1C0u);
    CHECK(rpp_check_image(a, len, rpp_crc32(a, len), &slot) == RPP_IMG_BAD_THUMB, "reset without Thumb bit refused");
    put_u32(a + 4, 0x10200001u);
    CHECK(rpp_check_image(a, len, rpp_crc32(a, len), &slot) == RPP_IMG_BAD_RESET, "reset outside both slots refused");
    put_u32(a + 4, 0x100E1000u + 0xD0000u + 1u);
    CHECK(rpp_check_image(a, len, rpp_crc32(a, len), &slot) == RPP_IMG_BAD_RESET, "reset one past slot B window refused");
    put_u32(a + 4, 0x10011001u);
    CHECK(rpp_check_image(a, len, rpp_crc32(a, len), &slot) == RPP_IMG_OK && slot == RPP_SLOT_A,
          "reset at slot A base accepted");
    put_u32(a + 4, 0x08000001u);
    CHECK(rpp_check_image(a, len, a_crc, &slot) == RPP_IMG_BAD_RESET,
          "vector verdict wins over a stale CRC for a wrong-slot file");

    CHECK(rpp_check_image(a, 4, 0, &slot) == RPP_IMG_EMPTY, "too short to hold vectors");
    CHECK(rpp_check_image(NULL, 100, 0, &slot) == RPP_IMG_EMPTY, "null image");
    CHECK(rpp_check_image(a, RPP_SLOT_SIZE + 1u, 0, &slot) == RPP_IMG_TOO_BIG, "larger than the slot refused");
    CHECK(rpp_check_image(a, RPP_SLOT_SIZE + 1u, 0, &slot) != RPP_IMG_OK && slot == RPP_SLOT_UNKNOWN,
          "slot unknown on refusal");
    CHECK(rpp_slot_xip_base(RPP_SLOT_A) == 0x10011000u && rpp_slot_xip_base(RPP_SLOT_B) == 0x100E1000u,
          "slot bases");
    CHECK(rpp_slot_xip_base(5) == 0, "bad slot has no base");
    CHECK(strstr(rpp_image_result_str(RPP_IMG_BAD_RESET), "SaftyFW_slotA.bin") != NULL,
          "wrong-slot message names the files");

    // Target resolution.
    rpp_target_t t = rpp_resolve_target(RPP_SLOT_A, RPP_SLOT_UNKNOWN);
    CHECK(t.target_slot == RPP_SLOT_B && t.source == RPP_TARGET_FROM_APP, "active A -> target B (from app)");
    t = rpp_resolve_target(RPP_SLOT_B, RPP_SLOT_UNKNOWN);
    CHECK(t.target_slot == RPP_SLOT_A && t.source == RPP_TARGET_FROM_APP, "active B -> target A (from app)");
    t = rpp_resolve_target(RPP_SLOT_A, RPP_SLOT_A);
    CHECK(t.target_slot == RPP_SLOT_B && t.source == RPP_TARGET_FROM_APP, "Frame A beats the operator");
    t = rpp_resolve_target(RPP_SLOT_UNKNOWN, RPP_SLOT_A);
    CHECK(t.target_slot == RPP_SLOT_A && t.source == RPP_TARGET_OPERATOR, "operator slot used without Frame A");
    t = rpp_resolve_target(RPP_SLOT_UNKNOWN, RPP_SLOT_UNKNOWN);
    CHECK(t.target_slot == RPP_SLOT_UNKNOWN && t.source == RPP_TARGET_UNRESOLVED,
          "nothing known -> unresolved, never an assumed slot");
    CHECK(!rpp_image_matches_target(RPP_SLOT_B, t), "B image refused for an unresolved target");
    CHECK(!rpp_image_matches_target(RPP_SLOT_A, t), "A image refused for an unresolved target");
    CHECK(!rpp_image_matches_target(RPP_SLOT_UNKNOWN, t), "unknown image slot never matches");
    t = rpp_resolve_target(RPP_SLOT_UNKNOWN, RPP_SLOT_B);
    CHECK(rpp_image_matches_target(RPP_SLOT_B, t) && !rpp_image_matches_target(RPP_SLOT_A, t),
          "operator slot B matches only a B image");

    // Refusal text.
    char why[400];
    rpp_target_t un = rpp_resolve_target(RPP_SLOT_UNKNOWN, RPP_SLOT_UNKNOWN);
    CHECK(rpp_describe_target_refusal(un, RPP_SLOT_B, true, why, sizeof(why)) &&
              strstr(why, "target unverified") && strstr(why, "bootloader") &&
              strstr(why, "cannot report") && strstr(why, "unbootable") && strstr(why, "SWD"),
          "bootloader unresolved text: unverified, cannot read, unbootable until SWD");
    CHECK(rpp_describe_target_refusal(un, RPP_SLOT_B, false, why, sizeof(why)) &&
              strstr(why, "target unverified") && strstr(why, "application did not report") &&
              strstr(why, "bootloader") == NULL,
          "app unresolved text does not claim the bootloader cannot be read");
    t = rpp_resolve_target(RPP_SLOT_UNKNOWN, RPP_SLOT_A);
    CHECK(rpp_describe_target_refusal(t, RPP_SLOT_B, true, why, sizeof(why)) &&
              strstr(why, "target unverified") && strstr(why, "slot B") && strstr(why, "slot A"),
          "operator-sourced mismatch says target unverified");
    t = rpp_resolve_target(RPP_SLOT_A, RPP_SLOT_UNKNOWN);
    CHECK(rpp_describe_target_refusal(t, RPP_SLOT_A, false, why, sizeof(why)) &&
              strstr(why, "target unverified") == NULL && strstr(why, "read from the Pico"),
          "app-sourced mismatch does not say unverified");
    CHECK(!rpp_describe_target_refusal(t, RPP_SLOT_B, false, why, sizeof(why)),
          "matching target has nothing to refuse");

    // ANNOUNCE_VERSION packer: 0x0F, protocol 16, min 7, empty strings.
    uint8_t ap[KILNLINK_FRAME_MAX_PAYLOAD];
    CHECK(rpp_pack_announce(ap) == 9 && ap[0] == 0x0F && ap[1] == 16 && ap[2] == 0 && ap[3] == 7 &&
              ap[4] == 0 && ap[6] == 0 && ap[7] == 0,
          "announce payload layout (protocol 16, min 7, no strings)");

    CHECK(rpp_chunk_count(0) == 0 && rpp_chunk_count(1) == 1 && rpp_chunk_count(248) == 1 &&
              rpp_chunk_count(249) == 2 && rpp_chunk_count(RPP_SLOT_SIZE) == 3436,
          "chunk count");
    free(a);
    free(b);
}

// ---------------------------------------------------------------------------
// Fake receiver (SaftyFW bootloader / application behaviour as read from
// bootloader/recovery_update.c, src/tasks/update_task.c and
// src/update/received_ranges.c) driven in virtual time. The properties that
// matter, and that earlier versions of this fake got wrong, with the receiver
// source each one comes from:
//
//  Both receivers
//  - While a transfer is active and chunks are missing they beacon
//    UPDATE_STATUS(RECEIVING) with up to 32 gap indices taken from a ROTATING
//    cursor: update_received_ranges_find_gaps() scans circularly from the
//    cursor (received_ranges.c:61-82), the cursor then moves to the last
//    reported gap + 1, and a pass that reaches the final chunk counts one
//    retransmit round (recovery_update.c:443-470, update_task.c:1286-1315).
//    A list can therefore wrap past the end of the image and so name TAIL
//    gaps, and it is NOT "every gap behind the highest chunk received".
//    A round count that stops update_retransmit_should_continue() (< 10)
//    ends the transfer with FAILED + RETRANSMIT_CAP.
//  - Once every chunk is in they are SILENT until END (the periodic report
//    returns when update_received_ranges_is_complete(), recovery_update.c:
//    438-440, update_task.c:1281-1283).
//  - END with chunks still missing is answered RECEIVING with ZERO gaps, not a
//    gap list (recovery_update.c:383-386, update_task.c:1146); the gaps arrive
//    with the next periodic beacon.
//  - END with everything in goes VERIFYING, then COMPLETE (or FAILED on a CRC
//    mismatch) after the synchronous CRC check, each reported once.
//  - DATA/END while no transfer is active are discarded.
//
//  Bootloader only
//  - polls a 32-byte UART FIFO and programs flash with interrupts off: a DATA
//    frame arriving < 12 ms after the previous one overruns it and is lost;
//  - silent for the whole (synchronous) erase;
//  - once a transfer ended (COMPLETE/FAILED) it keeps beaconing IDLE every 1 s
//    (recovery_update.c:435-437) -- it does NOT go quiet.
//
//  Application only
//  - sends NO IDLE beacons, before BEGIN or after a transfer (update_task.c:
//    1279); it reports only while a transfer is active;
//  - sends ERASING exactly once, immediately before the synchronous erase
//    (update_task.c:958), then is blocked (no drain, no beacons) until done;
//  - every UPDATE_* frame goes through a 4-deep queue
//    (UPDATE_TASK_QUEUE_DEPTH, update_task.c:134) drained once per wake of
//    UPDATE_TASK_POLL_MS = 100 ms (update_task.c:131, 1683-1697); a frame
//    that finds the queue full is DROPPED (update_task.c:1610-1622). The
//    time the drained frames themselves cost is modelled by
//    app_wake_extra_ms (it lengthens each wake window).
// ---------------------------------------------------------------------------
#define FR_MAX_CHUNKS 3600
#define FR_QUEUE 64
#define FR_APP_QUEUE_DEPTH 4 // update_task.c:134
#define FR_APP_POLL_MS 100   // update_task.c:131
#define FR_ROUND_CAP 10      // update_retransmit_should_continue(): round_count < 10
#define FR_FIFO_GAP_MS 12    // bootloader: a frame < 12 ms after the last overruns the FIFO

typedef struct {
    uint8_t kind; // 'B' begin, 'D' data, 'E' end, 'A' abort
    uint32_t idx;
} fr_msg_t;

typedef struct {
    bool bootloader;
    uint32_t beacon_ms;
    uint32_t erase_ms;
    uint32_t verify_ms;
    uint32_t total;
    bool crc_good;
    bool deaf_first_begin;  // the first BEGIN is not seen
    bool drop_all_data;     // never accepts DATA
    uint32_t drop_ends;
    bool drop_verifying;    // the VERIFYING status is lost on the way to the ESP
    bool drop_complete;     // the COMPLETE status is lost on the way to the ESP
    uint32_t app_wake_extra_ms; // app: extra time each wake takes beyond the 100 ms poll
    uint32_t loss_ppm;      // random loss of DATA frames (all transmissions), parts per million
    uint32_t lcg;
    bool drop_first[FR_MAX_CHUNKS]; // lose this chunk's first transmission
    // receiver state
    bool active;            // a transfer is active (the real s_transfer_active)
    int state;              // RPP_STATE_*
    bool have_chunk[FR_MAX_CHUNKS];
    uint32_t received;
    uint32_t cursor;        // s_gap_cursor
    bool pass_had_gap;      // s_pass_had_gap
    uint32_t round_count;   // s_retransmit_round_count
    uint32_t max_round_count;
    int64_t erase_done, verify_done, last_beacon, last_data, next_wake;
    // app input queue
    fr_msg_t inq[FR_APP_QUEUE_DEPTH];
    int qn;
    // observations
    uint32_t begins_seen, begins_while_erasing, ends_seen, aborts_seen, overruns, statuses_emitted;
    uint32_t queue_drops, idle_beacons, erasing_statuses, random_losses;
    int64_t min_data_gap_ms;
    // output queue of encoded status payloads
    uint8_t q[FR_QUEUE][16 + 2 * RPP_STATUS_MAX_GAPS];
    size_t qlen[FR_QUEUE];
    int qh, qt;
} fake_rx_t;

static int64_t g_now_ms;

static void fr_init(fake_rx_t *r, bool bootloader, uint32_t total)
{
    memset(r, 0, sizeof(*r));
    r->bootloader = bootloader;
    r->beacon_ms = bootloader ? 1000u : 500u;
    r->erase_ms = 1500;
    r->verify_ms = 300;
    r->total = total;
    r->crc_good = true;
    r->state = RPP_STATE_IDLE;
    r->min_data_gap_ms = 1000000;
    r->last_data = -1000000;
    r->last_beacon = g_now_ms;
    r->next_wake = g_now_ms + FR_APP_POLL_MS;
    r->lcg = 12345u;
}

static fake_rx_t *fr_new(bool bootloader, uint32_t total)
{
    fake_rx_t *r = calloc(1, sizeof(*r));
    g_now_ms = 0;
    fr_init(r, bootloader, total);
    return r;
}

// update_received_ranges_find_gaps(): circular scan from `start`.
static uint32_t fr_find_gaps(const fake_rx_t *r, uint32_t start, uint16_t *out, uint32_t max_out)
{
    if (r->total == 0 || max_out == 0) {
        return 0;
    }
    uint32_t found = 0;
    uint32_t i = start % r->total;
    for (uint32_t scanned = 0; scanned < r->total && found < max_out; scanned++) {
        if (!r->have_chunk[i]) {
            out[found++] = (uint16_t)i;
        }
        i = (i + 1u) % r->total;
    }
    return found;
}

// Delivers one status to the ESP (unless the scenario loses it on the wire).
static void fr_emit(fake_rx_t *r, int state, uint8_t err, const uint16_t *gaps, uint32_t ng)
{
    if ((state == RPP_STATE_COMPLETE && r->drop_complete) ||
        (state == RPP_STATE_VERIFYING && r->drop_verifying)) {
        return;
    }
    uint8_t *p = r->q[r->qt % FR_QUEUE];
    memset(p, 0, 16);
    p[0] = RPP_CMD_UPDATE_STATUS;
    p[1] = (uint8_t)state;
    p[2] = err;
    put_u32(p + 3, r->received * RPP_CHUNK_LEN);
    put_u32(p + 7, r->total);
    put_u32(p + 11, r->received);
    p[15] = (uint8_t)ng;
    for (uint32_t i = 0; i < ng; i++) {
        p[16 + 2 * i] = (uint8_t)gaps[i];
        p[17 + 2 * i] = (uint8_t)(gaps[i] >> 8);
    }
    r->qlen[r->qt % FR_QUEUE] = 16 + 2 * ng;
    r->qt++;
    r->statuses_emitted++;
    if (state == RPP_STATE_IDLE) {
        r->idle_beacons++;
    }
    if (state == RPP_STATE_ERASING) {
        r->erasing_statuses++;
    }
}

static bool fr_complete(const fake_rx_t *r)
{
    return r->received >= r->total;
}

// recovery_periodic_status() / update_task_periodic_status().
#ifndef FR_IDLE_ONLY_BOOTLOADER
#define FR_IDLE_ONLY_BOOTLOADER (r->bootloader)
#endif
static void fr_beacon(fake_rx_t *r)
{
    if (!r->active) {
        if (FR_IDLE_ONLY_BOOTLOADER) {
            fr_emit(r, RPP_STATE_IDLE, 0, NULL, 0); // recovery_update.c:435-437
        }
        return; // application: no IDLE beacons (update_task.c:1279)
    }
    if (r->state != RPP_STATE_RECEIVING) {
        return; // erase / verify are synchronous: nothing beacons meanwhile
    }
    if (fr_complete(r)) {
        return; // END will finish this transfer
    }
    uint16_t gaps[RPP_STATUS_MAX_GAPS];
    uint32_t n = fr_find_gaps(r, r->cursor, gaps, RPP_STATUS_MAX_GAPS);
    if (n > 0) {
        r->pass_had_gap = true;
        uint32_t next_cursor = (uint32_t)gaps[n - 1] + 1u;
        if (next_cursor >= r->total) {
            if (r->pass_had_gap) {
                r->round_count++;
                if (r->round_count > r->max_round_count) {
                    r->max_round_count = r->round_count;
                }
            }
            r->pass_had_gap = false;
            r->cursor = 0;
        } else {
            r->cursor = next_cursor;
        }
    } else {
        r->cursor = 0;
        r->pass_had_gap = false;
    }
    if (!(r->round_count < FR_ROUND_CAP)) {
        r->active = false;
        r->state = RPP_STATE_FAILED;
        fr_emit(r, RPP_STATE_FAILED, RPP_ERR_RETRANSMIT_CAP, NULL, 0);
        return;
    }
    fr_emit(r, RPP_STATE_RECEIVING, 0, gaps, n);
}

// --- frame handlers (identical for both receivers; the app runs them from its
// wake, the bootloader straight off the UART) -------------------------------
static void fr_do_begin(fake_rx_t *r)
{
    if (r->state == RPP_STATE_ERASING) {
        r->begins_while_erasing++;
    }
    if (!r->active || r->state == RPP_STATE_ERASING) {
        if (!r->active) {
            memset(r->have_chunk, 0, sizeof(r->have_chunk));
            r->received = 0;
            r->cursor = 0;
            r->pass_had_gap = false;
            r->round_count = 0;
        }
        r->active = true;
        r->state = RPP_STATE_ERASING;
        r->erase_done = g_now_ms + r->erase_ms;
        if (!r->bootloader) {
            fr_emit(r, RPP_STATE_ERASING, 0, NULL, 0); // once, before the erase (update_task.c:958)
        }
    }
}

static void fr_do_data(fake_rx_t *r, uint32_t idx)
{
    if (!r->active || r->state != RPP_STATE_RECEIVING || r->drop_all_data || idx >= r->total) {
        return;
    }
    if (r->drop_first[idx]) {
        r->drop_first[idx] = false;
        return;
    }
    if (!r->have_chunk[idx]) {
        r->have_chunk[idx] = true;
        r->received++;
    }
}

static void fr_do_end(fake_rx_t *r)
{
    if (!r->active || r->state != RPP_STATE_RECEIVING) {
        return; // discarded while no transfer is active (update_task.c:1132)
    }
    if (!fr_complete(r)) {
        // END too early: RECEIVING with ZERO gaps (recovery_update.c:383-386,
        // update_task.c:1146); the missing chunks come with the next beacon.
        fr_emit(r, RPP_STATE_RECEIVING, 0, NULL, 0); // early-END reply
        return;
    }
    r->state = RPP_STATE_VERIFYING;
    r->verify_done = g_now_ms + r->verify_ms;
    fr_emit(r, RPP_STATE_VERIFYING, 0, NULL, 0);
}

static void fr_do_abort(fake_rx_t *r)
{
    if (!r->active) {
        return;
    }
    r->active = false;
    r->state = RPP_STATE_ABORTED;
    fr_emit(r, RPP_STATE_ABORTED, 0, NULL, 0);
}

static void fr_dispatch(fake_rx_t *r, const fr_msg_t *m)
{
    switch (m->kind) {
    case 'B': fr_do_begin(r); break;
    case 'D': fr_do_data(r, m->idx); break;
    case 'E': fr_do_end(r); break;
    case 'A': fr_do_abort(r); break;
    }
}

// A frame arrives on the wire. The application queues it; the bootloader
// handles it on the spot.
static void fr_arrive(fake_rx_t *r, uint8_t kind, uint32_t idx)
{
    fr_msg_t m = {kind, idx};
    if (r->bootloader) {
        fr_dispatch(r, &m);
        return;
    }
#ifndef FR_NO_APP_QUEUE
    if (r->qn >= FR_APP_QUEUE_DEPTH) {
        r->queue_drops++; // xQueueSend(..., 0) on a full queue (update_task.c:1610-1622)
        return;
    }
    r->inq[r->qn++] = m;
#else
    fr_dispatch(r, &m);
#endif
}

static void fr_begin(fake_rx_t *r)
{
    r->begins_seen++;
    if (r->deaf_first_begin && r->begins_seen == 1) {
        return;
    }
    fr_arrive(r, 'B', 0);
}

static void fr_data(fake_rx_t *r, uint32_t idx)
{
    if (r->bootloader) {
        int64_t gap = g_now_ms - r->last_data;
        r->last_data = g_now_ms;
        if (r->state == RPP_STATE_RECEIVING && gap < r->min_data_gap_ms) {
            r->min_data_gap_ms = gap;
        }
        if (gap < FR_FIFO_GAP_MS) {
            r->overruns++; // FIFO overrun: the frame is lost
            return;
        }
    }
    if (r->loss_ppm) {
        r->lcg = r->lcg * 1664525u + 1013904223u;
        if ((r->lcg >> 8) % 1000000u < r->loss_ppm) {
            r->random_losses++;
            return;
        }
    }
    fr_arrive(r, 'D', idx);
}

static void fr_end(fake_rx_t *r)
{
    r->ends_seen++;
    if (r->drop_ends > 0) {
        r->drop_ends--;
        return; // the END frame was lost on the wire: the receiver never saw it
    }
    fr_arrive(r, 'E', 0);
}

static void fr_abort(fake_rx_t *r)
{
    r->aborts_seen++;
    fr_arrive(r, 'A', 0);
}

static bool fr_pop(fake_rx_t *r, rpp_status_t *st)
{
    while (r->qh < r->qt) {
        int i = r->qh % FR_QUEUE;
        r->qh++;
        if (rpp_parse_status(r->q[i], r->qlen[i], st)) {
            return true;
        }
    }
    return false;
}

static void fr_tick(fake_rx_t *r)
{
    if (r->state == RPP_STATE_ERASING && g_now_ms >= r->erase_done) {
        r->state = RPP_STATE_RECEIVING;
        r->last_beacon = g_now_ms;
        fr_emit(r, RPP_STATE_RECEIVING, 0, NULL, 0);
    }
    if (r->state == RPP_STATE_VERIFYING && g_now_ms >= r->verify_done) {
        r->active = false;
        if (r->crc_good) {
            r->state = RPP_STATE_COMPLETE;
            fr_emit(r, RPP_STATE_COMPLETE, 0, NULL, 0);
        } else {
            r->state = RPP_STATE_FAILED;
            fr_emit(r, RPP_STATE_FAILED, RPP_ERR_CRC_MISMATCH, NULL, 0);
        }
    }
    if (r->bootloader) {
        if ((int64_t)(g_now_ms - r->last_beacon) >= (int64_t)r->beacon_ms) {
            r->last_beacon = g_now_ms;
            fr_beacon(r);
        }
        return;
    }
    // Application: one wake per poll period. The task is blocked inside a
    // synchronous erase / CRC check, so no wake happens until those finish.
    if (r->state == RPP_STATE_ERASING || r->state == RPP_STATE_VERIFYING) {
        return;
    }
    if (g_now_ms >= r->next_wake) {
        for (int i = 0; i < r->qn; i++) {
            fr_dispatch(r, &r->inq[i]);
        }
        r->qn = 0;
        if ((int64_t)(g_now_ms - r->last_beacon) >= (int64_t)r->beacon_ms) {
            r->last_beacon = g_now_ms;
            fr_beacon(r);
        }
        r->next_wake = g_now_ms + FR_APP_POLL_MS + r->app_wake_extra_ms;
    }
}

static void advance(fake_rx_t *r, int64_t ms)
{
    for (int64_t i = 0; i < ms; i++) {
        g_now_ms++;
        fr_tick(r);
    }
}

// --- relay driver: mirrors recovery_pico.c's run_transfer flow, on the pure
// helpers (rpp_begin_*, rpp_fin_*, rpp_pace_*) -------------------------------
typedef struct {
    bool ok;
    bool unknown;      // RPP_FIN_UNKNOWN: outcome unknown, no ABORT sent
    bool abort_sent;   // the driver sent ABORT (a plain failure)
    const char *why;
    uint32_t end_sends;
    uint32_t retransmits;
    uint32_t chunks_sent;
    int64_t elapsed_ms;
} drive_t;

static bool drive_begin(fake_rx_t *r, drive_t *d)
{
    rpp_begin_t b;
    fr_begin(r);
    rpp_begin_start(&b, g_now_ms);
    int64_t t0 = g_now_ms;
    while (g_now_ms - t0 < (int64_t)RPP_ERASE_TIMEOUT_MS + 5000) {
        rpp_status_t st;
        rpp_begin_action_t a;
        if (fr_pop(r, &st)) {
            a = rpp_begin_step(&b, &st, g_now_ms);
        } else {
            a = rpp_begin_step(&b, NULL, g_now_ms);
        }
        if (a == RPP_BEGIN_RECEIVING) {
            return true;
        }
        if (a == RPP_BEGIN_FAIL) {
            d->why = "begin failed";
            return false;
        }
        if (a == RPP_BEGIN_RESEND) {
            fr_begin(r);
        }
        advance(r, 10);
    }
    d->why = "begin timeout";
    return false;
}

static void paced_send(fake_rx_t *r, uint32_t idx, int64_t *next_send_us, uint32_t pace_ms, drive_t *d)
{
    for (;;) {
        uint32_t w = rpp_pace_wait_us(g_now_ms * 1000, *next_send_us);
        if (w == 0) {
            break;
        }
        // 10 ms RTOS tick: delay whole ticks rounded up (never short).
        uint32_t ticks = rpp_pace_delay_ticks(w, 10000u);
        advance(r, ticks ? (int64_t)ticks * 10 : 1); // (0 ticks only from a broken helper)
    }
    fr_data(r, idx);
    d->chunks_sent++;
    *next_send_us = g_now_ms * 1000 + (int64_t)pace_ms * 1000;
}

static bool state_is_terminal(uint8_t s)
{
    return s == RPP_STATE_FAILED || s == RPP_STATE_ABORTED || s == RPP_STATE_REFUSED ||
           s == RPP_STATE_REJECTED_SLOT_LINKAGE || s == RPP_STATE_REFUSED_RUNNING_IMAGE_OVERLAP;
}

static drive_t drive_transfer(fake_rx_t *r, uint32_t pace_ms)
{
    drive_t d;
    memset(&d, 0, sizeof(d));
    int64_t start = g_now_ms;
    if (!drive_begin(r, &d)) {
        d.elapsed_ms = g_now_ms - start;
        return d;
    }
    int64_t next_us = g_now_ms * 1000;
    for (uint32_t i = 0; i < r->total; i++) {
        paced_send(r, i, &next_us, pace_ms, &d);
    }
    // Statuses that arrived mid-pass are stale: discard, then END first.
    rpp_status_t st;
    memset(&st, 0, sizeof(st));
    while (fr_pop(r, &st)) {
    }
    rpp_fin_t f;
    rpp_fin_start(&f);
    rpp_fin_action_t act = RPP_FIN_SEND_END;
    int64_t t_end = g_now_ms;
    int64_t deadline = g_now_ms + 900000;
    while (g_now_ms < deadline) {
        if (act == RPP_FIN_DONE) {
            d.ok = true;
            break;
        }
        if (act == RPP_FIN_UNKNOWN) {
            // Outcome unknown: NO ABORT (recovery_pico.c finish_unknown()).
            d.unknown = true;
            d.why = f.why;
            break;
        }
        if (act == RPP_FIN_FAIL) {
            d.why = f.why;
            if (!state_is_terminal(st.state)) { // the Pico did not end it itself
                fr_abort(r);
                d.abort_sent = true;
            }
            break;
        }
        if (act == RPP_FIN_RETRANSMIT) {
            d.retransmits++;
            rpp_status_t last = st; // gaps from the status that asked for it
            // NB: next_us is deliberately NOT reset here -- the previous frame's
            // pace still applies (resetting it overran the receiver).
            for (uint8_t g = 0; g < last.gap_count; g++) {
                if (last.gaps[g] < r->total) {
                    paced_send(r, last.gaps[g], &next_us, pace_ms, &d);
                }
            }
        }
        if (act == RPP_FIN_SEND_END || act == RPP_FIN_RETRANSMIT) {
            // Statuses queued before this END (beacons during a retransmit
            // burst) describe the past: only END's own reply counts.
            while (rpp_pace_wait_us(g_now_ms * 1000, next_us) > 0) {
                advance(r, 1); // END is a frame too: honour the pace after the last DATA
            }
            rpp_status_t stale;
            while (fr_pop(r, &stale)) {
            }
            fr_end(r);
            rpp_fin_end_sent(&f);
            d.end_sends++;
            t_end = g_now_ms;
        }
        // Wait for a status for up to one gap round.
        bool got = false;
        for (int64_t w = 0; w < (int64_t)RPP_GAP_ROUND_WAIT_MS && !got; w += 10) {
            if (fr_pop(r, &st)) {
                got = true;
                break;
            }
            advance(r, 10);
        }
        act = rpp_fin_step(&f, got ? RPP_EV_STATUS : RPP_EV_QUIET, got ? &st : NULL,
                           (uint32_t)(g_now_ms - t_end));
        if (f.restart_timer) {
            t_end = g_now_ms;
            f.restart_timer = false;
        }
    }
    if (!d.ok && !d.why) {
        d.why = "driver deadline (never finished)";
    }
    d.elapsed_ms = g_now_ms - start;
    return d;
}

// What the OLD relay did after the first pass: wait for a status that reports
// gap_count == 0 with every chunk in, then send END. Kept only to prove the
// fake receiver reproduces the deadlock the rewrite fixed.
static bool legacy_relay_sees_complete_status(fake_rx_t *r)
{
    if (!drive_begin(r, &(drive_t){0})) {
        return false;
    }
    drive_t d;
    memset(&d, 0, sizeof(d));
    int64_t next_us = g_now_ms * 1000;
    for (uint32_t i = 0; i < r->total; i++) {
        paced_send(r, i, &next_us, rpp_data_pace_ms(r->bootloader), &d);
    }
    rpp_status_t st;
    while (fr_pop(r, &st)) {
    }
    for (int64_t w = 0; w < 4 * (int64_t)RPP_GAP_ROUND_WAIT_MS; w += 10) { // old: 4 silent rounds -> fail
        advance(r, 10);
        if (fr_pop(r, &st) && st.gap_count == 0 && st.received_chunks >= st.total_chunks) {
            return true;
        }
    }
    return false;
}

static void test_receiver_model(void)
{
    for (int v = 0; v < 2; v++) {
        fake_rx_t *r = fr_new(v == 0, 120);
        CHECK(!legacy_relay_sees_complete_status(r),
              "old relay waited for a no-gap status the silent receiver never sends");
        CHECK(r->received == r->total, "legacy path: every chunk was delivered");
        free(r);
    }

    // END sent too early is answered RECEIVING with ZERO gaps (recovery_update.c:383-386,
    // update_task.c:1146), never the gap list: the gaps come with the next beacon.
    for (int v = 0; v < 2; v++) {
        const char *vn = v == 0 ? "bootloader" : "app";
        char nm[160];
        fake_rx_t *r = fr_new(v == 0, 50);
        fr_begin(r);
        advance(r, 3000);
        rpp_status_t st;
        while (fr_pop(r, &st)) {
        }
        for (uint32_t i = 0; i < 40; i++) {
            advance(r, 40);
            fr_data(r, i);
        }
        advance(r, 200);
        while (fr_pop(r, &st)) {
        }
        fr_end(r);
        advance(r, 150); // app: the END is handled at its next wake
        snprintf(nm, sizeof(nm), "%s early END answered RECEIVING with 0 gaps", vn);
        CHECK(fr_pop(r, &st) && st.state == RPP_STATE_RECEIVING && st.gap_count == 0 &&
                  st.received_chunks == 40,
              nm);
        free(r);
    }

    // Rotating-cursor gap lists (received_ranges.c:61-82; recovery_update.c:443-470).
    {
        fake_rx_t *r = fr_new(true, 100);
        r->active = true;
        r->state = RPP_STATE_RECEIVING;
        for (uint32_t i = 0; i < 40; i++) {
            r->have_chunk[i] = true;
        }
        r->received = 40;
        rpp_status_t st;
        fr_beacon(r);
        CHECK(fr_pop(r, &st) && st.gap_count == 32 && st.gaps[0] == 40 && st.gaps[31] == 71 &&
                  r->cursor == 72 && r->round_count == 0,
              "first beacon lists 32 gaps from the cursor and moves it past the last one");
        fr_beacon(r);
        // The scan is circular: it names the TAIL gaps 72..99, then wraps to the
        // front and picks up 40..43 to fill the 32 slots. The cursor lands on 44
        // and no round is counted (the last listed gap is not the final chunk).
        CHECK(fr_pop(r, &st) && st.gap_count == 32 && st.gaps[0] == 72 && st.gaps[27] == 99 &&
                  st.gaps[28] == 40 && st.gaps[31] == 43,
              "second beacon reports the TAIL gaps then wraps to the front");
        CHECK(r->cursor == 44 && r->round_count == 0, "a wrapped list not ending at the last chunk counts no round");
        free(r);

        // A pass whose list ENDS at the final chunk counts one retransmit round
        // (recovery_update.c:454-460) and rewinds the cursor.
        r = fr_new(true, 100);
        r->active = true;
        r->state = RPP_STATE_RECEIVING;
        for (uint32_t i = 0; i < 90; i++) {
            r->have_chunk[i] = true;
        }
        r->received = 90;
        fr_beacon(r);
        CHECK(fr_pop(r, &st) && st.gap_count == 10 && st.gaps[0] == 90 && st.gaps[9] == 99 &&
                  r->round_count == 1 && r->cursor == 0,
              "a list ending at the final chunk counts one round and rewinds the cursor");
        free(r);

        r = fr_new(true, 100);
        r->active = true;
        r->state = RPP_STATE_RECEIVING;
        for (uint32_t i = 50; i < 100; i++) {
            r->have_chunk[i] = true;
        }
        r->received = 50;
        r->cursor = 30;
        fr_beacon(r);
        CHECK(fr_pop(r, &st) && st.gap_count == 32 && st.gaps[0] == 30 && st.gaps[19] == 49 &&
                  st.gaps[20] == 0 && st.gaps[31] == 11 && r->cursor == 12,
              "a gap list wraps past the end of the image back to chunk 0");
        free(r);

        // The round cap ends the transfer (update_retransmit_should_continue: < 10).
        r = fr_new(true, 100);
        r->active = true;
        r->state = RPP_STATE_RECEIVING;
        r->received = 0;
        for (int i = 0; i < 2000 && r->active; i++) {
            fr_beacon(r);
        }
        CHECK(!r->active && r->state == RPP_STATE_FAILED && r->round_count == FR_ROUND_CAP,
              "ten rounds with gaps end the transfer");
        CHECK(fr_pop(r, &st) && st.state != RPP_STATE_FAILED, "earlier beacons were ordinary reports");
        free(r);
    }

    // Bootloader keeps beaconing IDLE after COMPLETE (recovery_update.c:435-437);
    // the application sends no IDLE beacons, ever (update_task.c:1279).
    for (int v = 0; v < 2; v++) {
        const char *vn = v == 0 ? "bootloader" : "app";
        char nm[160];
        fake_rx_t *r = fr_new(v == 0, 30);
        advance(r, 5000);
        snprintf(nm, sizeof(nm), "%s before BEGIN: %s", vn, v == 0 ? "IDLE beacons" : "silent");
        CHECK(v == 0 ? r->idle_beacons >= 4 : r->statuses_emitted == 0, nm);
        drive_t d = drive_transfer(r, rpp_data_pace_ms(v == 0));
        CHECK(d.ok && r->state == RPP_STATE_COMPLETE, "transfer completes before the IDLE-after-COMPLETE check");
        rpp_status_t st;
        while (fr_pop(r, &st)) {
        }
        uint32_t idle_before = r->idle_beacons, em_before = r->statuses_emitted;
        advance(r, 3500);
        if (v == 0) {
            int idles = 0;
            bool only_idle = true;
            while (fr_pop(r, &st)) {
                idles += st.state == RPP_STATE_IDLE;
                only_idle = only_idle && st.state == RPP_STATE_IDLE;
            }
            CHECK(idles >= 3 && only_idle && r->idle_beacons - idle_before >= 3,
                  "bootloader beacons IDLE every second after COMPLETE, it does not go silent");
        } else {
            CHECK(r->statuses_emitted == em_before && r->idle_beacons == 0,
                  "app sends no IDLE beacon after COMPLETE");
        }
        free(r);
    }

    // ERASING: the app sends it once, before the synchronous erase; the
    // bootloader is silent throughout (update_task.c:958).
    for (int v = 0; v < 2; v++) {
        fake_rx_t *r = fr_new(v == 0, 30);
        r->erase_ms = 30000;
        fr_begin(r);
        advance(r, 40000);
        rpp_status_t st;
        uint32_t erasing = 0;
        while (fr_pop(r, &st)) {
            erasing += st.state == RPP_STATE_ERASING;
        }
        CHECK(v == 0 ? erasing == 0 : erasing == 1,
              v == 0 ? "bootloader never reports ERASING" : "app reports ERASING exactly once");
        free(r);
    }
}

// App input queue: 4 deep, drained once per 100 ms wake; frames dropped when full.
static uint32_t app_queue_drops(uint32_t pace_ms, uint32_t wake_extra_ms, bool *completed)
{
    fake_rx_t *r = fr_new(false, 400);
    r->app_wake_extra_ms = wake_extra_ms;
    drive_t d = drive_transfer(r, pace_ms);
    uint32_t drops = r->queue_drops;
    if (completed) {
        *completed = d.ok;
    }
    free(r);
    return drops;
}

static void test_app_queue(void)
{
    bool ok = false;
    CHECK(RPP_DATA_PACE_APP_MS >= 25u, "app pace at least 4 frames per 100 ms wake");
    CHECK(rpp_data_pace_ms(true) == RPP_DATA_PACE_MS && rpp_data_pace_ms(false) == RPP_DATA_PACE_APP_MS,
          "pace follows the connected receiver");
    CHECK(app_queue_drops(RPP_DATA_PACE_MS, 0, NULL) > 0,
          "the bootloader's 15 ms pace overflows the app's 4-deep queue (frames dropped)");
    CHECK(app_queue_drops(20, 0, NULL) > 0, "20 ms pace: more than 4 frames per wake, queue drops");
    CHECK(app_queue_drops(30, 30, NULL) > 0,
          "30 ms pace with a 30 ms-longer wake still overflows (5 frames in 130 ms)");
    CHECK(app_queue_drops(RPP_DATA_PACE_APP_MS, 0, &ok) == 0 && ok, "app pace: no drops on a nominal 100 ms wake");
    CHECK(app_queue_drops(RPP_DATA_PACE_APP_MS, 30, &ok) == 0 && ok,
          "app pace: no drops when each wake runs 30 ms long (the margin)");
    CHECK(app_queue_drops(RPP_DATA_PACE_APP_MS, 70, NULL) > 0,
          "the margin has a limit: a 170 ms wake window holds 5 frames at 40 ms");
}

static void test_finish_driver(void)
{
    for (int v = 0; v < 2; v++) {
        const char *vn = v == 0 ? "bootloader" : "app";
        uint32_t pace = rpp_data_pace_ms(v == 0);
        fake_rx_t *r = fr_new(v == 0, 200);
        char nm[160];

        // Clean transfer.
        drive_t d = drive_transfer(r, pace);
        snprintf(nm, sizeof(nm), "%s clean transfer completes (%s)", vn, d.why ? d.why : "ok");
        CHECK(d.ok && r->state == RPP_STATE_COMPLETE, nm);
        snprintf(nm, sizeof(nm), "%s clean transfer: exactly one END", vn);
        CHECK(d.end_sends == 1 && r->ends_seen == 1, nm);
        snprintf(nm, sizeof(nm), "%s clean transfer: no FIFO overrun, no queue drop, pace respected", vn);
        CHECK(r->overruns == 0 && r->queue_drops == 0 && r->min_data_gap_ms >= (int64_t)(v == 0 ? pace : 0), nm);
        snprintf(nm, sizeof(nm), "%s clean transfer: BEGIN sent once", vn);
        CHECK(r->begins_seen == 1, nm);
        free(r);

        // Dropped chunks, including the tail chunk (missing past the beacon's cursor).
        r = fr_new(v == 0, 200);
        for (uint32_t i = 3; i < 200; i += 7) {
            r->drop_first[i] = true;
        }
        r->drop_first[199] = true;
        d = drive_transfer(r, pace);
        snprintf(nm, sizeof(nm), "%s dropped chunks recovered by gap rounds (%s)", vn, d.why ? d.why : "ok");
        CHECK(d.ok && r->state == RPP_STATE_COMPLETE && r->received == r->total, nm);
        snprintf(nm, sizeof(nm), "%s dropped chunks needed retransmit rounds", vn);
        CHECK(d.retransmits >= 1 && d.chunks_sent > 200, nm);
        free(r);

        // Heavy loss: every other chunk dropped on first send.
        r = fr_new(v == 0, 300);
        for (uint32_t i = 0; i < 300; i += 2) {
            r->drop_first[i] = true;
        }
        d = drive_transfer(r, pace);
        snprintf(nm, sizeof(nm), "%s heavy loss still completes (%s)", vn, d.why ? d.why : "ok");
        CHECK(d.ok && r->state == RPP_STATE_COMPLETE, nm);
        free(r);

        // Two END replies lost: END is resent after the reply timeout, still completes.
        r = fr_new(v == 0, 100);
        r->drop_ends = 2;
        d = drive_transfer(r, pace);
        snprintf(nm, sizeof(nm), "%s lost END frames are resent (%s)", vn, d.why ? d.why : "ok");
        CHECK(d.ok && d.end_sends == 3, nm);
        free(r);

        // Every END lost (an ignored END): bounded, and reported as outcome
        // UNKNOWN with no ABORT -- never a plain failure, never a hang.
        r = fr_new(v == 0, 100);
        r->drop_ends = 100;
        d = drive_transfer(r, pace);
        advance(r, 500);
        snprintf(nm, sizeof(nm), "%s endless silence after END is outcome-unknown, bounded (%s)", vn,
                 d.why ? d.why : "?");
        CHECK(!d.ok && d.unknown && d.end_sends == RPP_MAX_END_SENDS && d.why != NULL &&
                  strstr(d.why, "outcome unknown") && strstr(d.why, "power-cycle"),
              nm);
        snprintf(nm, sizeof(nm), "%s endless silence after END sends no ABORT", vn);
        CHECK(!d.abort_sent && r->aborts_seen == 0 && r->state != RPP_STATE_ABORTED, nm);
        free(r);

        // CRC mismatch on the Pico: FAILED is reported as a failure (the Pico
        // ended it itself, so no ABORT either).
        r = fr_new(v == 0, 100);
        r->crc_good = false;
        d = drive_transfer(r, pace);
        snprintf(nm, sizeof(nm), "%s CRC mismatch (FAILED) fails the transfer", vn);
        CHECK(!d.ok && !d.unknown && r->state == RPP_STATE_FAILED && !d.abort_sent, nm);
        free(r);

        // Receiver that never accepts DATA: the stall limit ends it (a plain
        // failure, so the relay aborts the Pico).
        r = fr_new(v == 0, 60);
        r->drop_all_data = true;
        d = drive_transfer(r, pace);
        advance(r, 500);
        snprintf(nm, sizeof(nm), "%s a dead data path ends in failure, not a hang", vn);
        CHECK(!d.ok && !d.unknown && d.why != NULL && d.abort_sent && r->aborts_seen == 1, nm);
        free(r);

        // Lost COMPLETE: the Pico finished (or is finishing) and its result
        // never arrives. After VERIFYING was seen this must NOT be FAILED and
        // must send no ABORT: the operator is told the outcome is unknown.
        r = fr_new(v == 0, 100);
        r->drop_complete = true;
        d = drive_transfer(r, pace);
        advance(r, 500);
        snprintf(nm, sizeof(nm), "%s lost COMPLETE after VERIFYING: outcome unknown (%s)", vn, d.why ? d.why : "?");
        CHECK(!d.ok && d.unknown && d.why && strstr(d.why, "outcome unknown") &&
                  strstr(d.why, "power-cycle and check the Pico version") &&
                  strstr(d.why, "do NOT retry blindly"),
              nm);
        snprintf(nm, sizeof(nm), "%s lost COMPLETE: the Pico really did complete, and no ABORT was sent", vn);
        CHECK(r->state == RPP_STATE_COMPLETE && !d.abort_sent && r->aborts_seen == 0, nm);
        free(r);

        // Lost VERIFYING and COMPLETE: END was accepted but nothing was heard
        // (the bootloader then beacons IDLE, the app goes silent).
        r = fr_new(v == 0, 100);
        r->drop_verifying = true;
        r->drop_complete = true;
        d = drive_transfer(r, pace);
        advance(r, 500);
        snprintf(nm, sizeof(nm), "%s lost VERIFYING+COMPLETE: outcome unknown, no ABORT (%s)", vn,
                 d.why ? d.why : "?");
        CHECK(!d.ok && d.unknown && !d.abort_sent && r->aborts_seen == 0 && r->state == RPP_STATE_COMPLETE, nm);
        free(r);

        // Full-size image through the whole flow.
        r = fr_new(v == 0, 3436);
        for (uint32_t i = 5; i < 3436; i += 97) {
            r->drop_first[i] = true;
        }
        d = drive_transfer(r, pace);
        snprintf(nm, sizeof(nm), "%s full 3436-chunk image completes (%s)", vn, d.why ? d.why : "ok");
        CHECK(d.ok && r->state == RPP_STATE_COMPLETE && r->overruns == 0 && r->queue_drops == 0, nm);
        free(r);
    }
}

// Convergence under modelled loss: both receivers, a full-size image, random
// loss on EVERY DATA transmission (retransmissions included) on top of fixed
// first-send drops, within the receivers' round cap (a round counted every
// time the gap cursor reaches the final chunk, recovery_update.c:454-460).
static void test_convergence(void)
{
    static const uint32_t ppm[] = {0, 5000, 20000, 50000};
    for (int v = 0; v < 2; v++) {
        const char *vn = v == 0 ? "bootloader" : "app";
        for (size_t k = 0; k < sizeof(ppm) / sizeof(ppm[0]); k++) {
            char nm[200];
            fake_rx_t *r = fr_new(v == 0, 3436);
            r->loss_ppm = ppm[k];
            r->app_wake_extra_ms = v == 0 ? 0 : 30; // app drain costs time, see RPP_DATA_PACE_APP_MS
            for (uint32_t i = 11; i < 3436; i += 53) {
                r->drop_first[i] = true;
            }
            r->drop_first[3435] = true; // the final chunk too: it makes every beacon a counted round
            drive_t d = drive_transfer(r, rpp_data_pace_ms(v == 0));
            printf("  convergence %-10s loss=%5.1f%% ok=%d rounds_max=%u/%d retransmits=%u sent=%u losses=%u drops=%u t=%llds\n",
                   vn, ppm[k] / 10000.0, d.ok, r->max_round_count, FR_ROUND_CAP, d.retransmits,
                   d.chunks_sent, r->random_losses, r->queue_drops, (long long)(d.elapsed_ms / 1000));
            snprintf(nm, sizeof(nm), "%s converges at %.1f%% loss (%s)", vn, ppm[k] / 10000.0,
                     d.why ? d.why : "ok");
            CHECK(d.ok && r->state == RPP_STATE_COMPLETE && r->received == r->total, nm);
            snprintf(nm, sizeof(nm), "%s at %.1f%% loss stays inside the receiver's round cap", vn,
                     ppm[k] / 10000.0);
            CHECK(r->max_round_count < FR_ROUND_CAP, nm);
            snprintf(nm, sizeof(nm), "%s at %.1f%% loss: no FIFO overrun, no queue drop", vn, ppm[k] / 10000.0);
            CHECK(r->overruns == 0 && r->queue_drops == 0, nm);
            free(r);
        }
    }
}

static void test_begin_wait(void)
{
    // Slow erase: a bootloader is silent for 100 s, then RECEIVING. BEGIN is
    // sent once and never restarted.
    fake_rx_t *r = fr_new(true, 40);
    r->erase_ms = 100000;
    drive_t d = drive_transfer(r, RPP_DATA_PACE_MS);
    CHECK(d.ok && r->begins_seen == 1 && r->begins_while_erasing == 0,
          "100 s silent erase completes without a BEGIN resend");
    free(r);

    // Erase longer than the window fails (bounded).
    r = fr_new(true, 40);
    r->erase_ms = RPP_ERASE_TIMEOUT_MS + 30000u;
    d = drive_transfer(r, RPP_DATA_PACE_MS);
    CHECK(!d.ok && r->begins_seen == 1, "an erase past the window fails without restarting it");
    free(r);

    // Application: ERASING is sent once, then the task is blocked in the erase;
    // BEGIN is never restarted.
    r = fr_new(false, 40);
    r->erase_ms = 60000;
    d = drive_transfer(r, RPP_DATA_PACE_APP_MS);
    CHECK(d.ok && r->begins_while_erasing == 0 && r->erasing_statuses == 1,
          "app erase never triggers a resend");
    free(r);

    // A BEGIN the BOOTLOADER never saw is resent, but only after several IDLE
    // beacons AND the minimum time.
    r = fr_new(true, 40);
    r->deaf_first_begin = true;
    d = drive_transfer(r, RPP_DATA_PACE_MS);
    CHECK(d.ok && r->begins_seen == 2, "a missed BEGIN is resent once and the transfer completes");
    free(r);

    // The APPLICATION sends no IDLE beacons, so a BEGIN it never saw can never
    // be detected: it is NEVER resent and the wait fails closed at the erase
    // timeout (rpp_begin_step doc).
    r = fr_new(false, 40);
    r->deaf_first_begin = true;
    d = drive_transfer(r, RPP_DATA_PACE_APP_MS);
    CHECK(!d.ok && !d.unknown && r->begins_seen == 1 && r->statuses_emitted == 0,
          "app lost BEGIN: never resent, nothing heard");
    CHECK(d.elapsed_ms >= (int64_t)RPP_ERASE_TIMEOUT_MS && d.chunks_sent == 0 && r->received == 0,
          "app lost BEGIN: fails closed at the timeout without sending any DATA");
    free(r);

    // Pure decision: two IDLE beacons never resend; four plus the time do.
    rpp_begin_t b;
    rpp_status_t idle;
    memset(&idle, 0, sizeof(idle));
    idle.state = RPP_STATE_IDLE;
    rpp_begin_start(&b, 0);
    CHECK(rpp_begin_step(&b, &idle, 1000) == RPP_BEGIN_WAIT, "1 idle beacon: wait");
    CHECK(rpp_begin_step(&b, &idle, 2000) == RPP_BEGIN_WAIT, "2 idle beacons: still wait");
    CHECK(rpp_begin_step(&b, &idle, 3000) == RPP_BEGIN_WAIT, "3 idle beacons: still wait");
    CHECK(rpp_begin_step(&b, &idle, 4000) == RPP_BEGIN_WAIT, "4 idle beacons before the minimum time: wait");
    CHECK(rpp_begin_step(&b, &idle, 5000) == RPP_BEGIN_RESEND, "enough beacons and time: resend");
    rpp_begin_start(&b, 0);
    CHECK(rpp_begin_step(&b, &idle, 6000) == RPP_BEGIN_WAIT, "time satisfied, 1 beacon: wait");
    CHECK(rpp_begin_step(&b, &idle, 6001) == RPP_BEGIN_WAIT, "time satisfied, 2 beacons: wait");
    CHECK(rpp_begin_step(&b, &idle, 6002) == RPP_BEGIN_WAIT, "time satisfied, 3 beacons: wait");
    CHECK(rpp_begin_step(&b, &idle, 6003) == RPP_BEGIN_RESEND, "time satisfied, 4 beacons: resend");
    rpp_begin_start(&b, 0);
    for (int i = 0; i < 6; i++) {
        (void)rpp_begin_step(&b, &idle, 6000 + i);
    }
    CHECK(b.sends == 2, "one resend, not one per beacon");
    rpp_status_t er = idle;
    er.state = RPP_STATE_ERASING;
    CHECK(rpp_begin_step(&b, &er, 7000) == RPP_BEGIN_ERASING && b.idle_beacons == 0,
          "an ERASING status clears the idle count");
    // No beacons at all (the application): only the timeout ends the wait.
    rpp_begin_start(&b, 0);
    CHECK(rpp_begin_step(&b, NULL, 60000) == RPP_BEGIN_WAIT && b.sends == 1, "silence: no resend");
    CHECK(rpp_begin_step(&b, NULL, (int64_t)RPP_ERASE_TIMEOUT_MS) == RPP_BEGIN_FAIL, "silence: fail at the timeout");
}

static void test_fin_unit(void)
{
    rpp_fin_t f;
    rpp_status_t st;
    memset(&st, 0, sizeof(st));
    st.total_chunks = 100;

    rpp_fin_start(&f);
    CHECK(rpp_fin_step(&f, RPP_EV_QUIET, NULL, 0) == RPP_FIN_SEND_END, "silent round before END -> send END");
    rpp_fin_end_sent(&f);
    CHECK(rpp_fin_step(&f, RPP_EV_QUIET, NULL, 100) == RPP_FIN_WAIT, "quiet right after END -> wait");
    st.state = RPP_STATE_VERIFYING;
    CHECK(rpp_fin_step(&f, RPP_EV_STATUS, &st, 200) == RPP_FIN_WAIT && f.restart_timer && f.verifying_seen,
          "VERIFYING -> wait and restart the END timer");
    st.state = RPP_STATE_COMPLETE;
    CHECK(rpp_fin_step(&f, RPP_EV_STATUS, &st, 300) == RPP_FIN_DONE, "COMPLETE -> done");

    rpp_fin_start(&f);
    st.state = RPP_STATE_RECEIVING;
    st.received_chunks = 100;
    st.gap_count = 0;
    CHECK(rpp_fin_step(&f, RPP_EV_STATUS, &st, 0) == RPP_FIN_SEND_END, "RECEIVING with no gaps -> END");
    rpp_fin_end_sent(&f);
    CHECK(rpp_fin_step(&f, RPP_EV_STATUS, &st, 10) == RPP_FIN_WAIT, "END outstanding: no second END from a beacon");
    st.received_chunks = 90;
    st.gap_count = 10;
    CHECK(rpp_fin_step(&f, RPP_EV_STATUS, &st, 20) == RPP_FIN_RETRANSMIT, "RECEIVING plus gaps -> gap round");
    rpp_fin_end_sent(&f);
    CHECK(f.end_sends == 1, "end sends reset by a gap round");

    // Before any END nothing can have been accepted: IDLE is a plain failure.
    // Terminal Pico states are always failures.
    int fails[] = {RPP_STATE_FAILED, RPP_STATE_ABORTED, RPP_STATE_REFUSED, RPP_STATE_REJECTED_SLOT_LINKAGE,
                   RPP_STATE_REFUSED_RUNNING_IMAGE_OVERLAP, RPP_STATE_IDLE};
    for (size_t i = 0; i < sizeof(fails) / sizeof(fails[0]); i++) {
        rpp_fin_start(&f);
        st.state = (uint8_t)fails[i];
        CHECK(rpp_fin_step(&f, RPP_EV_STATUS, &st, 0) == RPP_FIN_FAIL && f.why != NULL,
              "terminal or lost-transfer state fails");
    }
    for (size_t i = 0; i < sizeof(fails) / sizeof(fails[0]) - 1; i++) {
        rpp_fin_start(&f);
        rpp_fin_end_sent(&f);
        f.verifying_seen = true;
        st.state = (uint8_t)fails[i];
        CHECK(rpp_fin_step(&f, RPP_EV_STATUS, &st, 0) == RPP_FIN_FAIL,
              "a Pico-reported terminal state stays a failure even after END/VERIFYING");
    }

    // After END (or VERIFYING) an IDLE beacon is ambiguous -- a dropped COMPLETE
    // looks like a reset Pico -- so it is outcome-unknown, never FAILED.
    rpp_fin_start(&f);
    rpp_fin_end_sent(&f);
    st.state = RPP_STATE_IDLE;
    CHECK(rpp_fin_step(&f, RPP_EV_STATUS, &st, 100) == RPP_FIN_UNKNOWN && f.why &&
              strstr(f.why, "outcome unknown") && strstr(f.why, "do NOT retry blindly"),
          "IDLE after END -> outcome unknown");
    rpp_fin_start(&f);
    rpp_fin_end_sent(&f);
    st.state = RPP_STATE_VERIFYING;
    (void)rpp_fin_step(&f, RPP_EV_STATUS, &st, 100);
    st.state = RPP_STATE_IDLE;
    CHECK(rpp_fin_step(&f, RPP_EV_STATUS, &st, 200) == RPP_FIN_UNKNOWN && f.why && strstr(f.why, "verifying") &&
              strstr(f.why, "power-cycle and check the Pico version"),
          "IDLE after VERIFYING -> outcome unknown, says it saw VERIFYING");
    // Silence after VERIFYING: the END timer restarts, then ENDs run out.
    rpp_fin_start(&f);
    rpp_fin_end_sent(&f);
    (void)rpp_fin_step(&f, RPP_EV_STATUS, &st, 0); // IDLE -> unknown (consumed)
    rpp_fin_start(&f);
    rpp_fin_end_sent(&f);
    st.state = RPP_STATE_VERIFYING;
    (void)rpp_fin_step(&f, RPP_EV_STATUS, &st, 100);
    rpp_fin_action_t a2 = RPP_FIN_WAIT;
    uint32_t guard = 0;
    while (a2 != RPP_FIN_UNKNOWN && a2 != RPP_FIN_FAIL && guard++ < 10) {
        a2 = rpp_fin_step(&f, RPP_EV_QUIET, NULL, RPP_END_REPLY_TIMEOUT_MS);
        if (a2 == RPP_FIN_SEND_END) {
            rpp_fin_end_sent(&f);
        }
    }
    CHECK(a2 == RPP_FIN_UNKNOWN && f.end_sends == RPP_MAX_END_SENDS,
          "silence after VERIFYING -> unknown after the allowed END resends");

    // No-progress gap reports end; steady progress does not; hard batch cap.
    rpp_fin_start(&f);
    st.state = RPP_STATE_RECEIVING;
    st.received_chunks = 50;
    st.gap_count = 5;
    rpp_fin_action_t a = RPP_FIN_RETRANSMIT;
    uint32_t n = 0;
    while (a != RPP_FIN_FAIL && n < 100) {
        a = rpp_fin_step(&f, RPP_EV_STATUS, &st, 0);
        n++;
    }
    CHECK(a == RPP_FIN_FAIL && n == RPP_MAX_RETRANSMIT_ROUNDS + 1u, "stalled gap reports fail after the limit");
    rpp_fin_start(&f);
    bool failed = false;
    st.received_chunks = 0;
    for (uint32_t i = 0; i < RPP_MAX_GAP_BATCHES && !failed; i++) {
        st.received_chunks++;
        failed = rpp_fin_step(&f, RPP_EV_STATUS, &st, 0) == RPP_FIN_FAIL;
    }
    CHECK(!failed, "steady progress never trips the stall limit");
    st.received_chunks++;
    CHECK(rpp_fin_step(&f, RPP_EV_STATUS, &st, 0) == RPP_FIN_FAIL, "hard batch cap");

    // END reply timeout: resend, then outcome-unknown after the allowed sends.
    rpp_fin_start(&f);
    rpp_fin_end_sent(&f);
    CHECK(rpp_fin_step(&f, RPP_EV_QUIET, NULL, RPP_END_REPLY_TIMEOUT_MS - 1) == RPP_FIN_WAIT, "before the END reply timeout");
    CHECK(rpp_fin_step(&f, RPP_EV_QUIET, NULL, RPP_END_REPLY_TIMEOUT_MS) == RPP_FIN_SEND_END, "END resent after the reply timeout");
    rpp_fin_end_sent(&f);
    rpp_fin_end_sent(&f);
    CHECK(rpp_fin_step(&f, RPP_EV_QUIET, NULL, RPP_END_REPLY_TIMEOUT_MS) == RPP_FIN_UNKNOWN,
          "END sends exhausted -> outcome unknown");
}

static void test_pace(void)
{
    CHECK(rpp_pace_wait_us(1000, 16000) == 15000u, "pace wait remaining");
    CHECK(rpp_pace_wait_us(16000, 16000) == 0u, "pace deadline reached");
    CHECK(rpp_pace_wait_us(20000, 16000) == 0u, "pace deadline passed");
    // Delay in whole ticks, rounded UP: never short of the requested wait.
    CHECK(rpp_pace_delay_ticks(0, 10000) == 0, "no wait, no ticks");
    CHECK(rpp_pace_delay_ticks(1, 10000) == 1, "1 us rounds up to a tick");
    CHECK(rpp_pace_delay_ticks(10000, 10000) == 1, "exactly one tick");
    CHECK(rpp_pace_delay_ticks(10001, 10000) == 2, "one us over rounds up");
    CHECK(rpp_pace_delay_ticks(15000, 10000) == 2, "15 ms at a 10 ms tick is 2 ticks");
    for (uint32_t w = 1; w < 40000; w += 137) {
        if (rpp_pace_delay_ticks(w, 10000) * 10000u < w) {
            CHECK(0, "tick delay never shorter than the wait");
            break;
        }
    }
    // Behavioural: a sender that ignores the pace overruns the receiver's FIFO,
    // one that honours it does not (the receiver model above drops < 12 ms).
    fake_rx_t *r = fr_new(true, 100);
    drive_t d = drive_transfer(r, 4); // 4 ms pace: too fast
    CHECK(r->overruns > 0, "a 4 ms pace overruns the bootloader FIFO model");
    free(r);
    r = fr_new(true, 100);
    d = drive_transfer(r, RPP_DATA_PACE_MS);
    CHECK(d.ok && r->overruns == 0 && r->min_data_gap_ms >= 12, "the configured pace never overruns");
    free(r);
}

int main(void)
{
    test_crc32();
    test_frames();
    test_deframer();
    test_status();
    test_frame_a();
    test_image_and_slots();
    test_receiver_model();
    test_app_queue();
    test_finish_driver();
    test_convergence();
    test_begin_wait();
    test_fin_unit();
    test_pace();
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
