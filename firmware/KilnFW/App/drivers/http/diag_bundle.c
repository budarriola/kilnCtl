#include "diag_bundle.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define DIAG_NAME_MAX 31

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

esp_err_t diag_frame_write(diag_out_t *out, const char *name, const void *data, size_t len)
{
    if (out == NULL || out->sink == NULL || !name_ok(name) || (data == NULL && len > 0)) {
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

/* Manifest/trailer JSON goes through this buffer (static section names only). */
#define DIAG_JSON_MAX 1536

esp_err_t diag_bundle_run(const diag_section_t *sections, size_t n, const char *pc_supplement,
                          diag_sink_fn sink, void *sink_ctx)
{
    if (sink == NULL || (sections == NULL && n > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    diag_out_t out = {.sink = sink, .sink_ctx = sink_ctx};
    char json[DIAG_JSON_MAX];
    size_t o;
    int w = snprintf(json, sizeof(json), "{\"format\":\"KDIAG1\",\"sections\":[");
    o = (w > 0) ? (size_t)w : 0;
    for (size_t i = 0; i < n; i++) {
        w = snprintf(json + o, sizeof(json) - o, "%s\"%s\"", i ? "," : "", sections[i].name);
        if (w < 0 || (size_t)w >= sizeof(json) - o) {
            return ESP_ERR_INVALID_ARG; /* too many sections for the manifest */
        }
        o += (size_t)w;
    }
    w = snprintf(json + o, sizeof(json) - o, "],\"pc_supplement\":%s}", pc_supplement ? pc_supplement : "[]");
    if (w < 0 || (size_t)w >= sizeof(json) - o) {
        return ESP_ERR_INVALID_ARG;
    }
    o += (size_t)w;
    if (diag_frame_write(&out, "_manifest", json, o) != ESP_OK) {
        return out.sink_err != ESP_OK ? out.sink_err : ESP_ERR_INVALID_ARG;
    }

    w = snprintf(json, sizeof(json), "{\"results\":[");
    o = (w > 0) ? (size_t)w : 0;
    for (size_t i = 0; i < n; i++) {
        out.section_bytes = 0;
        esp_err_t e = sections[i].produce ? sections[i].produce(&out, sections[i].name) : ESP_ERR_INVALID_ARG;
        if (out.sink_err != ESP_OK) {
            return out.sink_err;
        }
        w = snprintf(json + o, sizeof(json) - o, "%s{\"name\":\"%s\",\"ok\":%s,\"err\":%d,\"bytes\":%lu}",
                     i ? "," : "", sections[i].name, e == ESP_OK ? "true" : "false", (int)e,
                     (unsigned long)out.section_bytes);
        if (w < 0 || (size_t)w >= sizeof(json) - o) {
            return ESP_ERR_INVALID_ARG;
        }
        o += (size_t)w;
    }
    w = snprintf(json + o, sizeof(json) - o, "],\"complete\":true}");
    if (w < 0 || (size_t)w >= sizeof(json) - o) {
        return ESP_ERR_INVALID_ARG;
    }
    o += (size_t)w;
    out.section_bytes = 0;
    return diag_frame_write(&out, "_trailer", json, o) == ESP_OK ? ESP_OK : out.sink_err;
}
