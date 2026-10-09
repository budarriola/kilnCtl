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
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

// candidates[0] is always this test's file written relative to its OWN
// directory (e.g. "../drivers/ui/screen_idle.c") -- test_read_source_anchored()
// (test_common.h) uses it to resolve an absolute path anchored to __FILE__
// first, which works from ANY working directory the test binary is
// launched from, then falls back to the literal candidates[] entries
// (App/test, App, repo-root CWDs) as a second layer.
static char *read_file_any(const char *const *candidates, size_t count)
{
    return test_read_source_anchored(__FILE__, candidates[0], candidates, count);
}

// Extracts the body of a function from its exact declaration/signature
// substring (must include the opening paren, e.g. "static bool screen_idle_
// run_policy_locked(") through the first "\n}" after the opening brace --
// this codebase's own functions close at column 0, same assumption
// SaftyFW's test_safety_core_s8_wiring.c and test_safety_core_stack_
// budget.c make.
//
// Skips a bare PROTOTYPE match (the same signature substring followed by
// ");" rather than a "{") and keeps searching for a later occurrence that is
// actually a definition -- ui_page_network.c/ui_page_network_manage.c both
// forward-declare their refresh_cb() ahead of building the page, and the
// first version of sections 7/8's registration-site scans (2026-09-04)
// matched that PROTOTYPE line, walked forward to the next "{" it could find
// (the START of some LATER, unrelated function in the same file), and
// scanned that unrelated function's body instead of refresh_cb()'s real one
// -- a real deleted/hardware-relevant call injected into refresh_cb() for
// this file went completely unseen. Caught by hand while writing this
// file's own negative test (see the "if it does not fire" bar this file's
// top comment sets), not by any pre-existing check.
static const char *find_function_body(const char *text, const char *sig, size_t *out_len)
{
    const char *s = text;
    for (;;) {
        s = strstr(s, sig);
        if (!s) {
            return NULL;
        }
        // Walk from the end of `sig` (which stops right after the opening
        // paren) forward, tracking paren depth, to find where THIS
        // signature's own parameter list actually ends.
        const char *p = s + strlen(sig);
        int depth = 1; // sig already consumed the opening '('
        while (*p && depth > 0) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            p++;
        }
        if (depth != 0) {
            return NULL; // unbalanced parens -- malformed input, fail closed
        }
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        if (*p == ';') {
            // A prototype, not a definition -- keep looking.
            s = p + 1;
            continue;
        }
        if (*p != '{') {
            // Neither ';' nor '{' right after the parameter list -- not a
            // shape this scan understands (e.g. a K&R-style parameter
            // block, or something between the paren and the brace this
            // codebase does not actually use). Fail closed rather than
            // guessing.
            return NULL;
        }
        const char *open = p;
        const char *close = strstr(open, "\n}");
        if (!close) {
            return NULL;
        }
        *out_len = (size_t)(close - open);
        return open;
    }
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

/* Comment-stripped copy of `src`, for any check that must match real CODE
 * rather than prose. 2026-09-04, found by negative-testing this file's own
 * new lv_layer_top() check: the check passed with the fix DELETED, because
 * the fix's explanatory comment inside the same function body still
 * contained the literal "lv_layer_top()" the strstr was looking for. A
 * source-scan assertion that a well-written comment can satisfy is
 * vacuous -- and this repo has shipped that failure mode before
 * (feedback: "negative-test every check"). Everything between the comment
 * markers becomes a single space so adjacent tokens cannot be glued into
 * an accidental match. Handles /* ... *[/] and // ... newline; string
 * literals are not tracked, which is fine for the C sources this file
 * scans (none of them embed a comment opener inside a literal). */
static char *strip_c_comments(const char *src)
{
    size_t n = strlen(src);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        if (src[i] == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            out[o++] = ' ';
        } else if (src[i] == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') i++;
            out[o++] = ' ';
        } else {
            out[o++] = src[i++];
        }
    }
    out[o] = '\0';
    return out;
}

