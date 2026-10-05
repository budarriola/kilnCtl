// update_release.c -- see update_release.h.
#include "update_release.h"

#include <stdio.h>
#include <string.h>

#include "update_semver.h"

typedef struct {
    const char *p;
    const char *end;
} cur_t;

typedef update_rel_err_t (*visit_kv_fn)(const char *key, cur_t *c, int depth, void *ctx);
typedef update_rel_err_t (*visit_elem_fn)(cur_t *c, int depth, void *ctx);

#define KEY_MAX 40
#define NAME_MAX_LEN 64

static void skip_ws(cur_t *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')) {
        c->p++;
    }
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

// Reads one JSON string at c->p (which must be a quote). buf may be NULL to
// just skip. *overflow is set when the decoded text did not fit; buf is then
// truncated and NUL-terminated. Returns false on malformed input.
static bool read_string(cur_t *c, char *buf, size_t cap, bool *overflow)
{
    size_t n = 0;
    bool ov = false;
    if (c->p >= c->end || *c->p != '"') {
        return false;
    }
    c->p++;
    for (;;) {
        if (c->p >= c->end) {
            return false;
        }
        unsigned char ch = (unsigned char)*c->p++;
        char out;
        if (ch == '"') {
            break;
        }
        if (ch < 0x20) {
            return false;
        }
        if (ch == '\\') {
            if (c->p >= c->end) {
                return false;
            }
            char e = *c->p++;
            switch (e) {
            case '"': out = '"'; break;
            case '\\': out = '\\'; break;
            case '/': out = '/'; break;
            case 'b': out = '\b'; break;
            case 'f': out = '\f'; break;
            case 'n': out = '\n'; break;
            case 'r': out = '\r'; break;
            case 't': out = '\t'; break;
            case 'u': {
                if (c->end - c->p < 4) {
                    return false;
                }
                int cp = 0;
                for (int i = 0; i < 4; i++) {
                    int h = hexval(c->p[i]);
                    if (h < 0) {
                        return false;
                    }
                    cp = cp * 16 + h;
                }
                c->p += 4;
                out = (cp > 0 && cp < 0x80) ? (char)cp : '?'; // non-ASCII never matters here; NUL never kept
                break;
            }
            default:
                return false;
            }
        } else {
            out = (char)ch;
        }
        if (buf != NULL) {
            if (n + 1 < cap) {
                buf[n++] = out;
            } else {
                ov = true;
            }
        }
    }
    if (buf != NULL && cap > 0) {
        buf[n] = '\0';
    }
    if (overflow != NULL) {
        *overflow = ov;
    }
    return true;
}

// Consumes a number token. *ok is true (and *val set) only for a plain
// non-negative integer that fits uint32.
static bool read_number(cur_t *c, uint32_t *val, bool *ok)
{
    const char *s = c->p;
    uint64_t v = 0;
    bool plain = true;
    size_t digits = 0;
    while (c->p < c->end) {
        char ch = *c->p;
        if (ch >= '0' && ch <= '9') {
            v = v * 10 + (uint64_t)(ch - '0');
            if (++digits > 10) {
                plain = false;
                v = 0;
            }
        } else if (ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E') {
            plain = false;
        } else {
            break;
        }
        c->p++;
    }
    if (c->p == s) {
        return false;
    }
    if (digits == 0 || (digits > 1 && s[0] == '0')) {
        plain = false;
    }
    if (v > 0xFFFFFFFFu) {
        plain = false;
    }
    if (val != NULL) {
        *val = plain ? (uint32_t)v : 0;
    }
    if (ok != NULL) {
        *ok = plain;
    }
    return true;
}

static bool read_literal(cur_t *c, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) {
        return false;
    }
    c->p += n;
    return true;
}

static update_rel_err_t visit_object(cur_t *c, int depth, visit_kv_fn fn, void *ctx);
static update_rel_err_t visit_array(cur_t *c, int depth, visit_elem_fn fn, void *ctx);

static update_rel_err_t skip_value(cur_t *c, int depth);

static update_rel_err_t skip_kv(const char *key, cur_t *c, int depth, void *ctx)
{
    (void)key;
    (void)ctx;
    return skip_value(c, depth + 1);
}

static update_rel_err_t skip_elem(cur_t *c, int depth, void *ctx)
{
    (void)ctx;
    return skip_value(c, depth + 1);
}

