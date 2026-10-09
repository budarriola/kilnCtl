// Host tests for App/drivers/ui/ui_edit_firing_apply.c -- the LVGL-free half
// of the LCD "Edit firing" page (ui_page_edit_firing.c). Its own SEPARATE
// executable, same "#include the .c directly" convention as
// test_profiles_live_http.c: links the REAL live_profile.c (host hal_kv
// backend, fake_kv.c) so the fork/save/generation paths are exercised for
// real, and the REAL ui_edit_firing_apply.c -- the exact code the page's
// Apply button calls. Fakes only the neighbours that are not host-linkable
// here (profiles_http.c/profiles_edit_http.c/profiles_builtin.c/the
// executor's live-status accessor), the same seams test_profiles_live_http.c
// fakes.
#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hal_kv.h"
#include "profile_executor.h"
#include "profiles_builtin.h"
#include "../drivers/http/profiles_http_internal.h"

#define PROFILE_DECODE_RESULT_SHIM_DECLARED

// ---- fakes: blob format (profiles_http.c, not linked) ----------------------
size_t profile_encode_current_blob(const profile_t *profile, void *out, size_t cap)
{
    if (!profile || !out || cap < sizeof(profile_t)) {
        return 0;
    }
    memcpy(out, profile, sizeof(profile_t));
    return sizeof(profile_t);
}

profile_decode_result_t profile_decode_blob(const void *blob, size_t len, profile_t *out, const char **err_reason)
{
    if (!blob || !out || len != sizeof(profile_t)) {
        if (err_reason) {
            *err_reason = "bad length";
        }
        return PROFILE_DECODE_CORRUPT;
    }
    memcpy(out, blob, sizeof(profile_t));
    return PROFILE_DECODE_OK;
}

// ---- fakes: profiles_builtin.h ---------------------------------------------
static bool g_fake_builtin_on = false;
static builtin_profile_t g_fake_builtin = {.code = "C6TEST"};
static profile_t g_fake_builtin_profile;

bool profiles_builtin_id_valid(uint8_t id)
{
    return g_fake_builtin_on && id == PROFILE_BUILTIN_ID_BASE;
}
const builtin_profile_t *profiles_builtin_entry(uint8_t id)
{
    return profiles_builtin_id_valid(id) ? &g_fake_builtin : NULL;
}
bool profiles_builtin_get(uint8_t id, profile_t *out)
{
    if (!profiles_builtin_id_valid(id) || !out) {
        return false;
    }
    *out = g_fake_builtin_profile;
    return true;
}

// ---- fakes: profiles_http_get (user slots) ---------------------------------
static profile_t g_fake_slots[PROFILES_MAX_COUNT];
static bool g_fake_slot_used[PROFILES_MAX_COUNT];

bool profiles_http_get(uint8_t id, profile_t *out)
{
    if (id >= PROFILES_MAX_COUNT || !g_fake_slot_used[id] || !out) {
        return false;
    }
    *out = g_fake_slots[id];
    return true;
}

// ---- fake: profiles_validate_candidate -------------------------------------
// Stand-in for the zone-ceiling rule: refuses any ZONE_RAMP target above
// g_fake_zone_max_c, naming the segment, as the real HARD mode does.
static float g_fake_zone_max_c = 1300.0f;
static int g_fake_validate_calls = 0;

bool profiles_validate_candidate(const profile_t *candidate, profile_validate_mode_t mode, char *warnings_json,
                                  size_t warnings_json_cap, char *err_msg, size_t err_cap)
{
    g_fake_validate_calls++;
    TEST_CHECK(mode == PROFILE_VALIDATE_HARD, "LCD apply validates in HARD mode, same as the HTTP accept route");
    if (warnings_json && warnings_json_cap) {
        warnings_json[0] = '\0';
    }
    for (uint8_t i = 0; i < candidate->segment_count; i++) {
        if (candidate->segments[i].seg_kind == PROFILE_SEG_KIND_ZONE_RAMP &&
            candidate->segments[i].target_c > g_fake_zone_max_c) {
            snprintf(err_msg, err_cap, "segment %u target above zone max", (unsigned)(i + 1));
            return false;
        }
    }
    return true;
}

