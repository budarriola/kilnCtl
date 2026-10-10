#include "kiln_cfg_swap.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_crc.h"
#include "esp_heap_caps.h" /* heap_caps_malloc()/_free(), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT --
                            * docs/audits/kiln_cfg_swap_stack_overflow_2026-09-22.md's scratch
                            * structs, kept off both the stack and .bss */
#include "esp_log.h"
#include "hal_kv.h"

#include "kiln_cfg_store.h"
#include "kiln_package.h"
#include "persist_scratch.h" /* persist_scratch_alloc() -- save_pending()'s read-back buffer */
#include "ota_state.h" /* ota_http_check_interlocks()/OTA_INTERLOCK_OK -- same predicate
                        * kiln_cfg_store_apply() itself is built on, section 4.3 */
#include "safety_cfg_write.h" /* safety_cfg_write_apply_package_and_confirm()/_set_and_confirm_f32() */
#include "safety_cfg_store.h" /* safety_cfg_store_refetch()/_get_by_index()/_param_count()/_cached_crc() */
#include "safety_ceiling_sync.h" /* safety_ceiling_sync_reconcile_on_link_up()/_is_diverged()/
                                  * SAFETY_PARAM_ID_ABS_MAX_TEMP_C */
#include "zones_config_accessors.h" /* zones_config_export_blob()/_import_blob() */

static const char *TAG = "kiln_cfg_swap";

static SafetyLinkClass *s_link = NULL;

/* LOW-2: the target id of an ESP_DONE record that stays open only because
 * the active-id save failed after both sides were confirmed on the target.
 * KILN_CFG_NO_ACTIVE_ID otherwise. Set at the very end of
 * kiln_cfg_swap_apply_impl() / finish_esp_done_impl(), reset when an apply
 * starts and whenever the record is cleared. See kiln_cfg_swap_is_pending(). */
static volatile int32_t s_id_unsaved_target = KILN_CFG_NO_ACTIVE_ID;

/* M13 fix -- see kiln_cfg_swap_boot_fault_t's own doc comment
 * (kiln_cfg_swap.h) for scope and rationale. Latched once per boot, never
 * cleared mid-boot; only kiln_cfg_swap_boot_recover() writes it. */
static kiln_cfg_swap_boot_fault_t s_boot_fault = {0};

static void latch_boot_fault(kiln_cfg_swap_boot_fault_kind_t kind, int32_t target_id, const char *reason)
{
    if (s_boot_fault.occurred) {
        return; /* first one latched wins, same convention as zones_cfg_load_fault_t */
    }
    s_boot_fault.occurred = true;
    s_boot_fault.kind = kind;
    s_boot_fault.target_id = target_id;
    snprintf(s_boot_fault.reason, sizeof(s_boot_fault.reason), "%s", reason ? reason : "");
}

bool kiln_cfg_swap_get_boot_fault(kiln_cfg_swap_boot_fault_t *out)
{
    if (out) {
        *out = s_boot_fault;
    }
    return s_boot_fault.occurred;
}

kiln_cfg_swap_boot_fault_kind_t kiln_cfg_swap_get_boot_fault_kind(void)
{
    return s_boot_fault.occurred ? s_boot_fault.kind : KILN_CFG_SWAP_BOOT_FAULT_NONE;
}

const char *kiln_cfg_swap_boot_fault_kind_name(kiln_cfg_swap_boot_fault_kind_t kind)
{
    switch (kind) {
    case KILN_CFG_SWAP_BOOT_FAULT_NONE:
        return "none";
    case KILN_CFG_SWAP_BOOT_FAULT_UNREADABLE:
        return "unreadable";
    case KILN_CFG_SWAP_BOOT_FAULT_NO_LINK:
        return "no_link";
    case KILN_CFG_SWAP_BOOT_FAULT_ROLLBACK_FAILED:
        return "rollback_failed";
    case KILN_CFG_SWAP_BOOT_FAULT_ESP_DONE_UNCONFIRMED:
        return "esp_done_unconfirmed";
    case KILN_CFG_SWAP_BOOT_FAULT_UNRECOGNISED_MARKER:
        return "unrecognised_marker";
    case KILN_CFG_SWAP_BOOT_FAULT_ACTIVE_ID_UNSAVED:
        return "active_id_unsaved";
    default:
        return "unknown";
    }
}

void kiln_cfg_swap_set_link(SafetyLinkClass *link_or_null)
{
    s_link = link_or_null;
}

/* Same partition/namespace kiln_cfg_store.c uses -- a SEPARATE key within
 * it, never the store's own blob key (this record's corruption must never
 * be able to corrupt, or be confused with, the slot store itself). */
#define KILN_NVS_PARTITION_SWAP "kiln_nvs"
#define NVS_NAMESPACE_SWAP "kiln_cfg"
#define NVS_KEY_SWAP_PENDING "swap_pend"

/* LOW-4: what rollback() writes to its reason_out when both sides are back
 * on R but the journal clear failed. */
#define KILN_CFG_SWAP_ROLLBACK_UNCLEARED_NOTE "swap journal not cleared after rollback; retried at next boot"

static bool set_reason(char *reason_out, size_t reason_cap, const char *msg)
{
    if (reason_out && reason_cap) {
        strncpy(reason_out, msg, reason_cap - 1);
        reason_out[reason_cap - 1] = '\0';
    }
    return false;
}

static uint32_t pending_crc(const kiln_cfg_swap_pending_t *p)
{
    /* Over every field EXCEPT crc32 itself -- same "zero the CRC field,
     * hash the rest" convention zones_config_migrate.c/crash_report.c
     * already use (see this file's own #include comment). Whole-struct
     * hash, not a hand-picked field list: a corrupt byte ANYWHERE in the
     * record (including inside rollback_blob/rollback_pico) must be
     * detected, since a torn rollback package is exactly the thing this
     * check exists to catch (H10). */
    const uint8_t *base = (const uint8_t *)p;
    size_t off = offsetof(kiln_cfg_swap_pending_t, marker);
    size_t len = sizeof(*p) - off;
    return esp_crc32_le(0, base + off, (uint32_t)len);
}

/* `out_existed_but_unreadable` (may be NULL) distinguishes, on a false
 * return, "genuinely never staged" (HAL_NOT_FOUND -- the ordinary case on
 * every board that has never attempted a swap) from "a record IS present
 * and could not be read back" (HAL_IO, a short read, or hal_kv_open itself
 * failing on an otherwise-initialized partition) -- H10's own distinction.
 * The latter must NEVER be treated identically to "never staged" by a
 * caller deciding whether to alarm: an operator's evidence of an
 * interrupted swap must not be silently reinterpreted as "nothing
 * happened" just because it could not be read. */
static bool load_pending_ex(kiln_cfg_swap_pending_t *out, bool *out_existed_but_unreadable)
{
    memset(out, 0, sizeof(*out));
    if (out_existed_but_unreadable) {
        *out_existed_but_unreadable = false;
    }
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE_SWAP, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION_SWAP) != HAL_OK) {
        return false; /* partition/namespace itself not usable -- nothing was ever staged */
    }
    size_t len = sizeof(*out);
    hal_status_t err = hal_kv_get_blob(&h, NVS_KEY_SWAP_PENDING, out, &len);
    hal_kv_close(&h);
    if (err == HAL_NOT_FOUND) {
        memset(out, 0, sizeof(*out));
        return false; /* the ordinary "never attempted a swap" case */
    }
    if (err != HAL_OK || len != sizeof(*out)) {
        memset(out, 0, sizeof(*out));
        if (out_existed_but_unreadable) {
            *out_existed_but_unreadable = true;
        }
        return false; /* H10: present, but unreadable/wrong size -- NOT the same as absent */
    }
    return true;
}

static bool load_pending(kiln_cfg_swap_pending_t *out)
{
    return load_pending_ex(out, NULL);
}

static bool save_pending(const kiln_cfg_swap_pending_t *p)
{
    if (!hal_kv_write_safe_here()) {
        /* Same PSRAM-stack guard nvs_save_store() enforces (kiln_cfg_
         * store.c's caller_stack_is_external()) -- a flash write from a
         * PSRAM-backed stack aborts the whole board. kiln_cfg_swap_apply()
         * is documented to run on a dedicated internal-stack worker task,
         * never the httpd worker or a PSRAM-stacked task; this is the
         * defensive backstop if that is ever violated. */
        ESP_LOGE(TAG, "save_pending: REFUSING -- calling task's stack is external RAM (PSRAM)");
        return false;
    }
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE_SWAP, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION_SWAP) != HAL_OK) {
        return false;
    }
    hal_status_t err = hal_kv_set_blob(&h, NVS_KEY_SWAP_PENDING, p, sizeof(*p));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err != HAL_OK) {
        return false;
    }
    /* L6 (docs/audits/UNCHECKED_PERSIST_RESULT_AUDIT_2026-10-09.md): never
     * trust the write's return code alone -- boot_guard's 2026-09-08 failure
     * was an NVS write reporting HAL_OK while the value never landed. Read
     * the record back and compare every byte, the same verified-write
     * discipline as boot_guard's verify_persisted_count(). A record boot
     * recovery cannot rely on is reported as a failed save. Heap scratch,
     * not stack: the record is about 2 kB and the swap worker's stack
     * budget is measured. */
    kiln_cfg_swap_pending_t *rb = persist_scratch_alloc(sizeof(*rb));
    if (!rb) {
        ESP_LOGE(TAG, "save_pending: no memory for the read-back check -- treating the save as failed");
        return false;
    }
    bool verified = load_pending(rb) && memcmp(rb, p, sizeof(*p)) == 0;
    free(rb);
    if (!verified) {
        ESP_LOGE(TAG, "save_pending: write reported success but the record did not read back as written");
    }
    return verified;
}

