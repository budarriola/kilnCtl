/* live_profile.c -- see live_profile.h for the design. */

#include "live_profile.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "cfg_fs_status.h"
#include "cfg_save_lock.h"
#include "hal_kv.h"
#include "hal_sysinfo.h"
#include "nvs_key_check.h"
#include "pref_cfg_fs.h"
#include "persist_scratch.h"
#include "profiles_builtin.h" /* live_edit_name_collides() also scans the read-only catalogue -- LOW review item */

/* profile_encode_current_blob()/profile_decode_blob() live in
 * profiles_http_internal.h, which pulls in esp_http_server.h -- NOT
 * host-safe, and test_live_profile.c #includes this whole file directly
 * (host-only translation unit), same reasoning as profile_executor.c's own
 * local forward declaration of profiles_validate_candidate(). Declared
 * locally instead, matching that header's real signatures exactly (types
 * come from profiles_types.h, already visible via live_profile.h). */
#define PROFILE_BLOB_MAX_SIZE (sizeof(profile_t) + 32u)
/* Guarded so a host test that #includes this file directly (test_live_profile.c)
 * can pre-declare an identical shim ahead of this file's own #include (its
 * fake profile_encode_current_blob()/profile_decode_blob() bodies need the
 * type in scope before this file's declaration point) without a duplicate
 * enum-tag redefinition error -- unlike typedef aliases, C does not allow
 * redefining an enum tag twice even when byte-identical. */
#ifndef PROFILE_DECODE_RESULT_SHIM_DECLARED
#define PROFILE_DECODE_RESULT_SHIM_DECLARED
typedef enum {
    PROFILE_DECODE_OK,
    PROFILE_DECODE_CORRUPT,
    PROFILE_DECODE_NEWER,
} profile_decode_result_t;
#endif
profile_decode_result_t profile_decode_blob(const void *blob, size_t len, profile_t *out, const char **err_reason);
size_t profile_encode_current_blob(const profile_t *profile, void *out, size_t cap);

_Static_assert(PROFILE_BLOB_MAX_SIZE <= PREF_CFG_FS_MAX_LARGE_ITEM, "working profile must fit pref_cfg_fs");

static const char *LIVE_PROFILE_TAG = "live_profile";

#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_LIVE_RECORD "live_edit_v1" /* 12 chars */
#define NVS_KEY_LIVE_PROFILE "live_prof"   /* 9 chars */
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_LIVE_RECORD);
NVS_KEY_LEN_CHECK(NVS_KEY_LIVE_PROFILE);

#define LIVE_PROFILE_NVS_PARTITION "profiles_nvs" /* same partition the ordinary profile
                                                     * slots use -- this is still profile
                                                     * data, just under its own key rather
                                                     * than in the fixed 8-slot array. */
NVS_KEY_LEN_CHECK(LIVE_PROFILE_NVS_PARTITION);

/* On-flash record wrapper -- version-prefixed exactly like every other blob
 * this codebase persists (profile_persisted_t, zone_cfg blobs, ...). No CRC:
 * this record is small, fixed-shape, and a corrupt/short read already fails
 * the length check below; a CRC would be one more thing to keep in sync with
 * zero real integrity gain over the length+version check already gating a
 * ~28-byte struct. */
typedef struct {
    uint32_t version;
    uint8_t  origin_id;
    uint8_t  working_id;
    uint8_t  origin_is_builtin;
    uint8_t  pending;
    char     origin_name[PROFILE_NAME_MAX_LEN + 1];
} live_edit_persisted_t;

/* Atomic (LOW review item): live_profile_generation() is read from the
 * executor task while live_profile_fork()/live_profile_save_working()/
 * live_profile_clear() bump it from the HTTP task -- a plain uint32_t
 * increment is not guaranteed atomic on this target, and a torn read/write
 * here is exactly the kind of cross-task counter this codebase already uses
 * _Atomic for (see wifi_provision_http.c's s_httpd_open_sockets). */
static _Atomic uint32_t s_live_profile_generation;

/* A1 (REVIEW_WEB4_TESTS): the web request (profiles_live_http.c) and the LCD task (ui_edit_firing_apply.c) both
 * write the working copy. This lock makes "generation check + verified save + generation bump" one section
 * (live_profile_save_working_if_gen) and serialises clear's bump with it. The generation is RAM-only: it restarts
 * at 0 on reboot, so a client holding a pre-reboot value can match again after that many post-reboot saves. */
static cfg_save_lock_t s_live_save_lock = CFG_SAVE_LOCK_INIT;

/* ---- pure: record encode/decode ------------------------------------------ */

size_t live_edit_record_encode(const live_edit_record_t *rec, void *out, size_t cap)
{
    if (!rec || !out || cap < sizeof(live_edit_persisted_t)) {
        return 0;
    }
    live_edit_persisted_t p;
    memset(&p, 0, sizeof(p));
    p.version = LIVE_EDIT_RECORD_VERSION;
    p.origin_id = rec->origin_id;
    p.working_id = rec->working_id;
    p.origin_is_builtin = rec->origin_is_builtin;
    p.pending = rec->pending;
    strncpy(p.origin_name, rec->origin_name, sizeof(p.origin_name) - 1);
    memcpy(out, &p, sizeof(p));
    return sizeof(p);
}

