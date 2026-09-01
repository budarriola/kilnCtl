#include "zones_http.h"
#include "zones_config_json.h" /* the HTTP-free core -- zone_cfg_t/zones_cfg_t/
                                * zone_timing_profile_t, the versioned-blob decode/
                                * validate/CRC logic, and the field parsers this file
                                * used to define locally now live there; see that
                                * file's own header comment for the split rationale. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "MAX31856.h"
#include "autotune_engine.h"
#include "http_form.h"
#include "kiln_io_owner.h" /* B1 fix: the sweep is a relay writer too -- see zone_sweep_task()'s
                             * doc comment and kiln_io_owner.h's own top comment for why every
                             * relay write in this firmware goes through this module, never
                             * kiln_io_set_relay_mask()/kiln_io_all_relays_off() directly. */
#include "ota_http.h" /* ota_http_check_interlocks() -- the shared "not while firing" gate */
#include "ota_interlock.h" /* OTA_INTERLOCK_REASON_MAX / ota_interlock_result_t for the OTA-start gate below --
                             * NOT for a temperature ceiling any more, see H2's comment on
                             * zone_sweep_effective_ceiling_c() */
#include "kiln_io.h"
#include "profile_executor.h"
#include "safety_trip_words.h" /* safety_fault_source_words() -- decode the safety fault-source
                                 * mask instead of showing the operator a bare hex value
                                 * (ROADMAP.md M13). */
#include "relay_authority.h" /* the single shared heat claim -- see its doc comment
                               * above relay_heat_zone_claimant_t. Closes the race
                               * this file's own s_sweep.active check alone cannot:
                               * see zones_current_sweep_start()'s atomic gate. */
#include "safety_cfg_store.h" /* the ESP-side cache of the Pico's commissioning record --
                                * zone_sweep_confirm_ct_map_landed() forces a LIVE re-fetch
                                * through it, the same proof safety_cfg_http.c's
                                * confirm_commit_landed() demands before calling a commit a
                                * success. */
#include "thermo_combine.h"
#include "thermo_owner.h"
#include "uart_task_ids.h" /* SAFETY_FLAG_* for zones_get_safety_wiring() */
#include "web_encoding.h"
#include "wifi_provision_http.h"
#include "zone_settings_source_chain.h" /* the shared settings_source chain-walk -- see that
                                          * header's own comment for why it now lives outside
                                          * this file: test_backup_import.c's stub for
                                          * zones_config_settings_source_import_has_cycle()
                                          * calls the identical algorithm from here instead of
                                          * a hand-maintained re-implementation. */

static const char *TAG = "zones_http";

#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_ZONES "zones_cfg"

/* kiln_nvs is the 2026-08-13 split target for zones/rules/relay_cycles/
 * run_state (see partitions.csv and TODO.md 8.1); each module manages its own
 * migration and partition init independently rather than assuming another
 * module already brought the partition up. NVS_DEFAULT_PART_NAME (from
 * nvs_flash.h, expands to "nvs") is the old, still-live home this module's
 * data used to persist to, kept readable for the one-time migration below and
 * for firmware rollback. */
#define KILN_NVS_PARTITION "kiln_nvs"

/* Bump whenever zones_cfg_t's on-flash layout changes; see nvs_load_from().
 * 3 -> 4 (2026-08-17, TODO.md 10.8): added zone_cfg_t::thermo_mask. Same
 * "grows the struct, does not shrink it" case nvs_load_from()'s BUG FIXED
 * 2026-08-13 comment documents -- a v3 blob is read into a zeroed out_cfg,
 * so thermo_mask already reads as 0 with no explicit copy needed, and only
 * the version bump plus migrate_zones_cfg_v1_to_current()'s new
 * legacy-mapping fill-in (see there) are required to make the growth safe
 * for an operator's existing saved zones.
 *
 * 4 -> 5 (2026-08-21): added zone_cfg_t::tc_type (per-channel MAX31856 CR1
 * TC[3:0] thermocouple type -- the owner's actual complaint: the settings
 * page had no way to select B/E/J/K/N/R/S/T, and MAX31856.c hardcoded
 * THERMO_TC_K at boot with no persistence at all) and
 * zones_cfg_t::safety_tc_type (the RP2040 safety processor's OWN,
 * independent thermocouple type -- see safety_link.c's mirroring comment for
 * why this is a separate field rather than reusing any one zone's tc_type).
 * Both are a real hazard to leave at their zeroed default (0 decodes as Type
 * B, not "unconfigured" -- unlike thermo_mask's 0, there is no safe implicit
 * meaning for a zeroed tc_type), so THIS growth needs the same explicit
 * migration treatment thermo_mask got at 3->4, not the "0 already means the
 * right thing" case continue_on_zone_trip got at 1->2. See
 * migrate_zones_cfg_v1_to_current() below for the fill-in.
 *
 * 5 -> 6 (2026-08-27): added zone_cfg_t::ct_mask -- which of SaftyFW's
 * ZONE_CT_CHANNEL_COUNT current-sense channels this zone's readout on the
 * Thermocouples & Zones page should show. Purely informational (nothing in
 * profile_executor.c/thermal_guard.c reads it; the safety processor's own
 * guards already watch every CT channel regardless of any zone mapping), so
 * -- unlike thermo_mask's 3->4 growth -- a zeroed ct_mask ("no probe mapped
 * yet") is already the correct, safe meaning for a migrated v5 blob. No
 * explicit fill-in needed in migrate_zones_cfg_v1_to_current(), same "0
 * already means the right thing" case continue_on_zone_trip's 1->2 bump was.
 * Deliberately a bitMASK, not a single channel index: the owner's own spec
 * is "current sense probes may be reused across more than one zone", so no
 * exclusivity is enforced here, same as thermo_mask already allows a channel
 * to feed more than one zone.
 *
 * 6 -> 7 (2026-08-27, owner-report "saved securely like the others"): added
 * zones_cfg_t::crc32, and replaced the old load path's "memcpy the old blob
 * over the new struct, patch in defaults, hope the tail-only-growth
 * assumption still holds" migration with explicit per-version historical
 * struct layouts (see zone_cfg_v1_t/zone_cfg_v3_t/zone_cfg_v4_t/
 * zone_cfg_v5_t and convert_versioned_blob_to_current() below) plus a
 * per-version expected-length table (expected_len_for_version()) checked
 * BEFORE anything is copied or interpreted. That old assumption was false:
 * zone_cfg_t is an ARRAY ELEMENT, and three of the last four bumps before
 * this one (2->3, 3->4, 4->5) grew zone_cfg_t itself, not just the top-level
 * zones_cfg_t -- growing an array element displaces every element after the
 * first, so a v3/v4/v5 blob loaded the old way read zones[1] and zones[2]
 * (relay_mask, max_temp_c, every guard threshold) from the wrong byte
 * offsets, as arbitrary floats, and then got flagged VALID. Confirmed by
 * walking this file's own git history commit-by-commit (b9ecc52 = v1,
 * c17e80f = 1->2, 915f0fe = 2->3, 0b1674e = 3->4, 638ea87 = 4->5, this file's
 * own prior state = v6) to recover the EXACT historical field layout at each
 * version -- not guessed. zones_config_json_validate() (previously only run on the
 * snapshot-restore path, zones_config_import_blob()) now runs on every
 * decoded blob, old or current, via the shared zones_config_json_decode_blob() helper.
 * A CRC32 (esp_crc32_le(), already used by crash_report.c -- see that file's
 * compute_crc()/seal_crc() for the identical "compute over a zeroed-crc-field
 * copy" convention this follows) covers the current-version blob only: older
 * versions never had a crc32 field to check, so their integrity gate is the
 * length-must-match-the-claimed-version check plus zones_config_json_validate().
 *
 * 8 -> 9 (2026-08-27, same day, owner-report follow-up: "make it so that i
 * can have diffrent Safety timings and assign the zones to them ... that way
 * i dont need to copy the data multipal times"): the nine v8 per-zone
 * overrides (guard_progress_duty_min .. ramp_lock_band_c, see zone_cfg_t's own
 * 7->8 comment) move OFF zone_cfg_t entirely and onto a new
 * zones_cfg_t::timing_profiles[] array of zone_timing_profile_t -- named,
 * reusable bundles of those same nine fields. zone_cfg_t loses the nine
 * floats and gains one uint8_t, timing_profile, an index into
 * timing_profiles[]; zones_cfg_t gains timing_profile_count (how many of
 * timing_profiles[]'s MAX31856_CHANNEL_COUNT slots are in use) and the array
 * itself. Configuring three zones identically used to mean typing the same
 * nine numbers three times and keeping them in sync by hand; now it means
 * creating one profile and pointing three zones at it.
 *
 * zone_cfg_t is an ARRAY ELEMENT (see the 6->7 comment above for why that
 * matters): removing nine fields from the middle of every element and adding
 * one back is exactly the kind of change that makes an old blob's later
 * zones read from the wrong byte offsets if it is ever memcpy'd instead of
 * field-by-field converted -- so this bump gets the same typed-snapshot
 * treatment every prior one has: zone_cfg_v8_t/zones_cfg_v8_t below freeze the
 * pre-this-pass layout exactly, expected_len_for_version(8) returns
 * sizeof(zones_cfg_v8_t) -- the OLD, frozen struct, NEVER sizeof(zone_cfg_t)/
 * sizeof(zones_cfg_t), which as of this pass mean the NEW v9 layout. (A
 * sibling fix in profiles_http.c once returned sizeof() of the CURRENT struct
 * for an OLD version and rejected every profile already on the owner's board,
 * caught only by a hardware flash -- expected_len_for_version()'s whole design
 * exists to make that class of mistake impossible; case 8 below is written to
 * the same discipline as every case before it.) convert_versioned_blob_to_
 * current()'s new case 8 does the real migration work.
 *
 * MIGRATION MUST BE LOSSLESS: a v8 board's zones may each carry distinct,
 * separately-tuned nine-tuples. Collapsing them all onto profile 0 would
 * silently discard whichever zones' settings didn't happen to go first. The
 * migration instead deduplicates: for each zone, if an already-allocated
 * profile's nine values are all identical to this zone's, point the zone at
 * that profile; otherwise allocate a new one from this zone's own values.
 * timing_profiles[] is sized MAX31856_CHANNEL_COUNT (== the zone count), so
 * the worst case -- every zone's nine values distinct -- always fits with room
 * for one profile per zone, and nothing is ever dropped. A board where every
 * zone is still at the all-zero v8 default (true of every board that has
 * never touched the safety-timings page, including the owner's today)
 * collapses to exactly ONE shared profile named "Default" -- see
 * test_nvs_load_from_v8_blob_upconverts_to_shared_default_profile() in
 * test_zones_http.c, and test_nvs_load_from_v8_blob_with_distinct_zone_
 * values_migrates_losslessly() for the non-degenerate case.
 *
 * Versions 1-7 predate the nine fields entirely (they never had per-zone
 * timing overrides to migrate), so their conversion paths instead synthesize
 * a single all-zero "Default" profile and point every zone at it -- 0 in
 * every one of the nine fields is already each field's own "use the firmware
 * default" meaning, so an upgrading pre-v8 board behaves exactly as it did
 * before, same as the 7->8 bump's own upgrade guarantee.
 *
 * 9 -> 10 (2026-08-30, PID_EXPANSION_PLAN.md Phase 2): four new zone_cfg_t
 * fields for the fuzzy-PID control mode (ZONE_CONTROL_MODE_PID_FUZZY) and
 * the section 3.5 "same settings as zone N" UI convenience:
 *   - fuzzy_strength_pct (float, 0-100): section 3.3's "Adjustment strength"
 *     knob. 0 = no fuzzy adjustment, already the safe "behaves like classic
 *     PID" default.
 *   - coupling_coeff (float) + coupling_neighbor_zone (float, holds a zone
 *     index -- kept float, not uint8_t, for the same parse/JSON round-trip
 *     consistency zone_cfg_t's own top-of-struct comment documents for
 *     heater_window_ms): section 2c's measured cross-zone feedforward
 *     coefficient c_ij and which zone it was measured against. 0 coefficient
 *     = "no coupling measured", both the safe default and the
 *     degrade-to-today behavior.
 *   - settings_source (uint8_t): section 3.5's UI-only provenance marker --
 *     ZONE_SETTINGS_SOURCE_CUSTOM (0xFF) means "this zone's own settings",
 *     otherwise the index of the zone this one's dropdown claims to copy.
 *     Stored ONLY so the settings page can re-open showing the right
 *     dropdown state; the resolved values are written into each zone's own
 *     fields on save (a later, Phase 5 pass), so nothing in the control loop
 *     ever reads this field.
 *
 * All four are appended at zone_cfg_t's tail, after timing_profile -- the one
 * safe place to grow this struct without repeating the 4->5 tc_type mistake
 * (see ZONES_CFG_VERSION's 6->7 comment). Added at the tail of the uint8_t
 * group for settings_source (matching relay_mask/control_mode/tc_type/
 * thermo_mask/ct_mask/timing_profile's own grouping) and at the tail of the
 * float group for the other three, same discipline as model_k_dc/model_tau_s/
 * model_dead_time_s's own placement.
 *
 * MIGRATION MUST BE LOSSLESS AND, FOR settings_source, MUST NOT DEFAULT TO 0:
 * a v9 blob's zones carry no fuzzy/coupling/settings_source data at all, so
 * every zone's three new floats become 0 -- already each field's documented
 * "not configured" meaning, no different from any prior version bump. But
 * settings_source is NOT the same case: 0 is a REAL, DIFFERENT value here
 * ("copies zone 0's settings"), not a safe empty default the way 0 already is
 * for thermo_mask or the guard thresholds. Every migrated zone's
 * settings_source is explicitly set to ZONE_SETTINGS_SOURCE_CUSTOM (0xFF),
 * never left at the zero convert_zone_v9() (below) starts each destination
 * zone at -- see convert_zone_v9()'s own comment for where that explicit
 * assignment happens. Getting this wrong would mean every zone on an
 * upgrading board silently starts claiming to copy zone 0, and the first
 * ordinary settings-page save after Phase 5 lands would overwrite zones 1 and
 * 2's commissioned numbers with zone 0's -- exactly the class of "botched
 * migration destroys a commissioned kiln config" this bump's own task
 * description was written to prevent.
 *
 * ZONES_CONFIG_BLOB_MAX_SIZE widened 512 -> 640 (zones_http.h) to fit the
 * growth -- see that macro's own comment. The CRC32 already covers the whole
 * struct (zones_config_json_compute_crc() hashes sizeof(zones_cfg_t) with crc32 zeroed),
 * so no separate change was needed there: the new fields are inside the
 * struct it hashes, same as every prior version's growth.
 *
 * 10 -> 11 (2026-08-30, same-day follow-up): bench measurement of the real
 * 3-zone coupling matrix proved the v10 single (coupling_coeff,
 * coupling_neighbor_zone) pair cannot represent it -- coupling is BOTH
 * asymmetric (1->0 measured 1.86x stronger than 0->1) and multi-neighbor
 * (interior zone 1 has two very different cross-gains, one per peer). A
 * single pair keeps exactly one neighbor and silently drops the rest --
 * a third of a real 3-zone matrix, worse with more channels. See
 * zone_cfg_t::coupling_coeff's own doc comment above for the full replacement
 * shape and unit convention.
 *
 * MIGRATION MUST BE LOSSLESS: a v10 blob's single pair maps onto exactly one
 * cell of the new row -- coupling_coeff[old_neighbor] = old_coeff, every
 * other cell (including the diagonal) 0, which is already each cell's
 * documented "not measured" default. convert_zone_v10() does this. No
 * information a v10 board could have stored is lost; there was only ever
 * room for one neighbor before, and that one neighbor's value lands in
 * exactly the right cell of the new row.
 *
 * zone_cfg_t grows by (MAX31856_CHANNEL_COUNT - 2) floats vs v10 = 1 float
 * per zone (3 channels: 3 - 2 = 1) = 4 bytes/zone, 12 bytes total across the
 * three zones -- see the _Static_assert byte math on zone_cfg_v10_t and the
 * live zone_cfg_t below. Comfortably inside ZONES_CONFIG_BLOB_MAX_SIZE's
 * (640) existing headroom AT TODAY'S MAX31856_CHANNEL_COUNT == 3 ONLY --
 * coupling_coeff is a full N x N row per zone, so zones_cfg_t is O(N^2) in
 * channel count, not the flat "12 bytes total" this paragraph's arithmetic
 * describes. At N=5 the struct computes to ~700 bytes, already past the 640
 * ceiling; the existing _Static_assert on zone_cfg_t/zones_cfg_t catches
 * that at compile time (a build simply fails to fit, not a runtime
 * overflow), but do not read this comment as "there is headroom to grow
 * MAX31856_CHANNEL_COUNT" -- there is not, without also widening
 * ZONES_CONFIG_BLOB_MAX_SIZE. That macro is NOT touched by this pass (it is
 * shared with kiln_cfg_store_blob_t, owned elsewhere -- see this file's own
 * task brief). */

/* ZONE_CT_CHANNEL_COUNT moved to zones_http.h (2026-08-27, same day it was
 * added) -- backup_http.c's import validation needs it too, for the exact
 * same "ct_mask may only reference a channel that exists" bound this file's
 * own parse_zone_fields()/setter/import-validator enforce, and a second,
 * independently-defined 3u in backup_http.c would be exactly the kind of
 * two-copies-that-can-drift this codebase's own conventions warn against
 * elsewhere (see e.g. relay_authority.c's header comment). */

/* MAX31856 CR1.TC[3:0] nibble values 0x00-0x07 name a real thermocouple type
 * (B/E/J/K/N/R/S/T, uart_task_ids.h's THERMO_TC_* -- THERMO_TC_B is 0, the
 * lowest, THERMO_TC_T is 7, the highest of the eight); 0x08-0x0F are the
 * part's voltage-input modes (THERMO_TC_VMODE_G8/G32 and reserved codes in
 * between), not thermocouples at all. uart_bridge.c's raw CONFIG_CHANNEL
 * subcommand and MAX31856_configure() itself both accept the full 0-0x0F
 * range deliberately (a debug/raw path has legitimate reasons to want the
 * voltage-input modes) -- this page's operator-facing selector does not, and
 * TODO.md's owner-report task item 4 asks explicitly for the tighter check:
 * an operator picking "thermocouple type" off a labelled dropdown can never
 * mean a voltage-input mode, so this endpoint rejects anything past
 * THERMO_TC_T with a specific reason rather than silently accepting a
 * register code that isn't a thermocouple. */

/* Sanity bounds for the stored FOPDT plant model, and every other per-zone
 * field bound below -- ZONE_MODEL_K_MAX/ZONE_MODEL_TIME_MAX_S moved to
 * zones_http.h 2026-08-21, and ZONE_NAME_MAX_LEN plus the rest of this
 * file's per-field bounds moved there in the same pass right after, so
 * backup_http.c's import validation pass and the new zones_config_set_*()
 * setters can reject an out-of-range value BEFORE any write happens using
 * the EXACT same ceiling parse_zone_fields() below enforces -- see
 * zones_http.h for the full rationale. */

/* Embedded via EMBED_TXTFILES, pre-gzipped at configure time by
 * App/drivers/CMakeLists.txt -- same convention as every other *_page.html
 * in this component (see web_encoding.h's header comment for the flash-
 * budget reasoning that makes gzip the only stored representation). */
extern const uint8_t zones_page_html_gz_start[] asm("_binary_zones_page_html_gz_start");
extern const uint8_t zones_page_html_gz_end[] asm("_binary_zones_page_html_gz_end");
/* The safety-timings page (/settings/safety). Served from this file rather
 * than a module of its own because it edits the same zones_cfg_t record
 * through the same /api/zones endpoint -- see the page's own header comment
 * on why the two pages have to echo each other's fields back. */
extern const uint8_t safety_config_page_html_gz_start[] asm("_binary_safety_config_page_html_gz_start");
extern const uint8_t safety_config_page_html_gz_end[] asm("_binary_safety_config_page_html_gz_end");

/* One zone per configured thermocouple channel -- see zones_http.h. Bounded
 * by the hardware, not by anything a client can grow. */
/* Field order below is deliberately NOT declaration order/POST-form order:
 * every uint8_t field (relay_mask, control_mode, tc_type, thermo_mask,
 * ct_mask, timing_profile) is grouped at the STRUCT TAIL, after every float,
 * rather than interleaved among the floats the way earlier versions of this
 * struct had them. A float requires 4-byte alignment; a lone uint8_t sitting
 * between two floats forces the compiler to insert 3 bytes of padding on
 * either side of it that no field ever uses. Interleaved, this struct's six
 * single-byte fields cost 3 separate padding gaps (~9 wasted bytes) EACH; hard
 * numbers, per zone: 124 bytes interleaved vs 116 bytes grouped, and with
 * MAX31856_CHANNEL_COUNT zones plus the new timing_profiles[] array below,
 * that 8-byte-per-zone difference is what keeps zones_cfg_t under
 * ZONES_CONFIG_BLOB_MAX_SIZE (640, see zones_http.h) instead of past it --
 * confirmed by _Static_assert(sizeof(zones_cfg_t) <= ...) below, which does
 * not compile if this ever regresses. Purely a memory-layout optimization:
 * every field is still addressed by name everywhere in this file (parse_zone_
 * fields(), zones_config_json_validate(), every convert_zone_v*(), the JSON GET/POST
 * paths), so this reordering changes NOTHING about behavior, only the
 * in-memory (and on-flash) byte offsets -- which is exactly why it's safe to
 * do on the CURRENT struct even though the historical zone_cfg_v*_t snapshots
 * below deliberately keep their ORIGINAL field order (they must, byte-for-byte,
 * to correctly interpret an old blob). */

/* application/x-www-form-urlencoded whole-page submit: thermo_count,
 * relay_count, and 27 fields per zone (name/relay_mask/thermo_mask/cal/kp/
 * ki/kd/ramp/sanity/mode/maxtemp/mintemp/window/minon/minoff/xzone/k/tau/
 * deadtime plus the 8 guard-threshold overrides below) across up to
 * MAX31856_CHANNEL_COUNT zones. Generous headroom over what a legitimate
 * 3-zone submission needs -- checked against Content-Length before a single
 * byte is read, same discipline as every other handler in this codebase.
 * Bumped 2048->2560 when heater_window_ms/min_on_ms/min_off_ms were added,
 * 2560->2816 when cross_zone_max_delta_c was, 2816->3200 when the three
 * plant-model fields were, 3200->4096 when the 8 guard-threshold overrides
 * were: worst case those add 8 "z0_<name>=" keys plus separators and up to
 * zones_config_json_parse_float_field()'s 23-char value each, ~250 bytes a zone, ~750 across
 * three. z%u_thermo_mask (TODO.md 10.8) is a single 0-255 u8 field, well
 * under 20 bytes a zone even with its key name -- left inside the existing
 * 4096 without another bump; the three-zone worst case is nowhere near it.
 * z%u_tctype (2026-08-21) and the top-level safety_tc_type are each a
 * single 0-7 u8 field, smaller still -- also left inside the existing 4096.
 * z%u_ct_mask (2026-08-27) is the same shape as z%u_thermo_mask, similarly
 * left inside 4096. */
#define ZONES_BODY_MAX 4096

static struct {
    zones_cfg_t cfg;
} s_zones;

/* TODO.md 8.2 "Tie it to the guards, not only the UI": explicit "this
 * zone config is trustworthy" flag, distinct from s_zones.cfg simply reading
 * as all-zero. Before this flag existed, an unconfigured zone was refused
 * only by accident -- relay_mask happened to be 0 whether that was a
 * genuinely empty config OR a version-refused/wipe/first-boot load failure,
 * so apply_relay()/begin_run_locked() silently did nothing in either case
 * with no way for a caller (or the dashboard) to tell "nothing configured
 * yet" from "config failed to load, do not trust this."
 *
 * false whenever nvs_load_from() hit the wipe path (short/wrong-size blob),
 * the newer-refuses-to-load path, or the namespace was simply never created
 * (first boot, nothing saved yet) -- all three are "cannot vouch for this
 * config" for the same reason: what's live in s_zones.cfg is the
 * zero-initialized default, not something read off flash. true only after a
 * load that actually decoded a real blob (current version, or an older
 * version successfully migrated) or a POST that validated and committed a
 * fresh config to s_zones.cfg -- see nvs_load()/zones_http_start() and
 * zones_post_handler(). Whole-partition granularity, matching the loader:
 * this struct's load is all-or-nothing, so there is no meaningful
 * per-zone version of this flag. */
static bool s_zones_config_valid = false;

/* TODO.md 6A.7 config-reload counter. Kept outside s_zones deliberately:
 * s_zones.cfg is what nvs_save() blobs out verbatim, and this must never
 * become part of the persisted layout (a saved generation would be
 * meaningless across a reboot, and widening the blob would invalidate every
 * stored config -- see nvs_load's size check). Starts at 1 so a consumer
 * whose cached generation is zero-initialized always sees a difference on
 * its first comparison; see zones_config_generation()'s header comment.
 * A plain uint32_t needs no locking here: a 32-bit aligned load/store is
 * atomic on this core, and a reader racing a writer sees either the old or
 * the new value -- both correct answers to "has it changed since I last
 * looked," since a missed edit is simply picked up on the next tick. */
static uint32_t s_config_generation = 1;

/* ---- Hardware access for Tasks 1/2/3 (2026-08-27+2) ----------------------
 * zones_http_start() itself takes no hardware pointers (pure config CRUD --
 * see its own comment); the current sweep, the CT-mapping check, and the
 * safety-wiring readout all need real hardware, so main.c hands it over
 * separately, once, via zones_http_set_hw() below. NULL-tolerant, same
 * convention as every other *_start()'s io/thermo_bus/safety triple in this
 * codebase -- see zones_http.h's doc comment on this function. */
static kiln_io_t *s_hw_io = NULL;
static MAX31856BusClass *s_hw_thermo_bus = NULL;
static SafetyLinkClass *s_hw_safety = NULL;

void zones_http_set_hw(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                       SafetyLinkClass *safety_or_null)
{
    s_hw_io = io_or_null;
    s_hw_thermo_bus = thermo_bus_or_null;
    s_hw_safety = safety_or_null;
}

/* ---- NVS ---------------------------------------------------------------- */

static esp_err_t nvs_save(void);



/* Brings up one NVS partition, erasing ONLY that partition if its contents
 * are unusable. Copied/adapted from wifi_prov.c's nvs_partition_init() (see
 * that file for the full rationale) -- NO_FREE_PAGES / NEW_VERSION_FOUND have
 * no other cure, so erasing is the only way forward, but the erase must stay
 * scoped to the partition that is actually broken rather than blast-radius
 * the rest of kiln_nvs (or, worse, the default partition) with it. */
static esp_err_t nvs_partition_init(const char *partition)
{
    esp_err_t err = nvs_flash_init_partition(partition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition '%s' needs erase (%s) -- erasing THAT PARTITION ONLY and retrying",
                 partition, esp_err_to_name(err));
        err = nvs_flash_erase_partition(partition);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(partition);
        }
    }
    return err;
}

/* Reads NVS_NAMESPACE/NVS_KEY_ZONES out of `partition` into *out_cfg, applying
 * the three-outcome version handling nvs_load() relies on, via the shared
 * zones_config_json_decode_blob() decoder (length check first, typed per-version
 * conversion, zones_config_json_validate(), CRC on the current-version path). *out_found
 * reports whether the key held something WORTH NOT DISTURBING -- true for a
 * decoded-and-trustworthy blob (current or migrated-older) AND for a blob
 * refused as newer-than-firmware (a real config this build must not clobber,
 * even though it can't use it), false for genuine corruption (too short,
 * wrong length for its claimed version, bad CRC, or failed validation) where
 * there is nothing being protected and a caller is free to look elsewhere.
 * This is what the one-time migration below keys off. */
/* out_valid, if non-NULL, reports whether *out_cfg is a real decoded config
 * that later stages (zones_http_start(), zones_config_is_valid()) may treat
 * as trustworthy -- see s_zones_config_valid's comment for the exact rule.
 * Distinct from *out_found: found means "there is real data here that must
 * not be overwritten by a migration," valid means "and it's actually usable
 * to run a kiln against right now." A newer-refuses-to-load blob is found
 * but not valid; a migrated older blob is both; genuine corruption is
 * neither. */
static esp_err_t nvs_load_from(const char *partition, zones_cfg_t *out_cfg, bool *out_found, bool *out_valid)
{
    if (out_found) {
        *out_found = false;
    }
    if (out_valid) {
        *out_valid = false;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(partition, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* namespace never created -- nothing configured, not an error */
    }
    if (err != ESP_OK) {
        return err;
    }

    /* Raw byte buffer, not out_cfg directly: zones_config_json_decode_blob() needs the
     * blob's ACTUAL on-disk bytes to run the length-vs-claimed-version check
     * and, for an older version, to reinterpret them through the right
     * historical struct -- not bytes already reshaped by a direct
     * nvs_get_blob() into the CURRENT struct's layout (that reshaping is
     * exactly the bug this pass fixes). Sized to the largest possible
     * on-flash layout, which by construction is the current one (every
     * historical struct above is smaller). */
    uint8_t raw[sizeof(zones_cfg_t)];
    memset(raw, 0, sizeof(raw));
    size_t len = sizeof(raw);
    err = nvs_get_blob(h, NVS_KEY_ZONES, raw, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "zones_cfg blob read from '%s' failed (%s) -- treating as unreadable",
                 partition, esp_err_to_name(err));
        return ESP_OK;
    }

    const char *reason = "";
    zones_decode_result_t result = zones_config_json_decode_blob(raw, len, out_cfg, &reason);
    switch (result) {
    case ZONES_DECODE_OK:
        /* LOAD path only -- collapse, never reject, a pre-existing stored
         * cycle. See zones_config_json_normalize_settings_source_cycles()'s own comment for
         * why this runs here and NOT inside zones_config_json_decode_blob() (which
         * zones_config_import_blob() also calls, and that path must reject
         * instead). */
        zones_config_json_normalize_settings_source_cycles(out_cfg, partition);
        if (out_found) {
            *out_found = true;
        }
        if (out_valid) {
            *out_valid = true;
        }
        ESP_LOGI(TAG, "zones_cfg from '%s' loaded (on-disk version %u) as v%u", partition,
                 (unsigned)raw[0], (unsigned)ZONES_CFG_VERSION);
        return ESP_OK;
    case ZONES_DECODE_NEWER:
        /* Firmware-rollback case (TODO.md 8.1): leave flash untouched, fall
         * back to defaults for this boot only. *out_found MUST be true --
         * this is real, deliberately-protected data (kiln_nvs genuinely has
         * something), so migrate_from_default_partition() must not treat it
         * as "nothing here" and overwrite it with a stale pre-split copy. */
        ESP_LOGW(TAG, "zones_cfg from '%s' is version %u, newer than this firmware's %u -- "
                      "refusing to load, flash data left untouched",
                 partition, (unsigned)raw[0], (unsigned)ZONES_CFG_VERSION);
        if (out_found) {
            *out_found = true;
        }
        return ESP_OK;
    case ZONES_DECODE_CORRUPT:
    default:
        /* Genuine corruption (too short, wrong length for the claimed
         * version, bad CRC, or a decoded config that failed
         * zones_config_json_validate()) -- loud enough that an operator can see it,
         * naming the on-disk version and the specific rejection reason.
         * Nothing worth protecting was found here, so a caller (the
         * legacy-partition migration) is free to look elsewhere. */
        ESP_LOGW(TAG, "zones_cfg blob from '%s' (on-disk version %u, %u bytes) REJECTED: %s -- "
                      "falling back to defaults, NOT adopting this config",
                 partition, (unsigned)raw[0], (unsigned)len, reason);
        return ESP_OK;
    }
}

