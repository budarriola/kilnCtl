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
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

// ---------------------------------------------------------------------------
// Section 5.1 envelope -- kiln_package_export_json()/_import_json()
// (docs/KILN_PROFILES_PLAN.md items 3/4/9/14, 2026-09-14 upload/download
// follow-up). These three refusal-reason tests are the ones the dispatch
// prompt asked for explicitly: malformed, unknown-newer version, hash
// mismatch.
// ---------------------------------------------------------------------------

// Fixed stand-in board id for tests that don't care about the board-identity
// path specifically (round trip, malformed-kind, newer-schema, hash-tamper)
// -- kiln_board_identity.c lives in kiln_cfg_store.c's test executable, not
// this one (this file never includes it), so this module has no board-id
// concept of its own; a plain constant plays that role here.
#define TEST_SOME_BOARD_ID 0x11223344u

static bool build_sample_package(char *json_out, size_t json_cap, uint32_t *out_hash)
{
    reset_fake_table();
    kilnlink_param_value_t v;
    v.f32_val = 1285.0f;
    fake_add(0x0104, KILNLINK_PARAM_TYPE_F32, true, v);
    v.u8_val = 1;
    fake_add(0x0101, KILNLINK_PARAM_TYPE_U8, true, v);

    kiln_pkg_safety_t pico;
    kiln_pkg_pico_source_t src = fake_source();
    if (!kiln_package_capture_pico_half(&src, &pico)) {
        return false;
    }
    uint8_t esp_blob[16];
    for (size_t i = 0; i < sizeof(esp_blob); i++) {
        esp_blob[i] = (uint8_t)(i * 3 + 1);
    }
    uint32_t hash = 0;
    if (!kiln_package_compute_hash(KILN_PKG_SCHEMA_VERSION, esp_blob, sizeof(esp_blob), &pico, &hash)) {
        return false;
    }
    size_t len = 0;
    if (!kiln_package_export_json("Test Kiln", KILN_PKG_SCHEMA_VERSION, esp_blob, sizeof(esp_blob), &pico,
                                  hash, TEST_SOME_BOARD_ID, json_out, json_cap, &len)) {
        return false;
    }
    if (out_hash) {
        *out_hash = hash;
    }
    return true;
}

static void test_export_import_round_trip(void)
{
    TEST_SECTION("kiln_package_export_json/_import_json -- round trip is field-for-field identical");
    char json[KILN_PKG_JSON_MAX_LEN];
    uint32_t hash = 0;
    TEST_CHECK(build_sample_package(json, sizeof(json), &hash), "sample package builds");

    char name[32];
    uint16_t schema = 0;
    uint8_t esp_blob_out[16];
    uint16_t esp_len = 0;
    kiln_pkg_safety_t pico_out;
    uint32_t declared_hash = 0;
    bool has_source_board = false;
    uint32_t source_board_id = 0;
    char reason[160] = {0};
    bool ok = kiln_package_import_json(json, name, sizeof(name), &schema, esp_blob_out, sizeof(esp_blob_out),
                                       &esp_len, &pico_out, &declared_hash, &has_source_board, &source_board_id,
                                       reason, sizeof(reason));
    TEST_CHECK(ok, "import succeeds on a package this module just built");
    TEST_CHECK(strcmp(name, "Test Kiln") == 0, "name round-trips");
    TEST_CHECK(schema == KILN_PKG_SCHEMA_VERSION, "pkg_schema round-trips");
    TEST_CHECK(esp_len == 16, "esp_blob_len round-trips");
    for (size_t i = 0; i < 16; i++) {
        if (esp_blob_out[i] != (uint8_t)(i * 3 + 1)) {
            TEST_CHECK(false, "esp_blob byte round-trips exactly");
            break;
        }
    }
    TEST_CHECK(pico_out.count == 2, "pico entry count round-trips");
    TEST_CHECK(pico_out.entries[0].param_id == 0x0101, "pico entries round-trip sorted (0x0101 first)");
    TEST_CHECK(pico_out.entries[1].param_id == 0x0104, "pico entries round-trip sorted (0x0104 second)");
    TEST_CHECK(declared_hash == hash, "declared pkg_hash round-trips exactly");
    TEST_CHECK(has_source_board, "source_board_id round-trips as present");
    TEST_CHECK(source_board_id == TEST_SOME_BOARD_ID, "source_board_id round-trips exactly");
}