bool live_edit_record_decode(const void *blob, size_t len, live_edit_record_t *out)
{
    if (!blob || !out || len != sizeof(live_edit_persisted_t)) {
        return false;
    }
    live_edit_persisted_t p;
    memcpy(&p, blob, sizeof(p));
    if (p.version != LIVE_EDIT_RECORD_VERSION) {
        /* Discard, never migrate -- see this file's/header's doc comment on
         * LIVE_EDIT_RECORD_VERSION (the run_state precedent). */
        return false;
    }
    out->version = p.version;
    out->origin_id = p.origin_id;
    out->working_id = p.working_id;
    out->origin_is_builtin = p.origin_is_builtin;
    out->pending = p.pending;
    memset(out->origin_name, 0, sizeof(out->origin_name));
    strncpy(out->origin_name, p.origin_name, sizeof(out->origin_name) - 1);
    return true;
}

/* ---- pure: decision-layer rules ------------------------------------------ */

bool live_edit_can_overwrite(const live_edit_record_t *rec, char *err, size_t err_cap)
{
    if (!rec) {
        if (err) snprintf(err, err_cap, "no live edit in progress");
        return false;
    }
    /* Both checks read server-side state only -- plan section 6. Either one
     * alone would already be correct; both are asserted so a future caller
     * that forgets to set one of the two fields at fork time still gets
     * refused rather than silently trusting the other. */
    if (rec->origin_is_builtin) {
        if (err) snprintf(err, err_cap, "cannot overwrite a builtin profile (id %u is a factory schedule)",
                          rec->origin_id);
        return false;
    }
    if (rec->origin_id >= PROFILES_MAX_COUNT) {
        if (err) snprintf(err, err_cap, "cannot overwrite: origin id %u is outside the user profile range",
                          rec->origin_id);
        return false;
    }
    return true;
}

static void normalize_for_compare(const char *raw, char *out, size_t out_cap)
{
    size_t o = 0;
    size_t i = 0;
    size_t len = raw ? strlen(raw) : 0;
    /* trim leading whitespace */
    while (i < len && (raw[i] == ' ' || raw[i] == '\t')) i++;
    size_t end = len;
    while (end > i && (raw[end - 1] == ' ' || raw[end - 1] == '\t')) end--;
    for (; i < end && o + 1 < out_cap; i++) {
        char c = raw[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[o++] = c;
    }
    out[o] = '\0';
}

bool live_edit_name_collides_ex(const char *candidate_name, const char *(*name_at)(void *ctx, uint8_t id), void *ctx,
                                 uint8_t exclude_id, bool include_builtins, char *err, size_t err_cap)
{
    char norm_candidate[PROFILE_NAME_MAX_LEN + 1];
    normalize_for_compare(candidate_name, norm_candidate, sizeof(norm_candidate));

    if (!name_at) {
        return false;
    }
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        if (id == exclude_id) continue;
        const char *existing = name_at(ctx, id);
        if (!existing) continue;
        char norm_existing[PROFILE_NAME_MAX_LEN + 1];
        normalize_for_compare(existing, norm_existing, sizeof(norm_existing));
        if (strcmp(norm_candidate, norm_existing) == 0) {
            if (err) snprintf(err, err_cap, "a profile named \"%s\" already exists (id %u)", existing, id);
            return true;
        }
    }

    /* Opus review of 5dd23944, finding 1 (BLOCKER): the builtin scan below
     * must be OPT-IN, not unconditional. It was originally added (the "LOW
     * review item" this comment used to describe) reasoning that exclude_id
     * only ever names a USER id, so it could never accidentally exclude a
     * builtin -- true, but that missed that a USER SLOT SAVE naming itself
     * after a builtin's code is not a collision to refuse, it is exactly
     * what "Copy builtin" produces on purpose (profiles_catalog_http.c
     * emits a builtin's `code` as its JSON "name", and profiles_page.html's
     * copyBuiltin() posts that straight back as the new user slot's name).
     * With this scan unconditional, EVERY builtin copy got refused, and any
     * existing user slot already named like a builtin (e.g. restored from a
     * backup taken before this feature existed) could never be edited again
     * -- exclude_id only ever excludes a USER slot, never the builtin whose
     * name it shares. Callers writing a USER SLOT (profiles_http_save(),
     * profile_post_handler(), and both the single-import and batch-import
     * paths in profiles_export_http.c/backup_import.c) now pass
     * include_builtins=false: a user copy of a builtin is a normal, allowed
     * save. Opus review nit N1 widened this to live_edit_decide()'s
     * LIVE_EDIT_DECISION_SAVE_AS path too (see that switch case): it also
     * writes a USER slot, so it now calls live_edit_name_collides_ex()
     * directly with include_builtins=false instead of going through
     * live_edit_name_collides() below. Name uniqueness is enforced across
     * USER slots only, everywhere in this feature.
     *
     * Read-only either way: this only ever COMPARES against
     * g_builtin_profiles/profiles_builtin_entry(), never writes through
     * them -- a builtin's `code` can never be altered. Ids are contiguous
     * from PROFILE_BUILTIN_ID_BASE (profiles_builtin.c's builtin_index()),
     * so profiles_builtin_id_valid() going false ends the scan; the `bid !=
     * 0` guard is only there to stop a uint8_t wraparound from looping
     * forever if that ever stopped being true. */
    if (!include_builtins) {
        return false;
    }
    for (uint8_t bid = PROFILE_BUILTIN_ID_BASE; bid != 0 && profiles_builtin_id_valid(bid); bid++) {
        const builtin_profile_t *b = profiles_builtin_entry(bid);
        if (!b) continue;
        char norm_existing[PROFILE_NAME_MAX_LEN + 1];
        normalize_for_compare(b->code, norm_existing, sizeof(norm_existing));
        if (strcmp(norm_candidate, norm_existing) == 0) {
            if (err) snprintf(err, err_cap, "a builtin schedule named \"%s\" already exists (id %u)", b->code, bid);
            return true;
        }
    }
    return false;
}

