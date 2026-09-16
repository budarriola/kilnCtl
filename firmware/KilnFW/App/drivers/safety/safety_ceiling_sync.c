#include "safety_ceiling_sync.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h" /* MEDIUM 5 -- s_standing_warning_active/_reason (and the ceiling-
                               * divergence pair alongside them) are written by safety_poll_task
                               * and read lock-free by the httpd and LVGL tasks; same lazy-mutex
                               * convention as kiln_cfg_store.c's s_swap_lock. */

#include "MAX31856.h"
#include "config_divergence.h"
#include "hal_time.h" /* hal_time_now_us() -- HAL_INCLUDE_BOUNDARY: this file must not include esp_timer.h directly */
/* 2026-09-15 review follow-up (item G), NOW FULLY CLOSED (2026-09-16). The
 * finding was that a safety/ module including an http/ header is a layering
 * inversion. Two parts:
 *
 *  - The recent-ARMED-refusal timestamp was a bare file static in
 *    safety_cfg_http.c read through that header; it moved to
 *    safety_cfg_store (the natural owner of Pico-config state -- the HTTP
 *    handler writes it, this file reads it) on 2026-09-15.
 *
 *  - The remaining reason for the include was pico_ceiling_writer() below,
 *    which calls the stage/commit/confirm-by-read-back machinery this file
 *    must not reimplement. That machinery has now MOVED OUT of http/ into
 *    drivers/safety/safety_cfg_write.c, where it belongs: a set-and-confirm
 *    primitive over the safety link is safety-layer work, and http/ is now
 *    one of its callers. The http/ include is gone as a result.
 *
 * Why this mattered: an earlier attempt to close this by simply deleting
 * the #include left the call with no prototype in scope, C assumed "extern
 * returning int", the float target went through default argument promotion,
 * and the Pico ceiling was written as 0. Only test_zones_http.c's reconcile
 * test caught it (target read back 0.0 instead of 1200.0); the compiler
 * merely warned. That warning is now an ERROR on this component
 * (-Werror=implicit-function-declaration, App/drivers/CMakeLists.txt), so
 * the same mistake cannot compile again here or anywhere else in App/. */
#include "safety_cfg_write.h"
#include "safety_cfg_store.h"
#include "zones_config_accessors.h"

static const char *TAG = "safety_ceiling_sync";

/* 2026-09-10 opus review finding A -- see safety_ceiling_policy.h's own doc
 * comment on safety_ceiling_reconcile_backoff_t for the full rationale. One
 * instance is correct here: this file is the single ESP-side reconcile
 * caller (safety_link_poll.c's safety_update_health() is the only caller of
 * safety_ceiling_sync_reconcile_on_link_up(), and there is exactly one
 * safety link / one Pico this boot). */
static safety_ceiling_reconcile_backoff_t s_reconcile_backoff = { 0 };

/* Rate-limits the WARN log below, separately from the write-retry backoff
 * itself -- same "never silent, but never a line every attempt" discipline
 * as SAFETY_CFG_STORE_REFETCH_LOG_INTERVAL_US (safety_cfg_store.c). Kept
 * independent of the retry backoff on purpose: a future tuning pass could
 * shorten the retry backoff without also having to re-derive a sane log
 * cadence. */
#define SAFETY_CEILING_SYNC_LOG_INTERVAL_US ((int64_t)30 * 1000 * 1000)
static int64_t s_last_log_us = -SAFETY_CEILING_SYNC_LOG_INTERVAL_US; /* so the very first failure logs immediately */
static uint32_t s_suppressed_log_count = 0;

/* 2026-09-14 owner decision, verbatim: "if a config doesn't land and match
 * on both sides then alarm and dissable heaters." Separate log cadence from
 * the reconcile WARN above -- this is a distinct, louder condition (ERROR,
 * not WARN) and must not be suppressed just because the reconcile's own log
 * happens to be in its quiet window. Still rate-limited (same discipline as
 * every other repeating log in this file) so a persistent divergence does
 * not spam the log at ~2 Hz forever -- the ACTIVE enforcement below (all
 * relays off, halt any run) happens on EVERY tick regardless of this log's
 * cadence, since a skipped log line is merely a missed notification but a
 * skipped enforcement call would be a real safety gap. */
#define CONFIG_DIVERGENCE_LOG_INTERVAL_US ((int64_t)30 * 1000 * 1000)
static int64_t s_divergence_last_log_us = -CONFIG_DIVERGENCE_LOG_INTERVAL_US;
static bool s_divergence_active = false;
static char s_divergence_reason[CONFIG_DIVERGENCE_REASON_MAX] = { 0 };