/* One-time move of the persisted zones config out of the default partition's
 * NVS_NAMESPACE/NVS_KEY_ZONES and into KILN_NVS_PARTITION's, for boards
 * provisioned by firmware predating the 2026-08-13 split. Simplified from
 * wifi_prov.c's migrate_from_default_partition() to a one-directional copy:
 * kiln_nvs is only ever consulted first, so if it already has something there
 * is nothing to migrate and no "which wins" question to answer -- only the
 * old default-partition location could hold pre-migration data. The old copy
 * is deliberately left in place (not deleted), same rationale as
 * wifi_prov.c: it needs to still be there if someone rolls back to
 * pre-split firmware. */
static void migrate_from_default_partition(void)
{
    zones_cfg_t from_default;
    bool found_in_default = false;
    bool valid_in_default = false;
    esp_err_t err = nvs_load_from(NVS_DEFAULT_PART_NAME, &from_default, &found_in_default, &valid_in_default);
    if (err != ESP_OK || !found_in_default) {
        return; /* nothing to migrate */
    }
    if (!valid_in_default) {
        /* found_in_default is true for two different reasons now (see FIX 1
         * in nvs_load_from()): a refused newer-than-firmware blob, or a
         * genuinely current/older key nvs_get_blob() actually read that
         * turned out corrupt is instead reported found=false above, so the
         * only way to reach here with valid_in_default false is the
         * refused-newer case. Copying it forward would destroy exactly the
         * kind of data this whole refuse-and-leave-untouched discipline
         * exists to protect -- for the SAME reason it must not be
         * overwritten in kiln_nvs, it must not be blindly migrated out of
         * the default partition either. Leave both partitions as they are;
         * this runs again next boot with no data lost either way. */
        ESP_LOGW(TAG, "zones_cfg in the default NVS partition exists but nvs_load_from() refused it -- "
                      "not migrating it to '%s'", KILN_NVS_PARTITION);
        return;
    }

    ESP_LOGI(TAG, "migrating zones_cfg from the default NVS partition to '%s'", KILN_NVS_PARTITION);

    s_zones.cfg = from_default;
    /* Reaching here means valid_in_default was true -- nvs_load_from()
     * actually decoded from_default, current version or an older version
     * successfully migrated -- so this is always something ready to run a
     * kiln against, never a corrupt or refused blob (both return above). */
    s_zones_config_valid = valid_in_default;
    esp_err_t save_err = nvs_save();
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "migration write to '%s' failed: %s -- running from the old copy this boot, will retry",
                 KILN_NVS_PARTITION, esp_err_to_name(save_err));
    }
}

/* out_found/out_valid are nvs_load_from()'s own outputs, passed straight
 * through -- see FIX 1's history here: zones_http_start() used to reconstruct
 * "was anything found in kiln_nvs" from s_zones.cfg.version != 0 after this
 * call, which cannot tell "genuinely nothing was ever saved" (version reads
 * 0 because nothing was ever written) from "something WAS found, but it was
 * a newer-than-firmware blob nvs_load_from() refused and zeroed" (version
 * also reads 0, because the refusal path memsets out_cfg). Both callers now
 * get the real found/valid flags instead of guessing from the zeroed struct. */
static esp_err_t nvs_load(bool *out_found, bool *out_valid)
{
    return nvs_load_from(KILN_NVS_PARTITION, &s_zones.cfg, out_found, out_valid);
}