static bool clear_pending(void)
{
    kiln_cfg_swap_pending_t none;
    memset(&none, 0, sizeof(none));
    none.marker = KILN_CFG_SWAP_MARKER_NONE;
    none.target_id = KILN_CFG_NO_ACTIVE_ID;
    none.previous_active_id = KILN_CFG_NO_ACTIVE_ID;
    none.crc32 = pending_crc(&none);
    if (!save_pending(&none)) {
        return false;
    }
    s_id_unsaved_target = KILN_CFG_NO_ACTIVE_ID;
    return true;
}

static bool persist_marker(kiln_cfg_swap_pending_t *p, kiln_cfg_swap_marker_t marker)
{
    p->marker = (uint8_t)marker;
    p->crc32 = pending_crc(p);
    if (!save_pending(p)) {
        ESP_LOGE(TAG, "failed to persist swap marker %d -- a crash from here would be recovered "
                      "as if the record were stuck at its LAST successfully persisted marker",
                 (int)marker);
        return false;
    }
    return true;
}

kiln_cfg_swap_marker_t kiln_cfg_swap_get_marker(int32_t *out_target_id, int32_t *out_previous_active_id)
{
    kiln_cfg_swap_pending_t p;
    if (out_target_id) {
        *out_target_id = KILN_CFG_NO_ACTIVE_ID;
    }
    if (out_previous_active_id) {
        *out_previous_active_id = KILN_CFG_NO_ACTIVE_ID;
    }
    if (!load_pending(&p)) {
        return KILN_CFG_SWAP_MARKER_NONE;
    }
    if (pending_crc(&p) != p.crc32) {
        return KILN_CFG_SWAP_MARKER_NONE; /* corrupt -- see kiln_cfg_swap_boot_recover()'s own
                                           * handling; this read-only accessor deliberately
                                           * does not distinguish this from "absent" for
                                           * callers that only want a quick status line. */
    }
    if (out_target_id) {
        *out_target_id = p.target_id;
    }
    if (out_previous_active_id) {
        *out_previous_active_id = p.previous_active_id;
    }
    return (kiln_cfg_swap_marker_t)p.marker;
}

/* 2026-09-15 review (review_autosave_rework_5bc9afb5_2026-09-15.md, MEDIUM):
 * registered with kiln_cfg_store.c via kiln_cfg_store_set_swap_pending_
 * source() so kiln_cfg_store_autosave_from_live() can suppress a recapture
 * for the WHOLE duration of a transaction, not only once its own divergence
 * check has actually run -- see that seam's doc comment (kiln_cfg_store.h)
 * for the exact race. Plain wrapper over the same marker this file's own
 * boot-recovery and status-reporting callers already read. */
bool kiln_cfg_swap_is_pending(void)
{
    int32_t target_id = KILN_CFG_NO_ACTIVE_ID;
    kiln_cfg_swap_marker_t marker = kiln_cfg_swap_get_marker(&target_id, NULL);
    if (marker == KILN_CFG_SWAP_MARKER_NONE) {
        return false;
    }
    /* LOW-2: an ESP_DONE record kept open ONLY because the active-id save
     * failed (s_id_unsaved_target, set at the very end of a finished apply
     * or boot finish, never while one is in flight) does not block autosave
     * when RAM active_id already names that target. Autosave then writes
     * into the target's own slot -- the kiln that is genuinely live -- so a
     * zone edit after such an apply keeps the slot equal to the live config,
     * and the next boot's finish_esp_done_impl() finds them matching instead
     * of latching ESP_DONE_UNCONFIRMED. Every other pending state still
     * blocks, including ESP_DONE during an apply's own steps 10-13 (the race
     * kiln_cfg_store_set_swap_pending_source()'s doc comment describes). */
    if (marker == KILN_CFG_SWAP_MARKER_ESP_DONE && target_id != KILN_CFG_NO_ACTIVE_ID &&
        target_id == s_id_unsaved_target && kiln_cfg_store_get_active_id() == target_id) {
        return false;
    }
    return true;
}

/* ---- field-by-field Pico readback compare (section 4.2 step 7 / 3.1's
 * "field-by-field, not hash-only" rule, P7) --------------------------------
 *
 * Compares every SET entry of `expected` against safety_cfg_store's cache,
 * which the caller MUST have just refreshed via safety_cfg_store_refetch()
 * -- this function itself never triggers a refetch, so it never accidentally
 * re-reads a stale cache and calls that "confirmed" (config_divergence.h's
 * own top-comment hazard: "a Pico whose actual values have drifted would
 * still return the expected hash" if fed an echo rather than a fresh
 * fetch). Float fields are compared through config_identity_normalize_f32()
 * -- imported here by value rather than by #include, since this file
 * already has its own float-text round trip via safety_cfg_write's %.9g
 * encode and does not need config_divergence.h's whole API for a single
 * field compare; duplicating exactly ONE normalization step (not the
 * general identity/hash machinery, which item 7 already owns) avoids a
 * dependency on a module this transaction does not otherwise need. */
static float normalize_f32_like_wire(float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.9g", (double)value);
    return strtof(buf, NULL);
}

static bool pico_readback_matches(const kiln_pkg_safety_t *expected, uint16_t skip_param_id,
                                  char *reason_out, size_t reason_cap)
{
    for (uint16_t i = 0; i < expected->count; i++) {
        const kiln_pkg_pico_param_t *want = &expected->entries[i];
        if (!(want->flags & KILN_PKG_PARAM_FLAG_SET) || want->param_id == skip_param_id) {
            continue;
        }
        uint8_t type;
        const char *name;
        if (!safety_cfg_store_lookup(want->param_id, &type, &name)) {
            snprintf(reason_out, reason_cap, "param id %u unknown to this build's mirror table",
                     (unsigned)want->param_id);
            return false;
        }
        size_t n = safety_cfg_store_param_count();
        bool found = false;
        safety_cfg_param_t row;
        for (size_t idx = 0; idx < n; idx++) {
            if (safety_cfg_store_get_by_index(idx, &row) && row.param_id == want->param_id) {
                found = true;
                break;
            }
        }
        if (!found || !row.set) {
            snprintf(reason_out, reason_cap, "param '%s' (id %u) not confirmed by the Pico's readback",
                     name ? name : "?", (unsigned)want->param_id);
            return false;
        }
        bool mismatch = false;
        switch (want->type) {
        case KILNLINK_PARAM_TYPE_BOOL:
            mismatch = (row.value.bool_val != (uint8_t)(want->value_bits & 0xFFu));
            break;
        case KILNLINK_PARAM_TYPE_U8:
            mismatch = (row.value.u8_val != (uint8_t)(want->value_bits & 0xFFu));
            break;
        case KILNLINK_PARAM_TYPE_U16:
            mismatch = (row.value.u16_val != (uint16_t)(want->value_bits & 0xFFFFu));
            break;
        case KILNLINK_PARAM_TYPE_F32: {
            float wf;
            memcpy(&wf, &want->value_bits, sizeof(wf));
            mismatch = (normalize_f32_like_wire(row.value.f32_val) != normalize_f32_like_wire(wf));
            break;
        }
        default:
            mismatch = true;
            break;
        }
        if (mismatch) {
            snprintf(reason_out, reason_cap, "param '%s' (id %u) did not read back as pushed",
                     name ? name : "?", (unsigned)want->param_id);
            return false;
        }
    }
    return true;
}

/* `volatile_install` (item 15): true for both the forward swap push and the
 * rollback restore -- the owner's "the Pico never leaves ARMED" rule (see
 * this file's header comment) means neither of those two may ever go
 * through the flash-writing COMMIT_CONFIG path, which is unconditionally
 * refused while ARMED. false is reserved for kiln_cfg_swap_apply()'s own
 * step-13 best-effort flash-fallback persist (kiln_cfg_swap_persist_pico_
 * fallback()), run only AFTER a swap has already verified via the volatile
 * path -- see that function's own doc comment. */
