// test_virtual_simfw_port_drift_coverage.c -- pins the REAL source text of
// the three things tools/virtual_simfw/src/virtual_simfw.c hand-copies from
// firmware/SimFW/src/tasks/fault_sched.{h,c} and sim_engine.c, so a
// divergence in the real firmware fails a host test instead of shipping
// silently into `kilnsim --virtual`'s CI-run scenarios.
//
// THE GAP this test closes: virtual_simfw.c's own header comment says its
// recompute_overrides() and device_tick() are "ported... near-verbatim" from
// fault_sched.c's recompute_overrides_locked() and sim_engine.c's
// sim_engine_tick(), and its local fault_type_t enum is "copied 1:1... keep
// in sync by hand if that header ever adds/reorders a value". Nothing
// mechanical enforces any of that. virtual_simfw.c is not part of this
// host-test build (it needs winsock2.h/a live TCP loop, not just pure sim/
// code) and is not exercised by SimFW's normal build either -- it is a
// separate PC-only tool tree. An edit to the real fault_sched.h/.c or
// sim_engine.c that the port doesn't pick up would only ever be caught by
// someone reading both files side by side, which is exactly the kind of gap
// firmware/SaftyFW/test/test_boot_checkin_coverage.c and this directory's
// own test_i2c_owner_bus_scan_probe_len_coverage.c exist to close for other
// files the host harness cannot link.
//
// Same technique as both precedents: read the real file, strip comments,
// and scan the non-comment text for the property under test, asserting the
// target symbol was actually found before asserting anything about its
// content. Comment-stripping is not optional -- this codebase documents
// fixed bugs and hand-porting caveats in prose right next to the code (see
// e.g. fault_sched.h's own "keep in sync by hand" comment, or virtual_simfw.c's
// "ported... near-verbatim" comment above recompute_overrides()), so a naive
// substring search could pass on a comment describing the correct order/
// value even after the real code drifted away from it.
//
// Three checks, three different levels of confidence -- see each section's
// comment for why each is (or is not) attempted:
//
//   1. ENUM ORDER (exact pin). fault_type_t (virtual_simfw.c) must list the
//      same FAULT_SCHED_TYPE_* names, in the same order, as
//      fault_sched_fault_type_t (fault_sched.h). This is the most dangerous
//      of the three: a reordered enumerator silently reindexes every value
//      a scenario could inject, and every scenario keeps "passing" while
//      testing the wrong fault. Mechanical and exact -- no judgment call.
//
//   2. SWITCH-CASE COVERAGE (set, not order). recompute_overrides()'s switch
//      (virtual_simfw.c) must handle exactly the same set of
//      FAULT_SCHED_TYPE_*/FT_* fault types as recompute_overrides_locked()'s
//      switch (fault_sched.c) -- neither missing one (a fault type added to
//      the real engine that the port silently no-ops) nor carrying a stale
//      extra one. This does NOT check that each case's *body* does the same
//      thing -- that would require compiling and running both, which would
//      mean linking FreeRTOS for the real side, exactly the effort this
//      host-test suite exists to avoid (build_host_tests.ps1's whole
//      premise). Set-of-handled-cases is the honest, real thing a text scan
//      can pin: it is a mechanical, low-false-positive check (a case list is
//      either complete or it isn't; nothing about it varies with unrelated
//      edits) and it is exactly the shape of bug most likely in practice --
//      a new fault type lands in fault_sched.c/.h without a matching port
//      update, not a byte-for-byte semantic rewrite of an existing case.
//
//   3. TICK ORDER (ordered anchor pin, not full equivalence). device_tick()
//      (virtual_simfw.c) must reach the same eight structurally-significant
//      points, in the same relative order, as sim_engine_tick()
//      (sim_engine.c): relay mask built -> faults evaluated -> duty[] base
//      from the relay mask -> fault-forced duty override -> K4's veto ->
//      thermal_model_tick() -> post-physics MANUAL override ->
//      current_a[] from the *effective* duty. A full behavioral-equivalence
//      test was considered and rejected: sim_engine_tick() is FreeRTOS-task
//      shaped (queue-drained commands, task-context-only statics) and
//      device_tick() deliberately collapses that scaffolding away (both
//      files' own header comments say so) -- there is no way to run them
//      side-by-side and diff outputs without either reimplementing FreeRTOS
//      task semantics on the host or stubbing so much of sim_engine.c that
//      the "test" would just be re-asserting the port's own assumptions.
//      What IS feasible and non-brittle: both files carry extensive, explicit
//      prose (sim_engine.c's own 9-step tick-order comment, and
//      device_tick()'s comment on the K4-gating block) insisting this exact
//      order matters -- e.g. K4's veto must come AFTER the duty override,
//      never before, or a welded-relay fault could out-live K4 opening. These
//      are not incidental implementation details likely to shuffle on an
//      innocent refactor; they are the one invariant both files' authors
//      already went out of their way to document and warn against breaking.
//      Pinning their relative order via stable, already-shared token names
//      (function calls and field names that are also real, both-sides-linked
//      API -- sim_engine_set_zone_duty_override()'s "zone_duty_override_active"
//      field name, thermal_model_tick(), etc.) is a real, low-noise catch for
//      exactly the failure class these comments warn about, without claiming
//      to verify anything about the arithmetic inside each step.
//
// What this file deliberately does NOT attempt: per-case semantic pinning
// for check 2 (e.g. "TC_DRIFT's offset must be params[0] * elapsed_s") and a
// full data-flow/numeric pin for check 3. Both would require either
// compiling+linking the real FreeRTOS-shaped files on the host (out of
// scope for this suite) or a second hand-maintained textual mirror of each
// case's arithmetic -- which just relocates the "did the copy actually
// stay in sync" problem one level down without solving it, and is exactly
// the kind of brittle-on-every-innocent-edit check that gets deleted for
// crying wolf (this file's own header rationale, and the standing project
// rule against exactly that failure mode).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "test_common.h"

