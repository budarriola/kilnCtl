// Golden byte-layout tests for the five PC-facing safety_link_build_*_payload()
// builders (safety_link_payload.c) and the safety_{read,put}_*_le helpers
// (safety_link_frame.c). docs/audits/HOST_TEST_COVERAGE_GAPS_2026-10-09.md
// campaign 5. #included by test_safety_link_compile.c AFTER the real
// safety_link*.c files, so the builders and make_link() are visible.
//
// Expected bytes are assembled by an INDEPENDENT local writer (gl_*), never by
// the production safety_put_*_le helpers, so a swapped offset or a wrong
// endianness in the production code cannot cancel itself out. The layouts are
// the ones documented in uart_task_ids.h (GET_DIAG / GET_TRIP_EVENT) and
// safety_link.h (STATUS/STATS lengths); the FW_VERSION builder is additionally
// round-tripped through safety_parse_fw_version(), the Pico's Frame C parser
// on the ESP side.
#ifndef TEST_SAFETY_LINK_PAYLOAD_GOLDEN_H
#define TEST_SAFETY_LINK_PAYLOAD_GOLDEN_H

static void gl_u16(uint8_t *o, uint16_t v) { o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); }
static void gl_u32(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 24);
}
static void gl_f32(uint8_t *o, float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    gl_u32(o, b);
}

static SafetyLinkClass gl_make(void)
{
    SafetyLinkClass l = make_link();
    l.initialized = true;
    return l;
}

static void test_golden_endian_helpers(void)
{
    TEST_SECTION("safety_put_*_le/safety_read_*_le -- byte order pinned with an independent writer");
    uint8_t b[4];
    safety_put_u16_le(b, 0x1234u);
    TEST_CHECK(b[0] == 0x34 && b[1] == 0x12, "u16 put is little-endian");
    TEST_CHECK(safety_read_u16_le(b) == 0x1234u, "u16 read is little-endian");
    safety_put_u16_le(b, 0xFFFFu);
    TEST_CHECK(b[0] == 0xFF && b[1] == 0xFF && safety_read_u16_le(b) == 0xFFFFu, "u16 max");
    safety_put_u32_le(b, 0x01020304u);
    TEST_CHECK(b[0] == 4 && b[1] == 3 && b[2] == 2 && b[3] == 1, "u32 put is little-endian");
    TEST_CHECK(safety_read_u32_le(b) == 0x01020304u, "u32 read is little-endian");
    safety_put_u32_le(b, 0xFFFFFFFFu);
    TEST_CHECK(safety_read_u32_le(b) == 0xFFFFFFFFu, "u32 max");
    safety_put_u32_le(b, 0u);
    TEST_CHECK(safety_read_u32_le(b) == 0u, "u32 zero");
    safety_put_f32_le(b, 1.0f); // 0x3F800000 -> 00 00 80 3F
    TEST_CHECK(b[0] == 0 && b[1] == 0 && b[2] == 0x80 && b[3] == 0x3F, "f32 1.0 bytes");
    TEST_CHECK(safety_read_f32_le(b) == 1.0f, "f32 read 1.0");
    safety_put_f32_le(b, INFINITY);
    TEST_CHECK(b[3] == 0x7F && b[2] == 0x80 && b[1] == 0 && b[0] == 0, "f32 +inf bytes");
    TEST_CHECK(isinf(safety_read_f32_le(b)), "f32 +inf round-trips");
    safety_put_f32_le(b, -INFINITY);
    TEST_CHECK(b[3] == 0xFF && isinf(safety_read_f32_le(b)) && safety_read_f32_le(b) < 0, "f32 -inf");
    safety_put_f32_le(b, NAN);
    TEST_CHECK(isnan(safety_read_f32_le(b)), "f32 NaN round-trips as NaN");
    uint8_t u[9] = {0};
    safety_put_u32_le(&u[1], 0xAABBCCDDu);
    TEST_CHECK(u[0] == 0 && u[1] == 0xDD && u[4] == 0xAA && u[5] == 0, "unaligned u32 write stays in 4 bytes");
    TEST_CHECK(safety_read_u32_le(&u[1]) == 0xAABBCCDDu, "unaligned u32 read");
}

