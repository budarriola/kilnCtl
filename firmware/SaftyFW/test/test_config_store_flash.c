// Host tests for firmware/SaftyFW/src/config_store_flash.c -- the flash I/O
// and ARMED-check glue, host-testable for the first time since its Phase 3
// item 2 rebase onto hal_flash.h (docs/HW_ABSTRACTION.md). Exercises
// the three things that rebase promised to keep intact: the ARMED write
// gate (config_store_decide_write() against relay_owner_get_state(), here
// driven by config_store_flash_host_stubs.c), the seq/CRC round-robin log
// across a real fake_flash-backed sector, and hal_flash_safe_execute()
// failure injection surfacing through config_store_flash_rc_reason().
//
// config_store.c's own pure pack/unpack/CRC/decision logic is already
// covered by test_config_store.c -- this file exercises config_store_
// flash.c's real-flash glue ON TOP of that, via config_store_boot_load()/
// config_store_write()/the getters, backed by fake_flash.c instead of a
// real RP2040.
//
// A SEPARATE executable from test_main.c's, same reason test_hal_spi_pico.c
// is (see build_host_tests.ps1's comment on that one): config_store_flash_
// host_stubs.c defines relay_owner_get_state()/console_uart_puts()/
// console_uart_write() as bare test doubles, and the main exe already links
// the REAL relay_owner.c (for its own hal_gpio tests) -- linking both into
// one executable is an LNK2005 multiply-defined-symbol error, not a valid
// combination. Own main(), own g_test_failures/g_test_count, own exit code,
// folded into build_host_tests.ps1's overall exit code same as the
// hal_spi_pico executable is.
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "test_common.h"

#include "config_store.h"
#include "config_store_flash_host_stubs.h"
#include "discrete_pin_policy.h"
#include "fake_flash.h"
// The production decision -> wire-reason mapping, exercised here against a
// REAL fake_flash-backed store so the cached/persisted disagreement is a
// genuine one rather than two hand-picked byte values (re-review defect 1).
#include "tasks/link_task_commit_reject.h"
#include "tasks/relay_owner.h"

int g_test_failures = 0;
int g_test_count = 0;

// Every test case starts from a freshly-erased sector at the SAME (base,
// size) config_store_flash.c's own hal_flash_region_t binds to --
// SAFTYFW_CONFIG_STORE_FLASH_OFFSET/_SIZE (flash_layout.h) -- big enough
// that hal_flash_region_init() accepts that real offset against the fake's
// configured total size. See fake_flash.h's FAKE_FLASH_MAX_SIZE_BYTES
// comment for why the fake was sized to allow this.
#include "flash_layout.h"
// Covers sector A AND sector B (flash_endurance_review_2026-09-07.md R2):
// OFFSET_B == OFFSET + SIZE (flash_layout.h), so the fake image must reach
// OFFSET_B + SIZE for hal_flash_region_init() to accept sector B's real
// offset too.
#define TEST_FLASH_TOTAL_SIZE \
    (SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B + SAFTYFW_CONFIG_STORE_FLASH_SIZE)

static void reset_all(void)
{
    TEST_CHECK(fake_flash_reset_all_sized(TEST_FLASH_TOTAL_SIZE) == true,
               "fake_flash sized to cover the real config-store offset");
    config_store_flash_host_stub_reset();
    // The fallback double buffer's own statics (s_fallback_valid/_active/
    // _gen/_buf) are NOT reset by the above -- on real hardware a power
    // cycle reinitialises them, but in this one long-lived test process
    // they would otherwise carry state across every test case (see
    // config_store_test_reset_fallback_state()'s own comment).
    config_store_test_reset_fallback_state();
    config_store_set_heat_possible_probe(NULL);
}

static void test_boot_load_blank_sector_is_default(void)
{
    TEST_SECTION("config_store_flash: blank sector -> default record, not rejected");
    reset_all();

    config_store_boot_load();

    TEST_CHECK(config_store_is_config_rejected() == false, "blank sector is not a rejection");
    TEST_CHECK(config_store_get_config_crc() == 0, "uncommissioned CRC reads 0");
    TEST_CHECK(config_store_get_config_version() == 0, "uncommissioned version reads 0");
    TEST_CHECK(config_store_is_calibration_missing() == true, "default calibration_missing");

    config_store_record_t rec;
    config_store_get_full_record(&rec);
    config_store_record_t def;
    config_store_default(&def);
    TEST_CHECK(memcmp(&rec, &def, sizeof(rec)) == 0,
               "full record matches config_store_default() on a blank sector");
}

static void test_write_then_reload_round_trips(void)
{
    TEST_SECTION("config_store_flash: write, then a fresh boot_load sees it");
    reset_all();
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u; // MAX31856_TC_TYPE_T
    rec.calibration_missing = false;

    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == true, "write succeeds while not ARMED");
    TEST_CHECK(reason != NULL && strcmp(reason, "ok") == 0, "reason is 'ok' on success");

    // Simulate a reboot: nothing in config_store_flash.c resets on its own,
    // so re-running boot_load re-scans the (now real) flash image fake_flash
    // is holding, exactly like a power cycle would re-scan real NOR flash.
    config_store_boot_load();

    TEST_CHECK(config_store_get_tc_type() == 0x07u, "tc_type survives a reload");
    TEST_CHECK(config_store_is_calibration_missing() == false,
               "calibration_missing survives a reload");
    TEST_CHECK(config_store_get_config_crc() != 0, "a committed record reports a nonzero CRC");
    TEST_CHECK(config_store_is_config_rejected() == false, "a good record is not a rejection");
}

// opus review 2026-09-09, finding B: before this fix, s_fallback_valid
// stayed false from boot until the first-ever config_store_write() in that
// boot -- so a core-1 reader hitting the exhausted-retries fallback path
// during that window got accessor defaults (calibration_missing=true, etc)
// instead of the perfectly good record config_store_boot_load() just loaded
// and validated from flash. This test commits a record, reboots (a fresh
// config_store_boot_load(), same simulated-power-cycle idiom as the reload
// test above), and then -- with NO write at all in this "boot" -- forces
// the fallback path via config_store_test_force_fallback_path() (no hook
// needed: proving the seed alone is enough, with no writer activity in this
// boot to ever flip s_fallback_valid the old way) and checks the record
// that comes back is the persisted one, not a default.
static void test_fallback_seeded_at_boot_before_any_write(void)
{
    TEST_SECTION("config_store_flash: fallback buffer is seeded at boot -- no availability gap "
                 "before the first commissioning write of a boot (opus review finding B)");
    reset_all();
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u; // MAX31856_TC_TYPE_T
    rec.calibration_missing = false;
    const char *reason = NULL;
    TEST_CHECK(config_store_write(&rec, &reason) == true, "fixture: commissioning write succeeds");

    // Simulate a reboot -- same idiom as test_write_then_reload_round_trips(),
    // but ALSO reset the fallback double buffer's own statics back to a true
    // cold-boot state (reset_all() does this too, but that ran before the
    // fixture write above -- this call is what actually simulates the power
    // cycle between the write and the reload). Deliberately NO config_store_
    // write() call after this point: the whole point is to prove the
    // fallback is trustworthy in a boot that has not committed anything yet.
    config_store_test_reset_fallback_state();
    config_store_boot_load();

    config_store_test_force_fallback_path(true);
    config_store_record_t snap;
    memset(&snap, 0, sizeof(snap));
    config_store_get_full_record(&snap);
    config_store_test_force_fallback_path(false);

    TEST_CHECK(snap.tc_type == 0x07u,
               "the fallback path returns the persisted tc_type, not CONFIG_STORE_DEFAULT_TC_TYPE "
               "-- no write happened yet this boot, so the old code would have returned false "
               "here (s_fallback_valid still false) and every getter's own accessor default");
    TEST_CHECK(snap.calibration_missing == false,
               "the fallback path returns the persisted calibration_missing, not the fail-safe "
               "'true' every getter falls back to when config_store_seqlock_read() returns false");
}

static void test_write_refused_while_armed(void)
{
    TEST_SECTION("config_store_flash: ARMED gate refuses the write and touches no flash");
    reset_all();
    config_store_boot_load();
    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u;

    // 2026-09-15 (Opus review item 6): this record differs from the cached
    // one ONLY in tc_type, but config_store_write() (the plain, non-"_ex"
    // wrapper) never determines heat state at all -- it must NOT claim
    // CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_ON, since that specifically
    // asserts "heat was checked and found on," a fact this path never
    // established. It gets its own honest CONFIG_STORE_WRITE_REFUSED_ARMED_
    // HEAT_UNKNOWN reason instead -- still distinct from the generic
    // CONFIG_STORE_WRITE_REFUSED_ARMED so a caller can tell "tc_type-only
    // change, heat unknown" apart from "some other field changed while
    // ARMED".
    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == false, "write refused while ARMED");
    TEST_CHECK(reason != NULL &&
                   reason == config_store_write_decision_reason(CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_UNKNOWN),
               "refusal reason is the tc_type/heat-unknown-specific one (item 6), not the "
               "heat-on-specific, generic ARMED, or flash-layer reason");

    // No sector was touched: config_store_write() must refuse BEFORE
    // scheduling any hal_flash_erase()/hal_flash_program() -- confirmed here
    // by checking the sector still reads back as a blank/default record.
    config_store_boot_load();
    TEST_CHECK(config_store_get_config_crc() == 0,
               "sector unchanged by a refused write -- still uncommissioned");
}

// KILN_PROFILES_PLAN.md item 15 -- config_store_write_volatile() must land
// in the live record (visible to every getter/guard) while ARMED, must bump
// config_version/config_crc exactly as a flash commit does, and must never
// touch flash (a reload must NOT see it -- it was never persisted).
static void test_write_volatile_installs_while_armed_and_bumps_identity(void)
{
    TEST_SECTION("config_store_flash: write_volatile lands in RAM, bumps identity, stays ARMED");
    reset_all();
    config_store_boot_load();
    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);

    TEST_CHECK(config_store_get_config_crc() == 0, "fixture: uncommissioned before the volatile install");
    uint8_t version_before = config_store_get_config_version();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u; // MAX31856_TC_TYPE_T
    rec.calibration_missing = false;

    const char *reason = NULL;
    TEST_CHECK(config_store_write_volatile(&rec, &reason) == true,
               "a tc_type-only install off an unconfigured (fields_set==0) baseline is a "
               "tightening/neutral change, never refused, even while ARMED");

    // Visible immediately.
    TEST_CHECK(config_store_get_tc_type() == 0x07u,
               "volatile install is visible to getters immediately");
    TEST_CHECK(config_store_is_calibration_missing() == false,
               "volatile install's calibration_missing is visible immediately");
    TEST_CHECK(config_store_get_config_crc() != 0,
               "volatile install bumps config_crc off the 'never commissioned' 0 sentinel");
    TEST_CHECK(config_store_get_config_version() != version_before,
               "volatile install bumps config_version");

    // Still ARMED: nothing about this call path ever consulted, or needed
    // to consult, relay_owner_get_state().
    TEST_CHECK(relay_owner_get_state() == RELAY_OWNER_STATE_ARMED,
               "the Pico never left ARMED across a volatile install");

    // Never reached flash: a fresh boot_load (simulated reboot) must see the
    // sector exactly as boot-loaded before the volatile install -- still
    // uncommissioned, not the volatile record.
    config_store_boot_load();
    TEST_CHECK(config_store_get_config_crc() == 0,
               "a volatile install never lands in flash -- a reload sees the pre-install state");
    TEST_CHECK(config_store_get_tc_type() != 0x07u,
               "a reload does not see the volatile tc_type either");
}

// A second volatile install must keep bumping the identity pair further
// (not just once off the zero sentinel), and stacking a real flash commit
// on top of a volatile install must still work normally (config_store_
// write()'s own ARMED gate is independent and unaffected by the volatile
// path having run first).
static void test_write_volatile_repeated_then_flash_commit_still_gated(void)
{
    TEST_SECTION("config_store_flash: repeated write_volatile, then an ARMED flash commit still refuses");
    reset_all();
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x01u; // fields_set stays 0 -- tc_type left uncommissioned throughout this test
    TEST_CHECK(config_store_write_volatile(&rec, NULL) == true, "fixture: first volatile install accepted");
    uint8_t version_1 = config_store_get_config_version();
    uint16_t crc_1 = config_store_get_config_crc();

    rec.tc_type = 0x02u;
    TEST_CHECK(config_store_write_volatile(&rec, NULL) == true,
               "a repeated install of an uncommissioned (fields_set==0) tc_type is never refused, "
               "not even while later ARMED (checked below)");
    uint8_t version_2 = config_store_get_config_version();
    uint16_t crc_2 = config_store_get_config_crc();

    TEST_CHECK(version_2 != version_1, "a second volatile install bumps the version again");
    TEST_CHECK(crc_2 != crc_1, "a second volatile install bumps the CRC again (different bytes)");
    TEST_CHECK(config_store_get_tc_type() == 0x02u, "the second volatile install's value wins");

    // config_store_write()'s ARMED gate is untouched by any of this -- it is
    // a completely separate function with its own check.
    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);
    config_store_record_t flash_rec;
    config_store_default(&flash_rec);
    flash_rec.tc_type = 0x03u;
    const char *reason = NULL;
    bool ok = config_store_write(&flash_rec, &reason);
    TEST_CHECK(ok == false,
               "config_store_write() still refuses while ARMED even after volatile installs ran");
    TEST_CHECK(config_store_get_tc_type() == 0x02u,
               "the refused flash write does not disturb the volatile record already live");
}