static esp_err_t nvs_save(void)
{
    s_zones.cfg.version = ZONES_CFG_VERSION;
    /* Stamped last, after every other field is final for this write -- see
     * zones_config_json_compute_crc()/zones_cfg_t::crc32's comments. Any in-RAM edit that
     * lands here (a setter, a POST commit, an import) gets a fresh, correct
     * CRC every time this function runs; there is no path that writes the
     * blob without also re-stamping it. */
    s_zones.cfg.crc32 = zones_config_json_compute_crc(&s_zones.cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_ZONES, &s_zones.cfg, sizeof(s_zones.cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- Relay names (owner request 2026-08-27+1: "the user should be able to
 * assign names to relays not assigned to zones as well") -------------------
 *
 * DESIGN DECISION, WITH NUMBERS: a SEPARATE NVS blob/key, not a field on
 * zones_cfg_t. zones_cfg_t is 500 bytes against the 512-byte
 * ZONES_CONFIG_BLOB_MAX_SIZE cap (_Static_assert below, zones_http.h) -- the
 * pass immediately before this one already spent the last 12 bytes of slack
 * reordering zone_cfg_t's fields and cutting TIMING_PROFILE_NAME_MAX_LEN to
 * 7 just to land there. KILN_IO_RELAY_COUNT (4) names at RELAY_NAME_MAX_LEN+1
 * (16) bytes each is 64 bytes on its own -- more than five TIMES the 12
 * bytes of headroom left, before even accounting for the version/CRC/length
 * bookkeeping a new field inside zones_cfg_t would also need. Putting it
 * there would mean either raising ZONES_CONFIG_BLOB_MAX_SIZE (a real
 * decision with its own consequences -- see that macro's own comment on what
 * else it sizes: kiln_cfg_store.c's fixed per-entry storage, multiplied by
 * however many saved kiln configs a board keeps) or clawing back another 64
 * bytes from zone_cfg_t the way the last pass clawed back 8/zone, for a
 * feature (relay names) that has nothing to do with a zone's own thermal
 * record at all -- a relay NOT in any zone is, by definition, data this
 * struct's own subject (zones) doesn't own.
 *
 * A relay name is also not safety-critical and nothing on the guard/control
 * path reads it (same "purely informational" status ct_mask has, per
 * ZONES_CFG_VERSION's 5->6 comment) -- there is no reason for it to share a
 * version number, a CRC, or a load/save transaction with the data that IS
 * safety-critical. Its own key, its own version, its own tiny struct,
 * loaded/saved independently, is the clean split.
 *
 * SIZE: 1 (version) + KILN_IO_RELAY_COUNT * (RELAY_NAME_MAX_LEN + 1)
 * (4 * 16 = 64) + 4 (crc32) = 69 bytes today, nowhere near even the smallest
 * NVS blob limits, and it grows only with KILN_IO_RELAY_COUNT -- never with
 * zones_cfg_t. The two ceilings are now completely independent, which is the
 * whole point of the split: a future zones_cfg_t growth pass never has to
 * think about relay names again, and a future relay-count growth never has
 * to think about ZONES_CONFIG_BLOB_MAX_SIZE.
 *
 * WHAT HAPPENS WHEN A NAMED RELAY BECOMES ZONE-OWNED: the name is KEPT, not
 * cleared. Clearing it the instant an operator ticks a relay checkbox while
 * still laying out their zones would throw away typing on nothing more than
 * a checkbox they may untick five minutes later -- and the string costs its
 * 16 bytes on flash whether populated or not, so there is no space pressure
 * pushing the other way, unlike thermo_mask's genuinely different "0 has a
 * real, different meaning" situation. The RENDERING layer (zones_page.html's
 * renderRelayNames()) is what hides a zone-owned relay's name field behind
 * "assigned to a zone" text instead of an editable box, so an operator never
 * SEES a stale name next to a zone relay even though it is still on flash
 * underneath, ready to reappear the moment the relay is unassigned again.
 *
 * "Not assigned to any zone" is computed LIVE from the current zone config
 * every time it matters, via zone_owned_relay_mask() below -- never cached.
 * Assignment changes whenever the operator edits the zones on the very same
 * page, exactly the live-recompute requirement rules_task.c's
 * compute_heater_relay_mask() and rules_http.c's check_relay_not_zone_owned()
 * already have for the identical question ("which relays does the zone
 * config currently claim"). Both of those files are DO-NOT-TOUCH for this
 * pass, so their rule is mirrored here rather than imported -- it is the
 * exact same computation (union of every configured zone's relay_mask), not
 * a second, independently-drifting one. */
#define RELAY_NAMES_CFG_VERSION 1
#define NVS_KEY_RELAY_NAMES "relay_names_cfg"

typedef struct {
    uint8_t version;
    char names[KILN_IO_RELAY_COUNT][RELAY_NAME_MAX_LEN + 1];
    uint32_t crc32;
} relay_names_cfg_t;

static struct {
    relay_names_cfg_t cfg;
} s_relay_names;

/* Same "compute over a zeroed-crc-field copy" convention as zones_config_json_compute_crc(). */
static uint32_t compute_relay_names_crc(const relay_names_cfg_t *cfg)
{
    relay_names_cfg_t tmp = *cfg;
    tmp.crc32 = 0;
    return esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}

/* Loads s_relay_names.cfg from NVS_KEY_RELAY_NAMES (same namespace/partition
 * as the zones blob -- see NVS_NAMESPACE/KILN_NVS_PARTITION above). Resets to
 * all-empty names -- not reported to the caller as a failure, this is purely
 * cosmetic data, unlike s_zones_config_valid's safety-relevant "cannot
 * trust this" gate -- on: namespace/key not found (first boot, or a board
 * that has never named a relay), a blob whose length doesn't match
 * sizeof(relay_names_cfg_t) (only one version exists so far, so there is
 * only one valid length, but the check is written the same "length before
 * interpretation" way zones_config_json_decode_blob() uses for the zones blob, not
 * skipped just because there is nothing to switch on yet), an unrecognized
 * version, or a CRC mismatch. RELAY_NAMES_CFG_VERSION exists now, before
 * there is a second version to get wrong, specifically so a future format
 * change has an established version field to switch on instead of repeating
 * this codebase's own "grew the struct, forgot the old layout" history (see
 * ZONES_CFG_VERSION's 6->7 comment for exactly what that mistake cost). */
static void relay_names_load(void)
{
    memset(&s_relay_names.cfg, 0, sizeof(s_relay_names.cfg));

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return; /* namespace not yet created -- first boot, names stay blank */
    }
    uint8_t raw[sizeof(relay_names_cfg_t)];
    size_t len = sizeof(raw);
    err = nvs_get_blob(h, NVS_KEY_RELAY_NAMES, raw, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return; /* ESP_ERR_NVS_NOT_FOUND (never saved) or a real error -- blank is safe either way */
    }
    if (len != sizeof(relay_names_cfg_t)) {
        ESP_LOGE(TAG, "relay_names blob is %u bytes, expected %u -- discarding, names reset to blank",
                 (unsigned)len, (unsigned)sizeof(relay_names_cfg_t));
        return;
    }
    relay_names_cfg_t cand;
    memcpy(&cand, raw, sizeof(cand));
    if (cand.version != RELAY_NAMES_CFG_VERSION) {
        ESP_LOGE(TAG, "relay_names blob version %u is not %u -- discarding, names reset to blank",
                 cand.version, RELAY_NAMES_CFG_VERSION);
        return;
    }
    uint32_t computed = compute_relay_names_crc(&cand);
    if (computed != cand.crc32) {
        ESP_LOGE(TAG, "relay_names blob CRC mismatch (stored 0x%08lx, computed 0x%08lx) -- discarding, "
                      "names reset to blank",
                 (unsigned long)cand.crc32, (unsigned long)computed);
        return;
    }
    /* Defensive NUL-termination against a corrupted-but-CRC-lucky blob --
     * every name must be a valid C string before JSON emission or a POST
     * scratch-buffer strncpy touches it. */
    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        cand.names[r][RELAY_NAME_MAX_LEN] = '\0';
    }
    s_relay_names.cfg = cand;
}

static esp_err_t relay_names_save(void)
{
    s_relay_names.cfg.version = RELAY_NAMES_CFG_VERSION;
    s_relay_names.cfg.crc32 = compute_relay_names_crc(&s_relay_names.cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_RELAY_NAMES, &s_relay_names.cfg, sizeof(s_relay_names.cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- Task 1's persisted normal-current results ----------------------------
 * Same reasoning as relay_names_cfg_t just above, applied to a different
 * field: a SEPARATE NVS blob/key, not a field on zones_cfg_t.
 * ZONES_CONFIG_BLOB_MAX_SIZE (640) already has zones_cfg_t sitting well up;
 * three more floats plus the version/CRC bookkeeping would either force that
 * ceiling up (a real decision with knock-on effects on kiln_cfg_store.c's
 * fixed per-entry size, see that macro's own comment) or claw back yet more
 * bytes from zone_cfg_t for data that has nothing to do with a zone's own
 * thermal record. Also not safety-critical -- nothing on the guard/control
 * path reads it, only Task 2's WARNING predicate above -- so it has no
 * business sharing a version/CRC/load transaction with data that is. */
/* 1 -> 2 (M12): the sweep now also derives which CT channel watches which
 * zone (ct_map_*, see zone_sweep_derive_ct_channel() below) and that record
 * has to outlive the sweep task -- the commissioning page reads it on a
 * later page load to decide whether ct_channel_map[0..2] renders as DERIVED
 * or as a manual-entry field. A v1 blob is discarded by the version check
 * below rather than migrated: the whole blob is re-measured by one button
 * press, and the alternative (reading a short struct and zero-filling the
 * tail) is a migration path worth writing only for data that cannot simply
 * be measured again. */
/* 2 -> 3 (M12b): the sweep now also derives k_ct_v_per_a[0..2] from the
 * nameplate power the operator already answered (COMMISSIONING_UX.md Q3/Q4)
 * and this run's own measured current, and the commissioning page has to be
 * able to say DERIVED on a later page load for that field too. A v2 blob is
 * discarded rather than migrated, for exactly the reason v1 was: one button
 * press re-measures the whole thing. */
#define ZONE_NORMALS_CFG_VERSION 3
#define NVS_KEY_ZONE_NORMALS "zone_normals_cfg"

typedef struct {
    uint8_t  version;
    uint8_t  measured_mask; /* bit i = zone i has a measured normal current */
    float    normal_current_a[MAX31856_CHANNEL_COUNT];
    /* v2: the derived CT-channel -> zone mapping. bit c of
     * ct_map_derived_mask set means ct_map_zone[c] is a zone index the sweep
     * derived UNAMBIGUOUSLY (COMMISSIONING_UX.md sec 1.2's condition); a
     * clear bit means "never derived", and ct_map_zone[c] is meaningless. */
    uint8_t  ct_map_derived_mask;
    uint8_t  ct_map_zone[ZONE_CT_CHANNEL_COUNT];
    /* v3: the derived CT volts-per-amp scale. bit c of k_ct_derived_mask set
     * means k_ct_v_per_a[c] is a value this board CALIBRATED from a complete
     * sweep and confirmed written to the safety processor; a clear bit means
     * "never derived here" and k_ct_v_per_a[c] is meaningless -- it says
     * nothing about whether the Pico's own k_ct_v_per_a[c] is set, which an
     * operator may always have entered by hand. */
    uint8_t  k_ct_derived_mask;
    float    k_ct_v_per_a[ZONE_CT_CHANNEL_COUNT];
    uint32_t crc32;
} zone_normals_cfg_t;

static struct {
    zone_normals_cfg_t cfg;
} s_zone_normals;

static uint32_t compute_zone_normals_crc(const zone_normals_cfg_t *cfg)
{
    zone_normals_cfg_t tmp = *cfg;
    tmp.crc32 = 0;
    return esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}

/* Same "reset to blank, log, move on" convention as relay_names_load() --
 * this is measured convenience data, not safety state, so a bad blob is
 * simply forgotten (every zone reads back as "never measured") rather than
 * blocking anything. */
static void zone_normals_load(void)
{
    memset(&s_zone_normals.cfg, 0, sizeof(s_zone_normals.cfg));

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return;
    }
    uint8_t raw[sizeof(zone_normals_cfg_t)];
    size_t len = sizeof(raw);
    err = nvs_get_blob(h, NVS_KEY_ZONE_NORMALS, raw, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return; /* never saved, or a read error -- blank is safe either way */
    }
    if (len != sizeof(zone_normals_cfg_t)) {
        ESP_LOGE(TAG, "zone_normals blob is %u bytes, expected %u -- discarding",
                 (unsigned)len, (unsigned)sizeof(zone_normals_cfg_t));
        return;
    }
    zone_normals_cfg_t cand;
    memcpy(&cand, raw, sizeof(cand));
    if (cand.version != ZONE_NORMALS_CFG_VERSION) {
        ESP_LOGE(TAG, "zone_normals blob version %u is not %u -- discarding", cand.version,
                 ZONE_NORMALS_CFG_VERSION);
        return;
    }
    uint32_t computed = compute_zone_normals_crc(&cand);
    if (computed != cand.crc32) {
        ESP_LOGE(TAG, "zone_normals blob CRC mismatch -- discarding");
        return;
    }
    s_zone_normals.cfg = cand;
}

static esp_err_t zone_normals_save(void)
{
    s_zone_normals.cfg.version = ZONE_NORMALS_CFG_VERSION;
    s_zone_normals.cfg.crc32 = compute_zone_normals_crc(&s_zone_normals.cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_ZONE_NORMALS, &s_zone_normals.cfg, sizeof(s_zone_normals.cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool zones_config_get_normal_current(uint8_t zone_index, float *out_amps, bool *out_measured)
{
    if (!out_amps || !out_measured || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    *out_measured = (s_zone_normals.cfg.measured_mask & (1u << zone_index)) != 0;
    *out_amps = *out_measured ? s_zone_normals.cfg.normal_current_a[zone_index] : 0.0f;
    return true;
}

/* Persists one zone's measured normal -- called only by zone_sweep_task()
 * below, once per zone that actually produced a sample. Not exposed in
 * zones_http.h: the sweep is the only legitimate writer (this is measured
 * data, not an operator-entered field), so there is no setter for anything
 * outside this file to call. */
static bool zone_normals_set(uint8_t zone_index, float amps)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !isfinite(amps) || amps < 0.0f) {
        return false;
    }
    s_zone_normals.cfg.normal_current_a[zone_index] = amps;
    s_zone_normals.cfg.measured_mask |= (uint8_t)(1u << zone_index);
    return zone_normals_save() == ESP_OK;
}

/* Same "the sweep is the only legitimate writer" rule zone_normals_set()
 * above states, applied to the derived CT map: this is measured data, and
 * an operator who wants to say it by hand says it on the commissioning page
 * (which writes the Pico's ct_channel_map directly), never here. Clearing
 * the whole record at the START of a sweep is deliberate -- a re-sweep of
 * rewired hardware must not leave a channel's stale "derived" claim behind
 * to be shown as current. */
static void zone_ct_map_clear(void)
{
    s_zone_normals.cfg.ct_map_derived_mask = 0;
    memset(s_zone_normals.cfg.ct_map_zone, 0, sizeof(s_zone_normals.cfg.ct_map_zone));
    /* Persisted immediately, not left for the first zone_ct_map_set() to
     * flush: a sweep that clears the map and then fails outright never
     * reaches a set(), and leaving the old map in NVS would resurrect it on
     * the next boot as though it were still current. */
    (void)zone_normals_save();
}

static bool zone_ct_map_set(uint8_t ct_channel, uint8_t zone_index)
{
    if (ct_channel >= ZONE_CT_CHANNEL_COUNT || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    s_zone_normals.cfg.ct_map_zone[ct_channel] = zone_index;
    s_zone_normals.cfg.ct_map_derived_mask |= (uint8_t)(1u << ct_channel);
    return zone_normals_save() == ESP_OK;
}

/* M12b: same discipline as zone_ct_map_clear()/zone_ct_map_set() just above
 * -- this record is provenance ONLY. The value the guards and the power
 * estimate actually use lives on the Pico and is written by
 * zone_sweep_push_k_ct_v_per_a(); nothing here is ever read back as a
 * calibration. */
static void zone_k_ct_clear(void)
{
    s_zone_normals.cfg.k_ct_derived_mask = 0;
    memset(s_zone_normals.cfg.k_ct_v_per_a, 0, sizeof(s_zone_normals.cfg.k_ct_v_per_a));
    (void)zone_normals_save();
}

static bool zone_k_ct_set(uint8_t ct_channel, float k_v_per_a)
{
    if (ct_channel >= ZONE_CT_CHANNEL_COUNT || !isfinite(k_v_per_a) || k_v_per_a <= 0.0f) {
        return false;
    }
    s_zone_normals.cfg.k_ct_v_per_a[ct_channel] = k_v_per_a;
    s_zone_normals.cfg.k_ct_derived_mask |= (uint8_t)(1u << ct_channel);
    return zone_normals_save() == ESP_OK;
}

void zones_ct_k_v_per_a_derived(uint8_t *out_derived_mask, float *out_k_v_per_a)
{
    if (out_derived_mask) {
        *out_derived_mask = s_zone_normals.cfg.k_ct_derived_mask;
    }
    if (out_k_v_per_a) {
        memcpy(out_k_v_per_a, s_zone_normals.cfg.k_ct_v_per_a, sizeof(s_zone_normals.cfg.k_ct_v_per_a));
    }
}

void zones_ct_channel_map_derived(uint8_t *out_derived_mask, uint8_t *out_zone_for_ch)
{
    if (out_derived_mask) {
        *out_derived_mask = s_zone_normals.cfg.ct_map_derived_mask;
    }
    if (out_zone_for_ch) {
        memcpy(out_zone_for_ch, s_zone_normals.cfg.ct_map_zone, ZONE_CT_CHANNEL_COUNT);
    }
}

/* Union of every currently-configured zone's relay_mask -- bit N-1 set iff
 * relay N (1-based) is claimed by SOME zone. Computed fresh from `cfg` every
 * call, never cached -- see this section's header comment. Deliberately a
 * local mirror of rules_task.c's compute_heater_relay_mask() / the loop
 * inside rules_http.c's check_relay_not_zone_owned(): both files are
 * off-limits for this pass, so the identical rule (union of relay_mask over
 * every zone index < thermo_count) is reimplemented here rather than
 * imported -- it must stay the SAME rule, not a second one that can drift. */
static uint8_t zone_owned_relay_mask(const zones_cfg_t *cfg)
{
    uint8_t mask = 0;
    for (uint8_t zi = 0; zi < cfg->thermo_count && zi < MAX31856_CHANNEL_COUNT; zi++) {
        mask |= cfg->zones[zi].relay_mask;
    }
    return mask;
}

/* ---- Public getters (profiles_http.c) ------------------------------------ */

bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr)
{
    if (!out_c_per_hr || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_c_per_hr = s_zones.cfg.zones[zone_index].max_ramp_c_per_hr;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_ramp enforces. 0 is legal (the
 * documented "never configured" encoding). */
bool zones_config_set_max_ramp(uint8_t zone_index, float c_per_hr)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(c_per_hr) || c_per_hr < 0.0f || c_per_hr > ZONE_MAX_RAMP_C_PER_HR_MAX) {
        return false;
    }
    s_zones.cfg.zones[zone_index].max_ramp_c_per_hr = c_per_hr;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_cal_offset(uint8_t zone_index, float *out_cal_offset_c)
{
    if (!out_cal_offset_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_cal_offset_c = s_zones.cfg.zones[zone_index].cal_offset_c;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_cal enforces. */
bool zones_config_set_cal_offset(uint8_t zone_index, float cal_offset_c)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(cal_offset_c) || cal_offset_c < ZONE_CAL_OFFSET_MIN_C || cal_offset_c > ZONE_CAL_OFFSET_MAX_C) {
        return false;
    }
    s_zones.cfg.zones[zone_index].cal_offset_c = cal_offset_c;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

uint32_t zones_config_generation(void)
{
    return s_config_generation;
}

/* TODO.md 8.2 "Tie it to the guards, not only the UI" -- see
 * s_zones_config_valid's comment for the exact rule. Consulted by
 * profile_executor.c/autotune_engine.c before allowing a run to start, and
 * reported on /api/status (dashboard_http.c) as zones_config_valid. */
bool zones_config_is_valid(void)
{
    return s_zones_config_valid;
}

uint8_t zones_config_get_thermo_count(void)
{
    return s_zones.cfg.thermo_count;
}

uint8_t zones_config_get_relay_count(void)
{
    return s_zones.cfg.relay_count;
}

uint8_t zones_config_get_max_simultaneous_relays(void)
{
    return s_zones.cfg.max_simultaneous_relays;
}

bool zones_config_get_continue_on_zone_trip(void)
{
    return s_zones.cfg.continue_on_zone_trip != 0;
}

/* 2026-08-21, TODO.md owner-report item 3 -- safety_link.c's consumer (see
 * its safety_sync_tc_type()): the RP2040 safety processor's own,
 * independent thermocouple type, as last saved on this page. Always
 * answerable, unlike the per-zone getters below -- this is a global setting
 * with a real value from the moment NVS first loads (defaulting to
 * THERMO_TC_K either via a fresh zero-init struct reading 0/THERMO_TC_B...
 * no: see below) -- so, unlike zones_config_get_max_ramp() and friends,
 * there is no "cannot answer" case to report via a bool return; the return
 * value exists only so this getter's shape matches every other one in this
 * file and a future caller doesn't have to special-case it.
 *
 * IMPORTANT: on a board that has never loaded a valid zones config at all
 * (s_zones_config_valid false -- first boot, corrupt NVS, a refused
 * newer-than-firmware blob), s_zones.cfg is the zeroed default, and 0 here
 * decodes as THERMO_TC_B, NOT THERMO_TC_K -- unlike MAX31856.c's own
 * hardcoded boot default. safety_link.c's caller MUST check
 * zones_config_is_valid() itself before trusting this value for anything
 * other than "what would get sent if asked to sync right now" -- see that
 * file's safety_sync_tc_type() for how it actually guards this. */
bool zones_config_get_safety_tc_type(uint8_t *out_tc_type)
{
    if (!out_tc_type) {
        return false;
    }
    *out_tc_type = s_zones.cfg.safety_tc_type;
    return true;
}

/* 2026-08-21, LCD item 1 (TODO.md owner-report item 1/4's on-device half):
 * the web zones page (zones_page.html) gained per-channel thermocouple type
 * selection this same day, but only as an HTTP form field -- there was no
 * public getter/setter an LCD page could call, only the POST body parser's
 * private z%u_tctype handling above. Follows zones_config_get_pid()'s/
 * zones_config_set_pid()'s exact shape (same zone_index bounds check, same
 * "false means cannot answer" convention, setter bumps s_config_generation
 * before nvs_save() so a running profile's next tick sees the new value
 * whether or not the flash write itself succeeds) so this module keeps
 * exactly one accessor pattern rather than growing a second one for LCD
 * callers specifically.
 *
 * Deliberately does NOT call thermo_owner_command_config_channel() to push
 * the new type to hardware immediately -- checked zones_http.c's own POST
 * /api/zones handler (the web page's save path) before writing this, and it
 * doesn't either: the only place this module ever calls that function is
 * zones_http_start()'s boot-time apply, further down this file. So an LCD
 * write and a web-page save now have the SAME behaviour (persisted
 * immediately, taken into the running MAX31856 register only on the next
 * boot) rather than the LCD accidentally doing more than the web form it is
 * mirroring. If that boot-only gap is ever closed for the web path, this
 * setter should gain the same live-apply call at the same time -- not
 * silently drift ahead of it. */
bool zones_config_get_tc_type(uint8_t zone_index, uint8_t *out_tc_type)
{
    if (!out_tc_type || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_tc_type = s_zones.cfg.zones[zone_index].tc_type;
    return true;
}

bool zones_config_set_tc_type(uint8_t zone_index, uint8_t tc_type)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (tc_type > ZONE_TC_TYPE_MAX_REAL) {
        /* Same bound the POST parser enforces (parse_zone_fields()) -- a
         * voltage-input mode is never a legal choice from an
         * operator-facing "thermocouple type" control, LCD or web alike. */
        return false;
    }
    s_zones.cfg.zones[zone_index].tc_type = tc_type;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* Setter half of zones_config_get_safety_tc_type() above -- same "global,
 * not per-zone" setting (the RP2040 safety processor's own MAX31856-equivalent
 * type), same bound as the per-channel setter just above, same generation/
 * nvs_save shape as every other setter in this file. safety_link.c's
 * safety_sync_tc_type() is the consumer that notices the generation bump and
 * re-mirrors this to the Pico -- this function does not talk to the safety
 * link itself, matching the POST handler's own division of labor (this
 * module owns storage only). */
bool zones_config_set_safety_tc_type(uint8_t tc_type)
{
    if (tc_type > ZONE_TC_TYPE_MAX_REAL) {
        return false;
    }
    s_zones.cfg.safety_tc_type = tc_type;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (!out_mask || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_mask = s_zones.cfg.zones[zone_index].relay_mask;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_relay_mask handling enforces --
 * relay_mask may only reference relays 1..relay_count. */
bool zones_config_set_relay_mask(uint8_t zone_index, uint8_t relay_mask)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t valid_bits = s_zones.cfg.relay_count >= 8 ? 0xFF : (uint8_t)((1u << s_zones.cfg.relay_count) - 1u);
    if ((relay_mask & ~valid_bits) != 0) {
        return false;
    }
    s_zones.cfg.zones[zone_index].relay_mask = relay_mask;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_thermo_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (!out_mask || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_mask = s_zones.cfg.zones[zone_index].thermo_mask;
    return true;
}

/* Same bound parse_zone_fields()'s explicit z%u_thermo_mask handling
 * enforces -- thermo_mask may only reference channels 1..thermo_count.
 * Unlike the POST handler this setter has no "omitted means preserve the
 * legacy mapping" case -- see this function's header comment. */
bool zones_config_set_thermo_mask(uint8_t zone_index, uint8_t thermo_mask)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t valid_bits =
        s_zones.cfg.thermo_count >= 8 ? 0xFF : (uint8_t)((1u << s_zones.cfg.thermo_count) - 1u);
    if ((thermo_mask & ~valid_bits) != 0) {
        return false;
    }
    s_zones.cfg.zones[zone_index].thermo_mask = thermo_mask;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_ct_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (!out_mask || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_mask = s_zones.cfg.zones[zone_index].ct_mask;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_ct_mask handling enforces -- ct_mask
 * may only reference channels 1..ZONE_CT_CHANNEL_COUNT, a fixed hardware
 * count (not relay_count/thermo_count-relative like the two setters above). */
bool zones_config_set_ct_mask(uint8_t zone_index, uint8_t ct_mask)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t valid_bits = (uint8_t)((1u << ZONE_CT_CHANNEL_COUNT) - 1u);
    if ((ct_mask & ~valid_bits) != 0) {
        return false;
    }
    s_zones.cfg.zones[zone_index].ct_mask = ct_mask;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_name(uint8_t zone_index, char *out, size_t out_cap)
{
    if (!out || out_cap == 0 || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    strncpy(out, s_zones.cfg.zones[zone_index].name, out_cap - 1);
    out[out_cap - 1] = '\0';
    return true;
}

/* Same rejection parse_zone_fields() produces for an overlong z%u_name
 * (http_form_find_field() returning -2), applied to a NUL-terminated C
 * string. NULL is treated as an empty name (clears it), matching a POST that
 * omits the field. */
bool zones_config_set_name(uint8_t zone_index, const char *name)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    size_t len = name ? strlen(name) : 0;
    if (len > ZONE_NAME_MAX_LEN) {
        return false;
    }
    strncpy(s_zones.cfg.zones[zone_index].name, name ? name : "", ZONE_NAME_MAX_LEN);
    s_zones.cfg.zones[zone_index].name[ZONE_NAME_MAX_LEN] = '\0';
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* See zones_http.h's doc comment on this pair, and this file's relay-names
 * section header comment for the full design rationale (separate NVS blob,
 * name kept across zone reassignment). */
bool zones_config_get_relay_name(uint8_t relay_n, char *out, size_t out_cap)
{
    if (relay_n < 1 || relay_n > KILN_IO_RELAY_COUNT || !out || out_cap == 0) {
        return false;
    }
    strncpy(out, s_relay_names.cfg.names[relay_n - 1], out_cap - 1);
    out[out_cap - 1] = '\0';
    return true;
}

bool zones_config_set_relay_name(uint8_t relay_n, const char *name)
{
    if (relay_n < 1 || relay_n > KILN_IO_RELAY_COUNT) {
        return false;
    }
    size_t len = name ? strlen(name) : 0;
    if (len > RELAY_NAME_MAX_LEN) {
        return false;
    }
    strncpy(s_relay_names.cfg.names[relay_n - 1], name ? name : "", RELAY_NAME_MAX_LEN);
    s_relay_names.cfg.names[relay_n - 1][RELAY_NAME_MAX_LEN] = '\0';
    return relay_names_save() == ESP_OK;
}

bool zones_config_get_pid(uint8_t zone_index, float *out_kp, float *out_ki, float *out_kd)
{
    if (!out_kp || !out_ki || !out_kd || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_kp = z->pid_kp;
    *out_ki = z->pid_ki;
    *out_kd = z->pid_kd;
    return true;
}

bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(kp) || !isfinite(ki) || !isfinite(kd) || kp < 0.0f || ki < 0.0f || kd < 0.0f) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->pid_kp = kp;
    z->pid_ki = ki;
    z->pid_kd = kd;
    /* Bumped before the NVS write, not after it: the gains are already live
     * for the next control tick at this point, so a running profile must
     * re-read them (TODO.md 6A.7) whether or not the save succeeds. Note
     * this returns false on a save failure while the config change stands --
     * unchanged behaviour, and the generation reflects the in-RAM truth. */
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* See zones_http.h -- Phase 3 control-loop wiring's read of the fuzzy
 * adjustment-strength knob. */
bool zones_config_get_fuzzy_strength_pct(uint8_t zone_index, float *out_pct)
{
    if (!out_pct || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_pct = s_zones.cfg.zones[zone_index].fuzzy_strength_pct;
    return true;
}

/* Writer for the getter above. Same bound parse_zone_fields()'s
 * z%u_fuzzy_strength enforces (0..ZONE_FUZZY_STRENGTH_PCT_MAX) -- refused,
 * never clamped, same discipline as every other setter in this file. */
bool zones_config_set_fuzzy_strength_pct(uint8_t zone_index, float pct)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(pct) || pct < 0.0f || pct > ZONE_FUZZY_STRENGTH_PCT_MAX) {
        return false;
    }
    s_zones.cfg.zones[zone_index].fuzzy_strength_pct = pct;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* Row-based (ZONES_CFG_VERSION 10->11) -- see zone_cfg_t::coupling_coeff's
 * own doc comment for what each cell means. out_row must have room for
 * MAX31856_CHANNEL_COUNT floats; the diagonal (out_row[zone_index]) is
 * always 0 on return, same "unused, stays zero" rule the storage itself
 * enforces. */
bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    memcpy(out_row, z->coupling_coeff, sizeof(z->coupling_coeff));
    return true;
}

/* Whole-row setter -- every cell checked before ANY is written, same
 * "no half-updated group" discipline as zones_config_set_model()/
 * zones_config_set_temp_limits(). Bounds match parse_zone_fields()'s
 * z%u_coupling_c%u: each off-diagonal cell finite and in
 * 0..ZONE_COUPLING_COEFF_MAX (see that macro's doc comment for why this
 * stayed non-negative rather than gaining an independent sign). The
 * diagonal MUST be exactly 0 -- a zone's response to its own heater is
 * model_k_dc, not a coupling cell, and a nonzero diagonal would be
 * ambiguous with a real (if coincidentally equal) cross-gain. */
bool zones_config_set_coupling(uint8_t zone_index, const float row[MAX31856_CHANNEL_COUNT])
{
    if (!row || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        if (!isfinite(row[j])) {
            return false;
        }
        if (j == zone_index) {
            if (row[j] != 0.0f) {
                return false;
            }
            continue;
        }
        if (row[j] < 0.0f || row[j] > ZONE_COUPLING_COEFF_MAX) {
            return false;
        }
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    memcpy(z->coupling_coeff, row, sizeof(z->coupling_coeff));
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* Single-cell setter -- lets a caller update ONE neighbor's measured
 * coupling without clobbering the rest of the row's already-stored cells.
 * autotune_engine.c's finalize_fit() needs exactly this: a single relay run
 * on zone i only measures i's effect on each OTHER zone j, one cell of zone
 * j's row at a time, and must never wipe out zone j's other, previously
 * measured neighbors just because this run didn't touch them. Same bounds
 * as the whole-row setter above, applied to the one cell being written. */
bool zones_config_set_coupling_cell(uint8_t zone_index, uint8_t neighbor_index, float coeff)
{
    if (zone_index >= s_zones.cfg.thermo_count || neighbor_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    if (!isfinite(coeff)) {
        return false;
    }
    if (neighbor_index == zone_index) {
        if (coeff != 0.0f) {
            return false;
        }
        return true; /* writing the diagonal to 0 is a no-op, not an error */
    }
    if (coeff < 0.0f || coeff > ZONE_COUPLING_COEFF_MAX) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->coupling_coeff[neighbor_index] = coeff;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_settings_source(uint8_t zone_index, uint8_t *out_settings_source)
{
    if (!out_settings_source || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_settings_source = s_zones.cfg.zones[zone_index].settings_source;
    return true;
}

/* Setter for the getter above. Same rule parse_zone_fields()'s
 * z%u_settings_source enforces: either ZONE_SETTINGS_SOURCE_CUSTOM (0xFF) or
 * a real zone index < MAX31856_CHANNEL_COUNT that is NOT zone_index itself
 * (self-reference is the degenerate inheritance cycle, refused here for the
 * identical reason parse_zone_fields() refuses it) -- AND, past that, does
 * not close a longer cycle through any OTHER zone's already-stored link
 * (zones_config_json_settings_source_chain_has_cycle() on a probe copy with this one link
 * applied). Only this one link changes here, so a cycle can only be newly
 * created running THROUGH zone_index -- walking the chain starting there,
 * against every other zone's live stored value, is sufficient; it does not
 * need to check every zone. */
bool zones_config_set_settings_source(uint8_t zone_index, uint8_t settings_source)
{
    /* zone_index is bounds-checked against thermo_count, same as every
     * other per-zone setter in this file -- but thermo_count itself is only
     * ever trusted up to MAX31856_CHANNEL_COUNT elsewhere (see
     * zones_config_settings_source_import_has_cycle() and
     * zones_config_json_normalize_settings_source_cycles(), which both clamp it before using
     * it as a bound). This site did not: a corrupt thermo_count >
     * MAX31856_CHANNEL_COUNT (direct NVS tampering, or firmware that
     * predates zones_config_json_validate()'s own range check on it) would let a
     * zone_index >= MAX31856_CHANNEL_COUNT pass this check and then index
     * probe[]/s_zones.cfg.zones[] -- both fixed
     * MAX31856_CHANNEL_COUNT-sized arrays -- out of bounds on the very next
     * line, before the chain-walk below is even reached. Clamping here (not
     * just in the chain-walk's own thermo_count argument, which is a
     * separate, secondary consistency fix below) is what actually closes
     * that hole. */
    uint8_t thermo_count = s_zones.cfg.thermo_count > MAX31856_CHANNEL_COUNT ? MAX31856_CHANNEL_COUNT
                                                                             : s_zones.cfg.thermo_count;
    if (zone_index >= thermo_count) {
        return false;
    }
    if (settings_source != ZONE_SETTINGS_SOURCE_CUSTOM && settings_source >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    if (settings_source == zone_index) {
        return false;
    }
    zone_cfg_t probe[MAX31856_CHANNEL_COUNT];
    memcpy(probe, s_zones.cfg.zones, sizeof(probe));
    probe[zone_index].settings_source = settings_source;
    if (zones_config_json_settings_source_chain_has_cycle(probe, zone_index, thermo_count)) {
        return false;
    }
    s_zones.cfg.zones[zone_index].settings_source = settings_source;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* Commit-loop counterpart to zones_config_set_settings_source() for a
 * multi-entry import/whole-page write that has ALREADY passed
 * zones_config_settings_source_import_has_cycle() against the full proposed
 * set. That pre-check validates the FINAL assembled state; this setter skips
 * the chain-walk zones_config_set_settings_source() runs against the
 * PARTIALLY APPLIED live config while a multi-entry commit loop is
 * mid-flight, because that walk can spuriously refuse an intermediate state
 * a valid two-zone swap (e.g. live 0->1,1->0 changing to 0->2,1->CUSTOM: pass
 * 1 accepts the final state, but committing zone 0 first makes the walk see
 * live {0->2,1->0}, no cycle there -- the actual failure case is the reverse
 * order or a longer swap, see backup_http.c's restore-scenario comment)
 * without any live-config help from an in-progress commit. Only bounds and
 * self-reference are re-checked here (still real defenses against a
 * corrupt/malicious override_source entry slipping past pass 1); the cycle
 * walk itself is intentionally omitted so this call cannot fail for a
 * zone/value pair pass 1 already accepted, keeping the two-pass invariant
 * this file's import/whole-page paths depend on: pass 2 must not be able to
 * fail. Callers MUST have run zones_config_settings_source_import_has_cycle()
 * over every candidate in this commit loop first -- this function trusts
 * that check, it does not repeat it. */
bool zones_config_set_settings_source_unchecked(uint8_t zone_index, uint8_t settings_source)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (settings_source != ZONE_SETTINGS_SOURCE_CUSTOM && settings_source >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    if (settings_source == zone_index) {
        return false;
    }
    s_zones.cfg.zones[zone_index].settings_source = settings_source;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_settings_source_import_has_cycle(const bool has_override[MAX31856_CHANNEL_COUNT],
                                                    const uint8_t override_source[MAX31856_CHANNEL_COUNT],
                                                    uint8_t *out_cycle_zone)
{
    zone_cfg_t probe[MAX31856_CHANNEL_COUNT];
    memcpy(probe, s_zones.cfg.zones, sizeof(probe));
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        if (has_override[i]) {
            probe[i].settings_source = override_source[i];
        }
    }
    uint8_t thermo_count = s_zones.cfg.thermo_count > MAX31856_CHANNEL_COUNT ? MAX31856_CHANNEL_COUNT
                                                                             : s_zones.cfg.thermo_count;
    for (uint8_t i = 0; i < thermo_count; i++) {
        if (zones_config_json_settings_source_chain_has_cycle(probe, i, thermo_count)) {
            if (out_cycle_zone) {
                *out_cycle_zone = i;
            }
            return true;
        }
    }
    return false;
}

bool zones_config_get_sanity_rate(uint8_t zone_index, float *out_c_per_min)
{
    if (!out_c_per_min || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_c_per_min = s_zones.cfg.zones[zone_index].sanity_rate_c_per_min;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_sanity enforces. 0 is legal (the
 * documented "never configured" encoding). */
bool zones_config_set_sanity_rate(uint8_t zone_index, float c_per_min)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(c_per_min) || c_per_min < 0.0f || c_per_min > ZONE_SANITY_RATE_MAX_C_PER_MIN) {
        return false;
    }
    s_zones.cfg.zones[zone_index].sanity_rate_c_per_min = c_per_min;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_control_mode(uint8_t zone_index, zone_control_mode_t *out_mode)
{
    if (!out_mode || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_mode = (zone_control_mode_t)s_zones.cfg.zones[zone_index].control_mode;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_mode enforces (0-3). */
bool zones_config_set_control_mode(uint8_t zone_index, zone_control_mode_t mode)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if ((unsigned)mode > (unsigned)ZONE_CONTROL_MODE_PID_FUZZY) {
        return false;
    }
    s_zones.cfg.zones[zone_index].control_mode = (uint8_t)mode;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c)
{
    if (!out_max_temp_c || !out_min_temp_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_max_temp_c = z->max_temp_c;
    *out_min_temp_c = z->min_temp_c;
    return true;
}

/* Bundled setter -- see zones_http.h's comment on this pair for why NO
 * max_temp_c >= min_temp_c cross-check is added here: parse_zone_fields()
 * (the POST /api/zones authority) does not enforce one either, so this
 * setter matches it exactly rather than becoming stricter than the page it
 * mirrors. Both fields checked before either is written, same
 * reject-nothing-half-applied discipline as zones_config_set_model(). */
bool zones_config_set_temp_limits(uint8_t zone_index, float max_temp_c, float min_temp_c)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(max_temp_c) || max_temp_c < 0.0f || max_temp_c > ZONE_MAX_TEMP_C_MAX) {
        return false;
    }
    if (!isfinite(min_temp_c) || min_temp_c < ZONE_MIN_TEMP_C_MIN || min_temp_c > ZONE_MIN_TEMP_C_MAX) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->max_temp_c = max_temp_c;
    z->min_temp_c = min_temp_c;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_heater_cfg(uint8_t zone_index, float *out_window_ms, float *out_min_on_ms,
                                 float *out_min_off_ms)
{
    if (!out_window_ms || !out_min_on_ms || !out_min_off_ms || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_window_ms = z->heater_window_ms;
    *out_min_on_ms = z->heater_min_on_ms;
    *out_min_off_ms = z->heater_min_off_ms;
    return true;
}

/* Bundled setter, same "no half-updated group" discipline as
 * zones_config_set_model()/zones_config_set_temp_limits(). No
 * min_on_ms/min_off_ms-vs-window_ms cross-check: parse_zone_fields() (the
 * POST authority) does not enforce one either -- see
 * zones_config_set_temp_limits()'s comment for the identical reasoning. */
bool zones_config_set_heater_cfg(uint8_t zone_index, float window_ms, float min_on_ms, float min_off_ms)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(window_ms) || window_ms < 0.0f || window_ms > ZONE_HEATER_WINDOW_MS_MAX) {
        return false;
    }
    if (!isfinite(min_on_ms) || min_on_ms < 0.0f || min_on_ms > ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
        return false;
    }
    /* Same disjoint rule parse_zone_fields() enforces -- this setter is a
     * second door into the same field (it is reachable without going through
     * a POST body) and must not be the loose one. */
    if (min_on_ms > 0.0f && min_on_ms < ZONE_HEATER_MIN_ON_MS_FLOOR) {
        return false;
    }
    if (!isfinite(min_off_ms) || min_off_ms < 0.0f || min_off_ms > ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
        return false;
    }
    /* Window-vs-min-on relationship, same disjoint rule parse_zone_fields()
     * enforces. This setter is the door backup import comes through, and a
     * backup written before 2026-08-29 can easily carry a 2000 ms window. */
    if (window_ms > 0.0f && window_ms < zone_required_window_ms(min_on_ms)) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->heater_window_ms = window_ms;
    z->heater_min_on_ms = min_on_ms;
    z->heater_min_off_ms = min_off_ms;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_guard_thresholds(uint8_t zone_index, float *out_wrong_dir_window_s,
                                       float *out_wrong_dir_rate_c_per_min, float *out_off_settle_s,
                                       float *out_runaway_rate_c_per_min, float *out_runaway_margin_c,
                                       float *out_drift_period_s, float *out_sensor_fault_debounce_ticks,
                                       float *out_frozen_window_s)
{
    if (!out_wrong_dir_window_s || !out_wrong_dir_rate_c_per_min || !out_off_settle_s ||
        !out_runaway_rate_c_per_min || !out_runaway_margin_c || !out_drift_period_s ||
        !out_sensor_fault_debounce_ticks || !out_frozen_window_s || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_wrong_dir_window_s = z->guard_wrong_dir_window_s;
    *out_wrong_dir_rate_c_per_min = z->guard_wrong_dir_rate_c_per_min;
    *out_off_settle_s = z->guard_off_settle_s;
    *out_runaway_rate_c_per_min = z->guard_runaway_rate_c_per_min;
    *out_runaway_margin_c = z->guard_runaway_margin_c;
    *out_drift_period_s = z->guard_drift_period_s;
    *out_sensor_fault_debounce_ticks = z->guard_sensor_fault_debounce_ticks;
    *out_frozen_window_s = z->guard_frozen_window_s;
    return true;
}

/* The five per-zone guard overrides added in v8 (see zone_cfg_t). Bundled for
 * the same reason the 8 above are: thermal_guard.c reads them as one group
 * when it builds a thermal_guard_cfg_t. Every one keeps the "0 = use the
 * module's named default" convention, so this getter reports the stored value
 * verbatim and the substitution stays where it belongs -- in the module that
 * owns the default.
 *
 * SIGNATURE UNCHANGED by ZONES_CFG_VERSION 9 (2026-08-27, timing profiles --
 * see zones_http.h's own note on this pair): zone_index now resolves to
 * zone_cfg_t::timing_profile first, then to that slot in
 * zones_cfg_t::timing_profiles[], instead of reading the nine fields directly
 * off zone_cfg_t -- neither caller had to change. */
bool zones_config_get_guard_extra(uint8_t zone_index, float *out_progress_duty_min,
                                  float *out_progress_window_s, float *out_drift_hysteresis_c,
                                  float *out_frozen_eps_c, float *out_cross_zone_period_s)
{
    if (!out_progress_duty_min || !out_progress_window_s || !out_drift_hysteresis_c ||
        !out_frozen_eps_c || !out_cross_zone_period_s || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t p = s_zones.cfg.zones[zone_index].timing_profile;
    /* Defensive, not expected in practice: every path that can write
     * s_zones.cfg (POST /api/zones, NVS load via zones_config_json_validate(), a
     * kiln-config import) already bounds timing_profile against
     * timing_profile_count before it is ever stored. A getter must still
     * never read past the array on the strength of that alone. */
    if (p >= s_zones.cfg.timing_profile_count) {
        return false;
    }
    const zone_timing_profile_t *tp = &s_zones.cfg.timing_profiles[p];
    *out_progress_duty_min = tp->guard_progress_duty_min;
    *out_progress_window_s = tp->guard_progress_window_s;
    *out_drift_hysteresis_c = tp->guard_drift_hysteresis_c;
    *out_frozen_eps_c = tp->guard_frozen_eps_c;
    *out_cross_zone_period_s = tp->guard_cross_zone_period_s;
    return true;
}

/* The four per-zone executor overrides added in v8 -- profile_executor.c's
 * side of the same pass. Same 0-means-default convention, and the same
 * "signature unchanged, only the internal lookup moved to timing_profiles[]"
 * note as zones_config_get_guard_extra() above applies here too. */
bool zones_config_get_executor_thresholds(uint8_t zone_index, float *out_bangbang_hysteresis_c,
                                          float *out_cooling_limited_margin_c,
                                          float *out_cooling_limited_hold_s, float *out_ramp_lock_band_c)
{
    if (!out_bangbang_hysteresis_c || !out_cooling_limited_margin_c || !out_cooling_limited_hold_s ||
        !out_ramp_lock_band_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t p = s_zones.cfg.zones[zone_index].timing_profile;
    if (p >= s_zones.cfg.timing_profile_count) {
        return false; /* defensive -- see zones_config_get_guard_extra()'s identical comment */
    }
    const zone_timing_profile_t *tp = &s_zones.cfg.timing_profiles[p];
    *out_bangbang_hysteresis_c = tp->bangbang_hysteresis_c;
    *out_cooling_limited_margin_c = tp->cooling_limited_margin_c;
    *out_cooling_limited_hold_s = tp->cooling_limited_hold_s;
    *out_ramp_lock_band_c = tp->ramp_lock_band_c;
    return true;
}

/* The one global v8 override. Not gated on thermo_count -- the PC link exists
 * whether or not a single zone has been configured. */
bool zones_config_get_pc_link_abort_silence_ms(float *out_ms)
{
    if (!out_ms) {
        return false;
    }
    *out_ms = s_zones.cfg.pc_link_abort_silence_ms;
    return true;
}

/* Bundled setter, same "reject nothing half-written" discipline as every
 * bundled setter above. Each of the 8 fields checked against its own
 * independent bound (matching which ceiling parse_zone_fields() applies to
 * that specific key) -- no cross-field check between any pair of these 8,
 * matching the POST authority's own lack of one. */
bool zones_config_set_guard_thresholds(uint8_t zone_index, float wrong_dir_window_s,
                                       float wrong_dir_rate_c_per_min, float off_settle_s,
                                       float runaway_rate_c_per_min, float runaway_margin_c,
                                       float drift_period_s, float sensor_fault_debounce_ticks,
                                       float frozen_window_s)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(wrong_dir_window_s) || wrong_dir_window_s < 0.0f || wrong_dir_window_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    if (!isfinite(wrong_dir_rate_c_per_min) || wrong_dir_rate_c_per_min < 0.0f ||
        wrong_dir_rate_c_per_min > ZONE_GUARD_RATE_C_PER_MIN_MAX) {
        return false;
    }
    if (!isfinite(off_settle_s) || off_settle_s < 0.0f || off_settle_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    if (!isfinite(runaway_rate_c_per_min) || runaway_rate_c_per_min < 0.0f ||
        runaway_rate_c_per_min > ZONE_GUARD_RATE_C_PER_MIN_MAX) {
        return false;
    }
    if (!isfinite(runaway_margin_c) || runaway_margin_c < 0.0f || runaway_margin_c > ZONE_GUARD_MARGIN_C_MAX) {
        return false;
    }
    if (!isfinite(drift_period_s) || drift_period_s < 0.0f || drift_period_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    if (!isfinite(sensor_fault_debounce_ticks) || sensor_fault_debounce_ticks < 0.0f ||
        sensor_fault_debounce_ticks > ZONE_GUARD_DEBOUNCE_TICKS_MAX) {
        return false;
    }
    if (!isfinite(frozen_window_s) || frozen_window_s < 0.0f || frozen_window_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->guard_wrong_dir_window_s = wrong_dir_window_s;
    z->guard_wrong_dir_rate_c_per_min = wrong_dir_rate_c_per_min;
    z->guard_off_settle_s = off_settle_s;
    z->guard_runaway_rate_c_per_min = runaway_rate_c_per_min;
    z->guard_runaway_margin_c = runaway_margin_c;
    z->guard_drift_period_s = drift_period_s;
    z->guard_sensor_fault_debounce_ticks = sensor_fault_debounce_ticks;
    z->guard_frozen_window_s = frozen_window_s;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_cross_zone_delta(uint8_t zone_index, float *out_max_delta_c)
{
    if (!out_max_delta_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_max_delta_c = s_zones.cfg.zones[zone_index].cross_zone_max_delta_c;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_xzone enforces. 0 is legal (the
 * documented "guard disabled" encoding). */
bool zones_config_set_cross_zone_delta(uint8_t zone_index, float max_delta_c)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(max_delta_c) || max_delta_c < 0.0f || max_delta_c > ZONE_CROSS_ZONE_DELTA_C_MAX) {
        return false;
    }
    s_zones.cfg.zones[zone_index].cross_zone_max_delta_c = max_delta_c;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_model(uint8_t zone_index, float *out_k_dc, float *out_tau_s, float *out_dead_time_s)
{
    if (!out_k_dc || !out_tau_s || !out_dead_time_s || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_k_dc = z->model_k_dc;
    *out_tau_s = z->model_tau_s;
    *out_dead_time_s = z->model_dead_time_s;
    return true;
}

bool zones_config_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    /* Same reject-without-writing-anything discipline as
     * zones_config_set_pid(): a caller handing us one bad number must not
     * end up with two of the three fields updated, because a half-written
     * model is indistinguishable from a whole one to every reader and would
     * feed the feedforward term a gain from one run and a tau from another.
     *
     * Note this deliberately accepts an all-zero triple: that is the
     * documented "no model" encoding, so writing it is how a caller clears a
     * stale model rather than a validation failure. A NEGATIVE value is
     * rejected outright -- a heater with negative static gain, or a plant
     * that responds before it is driven, is a fit that went wrong, not a
     * kiln. */
    if (!isfinite(k_dc) || !isfinite(tau_s) || !isfinite(dead_time_s) || k_dc < 0.0f || tau_s < 0.0f ||
        dead_time_s < 0.0f || k_dc > ZONE_MODEL_K_MAX || tau_s > ZONE_MODEL_TIME_MAX_S ||
        dead_time_s > ZONE_MODEL_TIME_MAX_S) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->model_k_dc = k_dc;
    z->model_tau_s = tau_s;
    z->model_dead_time_s = dead_time_s;
    /* Bumped before the NVS write for the same reason set_pid does it: the
     * model is live for the next control tick regardless of whether it
     * reaches flash, and a feedforward term computed from a stale K while a
     * firing is running is precisely what TODO.md 6A.7's reload path
     * exists to prevent. */
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

float zones_config_apply_cal(uint8_t zone_index, float raw_c)
{
    if (isnan(raw_c) || zone_index >= s_zones.cfg.thermo_count) {
        return raw_c;
    }
    return raw_c + s_zones.cfg.zones[zone_index].cal_offset_c;
}

/* ---- Whole-config export/import for kiln_cfg_store.c (see zones_http.h's
 * doc comment on this pair for the full rationale) -------------------------- */

_Static_assert(sizeof(zones_cfg_t) <= ZONES_CONFIG_BLOB_MAX_SIZE,
               "zones_cfg_t grew past ZONES_CONFIG_BLOB_MAX_SIZE -- widen the macro in "
               "zones_http.h (existing kiln_cfg_store entries keep their old, smaller blob "
               "size until re-saved, same discipline as ZONES_CFG_VERSION migrations)");

size_t zones_config_blob_size(void)
{
    return sizeof(zones_cfg_t);
}

bool zones_config_export_blob(void *out, size_t out_cap)
{
    if (!out || out_cap < sizeof(s_zones.cfg)) {
        return false;
    }
    memcpy(out, &s_zones.cfg, sizeof(s_zones.cfg));
    return true;
}

bool zones_config_import_blob(const void *blob, size_t len, char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap) {
        reason_out[0] = '\0';
    }

    /* Same decoder nvs_load_from() uses -- length-vs-claimed-version check
     * before anything is interpreted, typed per-version conversion (never a
     * memcpy of one struct shape over another), zones_config_json_validate(), and a
     * CRC check on the current-version path. This blob may have been saved
     * years ago by older firmware under looser bounds (kiln_cfg_store.c), so
     * it gets exactly the same scrutiny a blob read off flash does -- no
     * separate, looser path for "this one came from a kiln config slot
     * instead of the live NVS key." */
    zones_cfg_t cand;
    const char *reason = "";
    zones_decode_result_t result = zones_config_json_decode_blob(blob, len, &cand, &reason);
    if (result != ZONES_DECODE_OK) {
        if (reason_out && reason_cap) {
            snprintf(reason_out, reason_cap, "%s", reason);
        }
        return false;
    }

    /* Pass 1 for settings_source: zones_config_json_decode_blob()/zones_config_json_validate()
     * do not check for an inheritance cycle (they're shared with the LOAD
     * path, which must collapse rather than reject one -- see
     * zones_config_json_normalize_settings_source_cycles()'s comment). The import path is
     * different: there is a live client on the other end of this call
     * (backup_http.c's importer) who can be handed a clear reason, so this
     * is refused HERE, before the commit point below, matching this file's
     * own two-pass-import discipline (reject in pass 1, never fail
     * mid-commit) rather than silently rewriting a hand-edited backup's
     * cycle to Custom underneath the operator. */
    {
    uint8_t import_thermo_count = cand.thermo_count > MAX31856_CHANNEL_COUNT ? MAX31856_CHANNEL_COUNT
                                                                             : cand.thermo_count;
    for (uint8_t i = 0; i < import_thermo_count; i++) {
        if (zones_config_json_settings_source_chain_has_cycle(cand.zones, i, import_thermo_count)) {
            if (reason_out && reason_cap) {
                snprintf(reason_out, reason_cap,
                         "zone %u's settings_source forms an inheritance cycle", (unsigned)i);
            }
            return false;
        }
    }
    }

    /* Commit point -- everything above only touched `cand`, a local scratch
     * copy; nothing has been written to s_zones or NVS until this line, so
     * any rejection above (bad version, bad size, any one field out of
     * range) leaves the live config completely untouched. Same
     * all-or-nothing discipline as zones_post_handler()'s own commit
     * point. */
    s_zones.cfg = cand;
    s_zones_config_valid = true;
    s_config_generation++;
    esp_err_t err = nvs_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save after kiln-config apply failed: %s -- config applied live but "
                      "will not survive a reboot",
                 esp_err_to_name(err));
    }
    return true;
}

/* ---- HTTP handlers --------------------------------------------------------- */

/* Same content-negotiation shape as diagnostics_http.c's send_gz_page():
 * web_client_accepts_gzip() covers the "no Accept-Encoding header" (legal,
 * served gzip per RFC 9110 s12.5.3) and "header present but excludes gzip"
 * (406, since this server keeps no uncompressed copy) cases; see
 * web_encoding.h for the full rationale. */
static esp_err_t page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "zones_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)zones_page_html_gz_start,
                           (size_t)(zones_page_html_gz_end - zones_page_html_gz_start));
}

/* Same escaping convention as wifi_provision_http.c's json_escape -- a zone
 * name came from a POST body at some point, so it's untrusted-ish. */
static void json_escape(const char *src, char *out, size_t out_cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < out_cap; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 3 >= out_cap) {
                break;
            }
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

static esp_err_t safety_config_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "safety_config_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)safety_config_page_html_gz_start,
                           (size_t)(safety_config_page_html_gz_end - safety_config_page_html_gz_start));
}

static esp_err_t zones_get_handler(httpd_req_t *req)
{
    /* HEAP, not stack -- this was easily the largest single transient
     * buffer on the httpd_worker task's request path (dashboard_http.c's
     * profile_exec/control/autotune_matrix/profile_plan handlers all share
     * this same task and stack; httpd_worker was measured at 64 bytes free
     * of 8192 live). At 5760 bytes this one buffer alone was more than 70%
     * of the entire 8192-byte task stack. Freed on every return path
     * (success and truncated). */
    const size_t json_cap = 5760; /* 5632 -> 5760 (2026-08-30, same-day follow-up,
                      * ZONES_CFG_VERSION 10->11): coupling_coeff/
                      * coupling_neighbor_zone (2 keys) replaced by
                      * MAX31856_CHANNEL_COUNT indexed coupling_c%u keys (3
                      * keys, ~20 bytes each worst case) -- net +1 key/zone,
                      * ~60 bytes across 3 zones, rounded up generously.
                      * 5120 -> 5632 (2026-08-30, PID_EXPANSION_PLAN.md Phase 4):
                      * four new per-zone keys/values (fuzzy_strength_pct,
                      * coupling_coeff, coupling_neighbor_zone, settings_source
                      * -- ~130 bytes a zone at worst, MAX31856_CHANNEL_COUNT
                      * zones), comfortably inside this bump.
                      * 4608 -> 5120 (2026-08-27+2, Tasks 1/2/3): one
                      * top-level safety_wiring object (~110 bytes) plus
                      * ct_warn_mask (~20 bytes), and two new per-zone keys
                      * (normal_current_measured/normal_current_a, ~50 bytes
                      * a zone) -- comfortably inside the ~500 bytes of
                      * headroom this bumps by.
                      * 4352 -> 4608 (2026-08-27, ZONES_CFG_VERSION 8->9,
                      * timing profiles): the nine v8 override keys/values move
                      * OFF each zone object and onto a new top-level
                      * timing_profiles array, one object per profile
                      * (name + the same nine keys/values that used to be
                      * inline on every zone, up to MAX31856_CHANNEL_COUNT of
                      * them) -- roughly what the per-zone removal frees up,
                      * since worst case (every zone its own profile) is the
                      * same MAX31856_CHANNEL_COUNT repeats of the same nine
                      * keys either way. Each zone object shrinks by those nine
                      * keys/values (~250 bytes) and grows by one
                      * "timing_profile":%u (~20 bytes); the net is a modest
                      * increase, not a decrease, because the new
                      * timing_profiles array also carries a name string per
                      * profile that the old inline fields never had. Sized
                      * with headroom rather than computed exactly, same
                      * discipline as every prior bump of this buffer.
                      * 2816 -> 4352 (2026-08-27) with the nine v8 per-zone
                      * overrides plus one global: nine float keys a zone,
                      * whose names alone run ~250 bytes before any values.
                      * 2688 -> 2816 (2026-08-27) with ct_mask: one small integer
                      * key/value per zone, well under the 128 bytes added.
                      * 1024 -> 1536 with heater_window_ms/min_on_ms/min_off_ms,
                      * 1536 -> 1792 with cross_zone_max_delta_c,
                      * 1792 -> 2048 with the three plant-model fields (their
                      * key names alone are ~50 bytes a zone before values),
                      * 2048 -> 2560 with the 8 guard-threshold overrides,
                      * 2560 -> 2688 with tc_type/safety_tc_type (2026-08-21):
                      * one small integer per zone plus one top-level field.
                      * thermo_mask (TODO.md 10.8) added ~20 bytes/zone --
                      * left inside the existing 2560 headroom rather than
                      * bumped again, MAX31856_CHANNEL_COUNT zones' worth of
                      * one small integer key is nowhere near what's left. */
    char *json = heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        ESP_LOGE(TAG, "GET /api/zones: malloc(%u) failed for the response buffer", (unsigned)json_cap);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
    size_t o = 0;
    int n;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(json + o, json_cap - o, __VA_ARGS__);                                   \
        if (n < 0 || (size_t)n >= json_cap - o) {                                             \
            goto truncated;                                                                            \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    /* Task 3 (2026-08-27+2): the safety thermocouple/relay's LIVE reading,
     * read-only -- computed BEFORE the APPEND below so its values are
     * ordinary args, same as every other field here. See
     * zone_safety_wiring_t's doc comment for why UNSET (tc_temp_valid
     * false) renders distinct from a real 0/false rather than being folded
     * into the same field. Task 2 (2026-08-27+2): ct_warn_mask, computed the
     * same way -- bit N-1 = zone N is currently commanded on and its live
     * current mismatches its Task 1 measured normal, a WARNING never a trip
     * (zones_ct_mapping_mismatch()'s doc comment); 0 for any zone whose
     * normal was never measured. */
    zone_safety_wiring_t safety_wiring;
    zones_get_safety_wiring(&safety_wiring);
    uint8_t ct_warn_mask = zones_ct_mapping_warn_mask();

    APPEND("{\"thermo_count\":%u,\"relay_count\":%u,\"max_simultaneous_relays\":%u,"
           "\"continue_on_zone_trip\":%s,\"safety_tc_type\":%u,"
           "\"pc_link_abort_silence_ms\":%.0f,"
           /* 2026-08-27+1 (owner request: name relays that are NOT in any
            * zone): relay_zone_owned_mask lets the page tell, without its
            * own recompute, which of relay_names[] below it should render
            * editable -- bit N-1 set iff relay N is currently claimed by
            * SOME zone (zone_owned_relay_mask(), same rule
            * rules_task.c/rules_http.c already enforce server-side). The
            * page still recomputes this live client-side too, from its own
            * in-progress checkbox state -- see zones_page.html's
            * clientOwnedRelayMask() -- since an operator can retick a relay
            * checkbox after this GET without reloading; this field is what a
            * fresh load (or a consumer that isn't this page) sees.
            * relay_names is DENSE over KILN_IO_RELAY_COUNT, index r == bit r
            * (relay r+1 on the wire), always emitted regardless of
            * relay_count or ownership -- a name persists whether or not its
            * relay is in use today, same "always emit, let the page decide
            * what to show" convention as zone_cfg_t's model_k_dc/etc above. */
           "\"relay_zone_owned_mask\":%u,"
           "\"safety_wiring\":{\"link_up\":%s,\"tc_temp_valid\":%s,\"tc_temp_c\":%.1f,"
           "\"tc_fault\":%u,\"relay_energized\":%s},"
           "\"ct_warn_mask\":%u,"
           "\"relay_names\":[",
           s_zones.cfg.thermo_count, s_zones.cfg.relay_count, s_zones.cfg.max_simultaneous_relays,
           s_zones.cfg.continue_on_zone_trip ? "true" : "false", s_zones.cfg.safety_tc_type,
           (double)s_zones.cfg.pc_link_abort_silence_ms, zone_owned_relay_mask(&s_zones.cfg),
           safety_wiring.link_up ? "true" : "false", safety_wiring.tc_temp_valid ? "true" : "false",
           (double)safety_wiring.tc_temp_c, safety_wiring.tc_fault, safety_wiring.relay_energized ? "true" : "false",
           ct_warn_mask);

    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        char rn_escaped[RELAY_NAME_MAX_LEN * 2 + 1];
        json_escape(s_relay_names.cfg.names[r], rn_escaped, sizeof(rn_escaped));
        APPEND("%s\"%s\"", r == 0 ? "" : ",", rn_escaped);
    }
    APPEND("],\"timing_profiles\":[");
    /* 2026-08-27 (ZONES_CFG_VERSION 8->9): exactly timing_profile_count
     * entries, never padded out to MAX31856_CHANNEL_COUNT -- the page derives
     * "how many profiles exist" from this array's length (the same way it
     * already derives "how many zones exist" from thermo_count, not from
     * zones[]'s fixed capacity), and a POST replays that same dense
     * 0..count-1 range back as tp0_name.. (see zones_post_handler()'s own
     * comment on that loop). */
    for (uint8_t p = 0; p < s_zones.cfg.timing_profile_count; p++) {
        const zone_timing_profile_t *tp = &s_zones.cfg.timing_profiles[p];
        char tp_name_escaped[TIMING_PROFILE_NAME_MAX_LEN * 2 + 1];
        json_escape(tp->name, tp_name_escaped, sizeof(tp_name_escaped));
        APPEND(
            "%s{\"index\":%u,\"name\":\"%s\","
            "\"guard_progress_duty_min\":%.3f,\"guard_progress_window_s\":%.1f,"
            "\"guard_drift_hysteresis_c\":%.1f,\"guard_frozen_eps_c\":%.3f,"
            "\"guard_cross_zone_period_s\":%.1f,\"bangbang_hysteresis_c\":%.1f,"
            "\"cooling_limited_margin_c\":%.1f,\"cooling_limited_hold_s\":%.1f,"
            "\"ramp_lock_band_c\":%.1f}",
            p == 0 ? "" : ",", p, tp_name_escaped,
            (double)tp->guard_progress_duty_min, (double)tp->guard_progress_window_s,
            (double)tp->guard_drift_hysteresis_c, (double)tp->guard_frozen_eps_c,
            (double)tp->guard_cross_zone_period_s, (double)tp->bangbang_hysteresis_c,
            (double)tp->cooling_limited_margin_c, (double)tp->cooling_limited_hold_s,
            (double)tp->ramp_lock_band_c);
    }
    APPEND("],\"zones\":[");
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        const zone_cfg_t *z = &s_zones.cfg.zones[i];
        char name_escaped[ZONE_NAME_MAX_LEN * 2 + 1];
        json_escape(z->name, name_escaped, sizeof(name_escaped));
        /* Task 1 (2026-08-27+2): this zone's measured normal current, if
         * any -- read-only, never round-tripped through POST (it is
         * measured data, not an operator-entered field; see
         * zones_config_get_normal_current()'s doc comment). */
        float normal_a = 0.0f;
        bool normal_measured = false;
        zones_config_get_normal_current(i, &normal_a, &normal_measured);
        APPEND(
            "%s{\"index\":%u,\"name\":\"%s\",\"relay_mask\":%u,\"thermo_mask\":%u,\"cal_offset_c\":%.3f,"
            "\"pid_kp\":%.4f,\"pid_ki\":%.4f,\"pid_kd\":%.4f,\"max_ramp_c_per_hr\":%.2f,"
            "\"sanity_rate_c_per_min\":%.3f,\"control_mode\":%u,\"max_temp_c\":%.1f,"
            "\"min_temp_c\":%.1f,\"heater_window_ms\":%.0f,\"heater_min_on_ms\":%.0f,"
            "\"heater_min_off_ms\":%.0f,"
            "\"guard_wrong_dir_window_s\":%.1f,\"guard_wrong_dir_rate_c_per_min\":%.3f,"
            "\"guard_off_settle_s\":%.1f,\"guard_runaway_rate_c_per_min\":%.3f,"
            "\"guard_runaway_margin_c\":%.1f,\"guard_drift_period_s\":%.1f,"
            "\"guard_sensor_fault_debounce_ticks\":%.0f,\"guard_frozen_window_s\":%.1f,"
            "\"cross_zone_max_delta_c\":%.1f,"
            /* Emitted for every zone whether or not a model exists -- an
             * absent key and a zero would mean the same thing to a client,
             * and always emitting keeps the page's read-back-and-repost
             * round-trip (see the POST side) from depending on which zones
             * happen to have been autotuned. %.4f on K because a small-gain
             * zone's fit can land in the fractional range and the
             * feedforward divides by it. */
            "\"model_k_dc\":%.4f,\"model_tau_s\":%.1f,\"model_dead_time_s\":%.1f,"
            /* tc_type is CONFIG, not a live reading, so it deliberately does
             * NOT go anywhere near kc-live-value on the page -- see
             * zones_page.html's rendering of this field. */
            "\"tc_type\":%u,\"ct_mask\":%u,"
            /* 2026-08-27 (ZONES_CFG_VERSION 8->9): replaces the nine v8
             * override fields that used to be inline here -- see this zone's
             * chosen profile in the top-level timing_profiles array above.
             * Always emitted (even for a zone past thermo_count), same
             * round-trip reasoning as every other always-emitted field here:
             * the page reads this back and reposts it. */
            /* PID_EXPANSION_PLAN.md Phase 4 (2026-08-30): always emitted for
             * every zone, same "read-back-and-repost round-trip" reasoning as
             * model_k_dc/etc above -- a page that reads this back and posts
             * it straight through untouched must never see an absent key
             * mean something different from a zero. */
            "\"timing_profile\":%u,\"normal_current_measured\":%s,\"normal_current_a\":%.3f,"
            "\"fuzzy_strength_pct\":%.2f,",
            i == 0 ? "" : ",", i, name_escaped, z->relay_mask, z->thermo_mask, (double)z->cal_offset_c,
            (double)z->pid_kp, (double)z->pid_ki, (double)z->pid_kd, (double)z->max_ramp_c_per_hr,
            (double)z->sanity_rate_c_per_min, z->control_mode, (double)z->max_temp_c,
            (double)z->min_temp_c, (double)z->heater_window_ms, (double)z->heater_min_on_ms,
            (double)z->heater_min_off_ms,
            (double)z->guard_wrong_dir_window_s, (double)z->guard_wrong_dir_rate_c_per_min,
            (double)z->guard_off_settle_s, (double)z->guard_runaway_rate_c_per_min,
            (double)z->guard_runaway_margin_c, (double)z->guard_drift_period_s,
            (double)z->guard_sensor_fault_debounce_ticks, (double)z->guard_frozen_window_s,
            (double)z->cross_zone_max_delta_c, (double)z->model_k_dc,
            (double)z->model_tau_s, (double)z->model_dead_time_s, z->tc_type, z->ct_mask,
            z->timing_profile, normal_measured ? "true" : "false", (double)normal_a,
            (double)z->fuzzy_strength_pct);
        /* 2026-08-30 (ZONES_CFG_VERSION 10->11): the coupling row, one
         * indexed key per cell (z%u_coupling_c%u is the matching POST-side
         * wire name -- see parse_zone_fields()) rather than a JSON array, so
         * the same key-per-value convention this whole object already uses
         * for every other field extends here too, and a diff between two
         * saved configs stays a per-key diff rather than needing array-aware
         * tooling. Always emitted for every cell including the diagonal
         * (always 0) -- same always-emit, read-back-and-repost reasoning as
         * model_k_dc/timing_profile above. */
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            APPEND("\"coupling_c%u\":%.4f,", j, (double)z->coupling_coeff[j]);
        }
        APPEND("\"settings_source\":%u}", z->settings_source);
    }
    APPEND("]}");

#undef APPEND

    httpd_resp_set_type(req, "application/json");
    {
        esp_err_t ret = httpd_resp_send(req, json, o);
        free(json);
        return ret;
    }

    /* Reached only if `json` is too small for the config it holds. The old
     * behaviour was to send what had been written so far, which is a truncated
     * JSON document: the page's fetch throws on it, and the operator sees a
     * settings page stuck on "Loading" with no idea their zone config is fine
     * and only the response was too big. A 500 with a valid body at least says
     * what happened. Sizing `json` is the actual fix; this is the guard that
     * makes an undersized buffer visible instead of silent. */
truncated:
    ESP_LOGE(TAG, "GET /api/zones did not fit in %u bytes -- raise the buffer", (unsigned)json_cap);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    {
        esp_err_t ret = httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"zone config did not fit in the response "
                                  "buffer -- this is a firmware sizing bug, not a bad configuration\"}");
        free(json);
        return ret;
    }
}

/* ---- POST /api/zones ------------------------------------------------------
 * Whole-page submit; every field validated into a scratch struct before
 * anything is written to the in-RAM copy or NVS -- reject cleanly, never
 * partially apply, same discipline as every other untrusted-input boundary
 * in this codebase. */

/* Parses and validates zone index i's 7 fields from body into *z. thermo_count
 * is the just-parsed candidate count (not yet committed) -- zones at or past
 * it are still parsed (so a round-trip GET/POST of an unused zone block
 * doesn't need special-casing on the page) but not checked against
 * relay_count, since a shrunk relay_count would otherwise reject fields the
 * page never showed for a zone the submission isn't even claiming to use. */
static bool parse_zone_fields(const char *body, uint8_t i, uint8_t thermo_count, uint8_t relay_count,
                              uint8_t timing_profile_count, const zone_cfg_t *current_z, zone_cfg_t *z,
                              const char **err_reason)
{
    char key[24]; /* 16 -> 24 when the 8 guard-threshold override keys were
                   * added -- "z0_wrongdirwindow" is the longest at 18 chars
                   * plus terminator. */

    snprintf(key, sizeof(key), "z%u_name", i);
    char name[ZONE_NAME_MAX_LEN + 1];
    int name_len = http_form_find_field(body, key, name, sizeof(name));
    if (name_len == -2) {
        *err_reason = "zone name too long";
        return false;
    }
    if (name_len < 0) {
        /* Omitted: preserve the currently-stored name rather than blanking
         * it. In practice zones_page.html always sends z%u_name for every
         * zone it renders (i < thermo_count), so this only matters for a
         * slot the page never showed -- see the i >= thermo_count preserve
         * block below, which this keeps consistent with. */
        strncpy(z->name, current_z->name, ZONE_NAME_MAX_LEN);
        z->name[ZONE_NAME_MAX_LEN] = '\0';
    } else {
        strncpy(z->name, name, ZONE_NAME_MAX_LEN);
        z->name[ZONE_NAME_MAX_LEN] = '\0';
    }

    /* 2026-08-21, TODO.md owner-report item 1/4: this channel's MAX31856
     * thermocouple type. Parsed BEFORE the `i >= thermo_count` early return
     * below, deliberately unlike relay_mask/thermo_mask/every other zone
     * field -- this is per-CHANNEL hardware state (see zone_cfg_t::tc_type's
     * comment), not per-zone, so a channel physically present but not
     * currently claimed by any configured zone (thermo_count set lower than
     * the physical channel count) must still keep its own real type across
     * an ordinary page save rather than being silently zeroed to THERMO_TC_B
     * the moment it falls outside thermo_count's range -- which is exactly
     * what would happen if this fell after the early return, since tmp is
     * zero-initialized by the caller and 0 is a real, different, wrong type
     * here (see ZONES_CFG_VERSION's migration comment for the identical
     * reasoning).
     *
     * OPTIONAL, falling back to current_z->tc_type (the live value) rather
     * than to a fixed default when omitted -- there is no default
     * thermocouple type that could possibly be correct for a channel this
     * submission never mentioned, unlike thermo_mask's "zone i reads channel
     * i" legacy mapping just below. zones_page.html always sends this field
     * (see its JS), so the fallback matters only for a client that predates
     * thermocouple-type selection entirely (pc_tools/MCP, the test
     * harnesses).
     *
     * Present-but-out-of-range is still an error -- "in range" is
     * ZONE_TC_TYPE_MAX_REAL (0-7, the eight real thermocouple types), not
     * MAX31856_configure()'s wider 0-0x0F: see that macro's comment for why
     * this operator-facing endpoint is deliberately stricter than the raw
     * UART debug path. */
    snprintf(key, sizeof(key), "z%u_tctype", i);
    {
        char probe[8];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            uint8_t tc_type_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, ZONE_TC_TYPE_MAX_REAL, &tc_type_raw)) {
                *err_reason = "zone thermocouple type must be a real thermocouple type (0-7: "
                              "B/E/J/K/N/R/S/T), not a voltage-input mode";
                return false;
            }
            z->tc_type = tc_type_raw;
        } else {
            z->tc_type = current_z->tc_type;
        }
    }

    if (i >= thermo_count) {
        /* DEFECT FIX (found by black-box testing against the live board):
         * this slot is past the just-submitted thermo_count, so
         * zones_page.html's whole-page submit never rendered UI for it and
         * carries no data for relay_mask/thermo_mask/cal/PID/ramp/guard
         * thresholds/model/etc. The caller's `z` starts zero-initialized
         * (zones_post_handler()'s memset(&tmp, 0, ...)), so returning here
         * unconditionally used to leave every one of those fields at 0 --
         * an accepted 200 OK POST that silently zeroed state the operator
         * never asked to change (reproduced live: thermo_count=1 zeroed
         * zone 1 and zone 2's thermo_mask on an ordinary re-save that only
         * replayed what GET had just reported).
         *
         * Fix: preserve the currently-stored zone_cfg_t for this slot
         * instead of leaving it zeroed. z->name and z->tc_type are already
         * set above (name/tc_type each have their own omit-means-preserve
         * handling), so save and restore just those two fields around a
         * bulk copy of everything else from current_z -- the live value,
         * same object z%u_tctype's fallback already reads from just above.
         * A future submission that raises thermo_count back up (or a
         * client that explicitly names this slot's fields once one exists)
         * still goes through the normal validated path below, unaffected --
         * this branch only runs for a slot outside today's thermo_count. */
        char preserved_name[ZONE_NAME_MAX_LEN + 1];
        strncpy(preserved_name, z->name, sizeof(preserved_name));
        preserved_name[ZONE_NAME_MAX_LEN] = '\0';
        uint8_t preserved_tc_type = z->tc_type;
        *z = *current_z;
        strncpy(z->name, preserved_name, ZONE_NAME_MAX_LEN);
        z->name[ZONE_NAME_MAX_LEN] = '\0';
        z->tc_type = preserved_tc_type;
        return true;
    }

    snprintf(key, sizeof(key), "z%u_relay_mask", i);
    uint8_t relay_mask_raw;
    if (!zones_config_json_parse_u8_field(body, key, 0, 0xFF, &relay_mask_raw)) {
        *err_reason = "zone relay_mask missing or invalid";
        return false;
    }
    uint8_t valid_bits = relay_count >= 8 ? 0xFF : (uint8_t)((1u << relay_count) - 1u);
    if ((relay_mask_raw & ~valid_bits) != 0) {
        *err_reason = "zone relay_mask references an unconfigured relay";
        return false;
    }
    z->relay_mask = relay_mask_raw;

    /* 2026-08-27 (ZONES_CFG_VERSION 8->9, owner's request: "assign the zones
     * to them"): which timing_profiles[] slot this zone uses. REQUIRED, unlike
     * the nine fields it replaces (which were each individually OPTIONAL) --
     * there is no legacy client to preserve backward compatibility for here,
     * since no firmware before this pass ever had a "timing profile" concept
     * to send or omit. Bounded against timing_profile_count, the number of
     * profiles THIS submission itself defines (parsed by zones_post_handler()
     * before this per-zone loop runs -- see its own comment), not against
     * some fixed ceiling: a submission that only defines two profiles must not
     * let a zone reference a third one that doesn't exist in it. */
    snprintf(key, sizeof(key), "z%u_timingprofile", i);
    if (!zones_config_json_parse_u8_field(body, key, 0, timing_profile_count - 1, &z->timing_profile)) {
        *err_reason = "zone timing_profile missing or references a profile that doesn't exist "
                      "in this submission";
        return false;
    }

    /* TODO.md 10.8. Deliberately NOT validated/defaulted the same way as
     * z%u_xzone/z%u_k above (present-but-omit-means-0/disabled): omitting
     * this field must mean "this client doesn't know about multi-thermo,
     * keep controlling off the channel this zone always used", not "no
     * thermocouple assigned". zones_page.html doesn't send this field yet
     * (TODO.md 10.8's open item -- see the getter's header comment), and if
     * an absent field defaulted to 0 here, saving that page's form today
     * would silently blind every zone on the very next ordinary settings
     * save. bit i is that legacy mapping (zone i <-> channel i), same as
     * migrate_zones_cfg_v1_to_current() falls back to for an already-saved
     * blob that predates this field entirely -- one fallback rule for both
     * an old blob and a client that just doesn't send the key.
     *
     * A client that DOES know this field and sends it explicitly -- including
     * an explicit 0, deliberately clearing a zone's thermocouple -- is
     * honoured exactly as sent; present-but-out-of-range is still an error,
     * same discipline as relay_mask above. */
    snprintf(key, sizeof(key), "z%u_thermo_mask", i);
    {
        char probe[8];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            uint8_t thermo_mask_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, 0xFF, &thermo_mask_raw)) {
                *err_reason = "zone thermo_mask missing or invalid";
                return false;
            }
            uint8_t valid_thermo_bits =
                thermo_count >= 8 ? 0xFF : (uint8_t)((1u << thermo_count) - 1u);
            if ((thermo_mask_raw & ~valid_thermo_bits) != 0) {
                *err_reason = "zone thermo_mask references an unconfigured thermocouple channel";
                return false;
            }
            z->thermo_mask = thermo_mask_raw;
        } else {
            z->thermo_mask = (uint8_t)(1u << i);
        }
    }

    /* 2026-08-27: purely informational (see ZONES_CFG_VERSION's 5->6
     * comment), so unlike thermo_mask above there is no legacy single-
     * channel mapping to preserve -- an omitted field is simply "no CT probe
     * mapped to this zone", the same safe-zero default a brand-new zone
     * already gets. Still validated against ZONE_CT_CHANNEL_COUNT (a fixed
     * hardware count, not relay_count/thermo_count) when present. */
    snprintf(key, sizeof(key), "z%u_ct_mask", i);
    {
        char probe[8];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            uint8_t ct_mask_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, 0xFF, &ct_mask_raw)) {
                *err_reason = "zone ct_mask missing or invalid";
                return false;
            }
            uint8_t valid_ct_bits = (uint8_t)((1u << ZONE_CT_CHANNEL_COUNT) - 1u);
            if ((ct_mask_raw & ~valid_ct_bits) != 0) {
                *err_reason = "zone ct_mask references an unconfigured current-sense channel";
                return false;
            }
            z->ct_mask = ct_mask_raw;
        } else {
            z->ct_mask = 0;
        }
    }

    /* Sane numeric bounds -- firmware sanity bounds against a malformed/
     * typo'd submission, not real kiln-safety limits (that's the feasibility
     * check in profiles_http.c, on the ramp-rate side). Kp/Ki/Kd have no
     * natural physical bound, so 0..1000 is just generous headroom over
     * anything a real PID loop on this hardware would ever be tuned to. */
    snprintf(key, sizeof(key), "z%u_cal", i);
    if (!zones_config_json_parse_float_field(body, key, ZONE_CAL_OFFSET_MIN_C, ZONE_CAL_OFFSET_MAX_C, &z->cal_offset_c)) {
        *err_reason = "zone cal_offset_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_kp", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, 1000.0f, &z->pid_kp)) {
        *err_reason = "zone pid_kp missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_ki", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, 1000.0f, &z->pid_ki)) {
        *err_reason = "zone pid_ki missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_kd", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, 1000.0f, &z->pid_kd)) {
        *err_reason = "zone pid_kd missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_ramp", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MAX_RAMP_C_PER_HR_MAX, &z->max_ramp_c_per_hr)) {
        *err_reason = "zone max_ramp_c_per_hr missing or out of range";
        return false;
    }
    /* 0 = "never configured" (profile_executor.c substitutes its own
     * default); 20 C/min is a generous ceiling -- well above anything this
     * board's bang-bang control could plausibly produce, just a sanity bound
     * against a typo. */
    snprintf(key, sizeof(key), "z%u_sanity", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_SANITY_RATE_MAX_C_PER_MIN, &z->sanity_rate_c_per_min)) {
        *err_reason = "zone sanity_rate_c_per_min missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_mode", i);
    uint8_t mode_raw;
    if (!zones_config_json_parse_u8_field(body, key, 0, (long)ZONE_CONTROL_MODE_PID_FUZZY, &mode_raw)) {
        *err_reason = "zone control_mode missing or out of range (0-3)";
        return false;
    }
    z->control_mode = mode_raw;
    /* 1400C ceiling matches PROFILE_TARGET_C_MAX (profiles_http.c) -- a
     * guard 5 limit tighter than what a profile could ever request would be
     * a contradiction between the two checks. */
    snprintf(key, sizeof(key), "z%u_maxtemp", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MAX_TEMP_C_MAX, &z->max_temp_c)) {
        *err_reason = "zone max_temp_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_mintemp", i);
    if (!zones_config_json_parse_float_field(body, key, ZONE_MIN_TEMP_C_MIN, ZONE_MIN_TEMP_C_MAX, &z->min_temp_c)) {
        *err_reason = "zone min_temp_c missing or out of range";
        return false;
    }
    /* 0 = "not configured" (caller substitutes PROFILE_EXECUTOR_DEFAULT_*_MS,
     * same convention as sanity_rate_c_per_min above). 600000ms (10min) is a
     * generous upper bound on window_ms -- well past any window that would
     * still make sense against a kiln's thermal time constant; 60000ms on
     * min_on/min_off is the same generosity relative to window_ms's own
     * range. */
    snprintf(key, sizeof(key), "z%u_window", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_HEATER_WINDOW_MS_MAX, &z->heater_window_ms)) {
        *err_reason = "zone heater_window_ms missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_minon", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_HEATER_MIN_ON_OFF_MS_MAX, &z->heater_min_on_ms)) {
        *err_reason = "zone heater_min_on_ms missing or out of range";
        return false;
    }
    /* The one heater field with a LOWER bound too, and the only reason it
     * cannot just be another zones_config_json_parse_float_field() range: the accepted set is
     * disjoint (0, or >= the floor), not an interval. See
     * ZONE_HEATER_MIN_ON_MS_FLOOR in zones_http.h for why this is refused
     * rather than quietly raised. */
    if (z->heater_min_on_ms > 0.0f && z->heater_min_on_ms < ZONE_HEATER_MIN_ON_MS_FLOOR) {
        *err_reason = "zone heater_min_on_ms below the 10000 ms relay-protection floor "
                      "(use 0 for the firmware default)";
        return false;
    }
    /* The window-vs-min-on RELATIONSHIP (2026-08-29). Checked here, after
     * both fields are parsed, because it is the only check in this function
     * that needs two of them at once. A window that passes its own range but
     * fails this cannot render a fractional duty at all -- see
     * ZONE_HEATER_WINDOW_MIN_MULTIPLE in zones_http.h. */
    if (z->heater_window_ms > 0.0f && z->heater_window_ms < zone_required_window_ms(z->heater_min_on_ms)) {
        *err_reason = "zone heater_window_ms too short for its heater_min_on_ms: the window must be at "
                      "least 3x the minimum on-time or no fractional duty can be rendered "
                      "(use 0 for the firmware default)";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_minoff", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_HEATER_MIN_ON_OFF_MS_MAX, &z->heater_min_off_ms)) {
        *err_reason = "zone heater_min_off_ms missing or out of range";
        return false;
    }
    /* TODO.md 6A.3's remaining named guard thresholds. OPTIONAL, same reason
     * z%u_xzone below is: a submission that omits one leaves the
     * corresponding firmware default in force (z is zero-initialized by the
     * caller, and 0 is thermal_guard.c's own "substitute the default" value
     * for every one of these -- unlike z%u_xzone, omitting one of these does
     * NOT disable its guard). Bounds are generous sanity ceilings against a
     * typo, not real per-field tuning limits: rates 0-20C/min matches
     * z%u_sanity's own ceiling, windows/periods 0-7200s (2h) covers any
     * kiln's plausible time constant, debounce ticks 0-100, margin 0-500C. */
    snprintf(key, sizeof(key), "z%u_wrongdirwindow", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_wrong_dir_window_s)) {
                *err_reason = "zone guard_wrong_dir_window_s out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_wrongdirrate", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_RATE_C_PER_MIN_MAX, &z->guard_wrong_dir_rate_c_per_min)) {
                *err_reason = "zone guard_wrong_dir_rate_c_per_min out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_offsettle", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_off_settle_s)) {
                *err_reason = "zone guard_off_settle_s out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_runawayrate", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_RATE_C_PER_MIN_MAX, &z->guard_runaway_rate_c_per_min)) {
                *err_reason = "zone guard_runaway_rate_c_per_min out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_runawaymargin", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_MARGIN_C_MAX, &z->guard_runaway_margin_c)) {
                *err_reason = "zone guard_runaway_margin_c out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_driftperiod", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_drift_period_s)) {
                *err_reason = "zone guard_drift_period_s out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_debounce", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_DEBOUNCE_TICKS_MAX, &z->guard_sensor_fault_debounce_ticks)) {
                *err_reason = "zone guard_sensor_fault_debounce_ticks out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_frozenwindow", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_frozen_window_s)) {
                *err_reason = "zone guard_frozen_window_s out of range";
                return false;
            }
        }
    }
    /* The nine v8 overrides that used to be parsed inline here now live on
     * the timing profile this zone points at -- see z%u_timingprofile above
     * and zones_config_json_parse_timing_profile_fields() (tp%u_progressduty..tp%u_ramplock),
     * parsed once per PROFILE rather than once per zone. */
    /* Guard 8. OPTIONAL, unlike every field above: a submission that omits
     * it means "leave the guard disabled" (z is zero-initialized by the
     * caller), so older clients -- the MCP/pc_tools path and the test
     * harnesses that post the original 14 fields -- keep working unchanged
     * instead of being rejected by a field they've never heard of. Such a
     * client does clear a previously saved threshold, which is the same
     * whole-page-submit semantics every other field already has. Present
     * but malformed is still an error. 1000C is a sanity bound only; a real
     * threshold comes from a measured cross-gain matrix. */
    snprintf(key, sizeof(key), "z%u_xzone", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_CROSS_ZONE_DELTA_C_MAX, &z->cross_zone_max_delta_c)) {
                *err_reason = "zone cross_zone_max_delta_c out of range";
                return false;
            }
        }
    }
    /* The identified plant model (TODO.md 6A.4 -> 6A.2's feedforward).
     * OPTIONAL, for exactly the same reason z%u_xzone above is: a client
     * that predates these fields -- pc_tools/MCP, the test harnesses --
     * must not start getting 400s for a field it has never heard of.
     *
     * The whole-page-submit semantics have real teeth here, though. These
     * numbers are NOT typed by an operator; they are measured by a
     * multi-hour step test. A client that omits them silently deletes that
     * measurement, because z is zero-initialized by the caller and zero is
     * the "no model" encoding. That is the established behaviour of this
     * endpoint and is left as-is rather than special-cased into a
     * merge-on-omit, which would make this one field group behave unlike
     * every other one on the page -- but it is why zones_page.html reads
     * these back from GET and posts them straight through untouched, and
     * why anything else driving this endpoint must do the same.
     *
     * Bounds are ZONE_MODEL_*_MAX so this path and zones_config_set_model()
     * accept exactly the same set of models; see their definition. Present
     * but malformed is still an error. */
    snprintf(key, sizeof(key), "z%u_k", i);
    {
        char probe[24];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MODEL_K_MAX, &z->model_k_dc)) {
                *err_reason = "zone model K out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_tau", i);
    {
        char probe[24];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MODEL_TIME_MAX_S, &z->model_tau_s)) {
                *err_reason = "zone model tau out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_deadtime", i);
    {
        char probe[24];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MODEL_TIME_MAX_S, &z->model_dead_time_s)) {
                *err_reason = "zone model dead time out of range";
                return false;
            }
        }
    }
    /* PID_EXPANSION_PLAN.md Phase 2/4 (2026-08-30): the fuzzy-PID and
     * cross-zone-coupling fields. OPTIONAL, same "older clients must not
     * start getting 400s for a field they've never heard of" reasoning as
     * z%u_xzone/z%u_k above -- but UNLIKE those, omitted means PRESERVE the
     * currently-stored value (current_z), the same convention z%u_tctype/
     * z%u_settings_source use, not "reset to 0". These three are measured
     * quantities (an operator-set adjustment knob, and an autotune-measured
     * coupling coefficient), and a whole-page save from a client that
     * predates this field (or simply didn't re-render every input) must not
     * silently delete a measurement/setting that took real effort to obtain
     * -- the model_k_dc/model_tau_s/model_dead_time_s "omit deletes it" case
     * above is this file's OWN documented sharp edge, not a precedent to
     * repeat for a field with no compensating "the page always posts these
     * back verbatim" guarantee behind it. Present but out of range is still
     * an error, never silently clamped (PID_EXPANSION_PLAN.md's own "prove
     * range checks refuse, not clamp" rule). */
    snprintf(key, sizeof(key), "z%u_fuzzy_strength", i);
    {
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_FUZZY_STRENGTH_PCT_MAX, &z->fuzzy_strength_pct)) {
                *err_reason = "zone fuzzy_strength_pct out of range (0-100)";
                return false;
            }
        } else {
            z->fuzzy_strength_pct = current_z->fuzzy_strength_pct;
        }
    }
    /* 2026-08-30 (ZONES_CFG_VERSION 10->11): one indexed key per cell,
     * z%u_coupling_c%u -- e.g. z1_coupling_c0 is zone 1's measured response
     * to zone 0's heater. Same per-cell "omit preserves the currently-stored
     * value" convention z%u_fuzzy_strength above uses (these are measured
     * quantities; a whole-page save from a client that predates a cell must
     * not silently delete it), and the diagonal (j == i) is refused if a
     * client submits anything but 0 for it, matching zones_config_set_
     * coupling()'s own storage-layer rule. */
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        snprintf(key, sizeof(key), "z%u_coupling_c%u", i, j);
        if (zones_config_json_field_present(body, key)) {
            float cell;
            if (!zones_config_json_parse_float_field(body, key, 0.0f, (j == i) ? 0.0f : ZONE_COUPLING_COEFF_MAX, &cell)) {
                *err_reason = "zone coupling_coeff out of range";
                return false;
            }
            z->coupling_coeff[j] = cell;
        } else {
            z->coupling_coeff[j] = current_z->coupling_coeff[j];
        }
    }
    /* settings_source: UNLIKE the three floats above, omitted must NOT
     * default to 0 -- 0 is a real, different value here ("copies zone 0's
     * settings"), not a safe empty default. Falls back to the CURRENT stored
     * value (current_z), same "omit preserves the live setting" convention
     * z%u_tctype uses just above, rather than to ZONE_SETTINGS_SOURCE_CUSTOM
     * unconditionally -- this is a whole-page submit, and an older client
     * that predates this field must not silently flip every zone back to
     * "custom" on an otherwise ordinary save (see this file's own
     * whole-page-submit discipline: every other optional field either
     * defaults to a safe zero or preserves the live value, never invents a
     * third behavior). */
    snprintf(key, sizeof(key), "z%u_settings_source", i);
    {
        if (zones_config_json_field_present(body, key)) {
            uint8_t src_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, 0xFF, &src_raw)) {
                *err_reason = "zone settings_source missing or invalid";
                return false;
            }
            if (src_raw != ZONE_SETTINGS_SOURCE_CUSTOM && src_raw >= MAX31856_CHANNEL_COUNT) {
                *err_reason = "zone settings_source references a zone that doesn't exist";
                return false;
            }
            /* Self-reference is the degenerate cycle ("zone 1 copies zone
             * 1"). Phase 5 owns the general cycle/disabled-zone guards, but
             * this one case is free to reject here and saves Phase 5 having
             * to unwind it. */
            if (src_raw == i) {
                *err_reason = "zone settings_source cannot point at itself";
                return false;
            }
            /* Longer cycle (2-zone, 3-zone, ...): same chain-walk
             * zones_config_set_settings_source() runs, against the live
             * s_zones.cfg for every OTHER zone -- this is a single-zone
             * write (only slot i's link is changing here), so any NEW cycle
             * must run through zone i; walking from i against everyone
             * else's live value is sufficient, matching the setter's own
             * reasoning. A whole-page POST that changes several zones'
             * links AT ONCE in a way that only cycles once every change is
             * applied is caught by the zones-POST handler's own re-walk of
             * every zone's chain across the fully-assembled tmp.zones[],
             * right after this function's call site's loop and before that
             * handler's commit point -- see the comment there. */
            {
                zone_cfg_t probe[MAX31856_CHANNEL_COUNT];
                memcpy(probe, s_zones.cfg.zones, sizeof(probe));
                probe[i].settings_source = src_raw;
                if (zones_config_json_settings_source_chain_has_cycle(probe, i, thermo_count)) {
                    *err_reason = "zone settings_source would create an inheritance cycle";
                    return false;
                }
            }
            z->settings_source = src_raw;
        } else {
            z->settings_source = current_z->settings_source;
        }
    }
    return true;
}