static const char *SCREEN_IDLE_C_CANDIDATES[] = {
    "../drivers/ui/screen_idle.c",
    "App/drivers/ui/screen_idle.c",
    "firmware/KilnFW/App/drivers/ui/screen_idle.c",
};
static const char *LVGL_PORT_C_CANDIDATES[] = {
    "../drivers/ui/lvgl_port.c",
    "App/drivers/ui/lvgl_port.c",
    "firmware/KilnFW/App/drivers/ui/lvgl_port.c",
};
static const char *KILN_UI_C_CANDIDATES[] = {
    "../drivers/ui/kiln_ui.c",
    "App/drivers/ui/kiln_ui.c",
    "firmware/KilnFW/App/drivers/ui/kiln_ui.c",
};
static const char *KILN_UI_H_CANDIDATES[] = {
    "../drivers/ui/kiln_ui.h",
    "App/drivers/ui/kiln_ui.h",
    "firmware/KilnFW/App/drivers/ui/kiln_ui.h",
};
static const char *UART_BRIDGE_UI_TEST_C_CANDIDATES[] = {
    "../drivers/bridge/uart_bridge_ui_test.c",
    "App/drivers/bridge/uart_bridge_ui_test.c",
    "firmware/KilnFW/App/drivers/bridge/uart_bridge_ui_test.c",
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

    TEST_CHECK(strstr(refresh_fn, "profile_executor_get_status(pst)") != NULL &&
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

    TEST_CHECK(strstr(refresh_fn, "pst->state == PROFILE_EXEC_FAULTED") != NULL,
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

    const char *pe_call = strstr(fn, "profile_executor_get_status(pst)");
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
        // 2026-09-04 opus review, second real defect in the same wake edge:
        // the blank is an ILI9488_clear() straight to panel GRAM, so it
        // erases EVERY pixel on the glass -- including LVGL's
        // screen-independent overlay layers. Every modal this UI puts up
        // (ui_confirm.c's lv_msgbox_create(NULL), ui_num_pad.c's keypad,
        // ui_page_network.c's connect modal) lives on lv_layer_top(), not
        // under the active screen -- kiln_ui.c's log_all_tap_targets()
        // documents exactly that and walks all three roots for the same
        // reason. Invalidating only lv_screen_active() repainted the page
        // and left an open dialog missing from the glass while LVGL still
        // had it focused and swallowing input.
        // Matched against a COMMENT-STRIPPED copy on purpose -- see
        // strip_c_comments()'s own comment: the first version of these two
        // checks passed with the fix deleted, because the fix's explanatory
        // comment inside this same function body still mentioned
        // lv_layer_top().
        char *wake_code = strip_c_comments(wake_fn);
        TEST_CHECK(wake_code != NULL, "malloc for the comment-stripped wake body succeeded");
        if (wake_code) {
            TEST_CHECK(strstr(wake_code, "lv_layer_top()") != NULL,
                       "lvgl_port_service_idle_wake() must also invalidate lv_layer_top() -- "
                       "the blank cleared the whole panel, and every modal in this UI "
                       "(ui_confirm.c, ui_num_pad.c, ui_page_network.c's connect modal) is "
                       "parented there, not under the active screen. Without it, waking with "
                       "a dialog open repaints the page WITHOUT the dialog, while LVGL still "
                       "has the invisible dialog focused and swallowing input.");
            TEST_CHECK(strstr(wake_code, "lv_layer_sys()") != NULL,
                       "lvgl_port_service_idle_wake() must also invalidate lv_layer_sys() -- "
                       "same screen-independent-root reasoning kiln_ui.c's "
                       "log_all_tap_targets() records for including it, so a future "
                       "toast/system overlay is covered by construction rather than by the "
                       "next bench crash.");
            free(wake_code);
        }
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

// ---------------------------------------------------------------------------
// Sections 7-9, 2026-09-04 opus review of the display path's execution
// contexts (UI_PLAN.md "Context rules for the display path"): four defects
// landed in one day because five different execution contexts all look like
// ordinary C and nothing at the call site says which one you are in. This
// review's own explicit conclusion is that documentation is not the
// mechanism -- every one of the four bugs had a CORRECT comment sitting
// beside the wrong code -- so these three scans make the context table's
// rules EXECUTABLE instead of merely written down. Each is generalised over
// a REGISTRATION SITE (who calls lv_display_set_flush_cb()/
// lv_indev_set_read_cb()/lv_timer_create(), who calls screen_idle_lock()) so
// a NEW callback or a NEW lock span is covered automatically, the same
// "discovery, not a list" principle tools/run_all_checks.ps1's own top
// comment states for guard scripts in general.
// ---------------------------------------------------------------------------

/* Finds the next occurrence of `call_prefix` (e.g. "lv_display_set_flush_cb(")
 * at or after `from`, and returns a heap-allocated copy of the LAST
 * comma-separated argument inside that call's parentheses, trimmed of
 * whitespace/semicolon -- i.e. the registered callback function's bare name
 * for a `lv_display_set_flush_cb(disp, FUNC)` / `lv_indev_set_read_cb(indev,
 * FUNC)` / `lv_timer_create(FUNC, period, user_data)` call site (FUNC is the
 * first arg for lv_timer_create, so callers needing that pass arg_index=0;
 * everything else here uses the LAST arg, arg_index=-1). Advances *cursor
 * past the match so a caller can loop over every occurrence in one file.
 * Returns NULL once no more occurrences exist. */
static char *find_next_registered_callback(const char *text, const char *call_prefix,
                                            int arg_index, const char **cursor)
{
    const char *p = strstr(*cursor, call_prefix);
    if (!p) return NULL;
    const char *args_start = p + strlen(call_prefix);
    const char *close = strchr(args_start, ')');
    if (!close) return NULL;
    *cursor = close + 1;

    // Split args on top-level commas (none of this codebase's registration
    // call sites nest a parenthesised expression as an argument, so a plain
    // comma split is sufficient -- no need for a full paren-depth parser).
    const char *arg_starts[4] = {args_start, NULL, NULL, NULL};
    int n_args = 1;
    for (const char *q = args_start; q < close && n_args < 4; q++) {
        if (*q == ',') {
            arg_starts[n_args] = q + 1;
            n_args++;
        }
    }
    int idx = (arg_index < 0) ? (n_args - 1) : arg_index;
    if (idx < 0 || idx >= n_args) return NULL;
    const char *seg_start = arg_starts[idx];
    const char *seg_end = (idx + 1 < n_args) ? strchr(seg_start, ',') : close;
    if (!seg_end || seg_end > close) seg_end = close;

    while (seg_start < seg_end && (*seg_start == ' ' || *seg_start == '\n' || *seg_start == '\t')) seg_start++;
    while (seg_end > seg_start &&
           (seg_end[-1] == ' ' || seg_end[-1] == '\n' || seg_end[-1] == '\t' || seg_end[-1] == ';')) seg_end--;
    if (seg_end <= seg_start) return NULL;

    // A bare function name must be a valid C identifier -- rejects an
    // argument like "&s_relay_ctx[r]" or "mbox" that happens to occupy the
    // slot this scan is not looking at (defensive; today's call sites never
    // hit this, but a future registration with extra args should fail
    // closed -- return NULL, which the caller treats as "could not resolve",
    // not silently scan the wrong text).
    for (const char *c = seg_start; c < seg_end; c++) {
        if (!(isalnum((unsigned char)*c) || *c == '_')) return NULL;
    }
    return dup_range(seg_start, (size_t)(seg_end - seg_start));
}

/* Finds FUNC's body (see find_function_body()) trying several signature
 * shapes, since a registered callback may be `static void`, `static bool`,
 * or plain `void` (declared in a header, defined in a different .c file --
 * e.g. ui_home_refresh_cb()). Returns NULL if none match, same fail-closed
 * contract as find_function_body(). */
static const char *find_function_body_any_sig(const char *text, const char *func_name, size_t *out_len)
{
    static const char *const PREFIXES[] = {"static void ", "static bool ", "void "};
    char sig[256];
    for (size_t i = 0; i < sizeof(PREFIXES) / sizeof(PREFIXES[0]); i++) {
        int n = snprintf(sig, sizeof(sig), "%s%s(", PREFIXES[i], func_name);
        if (n <= 0 || (size_t)n >= sizeof(sig)) continue;
        const char *body = find_function_body(text, sig, out_len);
        if (body) return body;
    }
    return NULL;
}

static const char *const LVGL_MUTATOR_DENYLIST[] = {
    "lv_obj_invalidate(", "lv_obj_del(",     "lv_obj_del_async(",
    "lv_screen_load(",    "lv_scr_load(",    "lv_refr_now(",
};

static void run_section7_flush_and_indev_cb_mutator_denylist(void)
{
    TEST_SECTION("every lv_display_set_flush_cb()/lv_indev_set_read_cb() registration in "
                 "lvgl_port.c -- generalised over the REGISTRATION SITE, not one hardcoded "
                 "function name -- has a comment-stripped body free of lv_* mutators. Context "
                 "rule: 'ili9488_flush_cb() ... may NOT do any lv_* mutator ... reads only' "
                 "(51e1ef5's lv_obj_invalidate()-from-inside-flush task-watchdog kill, and "
                 "7fc17cc's second instance of the exact same defect the same day). Unlike the "
                 "old section 6 check, which named ili9488_flush_cb() literally, a SECOND "
                 "display or a SECOND indev registered anywhere in this file is covered "
                 "automatically -- nobody has to remember to add a new hardcoded name here.");

    char *text = read_file_any(LVGL_PORT_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/lvgl_port.c");
        return;
    }
    char *stripped = strip_c_comments(text);
    TEST_CHECK(stripped != NULL, "malloc for the comment-stripped copy of lvgl_port.c succeeded");
    if (!stripped) {
        free(text);
        return;
    }

    static const char *const REG_PREFIXES[] = {
        "lv_display_set_flush_cb(",
        "lv_indev_set_read_cb(",
    };

    int total_checked = 0;
    for (size_t r = 0; r < sizeof(REG_PREFIXES) / sizeof(REG_PREFIXES[0]); r++) {
        const char *cursor = stripped;
        char *func_name;
        while ((func_name = find_next_registered_callback(stripped, REG_PREFIXES[r], -1, &cursor)) != NULL) {
            size_t body_len = 0;
            const char *body = find_function_body_any_sig(text, func_name, &body_len);
            if (!body) {
                TEST_CHECK(false, "found a registration of a callback via a source-text match "
                                   "but could not locate its function body -- either the scan's "
                                   "signature guesses are stale, or the registered symbol is not "
                                   "a plain named function this scan can extract; update "
                                   "find_function_body_any_sig()'s PREFIXES list.");
                free(func_name);
                continue;
            }
            char *fn = dup_range(body, body_len);
            if (fn) {
                char *fn_stripped = strip_c_comments(fn);
                if (fn_stripped) {
                    for (size_t d = 0; d < sizeof(LVGL_MUTATOR_DENYLIST) / sizeof(LVGL_MUTATOR_DENYLIST[0]); d++) {
                        bool clean = strstr(fn_stripped, LVGL_MUTATOR_DENYLIST[d]) == NULL;
                        char msg[320];
                        snprintf(msg, sizeof(msg),
                                 "%s (registered via %s) must not call the lv_* mutator '%s' -- "
                                 "this callback runs INSIDE lv_timer_handler()'s own active "
                                 "refresh/indev-read, and mutating the widget tree or invalid-"
                                 "area list from there is the exact reentrancy that killed the "
                                 "lvgl task with a task-watchdog reset (51e1ef5/7fc17cc). If a "
                                 "redraw belongs anywhere, it is lvgl_port_task()'s own loop "
                                 "BEFORE lv_timer_handler() runs, not here.",
                                 func_name, REG_PREFIXES[r], LVGL_MUTATOR_DENYLIST[d]);
                        TEST_CHECK(clean, msg);
                    }
                    free(fn_stripped);
                }
                free(fn);
            }
            total_checked++;
            free(func_name);
        }
    }
    TEST_CHECK(total_checked >= 2,
               "expected to find and check at least 2 registered callbacks (the flush cb and "
               "the indev read cb) -- found fewer, which means either the registration calls "
               "were removed/renamed (update REG_PREFIXES) or this scan's parser regressed.");

    free(stripped);
    free(text);
}

/* Page-refresh timer callbacks (lv_timer_create(), the third row of the
 * context table) must not block for long -- this codebase's own known
 * violation is calling dashboard_get_status() (five MAX31856 SPI reads, a
 * kiln_io_owner round trip that can block 200ms, interrupts-disabled heap
 * walks) directly from the lvgl task's 1-2 Hz page timers. Fixing that is
 * explicitly NOT this task's job (the right fix is a cross-page snapshot
 * cache other agents are mid-implementing) -- allowlisted here, by exact
 * function+file, with the TODO below, rather than silently excluded from
 * the scan. A NEW page's refresh_cb calling one of these is NOT allowlisted
 * and fails. */
static const char *const BLOCKING_TIMER_DENYLIST[] = {
    "dashboard_get_status(", "heap_caps_", "nvs_", "spi_device_", "i2c_master_", "kiln_io_owner_command_",
};

typedef struct {
    const char *func_name;
    /* Substring of the DEFINING file's path -- matched against the
     * candidate-array entry the body was actually found in, never the
     * bare function name alone. "refresh_cb" is reused as a static
     * function name in several of these page files (ui_page_network.c/
     * ui_page_network_manage.c both have their OWN clean "refresh_cb" that
     * must NOT be silently allowlisted just because the name matches one of
     * the two genuinely-allowlisted "refresh_cb"s below) -- keying on name
     * alone was this scan's own first-draft bug, caught while writing it,
     * before any hardware or CI ever saw it. */
    const char *defining_file_substr;
    const char *reason;
} allowlisted_blocking_cb_t;

/* TODO(display-power context rules, UI_PLAN.md 2026-09-04 opus review): all
 * three of these call dashboard_get_status() straight from an lv_timer
 * callback on the lvgl task -- the exact "page-refresh timers must not
 * block" violation this scan exists to catch. Reviewed and left in place
 * deliberately, not missed: fixing it means a shared snapshot cache other
 * agents are actively building pages against (see UI_PLAN.md's open items),
 * and duplicating that work here would conflict with theirs. Remove an
 * entry the day its file adopts the snapshot cache instead of calling
 * dashboard_get_status() directly -- if this scan still fails after that, it
 * caught a regression, not a stale allowlist. */
static const allowlisted_blocking_cb_t BLOCKING_TIMER_ALLOWLIST[] = {
    {"refresh_cb", "ui_page_diagnostics.c", "2s timer, dashboard_get_status() -- see TODO above"},
    {"refresh_cb", "ui_page_temperature.c", "1s timer, dashboard_get_status() -- see TODO above"},
    {"ui_home_refresh_cb", "ui_page_home_refresh.c",
     "1s timer, dashboard_get_status()+profile_executor_get_status() -- same class, found by "
     "THIS scan (not called out by name in the opus review's own text, which only named "
     "diagnostics/temperature) -- see TODO above"},
};

static bool is_allowlisted_blocking_cb(const char *func_name, const char *defining_file_path)
{
    for (size_t i = 0; i < sizeof(BLOCKING_TIMER_ALLOWLIST) / sizeof(BLOCKING_TIMER_ALLOWLIST[0]); i++) {
        if (strcmp(BLOCKING_TIMER_ALLOWLIST[i].func_name, func_name) == 0 &&
            strstr(defining_file_path, BLOCKING_TIMER_ALLOWLIST[i].defining_file_substr) != NULL) {
            return true;
        }
    }
    return false;
}

static void run_section8_timer_refresh_cb_blocking_denylist(void)
{
    TEST_SECTION("every lv_timer_create() page-refresh callback across the LCD page files -- "
                 "generalised over the REGISTRATION SITE within each listed file -- has a "
                 "comment-stripped body free of the blocking-call denylist (dashboard_get_"
                 "status()/heap_caps_*/nvs_*/SPI+I2C entry points), except the three "
                 "explicitly allowlisted below with a TODO. Context rule: 'lv_timer page-"
                 "refresh callbacks ... may NOT ... block for long'.");

    static const char *const PAGE_FILE_CANDIDATES[][3] = {
        {"../drivers/ui/ui_page_diagnostics.c", "App/drivers/ui/ui_page_diagnostics.c",
         "firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c"},
        {"../drivers/ui/ui_page_temperature.c", "App/drivers/ui/ui_page_temperature.c",
         "firmware/KilnFW/App/drivers/ui/ui_page_temperature.c"},
        {"../drivers/ui/ui_page_home.c", "App/drivers/ui/ui_page_home.c",
         "firmware/KilnFW/App/drivers/ui/ui_page_home.c"},
        {"../drivers/ui/ui_page_home_refresh.c", "App/drivers/ui/ui_page_home_refresh.c",
         "firmware/KilnFW/App/drivers/ui/ui_page_home_refresh.c"},
        {"../drivers/ui/ui_page_network.c", "App/drivers/ui/ui_page_network.c",
         "firmware/KilnFW/App/drivers/ui/ui_page_network.c"},
        {"../drivers/ui/ui_page_network_manage.c", "App/drivers/ui/ui_page_network_manage.c",
         "firmware/KilnFW/App/drivers/ui/ui_page_network_manage.c"},
    };
    const size_t n_files = sizeof(PAGE_FILE_CANDIDATES) / sizeof(PAGE_FILE_CANDIDATES[0]);

    // KNOWN LIMITATION, stated plainly rather than implied: this candidate
    // list is the current set of LCD page files known to register an
    // lv_timer_create() refresh callback. A NEW page file added later is
    // covered for what happens INSIDE it automatically -- but only once its
    // path is added to PAGE_FILE_CANDIDATES above. This scan cannot discover
    // a wholly new .c file on its own (no portable directory-glob available
    // to a host-test C file without pulling in platform-specific headers);
    // that is a real gap in "generalised automatically" versus scan #7's
    // full generality within lvgl_port.c, and is exactly why this comment
    // exists instead of a silent assumption.
    char *texts[6] = {0};
    char *stripped_texts[6] = {0};
    for (size_t i = 0; i < n_files; i++) {
        texts[i] = read_file_any(PAGE_FILE_CANDIDATES[i], 3);
        if (!texts[i]) {
            char msg[256];
            snprintf(msg, sizeof(msg), "could not locate %s from the host test's working "
                                        "directory -- update PAGE_FILE_CANDIDATES if it moved",
                     PAGE_FILE_CANDIDATES[i][2]);
            TEST_CHECK(false, msg);
            continue;
        }
        stripped_texts[i] = strip_c_comments(texts[i]);
    }

    int total_checked = 0;
    for (size_t i = 0; i < n_files; i++) {
        if (!stripped_texts[i]) continue;
        const char *cursor = stripped_texts[i];
        char *func_name;
        while ((func_name = find_next_registered_callback(stripped_texts[i], "lv_timer_create(", 0, &cursor)) !=
               NULL) {
            // Try the REGISTERING file (i) first -- "refresh_cb" is reused
            // as a distinct static function name in several of these files,
            // so searching in candidate-array order regardless of which
            // file is being scanned would resolve every "refresh_cb" lookup
            // to whichever file happens to come first in
            // PAGE_FILE_CANDIDATES (ui_page_diagnostics.c), silently
            // checking that file's own body over and over instead of the
            // one actually registered by ui_page_network.c/ui_page_network_
            // manage.c's own distinct refresh_cb() -- caught by hand
            // negative-testing this scan (see this file's top comment): an
            // injected dashboard_get_status() call in ui_page_network.c's
            // refresh_cb() went completely unseen until this ordering fix.
            // Only fall back to the other candidate files for the genuine
            // cross-file case (ui_home_refresh_cb(): registered in
            // ui_page_home.c, DEFINED in ui_page_home_refresh.c).
            size_t body_len = 0;
            const char *body = NULL;
            size_t defining_file = n_files;
            if (texts[i]) {
                body = find_function_body_any_sig(texts[i], func_name, &body_len);
                if (body) defining_file = i;
            }
            for (size_t j = 0; j < n_files && !body; j++) {
                if (!texts[j]) continue;
                body = find_function_body_any_sig(texts[j], func_name, &body_len);
                if (body) defining_file = j;
            }
            if (!body) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "registered lv_timer_create() callback '%s' has no locatable function "
                         "body across the candidate files -- update PAGE_FILE_CANDIDATES or "
                         "find_function_body_any_sig()'s signature guesses.",
                         func_name);
                TEST_CHECK(false, msg);
                free(func_name);
                continue;
            }
            total_checked++;
            // Keyed on (func_name, DEFINING file) -- not func_name alone. See
            // allowlisted_blocking_cb_t's own field comment for why: several
            // of these files reuse "refresh_cb" as their own clean, unrelated
            // static function.
            if (is_allowlisted_blocking_cb(func_name, PAGE_FILE_CANDIDATES[defining_file][2])) {
                free(func_name);
                continue;
            }
            char *fn = dup_range(body, body_len);
            if (fn) {
                char *fn_stripped = strip_c_comments(fn);
                if (fn_stripped) {
                    for (size_t d = 0; d < sizeof(BLOCKING_TIMER_DENYLIST) / sizeof(BLOCKING_TIMER_DENYLIST[0]);
                         d++) {
                        bool clean = strstr(fn_stripped, BLOCKING_TIMER_DENYLIST[d]) == NULL;
                        char msg[384];
                        snprintf(msg, sizeof(msg),
                                 "lv_timer callback '%s' calls the blocking denylist entry '%s' "
                                 "and is not allowlisted -- page-refresh timers run on the lvgl "
                                 "task and must not block it for the multi-hundred-ms this class "
                                 "of call can take (ui_page_diagnostics.c/ui_page_temperature.c "
                                 "already do this and are deliberately allowlisted with a TODO; "
                                 "if this is a NEW page hitting the same pattern, either avoid "
                                 "the blocking call or add it to BLOCKING_TIMER_ALLOWLIST with "
                                 "the same TODO reasoning -- do not silently exclude it).",
                                 func_name, BLOCKING_TIMER_DENYLIST[d]);
                        TEST_CHECK(clean, msg);
                    }
                    free(fn_stripped);
                }
                free(fn);
            }
            free(func_name);
        }
    }
    TEST_CHECK(total_checked >= 3,
               "expected to find and check at least 3 registered lv_timer_create() page-"
               "refresh callbacks across the candidate files -- found fewer, which means either "
               "a registration was removed/renamed or this scan's parser regressed.");

    for (size_t i = 0; i < n_files; i++) {
        free(stripped_texts[i]);
        free(texts[i]);
    }
}

/* screen_idle.c's own load-bearing invariant, stated once in UI_PLAN.md:
 * "idle->lock may only ever be held across pure computation and plain
 * struct field access -- never across SPI, I2C, a queue wait, a heap walk,
 * an NVS access, or any lv_* call." 7a8594d was exactly this: a lock held
 * across five SPI reads, a 200ms queue wait, and interrupts-disabled heap
 * walks. Generalised over every screen_idle_lock()/screen_idle_unlock()
 * SPAN in the file (not one hardcoded function) by pairing each lock with
 * the LAST unlock encountered before the next lock -- a deliberate superset
 * when a scope has more than one exit path (screen_idle_touch_swallow()'s
 * early-return-on-release branch unlocks once, then the pressed path
 * continues under the SAME still-held lock to a second, final unlock). That
 * superset can only ever flag a false positive (a few bytes of genuinely
 * unlocked glue code between two unlocks, if it happened to call something
 * denylisted -- it does not, today), never a false negative that misses
 * real locked code -- the safe direction for a scan whose job is "prove
 * nothing bad happens under this lock". */
static void run_section9_idle_lock_scope_denylist(void)
{
    TEST_SECTION("every screen_idle_lock()->screen_idle_unlock() span in screen_idle.c -- "
                 "generalised over every lock/unlock PAIR in the file, not one hardcoded "
                 "function -- calls nothing from the denylist (*_get_status, heap_caps_*, "
                 "*_command_*, nvs_*, lv_*, SPI/I2C entry points). Pins UI_PLAN.md's stated "
                 "invariant: 'idle->lock may only ever be held across pure computation and "
                 "plain struct field access'. Regression test for 7a8594d.");

    char *text = read_file_any(SCREEN_IDLE_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/screen_idle.c");
        return;
    }
    char *stripped = strip_c_comments(text);
    TEST_CHECK(stripped != NULL, "malloc for the comment-stripped copy of screen_idle.c succeeded");
    if (!stripped) {
        free(text);
        return;
    }

    static const char *const LOCK_TOK = "screen_idle_lock(";
    static const char *const UNLOCK_TOK = "screen_idle_unlock(";

    static const char *const LOCK_SCOPE_DENYLIST[] = {
        "_get_status(", "heap_caps_", "_command_", "nvs_", "lv_", "spi_device_", "i2c_master_",
    };

    int span_count = 0;
    const char *lock_pos = strstr(stripped, LOCK_TOK);
    while (lock_pos) {
        const char *next_lock = strstr(lock_pos + 1, LOCK_TOK);
        // The last unlock() strictly between this lock() and the NEXT
        // lock() (or end of text, for the final span) is this span's true
        // end -- see this function's top comment for why "last", not
        // "first", is the correct (conservative) pairing.
        const char *scan_limit = next_lock ? next_lock : (stripped + strlen(stripped));
        const char *unlock_pos = NULL;
        for (const char *u = strstr(lock_pos, UNLOCK_TOK); u && u < scan_limit;
             u = strstr(u + 1, UNLOCK_TOK)) {
            unlock_pos = u;
        }
        if (!unlock_pos) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "found screen_idle_lock() at offset %ld with no matching screen_idle_"
                     "unlock() before the next lock (or end of file) -- either a lock is never "
                     "released (a real bug this scan should not paper over) or this scan's "
                     "pairing heuristic needs revisiting for a genuinely new lock usage shape.",
                     (long)(lock_pos - stripped));
            TEST_CHECK(false, msg);
            lock_pos = next_lock;
            continue;
        }

        size_t span_len = (size_t)(unlock_pos - lock_pos);
        char *span = dup_range(lock_pos, span_len);
        if (span) {
            for (size_t d = 0; d < sizeof(LOCK_SCOPE_DENYLIST) / sizeof(LOCK_SCOPE_DENYLIST[0]); d++) {
                bool clean = strstr(span, LOCK_SCOPE_DENYLIST[d]) == NULL;
                char msg[320];
                snprintf(msg, sizeof(msg),
                         "the screen_idle_lock()..screen_idle_unlock() span starting at offset "
                         "%ld calls the denylisted pattern '%s' -- idle->lock may only ever be "
                         "held across pure computation and plain field access; SPI/I2C/NVS/a "
                         "heap walk/an lv_* call under this lock is exactly 7a8594d's defect "
                         "(a lock held across five SPI reads, a 200ms queue wait, and "
                         "interrupts-disabled heap walks) reopening.",
                         (long)(lock_pos - stripped), LOCK_SCOPE_DENYLIST[d]);
                TEST_CHECK(clean, msg);
            }
            free(span);
        }
        span_count++;
        lock_pos = next_lock;
    }
    TEST_CHECK(span_count >= 4,
               "expected to find and check at least 4 screen_idle_lock()/screen_idle_unlock() "
               "spans in screen_idle.c (screen_idle_refresh_inputs(), screen_idle_task(), "
               "screen_idle_touch_swallow(), screen_idle_get_state()) -- found fewer, which "
               "means either lock usage was removed/restructured or this scan's pairing "
               "regressed.");

    free(stripped);
    free(text);
}