// 2026-09-15 (Opus review item 4): config_store_write_ex()/config_store_write()
// must classify "does this candidate differ from flash-truth in tc_type
// only" against what is actually PERSISTED on flash, not against
// s_cached_record -- the RAM record a prior volatile install already left
// mutated in some OTHER field too. Comparing against the RAM record instead
// would misclassify this scenario as "more than tc_type changed" and give
// the generic CONFIG_STORE_WRITE_REFUSED_ARMED, silently losing the
// tc_type-only carve-out's more specific refusal reason precisely when a
// volatile install happens to be live.
static void test_write_uses_persisted_record_not_ram_after_volatile_install(void)
{
    TEST_SECTION("config_store_flash: tc_type-only comparison uses the persisted (flash) record, "
                 "not a RAM record already mutated by a volatile install (opus review item 4)");
    reset_all();
    config_store_boot_load(); // persisted record: all-default, uncommissioned

    // A neutral volatile install while NOT armed: allowed, and it leaves
    // s_cached_record differing from the persisted/flash record in
    // mains_voltage_v -- but flash itself is untouched.
    config_store_record_t volatile_rec;
    config_store_default(&volatile_rec);
    volatile_rec.mains_voltage_v = 240.0f;
    TEST_CHECK(config_store_write_volatile(&volatile_rec, NULL) == true,
               "fixture: neutral volatile install accepted while not armed");

    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);

    // This candidate differs from the PERSISTED/flash record in tc_type
    // ONLY -- but differs from the current RAM record (s_cached_record) in
    // BOTH tc_type and mains_voltage_v, since the volatile install above
    // only touched RAM.
    config_store_record_t candidate;
    config_store_default(&candidate);
    candidate.tc_type = 0x07u;
    const char *reason = NULL;
    bool ok = config_store_write(&candidate, &reason);
    TEST_CHECK(ok == false, "write refused while ARMED");
    TEST_CHECK(reason != NULL &&
                   reason == config_store_write_decision_reason(CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_UNKNOWN),
               "classified as tc_type-only against the PERSISTED record (item 4) -- comparing "
               "against the volatile-mutated RAM record instead would report the generic "
               "CONFIG_STORE_WRITE_REFUSED_ARMED here");
}

// config_store_is_volatile_dirty()/config_store_get_persisted_config_version()
// -- a volatile install must flip dirty true and bump config_version without
// moving the PERSISTED version at all; a subsequent durable commit must clear
// dirty and bring the persisted version up to match.
static void test_volatile_dirty_flag_and_persisted_version(void)
{
    TEST_SECTION("config_store_flash: is_volatile_dirty/get_persisted_config_version");
    reset_all();
    config_store_boot_load();

    TEST_CHECK(config_store_is_volatile_dirty() == false,
               "a freshly booted, never-written cache is not dirty");
    uint8_t persisted_before = config_store_get_persisted_config_version();
    TEST_CHECK(persisted_before == config_store_get_config_version(),
               "cache and flash-truth versions agree before any write");

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u; // MAX31856_TC_TYPE_T
    rec.calibration_missing = false;
    TEST_CHECK(config_store_write_volatile(&rec, NULL) == true,
               "fixture: volatile install accepted");

    TEST_CHECK(config_store_is_volatile_dirty() == true,
               "a volatile install leaves the cache ahead of flash-truth");
    TEST_CHECK(config_store_get_persisted_config_version() == persisted_before,
               "a volatile install must not move the persisted version");
    TEST_CHECK(config_store_get_config_version() != persisted_before,
               "fixture: the live (cache) version did move");

    // A durable commit clears the divergence: both sides land on the same
    // seq, so dirty must go false again and the persisted version must catch
    // up to what the cache already reported.
    const char *reason = NULL;
    TEST_CHECK(config_store_write(&rec, &reason) == true,
               "fixture: durable commit accepted");
    TEST_CHECK(config_store_is_volatile_dirty() == false,
               "a durable write clears the volatile-dirty flag");
    TEST_CHECK(config_store_get_persisted_config_version() == config_store_get_config_version(),
               "a durable write brings the persisted version back in sync with the cache");
}

// 2026-09-15, adversarial re-review of d43e96b2, defect 1 -- THE case that
// distinguishes a correct F2 fix from the one that shipped.
//
// F2 labels a refused ARMED commit as ARMED_MIXED (rather than plain ARMED)
// when the refused write ALSO changes the thermocouple type. The question is
// which record "also changes" is measured against. config_store_write_ex()
// measures it against s_persisted_record -- flash truth -- and builds its log
// sentence from that. The shipped F2 measured it against the CACHED record
// (config_store_get_tc_type()), which is precisely the read the "item 15" fix
// removed from this comparison one layer down, because a prior
// config_store_write_volatile() install leaves the cache carrying values that
// are not on flash.
//
// So this test deliberately constructs the disagreement: a volatile install
// moves the CACHED tc_type away from the PERSISTED one, and the refused
// candidate is then chosen to match the cached value and differ from the
// persisted one. The two inputs give opposite answers, and only the persisted
// one agrees with the sentence config_store_write_ex() itself logged. A test
// that does not construct this disagreement passes against the defect.
static void test_mixed_armed_refusal_wire_reason_uses_persisted_not_cached_tc_type(void)
{
    TEST_SECTION("config_store_flash: a MIXED ARMED refusal's wire reason is derived from the "
                 "PERSISTED tc_type and agrees with its own log sentence (re-review defect 1)");
    reset_all();
    config_store_boot_load(); // persisted record: all-default, uncommissioned

    const uint8_t persisted_type_before = config_store_get_persisted_tc_type();
    // Any valid type code that is not the persisted one -- derived, not
    // hardcoded, so a change to CONFIG_STORE_DEFAULT_TC_TYPE cannot silently
    // collapse this test's two values into one and make it vacuous.
    const uint8_t other_type = (persisted_type_before == 0x06u) ? 0x02u : 0x06u;

    // Step 1: pollute the RAM cache while NOT armed. A volatile install
    // lands in RAM only -- flash, and therefore s_persisted_record, is
    // untouched.
    config_store_record_t volatile_rec;
    config_store_default(&volatile_rec);
    volatile_rec.fields_set |= (uint16_t)CONFIG_STORE_SET_TC_TYPE;
    volatile_rec.tc_type = other_type;
    TEST_CHECK(config_store_write_volatile(&volatile_rec, NULL) == true,
               "fixture: volatile install accepted while not armed");

    // Step 2: the cached and persisted types now genuinely DISAGREE. Without
    // this, every input below would give the same answer and the test would
    // prove nothing.
    TEST_CHECK(config_store_get_tc_type() == other_type,
               "fixture: the CACHED tc_type is the volatile install's value");
    TEST_CHECK(config_store_get_persisted_tc_type() == persisted_type_before,
               "fixture: the PERSISTED tc_type is still flash truth, unmoved by the volatile "
               "install");
    TEST_CHECK(config_store_get_tc_type() != config_store_get_persisted_tc_type(),
               "fixture: cached and persisted tc_type DISAGREE -- this is the case that "
               "separates a correct F2 from the shipped one");

    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);

    // Step 3: a candidate that is MIXED against FLASH (tc_type differs AND
    // mains_voltage_v differs) but whose tc_type MATCHES the polluted cache.
    config_store_record_t candidate;
    config_store_default(&candidate);
    candidate.fields_set |= (uint16_t)CONFIG_STORE_SET_TC_TYPE;
    candidate.tc_type = other_type;
    candidate.mains_voltage_v = 240.0f;

    const char *reason = NULL;
    config_store_write_decision_t decision = CONFIG_STORE_WRITE_OK;
    // heat_safe = true: a tc_type-ONLY change would be ACCEPTED here, so the
    // refusal below is caused purely by the other changed field, which is
    // exactly what "MIXED" is supposed to tell the operator.
    bool written = config_store_write_ex(&candidate, true, &reason, &decision);

    TEST_CHECK(written == false, "the mixed change is refused while ARMED");
    TEST_CHECK(decision == CONFIG_STORE_WRITE_REFUSED_ARMED,
               "the decision is the plain ARMED refusal (MIXED is a label on it, not a separate "
               "decision)");
    TEST_CHECK(reason != NULL && strstr(reason, "other than thermocouple type") != NULL,
               "config_store_write_ex() logged the MIXED sentence -- it classified this as a "
               "mixed change against the PERSISTED record");

    // Step 4: the wire reason the production mapping produces from the
    // PERSISTED type agrees with that sentence.
    kilnlink_commit_config_reject_reason_t wire_from_persisted =
        link_task_commit_config_reject_reason_for(decision, config_store_get_persisted_tc_type(),
                                                   candidate.tc_type);
    TEST_CHECK(wire_from_persisted == KILNLINK_COMMIT_CONFIG_REJECT_ARMED_MIXED,
               "the wire reason built from the PERSISTED tc_type is ARMED_MIXED -- it AGREES "
               "with the log sentence above");

    // Step 5: and the defect's input gives the contradicting answer. This
    // assertion is what makes the two records' disagreement load-bearing
    // rather than decorative.
    kilnlink_commit_config_reject_reason_t wire_from_cached =
        link_task_commit_config_reject_reason_for(decision, config_store_get_tc_type(),
                                                   candidate.tc_type);
    TEST_CHECK(wire_from_cached == KILNLINK_COMMIT_CONFIG_REJECT_ARMED,
               "the wire reason built from the CACHED tc_type is the plain ARMED reason -- the "
               "shipped F2's input, and it CONTRADICTS the log sentence for this one refusal");
    TEST_CHECK(wire_from_persisted != wire_from_cached,
               "the two inputs genuinely diverge on this case (guards against a future change "
               "that makes them identical and quietly turns this test vacuous)");
}

// 2026-09-18 -- CT auto-zero commissioning deadlock fix, end-to-end through
// config_store_write_ex(). Bench evidence (Pico build 49683b45, channel 2):
// ct_auto_zero_check_preconditions() requires K4 closed, which only happens
// while the Pico is ARMED, but every prior write path refused ANY config
// write while ARMED except a tc_type-only change -- so a CT-zero commit
// (which changes zero_counts[ch]/k_ct_v_per_a[ch], never tc_type) could
// never land. This widens config_store_write_ex()'s narrow ARMED exemption
// to also cover a single channel's zero_counts+k_ct_v_per_a pair, gated by
// the SAME heat_safe check already used for the tc_type exemption -- no new
// gate, no protocol change. These three cases are the ones that matter on
// the wire: the exact deadlocked scenario now succeeds; the exemption still
// fails closed when heat_safe is false; and it does not widen to cover an
// unrelated bundled field change.
static void test_write_ex_ct_cal_only_armed_exemption(void)
{
    TEST_SECTION("config_store_flash: a single channel's zero_counts/k_ct_v_per_a-only change is "
                 "exempted from the ARMED write refusal when heat_safe, exactly like tc_type "
                 "(2026-09-18 CT auto-zero deadlock fix)");
    reset_all();
    config_store_boot_load(); // persisted record: all-default, uncommissioned

    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);

    // Case 1: the deadlocked scenario itself -- ARMED, heat_safe true (K4
    // closed, no active heat owner), candidate differs from persisted in
    // exactly one channel's zero_counts+k_ct_v_per_a. Must now succeed.
    config_store_record_t candidate_ok;
    config_store_default(&candidate_ok); // persisted record is all-default after boot_load above
    candidate_ok.zero_counts[1] = (uint16_t)(candidate_ok.zero_counts[1] + 5u);
    candidate_ok.k_ct_v_per_a[1] = candidate_ok.k_ct_v_per_a[1] + 0.01f;

    const char *reason_ok = NULL;
    config_store_write_decision_t decision_ok = CONFIG_STORE_WRITE_REFUSED_ARMED;
    bool written_ok = config_store_write_ex(&candidate_ok, /*heat_safe=*/true, &reason_ok,
                                             &decision_ok);
    TEST_CHECK(written_ok == true,
               "a ct-cal-only (zero_counts+k_ct_v_per_a, one channel) change while ARMED with "
               "heat_safe succeeds -- this is the bench deadlock case");
    TEST_CHECK(decision_ok == CONFIG_STORE_WRITE_OK, "decision is OK, not a refusal");

    // Case 2: same shape of change, but heat_safe is false (K4 not actually
    // closed, or no recent REQUEST_ENABLE, or an active heat owner) -- must
    // still fail closed, exactly like the tc_type exemption does. Reset
    // first: case 1's successful write above already moved the PERSISTED
    // record away from all-default (channel 1 now differs), so building this
    // candidate from a fresh default would make it differ from the new
    // persisted record in TWO channels (1 and 2), not one, and wrongly
    // exercise the "more than one channel changed" refusal instead of the
    // heat_safe gate this case means to test.
    reset_all();
    config_store_boot_load();
    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);
    config_store_record_t candidate_unsafe;
    config_store_default(&candidate_unsafe);
    candidate_unsafe.zero_counts[2] = (uint16_t)(candidate_unsafe.zero_counts[2] + 5u);
    candidate_unsafe.k_ct_v_per_a[2] = candidate_unsafe.k_ct_v_per_a[2] + 0.01f;

    const char *reason_unsafe = NULL;
    config_store_write_decision_t decision_unsafe = CONFIG_STORE_WRITE_OK;
    bool written_unsafe = config_store_write_ex(&candidate_unsafe, /*heat_safe=*/false,
                                                 &reason_unsafe, &decision_unsafe);
    TEST_CHECK(written_unsafe == false,
               "the same class of change is REFUSED while ARMED when heat_safe is false -- the "
               "exemption never bypasses the heat-safety gate");
    TEST_CHECK(decision_unsafe == CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_ON,
               "refused specifically as the heat-unsafe narrow-change refusal, not silently "
               "swallowed some other way");

    // Case 3: bundling the ct-cal change with an unrelated field change must
    // NOT ride the exemption, even with heat_safe true -- the exemption is
    // narrow by construction (config_store_only_ct_cal_differs() requires
    // everything else to match byte-for-byte). Reset again for the same
    // reason as case 2.
    reset_all();
    config_store_boot_load();
    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);
    config_store_record_t candidate_mixed;
    config_store_default(&candidate_mixed);
    candidate_mixed.zero_counts[0] = (uint16_t)(candidate_mixed.zero_counts[0] + 5u);
    candidate_mixed.k_ct_v_per_a[0] = candidate_mixed.k_ct_v_per_a[0] + 0.01f;
    candidate_mixed.mains_voltage_v = 240.0f;

    const char *reason_mixed = NULL;
    config_store_write_decision_t decision_mixed = CONFIG_STORE_WRITE_OK;
    bool written_mixed = config_store_write_ex(&candidate_mixed, /*heat_safe=*/true,
                                                &reason_mixed, &decision_mixed);
    TEST_CHECK(written_mixed == false,
               "a ct-cal change bundled with an unrelated field change is refused while ARMED "
               "even with heat_safe true -- the exemption does not widen to cover it");
    TEST_CHECK(decision_mixed == CONFIG_STORE_WRITE_REFUSED_ARMED,
               "the mixed change gets the plain ARMED refusal, not the narrow-change one");
}

