#include "ota_http.h"
#include "ota_http_internal.h"
#include "ota_http_util.h"

#include <stdarg.h>
#include <string.h>

#include "psa/crypto.h"

#include "build_info.h" /* FW_GIT_COMMIT/FW_GIT_DIRTY/FW_BUILD_DATE/FW_BUILD_TIME -- TODO.md 9.6's
                          * per-processor build-identity fields for the ESP side, same header
                          * safety_link.c already includes for the ANNOUNCE_VERSION payload */
#include "esp_app_desc.h" /* esp_app_desc_t -- esp_ota_get_partition_description() (inactive slot) still uses this directly, out of hal_sysinfo's scope */
#include "esp_app_format.h" /* esp_image_header_t, ESP_IMAGE_HEADER_MAGIC -- section 3's pre-esp_ota_begin() check */
#include "hal_sysinfo.h" /* hal_sysinfo_get_build_info()/_get_running_partition() -- the RUNNING image's own version/label, see call sites below */
#include "http_auth_http.h" /* http_auth_caller_is_admin() -- ota_esp_status_get_handler() below trims
                              * exact commit/dirty/build-date identity to admins only; see call site */
#include "http_auth_policy_iface.h" /* http_auth_policy_web_enabled() -- see the is_admin comment
                                      * below (2026-09-17 audit finding 6) for why this route does
                                      * not trust http_auth_caller_is_admin() alone */
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_rom_crc.h" /* esp_rom_crc32_le() -- section 4's Pico-image running CRC32, see ota_pico_do_stage() */
#include "hal_time.h" /* hal_time_now_us() -- ota_record_fill()'s uptime-seconds timestamp below, was esp_timer_get_time() */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <math.h>

#include "autotune_engine.h"
#include "boot_button.h"
#include "boot_guard.h"
#include "kilnlink/kilnlink_rollback_result.h" /* KILNLINK_ROLLBACK_RESULT_REASON_* -- ota_pico_rollback_post_handler()'s response mapping */
#include "kiln_io.h"
#include "MAX31856.h"
#include "ota_auth.h"
#include "ota_pico_relay.h"
#include "ota_record.h"
#include "profile_executor.h" /* PROFILE_EXEC_* enum only, not its live state -- see below */
#include "run_state.h"
#include "stack_margin.h"
#include "web_encoding.h"
#include "sim_backend.h"
#include "wifi_prov.h"
#include "wifi_provision_http.h"
#include "zones_config_accessors.h"

// --- POST /api/ota/esp progress (ota_http.h's ota_http_get_esp_progress()) -
// Single writer (ota_esp_post_handler(), one at a time -- s_update_claim
// above already guarantees no second transfer overlaps it), arbitrarily
// many readers -- `volatile` is enough here, no semaphore needed, per
// ota_http.h's doc comment on why a torn read of a phase enum/percentage
// isn't a correctness problem the way the nonce/lockout state would be.
static volatile ota_http_esp_phase_t s_esp_phase = OTA_HTTP_ESP_PHASE_IDLE;
static volatile uint8_t s_esp_progress_pct = 0;

static void esp_progress_set(ota_http_esp_phase_t phase, uint8_t pct)
{
    s_esp_phase = phase;
    s_esp_progress_pct = pct;
}

void ota_http_get_esp_progress(ota_http_esp_phase_t *phase_out, uint8_t *percent_out)
{
    if (phase_out) {
        *phase_out = s_esp_phase;
    }
    if (percent_out) {
        *percent_out = s_esp_progress_pct;
    }
}

// Streamed in fixed-size chunks so the ~1.1-2 MB image never sits in RAM
// whole (UPDATE_PROTOCOL.md section 3: "a full image will not fit in RAM").
// 4 KB, matching the doc's own suggested size. This is `static`, NOT a
// stack buffer -- wifi_provision_http.c's httpd config.stack_size is 8192,
// already sized against the largest existing handler's *smaller* buffers
// (zones_post_handler's 2561-byte body, see that file's comment on the hang/
// reset it caused before being bumped); a 4 KB buffer on top of that same
// stack would eat half of it just for this one variable. Safe as a single
// shared buffer because it is only ever touched while s_update_claim (above)
// is held by THIS transfer -- ota_http_update_try_begin() guarantees no
// second ESP or Pico transfer can be in flight at the same time to race it.
#define OTA_ESP_CHUNK_SIZE 4096
static uint8_t s_ota_esp_chunk[OTA_ESP_CHUNK_SIZE];

