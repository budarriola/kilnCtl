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
// recovery_update.c and update_task.c) driven in virtual time. The properties
// that matter, and that the old relay got wrong:
//  - it beacons UPDATE_STATUS only WHILE RECEIVING with chunks still missing,
//    and is SILENT once every chunk is in (until END);
//  - END with chunks missing is answered RECEIVING + the gap list;
//  - END with everything in goes VERIFYING, then COMPLETE (or FAILED on a CRC
//    mismatch) after the CRC check, each reported once;
//  - the bootloader is silent for the whole erase;
//  - a DATA frame arriving sooner than ~12 ms after the previous one overruns
//    the FIFO and is lost.
// ---------------------------------------------------------------------------
#define FR_MAX_CHUNKS 3600
#define FR_QUEUE 64

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
    bool drop_first[FR_MAX_CHUNKS]; // lose this chunk's first transmission
    // state
    int state; // RPP_STATE_*
    bool have_chunk[FR_MAX_CHUNKS];
    uint32_t received;
    int64_t erase_done, verify_done, last_beacon, last_data;
    // observations
    uint32_t begins_seen, begins_while_erasing, ends_seen, overruns, statuses_emitted;
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
}

static uint32_t fr_gaps(const fake_rx_t *r, uint16_t *gaps, bool up_to_total)
{
    // Gaps behind the highest chunk received (beacon) or anywhere (END reply).
    uint32_t hi = 0;
    for (uint32_t i = 0; i < r->total; i++) {
        if (r->have_chunk[i]) {
            hi = i + 1;
        }
    }
    uint32_t lim = up_to_total ? r->total : hi;
    uint32_t n = 0;
    for (uint32_t i = 0; i < lim; i++) {
        if (!r->have_chunk[i]) {
            if (n < RPP_STATUS_MAX_GAPS) {
                gaps[n] = (uint16_t)i;
            }
            n++;
        }
    }
    return n > RPP_STATUS_MAX_GAPS ? RPP_STATUS_MAX_GAPS : n;
}

static void fr_emit(fake_rx_t *r, int state, uint8_t err, bool up_to_total)
{
    uint16_t gaps[RPP_STATUS_MAX_GAPS];
    uint32_t ng = (state == RPP_STATE_RECEIVING) ? fr_gaps(r, gaps, up_to_total) : 0;
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
}

static void fr_tick(fake_rx_t *r)
{
    if (r->state == RPP_STATE_ERASING && g_now_ms >= r->erase_done) {
        r->state = RPP_STATE_RECEIVING;
        r->last_beacon = g_now_ms;
        fr_emit(r, RPP_STATE_RECEIVING, 0, false);
    }
    if (r->state == RPP_STATE_VERIFYING && g_now_ms >= r->verify_done) {
        if (r->crc_good) {
            r->state = RPP_STATE_COMPLETE;
            fr_emit(r, RPP_STATE_COMPLETE, 0, false);
        } else {
            r->state = RPP_STATE_FAILED;
            fr_emit(r, RPP_STATE_FAILED, RPP_ERR_CRC_MISMATCH, false);
        }
    }
    if ((int64_t)(g_now_ms - r->last_beacon) >= (int64_t)r->beacon_ms) {
        r->last_beacon = g_now_ms;
        if (r->state == RPP_STATE_IDLE) {
            fr_emit(r, RPP_STATE_IDLE, 0, false);
        } else if (r->state == RPP_STATE_RECEIVING && r->received < r->total) {
            fr_emit(r, RPP_STATE_RECEIVING, 0, false);
        } else if (r->state == RPP_STATE_ERASING && !r->bootloader) {
            fr_emit(r, RPP_STATE_ERASING, 0, false);
        }
        // complete / verifying / bootloader-erasing: silent
    }
}

static void fr_begin(fake_rx_t *r)
{
    r->begins_seen++;
    if (r->deaf_first_begin && r->begins_seen == 1) {
        return;
    }
    if (r->state == RPP_STATE_ERASING) {
        r->begins_while_erasing++;
    }
    if (r->state == RPP_STATE_IDLE || r->state == RPP_STATE_ERASING) {
        r->state = RPP_STATE_ERASING;
        r->erase_done = g_now_ms + r->erase_ms;
    }
}