static bool push_and_verify_pico(SafetyLinkClass *link, const kiln_pkg_safety_t *pkg, bool volatile_install,
                                 char *reason_out, size_t reason_cap)
{
    char push_reason[KILN_CFG_SWAP_REASON_MAX];
    push_reason[0] = '\0';
    if (!safety_cfg_write_apply_package_and_confirm(link, pkg, volatile_install, push_reason, sizeof(push_reason),
                                                    NULL)) {
        /* Precision cap (.181s), not a magic number: GCC's -Werror=format-
         * truncation can prove push_reason's DECLARED size (KILN_CFG_SWAP_
         * REASON_MAX, a fixed-size local array) is up to 199 bytes, and
         * cannot otherwise prove this snprintf into a caller-sized
         * reason_out/reason_cap never truncates -- this class is real (a
         * long inner reason CAN be cut off here) but truncation itself is
         * harmless (snprintf always NUL-terminates); the precision only
         * makes the WORST case provable to the compiler instead of merely
         * true in practice. Same fix applied at every other site in this
         * file GCC's constant-propagation flagged (rollback()'s two
         * ROLLBACK FAILED messages). */
        snprintf(reason_out, reason_cap, "Pico push failed: %.181s", push_reason);
        return false;
    }
    /* Force a FRESH GET_CONFIG_PAGE round trip -- never trust the cache
     * apply_package_and_confirm() may have just populated from its own
     * commit-ack path, per this file's own top comment on config_
     * divergence.h's "never an echo" hazard. config_crc=0 forces the
     * unconditional-refetch path (safety_cfg_store_refetch()'s own
     * contract: 0 never matches a real config_crc). */
    if (!safety_cfg_store_refetch(link, 0)) {
        snprintf(reason_out, reason_cap, "Pico readback (GET_CONFIG_PAGE) failed after push");
        return false;
    }
    return pico_readback_matches(pkg, SAFETY_PARAM_ID_ABS_MAX_TEMP_C, reason_out, reason_cap);
}

/* Restores R's own recorded ceiling (p->rollback_pico's
 * SAFETY_PARAM_ID_ABS_MAX_TEMP_C entry) through the volatile install.
 * Best-effort: a failure is logged only, see rollback()'s comment below.
 * Shared by rollback() and kiln_cfg_swap_apply_impl()'s PICO_OPEN-persist
 * failure path, where the raise-first ceiling is the only thing the Pico has
 * been told.
 *
 * LOW-6 (docs/audits/UNCHECKED_PERSIST_RESULT_AUDIT_2026-10-09.md follow-up):
 * R's snapshot can be older than the live zones. A zones POST that raised a
 * zone max between the snapshot (or the raise-first step) and this restore
 * has already raised the Pico ceiling to match; writing R's lower snapshot
 * value back would leave the Pico tighter than the zone maxima, which the
 * owner's "same or looser, never tighter" rule forbids. So the value written
 * is max(R's ceiling, the ceiling the CURRENT zones require), the latter
 * from safety_ceiling_sync_required_ceiling_c() -- the same policy and zone
 * read the reconcile and the zones POST guard use. When the current
 * requirement is unknown (zones not valid yet), R's value is written as
 * before. A zones POST that has guard-raised but not yet committed its
 * zones is not visible here; the reconcile every caller runs right after
 * this re-derives the target and is the second pass for that window. */
static void restore_r_ceiling_volatile(SafetyLinkClass *link, const kiln_cfg_swap_pending_t *p)
{
    for (uint16_t i = 0; i < p->rollback_pico.count; i++) {
        if (p->rollback_pico.entries[i].param_id == SAFETY_PARAM_ID_ABS_MAX_TEMP_C &&
            (p->rollback_pico.entries[i].flags & KILN_PKG_PARAM_FLAG_SET)) {
            float r_ceiling = 0.0f;
            memcpy(&r_ceiling, &p->rollback_pico.entries[i].value_bits, sizeof(r_ceiling));
            float required_c = 0.0f;
            if (safety_ceiling_sync_required_ceiling_c(&required_c) && required_c > r_ceiling) {
                ESP_LOGW(TAG, "restoring the Pico ceiling to %.1f C, not R's %.1f C -- the current zone maxima "
                              "require it (never tighter than the zones)",
                         (double)required_c, (double)r_ceiling);
                r_ceiling = required_c;
            }
            char ceiling_reason[KILN_CFG_SWAP_REASON_MAX];
            ceiling_reason[0] = '\0';
            if (!safety_cfg_write_set_and_confirm_f32_volatile(link, SAFETY_PARAM_ID_ABS_MAX_TEMP_C, r_ceiling,
                                                              ceiling_reason, sizeof(ceiling_reason), NULL)) {
                ESP_LOGW(TAG, "could not restore the Pico's pre-swap ceiling directly (%s) -- "
                              "the standing reconcile-on-link-up call is a second attempt",
                         ceiling_reason);
            }
            break;
        }
    }
}

/* ---- rollback -------------------------------------------------------------
 *
 * Re-applies R (the pre-swap snapshot) to whichever side(s) actually moved.
 * `esp_was_committed` is true only once step 8 (zones_config_import_blob(P))
 * has actually succeeded -- before that, the ESP side is UNTOUCHED and
 * rollback only has to deal with the Pico. Returns true iff both sides are
 * confirmed back on R (or were never moved off it) -- false means the
 * owner's worst case: latch, disable, and leave the pending record for a
 * retry (never silently re-arm on an unconfirmed rollback). */
static bool rollback(SafetyLinkClass *link, const kiln_cfg_swap_pending_t *p, bool esp_was_committed,
                     char *reason_out, size_t reason_cap)
{
    char sub[KILN_CFG_SWAP_REASON_MAX];
    sub[0] = '\0';
    /* Item 15: rollback uses the SAME mechanism as the forward push
     * (volatile_install=true) -- the owner's "never a way that the Pico is
     * not armed" rule applies just as much to undoing a swap as to doing
     * one. Before item 15 existed, this restore went through the same
     * flash-writing COMMIT_CONFIG path the forward push did and would have
     * been refused while ARMED exactly like the forward push -- routing
     * only the forward path through 0x2D and leaving this one on flash
     * would have left the single most likely failure case (a forward push
     * that fails partway and needs undoing) unable to actually roll back on
     * an armed board. */
    if (!push_and_verify_pico(link, &p->rollback_pico, /*volatile_install=*/true, sub, sizeof(sub))) {
        /* Precision cap -- see push_and_verify_pico()'s identical comment
         * above for why (GCC -Werror=format-truncation, provable via `sub`'s
         * fixed KILN_CFG_SWAP_REASON_MAX declared size). */
        snprintf(reason_out, reason_cap, "ROLLBACK FAILED (Pico would not accept the previous config): %.138s", sub);
        return false;
    }
    if (esp_was_committed) {
        kiln_cfg_store_lock();
        /* Defect 1, same rationale as kiln_cfg_swap_apply()'s step 8: this
         * re-import is restoring p->previous_active_id's config, but
         * active_id itself is not restored to previous_active_id until
         * below (kiln_cfg_store_set_active_id_raw()) -- without the
         * override, this import's autosave would target whatever slot is
         * CURRENTLY marked active (the swap's target_id) instead of the
         * slot this blob actually belongs to. */
        kiln_cfg_store_set_autosave_target_override(p->previous_active_id);
        bool imported = zones_config_import_blob(p->rollback_blob, p->rollback_blob_len, sub, sizeof(sub));
        kiln_cfg_store_set_autosave_target_override(KILN_CFG_AUTOSAVE_OVERRIDE_NONE);
        kiln_cfg_store_unlock();
        if (!imported) {
            snprintf(reason_out, reason_cap, "ROLLBACK FAILED (ESP would not re-accept the previous config): %.136s",
                     sub);
            return false;
        }
    }
    /* Ceiling: push_and_verify_pico() above (like the forward path's own
     * push) always excludes SAFETY_PARAM_ID_ABS_MAX_TEMP_C -- restore R's
     * own recorded ceiling directly here, via the SAME volatile mechanism,
     * rather than leaving it to the standing reconcile-on-link-up's
     * eventual (flash-writing, ARMED-backoff-gated) lower/raise. This is
     * what makes the rollback deterministic and immediate instead of
     * dependent on a background retry cadence the owner's "never disarm"
     * rule was specifically written against. Best-effort: a failure here is
     * logged, not treated as rollback failure -- the two sides' non-ceiling
     * content and the Pico's ceiling identity are what the caller's own
     * post-rollback state actually depends on, and the standing reconcile
     * call right after this still runs as a second attempt/verification. */
    restore_r_ceiling_volatile(link, p);
    /* Best-effort second pass / verification, exactly like the forward
     * path's own step 10 (safety_ceiling_sync_apply_lower()'s documented
     * contract: never blocks, never a failure the caller must act on). */
    safety_ceiling_sync_reconcile_on_link_up(link);
    /* LOW-5: under kiln_cfg_store_lock like every other active_id writer in
     * this file. Taken only after the Pico round trips and the reconcile
     * above have returned -- never held across Pico or HTTP calls. */
    kiln_cfg_store_lock();
    bool id_restored = kiln_cfg_store_set_active_id_raw(p->previous_active_id, sub, sizeof(sub));
    kiln_cfg_store_unlock();
    if (!id_restored) {
        ESP_LOGW(TAG, "rollback: restoring previous active_id bookkeeping failed: %s -- live config is "
                      "correct, only the picker's 'active' marker may be stale",
                 sub);
    }
    /* Both sides confirmed back on R -- the pending record's job is done.
     * Idempotent with boot_recover()'s own explicit clear_pending() calls
     * on its PICO_OPEN/PICO_DONE paths (this function is their shared
     * implementation); every OTHER caller (kiln_cfg_swap_apply()'s own
     * rollback branches) relied on this and would otherwise leave a stale
     * PICO_OPEN/PICO_DONE/ESP_DONE record behind after a swap that was, in
     * fact, cleanly refused with nothing changed.
     *
     * LOW-4: the clear's result is checked. Both sides ARE back on R, so
     * this still returns true (the rollback itself succeeded), but a failed
     * clear leaves the record readable and kiln_cfg_swap_is_pending() true
     * -- autosave stays off until the next boot's recovery, which finds both
     * sides on R and clears it (finish_esp_done_impl()'s LOW-3 branch, or
     * the PICO_OPEN/PICO_DONE rollback paths). Logged, and written to
     * reason_out (KILN_CFG_SWAP_ROLLBACK_UNCLEARED_NOTE; on success the
     * caller's reason_out is otherwise left empty) so apply's success-path
     * callers report it via rolled_back_reason() instead of going silent. */
    if (!clear_pending()) {
        ESP_LOGE(TAG, "rollback: both sides are back on the previous config, but clearing the swap journal "
                      "failed -- autosave stays suppressed until the next boot clears it");
        (void)set_reason(reason_out, reason_cap, KILN_CFG_SWAP_ROLLBACK_UNCLEARED_NOTE);
    }
    return true;
}

