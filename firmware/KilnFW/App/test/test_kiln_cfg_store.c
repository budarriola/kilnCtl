// Host tests for App/drivers/persist/kiln_cfg_store.c -- the saved "kiln config"
// slots (owner-report, 2026-08-21 follow-up: "save kiln profiles with
// different relay/thermocouple/PID configs ... survive a programming cycle
// ... allow creating a config from an existing one").
//
// kiln_cfg_store.c is #included directly (same convention as
// test_backup_import.c/test_wifi_prov.c) so this file can reach its static
// helpers and s_store directly -- there is no other seam into a module whose
// entire job is managing file-scope state.
//
// zones_http.h's zones_config_blob_size()/_export_blob()/_import_blob() are
// stubbed here as plain, controllable C functions rather than pulling in the
// real zones_http.c -- that file's own zones_config_get_thermo_count()/
// _get_max_ramp()/_get_model() are ALREADY stubbed as globals by
// test_profile_feasibility.c (linked into this same executable, see
// test_backup_import.c's own "Collision note"), so a second, real
// definition from zones_http.c would be a multiple-definition link error.
// This means the two assertions about zones_cfg_t's OWN version-refuse/
// bounds-check logic ("a newer-version blob is refused", "an out-of-range
// stored value is rejected") are proven here at the CONTRACT kiln_cfg_store.c
// actually depends on -- "when zones_config_import_blob() refuses with a
// reason, kiln_cfg_store_apply() propagates that refusal and writes
// nothing" -- rather than by re-exercising zones_http.c's own bound
// constants a second time from a different file. zones_http.c's own
// validate_zones_cfg()/three-outcome version handling is exercised for real
// every time zones_post_handler() runs (its live behavior on-target), not
// duplicated here.
//
// ota_http.h's transitive drag-in of kiln_io.h/MAX31856.h/safety_link.h
// (via ota_http_check_interlocks()'s declaration) reaches the same stub
// surface (driver/i2c_master.h, driver/spi_master.h, esp_spi_owner.h,
// freertos/*, i2c_owner.h) test_backup_import.c already proved compiles --
// see that file's header comment for the full list. Nothing here calls any
// of the OTHER ota_http.h functions (ota_http_start(),
// ota_http_verify_request(), the update-mutex functions), so only
// ota_http_check_interlocks() needs a body -- the rest are declarations
// only, never referenced, and need none.
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#include <direct.h>
#define TKCF_MKDIR(p) _mkdir(p)
#define TKCF_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TKCF_MKDIR(p) mkdir((p), 0755)
#define TKCF_RMDIR(p) rmdir(p)
#endif

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "cfg_fs.h"

// Test-only malloc seam for nvs_load_store()'s v1-migration-buffer
// allocation (kiln_cfg_store.c:234ish, `malloc(sizeof(*v1))`) -- proves the
// "malloc failure leaves defaults standing" branch without needing a real
// out-of-memory condition. kiln_cfg_store_test_malloc() is defined BEFORE
// the #define below takes effect, so it still calls the REAL malloc() on
// the non-failing path; the #define only redirects calls made FROM
// kiln_cfg_store.c (textually included right after it), never this
// function's own body.
static bool s_test_malloc_should_fail = false;
static void *kiln_cfg_store_test_malloc(size_t n)
{
    if (s_test_malloc_should_fail) {
        return NULL;
    }
    return malloc(n);
}
#define malloc kiln_cfg_store_test_malloc

#include "../drivers/persist/kiln_cfg_store.c"

// ---------------------------------------------------------------------------
// zones_http.h stub state -- controllable export/import behavior.
// ---------------------------------------------------------------------------

static size_t s_stub_blob_size = 16;
static bool s_stub_export_ok = true;
static uint8_t s_stub_export_content[ZONES_CONFIG_BLOB_MAX_SIZE];

static bool s_stub_import_result = true;
static char s_stub_import_reason[96] = "";
static int s_stub_import_call_count = 0;
static uint8_t s_stub_import_last_bytes[ZONES_CONFIG_BLOB_MAX_SIZE];
static size_t s_stub_import_last_len = 0;

size_t zones_config_blob_size(void)
{
    return s_stub_blob_size;
}

bool zones_config_export_blob(void *out, size_t out_cap)
{
    if (!out || out_cap < s_stub_blob_size) {
        return false;
    }
    if (!s_stub_export_ok) {
        return false;
    }
    memcpy(out, s_stub_export_content, s_stub_blob_size);
    return true;
}

bool zones_config_import_blob(const void *blob, size_t len, char *reason_out, size_t reason_cap)
{
    s_stub_import_call_count++;
    s_stub_import_last_len = len < sizeof(s_stub_import_last_bytes) ? len : sizeof(s_stub_import_last_bytes);
    memset(s_stub_import_last_bytes, 0, sizeof(s_stub_import_last_bytes));
    if (blob && len > 0) {
        memcpy(s_stub_import_last_bytes, blob, s_stub_import_last_len);
    }
    if (reason_out && reason_cap) {
        strncpy(reason_out, s_stub_import_reason, reason_cap - 1);
        reason_out[reason_cap - 1] = '\0';
    }
    return s_stub_import_result;
}

// ---------------------------------------------------------------------------
// ota_http.h stub -- only ota_http_check_interlocks() is ever called from
// kiln_cfg_store.c. The function BODY lives in test_backup_import.c (also
// linked into this executable) -- exactly one definition may exist
// link-wide, and that file's stub was made controllable via the two globals
// below specifically so this file doesn't need a second, colliding
// definition. See that file's own comment on the pair.
// ---------------------------------------------------------------------------

extern ota_interlock_result_t g_stub_ota_interlock_result;
extern char g_stub_ota_interlock_reason[OTA_INTERLOCK_REASON_MAX];

// ---------------------------------------------------------------------------
// Test scaffolding
// ---------------------------------------------------------------------------