/* 2026-09-15 review (docs/audits/review_divergence_check_561efa3b_2026-09-15.md,
 * HIGH 2): kiln_cfg_store_apply() imports only the ESP half and switches the
 * active slot without ever pushing the Pico half over kilnlink -- there is no
 * production path today that confirms a push of the ~60 broadened fields the
 * way the dedicated abs_max_temp_c field is confirmed by pico_ceiling_writer()
 * above (stage+commit+readback). Until that push path exists (0x2D/
 * APPLY_CONFIG_VOLATILE has no success ACK; kiln_cfg_swap_apply() does confirm
 * by readback but is not yet the caller of an ordinary apply), enforcing
 * heat-off on a mismatch in those broadened fields would trip on every
 * ordinary apply whose captured Pico half merely predates the live Pico
 * value -- a false positive, not a real divergence. Interim scope, explicitly
 * chosen over the "push properly" alternative because that push is a
 * substantially larger change: heat-off enforcement (this flag,
 * s_divergence_active/`safety_ceiling_sync_is_diverged()`) stays SCOPED TO
 * abs_max_temp_c ONLY, exactly as before the broadened check landed. A
 * mismatch confined to the broadened (non-ceiling) field set is still
 * detected and reported -- see s_standing_warning_active below -- as a
 * WARNING, never as a heat-disabling condition. This narrowing never disarms
 * the Pico and never changes the abs_max_temp_c-must-match rule; it only
 * changes which mismatch is severe enough to force heat off. */
static bool s_standing_warning_active = false;
static char s_standing_warning_reason[CONFIG_DIVERGENCE_REASON_MAX] = { 0 };
static int64_t s_standing_warning_last_log_us = -CONFIG_DIVERGENCE_LOG_INTERVAL_US;

/* 2026-09-15 review (review_divergence_fixes_b2e7017f_2026-09-15.md,
 * MEDIUM 3): see safety_ceiling_sync_latch_evaluated_generation()'s own doc
 * comment (safety_ceiling_sync.h). Updated ONLY at the bottom of
 * enforce_ceiling_divergence(), after both latches above have been
 * (re)computed against whatever safety_cfg_store cache generation was
 * current at the top of that same call. */
/* 2026-09-15 review follow-up (item F): this stamp is part of the same
 * published snapshot as the two latches -- a reader compares it against
 * safety_cfg_store_cache_generation() to decide whether the latch it just
 * read was evaluated against the current cache. Writing it outside the lock
 * that publishes the latches let a reader observe a NEW stamp beside an OLD
 * latch pair (or the reverse), which is exactly the staleness the stamp
 * exists to rule out. It is now written and read under
 * s_divergence_state_lock like everything else in the snapshot. */
static uint32_t s_latch_evaluated_generation = 0;

/* Forward decls -- defined with the rest of the lock machinery just below.
 * Needed here because the accessor above them must take the lock too (item
 * F). */
static void divergence_state_lock_take(void);
static void divergence_state_lock_give(void);

uint32_t safety_ceiling_sync_latch_evaluated_generation(void)
{
    divergence_state_lock_take();
    uint32_t gen = s_latch_evaluated_generation;
    divergence_state_lock_give();
    return gen;
}

static safety_ceiling_disable_heat_fn s_disable_all_relays_off = NULL;
static safety_ceiling_disable_heat_fn s_disable_halt_run = NULL;

void safety_ceiling_sync_set_disable_heat_hooks(safety_ceiling_disable_heat_fn all_relays_off,
                                                 safety_ceiling_disable_heat_fn halt_run)
{
    s_disable_all_relays_off = all_relays_off;
    s_disable_halt_run = halt_run;
}

/* See safety_ceiling_sync.h's doc comment on this seam for the full
 * rationale (2026-09-15 audit fix, Defect 2). NULL/no-op default -- every
 * existing host test leaves this unset and keeps comparing abs_max_temp_c
 * only, unchanged from before this fix. */
static safety_ceiling_expected_pico_fields_fn s_expected_pico_fields_source = NULL;

void safety_ceiling_sync_set_expected_pico_fields_source(safety_ceiling_expected_pico_fields_fn fn)
{
    s_expected_pico_fields_source = fn;
}

/* MEDIUM 5 -- lazily-created lock guarding the four divergence-state fields
 * (s_divergence_active/_reason, s_standing_warning_active/_reason) against
 * the cross-task read/write race: safety_poll_task writes them (via
 * enforce_ceiling_divergence() below), while the httpd task
 * (dashboard_status_http.c) and the LVGL task (ui_page_home_refresh.c) read
 * them lock-free today. Never held across a producer or blocking call --
 * every critical section below is a plain snprintf/bool copy. */