// 2026-09-24: makes a swallowed tap observable (KILN_UI_CLICK_SWALLOWED /
// TOUCH_CMD_GET_STATE's power_state+swallow_count+last_swallow_reason
// fields) -- proves screen_idle_run_policy_locked() actually counts a
// swallow and attributes its reason from the PRE-overwrite policy_state
// (the ordering this feature's correctness depends on: idle->policy_state
// must be read for the reason BEFORE it is set to out.state a few lines
// later, since rule 4/wake only ever fires from DISPLAY_POWER_OFF and rule
// 5/error-hold-dismiss only ever fires from DISPLAY_POWER_ERROR_HOLD -- see
// screen_idle.c's own comment at this call site).
static void run_section10_swallow_diag_wired(void)
{
    TEST_SECTION("screen_idle_run_policy_locked() counts a swallow and "
                 "attributes its reason before overwriting policy_state -- "
                 "source-text scan (screen_idle.c is not host-compilable)");

    char *text = read_file_any(SCREEN_IDLE_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/screen_idle.c from the host test's "
                           "working directory");
        return;
    }

    size_t body_len = 0;
    const char *body =
        find_function_body(text, "static bool screen_idle_run_policy_locked(", &body_len);
    if (!body) {
        TEST_CHECK(false, "could not find screen_idle_run_policy_locked()'s function body -- "
                           "update this test if it was renamed/restructured");
        free(text);
        return;
    }
    char *fn = dup_range(body, body_len);
    TEST_CHECK(fn != NULL, "malloc for the extracted function body succeeded");
    if (!fn) {
        free(text);
        return;
    }

    TEST_CHECK(strstr(fn, "out.swallow_touch") != NULL &&
                   strstr(fn, "idle->swallow_count++") != NULL,
               "screen_idle_run_policy_locked() must increment idle->swallow_count when "
               "out.swallow_touch is set -- if this fails, a swallowed touch is no longer "
               "counted at all, defeating TOUCH_CMD_GET_STATE's swallow_count diagnostic.");

    TEST_CHECK(strstr(fn, "idle->last_swallow_reason") != NULL &&
                   strstr(fn, "SCREEN_IDLE_SWALLOW_WAKE") != NULL &&
                   strstr(fn, "SCREEN_IDLE_SWALLOW_ERROR_HOLD") != NULL,
               "screen_idle_run_policy_locked() must attribute last_swallow_reason to either "
               "SCREEN_IDLE_SWALLOW_WAKE or SCREEN_IDLE_SWALLOW_ERROR_HOLD -- if this fails "
               "the reason is no longer derived at all, or was collapsed to a single value.");

    // Ordering check: the swallow-reason attribution block must appear
    // BEFORE idle->policy_state is overwritten with out.state -- otherwise
    // the reason would always read as whatever state the board is ABOUT TO
    // enter, not the one the swallowed tap actually happened in (DISPLAY_
    // POWER_OFF for a wake, DISPLAY_POWER_ERROR_HOLD for a dismiss).
    const char *swallow_pos = strstr(fn, "idle->swallow_count++");
    const char *state_write_pos = strstr(fn, "idle->policy_state = out.state");
    TEST_CHECK(swallow_pos != NULL && state_write_pos != NULL && swallow_pos < state_write_pos,
               "idle->swallow_count++/last_swallow_reason attribution must run BEFORE "
               "idle->policy_state is overwritten with out.state -- if this fails the ordering "
               "was changed and the reason attribution is now reading the WRONG (post-"
               "transition) policy_state, silently mislabeling every wake as an error-hold "
               "dismiss or vice versa.");

    free(fn);
    free(text);
}

