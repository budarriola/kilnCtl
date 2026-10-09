// event_log_emit -- device-only glue for event_log.h's emit path. Split out
// of event_log.c for the exact reason telemetry_log.c is split from
// telemetry_format.c: this file needs hal_time_now_us() and the
// log_store_mount.c flash-worker dispatch, neither of which exists off-
// target, so it is NOT part of the host test build (event_log.c's pure
// encode/decode is -- see test_event_log.c).
#include "event_log.h"

#include <string.h>

#include "hal_time.h"
#include "log_store_mount.h"

esp_err_t event_log_emit(log_store_kind_t kind, event_log_severity_t severity, event_log_source_t source,
                          event_log_code_t code, uint8_t zone, int32_t arg, const char *note)
{
    event_log_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.severity = (uint8_t)severity;
    ev.source = (uint8_t)source;
    ev.code = (uint8_t)code;
    ev.zone = zone;
    ev.arg = arg;
    ev.uptime_s = (uint32_t)(hal_time_now_us() / 1000000); /* same conversion dashboard_http.c uses */
    if (note) {
        strncpy(ev.note, note, EVENT_LOG_NOTE_LEN);
    }

    uint8_t rec[EVENT_LOG_RECORD_SIZE];
    event_log_encode(&ev, rec);
    return log_store_write_event(kind, rec, EVENT_LOG_RECORD_SIZE);
}
