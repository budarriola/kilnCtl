// Host tests for App/drivers/bridge/uart_bridge_ext_control.c's CONTROL
// (task 8) handler -- specifically CONTROL_CMD_SET_ZONE_PID/SET_ZONE_MODEL,
// which reach zones_config_set_pid()/set_model() and must be refused while a
// firing or autotune run is active, same as the HTTP side
// (zones_http_post.c's POST /api/zones/pid) per owner decision Q2
// (docs/SYSTEM_MODE_GATE.md, 2026-09-25, gate-slices-2/4/5 spec).
//
// #includes uart_bridge_ext_control.c directly (same convention as
// test_adaptive_tune_http_gate.c/test_adaptive_tune.c) to reach its static
// control_handle_message() -- the only entry point that exercises
// control_zones_write_refused() without a real uart_protocol_t/task/queue
// stack. Own executable (build_host_tests.ps1) since it fakes
// zones_config_*/profiles_http_*/etc with its own tiny in-RAM tables, same
// as every other file that #includes a drivers/*.c file directly.
//
// Links the REAL firmware/KilnFW/App/drivers/safety/system_mode_gate.c --
// the gate decision itself is the thing under test, not something to fake.
//
// uart_bridge_ext_control.c #includes "uart_bridge.h" only to reach
// uart_protocol_t (used opaquely, as control_ctx_t's `proto` pointer -- CONTROL
// task 8 never dereferences its fields directly; every call that does,
// uart_bridge_ext_reply()/_reply_ok_err(), is faked below). uart_bridge.h
// itself also pulls in safety_link.h -> screen_idle.h -> panel_spi.h, which
// needs real ESP-IDF SPI/I2C driver types no host stub in stubs/ covers (no
// other test compiles this chain) -- so rather than write a from-scratch
// panel_spi.h stub for a header this file never actually uses, pre-empt
// UART_BRIDGE_H's include guard here so that #include never happens.
// uart_protocol_t/uart_proto_message_t still get defined -- profile_executor.h
// and zones_config_accessors.h (both included below, and both already needed
// for their own fakes) bare-#include "safety_link.h", which resolves via the
// shared rsp's /I order to stubs/uart_protocol.h -- and that stub's
// uart_proto_message_t (device/task_id/msg_index/length/payload[253]) is
// byte-for-byte the same layout as the real hwAbstraction one this test
// builds wire messages against, so no divergence from the real wire format
// is introduced by using it here.
#define UART_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

// MAX31856_CHANNEL_COUNT / KILN_IO_RELAY_COUNT / zone_control_mode_t /
// profile_exec_state_t -- needed ahead of uart_bridge_ext_control.c's own
// #includes of these headers, same convention test_adaptive_tune.c follows.
#include "../drivers/hw/MAX31856.h"
#include "../drivers/owners/kiln_io.h"
#include "../drivers/control/profile_executor.h"
#include "../drivers/persist/zones_config_accessors.h"
#include "../drivers/persist/profiles_types.h"
#include "../drivers/persist/unit_pref.h"

int g_test_failures = 0;
int g_test_count = 0;

// ---------------------------------------------------------------------
// uart_protocol_t / uart_proto_message_t -- the stub header's shape is the
// exact same layout uart_bridge_ext_control.c's real espInterfaces header
// uses; #include "uart_bridge.h" (below, via the file under test) resolves
// against the real hwAbstraction header instead of the http-tier stub
// (stubs/uart_protocol.h), since this file DOES need real send/receive
// declarations to link against.
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// zones_config_accessors.h fakes -- tiny in-RAM per-zone table, same
// convention as test_adaptive_tune.c's s_fake_zone_cfg.
// ---------------------------------------------------------------------
#define TEST_MAX_ZONES 5
static struct {
    float kp, ki, kd;
    float k_dc, tau_s, dead_time_s;
    uint8_t relay_mask;
    zone_control_mode_t control_mode;
    float max_ramp;
    float max_temp_c, min_temp_c;
} s_fake_zone_cfg[TEST_MAX_ZONES];

static unsigned s_set_pid_calls = 0;
static unsigned s_set_model_calls = 0;

uint8_t zones_config_get_thermo_count(void) { return TEST_MAX_ZONES; }

bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_mask = s_fake_zone_cfg[zone_index].relay_mask;
    return true;
}
bool zones_config_get_control_mode(uint8_t zone_index, zone_control_mode_t *out_mode)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_mode = s_fake_zone_cfg[zone_index].control_mode;
    return true;
}
float zones_config_apply_cal(uint8_t zone_index, float raw_c)
{
    (void)zone_index;
    return raw_c;
}
bool zones_config_get_pid(uint8_t zone_index, float *out_kp, float *out_ki, float *out_kd)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_kp = s_fake_zone_cfg[zone_index].kp;
    *out_ki = s_fake_zone_cfg[zone_index].ki;
    *out_kd = s_fake_zone_cfg[zone_index].kd;
    return true;
}
// Under test (via CONTROL_CMD_SET_ZONE_PID): call-counted so a refused
// write's "never reached the setter" claim is actually verified, not just
// inferred from the reply.
bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    s_set_pid_calls++;
    if (zone_index >= TEST_MAX_ZONES) return false;
    s_fake_zone_cfg[zone_index].kp = kp;
    s_fake_zone_cfg[zone_index].ki = ki;
    s_fake_zone_cfg[zone_index].kd = kd;
    return true;
}
// Under test (via CONTROL_CMD_SET_ZONE_MODEL).
bool zones_config_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s)
{
    s_set_model_calls++;
    if (zone_index >= TEST_MAX_ZONES) return false;
    s_fake_zone_cfg[zone_index].k_dc = k_dc;
    s_fake_zone_cfg[zone_index].tau_s = tau_s;
    s_fake_zone_cfg[zone_index].dead_time_s = dead_time_s;
    return true;
}
bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_c_per_hr = s_fake_zone_cfg[zone_index].max_ramp;
    return true;
}
bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_max_temp_c = s_fake_zone_cfg[zone_index].max_temp_c;
    *out_min_temp_c = s_fake_zone_cfg[zone_index].min_temp_c;
    return true;
}

// ---------------------------------------------------------------------
// relay_authority.h -- system_mode_gate snapshot input, same convention as
// test_adaptive_tune_http_gate.c/test_zones_http.c/test_kiln_cfg_http.c's
// own copy of this stub. This is the knob the gate tests below flip.
// ---------------------------------------------------------------------
static bool s_test_profile_running = false;
static bool s_test_autotune_running = false;
void relay_authority_heat_run_active(bool *profile_running_out, bool *autotune_running_out)
{
    if (profile_running_out) *profile_running_out = s_test_profile_running;
    if (autotune_running_out) *autotune_running_out = s_test_autotune_running;
}

// ---------------------------------------------------------------------
// unit_pref.h fakes -- not gated (read/preference, not zone-config write),
// needed only so CONTROL_CMD_GET_UNIT_PREF/SET_UNIT_PREF compile+link.
// ---------------------------------------------------------------------
static unit_pref_t s_fake_unit_pref = UNIT_PREF_CELSIUS;
unit_pref_t unit_pref_get(void) { return s_fake_unit_pref; }
esp_err_t unit_pref_set(unit_pref_t pref)
{
    s_fake_unit_pref = pref;
    return ESP_OK;
}

// ---------------------------------------------------------------------
// profiles_http.h / profiles_builtin.h / run_state.h / profile_executor.h
// fakes -- the PROFILES (task 9) section of uart_bridge_ext_control.c is
// not exercised by any test in this file, but it is compiled into the same
// translation unit (whole-file #include), so every symbol it references
// still needs to resolve at link time. Minimal bodies only.
// ---------------------------------------------------------------------
const size_t g_builtin_profile_count = 0;
bool profiles_builtin_id_valid(uint8_t id) { (void)id; return false; }
bool profiles_builtin_is_hidden(uint8_t id) { (void)id; return false; }

bool profiles_http_get(uint8_t id, profile_t *out)
{
    (void)id;
    if (out) memset(out, 0, sizeof(*out));
    return false;
}
bool profiles_http_save(uint8_t requested_id, const profile_t *candidate, uint8_t *out_id,
                         uint8_t *warn_count, char *err_msg, size_t err_cap)
{
    (void)requested_id; (void)candidate;
    if (out_id) *out_id = 0;
    if (warn_count) *warn_count = 0;
    if (err_msg && err_cap) err_msg[0] = '\0';
    return false;
}
bool profiles_http_delete(uint8_t id) { (void)id; return false; }