// 2026-09-24: proves lvgl_port.c's seq/verdict handoff is actually wired --
// lvgl_port_inject_touch() assigns a seq on press and touch_read_cb()
// records a verdict against it under the SAME lock the struct already uses
// (never a new lock held across an lv_* call).
static void run_section11_inject_verdict_handoff_wired(void)
{
    TEST_SECTION("lvgl_port.c's inject seq/verdict handoff is wired: press "
                 "gets a seq, touch_read_cb() records the verdict under "
                 "touch_inject_lock() -- source-text scan (lvgl_port.c is not "
                 "host-compilable)");

    char *text = read_file_any(LVGL_PORT_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/ui/lvgl_port.c from the host test's "
                           "working directory");
        return;
    }

    size_t inject_len = 0;
    const char *inject_body =
        find_function_body(text, "uint32_t lvgl_port_inject_touch(", &inject_len);
    if (!inject_body) {
        TEST_CHECK(false, "could not find lvgl_port_inject_touch()'s function body, or it no "
                           "longer returns uint32_t -- kiln_ui_click_by_name() depends on this "
                           "return value to learn the swallow verdict for its own press.");
        free(text);
        return;
    }
    char *inject_fn = dup_range(inject_body, inject_len);
    TEST_CHECK(inject_fn != NULL, "malloc for the extracted function body succeeded");
    if (inject_fn) {
        TEST_CHECK(strstr(inject_fn, "s_inject_seq_counter") != NULL,
                   "lvgl_port_inject_touch() must assign/advance a sequence number on press -- "
                   "if this fails, the caller has no way to identify which verdict later "
                   "belongs to its own press.");
        free(inject_fn);
    }

    size_t cb_len = 0;
    const char *cb_body = find_function_body(text, "static void touch_read_cb(", &cb_len);
    if (!cb_body) {
        TEST_CHECK(false, "could not find touch_read_cb()'s function body -- update this test "
                           "if it was renamed.");
        free(text);
        return;
    }
    char *cb_fn = dup_range(cb_body, cb_len);
    TEST_CHECK(cb_fn != NULL, "malloc for the extracted function body succeeded");
    if (cb_fn) {
        TEST_CHECK(strstr(cb_fn, "s_inject_verdict_seq") != NULL &&
                       strstr(cb_fn, "s_inject_verdict_swallowed") != NULL,
                   "touch_read_cb() must record the swallow verdict (s_inject_verdict_seq/"
                   "s_inject_verdict_swallowed) for the press it just processed -- if this "
                   "fails, lvgl_port_get_inject_verdict() can never find a match and every "
                   "click_by_name() call falls back to the conservative 'not swallowed' "
                   "timeout, silently defeating KILN_UI_CLICK_SWALLOWED.");
        free(cb_fn);
    }

    free(text);
}

