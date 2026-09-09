// test_reboot_in_place_wiring.c -- SAFETY_CMD_REBOOT (0x29), the Pico half
// of KilnFW's POST /api/sw_reset (2026-09-09).
//
// Like test_update_task_relay_wiring.c / test_safety_core_s8_wiring.c before
// it, this scans the ACTUAL compiled source text rather than restating its
// logic somewhere host-friendly: src/tasks/update_task.c and
// src/tasks/link_task.c both pull in FreeRTOS.h / pico/flash.h /
// hardware/flash.h / hardware/watchdog.h, none of which exist off real
// hardware, so neither file can be host-compiled and called directly.
//
// THE PROPERTIES, all three of which are the reason this command was allowed
// to exist at all:
//
//   1. IT TOUCHES NO CONFIGURATION. update_task_reboot_now() -- the whole of
//      the act half -- must contain no flash write, no config_store call, and
//      no bootloader-metadata persist. That is the entire difference between
//      this command and SAFETY_CMD_ROLLBACK (0x17), which deliberately DOES
//      write a metadata record before its own reboot. A future edit that
//      slipped a persist into this path would turn "reboot in place" into
//      something that changes what the board boots, silently.
//
//   2. IT REUSES THE ONE REBOOT MECHANISM. hal_wdt_reboot() (pico-sdk's
//      watchdog_reboot()) is the single reboot path in this firmware; a
//      second one introduced here would be a second thing to reason about
//      during a fault.
//
//   3. IT IS REFUSABLE, and refuses on the same ARMED gate every other
//      consequential command uses -- and the accept/refuse decision is made
//      BEFORE the reply is sent, which is made before the reset. Get that
//      order wrong and the operator is told nothing, or told the wrong thing,
//      about a processor that has already vanished.
//
// FAILS CLOSED: a missing source file, or a renamed function this scan can no
// longer find defined where expected, is a TEST FAILURE, never a skip -- a
// scan that silently matches nothing would be worse than no test at all.
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

// Mirrors test_update_task_relay_wiring.c's read_file_any() exactly -- same
// "different build layouts have different working directories" reasoning.
static char *read_file_any(const char *const *candidates, size_t count)
{
    return test_read_source_anchored(__FILE__, candidates[0], candidates, count);
}

static const char *UPDATE_TASK_CANDIDATES[] = {
    "../src/tasks/update_task.c",
    "src/tasks/update_task.c",
    "firmware/SaftyFW/src/tasks/update_task.c",
};

static const char *LINK_TASK_CANDIDATES[] = {
    "../src/tasks/link_task.c",
    "src/tasks/link_task.c",
    "firmware/SaftyFW/src/tasks/link_task.c",
};

// Rewrites C comments to spaces, in place. Every scan below runs on stripped
// text, because these functions are heavily commented and their comments
// legitimately NAME the very things the code must not DO ("no
// config_store call", "compare update_task_request_rollback(), which
// deliberately does"). Scanning raw text would either fail on the comments or
// force the comments to be written around the test, which is exactly
// backwards: the comments are the documentation and the test must read the
// code. Preserves length and line structure so nothing else has to change.
static void strip_comments(char *text)
{
    bool in_block = false;
    bool in_line = false;
    for (char *p = text; *p; ++p) {
        if (in_line) {
            if (*p == '\n') {
                in_line = false;
            } else {
                *p = ' ';
            }
        } else if (in_block) {
            if (p[0] == '*' && p[1] == '/') {
                *p++ = ' ';
                *p = ' ';
                in_block = false;
            } else if (*p != '\n') {
                *p = ' ';
            }
        } else if (p[0] == '/' && p[1] == '/') {
            *p++ = ' ';
            *p = ' ';
            in_line = true;
        } else if (p[0] == '/' && p[1] == '*') {
            *p++ = ' ';
            *p = ' ';
            in_block = true;
        }
    }
}

// Returns a malloc'd copy of the body of the function whose definition line
// contains `signature`, from its opening brace to the matching closing brace
// at brace-depth 0. NULL if the signature is absent or the braces never
// balance -- both of which the callers treat as failures, never as "nothing
// to check". Brace counting is deliberately crude (it does not understand
// braces inside string literals or comments); the functions it is pointed at
// contain neither, and the sanity checks below assert that the extracted body
// still contains the text it is supposed to contain, so a mis-extraction
// shows up as a failure rather than as a vacuous pass.
static char *extract_function_body(const char *text, const char *signature)
{
    const char *sig = strstr(text, signature);
    if (!sig) {
        return NULL;
    }
    const char *open = strchr(sig, '{');
    if (!open) {
        return NULL;
    }
    int depth = 0;
    const char *p = open;
    for (; *p; ++p) {
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) {
                break;
            }
        }
    }
    if (depth != 0 || !*p) {
        return NULL;
    }
    size_t len = (size_t)(p - open) + 1u;
    char *body = (char *)malloc(len + 1u);
    if (!body) {
        return NULL;
    }
    memcpy(body, open, len);
    body[len] = '\0';
    return body;
}