// Everything from "the mutex is held" to "the mutex is released" -- a
// single function so ota_esp_post_handler() below has exactly one call site
// for ota_http_update_end(), per TODO.md 9.5's "use a single cleanup path,
// not duplicated calls at every return" requirement. Every exit -- success,
// a refused/corrupt image, a mid-transfer read/write failure -- sets
// `ok`/`fail_reason` and falls through to the one cleanup block at the
// bottom, which appends the NVS record and updates the progress snapshot
// exactly once regardless of which path got there.
static void ota_esp_do_transfer(httpd_req_t *req, const char *ip)
{
    bool ok = false;
    char fail_reason[OTA_RECORD_REASON_MAX] = "unknown failure";
    char version_after[OTA_RECORD_VERSION_STR_MAX] = "";
    esp_ota_handle_t handle = 0;
    bool ota_began = false;
    const esp_partition_t *target = NULL;

    // Image SHA-256, computed over exactly the bytes esp_ota_write() is
    // given (the 24-byte header first, then every streamed chunk) -- a
    // record of what was actually written, not a gate (see ota_record.h's
    // header comment: nothing compares this against an expected value).
    // Best-effort: a PSA failure here logs and leaves the record's hash
    // field empty rather than failing an otherwise-good transfer over it.
    psa_hash_operation_t sha_op = psa_hash_operation_init();
    bool sha_op_active = false;
    char sha_hex[OTA_RECORD_SHA256_HEX_MAX] = "";

    hal_sysinfo_build_info_t running_build;
    hal_sysinfo_get_build_info(&running_build);
    const char *version_before = (running_build.valid && running_build.version[0]) ? running_build.version : "";

    size_t content_len = req->content_len;
    if (content_len == 0) {
        ota_http_set_fail_reason(fail_reason, sizeof(fail_reason), "missing Content-Length / empty body");
        ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, fail_reason);
        goto cleanup;
    }

    target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ota_http_set_fail_reason(fail_reason, sizeof(fail_reason), "no free OTA partition");
        ESP_LOGE(OTA_HTTP_TAG, "OTA esp update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fail_reason);
        goto cleanup;
    }
    if (content_len > target->size) {
        ota_http_set_fail_reason(fail_reason, sizeof(fail_reason), "image (%u B) larger than the OTA partition (%u B)",
                 (unsigned)content_len, (unsigned)target->size);
        ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, fail_reason);
        goto cleanup;
    }

    // Per-connection socket timeout, NOT the server-wide default (that
    // stays whatever wifi_provision_http.c's config sets and applies to
    // every other endpoint) -- see ota_http.h's doc comment for why a
    // targeted setsockopt() here is the chosen fix over a global config
    // bump. This is a PER-RECV timeout (how long to wait for the NEXT
    // chunk to arrive), not a whole-transfer deadline -- TCP keeps
    // delivering chunks well inside this window on any link that is
    // actually making progress, so bounding each individual recv() call
    // at 30 s is what "covers the whole transfer" means in practice: the
    // total transfer can take minutes as long as no single gap between
    // chunks exceeds 30 s. A dead connection still times out and is
    // cleaned up; a slow-but-alive one is not punished for its aggregate
    // duration.
    {
        int sockfd = httpd_req_to_sockfd(req);
        if (sockfd >= 0) {
            struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
            if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
                ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: could not raise the socket receive timeout -- "
                              "the server-wide default will apply instead",
                         ip);
            }
        }
    }

    // Buffer just the image header (24 B) before esp_ota_begin() --
    // UPDATE_PROTOCOL.md section 3: "ESP-IDF images already carry a magic
    // byte and a chip ID; verify them before calling esp_ota_begin()."
    esp_progress_set(OTA_HTTP_ESP_PHASE_VERIFYING, 0);
    {
        esp_image_header_t hdr;
        size_t hdr_received = 0;
        while (hdr_received < sizeof(hdr)) {
            int ret = httpd_req_recv(req, ((char *)&hdr) + hdr_received, sizeof(hdr) - hdr_received);
            if (ret <= 0) {
                ota_http_set_fail_reason(fail_reason, sizeof(fail_reason), "body read failed/closed while reading the image header (%d)", ret);
                ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: %s", ip, fail_reason);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed while reading image header");
                goto cleanup;
            }
            hdr_received += (size_t)ret;
        }

        if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
            ota_http_set_fail_reason(fail_reason, sizeof(fail_reason), "not an ESP-IDF image (bad magic 0x%02X)", hdr.magic);
            ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a valid ESP-IDF image (bad magic)");
            goto cleanup;
        }
        if (hdr.chip_id != ESP_CHIP_ID_ESP32S3) {
            ota_http_set_fail_reason(fail_reason, sizeof(fail_reason), "image is for chip id %u, this board is ESP32-S3 (%u)",
                     (unsigned)hdr.chip_id, (unsigned)ESP_CHIP_ID_ESP32S3);
            ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image is built for a different chip");
            goto cleanup;
        }

        esp_err_t rc = esp_ota_begin(target, content_len, &handle);
        if (rc != ESP_OK) {
            ota_http_set_fail_reason(fail_reason, sizeof(fail_reason), "esp_ota_begin failed: %s", esp_err_to_name(rc));
            ESP_LOGE(OTA_HTTP_TAG, "OTA esp update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
            goto cleanup;
        }
        ota_began = true;

        psa_status_t hs = psa_hash_setup(&sha_op, PSA_ALG_SHA_256);
        sha_op_active = (hs == PSA_SUCCESS);
        if (!sha_op_active) {
            ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: psa_hash_setup failed (%d) -- record will have no "
                          "image hash, transfer continues",
                     ip, (int)hs);
        }

        rc = esp_ota_write(handle, &hdr, sizeof(hdr));
        if (rc != ESP_OK) {
            ota_http_set_fail_reason(fail_reason, sizeof(fail_reason), "esp_ota_write (header) failed: %s", esp_err_to_name(rc));
            ESP_LOGE(OTA_HTTP_TAG, "OTA esp update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
            goto cleanup;
        }
        if (sha_op_active) {
            (void)psa_hash_update(&sha_op, (const uint8_t *)&hdr, sizeof(hdr));
        }
    }

    // Stream the rest. Never read ahead of what esp_ota_write() has
    // consumed -- each loop iteration reads one chunk and writes it before
    // asking for the next, so TCP flow control (not a read-ahead buffer
    // with nowhere to go) paces the transfer, per UPDATE_PROTOCOL.md's
    // "do not read the request body faster than the link drains."
    {
        size_t written = sizeof(esp_image_header_t);
        int last_logged_decile = 0;
        esp_progress_set(OTA_HTTP_ESP_PHASE_WRITING, 0);
        while (written < content_len) {
            size_t want = content_len - written;
            if (want > sizeof(s_ota_esp_chunk)) {
                want = sizeof(s_ota_esp_chunk);
            }
            int ret = httpd_req_recv(req, (char *)s_ota_esp_chunk, want);
            if (ret <= 0) {
                ota_http_set_fail_reason(fail_reason, sizeof(fail_reason), "body read failed/closed at %u/%u bytes (%d)",
                         (unsigned)written, (unsigned)content_len, ret);
                ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: %s", ip, fail_reason);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed mid-transfer");
                goto cleanup;
            }

            esp_err_t rc = esp_ota_write(handle, s_ota_esp_chunk, (size_t)ret);
            if (rc != ESP_OK) {
                ota_http_set_fail_reason(fail_reason, sizeof(fail_reason), "esp_ota_write failed at %u bytes: %s",
                         (unsigned)written, esp_err_to_name(rc));
                ESP_LOGE(OTA_HTTP_TAG, "OTA esp update from %s: %s", ip, fail_reason);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
                goto cleanup;
            }
            if (sha_op_active) {
                (void)psa_hash_update(&sha_op, s_ota_esp_chunk, (size_t)ret);
            }
            written += (size_t)ret;

            // Progress every ~10% (TODO.md 9.5: "progress pushed... at
            // least every 2 s" -- decile logging on a multi-second/minute
            // transfer satisfies that cadence without flooding the log on
            // a fast LAN).
            int decile = (int)((written * 10u) / content_len);
            if (decile > last_logged_decile) {
                last_logged_decile = decile;
                uint8_t pct = (uint8_t)((written * 100u) / content_len);
                esp_progress_set(OTA_HTTP_ESP_PHASE_WRITING, pct);
                ESP_LOGI(OTA_HTTP_TAG, "OTA esp update from %s: %u%% (%u/%u bytes)", ip, pct,
                         (unsigned)written, (unsigned)content_len);
            }
        }
    }

    esp_progress_set(OTA_HTTP_ESP_PHASE_FINALIZING, 100);
    {
        esp_err_t rc = esp_ota_end(handle);
        // esp_ota_end() frees the handle regardless of its result (see its
        // own doc comment) -- ota_began must go false here either way so
        // the cleanup block below never calls esp_ota_abort() on a handle
        // that no longer exists.
        ota_began = false;
        if (rc != ESP_OK) {
            ota_http_set_fail_reason(fail_reason, sizeof(fail_reason), "esp_ota_end failed: %s (image validation failed?)",
                     esp_err_to_name(rc));
            ESP_LOGE(OTA_HTTP_TAG, "OTA esp update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "esp_ota_end failed -- image rejected");
            goto cleanup;
        }

        rc = esp_ota_set_boot_partition(target);
        if (rc != ESP_OK) {
            ota_http_set_fail_reason(fail_reason, sizeof(fail_reason), "esp_ota_set_boot_partition failed: %s", esp_err_to_name(rc));
            ESP_LOGE(OTA_HTTP_TAG, "OTA esp update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not set boot partition -- "
                                                                       "old image is still active");
            goto cleanup;
        }
    }

    // Real, not a placeholder: read back the app description from the
    // partition that was just written, the same way esp_ota_get_partition_
    // description() is documented to be used for an inactive slot's
    // version. Best-effort -- a failure here does not undo a successful
    // update, it just leaves version_after blank in the record.
    {
        esp_app_desc_t written_desc;
        if (esp_ota_get_partition_description(target, &written_desc) == ESP_OK) {
            strncpy(version_after, written_desc.version, sizeof(version_after) - 1);
            version_after[sizeof(version_after) - 1] = '\0';
        }
    }

    ok = true;
    strncpy(fail_reason, "ok", sizeof(fail_reason));
    ESP_LOGI(OTA_HTTP_TAG, "OTA esp update from %s: complete, %u bytes written to '%s', now %s -- "
                  "reboot required to run it (this image stays PENDING_VERIFY until "
                  "ota_rollback_confirm_task() in main.c confirms it)",
             ip, (unsigned)content_len, target->label, version_after[0] ? version_after : "(unknown version)");

cleanup:
    if (ota_began) {
        // Any goto above that happens after esp_ota_begin() succeeded but
        // before esp_ota_end() ran leaves ota_began true -- abort so the
        // partial write can never be selected as a boot target.
        esp_ota_abort(handle);
    }

    // Finish (on success -- the hash covers exactly the bytes that made it
    // into the flash write path) or abort (on failure -- PSA requires every
    // started operation to be finished or aborted, and a failed transfer's
    // partial hash is not meaningful anyway) whatever hash operation was
    // started above. sha_hex stays "" if no operation was ever started, or
    // if psa_hash_finish() itself failed.
    if (sha_op_active) {
        if (ok) {
            uint8_t digest[32];
            size_t digest_len = 0;
            psa_status_t hs = psa_hash_finish(&sha_op, digest, sizeof(digest), &digest_len);
            if (hs == PSA_SUCCESS && digest_len == sizeof(digest)) {
                hex_encode(digest, sizeof(digest), sha_hex);
            } else {
                ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: psa_hash_finish failed (%d) -- record will "
                              "have no image hash",
                         ip, (int)hs);
            }
        } else {
            (void)psa_hash_abort(&sha_op);
        }
    }

    esp_progress_set(ok ? OTA_HTTP_ESP_PHASE_DONE : OTA_HTTP_ESP_PHASE_FAILED,
                      ok ? 100 : s_esp_progress_pct);

    {
        ota_record_t rec;
        ota_record_fill(&rec, (uint32_t)(hal_time_now_us() / 1000000), "esp", version_before,
                         version_after, ok, fail_reason, sha_hex);
        ota_record_append(&rec); // best-effort, logs its own failure -- see ota_record.h
    }

    if (ok) {
        char body[128];
        int n = snprintf(body, sizeof(body), "{\"ok\":true,\"bytes\":%u,\"partition\":\"%s\",\"version\":\"%s\"}",
                          (unsigned)content_len, target->label, version_after);
        httpd_resp_set_type(req, "application/json");
        ota_http_send_json_clamped(req, body, n, sizeof(body));
    }
    // On failure, the specific httpd_resp_send_err() call above (at
    // whichever goto fired) has already sent the response -- nothing left
    // to send here.

    // Released exactly once, regardless of which path got here -- the
    // single-cleanup-path requirement this whole function exists to
    // satisfy. Idempotent even if something above went wrong before the
    // claim was actually held, per ota_http_update_end()'s own doc comment.
    ota_http_update_end();
}

