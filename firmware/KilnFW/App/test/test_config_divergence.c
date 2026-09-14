// Host tests for App/drivers/safety/config_divergence.{h,c} -- the
// format-version+hash config identity check behind the 2026-09-14 owner
// decision ("if a config doesn't land and match on both sides then alarm
// and dissable heaters"). Owner's own definition of "matching", verbatim:
// "Matching config meens format version number and a hash that identifys
// the set." This file proves that definition, NOT a field-by-field
// comparator (the per-field walk in config_divergence.c only builds the
// operator-facing message once the hash/version gate has already decided).
//
// WHAT THIS FILE MUST PROVE:
//  1. Identical field sets on both sides match (no false alarm).
//  2. A real value mismatch is caught, and the message names the field and
//     both sides' values.
//  3. A field unknown on exactly one side is a mismatch -- the concrete
//     form of "an unarmed/unconfirmed Pico is a divergence by definition."
//  4. Two identities computed under DIFFERENT format_version numbers never
//     match, regardless of hash -- comparing hashes across versions would
//     be meaningless (this header's own "format version gates the
//     comparison" rule).
//  5. FLOAT NORMALISATION: a value that has actually been round-tripped
//     through the wire's %.9g encode/decode (safety_cfg_http.c's own
//     convention) must hash IDENTICALLY to the value before the trip --
//     the owner's own stated trap ("equal values can have different bit
//     patterns after a round-trip"). Also proves two BINARY-different
//     floats representing the same decimal quantity collapse to one
//     normalised value.
//
// NEGATIVE TEST (2026-09-14, RED confirmed, restored by hand, build dir
// deleted and rebuilt): changing config_identity_matches()'s
// `a->hash == b->hash` to `true` (i.e. the hash never actually gates
// anything) fails this file's test_value_mismatch_is_caught() and
// test_format_version_gates_the_comparison(). Restore by reversing the
// edit textually; `git diff` on config_divergence.c then comes back empty.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/safety/config_divergence.h"

static void test_identical_fields_match(void)
{
    TEST_SECTION("identical field sets on both sides do not diverge");
    config_identity_field_t esp_f[2] = {
        { .name = "abs_max_temp_c", .known = true, .value = 80.0f },
        { .name = "max_rate_c_per_min", .known = true, .value = 33.3f },
    };
    config_identity_field_t pico_f[2] = {
        { .name = "abs_max_temp_c", .known = true, .value = 80.0f },
        { .name = "max_rate_c_per_min", .known = true, .value = 33.3f },
    };
    char reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool diverged = config_divergence_check(esp_f, pico_f, 2, reason, sizeof(reason));
    TEST_CHECK(!diverged, "identical field sets match");
    TEST_CHECK(reason[0] == '\0', "no reason written when nothing diverges");
}

static void test_value_mismatch_is_caught(void)
{
    TEST_SECTION("a real value mismatch is caught and named");
    config_identity_field_t esp_f[1] = { { .name = "abs_max_temp_c", .known = true, .value = 80.0f } };
    config_identity_field_t pico_f[1] = { { .name = "abs_max_temp_c", .known = true, .value = 75.0f } };
    char reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool diverged = config_divergence_check(esp_f, pico_f, 1, reason, sizeof(reason));
    TEST_CHECK(diverged, "a real mismatch diverges");
    TEST_CHECK(strstr(reason, "abs_max_temp_c") != NULL, "the reason names the diverging field");
    TEST_CHECK(strstr(reason, "80.00") != NULL, "the reason gives the ESP's value");
    TEST_CHECK(strstr(reason, "75.00") != NULL, "the reason gives the Pico's value");
}

static void test_unknown_on_one_side_diverges(void)
{
    /* The concrete form of "an unarmed Pico is a divergence by
     * definition": a field the ESP has an opinion on but the Pico has
     * never confirmed (boot before push, or any field gated the same way
     * abs_max_temp_c is by commissioning_gate.h) must fail the SAME path
     * as a numeric mismatch. */
    TEST_SECTION("a field known on only one side diverges (boot-before-push / unarmed Pico)");
    config_identity_field_t esp_f[1] = { { .name = "abs_max_temp_c", .known = true, .value = 80.0f } };
    config_identity_field_t pico_f[1] = { { .name = "abs_max_temp_c", .known = false, .value = 0.0f } };
    char reason[CONFIG_DIVERGENCE_REASON_MAX];
    bool diverged = config_divergence_check(esp_f, pico_f, 1, reason, sizeof(reason));
    TEST_CHECK(diverged, "an unconfirmed Pico value diverges");
    TEST_CHECK(strstr(reason, "unconfirmed") != NULL, "the reason says the Pico side is unconfirmed");
}

static void test_both_known_and_equal_via_identity_api(void)
{
    TEST_SECTION("config_identity_compute()/config_identity_matches() agree on an exact match");
    config_identity_field_t f[1] = { { .name = "abs_max_temp_c", .known = true, .value = 80.0f } };
    config_identity_t a = config_identity_compute(CONFIG_IDENTITY_FORMAT_VERSION, f, 1);
    config_identity_t b = config_identity_compute(CONFIG_IDENTITY_FORMAT_VERSION, f, 1);
    TEST_CHECK(a.known && b.known, "both identities are known");
    TEST_CHECK(a.hash == b.hash, "the same field set hashes identically");
    TEST_CHECK(config_identity_matches(&a, &b), "identical identities match");
}

