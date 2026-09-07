#include "zones_http.h"
#include "zones_http_internal.h"
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

const char *ZONES_HTTP_TAG = "zones_http";

/* NVS_NAMESPACE/NVS_KEY_ZONES/KILN_NVS_PARTITION moved to
 * zones_http_internal.h -- zones_config_store.c (which now owns
 * nvs_partition_init()/nvs_load()/nvs_save()) needs them too, not just the
 * zones_http_start() call site below that still names KILN_NVS_PARTITION. */

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
 * task brief).
 *
 * 11 -> 12 (2026-08-31, DATA PLUMBING pass): autotune_engine.c's finalize_fit()
 * already fits a full FOPDT model (K, tau, L) for every off-diagonal
 * coupling cell, not just the gain -- only the gain ever reached storage;
 * tau/L died with the RAM-only s_at.coupling the moment the run ended. This
 * bump adds coupling_tau_s[]/coupling_dead_time_s[], row-per-zone, SAME
 * orientation as coupling_coeff[] (zones[affected].coupling_tau_s[stepped]).
 * See zone_cfg_t::coupling_tau_s's own doc comment for the full unit/
 * orientation story. Pure storage -- no consumer of either array exists yet.
 *
 * MIGRATION IS TRIVIAL: no version before v12 ever stored either array, so
 * every migrated zone's two new rows are left at the memset-zero
 * convert_zone_v11() (below) starts each destination zone at -- already each
 * cell's documented "not measured" meaning, identical to how coupling_coeff
 * itself was born zeroed on the v10->v11 bump for every zone the single old
 * pair didn't already cover.
 *
 * zone_cfg_t grows by 2*MAX31856_CHANNEL_COUNT floats per zone (3 channels:
 * 24 bytes/zone, 72 bytes total) -- see the _Static_assert byte math on
 * zone_cfg_v11_t and the live zone_cfg_t below, and the _Static_assert on
 * zones_cfg_t/ZONES_CONFIG_BLOB_MAX_SIZE just below this file's struct
 * definitions, which fails the build rather than silently overflowing if
 * this ever does not fit. */

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

/* Tuning-method recommendation evidence artifact (OWNER REQUEST 2026-09-02
 * part 2/2). Embedded PLAIN, not gzipped -- see App/drivers/CMakeLists.txt's
 * KILNCTL_TUNING_REC_SRC comment: it is a few hundred bytes to a few KB, and
 * a JSON-only endpoint this small does not carry its own weight as a second
 * gzip content-negotiation path. Staged at configure time from
 * tools/PcTools/config_presets/tuning_recommendations.json when the
 * simulation campaign has produced one, or from this component's own
 * tuning_recommendations_fallback.json (schema_version 1, empty
 * recommendations list) when it has not -- either way this symbol always
 * exists and is always valid JSON zones_page.html's tuningRecArtifactUsable()
 * can parse. */
extern const uint8_t tuning_recommendations_json_start[] asm("_binary_tuning_recommendations_json_start");
extern const uint8_t tuning_recommendations_json_end[] asm("_binary_tuning_recommendations_json_end");

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

/* ZONES_BODY_MAX moved to zones_http_internal.h (unchanged) --
 * zones_http_handlers.c's zones_post_handler() is the only remaining user,
 * but the macro itself lives in the internal header now, alongside the
 * other constants that moved out of this file's original top-of-file
 * section during the 2026-09-01 split. */

zones_state_t s_zones;

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
bool s_zones_config_valid = false;

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
uint32_t s_config_generation = 1;

/* ---- Hardware access for Tasks 1/2/3 (2026-08-27+2) ----------------------
 * zones_http_start() itself takes no hardware pointers (pure config CRUD --
 * see its own comment); the current sweep, the CT-mapping check, and the
 * safety-wiring readout all need real hardware, so main.c hands it over
 * separately, once, via zones_http_set_hw() below. NULL-tolerant, same
 * convention as every other *_start()'s io/thermo_bus/safety triple in this
 * codebase -- see zones_http.h's doc comment on this function. */
kiln_io_t *s_hw_io = NULL;
MAX31856BusClass *s_hw_thermo_bus = NULL;
SafetyLinkClass *s_hw_safety = NULL;

