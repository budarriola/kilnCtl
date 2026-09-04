// test_display_power_wiring.c -- proves display_power_policy_step() (the
// pure decision core, host-tested exhaustively by test_display_power_
// policy.c) is actually WIRED into screen_idle.c/lvgl_port.c on real
// hardware, and fed REAL producers for firing_active/error_active rather
// than a test stub or a value nothing writes -- the exact "consumer
// without producer"/"idealized wiring" gap this codebase has shipped
// before (see project_consumer_without_producer_class and project_split_
// module_missing_name_class in the durable memory).
//
// THE PROBLEM THIS FILE SOLVES: screen_idle.c and lvgl_port.c are not
// host-compilable -- they #include panel_spi.h/NS2009.h/lvgl.h/FreeRTOS
// headers, none of which exist off real hardware (screen_idle.h's own
// header comment: "panel_spi.h fails outright against the host-test stub
// set", the exact reason backlight_pwm.h keeps screen_idle_t behind a
// `const void *`). test_safety_core_s8_wiring.c (SaftyFW) established the
// precedent this file follows: fall back to a source-text scan of the real
// .c files rather than compiling them, extract the specific function body
// under test so a match can only land inside it (not a stray comment or an
// unrelated function), and FAIL CLOSED -- if the file or the expected
// symbol cannot be located at all, this test fails rather than silently
// reporting nothing wrong.
//
// This intentionally does NOT re-verify display_power_policy_step()'s own
// logic (test_display_power_policy.c already does that exhaustively,
// host-compiled, on the real compiled function) -- it verifies the WIRING
// around it: that screen_idle.c calls it at all, that lvgl_port.c's touch
// callback actually gates delivery on the swallow verdict, and that the
// firing/error inputs are read from the REAL producers (profile_executor_
// get_status()'s PROFILE_EXEC_RUNNING/PAUSED -- the same accessor boot_
// button.c/danger_mode.c/gpio_probe.c already use -- and dashboard_get_
// status()'s diag_ever_received/diag_state/diag_age_ms, the identical
// fields+gate ui_page_home.c's own safety-trip strip keys off) rather than
// a placeholder that always reads false/true or a variable nothing else
// writes.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

static char *read_file_any(const char *const *candidates, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (!f) {
            continue;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            continue;
        }
        long len = ftell(f);
        if (len < 0) {
            fclose(f);
            continue;
        }
        rewind(f);
        char *buf = (char *)malloc((size_t)len + 1);
        if (!buf) {
            fclose(f);
            return NULL;
        }
        size_t got = fread(buf, 1, (size_t)len, f);
        fclose(f);
        buf[got] = '\0';
        return buf;
    }
    return NULL;
}

// Extracts the body of a function from its exact declaration/signature
// substring (must be unique and include the opening paren, e.g.
// "static bool screen_idle_run_policy_locked(") through the first "\n}"
// after the opening brace -- this codebase's own functions close at column
// 0, same assumption SaftyFW's test_safety_core_s8_wiring.c and test_
// safety_core_stack_budget.c make.
static const char *find_function_body(const char *text, const char *sig, size_t *out_len)
{
    const char *s = strstr(text, sig);
    if (!s) {
        return NULL;
    }
    const char *open = strchr(s, '{');
    if (!open) {
        return NULL;
    }
    const char *close = strstr(open, "\n}");
    if (!close) {
        return NULL;
    }
    *out_len = (size_t)(close - open);
    return open;
}

static char *dup_range(const char *start, size_t len)
{
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}

static const char *SCREEN_IDLE_C_CANDIDATES[] = {
    "../drivers/screen_idle.c",
    "App/drivers/screen_idle.c",
    "firmware/KilnFW/App/drivers/screen_idle.c",
};
static const char *LVGL_PORT_C_CANDIDATES[] = {
    "../drivers/lvgl_port.c",
    "App/drivers/lvgl_port.c",
    "firmware/KilnFW/App/drivers/lvgl_port.c",
};

