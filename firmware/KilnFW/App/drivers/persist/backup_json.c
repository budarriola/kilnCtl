#include "backup_json.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

const char *backup_json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

const char *backup_json_skip_value(const char *p)
{
    p = backup_json_skip_ws(p);
    if (*p == '\0') {
        return p;
    }
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p++;
            }
            p++;
        }
        if (*p == '"') {
            p++;
        }
        return p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) {
                        p++;
                    }
                    p++;
                }
                if (*p == '"') {
                    p++;
                }
                continue;
            }
            if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
            }
            p++;
        }
        return p;
    }
    /* number / true / false / null -- scan to the next structural delimiter */
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }
    return p;
}

const char *backup_json_obj_find(const char *obj, const char *key)
{
    const char *p = backup_json_skip_ws(obj);
    if (*p != '{') {
        return NULL;
    }
    p++;
    size_t key_len = strlen(key);
    for (;;) {
        p = backup_json_skip_ws(p);
        if (*p == '}' || *p == '\0') {
            return NULL;
        }
        if (*p != '"') {
            return NULL; /* malformed -- not a "key": pair here */
        }
        const char *kstart = p + 1;
        const char *kend = kstart;
        while (*kend && *kend != '"') {
            if (*kend == '\\' && kend[1]) {
                kend++;
            }
            kend++;
        }
        size_t klen = (size_t)(kend - kstart);
        p = (*kend == '"') ? kend + 1 : kend;
        p = backup_json_skip_ws(p);
        if (*p != ':') {
            return NULL;
        }
        p++;
        p = backup_json_skip_ws(p);
        const char *vstart = p;
        bool match = (klen == key_len && strncmp(kstart, key, klen) == 0);
        p = backup_json_skip_value(p);
        if (match) {
            return vstart;
        }
        p = backup_json_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            return NULL;
        }
        return NULL; /* malformed */
    }
}

const char *backup_json_arr_first(const char *arr)
{
    if (!arr) {
        return NULL;
    }
    const char *p = backup_json_skip_ws(arr);
    if (*p != '[') {
        return NULL;
    }
    p++;
    p = backup_json_skip_ws(p);
    if (*p == ']') {
        return NULL;
    }
    return p;
}

const char *backup_json_arr_next(const char *elem)
{
    const char *p = backup_json_skip_value(elem);
    p = backup_json_skip_ws(p);
    if (*p == ',') {
        p++;
        return backup_json_skip_ws(p);
    }
    return NULL;
}

static const char *bj_check_number(const char *p);

bool backup_json_field_num(const char *obj, const char *key, double *out)
{
    const char *v = backup_json_obj_find(obj, key);
    if (!v) {
        return false;
    }
    /* K10-04/K10-05: strict JSON number grammar (no hex floats, "inf", leading '+', ...) followed by a
     * value delimiter -- "12abc" is not 12. strtod only converts text this check already accepted. */
    const char *nend = bj_check_number(v);
    if (!nend) {
        return false;
    }
    if (*nend != '\0' && *nend != ',' && *nend != '}' && *nend != ']' && *nend != ' ' && *nend != '\t' &&
        *nend != '\n' && *nend != '\r') {
        return false;
    }
    char *end = NULL;
    double d = strtod(v, &end);
    if (end != nend || !isfinite(d)) {
        return false;
    }
    *out = d;
    return true;
}

bool backup_json_field_opt_num(const char *obj, const char *key, double min, double max, double *out,
                               bool *out_has, const char *field_desc, char *err_msg, size_t err_cap,
                               unsigned entry_idx)
{
    if (!backup_json_obj_find(obj, key)) {
        *out_has = false;
        return true;
    }
    if (!backup_json_field_num(obj, key, out) || *out < min || *out > max) {
        snprintf(err_msg, err_cap, "zone tuning entry %u: %s missing or out of range", entry_idx, field_desc);
        return false;
    }
    *out_has = true;
    return true;
}