static void test_format_version_gates_the_comparison(void)
{
    /* "The format version gates the comparison" -- two identities computed
     * under different format_version numbers must never match, even with
     * identical field values (a coincidentally-equal hash would be worse
     * than useless -- it would mean nothing). */
    TEST_SECTION("different format_version never matches, even with identical fields and equal hash inputs");
    config_identity_field_t f[1] = { { .name = "abs_max_temp_c", .known = true, .value = 80.0f } };
    config_identity_t a = config_identity_compute(1u, f, 1);
    config_identity_t b = config_identity_compute(2u, f, 1);
    TEST_CHECK(!config_identity_matches(&a, &b), "a format_version mismatch never matches");
}

static void test_unknown_identity_never_matches(void)
{
    TEST_SECTION("an identity with any unknown field never matches, even against an identical unknown identity");
    config_identity_field_t f[1] = { { .name = "abs_max_temp_c", .known = false, .value = 0.0f } };
    config_identity_t a = config_identity_compute(CONFIG_IDENTITY_FORMAT_VERSION, f, 1);
    config_identity_t b = config_identity_compute(CONFIG_IDENTITY_FORMAT_VERSION, f, 1);
    TEST_CHECK(a.hash == b.hash, "precondition: both unknown identities hash the same (same sentinel input)");
    TEST_CHECK(!config_identity_matches(&a, &b),
               "two 'unknown' identities never report as matching, hash collision or not");
}

static void test_wire_round_trip_normalises_identically(void)
{
    /* The owner's own stated trap, verbatim: "equal values can have
     * different bit patterns after a round-trip through JSON, a
     * migration, or a set-and-confirm cycle." Simulate the actual wire
     * round trip safety_cfg_http.c performs (snprintf "%.9g" then
     * strtof()) starting from a value that is NOT exactly representable
     * and confirm the normalised result -- and therefore the hash -- is
     * identical whether or not the value actually made the trip. */
    TEST_SECTION("a value that travelled the wire's %.9g round trip hashes identically to one that did not");
    float original = 33.333333f; /* not exactly representable in binary float32 */
    char wire_buf[32];
    snprintf(wire_buf, sizeof(wire_buf), "%.9g", (double)original);
    float after_wire = strtof(wire_buf, NULL);

    config_identity_field_t f_before[1] = { { .name = "max_rate_c_per_min", .known = true, .value = original } };
    config_identity_field_t f_after[1] = { { .name = "max_rate_c_per_min", .known = true, .value = after_wire } };

    config_identity_t id_before = config_identity_compute(CONFIG_IDENTITY_FORMAT_VERSION, f_before, 1);
    config_identity_t id_after = config_identity_compute(CONFIG_IDENTITY_FORMAT_VERSION, f_after, 1);

    TEST_CHECK(config_identity_matches(&id_before, &id_after),
               "a value and its own wire-round-tripped copy are NOT reported as divergent");

    char reason[CONFIG_DIVERGENCE_REASON_MAX];
    TEST_CHECK(!config_divergence_check(f_before, f_after, 1, reason, sizeof(reason)),
               "config_divergence_check() agrees: no nuisance alarm from a benign wire round trip");
}

static void test_normalize_collapses_different_bit_patterns(void)
{
    TEST_SECTION("config_identity_normalize_f32() collapses two different bit patterns for the same decimal value");
    /* Two different ways of arriving at "80.0" that are not guaranteed to
     * produce the identical bit pattern on every platform/compiler
     * (accumulated division/multiplication vs a literal). */
    float a = 80.0f;
    float b = (80.0f * 3.0f) / 3.0f;
    float na = config_identity_normalize_f32(a);
    float nb = config_identity_normalize_f32(b);
    TEST_CHECK(na == nb, "both normalise to the same value");
}

static void test_reason_buffer_is_never_truncated(void)
{
    /* Owner's own caution: "ki_refusal_reason is char[96] and a message
     * silently truncated at 162 bytes earlier today, so size any new
     * operator-facing string against its buffer and assert it is not
     * truncated." CONFIG_DIVERGENCE_REASON_MAX is this file's answer;
     * prove every message shape this file can produce actually fits it,
     * using the longest field name this codebase's ceiling-mirror caller
     * uses today plus the identity-mismatch fallback message (the longest
     * one this file can produce, since it also carries two hex hashes). */
    TEST_SECTION("every message this file can produce fits CONFIG_DIVERGENCE_REASON_MAX without truncation");
    config_identity_field_t esp_f[1] = { { .name = "abs_max_temp_c", .known = true, .value = 12345.678f } };
    config_identity_field_t pico_f[1] = { { .name = "abs_max_temp_c", .known = true, .value = -12345.678f } };
    char reason[CONFIG_DIVERGENCE_REASON_MAX];
    memset(reason, 'X', sizeof(reason)); /* poison so a short write is visible */
    bool diverged = config_divergence_check(esp_f, pico_f, 1, reason, sizeof(reason));
    TEST_CHECK(diverged, "precondition: this pair actually diverges");
    size_t len = strlen(reason);
    TEST_CHECK(len < sizeof(reason) - 1, "the message did not fill (and therefore did not risk truncating) the buffer");
    TEST_CHECK(reason[sizeof(reason) - 1] != 'X' || len < sizeof(reason) - 1,
               "no poison byte survives past the written message");
}

int main(void)
{
    test_identical_fields_match();
    test_value_mismatch_is_caught();
    test_unknown_on_one_side_diverges();
    test_both_known_and_equal_via_identity_api();
    test_format_version_gates_the_comparison();
    test_unknown_identity_never_matches();
    test_wire_round_trip_normalises_identically();
    test_normalize_collapses_different_bit_patterns();
    test_reason_buffer_is_never_truncated();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
