#include "pico_auto_update_boot.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "ota_http.h" /* ota_http_update_try_begin() -- the single cross-processor update mutex.
                       * ota_state.h beside it declares only the _end() half. */
#include "ota_interlock.h"
#include "ota_pico_relay.h"
#include "ota_state.h"
#include "pico_auto_update.h"
#include "pico_auto_update_state.h"
#include "pico_image_source.h"
#include "pico_update_attempts.h"
#include "stack_margin.h"

static const char *TAG = "pico_auto_update";

/* 4096 is the same size ota_pico_rollback (ota_http_pico.c) runs in, and this
 * task does strictly less: no httpd request, one static-buffered flash scan
 * (the scan buffer lives in pico_image_source.c precisely so it is NOT on
 * this stack), and one decision. Must match the stack_margin_register()
 * literal below. */
#define PICO_AUTO_UPDATE_TASK_STACK 4096
/* Below every control/safety task. Nothing waits on this task's answer, and
 * it must never delay safety_poll or the link. */
#define PICO_AUTO_UPDATE_TASK_PRIORITY 3

/* The Pico sends an unsolicited FW_VERSION burst at its own boot, and the
 * ESP's poll loop re-requests one every poll period for as long as it has
 * none -- so this is a bounded wait for something already being asked for
 * repeatedly, not a poll that drives the request. 20 s covers an RP2040 that
 * booted well after the ESP (a dual reflash, say). Timing out is not a fault
 * and not a block: it resolves to LINK_DOWN, which does nothing. */
#define FW_VERSION_WAIT_MS 20000
#define FW_VERSION_POLL_MS 250

static SafetyLinkClass *s_link;
static TaskHandle_t s_task; /* stack_margin_register() target; also the "already running" guard */

/* Waits for the Pico's reported build identity. Returns true and fills the
 * caller's buffers only once safety_link_get_peer_build_status() reports
 * known. commit_buf is NOT NUL-terminated by that call -- *out_commit_len is
 * authoritative, exactly as pico_auto_update_inputs_t expects. */
static bool wait_for_peer_identity(uint8_t *commit_buf, uint8_t *out_commit_len, bool *out_dirty)
{
    const int attempts = FW_VERSION_WAIT_MS / FW_VERSION_POLL_MS;
    for (int i = 0; i < attempts; i++) {
        bool known = false;
        bool dirty = false;
        uint8_t commit_len = 0;
        uint8_t datetime[64];
        uint8_t datetime_len = 0;
        uint8_t config_version = 0;
        uint16_t config_crc = 0;
        esp_err_t err = safety_link_get_peer_build_status(s_link, &known, &dirty, commit_buf,
                                                          &commit_len, datetime, &datetime_len,
                                                          &config_version, &config_crc);
        if (err == ESP_OK && known && commit_len > 0) {
            *out_commit_len = commit_len;
            *out_dirty = dirty;
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(FW_VERSION_POLL_MS));
    }
    return false;
}

/* One attempt. Claims the SAME cross-processor update mutex the HTTP paths
 * claim, counts the attempt BEFORE handing off (an attempt that starts and
 * then wedges must still consume budget -- otherwise a board that hangs
 * mid-relay retries forever, which is exactly what the budget exists to
 * prevent), and hands the mutex to the relay task on success. Returns true
 * if the relay took ownership. */
static bool attempt_update(const pico_image_source_info_t *img, uint32_t pair_hash)
{
    /* Re-check, immediately before claiming: the interlock snapshot taken at
     * decision time is seconds old by now, and "no firing, no heat" is
     * exactly the kind of fact that can change underneath a stale read. */
    char reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(false, reason, sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "update attempt stood down at the last moment: %s", reason);
        return false;
    }

    if (!ota_http_update_try_begin(OTA_HTTP_CONTEXT_PICO)) {
        ESP_LOGW(TAG, "update attempt stood down: an update is already in progress");
        return false;
    }

    uint32_t new_count = 0;
    if (!pico_update_attempts_record_attempt(pair_hash, &new_count)) {
        /* The counter did NOT verify (pico_update_attempts.c already logged
         * why). Proceeding anyway would mean an unbounded retry loop across
         * reboots -- the single failure mode plan sec 7 exists to prevent --
         * so the attempt is abandoned instead. The Pico keeps running its
         * existing image; nothing has been touched. */
        ESP_LOGE(TAG, "attempt %lu could not be persisted -- standing down rather than risk an "
                      "unbounded retry loop across reboots",
                 (unsigned long)new_count);
        ota_http_update_end();
        return false;
    }
    ESP_LOGW(TAG, "attempt %lu of %u: pushing staged SaftyFW %s (%lu bytes) to the safety processor",
             (unsigned long)new_count, (unsigned)PICO_AUTO_UPDATE_ATTEMPT_BUDGET, img->commit,
             (unsigned long)img->image_length);

    /* No version16 and no SHA: the staged image's integrity was just
     * re-verified against the manifest CRC-32 by pico_image_source_describe(),
     * and the relay checks that same CRC end to end. Passing NULL for the
     * optional SHA is the documented shape (ota_pico_relay.h), used by every
     * caller that does not have a digest in hand. */
    if (!ota_pico_relay_start(s_link, img->image_length, img->image_crc32, NULL, NULL)) {
        /* ota_pico_relay_start() returning false means the relay task never
         * took ownership, so releasing the mutex is still OURS to do -- its
         * header states this contract explicitly. */
        ESP_LOGE(TAG, "the relay task could not be started -- update abandoned");
        ota_http_update_end();
        (void)pico_update_attempts_record_failure(pair_hash);
        return false;
    }
    /* From here the relay task owns the mutex and the outcome. It reboots the
     * Pico into the new image on success; on failure the Pico stays on its
     * existing, armed image and the next boot re-evaluates with one less
     * attempt left. */
    return true;
}