static void run_section1_screen_idle_calls_policy(void)
{
    TEST_SECTION("screen_idle.c actually calls display_power_policy_step() -- "
                 "source-text scan, screen_idle.c is not host-compilable (pulls in "
                 "panel_spi.h/NS2009.h -- see screen_idle.h's own header comment)");

    char *text = read_file_any(SCREEN_IDLE_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/screen_idle.c from the host test's "
                           "working directory -- update the candidate paths in this test if "
                           "the build layout moved");
        return;
    }

    size_t body_len = 0;
    const char *body =
        find_function_body(text, "static bool screen_idle_run_policy_locked(", &body_len);
    if (!body) {
        TEST_CHECK(false, "could not find screen_idle_run_policy_locked()'s function body in "
                           "screen_idle.c -- update this test if it was renamed/restructured. "
                           "If this function is simply gone, the display-power policy is no "
                           "longer wired into screen_idle.c at all.");
        free(text);
        return;
    }
    char *fn = dup_range(body, body_len);
    TEST_CHECK(fn != NULL, "malloc for the extracted function body succeeded");
    if (!fn) {
        free(text);
        return;
    }

    TEST_CHECK(strstr(fn, "display_power_policy_step(&in)") != NULL,
               "screen_idle_run_policy_locked() must actually call "
               "display_power_policy_step() -- the pure decision core this whole feature "
               "exists to wire in. If this fails, the call was deleted and screen_idle is "
               "back to driving screen_on some other way (or not at all).");

    // 2026-09-04 opus review: the producer reads moved OUT of screen_idle_run_
    // policy_locked() (which runs with idle->lock held) into screen_idle_
    // refresh_inputs(), which screen_idle_task calls with the lock NOT held --
    // see section 3b for why. They are still REAL producer reads and still
    // pinned here, just in the function that now performs them; extract that
    // body and hold it to the same standard.
    size_t refresh_len = 0;
    const char *refresh_body =
        find_function_body(text, "static void screen_idle_refresh_inputs(", &refresh_len);
    if (!refresh_body) {
        TEST_CHECK(false, "could not find screen_idle_refresh_inputs()'s function body in "
                           "screen_idle.c -- that is where the firing/error producer reads "
                           "live; if it is gone, this module is no longer reading them at all.");
        free(fn);
        free(text);
        return;
    }
    char *refresh_fn = dup_range(refresh_body, refresh_len);
    TEST_CHECK(refresh_fn != NULL, "malloc for the extracted refresh function body succeeded");
    if (!refresh_fn) {
        free(fn);
        free(text);
        return;
    }

    TEST_CHECK(strstr(fn, "idle->cached_firing_active") != NULL &&
                   strstr(fn, "idle->cached_error_active") != NULL,
               "screen_idle_run_policy_locked() must feed the policy from the idle->cached_* "
               "snapshot screen_idle_refresh_inputs() writes -- if this fails the policy is "
               "being fed from somewhere else entirely.");

    TEST_CHECK(strstr(refresh_fn, "profile_executor_get_status(&pst)") != NULL &&
                   strstr(refresh_fn, "PROFILE_EXEC_RUNNING") != NULL &&
                   strstr(refresh_fn, "PROFILE_EXEC_PAUSED") != NULL,
               "firing_active must be read from profile_executor_get_status()'s REAL state "
               "(RUNNING or PAUSED) -- the same accessor boot_button.c/danger_mode.c/"
               "gpio_probe.c already use for this exact question. If this fails, either the "
               "real producer call was deleted, or firing_active is being fed from something "
               "else (a stub, a hardcoded value) -- the 'consumer without producer' bug class "
               "this test exists to catch.");

    TEST_CHECK(strstr(refresh_fn, "dashboard_get_status(&ds)") != NULL &&
                   strstr(refresh_fn, "SAFETY_LINK_DIAG_STATE_TRIPPED") != NULL &&
                   strstr(refresh_fn, "diag_ever_received") != NULL && strstr(refresh_fn, "diag_age_ms") != NULL,
               "error_active must be read from dashboard_get_status()'s REAL diag_ever_"
               "received/diag_state/diag_age_ms fields, gated the identical way ui_page_"
               "home.c's own safety-trip strip is -- not a placeholder or a differently-gated "
               "notion of 'error'. If this fails, the real producer read was deleted or "
               "replaced.");

    TEST_CHECK(strstr(fn, ".timeout_setting = display_power_cfg_timeout_setting()") != NULL &&
                   strstr(fn, ".keep_on_while_firing = display_power_cfg_keep_on_while_firing()") !=
                       NULL &&
                   strstr(fn, ".display_on_error = display_power_cfg_display_on_error()") != NULL,
               "the three persisted settings (timeout/keep-on-while-firing/display-on-error) "
               "must be read from display_power_cfg.c's real accessors, not hardcoded -- "
               "otherwise the settings page's POSTed values would have no effect on the "
               "actual on-device behaviour.");

    // 2026-09-04 opus review: BOTH producers, on both inputs. The executor
    // alone does not know an autotune run is heating (autotune_engine drives
    // relays with the executor at PROFILE_EXEC_IDLE), and the safety link's
    // diag_state alone does not know the ESP's own global thermal-guard
    // abort happened (PROFILE_EXEC_FAULTED). Reading only one of each pair
    // is this codebase's documented "consumer reading a different producer
    // than the one that actually gets written" bug class.
    TEST_CHECK(strstr(refresh_fn, "autotune_engine_is_active();") != NULL,
               "firing_active must ALSO be true during an autotune run "
               "(autotune_engine_is_active()) -- otherwise 'keep display on while firing' "
               "blanks the panel mid-autotune, because autotune holds relay authority with "
               "profile_executor still at PROFILE_EXEC_IDLE.");

    TEST_CHECK(strstr(refresh_fn, "pst.state == PROFILE_EXEC_FAULTED") != NULL,
               "error_active must ALSO cover the ESP's own global thermal-guard abort "
               "(profile_executor's PROFILE_EXEC_FAULTED) -- the safety link's diag_state is "
               "the RP2040's own trip and is never set by an ESP-side guard fault, so keying "
               "'error' on it alone misses an entire class of error the owner expects to "
               "raise the display.");

    TEST_CHECK(strstr(fn, ".error_entered_this_tick = error_entered_this_tick") != NULL &&
                   strstr(fn, "error_active && !idle->error_prev_active") != NULL,
               "error_entered_this_tick must be computed as a real false->true EDGE against "
               "the module's own remembered previous level (idle->error_prev_active), not "
               "passed the raw level -- display_power_policy.h's own header comment explains "
               "why a level here breaks rule 5's 'resume normal timeout behaviour after the "
               "dismissing touch'. This is exactly the kind of subtle edge-vs-level bug this "
               "codebase has shipped before.");

    free(refresh_fn);
    free(fn);
    free(text);
}

