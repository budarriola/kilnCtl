// Host test pinning SimFW's command-PAYLOAD wire layout (docs/PROTOCOL.md
// sections 4/5) against firmware/SimFW/test/vectors/cmd_payload_vectors.json
// -- the shared manifest that closes the gap benchproto_frame_vectors.json's
// own convention left open: that manifest only proves the frame *envelope*
// byte-identical between kilnsim's Python codec and benchproto_frame.c.
// Every COMMAND PAYLOAD layout *inside* that envelope was, until this
// manifest existed, duplicated by hand in cmd_task.c and in
// tools/PcTools/src/kilnsim/payloads.py with nothing cross-checking them
// (tools/PcTools/tests/test_kilnsim_cmd_payload_vectors.py is the other
// independent consumer of the same manifest).
//
// Why this file mirrors rather than calls the real code: cmd_task.c is a
// FreeRTOS task file (includes FreeRTOS.h/task.h, calls sim_engine.h/
// fault_sched.h/i2c_owner.h/wave_owner.h/usb_owner.h task APIs), so it is
// not part of this host-test harness's source list -- same pure/task
// boundary test_cmd_task_gap_closure.c's own header comment already
// documents, and the same reason there is no test_cmd_task.c. The
// arg_reader_t/reply_writer_t types and rw_*/ar_* helper functions below are
// a byte-for-byte copy of cmd_task.c's own (same file, same names, same
// semantics) -- keep the two in sync by hand if either changes. Each
// covered command's decode/encode below mirrors the exact ar_*/rw_* call
// sequence its real handle_*() function in cmd_task.c uses, with literal
// values standing in for what a real owner-API call would have returned.
//
// The hardcoded byte arrays below are transcribed by hand from
// firmware/SimFW/test/vectors/cmd_payload_vectors.json (same convention
// test_benchproto_frame.c already uses for benchproto_frame_vectors.json --
// this host-test harness has no JSON parser, so the manifest's role here is
// "the vectors were derived independently of this file," not "this file
// reads the manifest at runtime"). If the manifest and this file ever
// disagree, the manifest is authoritative -- update this file's arrays to
// match it, not the other way around.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

// --- Mirrors of cmd_task.c's reply_writer_t / rw_* -------------------------
typedef struct {
    uint8_t *buf;
    uint8_t cap;
    uint8_t len;
    bool overflow;
} reply_writer_t;

static void rw_init(reply_writer_t *w, uint8_t *buf, uint8_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
}

static void rw_bytes(reply_writer_t *w, const uint8_t *data, uint8_t n)
{
    if (w->overflow || (uint16_t)w->len + (uint16_t)n > (uint16_t)w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, data, n);
    w->len = (uint8_t)(w->len + n);
}

static void rw_u8(reply_writer_t *w, uint8_t v) { rw_bytes(w, &v, 1); }

static void rw_u16le(reply_writer_t *w, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu)};
    rw_bytes(w, b, 2);
}

static void rw_u32le(reply_writer_t *w, uint32_t v)
{
    uint8_t b[4] = {
        (uint8_t)(v & 0xFFu),
        (uint8_t)((v >> 8) & 0xFFu),
        (uint8_t)((v >> 16) & 0xFFu),
        (uint8_t)((v >> 24) & 0xFFu),
    };
    rw_bytes(w, b, 4);
}

static void rw_u64le(reply_writer_t *w, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = (uint8_t)((v >> (8 * i)) & 0xFFu);
    }
    rw_bytes(w, b, 8);
}

static void rw_f32le(reply_writer_t *w, float v)
{
    union {
        float f;
        uint32_t u;
    } conv;
    conv.f = v;
    rw_u32le(w, conv.u);
}

// --- Mirrors of cmd_task.c's arg_reader_t / ar_* ---------------------------
typedef struct {
    const uint8_t *buf;
    uint8_t len;
    uint8_t pos;
    bool overflow;
} arg_reader_t;

