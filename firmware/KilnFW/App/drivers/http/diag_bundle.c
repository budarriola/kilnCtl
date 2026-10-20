#include "diag_bundle.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#define DIAG_NAME_MAX 31
/* DB-04: no single frame may exceed 1 MiB (producers write <= 2 KB frames); the PC parser caps likewise. */
#define DIAG_FRAME_MAX (1024u * 1024u)

static bool name_ok(const char *s)
{
    if (s == NULL || s[0] == '\0') {
        return false;
    }
    size_t n = strlen(s);
    if (n > DIAG_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* DB-07: names the PC tool writes itself (manifest.json, trailer.json, supplement_*.json,
 * coredump.symbolize.txt) or that mark the framing frames. A producer section may not use them. */
static bool name_reserved(const char *s)
{
    return s[0] == '_' || s[0] == '.' || strcmp(s, "manifest") == 0 || strcmp(s, "trailer") == 0 ||
           strcmp(s, "coredump.symbolize") == 0 || strncmp(s, "supplement_", 11) == 0;
}

static esp_err_t emit(diag_out_t *out, const void *d, size_t n)
{
    if (out->sink_err != ESP_OK) {
        return out->sink_err;
    }
    if (n == 0) {
        return ESP_OK;
    }
    esp_err_t e = out->sink(out->sink_ctx, d, n);
    if (e != ESP_OK) {
        out->sink_err = e;
    }
    return e;
}

static esp_err_t frame_write(diag_out_t *out, const char *name, const void *data, size_t len, bool framing)
{
    if (out == NULL || out->sink == NULL || !name_ok(name) || (!framing && name_reserved(name)) ||
        (data == NULL && len > 0) || len > DIAG_FRAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    char head[64];
    int n = snprintf(head, sizeof(head), "KDF1 %s %lu\n", name, (unsigned long)len);
    if (n <= 0 || (size_t)n >= sizeof(head)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t e = emit(out, head, (size_t)n);
    if (e == ESP_OK) {
        e = emit(out, data, len);
    }
    if (e == ESP_OK) {
        e = emit(out, "\n", 1);
    }
    if (e == ESP_OK) {
        out->section_bytes += (uint32_t)len;
    }
    return e;
}

esp_err_t diag_frame_write(diag_out_t *out, const char *name, const void *data, size_t len)
{
    return frame_write(out, name, data, len, false);
}

/* Manifest/trailer JSON goes through this buffer (static section names only; PSRAM, DB-01). */
#define DIAG_JSON_MAX 1536

static esp_err_t diag_bundle_run_json(const diag_section_t *sections, size_t n, const char *pc_supplement,
                                         const char *manifest_extra, diag_sink_fn sink, void *sink_ctx, char *json)
{
    diag_out_t out = {.sink = sink, .sink_ctx = sink_ctx};
    size_t o;
    int w = snprintf(json, DIAG_JSON_MAX, "{\"format\":\"KDIAG1\",\"sections\":[");
    o = (w > 0) ? (size_t)w : 0;
    for (size_t i = 0; i < n; i++) {
        w = snprintf(json + o, DIAG_JSON_MAX - o, "%s\"%s\"", i ? "," : "", sections[i].name);
        if (w < 0 || (size_t)w >= DIAG_JSON_MAX - o) {
            return ESP_ERR_INVALID_ARG; /* too many sections for the manifest */
        }
        o += (size_t)w;
    }
    w = snprintf(json + o, DIAG_JSON_MAX - o, "],\"pc_supplement\":%s%s%s}", pc_supplement ? pc_supplement : "[]",
                 manifest_extra ? "," : "", manifest_extra ? manifest_extra : "");
    if (w < 0 || (size_t)w >= DIAG_JSON_MAX - o) {
        return ESP_ERR_INVALID_ARG;
    }
    o += (size_t)w;
    if (frame_write(&out, "_manifest", json, o, true) != ESP_OK) {
        return out.sink_err != ESP_OK ? out.sink_err : ESP_ERR_INVALID_ARG;
    }

    w = snprintf(json, DIAG_JSON_MAX, "{\"results\":[");
    o = (w > 0) ? (size_t)w : 0;
    for (size_t i = 0; i < n; i++) {
        out.section_bytes = 0;
        esp_err_t e = sections[i].produce ? sections[i].produce(&out, sections[i].name) : ESP_ERR_INVALID_ARG;
        if (out.sink_err != ESP_OK) {
            return out.sink_err;
        }
        w = snprintf(json + o, DIAG_JSON_MAX - o, "%s{\"name\":\"%s\",\"ok\":%s,\"err\":%d,\"bytes\":%lu}",
                     i ? "," : "", sections[i].name, e == ESP_OK ? "true" : "false", (int)e,
                     (unsigned long)out.section_bytes);
        if (w < 0 || (size_t)w >= DIAG_JSON_MAX - o) {
            return ESP_ERR_INVALID_ARG;
        }
        o += (size_t)w;
    }
    w = snprintf(json + o, DIAG_JSON_MAX - o, "],\"complete\":true}");
    if (w < 0 || (size_t)w >= DIAG_JSON_MAX - o) {
        return ESP_ERR_INVALID_ARG;
    }
    o += (size_t)w;
    out.section_bytes = 0;
    return frame_write(&out, "_trailer", json, o, true) == ESP_OK ? ESP_OK : out.sink_err;
}

esp_err_t diag_bundle_run_x(const diag_section_t *sections, size_t n, const char *pc_supplement,
                            const char *manifest_extra, diag_sink_fn sink, void *sink_ctx)
{
    if (sink == NULL || (sections == NULL && n > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < n; i++) {
        if (!name_ok(sections[i].name) || name_reserved(sections[i].name)) {
            return ESP_ERR_INVALID_ARG; /* DB-07: nothing emitted */
        }
    }
    char *json = heap_caps_malloc(DIAG_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t r = diag_bundle_run_json(sections, n, pc_supplement, manifest_extra, sink, sink_ctx, json);
    free(json);
    return r;
}

esp_err_t diag_bundle_run(const diag_section_t *sections, size_t n, const char *pc_supplement,
                          diag_sink_fn sink, void *sink_ctx)
{
    return diag_bundle_run_x(sections, n, pc_supplement, NULL, sink, sink_ctx);
}

/* DB-02: manifest members recording whether the opt-in RAM dump is in this bundle. The warning
 * text lives here (not in diag_bundle_http.c, whose source is pinned to carry no credential words). */
const char *diag_bundle_manifest_extra(bool coredump)
{
    return coredump ? "\"coredump_included\":true,\"warning\":\"The crash dump is a RAM capture and may "
                      "contain the Wi-Fi password and web session data. Do not share this bundle publicly.\""
                    : "\"coredump_included\":false";
}