static void run_section2_touch_swallow_wired(void)
{
    TEST_SECTION("screen_idle_touch_swallow() actually gates LVGL delivery in "
                 "lvgl_port.c's touch_read_cb() -- source-text scan, lvgl_port.c is not "
                 "host-compilable (pulls in lvgl.h/panel_spi.h/touch_dev.h)");

    char *text = read_file_any(LVGL_PORT_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/lvgl_port.c from the host test's working "
                           "directory -- update the candidate paths in this test if the build "
                           "layout moved");
        return;
    }

    size_t body_len = 0;
    const char *body = find_function_body(text, "static void touch_read_cb(", &body_len);
    if (!body) {
        TEST_CHECK(false, "could not find touch_read_cb()'s function body in lvgl_port.c -- "
                           "update this test if it was renamed/restructured. If this function "
                           "is simply gone, touch delivery into LVGL is wired some other way "
                           "and this test needs to move with it.");
        free(text);
        return;
    }
    char *fn = dup_range(body, body_len);
    TEST_CHECK(fn != NULL, "malloc for the extracted function body succeeded");
    if (!fn) {
        free(text);
        return;
    }

    // Count call sites -- the physical-touch path and the injected-touch
    // path each need their own screen_idle_touch_swallow() call (both are
    // real LVGL delivery points; see this file's own top comment on why an
    // injected touch must see the identical wake-swallows behaviour a
    // physical one does). Two, not one, not zero.
    int call_count = 0;
    const char *p = fn;
    while ((p = strstr(p, "screen_idle_touch_swallow(")) != NULL) {
        call_count++;
        p += 1;
    }
    TEST_CHECK(call_count >= 2,
               "touch_read_cb() must call screen_idle_touch_swallow() from BOTH the physical-"
               "touch path and the LVGL-side injected-touch path -- found fewer than 2 call "
               "sites. If this fails, one of the two touch sources can act on the UI "
               "underneath a wake tap while the other cannot, which is exactly the kind of "
               "one-sided wiring gap this codebase's 'Reset-one-side bug class' memory entry "
               "describes.");

    // Not just "the RELEASED-setting text appears somewhere in this function"
    // (a release-on-no-touch branch elsewhere in touch_read_cb would satisfy
    // that trivially even if the swallow gate itself were deleted) -- count
    // "if (swallow) {" gates specifically, and require at least 2 (one per
    // call site from the check above). Gutting just ONE gate's body (e.g.
    // deleting its `data->state = LV_INDEV_STATE_RELEASED;` line while
    // leaving the empty `if (swallow) { }` behind) is exactly the failure
    // mode section-1-style presence checks would miss; this counts the
    // gates, and the mutation caught by hand while writing this test was
    // dropping a `return;`/state-reset out of one of them, which a plain
    // "does this substring exist anywhere" check did not catch.
    int swallow_gate_count = 0;
    {
        const char *q = fn;
        while ((q = strstr(q, "if (swallow) {")) != NULL) {
            swallow_gate_count++;
            q += 1;
        }
    }
    TEST_CHECK(swallow_gate_count >= 2,
               "found fewer than 2 'if (swallow) {' gates in touch_read_cb() -- a true swallow "
               "verdict must actually change data->state back to RELEASED at EACH of the two "
               "call sites (physical and injected), otherwise that path's touch still reaches "
               "whatever widget is underneath, which is precisely rule 3/4's violation (\"the "
               "first touch only WAKES it -- must NOT act on the UI underneath\").");

    free(fn);
    free(text);
}

