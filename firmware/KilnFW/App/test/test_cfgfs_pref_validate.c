// Host test for cfgfs_file_validate.c's flat-pref-file table (POST
// /api/cfgfs/file). Linked into the zones_http executable. The owning
// modules are not linked there, so their validators are replaced by the
// stubs below (size must be STUB_ITEM bytes, first byte == that row's marker); what this
// proves is the dispatch: right name -> validator, 4-byte rev stripped,
// short body refused, and raw=1 never waiving. The real validator rules are
// exercised by each module's own test, which passes the same renamed
// function to pref_cfg_fs_load_raw(). ct_verify.bin uses the real
// ct_verify_blob_validate() (that module is linked here); iter_tune.bin uses
// a stub of iter_tune_store_blob_validate().
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "cfgfs_file_validate.h"
#include "cfgfs_file_validators.h"
#include "iter_tune_store.h"

#define STUB_ITEM 6u
/* Each stub accepts only its own marker byte (item byte 0), so swapping two
 * rows' validators in PREF_FILE_RULES makes that row's valid body fail.
 * aux_out.dat's marker is 1 because the POST rule refuses item byte 0 > 1
 * (a newer blob version) before calling the validator. */
static bool stub(const void *b, size_t len, uint8_t marker)
{
    return len == STUB_ITEM && ((const uint8_t *)b)[0] == marker;
}
bool adaptive_tune_kibase_file_validate(const void *b, size_t l) { return stub(b, l, 2); }
bool ramp_assist_cfg_file_validate(const void *b, size_t l) { return stub(b, l, 3); }
bool time_sync_tz_file_validate(const void *b, size_t l) { return stub(b, l, 4); }
/* Accepts a "newer version" byte (2) like the real loader validator, so only the POST wrapper can refuse it. */
bool aux_outputs_cfg_file_validate(const void *b, size_t l) { return stub(b, l, 1) || stub(b, l, 2); }
bool display_power_cfg_file_validate(const void *b, size_t l) { return stub(b, l, 5); }
bool profiles_builtin_hidden_file_validate(const void *b, size_t l) { return stub(b, l, 11); }
bool profiles_favorites_file_validate(const void *b, size_t l) { return stub(b, l, 6); }
bool relay_cycles_file_validate(const void *b, size_t l) { return stub(b, l, 7); }
bool setup_wizard_progress_file_validate(const void *b, size_t l) { return stub(b, l, 8); }
bool update_settings_file_validate(const void *b, size_t l) { return stub(b, l, 9); }
bool iter_tune_store_blob_validate(const void *b, size_t l) { return stub(b, l, 10); }

void run_test_cfgfs_pref_validate(void)
{
    TEST_SECTION("cfgfs pref files: validated by the loader's validator, raw=1 does not waive");
    static const struct {
        const char *name;
        uint8_t marker;
    } rows[] = {
        { "ki_base.dat", 2 }, { "ramp_assist.dat", 3 }, { "tz.dat", 4 }, { "aux_out.dat", 1 },
        { "display_power.dat", 5 }, { "prof_fav.bin", 6 }, { "relay_cycles.dat", 7 },
        { "setup_wiz.bin", 8 }, { "update_repo.dat", 9 }, { "iter_tune.bin", 10 },
    };
    const char *why = "";
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const char *n = rows[i].name;
        uint8_t good[4 + STUB_ITEM] = { 1, 0, 0, 0, rows[i].marker, 2, 3, 4, 5, 6 };
        uint8_t bad[4 + STUB_ITEM];
        memcpy(bad, good, sizeof(good));
        bad[4] = 0xEE;
        TEST_CHECK(cfgfs_file_check_write(n, false, good, sizeof(good), &why) == CFGFS_FILE_CHECK_OK, n);
        TEST_CHECK(cfgfs_file_check_write(n, true, good, sizeof(good), &why) == CFGFS_FILE_CHECK_OK, n);
        TEST_CHECK(cfgfs_file_check_write(n, false, bad, sizeof(bad), &why) == CFGFS_FILE_CHECK_INVALID, n);
        TEST_CHECK(cfgfs_file_check_write(n, true, bad, sizeof(bad), &why) == CFGFS_FILE_CHECK_INVALID,
                   "raw=1 does not waive the validator");
        TEST_CHECK(cfgfs_file_check_write(n, true, good, 3, &why) == CFGFS_FILE_CHECK_INVALID,
                   "body shorter than the rev prefix refused");
        TEST_CHECK(cfgfs_file_check_write(n, false, good, sizeof(good) - 1, &why) == CFGFS_FILE_CHECK_INVALID,
                   "wrong-size item refused");
        /* Every OTHER row's valid body must be refused here: catches swapped validators. */
        for (size_t j = 0; j < sizeof(rows) / sizeof(rows[0]); j++) {
            if (j == i) {
                continue;
            }
            uint8_t other[4 + STUB_ITEM];
            memcpy(other, good, sizeof(good));
            other[4] = rows[j].marker;
            TEST_CHECK(cfgfs_file_check_write(n, true, other, sizeof(other), &why) == CFGFS_FILE_CHECK_INVALID,
                       "another row's valid body refused (validator not swapped)");
        }
    }
    /* aux_out.dat: a NEWER blob version is refused by the POST rule even though
     * the loader's validator would accept it (it would quarantine aux at boot). */
    {
        uint8_t newer[4 + STUB_ITEM] = { 1, 0, 0, 0, 2, 2, 3, 4, 5, 6 };
        TEST_CHECK(cfgfs_file_check_write("aux_out.dat", true, newer, sizeof(newer), &why) == CFGFS_FILE_CHECK_INVALID,
                   "aux_out.dat newer version refused");
        TEST_CHECK(cfgfs_file_check_write("aux_out.dat", false, newer, sizeof(newer), &why) == CFGFS_FILE_CHECK_INVALID,
                   "aux_out.dat newer version refused (raw=0)");
    }
    /* Subdirectory files are out of scope (cfgfs_file_name_get refuses '/'): no row. */
    {
        uint8_t g[4 + STUB_ITEM] = { 1, 0, 0, 0, 11, 2, 3, 4, 5, 6 };
        TEST_CHECK(cfgfs_file_check_write("profiles/hidden.json", false, g, sizeof(g), &why) == CFGFS_FILE_CHECK_NO_VALIDATOR,
                   "profiles/hidden.json has no row");
    }
    uint8_t good[4 + STUB_ITEM] = { 1, 0, 0, 0, 1, 2, 3, 4, 5, 6 };
    /* Files that still have no validator here stay raw-only. */
    TEST_CHECK(cfgfs_file_check_write("unit_pref.dat", false, good, sizeof(good), &why) == CFGFS_FILE_CHECK_NO_VALIDATOR,
               "unit_pref.dat remains raw-only");
    TEST_CHECK(cfgfs_file_check_write("unit_pref.dat", true, good, sizeof(good), &why) == CFGFS_FILE_CHECK_OK,
               "unit_pref.dat raw=1 accepted");
    /* ct_verify.bin: real validator. */
    TEST_CHECK(cfgfs_file_check_write("ct_verify.bin", false, good, sizeof(good), &why) == CFGFS_FILE_CHECK_INVALID,
               "ct_verify.bin garbage refused");
    TEST_CHECK(cfgfs_file_check_write("ct_verify.bin", true, good, sizeof(good), &why) == CFGFS_FILE_CHECK_INVALID,
               "ct_verify.bin raw=1 does not waive");
}
