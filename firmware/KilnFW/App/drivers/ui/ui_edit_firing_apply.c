#include "ui_edit_firing_apply.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "live_profile.h"
#include "profile_executor.h"
#include "profiles_builtin.h"
#include "profiles_http.h"
#include "profiles_http_internal.h" /* PROFILE_* bounds, profiles_validate_candidate() --
                                      * the same ones profiles_live_http.c uses.
                                      * Cross-directory include is the existing
                                      * convention (live_profile.c does the same). */

/* Transient profile_t buffers go to PSRAM, same as profiles_live_http.c's own
 * transient buffers: nothing here needs internal RAM, and a profile_t is ~424 B
 * the LVGL task stack should not carry. */
static profile_t *alloc_profile(void)
{
    profile_t *p = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(sizeof(profile_t), MALLOC_CAP_8BIT);
    }
    return p;
}

static bool get_origin_reference(uint8_t origin_id, bool origin_is_builtin, profile_t *out)
{
    return origin_is_builtin ? profiles_builtin_get(origin_id, out) : profiles_http_get(origin_id, out);
}

edit_firing_seg_phase_t edit_firing_seg_phase(uint8_t seg, uint8_t running_seg)
{
    if (seg < running_seg) {
        return EDIT_FIRING_SEG_FINISHED;
    }
    return seg == running_seg ? EDIT_FIRING_SEG_RUNNING : EDIT_FIRING_SEG_UPCOMING;
}