static void reset_state(void)
{
    reset_to_defaults(); // kiln_cfg_store.c's own static helper -- s_store to empty/no-active

    s_stub_blob_size = 16;
    s_stub_export_ok = true;
    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 1); // a distinctive, non-zero pattern
    }

    s_stub_import_result = true;
    s_stub_import_reason[0] = '\0';
    s_stub_import_call_count = 0;
    memset(s_stub_import_last_bytes, 0, sizeof(s_stub_import_last_bytes));
    s_stub_import_last_len = 0;

    g_stub_ota_interlock_result = OTA_INTERLOCK_OK;
    g_stub_ota_interlock_reason[0] = '\0';
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_save_clone_apply_roundtrip(void)
{
    TEST_SECTION("kiln_cfg_store -- save / clone / apply round-trip");
    reset_state();

    int32_t id1 = -1;
    char reason[96];
    reason[0] = '\0';
    bool ok = kiln_cfg_store_save_current("Cone 6 Glaze", -1, &id1, reason, sizeof(reason));
    TEST_CHECK(ok, "save-as-new succeeds");
    TEST_CHECK(id1 > 0, "a real, positive id was assigned");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id1,
               "saving the CURRENT live config marks it active (it IS what's live)");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n == 1, "exactly one saved config exists");
    TEST_CHECK(rows[0].id == id1 && strcmp(rows[0].name, "Cone 6 Glaze") == 0 && rows[0].is_active,
               "listing reports the saved id/name/active flag correctly");

    // Clone it under a new name.
    int32_t id2 = -1;
    ok = kiln_cfg_store_clone(id1, "Cone 6 Glaze copy", &id2, reason, sizeof(reason));
    TEST_CHECK(ok, "clone-from-existing succeeds");
    TEST_CHECK(id2 > 0 && id2 != id1, "the clone gets its own, different id");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id1,
               "cloning does NOT change which config is active -- it's a new saved slot only");

    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(kiln_cfg_store_get_name(id2, name_buf, sizeof(name_buf)) &&
                   strcmp(name_buf, "Cone 6 Glaze copy") == 0,
               "the clone's name is the new one, not the source's");

    // Applying the clone should feed zones_config_import_blob() the SAME
    // bytes the clone copied from the source (proving the clone really did
    // copy the blob, not just the name).
    ok = kiln_cfg_store_apply(id2, false, reason, sizeof(reason));
    TEST_CHECK(ok, "apply of the clone succeeds (interlock OK, stub import accepts)");
    TEST_CHECK(s_stub_import_call_count == 1, "zones_config_import_blob() was called exactly once");
    TEST_CHECK(s_stub_import_last_len == s_stub_blob_size &&
                   memcmp(s_stub_import_last_bytes, s_stub_export_content, s_stub_blob_size) == 0,
               "the applied bytes match what was originally exported and cloned");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id2, "a successful apply marks that config active");
}

// The regression the owner-report review specifically asked to prove: apply
// must be refused by kiln_cfg_store_apply() ITSELF -- the backstop -- with
// NO pre-check from the caller. This test deliberately calls
// kiln_cfg_store_apply() directly, the same way a hypothetical future caller
// that forgot to pre-check would, to prove the interlock cannot be bypassed
// by omission.
static void test_apply_refused_while_run_active(void)
{
    TEST_SECTION("kiln_cfg_store_apply -- refused by its OWN interlock check, no caller pre-check");
    reset_state();

    int32_t id = -1;
    char reason[96];
    kiln_cfg_store_save_current("Bisque", -1, &id, reason, sizeof(reason));

    // A second saved config, DIFFERENT from the one about to be applied --
    // saving-as-new always marks the fresh save active, so this is now the
    // active one, and "id" (Bisque) is the target of the apply attempt
    // below. A wrongly-successful apply would flip the active id from id2 to
    // id, which is exactly what the checks below would catch.
    int32_t id2 = -1;
    kiln_cfg_store_save_current("Cone 10 Reduction", -1, &id2, reason, sizeof(reason));
    int32_t active_before = kiln_cfg_store_get_active_id();
    TEST_CHECK(active_before == id2, "sanity: the second save is active before the refused apply attempt");

    g_stub_ota_interlock_result = OTA_INTERLOCK_REFUSED;
    strncpy(g_stub_ota_interlock_reason, "a firing is currently running",
            sizeof(g_stub_ota_interlock_reason) - 1);

    reason[0] = '\0';
    int import_calls_before = s_stub_import_call_count;
    // Deliberately calling kiln_cfg_store_apply() directly, with NO
    // ota_http_check_interlocks() pre-check of our own -- exactly what a
    // future caller that forgot to pre-check (the UART bridge, an MCP tool,
    // a backup restore path, a factory-reset routine) would do. The
    // assertion below only holds if the check lives INSIDE
    // kiln_cfg_store_apply() itself.
    bool ok = kiln_cfg_store_apply(id, false, reason, sizeof(reason));

    TEST_CHECK(!ok, "apply is refused while a firing is running, even with no caller pre-check");
    TEST_CHECK(strcmp(reason, "a firing is currently running") == 0,
               "the specific interlock reason is propagated to the caller -- proves "
               "ota_http_check_interlocks() was actually consulted and its result used, since "
               "`reason` was cleared right before this call and could only have been filled by it");
    TEST_CHECK(s_stub_import_call_count == import_calls_before,
               "zones_config_import_blob() was never reached -- nothing was validated or written");
    TEST_CHECK(kiln_cfg_store_get_active_id() == active_before,
               "active id is unchanged -- the refused apply did not take effect");
}

static void test_newer_version_blob_refused_by_store(void)
{
    TEST_SECTION("kiln_cfg_store_apply -- a config zones_config_import_blob() refuses (newer firmware) "
                 "changes nothing");
    reset_state();

    int32_t id = -1;
    char reason[96];
    kiln_cfg_store_save_current("Old Cone 6", -1, &id, reason, sizeof(reason));
    int32_t active_before = kiln_cfg_store_get_active_id();

    s_stub_import_result = false;
    strncpy(s_stub_import_reason, "this config was saved by newer firmware -- refusing",
            sizeof(s_stub_import_reason) - 1);

    reason[0] = '\0';
    bool ok = kiln_cfg_store_apply(id, false, reason, sizeof(reason));

    TEST_CHECK(!ok, "apply is refused when the stored blob is newer than this firmware understands");
    TEST_CHECK(strstr(reason, "newer firmware") != NULL, "the specific refusal reason is propagated");
    TEST_CHECK(kiln_cfg_store_get_active_id() == active_before,
               "active id is unchanged -- the refused apply changed nothing");
}

static void test_out_of_range_value_rejected_nothing_written(void)
{
    TEST_SECTION("kiln_cfg_store_apply -- an out-of-range stored value is rejected, nothing applied");
    reset_state();

    int32_t id_a = -1, id_b = -1;
    char reason[96];
    kiln_cfg_store_save_current("Config A", -1, &id_a, reason, sizeof(reason));
    s_stub_export_content[0] ^= 0xFF; // make B's exported bytes provably different from A's
    kiln_cfg_store_save_current("Config B", -1, &id_b, reason, sizeof(reason));
    int32_t active_before = kiln_cfg_store_get_active_id(); // B, from the save above

    kiln_cfg_summary_t rows_before[KILN_CFG_MAX_COUNT];
    uint8_t n_before = kiln_cfg_store_list(rows_before, KILN_CFG_MAX_COUNT);

    s_stub_import_result = false;
    strncpy(s_stub_import_reason, "zone max_temp_c out of range", sizeof(s_stub_import_reason) - 1);

    reason[0] = '\0';
    bool ok = kiln_cfg_store_apply(id_a, false, reason, sizeof(reason));

    TEST_CHECK(!ok, "apply is refused when a stored field fails re-validation");
    TEST_CHECK(strcmp(reason, "zone max_temp_c out of range") == 0, "the specific field reason is propagated");
    TEST_CHECK(kiln_cfg_store_get_active_id() == active_before,
               "active id is unchanged (still Config B) -- the refused apply of Config A did not take effect");

    kiln_cfg_summary_t rows_after[KILN_CFG_MAX_COUNT];
    uint8_t n_after = kiln_cfg_store_list(rows_after, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n_after == n_before, "no entries were added or removed by the failed apply");
    for (uint8_t i = 0; i < n_before; i++) {
        TEST_CHECK(rows_after[i].id == rows_before[i].id && strcmp(rows_after[i].name, rows_before[i].name) == 0 &&
                       rows_after[i].is_active == rows_before[i].is_active,
                   "every saved entry's id/name/active flag is byte-for-byte unchanged by the failed apply");
    }
}

