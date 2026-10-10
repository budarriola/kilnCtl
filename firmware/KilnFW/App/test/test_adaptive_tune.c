// Host tests for App/drivers/control/adaptive_tune.c -- PID_EXPANSION_PLAN.md Phase
// 7d (learn a zone's steady-state gain from the settled dwells an ordinary
// firing already produces).
//
// #includes adaptive_tune.c directly (same convention as
// test_autotune_engine_prestart.c/test_profile_executor_prestart.c) so this
// file can reach its guard constants and would otherwise need to fake a
// dozen zones_config_*()/httpd_*() symbols with no other seam. Own
// executable (build_host_tests.ps1's sixth build+run step) for the usual
// reason: it defines the REAL zones_config_get_model()/set_model()/get_pid()/
// set_pid() bodies as a tiny in-RAM fake table, which would multiply-define
// against every other test file that fakes those same names.
//
// 2026-09-01: this file's test bodies were split across six sibling files
// (test_adaptive_tune_dwell/model/coupled/ki_verdict/ki_bounds/status.c),
// grouped by production seam (matching the adaptive_tune.c/adaptive_tune_
// model.c/adaptive_tune_ki.c split), to keep it under this repo's 1500-line
// guidance -- it had grown to ~3069 lines. This file now holds only the
// shared fakes, test helpers, and run_test_adaptive_tune()/main(); it
// #includes every sibling file below, same one-TU convention as its own
// #include of adaptive_tune.c/adaptive_tune_model.c/adaptive_tune_ki.c.
// build_host_tests.ps1's cmd17 is unchanged -- it still names only this
// file, which is all it needs since the sibling files are pulled in here.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/hw/MAX31856.h" // MAX31856_CHANNEL_COUNT, needed by the coupling-row fakes below,
                                  // ahead of adaptive_tune.c's own #include of it further down this file
#include "esp_err.h"
#include "fake_kv.h" /* hal_kv.h's host fake -- adaptive_tune.c now calls hal_kv_*() instead of
                       * nvs_*() directly (HW_ABSTRACTION.md Phase 3 item 3); used by
                       * test_adaptive_tune_status.c's opt-in-migration tests below. */

// profile_exec_status_t/profile_exec_state_t/PROFILE_EXEC_* -- needed by the
// profile_executor_get_status() fake below (U1 one-click revert's mid-firing
// guard), pulled in explicitly here since adaptive_tune.c's own #include of
// profile_executor.h (via adaptive_tune.h) does not happen until further
// down this file.
#include "../drivers/control/profile_executor.h"

// zone_control_mode_t/ZONE_CONTROL_MODE_* -- needed by s_fake_zone_cfg's
// control_mode field and the zones_config_get_control_mode()/
// zones_config_get_fuzzy_strength_pct() fakes below, ahead of adaptive_
// tune.c's own #include of this header further down this file (same
// reasoning as the MAX31856.h/profile_executor.h #includes just above).
#include "../drivers/persist/zones_config_accessors.h"

// Own executable (see this file's header comment).
int g_test_failures = 0;
int g_test_count = 0;

// ---------------------------------------------------------------------
// Fakes for adaptive_tune.c's extern dependencies, in link order it needs
// them. zones_config_* below is a tiny per-zone table this file owns
// entirely, standing in for zones_config_accessors.c's real (much larger)
// NVS-backed store -- the tests only care that adaptive_tune.c reads the
// right zone's prior model and writes back the right zone's refined one,
// which this fake is precise enough to prove (see the asymmetric-zone
// tests below, which would fail if adaptive_tune.c ever transposed a zone
// index reaching into this table).
// ---------------------------------------------------------------------
#define TEST_MAX_ZONES 5
static struct {
    float k_dc, tau_s, dead_time_s;
    float kp, ki, kd;
    float autotune_baseline_k_dc; // 0 = "not recorded yet", same sentinel convention as k_dc
    zone_control_mode_t control_mode; // defaults to ZONE_CONTROL_MODE_PID(=0)... actually OFF(=0); every
                                       // existing test that never sets this explicitly stays off the
                                       // effective-vs-reference fuzzy guard below (adaptive_tune_ki.c)
    float fuzzy_strength_pct;
} s_fake_zone_cfg[TEST_MAX_ZONES];

// zone_is_on_off() fake: real body lives in zones_config_accessors.c, not
// linked into this executable (see file banner). Defaults every zone to
// HEATER (false), same as the real accessor's zero-init default -- tests
// that need an on/off zone flip the entry explicitly and restore it after,
// same convention as test_profile_executor_prestart.c's g_stub_zone_is_on_off.
static bool s_stub_zone_is_on_off[TEST_MAX_ZONES];
bool zone_is_on_off(uint8_t zone_index)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    return s_stub_zone_is_on_off[zone_index];
}
// zone_is_monitor_only() fake (docs/SPARE_RELAY_ONOFF_PLAN.md sec 10), same
// convention as the on/off fake above: every zone defaults to a driven heater.
static bool s_stub_zone_is_monitor_only[TEST_MAX_ZONES];
bool zone_is_monitor_only(uint8_t zone_index)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    return s_stub_zone_is_monitor_only[zone_index];
}

// F3 (FLASH_WORKER_LOCK_INVERSION_AUDIT): the real zones setters end in the zones
// NVS save and reach the flash worker, which itself takes adaptive_tune_lock. The
// semphr stub counts every held lock in g_test_stub_lock_depth and the only lock
// this module takes is adaptive_tune_lock, so a nonzero depth inside a setter
// means run_end called it with that lock held.
static int g_setter_calls = 0;
static int g_setter_max_lock_depth = 0;
#define SETTER_LOCK_PROBE()     do {         g_setter_calls++;         if (g_test_stub_lock_depth > g_setter_max_lock_depth) g_setter_max_lock_depth = g_test_stub_lock_depth;     } while (0)