static void ar_init(arg_reader_t *r, const uint8_t *buf, uint8_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    r->overflow = false;
}

static bool ar_bytes(arg_reader_t *r, uint8_t *out, uint8_t n)
{
    if (r->overflow || (uint16_t)r->pos + (uint16_t)n > (uint16_t)r->len) {
        r->overflow = true;
        if (out != NULL) memset(out, 0, n);
        return false;
    }
    memcpy(out, r->buf + r->pos, n);
    r->pos = (uint8_t)(r->pos + n);
    return true;
}

static uint8_t ar_u8(arg_reader_t *r)
{
    uint8_t v = 0;
    ar_bytes(r, &v, 1);
    return v;
}

static uint16_t ar_u16le(arg_reader_t *r)
{
    uint8_t b[2] = {0, 0};
    ar_bytes(r, b, 2);
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t ar_u32le(arg_reader_t *r)
{
    uint8_t b[4] = {0, 0, 0, 0};
    ar_bytes(r, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t ar_u64le(arg_reader_t *r)
{
    uint8_t b[8] = {0};
    ar_bytes(r, b, 8);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}

static float ar_f32le(arg_reader_t *r)
{
    union {
        float f;
        uint32_t u;
    } conv;
    conv.u = ar_u32le(r);
    return conv.f;
}

static bool bytes_eq(const uint8_t *a, const uint8_t *b, size_t n) { return memcmp(a, b, n) == 0; }

// ===========================================================================
// SYS group -- vectors/cmd_payload_vectors.json "sys" section
// ===========================================================================

static void test_sys_requests(void)
{
    TEST_SECTION("cmd payload vectors -- SYS requests");

    { // ping (no args)
        static const uint8_t wire[] = {0x01};
        TEST_CHECK(wire[0] == 1u, "sys/ping: cmd_id byte is 1");
    }
    { // reset_sim_keep_true: {0x03, 0x01}
        static const uint8_t wire[] = {0x03, 0x01};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint8_t keep_params = ar_u8(&r);
        TEST_CHECK(!r.overflow, "sys/reset_sim_keep_true: no overflow");
        TEST_CHECK(keep_params == 1u, "sys/reset_sim_keep_true: keep_params decodes to 1");
    }
    { // reset_sim_keep_false: {0x03, 0x00}
        static const uint8_t wire[] = {0x03, 0x00};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint8_t keep_params = ar_u8(&r);
        TEST_CHECK(keep_params == 0u, "sys/reset_sim_keep_false: keep_params decodes to 0");
    }
    { // set_timescale_10x: {0x04, 0xE8,0x03,0x00,0x00} -> 1000
        static const uint8_t wire[] = {0x04, 0xE8, 0x03, 0x00, 0x00};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint32_t timescale_x100 = ar_u32le(&r);
        TEST_CHECK(!r.overflow, "sys/set_timescale_10x: no overflow");
        TEST_CHECK(timescale_x100 == 1000u, "sys/set_timescale_10x: decodes to 1000 (LE, not BE)");
    }
    { // set_timescale_zero
        static const uint8_t wire[] = {0x04, 0x00, 0x00, 0x00, 0x00};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        TEST_CHECK(ar_u32le(&r) == 0u, "sys/set_timescale_zero: decodes to 0");
    }
    { // set_timescale_max_u32
        static const uint8_t wire[] = {0x04, 0xFF, 0xFF, 0xFF, 0xFF};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        TEST_CHECK(ar_u32le(&r) == 0xFFFFFFFFu, "sys/set_timescale_max_u32: decodes to UINT32_MAX");
    }
    { // set_seed_typical: {0x05, 0x78,0x56,0x34,0x12} -> 0x12345678
        static const uint8_t wire[] = {0x05, 0x78, 0x56, 0x34, 0x12};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint32_t seed = ar_u32le(&r);
        TEST_CHECK(seed == 0x12345678u, "sys/set_seed_typical: LE decode gives 0x12345678, not 0x78563412");
    }
    { // reboot_bootloader_correct_magic: {0x08, 0x07,0xB0,0x07,0xB0} -> 0xB007B007
        static const uint8_t wire[] = {0x08, 0x07, 0xB0, 0x07, 0xB0};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint32_t confirm = ar_u32le(&r);
        TEST_CHECK(confirm == 0xB007B007u, "sys/reboot_bootloader_correct_magic: decodes to SIMFW_CMD_SYS_REBOOT_BOOTLOADER_MAGIC");
    }
    { // reboot_bootloader_bad_magic: {0x08, 0,0,0,0}
        static const uint8_t wire[] = {0x08, 0x00, 0x00, 0x00, 0x00};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint32_t confirm = ar_u32le(&r);
        TEST_CHECK(confirm != 0xB007B007u, "sys/reboot_bootloader_bad_magic: decodes to something other than the magic");
    }
    { // get_task_stats: {0x0A} -- no args
        static const uint8_t wire[] = {0x0A};
        TEST_CHECK(wire[0] == 10u, "sys/get_task_stats: cmd_id byte is 10 (0x0A)");
    }
}

static void test_sys_replies(void)
{
    TEST_SECTION("cmd payload vectors -- SYS replies");

    { // ping_ok: {0x00}
        uint8_t out[16];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        static const uint8_t expected[] = {0x00};
        TEST_CHECK(w.len == sizeof(expected) && bytes_eq(out, expected, sizeof(expected)), "sys/ping_ok matches manifest");
    }
    { // get_version_ok -- synthetic version block (protocol_version=7, min_compatible=5,
      // fw 2.3.4, dirty=1, git_hash="abc1234")
        uint8_t out[32];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u16le(&w, 7);
        rw_u16le(&w, 5);
        rw_u8(&w, 2);
        rw_u8(&w, 3);
        rw_u8(&w, 4);
        rw_u8(&w, 1);
        const char *hash = "abc1234";
        rw_u8(&w, (uint8_t)strlen(hash));
        rw_bytes(&w, (const uint8_t *)hash, (uint8_t)strlen(hash));
        static const uint8_t expected[] = {
            0x00, 0x07, 0x00, 0x05, 0x00, 0x02, 0x03, 0x04, 0x01, 0x07,
            0x61, 0x62, 0x63, 0x31, 0x32, 0x33, 0x34,
        };
        TEST_CHECK(w.len == sizeof(expected) && bytes_eq(out, expected, sizeof(expected)), "sys/get_version_ok matches manifest");
    }
    { // get_caps_ok -- same version block + caps tail
        uint8_t out[48];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u16le(&w, 7);
        rw_u16le(&w, 5);
        rw_u8(&w, 2);
        rw_u8(&w, 3);
        rw_u8(&w, 4);
        rw_u8(&w, 1);
        const char *hash = "abc1234";
        rw_u8(&w, (uint8_t)strlen(hash));
        rw_bytes(&w, (const uint8_t *)hash, (uint8_t)strlen(hash));
        rw_u8(&w, 1); // zone_count_min
        rw_u8(&w, 4); // zone_count_max
        rw_u8(&w, 3); // zone_count_default
        rw_u8(&w, 3); // tc_main_channels
        rw_u8(&w, 1); // tc_safety_channels
        rw_u8(&w, 3); // ct_channels
        rw_u8(&w, 3); // relay_channels
        rw_u32le(&w, 0x3Fu); // feature_bitmask
        static const uint8_t expected[] = {
            0x00, 0x07, 0x00, 0x05, 0x00, 0x02, 0x03, 0x04, 0x01, 0x07,
            0x61, 0x62, 0x63, 0x31, 0x32, 0x33, 0x34,
            0x01, 0x04, 0x03, 0x03, 0x01, 0x03, 0x03,
            0x3F, 0x00, 0x00, 0x00,
        };
        TEST_CHECK(w.len == sizeof(expected) && bytes_eq(out, expected, sizeof(expected)), "sys/get_caps_ok matches manifest");
    }
    { // get_sim_state_typical: seed=0xDEADBEEF, snapshot_valid=1, timescale_x100=250, sim_time_us=0x0102030405060708
        uint8_t out[24];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u32le(&w, 0xDEADBEEFu);
        rw_u8(&w, 1);
        rw_u32le(&w, 250u);
        rw_u64le(&w, 0x0102030405060708ULL);
        static const uint8_t expected[] = {
            0x00, 0xEF, 0xBE, 0xAD, 0xDE, 0x01, 0xFA, 0x00, 0x00, 0x00,
            0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        };
        TEST_CHECK(w.len == sizeof(expected) && bytes_eq(out, expected, sizeof(expected)), "sys/get_sim_state_typical matches manifest");
    }
    { // get_sim_state_zero
        uint8_t out[24];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u32le(&w, 0u);
        rw_u8(&w, 0);
        rw_u32le(&w, 0u);
        rw_u64le(&w, 0ULL);
        static const uint8_t expected[18] = {0};
        TEST_CHECK(w.len == sizeof(expected) && bytes_eq(out, expected, sizeof(expected)), "sys/get_sim_state_zero matches manifest");
    }
    { // reset_sim_busy / reboot_bootloader_bad_args -- single-status replies
        uint8_t out[4];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x04); // ERR_BUSY
        static const uint8_t expected[] = {0x04};
        TEST_CHECK(bytes_eq(out, expected, sizeof(expected)), "sys/reset_sim_busy matches manifest");

        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x02); // ERR_BAD_ARGS
        static const uint8_t expected2[] = {0x02};
        TEST_CHECK(bytes_eq(out, expected2, sizeof(expected2)), "sys/reboot_bootloader_bad_args matches manifest");
    }
    { // get_task_stats_two_tasks -- mirrors handle_sys_get_task_stats()'s
      // own rw_* call sequence: status, then a count byte patched after the
      // fact (same "patch the count slot once N is known" pattern
      // handle_fault_list() uses), then N * {u8 task_stats_id,
      // u16 allocated_bytes LE, u16 hwm_free_bytes LE}. This mirror stands
      // in for the WORDS->BYTES conversion + uxTaskGetSystemState() walk
      // the real handler does (not host-testable -- FreeRTOS-only) by
      // starting directly from already-converted byte values, same as this
      // file's other reply mirrors stand in for their owner-API call
      // results with literal values.
        uint8_t out[32];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00); // status OK
        uint8_t *count_slot = &out[w.len];
        rw_u8(&w, 0); // patched below
        uint8_t returned = 0;

        rw_u8(&w, 1); // task_stats_id: SIMFW_TASK_STATS_ID_CMD_TASK
        rw_u16le(&w, 3072); // allocated_bytes
        rw_u16le(&w, 2500); // hwm_free_bytes
        returned++;

        rw_u8(&w, 6); // task_stats_id: SIMFW_TASK_STATS_ID_TELEMETRY
        rw_u16le(&w, 4096); // allocated_bytes
        rw_u16le(&w, 1000); // hwm_free_bytes
        returned++;

        *count_slot = returned;

        static const uint8_t expected[] = {
            0x00, 0x02,
            0x01, 0x00, 0x0C, 0xC4, 0x09,
            0x06, 0x00, 0x10, 0xE8, 0x03,
        };
        TEST_CHECK(w.len == sizeof(expected) && bytes_eq(out, expected, sizeof(expected)),
                   "sys/get_task_stats_two_tasks matches manifest");
    }
}