static esp_err_t zones_post_handler(httpd_req_t *req)
{
    /* Refuse to rewrite zone/relay/guard configuration while a firing is running, the same
     * gate kiln_cfg_http.c's apply and backup_http.c's restore already put in
     * front of the very same zones_cfg_t. Without it, changing relay_mask
     * mid-run moved the firing onto a different physical relay and left the
     * old one wherever it was last commanded, with nobody driving it off --
     * a contact that stays closed because the code that owned it stopped
     * looking at it.
     *
     * ota_http_check_interlocks() is that shared gate rather than a private
     * profile-is-RUNNING check, deliberately: it also covers a hot zone and
     * a commanded heater, and reads the kiln's ACTUAL current state rather
     * than profile_executor's own view (see its doc comment for why that
     * distinction matters). ota_http_req_ack_no_safety() carries the same
     * per-request operator acknowledgement every other caller passes, so a
     * board with no safety processor can still be configured -- saving this
     * page streams nothing over the link, exactly as backup_http's restore
     * argues for itself. */
    char interlock_reason[OTA_INTERLOCK_REASON_MAX];
    interlock_reason[0] = '\0';
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req),
                                                            interlock_reason, sizeof(interlock_reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "POST /api/zones refused by interlock: %s", interlock_reason);
        return ota_http_send_interlock_refusal(req, gate, interlock_reason);
    }

    if (req->content_len <= 0 || req->content_len > ZONES_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    /* HEAP (PSRAM), not stack -- ZONES_BODY_MAX+1 (4097B) was the second-
     * largest transient buffer on the whole httpd_worker task's request
     * path (coordinator review, 2026-08-31), and unlike profile_post_
     * handler's `body` this one genuinely IS read throughout the entire
     * function (http_form_find_field() calls scattered across the whole
     * parse), so it cannot be freed early the way that one's could -- it is
     * freed on EVERY return path below instead, mirroring every other
     * heap-converted handler in this pass. */
    char *body = heap_caps_malloc(ZONES_BODY_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        ESP_LOGE(TAG, "POST /api/zones: malloc(%u) failed for the request body buffer",
                 (unsigned)(ZONES_BODY_MAX + 1));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory reading the request body\"}");
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "zones body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            free(body);
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    zones_cfg_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    if (!zones_config_json_parse_u8_field(body, "thermo_count", 0, MAX31856_CHANNEL_COUNT, &tmp.thermo_count)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "thermo_count missing or out of range");
        free(body);
        return ESP_OK;
    }
    if (!zones_config_json_parse_u8_field(body, "relay_count", 0, KILN_IO_RELAY_COUNT, &tmp.relay_count)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay_count missing or out of range");
        free(body);
        return ESP_OK;
    }
    /* Optional -- unlike thermo_count/relay_count, a missing
     * max_simultaneous_relays means "not set" (0, unlimited), not a
     * rejected request, so existing/older callers that don't send it keep
     * working unchanged. Present-but-out-of-range is still rejected. */
    {
        char val[8];
        int len = http_form_find_field(body, "max_simultaneous_relays", val, sizeof(val));
        if (len > 0) {
            char *end = NULL;
            long v = strtol(val, &end, 10);
            /* *end != '\0' rejects trailing garbage after a valid numeric
             * prefix (e.g. "2X"), same gap as zones_config_json_parse_u8_field()/
             * zones_config_json_parse_float_field() above -- end == val alone lets it through. */
            if (end == val || *end != '\0' || v < 0 || v > KILN_IO_RELAY_COUNT) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "max_simultaneous_relays out of range");
                free(body);
                return ESP_OK;
            }
            tmp.max_simultaneous_relays = (uint8_t)v;
        }
    }
    /* Optional, same "missing means keep the safe default" convention as
     * max_simultaneous_relays above -- tmp is zeroed, so a caller that
     * never sends this field gets continue_on_zone_trip=0 (abort the whole
     * firing), TODO.md 6A.3's stated default. Only "0" or "1" accepted. */
    {
        char val[4];
        int len = http_form_find_field(body, "continue_on_zone_trip", val, sizeof(val));
        if (len > 0) {
            if (strcmp(val, "1") == 0) {
                tmp.continue_on_zone_trip = 1;
            } else if (strcmp(val, "0") == 0) {
                tmp.continue_on_zone_trip = 0;
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "continue_on_zone_trip must be 0 or 1");
                free(body);
                return ESP_OK;
            }
        }
    }

    /* 2026-08-27 (ZONES_CFG_VERSION 8->9, owner's request: "make it so that i
     * can have diffrent Safety timings and assign the zones to them"): the
     * named timing profiles this submission defines, parsed BEFORE the
     * per-zone loop below -- each zone's z%u_timingprofile must be bounded
     * against however many profiles THIS submission actually carries, and
     * that count is only known once this loop finishes. A profile slot is
     * "in the submission" iff tp<N>_name is present, checked contiguously
     * from N=0: GET /api/zones always emits a dense 0..count-1
     * timing_profiles array (see zones_get_handler()), so the page always
     * reposts one too, and the first missing tp<N>_name is the end of the
     * list, not a gap to skip past. At least one profile (tp0_name) is
     * required -- an empty timing_profiles[] would leave every zone's
     * z%u_timingprofile with nothing valid to reference, and
     * zones_cfg_t::timing_profile_count must never be 0 in a config this
     * handler commits (see its own comment). */
    for (uint8_t p = 0; p < MAX31856_CHANNEL_COUNT; p++) {
        char probe_key[16];
        char probe[TIMING_PROFILE_NAME_MAX_LEN + 1];
        snprintf(probe_key, sizeof(probe_key), "tp%u_name", p);
        int probe_len = http_form_find_field(body, probe_key, probe, sizeof(probe));
        if (probe_len == -2) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "timing profile name too long");
            free(body);
            return ESP_OK;
        }
        if (probe_len < 0) {
            break; /* no tp<p>_name -- this and every following slot is absent from this submission */
        }
        const char *err_reason = "invalid timing profile field";
        if (!zones_config_json_parse_timing_profile_fields(body, p, &tmp.timing_profiles[p], &err_reason)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_reason);
            free(body);
            return ESP_OK;
        }
        tmp.timing_profile_count = (uint8_t)(p + 1);
    }
    if (tmp.timing_profile_count == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "at least one timing profile (tp0_name) is required");
        free(body);
        return ESP_OK;
    }

    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        const char *err_reason = "invalid zone field";
        /* &s_zones.cfg.zones[i]: the LIVE value, for z%u_tctype's
         * omit-means-preserve fallback (see parse_zone_fields()'s comment) --
         * tmp itself is zeroed, so tmp.zones[i] can't supply "what this
         * channel is already set to." */
        if (!parse_zone_fields(body, i, tmp.thermo_count, tmp.relay_count, tmp.timing_profile_count,
                               &s_zones.cfg.zones[i], &tmp.zones[i], &err_reason)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_reason);
            free(body);
            return ESP_OK;
        }
    }
    /* parse_zone_fields()'s own per-zone chain-walk (above, inside that
     * function) only ever sees ONE zone's link changing at a time -- it
     * checks the new link for zone i against every OTHER zone's LIVE stored
     * value, which is correct for that single write but blind to a
     * whole-page POST that changes SEVERAL zones' links in the same
     * request, none of which is a cycle by itself against the old live
     * config, but which together close one (e.g. z0_settings_source=1 and
     * z1_settings_source=0 in the same POST when neither zone pointed
     * anywhere before). tmp.zones[] now holds every zone's fully-assembled
     * NEW link, so re-walk every zone's chain against THAT -- before the
     * commit point below, so a rejection here still leaves s_zones
     * untouched, same "never partially apply" discipline the rest of this
     * handler follows. Bounded by tmp.thermo_count, same "unused trailing
     * slot" discipline zones_config_json_settings_source_chain_has_cycle()'s own comment
     * explains -- a slot past this submission's own thermo_count was never
     * rendered and never posted to, so it is excluded from this walk
     * entirely rather than treated as a real link. (It does NOT read back at
     * a zero-initialized default: parse_zone_fields()'s early return for
     * such a slot does `*z = *current_z`, so tmp.zones[i] carries whatever
     * settings_source is already LIVE and stored for that zone, not 0 --
     * still bounded out of this walk on principle, since that live value was
     * not part of this submission either, but the "reads back as 0" premise
     * would be wrong if repeated as a reason.) */
    for (uint8_t i = 0; i < tmp.thermo_count && i < MAX31856_CHANNEL_COUNT; i++) {
        if (zones_config_json_settings_source_chain_has_cycle(tmp.zones, i, tmp.thermo_count)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "zone settings_source would create an inheritance cycle");
            free(body);
            return ESP_OK;
        }
    }

    /* 2026-08-21, TODO.md owner-report item 3: the safety processor's own
     * thermocouple type (see zones_cfg_t::safety_tc_type's comment for why
     * this is a separate global setting rather than any zone's tc_type).
     * OPTIONAL, falling back to the current live value on omit -- same
     * "cannot silently relinearize a channel against the wrong type"
     * reasoning as z%u_tctype above, and for the identical reason: an older
     * client that predates this field must not zero a real, physically
     * meaningful setting just by doing an otherwise-ordinary whole-page
     * save. Bounds match ZONE_TC_TYPE_MAX_REAL -- the safety processor's own
     * MAX31856 is the same part with the same eight real thermocouple types;
     * see that macro's comment. */
    {
        char val[8];
        int len = http_form_find_field(body, "safety_tc_type", val, sizeof(val));
        if (len > 0) {
            char *end = NULL;
            long v = strtol(val, &end, 10);
            /* *end != '\0' rejects trailing garbage, same gap as the other
             * numeric parsers in this file -- see zones_config_json_parse_u8_field()'s comment. */
            if (end == val || *end != '\0' || v < 0 || v > ZONE_TC_TYPE_MAX_REAL) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "safety_tc_type must be a real thermocouple type (0-7: "
                                    "B/E/J/K/N/R/S/T), not a voltage-input mode");
                free(body);
                return ESP_OK;
            }
            tmp.safety_tc_type = (uint8_t)v;
        } else {
            tmp.safety_tc_type = s_zones.cfg.safety_tc_type;
        }
    }

    /* The one global v8 override. OPTIONAL, and on omit it keeps the CURRENT
     * live value rather than resetting to 0 -- the same reasoning as
     * safety_tc_type above: this is a whole-page submit, and an older client
     * that predates the field must not silently reset how long a firing
     * tolerates a dead PC link just by saving the zones page. */
    {
        char val[16];
        int len = http_form_find_field(body, "pc_link_abort_silence_ms", val, sizeof(val));
        if (len > 0) {
            if (!zones_config_json_parse_float_field(body, "pc_link_abort_silence_ms", 0.0f,
                                   ZONE_PC_LINK_SILENCE_MS_MAX, &tmp.pc_link_abort_silence_ms)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "pc_link_abort_silence_ms out of range (0 = firmware default)");
                free(body);
                return ESP_OK;
            }
        } else {
            tmp.pc_link_abort_silence_ms = s_zones.cfg.pc_link_abort_silence_ms;
        }
    }

    /* 2026-08-27+1 (owner request: name relays not assigned to any zone).
     * relay<N>_name (N = 1..KILN_IO_RELAY_COUNT, matching relay_mask's wire
     * numbering) is GLOBAL, relay-indexed, not per-zone -- so it follows the
     * same "omitted means keep the current live value" convention this
     * handler already uses for safety_tc_type/pc_link_abort_silence_ms
     * above, NOT the per-zone fields' "omitted means zero" convention
     * (tmp.zones[] is zero-initialized; this isn't). That matters here even
     * more than it does for those: zones_page.html and safety_config_page.html
     * BOTH POST to this same endpoint, and only zones_page.html renders a
     * relay-name input at all (see its renderRelayNames()) -- a save
     * triggered from /settings/safety must not wipe every relay name just
     * because that page has no editable field for them. safety_config_page.html
     * still echoes them anyway (defense in depth, matching its existing
     * safety_tc_type/pc_link_abort_silence_ms echo style), but this
     * omit-preserves convention is what actually GUARANTEES neither page can
     * clobber the other's relay names, independent of whether that echo is
     * ever forgotten in a future edit to either page.
     *
     * Storage is unconditional regardless of the relay's CURRENT zone
     * ownership -- see this file's relay-names section header comment for
     * why a name is kept, not cleared, when its relay becomes zone-owned.
     * Present-but-overlong is still rejected (a real mistake, not a
     * deliberate omission), same as z%u_name's -2 handling in
     * parse_zone_fields(). Parsed into a scratch copy of the CURRENT live
     * relay names, not applied to s_relay_names.cfg directly, so a request
     * that gets rejected later in this handler (impossible past this point
     * today, but kept for the same "never partially apply" discipline every
     * other section of this handler follows) has touched nothing. */
    relay_names_cfg_t tmp_relay_names = s_relay_names.cfg;
    for (uint8_t r = 1; r <= KILN_IO_RELAY_COUNT; r++) {
        char rkey[16];
        snprintf(rkey, sizeof(rkey), "relay%u_name", r);
        char rval[RELAY_NAME_MAX_LEN + 1];
        int rlen = http_form_find_field(body, rkey, rval, sizeof(rval));
        if (rlen == -2) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay name too long");
            free(body);
            return ESP_OK;
        }
        if (rlen >= 0) {
            strncpy(tmp_relay_names.names[r - 1], rval, RELAY_NAME_MAX_LEN);
            tmp_relay_names.names[r - 1][RELAY_NAME_MAX_LEN] = '\0';
        }
        /* rlen < 0 (omitted): tmp_relay_names.names[r-1] already holds the
         * current live value, copied above -- left untouched. */
    }

    /* Commit point: every rejection above returned before touching s_zones,
     * so this is the first and only line at which the submission becomes the
     * live config -- and therefore the only place in this handler the
     * generation may advance. A 400'd submission changed nothing and must
     * not make a running profile re-read identical settings (TODO.md
     * 6A.7). */
    s_zones.cfg = tmp;
    s_relay_names.cfg = tmp_relay_names;
    /* A validated, freshly-submitted config is trustworthy the moment it's
     * live in RAM, regardless of whether the NVS write below succeeds --
     * same "applied now either way" convention nvs_save()'s failure handling
     * already uses below. This is the other half of s_zones_config_valid's
     * contract: true after either a real successful load OR a fresh valid
     * save. */
    s_zones_config_valid = true;
    s_config_generation++;
    esp_err_t err = nvs_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save failed: %s -- config applied live but will not survive a reboot",
                 esp_err_to_name(err));
        /* Still applied above -- the operator asked for this right now,
         * whether or not it persists past a reboot, same convention as
         * wifi_prov.c's nvs_save_creds failure handling. */
    }
    esp_err_t names_err = relay_names_save();
    if (names_err != ESP_OK) {
        ESP_LOGE(TAG, "relay_names_save failed: %s -- names applied live but will not survive a reboot",
                 esp_err_to_name(names_err));
        /* Same "applied now either way" convention as nvs_save() above --
         * cosmetic data that failed to persist is not worth refusing a
         * whole-page save that DID validate and apply everything else. */
    }
    free(body);
    return httpd_resp_sendstr(req, "ok");
}

