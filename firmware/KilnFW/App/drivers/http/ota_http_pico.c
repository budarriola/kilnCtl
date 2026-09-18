#include "ota_http.h"
#include "ota_http_internal.h"
#include "ota_http_util.h"

#include <stdarg.h>
#include <string.h>

#include "psa/crypto.h"

#include "build_info.h" /* FW_GIT_COMMIT/FW_GIT_DIRTY/FW_BUILD_DATE/FW_BUILD_TIME -- TODO.md 9.6's
                          * per-processor build-identity fields for the ESP side, same header
                          * safety_link.c already includes for the ANNOUNCE_VERSION payload */
#include "esp_app_desc.h"
#include "esp_app_format.h" /* esp_image_header_t, ESP_IMAGE_HEADER_MAGIC -- section 3's pre-esp_ota_begin() check */
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "hal_time.h"

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
#include "ota_image_crc.h" /* ota_image_crc32_update() -- the staging CRC32, in the one
                            * parameterization SaftyFW's bootloader_crc32() agrees with */
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

// Same static-not-stack reasoning as OTA_ESP_CHUNK_SIZE/s_ota_esp_chunk
// above. A SEPARATE buffer rather than reusing s_ota_esp_chunk: the two
// could technically share one (the cross-processor update mutex guarantees
// only one of the ESP or Pico transfer is ever in flight at a time), but
// keeping them distinct keeps each transfer's code readable on its own
// without a reader having to go verify that cross-file invariant first.
#define OTA_PICO_CHUNK_SIZE 4096
static uint8_t s_ota_pico_chunk[OTA_PICO_CHUNK_SIZE];