// ===========================================================================
// IO group -- vectors/cmd_payload_vectors.json "io" section
// ===========================================================================

static void test_io_requests(void)
{
    TEST_SECTION("cmd payload vectors -- IO requests");

    { // set_dir_typical: {0x01, exp=1, pin=7, is_input=1, pullup=0}
        static const uint8_t wire[] = {0x01, 0x01, 0x07, 0x01, 0x00};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint8_t exp = ar_u8(&r), pin = ar_u8(&r), is_input = ar_u8(&r), pullup = ar_u8(&r);
        TEST_CHECK(!r.overflow, "io/set_dir_typical: no overflow");
        TEST_CHECK(exp == 1 && pin == 7 && is_input == 1 && pullup == 0, "io/set_dir_typical: fields decode in exp,pin,is_input,pullup order");
    }
    { // set_dir_min: {0x01, 0,0,0,1}
        static const uint8_t wire[] = {0x01, 0x00, 0x00, 0x00, 0x01};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint8_t exp = ar_u8(&r), pin = ar_u8(&r), is_input = ar_u8(&r), pullup = ar_u8(&r);
        TEST_CHECK(exp == 0 && pin == 0 && is_input == 0 && pullup == 1, "io/set_dir_min matches manifest");
    }
    { // set_dir_max_pin: {0x01, 2, 255, 1, 1}
        static const uint8_t wire[] = {0x01, 0x02, 0xFF, 0x01, 0x01};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint8_t exp = ar_u8(&r), pin = ar_u8(&r), is_input = ar_u8(&r), pullup = ar_u8(&r);
        TEST_CHECK(exp == 2 && pin == 255 && is_input == 1 && pullup == 1, "io/set_dir_max_pin matches manifest (pin=255 doesn't wrap)");
    }
    { // write_typical: {0x02, exp=0, pin=3, level=1}
        static const uint8_t wire[] = {0x02, 0x00, 0x03, 0x01};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint8_t exp = ar_u8(&r), pin = ar_u8(&r), level = ar_u8(&r);
        TEST_CHECK(exp == 0 && pin == 3 && level == 1, "io/write_typical matches manifest");
    }
    { // read_typical: {0x03, exp=1, pin=15}
        static const uint8_t wire[] = {0x03, 0x01, 0x0F};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint8_t exp = ar_u8(&r), pin = ar_u8(&r);
        TEST_CHECK(!r.overflow && exp == 1 && pin == 15, "io/read_typical matches manifest");
    }
    { // estop_set_open / estop_set_closed
        static const uint8_t wire_open[] = {0x04, 0x01};
        static const uint8_t wire_closed[] = {0x04, 0x00};
        arg_reader_t r;
        ar_init(&r, wire_open + 1, sizeof(wire_open) - 1);
        TEST_CHECK(ar_u8(&r) == 1, "io/estop_set_open matches manifest");
        ar_init(&r, wire_closed + 1, sizeof(wire_closed) - 1);
        TEST_CHECK(ar_u8(&r) == 0, "io/estop_set_closed matches manifest");
    }
    { // dut_power_set_on / dut_power_safety_set_on
        static const uint8_t wire[] = {0x06, 0x01};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        TEST_CHECK(ar_u8(&r) == 1, "io/dut_power_set_on matches manifest");
        static const uint8_t wire2[] = {0x09, 0x01};
        ar_init(&r, wire2 + 1, sizeof(wire2) - 1);
        TEST_CHECK(ar_u8(&r) == 1, "io/dut_power_safety_set_on matches manifest");
    }
    { // no-arg requests: fault_line_get, estop_get, dut_power_get, dut_power_safety_get, bus_scan
        static const uint8_t w1[] = {0x05};
        static const uint8_t w2[] = {0x07};
        static const uint8_t w3[] = {0x08};
        static const uint8_t w4[] = {0x0A};
        static const uint8_t w5[] = {0x0B};
        TEST_CHECK(w1[0] == 0x05, "io/fault_line_get: cmd_id byte matches manifest");
        TEST_CHECK(w2[0] == 0x07, "io/estop_get: cmd_id byte matches manifest");
        TEST_CHECK(w3[0] == 0x08, "io/dut_power_get: cmd_id byte matches manifest");
        TEST_CHECK(w4[0] == 0x0A, "io/dut_power_safety_get: cmd_id byte matches manifest");
        TEST_CHECK(w5[0] == 0x0B, "io/bus_scan: cmd_id byte matches manifest");
    }
}