bool backup_json_field_str(const char *obj, const char *key, char *out, size_t cap)
{
    const char *v = backup_json_obj_find(obj, key);
    if (!v || *v != '"' || cap == 0) {
        return false;
    }
    v++;
    size_t o = 0;
    out[0] = '\0';
    /* K10-01/02/03: an over-long value, an unterminated string and an undecodable escape are ERRORS --
     * returning a truncated or mangled string as success restores a damaged value silently. */
    while (*v != '"') {
        if (*v == '\0') {
            out[0] = '\0';
            return false; /* unterminated (truncated backup) */
        }
        char buf[4];
        size_t n = 1;
        if (*v == '\\') {
            v++;
            switch (*v) {
            case '"': buf[0] = '"'; break;
            case '\\': buf[0] = '\\'; break;
            case '/': buf[0] = '/'; break;
            case 'b': buf[0] = '\b'; break;
            case 'f': buf[0] = '\f'; break;
            case 'n': buf[0] = '\n'; break;
            case 'r': buf[0] = '\r'; break;
            case 't': buf[0] = '\t'; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 1; i <= 4; i++) {
                    char h = v[i];
                    unsigned dgt;
                    if (h >= '0' && h <= '9') {
                        dgt = (unsigned)(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        dgt = (unsigned)(h - 'a') + 10u;
                    } else if (h >= 'A' && h <= 'F') {
                        dgt = (unsigned)(h - 'A') + 10u;
                    } else {
                        out[0] = '\0';
                        return false; /* bad hex (also catches a NUL) */
                    }
                    cp = (cp << 4) | dgt;
                }
                if (cp >= 0xD800u && cp <= 0xDFFFu) {
                    out[0] = '\0';
                    return false; /* surrogate half: not decoded here */
                }
                if (cp == 0u) {
                    out[0] = '\0';
                    return false; /* an embedded NUL cannot live in a C string */
                }
                if (cp < 0x80u) {
                    buf[0] = (char)cp;
                } else if (cp < 0x800u) {
                    buf[0] = (char)(0xC0u | (cp >> 6));
                    buf[1] = (char)(0x80u | (cp & 0x3Fu));
                    n = 2;
                } else {
                    buf[0] = (char)(0xE0u | (cp >> 12));
                    buf[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    buf[2] = (char)(0x80u | (cp & 0x3Fu));
                    n = 3;
                }
                v += 4; /* now on the last hex digit */
                break;
            }
            default:
                out[0] = '\0';
                return false; /* unknown escape (or a lone backslash at the end) */
            }
        } else {
            buf[0] = *v;
        }
        if (o + n + 1 > cap) {
            out[0] = '\0';
            return false; /* does not fit */
        }
        memcpy(out + o, buf, n);
        o += n;
        v++;
    }
    out[o] = '\0';
    return true;
}

#define BACKUP_JSON_MAX_DEPTH 32

static const char *bj_fail(char *err, size_t cap, const char *msg)
{
    if (err && cap) {
        snprintf(err, cap, "%s", msg);
    }
    return NULL;
}

/* Validates one JSON string starting at the opening quote; returns the byte after the closing quote or NULL. */
static const char *bj_check_string(const char *p)
{
    p++;
    while (*p && *p != '"') {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20) {
            return NULL;
        }
        if (ch == '\\') {
            p++;
            /* Lenient on the escape letter: the field scanners define its meaning and some callers refuse
             * backslashes outright with their own message. Only a NUL or control byte here is malformed. */
            if ((unsigned char)*p < 0x20) {
                return NULL;
            }
        }
        p++;
    }
    return (*p == '"') ? p + 1 : NULL;
}

static const char *bj_check_number(const char *p)
{
    if (*p == '-') {
        p++;
    }
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    } else {
        return NULL;
    }
    if (*p == '.') {
        p++;
        if (!(*p >= '0' && *p <= '9')) {
            return NULL;
        }
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') {
            p++;
        }
        if (!(*p >= '0' && *p <= '9')) {
            return NULL;
        }
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    }
    return p;
}


/* F6: refuse a top-level key that appears twice (the field scanners would silently take the first). The
 * document is already known to be well-formed here. */
