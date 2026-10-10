// Private shim for test_dashboard_http_relay.c: MSVC has no <strings.h>;
// dashboard_http.c includes it only for strcasecmp.
#ifndef DASHBOARD_HTTP_RELAY_STRINGS_H
#define DASHBOARD_HTTP_RELAY_STRINGS_H
#include <string.h>
#ifndef strcasecmp
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif
#endif