// F3 follow-up race tests: a one-shot hook run AFTER a successful
// zones_config_set_model()/set_pid() write, i.e. inside the window where the
// caller has adaptive_tune_lock released. Cleared before it runs, so a hook
// that re-enters a setter cannot recurse.
static void (*s_set_model_hook)(void) = NULL;
static void (*s_set_pid_hook)(void) = NULL;
// Dev review 9 L1: fires on the first zones_config_get_pid() of a zone whose write_in_flight is set,
// i.e. run_end's apply phase before its first write (between plan and apply).
static void (*s_get_pid_in_flight_hook)(void) = NULL;
static bool zone_write_in_flight_unlocked(uint8_t zone_index); // defined after the adaptive_tune .c includes
static void run_setter_hook(void (**hook)(void))
{
    void (*h)(void) = *hook;
    *hook = NULL;
    if (h) h();
}

bool zones_config_get_model(uint8_t zone_index, float *out_k_dc, float *out_tau_s, float *out_dead_time_s)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_k_dc = s_fake_zone_cfg[zone_index].k_dc;
    *out_tau_s = s_fake_zone_cfg[zone_index].tau_s;
    *out_dead_time_s = s_fake_zone_cfg[zone_index].dead_time_s;
    return true;
}
bool zones_config_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s)
{
    SETTER_LOCK_PROBE(); // F3: adaptive_tune_lock must not be held here
    if (zone_index >= TEST_MAX_ZONES) return false;
    s_fake_zone_cfg[zone_index].k_dc = k_dc;
    s_fake_zone_cfg[zone_index].tau_s = tau_s;
    s_fake_zone_cfg[zone_index].dead_time_s = dead_time_s;
    run_setter_hook(&s_set_model_hook);
    return true;
}
// docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md's fix: the
// real zones_config_get/set_autotune_baseline_k_dc() (zones_config_
// accessors.c) this fakes -- same "tiny in-RAM table" convention and same
// 0-is-a-legal-sentinel behavior as the real setter (no bound check here;
// the real ZONE_AUTOTUNE_K_DC_MAX bound is exercised via adaptive_tune_
// model.c's OWN ADAPTIVE_TUNE_K_DC_ABS_MAX check before it ever calls this
// setter with an out-of-range value -- see test_adaptive_tune_model.c's
// K_dc-ratchet regression test).
bool zones_config_get_autotune_baseline_k_dc(uint8_t zone_index, float *out_k_dc)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_k_dc = s_fake_zone_cfg[zone_index].autotune_baseline_k_dc;
    return true;
}
bool zones_config_set_autotune_baseline_k_dc(uint8_t zone_index, float k_dc)
{
    SETTER_LOCK_PROBE(); // F3: adaptive_tune_lock must not be held here
    if (zone_index >= TEST_MAX_ZONES) return false;
    s_fake_zone_cfg[zone_index].autotune_baseline_k_dc = k_dc;
    return true;
}

bool zones_config_get_pid(uint8_t zone_index, float *out_kp, float *out_ki, float *out_kd)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    if (s_get_pid_in_flight_hook && zone_write_in_flight_unlocked(zone_index)) {
        run_setter_hook(&s_get_pid_in_flight_hook);
    }
    *out_kp = s_fake_zone_cfg[zone_index].kp;
    *out_ki = s_fake_zone_cfg[zone_index].ki;
    *out_kd = s_fake_zone_cfg[zone_index].kd;
    return true;
}
// H3(a): mirrors the REAL zones_config_set_pid() (zones_config_accessors.c)
// bound-and-reject behaviour, not just "accept anything" -- the real setter
// REJECTS (returns false, leaves the stored gain untouched) any of kp/ki/kd
// outside [0, ZONE_PID_GAIN_MAX], it does not silently clamp. Before this
// fix the fake accepted any finite value unconditionally, which is exactly
// this repo's idealized-test-input bug class: adaptive_tune_refine_ki_locked()'s
// repeated-Ki-application path (H3) had no real ceiling to run into on host,
// even though hardware does (this same bound, reached through the identical
// setter every caller in this file uses -- see that function's own comment
// on why the invalidation/bound checks live in the one shared setter).
#define TEST_ZONE_PID_GAIN_MAX 1000.0f // == zones_http.h's ZONE_PID_GAIN_MAX; redefined by hand since this
                                        // file does not include zones_http.h (see its own #include list)
bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    SETTER_LOCK_PROBE(); // F3: adaptive_tune_lock must not be held here
    if (zone_index >= TEST_MAX_ZONES) return false;
    if (!isfinite(kp) || !isfinite(ki) || !isfinite(kd) || kp < 0.0f || ki < 0.0f || kd < 0.0f ||
        kp > TEST_ZONE_PID_GAIN_MAX || ki > TEST_ZONE_PID_GAIN_MAX || kd > TEST_ZONE_PID_GAIN_MAX) {
        return false;
    }
    s_fake_zone_cfg[zone_index].kp = kp;
    s_fake_zone_cfg[zone_index].ki = ki;
    s_fake_zone_cfg[zone_index].kd = kd;
    run_setter_hook(&s_set_pid_hook);
    return true;
}