// ---- fake: executor live status --------------------------------------------
static profile_executor_live_status_t g_fake_live_status;
void profile_executor_get_live_status(profile_executor_live_status_t *out)
{
    *out = g_fake_live_status;
}

#include "../drivers/persist/live_profile.c"
#include "../drivers/ui/ui_edit_firing_apply.c"

// ---------------------------------------------------------------------------

static profile_t make_profile(const char *name)
{
    profile_t p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name, name, sizeof(p.name) - 1);
    p.zone_mask = 0x7;
    p.segment_count = 4;
    for (uint8_t i = 0; i < 3; i++) {
        p.segments[i].seg_kind = PROFILE_SEG_KIND_ZONE_RAMP;
        p.segments[i].target_c = 200.0f + 300.0f * i;
        p.segments[i].ramp_c_per_hr = 100.0f;
        p.segments[i].dwell_min = 10;
    }
    p.segments[3].seg_kind = PROFILE_SEG_KIND_RELAY_IO;
    p.segments[3].io_target = PROFILE_IO_TARGET_RELAY_BASE;
    p.segments[3].io_state = 1;
    p.segments[3].io_blocking = 1;
    p.segments[3].dwell_min = 5;
    return p;
}

static void reset_world(void)
{
    char err[128];
    live_profile_clear(err, sizeof(err));
    memset(g_fake_slot_used, 0, sizeof(g_fake_slot_used));
    g_fake_builtin_on = false;
    g_fake_zone_max_c = 1300.0f;
    g_fake_validate_calls = 0;
    memset(&g_fake_live_status, 0, sizeof(g_fake_live_status));

    g_fake_slots[2] = make_profile("Bisque");
    g_fake_slot_used[2] = true;
    g_fake_slots[5] = make_profile("Glaze");
    g_fake_slot_used[5] = true;
    g_fake_live_status.active = true;
    g_fake_live_status.profile_id = 2;
    g_fake_live_status.segment_index = 1;
}

static bool nothing_pending(void)
{
    live_edit_record_t rec;
    return !live_profile_load_record(&rec) || !rec.pending;
}

static void test_seg_editable(void)
{
    TEST_SECTION("edit_firing_seg_editable -- finished locked, relay/IO read-only, running/upcoming ramp editable");
    profile_t p = make_profile("x");
    TEST_CHECK(!edit_firing_seg_editable(&p, 0, 1), "finished segment is not editable");
    TEST_CHECK(edit_firing_seg_editable(&p, 1, 1), "running ZONE_RAMP segment is editable");
    TEST_CHECK(edit_firing_seg_editable(&p, 2, 1), "upcoming ZONE_RAMP segment is editable");
    TEST_CHECK(!edit_firing_seg_editable(&p, 3, 1), "upcoming RELAY_IO segment is read-only (web shows it read-only)");
    TEST_CHECK(!edit_firing_seg_editable(&p, 4, 1), "index past segment_count is not editable");
    TEST_CHECK(edit_firing_seg_phase(0, 1) == EDIT_FIRING_SEG_FINISHED, "phase: finished");
    TEST_CHECK(edit_firing_seg_phase(1, 1) == EDIT_FIRING_SEG_RUNNING, "phase: running");
    TEST_CHECK(edit_firing_seg_phase(2, 1) == EDIT_FIRING_SEG_UPCOMING, "phase: upcoming");
}