static void run_section3_recovery_mode_gated(void)
{
    TEST_SECTION("screen_idle does not call into subsystems "
                 "boot_guard.h RECOVERY MODE skips -- 2026-09-04 bench crash: "
                 "screen_idle_start() runs UNCONDITIONALLY (main_boot_early.c, before "
                 "main_control_bringup.c's recovery-mode skip of profile_executor_start()/"
                 "autotune_engine_start() even runs), so every poll tick called into those "
                 "un-started subsystems and the board interrupt-watchdog-panicked "
                 "(screen_idle_task -> screen_idle_unlock -> xQueueGenericSend, hardware-"
                 "confirmed via COM3/addr2line) forever, never confirming a healthy boot -- "
                 "495+ consecutive recovery-mode boots on the bench before this fix.");

    char *text = read_file_any(SCREEN_IDLE_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/screen_idle.c from the host test's "
                           "working directory -- update the candidate paths in this test if "
                           "the build layout moved");
        return;
    }

    // 2026-09-04 opus review: screen_idle_run_policy_locked() no longer makes
    // these calls at all -- they moved to screen_idle_refresh_inputs(), which
    // runs with idle->lock NOT held (section 3b). Pin BOTH halves: the locked
    // function must be free of them, and the gate must sit ahead of them where
    // they actually live.
    size_t policy_len = 0;
    const char *policy_body =
        find_function_body(text, "static bool screen_idle_run_policy_locked(", &policy_len);
    if (!policy_body) {
        TEST_CHECK(false, "could not find screen_idle_run_policy_locked()'s function body in "
                           "screen_idle.c -- update this test if it was renamed/restructured.");
        free(text);
        return;
    }
    char *policy_fn = dup_range(policy_body, policy_len);
    TEST_CHECK(policy_fn != NULL, "malloc for the extracted policy function body succeeded");
    if (!policy_fn) {
        free(text);
        return;
    }
    TEST_CHECK(strstr(policy_fn, "profile_executor_get_status(") == NULL &&
                   strstr(policy_fn, "dashboard_get_status(") == NULL &&
                   strstr(policy_fn, "autotune_engine_is_active(") == NULL,
               "screen_idle_run_policy_locked() runs with idle->lock HELD and must NOT make "
               "the producer calls itself -- see section 3b. If they come back here, both "
               "the recovery-mode gate below and the off-lock guarantee are gone at once.");
    free(policy_fn);

    size_t body_len = 0;
    const char *body =
        find_function_body(text, "static void screen_idle_refresh_inputs(", &body_len);
    if (!body) {
        TEST_CHECK(false, "could not find screen_idle_refresh_inputs()'s function body in "
                           "screen_idle.c -- this is THE recovery-mode gate site for the "
                           "2026-09-04 brick; update this test only if it genuinely moved.");
        free(text);
        return;
    }
    char *fn = dup_range(body, body_len);
    TEST_CHECK(fn != NULL, "malloc for the extracted function body succeeded");
    if (!fn) {
        free(text);
        return;
    }

    const char *gate = strstr(fn, "idle->recovery_mode");
    TEST_CHECK(gate != NULL,
               "screen_idle_refresh_inputs() must check idle->recovery_mode -- if this "
               "fails, the recovery-mode gate was deleted and the board bricks itself in "
               "recovery mode again the next time this module gains a new dependency on a "
               "subsystem recovery mode skips.");

    const char *pe_call = strstr(fn, "profile_executor_get_status(&pst)");
    const char *dash_call = strstr(fn, "dashboard_get_status(&ds)");
    const char *at_call = strstr(fn, "autotune_engine_is_active();");
    TEST_CHECK(pe_call != NULL && dash_call != NULL && at_call != NULL,
               "all three producer reads must live in screen_idle_refresh_inputs() -- a "
               "missing one means the input it feeds is no longer read at all, or moved back "
               "under the lock.");
    const char *gate_return = gate ? strstr(gate, "return;") : NULL;
    TEST_CHECK(gate_return != NULL && pe_call != NULL && gate_return < pe_call,
               "the idle->recovery_mode branch must RETURN before the producer calls, not "
               "merely mention the flag -- a gate that falls through is decorative.");
    TEST_CHECK(gate != NULL && pe_call != NULL && dash_call != NULL && at_call != NULL &&
                   gate < pe_call && gate < dash_call && gate < at_call,
               "the idle->recovery_mode check must appear BEFORE the profile_executor_get_"
               "status()/dashboard_get_status()/autotune_engine_is_active() calls in this "
               "function's source order, with a path that returns without falling through to "
               "them -- otherwise recovery mode still reaches the calls that bricked the "
               "board and the gate is decorative. This does not execute the function (see "
               "this file's own top comment on why screen_idle.c is not host-compilable); it "
               "proves the source ORDER a real recovery-mode boot would take.");

    free(fn);
    free(text);
}