// Fakes for the effective-vs-reference guard adaptive_tune_ki.c added
// 2026-09-13 (docs/audits/adaptive_tune_ki_effective_reference_loop_2026-09-13.md):
// tiny in-RAM fields, same convention as every other s_fake_zone_cfg member.
// Defaults (control_mode 0 == ZONE_CONTROL_MODE_OFF, fuzzy_strength_pct 0)
// keep every pre-existing test in this file off the guard unless a test
// deliberately opts a zone into PID_FUZZY.
// K8 (docs/audits/adaptive_tune_ki_guard_timing_and_failopen_2026-09-14.md,
// defect 2): fail-injection knobs so a test can force either accessor to
// report failure (return false) the way a real one legitimately can (a
// zones_config read hitting an uninitialized/corrupt slot, say) -- the
// index-range check above can never exercise this, since every real zone
// index is well within TEST_MAX_ZONES. Both default false (existing tests
// unaffected) and are reset by reset_module_state() below.
static bool s_fake_control_mode_fail;
static bool s_fake_fuzzy_pct_fail;
bool zones_config_get_control_mode(uint8_t zone_index, zone_control_mode_t *out_mode)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    if (s_fake_control_mode_fail) return false;
    *out_mode = s_fake_zone_cfg[zone_index].control_mode;
    return true;
}
bool zones_config_get_fuzzy_strength_pct(uint8_t zone_index, float *out_pct)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    if (s_fake_fuzzy_pct_fail) return false;
    *out_pct = s_fake_zone_cfg[zone_index].fuzzy_strength_pct;
    return true;
}

// Coupling-row fake, same "tiny in-RAM table" convention as s_fake_zone_cfg
// above, standing in for zones_config_accessors.c's real coupling_coeff[]
// storage -- adaptive_tune.c's coupled-solve apply path
// (adaptive_tune_refine_coupled_locked()) reads/writes exactly this surface.
static float s_fake_coupling[TEST_MAX_ZONES][TEST_MAX_ZONES];
static float s_fake_coupling_tau[TEST_MAX_ZONES][TEST_MAX_ZONES];
static float s_fake_coupling_dead[TEST_MAX_ZONES][TEST_MAX_ZONES];

bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) out_row[j] = s_fake_coupling[zone_index][j];
    /* Mirror the real getter's on/off masking (zones_config_accessors.c): the
     * whole row if this zone is on/off, else every on/off COLUMN reads 0.0 --
     * so A4 follow-up B's test exercises the masked-zero prior it guards. */
    for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        if (s_stub_zone_is_on_off[zone_index] || s_stub_zone_is_on_off[j] ||
            s_stub_zone_is_monitor_only[zone_index] || s_stub_zone_is_monitor_only[j]) out_row[j] = 0.0f;
    }
    return true;
}
/* PID_EXPANSION_PLAN.md sec 3.2 ("the solver switch itself"): this file only
 * exercises zone_coupling_gauss_solve_partial_pivot_vec() (the coupled
 * identification's linear solve), never zone_coupling_solve_hold()/_climb(),
 * so coupling_diagonal_k_dc()'s zones_config_get_coupling_diag_k_dc() call is
 * dead code from this executable's point of view -- but it is still compiled
 * into zone_coupling_solve.o, so the symbol must resolve at link time. */
bool zones_config_get_coupling_diag_k_dc(uint8_t zone_index, float *out_k_dc)
{
    (void)zone_index;
    (void)out_k_dc;
    return false;
}
bool zones_config_get_coupling_tau(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) out_row[j] = s_fake_coupling_tau[zone_index][j];
    return true;
}
bool zones_config_get_coupling_dead_time(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) out_row[j] = s_fake_coupling_dead[zone_index][j];
    return true;
}
bool zones_config_set_coupling_cell(uint8_t zone_index, uint8_t neighbor_index, float coeff, float tau_s,
                                     float dead_time_s)
{
    SETTER_LOCK_PROBE(); // F3: adaptive_tune_lock must not be held here
    if (zone_index >= TEST_MAX_ZONES || neighbor_index >= TEST_MAX_ZONES) return false;
    s_fake_coupling[zone_index][neighbor_index] = coeff;
    s_fake_coupling_tau[zone_index][neighbor_index] = tau_s;
    s_fake_coupling_dead[zone_index][neighbor_index] = dead_time_s;
    return true;
}

// U2 (2026-09-01, PID_EXPANSION_PLAN.md 3.3 "consolidate the opt-in flag"):
// tiny in-RAM fake for the opt-in flag's new home, same "one file, one
// table" convention as s_fake_zone_cfg above -- stands in for zones_config_
// accessors.c's real (NVS-backed) zone_cfg_t::adaptive_tune_enabled.
static bool s_fake_adaptive_enabled[TEST_MAX_ZONES];
bool zones_config_get_adaptive_tune_enabled(uint8_t zone_index)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    return s_fake_adaptive_enabled[zone_index];
}
bool zones_config_set_adaptive_tune_enabled(uint8_t zone_index, bool enabled)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    s_fake_adaptive_enabled[zone_index] = enabled;
    return true;
}

// U1 (one-click revert): adaptive_tune_revert() calls profile_executor_get_
// status() to refuse mid-firing -- fake it as a simple settable state, same
// "own it here" convention as everything else on this page. Defaults to
// IDLE (no firing), matching a board that has never run a profile.
static profile_exec_state_t s_fake_exec_state = PROFILE_EXEC_IDLE;
void profile_executor_get_status(profile_exec_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = s_fake_exec_state;
}
/* adaptive_tune_revert() now calls this narrow accessor instead of the
 * full-struct getter (2026-09-22 exec-status stack-local audit) -- derives
 * the same RUNNING-or-PAUSED answer from the same s_fake_exec_state the
 * fake above uses. */
bool profile_executor_get_active_id(uint8_t *out_id)
{
    if (out_id) {
        *out_id = 0;
    }
    return (s_fake_exec_state == PROFILE_EXEC_RUNNING || s_fake_exec_state == PROFILE_EXEC_PAUSED);
}

