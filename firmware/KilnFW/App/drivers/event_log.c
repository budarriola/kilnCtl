#include "event_log.h"

#include <string.h>

/* Pure encode/decode only -- no ESP-IDF, no FreeRTOS. Kept host-testable the
 * same way telemetry_format.c is split out of telemetry_log.c: the device-
 * only wrapper (event_log_emit(), which needs esp_timer_get_time() and the
 * flash-worker dispatch) lives in event_log_emit.c instead. */

/* Explicit byte offsets, not a packed struct cast -- see event_log.h's file
 * banner for why (MSVC host build vs GCC/Xtensa device build). */
#define OFF_MAGIC     0u
#define OFF_VERSION   1u
#define OFF_SEVERITY  2u
#define OFF_SOURCE    3u
#define OFF_UPTIME    4u  /* 4 bytes, LE */
#define OFF_CODE      8u
#define OFF_ZONE      9u
#define OFF_RESERVED  10u /* 2 bytes, always 0 */
#define OFF_ARG       12u /* 4 bytes, LE, signed */
#define OFF_NOTE      16u /* EVENT_LOG_NOTE_LEN bytes */

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void event_log_encode(const event_log_event_t *ev, uint8_t out[EVENT_LOG_RECORD_SIZE])
{
    memset(out, 0, EVENT_LOG_RECORD_SIZE);

    out[OFF_MAGIC] = EVENT_LOG_RECORD_MAGIC;
    out[OFF_VERSION] = EVENT_LOG_RECORD_VERSION;
    out[OFF_SEVERITY] = ev->severity;
    out[OFF_SOURCE] = ev->source;
    put_u32le(&out[OFF_UPTIME], ev->uptime_s);
    out[OFF_CODE] = ev->code;
    out[OFF_ZONE] = ev->zone;
    /* OFF_RESERVED already zeroed by the memset above. */
    put_u32le(&out[OFF_ARG], (uint32_t)ev->arg);

    size_t note_len = strnlen(ev->note, EVENT_LOG_NOTE_LEN);
    memcpy(&out[OFF_NOTE], ev->note, note_len); /* remainder already zeroed -> NUL padding */
}

bool event_log_decode(const uint8_t in[EVENT_LOG_RECORD_SIZE], event_log_event_t *out)
{
    if (in[OFF_MAGIC] != EVENT_LOG_RECORD_MAGIC || in[OFF_VERSION] != EVENT_LOG_RECORD_VERSION) {
        return false; /* not this format -- refuse rather than misread (old text log, corruption, etc.) */
    }

    out->severity = in[OFF_SEVERITY];
    out->source = in[OFF_SOURCE];
    out->uptime_s = get_u32le(&in[OFF_UPTIME]);
    out->code = in[OFF_CODE];
    out->zone = in[OFF_ZONE];
    out->arg = (int32_t)get_u32le(&in[OFF_ARG]);

    memcpy(out->note, &in[OFF_NOTE], EVENT_LOG_NOTE_LEN);
    /* Guarantee NUL-termination even if the 16 note bytes were entirely
     * full with no NUL of their own -- callers must be able to treat
     * out->note as a plain C string unconditionally. */
    out->note[EVENT_LOG_NOTE_LEN - 1] = '\0';
    return true;
}
