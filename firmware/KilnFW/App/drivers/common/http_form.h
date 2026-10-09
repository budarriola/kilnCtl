// http_form -- tiny application/x-www-form-urlencoded helpers shared by
// every HTTP POST handler on this board (wifi_provision_http.c,
// dashboard_http.c). static inline, header-only: small enough that a shared
// .c/.o pair would be more build-graph ceremony than the code it saves.
#ifndef HTTP_FORM_H
#define HTTP_FORM_H

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Percent-decodes src[0..src_len) into out (which must hold at least
 * out_cap bytes, NUL included). '+' decodes to space, per
 * application/x-www-form-urlencoded. Returns the decoded length (< out_cap)
 * on success, or -1 if the decoded value would not fit -- the caller must
 * treat that as a rejected request, not a truncated one, since silently
 * truncating e.g. a password is worse than refusing it. */
static inline int http_form_url_decode(const char *src, size_t src_len, char *out, size_t out_cap)
{
    size_t o = 0;
    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];
        if (o + 1 >= out_cap) {
            return -1;
        }
        if (c == '+') {
            out[o++] = ' ';
        } else if (c == '%' && i + 2 < src_len && isxdigit((unsigned char)src[i + 1]) &&
                   isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return (int)o;
}

/* Finds "key=" as a whole &-delimited field in body and decodes its value
 * into out. Returns decoded length (>=0) if found and it fits, -1 if not
 * present, -2 if present but too long to decode into out_cap. */
static inline int http_form_find_field(const char *body, const char *key, char *out, size_t out_cap)
{
    size_t key_len = strlen(key);
    const char *p = body;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t field_len = amp ? (size_t)(amp - p) : strlen(p);
        if (field_len > key_len && p[key_len] == '=' && strncmp(p, key, key_len) == 0) {
            int decoded = http_form_url_decode(p + key_len + 1, field_len - key_len - 1, out, out_cap);
            return decoded < 0 ? -2 : decoded;
        }
        if (!amp) {
            break;
        }
        p = amp + 1;
    }
    return -1;
}

#ifdef __cplusplus
}
#endif

#endif // HTTP_FORM_H
