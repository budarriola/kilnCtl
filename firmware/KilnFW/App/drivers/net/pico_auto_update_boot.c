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
#include "pico_image_embedded.h"
#include "pico_image_source.h"
#include "pico_img_stage.h"
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

/* Acquire half of attempt_update_staged() (review finding F1): re-checks the
 * interlock snapshot (seconds old by the time a caller reaches this point,
 * and "no firing, no heat" is exactly the kind of fact that can change
 * underneath a stale read) and claims the SAME cross-processor update mutex
 * the HTTP paths claim. Split out so a caller that must stage bytes into
 * pico_img BEFORE it has anything to hand to the relay -- the embedded path,
 * whose staging write is itself only safe under this same mutex per
 * pico_img_stage.h's "not reentrant and not locked internally" contract --
 * can claim it first and release it on any subsequent failure, rather than
 * staging unlocked and claiming only once staging has already finished.
 * Returns false (nothing claimed) if the interlock or the mutex refuses. */
static bool attempt_update_acquire(void)
{
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
    return true;
}

/* Post-acquire half of the old attempt_update_staged(): the mutex from
 * attempt_update_acquire() is already held on entry. Counts the attempt
 * BEFORE handing off (an attempt that starts and then wedges must still
 * consume budget -- otherwise a board that hangs mid-relay retries forever,
 * which is exactly what the budget exists to prevent), and hands the mutex to
 * the relay task on success. Returns true if the relay took ownership.
 * Releases the mutex itself (ota_http_update_end()) on EVERY failure return
 * path -- the caller must not call it again.
 *
 * slot_tried: which embedded slot (0=A, 1=B) this attempt used, persisted
 * alongside the attempt count so the NEXT boot's pico_update_attempts_next_slot()
 * can alternate to the other one if this guess was wrong -- see that
 * function's header comment. The manifest (HTTP-staged) path is a single
 * image, not a two-slot pair, so it has no real slot to report; it passes 0
 * as a fixed placeholder and is never read by next_slot() for that pair since
 * a manifest-sourced pair_hash never round-trips through the embedded path. */