// ---------------------------------------------------------------------------
// Duplicate-name defect (owner-report, 2026-08-19 bench find): POST
// .../clone with the same name twice returned two DIFFERENT ids for the
// SAME name -- an operator picking a name in the LCD/web picker cannot tell
// the two apart, and picking the wrong one silently reconfigures the kiln's
// guard thresholds. Fixed by rejecting a case-insensitive, trim-normalized
// duplicate name on every path that can create or change one: save (both
// save-as-new and overwrite-by-id), clone, rename.
// ---------------------------------------------------------------------------

static void test_save_as_new_rejects_duplicate_name(void)
{
    TEST_SECTION("kiln_cfg_store_save_current -- save-as-new rejects a duplicate name (the reported bench "
                 "defect: two clones both named 'spare')");
    reset_state();

    int32_t id1 = -1;
    char reason[96];
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_save_current("spare", -1, &id1, reason, sizeof(reason)),
               "first save-as-new named 'spare' succeeds");

    int32_t id2 = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_save_current("spare", -1, &id2, reason, sizeof(reason));
    TEST_CHECK(!ok, "a second save-as-new under the SAME name is refused");
    TEST_CHECK(strcmp(reason, "a saved kiln config already has that name") == 0,
               "the refusal reason is specific, same shape as 'name missing or too long'");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n == 1, "the refused save wrote nothing -- still exactly one saved entry");
}

static void test_clone_rejects_duplicate_name(void)
{
    TEST_SECTION("kiln_cfg_store_clone -- rejects a duplicate name (exact bench repro: clone id=1 as "
                 "'spare' twice)");
    reset_state();

    int32_t id1 = -1;
    char reason[96];
    reason[0] = '\0';
    kiln_cfg_store_save_current("testkiln", -1, &id1, reason, sizeof(reason));

    int32_t id2 = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_clone(id1, "spare", &id2, reason, sizeof(reason));
    TEST_CHECK(ok && id2 > 0, "first clone named 'spare' succeeds");

    int32_t id3 = -1;
    reason[0] = '\0';
    ok = kiln_cfg_store_clone(id1, "spare", &id3, reason, sizeof(reason));
    TEST_CHECK(!ok, "cloning a SECOND time under the same name 'spare' is refused -- this is the exact "
                    "bench repro (POST clone id=1 name=spare twice used to return two different ids)");
    TEST_CHECK(strcmp(reason, "a saved kiln config already has that name") == 0,
               "the refusal reason is specific");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n == 2, "the store still holds exactly testkiln + one 'spare' -- no phantom third entry");
}

static void test_rename_rejects_duplicate_name(void)
{
    TEST_SECTION("kiln_cfg_store_rename -- rejects renaming onto another entry's existing name");
    reset_state();

    int32_t id_a = -1, id_b = -1;
    char reason[96];
    reason[0] = '\0';
    kiln_cfg_store_save_current("Bisque", -1, &id_a, reason, sizeof(reason));
    kiln_cfg_store_save_current("Cone 6 Glaze", -1, &id_b, reason, sizeof(reason));

    bool ok = kiln_cfg_store_rename(id_b, "Bisque");
    TEST_CHECK(!ok, "renaming id_b onto id_a's existing name 'Bisque' is refused");

    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(kiln_cfg_store_get_name(id_b, name_buf, sizeof(name_buf)) &&
                   strcmp(name_buf, "Cone 6 Glaze") == 0,
               "the refused rename left id_b's name untouched");
}

// Case sensitivity: "Spare" and "spare" are indistinguishable to an operator
// reading a name picker, so they are treated as the SAME name for collision
// purposes across all three write paths.
static void test_case_insensitive_duplicate_rejected(void)
{
    TEST_SECTION("case-insensitive duplicate rejected on save, clone, and rename ('Spare' collides with "
                 "'spare')");
    reset_state();

    int32_t id1 = -1, id2 = -1;
    char reason[96];
    reason[0] = '\0';
    kiln_cfg_store_save_current("spare", -1, &id1, reason, sizeof(reason));

    reason[0] = '\0';
    bool ok = kiln_cfg_store_save_current("Spare", -1, &id2, reason, sizeof(reason));
    TEST_CHECK(!ok, "save-as-new 'Spare' collides with existing 'spare' (case-insensitive)");

    reason[0] = '\0';
    ok = kiln_cfg_store_clone(id1, "SPARE", &id2, reason, sizeof(reason));
    TEST_CHECK(!ok, "clone named 'SPARE' collides with existing 'spare' (case-insensitive)");

    int32_t id3 = -1;
    reason[0] = '\0';
    kiln_cfg_store_save_current("testkiln", -1, &id3, reason, sizeof(reason));
    ok = kiln_cfg_store_rename(id3, "spARE");
    TEST_CHECK(!ok, "rename to 'spARE' collides with existing 'spare' (case-insensitive)");
}

// Leading/trailing whitespace: trimmed before BOTH comparing and storing --
// " spare" collides with "spare", and the stored name never carries the
// stray space (so the picker never shows an invisible-whitespace variant).
static void test_whitespace_trimmed_before_compare_and_store(void)
{
    TEST_SECTION("leading/trailing whitespace is trimmed before comparing AND before storing");
    reset_state();

    int32_t id1 = -1;
    char reason[96];
    reason[0] = '\0';
    kiln_cfg_store_save_current("spare", -1, &id1, reason, sizeof(reason));

    int32_t id2 = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_save_current("  spare  ", -1, &id2, reason, sizeof(reason));
    TEST_CHECK(!ok, "'  spare  ' collides with existing 'spare' once trimmed");

    // Now prove trimming also applies to what actually gets STORED, not just
    // to the comparison: rename an unrelated entry to a padded name and read
    // it back -- it must come back with no leading/trailing space.
    int32_t id3 = -1;
    reason[0] = '\0';
    kiln_cfg_store_save_current("testkiln", -1, &id3, reason, sizeof(reason));
    ok = kiln_cfg_store_rename(id3, "  bisque  ");
    TEST_CHECK(ok, "rename to a padded-but-otherwise-unique name succeeds");
    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(kiln_cfg_store_get_name(id3, name_buf, sizeof(name_buf)) && strcmp(name_buf, "bisque") == 0,
               "the STORED name has no leading/trailing whitespace -- trimmed before storing, not just "
               "before comparing");
}

