// update_url.c -- see update_url.h.
#include "update_url.h"

#include <string.h>

#include "update_semver.h"

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool ieq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

static bool host_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

update_url_err_t update_url_parse(const char *url, char *host, size_t host_cap, const char **path_out)
{
    if (host != NULL && host_cap > 0) {
        host[0] = '\0';
    }
    if (path_out != NULL) {
        *path_out = NULL;
    }
    if (url == NULL || url[0] == '\0') {
        return UPDATE_URL_E_EMPTY;
    }
    size_t len = strnlen(url, UPDATE_URL_MAX);
    if (len >= UPDATE_URL_MAX) {
        return UPDATE_URL_E_TOO_LONG;
    }
    // No whitespace, control characters or backslashes anywhere.
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 0x20 || c >= 0x7F || c == '\\') {
            return UPDATE_URL_E_MALFORMED;
        }
    }
    static const char scheme[] = "https://";
    if (len < sizeof(scheme) - 1 || !ieq_n(url, scheme, sizeof(scheme) - 1)) {
        return UPDATE_URL_E_SCHEME;
    }
    const char *auth = url + (sizeof(scheme) - 1);
    size_t alen = 0;
    while (auth[alen] != '\0' && auth[alen] != '/' && auth[alen] != '?' && auth[alen] != '#') {
        alen++;
    }
    if (alen == 0) {
        return UPDATE_URL_E_MALFORMED;
    }
    size_t hlen = alen;
    for (size_t i = 0; i < alen; i++) {
        if (auth[i] == '@' || auth[i] == '[' || auth[i] == ']') {
            return UPDATE_URL_E_MALFORMED; // userinfo / IPv6 literal
        }
        if (auth[i] == ':') {
            hlen = i;
            break;
        }
    }
    if (hlen < alen) {
        // Only ":443" is accepted.
        if (!(alen - hlen == 4 && memcmp(auth + hlen, ":443", 4) == 0)) {
            return UPDATE_URL_E_MALFORMED;
        }
    }
    if (hlen == 0 || hlen >= UPDATE_HOST_MAX) {
        return UPDATE_URL_E_MALFORMED;
    }
    if (auth[0] == '.' || auth[0] == '-' || auth[hlen - 1] == '.' || auth[hlen - 1] == '-') {
        return UPDATE_URL_E_MALFORMED;
    }
    char tmp[UPDATE_HOST_MAX];
    for (size_t i = 0; i < hlen; i++) {
        char c = lower(auth[i]);
        if (!host_char_ok(c)) {
            return UPDATE_URL_E_MALFORMED;
        }
        if (c == '.' && i > 0 && auth[i - 1] == '.') {
            return UPDATE_URL_E_MALFORMED;
        }
        tmp[i] = c;
    }
    tmp[hlen] = '\0';
    if (host != NULL) {
        if (host_cap <= hlen) {
            return UPDATE_URL_E_MALFORMED;
        }
        memcpy(host, tmp, hlen + 1);
    }
    if (path_out != NULL) {
        const char *rest = auth + alen;
        *path_out = (*rest == '/') ? rest : "/";
    }
    return UPDATE_URL_OK;
}

bool update_host_allowed(const char *host)
{
    static const char *const allowed[] = {
        "api.github.com",
        "github.com",
        "objects.githubusercontent.com",
        "release-assets.githubusercontent.com",
    };
    if (host == NULL) {
        return false;
    }
    size_t n = strlen(host);
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strlen(allowed[i]) == n && ieq_n(host, allowed[i], n)) {
            return true;
        }
    }
    return false;
}

update_url_err_t update_url_check(const char *url, char *host, size_t host_cap)
{
    char h[UPDATE_HOST_MAX];
    update_url_err_t e = update_url_parse(url, h, sizeof(h), NULL);
    if (e != UPDATE_URL_OK) {
        return e;
    }
    if (!update_host_allowed(h)) {
        return UPDATE_URL_E_HOST;
    }
    if (host != NULL && host_cap > 0) {
        size_t n = strlen(h);
        if (host_cap <= n) {
            return UPDATE_URL_E_MALFORMED;
        }
        memcpy(host, h, n + 1);
    }
    return UPDATE_URL_OK;
}

update_url_err_t update_redirect_check(unsigned hops_followed, const char *location)
{
    if (hops_followed >= UPDATE_MAX_REDIRECTS) {
        return UPDATE_URL_E_TOO_MANY_HOPS;
    }
    return update_url_check(location, NULL, 0);
}

const char *update_url_err_name(update_url_err_t e)
{
    switch (e) {
    case UPDATE_URL_OK: return "ok";
    case UPDATE_URL_E_EMPTY: return "empty_url";
    case UPDATE_URL_E_TOO_LONG: return "url_too_long";
    case UPDATE_URL_E_SCHEME: return "not_https";
    case UPDATE_URL_E_MALFORMED: return "malformed_url";
    case UPDATE_URL_E_HOST: return "host_not_allowed";
    case UPDATE_URL_E_TOO_MANY_HOPS: return "too_many_redirects";
    }
    return "unknown";
}

static bool repo_part_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-';
}