static bool attempt_update_staged_locked(const char *commit_for_log, uint32_t image_length,
                                         uint32_t image_crc32, uint32_t pair_hash, int slot_tried)
{
    uint32_t new_count = 0;
    if (!pico_update_attempts_record_attempt(pair_hash, slot_tried, &new_count)) {
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
    ESP_LOGW(TAG, "attempt %lu of %u: pushing staged SaftyFW %s slot %c (%lu bytes) to the safety "
                  "processor",
             (unsigned long)new_count, (unsigned)PICO_AUTO_UPDATE_ATTEMPT_BUDGET, commit_for_log,
             slot_tried == 0 ? 'A' : 'B', (unsigned long)image_length);

    /* No version16 and no SHA: the staged image's integrity was just
     * re-verified against its CRC-32 (either the manifest's, or the one
     * pico_img_stage_finish() just computed while writing it), and the relay
     * checks that same CRC end to end. Passing NULL for the optional SHA is
     * the documented shape (ota_pico_relay.h), used by every caller that does
     * not have a digest in hand. */
    if (!ota_pico_relay_start(s_link, image_length, image_crc32, NULL, NULL)) {
        /* ota_pico_relay_start() returning false means the relay task never
         * took ownership, so releasing the mutex is still OURS to do -- its
         * header states this contract explicitly.
         *
         * Review finding (Opus, 2026-09-20): this is an ESP-local failure
         * (task creation, typically) -- the relay never touched the Pico at
         * all, so it says nothing about the Pico or the staged image. It
         * used to call pico_update_attempts_record_failure() here, which
         * persists prior_attempt_failed for this pair_hash; decide() then
         * returns ABANDONED_PRIOR_FAILED on every future boot with no
         * escape (the only clear is a MATCH, which an abandoned pair can
         * never reach). A transient local hiccup must not latch a permanent
         * refusal. Do NOT record a terminal failure here -- the attempt
         * count above was already bumped, so the existing attempt budget
         * (pico_update_attempts_record_attempt(), just above) still bounds
         * retries across reboots without this. record_failure() is kept for
         * a genuine terminal relay OUTCOME (the relay took ownership, ran,
         * and reported back a non-retryable result) -- no such caller exists
         * yet; see pico_update_attempts.h's header comment on
         * pico_update_attempts_record_failure() for why it is kept anyway. */
        ESP_LOGW(TAG, "the relay task could not be started (local failure, Pico untouched) -- "
                      "standing down this boot, budget already counted above");
        ota_http_update_end();
        return false;
    }
    /* From here the relay task owns the mutex and the outcome. It reboots the
     * Pico into the new image on success; on failure the Pico stays on its
     * existing, armed image and the next boot re-evaluates with one less
     * attempt left.
     *
     * FUTURE HOOK: SaftyFW's update_task.c (as of 2026-09-20) now DOES emit a
     * "wrong slot" rejection, SAFETY_LINK_UPDATE_STATE_REJECTED_SLOT_LINKAGE
     * (safety_link.h), recognized by ota_pico_relay.c's relay_wait_for_states()
     * as a distinct terminal state instead of falling through to the generic
     * 10 s UPDATE_END timeout. What is still missing is on THIS side: nothing
     * here yet retries against the OTHER slot when that rejection comes back,
     * and it still should not consume another unit of pico_update_attempts'
     * budget when it does -- that rejection is proof the ESP's guess was
     * structurally wrong, not that the image or the link is bad, so it should
     * not cost the same as a real failure. ota_pico_relay.c's own header is
     * the place that retry hook belongs; it is not wired up yet. */
    return true;
}

/* Manifest (HTTP-staged) path: the image is already sitting in pico_img from
 * an earlier upload, described by pico_image_source_describe(). Not a
 * two-slot pair, so slot_tried is the fixed placeholder 0 -- see
 * attempt_update_staged_locked()'s header comment. Nothing is staged here (the
 * bytes were staged earlier, by the HTTP upload handler, itself already under
 * this same mutex per ota_http_pico.c), so acquire-then-stage ordering does
 * not apply -- only the relay hand-off needs the mutex. */
static bool attempt_update(const pico_image_source_info_t *img, uint32_t pair_hash)
{
    if (!attempt_update_acquire()) {
        return false;
    }
    return attempt_update_staged_locked(img->commit, img->image_length, img->image_crc32, pair_hash,
                                        0);
}

/* Embedded path: stages the chosen slot's bytes (already resident in this
 * app's own flash-mapped .rodata, per pico_image_embedded.h) into pico_img
 * via the SAME erase/write/manifest sequence ota_http_pico.c's browser-upload
 * handler uses (pico_img_stage.h), then hands off exactly like the manifest
 * path. Written directly from the rodata pointer in one call -- no chunking
 * loop and no stack buffer, since the whole image is already addressable
 * memory (unlike the HTTP path, which streams off a socket and must chunk).
 *
 * Review finding F1: pico_img_stage.h's own contract says it is "NOT
 * reentrant and not locked internally: exactly one context may be staging
 * into pico_img at a time," relying entirely on the cross-processor update
 * mutex every OTHER writer already claims first. This function used to stage
 * BEFORE claiming that mutex, so a concurrent HTTP upload to the Pico-OTA
 * route could interleave its own erase/write into the same partition. The
 * mutex is now claimed via attempt_update_acquire() before the first byte is
 * staged, and released on every staging-failure return path below -- nothing
 * between the acquire and the eventual attempt_update_staged_locked() call
 * may return without either releasing it or handing it to the relay. */
static bool attempt_update_embedded(const pico_image_embedded_info_t *emb, int slot,
                                    uint32_t pair_hash)
{
    if (!attempt_update_acquire()) {
        return false;
    }

    pico_img_stage_ctx_t ctx;
    char fail_reason[96];
    fail_reason[0] = '\0';
    if (!pico_img_stage_begin(&ctx, emb->slot_len[slot], fail_reason, sizeof(fail_reason), NULL)) {
        ESP_LOGE(TAG, "embedded slot %c could not be staged: %s", slot == 0 ? 'A' : 'B',
                 fail_reason);
        ota_http_update_end();
        return false;
    }
    if (!pico_img_stage_write_chunk(&ctx, emb->slot_data[slot], emb->slot_len[slot], fail_reason,
                                    sizeof(fail_reason))) {
        ESP_LOGE(TAG, "embedded slot %c could not be staged: %s", slot == 0 ? 'A' : 'B',
                 fail_reason);
        ota_http_update_end();
        return false;
    }
    uint32_t crc = 0;
    if (!pico_img_stage_finish(&ctx, &crc)) {
        /* Non-fatal per pico_img_stage.h's contract: the bytes ARE staged and
         * usable this boot, the manifest just won't survive to describe them
         * to a later boot. Proceed -- the mutex stays held into the relay
         * hand-off below, same as every other non-fatal path here. */
        ESP_LOGW(TAG, "embedded slot %c staged but its manifest could not be persisted -- usable "
                      "this boot only",
                 slot == 0 ? 'A' : 'B');
    }
    return attempt_update_staged_locked(emb->commit, (uint32_t)emb->slot_len[slot], crc, pair_hash,
                                        slot);
}

/* Bug found 2026-09-21 (bench triage): docs/PICO_AUTO_UPDATE_PLAN.md sec 11's
 * source-only review found this board's SaftyFW has no confirmed two-slot
 * bootloader today (a flat image at 0x10000000, no seeded metadata sector) --
 * the P1-closed claim earlier in that plan does not hold for this board.
 * Neither kilnlink_fw_version_t nor any other wire message the Pico sends
 * today carries "I have an update-capable bootloader installed" as a fact;
 * the only way the ESP finds out is by trying a relay and watching it fail
 * (an erase-phase watchdog reset, per that section), which is exactly the
 * failure mode this gate exists to avoid provoking automatically at every
 * boot. Until a wire-level signal exists (a follow-up, not built here),
 * automatic Pico updates are gated off by this build-time default -- set
 * to 1 only once a board's bootloader install is confirmed and section 11's
 * three NO-GO items are resolved. This does not touch pico_auto_update.h's
 * decide() shape or its host tests: the gate short-circuits before decide()
 * is ever called, the same way the "no image at all" inert path already
 * does below. */
#ifndef PICO_AUTO_UPDATE_BOOTLOADER_PRESENT
#define PICO_AUTO_UPDATE_BOOTLOADER_PRESENT 0
#endif

static void pico_auto_update_task(void *arg)
{
    (void)arg;

#if !PICO_AUTO_UPDATE_BOOTLOADER_PRESENT
    ESP_LOGW(TAG, "automatic Pico update is compiled OFF: no wire-level signal exists yet to "
                  "confirm this board's SaftyFW has an update-capable two-slot bootloader "
                  "installed (docs/PICO_AUTO_UPDATE_PLAN.md sec 11, NO-GO as of 2026-09-21) -- "
                  "set -DPICO_AUTO_UPDATE_BOOTLOADER_PRESENT=1 once that is confirmed");
    pico_auto_update_state_set_blocking(false, NULL);
    pico_auto_update_state_set_warning(NULL);
    pico_auto_update_state_set_last_decision(
        "deliberately_off: safety processor has no confirmed update-capable bootloader", false);
    goto done;
#endif

    /* Owner decision 2026-09-20: the embedded pair (baked into THIS build, see
     * pico_image_embedded.h) is the primary image source -- unlike the
     * manifest path below it is unconditionally present, so it is tried
     * first. The manifest/HTTP-staged path (pico_image_source.c) remains a
     * fallback for the bench upload tool and for any board whose embedded
     * pair turned out unusable (a build-system mismatch between the two
     * slots -- see pico_image_embedded.h's "fleet-wide-refusal trap" note). */
    pico_image_embedded_info_t emb;
    (void)pico_image_embedded_describe(&emb);
    int use_slot = 0;
    pico_image_source_info_t img;
    bool have_manifest = false;
    bool use_embedded = pico_image_embedded_should_use(&emb);

    if (emb.usable && emb.dirty) {
        /* Review finding D4: pico_auto_update_identity_matches()
         * (pico_image_embedded.h) requires the Pico's OWN dirty flag to read
         * 0 before it will call an already-running image a match for the
         * embedded pair. A dirty embedded image can therefore never be
         * confirmed as matching even after a fully successful push, which
         * burns the 3-attempt budget every boot and latches
         * pico_auto_update_state_set_blocking(true) -- silently blocking
         * every future firing via readiness_gate. Mirror the "no image
         * staged either" branch below: log it and go inert, same as
         * ABANDONED_NO_IMAGE, rather than attempting an update that can
         * never be confirmed. */
        ESP_LOGW(TAG, "embedded SaftyFW image is marked dirty -- automatic Pico update is inert "
                      "this boot (a dirty image can never be confirmed as matching, so attempting "
                      "it would only burn the attempt budget and block readiness)");
    }

    if (!use_embedded) {
        have_manifest = pico_image_source_describe(&img);
        if (!have_manifest) {
            /* No embedded image usable AND no image has ever been staged on
             * this board via the manifest path either. Nothing is expected,
             * so nothing is decided and nothing is blocked -- see
             * pico_image_source.h's "feature-inert" note for why this is NOT
             * ABANDONED_NO_IMAGE. This should not happen once embedding is
             * routinely built (emb.usable should be true on every normal
             * build), so it is logged at WARN rather than INFO to make a
             * silently-broken embed step visible. */
            ESP_LOGW(TAG, "no usable SaftyFW image (embedded: %s; no manifest staged either) -- "
                          "automatic Pico update is inert this boot",
                     emb.reason[0] != '\0' ? emb.reason : "none embedded");
            pico_auto_update_state_set_last_decision(
                "no usable SaftyFW image is available to compare against -- nothing decided", false);
            goto done;
        }
    }

    pico_auto_update_inputs_t in;
    memset(&in, 0, sizeof(in));
    if (use_embedded) {
        in.expected_commit = emb.commit;
        in.image_available = true;
    } else {
        in.expected_commit = img.usable ? img.commit : "";
        in.image_available = img.usable;
    }

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
    if (use_embedded) {
        /* Alternates A/B per pair, per pico_update_attempts_next_slot()'s own
         * header comment; a fresh pair defaults to slot 0/A. */
        (void)pico_update_attempts_next_slot(pair_hash, &use_slot);
    }

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
        pico_auto_update_state_set_warning(NULL);
        pico_auto_update_state_set_last_decision(why, true);
        break;

    case PICO_AUTO_UPDATE_LINK_DOWN:
        ESP_LOGW(TAG, "%s", why);
        pico_auto_update_state_set_blocking(false, NULL);
        pico_auto_update_state_set_warning(NULL);
        pico_auto_update_state_set_last_decision(why, false);
        break;

    case PICO_AUTO_UPDATE_DEFER_FIRING:
        ESP_LOGW(TAG, "%s (%s)", why, gate_reason[0] != '\0' ? gate_reason : "interlock");
        pico_auto_update_state_set_blocking(false, NULL);
        pico_auto_update_state_set_warning(NULL);
        pico_auto_update_state_set_last_decision(why, false);
        break;

    case PICO_AUTO_UPDATE_NEEDED: {
        ESP_LOGW(TAG, "%s", why);
        bool started = use_embedded ? attempt_update_embedded(&emb, use_slot, pair_hash)
                                    : attempt_update(&img, pair_hash);
        if (!started) {
            /* Standing down is not abandonment: the budget is intact unless
             * the attempt actually began, and the next boot re-evaluates. */
            pico_auto_update_state_set_blocking(false, NULL);
            pico_auto_update_state_set_warning(NULL);
            pico_auto_update_state_set_last_decision(
                "an update attempt stood down before it could start (see log) -- not a match", false);
        } else {
            pico_auto_update_state_set_last_decision(
                "a Pico update is in progress this boot -- not yet confirmed matching", false);
        }
        break;
    }

    case PICO_AUTO_UPDATE_ABANDONED_BUDGET_SPENT: {
        /* Review finding F2, owner decision (option c), 2026-09-20: budget-
         * spent must NOT block firing. Unlike ABANDONED_NO_IMAGE/CHAIN_GAP/
         * PRIOR_FAILED, this cause can be produced entirely by uncooperative
         * hardware around an otherwise-good image (a bad cable, a wedged
         * Pico bootloader) -- latching a permanent, un-escapable firing
         * refusal for that is worse than the status quo before this feature
         * existed. Logged at WARN, not ERROR, and surfaced as a non-blocking
         * /readiness warning (pico_auto_update_state_set_warning()) naming
         * the attempt count and the embedded/expected commit, rather than
         * pico_auto_update_state_set_blocking(true). The board keeps firing
         * on whatever SaftyFW it already has. */
        char warn_reason[PICO_AUTO_UPDATE_STATE_REASON_MAX];
        snprintf(warn_reason, sizeof(warn_reason),
                "Pico update budget spent (%lu/%u attempts) against %.24s",
                (unsigned long)in.attempt_count, (unsigned)PICO_AUTO_UPDATE_ATTEMPT_BUDGET,
                in.expected_commit != NULL ? in.expected_commit : "");
        ESP_LOGW(TAG, "%s -- %s", why, warn_reason);
        pico_auto_update_state_set_blocking(false, NULL);
        pico_auto_update_state_set_warning(warn_reason);
        pico_auto_update_state_set_last_decision(why, false);
        break;
    }

    case PICO_AUTO_UPDATE_ABANDONED_PRIOR_FAILED: {
        /* Review finding (Opus, 2026-09-20), fix part (a): this used to fall
         * into the default: branch and set_blocking(true) -- a PERMANENT
         * firing refusal with no operator escape, since the only clear is a
         * verified MATCH (pico_update_attempts_clear() above), and an
         * abandoned pair never attempts again to reach one. That was made
         * worse by fix part (b) above still leaving a stale
         * prior_attempt_failed=true possibly already persisted on a board
         * flashed with 67ae5a08-era firmware (the record_failure() call this
         * commit removes from attempt_update_staged_locked()) -- even a
         * clean rebuild of THIS fix cannot un-persist that old NVS record by
         * itself. Treat it the same as BUDGET_SPENT: non-blocking, surfaced
         * as a /readiness warning naming the pair, so a stale flag can never
         * latch a permanent refusal either. The attempt budget above still
         * bounds any real retry storm independently of this flag. */
        char warn_reason[PICO_AUTO_UPDATE_STATE_REASON_MAX];
        snprintf(warn_reason, sizeof(warn_reason),
                "Pico update: prior attempt for this pair reported a terminal failure "
                "(against %.24s)",
                in.expected_commit != NULL ? in.expected_commit : "");
        ESP_LOGW(TAG, "%s -- %s", why, warn_reason);
        pico_auto_update_state_set_blocking(false, NULL);
        pico_auto_update_state_set_warning(warn_reason);
        pico_auto_update_state_set_last_decision(why, false);
        break;
    }

    default: /* the remaining ABANDONED_* causes: NO_IMAGE, CHAIN_GAP */
        ESP_LOGE(TAG, "automatic Pico update abandoned: %s%s%s", why,
                 (use_embedded ? emb.reason[0] : img.reason[0]) != '\0' ? " -- " : "",
                 use_embedded ? emb.reason : img.reason);
        pico_auto_update_state_set_blocking(true, why);
        pico_auto_update_state_set_last_decision(why, false);
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