static void test_io_replies(void)
{
    TEST_SECTION("cmd payload vectors -- IO replies");

    { // read_level_high / read_level_low
        uint8_t out[4];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u8(&w, 1);
        static const uint8_t expected_high[] = {0x00, 0x01};
        TEST_CHECK(w.len == 2 && bytes_eq(out, expected_high, 2), "io/read_level_high matches manifest");

        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u8(&w, 0);
        static const uint8_t expected_low[] = {0x00, 0x00};
        TEST_CHECK(bytes_eq(out, expected_low, 2), "io/read_level_low matches manifest");
    }
    { // read_no_sample: status-only
        uint8_t out[4];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x05); // ERR_NO_SAMPLE
        static const uint8_t expected[] = {0x05};
        TEST_CHECK(w.len == 1 && bytes_eq(out, expected, 1), "io/read_no_sample matches manifest");
    }
    { // fault_line_get_typical: asserted=1, sample_time_us=1, valid=1
        uint8_t out[16];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u8(&w, 1);
        rw_u64le(&w, 1ULL);
        rw_u8(&w, 1);
        static const uint8_t expected[] = {0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
        TEST_CHECK(w.len == sizeof(expected) && bytes_eq(out, expected, sizeof(expected)), "io/fault_line_get_typical matches manifest");
    }
    { // fault_line_get_zero
        uint8_t out[16];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u8(&w, 0);
        rw_u64le(&w, 0ULL);
        rw_u8(&w, 0);
        static const uint8_t expected[11] = {0};
        TEST_CHECK(w.len == sizeof(expected) && bytes_eq(out, expected, sizeof(expected)), "io/fault_line_get_zero matches manifest");
    }
    { // fault_line_get_max_time: sample_time_us = UINT64_MAX
        uint8_t out[16];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u8(&w, 1);
        rw_u64le(&w, 0xFFFFFFFFFFFFFFFFULL);
        rw_u8(&w, 1);
        static const uint8_t expected[] = {0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
        TEST_CHECK(w.len == sizeof(expected) && bytes_eq(out, expected, sizeof(expected)), "io/fault_line_get_max_time matches manifest");
    }
    { // estop_get_open / estop_get_closed / dut_power_get_on / dut_power_safety_get_off
        uint8_t out[4];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u8(&w, 1);
        static const uint8_t expect_open[] = {0x00, 0x01};
        TEST_CHECK(bytes_eq(out, expect_open, 2), "io/estop_get_open matches manifest");

        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u8(&w, 0);
        static const uint8_t expect_closed[] = {0x00, 0x00};
        TEST_CHECK(bytes_eq(out, expect_closed, 2), "io/estop_get_closed and io/dut_power_safety_get_off match manifest");
    }
    { // bus_scan_match: configured 0x25/0x26 (37/38), both found -- bitmap
      // bit 29 (addr 0x25) + bit 30 (addr 0x26) set = byte index 3 = 0x60,
      // every other bitmap byte 0 (handle_io_bus_scan()'s rw_bytes(bitmap, 14)).
        uint8_t out[24];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u8(&w, 37);  // configured_addr1 = 0x25
        rw_u8(&w, 38);  // configured_addr2 = 0x26
        uint8_t bitmap_match[14] = {0};
        bitmap_match[3] = 0x60u;
        rw_bytes(&w, bitmap_match, sizeof(bitmap_match));
        static const uint8_t expected[] = {
            0x00, 0x25, 0x26,
            0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        TEST_CHECK(sizeof(expected) == 17u, "sanity: expected array is 17 bytes (1 status + 2 addrs + 14-byte bitmap)");
        TEST_CHECK(w.len == sizeof(expected) && bytes_eq(out, expected, sizeof(expected)),
                   "io/bus_scan_match matches manifest (17 bytes: status, 2 configured addrs, 14-byte bitmap)");
    }
    { // bus_scan_mismatch: configured 0x20/0x21 (32/33, the POR default this
      // pass's incident showed was WRONG on the real bench) but the same
      // 0x25/0x26-only bitmap as bus_scan_match -- proves the reply layout
      // is identical regardless of match/mismatch; the *content* difference
      // (configured_addr1/2) is what a client's own comparison (cli.py's
      // _print_io_scan_result(), payloads.py's "match" field) acts on, not
      // anything the wire layout itself encodes as a boolean.
        uint8_t out[24];
        reply_writer_t w;
        rw_init(&w, out, sizeof(out));
        rw_u8(&w, 0x00);
        rw_u8(&w, 32);  // configured_addr1 = 0x20
        rw_u8(&w, 33);  // configured_addr2 = 0x21
        uint8_t bitmap_mismatch[14] = {0};
        bitmap_mismatch[3] = 0x60u;
        rw_bytes(&w, bitmap_mismatch, sizeof(bitmap_mismatch));
        static const uint8_t expected[] = {
            0x00, 0x20, 0x21,
            0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        TEST_CHECK(sizeof(expected) == 17u, "sanity: expected array is 17 bytes (1 status + 2 addrs + 14-byte bitmap)");
        TEST_CHECK(w.len == sizeof(expected) && bytes_eq(out, expected, sizeof(expected)),
                   "io/bus_scan_mismatch matches manifest -- same bitmap, different configured addrs");
    }
}

// ===========================================================================
// TC group float sample -- vectors/cmd_payload_vectors.json "tc_float_sample"
// ===========================================================================

static void test_tc_float_requests(void)
{
    TEST_SECTION("cmd payload vectors -- TC float sample (FORCE_TEMP)");

    { // force_temp_typical: channel=1, temp_c=1234.5
        static const uint8_t wire[] = {0x02, 0x01, 0x00, 0x50, 0x9A, 0x44};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint8_t channel = ar_u8(&r);
        float temp_c = ar_f32le(&r);
        TEST_CHECK(!r.overflow, "tc/force_temp_typical: no overflow");
        TEST_CHECK(channel == 1, "tc/force_temp_typical: channel decodes to 1");
        TEST_CHECK_NEAR(temp_c, 1234.5, 0.001, "tc/force_temp_typical: temp_c decodes to 1234.5 (LE IEEE-754 f32)");
    }
    { // force_temp_zero: channel=0, temp_c=0.0
        static const uint8_t wire[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint8_t channel = ar_u8(&r);
        float temp_c = ar_f32le(&r);
        TEST_CHECK(channel == 0 && temp_c == 0.0f, "tc/force_temp_zero matches manifest");
    }
    { // force_temp_negative: channel=3, temp_c=-40.0
        static const uint8_t wire[] = {0x02, 0x03, 0x00, 0x00, 0x20, 0xC2};
        arg_reader_t r;
        ar_init(&r, wire + 1, sizeof(wire) - 1);
        uint8_t channel = ar_u8(&r);
        float temp_c = ar_f32le(&r);
        TEST_CHECK(channel == 3, "tc/force_temp_negative: channel decodes to 3");
        TEST_CHECK_NEAR(temp_c, -40.0, 0.001, "tc/force_temp_negative: negative float decodes correctly, sign bit not lost");
    }
}

void run_test_cmd_payload_vectors(void)
{
    test_sys_requests();
    test_sys_replies();
    test_io_requests();
    test_io_replies();
    test_tc_float_requests();
}