static void run_section3b_refresh_called_off_lock(void)
{
    TEST_SECTION("screen_idle_task refreshes the producer snapshot OUTSIDE idle->lock, and "
                 "throttles it -- 2026-09-04 opus review of the recovery-mode/stack fix: the "
                 "producer reads used to run inline inside screen_idle_run_policy_locked(), "
                 "i.e. with the lock HELD, on every 50 ms tick. That is 20 Hz of five "
                 "MAX31856 SPI burst reads on the panel's own SPI host, a kiln_io_owner_"
                 "command_read() round trip that can block 200 ms, and two interrupts-"
                 "disabled heap walks -- all under a lock lvgl_port.c's task (screen_idle_"
                 "get_state(), every LVGL tick and every touch) and backlight_pwm.c's task "
                 "both take, and directly against screen_idle.c's own documented invariant "
                 "that the lock is 'never held across I2C or SPI traffic'.");

    char *text = read_file_any(SCREEN_IDLE_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/screen_idle.c");
        return;
    }
    size_t body_len = 0;
    const char *body = find_function_body(text, "static void screen_idle_task(", &body_len);
    if (!body) {
        TEST_CHECK(false, "could not find screen_idle_task()'s function body in screen_idle.c "
                           "-- update this test only if it was genuinely renamed.");
        free(text);
        return;
    }
    char *fn = dup_range(body, body_len);
    TEST_CHECK(fn != NULL, "malloc for the extracted task body succeeded");
    if (!fn) {
        free(text);
        return;
    }
    const char *refresh = strstr(fn, "screen_idle_refresh_inputs(idle)");
    const char *lock = strstr(fn, "screen_idle_lock(idle)");
    TEST_CHECK(refresh != NULL,
               "screen_idle_task must call screen_idle_refresh_inputs() -- without it the "
               "cached snapshot the policy reads is never written and firing/error would be "
               "permanently false: a consumer with no producer, the exact class this file "
               "exists to catch.");
    TEST_CHECK(lock != NULL,
               "could not find screen_idle_lock(idle) in screen_idle_task() -- update this "
               "test if the poll tick's locking was restructured.");
    TEST_CHECK(refresh != NULL && lock != NULL && refresh < lock,
               "screen_idle_refresh_inputs() must be called BEFORE screen_idle_lock() in the "
               "poll tick -- calling it inside the locked region is exactly the blocking-"
               "under-lock defect this section pins.");
    // ">= SCREEN_IDLE_INPUT_POLL_MS", not just the bare token: the token also
    // appears in this loop's own explanatory comment, so matching it alone
    // stayed green under a mutation that deleted the real comparison (proved
    // by hand, 2026-09-04 -- negative-test-every-check).
    TEST_CHECK(strstr(fn, ">= SCREEN_IDLE_INPUT_POLL_MS") != NULL,
               "the refresh must be throttled by SCREEN_IDLE_INPUT_POLL_MS, not run on every "
               "SCREEN_IDLE_POLL_MS touch tick -- dashboard_get_status()'s own doc comment "
               "sizes its cost against a browser poll / the 2 s LCD tick, not 20 Hz forever.");
    free(fn);
    free(text);
}

