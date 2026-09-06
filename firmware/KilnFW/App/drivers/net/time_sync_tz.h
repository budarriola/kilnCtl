#pragma once

/* Pure, dependency-free (stdbool/stdint/string only -- no ESP-IDF, no
 * FreeRTOS) POSIX-TZ validation and degrade-to-UTC logic, factored out of
 * time_sync.c the same way safety_trip_decision.c was factored out of
 * safety_link.c: so the decision itself can be host-tested without pulling
 * in esp_netif_sntp.h/nvs.h, and so time_sync.c's real NVS-backed TZ load
 * has exactly one place that decides "is this string usable" rather than
 * re-deriving it inline.
 *
 * "Refuse, never clamp" (PID_EXPANSION_PLAN.md's rule, restated in
 * zones_http.c's parse_zone_fields() comment): time_sync_tz_is_valid()
 * rejects outright rather than truncating or substituting a close guess --
 * the caller (settings_http.c's TZ setter) refuses the whole request on
 * false, and time_sync.c's own NVS load treats a stored value that fails
 * this check as "never validly set" and falls back to UTC, never a mangled
 * partial string. */

#include <stdbool.h>
#include <stddef.h>

/* POSIX TZ strings ("EST5EDT,M3.2.0,M11.1.0", "America/Chicago" is NOT a
 * valid libc TZ value -- glibc/newlib TZ is the POSIX rule-string form, not
 * an IANA zone name) are short. 63 chars is generous headroom over any real
 * rule string while keeping the persisted NVS blob and the in-RAM copy
 * small. +1 for the terminator is the caller's job, same convention as
 * ZONE_NAME_MAX_LEN in zones_http.h. */
#define TIME_SYNC_TZ_MAX_LEN 63

/* UTC0's the sole hardcoded degrade target -- see time_sync_tz_effective()
 * below. Passes time_sync_tz_is_valid() itself (short, printable ASCII),
 * so a caller that re-validates the output of time_sync_tz_effective()
 * never needs a second special case for it. */
#define TIME_SYNC_TZ_DEFAULT "UTC0"

/* Accepts iff tz is non-NULL, non-empty, at most TIME_SYNC_TZ_MAX_LEN bytes
 * (not counting the terminator), every byte is printable ASCII (0x20..0x7E
 * per isprint() in the "C" locale -- deliberately hand-rolled rather than
 * calling isprint() itself, since isprint() is locale- and signedness-of-
 * char sensitive and a corrupt NVS byte can be negative as a plain `char`),
 * AND the string parses as a POSIX TZ rule: `std offset[dst[offset]
 * [,rule,rule]]` (IEEE Std 1003.1-2017). That last clause exists because
 * printable-ASCII alone let "America/Chicago" -- an IANA zone name, NOT a
 * valid POSIX TZ value, and exactly what an operator will type first --
 * through: it persisted, applied via setenv()/tzset(), and was echoed back
 * to the UI while newlib's tzset() silently rejected it and left the clock
 * on UTC, with no error surfaced anywhere. A tab, newline, non-ASCII byte,
 * or IANA-style name is refused, not stripped or guessed at -- this is the
 * "refuse at the door" check settings_http.c's TZ setter and time_sync.c's
 * NVS load both call before trusting a string. */
bool time_sync_tz_is_valid(const char *tz);

/* Copies the TZ string to actually apply into out (out_len >= 1) given a
 * possibly-NULL, possibly-corrupt `stored` candidate (e.g. read back from
 * NVS, or absent because the key was never written). If stored is NULL or
 * fails time_sync_tz_is_valid(), or does not fit in out, writes
 * TIME_SYNC_TZ_DEFAULT ("UTC0") instead -- this is the ONE place "unset or
 * corrupt degrades to UTC, never garbage" is decided; time_sync.c's boot
 * path and its host test both go through this same function so they cannot
 * drift apart. Always NUL-terminates out. */
void time_sync_tz_effective(const char *stored, char *out, size_t out_len);
