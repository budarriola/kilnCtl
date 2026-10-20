#include "telemetry_time_sync.h"

#include <stdbool.h>
#include <time.h>

#include "boot_history.h"
#include "event_log.h"
#include "time_sync.h"

static bool s_boot_marked;
static bool s_have_anchor;     /* a sync was already logged this boot */
static int64_t s_anchor_off;   /* utc - uptime of the last logged sync */
static bool s_wall_backfilled;

void telemetry_time_sync_service(uint32_t uptime_s)
{
    if (!s_boot_marked) {
        int32_t seq = (int32_t)boot_history_latest_seq();
        bool ok = true;
        ok &= (event_log_emit(LOG_STORE_KIND_FIRING, EVENT_LOG_SEV_INFO, EVENT_LOG_SRC_SYSTEM,
                              EVENT_CODE_SYS_BOOT, EVENT_LOG_ZONE_NONE, seq, NULL) == ESP_OK);
        ok &= (event_log_emit(LOG_STORE_KIND_AUTOTUNE, EVENT_LOG_SEV_INFO, EVENT_LOG_SRC_SYSTEM,
                              EVENT_CODE_SYS_BOOT, EVENT_LOG_ZONE_NONE, seq, NULL) == ESP_OK);
        if (!ok) {
            return; /* retry next pass; the marker must precede this boot's TIME_SYNC */
        }
        s_boot_marked = true;
    }

    time_t sync_utc;
    if (!time_sync_peek_event(&sync_utc)) {
        return;
    }
    int32_t arg = (int32_t)(uint32_t)sync_utc;
    int64_t off = (int64_t)sync_utc - (int64_t)uptime_s;
    if (event_log_emit(LOG_STORE_KIND_FIRING, EVENT_LOG_SEV_INFO, EVENT_LOG_SRC_SYSTEM, EVENT_CODE_SYS_TIME_SYNC,
                       EVENT_LOG_ZONE_NONE, arg, NULL) != ESP_OK) {
        return; /* FQ-02: flag stays pending, retried next pass */
    }
    int64_t d = off - s_anchor_off;
    if (!s_have_anchor || d > TELEMETRY_TIME_SYNC_STEP_S || d < -TELEMETRY_TIME_SYNC_STEP_S) {
        (void)event_log_emit(LOG_STORE_KIND_AUTOTUNE, EVENT_LOG_SEV_INFO, EVENT_LOG_SRC_SYSTEM,
                             EVENT_CODE_SYS_TIME_SYNC, EVENT_LOG_ZONE_NONE, arg, NULL);
    }
    s_have_anchor = true;
    s_anchor_off = off;
    time_sync_ack_event(sync_utc);

    if (!s_wall_backfilled && off > 0) {
        s_wall_backfilled = true; /* one attempt per boot: at most one extra NVS write */
        (void)boot_history_backfill_wall_time((uint32_t)off);
    }
}