static update_rel_err_t skip_value(cur_t *c, int depth)
{
    if (depth > UPDATE_JSON_MAX_DEPTH) {
        return UPDATE_REL_E_JSON;
    }
    skip_ws(c);
    if (c->p >= c->end) {
        return UPDATE_REL_E_JSON;
    }
    switch (*c->p) {
    case '"':
        return read_string(c, NULL, 0, NULL) ? UPDATE_REL_OK : UPDATE_REL_E_JSON;
    case '{':
        return visit_object(c, depth, skip_kv, NULL);
    case '[':
        return visit_array(c, depth, skip_elem, NULL);
    case 't':
        return read_literal(c, "true") ? UPDATE_REL_OK : UPDATE_REL_E_JSON;
    case 'f':
        return read_literal(c, "false") ? UPDATE_REL_OK : UPDATE_REL_E_JSON;
    case 'n':
        return read_literal(c, "null") ? UPDATE_REL_OK : UPDATE_REL_E_JSON;
    default:
        return read_number(c, NULL, NULL) ? UPDATE_REL_OK : UPDATE_REL_E_JSON;
    }
}

static update_rel_err_t visit_object(cur_t *c, int depth, visit_kv_fn fn, void *ctx)
{
    if (depth > UPDATE_JSON_MAX_DEPTH) {
        return UPDATE_REL_E_JSON;
    }
    skip_ws(c);
    if (c->p >= c->end || *c->p != '{') {
        return UPDATE_REL_E_JSON;
    }
    c->p++;
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') {
        c->p++;
        return UPDATE_REL_OK;
    }
    for (;;) {
        char key[KEY_MAX];
        bool ov = false;
        skip_ws(c);
        if (!read_string(c, key, sizeof(key), &ov)) {
            return UPDATE_REL_E_JSON;
        }
        if (ov) {
            key[0] = '\0';
        }
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') {
            return UPDATE_REL_E_JSON;
        }
        c->p++;
        skip_ws(c);
        update_rel_err_t e = fn(key, c, depth, ctx);
        if (e != UPDATE_REL_OK) {
            return e;
        }
        skip_ws(c);
        if (c->p >= c->end) {
            return UPDATE_REL_E_JSON;
        }
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == '}') {
            c->p++;
            return UPDATE_REL_OK;
        }
        return UPDATE_REL_E_JSON;
    }
}

static update_rel_err_t visit_array(cur_t *c, int depth, visit_elem_fn fn, void *ctx)
{
    if (depth > UPDATE_JSON_MAX_DEPTH) {
        return UPDATE_REL_E_JSON;
    }
    skip_ws(c);
    if (c->p >= c->end || *c->p != '[') {
        return UPDATE_REL_E_JSON;
    }
    c->p++;
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') {
        c->p++;
        return UPDATE_REL_OK;
    }
    for (;;) {
        skip_ws(c);
        update_rel_err_t e = fn(c, depth, ctx);
        if (e != UPDATE_REL_OK) {
            return e;
        }
        skip_ws(c);
        if (c->p >= c->end) {
            return UPDATE_REL_E_JSON;
        }
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == ']') {
            c->p++;
            return UPDATE_REL_OK;
        }
        return UPDATE_REL_E_JSON;
    }
}

// Runs `fn` over the top-level object of the document and demands nothing but
// whitespace after it.
static update_rel_err_t walk_document(const char *json, size_t len, visit_kv_fn fn, void *ctx)
{
    cur_t c = {json, json + len};
    update_rel_err_t e = visit_object(&c, 1, fn, ctx);
    if (e != UPDATE_REL_OK) {
        return e;
    }
    skip_ws(&c);
    return (c.p == c.end) ? UPDATE_REL_OK : UPDATE_REL_E_JSON;
}

// Reads a string value into buf; false if it is not a string or does not fit.
static bool take_string(cur_t *c, char *buf, size_t cap)
{
    bool ov = false;
    if (!read_string(c, buf, cap, &ov)) {
        return false;
    }
    return !ov;
}

// ---------------------------------------------------------------------------
// GitHub API reply
// ---------------------------------------------------------------------------