// 2026-09-24 KILN_UI_CLICK_INJECT_FAILED: kiln_ui_click_by_name() skips both
// the verdict wait and the release when lvgl_port_inject_touch() returns 0.
// Skipping the release is only safe if 0 really means "nothing was latched",
// i.e. every `return 0` in lvgl_port_inject_touch() precedes its first write
// to s_inject. This section pins that invariant, the early-return ordering in
// kiln_ui_click_by_name(), and that the UART bridge maps every
// kiln_ui_click_result_t member explicitly (a missed case falls into the
// bridge's default arm and goes on the wire as NOT_FOUND).
static void run_section12_inject_failed_wired(void)
{
    TEST_SECTION("KILN_UI_CLICK_INJECT_FAILED: 0 from lvgl_port_inject_touch() "
                 "latches nothing, click_by_name() returns before the wait and "
                 "release, and the bridge maps every click result -- "
                 "source-text scan (none of these files is host-compilable)");

    // --- lvgl_port_inject_touch(): every `return 0` precedes the latch. ---
    char *port_text = read_file_any(LVGL_PORT_C_CANDIDATES, 3);
    char *port_stripped = port_text ? strip_c_comments(port_text) : NULL;
    if (!port_stripped) {
        TEST_CHECK(false, "could not locate/strip drivers/ui/lvgl_port.c");
    } else {
        size_t len = 0;
        const char *body = find_function_body(port_stripped, "uint32_t lvgl_port_inject_touch(", &len);
        char *fn = body ? dup_range(body, len) : NULL;
        TEST_CHECK(fn != NULL, "found lvgl_port_inject_touch()'s function body");
        if (fn) {
            const char *latch = strstr(fn, "s_inject.pending = true");
            const char *seq_skip_zero = strstr(fn, "s_inject_seq_counter = 1");
            TEST_CHECK(latch != NULL,
                       "lvgl_port_inject_touch() must latch the press via "
                       "`s_inject.pending = true` -- update this scan if it was restructured.");
            TEST_CHECK(seq_skip_zero != NULL,
                       "lvgl_port_inject_touch() must skip the reserved seq 0 on wraparound "
                       "(`s_inject_seq_counter = 1`) -- otherwise a latched press could return "
                       "0 and kiln_ui_click_by_name() would skip its release, leaving a stuck "
                       "press until the 5 s auto-release.");
            int zero_returns = 0;
            bool zero_after_latch = false;
            for (const char *r = strstr(fn, "return 0;"); r; r = strstr(r + 1, "return 0;")) {
                zero_returns++;
                if (latch && r > latch) {
                    zero_after_latch = true;
                }
            }
            TEST_CHECK(zero_returns >= 1 && !zero_after_latch,
                       "every `return 0;` in lvgl_port_inject_touch() must precede "
                       "`s_inject.pending = true` -- a 0 return after the latch would make "
                       "kiln_ui_click_by_name()'s INJECT_FAILED path skip the release of a "
                       "press that WAS latched.");
            free(fn);
        }
    }
    free(port_stripped);
    free(port_text);

    // --- kiln_ui_click_by_name(): INJECT_FAILED returns before wait/release. ---
    char *ui_text = read_file_any(KILN_UI_C_CANDIDATES, 3);
    char *ui_stripped = ui_text ? strip_c_comments(ui_text) : NULL;
    if (!ui_stripped) {
        TEST_CHECK(false, "could not locate/strip drivers/ui/kiln_ui.c");
    } else {
        size_t len = 0;
        const char *body = find_function_body(ui_stripped, "kiln_ui_click_result_t kiln_ui_click_by_name(", &len);
        char *fn = body ? dup_range(body, len) : NULL;
        TEST_CHECK(fn != NULL, "found kiln_ui_click_by_name()'s function body");
        if (fn) {
            // "== 0)" with the closing paren, so `press_seq == 0xFF...` or
            // `press_seq == 0 && ...` cannot satisfy it by prefix.
            const char *guard = strstr(fn, "if (press_seq == 0)");
            const char *ret = guard ? strstr(guard, "return KILN_UI_CLICK_INJECT_FAILED;") : NULL;
            const char *wait = strstr(fn, "lvgl_port_get_inject_verdict(");
            const char *release = strstr(fn, "lvgl_port_inject_touch(cx, cy, false)");
            TEST_CHECK(guard != NULL && ret != NULL && wait != NULL && release != NULL &&
                           ret < wait && ret < release,
                       "kiln_ui_click_by_name() must return KILN_UI_CLICK_INJECT_FAILED on "
                       "`press_seq == 0` BEFORE the lvgl_port_get_inject_verdict() wait and "
                       "the release -- otherwise a never-queued press is reported as "
                       "VERDICT_UNKNOWN after a 250 ms wait.");
            free(fn);
        }
    }
    free(ui_stripped);
    free(ui_text);

    // --- kiln_ui_click_by_name(): WALK_BUSY returned before the match/
    // injection logic, on a dispatch-timeout read (n==0 && truncated). ---
    char *ui_text_wb = read_file_any(KILN_UI_C_CANDIDATES, 3);
    char *ui_stripped_wb = ui_text_wb ? strip_c_comments(ui_text_wb) : NULL;
    if (!ui_stripped_wb) {
        TEST_CHECK(false, "could not locate/strip drivers/ui/kiln_ui.c");
    } else {
        size_t len = 0;
        const char *body = find_function_body(ui_stripped_wb, "kiln_ui_click_result_t kiln_ui_click_by_name(", &len);
        char *fn = body ? dup_range(body, len) : NULL;
        TEST_CHECK(fn != NULL, "found kiln_ui_click_by_name()'s function body");
        if (fn) {
            const char *guard = strstr(fn, "if (n == 0 && truncated)");
            const char *ret = guard ? strstr(guard, "return KILN_UI_CLICK_WALK_BUSY;") : NULL;
            const char *match_scan = strstr(fn, "int match = -1;");
            TEST_CHECK(guard != NULL && ret != NULL && match_scan != NULL && ret < match_scan,
                       "kiln_ui_click_by_name() must return KILN_UI_CLICK_WALK_BUSY on "
                       "`n == 0 && truncated` (a dispatch timeout from "
                       "lvgl_port_collect_tap_targets(), e.g. lvgl_port_task busy/wedged past "
                       "UI_WALK_WAIT_TIMEOUT_MS) BEFORE the match-scanning loop -- otherwise a "
                       "walk that never completed is indistinguishable from a genuinely absent "
                       "name (2026-09-30 LCD-09/LCD-16 false-FAIL root cause).");
            free(fn);
        }
    }
    free(ui_stripped_wb);
    free(ui_text_wb);

    // --- log_all_tap_targets(): the hidden-target pass runs only for a click
    // collect (s_collect_hidden && an out buffer), never a log dump (fec20423). ---
    char *ui_text_hp = read_file_any(KILN_UI_C_CANDIDATES, 3);
    char *ui_stripped_hp = ui_text_hp ? strip_c_comments(ui_text_hp) : NULL;
    if (!ui_stripped_hp) {
        TEST_CHECK(false, "could not locate/strip drivers/ui/kiln_ui.c");
    } else {
        size_t len = 0;
        const char *body = find_function_body(ui_stripped_hp, "static void log_all_tap_targets(", &len);
        char *fn = body ? dup_range(body, len) : NULL;
        TEST_CHECK(fn != NULL, "found log_all_tap_targets()'s function body");
        if (fn) {
            const char *guard = strstr(fn, "if (!s_collect_hidden || !ctx->out || ctx->do_log)");
            const char *ret = guard ? strstr(guard, "return;") : NULL;
            const char *arm = strstr(fn, "ctx->hidden_pass = true;");
            TEST_CHECK(guard != NULL && ret != NULL && arm != NULL && ret < arm,
                       "log_all_tap_targets() must return early on `!s_collect_hidden || "
                       "!ctx->out || ctx->do_log` BEFORE arming `ctx->hidden_pass = true` -- "
                       "otherwise a log dump (do_log) runs the hidden pass and floods the log "
                       "with hidden=true targets (fec20423).");
            // The only place the hidden pass is armed is behind that guard.
            // Count EVERY arming spelling: `hidden_pass` + optional spaces + `=` +
            // optional spaces + `true`/`1` (member store, designated initialiser,
            // `ctx.hidden_pass=true`, `= 1`, ...).
            int arm_count = 0;
            const char *arm_at = NULL;
            for (const char *s = strstr(ui_stripped_hp, "hidden_pass"); s;
                 s = strstr(s + 1, "hidden_pass")) {
                const char *q = s + strlen("hidden_pass");
                while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
                if (*q != '=' || q[1] == '=') continue;
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
                size_t vl = strncmp(q, "true", 4) == 0 ? 4 : (*q == '1' ? 1 : 0);
                if (vl == 0) continue;
                char c = q[vl];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') continue;
                arm_count++;
                if (!arm_at) arm_at = s;
            }
            TEST_CHECK(arm_count == 1 && arm_at >= body && arm_at < body + len,
                       "`hidden_pass = true` must be armed exactly once in kiln_ui.c, inside "
                       "log_all_tap_targets() behind the guard -- a second arming site would "
                       "bypass it.");
            free(fn);
        }
    }
    free(ui_stripped_hp);
    free(ui_text_hp);

    // --- uart_bridge_ui_test.c: an explicit case for every enum member. ---
    char *h_text = read_file_any(KILN_UI_H_CANDIDATES, 3);
    char *h_stripped = h_text ? strip_c_comments(h_text) : NULL;
    char *br_text = read_file_any(UART_BRIDGE_UI_TEST_C_CANDIDATES, 3);
    char *br_stripped = br_text ? strip_c_comments(br_text) : NULL;
    if (!h_stripped || !br_stripped) {
        TEST_CHECK(false, "could not locate/strip drivers/ui/kiln_ui.h or "
                           "drivers/bridge/uart_bridge_ui_test.c");
    } else {
        const char *end = strstr(h_stripped, "} kiln_ui_click_result_t;");
        const char *start = NULL;
        for (const char *s = strstr(h_stripped, "typedef enum {"); s && (!end || s < end);
             s = strstr(s + 1, "typedef enum {")) {
            start = s;
        }
        TEST_CHECK(start != NULL && end != NULL, "found kiln_ui_click_result_t's enum body");
        int members = 0;
        if (start && end) {
            for (const char *m = strstr(start, "KILN_UI_CLICK_"); m && m < end;
                 m = strstr(m + 1, "KILN_UI_CLICK_")) {
                char ident[64];
                size_t n = 0;
                while (n + 1 < sizeof(ident) && (isalnum((unsigned char)m[n]) || m[n] == '_')) {
                    ident[n] = m[n];
                    n++;
                }
                ident[n] = '\0';
                members++;
                char needle[80];
                snprintf(needle, sizeof(needle), "case %s:", ident);
                if (!strstr(br_stripped, needle)) {
                    char msg[200];
                    snprintf(msg, sizeof(msg),
                             "uart_bridge_ui_test.c has no explicit `%s` -- it falls into the "
                             "default arm and goes on the wire as UI_TEST_CLICK_NOT_FOUND", needle);
                    TEST_CHECK(false, msg);
                }
            }
        }
        TEST_CHECK(members >= 7,
                   "expected at least 7 kiln_ui_click_result_t members (OK..INJECT_FAILED) -- "
                   "the enum scan found fewer, so it is not reading the real enum body.");
        TEST_CHECK(strstr(br_stripped, "case KILN_UI_CLICK_INJECT_FAILED: wire_result = "
                                       "UI_TEST_CLICK_INJECT_FAILED;") != NULL,
                   "uart_bridge_ui_test.c must map KILN_UI_CLICK_INJECT_FAILED to "
                   "UI_TEST_CLICK_INJECT_FAILED.");
        TEST_CHECK(strstr(br_stripped, "case KILN_UI_CLICK_WALK_BUSY: wire_result = "
                                       "UI_TEST_CLICK_WALK_BUSY;") != NULL,
                   "uart_bridge_ui_test.c must map KILN_UI_CLICK_WALK_BUSY to "
                   "UI_TEST_CLICK_WALK_BUSY.");
    }
    free(h_stripped);
    free(h_text);
    free(br_stripped);
    free(br_text);
}