bool update_repo_valid(const char *repo)
{
    if (repo == NULL) {
        return false;
    }
    size_t len = strnlen(repo, UPDATE_REPO_MAX);
    if (len >= UPDATE_REPO_MAX || len < 3 || len > UPDATE_REPO_VALID_MAX_LEN) {
        return false;
    }
    const char *slash = memchr(repo, '/', len);
    if (slash == NULL) {
        return false;
    }
    if (memchr(slash + 1, '/', len - (size_t)(slash - repo) - 1) != NULL) {
        return false;
    }
    size_t olen = (size_t)(slash - repo);
    size_t nlen = len - olen - 1;
    if (olen < 1 || olen > 39 || nlen < 1 || nlen > 100) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (repo + i == slash) {
            continue;
        }
        if (!repo_part_char_ok(repo[i])) {
            return false;
        }
    }
    if (strstr(repo, "..") != NULL) {
        return false;
    }
    if (repo[0] == '-' || repo[olen - 1] == '-') {
        return false;
    }
    // A segment starting with '.' (this includes "." and "..") is refused: GitHub rejects such
    // owners, and a "." / ".." segment would be a path-traversal spelling once the fetcher
    // builds /repos/<owner>/<name>/... from it.
    const char *name = slash + 1;
    if (repo[0] == '.' || name[0] == '.') {
        return false;
    }
    return true;
}

bool update_url_build_latest(const char *repo, char *out, size_t out_cap)
{
    static const char pre[] = "https://api.github.com/repos/";
    static const char post[] = "/releases/latest";
    if (out == NULL || out_cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (!update_repo_valid(repo)) {
        return false;
    }
    size_t rl = strlen(repo);
    if (out_cap < sizeof(pre) - 1 + rl + sizeof(post)) {
        return false;
    }
    memcpy(out, pre, sizeof(pre) - 1);
    memcpy(out + sizeof(pre) - 1, repo, rl);
    memcpy(out + sizeof(pre) - 1 + rl, post, sizeof(post));
    return true;
}

bool update_tag_valid(const char *tag)
{
    if (tag == NULL || tag[0] != 'v') {
        return false;
    }
    size_t len = strnlen(tag, 40);
    if (len < 6 || len > 32) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        char c = tag[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-';
        if (!ok) {
            return false; // in particular no '+', '/', '%', whitespace
        }
    }
    update_semver_t v;
    return update_semver_parse(tag, &v);
}

bool update_asset_url_matches(const char *url, const char *repo, const char *tag, const char *name)
{
    if (url == NULL || repo == NULL || tag == NULL || name == NULL || !update_repo_valid(repo) ||
        !update_tag_valid(tag) || name[0] == '\0') {
        return false;
    }
    char host[UPDATE_HOST_MAX];
    const char *path = NULL;
    if (update_url_parse(url, host, sizeof(host), &path) != UPDATE_URL_OK || path == NULL) {
        return false;
    }
    if (strcmp(host, "github.com") != 0) {
        return false;
    }
    // path must be exactly "/" repo "/releases/download/" tag "/" name
    static const char mid[] = "/releases/download/";
    size_t rl = strlen(repo);
    size_t tl = strlen(tag);
    size_t nl = strlen(name);
    size_t want = 1 + rl + (sizeof(mid) - 1) + tl + 1 + nl;
    if (strlen(path) != want || path[0] != '/') {
        return false;
    }
    const char *p = path + 1;
    if (!ieq_n(p, repo, rl)) {
        return false;
    }
    p += rl;
    if (memcmp(p, mid, sizeof(mid) - 1) != 0) {
        return false;
    }
    p += sizeof(mid) - 1;
    if (memcmp(p, tag, tl) != 0) {
        return false;
    }
    p += tl;
    if (*p != '/') {
        return false;
    }
    p++;
    return memcmp(p, name, nl) == 0;
}

// ---- Location capture ---------------------------------------------------------------------------
void update_loc_capture_init(update_loc_capture_t *c, char *buf, size_t cap)
{
    if (c == NULL) {
        return;
    }
    c->buf = buf;
    c->cap = cap;
    c->seen = false;
    c->refused = (buf == NULL || cap == 0);
    if (buf != NULL && cap > 0) {
        buf[0] = '\0';
    }
}

void update_loc_capture_feed(update_loc_capture_t *c, const char *key, const char *value)
{
    if (c == NULL || key == NULL || !(strlen(key) == 8 && ieq_n(key, "Location", 8))) {
        return;
    }
    if (c->seen || value == NULL || c->buf == NULL) {
        c->refused = true; // a second Location, or nothing to read: ambiguous
        return;
    }
    c->seen = true;
    size_t n = strnlen(value, c->cap);
    if (n >= c->cap) {
        c->refused = true; // would not fit with its NUL: never truncate a redirect target
        c->buf[0] = '\0';
        return;
    }
    memcpy(c->buf, value, n);
    c->buf[n] = '\0';
}

const char *update_loc_capture_get(const update_loc_capture_t *c, bool *refused)
{
    if (refused != NULL) {
        *refused = c != NULL && c->refused;
    }
    if (c == NULL || c->refused || !c->seen || c->buf == NULL || c->buf[0] == '\0') {
        return NULL;
    }
    return c->buf;
}