static void test_import_refuses_malformed_kind(void)
{
    TEST_SECTION("kiln_package_import_json -- refuses a file with the wrong/missing \"kind\" (malformed)");
    const char *bad = "{\"pkg_schema\":1,\"name\":\"x\",\"esp_blob_len\":1,\"esp_blob_hex\":\"ab\","
                      "\"pico\":[],\"pkg_hash\":\"0x00000000\"}";
    char name[32];
    uint16_t schema;
    uint8_t esp_blob_out[16];
    uint16_t esp_len;
    kiln_pkg_safety_t pico_out;
    uint32_t declared_hash;
    bool has_source_board = false;
    uint32_t source_board_id = 0;
    char reason[160] = {0};
    bool ok = kiln_package_import_json(bad, name, sizeof(name), &schema, esp_blob_out, sizeof(esp_blob_out),
                                       &esp_len, &pico_out, &declared_hash, &has_source_board, &source_board_id,
                                       reason, sizeof(reason));
    TEST_CHECK(!ok, "refuses a package with no recognisable \"kind\"");
    TEST_CHECK(strstr(reason, "kind") != NULL || strstr(reason, "not a kiln package") != NULL,
               "refusal reason names the problem, not a generic error");
}

static void test_import_refuses_newer_pkg_schema(void)
{
    TEST_SECTION("kiln_package_import_json -- refuses a pkg_schema newer than this firmware knows");
    char json[KILN_PKG_JSON_MAX_LEN];
    uint32_t hash = 0;
    TEST_CHECK(build_sample_package(json, sizeof(json), &hash), "sample package builds");

    // Tamper the schema number upward -- KILN_PKG_SCHEMA_VERSION is 1 today,
    // so "2" is unambiguously "newer than this firmware knows".
    char *p = strstr(json, "\"pkg_schema\":1");
    TEST_CHECK(p != NULL, "found pkg_schema field to tamper");
    if (p) {
        p[strlen("\"pkg_schema\":")] = '2';
    }

    char name[32];
    uint16_t schema;
    uint8_t esp_blob_out[16];
    uint16_t esp_len;
    kiln_pkg_safety_t pico_out;
    uint32_t declared_hash;
    bool has_source_board = false;
    uint32_t source_board_id = 0;
    char reason[160] = {0};
    bool ok = kiln_package_import_json(json, name, sizeof(name), &schema, esp_blob_out, sizeof(esp_blob_out),
                                       &esp_len, &pico_out, &declared_hash, &has_source_board, &source_board_id,
                                       reason, sizeof(reason));
    TEST_CHECK(!ok, "refuses a newer-than-known pkg_schema outright, never best-effort parsed");
    TEST_CHECK(strstr(reason, "newer") != NULL, "refusal reason says WHY (newer format version)");
}

static void test_export_json_detects_hash_mismatch_downstream(void)
{
    // kiln_package_import_json() itself does not verify the hash (see its
    // own header comment -- that requires the canonical re-derivation only
    // kiln_cfg_store.c can do). This test proves the PRIMITIVE this
    // downstream check depends on: a package tampered after export still
    // decodes cleanly (envelope-valid) but its declared pkg_hash provably
    // no longer matches a hash recomputed over the tampered bytes -- i.e.
    // the tamper is real and detectable, which is exactly what
    // kiln_cfg_store_import_package_json()'s hash-comparison step (host-
    // tested separately, since it needs zones_config_json.h) relies on.
    TEST_SECTION("tampering the ESP blob after export is detectable by recomputing the hash");
    char json[KILN_PKG_JSON_MAX_LEN];
    uint32_t original_hash = 0;
    TEST_CHECK(build_sample_package(json, sizeof(json), &original_hash), "sample package builds");

    // Flip one hex nibble inside esp_blob_hex -- a single-byte tamper, the
    // "hand-edited or truncated in transit" case section 5.1 exists for.
    char *hexval = strstr(json, "\"esp_blob_hex\":\"");
    TEST_CHECK(hexval != NULL, "found esp_blob_hex to tamper");
    if (hexval) {
        char *digit = hexval + strlen("\"esp_blob_hex\":\"");
        *digit = (*digit == 'a') ? 'b' : 'a';
    }

    char name[32];
    uint16_t schema;
    uint8_t esp_blob_out[16];
    uint16_t esp_len;
    kiln_pkg_safety_t pico_out;
    uint32_t declared_hash;
    bool has_source_board = false;
    uint32_t source_board_id = 0;
    char reason[160] = {0};
    bool ok = kiln_package_import_json(json, name, sizeof(name), &schema, esp_blob_out, sizeof(esp_blob_out),
                                       &esp_len, &pico_out, &declared_hash, &has_source_board, &source_board_id,
                                       reason, sizeof(reason));
    TEST_CHECK(ok, "envelope itself still decodes -- the tamper is inside a valid hex field, not garbage");
    uint32_t recomputed = 0;
    TEST_CHECK(kiln_package_compute_hash(schema, esp_blob_out, esp_len, &pico_out, &recomputed),
               "recompute over the tampered bytes succeeds");
    TEST_CHECK(recomputed != declared_hash,
               "recomputed hash no longer matches the declared pkg_hash -- the tamper is caught");
    TEST_CHECK(declared_hash == original_hash, "declared pkg_hash itself is untouched by the tamper");
}