static SemaphoreHandle_t s_divergence_state_lock;

static void divergence_state_lock_take(void)
{
    if (!s_divergence_state_lock) {
        s_divergence_state_lock = xSemaphoreCreateMutex();
    }
    if (s_divergence_state_lock) {
        xSemaphoreTake(s_divergence_state_lock, portMAX_DELAY);
    }
}

static void divergence_state_lock_give(void)
{
    if (s_divergence_state_lock) {
        xSemaphoreGive(s_divergence_state_lock);
    }
}

bool safety_ceiling_sync_is_diverged(char *reason_out, size_t reason_cap)
{
    divergence_state_lock_take();
    if (reason_out && reason_cap > 0) {
        snprintf(reason_out, reason_cap, "%s", s_divergence_reason);
    }
    bool active = s_divergence_active;
    divergence_state_lock_give();
    return active;
}

/* 2026-09-15 review HIGH 2 interim (see s_standing_warning_active's own doc
 * comment above): true iff the broadened (non-ceiling) field set currently
 * disagrees, even though abs_max_temp_c itself still matches and heat is NOT
 * being forced off for this reason. Exposed so a caller that wants the fuller
 * picture (kiln_cfg_store.c's autosave gate, per plan sec 2.4 rule 6 --
 * autosave must not launder ANY known disagreement into a "consistent" saved
 * state, not only a heat-disabling one) can see this without the readiness/
 * enforcement callers above (which intentionally only ever asked about the
 * heat-disabling ceiling verdict) having their contract changed under them. */
bool safety_ceiling_sync_is_standing_diverged(char *reason_out, size_t reason_cap)
{
    divergence_state_lock_take();
    if (reason_out && reason_cap > 0) {
        snprintf(reason_out, reason_cap, "%s", s_standing_warning_reason);
    }
    bool active = s_standing_warning_active;
    divergence_state_lock_give();
    return active;
}

/* The writer callback safety_ceiling_policy.c calls. `ctx` is the
 * SafetyLinkClass* to write through (may be NULL -- handled below, callers
 * of THIS file never reach a NULL-link writer call because both entry
 * points short-circuit on `!link` first). Reuses safety_cfg_write.c's
 * apply_pairs()/confirm_commit_landed() machinery via its public wrapper --
 * stage, commit, and a live read-back that proves the value landed, never
 * a bare ACK. */
static bool pico_ceiling_writer(void *ctx, float target_c, char *reason_out, size_t reason_cap,
                                 safety_ceiling_refusal_class_t *out_class)
{
    SafetyLinkClass *link = (SafetyLinkClass *)ctx;
    return safety_cfg_write_set_and_confirm_f32(link, SAFETY_PARAM_ID_ABS_MAX_TEMP_C, target_c, reason_out,
                                                reason_cap, out_class);
}

bool safety_ceiling_sync_get_current_pico_ceiling(float *out_value)
{
    size_t count = safety_cfg_store_param_count();
    for (size_t i = 0; i < count; i++) {
        safety_cfg_param_t row;
        if (!safety_cfg_store_get_by_index(i, &row)) {
            continue;
        }
        if (row.param_id == SAFETY_PARAM_ID_ABS_MAX_TEMP_C) {
            if (!row.set) {
                return false;
            }
            if (out_value) {
                *out_value = row.value.f32_val;
            }
            return true;
        }
    }
    return false;
}

bool safety_ceiling_sync_guard_raise(SafetyLinkClass *link, const float *new_max_temp_c, size_t n,
                                      safety_ceiling_sync_result_t *out_result, char *reason_out,
                                      size_t reason_cap, safety_ceiling_refusal_class_t *out_refusal_class)
{
    if (!link) {
        if (out_result) {
            *out_result = SAFETY_CEILING_SYNC_NONE;
        }
        if (reason_out && reason_cap > 0) {
            reason_out[0] = '\0';
        }
        return true; /* no safety processor this boot -- nothing to guard */
    }
    float current = 0.0f;
    bool known = safety_ceiling_sync_get_current_pico_ceiling(&current);
    return safety_ceiling_policy_guard_raise(current, known, new_max_temp_c, n, pico_ceiling_writer, link,
                                              out_result, reason_out, reason_cap, out_refusal_class);
}

void safety_ceiling_sync_apply_lower(SafetyLinkClass *link, const float *new_max_temp_c, size_t n,
                                      safety_ceiling_sync_result_t *out_result, char *reason_out,
                                      size_t reason_cap)
{
    if (!link) {
        if (out_result) {
            *out_result = SAFETY_CEILING_SYNC_NONE;
        }
        if (reason_out && reason_cap > 0) {
            reason_out[0] = '\0';
        }
        return;
    }
    float current = 0.0f;
    bool known = safety_ceiling_sync_get_current_pico_ceiling(&current);
    safety_ceiling_policy_apply_lower(current, known, new_max_temp_c, n, pico_ceiling_writer, link, out_result,
                                       reason_out, reason_cap);
}