esp_err_t ota_esp_post_handler(httpd_req_t *req)
{
    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip));

    // 1. X-Ota-Mac header present and exactly 64 hex chars -- refused
    // before the body is touched at all, per ota_http.h's documented order.
    size_t mac_hex_len = httpd_req_get_hdr_value_len(req, OTA_MAC_HEADER);
    if (mac_hex_len != 64) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: missing or malformed X-Ota-Mac header (len %u, want 64)",
                 ip, (unsigned)mac_hex_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or malformed X-Ota-Mac header (want 64 hex chars)");
        return ESP_OK;
    }
    char mac_hex[65];
    if (httpd_req_get_hdr_value_str(req, OTA_MAC_HEADER, mac_hex, sizeof(mac_hex)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "could not read X-Ota-Mac header");
        return ESP_OK;
    }
    uint8_t mac[32];
    if (!hex_decode(mac_hex, 64, mac)) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: X-Ota-Mac is not valid hex", ip);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Ota-Mac must be 64 hex characters");
        return ESP_OK;
    }

    // 2. Auth -- refuse immediately (403) on anything but OK, still before
    // the body is read.
    ota_http_verify_result_t vr = ota_http_verify_request(OTA_HTTP_CONTEXT_ESP, mac, ip);
    if (vr != OTA_HTTP_VERIFY_OK) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, verify_result_str(vr));
        return ESP_OK;
    }

    // 3. Interlocks -- run AFTER auth (see ota_http_check_interlocks()'s own
    // doc comment for why: an unauthenticated interlock check would leak
    // live kiln telemetry). esp_http_server's httpd_err_code_t has no 409
    // entry, so the "409 Conflict" status TODO.md asks for ("409 or
    // similar") is set directly via httpd_resp_set_status() rather than
    // httpd_resp_send_err(), which only knows the enum's fixed set.
    char reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req), reason,
                                                            sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: refused by interlock: %s", ip, reason);
        return ota_http_send_interlock_refusal(req, gate, reason);
    }

    // 4. Single update mutex -- claimed before any body byte is read, so a
    // second concurrent attempt (another tab, an agent racing a human) is
    // refused immediately rather than partway through a transfer.
    if (!ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP)) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA esp update from %s: refused, an update is already in progress", ip);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "an update is already in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // From here, the mutex is held and ota_esp_do_transfer() owns releasing
    // it exactly once, on every exit path -- see that function's own doc
    // comment.
    ota_esp_do_transfer(req, ip);
    return ESP_OK;
}