static void test_hash_independent_of_source_board_id(void)
{
    // Ruling, kiln_package.h (2026-09-16): source_board_id is envelope-only
    // metadata, deliberately NEVER an input to kiln_package_compute_hash()'s
    // canonical serialization. Prove it directly: two exports of the SAME
    // pico/esp content but DIFFERENT source_board_id values must produce the
    // identical declared pkg_hash (the hash argument itself is computed
    // once, up front, independent of export -- this proves export doesn't
    // fold the board id in some other way, e.g. by re-hashing internally).
    TEST_SECTION("kiln_package_compute_hash() output does not depend on source_board_id");
    reset_fake_table();
    kilnlink_param_value_t v;
    v.f32_val = 1285.0f;
    fake_add(0x0104, KILNLINK_PARAM_TYPE_F32, true, v);
    kiln_pkg_safety_t pico;
    kiln_pkg_pico_source_t src = fake_source();
    TEST_CHECK(kiln_package_capture_pico_half(&src, &pico), "pico half captures");
    uint8_t esp_blob[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t hash = 0;
    TEST_CHECK(kiln_package_compute_hash(KILN_PKG_SCHEMA_VERSION, esp_blob, sizeof(esp_blob), &pico, &hash),
               "hash computes");

    char json_a[KILN_PKG_JSON_MAX_LEN];
    char json_b[KILN_PKG_JSON_MAX_LEN];
    size_t len_a = 0, len_b = 0;
    TEST_CHECK(kiln_package_export_json("X", KILN_PKG_SCHEMA_VERSION, esp_blob, sizeof(esp_blob), &pico, hash,
                                        0x00000001u, json_a, sizeof(json_a), &len_a),
               "export with board id 0x00000001 succeeds");
    TEST_CHECK(kiln_package_export_json("X", KILN_PKG_SCHEMA_VERSION, esp_blob, sizeof(esp_blob), &pico, hash,
                                        0xFFFFFFFFu, json_b, sizeof(json_b), &len_b),
               "export with board id 0xFFFFFFFF succeeds");

    char name_a[32], name_b[32];
    uint16_t schema_a, schema_b;
    uint8_t blob_a[8], blob_b[8];
    uint16_t elen_a, elen_b;
    kiln_pkg_safety_t pico_a, pico_b;
    uint32_t hash_a, hash_b;
    bool has_a = false, has_b = false;
    uint32_t sb_a = 0, sb_b = 0;
    char reason[160] = {0};
    TEST_CHECK(kiln_package_import_json(json_a, name_a, sizeof(name_a), &schema_a, blob_a, sizeof(blob_a),
                                        &elen_a, &pico_a, &hash_a, &has_a, &sb_a, reason, sizeof(reason)),
               "re-import of A succeeds");
    TEST_CHECK(kiln_package_import_json(json_b, name_b, sizeof(name_b), &schema_b, blob_b, sizeof(blob_b),
                                        &elen_b, &pico_b, &hash_b, &has_b, &sb_b, reason, sizeof(reason)),
               "re-import of B succeeds");
    TEST_CHECK(has_a && sb_a == 0x00000001u, "A's source_board_id round-trips");
    TEST_CHECK(has_b && sb_b == 0xFFFFFFFFu, "B's source_board_id round-trips");
    TEST_CHECK(hash_a == hash_b, "declared pkg_hash is IDENTICAL across differing source_board_id");
    TEST_CHECK(hash_a == hash, "declared pkg_hash matches the independently-computed hash");
}

static void test_import_source_board_id_absent(void)
{
    // Backward compatibility ruling (kiln_package.h): a package with no
    // "source_board_id" field at all (an old file, or one from firmware
    // before this field existed) must parse successfully with
    // *out_has_source_board == false -- absence is not a parse error, only
    // kiln_cfg_store.c's caller decides what absence MEANS (fail-safe
    // "foreign").
    TEST_SECTION("kiln_package_import_json -- \"source_board_id\" absent is not an error");
    char json[KILN_PKG_JSON_MAX_LEN];
    uint32_t hash = 0;
    TEST_CHECK(build_sample_package(json, sizeof(json), &hash), "sample package builds");

    // Strip the field this test file's own build_sample_package() just
    // added, simulating an old-format file.
    char *field = strstr(json, "\"source_board_id\":\"0x11223344\",");
    TEST_CHECK(field != NULL, "found source_board_id field to remove");
    if (field) {
        size_t field_len = strlen("\"source_board_id\":\"0x11223344\",");
        memmove(field, field + field_len, strlen(field + field_len) + 1);
    }

    char name[32];
    uint16_t schema;
    uint8_t esp_blob_out[16];
    uint16_t esp_len;
    kiln_pkg_safety_t pico_out;
    uint32_t declared_hash;
    bool has_source_board = true; // deliberately pre-set to the WRONG value
    uint32_t source_board_id = 0xDEADBEEFu;
    char reason[160] = {0};
    bool ok = kiln_package_import_json(json, name, sizeof(name), &schema, esp_blob_out, sizeof(esp_blob_out),
                                       &esp_len, &pico_out, &declared_hash, &has_source_board, &source_board_id,
                                       reason, sizeof(reason));
    TEST_CHECK(ok, "import still succeeds with the field entirely absent");
    TEST_CHECK(!has_source_board, "has_source_board is explicitly cleared to false, not left stale");
    TEST_CHECK(source_board_id == 0, "source_board_id is explicitly zeroed, not left stale");
}

static void test_import_source_board_id_malformed_refuses(void)
{
    // Present-but-malformed is a REFUSAL, not a silent "treat as absent" --
    // a field that exists but cannot be trusted is worse than one that was
    // never written (same asymmetry kiln_package.h's ruling states).
    TEST_SECTION("kiln_package_import_json -- \"source_board_id\" present but malformed is refused");
    char json[KILN_PKG_JSON_MAX_LEN];
    uint32_t hash = 0;
    TEST_CHECK(build_sample_package(json, sizeof(json), &hash), "sample package builds");

    char *field = strstr(json, "\"source_board_id\":\"0x11223344\"");
    TEST_CHECK(field != NULL, "found source_board_id field to tamper");
    if (field) {
        // Corrupt one hex digit into a non-hex character.
        char *digit = field + strlen("\"source_board_id\":\"0x");
        *digit = 'z';
    }

    char name[32];
    uint16_t schema;
    uint8_t esp_blob_out[16];
    uint16_t esp_len;
    kiln_pkg_safety_t pico_out;
    uint32_t declared_hash;
    bool has_source_board = false;
    uint32_t source_board_id = 0;
    char reason[160] = {0};
    bool ok = kiln_package_import_json(json, name, sizeof(name), &schema, esp_blob_out, sizeof(esp_blob_out),
                                       &esp_len, &pico_out, &declared_hash, &has_source_board, &source_board_id,
                                       reason, sizeof(reason));
    TEST_CHECK(!ok, "refuses outright rather than silently treating malformed as absent");
    TEST_CHECK(strstr(reason, "source_board_id") != NULL, "refusal reason names the field");
}

// ---------------------------------------------------------------------------
// Golden for tools/PcTools/src/kilnctrl/config_convert.py's kiln_package
// hash/export -- see firmware/KilnFW/App/test/test_zones_blob_golden.c's own
// header comment for the rationale (pinning a hand-mirrored Python layout
// against bytes the FIRMWARE actually produced, so a layout drift on either
// side goes red instead of the two sides silently comparing themselves to
// themselves).
//
// This executable (test_kiln_package.c) does not link zones_config_store.c,
// so it cannot fill/save its own zones_cfg_t the way test_zones_blob_golden.c
// does. Instead it reads the ALREADY-COMMITTED zones_cfg_t golden fixture
// (zones_cfg_golden.txt's own "blob " hex line) to get a real firmware-
// produced esp_blob, builds a fixed kiln_pkg_safety_t with two sentinel pico
// entries, and runs the real kiln_package_compute_hash()/
// kiln_package_export_json() over them -- the same production functions
// convert_kiln_package's own callers use.
//
// tools/PcTools/tests/test_config_convert_kiln_package_golden.py checks
// config_convert.py's _kiln_pkg_compute_hash()/convert_kiln_package() against
// this same committed fixture, so this replaces the old hash test that
// compared the Python function with itself.
//
// Regenerate after an intentional kiln_package.c wire-format change by
// running this executable with KILNCTL_REGEN_CONFIG_CONVERT_GOLDENS=1 set,
// then update config_convert.py until the pytest passes again.
#define KPG_GOLDEN_REL "../../../../tools/PcTools/tests/fixtures/config_convert/kiln_package_golden.txt"
#define KPG_ZONES_GOLDEN_REL "../../../../tools/PcTools/tests/fixtures/config_convert/zones_cfg_golden.txt"
#define KPG_REGEN_ENV "KILNCTL_REGEN_CONFIG_CONVERT_GOLDENS"

static char s_kpg_text[16 * 1024];
static size_t s_kpg_text_len;

static void kpg_emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(s_kpg_text + s_kpg_text_len, sizeof(s_kpg_text) - s_kpg_text_len, fmt, ap);
    va_end(ap);
    if (w > 0 && (size_t)w < sizeof(s_kpg_text) - s_kpg_text_len) {
        s_kpg_text_len += (size_t)w;
    }
}

