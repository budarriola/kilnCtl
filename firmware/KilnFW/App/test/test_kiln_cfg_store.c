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
// of the OTHER ota_http.h functions (ota_http_start(), the update-mutex
// functions), so only ota_http_check_interlocks() needs a body -- the rest
// are declarations only, never referenced, and need none.
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

// docs/WEB_AUTH_PLAN.md item 12b -- web_auth_store.c (linked for real, see
// build_host_tests.ps1's comment on this executable's $sources entry) so the
// config-package export/import test below can seed a real credential and
// prove it survives kiln_cfg_store_export_package_json()/_import_package_json()
// untouched. g_stub_psa_import_key_result is already defined once for this
// whole executable in test_backup_import.c.
#include "web_auth_store.h"

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
#include "../drivers/ui/ui_page_home_rail.h"

// Shared mutable stub state, defined in test_profile_feasibility.c and
// declared extern (ad-hoc, same convention as test_backup_import.c) so this
// TU can pin a known live_thermo_count before an import that must pass the
// 5.2a hardware-compatibility check.
extern void test_stub_zones_set_thermo_count(uint8_t n);

// ---------------------------------------------------------------------------
// zones_http.h stub state -- controllable export/import behavior.
// ---------------------------------------------------------------------------

static size_t s_stub_blob_size = 16;
static bool s_stub_export_ok = true;
static uint8_t s_stub_export_content[ZONES_CONFIG_BLOB_MAX_SIZE];

// Cross-file test hook (same convention as test_stub_zones_set_thermo_count(),
// defined in test_profile_feasibility.c and declared extern by every TU that
// needs it): flips one byte of the "live" export content this stub's
// zones_config_export_blob() returns, so a caller in another translation
// unit (test_backup_import.c) can make two kiln_cfg_store_save_current()
// calls produce genuinely DIFFERENT identities (schema/hash), without this
// file's own s_stub_export_content being reachable across TUs directly (it
// has internal linkage on purpose -- see this section's header comment).
void test_stub_kiln_cfg_export_content_toggle_byte0(void)
{
    s_stub_export_content[0] ^= 0xFF;
}

static bool s_stub_import_result = true;
static char s_stub_import_reason[96] = "";
static int s_stub_import_call_count = 0;
static uint8_t s_stub_import_last_bytes[ZONES_CONFIG_BLOB_MAX_SIZE];
static size_t s_stub_import_last_len = 0;

// Adversarial review 2026-09-15 (docs/audits/review_autosave_slot_fix_
// a93ee77b_2026-09-15.md, MEDIUM finding 4): production's real nvs_save()
// dispatches an autosave synchronously from INSIDE zones_config_import_
// blob() (via the flash worker, which blocks the caller) -- this stub does
// not model that by default (see H1's own reasoning above for why this file
// avoids linking the real zones_http.c). When a test needs to exercise the
// REAL kiln_cfg_store_autosave_from_live() branch threaded through a REAL
// kiln_cfg_store_apply()/kiln_cfg_swap.c call (not a fake setter, per this
// finding), it sets this flag so the stub calls the real
// kiln_cfg_store_autosave_from_live() itself, at the same point in the call
// sequence production does: while the override that call set is still live
// and active_id has not yet moved.
static bool s_stub_autosave_during_import = false;

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
    if (s_stub_import_result && s_stub_autosave_during_import) {
        // Model production's real nvs_save()->autosave dispatch, which fires
        // synchronously from inside this call, before the caller's next
        // statement (active_id = id) ever runs. The live "export" content is
        // whatever the caller already set up as s_stub_export_content --
        // real firmware's live zones config is genuinely the imported bytes
        // by this point, since zones_config_import_blob() already committed
        // them to RAM before nvs_save() dispatches.
        // Carries the DISPATCHING TASK's handle, exactly as production's
        // zones_config_store.c nvs_save() now does via zones_autosave_job()'s
        // `arg` -- the import's OWN autosave is the one case that legitimately
        // honors the autosave target override (2026-09-16 cross-task fix).
        char sub[96] = {0};
        kiln_cfg_store_autosave_from_live_for_dispatcher((void *)xTaskGetCurrentTaskHandle(), sub,
                                                         sizeof(sub));
    }
    return s_stub_import_result;
}

// H1 (docs/audits/kiln_profiles_robustness_2026-09-14.md,
// docs/audits/kiln_package_canonical_serializer_2026-09-14.md):
// populate_pico_half_and_hash() now feeds kiln_package_compute_hash()'s ESP
// half through zones_config_export_canonical() instead of the raw blob --
// this file stubs that function the same way it already stubs
// zones_config_export_blob() above, rather than linking the real
// zones_config_accessors.c (which would drag in the whole zones_http.c
// hardware-owning surface this file's own header comment explains it
// deliberately avoids). Passed `cfg` here is populate_pico_half_and_hash()'s
// own `scratch_cfg` -- a zeroed zones_cfg_t with s_stub_export_content's
// bytes copied over its front s_stub_blob_size bytes (see
// kiln_cfg_store.c's own save-path code) -- so copying its first N raw bytes
// out preserves this file's existing "a different ESP blob content produces
// a different pkg_hash" contract (test_kiln_cfg_store_save_current_
// captures_pico_and_hash below) without this stub needing to know
// zones_cfg_t's real layout at all. */
size_t zones_config_canonical_max_size(void)
{
    return ZONES_CONFIG_BLOB_MAX_SIZE;
}

bool zones_config_export_canonical(const void *cfg, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!cfg || !out || !out_len) {
        return false;
    }
    size_t n = s_stub_blob_size <= out_cap ? s_stub_blob_size : out_cap;
    memcpy(out, cfg, n);
    *out_len = n;
    return true;
}

// ---------------------------------------------------------------------------
// 2026-09-14 follow-up (docs/KILN_PROFILES_PLAN.md items 3/4/14, "finish
// upload/download"): kiln_cfg_store_import_package_json() also calls
// zones_config_json_validate() and (for the section 5.2a compatibility
// checks) zones_config_get_relay_count()/zones_config_get_thermo_count().
// The latter two are ALREADY faked, non-static, in test_backup_import.c/
// test_profile_feasibility.c -- both linked into this same main executable
// -- so this file must NOT redefine them (MSVC's LNK2005 caught exactly
// that on the first attempt). Their real values don't matter for these
// tests anyway: s_stub_blob_size is 16 here, so a decoded candidate's
// zones[] array (which starts well past byte 16 of zones_cfg_t) is always
// left at zeroed relay_mask/thermo_mask by memset(&cand, 0, ...) --
// compatibility trivially passes regardless of live counts. Only
// zones_config_json_validate() needs a fake here (test_zones_http.c's own
// copy is a SEPARATE executable, no collision). */
static bool s_stub_validate_result = true;
static const char *s_stub_validate_err = "stub validation failure";

bool zones_config_json_validate(const zones_cfg_t *cand, const char **err_reason)
{
    (void)cand;
    if (!s_stub_validate_result && err_reason) {
        *err_reason = s_stub_validate_err;
    }
    return s_stub_validate_result;
}

// ---------------------------------------------------------------------------
// safety_ceiling_sync.h stub -- 2026-09-15 review (review_divergence_check_
// 561efa3b_2026-09-15.md, HIGH 1): kiln_cfg_store_autosave_from_live() now
// gates on divergence (plan sec 2.4 rule 6). This executable does not link
// the real safety_ceiling_sync.c, so both predicates need a body here.
// Off by default, matching this file's other fakes -- tests that need
// autosave suppressed under divergence flip these flags explicitly.
// ---------------------------------------------------------------------------
static bool s_stub_ceiling_diverged = false;
static bool s_stub_standing_diverged = false;

bool safety_ceiling_sync_is_diverged(char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap > 0) {
        reason_out[0] = '\0';
    }
    return s_stub_ceiling_diverged;
}

bool safety_ceiling_sync_is_standing_diverged(char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap > 0) {
        reason_out[0] = '\0';
    }
    return s_stub_standing_diverged;
}

// 2026-09-15 review (review_divergence_fixes_b2e7017f_2026-09-15.md, MEDIUM
// 3): kiln_cfg_store_autosave_from_live() also compares safety_cfg_store's
// REAL cache generation (safety_cfg_store.c is linked for real into this
// executable via test_safety_cfg_store.c's #include -- see this file's own
// top comment and the MEDIUM 4 test below) against safety_ceiling_sync's
// latch-evaluated generation to detect a refetch racing in between the
// latch's last evaluation and this autosave call. safety_ceiling_sync.c
// itself is NOT linked here, so only its half needs a controllable stub.
// Zero by default; the one test that exercises the race sets it explicitly
// against the real generation counter's own value.
static uint32_t s_stub_latch_evaluated_generation = 0;

uint32_t safety_ceiling_sync_latch_evaluated_generation(void)
{
    return s_stub_latch_evaluated_generation;
}

// review_autosave_rework_5bc9afb5_2026-09-15.md MEDIUM: a real kiln_cfg_swap_
// is_pending() predicate for tests to register via kiln_cfg_store_set_swap_
// pending_source(), to prove the gate does not key on the divergence latch
// alone.
static bool s_stub_swap_pending = false;
static bool stub_swap_is_pending(void)
{
    return s_stub_swap_pending;
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

// test_safety_cfg_store.c's staging helper, see its own comment on why this
// is the one crossing point into that file's static fake safety_link_get_
// config_page() page state (MEDIUM 4 test below).
extern void test_safety_cfg_store_stage_page_for_kiln_cfg_store_test(size_t page_idx, bool more,
                                                                       const uint16_t *ids, const uint16_t *vals,
                                                                       size_t n);

// test_safety_cfg_store.c's SECOND staging helper (owner decision
// 2026-09-16), for seeding a real F32 value -- see its own comment.
extern void test_safety_cfg_store_stage_f32_for_kiln_cfg_store_test(size_t page_idx, uint16_t id, float value);

// test_safety_cfg_store.c's THIRD staging helper (owner decisions
// 2026-09-16): resets the live safety_cfg_store cache to empty, so a seed
// staged by one test cannot leak into the next.
extern void test_safety_cfg_store_reset_for_kiln_cfg_store_test(void);
extern void test_safety_cfg_store_mark_unfetched_for_kiln_cfg_store_test(void);
extern void test_safety_cfg_store_mark_fetched_for_kiln_cfg_store_test(void);

// ---------------------------------------------------------------------------
// Test scaffolding
// ---------------------------------------------------------------------------

static void reset_state(void)
{
    reset_to_defaults(); // kiln_cfg_store.c's own static helper -- s_store to empty/no-active
    test_safety_cfg_store_reset_for_kiln_cfg_store_test(); // safety_cfg_store.c's live cache, real
                                                            // module linked once -- see that seam's
                                                            // own comment (owner decisions 2026-09-16)
    // reset_to_defaults() above does NOT (and must not) reset safety_cfg_
    // store.c's own s_cache_generation -- that counter is a real,
    // monotonic, whole-run value (bumped once per successful refetch) and
    // is not part of the cache contents reset_to_defaults() clears. But
    // s_stub_latch_evaluated_generation (this file's stand-in for
    // safety_ceiling_sync's own latch-evaluated generation, see its
    // comment above) defaults to 0 and is untouched by reset_to_defaults()
    // too -- once any test in the run calls safety_cfg_store_refetch() the
    // real counter moves past 0 permanently, and every LATER test that
    // relies on kiln_cfg_store_autosave_from_live()'s "no race" path (stub
    // generation == real generation) would then spuriously read as
    // diverged forever, since nothing ever re-syncs the two. Re-sync here,
    // every test, to the real counter's CURRENT value: this is the "no
    // race, matches what was just evaluated" baseline every test other
    // than the one race test explicitly wants, and it is what all these
    // tests got for free before board-id tests started calling refetch().
    s_stub_latch_evaluated_generation = safety_cfg_store_cache_generation();
    // H3 (docs/audits/kiln_profiles_robustness_2026-09-14.md): quarantine
    // state is process-wide static (like s_store itself), set only by
    // nvs_load_store() -- reset it here too, or a quarantine test earlier
    // in the suite would leave every later test's writes refused.
    s_quarantined = false;
    s_quarantine_reason[0] = '\0';

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

    s_stub_validate_result = true;
    s_stub_validate_err = "stub validation failure";

    s_stub_ceiling_diverged = false;
    s_stub_standing_diverged = false;
    // NOT zeroed -- re-synced to the real (whole-run, monotonic) generation
    // counter earlier in this function instead; see that assignment's own
    // comment. Zeroing it here unconditionally used to silently re-break
    // that sync on every call (this line ran AFTER it).
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
    ok = kiln_cfg_store_apply(id2, false, false, reason, sizeof(reason));
    TEST_CHECK(ok, "apply of the clone succeeds (interlock OK, stub import accepts)");
    TEST_CHECK(s_stub_import_call_count == 1, "zones_config_import_blob() was called exactly once");
    TEST_CHECK(s_stub_import_last_len == s_stub_blob_size &&
                   memcmp(s_stub_import_last_bytes, s_stub_export_content, s_stub_blob_size) == 0,
               "the applied bytes match what was originally exported and cloned");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id2, "a successful apply marks that config active");
}

// docs/KILN_PROFILES_PLAN.md items 1/2/12 -- a save-current captures the
// Pico half (via the REAL safety_cfg_store_param_count()/_get_by_index(),
// this executable's own s_store starting empty/all-unset since nothing here
// ever calls safety_cfg_store_refetch()) and computes a package identity
// over it. A save with an unpopulated Pico cache still succeeds and still
// gets a real pkg_hash -- an unset Pico param is packaged, never omitted
// (kiln_package.h's own contract), so "the Pico has never answered" is
// itself a real, hashable state.
static void test_save_current_captures_pico_half_and_hash(void)
{
    TEST_SECTION("kiln_cfg_store_save_current -- captures the Pico half and computes pkg_schema/pkg_hash");
    reset_state();

    int32_t id1 = -1;
    char reason[96];
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_save_current("Kiln A", -1, &id1, reason, sizeof(reason)), "save-as-new succeeds");

    bool pico_populated = false;
    uint16_t pkg_schema = 0;
    uint32_t pkg_hash = 0;
    TEST_CHECK(kiln_cfg_store_get_package_identity(id1, &pico_populated, &pkg_schema, &pkg_hash),
               "package identity is readable for the just-saved slot");
    TEST_CHECK(pico_populated, "the Pico half was captured (even with every param reading unset)");
    TEST_CHECK(pkg_schema == KILN_PKG_SCHEMA_VERSION, "pkg_schema is the current package format identity");
    TEST_CHECK(pkg_hash != 0, "pkg_hash is a real, computed value, not left at 0");

    int idx = find_index_by_id(id1);
    TEST_CHECK(idx >= 0 && s_store.entries[idx].pico.count == safety_cfg_store_param_count(),
               "the captured Pico half enumerates every param the ESP-side table knows about");

    // Changing the ESP-side blob (a different exported config) changes the
    // hash -- proves the hash is not a constant/always-the-same value.
    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(s_stub_export_content[i] ^ 0xFF);
    }
    int32_t id2 = -1;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_save_current("Kiln B", -1, &id2, reason, sizeof(reason)), "second save-as-new succeeds");
    uint32_t pkg_hash2 = 0;
    TEST_CHECK(kiln_cfg_store_get_package_identity(id2, NULL, NULL, &pkg_hash2),
               "second slot's identity is readable");
    TEST_CHECK(pkg_hash2 != pkg_hash, "a different ESP blob content produces a different pkg_hash");
}

static void test_save_current_unfetched_cache_is_not_marked_populated(void)
{
    TEST_SECTION("kiln_cfg_store_save_current -- an unfetched safety cache is never stored as a populated Pico half");
    reset_state();
    test_safety_cfg_store_mark_unfetched_for_kiln_cfg_store_test();

    int32_t id1 = -1;
    char reason[96];
    reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_save_current("Kiln A", -1, &id1, reason, sizeof(reason)),
               "explicit save-as-new is refused while the safety cache is unfetched");
    TEST_CHECK(strstr(reason, "safety processor config not fetched yet") != NULL,
               "refusal names the unfetched safety config");
    TEST_CHECK(id1 == -1, "no slot id was handed out by the refused save");
    bool pico_populated = true;
    uint16_t pkg_schema = 1;
    uint32_t pkg_hash = 1;
    TEST_CHECK(!kiln_cfg_store_get_package_identity(0, &pico_populated, &pkg_schema, &pkg_hash),
               "no slot was created by the refused save");

    // Once the cache has been fetched, the same save captures normally.
    test_safety_cfg_store_reset_for_kiln_cfg_store_test();
    int32_t id2 = -1;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_save_current("Kiln B", -1, &id2, reason, sizeof(reason)), "second save succeeds");
    TEST_CHECK(kiln_cfg_store_get_package_identity(id2, &pico_populated, &pkg_schema, &pkg_hash) && pico_populated &&
                   pkg_hash != 0,
               "fetched cache: slot is populated with a real hash");
}

static void test_get_package_identity_unknown_id_and_migrated_slot(void)
{
    TEST_SECTION("kiln_cfg_store_get_package_identity -- unknown id fails; a v2-migrated slot reads not-yet-captured");
    reset_state();

    bool pico_populated = true;
    uint16_t pkg_schema = 99;
    uint32_t pkg_hash = 99;
    TEST_CHECK(!kiln_cfg_store_get_package_identity(999999, &pico_populated, &pkg_schema, &pkg_hash),
               "an id that does not exist is refused, outputs untouched by convention");

    // Simulate a slot that survived a v2->v3 migration (pico_populated left
    // at 0 by migrate_store_v2_to_v3(), never re-saved since) -- directly
    // poking s_store here rather than staging a real v2 NVS blob, since
    // migrate_store_v2_to_v3() itself is exercised end-to-end by the
    // dedicated migration test below; this test only checks the ACCESSOR's
    // contract for that state.
    reset_to_defaults();
    s_store.entries[0].in_use = 1;
    s_store.entries[0].id = 5;
    strncpy(s_store.entries[0].name, "Migrated", KILN_CFG_NAME_MAX_LEN);
    s_store.entries[0].pico_populated = 0;
    s_store.entries[0].pkg_schema = 0;
    s_store.entries[0].pkg_hash = 0;

    pico_populated = true;
    pkg_schema = 99;
    pkg_hash = 99;
    TEST_CHECK(kiln_cfg_store_get_package_identity(5, &pico_populated, &pkg_schema, &pkg_hash),
               "the slot itself exists, so the call succeeds");
    TEST_CHECK(!pico_populated, "a migrated-but-not-resaved slot reports pico_populated=false");
    TEST_CHECK(pkg_schema == 0 && pkg_hash == 0,
               "schema/hash both read 0 for a not-yet-captured slot -- never a stale or fabricated identity");
}