// 2026-09-14 review, Finding A -- config_store_write_volatile() must refuse
// a narrow class of installs while ARMED: raising or clearing an already-
// commissioned abs_max_temp_c/max_rate_c_per_min, or changing an already-
// commissioned tc_type. Everything else -- first commissioning of any of
// those three (unset -> set), lowering an already-commissioned threshold,
// or touching any other field -- must still install while ARMED, exactly as
// before this fix, since the whole point of the volatile path is that an
// ordinary kiln-package swap never has to unarm the Pico.
static void test_write_volatile_refuses_loosening_while_armed(void)
{
    TEST_SECTION("config_store_flash: write_volatile refuses a LOOSENING install while ARMED, "
                 "allows tightening/neutral ones");
    reset_all();
    config_store_boot_load();

    // Commission a real baseline (NOT ARMED yet) -- abs_max_temp_c 1100C,
    // max_rate_c_per_min 20, tc_type K -- so there is something to loosen.
    config_store_record_t baseline;
    config_store_default(&baseline);
    baseline.fields_set |= (uint16_t)(CONFIG_STORE_SET_ABS_MAX_TEMP_C | CONFIG_STORE_SET_MAX_RATE_C_PER_MIN |
                                       CONFIG_STORE_SET_TC_TYPE);
    baseline.abs_max_temp_c = 1100.0f;
    baseline.max_rate_c_per_min = 20.0f;
    baseline.tc_type = 0x03u; // MAX31856_TC_TYPE_K

    // Each sub-case re-establishes the SAME baseline while de-energized
    // (allowed unconditionally -- config_store_write_volatile() never checks
    // ARMED for a re-install of an unchanged/tightening record), THEN arms,
    // THEN attempts one mutation -- so every case is judged against the same
    // known "currently live" record, not against whatever a PRIOR case's
    // successful install left behind.
#define REARM_FROM_BASELINE()                                                                        \
    do {                                                                                              \
        config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_INIT); /* not ARMED */          \
        TEST_CHECK(config_store_write_volatile(&baseline, NULL) == true, "fixture: baseline restored"); \
        config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);                        \
    } while (0)

    REARM_FROM_BASELINE();
    // 1. Raising abs_max_temp_c while ARMED: refused.
    {
        config_store_record_t rec = baseline;
        rec.abs_max_temp_c = 1372.0f; // type-K max -- exactly the review's own example
        const char *reason = NULL;
        TEST_CHECK(config_store_write_volatile(&rec, &reason) == false,
                   "raising a commissioned abs_max_temp_c ceiling while ARMED is refused");
        TEST_CHECK(reason != NULL &&
                       reason == config_store_write_decision_reason(CONFIG_STORE_WRITE_REFUSED_ARMED),
                   "refusal reason is the ARMED one");
    }

    REARM_FROM_BASELINE();
    // 2. Clearing abs_max_temp_c (back to "never trips") while ARMED: refused.
    {
        config_store_record_t rec = baseline;
        rec.fields_set = (uint16_t)(rec.fields_set & ~(uint32_t)CONFIG_STORE_SET_ABS_MAX_TEMP_C);
        TEST_CHECK(config_store_write_volatile(&rec, NULL) == false,
                   "un-committing abs_max_temp_c while ARMED is refused (loosens back to 'never trips')");
    }

    REARM_FROM_BASELINE();
    // 3. Lowering abs_max_temp_c while ARMED: allowed (tightening).
    {
        config_store_record_t rec = baseline;
        rec.abs_max_temp_c = 900.0f;
        TEST_CHECK(config_store_write_volatile(&rec, NULL) == true,
                   "lowering a commissioned abs_max_temp_c ceiling while ARMED is allowed");
        config_store_record_t live;
        config_store_get_full_record(&live);
        TEST_CHECK(live.abs_max_temp_c == 900.0f, "the lowering install actually landed");
    }

    REARM_FROM_BASELINE();
    // 4. Raising max_rate_c_per_min (S8) while ARMED: refused.
    {
        config_store_record_t rec = baseline;
        rec.max_rate_c_per_min = 50.0f;
        TEST_CHECK(config_store_write_volatile(&rec, NULL) == false,
                   "raising a commissioned S8 rate cap while ARMED is refused");
    }

    REARM_FROM_BASELINE();
    // 5. Changing an already-commissioned tc_type while ARMED: refused.
    {
        config_store_record_t rec = baseline;
        rec.tc_type = 0x07u; // MAX31856_TC_TYPE_T
        TEST_CHECK(config_store_write_volatile(&rec, NULL) == false,
                   "changing an already-commissioned tc_type while ARMED is refused (no 'safer' "
                   "direction between TC types)");
    }

    REARM_FROM_BASELINE();
    // 6. A neutral change (no safety field touched) while ARMED: allowed --
    // an ordinary kiln-package swap must not have to unarm the Pico.
    {
        config_store_record_t rec = baseline;
        rec.mains_voltage_v = 240.0f;
        TEST_CHECK(config_store_write_volatile(&rec, NULL) == true,
                   "a non-safety-field change while ARMED is never refused by the carve-out");
        config_store_record_t live;
        config_store_get_full_record(&live);
        TEST_CHECK(live.mains_voltage_v == 240.0f, "the neutral install actually landed");
    }

#undef REARM_FROM_BASELINE
    TEST_CHECK(relay_owner_get_state() == RELAY_OWNER_STATE_ARMED,
               "a refusal never itself changes relay state -- it just declines to write");
}

static void test_seq_increments_and_survives_wraparound(void)
{
    TEST_SECTION("config_store_flash: seq/CRC log across a full 8-slot wraparound");
    reset_all();
    config_store_boot_load();

    // CONFIG_STORE_SLOTS_PER_SECTOR (8) writes plus a few more to force at
    // least one erase-and-wrap, checking seq keeps climbing and the record
    // read back after EVERY write matches what was just written.
    for (uint32_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR + 3u; i++) {
        config_store_record_t rec;
        config_store_default(&rec);
        rec.tc_type = (uint8_t)(i % 8u);
        rec.calibration_missing = false;

        const char *reason = NULL;
        bool ok = config_store_write(&rec, &reason);
        TEST_CHECK(ok == true, "each write in the wraparound loop succeeds");

        uint8_t expect_version = config_store_seq_to_version(i + 1u);
        TEST_CHECK(config_store_get_config_version() == expect_version,
                   "config_version advances with seq, including across the wrap");
        TEST_CHECK(config_store_get_tc_type() == (uint8_t)(i % 8u),
                   "just-written tc_type is immediately visible without a reload");
    }

    // A fresh boot_load (simulated reboot) must still find the LATEST
    // record, not an older one a naive scan might prefer after the sector
    // wrapped and was re-erased.
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == (uint8_t)((CONFIG_STORE_SLOTS_PER_SECTOR + 2u) % 8u),
               "reload after wraparound finds the truly latest record");
}

static void test_safe_execute_timeout_is_reported_and_leaves_cache_unchanged(void)
{
    TEST_SECTION("config_store_flash: hal_flash_safe_execute() TIMEOUT surfaces distinctly");
    reset_all();
    config_store_boot_load();

    fake_flash_script_next_op_status(FAKE_FLASH_OP_SAFE_EXECUTE, HAL_TIMEOUT);

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x03u;

    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == false, "write fails when hal_flash_safe_execute() times out");
    TEST_CHECK(reason != NULL && strstr(reason, "timed out") != NULL,
               "TIMEOUT is reported as a timeout, not folded into a generic failure");

    // The in-RAM cache must not have been advanced by a failed write --
    // config_store_write() only assigns s_cached_record/s_cached_slot AFTER
    // a successful hal_flash_safe_execute().
    TEST_CHECK(config_store_get_config_crc() == 0,
               "a timed-out write leaves the cache at its pre-write (uncommissioned) state");
}

// persist/save logging audit (2026-09-06): config_store_write_cb() used to
// discard hal_flash_program()'s own return value with a bare (void) cast,
// on the argument that it could only ever fail with a caller-bug status.
// The host fake's hal_flash_safe_execute() (fake_flash.c) documents exactly
// why that argument does not cover this case: "any erase/program failure the
// callback triggers is surfaced through ITS own return path ... not through
// this function's return value" -- so a HAL_OK from hal_flash_safe_execute()
// says only that the callback RAN, never that the program landed. This
// pins that config_store_write() now catches a scripted PROGRAM failure
// even though SAFE_EXECUTE itself reports HAL_OK.
static void test_program_failure_is_not_masked_by_safe_execute_ok(void)
{
    TEST_SECTION("config_store_flash: a failed hal_flash_program() is not "
                 "masked by hal_flash_safe_execute()'s own HAL_OK");
    reset_all();
    config_store_boot_load();

    fake_flash_script_next_op_status(FAKE_FLASH_OP_PROGRAM, HAL_IO);

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x03u;

    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == false,
               "write fails when the underlying hal_flash_program() fails, "
               "even though hal_flash_safe_execute() itself returns HAL_OK");
    TEST_CHECK(reason != NULL && strcmp(reason, "ok") != 0,
               "the reason string is not the success sentinel on a masked failure");

    // Same "must not silently advance the cache" property the TIMEOUT test
    // above pins, for the other failure path.
    TEST_CHECK(config_store_get_config_crc() == 0,
               "a program failure the safe_execute wrapper did not itself "
               "report still leaves the cache at its pre-write state");
}

// --- A/B sectors (flash_endurance_review_2026-09-07.md R2) -----------------
//
// Sector A is at SAFTYFW_CONFIG_STORE_FLASH_OFFSET, sector B immediately
// after it at SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B -- both HAL_FLASH_ERASE_
// SIZE-aligned, so fake_flash_get_erase_count()'s sector index is simply
// offset / HAL_FLASH_ERASE_SIZE.
#define SECTOR_A_INDEX (SAFTYFW_CONFIG_STORE_FLASH_OFFSET / HAL_FLASH_ERASE_SIZE)
#define SECTOR_B_INDEX (SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B / HAL_FLASH_ERASE_SIZE)

static void write_default_with_tc_type(uint8_t tc_type)
{
    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = tc_type;
    rec.calibration_missing = false;
    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == true, "setup write succeeds");
}