bool live_edit_name_collides(const char *candidate_name, const char *(*name_at)(void *ctx, uint8_t id), void *ctx,
                              uint8_t exclude_id, char *err, size_t err_cap)
{
    /* Original always-scan-builtins signature/behavior. As of Opus review
     * nit N1, live_edit_decide()'s SAVE_AS path no longer calls this --
     * every writer of a USER slot now goes through
     * live_edit_name_collides_ex(..., include_builtins=false) directly (see
     * that function's comment above). This wrapper has no remaining
     * production caller; it stays published (declared in live_profile.h)
     * and directly unit-tested so the include_builtins=true arm of
     * live_edit_name_collides_ex() keeps coverage. */
    return live_edit_name_collides_ex(candidate_name, name_at, ctx, exclude_id, true, err, err_cap);
}

bool live_edit_decide(live_edit_decision_kind_t action, const live_edit_record_t *rec, const char *candidate_name,
                       bool confirm, const char *(*name_at)(void *ctx, uint8_t id), void *ctx, char *err,
                       size_t err_cap)
{
    switch (action) {
    case LIVE_EDIT_DECISION_SAVE_AS:
        /* exclude_id 0xFF -- save-as always targets a fresh slot, never a
         * rename target (that is what OVERWRITE is for).
         *
         * Opus review nit N1: SAVE_AS writes a USER slot, exactly like
         * profiles_http_save()/profile_post_handler()/the import paths --
         * a save-as naming itself after a builtin's code (e.g. "Copy
         * builtin" followed by "Save As" reusing the same name) is a
         * normal, allowed save, not a collision. Name uniqueness is
         * enforced across USER slots only, everywhere -- so this now calls
         * live_edit_name_collides_ex() directly with include_builtins=false,
         * matching every other writer instead of the builtin-scanning
         * wrapper. */
        if (live_edit_name_collides_ex(candidate_name, name_at, ctx, 0xFF, false, err, err_cap)) {
            return false; /* live_edit_name_collides_ex already filled err */
        }
        return true;
    case LIVE_EDIT_DECISION_OVERWRITE:
        /* Structural refusal first (plan section 6) -- a builtin origin is
         * refused before confirm is even inspected, so a caller cannot
         * bypass the structural check by also setting confirm. */
        if (!live_edit_can_overwrite(rec, err, err_cap)) {
            return false; /* live_edit_can_overwrite already filled err */
        }
        if (!confirm) {
            if (err) snprintf(err, err_cap, "overwrite requires explicit confirmation (confirm=1)");
            return false;
        }
        return true;
    case LIVE_EDIT_DECISION_DISCARD:
        /* No collision or ownership question -- plan section 5: "Discard
         * deletes the working slot", unconditionally. */
        return true;
    default:
        if (err) snprintf(err, err_cap, "unknown decision action %d", (int)action);
        return false;
    }
}

bool live_edit_should_prompt(const live_edit_record_t *rec, bool executor_running)
{
    /* Plan section 5: "GET /api/profile/live reports pending_decision
     * whenever the record says pending and the executor is not RUNNING."
     * Deliberately does not distinguish DONE/HALTED/FAULTED/a reboot that
     * interrupted the firing -- all of those just mean "not RUNNING", and
     * the plan is explicit that this must be a single uniform condition
     * rather than one that tries to special-case how the run ended. */
    if (!rec) {
        return false;
    }
    return rec->pending != 0 && !executor_running;
}