typedef struct {
    int pass; // 1: tag_name/draft/prerelease; 2: assets
    char tag[UPDATE_TAG_MAX];
    bool seen_tag, seen_draft, seen_prerelease, seen_assets;
    bool draft, prerelease;
    bool tag_bad;
    const char *repo;
    uint32_t max_app_size;
    char app_name[UPDATE_TAG_MAX + 16];
    update_release_info_t *out;
    bool have_app, have_manifest;
} api_ctx_t;

typedef struct {
    char name[NAME_MAX_LEN];
    char url[UPDATE_URL_MAX];
    uint32_t size;
    bool size_ok, seen_name, seen_url, seen_size, url_ok, name_ok;
} asset_ctx_t;

static update_rel_err_t asset_kv(const char *key, cur_t *c, int depth, void *vctx)
{
    asset_ctx_t *a = (asset_ctx_t *)vctx;
    if (strcmp(key, "name") == 0) {
        if (a->seen_name) {
            return UPDATE_REL_E_JSON;
        }
        a->seen_name = true;
        skip_ws(c);
        if (c->p < c->end && *c->p == '"') {
            a->name_ok = take_string(c, a->name, sizeof(a->name));
            if (!a->name_ok) {
                a->name[0] = '\0';
            }
            return UPDATE_REL_OK;
        }
        return skip_value(c, depth + 1);
    }
    if (strcmp(key, "browser_download_url") == 0) {
        if (a->seen_url) {
            return UPDATE_REL_E_JSON;
        }
        a->seen_url = true;
        skip_ws(c);
        if (c->p < c->end && *c->p == '"') {
            a->url_ok = take_string(c, a->url, sizeof(a->url));
            if (!a->url_ok) {
                a->url[0] = '\0';
            }
            return UPDATE_REL_OK;
        }
        return skip_value(c, depth + 1);
    }
    if (strcmp(key, "size") == 0) {
        if (a->seen_size) {
            return UPDATE_REL_E_JSON;
        }
        a->seen_size = true;
        skip_ws(c);
        if (c->p < c->end && ((*c->p >= '0' && *c->p <= '9') || *c->p == '-')) {
            return read_number(c, &a->size, &a->size_ok) ? UPDATE_REL_OK : UPDATE_REL_E_JSON;
        }
        return skip_value(c, depth + 1);
    }
    return skip_value(c, depth + 1);
}

static update_rel_err_t asset_elem(cur_t *c, int depth, void *vctx)
{
    api_ctx_t *x = (api_ctx_t *)vctx;
    asset_ctx_t a;
    memset(&a, 0, sizeof(a));
    update_rel_err_t e = visit_object(c, depth + 1, asset_kv, &a);
    if (e != UPDATE_REL_OK) {
        return e;
    }
    if (!a.name_ok) {
        return UPDATE_REL_OK; // unnamed / over-long name: not one of ours
    }
    bool is_app = strcmp(a.name, x->app_name) == 0;
    bool is_manifest = strcmp(a.name, "release.json") == 0;
    if (!is_app && !is_manifest) {
        return UPDATE_REL_OK;
    }
    if ((is_app && x->have_app) || (is_manifest && x->have_manifest)) {
        return UPDATE_REL_E_DUP_ASSET;
    }
    if (!a.url_ok || !update_asset_url_matches(a.url, x->repo, x->tag, a.name)) {
        return UPDATE_REL_E_BAD_URL;
    }
    if (!a.size_ok || a.size == 0) {
        return UPDATE_REL_E_BAD_SIZE;
    }
    if (is_app) {
        if (a.size > x->max_app_size) {
            return UPDATE_REL_E_BAD_SIZE;
        }
        // is_app means a.name equals x->app_name, so copy that (it is sized for the out field).
        snprintf(x->out->app_name, sizeof(x->out->app_name), "%s", x->app_name);
        snprintf(x->out->app_url, sizeof(x->out->app_url), "%s", a.url);
        x->out->app_size = a.size;
        x->have_app = true;
    } else {
        // release.json is small; refuse an absurd one outright (the fetch
        // buffer is 16 KiB).
        if (a.size > 16384u) {
            return UPDATE_REL_E_BAD_SIZE;
        }
        snprintf(x->out->manifest_url, sizeof(x->out->manifest_url), "%s", a.url);
        x->out->manifest_size = a.size;
        x->have_manifest = true;
    }
    return UPDATE_REL_OK;
}