/* 2026-09-14 owner decision. Reads the SAME target/current-ceiling facts the
 * reconcile above just computed (never re-derived independently -- exactly
 * the "one owning function" discipline CLAUDE.md's "reset one side of a
 * pair" note asks for) and runs them through config_divergence_check(), the
 * reusable comparator. On divergence: logs ERROR (rate-limited, see above)
 * and ACTIVELY disables heat -- kiln_io_owner_command_all_relays_off() (the
 * one sanctioned "relays off now" entry point; see CLAUDE.md's "Bypassed
 * owner module" note on why this must never be a direct relay write) and
 * profile_executor_halt() (so a RUNNING firing does not immediately try to
 * re-energize on its own next tick -- an all-relays-off with the executor
 * still RUNNING is exactly the "reset one side, not the other" shape this
 * function exists to avoid). Both calls are safe to make when nothing is
 * running or already off -- see their own doc comments.
 *
 * There is no separate "clear the alarm" action: this function is called
 * every tick and simply stops calling the disable path (and stops logging)
 * the moment config_divergence_check() reports no divergence -- which can
 * only happen once a live read-back confirms both sides agree, since
 * `target_c`/`pico_c` here are exactly what the readiness page and the
 * reconcile above just fetched/computed. There is nothing for an operator
 * to dismiss; the condition cannot be masked, only actually fixed.
 *
 * RECORDED, NOT A DEFECT (2026-09-14 opus review,
 * docs/audits/pico_ceiling_mirror_and_rate_guard_2026-09-14.md item D, and
 * docs/audits/divergence_message_and_enforcement_test_2026-09-14.md):
 *
 *  1. This function runs on EVERY tick of safety_ceiling_sync_reconcile_
 *     on_link_up(), which runs on EVERY tick of safety_poll_task -- the
 *     ESP->Pico liveness heartbeat. A board stuck permanently diverged
 *     therefore issues one relays-off/halt-run owner RPC per tick, forever,
 *     for as long as the divergence persists. Deliberate: the alternative
 *     (calling the disable path only on the diverged->not-diverged edge)
 *     would leave a WINDOW after some other code path re-energizes relays
 *     mid-divergence with nothing to immediately re-disable them until the
 *     next edge. A steady stream of "make it so" RPCs to an owner that is
 *     almost always already in the requested state is judged an acceptable
 *     cost against that gap.
 *  2. profile_executor_halt() (like kiln_io_owner_command_all_relays_off())
 *     is invoked from inside this call chain, which importantly means a
 *     persistently diverged board blocks safety_poll_task itself on
 *     whatever lock profile_executor_halt() takes (profile_executor.c's
 *     `s_exec.lock`, a portMAX_DELAY/blocking take) for as long as that
 *     lock is held elsewhere. Since safety_poll_task is also the ESP side
 *     of the safety link's liveness heartbeat, a late heartbeat during that
 *     window can trip the Pico's own S6b (link-dead) guard.
 *
 * BOTH judged deliberate and fail-safe, not bugs: heat is already forced
 * off by the very divergence that is doing the blocking, so a heartbeat
 * that arrives late (or an S6b trip that cuts power independently) cannot
 * make anything less safe than it already is -- it can only ever add a
 * second, independent reason heat stays off. Recorded here explicitly so
 * neither is later mistaken for an unnoticed fault: a diverged board that
 * ALSO shows S6b trips, or that logs late/dropped heartbeats, is not a new
 * symptom to chase -- it is this coupling working as designed. */