// docs/KILN_PROFILES_PLAN.md item 1's negative test: a v2 (8-slot, pre-Pico-
// half) blob must migrate through nvs_load_store() into the v3 layout with
// every ESP-side field intact and EVERY migrated slot's pico_populated left
// at 0 (never fabricated) -- proven by staging a real v2-shaped blob through
// a real hal_kv_set_blob()/nvs_load_store() round trip, the same "full-size,
// real round trip" discipline test_nvs_load_store_migrates_v1_blob_at_full_
// size() already established for the v1 branch.
static void build_v2_blob(kiln_cfg_store_blob_v2_t *out, int32_t active_id, uint8_t fill_byte)
{
    memset(out, 0, sizeof(*out));
    out->version = 2;
    out->active_id = active_id;
    out->next_id = 9;
    out->entries[0].in_use = 1;
    out->entries[0].id = 1;
    snprintf(out->entries[0].name, sizeof(out->entries[0].name), "V2 Config");
    out->entries[0].blob_len = 8;
    for (size_t i = 0; i < out->entries[0].blob_len; i++) {
        out->entries[0].blob[i] = (uint8_t)(fill_byte + i);
    }
    out->entries[7].in_use = 1;
    out->entries[7].id = 8;
    snprintf(out->entries[7].name, sizeof(out->entries[7].name), "V2 Last Slot");
}

static void test_nvs_load_store_migrates_v2_blob_at_full_size(void)
{
    TEST_SECTION("nvs_load_store() -- v2 (8-slot, pre-Pico-half) blob migrates to v3, no data lost, "
                 "no Pico half fabricated");
    reset_state();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    kiln_cfg_store_blob_v2_t v2;
    build_v2_blob(&v2, 1, 0x20);
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "hal_kv open succeeds");
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_STORE, &v2, sizeof(v2)) == HAL_OK,
               "a full-size v2 blob fits the fake's storage slot");
    hal_kv_commit(&h);
    hal_kv_close(&h);

    nvs_load_store();

    TEST_CHECK(s_store.version == KILN_CFG_STORE_VERSION, "migrated store carries the CURRENT (v3) version");
    TEST_CHECK(s_store.active_id == 1, "active_id carried over from the v2 blob");
    TEST_CHECK(s_store.next_id == 9, "next_id carried over from the v2 blob");
    TEST_CHECK(s_store.entries[0].in_use == 1 && s_store.entries[0].id == 1,
               "entry 0 migrated (in_use/id)");
    TEST_CHECK(strcmp(s_store.entries[0].name, "V2 Config") == 0, "entry 0's name migrated");
    TEST_CHECK(s_store.entries[0].blob_len == 8, "entry 0's blob_len migrated");
    TEST_CHECK(s_store.entries[0].blob[0] == 0x20 && s_store.entries[0].blob[7] == 0x27,
               "entry 0's blob bytes migrated verbatim");
    TEST_CHECK(s_store.entries[0].pico_populated == 0,
               "migrated entry 0 has NO Pico half -- migrate_store_v2_to_v3() must never fabricate one");
    TEST_CHECK(s_store.entries[0].pkg_hash == 0 && s_store.entries[0].pkg_schema == 0,
               "migrated entry 0's package identity is 0/0, not a stale or invented hash");
    TEST_CHECK(s_store.entries[7].in_use == 1 && s_store.entries[7].id == 8 &&
                   strcmp(s_store.entries[7].name, "V2 Last Slot") == 0,
               "entry 7 (the OLD store's last slot) survived the migration too, not just entry 0");
    TEST_CHECK(s_store.entries[8].in_use == 0 && s_store.entries[9].in_use == 0,
               "the two NEW slots (8, 9) this bump added are empty, not fabricated as in_use");

    fake_kv_reset_all();
}

// The negative test itself (item 1's acceptance criterion): a v2-sized blob
// must NOT be blindly memcpy'd/reinterpreted as a v3 struct -- if it were,
// s_store.entries[0]'s v3-only trailing fields (pico_populated/pkg_schema/
// pkg_hash/pico, which sit at BYTE OFFSETS a v2 blob never wrote) would read
// as whatever garbage happened to follow entry 0's 896-byte blob in the v2
// buffer, and entries[8]/[9] (which don't exist in the 8-slot v2 layout at
// all) would be reinterpreted from BYTES BELONGING TO A DIFFERENT ENTRY
// enitrely (v2's own header fields for a phantom 9th/10th slot that was
// never there) -- exactly the "wrong-version bytes read as a wrong-shape
// struct" defect class this whole migration chain exists to prevent. This
// test breaks the version check BY HAND (in nvs_load_store()'s v2-sized
// branch), watches the resulting corruption, then restores it.
static void test_v2_blob_never_blindly_reinterpreted_as_v3(void)
{
    TEST_SECTION("NEGATIVE TEST -- a v2-sized blob whose version check is bypassed reads as CORRUPT garbage, "
                 "proving the version check (not the size check alone) is load-bearing");
    reset_state();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    // Stage a v2-SIZED blob that claims version 99 (a version this build
    // does not know how to migrate) -- nvs_load_store()'s real v2 branch
    // checks `v2->version != 2` and refuses (defaults stand). This is the
    // real, unmodified production behavior being proven, not a hand-broken
    // one -- see the comment above for why the ALTERNATIVE (skipping this
    // check) would be dangerous, which is what makes this check worth
    // pinning explicitly rather than trusting the size match alone.
    kiln_cfg_store_blob_v2_t v2;
    build_v2_blob(&v2, 3, 0x55);
    v2.version = 99;
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_blob(&h, NVS_KEY_STORE, &v2, sizeof(v2));
    hal_kv_commit(&h);
    hal_kv_close(&h);

    bool trustworthy = nvs_load_store();
    TEST_CHECK(!trustworthy, "a v2-sized blob claiming an unrecognised version 99 is refused, not migrated");
    TEST_CHECK(s_store.active_id == KILN_CFG_NO_ACTIVE_ID,
               "defaults stand -- active_id is NOT the staged blob's 3");
    TEST_CHECK(s_store.entries[0].in_use == 0,
               "defaults stand -- entry 0 is NOT the staged 'V2 Config' -- if the version check had been "
               "bypassed and the buffer reinterpreted anyway, this would spuriously read as in_use with "
               "garbage/incorrect v3-only fields instead of a clean, empty default store");

    fake_kv_reset_all();
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
    bool ok = kiln_cfg_store_apply(id, false, false, reason, sizeof(reason));

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

// docs/audits/kiln_profiles_robustness_2026-09-14.md H5: kiln_cfg_store_
// delete() must refuse to delete the ACTIVE config outright, regardless of
// interlock state -- it is the one stored copy of what this controller is
// running.
static void test_delete_refuses_the_active_config(void)
{
    TEST_SECTION("kiln_cfg_store_delete -- refuses to delete the ACTIVE config, entry survives");
    reset_state();

    int32_t id_a = -1, id_b = -1;
    char reason[96];
    TEST_CHECK(kiln_cfg_store_save_current("Cone 6 Glaze", -1, &id_a, reason, sizeof(reason)), "save A");
    TEST_CHECK(kiln_cfg_store_save_current("Cone 10 Reduction", -1, &id_b, reason, sizeof(reason)), "save B");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id_b, "B (the latest save) is active");

    // No firing running (interlock OK) -- the refusal must still fire
    // because id_b IS the active config, independent of interlock state.
    reason[0] = '\0';
    bool ok = kiln_cfg_store_delete(id_b, false, reason, sizeof(reason));
    TEST_CHECK(!ok, "deleting the active config is refused even with the interlock OK");
    TEST_CHECK(strstr(reason, "Cone 10 Reduction") != NULL && strstr(reason, "running") != NULL,
               "the refusal reason names the active config and says why");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n == 2, "both entries still exist -- the refused delete wrote nothing");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id_b, "active id is unchanged");

    // A NON-active config can still be deleted, interlock permitting.
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_delete(id_a, false, reason, sizeof(reason)),
               "deleting the NON-active config succeeds");
    n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n == 1 && rows[0].id == id_b, "only the non-active entry was removed");
}

// H5, second half: the interlock backstop itself -- deleting a NON-active
// config is still refused while a firing is running, with no caller
// pre-check, same "backstop inside the store" pattern as apply().
static void test_delete_refused_by_interlock_while_firing_even_when_not_active(void)
{
    TEST_SECTION("kiln_cfg_store_delete -- refused by its OWN interlock check while a firing runs, "
                 "even for a non-active config, no caller pre-check");
    reset_state();

    int32_t id_a = -1, id_b = -1;
    char reason[96];
    kiln_cfg_store_save_current("Bisque", -1, &id_a, reason, sizeof(reason));
    kiln_cfg_store_save_current("Cone 10 Reduction", -1, &id_b, reason, sizeof(reason));
    TEST_CHECK(kiln_cfg_store_get_active_id() == id_b, "sanity: B is active, A (the delete target) is not");

    g_stub_ota_interlock_result = OTA_INTERLOCK_REFUSED;
    strncpy(g_stub_ota_interlock_reason, "a firing is currently running",
            sizeof(g_stub_ota_interlock_reason) - 1);

    reason[0] = '\0';
    bool ok = kiln_cfg_store_delete(id_a, false, reason, sizeof(reason));
    TEST_CHECK(!ok, "delete of a non-active config is STILL refused while a firing runs");
    TEST_CHECK(strcmp(reason, "a firing is currently running") == 0,
               "the specific interlock reason is propagated -- proves ota_http_check_interlocks() "
               "was actually consulted");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n == 2, "nothing was deleted");
}

// Negative test for H5: remove the active-config check (simulate the
// pre-fix behavior) and assert the "refuses the active config" test fails.
// This is a documentation-only illustration of the negative test performed
// by hand during implementation (see docs/audits/
// kiln_profiles_robustness_2026-09-14.md's own negative-test instruction);
// the actual hand-break/restore/rebuild cycle is recorded in
// docs/audits/kiln_profiles_implementation_2026-09-14.md, not re-encoded
// here as a permanent test (a permanently-broken production function would
// break every other test in this file).

// docs/audits/kiln_profiles_robustness_2026-09-14.md H17: a slot with
// pico_populated==0 (a v2-migrated half-package, simulated here directly
// since the real migration path is covered by its own dedicated test) must
// refuse apply outright -- never a silent ESP-only partial swap.
static void test_apply_refuses_half_package(void)
{
    TEST_SECTION("kiln_cfg_store_apply -- refuses a pico_populated==0 (half-package) slot, no ESP-only apply");
    reset_state();

    int32_t id = -1;
    char reason[96];
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_save_current("Half Package", -1, &id, reason, sizeof(reason)), "save succeeds");
    int idx = find_index_by_id(id);
    TEST_CHECK(idx >= 0 && s_store.entries[idx].pico_populated, "sanity: an ordinary save DOES populate the Pico half");

    // Simulate a v2-migrated slot that was never re-saved.
    s_store.entries[idx].pico_populated = 0;
    s_store.entries[idx].pkg_schema = 0;
    s_store.entries[idx].pkg_hash = 0;

    int import_calls_before = s_stub_import_call_count;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_apply(id, false, false, reason, sizeof(reason));
    TEST_CHECK(!ok, "apply of a half-package slot is refused");
    TEST_CHECK(strstr(reason, "Half Package") != NULL && strstr(reason, "safety processor") != NULL,
               "the refusal names the slot and explains why");
    TEST_CHECK(s_stub_import_call_count == import_calls_before,
               "zones_config_import_blob() was NEVER called -- this is not a partial/ESP-only apply, "
               "it is a full refusal before anything is touched");

    // Completing the slot (an ordinary re-save) must make apply succeed again.
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_save_current("Half Package", id, NULL, reason, sizeof(reason)),
               "re-save (overwrite by id) completes the slot");
    TEST_CHECK(s_store.entries[idx].pico_populated, "re-save captured a real Pico half");
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_apply(id, false, false, reason, sizeof(reason)), "apply now succeeds");
}

// docs/audits/kiln_profiles_robustness_2026-09-14.md H3: a corrupt store
// quarantines instead of silently reset_to_defaults()-ing, and every
// mutation is refused until the operator explicitly discards it.
static void test_corrupt_store_quarantines_and_blocks_writes(void)
{
    TEST_SECTION("nvs_load_store() -- a corrupt (wrong-size) blob quarantines the store; every write is "
                 "refused until quarantine_clear(confirm_discard=1)");
    reset_state();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    // A blob one byte short of the current version's real size -- "wrong
    // size for any known version" is genuine corruption per nvs_load_store()'s
    // own comment.
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    kiln_cfg_store_blob_t garbage;
    memset(&garbage, 0xAB, sizeof(garbage));
    garbage.version = KILN_CFG_STORE_VERSION;
    hal_kv_set_blob(&h, NVS_KEY_STORE, &garbage, sizeof(garbage) - 1); // one byte short
    hal_kv_commit(&h);
    hal_kv_close(&h);

    bool trustworthy = nvs_load_store();
    TEST_CHECK(!trustworthy, "a wrong-size blob is not trustworthy");

    char reason[192];
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_is_quarantined(reason, sizeof(reason)), "the store reports itself quarantined");
    TEST_CHECK(reason[0] != '\0', "a specific reason is given");

    int32_t id = -1;
    char op_reason[192];
    op_reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_save_current("New Kiln", -1, &id, op_reason, sizeof(op_reason)),
               "save is refused while quarantined");
    TEST_CHECK(strstr(op_reason, "quarantine") != NULL, "the refusal mentions the quarantine");

    op_reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_quarantine_clear(false, op_reason, sizeof(op_reason)),
               "clearing WITHOUT confirm_discard=1 is refused");
    TEST_CHECK(kiln_cfg_store_is_quarantined(NULL, 0), "still quarantined -- the refused clear changed nothing");

    op_reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_quarantine_clear(true, op_reason, sizeof(op_reason)),
               "clearing WITH confirm_discard=1 succeeds");
    TEST_CHECK(!kiln_cfg_store_is_quarantined(NULL, 0), "no longer quarantined");
    TEST_CHECK(kiln_cfg_store_get_active_id() == KILN_CFG_NO_ACTIVE_ID, "the store is now a clean, empty default");

    // Now an ordinary save must work again.
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_save_current("Fresh Start", -1, &id, reason, sizeof(reason)),
               "a save after clearing the quarantine succeeds normally");

    fake_kv_reset_all();
}

// Negative test for H3: confirms the quarantine gate is load-bearing, not
// decorative, by observing what WOULD happen without it -- the corrupt
// bytes are what a save would otherwise silently overwrite. Performed by
// temporarily removing the refuse_if_quarantined() call from kiln_cfg_
// store_save_current() during implementation (see docs/audits/
// kiln_profiles_implementation_2026-09-14.md's recorded hand-break/restore
// cycle) -- not re-encoded as a permanent test, since a permanently broken
// production function would fail every other test in this file.

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
    bool ok = kiln_cfg_store_apply(id, false, false, reason, sizeof(reason));

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
    bool ok = kiln_cfg_store_apply(id_a, false, false, reason, sizeof(reason));

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
    TEST_CHECK(strcmp(reason, "name missing, too long, or contains invalid characters") == 0,
               "same refusal reason as an empty name");

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

// docs/audits/kiln_profiles_robustness_2026-09-14.md H9: control bytes,
// '"'/'\\'/'/', and truncated UTF-8 must all be rejected; ordinary
// printable/UTF-8 names must still be accepted.
static void test_name_character_set_validation(void)
{
    TEST_SECTION("kiln_cfg_store -- name character-set + UTF-8 validation (H9)");
    reset_state();

    struct {
        const char *name;
        bool should_accept;
        const char *why;
    } cases[] = {
        {"Skutt KM-1027", true, "ordinary printable name"},
        {"Bailey #2", true, "printable punctuation, not on the reject list"},
        {"Caf\xC3\xA9 Kiln", true, "valid 2-byte UTF-8 (e with acute)"},
        {"Kiln\twith\ttab", false, "control byte (tab) in the MIDDLE, not just at an edge"},
        {"Kiln\nwith\nnewline", false, "control byte (newline) in the middle"},
        {"Kiln\x01name", false, "raw control byte 0x01"},
        {"Kiln\x7Fname", false, "DEL (0x7F)"},
        {"Say \"hi\"", false, "double-quote -- would break a future JSON/HTTP-header quoting"},
        {"back\\slash", false, "backslash"},
        {"a/b", false, "forward slash -- hazardous in a filename"},
        {"Caf\xC3", false, "truncated 2-byte UTF-8 sequence (lead byte, no continuation)"},
        {"Caf\xE2\x82", false, "truncated 3-byte UTF-8 sequence (missing the last continuation byte)"},
        {"\x80name", false, "bare continuation byte with no lead byte"},
        {"\xC0\x80name", false, "overlong-encoding lead byte (0xC0), never valid UTF-8"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset_state();
        int32_t id = -1;
        char reason[96];
        reason[0] = '\0';
        bool ok = kiln_cfg_store_save_current(cases[i].name, -1, &id, reason, sizeof(reason));
        TEST_CHECK(ok == cases[i].should_accept, cases[i].why);
    }
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

static void reset_state_cfg_fs(void); // defined with the cfg_fs section below
static const char *KCFG_SCRATCH_BASE;  // tentative definition; initialised with the cfg_fs section below

static void test_nvs_save_store_proceeds_normally_on_an_internal_ram_stack(void)
{
    TEST_SECTION("nvs_save_store -- proceeds normally when the calling task's stack is internal RAM");
    // Saves land in the `cfg` file only now, so the write needs a mounted scratch cfg.
    reset_state_cfg_fs();
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    // fake_kv_set_write_safe_here(true) is the fake's default state after reset.
    hal_status_t err = nvs_save_store();

    TEST_CHECK(err == HAL_OK, "the guard does not fire on an internal-RAM stack -- the write "
                              "proceeds exactly as before this net was added");

    cfg_fs_deinit();
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

/* What a LEGACY (pre dual-write-close) firmware left in NVS: the whole-store blob plus
 * its kilncfgrv rev key. Nothing in production writes these keys any more. */
static void kcfg_stage_legacy_nvs(const kiln_cfg_store_blob_t *blob, uint32_t rev)
{
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "stage legacy: NVS opened");
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_STORE, blob, sizeof(*blob)) == HAL_OK, "stage legacy: blob");
    TEST_CHECK(hal_kv_set_u32(&h, NVS_KEY_STORE_REV, rev) == HAL_OK, "stage legacy: rev");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "stage legacy: commit");
    hal_kv_close(&h);
}

static bool kcfg_nvs_store_absent(void)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) != HAL_OK) {
        return true;
    }
    static kiln_cfg_store_blob_t probe;
    size_t len = sizeof(probe);
    hal_status_t g = hal_kv_get_blob(&h, NVS_KEY_STORE, &probe, &len);
    hal_kv_close(&h);
    return g != HAL_OK;
}

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

// 1. Partition absent (recovery mode / failed mount): a save still takes
//    effect in RAM, is never written to NVS, and persists nowhere.
static void test_cfg_fs_partition_absent_falls_through_to_nvs_only(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: partition absent -- save lives in RAM only, NVS is never written");
    reset_state_cfg_fs();
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs never mounted in this test");

    int32_t id = -1;
    char reason[96];
    reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_save_current("absent", -1, &id, reason, sizeof(reason)),
               "save-as-new FAILS when cfg is not mounted (no NVS fallback, no fake success)");
    TEST_CHECK(kiln_cfg_store_reason_is_persist_failure(reason), "the failure reason names the persist failure");
    TEST_CHECK(kiln_cfg_store_get_active_id() == KILN_CFG_NO_ACTIVE_ID,
               "the failed save left no live entry or active id");

    kiln_cfg_store_blob_t raw;
    uint32_t rev = 999;
    bool raw_valid = true;
    kiln_cfg_store_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(!raw_valid && rev == 0, "no file was ever written -- cfg_fs_is_available() gated every file op");
    TEST_CHECK(kcfg_nvs_store_absent(), "and nothing fell back to NVS");
}