static bool bj_no_dup_top_level_key(const char *doc, char *err, size_t err_cap)
{
    const char *p = backup_json_skip_ws(doc) + 1;
    for (;;) {
        p = backup_json_skip_ws(p);
        if (*p != '"') {
            return true; /* '}' -- end of object */
        }
        const char *ks = p + 1;
        const char *ke = ks;
        while (*ke != '"') {
            ke += (*ke == '\\') ? 2 : 1;
        }
        size_t kl = (size_t)(ke - ks);
        /* compare against every later key */
        const char *q = backup_json_skip_ws(ke + 1);
        q = backup_json_skip_value(q + 1); /* past ':' and the value */
        for (;;) {
            q = backup_json_skip_ws(q);
            if (*q != ',') {
                break;
            }
            q = backup_json_skip_ws(q + 1);
            const char *ks2 = q + 1;
            const char *ke2 = ks2;
            while (*ke2 != '"') {
                ke2 += (*ke2 == '\\') ? 2 : 1;
            }
            if ((size_t)(ke2 - ks2) == kl && strncmp(ks, ks2, kl) == 0) {
                bj_fail(err, err_cap, "duplicate top-level key in backup");
                return false;
            }
            q = backup_json_skip_ws(ke2 + 1);
            q = backup_json_skip_value(q + 1);
        }
        p = backup_json_skip_value(backup_json_skip_ws(ke + 1) + 1);
        p = backup_json_skip_ws(p);
        if (*p != ',') {
            return true;
        }
        p++;
    }
}

bool backup_json_key_present_not_array(const char *obj, const char *key)
{
    const char *v = backup_json_obj_find(obj, key);
    return v && *backup_json_skip_ws(v) != '[';
}

bool backup_json_validate_document(const char *doc, char *err, size_t err_cap)
{
    if (!doc) {
        bj_fail(err, err_cap, "empty body");
        return false;
    }
    /* stack[i]: '{' or '['. expect: 0 = value, 1 = after value (',' or close), 2 = key (or '}' if first),
     * 3 = value required (after ',' or ':'), 4 = value-or-']' (just after '['). */
    char stack[BACKUP_JSON_MAX_DEPTH];
    int depth = 0;
    int expect = 3;
    const char *p = backup_json_skip_ws(doc);
    if (*p != '{') {
        bj_fail(err, err_cap, "backup is not a JSON object");
        return false;
    }
    for (;;) {
        p = backup_json_skip_ws(p);
        if (expect == 1) {
            if (depth == 0) {
                if (*p != '\0') {
                    bj_fail(err, err_cap, "trailing data after the JSON document");
                    return false;
                }
                return bj_no_dup_top_level_key(doc, err, err_cap);
            }
            if (*p == ',') {
                p++;
                expect = (stack[depth - 1] == '{') ? 5 : 3;
            } else if ((*p == '}' && stack[depth - 1] == '{') || (*p == ']' && stack[depth - 1] == '[')) {
                p++;
                depth--;
                expect = 1;
            } else {
                bj_fail(err, err_cap, "malformed or truncated JSON document");
                return false;
            }
            continue;
        }
        if (expect == 2 || expect == 5) { /* object: key (or '}' when first) */
            if (expect == 2 && *p == '}') {
                p++;
                depth--;
                expect = 1;
                continue;
            }
            if (*p != '"') {
                bj_fail(err, err_cap, "malformed or truncated JSON document");
                return false;
            }
            p = bj_check_string(p);
            if (!p) {
                bj_fail(err, err_cap, "malformed or truncated JSON string");
                return false;
            }
            p = backup_json_skip_ws(p);
            if (*p != ':') {
                bj_fail(err, err_cap, "malformed or truncated JSON document");
                return false;
            }
            p++;
            expect = 3;
            continue;
        }
        /* expect a value (3), or a value-or-']' (4) */
        if (expect == 4 && *p == ']') {
            p++;
            depth--;
            expect = 1;
            continue;
        }
        if (*p == '{' || *p == '[') {
            if (depth >= BACKUP_JSON_MAX_DEPTH) {
                bj_fail(err, err_cap, "JSON nested too deeply");
                return false;
            }
            stack[depth++] = *p;
            expect = (*p == '{') ? 2 : 4;
            p++;
        } else if (*p == '"') {
            p = bj_check_string(p);
            if (!p) {
                bj_fail(err, err_cap, "malformed or truncated JSON string");
                return false;
            }
            expect = 1;
        } else if (strncmp(p, "true", 4) == 0) {
            p += 4;
            expect = 1;
        } else if (strncmp(p, "false", 5) == 0) {
            p += 5;
            expect = 1;
        } else if (strncmp(p, "null", 4) == 0) {
            p += 4;
            expect = 1;
        } else {
            p = bj_check_number(p);
            if (!p) {
                bj_fail(err, err_cap, "malformed or truncated JSON document");
                return false;
            }
            expect = 1;
        }
    }
}