// Property 1 + 2: the act half writes nothing and reuses hal_wdt_reboot().
static void test_reboot_now_touches_no_configuration(void)
{
    TEST_SECTION("update_task_reboot_now() writes NOTHING -- no flash, no config_store, no "
                 "bootloader metadata -- and reboots via the existing hal_wdt_reboot()");

    char *text = read_file_any(UPDATE_TASK_CANDIDATES,
                               sizeof(UPDATE_TASK_CANDIDATES) / sizeof(UPDATE_TASK_CANDIDATES[0]));
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/update_task.c from the host test's working "
                          "directory -- update the candidate paths in this test if the build "
                          "layout moved");
        return;
    }
    strip_comments(text);

    char *body = extract_function_body(text, "void update_task_reboot_now(void)");
    if (!body) {
        TEST_CHECK(false, "update_task_reboot_now(void) is not defined in update_task.c with its "
                          "expected signature, or its braces do not balance -- the scan below "
                          "would prove nothing, so this is a failure, not a skip");
        free(text);
        return;
    }

    // Sanity first: prove the extraction actually captured the function, so
    // the absence checks below cannot pass vacuously on an empty string.
    TEST_CHECK(strstr(body, "hal_wdt_reboot()") != NULL,
               "sanity: the extracted body really is update_task_reboot_now() -- it contains the "
               "hal_wdt_reboot() call, which is also property 2: the ONE reboot mechanism this "
               "firmware has, reused rather than duplicated");
    TEST_CHECK(strstr(body, "watchdog_reboot(") == NULL,
               "the reboot goes through hal_wdt_reboot(), not a second, raw watchdog_reboot() "
               "call introduced alongside it");

    // Property 1, the whole point of the command.
    TEST_CHECK(strstr(body, "config_store_") == NULL,
               "update_task_reboot_now() calls nothing in config_store -- a reboot in place must "
               "not touch configuration");
    TEST_CHECK(strstr(body, "persist_metadata") == NULL,
               "update_task_reboot_now() persists no bootloader metadata record -- that is what "
               "makes this a reboot in place and not a rollback (compare "
               "update_task_request_rollback(), which deliberately does)");
    TEST_CHECK(strstr(body, "flash_safe_execute") == NULL && strstr(body, "flash_range_") == NULL,
               "update_task_reboot_now() performs no flash write of any kind");
    TEST_CHECK(strstr(body, "bootloader_decide") == NULL,
               "update_task_reboot_now() makes no bootloader slot decision -- it comes back on "
               "exactly the image it is running");

    free(body);
    free(text);
}

// Property 3, the policy half: update_task_reboot_allowed() gathers the two
// live inputs and defers the actual ARMED/TRANSFER_ACTIVE decision to
// update_task_reboot_policy_decide() (update_task_reboot_policy.c, a
// separate freestanding file with no FreeRTOS/pico dependency -- 2026-09-09,
// see that file's own header comment). THIS scan only pins the WIRING: that
// update_task_reboot_allowed() reads relay state the legal way, calls the
// pure decision function, and never reboots or writes anything itself. The
// decision table's actual input/output behaviour -- what each combination of
// relay-armed and transfer-active actually returns -- is no longer provable
// by a source-text scan (a scan proves identifiers are PRESENT, not that the
// function returns the right thing for a given input) and is instead pinned
// behaviourally, against the real linked-in production function, by
// test_update_task_reboot_policy.c.
static void test_reboot_allowed_is_the_armed_gate_only(void)
{
    TEST_SECTION("update_task_reboot_allowed() is pure policy wiring -- it reads relay/transfer "
                 "state the legal way, defers the decision to update_task_reboot_policy_decide(), "
                 "and never reboots or writes anything itself");

    char *text = read_file_any(UPDATE_TASK_CANDIDATES,
                               sizeof(UPDATE_TASK_CANDIDATES) / sizeof(UPDATE_TASK_CANDIDATES[0]));
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/update_task.c");
        return;
    }
    strip_comments(text);

    char *body = extract_function_body(
        text, "bool update_task_reboot_allowed(const char **out_reason, uint8_t *out_reason_code)");
    if (!body) {
        TEST_CHECK(false, "update_task_reboot_allowed() is not defined in update_task.c with its "
                          "expected signature, or its braces do not balance");
        free(text);
        return;
    }

    TEST_CHECK(strstr(body, "safety_core_get_output_status") != NULL,
               "the refusal reads relay state through safety_core_get_output_status() -- the same "
               "legal channel update_task_request_rollback() uses, not relay_owner.h directly");
    TEST_CHECK(strstr(body, "update_task_transfer_active") != NULL,
               "the wrapper also reads the transfer-active state, so the pure decision function "
               "gets both live inputs");
    TEST_CHECK(strstr(body, "update_task_reboot_policy_decide") != NULL,
               "the actual ARMED/TRANSFER_ACTIVE decision is made by the pure, host-tested "
               "update_task_reboot_policy_decide() -- not restated inline here where it could "
               "silently drift from the behaviourally-tested copy");
    TEST_CHECK(strstr(body, "hal_wdt_reboot") == NULL,
               "the policy half never reboots -- that separation is what lets link_task reply on "
               "the wire BEFORE the reset, which a rollback structurally cannot do");

    free(body);
    free(text);
}

