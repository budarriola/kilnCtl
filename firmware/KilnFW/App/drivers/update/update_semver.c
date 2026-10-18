// update_semver.c -- see update_semver.h.
#include "update_semver.h"

#include <stdio.h>
#include <string.h>

#define SEMVER_MAX_LEN 64u
#define SEMVER_MAX_DIGITS 9u

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_ident_char(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

// Parse digits at s[*i..len); no leading zeros ("0" alone is fine).
static bool parse_num(const char *s, size_t len, size_t *i, uint32_t *out)
{
    size_t start = *i;
    uint32_t v = 0;
    while (*i < len && is_digit(s[*i])) {
        if (*i - start >= SEMVER_MAX_DIGITS) {
            return false;
        }
        v = v * 10u + (uint32_t)(s[*i] - '0');
        (*i)++;
    }
    size_t n = *i - start;
    if (n == 0) {
        return false;
    }
    if (n > 1 && s[start] == '0') {
        return false;
    }
    *out = v;
    return true;
}

// Validate dot-separated identifiers in s[b..e): non-empty, charset, and (for
// prerelease) numeric identifiers without leading zeros.
static bool valid_identifiers(const char *s, size_t b, size_t e, bool forbid_numeric_leading_zero)
{
    if (b >= e) {
        return false;
    }
    size_t i = b;
    for (;;) {
        size_t j = i;
        bool all_digits = true;
        while (j < e && s[j] != '.') {
            if (!is_ident_char(s[j])) {
                return false;
            }
            if (!is_digit(s[j])) {
                all_digits = false;
            }
            j++;
        }
        size_t n = j - i;
        if (n == 0) {
            return false;
        }
        if (forbid_numeric_leading_zero && all_digits && n > 1 && s[i] == '0') {
            return false;
        }
        if (j >= e) {
            break;
        }
        i = j + 1;
    }
    return true;
}

bool update_semver_parse_n(const char *s, size_t len, update_semver_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (s == NULL || len == 0 || len > SEMVER_MAX_LEN) {
        return false;
    }
    size_t i = 0;
    if (s[0] == 'v' || s[0] == 'V') {
        i = 1;
    }
    update_semver_t t;
    memset(&t, 0, sizeof(t));
    if (!parse_num(s, len, &i, &t.major) || i >= len || s[i] != '.') {
        return false;
    }
    i++;
    if (!parse_num(s, len, &i, &t.minor) || i >= len || s[i] != '.') {
        return false;
    }
    i++;
    if (!parse_num(s, len, &i, &t.patch)) {
        return false;
    }
    if (i < len && s[i] == '-') {
        size_t b = i + 1;
        size_t e = b;
        while (e < len && s[e] != '+') {
            e++;
        }
        if (!valid_identifiers(s, b, e, true) || (e - b) > UPDATE_SEMVER_PRE_MAX) {
            return false;
        }
        memcpy(t.pre, s + b, e - b);
        t.pre[e - b] = '\0';
        t.has_pre = true;
        i = e;
    }
    if (i < len && s[i] == '+') {
        if (!valid_identifiers(s, i + 1, len, false)) {
            return false;
        }
        i = len;
    }
    if (i != len) {
        return false;
    }
    *out = t;
    return true;
}

bool update_semver_parse(const char *s, update_semver_t *out)
{
    if (s == NULL) {
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }
    size_t n = 0;
    while (n <= SEMVER_MAX_LEN && s[n] != '\0') {
        n++;
    }
    return update_semver_parse_n(s, n, out);
}

static int cmp_u32(uint32_t a, uint32_t b) { return a < b ? -1 : (a > b ? 1 : 0); }

static bool all_digits_n(const char *p, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        if (!is_digit(p[k])) {
            return false;
        }
    }
    return true;
}

// Compare two prerelease strings (both valid, non-empty).
static int cmp_pre(const char *a, const char *b)
{
    for (;;) {
        const char *ae = strchr(a, '.');
        const char *be = strchr(b, '.');
        size_t al = ae ? (size_t)(ae - a) : strlen(a);
        size_t bl = be ? (size_t)(be - b) : strlen(b);
        bool an = all_digits_n(a, al);
        bool bn = all_digits_n(b, bl);
        int c;
        if (an && bn) {
            // No leading zeros, so a longer numeric identifier is larger.
            c = al < bl ? -1 : (al > bl ? 1 : 0);
            if (c == 0) {
                c = memcmp(a, b, al);
                c = c < 0 ? -1 : (c > 0 ? 1 : 0);
            }
        } else if (an != bn) {
            c = an ? -1 : 1; // numeric < alphanumeric
        } else {
            size_t m = al < bl ? al : bl;
            c = memcmp(a, b, m);
            if (c == 0) {
                c = al < bl ? -1 : (al > bl ? 1 : 0);
            } else {
                c = c < 0 ? -1 : 1;
            }
        }
        if (c != 0) {
            return c;
        }
        if (ae == NULL || be == NULL) {
            if (ae == NULL && be == NULL) {
                return 0;
            }
            return ae == NULL ? -1 : 1; // fewer identifiers sorts lower
        }
        a = ae + 1;
        b = be + 1;
    }
}