/* ---- Task 1: per-zone normal-current measurement sweep -------------------
 *
 * How long to hold one zone's relay(s) on while measuring, and why:
 *
 * SaftyFW/docs/CURRENT_SENSE.md §3: the analog front end's rise to a full
 * reading is < 10 ms once current actually flows through the CT -- that part
 * of the path is effectively instant and does not gate anything here.
 *
 * The real settle time is mechanical + protocol. The relay coil (EE2-12NUH,
 * kiln_io.h) needs to physically close, and this ESP only LEARNS the new
 * current_a[] reading once the next SAFETY_LINK status poll lands --
 * safety_link.h's poll_period_ms is 500 ms (SAFETY_LINK_UP_PERIODS=3 *
 * 500ms comment on link_up). ZONE_SWEEP_SETTLE_MS (1000ms = two full poll
 * periods) guarantees at least one FRESH sample has arrived after the relay
 * physically closed, with margin for a poll that happened to land just
 * before the relay engaged.
 *
 * After settling, ZONE_SWEEP_SAMPLE_MS (4000ms, ~8 more polls at 500ms) of
 * current_a[] readings are averaged -- the same "oversample and average"
 * discipline CURRENT_SENSE.md §4 documents for the ADC itself (16x per
 * sample there), applied one level up here to average out poll-to-poll
 * noise on the already-demodulated current reading.
 *
 * Total 5000ms/zone is short and bounded: swept back-to-back across
 * MAX31856_CHANNEL_COUNT zones that is at most ~5s * count, well under a
 * minute even for a fully populated board, and every zone but the one being
 * measured has its relay(s) OFF for the whole sweep (structural, not a
 * convention -- see zone_sweep_task() below). */
#define ZONE_SWEEP_SETTLE_MS 1000u
#define ZONE_SWEEP_SAMPLE_MS 4000u
#define ZONE_SWEEP_ENERGIZE_MS (ZONE_SWEEP_SETTLE_MS + ZONE_SWEEP_SAMPLE_MS)
#define ZONE_SWEEP_POLL_MS 500u /* matches safety_link.h's poll_period_ms */

/* ---- M12: deriving ct_channel_map from this sweep ------------------------
 * COMMISSIONING_UX.md sec 1.2 permits ct_channel_map[0..2] to be derived
 * "only when the mapping is unambiguous AND confirmed by the one-zone-at-a-
 * time energize (CURRENT_SENSE.md sec 5 step 2)". This sweep IS that
 * energize -- one zone's relay(s) on, every other relay forced off for the
 * whole 5s window (zone_sweep_hw_energize()'s 0xFF mask) -- so it is the
 * only place on this board that can honestly claim both halves. Before
 * this, the only producer of ct_channel_map was three hand-typed fields on
 * the commissioning page, which means S14 (and the mapping half of S3/S4)
 * armed only if somebody typed the right three numbers.
 *
 * A channel is taken to have responded to the zone under test only if:
 *   - it is carrying real load current (>= ZONE_SWEEP_CT_RESPOND_A), and
 *   - it dominates every other channel by ZONE_SWEEP_CT_DOMINANCE.
 * The threshold is the same order as SaftyFW's i_present_a load-active
 * default (2.0 A) on purpose: this only has to separate a conducting
 * element from measurement noise, and CURRENT_SENSE.md sec 0 already scopes
 * the current chain's accuracy as "within a factor of ~2" -- a tighter
 * number would be promising precision the hardware does not have. The
 * dominance factor is what refuses the genuinely ambiguous cases: ct_mask
 * explicitly permits one CT feeding more than one zone (zones_http.c's 5->6
 * comment), and two channels reading within 4x of each other during one
 * zone's window is exactly that shared-CT case, or a foreign load. Neither
 * is derivable, so nothing is written and the operator is told which zone
 * could not be resolved -- guessing here writes a wrong value into a red
 * field that S3/S4/S14 then trust. */
#define ZONE_SWEEP_CT_RESPOND_A 2.0f
#define ZONE_SWEEP_CT_DOMINANCE 4.0f

/* Pure decision for the rule above. `per_ch_a` is ZONE_CT_CHANNEL_COUNT
 * averaged per-channel currents from one zone's sample window; a NaN entry
 * (no per-channel sampler wired) makes the whole call ambiguous rather than
 * being treated as 0 A. */
static bool zone_sweep_derive_ct_channel(const float *per_ch_a, uint8_t *out_ch)
{
    uint8_t best = 0;
    float best_a = -1.0f, second_a = -1.0f;
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if (!isfinite(per_ch_a[c])) {
            return false;
        }
        if (per_ch_a[c] > best_a) {
            second_a = best_a;
            best_a = per_ch_a[c];
            best = c;
        } else if (per_ch_a[c] > second_a) {
            second_a = per_ch_a[c];
        }
    }
    if (best_a < ZONE_SWEEP_CT_RESPOND_A) {
        return false; /* nothing conducted -- CT not fitted, or the zone drew no current */
    }
    if (second_a > 0.0f && best_a < second_a * ZONE_SWEEP_CT_DOMINANCE) {
        return false; /* two channels saw this zone -- shared CT or foreign load */
    }
    if (out_ch) {
        *out_ch = best;
    }
    return true;
}

/* ---- M12b: calibrating k_ct_v_per_a from the same sweep -------------------
 * CURRENT_SENSE.md sec 5 step 3 used to be the only producer of
 * k_ct_v_per_a: "compare the computed amps against a clamp meter and adjust
 * k_ct_v_per_a to match". That is a number nobody has at commissioning time,
 * on a page that then asked for it in V/A -- so in practice it stayed at
 * config_store.c's memset(0) placeholder, which is NOT harmless: with
 * k_ct_v_per_a <= 0 current_presence_policy.c falls back to a fixed
 * counts-domain margin rather than the i_present_a the operator configured
 * (that header's own comment), and every reported amps/watts figure reads 0.
 *
 * The sweep already has both halves of a calibration the operator does not
 * have to type:
 *   - the EXPECTED whole-kiln current at full output, from the two figures
 *     the guided commissioning flow already collects (COMMISSIONING_UX.md
 *     Q3 mains_voltage_v 0x030E, Q4 max_expected_power_w 0x0319):
 *     I_expected = P / V.
 *   - the MEASURED whole-kiln current, as the sum of each zone's dominant
 *     CT channel over a run that energized every zone exactly once with all
 *     other relays forced off (zone_sweep_hw_energize()'s 0xFF mask). Summing
 *     the one-zone-at-a-time readings is the same total a simultaneous
 *     full-output firing would draw, and it is the only version of that total
 *     this fixture can measure without ever having two zones on at once.
 *
 * Since amps are computed on the Pico as I = V_adc / (gain * sqrt2 * k_ct),
 * the measurement is inversely proportional to k_ct, so the whole calibration
 * is one scale factor:
 *
 *     k_new[c] = k_old[c] * (I_measured_total / I_expected_total)
 *
 * with k_old[c] the value the Pico has actually COMMITTED. That dependence on
 * a prior k_old is not a weakness of this derivation, it is inherent: the
 * link carries amps, never raw counts (safety_link_status_t has current_a[3]
 * and nothing else), so an uncommissioned k_old makes every channel read
 * 0.0 A and there is no measurement to scale. The same is already true of
 * the CT-MAP derivation above -- zone_sweep_derive_ct_channel() needs
 * >= 2.0 A to resolve anything -- so a board that can derive the map can
 * always derive this too, and one that cannot is refused here with a reason
 * rather than being given an invented number. */

/* The correction this is willing to apply. CURRENT_SENSE.md sec 0 scopes the
 * whole current chain's accuracy as "within a factor of ~2", so a correction
 * inside this band is a plausible calibration of a plausible starting value.
 * Outside it, the disagreement is not a scale error at all -- a nameplate in
 * the wrong units, a CT on the wrong conductor, a current-output CT fitted
 * where a voltage-output one belongs (CURRENT_SENSE.md's "wiring error the
 * firmware cannot detect") -- and quietly scaling k_ct to paper over it would
 * make the reported amps look right while the presence threshold this same
 * constant feeds moved to match a lie. Refuse and say so. */