/* Reason for an apply that was refused and rolled back cleanly. `roll_note`
 * is the rollback()'s reason_out: empty on a fully clean rollback, or
 * KILN_CFG_SWAP_ROLLBACK_UNCLEARED_NOTE when the journal clear failed
 * (LOW-4), which is put first so truncation never drops it. Always returns false. */
static bool rolled_back_reason(char *reason_out, size_t reason_cap, const char *msg, const char *roll_note)
{
    if (roll_note && roll_note[0] && reason_out && reason_cap) {
        /* Note first: a long msg truncates, never the note (LOW-4). */
        snprintf(reason_out, reason_cap, "%s; %s", roll_note, msg);
        return false;
    }
    return set_reason(reason_out, reason_cap, msg);
}

/* ---- step 13: best-effort Pico flash-fallback persist ---------------------
 *
 * Runs ONLY after kiln_cfg_swap_apply() has already decided the swap
 * succeeded (both halves committed, read back, and verified matching, and
 * the post-swap ceiling/arming check passed) -- this function's job is
 * purely "since we are here anyway, try to make it survive a reboot too",
 * never a precondition for the swap's own result. It re-pushes the SAME
 * `target_pico` fields that were already volatile-installed and verified,
 * but through the flash-writing COMMIT_CONFIG path
 * (volatile_install=false) -- i.e. section 1a.4's case 1, "persist what is
 * now proven live". Ceiling is pushed too (this is the one place a flash
 * write of the ceiling is attempted for a swap; the flash-writing single-
 * field wrapper is safety_cfg_write_set_and_confirm_f32(), unchanged, NOT
 * the volatile variant this module otherwise uses).
 *
 * ALLOWED TO FAIL, always, with only a log line -- an ARMED refusal here
 * (the Pico's ordinary running state; this call is expected to fail on
 * essentially every armed board) is not a swap failure and never alarms.
 * The swap is already complete: content is proven live and verified on
 * both processors before this function is ever called. What this function
 * does NOT do -- and item 15's plan section 1a.4 describes but this pass
 * does not implement -- is the `pkg_hash` bookkeeping a future boot-time
 * FALLBACK/CONFIGURED/UNCONFIGURED state machine would need to know WHICH
 * package a successful flash-fallback write came from; no such field
 * exists in config_store_record_t or on the wire today, and adding one
 * is out of this pass's scope (no ZONES_CFG_VERSION bump, no new wire
 * field). A flash write that lands here simply becomes the Pico's ordinary
 * flashed record, exactly as if an operator had pushed it while unarmed --
 * indistinguishable from any other COMMIT_CONFIG, which is a reasonable
 * bring-up fallback on its own even without the hash bookkeeping. */
static void persist_pico_flash_fallback(SafetyLinkClass *link, const kiln_pkg_safety_t *target_pico,
                                        float target_ceiling, bool have_target_ceiling)
{
    if (have_target_ceiling) {
        char ceiling_reason[KILN_CFG_SWAP_REASON_MAX];
        ceiling_reason[0] = '\0';
        if (!safety_cfg_write_set_and_confirm_f32(link, SAFETY_PARAM_ID_ABS_MAX_TEMP_C, target_ceiling,
                                                 ceiling_reason, sizeof(ceiling_reason), NULL)) {
            ESP_LOGI(TAG, "step 13 (flash fallback): ceiling flash persist did not land (%s) -- expected "
                          "while ARMED, swap already succeeded via the volatile install, not a failure",
                     ceiling_reason);
        }
    }
    char push_reason[KILN_CFG_SWAP_REASON_MAX];
    push_reason[0] = '\0';
    if (!safety_cfg_write_apply_package_and_confirm(link, target_pico, /*volatile_install=*/false, push_reason,
                                                    sizeof(push_reason), NULL)) {
        ESP_LOGI(TAG, "step 13 (flash fallback): config flash persist did not land (%s) -- expected while "
                      "ARMED, swap already succeeded via the volatile install, not a failure",
                 push_reason);
    } else {
        ESP_LOGI(TAG, "step 13 (flash fallback): the just-verified swap also landed on Pico flash -- "
                      "will survive a Pico reboot");
    }
}

/* docs/audits/kiln_cfg_swap_stack_overflow_2026-09-22.md, review follow-up:
 * these four aggregates (target_blob/target_pico/p/readback_blob, ~4.25 kB
 * combined) are the scratch a single in-flight kiln_cfg_swap_apply() call
 * needs. HEAP, not static: a static (the original fix) is a PERMANENT
 * +8.5 kB .bss cost paid every boot whether or not a swap has ever run,
 * on a board whose internal-DRAM headroom this project has already
 * exhausted once (project_esp_internal_dram_exhaustion). Allocating this
 * struct on kiln_cfg_swap_apply()'s own entry and freeing it on every
 * return -- see the thin kiln_cfg_swap_apply()/_impl() wrapper below --
 * costs nothing when idle and only ever holds the memory for the duration
 * of one apply job, the same "smallest permanent footprint, never partial"
 * tradeoff profiles_live_http.c's own heap_caps_malloc()/_free() pairs
 * already make. MALLOC_CAP_INTERNAL, not SPIRAM: this scratch is read by
 * zones_config_import_blob()/_export_blob() (NVS/flash) and pushed over the
 * safety-link UART, both of which this codebase's convention requires stay
 * off PSRAM. Concurrency is unaffected by moving off `static`: this
 * function still runs ONLY on the dedicated kiln_cfg_swap_worker task
 * (kiln_cfg_swap_worker.c), serialized by a depth-1 queue plus an
 * is_busy() interlock, so at most one heap instance is ever alive. */
typedef struct {
    uint8_t target_blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    kiln_pkg_safety_t target_pico;
    kiln_cfg_swap_pending_t p;
    uint8_t readback_blob[ZONES_CONFIG_BLOB_MAX_SIZE];
} kiln_cfg_swap_apply_scratch_t;

