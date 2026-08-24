// test_i2c_owner_bus_scan_probe_len_coverage.c -- pins the REAL source text
// of i2c_owner.c's perform_bus_scan(), not the hand-maintained mirror in
// test_i2c_owner_bus_scan.c.
//
// THE GAP this test closes: test_i2c_owner_bus_scan.c's own header comment
// says it tests mirror_perform_bus_scan(), "a byte-for-byte copy of
// i2c_owner.c's perform_bus_scan() bit-index math ... Keep the two in sync
// by hand if either changes." That mirror is necessary -- i2c_owner.c
// includes pico-sdk (hardware/i2c.h) and FreeRTOS headers this host-test
// harness cannot link -- but it also means nothing in the existing suite
// actually reads the real call site. An edit changing the real
// i2c_write_blocking(I2C_OWNER_I2C_PORT, addr, &probe_byte, 1u, false)'s
// `1u` to `0` (or `0u`) would leave the mirror, and every test built on it,
// completely unaffected: it would ship uncaught, reproducing the exact
// pico-sdk invalid_params_if(I2C, len == 0) release-build no-op bug
// documented in both i2c_owner.c's CRITICAL comment above that call and
// test_i2c_owner_bus_scan.c's own header comment.
//
// This is deliberately a source-text scan of the real file, following the
// precedent in firmware/SaftyFW/test/test_boot_checkin_coverage.c and
// test_watchdog_budget_coverage.c for exactly this situation (a real source
// file the host harness cannot link): read the file, strip comments, and
// search the non-comment text for the property under test.
//
// Comment-stripping is not optional here either. i2c_owner.c's own CRITICAL
// comment immediately above the call spells out "1u" and "0-byte write" and
// "len == 1" in prose -- a naive (non-comment-stripped) substring search for
// "1u" would pass on that comment alone even if the real call's length
// argument were changed to 0u, making the guard structurally unfailable.
// Comments are stripped first so only the actual call-site argument can
// satisfy the check.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "test_common.h"

// Reads a whole file into a NUL-terminated malloc'd buffer, or NULL if it
// can't be found. Tries a few relative-path candidates because
// build_host_tests.ps1 and a plain `cl`/IDE invocation can each have a
// different working directory -- copied from
// test_boot_checkin_coverage.c's read_file_any().
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

// Strips // line comments and /* */ block comments in place. Copied from
// test_boot_checkin_coverage.c's strip_comments() -- see that file's header
// comment for why this doesn't need to be string/char-literal-aware to be
// correct on this codebase's source.
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

// Isolates perform_bus_scan()'s function body out of the (already
// comment-stripped) source text, by finding the "static void
// perform_bus_scan(" signature and then brace-matching from the first '{'
// after it. Scoping the scan to just this function (rather than searching
// the whole file) is what lets the "exactly one probe call" check below be
// meaningful, and keeps the check from being satisfied by some unrelated
// i2c_write_blocking() call elsewhere in the file.
static const char *find_function_body(const char *stripped_text, const char *signature,
                                       size_t *out_len)
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
                p++; // include the closing brace
                break;
            }
        }
    }
    if (depth != 0) {
        return NULL; // unbalanced braces -- malformed scan, don't guess
    }
    *out_len = (size_t)(p - brace);
    return brace;
}

// Counts occurrences of "i2c_write_blocking(" within [text, text+len), and
// reports whether any of them use a probe length of the "obvious" spellings
// of 1 (1, 1u, 1U, 1l, 1L and combinations) as the 4th call argument, versus
// any spelling of 0 (0, 0u, 0U, ...). This only needs to understand this
// codebase's one real call shape --
// i2c_write_blocking(I2C_OWNER_I2C_PORT, addr, &probe_byte, 1u, false) --
// not general C expression parsing.
typedef struct {
    int call_count;
    bool any_len_is_one;
    bool any_len_is_zero;
} probe_len_scan_t;

static bool token_is_integer_literal(const char *tok, size_t tok_len, long expect_value)
{
    // Strip trailing u/U/l/L suffixes, then parse the remaining digits.
    while (tok_len > 0 && strchr("uUlL", tok[tok_len - 1]) != NULL) {
        tok_len--;
    }
    if (tok_len == 0) {
        return false;
    }
    char buf[32];
    if (tok_len >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, tok, tok_len);
    buf[tok_len] = '\0';
    for (size_t i = 0; i < tok_len; i++) {
        if (!isdigit((unsigned char)buf[i])) {
            return false;
        }
    }
    return atol(buf) == expect_value;
}

