// test_link_task_fuzz.c -- host campaign 2 (docs/audits/HOST_TEST_COVERAGE_GAPS_2026-10-09.md).
//
// link_task.c is compiled into this executable as-is (#include of the .c),
// so the statics it keeps (RX assembly buffer, context session state, trip
// burst state) are directly visible, and the REAL rx path
// (link_task_rx_process_byte -> link_task_handle_raw_frame -> handlers) is
// driven byte by byte. Everything link_task calls OUT to (safety_core, uart,
// config_store state, thermo/current/update tasks, log) is a recording fake
// defined below.
//
// What this proves:
//   * The only route to safety_core_request_enable(true) is a CRC-valid
//     BROADCAST frame ESP(0) -> SAFETY(2), cmd 0x02, length exactly 2,
//     payload[1] != 0. Bad CRC (every single-bit flip), truncation, trailing
//     bytes, wrong direction, wrong msg_type, wrong length, and a seeded
//     stream of random bytes / mutated frames / re-CRC'd random payloads
//     never grant heat.
//   * PUSH_CONTEXT never grants heat; a new ESP session (boot_id change or
//     context gap) without HEAT_OWNER_ACTIVE de-energises (enable(false)).
//   * Oversized runs resync on the next delimiter; malformed frames never
//     crash or run past a buffer (this exe is built with ASan when the
//     toolchain supports it, see build_host_tests.ps1).
//   * trip_seq wrap 255 -> 1 starts a new trip burst, a repeated seq does not.
//
// Deterministic: fixed xorshift seed, bounded iteration counts.

#include "link_task.c"

#include <stdio.h>
#include <stdlib.h>

// --- recording fakes ----------------------------------------------------------

static TickType_t g_tick_ms;
static int g_enable_true, g_enable_false;
static int g_sends, g_logs, g_cfg_writes, g_reboots, g_clear_trip_requests;