// R2 (2026-09-01, opus review of commit 7c47683) / S2+S3 (2026-09-01 audit
// of ae5905f): the busy-modeling uart_bridge_ext_run_on_flash_worker()/
// uart_bridge_ext_is_on_flash_worker() stub used to live only here, hand-
// rolled. It is now shared -- see stubs/bx_worker_stub.h's own header
// comment for why (test_profile_executor_prestart.c linked the real
// adaptive_tune.c and exercised profile_executor_halt()'s re-entrant path
// through the OLD, blind, bare `fn(arg); return ESP_OK;` shape this
// replaced) and for the self-consistency fix (the stub now sets
// s_stub_on_flash_worker itself for the duration of fn(arg), rather than
// callers setting it by hand).
#include "bx_worker_stub.h"

// No httpd fakes needed here any more -- adaptive_tune.c's HTTP surface
// moved to adaptive_tune_http.c (2026-09-01 split), which this file does not
// #include, so adaptive_tune.c itself now has no httpd_*/wifi_provision_
// http_* symbols left to satisfy.

// 2026-09-01 split (adaptive_tune_internal.h's own doc comment has the
// full shape): adaptive_tune.c no longer contains the model refine or the
// Ki diagnosis -- both moved to their own files. #include all three here,
// same "one TU, own executable" convention as test_profile_executor_
// prestart.c uses for the profile_executor.c split.
#include "../drivers/control/adaptive_tune.c"
#include "../drivers/control/adaptive_tune_model.c"
#include "../drivers/control/adaptive_tune_ki.c"
static bool zone_write_in_flight_unlocked(uint8_t zone_index)
{
    return adaptive_tune_zones[zone_index].write_in_flight; // test-only raw read: the caller already holds what it needs
}

#ifdef _WIN32
#include <direct.h>
#define ATCF_MKDIR(p) _mkdir(p)
#define ATCF_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define ATCF_MKDIR(p) mkdir((p), 0755)
#define ATCF_RMDIR(p) rmdir(p)
#endif
#include "cfg_fs.h" /* cfg_fs_init()/_deinit()/_is_available()/_exists() -- the ki-baseline
                       * cfg_fs tests below call these directly. Was relying on an implicit
                       * declaration (C4013); now an error. */

static const char *AT_SCRATCH_BASE = "cfg_fs_test_adaptive_tune";

/* Saves are cfg-file-only since the NVS dual-write close, so every test that
 * persists the Ki baseline needs a freshly mounted cfg scratch directory.
 * Also usable by the #included test_adaptive_tune_ki_bounds.c. */
static void at_mount_scratch(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", AT_SCRATCH_BASE, ADAPTIVE_TUNE_KIBASE_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", AT_SCRATCH_BASE, ADAPTIVE_TUNE_KIBASE_FILE_PATH);
    remove(path);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", AT_SCRATCH_BASE);
    ATCF_RMDIR(tmp);
    ATCF_RMDIR(AT_SCRATCH_BASE);
    ATCF_MKDIR(AT_SCRATCH_BASE);
    cfg_fs_deinit();
    (void)cfg_fs_init(AT_SCRATCH_BASE, NULL);
}

/* Stages the blob+rev a LEGACY (pre dual-write-close) firmware left in NVS. */
static void at_stage_legacy_kibase(const adaptive_tune_kibase_blob_t *kb, uint32_t rev)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, ADAPTIVE_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ADAPTIVE_TUNE_NVS_PARTITION) == HAL_OK) {
        hal_kv_set_blob(&h, ADAPTIVE_TUNE_NVS_KEY_KIBASE, kb, sizeof(*kb));
        hal_kv_set_u32(&h, ADAPTIVE_TUNE_NVS_KEY_KIBASE_REV, rev);
        hal_kv_commit(&h);
        hal_kv_close(&h);
    }
}

// ---------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------

// Quantizes to 0.1 degC -- history_pack()'s own resolution (profile_
// executor.c) and well above the MAX31856's real ~0.0078 degC LSB, so this
// is coarser-than-hardware quantization, not an idealized float. Every
// temperature fed into these tests goes through this -- see this repo's
// standing "unquantized synthetic input hides whole branches" trap.
static float q1(float c) { return roundf(c * 10.0f) / 10.0f; }

static void reset_module_state(void)
{
    memset(adaptive_tune_zones, 0, sizeof(adaptive_tune_zones));
    memset(s_fake_zone_cfg, 0, sizeof(s_fake_zone_cfg));
    memset(s_fake_adaptive_enabled, 0, sizeof(s_fake_adaptive_enabled));
    s_fake_exec_state = PROFILE_EXEC_IDLE;
    memset(s_fake_coupling, 0, sizeof(s_fake_coupling));
    memset(s_fake_coupling_tau, 0, sizeof(s_fake_coupling_tau));
    memset(s_fake_coupling_dead, 0, sizeof(s_fake_coupling_dead));
    for (int i = 0; i < TEST_MAX_ZONES; i++) {
        s_fake_zone_cfg[i].tau_s = 200.0f;
        s_fake_zone_cfg[i].dead_time_s = 20.0f;
    }
    memset(adaptive_tune_joint_ring, 0, sizeof(adaptive_tune_joint_ring));
    adaptive_tune_joint_ring_count = 0;
    adaptive_tune_joint_ring_head = 0;
    adaptive_tune_joint_observations_lifetime = 0;
    memset(adaptive_tune_joint_last_duty, 0, sizeof(adaptive_tune_joint_last_duty));
    memset(adaptive_tune_joint_last_rise_c, 0, sizeof(adaptive_tune_joint_last_rise_c));
    memset(adaptive_tune_joint_last_valid, 0, sizeof(adaptive_tune_joint_last_valid));
    adaptive_tune_joint_dwell_row_committed = false;
    s_fake_control_mode_fail = false;
    s_fake_fuzzy_pct_fail = false;
    memset(s_stub_zone_is_on_off, 0, sizeof(s_stub_zone_is_on_off));
    memset(s_stub_zone_is_monitor_only, 0, sizeof(s_stub_zone_is_monitor_only));
    memset(adaptive_tune_ki_clear_gen, 0, sizeof(adaptive_tune_ki_clear_gen));
    s_set_model_hook = NULL;
    s_set_pid_hook = NULL;
    s_get_pid_in_flight_hook = NULL;
}

