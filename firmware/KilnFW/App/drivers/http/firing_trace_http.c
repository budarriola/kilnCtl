// FD-02: GET /api/firing_trace (ADMIN). No query: JSON list of the retained firings. ?id=N: that
// firing's durable per-minute trace as CSV. See persist/firing_trace.h.
#include "dashboard_http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "firing_trace.h"

static esp_err_t ft_list_json(httpd_req_t *req)
{
    firing_trace_entry_t e[FT_SLOTS];
    size_t n = firing_trace_list(e);
    char json[48 + FT_SLOTS * 160];
    size_t o = (size_t)snprintf(json, sizeof(json), "{\"firings\":[");
    for (size_t i = 0; i < n && o < sizeof(json); i++) {
        o += (size_t)snprintf(json + o, sizeof(json) - o,
                              "%s{\"id\":%lu,\"start_epoch\":%lu,\"samples\":%u,\"closed\":%s,\"truncated\":%s,"
                              "\"in_progress\":%s,\"interrupted\":%s,\"last_flush_epoch\":%lu}",
                              i ? "," : "", (unsigned long)e[i].seq, (unsigned long)e[i].start_epoch,
                              (unsigned)e[i].n, (e[i].flags & FT_FLAG_CLOSED) ? "true" : "false",
                              (e[i].flags & FT_FLAG_TRUNCATED) ? "true" : "false",
                              FT_IN_PROGRESS(e[i].flags) ? "true" : "false",
                              (e[i].flags & FT_FLAG_INTERRUPTED) ? "true" : "false",
                              (unsigned long)e[i].last_epoch);
    }
    if (o < sizeof(json)) o += (size_t)snprintf(json + o, sizeof(json) - o, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o < sizeof(json) ? o : sizeof(json) - 1);
}

esp_err_t firing_trace_get_handler(httpd_req_t *req)
{
    char query[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return ft_list_json(req);
    }
    char id_str[12];
    if (httpd_query_key_value(query, "id", id_str, sizeof(id_str)) != ESP_OK) {
        return ft_list_json(req);
    }
    char *end = NULL;
    unsigned long id = strtoul(id_str, &end, 10);
    if (end == id_str || *end != '\0' || id == 0 || id > 0xFFFFFFFEul) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
        return ESP_OK;
    }
    uint8_t *buf = heap_caps_malloc(FT_FILE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *line = heap_caps_malloc(FT_CSV_LINE_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf || !line) {
        free(buf);
        free(line);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    firing_trace_view_t v;
    esp_err_t err = firing_trace_open((uint32_t)id, buf, FT_FILE_MAX, &v);
    if (err != ESP_OK) {
        free(buf);
        free(line);
        if (err == ESP_ERR_NOT_FOUND) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such firing trace");
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "trace unreadable");
        }
        return ESP_OK;
    }
    char disp[64];
    snprintf(disp, sizeof(disp), "attachment; filename=\"firing_%lu%s.csv\"", id,
             FT_IN_PROGRESS(v.entry.flags) ? "_in_progress" : "");
    /* FY-04: the CSV holds only flushed samples; say so for an in-progress firing. */
    char lastf[16];
    snprintf(lastf, sizeof(lastf), "%lu", (unsigned long)v.entry.last_epoch);
    httpd_resp_set_hdr(req, "X-Firing-In-Progress", FT_IN_PROGRESS(v.entry.flags) ? "1" : "0");
    httpd_resp_set_hdr(req, "X-Firing-Last-Flush-Epoch", lastf);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    size_t o = firing_trace_csv_header(line, FT_CSV_LINE_CAP);
    err = httpd_resp_send_chunk(req, line, (ssize_t)o);
    for (uint16_t i = 0; i < v.entry.n && err == ESP_OK; i++) {
        o = firing_trace_csv_row(&v, i, line, FT_CSV_LINE_CAP);
        err = httpd_resp_send_chunk(req, line, (ssize_t)o);
    }
    free(buf);
    free(line);
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