static void enforce_ceiling_divergence(float target_c, bool target_known, float pico_c, bool pico_known)
{
    /* Snapshot the cache generation BEFORE reading any live safety_cfg_store
     * rows below -- see safety_ceiling_sync_latch_evaluated_generation()'s
     * doc comment. Recorded even on the early "no target yet" return path
     * (still stamped just above that return) so a caller never observes an
     * artificially stale generation number while genuinely nothing has
     * changed. */
    uint32_t cache_gen_at_entry = safety_cfg_store_cache_generation();
    if (!target_known) {
        /* No zone has a positive max_temp_c -- the ESP itself has no
         * ceiling opinion yet (a fresh/all-zero config). There is nothing
         * to compare (same "do not invent a target" rule safety_ceiling_
         * policy_target_c() already follows), so this is NOT a divergence
         * -- a never-configured board must not alarm and disable heat on
         * every tick before anyone has ever set a zone ceiling. Precision
         * matters here: a check that fires on a benign, common state is
         * exactly the "nuisance check gets switched off" the owner warned
         * against. */
        divergence_state_lock_take();
        s_divergence_active = false;
        s_divergence_reason[0] = '\0';
        s_standing_warning_active = false;
        s_standing_warning_reason[0] = '\0';
        /* Item F: stamped inside the same critical section that publishes
         * the latches above, so the pair is always mutually consistent. */
        s_latch_evaluated_generation = cache_gen_at_entry;
        divergence_state_lock_give();
        return;
    }
    /* abs_max_temp_c is field 0, always present -- see config_divergence.h's
     * top comment on CONFIG_IDENTITY_FORMAT_VERSION. `esp_fields` is this
     * ESP's own live, authoritative target (freshly recomputed by the
     * caller, never a cached push); `pico_fields` is the value most
     * recently FETCHED from the Pico's own GET_CONFIG_PAGE report
     * (safety_cfg_store's cache) -- never an echo of a value this file
     * itself just wrote. See config_divergence.h's own doc comment on this
     * distinction.
     *
     * 2026-09-15 audit fix (Defect 2): fields 1.. are the OTHER ~60+
     * commissioning params, broadened in via the injected safety_ceiling_
     * sync_set_expected_pico_fields_source() seam so this file keeps no
     * hard dependency on the kiln-profiles persist layer (see that seam's
     * doc comment in the header). `static` (file-scope), not stack-local:
     * these two arrays are ~1.5KB combined (96 config_identity_field_t
     * entries x 2, each holding a `const char *name` + bool + float) and
     * this function is provably only ever called from safety_poll_task
     * (see safety_ceiling_sync_reconcile_on_link_up()'s own comment on
     * `now_us` for the identical single-caller/no-reentrancy reasoning) --
     * a stack local of this size risks tripping check_httpd_task_stack_
     * budget/check_executor_task_stack_budget the way the audit's rejected
     * hazard-1 fix attempt did. */
    static config_identity_field_t esp_fields[1 + SAFETY_CEILING_SYNC_MAX_STANDING_FIELDS];
    static config_identity_field_t pico_fields[1 + SAFETY_CEILING_SYNC_MAX_STANDING_FIELDS];
    static safety_ceiling_expected_param_t expected[SAFETY_CEILING_SYNC_MAX_STANDING_FIELDS];
    /* Field names must outlive config_divergence_check()'s call below --
     * these are static storage too, one small fixed-width buffer per
     * possible extra field, formatted once per tick from the live param
     * table (safety_cfg_store_lookup() names are compile-time string
     * literals themselves, but a param this build's mirror table does not
     * recognize still needs SOME name for the reason string). */
    static char extra_names[SAFETY_CEILING_SYNC_MAX_STANDING_FIELDS][24];

    esp_fields[0].name = "abs_max_temp_c";
    esp_fields[0].known = target_known;
    esp_fields[0].value = target_c;

    pico_fields[0].name = "abs_max_temp_c";
    pico_fields[0].known = pico_known;
    pico_fields[0].value = pico_c;

    size_t n = 1;
    size_t extra_count = s_expected_pico_fields_source
                             ? s_expected_pico_fields_source(expected, SAFETY_CEILING_SYNC_MAX_STANDING_FIELDS)
                             : 0;
    if (extra_count > SAFETY_CEILING_SYNC_MAX_STANDING_FIELDS) {
        extra_count = SAFETY_CEILING_SYNC_MAX_STANDING_FIELDS; /* defensive; the seam's own contract already caps this */
    }
    for (size_t i = 0; i < extra_count; i++) {
        uint16_t param_id = expected[i].param_id;
        if (param_id == SAFETY_PARAM_ID_ABS_MAX_TEMP_C) {
            continue; /* never duplicate the dedicated field above */
        }
        uint8_t type;
        const char *name = NULL;
        bool known_by_table = safety_cfg_store_lookup(param_id, &type, &name);
        if (known_by_table && name) {
            snprintf(extra_names[n - 1], sizeof(extra_names[n - 1]), "%s", name);
        } else {
            snprintf(extra_names[n - 1], sizeof(extra_names[n - 1]), "param_0x%04x", (unsigned)param_id);
        }

        esp_fields[n].name = extra_names[n - 1];
        esp_fields[n].known = true;
        esp_fields[n].value = expected[i].value;

        /* Live value: linear scan of safety_cfg_store's cache, same pattern
         * kiln_cfg_swap.c's pico_readback_matches() already uses at swap
         * time -- this is that same comparator, run every tick instead of
         * only at swap time. */
        bool live_known = false;
        float live_value = 0.0f;
        /* 2026-09-15 review (review_divergence_check_561efa3b_2026-09-15.md,
         * MEDIUM 5): a stale cache (config_crc known to disagree with the
         * live Pico's, refetch not yet caught up -- e.g. right after a Pico
         * reboot, or mid-backoff on a bad link) must never be read as if it
         * still agrees. safety_cfg_store_refetch_locked() leaves the
         * PREVIOUS cache contents in place on a failed fetch, so every row
         * would otherwise keep reporting the pre-reboot/pre-revert value as
         * confidently SET forever, silently hiding a real revert instead of
         * flagging it as the unknown it actually is. Scoped to this
         * broadened/extra (WARNING-only) field set only -- the ceiling field
         * above has the identical weakness but predates this fix and is left
         * alone here, per the audit's own note that it is a pre-existing,
         * separate issue. */
        bool cache_stale = safety_cfg_store_cache_is_stale();
        size_t count = safety_cfg_store_param_count();
        for (size_t idx = 0; idx < count; idx++) {
            safety_cfg_param_t row;
            if (safety_cfg_store_get_by_index(idx, &row) && row.param_id == param_id) {
                if (row.set && !cache_stale) {
                    live_known = true;
                    /* Type-aware decode -- same switch kiln_cfg_swap.c's
                     * pico_readback_matches() uses; non-float params are
                     * widened into `float` for the shared identity
                     * comparator, matching how kiln_cfg_store_capture_
                     * expected_pico_fields() (the expected side) already
                     * widens them from kiln_pkg_pico_param_t.value_bits. */
                    switch (row.type) {
                    case KILNLINK_PARAM_TYPE_BOOL:
                        live_value = (float)row.value.bool_val;
                        break;
                    case KILNLINK_PARAM_TYPE_U8:
                        live_value = (float)row.value.u8_val;
                        break;
                    case KILNLINK_PARAM_TYPE_U16:
                        live_value = (float)row.value.u16_val;
                        break;
                    case KILNLINK_PARAM_TYPE_F32:
                    default:
                        live_value = row.value.f32_val;
                        break;
                    }
                }
                break;
            }
        }
        pico_fields[n].name = extra_names[n - 1];
        pico_fields[n].known = live_known;
        pico_fields[n].value = live_value;
        n++;
    }

    /* HIGH 2 interim (see s_standing_warning_active's doc comment): heat-off
     * enforcement is decided from the CEILING FIELD ALONE (esp_fields[0]/
     * pico_fields[0], n==1) -- exactly the pre-broadening comparison -- never
     * from the full n-field set. The full set still feeds the broadened
     * WARNING below. */
    char ceiling_reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool ceiling_diverged = config_divergence_check(esp_fields, pico_fields, 1, ceiling_reason, sizeof(ceiling_reason));
    int64_t now_us = (int64_t)hal_time_now_us();

    if (!ceiling_diverged) {
        divergence_state_lock_take();
        s_divergence_active = false;
        s_divergence_reason[0] = '\0';
        divergence_state_lock_give();
    } else {
        /* See safety_ceiling_sync.h's own doc comment on safety_ceiling_sync_
         * set_disable_heat_hooks() for why these are injected function
         * pointers rather than direct kiln_io_owner.h/profile_executor.h
         * calls: this file is compiled into more than one host test executable
         * with different fake ecosystems, and NULL (the default, e.g. every
         * host test that never calls the setter) is the correct, safe no-op --
         * a test binary has no real relays to turn off. Called OUTSIDE the
         * divergence-state lock below (MEDIUM 5) -- never hold that lock
         * across these hook calls, which may themselves take other modules'
         * locks or do real I/O. */
        if (s_disable_all_relays_off) {
            s_disable_all_relays_off();
        }
        if (s_disable_halt_run) {
            s_disable_halt_run();
        }

        bool should_log = false;
        divergence_state_lock_take();
        snprintf(s_divergence_reason, sizeof(s_divergence_reason), "%s", ceiling_reason);
        if (!s_divergence_active || (now_us - s_divergence_last_log_us >= CONFIG_DIVERGENCE_LOG_INTERVAL_US)) {
            should_log = true;
            s_divergence_last_log_us = now_us;
        }
        s_divergence_active = true;
        divergence_state_lock_give();
        if (should_log) {
            ESP_LOGE(TAG, "ALARM: %s -- heaters disabled (all relays forced off, any run halted)", ceiling_reason);
        }
    }

    /* Broadened (non-ceiling) fields, fields[1..n): run the SAME comparator
     * over the whole set (including the ceiling field -- config_divergence.h's
     * hash covers the array it is given, and re-including field 0 costs
     * nothing and keeps this a single, easily-audited call shape identical to
     * the ceiling-only one above) purely to decide whether anything beyond
     * the ceiling disagrees. A mismatch here is reported as a WARNING only --
     * see this function's own top-of-file doc comment on why heat-off is not
     * yet wired to it (HIGH 2 interim, no confirmed push path for these
     * fields exists in production today). */
    char standing_reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool standing_diverged = config_divergence_check(esp_fields, pico_fields, n, standing_reason, sizeof(standing_reason));
    bool non_ceiling_diverged = standing_diverged && !ceiling_diverged;
    if (!non_ceiling_diverged) {
        divergence_state_lock_take();
        s_standing_warning_active = false;
        s_standing_warning_reason[0] = '\0';
        divergence_state_lock_give();
    } else {
        /* 2026-09-15 review (HIGH 2): a mismatch here often cannot be
         * corrected right now because a commissioning re-push was refused
         * while the relay is ARMED -- see safety_cfg_store_recent_armed_
         * refusal()'s doc comment. Naming that explicitly turns "config
         * mismatch, no visible remedy" (the shape most likely to end with
         * this warning ignored or silenced) into "disarm and retry", an
         * actionable instruction. Appended, not substituted, so the
         * underlying field/value detail from config_divergence_check() is
         * never lost. safety_cfg_store_recent_armed_refusal() is a plain
         * timestamp read (no lock of its own), safe to call before taking
         * the divergence-state lock below. */
        bool armed_refusal = safety_cfg_store_recent_armed_refusal();
        bool should_log = false;
        divergence_state_lock_take();
        if (armed_refusal) {
            /* 2026-09-15 review follow-up (item H): this used to be an
             * snprintf("%s <fixed suffix>") wrapped in a -Wformat-truncation
             * pragma push/pop, because standing_reason and
             * s_standing_warning_reason are both CONFIG_DIVERGENCE_REASON_MAX
             * and the suffix cannot always fit. Suppressing the warning hid a
             * real weakness in that formulation: snprintf truncates the TAIL,
             * so a long reason would have silently dropped the ARMED hint --
             * the entire point of this branch -- rather than the field detail.
             * Built explicitly instead: truncate the reason text to whatever
             * leaves room, then always append the full suffix. No diagnostic
             * needs silencing because nothing can truncate unexpectedly. */
            static const char kArmedSuffix[] =
                " (re-push refused: relay is ARMED -- disarm and retry commissioning)";
            const size_t cap = sizeof(s_standing_warning_reason);
            const size_t suffix_len = sizeof(kArmedSuffix) - 1u;
            if (cap > suffix_len) {
                size_t head_cap = cap - suffix_len - 1u;
                size_t head_len = strlen(standing_reason);
                if (head_len > head_cap) {
                    head_len = head_cap;
                }
                memcpy(s_standing_warning_reason, standing_reason, head_len);
                memcpy(s_standing_warning_reason + head_len, kArmedSuffix, suffix_len);
                s_standing_warning_reason[head_len + suffix_len] = '\0';
            } else {
                /* Unreachable with today's sizes (160 vs ~68); kept so a
                 * future shrink of CONFIG_DIVERGENCE_REASON_MAX degrades to
                 * "hint only" rather than overflowing. */
                snprintf(s_standing_warning_reason, cap, "%s", kArmedSuffix);
            }
        } else {
            snprintf(s_standing_warning_reason, sizeof(s_standing_warning_reason), "%s", standing_reason);
        }
        if (!s_standing_warning_active ||
            (now_us - s_standing_warning_last_log_us >= CONFIG_DIVERGENCE_LOG_INTERVAL_US)) {
            should_log = true;
            s_standing_warning_last_log_us = now_us;
        }
        s_standing_warning_active = true;
        divergence_state_lock_give();
        if (should_log) {
            ESP_LOGW(TAG,
                     "standing config divergence (non-ceiling): %s -- heat NOT disabled for this "
                     "(HIGH 2 interim; see safety_ceiling_sync.h)",
                     standing_reason);
        }
    }

    /* Item F: same reasoning as the early-return stamp above -- published
     * under the lock so the stamp and the latches are never observed out of
     * step with each other. */
    divergence_state_lock_take();
    s_latch_evaluated_generation = cache_gen_at_entry;
    divergence_state_lock_give();
}