// --- File I/O + comment stripping -- copied from
// test_i2c_owner_bus_scan_probe_len_coverage.c / test_boot_checkin_coverage.c,
// see either file's header comment for why this doesn't need to be
// string/char-literal-aware to be correct on this codebase's source. -------
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

static void strip_comments(char *s)
{
    char *out = s;
    while (*s) {
        if (s[0] == '/' && s[1] == '/') {
            while (*s && *s != '\n') {
                s++;
            }
        } else if (s[0] == '/' && s[1] == '*') {
            s += 2;
            while (*s && !(s[0] == '*' && s[1] == '/')) {
                if (*s == '\n') {
                    *out++ = '\n';
                }
                s++;
            }
            if (*s) {
                s += 2;
            }
        } else {
            *out++ = *s++;
        }
    }
    *out = '\0';
}

// Brace-matches a function body starting after `signature`'s first '{' --
// same as test_i2c_owner_bus_scan_probe_len_coverage.c's find_function_body().
static const char *find_function_body(const char *stripped_text, const char *signature, size_t *out_len)
{
    const char *sig = strstr(stripped_text, signature);
    if (!sig) {
        return NULL;
    }
    const char *brace = strchr(sig, '{');
    if (!brace) {
        return NULL;
    }
    int depth = 0;
    const char *p = brace;
    for (; *p; p++) {
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) {
                p++;
                break;
            }
        }
    }
    if (depth != 0) {
        return NULL;
    }
    *out_len = (size_t)(p - brace);
    return brace;
}

// Finds the body of the LAST "typedef enum {" appearing before `closing_marker`
// (e.g. "} fault_type_t;") in `stripped_text`. Scoping to "last before the
// marker" is what lets fault_sched.h's second, unrelated
// fault_sched_system_target_t enum (defined right after the one this test
// cares about) coexist in the same file without confusing which "typedef
// enum {" belongs to which closing brace.
static const char *find_enum_body(const char *stripped_text, const char *closing_marker, size_t *out_len)
{
    const char *close = strstr(stripped_text, closing_marker);
    if (!close) {
        return NULL;
    }
    const char *needle = "typedef enum {";
    const char *last_begin = NULL;
    for (const char *p = stripped_text; p < close;) {
        const char *hit = strstr(p, needle);
        if (!hit || hit >= close) {
            break;
        }
        last_begin = hit;
        p = hit + 1;
    }
    if (!last_begin) {
        return NULL;
    }
    const char *body_start = last_begin + strlen(needle);
    if (body_start >= close) {
        return NULL;
    }
    *out_len = (size_t)(close - body_start);
    return body_start;
}

#define NAME_LIST_MAX 64
#define NAME_MAX_LEN  64

typedef struct {
    char names[NAME_LIST_MAX][NAME_MAX_LEN];
    int count;
} name_list_t;