static bool segments_equal(const profile_segment_t *a, const profile_segment_t *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

bool live_edit_check_window(const profile_t *running, const profile_t *candidate, uint8_t segment_index, char *err,
                             size_t err_cap)
{
    if (!running || !candidate) {
        if (err) snprintf(err, err_cap, "internal error: missing profile");
        return true;
    }
    if (running->zone_mask != candidate->zone_mask) {
        if (err) snprintf(err, err_cap, "zone_mask cannot change while a firing is running");
        return true;
    }
    /* Frozen segments -- already executed, or the shape of every segment
     * before the one currently running. */
    uint8_t frozen_count = segment_index; /* indices 0..segment_index-1 */
    if (frozen_count > running->segment_count) frozen_count = running->segment_count;
    for (uint8_t i = 0; i < frozen_count; i++) {
        if (i >= candidate->segment_count || !segments_equal(&running->segments[i], &candidate->segments[i])) {
            if (err) snprintf(err, err_cap, "segment %u has already run and cannot be changed", i + 1);
            return true;
        }
    }
    /* The running segment itself: seg_kind (and every io_* field, since they
     * are only meaningful together with seg_kind) is frozen; target_c/
     * ramp_c_per_hr/dwell_min may move. */
    if (segment_index < running->segment_count) {
        if (segment_index >= candidate->segment_count) {
            if (err) snprintf(err, err_cap, "segment %u is currently running and cannot be deleted",
                              segment_index + 1);
            return true;
        }
        const profile_segment_t *r = &running->segments[segment_index];
        const profile_segment_t *c = &candidate->segments[segment_index];
        if (r->seg_kind != c->seg_kind || r->io_target != c->io_target || r->io_state != c->io_state ||
            r->io_blocking != c->io_blocking || r->io_leave_on_at_end != c->io_leave_on_at_end) {
            if (err) snprintf(err, err_cap, "segment %u's kind cannot change while it is running",
                              segment_index + 1);
            return true;
        }
    }
    /* On/off rules at or before the running segment are frozen too -- future
     * ones are unconstrained (plan section 2). */
    for (uint8_t i = 0; i < running->on_off_rule_count; i++) {
        const profile_on_off_rule_t *r = &running->on_off_rules[i];
        if (r->segment_index > segment_index) {
            continue; /* future -- unconstrained */
        }
        bool found = false;
        for (uint8_t j = 0; j < candidate->on_off_rule_count && !found; j++) {
            const profile_on_off_rule_t *c = &candidate->on_off_rules[j];
            if (c->segment_index == r->segment_index && c->zone_index == r->zone_index &&
                memcmp(r, c, sizeof(*r)) == 0) {
                found = true;
            }
        }
        if (!found) {
            if (err) snprintf(err, err_cap,
                              "on/off rule for segment %u is at or before the running segment and cannot change",
                              r->segment_index + 1);
            return true;
        }
    }
    return false;
}

/* ---- persistence ----------------------------------------------------------
 * Owner decision 2026-10-07 (docs/CONFIG_FILESYSTEM.md): the pending-decision
 * record and the working profile live in two cfg files (LIVE_PROFILE_RECORD_
 * FILE_PATH / LIVE_PROFILE_WORKING_FILE_PATH, "<4-byte rev><bytes>" via
 * pref_cfg_fs.h). Saves go to the files ONLY. The legacy NVS keys are a
 * read-only fallback while a file is absent (live_profile_start() copies them
 * into cfg once at boot); live_profile_clear() erases them FIRST so a cleared
 * edit can never reappear from the fallback. A save while cfg is unmounted
 * fails with a message naming the cause. The files' revs are read back from
 * the file at save time (next = file rev + 1, or 1), so there is no RAM
 * counter to drift. */

#define LIVE_FS_REC_CAP (sizeof(live_edit_persisted_t))

/* Every save and clear here writes flash: the cfg file (a LittleFS write,
 * which disables the cache exactly like an NVS write) and, for
 * live_profile_clear(), the legacy NVS erase too. Refuse all three from a
 * stack the flash layer cannot write from (hal_kv.h's write-context contract;
 * the same shared hal_kv_write_safe_here() predicate flash_worker_lint.py's
 * allowlist names). */
static bool caller_stack_is_external(void)
{
    return !hal_kv_write_safe_here();
}

static uint32_t next_rev_for(const char *path, size_t cap)
{
    uint8_t *tmp = (uint8_t *)persist_scratch_alloc(cap);
    if (tmp == NULL) {
        return 1;
    }
    size_t len = 0;
    uint32_t rev = 0;
    if (!pref_cfg_fs_load_var(path, tmp, cap, &len, &rev)) {
        rev = 0;
    }
    free(tmp);
    return rev + 1;
}

/* Reads the legacy NVS blob. *opened is false when the partition/namespace
 * itself could not be opened (a TRANSIENT-class failure for the working
 * profile; "nothing there" for the record). */
static hal_status_t nvs_legacy_get(const char *key, void *buf, size_t *len, bool *opened)
{
    hal_kv_handle_t h;
    hal_status_t kv_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, LIVE_PROFILE_NVS_PARTITION);
    if (opened) {
        *opened = (kv_err == HAL_OK);
    }
    if (kv_err != HAL_OK) {
        return kv_err;
    }
    kv_err = hal_kv_get_blob(&h, key, buf, len);
    hal_kv_close(&h);
    return kv_err;
}

static void fill_unmounted_err(char *err, size_t err_cap, const char *what)
{
    if (err) {
        snprintf(err, err_cap,
                 "live_profile: %s not saved -- the cfg filesystem is not mounted or the write failed "
                 "(POST /api/cfgfs/format_confirm if cfg was reported unformatted)", what);
    }
}

bool live_profile_save_record(const live_edit_record_t *rec, char *err, size_t err_cap)
{
    if (caller_stack_is_external()) {
        if (err) snprintf(err, err_cap, "live_profile: refused -- caller stack is not write-safe here");
        return false;
    }
    uint8_t buf[sizeof(live_edit_persisted_t)];
    size_t len = live_edit_record_encode(rec, buf, sizeof(buf));
    if (len == 0) {
        if (err) snprintf(err, err_cap, "live_profile: record encode failed");
        return false;
    }
    uint32_t rev = next_rev_for(LIVE_PROFILE_RECORD_FILE_PATH, LIVE_FS_REC_CAP);
    if (pref_cfg_fs_commit(LIVE_PROFILE_RECORD_FILE_PATH, buf, len, rev, "live-edit record") != ESP_OK) {
        fill_unmounted_err(err, err_cap, "record");
        return false;
    }

    /* Read-back verification (this file's header doc comment / CLAUDE.md's
     * "logging unchecked success" class -- boot_guard_mark_healthy()'s fix
     * is the precedent). A write whose commit reported OK but whose
     * read-back disagrees is treated as a failure, not a false success. */
    live_edit_record_t readback;
    if (!live_profile_load_record(&readback) || readback.origin_id != rec->origin_id ||
        readback.working_id != rec->working_id || readback.origin_is_builtin != rec->origin_is_builtin ||
        readback.pending != rec->pending || strncmp(readback.origin_name, rec->origin_name,
                                                      sizeof(readback.origin_name)) != 0) {
        ESP_LOGE(LIVE_PROFILE_TAG, "live-edit record write reported success but read-back did not match -- "
                                    "treating as a failed write");
        if (err) snprintf(err, err_cap, "record write could not be verified by read-back");
        return false;
    }
    return true;
}