// Ticks a single settled dwell into zone zi: `ticks` ticks of dt_s seconds
// each, temperature held at q1(target_c) the whole time (quantized, exactly
// as a real settled reading would round), duty held at `duty`.
static void feed_settled_dwell(uint8_t zi, float target_c, float ambient_c, float duty, int ticks, float dt_s)
{
    // One non-dwelling tick first, unconditionally, so this dwell always
    // starts its OWN fresh settle window -- otherwise a second call for the
    // same zone (simulating the next dwell in a multi-segment profile)
    // would still read as a continuation of whichever dwell came before it,
    // and recorded_this_dwell from that earlier dwell would silently
    // swallow every tick here. A real profile always ramps between two
    // dwells, which is exactly this reset edge on real hardware too.
    adaptive_tune_zone_tick(zi, q1(target_c), true, duty, false, ambient_c, dt_s);
    float c = q1(target_c);
    for (int i = 0; i < ticks; i++) {
        adaptive_tune_zone_tick(zi, c, true, duty, true, ambient_c, dt_s);
    }
}

// Ticks ALL MAX31856_CHANNEL_COUNT zones through one JOINT settled dwell --
// every zone's tick for a given loop iteration happens before the next
// iteration for any zone, so every zone settles at the same simulated
// instant (matching a real profile, where every enabled zone shares the
// same segment boundaries -- see adaptive_tune_joint_dwell_row_committed's own comment
// on why that assumption is what makes one dwell commit exactly one joint
// row). target_c/duty are per-zone arrays of length MAX31856_CHANNEL_COUNT.
static void feed_joint_settled_dwell(const float target_c[MAX31856_CHANNEL_COUNT], float ambient_c,
                                      const float duty[MAX31856_CHANNEL_COUNT], int ticks, float dt_s)
{
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        adaptive_tune_zone_tick(zi, q1(target_c[zi]), true, duty[zi], false, ambient_c, dt_s);
    }
    for (int i = 0; i < ticks; i++) {
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            adaptive_tune_zone_tick(zi, q1(target_c[zi]), true, duty[zi], true, ambient_c, dt_s);
        }
    }
}

// A dwell that never actually settles: temperature keeps climbing the whole
// window at a slope well above the settle floor.
static void feed_unsettled_dwell(uint8_t zi, float start_c, float ambient_c, float duty, int ticks, float dt_s,
                                  float climb_c_per_tick)
{
    adaptive_tune_zone_tick(zi, start_c, true, duty, false, ambient_c, dt_s); // fresh dwell window, see feed_settled_dwell()
    float c = start_c;
    for (int i = 0; i < ticks; i++) {
        c = q1(c + climb_c_per_tick);
        adaptive_tune_zone_tick(zi, c, true, duty, true, ambient_c, dt_s);
    }
}

// Feeds a dwell with FLAT (quantized) temperature -- slope == 0, always
// clears ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S -- but a per-tick DUTY
// sequence supplied by the caller, so a fixture can reproduce the real
// defect shape: temperature reads settled while duty is still moving.
// `duties` has length `ticks`.
static void feed_flat_temp_oscillating_duty_dwell(uint8_t zi, float target_c, float ambient_c, const float *duties,
                                                    int ticks, float dt_s)
{
    float c = q1(target_c);
    adaptive_tune_zone_tick(zi, c, true, duties[0], false, ambient_c, dt_s); // fresh dwell window
    for (int i = 0; i < ticks; i++) {
        adaptive_tune_zone_tick(zi, c, true, duties[i], true, ambient_c, dt_s);
    }
}

static profile_firing_run_record_t make_clean_record(uint8_t profile_id, uint8_t zi, uint32_t samples)
{
    profile_firing_run_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.profile_id = profile_id;
    rec.zones[zi].active = true;
    rec.zones[zi].stats.sample_count = samples;
    rec.zones[zi].stats.excluded_sample_count = 0;
    return rec;
}

// 30s ticks. 2026-09-14 correction (docs/audits/adaptive_tune_harvest_gate_
// 2026-09-14.md): SETTLE_TICKS was 7 (210s), enough to clear ADAPTIVE_TUNE_
// SETTLE_MIN_S (180s) alone, back when the duty-stability check had no
// minimum-history requirement of its own. It now does: the duty-stability
// window is a bucketed sliding window (ADAPTIVE_TUNE_DUTY_WINDOW_BUCKET_S *
// _NUM_BUCKETS == 300s) that refuses a verdict outright until it holds a
// full span of history, so ANY fixture that expects a harvested observation
// must run long enough to mature that window too, not just the temperature
// gate. 11 ticks * 30s == 330s comfortably clears both (300s duty-window
// maturity, with one tick of margin against the exact boundary; 180s
// temperature settle was already satisfied by tick 6).
#define DT_S 30.0f
#define SETTLE_TICKS 11

// ---------------------------------------------------------------------
// Test bodies -- split across sibling files (2026-09-01) to keep this file
// under this repo's 1500-line guidance. #included (not separately compiled)
// so every file below shares the fakes and helpers defined above in this
// same translation unit -- same convention as adaptive_tune.c's own
// #include of adaptive_tune_model.c/adaptive_tune_ki.c a few lines up.
// Pure refactor: no test removed, no assertion changed, no comment dropped,
// no production code touched. See each file's own header comment for what
// it covers and why that's the seam.
// ---------------------------------------------------------------------
#include "test_adaptive_tune_dwell.c"
#include "test_adaptive_tune_model.c"
#include "test_adaptive_tune_coupled.c"
#include "test_adaptive_tune_ki_verdict.c"
#include "test_adaptive_tune_ki_bounds.c"
#include "test_adaptive_tune_status.c"

