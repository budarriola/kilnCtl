// web_encoding -- see web_encoding.h for the RFC 9110 s12.5.3 rationale and
// the flash-budget measurement that made gzip the only stored representation
// of the embedded pages (TODO.md 10.6a).
#include "web_encoding.h"

#include <stddef.h>

#include "esp_log.h"

/* The uncompressed (no Content-Encoding) body sent to a client that
 * explicitly refused gzip. Deliberately tiny -- flash is at 5% free. */
#define WEB_ENCODING_406_BODY                                                  \
    "406 Not Acceptable: this server stores its web pages only as "            \
    "gzip-compressed blobs in flash and cannot send them uncompressed. "       \
    "Your Accept-Encoding header excluded gzip. Retry allowing gzip "          \
    "(curl: use --compressed).\n"

static bool ci_eq(const char *a, size_t a_len, const char *lit)
{
    size_t i = 0;
    for (; i < a_len; i++) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (lit[i] == '\0' || c != lit[i]) {
            return false;
        }
    }
    return lit[i] == '\0';
}

/* True when the qvalue text (everything after "q=") is zero: "0", "0.",
 * "0.0", "0.000". Anything else (including garbage) counts as non-zero,
 * i.e. acceptable -- fail open rather than 406 a real browser. */
static bool q_is_zero(const char *s, const char *end)
{
    while (s < end && (*s == ' ' || *s == '\t')) {
        s++;
    }
    if (s >= end || *s != '0') {
        return false;
    }
    s++;
    if (s < end && *s == '.') {
        s++;
        while (s < end && *s == '0') {
            s++;
        }
    }
    while (s < end && (*s == ' ' || *s == '\t')) {
        s++;
    }
    return s == end;
}

bool web_client_accepts_gzip(httpd_req_t *req)
{
    char enc[128];
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", enc, sizeof(enc)) != ESP_OK) {
        return true; /* absent (or too long to parse): anything is acceptable */
    }

    int gzip_ok = -1; /* -1 = not mentioned, 0 = q=0, 1 = acceptable */
    int star_ok = -1;

    const char *p = enc;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *elem = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        const char *elem_end = p; /* one past this element */

        /* Split coding name from its parameters. */
        const char *semi = elem;
        while (semi < elem_end && *semi != ';') {
            semi++;
        }
        const char *name_end = semi;
        while (name_end > elem && (name_end[-1] == ' ' || name_end[-1] == '\t')) {
            name_end--;
        }

        bool acceptable = true;
        if (semi < elem_end) {
            /* Scan params for q=<value>; last one wins, like a real parser. */
            const char *q = semi;
            while (q < elem_end) {
                while (q < elem_end && (*q == ';' || *q == ' ' || *q == '\t')) {
                    q++;
                }
                const char *param = q;
                while (q < elem_end && *q != ';') {
                    q++;
                }
                const char *param_end = q;
                if ((size_t)(param_end - param) >= 2 && (param[0] == 'q' || param[0] == 'Q')) {
                    const char *eq = param + 1;
                    while (eq < param_end && (*eq == ' ' || *eq == '\t')) {
                        eq++;
                    }
                    if (eq < param_end && *eq == '=') {
                        acceptable = !q_is_zero(eq + 1, param_end);
                    }
                }
            }
        }

        size_t name_len = (size_t)(name_end - elem);
        if (ci_eq(elem, name_len, "gzip") || ci_eq(elem, name_len, "x-gzip")) {
            gzip_ok = acceptable ? 1 : 0;
        } else if (name_len == 1 && elem[0] == '*') {
            star_ok = acceptable ? 1 : 0;
        }
    }

    if (gzip_ok >= 0) {
        return gzip_ok == 1;
    }
    if (star_ok >= 0) {
        return star_ok == 1;
    }
    return false; /* header present, gzip not covered by it */
}

esp_err_t web_send_gzip_not_acceptable(httpd_req_t *req, const char *tag, const char *page_name)
{
    char enc[128];
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", enc, sizeof(enc)) != ESP_OK) {
        enc[0] = '\0';
    }
    ESP_LOGW(tag, "%s: client Accept-Encoding \"%s\" excludes gzip; only a gzip "
                  "representation is stored (TODO.md 10.6a) -- 406", page_name, enc);
    httpd_resp_set_status(req, "406 Not Acceptable");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, WEB_ENCODING_406_BODY, HTTPD_RESP_USE_STRLEN);
}

void web_set_asset_cache_headers(httpd_req_t *req)
{
    /* See web_encoding.h for why this exists and why it is no-cache rather
     * than no-store. httpd_resp_set_hdr() does not copy the value, so the
     * string must outlive the response -- a string literal does. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
}