static void test_switch_to_sector_b_after_sector_a_fills(void)
{
    TEST_SECTION("config_store_flash: A/B -- 9th write switches to sector B");
    reset_all();
    config_store_boot_load();

    // Fill sector A's 8 slots -- no erase needed for any of these (mirrors
    // the pre-A/B wraparound test).
    for (uint32_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        write_default_with_tc_type((uint8_t)(i % 8u));
    }
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_A_INDEX) == 0,
               "filling sector A's 8 slots never erases it");
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_B_INDEX) == 0,
               "sector B untouched while sector A still has room");

    // The 9th write must switch to sector B (erase B, program slot 0),
    // leaving sector A's 8 records untouched (not erased).
    write_default_with_tc_type(0x05u);
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_A_INDEX) == 0,
               "the switch erases the OTHER sector, never the one just filled");
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_B_INDEX) == 1,
               "sector B was erased exactly once, for this switch");
    TEST_CHECK(config_store_get_tc_type() == 0x05u, "the switch write is immediately visible");

    config_store_boot_load(); // simulated reboot
    TEST_CHECK(config_store_get_tc_type() == 0x05u,
               "reboot after a sector switch still finds sector B's record");
}

static void test_wear_is_spread_across_both_sectors(void)
{
    TEST_SECTION("config_store_flash: A/B -- erases alternate between sectors (wear levelling)");
    reset_all();
    config_store_boot_load();

    // 3 full sector-fills' worth of writes (24) forces two switches: A->B,
    // then B->A. Each sector should show ~1 erase per 8 writes, not one
    // sector absorbing all of them the way the single-sector design did.
    for (uint32_t i = 0; i < 3u * CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        write_default_with_tc_type((uint8_t)(i % 8u));
    }
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_A_INDEX) == 1,
               "sector A erased once (the B->A switch at write 17)");
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_B_INDEX) == 1,
               "sector B erased once (the A->B switch at write 9)");
}

static void test_power_loss_mid_erase_of_target_leaves_old_sector_valid(void)
{
    TEST_SECTION("config_store_flash: power cut MID-ERASE of the target sector "
                 "leaves the old sector's record valid");
    reset_all();
    config_store_boot_load();

    for (uint32_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        write_default_with_tc_type((uint8_t)(i % 8u));
    }
    uint8_t last_good_tc_type = config_store_get_tc_type();
    uint16_t crc_before = config_store_get_config_crc();

    // The 9th write must erase sector B first. fake_flash_simulate_power_
    // loss_during() is NOT usable here: it is a one-shot consumed by the
    // very next hal_flash_* call regardless of which primitive that is
    // (fake_flash.h's own doc comment), and config_store_write() always
    // reaches the erase THROUGH hal_flash_safe_execute() -- that outer call
    // is itself a hal_flash_* call, so arming ERASE here would be silently
    // eaten by hal_flash_safe_execute()'s own internal power-loss check
    // before the erase ever runs (confirmed: the write succeeded regardless
    // when this test first used simulate_power_loss_during(ERASE)).
    // fake_flash_script_next_op_status() is the mechanism this repo's other
    // config_store_flash tests already use to reach an op nested inside
    // safe_execute (see test_program_failure_is_not_masked_by_safe_execute_
    // ok() above) -- it is a per-op slot, not consumed by an unrelated call.
    // This models the erase call failing outright with NO effect at all
    // (take_scripted_status() short-circuits before touching the image) --
    // one legitimate power-cut instant: the erase command was issued but
    // nothing landed before power was lost.
    fake_flash_script_next_op_status(FAKE_FLASH_OP_ERASE, HAL_IO);

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x06u;
    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == false, "write fails when the switch-target erase is interrupted");

    // The cache must not have advanced -- same property the existing TIMEOUT/
    // program-failure tests pin, now for an erase failure during a switch.
    TEST_CHECK(config_store_get_tc_type() == last_good_tc_type,
               "in-RAM cache still reports the old sector's record after a failed erase");

    // Simulated reboot: re-scan BOTH sectors from scratch. Sector A must
    // still hold its full, untouched, valid last record -- the erase never
    // reached sector A at all (config_store_write_cb() was only ever handed
    // sector B's region).
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == last_good_tc_type,
               "reboot after an interrupted switch-erase still finds the old sector's record");
    TEST_CHECK(config_store_get_config_crc() == crc_before,
               "the surviving record's CRC/version is exactly what it was before the failed switch");
}

static void test_power_loss_mid_program_of_target_leaves_old_sector_valid(void)
{
    TEST_SECTION("config_store_flash: power cut MID-PROGRAM of the target sector "
                 "leaves the old sector's record valid");
    reset_all();
    config_store_boot_load();

    for (uint32_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        write_default_with_tc_type((uint8_t)(i % 8u));
    }
    uint8_t last_good_tc_type = config_store_get_tc_type();
    uint16_t crc_before = config_store_get_config_crc();

    // fake_flash_simulate_power_loss_during(PROGRAM)'s torn-write model is
    // not reachable through config_store_write() for the same reason the
    // erase test above switched away from it -- hal_flash_safe_execute()'s
    // own internal power-loss check consumes the arm before the inner
    // program ever runs. So this test reproduces the torn-write STATE
    // directly (bypassing config_store_write() entirely for the fault
    // injection), which is arguably the more faithful test anyway: it
    // exercises exactly what the ARBITER (config_store_find_latest_multi_ex(),
    // via config_store_boot_load()) does when it finds a sector genuinely
    // left mid-program by a real power cut, independent of how the write
    // path happens to be plumbed.
    hal_flash_region_t region_b;
    TEST_CHECK(hal_flash_region_init(&region_b, SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B,
                                      SAFTYFW_CONFIG_STORE_FLASH_SIZE) == HAL_OK,
               "fixture can bind sector B's real region");
    TEST_CHECK(hal_flash_erase(&region_b, 0, SAFTYFW_CONFIG_STORE_FLASH_SIZE) == HAL_OK,
               "fixture erases sector B (this IS what the real switch's erase half would do)");

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x06u;
    rec.seq = 999u; // would-be seq -- irrelevant, this record is never fully committed
    uint8_t packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, packed);
    // Program only the FIRST HAL_FLASH_PROGRAM_SIZE (256 B) page of the 512 B
    // record -- exactly fake_flash's own torn-write model (fake_flash.h:
    // "the NEXT hal_flash_program() call writes only the first program-page
    // ... and then reports HAL_IO"), just driven directly instead of through
    // the swallowed power-loss arm. Slot 0's remaining 256 B, and the CRC at
    // offset 504, stay erased (0xFF) -- a real crash mid multi-page
    // flash_range_program() loop leaves exactly this shape.
    TEST_CHECK(hal_flash_program(&region_b, 0, packed, HAL_FLASH_PROGRAM_SIZE) == HAL_OK,
               "fixture programs only the first page of the torn record");

    // Simulated reboot: sector B's slot 0 fails CRC (magic/seq are there,
    // but the CRC field at offset 504 was never programmed and the fields it
    // covers past byte 256 are still 0xFF) -- the arbiter must fall back to
    // sector A's still-fully-valid last record.
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == last_good_tc_type,
               "reboot after a torn switch-program still finds the OLD sector's valid record, "
               "not the torn (CRC-invalid) one in the new sector");
    TEST_CHECK(config_store_get_config_crc() == crc_before,
               "the surviving record is exactly the pre-switch one, seq and all");
    TEST_CHECK(config_store_is_config_rejected() == false,
               "a torn write is an ordinary CRC failure, not the range-rejection diagnostic case");
}

static void test_power_loss_between_erase_and_program_of_target_leaves_old_sector_valid(void)
{
    TEST_SECTION("config_store_flash: power cut BETWEEN erase and program of the "
                 "target sector (erase lands, program never starts) leaves the old "
                 "sector's record valid");
    reset_all();
    config_store_boot_load();

    for (uint32_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        write_default_with_tc_type((uint8_t)(i % 8u));
    }
    uint8_t last_good_tc_type = config_store_get_tc_type();

    // Erase succeeds; program never runs at all (a clean HAL_IO on the very
    // next PROGRAM call, no partial-write model this time) -- this is the
    // exact instant the endurance review's original single-sector defect
    // had ZERO valid copies of the config: sector B is now blank, and
    // nothing has been written to it yet.
    fake_flash_script_next_op_status(FAKE_FLASH_OP_PROGRAM, HAL_IO);

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x06u;
    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == false, "write fails when the program half never runs");

    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == last_good_tc_type,
               "sector B is blank (erased, never programmed) at this exact instant -- "
               "sector A, never touched by this write, is still found and still valid");
}

// D2 fix (docs/audits/rp2040_config_store_write_atomicity_2026-09-14.md):
// a torn write left behind by a power cut mid-program, INSIDE a sector that
// still has room left (not the 8th-write/switch case above -- an ordinary
// same-sector append), used to be silently reused by the very next write:
// config_store_next_write_slot() picks latest_good+1 by arithmetic alone,
// which is exactly the torn slot in this scenario. Programming a fresh
// record on top of those non-erased bytes ANDs the new record against the
// leftover torn bits (hal_flash_program()'s own doc comment), corrupting it
// -- so THIS write would also silently fail to actually land, while every
// flash call along the way still reports HAL_OK. The fix must detect the
// target slot is not erased and switch sectors instead of reusing it.
static void test_torn_inline_slot_is_never_reprogrammed(void)
{
    TEST_SECTION("config_store_flash: a torn write left mid-sector (not a switch) is "
                 "never reprogrammed by the next write -- it switches sectors instead");
    reset_all();
    config_store_boot_load();

    // Three good writes land in sector A slots 0, 1, 2.
    write_default_with_tc_type(0x01u);
    write_default_with_tc_type(0x02u);
    write_default_with_tc_type(0x03u);
    uint8_t last_good_tc_type = config_store_get_tc_type();
    TEST_CHECK(last_good_tc_type == 0x03u, "setup: sector A slot 2 holds the last good record");

    // Directly tear slot 3 of sector A (bypassing config_store_write(), same
    // fixture technique test_power_loss_mid_program_of_target_leaves_old_
    // sector_valid() above uses) -- models a power cut mid-program during
    // what WOULD have been the 4th, ordinary, no-erase-needed append.
    hal_flash_region_t region_a;
    TEST_CHECK(hal_flash_region_init(&region_a, SAFTYFW_CONFIG_STORE_FLASH_OFFSET,
                                      SAFTYFW_CONFIG_STORE_FLASH_SIZE) == HAL_OK,
               "fixture can bind sector A's real region");
    config_store_record_t torn;
    config_store_default(&torn);
    torn.tc_type = 0x09u;
    torn.seq = 999u;
    uint8_t torn_packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&torn, torn_packed);
    TEST_CHECK(hal_flash_program(&region_a, 3u * CONFIG_STORE_RECORD_LEN, torn_packed,
                                  HAL_FLASH_PROGRAM_SIZE) == HAL_OK,
               "fixture tears slot 3: only its first page is programmed");
    uint8_t slot3_after_tear[CONFIG_STORE_RECORD_LEN];
    TEST_CHECK(hal_flash_read(&region_a, 3u * CONFIG_STORE_RECORD_LEN, slot3_after_tear,
                               sizeof(slot3_after_tear)) == HAL_OK,
               "fixture reads back the torn slot for later comparison");

    // Simulated reboot: the arbiter finds slot 2 (last CRC-valid record);
    // slot 3 fails CRC and is skipped, exactly like any other corrupt slot.
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == last_good_tc_type,
               "boot after the mid-sector tear still finds slot 2's valid record");

    // The next write must NOT land back on slot 3. Before the D2 fix this
    // would silently AND-corrupt slot 3 with the new record's bytes; now it
    // must detect slot 3 is not erased and switch to sector B instead.
    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u;
    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == true, "the write itself succeeds (via a sector switch, not slot 3 reuse)");
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_B_INDEX) == 1,
               "the write switched to (and erased) sector B rather than reusing torn slot 3");
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_A_INDEX) == 0,
               "sector A -- still holding the one valid record at slot 2 -- was never erased");

    uint8_t slot3_after_write[CONFIG_STORE_RECORD_LEN];
    TEST_CHECK(hal_flash_read(&region_a, 3u * CONFIG_STORE_RECORD_LEN, slot3_after_write,
                               sizeof(slot3_after_write)) == HAL_OK,
               "fixture re-reads slot 3 after the write");
    TEST_CHECK(memcmp(slot3_after_tear, slot3_after_write, CONFIG_STORE_RECORD_LEN) == 0,
               "slot 3's torn bytes are byte-for-byte untouched -- the write never "
               "reprogrammed over them");

    TEST_CHECK(config_store_get_tc_type() == 0x07u,
               "the new record is immediately visible after the switch");
    config_store_boot_load(); // simulated reboot
    TEST_CHECK(config_store_get_tc_type() == 0x07u,
               "the value present after a simulated reboot matches what was last "
               "successfully written (sector B's record), not slot 2's older one");
}

