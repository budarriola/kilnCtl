// Host-test stand-in for POSIX <strings.h> -- MSVC has no such header. Only
// strcasecmp() is needed by anything in this tree so far (dashboard_http.c's
// unit_pref_post_handler() "fahrenheit"/"celsius" match); mapped straight to
// MSVC's own _stricmp(), which is byte-for-byte the same case-insensitive
// comparison. Add more POSIX string functions here only as a real caller
// needs them -- this is not meant to become a general compat shim.
#ifndef KILNCTL_HOST_STUB_STRINGS_H
#define KILNCTL_HOST_STUB_STRINGS_H

#include <string.h>

static inline int strcasecmp(const char *a, const char *b)
{
    return _stricmp(a, b);
}

#endif