static int kpg_text_equal_ignoring_cr(const char *a, const char *b, int *first_diff_line)
{
    int line = 1;
    while (*a || *b) {
        if (*a == '\r') { a++; continue; }
        if (*b == '\r') { b++; continue; }
        if (*a != *b) { *first_diff_line = line; return 0; }
        if (*a == '\n') { line++; }
        a++;
        b++;
    }
    return 1;
}

// Extracts the hex payload of the committed zones golden's "blob " line
// without needing any struct/JSON parsing -- this file only needs the raw
// bytes, not the field-by-field breakdown test_zones_blob_golden.c produces.
static bool kpg_read_zones_golden_blob(uint8_t *out, size_t out_cap, size_t *out_len)
{
    char *path = test_resolve_from_here(__FILE__, KPG_ZONES_GOLDEN_REL);
    if (!path) {
        return false;
    }
    char *text = test_read_whole_file(path);
    free(path);
    if (!text) {
        return false;
    }
    bool ok = false;
    const char *line = text;
    while (line) {
        if (strncmp(line, "blob ", 5) == 0) {
            const char *hex = line + 5;
            size_t n = 0;
            while (hex[n] && hex[n] != '\n' && hex[n] != '\r') {
                n++;
            }
            if (n % 2 == 0 && n / 2 <= out_cap) {
                size_t bytes = n / 2;
                bool bad = false;
                for (size_t i = 0; i < bytes; i++) {
                    unsigned v;
                    if (sscanf(hex + i * 2, "%2x", &v) != 1) {
                        bad = true;
                        break;
                    }
                    out[i] = (uint8_t)v;
                }
                if (!bad) {
                    *out_len = bytes;
                    ok = true;
                }
            }
            break;
        }
        const char *next = strchr(line, '\n');
        line = next ? next + 1 : NULL;
    }
    free(text);
    return ok;
}