static void test_step_clamps(void)
{
    TEST_SECTION("edit_firing_step -- steps and clamps to the HTTP parser's PROFILE_* bounds");
    profile_t p = make_profile("x");

    TEST_CHECK(!edit_firing_step(&p, 0, 1, EDIT_FIRING_FIELD_TARGET, +1), "finished segment refuses a step");
    TEST_CHECK(p.segments[0].target_c == 200.0f, "finished segment unchanged");
    TEST_CHECK(!edit_firing_step(&p, 3, 1, EDIT_FIRING_FIELD_DWELL, +1), "relay/IO segment refuses a step");
    TEST_CHECK(p.segments[3].dwell_min == 5, "relay/IO dwell unchanged");

    TEST_CHECK(edit_firing_step(&p, 1, 1, EDIT_FIRING_FIELD_TARGET, +1), "target step up changes value");
    TEST_CHECK_NEAR(p.segments[1].target_c, 505.0f, 0.001f, "target +5 C");

    p.segments[1].target_c = 2013.0f;
    edit_firing_step(&p, 1, 1, EDIT_FIRING_FIELD_TARGET, +1);
    TEST_CHECK_NEAR(p.segments[1].target_c, PROFILE_TARGET_C_MAX, 0.001f, "target clamps at 2015");
    TEST_CHECK(!edit_firing_step(&p, 1, 1, EDIT_FIRING_FIELD_TARGET, +1), "target at max: no change");

    p.segments[1].target_c = 3.0f;
    edit_firing_step(&p, 1, 1, EDIT_FIRING_FIELD_TARGET, -1);
    TEST_CHECK_NEAR(p.segments[1].target_c, 0.0f, 0.001f, "target clamps at 0");

    p.segments[1].ramp_c_per_hr = 998.0f;
    edit_firing_step(&p, 1, 1, EDIT_FIRING_FIELD_RAMP, +1);
    TEST_CHECK_NEAR(p.segments[1].ramp_c_per_hr, PROFILE_RAMP_C_PER_HR_MAX, 0.001f, "ramp clamps at 1000");
    p.segments[1].ramp_c_per_hr = 2.0f;
    edit_firing_step(&p, 1, 1, EDIT_FIRING_FIELD_RAMP, -1);
    TEST_CHECK_NEAR(p.segments[1].ramp_c_per_hr, 0.0f, 0.001f, "ramp clamps at 0 (= none)");

    p.segments[1].dwell_min = 1438;
    edit_firing_step(&p, 1, 1, EDIT_FIRING_FIELD_DWELL, +1);
    TEST_CHECK(p.segments[1].dwell_min == PROFILE_DWELL_MIN_MAX, "dwell clamps at 1440");
    p.segments[1].dwell_min = 3;
    edit_firing_step(&p, 1, 1, EDIT_FIRING_FIELD_DWELL, -1);
    TEST_CHECK(p.segments[1].dwell_min == 0, "dwell clamps at 0");
}

static void test_fields_in_range(void)
{
    TEST_SECTION("edit_firing_fields_in_range -- the parser's per-segment ranges");
    char err[128];
    profile_t p = make_profile("x");
    TEST_CHECK(edit_firing_fields_in_range(&p, err, sizeof(err)), "a normal profile is in range");

    p.segments[2].dwell_min = 1441;
    TEST_CHECK(!edit_firing_fields_in_range(&p, err, sizeof(err)), "dwell 1441 refused");
    TEST_CHECK(strstr(err, "segment 3") != NULL, "reason names the segment");

    p = make_profile("x");
    p.segments[1].target_c = 2016.0f;
    TEST_CHECK(!edit_firing_fields_in_range(&p, err, sizeof(err)), "target 2016 refused");
    p.segments[1].target_c = NAN;
    TEST_CHECK(!edit_firing_fields_in_range(&p, err, sizeof(err)), "NaN target refused");

    p = make_profile("x");
    p.segments[1].ramp_c_per_hr = -1.0f;
    TEST_CHECK(!edit_firing_fields_in_range(&p, err, sizeof(err)), "negative ramp refused");

    p = make_profile("x");
    p.segments[3].target_c = 5000.0f; /* meaningless on a RELAY_IO segment -- the parser never reads it */
    TEST_CHECK(edit_firing_fields_in_range(&p, err, sizeof(err)), "relay/IO target is not range-checked");
}