bool edit_firing_seg_editable(const profile_t *p, uint8_t seg, uint8_t running_seg)
{
    if (!p || seg >= p->segment_count || seg >= PROFILE_MAX_SEGMENTS) {
        return false;
    }
    if (edit_firing_seg_phase(seg, running_seg) == EDIT_FIRING_SEG_FINISHED) {
        return false;
    }
    return p->segments[seg].seg_kind == PROFILE_SEG_KIND_ZONE_RAMP;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

bool edit_firing_step(profile_t *p, uint8_t seg, uint8_t running_seg, edit_firing_field_t field, int dir)
{
    if (!edit_firing_seg_editable(p, seg, running_seg)) {
        return false;
    }
    profile_segment_t *s = &p->segments[seg];
    bool up = dir > 0;
    switch (field) {
    case EDIT_FIRING_FIELD_TARGET: {
        float before = s->target_c;
        float v = before + (up ? EDIT_FIRING_TARGET_STEP_C : -EDIT_FIRING_TARGET_STEP_C);
        s->target_c = clampf(v, PROFILE_TARGET_C_MIN, PROFILE_TARGET_C_MAX);
        return s->target_c != before;
    }
    case EDIT_FIRING_FIELD_RAMP: {
        float before = s->ramp_c_per_hr;
        float v = before + (up ? EDIT_FIRING_RAMP_STEP_C_HR : -EDIT_FIRING_RAMP_STEP_C_HR);
        s->ramp_c_per_hr = clampf(v, PROFILE_RAMP_C_PER_HR_MIN, PROFILE_RAMP_C_PER_HR_MAX);
        return s->ramp_c_per_hr != before;
    }
    case EDIT_FIRING_FIELD_DWELL: {
        uint32_t before = s->dwell_min;
        uint32_t v;
        if (up) {
            v = before + EDIT_FIRING_DWELL_STEP_MIN;
            if (v > PROFILE_DWELL_MIN_MAX) {
                v = PROFILE_DWELL_MIN_MAX;
            }
        } else {
            v = before > EDIT_FIRING_DWELL_STEP_MIN ? before - EDIT_FIRING_DWELL_STEP_MIN : 0u;
            if (v > PROFILE_DWELL_MIN_MAX) {
                v = PROFILE_DWELL_MIN_MAX;
            }
        }
        s->dwell_min = v;
        return v != before;
    }
    default:
        return false;
    }
}

bool edit_firing_fields_in_range(const profile_t *p, char *err, size_t err_cap)
{
    if (!p || p->segment_count > PROFILE_MAX_SEGMENTS) {
        snprintf(err, err_cap, "segment count out of range");
        return false;
    }
    for (uint8_t i = 0; i < p->segment_count; i++) {
        const profile_segment_t *s = &p->segments[i];
        if (s->dwell_min > PROFILE_DWELL_MIN_MAX) {
            snprintf(err, err_cap, "segment %u: dwell_min out of range (0-1440)", (unsigned)(i + 1));
            return false;
        }
        if (s->seg_kind != PROFILE_SEG_KIND_ZONE_RAMP) {
            continue;
        }
        /* Written as !(in range) so a NaN is out of range too, same as the parser. */
        if (!(s->target_c >= PROFILE_TARGET_C_MIN && s->target_c <= PROFILE_TARGET_C_MAX)) {
            snprintf(err, err_cap, "segment %u: target_c out of range (%.0f-%.0f)", (unsigned)(i + 1),
                     (double)PROFILE_TARGET_C_MIN, (double)PROFILE_TARGET_C_MAX);
            return false;
        }
        if (!(s->ramp_c_per_hr >= PROFILE_RAMP_C_PER_HR_MIN && s->ramp_c_per_hr <= PROFILE_RAMP_C_PER_HR_MAX)) {
            snprintf(err, err_cap, "segment %u: ramp_c_per_hr out of range (0-1000)", (unsigned)(i + 1));
            return false;
        }
    }
    return true;
}

bool edit_firing_load(profile_t *out, edit_firing_ctx_t *ctx)
{
    if (!out || !ctx) {
        return false;
    }
    profile_executor_live_status_t st;
    profile_executor_get_live_status(&st);
    if (!st.active) {
        return false;
    }
    /* Generation read BEFORE the content, so a save that lands between the two
     * reads makes the page's first Apply refuse rather than overwrite it. */
    ctx->generation = live_profile_generation();
    ctx->origin_id = st.profile_id;
    ctx->running_seg = st.segment_index;

    if (live_profile_has_pending_for_origin(st.profile_id) &&
        live_profile_load_working_for_origin(st.profile_id, out) == LIVE_PROFILE_LOAD_OK) {
        return true;
    }
    return get_origin_reference(st.profile_id, st.profile_id >= PROFILES_MAX_COUNT, out);
}

bool edit_firing_apply(const profile_t *candidate, edit_firing_ctx_t *ctx, char *err, size_t err_cap)
{
    if (!candidate || !ctx || !err || err_cap == 0) {
        return false;
    }
    err[0] = '\0';

    profile_executor_live_status_t st;
    profile_executor_get_live_status(&st);
    if (!st.active) {
        snprintf(err, err_cap, "no active firing");
        return false;
    }
    if (st.profile_id != ctx->origin_id) {
        snprintf(err, err_cap, "a different firing is running -- reopen");
        return false;
    }
    if (live_profile_generation() != ctx->generation) {
        snprintf(err, err_cap, "edited elsewhere -- reopen to reload");
        return false;
    }

    /* 1. The parser's ranges (the HTTP route gets these from
     *    profiles_parse_profile_fields(); the LCD has no form to parse). */
    if (!edit_firing_fields_in_range(candidate, err, err_cap)) {
        return false;
    }
    /* 2. Same bounds validator, same mode, as the HTTP accept route. */
    if (!profiles_validate_candidate(candidate, PROFILE_VALIDATE_HARD, NULL, 0, err, err_cap)) {
        if (!err[0]) {
            snprintf(err, err_cap, "invalid");
        }
        return false;
    }

    /* 3. Window check against the same origin reference the HTTP route uses.
     * origin_is_builtin defaults to the same id-range test the HTTP fork
     * route (profiles_live_http.c) uses, and is overridden from the
     * persisted live_edit_v1 record ONLY when that record actually belongs
     * to THIS origin -- live_profile_load_record() returns whatever record
     * is on flash with no origin filtering of its own, so a stale record
     * left behind by a different profile's edit (e.g. after a discard that
     * predates this firing) must never be allowed to steer this firing's
     * fork/window-check decision. */
    bool origin_is_builtin = (st.profile_id >= PROFILES_MAX_COUNT);
    live_edit_record_t rec;
    if (live_profile_load_record(&rec) && rec.origin_id == st.profile_id) {
        origin_is_builtin = rec.origin_is_builtin;
    }
    profile_t *origin = alloc_profile();
    if (!origin) {
        snprintf(err, err_cap, "out of memory");
        return false;
    }
    bool have_origin = get_origin_reference(st.profile_id, origin_is_builtin, origin);
    if (have_origin && live_edit_check_window(origin, candidate, st.segment_index, err, err_cap)) {
        heap_caps_free(origin);
        if (!err[0]) {
            snprintf(err, err_cap, "window violation");
        }
        return false;
    }

    /* 4. Fork only now, with every check passed -- same inputs as the fork
     *    route (origin name: slot name, or the builtin's code). */
    if (!live_profile_has_pending_for_origin(st.profile_id)) {
        if (!have_origin) {
            heap_caps_free(origin);
            snprintf(err, err_cap, "origin profile not readable");
            return false;
        }
        char origin_name[PROFILE_NAME_MAX_LEN + 1] = {0};
        if (origin_is_builtin) {
            const builtin_profile_t *bp = profiles_builtin_entry(st.profile_id);
            if (bp) {
                strncpy(origin_name, bp->code, sizeof(origin_name) - 1);
            }
        } else {
            strncpy(origin_name, origin->name, sizeof(origin_name) - 1);
        }
        profile_t *fork_out = alloc_profile();
        bool forked = fork_out && live_profile_fork(st.profile_id, origin_is_builtin, origin_name, origin, fork_out,
                                                     &rec, err, err_cap);
        if (fork_out) {
            heap_caps_free(fork_out);
        }
        if (!forked) {
            heap_caps_free(origin);
            if (!err[0]) {
                snprintf(err, err_cap, fork_out ? "fork failed" : "out of memory");
            }
            return false;
        }
    }
    heap_caps_free(origin);

    /* 5. Save -- bumps live_profile_generation(), which the executor polls
     *    (reload_live_profile_if_changed()), exactly as a web save does. */
    if (!live_profile_save_working(candidate, err, err_cap)) {
        if (!err[0]) {
            snprintf(err, err_cap, "save failed");
        }
        return false;
    }
    ctx->generation = live_profile_generation();
    ctx->running_seg = st.segment_index;
    return true;
}

void edit_firing_poll(const edit_firing_ctx_t *ctx, uint32_t applied_generation, edit_firing_poll_t *out)
{
    memset(out, 0, sizeof(*out));
    profile_executor_live_status_t st;
    profile_executor_get_live_status(&st);
    if (!st.active) {
        out->state = EDIT_FIRING_POLL_ENDED;
        return;
    }
    out->running_seg = st.segment_index;
    if (st.profile_id != ctx->origin_id) {
        out->state = EDIT_FIRING_POLL_OTHER_FIRING;
        return;
    }
    out->state = (live_profile_generation() != ctx->generation) ? EDIT_FIRING_POLL_EDITED_ELSEWHERE
                                                                : EDIT_FIRING_POLL_OK;
    if (applied_generation != 0 && st.has_refusal && st.refusal_generation == applied_generation) {
        out->refused = true;
        snprintf(out->refusal_msg, sizeof(out->refusal_msg), "%s", st.refusal_err_msg);
    }
}