// Property 3, the ordering half. This is the sequencing that makes the ESP's
// report honest: decide, then reply, then reset. Checked by relative position
// in the handler body, because a reply queued after the reset call would
// never be sent and a reply queued before the decision would be a lie.
static void test_link_task_replies_before_it_reboots(void)
{
    TEST_SECTION("link_task_handle_reboot() decides, THEN replies on the wire, THEN resets -- in "
                 "that order");

    char *text = read_file_any(LINK_TASK_CANDIDATES,
                               sizeof(LINK_TASK_CANDIDATES) / sizeof(LINK_TASK_CANDIDATES[0]));
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/link_task.c from the host test's working "
                          "directory -- update the candidate paths in this test if the build "
                          "layout moved");
        return;
    }
    strip_comments(text);

    char *body = extract_function_body(text, "static void link_task_handle_reboot(const kilnlink_frame_t *frame)");
    if (!body) {
        TEST_CHECK(false, "link_task_handle_reboot() is not defined in link_task.c with its "
                          "expected signature, or its braces do not balance");
        free(text);
        return;
    }

    const char *decide = strstr(body, "update_task_reboot_allowed(");
    const char *reply = strstr(body, "link_task_send_broadcast(");
    const char *reset = strstr(body, "update_task_reboot_now(");

    TEST_CHECK(decide != NULL, "the handler asks update_task_reboot_allowed() for the decision");
    TEST_CHECK(reply != NULL, "the handler sends a reply frame");
    TEST_CHECK(reset != NULL, "the handler resets the chip via update_task_reboot_now()");
    if (!decide || !reply || !reset) {
        free(body);
        free(text);
        return;
    }

    TEST_CHECK(decide < reply, "the decision is made BEFORE the reply is queued -- a reply sent "
                               "first could only be a guess");
    TEST_CHECK(reply < reset, "the reply is queued BEFORE the reset -- anything after "
                              "update_task_reboot_now() never runs, so a reply queued after it "
                              "would silently never reach the ESP");

    TEST_CHECK(strstr(body, "kilnlink_reboot_result_encode") != NULL,
               "the reply is a real SAFETY_CMD_REBOOT_RESULT frame, built by its own codec");
    TEST_CHECK(strstr(body, "kilnlink_reboot_decode") != NULL,
               "the incoming frame is decoded (and a malformed one rejected) before anything acts "
               "on it -- untrusted wire input, CommonFW/README.md rule 6");

    // The drain: the reply must actually leave the UART before the chip
    // resets out from under the TX ring.
    TEST_CHECK(strstr(body, "uart_owner_get_tx_head()") != NULL &&
                   strstr(body, "uart_owner_get_tx_tail()") != NULL,
               "the handler waits for the TX ring to drain before resetting, rather than assuming "
               "a queued frame has already gone out");

    free(body);
    free(text);
}

// A reboot in place must never be mistaken for, or implemented as, a
// rollback: the two are one wire id apart and only one of them changes which
// firmware the board comes back on.
static void test_reboot_is_not_a_rollback(void)
{
    TEST_SECTION("the reboot handler shares no machinery with the rollback handler -- 0x29 must "
                 "never reach the slot-changing path");

    char *text = read_file_any(LINK_TASK_CANDIDATES,
                               sizeof(LINK_TASK_CANDIDATES) / sizeof(LINK_TASK_CANDIDATES[0]));
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/link_task.c");
        return;
    }
    strip_comments(text);

    char *body = extract_function_body(text, "static void link_task_handle_reboot(const kilnlink_frame_t *frame)");
    if (!body) {
        TEST_CHECK(false, "link_task_handle_reboot() is not defined in link_task.c with its "
                          "expected signature, or its braces do not balance");
        free(text);
        return;
    }

    TEST_CHECK(strstr(body, "update_task_request_rollback") == NULL,
               "the reboot handler never calls update_task_request_rollback() -- that function "
               "marks the running slot BAD and comes back on the OTHER image");
    TEST_CHECK(strstr(body, "rollback_result") == NULL,
               "the reboot handler never emits a ROLLBACK_RESULT frame -- it has its own reply "
               "(0x2A), and an ESP told the wrong one would draw the wrong conclusion about "
               "which firmware is now running");

    // And the dispatch really does route 0x29 here, rather than this handler
    // being dead code nothing calls.
    TEST_CHECK(strstr(text, "case LINK_FRAME_REBOOT_CMD:") != NULL,
               "link_task's dispatch switch has a case for LINK_FRAME_REBOOT_CMD (0x29)");
    TEST_CHECK(strstr(text, "link_task_handle_reboot(&frame);") != NULL,
               "that case actually calls link_task_handle_reboot()");

    free(body);
    free(text);
}

void run_test_reboot_in_place_wiring(void)
{
    test_reboot_now_touches_no_configuration();
    test_reboot_allowed_is_the_armed_gate_only();
    test_link_task_replies_before_it_reboots();
    test_reboot_is_not_a_rollback();
}