void run_test_adaptive_tune(void)
{
    TEST_SECTION("adaptive_tune: settling");
    test_unsettled_dwell_is_not_recorded();
    test_settled_dwell_is_recorded();
    test_oscillating_duty_flat_temperature_is_refused();
    test_genuinely_steady_duty_is_still_accepted();
    test_overshooting_entry_dwell_still_harvests(); // 2026-09-14 harvest-gate fix
    test_persistent_duty_oscillation_never_harvests(); // 2026-09-14 harvest-gate fix
    test_window_aligned_sawtooth_never_harvests(); // 2026-09-14 sliding-window correction

    TEST_SECTION("adaptive_tune: opt-in default off");
    test_opt_in_default_off_records_nothing();
    test_run_end_skips_disabled_zone_even_with_ring_data_present(); // P3
    test_run_end_skips_on_off_zone_even_with_ring_data_present(); // 2026-09-14 on/off gap

    TEST_SECTION("adaptive_tune: public accessor surface (same one adaptive_tune_http.c calls)");
    test_default_off_for_every_zone();
    test_enable_one_zone_leaves_others_untouched();
    test_enable_round_trips_through_persistence();
    test_status_reports_engine_held_fields_not_test_written_values();

    TEST_SECTION("adaptive_tune: guards");
    test_min_observations_guard_rejects_too_few();
    test_run_end_holds_no_lock_across_zones_setters(); // F3
    test_duty_spread_guard_rejects_clustered_observations();
    test_duty_spread_guard_pins_exact_threshold(); // P4
    test_implausible_jump_guard_rejects_far_off_fit();
    test_dirty_run_is_never_training_data();
    test_per_run_move_is_bounded_even_with_many_dwells();

    TEST_SECTION("adaptive_tune: refinement improves the estimate");
    test_refinement_improves_gain_estimate_on_known_plant();
    test_repeated_accepted_refinements_stay_within_baseline_envelope();
    test_adversarial_refinement_sequences_stay_within_baseline_envelope();

    TEST_SECTION("adaptive_tune: coupled identification -- pure fit");
    test_coupled_fit_refuses_underdetermined_observation_set();
    test_coupled_fit_refuses_ill_conditioned_observations();
    test_coupled_fit_recovers_known_asymmetric_matrix();
    test_coupled_fit_refuses_negative_coefficient();
    test_coupled_fit_refuses_nondominant_diagonal();
    test_coupled_fit_accepts_plausible_matrix();

    TEST_SECTION("adaptive_tune: Ki diagnosis from dwells");
    test_ki_diagnose_insufficient_below_min_samples();
    test_ki_diagnose_steady_offset_flags_small_ki();
    test_ki_diagnose_floored_not_misdiagnosed_as_small_ki();
    test_ki_diagnose_monotonic_drift_is_not_misread_as_oscillation();
    test_ki_diagnose_monotonic_drift_with_steady_offset_flags_small_ki();
    test_ki_diagnose_floored_wins_even_with_monotonic_drift();
    test_ki_diagnose_flat_duty_mid_range_is_not_floored();
    test_ki_diagnose_near_rail_but_varying_is_not_floored();
    test_ki_diagnose_limit_cycle_yields_ku_tu();
    test_ki_diagnose_irregular_hunting_yields_oscillating();
    test_ki_diagnose_ok_when_tracking_cleanly();

    TEST_SECTION("adaptive_tune: coupling cell convergence (D1)");
    test_coupling_cell_converges_from_zero_prior_over_repeated_runs();
    test_coupling_cell_per_run_move_is_bounded_by_abs_cap();
    test_coupling_ratio_guard_upper_bound_accepts_high_fit_from_low_confident_prior();
    test_coupling_ratio_guard_upper_bound_rejects_fit_above_absolute_ceiling();
    test_coupled_refine_skips_on_off_column_leaves_stored_cell_untouched();
    test_coupled_refine_skips_monitor_only_column_leaves_stored_cell_untouched();
    test_run_end_skips_monitor_only_zone_row_and_model_untouched();

    TEST_SECTION("adaptive_tune: joint observation floor counts distinct dwells (D4)");
    test_joint_floor_counts_distinct_dwells_not_rows();

    TEST_SECTION("adaptive_tune: coupled apply orientation (D6)");
    test_coupled_apply_writes_cells_in_storage_orientation();

    TEST_SECTION("adaptive_tune: model refine and Ki diagnosis do not stack (D5)");
    test_ki_diagnosis_skipped_same_run_as_model_refine();

    TEST_SECTION("adaptive_tune: Ki diagnosis eventually gets a turn (F1)");
    test_ki_diagnosis_eventually_runs_after_repeated_converging_refinements();

    TEST_SECTION("adaptive_tune: skipped Ki diagnosis clears stale status fields (F2)");
    test_ki_status_fields_cleared_when_diagnosis_skipped();

    TEST_SECTION("adaptive_tune: full-skip run_end paths clear ALL stale status fields (H2)");
    test_run_status_fields_cleared_on_faulted_run();
    test_run_status_fields_cleared_on_excluded_fraction_refusal();
    test_run_status_fields_cleared_when_zone_masked_out_of_profile();

    TEST_SECTION("adaptive_tune: Ki diagnosis never applies any correction any more (K9, diagnostic-only)");
    test_ki_diagnosis_never_applies_any_verdict();

    TEST_SECTION("adaptive_tune: dwell-entry bookkeeping survives invalid data / late enable (F3)");
    test_dwelling_prev_tracks_dwelling_state_even_when_data_is_invalid();
    test_enabling_zone_mid_dwell_does_not_reopen_committed_joint_row();

    TEST_SECTION("adaptive_tune: Ki baseline survives a reboot, latched by SIMC only now (P1/K9)");
    test_ki_baseline_survives_reboot_not_relatched_from_grown_ki();

    TEST_SECTION("adaptive_tune: re-autotune actually clears the Ki-diagnosis baseline (Q3/K9)");
    test_clear_ki_baseline_lets_the_next_refine_relatch_fresh();

    TEST_SECTION("adaptive_tune: accept-path clear_ki_baseline() does not re-enter the flash worker (R1)");
    test_accept_path_clear_ki_baseline_does_not_reenter_worker();

    TEST_SECTION("adaptive_tune: halt-path run_end() does not re-enter the flash worker (S1)");
    test_halt_path_run_end_does_not_reenter_worker();

    TEST_SECTION("adaptive_tune: the model layer keeps the Ki baseline tracking its own SIMC output (Q4)");
    test_model_refine_relatches_ki_baseline_to_fresh_simc_ki();

    TEST_SECTION("adaptive_tune: fuzzy + adaptive_tune run concurrently with no interlock, no ratchet (K9)");
    test_ki_diagnosis_never_ratchets_with_fuzzy_and_adaptive_tune_concurrent();

    TEST_SECTION("adaptive_tune: U2 opt-in flag migration out of the old NVS namespace");
    test_migrate_pulls_old_mask_into_zone_config();
    test_migrate_does_not_reconsult_old_mask_after_first_migration();
    test_migrate_handles_absent_old_key();

    TEST_SECTION("adaptive_tune: U1 one-click revert");
    test_revert_restores_exact_prior_gains_and_ki_baseline();
    test_revert_refuses_when_nothing_to_revert();
    test_revert_refuses_while_firing_active();

    TEST_SECTION("adaptive_tune: F3 follow-up -- revert vs run_end's unlocked apply window");
    test_revert_during_run_end_apply_is_refused_busy();
    test_run_end_during_revert_write_skips_the_zone();
    test_ki_clear_gen_is_per_zone();
    test_any_write_in_flight_covers_the_apply_window();
    test_apply_skips_zone_whose_gains_changed_since_plan();
}