// Streams the browser upload into `pico_img`, computing a running CRC32
// alongside it via ota_image_crc.h -- NOT by calling esp_rom_crc32_le()
// here directly. That module owns the parameterization, because this CRC
// is compared on the other processor against SaftyFW bootloader_crc32()'s
// own reading of the same bytes, and the two must agree on the arithmetic
// as well as the data: standard CRC-32 (reflected poly 0xEDB88320, init
// 0xFFFFFFFF, final XOR 0xFFFFFFFF).
//
// Reaching that means seeding OTA_IMAGE_CRC32_INIT (zero), chaining each
// chunk's return value into the next call, and applying NO final XOR: the
// CRC primitive underneath complements the seed on entry and the result on
// exit itself. esp_rom_crc.h's "add ~ at the beginning and the end" note
// describes what that function does FOR the caller, not something the
// caller is meant to do as well. This code used to do it as well --
// seeding 0xFFFFFFFF and then XORing the result -- which does not cancel;
// it silently computes a different CRC-32 variant (init 0, no output XOR)
// that the Pico could never agree with, for any image. See
// docs/audits/pico_ota_staged_crc_mismatch_2026-09-18.md.
//
// On success, hands off to ota_pico_relay_start() and returns
// without releasing the update mutex (see ota_http.h's header comment for
// why); on any failure, releases the mutex itself and responds with a
// specific error.
static void ota_pico_do_stage(httpd_req_t *req, const char *ip)
{
    bool started_relay = false;
    char fail_reason[256] = "unknown failure";
    // Declared up here, not at first use, so every goto below (including
    // the very first check) can jump straight to the single cleanup block
    // without skipping past an initializer -- same discipline
    // ota_esp_do_transfer() uses for handle/ota_began/target.
    size_t content_len = 0;
    const esp_partition_t *part = NULL;
    uint32_t crc = OTA_IMAGE_CRC32_INIT; // seed 0, no final XOR -- see ota_image_crc.h
    size_t written = 0;

    // Image SHA-256 over every byte staged into pico_img -- same
    // best-effort, record-not-gate reasoning as ota_esp_do_transfer()'s own
    // copy of this pattern (see ota_record.h's header comment). The 32-byte
    // digest (not the hex string) is handed to ota_pico_relay_start(),
    // which owns turning it into the eventual ota_record_t for the Pico
    // path -- this function's own job ends at "staged successfully, relay
    // started."
    psa_hash_operation_t sha_op = psa_hash_operation_init();
    bool sha_op_active = false;
    uint8_t sha_digest[32];
    bool have_sha_digest = false;

    if (!ota_http_safety) {
        snprintf(fail_reason, sizeof(fail_reason), "no safety link configured this boot -- nothing to relay to");
        ESP_LOGE(OTA_HTTP_TAG, "OTA pico update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fail_reason);
        goto cleanup;
    }

    content_len = req->content_len;
    if (content_len == 0) {
        snprintf(fail_reason, sizeof(fail_reason), "missing Content-Length / empty body");
        ESP_LOGW(OTA_HTTP_TAG, "OTA pico update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, fail_reason);
        goto cleanup;
    }

    part = ota_pico_img_partition();
    if (!part) {
        snprintf(fail_reason, sizeof(fail_reason), "pico_img staging partition not found");
        ESP_LOGE(OTA_HTTP_TAG, "OTA pico update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fail_reason);
        goto cleanup;
    }
    if (content_len > part->size) {
        snprintf(fail_reason, sizeof(fail_reason), "image (%u B) larger than the pico_img partition (%u B)",
                 (unsigned)content_len, (unsigned)part->size);
        ESP_LOGW(OTA_HTTP_TAG, "OTA pico update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, fail_reason);
        goto cleanup;
    }

    // Same per-connection socket timeout rationale as ota_esp_do_transfer()
    // above -- a per-recv-call bound, not a whole-transfer deadline.
    {
        int sockfd = httpd_req_to_sockfd(req);
        if (sockfd >= 0) {
            struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
            if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
                ESP_LOGW(OTA_HTTP_TAG, "OTA pico update from %s: could not raise the socket receive timeout", ip);
            }
        }
    }

    // Erase only what this upload needs, rounded up to the flash sector
    // size esp_partition_write() requires already-erased -- not the whole
    // 896K partition, which would cost real time for no benefit on a
    // typical (much smaller) Pico image.
    {
        uint32_t sector = esp_partition_get_main_flash_sector_size();
        size_t erase_len = ((content_len + sector - 1u) / sector) * sector;
        esp_err_t erc = esp_partition_erase_range(part, 0, erase_len);
        if (erc != ESP_OK) {
            snprintf(fail_reason, sizeof(fail_reason), "pico_img erase failed: %s", esp_err_to_name(erc));
            ESP_LOGE(OTA_HTTP_TAG, "OTA pico update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash erase failed");
            goto cleanup;
        }
    }

    {
        psa_status_t hs = psa_hash_setup(&sha_op, PSA_ALG_SHA_256);
        sha_op_active = (hs == PSA_SUCCESS);
        if (!sha_op_active) {
            ESP_LOGW(OTA_HTTP_TAG, "OTA pico update from %s: psa_hash_setup failed (%d) -- record will have no "
                          "image hash, staging continues",
                     ip, (int)hs);
        }
    }

    // Stream the body into pico_img, one httpd_req_recv() per
    // esp_partition_write(), same "never read ahead of what has been
    // consumed" discipline as ota_esp_do_transfer() -- and the same reason
    // it matters here: UPDATE_PROTOCOL.md's "do not read the request body
    // faster than the link drains" is about the SLOW isolated-link relay
    // that happens after this handler returns, but reading the HTTP body
    // no faster than it can be written to flash is the same principle
    // applied to this (fast) staging step.
    int last_logged_decile = 0;
    while (written < content_len) {
        size_t want = content_len - written;
        if (want > sizeof(s_ota_pico_chunk)) {
            want = sizeof(s_ota_pico_chunk);
        }
        int ret = httpd_req_recv(req, (char *)s_ota_pico_chunk, want);
        if (ret <= 0) {
            snprintf(fail_reason, sizeof(fail_reason), "body read failed/closed at %u/%u bytes (%d)",
                     (unsigned)written, (unsigned)content_len, ret);
            ESP_LOGW(OTA_HTTP_TAG, "OTA pico update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed mid-transfer");
            goto cleanup;
        }

        esp_err_t werr = esp_partition_write(part, written, s_ota_pico_chunk, (size_t)ret);
        if (werr != ESP_OK) {
            snprintf(fail_reason, sizeof(fail_reason), "pico_img write failed at %u bytes: %s",
                     (unsigned)written, esp_err_to_name(werr));
            ESP_LOGE(OTA_HTTP_TAG, "OTA pico update from %s: %s", ip, fail_reason);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
            goto cleanup;
        }
        crc = ota_image_crc32_update(crc, s_ota_pico_chunk, (size_t)ret);
        if (sha_op_active) {
            (void)psa_hash_update(&sha_op, s_ota_pico_chunk, (size_t)ret);
        }
        written += (size_t)ret;

        int decile = (int)((written * 10u) / content_len);
        if (decile > last_logged_decile) {
            last_logged_decile = decile;
            ESP_LOGI(OTA_HTTP_TAG, "OTA pico update from %s: staged %u%% (%u/%u bytes)", ip,
                     (unsigned)((written * 100u) / content_len), (unsigned)written, (unsigned)content_len);
        }
    }

    if (sha_op_active) {
        size_t digest_len = 0;
        psa_status_t hs = psa_hash_finish(&sha_op, sha_digest, sizeof(sha_digest), &digest_len);
        if (hs == PSA_SUCCESS && digest_len == sizeof(sha_digest)) {
            have_sha_digest = true;
        } else {
            ESP_LOGW(OTA_HTTP_TAG, "OTA pico update from %s: psa_hash_finish failed (%d) -- record will have no "
                          "image hash",
                     ip, (int)hs);
        }
        sha_op_active = false; // finished (or failed to finish) -- nothing left to abort in cleanup
    }

    ESP_LOGI(OTA_HTTP_TAG, "OTA pico update from %s: staged %u bytes to pico_img, crc32=0x%08X -- starting relay",
             ip, (unsigned)written, (unsigned)crc);

    if (!ota_pico_relay_start(ota_http_safety, (uint32_t)written, crc, NULL, have_sha_digest ? sha_digest : NULL)) {
        snprintf(fail_reason, sizeof(fail_reason), "image staged, but the relay task could not be started");
        ESP_LOGE(OTA_HTTP_TAG, "OTA pico update from %s: %s", ip, fail_reason);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fail_reason);
        goto cleanup;
    }
    started_relay = true;

    {
        char body[160];
        int n = snprintf(body, sizeof(body),
                          "{\"ok\":true,\"status\":\"relay_started\",\"bytes\":%u,\"crc32\":\"0x%08X\"}",
                          (unsigned)written, (unsigned)crc);
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_set_type(req, "application/json");
        ota_http_send_json_clamped(req, body, n, sizeof(body));
    }

cleanup:
    // Any goto above that fired while sha_op_active was still true left a
    // PSA hash operation started-but-not-finished (a read/write failure
    // mid-stream, staging failing before ota_pico_relay_start() -- the
    // finish-or-fail-fast block above already turned sha_op_active back to
    // false on every path that actually reached it). PSA requires every
    // started operation to be finished or aborted; abort here rather than
    // leak it, same "always release the resource this function borrowed"
    // discipline as the ota_began/esp_ota_abort() cleanup in
    // ota_esp_do_transfer().
    if (sha_op_active) {
        (void)psa_hash_abort(&sha_op);
    }

    // Ownership handoff: if the relay task was successfully started, IT now
    // owns calling ota_http_update_end() (see ota_http.h's header comment
    // and ota_pico_relay.h's own for the full reasoning) -- calling it here
    // too would release a claim the relay task is still actively using.
    // Every OTHER path above (staging never got far enough to start a
    // relay) still owns cleanup itself, exactly like ota_esp_do_transfer()'s
    // single cleanup block.
    if (!started_relay) {
        ota_http_update_end();
    }
}

esp_err_t ota_pico_post_handler(httpd_req_t *req)
{
    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip));

    // Same four-step order as ota_esp_post_handler() -- see ota_http.h's
    // documented order and that handler's own comments for why each step
    // precedes the next.
    size_t mac_hex_len = httpd_req_get_hdr_value_len(req, OTA_MAC_HEADER);
    if (mac_hex_len != 64) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA pico update from %s: missing or malformed X-Ota-Mac header (len %u, want 64)",
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
        ESP_LOGW(OTA_HTTP_TAG, "OTA pico update from %s: X-Ota-Mac is not valid hex", ip);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Ota-Mac must be 64 hex characters");
        return ESP_OK;
    }

    ota_http_verify_result_t vr = ota_http_verify_request(OTA_HTTP_CONTEXT_PICO, mac, ip);
    if (vr != OTA_HTTP_VERIFY_OK) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, verify_result_str(vr));
        return ESP_OK;
    }

    char reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req), reason,
                                                            sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA pico update from %s: refused by interlock: %s", ip, reason);
        return ota_http_send_interlock_refusal(req, gate, reason);
    }

    if (!ota_http_update_try_begin(OTA_HTTP_CONTEXT_PICO)) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA pico update from %s: refused, an update is already in progress", ip);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "an update is already in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // From here, ota_pico_do_stage() owns the mutex -- either it releases
    // it itself (staging failure) or it starts the relay task, which then
    // owns release. See that function's own doc comment.
    ota_pico_do_stage(req, ip);
    return ESP_OK;
}