static void run_section4_screen_idle_init_takes_recovery_mode(void)
{
    TEST_SECTION("screen_idle_init()'s signature carries a recovery_mode parameter, and "
                 "main_boot_early.c's call site actually passes ctx->recovery_mode -- not a "
                 "hardcoded false, which would silently defeat section 3's gate on every "
                 "real recovery-mode boot while still passing it.");

    char *text = read_file_any(SCREEN_IDLE_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/screen_idle.c");
        return;
    }
    TEST_CHECK(strstr(text, "bool recovery_mode)") != NULL,
               "screen_idle_init() must take a recovery_mode parameter -- if this fails, "
               "screen_idle has no way to know boot_guard_is_recovery_mode()'s answer and "
               "section 3's idle->recovery_mode field can only ever be its zero-init value.");
    TEST_CHECK(strstr(text, "idle->recovery_mode = recovery_mode;") != NULL,
               "screen_idle_init() must store its recovery_mode argument into idle->"
               "recovery_mode -- if this fails, the parameter is accepted but discarded and "
               "the struct field never reflects the real boot state.");
    free(text);

    static const char *MAIN_BOOT_EARLY_CANDIDATES[] = {
        "../main_boot_early.c",
        "App/main_boot_early.c",
        "firmware/KilnFW/App/main_boot_early.c",
    };
    char *main_text = read_file_any(MAIN_BOOT_EARLY_CANDIDATES, 3);
    if (!main_text) {
        TEST_CHECK(false, "could not locate App/main_boot_early.c from the host test's "
                           "working directory -- update the candidate paths in this test if "
                           "the build layout moved");
        return;
    }
    TEST_CHECK(strstr(main_text, "screen_idle_init(&ctx->screen_idle, &ctx->display, NULL, "
                                  "ctx->recovery_mode)") != NULL,
               "main_boot_early.c must call screen_idle_init() with ctx->recovery_mode (set "
               "from boot_guard_is_recovery_mode() earlier in the same bring-up) -- passing "
               "a hardcoded false/true here would make section 3's source-order gate exist "
               "but never actually engage (or always engage) on real hardware.");
    free(main_text);
}