// Companion to the above from the NO_SLOT (never-committed) side: a torn
// FIRST-EVER write (slot 0 of sector A, before any record ever validated)
// must not be reprogrammed by the next write either.
static void test_torn_first_slot_before_any_valid_record_is_never_reprogrammed(void)
{
    TEST_SECTION("config_store_flash: a torn FIRST write (no valid record ever committed) "
                 "is never reprogrammed by the next write");
    reset_all();
    config_store_boot_load();
    TEST_CHECK(config_store_is_config_rejected() == false,
               "setup: a blank board is not the rejected case, just never-committed");

    hal_flash_region_t region_a;
    TEST_CHECK(hal_flash_region_init(&region_a, SAFTYFW_CONFIG_STORE_FLASH_OFFSET,
                                      SAFTYFW_CONFIG_STORE_FLASH_SIZE) == HAL_OK,
               "fixture can bind sector A's real region");
    config_store_record_t torn;
    config_store_default(&torn);
    torn.tc_type = 0x09u;
    torn.seq = 1u;
    uint8_t torn_packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&torn, torn_packed);
    TEST_CHECK(hal_flash_program(&region_a, 0, torn_packed, HAL_FLASH_PROGRAM_SIZE) == HAL_OK,
               "fixture tears slot 0 of sector A: only its first page is programmed");

    // Simulated reboot: nothing validates -- s_cached_slot == CONFIG_STORE_NO_SLOT.
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u;
    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == true, "the write succeeds (sector A is erased first, reclaiming the torn slot)");
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_A_INDEX) == 1,
               "the NO_SLOT case erases sector A itself before writing -- nothing valid "
               "existed anywhere to protect");
    TEST_CHECK(config_store_get_tc_type() == 0x07u,
               "the new record is immediately visible");
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == 0x07u,
               "the value survives a simulated reboot");
}

static void test_migration_from_legacy_single_sector_layout(void)
{
    TEST_SECTION("config_store_flash: migration -- a board running the OLD "
                 "single-sector firmware's on-flash image loads unchanged");
    reset_all();

    // Build EXACTLY what a board flashed with the pre-A/B firmware has on
    // flash today: real committed records in sector A's slots (packed
    // directly, bypassing config_store_write() so this fixture cannot
    // accidentally depend on the NEW write path), sector B left at its
    // erased-flash 0xFF -- because the old firmware never knew sector B
    // existed and never wrote a byte there. abs_max_temp_c and tc_type are
    // exactly the safety fields the audit named as at risk of a silent
    // reset to defaults.
    config_store_record_t legacy_rec;
    config_store_default(&legacy_rec);
    legacy_rec.seq = 7;
    legacy_rec.tc_type = 0x01u; // MAX31856_TC_TYPE_J -- deliberately non-default
    legacy_rec.abs_max_temp_c = 1300.0f;
    legacy_rec.fields_set |= CONFIG_STORE_SET_ABS_MAX_TEMP_C | CONFIG_STORE_SET_TC_TYPE;
    legacy_rec.calibration_missing = false;

    uint8_t packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&legacy_rec, packed);

    // Write straight into the fake image at sector A's slot 3, sector B
    // untouched (still all-0xFF from reset_all()) -- config_store_flash.c's
    // own hal_flash_region_t for sector A binds at SAFTYFW_CONFIG_STORE_
    // FLASH_OFFSET, so this is byte-for-byte "what real flash already holds".
    hal_flash_region_t region_a;
    TEST_CHECK(hal_flash_region_init(&region_a, SAFTYFW_CONFIG_STORE_FLASH_OFFSET,
                                      SAFTYFW_CONFIG_STORE_FLASH_SIZE) == HAL_OK,
               "fixture can bind sector A's real region");
    TEST_CHECK(hal_flash_program(&region_a, 3u * CONFIG_STORE_RECORD_LEN, packed,
                                  sizeof(packed)) == HAL_OK,
               "fixture can program the legacy record into sector A slot 3");

    // Now boot the NEW (A/B) firmware against this pre-existing image --
    // this is the exact call main.c makes at boot, no migration step exists
    // or is needed.
    config_store_boot_load();

    TEST_CHECK(config_store_get_tc_type() == 0x01u,
               "the pre-existing tc_type is found, not silently reset to the K default");
    TEST_CHECK(config_store_is_calibration_missing() == false,
               "the pre-existing calibration_missing==false survives -- not forced back to true");
    TEST_CHECK(config_store_is_config_rejected() == false,
               "the legacy record is not treated as a rejection");
    config_store_record_t rt;
    config_store_get_full_record(&rt);
    TEST_CHECK(rt.abs_max_temp_c == 1300.0f,
               "abs_max_temp_c (the safety ceiling the audit specifically named) survives intact");
    TEST_CHECK(config_store_field_is_set(&rt.fields_set, CONFIG_STORE_SET_ABS_MAX_TEMP_C),
               "the commissioned bit for abs_max_temp_c survives, not just the raw float");

    // A write after this point must still behave correctly -- proves the
    // cached (sector, slot) this boot_load derived (sector 0, slot 3) feeds
    // config_store_plan_write() sanely, continuing the round-robin from
    // exactly where the legacy image left off rather than assuming slot 0.
    write_default_with_tc_type(0x02u);
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_A_INDEX) == 0,
               "continuing sector A's round-robin from slot 3 needs no erase yet");
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == 0x02u,
               "the post-migration write round-trips normally");
}

static void test_corrupt_active_sector_falls_back_to_other_sector(void)
{
    TEST_SECTION("config_store_flash: a corrupted current sector falls back "
                 "to the other, still-valid sector");
    reset_all();
    config_store_boot_load();

    // Fill A, switch to B (B now holds the current record; A's 8 records
    // are stale but individually still valid -- never erased proactively).
    for (uint32_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        write_default_with_tc_type((uint8_t)(i % 8u));
    }
    write_default_with_tc_type(0x04u); // switch write, lands in sector B slot 0
    TEST_CHECK(config_store_get_tc_type() == 0x04u, "sanity: switch write landed as expected");

    // Directly corrupt sector B's only record (bit-rot / a botched program
    // that still happens to have looked complete to the write path but rots
    // afterward) -- read the whole first program-page back, clear one data
    // byte to 0x00 in the local copy, and reprogram that SAME page (AND-only,
    // matching real NOR semantics: every other byte ANDs with itself and is
    // unchanged; only the forced byte actually clears). hal_flash_program()
    // requires an HAL_FLASH_PROGRAM_SIZE(256)-aligned offset/len -- a bare
    // single byte at offset 20 is not a legal call, hence the whole-page
    // round trip.
    hal_flash_region_t region_b;
    TEST_CHECK(hal_flash_region_init(&region_b, SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B,
                                      SAFTYFW_CONFIG_STORE_FLASH_SIZE) == HAL_OK,
               "fixture can bind sector B's real region");
    uint8_t page0[HAL_FLASH_PROGRAM_SIZE];
    TEST_CHECK(hal_flash_read(&region_b, 0u, page0, sizeof(page0)) == HAL_OK,
               "fixture can read sector B's first page");
    // Offset 20 (inside abs_max_temp_c) is a bad choice -- config_store_
    // default()'s abs_max_temp_c is 0.0f, whose bytes are already all-zero,
    // so forcing it to 0 would be a no-op that leaves the CRC untouched and
    // proves nothing. Byte 0 (part of the magic, "KLC1") is never zero in a
    // real record -- forcing it to 0x00 is guaranteed to break the record.
    page0[0] = 0x00u; // clearing bits is always legal without an erase
    TEST_CHECK(hal_flash_program(&region_b, 0u, page0, sizeof(page0)) == HAL_OK,
               "fixture can corrupt sector B's record in place");

    // Reboot: sector B's slot 0 now fails CRC. Sector A's slot 7 (seq one
    // less than B's) must be found instead, SILENTLY -- no rejection is
    // reported, because a corrupt/superseded slot is not the "structurally-
    // valid-but-range-refused" case config_store_is_config_rejected() exists
    // to flag (see config_store.h's "Load-time rejection diagnostics").
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == (uint8_t)((CONFIG_STORE_SLOTS_PER_SECTOR - 1u) % 8u),
               "corrupted sector B is skipped; sector A's last-good record is used instead");
    TEST_CHECK(config_store_is_config_rejected() == false,
               "falling back to the other sector is not reported as a rejection -- ordinary skip");
}

// --- Seqlock concurrency test (2026-09-09) ----------------------------------
//
// WHAT THIS DOES AND DOES NOT PROVE. This host test cannot model two
// Cortex-M0+ cores, their memory-ordering rules, or the AHB-Lite fabric
// between them -- there is no RP2040 in this build. What it CAN model
// honestly: config_store_record_t is 512 bytes, far bigger than any atomic
// access this compiler/CPU can do in one instruction, so a plain struct
// assignment (the pre-seqlock code: `s_cached_record = to_write;`) is
// necessarily a multi-store copy that a genuinely concurrent reader,
// running on a real OS thread that this host DOES schedule onto a separate
// physical core, can observe mid-copy. Two real Windows threads (one
// hammering config_store_write(), one hammering config_store_get_full_
// record()) is a real concurrent-memory-tearing hazard, just not the exact
// RP2040 cross-core hazard the seqlock was written for -- it exercises "is
// the read/write pair internally consistent under real concurrency", not
// "does this specific barrier sequence hold on Cortex-M0+ silicon". Treat a
// PASS here as evidence the seqlock protocol itself is sound under real
// concurrent access, not as proof the HAL_DMB() choice is correct on
// RP2040 hardware -- that requires the actual bench board.
//
// Detection method: each write sets TWO fields that live far apart inside
// the 512-byte record (tc_type near the front, estop_active_level near the
// end -- config_store.h's field layout) to values that encode the SAME
// alternating generation bit. A reader that ever observes the two fields
// disagreeing has necessarily read a struct that was partway through being
// overwritten -- there is no valid committed record in which they differ,
// since every write sets both from the same `g`.
typedef struct {
    volatile LONG stop;
    volatile LONG torn_count;
    volatile LONG consistent_count;
    volatile LONG write_count;
} seqlock_race_ctx_t;

static DWORD WINAPI seqlock_race_writer_fn(LPVOID param)
{
    seqlock_race_ctx_t *ctx = (seqlock_race_ctx_t *)param;
    uint8_t g = 0;
    while (!ctx->stop) {
        config_store_record_t rec;
        config_store_default(&rec);
        rec.calibration_missing = false;
        rec.tc_type = g ? 0x02u : 0x01u; // both <= 7 (config_params.c's RANGE_U8_MAX), no fields_set gating required
        rec.estop_active_level = g ? DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW
                                    : DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH;
        const char *reason = NULL;
        (void)config_store_write(&rec, &reason);
        InterlockedIncrement(&ctx->write_count);
        g = (uint8_t)(g ^ 1u);
    }
    return 0;
}

static DWORD WINAPI seqlock_race_reader_fn(LPVOID param)
{
    seqlock_race_ctx_t *ctx = (seqlock_race_ctx_t *)param;
    while (!ctx->stop) {
        config_store_record_t snap;
        config_store_get_full_record(&snap);
        uint8_t g_from_tc_type = (snap.tc_type == 0x02u) ? 1u : 0u;
        uint8_t g_from_estop = (snap.estop_active_level == DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW) ? 1u : 0u;
        if (g_from_tc_type != g_from_estop) {
            InterlockedIncrement(&ctx->torn_count);
        } else {
            InterlockedIncrement(&ctx->consistent_count);
        }
    }
    return 0;
}

static void test_seqlock_concurrent_read_never_tears(void)
{
    TEST_SECTION("config_store_flash: concurrent writer/reader never observes a torn record "
                 "(see this test's own header comment for exactly what this does and does not prove)");
    reset_all();
    config_store_boot_load();

    seqlock_race_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    HANDLE writer = CreateThread(NULL, 0, seqlock_race_writer_fn, &ctx, 0, NULL);
    HANDLE reader = CreateThread(NULL, 0, seqlock_race_reader_fn, &ctx, 0, NULL);
    TEST_CHECK(writer != NULL && reader != NULL, "fixture can start both race threads");

    Sleep(500); // real wall-clock window for the OS scheduler to interleave both threads
    InterlockedExchange(&ctx.stop, 1);
    WaitForSingleObject(writer, INFINITE);
    WaitForSingleObject(reader, INFINITE);
    CloseHandle(writer);
    CloseHandle(reader);

    printf("    (writes=%ld, consistent reads=%ld, torn reads=%ld)\n",
           ctx.write_count, ctx.consistent_count, ctx.torn_count);

    TEST_CHECK(ctx.write_count > 0, "sanity: the writer thread actually ran");
    TEST_CHECK(ctx.consistent_count > 0, "sanity: the reader thread actually ran");
    TEST_CHECK(ctx.torn_count == 0,
               "no reader ever observed tc_type/estop_active_level disagreeing -- "
               "the seqlock rejected or masked every torn snapshot");
}