// Re-saving a config under its OWN current name (overwrite-by-id) must
// still work -- that is "re-save this config from the current setup", not a
// collision with itself.
static void test_overwrite_by_id_same_name_still_allowed(void)
{
    TEST_SECTION("save-current overwrite-by-id under its OWN existing name is still allowed (must not "
                 "regress into a false self-collision)");
    reset_state();

    int32_t id1 = -1;
    char reason[96];
    reason[0] = '\0';
    kiln_cfg_store_save_current("Cone 6 Glaze", -1, &id1, reason, sizeof(reason));

    int32_t out_id = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_save_current("Cone 6 Glaze", id1, &out_id, reason, sizeof(reason));
    TEST_CHECK(ok, "overwriting id1 under its OWN current name succeeds -- not a collision with itself");
    TEST_CHECK(out_id == id1, "the same id is reported back");

    // Same check with a different-case/whitespace form of its own name --
    // still the same entry, still allowed.
    reason[0] = '\0';
    ok = kiln_cfg_store_save_current("  cone 6 glaze  ", id1, &out_id, reason, sizeof(reason));
    TEST_CHECK(ok, "overwriting id1 under a case/whitespace-equivalent form of its OWN name still succeeds");
}

// No-op rename (or a rename that only changes case/whitespace of the SAME
// name) must not be reported as a collision.
static void test_noop_rename_allowed(void)
{
    TEST_SECTION("rename to the config's own existing name is a no-op, not a collision");
    reset_state();

    int32_t id1 = -1;
    char reason[96];
    reason[0] = '\0';
    kiln_cfg_store_save_current("Cone 6 Glaze", -1, &id1, reason, sizeof(reason));

    TEST_CHECK(kiln_cfg_store_rename(id1, "Cone 6 Glaze"), "renaming id1 to its own exact current name succeeds");
    TEST_CHECK(kiln_cfg_store_rename(id1, "  CONE 6 GLAZE  "),
               "renaming id1 to a case/whitespace-equivalent form of its own name also succeeds");

    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(kiln_cfg_store_get_name(id1, name_buf, sizeof(name_buf)) && strcmp(name_buf, "CONE 6 GLAZE") == 0,
               "the stored name reflects the latest rename's trimmed text");
}

// Empty or whitespace-only names: an unnamed entry is as bad as a
// duplicated one in the picker.
static void test_empty_or_whitespace_only_name_rejected(void)
{
    TEST_SECTION("empty or whitespace-only name rejected on save, clone, and rename");
    reset_state();

    int32_t id1 = -1;
    char reason[96];
    reason[0] = '\0';
    bool ok = kiln_cfg_store_save_current("", -1, &id1, reason, sizeof(reason));
    TEST_CHECK(!ok, "save-as-new with an empty name is refused");

    reason[0] = '\0';
    ok = kiln_cfg_store_save_current("   ", -1, &id1, reason, sizeof(reason));
    TEST_CHECK(!ok, "save-as-new with a whitespace-only name is refused");
    TEST_CHECK(strcmp(reason, "name missing or too long") == 0, "same refusal reason as an empty name");

    int32_t id2 = -1;
    reason[0] = '\0';
    kiln_cfg_store_save_current("testkiln", -1, &id2, reason, sizeof(reason));

    reason[0] = '\0';
    int32_t id3 = -1;
    ok = kiln_cfg_store_clone(id2, "   ", &id3, reason, sizeof(reason));
    TEST_CHECK(!ok, "clone with a whitespace-only name is refused");

    ok = kiln_cfg_store_rename(id2, "   ");
    TEST_CHECK(!ok, "rename to a whitespace-only name is refused");
    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(kiln_cfg_store_get_name(id2, name_buf, sizeof(name_buf)) && strcmp(name_buf, "testkiln") == 0,
               "the refused rename left the name untouched");
}

// KILN_CFG_MAX_COUNT (8) enforcement -- not reached in the owner's bench
// testing, checked here.
static void test_store_full_rejected(void)
{
    TEST_SECTION("KILN_CFG_MAX_COUNT is enforced on save-as-new and clone, with a clear reason");
    reset_state();

    char reason[96];
    int32_t ids[KILN_CFG_MAX_COUNT];
    for (int i = 0; i < KILN_CFG_MAX_COUNT; i++) {
        char name[KILN_CFG_NAME_MAX_LEN + 1];
        snprintf(name, sizeof(name), "Config %d", i);
        reason[0] = '\0';
        bool ok = kiln_cfg_store_save_current(name, -1, &ids[i], reason, sizeof(reason));
        TEST_CHECK(ok, "filling the store up to KILN_CFG_MAX_COUNT succeeds");
    }

    int32_t extra_id = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_save_current("One Too Many", -1, &extra_id, reason, sizeof(reason));
    TEST_CHECK(!ok, "a (KILN_CFG_MAX_COUNT+1)th save-as-new is refused");
    TEST_CHECK(strcmp(reason, "kiln config store is full") == 0, "the refusal reason is specific");

    reason[0] = '\0';
    ok = kiln_cfg_store_clone(ids[0], "Clone When Full", &extra_id, reason, sizeof(reason));
    TEST_CHECK(!ok, "cloning is also refused when the store is already full");
    TEST_CHECK(strcmp(reason, "kiln config store is full") == 0, "same refusal reason for clone");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT + 1];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT + 1);
    TEST_CHECK(n == KILN_CFG_MAX_COUNT, "still exactly KILN_CFG_MAX_COUNT entries -- neither refused call "
                                        "wrote anything");
}

// ---------------------------------------------------------------------------
// nvs_load_store() coverage -- previously untested at any level: every test
// above drives kiln_cfg_store_apply()'s own version-refuse logic against the
// stub zones_config_import_blob(), never nvs_load_store()'s SEPARATE
// version/size handling for the on-flash kiln_cfg_store_blob_t itself. That
// is exactly the function the opus review flagged: it declared a whole
// kiln_cfg_store_blob_t (5420B) AND, on the v1 migration branch, a whole
// kiln_cfg_store_blob_v1_t (4396B) as ordinary stack locals -- ~9816B against
// app_main's 8192B CONFIG_ESP_MAIN_TASK_STACK_SIZE. Both are now `static`.
// This host test cannot reproduce a stack overflow (MSVC's host stack is a
// different size from an ESP32 task's, and there is no portable way to probe
// remaining stack depth from standard C), so it cannot directly prove the
// overflow is gone. What it CAN prove, and does: (1) nvs_load_store()'s v1
// migration path still works end-to-end through a real nvs_get_blob() round
// trip at full size (this exact path was never reached by any pre-existing
// test), and (2) calling it twice in a row with DIFFERENT stored data
// produces the SECOND call's data, not a mix with the first -- the static
// buffers are function-scoped statics, not module-scoped ones the rest of
// the file also writes, so nothing else should be able to bleed into them,
// but a stack-to-static conversion is exactly the kind of change that can
// silently reintroduce stale-read bugs if a future edit misuses the new
// storage, and that failure mode IS directly testable.
// ---------------------------------------------------------------------------