#define ZONE_KCT_RATIO_MIN 0.2f
#define ZONE_KCT_RATIO_MAX 5.0f

/* The absolute band a self-burdened voltage-output CT can plausibly land in:
 * an SCT-013-030 is 0.0333 V/A (CURRENT_SENSE.md sec 5), a 1 V/100 A part is
 * 0.01, and a burdened part with R72 fitted is higher still. Two decades
 * either side of that spread is generous; anything outside is not a CT. */
#define ZONE_KCT_MIN_V_PER_A 0.0005f
#define ZONE_KCT_MAX_V_PER_A 0.5f

/* The measured total below which no calibration is attempted. Same order and
 * same reasoning as ZONE_SWEEP_CT_RESPOND_A: below a conducting element's
 * current the ratio is dominated by measurement noise, and a scale factor
 * computed from noise is worse than no scale factor. */
#define ZONE_KCT_MIN_MEASURED_A ZONE_SWEEP_CT_RESPOND_A

typedef enum {
    ZONE_KCT_DERIVE_OK = 0,
    ZONE_KCT_DERIVE_NO_NAMEPLATE,   /* mains_voltage_v / max_expected_power_w not both usable */
    ZONE_KCT_DERIVE_NO_MEASUREMENT, /* this run measured no usable total current */
    ZONE_KCT_DERIVE_NO_PRIOR_K,     /* k_ct_v_per_a uncommissioned -- amps carry no scale */
    ZONE_KCT_DERIVE_IMPLAUSIBLE,    /* the correction is outside the bands above */
} zone_kct_derive_t;

/* Pure decision for the rule above -- every input is a plain number the
 * caller gathers, so the whole calibration is host-testable off-target.
 * *out_k is written ONLY on ZONE_KCT_DERIVE_OK. */
static zone_kct_derive_t zone_sweep_derive_k_ct(float measured_total_a, float expected_power_w,
                                                 float mains_voltage_v, float k_old, float *out_k)
{
    if (!isfinite(expected_power_w) || expected_power_w <= 0.0f || !isfinite(mains_voltage_v) ||
        mains_voltage_v <= 0.0f) {
        return ZONE_KCT_DERIVE_NO_NAMEPLATE;
    }
    if (!isfinite(measured_total_a) || measured_total_a < ZONE_KCT_MIN_MEASURED_A) {
        return ZONE_KCT_DERIVE_NO_MEASUREMENT;
    }
    if (!isfinite(k_old) || k_old <= 0.0f) {
        return ZONE_KCT_DERIVE_NO_PRIOR_K;
    }
    float expected_a = expected_power_w / mains_voltage_v;
    if (!isfinite(expected_a) || expected_a <= 0.0f) {
        return ZONE_KCT_DERIVE_NO_NAMEPLATE; /* P/V overflowed or underflowed to nothing usable */
    }
    float ratio = measured_total_a / expected_a;
    if (!isfinite(ratio) || ratio < ZONE_KCT_RATIO_MIN || ratio > ZONE_KCT_RATIO_MAX) {
        return ZONE_KCT_DERIVE_IMPLAUSIBLE;
    }
    float k_new = k_old * ratio;
    if (!isfinite(k_new) || k_new < ZONE_KCT_MIN_V_PER_A || k_new > ZONE_KCT_MAX_V_PER_A) {
        return ZONE_KCT_DERIVE_IMPLAUSIBLE;
    }
    if (out_k) {
        *out_k = k_new;
    }
    return ZONE_KCT_DERIVE_OK;
}

static const char *zone_kct_derive_str(zone_kct_derive_t r)
{
    switch (r) {
    case ZONE_KCT_DERIVE_OK: return "ok";
    case ZONE_KCT_DERIVE_NO_NAMEPLATE:
        return "answer the mains voltage and full-output power questions first";
    case ZONE_KCT_DERIVE_NO_MEASUREMENT: return "the sweep measured no load current";
    case ZONE_KCT_DERIVE_NO_PRIOR_K: return "k_ct_v_per_a has never been set, so amps read zero";
    case ZONE_KCT_DERIVE_IMPLAUSIBLE:
        return "measured and nameplate current disagree too far to be a scale error";
    default: return "unknown";
    }
}

const char *zone_sweep_refusal_str(zone_sweep_refusal_t r)
{
    switch (r) {
    case ZONE_SWEEP_REFUSE_OK: return "ok";
    case ZONE_SWEEP_REFUSE_ALREADY_RUNNING: return "a current sweep is already running";
    case ZONE_SWEEP_REFUSE_NO_HW: return "board I/O is not available this boot";
    case ZONE_SWEEP_REFUSE_CONFIG_INVALID: return "zone config did not load cleanly -- cannot energize relays";
    case ZONE_SWEEP_REFUSE_NO_ZONES: return "no zones are configured";
    case ZONE_SWEEP_REFUSE_PROFILE_RUNNING: return "a firing profile is running or paused";
    case ZONE_SWEEP_REFUSE_AUTOTUNE_RUNNING: return "autotune is running";
    case ZONE_SWEEP_REFUSE_LINK_DOWN: return "the safety link is down";
    case ZONE_SWEEP_REFUSE_TRIP_LATCHED: return "a safety trip is latched";
    case ZONE_SWEEP_REFUSE_RELAYS_ON: return "a relay is already on -- turn it off before sweeping";
    default: return "unknown refusal";
    }
}

/* Pure, host-tested refusal decision -- every input is a plain value the
 * caller (zones_current_sweep_start() below) gathers from the real
 * subsystems; this function itself touches no hardware and can be exercised
 * completely off-target. First matching reason wins; order matches the
 * doc comment on zones_current_sweep_start() in zones_http.h. */
static zone_sweep_refusal_t zone_sweep_check_refusal(bool already_running, bool have_hw, bool config_valid,
                                                      uint8_t thermo_count, bool profile_running_or_paused,
                                                      bool autotune_active, bool link_up, bool trip_latched,
                                                      bool relays_on)
{
    if (already_running) {
        return ZONE_SWEEP_REFUSE_ALREADY_RUNNING;
    }
    if (!have_hw) {
        return ZONE_SWEEP_REFUSE_NO_HW;
    }
    if (!config_valid) {
        return ZONE_SWEEP_REFUSE_CONFIG_INVALID;
    }
    if (thermo_count == 0) {
        return ZONE_SWEEP_REFUSE_NO_ZONES;
    }
    if (profile_running_or_paused) {
        return ZONE_SWEEP_REFUSE_PROFILE_RUNNING;
    }
    if (autotune_active) {
        return ZONE_SWEEP_REFUSE_AUTOTUNE_RUNNING;
    }
    if (!link_up) {
        return ZONE_SWEEP_REFUSE_LINK_DOWN;
    }
    if (trip_latched) {
        return ZONE_SWEEP_REFUSE_TRIP_LATCHED;
    }
    if (relays_on) {
        return ZONE_SWEEP_REFUSE_RELAYS_ON;
    }
    return ZONE_SWEEP_REFUSE_OK;
}

/* H2 (opus reviews, 2026-08-27 and 2026-08-28): max_temp_c <= 0 is
 * zones_config_get_temp_limits()'s documented "no ceiling configured" state
 * -- and the DEFAULT on any zone that has never been commissioned. This
 * sweep is specifically a COMMISSIONING tool: an uncommissioned board is
 * exactly the board it will first be run on, so "no ceiling configured"
 * cannot mean "no thermal abort at all" here the way it legitimately can for
 * a firing profile the operator already reviewed. Refusing to sweep an
 * unceilinged zone would make the tool useless on the one board that most
 * needs it (nothing to measure a normal current against yet), so instead:
 * derive an effective ceiling.
 *
 * The 2026-08-27 pass reused OTA_INTERLOCK_TEMP_CEILING_C (100 C) for this.
 * The 2026-08-28 review rejected that: OTA_INTERLOCK_TEMP_CEILING_C was
 * chosen as a conservative OTA precondition for a kiln AT REST, not as a
 * thermal abort for a deliberate energize, and it drifts independently of
 * this file's own needs -- worse, THIS bench is capped at 80 C by every
 * zone's own configured ceiling, so a 100 C fallback can structurally never
 * fire on the hardware this sweep actually runs against. Fixed:
 * zone_sweep_effective_ceiling_c() below derives the ceiling instead of
 * borrowing OTA's -- the tightest configured ceiling across every zone that
 * HAS one, self-calibrating to whatever board this is (80 C on this bench,
 * the operator's tightest real ceiling on a real kiln). Only when NO zone
 * anywhere has a ceiling configured does it fall back to the fixed
 * ZONE_SWEEP_UNCOMMISSIONED_CEILING_C (60 C): a 5s energize on a stone-cold,
 * never-commissioned chamber raises it a few degrees at most, so a low
 * absolute costs nothing in false aborts.
 *
 * !actual_valid (no usable thermocouple reading) is NOT a ceiling hit
 * either: it is a different failure (see N1's consecutive-invalid-poll
 * counter in zone_sweep_run_one_zone() below, which is what actually stops
 * an unsupervised run once the thermo bus stops answering -- link_up()
 * watches the ESP<->Pico UART, not temperature, and cannot substitute for
 * this). */
#define ZONE_SWEEP_UNCOMMISSIONED_CEILING_C 60.0f

static float zone_sweep_effective_ceiling_c(void)
{
    bool have_any = false;
    float min_ceiling_c = 0.0f;
    uint8_t n = s_zones.cfg.thermo_count;
    if (n > MAX31856_CHANNEL_COUNT) {
        n = MAX31856_CHANNEL_COUNT;
    }
    for (uint8_t zi = 0; zi < n; zi++) {
        float max_temp_c = 0.0f, min_temp_c = 0.0f;
        if (!zones_config_get_temp_limits(zi, &max_temp_c, &min_temp_c)) {
            continue;
        }
        if (max_temp_c > 0.0f && (!have_any || max_temp_c < min_ceiling_c)) {
            min_ceiling_c = max_temp_c;
            have_any = true;
        }
    }
    return have_any ? min_ceiling_c : ZONE_SWEEP_UNCOMMISSIONED_CEILING_C;
}

static bool zone_sweep_ceiling_hit(float actual_c, bool actual_valid, float effective_ceiling_c)
{
    if (!actual_valid || isnan(actual_c)) {
        return false;
    }
    return actual_c >= effective_ceiling_c;
}

static bool zone_sweep_should_sample(uint32_t elapsed_ms)
{
    return elapsed_ms >= ZONE_SWEEP_SETTLE_MS;
}

static bool zone_sweep_zone_done(uint32_t elapsed_ms)
{
    return elapsed_ms >= ZONE_SWEEP_ENERGIZE_MS;
}

/* LOW (opus review, 2026-08-27): every field below is written by the sweep
 * task on one core and read by the HTTP task (zones_current_sweep_get_status()/
 * zones_current_sweep_start()'s `.active` check) on the other, with no lock
 * between them -- only `abort_requested` used to be `volatile`, which stops
 * the compiler from caching a stale value in a register across the sweep
 * task's poll loop but says nothing about the other fields the HTTP task
 * reads. `volatile` here does not make the read/modify sequence atomic (it
 * still is not: a status read mid-snprintf() can observe a partially written
 * `reason[]`, same as before) -- it only stops each individual field access
 * from being reordered or cached across the two tasks the way a plain field
 * could be. `zones_total`/`zone_index`/`zones_done`/`reason[]` are bytes that
 * only ever move forward/get overwritten wholesale, so a torn read here is a
 * momentarily-stale status string or count, not a value that could look like
 * a valid-but-wrong number no snapshot of memory ever actually held. */
typedef struct {
    volatile bool               active;   /* a sweep task is currently running */
    volatile bool               abort_requested;
    volatile zone_sweep_state_t state;
    volatile uint8_t            zone_index;
    volatile uint8_t            zones_done;
    volatile uint8_t            zones_total;
    volatile char                reason[64];
    /* M12: what this run made of the CT map. ct_map_derived_mask is the set
     * of channels resolved AND written to the safety processor;
     * ct_map_reason is "" only when every swept zone resolved and the write
     * landed -- a sweep that measured every normal current perfectly but
     * could not resolve a CT must not read as an unqualified success. */
    volatile uint8_t             ct_map_derived_mask;
    volatile char                ct_map_reason[96];
    /* M12b: the same pair for the k_ct_v_per_a calibration -- separate from
     * ct_map_* on purpose, since the two derivations fail independently (a
     * perfectly mapped board with no nameplate answer derives one and not
     * the other) and a single shared reason string could only report one. */
    volatile uint8_t             k_ct_derived_mask;
    volatile char                k_ct_reason[96];
    TaskHandle_t                 task;
} zone_sweep_ctx_t;

static zone_sweep_ctx_t s_sweep = {
    .active = false, .abort_requested = false, .state = ZONE_SWEEP_IDLE,
    .zone_index = 0, .zones_done = 0, .zones_total = 0, .reason = "", .task = NULL,
};

/* THE single choke point every exit path of the sweep goes through to make
 * sure relays end up off -- abort, ceiling hit, link loss, or ordinary
 * completion all call this and NOTHING ELSE turns a relay off in this
 * module's sweep code.
 *
 * B1 fix (opus review, 2026-08-27): this used to call kiln_io_all_relays_off()
 * directly. kiln_io_owner.h's own top comment names that call out by name as
 * outside its documented licence when used as a NORMAL end-of-measurement
 * path rather than a last-resort fail-safe (link watchdog / guard-9 watchdog
 * / kiln_enter_safe_state()) -- this is the normal path, every single zone.
 * Routed through kiln_io_owner_command_all_relays_off() instead: same
 * "unconditional, no gating" behavior kiln_io_owner.h documents for it
 * (relays only ever come off here, never on), now serialized against the
 * other five relay writers through the owner task's queue instead of racing
 * them on a stale SX1509 read. */
static void zone_sweep_force_relays_off(void)
{
    if (s_hw_io) {
        kiln_io_owner_command_all_relays_off();
    }
}

/* Reads zone zi's combined, calibrated temperature the same way
 * profile_executor.c's control tick does (MAX31856_read_all() +
 * thermo_combine() over the zone's thermo_mask) -- duplicated rather than
 * shared because profile_executor.c is off-limits for this pass and its own
 * read is buried inside a much larger per-tick loop with no standalone
 * entry point. *out_valid false means "no usable reading", matching
 * thermo_combine()'s own convention. */
static void zone_sweep_read_zone_temp(uint8_t zi, float *out_c, bool *out_valid)
{
    *out_c = NAN;
    *out_valid = false;
    if (!s_hw_thermo_bus || !s_hw_thermo_bus->initialized) {
        return;
    }
    MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
    size_t count = 0;
    if (MAX31856_read_all(s_hw_thermo_bus, readings, MAX31856_CHANNEL_COUNT, &count) != ESP_OK && count == 0) {
        return;
    }
    float ch_c[MAX31856_CHANNEL_COUNT];
    bool ch_ok[MAX31856_CHANNEL_COUNT];
    for (uint8_t ci = 0; ci < MAX31856_CHANNEL_COUNT; ci++) {
        ch_c[ci] = NAN;
        ch_ok[ci] = false;
    }
    for (size_t i = 0; i < count; i++) {
        uint8_t ci = readings[i].channel;
        if (ci >= MAX31856_CHANNEL_COUNT) {
            continue;
        }
        ch_c[ci] = readings[i].tc_temperature_c;
        bool fault_bits_bad = (readings[i].fault_status & (0x01u | 0x02u | 0x40u)) != 0;
        ch_ok[ci] = !readings[i].spi_failed && !isnan(ch_c[ci]) && !fault_bits_bad;
    }
    uint8_t tmask = 0;
    zones_config_get_thermo_mask(zi, &tmask);
    bool valid = false;
    float combined = thermo_combine(ch_c, ch_ok, MAX31856_CHANNEL_COUNT, tmask, &valid);
    *out_valid = valid;
    *out_c = valid ? zones_config_apply_cal(zi, combined) : NAN;
}

/* ---- M3 (opus review, 2026-08-27): the per-zone state machine, extracted
 * from zone_sweep_task() below into a form the host tests can drive without
 * xTaskCreate() ever running. The task body used to be untestable by
 * construction (test_zones_http.c stubs xTaskCreate() so it never invokes
 * its task function, and says so in a comment) -- every safety-relevant
 * property named in the finding (relay on/off sequencing, the single-choke-
 * point property, the abort path, the link-loss exit) lived only in that
 * unreachable function body. Dependency injection through zone_sweep_zone_
 * deps_t is what makes it reachable: the SAME sequencing logic that runs on
 * target against real hardware runs in the host tests against fakes that
 * record call order.
 *
 * Deliberately excluded from the deps: the CT-mask -> live_a summation is
 * plain arithmetic over already-fetched values (covered by other, simpler
 * tests) and not itself part of the safety argument this extraction targets;
 * `sample_current` hands the step function an already-summed reading so the
 * deps surface stays focused on the calls that matter here -- energize,
 * de-energize, abort, link status. */
typedef struct {
    /* B1: routes through kiln_io_owner_command_set_relay_mask() (MANUAL) --
     * see zone_sweep_run_one_zone()'s call site below for why MANUAL over
     * AUTHORIZED was chosen here. */
    kiln_io_owner_relay_result_t (*energize)(void *ctx, uint8_t relay_mask, uint32_t *out_safety_sources);
    void (*force_off)(void *ctx);              /* the choke point */
    void (*read_temp)(void *ctx, uint8_t zi, float *out_c, bool *out_valid);
    float (*sample_current)(void *ctx, uint8_t zi); /* already-summed live_a for zi's ct_mask */
    /* M12: the same instant's reading split PER CT CHANNEL, unfiltered by
     * any ct_mask -- the ct_mask is precisely the thing being derived here,
     * so summing through it first would only ever confirm what was already
     * configured. Optional: NULL leaves the per-channel averages NaN, which
     * zone_sweep_derive_ct_channel() treats as "cannot tell", never as 0 A. */
    void (*sample_channels)(void *ctx, float *out_a); /* ZONE_CT_CHANNEL_COUNT entries */
    bool (*link_up)(void *ctx);
    bool (*trip_latched)(void *ctx);            /* N10: fault_asserted || diag TRIPPED */
    bool (*abort_requested)(void *ctx);
    void (*delay_poll)(void *ctx);              /* one ZONE_SWEEP_POLL_MS tick */
    void *ctx;
} zone_sweep_zone_deps_t;

/* N1 (opus review, 2026-08-28): consecutive `!actual_valid` polls tolerated
 * before treating "no usable thermocouple reading" as its own abort. One
 * poll of noise tolerance, not zero -- a single dropped/garbled SPI
 * transaction is not itself evidence the bus is wedged. Two consecutive
 * misses (this constant) is. */
#define ZONE_SWEEP_TEMP_LOST_POLLS 2u

typedef enum {
    ZONE_SWEEP_ZONE_SKIPPED = 0,      /* relay_mask == 0 -- nothing wired, nothing measured */
    ZONE_SWEEP_ZONE_ABORTED,
    ZONE_SWEEP_ZONE_CEILING_HIT,
    ZONE_SWEEP_ZONE_LINK_LOST,
    ZONE_SWEEP_ZONE_TRIP_LATCHED,     /* N10: a safety trip latched mid-zone */
    ZONE_SWEEP_ZONE_TEMP_LOST,        /* N1: the ceiling abort went blind -- ZONE_SWEEP_TEMP_LOST_POLLS
                                        * consecutive polls with no usable thermocouple reading */
    ZONE_SWEEP_ZONE_ENERGIZE_REFUSED, /* B1: the owner refused the ON write (owned/safety/updating/io) */
    ZONE_SWEEP_ZONE_OK,
} zone_sweep_zone_outcome_t;

/* Runs one zone of the sweep to completion using `deps` for every side
 * effect. Mirrors the original inline loop body exactly, plus:
 *   - B1: the energize call itself can now be REFUSED (owner-gated), which
 *     the original direct kiln_io_set_relay_mask() call could never report --
 *     that refusal is itself an exit path and goes through the same choke
 *     point as every other one below.
 *   - N1 (opus review, 2026-08-28): the ceiling check is inert while
 *     actual_valid is false (zone_sweep_ceiling_hit() correctly never
 *     invents a hot reading). If the MAX31856 bus faults mid-run -- SPI
 *     wedge, fault bits set, a pulled thermocouple -- every subsequent poll
 *     comes back invalid and the ceiling abort could not fire for the rest
 *     of the zone's 5s energize, with link_up() providing no substitute
 *     supervision (it watches the ESP<->Pico UART, not temperature). Fixed:
 *     count consecutive invalid polls and exit via ZONE_SWEEP_ZONE_TEMP_LOST
 *     after ZONE_SWEEP_TEMP_LOST_POLLS, through the same choke point.
 *   - N10 (opus review, 2026-08-28): a safety trip latching mid-zone used to
 *     be invisible to this loop -- not a heat hazard on its own (the Pico
 *     opens its own contactor independently), but the sweep would grind on
 *     for the rest of its 5s dwell and every zone after this one would then
 *     be refused at the energize step. Exit promptly instead. */
static zone_sweep_zone_outcome_t zone_sweep_run_one_zone(uint8_t zi, uint8_t relay_mask,
                                                          const zone_sweep_zone_deps_t *deps,
                                                          float *out_avg_current_a,
                                                          float *out_per_ch_avg_a,
                                                          uint32_t *out_energize_refused_sources)
{
    if (relay_mask == 0) {
        return ZONE_SWEEP_ZONE_SKIPPED; /* nothing wired to this zone -- nothing to measure */
    }
    if (deps->abort_requested(deps->ctx)) {
        return ZONE_SWEEP_ZONE_ABORTED;
    }
    uint32_t safety_sources = 0;
    kiln_io_owner_relay_result_t rr = deps->energize(deps->ctx, relay_mask, &safety_sources);
    if (rr != KILN_IO_OWNER_RELAY_OK) {
        deps->force_off(deps->ctx); /* choke point -- nothing was left on, but be explicit */
        if (out_energize_refused_sources) *out_energize_refused_sources = safety_sources;
        return ZONE_SWEEP_ZONE_ENERGIZE_REFUSED;
    }

    uint32_t elapsed = 0;
    float sum_a = 0.0f;
    uint32_t samples = 0;
    float ch_sum_a[ZONE_CT_CHANNEL_COUNT] = {0};
    uint32_t invalid_streak = 0;
    float effective_ceiling_c = zone_sweep_effective_ceiling_c();
    zone_sweep_zone_outcome_t outcome = ZONE_SWEEP_ZONE_OK;
    while (!zone_sweep_zone_done(elapsed)) {
        if (deps->abort_requested(deps->ctx)) {
            outcome = ZONE_SWEEP_ZONE_ABORTED;
            break;
        }
        deps->delay_poll(deps->ctx);
        elapsed += ZONE_SWEEP_POLL_MS;

        float actual_c;
        bool actual_valid;
        deps->read_temp(deps->ctx, zi, &actual_c, &actual_valid);

        if (!actual_valid || isnan(actual_c)) {
            invalid_streak++;
            if (invalid_streak >= ZONE_SWEEP_TEMP_LOST_POLLS) {
                outcome = ZONE_SWEEP_ZONE_TEMP_LOST;
                break;
            }
        } else {
            invalid_streak = 0;
        }

        if (zone_sweep_ceiling_hit(actual_c, actual_valid, effective_ceiling_c)) {
            outcome = ZONE_SWEEP_ZONE_CEILING_HIT;
            break;
        }

        if (!deps->link_up(deps->ctx)) {
            outcome = ZONE_SWEEP_ZONE_LINK_LOST;
            break;
        }

        if (deps->trip_latched(deps->ctx)) {
            outcome = ZONE_SWEEP_ZONE_TRIP_LATCHED;
            break;
        }

        if (zone_sweep_should_sample(elapsed)) {
            sum_a += deps->sample_current(deps->ctx, zi);
            if (deps->sample_channels) {
                float ch_a[ZONE_CT_CHANNEL_COUNT];
                deps->sample_channels(deps->ctx, ch_a);
                for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
                    ch_sum_a[c] += ch_a[c];
                }
            }
            samples++;
        }
    }

    deps->force_off(deps->ctx); /* choke point -- every path out of this zone goes through here */

    if (outcome == ZONE_SWEEP_ZONE_OK && samples > 0 && out_avg_current_a) {
        *out_avg_current_a = sum_a / (float)samples;
    }
    /* M12: NaN, not 0, whenever there is nothing real to report -- a zone
     * that aborted, took no samples, or ran without a per-channel sampler
     * must read as "cannot tell" to zone_sweep_derive_ct_channel(), never as
     * three channels that all measured zero. */
    if (out_per_ch_avg_a) {
        bool have = (outcome == ZONE_SWEEP_ZONE_OK) && samples > 0 && deps->sample_channels != NULL;
        for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
            out_per_ch_avg_a[c] = have ? (ch_sum_a[c] / (float)samples) : NAN;
        }
    }
    return outcome;
}

/* ---- Real hardware bindings for zone_sweep_zone_deps_t, used only by
 * zone_sweep_task() below -- host tests supply their own fakes instead and
 * never link these. */
static kiln_io_owner_relay_result_t zone_sweep_hw_energize(void *ctx, uint8_t relay_mask,
                                                            uint32_t *out_safety_sources)
{
    (void)ctx;
    /* B1: MANUAL, not AUTHORIZED -- deliberate choice (opus review). The
     * AUTHORIZED entry point exists for profile_executor.c/autotune_engine.c
     * because THEY already apply their own zone-level ownership/safety gate
     * (relay_authority_zone_blocked()) before calling in; this module has no
     * equivalent gate of its own; a sweep is not "the owner" of anything the
     * way a running profile zone is. MANUAL is also the one that puts
     * ota_http_heat_blocked_by_update()'s OTA gate on every write (via
     * kiln_io_owner.c's relay_on_blocked()) -- B2's other half: even if the
     * forward interlock (autotune/profile/OTA refusing to START while a
     * sweep is active) were somehow bypassed, MANUAL still refuses to
     * energize while an update is genuinely in flight, mid-sweep, the same
     * way it already refuses a manual dashboard relay-on. ERR_OWNED is the
     * expected refusal if a profile/autotune run is (impossibly, given B2's
     * forward interlock) racing this sweep for the same relay -- fail
     * closed either way.
     *
     * N9 (opus review, 2026-08-28): mask is 0xFF, not relay_mask -- this
     * asserts an all-others-off precondition on every energize write, not
     * just "make sure this zone's relay(s) are on". A relay latched on from
     * the dashboard (or left on by a profile that just ended) before the
     * sweep started used to keep whatever state it had: if it shared a CT
     * channel with the zone under test, its load landed on that channel too
     * and zone_normals_set() persisted the inflated total as the zone's
     * measured "normal" -- permanently poisoning
     * zones_ct_mapping_mismatch()'s reference. zones_current_sweep_start()
     * also now refuses to start at all while any relay shadow bit is set
     * (ZONE_SWEEP_REFUSE_RELAYS_ON), so this is belt-and-suspenders: the
     * start-time refusal is the primary defense, this write is what keeps
     * every OTHER relay off for the whole 5s a zone is actually measured. */
    return kiln_io_owner_command_set_relay_mask(0xFFu, relay_mask, out_safety_sources);
}

static void zone_sweep_hw_force_off(void *ctx)
{
    (void)ctx;
    zone_sweep_force_relays_off();
}

static void zone_sweep_hw_read_temp(void *ctx, uint8_t zi, float *out_c, bool *out_valid)
{
    (void)ctx;
    zone_sweep_read_zone_temp(zi, out_c, out_valid);
}

static float zone_sweep_hw_sample_current(void *ctx, uint8_t zi)
{
    (void)ctx;
    uint8_t ctmask = 0;
    zones_config_get_ct_mask(zi, &ctmask);
    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    if (!s_hw_safety || safety_link_get_status(s_hw_safety, &st) != ESP_OK) {
        return 0.0f;
    }
    float live_a = 0.0f;
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if (ctmask & (1u << c)) {
            live_a += st.current_a[c];
        }
    }
    return live_a;
}

/* M12: the same safety_link_get_status() snapshot zone_sweep_hw_sample_
 * current() reads, handed over WITHOUT the ct_mask summing -- see
 * zone_sweep_zone_deps_t::sample_channels for why the mask must not be
 * applied here. A link read that fails leaves every channel NaN, which
 * zone_sweep_derive_ct_channel() reads as "cannot tell". */
static void zone_sweep_hw_sample_channels(void *ctx, float *out_a)
{
    (void)ctx;
    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    bool ok = s_hw_safety && safety_link_get_status(s_hw_safety, &st) == ESP_OK;
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        out_a[c] = ok ? st.current_a[c] : NAN;
    }
}

static bool zone_sweep_hw_link_up(void *ctx)
{
    (void)ctx;
    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    return s_hw_safety && safety_link_get_status(s_hw_safety, &st) == ESP_OK && st.link_up;
}

/* N10 (opus review, 2026-08-28): same fault_asserted/diag-TRIPPED test
 * zones_current_sweep_start() uses to refuse a START, applied mid-run too --
 * see zone_sweep_run_one_zone()'s comment for why this loop needs it. */
static bool zone_sweep_hw_trip_latched(void *ctx)
{
    (void)ctx;
    if (!s_hw_safety) {
        return false;
    }
    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    if (safety_link_get_status(s_hw_safety, &st) != ESP_OK) {
        return false;
    }
    return st.fault_asserted || (st.diag_ever_received && st.diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED);
}

static bool zone_sweep_hw_abort_requested(void *ctx)
{
    (void)ctx;
    return s_sweep.abort_requested;
}

static void zone_sweep_hw_delay_poll(void *ctx)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ZONE_SWEEP_POLL_MS));
}

/* ---- M3 (opus review, 2026-08-28): the whole-sweep loop, extracted from
 * zone_sweep_task() below into a form the host tests can drive without
 * xTaskCreate() ever running -- the same reason zone_sweep_run_one_zone()
 * was extracted from it in the earlier pass. Before this, "no two zones on
 * at once" (the comment on zone_sweep_task() below calls it "structural, not
 * a convention") was argued from reading the code, never actually exercised:
 * xTaskCreate() is stubbed under test, so this loop's body never ran.
 * zone_sweep_run_all_zones() is that same loop body, dependency-injected the
 * same way zone_sweep_run_one_zone() is -- `deps` for the per-zone hardware
 * calls, `hooks` for the per-sweep bookkeeping (which zone owns which relay
 * mask, live status updates, persisting a measured normal) that used to be
 * s_sweep/s_zones/zone_normals_set() called directly inline. */
typedef struct {
    uint8_t (*relay_mask_for_zone)(void *ctx, uint8_t zi);
    void (*set_zone_index)(void *ctx, uint8_t zi);      /* only for a zone actually being measured */
    void (*record_normal)(void *ctx, uint8_t zi, float avg_a); /* only when avg_a is a real sample */
    /* M12: this zone's per-CT-channel averages, handed over for every zone
     * that completed -- including the ambiguous ones, so the hook can say
     * WHICH zone it could not resolve rather than the sweep silently ending
     * with fewer channels mapped than zones swept. */
    void (*record_ct_channels)(void *ctx, uint8_t zi, uint8_t relay_mask, const float *per_ch_avg_a);
    void (*zone_done)(void *ctx);                        /* once per ZONE_SWEEP_ZONE_OK zone, live */
    void *ctx;
} zone_sweep_all_hooks_t;