static void test_golden_status_payload(void)
{
    TEST_SECTION("safety_link_build_status_payload -- 27-byte layout, every offset");
    SafetyLinkClass l = gl_make();
    l.cached.flags = (uint8_t)(SAFETY_FLAG_ESTOP | SAFETY_FLAG_RELAY | SAFETY_FLAG_ENABLED |
                               SAFETY_FLAG_TEMP_VALID | SAFETY_FLAG_TC_NOT_INSTALLED);
    l.cached.tc_temp_c = 1234.5f;
    l.cached.cj_temp_c = -12.25f;
    l.cached.tc_fault = 0xA5;
    l.cached.current_a[0] = 1.5f;
    l.cached.current_a[1] = 2.5f;
    l.cached.current_a[2] = 3.5f;
    l.cached.tx_dropped_sat = 0x77;
    l.cached.tx_dropped_known = true;
    uint8_t out[SAFETY_LINK_STATUS_PAYLOAD_LEN + 8];
    memset(out, 0xEE, sizeof(out));
    size_t n = safety_link_build_status_payload(&l, out);
    TEST_CHECK(n == 27u && n == SAFETY_LINK_STATUS_PAYLOAD_LEN, "length is 27");
    TEST_CHECK(out[27] == 0xEE, "no write past the 27-byte payload");
    uint8_t e[27];
    memset(e, 0, sizeof(e));
    e[0] = SAFETY_CMD_GET_STATUS;
    e[1] = (uint8_t)(l.cached.flags | (safety_link_up_locked(&l) ? SAFETY_FLAG_LINK_UP : 0u));
    gl_f32(&e[2], 1234.5f);
    gl_f32(&e[6], -12.25f);
    e[10] = 0xA5;
    gl_f32(&e[11], 1.5f);
    gl_f32(&e[15], 2.5f);
    gl_f32(&e[19], 3.5f);
    gl_u16(&e[23], (uint16_t)safety_age_ms_locked(&l));
    e[25] = 0x77;
    e[26] = 0x01;
    TEST_CHECK(memcmp(out, e, 27) == 0, "status payload matches golden bytes");
    TEST_CHECK(out[0] == SAFETY_CMD_GET_STATUS, "cmd byte is GET_STATUS");

    l.cached.flags = 0;
    l.fault_sources = 0x4u;
    safety_link_build_status_payload(&l, out);
    TEST_CHECK((out[1] & SAFETY_FLAG_FAULT) != 0, "fault_sources -> FAULT bit");
    l.cached.tx_dropped_known = false;
    safety_link_build_status_payload(&l, out);
    TEST_CHECK(out[26] == 0, "tx_dropped_known false -> byte26 == 0");

    l.cached.tc_temp_c = NAN;
    l.cached.cj_temp_c = INFINITY;
    l.cached.current_a[0] = -INFINITY;
    l.cached.current_a[1] = 3.4028235e38f;
    l.cached.current_a[2] = 1e-45f;
    safety_link_build_status_payload(&l, out);
    TEST_CHECK(isnan(safety_read_f32_le(&out[2])), "NaN tc survives");
    TEST_CHECK(isinf(safety_read_f32_le(&out[6])) && safety_read_f32_le(&out[6]) > 0, "+inf cj survives");
    TEST_CHECK(isinf(safety_read_f32_le(&out[11])) && safety_read_f32_le(&out[11]) < 0, "-inf current survives");
    TEST_CHECK(safety_read_f32_le(&out[15]) == 3.4028235e38f, "FLT_MAX survives");
    TEST_CHECK(safety_read_u32_le(&out[19]) == 1u, "denormal bit pattern survives");

    uint8_t out2[SAFETY_LINK_STATUS_PAYLOAD_LEN];
    TEST_CHECK(safety_link_build_status_payload(NULL, out2) == 0, "NULL link -> 0");
    TEST_CHECK(safety_link_build_status_payload(&l, NULL) == 0, "NULL out -> 0");
    SafetyLinkClass u = make_link(); // not initialized
    TEST_CHECK(safety_link_build_status_payload(&u, out2) == 0, "uninitialized link -> 0");
}

