// Host test for App/drivers/control/ct_leak_alarm_service.c (HOST_TEST_COVERAGE_GAPS
// round 2, campaign R2-1): the H9 on-target glue around the (already tested) pure
// ct_leak_alarm core. Covers what the core test cannot: unbound no-op, 500 ms
// self-throttle, safety_cfg param scan (ct_installed / topology / k_ct, unset rows
// ignored), stale/down link gating, activity predicates, relay-shadow gating.
// ct_leak_alarm_service.c and ct_leak_alarm.c are #include'd directly.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;
#define CHECK(c) TEST_CHECK((c), #c)

#include "../drivers/safety/ct_leak_alarm.c"
#include "../drivers/control/ct_leak_alarm_service.c"

/* ---- fakes ---- */
static uint64_t s_now_us;
uint64_t hal_time_now_us(void) { return s_now_us; }

static safety_link_status_t s_status;
static esp_err_t s_status_rc = ESP_OK;
esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out)
{
    (void)link;
    if (s_status_rc == ESP_OK) {
        *out = s_status;
    }
    return s_status_rc;
}

static uint32_t s_relays_off_ms;
uint32_t kiln_io_relays_off_ms(const kiln_io_t *io) { (void)io; return s_relays_off_ms; }

static bool s_exec_active, s_at_active, s_sweep_active;
bool profile_executor_get_active_id(uint8_t *out) { if (out) *out = 1; return s_exec_active; }
bool autotune_engine_is_active(void) { return s_at_active; }
bool zones_current_sweep_is_active(void) { return s_sweep_active; }

#define MAXP 8
static safety_cfg_param_t s_rows[MAXP];
static size_t s_nrows;
size_t safety_cfg_store_param_count(void) { return s_nrows; }
bool safety_cfg_store_get_by_index(size_t i, safety_cfg_param_t *out)
{
    if (i >= s_nrows || out == NULL) return false;
    *out = s_rows[i];
    return true;
}

static void add_u8(uint16_t id, uint8_t v, bool set)
{
    memset(&s_rows[s_nrows], 0, sizeof(s_rows[0]));
    s_rows[s_nrows].param_id = id; s_rows[s_nrows].set = set;
    s_rows[s_nrows].value.u8_val = v; s_nrows++;
}
static void add_f32(uint16_t id, float v)
{
    memset(&s_rows[s_nrows], 0, sizeof(s_rows[0]));
    s_rows[s_nrows].param_id = id; s_rows[s_nrows].set = true;
    s_rows[s_nrows].value.f32_val = v; s_nrows++;
}

static kiln_io_t *const IO = (kiln_io_t *)(uintptr_t)0x1000;
static SafetyLinkClass *const LINK = (SafetyLinkClass *)(uintptr_t)0x2000;

static void fresh(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_started = false; s_last_eval_ms = 0; s_last_log_ms = 0;
    ct_leak_alarm_publish(&s_state);
    s_now_us = 100000ull * 1000u; /* 100 s */
    memset(&s_status, 0, sizeof(s_status));
    s_status.link_up = true; s_status.age_ms = 10;
    s_status_rc = ESP_OK;
    s_relays_off_ms = 60000u;
    s_exec_active = s_at_active = s_sweep_active = false;
    s_nrows = 0;
    ct_leak_alarm_service_bind(IO);
}

/* Run the service for span_ms at 500 ms steps. */
static void run_span(uint32_t span_ms)
{
    uint32_t end = (uint32_t)(s_now_us / 1000u) + span_ms;
    while ((uint32_t)(s_now_us / 1000u) < end) {
        ct_leak_alarm_service(LINK);
        s_now_us += 500u * 1000u;
    }
}