static bool name_list_push(name_list_t *list, const char *start, size_t len)
{
    if (list->count >= NAME_LIST_MAX || len >= NAME_MAX_LEN) {
        return false;
    }
    memcpy(list->names[list->count], start, len);
    list->names[list->count][len] = '\0';
    list->count++;
    return true;
}

static bool name_in_list(const name_list_t *list, const char *name)
{
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

// Extracts every identifier in [text, text+len) that starts with `prefix`,
// in order of appearance, with the prefix stripped -- used for the enum
// body scan (check 1), where every occurrence of the prefix inside the
// (comment-stripped, enum-body-scoped) text is an enumerator label.
static void extract_prefixed_identifiers(const char *text, size_t len, const char *prefix, name_list_t *out)
{
    out->count = 0;
    size_t prefix_len = strlen(prefix);
    const char *end = text + len;
    const char *p = text;
    while (p < end) {
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *tok_start = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
                p++;
            }
            size_t tok_len = (size_t)(p - tok_start);
            if (tok_len > prefix_len && strncmp(tok_start, prefix, prefix_len) == 0) {
                name_list_push(out, tok_start + prefix_len, tok_len - prefix_len);
            }
        } else {
            p++;
        }
    }
}

// Extracts the identifier following every "case <prefix>...:" occurrence in
// [text, text+len), prefix stripped -- used for the switch-case coverage
// scan (check 2), scoped to one function body so it cannot pick up an
// unrelated case label elsewhere in the file.
static void extract_case_labels(const char *text, size_t len, const char *prefix, name_list_t *out)
{
    out->count = 0;
    size_t prefix_len = strlen(prefix);
    const char *needle = "case ";
    const char *end = text + len;
    for (const char *p = text; p < end;) {
        const char *hit = strstr(p, needle);
        if (!hit || hit >= end) {
            break;
        }
        const char *tok_start = hit + strlen(needle);
        while (tok_start < end && isspace((unsigned char)*tok_start)) {
            tok_start++;
        }
        if ((size_t)(end - tok_start) > prefix_len && strncmp(tok_start, prefix, prefix_len) == 0) {
            const char *tok_end = tok_start;
            while (tok_end < end && (isalnum((unsigned char)*tok_end) || *tok_end == '_')) {
                tok_end++;
            }
            name_list_push(out, tok_start + prefix_len, (size_t)(tok_end - tok_start) - prefix_len);
        }
        p = hit + strlen(needle);
    }
}

// Bounded strstr -- `haystack` is a slice of a larger NUL-terminated buffer
// (a function/enum body carved out of the whole file's text), so a plain
// strstr() would happily match past the slice's end into unrelated later
// code. This caps the search to exactly [haystack, haystack+haystack_len).
static const char *bounded_strstr(const char *haystack, size_t haystack_len, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > haystack_len) {
        return NULL;
    }
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

// Confirms every anchors[i] appears in body, in strictly increasing
// position order (each search starts just past the previous match) -- see
// this file's header comment, check 3, for why these particular anchors.
static bool anchors_in_order(const char *body, size_t body_len, const char *const *anchors, size_t n,
                              size_t *out_fail_index)
{
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        const char *hit = bounded_strstr(body + pos, body_len - pos, anchors[i]);
        if (!hit) {
            *out_fail_index = i;
            return false;
        }
        pos = (size_t)(hit - body) + 1;
    }
    return true;
}