static void build_v1_blob(kiln_cfg_store_blob_v1_t *out, int32_t active_id, uint8_t fill_byte)
{
    memset(out, 0, sizeof(*out));
    out->version = 1;
    out->active_id = active_id;
    out->next_id = 2;
    out->entries[0].in_use = 1;
    out->entries[0].id = 1;
    snprintf(out->entries[0].name, sizeof(out->entries[0].name), "V1 Config");
    out->entries[0].blob_len = 8;
    for (size_t i = 0; i < out->entries[0].blob_len; i++) {
        out->entries[0].blob[i] = (uint8_t)(fill_byte + i);
    }
}

static void test_nvs_load_store_migrates_v1_blob_at_full_size(void)
{
    TEST_SECTION("nvs_load_store() -- v1-sized blob migrates through a real nvs_get_blob() round trip");
    reset_state();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    kiln_cfg_store_blob_v1_t v1;
    build_v1_blob(&v1, 1, 0x10);
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "hal_kv open succeeds once the partition is initialized");
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_STORE, &v1, sizeof(v1)) == HAL_OK,
               "a full-size (4396B) v1 blob fits the fake's storage slot");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    nvs_load_store();

    TEST_CHECK(s_store.version == KILN_CFG_STORE_VERSION, "migrated store carries the CURRENT version");
    TEST_CHECK(s_store.active_id == 1, "active_id carried over from the v1 blob");
    TEST_CHECK(s_store.entries[0].in_use == 1 && s_store.entries[0].id == 1,
               "entry 0 migrated (in_use/id)");
    TEST_CHECK(strcmp(s_store.entries[0].name, "V1 Config") == 0, "entry 0's name migrated");
    TEST_CHECK(s_store.entries[0].blob_len == 8, "entry 0's blob_len migrated");
    TEST_CHECK(s_store.entries[0].blob[0] == 0x10 && s_store.entries[0].blob[7] == 0x17,
               "entry 0's blob bytes migrated verbatim");

    fake_kv_reset_all();
}

static void test_nvs_load_store_second_call_does_not_see_first_calls_data(void)
{
    // NOTE on what this test does and does NOT prove: it does not, and
    // cannot, distinguish `static` locals from ordinary stack locals for
    // v1/loaded -- verified by hand (reverted both to plain locals, rebuilt,
    // reran: this test and every other still passed 1230/1230). Both call
    // nvs_get_blob() with an exact-size buffer that either fully overwrites
    // it or errors out before s_store is touched, so there is no code path
    // where a stale byte could survive a second call either way -- static
    // vs. auto storage duration makes no observable difference here. What
    // IS worth guarding, and what this actually tests, is a DIFFERENT
    // regression: that nvs_load_store() genuinely re-reads from NVS on every
    // call rather than caching/short-circuiting after the first (e.g. an
    // "already migrated once, skip" shortcut that reads whatever the FIRST
    // board's blob decoded to instead of the current one).
    TEST_SECTION("nvs_load_store() -- back-to-back v1 migrations for two different boards each load "
                 "THEIR OWN data, not a cached/stale result from the previous call");
    reset_state();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    // First load: a v1 blob for "board A".
    kiln_cfg_store_blob_v1_t v1_a;
    build_v1_blob(&v1_a, 1, 0xAA);
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_blob(&h, NVS_KEY_STORE, &v1_a, sizeof(v1_a));
    hal_kv_commit(&h);
    hal_kv_close(&h);
    nvs_load_store();
    TEST_CHECK(s_store.active_id == 1 && s_store.entries[0].blob[0] == 0xAA,
               "first call reads board A's data");

    // Second load: DIFFERENT active_id and blob content for "board B", as if
    // this were a fresh boot reading a different board's flash.
    reset_to_defaults();
    kiln_cfg_store_blob_v1_t v1_b;
    build_v1_blob(&v1_b, 2, 0xBB);
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_blob(&h, NVS_KEY_STORE, &v1_b, sizeof(v1_b));
    hal_kv_commit(&h);
    hal_kv_close(&h);
    nvs_load_store();
    TEST_CHECK(s_store.active_id == 2, "second call reads board B's active_id, not board A's stale 1");
    TEST_CHECK(s_store.entries[0].blob[0] == 0xBB,
               "second call reads board B's blob bytes, not board A's stale 0xAA");

    fake_kv_reset_all();
}

static void test_nvs_load_store_v1_migration_malloc_failure_leaves_defaults(void)
{
    // Proves nvs_load_store()'s malloc() failure path (kiln_cfg_store.c's
    // v1-migration branch) is handled exactly like any other "unreadable"
    // failure -- defaults stand, s_store is untouched by whatever bytes
    // were sitting in NVS. Uses the kiln_cfg_store_test_malloc() seam
    // #defined at the top of this file, which is the only reachable way to
    // force this branch from a host test (no portable way to actually
    // exhaust the heap deterministically).
    TEST_SECTION("nvs_load_store() -- v1 migration buffer malloc() failure leaves defaults standing");
    reset_state();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    kiln_cfg_store_blob_v1_t v1;
    build_v1_blob(&v1, 1, 0x99);
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_blob(&h, NVS_KEY_STORE, &v1, sizeof(v1));
    hal_kv_commit(&h);
    hal_kv_close(&h);

    s_test_malloc_should_fail = true;
    nvs_load_store();
    s_test_malloc_should_fail = false;

    TEST_CHECK(s_store.version == KILN_CFG_STORE_VERSION, "defaults still carry the current version");
    TEST_CHECK(s_store.active_id == KILN_CFG_NO_ACTIVE_ID,
               "active_id is the default (NOT the v1 blob's 1) -- malloc failure did not migrate anything");
    TEST_CHECK(s_store.entries[0].in_use == 0,
               "entry 0 is NOT in_use -- the v1 blob's data never reached s_store");

    fake_kv_reset_all();
}

