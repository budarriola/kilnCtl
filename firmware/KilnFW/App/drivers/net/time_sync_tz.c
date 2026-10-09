#include "time_sync_tz.h"

#include <ctype.h>
#include <string.h>

/* POSIX TZ grammar, hand-rolled per opus review Finding 2 --
 * "America/Chicago" (IANA form, not POSIX) is exactly the string an operator
 * will most naturally type, and the old check (1-63 printable-ASCII bytes)
 * accepted it: it is persisted, applied via setenv+tzset(), and echoed back
 * in /api/status.time_tz, while newlib's tzset() silently rejects it at
 * parse time and leaves the zone at UTC. No error anywhere; the UI shows
 * "America/Chicago" next to a UTC clock and a scheduled start lands hours
 * off. A fixed picker of known-good rule strings was the other option this
 * task allowed, but it forecloses any zone (with its own DST rule) the
 * picker's author didn't anticipate, and this codebase's own "refuse, never
 * clamp" convention favors validating the real grammar over curating a list.
 *
 * Grammar implemented (IEEE Std 1003.1-2017, "TZ" environment variable):
 *   std offset[dst[offset][,rule,rule]]
 * where
 *   std, dst  := 3+ alphabetic chars, or <...>-quoted alphanumeric/+/-
 *   offset    := [+-]hh[:mm[:ss]]           (hh 0-24, mm/ss 0-59)
 *   rule      := date[/time]
 *   date      := Jn (n 1-365, no leap day) | n (0-365) | Mm.w.d
 *                (m 1-12, w 1-5, d 0-6)
 *   time      := offset-shaped (hh[:mm[:ss]], sign allowed per POSIX)
 *
 * This is deliberately closer to strict-POSIX than "accept whatever glibc's
 * extensions allow" -- rejecting a few glibc-only forms is a safe direction
 * to err in (this firmware links newlib, not glibc), never the reverse. */

static bool parse_number(const char **p, int *val, int max_digits)
{
    const char *s = *p;
    int n = 0;
    int digits = 0;
    while (isdigit((unsigned char)*s) && digits < max_digits) {
        n = n * 10 + (*s - '0');
        s++;
        digits++;
    }
    if (digits == 0) {
        return false;
    }
    *val = n;
    *p = s;
    return true;
}

static bool parse_name(const char **p)
{
    const char *s = *p;
    if (*s == '<') {
        s++;
        const char *start = s;
        while (isalnum((unsigned char)*s) || *s == '+' || *s == '-') {
            s++;
        }
        if (s == start || *s != '>') {
            return false;
        }
        s++;
    } else {
        const char *start = s;
        while (isalpha((unsigned char)*s)) {
            s++;
        }
        if (s - start < 3) {
            return false;
        }
    }
    *p = s;
    return true;
}

/* offset := [+-]hh[:mm[:ss]] -- also used, per POSIX, for a rule's optional
 * /time suffix, so no separate range limit on hh here (POSIX allows an
 * unbounded-looking hh for /time; the 2-digit cap below still bounds it to
 * something sane and rejects garbage like a 9-digit number). */
static bool parse_offset(const char **p)
{
    const char *s = *p;
    if (*s == '+' || *s == '-') {
        s++;
    }
    int h;
    if (!parse_number(&s, &h, 2) || h > 24) {
        return false;
    }
    if (*s == ':') {
        s++;
        int m;
        if (!parse_number(&s, &m, 2) || m > 59) {
            return false;
        }
        if (*s == ':') {
            s++;
            int sec;
            if (!parse_number(&s, &sec, 2) || sec > 59) {
                return false;
            }
        }
    }
    *p = s;
    return true;
}

static bool parse_date(const char **p)
{
    const char *s = *p;
    if (*s == 'J') {
        s++;
        int n;
        if (!parse_number(&s, &n, 3) || n < 1 || n > 365) {
            return false;
        }
    } else if (*s == 'M') {
        s++;
        int m, w, d;
        if (!parse_number(&s, &m, 2) || m < 1 || m > 12) {
            return false;
        }
        if (*s != '.') {
            return false;
        }
        s++;
        if (!parse_number(&s, &w, 1) || w < 1 || w > 5) {
            return false;
        }
        if (*s != '.') {
            return false;
        }
        s++;
        if (!parse_number(&s, &d, 1) || d > 6) {
            return false;
        }
    } else {
        int n;
        if (!parse_number(&s, &n, 3) || n > 365) {
            return false;
        }
    }
    *p = s;
    return true;
}

static bool parse_rule(const char **p)
{
    const char *s = *p;
    if (!parse_date(&s)) {
        return false;
    }
    if (*s == '/') {
        s++;
        if (!parse_offset(&s)) {
            return false;
        }
    }
    *p = s;
    return true;
}

/* Full "std offset[dst[offset][,rule,rule]]" grammar over an already
 * charset/length-validated string (time_sync_tz_is_valid() runs this only
 * after the printable-ASCII/length gate, so parse_name()/parse_number() can
 * assume no embedded control bytes). Returns false on any leftover,
 * unparsed trailing bytes -- a prefix match is not a match. */
static bool tz_grammar_valid(const char *tz)
{
    const char *s = tz;
    if (!parse_name(&s)) {
        return false;
    }
    if (!parse_offset(&s)) {
        return false;
    }
    if (*s == '\0') {
        return true; /* "std offset" alone -- e.g. "UTC0", "PST8" */
    }
    if (!parse_name(&s)) {
        return false; /* dst name required once anything follows std offset */
    }
    if (*s != '\0' && *s != ',') {
        if (!parse_offset(&s)) {
            return false; /* optional dst offset */
        }
    }
    if (*s == '\0') {
        return true; /* "std offset dst" -- dst offset defaults to std-1h */
    }
    if (*s != ',') {
        return false;
    }
    s++;
    if (!parse_rule(&s) || *s != ',') {
        return false;
    }
    s++;
    if (!parse_rule(&s) || *s != '\0') {
        return false;
    }
    return true;
}

bool time_sync_tz_is_valid(const char *tz)
{
    if (tz == NULL) {
        return false;
    }
    size_t len = strlen(tz);
    if (len == 0 || len > TIME_SYNC_TZ_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)tz[i];
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    /* Charset/length gate passed -- now require the actual POSIX TZ grammar
     * newlib's tzset() implements, so a string that would silently degrade
     * to UTC can never reach setenv()/persist (Finding 2). */
    return tz_grammar_valid(tz);
}

void time_sync_tz_effective(const char *stored, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    if (stored != NULL && time_sync_tz_is_valid(stored) && strlen(stored) < out_len) {
        strncpy(out, stored, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }
    strncpy(out, TIME_SYNC_TZ_DEFAULT, out_len - 1);
    out[out_len - 1] = '\0';
}