static update_rel_err_t api_kv(const char *key, cur_t *c, int depth, void *vctx)
{
    api_ctx_t *x = (api_ctx_t *)vctx;
    if (x->pass == 1) {
        if (strcmp(key, "tag_name") == 0) {
            if (x->seen_tag) {
                return UPDATE_REL_E_JSON;
            }
            x->seen_tag = true;
            skip_ws(c);
            if (c->p < c->end && *c->p == '"') {
                if (!take_string(c, x->tag, sizeof(x->tag))) {
                    x->tag[0] = '\0';
                    x->tag_bad = true;
                }
                return UPDATE_REL_OK;
            }
            x->tag_bad = true;
            return skip_value(c, depth + 1);
        }
        if (strcmp(key, "draft") == 0 || strcmp(key, "prerelease") == 0) {
            bool is_draft = key[0] == 'd';
            bool *seen = is_draft ? &x->seen_draft : &x->seen_prerelease;
            bool *val = is_draft ? &x->draft : &x->prerelease;
            if (*seen) {
                return UPDATE_REL_E_JSON;
            }
            *seen = true;
            skip_ws(c);
            if (read_literal(c, "true")) {
                *val = true;
                return UPDATE_REL_OK;
            }
            if (read_literal(c, "false")) {
                *val = false;
                return UPDATE_REL_OK;
            }
            return UPDATE_REL_E_JSON;
        }
        return skip_value(c, depth + 1);
    }
    if (strcmp(key, "assets") == 0) {
        if (x->seen_assets) {
            return UPDATE_REL_E_JSON;
        }
        x->seen_assets = true;
        return visit_array(c, depth + 1, asset_elem, x);
    }
    return skip_value(c, depth + 1);
}

update_rel_err_t update_release_parse_api(const char *json, size_t len, const char *repo, uint32_t max_app_size,
                                          update_release_info_t *out)
{
    if (out == NULL) {
        return UPDATE_REL_E_ARGS;
    }
    memset(out, 0, sizeof(*out));
    if (json == NULL || len == 0 || repo == NULL || !update_repo_valid(repo) || max_app_size == 0) {
        return UPDATE_REL_E_ARGS;
    }
    api_ctx_t x;
    memset(&x, 0, sizeof(x));
    x.repo = repo;
    x.max_app_size = max_app_size;
    x.out = out;

    x.pass = 1;
    update_rel_err_t e = walk_document(json, len, api_kv, &x);
    if (e != UPDATE_REL_OK) {
        return e;
    }
    if (!x.seen_tag) {
        return UPDATE_REL_E_NO_TAG;
    }
    if (x.tag_bad || !update_tag_valid(x.tag)) {
        return UPDATE_REL_E_BAD_TAG;
    }
    if (x.draft) {
        return UPDATE_REL_E_DRAFT;
    }
    snprintf(x.app_name, sizeof(x.app_name), "KilnCtrl-%s.bin", x.tag);

    x.pass = 2;
    e = walk_document(json, len, api_kv, &x);
    if (e != UPDATE_REL_OK) {
        memset(out, 0, sizeof(*out));
        return e;
    }
    if (!x.have_app) {
        memset(out, 0, sizeof(*out));
        return UPDATE_REL_E_NO_APP;
    }
    if (!x.have_manifest) {
        memset(out, 0, sizeof(*out));
        return UPDATE_REL_E_NO_MANIFEST;
    }
    snprintf(out->tag, sizeof(out->tag), "%s", x.tag);
    out->prerelease = x.prerelease;
    return UPDATE_REL_OK;
}

// ---------------------------------------------------------------------------
// release.json
// ---------------------------------------------------------------------------

typedef struct {
    bool seen_zones, seen_kl, seen_uart, seen_sha, seen_min;
    uint32_t zones, kl, uart;
    char sha[UPDATE_PARTITIONS_SHA_HEX_LEN + 1];
    char min_from[UPDATE_VERSION_STR_MAX];
    bool bad;
} compat_ctx_t;