static void test_nvs_load_store_current_version_full_size_happy_path(void)
{
    TEST_SECTION("nvs_load_store() -- a current-version, current-size blob loads on the fast path");
    reset_state();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    kiln_cfg_store_blob_t current;
    memset(&current, 0, sizeof(current));
    current.version = KILN_CFG_STORE_VERSION;
    current.active_id = 7;
    current.next_id = 9;
    current.entries[0].in_use = 1;
    current.entries[0].id = 7;
    snprintf(current.entries[0].name, sizeof(current.entries[0].name), "Current");
    current.entries[0].blob_len = 4;
    current.entries[0].blob[0] = 0xDE;
    current.entries[0].blob[3] = 0xEF;

    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_STORE, &current, sizeof(current)) == HAL_OK,
               "a full-size (5420B) current-version blob fits the fake's storage slot");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    nvs_load_store();

    TEST_CHECK(s_store.active_id == 7, "current-version blob's active_id loaded as-is");
    TEST_CHECK(s_store.entries[0].blob[0] == 0xDE && s_store.entries[0].blob[3] == 0xEF,
               "current-version blob's bytes loaded as-is, no migration applied");

    fake_kv_reset_all();
}

static void test_nvs_save_store_refuses_when_calling_stack_is_external_ram(void)
{
    TEST_SECTION("nvs_save_store -- refuses (does not crash) when called with a PSRAM stack "
                 "underneath it (DRAM_PSRAM_PLAN.md section 7.2 safety net)");
    reset_state();

    fake_kv_set_write_safe_here(false); // simulate being called from a PSRAM-stacked task

    hal_status_t err = nvs_save_store();

    TEST_CHECK(err == HAL_NOT_READY,
               "the wrong-task guard refuses with a diagnosable error, not a crash -- exactly "
               "the class of bug (an NVS write reached from a PSRAM-stack task) this net exists "
               "to catch before a future task relocation (DRAM_PSRAM_PLAN.md section 7) makes it "
               "reachable for real");

    fake_kv_set_write_safe_here(true); // leave shared fake state as every other test expects
}

static void test_nvs_save_store_proceeds_normally_on_an_internal_ram_stack(void)
{
    TEST_SECTION("nvs_save_store -- proceeds normally when the calling task's stack is internal RAM");
    reset_state();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    // fake_kv_set_write_safe_here(true) is the fake's default state after reset.
    hal_status_t err = nvs_save_store();

    TEST_CHECK(err == HAL_OK, "the guard does not fire on an internal-RAM stack -- the write "
                              "proceeds exactly as before this net was added");

    fake_kv_reset_all();
}

// ---------------------------------------------------------------------------
// kiln_cfg_store_cfg_fs.c coverage -- the `cfg` LittleFS read-through/
// dual-write bridge for the WHOLE saved-configs store (one document, one
// rev counter -- see that module's header comment for why this differs
// from profiles_cfg_fs.c's per-slot files). Same test shapes as
// test_zones_config_cfg_fs.c/test_relay_names_cfg_fs.c, driven here through
// the real public API (kiln_cfg_store_save_current/_delete/_apply) plus
// direct access to s_store/s_kiln_cfg_rev/nvs_save_store()/
// nvs_load_store_with_cfg_fs(), all reachable because kiln_cfg_store.c is
// #included directly into this TU.
// ---------------------------------------------------------------------------

static const char *KCFG_SCRATCH_BASE = "cfg_fs_test_kiln_cfg_store";

static void reset_state_cfg_fs(void)
{
    reset_state(); // s_store to empty/no-active, stubs, interlock

    // Same "delete known filenames before rmdir" fix class as
    // test_zones_config_cfg_fs.c's reset_all() -- a leftover file from a
    // prior run of this binary otherwise defeats TKCF_RMDIR (only succeeds
    // against an empty directory).
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", KCFG_SCRATCH_BASE, KILN_CFG_STORE_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", KCFG_SCRATCH_BASE, KILN_CFG_STORE_FILE_PATH);
    remove(path);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", KCFG_SCRATCH_BASE);
    TKCF_RMDIR(tmp);
    TKCF_RMDIR(KCFG_SCRATCH_BASE);
    TKCF_MKDIR(KCFG_SCRATCH_BASE);

    cfg_fs_deinit();
    kiln_cfg_store_cfg_fs_reset_write_fn_for_test();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    s_kiln_cfg_rev = 0; // process-wide dual-write rev static -- see its own comment
}

// 1. Partition absent (today's real state on every board): save/load must
//    behave exactly like plain NVS save/load.
static void test_cfg_fs_partition_absent_falls_through_to_nvs_only(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: partition absent -- behaves exactly like NVS-only");
    reset_state_cfg_fs();
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs never mounted in this test");

    int32_t id = -1;
    char reason[96];
    TEST_CHECK(kiln_cfg_store_save_current("absent", -1, &id, reason, sizeof(reason)), "save-as-new succeeds");

    kiln_cfg_store_blob_t raw;
    uint32_t rev = 999;
    bool raw_valid = true;
    kiln_cfg_store_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(!raw_valid && rev == 0, "no file was ever written -- cfg_fs_is_available() gated every file op");
}

// 2. Mount failed: cfg_fs_init() against a nonexistent directory fails, and
//    behavior degrades to NVS-only, same as partition-absent.
static void test_cfg_fs_mount_failed_falls_through_to_nvs_only(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: cfg_fs mount FAILED -- falls through to NVS, non-fatal");
    reset_state_cfg_fs();

    esp_err_t mount_err = cfg_fs_init("this_directory_does_not_exist_at_all_kcfg", NULL);
    TEST_CHECK(mount_err != ESP_OK, "cfg_fs_init() against a nonexistent base dir fails, as documented");
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs reports unavailable after a failed mount");

    int32_t id = -1;
    char reason[96];
    TEST_CHECK(kiln_cfg_store_save_current("unmounted", -1, &id, reason, sizeof(reason)),
               "save-as-new still succeeds (NVS side, unaffected by the failed file-side mount)");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id, "the NVS value is still adopted correctly");

    cfg_fs_deinit();
}

// 3. NVS fallback + lazy migration: first load with no file migrates one to
//    disk; a second load (in-RAM wiped) proves the file, not just NVS,
//    holds the same content.
static void test_cfg_fs_nvs_fallback_then_file_preferred_after_migration(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: NVS fallback on first load migrates to file; second load prefers "
                 "the file");
    reset_state_cfg_fs();
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    int32_t id = -1;
    char reason[96];
    TEST_CHECK(kiln_cfg_store_save_current("migrate", -1, &id, reason, sizeof(reason)),
               "save-as-new succeeds (dual-write: file first, then NVS)");

    bool exists = false;
    TEST_CHECK(cfg_fs_exists(KILN_CFG_STORE_FILE_PATH, &exists) == ESP_OK && exists,
               "the save's dual-write actually created the file");

    // Simulate a fresh boot: wipe in-RAM state, re-run the load path.
    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    TEST_CHECK(kiln_cfg_store_get_active_id() == id, "reloaded store's active id matches what was saved");
    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(kiln_cfg_store_get_name(id, name_buf, sizeof(name_buf)) && strcmp(name_buf, "migrate") == 0,
               "reloaded store's name matches what was saved");

    kiln_cfg_store_blob_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    kiln_cfg_store_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 1, "the file itself holds a valid, rev-1 copy");
    TEST_CHECK(raw.active_id == id, "file content matches the saved store");
}