// --- Multi-reader seqlock fallback test (2026-09-09, opus review) ----------
//
// The single-reader test above is exactly what let b202fe56/5671ee03's
// fallback bug through: with only one reader thread, there is no second
// reader around to race against config_store_seqlock_read()'s (pre-fix)
// unsynchronised `s_last_good_record = copy;` write, which every
// successful read used to perform regardless of whether that call was on
// the exhausted-retries fallback path or not. On real hardware the two
// concurrent readers are core-1's trip-path callers (safety_core.c,
// thermo_task.c, current_task.c) -- so this test spawns several reader
// threads against one writer thread, all real OS threads the host
// scheduler does put on separate physical cores, same rationale as the
// single-reader test's header comment above.
//
// SAME CAVEAT AS ABOVE APPLIES: this proves the read-side protocol has no
// internal data race under real concurrent access on x86/Windows threads;
// it does not and cannot prove the HAL_DMB()/hardware/sync.h barrier choice
// is correct on actual Cortex-M0+ silicon. That still requires the bench
// board.
#define SEQLOCK_RACE_READER_COUNT 4

static void test_seqlock_multi_reader_never_tears(void)
{
    TEST_SECTION("config_store_flash: MULTIPLE concurrent readers never observe a torn record "
                 "(regression test for the writer-owned-fallback fix -- a single reader thread "
                 "cannot exercise this)");
    reset_all();
    config_store_boot_load();
    config_store_test_fallback_taken_count_reset();

    seqlock_race_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    HANDLE writer = CreateThread(NULL, 0, seqlock_race_writer_fn, &ctx, 0, NULL);
    TEST_CHECK(writer != NULL, "fixture can start the writer thread");

    HANDLE readers[SEQLOCK_RACE_READER_COUNT];
    for (int i = 0; i < SEQLOCK_RACE_READER_COUNT; i++) {
        readers[i] = CreateThread(NULL, 0, seqlock_race_reader_fn, &ctx, 0, NULL);
        TEST_CHECK(readers[i] != NULL, "fixture can start each reader thread");
    }

    Sleep(500); // real wall-clock window for the OS scheduler to interleave all threads
    InterlockedExchange(&ctx.stop, 1);
    WaitForSingleObject(writer, INFINITE);
    CloseHandle(writer);
    for (int i = 0; i < SEQLOCK_RACE_READER_COUNT; i++) {
        WaitForSingleObject(readers[i], INFINITE);
        CloseHandle(readers[i]);
    }

    uint32_t fallback_taken = config_store_test_fallback_taken_count();
    printf("    (%d readers; writes=%ld, consistent reads=%ld, torn reads=%ld, "
           "fallback_taken=%lu)\n",
           SEQLOCK_RACE_READER_COUNT, ctx.write_count, ctx.consistent_count, ctx.torn_count,
           (unsigned long)fallback_taken);

    TEST_CHECK(ctx.write_count > 0, "sanity: the writer thread actually ran");
    TEST_CHECK(ctx.consistent_count > 0, "sanity: the reader threads actually ran");
    // opus review 2026-09-09: a torn_count==0 result proves nothing about the
    // fallback path specifically unless the fallback path actually ran at
    // least once during this test -- otherwise "0 torn" is equally
    // consistent with "the fallback was exercised and never tore" (what this
    // test claims) and "the primary seqlock always won and the fallback
    // branch never executed" (a vacuous pass). See s_fallback_taken_count's
    // comment in config_store_flash.c.
    TEST_CHECK(fallback_taken > 0,
               "the exhausted-retries fallback path was actually taken at least once during "
               "this race -- otherwise the torn_count==0 check below is vacuous");
    TEST_CHECK(ctx.torn_count == 0,
               "no reader ever observed tc_type/estop_active_level disagreeing with "
               "MULTIPLE concurrent readers -- the fallback double buffer's own seqlock "
               "(s_fallback_gen) means a reader that catches the writer mid-commit, "
               "including across two commits (ABA), retries instead of trusting a torn copy");
}

// --- Deterministic fallback-buffer ABA test (2026-09-09, opus review finding A) --
//
// The real-thread races above are honest about what they can and cannot
// prove (see that section's own header comment) -- and in practice, even
// heavily loaded, they did not land the exact two-commits-during-one-copy
// interleaving needed to exercise the ABA this fix closes: the window is a
// handful of instructions wide, not a useful fraction of an OS scheduling
// quantum. This test does not rely on OS thread timing at all. It uses
// config_store_test_force_fallback_path() to make config_store_seqlock_
// read() go straight to the fallback branch (single-threaded, no writer is
// ever actually racing the primary seqlock, so it would otherwise never
// fail on its own), and config_store_test_set_fallback_hook() to run two
// full config_store_write() commits from INSIDE the fallback branch, in the
// gap between the two halves of its own copy -- deterministically
// reproducing "the writer lands two commits while a reader is mid-copy of
// the fallback buffer", landing the second one back in the exact slot the
// reader started reading. See config_store_flash.c's s_fallback_gen comment
// for why this specific shape (index returns to the same value, but the
// buffer underneath was rewritten in between) defeats a bare index recheck.
static int s_aba_hook_calls;      // every time the production code invoked the hook pointer
static int s_aba_hook_commits_ran; // guards the actual writes to fire only once -- the fix's
                                    // own retry loop calls this hook again on every fallback
                                    // attempt it takes (up to CONFIG_STORE_FALLBACK_SEQLOCK_
                                    // MAX_RETRIES times), and re-running two more commits on a
                                    // later retry would just move the goalposts, not test
                                    // anything -- one deliberate double-commit is the scenario.

static void aba_two_commits_hook(void)
{
    s_aba_hook_calls++;
    if (s_aba_hook_commits_ran) {
        return;
    }
    s_aba_hook_commits_ran = 1;
    config_store_record_t rec;
    config_store_default(&rec);
    rec.calibration_missing = false;
    const char *reason = NULL;
    // First commit: writer targets the OTHER slot from whatever the reader
    // is currently reading and flips active there (safe on its own -- the
    // reader's slot is untouched by this one commit).
    rec.tc_type = 0x01u;
    rec.estop_active_level = DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH;
    TEST_CHECK(config_store_write(&rec, &reason), "ABA test: first in-flight commit succeeds");
    // Second commit: writer's target flips back to the reader's ORIGINAL
    // slot -- overwriting the exact buffer the reader is mid-copy of.
    rec.tc_type = 0x02u;
    rec.estop_active_level = DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW;
    TEST_CHECK(config_store_write(&rec, &reason), "ABA test: second in-flight commit succeeds");
}

static void test_fallback_aba_two_commits_during_one_copy(void)
{
    TEST_SECTION("config_store_flash: fallback buffer survives TWO writer commits landing "
                 "during one reader's copy (deterministic ABA regression test, opus review "
                 "finding A -- ff2506e9-shape fix)");
    reset_all();
    config_store_boot_load();

    // Seed the fallback buffer with a known, stable record (tc_type=0x01,
    // ESTOP_ACTIVE_HIGH) before forcing the race, so the "before" state is
    // well-defined and distinct from both in-flight commits the hook makes.
    config_store_record_t seed;
    config_store_default(&seed);
    seed.calibration_missing = false;
    seed.tc_type = 0x01u;
    seed.estop_active_level = DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH;
    const char *reason = NULL;
    TEST_CHECK(config_store_write(&seed, &reason), "fixture: seed commit succeeds");

    s_aba_hook_calls = 0;
    s_aba_hook_commits_ran = 0;
    config_store_test_force_fallback_path(true);
    config_store_test_set_fallback_hook(aba_two_commits_hook);

    config_store_record_t snap;
    memset(&snap, 0, sizeof(snap));
    config_store_get_full_record(&snap);

    config_store_test_set_fallback_hook(NULL);
    config_store_test_force_fallback_path(false);

    TEST_CHECK(s_aba_hook_calls >= 1,
               "fixture: the hook ran at least once during the read (the fix's own retry loop "
               "may call it again on a later attempt; only the first invocation's two commits "
               "actually run, see the hook's own guard)");
    TEST_CHECK(s_aba_hook_commits_ran == 1, "fixture: the two in-flight commits actually ran");

    uint8_t g_from_tc_type = (snap.tc_type == 0x02u) ? 1u : (snap.tc_type == 0x01u ? 0u : 0xFFu);
    uint8_t g_from_estop =
        (snap.estop_active_level == DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW)
            ? 1u
            : (snap.estop_active_level == DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH ? 0u : 0xFFu);
    printf("    (snap.tc_type=0x%02X snap.estop_active_level=%u -> g_tc=%u g_estop=%u)\n",
           snap.tc_type, snap.estop_active_level, g_from_tc_type, g_from_estop);
    TEST_CHECK(g_from_tc_type != 0xFFu && g_from_estop != 0xFFu,
               "fixture: the returned record carries one of the known generations in each field");
    TEST_CHECK(g_from_tc_type == g_from_estop,
               "the fallback seqlock's generation recheck caught the two in-flight commits and "
               "did not hand back a record mixing tc_type from one generation with "
               "estop_active_level from another -- this is the exact ABA a bare 0/1 index "
               "recheck cannot detect (the index legitimately returns to the same value)");
}

// 2026-09-23, Opus review of the original config_check_period_s work --
// config_store_check_ram_integrity()'s repair used to install
// config_store_default() (silently zeroing abs_max_temp_c, disabling S1)
// directly via config_store_seqlock_write(), bypassing config_store_write_
// ex()'s "refuse while ARMED" gate. These three tests exercise the corrected
// design: restore from s_persisted_record (the real, committed flash copy),
// never compiled defaults; keep the cache's seq in lockstep with the
// persisted seq so config_store_is_volatile_dirty() reads false afterward;
// and only escalate to a recurrence-trip request on a SECOND corruption
// within the same boot.
static void test_ram_integrity_repair_restores_persisted_not_defaults(void)
{
    TEST_SECTION("config_store_flash: ram_integrity repair restores persisted record, not defaults");
    reset_all();
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u; // MAX31856_TC_TYPE_T -- a real, non-default committed value
    rec.calibration_missing = false;
    rec.abs_max_temp_c = 950.0f; // a real ceiling; config_store_default() would zero this
    rec.fields_set |= CONFIG_STORE_SET_ABS_MAX_TEMP_C;
    const char *reason = NULL;
    TEST_CHECK(config_store_write(&rec, &reason) == true, "fixture: durable commit accepted");
    TEST_CHECK(config_store_is_volatile_dirty() == false, "fixture: freshly committed, not dirty");
    TEST_CHECK(config_store_get_ram_integrity_fail_count() == 0u, "fixture: no corruption yet");

    config_store_test_corrupt_cached_record();
    TEST_CHECK(config_store_check_ram_integrity() == false,
               "corruption is detected (cache no longer hashes to its tracked CRC)");
    TEST_CHECK(config_store_get_ram_integrity_fail_count() == 1u,
               "the fail counter increments exactly once for one corruption");

    config_store_record_t after;
    TEST_CHECK(config_store_get_full_record(&after) == true, "fixture: repaired record readable");
    TEST_CHECK(after.tc_type == 0x07u,
               "repair restores the PERSISTED (committed) tc_type, not config_store_default()'s");
    TEST_CHECK(after.abs_max_temp_c == 950.0f,
               "repair restores the real abs_max_temp_c -- never silently zeroed (S1 must not go dark)");
    TEST_CHECK(after.calibration_missing == true,
               "calibration_missing is forced true on a repaired record regardless of what was persisted "
               "(SAFETY_MODEL.md section 4: report calibration_missing on detection)");
    TEST_CHECK(config_store_is_volatile_dirty() == false,
               "restoring the whole record (seq included) keeps cache/persisted seq in lockstep -- a "
               "repair must never be mistaken for a live volatile install");
    TEST_CHECK(config_store_ram_integrity_recurrence_pending() == false,
               "a single corruption this boot is WARN-only -- no trip requested yet");
}

static void test_ram_integrity_second_corruption_this_boot_requests_trip(void)
{
    TEST_SECTION("config_store_flash: second ram_integrity corruption this boot requests a trip");
    reset_all();
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.calibration_missing = false;
    const char *reason = NULL;
    TEST_CHECK(config_store_write(&rec, &reason) == true, "fixture: durable commit accepted");

    config_store_test_corrupt_cached_record();
    TEST_CHECK(config_store_check_ram_integrity() == false, "fixture: first corruption detected and repaired");
    TEST_CHECK(config_store_ram_integrity_recurrence_pending() == false,
               "fixture: first detection this boot does not request a trip");

    config_store_test_corrupt_cached_record();
    TEST_CHECK(config_store_check_ram_integrity() == false, "second corruption detected and repaired");
    TEST_CHECK(config_store_get_ram_integrity_fail_count() == 2u,
               "the fail counter reflects both corruptions");
    TEST_CHECK(config_store_ram_integrity_recurrence_pending() == true,
               "a SECOND corruption within the same boot escalates to a trip request "
               "(safety_core.c's S16/SAFETY_TRIP_CONFIG_CORRUPT reads this every tick)");

    // The repair itself remains correct even on recurrence -- still a real
    // record, not defaults, and still not left volatile-dirty.
    config_store_record_t after;
    TEST_CHECK(config_store_get_full_record(&after) == true, "fixture: repaired record readable");
    TEST_CHECK(after.calibration_missing == true, "calibration_missing still forced true on recurrence");
    TEST_CHECK(config_store_is_volatile_dirty() == false, "still not volatile-dirty on recurrence");
}