// Background task for POST /api/ota/pico/rollback -- see that handler's own
// doc comment (opus-review finding 3) for why this call moved off the httpd
// worker task. Owns releasing the OTA_HTTP_CONTEXT_PICO update-claim mutex
// the handler claimed before starting this task (same "task owns the
// release" contract ota_pico_do_stage()'s relay task already uses for the
// plain Pico update path), and owns writing the final outcome into
// ota_http_pico_rollback_async for ota_pico_rollback_status_get_handler() to read
// back. No arg: reads the same ota_http_safety this whole file already treats as
// fixed for the boot (set once by ota_http_start()).
static void ota_pico_rollback_task(void *arg)
{
    (void)arg;

    safety_link_rollback_outcome_t outcome = SAFETY_LINK_ROLLBACK_OUTCOME_LINK_DOWN;
    uint8_t reason_code = KILNLINK_ROLLBACK_RESULT_REASON_UNKNOWN;
    (void)safety_link_send_rollback_ex(ota_http_safety, &outcome, &reason_code);

    if (xSemaphoreTake(ota_http_pico_rollback_async_lock, portMAX_DELAY) == pdTRUE) {
        ota_http_pico_rollback_async.state = OTA_PICO_ROLLBACK_ASYNC_DONE;
        ota_http_pico_rollback_async.outcome = outcome;
        ota_http_pico_rollback_async.reason_code = reason_code;
        xSemaphoreGive(ota_http_pico_rollback_async_lock);
    }

    ESP_LOGW(OTA_HTTP_TAG, "OTA pico rollback: safety_link_send_rollback_ex outcome=%d reason=%u",
             (int)outcome, (unsigned)reason_code);

    // Released here, on EVERY outcome, now that the WHOLE rollback attempt
    // (send-burst/reply-window leg AND the boot_id-reconnect watch) has run
    // to completion on this task -- see ota_pico_rollback_post_handler()'s
    // own doc comment for why the claim must stay held for that entire
    // span, not just until the handler returns.
    ota_http_update_end();
    vTaskDelete(NULL);
}