static void run_section5_screen_idle_stack_sized_for_dashboard(void)
{
    TEST_SECTION("screen_idle_task's stack is sized big enough for the deep call chain its "
                 "poll tick now makes -- 2026-09-04 bench crash: a normal (non-recovery) boot "
                 "corrupted the internal DRAM heap and panicked (LoadProhibited) inside "
                 "dashboard_get_status()'s heap_caps_get_largest_free_block() call, backtrace "
                 "screen_idle_task -> screen_idle_run_policy_locked -> dashboard_get_status -> "
                 "heap_caps_get_largest_free_block -> tlsf_walk_pool, on a heap block sitting "
                 "immediately past this task's own (undersized, 3072 B) stack.");

    char *text = read_file_any(SCREEN_IDLE_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/screen_idle.c");
        return;
    }

    TEST_CHECK(strstr(text, "\"screen_idle\", 3072,") == NULL,
               "screen_idle_task must not still be created with the old 3072-byte stack -- "
               "that size was never re-measured after profile_executor_get_status()/"
               "dashboard_get_status()/autotune_engine_is_active() were added to its poll "
               "tick's non-recovery-mode path, and it corrupted the heap on real hardware. "
               "Every OTHER dashboard_get_status() caller in this codebase runs on a task "
               "sized for it (lvgl_port.c's own task asks for 8192).");

    const char *create = strstr(text, "xTaskCreatePinnedToCore(screen_idle_task, \"screen_idle\", ");
    TEST_CHECK(create != NULL,
               "could not find screen_idle_task's xTaskCreatePinnedToCore() call -- update "
               "this test if it was renamed/restructured.");
    long stack_words = 0;
    if (create) {
        const char *num = create + strlen("xTaskCreatePinnedToCore(screen_idle_task, \"screen_idle\", ");
        stack_words = strtol(num, NULL, 10);
    }
    TEST_CHECK(stack_words >= 6144,
               "screen_idle_task's stack must be at least 6144 bytes (the deliberately "
               "generous doubling this fix landed with) -- if this fails, someone shrank it "
               "back toward the old, hardware-proven-too-small 3072 without a get_stack_"
               "margin() measurement backing the smaller number.");

    const char *reg = strstr(text, "stack_margin_register(\"screen_idle\", &s_task_handle, ");
    TEST_CHECK(reg != NULL, "could not find screen_idle's stack_margin_register() call.");
    long reg_words = 0;
    if (reg) {
        const char *num = reg + strlen("stack_margin_register(\"screen_idle\", &s_task_handle, ");
        reg_words = strtol(num, NULL, 10);
    }
    TEST_CHECK(reg != NULL && create != NULL && reg_words == stack_words,
               "stack_margin_register()'s size argument must match xTaskCreatePinnedToCore()'s "
               "-- a mismatch here makes get_stack_margin()'s headroom report wrong (comparing "
               "the real high-water mark against a size the task was not actually created "
               "with), silently hiding exactly the kind of undersized-stack bug this fix is "
               "for.");

    free(text);
}