// ===========================================================================
// Check 1 -- enum order pin: fault_type_t (virtual_simfw.c) vs
// fault_sched_fault_type_t (fault_sched.h).
// ===========================================================================
static void check_enum_order(void)
{
    TEST_SECTION("virtual_simfw fault_type_t enum order pins fault_sched_fault_type_t (fault_sched.h)");

    static const char *header_candidates[] = {
        "../src/tasks/fault_sched.h",
        "src/tasks/fault_sched.h",
        "firmware/SimFW/src/tasks/fault_sched.h",
    };
    static const char *port_candidates[] = {
        "../tools/virtual_simfw/src/virtual_simfw.c",
        "tools/virtual_simfw/src/virtual_simfw.c",
        "firmware/SimFW/tools/virtual_simfw/src/virtual_simfw.c",
    };

    char *header_text = read_file_any(header_candidates, 3);
    char *port_text = read_file_any(port_candidates, 3);
    if (!header_text || !port_text) {
        TEST_CHECK(false, "could not locate fault_sched.h and/or virtual_simfw.c from the host "
                           "test's working directory -- update the candidate paths in "
                           "test_virtual_simfw_port_drift_coverage.c if the build layout moved");
        free(header_text);
        free(port_text);
        return;
    }

    // Comments stripped before scanning either enum -- fault_sched.h's own
    // per-value doc comments describe each enumerator's meaning in prose
    // (e.g. "params[0] = ..." next to several names), which could otherwise
    // satisfy a naive search regardless of the real (post-strip) order.
    strip_comments(header_text);
    strip_comments(port_text);

    size_t header_body_len = 0;
    const char *header_body = find_enum_body(header_text, "} fault_sched_fault_type_t;", &header_body_len);
    TEST_CHECK(header_body != NULL, "fault_sched_fault_type_t enum body not found in fault_sched.h's "
                                     "(comment-stripped) source text -- the enum may have been renamed; "
                                     "update this test's closing-marker string if so");

    size_t port_body_len = 0;
    const char *port_body = find_enum_body(port_text, "} fault_type_t;", &port_body_len);
    TEST_CHECK(port_body != NULL, "fault_type_t enum body not found in virtual_simfw.c's (comment-stripped) "
                                   "source text -- the enum may have been renamed; update this test's "
                                   "closing-marker string if so");

    if (!header_body || !port_body) {
        free(header_text);
        free(port_text);
        return;
    }

    name_list_t header_names;
    name_list_t port_names;
    extract_prefixed_identifiers(header_body, header_body_len, "FAULT_SCHED_TYPE_", &header_names);
    extract_prefixed_identifiers(port_body, port_body_len, "FT_", &port_names);

    TEST_CHECK(header_names.count > 0, "no FAULT_SCHED_TYPE_* enumerators found in fault_sched.h's enum "
                                        "body -- the extraction itself is broken, not a real pass");
    TEST_CHECK(port_names.count > 0, "no FT_* enumerators found in virtual_simfw.c's enum body -- the "
                                      "extraction itself is broken, not a real pass");

    TEST_CHECK(header_names.count == port_names.count,
               "fault_sched_fault_type_t (fault_sched.h) and fault_type_t (virtual_simfw.c) have a "
               "different number of enumerators -- a value was added/removed on one side without the "
               "other; virtual_simfw.c's own comment says to keep this enum in sync by hand");

    int n = (header_names.count < port_names.count) ? header_names.count : port_names.count;
    for (int i = 0; i < n; i++) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "enumerator #%d out of order/mismatched: fault_sched.h has FAULT_SCHED_TYPE_%s, "
                 "virtual_simfw.c has FT_%s at the same position -- a reordered or renamed fault type "
                 "silently reindexes every fault id a scenario can inject",
                 i, header_names.names[i], port_names.names[i]);
        TEST_CHECK(strcmp(header_names.names[i], port_names.names[i]) == 0, msg);
    }

    free(header_text);
    free(port_text);
}