bool live_profile_load_record(live_edit_record_t *out)
{
    uint8_t buf[sizeof(live_edit_persisted_t)];
    size_t len = 0;
    uint32_t rev = 0;
    esp_err_t lerr = pref_cfg_fs_load_var_checked(LIVE_PROFILE_RECORD_FILE_PATH, buf, sizeof(buf), &len, &rev);
    if (lerr == ESP_OK) {
        /* A file that is present is final: a wrong-version/short record is
         * discarded, never papered over by the older legacy NVS copy. */
        return live_edit_record_decode(buf, len, out);
    }
    if (lerr != ESP_ERR_NOT_FOUND) {
        /* K10-09b: the file could not be read (alloc/I/O) -- its state is unknown, so the older legacy
         * NVS copy must not stand in for it. */
        return false;
    }
    len = sizeof(buf);
    if (nvs_legacy_get(NVS_KEY_LIVE_RECORD, buf, &len, NULL) != HAL_OK) {
        return false;
    }
    return live_edit_record_decode(buf, len, out);
}

static bool live_profile_save_working_locked(const profile_t *p, char *err, size_t err_cap)
{
    if (caller_stack_is_external()) {
        if (err) snprintf(err, err_cap, "live_profile: refused -- caller stack is not write-safe here");
        return false;
    }
    uint8_t *buf = (uint8_t *)persist_scratch_alloc(PROFILE_BLOB_MAX_SIZE);
    if (buf == NULL) {
        if (err) snprintf(err, err_cap, "live_profile: out of memory");
        return false;
    }
    size_t len = profile_encode_current_blob(p, buf, PROFILE_BLOB_MAX_SIZE);
    if (len == 0) { /* encode refused -- profile too large for PROFILE_BLOB_MAX_SIZE */
        free(buf);
        if (err) snprintf(err, err_cap, "live_profile: working profile write failed");
        return false;
    }
    uint32_t rev = next_rev_for(LIVE_PROFILE_WORKING_FILE_PATH, PROFILE_BLOB_MAX_SIZE);
    esp_err_t werr = pref_cfg_fs_commit(LIVE_PROFILE_WORKING_FILE_PATH, buf, len, rev, "live-edit working profile");
    free(buf);
    if (werr != ESP_OK) {
        fill_unmounted_err(err, err_cap, "working profile");
        return false;
    }

    /* Read-back verification, same discipline as the record above. */
    profile_t readback;
    if (!live_profile_load_working(&readback) || memcmp(&readback, p, sizeof(readback)) != 0) {
        ESP_LOGE(LIVE_PROFILE_TAG,
                 "live working profile write reported success but read-back did not match -- treating as failed");
        if (err) snprintf(err, err_cap, "working profile write could not be verified by read-back");
        return false;
    }
    atomic_fetch_add(&s_live_profile_generation, 1u);
    return true;
}

bool live_profile_save_working(const profile_t *p, char *err, size_t err_cap)
{
    cfg_save_lock_take(&s_live_save_lock);
    bool ok = live_profile_save_working_locked(p, err, err_cap);
    cfg_save_lock_give(&s_live_save_lock);
    return ok;
}

live_save_result_t live_profile_save_working_if_gen(const profile_t *p, bool check_gen, uint32_t expected_gen,
                                                    uint32_t *out_gen, char *err, size_t err_cap)
{
    cfg_save_lock_take(&s_live_save_lock);
    live_save_result_t r;
    if (check_gen && atomic_load(&s_live_profile_generation) != expected_gen) {
        r = LIVE_SAVE_STALE;
    } else if (live_profile_save_working_locked(p, err, err_cap)) {
        if (out_gen) *out_gen = atomic_load(&s_live_profile_generation); /* produced by THIS save, still under the lock */
        r = LIVE_SAVE_OK;
    } else {
        r = LIVE_SAVE_FAILED;
    }
    cfg_save_lock_give(&s_live_save_lock);
    return r;
}

/* Pass-3 review fix (2026-09-19): internal tri-state core shared by
 * live_profile_load_working() (which only ever needed OK/fail) and
 * live_profile_load_working_for_origin() (which now needs to tell a
 * TRANSIENT failure -- one that may resolve on retry -- apart from a
 * PERMANENT one -- one that never will, so the caller can stop polling for
 * it). Only hal_kv_open() failing (the partition/namespace itself could not
 * be opened) or the decode-buffer malloc failing are TRANSIENT: neither says
 * anything about the blob's own content, and either can plausibly clear on
 * its own (a concurrent NVS commit, memory freed elsewhere) by the next
 * tick. HAL_NOT_FOUND/HAL_INVALID_SIZE from hal_kv_get_blob(), and any
 * profile_decode_blob() failure, are PERMANENT: the blob that exists right
 * now is provably not a loadable working profile, and nothing about a
 * retry with the same stored bytes will change that. `out_reason` receives
 * a short static string for logging; pass NULL to ignore it. */
typedef enum {
    LOAD_WORKING_OK = 0,
    LOAD_WORKING_PERMANENT,
    LOAD_WORKING_TRANSIENT,
} load_working_outcome_t;