// --- POST /api/ota/pico, GET /api/ota/pico/status (TODO.md 9.5) -----------
// See ota_http.h's header comment on this section for the full wire
// contract and the mutex-ownership handoff to ota_pico_relay.c.


// GET /api/ota/esp/status -- see ota_http.h's doc comment above
// ota_http_get_esp_progress() for the full field-by-field contract. Closes
// the gap ota_http_client.py's module doc comment and mcp_server.py's
// ota_status() doc comment both flagged: neither the ESP self-update's own
// progress nor the persisted ota_record.h "last update" blob had an HTTP
// route before this handler.
esp_err_t ota_esp_status_get_handler(httpd_req_t *req)
{
    ota_http_esp_phase_t phase;
    uint8_t percent;
    ota_http_get_esp_progress(&phase, &percent);

    // ota_record_load() is null-tolerant on "no record yet" the same way
    // safety_link_get_status() is null-tolerant on "no link this boot" --
    // ESP_ERR_NVS_NOT_FOUND (or any other non-OK, e.g. NVS partition not
    // yet initialized) means "nothing to report", not an error worth
    // failing this GET over. last_update stays absent (JSON null) in
    // exactly that case.
    ota_record_t rec;
    bool have_record = (ota_record_load(&rec) == ESP_OK);

    // TODO.md 9.6: "running version, build commit, build date, dirty flag,
    // active slot, and the version sitting in the inactive slot" -- none of
    // this was on any existing HTTP route before this pass (dashboard_http.c's
    // /api/status has no such fields; grepped for esp_app_get_description/
    // FW_GIT_COMMIT/esp_ota_get_running_partition there and found nothing).
    // Added directly to this already-existing status route rather than a new
    // one, same "small, contained addition to an existing endpoint" the
    // Pico-status handler below also gets for its own available fields.
    hal_sysinfo_build_info_t running_build;
    hal_sysinfo_get_build_info(&running_build);
    const char *running_version = (running_build.valid && running_build.version[0]) ? running_build.version : "";
    hal_sysinfo_partition_info_t running_info;
    bool have_running_info = (hal_sysinfo_get_running_partition(&running_info) == HAL_OK);
    const char *active_slot = have_running_info ? running_info.label : "unknown";

    const esp_partition_t *inactive_part = esp_ota_get_next_update_partition(NULL);
    const char *inactive_slot = inactive_part ? inactive_part->label : "unknown";
    char inactive_version[33] = "";
    if (inactive_part) {
        esp_app_desc_t inactive_desc;
        if (esp_ota_get_partition_description(inactive_part, &inactive_desc) == ESP_OK) {
            strncpy(inactive_version, inactive_desc.version, sizeof(inactive_version) - 1);
            inactive_version[sizeof(inactive_version) - 1] = '\0';
        }
        // Left blank (not "unknown") when the inactive slot has no readable
        // app descriptor -- an erased/never-flashed factory or ota_1
        // partition on a fresh board is a real, common state, not an error;
        // the page renders an empty string as "(empty)" itself.
    }

    // rec.reason (ota_record_fill()'s callers, ota_esp_do_transfer() above)
    // is always this codebase's own snprintf() output -- never copied
    // verbatim from an external source -- so, same as
    // ota_pico_status_get_handler()'s last_error field below, it cannot
    // contain a raw '"' or '\' that would need JSON escaping here. version/
    // active_slot/inactive_slot are equally safe: version comes from this
    // firmware's own PROJECT_VER (esp_app_desc_t), the slot labels come from
    // the partition table (esp_partition_t::label), and inactive_version
    // comes from the SAME struct field on a partition this build itself
    // wrote (or its factory-default) -- none of these are attacker-supplied.
    // boot_guard.h / ROADMAP.md watchdog-recovery pass: surfaced here so the
    // OTA page (and anyone polling this JSON) can show "this board is in
    // recovery mode" without needing a separate route. recovery_mode is
    // decided once, at boot, by boot_guard_is_recovery_mode() -- it does not
    // change within a boot even after boot_guard_mark_healthy() clears the
    // counter for the NEXT boot (see boot_guard.h's doc comment).
    bool recovery_mode = boot_guard_is_recovery_mode();

    // route_tier_table.h keeps this route ROUTE_TIER_OPEN deliberately (OTA
    // clients call it pre-authentication) -- but the OPEN tier's own test
    // (WEB_AUTH_PLAN.md section 2b: "what an onlooker standing at the kiln
    // can already see") does not cover exact firmware build identity: the
    // commit hash and dirty-build flag narrow an attacker's search for a
    // specific known defect, and the build date/time adds nothing an
    // onlooker could not otherwise infer less precisely. Rather than
    // retiering the route (which would break pre-auth OTA callers) or
    // dropping these fields from the schema (which would break the
    // authenticated /ota admin page's renderEspInfo(), which reads them),
    // redact just the VALUES to JSON null for a non-admin caller and keep
    // the real values for an authenticated admin -- kiln_http_prehandler()
    // never resolves a role for OPEN routes (see its own comment), so this
    // handler resolves one for itself via http_auth_caller_is_admin().
    //
    // recovery_mode is deliberately NOT redacted: app.js's pollRecoveryMode()
    // reads it on the OPEN-tier main dashboard to show a recovery-mode
    // banner, a shipped fix for the 2026-09-08 incident where recovery mode
    // was effectively invisible to whoever was standing at the board -- an
    // onlooker at the kiln can already see the board is unhealthy (LCD/relay
    // behaviour), so this is exactly the OPEN test's own "already visible"
    // case, not a narrowing fact like the commit hash is.
    //
    // 2026-09-17 audit finding 6: http_auth_caller_is_admin() alone is NOT
    // enough to gate this. It returns true unconditionally when web auth is
    // off (WEB_AUTH_PLAN.md section 11's "a board with auth off is exactly
    // as open as the board is today" -- deliberate, and load-bearing
    // elsewhere: security_http.c's /api/auth/security handler relies on
    // that exact "no auth system yet => admin" answer to let an operator set
    // the FIRST admin password on a board that has never had one, since
    // there is no session to resolve an ADMIN role from until one exists.
    // That is a real, intended use of "admin" meaning "carries admin
    // authority" for AUTHORIZATION purposes.
    //
    // This call site is asking a different question -- "should this
    // anonymous, pre-auth caller see sensitive build identity" -- and reusing
    // the authorization answer for that confidentiality decision is exactly
    // what made 9c2b1c1b's redaction inert on a default (auth-never-
    // configured) board: everyone is "admin" there, so everyone got the real
    // values. With no auth system installed, there is no caller anyone can
    // point to as more trusted than another, so the right default for a
    // DISCLOSURE decision is to redact for everyone until an administrator
    // has actually turned auth on and logged in -- i.e. this route must
    // require both a live policy that enables auth AND caller_is_admin()
    // resolving a real ADMIN session, not either alone. This does not change
    // http_auth_caller_is_admin()'s own meaning (still needed as-is for the
    // bootstrap case above) -- only how this one handler uses it.
    bool is_admin = http_auth_policy_web_enabled() && http_auth_caller_is_admin(req);
    char commit_json[64];
    char build_date_json[80];
    const char *dirty_json;
    if (is_admin) {
        snprintf(commit_json, sizeof(commit_json), "\"%s\"", FW_GIT_COMMIT);
        snprintf(build_date_json, sizeof(build_date_json), "\"%s\"", FW_BUILD_DATE " " FW_BUILD_TIME);
        dirty_json = FW_GIT_DIRTY ? "true" : "false";
    } else {
        strcpy(commit_json, "null");
        strcpy(build_date_json, "null");
        dirty_json = "null";
    }

    char body[768];
    int n;
    if (have_record) {
        n = snprintf(body, sizeof(body),
                      "{\"phase\":\"%s\",\"percent\":%u,"
                      "\"version\":\"%s\",\"commit\":%s,\"dirty\":%s,\"build_date\":%s,"
                      "\"active_slot\":\"%s\",\"inactive_slot\":\"%s\",\"inactive_version\":\"%s\","
                      "\"recovery_mode\":%s,"
                      "\"last_update\":"
                      "{\"processor\":\"%s\",\"version_before\":\"%s\",\"version_after\":\"%s\","
                      "\"success\":%s,\"reason\":\"%s\",\"uptime_s\":%u,\"image_sha256\":\"%s\"}}",
                      ota_http_esp_phase_str(phase), (unsigned)percent,
                      running_version, commit_json, dirty_json,
                      build_date_json, active_slot, inactive_slot, inactive_version,
                      recovery_mode ? "true" : "false",
                      rec.processor, rec.version_before,
                      rec.version_after, rec.success ? "true" : "false", rec.reason,
                      (unsigned)rec.uptime_s, rec.image_sha256_hex);
    } else {
        n = snprintf(body, sizeof(body),
                      "{\"phase\":\"%s\",\"percent\":%u,"
                      "\"version\":\"%s\",\"commit\":%s,\"dirty\":%s,\"build_date\":%s,"
                      "\"active_slot\":\"%s\",\"inactive_slot\":\"%s\",\"inactive_version\":\"%s\","
                      "\"recovery_mode\":%s,"
                      "\"last_update\":null}",
                      ota_http_esp_phase_str(phase), (unsigned)percent,
                      running_version, commit_json, dirty_json,
                      build_date_json, active_slot, inactive_slot, inactive_version,
                      recovery_mode ? "true" : "false");
    }
    httpd_resp_set_type(req, "application/json");
    ota_http_send_json_clamped(req, body, n, sizeof(body));
    return ESP_OK;
}

