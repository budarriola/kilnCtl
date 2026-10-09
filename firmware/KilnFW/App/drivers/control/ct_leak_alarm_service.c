// H9 CT alarm -- the on-target glue around the pure ct_leak_alarm core
// (safety/ct_leak_alarm.c). Called from safety_poll_task's service block (the
// same place heat_enable_service_pending_release() runs), so it needs no task
// of its own and therefore no stack-margin registration of its own: it runs
// on safety_poll_task's already-registered stack.
//
// Reads only cheap, lock-brief accessors: safety_link_get_status() (a copy),
// kiln_io_relays_off_ms() (the FULL relay shadow -- all four relays, zone,
// manual and aux alike), the committed safety-cfg cache, and three activity
// predicates. Nothing here holds a lock across another module's call.
#include "ct_leak_alarm_service.h"

#include <string.h>

#include "autotune_engine.h"
#include "ct_leak_alarm.h"
#include "esp_log.h"
#include "hal_time.h"
#include "kiln_io.h"
#include "profile_executor.h"
#include "safety_cfg_store.h"
#include "safety_link.h"
#include "zones_config_accessors.h" /* zones_current_sweep_is_active() */

static const char *TAG = "ct_leak";

#define CT_LEAK_SERVICE_PERIOD_MS 500u
#define CT_LEAK_RELOG_MS 60000u
/* safety_cfg wire ids: ct_installed (U8), ct_topology (U8), k_ct_v_per_a[ch]
 * (F32). Same ids zones_current_sweep_task.c and readiness_http.c read. */
#define CT_LEAK_PARAM_INSTALLED 0x0109u
#define CT_LEAK_PARAM_TOPOLOGY 0x031Fu
#define CT_LEAK_PARAM_KCT(ch) ((uint16_t)(0x0308u + (ch)))

static kiln_io_t *s_io = NULL;
static ct_leak_alarm_state_t s_state;
static uint32_t s_last_eval_ms = 0;
static uint32_t s_last_log_ms = 0;
static bool s_started = false;

void ct_leak_alarm_service_bind(kiln_io_t *io_or_null)
{
    s_io = io_or_null;
}

static void read_params(ct_leak_alarm_input_t *in)
{
    in->ct_installed = true; /* unset reads installed: an unknown value must never relax the alarm */
    in->summed = false;
    for (int c = 0; c < CT_LEAK_CHANNELS; c++) {
        in->k_ct_v_per_a[c] = 0.0f;
    }
    size_t count = safety_cfg_store_param_count();
    for (size_t i = 0; i < count; i++) {
        safety_cfg_param_t row;
        memset(&row, 0, sizeof(row));
        if (!safety_cfg_store_get_by_index(i, &row) || !row.set) {
            continue;
        }
        if (row.param_id == CT_LEAK_PARAM_INSTALLED) {
            in->ct_installed = row.value.u8_val != 0u;
        } else if (row.param_id == CT_LEAK_PARAM_TOPOLOGY) {
            in->summed = row.value.u8_val != 0u;
        } else {
            for (int c = 0; c < CT_LEAK_CHANNELS; c++) {
                if (row.param_id == CT_LEAK_PARAM_KCT(c)) {
                    in->k_ct_v_per_a[c] = row.value.f32_val;
                }
            }
        }
    }
}

void ct_leak_alarm_service(SafetyLinkClass *link)
{
    if (link == NULL || s_io == NULL) {
        return;
    }
    uint32_t now_ms = (uint32_t)(hal_time_now_us() / 1000u);
    if (s_started && (uint32_t)(now_ms - s_last_eval_ms) < CT_LEAK_SERVICE_PERIOD_MS) {
        return;
    }
    s_started = true;
    s_last_eval_ms = now_ms;

    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    ct_leak_alarm_input_t in;
    memset(&in, 0, sizeof(in));
    in.now_ms = now_ms;
    in.link_up = (safety_link_get_status(link, &st) == ESP_OK) && st.link_up &&
                 !safety_link_is_stale(st.age_ms, SAFETY_LINK_STALE_MS);
    for (int c = 0; c < CT_LEAK_CHANNELS; c++) {
        in.current_a[c] = st.current_a[c];
    }
    read_params(&in);
    in.relays_off_ms = kiln_io_relays_off_ms(s_io);
    uint8_t active_id = 0;
    in.activity = profile_executor_get_active_id(&active_id) || autotune_engine_is_active() ||
                  zones_current_sweep_is_active();

    bool was_alarm = s_state.alarm;
    bool raised = ct_leak_alarm_tick(&s_state, &in);
    ct_leak_alarm_publish(&s_state);

    if (raised) {
        char text[96];
        ct_leak_alarm_describe(text, sizeof(text));
        ESP_LOGE(TAG, "H9 ALARM: %s. Something downstream of the relays is conducting; "
                      "the ESP cannot interrupt it. New firings are refused. Isolate mains at the "
                      "contactor/breaker and inspect the SSRs and contactor.", text);
        s_last_log_ms = now_ms;
    } else if (s_state.alarm && (uint32_t)(now_ms - s_last_log_ms) >= CT_LEAK_RELOG_MS) {
        char text[96];
        ct_leak_alarm_describe(text, sizeof(text));
        ESP_LOGE(TAG, "H9 ALARM still active: %s", text);
        s_last_log_ms = now_ms;
    } else if (was_alarm && !s_state.alarm) {
        ESP_LOGW(TAG, "H9 alarm cleared: CT channels quiet with every relay off");
    }
}