// 2. Mount failed: cfg_fs_init() against a nonexistent directory fails; same
//    behavior as partition-absent.
static void test_cfg_fs_mount_failed_falls_through_to_nvs_only(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: cfg_fs mount FAILED -- save lives in RAM only, non-fatal");
    reset_state_cfg_fs();

    esp_err_t mount_err = cfg_fs_init("this_directory_does_not_exist_at_all_kcfg", NULL);
    TEST_CHECK(mount_err != ESP_OK, "cfg_fs_init() against a nonexistent base dir fails, as documented");
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs reports unavailable after a failed mount");

    int32_t id = -1;
    char reason[96];
    reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_save_current("unmounted", -1, &id, reason, sizeof(reason)),
               "save-as-new FAILS after a failed mount");
    TEST_CHECK(kiln_cfg_store_reason_is_persist_failure(reason), "the failure reason names the persist failure");
    TEST_CHECK(kcfg_nvs_store_absent(), "nothing fell back to NVS");

    cfg_fs_deinit();
}

// 3. Legacy NVS-only store + lazy migration: a board upgraded from the
//    dual-write firmware has only NVS; the first load migrates it to the
//    file at the NVS rev, and a later load needs NVS no more.
static void test_cfg_fs_nvs_fallback_then_file_preferred_after_migration(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: a legacy NVS-only store migrates to the file on first load; the file "
                 "carries it afterward");
    reset_state_cfg_fs();

    int32_t id = -1;
    char reason[96];
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    TEST_CHECK(kiln_cfg_store_save_current("migrate", -1, &id, reason, sizeof(reason)),
               "save-as-new builds the content to stage");
    kcfg_stage_legacy_nvs(&s_store, 4);
    // Leave ONLY the legacy NVS copy: remove the file the save just wrote.
    TEST_CHECK(cfg_fs_delete(KILN_CFG_STORE_FILE_PATH) == ESP_OK, "the file written by the staging save is removed");
    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    TEST_CHECK(kiln_cfg_store_get_active_id() == id, "legacy store's active id loaded from NVS");
    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(kiln_cfg_store_get_name(id, name_buf, sizeof(name_buf)) && strcmp(name_buf, "migrate") == 0,
               "legacy store's name loaded from NVS");

    bool exists = false;
    TEST_CHECK(cfg_fs_exists(KILN_CFG_STORE_FILE_PATH, &exists) == ESP_OK && exists,
               "the first load migrated the store into the file");
    kiln_cfg_store_blob_t raw;
    uint32_t rev = 0;
    bool raw_valid = false;
    kiln_cfg_store_cfg_fs_load_raw(&raw, &rev, &raw_valid);
    TEST_CHECK(raw_valid && rev == 4, "the file holds a valid copy at the NVS rev (resync, not above it)");
    TEST_CHECK(raw.active_id == id, "file content matches the legacy store");

    // NVS gone: the file alone must carry the store.
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    TEST_CHECK(kiln_cfg_store_get_name(id, name_buf, sizeof(name_buf)) && strcmp(name_buf, "migrate") == 0,
               "reload from the file alone returns the same store");
}

// 4. Saves and deletes each advance the shared whole-document rev, in the
//    file only.
static void test_cfg_fs_dual_write_keeps_file_and_nvs_in_sync(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: repeated saves/deletes advance the file rev; NVS is never written");
    reset_state_cfg_fs();
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    char reason[96];
    int32_t id_a = -1, id_b = -1;
    TEST_CHECK(kiln_cfg_store_save_current("A", -1, &id_a, reason, sizeof(reason)), "save 1 (rev 1)");
    TEST_CHECK(kiln_cfg_store_save_current("B", -1, &id_b, reason, sizeof(reason)), "save 2 (rev 2)");
    TEST_CHECK(kiln_cfg_store_delete(id_a, false, reason, sizeof(reason)), "delete of A (rev 3)");

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
    TEST_CHECK(kcfg_nvs_store_absent(), "NVS was never written -- the dual-write window is closed");

    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(kiln_cfg_store_get_name(id_b, name_buf, sizeof(name_buf)) && strcmp(name_buf, "B") == 0,
               "a reload returns the file's content after the mixed save/delete sequence");
    TEST_CHECK(!kiln_cfg_store_get_name(id_a, name_buf, sizeof(name_buf)), "the deleted entry stays gone");
}

// 5. Divergence tie-break, BOTH directions.
static esp_err_t kcfg_failing_write_fn(const char *rel_path, const void *data, size_t len)
{
    (void)rel_path;
    (void)data;
    (void)len;
    return ESP_FAIL;
}

// Every public mutator reports a failed cfg write as a failure (HTTP callers
// turn it into a 500) and rolls its RAM change back.
static void test_cfg_fs_all_mutators_report_persist_failure(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: rename/clone/set_active_id_raw/save-over report a failed cfg write");
    reset_state_cfg_fs();
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    char reason[96];
    int32_t a = -1, b = -1;
    TEST_CHECK(kiln_cfg_store_save_current("alpha", -1, &a, reason, sizeof(reason)), "alpha saved");
    TEST_CHECK(kiln_cfg_store_save_current("beta", -1, &b, reason, sizeof(reason)), "beta saved (active)");

    kiln_cfg_store_cfg_fs_set_write_fn(kcfg_failing_write_fn);
    char nm[KILN_CFG_NAME_MAX_LEN + 1];

    reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_rename_ex(a, "gamma", reason, sizeof(reason)), "rename fails");
    TEST_CHECK(kiln_cfg_store_reason_is_persist_failure(reason), "rename reason names the persist failure");
    TEST_CHECK(kiln_cfg_store_get_name(a, nm, sizeof(nm)) && strcmp(nm, "alpha") == 0, "rename rolled back");
    TEST_CHECK(!kiln_cfg_store_rename(a, "gamma"), "legacy rename wrapper fails too");

    int32_t c = -1;
    reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_clone(a, "alpha copy", &c, reason, sizeof(reason)), "clone fails");
    TEST_CHECK(kiln_cfg_store_reason_is_persist_failure(reason), "clone reason names the persist failure");
    TEST_CHECK(!kiln_cfg_store_name_would_collide("alpha copy", -1), "failed clone left no entry behind");

    reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_set_active_id_raw(a, reason, sizeof(reason)), "set_active_id_raw fails");
    TEST_CHECK(kiln_cfg_store_reason_is_persist_failure(reason), "set_active reason names the persist failure");

    reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_save_current("alpha2", a, NULL, reason, sizeof(reason)), "overwrite-save fails");
    TEST_CHECK(kiln_cfg_store_get_name(a, nm, sizeof(nm)) && strcmp(nm, "alpha") == 0,
               "overwrite-save rolled back the entry's name");

    kiln_cfg_store_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(kiln_cfg_store_rename(a, "gamma"), "the same rename succeeds once writes work again");
    TEST_CHECK(!kiln_cfg_store_reason_is_persist_failure("no saved kiln config with that id") &&
                   !kiln_cfg_store_reason_is_persist_failure(NULL),
               "ordinary refusals are not classed as persist failures");
    cfg_fs_deinit();
}

static void test_cfg_fs_divergence_tie_break_both_directions(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: a failed cfg write advances no rev; a legacy NVS copy with a higher rev "
                 "wins and the file is resynced");
    reset_state_cfg_fs();
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    char reason[96];
    int32_t id1 = -1;
    TEST_CHECK(kiln_cfg_store_save_current("rev1", -1, &id1, reason, sizeof(reason)), "rev 1 saved to the file");

    // A file write failure: the change lives in RAM only, the file stays at
    // rev 1 with the OLD content, and nothing falls back to NVS.
    kiln_cfg_store_cfg_fs_set_write_fn(kcfg_failing_write_fn);
    int32_t id2 = -1;
    reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_save_current("rev2_lost", -1, &id2, reason, sizeof(reason)),
               "rev 2 save FAILS when the cfg write fails");
    TEST_CHECK(kiln_cfg_store_reason_is_persist_failure(reason), "the failure reason names the persist failure");
    {
        char live_name[KILN_CFG_NAME_MAX_LEN + 1];
        TEST_CHECK(kiln_cfg_store_get_name(id1, live_name, sizeof(live_name)) && strcmp(live_name, "rev1") == 0 &&
                       kiln_cfg_store_get_active_id() == id1,
                   "the failed save was rolled back in RAM (previous entry and active id intact)");
    }
    kiln_cfg_store_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(kcfg_nvs_store_absent(), "the failed cfg write did NOT fall back to NVS");

    kiln_cfg_store_blob_t raw_before;
    uint32_t rev_before = 0;
    bool raw_valid_before = false;
    kiln_cfg_store_cfg_fs_load_raw(&raw_before, &rev_before, &raw_valid_before);
    TEST_CHECK(raw_valid_before && rev_before == 1, "file is stuck at rev 1 -- the failed write never landed");

    // A reboot loses the unpersisted change (no NVS safety net any more).
    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(!kiln_cfg_store_get_name(id2, name_buf, sizeof(name_buf)),
               "the change whose cfg write failed did not survive a reload");
    TEST_CHECK(kiln_cfg_store_get_name(id1, name_buf, sizeof(name_buf)) && strcmp(name_buf, "rev1") == 0,
               "the last verified file content stands");

    // Legacy NVS copy with a higher rev: wins, and the file is resynced at the NVS rev.
    kiln_cfg_store_blob_t legacy = s_store;
    strncpy(legacy.entries[0].name, "legacy_nvs", KILN_CFG_NAME_MAX_LEN);
    legacy.entries[0].name[KILN_CFG_NAME_MAX_LEN] = '\0';
    kcfg_stage_legacy_nvs(&legacy, 5);
    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    TEST_CHECK(strcmp(s_store.entries[0].name, "legacy_nvs") == 0,
               "legacy NVS (higher rev) wins the tie-break, not the older file");

    kiln_cfg_store_blob_t raw_after;
    uint32_t rev_after = 0;
    bool raw_valid_after = false;
    kiln_cfg_store_cfg_fs_load_raw(&raw_after, &rev_after, &raw_valid_after);
    TEST_CHECK(raw_valid_after && rev_after == 5, "the file was resynced from NVS at the NVS rev");

    // Normal direction: one more ordinary save advances from the resolved rev.
    int32_t id3 = -1;
    TEST_CHECK(kiln_cfg_store_save_current("rev3", -1, &id3, reason, sizeof(reason)), "next save lands in the file");
    kiln_cfg_store_cfg_fs_load_raw(&raw_after, &rev_after, &raw_valid_after);
    TEST_CHECK(raw_valid_after && rev_after == 6, "the save advanced the rev from the resolved one (5 -> 6)");
    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    TEST_CHECK(kiln_cfg_store_get_name(id3, name_buf, sizeof(name_buf)) && strcmp(name_buf, "rev3") == 0,
               "a reload returns the new save");
}

// 6. EQUAL revs with differing content must adopt NVS, not the stale file --
//    the rollback round trip the rev counter exists for (a rolled-back
//    firmware that still wrote NVS).
static void test_cfg_fs_equal_rev_divergence_adopts_nvs_not_the_stale_file(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: equal revs with differing content adopt NVS (rolled-back-firmware "
                 "edit survives a roll-forward)");
    reset_state_cfg_fs();
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    char reason[96];
    int32_t id1 = -1;
    TEST_CHECK(kiln_cfg_store_save_current("before_rollback", -1, &id1, reason, sizeof(reason)),
               "rev 1 saved to the file");

    // Act like firmware that predates the rev counter: write ONLY the NVS
    // blob, leaving kilncfgrv at 1 and the file at rev 1/old content.
    kiln_cfg_store_blob_t rolled_back = s_store;
    strncpy(rolled_back.entries[0].name, "rolled_edit", KILN_CFG_NAME_MAX_LEN);
    rolled_back.entries[0].name[KILN_CFG_NAME_MAX_LEN] = '\0';
    kcfg_stage_legacy_nvs(&rolled_back, 1);

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
                   "fixture check: kilncfgrv is 1 -- file_rev == nvs_rev, the EQUAL-rev case");
        hal_kv_close(&h);
    }

    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    TEST_CHECK(strcmp(s_store.entries[0].name, "rolled_edit") == 0,
               "NVS wins the EQUAL-rev tie -- the edit made on rolled-back firmware is NOT discarded in "
               "favour of the stale file");

    kiln_cfg_store_cfg_fs_load_raw(&file_raw, &file_rev, &file_raw_valid);
    TEST_CHECK(file_raw_valid && file_rev == 1 && strcmp(file_raw.entries[0].name, "rolled_edit") == 0,
               "the losing file was resynced from NVS (at the same rev), so the divergence does not persist");
}

