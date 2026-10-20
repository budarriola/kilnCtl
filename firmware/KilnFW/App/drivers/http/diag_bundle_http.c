// diag_bundle_http.c -- GET /api/diag/bundle (FD-01, owner 2026-10-19): one
// streamed diagnostic support bundle for field debugging. Framing and the
// section runner are in diag_bundle.c (host-tested); this file holds the
// producers and the httpd glue.
//
// ADMIN tier (route_tier_table.h). Small chunks, no large heap buffers: every
// buffer here is <= 2 KB and lives in PSRAM (internal-RAM floor, owner
// 2026-10-01: heap_internal min_free >= 8192 B).
//
// What is NOT here and why: /api/status, readiness, partitions, boot_guard and
// OTA status JSON are built inline inside their own handlers (no reusable
// generator); the PC tool (diag_bundle_fetch) fetches them with the same
// admin login and merges them into the unpacked bundle. They are listed in
// the manifest's "pc_supplement" array so the contract is visible.
//
// ADDING A SECTION: write `static esp_err_t sec_xxx(diag_out_t *out, const char *name)`
// and add one `{"xxx", sec_xxx},` line to s_sections below.
#include "diag_bundle_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/json_escape_ctl.h"
#include "boot_guard.h"
#include "crash_report.h"
#include "diag_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "hal_sysinfo.h"
#include "http_auth_http.h" // kiln_http_register()
#include "log_store.h"
#include "ota_record.h"
#include "wifi_provision_http.h"

static const char *TAG = "diag_bundle_http";

#define DIAG_BUF_BYTES 1024u
#define DIAG_CORE_CHUNK 2048u

static esp_err_t sec_build(diag_out_t *out, const char *name)
{
    hal_sysinfo_build_info_t b;
    hal_sysinfo_get_build_info(&b);
    hal_sysinfo_partition_info_t run;
    bool have_run = (hal_sysinfo_get_running_partition(&run) == HAL_OK);
    char v[sizeof(b.version) * 2 + 1], d[sizeof(b.date) * 2 + 1], t[sizeof(b.time) * 2 + 1];
    kiln_json_escape_ctl(b.version, v, sizeof(v));
    kiln_json_escape_ctl(b.date, d, sizeof(d));
    kiln_json_escape_ctl(b.time, t, sizeof(t));
    char lbl[sizeof(run.label) * 2 + 1] = "";
    if (have_run) {
        kiln_json_escape_ctl(run.label, lbl, sizeof(lbl));
    }
    char json[384];
    int n = snprintf(json, sizeof(json),
                     "{\"valid\":%s,\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\",\"running_partition\":\"%s\"}",
                     b.valid ? "true" : "false", v, d, t, lbl);
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return diag_frame_write(out, name, json, (size_t)n);
}

static esp_err_t sec_boot_guard(diag_out_t *out, const char *name)
{
    uint32_t persisted = 0;
    bool have = boot_guard_get_persisted_count(&persisted);
    char json[160];
    int n = snprintf(json, sizeof(json),
                     "{\"boot_count\":%lu,\"persisted_count_known\":%s,\"persisted_count\":%lu,\"recovery_mode\":%s}",
                     (unsigned long)boot_guard_get_boot_count(), have ? "true" : "false",
                     (unsigned long)persisted, boot_guard_is_recovery_mode() ? "true" : "false");
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return diag_frame_write(out, name, json, (size_t)n);
}

static esp_err_t sec_ota_record(diag_out_t *out, const char *name)
{
    ota_record_t rec;
    if (ota_record_load(&rec) != ESP_OK) {
        return diag_frame_write(out, name, "{\"present\":false}", 17);
    }
    char proc[OTA_RECORD_PROCESSOR_MAX * 2 + 1], vb[OTA_RECORD_VERSION_STR_MAX * 2 + 1],
        va[OTA_RECORD_VERSION_STR_MAX * 2 + 1], rs[OTA_RECORD_REASON_MAX * 2 + 1];
    kiln_json_escape_ctl(rec.processor, proc, sizeof(proc));
    kiln_json_escape_ctl(rec.version_before, vb, sizeof(vb));
    kiln_json_escape_ctl(rec.version_after, va, sizeof(va));
    kiln_json_escape_ctl(rec.reason, rs, sizeof(rs));
    char json[512];
    int n = snprintf(json, sizeof(json),
                     "{\"present\":true,\"uptime_s\":%lu,\"processor\":\"%s\",\"version_before\":\"%s\","
                     "\"version_after\":\"%s\",\"success\":%s,\"reason\":\"%s\",\"image_sha256\":\"%s\"}",
                     (unsigned long)rec.uptime_s, proc, vb, va, rec.success ? "true" : "false", rs,
                     rec.image_sha256_hex);
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return diag_frame_write(out, name, json, (size_t)n);
}