static bool kiln_cfg_swap_apply_impl(int32_t target_id, bool ack_no_safety_processor, char *reason_out,
                                     size_t reason_cap, bool *out_diverged, kiln_cfg_swap_apply_scratch_t *scratch)
{
    if (out_diverged) {
        *out_diverged = false;
    }
    if (!reason_out || reason_cap == 0) {
        return false;
    }
    /* step 0 */
    if (ota_http_check_interlocks(ack_no_safety_processor, reason_out, reason_cap) != OTA_INTERLOCK_OK) {
        return false;
    }
    SafetyLinkClass *link = s_link;
    if (!link) {
        return set_reason(reason_out, reason_cap, "no safety processor link this boot -- a kiln swap "
                                                    "always includes the safety processor's half");
    }

    /* step 1 (H17): a single call, pico_out non-NULL, so a half-package
     * (pico_populated==0) is refused right here before anything is
     * snapshotted or touched. */
    uint8_t *target_blob = scratch->target_blob;
    uint16_t target_blob_len = 0;
    kiln_pkg_safety_t *target_pico = &scratch->target_pico;
    if (!kiln_cfg_store_get_full_package(target_id, target_blob, sizeof(scratch->target_blob), &target_blob_len,
                                         target_pico, reason_out, reason_cap)) {
        return false;
    }

    /* step 2: snapshot R and persist it, marker=STAGED. */
    kiln_cfg_swap_pending_t *p = &scratch->p;
    memset(p, 0, sizeof(*p));
    p->target_id = target_id;
    kiln_cfg_store_lock();
    p->previous_active_id = kiln_cfg_store_get_active_id();
    uint32_t gen_before = kiln_cfg_store_generation();
    bool exported = zones_config_export_blob(p->rollback_blob, sizeof(p->rollback_blob));
    kiln_cfg_store_unlock();
    if (!exported) {
        return set_reason(reason_out, reason_cap, "could not snapshot the current ESP config for rollback");
    }
    /* zones_config_export_blob() always writes the same fixed size (see its
     * own header comment) -- ZONES_CONFIG_BLOB_MAX_SIZE, which is also
     * target_blob_len's upper bound, so this length is safe to reuse. */
    p->rollback_blob_len = (uint16_t)ZONES_CONFIG_BLOB_MAX_SIZE;
    {
        kiln_pkg_pico_source_t src = kiln_pkg_pico_source_default();
        if (!kiln_package_capture_pico_half(&src, &p->rollback_pico)) {
            return set_reason(reason_out, reason_cap, "could not snapshot the current Pico config for rollback");
        }
    }
    /* LOW-2: a new transaction supersedes any earlier "only the active id is
     * unsaved" record; autosave stays blocked for this one's whole run. */
    s_id_unsaved_target = KILN_CFG_NO_ACTIVE_ID;
    if (!persist_marker(p, KILN_CFG_SWAP_MARKER_STAGED)) {
        return set_reason(reason_out, reason_cap, "could not persist the rollback record -- refusing to "
                                                    "start a swap with no way back");
    }

    /* step 4: ceiling raise-first (lower-last is handled at step 10 via
     * safety_ceiling_sync_reconcile_on_link_up(), see this file's header
     * comment). Item 15: goes through the volatile install
     * (safety_cfg_write_set_and_confirm_f32_volatile()), NEVER the flash-
     * writing safety_cfg_write_set_and_confirm_f32() the standing ceiling-
     * reconcile loop still uses -- this raise is part of a swap the owner's
     * rule requires to never disarm the Pico, and the flash path is
     * unconditionally refused while ARMED (the Pico's ordinary running
     * state). */
    float target_ceiling = 0.0f;
    bool have_target_ceiling = false;
    for (uint16_t i = 0; i < target_pico->count; i++) {
        if (target_pico->entries[i].param_id == SAFETY_PARAM_ID_ABS_MAX_TEMP_C &&
            (target_pico->entries[i].flags & KILN_PKG_PARAM_FLAG_SET)) {
            memcpy(&target_ceiling, &target_pico->entries[i].value_bits, sizeof(target_ceiling));
            have_target_ceiling = true;
            break;
        }
    }
    float current_ceiling = 0.0f;
    bool have_current_ceiling = safety_ceiling_sync_get_current_pico_ceiling(&current_ceiling);
    bool raise_first = have_target_ceiling && (!have_current_ceiling || target_ceiling >= current_ceiling);
    if (raise_first) {
        char sub[KILN_CFG_SWAP_REASON_MAX];
        sub[0] = '\0';
        if (!safety_cfg_write_set_and_confirm_f32_volatile(link, SAFETY_PARAM_ID_ABS_MAX_TEMP_C, target_ceiling, sub,
                                                          sizeof(sub), NULL)) {
            clear_pending();
            snprintf(reason_out, reason_cap, "could not raise the Pico's ceiling before the swap: %s", sub);
            return false; /* nothing else touched yet -- safe to just discard the STAGED record */
        }
    }

    /* step 5 (M2, docs/audits/UNCHECKED_PERSIST_RESULT_AUDIT_2026-10-09.md):
     * PICO_OPEN must be on flash BEFORE the Pico is told anything but the
     * raise-first ceiling. If it is not, a crash after step 6 would be
     * recovered as STAGED (discard, no re-apply of R) while the Pico holds the
     * target's volatile values. So a failed persist refuses here: the only
     * change on the Pico so far is a ceiling that is the same as or looser
     * than R's (the owner's "same or looser" rule allows that), and it is put
     * back to R's best-effort, with the standing reconcile as a second pass.
     * The record is then cleared; if that clear fails too, it still reads
     * STAGED, and boot's STAGED discard is correct for this state. */
    if (!persist_marker(p, KILN_CFG_SWAP_MARKER_PICO_OPEN)) {
        if (raise_first) {
            restore_r_ceiling_volatile(link, p);
            safety_ceiling_sync_reconcile_on_link_up(link);
        }
        clear_pending();
        return set_reason(reason_out, reason_cap, "could not persist the swap journal before touching the safety "
                                                    "processor -- swap refused, nothing changed");
    }

    /* step 6/7: push everything except the ceiling, read back, compare.
     * Item 15: volatile_install=true -- the owner's "Pico never leaves
     * ARMED" rule; see push_and_verify_pico()'s own doc comment. */
    char push_reason[KILN_CFG_SWAP_REASON_MAX];
    push_reason[0] = '\0';
    if (!push_and_verify_pico(link, target_pico, /*volatile_install=*/true, push_reason, sizeof(push_reason))) {
        char roll_reason[KILN_CFG_SWAP_REASON_MAX];
        roll_reason[0] = '\0';
        if (!rollback(link, p, /*esp_was_committed=*/false, roll_reason, sizeof(roll_reason))) {
            /* leave the pending record intact for boot/retry */
            if (out_diverged) {
                *out_diverged = true;
            }
            snprintf(reason_out, reason_cap, "swap refused (%s) AND rollback failed (%s) -- alarmed", push_reason,
                     roll_reason);
            return false;
        }
        char refused[KILN_CFG_SWAP_REASON_MAX + 40];
        snprintf(refused, sizeof(refused), "swap refused, rolled back cleanly: %s", push_reason);
        return rolled_back_reason(reason_out, reason_cap, refused, roll_reason);
    }
    /* M2: PICO_DONE must be on flash before the ESP half is committed. If it
     * is not, the record still reads PICO_OPEN, whose boot recovery restores
     * only the Pico (esp_was_committed=false) -- a crash after step 8 would
     * leave the ESP on the target and the Pico on R. Roll back now instead,
     * while the ESP is untouched. */
    if (!persist_marker(p, KILN_CFG_SWAP_MARKER_PICO_DONE)) {
        char roll_reason[KILN_CFG_SWAP_REASON_MAX];
        roll_reason[0] = '\0';
        if (!rollback(link, p, /*esp_was_committed=*/false, roll_reason, sizeof(roll_reason))) {
            if (out_diverged) {
                *out_diverged = true;
            }
            snprintf(reason_out, reason_cap,
                     "could not persist the swap journal after the safety processor half, AND rollback failed "
                     "(%.100s) -- alarmed",
                     roll_reason);
            return false;
        }
        return rolled_back_reason(reason_out, reason_cap,
                                  "could not persist the swap journal after the safety processor half -- swap "
                                  "refused and rolled back",
                                  roll_reason);
    }

    /* generation check -- H6: refuse to proceed if another writer touched
     * the store while we were off doing the Pico round trip, unlocked. */
    if (kiln_cfg_store_generation() != gen_before) {
        char roll_reason[KILN_CFG_SWAP_REASON_MAX];
        roll_reason[0] = '\0';
        if (!rollback(link, p, /*esp_was_committed=*/false, roll_reason, sizeof(roll_reason))) {
            if (out_diverged) {
                *out_diverged = true;
            }
            snprintf(reason_out, reason_cap,
                     "another kiln-config write raced this swap, AND rollback failed (%s) -- alarmed", roll_reason);
            return false;
        }
        return rolled_back_reason(reason_out, reason_cap,
                                  "another kiln-config write raced this swap -- refused and rolled back, try again",
                                  roll_reason);
    }

    /* step 8/9: commit the ESP half, read it back byte-for-byte */
    char esp_reason[KILN_CFG_SWAP_REASON_MAX];
    esp_reason[0] = '\0';
    kiln_cfg_store_lock();
    /* Defect 1 (docs/audits/kiln_profiles_feature_review_2026-09-15.md):
     * active_id still names the OUTGOING kiln here -- it only moves to
     * target_id at step 12, below -- so without this override the autosave
     * this import's nvs_save() dispatches would write the INCOMING config
     * over the OUTGOING kiln's saved slot. The config landing right now
     * belongs to target_id; say so explicitly. Cleared unconditionally
     * right after, on both the success and failure path, per this override's
     * own contract (kiln_cfg_store.h). */
    kiln_cfg_store_set_autosave_target_override(target_id);
    bool esp_ok = zones_config_import_blob(target_blob, target_blob_len, esp_reason, sizeof(esp_reason));
    kiln_cfg_store_set_autosave_target_override(KILN_CFG_AUTOSAVE_OVERRIDE_NONE);
    kiln_cfg_store_unlock();
    if (!esp_ok) {
        char roll_reason[KILN_CFG_SWAP_REASON_MAX];
        roll_reason[0] = '\0';
        if (!rollback(link, p, /*esp_was_committed=*/false, roll_reason, sizeof(roll_reason))) {
            if (out_diverged) {
                *out_diverged = true;
            }
            snprintf(reason_out, reason_cap, "ESP commit refused (%s) AND rollback failed (%s) -- alarmed",
                     esp_reason, roll_reason);
            return false;
        }
        char refused[KILN_CFG_SWAP_REASON_MAX + 40];
        snprintf(refused, sizeof(refused), "ESP half refused, rolled back cleanly: %s", esp_reason);
        return rolled_back_reason(reason_out, reason_cap, refused, roll_reason);
    }
    /* M2: without ESP_DONE on flash the record still reads PICO_DONE, and the
     * final clear below would most likely fail the same way -- the next boot
     * would then roll back a swap this call had reported as a success. Roll
     * back now, so the persisted record never lags behind the two sides. */
    if (!persist_marker(p, KILN_CFG_SWAP_MARKER_ESP_DONE)) {
        char roll_reason[KILN_CFG_SWAP_REASON_MAX];
        roll_reason[0] = '\0';
        if (!rollback(link, p, /*esp_was_committed=*/true, roll_reason, sizeof(roll_reason))) {
            if (out_diverged) {
                *out_diverged = true;
            }
            snprintf(reason_out, reason_cap,
                     "could not persist the swap journal after the ESP half, AND rollback failed (%.100s) -- "
                     "alarmed",
                     roll_reason);
            return false;
        }
        return rolled_back_reason(reason_out, reason_cap,
                                  "could not persist the swap journal after the ESP half -- swap refused and "
                                  "rolled back",
                                  roll_reason);
    }

    kiln_cfg_store_lock();
    uint8_t *readback_blob = scratch->readback_blob;
    bool readback_ok = zones_config_export_blob(readback_blob, sizeof(scratch->readback_blob)) &&
                       memcmp(readback_blob, target_blob, target_blob_len) == 0;
    kiln_cfg_store_unlock();
    if (!readback_ok) {
        char roll_reason[KILN_CFG_SWAP_REASON_MAX];
        roll_reason[0] = '\0';
        if (!rollback(link, p, /*esp_was_committed=*/true, roll_reason, sizeof(roll_reason))) {
            if (out_diverged) {
                *out_diverged = true;
            }
            snprintf(reason_out, reason_cap, "ESP readback did not match what was committed, AND rollback "
                                              "failed (%s) -- alarmed",
                     roll_reason);
            return false;
        }
        return rolled_back_reason(reason_out, reason_cap,
                                  "ESP readback did not match what was committed -- rolled back cleanly",
                                  roll_reason);
    }

    /* step 12 moved up here, right after readback proves the live zones
     * config genuinely IS target_id's (MEDIUM finding 3, adversarial review
     * 2026-09-15, docs/audits/review_autosave_slot_fix_a93ee77b_2026-09-15.md):
     * a short override window around step 8's import alone left active_id
     * stale (still naming the outgoing kiln) all the way through the
     * ceiling/arming divergence check below -- including on the "diverged"
     * exit, which used to return with that mismatch in place until a retry
     * or reboot. Any ordinary nvs_save() in that window (a zones POST, an
     * autotune write) would still have overwritten the OUTGOING kiln's
     * slot, override or not, since the override had already been cleared.
     * Moving the assignment here instead of leaving it at the bottom closes
     * the gap at its root: active_id now tracks "what zones config is
     * actually loaded", which readback above already proved is target_id's,
     * independent of whether the Pico ceiling cross-check below passes. */
    char final_reason[KILN_CFG_SWAP_REASON_MAX];
    final_reason[0] = '\0';
    kiln_cfg_store_lock();
    bool finalized = kiln_cfg_store_set_active_id_raw(target_id, final_reason, sizeof(final_reason));
    kiln_cfg_store_unlock();
    if (!finalized) {
        /* M1 (docs/audits/UNCHECKED_PERSIST_RESULT_AUDIT_2026-10-09.md):
         * content is correct on both sides and RAM active_id already names
         * target_id (set_active_id_raw sets RAM before persisting), but the
         * persisted id still names the outgoing kiln. The record stays at
         * ESP_DONE (never cleared, see the end of this function) so the next
         * boot's finish_esp_done() re-verifies and retries the id save.
         * Autosave never writes into the outgoing kiln's slot: it targets
         * RAM active_id (the target), and kiln_cfg_swap_is_pending() keeps it
         * off entirely until this apply has finished (LOW-2 below lifts that
         * only at the end). Reported as a failed apply, not a clean success. */
        ESP_LOGE(TAG, "swap content landed and verified, but recording active_id failed: %s -- swap journal "
                      "kept open, retried at next boot",
                 final_reason);
    }

    /* step 10/11: ceiling identity + arming, reusing item 7's existing
     * divergence primitive rather than a second detector (see this file's
     * header comment). A failure here does NOT roll back -- both halves
     * already agree on content; only the cross-check failed -- it alarms
     * and leaves the pending record at ESP_DONE for boot recovery's
     * "verify then finish" path (section 4.4's ESP_DONE row) to retry. */
    safety_ceiling_sync_reconcile_on_link_up(link);
    char div_reason[KILN_CFG_SWAP_REASON_MAX];
    div_reason[0] = '\0';
    bool diverged = safety_ceiling_sync_is_diverged(div_reason, sizeof(div_reason));
    if (diverged) {
        if (out_diverged) {
            *out_diverged = true;
        }
        /* active_id was already moved to target_id above, right after
         * readback proved the content match -- this alarm is reported
         * against a store that already correctly reflects what's live.
         * This is the ONLY branch that disables heat: it means
         * safety_ceiling_sync_is_diverged() found the ceiling/arming latch
         * set by enforce_ceiling_divergence(), which is what actually calls
         * the disable-relays/halt-run hooks. */
        snprintf(reason_out, reason_cap,
                 "both halves committed and matched, but the post-swap ceiling/arming check failed (%s) -- "
                 "heaters disabled and alarmed, config left pending for retry",
                 div_reason);
        return false;
    }

    /* step 13: best-effort Pico flash-fallback persist (item 15's "persist
     * what is now proven live" case). See this function's own doc comment
     * for why this is allowed to fail and never affects the swap's own
     * result, which is already decided above. */
    persist_pico_flash_fallback(link, target_pico, target_ceiling, have_target_ceiling);

    if (!finalized) {
        /* LOW-2: the transaction is finished in every respect but the
         * persisted id; let autosave follow the live (target) kiln. */
        s_id_unsaved_target = target_id;
        snprintf(reason_out, reason_cap,
                 "%s both processors, but saving which kiln is active failed (%.80s) -- retried at next boot; "
                 "until then the kiln list may name the previous kiln",
                 KILN_CFG_SWAP_REASON_ACTIVE_ID_UNSAVED_PREFIX, final_reason);
        return false;
    }
    clear_pending();
    return true;
}

