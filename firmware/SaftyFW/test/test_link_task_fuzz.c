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

#include "trip_seq.h"
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

// Last frame handed to the UART (stuffed bytes), for scenarios that decode the reply.
static uint8_t g_last_tx[KILNLINK_FRAME_STUFFED_MAX];
static size_t g_last_tx_len;
bool uart_owner_send(const uint8_t *d, size_t n)
{
    g_sends++;
    g_last_tx_len = (n <= sizeof(g_last_tx)) ? n : 0;
    if (g_last_tx_len) { memcpy(g_last_tx, d, n); }
    return true;
}
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
static uint8_t g_pers_tc, g_cur_tc;
uint8_t config_store_get_persisted_tc_type(void) { return g_pers_tc; }
uint32_t config_store_get_ram_integrity_fail_count(void) { return 0; }
uint8_t config_store_get_tc_type(void) { return g_cur_tc; }
bool config_store_is_abs_max_temp_disabled(void) { return false; }
bool config_store_is_calibration_missing(void) { return false; }
bool config_store_is_rate_guard_disabled(void) { return false; }
bool config_store_is_volatile_dirty(void) { return false; }
void config_store_set_heat_possible_probe(config_store_heat_possible_probe_t p) { (void)p; }
static bool g_cw_ret;
static int g_cw_calls;
static config_store_record_t g_cw_rec;
bool config_store_write(const config_store_record_t *r, const char **why)
{
    g_cfg_writes++; g_cw_calls++; g_cw_rec = *r; if (why) { *why = "fake"; } return g_cw_ret;
}
// Controllable write fakes: the commit scenarios choose the outcome and read
// back the exact candidate record the handler built.
static bool g_wex_ret, g_wvol_ret;
static config_store_write_decision_t g_wex_decision;
static int g_wex_calls, g_wvol_calls, g_wex_heat_safe;
static config_store_record_t g_last_cand;
bool config_store_write_ex(const config_store_record_t *r, bool heat_safe, const char **why,
                            config_store_write_decision_t *d)
{
    g_cfg_writes++; g_wex_calls++; g_wex_heat_safe = heat_safe ? 1 : 0; g_last_cand = *r;
    if (why) { *why = "fake"; }
    if (d) { *d = g_wex_decision; }
    return g_wex_ret;
}
bool config_store_write_volatile(const config_store_record_t *r, const char **why)
{
    g_cfg_writes++; g_wvol_calls++; g_last_cand = *r; if (why) { *why = "fake"; } return g_wvol_ret;
}

static bool g_any_current_present;
bool current_task_any_current_present(void) { return g_any_current_present; }
bool current_task_ct_auto_zero_begin(uint8_t ch) { (void)ch; return false; }
void current_task_ct_auto_zero_poll(current_task_auto_zero_status_t *o) { memset(o, 0, sizeof(*o)); }
void current_task_get_power(current_sense_power_t *o) { memset(o, 0, sizeof(*o)); }
void current_task_get_snapshot(current_snapshot_t *o) { memset(o, 0, sizeof(*o)); }
static int g_reload_cal;
void current_task_reload_cal(void) { g_reload_cal++; }
static int g_reload_ct;
void current_task_reload_ct_cal(void) { g_reload_ct++; }
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
static int g_tc_reapply;
void thermo_task_request_tc_type_reapply(void) { g_tc_reapply++; }

bool update_task_get_active_slot(bool *b) { *b = false; return false; }
static int g_upd_calls[4];
static uint8_t g_upd_last_len[4];
static uint8_t g_upd_last_cmd[4];
static void upd_rec(int i, const uint8_t *p, uint8_t n)
{
    g_upd_calls[i]++;
    g_upd_last_len[i] = n;
    g_upd_last_cmd[i] = (n > 0 && p) ? p[0] : 0xEE;
}
void update_task_handle_abort(const uint8_t *p, uint8_t n) { upd_rec(3, p, n); }
void update_task_handle_begin(const uint8_t *p, uint8_t n) { upd_rec(0, p, n); }
void update_task_handle_data(const uint8_t *p, uint8_t n) { upd_rec(1, p, n); }
void update_task_handle_end(const uint8_t *p, uint8_t n) { upd_rec(2, p, n); }
static bool g_reboot_allowed;
static uint8_t g_reboot_code;
bool update_task_reboot_allowed(const char **why, uint8_t *code) { *why = "fake"; *code = g_reboot_code; return g_reboot_allowed; }
void update_task_reboot_now(void) { g_reboots++; }
static uint8_t g_rb_code;
static bool g_rb_accept;
bool update_task_request_rollback(const char **why, uint8_t *code) { *why = "fake"; *code = g_rb_code; return g_rb_accept; }

// --- harness ---------------------------------------------------------------------

