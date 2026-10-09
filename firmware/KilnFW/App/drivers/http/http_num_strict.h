// http_num_strict.h -- strict decimal parsing for HTTP query/form values.
// strtoul() accepts a leading '-' (wraps) and an empty string (returns 0 with
// endp == s). HTTP input audit L7 (2026-10-09). Header-only, host-testable.
#ifndef KILNCTL_HTTP_NUM_STRICT_H
#define KILNCTL_HTTP_NUM_STRICT_H

#include <stdbool.h>
#include <stdlib.h>

// true only for a non-empty run of ASCII digits (no sign, no space, no suffix)
// whose value fits an unsigned long.
static inline bool http_num_parse_ulong(const char *s, unsigned long *out) {
    if (s == NULL || s[0] < '0' || s[0] > '9') {
        return false;
    }
    char *endp = NULL;
    unsigned long v = strtoul(s, &endp, 10);
    if (endp == NULL || *endp != '\0') {
        return false;
    }
    for (const char *p = s; *p != '\0'; p++) { /* strtoul saturates on overflow: refuse */
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    if (v == (unsigned long)-1 && s[0] != '\0') {
        /* ULONG_MAX is only legitimate if the text is exactly that value; cheap guard:
         * overflow is reported via errno, which callers do not share, so refuse the
         * saturated value outright (no HTTP field here needs it). */
        return false;
    }
    *out = v;
    return true;
}

#endif