bool profile_executor_get_active_id(uint8_t *out_id) { if (out_id) *out_id = 0; return false; }
bool profile_executor_run(uint8_t profile_id, char *err_msg, size_t err_cap)
{
    (void)profile_id;
    if (err_msg && err_cap) err_msg[0] = '\0';
    return false;
}
void profile_executor_halt(void) {}
bool profile_executor_pause(void) { return false; }
bool profile_executor_resume(void) { return false; }
void profile_executor_get_status(profile_exec_status_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

bool run_state_acknowledge(void) { return false; }

// ---------------------------------------------------------------------
// Shared flash-safe-worker dispatch stub -- see stubs/bx_worker_stub.h's
// own header comment. Included after test_common.h/esp_err.h are visible,
// per its own convention.
// ---------------------------------------------------------------------
#include "stubs/bx_worker_stub.h"

// The file under test.
#include "../drivers/bridge/uart_bridge_ext_control.c"

// ---------------------------------------------------------------------
// uart_bridge_ext_internal.h helpers -- uart_bridge_ext_control.c calls
// these (defined for real in uart_bridge_ext.c, not linked into this
// executable) to serialize its LE fields and build replies. Faked here as
// plain, observable bodies: the LE helpers do real encode/decode (needed
// so CONTROL_CMD_SET_ZONE_PID/MODEL's payload parsing actually round-trips
// the float args this file's tests send), and the reply helpers just
// capture what was sent so a test can assert refused-vs-ok without a real
// transport.
// ---------------------------------------------------------------------
const char *UART_BRIDGE_EXT_TAG = "test_uart_bridge_ext_control_gate";

uint32_t uart_bridge_ext_u32_le(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
void uart_bridge_ext_put_u32_le(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)(v & 0xFF);
    o[1] = (uint8_t)((v >> 8) & 0xFF);
    o[2] = (uint8_t)((v >> 16) & 0xFF);
    o[3] = (uint8_t)((v >> 24) & 0xFF);
}
void uart_bridge_ext_put_u16_le(uint8_t *o, uint16_t v)
{
    o[0] = (uint8_t)(v & 0xFF);
    o[1] = (uint8_t)((v >> 8) & 0xFF);
}
float uart_bridge_ext_f32_le(const uint8_t *b)
{
    uint32_t u = uart_bridge_ext_u32_le(b);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}
void uart_bridge_ext_put_f32_le(uint8_t *o, float v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    uart_bridge_ext_put_u32_le(o, u);
}
bool uart_bridge_ext_args_ok(const char *who, const uart_proto_message_t *msg, size_t need)
{
    (void)who;
    return (size_t)msg->length >= need;
}
size_t uart_bridge_ext_put_lstring(uint8_t *out, size_t cap, size_t o, const char *text)
{
    size_t len = text ? strlen(text) : 0;
    if (o + 1 > cap) return o;
    size_t room = cap - o - 1;
    if (len > room) len = room;
    out[o++] = (uint8_t)len;
    memcpy(&out[o], text, len);
    return o + len;
}

// Captured reply state, for test assertions.
static unsigned s_reply_calls = 0;
static uint8_t s_last_reply[BRIDGE_REPLY_MAX];
static size_t s_last_reply_len = 0;

static unsigned s_reply_ok_err_calls = 0;
static uint8_t s_last_subcmd = 0;
static bool s_last_ok = false;
static char s_last_err_msg[256] = "";

void uart_bridge_ext_reply(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t src_task,
                            const uint8_t *reply, size_t reply_len)
{
    (void)proto; (void)msg; (void)src_task;
    s_reply_calls++;
    s_last_reply_len = reply_len < sizeof(s_last_reply) ? reply_len : sizeof(s_last_reply);
    memcpy(s_last_reply, reply, s_last_reply_len);
}
void uart_bridge_ext_reply_ok_err(uart_protocol_t *proto, const uart_proto_message_t *msg, uint8_t src_task,
                                   uint8_t subcmd, bool ok, const char *err_msg)
{
    (void)proto; (void)msg; (void)src_task;
    s_reply_ok_err_calls++;
    s_last_subcmd = subcmd;
    s_last_ok = ok;
    if (err_msg) {
        strncpy(s_last_err_msg, err_msg, sizeof(s_last_err_msg) - 1);
        s_last_err_msg[sizeof(s_last_err_msg) - 1] = '\0';
    } else {
        s_last_err_msg[0] = '\0';
    }
}

// ---------------------------------------------------------------------
// uart_bridge_start_control_task()/control_task() themselves are never
// exercised by these tests (they only call control_handle_message()
// directly, same as every other *_gate test's static-function seam), but
// both compile into this TU and reference these five symbols, so minimal
// never-called bodies are needed only to satisfy the linker.
// ---------------------------------------------------------------------
esp_err_t uart_protocol_register_task(uart_protocol_t *proto, uint8_t task_id,
                                       UBaseType_t inbox_len, QueueHandle_t *out_inbox)
{
    (void)proto; (void)task_id; (void)inbox_len;
    if (out_inbox) *out_inbox = NULL;
    return ESP_FAIL;
}
esp_err_t uart_protocol_unregister_task(uart_protocol_t *proto, uint8_t task_id)
{
    (void)proto; (void)task_id;
    return ESP_OK;
}
esp_err_t uart_protocol_receive(QueueHandle_t inbox, uart_proto_message_t *out_msg, TickType_t wait_ticks)
{
    (void)inbox; (void)out_msg; (void)wait_ticks;
    return ESP_FAIL;
}
BaseType_t uart_bridge_ext_retry_task_create_pinned(TaskFunction_t task_fn, const char *name, uint32_t stack_depth,
                                                     void *param, UBaseType_t priority)
{
    (void)task_fn; (void)name; (void)stack_depth; (void)param; (void)priority;
    return pdFAIL;
}
bool uart_bridge_ext_worker_ensure_started(void)
{
    return false;
}

// ---------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------
static void test_reset(void)
{
    memset(s_fake_zone_cfg, 0, sizeof(s_fake_zone_cfg));
    s_test_profile_running = false;
    s_test_autotune_running = false;
    s_set_pid_calls = 0;
    s_set_model_calls = 0;
    s_reply_calls = 0;
    s_reply_ok_err_calls = 0;
    s_last_subcmd = 0;
    s_last_ok = false;
    s_last_err_msg[0] = '\0';
    s_stub_bx_busy = false;
    s_stub_on_flash_worker = false;
    s_stub_dispatch_count = 0;
}

static void build_set_zone_pid_msg(uart_proto_message_t *msg, uint8_t zone_index, float kp, float ki, float kd)
{
    memset(msg, 0, sizeof(*msg));
    msg->payload[0] = CONTROL_CMD_SET_ZONE_PID;
    msg->payload[1] = zone_index;
    uart_bridge_ext_put_f32_le(&msg->payload[2], kp);
    uart_bridge_ext_put_f32_le(&msg->payload[6], ki);
    uart_bridge_ext_put_f32_le(&msg->payload[10], kd);
    msg->length = 14;
}

static void build_set_zone_model_msg(uart_proto_message_t *msg, uint8_t zone_index, float k_dc, float tau_s,
                                      float dead_time_s)
{
    memset(msg, 0, sizeof(*msg));
    msg->payload[0] = CONTROL_CMD_SET_ZONE_MODEL;
    msg->payload[1] = zone_index;
    uart_bridge_ext_put_f32_le(&msg->payload[2], k_dc);
    uart_bridge_ext_put_f32_le(&msg->payload[6], tau_s);
    uart_bridge_ext_put_f32_le(&msg->payload[10], dead_time_s);
    msg->length = 14;
}

static void run_control_message(const uart_proto_message_t *msg)
{
    control_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    bx_handler_args_t args = { .ctx = &ctx, .msg = msg };
    control_handle_message(&args);
}

// ---------------------------------------------------------------------
// SET_ZONE_PID
// ---------------------------------------------------------------------
static void test_set_zone_pid_refused_while_profile_running(void)
{
    test_reset();
    s_test_profile_running = true;
    uart_proto_message_t msg;
    build_set_zone_pid_msg(&msg, 1, 5.0f, 0.1f, 0.01f);
    run_control_message(&msg);

    TEST_CHECK(s_set_pid_calls == 0, "SET_ZONE_PID must not reach zones_config_set_pid() while a profile runs");
    TEST_CHECK(s_reply_ok_err_calls == 1, "expected exactly one ok/err reply");
    TEST_CHECK(!s_last_ok, "SET_ZONE_PID must be refused while a profile runs");
    TEST_CHECK(strlen(s_last_err_msg) > 0, "a refusal must carry a non-empty reason");
}

static void test_set_zone_pid_refused_while_autotune_running(void)
{
    test_reset();
    s_test_autotune_running = true;
    uart_proto_message_t msg;
    build_set_zone_pid_msg(&msg, 1, 5.0f, 0.1f, 0.01f);
    run_control_message(&msg);

    TEST_CHECK(s_set_pid_calls == 0, "SET_ZONE_PID must not reach zones_config_set_pid() while autotune runs");
    TEST_CHECK(s_reply_ok_err_calls == 1, "expected exactly one ok/err reply");
    TEST_CHECK(!s_last_ok, "SET_ZONE_PID must be refused while autotune runs");
}

static void test_set_zone_pid_allowed_when_idle(void)
{
    test_reset();
    uart_proto_message_t msg;
    build_set_zone_pid_msg(&msg, 2, 7.5f, 0.2f, 0.03f);
    run_control_message(&msg);

    TEST_CHECK(s_set_pid_calls == 1, "SET_ZONE_PID must reach zones_config_set_pid() when idle");
    TEST_CHECK(s_reply_ok_err_calls == 1, "expected exactly one ok/err reply");
    TEST_CHECK(s_last_ok, "SET_ZONE_PID must succeed when idle");
    TEST_CHECK(s_fake_zone_cfg[2].kp == 7.5f, "the real write must land in the target zone");
}

// ---------------------------------------------------------------------
// SET_ZONE_MODEL
// ---------------------------------------------------------------------
static void test_set_zone_model_refused_while_profile_running(void)
{
    test_reset();
    s_test_profile_running = true;
    uart_proto_message_t msg;
    build_set_zone_model_msg(&msg, 1, 1.2f, 300.0f, 15.0f);
    run_control_message(&msg);

    TEST_CHECK(s_set_model_calls == 0, "SET_ZONE_MODEL must not reach zones_config_set_model() while a profile runs");
    TEST_CHECK(s_reply_ok_err_calls == 1, "expected exactly one ok/err reply");
    TEST_CHECK(!s_last_ok, "SET_ZONE_MODEL must be refused while a profile runs");
    TEST_CHECK(strlen(s_last_err_msg) > 0, "a refusal must carry a non-empty reason");
}

static void test_set_zone_model_refused_while_autotune_running(void)
{
    test_reset();
    s_test_autotune_running = true;
    uart_proto_message_t msg;
    build_set_zone_model_msg(&msg, 1, 1.2f, 300.0f, 15.0f);
    run_control_message(&msg);

    TEST_CHECK(s_set_model_calls == 0, "SET_ZONE_MODEL must not reach zones_config_set_model() while autotune runs");
    TEST_CHECK(s_reply_ok_err_calls == 1, "expected exactly one ok/err reply");
    TEST_CHECK(!s_last_ok, "SET_ZONE_MODEL must be refused while autotune runs");
}

static void test_set_zone_model_allowed_when_idle(void)
{
    test_reset();
    uart_proto_message_t msg;
    build_set_zone_model_msg(&msg, 3, 2.5f, 250.0f, 20.0f);
    run_control_message(&msg);

    TEST_CHECK(s_set_model_calls == 1, "SET_ZONE_MODEL must reach zones_config_set_model() when idle");
    TEST_CHECK(s_reply_ok_err_calls == 1, "expected exactly one ok/err reply");
    TEST_CHECK(s_last_ok, "SET_ZONE_MODEL must succeed when idle");
    TEST_CHECK(s_fake_zone_cfg[3].k_dc == 2.5f, "the real write must land in the target zone");
}

// ---------------------------------------------------------------------
// PAUSED counts as "running" too (owner decision Q2) -- both profile and
// autotune flags flipped together, matching relay_authority_heat_run_active()'s
// real contract of reporting RUNNING-or-PAUSED as "active".
// ---------------------------------------------------------------------
static void test_set_zone_pid_refused_while_both_flags_set(void)
{
    test_reset();
    s_test_profile_running = true;
    s_test_autotune_running = true;
    uart_proto_message_t msg;
    build_set_zone_pid_msg(&msg, 0, 1.0f, 1.0f, 1.0f);
    run_control_message(&msg);

    TEST_CHECK(s_set_pid_calls == 0, "SET_ZONE_PID must be refused when both flags are set");
    TEST_CHECK(!s_last_ok, "SET_ZONE_PID must be refused when both flags are set");
}

int main(void)
{
    test_set_zone_pid_refused_while_profile_running();
    test_set_zone_pid_refused_while_autotune_running();
    test_set_zone_pid_allowed_when_idle();
    test_set_zone_model_refused_while_profile_running();
    test_set_zone_model_refused_while_autotune_running();
    test_set_zone_model_allowed_when_idle();
    test_set_zone_pid_refused_while_both_flags_set();

    printf("test_uart_bridge_ext_control_gate: %d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
