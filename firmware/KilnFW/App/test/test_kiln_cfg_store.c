// Host tests for App/drivers/kiln_cfg_store.c -- the saved "kiln config"
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
#include <string.h>

#include "test_common.h"

#include "esp_err.h"

#include "../drivers/kiln_cfg_store.c"

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
}