static void test_load(void)
{
    TEST_SECTION("edit_firing_load -- origin when nothing pending, working copy when pending, none when idle");
    reset_world();
    profile_t out;
    edit_firing_ctx_t ctx;

    g_fake_live_status.active = false;
    TEST_CHECK(!edit_firing_load(&out, &ctx), "no active firing: nothing to load");

    g_fake_live_status.active = true;
    TEST_CHECK(edit_firing_load(&out, &ctx), "loads with a firing active");
    TEST_CHECK(strcmp(out.name, "Bisque") == 0, "loaded the origin profile");
    TEST_CHECK(ctx.origin_id == 2 && ctx.running_seg == 1, "ctx carries origin id and running segment");
    TEST_CHECK(ctx.generation == live_profile_generation(), "ctx carries the current generation");

    profile_t edited = g_fake_slots[2];
    edited.segments[2].target_c = 777.0f;
    char err[128];
    live_edit_record_t rec;
    profile_t tmp;
    TEST_CHECK(live_profile_fork(2, false, "Bisque", &g_fake_slots[2], &tmp, &rec, err, sizeof(err)), "web fork");
    TEST_CHECK(live_profile_save_working(&edited, err, sizeof(err)), "web save");
    TEST_CHECK(edit_firing_load(&out, &ctx), "loads with a pending working copy");
    TEST_CHECK(out.segments[2].target_c == 777.0f, "loaded the pending working copy, not the origin");
}

static void test_apply_success_forks_then_saves(void)
{
    TEST_SECTION("edit_firing_apply -- first Apply forks, saves, advances ctx.generation; executor sees a new "
                 "generation (same pickup mechanism as the web)");
    reset_world();
    profile_t w;
    edit_firing_ctx_t ctx;
    char err[128];
    TEST_CHECK(edit_firing_load(&w, &ctx), "load");
    uint32_t gen_before = live_profile_generation();

    TEST_CHECK(edit_firing_step(&w, 1, ctx.running_seg, EDIT_FIRING_FIELD_TARGET, +1), "edit running segment");
    TEST_CHECK(edit_firing_step(&w, 2, ctx.running_seg, EDIT_FIRING_FIELD_DWELL, +1), "edit upcoming segment");
    TEST_CHECK(edit_firing_apply(&w, &ctx, err, sizeof(err)), "apply succeeds");
    TEST_CHECK(live_profile_generation() != gen_before, "generation bumped (executor pickup trigger)");
    TEST_CHECK(ctx.generation == live_profile_generation(), "ctx.generation follows the save");
    TEST_CHECK(g_fake_validate_calls == 1, "bounds validator ran exactly once");

    live_edit_record_t rec;
    TEST_CHECK(live_profile_load_record(&rec) && rec.pending && rec.origin_id == 2, "pending record for origin 2");
    TEST_CHECK(strcmp(rec.origin_name, "Bisque") == 0, "fork used the slot name, as the fork route does");
    profile_t saved;
    TEST_CHECK(live_profile_load_working(&saved), "working copy readable");
    TEST_CHECK(saved.segments[1].target_c == 505.0f && saved.segments[2].dwell_min == 15, "edit persisted");
    TEST_CHECK(g_fake_slots[2].segments[1].target_c == 500.0f, "saved origin profile never touched");

    TEST_CHECK(edit_firing_step(&w, 2, ctx.running_seg, EDIT_FIRING_FIELD_RAMP, +1), "second edit");
    TEST_CHECK(edit_firing_apply(&w, &ctx, err, sizeof(err)), "second Apply from same page is not 'edited elsewhere'");
}