// --- POST /api/ota/pico/rollback -------------------------------------------
//
// The Pico half of "roll back the firmware from the OTA page" -- the missing
// piece a previous pass on this feature correctly stopped short of shipping,
// because SAFETY_CMD_ROLLBACK used to be unable to tell the ESP whether the
// safety processor had refused (relay ARMED, or the other bootloader slot
// not VALID/PENDING_VERIFY) or accepted. kilnlink_rollback_result.h /
// safety_link_send_rollback_ex() close that gap; this handler is the HTTP
// surface on top of it.
//
// Auth: its own context, OTA_HTTP_CONTEXT_PICO_ROLLBACK ("pico-rollback") --
// NOT a reuse of OTA_HTTP_CONTEXT_PICO (pushing a new Pico image) and NOT a
// reuse of OTA_HTTP_CONTEXT_ESP_ROLLBACK (rolling the OTHER processor back)
// -- see ota_http.h's doc comment on the enum value for why a MAC signed for
// one action must never double as authorization for a different one.
//
// Heat interlock: a Pico rollback reboots the SAFETY processor mid-firing,
// which this handler treats as AT LEAST as disruptive as a plain Pico
// firmware update (heat_interlock.h already collapses any non-ESP update
// context onto HEAT_INTERLOCK_UPDATE_PICO) -- so the single cross-processor
// update mutex is claimed as OTA_HTTP_CONTEXT_PICO (the same slot a plain
// Pico update claims, not a separate one), same precedent
// ota_esp_rollback_post_handler() sets for the ESP side just above. Unlike
// the ESP rollback path, this ESP does not itself reboot, so there is no
// natural "claim survives until a fresh boot clears it" moment to lean on --
// holding the claim indefinitely with no release path would eventually wedge
// heat/updates for good if anything went wrong on the Pico side. Instead the
// claim is held for exactly as long as safety_link_send_rollback_ex() takes
// to run -- covering the send-burst/reply-window leg (SAFETY_LINK_REPLY_
// TIMEOUT_MS-ish, the moment update_task_request_rollback() reads the ARMED/
// relay-energized fact on the Pico, the actual race this mutex originally
// existed to prevent) AND the boot_id-reconnect watch that follows it
// (SAFETY_LINK_ROLLBACK_BOOT_WATCH_MS, safety_link.c) when no refusal
// arrives -- and released on every outcome. That second leg is exactly the
// "cover the Pico's own reboot-and-reload time" gap a previous pass here
// left open -- without it, ota_http_heat_blocked_by_update() (this claim's
// read side) would have stopped blocking heat the moment the reply window
// closed, while the safety processor could still be mid-reboot with no
// safety link at all, guarded only by safety_link_get_status()'s own
// up-to-1500ms-stale link_up. If the boot_id watch times out with no
// evidence either way, the claim is still released (UNKNOWN_TIMEOUT is not
// withheld forever) -- that residual window is bounded by link_up's own
// staleness check, the same as any other "safety link went quiet" case this
// driver already handles.
//
// Async since opus-review finding 3: safety_link_send_rollback_ex() blocks
// for up to ~6.3s, and esp_http_server here has exactly one worker task
// (wifi_provision_http.c), so running that call ON this handler's task used
// to queue every other request -- including the dashboard's ~1Hz /api/
// status poll -- behind a single rollback attempt. The call above ("held
// for exactly as long as... takes to run") is now literally true of a
// background task, ota_pico_rollback_task(), not of this handler: the
// handler itself does steps 1-4 below, starts that task, and returns a 202
// within normal request time. The claim is still held across the task's
// entire run (the invariant this comment exists to document is unchanged),
// it is just no longer this handler's own task doing the holding. Poll GET
// /api/ota/pico/rollback/status (ota_pico_rollback_status_get_handler(),
// defined right after this handler) for the eventual outcome.
esp_err_t ota_pico_rollback_post_handler(httpd_req_t *req)
{
    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip));

    // 1. X-Ota-Mac header present and exactly 64 hex chars -- same order as
    // every other mutating OTA handler.
    size_t mac_hex_len = httpd_req_get_hdr_value_len(req, OTA_MAC_HEADER);
    if (mac_hex_len != 64) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA pico rollback from %s: missing or malformed X-Ota-Mac header (len %u, want 64)",
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
        ESP_LOGW(OTA_HTTP_TAG, "OTA pico rollback from %s: X-Ota-Mac is not valid hex", ip);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Ota-Mac must be 64 hex characters");
        return ESP_OK;
    }

    // 2. Auth -- its own context (OTA_HTTP_CONTEXT_PICO_ROLLBACK), see this
    // handler's own doc comment above for why a rollback MAC is not
    // interchangeable with a plain-pico-update or an esp-rollback MAC.
    ota_http_verify_result_t vr = ota_http_verify_request(OTA_HTTP_CONTEXT_PICO_ROLLBACK, mac, ip);
    if (vr != OTA_HTTP_VERIFY_OK) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, verify_result_str(vr));
        return ESP_OK;
    }

    // 3. Interlocks -- identical gate to POST /api/ota/esp/rollback: a Pico
    // rollback is exactly as disruptive as pushing it a new image (kiln not
    // idle/cool, safety link down, another update in progress, ...), and
    // ota_http_check_interlocks() already refuses when the safety link
    // itself is down, which a rollback request obviously cannot survive
    // either.
    char reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req), reason,
                                                            sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA pico rollback from %s: refused by interlock: %s", ip, reason);
        return ota_http_send_interlock_refusal(req, gate, reason);
    }

    // 4. Single update mutex -- claimed as OTA_HTTP_CONTEXT_PICO (the same
    // slot a plain Pico update claims), see this handler's own doc comment
    // above for why this is deliberately not a distinct claim kind.
    if (!ota_http_update_try_begin(OTA_HTTP_CONTEXT_PICO)) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA pico rollback from %s: refused, an update is already in progress", ip);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "an update is already in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (!ota_http_safety) {
        ESP_LOGW(OTA_HTTP_TAG, "OTA pico rollback from %s: refused, no safety link configured this boot", ip);
        ota_http_update_end();
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "no safety link configured", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    {
        ota_record_t rec;
        // No trustworthy version-before string for the Pico from this side
        // -- same reasoning ota_pico_relay.c's own ota_record_fill() call
        // already documents (this ESP-side code never reads the Pico's own
        // running version back out). Left blank rather than guessed.
        ota_record_fill(&rec, (uint32_t)(hal_time_now_us() / 1000000), "pico", "", "", true,
                         "rollback requested", NULL);
        ota_record_append(&rec); // best-effort, logs its own failure -- see ota_record.h
    }

    ESP_LOGW(OTA_HTTP_TAG, "OTA pico rollback from %s: requesting the safety processor revert to its "
                  "previous bootloader slot (async -- see GET /api/ota/pico/rollback/status)", ip);

    // opus-review finding 3: hand the whole multi-second attempt off to its
    // own task (ota_pico_rollback_task() above) rather than blocking this
    // httpd worker for it -- see that task's own doc comment. Mark IN_
    // PROGRESS before starting the task so a status poll that lands before
    // the task's first scheduler slot still reports something better than a
    // stale prior DONE.
    if (xSemaphoreTake(ota_http_pico_rollback_async_lock, portMAX_DELAY) == pdTRUE) {
        ota_http_pico_rollback_async.state = OTA_PICO_ROLLBACK_ASYNC_IN_PROGRESS;
        xSemaphoreGive(ota_http_pico_rollback_async_lock);
    }

    static TaskHandle_t s_ota_pico_rollback_task; /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): stack_margin_register() target */
    if (xTaskCreate(ota_pico_rollback_task, "ota_pico_rollback", 4096, NULL, tskIDLE_PRIORITY + 1,
                     &s_ota_pico_rollback_task) !=
        pdPASS) {
        ESP_LOGE(OTA_HTTP_TAG, "OTA pico rollback from %s: failed to start the rollback task -- "
                      "the update claim was never released, this OTA layer is now wedged", ip);
        // Failed before the task could ever run, so nothing else will
        // release the claim this handler took at step 4 above -- release it
        // here, the same "whoever fails to hand off owns cleanup" rule
        // ota_pico_do_stage() follows for its own relay-task start failure.
        if (xSemaphoreTake(ota_http_pico_rollback_async_lock, portMAX_DELAY) == pdTRUE) {
            ota_http_pico_rollback_async.state = OTA_PICO_ROLLBACK_ASYNC_IDLE;
            xSemaphoreGive(ota_http_pico_rollback_async_lock);
        }
        ota_http_update_end();
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "failed to start the rollback task", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): registration only, no size change --
     * only reached with a real handle since the failure branch above already
     * returned. 4096 must match the xTaskCreate() literal above. */
    stack_margin_register("ota_pico_rollback", &s_ota_pico_rollback_task, 4096);

    // 202, not 200: the request has been accepted and IS being acted on,
    // but the outcome is not known yet -- the page is expected to poll GET
    // /api/ota/pico/rollback/status (ota_pico_rollback_status_get_handler()
    // below) for it, same shape /api/ota/pico/status already establishes
    // for the plain Pico update's phase/percent polling.
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    static const char pending_body[] =
        "{\"ok\":true,\"status\":\"pending\","
        "\"detail\":\"rollback request sent; poll /api/ota/pico/rollback/status for the outcome\"}";
    httpd_resp_send(req, pending_body, sizeof(pending_body) - 1);
    return ESP_OK;
}