absolute_time_t get_absolute_time(void)
{
    absolute_time_t t;
    t.us = (uint64_t)g_tick_ms * 1000u;
    return t;
}
uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t.us / 1000u); }
uint32_t time_us_32(void) { return (uint32_t)((uint64_t)g_tick_ms * 1000u); }
uint32_t get_rand_32(void) { return 0x5Au; }
TickType_t xTaskGetTickCount(void) { return g_tick_ms; }
void vTaskDelay(TickType_t t) { g_tick_ms += t; }
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_words, void *arg,
                        UBaseType_t priority, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack_words; (void)arg; (void)priority;
    if (out) { *out = (TaskHandle_t)1; }
    return pdPASS;
}
void vTaskCoreAffinitySet(TaskHandle_t t, UBaseType_t m) { (void)t; (void)m; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t t) { (void)h; (void)t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { (void)h; return pdTRUE; }
void watchdog_task_checkin(watchdog_checkin_id_t id) { (void)id; }

bool uart_owner_send(const uint8_t *d, size_t n) { (void)d; (void)n; g_sends++; return true; }
size_t uart_owner_rx_read(uint8_t *o, size_t m) { (void)o; (void)m; return 0; }
size_t uart_owner_get_last_send_remainder(void) { return 0; }
uint32_t uart_owner_get_tx_bytes_from_isr(void) { return 0; }
size_t uart_owner_get_tx_capacity(void) { return 1024; }
uint32_t uart_owner_get_tx_dropped(void) { return 0; }
uint32_t uart_owner_get_tx_head(void) { return 0; }
uint32_t uart_owner_get_tx_tail(void) { return 0; }
size_t uart_owner_get_tx_used(void) { return 0; }

// safety_core: only the enable path matters; everything else is inert.
bool safety_core_request_enable(bool enable)
{
    if (enable) { g_enable_true++; } else { g_enable_false++; }
    return true;
}
bool safety_core_request_clear_trip(bool bound, uint8_t seq) { (void)bound; (void)seq; g_clear_trip_requests++; return true; }
uint32_t safety_core_ms_since_last_enable_true_request(bool *ever) { if (ever) { *ever = false; } return 0; }
void safety_core_set_tc_type_apply_in_progress(bool b) { (void)b; }
void safety_core_get_output_status(bool *r, bool *h) { if (r) { *r = false; } if (h) { *h = false; } }
void safety_core_get_diag_status(safety_trip_t *t, bool *w, uint8_t *s, uint16_t *m)
{
    *t = SAFETY_TRIP_NONE; *w = false; *s = 0; *m = 0;
}

static uint8_t g_trip_seq;
static safety_trip_t g_trip_reason;
static bool g_have_trip;
bool safety_core_get_trip_event(uint8_t *seq, safety_trip_t *reason, uint32_t *uptime, float *tc, float *thr)
{
    *seq = g_trip_seq; *reason = g_trip_reason; *uptime = 1234u; *tc = 1000.0f; *thr = 900.0f;
    return g_have_trip;
}

saftyfw_boot_reason_t boot_reason_get_cached(void) { saftyfw_boot_reason_t r; memset(&r, 0, sizeof(r)); return r; }
clear_trip_diag_t clear_trip_diag_get_cached(void) { clear_trip_diag_t r; memset(&r, 0, sizeof(r)); return r; }
watchdog_fatal_diag_t watchdog_fatal_diag_get_cached(void) { watchdog_fatal_diag_t r; memset(&r, 0, sizeof(r)); return r; }

bool config_store_check_ram_integrity(void) { return true; }
uint16_t config_store_get_config_crc(void) { return 0; }
uint8_t config_store_get_config_version(void) { return 1; }
void config_store_get_ct_cal(config_store_ct_channel_cal_t out[CONFIG_STORE_CT_CAL_NUM_CHANNELS])
{
    memset(out, 0, sizeof(config_store_ct_channel_cal_t) * CONFIG_STORE_CT_CAL_NUM_CHANNELS);
}
bool config_store_get_full_record(config_store_record_t *out) { config_store_default(out); return true; }
uint8_t config_store_get_persisted_tc_type(void) { return 0; }
uint32_t config_store_get_ram_integrity_fail_count(void) { return 0; }
uint8_t config_store_get_tc_type(void) { return 0; }
bool config_store_is_abs_max_temp_disabled(void) { return false; }
bool config_store_is_calibration_missing(void) { return false; }
bool config_store_is_rate_guard_disabled(void) { return false; }
bool config_store_is_volatile_dirty(void) { return false; }
void config_store_set_heat_possible_probe(config_store_heat_possible_probe_t p) { (void)p; }
bool config_store_write(const config_store_record_t *r, const char **why)
{
    (void)r; g_cfg_writes++; if (why) { *why = "fake"; } return false;
}
bool config_store_write_ex(const config_store_record_t *r, bool heat_safe, const char **why,
                            config_store_write_decision_t *d)
{
    (void)r; (void)heat_safe; g_cfg_writes++; if (why) { *why = "fake"; } if (d) { memset(d, 0, sizeof(*d)); } return false;
}
bool config_store_write_volatile(const config_store_record_t *r, const char **why)
{
    (void)r; g_cfg_writes++; if (why) { *why = "fake"; } return false;
}

bool current_task_any_current_present(void) { return false; }
bool current_task_ct_auto_zero_begin(uint8_t ch) { (void)ch; return false; }
void current_task_ct_auto_zero_poll(current_task_auto_zero_status_t *o) { memset(o, 0, sizeof(*o)); }
void current_task_get_power(current_sense_power_t *o) { memset(o, 0, sizeof(*o)); }
void current_task_get_snapshot(current_snapshot_t *o) { memset(o, 0, sizeof(*o)); }
void current_task_reload_cal(void) {}
void current_task_reload_ct_cal(void) {}
bool discrete_task_estop_pressed(void) { return false; }

uint32_t log_task_get_dropped(void) { return 0; }
bool log_task_log(uint8_t lvl, const char *tag, const char *msg) { (void)lvl; (void)tag; (void)msg; g_logs++; return true; }
void log_task_set_level(uint8_t lvl) { (void)lvl; }

const saftyfw_image_identity_t *saftyfw_image_identity_get(void)
{
    static saftyfw_image_identity_t id;
    return &id;
}
void stack_margin_poller_snapshot(kilnlink_stack_margin_t *o) { memset(o, 0, sizeof(*o)); }

bool thermo_task_get_snapshot(thermo_snapshot_t *o) { memset(o, 0, sizeof(*o)); return false; }
bool thermo_task_inject_reading(bool v, float tc, float cj, uint8_t f) { (void)v; (void)tc; (void)cj; (void)f; return false; }
bool thermo_task_injection_active(void) { return false; }
uint32_t thermo_task_live_config_mismatch_count(void) { return 0; }
bool thermo_task_reconfig_gave_up(void) { return false; }
void thermo_task_request_tc_type_reapply(void) {}

bool update_task_get_active_slot(bool *b) { *b = false; return false; }
void update_task_handle_abort(const uint8_t *p, uint8_t n) { (void)p; (void)n; }
void update_task_handle_begin(const uint8_t *p, uint8_t n) { (void)p; (void)n; }
void update_task_handle_data(const uint8_t *p, uint8_t n) { (void)p; (void)n; }
void update_task_handle_end(const uint8_t *p, uint8_t n) { (void)p; (void)n; }
bool update_task_reboot_allowed(const char **why, uint8_t *code) { *why = "fake"; *code = 0; return false; }
void update_task_reboot_now(void) { g_reboots++; }
bool update_task_request_rollback(const char **why, uint8_t *code) { *why = "fake"; *code = 0; return false; }

// --- harness ---------------------------------------------------------------------

static int g_checks, g_fail;
#define CHECK(c, ...)                                                                  \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(c)) {                                                                    \
            g_fail++;                                                                  \
            printf("FAIL line %d: ", __LINE__);                                        \
            printf(__VA_ARGS__);                                                       \
            printf("\n");                                                              \
        }                                                                              \
    } while (0)