// --- POST /api/ota/esp/rollback -- see ota_http.h's doc comment above this
// section for the full contract. Runs on its own short-lived task (same
// factory_reset.c reboot_task() pattern) so the JSON response already
// queued by the handler has a chance to reach the client before the
// connection is torn down by the reboot.
static void ota_rollback_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));

    // KilnFW/TODO.md's "SAFETY_CMD_ANNOUNCE_REBOOT sent before the ESP
    // reboots" line: this call to esp_ota_mark_app_invalid_rollback_and_
    // reboot() below is the one existing path in this file that actually
    // calls esp_restart() (the plain OTA transfer path, ota_esp_do_
    // transfer(), only sets the boot partition and does not itself reboot --
    // see that function's own doc note -- so it has no reboot moment to hook
    // yet; when it grows one, it must send this too). Best-effort: a failed
    // send here does not block or abort the reboot -- worst case SaftyFW's
    // S6b guard behaves exactly as it did before this feature existed, which
    // is the same "no announcement" fallback the grace window itself
    // degrades to on expiry.
    esp_err_t announce_err = safety_link_send_announce_reboot(ota_http_safety);
    if (announce_err != ESP_OK) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA rollback: safety_link_send_announce_reboot failed (%s) -- "
                      "rebooting anyway, S6b may nuisance-trip on the safety processor",
                 esp_err_to_name(announce_err));
    }

    ESP_LOGW(OTA_HTTP_TAG, "OTA rollback: rebooting now into the previous image");
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    // Only reached if the call itself failed to even start the reboot --
    // on success this line never runs, the board is already restarting.
    ESP_LOGE(OTA_HTTP_TAG, "esp_ota_mark_app_invalid_rollback_and_reboot failed: %s -- "
                  "board NOT rebooted, still running the current image",
             esp_err_to_name(err));
}
esp_err_t ota_esp_rollback_post_handler(httpd_req_t *req)
{
    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip));

    // 1. X-Ota-Mac header present and exactly 64 hex chars -- same order as
    // ota_esp_post_handler(), before anything else is checked.
    size_t mac_hex_len = httpd_req_get_hdr_value_len(req, OTA_MAC_HEADER);
    if (mac_hex_len != 64) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA esp rollback from %s: missing or malformed X-Ota-Mac header (len %u, want 64)",
                 ip, (unsigned)mac_hex_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or malformed X-Ota-Mac header (want 64 hex chars)");
        return ESP_OK;
    }
    char mac_hex[65];
    if (httpd_req_get_hdr_value_str(req, OTA_MAC_HEADER, mac_hex, sizeof(mac_hex)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "could not read X-Ota-Mac header");
        return ESP_OK;
    }
    uint8_t mac[32];
    if (!hex_decode(mac_hex, 64, mac)) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA esp rollback from %s: X-Ota-Mac is not valid hex", ip);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Ota-Mac must be 64 hex characters");
        return ESP_OK;
    }

    // 2. Auth -- its own context (OTA_HTTP_CONTEXT_ESP_ROLLBACK), see
    // ota_http.h's doc comment on that enum value for why a rollback MAC is
    // not interchangeable with a plain-update MAC.
    ota_http_verify_result_t vr = ota_http_verify_request(OTA_HTTP_CONTEXT_ESP_ROLLBACK, mac, ip);
    if (vr != OTA_HTTP_VERIFY_OK) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, verify_result_str(vr));
        return ESP_OK;
    }

    // 3. Interlocks -- identical gate to POST /api/ota/esp: a rollback
    // reboots into different code just like an update does, so it is
    // exactly as disruptive and must be refused under the same conditions
    // (kiln not idle/cool, safety link down, another update in progress, ...).
    char reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req), reason,
                                                            sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA esp rollback from %s: refused by interlock: %s", ip, reason);
        return ota_http_send_interlock_refusal(req, gate, reason);
    }

    // 4. Single update mutex -- claimed as OTA_HTTP_CONTEXT_ESP (not a
    // separate rollback slot): a rollback is exactly as mutually exclusive
    // with an in-flight ESP or Pico update as a second ESP update would be,
    // there is still only one slot.
    if (!ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP)) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA esp rollback from %s: refused, an update is already in progress", ip);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "an update is already in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // 5. Is there actually a previous valid image to roll back to? Checked
    // explicitly rather than calling esp_ota_mark_app_invalid_rollback_and_
    // reboot() blind and letting it discover there is nothing -- refuses
    // cleanly, naming the reason, same as every other interlock in this file.
    if (!esp_ota_check_rollback_is_possible()) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA esp rollback from %s: refused, no previous valid image to roll back to", ip);
        ota_http_update_end();
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "no previous valid image to roll back to", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    hal_sysinfo_build_info_t running_build;
    hal_sysinfo_get_build_info(&running_build);
    const char *version_before = (running_build.valid && running_build.version[0]) ? running_build.version : "";

    {
        ota_record_t rec;
        // No image hash for a rollback record -- this action reverts to the
        // PREVIOUS image (already written and hashed, if at all, by whatever
        // update put it there), it does not write new bytes for this record
        // to hash.
        ota_record_fill(&rec, (uint32_t)(hal_time_now_us() / 1000000), "esp", version_before,
                         "", true, "rollback requested", NULL);
        ota_record_append(&rec); // best-effort, logs its own failure -- see ota_record.h
    }

    ESP_LOGW(OTA_HTTP_TAG, "OTA esp rollback from %s: accepted, was running '%s' -- rebooting into the "
                  "previous image", ip, version_before[0] ? version_before : "(unknown version)");

    char body[96];
    int n = snprintf(body, sizeof(body), "{\"ok\":true,\"status\":\"rebooting\",\"version_before\":\"%s\"}",
                      version_before);
    httpd_resp_set_type(req, "application/json");
    ota_http_send_json_clamped(req, body, n, sizeof(body));

    // The mutex is intentionally left held across the reboot -- there is no
    // "release it after the transfer" moment here the way ota_esp_do_
    // transfer()'s cleanup path has, because the board is about to reboot
    // out from under this claim entirely. A fresh boot starts with
    // s_update_claim reset to OTA_UPDATE_NONE (ota_http_start()), so there
    // is nothing left to release.
    static TaskHandle_t s_ota_rollback_reboot_task; /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): stack_margin_register() target */
    if (xTaskCreate(ota_rollback_reboot_task, "ota_rollback_reboot", 3072, NULL,
                     tskIDLE_PRIORITY + 1, &s_ota_rollback_reboot_task) != pdPASS) {
        ESP_LOGE(OTA_HTTP_TAG, "OTA esp rollback from %s: failed to start the reboot task -- "
                      "board will NOT reboot, still running the current image", ip);
        ota_http_update_end();
    }
    /* Registered unconditionally, success or not -- stack_margin_register()
     * reads *task_handle_slot fresh at report time, so a creation failure
     * just reads back alive=false rather than needing a second branch here.
     * 3072 must match the xTaskCreate() literal above. */
    stack_margin_register("ota_rollback_reboot", &s_ota_rollback_reboot_task, 3072);

    return ESP_OK;
}

// ota_pico_rollback_reason_str()/ota_pico_rollback_format_body() moved to
// ota_http_util.c (ota_http_pico_rollback_reason_str()/
// ota_http_pico_rollback_format_body()) -- see that file's header comment.
// Local aliases keep every call site below unchanged.
#define ota_pico_rollback_reason_str ota_http_pico_rollback_reason_str
#define ota_pico_rollback_format_body ota_http_pico_rollback_format_body
