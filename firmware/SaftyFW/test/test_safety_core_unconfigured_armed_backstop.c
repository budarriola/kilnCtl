// test_safety_core_unconfigured_armed_backstop.c -- KILN_PROFILES_PLAN.md
// item 16: an unconfigured Pico (abs_max_temp_c's fields_set bit clear) must
// never be silently ARMED. safety_core.c is not host-compilable (FreeRTOS.h,
// pico/time.h, relay_owner.h all reach real hardware) -- same precedent as
// test_safety_core_s8_wiring.c and test_safety_core_stack_budget.c, so this
// pins the backstop by scanning the real, compiled-into-firmware source text
// rather than a paraphrase. A change that deletes or weakens the backstop
// makes this fail; a change that only rewords a comment near it does not.
//
// What this does NOT prove: this is a source-text scan, not a runtime test
// (safety_core.c cannot be linked on the host at all -- see the file header
// comment above). It proves the check exists, uses the SAME fields_set bit
// safety_core_load_guard_cfg() uses (so it cannot silently drift onto a
// different, wrong bit -- project_reset_one_side_bug_class), gates on
// RELAY_OWNER_STATE_ARMED specifically, and calls relay_owner_command_
// energize(false) -- never a raw GPIO/relay write (project_bypassed_owner_
// module_bug_class). Runtime coverage of "the primary interlock chain
// refuses ON for an uncommissioned board" already exists via the
// commissioning_gate/relay_owner test suites (opus review d22431d0); this
// file covers the SEPARATE, second-layer backstop item 16 adds.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

static char *read_source(void)
{
    static const char *candidates[] = {
        "../src/tasks/safety_core.c",
        "src/tasks/safety_core.c",
        "firmware/SaftyFW/src/tasks/safety_core.c",
    };
    return test_read_source_anchored(__FILE__, candidates[0], candidates,
                                      sizeof(candidates) / sizeof(candidates[0]));
}

// Locates the backstop block by its distinctive local variable name,
// s_unconfigured_armed_warned's own use-site (not its declaration) --
// unique enough not to collide with anything else in the file.
static const char *find_backstop_block(const char *text)
{
    const char *decl = strstr(text, "static bool s_unconfigured_armed_warned;");
    if (!decl) {
        return NULL;
    }
    const char *use = strstr(decl, "abs_max_temp_c_unconfigured");
    return use;
}

static void test_backstop_declared_and_reachable(void)
{
    TEST_SECTION("safety_core.c: s_unconfigured_armed_warned is declared and used");
    char *text = read_source();
    TEST_CHECK(text != NULL, "safety_core.c source is readable");
    if (!text) {
        return;
    }
    TEST_CHECK(find_backstop_block(text) != NULL,
               "the item-16 backstop block (abs_max_temp_c_unconfigured) exists after the "
               "s_unconfigured_armed_warned declaration");
    free(text);
}

static void test_backstop_uses_the_same_fields_set_bit_as_s1(void)
{
    TEST_SECTION("safety_core.c: backstop reads CONFIG_STORE_SET_ABS_MAX_TEMP_C, "
                 "the SAME bit safety_core_load_guard_cfg() uses for S1");
    char *text = read_source();
    TEST_CHECK(text != NULL, "safety_core.c source is readable");
    if (!text) {
        return;
    }

    // S1's own read, a few lines above the backstop -- pin that it exists so
    // a future refactor of THIS check's own anchor is caught rather than
    // silently matching nothing.
    TEST_CHECK(strstr(text, "(rec->fields_set & CONFIG_STORE_SET_ABS_MAX_TEMP_C) ? rec->abs_max_temp_c : 0.0f;") !=
                   NULL,
               "fixture: S1's own fields_set-gated abs_max_temp_c copy is still present as written");

    const char *block = find_backstop_block(text);
    TEST_CHECK(block != NULL, "fixture: backstop block located");
    if (!block) {
        free(text);
        return;
    }

    // The backstop's own condition line, within a short, bounded window of
    // its variable's assignment -- guards against matching some unrelated
    // later use of the same macro name elsewhere in the file.
    size_t window = 2400;
    size_t avail = strlen(block);
    size_t len = (avail < window) ? avail : window;
    char *window_buf = (char *)malloc(len + 1);
    TEST_CHECK(window_buf != NULL, "fixture: window buffer allocated");
    if (!window_buf) {
        free(text);
        return;
    }
    memcpy(window_buf, block, len);
    window_buf[len] = '\0';

    TEST_CHECK(strstr(window_buf, "CONFIG_STORE_SET_ABS_MAX_TEMP_C) == 0u") != NULL,
               "backstop's unconfigured test reads CONFIG_STORE_SET_ABS_MAX_TEMP_C, "
               "the same bit S1 itself uses -- not a second, independently-derived fact "
               "that could silently drift from it (project_reset_one_side_bug_class)");
    TEST_CHECK(strstr(window_buf, "RELAY_OWNER_STATE_ARMED") != NULL,
               "backstop gates specifically on RELAY_OWNER_STATE_ARMED");
    TEST_CHECK(strstr(window_buf, "relay_owner_command_energize(false)") != NULL,
               "backstop de-energizes THROUGH relay_owner (never a raw relay/GPIO write -- "
               "project_bypassed_owner_module_bug_class)");
    // It must not invent a ceiling value -- CONFIG_REFERENCE.md section 7's
    // "no default may be a guess dressed as a value". Confirm the backstop
    // never assigns anything to abs_max_temp_c itself.
    TEST_CHECK(strstr(window_buf, "abs_max_temp_c =") == NULL ||
                   strstr(window_buf, "abs_max_temp_c =") > strstr(window_buf, "relay_owner_command_energize"),
               "backstop does not itself assign a fabricated abs_max_temp_c value");

    free(window_buf);
    free(text);
}