static probe_len_scan_t scan_probe_calls(const char *text, size_t len)
{
    probe_len_scan_t result = {0, false, false};
    const char *needle = "i2c_write_blocking(";
    const char *end = text + len;

    for (const char *p = text; p < end;) {
        const char *hit = strstr(p, needle);
        if (!hit || hit >= end) {
            break;
        }
        result.call_count++;

        // Walk the argument list, splitting on top-level commas (depth 0
        // relative to this call's own parens), to isolate the 4th argument
        // (the length). Index: 0=port, 1=addr, 2=buf, 3=len, 4=nostop.
        const char *args = hit + strlen(needle);
        int paren_depth = 0;
        int arg_index = 0;
        const char *arg_start = args;
        const char *len_arg = NULL;
        size_t len_arg_len = 0;
        const char *q = args;
        for (; *q; q++) {
            if (*q == '(') {
                paren_depth++;
            } else if (*q == ')') {
                if (paren_depth == 0) {
                    // end of this call's argument list
                    if (arg_index == 3) {
                        len_arg = arg_start;
                        len_arg_len = (size_t)(q - arg_start);
                    }
                    q++;
                    break;
                }
                paren_depth--;
            } else if (*q == ',' && paren_depth == 0) {
                if (arg_index == 3) {
                    len_arg = arg_start;
                    len_arg_len = (size_t)(q - arg_start);
                }
                arg_index++;
                arg_start = q + 1;
            }
        }

        if (len_arg) {
            // Trim surrounding whitespace.
            while (len_arg_len > 0 && isspace((unsigned char)*len_arg)) {
                len_arg++;
                len_arg_len--;
            }
            while (len_arg_len > 0 && isspace((unsigned char)len_arg[len_arg_len - 1])) {
                len_arg_len--;
            }
            if (token_is_integer_literal(len_arg, len_arg_len, 1)) {
                result.any_len_is_one = true;
            }
            if (token_is_integer_literal(len_arg, len_arg_len, 0)) {
                result.any_len_is_zero = true;
            }
        }

        p = hit + strlen(needle);
    }

    return result;
}

void run_test_i2c_owner_bus_scan_probe_len_coverage(void)
{
    TEST_SECTION("i2c_owner bus-scan probe length coverage (real source, not the mirror)");

    static const char *source_candidates[] = {
        "../src/tasks/i2c_owner.c",
        "src/tasks/i2c_owner.c",
        "firmware/SimFW/src/tasks/i2c_owner.c",
    };

    char *source_text = read_file_any(source_candidates, 3);
    if (!source_text) {
        TEST_CHECK(false, "could not locate i2c_owner.c from the host test's working directory -- "
                           "update the candidate paths in "
                           "test_i2c_owner_bus_scan_probe_len_coverage.c if the build layout moved");
        return;
    }

    // Comments stripped BEFORE searching -- see this file's header comment
    // for why: i2c_owner.c's own CRITICAL comment above the call spells out
    // "1u" and "len == 1" in prose, so an unstripped search could pass on
    // the comment alone even with a regressed call site.
    strip_comments(source_text);

    size_t body_len = 0;
    const char *body = find_function_body(source_text, "static void perform_bus_scan(", &body_len);

    // Assert the target symbol was actually found before asserting anything
    // about its content -- a scan that silently finds nothing must FAIL,
    // not vacuously pass, per this repo's standing rule against
    // structurally-unfailable checks.
    TEST_CHECK(body != NULL, "perform_bus_scan() function body was not found in i2c_owner.c's "
                              "(comment-stripped) source text -- the function may have been "
                              "renamed or restructured; update this test's signature string if so");
    if (!body) {
        free(source_text);
        return;
    }

    probe_len_scan_t scan = scan_probe_calls(body, body_len);

    TEST_CHECK(scan.call_count == 1,
               "expected exactly one i2c_write_blocking(...) probe call inside "
               "perform_bus_scan()'s body in the real (non-comment) source text");

    TEST_CHECK(scan.any_len_is_one,
               "perform_bus_scan()'s i2c_write_blocking() call must pass a length of 1 (1u/1/1U/...) "
               "as its 4th argument in the real, non-comment source text -- a 0-length write is a "
               "pico-sdk invalid_params_if(I2C, len == 0) release-build no-op: the address phase is "
               "never driven and every address falsely reports present (the confirmed bug this "
               "command exists to have never shipped)");

    TEST_CHECK(!scan.any_len_is_zero,
               "perform_bus_scan()'s i2c_write_blocking() call must NOT pass a length of 0 (0u/0/0U/...)");

    free(source_text);
}