static int g_checks, g_fail;
#define CHECK(c, ...)                                                                  \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(c)) {                                                                    \
            g_fail++;                                                                  \
            printf("FAIL test_link_task_fuzz.c:%d: ", __LINE__);                                        \
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
    s_context_lock = (SemaphoreHandle_t)1; /* publish needs a lock; without it malformed-never-overwrites is vacuous (A-LOW-2) */
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
    // Distinctive last-good snapshot so a published malformed frame (zeros / partial fields) cannot compare equal (A-LOW-2).
    g_tick_ms += 100;
    n = build_context(p, CONTEXT_FLAG_CONTEXT_VALID | CONTEXT_FLAG_HEAT_OWNER_ACTIVE, 77, 1);
    send_esp(p, n);
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
    CHECK(had && have && before.boot_id == 77 && memcmp(&before, &after, sizeof(before)) == 0 &&
              after.flags == before.flags,
          "malformed contexts never overwrite the last good snapshot");
    CHECK(g_enable_true == 0 && g_enable_false == 0, "malformed context has no grant side effect");

    // Max zone_count frame is accepted at exactly MAX.
    reset_counts();
    n = build_context(p, CONTEXT_FLAG_CONTEXT_VALID, 0, CONTEXT_SNAPSHOT_MAX_ZONES);
    uint32_t okc = s_context_frames_ok;
    send_esp(p, n);
    CHECK(s_context_frames_ok == okc + 1, "zone_count == MAX accepted");
    s_context_lock = NULL; /* do not leak the fake lock into later scenarios */
    s_context_published = false;
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
        seq = trip_seq_next(seq);
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


// --- campaign 2 extension: SET_PARAM range refusal + update command routing ----

static void send_set_param(uint16_t id, uint8_t type, const uint8_t *val, uint8_t vlen)
{
    uint8_t p[8];
    p[0] = KILNLINK_SET_PARAM_CMD;
    p[1] = (uint8_t)(id & 0xFFu);
    p[2] = (uint8_t)(id >> 8);
    p[3] = type;
    memcpy(&p[4], val, vlen);
    send_esp(p, (uint8_t)(4u + vlen));
}

static void scenario_set_param_refusals(void)
{
    reset_link_state();
    link_staging_reset(&s_staging);
    g_cfg_writes = 0;
    uint8_t v;

    v = 8; // tc_type max is 7
    send_set_param(0x0105u, KILNLINK_PARAM_TYPE_U8, &v, 1);
    CHECK(link_staging_count(&s_staging) == 0, "tc_type 8 (out of range) staged");
    v = 255;
    send_set_param(0x0105u, KILNLINK_PARAM_TYPE_U8, &v, 1);
    CHECK(link_staging_count(&s_staging) == 0, "tc_type 255 staged");
    v = 7;
    send_set_param(0x0105u, KILNLINK_PARAM_TYPE_U8, &v, 1);
    CHECK(link_staging_count(&s_staging) == 1, "tc_type 7 (in range) must stage, count=%u", (unsigned)link_staging_count(&s_staging));

    v = 200; // tc_placement_mode above its max
    send_set_param(0x0103u, KILNLINK_PARAM_TYPE_U8, &v, 1);
    CHECK(link_staging_count(&s_staging) == 1, "tc_placement_mode 200 staged");

    uint8_t two[2] = { 5, 0 }; // wrong wire type for a real id
    send_set_param(0x0105u, KILNLINK_PARAM_TYPE_U16, two, 2);
    CHECK(link_staging_count(&s_staging) == 1, "tc_type as U16 staged");

    v = 1; // unknown ids
    send_set_param(0x7777u, KILNLINK_PARAM_TYPE_U8, &v, 1);
    send_set_param(0x0000u, KILNLINK_PARAM_TYPE_U8, &v, 1);
    send_set_param(0xFFFFu, KILNLINK_PARAM_TYPE_U8, &v, 1);
    CHECK(link_staging_count(&s_staging) == 1, "unknown param ids staged");

    // abs_max_temp_c: NaN, negative, zero, -inf refused; positive finite accepted
    uint32_t bad_bits[4] = { 0x7FC00000u, 0xBF800000u, 0x00000000u, 0xFF800000u };
    for (int i = 0; i < 4; i++) {
        uint8_t b[4];
        memcpy(b, &bad_bits[i], 4);
        send_set_param(0x0104u, KILNLINK_PARAM_TYPE_F32, b, 4);
    }
    CHECK(link_staging_count(&s_staging) == 1, "abs_max_temp_c non-positive/NaN staged, count=%u", (unsigned)link_staging_count(&s_staging));
    float good = 1300.0f;
    uint8_t gb[4];
    memcpy(gb, &good, 4);
    send_set_param(0x0104u, KILNLINK_PARAM_TYPE_F32, gb, 4);
    CHECK(link_staging_count(&s_staging) == 2, "abs_max_temp_c 1300 must stage");

    uint8_t mal[6] = { KILNLINK_SET_PARAM_CMD, 0x05, 0x01, KILNLINK_PARAM_TYPE_U8, 1, 1 };
    send_esp(mal, 6);
    send_esp(mal, 3);
    mal[3] = 0x77;
    send_esp(mal, 5);
    CHECK(link_staging_count(&s_staging) == 2, "malformed SET_PARAM staged");

    uint8_t p[5] = { KILNLINK_SET_PARAM_CMD, 0x05, 0x01, KILNLINK_PARAM_TYPE_U8, 3 };
    link_staging_reset(&s_staging);
    send_frame(KILNLINK_MSG_BROADCAST, LINK_FRAME_DEVICE_SAFETY, LINK_FRAME_DEVICE_ESP, p, 5);
    send_frame(KILNLINK_MSG_BROADCAST, LINK_FRAME_DEVICE_ESP, LINK_FRAME_DEVICE_ESP, p, 5);
    CHECK(link_staging_count(&s_staging) == 0, "wrong-direction SET_PARAM staged");

    uint8_t st[KILNLINK_FRAME_STUFFED_MAX];
    size_t sn = make_stuffed(st, sizeof(st), KILNLINK_MSG_BROADCAST, 0, 2, p, 5, g_msg_index++);
    st[sn / 2] ^= 0x01;
    feed(st, sn);
    CHECK(link_staging_count(&s_staging) == 0, "bad-CRC SET_PARAM staged");

    CHECK(g_cfg_writes == 0, "SET_PARAM must never reach config_store_write, writes=%d", g_cfg_writes);
    CHECK(g_enable_true == 0, "SET_PARAM granted heat");
}