// 7. Stale-delete-not-resurrected: a delete whose file write fails leaves
//    the file at the pre-delete document. A legacy NVS copy that DID record
//    the delete at a higher rev must win through the same strict tie-break,
//    so the slot does not come back.
static void test_cfg_fs_stale_delete_not_resurrected(void)
{
    TEST_SECTION("kiln_cfg_store cfg_fs: a legacy NVS copy recording a delete beats the stale file; a failed "
                 "cfg delete advances no rev");
    reset_state_cfg_fs();
    TEST_CHECK(cfg_fs_init(KCFG_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    char reason[96];
    int32_t id = -1, other_id = -1;
    TEST_CHECK(kiln_cfg_store_save_current("doomed", -1, &id, reason, sizeof(reason)),
               "rev 1: slot saved");
    // H5 (docs/audits/kiln_profiles_robustness_2026-09-14.md) fix: deleting
    // the ACTIVE config is refused outright. Save a second slot so
    // "doomed" is no longer active before deleting it.
    TEST_CHECK(kiln_cfg_store_save_current("keeper", -1, &other_id, reason, sizeof(reason)),
               "rev 2: a second slot is saved and becomes active, freeing 'doomed' to be deleted");

    // The delete's file write fails: RAM shows the delete, the file is left
    // holding the pre-delete document (the slot still in_use) at rev 2.
    kiln_cfg_store_cfg_fs_set_write_fn(kcfg_failing_write_fn);
    reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_delete(id, false, reason, sizeof(reason)),
               "the delete FAILS when the file write fails");
    TEST_CHECK(kiln_cfg_store_reason_is_persist_failure(reason), "the failure reason names the persist failure");
    {
        char live_name[KILN_CFG_NAME_MAX_LEN + 1];
        TEST_CHECK(kiln_cfg_store_get_name(id, live_name, sizeof(live_name)),
                   "the failed delete kept the slot in RAM");
    }
    kiln_cfg_store_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(kcfg_nvs_store_absent(), "the failed cfg write did NOT fall back to NVS");

    kiln_cfg_store_blob_t file_raw;
    uint32_t file_rev = 0;
    bool file_raw_valid = false;
    kiln_cfg_store_cfg_fs_load_raw(&file_raw, &file_rev, &file_raw_valid);
    TEST_CHECK(file_raw_valid && file_rev == 2, "fixture check: the stale file is still at rev 2");
    int stale_still_in_use = 0;
    for (int i = 0; i < KILN_CFG_MAX_COUNT; i++) {
        if (file_raw.entries[i].in_use && file_raw.entries[i].id == id) {
            stale_still_in_use = 1;
        }
    }
    TEST_CHECK(stale_still_in_use, "fixture check: the stale file still shows the deleted slot as in_use");

    // A legacy NVS copy that recorded the delete at rev 3 (what the old
    // dual-write firmware would have left behind): it must win.
    {
        kiln_cfg_store_blob_t legacy_del = s_store;
        for (int i = 0; i < KILN_CFG_MAX_COUNT; i++) {
            if (legacy_del.entries[i].in_use && legacy_del.entries[i].id == id) {
                memset(&legacy_del.entries[i], 0, sizeof(legacy_del.entries[i]));
            }
        }
        kcfg_stage_legacy_nvs(&legacy_del, 3);
    }
    memset(&s_store, 0, sizeof(s_store));
    nvs_load_store_with_cfg_fs();
    char name_buf[KILN_CFG_NAME_MAX_LEN + 1];
    TEST_CHECK(!kiln_cfg_store_get_name(id, name_buf, sizeof(name_buf)),
               "the deleted slot did NOT resurrect -- the legacy NVS copy's higher rev (the delete) won");

    kiln_cfg_store_cfg_fs_load_raw(&file_raw, &file_rev, &file_raw_valid);
    TEST_CHECK(file_raw_valid && file_rev == 3, "the stale file was resynced -- it no longer shows the "
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

// ---------------------------------------------------------------------------
// Download/upload (docs/KILN_PROFILES_PLAN.md items 3/4/14, 2026-09-14
// "finish upload/download" follow-up). Three refusal-reason tests requested
// explicitly: malformed, unknown-newer version, hash mismatch -- each
// asserts nothing is written (store count unchanged) and the live config
// (s_stub_export_content, read via kiln_cfg_store_save_current() itself
// never being called again) is untouched.
// ---------------------------------------------------------------------------

static void test_export_import_new_slot_round_trip(void)
{
    TEST_SECTION("kiln_cfg_store_export_package_json/_import_package_json -- round trip into a NEW slot, "
                 "never overwrites, never applies");
    reset_state();

    int32_t id1 = -1;
    char reason[160];
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_save_current("Skutt KM-1027", -1, &id1, reason, sizeof(reason)), "save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "export succeeds for a fully-populated (pico_populated) slot");

    // A saved slot's own name always collides with itself (same rule
    // kiln_cfg_store_clone() already enforces -- "a saved kiln config
    // already has that name" is a real, deliberate refusal, not a bug this
    // test should paper over). Simulate the realistic "importing this
    // package onto a DIFFERENT controller, or after renaming" case by
    // giving it a distinct (but SAME-LENGTH, for a trivial in-place
    // overwrite) name before import, exactly as an operator would via the
    // upload UI's name field.
    char *name_digit = strstr(json, "\"name\":\"Skutt KM-1027\"");
    TEST_CHECK(name_digit != NULL, "found the name field to rename before import");
    if (name_digit) {
        name_digit[strlen("\"name\":\"Skutt KM-102")] = '8'; // "...1027" -> "...1028", same length
    }

    int32_t id2 = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_import_package_json(json, &id2, reason, sizeof(reason));
    TEST_CHECK(ok, "import of the just-exported package succeeds");
    TEST_CHECK(id2 > 0 && id2 != id1, "import allocates a BRAND NEW slot, never overwrites id1");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id1, "import never changes which config is active");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(n == 2, "exactly two slots now exist -- the original plus the imported copy");

    uint8_t blob1[ZONES_CONFIG_BLOB_MAX_SIZE], blob2[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint16_t len1 = 0, len2 = 0;
    kiln_pkg_safety_t pico1, pico2;
    TEST_CHECK(kiln_cfg_store_get_full_package(id1, blob1, sizeof(blob1), &len1, &pico1, NULL, 0),
               "read back original");
    TEST_CHECK(kiln_cfg_store_get_full_package(id2, blob2, sizeof(blob2), &len2, &pico2, NULL, 0),
               "read back imported copy");
    TEST_CHECK(len1 == len2 && memcmp(blob1, blob2, len1) == 0, "ESP half is byte-for-byte identical");
    TEST_CHECK(pico1.count == pico2.count && memcmp(pico1.entries, pico2.entries,
                                                    sizeof(pico1.entries[0]) * pico1.count) == 0,
               "Pico half is byte-for-byte identical");

    free(json);
}

// ---------------------------------------------------------------------------
// docs/WEB_AUTH_PLAN.md item 12b: "the config package" (per-slot
// export/import via kiln_cfg_store_export_package_json()/
// _import_package_json(), distinct from the whole-board backup covered in
// test_backup_import.c) must never contain, and must never disturb, a
// kiln_auth credential. A synthetic credential is used, never a real one.
// ---------------------------------------------------------------------------
static const uint8_t PKG12B_SALT[WEB_AUTH_SALT_LEN] = {
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20};
#define PKG12B_SYNTHETIC_PASSWORD "Synthetic-Package-Secret-4"
#define PKG12B_SYNTHETIC_PIN "608274"

static void test_export_package_json_never_contains_or_disturbs_credential(void)
{
    TEST_SECTION("kiln_cfg_store_export_package_json/_import_package_json -- WEB_AUTH_PLAN.md item 12b: "
                 "a kiln_auth credential is absent from the exported package JSON and untouched by import");
    reset_state();
    fake_kv_reset_all();
    TEST_CHECK(hal_kv_init_partition(KILN_NVS_PARTITION) == HAL_OK, "setup: re-init kiln_nvs after fake_kv_reset_all");
    TEST_CHECK(hal_kv_init_partition(NULL) == HAL_OK, "setup: init the default nvs partition (kiln_auth's home)");
    TEST_CHECK(web_auth_store_set_password(WEB_AUTH_ROLE_ADMINISTRATOR, "pkgtestuser",
                                            PKG12B_SYNTHETIC_PASSWORD, PKG12B_SALT, false) == HAL_OK,
              "setup: seed a synthetic administrator password");
    TEST_CHECK(web_auth_store_set_pin(WEB_AUTH_ROLE_USER, PKG12B_SYNTHETIC_PIN, PKG12B_SALT) == HAL_OK,
              "setup: seed a synthetic user LCD PIN");

    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Cred-Package-Test", -1, &id1, reason, sizeof(reason)), "save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "export succeeds");
    TEST_CHECK(strstr(json, "kiln_auth") == NULL, "package JSON never names the kiln_auth namespace");
    TEST_CHECK(strstr(json, "web_auth") == NULL, "package JSON never names the web_auth key");
    TEST_CHECK(strstr(json, "lcd_auth") == NULL, "package JSON never names the lcd_auth key");
    TEST_CHECK(strstr(json, "auth_policy") == NULL, "package JSON never names the auth_policy key");
    TEST_CHECK(strstr(json, "password") == NULL, "package JSON never contains the literal word 'password'");
    TEST_CHECK(strstr(json, PKG12B_SYNTHETIC_PASSWORD) == NULL,
              "package JSON never contains the plaintext synthetic password");

    // Import it back (into a new slot, same convention as
    // test_export_import_new_slot_round_trip() above) and confirm the
    // credential is still exactly as seeded -- import must not disturb it.
    char *name_field = strstr(json, "\"name\":\"Cred-Package-Test\"");
    TEST_CHECK(name_field != NULL, "found the name field to rename before import");
    if (name_field) {
        name_field[strlen("\"name\":\"Cred-Package-Tes")] = '5'; // same length, avoids the self-name collision
    }
    int32_t id2 = -1;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_import_package_json(json, &id2, reason, sizeof(reason)), "import succeeds");

    TEST_CHECK(web_auth_store_verify_password(WEB_AUTH_ROLE_ADMINISTRATOR, PKG12B_SYNTHETIC_PASSWORD),
              "the administrator password must still verify after export+import of a config package");
    TEST_CHECK(web_auth_store_verify_pin(WEB_AUTH_ROLE_USER, PKG12B_SYNTHETIC_PIN),
              "the user PIN must still verify after export+import of a config package");

    free(json);
}

// ---------------------------------------------------------------------------
// Task 3 (bkfinish_assessment.md item 3): host tests for the validate-only
// seam added by docs/KILN_PROFILES_PLAN.md item 17
// (kiln_cfg_store_validate_package_json()) and the name-override import
// entry point (kiln_cfg_store_import_package_json_as()) that backup_import.c
// (task 4/6/7) needs. Neither had ANY coverage before this task -- the WIP
// assessment named this explicitly ("both new entry points ship with zero
// coverage").
// ---------------------------------------------------------------------------

static void test_validate_package_json_writes_nothing_on_success(void)
{
    TEST_SECTION("kiln_cfg_store_validate_package_json -- a valid package normalizes name/schema/hash and "
                 "writes NOTHING (slot count unchanged, no NVS write)");
    reset_state();

    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("  Validate Me  ", -1, &id1, reason, sizeof(reason)), "setup: save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "setup: export succeeds");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t before = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);

    char name_out[KILN_CFG_NAME_MAX_LEN + 1] = {0};
    uint16_t out_schema = 0;
    uint32_t out_hash = 0;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_validate_package_json(json, name_out, sizeof(name_out), &out_schema, &out_hash, reason,
                                                   sizeof(reason));
    TEST_CHECK(ok, "a valid, just-exported package validates");
    TEST_CHECK(strcmp(name_out, "Validate Me") == 0, "name_out is trimmed/normalized exactly as stored");
    TEST_CHECK(out_schema != 0, "pkg_schema is reported");
    TEST_CHECK(out_hash != 0, "pkg_hash is reported (non-zero for a real populated package)");

    uint8_t after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(after == before, "slot count is UNCHANGED -- validate-only never creates a slot");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id1, "active id is unchanged");

    // Confirm the identity validate() reported matches what actually
    // committing the same bytes would produce -- this is the whole point of
    // the seam (backup_import.c compares this against a live board's own
    // kiln_cfg_store_get_package_identity() before deciding create/rename).
    // The file's own name ("Validate Me") collides with id1's existing slot
    // by construction (it's a self-export), so rename it first -- same trick
    // as test_export_import_new_slot_round_trip() above; the pkg_schema/
    // pkg_hash identity being compared is unaffected by the name field.
    char *name_field_commit = strstr(json, "\"name\":\"Validate Me\"");
    TEST_CHECK(name_field_commit != NULL, "found the name field to rename before the real commit");
    if (name_field_commit) {
        name_field_commit[strlen("\"name\":\"Validate M")] = 'f'; // "Me" -> "Mf", same length
    }
    int32_t id2 = -1;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_import_package_json(json, &id2, reason, sizeof(reason)),
              "committing the identical (renamed) bytes for real must still succeed");
    bool committed_pico_populated = false;
    uint16_t committed_schema = 0;
    uint32_t committed_hash = 0;
    TEST_CHECK(kiln_cfg_store_get_package_identity(id2, &committed_pico_populated, &committed_schema,
                                                   &committed_hash),
              "read back the committed slot's identity");
    TEST_CHECK(committed_schema == out_schema, "validate()'s reported pkg_schema matches the real committed one");
    TEST_CHECK(committed_hash == out_hash, "validate()'s reported pkg_hash matches the real committed one");

    free(json);
}

static void test_validate_package_json_refuses_bad_hash_same_reason_as_import(void)
{
    TEST_SECTION("kiln_cfg_store_validate_package_json -- refuses a pkg_hash mismatch with the SAME reason "
                 "the committing call gives, writes nothing");
    reset_state();
    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Original", -1, &id1, reason, sizeof(reason)), "setup: save succeeds");

    char *json_for_validate = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    char *json_for_import = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json_for_validate != NULL && json_for_import != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json_for_validate, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "setup: export succeeds (copy 1, for validate)");
    len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json_for_import, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "setup: export succeeds (copy 2, identically tampered, for import)");

    // Flip one hex nibble inside esp_blob_hex in both copies -- envelope
    // stays well-formed (same length, still valid hex), so this exercises
    // the HASH check specifically, not the envelope parser. Same recipe as
    // test_import_refuses_hash_mismatch_nothing_written() above (pkg_hash
    // itself is a quoted hex STRING, not a bare number -- corrupting it
    // directly breaks JSON structure and gets refused by the parser instead
    // of the hash comparison, which is not what this test means to exercise).
    {
        char *h1 = strstr(json_for_validate, "\"esp_blob_hex\":\"");
        char *h2 = strstr(json_for_import, "\"esp_blob_hex\":\"");
        TEST_CHECK(h1 != NULL && h2 != NULL, "found esp_blob_hex to tamper in both copies");
        if (h1) {
            char *digit = h1 + strlen("\"esp_blob_hex\":\"");
            *digit = (*digit == '0') ? '1' : '0';
        }
        if (h2) {
            char *digit = h2 + strlen("\"esp_blob_hex\":\"");
            *digit = (*digit == '0') ? '1' : '0';
        }
    }

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t before = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);

    char name_out[KILN_CFG_NAME_MAX_LEN + 1] = {0};
    uint16_t out_schema = 0;
    uint32_t out_hash = 0;
    char validate_reason[160] = {0};
    bool validate_ok = kiln_cfg_store_validate_package_json(json_for_validate, name_out, sizeof(name_out),
                                                            &out_schema, &out_hash, validate_reason,
                                                            sizeof(validate_reason));
    TEST_CHECK(!validate_ok, "validate refuses the hash-mismatched package");
    TEST_CHECK(strstr(validate_reason, "hash") != NULL, "reason names the hash mismatch specifically");

    int32_t new_id = -1;
    char import_reason[160] = {0};
    bool import_ok = kiln_cfg_store_import_package_json(json_for_import, &new_id, import_reason,
                                                        sizeof(import_reason));
    TEST_CHECK(!import_ok, "the committing call refuses the identically-tampered package too");
    TEST_CHECK(strcmp(validate_reason, import_reason) == 0,
              "validate-only gives the SAME reason string the committing call gives for identical input");

    uint8_t after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(after == before, "no slot was allocated by either the validate-only or the committing call");

    free(json_for_validate);
    free(json_for_import);
}

static void test_validate_package_json_succeeds_while_quarantined(void)
{
    TEST_SECTION("kiln_cfg_store_validate_package_json -- succeeds while the store is QUARANTINED, unlike "
                 "the committing call which still refuses");
    reset_state();

    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Pre-Quarantine", -1, &id1, reason, sizeof(reason)), "setup: save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "setup: export succeeds");
    // Give it a distinct name so import doesn't self-collide (same trick as
    // test_export_import_new_slot_round_trip() above).
    char *name_field = strstr(json, "\"name\":\"Pre-Quarantine\"");
    TEST_CHECK(name_field != NULL, "found the name field to rename before import");
    if (name_field) {
        name_field[strlen("\"name\":\"Pre-Quarantin")] = '3'; // same length
    }

    // Now corrupt the store's own NVS blob and reload -- same recipe as
    // test_corrupt_store_quarantines_and_blocks_writes() above.
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    kiln_cfg_store_blob_t garbage;
    memset(&garbage, 0xAB, sizeof(garbage));
    garbage.version = KILN_CFG_STORE_VERSION;
    hal_kv_set_blob(&h, NVS_KEY_STORE, &garbage, sizeof(garbage) - 1); // one byte short
    hal_kv_commit(&h);
    hal_kv_close(&h);
    TEST_CHECK(!nvs_load_store(), "setup: the wrong-size blob is not trustworthy");
    TEST_CHECK(kiln_cfg_store_is_quarantined(NULL, 0), "setup: the store is now quarantined");

    char name_out[KILN_CFG_NAME_MAX_LEN + 1] = {0};
    uint16_t out_schema = 0;
    uint32_t out_hash = 0;
    char validate_reason[160] = {0};
    bool validate_ok = kiln_cfg_store_validate_package_json(json, name_out, sizeof(name_out), &out_schema, &out_hash,
                                                            validate_reason, sizeof(validate_reason));
    TEST_CHECK(validate_ok, "validate-only succeeds even while the store is quarantined -- it never checks "
                           "quarantine, by design (the quarantine gate belongs on the committing path only)");

    int32_t new_id = -1;
    char import_reason[160] = {0};
    bool import_ok = kiln_cfg_store_import_package_json(json, &new_id, import_reason, sizeof(import_reason));
    TEST_CHECK(!import_ok, "the committing call still refuses while quarantined");
    TEST_CHECK(strstr(import_reason, "quarantine") != NULL, "the committing refusal names the quarantine");

    free(json);
}

static void test_import_package_json_as_honours_name_override(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json_as -- creates the slot under name_override, not the "
                 "name embedded in the file");
    reset_state();

    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Same Name", -1, &id1, reason, sizeof(reason)), "setup: save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "setup: export succeeds");

    // Deliberately do NOT rename the embedded name -- the whole point of
    // name_override is the "different identity, same name" collision case:
    // the file's own name ("Same Name") collides with id1's existing slot,
    // which the plain kiln_cfg_store_import_package_json() would refuse.
    int32_t id2 = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_import_package_json_as(json, "Renamed Copy", &id2, reason, sizeof(reason));
    TEST_CHECK(ok, "import_package_json_as succeeds under a distinct override name despite the file's own "
                  "name colliding with an existing slot");
    TEST_CHECK(id2 > 0 && id2 != id1, "a brand new slot was created");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    bool found_override = false, found_original_name_only_once = false;
    int original_name_count = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (strcmp(rows[i].name, "Renamed Copy") == 0) {
            found_override = true;
            TEST_CHECK(rows[i].id == id2, "the override-named row is the newly-created slot");
        }
        if (strcmp(rows[i].name, "Same Name") == 0) {
            original_name_count++;
        }
    }
    found_original_name_only_once = (original_name_count == 1);
    TEST_CHECK(found_override, "a slot named exactly \"Renamed Copy\" exists");
    TEST_CHECK(found_original_name_only_once,
              "\"Same Name\" still names exactly the ORIGINAL slot -- the override name never touched it");
}

static void test_import_package_json_as_rejects_invalid_override(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json_as -- rejects an invalid override name, writes nothing");
    reset_state();

    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Original", -1, &id1, reason, sizeof(reason)), "setup: save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "setup: export succeeds");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t before = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);

    // An empty/whitespace-only override name is invalid by the same
    // character-set/length rule every other stored name is held to (see
    // test_empty_or_whitespace_only_name_rejected() above).
    int32_t new_id = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_import_package_json_as(json, "   ", &new_id, reason, sizeof(reason));
    TEST_CHECK(!ok, "an empty/whitespace-only override name is refused");
    TEST_CHECK(reason[0] != '\0', "a specific reason is given");

    uint8_t after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(after == before, "no slot was allocated on refusal");

    free(json);
}

static void test_import_package_json_as_null_override_matches_plain_import(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json_as(..., NULL, ...) -- behaviourally identical to "
                 "kiln_cfg_store_import_package_json()");
    reset_state();

    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Base", -1, &id1, reason, sizeof(reason)), "setup: save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "setup: export succeeds");
    char *name_field = strstr(json, "\"name\":\"Base\"");
    TEST_CHECK(name_field != NULL, "found the name field to rename before import (avoid self-collision)");
    if (name_field) {
        name_field[strlen("\"name\":\"Bas")] = 'e' + 1; // "Base" -> "Basf", same length, distinct
    }

    int32_t id2 = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_import_package_json_as(json, NULL, &id2, reason, sizeof(reason));
    TEST_CHECK(ok, "a NULL override imports exactly as the plain function would");
    TEST_CHECK(id2 > 0 && id2 != id1, "a brand new slot was created, using the file's own embedded name");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t n = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    bool found = false;
    for (uint8_t i = 0; i < n; i++) {
        if (rows[i].id == id2) {
            TEST_CHECK(strcmp(rows[i].name, "Basf") == 0, "the slot's name is the file's own embedded name, "
                                                          "unchanged by a NULL override");
            found = true;
        }
    }
    TEST_CHECK(found, "the new slot is present in the listing");

    free(json);
}

// NEGATIVE TEST (task 3, per-instructions protocol): confirms
// validate_package_json_common()'s hash check is load-bearing, not a check
// that happens to never fire. Sabotaged by hand at review time -- see the
// commit message for the exact one-line change, the observed failure (this
// test's TEST_CHECK(!ok, ...) assertions flipping to FAIL), the by-hand
// restore, and the git hash-object verification performed before the fix
// was measured again. This function intentionally duplicates
// test_validate_package_json_refuses_bad_hash_same_reason_as_import() above
// rather than replacing it -- that test also proves the reason STRING
// matches between the two entry points, which is a distinct property from
// "the hash check exists at all".
static void test_validate_package_json_hash_check_is_load_bearing(void)
{
    TEST_SECTION("kiln_cfg_store_validate_package_json -- NEGATIVE-TEST-VERIFIED: the pkg_hash check is real, "
                 "not vacuous (see commit message for the sabotage/restore/rebuild protocol actually run)");
    reset_state();
    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Original", -1, &id1, reason, sizeof(reason)), "setup: save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "setup: export succeeds");
    // Flip one hex nibble inside esp_blob_hex -- same recipe as
    // test_import_refuses_hash_mismatch_nothing_written() (pkg_hash is a
    // quoted hex STRING; corrupting it directly breaks JSON structure and
    // gets refused by the envelope parser instead of the hash comparison).
    char *hexval = strstr(json, "\"esp_blob_hex\":\"");
    TEST_CHECK(hexval != NULL, "found esp_blob_hex to tamper");
    if (hexval) {
        char *digit = hexval + strlen("\"esp_blob_hex\":\"");
        *digit = (*digit == '0') ? '1' : '0';
    }

    char name_out[KILN_CFG_NAME_MAX_LEN + 1] = {0};
    uint16_t out_schema = 0;
    uint32_t out_hash = 0;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_validate_package_json(json, name_out, sizeof(name_out), &out_schema, &out_hash, reason,
                                                   sizeof(reason));
    TEST_CHECK(!ok, "a tampered esp_blob_hex must be refused via the hash check (this assertion is the one "
                   "that FAILED during the negative test, when the check was sabotaged out)");
    TEST_CHECK(strstr(reason, "hash") != NULL, "reason names the hash mismatch specifically");

    free(json);
}

static void test_import_refuses_malformed(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json -- refuses a malformed file, writes nothing");
    reset_state();
    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t before = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);

    int32_t new_id = -1;
    char reason[160] = {0};
    bool ok = kiln_cfg_store_import_package_json("{ this is not json at all", &new_id, reason, sizeof(reason));
    TEST_CHECK(!ok, "refuses garbage input");
    TEST_CHECK(reason[0] != '\0', "a specific reason is given, not silence");

    uint8_t after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(after == before, "no slot was allocated on refusal");
}

static void test_import_refuses_newer_pkg_schema_nothing_written(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json -- refuses a pkg_schema newer than this firmware "
                 "knows, writes nothing");
    reset_state();
    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Original", -1, &id1, reason, sizeof(reason)), "save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "export succeeds");

    char *p = strstr(json, "\"pkg_schema\":1");
    TEST_CHECK(p != NULL, "found pkg_schema to tamper (KILN_PKG_SCHEMA_VERSION is 1 today)");
    if (p) {
        p[strlen("\"pkg_schema\":")] = '9'; // unambiguously newer than known
    }

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t before = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    int32_t new_id = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_import_package_json(json, &new_id, reason, sizeof(reason));
    TEST_CHECK(!ok, "refuses the newer-than-known pkg_schema");
    TEST_CHECK(strstr(reason, "newer") != NULL, "reason names WHY (newer format version)");
    uint8_t after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(after == before, "no slot was allocated on refusal");

    free(json);
}