void safety_ceiling_sync_reconcile_on_link_up(SafetyLinkClass *link)
{
    if (!link) {
        return; /* nothing to reconcile against */
    }
    if (!zones_config_is_valid()) {
        return; /* same gate the removed safety_sync_tc_type() used -- no real config to derive a target from yet */
    }

    /* 2026-09-10 opus review finding: the comment this replaces claimed
     * `now_us` was made static (file-scope storage, not a stack local) as a
     * stack-budget measure for safety_poll_task -- that claim was false.
     * This same function frame already carries `float new_max_temp_c[3]`
     * and `char reason[128]` (both genuine stack locals, ~140 bytes) a few
     * lines below, so moving 8 bytes of `int64_t` off the stack saves
     * nothing meaningful; safety_poll_task in fact has 4788 of 8192 bytes
     * free. `now_us` stays static anyway (harmless, not a fix for anything)
     * purely for consistency with s_last_log_us/s_suppressed_log_count
     * below, which for the same reason as always -- rate-limiting a WARN
     * log across calls -- genuinely must persist between calls; this
     * function is only ever called from safety_poll_task, never
     * concurrently, so there is no reentrancy hazard in reusing one
     * instance. Do not cite this comment as a stack-budget justification
     * for anything -- it is not one. */
    static int64_t now_us;
    now_us = (int64_t)hal_time_now_us();

    float new_max_temp_c[MAX31856_CHANNEL_COUNT];
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        float cur_max = 0.0f, cur_min = 0.0f;
        zones_config_get_temp_limits(zi, &cur_max, &cur_min);
        new_max_temp_c[zi] = cur_max;
    }

    /* Divergence enforcement runs UNCONDITIONALLY, before the backoff check
     * below -- deliberately. The backoff exists only to bound the cost of
     * RE-ATTEMPTING an expensive stage+commit+confirm UART write; it must
     * never also suppress the cheap (cache-only, no wire I/O) divergence
     * check and its active heaters-off enforcement. The single most likely
     * divergent case -- the Pico is ARMED and refuses a raise -- is exactly
     * the case the backoff spends most of its time in, and that is the ONE
     * case this enforcement absolutely must not go quiet during. */
    {
        float target_c = safety_ceiling_policy_target_c(new_max_temp_c, MAX31856_CHANNEL_COUNT);
        bool target_known = target_c > 0.0f;
        float pico_c = 0.0f;
        bool pico_known = safety_ceiling_sync_get_current_pico_ceiling(&pico_c);
        enforce_ceiling_divergence(target_c, target_known, pico_c, pico_known);
    }

    if (!safety_ceiling_reconcile_should_attempt(&s_reconcile_backoff, now_us)) {
        /* Backing off from a prior failed raise (most commonly the Pico
         * reporting ARMED, its ordinary standing state) -- skip the retry
         * itself, no UART round trip, no log line. See safety_ceiling_
         * policy.h's safety_ceiling_reconcile_backoff_t comment for the
         * full rationale and the window this leaves open. */
        return;
    }

    safety_ceiling_sync_result_t result = SAFETY_CEILING_SYNC_NONE;
    char reason[128] = { 0 };
    safety_ceiling_refusal_class_t refusal_class = SAFETY_CEILING_REFUSAL_NONE;
    bool ok = safety_ceiling_sync_guard_raise(link, new_max_temp_c, MAX31856_CHANNEL_COUNT, &result, reason,
                                              sizeof(reason), &refusal_class);
    /* 2026-09-10 opus review finding: the backoff decision now runs on the
     * numeric `refusal_class` above, never on `reason`'s prose -- `reason`
     * is kept purely for the human-readable log line below. */
    safety_ceiling_reconcile_record_result(&s_reconcile_backoff, now_us, ok, refusal_class);
    if (!ok) {
        if (now_us - s_last_log_us >= SAFETY_CEILING_SYNC_LOG_INTERVAL_US) {
            ESP_LOGW(TAG,
                     "link-up reconcile: could not raise/confirm the safety processor's ceiling to match the "
                     "ESP's zone config -- %s -- backing off %lld s before retrying%s",
                     reason, (long long)(s_reconcile_backoff.backoff_until_us - now_us) / 1000000,
                     s_suppressed_log_count > 0 ? " (repeat warnings suppressed since the last one)" : "");
            s_last_log_us = now_us;
            s_suppressed_log_count = 0;
        } else {
            s_suppressed_log_count++;
        }
        return;
    }
    if (result == SAFETY_CEILING_SYNC_RAISED) {
        ESP_LOGI(TAG,
                 "link-up reconcile: safety processor's ceiling was behind the ESP's zone config -- raised and "
                 "confirmed");
    }
    /* SAFETY_CEILING_SYNC_NONE: already wide enough (the ordinary case on a
     * healthy reconnect where nothing changed while the link was down) --
     * nothing worth logging. */
}