static void scenario_update_routing(void)
{
    static const uint8_t cmds[4] = { LINK_FRAME_UPDATE_BEGIN_CMD, LINK_FRAME_UPDATE_DATA_CMD,
                                     LINK_FRAME_UPDATE_END_CMD, LINK_FRAME_UPDATE_ABORT_CMD };
    reset_link_state();
    memset(g_upd_calls, 0, sizeof(g_upd_calls));
    uint8_t p[40];
    memset(p, 0xA5, sizeof(p));
    for (int i = 0; i < 4; i++) {
        p[0] = cmds[i];
        send_esp(p, 40);
        for (int j = 0; j < 4; j++) {
            CHECK(g_upd_calls[j] == (j <= i ? 1 : 0), "cmd 0x%02X: handler %d calls=%d", cmds[i], j, g_upd_calls[j]);
        }
        CHECK(g_upd_last_len[i] == 40 && g_upd_last_cmd[i] == cmds[i], "handler %d got len=%u cmd=0x%02X (payload incl. cmd byte)",
              i, (unsigned)g_upd_last_len[i], g_upd_last_cmd[i]);
    }
    CHECK(g_enable_true == 0 && g_reboots == 0, "update commands granted heat / rebooted");

    memset(g_upd_calls, 0, sizeof(g_upd_calls));
    for (int i = 0; i < 4; i++) {
        p[0] = cmds[i];
        send_frame(KILNLINK_MSG_BROADCAST, LINK_FRAME_DEVICE_SAFETY, LINK_FRAME_DEVICE_ESP, p, 10);
        send_frame(KILNLINK_MSG_BROADCAST, LINK_FRAME_DEVICE_ESP, LINK_FRAME_DEVICE_ESP, p, 10);
        uint8_t st[KILNLINK_FRAME_STUFFED_MAX];
        size_t sn = make_stuffed(st, sizeof(st), KILNLINK_MSG_BROADCAST, 0, 2, p, 10, g_msg_index++);
        st[sn / 2] ^= 0x04;
        feed(st, sn);
    }
    CHECK(g_upd_calls[0] + g_upd_calls[1] + g_upd_calls[2] + g_upd_calls[3] == 0,
          "update handlers reached by wrong-direction/bad-CRC frames");
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


// LOW-1 (REVIEW_SAFTYFW_SINCE_PRE2_2026-10-10): pins CURRENT, deliberately
// conservative behaviour. The heat-possible probe that gates
// config_store_write_volatile() counts current_task_any_current_present() as
// "heat possible". On a fitted-but-uncalibrated CT the op-amp offset floor reads
// as present, so the probe stays true and a non-tightening volatile install
// (k_ct_v_per_a, abs_max raise) is refused until the CT is committed while
// disarmed. If this test fails because the probe was loosened, that is a safety
// semantics change that needs an owner decision, not a test fix.
static void scenario_heat_probe_current_floor(void)
{
    reset_link_state();
    uint8_t p[15 + 14 * 12];
    s_context_lock = (SemaphoreHandle_t)1; /* task init is not run here; the gate needs a lock to read the snapshot */
    s_degraded_no_context = false;
    uint8_t n = build_context(p, CONTEXT_FLAG_CONTEXT_VALID, 40, 1);
    g_any_current_present = false;
    send_esp(p, n);
    CHECK(!link_task_heat_possible_probe(), "fresh idle context, no current: heat not possible (baseline)");
    g_any_current_present = true; /* uncalibrated CT offset floor reads as present */
    CHECK(link_task_heat_possible_probe(),
          "current present (e.g. uncalibrated CT offset floor) => heat possible, fail closed (LOW-1, owner decision pending)");
    g_any_current_present = false;
    s_context_lock = NULL; /* A-LOW-1: restore so later scenarios see the untouched state */
    s_context_published = false;
}

// --- R2-B: COMMIT_CONFIG / APPLY_CONFIG_VOLATILE handlers ------------------------

// Decodes the last UART frame into a COMMIT_CONFIG_REJECTED (0x20) payload.
static bool last_tx_rejected(uint16_t *param_id, uint8_t *reason)
{
    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t st;
    size_t rl = kilnlink_unstuff(g_last_tx, g_last_tx_len, raw, sizeof(raw), &st);
    if (rl == 0) { return false; }
    kilnlink_frame_t f;
    if (kilnlink_frame_decode(raw, rl, &f) != KILNLINK_FRAME_OK) { return false; }
    kilnlink_commit_config_rejected_t m;
    kilnlink_commit_config_rejected_status_t ds = kilnlink_commit_config_rejected_decode(f.payload, f.length, &m);
    if (ds != KILNLINK_COMMIT_CONFIG_REJECTED_OK) { return false; }
    *param_id = m.param_id;
    *reason = m.reason;
    return true;
}

static void commit_reset(void)
{
    reset_link_state();
    link_staging_reset(&s_staging);
    g_wex_ret = g_wvol_ret = false;
    g_wex_decision = CONFIG_STORE_WRITE_OK;
    g_wex_calls = g_wvol_calls = g_reload_cal = g_tc_reapply = 0;
    g_pers_tc = g_cur_tc = 0;
    g_last_tx_len = 0;
    s_tc_type_reapply_pending = false;
    memset(&g_last_cand, 0, sizeof(g_last_cand));
}

static void stage_u8(uint16_t id, uint8_t v) { send_set_param(id, KILNLINK_PARAM_TYPE_U8, &v, 1); }
static void send_cmd1(uint8_t cmd) { send_esp(&cmd, 1); }

static void scenario_commit_config(void)
{
    uint16_t pid = 0;
    uint8_t reason = 0xEE;

    // 1. Contradictory pair (BORROWED_ZONE + EXTERNAL_OVERHEAT): refused before
    //    any write, names tc_placement_mode (0x0103), CONTRADICTION, staging kept.
    commit_reset();
    stage_u8(0x0101u, CONFIG_STORE_TC_SOURCE_BORROWED_ZONE);
    stage_u8(0x0103u, CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT);
    CHECK(link_staging_count(&s_staging) == 2, "contradiction setup staged %u", (unsigned)link_staging_count(&s_staging));
    g_sends = 0;
    send_cmd1(KILNLINK_COMMIT_CONFIG_CMD);
    CHECK(g_wex_calls == 0, "contradiction must not reach config_store_write_ex, calls=%d", g_wex_calls);
    CHECK(g_sends == 1, "one rejection frame sent, sends=%d", g_sends);
    CHECK(last_tx_rejected(&pid, &reason), "rejection frame decodes");
    CHECK(pid == 0x0103u, "contradiction names tc_placement_mode 0x0103, got 0x%04X", (unsigned)pid);
    CHECK(reason == KILNLINK_COMMIT_CONFIG_REJECT_CONTRADICTION, "reason CONTRADICTION, got %u", (unsigned)reason);
    CHECK(link_staging_count(&s_staging) == 2, "staging kept after validation refusal, count=%u", (unsigned)link_staging_count(&s_staging));
    CHECK(g_reload_cal == 0 && g_tc_reapply == 0, "no reload/reapply on refusal");

    // 1b. abs_max_temp_c above the Type T ceiling: names abs_max_temp_c (0x0104).
    commit_reset();
    stage_u8(0x0105u, 7);
    {
        float f = 1500.0f;
        uint8_t b[4];
        memcpy(b, &f, 4);
        send_set_param(0x0104u, KILNLINK_PARAM_TYPE_F32, b, 4);
    }
    g_sends = 0;
    send_cmd1(KILNLINK_COMMIT_CONFIG_CMD);
    CHECK(g_wex_calls == 0, "abs_max vs tc_type contradiction must not write");
    CHECK(last_tx_rejected(&pid, &reason), "abs_max rejection decodes");
    CHECK(pid == 0x0104u && reason == KILNLINK_COMMIT_CONFIG_REJECT_CONTRADICTION,
          "abs_max contradiction pid=0x%04X reason=%u", (unsigned)pid, (unsigned)reason);

    // 2. Refusal decisions from the write map onto the wire reason and keep staging.
    struct { config_store_write_decision_t d; uint8_t want; const char *name; } cases[] = {
        { CONFIG_STORE_WRITE_REFUSED_ARMED, KILNLINK_COMMIT_CONFIG_REJECT_ARMED, "ARMED" },
        { CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_ON, KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_ON, "HEAT_ON" },
        { CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_UNKNOWN, KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_UNKNOWN, "HEAT_UNKNOWN" },
        { CONFIG_STORE_WRITE_FLASH_FAILURE, KILNLINK_COMMIT_CONFIG_REJECT_STORAGE, "STORAGE" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        commit_reset();
        stage_u8(0x0105u, 3);
        g_wex_ret = false;
        g_wex_decision = cases[i].d;
        g_pers_tc = 3; // persisted == candidate, so ARMED is not promoted to MIXED
        g_sends = 0;
        send_cmd1(KILNLINK_COMMIT_CONFIG_CMD);
        CHECK(g_wex_calls == 1, "%s: one write attempt, calls=%d", cases[i].name, g_wex_calls);
        CHECK(g_sends == 1 && last_tx_rejected(&pid, &reason), "%s: rejection frame", cases[i].name);
        CHECK(pid == KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID, "%s: no-param sentinel, got 0x%04X", cases[i].name, (unsigned)pid);
        CHECK(reason == cases[i].want, "%s: reason want %u got %u", cases[i].name, (unsigned)cases[i].want, (unsigned)reason);
        CHECK(link_staging_count(&s_staging) == 1, "%s: staging kept for retry, count=%u", cases[i].name, (unsigned)link_staging_count(&s_staging));
        CHECK(g_reload_cal == 0 && g_tc_reapply == 0, "%s: no reload/reapply on refusal", cases[i].name);
    }

    // 2b. ARMED with a tc_type that differs from the PERSISTED one is MIXED.
    commit_reset();
    stage_u8(0x0105u, 3);
    g_wex_decision = CONFIG_STORE_WRITE_REFUSED_ARMED;
    g_pers_tc = 5;
    send_cmd1(KILNLINK_COMMIT_CONFIG_CMD);
    CHECK(last_tx_rejected(&pid, &reason) && reason == KILNLINK_COMMIT_CONFIG_REJECT_ARMED_MIXED,
          "ARMED with tc_type differing from persisted maps to MIXED, got %u", (unsigned)reason);

    // 3. Accepted, tc_type unchanged: staging reset, cal reloaded, no reapply, no frame.
    commit_reset();
    stage_u8(0x0105u, 0); // candidate tc_type 0 == current 0
    g_wex_ret = true;
    g_sends = 0;
    send_cmd1(KILNLINK_COMMIT_CONFIG_CMD);
    CHECK(g_wex_calls == 1, "accepted: one write, calls=%d", g_wex_calls);
    CHECK(g_sends == 0, "accepted commit sends no rejection frame, sends=%d", g_sends);
    CHECK(link_staging_count(&s_staging) == 0, "accepted commit resets staging, count=%u", (unsigned)link_staging_count(&s_staging));
    CHECK(g_reload_cal == 1, "accepted commit reloads CT cal exactly once, got %d", g_reload_cal);
    CHECK(g_tc_reapply == 0 && !s_tc_type_reapply_pending, "unchanged tc_type: no reapply, no pending");
    CHECK(g_last_cand.tc_type == 0, "candidate carries staged tc_type");
    CHECK(g_last_cand.calibration_missing, "required no-safe-default fields unset: calibration_missing stays true");

    // 4. Accepted with tc_type change while heat is NOT provably safe (no context
    //    ever received): reapply is deferred and the pending retry is armed.
    commit_reset();
    stage_u8(0x0105u, 4);
    g_cur_tc = 0;
    g_wex_ret = true;
    send_cmd1(KILNLINK_COMMIT_CONFIG_CMD);
    CHECK(g_wex_heat_safe == 0, "no context: heat not provably safe");
    CHECK(g_tc_reapply == 0, "heat unsafe: immediate reapply skipped, got %d", g_tc_reapply);
    CHECK(s_tc_type_reapply_pending && s_tc_type_reapply_pending_value == 4,
          "heat unsafe: retry armed for tc_type 4 (pending=%d value=%u)", (int)s_tc_type_reapply_pending,
          (unsigned)s_tc_type_reapply_pending_value);
    CHECK(g_reload_cal == 1, "heat unsafe: still reloads cal, got %d", g_reload_cal);
    CHECK(link_staging_count(&s_staging) == 0, "heat unsafe: staging reset after accepted write");

    // 4b. Accepted with tc_type change while heat IS provably safe (fresh idle
    //     context, no current): immediate reapply, nothing armed.
    commit_reset();
    s_context_lock = (SemaphoreHandle_t)1;
    s_degraded_no_context = false;
    g_any_current_present = false;
    {
        uint8_t cp[15 + 14 * 12];
        uint8_t cn = build_context(cp, CONTEXT_FLAG_CONTEXT_VALID, 40, 1);
        send_esp(cp, cn);
    }
    stage_u8(0x0105u, 4);
    g_cur_tc = 0;
    g_wex_ret = true;
    send_cmd1(KILNLINK_COMMIT_CONFIG_CMD);
    CHECK(g_wex_heat_safe == 1, "fresh idle context: heat provably safe");
    CHECK(g_tc_reapply == 1, "heat safe: immediate reapply requested once, got %d", g_tc_reapply);
    CHECK(!s_tc_type_reapply_pending, "heat safe: no retry armed");
    CHECK(g_reload_cal == 1, "still reloads cal, got %d", g_reload_cal);
    CHECK(link_staging_count(&s_staging) == 0, "staging reset after accepted write");

    // 5. Malformed COMMIT_CONFIG (extra byte) is dropped silently: no write, no frame.
    commit_reset();
    stage_u8(0x0105u, 3);
    g_sends = 0;
    {
        uint8_t bad[2] = { KILNLINK_COMMIT_CONFIG_CMD, 0 };
        send_esp(bad, 2);
    }
    CHECK(g_wex_calls == 0 && g_sends == 0, "malformed commit ignored: writes=%d sends=%d", g_wex_calls, g_sends);
    CHECK(link_staging_count(&s_staging) == 1, "malformed commit leaves staging");
}

static void scenario_apply_config_volatile(void)
{
    uint16_t pid = 0;
    uint8_t reason = 0xEE;

    // 1. Validation refusal: same wire mapping, no volatile write, staging kept.
    commit_reset();
    stage_u8(0x0101u, CONFIG_STORE_TC_SOURCE_BORROWED_ZONE);
    stage_u8(0x0103u, CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT);
    g_sends = 0;
    send_cmd1(KILNLINK_APPLY_CONFIG_VOLATILE_CMD);
    CHECK(g_wvol_calls == 0 && g_wex_calls == 0, "volatile contradiction writes nothing");
    CHECK(last_tx_rejected(&pid, &reason), "volatile rejection decodes");
    CHECK(pid == 0x0103u && reason == KILNLINK_COMMIT_CONFIG_REJECT_CONTRADICTION,
          "volatile contradiction pid=0x%04X reason=%u", (unsigned)pid, (unsigned)reason);
    CHECK(link_staging_count(&s_staging) == 2, "volatile refusal keeps staging");

    // 2. Heat-possible refusal from config_store_write_volatile: reported as ARMED.
    commit_reset();
    stage_u8(0x0105u, 3);
    g_wvol_ret = false;
    g_sends = 0;
    send_cmd1(KILNLINK_APPLY_CONFIG_VOLATILE_CMD);
    CHECK(g_wvol_calls == 1 && g_wex_calls == 0, "volatile path calls only write_volatile (vol=%d ex=%d)", g_wvol_calls, g_wex_calls);
    CHECK(last_tx_rejected(&pid, &reason), "volatile heat refusal frame");
    CHECK(pid == KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID && reason == KILNLINK_COMMIT_CONFIG_REJECT_ARMED,
          "volatile refusal pid=0x%04X reason=%u", (unsigned)pid, (unsigned)reason);
    CHECK(link_staging_count(&s_staging) == 1, "volatile refusal keeps staging");
    CHECK(g_reload_cal == 0 && g_tc_reapply == 0, "volatile refusal: no reload/reapply");

    // 3. Accepted, tc_type unchanged: staging reset, cal reloaded, no frame.
    commit_reset();
    stage_u8(0x0105u, 0);
    g_wvol_ret = true;
    g_sends = 0;
    send_cmd1(KILNLINK_APPLY_CONFIG_VOLATILE_CMD);
    CHECK(g_wvol_calls == 1, "volatile accepted: one write");
    CHECK(g_sends == 0, "volatile accept sends no frame, sends=%d", g_sends);
    CHECK(link_staging_count(&s_staging) == 0, "volatile accept resets staging");
    CHECK(g_reload_cal == 1, "volatile accept reloads cal once, got %d", g_reload_cal);
    CHECK(g_tc_reapply == 0, "volatile accept, same tc_type: no reapply");
    CHECK(g_last_cand.calibration_missing, "volatile candidate keeps calibration_missing when required fields unset");

    // 4. Accepted with a tc_type change (no context => heat not provably safe).
    commit_reset();
    stage_u8(0x0105u, 6);
    g_wvol_ret = true;
    send_cmd1(KILNLINK_APPLY_CONFIG_VOLATILE_CMD);
    CHECK(g_reload_cal == 1, "volatile tc change reloads cal");
    CHECK(link_staging_count(&s_staging) == 0, "volatile tc change resets staging");
    CHECK(g_tc_reapply == 1 && !s_tc_type_reapply_pending,
          "tc change: volatile path reapplies immediately, never arms the commit retry (reapply=%d pending=%d)",
          g_tc_reapply, (int)s_tc_type_reapply_pending);

    // 5. Malformed frame ignored.
    commit_reset();
    stage_u8(0x0105u, 3);
    g_sends = 0;
    {
        uint8_t bad[3] = { KILNLINK_APPLY_CONFIG_VOLATILE_CMD, 1, 2 };
        send_esp(bad, 3);
    }
    CHECK(g_wvol_calls == 0 && g_sends == 0, "malformed volatile ignored");
}

// --- round 3: SET_CT_CAL, SET_FIRING_CEILING/SET_CLOCK, REBOOT/ROLLBACK ------------

static void send_ct_cal(uint8_t ch, uint8_t calibrated, float gain, float offset)
{
    kilnlink_set_ct_cal_t m = { ch, calibrated, gain, offset };
    uint8_t p[KILNLINK_SET_CT_CAL_LEN];
    kilnlink_set_ct_cal_status_t st;
    size_t n = kilnlink_set_ct_cal_encode(&m, p, sizeof(p), &st);
    send_esp(p, (uint8_t)n);
}

static void scenario_set_ct_cal(void)
{
    commit_reset();
    g_cw_ret = true; g_cw_calls = 0; g_reload_ct = 0;

    // Out-of-range channel: refused before any write.
    send_ct_cal(3, 1, 1.0f, 0.0f);
    send_ct_cal(255, 1, 1.0f, 0.0f);
    CHECK(g_cw_calls == 0 && g_reload_ct == 0, "ct_cal bad channel wrote (%d) reloaded (%d)", g_cw_calls, g_reload_ct);

    // Calibrated with gain 0 would blind S14; NaN/Inf/negative/huge gain; NaN offset.
    send_ct_cal(0, 1, 0.0f, 0.0f);
    send_ct_cal(0, 1, NAN, 0.0f);
    send_ct_cal(0, 1, INFINITY, 0.0f);
    send_ct_cal(0, 1, -1.0f, 0.0f);
    send_ct_cal(0, 1, 1.0e9f, 0.0f);
    send_ct_cal(0, 1, 1.0f, NAN);
    send_ct_cal(0, 1, 1.0f, 1.0e9f);
    CHECK(g_cw_calls == 0 && g_reload_ct == 0, "ct_cal bad values wrote (%d) reloaded (%d)", g_cw_calls, g_reload_ct);

    // Malformed length ignored.
    {
        uint8_t bad[5] = { KILNLINK_SET_CT_CAL_CMD, 0, 1, 0, 0 };
        send_esp(bad, 5);
    }
    CHECK(g_cw_calls == 0, "malformed SET_CT_CAL wrote");

    // Write refused by config_store: no reload.
    g_cw_ret = false;
    send_ct_cal(1, 1, 2.0f, 0.5f);
    CHECK(g_cw_calls == 1, "valid SET_CT_CAL must reach config_store_write once, got %d", g_cw_calls);
    CHECK(g_reload_ct == 0, "refused write must not reload ct cal");

    // Accepted: candidate carries the channel values, reload happens, superseded staged gain dropped.
    link_staging_reset(&s_staging);
    {
        float g = 9.0f;
        uint8_t gb[4];
        memcpy(gb, &g, 4);
        send_set_param(0x0311u, KILNLINK_PARAM_TYPE_F32, gb, 4); // gain ch1 staged
    }
    unsigned before = (unsigned)link_staging_count(&s_staging);
    CHECK(before == 1u, "precondition: gain ch1 staged, count=%u", before);
    g_cw_ret = true; g_cw_calls = 0;
    send_ct_cal(1, 1, 2.0f, 0.5f);
    CHECK(g_cw_calls == 1 && g_reload_ct == 1, "accepted SET_CT_CAL write=%d reload=%d", g_cw_calls, g_reload_ct);
    CHECK(g_cw_rec.ct_cal[1].calibrated && g_cw_rec.ct_cal[1].gain == 2.0f && g_cw_rec.ct_cal[1].offset == 0.5f,
          "candidate ch1 gain=%f off=%f", (double)g_cw_rec.ct_cal[1].gain, (double)g_cw_rec.ct_cal[1].offset);
    CHECK(!g_cw_rec.ct_cal[0].calibrated, "other channel must stay uncalibrated");
    CHECK(link_staging_count(&s_staging) == 0u, "accepted SET_CT_CAL must drop the superseded staged gain, count=%u",
          (unsigned)link_staging_count(&s_staging));
    // mcpfx1 LOW-6: edge-value accepted/refused pairs (positive controls at each bound).
    g_cw_ret = true;
    g_cw_calls = 0;
    send_ct_cal(2, 1, 10.0f, 50.0f);   // top channel, gain max, +offset max: all inclusive
    CHECK(g_cw_calls == 1, "ct_cal edge ch2/gain 10/off +50 accepted, calls=%d", g_cw_calls);
    send_ct_cal(0, 1, 1.0f, -50.0f);   // -offset max inclusive
    CHECK(g_cw_calls == 2, "ct_cal edge off -50 accepted, calls=%d", g_cw_calls);
    send_ct_cal(0, 0, 0.0f, 0.0f);     // uncalibrated gain 0 is allowed
    CHECK(g_cw_calls == 3, "ct_cal uncalibrated gain 0 accepted, calls=%d", g_cw_calls);
    send_ct_cal(0, 1, 10.01f, 0.0f);   // just above gain max
    send_ct_cal(0, 1, 1.0f, 50.01f);   // just above +offset max
    send_ct_cal(0, 1, 1.0f, -50.01f);  // just below -offset max
    CHECK(g_cw_calls == 3, "ct_cal just-out-of-range values refused, calls=%d", g_cw_calls);
    g_cw_ret = false; // LOW-5: do not leak the accepting fake into later scenarios (scenario_fuzz)
}

static void send_ceiling(float v)
{
    kilnlink_ceiling_t m = { v };
    uint8_t p[KILNLINK_CEILING_LEN];
    kilnlink_ceiling_status_t st;
    size_t n = kilnlink_ceiling_encode(&m, p, sizeof(p), &st);
    send_esp(p, (uint8_t)n);
}

static void send_clock(uint64_t ms)
{
    kilnlink_set_clock_t m = { ms };
    uint8_t p[KILNLINK_SET_CLOCK_LEN];
    kilnlink_set_clock_status_t st;
    size_t n = kilnlink_set_clock_encode(&m, p, sizeof(p), &st);
    send_esp(p, (uint8_t)n);
}

static void scenario_ceiling_and_clock(void)
{
    commit_reset();
    s_firing_ceiling_have = false; s_firing_ceiling_c = 0.0f;
    s_wall_clock_have = false; s_wall_clock_epoch_ms = 0;

    send_ceiling(1200.0f);
    CHECK(s_firing_ceiling_have && s_firing_ceiling_c == 1200.0f, "valid ceiling not stored");
    // Each non-active value must CLEAR a previously stored ceiling, never half-accept it.
    float bad[4] = { 0.0f, -50.0f, NAN, INFINITY };
    for (int i = 0; i < 4; i++) {
        send_ceiling(1200.0f);
        send_ceiling(bad[i]);
        CHECK(!s_firing_ceiling_have && s_firing_ceiling_c == 0.0f, "ceiling bad[%d] left have=%d c=%f", i,
              (int)s_firing_ceiling_have, (double)s_firing_ceiling_c);
    }
    send_ceiling(1200.0f);
    send_ceiling(-INFINITY);
    CHECK(!s_firing_ceiling_have, "-inf ceiling accepted");
    send_ceiling(1200.0f);
    {
        uint8_t m[3] = { KILNLINK_CEILING_CMD, 0, 0 };
        send_esp(m, 3);
    }
    CHECK(s_firing_ceiling_have && s_firing_ceiling_c == 1200.0f, "malformed ceiling must be ignored, not clear");

    send_clock(1790000000000ULL);
    CHECK(s_wall_clock_have && s_wall_clock_epoch_ms == 1790000000000ULL, "plausible clock not stored");
    send_clock(0ULL);
    send_clock(1577836799999ULL);
    send_clock(4102444800001ULL);
    send_clock(0xFFFFFFFFFFFFFFFFULL);
    CHECK(s_wall_clock_epoch_ms == 1790000000000ULL, "implausible epoch overwrote the clock: %llu",
          (unsigned long long)s_wall_clock_epoch_ms);
    send_clock(1577836800000ULL);
    CHECK(s_wall_clock_epoch_ms == 1577836800000ULL, "lower bound inclusive");
    send_clock(4102444800000ULL);
    CHECK(s_wall_clock_epoch_ms == 4102444800000ULL, "upper bound inclusive");
}

static bool last_tx_result(uint8_t cmd, uint8_t *accepted, uint8_t *reason)
{
    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t st;
    size_t rl = kilnlink_unstuff(g_last_tx, g_last_tx_len, raw, sizeof(raw), &st);
    if (rl == 0) { return false; }
    kilnlink_frame_t f;
    if (kilnlink_frame_decode(raw, rl, &f) != KILNLINK_FRAME_OK) { return false; }
    if (f.length != 3 || f.payload[0] != cmd) { return false; }
    *accepted = f.payload[1];
    *reason = f.payload[2];
    return true;
}

static void scenario_reboot_rollback(void)
{
    uint8_t acc = 0xEE, rsn = 0xEE;
    const uint16_t saved_announce_version = s_peer_announce.version; // LOW-7: restore, do not force 0

    // REBOOT refused (e.g. armed): reply says refused + reason, chip is NOT reset.
    commit_reset();
    g_reboots = 0;
    g_reboot_allowed = false;
    g_reboot_code = KILNLINK_REBOOT_RESULT_REASON_ARMED;
    g_sends = 0;
    send_cmd1(KILNLINK_REBOOT_CMD);
    CHECK(g_reboots == 0, "refused REBOOT must not reset the chip");
    CHECK(g_sends == 1 && last_tx_result(KILNLINK_REBOOT_RESULT_CMD, &acc, &rsn), "refused REBOOT must reply");
    CHECK(acc == 0 && rsn == KILNLINK_REBOOT_RESULT_REASON_ARMED, "refused reply acc=%u rsn=%u", acc, rsn);

    // REBOOT accepted: accepted=1/reason NONE, then exactly one reset.
    g_reboot_allowed = true;
    g_reboot_code = KILNLINK_REBOOT_RESULT_REASON_ARMED; // must be ignored on accept
    g_sends = 0;
    send_cmd1(KILNLINK_REBOOT_CMD);
    CHECK(g_reboots == 1, "accepted REBOOT must reset exactly once, got %d", g_reboots);
    CHECK(last_tx_result(KILNLINK_REBOOT_RESULT_CMD, &acc, &rsn), "accepted REBOOT must reply");
    CHECK(acc == 1 && rsn == KILNLINK_REBOOT_RESULT_REASON_NONE, "accepted reply acc=%u rsn=%u", acc, rsn);

    // Malformed REBOOT (extra byte): ignored entirely.
    g_reboots = 0; g_sends = 0;
    {
        uint8_t m[2] = { KILNLINK_REBOOT_CMD, 0 };
        send_esp(m, 2);
    }
    CHECK(g_reboots == 0 && g_sends == 0, "malformed REBOOT acted on");

    // ROLLBACK refused: result frame only to a peer that announced protocol >= 9.
    g_rb_code = KILNLINK_ROLLBACK_RESULT_REASON_SLOT_INVALID;
    s_peer_announce.version = 0;
    g_sends = 0;
    send_cmd1(KILNLINK_ROLLBACK_CMD);
    CHECK(g_sends == 0, "rollback result sent to an unannounced peer (skew safety)");
    s_peer_announce.version = (uint16_t)(KILNLINK_ROLLBACK_RESULT_MIN_PROTOCOL - 1u);
    send_cmd1(KILNLINK_ROLLBACK_CMD);
    CHECK(g_sends == 0, "rollback result sent to a protocol-%u peer", (unsigned)s_peer_announce.version);
    s_peer_announce.version = (uint16_t)KILNLINK_ROLLBACK_RESULT_MIN_PROTOCOL;
    send_cmd1(KILNLINK_ROLLBACK_CMD);
    CHECK(g_sends == 1 && last_tx_result(KILNLINK_ROLLBACK_RESULT_CMD, &acc, &rsn), "supported peer must get result");
    CHECK(acc == 0 && rsn == KILNLINK_ROLLBACK_RESULT_REASON_SLOT_INVALID, "rollback reply acc=%u rsn=%u", acc, rsn);

    // LOW-7: accepted path sends NO result frame (the real call would not return; the fake does).
    g_rb_accept = true;
    g_sends = 0;
    send_cmd1(KILNLINK_ROLLBACK_CMD);
    CHECK(g_sends == 0, "accepted ROLLBACK must not send a refusal result, sent %d", g_sends);
    g_rb_accept = false;
    s_peer_announce.version = saved_announce_version;
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
    CHECK(s_context_lock == NULL && !s_context_published, "push_context scenario leaves no fake context lock/publish behind");
    printf("-> heat_probe_current_floor\n");
    scenario_heat_probe_current_floor();
    CHECK(s_context_lock == NULL && !s_context_published, "heat_probe scenario leaves no fake context lock/publish behind (A-LOW-1)");
    printf("-> trip_seq\n");
    scenario_trip_seq();
    printf("-> unknown_commands\n");
    scenario_unknown_commands();
    printf("-> set_param_refusals\n");
    scenario_set_param_refusals();
    printf("-> update_routing\n");
    scenario_update_routing();
    scenario_commit_config();
    scenario_apply_config_volatile();
    scenario_set_ct_cal();
    CHECK(!g_cw_ret, "set_ct_cal scenario leaves the accepting config-write fake reset (LOW-5)");
    scenario_ceiling_and_clock();
    scenario_reboot_rollback();
    printf("-> fuzz\n");
    scenario_fuzz();
    printf("test_link_task_fuzz: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