static void test_kiln_package_golden_matches_firmware_layout(void)
{
    TEST_SECTION("kiln_package hash/export -- firmware-generated golden");

    static uint8_t esp_blob[4096];
    size_t esp_blob_len = 0;
    bool got_blob = kpg_read_zones_golden_blob(esp_blob, sizeof(esp_blob), &esp_blob_len);
    TEST_CHECK(got_blob, "kiln_package golden: read committed zones_cfg_t golden blob");
    if (!got_blob) {
        return;
    }

    kiln_pkg_safety_t pico;
    memset(&pico, 0, sizeof(pico));
    pico.count = 2;
    pico.entries[0].param_id = 0x0102;
    pico.entries[0].type = 3;
    pico.entries[0].flags = 1;
    pico.entries[0].value_bits = 0x11223344u;
    pico.entries[1].param_id = 0x0304;
    pico.entries[1].type = 7;
    pico.entries[1].flags = 0;
    pico.entries[1].value_bits = 0xaabbccddu;

    uint16_t pkg_schema = KILN_PKG_SCHEMA_VERSION;
    uint32_t hash = 0;
    bool hashed = kiln_package_compute_hash(pkg_schema, esp_blob, (uint16_t)esp_blob_len, &pico, &hash);
    TEST_CHECK(hashed, "kiln_package golden: kiln_package_compute_hash succeeds");
    if (!hashed) {
        return;
    }

    static char exported[8192];
    size_t exported_len = 0;
    bool exported_ok = kiln_package_export_json("golden-package", pkg_schema, esp_blob, (uint16_t)esp_blob_len,
                                                &pico, hash, 0x11223344u, exported, sizeof(exported), &exported_len);
    TEST_CHECK(exported_ok, "kiln_package golden: kiln_package_export_json succeeds");
    if (!exported_ok) {
        return;
    }

    kpg_emit("# kiln_package hash/export golden -- GENERATED by firmware/KilnFW/App/test/test_kiln_package.c.\n");
    kpg_emit("# Do not edit by hand; regenerate with %s=1 (see that file's header).\n", KPG_REGEN_ENV);
    kpg_emit("pkg_schema %u\n", (unsigned)pkg_schema);
    kpg_emit("esp_blob_len %u\n", (unsigned)esp_blob_len);
    kpg_emit("pico_count %u\n", (unsigned)pico.count);
    for (uint16_t i = 0; i < pico.count; i++) {
        kpg_emit("pico[%u] id=%u type=%u flags=%u value_bits=%lu\n", (unsigned)i,
                 (unsigned)pico.entries[i].param_id, (unsigned)pico.entries[i].type,
                 (unsigned)pico.entries[i].flags, (unsigned long)pico.entries[i].value_bits);
    }
    kpg_emit("hash 0x%08lx\n", (unsigned long)hash);
    kpg_emit("export_json %s\n", exported);

    char *golden_path = test_resolve_from_here(__FILE__, KPG_GOLDEN_REL);
    TEST_CHECK(golden_path != NULL, "kiln_package golden: resolve golden path");
    if (!golden_path) {
        return;
    }
    const char *regen = getenv(KPG_REGEN_ENV);
    if (regen && regen[0] == '1') {
        FILE *f = fopen(golden_path, "wb");
        TEST_CHECK(f != NULL, "kiln_package golden: open golden for regeneration");
        if (f) {
            fwrite(s_kpg_text, 1, s_kpg_text_len, f);
            fclose(f);
            printf("  regenerated %s\n", golden_path);
        }
    }
    char *committed = test_read_whole_file(golden_path);
    if (!committed) {
        printf("  golden not found at %s\n", golden_path);
        TEST_CHECK(false, "kiln_package golden: committed golden file exists");
    } else {
        int diff_line = 0;
        int same = kpg_text_equal_ignoring_cr(s_kpg_text, committed, &diff_line);
        if (!same) {
            printf("  kiln_package golden differs from the committed file at line %d: %s\n"
                   "  kiln_package's wire format (or this test's fixed inputs) changed. If intentional,\n"
                   "  regenerate with %s=1 and update tools/PcTools/src/kilnctrl/config_convert.py to match.\n",
                   diff_line, golden_path, KPG_REGEN_ENV);
        }
        TEST_CHECK(same, "kiln_package golden: firmware-generated golden matches the committed file");
        free(committed);
    }
    free(golden_path);
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
    test_export_import_round_trip();
    test_import_refuses_malformed_kind();
    test_import_refuses_newer_pkg_schema();
    test_export_json_detects_hash_mismatch_downstream();
    test_hash_independent_of_source_board_id();
    test_import_source_board_id_absent();
    test_import_source_board_id_malformed_refuses();
    test_kiln_package_golden_matches_firmware_layout();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
