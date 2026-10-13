// json_escape_ctl.h -- the ONE JSON string escaper for HTTP emitters (F5,
// WEB_UI_XSS_AUDIT_2026-10-09). Escapes '"' and '\\' and emits \u00XX for
// control bytes (< 0x20) and DEL (0x7f), so a stored name or an on-air SSID
// holding a raw control byte can never make a JSON document unparseable.
// out_cap includes the NUL. When the escaped text does not fit it is cut at a
// character boundary (output always valid JSON, never a half escape).
#ifndef KILNCTL_JSON_ESCAPE_CTL_H
#define KILNCTL_JSON_ESCAPE_CTL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void kiln_json_escape_ctl(const char *src, char *out, size_t out_cap)
{
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    if (out_cap == 0) {
        return;
    }
    for (const char *p = src; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            if (o + 3 > out_cap) {
                break;
            }
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20 || c == 0x7f) {
            if (o + 7 > out_cap) {
                break;
            }
            out[o++] = '\\';
            out[o++] = 'u';
            out[o++] = '0';
            out[o++] = '0';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xf];
        } else {
            if (o + 2 > out_cap) {
                break;
            }
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_JSON_ESCAPE_CTL_H