static void test_apply_refusals_write_nothing(void)
{
    TEST_SECTION("edit_firing_apply -- every refusal leaves no fork and no save behind");
    profile_t w;
    edit_firing_ctx_t ctx;
    char err[128];

    reset_world();
    TEST_CHECK(edit_firing_load(&w, &ctx), "load");
    w.segments[2].target_c = 1400.0f; /* above fake zone max, inside parser range */
    TEST_CHECK(!edit_firing_apply(&w, &ctx, err, sizeof(err)), "bound violation refused");
    TEST_CHECK(strstr(err, "zone max") != NULL, "validator's reason surfaced");
    TEST_CHECK(nothing_pending(), "no fork left behind by a bound refusal");

    reset_world();
    TEST_CHECK(edit_firing_load(&w, &ctx), "load");
    w.segments[2].dwell_min = 2000;
    int calls = g_fake_validate_calls;
    TEST_CHECK(!edit_firing_apply(&w, &ctx, err, sizeof(err)), "parser-range violation refused");
    TEST_CHECK(g_fake_validate_calls == calls, "range check runs before the validator");
    TEST_CHECK(nothing_pending(), "no fork left behind by a range refusal");

    reset_world();
    TEST_CHECK(edit_firing_load(&w, &ctx), "load");
    w.segments[0].target_c += 5.0f; /* segment 0 finished (running = 1) */
    TEST_CHECK(!edit_firing_apply(&w, &ctx, err, sizeof(err)), "window violation refused");
    TEST_CHECK(err[0] != '\0', "window reason surfaced");
    TEST_CHECK(nothing_pending(), "no fork left behind by a window refusal");

    reset_world();
    TEST_CHECK(edit_firing_load(&w, &ctx), "load at segment 1");
    w.segments[1].target_c += 5.0f;
    g_fake_live_status.segment_index = 2; /* firing advanced while page open */
    TEST_CHECK(!edit_firing_apply(&w, &ctx, err, sizeof(err)), "segment that became finished is refused");
    TEST_CHECK(nothing_pending(), "no fork left behind by a frozen-meanwhile refusal");

    reset_world();
    TEST_CHECK(edit_firing_load(&w, &ctx), "load");
    g_fake_live_status.active = false;
    TEST_CHECK(!edit_firing_apply(&w, &ctx, err, sizeof(err)), "inactive firing refused");
    TEST_CHECK(nothing_pending(), "no fork when inactive");

    reset_world();
    TEST_CHECK(edit_firing_load(&w, &ctx), "load profile 2");
    g_fake_live_status.profile_id = 5; /* firing ended and a different one started */
    g_fake_live_status.segment_index = 0;
    TEST_CHECK(!edit_firing_apply(&w, &ctx, err, sizeof(err)), "stale copy from another firing refused");
    TEST_CHECK(nothing_pending(), "profile 2's copy never saved as profile 5's working copy");

    reset_world();
    TEST_CHECK(edit_firing_load(&w, &ctx), "load");
    {
        profile_t web = g_fake_slots[2];
        web.segments[2].target_c = 888.0f;
        live_edit_record_t rec;
        profile_t tmp;
        TEST_CHECK(live_profile_fork(2, false, "Bisque", &g_fake_slots[2], &tmp, &rec, err, sizeof(err)), "web fork");
        TEST_CHECK(live_profile_save_working(&web, err, sizeof(err)), "web save while LCD page open");
    }
    w.segments[2].dwell_min += 5;
    TEST_CHECK(!edit_firing_apply(&w, &ctx, err, sizeof(err)), "LCD refuses to overwrite the web's newer edit");
    profile_t saved;
    TEST_CHECK(live_profile_load_working(&saved) && saved.segments[2].target_c == 888.0f, "web edit survives");
}