int main(void)
{
    TEST_SECTION("ct_leak_alarm_service -- H9 glue");

    /* unbound / NULL link: no-op, nothing evaluated */
    fresh();
    s_status.current_a[0] = 5.0f;
    ct_leak_alarm_service_bind(NULL);
    run_span(20000);
    CHECK(!ct_leak_alarm_is_active());
    CHECK(!s_started);
    ct_leak_alarm_service_bind(IO);
    ct_leak_alarm_service(NULL);
    CHECK(!s_started);

    /* default-safe: no params set => installed, per-zone; leak raises after 10 s */
    fresh();
    s_status.current_a[1] = 0.5f;
    run_span(9000);
    CHECK(!ct_leak_alarm_is_active());
    run_span(3000);
    CHECK(ct_leak_alarm_is_active());
    char txt[96];
    ct_leak_alarm_describe(txt, sizeof(txt));
    CHECK(strstr(txt, "ch2") != NULL);

    /* self-throttle: calls inside 500 ms do not re-evaluate */
    fresh();
    ct_leak_alarm_service(LINK);
    uint32_t first = s_last_eval_ms;
    s_now_us += 200u * 1000u;
    ct_leak_alarm_service(LINK);
    CHECK(s_last_eval_ms == first);
    s_now_us += 300u * 1000u;
    ct_leak_alarm_service(LINK);
    CHECK(s_last_eval_ms == first + 500u);

    /* ct_installed = 0 declared: never alarms */
    fresh();
    add_u8(CT_LEAK_PARAM_INSTALLED, 0, true);
    s_status.current_a[0] = 5.0f;
    run_span(30000);
    CHECK(!ct_leak_alarm_is_active());

    /* an UNSET ct_installed row (value 0, set=false) must be ignored -> still installed */
    fresh();
    add_u8(CT_LEAK_PARAM_INSTALLED, 0, false);
    s_status.current_a[0] = 5.0f;
    run_span(12000);
    CHECK(ct_leak_alarm_is_active());

    /* summed topology: only channel index 2 counts */
    fresh();
    add_u8(CT_LEAK_PARAM_TOPOLOGY, 1, true);
    s_status.current_a[0] = 5.0f;
    run_span(30000);
    CHECK(!ct_leak_alarm_is_active());
    s_status.current_a[2] = 5.0f;
    run_span(12000);
    CHECK(ct_leak_alarm_is_active());

    /* k_ct scales the floor (0.045 A * ref/k) */
    fresh();
    add_f32(CT_LEAK_PARAM_KCT(0), 0.25f); /* floor = 4x reference */
    s_status.current_a[0] = 0.10f;
    run_span(20000);
    CHECK(!ct_leak_alarm_is_active());
    fresh();
    add_f32(CT_LEAK_PARAM_KCT(0), 4.0f);  /* floor = reference / 4 */
    s_status.current_a[0] = 0.04f;
    run_span(12000);
    CHECK(ct_leak_alarm_is_active());

    /* link down / stale / read error: no progress */
    fresh();
    s_status.current_a[0] = 5.0f;
    s_status.link_up = false;
    run_span(30000);
    CHECK(!ct_leak_alarm_is_active());
    fresh();
    s_status.current_a[0] = 5.0f;
    s_status.age_ms = SAFETY_LINK_STALE_MS + 1u;
    run_span(30000);
    CHECK(!ct_leak_alarm_is_active());
    fresh();
    s_status.current_a[0] = 5.0f;
    s_status_rc = ESP_FAIL;
    run_span(30000);
    CHECK(!ct_leak_alarm_is_active());

    /* each activity predicate suppresses evaluation */
    for (int k = 0; k < 3; k++) {
        fresh();
        s_status.current_a[0] = 5.0f;
        s_exec_active = (k == 0); s_at_active = (k == 1); s_sweep_active = (k == 2);
        run_span(30000);
        CHECK(!ct_leak_alarm_is_active());
    }

    /* relay shadow gating: any relay on (UINT32_MAX) or <5 s settle suppresses */
    fresh();
    s_status.current_a[0] = 5.0f;
    s_relays_off_ms = UINT32_MAX;
    run_span(30000);
    CHECK(!ct_leak_alarm_is_active());
    s_relays_off_ms = 4999u;
    run_span(30000);
    CHECK(!ct_leak_alarm_is_active());
    s_relays_off_ms = 5000u;
    run_span(12000);
    CHECK(ct_leak_alarm_is_active());

    /* latched alarm survives activity; clears only after 30 s quiet with relays off */
    s_exec_active = true;
    s_status.current_a[0] = 0.0f;
    run_span(40000);
    CHECK(ct_leak_alarm_is_active());
    s_exec_active = false;
    run_span(25000);
    CHECK(ct_leak_alarm_is_active());
    run_span(10000);
    CHECK(!ct_leak_alarm_is_active());

    if (g_test_failures) {
        printf("test_ct_leak_alarm_service: %d FAILED\n", g_test_failures);
        return 1;
    }
    printf("\n%d checks passed\n", g_test_count);
    return 0;
}
