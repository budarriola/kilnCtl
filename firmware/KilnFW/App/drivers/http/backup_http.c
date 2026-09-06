// backup_http -- Settings & profile backup/restore (TODO.md 0.5's settings
// import/export item and UI_PLAN.md's separately-named import/export item,
// unified into ONE feature per this pass's brief: they were the same
// request wearing two names, not two features).
//
// Serves GET /settings/backup (a dedicated page, not a section bolted onto
// settings_page.html -- both source docs call out import/export as its own
// destination), GET /api/backup/export (a streamed JSON file download, same
// Content-Disposition convention as dashboard_http.c's /api/history.csv) and
// POST /api/backup/import (validate-everything-before-writing-anything JSON
// upload).
//
// SCOPE, and why it is narrower than "every setting on the board": this pass
// owns settings_http.c/.h, profiles_http.c/.h, and their two *_page.html
// files -- NOT rules_http.c/.h or wifi_prov.c/.h, which other work this same
// session owns. zones_http.c/.h IS this file's to widen, though (2026-08-21,
// backup format version 2): a backup can only round-trip a value through a
// PUBLIC getter *and* a public SETTER, and zones_http.h originally exposed
// get+set pairs for only four things (per-channel PID gains, the FOPDT plant
// model, per-channel thermocouple type, and the RP2040 safety processor's own
// thermocouple type) -- everything else a zone stores (name, relay/
// thermocouple wiring, cal_offset_c, max ramp ceiling, sanity-rate threshold,
// control mode, temperature limits, heater timing, the 8 guard-threshold
// overrides, and cross_zone_max_delta_c) had a getter with no setter at all,
// so it read back on export but could never actually be restored. This pass
// added the missing setters to zones_http.h/.c and widened the export/import
// to match, closing that gap: a field that reads back but can't be written is
// worse than one that was never offered, and now every zone field is one or
// the other -- exported AND restorable.
//
// Fire profiles (profiles_http.h: profiles_http_get()/_save()/_delete(), a
// complete CRUD API already) round-trip in full.
//
// Wi-Fi credentials are DELIBERATELY NOT included, on either side. A backup
// restored onto a board on a different network with the original board's
// Wi-Fi credentials baked in would try to join a network that may not exist
// where the replacement board now lives, or -- worse -- silently join
// whatever network the ORIGINAL board's credentials happened to name if that
// SSID exists nearby (an operator who thinks they are restoring kiln tuning
// would not expect their kiln to also change what network it's on). Wi-Fi
// stays a per-board, page-local decision made on /wifi, same as it always
// has been; see backup_page.html's own on-page text, which says this
// explicitly rather than leaving it as a silent omission a reader would have
// to infer.
//
// SPLIT (2026-09-04, ROADMAP.md M15's 1500-line item): this file used to
// hold the whole feature (1768 lines) and now only holds the shared log tag
// and backup_http_start()'s route registration -- see backup_http_internal.h
// for the full split map (backup_json.c, backup_export.c, backup_import.c).

#include "backup_http.h"
#include "backup_http_internal.h"

#include "esp_log.h"

#include "wifi_provision_http.h"

const char *BACKUP_TAG = "backup_http";

esp_err_t backup_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(BACKUP_TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/settings/backup", .method = HTTP_GET, .handler = backup_page_get_handler,
    };
    static const httpd_uri_t export_uri = {
        .uri = "/api/backup/export", .method = HTTP_GET, .handler = backup_export_get_handler,
    };
    static const httpd_uri_t import_uri = {
        .uri = "/api/backup/import", .method = HTTP_POST, .handler = backup_import_post_handler,
    };

    esp_err_t err = httpd_register_uri_handler(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(BACKUP_TAG, "httpd_register_uri_handler(/settings/backup) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &export_uri);
    if (err != ESP_OK) {
        ESP_LOGE(BACKUP_TAG, "httpd_register_uri_handler(/api/backup/export) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &import_uri);
    if (err != ESP_OK) {
        ESP_LOGE(BACKUP_TAG, "httpd_register_uri_handler(/api/backup/import) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(BACKUP_TAG, "settings/backup page up");
    return ESP_OK;
}