// ---------------------------------------------------------------------
// cfg-filesystem dual-write bridge for the Ki baseline blob
// (docs/FILESYSTEM_USER_DATA.md section 5, item 9 -- "adaptive-tune
// state", the simplified/re-derivable treatment). Uses the real
// adaptive_tune_init()/adaptive_tune_clear_ki_baseline() public entry
// points plus a real cfg_fs.c against a temp directory.
// ---------------------------------------------------------------------
static void reset_all_cfg_fs_at(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", AT_SCRATCH_BASE, ADAPTIVE_TUNE_KIBASE_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", AT_SCRATCH_BASE, ADAPTIVE_TUNE_KIBASE_FILE_PATH);
    remove(path);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", AT_SCRATCH_BASE);
    ATCF_RMDIR(tmp);
    ATCF_RMDIR(AT_SCRATCH_BASE);
    ATCF_MKDIR(AT_SCRATCH_BASE);

    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
    reset_module_state();
    fake_kv_reset_all();
    hal_kv_init_partition(ADAPTIVE_TUNE_NVS_PARTITION);
    s_kibase_rev = 0;
}

static void test_kibase_cfg_fs_partition_absent_behaves_like_before(void)
{
    TEST_SECTION("adaptive_tune ki-baseline cfg_fs: partition absent -- a legacy NVS copy still loads, a save "
                 "fails loud and never falls back to NVS");
    reset_all_cfg_fs_at();
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs never mounted in this test");

    adaptive_tune_kibase_blob_t legacy;
    memset(&legacy, 0, sizeof(legacy));
    legacy.mask = 0x02;
    legacy.vals[1] = 3.5f;
    at_stage_legacy_kibase(&legacy, 1);
    adaptive_tune_init();
    TEST_CHECK(adaptive_tune_zones[1].ki_baseline_valid && adaptive_tune_zones[1].ki_baseline == 3.5f,
               "a legacy NVS-only board still loads its baseline");

    adaptive_tune_zones[1].ki_baseline = 4.5f;
    kibase_job_t job = {.result = ESP_OK};
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        job.blob.vals[zi] = adaptive_tune_zones[zi].ki_baseline;
        if (adaptive_tune_zones[zi].ki_baseline_valid) job.blob.mask |= (uint8_t)(1u << zi);
    }
    save_kibase_job(&job);
    TEST_CHECK(job.result != ESP_OK, "save fails loud with no `cfg` partition mounted");

    memset(adaptive_tune_zones, 0, sizeof(adaptive_tune_zones));
    adaptive_tune_init();
    TEST_CHECK(adaptive_tune_zones[1].ki_baseline_valid && adaptive_tune_zones[1].ki_baseline == 3.5f,
               "the NVS copy was NOT overwritten by the failed save (no fallback)");
}