static uint32_t g_rng = 0x1234ABCDu;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static uint16_t g_msg_index;

// Encodes + stuffs a frame, returns the stuffed length.
static size_t make_stuffed(uint8_t *out, size_t cap, kilnlink_msg_type_t type, uint8_t src, uint8_t dst,
                           const uint8_t *payload, uint8_t len, uint16_t idx)
{
    kilnlink_frame_t f;
    memset(&f, 0, sizeof(f));
    f.msg_type = type;
    f.msg_index = idx;
    f.src_device = src;
    f.dst_device = dst;
    f.length = len;
    f.payload = payload;
    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t st;
    size_t rl = kilnlink_frame_encode_raw(&f, raw, sizeof(raw), &st);
    if (rl == 0) { return 0; }
    return kilnlink_stuff(raw, rl, out, cap);
}

static size_t make_raw(uint8_t *raw, kilnlink_msg_type_t type, uint8_t src, uint8_t dst, const uint8_t *payload,
                       uint8_t len, uint16_t idx)
{
    kilnlink_frame_t f;
    memset(&f, 0, sizeof(f));
    f.msg_type = type;
    f.msg_index = idx;
    f.src_device = src;
    f.dst_device = dst;
    f.length = len;
    f.payload = payload;
    kilnlink_frame_status_t st;
    return kilnlink_frame_encode_raw(&f, raw, KILNLINK_FRAME_RAW_MAX, &st);
}

static void feed(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) { link_task_rx_process_byte(b[i]); }
}

static void send_frame(kilnlink_msg_type_t type, uint8_t src, uint8_t dst, const uint8_t *payload, uint8_t len)
{
    uint8_t buf[KILNLINK_FRAME_STUFFED_MAX];
    size_t n = make_stuffed(buf, sizeof(buf), type, src, dst, payload, len, g_msg_index++);
    feed(buf, n);
}

static void send_esp(const uint8_t *payload, uint8_t len)
{
    send_frame(KILNLINK_MSG_BROADCAST, LINK_FRAME_DEVICE_ESP, LINK_FRAME_DEVICE_SAFETY, payload, len);
}

static void reset_counts(void)
{
    g_enable_true = g_enable_false = 0;
}

static void reset_link_state(void)
{
    s_rx_assembly_len = 0;
    s_rx_collecting = false;
    s_valid_frame_seen = false;
    reset_counts();
}

// PUSH_CONTEXT payload: cmd, flags, boot_id, seq(4), uptime(4), now, recent, window, zone_count
static uint8_t build_context(uint8_t *p, uint8_t flags, uint8_t boot_id, uint8_t zones)
{
    memset(p, 0, 15u + zones * 14u);
    p[0] = LINK_FRAME_PUSH_CONTEXT_CMD;
    p[1] = flags;
    p[2] = boot_id;
    p[14] = zones;
    return (uint8_t)(15u + zones * 14u);
}