static void test_golden_stats_payload(void)
{
    TEST_SECTION("safety_link_build_stats_payload -- 96-byte layout, every offset");
    SafetyLinkClass l = gl_make();
    safety_link_stats_t *s = &l.stats;
    l.poll_period_ms = 0xBEEF;
    s->frames_sent = 0x01010101u;     s->frames_received = 0x02020202u;
    s->timeouts = 0x04040404u;
    s->diag_applied = 0x06060606u;    s->power_applied = 0x07070707u;
    s->dequeued_total = 0x0A0A0A0Au;  s->unmatched_cmd_count = 0x0B0B0B0Bu;
    s->last_unmatched_cmd_byte = 0xCC;
    s->cmd_status_count = 0x0D0D0D0Du;        s->cmd_fw_version_count = 0x0E0E0E0Eu;
    s->cmd_update_status_count = 0x0F0F0F0Fu; s->cmd_power_count = 0x10101010u;
    s->cmd_diag_count = 0x11111111u;          s->cmd_trip_event_count = 0x12121212u;
    s->cmd_ct_cal_count = 0x13131313u;        s->cmd_config_page_count = 0x14141414u;
    s->cmd_commit_config_rejected_count = 0xFFFFFFFFu;
    uint8_t out[SAFETY_LINK_STATS_PAYLOAD_LEN + 8];
    memset(out, 0xEE, sizeof(out));
    size_t n = safety_link_build_stats_payload(&l, out);
    TEST_CHECK(n == 96u && n == SAFETY_LINK_STATS_PAYLOAD_LEN, "length is 96");
    TEST_CHECK(out[96] == 0xEE, "no write past the 96-byte payload");
    TEST_CHECK(out[0] == SAFETY_CMD_GET_LINK_STATS, "cmd byte");
    TEST_CHECK(safety_read_u32_le(&out[1]) == 0x01010101u, "frames_sent @1");
    TEST_CHECK(safety_read_u32_le(&out[5]) == 0x02020202u, "frames_received @5");
    TEST_CHECK(safety_read_u32_le(&out[13]) == 0x04040404u, "timeouts @13");
    TEST_CHECK(out[17] == 0xEF && out[18] == 0xBE, "poll_period_ms @17 LE");
    TEST_CHECK(safety_read_u32_le(&out[23]) == 0x06060606u, "diag_applied @23");
    TEST_CHECK(safety_read_u32_le(&out[27]) == 0x07070707u, "power_applied @27");
    TEST_CHECK(safety_read_u32_le(&out[51]) == 0x0A0A0A0Au, "dequeued_total @51");
    TEST_CHECK(safety_read_u32_le(&out[55]) == 0x0B0B0B0Bu, "unmatched_cmd_count @55");
    TEST_CHECK(out[59] == 0xCC, "last_unmatched_cmd_byte @59");
    TEST_CHECK(safety_read_u32_le(&out[60]) == 0x0D0D0D0Du, "cmd_status_count @60");
    TEST_CHECK(safety_read_u32_le(&out[64]) == 0x0E0E0E0Eu, "cmd_fw_version_count @64");
    TEST_CHECK(safety_read_u32_le(&out[68]) == 0x0F0F0F0Fu, "cmd_update_status_count @68");
    TEST_CHECK(safety_read_u32_le(&out[72]) == 0x10101010u, "cmd_power_count @72");
    TEST_CHECK(safety_read_u32_le(&out[76]) == 0x11111111u, "cmd_diag_count @76");
    TEST_CHECK(safety_read_u32_le(&out[80]) == 0x12121212u, "cmd_trip_event_count @80");
    TEST_CHECK(safety_read_u32_le(&out[84]) == 0x13131313u, "cmd_ct_cal_count @84");
    TEST_CHECK(safety_read_u32_le(&out[88]) == 0x14141414u, "cmd_config_page_count @88");
    TEST_CHECK(out[92] == 0xFF && out[93] == 0xFF && out[94] == 0xFF && out[95] == 0xFF,
               "cmd_commit_config_rejected_count @92 (u32 max, last 4 bytes)");
    // Fields filled from the uart_protocol / uart_owner fakes are read back via the
    // accessor, so the test pins placement, not the fake's values.
    safety_link_stats_t got;
    TEST_CHECK(safety_link_get_stats(&l, &got) == ESP_OK, "get_stats ok");
    TEST_CHECK(safety_read_u32_le(&out[9]) == got.frame_errors, "frame_errors @9");
    TEST_CHECK(safety_read_u32_le(&out[19]) == got.broadcast_dropped, "broadcast_dropped @19");
    TEST_CHECK(safety_read_u32_le(&out[31]) == got.frames_deframed, "frames_deframed @31");
    TEST_CHECK(safety_read_u32_le(&out[35]) == got.frames_routed_nowhere, "frames_routed_nowhere @35");
    TEST_CHECK(safety_read_u32_le(&out[39]) == got.frame_length_mismatch, "frame_length_mismatch @39");
    TEST_CHECK(safety_read_u32_le(&out[43]) == got.frame_crc_mismatch, "frame_crc_mismatch @43");
    TEST_CHECK(safety_read_u32_le(&out[47]) == got.frame_resync, "frame_resync @47");
    TEST_CHECK(safety_link_build_stats_payload(NULL, out) == 0, "NULL link -> 0");
    TEST_CHECK(safety_link_build_stats_payload(&l, NULL) == 0, "NULL out -> 0");
}

