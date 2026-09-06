// Host tests for time_sync_tz.c -- the pure, dependency-free half of
// time_sync.c (TZ string validation and the unset/corrupt->UTC degrade
// rule). Deliberately does NOT test time_sync.c itself: that file pulls in
// esp_netif_sntp.h/nvs.h/the real SNTP client and has no host stub written
// for it in this pass -- see time_sync_tz.h's header comment for why the
// decision was factored out instead. Network sync landing, NVS persistence
// and the wifi_prov.c hook are exercised on real hardware only.
#include "test_common.h"
#include "../drivers/net/time_sync_tz.h"

#include <string.h>

void run_test_time_sync(void)
{
    TEST_SECTION("time_sync (TZ validation + degrade-to-UTC)");

    // ---- time_sync_tz_is_valid(): valid strings accepted -------------------
    TEST_CHECK(time_sync_tz_is_valid("UTC0"), "plain UTC accepted");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT,M3.2.0,M11.1.0"), "US Eastern POSIX rule accepted");
    TEST_CHECK(time_sync_tz_is_valid("ABC1"), "shortest valid form (3-letter name + single-digit offset) accepted");
    {
        // Exactly TIME_SYNC_TZ_MAX_LEN bytes -- the boundary itself must be
        // accepted, not just comfortably-short strings, or the "over-long
        // refused" test below would prove nothing about where the real
        // limit is. Must still be a grammatically valid TZ rule now that
        // time_sync_tz_is_valid() checks grammar, not just charset/length --
        // an unquoted std name has no upper length limit of its own, so
        // TIME_SYNC_TZ_MAX_LEN-1 letters plus a 1-digit offset lands exactly
        // on the boundary.
        char exact[TIME_SYNC_TZ_MAX_LEN + 1];
        memset(exact, 'A', TIME_SYNC_TZ_MAX_LEN - 1);
        exact[TIME_SYNC_TZ_MAX_LEN - 1] = '0';
        exact[TIME_SYNC_TZ_MAX_LEN] = '\0';
        TEST_CHECK(time_sync_tz_is_valid(exact), "exactly TIME_SYNC_TZ_MAX_LEN bytes, valid grammar, accepted");
    }

    // ---- refused: over-long -------------------------------------------------
    {
        // Same shape as the boundary-accepted case above (valid name +
        // offset grammar) but one byte over the length cap -- proves this
        // is refused for LENGTH, not because it happens to also be bad
        // grammar (an all-'A' run with no offset would be refused for
        // grammar first, masking the length check).
        char too_long[TIME_SYNC_TZ_MAX_LEN + 2];
        memset(too_long, 'A', TIME_SYNC_TZ_MAX_LEN);
        too_long[TIME_SYNC_TZ_MAX_LEN] = '0';
        too_long[TIME_SYNC_TZ_MAX_LEN + 1] = '\0';
        TEST_CHECK(!time_sync_tz_is_valid(too_long), "one byte over TIME_SYNC_TZ_MAX_LEN refused");
    }

    // ---- refused: non-printable ---------------------------------------------
    // Each of these is otherwise a plainly valid-length string -- the ONLY
    // thing wrong is the one non-printable byte, so a pass here proves the
    // printable-ASCII check itself fired, not that the string was refused
    // for length or emptiness instead (the task's "make sure the input
    // reaches the logic it names" warning).
    TEST_CHECK(!time_sync_tz_is_valid("UTC\ttab"), "embedded tab refused");
    TEST_CHECK(!time_sync_tz_is_valid("UTC\nline"), "embedded newline refused");
    TEST_CHECK(!time_sync_tz_is_valid("UTC\x01"), "embedded control byte 0x01 refused");
    TEST_CHECK(!time_sync_tz_is_valid("UTC\x7F"), "embedded DEL (0x7F) refused");
    TEST_CHECK(!time_sync_tz_is_valid("UTC\xC3\xA9"), "embedded non-ASCII (UTF-8 e-acute) byte refused");

    // ---- refused: empty / NULL ------------------------------------------
    TEST_CHECK(!time_sync_tz_is_valid(""), "empty string refused");
    TEST_CHECK(!time_sync_tz_is_valid(NULL), "NULL refused");

    // ---- time_sync_tz_effective(): never-set / default state ---------------
    {
        char out[TIME_SYNC_TZ_MAX_LEN + 1];
        time_sync_tz_effective(NULL, out, sizeof(out));
        TEST_CHECK(strcmp(out, TIME_SYNC_TZ_DEFAULT) == 0, "NULL stored -> default UTC string");
        TEST_CHECK(time_sync_tz_is_valid(out), "default UTC string is itself valid");
    }

    // ---- time_sync_tz_effective(): degrade-to-UTC on a corrupt stored TZ ---
    {
        char out[TIME_SYNC_TZ_MAX_LEN + 1];
        // A stored value that fails validation for the SAME reason a
        // corrupted NVS byte would (embedded control byte) -- must degrade
        // to the default, never pass the garbage through.
        time_sync_tz_effective("UTC\x01garbage", out, sizeof(out));
        TEST_CHECK(strcmp(out, TIME_SYNC_TZ_DEFAULT) == 0, "corrupt stored TZ degrades to default UTC");
    }
    {
        char out[TIME_SYNC_TZ_MAX_LEN + 1];
        // Over-long stored value (e.g. a wider future format read by this
        // older build) also degrades rather than being silently truncated.
        char too_long[TIME_SYNC_TZ_MAX_LEN + 2];
        memset(too_long, 'B', TIME_SYNC_TZ_MAX_LEN + 1);
        too_long[TIME_SYNC_TZ_MAX_LEN + 1] = '\0';
        time_sync_tz_effective(too_long, out, sizeof(out));
        TEST_CHECK(strcmp(out, TIME_SYNC_TZ_DEFAULT) == 0, "over-long stored TZ degrades to default UTC");
    }

    // ---- time_sync_tz_effective(): valid stored value passes through -------
    {
        char out[TIME_SYNC_TZ_MAX_LEN + 1];
        time_sync_tz_effective("PST8PDT,M3.2.0,M11.1.0", out, sizeof(out));
        TEST_CHECK(strcmp(out, "PST8PDT,M3.2.0,M11.1.0") == 0, "valid stored TZ passed through unchanged");
    }

    // ---- time_sync_tz_effective(): valid-but-doesn't-fit-out_len degrades --
    // Exercises the `strlen(stored) < out_len` branch specifically: "EST5EDT"
    // is otherwise perfectly valid (passes time_sync_tz_is_valid() on its
    // own, checked below), so a failure here can only be the fits-in-out_len
    // check, not length/grammar rejection upstream of it -- the two other
    // degrade tests above already use inputs time_sync_tz_is_valid() itself
    // refuses, which would mask this branch rather than reach it.
    {
        TEST_CHECK(time_sync_tz_is_valid("EST5EDT"), "EST5EDT is independently valid (sanity check for the next case)");
        char out[5]; // room for 4 chars + NUL -- "EST5EDT" (7 chars) cannot fit
        time_sync_tz_effective("EST5EDT", out, sizeof(out));
        TEST_CHECK(strcmp(out, TIME_SYNC_TZ_DEFAULT) == 0,
                   "valid stored TZ that doesn't fit out_len degrades to default UTC");
    }

    // ---- POSIX TZ grammar: valid rule strings accepted ---------------------
    TEST_CHECK(time_sync_tz_is_valid("UTC0"), "std+offset only");
    TEST_CHECK(time_sync_tz_is_valid("PST8"), "std+offset, no sign on offset");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT"), "std+offset+dst, no dst offset, no rule");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT4"), "std+offset+dst+offset, no rule");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT,M3.2.0,M11.1.0"), "full US Eastern rule with M-form dates");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT,M3.2.0/2,M11.1.0/2"), "M-form dates with explicit /time");
    TEST_CHECK(time_sync_tz_is_valid("AEST-10AEDT,M10.1.0,M4.1.0/3"), "leading '-' offset (east of UTC)");
    TEST_CHECK(time_sync_tz_is_valid("<+07>-7"), "quoted <...> std name with numeric/sign body");
    TEST_CHECK(time_sync_tz_is_valid("NZST-12:00:00NZDT,M9.5.0,M4.1.0/3"), "hh:mm:ss offset precision");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT,J1,J365"), "Julian (no-leap-day) date rule");
    TEST_CHECK(time_sync_tz_is_valid("EST5EDT,0,364"), "plain Julian (with-leap-day) date rule");

    // ---- POSIX TZ grammar: refused ------------------------------------------
    // Each input below is short, printable ASCII and would pass the OLD
    // charset/length-only check -- the only thing that can refuse it now is
    // the grammar parser itself, proving Finding 2's actual failure mode
    // (an IANA name reaching setenv()/persist and silently staying UTC) is
    // closed.
    TEST_CHECK(!time_sync_tz_is_valid("America/Chicago"), "IANA zone name refused (the Finding 2 case)");
    TEST_CHECK(!time_sync_tz_is_valid("UTC"), "std name with no offset refused");
    TEST_CHECK(!time_sync_tz_is_valid("ES5EDT"), "std name under 3 letters refused");
    TEST_CHECK(!time_sync_tz_is_valid("EST5EDT,M13.1.0,M11.1.0"), "month 13 out of range refused");
    TEST_CHECK(!time_sync_tz_is_valid("EST5EDT,M3.6.0,M11.1.0"), "week 6 out of range refused");
    TEST_CHECK(!time_sync_tz_is_valid("EST5EDT,M3.2.7,M11.1.0"), "weekday 7 out of range refused");
    TEST_CHECK(!time_sync_tz_is_valid("EST5EDT,M3.2.0"), "only one rule (dst start with no end) refused");
    TEST_CHECK(!time_sync_tz_is_valid("EST25EDT"), "offset hour 25 out of range refused");
    TEST_CHECK(!time_sync_tz_is_valid("EST5EDT trailing junk"), "valid prefix with trailing garbage refused");
    TEST_CHECK(!time_sync_tz_is_valid("<+07-7"), "unterminated <...> quoted name refused");
}