static void test_apply_builtin_origin(void)
{
    TEST_SECTION("edit_firing_apply -- builtin origin forks with the builtin's code as its name");
    reset_world();
    g_fake_builtin_on = true;
    g_fake_builtin_profile = make_profile("Cone 6 long title");
    g_fake_live_status.profile_id = PROFILE_BUILTIN_ID_BASE;
    g_fake_live_status.segment_index = 0;
    profile_t w;
    edit_firing_ctx_t ctx;
    char err[128];
    TEST_CHECK(edit_firing_load(&w, &ctx), "load builtin");
    edit_firing_step(&w, 0, ctx.running_seg, EDIT_FIRING_FIELD_TARGET, -1);
    TEST_CHECK(edit_firing_apply(&w, &ctx, err, sizeof(err)), "apply on builtin succeeds");
    live_edit_record_t rec;
    TEST_CHECK(live_profile_load_record(&rec) && rec.origin_is_builtin, "record marks builtin origin");
    TEST_CHECK(strcmp(rec.origin_name, "C6TEST") == 0, "origin name is the builtin code, as the fork route uses");
}

static void test_apply_stale_foreign_origin_record_never_borrowed(void)
{
    TEST_SECTION("edit_firing_apply -- origin_is_builtin is derived from THIS firing's origin id, never taken "
                 "from a stale live_edit record left pending by a DIFFERENT origin (review follow-up (a))");
    reset_world();

    // A previous, still-undecided live edit is on flash for the BUILTIN origin
    // -- e.g. that firing tripped or rebooted before the operator ever answered
    // the save-as/overwrite/discard prompt (live_edit_should_prompt()'s own
    // scenario). It is deliberately left pending: nothing decides or clears it.
    g_fake_builtin_on = true;
    g_fake_builtin_profile = make_profile("Cone 6 old");
    char err[128];
    live_edit_record_t stale_rec;
    profile_t tmp;
    TEST_CHECK(live_profile_fork(PROFILE_BUILTIN_ID_BASE, true, "C6TEST", &g_fake_builtin_profile, &tmp, &stale_rec,
                                  err, sizeof(err)),
               "stale fork for a builtin origin, left pending/undecided");

    // A NEW firing starts on a different, USER-SLOT origin (5, "Glaze") with no
    // pending record of its own -- the only record on flash still names the
    // builtin above.
    g_fake_live_status.active = true;
    g_fake_live_status.profile_id = 5;
    g_fake_live_status.segment_index = 1;

    profile_t w;
    edit_firing_ctx_t ctx;
    TEST_CHECK(edit_firing_load(&w, &ctx), "load origin 5 (Glaze, a user slot)");
    TEST_CHECK(edit_firing_step(&w, 2, ctx.running_seg, EDIT_FIRING_FIELD_TARGET, +1),
               "edit an upcoming, in-window segment -- otherwise unremarkable");
    bool ok = edit_firing_apply(&w, &ctx, err, sizeof(err));
    TEST_CHECK(!ok, "still refused -- the stale builtin record must be resolved before a new one forks");
    TEST_CHECK(strstr(err, "already pending for a different profile") != NULL,
               "refused for the REAL reason (live_profile_fork()'s own stale-origin guard, reached only "
               "because the window check ahead of it correctly loaded origin 5 as a non-builtin) -- "
               "with the bug, origin_is_builtin is wrongly borrowed as true from the stale builtin record, "
               "profiles_builtin_get() then refuses id 5, and the apply is refused for the WRONG reason");
    TEST_CHECK(strstr(err, "origin profile not readable") == NULL,
               "must not report the wrong-accessor symptom of the origin_is_builtin-from-a-foreign-record bug");
}