static load_working_outcome_t load_working_internal(profile_t *out, const char **out_reason)
{
    const char *reason = "";
    /* Heap-allocated, not a stack local -- same reasoning as
     * profile_executor.c's reload_live_profile_if_changed() candidate blob:
     * this function sits on executor_task_entry's call chain
     * (check_executor_task_stack_budget.ps1), and PROFILE_BLOB_MAX_SIZE is
     * cumulative on top of every other frame in that chain. Established
     * pattern per firing_stats_persist()/firing_stats_load(): heap_caps_malloc
     * + free() on every return path, never a bigger stack. Internal DRAM: the
     * legacy path reads NVS via hal_kv, same reasoning as firing_stats_load()'s
     * own MALLOC_CAP_INTERNAL comment. */
    uint8_t *buf = heap_caps_malloc(PROFILE_BLOB_MAX_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(LIVE_PROFILE_TAG, "live_profile_load_working: malloc(%u) failed",
                 (unsigned)PROFILE_BLOB_MAX_SIZE);
        if (out_reason) *out_reason = "decode buffer malloc failed";
        return LOAD_WORKING_TRANSIENT;
    }
    size_t len = 0;
    uint32_t rev = 0;
    esp_err_t lerr = pref_cfg_fs_load_var_checked(LIVE_PROFILE_WORKING_FILE_PATH, buf, PROFILE_BLOB_MAX_SIZE, &len, &rev);
    if (lerr != ESP_OK && lerr != ESP_ERR_NOT_FOUND) {
        /* K10-09b: unreadable file (alloc/I/O), not an absent one: do not fall back to the older legacy copy. */
        free(buf);
        if (out_reason) *out_reason = "cfg file unreadable";
        return LOAD_WORKING_TRANSIENT;
    }
    if (lerr != ESP_OK) {
        /* No cfg file: the legacy NVS copy (pre-2026-10-07 builds) is the
         * fallback. */
        bool opened = false;
        len = PROFILE_BLOB_MAX_SIZE;
        hal_status_t kv_err = nvs_legacy_get(NVS_KEY_LIVE_PROFILE, buf, &len, &opened);
        if (!opened) {
            free(buf);
            if (out_reason) *out_reason = "hal_kv_open failed";
            return LOAD_WORKING_TRANSIENT;
        }
        if (kv_err != HAL_OK) {
            free(buf);
            if (kv_err == HAL_NOT_FOUND || kv_err == HAL_INVALID_SIZE) {
                if (out_reason) *out_reason = (kv_err == HAL_NOT_FOUND) ? "working blob not found" : "working blob wrong length";
                return LOAD_WORKING_PERMANENT;
            }
            if (out_reason) *out_reason = "hal_kv_get_blob transient error";
            return LOAD_WORKING_TRANSIENT;
        }
    }
    profile_decode_result_t dres = profile_decode_blob(buf, len, out, &reason);
    free(buf);
    if (dres != PROFILE_DECODE_OK) {
        if (out_reason) *out_reason = "working blob failed to decode";
        return LOAD_WORKING_PERMANENT;
    }
    return LOAD_WORKING_OK;
}

bool live_profile_load_working(profile_t *out)
{
    return load_working_internal(out, NULL) == LOAD_WORKING_OK;
}

/* MEDIUM-1 (review): the pickup-side half of the origin_id guard -- fork()
 * above refuses a NEW fork from adopting a stale pending record, but the
 * RUNNING executor's own poll (profile_executor.c's live-edit pickup) must
 * independently refuse to ADOPT a working copy that isn't for the profile
 * it is currently running, in case a record ever reaches this state some
 * other way than fork() (a future decision-layer bug, a hand-edited NVS
 * blob during bring-up, etc.) -- "compare in fork and in the pickup
 * caller" per the review. Returns LIVE_PROFILE_LOAD_NONE_FOR_ORIGIN
 * (untouched *out) if there is no pending record, the record isn't pending,
 * or its origin_id does not match expect_origin_id -- all three are "not for
 * this run" and, per the HIGH fix above, all three are DEFINITIVE (the
 * caller may consume the generation it observed), unlike
 * LIVE_PROFILE_LOAD_TRANSIENT (a record IS pending for this run but its blob
 * failed to load), which must not be. */
live_profile_load_result_t live_profile_load_working_for_origin(uint8_t expect_origin_id, profile_t *out)
{
    live_edit_record_t rec;
    /* HIGH (review): "no record at all" is read through the exact same
     * hal_kv_open()/hal_kv_get_blob() path as a genuine transient error, but
     * per plan it is treated as the definitive, common case here (there is
     * no live edit for ANY run yet) rather than as TRANSIENT -- matching
     * live_profile_has_pending_for_origin()'s treatment of the same
     * !live_profile_load_record() outcome just below. */
    if (!live_profile_load_record(&rec) || !rec.pending || rec.origin_id != expect_origin_id) {
        return LIVE_PROFILE_LOAD_NONE_FOR_ORIGIN;
    }
    const char *reason = "";
    load_working_outcome_t r = load_working_internal(out, &reason);
    switch (r) {
    case LOAD_WORKING_OK:
        return LIVE_PROFILE_LOAD_OK;
    case LOAD_WORKING_PERMANENT:
        /* Pass-3 review fix: logged HERE, once, at the point the permanent
         * fact is actually established -- the caller (profile_executor.c)
         * additionally records this in s_exec.live_edit_last_refusal so an
         * operator polling GET /api/profile/live sees it too, but the raw
         * reason string is only ever available here. */
        ESP_LOGE(LIVE_PROFILE_TAG,
                 "pending live edit for origin %u can never be adopted, discarding this generation: %s",
                 (unsigned)expect_origin_id, reason);
        return LIVE_PROFILE_LOAD_PERMANENT;
    case LOAD_WORKING_TRANSIENT:
    default:
        return LIVE_PROFILE_LOAD_TRANSIENT;
    }
}

/* MEDIUM-2 (review): profile_executor_run.c seeds s_exec.live_edit_generation
 * from live_profile_generation() at the start of every run, including a
 * warm-start resume after a reboot -- but s_live_profile_generation is
 * RAM-only and resets across that reboot, while a pending working copy on
 * disk does not. Seeding straight from the (reset) counter makes that run
 * believe the persisted edit is its own already-seen baseline, so it is
 * never adopted until some UNRELATED later edit bumps the counter again.
 * This lets the caller check, before seeding, whether a pending record
 * already exists for the profile about to run -- see profile_executor_run.c
 * for how the answer is used. */
