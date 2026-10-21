// Host tests for persist/telemetry_time_sync.c (FQ-01/02/03/05, BH-02): the take-and-write path of the
// SYS_BOOT marker and the TIME_SYNC event. event_log_emit / time_sync / boot_history are faked.
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/persist/event_log.h"
#include "../drivers/persist/boot_history.h"
#include "../drivers/net/time_sync.h"

typedef struct { log_store_kind_t kind; event_log_code_t code; int32_t arg; } emitted_t;
static emitted_t s_em[32];
static int s_em_n;
static esp_err_t s_emit_result;
static bool s_pending;
static time_t s_utc;
static int s_backfill_calls;
static uint32_t s_backfill_val;

esp_err_t event_log_emit(log_store_kind_t kind, event_log_severity_t severity, event_log_source_t source,
                         event_log_code_t code, uint8_t zone, int32_t arg, const char *note)
{
    (void)severity; (void)source; (void)zone; (void)note;
    if (s_emit_result != ESP_OK) {
        return s_emit_result;
    }
    if (s_em_n < 32) {
        s_em[s_em_n].kind = kind; s_em[s_em_n].code = code; s_em[s_em_n].arg = arg; s_em_n++;
    }
    return ESP_OK;
}
bool time_sync_peek_event(time_t *u) { if (s_pending && u) { *u = s_utc; } return s_pending; }
void time_sync_ack_event(time_t u) { if (u == s_utc) { s_pending = false; } }
unsigned boot_history_recent(boot_history_entry_t out[BOOT_HISTORY_RECENT])
{
    memset(out, 0, sizeof(boot_history_entry_t) * BOOT_HISTORY_RECENT);
    out[0].seq = 7u;
    return 1u;
}
uint32_t boot_history_latest_seq(void) { return 7u; }
uint8_t boot_history_latest_rep(void) { return 0u; }
esp_err_t boot_history_backfill_wall_time(uint32_t w) { s_backfill_calls++; s_backfill_val = w; return ESP_OK; }

#include "../drivers/persist/telemetry_time_sync.c"

static int count(log_store_kind_t k, event_log_code_t c)
{
    int n = 0;
    for (int i = 0; i < s_em_n; i++) { if (s_em[i].kind == k && s_em[i].code == c) { n++; } }
    return n;
}

static void sync_at(time_t utc) { s_utc = utc; s_pending = true; }

static void test_all(void)
{
    TEST_SECTION("telemetry_time_sync: boot marker, FQ-02 retry, FQ-03 filter, BH-02 backfill");
    telemetry_time_sync_service(1);
    TEST_CHECK(count(LOG_STORE_KIND_FIRING, EVENT_CODE_SYS_BOOT) == 1 && count(LOG_STORE_KIND_AUTOTUNE, EVENT_CODE_SYS_BOOT) == 1,
               "FQ-01: SYS_BOOT written to both stores");
    TEST_CHECK(s_em[0].arg == 7, "FQ-01: SYS_BOOT arg is the boot_history seq");
    telemetry_time_sync_service(2);
    TEST_CHECK(count(LOG_STORE_KIND_FIRING, EVENT_CODE_SYS_BOOT) == 1, "FQ-01: marker only once per boot");

    /* FQ-02: failed write keeps the flag, next pass retries */
    sync_at(1800000000);
    s_emit_result = ESP_FAIL;
    telemetry_time_sync_service(100);
    TEST_CHECK(s_pending, "FQ-02: flag still pending after a failed write");
    TEST_CHECK(count(LOG_STORE_KIND_FIRING, EVENT_CODE_SYS_TIME_SYNC) == 0, "FQ-02: nothing written yet");
    s_emit_result = ESP_OK;
    telemetry_time_sync_service(101);
    TEST_CHECK(!s_pending, "FQ-02: flag cleared after the successful write");
    TEST_CHECK(count(LOG_STORE_KIND_FIRING, EVENT_CODE_SYS_TIME_SYNC) == 1 &&
                   count(LOG_STORE_KIND_AUTOTUNE, EVENT_CODE_SYS_TIME_SYNC) == 1,
               "FQ-03: first sync of the boot goes to both stores");

    /* BH-02: backfilled once with utc - uptime */
    TEST_CHECK(s_backfill_calls == 1 && s_backfill_val == (uint32_t)(1800000000 - 101), "BH-02: wall time = utc - uptime, once");

    /* hourly resync, no step: firing log only */
    sync_at(1800000000 + 3600);
    telemetry_time_sync_service(101 + 3600);
    TEST_CHECK(count(LOG_STORE_KIND_FIRING, EVENT_CODE_SYS_TIME_SYNC) == 2, "FQ-03: firing log keeps every sync");
    TEST_CHECK(count(LOG_STORE_KIND_AUTOTUNE, EVENT_CODE_SYS_TIME_SYNC) == 1, "FQ-03: autotune log skips a no-step resync");
    TEST_CHECK(s_backfill_calls == 1, "BH-02: no second backfill");

    /* step > 60 s */
    sync_at(1800000000 + 7200 + 90);
    telemetry_time_sync_service(101 + 7200);
    TEST_CHECK(count(LOG_STORE_KIND_AUTOTUNE, EVENT_CODE_SYS_TIME_SYNC) == 2, "FQ-03: a >60 s step is logged to the autotune store");
}

static void test_fv02_early_sync(void)
{
    TEST_SECTION("telemetry_time_sync: FV-02 SNTP before 60 s uptime still backfills at 60 s");
    s_boot_marked = false; s_have_anchor = false; s_anchor_off = 0; s_wall_backfilled = false;
    s_backfill_calls = 0; s_backfill_val = 0; s_em_n = 0;
    sync_at(1900000000);
    telemetry_time_sync_service(20);
    TEST_CHECK(!s_pending && s_backfill_calls == 0, "FV-02: early sync acked, no backfill yet");
    telemetry_time_sync_service(40);
    TEST_CHECK(s_backfill_calls == 0, "FV-02: still none before 60 s");
    telemetry_time_sync_service(61);
    TEST_CHECK(s_backfill_calls == 1 && s_backfill_val == (uint32_t)(1900000000 - 20), "FV-02: deferred backfill runs once with the sync offset");
    telemetry_time_sync_service(62);
    TEST_CHECK(s_backfill_calls == 1, "FV-02: only once");
}

int main(void)
{
    test_all();
    test_fv02_early_sync();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures ? 1 : 0;
}
