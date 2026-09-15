// Host tests for App/drivers/persist/kiln_package.c -- the Pico-half
// packaging and package-identity hash (docs/KILN_PROFILES_PLAN.md items
// 1/2/12).
//
// Own SEPARATE executable (same convention test_safety_cfg_http.c's own
// header comment documents): kiln_package.c is #included directly here so
// this file can drive kiln_package_capture_pico_half() against a FAKE
// kiln_pkg_pico_source_t built from file-local functions matching safety_
// cfg_store_param_count()/_get_by_index()'s exact signatures -- the whole
// point of that injected accessor (see kiln_package.h's own header comment)
// is that this module's logic is testable with NO dependency on the real
// safety_cfg_store.c, which needs its own NVS/UART/FreeRTOS stub surface
// (test_safety_cfg_store.c, linked into the MAIN executable instead). This
// file never links the real safety_cfg_store.c, so it defines its own
// fake safety_cfg_param_t rows directly -- no multiple-definition risk with
// the main executable's real ones.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "esp_crc.h" /* the real esp_crc32_le() host stub -- same one kiln_package.c itself
                       * includes; used here so a test can independently recompute the SAME
                       * CRC over a hand-built canonical buffer to prove kiln_package_compute_
                       * hash() actually matches the documented byte layout, not just that it
                       * is internally self-consistent. */

#include "../drivers/persist/kiln_package.c"

// kiln_pkg_pico_source_default() (production convenience, unused by these
// tests -- they always inject fake_source() below) still references
// safety_cfg_store_param_count()/_get_by_index() by name, and MSVC links a
// whole .obj's external references regardless of which functions inside it
// are actually called -- so this file needs SOME definition of the two to
// link, even though nothing here ever calls kiln_pkg_pico_source_default()
// itself. Bodies that abort if ever actually invoked -- a real call would
// mean a test started depending on the production accessor by mistake. */
size_t safety_cfg_store_param_count(void)
{
    fprintf(stderr, "test_kiln_package.c: safety_cfg_store_param_count() unexpectedly called -- "
                    "this test file never uses the production accessor\n");
    abort();
}

bool safety_cfg_store_get_by_index(size_t index, safety_cfg_param_t *out)
{
    (void)index;
    (void)out;
    fprintf(stderr, "test_kiln_package.c: safety_cfg_store_get_by_index() unexpectedly called -- "
                    "this test file never uses the production accessor\n");
    abort();
}

// ---------------------------------------------------------------------------
// Fake Pico param table -- fully under test control, no real safety_cfg_
// store.c involved.
// ---------------------------------------------------------------------------

#define FAKE_TABLE_MAX 100
static safety_cfg_param_t s_fake_table[FAKE_TABLE_MAX];
static size_t s_fake_table_count = 0;
static bool s_fake_get_by_index_should_fail_at = false;
static size_t s_fake_fail_index = 0;

static size_t fake_param_count(void)
{
    return s_fake_table_count;
}

static bool fake_get_by_index(size_t index, safety_cfg_param_t *out)
{
    if (s_fake_get_by_index_should_fail_at && index == s_fake_fail_index) {
        return false;
    }
    if (index >= s_fake_table_count) {
        return false;
    }
    *out = s_fake_table[index];
    return true;
}

static void reset_fake_table(void)
{
    memset(s_fake_table, 0, sizeof(s_fake_table));
    s_fake_table_count = 0;
    s_fake_get_by_index_should_fail_at = false;
    s_fake_fail_index = 0;
}

/* Appends one row to the fake table, table order deliberately NOT sorted --
 * every test that populates more than one row scrambles param_id order so a
 * pass proves kiln_package_capture_pico_half() sorts, rather than merely
 * preserving an already-sorted input. */
static void fake_add(uint16_t param_id, uint8_t type, bool set, kilnlink_param_value_t value)
{
    safety_cfg_param_t *row = &s_fake_table[s_fake_table_count++];
    row->param_id = param_id;
    row->name = "fake";
    row->type = type;
    row->set = set;
    row->value = value;
}