// 2026-09-24: lvgl_port_collect_tap_targets()'s cross-task handoff. The
// result buffer (s_ui_walk_targets) is shared between lvgl_port_task (the
// only task allowed to walk the LVGL tree) and the requester; a torn or stale
// copy-out hands kiln_ui_click_by_name() a name/coordinate pair that does not
// belong together, and it then injects a REAL touch at those coordinates.
// None of this is host-runnable (lvgl_port.c needs LVGL and real FreeRTOS
// threads), so this pins the four invariants that make it safe:
//   1. the requester accepts a completion only when served_seq matches its
//      own request's sequence (a late completion of an abandoned request is
//      not taken as a later caller's answer), and copies out only inside that
//      check and under s_ui_walk.data;
//   2. the sequence is bumped under s_ui_walk.data;
//   3. lvgl_port_task walks into s_ui_walk_targets and records served_seq
//      only under s_ui_walk.data, taken with a ZERO timeout (it never blocks
//      on a requester), and never takes s_ui_walk.data any other way;
//   4. lvgl_port_task gives `done` only after releasing `data`.
// 2026-09-25 KILN_UI_CLICK_OFFSCREEN: a tap target whose reported centre lies
// off the panel used to be reported KILN_UI_CLICK_OK (LVGL never clamps an
// injected point, so the hit test just silently misses) -- a false pass, not
// a caught defect (logs/bench_test/20260925T150107Z_lcd). This pins that
// kiln_ui_click_by_name() checks the match's coordinates against the
// display's resolution BEFORE the hidden/visible split (so an off-screen
// widget is reported as off-screen even if it also happens to be hidden),
// and that the bridge maps it explicitly.
static void run_section14_offscreen_checked_before_hidden(void)
{
    TEST_SECTION("KILN_UI_CLICK_OFFSCREEN: checked ahead of the hidden/visible "
                 "split in click_by_name(), and the bridge maps it -- "
                 "source-text scan (none of these files is host-compilable)");

    char *ui_text = read_file_any(KILN_UI_C_CANDIDATES, 3);
    char *ui_stripped = ui_text ? strip_c_comments(ui_text) : NULL;
    if (!ui_stripped) {
        TEST_CHECK(false, "could not locate/strip drivers/ui/kiln_ui.c");
    } else {
        size_t len = 0;
        const char *body = find_function_body(ui_stripped, "kiln_ui_click_result_t kiln_ui_click_by_name(", &len);
        char *fn = body ? dup_range(body, len) : NULL;
        TEST_CHECK(fn != NULL, "found kiln_ui_click_by_name()'s function body");
        if (fn) {
            const char *offscreen_ret = strstr(fn, "return KILN_UI_CLICK_OFFSCREEN;");
            const char *hidden_ret = strstr(fn, "return KILN_UI_CLICK_HIDDEN;");
            const char *ambiguous_ret = strstr(fn, "return KILN_UI_CLICK_AMBIGUOUS;");
            /* 2026-09-25 review follow-up: kiln_ui_click_by_name() runs on the
             * UART bridge task, not lvgl_port_task -- the one task LVGL may be
             * called from -- so it must not call an lv_display_get_*_
             * resolution() itself. It now gets disp_w/disp_h back from
             * lvgl_port_collect_tap_targets()'s out params, which reads them
             * on lvgl_port_task during the same walk (see that function's doc
             * comment in lvgl_port.h). */
            const char *collect = strstr(fn, "lvgl_port_collect_tap_targets(");
            const char *hres = strstr(fn, "lv_display_get_horizontal_resolution(");
            const char *vres = strstr(fn, "lv_display_get_vertical_resolution(");
            const char *disp_w_use = strstr(fn, "targets[match].cx >= disp_w");
            const char *disp_h_use = strstr(fn, "targets[match].cy >= disp_h");
            TEST_CHECK(offscreen_ret != NULL && hidden_ret != NULL && ambiguous_ret != NULL &&
                           offscreen_ret < hidden_ret && offscreen_ret < ambiguous_ret,
                       "kiln_ui_click_by_name() must return KILN_UI_CLICK_OFFSCREEN BEFORE "
                       "the hidden/visible split -- otherwise an off-screen-and-hidden target "
                       "reports HIDDEN instead of OFFSCREEN, or an off-screen widget slips "
                       "through to the AMBIGUOUS/OK path.");
            TEST_CHECK(hres == NULL && vres == NULL,
                       "kiln_ui_click_by_name() must NOT call lv_display_get_horizontal/"
                       "vertical_resolution() itself -- it runs on the UART bridge task, and "
                       "LVGL may only be called from lvgl_port_task; get the resolution back "
                       "from lvgl_port_collect_tap_targets()'s out params instead.");
            TEST_CHECK(collect != NULL && disp_w_use != NULL && disp_h_use != NULL &&
                           collect < disp_w_use && collect < disp_h_use,
                       "kiln_ui_click_by_name() must bounds-check the match's centre against "
                       "the disp_w/disp_h it got back from lvgl_port_collect_tap_targets(), "
                       "not a hardcoded constant or a direct lv_display_get_*_resolution() "
                       "call.");
            free(fn);
        }
    }
    free(ui_stripped);
    free(ui_text);

    // --- uart_bridge_ui_test.c: an explicit case for OFFSCREEN too. ---
    char *br_text = read_file_any(UART_BRIDGE_UI_TEST_C_CANDIDATES, 3);
    char *br_stripped = br_text ? strip_c_comments(br_text) : NULL;
    if (!br_stripped) {
        TEST_CHECK(false, "could not locate/strip drivers/bridge/uart_bridge_ui_test.c");
    } else {
        TEST_CHECK(strstr(br_stripped, "case KILN_UI_CLICK_OFFSCREEN: wire_result = "
                                       "UI_TEST_CLICK_OFFSCREEN;") != NULL,
                   "uart_bridge_ui_test.c must map KILN_UI_CLICK_OFFSCREEN to "
                   "UI_TEST_CLICK_OFFSCREEN.");
    }
    free(br_stripped);
    free(br_text);
}