static void run_section6_wake_invalidate_not_in_flush_cb(void)
{
    TEST_SECTION("the off->on wake edge's lv_obj_invalidate() call does NOT live inside "
                 "ili9488_flush_cb() -- 2026-09-04 bench task-watchdog crash: two clean "
                 "reproductions on an IDLE, non-firing board (timeout-driven blank, then a "
                 "single injected touch) both reset with reset_reason=TASK_WDT, "
                 "exc_task='lvgl', and a garbage exc_cause/pc/addr in the captured crash "
                 "record (the same 'corrupted beyond a trustworthy backtrace' signature as "
                 "e7b8efc's stack-overflow bug earlier the same day). ili9488_flush_cb() runs "
                 "from INSIDE lv_timer_handler()'s active refresh; calling lv_obj_invalidate() "
                 "from there reenters LVGL's own invalid-area walk mid-iteration, which LVGL "
                 "does not support from a flush callback. The fix moves wake detection to "
                 "lvgl_port_service_idle_wake(), called from lvgl_port_task's loop BEFORE "
                 "lv_timer_handler() runs -- see UI_PLAN.md's Display power section.");

    char *text = read_file_any(LVGL_PORT_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/lvgl_port.c");
        return;
    }

    size_t flush_len = 0;
    const char *flush_body = find_function_body(text, "static void ili9488_flush_cb(", &flush_len);
    if (!flush_body) {
        TEST_CHECK(false, "could not find ili9488_flush_cb()'s function body in lvgl_port.c -- "
                           "update this test if it was renamed/restructured.");
        free(text);
        return;
    }
    char *flush_fn = dup_range(flush_body, flush_len);
    TEST_CHECK(flush_fn != NULL, "malloc for the extracted flush function body succeeded");
    if (!flush_fn) {
        free(text);
        return;
    }
    TEST_CHECK(strstr(flush_fn, "lv_obj_invalidate(") == NULL,
               "ili9488_flush_cb() must NEVER call lv_obj_invalidate() (or any other lv_* "
               "mutator) -- it runs from inside LVGL's own active refresh, and reentering the "
               "invalid-area list from there is exactly what produced the reproducible "
               "task-watchdog crash this test pins. If a wake-redraw call belongs anywhere, "
               "it is lvgl_port_service_idle_wake(), not here.");
    free(flush_fn);

    size_t wake_len = 0;
    const char *wake_body =
        find_function_body(text, "static void lvgl_port_service_idle_wake(", &wake_len);
    if (!wake_body) {
        TEST_CHECK(false, "could not find lvgl_port_service_idle_wake()'s function body in "
                           "lvgl_port.c -- if this function is gone, the wake-edge redraw is "
                           "either missing entirely or moved back into ili9488_flush_cb().");
        free(text);
        return;
    }
    char *wake_fn = dup_range(wake_body, wake_len);
    TEST_CHECK(wake_fn != NULL, "malloc for the extracted wake function body succeeded");
    if (wake_fn) {
        TEST_CHECK(strstr(wake_fn, "lv_obj_invalidate(lv_screen_active())") != NULL,
                   "lvgl_port_service_idle_wake() must call lv_obj_invalidate(lv_screen_"
                   "active()) on the off->on edge -- otherwise the screen never redraws after "
                   "waking (the ORIGINAL bug this whole feature fixed, before it was moved to "
                   "the wrong call site).");
        free(wake_fn);
    }

    size_t task_len = 0;
    const char *task_body = find_function_body(text, "static void lvgl_port_task(", &task_len);
    if (!task_body) {
        TEST_CHECK(false, "could not find lvgl_port_task()'s function body in lvgl_port.c -- "
                           "update this test if it was renamed/restructured.");
        free(text);
        return;
    }
    char *task_fn = dup_range(task_body, task_len);
    TEST_CHECK(task_fn != NULL, "malloc for the extracted task function body succeeded");
    if (task_fn) {
        const char *wake_call = strstr(task_fn, "lvgl_port_service_idle_wake()");
        const char *timer_call = strstr(task_fn, "lv_timer_handler()");
        TEST_CHECK(wake_call != NULL,
                   "lvgl_port_task() must call lvgl_port_service_idle_wake() -- without it "
                   "nothing ever detects the wake edge and the screen stays blank/stale "
                   "forever after a touch wakes it.");
        TEST_CHECK(wake_call != NULL && timer_call != NULL && wake_call < timer_call,
                   "lvgl_port_service_idle_wake() must run BEFORE lv_timer_handler() in the "
                   "task loop -- calling it after (or from within) an active refresh reopens "
                   "the exact reentrancy this section exists to prevent.");
        free(task_fn);
    }

    free(text);
}

void run_test_display_power_wiring(void)
{
    run_section1_screen_idle_calls_policy();
    run_section2_touch_swallow_wired();
    run_section3_recovery_mode_gated();
    run_section3b_refresh_called_off_lock();
    run_section4_screen_idle_init_takes_recovery_mode();
    run_section5_screen_idle_stack_sized_for_dashboard();
    run_section6_wake_invalidate_not_in_flush_cb();
}