static kiln_pkg_pico_source_t fake_source(void)
{
    kiln_pkg_pico_source_t src;
    src.param_count = fake_param_count;
    src.get_by_index = fake_get_by_index;
    return src;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_capture_sorts_ascending_by_param_id(void)
{
    TEST_SECTION("kiln_package_capture_pico_half -- sorts ascending by param_id, table order ignored");
    reset_fake_table();

    kilnlink_param_value_t v;
    v.f32_val = 1300.0f;
    fake_add(0x0301, KILNLINK_PARAM_TYPE_F32, true, v);
    v.u8_val = 3;
    fake_add(0x0101, KILNLINK_PARAM_TYPE_U8, true, v);
    v.u16_val = 42;
    fake_add(0x0204, KILNLINK_PARAM_TYPE_U16, true, v);

    kiln_pkg_safety_t pkg;
    kiln_pkg_pico_source_t src = fake_source();
    bool ok = kiln_package_capture_pico_half(&src, &pkg);
    TEST_CHECK(ok, "capture succeeds");
    TEST_CHECK(pkg.count == 3, "all three rows captured");
    TEST_CHECK(pkg.entries[0].param_id == 0x0101, "sorted: 0x0101 first");
    TEST_CHECK(pkg.entries[1].param_id == 0x0204, "sorted: 0x0204 second");
    TEST_CHECK(pkg.entries[2].param_id == 0x0301, "sorted: 0x0301 third");
}

static void test_capture_packages_unset_rows_never_omits(void)
{
    TEST_SECTION("kiln_package_capture_pico_half -- an unset row is packaged with flags=0, never omitted");
    reset_fake_table();

    kilnlink_param_value_t zero;
    memset(&zero, 0, sizeof(zero));
    fake_add(0x0102, KILNLINK_PARAM_TYPE_F32, false, zero); // never fetched/commissioned

    kiln_pkg_safety_t pkg;
    kiln_pkg_pico_source_t src = fake_source();
    TEST_CHECK(kiln_package_capture_pico_half(&src, &pkg), "capture succeeds even with an unset row");
    TEST_CHECK(pkg.count == 1, "the unset row is still represented, not skipped");
    TEST_CHECK((pkg.entries[0].flags & KILN_PKG_PARAM_FLAG_SET) == 0, "flags report NOT set");
    TEST_CHECK(pkg.entries[0].value_bits == 0, "value_bits is 0 for an unset row, never a fabricated value");
}

static void test_capture_f32_bits_round_trip_exactly(void)
{
    TEST_SECTION("kiln_package_capture_pico_half -- an f32 param's value_bits is the exact IEEE-754 bit pattern");
    reset_fake_table();

    kilnlink_param_value_t v;
    v.f32_val = 123.456f;
    fake_add(0x0105, KILNLINK_PARAM_TYPE_F32, true, v);

    kiln_pkg_safety_t pkg;
    kiln_pkg_pico_source_t src = fake_source();
    TEST_CHECK(kiln_package_capture_pico_half(&src, &pkg), "capture succeeds");

    union { float f; uint32_t bits; } conv;
    conv.f = 123.456f;
    TEST_CHECK(pkg.entries[0].value_bits == conv.bits, "value_bits matches the float's own bit pattern exactly");
}

static void test_capture_too_many_rows_refuses_never_truncates(void)
{
    TEST_SECTION("kiln_package_capture_pico_half -- more rows than KILN_PKG_SAFETY_PARAM_CAP refuses, never truncates");
    reset_fake_table();
    kilnlink_param_value_t v;
    v.u8_val = 1;
    for (uint16_t i = 0; i < KILN_PKG_SAFETY_PARAM_CAP + 1 && s_fake_table_count < FAKE_TABLE_MAX; i++) {
        fake_add((uint16_t)(0x1000 + i), KILNLINK_PARAM_TYPE_U8, true, v);
    }
    kiln_pkg_safety_t pkg;
    memset(&pkg, 0xAA, sizeof(pkg)); // poison, so a false "success leaving stale bytes" would be visible
    kiln_pkg_pico_source_t src = fake_source();
    bool ok = kiln_package_capture_pico_half(&src, &pkg);
    TEST_CHECK(!ok, "refuses rather than truncating when the table exceeds capacity");
    TEST_CHECK(pkg.count == 0, "out is zeroed on refusal, never left holding a partial/poisoned capture");
}

static void test_capture_get_by_index_failure_refuses_whole_capture(void)
{
    TEST_SECTION("kiln_package_capture_pico_half -- a mid-walk read failure refuses the WHOLE capture, not a hole");
    reset_fake_table();
    kilnlink_param_value_t v;
    v.u8_val = 1;
    fake_add(0x0101, KILNLINK_PARAM_TYPE_U8, true, v);
    fake_add(0x0102, KILNLINK_PARAM_TYPE_U8, true, v);
    fake_add(0x0103, KILNLINK_PARAM_TYPE_U8, true, v);
    s_fake_get_by_index_should_fail_at = true;
    s_fake_fail_index = 1;

    kiln_pkg_safety_t pkg;
    kiln_pkg_pico_source_t src = fake_source();
    bool ok = kiln_package_capture_pico_half(&src, &pkg);
    TEST_CHECK(!ok, "refuses on a mid-walk read failure");
    TEST_CHECK(pkg.count == 0, "out is zeroed -- never a package missing just the one failed row");
}

static void test_hash_changes_with_any_pico_field(void)
{
    TEST_SECTION("kiln_package_compute_hash -- changes when ANY packaged field changes, only then");
    reset_fake_table();
    kilnlink_param_value_t v;
    v.f32_val = 1300.0f;
    fake_add(0x0101, KILNLINK_PARAM_TYPE_F32, true, v);

    kiln_pkg_safety_t pkg_a;
    kiln_pkg_pico_source_t src = fake_source();
    TEST_CHECK(kiln_package_capture_pico_half(&src, &pkg_a), "capture A succeeds");

    uint8_t esp_blob[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t hash_a = 0;
    TEST_CHECK(kiln_package_compute_hash(1, esp_blob, sizeof(esp_blob), &pkg_a, &hash_a), "hash A computed");

    // Identical recompute -- same hash.
    uint32_t hash_a2 = 0;
    TEST_CHECK(kiln_package_compute_hash(1, esp_blob, sizeof(esp_blob), &pkg_a, &hash_a2) && hash_a2 == hash_a,
               "recomputing over the identical package gives the identical hash");

    // Change one ESP byte -- hash must change.
    uint8_t esp_blob_b[8] = {1, 2, 3, 4, 5, 6, 7, 9};
    uint32_t hash_b = 0;
    TEST_CHECK(kiln_package_compute_hash(1, esp_blob_b, sizeof(esp_blob_b), &pkg_a, &hash_b) && hash_b != hash_a,
               "changing one ESP blob byte changes the hash");

    // Change the pico value by one ULP -- hash must change.
    reset_fake_table();
    v.f32_val = 1300.0001f; // one ULP+ away at this magnitude, distinguishable
    fake_add(0x0101, KILNLINK_PARAM_TYPE_F32, true, v);
    kiln_pkg_safety_t pkg_c;
    kiln_pkg_pico_source_t src_c = fake_source();
    TEST_CHECK(kiln_package_capture_pico_half(&src_c, &pkg_c), "capture C succeeds");
    uint32_t hash_c = 0;
    TEST_CHECK(kiln_package_compute_hash(1, esp_blob, sizeof(esp_blob), &pkg_c, &hash_c) && hash_c != hash_a,
               "changing a single Pico float value changes the hash");

    // Change pkg_schema alone -- hash must change (schema is part of the canonical input).
    uint32_t hash_d = 0;
    TEST_CHECK(kiln_package_compute_hash(2, esp_blob, sizeof(esp_blob), &pkg_a, &hash_d) && hash_d != hash_a,
               "changing pkg_schema alone changes the hash");
}

static void test_hash_reproduces_documented_canonical_layout(void)
{
    TEST_SECTION("kiln_package_compute_hash -- matches an independently hand-built canonical buffer");
    reset_fake_table();
    kilnlink_param_value_t v;
    v.u16_val = 111;
    fake_add(0x0204, KILNLINK_PARAM_TYPE_U16, true, v);

    kiln_pkg_safety_t pkg;
    kiln_pkg_pico_source_t src = fake_source();
    TEST_CHECK(kiln_package_capture_pico_half(&src, &pkg), "capture succeeds");

    uint8_t esp_blob[3] = {0xAA, 0xBB, 0xCC};
    uint32_t hash = 0;
    TEST_CHECK(kiln_package_compute_hash(7, esp_blob, sizeof(esp_blob), &pkg, &hash), "hash computed");

    // Hand-build the EXACT canonical buffer per kiln_package.h's documented
    // layout and confirm the two CRCs agree -- this is what proves the
    // function actually implements the documented byte layout, not merely
    // "some" self-consistent hash.
    uint8_t expect[2 + 2 + 3 + 2 + 8];
    size_t off = 0;
    expect[off++] = 7;
    expect[off++] = 0;
    expect[off++] = 3;
    expect[off++] = 0;
    expect[off++] = 0xAA;
    expect[off++] = 0xBB;
    expect[off++] = 0xCC;
    expect[off++] = 1; // pico->count == 1
    expect[off++] = 0;
    expect[off++] = 0x04; // param_id 0x0204 LE
    expect[off++] = 0x02;
    expect[off++] = KILNLINK_PARAM_TYPE_U16;
    expect[off++] = KILN_PKG_PARAM_FLAG_SET;
    expect[off++] = 111; // value_bits LE (0x0000006F)
    expect[off++] = 0;
    expect[off++] = 0;
    expect[off++] = 0;
    uint32_t expect_crc = esp_crc32_le(0, expect, (uint32_t)off);
    TEST_CHECK(hash == expect_crc, "computed hash matches the hand-built canonical buffer's CRC exactly");
}

static void test_hash_rejects_oversized_input_never_truncated_hash(void)
{
    TEST_SECTION("kiln_package_compute_hash -- refuses rather than hashing a truncated buffer");
    kiln_pkg_safety_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    pkg.count = 1; // one real entry, but we lie about esp_blob_len below to force an overflow
    // esp_blob_len itself cannot exceed the uint16_t range, but the check
    // (needed > KILN_PKG_HASH_SCRATCH_CAP) is exercised directly by asking
    // for the theoretical max -- this proves the guard exists and is not
    // vacuous (KILN_PKG_HASH_SCRATCH_CAP is 4096; 65535 + header cannot fit).
    uint8_t *big = (uint8_t *)malloc(65535);
    TEST_CHECK(big != NULL, "test scratch alloc succeeds");
    memset(big, 0, 65535);
    uint32_t crc = 0xDEADBEEFu; // poison -- must be left untouched on refusal, per the H2 fix's own contract
    bool ok = kiln_package_compute_hash(1, big, 65535, &pkg, &crc);
    TEST_CHECK(!ok, "refuses when the canonical buffer would not fit the scratch cap");
    TEST_CHECK(crc == 0xDEADBEEFu, "out_hash is left untouched on failure, never zeroed or garbage-written");
    free(big);
}

int main(void)
{
    test_capture_sorts_ascending_by_param_id();
    test_capture_packages_unset_rows_never_omits();
    test_capture_f32_bits_round_trip_exactly();
    test_capture_too_many_rows_refuses_never_truncates();
    test_capture_get_by_index_failure_refuses_whole_capture();
    test_hash_changes_with_any_pico_field();
    test_hash_reproduces_documented_canonical_layout();
    test_hash_rejects_oversized_input_never_truncated_hash();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