static esp_err_t sec_crash_report(diag_out_t *out, const char *name)
{
    crash_report_record_t rec;
    if (!crash_report_get(&rec)) {
        return diag_frame_write(out, name, "{\"present\":false}", 17);
    }
    char task[sizeof(rec.exc_task) * 2 + 1], cause[sizeof(rec.exc_cause_str) * 2 + 1],
        build[sizeof(rec.fw_build) * 2 + 1], sha[sizeof(rec.dump_elf_sha) * 2 + 1];
    kiln_json_escape_ctl(rec.exc_task, task, sizeof(task));
    kiln_json_escape_ctl(rec.exc_cause_str, cause, sizeof(cause));
    kiln_json_escape_ctl(rec.fw_build, build, sizeof(build));
    kiln_json_escape_ctl(rec.dump_elf_sha, sha, sizeof(sha));
    char *json = heap_caps_malloc(DIAG_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t o = 0;
    int n = snprintf(json, DIAG_BUF_BYTES,
                     "{\"present\":true,\"exc_cause\":%lu,\"exc_cause_str\":\"%s\",\"exc_pc\":\"0x%08lx\","
                     "\"exc_task\":\"%s\",\"dump_id\":%lu,\"fw_build\":\"%s\",\"dump_elf_sha\":\"%s\","
                     "\"crash_uptime_s\":%lu,\"backtrace\":[",
                     (unsigned long)rec.exc_cause, cause, (unsigned long)rec.exc_pc, task,
                     (unsigned long)rec.dump_id, build, sha, (unsigned long)rec.crash_uptime_s);
    o = (n > 0 && (size_t)n < DIAG_BUF_BYTES) ? (size_t)n : 0;
    for (uint8_t i = 0; o > 0 && i < rec.bt_count && i < CRASH_REPORT_BT_MAX; i++) {
        n = snprintf(json + o, DIAG_BUF_BYTES - o, "%s\"0x%08lx\"", i ? "," : "", (unsigned long)rec.backtrace_pc[i]);
        if (n <= 0 || (size_t)n >= DIAG_BUF_BYTES - o) {
            o = 0;
            break;
        }
        o += (size_t)n;
    }
    if (o > 0) {
        n = snprintf(json + o, DIAG_BUF_BYTES - o, "]}");
        o = (n > 0 && (size_t)n < DIAG_BUF_BYTES - o) ? o + (size_t)n : 0;
    }
    esp_err_t e = (o > 0) ? diag_frame_write(out, name, json, o) : ESP_ERR_INVALID_SIZE;
    free(json);
    return e;
}

/* Raw 32-byte event records, coalesced into <= 1 KB frames. */
static esp_err_t stream_log(diag_out_t *out, const char *name, log_store_kind_t kind)
{
    uint8_t *buf = heap_caps_malloc(DIAG_BUF_BYTES + LOG_STORE_MAX_RECORD_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    log_store_reader_t *rd = log_store_reader_open(kind);
    if (rd == NULL) {
        free(buf);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t e = ESP_OK;
    size_t fill = 0, rec_len = 0;
    uint8_t *rec = buf + DIAG_BUF_BYTES;
    while (e == ESP_OK && log_store_reader_next(rd, rec, LOG_STORE_MAX_RECORD_LEN, &rec_len)) {
        if (rec_len > LOG_STORE_MAX_RECORD_LEN) {
            rec_len = LOG_STORE_MAX_RECORD_LEN;
        }
        if (fill + rec_len > DIAG_BUF_BYTES) {
            e = diag_frame_write(out, name, buf, fill);
            fill = 0;
        }
        memcpy(buf + fill, rec, rec_len);
        fill += rec_len;
    }
    if (e == ESP_OK && fill > 0) {
        e = diag_frame_write(out, name, buf, fill);
    }
    log_store_reader_close(rd);
    free(buf);
    return e;
}

static esp_err_t sec_firing_log(diag_out_t *out, const char *name)
{
    return stream_log(out, name, LOG_STORE_KIND_FIRING);
}

static esp_err_t sec_autotune_log(diag_out_t *out, const char *name)
{
    return stream_log(out, name, LOG_STORE_KIND_AUTOTUNE);
}

static esp_err_t sec_coredump(diag_out_t *out, const char *name)
{
    hal_sysinfo_coredump_info_t info;
    if (hal_sysinfo_coredump_get_info(&info) != HAL_OK) {
        return ESP_FAIL;
    }
    if (!info.present || info.data_len == 0) {
        return ESP_OK; /* no frames: the PC tool reads "section empty" as absent */
    }
    uint8_t *buf = heap_caps_malloc(DIAG_CORE_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t e = ESP_OK;
    for (uint32_t off = 0; e == ESP_OK && off < info.data_len;) {
        uint32_t len = info.data_len - off;
        if (len > DIAG_CORE_CHUNK) {
            len = DIAG_CORE_CHUNK;
        }
        if (hal_sysinfo_coredump_read(off, buf, len) != HAL_OK) {
            e = ESP_FAIL;
            break;
        }
        e = diag_frame_write(out, name, buf, len);
        off += len;
    }
    free(buf);
    return e;
}

/* FD-03 / FD-04 sections go here, one line each. */
static const diag_section_t s_sections[] = {
    {"build", sec_build},
    {"boot_guard", sec_boot_guard},
    {"ota_record", sec_ota_record},
    {"crash_report", sec_crash_report},
    {"firing_log", sec_firing_log},
    {"autotune_log", sec_autotune_log},
    {"coredump", sec_coredump},
};

const char *const DIAG_BUNDLE_PC_SUPPLEMENT_JSON =
    "[\"/api/status\",\"/api/readiness\",\"/api/partitions\",\"/api/boot_guard\","
    "\"/api/ota/esp/status\",\"/api/saftyfw_stack_margin\"]";

/* Coalescing sink: many tiny frame pieces -> <= 1 KB httpd chunks. */
typedef struct {
    httpd_req_t *req;
    uint8_t *buf;
    size_t fill;
} sink_ctx_t;

static esp_err_t sink_flush(sink_ctx_t *c)
{
    if (c->fill == 0) {
        return ESP_OK;
    }
    esp_err_t e = httpd_resp_send_chunk(c->req, (const char *)c->buf, c->fill);
    c->fill = 0;
    return e;
}

static esp_err_t http_sink(void *ctx, const void *data, size_t len)
{
    sink_ctx_t *c = (sink_ctx_t *)ctx;
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        size_t room = DIAG_BUF_BYTES - c->fill;
        size_t take = len < room ? len : room;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take;
        p += take;
        len -= take;
        if (c->fill == DIAG_BUF_BYTES) {
            esp_err_t e = sink_flush(c);
            if (e != ESP_OK) {
                return e;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t diag_bundle_get_handler(httpd_req_t *req)
{
    sink_ctx_t c = {.req = req, .fill = 0};
    c.buf = heap_caps_malloc(DIAG_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c.buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"kiln_support_bundle.kdiag\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = diag_bundle_run(s_sections, sizeof(s_sections) / sizeof(s_sections[0]),
                                  DIAG_BUNDLE_PC_SUPPLEMENT_JSON, http_sink, &c);
    if (e == ESP_OK) {
        e = sink_flush(&c);
    }
    free(c.buf);
    if (e == ESP_OK) {
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    ESP_LOGW(TAG, "bundle aborted: %s", esp_err_to_name(e));
    return ESP_FAIL; /* no terminating chunk: the client sees a truncated transfer */
}

esp_err_t diag_bundle_http_register(void *server_handle)
{
    static const httpd_uri_t bundle_uri = {
        .uri = "/api/diag/bundle", .method = HTTP_GET, .handler = diag_bundle_get_handler,
    };
    esp_err_t err = kiln_http_register((httpd_handle_t)server_handle, &bundle_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/diag/bundle) failed: %s", esp_err_to_name(err));
    }
    return err;
}