// ===========================================================================
// Check 2 -- switch-case coverage pin: recompute_overrides() (virtual_simfw.c)
// vs recompute_overrides_locked() (fault_sched.c) must handle the same SET
// of fault types (order doesn't matter -- it's a switch).
// ===========================================================================
static void check_switch_case_coverage(void)
{
    TEST_SECTION("virtual_simfw recompute_overrides() handles the same fault types as "
                 "fault_sched.c's recompute_overrides_locked()");

    static const char *real_candidates[] = {
        "../src/tasks/fault_sched.c",
        "src/tasks/fault_sched.c",
        "firmware/SimFW/src/tasks/fault_sched.c",
    };
    static const char *port_candidates[] = {
        "../tools/virtual_simfw/src/virtual_simfw.c",
        "tools/virtual_simfw/src/virtual_simfw.c",
        "firmware/SimFW/tools/virtual_simfw/src/virtual_simfw.c",
    };

    char *real_text = read_file_any(real_candidates, 3);
    char *port_text = read_file_any(port_candidates, 3);
    if (!real_text || !port_text) {
        TEST_CHECK(false, "could not locate fault_sched.c and/or virtual_simfw.c from the host test's "
                           "working directory -- update the candidate paths in "
                           "test_virtual_simfw_port_drift_coverage.c if the build layout moved");
        free(real_text);
        free(port_text);
        return;
    }

    // Comment-stripping matters here too: recompute_overrides_locked()'s own
    // file header prose repeatedly names FAULT_SCHED_TYPE_* values while
    // explaining the "recompute, don't patch" strategy.
    strip_comments(real_text);
    strip_comments(port_text);

    size_t real_body_len = 0;
    const char *real_body =
        find_function_body(real_text, "static void recompute_overrides_locked(void)", &real_body_len);
    TEST_CHECK(real_body != NULL, "recompute_overrides_locked() function body not found in fault_sched.c's "
                                   "(comment-stripped) source text -- it may have been renamed/restructured; "
                                   "update this test's signature string if so");

    size_t port_body_len = 0;
    const char *port_body = find_function_body(port_text, "static void recompute_overrides(device_t *d)",
                                                &port_body_len);
    TEST_CHECK(port_body != NULL, "recompute_overrides() function body not found in virtual_simfw.c's "
                                   "(comment-stripped) source text -- it may have been renamed/restructured; "
                                   "update this test's signature string if so");

    if (!real_body || !port_body) {
        free(real_text);
        free(port_text);
        return;
    }

    name_list_t real_cases;
    name_list_t port_cases;
    extract_case_labels(real_body, real_body_len, "FAULT_SCHED_TYPE_", &real_cases);
    extract_case_labels(port_body, port_body_len, "FT_", &port_cases);

    TEST_CHECK(real_cases.count > 0, "no 'case FAULT_SCHED_TYPE_*' labels found inside "
                                      "recompute_overrides_locked()'s body -- the extraction itself is "
                                      "broken, not a real pass");
    TEST_CHECK(port_cases.count > 0, "no 'case FT_*' labels found inside recompute_overrides()'s body -- "
                                      "the extraction itself is broken, not a real pass");

    TEST_CHECK(real_cases.count == port_cases.count,
               "fault_sched.c's recompute_overrides_locked() and virtual_simfw.c's recompute_overrides() "
               "handle a different NUMBER of fault-type cases -- a case was added/removed on one side "
               "without the other");

    for (int i = 0; i < real_cases.count; i++) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "fault_sched.c's recompute_overrides_locked() handles case FAULT_SCHED_TYPE_%s, but "
                 "virtual_simfw.c's recompute_overrides() has no matching 'case FT_%s' -- the port "
                 "silently no-ops this fault type instead of applying its override, and every scenario "
                 "using it will keep passing against a virtual DUT that never actually models the fault",
                 real_cases.names[i], real_cases.names[i]);
        TEST_CHECK(name_in_list(&port_cases, real_cases.names[i]), msg);
    }
    for (int i = 0; i < port_cases.count; i++) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "virtual_simfw.c's recompute_overrides() handles case FT_%s, but fault_sched.c's "
                 "recompute_overrides_locked() has no matching 'case FAULT_SCHED_TYPE_%s' -- the port is "
                 "carrying a stale case for a fault type the real engine no longer has",
                 port_cases.names[i], port_cases.names[i]);
        TEST_CHECK(name_in_list(&real_cases, port_cases.names[i]), msg);
    }

    free(real_text);
    free(port_text);
}

// ===========================================================================
// Check 3 -- tick order pin: device_tick() (virtual_simfw.c) vs
// sim_engine_tick() (sim_engine.c) must reach the same structurally
// significant points in the same relative order. See this file's header
// comment for why this (anchor order), and not full equivalence, is the
// right-sized check here.
// ===========================================================================
typedef struct {
    const char *real_anchor;
    const char *port_anchor;
    const char *desc;
} anchor_pair_t;

static const anchor_pair_t k_tick_anchors[] = {
    {"SIM_RELAY_BIT_K1", "SIM_RELAY_BIT_K1", "relay_mask built from the K1..K4 sense bits"},
    {"fault_sched_tick(", "fault_engine_tick(",
     "faults evaluated for this tick (real: fault_sched_tick(); port: its inlined "
     "fault_engine_tick()/apply_edge_effects()/recompute_overrides() equivalent)"},
    {"zone_relay_bit[z]", "zone_relay_bit[z]", "duty[] base computed from the relay mask"},
    {"zone_duty_override_active[z]", "zone_duty_override_active[z]",
     "fault-forced duty override applied on top of the relay-derived base"},
    {"k4_closed", "k4_closed", "K4 pilot-relay veto gates duty AFTER the override, never before"},
    {"thermal_model_tick(", "thermal_model_tick(", "physics step"},
    {"zone_manual[z]", "zone_manual[z]", "MANUAL zone-temp override applied post-physics"},
    {"current_a[z] =", "current_a[z] =", "current computed from the *effective* (post-override, post-K4) duty"},
};
#define K_TICK_ANCHORS_COUNT (sizeof(k_tick_anchors) / sizeof(k_tick_anchors[0]))