static void test_import_refuses_hash_mismatch_nothing_written(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json -- refuses a package whose declared pkg_hash does not "
                 "match its contents, writes nothing");
    reset_state();
    int32_t id1 = -1;
    char reason[160] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Original", -1, &id1, reason, sizeof(reason)), "save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "export succeeds");

    // Flip one hex nibble inside esp_blob_hex -- envelope stays well-formed
    // (same length, still valid hex), so this exercises the HASH check
    // specifically, not the envelope parser.
    char *hexval = strstr(json, "\"esp_blob_hex\":\"");
    TEST_CHECK(hexval != NULL, "found esp_blob_hex to tamper");
    if (hexval) {
        char *digit = hexval + strlen("\"esp_blob_hex\":\"");
        *digit = (*digit == '0') ? '1' : '0';
    }

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t before = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    int32_t new_id = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_import_package_json(json, &new_id, reason, sizeof(reason));
    TEST_CHECK(!ok, "refuses the tampered package on hash mismatch");
    TEST_CHECK(strstr(reason, "hash") != NULL, "reason names the hash mismatch specifically");
    uint8_t after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(after == before, "no slot was allocated on refusal");

    free(json);
}

static void test_import_reports_failure_when_persist_fails(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json -- Defect 4 "
                 "(docs/audits/kiln_profiles_feature_review_2026-09-15.md): reports FAILURE, not success, "
                 "when the store cannot be persisted to flash, and does not leave a phantom RAM-only slot");
    reset_state();

    int32_t id1 = -1;
    char reason[200] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Original", -1, &id1, reason, sizeof(reason)), "save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "export succeeds");
    char *name_digit = strstr(json, "\"name\":\"Original\"");
    TEST_CHECK(name_digit != NULL, "found the name field to rename before import");
    if (name_digit) {
        // Same length, different name so it does not collide with "Original".
        memcpy(name_digit, "\"name\":\"Imported\"", strlen("\"name\":\"Imported\""));
    }

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t before = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);

    fake_kv_set_write_safe_here(false); // simulate the NVS write failing (same hook the
                                         // PSRAM-stack guard test above uses)
    int32_t new_id = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_import_package_json(json, &new_id, reason, sizeof(reason));
    fake_kv_set_write_safe_here(true); // restore for every later test

    TEST_CHECK(!ok, "a persist failure is reported as FAILURE, not claimed success (this is the fix -- "
                    "the pre-fix code returned true here)");
    TEST_CHECK(reason[0] != '\0', "a specific reason is given, not silence");
    TEST_CHECK(new_id == -1, "out_id is left untouched/at its initial value on failure, never a phantom id");

    uint8_t after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(after == before, "no slot is left behind in RAM when the persist failed -- a slot that "
                                "cannot survive a reboot must not appear to exist either");

    // Bookkeeping check, not just a count coincidence (docs/audits/
    // review_autosave_slot_fix_a93ee77b_2026-09-15.md): the failed import's
    // slot-allocation bookkeeping (e->in_use in particular) must be fully
    // reversed, not merely "the count happens to still add up" -- prove it
    // by showing a SUBSEQUENT, succeeding import can still find and use a
    // free slot rather than seeing the store as full/corrupted from the
    // failed attempt's leftovers.
    int32_t retry_id = -1;
    reason[0] = '\0';
    bool retry_ok = kiln_cfg_store_import_package_json(json, &retry_id, reason, sizeof(reason));
    TEST_CHECK(retry_ok, "a later import (persist now working) succeeds -- the failed attempt's slot "
                         "bookkeeping did not leave the store thinking that slot is still occupied");
    TEST_CHECK(retry_id > 0, "the retry got a real slot id");
    uint8_t after_retry = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(after_retry == before + 1, "exactly one NEW slot exists after the retry -- not two (which "
                                          "would mean the failed attempt's slot was still occupied "
                                          "alongside the retry's own) and not zero");

    free(json);
}

static void test_import_refuses_abs_max_temp_c_tighter_than_zone_max(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json -- section 5.3 table row 1: refuses a package whose "
                 "Pico abs_max_temp_c ceiling is lower than its own highest configured zone max_temp_c");
    reset_state();
    test_stub_zones_set_thermo_count(1); // so zone 0's thermo_mask bit 0 passes the 5.2a hardware check

    zones_cfg_t cand;
    memset(&cand, 0, sizeof(cand));
    cand.zones[0].thermo_mask = 0x01;
    cand.zones[0].max_temp_c = 1200.0f;
    TEST_CHECK(sizeof(cand) <= sizeof(s_stub_export_content), "test assumption: zones_cfg_t fits the stub "
                                                              "content buffer");
    s_stub_blob_size = sizeof(cand); // governs both the stub export/canonical size AND, below,
                                     // the size kiln_cfg_store_import_package_json() re-derives
                                     // the canonical form at (via the same stub)

    kiln_pkg_safety_t pico;
    memset(&pico, 0, sizeof(pico));
    pico.count = 1;
    pico.entries[0].param_id = 0x0104u; // "abs_max_temp_c", safety_cfg_store.c's own table
    pico.entries[0].type = KILNLINK_PARAM_TYPE_F32;
    pico.entries[0].flags = KILN_PKG_PARAM_FLAG_SET;
    union { uint32_t bits; float f; } conv;
    conv.f = 500.0f; // tighter than the package's own 1200 C zone ceiling
    pico.entries[0].value_bits = conv.bits;

    uint32_t hash = 0;
    TEST_CHECK(kiln_package_compute_hash(1, (const uint8_t *)&cand, (uint16_t)sizeof(cand), &pico, &hash),
               "test setup: hash computation");

    char *json = (char *)malloc(KILN_PKG_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_package_export_json("TooHot", 1, (const uint8_t *)&cand, (uint16_t)sizeof(cand), &pico,
                                        hash, 0x11223344u, json, KILN_PKG_JSON_MAX_LEN, &len),
               "test setup: package JSON built");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t before = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    int32_t new_id = -1;
    char reason[200] = {0};
    bool ok = kiln_cfg_store_import_package_json(json, &new_id, reason, sizeof(reason));
    TEST_CHECK(!ok, "refuses a Pico ceiling tighter than the package's own zone max_temp_c");
    TEST_CHECK(strstr(reason, "abs_max_temp_c") != NULL, "reason names the field");
    uint8_t after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(after == before, "no slot was allocated on refusal");

    free(json);
}

static void test_import_refuses_abs_max_temp_c_above_firmware_ceiling(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json -- section 5.3 table row 1: refuses a Pico "
                 "abs_max_temp_c above this firmware's absolute sanity ceiling (ZONE_MAX_TEMP_C_MAX)");
    reset_state();
    test_stub_zones_set_thermo_count(1);

    zones_cfg_t cand;
    memset(&cand, 0, sizeof(cand));
    cand.zones[0].thermo_mask = 0x01;
    cand.zones[0].max_temp_c = 1200.0f;
    s_stub_blob_size = sizeof(cand);

    kiln_pkg_safety_t pico;
    memset(&pico, 0, sizeof(pico));
    pico.count = 1;
    pico.entries[0].param_id = 0x0104u;
    pico.entries[0].type = KILNLINK_PARAM_TYPE_F32;
    pico.entries[0].flags = KILN_PKG_PARAM_FLAG_SET;
    union { uint32_t bits; float f; } conv;
    conv.f = ZONE_MAX_TEMP_C_MAX + 1.0f; // over this firmware's absolute ceiling
    pico.entries[0].value_bits = conv.bits;

    uint32_t hash = 0;
    TEST_CHECK(kiln_package_compute_hash(1, (const uint8_t *)&cand, (uint16_t)sizeof(cand), &pico, &hash),
               "test setup: hash computation");

    char *json = (char *)malloc(KILN_PKG_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_package_export_json("WayTooHot", 1, (const uint8_t *)&cand, (uint16_t)sizeof(cand), &pico,
                                        hash, 0x11223344u, json, KILN_PKG_JSON_MAX_LEN, &len),
               "test setup: package JSON built");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t before = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    int32_t new_id = -1;
    char reason[200] = {0};
    bool ok = kiln_cfg_store_import_package_json(json, &new_id, reason, sizeof(reason));
    TEST_CHECK(!ok, "refuses a Pico ceiling above this firmware's absolute sanity ceiling");
    TEST_CHECK(strstr(reason, "abs_max_temp_c") != NULL, "reason names the field");
    uint8_t after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(after == before, "no slot was allocated on refusal");

    free(json);
}

static void test_import_refuses_missing_abs_max_temp_c_when_zone_configured(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json -- section 5.3 table row 1: refuses a package with a "
                 "configured zone but no Pico abs_max_temp_c at all, rather than assuming it is safe");
    reset_state();
    test_stub_zones_set_thermo_count(1);

    zones_cfg_t cand;
    memset(&cand, 0, sizeof(cand));
    cand.zones[0].thermo_mask = 0x01;
    cand.zones[0].max_temp_c = 1200.0f;
    s_stub_blob_size = sizeof(cand);

    kiln_pkg_safety_t pico;
    memset(&pico, 0, sizeof(pico)); // count == 0 -- no abs_max_temp_c entry packaged at all

    uint32_t hash = 0;
    TEST_CHECK(kiln_package_compute_hash(1, (const uint8_t *)&cand, (uint16_t)sizeof(cand), &pico, &hash),
               "test setup: hash computation");

    char *json = (char *)malloc(KILN_PKG_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_package_export_json("NoCeiling", 1, (const uint8_t *)&cand, (uint16_t)sizeof(cand), &pico,
                                        hash, 0x11223344u, json, KILN_PKG_JSON_MAX_LEN, &len),
               "test setup: package JSON built");

    int32_t new_id = -1;
    char reason[200] = {0};
    bool ok = kiln_cfg_store_import_package_json(json, &new_id, reason, sizeof(reason));
    TEST_CHECK(!ok, "refuses rather than assuming a missing ceiling is safe");
    TEST_CHECK(strstr(reason, "abs_max_temp_c") != NULL, "reason names the field");

    free(json);
}

static char *build_ceiling_test_json(const char *name, float zone_max_c, bool ceiling_set, float ceiling_c)
{
    zones_cfg_t cand;
    memset(&cand, 0, sizeof(cand));
    cand.zones[0].thermo_mask = 0x01;
    cand.zones[0].max_temp_c = zone_max_c;
    s_stub_blob_size = sizeof(cand);

    kiln_pkg_safety_t pico;
    memset(&pico, 0, sizeof(pico));
    if (ceiling_set) {
        union { uint32_t bits; float f; } conv;
        conv.f = ceiling_c;
        pico.count = 1;
        pico.entries[0].param_id = 0x0104u;
        pico.entries[0].type = KILNLINK_PARAM_TYPE_F32;
        pico.entries[0].flags = KILN_PKG_PARAM_FLAG_SET;
        pico.entries[0].value_bits = conv.bits;
    } else {
        // What kiln_cfg_store_save_current() captures when the safety cache was never commissioned: the
        // entry is PRESENT but flags == 0 (unset), exactly as in the 2026-10-04 bench backup.
        pico.count = 1;
        pico.entries[0].param_id = 0x0104u;
        pico.entries[0].type = KILNLINK_PARAM_TYPE_F32;
        pico.entries[0].flags = 0;
        pico.entries[0].value_bits = 0;
    }
    uint32_t hash = 0;
    TEST_CHECK(kiln_package_compute_hash(1, (const uint8_t *)&cand, (uint16_t)sizeof(cand), &pico, &hash),
               "test setup: hash computation");
    char *json = (char *)malloc(KILN_PKG_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_package_export_json(name, 1, (const uint8_t *)&cand, (uint16_t)sizeof(cand), &pico, hash,
                                        0x11223344u, json, KILN_PKG_JSON_MAX_LEN, &len),
               "test setup: package JSON built");
    return json;
}

static void test_restore_path_accepts_unset_ceiling_but_still_checks_a_set_one(void)
{
    TEST_SECTION("backup-restore path (validate_package_json / import_package_json_as with a name override): "
                 "a package whose Pico abs_max_temp_c is UNSET -- what the board's own export emits for a "
                 "never-commissioned slot -- restores; a SET but too-tight ceiling is still refused; the "
                 "hand-upload path (no name override) keeps its strict refusal");
    reset_state();
    test_stub_zones_set_thermo_count(1);

    char name_out[KILN_CFG_NAME_MAX_LEN + 1];
    uint16_t schema = 0;
    uint32_t hash_out = 0;
    char reason[200] = {0};

    char *unset = build_ceiling_test_json("Unset", 1200.0f, false, 0.0f);
    TEST_CHECK(kiln_cfg_store_validate_package_json(unset, name_out, sizeof(name_out), &schema, &hash_out, reason,
                                                    sizeof(reason)),
               "restore dry-run accepts an unset ceiling");
    int32_t new_id = -1;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_import_package_json_as(unset, "Unset", &new_id, reason, sizeof(reason)),
               "restore create (name override) accepts an unset ceiling");
    TEST_CHECK(new_id > 0, "a real slot was allocated");
    reason[0] = '\0';
    new_id = -1;
    TEST_CHECK(!kiln_cfg_store_import_package_json(unset, &new_id, reason, sizeof(reason)),
               "hand-upload path still refuses an unset ceiling");
    TEST_CHECK(strstr(reason, "abs_max_temp_c") != NULL, "reason names the field");
    free(unset);

    char *tight = build_ceiling_test_json("Tight", 1200.0f, true, 900.0f);
    reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_validate_package_json(tight, name_out, sizeof(name_out), &schema, &hash_out, reason,
                                                     sizeof(reason)),
               "restore dry-run still refuses a SET ceiling tighter than the zones");
    TEST_CHECK(strstr(reason, "abs_max_temp_c") != NULL, "reason names the field");
    reason[0] = '\0';
    TEST_CHECK(!kiln_cfg_store_import_package_json_as(tight, "Tight", &new_id, reason, sizeof(reason)),
               "restore create still refuses a SET ceiling tighter than the zones");
    free(tight);
}

static void test_import_accepts_abs_max_temp_c_at_or_above_zone_max(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json -- section 5.3 table row 1: a Pico ceiling AT OR ABOVE "
                 "the package's own zone max is accepted (the rule is >=, not >)");
    reset_state();
    test_stub_zones_set_thermo_count(1);

    zones_cfg_t cand;
    memset(&cand, 0, sizeof(cand));
    cand.zones[0].thermo_mask = 0x01;
    cand.zones[0].max_temp_c = 1200.0f;
    s_stub_blob_size = sizeof(cand);

    kiln_pkg_safety_t pico;
    memset(&pico, 0, sizeof(pico));
    pico.count = 1;
    pico.entries[0].param_id = 0x0104u;
    pico.entries[0].type = KILNLINK_PARAM_TYPE_F32;
    pico.entries[0].flags = KILN_PKG_PARAM_FLAG_SET;
    union { uint32_t bits; float f; } conv;
    conv.f = 1200.0f; // exactly equal -- must not be refused
    pico.entries[0].value_bits = conv.bits;

    uint32_t hash = 0;
    TEST_CHECK(kiln_package_compute_hash(1, (const uint8_t *)&cand, (uint16_t)sizeof(cand), &pico, &hash),
               "test setup: hash computation");

    char *json = (char *)malloc(KILN_PKG_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_package_export_json("JustRight", 1, (const uint8_t *)&cand, (uint16_t)sizeof(cand), &pico,
                                        hash, 0x11223344u, json, KILN_PKG_JSON_MAX_LEN, &len),
               "test setup: package JSON built");

    int32_t new_id = -1;
    char reason[200] = {0};
    bool ok = kiln_cfg_store_import_package_json(json, &new_id, reason, sizeof(reason));
    TEST_CHECK(ok, "an equal ceiling is accepted, not refused");
    TEST_CHECK(new_id > 0, "a real slot was allocated");

    free(json);
}

// ---------------------------------------------------------------------------
// Section 5.3 rows 2/3/4 at the kiln_cfg_store.c level (owner decisions
// 2026-09-16, Defect 5): the source_board_id-driven calibration reset and
// the ack_hardware_differs apply gate, both already covered at the
// kiln_package.c contract level (test_kiln_package.c), exercised here
// through the REAL kiln_cfg_store_save_current()/_export_package_json()/
// _import_package_json()/_apply() pipeline.
// ---------------------------------------------------------------------------

// Seeds the live safety_cfg_store cache (real module, linked once via
// test_safety_cfg_store.c's #include, see this file's top-of-file comment)
// with ct_cal[0].calibrated=true and i_normal_a[0] SET to a real, nonzero
// value, then a bool/topology triple all set, so kiln_cfg_store_save_current()
// -> kiln_package_capture_pico_half() captures a slot whose pico half has
// something for the foreign-board reset to actually clear.
static void seed_live_pico_for_board_id_tests(void)
{
    uint16_t seed_ids[] = { 0x0316u, 0x0109u, 0x031Fu, 0x0211u }; // ct_cal[0].calibrated, ct_installed,
                                                                  // ct_topology, safety_tc_installed
    uint16_t seed_vals[] = { 1u, 1u, 0u, 1u };
    test_safety_cfg_store_stage_page_for_kiln_cfg_store_test(0, /*more=*/true, seed_ids, seed_vals, 4);
    test_safety_cfg_store_stage_f32_for_kiln_cfg_store_test(1, 0x031Au, 5.5f); // i_normal_a[0], page 1 --
                                                                               // page 0 above must say
                                                                               // more=true or refetch()
                                                                               // stops before reading it
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    TEST_CHECK(safety_cfg_store_refetch(&fake_link, 0x00DDu), "test setup: live pico seed refetch succeeds");
}

// Looks up one param id's flags/value_bits in a captured kiln_pkg_safety_t --
// test-local helper, not production code.
static bool pico_find(const kiln_pkg_safety_t *pico, uint16_t param_id, uint8_t *flags_out,
                       uint32_t *value_bits_out)
{
    for (uint16_t i = 0; i < pico->count; i++) {
        if (pico->entries[i].param_id == param_id) {
            if (flags_out) {
                *flags_out = pico->entries[i].flags;
            }
            if (value_bits_out) {
                *value_bits_out = pico->entries[i].value_bits;
            }
            return true;
        }
    }
    return false;
}