static void fr_data(fake_rx_t *r, uint32_t idx)
{
    if (r->state != RPP_STATE_RECEIVING || r->drop_all_data || idx >= r->total) {
        return;
    }
    int64_t gap = g_now_ms - r->last_data;
    r->last_data = g_now_ms;
    if (gap < r->min_data_gap_ms) {
        r->min_data_gap_ms = gap;
    }
    if (gap < 12) {
        r->overruns++; // FIFO overrun: the frame is lost
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

static void fr_end(fake_rx_t *r)
{
    r->ends_seen++;
    if (r->drop_ends > 0) {
        r->drop_ends--;
        return; // the END frame was lost on the wire: the receiver never saw it
    }
    if (r->state != RPP_STATE_RECEIVING) {
        return;
    }
    if (r->received < r->total) {
        fr_emit(r, RPP_STATE_RECEIVING, 0, true); // END too early: gaps again
        return;
    }
    r->state = RPP_STATE_VERIFYING;
    r->verify_done = g_now_ms + r->verify_ms;
    fr_emit(r, RPP_STATE_VERIFYING, 0, false);
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
        if (act == RPP_FIN_FAIL) {
            d.why = f.why;
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
        paced_send(r, i, &next_us, RPP_DATA_PACE_MS, &d);
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
        fake_rx_t *r = calloc(1, sizeof(*r));
        g_now_ms = 0;
        fr_init(r, v == 0, 120);
        CHECK(!legacy_relay_sees_complete_status(r),
              "old relay waited for a no-gap status the silent receiver never sends");
        CHECK(r->received == r->total, "legacy path: every chunk was delivered");
        free(r);
    }
    // END sent too early is answered RECEIVING + gaps.
    fake_rx_t *r = calloc(1, sizeof(*r));
    g_now_ms = 0;
    fr_init(r, true, 50);
    fr_begin(r);
    advance(r, 3000);
    rpp_status_t st;
    while (fr_pop(r, &st)) {
    }
    for (uint32_t i = 0; i < 40; i++) {
        advance(r, 15);
        fr_data(r, i);
    }
    while (fr_pop(r, &st)) {
    }
    fr_end(r);
    CHECK(fr_pop(r, &st) && st.state == RPP_STATE_RECEIVING && st.gap_count == 10 && st.gaps[0] == 40,
          "early END answered RECEIVING plus the missing chunks");
    free(r);
}

static void test_finish_driver(void)
{
    for (int v = 0; v < 2; v++) {
        const char *vn = v == 0 ? "bootloader" : "app";
        fake_rx_t *r = calloc(1, sizeof(*r));
        char nm[160];

        // Clean transfer.
        g_now_ms = 0;
        fr_init(r, v == 0, 200);
        drive_t d = drive_transfer(r, RPP_DATA_PACE_MS);
        snprintf(nm, sizeof(nm), "%s clean transfer completes (%s)", vn, d.why ? d.why : "ok");
        CHECK(d.ok && r->state == RPP_STATE_COMPLETE, nm);
        snprintf(nm, sizeof(nm), "%s clean transfer: exactly one END", vn);
        CHECK(d.end_sends == 1 && r->ends_seen == 1, nm);
        snprintf(nm, sizeof(nm), "%s clean transfer: no FIFO overrun, pace respected", vn);
        CHECK(r->overruns == 0 && r->min_data_gap_ms >= (int64_t)RPP_DATA_PACE_MS, nm);
        snprintf(nm, sizeof(nm), "%s clean transfer: BEGIN sent once", vn);
        CHECK(r->begins_seen == 1, nm);

        // Dropped chunks, including the tail chunk (missing past the beacon's cursor).
        g_now_ms = 0;
        fr_init(r, v == 0, 200);
        for (uint32_t i = 3; i < 200; i += 7) {
            r->drop_first[i] = true;
        }
        r->drop_first[199] = true;
        d = drive_transfer(r, RPP_DATA_PACE_MS);
        snprintf(nm, sizeof(nm), "%s dropped chunks recovered by gap rounds (%s)", vn, d.why ? d.why : "ok");
        CHECK(d.ok && r->state == RPP_STATE_COMPLETE && r->received == r->total, nm);
        snprintf(nm, sizeof(nm), "%s dropped chunks needed retransmit rounds", vn);
        CHECK(d.retransmits >= 1 && d.chunks_sent > 200, nm);

        // Heavy loss: every other chunk dropped on first send.
        g_now_ms = 0;
        fr_init(r, v == 0, 300);
        for (uint32_t i = 0; i < 300; i += 2) {
            r->drop_first[i] = true;
        }
        d = drive_transfer(r, RPP_DATA_PACE_MS);
        snprintf(nm, sizeof(nm), "%s heavy loss still completes (%s)", vn, d.why ? d.why : "ok");
        CHECK(d.ok && r->state == RPP_STATE_COMPLETE, nm);

        // Two END replies lost: END is resent after the reply timeout, still completes.
        g_now_ms = 0;
        fr_init(r, v == 0, 100);
        r->drop_ends = 2;
        d = drive_transfer(r, RPP_DATA_PACE_MS);
        snprintf(nm, sizeof(nm), "%s lost END frames are resent (%s)", vn, d.why ? d.why : "ok");
        CHECK(d.ok && d.end_sends == 3, nm);

        // Every END reply lost: bounded, fails (never loops forever).
        g_now_ms = 0;
        fr_init(r, v == 0, 100);
        r->drop_ends = 100;
        d = drive_transfer(r, RPP_DATA_PACE_MS);
        snprintf(nm, sizeof(nm), "%s endless silence after END fails loudly", vn);
        CHECK(!d.ok && d.end_sends == RPP_MAX_END_SENDS && d.why != NULL, nm);

        // CRC mismatch on the Pico: FAILED is reported as a failure.
        g_now_ms = 0;
        fr_init(r, v == 0, 100);
        r->crc_good = false;
        d = drive_transfer(r, RPP_DATA_PACE_MS);
        snprintf(nm, sizeof(nm), "%s CRC mismatch (FAILED) fails the transfer", vn);
        CHECK(!d.ok && r->state == RPP_STATE_FAILED, nm);

        // Receiver that never accepts DATA: the stall limit ends it.
        g_now_ms = 0;
        fr_init(r, v == 0, 60);
        r->drop_all_data = true;
        d = drive_transfer(r, RPP_DATA_PACE_MS);
        snprintf(nm, sizeof(nm), "%s a dead data path ends in failure, not a hang", vn);
        CHECK(!d.ok && d.why != NULL, nm);

        // Full-size image through the whole flow.
        g_now_ms = 0;
        fr_init(r, v == 0, 3436);
        for (uint32_t i = 5; i < 3436; i += 97) {
            r->drop_first[i] = true;
        }
        d = drive_transfer(r, RPP_DATA_PACE_MS);
        snprintf(nm, sizeof(nm), "%s full 3436-chunk image completes (%s)", vn, d.why ? d.why : "ok");
        CHECK(d.ok && r->state == RPP_STATE_COMPLETE && r->overruns == 0, nm);
        free(r);
    }
}

static void test_begin_wait(void)
{
    // Slow erase: a bootloader is silent for 100 s, then RECEIVING. BEGIN is
    // sent once and never restarted.
    fake_rx_t *r = calloc(1, sizeof(*r));
    g_now_ms = 0;
    fr_init(r, true, 40);
    r->erase_ms = 100000;
    drive_t d = drive_transfer(r, RPP_DATA_PACE_MS);
    CHECK(d.ok && r->begins_seen == 1 && r->begins_while_erasing == 0,
          "100 s silent erase completes without a BEGIN resend");

    // Erase longer than the window fails (bounded).
    g_now_ms = 0;
    fr_init(r, true, 40);
    r->erase_ms = RPP_ERASE_TIMEOUT_MS + 30000u;
    d = drive_transfer(r, RPP_DATA_PACE_MS);
    CHECK(!d.ok && r->begins_seen == 1, "an erase past the window fails without restarting it");

    // Application variant reports ERASING beacons: also never restarted.
    g_now_ms = 0;
    fr_init(r, false, 40);
    r->erase_ms = 60000;
    d = drive_transfer(r, RPP_DATA_PACE_MS);
    CHECK(d.ok && r->begins_while_erasing == 0, "app erase beacons never trigger a resend");

    // A BEGIN the Pico never saw is resent, but only after several IDLE beacons
    // AND the minimum time.
    g_now_ms = 0;
    fr_init(r, true, 40);
    r->deaf_first_begin = true;
    d = drive_transfer(r, RPP_DATA_PACE_MS);
    CHECK(d.ok && r->begins_seen == 2, "a missed BEGIN is resent once and the transfer completes");
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
    CHECK(rpp_fin_step(&f, RPP_EV_STATUS, &st, 200) == RPP_FIN_WAIT && f.restart_timer,
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

    int fails[] = {RPP_STATE_FAILED, RPP_STATE_ABORTED, RPP_STATE_REFUSED, RPP_STATE_REJECTED_SLOT_LINKAGE,
                   RPP_STATE_REFUSED_RUNNING_IMAGE_OVERLAP, RPP_STATE_IDLE};
    for (size_t i = 0; i < sizeof(fails) / sizeof(fails[0]); i++) {
        rpp_fin_start(&f);
        st.state = (uint8_t)fails[i];
        CHECK(rpp_fin_step(&f, RPP_EV_STATUS, &st, 0) == RPP_FIN_FAIL && f.why != NULL,
              "terminal or lost-transfer state fails");
    }

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

    // END reply timeout: resend, then fail after the allowed sends.
    rpp_fin_start(&f);
    rpp_fin_end_sent(&f);
    CHECK(rpp_fin_step(&f, RPP_EV_QUIET, NULL, RPP_END_REPLY_TIMEOUT_MS - 1) == RPP_FIN_WAIT, "before the END reply timeout");
    CHECK(rpp_fin_step(&f, RPP_EV_QUIET, NULL, RPP_END_REPLY_TIMEOUT_MS) == RPP_FIN_SEND_END, "END resent after the reply timeout");
    rpp_fin_end_sent(&f);
    rpp_fin_end_sent(&f);
    CHECK(rpp_fin_step(&f, RPP_EV_QUIET, NULL, RPP_END_REPLY_TIMEOUT_MS) == RPP_FIN_FAIL, "END sends exhausted -> fail");
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
    fake_rx_t *r = calloc(1, sizeof(*r));
    g_now_ms = 0;
    fr_init(r, true, 100);
    drive_t d = drive_transfer(r, 4); // 4 ms pace: too fast
    CHECK(r->overruns > 0, "a 4 ms pace overruns the bootloader FIFO model");
    g_now_ms = 0;
    fr_init(r, true, 100);
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
    test_finish_driver();
    test_begin_wait();
    test_fin_unit();
    test_pace();
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