// Local mirror of kiln_ui_click_by_name()'s offscreen bounds check (2026-09-25
// KILN_UI_CLICK_OFFSCREEN, see run_section14_offscreen_checked_before_hidden()
// above). kiln_ui.c itself is not host-compilable (needs real LVGL/FreeRTOS),
// so this reimplements the same four comparisons -- cx < 0, cx >= disp_w,
// cy < 0, cy >= disp_h -- as a pure function this file CAN exercise with a
// point/resolution matrix. That would silently drift from the real source if
// left on its own, so run_section15_point_on_panel_boundary_matrix() below
// pins the exact token spelling of all four comparisons in kiln_ui.c's
// function body first (same source-text-scan technique the rest of this file
// uses) -- if kiln_ui.c's operators ever change, that scan fails and this
// mirror's numeric coverage is never silently testing a stale copy of the
// real logic. Left in kiln_ui.h's coordinate space: cx/cy are int16_t there
// (kiln_ui_tap_target_t), disp_w/disp_h are int32_t (lv_display_get_*
// resolution's own return type) -- kept as int32_t here for headroom on the
// disp_w/disp_h bound itself, matching the real signature.
static bool point_on_panel(int32_t cx, int32_t cy, int32_t disp_w, int32_t disp_h)
{
    if (cx < 0 || cx >= disp_w || cy < 0 || cy >= disp_h) {
        return false; /* KILN_UI_CLICK_OFFSCREEN in the real function */
    }
    return true;
}