static void test_golden_diag_payload(void)
{
    TEST_SECTION("safety_link_build_diag_payload -- 31-byte layout vs uart_task_ids.h");
    SafetyLinkClass l = gl_make();
    l.cached.diag_ever_received = true;
    l.cached.diag_trip_reason = 0x0D;
    l.cached.diag_warn_mask = 0x1234;
    l.cached.diag_trip_mask = 0xFFFF;
    l.cached.diag_uptime_ms = 0xFFFFFFFEu;
    l.cached.diag_boot_reason = 0x03;
    l.cached.diag_context_age_100ms = 255;
    l.cached.diag_context_frames_ok = 0x11223344u;
    l.cached.diag_context_frames_bad = 0x55667788u;
    l.cached.diag_tx_frames_dropped = 0x99AABBCCu;
    l.cached.diag_state = 2;
    l.cached.diag_flags = 0x81;
    l.cached.diag_log_frames_dropped = 0xDEADBEEFu;
    uint8_t out[SAFETY_LINK_DIAG_PAYLOAD_LEN + 8];
    memset(out, 0xEE, sizeof(out));
    size_t n = safety_link_build_diag_payload(&l, out);
    TEST_CHECK(n == 31u, "length is 31");
    TEST_CHECK(out[31] == 0xEE, "no write past 31 bytes");
    uint8_t e[31];
    memset(e, 0, sizeof(e));
    e[0] = SAFETY_CMD_GET_DIAG; e[1] = 1; e[2] = 0x0D;
    gl_u16(&e[3], 0x1234); gl_u16(&e[5], 0xFFFF); gl_u32(&e[7], 0xFFFFFFFEu);
    e[11] = 3; e[12] = 255;
    gl_u32(&e[13], 0x11223344u); gl_u32(&e[17], 0x55667788u); gl_u32(&e[21], 0x99AABBCCu);
    e[25] = 2; e[26] = 0x81; gl_u32(&e[27], 0xDEADBEEFu);
    TEST_CHECK(memcmp(out, e, 31) == 0, "diag payload matches golden bytes");
    l.cached.diag_ever_received = false;
    safety_link_build_diag_payload(&l, out);
    TEST_CHECK(out[1] == 0, "ever_received false -> byte1 == 0");
    TEST_CHECK(safety_link_build_diag_payload(NULL, out) == 0, "NULL link -> 0");
    TEST_CHECK(safety_link_build_diag_payload(&l, NULL) == 0, "NULL out -> 0");
}