typedef struct {
    zone_sweep_state_t state; /* ZONE_SWEEP_DONE / _ABORTED / _FAILED */
    char               reason[64];
    uint8_t            zones_done;
} zone_sweep_all_result_t;

static void zone_sweep_run_all_zones(uint8_t zones_total, const zone_sweep_zone_deps_t *deps,
                                      const zone_sweep_all_hooks_t *hooks, zone_sweep_all_result_t *out)
{
    out->state = ZONE_SWEEP_DONE;
    out->reason[0] = '\0';
    out->zones_done = 0;

    for (uint8_t zi = 0; zi < zones_total; zi++) {
        uint8_t relay_mask = hooks->relay_mask_for_zone(hooks->ctx, zi);
        if (relay_mask != 0 && hooks->set_zone_index) {
            hooks->set_zone_index(hooks->ctx, zi);
        }

        float avg_a = NAN; /* stays NaN unless zone_sweep_run_one_zone() got >=1 sample */
        float per_ch_avg_a[ZONE_CT_CHANNEL_COUNT];
        uint32_t refused_sources = 0;
        zone_sweep_zone_outcome_t outcome =
            zone_sweep_run_one_zone(zi, relay_mask, deps, &avg_a, per_ch_avg_a, &refused_sources);

        switch (outcome) {
        case ZONE_SWEEP_ZONE_SKIPPED:
            continue; /* nothing wired to this zone -- nothing to measure */
        case ZONE_SWEEP_ZONE_ABORTED:
            snprintf(out->reason, sizeof(out->reason), "aborted");
            out->state = ZONE_SWEEP_ABORTED;
            return;
        case ZONE_SWEEP_ZONE_CEILING_HIT:
            snprintf(out->reason, sizeof(out->reason), "zone %u reached its temperature ceiling", zi);
            out->state = ZONE_SWEEP_FAILED;
            return;
        case ZONE_SWEEP_ZONE_LINK_LOST:
            snprintf(out->reason, sizeof(out->reason), "safety link dropped during zone %u", zi);
            out->state = ZONE_SWEEP_FAILED;
            return;
        case ZONE_SWEEP_ZONE_TRIP_LATCHED:
            /* N10: not a heat hazard by itself -- the Pico opens its own
             * contactor independently of anything this ESP does -- but the
             * sweep must not grind on for the rest of the dwell, or refuse
             * every later zone at the energize step without saying why. */
            snprintf(out->reason, sizeof(out->reason), "safety trip latched during zone %u", zi);
            out->state = ZONE_SWEEP_FAILED;
            return;
        case ZONE_SWEEP_ZONE_TEMP_LOST:
            /* N1: the ceiling abort went blind -- see zone_sweep_run_one_zone()'s comment. */
            snprintf(out->reason, sizeof(out->reason), "zone %u lost its temperature reading mid-sweep", zi);
            out->state = ZONE_SWEEP_FAILED;
            return;
        case ZONE_SWEEP_ZONE_ENERGIZE_REFUSED:
            /* B1: the owner refused the ON write -- most likely a firmware
             * update started mid-sweep (ERR_UPDATING) or a safety fault
             * asserted mid-sweep (ERR_SAFETY), the exact gap the direct
             * kiln_io_set_relay_mask() call used to have no way to see. */
            /* ROADMAP.md M13: decode the fault-source mask instead of
             * showing a bare hex value. out->reason is char[64] (matching
             * zones_http.h's zone_sweep_status_t), and the full comma-joined
             * safety_fault_source_words() sentence can run to 141 bytes (see
             * safety_trip_words.h's comment), so the full decode does not
             * fit here -- take just the first asserted source's name and
             * note "(+more)" if others are also set, same shortening
             * ui_page_temperature.c's relay-refusal message uses.
             *
             * Kept short and unconditionally non-truncating: zi is uint8_t
             * (max 3 digits), and the "%.16s" precision (not just a big
             * buffer) is what lets -Werror=format-truncation prove this can
             * never overflow out->reason regardless of how long the decoded
             * word actually is: "zone " + 3 + " energize refused: " + 16 +
             * " (+more)" = 5 + 3 + 19 + 16 + 8 = 51 bytes, plus the NUL,
             * fits in 64. */
            {
                char src_words[160];
                safety_fault_source_words(refused_sources, src_words, sizeof(src_words));
                char *comma = strchr(src_words, ',');
                bool more = (comma != NULL);
                if (comma != NULL) {
                    *comma = '\0';
                }
                snprintf(out->reason, sizeof(out->reason), "zone %u energize refused: %.16s%s", zi, src_words,
                         more ? " (+more)" : "");
            }
            out->state = ZONE_SWEEP_FAILED;
            return;
        case ZONE_SWEEP_ZONE_OK:
        default:
            /* M3: the OK-with-zero-samples path -- outcome can be OK with
             * samples == 0 if the zone's whole dwell elapsed without ever
             * reaching zone_sweep_should_sample()'s window (not reachable
             * with today's fixed SETTLE/ENERGIZE constants, but the
             * aggregation logic itself does not assume that and is tested
             * as such below). avg_a stays NaN in that case and must not be
             * recorded as a measured normal, but the zone still counts as
             * "done" -- it completed without aborting/failing. */
            if (!isnan(avg_a) && hooks->record_normal) {
                hooks->record_normal(hooks->ctx, zi, avg_a);
            }
            if (hooks->record_ct_channels) {
                hooks->record_ct_channels(hooks->ctx, zi, relay_mask, per_ch_avg_a);
            }
            out->zones_done++;
            if (hooks->zone_done) {
                hooks->zone_done(hooks->ctx);
            }
            break;
        }
    }
}

static uint8_t zone_sweep_task_relay_mask_for_zone(void *ctx, uint8_t zi)
{
    (void)ctx;
    return s_zones.cfg.zones[zi].relay_mask;
}

static void zone_sweep_task_set_zone_index(void *ctx, uint8_t zi)
{
    (void)ctx;
    s_sweep.zone_index = zi;
}

static void zone_sweep_task_record_normal(void *ctx, uint8_t zi, float avg_a)
{
    (void)ctx;
    zone_normals_set(zi, avg_a);
}

/* ---- M12: the derived CT map, accumulated across one sweep run ------------
 * Written only by the sweep task (one at a time, enforced by s_sweep.active
 * and the heat claim), read only by zone_sweep_push_ct_channel_map() on that
 * same task, so unlike s_sweep this needs no volatile. Reset by
 * zone_sweep_task() before the loop starts. */
static struct {
    uint8_t zone_for_ch[ZONE_CT_CHANNEL_COUNT];
    uint8_t derived_mask;      /* channels resolved unambiguously this run */
    uint8_t conflict_mask;     /* channels TWO zones both claimed -- see below */
    uint8_t unresolved_zone_mask;
    /* M12b: the measured half of the k_ct calibration -- the sum, over every
     * zone this run resolved, of that zone's dominant CT channel reading.
     * Accumulated here (not recomputed later) because the per-zone
     * per-channel averages exist only for the length of one
     * zone_sweep_run_all_zones() iteration. */
    float   measured_total_a;
    /* M12b: whether zone_sweep_push_ct_channel_map() ended in a failure it
     * could not fully back out. The k_ct push must not run after one: both
     * write through the SAME staged-config buffer on the Pico, so committing
     * k_ct on top of a staged buffer known to hold ct_channel_map values
     * that were meant to be discarded would commit exactly the leftovers
     * that function's H3 repair exists to prevent. */
    bool    map_push_failed;
} s_ct_derive;

static void zone_sweep_task_record_ct_channels(void *ctx, uint8_t zi, uint8_t relay_mask,
                                                const float *per_ch_avg_a)
{
    (void)ctx;
    uint8_t ch = 0;
    /* ct_channel_map[] is indexed into the Pico's relay_now_mask directly
     * (safety_core.c: `ctx.relay_now_mask & (1u << ct_channel_map[ch])`), so
     * the value written has to be a RELAY bit position, and the whole
     * mapping is only meaningful while zone id and relay id are the same
     * number -- which is what config_store.h's "zone/relay id" comment
     * assumes and what a stock three-zone board actually is. Refuse to
     * derive for any zone where that identity does not hold (more than one
     * relay, or a relay outside bits 0-2): the sweep genuinely cannot say
     * which single relay a channel's current belongs to, and writing the
     * zone index anyway would point S14 at some other zone's relay. */
    if (relay_mask != (uint8_t)(1u << zi) || zi >= 3u) {
        if (zi < 8) {
            s_ct_derive.unresolved_zone_mask |= (uint8_t)(1u << zi);
        }
        return;
    }
    if (!zone_sweep_derive_ct_channel(per_ch_avg_a, &ch)) {
        if (zi < 8) {
            s_ct_derive.unresolved_zone_mask |= (uint8_t)(1u << zi);
        }
        return;
    }
    /* Two zones dominating the SAME channel is not a mapping -- it is the
     * one-to-one inversion COMMISSIONING_UX.md sec 1.2 requires failing, and
     * it fails in a direction no per-zone check can see (each zone looked
     * perfectly unambiguous on its own). Drop the channel entirely rather
     * than letting whichever zone swept last win. */
    if ((s_ct_derive.derived_mask & (1u << ch)) != 0 && s_ct_derive.zone_for_ch[ch] != zi) {
        s_ct_derive.derived_mask &= (uint8_t)~(1u << ch);
        s_ct_derive.conflict_mask |= (uint8_t)(1u << ch);
        return;
    }
    if ((s_ct_derive.conflict_mask & (1u << ch)) != 0) {
        return; /* already disqualified by an earlier zone -- a third claimant changes nothing */
    }
    s_ct_derive.zone_for_ch[ch] = zi;
    s_ct_derive.derived_mask |= (uint8_t)(1u << ch);
    /* M12b: this zone's contribution to the whole-kiln total. Added only on
     * the unambiguous path -- a zone whose channel could not be resolved is
     * a hole in the total, and zone_sweep_push_k_ct_v_per_a() refuses to
     * calibrate from an incomplete one rather than under-counting the kiln
     * and scaling k_ct down to match. */
    s_ct_derive.measured_total_a += per_ch_avg_a[ch];
}

/* The wire id of ct_channel_map[ch] -- one place, so the staging, the
 * unstaging and the read-back can never drift apart. */
#define ZONE_CT_MAP_PARAM_ID(ch) ((uint16_t)(0x0106u + (ch)))

/* config_store.c seeds an uncommissioned record's ct_channel_map[] with
 * 0xFF, "a visibly implausible relay/zone id" -- S14 compares
 * `relay_now_mask & (1u << ct_channel_map[ch])`, and no relay bit 255
 * exists, so a channel left holding this can never point the over-current
 * guard at somebody else's relay. It is the only value this file is allowed
 * to invent, and only ever as a repair for a channel that had no committed
 * value to restore. */
#define ZONE_CT_MAP_IMPLAUSIBLE 0xFFu

/* The value the Pico has actually COMMITTED for ct_channel_map[ch],
 * according to the ESP's cache of its record. False when the cache has never
 * seen that field set (an uncommissioned board), in which case there is no
 * prior value to restore. */
static bool zone_ct_map_committed_value(uint8_t ch, uint8_t *out)
{
    size_t count = safety_cfg_store_param_count();
    for (size_t i = 0; i < count; i++) {
        safety_cfg_param_t row;
        memset(&row, 0, sizeof(row));
        if (!safety_cfg_store_get_by_index(i, &row) || row.param_id != ZONE_CT_MAP_PARAM_ID(ch)) {
            continue;
        }
        if (!row.set) {
            return false;
        }
        if (out) {
            *out = row.value.u8_val;
        }
        return true;
    }
    return false;
}

/* H3 fix (opus review, 2026-08-28): a SET_PARAM that fails PART WAY through
 * the three-channel push used to just `break`, which leaves the Pico's
 * link_task.c s_staged_config holding whatever channels DID stage. That
 * buffer is a persistent baseline -- seeded once from the committed record
 * and only re-seeded by a SUCCESSFUL COMMIT_CONFIG or a reboot
 * (link_task.c's s_staged_config_init, reset only at task start) -- so the
 * next unrelated COMMIT_CONFIG, e.g. the operator saving one field on the
 * commissioning page, would have carried this abandoned sweep's leftover
 * ct_channel_map[] into flash as though it had been commissioned.
 *
 * The link protocol has no "discard staged config" command (uart_task_ids.h
 * enumerates every subcommand on this wire; SET_CONFIG/SET_CT_CAL/COMMIT are
 * all it offers for the config record), and inventing a new wire command for
 * a failure path is not worth a protocol change. So the leftovers are
 * OVERWRITTEN instead: each channel that staged is re-staged back to the
 * value the Pico has actually committed, and, for a channel that has never
 * been committed at all, to config_store.c's own 0xFF placeholder. Either
 * way the staged buffer ends up holding a record that is safe to commit --
 * which is the only property that matters, since this code cannot stop
 * somebody else committing it.
 *
 * Best-effort by construction: this runs because the link already failed
 * once. A repair send that fails too is reported in `note` rather than
 * retried -- the caller has already decided this sweep derived nothing. */
static void zone_sweep_unstage_ct_channels(uint8_t staged_mask, char *note, size_t note_cap)
{
    if (staged_mask == 0 || !s_hw_safety) {
        return;
    }
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if ((staged_mask & (1u << c)) == 0) {
            continue;
        }
        uint8_t restore = ZONE_CT_MAP_IMPLAUSIBLE;
        (void)zone_ct_map_committed_value(c, &restore);
        kilnlink_param_value_t v;
        memset(&v, 0, sizeof(v));
        v.u8_val = restore;
        if (safety_link_send_set_param(s_hw_safety, ZONE_CT_MAP_PARAM_ID(c),
                                       KILNLINK_PARAM_TYPE_U8, v) != ESP_OK) {
            snprintf(note, note_cap,
                     "CT map staging failed and could NOT be backed out -- re-run the sweep "
                     "before saving again");
            return;
        }
    }
}

/* H1 fix (opus review, 2026-08-28): an ACKed, un-rejected COMMIT_CONFIG is
 * NOT proof the Pico stored anything -- SET_PARAM/COMMIT_CONFIG are both
 * fire-and-forget broadcasts and a late REJECTED frame can miss the reply
 * window, which is exactly why safety_cfg_http.c's confirm_commit_landed()
 * exists for the hand-typed path. That function is static to its own file
 * (and takes safety_cfg_post_pair_t text pairs this caller has none of), so
 * its VERIFICATION is reproduced here rather than shared: force a live
 * re-fetch of the Pico's record -- never the ESP's own cache as it stands --
 * and require every channel this push claims to have written to read back
 * exactly the zone index that was sent. Anything else (unreadable, unset, or
 * set to something different) is reported as unconfirmed, and the caller
 * then reports the map as NOT derived, so the field falls back to manual
 * entry exactly as it does when no sweep has run. */
static bool zone_sweep_confirm_ct_map_landed(uint8_t mask, char *reason, size_t reason_cap)
{
    uint16_t best_known_crc = 0;
    bool peer_known = false;
    (void)safety_link_get_peer_build_status(s_hw_safety, &peer_known, NULL, NULL, NULL, NULL, NULL,
                                             NULL, &best_known_crc);
    if (!safety_cfg_store_refetch(s_hw_safety, peer_known ? best_known_crc : 0)) {
        snprintf(reason, reason_cap,
                 "the CT map commit could not be read back to confirm it -- treated as "
                 "NOT written");
        return false;
    }
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if ((mask & (1u << c)) == 0) {
            continue;
        }
        uint8_t committed = 0;
        if (!zone_ct_map_committed_value(c, &committed) || committed != s_ct_derive.zone_for_ch[c]) {
            snprintf(reason, reason_cap,
                     "ct_channel_map[%u] does not read back as zone %u -- treated as "
                     "NOT written",
                     (unsigned)c, (unsigned)s_ct_derive.zone_for_ch[c]);
            return false;
        }
    }
    return true;
}

/* Stages every channel this run resolved as SET_PARAM 0x0106+ch (u8, the
 * zone index) and commits, exactly the path safety_cfg_http.c's apply_pairs()
 * uses for a hand-typed value -- the Pico cannot and must not tell the
 * difference between a derived write and a typed one. The Pico's own
 * config_params_finalize_ct_channel_map() runs inside its COMMIT_CONFIG
 * handler (link_task.c) and is what marks the group commissioned once all
 * three per-channel bits are present, so there is deliberately nothing here
 * that waits for a complete triple before sending: a partial derivation
 * stages the channels it actually confirmed and leaves the group bit unset,
 * which is exactly what that function already does with two of three.
 *
 * Only ever called after the sweep's relays are off and the run has ended --
 * safety_link_send_set_param()/_commit_config() both block for a link round
 * trip, and this must not sit inside a window where a relay is energized. */
static void zone_sweep_push_ct_channel_map(void)
{
    char note[sizeof(s_sweep.ct_map_reason)];
    note[0] = '\0';
    /* Scoped to the whole function, not just the staging loop below: every
     * failure arm past the loop (commit unacked, commit rejected, commit
     * acked but the read-back doesn't confirm it landed) still needs to know
     * which channels made it into the Pico's staged buffer, so it can back
     * them out the same way a staging failure already does -- see H3's
     * comment on zone_sweep_unstage_ct_channels() above. Left at 0 (a no-op
     * for that function) on every path that never reaches the loop. */
    uint8_t staged_mask = 0;

    if (s_ct_derive.derived_mask == 0) {
        snprintf(note, sizeof(note), "no CT channel could be identified -- map not changed");
    } else if (!s_hw_safety) {
        snprintf(note, sizeof(note), "safety link not available -- CT map not written");
        s_ct_derive.derived_mask = 0;
        /* Nothing staged, so nothing to back out -- but there is also no
         * link for the k_ct push to use, and its own !s_hw_safety arm says
         * so. Not flagged as a push FAILURE: the staged buffer is untouched. */
    } else {
        for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
            if ((s_ct_derive.derived_mask & (1u << c)) == 0) {
                continue;
            }
            kilnlink_param_value_t v;
            memset(&v, 0, sizeof(v));
            v.u8_val = s_ct_derive.zone_for_ch[c];
            esp_err_t err = safety_link_send_set_param(s_hw_safety, ZONE_CT_MAP_PARAM_ID(c),
                                                       KILNLINK_PARAM_TYPE_U8, v);
            if (err != ESP_OK) {
                /* %.24s, not a bare %s, for the same reason the ambiguity
                 * note below uses one -- see its comment. */
                snprintf(note, sizeof(note), "staging ct_channel_map[%u] failed: %.24s", c,
                         esp_err_to_name(err));
                /* H3: whatever already staged must not be left sitting in the
                 * Pico's staged record for an unrelated commit to pick up. */
                zone_sweep_unstage_ct_channels(staged_mask, note, sizeof(note));
                s_ct_derive.derived_mask = 0;
                s_ct_derive.map_push_failed = true;
                break;
            }
            staged_mask |= (uint8_t)(1u << c);
        }
    }

    if (s_ct_derive.derived_mask != 0) {
        uint16_t reject_param_id = 0;
        uint8_t reject_reason = 0;
        bool rejected = false;
        esp_err_t err = safety_link_send_commit_config(s_hw_safety, &reject_param_id, &reject_reason,
                                                        &rejected);
        if (err != ESP_OK) {
            snprintf(note, sizeof(note), "CT map staged but the commit was not "
                                          "acknowledged (%.24s)", esp_err_to_name(err));
            /* An unacked commit's fate on the Pico is unknown -- it may not
             * have applied, in which case the staged buffer this loop wrote
             * is still sitting there for an unrelated later commit to pick
             * up. Same H3 exposure as a staging-loop failure, so the same
             * repair. */
            zone_sweep_unstage_ct_channels(staged_mask, note, sizeof(note));
            s_ct_derive.derived_mask = 0;
            s_ct_derive.map_push_failed = true;
        } else if (rejected) {
            /* Expected and legitimate when the relay is ARMED -- config
             * writes are refused outright then (CONFIG_REFERENCE.md). Say so
             * rather than leaving the operator to infer it from a map that
             * silently did not change. A rejected commit leaves the staged
             * buffer exactly as staged (rejection means nothing was
             * applied), so it still needs backing out. */
            snprintf(note, sizeof(note), "the safety processor rejected the CT map commit "
                                          "(id 0x%04X, reason %u)", (unsigned)reject_param_id,
                     (unsigned)reject_reason);
            zone_sweep_unstage_ct_channels(staged_mask, note, sizeof(note));
            s_ct_derive.derived_mask = 0;
            s_ct_derive.map_push_failed = true;
        } else if (!zone_sweep_confirm_ct_map_landed(s_ct_derive.derived_mask, note, sizeof(note))) {
            /* H1: ACKed and not rejected is not proof. Nothing is persisted
             * and no channel is reported as derived -- the operator sees the
             * reason and enters the map by hand, exactly as they would if the
             * sweep had never run. The read-back disagreeing is exactly the
             * case H3 exists for too: back the staged buffer out rather than
             * leave a value known not to match what was intended sitting
             * there for the next commit. */
            zone_sweep_unstage_ct_channels(staged_mask, note, sizeof(note));
            s_ct_derive.derived_mask = 0;
            s_ct_derive.map_push_failed = true;
        } else {
            for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
                if ((s_ct_derive.derived_mask & (1u << c)) != 0) {
                    zone_ct_map_set(c, s_ct_derive.zone_for_ch[c]);
                }
            }
        }
    }

    /* An ambiguity note never overwrites a hard failure above -- the failure
     * is the more actionable of the two -- but it is reported whenever the
     * write itself was fine and some zone still could not be resolved. */
    if (note[0] == '\0' && (s_ct_derive.unresolved_zone_mask != 0 || s_ct_derive.conflict_mask != 0)) {
        /* %.24s, not a bare %s, so -Werror=format-truncation can prove this
         * fits note[] whatever the two literals grow into later. */
        snprintf(note, sizeof(note), "CT map incomplete (%.24s) -- enter the rest by hand",
                 s_ct_derive.conflict_mask != 0 ? "two zones share one CT" : "a zone was ambiguous");
    }

    s_sweep.ct_map_derived_mask = s_ct_derive.derived_mask;
    strncpy((char *)s_sweep.ct_map_reason, note, sizeof(s_sweep.ct_map_reason) - 1);
    s_sweep.ct_map_reason[sizeof(s_sweep.ct_map_reason) - 1] = '\0';
}

/* ---- M12b: pushing the derived k_ct_v_per_a ------------------------------
 * Deliberately a MIRROR of the ct_channel_map push above rather than a
 * generalisation of it: the two share a shape (stage / commit / read-back /
 * back out) but not a single decision -- different param ids, a different
 * wire type, a different restore placeholder, a different definition of
 * "landed", and a completely different rule for what may be derived at all.
 * Folding them into one parameterised routine would mean every future change
 * to one has to be argued for the other, which is exactly the coupling the
 * CT-map push's own H1/H3 comments were written to avoid. */
#define ZONE_KCT_PARAM_ID(ch) ((uint16_t)(0x0308u + (ch)))

/* The two commissioning answers this calibration reads (COMMISSIONING_UX.md
 * sec 2, Q3 and Q4). Both are F32 on the wire. */
#define ZONE_MAINS_VOLTAGE_PARAM_ID  ((uint16_t)0x030Eu)
#define ZONE_MAX_POWER_PARAM_ID      ((uint16_t)0x0319u)

/* config_store.c leaves k_ct_v_per_a at its memset(0) -- the uncommissioned
 * value (that file's own comment at the seed). Unlike ct_channel_map's 0xFF,
 * 0.0f is not merely implausible but actively meaningful downstream:
 * current_presence_policy.c branches on k_ct_v_per_a <= 0.0f and falls back
 * to its deliberately sensitive counts-domain margin. Restoring 0.0f for a
 * channel that has never been committed therefore puts the Pico back on the
 * SAFE side of that branch, which is the right direction for a repair. */
#define ZONE_KCT_UNCOMMISSIONED 0.0f

/* The F32 value the Pico has actually COMMITTED for `param_id`, according to
 * the ESP's cache of its record -- the f32 twin of
 * zone_ct_map_committed_value() above, and it answers false in the same two
 * cases (no such row, or a row the Pico has never had set). */
static bool zone_cfg_committed_f32(uint16_t param_id, float *out)
{
    size_t count = safety_cfg_store_param_count();
    for (size_t i = 0; i < count; i++) {
        safety_cfg_param_t row;
        memset(&row, 0, sizeof(row));
        if (!safety_cfg_store_get_by_index(i, &row) || row.param_id != param_id) {
            continue;
        }
        if (!row.set) {
            return false;
        }
        if (out) {
            *out = row.value.f32_val;
        }
        return true;
    }
    return false;
}

/* H3's repair, for this push -- see zone_sweep_unstage_ct_channels()'s own
 * comment for the full reasoning about why leftovers in the Pico's staged
 * buffer are a real hazard and why overwriting is the only available
 * remedy. Best-effort by construction, for the same reason. */
static void zone_sweep_unstage_k_ct(uint8_t staged_mask, char *note, size_t note_cap)
{
    if (staged_mask == 0 || !s_hw_safety) {
        return;
    }
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if ((staged_mask & (1u << c)) == 0) {
            continue;
        }
        float restore = ZONE_KCT_UNCOMMISSIONED;
        (void)zone_cfg_committed_f32(ZONE_KCT_PARAM_ID(c), &restore);
        kilnlink_param_value_t v;
        memset(&v, 0, sizeof(v));
        v.f32_val = restore;
        if (safety_link_send_set_param(s_hw_safety, ZONE_KCT_PARAM_ID(c),
                                       KILNLINK_PARAM_TYPE_F32, v) != ESP_OK) {
            snprintf(note, note_cap,
                     "CT scale staging failed and could NOT be backed out -- re-run the "
                     "sweep before saving again");
            return;
        }
    }
}

/* H1's verification, for this push. An ACKed, un-rejected COMMIT_CONFIG is
 * not proof anything was stored (zone_sweep_confirm_ct_map_landed()'s
 * comment) -- force a LIVE re-fetch and require every channel written to
 * read back as exactly the float that was sent. Exact equality is right
 * here, not a tolerance: the wire, config_store's record and this cache all
 * carry the identical IEEE-754 f32, so anything other than bit-equality
 * means the value did not land, not that it landed imprecisely. */
static bool zone_sweep_confirm_k_ct_landed(uint8_t mask, const float *k_new, char *reason,
                                            size_t reason_cap)
{
    uint16_t best_known_crc = 0;
    bool peer_known = false;
    (void)safety_link_get_peer_build_status(s_hw_safety, &peer_known, NULL, NULL, NULL, NULL, NULL,
                                             NULL, &best_known_crc);
    if (!safety_cfg_store_refetch(s_hw_safety, peer_known ? best_known_crc : 0)) {
        snprintf(reason, reason_cap,
                 "the CT scale commit could not be read back to confirm it -- treated as "
                 "NOT written");
        return false;
    }
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if ((mask & (1u << c)) == 0) {
            continue;
        }
        float committed = 0.0f;
        if (!zone_cfg_committed_f32(ZONE_KCT_PARAM_ID(c), &committed) || committed != k_new[c]) {
            snprintf(reason, reason_cap,
                     "k_ct_v_per_a[%u] does not read back as written -- treated as NOT written",
                     (unsigned)c);
            return false;
        }
    }
    return true;
}

/* Decides what this run may calibrate, ahead of touching the link at all --
 * separated from the staging below so the whole decision (including every
 * refusal) is reachable from a host test without a fake link. Returns the
 * mask of channels to write, filling out_k[] for each, and `note` with the
 * reason whenever that mask comes back 0. */
static uint8_t zone_sweep_plan_k_ct(float *out_k, char *note, size_t note_cap)
{
    if (s_ct_derive.derived_mask == 0) {
        snprintf(note, note_cap, "no CT channel was identified -- CT scale not calibrated");
        return 0;
    }
    /* An incomplete pass cannot be summed into a whole-kiln total: a zone
     * that did not resolve still drew its current, so its absence would drag
     * the measured total down and scale k_ct with it -- silently, and in the
     * direction that makes every later reading read LOW. Same reason the map
     * push refuses to derive anything from a partial run. */
    if (s_ct_derive.unresolved_zone_mask != 0 || s_ct_derive.conflict_mask != 0) {
        /* Kept to 94 chars + NUL so it fits k_ct_reason[96] -- the longer
         * wording this replaced was 111 bytes and broke the build under
         * -Werror=format-truncation, which is the compiler correctly
         * refusing to let a reason string be silently cut in half on the
         * status page. Same meaning, no truncation. */
        snprintf(note, note_cap,
                 "not every zone resolved to a CT -- an incomplete total would scale k_ct low, "
                 "so it was not set");
        return 0;
    }
    if (s_ct_derive.map_push_failed) {
        snprintf(note, note_cap, "the CT map write failed -- CT scale not calibrated");
        return 0;
    }
    float mains_v = 0.0f, power_w = 0.0f;
    if (!zone_cfg_committed_f32(ZONE_MAINS_VOLTAGE_PARAM_ID, &mains_v) ||
        !zone_cfg_committed_f32(ZONE_MAX_POWER_PARAM_ID, &power_w)) {
        snprintf(note, note_cap, "CT scale not calibrated: %.72s",
                 zone_kct_derive_str(ZONE_KCT_DERIVE_NO_NAMEPLATE));
        return 0;
    }
    uint8_t plan_mask = 0;
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if ((s_ct_derive.derived_mask & (1u << c)) == 0) {
            continue;
        }
        float k_old = 0.0f;
        if (!zone_cfg_committed_f32(ZONE_KCT_PARAM_ID(c), &k_old)) {
            k_old = 0.0f; /* never committed -- zone_sweep_derive_k_ct() refuses on it */
        }
        float k_new = 0.0f;
        zone_kct_derive_t r =
            zone_sweep_derive_k_ct(s_ct_derive.measured_total_a, power_w, mains_v, k_old, &k_new);
        if (r != ZONE_KCT_DERIVE_OK) {
            /* One channel's refusal ends the whole calibration rather than
             * calibrating the others: the scale factor is a property of the
             * measurement, not of a channel, so a channel that cannot take
             * it means this run's own inputs are unusable. Writing the rest
             * would leave the three channels on different scales with
             * nothing recording that they disagree. */
            snprintf(note, note_cap, "CT scale not calibrated: %.72s", zone_kct_derive_str(r));
            return 0;
        }
        out_k[c] = k_new;
        plan_mask |= (uint8_t)(1u << c);
    }
    return plan_mask;
}

/* Stages every planned channel as SET_PARAM 0x0308+c (f32) and commits --
 * the same path a hand-typed value takes through safety_cfg_http.c, since
 * the Pico must not be able to tell a derived write from a typed one.
 *
 * Called only from zone_sweep_task(), immediately after
 * zone_sweep_push_ct_channel_map(), with every relay already off: both
 * senders block for a link round trip and neither may sit inside a window
 * where an element is energized. */