static void scenario_enable_gating(void)
{
    reset_link_state();
    uint8_t on[2] = { LINK_FRAME_REQUEST_ENABLE_CMD, 1 };
    uint8_t off[2] = { LINK_FRAME_REQUEST_ENABLE_CMD, 0 };

    send_esp(on, 2);
    CHECK(g_enable_true == 1 && g_enable_false == 0, "valid enable(1): true=%d false=%d", g_enable_true, g_enable_false);
    CHECK(s_valid_frame_seen, "valid frame marks link alive");
    send_esp(off, 2);
    CHECK(g_enable_true == 1 && g_enable_false == 1, "valid enable(0): true=%d false=%d", g_enable_true, g_enable_false);

    // payload[1] = any nonzero is a grant request, including 0xFF and 2.
    reset_counts();
    uint8_t ff[2] = { LINK_FRAME_REQUEST_ENABLE_CMD, 0xFF };
    send_esp(ff, 2);
    CHECK(g_enable_true == 1, "enable(0xFF) is a grant request (documented: != 0)");

    // wrong lengths 0..N (length 0 BROADCAST is dropped before dispatch)
    reset_counts();
    for (uint8_t len = 1; len <= 40; len++) {
        if (len == 2) { continue; }
        uint8_t p[40];
        memset(p, 1, sizeof(p));
        p[0] = LINK_FRAME_REQUEST_ENABLE_CMD;
        send_esp(p, len);
    }
    CHECK(g_enable_true == 0 && g_enable_false == 0, "wrong-length enable frames: true=%d false=%d", g_enable_true, g_enable_false);
    uint8_t big[255];
    memset(big, 1, sizeof(big));
    big[0] = LINK_FRAME_REQUEST_ENABLE_CMD;
    send_esp(big, 255);
    CHECK(g_enable_true == 0, "max-length (255) enable frame refused");

    // wrong direction / device pairs
    reset_link_state();
    for (int src = 0; src < 4; src++) {
        for (int dst = 0; dst < 4; dst++) {
            if (src == LINK_FRAME_DEVICE_ESP && dst == LINK_FRAME_DEVICE_SAFETY) { continue; }
            send_frame(KILNLINK_MSG_BROADCAST, (uint8_t)src, (uint8_t)dst, on, 2);
        }
    }
    send_frame(KILNLINK_MSG_BROADCAST, 0xFF, 0xFF, on, 2);
    CHECK(g_enable_true == 0, "wrong src/dst never grants: %d", g_enable_true);
    CHECK(!s_valid_frame_seen, "wrong-direction frames (incl. loopback 2->0) never mark the link alive");

    // wrong msg_type
    reset_counts();
    send_frame(KILNLINK_MSG_DATA, 0, 2, on, 2);
    send_frame(KILNLINK_MSG_ACK, 0, 2, on, 2);
    send_frame(KILNLINK_MSG_NACK, 0, 2, on, 2);
    CHECK(g_enable_true == 0, "DATA/ACK/NACK enable frames never grant: %d", g_enable_true);
    CHECK(s_valid_frame_seen, "non-broadcast ESP->Pico frames still prove the ESP is alive");

    // duplicate frame (same msg_index): no Pico-side dedup, each copy is a request
    reset_counts();
    uint8_t buf[KILNLINK_FRAME_STUFFED_MAX];
    size_t n = make_stuffed(buf, sizeof(buf), KILNLINK_MSG_BROADCAST, 0, 2, on, 2, 7);
    feed(buf, n);
    feed(buf, n);
    CHECK(g_enable_true == 2, "duplicate msg_index is not de-duplicated by link_task (observation): %d", g_enable_true);
    // msg_index 0xFFFF -> 0 wrap carries no state
    reset_counts();
    g_msg_index = 0xFFFEu;
    send_esp(on, 2);
    send_esp(on, 2);
    send_esp(on, 2);
    CHECK(g_enable_true == 3, "msg_index wrap 0xFFFF->0 does not affect handling: %d", g_enable_true);
}

static void scenario_bad_crc_and_truncation(void)
{
    reset_link_state();
    uint8_t on[2] = { LINK_FRAME_REQUEST_ENABLE_CMD, 1 };
    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    size_t rl = make_raw(raw, KILNLINK_MSG_BROADCAST, 0, 2, on, 2, 1);
    CHECK(rl > 0, "raw encode");

    // every single-bit flip of the unstuffed frame (header, payload, CRC) is rejected
    int flipped_grants = 0;
    for (size_t i = 0; i < rl; i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t c[KILNLINK_FRAME_RAW_MAX];
            memcpy(c, raw, rl);
            c[i] ^= (uint8_t)(1u << bit);
            uint8_t st[KILNLINK_FRAME_STUFFED_MAX];
            size_t sn = kilnlink_stuff(c, rl, st, sizeof(st));
            reset_counts();
            feed(st, sn);
            flipped_grants += g_enable_true;
        }
    }
    CHECK(flipped_grants == 0, "single-bit-flipped enable frames granted %d times", flipped_grants);

    // truncation at every length (dropping tail bytes, frame re-delimited)
    int trunc_grants = 0;
    for (size_t cut = 0; cut < rl; cut++) {
        uint8_t st[KILNLINK_FRAME_STUFFED_MAX];
        size_t sn = kilnlink_stuff(raw, cut, st, sizeof(st));
        reset_counts();
        feed(st, sn);
        trunc_grants += g_enable_true;
    }
    CHECK(trunc_grants == 0, "truncated enable frames granted %d times", trunc_grants);

    // trailing garbage inside the frame body (length mismatch)
    for (uint8_t extra = 1; extra <= 8; extra++) {
        uint8_t c[KILNLINK_FRAME_RAW_MAX];
        memcpy(c, raw, rl);
        for (uint8_t k = 0; k < extra; k++) { c[rl + k] = (uint8_t)rnd(); }
        uint8_t st[KILNLINK_FRAME_STUFFED_MAX];
        size_t sn = kilnlink_stuff(c, rl + extra, st, sizeof(st));
        reset_counts();
        feed(st, sn);
        CHECK(g_enable_true == 0, "frame with %u trailing bytes granted", (unsigned)extra);
    }

    // sanity: the unmodified frame does grant
    uint8_t st[KILNLINK_FRAME_STUFFED_MAX];
    size_t sn = kilnlink_stuff(raw, rl, st, sizeof(st));
    reset_counts();
    feed(st, sn);
    CHECK(g_enable_true == 1, "control: unmodified frame grants once");

    // unterminated escape and lone delimiters
    reset_counts();
    uint8_t esc[] = { 0x7E, 0x7D, 0x7E, 0x7E, 0x7E, 0x7D, 0x7D, 0x7E };
    feed(esc, sizeof(esc));
    CHECK(g_enable_true == 0, "escape garbage never grants");
}