static void test_golden_trip_event_payload(void)
{
    TEST_SECTION("safety_link_build_trip_event_payload -- 34-byte layout vs uart_task_ids.h");
    SafetyLinkClass l = gl_make();
    l.cached.trip_event_ever_received = true;
    l.cached.trip_last_seq = 0xFE;
    l.cached.trip_reason = 0x06;
    l.cached.trip_uptime_ms = 0x0A0B0C0Du;
    l.cached.trip_safety_tc_c = 1300.75f;
    l.cached.trip_deciding_threshold = NAN;
    l.cached.trip_current_a[0] = INFINITY;
    l.cached.trip_current_a[1] = -0.0f;
    l.cached.trip_current_a[2] = 12.5f;
    l.cached.trip_relay_recent_mask = 0x0F;
    l.cached.trip_context_age_100ms = 0xFF;
    uint8_t out[SAFETY_LINK_TRIP_EVENT_PAYLOAD_LEN + 8];
    memset(out, 0xEE, sizeof(out));
    size_t n = safety_link_build_trip_event_payload(&l, out);
    TEST_CHECK(n == 34u && n == SAFETY_LINK_TRIP_EVENT_PAYLOAD_LEN, "length is 34");
    TEST_CHECK(out[34] == 0xEE, "no write past 34 bytes");
    safety_link_status_t st;
    TEST_CHECK(safety_link_get_status(&l, &st) == ESP_OK, "status read");
    uint8_t e[34];
    memset(e, 0, sizeof(e));
    e[0] = SAFETY_CMD_GET_TRIP_EVENT; e[1] = 1; e[2] = 0xFE; e[3] = 6;
    gl_u32(&e[4], 0x0A0B0C0Du);
    gl_f32(&e[8], 1300.75f);
    memcpy(&e[12], &l.cached.trip_deciding_threshold, 4); // NaN: compare raw bits
    gl_f32(&e[16], INFINITY);
    gl_f32(&e[20], -0.0f);
    gl_f32(&e[24], 12.5f);
    e[28] = 0x0F; e[29] = 0xFF;
    gl_u32(&e[30], st.trip_event_age_ms);
    TEST_CHECK(memcmp(out, e, 34) == 0, "trip event payload matches golden bytes");
    TEST_CHECK(out[23] == 0x80, "-0.0f sign bit preserved");
    l.cached.trip_event_ever_received = false;
    safety_link_build_trip_event_payload(&l, out);
    TEST_CHECK(out[1] == 0, "ever_received false -> byte1 == 0");
    TEST_CHECK(safety_read_u32_le(&out[30]) == 0u, "event age is 0 when never received");
    TEST_CHECK(safety_link_build_trip_event_payload(NULL, out) == 0, "NULL link -> 0");
    TEST_CHECK(safety_link_build_trip_event_payload(&l, NULL) == 0, "NULL out -> 0");
}

static void gl_set_peer(SafetyLinkClass *l, bool known, uint16_t proto, uint16_t minc, bool build_known,
                        bool dirty, const char *commit, size_t commit_len, const char *dt, size_t dt_len,
                        uint8_t cfg_ver, uint16_t cfg_crc, bool boot_known, uint8_t boot_id)
{
    l->peer_version_known = known;
    l->peer_version_compatible = known;
    l->peer_protocol_version = proto;
    l->peer_min_compatible = minc;
    l->peer_build_known = build_known;
    l->peer_build_dirty = dirty;
    memset(l->peer_build_commit, 0, sizeof(l->peer_build_commit));
    memset(l->peer_build_datetime, 0, sizeof(l->peer_build_datetime));
    if (commit_len > sizeof(l->peer_build_commit)) commit_len = sizeof(l->peer_build_commit);
    if (dt_len > sizeof(l->peer_build_datetime)) dt_len = sizeof(l->peer_build_datetime);
    memcpy(l->peer_build_commit, commit, commit_len);
    memcpy(l->peer_build_datetime, dt, dt_len);
    l->peer_build_commit_len = (uint8_t)commit_len;
    l->peer_build_datetime_len = (uint8_t)dt_len;
    l->peer_config_version = cfg_ver;
    l->peer_config_crc = cfg_crc;
    l->pico_boot_id_known = boot_known;
    l->pico_boot_id = boot_id;
}