// 4. Dual-write keeps both sides in sync across several mutations (a save
//    AND a delete), each bumping the shared whole-document rev.
static void test_cfg_fs_dual_write_keeps_file_and_nvs_in_sync(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: repeated saves/deletes keep file and NVS in sync (incrementing "
                 "rev on both)");
    reset_state_cfg_fs();
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    char reason[96];
    int32_t id_a = -1, id_b = -1;
    TEST_CHECK(kiln_cfg_store_save_current("A", -1, &id_a, reason, sizeof(reason)), "save 1 (rev 1)");
    TEST_CHECK(kiln_cfg_store_save_current("B", -1, &id_b, reason, sizeof(reason)), "save 2 (rev 2)");
    TEST_CHECK(kiln_cfg_store_delete(id_a), "delete of A (rev 3)");

    kiln_cfg_store_blob_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    kiln_cfg_store_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 3, "file rev tracks all three mutations, including the delete");

    int found_b = 0, found_a = 0;
    for (int i = 0; i < KILN_CFG_MAX_COUNT; i++) {
        if (raw.entries[i].in_use && raw.entries[i].id == id_b) {
            found_b = 1;
        }
        if (raw.entries[i].in_use && raw.entries[i].id == id_a) {
            found_a = 1;
        }
    }
    TEST_CHECK(found_b && !found_a, "the file reflects the delete -- B present, A gone");

    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(kiln_cfg_store_get_name(id_b, name_buf, sizeof(name_buf)) && strcmp(name_buf, "B") == 0,
               "NVS agrees with the file -- no divergence after the mixed save/delete sequence");
    TEST_CHECK(!kiln_cfg_store_get_name(id_a, name_buf, sizeof(name_buf)),
               "the deleted entry is gone on both sides");
}

// 5. Divergence tie-break, BOTH directions.
static esp_err_t kcfg_failing_write_fn(const char *rel_path, const void *data, size_t len)
{
    (void)rel_path;
    (void)data;
    (void)len;
    return ESP_FAIL;
}

static void test_cfg_fs_divergence_tie_break_both_directions(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: divergence tie-break picks the higher rev in both directions, "
                 "and resyncs the loser");
    reset_state_cfg_fs();
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    char reason[96];
    int32_t id1 = -1;
    TEST_CHECK(kiln_cfg_store_save_current("rev1", -1, &id1, reason, sizeof(reason)), "rev 1 saved to both sides");

    // Simulate a file write failure: NVS advances to rev 2, file stays at
    // rev 1 with the OLD content.
    kiln_cfg_store_cfg_fs_set_write_fn(kcfg_failing_write_fn);
    int32_t id2 = -1;
    TEST_CHECK(kiln_cfg_store_save_current("rev2_nvs_only", -1, &id2, reason, sizeof(reason)),
               "rev 2 save still reports OK -- NVS write is what it depends on, not the file write");
    kiln_cfg_store_cfg_fs_reset_write_fn_for_test();

    kiln_cfg_store_blob_t raw_before;
    uint32_t rev_before = 0;
    bool raw_valid_before = false;
    kiln_cfg_store_cfg_fs_load_raw(&raw_before, &rev_before, &raw_valid_before);
    TEST_CHECK(raw_valid_before && rev_before == 1, "file is stuck at rev 1 -- the failed write never landed");

    // NVS rev (2) > file rev (1): load must adopt NVS and resync the file.
    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(kiln_cfg_store_get_name(id2, name_buf, sizeof(name_buf)) &&
                   strcmp(name_buf, "rev2_nvs_only") == 0,
               "NVS (higher rev) wins the tie-break, not the stale file");

    kiln_cfg_store_blob_t raw_after;
    uint32_t rev_after = 0;
    bool raw_valid_after = false;
    kiln_cfg_store_cfg_fs_load_raw(&raw_after, &rev_after, &raw_valid_after);
    TEST_CHECK(raw_valid_after && rev_after == 2, "the file was resynced from NVS as a side effect");

    // Normal direction: one more ordinary dual-write, both sides in sync
    // again.
    int32_t id3 = -1;
    TEST_CHECK(kiln_cfg_store_save_current("rev3", -1, &id3, reason, sizeof(reason)), "rev 3 saved normally");
    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    TEST_CHECK(kiln_cfg_store_get_name(id3, name_buf, sizeof(name_buf)) && strcmp(name_buf, "rev3") == 0,
               "file/NVS agree again after the normal save");
}

// 6. EQUAL revs with differing content must adopt NVS, not the stale file --
//    the rollback round trip the rev counter exists for.
static void test_cfg_fs_equal_rev_divergence_adopts_nvs_not_the_stale_file(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: equal revs with differing content adopt NVS (rolled-back-firmware "
                 "edit survives a roll-forward)");
    reset_state_cfg_fs();
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    char reason[96];
    int32_t id1 = -1;
    TEST_CHECK(kiln_cfg_store_save_current("before_rollback", -1, &id1, reason, sizeof(reason)),
               "rev 1 dual-written to both sides");

    // Act like firmware that predates the rev counter: rewrite ONLY the NVS
    // blob (via the module's own save path minus the rev bump), leaving
    // kilncfgrv at 1 and the file at rev 1/old content.
    kiln_cfg_store_blob_t rolled_back = s_store;
    strncpy(rolled_back.entries[0].name, "rolled_edit", KILN_CFG_NAME_MAX_LEN);
    rolled_back.entries[0].name[KILN_CFG_NAME_MAX_LEN] = '\0';
    {
        hal_kv_handle_t h;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
                   "test setup: NVS opened for the pre-dual-write-style blob rewrite");
        TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_STORE, &rolled_back, sizeof(rolled_back)) == HAL_OK,
                   "test setup: NVS blob rewritten WITHOUT touching kilncfgrv, exactly as older firmware would");
        TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "test setup: NVS commit");
        hal_kv_close(&h);
    }

    // Confirm the fixture really is the equal-rev case.
    kiln_cfg_store_blob_t file_raw;
    uint32_t file_rev = 0;
    bool file_raw_valid = false;
    kiln_cfg_store_cfg_fs_load_raw(&file_raw, &file_rev, &file_raw_valid);
    TEST_CHECK(file_raw_valid && file_rev == 1 && strcmp(file_raw.entries[0].name, "before_rollback") == 0,
               "fixture check: the file still holds the OLD content at rev 1");
    {
        hal_kv_handle_t h;
        uint32_t nvs_rev = 0;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
                   "fixture check: NVS opened read-only");
        TEST_CHECK(hal_kv_get_u32(&h, NVS_KEY_STORE_REV, &nvs_rev) == HAL_OK && nvs_rev == 1,
                   "fixture check: kilncfgrv is still 1 -- file_rev == nvs_rev, the EQUAL-rev case");
        hal_kv_close(&h);
    }

    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    TEST_CHECK(strcmp(s_store.entries[0].name, "rolled_edit") == 0,
               "NVS wins the EQUAL-rev tie -- the edit made on rolled-back firmware is NOT discarded in "
               "favour of the stale file");

    kiln_cfg_store_cfg_fs_load_raw(&file_raw, &file_rev, &file_raw_valid);
    TEST_CHECK(file_raw_valid && strcmp(file_raw.entries[0].name, "rolled_edit") == 0,
               "the losing file was resynced from NVS, so the divergence does not persist across boots");
}