static void scenario_resync(void)
{
    reset_link_state();
    uint8_t on[2] = { LINK_FRAME_REQUEST_ENABLE_CMD, 1 };

    // A run longer than the assembly buffer with no delimiter, then a good frame.
    for (size_t i = 0; i < (size_t)LINK_RX_ASSEMBLY_MAX * 3u; i++) { link_task_rx_process_byte(0x11); }
    CHECK(s_rx_assembly_len <= LINK_RX_ASSEMBLY_MAX, "assembly length bounded: %u", (unsigned)s_rx_assembly_len);
    reset_counts();
    send_esp(on, 2);
    CHECK(g_enable_true == 1, "valid frame after oversized run grants (resync): %d", g_enable_true);

    // Noise before the first delimiter is dropped, never assembled.
    reset_link_state();
    for (int i = 0; i < 700; i++) {
        uint8_t b = (uint8_t)rnd();
        if (b == 0x7Eu) { b = 0x01; }
        link_task_rx_process_byte(b);
    }
    send_esp(on, 2);
    CHECK(g_enable_true == 1, "frame after pre-delimiter noise grants: %d", g_enable_true);

    // Oversized run opened by a delimiter, then the same run, then a frame.
    reset_link_state();
    link_task_rx_process_byte(0x7E);
    for (size_t i = 0; i < (size_t)LINK_RX_ASSEMBLY_MAX + 50u; i++) { link_task_rx_process_byte(0x22); }
    CHECK(!s_rx_collecting, "overflow drops collection until next delimiter");
    send_esp(on, 2);
    CHECK(g_enable_true == 1, "overflow then valid frame grants: %d", g_enable_true);

    // Two frames back to back sharing one delimiter.
    reset_link_state();
    uint8_t a[KILNLINK_FRAME_STUFFED_MAX], b[KILNLINK_FRAME_STUFFED_MAX];
    size_t an = make_stuffed(a, sizeof(a), KILNLINK_MSG_BROADCAST, 0, 2, on, 2, 1);
    size_t bn = make_stuffed(b, sizeof(b), KILNLINK_MSG_BROADCAST, 0, 2, on, 2, 2);
    feed(a, an);
    feed(b + 1, bn - 1); // drop b's leading delimiter; a's trailing one serves both
    CHECK(g_enable_true == 2, "shared-delimiter back-to-back frames: %d", g_enable_true);
}