bool kiln_cfg_swap_apply(int32_t target_id, bool ack_no_safety_processor, char *reason_out, size_t reason_cap,
                         bool *out_diverged)
{
    /* MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT, not SPIRAM -- see
     * kiln_cfg_swap_apply_scratch_t's own doc comment above. Failing loud
     * here, before ota_http_check_interlocks()/kiln_cfg_store_get_full_package()
     * have run, means an allocation failure can never leave a swap
     * half-started: nothing has been snapshotted or touched yet. */
    kiln_cfg_swap_apply_scratch_t *scratch =
        heap_caps_malloc(sizeof(*scratch), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!scratch) {
        if (out_diverged) {
            *out_diverged = false;
        }
        ESP_LOGE(TAG, "kiln_cfg_swap_apply: heap_caps_malloc(%u B, INTERNAL) failed -- refusing, "
                      "nothing touched",
                 (unsigned)sizeof(*scratch));
        return set_reason(reason_out, reason_cap,
                          "out of memory staging the kiln config swap -- nothing was touched, try again");
    }
    bool ok = kiln_cfg_swap_apply_impl(target_id, ack_no_safety_processor, reason_out, reason_cap, out_diverged,
                                       scratch);
    heap_caps_free(scratch);
    return ok;
}

/* ---- boot recovery (section 4.4 / plan item 8) ---------------------------- */

/* Deliberately its own NON-INLINED frame, not a branch of the switch in
 * kiln_cfg_swap_boot_recover() below.
 *
 * GCC sums the locals of sibling switch branches into one frame, so while
 * this code sat inline it contributed target_blob[896] + live_blob[896] +
 * a kiln_pkg_safety_t + two reason buffers to EVERY path through boot
 * recovery, including the paths that never touch a target slot. Measured,
 * that made boot_recover's own frame 4928 B, and the deepest path through it
 * 8192 B once the safety-link push chain beneath it is counted -- which
 * overflowed the `main` task (check_main_task_stack_budget.ps1, budget
 * 6144 B) and would equally have overflowed this feature's own 8192 B worker
 * task, with 112 B to spare. A boot-time stack overflow here is not a clean
 * crash: it smashes the return address and the board takes an
 * IllegalInstruction panic with a corrupted backtrace
 * (docs/audits/boot_hang_2026-09-08.md), which is how this board has been
 * bricked into a recovery loop before.
 *
 * `noinline` is load-bearing, not decorative: without it the compiler is
 * free to fold this straight back into the caller and silently restore the
 * oversized frame. The budget check would catch that, but only after it had
 * already been written. */