static void test_ram_integrity_repair_discards_live_volatile_install(void)
{
    TEST_SECTION("config_store_flash: ram_integrity repair discards a live volatile install, not just RAM corruption");
    reset_all();
    config_store_boot_load();

    // Durable, committed base: tc_type A.
    config_store_record_t durable;
    config_store_default(&durable);
    durable.tc_type = 0x01u; // MAX31856_TC_TYPE_K
    durable.calibration_missing = false;
    const char *reason = NULL;
    TEST_CHECK(config_store_write(&durable, &reason) == true, "fixture: durable commit (tc_type K) accepted");

    // A legitimate RAM-only volatile install moves the cache to tc_type B,
    // ahead of flash-truth -- this is NOT corruption (config_store_is_
    // volatile_dirty() would correctly read true right now).
    config_store_record_t volatile_rec;
    config_store_default(&volatile_rec);
    volatile_rec.tc_type = 0x07u; // MAX31856_TC_TYPE_T
    volatile_rec.calibration_missing = false;
    TEST_CHECK(config_store_write_volatile(&volatile_rec, NULL) == true,
               "fixture: volatile install (tc_type T) accepted");
    TEST_CHECK(config_store_is_volatile_dirty() == true, "fixture: cache now legitimately ahead of flash-truth");

    // Now corrupt the (volatile-dirty) cache on top of that.
    config_store_test_corrupt_cached_record();
    TEST_CHECK(config_store_check_ram_integrity() == false, "corruption on top of a volatile install is detected");

    config_store_record_t after;
    TEST_CHECK(config_store_get_full_record(&after) == true, "fixture: repaired record readable");
    TEST_CHECK(after.tc_type == 0x01u,
               "repair restores the PERSISTED tc_type (K) -- the live volatile install (T) is discarded, "
               "not preserved, since it cannot be trusted once the RAM holding it was found corrupted");
    TEST_CHECK(config_store_is_volatile_dirty() == false,
               "the repair also clears the (now-discarded) volatile-dirty state");
}

// 2026-09-23 (s_persisted_record CRC hardening, coordinator-required
// follow-up): s_persisted_record is the repair's only known-good fallback,
// and it lives in RAM too -- corrupt it too and there is nothing left to
// restore from.
static void test_ram_integrity_both_copies_corrupted_requests_trip_no_repair(void)
{
    TEST_SECTION("config_store_flash: both cached AND persisted copies corrupted -- no known-good copy, immediate trip, no repair");
    reset_all();
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u;
    rec.calibration_missing = false;
    rec.abs_max_temp_c = 950.0f;
    rec.fields_set |= CONFIG_STORE_SET_ABS_MAX_TEMP_C;
    const char *reason = NULL;
    TEST_CHECK(config_store_write(&rec, &reason) == true, "fixture: durable commit accepted");
    TEST_CHECK(config_store_ram_integrity_recurrence_pending() == false, "fixture: no trip requested yet");

    config_store_test_corrupt_cached_record();
    config_store_test_corrupt_persisted_record();
    TEST_CHECK(config_store_check_ram_integrity() == false,
               "still reports a failure (nothing to report success about)");
    TEST_CHECK(config_store_ram_integrity_recurrence_pending() == true,
               "no known-good copy exists -- S16 is requested on this very first detection, "
               "not deferred to a second corruption");

    // No repair was attempted: config_store_check_ram_integrity()'s repair
    // path unconditionally forces calibration_missing=true on the record it
    // installs (see the other ram_integrity tests above) -- this call
    // returned before ever reaching that install, so calibration_missing
    // must still read whatever the (corrupted) cache already had, false
    // here, never flipped true by a skipped repair.
    config_store_record_t after;
    TEST_CHECK(config_store_get_full_record(&after) == true, "record still readable (seqlock unaffected)");
    TEST_CHECK(after.calibration_missing == false,
               "no repair ran -- calibration_missing was never forced true by the (skipped) install");
}

// A persisted-copy-only corruption is never itself examined by the periodic
// check -- s_persisted_record's CRC is only consulted as a would-be repair
// source, and with a healthy cache no repair is ever attempted. This must
// stay silent: nothing should trip or "repair" anything just because the
// fallback copy alone went bad while nothing needs it.
static void test_ram_integrity_persisted_only_corruption_with_healthy_cache_is_a_no_op(void)
{
    TEST_SECTION("config_store_flash: persisted-copy-only corruption with a healthy cache trips and repairs nothing");
    reset_all();
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.calibration_missing = false;
    const char *reason = NULL;
    TEST_CHECK(config_store_write(&rec, &reason) == true, "fixture: durable commit accepted");

    config_store_test_corrupt_persisted_record();
    TEST_CHECK(config_store_check_ram_integrity() == true,
               "cached record's own CRC is still fine -- the periodic check only examines "
               "s_cached_record's CRC as its trigger, so this reports healthy");
    TEST_CHECK(config_store_get_ram_integrity_fail_count() == 0u, "no failure counted");
    TEST_CHECK(config_store_ram_integrity_recurrence_pending() == false, "no trip requested");

    config_store_record_t after;
    TEST_CHECK(config_store_get_full_record(&after) == true, "record still readable");
    TEST_CHECK(after.calibration_missing == false,
               "cache is untouched -- a quietly corrupted persisted copy that is never needed "
               "as a repair source must not itself change anything");
}

// KILNLINK_ROBUSTNESS_AUDIT_2026-10-09 L3: a write whose record is
// byte-identical to the stored slot is answered without touching flash. The
// fake is scripted to fail the next safe_execute AND the next program, so a
// write that reaches either one returns false; the identical write must still
// succeed, and the stored seq (config_version) must not move.
static void test_identical_write_skips_flash(void)
{
    TEST_SECTION("config_store_flash: byte-identical write is not written again (audit L3)");
    reset_all();
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u;
    rec.calibration_missing = false;
    const char *reason = NULL;
    TEST_CHECK(config_store_write(&rec, &reason) == true, "fixture: first write lands");
    uint8_t version_after_first = config_store_get_config_version();
    uint32_t erases_a = fake_flash_get_erase_count(SAFTYFW_CONFIG_STORE_FLASH_OFFSET / HAL_FLASH_ERASE_SIZE);
    uint32_t erases_b = fake_flash_get_erase_count(SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B / HAL_FLASH_ERASE_SIZE);

    fake_flash_script_next_op_status(FAKE_FLASH_OP_SAFE_EXECUTE, HAL_IO);
    fake_flash_script_next_op_status(FAKE_FLASH_OP_PROGRAM, HAL_IO);
    config_store_write_decision_t decision = CONFIG_STORE_WRITE_REFUSED_ARMED;
    reason = NULL;
    bool ok = config_store_write_ex(&rec, false, &reason, &decision);
    fake_flash_script_next_op_status(FAKE_FLASH_OP_SAFE_EXECUTE, HAL_OK);
    fake_flash_script_next_op_status(FAKE_FLASH_OP_PROGRAM, HAL_OK);

    TEST_CHECK(ok == true, "identical write succeeds without reaching safe_execute or program");
    TEST_CHECK(decision == CONFIG_STORE_WRITE_OK, "identical write reports WRITE_OK");
    TEST_CHECK(reason != NULL && strncmp(reason, "ok", 2) == 0, "identical write reason starts with 'ok'");
    TEST_CHECK(config_store_get_config_version() == version_after_first,
               "identical write does not bump the stored seq / config_version");
    TEST_CHECK(fake_flash_get_erase_count(SAFTYFW_CONFIG_STORE_FLASH_OFFSET / HAL_FLASH_ERASE_SIZE) == erases_a &&
                   fake_flash_get_erase_count(SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B / HAL_FLASH_ERASE_SIZE) == erases_b,
               "identical write erases nothing");

    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == 0x07u && config_store_get_config_version() == version_after_first,
               "a reload still finds the original slot, unchanged");

    // Control: a CHANGED record under the same scripted failure must fail,
    // proving the scripting above would have caught a real write.
    rec.tc_type = 0x03u;
    fake_flash_script_next_op_status(FAKE_FLASH_OP_SAFE_EXECUTE, HAL_IO);
    ok = config_store_write(&rec, NULL);
    fake_flash_script_next_op_status(FAKE_FLASH_OP_SAFE_EXECUTE, HAL_OK);
    TEST_CHECK(ok == false, "control: a changed record does reach safe_execute and fails there");
    TEST_CHECK(config_store_write(&rec, NULL) == true, "a changed record still writes normally");
    TEST_CHECK(config_store_get_config_version() != version_after_first, "a real write bumps the seq");
}

// L3: an identical durable commit after a volatile install must still drop
// the volatile install (the cache becomes flash truth again), exactly as a
// real write of the same content would.
static void test_identical_write_after_volatile_clears_dirty(void)
{
    TEST_SECTION("config_store_flash: identical commit after a volatile install clears dirty (audit L3)");
    reset_all();
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u;
    rec.calibration_missing = false;
    TEST_CHECK(config_store_write(&rec, NULL) == true, "fixture: committed");
    uint8_t persisted = config_store_get_persisted_config_version();

    config_store_record_t vol = rec;
    vol.tc_type = 0x03u;
    TEST_CHECK(config_store_write_volatile(&vol, NULL) == true, "fixture: volatile install accepted");
    TEST_CHECK(config_store_is_volatile_dirty() == true, "fixture: cache is ahead of flash");
    TEST_CHECK(config_store_get_tc_type() == 0x03u, "fixture: volatile value is live");

    TEST_CHECK(config_store_write(&rec, NULL) == true, "identical durable commit accepted");
    TEST_CHECK(config_store_is_volatile_dirty() == false, "identical commit clears volatile-dirty");
    TEST_CHECK(config_store_get_tc_type() == 0x07u, "the committed value is live again");
    TEST_CHECK(config_store_get_persisted_config_version() == persisted,
               "no flash write: the persisted version did not move");
    TEST_CHECK(config_store_get_config_version() == persisted, "cache and flash versions agree");
}

// L3 never turns a refusal into a success: an identical record is still a
// full-record write and is refused while ARMED, before the skip is reached.
static void test_identical_write_still_refused_while_armed(void)
{
    TEST_SECTION("config_store_flash: identical write is still refused while ARMED (audit L3)");
    reset_all();
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u;
    rec.calibration_missing = false;
    TEST_CHECK(config_store_write(&rec, NULL) == true, "fixture: committed while idle");

    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);
    config_store_write_decision_t decision = CONFIG_STORE_WRITE_OK;
    bool ok = config_store_write_ex(&rec, false, NULL, &decision);
    TEST_CHECK(ok == false, "identical write refused while ARMED");
    TEST_CHECK(decision != CONFIG_STORE_WRITE_OK, "decision is a refusal, not WRITE_OK");
}

// 2026-10-09 guard-fixes review HIGH-1 / MED-1 / INFO / LOW-3.
//
// The volatile-install gate is fail-closed: while heat is possible, ANY
// trip-relevant field that differs from the running record is refused, except
// an explicit allowlist of non-safety fields (kind ALLOWED below) and the
// provable tightenings (abs_max_temp_c / max_rate_c_per_min / tc_type, kind
// SKIP here, covered by test_write_volatile_refuses_loosening_while_armed).
// The table below classifies EVERY byte of config_store_record_t: the
// coverage check fails when a field is added (or carved out of `reserved`)
// without being classified here, which is the "unclassified new field fails
// the test" requirement.
enum { K_SAFETY = 0, K_ALLOWED = 1, K_SKIP = 2 };
typedef struct {
    const char *name;
    size_t off;
    size_t size;
    int kind;
} f1_field_t;
#define F1(member, kind_) { #member, offsetof(config_store_record_t, member), sizeof(((config_store_record_t *)0)->member), kind_ }
static const f1_field_t k_f1_fields[] = {
    F1(format_version, K_ALLOWED), F1(seq, K_ALLOWED), F1(fields_set, K_SKIP),
    F1(tc_source, K_SAFETY), F1(borrowed_zone_index, K_SAFETY), F1(tc_placement_mode, K_SAFETY),
    F1(abs_max_temp_c, K_SKIP), F1(tc_type, K_SKIP), F1(ct_channel_map, K_SAFETY),
    F1(ct_installed, K_SAFETY), F1(safety_tc_installed, K_SAFETY), F1(calibration_missing, K_ALLOWED),
    F1(firing_margin_c, K_SAFETY), F1(overshoot_margin_c, K_SAFETY), F1(overshoot_time_s, K_SAFETY),
    F1(max_rate_c_per_min, K_SKIP), F1(rate_window_s, K_SAFETY), F1(blind_grace_s, K_SAFETY),
    F1(frozen_window_s, K_SAFETY), F1(tc_disagreement_c, K_SAFETY), F1(tc_disagreement_time_s, K_SAFETY),
    F1(tc_expected_offset_c, K_SAFETY), F1(cj_warn_c, K_SAFETY), F1(cj_max_c, K_SAFETY),
    F1(cj_time_s, K_SAFETY), F1(borrowed_stale_s, K_SAFETY), F1(borrowed_stale_trip_s, K_SAFETY),
    F1(borrowed_type_expected, K_SAFETY), F1(i_present_a, K_SAFETY), F1(zero_counts, K_SAFETY),
    F1(correlation_window_s, K_SAFETY), F1(stuck_on_time_s, K_SAFETY), F1(trip_verify_s, K_SAFETY),
    F1(k_ct_v_per_a, K_ALLOWED), F1(gain, K_SAFETY), F1(mains_voltage_v, K_ALLOWED),
    F1(power_window_s, K_ALLOWED), F1(context_max_age_s, K_SAFETY), F1(link_timeout_s, K_SAFETY),
    F1(link_dead_hard_s, K_SAFETY), F1(mainfault_debounce_ms, K_SAFETY), F1(telemetry_period_ms, K_ALLOWED),
    F1(startup_grace_s, K_SAFETY), F1(estop_debounce_ms, K_SAFETY), F1(watchdog_timeout_ms, K_SAFETY),
    F1(config_check_period_s, K_SAFETY), F1(ct_cal, K_SAFETY), F1(max_expected_power_w, K_ALLOWED),
    F1(i_normal_a, K_SAFETY), F1(overcurrent_pct, K_SAFETY), F1(overcurrent_time_s, K_SAFETY),
    F1(ct_topology, K_SAFETY), F1(zone_ct_channel, K_SAFETY), F1(i_present_a_manual, K_ALLOWED),
    F1(tc_offset_c, K_SAFETY), F1(estop_active_level, K_SAFETY), F1(reserved, K_ALLOWED),
};
#undef F1
// Bytes of config_store_record_t that belong to no member (compiler padding).
// A NEW member that is not in the table raises the uncovered count above this
// and fails the coverage test -- classify it in the table, do not bump this.
#define F1_EXPECTED_PADDING_BYTES 17u // measured: MSVC alignment padding between members

