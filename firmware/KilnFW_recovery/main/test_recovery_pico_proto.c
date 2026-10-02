// test_recovery_pico_proto.c -- host test for recovery_pico_proto.c (the pure
// half of the recovery image's Pico update relay): kilnlink framing with byte
// stuffing and CRC16, the UPDATE_* packers, the streaming deframer, UPDATE_STATUS
// and Frame A parsing, image vector/slot/CRC validation, target-slot resolution,
// the gap-retransmit round logic and the pacing helper.
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

    char t[160];
    rpp_describe_state(RPP_STATE_REFUSED, RPP_ERR_RELAY_CLOSED | RPP_ERR_TRIP_PENDING | RPP_ERR_TOO_HOT,
                       t, sizeof(t));
    CHECK(strstr(t, "refused") && strstr(t, "relay is closed") && strstr(t, "trip is pending") &&
              strstr(t, "temperature"),
          "refusal text names every reason");
    CHECK(strstr(t, "clear") == NULL && strstr(t, "CLEAR") == NULL, "refusal text never offers a trip clear");
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
    CHECK(t.target_slot == RPP_SLOT_B && t.source == RPP_TARGET_ASSUMED_DEFAULT, "default assumes target B");
    CHECK(rpp_image_matches_target(RPP_SLOT_B, t), "B image matches default target");
    CHECK(!rpp_image_matches_target(RPP_SLOT_A, t), "A image refused for target B");
    CHECK(!rpp_image_matches_target(RPP_SLOT_UNKNOWN, t), "unknown image slot never matches");

    CHECK(rpp_chunk_count(0) == 0 && rpp_chunk_count(1) == 1 && rpp_chunk_count(248) == 1 &&
              rpp_chunk_count(249) == 2 && rpp_chunk_count(RPP_SLOT_SIZE) == 3436,
          "chunk count");
    free(a);
    free(b);
}

static void test_gap_and_pace(void)
{
    rpp_gap_tracker_t g;
    rpp_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = RPP_STATE_RECEIVING;
    st.total_chunks = 100;

    rpp_gap_tracker_init(&g);
    st.received_chunks = 90;
    st.gap_count = 10;
    CHECK(rpp_gap_next(&g, &st) == RPP_GAP_RETRANSMIT, "gaps listed -> retransmit");
    st.received_chunks = 100;
    st.gap_count = 0;
    CHECK(rpp_gap_next(&g, &st) == RPP_GAP_SEND_END, "complete -> send END");
    st.received_chunks = 97;
    st.gap_count = 0;
    CHECK(rpp_gap_next(&g, &st) == RPP_GAP_WAIT, "no gaps listed but chunks missing -> wait, not END");
    st.total_chunks = 0;
    st.received_chunks = 0;
    CHECK(rpp_gap_next(&g, &st) != RPP_GAP_SEND_END, "zero total never sends END");

    // Stall: received_chunks never grows.
    rpp_gap_tracker_init(&g);
    st.total_chunks = 100;
    st.received_chunks = 50;
    st.gap_count = 5;
    rpp_gap_action_t a = RPP_GAP_RETRANSMIT;
    uint32_t n = 0;
    while (a != RPP_GAP_FAIL && n < 100) {
        a = rpp_gap_next(&g, &st);
        n++;
    }
    CHECK(a == RPP_GAP_FAIL && n == RPP_MAX_RETRANSMIT_ROUNDS + 1u, "stall fails after the no-progress limit");

    // Progress resets the stall counter.
    rpp_gap_tracker_init(&g);
    st.received_chunks = 10;
    bool failed = false;
    for (int i = 0; i < 100 && !failed; i++) {
        st.received_chunks += 1; // always progressing
        failed = rpp_gap_next(&g, &st) == RPP_GAP_FAIL;
        if (st.received_chunks >= 99) {
            break;
        }
    }
    CHECK(!failed, "steady progress never trips the stall limit");

    // Hard batch cap even with progress.
    rpp_gap_tracker_init(&g);
    st.total_chunks = 100000;
    st.received_chunks = 0;
    st.gap_count = 1;
    a = RPP_GAP_RETRANSMIT;
    for (uint32_t i = 0; i < RPP_MAX_GAP_BATCHES + 2u && a != RPP_GAP_FAIL; i++) {
        st.received_chunks++;
        a = rpp_gap_next(&g, &st);
    }
    CHECK(a == RPP_GAP_FAIL && g.batches == RPP_MAX_GAP_BATCHES + 1u, "hard batch cap");

    // Pacing.
    CHECK(rpp_pace_wait_us(1000, 16000) == 15000u, "pace wait remaining");
    CHECK(rpp_pace_wait_us(16000, 16000) == 0u, "pace deadline reached");
    CHECK(rpp_pace_wait_us(20000, 16000) == 0u, "pace deadline passed");
    CHECK(RPP_DATA_PACE_MS >= 12u && RPP_DATA_PACE_MS <= 30u, "pace constant in the bootloader-safe band");
    CHECK(RPP_ERASE_TIMEOUT_MS >= 120000u, "erase timeout allows the silent erase");
}

int main(void)
{
    test_crc32();
    test_frames();
    test_deframer();
    test_status();
    test_frame_a();
    test_image_and_slots();
    test_gap_and_pace();
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
