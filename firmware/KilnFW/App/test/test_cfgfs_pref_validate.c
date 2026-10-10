// Host test for cfgfs_file_validate.c's flat-pref-file table (POST
// /api/cfgfs/file). Linked into the zones_http executable. The ten owning
// modules are not linked there, so their validators are replaced by the
// stubs below (size must be STUB_ITEM bytes, first byte != 0xEE); what this
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
static bool stub(const void *b, size_t len)
{
    return len == STUB_ITEM && ((const uint8_t *)b)[0] != 0xEE;
}
bool adaptive_tune_kibase_file_validate(const void *b, size_t l) { return stub(b, l); }
bool ramp_assist_cfg_file_validate(const void *b, size_t l) { return stub(b, l); }
bool time_sync_tz_file_validate(const void *b, size_t l) { return stub(b, l); }
bool aux_outputs_cfg_file_validate(const void *b, size_t l) { return stub(b, l); }
bool display_power_cfg_file_validate(const void *b, size_t l) { return stub(b, l); }
bool profiles_builtin_hidden_file_validate(const void *b, size_t l) { return stub(b, l); }
bool profiles_favorites_file_validate(const void *b, size_t l) { return stub(b, l); }
bool relay_cycles_file_validate(const void *b, size_t l) { return stub(b, l); }
bool setup_wizard_progress_file_validate(const void *b, size_t l) { return stub(b, l); }
bool update_settings_file_validate(const void *b, size_t l) { return stub(b, l); }
bool iter_tune_store_blob_validate(const void *b, size_t l) { return stub(b, l); }

void run_test_cfgfs_pref_validate(void)
{
    TEST_SECTION("cfgfs pref files: validated by the loader's validator, raw=1 does not waive");
    static const char *const names[] = {
        "ki_base.dat", "ramp_assist.dat", "tz.dat", "aux_out.dat", "display_power.dat",
        "profiles/hidden.json", "prof_fav.bin", "relay_cycles.dat", "setup_wiz.bin",
        "update_repo.dat", "iter_tune.bin",
    };
    uint8_t good[4 + STUB_ITEM] = { 1, 0, 0, 0, 1, 2, 3, 4, 5, 6 };
    uint8_t bad[4 + STUB_ITEM];
    memcpy(bad, good, sizeof(good));
    bad[4] = 0xEE;
    const char *why = "";
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *n = names[i];
        TEST_CHECK(cfgfs_file_check_write(n, false, good, sizeof(good), &why) == CFGFS_FILE_CHECK_OK, n);
        TEST_CHECK(cfgfs_file_check_write(n, true, good, sizeof(good), &why) == CFGFS_FILE_CHECK_OK, n);
        TEST_CHECK(cfgfs_file_check_write(n, false, bad, sizeof(bad), &why) == CFGFS_FILE_CHECK_INVALID, n);
        TEST_CHECK(cfgfs_file_check_write(n, true, bad, sizeof(bad), &why) == CFGFS_FILE_CHECK_INVALID,
                   "raw=1 does not waive the validator");
        TEST_CHECK(cfgfs_file_check_write(n, true, good, 3, &why) == CFGFS_FILE_CHECK_INVALID,
                   "body shorter than the rev prefix refused");
        TEST_CHECK(cfgfs_file_check_write(n, false, good, sizeof(good) - 1, &why) == CFGFS_FILE_CHECK_INVALID,
                   "wrong-size item refused");
    }
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
