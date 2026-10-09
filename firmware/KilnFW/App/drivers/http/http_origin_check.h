// http_origin_check.h -- CSRF / cross-origin guard for state-changing requests
// (ROUTE_TIER_REVIEW_2026-10-09 MED-1). Header-only, pure, no ESP-IDF: shared by
// the application's kiln_http_prehandler() and the recovery image's httpd, and
// host-tested in test_http_origin_check.c.
//
// Rule (non-GET/HEAD only): Origin present -> must be non-"null" and its
// host[:port] must equal the Host header's (host case-insensitive, missing port
// = 80). Origin absent -> Referer's scheme://host[:port] gets the same compare.
// Both absent -> allowed (MCP tools, curl and the LCD send neither).
#ifndef KILNCTL_HTTP_ORIGIN_CHECK_H
#define KILNCTL_HTTP_ORIGIN_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Size of the fixed header buffers callers use (Origin/Referer/Host).
#define HTTP_ORIGIN_HDR_BUF 96

typedef struct {
    char host[HTTP_ORIGIN_HDR_BUF];
    int port;
} http_origin_hp_t;

static inline char http_origin_lc_(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Parse "host[:port]" (authority); stops at '/', '?', '#', end. false = malformed.
static inline bool http_origin_parse_authority_(const char *s, http_origin_hp_t *out) {
    size_t n = 0;
    if (*s == '[') { // bracketed IPv6
        while (s[n] != '\0' && s[n] != ']') {
            n++;
        }
        if (s[n] != ']') {
            return false;
        }
        n++;
    } else {
        while (s[n] != '\0' && s[n] != ':' && s[n] != '/' && s[n] != '?' && s[n] != '#') {
            n++;
        }
    }
    if (n == 0 || n >= sizeof(out->host)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        out->host[i] = http_origin_lc_(s[i]);
    }
    out->host[n] = '\0';
    out->port = 80;
    if (s[n] == ':') {
        int p = 0;
        size_t d = 0;
        n++;
        while (s[n] >= '0' && s[n] <= '9' && d < 6) {
            p = p * 10 + (s[n] - '0');
            n++;
            d++;
        }
        if (d == 0 || p > 65535) {
            return false;
        }
        out->port = p;
    }
    return s[n] == '\0' || s[n] == '/' || s[n] == '?' || s[n] == '#';
}

// URL form: skip "scheme://" then parse the authority.
static inline bool http_origin_parse_url_(const char *s, http_origin_hp_t *out) {
    const char *p = strstr(s, "://");
    if (p == NULL || p == s) {
        return false;
    }
    return http_origin_parse_authority_(p + 3, out);
}

// Returns true when the request must be REFUSED (cross-origin).
//   origin/referer: header value, or NULL when absent.
//   host:           Host header value, or NULL when absent.
//   overlong:       caller found a relevant header too long for its buffer.
static inline bool http_origin_is_cross_origin(const char *origin, const char *referer,
                                               const char *host, bool overlong) {
    if (overlong) {
        return true;
    }
    const char *src = origin != NULL ? origin : referer;
    if (src == NULL) {
        return false; // no browser provenance headers: non-browser client
    }
    if (origin != NULL && strcmp(origin, "null") == 0) {
        return true;
    }
    http_origin_hp_t a, b;
    if (host == NULL || !http_origin_parse_url_(src, &a) || !http_origin_parse_authority_(host, &b)) {
        return true;
    }
    return !(a.port == b.port && strcmp(a.host, b.host) == 0);
}

// ---- Header-extraction glue (shared by both httpd call sites; host-tested with fakes) ----
// Callbacks wrap httpd_req_get_hdr_value_len()/_str(). get_str returns 0 = ok,
// 1 = value copied but TRUNCATED to cap-1 chars, <0 = not available.
typedef size_t (*http_origin_hdr_len_fn)(void *c, const char *name);
typedef int (*http_origin_hdr_str_fn)(void *c, const char *name, char *buf, size_t cap);

// A Referer longer than the buffer is legitimate (long query strings): only
// scheme://host[:port] is compared, so a truncated copy is usable iff the
// authority ends (a '/', '?' or '#') inside what we kept.
static inline bool http_origin_url_authority_complete_(const char *s) {
    const char *p = strstr(s, "://");
    if (p == NULL) {
        return false;
    }
    for (p += 3; *p != '\0'; p++) {
        if (*p == '/' || *p == '?' || *p == '#') {
            return true;
        }
    }
    return false;
}

// Full MED-1 decision for one non-GET/HEAD request. true = REFUSE.
// Caller decides GET/HEAD from the REAL request method (req->method), not the
// method a route was registered under. Reverse-proxy Host rewriting is refused
// by design (Origin host != Host header).
static inline bool http_origin_request_is_cross_origin(void *c, http_origin_hdr_len_fn len_fn,
                                                       http_origin_hdr_str_fn get_fn) {
    char origin[HTTP_ORIGIN_HDR_BUF], referer[HTTP_ORIGIN_HDR_BUF], host[HTTP_ORIGIN_HDR_BUF];
    const char *o = NULL, *r = NULL, *h = NULL;
    bool overlong = false;
    size_t n = len_fn(c, "Origin");
    if (n >= sizeof(origin)) {
        overlong = true;
    } else if (n > 0 && get_fn(c, "Origin", origin, sizeof(origin)) == 0) {
        o = origin;
    }
    if (o == NULL && !overlong) {
        n = len_fn(c, "Referer");
        if (n > 0) {
            int rc = get_fn(c, "Referer", referer, sizeof(referer));
            if (rc == 0 || (rc == 1 && http_origin_url_authority_complete_(referer))) {
                r = referer;
            } else if (rc == 1) {
                overlong = true;
            }
        }
    }
    if (o != NULL || r != NULL) {
        n = len_fn(c, "Host");
        if (n >= sizeof(host)) {
            overlong = true;
        } else if (n > 0 && get_fn(c, "Host", host, sizeof(host)) == 0) {
            h = host;
        }
    }
    return http_origin_is_cross_origin(o, r, h, overlong);
}

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_HTTP_ORIGIN_CHECK_H
