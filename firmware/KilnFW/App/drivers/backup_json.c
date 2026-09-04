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

bool backup_json_field_num(const char *obj, const char *key, double *out)
{
    const char *v = backup_json_obj_find(obj, key);
    if (!v) {
        return false;
    }
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v || !isfinite(d)) {
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
    while (*v && *v != '"' && o + 1 < cap) {
        if (*v == '\\' && v[1]) {
            v++;
            char c = *v;
            out[o++] = (c == 'n') ? '\n' : (c == 't') ? '\t' : c;
            v++;
        } else {
            out[o++] = *v++;
        }
    }
    out[o] = '\0';
    return true;
}