static void test_golden_fw_version_payload(void)
{
    TEST_SECTION("safety_link_build_fw_version_payload -- variable layout, golden + parser round trip");
    SafetyLinkClass l = gl_make();
    uint8_t out[256];

    // Unknown: well-formed shape, everything zero except the independently-known boot id.
    memset(out, 0xEE, sizeof(out));
    gl_set_peer(&l, false, 0x1111, 0x2222, false, true, "zz", 2, "yy", 2, 9, 0x9999, true, 0x42);
    size_t n = safety_link_build_fw_version_payload(&l, out);
    {
        // cmd, proto(2), min(2), dirty, commit_len, dt_len, boot_id, cfg_ver, crc(2)
        uint8_t e[] = {SAFETY_CMD_FW_VERSION, 0, 0, 0, 0, 0, 0, 0, 0x42, 0, 0, 0};
        TEST_CHECK(n == 12u, "unknown shape is 12 bytes");
        TEST_CHECK(n == 12u && memcmp(out, e, 12) == 0,
                   "unknown shape: protocol/dirty/strings/cfg all zero, boot_id kept");
    }

    // Known, short strings: exact golden bytes.
    gl_set_peer(&l, true, 0x0102, 0x0304, true, true, "abcdef0", 7, "12:00", 5, 3, 0xBEEF, true, 0xA7);
    memset(out, 0xEE, sizeof(out));
    n = safety_link_build_fw_version_payload(&l, out);
    {
        uint8_t e[64];
        size_t i = 0;
        e[i++] = SAFETY_CMD_FW_VERSION;
        e[i++] = 0x02; e[i++] = 0x01; e[i++] = 0x04; e[i++] = 0x03;
        e[i++] = 1; e[i++] = 7;
        memcpy(&e[i], "abcdef0", 7); i += 7;
        e[i++] = 5;
        memcpy(&e[i], "12:00", 5); i += 5;
        e[i++] = 0xA7; e[i++] = 3; e[i++] = 0xEF; e[i++] = 0xBE;
        TEST_CHECK(n == i, "known short length");
        TEST_CHECK(n == i && memcmp(out, e, i) == 0, "known short matches golden bytes");
        TEST_CHECK(out[n] == 0xEE, "no write past the payload");
    }

    // Round trip through the production parser.
    {
        uint16_t pr = 0, mc = 0, crc = 0;
        uint8_t bid = 0, cv = 0, cl = 0, dl = 0;
        bool hb = false, dirty = false, hbuild = false;
        uint8_t cm[64], dt[32];
        bool ok = safety_parse_fw_version(out, (uint8_t)n, &pr, &mc, &bid, &hb, &dirty, cm, &cl, dt, &dl,
                                          &cv, &crc, &hbuild);
        TEST_CHECK(ok && pr == 0x0102 && mc == 0x0304, "parser: versions");
        TEST_CHECK(hb && bid == 0xA7 && hbuild && cv == 3 && crc == 0xBEEF && dirty, "parser: tail fields");
        TEST_CHECK(cl == 7 && memcmp(cm, "abcdef0", 7) == 0 && dl == 5 && memcmp(dt, "12:00", 5) == 0,
                   "parser: strings");
    }

    // Max-length commit (64) and datetime (32): 12 fixed + 64 + 32 = 108 bytes (builder comment says 105: miscount).
    {
        char c64[64], d32[32];
        memset(c64, 'C', 64); memset(d32, 'D', 32);
        gl_set_peer(&l, true, 1, 1, true, false, c64, 64, d32, 32, 1, 1, true, 1);
        memset(out, 0xEE, sizeof(out));
        n = safety_link_build_fw_version_payload(&l, out);
        TEST_CHECK(n == 108u, "max-length payload is 108 bytes");
        TEST_CHECK(out[6] == 64 && out[7 + 64] == 32, "length prefixes 64/32");
        TEST_CHECK(out[108] == 0xEE, "no write past 108 bytes");
        TEST_CHECK(out[7 + 64 + 1 + 32 - 1] == 'D', "last datetime byte");
    }

    // Corrupt cached lengths (over the buffers) are clamped, never overrun.
    {
        gl_set_peer(&l, true, 1, 1, true, false, "x", 1, "y", 1, 1, 1, true, 1);
        l.peer_build_commit_len = 200;
        l.peer_build_datetime_len = 255;
        memset(out, 0xEE, sizeof(out));
        n = safety_link_build_fw_version_payload(&l, out);
        TEST_CHECK(n == 108u, "over-long cached lengths clamp to 64/32");
        TEST_CHECK(out[6] == 64 && out[7 + 64] == 32, "clamped prefixes");
    }

    // Boot id not known -> 0, not a stale value.
    gl_set_peer(&l, true, 1, 1, true, false, "x", 1, "y", 1, 1, 1, false, 0x99);
    n = safety_link_build_fw_version_payload(&l, out);
    TEST_CHECK(out[n - 4] == 0, "unknown boot_id reported as 0");

    TEST_CHECK(safety_link_build_fw_version_payload(&l, NULL) == 0, "NULL out -> 0");
    TEST_CHECK(safety_link_build_fw_version_payload(NULL, out) == 0, "NULL link -> 0");
}

static void run_golden_payload_tests(void)
{
    test_golden_endian_helpers();
    test_golden_status_payload();
    test_golden_stats_payload();
    test_golden_diag_payload();
    test_golden_trip_event_payload();
    test_golden_fw_version_payload();
}

#endif