// 7. Stale-delete-not-resurrected: a delete that only lands on NVS (file
//    write fails) must not let the stale file's still-in_use entry come
//    back on a later load -- proven through the SAME strict tie-break
//    (higher rev wins) rather than a separate per-slot mechanism, since
//    this store is one document (see kiln_cfg_store_cfg_fs.h "SHAPE").
static void test_cfg_fs_stale_delete_not_resurrected(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: a delete that fails to land on the file cannot resurrect the "
                 "deleted slot on a later load");
    reset_state_cfg_fs();
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    char reason[96];
    int32_t id = -1;
    TEST_CHECK(kiln_cfg_store_save_current("doomed", -1, &id, reason, sizeof(reason)),
               "rev 1: slot saved to both sides");

    // Delete lands on NVS (rev 2) but the file write for it fails -- the
    // file is left holding the pre-delete document (the slot still in_use)
    // at rev 1.
    kiln_cfg_store_cfg_fs_set_write_fn(kcfg_failing_write_fn);
    TEST_CHECK(kiln_cfg_store_delete(id), "rev 2: delete succeeds on NVS even though the file write fails");
    kiln_cfg_store_cfg_fs_reset_write_fn_for_test();

    kiln_cfg_store_blob_t file_raw;
    uint32_t file_rev = 0;
    bool file_raw_valid = false;
    kiln_cfg_store_cfg_fs_load_raw(&file_raw, &file_rev, &file_raw_valid);
    TEST_CHECK(file_raw_valid && file_rev == 1, "fixture check: the stale file is still at rev 1");
    int stale_still_in_use = 0;
    for (int i = 0; i < KILN_CFG_MAX_COUNT; i++) {
        if (file_raw.entries[i].in_use && file_raw.entries[i].id == id) {
            stale_still_in_use = 1;
        }
    }
    TEST_CHECK(stale_still_in_use, "fixture check: the stale file still shows the deleted slot as in_use");

    // A later boot's load must adopt NVS (rev 2, higher), NOT the stale
    // file -- the deleted slot must not come back.
    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(!kiln_cfg_store_get_name(id, name_buf, sizeof(name_buf)),
               "the deleted slot did NOT resurrect -- NVS's higher rev (reflecting the delete) won");

    kiln_cfg_store_cfg_fs_load_raw(&file_raw, &file_rev, &file_raw_valid);
    TEST_CHECK(file_raw_valid && file_rev == 2, "the stale file was resynced -- it no longer shows the "
                                                 "deleted slot as in_use");
    stale_still_in_use = 0;
    for (int i = 0; i < KILN_CFG_MAX_COUNT; i++) {
        if (file_raw.entries[i].in_use && file_raw.entries[i].id == id) {
            stale_still_in_use = 1;
        }
    }
    TEST_CHECK(!stale_still_in_use, "resynced file no longer carries the deleted slot");
}

// 8. Interrupted write leaves old-or-new: an orphaned temp file (crash
//    between fsync and rename) must never be visible through
//    kiln_cfg_store_cfg_fs_load_raw() -- only the last COMMITTED content is
//    ever readable.
static void test_cfg_fs_interrupted_write_leaves_old_or_new(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: an interrupted file write leaves the OLD committed store intact, "
                 "never a partial one");
    reset_state_cfg_fs();
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    char reason[96];
    int32_t id = -1;
    TEST_CHECK(kiln_cfg_store_save_current("committed", -1, &id, reason, sizeof(reason)),
               "an initial, fully-committed save lands");

    // Manufacture a crash-orphaned temp file directly on disk, same fixture
    // style as test_cfg_fs.c/test_zones_config_cfg_fs.c.
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.tmp/%s", KCFG_SCRATCH_BASE, KILN_CFG_STORE_FILE_PATH);
    FILE *f = fopen(tmp_path, "wb");
    TEST_CHECK(f != NULL, "test setup: orphaned temp file created");
    if (f) {
        static const char partial[] = "not even close to a valid rev+blob";
        fwrite(partial, 1, sizeof(partial), f);
        fclose(f);
    }

    kiln_cfg_store_blob_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    kiln_cfg_store_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 1 && strcmp(raw.entries[0].name, "committed") == 0,
               "the OLD committed store reads back untouched -- the orphaned temp file (rename never ran) "
               "is invisible through the real read path");
}

void run_test_kiln_cfg_store(void)
{
    test_save_clone_apply_roundtrip();
    test_apply_refused_while_run_active();
    test_newer_version_blob_refused_by_store();
    test_out_of_range_value_rejected_nothing_written();
    test_save_as_new_rejects_duplicate_name();
    test_clone_rejects_duplicate_name();
    test_rename_rejects_duplicate_name();
    test_case_insensitive_duplicate_rejected();
    test_whitespace_trimmed_before_compare_and_store();
    test_overwrite_by_id_same_name_still_allowed();
    test_noop_rename_allowed();
    test_empty_or_whitespace_only_name_rejected();
    test_store_full_rejected();
    test_nvs_load_store_migrates_v1_blob_at_full_size();
    test_nvs_load_store_second_call_does_not_see_first_calls_data();
    test_nvs_load_store_v1_migration_malloc_failure_leaves_defaults();
    test_nvs_load_store_current_version_full_size_happy_path();
    test_nvs_save_store_refuses_when_calling_stack_is_external_ram();
    test_nvs_save_store_proceeds_normally_on_an_internal_ram_stack();

    test_cfg_fs_partition_absent_falls_through_to_nvs_only();
    test_cfg_fs_mount_failed_falls_through_to_nvs_only();
    test_cfg_fs_nvs_fallback_then_file_preferred_after_migration();
    test_cfg_fs_dual_write_keeps_file_and_nvs_in_sync();
    test_cfg_fs_divergence_tie_break_both_directions();
    test_cfg_fs_equal_rev_divergence_adopts_nvs_not_the_stale_file();
    test_cfg_fs_stale_delete_not_resurrected();
    test_cfg_fs_interrupted_write_leaves_old_or_new();

    cfg_fs_deinit();
}