static void test_import_cross_board_forces_calibration_reset(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json -- section 5.3 rows 2/3: a package whose "
                 "source_board_id differs from THIS board's own id has ct_cal[].calibrated forced to "
                 "SET/false and i_normal_a[] forced UNSET on the resulting new slot");
    reset_state();
    kiln_board_identity_set_test_override(true, 0xAAAAAAAAu);
    seed_live_pico_for_board_id_tests();

    int32_t id1 = -1;
    char reason[200] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("BoardA Kiln", -1, &id1, reason, sizeof(reason)), "save on board A "
                                                                                              "succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "export from board A succeeds, embedding board A's own id as source_board_id");
    char *name_digit = strstr(json, "\"name\":\"BoardA Kiln\"");
    TEST_CHECK(name_digit != NULL, "found the name field to rename before import");
    if (name_digit) {
        memcpy(name_digit, "\"name\":\"BoardB Kiln\"", strlen("\"name\":\"BoardB Kiln\""));
    }

    // Switch to a DIFFERENT board identity before importing -- exactly the
    // "upload this package on a different controller" scenario.
    kiln_board_identity_set_test_override(true, 0xBBBBBBBBu);
    int32_t id2 = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_import_package_json(json, &id2, reason, sizeof(reason));
    TEST_CHECK(ok, "import onto a different board still succeeds -- the mismatch degrades calibration, "
                   "it does not refuse the whole package");
    TEST_CHECK(id2 > 0, "a real slot was allocated");

    uint8_t blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint16_t blob_len = 0;
    kiln_pkg_safety_t pico2;
    TEST_CHECK(kiln_cfg_store_get_full_package(id2, blob, sizeof(blob), &blob_len, &pico2, NULL, 0),
               "read back the imported slot's pico half");
    uint8_t cal_flags = 0;
    uint32_t cal_bits = 0xFFFFFFFFu;
    TEST_CHECK(pico_find(&pico2, 0x0316u, &cal_flags, &cal_bits), "ct_cal[0].calibrated is present");
    TEST_CHECK((cal_flags & KILN_PKG_PARAM_FLAG_SET) != 0, "ct_cal[0].calibrated is SET (an explicit "
                                                            "known-false, not left unknown)");
    TEST_CHECK(cal_bits == 0, "ct_cal[0].calibrated's value is false");
    uint8_t i_normal_flags = 0xFF;
    TEST_CHECK(pico_find(&pico2, 0x031Au, &i_normal_flags, NULL), "i_normal_a[0] is present");
    TEST_CHECK((i_normal_flags & KILN_PKG_PARAM_FLAG_SET) == 0, "i_normal_a[0] is forced UNSET, not "
                                                                 "zeroed-but-set");

    kiln_board_identity_set_test_override(false, 0);
    free(json);
}

static void test_import_matching_board_preserves_calibration(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json -- section 5.3 rows 2/3: a package whose "
                 "source_board_id MATCHES this board's own id leaves ct_cal[].calibrated and i_normal_a[] "
                 "untouched");
    reset_state();
    kiln_board_identity_set_test_override(true, 0xCCCCCCCCu);
    seed_live_pico_for_board_id_tests();

    int32_t id1 = -1;
    char reason[200] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Same Board Kiln", -1, &id1, reason, sizeof(reason)),
               "save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "export succeeds");
    char *name_digit = strstr(json, "\"name\":\"Same Board Kiln\"");
    TEST_CHECK(name_digit != NULL, "found the name field to rename before import");
    if (name_digit) {
        memcpy(name_digit, "\"name\":\"Same Board Kil2\"", strlen("\"name\":\"Same Board Kil2\""));
    }

    // Board identity is UNCHANGED -- still 0xCCCCCCCCu -- when importing.
    int32_t id2 = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_import_package_json(json, &id2, reason, sizeof(reason));
    TEST_CHECK(ok, "import succeeds");
    TEST_CHECK(id2 > 0, "a real slot was allocated");

    uint8_t blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint16_t blob_len = 0;
    kiln_pkg_safety_t pico1, pico2;
    TEST_CHECK(kiln_cfg_store_get_full_package(id1, blob, sizeof(blob), &blob_len, &pico1, NULL, 0),
               "read back the original slot's pico half");
    TEST_CHECK(kiln_cfg_store_get_full_package(id2, blob, sizeof(blob), &blob_len, &pico2, NULL, 0),
               "read back the imported slot's pico half");

    uint8_t cal_flags1 = 0, cal_flags2 = 0;
    uint32_t cal_bits1 = 0, cal_bits2 = 0xFFFFFFFFu;
    TEST_CHECK(pico_find(&pico1, 0x0316u, &cal_flags1, &cal_bits1), "original: ct_cal[0].calibrated present");
    TEST_CHECK(pico_find(&pico2, 0x0316u, &cal_flags2, &cal_bits2), "imported: ct_cal[0].calibrated present");
    TEST_CHECK(cal_flags1 == cal_flags2 && cal_bits1 == cal_bits2, "calibrated is unchanged by a same-board "
                                                                    "round trip");
    TEST_CHECK((cal_flags2 & KILN_PKG_PARAM_FLAG_SET) != 0 && cal_bits2 != 0, "and it is still the TRUE "
                                                                               "calibration this test seeded, "
                                                                               "not incidentally false");

    uint8_t inorm_flags1 = 0, inorm_flags2 = 0;
    TEST_CHECK(pico_find(&pico1, 0x031Au, &inorm_flags1, NULL), "original: i_normal_a[0] present");
    TEST_CHECK(pico_find(&pico2, 0x031Au, &inorm_flags2, NULL), "imported: i_normal_a[0] present");
    TEST_CHECK((inorm_flags1 & KILN_PKG_PARAM_FLAG_SET) != 0, "original: i_normal_a[0] is SET (test setup "
                                                               "sanity)");
    TEST_CHECK(inorm_flags1 == inorm_flags2, "i_normal_a[0]'s SET-ness is unchanged by a same-board round "
                                              "trip");

    kiln_board_identity_set_test_override(false, 0);
    free(json);
}

static void test_import_absent_source_board_id_forces_calibration_reset(void)
{
    TEST_SECTION("kiln_cfg_store_import_package_json -- section 5.3 rows 2/3, fail-safe half: a package "
                 "with NO source_board_id field at all (an older package) is treated as foreign too, even "
                 "when the importing board's identity happens to equal the ORIGINAL exporting board's id");
    reset_state();
    kiln_board_identity_set_test_override(true, 0xDDDDDDDDu);
    seed_live_pico_for_board_id_tests();

    int32_t id1 = -1;
    char reason[200] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Old Package Kiln", -1, &id1, reason, sizeof(reason)),
               "save succeeds");

    char *json = (char *)malloc(KILN_CFG_EXPORT_JSON_MAX_LEN);
    TEST_CHECK(json != NULL, "test scratch alloc");
    size_t len = 0;
    TEST_CHECK(kiln_cfg_store_export_package_json(id1, json, KILN_CFG_EXPORT_JSON_MAX_LEN, &len, reason,
                                                  sizeof(reason)),
               "export succeeds");
    char *name_digit = strstr(json, "\"name\":\"Old Package Kiln\"");
    TEST_CHECK(name_digit != NULL, "found the name field to rename before import");
    if (name_digit) {
        memcpy(name_digit, "\"name\":\"Old Package Kil2\"", strlen("\"name\":\"Old Package Kil2\""));
    }

    // String-surgically remove the source_board_id field entirely -- same
    // technique test_kiln_package.c's test_import_source_board_id_absent()
    // uses at the kiln_package.c contract level, exercised here through the
    // real kiln_cfg_store.c import pipeline instead.
    char field[64];
    snprintf(field, sizeof(field), "\"source_board_id\":\"0x%08x\",", (unsigned)0xDDDDDDDDu);
    char *f = strstr(json, field);
    TEST_CHECK(f != NULL, "found source_board_id field to remove");
    if (f) {
        size_t field_len = strlen(field);
        memmove(f, f + field_len, strlen(f + field_len) + 1);
        len -= field_len;
    }

    // Board identity STAYS at 0xDDDDDDDDu -- the same id the package was
    // originally exported from -- so the only thing that can make this
    // import "foreign" is the field's absence, never a numeric mismatch.
    int32_t id2 = -1;
    reason[0] = '\0';
    bool ok = kiln_cfg_store_import_package_json(json, &id2, reason, sizeof(reason));
    TEST_CHECK(ok, "import succeeds despite the missing field -- absence degrades calibration, it does not "
                   "refuse the package");
    TEST_CHECK(id2 > 0, "a real slot was allocated");

    uint8_t blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint16_t blob_len = 0;
    kiln_pkg_safety_t pico2;
    TEST_CHECK(kiln_cfg_store_get_full_package(id2, blob, sizeof(blob), &blob_len, &pico2, NULL, 0),
               "read back the imported slot's pico half");
    uint8_t cal_flags = 0;
    uint32_t cal_bits = 0xFFFFFFFFu;
    TEST_CHECK(pico_find(&pico2, 0x0316u, &cal_flags, &cal_bits), "ct_cal[0].calibrated is present");
    TEST_CHECK((cal_flags & KILN_PKG_PARAM_FLAG_SET) != 0 && cal_bits == 0, "calibrated was fail-safe reset "
                                                                            "to a known false, same as the "
                                                                            "cross-board case, purely because "
                                                                            "the field was absent");
    uint8_t inorm_flags = 0xFF;
    TEST_CHECK(pico_find(&pico2, 0x031Au, &inorm_flags, NULL), "i_normal_a[0] is present");
    TEST_CHECK((inorm_flags & KILN_PKG_PARAM_FLAG_SET) == 0, "i_normal_a[0] is forced UNSET on an absent "
                                                              "source_board_id too");

    kiln_board_identity_set_test_override(false, 0);
    free(json);
}

static void test_apply_refuses_hardware_mismatch_without_ack(void)
{
    TEST_SECTION("kiln_cfg_store_apply -- section 5.3 table row 4: refuses when a saved slot's packaged "
                 "ct_installed/ct_topology/safety_tc_installed differs from what this controller's live "
                 "safety_cfg_store cache currently reports, unless ack_hardware_differs is true");
    reset_state();
    kiln_board_identity_set_test_override(true, 0xEEEEEEEEu);

    // Seed the live cache with ct_installed=1 (SET), matching what the
    // about-to-be-saved slot's own pico half will also capture (both come
    // from the SAME live cache at save time) -- then, AFTER saving, change
    // the LIVE cache's ct_installed to 0, simulating a real hardware-shape
    // change discovered later (a CT unplugged, a board reconfigured), so the
    // saved slot and the live board now genuinely disagree.
    uint16_t seed_ids[] = { 0x0109u, 0x031Fu, 0x0211u };
    uint16_t seed_vals[] = { 1u, 0u, 1u };
    test_safety_cfg_store_stage_page_for_kiln_cfg_store_test(0, false, seed_ids, seed_vals, 3);
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    TEST_CHECK(safety_cfg_store_refetch(&fake_link, 0x00EEu), "test setup: initial live seed refetch succeeds");

    int32_t id1 = -1;
    char reason[200] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("HW Kiln", -1, &id1, reason, sizeof(reason)), "save succeeds, "
                                                                                          "capturing "
                                                                                          "ct_installed=1");

    // Now the live board's ct_installed changes to 0 -- a real disagreement.
    uint16_t seed_vals2[] = { 0u, 0u, 1u };
    test_safety_cfg_store_stage_page_for_kiln_cfg_store_test(0, false, seed_ids, seed_vals2, 3);
    TEST_CHECK(safety_cfg_store_refetch(&fake_link, 0x00EFu), "test setup: changed live seed refetch "
                                                               "succeeds");

    reason[0] = '\0';
    bool refused = kiln_cfg_store_apply(id1, /*ack_no_safety_processor=*/true, /*ack_hardware_differs=*/false,
                                        reason, sizeof(reason));
    TEST_CHECK(!refused, "apply is refused without the ack");
    TEST_CHECK(strstr(reason, "ct_installed") != NULL, "reason names the specific differing field");

    kiln_board_identity_set_test_override(false, 0);
}

static void test_hardware_differs_message_fits_longest_label(void)
{
    TEST_SECTION("apply_hardware_differs() -- the formatted 428 message for the longest kFields[] label "
                 "('safety_tc_installed', the ct_installed/ct_topology/safety_tc_installed set apply_"
                 "hardware_differs() compares) fits with the sentence separator present, and no caller's "
                 "buffer truncates it (kiln_cfg_http.c's hw_msg and this file's own msg are both 256 "
                 "bytes). kFields[] itself is a local static inside apply_hardware_differs(), not file-"
                 "scope, so this asserts against the same three literal labels that array holds rather "
                 "than iterating it -- if a label is ever added or lengthened there, update this list too.");

    static const char *const kKnownLabels[] = { "ct_installed", "ct_topology", "safety_tc_installed" };
    size_t longest_label_len = 0;
    for (size_t f = 0; f < sizeof(kKnownLabels) / sizeof(kKnownLabels[0]); f++) {
        size_t l = strlen(kKnownLabels[f]);
        if (l > longest_label_len) {
            longest_label_len = l;
        }
    }
    TEST_CHECK(longest_label_len == strlen("safety_tc_installed"),
               "the longest known label is still 'safety_tc_installed' -- if this fails, a longer label "
               "was added to kFields[] and the 256-byte buffers may need re-checking");

    /* Drive the REAL apply_hardware_differs() (via its public wrapper) with a genuine
     * safety_tc_installed mismatch, rather than reformatting the same literal locally --
     * a self-contained reformat would pass even if the real source regressed. */
    reset_state();
    kiln_board_identity_set_test_override(true, 0xFFFFFFF2u);
    uint16_t seed_ids[] = { 0x0109u, 0x031Fu, 0x0211u };
    uint16_t seed_vals[] = { 1u, 0u, 1u };
    test_safety_cfg_store_stage_page_for_kiln_cfg_store_test(0, false, seed_ids, seed_vals, 3);
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    TEST_CHECK(safety_cfg_store_refetch(&fake_link, 0x00F0u), "test setup: initial live seed refetch "
                                                               "succeeds");

    int32_t id1 = -1;
    char reason[200] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("HW Longest Label", -1, &id1, reason, sizeof(reason)),
               "test setup: save succeeds");

    /* Change the live safety_tc_installed value so the saved slot now differs from live. */
    uint16_t seed_vals2[] = { 1u, 0u, 0u };
    test_safety_cfg_store_stage_page_for_kiln_cfg_store_test(0, false, seed_ids, seed_vals2, 3);
    TEST_CHECK(safety_cfg_store_refetch(&fake_link, 0x00F0u), "test setup: second live refetch succeeds");

    char msg[256] = {0};
    bool differs = kiln_cfg_store_slot_hardware_differs(id1, msg, sizeof(msg));
    TEST_CHECK(differs, "test setup: the real apply_hardware_differs() reports a mismatch for this slot");
    TEST_CHECK(strlen(msg) < sizeof(msg), "the real formatted message (with the separator) fits inside a "
                                          "256-byte buffer with room to spare");
    TEST_CHECK(strstr(msg, "applying. Resend") != NULL, "the sentence separator is present in the REAL "
                                                         "message -- 'applying' and 'Resend' are not "
                                                         "concatenated into 'applyingResend'/'applying "
                                                         "Resend'");
    TEST_CHECK(strstr(msg, "safety_tc_installed") != NULL, "the real message names the longest label");

    /* Review follow-up: the WRAPPER is not the only caller. kiln_cfg_store_apply()
     * has its own hw_msg buffer for the same message; drive that path too and
     * assert the message survives to the caller's reason intact, trailing period
     * included -- a too-small buffer there truncates the final '.' silently. */
    char apply_reason[256] = {0};
    bool refused = kiln_cfg_store_apply(id1, /*ack_no_safety_processor=*/true,
                                        /*ack_hardware_differs=*/false, apply_reason,
                                        sizeof(apply_reason));
    TEST_CHECK(!refused, "kiln_cfg_store_apply() refuses the same mismatch without the ack");
    TEST_CHECK(strstr(apply_reason, "X-Kiln-Ack-Hardware-Differs: 1.") != NULL,
               "kiln_cfg_store_apply()'s own hw_msg buffer holds the WHOLE message for the longest "
               "label, trailing period included -- not truncated at 192 bytes");

    kiln_board_identity_set_test_override(false, 0);
}

static void test_apply_allows_hardware_mismatch_with_ack(void)
{
    TEST_SECTION("kiln_cfg_store_apply -- section 5.3 table row 4: the SAME real mismatch as the refuse-"
                 "direction test succeeds once ack_hardware_differs is true -- the gate is a confirmation, "
                 "not a hard refusal");
    reset_state();
    kiln_board_identity_set_test_override(true, 0xFFFFFFF1u);

    uint16_t seed_ids[] = { 0x0109u, 0x031Fu, 0x0211u };
    uint16_t seed_vals[] = { 1u, 0u, 1u };
    test_safety_cfg_store_stage_page_for_kiln_cfg_store_test(0, false, seed_ids, seed_vals, 3);
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    TEST_CHECK(safety_cfg_store_refetch(&fake_link, 0x00F0u), "test setup: initial live seed refetch "
                                                               "succeeds");

    int32_t id1 = -1;
    char reason[200] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("HW Kiln 2", -1, &id1, reason, sizeof(reason)), "save succeeds");

    uint16_t seed_vals2[] = { 0u, 0u, 1u };
    test_safety_cfg_store_stage_page_for_kiln_cfg_store_test(0, false, seed_ids, seed_vals2, 3);
    TEST_CHECK(safety_cfg_store_refetch(&fake_link, 0x00F1u), "test setup: changed live seed refetch "
                                                               "succeeds");

    reason[0] = '\0';
    bool applied = kiln_cfg_store_apply(id1, /*ack_no_safety_processor=*/true, /*ack_hardware_differs=*/true,
                                        reason, sizeof(reason));
    TEST_CHECK(applied, "apply succeeds despite the real mismatch, because the caller explicitly "
                        "acknowledged it");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id1, "the slot is now active");

    kiln_board_identity_set_test_override(false, 0);
}

static void test_apply_refuses_live_ceiling_tighter_than_zone_max(void)
{
    TEST_SECTION("kiln_cfg_store_apply -- owner decision 2026-09-16: the apply-time live-ceiling re-check "
                 "refuses when THIS controller's LIVE safety_cfg_store abs_max_temp_c is lower than the "
                 "slot's highest configured zone max_temp_c, even though the slot's own PACKAGED "
                 "abs_max_temp_c (checked at import time) was fine");
    reset_state();
    test_stub_zones_set_thermo_count(1);
    zones_cfg_t cand;
    memset(&cand, 0, sizeof(cand));
    cand.zones[0].thermo_mask = 0x01;
    cand.zones[0].max_temp_c = 1200.0f;
    TEST_CHECK(sizeof(cand) <= sizeof(s_stub_export_content), "test assumption: zones_cfg_t fits the stub "
                                                              "content buffer");
    memcpy(s_stub_export_content, &cand, sizeof(cand));
    s_stub_blob_size = sizeof(cand);

    int32_t id1 = -1;
    char reason[200] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Ceiling Kiln", -1, &id1, reason, sizeof(reason)), "save succeeds, "
                                                                                              "capturing a "
                                                                                              "1200 C zone "
                                                                                              "ceiling");

    // The LIVE board's own abs_max_temp_c is now (re-)commissioned to
    // something LOWER than 1200 C -- e.g. a safety-processor firmware/
    // config change made after this slot was saved.
    test_safety_cfg_store_stage_f32_for_kiln_cfg_store_test(0, 0x0104u, 900.0f);
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    TEST_CHECK(safety_cfg_store_refetch(&fake_link, 0x0100u), "test setup: live ceiling seed refetch "
                                                              "succeeds");

    reason[0] = '\0';
    bool applied = kiln_cfg_store_apply(id1, /*ack_no_safety_processor=*/true, /*ack_hardware_differs=*/true,
                                        reason, sizeof(reason));
    TEST_CHECK(!applied, "apply is refused -- the live ceiling is tighter than the kiln's own zone max, "
                         "and NO ack bypasses this (it is never a confirmable hardware-shape difference)");
    TEST_CHECK(strstr(reason, "abs_max_temp_c") != NULL, "reason names the ceiling field");
}