bool live_profile_has_pending_for_origin(uint8_t origin_id)
{
    live_edit_record_t rec;
    return live_profile_load_record(&rec) && rec.pending && rec.origin_id == origin_id;
}

bool live_profile_fork_gen(uint8_t origin_id, bool origin_is_builtin, const char *origin_name, const profile_t *origin,
                           profile_t *out_working, live_edit_record_t *out_record, uint32_t *out_gen,
                           bool *out_forked, char *err, size_t err_cap)
{
    if (out_forked) *out_forked = false;
    if (!origin || !out_working) {
        if (err) snprintf(err, err_cap, "internal error: missing origin profile");
        return false;
    }

    /* Idempotent: a pending record already on disk wins over re-forking --
     * plan section 10's "second call returns the existing working copy".
     * MEDIUM-1 (review): but ONLY when that pending record is for THIS SAME
     * origin_id. A record left pending by a previous, still-undecided
     * firing (its own operator abandoned the prompt, tripped, or rebooted
     * before answering save-as/overwrite/discard) must never be silently
     * handed to a NEW firing of a DIFFERENT profile as if it were that new
     * firing's own edit -- refuse instead, so the stale record's prompt is
     * what the operator sees, not a fork silently masquerading as one. */
    live_edit_record_t existing;
    if (live_profile_load_record(&existing) && existing.pending) {
        if (existing.origin_id != origin_id) {
            if (err) {
                snprintf(err, err_cap,
                         "a live edit is already pending for a different profile (origin id %u) -- resolve it "
                         "(save-as/overwrite/discard) before starting a new one",
                         existing.origin_id);
            }
            return false;
        }
        if (!live_profile_load_working(out_working)) {
            if (err) snprintf(err, err_cap, "a live edit is pending but its working profile could not be loaded");
            return false;
        }
        if (out_record) *out_record = existing;
        if (out_gen) {
            cfg_save_lock_take(&s_live_save_lock);
            *out_gen = atomic_load(&s_live_profile_generation);
            cfg_save_lock_give(&s_live_save_lock);
        }
        return true;
    }

    /* The generation THIS save produced is captured under the save lock; callers that go on to a
     * compare-and-save (LCD Apply) must use it, never a fresh unlocked read. */
    uint32_t own_gen = 0;
    if (live_profile_save_working_if_gen(origin, false, 0, &own_gen, err, err_cap) != LIVE_SAVE_OK) {
        return false;
    }

    live_edit_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = LIVE_EDIT_RECORD_VERSION;
    rec.origin_id = origin_id;
    rec.working_id = LIVE_EDIT_WORKING_SLOT_ID;
    rec.origin_is_builtin = origin_is_builtin ? 1 : 0;
    rec.pending = 1;
    if (origin_name) {
        strncpy(rec.origin_name, origin_name, sizeof(rec.origin_name) - 1);
    }
    if (!live_profile_save_record(&rec, err, err_cap)) {
        return false;
    }

    *out_working = *origin;
    if (out_record) *out_record = rec;
    if (out_gen) *out_gen = own_gen;
    if (out_forked) *out_forked = true;
    return true;
}

bool live_profile_fork(uint8_t origin_id, bool origin_is_builtin, const char *origin_name, const profile_t *origin,
                       profile_t *out_working, live_edit_record_t *out_record, char *err, size_t err_cap)
{
    return live_profile_fork_gen(origin_id, origin_is_builtin, origin_name, origin, out_working, out_record, NULL,
                                 NULL, err, err_cap);
}

static bool live_profile_clear_locked(char *err, size_t err_cap);

/* REVIEW_WEBFX4 LOW-2: the whole clear (erase + read-back + bump) runs under the live save lock, so a concurrent
 * save cannot recreate the working file between the erase and the bump. */
bool live_profile_clear(char *err, size_t err_cap)
{
    cfg_save_lock_take(&s_live_save_lock);
    bool ok = live_profile_clear_locked(err, err_cap);
    cfg_save_lock_give(&s_live_save_lock);
    return ok;
}

static bool live_profile_clear_locked(char *err, size_t err_cap)
{
    /* Legacy NVS copy first: if the file removal below were to fail or be
     * interrupted, the fallback must not be able to resurrect the edit. */
    if (caller_stack_is_external()) {
        if (err) snprintf(err, err_cap, "live_profile: refused -- caller stack is not write-safe here");
        return false;
    }
    hal_kv_handle_t h;
    hal_status_t kv_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, LIVE_PROFILE_NVS_PARTITION);
    if (kv_err != HAL_OK) {
        if (err) snprintf(err, err_cap, "live_profile: open failed (%s)", hal_status_to_name(kv_err));
        return false;
    }
    hal_status_t e1 = hal_kv_erase_key(&h, NVS_KEY_LIVE_RECORD);
    hal_status_t e2 = hal_kv_erase_key(&h, NVS_KEY_LIVE_PROFILE);
    hal_status_t commit_err = hal_kv_commit(&h);
    hal_kv_close(&h);
    /* NOT_FOUND on either key is fine (idempotent, per header doc comment):
     * there being nothing to erase is success, not failure. */
    if ((e1 != HAL_OK && e1 != HAL_NOT_FOUND) || (e2 != HAL_OK && e2 != HAL_NOT_FOUND) || commit_err != HAL_OK) {
        if (err) snprintf(err, err_cap, "live_profile: clear failed");
        return false;
    }
    /* Then the cfg files. Unmounted cfg cannot hold a live edit (every save
     * was refused), so there is nothing to remove and the clear stands. */
    esp_err_t r1 = pref_cfg_fs_remove(LIVE_PROFILE_RECORD_FILE_PATH);
    esp_err_t r2 = pref_cfg_fs_remove(LIVE_PROFILE_WORKING_FILE_PATH);
    bool r1_bad = (r1 != ESP_OK && r1 != ESP_ERR_INVALID_STATE && r1 != ESP_ERR_NOT_FOUND);
    bool r2_bad = (r2 != ESP_OK && r2 != ESP_ERR_INVALID_STATE && r2 != ESP_ERR_NOT_FOUND);
    if (r1_bad || r2_bad) {
        ESP_LOGE(LIVE_PROFILE_TAG, "live-edit cfg file removal failed (record %s, working %s)",
                 esp_err_to_name(r1), esp_err_to_name(r2));
        if (err) snprintf(err, err_cap, "live_profile: clear failed");
        return false;
    }
    live_edit_record_t readback;
    if (live_profile_load_record(&readback) && readback.pending) {
        ESP_LOGE(LIVE_PROFILE_TAG, "live-edit clear reported success but a pending record is still readable");
        if (err) snprintf(err, err_cap, "clear could not be verified by read-back");
        return false;
    }
    atomic_fetch_add(&s_live_profile_generation, 1u);
    return true;
}