static update_rel_err_t compat_kv(const char *key, cur_t *c, int depth, void *vctx)
{
    compat_ctx_t *k = (compat_ctx_t *)vctx;
    uint32_t *num = NULL;
    bool *seen = NULL;
    if (strcmp(key, "zones_cfg_version") == 0) {
        num = &k->zones;
        seen = &k->seen_zones;
    } else if (strcmp(key, "kilnlink_version") == 0) {
        num = &k->kl;
        seen = &k->seen_kl;
    } else if (strcmp(key, "uart_version") == 0) {
        num = &k->uart;
        seen = &k->seen_uart;
    }
    if (num != NULL) {
        if (*seen) {
            return UPDATE_REL_E_JSON;
        }
        *seen = true;
        skip_ws(c);
        bool ok = false;
        if (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            if (!read_number(c, num, &ok)) {
                return UPDATE_REL_E_JSON;
            }
        } else {
            return skip_value(c, depth + 1) == UPDATE_REL_OK ? (k->bad = true, UPDATE_REL_OK) : UPDATE_REL_E_JSON;
        }
        if (!ok || *num == 0) {
            k->bad = true;
        }
        return UPDATE_REL_OK;
    }
    if (strcmp(key, "partitions_sha256") == 0 || strcmp(key, "min_updatable_from") == 0) {
        bool is_sha = key[0] == 'p';
        bool *s = is_sha ? &k->seen_sha : &k->seen_min;
        if (*s) {
            return UPDATE_REL_E_JSON;
        }
        *s = true;
        skip_ws(c);
        if (c->p < c->end && *c->p == '"') {
            char *dst = is_sha ? k->sha : k->min_from;
            size_t cap = is_sha ? sizeof(k->sha) : sizeof(k->min_from);
            if (!take_string(c, dst, cap)) {
                dst[0] = '\0';
                k->bad = true;
            }
            return UPDATE_REL_OK;
        }
        k->bad = true;
        return skip_value(c, depth + 1);
    }
    return skip_value(c, depth + 1);
}

typedef struct {
    char name[16];
    char file[UPDATE_TAG_MAX + 16];
    char sha[UPDATE_SHA256_HEX_LEN + 1];
    uint32_t size;
    bool size_ok, name_ok, file_ok, sha_ok;
    bool seen_name, seen_file, seen_sha, seen_size; // a repeated key is a malformed manifest
} image_ctx_t;

static update_rel_err_t image_kv(const char *key, cur_t *c, int depth, void *vctx)
{
    image_ctx_t *im = (image_ctx_t *)vctx;
    skip_ws(c);
    bool is_str = c->p < c->end && *c->p == '"';
    if (strcmp(key, "name") == 0) {
        if (im->seen_name) {
            return UPDATE_REL_E_JSON;
        }
        im->seen_name = true;
        if (is_str) {
            im->name_ok = take_string(c, im->name, sizeof(im->name));
            return UPDATE_REL_OK;
        }
    } else if (strcmp(key, "file") == 0) {
        if (im->seen_file) {
            return UPDATE_REL_E_JSON;
        }
        im->seen_file = true;
        if (is_str) {
            im->file_ok = take_string(c, im->file, sizeof(im->file));
            return UPDATE_REL_OK;
        }
    } else if (strcmp(key, "sha256") == 0) {
        if (im->seen_sha) {
            return UPDATE_REL_E_JSON;
        }
        im->seen_sha = true;
        if (is_str) {
            im->sha_ok = take_string(c, im->sha, sizeof(im->sha));
            return UPDATE_REL_OK;
        }
    } else if (strcmp(key, "size") == 0) {
        if (im->seen_size) {
            return UPDATE_REL_E_JSON;
        }
        im->seen_size = true;
        if (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            return read_number(c, &im->size, &im->size_ok) ? UPDATE_REL_OK : UPDATE_REL_E_JSON;
        }
    }
    return skip_value(c, depth + 1);
}

typedef struct {
    bool seen_schema, seen_tag, seen_repo, seen_commit, seen_dirty, seen_compat, seen_images;
    uint32_t schema;
    bool schema_ok;
    char tag[UPDATE_TAG_MAX];
    bool tag_ok;
    char repo[UPDATE_REPO_MAX];
    bool repo_ok;
    char commit[UPDATE_COMMIT_HEX_LEN + 1];
    bool commit_ok;
    bool dirty_false;
    compat_ctx_t compat;
    bool have_app;
    image_ctx_t app;
    const char *want_file; // KilnCtrl-<tag>.bin, set by the caller
} manifest_ctx_t;

