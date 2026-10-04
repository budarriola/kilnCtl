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