static void pico_auto_update_task(void *arg)
{
    (void)arg;

    pico_image_source_info_t img;
    if (!pico_image_source_describe(&img)) {
        /* No image has ever been staged on this board. Nothing is expected,
         * so nothing is decided and nothing is blocked -- see the header's
         * "feature-inert" note for why this is NOT ABANDONED_NO_IMAGE. */
        ESP_LOGI(TAG, "no SaftyFW image has been staged on this board -- automatic Pico update is "
                      "inert this boot (POST /api/ota/pico to stage one)");
        goto done;
    }

    pico_auto_update_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.expected_commit = img.usable ? img.commit : "";
    in.image_available = img.usable;

    uint8_t commit_buf[PICO_AUTO_UPDATE_MAX_COMMIT_LEN];
    memset(commit_buf, 0, sizeof(commit_buf));
    uint8_t commit_len = 0;
    bool dirty = false;
    if (wait_for_peer_identity(commit_buf, &commit_len, &dirty)) {
        in.fw_version_known = true;
        if (commit_len > (uint8_t)sizeof(in.observed_commit)) {
            commit_len = (uint8_t)sizeof(in.observed_commit);
        }
        memcpy(in.observed_commit, commit_buf, commit_len);
        in.observed_commit_len = commit_len;
        in.observed_dirty = dirty;
    }

    /* FIRING/HEAT: taken from ota_http_check_interlocks(), the existing
     * seven-check update interlock, rather than from a second rule of this
     * module's own -- the standing instruction for this path. Every refusal
     * it can return (a firing running/paused/resumable, heat granted, a hot
     * kiln, an update already in progress, ...) is a reason to DEFER, never
     * to abandon: decide() checks firing_active before any unrecoverable
     * cause precisely so a busy board cannot burn its budget. */
    char gate_reason[OTA_INTERLOCK_REASON_MAX];
    gate_reason[0] = '\0';
    ota_interlock_result_t gate = ota_http_check_interlocks(false, gate_reason, sizeof(gate_reason));
    in.firing_active = (gate != OTA_INTERLOCK_OK);

    /* CONFIG CHAIN GAP (plan sec 6/8): false today, and that is a statement
     * of fact rather than a stub. The gap the plan describes would exist if a
     * staged image's CONFIG_STORE_FORMAT_VERSION were more than one step
     * ahead of what the Pico is holding, so a migration step is missing. The
     * only format versions that exist are 1 and 2 (config_store.h), the Pico
     * reports a config RECORD version over the link and not a FORMAT version,
     * and so no gap is currently expressible or detectable. The image's own
     * declared config_format_version is carried through
     * saftyfw_image_identity_t for exactly this check, so when a v3 lands
     * this is the one line that needs the comparison -- and the plan's sec 6
     * work (the ct-normals carry) is what makes that comparison meaningful. */
    in.config_chain_gap = false;

    uint32_t pair_hash = pico_update_attempts_pair_hash(in.expected_commit, in.observed_commit,
                                                         in.observed_commit_len);
    (void)pico_update_attempts_load(pair_hash, &in.attempt_count, &in.prior_attempt_failed);

    const char *why = NULL;
    pico_auto_update_decision_t d = pico_auto_update_decide(&in, &why);

    switch (d) {
    case PICO_AUTO_UPDATE_MATCH:
        ESP_LOGI(TAG, "%s", why);
        /* Plan sec 7: "the counter clears only on a verified success." This
         * IS that verified success -- the Pico is reporting the expected
         * identity, from its own running image. */
        if (!pico_update_attempts_clear()) {
            ESP_LOGW(TAG, "identity matches but the attempt counter could not be cleared -- a "
                          "future mismatch may start with a partly-spent budget");
        }
        pico_auto_update_state_set_blocking(false, NULL);
        break;

    case PICO_AUTO_UPDATE_LINK_DOWN:
        ESP_LOGW(TAG, "%s", why);
        pico_auto_update_state_set_blocking(false, NULL);
        break;

    case PICO_AUTO_UPDATE_DEFER_FIRING:
        ESP_LOGW(TAG, "%s (%s)", why, gate_reason[0] != '\0' ? gate_reason : "interlock");
        pico_auto_update_state_set_blocking(false, NULL);
        break;

    case PICO_AUTO_UPDATE_NEEDED:
        ESP_LOGW(TAG, "%s", why);
        if (!attempt_update(&img, pair_hash)) {
            /* Standing down is not abandonment: the budget is intact unless
             * the attempt actually began, and the next boot re-evaluates. */
            pico_auto_update_state_set_blocking(false, NULL);
        }
        break;

    default: /* every ABANDONED_* */
        ESP_LOGE(TAG, "automatic Pico update abandoned: %s%s%s", why,
                 img.reason[0] != '\0' ? " -- " : "", img.reason);
        pico_auto_update_state_set_blocking(true, why);
        break;
    }

done:
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t pico_auto_update_boot_start(SafetyLinkClass *link)
{
    if (link == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_link = link;
    if (xTaskCreate(pico_auto_update_task, "pico_auto_update", PICO_AUTO_UPDATE_TASK_STACK, NULL,
                    PICO_AUTO_UPDATE_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* 4096 must match the PICO_AUTO_UPDATE_TASK_STACK literal above. Only
     * reached with a real handle -- the failure branch already returned. */
    stack_margin_register("pico_auto_update", &s_task, PICO_AUTO_UPDATE_TASK_STACK);
    return ESP_OK;
}