static void test_apply_skips_live_ceiling_check_when_live_unset(void)
{
    TEST_SECTION("kiln_cfg_store_apply -- owner decision 2026-09-16: non-regression -- when the live "
                 "safety_cfg_store cache has no abs_max_temp_c reading at all (not yet commissioned, the "
                 "state every OTHER apply test in this file already runs under), the new re-check is "
                 "SKIPPED, never treated as a refusal");
    reset_state();
    test_stub_zones_set_thermo_count(1);
    zones_cfg_t cand;
    memset(&cand, 0, sizeof(cand));
    cand.zones[0].thermo_mask = 0x01;
    cand.zones[0].max_temp_c = 1200.0f;
    memcpy(s_stub_export_content, &cand, sizeof(cand));
    s_stub_blob_size = sizeof(cand);

    int32_t id1 = -1;
    char reason[200] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Uncommissioned Kiln", -1, &id1, reason, sizeof(reason)),
               "save succeeds");

    // No live seed/refetch at all -- the live cache starts and stays empty,
    // exactly like every pre-existing apply test in this file.
    reason[0] = '\0';
    bool applied = kiln_cfg_store_apply(id1, /*ack_no_safety_processor=*/true, /*ack_hardware_differs=*/true,
                                        reason, sizeof(reason));
    TEST_CHECK(applied, "apply succeeds -- an uncommissioned live board is not treated as a ceiling "
                        "violation");
}

// ---------------------------------------------------------------------------
// Cross-task autosave-override race -- LOW finding 5 of
// docs/audits/review_autosave_slot_fix_a93ee77b_2026-09-15.md ("cross-task
// observation"), re-affirmed by review_autosave_rework_5bc9afb5_2026-09-15.md.
//
// kiln_cfg_store_apply() and kiln_cfg_swap.c set s_autosave_target_override
// around zones_config_import_blob(), which commits the RAM config and only
// THEN calls nvs_save() -- a window spanning a blob decode, a JSON validate
// and an O(groups x zones) inheritance-cycle scan. An UNRELATED task's zones
// write landing in that window (confirmed reachable: uart_bridge_ext_control.c
// dispatches control_handle_message onto bx_flash_worker, and
// CONTROL_CMD_SET_ZONE_PID / SET_ZONE_MODEL both end in nvs_save()) read the
// override and wrote the OUTGOING config into the INCOMING slot, with
// populate_pico_half_and_hash() certifying it with a correct pkg_hash --
// silent, and not hash-detectable.
//
// The interloper is modelled exactly as it reaches this module: through the
// PLAIN entry point, carrying no dispatcher identity, while an override set
// by another task is live. It must land in the ACTIVE slot -- and it must
// still be WRITTEN. Suppressing it was explicitly rejected: that trades
// silent corruption for a silently dropped operator save.
static void test_autosave_override_ignores_a_foreign_dispatcher(void)
{
    TEST_SECTION("kiln_cfg_store autosave target override -- an UNRELATED task's autosave during an "
                 "in-flight import targets the ACTIVE slot, never the import's override slot, and is "
                 "still written rather than suppressed");
    reset_state();

    int32_t id_outgoing = -1;
    int32_t id_incoming = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Outgoing Kiln", -1, &id_outgoing, reason, sizeof(reason)),
               "test setup: the OUTGOING (active) slot exists");
    s_stub_export_content[0] = 0x5A;
    TEST_CHECK(kiln_cfg_store_save_current("Incoming Kiln", -1, &id_incoming, reason, sizeof(reason)),
               "test setup: the INCOMING slot (the one an apply() is importing into) exists");
    TEST_CHECK(id_outgoing != id_incoming, "test setup: two distinct slots");
    s_store.active_id = id_outgoing;

    // The interloper's live bytes -- distinct from anything saved so far.
    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 200);
    }

    // An import is in flight on ANOTHER task: the override names the slot
    // that import belongs to.
    kiln_cfg_store_set_autosave_target_override(id_incoming);
    bool ok = kiln_cfg_store_autosave_from_live(reason, sizeof(reason));
    kiln_cfg_store_set_autosave_target_override(KILN_CFG_AUTOSAVE_OVERRIDE_NONE);
    TEST_CHECK(ok, "the interloper's own save reports success");

    uint8_t blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint16_t blob_len = 0;
    TEST_CHECK(kiln_cfg_store_get_full_package(id_incoming, blob, sizeof(blob), &blob_len, NULL, NULL, 0),
               "read back the INCOMING slot");
    TEST_CHECK(blob[0] != 200,
               "the interloper's config was NOT written into the in-flight import's incoming slot -- this "
               "is the silent, hash-certified cross-slot corruption the dispatcher-identity fix closes");

    TEST_CHECK(kiln_cfg_store_get_full_package(id_outgoing, blob, sizeof(blob), &blob_len, NULL, NULL, 0),
               "read back the ACTIVE (outgoing) slot");
    TEST_CHECK(blob[0] == 200,
               "the interloper's save landed in the ACTIVE slot -- it was neither dropped nor deferred, "
               "which is the branch the owner explicitly rejected");
}

// The other direction: the import's OWN autosave, carrying the identity of
// the task that set the override, must still be steered by it -- otherwise
// the fix above would have simply disabled the override and re-opened
// Defect 1 (an import's autosave overwriting the OUTGOING kiln's slot).
static void test_autosave_override_applies_to_its_own_dispatcher(void)
{
    TEST_SECTION("kiln_cfg_store autosave target override -- the IMPORT'S OWN autosave, dispatched by the "
                 "task that set the override, still targets the override slot (Defect 1 stays fixed)");
    reset_state();

    int32_t id_outgoing = -1;
    int32_t id_incoming = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Outgoing Kiln", -1, &id_outgoing, reason, sizeof(reason)),
               "test setup: the OUTGOING (active) slot exists");
    s_stub_export_content[0] = 0x5A;
    TEST_CHECK(kiln_cfg_store_save_current("Incoming Kiln", -1, &id_incoming, reason, sizeof(reason)),
               "test setup: the INCOMING slot exists");
    s_store.active_id = id_outgoing;

    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 200);
    }

    // Same task sets the override and dispatches the autosave -- exactly the
    // shape zones_config_store.c's nvs_save() produces for an import.
    kiln_cfg_store_set_autosave_target_override(id_incoming);
    bool ok = kiln_cfg_store_autosave_from_live_for_dispatcher((void *)xTaskGetCurrentTaskHandle(), reason,
                                                               sizeof(reason));
    kiln_cfg_store_set_autosave_target_override(KILN_CFG_AUTOSAVE_OVERRIDE_NONE);
    TEST_CHECK(ok, "the import's own autosave succeeds");

    uint8_t blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint16_t blob_len = 0;
    TEST_CHECK(kiln_cfg_store_get_full_package(id_incoming, blob, sizeof(blob), &blob_len, NULL, NULL, 0),
               "read back the INCOMING slot");
    TEST_CHECK(blob[0] == 200,
               "the import's own autosave DID follow the override into the incoming slot");

    TEST_CHECK(kiln_cfg_store_get_full_package(id_outgoing, blob, sizeof(blob), &blob_len, NULL, NULL, 0),
               "read back the OUTGOING slot");
    TEST_CHECK(blob[0] != 200,
               "the OUTGOING kiln's slot was left alone -- Defect 1 is still fixed");
}

// A FOREIGN, NON-NULL dispatcher: the interloper is itself a dispatched flash-
// worker job (the PC control bridge's SET_ZONE_PID reaches nvs_save() from
// bx_flash_worker), so it arrives with a perfectly valid task handle that is
// simply NOT the one that set the override. Distinguishing "non-NULL" from
// "the owner" is the entire content of the fix -- a check that only rejected
// NULL would still mis-steer this save into the in-flight import's slot.
static void test_autosave_override_ignores_a_foreign_nonnull_dispatcher(void)
{
    TEST_SECTION("kiln_cfg_store autosave target override -- a FOREIGN but non-NULL dispatcher is not the "
                 "owner, so the override does not apply to it");
    reset_state();

    int32_t id_outgoing = -1;
    int32_t id_incoming = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Outgoing Kiln", -1, &id_outgoing, reason, sizeof(reason)),
               "test setup: the OUTGOING (active) slot exists");
    s_stub_export_content[0] = 0x5A;
    TEST_CHECK(kiln_cfg_store_save_current("Incoming Kiln", -1, &id_incoming, reason, sizeof(reason)),
               "test setup: the INCOMING slot exists");
    s_store.active_id = id_outgoing;

    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 200);
    }

    // The owner is whatever xTaskGetCurrentTaskHandle() returns under the host
    // stubs; this handle is deliberately a different, non-NULL value.
    void *foreign_task = (void *)0x7E57F0F0u;
    TEST_CHECK(foreign_task != (void *)xTaskGetCurrentTaskHandle(),
               "test setup: the foreign handle really is a different task than the override's owner");

    kiln_cfg_store_set_autosave_target_override(id_incoming);
    bool ok = kiln_cfg_store_autosave_from_live_for_dispatcher(foreign_task, reason, sizeof(reason));
    kiln_cfg_store_set_autosave_target_override(KILN_CFG_AUTOSAVE_OVERRIDE_NONE);
    TEST_CHECK(ok, "the interloper's own save still reports success");

    uint8_t blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint16_t blob_len = 0;
    TEST_CHECK(kiln_cfg_store_get_full_package(id_incoming, blob, sizeof(blob), &blob_len, NULL, NULL, 0),
               "read back the INCOMING slot");
    TEST_CHECK(blob[0] != 200,
               "a foreign NON-NULL dispatcher must NOT be steered into the in-flight import's incoming "
               "slot -- rejecting only NULL would leave this silent cross-slot corruption wide open");

    TEST_CHECK(kiln_cfg_store_get_full_package(id_outgoing, blob, sizeof(blob), &blob_len, NULL, NULL, 0),
               "read back the OUTGOING (active) slot");
    TEST_CHECK(blob[0] == 200,
               "the foreign dispatcher's save landed in the genuinely ACTIVE slot -- neither dropped nor "
               "deferred, the branch the owner explicitly rejected");
}

static void test_autosave_from_live_noop_with_no_active_config(void)
{
    TEST_SECTION("kiln_cfg_store_autosave_from_live -- no-op (success, nothing written) with no active config");
    reset_state();
    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    uint8_t before = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)), "reports success, not a failure");
    uint8_t after = kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT);
    TEST_CHECK(after == before, "nothing was allocated -- there is no active slot to autosave into");
}

static void test_autosave_from_live_updates_active_slot_and_hash(void)
{
    TEST_SECTION("kiln_cfg_store_autosave_from_live -- re-saves the live config over the ACTIVE slot and "
                 "recomputes pkg_hash");
    reset_state();
    int32_t id1 = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Live Kiln", -1, &id1, reason, sizeof(reason)), "save succeeds");
    uint32_t hash_before = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_before);

    // Simulate "the user made a change" -- the live config (this test's
    // stub) now exports different bytes.
    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 100);
    }

    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)), "autosave succeeds");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id1, "autosave does not change which slot is active");

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    TEST_CHECK(kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT) == 1,
               "autosave overwrote the existing slot -- it did not allocate a second one");

    uint8_t blob[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint16_t blob_len = 0;
    TEST_CHECK(kiln_cfg_store_get_full_package(id1, blob, sizeof(blob), &blob_len, NULL, NULL, 0),
               "read back the autosaved slot");
    TEST_CHECK(blob[0] == 100, "the autosaved slot now holds the NEW live bytes, not the original ones");

    uint32_t hash_after = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_after);
    TEST_CHECK(hash_after != hash_before, "pkg_hash was recomputed over the new content");
}

static void test_autosave_keeps_existing_pico_half_while_cache_unfetched(void)
{
    TEST_SECTION("kiln_cfg_store_autosave_from_live -- an unfetched safety cache defers the Pico-half "
                 "recapture like a divergence: the slot's existing good half is kept, never zeroed or "
                 "overwritten with an all-unset one, and the recapture is owed until the cache is fetched");
    reset_state();
    int32_t id1 = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Live Kiln", -1, &id1, reason, sizeof(reason)), "save succeeds");
    int idx = find_index_by_id(id1);
    TEST_CHECK(idx >= 0 && s_store.entries[idx].pico_populated, "setup: slot starts with a populated Pico half");
    kiln_pkg_safety_t half_before = s_store.entries[idx].pico;

    test_safety_cfg_store_mark_unfetched_for_kiln_cfg_store_test();
    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 100);
    }
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)), "autosave reports success");
    TEST_CHECK(reason[0] != '\0', "reason explains the deferral");
    idx = find_index_by_id(id1);
    TEST_CHECK(idx >= 0 && s_store.entries[idx].pico_populated, "the Pico half is still populated");
    TEST_CHECK(memcmp(&s_store.entries[idx].pico, &half_before, sizeof(half_before)) == 0,
               "the existing Pico half is byte-identical -- not zeroed or replaced");
    TEST_CHECK(s_store.entries[idx].pkg_hash != 0, "pkg_hash still a real hash");
    TEST_CHECK(kiln_cfg_store_pico_half_recapture_pending(), "a recapture is owed");

    test_safety_cfg_store_mark_fetched_for_kiln_cfg_store_test();
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)), "autosave succeeds once fetched");
    TEST_CHECK(!kiln_cfg_store_pico_half_recapture_pending(), "the owed recapture ran and cleared the flag");
}

static void test_autosave_from_live_suppressed_while_diverged(void)
{
    TEST_SECTION("kiln_cfg_store_autosave_from_live -- HIGH 1 rework (review_divergence_rework_c1d2c526_"
                 "2026-09-15.md): a latched divergence (ceiling or standing) defers only the PICO-HALF "
                 "recapture, never the ESP half -- full suppression would leave autosave off forever "
                 "with no self-clear trigger. The dirty flag records the deferred recapture and it must "
                 "never report a silent, unqualified success while one is outstanding");
    reset_state();
    int32_t id1 = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Live Kiln", -1, &id1, reason, sizeof(reason)), "save succeeds");
    TEST_CHECK(!kiln_cfg_store_pico_half_recapture_pending(), "no recapture pending right after an explicit save");
    uint32_t hash_before = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_before);

    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 100);
    }

    // Ceiling-off divergence latched: the ESP half is still saved (the slot
    // must keep tracking live zones/autotune/coupling edits regardless of
    // Pico divergence) and reason_out explains the deferred Pico-half
    // recapture; the dirty flag records it.
    s_stub_ceiling_diverged = true;
    s_stub_standing_diverged = false;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)),
               "reports success (a deferred recapture is not a failure)");
    uint32_t hash_after_ceiling = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_after_ceiling);
    TEST_CHECK(hash_after_ceiling != hash_before,
               "the ESP half WAS saved (and pkg_hash recomputed) while ceiling divergence is latched -- "
               "only the Pico-half recapture is deferred, per the HIGH 1 fix");
    TEST_CHECK(reason[0] != '\0', "reason explains the deferred recapture, never a silent empty string");
    TEST_CHECK(kiln_cfg_store_pico_half_recapture_pending(),
               "the dirty flag now records a deferred Pico-half recapture");

    // Standing (non-ceiling) warning alone must ALSO defer the recapture --
    // the plan's rule 6 covers any known disagreement, not only the
    // heat-disabling one.
    s_stub_ceiling_diverged = false;
    s_stub_standing_diverged = true;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)), "reports success while standing-diverged");
    TEST_CHECK(reason[0] != '\0', "reason explains the deferred recapture");
    TEST_CHECK(kiln_cfg_store_pico_half_recapture_pending(), "recapture still pending under a standing divergence");

    // Once both clear, the very next autosave call self-clears the dirty
    // flag -- this is the "retry once divergence clears" behavior the HIGH 1
    // fix requires, with no separate poller needed inside this module.
    s_stub_ceiling_diverged = false;
    s_stub_standing_diverged = false;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)), "autosave succeeds once agreement resumes");
    TEST_CHECK(!kiln_cfg_store_pico_half_recapture_pending(),
               "the deferred Pico-half recapture caught up and cleared the dirty flag");

    s_stub_ceiling_diverged = false;
    s_stub_standing_diverged = false;
}

static void test_autosave_from_live_suppressed_while_swap_pending(void)
{
    TEST_SECTION("kiln_cfg_store_autosave_from_live -- MEDIUM fix (review_autosave_rework_5bc9afb5_"
                 "2026-09-15.md): suppressed while a kiln_cfg_swap.c transaction has a pending record, "
                 "even with BOTH divergence flags reading false -- the latch alone is not enough because "
                 "the swap moves active_id to the target kiln before its own divergence check runs, and a "
                 "racing recapture is what would clear that latch");
    reset_state();
    int32_t id1 = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Live Kiln", -1, &id1, reason, sizeof(reason)), "save succeeds");
    uint32_t hash_before = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_before);

    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 100);
    }

    kiln_cfg_store_set_swap_pending_source(stub_swap_is_pending);
    s_stub_swap_pending = true;
    s_stub_ceiling_diverged = false;
    s_stub_standing_diverged = false;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)),
               "reports success (a suppressed autosave is not an error) while a swap is pending");
    uint32_t hash_after_pending = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_after_pending);
    TEST_CHECK(hash_after_pending == hash_before, "slot was NOT overwritten while a swap record is pending, "
                                                   "even though neither divergence flag is set");
    TEST_CHECK(reason[0] != '\0', "reason explains the suppression");

    s_stub_swap_pending = false;
    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)),
               "autosave succeeds once the pending swap record clears");
    uint32_t hash_after_clear = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_after_clear);
    TEST_CHECK(hash_after_clear != hash_before, "slot IS updated once no swap is pending and no divergence "
                                                 "is latched");

    kiln_cfg_store_set_swap_pending_source(NULL);
    s_stub_swap_pending = false;
}

static void test_recapture_pico_half_confirmed_bypasses_divergence_gate(void)
{
    TEST_SECTION("kiln_cfg_store_recapture_pico_half_confirmed -- MEDIUM 3 fix (review_divergence_rework_"
                 "c1d2c526_2026-09-15.md, HIGH3 race in safety_cfg_http.c's commissioning handler): a "
                 "caller that has just confirmed its OWN push to the Pico must be able to recapture the "
                 "active slot's Pico half immediately, even while the standing-diverged latch still reads "
                 "true (the very push being confirmed is often what would clear it) -- gating this call on "
                 "that latch the way the ordinary autosave path does would deadlock this caller");
    reset_state();
    int32_t id1 = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Live Kiln", -1, &id1, reason, sizeof(reason)), "save succeeds");
    uint32_t hash_before = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_before);

    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 100);
    }

    // Simulate the exact HIGH3 race: a poll tick landed in the gap and set
    // standing divergence between the commissioning handler's own Pico
    // cache refresh and its NVS write/autosave call.
    s_stub_ceiling_diverged = false;
    s_stub_standing_diverged = true;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_recapture_pico_half_confirmed(reason, sizeof(reason)),
               "confirmed-push recapture succeeds despite the standing-diverged latch reading true");
    uint32_t hash_after = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_after);
    TEST_CHECK(hash_after != hash_before,
               "the slot WAS updated (ESP half + recaptured Pico half) -- unlike the ordinary autosave "
               "path, this entry point does not defer on a latched divergence");
    TEST_CHECK(!kiln_cfg_store_pico_half_recapture_pending(),
               "no recapture is left pending after a successful confirmed recapture");

    s_stub_ceiling_diverged = false;
    s_stub_standing_diverged = false;
}