static update_rel_err_t image_elem(cur_t *c, int depth, void *vctx)
{
    manifest_ctx_t *m = (manifest_ctx_t *)vctx;
    image_ctx_t im;
    memset(&im, 0, sizeof(im));
    update_rel_err_t e = visit_object(c, depth + 1, image_kv, &im);
    if (e != UPDATE_REL_OK) {
        return e;
    }
    if (!im.name_ok || strcmp(im.name, "app") != 0) {
        return UPDATE_REL_OK; // recovery / other images are not ours to install
    }
    if (m->have_app) {
        return UPDATE_REL_E_NO_APP_IMAGE;
    }
    m->have_app = true;
    m->app = im;
    return UPDATE_REL_OK;
}

static update_rel_err_t manifest_kv(const char *key, cur_t *c, int depth, void *vctx)
{
    manifest_ctx_t *m = (manifest_ctx_t *)vctx;
    skip_ws(c);
    bool is_str = c->p < c->end && *c->p == '"';
    if (strcmp(key, "schema") == 0) {
        if (m->seen_schema) {
            return UPDATE_REL_E_JSON;
        }
        m->seen_schema = true;
        if (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            return read_number(c, &m->schema, &m->schema_ok) ? UPDATE_REL_OK : UPDATE_REL_E_JSON;
        }
        return skip_value(c, depth + 1);
    }
    if (strcmp(key, "tag") == 0) {
        if (m->seen_tag) {
            return UPDATE_REL_E_JSON;
        }
        m->seen_tag = true;
        if (is_str) {
            m->tag_ok = take_string(c, m->tag, sizeof(m->tag));
            return UPDATE_REL_OK;
        }
        return skip_value(c, depth + 1);
    }
    if (strcmp(key, "repo") == 0) {
        if (m->seen_repo) {
            return UPDATE_REL_E_JSON;
        }
        m->seen_repo = true;
        if (is_str) {
            m->repo_ok = take_string(c, m->repo, sizeof(m->repo));
            return UPDATE_REL_OK;
        }
        return skip_value(c, depth + 1);
    }
    if (strcmp(key, "commit") == 0) {
        if (m->seen_commit) {
            return UPDATE_REL_E_JSON;
        }
        m->seen_commit = true;
        if (is_str) {
            m->commit_ok = take_string(c, m->commit, sizeof(m->commit));
            return UPDATE_REL_OK;
        }
        return skip_value(c, depth + 1);
    }
    if (strcmp(key, "dirty") == 0) {
        if (m->seen_dirty) {
            return UPDATE_REL_E_JSON;
        }
        m->seen_dirty = true;
        if (read_literal(c, "false")) {
            m->dirty_false = true;
            return UPDATE_REL_OK;
        }
        return skip_value(c, depth + 1); // true or anything else: dirty_false stays false
    }
    if (strcmp(key, "compat") == 0) {
        if (m->seen_compat) {
            return UPDATE_REL_E_JSON;
        }
        m->seen_compat = true;
        if (c->p < c->end && *c->p == '{') {
            return visit_object(c, depth + 1, compat_kv, &m->compat);
        }
        m->compat.bad = true;
        return skip_value(c, depth + 1);
    }
    if (strcmp(key, "images") == 0) {
        if (m->seen_images) {
            return UPDATE_REL_E_JSON;
        }
        m->seen_images = true;
        if (c->p < c->end && *c->p == '[') {
            return visit_array(c, depth + 1, image_elem, m);
        }
        return skip_value(c, depth + 1);
    }
    return skip_value(c, depth + 1);
}

static bool ieq(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) {
            return false;
        }
    }
    return *a == *b;
}