/* Portable noinline, same guard and same reason as safety_cfg_store.c's
 * SAFETY_CFG_STORE_NOINLINE and autotune_engine.c's AUTOTUNE_ENGINE_NOINLINE:
 * this file's own host test (test_kiln_cfg_swap.c, MSVC via
 * build_host_tests.ps1) does not understand GCC/Xtensa's
 * __attribute__((noinline)) syntax at all -- under cl.exe it is a hard syntax
 * error, not a no-op, and it cascades into a dozen misleading "undeclared
 * identifier" errors further down this function. Note that
 * tools/run_all_checks.ps1 does NOT build the C host suite (only
 * check_js_host_tests.ps1); that gate is tools/verify.ps1, so a green
 * run_all_checks does not prove this compiles under MSVC. Behaviour on the
 * real ESP-IDF (Xtensa GCC) target, which is the only build whose stack
 * budget is measured, is unchanged. */
#if defined(_MSC_VER)
#define KILN_CFG_SWAP_NOINLINE
#else
#define KILN_CFG_SWAP_NOINLINE __attribute__((noinline))
#endif

/* HEAP, not static -- see kiln_cfg_swap_apply_scratch_t's doc comment above;
 * identical reasoning and the same review follow-up
 * (docs/audits/kiln_cfg_swap_stack_overflow_2026-09-22.md). finish_esp_done()
 * is reachable only from kiln_cfg_swap_boot_recover(), itself only ever
 * called from the dedicated kiln_cfg_swap_worker task before its job queue
 * loop starts -- never concurrent with kiln_cfg_swap_apply() on that same
 * task, so at most one heap instance of this scratch is ever alive too. */
typedef struct {
    uint8_t target_blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    kiln_pkg_safety_t target_pico;
    uint8_t live_blob[ZONES_CONFIG_BLOB_MAX_SIZE];
} kiln_cfg_swap_finish_scratch_t;

static void KILN_CFG_SWAP_NOINLINE finish_esp_done_impl(SafetyLinkClass *link, const kiln_cfg_swap_pending_t *p,
                                                        kiln_cfg_swap_finish_scratch_t *scratch)
{
    /* The one case where FINISHING is correct (section 4.4): both sides
     * already claim P. Re-verify both independently (never either side's
     * cache) and only then finish. */
    uint8_t *target_blob = scratch->target_blob;
    uint16_t target_blob_len = 0;
    kiln_pkg_safety_t *target_pico = &scratch->target_pico;
    char sub[KILN_CFG_SWAP_REASON_MAX];
    sub[0] = '\0';
    if (!kiln_cfg_store_get_full_package(p->target_id, target_blob, sizeof(scratch->target_blob), &target_blob_len,
                                         target_pico, sub, sizeof(sub))) {
        ESP_LOGE(TAG, "boot: ESP_DONE recovery could not re-read target slot %ld: %s -- rolling back "
                      "to the pre-swap config instead",
                 (long)p->target_id, sub);
        if (link && rollback(link, p, /*esp_was_committed=*/true, sub, sizeof(sub))) {
            clear_pending();
        } else {
            ESP_LOGE(TAG, "boot: ESP_DONE fallback rollback also failed: %s -- staying alarmed", sub);
            char op_reason[KILN_CFG_SWAP_REASON_MAX + 128];
            snprintf(op_reason, sizeof(op_reason),
                     "an interrupted kiln-config swap's target slot could not be re-read at boot, and "
                     "the fallback rollback also failed (%.40s) -- heaters stay alarmed/disabled; apply a "
                     "kiln config again to clear this", sub);
            latch_boot_fault(KILN_CFG_SWAP_BOOT_FAULT_ESP_DONE_UNCONFIRMED, p->target_id, op_reason);
        }
        return;
    }
    kiln_cfg_store_lock();
    uint8_t *live_blob = scratch->live_blob;
    bool live_ok = zones_config_export_blob(live_blob, sizeof(scratch->live_blob));
    bool esp_matches = live_ok && memcmp(live_blob, target_blob, target_blob_len) == 0;
    /* LOW-3: an ESP_DONE record whose two sides are both back on R is a
     * rollback that completed but whose final clear failed (rollback()'s
     * LOW-4 path). Nothing is left to finish or undo -- clear the record
     * instead of latching ESP_DONE_UNCONFIRMED over a consistent state. */
    bool esp_on_r = live_ok && !esp_matches && p->rollback_blob_len > 0 &&
                    memcmp(live_blob, p->rollback_blob, p->rollback_blob_len) == 0;
    kiln_cfg_store_unlock();
    bool pico_matches = false;
    bool pico_on_r = false;
    if (link && safety_cfg_store_refetch(link, 0)) {
        pico_matches = pico_readback_matches(target_pico, SAFETY_PARAM_ID_ABS_MAX_TEMP_C, sub, sizeof(sub));
        if (esp_on_r) {
            /* `sub` reused, not a second reason buffer: this frame's stack is
             * measured (see the noinline comment above), and esp_on_r implies
             * !esp_matches, so the target compare's text is not needed. */
            pico_on_r = pico_readback_matches(&p->rollback_pico, SAFETY_PARAM_ID_ABS_MAX_TEMP_C, sub, sizeof(sub));
        }
    }
    if (esp_on_r && pico_on_r) {
        /* Both sides hold R, so the persisted active_id must name R's kiln
         * before the record can go. A diverged/ACTIVE_ID_UNSAVED path may
         * already have persisted the target id; clearing with that id while
         * R is live would let autosave write R over the target's slot.
         * Restore previous_active_id first; if that fails, fall through to
         * the ESP_DONE_UNCONFIRMED latch below instead of clearing. */
        kiln_cfg_store_lock();
        bool id_ok = kiln_cfg_store_get_active_id() == p->previous_active_id ||
                     kiln_cfg_store_set_active_id_raw(p->previous_active_id, sub, sizeof(sub));
        kiln_cfg_store_unlock();
        if (!id_ok) {
            ESP_LOGE(TAG, "boot: ESP_DONE record with both sides back on the pre-swap config, but restoring "
                          "active_id %ld failed: %s -- not cleared",
                     (long)p->previous_active_id, sub);
            esp_on_r = false;
        }
    }
    if (esp_on_r && pico_on_r) {
        if (clear_pending()) {
            ESP_LOGW(TAG, "boot: ESP_DONE record found with both sides back on the pre-swap config (a rollback "
                          "whose journal clear failed) -- cleared");
        } else {
            ESP_LOGE(TAG, "boot: ESP_DONE record found with both sides back on the pre-swap config, but the "
                          "clear failed again -- will retry next boot");
        }
        return;
    }
    if (esp_matches && pico_matches) {
        kiln_cfg_store_lock();
        bool id_saved = kiln_cfg_store_set_active_id_raw(p->target_id, sub, sizeof(sub));
        kiln_cfg_store_unlock();
        /* step 13, same as kiln_cfg_swap_apply()'s own -- best-effort,
         * allowed to fail, never affects the outcome already decided
         * above. */
        float target_ceiling = 0.0f;
        bool have_target_ceiling = false;
        for (uint16_t i = 0; i < target_pico->count; i++) {
            if (target_pico->entries[i].param_id == SAFETY_PARAM_ID_ABS_MAX_TEMP_C &&
                (target_pico->entries[i].flags & KILN_PKG_PARAM_FLAG_SET)) {
                memcpy(&target_ceiling, &target_pico->entries[i].value_bits, sizeof(target_ceiling));
                have_target_ceiling = true;
                break;
            }
        }
        persist_pico_flash_fallback(link, target_pico, target_ceiling, have_target_ceiling);
        if (!id_saved) {
            /* M1: both sides are confirmed on the target, but the persisted
             * active_id still names the outgoing kiln. Keep the record at
             * ESP_DONE so the next boot retries. RAM active_id names the
             * target (set_active_id_raw sets RAM before persisting), so
             * LOW-2's s_id_unsaved_target lets autosave write into the
             * target's slot -- never the outgoing kiln's. Display-only
             * fault: the divergence check, not this latch, gates heat. */
            ESP_LOGE(TAG, "boot: ESP_DONE swap confirmed on both sides, but saving active_id %ld failed: %s -- "
                          "swap journal kept open, will retry next boot",
                     (long)p->target_id, sub);
            s_id_unsaved_target = p->target_id;
            latch_boot_fault(KILN_CFG_SWAP_BOOT_FAULT_ACTIVE_ID_UNSAVED, p->target_id,
                             "kiln config applied on both processors, but saving which kiln is active failed -- "
                             "retried automatically at each boot; heat is not affected");
            return;
        }
        clear_pending();
        ESP_LOGW(TAG, "boot: ESP_DONE swap confirmed complete on both sides -- finished");
    } else {
        ESP_LOGE(TAG, "boot: ESP_DONE could not confirm both sides on the new config (esp_matches=%d "
                      "pico_matches=%d) -- staying alarmed, will retry",
                 (int)esp_matches, (int)pico_matches);
        char op_reason[KILN_CFG_SWAP_REASON_MAX];
        snprintf(op_reason, sizeof(op_reason),
                 "an interrupted kiln-config swap could not be confirmed on both processors at boot "
                 "(esp=%s, pico=%s) -- heaters stay alarmed/disabled; apply a kiln config again to "
                 "clear this", esp_matches ? "ok" : "mismatch", pico_matches ? "ok" : "mismatch");
        latch_boot_fault(KILN_CFG_SWAP_BOOT_FAULT_ESP_DONE_UNCONFIRMED, p->target_id, op_reason);
    }
}