static void test_poll(void)
{
    TEST_SECTION("edit_firing_poll -- ended / other firing / edited elsewhere / pickup refusal");
    reset_world();
    profile_t w;
    edit_firing_ctx_t ctx;
    char err[128];
    edit_firing_poll_t pr;
    TEST_CHECK(edit_firing_load(&w, &ctx), "load");

    edit_firing_poll(&ctx, 0, &pr);
    TEST_CHECK(pr.state == EDIT_FIRING_POLL_OK && pr.running_seg == 1 && !pr.refused, "steady state");

    g_fake_live_status.segment_index = 2;
    edit_firing_poll(&ctx, 0, &pr);
    TEST_CHECK(pr.running_seg == 2, "poll reports the advanced running segment");

    edit_firing_step(&w, 2, 2, EDIT_FIRING_FIELD_DWELL, +1);
    ctx.running_seg = 2;
    TEST_CHECK(edit_firing_apply(&w, &ctx, err, sizeof(err)), "apply");
    uint32_t applied = ctx.generation;
    g_fake_live_status.has_refusal = true;
    g_fake_live_status.refusal_generation = applied;
    snprintf(g_fake_live_status.refusal_err_msg, sizeof(g_fake_live_status.refusal_err_msg), "ramp too fast");
    edit_firing_poll(&ctx, applied, &pr);
    TEST_CHECK(pr.refused && strcmp(pr.refusal_msg, "ramp too fast") == 0, "pickup refusal of our edit surfaced");
    g_fake_live_status.refusal_generation = applied - 1;
    edit_firing_poll(&ctx, applied, &pr);
    TEST_CHECK(!pr.refused, "a refusal of some other generation is not attributed to this page");
    g_fake_live_status.has_refusal = false;

    TEST_CHECK(live_profile_save_working(&w, err, sizeof(err)), "someone else saves");
    edit_firing_poll(&ctx, 0, &pr);
    TEST_CHECK(pr.state == EDIT_FIRING_POLL_EDITED_ELSEWHERE, "edited elsewhere detected");

    g_fake_live_status.profile_id = 5;
    edit_firing_poll(&ctx, 0, &pr);
    TEST_CHECK(pr.state == EDIT_FIRING_POLL_OTHER_FIRING, "other firing detected");

    g_fake_live_status.active = false;
    edit_firing_poll(&ctx, 0, &pr);
    TEST_CHECK(pr.state == EDIT_FIRING_POLL_ENDED, "firing end detected");
}

#ifdef _WIN32
#include <direct.h>
#define CFGM_MKDIR(p) _mkdir(p)
#define CFGM_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define CFGM_MKDIR(p) mkdir((p), 0755)
#define CFGM_RMDIR(p) rmdir(p)
#endif
#include "cfg_fs.h"

/* flash_worker_wait.c (linked for pref_cfg_fs) asks whether the flash worker
 * started; on the host there is no worker, so answer yes at once. */
bool uart_bridge_ext_flash_worker_started(void)
{
    return true;
}

/* The live-edit record and working profile are cfg-only (owner decision
 * 2026-10-07): the fork/save paths need a mounted cfg scratch directory. */
static void mount_fresh_cfg_scratch(void)
{
    static const char *const scratch = "cfg_fs_test_ui_edit_firing_apply";
    static const char *const files[] = {"prof_live_rec.bin", "prof_live_work.bin"};
    cfg_fs_deinit();
    for (size_t i = 0; i < 2; i++) {
        char path[600];
        snprintf(path, sizeof(path), "%s/.tmp/%s", scratch, files[i]);
        remove(path);
        snprintf(path, sizeof(path), "%s/%s", scratch, files[i]);
        remove(path);
    }
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", scratch);
    CFGM_RMDIR(tmp);
    CFGM_RMDIR(scratch);
    CFGM_MKDIR(scratch);
    (void)cfg_fs_init(scratch, NULL);
}

int main(void)
{
    // fake_kv.c needs every partition initialized before hal_kv_open();
    // "profiles_nvs" is live_profile.c's private LIVE_PROFILE_NVS_PARTITION.
    hal_kv_init_partition("profiles_nvs");
    mount_fresh_cfg_scratch();

    test_seg_editable();
    test_step_clamps();
    test_fields_in_range();
    test_load();
    test_apply_success_forks_then_saves();
    test_apply_refusals_write_nothing();
    test_apply_builtin_origin();
    test_apply_stale_foreign_origin_record_never_borrowed();
    test_poll();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    return 0;
}