static void test_kibase_cfg_fs_migrates_then_prefers_file(void)
{
    TEST_SECTION("adaptive_tune ki-baseline cfg_fs: NVS fallback migrates to file; a later boot prefers it");
    reset_all_cfg_fs_at();
    TEST_CHECK(cfg_fs_init(AT_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    adaptive_tune_zones[2].ki_baseline_valid = true;
    adaptive_tune_zones[2].ki_baseline = 7.25f;
    kibase_job_t job = {.result = ESP_FAIL};
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        job.blob.vals[zi] = adaptive_tune_zones[zi].ki_baseline;
        if (adaptive_tune_zones[zi].ki_baseline_valid) job.blob.mask |= (uint8_t)(1u << zi);
    }
    save_kibase_job(&job);
    TEST_CHECK(job.result == ESP_OK, "cfg-only save succeeds");

    bool exists = false;
    TEST_CHECK(cfg_fs_exists(ADAPTIVE_TUNE_KIBASE_FILE_PATH, &exists) == ESP_OK && exists,
               "the save actually created the file");

    memset(adaptive_tune_zones, 0, sizeof(adaptive_tune_zones));
    adaptive_tune_init();
    TEST_CHECK(adaptive_tune_zones[2].ki_baseline_valid && adaptive_tune_zones[2].ki_baseline == 7.25f,
               "reloaded baseline matches what was saved");

    adaptive_tune_kibase_blob_t raw;
    uint32_t rev = 0;
    bool valid = false;
    pref_cfg_fs_load_raw(ADAPTIVE_TUNE_KIBASE_FILE_PATH, sizeof(raw), adaptive_tune_kibase_file_validate, &raw, &rev, &valid);
    TEST_CHECK(valid && rev == 1, "the file holds a rev-1 copy after one save");
}

// NEGATIVE TEST: if save_kibase_job()'s file write is skipped (the
// production call deleted), the file must never catch up -- proving the
// dual-write is load-bearing, not decorative, even for this simplified,
// "re-derivable" item.
static esp_err_t at_failing_write_fn(const char *rel_path, const void *data, size_t len)
{
    (void)rel_path; (void)data; (void)len;
    return ESP_FAIL;
}

static void test_kibase_cfg_fs_negative_no_file_write_means_file_never_catches_up(void)
{
    TEST_SECTION("adaptive_tune ki-baseline cfg_fs NEGATIVE TEST: skipped file write leaves the file "
                 "permanently behind");
    reset_all_cfg_fs_at();
    TEST_CHECK(cfg_fs_init(AT_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    pref_cfg_fs_set_write_fn(at_failing_write_fn); // stands in for "the file-write call was deleted"
    adaptive_tune_zones[0].ki_baseline_valid = true;
    adaptive_tune_zones[0].ki_baseline = 9.0f;
    kibase_job_t job = {.result = ESP_FAIL};
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        job.blob.vals[zi] = adaptive_tune_zones[zi].ki_baseline;
        if (adaptive_tune_zones[zi].ki_baseline_valid) job.blob.mask |= (uint8_t)(1u << zi);
    }
    save_kibase_job(&job);
    TEST_CHECK(job.result != ESP_OK, "a failed cfg write is reported -- there is no NVS fallback any more");
    pref_cfg_fs_reset_write_fn_for_test();

    adaptive_tune_kibase_blob_t raw;
    uint32_t rev = 0;
    bool valid = false;
    pref_cfg_fs_load_raw(ADAPTIVE_TUNE_KIBASE_FILE_PATH, sizeof(raw), adaptive_tune_kibase_file_validate, &raw, &rev, &valid);
    TEST_CHECK(!valid, "with the file write skipped there is no file, and no NVS copy was written either");
}

static void test_kibase_status_padding_is_not_data(void)
{
    TEST_SECTION("adaptive_tune ki-baseline status: file and NVS identical except for padding bytes read as in "
                 "sync; a real value difference is still diverged");
    reset_all_cfg_fs_at();
    TEST_CHECK(cfg_fs_init(AT_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    adaptive_tune_kibase_blob_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.mask = 0x02;
    fb.vals[1] = 4.5f;
    adaptive_tune_kibase_blob_t nb = fb;
    unsigned char *fr = (unsigned char *)&fb;
    unsigned char *nr = (unsigned char *)&nb;
    for (size_t i = 1; i < offsetof(adaptive_tune_kibase_blob_t, vals); i++) { /* gap after `mask` */
        fr[i] = 0xAA;
        nr[i] = 0x55;
    }
    TEST_CHECK(memcmp(&fb, &nb, sizeof(fb)) != 0, "test setup: blobs differ only in padding");
    TEST_CHECK(pref_cfg_fs_save(ADAPTIVE_TUNE_KIBASE_FILE_PATH, &fb, sizeof(fb), 1) == ESP_OK, "file written");
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, ADAPTIVE_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ADAPTIVE_TUNE_NVS_PARTITION) == HAL_OK,
               "open NVS");
    TEST_CHECK(hal_kv_set_blob(&h, ADAPTIVE_TUNE_NVS_KEY_KIBASE, &nb, sizeof(nb)) == HAL_OK, "NVS blob written");
    TEST_CHECK(hal_kv_set_u32(&h, ADAPTIVE_TUNE_NVS_KEY_KIBASE_REV, 1) == HAL_OK, "NVS rev written");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    bool fv = false, nv = false, div = true;
    uint32_t frv = 0, nrv = 0;
    adaptive_tune_get_kibase_dualwrite_status(&fv, &frv, &nv, &nrv, &div);
    TEST_CHECK(fv && nv, "both sides valid");
    TEST_CHECK(!div, "padding-only difference reads as in sync");

    fb.vals[1] = 5.5f;
    TEST_CHECK(pref_cfg_fs_save(ADAPTIVE_TUNE_KIBASE_FILE_PATH, &fb, sizeof(fb), 1) == ESP_OK, "file value changed");
    adaptive_tune_get_kibase_dualwrite_status(&fv, &frv, &nv, &nrv, &div);
    TEST_CHECK(div, "a real value difference is still diverged");
}

int main(void)
{
    run_test_adaptive_tune();
    test_kibase_cfg_fs_partition_absent_behaves_like_before();
    test_kibase_cfg_fs_migrates_then_prefers_file();
    test_kibase_cfg_fs_negative_no_file_write_means_file_never_catches_up();
    test_kibase_status_padding_is_not_data();
    reset_all_cfg_fs_at();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