/* One-time boot migration: copies a legacy NVS record / working profile into
 * the cfg files when the matching file is absent (cfg mounted only). The NVS
 * keys stay as a read fallback until live_profile_clear() erases them. */
void live_profile_start(void)
{
    /* REVIEW_WEBFX4 LOW-1: the generation is RAM-only; a random boot seed keeps a pre-reboot client's value from
     * matching a post-reboot save. profile_executor_run.c seeds its baseline from gen-1, which still works. */
    atomic_store(&s_live_profile_generation, hal_sysinfo_random_u32() & 0x3fffffffu);
    uint8_t rbuf[sizeof(live_edit_persisted_t)];
    size_t rlen = 0;
    uint32_t rev = 0;
    /* K10-09b: migrate the legacy NVS copy only when the file is genuinely ABSENT; an unreadable file may
     * hold newer data and must not be overwritten by the older legacy copy. */
    if (pref_cfg_fs_load_var_checked(LIVE_PROFILE_RECORD_FILE_PATH, rbuf, sizeof(rbuf), &rlen, &rev) ==
        ESP_ERR_NOT_FOUND) {
        rlen = sizeof(rbuf);
        live_edit_record_t tmp;
        if (nvs_legacy_get(NVS_KEY_LIVE_RECORD, rbuf, &rlen, NULL) == HAL_OK && live_edit_record_decode(rbuf, rlen, &tmp)) {
            if (pref_cfg_fs_save(LIVE_PROFILE_RECORD_FILE_PATH, rbuf, rlen, 1) == ESP_OK) {
                ESP_LOGI(LIVE_PROFILE_TAG, "migrated the legacy live-edit record into cfg");
            }
        }
    }
    uint8_t *buf = (uint8_t *)persist_scratch_alloc(PROFILE_BLOB_MAX_SIZE);
    if (buf == NULL) {
        return;
    }
    size_t len = 0;
    if (pref_cfg_fs_load_var_checked(LIVE_PROFILE_WORKING_FILE_PATH, buf, PROFILE_BLOB_MAX_SIZE, &len, &rev) ==
        ESP_ERR_NOT_FOUND) {
        len = PROFILE_BLOB_MAX_SIZE;
        profile_t tmp;
        const char *reason = "";
        if (nvs_legacy_get(NVS_KEY_LIVE_PROFILE, buf, &len, NULL) == HAL_OK &&
            profile_decode_blob(buf, len, &tmp, &reason) == PROFILE_DECODE_OK) {
            if (pref_cfg_fs_save(LIVE_PROFILE_WORKING_FILE_PATH, buf, len, 1) == ESP_OK) {
                ESP_LOGI(LIVE_PROFILE_TAG, "migrated the legacy live working profile into cfg");
            }
        }
    }
    free(buf);
}

/* Read-only dual-write status for GET /api/cfgfs (the record file stands for
 * the pair: the working profile is only meaningful beside its record). */
void live_profile_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                       bool *diverged)
{
    uint8_t fb[sizeof(live_edit_persisted_t)];
    size_t flen = 0;
    uint32_t f_rev = 0;
    live_edit_record_t fr;
    bool f_valid = pref_cfg_fs_load_var(LIVE_PROFILE_RECORD_FILE_PATH, fb, sizeof(fb), &flen, &f_rev) &&
                   live_edit_record_decode(fb, flen, &fr);
    uint8_t nb[sizeof(live_edit_persisted_t)];
    size_t nlen = sizeof(nb);
    live_edit_record_t nr;
    bool n_valid = nvs_legacy_get(NVS_KEY_LIVE_RECORD, nb, &nlen, NULL) == HAL_OK &&
                   live_edit_record_decode(nb, nlen, &nr);
    bool content_equal = f_valid && n_valid && flen == nlen && memcmp(fb, nb, flen) == 0;
    if (file_valid) *file_valid = f_valid;
    if (file_rev) *file_rev = f_valid ? f_rev : 0;
    if (nvs_valid) *nvs_valid = n_valid;
    if (nvs_rev) *nvs_rev = 0; /* the NVS record has no rev key */
    if (diverged) *diverged = cfg_fs_status_item_diverged(f_valid, n_valid, content_equal);
}

uint32_t live_profile_generation(void)
{
    return atomic_load(&s_live_profile_generation);
}