// 2026-09-25: point_on_panel()/kiln_ui_click_by_name()'s offscreen check,
// left untested by run_section14 above (which pins ordering and call shape
// but never the boundary arithmetic itself -- an off-by-one here, e.g.
// `cx > disp_w` instead of `cx >= disp_w`, would report the real display's
// rightmost/bottommost column as offscreen, or a genuinely offscreen point
// one past it as on-panel, and nothing in section14 would catch it).
static void run_section15_point_on_panel_boundary_matrix(void)
{
    TEST_SECTION("kiln_ui_click_by_name()'s offscreen bounds check: exact "
                 "comparison tokens pinned in kiln_ui.c, then point_on_panel()'s "
                 "boundary matrix (inside/edges/just-outside/negative) run "
                 "against them");

    // --- pin the exact comparison tokens in kiln_ui.c first ---
    char *ui_text = read_file_any(KILN_UI_C_CANDIDATES, 3);
    char *ui_stripped = ui_text ? strip_c_comments(ui_text) : NULL;
    if (!ui_stripped) {
        TEST_CHECK(false, "could not locate/strip drivers/ui/kiln_ui.c");
    } else {
        size_t len = 0;
        const char *body = find_function_body(ui_stripped, "kiln_ui_click_result_t kiln_ui_click_by_name(", &len);
        char *fn = body ? dup_range(body, len) : NULL;
        TEST_CHECK(fn != NULL, "found kiln_ui_click_by_name()'s function body");
        if (fn) {
            TEST_CHECK(strstr(fn, "targets[match].cx < 0") != NULL,
                       "kiln_ui_click_by_name() must reject cx < 0 (negative x is offscreen) -- "
                       "point_on_panel()'s matrix below assumes this exact comparison.");
            TEST_CHECK(strstr(fn, "targets[match].cx >= disp_w") != NULL,
                       "kiln_ui_click_by_name() must reject cx >= disp_w (disp_w itself is one "
                       "past the last column, so this must be >=, not >) -- point_on_panel()'s "
                       "matrix below assumes this exact comparison.");
            TEST_CHECK(strstr(fn, "targets[match].cy < 0") != NULL,
                       "kiln_ui_click_by_name() must reject cy < 0 (negative y is offscreen) -- "
                       "point_on_panel()'s matrix below assumes this exact comparison.");
            TEST_CHECK(strstr(fn, "targets[match].cy >= disp_h") != NULL,
                       "kiln_ui_click_by_name() must reject cy >= disp_h (disp_h itself is one "
                       "past the last row, so this must be >=, not >) -- point_on_panel()'s "
                       "matrix below assumes this exact comparison.");

            // 2026-09-25, coordinator review follow-up: the four checks above
            // each pin one atom in isolation, so a source edit that kept all
            // four comparisons but joined them with && instead of ||, or that
            // moved KILN_UI_CLICK_OFFSCREEN out from behind the bounds check
            // entirely, would pass every check above while breaking the real
            // gate. Pin the full joined expression (all three "||" joins,
            // exact substring, spanning the source's own line break) and that
            // it -- not some other condition -- is what returns
            // KILN_UI_CLICK_OFFSCREEN.
            // Read raw ("rb", test_common.h's test_read_whole_file()) --
            // the committed blob is LF, but core.autocrlf can rewrite the
            // working-tree copy to CRLF on checkout, so the source's own
            // line break here can be either depending on the checkout.
            // Accept both so this pin doesn't depend on which one landed.
            const char *full_cond_crlf =
                "targets[match].cx < 0 || targets[match].cx >= disp_w ||\r\n"
                "            targets[match].cy < 0 || targets[match].cy >= disp_h";
            const char *full_cond_lf =
                "targets[match].cx < 0 || targets[match].cx >= disp_w ||\n"
                "            targets[match].cy < 0 || targets[match].cy >= disp_h";
            const char *cond_pos = strstr(fn, full_cond_crlf);
            size_t full_cond_len = strlen(full_cond_crlf);
            if (!cond_pos) {
                cond_pos = strstr(fn, full_cond_lf);
                full_cond_len = strlen(full_cond_lf);
            }
            TEST_CHECK(cond_pos != NULL,
                       "the four offscreen comparisons must be joined by || (not && or any "
                       "other operator) in exactly this order: cx<0, cx>=disp_w, cy<0, "
                       "cy>=disp_h -- point_on_panel()'s matrix assumes all four are ORed "
                       "into one bounds check, not evaluated/short-circuited separately.");
            if (cond_pos) {
                const char *after_cond = cond_pos + full_cond_len;
                const char *offscreen_ret = strstr(after_cond, "return KILN_UI_CLICK_OFFSCREEN;");
                const char *next_if = strstr(after_cond, "if (targets[match].hidden)");
                TEST_CHECK(offscreen_ret != NULL, "the joined bounds check must be followed by "
                           "a return KILN_UI_CLICK_OFFSCREEN; -- some other outcome (falling "
                           "through, a different result code) would mean an offscreen tap is "
                           "no longer reported as offscreen.");
                TEST_CHECK(offscreen_ret != NULL && next_if != NULL && offscreen_ret < next_if,
                           "return KILN_UI_CLICK_OFFSCREEN; must come BEFORE the hidden check "
                           "(run_section14 pins this ordering too) -- gate the bounds check, "
                           "don't just place the return text somewhere later in the function.");
            }
            free(fn);
        }
    }
    free(ui_stripped);
    free(ui_text);

    // --- boundary matrix, kiln_ui.h's real 480x320 panel resolution ---
    const int32_t w = 480, h = 320;
    struct { int32_t cx, cy; bool expect_on_panel; const char *label; } cases[] = {
        { 240, 160, true,  "inside, roughly centred" },
        { 0,   0,   true,  "top-left corner, (0,0)" },
        { w-1, h-1, true,  "bottom-right corner, (w-1,h-1)" },
        { 0,   h-1, true,  "bottom-left corner, (0,h-1)" },
        { w-1, 0,   true,  "top-right corner, (w-1,0)" },
        { w,   160, false, "just outside right edge, (w,y)" },
        { 240, h,   false, "just outside bottom edge, (x,h)" },
        { w,   h,   false, "just outside both edges, (w,h)" },
        { -1,  160, false, "negative x, (-1,y)" },
        { 240, -1,  false, "negative y, (x,-1)" },
        { -1,  -1,  false, "both negative, (-1,-1)" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool got = point_on_panel(cases[i].cx, cases[i].cy, w, h);
        TEST_CHECK(got == cases[i].expect_on_panel, cases[i].label);
    }

    // --- a degenerate/never-real resolution shouldn't crash or misreport ---
    TEST_CHECK(point_on_panel(0, 0, 0, 0) == false,
               "a 0x0 reported resolution (e.g. a dispatch that never reached "
               "lvgl_port_task, disp_w/disp_h left zero-initialised) must report "
               "every point offscreen, never on-panel by falling through a "
               "degenerate bound.");
}

static void run_section13_ui_walk_handoff_wired(void)
{
    TEST_SECTION("lvgl_port_collect_tap_targets() cross-task handoff: seq-matched "
                 "completion, copy-out and walk both under s_ui_walk.data, zero-timeout "
                 "take on lvgl_port_task, done given after data released -- source-text "
                 "scan (lvgl_port.c is not host-compilable)");

    char *text = read_file_any(LVGL_PORT_C_CANDIDATES, 3);
    char *stripped = text ? strip_c_comments(text) : NULL;
    if (!stripped) {
        TEST_CHECK(false, "could not locate/strip drivers/ui/lvgl_port.c");
        free(text);
        return;
    }

    // --- requester side ---
    size_t len = 0;
    const char *body = find_function_body(stripped, "size_t lvgl_port_collect_tap_targets(", &len);
    char *fn = body ? dup_range(body, len) : NULL;
    TEST_CHECK(fn != NULL, "found lvgl_port_collect_tap_targets()'s function body");
    if (fn) {
        const char *issue_take = strstr(fn, "xSemaphoreTake(s_ui_walk.data,");
        const char *bump = strstr(fn, "++s_ui_walk.req_seq");
        const char *issue_give = bump ? strstr(bump, "xSemaphoreGive(s_ui_walk.data)") : NULL;
        TEST_CHECK(issue_take != NULL && bump != NULL && issue_give != NULL && issue_take < bump,
                   "the requester must bump s_ui_walk.req_seq (`++s_ui_walk.req_seq`) while "
                   "holding s_ui_walk.data -- otherwise lvgl_port_task can record a "
                   "served_seq for a half-issued request.");

        const char *seq_if = strstr(fn, "if (s_ui_walk.served_seq == my_seq) {");
        TEST_CHECK(seq_if != NULL,
                   "the requester must accept a completion only under "
                   "`if (s_ui_walk.served_seq == my_seq) {` -- without it, a late completion "
                   "of an earlier, timed-out request (or a walk still in flight when this "
                   "request was issued) is taken as this caller's answer.");
        if (seq_if) {
            // The copy-out take is the LAST data take before the seq check.
            const char *copy_take = NULL;
            for (const char *t = strstr(fn, "xSemaphoreTake(s_ui_walk.data,"); t && t < seq_if;
                 t = strstr(t + 1, "xSemaphoreTake(s_ui_walk.data,")) {
                copy_take = t;
            }
            const char *copy = strstr(seq_if, "memcpy(out, s_ui_walk_targets,");
            const char *copy_give = strstr(seq_if, "xSemaphoreGive(s_ui_walk.data)");
            TEST_CHECK(copy_take != NULL && copy_take > issue_give && copy != NULL &&
                           copy_give != NULL && copy < copy_give,
                       "the copy-out (`memcpy(out, s_ui_walk_targets, ...)`) must sit inside "
                       "the served_seq check, after a fresh take of s_ui_walk.data and before "
                       "its release -- otherwise lvgl_port_task can start another walk into "
                       "s_ui_walk_targets mid-copy (a torn name/coordinate pair).");
            int copies = 0;
            for (const char *c = strstr(fn, "s_ui_walk_targets"); c; c = strstr(c + 1, "s_ui_walk_targets")) {
                copies++;
            }
            TEST_CHECK(copies == 1,
                       "s_ui_walk_targets must be read exactly once in the requester (the "
                       "seq-checked, locked copy-out) -- a second read is unguarded.");
        }
        free(fn);
    }

    // --- lvgl_port_task side ---
    body = find_function_body(stripped, "static void lvgl_port_task(", &len);
    fn = body ? dup_range(body, len) : NULL;
    TEST_CHECK(fn != NULL, "found lvgl_port_task()'s function body");
    if (fn) {
        const char *take = strstr(fn, "xSemaphoreTake(s_ui_walk.data, 0)");
        int takes = 0;
        for (const char *t = strstr(fn, "xSemaphoreTake(s_ui_walk."); t; t = strstr(t + 1, "xSemaphoreTake(s_ui_walk.")) {
            takes++;
        }
        TEST_CHECK(take != NULL && takes == 1,
                   "lvgl_port_task must take s_ui_walk.data only with a zero timeout "
                   "(`xSemaphoreTake(s_ui_walk.data, 0)`) and take no other s_ui_walk "
                   "semaphore -- a blocking take here stalls the LVGL task on a requester.");
        const char *walk = take ? strstr(take, "kiln_ui_collect_tap_targets(s_ui_walk_targets,") : NULL;
        const char *served = take ? strstr(take, "s_ui_walk.served_seq = s_ui_walk.req_seq") : NULL;
        const char *give_data = take ? strstr(take, "xSemaphoreGive(s_ui_walk.data)") : NULL;
        const char *give_done = give_data ? strstr(give_data, "xSemaphoreGive(s_ui_walk.done)") : NULL;
        TEST_CHECK(walk != NULL && served != NULL && give_data != NULL && walk < give_data &&
                       served < give_data,
                   "lvgl_port_task must walk into s_ui_walk_targets and record "
                   "`s_ui_walk.served_seq = s_ui_walk.req_seq` while holding s_ui_walk.data.");
        TEST_CHECK(give_done != NULL && strstr(fn, "xSemaphoreGive(s_ui_walk.done)") == give_done,
                   "lvgl_port_task must give s_ui_walk.done only AFTER releasing "
                   "s_ui_walk.data -- a requester woken while data is still held can burn "
                   "its remaining window waiting for the lock.");
        free(fn);
    }

    free(stripped);
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
    run_section7_flush_and_indev_cb_mutator_denylist();
    run_section8_timer_refresh_cb_blocking_denylist();
    run_section9_idle_lock_scope_denylist();
    run_section10_swallow_diag_wired();
    run_section11_inject_verdict_handoff_wired();
    run_section12_inject_failed_wired();
    run_section13_ui_walk_handoff_wired();
    run_section14_offscreen_checked_before_hidden();
    run_section15_point_on_panel_boundary_matrix();
}