static void scenario_push_context(void)
{
    reset_link_state();
    uint8_t p[15 + 14 * 12];
    uint8_t n;

    // First context: records boot id. Never an enable(true).
    n = build_context(p, CONTEXT_FLAG_CONTEXT_VALID, 10, 1);
    send_esp(p, n);
    CHECK(g_enable_true == 0, "PUSH_CONTEXT never grants heat");
    CHECK(s_context_boot_id_known && s_last_context_boot_id == 10, "boot id recorded");
    uint32_t ok_before = s_context_frames_ok;

    // Same session, no heat owner flag: no drop.
    reset_counts();
    g_tick_ms += 100;
    n = build_context(p, CONTEXT_FLAG_CONTEXT_VALID, 10, 1);
    send_esp(p, n);
    CHECK(g_enable_false == 0, "same boot_id: no grant drop (%d)", g_enable_false);
    CHECK(s_context_frames_ok == ok_before + 1, "context counted");

    // boot_id change with heat owner active: grant kept.
    reset_counts();
    g_tick_ms += 100;
    n = build_context(p, CONTEXT_FLAG_CONTEXT_VALID | CONTEXT_FLAG_HEAT_OWNER_ACTIVE, 11, 1);
    send_esp(p, n);
    CHECK(g_enable_false == 0 && g_enable_true == 0, "new session claiming heat owner keeps grant (false=%d)", g_enable_false);

    // boot_id change WITHOUT heat owner: grant dropped (de-energise only).
    reset_counts();
    g_tick_ms += 100;
    n = build_context(p, CONTEXT_FLAG_CONTEXT_VALID, 12, 1);
    send_esp(p, n);
    CHECK(g_enable_false == 1 && g_enable_true == 0, "new session without heat owner drops grant: false=%d true=%d", g_enable_false, g_enable_true);

    // context gap >= 5 s with the same boot id counts as a new session.
    reset_counts();
    g_tick_ms += 5000;
    n = build_context(p, CONTEXT_FLAG_CONTEXT_VALID, 12, 1);
    send_esp(p, n);
    CHECK(g_enable_false == 1 && g_enable_true == 0, "context gap without heat owner drops grant: false=%d", g_enable_false);

    // gap just under the limit is not a new session
    reset_counts();
    g_tick_ms += 4900;
    n = build_context(p, CONTEXT_FLAG_CONTEXT_VALID, 12, 1);
    send_esp(p, n);
    CHECK(g_enable_false == 0, "4.9 s gap is the same session: false=%d", g_enable_false);

    // boot_id wrap 255 -> 0 is still a change.
    reset_counts();
    g_tick_ms += 100;
    n = build_context(p, CONTEXT_FLAG_CONTEXT_VALID, 255, 0);
    send_esp(p, n);
    reset_counts();
    g_tick_ms += 100;
    n = build_context(p, CONTEXT_FLAG_CONTEXT_VALID, 0, 0);
    send_esp(p, n);
    CHECK(g_enable_false == 1, "boot_id 255->0 is a session change: false=%d", g_enable_false);

    // Malformed PUSH_CONTEXT: never published, bad counter moves, no grant.
    reset_counts();
    uint32_t bad_before = s_context_frames_bad;
    context_snapshot_t before;
    bool had = link_task_get_context_snapshot(&before);
    uint8_t q[15 + 14 * 12 + 8];
    memset(q, 0, sizeof(q));
    n = build_context(q, 0xFF, 99, 1);
    send_esp(q, (uint8_t)(n - 1)); // truncated
    send_esp(q, (uint8_t)(n + 1)); // trailing byte
    q[14] = 200;                   // zone_count > MAX
    send_esp(q, 15);
    q[14] = 0;
    send_esp(q, 14);               // shorter than header
    context_snapshot_t after;
    bool have = link_task_get_context_snapshot(&after);
    CHECK(s_context_frames_bad == bad_before + 4, "malformed contexts counted: %u", (unsigned)(s_context_frames_bad - bad_before));
    CHECK(had == have && (!had || (before.boot_id == after.boot_id && before.flags == after.flags)),
          "malformed contexts never overwrite the last good snapshot");
    CHECK(g_enable_true == 0 && g_enable_false == 0, "malformed context has no grant side effect");

    // Max zone_count frame is accepted at exactly MAX.
    reset_counts();
    n = build_context(p, CONTEXT_FLAG_CONTEXT_VALID, 0, CONTEXT_SNAPSHOT_MAX_ZONES);
    uint32_t okc = s_context_frames_ok;
    send_esp(p, n);
    CHECK(s_context_frames_ok == okc + 1, "zone_count == MAX accepted");
}

static void scenario_trip_seq(void)
{
    reset_link_state();
    s_trip_last_seq_seen = 0;
    s_trip_repeats_pending = 0;
    g_have_trip = true;
    g_trip_reason = SAFETY_TRIP_OVERTEMP;

    // seq 1 -> burst armed and first copy sent
    g_trip_seq = 1;
    g_tick_ms += 1000;
    int sends = g_sends;
    link_task_poll_trip_event(g_tick_ms);
    CHECK(g_sends == sends + 1, "first copy sent on new trip_seq");
    CHECK(s_trip_repeats_pending == LINK_TRIP_REPEAT_COUNT - 1u, "burst armed: %u", (unsigned)s_trip_repeats_pending);
    CHECK(s_pending_trip.trip_reason == (uint8_t)SAFETY_TRIP_OVERTEMP, "reason carried");

    // drain the burst
    for (int i = 0; i < 20; i++) {
        g_tick_ms += LINK_TRIP_REPEAT_PERIOD_MS;
        link_task_poll_trip_event(g_tick_ms);
    }
    CHECK(s_trip_repeats_pending == 0, "burst drains");
    int after_burst = g_sends;

    // same seq again: not a new trip
    g_tick_ms += 1000;
    link_task_poll_trip_event(g_tick_ms);
    CHECK(g_sends == after_burst && s_trip_repeats_pending == 0, "repeated trip_seq starts no new burst");

    // walk the seq through the 255 -> 1 wrap: each step is a new burst
    uint8_t seq = 1;
    int bursts = 0;
    for (int step = 0; step < 300; step++) {
        seq = link_frame_next_trip_seq(seq);
        CHECK(seq != 0, "trip_seq never 0 (step %d)", step);
        g_trip_seq = seq;
        g_tick_ms += 1000;
        int s0 = g_sends;
        link_task_poll_trip_event(g_tick_ms);
        if (g_sends == s0 + 1) { bursts++; }
        for (int i = 0; i < 6; i++) {
            g_tick_ms += LINK_TRIP_REPEAT_PERIOD_MS;
            link_task_poll_trip_event(g_tick_ms);
        }
        CHECK(s_trip_repeats_pending == 0, "burst drained at step %d", step);
    }
    CHECK(bursts == 300, "each of 300 consecutive seqs (incl. 255->1 wrap) started a burst: %d", bursts);

    // no trip -> no new frame
    g_have_trip = false;
    g_trip_seq = 77;
    int s1 = g_sends;
    g_tick_ms += 1000;
    link_task_poll_trip_event(g_tick_ms);
    CHECK(g_sends == s1, "no trip, no frame");
}