// 2026-09-14 review, Finding C: config_store_get_full_record() fails CLOSED
// to config_store_default() (fields_set == 0, byte-identical to a genuinely
// unconfigured record) whenever the seqlock read exhausts its retries -- a
// state proven reachable on a COMMISSIONED, ARMED board, not only a
// never-committed one. The backstop must gate on the function's return
// value (whether a REAL snapshot was obtained) in addition to the
// fields_set bit, or a transient read failure on a healthy, firing board
// reads identically to "genuinely never commissioned" and forces a
// spurious de-energize. Pins that the `!cfg_read_ok` branch exists ahead of
// the ARMED/unconfigured check, and that it does NOT call relay_owner_
// command_energize() itself (a failed read must decline to act, not assume
// either direction).
static void test_backstop_gates_on_a_real_snapshot_not_just_fields_set(void)
{
    TEST_SECTION("safety_core.c: backstop distinguishes a FAILED read from a "
                 "genuinely unconfigured record (2026-09-14 review Finding C)");
    char *text = read_source();
    TEST_CHECK(text != NULL, "safety_core.c source is readable");
    if (!text) {
        return;
    }

    // The read call this tick's cfg_rec comes from -- its return value must
    // be captured into a named variable, not discarded, for the backstop to
    // have anything to gate on.
    const char *get_full_record_call = strstr(text, "config_store_get_full_record(&cfg_rec)");
    TEST_CHECK(get_full_record_call != NULL,
               "fixture: safety_core.c still reads cfg_rec via config_store_get_full_record()");
    TEST_CHECK(strstr(text, "cfg_read_ok = config_store_get_full_record(&cfg_rec)") != NULL,
               "config_store_get_full_record()'s return value is captured (not discarded) as "
               "cfg_read_ok -- the signal the backstop needs to tell a failed read apart from a "
               "genuinely unconfigured record");

    const char *block = find_backstop_block(text);
    TEST_CHECK(block != NULL, "fixture: backstop block located");
    if (!block) {
        free(text);
        return;
    }

    size_t window = 2400;
    size_t avail = strlen(block);
    size_t len = (avail < window) ? avail : window;
    char *window_buf = (char *)malloc(len + 1);
    TEST_CHECK(window_buf != NULL, "fixture: window buffer allocated");
    if (!window_buf) {
        free(text);
        return;
    }
    memcpy(window_buf, block, len);
    window_buf[len] = '\0';

    const char *not_ok_branch = strstr(window_buf, "!cfg_read_ok");
    TEST_CHECK(not_ok_branch != NULL,
               "backstop checks cfg_read_ok before trusting abs_max_temp_c_unconfigured");

    const char *unconfigured_check = strstr(window_buf, "abs_max_temp_c_unconfigured &&");
    TEST_CHECK(not_ok_branch != NULL && unconfigured_check != NULL && not_ok_branch < unconfigured_check,
               "the !cfg_read_ok check comes BEFORE the unconfigured-and-ARMED check, i.e. it "
               "gates entry into it rather than merely being checked somewhere in the file");

    // A failed read must DECLINE to act, not de-energize -- confirm the
    // energize call in this window is not reachable from the !cfg_read_ok
    // branch alone by checking it sits in a later else-if, not directly
    // under the !cfg_read_ok condition's own braces. The cheapest reliable
    // text-level proxy: the energize call appears strictly after the
    // unconfigured_check, not between not_ok_branch and unconfigured_check.
    const char *energize_call = strstr(window_buf, "relay_owner_command_energize(false)");
    TEST_CHECK(energize_call != NULL && unconfigured_check != NULL && energize_call > unconfigured_check,
               "relay_owner_command_energize(false) is reached only via the unconfigured-and-ARMED "
               "branch, never directly from the !cfg_read_ok branch");

    free(window_buf);
    free(text);
}

void run_test_safety_core_unconfigured_armed_backstop(void)
{
    test_backstop_declared_and_reachable();
    test_backstop_uses_the_same_fields_set_bit_as_s1();
    test_backstop_gates_on_a_real_snapshot_not_just_fields_set();
}