static void test_volatile_gate_classifies_every_record_byte(void)
{
    TEST_SECTION("config_store_flash: every record byte is classified (new field fails here)");
    unsigned char *probe = (unsigned char *)malloc(sizeof(config_store_record_t));
    TEST_CHECK(probe != NULL, "alloc");
    if (probe == NULL) {
        return;
    }
    memset(probe, 0xAA, sizeof(config_store_record_t));
    for (size_t i = 0; i < sizeof k_f1_fields / sizeof k_f1_fields[0]; i++) {
        memset(probe + k_f1_fields[i].off, 0x00, k_f1_fields[i].size);
    }
    size_t uncovered = 0;
    for (size_t b = 0; b < sizeof(config_store_record_t); b++) {
        if (probe[b] == 0xAA) {
            uncovered++;
        }
    }
    printf("  uncovered (padding) bytes: %u\n", (unsigned)uncovered);
    TEST_CHECK(uncovered == F1_EXPECTED_PADDING_BYTES,
               "config_store_record_t has a byte that no table entry classifies: add the new field to "
               "k_f1_fields as K_SAFETY (default) or, only if provably non-safety, K_ALLOWED and to the "
               "allowlist in config_store_volatile_would_loosen_safety()");
    free(probe);
}

static bool f1_flip_and_try(size_t byte_off, bool heat_possible, int mode)
{
    reset_all();
    config_store_boot_load();
    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_GRACE);
    config_store_record_t base;
    config_store_default(&base);
    TEST_CHECK(config_store_write_volatile(&base, NULL) == true, "fixture: unarmed baseline install");
    config_store_flash_host_stub_set_relay_state(heat_possible ? RELAY_OWNER_STATE_ARMED : RELAY_OWNER_STATE_GRACE);
    config_store_record_t next;
    config_store_get_full_record(&next); // the running record, as installed
    if (mode == 0) {
        ((unsigned char *)&next)[byte_off] ^= 0x01u;
    }
    return config_store_write_volatile(&next, NULL);
}

static void test_volatile_gate_is_fail_closed_per_field(void)
{
    TEST_SECTION("config_store_flash: heat possible refuses a change to every non-allowlisted field "
                 "(HIGH-1), allowlisted fields pass");
    for (size_t i = 0; i < sizeof k_f1_fields / sizeof k_f1_fields[0]; i++) {
        const f1_field_t *f = &k_f1_fields[i];
        if (f->kind == K_SKIP) {
            continue;
        }
        const size_t offs[2] = { f->off, f->off + f->size - 1u };
        for (int which = 0; which < 2; which++) {
            if (which == 1 && f->size == 1u) {
                continue;
            }
            bool ok = f1_flip_and_try(offs[which], true, 0);
            printf("  %-24s byte %u -> %s\n", f->name, (unsigned)(offs[which] - f->off), ok ? "ACCEPTED" : "refused");
            if (f->kind == K_SAFETY) {
                TEST_CHECK(ok == false, "a change to this trip-relevant field is refused while heat is possible");
            } else {
                TEST_CHECK(ok == true, "a change to this allowlisted non-safety field is accepted");
            }
        }
    }
}

static void test_volatile_gate_identical_resend_and_idle_accept(void)
{
    TEST_SECTION("config_store_flash: identical resend accepted; idle installs of any field accepted (MED-1)");
    TEST_CHECK(f1_flip_and_try(0, true, 1) == true, "heat possible: identical resend is not a change");
    for (size_t i = 0; i < sizeof k_f1_fields / sizeof k_f1_fields[0]; i++) {
        const f1_field_t *f = &k_f1_fields[i];
        if (f->kind == K_SKIP) {
            continue;
        }
        TEST_CHECK(f1_flip_and_try(f->off, false, 0) == true,
                   "idle and de-energized: a change to any field installs (GRACE state, no heat possible)");
    }
}

static bool s_probe_heat = true;
static bool probe_fn(void) { return s_probe_heat; }

static void test_volatile_gate_follows_heat_probe_not_armed(void)
{
    TEST_SECTION("config_store_flash: gate follows the registered heat-possible probe, not relay ARMED (MED-1)");
    reset_all();
    config_store_boot_load();
    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);
    config_store_record_t base;
    config_store_default(&base);
    config_store_set_heat_possible_probe(probe_fn);

    config_store_record_t next = base;
    next.link_dead_hard_s += 5u; // trip-relevant
    s_probe_heat = false;
    TEST_CHECK(config_store_write_volatile(&next, NULL) == true,
               "ARMED but idle/de-energized per the probe: a trip-field change installs");
    config_store_record_t more = next;
    more.link_timeout_s += 1u;
    s_probe_heat = true;
    TEST_CHECK(config_store_write_volatile(&more, NULL) == false,
               "ARMED and heat possible per the probe: a trip-field change is refused");
    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_GRACE);
    TEST_CHECK(config_store_write_volatile(&more, NULL) == false,
               "probe says heat possible: refused regardless of relay state (probe is authoritative)");
    config_store_set_heat_possible_probe(NULL);
}

// INFO: a stale legacy ct_topology byte in the pushed record must not be
// refused spuriously -- the backfill runs before the comparison.
static void test_volatile_gate_backfills_before_compare(void)
{
    TEST_SECTION("config_store_flash: legacy ct_topology backfill runs before the heat-possible comparison (INFO)");
    reset_all();
    config_store_boot_load();
    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_GRACE);
    config_store_record_t base;
    config_store_default(&base);
    base.fields_set |= CONFIG_STORE_SET_ZONE_CT_CHANNEL;
    base.zone_ct_channel[0] = 2u;
    base.zone_ct_channel[1] = 2u;
    base.zone_ct_channel[2] = 2u;
    base.ct_topology = CONFIG_STORE_CT_TOPOLOGY_PER_ZONE; // stale: zone map says summed
    TEST_CHECK(config_store_write_volatile(&base, NULL) == true, "fixture: install (backfills ct_topology)");
    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);
    config_store_record_t resend = base; // still carries the stale legacy byte
    TEST_CHECK(config_store_write_volatile(&resend, NULL) == true,
               "heat possible: resend with a stale legacy ct_topology is not refused");
}

// LOW-3: fields_set differences alone (value bytes identical) are a change
// to what the guards treat as commissioned. Heat possible refuses both
// directions; the allowlisted mains/power bits stay accepted.
static void test_volatile_gate_refuses_fields_set_only_changes(void)
{
    TEST_SECTION("config_store_flash: heat possible refuses a fields_set-only change, either direction (LOW-3)");
    const uint32_t bits[] = {
        CONFIG_STORE_SET_TC_SOURCE, CONFIG_STORE_SET_BORROWED_ZONE_INDEX, CONFIG_STORE_SET_TC_PLACEMENT_MODE,
        CONFIG_STORE_SET_CT_CHANNEL_MAP, CONFIG_STORE_SET_CT_CHANNEL_MAP_0, CONFIG_STORE_SET_CT_CHANNEL_MAP_1,
        CONFIG_STORE_SET_CT_CHANNEL_MAP_2, CONFIG_STORE_SET_I_NORMAL_A_0, CONFIG_STORE_SET_I_NORMAL_A_1,
        CONFIG_STORE_SET_I_NORMAL_A_2, CONFIG_STORE_SET_CT_INSTALLED, CONFIG_STORE_SET_ZONE_CT_CHANNEL_0,
        CONFIG_STORE_SET_ZONE_CT_CHANNEL_1, CONFIG_STORE_SET_ZONE_CT_CHANNEL_2,
    };
    for (size_t i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        for (int dir = 0; dir < 2; dir++) {
            reset_all();
            config_store_boot_load();
            config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_GRACE);
            config_store_record_t base;
            config_store_default(&base);
            if (dir == 0) {
                base.fields_set &= ~bits[i];
            } else {
                base.fields_set |= bits[i];
            }
            TEST_CHECK(config_store_write_volatile(&base, NULL) == true, "fixture: idle install");
            config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);
            config_store_record_t next;
            config_store_get_full_record(&next);
            next.fields_set ^= bits[i];
            printf("  fields_set bit 0x%08X dir %d\n", (unsigned)bits[i], dir);
            TEST_CHECK(config_store_write_volatile(&next, NULL) == false,
                       "a fields_set-only change is refused while heat is possible");
        }
    }
    reset_all();
    config_store_boot_load();
    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);
    config_store_record_t cur;
    config_store_get_full_record(&cur);
    cur.fields_set ^= CONFIG_STORE_SET_MAINS_VOLTAGE_V;
    TEST_CHECK(config_store_write_volatile(&cur, NULL) == true, "the mains_voltage set-bit is allowlisted");
}
int main(void)
{
    test_boot_load_blank_sector_is_default();
    test_write_then_reload_round_trips();
    test_fallback_seeded_at_boot_before_any_write();
    test_write_refused_while_armed();
    test_write_volatile_installs_while_armed_and_bumps_identity();
    test_write_volatile_repeated_then_flash_commit_still_gated();
    test_write_uses_persisted_record_not_ram_after_volatile_install();
    test_volatile_dirty_flag_and_persisted_version();
    test_mixed_armed_refusal_wire_reason_uses_persisted_not_cached_tc_type();
    test_write_ex_ct_cal_only_armed_exemption();
    test_write_volatile_refuses_loosening_while_armed();
    test_seq_increments_and_survives_wraparound();
    test_safe_execute_timeout_is_reported_and_leaves_cache_unchanged();
    test_program_failure_is_not_masked_by_safe_execute_ok();
    test_switch_to_sector_b_after_sector_a_fills();
    test_wear_is_spread_across_both_sectors();
    test_power_loss_mid_erase_of_target_leaves_old_sector_valid();
    test_power_loss_mid_program_of_target_leaves_old_sector_valid();
    test_power_loss_between_erase_and_program_of_target_leaves_old_sector_valid();
    test_torn_inline_slot_is_never_reprogrammed();
    test_torn_first_slot_before_any_valid_record_is_never_reprogrammed();
    test_migration_from_legacy_single_sector_layout();
    test_corrupt_active_sector_falls_back_to_other_sector();
    test_seqlock_concurrent_read_never_tears();
    test_seqlock_multi_reader_never_tears();
    test_fallback_aba_two_commits_during_one_copy();
    test_ram_integrity_repair_restores_persisted_not_defaults();
    test_ram_integrity_second_corruption_this_boot_requests_trip();
    test_ram_integrity_repair_discards_live_volatile_install();
    test_ram_integrity_both_copies_corrupted_requests_trip_no_repair();
    test_ram_integrity_persisted_only_corruption_with_healthy_cache_is_a_no_op();
    test_identical_write_skips_flash();
    test_identical_write_after_volatile_clears_dirty();
    test_identical_write_still_refused_while_armed();
    test_volatile_gate_classifies_every_record_byte();
    test_volatile_gate_is_fail_closed_per_field();
    test_volatile_gate_identical_resend_and_idle_accept();
    test_volatile_gate_follows_heat_probe_not_armed();
    test_volatile_gate_backfills_before_compare();
    test_volatile_gate_refuses_fields_set_only_changes();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