static void scenario_unknown_commands(void)
{
    // Every command id with a spread of payload lengths from an ESP->Pico
    // BROADCAST: nothing but a well-formed cmd 0x02 may ever grant heat.
    reset_link_state();
    for (int cmd = 0; cmd < 256; cmd++) {
        for (int pass = 0; pass < 6; pass++) {
            uint8_t len;
            switch (pass) {
            case 0: len = 1; break;
            case 1: len = 2; break;
            case 2: len = 15; break;
            case 3: len = 29; break;
            case 4: len = (uint8_t)(1 + rnd() % 254); break;
            default: len = 255; break;
            }
            uint8_t p[255];
            for (int i = 0; i < len; i++) { p[i] = (uint8_t)rnd(); }
            p[0] = (uint8_t)cmd;
            if (cmd == LINK_FRAME_REQUEST_ENABLE_CMD && len == 2) { p[1] = 0; }
            reset_counts();
            send_esp(p, len);
            CHECK(g_enable_true == 0, "cmd 0x%02X len %u granted heat", cmd, (unsigned)len);
        }
    }
}

// --- seeded fuzzer ---------------------------------------------------------------

#define FUZZ_ITERATIONS 30000

static void scenario_fuzz(void)
{
    reset_link_state();
    int expected_grants = 0, actual_grants = 0, valid_enable_frames = 0;

    for (int it = 0; it < FUZZ_ITERATIONS; it++) {
        uint32_t kind = rnd() % 8u;
        uint8_t buf[KILNLINK_FRAME_STUFFED_MAX + 64];
        size_t bn = 0;
        int grants_expected = 0;
        g_tick_ms += (rnd() % 7000u == 0u) ? 6000u : 10u;

        switch (kind) {
        case 0: { // pure random bytes (may contain delimiters)
            bn = rnd() % 700u;
            if (bn > sizeof(buf)) { bn = sizeof(buf); }
            for (size_t i = 0; i < bn; i++) { buf[i] = (uint8_t)rnd(); }
            break;
        }
        case 1: { // random bytes inside delimiters
            bn = 2u + rnd() % 300u;
            for (size_t i = 0; i < bn; i++) { buf[i] = (uint8_t)rnd(); }
            buf[0] = 0x7E;
            buf[bn - 1] = 0x7E;
            break;
        }
        case 2: { // valid frame with a random bit flipped (unstuffed domain)
            uint8_t p[2] = { LINK_FRAME_REQUEST_ENABLE_CMD, (uint8_t)(rnd() & 1u) };
            uint8_t raw[KILNLINK_FRAME_RAW_MAX];
            size_t rl = make_raw(raw, KILNLINK_MSG_BROADCAST, 0, 2, p, 2, (uint16_t)rnd());
            raw[rnd() % rl] ^= (uint8_t)(1u << (rnd() % 8u));
            bn = kilnlink_stuff(raw, rl, buf, sizeof(buf));
            break;
        }
        case 3: { // valid frame cut short or extended with junk
            uint8_t p[2] = { LINK_FRAME_REQUEST_ENABLE_CMD, 1 };
            uint8_t raw[KILNLINK_FRAME_RAW_MAX];
            size_t rl = make_raw(raw, KILNLINK_MSG_BROADCAST, 0, 2, p, 2, (uint16_t)rnd());
            size_t nl = rnd() % (rl + 6u);
            if (nl == rl) { nl = rl - 1u; }
            for (size_t i = rl; i < nl; i++) { raw[i] = (uint8_t)rnd(); }
            bn = kilnlink_stuff(raw, nl, buf, sizeof(buf));
            break;
        }
        case 4: { // valid CRC, random header and payload (random direction/type/cmd/length)
            uint8_t len = (uint8_t)(rnd() % 256u);
            uint8_t p[255];
            for (int i = 0; i < 255; i++) { p[i] = (uint8_t)rnd(); }
            kilnlink_msg_type_t type = (rnd() & 1u) ? KILNLINK_MSG_BROADCAST : (kilnlink_msg_type_t)(1u + rnd() % 4u);
            uint8_t src = (rnd() % 3u == 0u) ? (uint8_t)rnd() : 0;
            uint8_t dst = (rnd() % 3u == 0u) ? (uint8_t)rnd() : 2;
            if (len > 0 && rnd() % 2u) { p[0] = LINK_FRAME_REQUEST_ENABLE_CMD; }
            bool is_grant = (type == KILNLINK_MSG_BROADCAST && src == 0 && dst == 2 && len == 2 &&
                             p[0] == LINK_FRAME_REQUEST_ENABLE_CMD && p[1] != 0);
            if (is_grant) { grants_expected = 1; }
            bn = make_stuffed(buf, sizeof(buf), type, src, dst, p, len, (uint16_t)rnd());
            break;
        }
        case 5: { // PUSH_CONTEXT with random zone count/flags/boot id and random length
            uint8_t p[15 + 14 * 12 + 8];
            for (size_t i = 0; i < sizeof(p); i++) { p[i] = (uint8_t)rnd(); }
            p[0] = LINK_FRAME_PUSH_CONTEXT_CMD;
            uint8_t zones = (uint8_t)(rnd() % 20u);
            p[14] = zones;
            uint8_t len = (rnd() % 4u == 0u) ? (uint8_t)(rnd() % sizeof(p)) : (uint8_t)(15u + (zones <= 12u ? zones : 0u) * 14u);
            bn = make_stuffed(buf, sizeof(buf), KILNLINK_MSG_BROADCAST, 0, 2, p, len, (uint16_t)rnd());
            break;
        }
        case 6: { // a genuine enable frame (control: the fuzzer must see grants too)
            uint8_t p[2] = { LINK_FRAME_REQUEST_ENABLE_CMD, (uint8_t)(rnd() % 3u) };
            grants_expected = (p[1] != 0) ? 1 : 0;
            bn = make_stuffed(buf, sizeof(buf), KILNLINK_MSG_BROADCAST, 0, 2, p, 2, (uint16_t)rnd());
            if (grants_expected) { valid_enable_frames++; }
            break;
        }
        default: { // oversized: long run with no delimiter
            size_t run = (size_t)LINK_RX_ASSEMBLY_MAX + rnd() % 200u;
            if (run > sizeof(buf)) { run = sizeof(buf); }
            for (size_t i = 0; i < run; i++) {
                buf[i] = (uint8_t)rnd();
                if (buf[i] == 0x7Eu) { buf[i] = 0x01; }
            }
            bn = run;
            break;
        }
        }

        int g0 = g_enable_true;
        feed(buf, bn);
        // flush any half-assembled data so a leftover tail cannot combine
        // with the next iteration's bytes into an unaccounted frame
        link_task_rx_process_byte(0x7E);
        link_task_rx_process_byte(0x7E);
        actual_grants += g_enable_true - g0;
        expected_grants += grants_expected;
        if (g_enable_true - g0 != grants_expected) {
            CHECK(0, "iteration %d kind %u: grants %d, expected %d", it, (unsigned)kind, g_enable_true - g0, grants_expected);
            break;
        }
        CHECK(s_rx_assembly_len <= LINK_RX_ASSEMBLY_MAX, "assembly bound held");
    }
    CHECK(actual_grants == expected_grants, "fuzz grants %d == expected %d", actual_grants, expected_grants);
    CHECK(valid_enable_frames > 100, "fuzzer exercised real enable frames: %d", valid_enable_frames);
    printf("fuzz: %d iterations, %d grants (all on well-formed enable frames), %d sends, %d logs, %d cfg-writes attempted\n",
           FUZZ_ITERATIONS, actual_grants, g_sends, g_logs, g_cfg_writes);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("-> enable_gating\n");
    scenario_enable_gating();
    printf("-> bad_crc_and_truncation\n");
    scenario_bad_crc_and_truncation();
    printf("-> resync\n");
    scenario_resync();
    printf("-> push_context\n");
    scenario_push_context();
    printf("-> trip_seq\n");
    scenario_trip_seq();
    printf("-> unknown_commands\n");
    scenario_unknown_commands();
    printf("-> fuzz\n");
    scenario_fuzz();
    printf("test_link_task_fuzz: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