int update_semver_compare(const update_semver_t *a, const update_semver_t *b)
{
    int c = cmp_u32(a->major, b->major);
    if (c != 0) {
        return c;
    }
    c = cmp_u32(a->minor, b->minor);
    if (c != 0) {
        return c;
    }
    c = cmp_u32(a->patch, b->patch);
    if (c != 0) {
        return c;
    }
    if (a->has_pre != b->has_pre) {
        return a->has_pre ? -1 : 1; // prerelease < release
    }
    if (!a->has_pre) {
        return 0;
    }
    return cmp_pre(a->pre, b->pre);
}

size_t update_semver_format(const update_semver_t *v, char *buf, size_t buf_len)
{
    if (v == NULL || buf == NULL || buf_len == 0) {
        return 0;
    }
    int n;
    if (v->has_pre) {
        n = snprintf(buf, buf_len, "%u.%u.%u-%s", (unsigned)v->major, (unsigned)v->minor, (unsigned)v->patch,
                     v->pre);
    } else {
        n = snprintf(buf, buf_len, "%u.%u.%u", (unsigned)v->major, (unsigned)v->minor, (unsigned)v->patch);
    }
    if (n < 0 || (size_t)n >= buf_len) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

// BM-02: a dev build has no release tag, but its esp_app_desc version is `git describe` output
// ("1.0.0-pre.1-201-g2071348", optionally "-dirty"). Map it to a comparable semver:
//  - no "-N-g<hex>" suffix: the text itself (a build exactly on a tag), if it parses;
//  - base has a prerelease: append ".N" ("1.0.0-pre.1.201"), which sorts after the tag and before
//    the next prerelease ("pre.2") exactly as N commits past the tag should;
//  - base is a release: the base (a numeric build counter cannot be expressed above a release), so a
//    candidate equal to it still needs force and a lower one is still a downgrade.
// Returns false (out = "") when the base is not a valid semver, so such a build stays unknown.
// RB-02: esp_app_desc_t.version is a fixed 32-byte field in the IDF image ABI (UPDATE_STAGE_APP_DESC_VERSION_OFFSET
// and the bootloader read it), so it cannot be widened; a describe longer than 31 chars arrives truncated and
// parses as unknown (fail-safe: an update then needs force, never a silent downgrade).
bool update_semver_from_describe(const char *desc, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (desc == NULL) {
        return false;
    }
    char buf[SEMVER_MAX_LEN + 1];
    size_t n = 0;
    while (n <= SEMVER_MAX_LEN && desc[n] != '\0') {
        n++;
    }
    if (n == 0 || n > SEMVER_MAX_LEN) {
        return false;
    }
    memcpy(buf, desc, n);
    buf[n] = '\0';
    static const char dirty[] = "-dirty";
    if (n > sizeof(dirty) - 1 && strcmp(buf + n - (sizeof(dirty) - 1), dirty) == 0) {
        n -= sizeof(dirty) - 1;
        buf[n] = '\0';
    }
    // Trailing "-<digits>-g<hex>".
    size_t commits = 0;
    bool has_suffix = false;
    size_t g = n;
    while (g > 0 && ((buf[g - 1] >= '0' && buf[g - 1] <= '9') || (buf[g - 1] >= 'a' && buf[g - 1] <= 'f'))) {
        g--;
    }
    if (g >= 4 && n - g >= 4 && buf[g - 1] == 'g' && buf[g - 2] == '-') {
        size_t d_end = g - 2;
        size_t d = d_end;
        while (d > 0 && buf[d - 1] >= '0' && buf[d - 1] <= '9') {
            d--;
        }
        if (d < d_end && d > 0 && buf[d - 1] == '-' && d_end - d <= 6) {
            for (size_t k = d; k < d_end; k++) {
                commits = commits * 10u + (size_t)(buf[k] - '0');
            }
            has_suffix = true;
            n = d - 1;
            buf[n] = '\0';
        }
    }
    update_semver_t v;
    if (!update_semver_parse_n(buf, n, &v)) {
        return false;
    }
    if (has_suffix && commits > 0 && v.has_pre) {
        char tail[16];
        int tn = snprintf(tail, sizeof(tail), ".%u", (unsigned)commits);
        size_t pl = strlen(v.pre);
        if (tn < 0 || pl + (size_t)tn > UPDATE_SEMVER_PRE_MAX) {
            return false;
        }
        memcpy(v.pre + pl, tail, (size_t)tn + 1);
    }
    return update_semver_format(&v, out, cap) != 0;
}