static void check_tick_order(void)
{
    TEST_SECTION("virtual_simfw device_tick() reaches the same structural points, in the same order, "
                 "as sim_engine.c's sim_engine_tick()");

    static const char *real_candidates[] = {
        "../src/tasks/sim_engine.c",
        "src/tasks/sim_engine.c",
        "firmware/SimFW/src/tasks/sim_engine.c",
    };
    static const char *port_candidates[] = {
        "../tools/virtual_simfw/src/virtual_simfw.c",
        "tools/virtual_simfw/src/virtual_simfw.c",
        "firmware/SimFW/tools/virtual_simfw/src/virtual_simfw.c",
    };

    char *real_text = read_file_any(real_candidates, 3);
    char *port_text = read_file_any(port_candidates, 3);
    if (!real_text || !port_text) {
        TEST_CHECK(false, "could not locate sim_engine.c and/or virtual_simfw.c from the host test's "
                           "working directory -- update the candidate paths in "
                           "test_virtual_simfw_port_drift_coverage.c if the build layout moved");
        free(real_text);
        free(port_text);
        return;
    }

    // Comment-stripping matters here too: sim_engine.c's own 9-step
    // tick-order comment and device_tick()'s K4-gating comment both narrate
    // this exact ordering in prose (that's *why* these anchors were picked --
    // the authors already flagged this order as significant) -- a naive
    // search could pass on the narration even if the real code around it
    // moved.
    strip_comments(real_text);
    strip_comments(port_text);

    size_t real_body_len = 0;
    const char *real_body = find_function_body(real_text, "static void sim_engine_tick(void)", &real_body_len);
    TEST_CHECK(real_body != NULL, "sim_engine_tick() function body not found in sim_engine.c's "
                                   "(comment-stripped) source text -- it may have been renamed/restructured; "
                                   "update this test's signature string if so");

    size_t port_body_len = 0;
    const char *port_body = find_function_body(port_text, "static void device_tick(device_t *d)", &port_body_len);
    TEST_CHECK(port_body != NULL, "device_tick() function body not found in virtual_simfw.c's "
                                   "(comment-stripped) source text -- it may have been renamed/restructured; "
                                   "update this test's signature string if so");

    if (!real_body || !port_body) {
        free(real_text);
        free(port_text);
        return;
    }

    const char *real_anchors[K_TICK_ANCHORS_COUNT];
    const char *port_anchors[K_TICK_ANCHORS_COUNT];
    for (size_t i = 0; i < K_TICK_ANCHORS_COUNT; i++) {
        real_anchors[i] = k_tick_anchors[i].real_anchor;
        port_anchors[i] = k_tick_anchors[i].port_anchor;
    }

    size_t fail_index = 0;
    bool real_ok = anchors_in_order(real_body, real_body_len, real_anchors, K_TICK_ANCHORS_COUNT, &fail_index);
    if (!real_ok) {
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "sim_engine.c's sim_engine_tick() no longer reaches anchor #%zu (%s) after the prior "
                 "anchors, in order -- this test's own reference order is now stale, update "
                 "k_tick_anchors[] in test_virtual_simfw_port_drift_coverage.c to match the real tick order",
                 fail_index, k_tick_anchors[fail_index].desc);
        TEST_CHECK(false, msg);
    } else {
        TEST_CHECK(true, "sim_engine_tick() anchor order matches k_tick_anchors[]");
    }

    bool port_ok = anchors_in_order(port_body, port_body_len, port_anchors, K_TICK_ANCHORS_COUNT, &fail_index);
    if (!port_ok) {
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "virtual_simfw.c's device_tick() no longer reaches anchor #%zu (%s) after the prior "
                 "anchors, in order -- the port has drifted from sim_engine_tick()'s real tick order",
                 fail_index, k_tick_anchors[fail_index].desc);
        TEST_CHECK(false, msg);
    } else {
        TEST_CHECK(true, "device_tick() anchor order matches k_tick_anchors[]");
    }

    free(real_text);
    free(port_text);
}

void run_test_virtual_simfw_port_drift_coverage(void)
{
    check_enum_order();
    check_switch_case_coverage();
    check_tick_order();
}
