// Host tests for App/drivers/persist/event_log.c -- event_log_encode()/
// event_log_decode(), the fixed 32-byte binary record format flash logging
// moved to (2026-09-02, event_log.h's file banner). Pure encode/decode only
// -- event_log_emit() (the esp_timer_get_time()/flash-worker device glue)
// lives in the SEPARATE event_log_emit.c and is not built here, same split
// telemetry_format.c/telemetry_log.c already use.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/persist/event_log.h"

// ---------------------------------------------------------------------------
// Round trip: encode then decode returns the same values.
// ---------------------------------------------------------------------------
static void test_round_trip(void)
{
    TEST_SECTION("event_log: encode/decode round-trips every field");

    event_log_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.severity = EVENT_LOG_SEV_ERROR;
    ev.source = EVENT_LOG_SRC_FIRING;
    ev.code = EVENT_CODE_FIRING_FAULTED;
    ev.zone = 2;
    ev.uptime_s = 123456u;
    ev.arg = -77;
    strncpy(ev.note, "guard7", sizeof(ev.note));

    uint8_t rec[EVENT_LOG_RECORD_SIZE];
    event_log_encode(&ev, rec);

    TEST_CHECK(sizeof(rec) == 32u, "record size is exactly 32 bytes");
    TEST_CHECK(rec[0] == EVENT_LOG_RECORD_MAGIC, "byte 0 is the magic byte");
    TEST_CHECK(rec[1] == EVENT_LOG_RECORD_VERSION, "byte 1 is the version byte");

    event_log_event_t out;
    memset(&out, 0xAA, sizeof(out)); /* poison, so a field decode skips would show up as 0xAA, not 0 */
    TEST_CHECK(event_log_decode(rec, &out), "decode succeeds on a record it just encoded");
    TEST_CHECK(out.severity == EVENT_LOG_SEV_ERROR, "severity round-trips");
    TEST_CHECK(out.source == EVENT_LOG_SRC_FIRING, "source round-trips");
    TEST_CHECK(out.code == EVENT_CODE_FIRING_FAULTED, "code round-trips");
    TEST_CHECK(out.zone == 2, "zone round-trips");
    TEST_CHECK(out.uptime_s == 123456u, "uptime_s round-trips");
    TEST_CHECK(out.arg == -77, "negative arg round-trips (sign-correct)");
    TEST_CHECK(strcmp(out.note, "guard7") == 0, "note text round-trips");
}

// ---------------------------------------------------------------------------
// Sentinel zone value and a max-magnitude negative arg both survive.
// ---------------------------------------------------------------------------
static void test_edge_values(void)
{
    TEST_SECTION("event_log: sentinel zone and extreme arg values round-trip");

    event_log_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.severity = EVENT_LOG_SEV_INFO;
    ev.source = EVENT_LOG_SRC_AUTOTUNE;
    ev.code = EVENT_CODE_TUNE_STARTED;
    ev.zone = EVENT_LOG_ZONE_NONE;
    ev.uptime_s = 0xFFFFFFFFu;
    ev.arg = INT32_MIN;
    /* No note -- proves an empty note round-trips as an empty string, not
     * garbage. */

    uint8_t rec[EVENT_LOG_RECORD_SIZE];
    event_log_encode(&ev, rec);

    event_log_event_t out;
    TEST_CHECK(event_log_decode(rec, &out), "decode succeeds");
    TEST_CHECK(out.zone == EVENT_LOG_ZONE_NONE, "zone-none sentinel round-trips");
    TEST_CHECK(out.uptime_s == 0xFFFFFFFFu, "max uptime_s round-trips");
    TEST_CHECK(out.arg == INT32_MIN, "INT32_MIN arg round-trips");
    TEST_CHECK(out.note[0] == '\0', "empty note decodes as an empty string");
}

// ---------------------------------------------------------------------------
// A note exactly EVENT_LOG_NOTE_LEN bytes long (no room for a NUL) still
// decodes as a valid, NUL-terminated string -- never reads past the field.
// ---------------------------------------------------------------------------
static void test_note_fills_field_exactly(void)
{
    TEST_SECTION("event_log: a note filling the whole field still NUL-terminates on decode");

    event_log_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.severity = EVENT_LOG_SEV_WARN;
    ev.source = EVENT_LOG_SRC_SYSTEM;
    ev.code = 0;
    ev.zone = EVENT_LOG_ZONE_NONE;
    /* Exactly EVENT_LOG_NOTE_LEN non-NUL bytes -- longer input is expected
     * to truncate safely, not overrun encode's fixed-size output. */
    memset(ev.note, 'x', sizeof(ev.note));

    uint8_t rec[EVENT_LOG_RECORD_SIZE];
    event_log_encode(&ev, rec);

    event_log_event_t out;
    TEST_CHECK(event_log_decode(rec, &out), "decode succeeds");
    bool nul_terminated = false;
    for (size_t i = 0; i < EVENT_LOG_NOTE_LEN; i++) {
        if (out.note[i] == '\0') {
            nul_terminated = true;
            break;
        }
    }
    TEST_CHECK(nul_terminated, "decoded note is NUL-terminated within EVENT_LOG_NOTE_LEN even when the field is full");
}

// ---------------------------------------------------------------------------
// A record from the OLD text-line format (or any garbage) is refused, not
// misread as a valid event. This is the compatibility contract:
// FLASH_BUDGET_PLAN.md's migration note says an old flash log is refused
// with a clear signal, never silently misdecoded.
// ---------------------------------------------------------------------------
static void test_decode_refuses_bad_magic(void)
{
    TEST_SECTION("event_log: decode refuses a record with the wrong magic/version");

    /* First bytes of an old-format text line, "KTEL1 FIRE t=..." -- 'K' is
     * 0x4B, nowhere near EVENT_LOG_RECORD_MAGIC (0xE7), by construction
     * (see event_log.h's file banner on why that was chosen). */
    uint8_t old_text[EVENT_LOG_RECORD_SIZE];
    memcpy(old_text, "KTEL1 FIRE t=1234 st=RUN", 25);
    memset(old_text + 25, 0, sizeof(old_text) - 25);

    event_log_event_t out;
    memset(&out, 0, sizeof(out));
    TEST_CHECK(!event_log_decode(old_text, &out), "old-format text bytes are refused, not decoded");

    /* Right magic, wrong version -- a genuinely future/older format. */
    uint8_t wrong_version[EVENT_LOG_RECORD_SIZE];
    memset(wrong_version, 0, sizeof(wrong_version));
    wrong_version[0] = EVENT_LOG_RECORD_MAGIC;
    wrong_version[1] = (uint8_t)(EVENT_LOG_RECORD_VERSION + 1u);
    TEST_CHECK(!event_log_decode(wrong_version, &out), "an unrecognized version byte is refused, not decoded");

    /* All-zero bytes (an unwritten/erased flash region, or a truncated
     * read) must not be misread as a valid record either. */
    uint8_t zeros[EVENT_LOG_RECORD_SIZE];
    memset(zeros, 0, sizeof(zeros));
    TEST_CHECK(!event_log_decode(zeros, &out), "all-zero bytes are refused, not decoded as a valid event");
}

int main(void)
{
    test_round_trip();
    test_edge_values();
    test_note_fills_field_exactly();
    test_decode_refuses_bad_magic();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