// --- GET /api/ota/pico/rollback/status --------------------------------------
//
// Poll target for the async POST above (opus-review finding 3) -- reports
// whatever ota_pico_rollback_task() has (or has not yet) written into
// ota_http_pico_rollback_async. Always 200: unlike the old synchronous POST
// response, the HTTP status code here describes "did this GET succeed",
// not "what was the rollback outcome" -- that distinction now lives entirely
// in the JSON body's "status" field, same convention ota_pico_status_get_
// handler() already uses for the plain Pico update's phase field.
esp_err_t ota_pico_rollback_status_get_handler(httpd_req_t *req)
{
    ota_pico_rollback_async_state_t state = OTA_PICO_ROLLBACK_ASYNC_IDLE;
    safety_link_rollback_outcome_t outcome = SAFETY_LINK_ROLLBACK_OUTCOME_LINK_DOWN;
    uint8_t reason_code = KILNLINK_ROLLBACK_RESULT_REASON_UNKNOWN;
    if (xSemaphoreTake(ota_http_pico_rollback_async_lock, portMAX_DELAY) == pdTRUE) {
        state = ota_http_pico_rollback_async.state;
        outcome = ota_http_pico_rollback_async.outcome;
        reason_code = ota_http_pico_rollback_async.reason_code;
        xSemaphoreGive(ota_http_pico_rollback_async_lock);
    }

    char body[256];
    int n;
    switch (state) {
        case OTA_PICO_ROLLBACK_ASYNC_IDLE:
            n = snprintf(body, sizeof(body),
                         "{\"ok\":true,\"status\":\"idle\",\"detail\":\"no rollback requested this boot\"}");
            break;
        case OTA_PICO_ROLLBACK_ASYNC_IN_PROGRESS:
            n = snprintf(body, sizeof(body),
                         "{\"ok\":true,\"status\":\"pending\",\"detail\":\"rollback in progress\"}");
            break;
        case OTA_PICO_ROLLBACK_ASYNC_DONE:
        default:
            n = ota_http_pico_rollback_format_body(outcome, reason_code, body, sizeof(body));
            break;
    }
    httpd_resp_set_type(req, "application/json");
    ota_http_send_json_clamped(req, body, n, sizeof(body));
    return ESP_OK;
}
esp_err_t ota_pico_status_get_handler(httpd_req_t *req)
{
    ota_pico_relay_status_t st;
    ota_pico_relay_get_status(&st);

    // TODO.md 9.6: the Pico half of "running version, build commit, build
    // date, dirty flag, active slot, inactive slot" -- and here the honest
    // answer is that most of it does NOT exist over this link. Per
    // safety_link.h's own header comment (SAFETY_CMD_FW_VERSION) the Pico's
    // reply carries protocol/min_compatible/dirty/commit/datetime/boot_id,
    // but safety_apply_fw_version()/safety_parse_fw_version() (safety_link.c)
    // only extract protocol/min_compatible/boot_id -- dirty/commit/datetime
    // are parsed past (to find boot_id's offset) and then discarded, never
    // stored in safety_link_status_t. There is also no concept of an
    // "active/inactive slot" on the Pico side in this protocol at all (no
    // A/B image slots the way the ESP has). What IS actually available is
    // exposed here: the peer's protocol version, whether it's known/
    // compatible with this ESP's build, and its boot_id -- via
    // safety_link_get_peer_version_status(), the same accessor
    // dashboard_http.c's peer_protocol_version fields already use.
    bool version_known = false, version_compatible = false;
    uint16_t peer_protocol = 0, peer_min_compatible = 0;
    if (ota_http_safety) {
        (void)safety_link_get_peer_version_status(ota_http_safety, &version_known, &version_compatible,
                                                    &peer_protocol, &peer_min_compatible);
    }

    // last_error is always built by this codebase's own snprintf() calls
    // (ota_pico_relay.c's relay_set_error()/format_update_error()) -- never
    // copied verbatim from an external source -- so it cannot contain a
    // raw '"' or '\' that would need JSON escaping here.
    char body[384];
    int n;
    if (version_known) {
        n = snprintf(body, sizeof(body),
                      "{\"phase\":\"%s\",\"percent\":%u,\"last_error\":\"%s\","
                      "\"protocol_version_known\":true,\"protocol_version\":%u,"
                      "\"protocol_min_compatible\":%u,\"protocol_compatible\":%s}",
                      ota_pico_relay_phase_str(st.phase), (unsigned)st.percent, st.last_error,
                      (unsigned)peer_protocol, (unsigned)peer_min_compatible,
                      version_compatible ? "true" : "false");
    } else {
        n = snprintf(body, sizeof(body),
                      "{\"phase\":\"%s\",\"percent\":%u,\"last_error\":\"%s\","
                      "\"protocol_version_known\":false}",
                      ota_pico_relay_phase_str(st.phase), (unsigned)st.percent, st.last_error);
    }
    httpd_resp_set_type(req, "application/json");
    ota_http_send_json_clamped(req, body, n, sizeof(body));
    return ESP_OK;
}