static void finish_esp_done(SafetyLinkClass *link, const kiln_cfg_swap_pending_t *p)
{
    kiln_cfg_swap_finish_scratch_t *scratch =
        heap_caps_malloc(sizeof(*scratch), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!scratch) {
        ESP_LOGE(TAG, "finish_esp_done: heap_caps_malloc(%u B, INTERNAL) failed -- cannot confirm "
                      "the interrupted swap this boot, staying alarmed, will retry next boot",
                 (unsigned)sizeof(*scratch));
        char op_reason[KILN_CFG_SWAP_REASON_MAX];
        snprintf(op_reason, sizeof(op_reason),
                 "out of memory confirming an interrupted kiln-config swap at boot -- heaters stay "
                 "alarmed/disabled; reboot or apply a kiln config again to retry");
        latch_boot_fault(KILN_CFG_SWAP_BOOT_FAULT_ESP_DONE_UNCONFIRMED, p->target_id, op_reason);
        return;
    }
    finish_esp_done_impl(link, p, scratch);
    heap_caps_free(scratch);
}

/* HEAP, not static -- see kiln_cfg_swap_apply_scratch_t's doc comment above;
 * identical reasoning. Runs once, as the kiln_cfg_swap_worker task's first
 * act, before its job queue loop (and thus before kiln_cfg_swap_apply())
 * ever starts -- never concurrent with it, so at most one heap instance of
 * this scratch is ever alive too. */
static void kiln_cfg_swap_boot_recover_impl(kiln_cfg_swap_pending_t *p)
{
    bool existed_but_unreadable = false;
    if (!load_pending_ex(p, &existed_but_unreadable)) {
        if (!existed_but_unreadable) {
            return; /* genuinely never staged a swap on this board -- ordinary case */
        }
        ESP_LOGE(TAG, "pending swap record exists but could not be read back at boot (HAL_IO/wrong size) -- "
                      "treating exactly like a failed CRC check: an unrecoverable interrupted swap, staying "
                      "alarmed until an operator applies a kiln config again");
        latch_boot_fault(KILN_CFG_SWAP_BOOT_FAULT_UNREADABLE, KILN_CFG_NO_ACTIVE_ID,
                         "an interrupted kiln-config swap record could not be read back at boot -- heaters "
                         "stay alarmed/disabled; apply a kiln config again to clear this");
        return;
    }
    if (pending_crc(p) != p->crc32) {
        /* H10: a corrupt marker is NOT treated as NONE. We cannot know
         * which side of an in-flight swap this board was on, so the
         * fail-safe default (section 3.4) applies exactly as it would for
         * PICO_OPEN/PICO_DONE: assume the worst, alarm, and do not clear
         * the record automatically -- clearing a record we could not even
         * read would throw away the one piece of evidence an operator
         * could use to understand what happened. The standing divergence
         * check (item 7, safety_ceiling_sync's own reconcile-on-link-up)
         * still runs on every tick regardless and will latch on its own
         * once the link comes up, per its existing contract -- this
         * function's only additional job is to make sure heating is not
         * possible in the meantime, which the "never clear + rely on the
         * pre-existing latched-by-default posture" approach already gives:
         * nothing in this codepath re-arms or clears anything. */
        ESP_LOGE(TAG, "pending swap record failed its own integrity check at boot -- treating as an "
                      "unrecoverable interrupted swap; heaters stay gated by the standing divergence "
                      "check until an operator applies a kiln config again");
        latch_boot_fault(KILN_CFG_SWAP_BOOT_FAULT_UNREADABLE, KILN_CFG_NO_ACTIVE_ID,
                         "an interrupted kiln-config swap record failed its integrity check at boot -- "
                         "heaters stay alarmed/disabled; apply a kiln config again to clear this");
        return;
    }
    SafetyLinkClass *link = s_link;
    char reason[KILN_CFG_SWAP_REASON_MAX];
    reason[0] = '\0';
    switch ((kiln_cfg_swap_marker_t)p->marker) {
    case KILN_CFG_SWAP_MARKER_NONE:
        return;
    case KILN_CFG_SWAP_MARKER_STAGED:
        /* Crashed before PICO_OPEN was persisted. Both sides' config is
         * still R: the one write that can precede PICO_OPEN is step 4's
         * raise-first ceiling (volatile), which is the same as or looser
         * than R's -- permitted by the "abs_max same or looser" rule -- and
         * the standing reconcile-on-link-up lowers it back to match the
         * zones. kiln_cfg_swap_apply_impl() refuses before step 6 if
         * PICO_OPEN cannot be persisted (M2), so a STAGED record never sits
         * behind a Pico that holds target values. Simply discard it; the
         * standing divergence check confirms this normally once the link is
         * up. */
        ESP_LOGW(TAG, "boot: discarding a STAGED swap record (crashed before the Pico config was touched) -- "
                      "both sides are still on the pre-swap config");
        clear_pending();
        return;
    case KILN_CFG_SWAP_MARKER_PICO_OPEN:
    case KILN_CFG_SWAP_MARKER_PICO_DONE:
        /* We do not know which values the Pico actually holds -- re-apply R
         * in full and verify, per section 4.4's own rule: "do not finish
         * the swap" for either of these rows. */
        ESP_LOGE(TAG, "boot: recovering from an interrupted swap (marker=%d) -- re-applying the "
                      "pre-swap config to the Pico and verifying",
                 (int)p->marker);
        if (!link) {
            ESP_LOGE(TAG, "boot: no safety link available to recover -- staying alarmed until one is up");
            latch_boot_fault(KILN_CFG_SWAP_BOOT_FAULT_NO_LINK, p->target_id,
                             "an interrupted kiln-config swap cannot be recovered without the safety "
                             "processor link -- heaters stay alarmed/disabled; check the safety-link "
                             "connection, or reboot once it is up");
            return;
        }
        if (rollback(link, p, /*esp_was_committed=*/(p->marker == KILN_CFG_SWAP_MARKER_PICO_DONE), reason,
                    sizeof(reason))) {
            clear_pending();
            ESP_LOGW(TAG, "boot: interrupted swap recovered -- both sides confirmed back on the pre-swap config");
        } else {
            ESP_LOGE(TAG, "boot: interrupted-swap recovery failed: %s -- staying alarmed, will retry", reason);
            char op_reason[KILN_CFG_SWAP_REASON_MAX + 128];
            snprintf(op_reason, sizeof(op_reason),
                     "an interrupted kiln-config swap could not be rolled back at boot (%.60s) -- heaters "
                     "stay alarmed/disabled; apply a kiln config again to clear this", reason);
            latch_boot_fault(KILN_CFG_SWAP_BOOT_FAULT_ROLLBACK_FAILED, p->target_id, op_reason);
        }
        return;
    case KILN_CFG_SWAP_MARKER_ESP_DONE:
        /* Body lives in finish_esp_done() above, in its own non-inlined
         * frame -- see that function's comment for the stack arithmetic that
         * forced the split. Behaviour is unchanged. */
        finish_esp_done(link, p);
        return;
    default:
        ESP_LOGE(TAG, "boot: pending swap record has an unrecognised marker %u -- treating as unrecoverable",
                 (unsigned)p->marker);
        latch_boot_fault(KILN_CFG_SWAP_BOOT_FAULT_UNRECOGNISED_MARKER, p->target_id,
                         "an interrupted kiln-config swap record has an unrecognised marker -- heaters "
                         "stay alarmed/disabled; apply a kiln config again to clear this");
        return;
    }
}

void kiln_cfg_swap_boot_recover(void)
{
    kiln_cfg_swap_pending_t *p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGE(TAG, "kiln_cfg_swap_boot_recover: heap_caps_malloc(%u B, INTERNAL) failed -- cannot "
                      "check for an interrupted swap this boot",
                 (unsigned)sizeof(*p));
        latch_boot_fault(KILN_CFG_SWAP_BOOT_FAULT_UNREADABLE, KILN_CFG_NO_ACTIVE_ID,
                         "out of memory checking for an interrupted kiln-config swap at boot -- heaters "
                         "stay alarmed/disabled; reboot or apply a kiln config again to retry");
        return;
    }
    kiln_cfg_swap_boot_recover_impl(p);
    heap_caps_free(p);
}