static void test_recapture_pico_half_confirmed_suppressed_while_swap_pending(void)
{
    TEST_SECTION("kiln_cfg_store_recapture_pico_half_confirmed -- still honors the swap-pending gate, "
                 "which is an unrelated reason to defer (a kiln_cfg_swap.c transaction mid-flight), not "
                 "the divergence latch this function is deliberately built to bypass");
    reset_state();
    int32_t id1 = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Live Kiln", -1, &id1, reason, sizeof(reason)), "save succeeds");
    uint32_t hash_before = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_before);

    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 100);
    }

    kiln_cfg_store_set_swap_pending_source(stub_swap_is_pending);
    s_stub_swap_pending = true;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_recapture_pico_half_confirmed(reason, sizeof(reason)),
               "reports success (a suppressed recapture is not an error) while a swap is pending");
    uint32_t hash_after = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_after);
    TEST_CHECK(hash_after == hash_before, "slot was NOT overwritten while a swap record is pending");
    TEST_CHECK(reason[0] != '\0', "reason explains the suppression");

    kiln_cfg_store_set_swap_pending_source(NULL);
    s_stub_swap_pending = false;
}

static void test_autosave_defers_when_cache_generation_moved_since_latch_evaluated(void)
{
    TEST_SECTION("kiln_cfg_store_autosave_from_live -- MEDIUM 3 fix (review_divergence_fixes_b2e7017f_"
                 "2026-09-15.md): both divergence latches reading clear is not enough -- if safety_cfg_"
                 "store's cache generation has moved past what the latch last evaluated, a refetch is "
                 "racing in and the latch's 'not diverged' verdict proves nothing about the value about "
                 "to be captured, so autosave must defer the Pico-half recapture exactly as if diverged");
    reset_state();
    int32_t id1 = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Live Kiln", -1, &id1, reason, sizeof(reason)), "save succeeds");
    uint32_t hash_before = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_before);

    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 100);
    }

    // Both latches clear, but the cache generation has moved on since the
    // latch was last evaluated against it -- simulates a refetch landing in
    // the exact window between safety_link_poll.c's maybe_refetch() and its
    // own enforce_ceiling_divergence() call, same task, same tick. The
    // generation counter is REAL (safety_cfg_store.c is linked for real into
    // this executable, see this file's own top comment and the MEDIUM 4 test
    // below), so the latch is pinned BEHIND it by actually performing one
    // real refetch here, rather than by writing an arbitrary stub number.
    s_stub_ceiling_diverged = false;
    s_stub_standing_diverged = false;
    uint32_t gen_before_refetch = safety_cfg_store_cache_generation();
    s_stub_latch_evaluated_generation = gen_before_refetch; // latch last saw the PRE-race generation
    uint16_t seed_ids2[] = { 0x0203u };
    uint16_t seed_vals2[] = { 45u };
    test_safety_cfg_store_stage_page_for_kiln_cfg_store_test(0, false, seed_ids2, seed_vals2, 1);
    SafetyLinkClass fake_link2;
    memset(&fake_link2, 0, sizeof(fake_link2));
    TEST_CHECK(safety_cfg_store_refetch(&fake_link2, 0x00DD), "the racing refetch itself succeeds");
    uint32_t gen_after_refetch = safety_cfg_store_cache_generation();
    TEST_CHECK(gen_after_refetch != gen_before_refetch, "the real refetch actually bumped the generation");
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)),
               "reports success (a deferred recapture is not a failure)");
    uint32_t hash_after_race = 0;
    kiln_cfg_store_get_package_identity(id1, NULL, NULL, &hash_after_race);
    TEST_CHECK(hash_after_race != hash_before, "the ESP half still saves even while the recapture defers");
    TEST_CHECK(kiln_cfg_store_pico_half_recapture_pending(),
               "the generation mismatch alone defers the Pico-half recapture, exactly like a real "
               "divergence would, even though both latches read false");
    TEST_CHECK(reason[0] != '\0', "reason explains the deferral");

    // Once the latch catches up to the same generation the cache is on, the
    // deferred recapture proceeds on the next call.
    s_stub_latch_evaluated_generation = gen_after_refetch;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)), "autosave succeeds once caught up");
    TEST_CHECK(!kiln_cfg_store_pico_half_recapture_pending(),
               "the deferred recapture clears once the latch's evaluated generation matches the cache's");

    s_stub_ceiling_diverged = false;
    s_stub_standing_diverged = false;
    s_stub_latch_evaluated_generation = 0;
}

static void test_capture_expected_pico_fields_excludes_abs_max_and_tc_type(void)
{
    TEST_SECTION("kiln_cfg_store_capture_expected_pico_fields -- MEDIUM 4 fix (review_divergence_fixes_"
                 "b2e7017f_2026-09-15.md): tc_type (0x0105) must be excluded from the broadened standing-"
                 "divergence field set, same as abs_max_temp_c (0x0104) -- the commissioning page owns tc_"
                 "type and the ESP has no push path for it, so including it would count every Pico-side tc_"
                 "type change (including a post-reboot default) as a standing divergence the ESP can never "
                 "clear, wedging the deferred-recapture autosave gate forever");
    reset_state();
    int32_t id1 = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Live Kiln", -1, &id1, reason, sizeof(reason)), "save succeeds");

    // Seed the REAL safety_cfg_store cache (linked for real into this same
    // executable via test_safety_cfg_store.c's #include, see this file's own
    // top-of-file comment) with abs_max_temp_c, tc_type, and one ordinary
    // broadened field, all reported SET by the (fake) Pico -- then recapture
    // via the confirmed entry point (bypasses the divergence gate, same as
    // test_recapture_pico_half_confirmed_bypasses_divergence_gate above) so
    // kiln_package_capture_pico_half() packages exactly these three fields
    // through the real capture path, not a fabricated package.
    uint16_t seed_ids[] = { 0x0104u, 0x0105u, 0x0203u }; // abs_max_temp_c, tc_type, overshoot_time_s
    uint16_t seed_vals[] = { 1250u, 2u, 30u };
    test_safety_cfg_store_stage_page_for_kiln_cfg_store_test(0, false, seed_ids, seed_vals, 3);
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    TEST_CHECK(safety_cfg_store_refetch(&fake_link, 0x00CC), "seeding refetch succeeds");

    TEST_CHECK(kiln_cfg_store_recapture_pico_half_confirmed(reason, sizeof(reason)), "recapture succeeds");

    safety_ceiling_expected_param_t out[SAFETY_CEILING_SYNC_MAX_STANDING_FIELDS];
    size_t n = kiln_cfg_store_capture_expected_pico_fields(out, sizeof(out) / sizeof(out[0]));
    TEST_CHECK(n == 1, "exactly one field survives -- abs_max_temp_c and tc_type are both excluded");
    if (n == 1) {
        TEST_CHECK(out[0].param_id == 0x0203u, "the surviving field is the ordinary broadened one");
    }
}

static void test_set_active_id_raw_drops_pending_flag_owned_by_old_slot(void)
{
    TEST_SECTION("kiln_cfg_store_set_active_id_raw -- LOW 7 fix (review_divergence_fixes_b2e7017f_"
                 "2026-09-15.md): a pending Pico-half recapture flag records WHICH slot it is owed for, "
                 "and switching active_id away from that slot drops the flag rather than silently letting "
                 "it apply to the new slot or leaving it orphaned forever");
    reset_state();
    int32_t id1 = -1, id2 = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Kiln A", -1, &id1, reason, sizeof(reason)), "save A succeeds");
    TEST_CHECK(kiln_cfg_store_save_current("Kiln B", -1, &id2, reason, sizeof(reason)), "save B succeeds");
    TEST_CHECK(kiln_cfg_store_set_active_id_raw(id1, reason, sizeof(reason)), "active id set back to A");

    // Defer a recapture while A is active.
    s_stub_ceiling_diverged = true;
    reason[0] = '\0';
    TEST_CHECK(kiln_cfg_store_autosave_from_live(reason, sizeof(reason)), "autosave defers under divergence");
    TEST_CHECK(kiln_cfg_store_pico_half_recapture_pending(), "recapture is pending, owed to slot A");

    // Switch active_id to B -- the flag was owed to A, which is no longer
    // active, so it must be dropped rather than silently later applying to
    // B once B's own recapture path runs.
    TEST_CHECK(kiln_cfg_store_set_active_id_raw(id2, reason, sizeof(reason)), "active id switches to B");
    TEST_CHECK(!kiln_cfg_store_pico_half_recapture_pending(),
               "the pending flag was dropped -- it was owed to slot A, not B, and A is no longer active");

    s_stub_ceiling_diverged = false;
}

// ui_page_home_refresh.c calls kiln_cfg_store_count() -- NOT
// kiln_cfg_store_list() -- because only whether the count reaches the
// "2 or more" owner threshold matters, never the entries themselves, and a
// count-only accessor needs no kiln_cfg_summary_t buffer at all on the
// 8192 B LVGL task stack. Prove the new accessor agrees with the entry list
// it replaced (the two must not drift: same in_use tally, same threshold
// verdict) at 1, 2 and 5 saved configs.
static void test_home_rail_kiln_count_matches_list_and_decides_threshold(void)
{
    TEST_SECTION("kiln_cfg_store_count() -- agrees with kiln_cfg_store_list() and decides the "
                 ">= 2 suffix threshold at 1, 2 and 5 saved configs (LVGL stack fix)");
    reset_state();

    kiln_cfg_summary_t rows[KILN_CFG_MAX_COUNT];
    char reason[96] = {0};
    int32_t id = -1;

    // 1 config: count is 1, threshold says hidden.
    TEST_CHECK(kiln_cfg_store_save_current("Kiln A", -1, &id, reason, sizeof(reason)), "save 1st succeeds");
    uint8_t n = kiln_cfg_store_count();
    TEST_CHECK(n == 1, "1 in_use: count returns 1");
    TEST_CHECK(n == kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT), "count matches an uncapped list");
    TEST_CHECK(!ui_page_home_kiln_suffix_visible(n), "1 config: suffix hidden");

    // 2 configs: count is 2, threshold says visible.
    TEST_CHECK(kiln_cfg_store_save_current("Kiln B", -1, &id, reason, sizeof(reason)), "save 2nd succeeds");
    n = kiln_cfg_store_count();
    TEST_CHECK(n == 2, "2 in_use: count returns 2");
    TEST_CHECK(n == kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT), "count matches an uncapped list");
    TEST_CHECK(ui_page_home_kiln_suffix_visible(n), "2 configs: suffix visible");

    // 5 configs: count reports the true total (it is never capped, unlike
    // kiln_cfg_store_list()'s out_cap-limited return), and still decides
    // the threshold the same way.
    TEST_CHECK(kiln_cfg_store_save_current("Kiln C", -1, &id, reason, sizeof(reason)), "save 3rd succeeds");
    TEST_CHECK(kiln_cfg_store_save_current("Kiln D", -1, &id, reason, sizeof(reason)), "save 4th succeeds");
    TEST_CHECK(kiln_cfg_store_save_current("Kiln E", -1, &id, reason, sizeof(reason)), "save 5th succeeds");
    n = kiln_cfg_store_count();
    TEST_CHECK(n == 5, "5 in_use: count returns the true total, uncapped");
    TEST_CHECK(n == kiln_cfg_store_list(rows, KILN_CFG_MAX_COUNT), "count matches an uncapped list");
    TEST_CHECK(ui_page_home_kiln_suffix_visible(n), "5 configs: suffix still visible");
}

static void test_apply_autosave_targets_incoming_slot_via_real_override(void)
{
    TEST_SECTION("kiln_cfg_store_apply -- REAL kiln_cfg_store_set_autosave_target_override()/"
                 "kiln_cfg_store_autosave_from_live() branch protects the INCOMING slot, not the "
                 "outgoing active one (HIGH finding 1 / MEDIUM finding 4, "
                 "docs/audits/review_autosave_slot_fix_a93ee77b_2026-09-15.md)");
    reset_state();

    // Slot A ("Outgoing"): saved and left active.
    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 1);
    }
    int32_t id_a = -1;
    char reason[96] = {0};
    TEST_CHECK(kiln_cfg_store_save_current("Outgoing", -1, &id_a, reason, sizeof(reason)),
               "test setup: save slot A");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id_a, "test setup: A is active");

    // Slot B ("Incoming"): a clone of A (clone never touches active_id, per
    // kiln_cfg_store_clone()'s own header comment) with its content then
    // overwritten via an overwrite-by-id save_current(id_b, ...), which also
    // does not touch active_id -- so A stays active going into the apply
    // below, exactly like a real "apply a different saved kiln" would.
    int32_t id_b = -1;
    TEST_CHECK(kiln_cfg_store_clone(id_a, "Incoming", &id_b, reason, sizeof(reason)),
               "test setup: clone A into a second, currently-inactive slot B");
    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 100);
    }
    TEST_CHECK(kiln_cfg_store_save_current("Incoming", id_b, NULL, reason, sizeof(reason)),
               "test setup: overwrite B's content in place (does not move active_id)");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id_a, "test setup: A is still active, B is not");

    uint8_t blob_a_before[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint16_t blob_a_before_len = 0;
    TEST_CHECK(kiln_cfg_store_get_full_package(id_a, blob_a_before, sizeof(blob_a_before), &blob_a_before_len,
                                               NULL, NULL, 0),
               "test setup: read A's original bytes");

    // Now apply B. Production's real sequence inside kiln_cfg_store_apply():
    // set override(B) -> zones_config_import_blob(B's bytes) [autosave fires
    // HERE, live config already reads as B, active_id is STILL A] -> clear
    // override -> active_id = B. The stub fires the REAL autosave_from_live()
    // at exactly that point, with the live "export" content already reading
    // as B's bytes below.
    for (size_t i = 0; i < sizeof(s_stub_export_content); i++) {
        s_stub_export_content[i] = (uint8_t)(i + 100); // B's bytes, matching what B was saved with
    }
    s_stub_autosave_during_import = true;
    bool applied = kiln_cfg_store_apply(id_b, /*ack_no_safety_processor=*/true, false, reason, sizeof(reason));
    s_stub_autosave_during_import = false;
    TEST_CHECK(applied, "apply succeeds");
    TEST_CHECK(kiln_cfg_store_get_active_id() == id_b, "active_id moved to B after apply returned");

    uint8_t blob_a_after[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint16_t blob_a_after_len = 0;
    TEST_CHECK(kiln_cfg_store_get_full_package(id_a, blob_a_after, sizeof(blob_a_after), &blob_a_after_len,
                                               NULL, NULL, 0),
               "read A's bytes after the apply+in-flight autosave");
    TEST_CHECK(blob_a_before_len == blob_a_after_len && memcmp(blob_a_before, blob_a_after, blob_a_before_len) == 0,
               "slot A (outgoing) was NOT touched by the autosave that fired mid-apply");

    uint8_t blob_b_after[ZONES_CONFIG_BLOB_MAX_SIZE];
    uint16_t blob_b_after_len = 0;
    TEST_CHECK(kiln_cfg_store_get_full_package(id_b, blob_b_after, sizeof(blob_b_after), &blob_b_after_len,
                                               NULL, NULL, 0),
               "read B's bytes after the apply+in-flight autosave");
    TEST_CHECK(blob_b_after_len > 0 && blob_b_after[0] == 100,
               "slot B (incoming) is the one the in-flight autosave protected/refreshed");
}

void run_test_kiln_cfg_store(void)
{
    test_save_clone_apply_roundtrip();
    test_apply_refused_while_run_active();
    test_delete_refuses_the_active_config();
    test_delete_refused_by_interlock_while_firing_even_when_not_active();
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
    test_name_character_set_validation();
    test_store_full_rejected();
    test_save_current_captures_pico_half_and_hash();
    test_save_current_unfetched_cache_is_not_marked_populated();
    test_get_package_identity_unknown_id_and_migrated_slot();
    test_apply_refuses_half_package();
    test_corrupt_store_quarantines_and_blocks_writes();
    test_nvs_load_store_migrates_v1_blob_at_full_size();
    test_nvs_load_store_migrates_v2_blob_at_full_size();
    test_v2_blob_never_blindly_reinterpreted_as_v3();
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
    test_cfg_fs_all_mutators_report_persist_failure();
    test_cfg_fs_equal_rev_divergence_adopts_nvs_not_the_stale_file();
    test_cfg_fs_stale_delete_not_resurrected();
    test_cfg_fs_interrupted_write_leaves_old_or_new();

    test_export_import_new_slot_round_trip();
    test_export_package_json_never_contains_or_disturbs_credential();
    test_import_refuses_malformed();
    test_import_refuses_newer_pkg_schema_nothing_written();
    test_import_refuses_hash_mismatch_nothing_written();
    test_import_reports_failure_when_persist_fails();
    test_import_refuses_abs_max_temp_c_tighter_than_zone_max();
    test_import_refuses_abs_max_temp_c_above_firmware_ceiling();
    test_import_refuses_missing_abs_max_temp_c_when_zone_configured();
    test_import_accepts_abs_max_temp_c_at_or_above_zone_max();
    test_restore_path_accepts_unset_ceiling_but_still_checks_a_set_one();
    test_import_cross_board_forces_calibration_reset();
    test_import_matching_board_preserves_calibration();
    test_import_absent_source_board_id_forces_calibration_reset();
    test_apply_refuses_hardware_mismatch_without_ack();
    test_hardware_differs_message_fits_longest_label();
    test_apply_allows_hardware_mismatch_with_ack();
    test_apply_refuses_live_ceiling_tighter_than_zone_max();
    test_apply_skips_live_ceiling_check_when_live_unset();
    test_autosave_from_live_noop_with_no_active_config();
    test_autosave_from_live_updates_active_slot_and_hash();
    test_autosave_override_ignores_a_foreign_dispatcher();
    test_autosave_override_applies_to_its_own_dispatcher();
    test_autosave_override_ignores_a_foreign_nonnull_dispatcher();
    test_autosave_keeps_existing_pico_half_while_cache_unfetched();
    test_autosave_from_live_suppressed_while_diverged();
    test_autosave_from_live_suppressed_while_swap_pending();
    test_recapture_pico_half_confirmed_bypasses_divergence_gate();
    test_recapture_pico_half_confirmed_suppressed_while_swap_pending();
    test_apply_autosave_targets_incoming_slot_via_real_override();
    test_autosave_defers_when_cache_generation_moved_since_latch_evaluated();
    test_capture_expected_pico_fields_excludes_abs_max_and_tc_type();
    test_set_active_id_raw_drops_pending_flag_owned_by_old_slot();
    test_home_rail_kiln_count_matches_list_and_decides_threshold();

    test_validate_package_json_writes_nothing_on_success();
    test_validate_package_json_refuses_bad_hash_same_reason_as_import();
    test_validate_package_json_succeeds_while_quarantined();
    test_import_package_json_as_honours_name_override();
    test_import_package_json_as_rejects_invalid_override();
    test_import_package_json_as_null_override_matches_plain_import();
    test_validate_package_json_hash_check_is_load_bearing();

    cfg_fs_deinit();
}