update_rel_err_t update_release_parse_manifest(const char *json, size_t len, const char *repo, const char *tag,
                                               uint32_t api_app_size, update_manifest_t *out)
{
    if (out == NULL) {
        return UPDATE_REL_E_ARGS;
    }
    memset(out, 0, sizeof(*out));
    if (json == NULL || len == 0 || repo == NULL || tag == NULL || !update_repo_valid(repo) || !update_tag_valid(tag) ||
        api_app_size == 0) {
        return UPDATE_REL_E_ARGS;
    }
    manifest_ctx_t m;
    memset(&m, 0, sizeof(m));
    update_rel_err_t e = walk_document(json, len, manifest_kv, &m);
    if (e != UPDATE_REL_OK) {
        return e;
    }
    if (!m.schema_ok || m.schema != 1) {
        return UPDATE_REL_E_SCHEMA;
    }
    if (!m.tag_ok || strcmp(m.tag, tag) != 0) {
        return UPDATE_REL_E_TAG_MISMATCH;
    }
    if (m.seen_repo && (!m.repo_ok || !ieq(m.repo, repo))) {
        return UPDATE_REL_E_REPO_MISMATCH;
    }
    if (!m.commit_ok || !update_policy_commit_valid(m.commit)) {
        return UPDATE_REL_E_BAD_COMMIT;
    }
    if (!m.dirty_false) {
        return UPDATE_REL_E_DIRTY;
    }
    compat_ctx_t *k = &m.compat;
    if (!m.seen_compat || k->bad || !k->seen_zones || !k->seen_kl || !k->seen_uart || !k->seen_sha ||
        !update_policy_sha256_hex_valid(k->sha)) {
        return UPDATE_REL_E_BAD_COMPAT;
    }
    if (k->seen_min && k->min_from[0] != '\0') {
        update_semver_t v;
        if (!update_semver_parse(k->min_from, &v)) {
            return UPDATE_REL_E_BAD_COMPAT;
        }
    }
    char want_file[UPDATE_TAG_MAX + 16];
    snprintf(want_file, sizeof(want_file), "KilnCtrl-%s.bin", tag);
    if (!m.have_app || !m.app.file_ok || strcmp(m.app.file, want_file) != 0 || !m.app.size_ok || m.app.size == 0 ||
        !m.app.sha_ok || !update_policy_sha256_hex_valid(m.app.sha)) {
        return UPDATE_REL_E_NO_APP_IMAGE;
    }
    if (m.app.size != api_app_size) {
        return UPDATE_REL_E_SIZE_MISMATCH;
    }
    snprintf(out->identity.version, sizeof(out->identity.version), "%s", tag);
    snprintf(out->identity.commit, sizeof(out->identity.commit), "%s", m.commit);
    snprintf(out->identity.partitions_sha256, sizeof(out->identity.partitions_sha256), "%s", k->sha);
    out->identity.zones_cfg_version = k->zones;
    out->identity.kilnlink_version = k->kl;
    out->identity.uart_version = k->uart;
    out->identity.dirty = false;
    if (k->seen_min) {
        snprintf(out->identity.min_updatable_from, sizeof(out->identity.min_updatable_from), "%s", k->min_from);
    }
    out->app_size = m.app.size;
    snprintf(out->app_sha256, sizeof(out->app_sha256), "%s", m.app.sha);
    return UPDATE_REL_OK;
}

const char *update_rel_err_name(update_rel_err_t e)
{
    switch (e) {
    case UPDATE_REL_OK: return "ok";
    case UPDATE_REL_E_ARGS: return "bad_args";
    case UPDATE_REL_E_JSON: return "bad_json";
    case UPDATE_REL_E_NO_TAG: return "no_tag";
    case UPDATE_REL_E_BAD_TAG: return "bad_tag";
    case UPDATE_REL_E_DRAFT: return "draft_release";
    case UPDATE_REL_E_NO_APP: return "no_app_asset";
    case UPDATE_REL_E_NO_MANIFEST: return "no_manifest_asset";
    case UPDATE_REL_E_DUP_ASSET: return "duplicate_asset";
    case UPDATE_REL_E_BAD_URL: return "bad_asset_url";
    case UPDATE_REL_E_BAD_SIZE: return "bad_asset_size";
    case UPDATE_REL_E_SCHEMA: return "manifest_schema";
    case UPDATE_REL_E_TAG_MISMATCH: return "manifest_tag_mismatch";
    case UPDATE_REL_E_REPO_MISMATCH: return "manifest_repo_mismatch";
    case UPDATE_REL_E_BAD_COMMIT: return "manifest_bad_commit";
    case UPDATE_REL_E_DIRTY: return "manifest_dirty";
    case UPDATE_REL_E_BAD_COMPAT: return "manifest_bad_compat";
    case UPDATE_REL_E_NO_APP_IMAGE: return "manifest_no_app_image";
    case UPDATE_REL_E_SIZE_MISMATCH: return "manifest_size_mismatch";
    }
    return "unknown";
}