void zones_http_set_hw(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                       SafetyLinkClass *safety_or_null)
{
    s_hw_io = io_or_null;
    s_hw_thermo_bus = thermo_bus_or_null;
    s_hw_safety = safety_or_null;
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
    zones_json_escape(st.reason, reason_escaped, sizeof(reason_escaped));
    char ct_reason_escaped[sizeof(st.ct_map_reason) * 2 + 1];
    zones_json_escape(st.ct_map_reason, ct_reason_escaped, sizeof(ct_reason_escaped));
    /* M12b: the CT-scale derivation reports separately -- see
     * zone_sweep_status_t's own comment for why the two share no field. */
    char k_reason_escaped[sizeof(st.k_ct_reason) * 2 + 1];
    zones_json_escape(st.k_ct_reason, k_reason_escaped, sizeof(k_reason_escaped));
    char json[1024];
    int n = snprintf(json, sizeof(json),
                     "{\"state\":\"%s\",\"zone_index\":%u,\"zones_done\":%u,\"zones_total\":%u,"
                     "\"reason\":\"%s\",\"ct_map_derived_mask\":%u,\"ct_map_reason\":\"%s\","
                     "\"k_ct_derived_mask\":%u,\"k_ct_reason\":\"%s\","
                     "\"summed_unmeasured_mask\":%u}",
                     zone_sweep_state_str(st.state), st.zone_index, st.zones_done, st.zones_total,
                     reason_escaped, st.ct_map_derived_mask, ct_reason_escaped,
                     st.k_ct_derived_mask, k_reason_escaped, st.summed_unmeasured_mask);
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

/* GET /api/tuning_recommendations -- serves the embedded evidence artifact
 * (real or fallback, see the extern symbols' comment above) as-is. Not
 * gzip-negotiated (see that comment); web_set_asset_cache_headers() is
 * deliberately NOT applied here, unlike the *_page.html handlers -- this
 * body changes across firmware builds as the campaign's artifact is
 * updated, and a stale cached copy would silently show old recommendations
 * after a flash that carries new evidence. */
/* REVIEW 2026-09-02 (Opus round 4): -1 for the NUL. EMBED_TXTFILES appends a
 * null byte and places the _end symbol AFTER it (see the generated
 * build/tuning_recommendations.json.S: the byte run ends
 * "... 0x7d, 0x0d, 0x0a, 0x00" and the file's own
 * tuning_recommendations_json_length symbol is 4152 while _end - _start is
 * 4153). The sibling *_page_html_gz handlers use the bare difference and get
 * away with it because a gzip decoder stops at the end of the deflate
 * stream and ignores the trailing byte -- but JSON.parse() does not: a body
 * with a trailing NUL throws SyntaxError, zones_page.html's
 * fetch().then(r => r.json()) catch sets tuningRecArtifact = null, and the
 * panel reports "no tuning recommendation data on this board yet" for every
 * request, even with the real campaign artifact embedded. Send the file's
 * own length, not the file plus the terminator IDF added for C-string use.
 *
 * Pulled out of tuning_rec_get_handler() as its own pure function (2026-09-02,
 * host-link fix for commit 333dd4e) so the host test suite can prove this
 * arithmetic directly: tuning_recommendations_json_start/_end are ESP-IDF
 * EMBED_FILES symbols with no host-toolchain equivalent, so
 * test_zones_http.c can only supply 1-byte placeholder stand-ins for THOSE
 * two names (same convention as every other *_page_html_gz pair in this
 * file) -- not a real adjacent start/end pair whose difference means
 * anything. This helper takes start/end as plain arguments instead, so the
 * test can hand it a real, deliberately-adjacent synthetic buffer and
 * verify the "-1 for the trailing NUL, floor at 0" logic itself, independent
 * of how the real symbols are laid out in flash. */
static size_t tuning_rec_body_len(const uint8_t *start, const uint8_t *end)
{
    const size_t raw = (size_t)(end - start);
    return raw > 0 ? raw - 1 : 0;
}

static esp_err_t tuning_rec_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    const size_t tuning_rec_len =
        tuning_rec_body_len(tuning_recommendations_json_start, tuning_recommendations_json_end);
    return httpd_resp_send(req, (const char *)tuning_recommendations_json_start, tuning_rec_len);
}

esp_err_t zones_http_start(void)
{
    /* kiln_nvs is shared by zones/rules/relay_cycles/run_state, and each
     * module brings it up independently rather than assuming another module
     * already has -- nvs_flash_init_partition() on an already-initialized
     * partition is a harmless no-op (ESP_OK), so this is safe to repeat. */
    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "NVS init for '%s' failed: %s -- zones will not persist",
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
        ESP_LOGW(ZONES_HTTP_TAG, "zones_cfg NVS load failed: %s -- starting unconfigured", esp_err_to_name(err));
        memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
        s_zones_config_valid = false;
    }
    if (!s_zones_config_valid) {
        ESP_LOGW(ZONES_HTTP_TAG, "zones config did NOT load cleanly -- zone commanding is refused until a valid "
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
                ESP_LOGE(ZONES_HTTP_TAG, "ch%u: stored tc_type=%u is not a real thermocouple type -- leaving "
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
                ESP_LOGW(ZONES_HTTP_TAG, "ch%u: could not apply persisted tc_type=%u at boot: %s", i,
                         (unsigned)tc_type, esp_err_to_name(tc_err));
            } else {
                ESP_LOGI(ZONES_HTTP_TAG, "ch%u: applied persisted thermocouple type %u from NVS", i,
                         (unsigned)tc_type);
            }
        }
    }

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(ZONES_HTTP_TAG, "no HTTP server -- wifi_provision_http_start() must run first");
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
    static const httpd_uri_t pid_post_uri = {
        .uri = "/api/zones/pid", .method = HTTP_POST, .handler = zones_pid_post_handler,
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
    static const httpd_uri_t tuning_rec_uri = {
        .uri = "/api/tuning_recommendations", .method = HTTP_GET, .handler = tuning_rec_get_handler,
    };
    err = httpd_register_uri_handler(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "httpd_register_uri_handler(/settings/zones) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &safety_page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "httpd_register_uri_handler(/settings/safety) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "httpd_register_uri_handler(GET /api/zones) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "httpd_register_uri_handler(POST /api/zones) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &pid_post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "httpd_register_uri_handler(POST /api/zones/pid) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &sweep_start_uri);
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "httpd_register_uri_handler(POST current_sweep/start) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &sweep_abort_uri);
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "httpd_register_uri_handler(POST current_sweep/abort) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &sweep_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "httpd_register_uri_handler(GET current_sweep/status) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &ct_map_uri);
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "httpd_register_uri_handler(GET ct_channel_map) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &tuning_rec_uri);
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "httpd_register_uri_handler(GET tuning_recommendations) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(ZONES_HTTP_TAG, "zones API up (thermo_count=%u, relay_count=%u)", s_zones.cfg.thermo_count,
             s_zones.cfg.relay_count);
    return ESP_OK;
}