static void zone_sweep_push_k_ct_v_per_a(void)
{
    char note[sizeof(s_sweep.k_ct_reason)];
    note[0] = '\0';
    float k_new[ZONE_CT_CHANNEL_COUNT] = {0};
    uint8_t staged_mask = 0;

    uint8_t plan_mask = zone_sweep_plan_k_ct(k_new, note, sizeof(note));
    if (plan_mask != 0 && !s_hw_safety) {
        snprintf(note, sizeof(note), "safety link not available -- CT scale not written");
        plan_mask = 0;
    }

    if (plan_mask != 0) {
        for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
            if ((plan_mask & (1u << c)) == 0) {
                continue;
            }
            kilnlink_param_value_t v;
            memset(&v, 0, sizeof(v));
            v.f32_val = k_new[c];
            esp_err_t err = safety_link_send_set_param(s_hw_safety, ZONE_KCT_PARAM_ID(c),
                                                       KILNLINK_PARAM_TYPE_F32, v);
            if (err != ESP_OK) {
                snprintf(note, sizeof(note), "staging k_ct_v_per_a[%u] failed: %.24s", c,
                         esp_err_to_name(err));
                zone_sweep_unstage_k_ct(staged_mask, note, sizeof(note));
                plan_mask = 0;
                break;
            }
            staged_mask |= (uint8_t)(1u << c);
        }
    }

    if (plan_mask != 0) {
        uint16_t reject_param_id = 0;
        uint8_t reject_reason = 0;
        bool rejected = false;
        esp_err_t err = safety_link_send_commit_config(s_hw_safety, &reject_param_id, &reject_reason,
                                                        &rejected);
        if (err != ESP_OK) {
            snprintf(note, sizeof(note), "CT scale staged but the commit was not "
                                          "acknowledged (%.24s)", esp_err_to_name(err));
            zone_sweep_unstage_k_ct(staged_mask, note, sizeof(note));
            plan_mask = 0;
        } else if (rejected) {
            snprintf(note, sizeof(note), "the safety processor rejected the CT scale commit "
                                          "(id 0x%04X, reason %u)", (unsigned)reject_param_id,
                     (unsigned)reject_reason);
            zone_sweep_unstage_k_ct(staged_mask, note, sizeof(note));
            plan_mask = 0;
        } else if (!zone_sweep_confirm_k_ct_landed(plan_mask, k_new, note, sizeof(note))) {
            zone_sweep_unstage_k_ct(staged_mask, note, sizeof(note));
            plan_mask = 0;
        } else {
            for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
                if ((plan_mask & (1u << c)) != 0) {
                    (void)zone_k_ct_set(c, k_new[c]);
                }
            }
        }
    }

    s_sweep.k_ct_derived_mask = plan_mask;
    strncpy((char *)s_sweep.k_ct_reason, note, sizeof(s_sweep.k_ct_reason) - 1);
    s_sweep.k_ct_reason[sizeof(s_sweep.k_ct_reason) - 1] = '\0';
}

static void zone_sweep_task_zone_done(void *ctx)
{
    (void)ctx;
    s_sweep.zones_done++; /* live -- visible to a status poll while the sweep is still running */
}

/* Background task body -- the only place this module ever commands a relay
 * ON (via zone_sweep_run_one_zone()'s deps->energize, called from
 * zone_sweep_run_all_zones() above). One zone at a time is structural, not a
 * convention: zone_sweep_run_all_zones() runs exactly one zone per iteration
 * and that call's own choke point (zone_sweep_force_relays_off()) drops
 * EVERY relay before ever starting the next iteration or exiting -- there is
 * no code path in this function that can have two zones' relays on at once,
 * and test_zone_sweep_run_all_zones_never_energizes_two_zones_at_once()
 * (M3, opus review 2026-08-28) proves it against this exact function rather
 * than only arguing it from reading the code. */
static void zone_sweep_task(void *arg)
{
    (void)arg;
    static const zone_sweep_zone_deps_t hw_deps = {
        .energize = zone_sweep_hw_energize,
        .force_off = zone_sweep_hw_force_off,
        .read_temp = zone_sweep_hw_read_temp,
        .sample_current = zone_sweep_hw_sample_current,
        .sample_channels = zone_sweep_hw_sample_channels,
        .link_up = zone_sweep_hw_link_up,
        .trip_latched = zone_sweep_hw_trip_latched,
        .abort_requested = zone_sweep_hw_abort_requested,
        .delay_poll = zone_sweep_hw_delay_poll,
        .ctx = NULL,
    };
    static const zone_sweep_all_hooks_t hw_hooks = {
        .relay_mask_for_zone = zone_sweep_task_relay_mask_for_zone,
        .set_zone_index = zone_sweep_task_set_zone_index,
        .record_normal = zone_sweep_task_record_normal,
        .record_ct_channels = zone_sweep_task_record_ct_channels,
        .zone_done = zone_sweep_task_zone_done,
        .ctx = NULL,
    };

    memset(&s_ct_derive, 0, sizeof(s_ct_derive));
    zone_ct_map_clear(); /* a re-sweep must not leave a stale channel claim visible as current */
    zone_k_ct_clear();   /* M12b: same reasoning, for the derived CT scale */

    zone_sweep_all_result_t result;
    zone_sweep_run_all_zones(s_sweep.zones_total, &hw_deps, &hw_hooks, &result);

    /* DELIBERATELY not `s_sweep.state = result.state` for a successful run:
     * DONE is published below, only once zone_sweep_push_ct_channel_map()
     * has finished (see that call's comment). A failed/aborted run publishes
     * immediately -- it derives nothing, so there is nothing to wait for. */
    if (result.state != ZONE_SWEEP_DONE) {
        s_sweep.state = result.state;
    }
    strncpy((char *)s_sweep.reason, result.reason, sizeof(s_sweep.reason) - 1);
    s_sweep.reason[sizeof(s_sweep.reason) - 1] = '\0';

    zone_sweep_force_relays_off(); /* final choke point -- covers normal completion too */
    if (s_sweep.state != ZONE_SWEEP_ABORTED && s_sweep.state != ZONE_SWEEP_FAILED) {
        s_sweep.reason[0] = '\0';
        /* M12: only a run that finished every zone gets to write the CT map.
         * An aborted or failed sweep has measured some zones and not others,
         * and a partial pass cannot see the two-zones-one-channel conflict
         * that is the whole reason the one-to-one check exists -- deriving
         * from it would write a map that looks confirmed and is not. */
        zone_sweep_push_ct_channel_map();
        /* M12b: strictly AFTER the map push, never before or interleaved.
         * Both stage into the SAME staged-config buffer on the Pico and each
         * ends with its own COMMIT_CONFIG, so they have to be two complete
         * transactions in sequence; zone_sweep_plan_k_ct() additionally
         * refuses outright if the map push left that buffer in a state it
         * could not repair. */
        zone_sweep_push_k_ct_v_per_a();
        /* DONE goes up only AFTER the push has finished (opus review,
         * 2026-08-28). Setting it first left a window two link round trips
         * wide in which a status poll saw state=done with
         * ct_map_derived_mask still 0 and rendered a permanent "not
         * derived" -- the page never re-reads a sweep it has already seen
         * finish. The push runs entirely with the relays off either way; it
         * is only the moment the page is TOLD the run is over that moves. */
        s_sweep.state = ZONE_SWEEP_DONE;
    }
    /* Release the heat claim taken in zones_current_sweep_start() -- must
     * happen before s_sweep.active goes false, not after: the moment
     * s_sweep.active reads false, a waiting profile/autotune start can
     * observe it and attempt its own heat-zone claim; releasing first means
     * that claim is genuinely free the instant this sweep stops being
     * reachable, instead of leaving a window where the sweep looks finished
     * but still (briefly) holds exclusivity. */
    relay_authority_heat_sweep_claim_end();
    s_sweep.active = false;
    s_sweep.task = NULL;
    vTaskDelete(NULL);
}

zone_sweep_refusal_t zones_current_sweep_start(void)
{
    bool profile_running_or_paused = false;
    profile_exec_status_t pstat;
    memset(&pstat, 0, sizeof(pstat));
    profile_executor_get_status(&pstat);
    profile_running_or_paused = (pstat.state == PROFILE_EXEC_RUNNING || pstat.state == PROFILE_EXEC_PAUSED);

    bool link_up = false;
    bool trip_latched = false;
    if (s_hw_safety) {
        safety_link_status_t st;
        memset(&st, 0, sizeof(st));
        if (safety_link_get_status(s_hw_safety, &st) == ESP_OK) {
            link_up = st.link_up;
            trip_latched = st.fault_asserted || (st.diag_ever_received && st.diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED);
        }
    }

    /* H1 (opus review, 2026-08-27): have_hw used to be `s_hw_io != NULL`
     * alone. zone_sweep_read_zone_temp() early-returns invalid whenever
     * !s_hw_thermo_bus || !initialized, and zone_sweep_ceiling_hit() treats
     * an invalid reading as "not a ceiling hit" (by design -- see that
     * function's own comment). Together that means a board with relay I/O
     * but no thermo bus wired in could start a sweep that energizes real
     * elements with the ceiling abort structurally incapable of ever firing
     * -- no thermocouple reading ever arrives to trip it. Require the thermo
     * bus (and its own initialized flag) in have_hw too, so that gap refuses
     * up front (ZONE_SWEEP_REFUSE_NO_HW) instead of running unsupervised. */
    bool have_hw = (s_hw_io != NULL) && (s_hw_thermo_bus != NULL) && s_hw_thermo_bus->initialized;
    /* N9 (opus review, 2026-08-28): belt-and-suspenders with
     * zone_sweep_hw_energize()'s 0xFF all-others-off write. That write
     * already forces every OTHER relay off once a zone starts measuring, so
     * on its own it would be enough -- this check adds refusing to start at
     * all while anything is on, per zones_current_sweep_start()'s own
     * contract ("no relay is ever touched on a refused start"), and surfaces
     * the foreign-load condition to the operator explicitly instead of
     * silently overriding whatever they had on. */
    bool relays_on = s_hw_io && (kiln_io_get_relay_shadow(s_hw_io) != 0);
    zone_sweep_refusal_t refusal = zone_sweep_check_refusal(
        s_sweep.active, have_hw, s_zones_config_valid, s_zones.cfg.thermo_count,
        profile_running_or_paused, autotune_engine_is_active(), link_up, trip_latched, relays_on);
    if (refusal != ZONE_SWEEP_REFUSE_OK) {
        return refusal;
    }

    /* The atomic gate (relay_authority.h's heat-claim doc comment): every
     * check above, including profile_running_or_paused/autotune_active just
     * fed into zone_sweep_check_refusal(), is a plain read of another
     * module's state with no lock spanning the read and this function's own
     * commit just below -- exactly the TOCTOU a reviewer found bounded but
     * not correct-by-construction. This call is the last possible moment
     * before that commit, and it is a single mutex-protected test-and-set
     * against profile_executor.c's/autotune_engine.c's matching gate, so
     * whichever of the two commits first is the one that actually wins --
     * the loser is refused here with the SAME reason the informational
     * check above already reports for the common (non-race) case. */
    relay_heat_sweep_claim_result_t heat_claim = relay_authority_heat_sweep_claim_begin();
    if (heat_claim != RELAY_HEAT_SWEEP_CLAIM_OK) {
        return (heat_claim == RELAY_HEAT_SWEEP_CLAIM_REFUSE_PROFILE) ? ZONE_SWEEP_REFUSE_PROFILE_RUNNING
                                                                      : ZONE_SWEEP_REFUSE_AUTOTUNE_RUNNING;
    }

    s_sweep.active = true;
    s_sweep.abort_requested = false;
    s_sweep.state = ZONE_SWEEP_RUNNING;
    s_sweep.zone_index = 0;
    s_sweep.zones_done = 0;
    s_sweep.zones_total = s_zones.cfg.thermo_count;
    if (s_sweep.zones_total > MAX31856_CHANNEL_COUNT) {
        s_sweep.zones_total = MAX31856_CHANNEL_COUNT;
    }
    s_sweep.reason[0] = '\0';
    s_sweep.ct_map_derived_mask = 0;
    s_sweep.ct_map_reason[0] = '\0';
    s_sweep.k_ct_derived_mask = 0;
    s_sweep.k_ct_reason[0] = '\0';

    BaseType_t created = xTaskCreate(zone_sweep_task, "zone_sweep", 4096, NULL, tskIDLE_PRIORITY + 2, &s_sweep.task);
    if (created != pdPASS) {
        relay_authority_heat_sweep_claim_end(); /* task never started -- give the claim back */
        s_sweep.active = false;
        s_sweep.state = ZONE_SWEEP_FAILED;
        snprintf((char *)s_sweep.reason, sizeof(s_sweep.reason), "failed to start sweep task");
        return ZONE_SWEEP_REFUSE_NO_HW;
    }
    return ZONE_SWEEP_REFUSE_OK;
}

/* B2: see zones_http.h's doc comment above the declaration. */
bool zones_current_sweep_is_active(void)
{
    return s_sweep.active;
}

void zones_current_sweep_abort(void)
{
    if (s_sweep.active) {
        s_sweep.abort_requested = true;
    }
}

void zones_current_sweep_get_status(zone_sweep_status_t *out)
{
    if (!out) {
        return;
    }
    out->state = s_sweep.state;
    out->zone_index = s_sweep.zone_index;
    out->zones_done = s_sweep.zones_done;
    out->zones_total = s_sweep.zones_total;
    strncpy(out->reason, (const char *)s_sweep.reason, sizeof(out->reason) - 1);
    out->reason[sizeof(out->reason) - 1] = '\0';
    out->ct_map_derived_mask = s_sweep.ct_map_derived_mask;
    strncpy(out->ct_map_reason, (const char *)s_sweep.ct_map_reason, sizeof(out->ct_map_reason) - 1);
    out->ct_map_reason[sizeof(out->ct_map_reason) - 1] = '\0';
    out->k_ct_derived_mask = s_sweep.k_ct_derived_mask;
    strncpy(out->k_ct_reason, (const char *)s_sweep.k_ct_reason, sizeof(out->k_ct_reason) - 1);
    out->k_ct_reason[sizeof(out->k_ct_reason) - 1] = '\0';
}

/* ---- Task 2: runtime CT-to-zone mapping check ----------------------------- */

/* Ratio band a live reading must fall within to be considered a plausible
 * match for the measured normal, plus an absolute floor so a tiny normal
 * (a lightly-loaded zone) doesn't turn ordinary measurement noise into a
 * false warning purely from ratio math. Deliberately wide -- this is a
 * WRONG-JACK detector (a swapped CT reads close to 0A, or reads some OTHER
 * zone's current instead), not a precision check; SaftyFW/docs/
 * CURRENT_SENSE.md §0 already scopes current accuracy as "within a factor
 * of ~2" for load-active detection, and this reuses that same order-of-
 * magnitude tolerance rather than inventing a tighter one nothing in the
 * hardware chain can actually promise. */
#define ZONE_CT_MISMATCH_RATIO_LOW 0.4f
#define ZONE_CT_MISMATCH_RATIO_HIGH 2.5f
#define ZONE_CT_MISMATCH_MIN_DELTA_A 0.3f

bool zones_ct_mapping_mismatch(float normal_current_a, bool normal_measured, float live_current_a)
{
    if (!normal_measured) {
        return false; /* silent -- see this function's header comment */
    }
    if (isnan(normal_current_a) || normal_current_a <= 0.0f || isnan(live_current_a) || live_current_a < 0.0f) {
        return false;
    }
    float lo = normal_current_a * ZONE_CT_MISMATCH_RATIO_LOW;
    float hi = normal_current_a * ZONE_CT_MISMATCH_RATIO_HIGH;
    if (live_current_a >= lo && live_current_a <= hi) {
        return false;
    }
    return fabsf(live_current_a - normal_current_a) >= ZONE_CT_MISMATCH_MIN_DELTA_A;
}

uint8_t zones_ct_mapping_warn_mask(void)
{
    if (!s_hw_io || !s_hw_safety) {
        return 0;
    }
    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    if (safety_link_get_status(s_hw_safety, &st) != ESP_OK || !st.link_up) {
        return 0;
    }
    uint8_t relay_now = kiln_io_get_relay_shadow(s_hw_io);
    uint8_t warn = 0;
    for (uint8_t zi = 0; zi < s_zones.cfg.thermo_count && zi < MAX31856_CHANNEL_COUNT; zi++) {
        uint8_t relay_mask = s_zones.cfg.zones[zi].relay_mask;
        if (relay_mask == 0 || (relay_now & relay_mask) == 0) {
            continue; /* zone not commanded on right now -- nothing to compare */
        }
        float normal_a = 0.0f;
        bool measured = false;
        zones_config_get_normal_current(zi, &normal_a, &measured);
        uint8_t ctmask = 0;
        zones_config_get_ct_mask(zi, &ctmask);
        float live_a = 0.0f;
        for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
            if (ctmask & (1u << c)) {
                live_a += st.current_a[c];
            }
        }
        if (zones_ct_mapping_mismatch(normal_a, measured, live_a)) {
            warn |= (uint8_t)(1u << zi);
        }
    }
    return warn;
}

/* ---- Task 3: read-only safety-processor wiring display --------------------- */

void zones_get_safety_wiring(zone_safety_wiring_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_hw_safety) {
        return;
    }
    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    if (safety_link_get_status(s_hw_safety, &st) != ESP_OK || !st.link_up) {
        return; /* leave the zeroed/false "UNSET" defaults */
    }
    out->link_up = true;
    out->tc_temp_valid = !isnan(st.tc_temp_c);
    out->tc_temp_c = st.tc_temp_c;
    out->tc_fault = st.tc_fault;
    out->relay_energized = (st.flags & SAFETY_FLAG_RELAY) != 0;
}

/* ---- HTTP: the current-sweep endpoints ------------------------------------ */

static esp_err_t sweep_start_post_handler(httpd_req_t *req)
{
    zone_sweep_refusal_t r = zones_current_sweep_start();
    char json[160];
    int n = snprintf(json, sizeof(json), "{\"ok\":%s,\"reason\":\"%s\"}", r == ZONE_SWEEP_REFUSE_OK ? "true" : "false",
                     zone_sweep_refusal_str(r));
    httpd_resp_set_type(req, "application/json");
    if (r != ZONE_SWEEP_REFUSE_OK) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    return httpd_resp_send(req, json, n > 0 ? (size_t)n : 0);
}

static esp_err_t sweep_abort_post_handler(httpd_req_t *req)
{
    zones_current_sweep_abort();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const char *zone_sweep_state_str(zone_sweep_state_t s)
{
    switch (s) {
    case ZONE_SWEEP_IDLE: return "idle";
    case ZONE_SWEEP_RUNNING: return "running";
    case ZONE_SWEEP_DONE: return "done";
    case ZONE_SWEEP_ABORTED: return "aborted";
    case ZONE_SWEEP_FAILED: return "failed";
    default: return "unknown";
    }
}

static esp_err_t sweep_status_get_handler(httpd_req_t *req)
{
    zone_sweep_status_t st;
    zones_current_sweep_get_status(&st);
    char reason_escaped[sizeof(st.reason) * 2 + 1];
    json_escape(st.reason, reason_escaped, sizeof(reason_escaped));
    char ct_reason_escaped[sizeof(st.ct_map_reason) * 2 + 1];
    json_escape(st.ct_map_reason, ct_reason_escaped, sizeof(ct_reason_escaped));
    /* M12b: the CT-scale derivation reports separately -- see
     * zone_sweep_status_t's own comment for why the two share no field. */
    char k_reason_escaped[sizeof(st.k_ct_reason) * 2 + 1];
    json_escape(st.k_ct_reason, k_reason_escaped, sizeof(k_reason_escaped));
    char json[1024];
    int n = snprintf(json, sizeof(json),
                     "{\"state\":\"%s\",\"zone_index\":%u,\"zones_done\":%u,\"zones_total\":%u,"
                     "\"reason\":\"%s\",\"ct_map_derived_mask\":%u,\"ct_map_reason\":\"%s\","
                     "\"k_ct_derived_mask\":%u,\"k_ct_reason\":\"%s\"}",
                     zone_sweep_state_str(st.state), st.zone_index, st.zones_done, st.zones_total,
                     reason_escaped, st.ct_map_derived_mask, ct_reason_escaped,
                     st.k_ct_derived_mask, k_reason_escaped);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n > 0 ? (size_t)n : 0);
}

/* M12: the persisted derivation record, for the commissioning page. Kept a
 * separate tiny endpoint rather than folded into /api/safety/commissioning:
 * that response is built by safety_cfg_http.c's PURE build_commissioning_
 * json(), whose whole point (and its host test's) is that it touches no NVS
 * and no other module -- reaching into zones NVS from there would cost that
 * property for one extra field the page can just as easily fetch itself. */
static esp_err_t ct_channel_map_get_handler(httpd_req_t *req)
{
    uint8_t mask = 0;
    uint8_t zone_for_ch[ZONE_CT_CHANNEL_COUNT];
    zones_ct_channel_map_derived(&mask, zone_for_ch);
    /* M12b: the derived CT scale rides the SAME endpoint rather than getting
     * one of its own -- it is the same question ("what did the sweep derive,
     * and is this field still a manual-entry field?") asked about a second
     * field, produced by the same run, read by the same page at the same
     * moment. A second endpoint would only add a second fetch that can fail
     * independently of the first. */
    uint8_t k_mask = 0;
    float k_v_per_a[ZONE_CT_CHANNEL_COUNT];
    zones_ct_k_v_per_a_derived(&k_mask, k_v_per_a);

    char json[256];
    int o = snprintf(json, sizeof(json), "{\"mask\":%u,\"zone\":[", (unsigned)mask);
    for (unsigned c = 0; c < ZONE_CT_CHANNEL_COUNT && o > 0 && (size_t)o < sizeof(json); c++) {
        o += snprintf(json + o, sizeof(json) - (size_t)o, "%s%u", c == 0 ? "" : ",",
                      (unsigned)zone_for_ch[c]);
    }
    if (o > 0 && (size_t)o < sizeof(json)) {
        o += snprintf(json + o, sizeof(json) - (size_t)o, "],\"k_mask\":%u,\"k\":[",
                      (unsigned)k_mask);
    }
    for (unsigned c = 0; c < ZONE_CT_CHANNEL_COUNT && o > 0 && (size_t)o < sizeof(json); c++) {
        /* %.6g, never %f: these are ~0.03 V/A values, and a fixed-point
         * format would report a real calibration as 0.000000. A channel
         * whose k_mask bit is clear prints 0 and means nothing -- the page
         * must branch on k_mask, exactly as it already does on mask. */
        o += snprintf(json + o, sizeof(json) - (size_t)o, "%s%.6g", c == 0 ? "" : ",",
                      isfinite(k_v_per_a[c]) ? (double)k_v_per_a[c] : 0.0);
    }
    if (o > 0 && (size_t)o < sizeof(json)) {
        o += snprintf(json + o, sizeof(json) - (size_t)o, "]}");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, (o > 0 && (size_t)o < sizeof(json)) ? (size_t)o : 0);
}

esp_err_t zones_http_start(void)
{
    /* kiln_nvs is shared by zones/rules/relay_cycles/run_state, and each
     * module brings it up independently rather than assuming another module
     * already has -- nvs_flash_init_partition() on an already-initialized
     * partition is a harmless no-op (ESP_OK), so this is safe to repeat. */
    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init for '%s' failed: %s -- zones will not persist",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
    }

    /* Independent of the zones blob's own found/valid/version handling below
     * -- see this file's relay-names section header comment for why this is
     * a completely separate NVS key with its own tiny load path. Loaded even
     * if the zones partition init above failed: relay_names_load() opens its
     * own handle and treats "namespace not there" as ordinary first-boot
     * (blank names), not a reason to skip trying. */
    relay_names_load();

    /* Task 1's measured-normal-current blob -- same independent-key
     * reasoning and load convention as relay_names_load() just above. */
    zone_normals_load();

    esp_err_t err = ESP_OK;
    if (part_err == ESP_OK) {
        bool valid = false;
        bool found = false;
        err = nvs_load(&found, &valid);
        s_zones_config_valid = (err == ESP_OK) && valid;
        /* FIX 1 (bug found in review): this used to be
         * (err == ESP_OK && s_zones.cfg.version != 0), which cannot
         * distinguish "kiln_nvs has never had anything saved" from "kiln_nvs
         * HAS a blob, but nvs_load_from() refused it as newer-than-firmware
         * and zeroed s_zones.cfg" -- both read version == 0 through that
         * proxy. The real found flag (threaded through nvs_load() above)
         * tells them apart directly: a refused blob is found (the key
         * existed) but not valid, so found_in_kiln_nvs is now true for it and
         * the migration below is correctly skipped. Without this, a
         * firmware-rollback-refused newer blob fell through to
         * migrate_from_default_partition(), which then overwrote it with
         * whatever stale copy the old default-partition namespace still
         * holds -- destroying the very data nvs_load_from() had just gone out
         * of its way to leave untouched. */
        bool found_in_kiln_nvs = (err == ESP_OK && found);
        if (!found_in_kiln_nvs) {
            /* Nothing usable in kiln_nvs yet -- see if the old default
             * partition has a pre-split copy worth carrying forward.
             * migrate_from_default_partition() sets s_zones_config_valid
             * itself if it finds and applies something. */
            migrate_from_default_partition();
        }
    } else {
        s_zones_config_valid = false; /* partition itself didn't come up */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "zones_cfg NVS load failed: %s -- starting unconfigured", esp_err_to_name(err));
        memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
        s_zones_config_valid = false;
    }
    if (!s_zones_config_valid) {
        ESP_LOGW(TAG, "zones config did NOT load cleanly -- zone commanding is refused until a valid "
                      "config is loaded or saved (TODO.md 8.2 'Tie it to the guards')");
    }
    /* A load replaces the whole config, not one field, so it counts as a
     * change even on the very first boot -- both branches above land here.
     * In the normal start-up order nothing is running yet and nobody has
     * cached a generation, so this bump is a no-op in practice; it exists so
     * the counter's contract ("advances whenever the stored config was
     * replaced") holds unconditionally rather than only for the paths a
     * consumer happens to be watching today. */
    s_config_generation++;

    /* 2026-08-21, TODO.md owner-report item 1 -- the actual fix for the
     * gap: apply the persisted per-channel thermocouple type to the real
     * hardware now, at bring-up, instead of leaving MAX31856.c's hardcoded
     * THERMO_TC_K in the register forever. This runs after
     * thermo_owner_start() unconditionally (main.c calls it well before
     * zones_http_start() -- see thermo_owner.h's doc comment on ordering),
     * so thermo_owner_command_config_channel() is reachable here whether or
     * not any channel actually came up; a channel that never came up simply
     * answers ESP_ERR_NOT_FOUND, exactly as it does for every other
     * thermo_owner producer.
     *
     * Gated on s_zones_config_valid, not merely "a blob was read": a zeroed
     * s_zones.cfg (first boot, corrupt NVS, refused newer-than-firmware
     * blob) has every zones[i].tc_type reading 0 (THERMO_TC_B), which is
     * NOT this kiln's THERMO_TC_K default -- applying it here would
     * relinearize a perfectly fine, never-configured board's temperature
     * readings against the wrong thermocouple type the moment this firmware
     * boots. Leaving the hardcoded default MAX31856_start_all() already
     * wrote in place (Type K) is exactly the "preserve today's behaviour
     * for anyone who never touches this setting" outcome the migration
     * above exists to guarantee for a config that WAS successfully loaded;
     * this is the mirror-image guarantee for a config that was not. */
    if (s_zones_config_valid) {
        MAX31856Config default_cfg;
        MAX31856_config_default(&default_cfg); /* avg_mode/filter_50hz/auto_convert only --
                                                 * tc_type below is overridden per channel. */
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            uint8_t tc_type = s_zones.cfg.zones[i].tc_type;
            if (tc_type > ZONE_TC_TYPE_MAX_REAL) {
                /* Defensive only: parse_zone_fields()/the migration above both
                 * bound this to 0-7 before it can ever reach flash, so this
                 * should be unreachable outside a corrupted blob that still
                 * happened to pass the version/size checks. Fail safe by
                 * leaving this channel at whatever MAX31856_start_all()
                 * already configured (Type K) rather than writing a
                 * voltage-input mode into a thermocouple channel's register. */
                ESP_LOGE(TAG, "ch%u: stored tc_type=%u is not a real thermocouple type -- leaving "
                              "this channel's hardware config unchanged",
                         i, (unsigned)tc_type);
                continue;
            }
            esp_err_t tc_err = thermo_owner_command_config_channel(
                i, tc_type, default_cfg.avg_mode, default_cfg.filter_50hz, default_cfg.auto_convert);
            if (tc_err != ESP_OK) {
                /* ESP_ERR_NOT_FOUND is the expected, unremarkable case for a
                 * channel that isn't physically populated (this bench has 3
                 * of MAX31856_CHANNEL_COUNT fitted) -- logged at the same
                 * WARN level as MAX31856_start_all()'s own per-channel
                 * bring-up failures, not ERROR, for the same reason. */
                ESP_LOGW(TAG, "ch%u: could not apply persisted tc_type=%u at boot: %s", i,
                         (unsigned)tc_type, esp_err_to_name(tc_err));
            } else {
                ESP_LOGI(TAG, "ch%u: applied persisted thermocouple type %u from NVS", i,
                         (unsigned)tc_type);
            }
        }
    }

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/settings/zones", .method = HTTP_GET, .handler = page_get_handler,
    };
    static const httpd_uri_t safety_page_uri = {
        .uri = "/settings/safety", .method = HTTP_GET, .handler = safety_config_page_get_handler,
    };
    static const httpd_uri_t get_uri = {
        .uri = "/api/zones", .method = HTTP_GET, .handler = zones_get_handler,
    };
    static const httpd_uri_t post_uri = {
        .uri = "/api/zones", .method = HTTP_POST, .handler = zones_post_handler,
    };
    static const httpd_uri_t sweep_start_uri = {
        .uri = "/api/zones/current_sweep/start", .method = HTTP_POST, .handler = sweep_start_post_handler,
    };
    static const httpd_uri_t sweep_abort_uri = {
        .uri = "/api/zones/current_sweep/abort", .method = HTTP_POST, .handler = sweep_abort_post_handler,
    };
    static const httpd_uri_t sweep_status_uri = {
        .uri = "/api/zones/current_sweep/status", .method = HTTP_GET, .handler = sweep_status_get_handler,
    };
    static const httpd_uri_t ct_map_uri = {
        .uri = "/api/zones/ct_channel_map", .method = HTTP_GET, .handler = ct_channel_map_get_handler,
    };
    err = httpd_register_uri_handler(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/settings/zones) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &safety_page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/settings/safety) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/zones) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/zones) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &sweep_start_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST current_sweep/start) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &sweep_abort_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST current_sweep/abort) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &sweep_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET current_sweep/status) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &ct_map_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET ct_channel_map) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "zones API up (thermo_count=%u, relay_count=%u)", s_zones.cfg.thermo_count,
             s_zones.cfg.relay_count);
    return ESP_OK;
}
