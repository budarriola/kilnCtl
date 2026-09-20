/* live_profile.c -- see live_profile.h for the design. */

#include "live_profile.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "hal_kv.h"
#include "nvs_key_check.h"
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

bool live_edit_name_collides(const char *candidate_name, const char *(*name_at)(void *ctx, uint8_t id), void *ctx,
                              uint8_t exclude_id, char *err, size_t err_cap)
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

    /* LOW review item: a save-as must also be refused against the read-only
     * builtin catalogue (profiles_builtin.c) -- exclude_id only ever names a
     * USER id (0xFF for a fresh save-as, or a user id for a rename target),
     * so it never accidentally excludes a builtin here. Read-only: this only
     * ever COMPARES against g_builtin_profiles/profiles_builtin_entry(),
     * never writes through them -- a builtin's `code` can never be altered.
     * Ids are contiguous from PROFILE_BUILTIN_ID_BASE (profiles_builtin.c's
     * builtin_index()), so profiles_builtin_id_valid() going false ends the
     * scan; the `bid != 0` guard is only there to stop a uint8_t wraparound
     * from looping forever if that ever stopped being true. */
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

bool live_edit_decide(live_edit_decision_kind_t action, const live_edit_record_t *rec, const char *candidate_name,
                       bool confirm, const char *(*name_at)(void *ctx, uint8_t id), void *ctx, char *err,
                       size_t err_cap)
{
    switch (action) {
    case LIVE_EDIT_DECISION_SAVE_AS:
        /* exclude_id 0xFF -- save-as always targets a fresh slot, never a
         * rename target (that is what OVERWRITE is for). */
        if (live_edit_name_collides(candidate_name, name_at, ctx, 0xFF, err, err_cap)) {
            return false; /* live_edit_name_collides already filled err */
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
 * caller_stack_is_external()-style write-context contract per hal_kv.h --
 * these functions are called from the flash worker / init-time context by
 * every existing caller convention (nvs_save_slot() in profiles_http.c is
 * the direct precedent this mirrors); no separate guard is added here
 * because none of pass 1's own callers (host tests, and pass 2's future HTTP
 * handler which will route through the same worker dispatch every other
 * profile write already uses) run on a PSRAM-backed stack. */

/* Pattern 2 (local caller_stack_is_external() guard, via the SAME shared
 * hal_kv_write_safe_here() predicate kiln_cfg_store.c's/kiln_cfg_swap.c's
 * entries in flash_worker_lint.py's allowlist use -- not a re-derived copy).
 * live_profile_save_record()/_save_working()/_clear() are reached from host
 * tests today and, from pass 2 on, from the profile-edit HTTP handler and
 * profile_executor's own end-of-firing decision path -- neither a PSRAM-
 * stacked task nor the flash worker itself, but refusing loudly here rather
 * than assuming so is exactly the discipline this lint exists to enforce. */
static bool caller_stack_is_external(void)
{
    return !hal_kv_write_safe_here();
}

bool live_profile_save_record(const live_edit_record_t *rec, char *err, size_t err_cap)
{
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
    uint8_t buf[sizeof(live_edit_persisted_t)];
    size_t len = live_edit_record_encode(rec, buf, sizeof(buf));
    kv_err = hal_kv_set_blob(&h, NVS_KEY_LIVE_RECORD, buf, len);
    if (kv_err == HAL_OK) {
        kv_err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (kv_err != HAL_OK) {
        if (err) snprintf(err, err_cap, "live_profile: record write failed (%s)", hal_status_to_name(kv_err));
        return false;
    }

    /* Read-back verification (this file's header doc comment / CLAUDE.md's
     * "logging unchecked success" class -- boot_guard_mark_healthy()'s fix
     * is the precedent). A write whose commit reported HAL_OK but whose
     * read-back disagrees is treated as a failure, not a false success. */
    live_edit_record_t readback;
    if (!live_profile_load_record(&readback) || readback.origin_id != rec->origin_id ||
        readback.working_id != rec->working_id || readback.origin_is_builtin != rec->origin_is_builtin ||
        readback.pending != rec->pending || strncmp(readback.origin_name, rec->origin_name,
                                                      sizeof(readback.origin_name)) != 0) {
        ESP_LOGE(LIVE_PROFILE_TAG, "live_edit_v1 write reported success but read-back did not match -- "
                                    "treating as a failed write");
        if (err) snprintf(err, err_cap, "record write could not be verified by read-back");
        return false;
    }
    return true;
}

bool live_profile_load_record(live_edit_record_t *out)
{
    hal_kv_handle_t h;
    hal_status_t kv_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, LIVE_PROFILE_NVS_PARTITION);
    if (kv_err != HAL_OK) {
        return false;
    }
    uint8_t buf[sizeof(live_edit_persisted_t)];
    size_t len = sizeof(buf);
    kv_err = hal_kv_get_blob(&h, NVS_KEY_LIVE_RECORD, buf, &len);
    hal_kv_close(&h);
    if (kv_err != HAL_OK) {
        return false;
    }
    return live_edit_record_decode(buf, len, out);
}

bool live_profile_save_working(const profile_t *p, char *err, size_t err_cap)
{
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
    uint8_t buf[PROFILE_BLOB_MAX_SIZE];
    size_t len = profile_encode_current_blob(p, buf, sizeof(buf));
    hal_status_t set_err = HAL_OK;
    if (len == 0) {
        set_err = HAL_INVALID_SIZE; /* encode refused -- profile too large for PROFILE_BLOB_MAX_SIZE */
    } else {
        set_err = hal_kv_set_blob(&h, NVS_KEY_LIVE_PROFILE, buf, len);
        if (set_err == HAL_OK) {
            set_err = hal_kv_commit(&h);
        }
    }
    hal_kv_close(&h);
    if (set_err != HAL_OK) {
        if (err) snprintf(err, err_cap, "live_profile: working profile write failed");
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

bool live_profile_load_working(profile_t *out)
{
    hal_kv_handle_t h;
    hal_status_t kv_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, LIVE_PROFILE_NVS_PARTITION);
    if (kv_err != HAL_OK) {
        return false;
    }
    /* Heap-allocated, not a stack local -- same reasoning as
     * profile_executor.c's reload_live_profile_if_changed() candidate blob:
     * this function sits on executor_task_entry's call chain
     * (check_executor_task_stack_budget.ps1), and PROFILE_BLOB_MAX_SIZE is
     * cumulative on top of every other frame in that chain. Established
     * pattern per firing_stats_persist()/firing_stats_load(): heap_caps_malloc
     * + free() on every return path, never a bigger stack. Internal DRAM: this
     * path reads NVS via hal_kv, same reasoning as firing_stats_load()'s own
     * MALLOC_CAP_INTERNAL comment. */
    uint8_t *buf = heap_caps_malloc(PROFILE_BLOB_MAX_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(LIVE_PROFILE_TAG, "live_profile_load_working: malloc(%u) failed",
                 (unsigned)PROFILE_BLOB_MAX_SIZE);
        hal_kv_close(&h);
        return false;
    }
    size_t len = PROFILE_BLOB_MAX_SIZE;
    kv_err = hal_kv_get_blob(&h, NVS_KEY_LIVE_PROFILE, buf, &len);
    hal_kv_close(&h);
    if (kv_err != HAL_OK) {
        free(buf);
        return false;
    }
    const char *reason = "";
    profile_decode_result_t dres = profile_decode_blob(buf, len, out, &reason);
    free(buf);
    return dres == PROFILE_DECODE_OK;
}

/* MEDIUM-1 (review): the pickup-side half of the origin_id guard -- fork()
 * above refuses a NEW fork from adopting a stale pending record, but the
 * RUNNING executor's own poll (profile_executor.c's live-edit pickup) must
 * independently refuse to ADOPT a working copy that isn't for the profile
 * it is currently running, in case a record ever reaches this state some
 * other way than fork() (a future decision-layer bug, a hand-edited NVS
 * blob during bring-up, etc.) -- "compare in fork and in the pickup
 * caller" per the review. Returns false (untouched *out) if there is no
 * pending record, the record isn't pending, or its origin_id does not
 * match expect_origin_id -- all three are "not for this run", indistinguishable
 * to the caller on purpose (see profile_executor.c's reload_live_profile_
 * if_changed(), which treats every one of them as a no-op, not a refusal
 * worth recording). */
bool live_profile_load_working_for_origin(uint8_t expect_origin_id, profile_t *out)
{
    live_edit_record_t rec;
    if (!live_profile_load_record(&rec) || !rec.pending) {
        return false;
    }
    if (rec.origin_id != expect_origin_id) {
        return false;
    }
    return live_profile_load_working(out);
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

bool live_profile_fork(uint8_t origin_id, bool origin_is_builtin, const char *origin_name, const profile_t *origin,
                        profile_t *out_working, live_edit_record_t *out_record, char *err, size_t err_cap)
{
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
        return true;
    }

    if (!live_profile_save_working(origin, err, err_cap)) {
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
    return true;
}

bool live_profile_clear(char *err, size_t err_cap)
{
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
    live_edit_record_t readback;
    if (live_profile_load_record(&readback) && readback.pending) {
        ESP_LOGE(LIVE_PROFILE_TAG, "live_edit_v1 clear reported success but a pending record is still readable");
        if (err) snprintf(err, err_cap, "clear could not be verified by read-back");
        return false;
    }
    atomic_fetch_add(&s_live_profile_generation, 1u);
    return true;
}

uint32_t live_profile_generation(void)
{
    return atomic_load(&s_live_profile_generation);
}
