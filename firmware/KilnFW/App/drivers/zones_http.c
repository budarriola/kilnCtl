#include "zones_http.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crc.h"
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
#include "relay_authority.h" /* the single shared heat claim -- see its doc comment
                               * above relay_heat_zone_claimant_t. Closes the race
                               * this file's own s_sweep.active check alone cannot:
                               * see zones_current_sweep_start()'s atomic gate. */
#include "thermo_combine.h"
#include "thermo_owner.h"
#include "uart_task_ids.h" /* SAFETY_FLAG_* for zones_get_safety_wiring() */
#include "web_encoding.h"
#include "wifi_provision_http.h"

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
 * version -- not guessed. validate_zones_cfg() (previously only run on the
 * snapshot-restore path, zones_config_import_blob()) now runs on every
 * decoded blob, old or current, via the shared decode_zones_blob() helper.
 * A CRC32 (esp_crc32_le(), already used by crash_report.c -- see that file's
 * compute_crc()/seal_crc() for the identical "compute over a zeroed-crc-field
 * copy" convention this follows) covers the current-version blob only: older
 * versions never had a crc32 field to check, so their integrity gate is the
 * length-must-match-the-claimed-version check plus validate_zones_cfg().
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
 * before, same as the 7->8 bump's own upgrade guarantee. */
#define ZONES_CFG_VERSION 9

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
#define ZONE_TC_TYPE_MAX_REAL THERMO_TC_T

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
 * ZONES_CONFIG_BLOB_MAX_SIZE (512, see zones_http.h) instead of past it --
 * confirmed by _Static_assert(sizeof(zones_cfg_t) <= ...) below, which does
 * not compile if this ever regresses. Purely a memory-layout optimization:
 * every field is still addressed by name everywhere in this file (parse_zone_
 * fields(), validate_zones_cfg(), every convert_zone_v*(), the JSON GET/POST
 * paths), so this reordering changes NOTHING about behavior, only the
 * in-memory (and on-flash) byte offsets -- which is exactly why it's safe to
 * do on the CURRENT struct even though the historical zone_cfg_v*_t snapshots
 * below deliberately keep their ORIGINAL field order (they must, byte-for-byte,
 * to correctly interpret an old blob). */
typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    float cal_offset_c; /* applied via zones_config_apply_cal() by dashboard_http.c and
                         * profile_executor.c, but not by uart_bridge.c -- see header */
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr; /* user-entered ceiling; 0 = never configured */
    float sanity_rate_c_per_min; /* direction/rate sanity threshold; 0 = never
                                   * configured -- see zones_config_get_sanity_rate() */
    float max_temp_c;        /* guard 5; 0 = not set, no ceiling */
    float min_temp_c;        /* guard 5; page defaults this to -20 */
    /* TODO.md 6A.9's "per-relay window_ms/min_on_ms/min_off_ms becoming
     * page-configurable" -- stored per zone rather than per physical relay,
     * since that's the granularity profile_executor.c/autotune_engine.c
     * actually apply a heater_output_cfg_t at (one zone's relay group
     * switches together as a unit; there's no per-individual-relay timing
     * concept anywhere in the control path). Float, not uint32_t, matching
     * every other field's parse_float_field()/JSON round-trip in this file
     * -- milliseconds up to 600000 round-trips exactly through a float's
     * 24-bit mantissa, so there's no precision cost to staying consistent
     * with the rest of the struct. 0 = "not configured," same convention as
     * sanity_rate_c_per_min -- the caller substitutes
     * PROFILE_EXECUTOR_DEFAULT_*_MS. */
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    /* TODO.md 6A.3's remaining named guard thresholds, promoted from
     * firmware-wide constants to per-zone override (2026-08-16) -- see
     * thermal_guard.h's doc comment for the full "0 substitutes a firmware
     * default, does NOT disable the guard" convention these all share.
     * OPTIONAL on POST, same reason z%u_xzone below is (see
     * parse_zone_fields()): an operator who never touches these keeps the
     * firmware defaults, and older clients (pc_tools/MCP, the test
     * harnesses) must not start getting 400s for fields they've never heard
     * of. */
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    /* Guard 8, cross-zone plausibility (TODO.md 6A.3): degC this zone's raw
     * reading may differ from its worst-disagreeing peer's before the guard
     * trips. 0 = "not configured", which DISABLES the guard rather than
     * substituting a default -- the opposite convention to
     * sanity_rate_c_per_min above, and deliberately so: a plausible number
     * here depends on how strongly this particular kiln's zones couple
     * (TODO.md 6A.5's cross-gain matrix), and a hand-picked default would
     * either nuisance-trip a kiln that genuinely stratifies or be so wide it
     * catches nothing. Left blank until the operator has a measurement. */
    float cross_zone_max_delta_c;
    /* The FOPDT plant model autotune (TODO.md 6A.4) fitted for this zone,
     * kept so 6A.2's feedforward term can be computed from it:
     * u_ff = (T_sp - T_ambient)/K + (dT_sp/dt)*tau/K. Until now the fit was
     * used once to derive Kp/Ki/Kd and then thrown away with the run, which
     * meant the feedforward had nothing to stand on after a reboot -- and a
     * step test costs the operator hours of real kiln heat, so re-measuring
     * it on every boot is not an option.
     *
     * Stored here rather than in autotune_engine.c for the same reason the
     * gains are: 6A.4 makes zones_http the single owner of persisted zone
     * config, and autotune must not touch NVS itself.
     *
     * 0 in ANY of the three means "no model identified for this zone" and
     * callers must treat it as "cannot answer" (see the getter's header
     * comment) -- all three, because a physically meaningful model needs a
     * non-zero gain AND a non-zero time constant, and dividing by either
     * zero in the feedforward expression is exactly the failure this
     * convention exists to prevent. A genuinely zero dead time is not
     * physically reachable on a kiln (tens of seconds is typical), so
     * nothing real is lost by folding it into the same rule. */
    float model_k_dc;        /* static gain, degC per unit duty at steady state */
    float model_tau_s;       /* first-order time constant, seconds */
    float model_dead_time_s; /* transport delay L, seconds */
    /* ---- Every remaining field is a uint8_t, deliberately grouped here at
     * the struct tail -- see this struct's own top-of-definition comment for
     * why (alignment padding, and the ZONES_CONFIG_BLOB_MAX_SIZE budget it
     * buys back). ---- */
    uint8_t relay_mask; /* bit N-1 = relay N belongs to this zone, N in 1..relay_count */
    uint8_t control_mode; /* zone_control_mode_t (TODO.md 6A.1) */
    /* 2026-08-21: the actual gap the owner reported -- "in the thermocouples
     * settings page i dont see anywhere i can select my thermocouple type."
     * MAX31856.c's tc_type field (CR1.TC[3:0]) was always THERMO_TC_K at
     * boot, hardcoded, never persisted, and never exposed here. This is
     * per-CHANNEL, not per-zone, even though it lives in the same
     * MAX31856_CHANNEL_COUNT-sized zones[] array indexed by i: slot i is
     * "channel i" for this field regardless of which zone(s) thermo_mask
     * says actually read that channel (a zone can combine several channels;
     * each physical channel still has exactly one real thermocouple wired to
     * it with exactly one type). Bounded to ZONE_TC_TYPE_MAX_REAL (0-7, the
     * eight real types) by both parse_zone_fields() and
     * migrate_zones_cfg_v1_to_current() -- the remaining CR1 nibble codes
     * are voltage-input modes, not thermocouples, and have no business being
     * reachable from this operator-facing page (see ZONE_TC_TYPE_MAX_REAL's
     * comment). Applied to hardware at boot by zones_http_start() below,
     * which is the fix for the other half of the bug: setting this over
     * UART today (thermo_owner_command_config_channel(), already wired) was
     * live only until the next reboot, because nothing read it back out of
     * NVS at bring-up. */
    uint8_t tc_type;
    /* TODO.md 10.8 (2026-08-17): which MAX31856 channels combine (mean of
     * valid readings, thermo_combine.c) into this zone's control
     * temperature -- bit N-1 = channel N, same convention as relay_mask
     * above. See zones_config_get_thermo_mask()'s doc comment (zones_http.h)
     * for the legacy-mapping default a 0 here falls back to when the field
     * was never explicitly supplied, which is what keeps this addition from
     * being the kind of silent-wipe field growth TODO.md 6A.1's
     * relay_cycles.c note (section 6A.1, 2026-08-12) warns against. */
    uint8_t thermo_mask;
    /* 2026-08-27: which of SaftyFW's ZONE_CT_CHANNEL_COUNT current-sense
     * channels feed this zone's live-current display -- bit N-1 = CT channel
     * N, same convention as relay_mask/thermo_mask above. See
     * ZONES_CFG_VERSION's 5->6 comment: purely a display mapping, no
     * exclusivity, may legitimately overlap another zone's ct_mask (one
     * physical CT probe clamped around a shared supply line feeding more
     * than one zone's element). 0 = no probe mapped to this zone yet. */
    uint8_t ct_mask;
    /* 2026-08-27 (ZONES_CFG_VERSION 8->9), owner-report follow-up ("make it so
     * that i can have diffrent Safety timings and assign the zones to them ...
     * that way i dont need to copy the data multipal times"): replaces the
     * nine per-zone override floats v8 added directly on this struct
     * (guard_progress_duty_min .. ramp_lock_band_c -- see ZONES_CFG_VERSION's
     * 8->9 comment for the full migration story). Those nine numbers now live
     * once per zone_timing_profile_t, not once per zone; this index picks
     * WHICH profile in zones_cfg_t::timing_profiles[] this zone uses. Must be
     * < zones_cfg_t::timing_profile_count -- validate_zones_cfg() enforces
     * that, and the POST /api/zones handler bounds a submitted z%u_timingprofile
     * against however many profiles the same submission defines. Profile 0
     * always exists (timing_profile_count is never 0 in a valid config), so a
     * freshly zero-initialized zone_cfg_t already points at a real profile --
     * the same "0 is always a safe, meaningful value" property control_mode/
     * thermo_mask/etc. already have, not a dangling reference. */
    uint8_t timing_profile;
} zone_cfg_t;

/* A named, reusable bundle of the nine thermal-timing numbers that used to be
 * typed once per zone (see zone_cfg_t::timing_profile's comment and
 * ZONES_CFG_VERSION's 8->9 comment for the full rationale). Every field here
 * keeps the identical "0 = not configured, the consuming module substitutes
 * its own named firmware default; 0 never disables" convention and the exact
 * same bounds these nine had as zone_cfg_t fields -- only WHERE they are
 * stored changed, not their meaning or validation. */
typedef struct {
    char name[TIMING_PROFILE_NAME_MAX_LEN + 1]; /* operator-entered label, e.g. "Default", "Fast bisque" */
    float guard_progress_duty_min;    /* guard 1 arms above this commanded duty */
    float guard_progress_window_s;    /* guard 1's no-progress window while heating */
    float guard_drift_hysteresis_c;   /* guard 4's settle band / drift threshold */
    float guard_frozen_eps_c;         /* guard 7: reading moves by less than this = frozen */
    float guard_cross_zone_period_s;  /* guard 8's sustained-disagreement window */
    float bangbang_hysteresis_c;      /* BANGBANG mode's switching band */
    float cooling_limited_margin_c;   /* "cannot cool fast enough" detection margin */
    float cooling_limited_hold_s;     /* ...sustained for this long before it counts */
    float ramp_lock_band_c;           /* ramp-rate lock engages within this of setpoint */
} zone_timing_profile_t;

typedef struct {
    uint8_t version; /* ZONES_CFG_VERSION at save time -- see nvs_load_from() */
    uint8_t thermo_count;
    uint8_t relay_count;
    /* TODO.md 6A.5 load-staggering: 0 = unlimited (default, existing
     * behavior unchanged). Global, not per-zone -- a breaker/supply limit
     * applies to the whole board, not one zone. Enforced by
     * profile_executor.c, not here; this struct only stores it. */
    uint8_t max_simultaneous_relays;
    /* TODO.md 6A.3's "default policy on a single-zone trip: abort the whole
     * firing" -- 0 (the zero-initialized default, matching a migrated v1
     * blob that predates this field) is that default; 1 is the explicitly
     * opted-in "continue with the other healthy zones" alternative the
     * bullet says needs justifying, not the abort. Global, same reasoning as
     * max_simultaneous_relays: a multi-zone firing's abort policy is a
     * whole-board decision, not a per-zone one. Enforced by
     * profile_executor.c's escalate_guard_trip(). */
    uint8_t continue_on_zone_trip;
    /* 2026-08-21, TODO.md owner-report task item 3: the RP2040 safety
     * processor has its OWN, physically independent MAX31856 thermocouple --
     * not one of the main board's MAX31856_CHANNEL_COUNT channels above, a
     * fourth (well, first) part on entirely separate hardware -- and its
     * type is set over the link by SAFETY_CMD_SET_CONFIG
     * (safety_link_send_set_config()). This is a SEPARATE setting from any
     * zone's tc_type above, deliberately: the safety processor's sensor is
     * wired to whatever thermocouple the operator physically attached to
     * ITS input, which has no reason to match any particular main-board
     * zone's channel (that is the whole point of it being an *independent*
     * cross-check -- see docs/HARDWARE.md and SaftyFW's own thermocouple
     * wiring). Defaults to THERMO_TC_K, matching what MAX31856.c has always
     * hardcoded for the main board's own channels, so a board that has never
     * touched this setting keeps behaving exactly as it does today. Mirrored
     * to the Pico by safety_link.c's poll task, not sent directly from this
     * file -- see that file's safety_sync_tc_type() for why (this module has
     * no reference to the SafetyLinkClass instance; main.c, which does, is
     * off-limits this pass) and for the "link was down when this changed"
     * re-apply-on-reconnect handling. */
    uint8_t safety_tc_type;
    zone_cfg_t zones[MAX31856_CHANNEL_COUNT];
    /* 2026-08-27 (ZONES_CFG_VERSION 8->9): how many of timing_profiles[]
     * below's MAX31856_CHANNEL_COUNT slots actually hold a profile. Sized to
     * the zone count, not some larger operator-facing limit, because the
     * worst case that must always fit is "every zone wants its own distinct
     * profile" -- never more than one profile per zone could ever be useful.
     * Never 0 in a config validate_zones_cfg() has accepted: every
     * zone_cfg_t::timing_profile must resolve to a real slot, including a
     * freshly zero-initialized zone at profile index 0, so at least one
     * profile (index 0) must always exist. */
    uint8_t timing_profile_count;
    /* The named timing-profile bundles zones point at via
     * zone_cfg_t::timing_profile -- see that field's comment and
     * ZONES_CFG_VERSION's 8->9 comment for the full rationale. Only indices
     * [0, timing_profile_count) are meaningful; slots past that are unused
     * and not emitted by GET /api/zones. */
    zone_timing_profile_t timing_profiles[MAX31856_CHANNEL_COUNT];
    /* 2026-08-27 (ZONES_CFG_VERSION 7->8): how long the PC link may go silent
     * before a running firing is aborted. Global rather than per-zone: the
     * link is one wire to one PC, and its loss is a whole-board condition, not
     * something one zone experiences and another does not. 0 = not configured,
     * substituting PROFILE_EXECUTOR_PC_LINK_ABORT_SILENCE_MS. Stored as float
     * for the same reason heater_window_ms is (see its comment) -- 30000ms
     * round-trips exactly, and every parse/emit helper in this file is
     * float-shaped. */
    float pc_link_abort_silence_ms;
    /* 2026-08-27 (ZONES_CFG_VERSION 6->7): CRC32 over this whole struct with
     * this field itself zeroed, stamped by nvs_save() (see compute_zones_crc())
     * and checked by decode_zones_blob() on every load of a CURRENT-version
     * blob. Appended at the true tail -- the one safe place to grow this
     * struct, unlike zone_cfg_t's own history of insertions mid-array-element
     * (see ZONES_CFG_VERSION's comment above). Older on-flash versions never
     * had this field; their integrity gate is the length-must-match-the-
     * claimed-version check plus validate_zones_cfg(), not a CRC. */
    uint32_t crc32;
} zones_cfg_t;

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
 * parse_float_field()'s 23-char value each, ~250 bytes a zone, ~750 across
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
static bool validate_zones_cfg(const zones_cfg_t *cand, const char **err_reason);

/* ---- Historical on-flash layouts (ZONES_CFG_VERSION 1..6) ----------------
 *
 * EXACT field-for-field layouts, recovered from this file's own git history
 * (not guessed): b9ecc52 (v1 introduced), c17e80f (1->2, continue_on_zone_trip),
 * 915f0fe (2->3, the 8 named guard thresholds -- zone_cfg_t itself grew here,
 * confirmed by that commit's own message "promote the remaining thermal_guard
 * thresholds to per-zone config"), 0b1674e (3->4, thermo_mask appended at the
 * zone_cfg_t tail), 638ea87 (4->5, tc_type inserted into zone_cfg_t BEFORE
 * the model_* fields and BEFORE thermo_mask -- i.e. NOT at the tail, the
 * exact insertion that made the old memcpy-and-patch migration wrong), and
 * this file's own prior state for v6 (5->6, ct_mask appended at the
 * zone_cfg_t tail). v1 and v2 share an identical zone_cfg_t (only the
 * top-level continue_on_zone_trip field differs between them).
 *
 * Each struct is used ONLY to interpret a raw on-flash blob whose length has
 * already been checked (expected_len_for_version()) against the size of
 * EXACTLY this struct before a single byte is copied out of it -- see
 * decode_zones_blob(). None of these are ever grown, edited, or reused for a
 * different version; a future version needing a new historical layout gets
 * its own new struct here, appended, never a change to one of these. */

typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask;
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    uint8_t control_mode;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float cross_zone_max_delta_c;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
} zone_cfg_v1_t; /* v1 and v2 -- predates the 8 guard thresholds, thermo_mask, tc_type, ct_mask */

typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask;
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    uint8_t control_mode;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
} zone_cfg_v3_t; /* v3 -- adds the 8 guard thresholds; predates thermo_mask/tc_type/ct_mask */

typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask;
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    uint8_t control_mode;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    uint8_t thermo_mask; /* appended at the tail -- still safe growth at this point */
} zone_cfg_v4_t; /* v4 -- adds thermo_mask; predates tc_type/ct_mask */

typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask;
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    uint8_t control_mode;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    uint8_t tc_type;   /* inserted HERE, before model_k_dc and thermo_mask -- NOT
                        * at the tail. This is the exact insertion that broke
                        * the old "grows at the tail only" migration for every
                        * zone after the first. */
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    uint8_t thermo_mask;
} zone_cfg_v5_t; /* v5 -- adds tc_type (mid-struct); predates ct_mask */

/* v6/v7 shared the same zone layout, ending at ct_mask. v8 appends nine
 * per-zone fields after it, so that layout now needs its own snapshot -- this
 * is what zone_cfg_t looked like before this pass, field for field. */
typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask;
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    uint8_t control_mode;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    uint8_t tc_type;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    uint8_t thermo_mask;
    uint8_t ct_mask;
} zone_cfg_v7_t; /* v6/v7 -- predates the nine v8 per-zone override fields */

/* v8 appended the nine per-zone timing overrides directly to zone_cfg_v7_t's
 * layout; v9 (this pass) removes them again in favor of timing_profile, so
 * v8's zone layout now needs its own frozen snapshot too -- this is what
 * zone_cfg_t looked like for the one version that had these nine fields
 * living per-zone. NEVER edited, grown, or reused -- see this file's own
 * WARNING in ZONES_CFG_VERSION's 8->9 comment for why sizeof(this struct),
 * not sizeof(the current zone_cfg_t), is the only correct length for a v8
 * blob. */
typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask;
    float cal_offset_c;
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr;
    float sanity_rate_c_per_min;
    uint8_t control_mode;
    float max_temp_c;
    float min_temp_c;
    float heater_window_ms;
    float heater_min_on_ms;
    float heater_min_off_ms;
    float guard_wrong_dir_window_s;
    float guard_wrong_dir_rate_c_per_min;
    float guard_off_settle_s;
    float guard_runaway_rate_c_per_min;
    float guard_runaway_margin_c;
    float guard_drift_period_s;
    float guard_sensor_fault_debounce_ticks;
    float guard_frozen_window_s;
    float cross_zone_max_delta_c;
    uint8_t tc_type;
    float model_k_dc;
    float model_tau_s;
    float model_dead_time_s;
    uint8_t thermo_mask;
    uint8_t ct_mask;
    float guard_progress_duty_min;
    float guard_progress_window_s;
    float guard_drift_hysteresis_c;
    float guard_frozen_eps_c;
    float guard_cross_zone_period_s;
    float bangbang_hysteresis_c;
    float cooling_limited_margin_c;
    float cooling_limited_hold_s;
    float ramp_lock_band_c;
} zone_cfg_v8_t; /* v8 -- the nine timing overrides lived here, per zone; predates timing_profile */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    zone_cfg_v1_t zones[MAX31856_CHANNEL_COUNT];
} zones_cfg_v1_t; /* v1 -- predates continue_on_zone_trip/safety_tc_type */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    zone_cfg_v1_t zones[MAX31856_CHANNEL_COUNT];
} zones_cfg_v2_t; /* v2 */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    zone_cfg_v3_t zones[MAX31856_CHANNEL_COUNT];
} zones_cfg_v3_t; /* v3 */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    zone_cfg_v4_t zones[MAX31856_CHANNEL_COUNT];
} zones_cfg_v4_t; /* v4 */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v5_t zones[MAX31856_CHANNEL_COUNT];
} zones_cfg_v5_t; /* v5 -- adds safety_tc_type */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v7_t zones[MAX31856_CHANNEL_COUNT];
} zones_cfg_v6_t; /* v6 -- v7's zone layout, but no crc32 yet */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v7_t zones[MAX31856_CHANNEL_COUNT];
    uint32_t crc32;
} zones_cfg_v7_t; /* v7 -- adds crc32; predates the v8 override fields */

typedef struct {
    uint8_t version;
    uint8_t thermo_count;
    uint8_t relay_count;
    uint8_t max_simultaneous_relays;
    uint8_t continue_on_zone_trip;
    uint8_t safety_tc_type;
    zone_cfg_v8_t zones[MAX31856_CHANNEL_COUNT];
    float pc_link_abort_silence_ms;
    uint32_t crc32;
} zones_cfg_v8_t; /* v8 -- the nine timing overrides lived per-zone; predates timing_profiles[] */

/* Per-version expected blob length -- checked in decode_zones_blob() BEFORE
 * a single byte is copied out of a stored blob or interpreted as any field.
 * A stored blob whose length does not match the size EXACTLY implied by its
 * own claimed version is corrupt and is rejected outright, never partially
 * repaired. Returns 0 for a version this build has no known historical (or
 * current) layout for -- also a rejection, not a guess. */
static size_t expected_len_for_version(uint8_t version)
{
    switch (version) {
    case 1: return sizeof(zones_cfg_v1_t);
    case 2: return sizeof(zones_cfg_v2_t);
    case 3: return sizeof(zones_cfg_v3_t);
    case 4: return sizeof(zones_cfg_v4_t);
    case 5: return sizeof(zones_cfg_v5_t);
    case 6: return sizeof(zones_cfg_v6_t);
    case 7: return sizeof(zones_cfg_v7_t);
    /* NEVER sizeof(zones_cfg_t) here -- that is the CURRENT (v9) layout.
     * zones_cfg_v8_t is a separate, frozen snapshot of what v8 actually
     * looked like; see ZONES_CFG_VERSION's 8->9 comment's WARNING for the
     * sibling profiles_http.c bug that returning the current struct's size
     * for an old version caused (every profile on the owner's board rejected
     * and marked unused, found only by a hardware flash). */
    case 8: return sizeof(zones_cfg_v8_t);
    case ZONES_CFG_VERSION: return sizeof(zones_cfg_t);
    default: return 0;
    }
}

/* Field-by-field converters, one per historical zone_cfg_t layout, each
 * writing every field of a fresh CURRENT-format zone_cfg_t explicitly --
 * never a memcpy of one shape over another. Fields the source layout does
 * not have get the same safe fill-in migrate_zones_cfg_v1_to_current() used
 * to apply (see ZONES_CFG_VERSION's own comment for why each one is safe):
 * thermo_mask defaults to the legacy "zone i <-> channel i" mapping
 * (1u << chan_idx), tc_type/safety_tc_type default to THERMO_TC_K (every
 * board's actual hardware register was always hardcoded to K until 4->5), and
 * ct_mask/the 8 guard thresholds default to 0, which is already the correct
 * "not configured" meaning for every one of them. */
static void convert_zone_v1(const zone_cfg_v1_t *s, zone_cfg_t *d, uint8_t chan_idx)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    /* guard thresholds: predate this layout -- 0 is the documented
     * "substitute the firmware default" meaning, not a rejection. */
    d->thermo_mask = (uint8_t)(1u << chan_idx);
    d->tc_type = THERMO_TC_K;
}

static void convert_zone_v3(const zone_cfg_v3_t *s, zone_cfg_t *d, uint8_t chan_idx)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = (uint8_t)(1u << chan_idx);
    d->tc_type = THERMO_TC_K;
}

static void convert_zone_v4(const zone_cfg_v4_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask; /* real, operator-set value */
    d->tc_type = THERMO_TC_K;        /* predates this field */
}

static void convert_zone_v5(const zone_cfg_v5_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->tc_type = s->tc_type; /* real, operator-set value */
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask; /* real, operator-set value */
}

/* Dispatches to the right typed converter for `version`, filling `out` (a
 * fresh CURRENT-format zones_cfg_t) field by field -- never a memcpy of one
 * struct shape over another. Caller (decode_zones_blob()) has already
 * checked `len` against expected_len_for_version(version), so the memcpy of
 * `blob` into each local, exactly-sized historical struct below is safe. */
/* v6/v7 -> current. Every field the old layout had is copied by name; the nine
 * v8 additions get 0, which is already their "not configured, use the firmware
 * default" meaning, so a board upgrading from v7 behaves exactly as it did. */
static void convert_zone_v7(const zone_cfg_v7_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->tc_type = s->tc_type;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask;
    d->ct_mask = s->ct_mask;
}

/* v8 -> current. Copies every field EXCEPT the nine timing overrides, which
 * moved off zone_cfg_t entirely -- the caller (convert_versioned_blob_to_
 * current()'s case 8) is responsible for setting d->timing_profile after this
 * returns, once it has decided (via dedup against the profiles already
 * allocated for earlier zones) which profile this zone's nine v8 values map
 * to. Deliberately does NOT touch d->timing_profile itself, unlike every
 * other convert_zone_v*() which fully owns its output zone -- this one field
 * is a whole-blob decision, not a per-zone one. */
static void convert_zone_v8(const zone_cfg_v8_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->tc_type = s->tc_type;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask;
    d->ct_mask = s->ct_mask;
}

/* Versions 1-7 predate the nine timing-override fields entirely -- there is
 * nothing to migrate, so every zone gets pointed at one synthesized, all-zero
 * "Default" profile (0 in each of the nine fields is already that field's own
 * "use the firmware default" meaning -- see zone_timing_profile_t's comment),
 * matching exactly how those boards already behaved pre-upgrade. Relies on
 * convert_versioned_blob_to_current()'s own memset(out, 0, sizeof(*out)) at
 * entry to have already zeroed timing_profiles[0]'s nine floats and every
 * zone's timing_profile index (0) -- this only needs to name the profile. */
static void set_default_timing_profile(zones_cfg_t *out)
{
    out->timing_profile_count = 1;
    snprintf(out->timing_profiles[0].name, sizeof(out->timing_profiles[0].name), "Default");
}

static bool convert_versioned_blob_to_current(uint8_t version, const void *blob, zones_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (version) {
    case 1: {
        zones_cfg_v1_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = 0; /* predates this field -- 0 is its documented default */
        out->safety_tc_type = THERMO_TC_K;
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v1(&src.zones[i], &out->zones[i], i);
        }
        set_default_timing_profile(out);
        return true;
    }
    case 2: {
        zones_cfg_v2_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = THERMO_TC_K;
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v1(&src.zones[i], &out->zones[i], i);
        }
        set_default_timing_profile(out);
        return true;
    }
    case 3: {
        zones_cfg_v3_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = THERMO_TC_K;
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v3(&src.zones[i], &out->zones[i], i);
        }
        set_default_timing_profile(out);
        return true;
    }
    case 4: {
        zones_cfg_v4_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = THERMO_TC_K;
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v4(&src.zones[i], &out->zones[i]);
        }
        set_default_timing_profile(out);
        return true;
    }
    case 5: {
        zones_cfg_v5_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type; /* real value from v5 on */
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v5(&src.zones[i], &out->zones[i]);
        }
        set_default_timing_profile(out);
        return true;
    }
    case 6: {
        zones_cfg_v6_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = 0.0f; /* v6 has no such field -- 0 = firmware default */
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v7(&src.zones[i], &out->zones[i]);
        }
        set_default_timing_profile(out);
        return true;
    }
    case 7: {
        zones_cfg_v7_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = 0.0f; /* v7 has no such field -- 0 = firmware default */
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v7(&src.zones[i], &out->zones[i]);
        }
        set_default_timing_profile(out);
        /* src.crc32 is deliberately NOT carried over: it covered the v7 shape,
         * and nvs_save() stamps a fresh one over the current struct. */
        return true;
    }
    case 8: {
        zones_cfg_v8_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v8 value */
        /* THE actual v8->v9 migration (see ZONES_CFG_VERSION's 8->9 comment
         * for the full rationale): v8 kept the nine timing overrides PER
         * ZONE, so migrating them verbatim would mean giving every zone its
         * own private profile -- exactly the "still copied N times" state
         * the owner's request asks to eliminate. Instead, deduplicate: for
         * each zone, look for an already-allocated profile (from an earlier
         * zone in this same loop) whose nine values are ALL identical to
         * this zone's; point the zone at that profile if found, otherwise
         * allocate a fresh profile from this zone's own values. A board
         * where every zone is still at the v8 all-zero default -- the
         * owner's board today -- collapses to profile_count == 1. A board
         * with three genuinely distinct zones ends up with three profiles,
         * one per zone, which still fits: timing_profiles[] is sized
         * MAX31856_CHANNEL_COUNT, the same as zones[], so "one profile per
         * zone" is always the worst case, never an overflow. */
        uint8_t profile_count = 0;
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            const zone_cfg_v8_t *s = &src.zones[i];
            uint8_t match = profile_count; /* == "no match found yet" sentinel */
            for (uint8_t p = 0; p < profile_count; p++) {
                const zone_timing_profile_t *tp = &out->timing_profiles[p];
                if (tp->guard_progress_duty_min == s->guard_progress_duty_min &&
                    tp->guard_progress_window_s == s->guard_progress_window_s &&
                    tp->guard_drift_hysteresis_c == s->guard_drift_hysteresis_c &&
                    tp->guard_frozen_eps_c == s->guard_frozen_eps_c &&
                    tp->guard_cross_zone_period_s == s->guard_cross_zone_period_s &&
                    tp->bangbang_hysteresis_c == s->bangbang_hysteresis_c &&
                    tp->cooling_limited_margin_c == s->cooling_limited_margin_c &&
                    tp->cooling_limited_hold_s == s->cooling_limited_hold_s &&
                    tp->ramp_lock_band_c == s->ramp_lock_band_c) {
                    match = p;
                    break;
                }
            }
            if (match == profile_count) {
                /* No existing profile matches -- allocate a new one from this
                 * zone's own nine values. profile_count can never reach
                 * MAX31856_CHANNEL_COUNT before this loop's own index i does
                 * (at most one NEW profile is allocated per zone iterated),
                 * so this write is always in-bounds. */
                zone_timing_profile_t *tp = &out->timing_profiles[profile_count];
                tp->guard_progress_duty_min = s->guard_progress_duty_min;
                tp->guard_progress_window_s = s->guard_progress_window_s;
                tp->guard_drift_hysteresis_c = s->guard_drift_hysteresis_c;
                tp->guard_frozen_eps_c = s->guard_frozen_eps_c;
                tp->guard_cross_zone_period_s = s->guard_cross_zone_period_s;
                tp->bangbang_hysteresis_c = s->bangbang_hysteresis_c;
                tp->cooling_limited_margin_c = s->cooling_limited_margin_c;
                tp->cooling_limited_hold_s = s->cooling_limited_hold_s;
                tp->ramp_lock_band_c = s->ramp_lock_band_c;
                /* Named after the fact below, once it's known whether this
                 * is the lone shared "Default" (every zone matched) or one
                 * of several genuinely distinct migrated profiles. */
                profile_count++;
            }
            convert_zone_v8(s, &out->zones[i]);
            out->zones[i].timing_profile = match;
        }
        /* Name the migrated profiles so an operator recognizes them. The
         * degenerate, overwhelmingly common case -- every zone was at the
         * v8 all-zero default, i.e. nobody had touched the safety-timings
         * page yet, true of the owner's board today -- collapses to exactly
         * one profile; call it "Default" rather than "Migrated (zone 1)",
         * since it isn't really zone 1's private setting, it's simply the
         * firmware default every zone was already (implicitly) using. */
        if (profile_count == 1) {
            snprintf(out->timing_profiles[0].name, sizeof(out->timing_profiles[0].name), "Default");
        } else {
            /* "Zone %u", not "Migrated %u" -- TIMING_PROFILE_NAME_MAX_LEN is
             * only 7 (see its own comment for why), too short for "Migrated 1"
             * (10 chars) to survive without silent truncation. "Zone 1"/
             * "Zone 2"/"Zone 3" fits with room to spare and is still a name an
             * operator recognizes: each of these profiles came from exactly
             * one zone's own v8 values. */
            /* The digit is written as a single char rather than with %u so
             * the compiler can see the output length. "Zone %u" against a
             * 7-char name is fine for any real channel count, but %u's widest
             * expansion is ten digits, which the target build's
             * -Werror=format-truncation rejects on a bound it cannot prove.
             * The static assert is what actually keeps this honest: it fails
             * the build if the channel count ever reaches double digits,
             * rather than letting the name silently lose its digit. */
            _Static_assert(MAX31856_CHANNEL_COUNT <= 9,
                           "single-digit zone naming below assumes at most 9 channels");
            for (uint8_t p = 0; p < profile_count; p++) {
                snprintf(out->timing_profiles[p].name, sizeof(out->timing_profiles[p].name),
                         "Zone %c", (char)('0' + p + 1));
            }
        }
        /* profile_count is always >= 1 here: MAX31856_CHANNEL_COUNT (the
         * loop bound above) is a fixed hardware constant > 0, so the loop
         * always runs at least once and always allocates at least the first
         * zone's profile. timing_profile_count must never be 0 in a valid
         * config -- see its own comment on zones_cfg_t -- and this migration
         * never produces that. */
        out->timing_profile_count = profile_count;
        return true;
    }
    default:
        /* No known historical (or current) layout for this version --
         * expected_len_for_version() already returned 0 for it and
         * decode_zones_blob() should never reach here; kept as a defensive
         * explicit refusal rather than silently guessing. */
        return false;
    }
}

/* esp_crc32_le() (same helper crash_report.c's compute_crc() uses) over the
 * struct with crc32 itself zeroed -- computed over a local copy so a caller
 * re-validating an already-loaded cfg's crc32 is never mutated by asking. */
static uint32_t compute_zones_crc(const zones_cfg_t *cfg)
{
    zones_cfg_t tmp = *cfg;
    tmp.crc32 = 0;
    return esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}

typedef enum {
    ZONES_DECODE_OK,      /* *out is a valid, current-format struct, ready to adopt */
    ZONES_DECODE_CORRUPT, /* reject outright: wrong length for claimed version, unknown
                           * version, CRC mismatch, or failed validate_zones_cfg() --
                           * *out is zeroed, nothing is adopted */
    ZONES_DECODE_NEWER,   /* version > ZONES_CFG_VERSION -- refuse without guessing;
                           * *out is zeroed, but the caller must treat the SOURCE bytes
                           * as real, protected data (see nvs_load_from()'s *out_found) */
} zones_decode_result_t;

/* The one place a stored zones_cfg blob (from NVS or a kiln_cfg_store import)
 * is turned into a trustworthy, current-format zones_cfg_t. Implements items
 * 1-3 of the "saved securely like the others" fix: a length check against the
 * blob's OWN claimed version before anything is copied or interpreted, typed
 * per-version conversion (never a memcpy of one struct shape over another),
 * and validate_zones_cfg() run on every path, not just import. Item 4 (CRC)
 * is folded in here too, for the current-version case only -- see
 * zones_cfg_t::crc32's comment for why older versions have no CRC to check. */
static zones_decode_result_t decode_zones_blob(const void *blob, size_t len, zones_cfg_t *out,
                                                const char **err_reason)
{
    static const char *unused_reason;
    const char **reason = err_reason ? err_reason : &unused_reason;
    *reason = "";
    memset(out, 0, sizeof(*out));

    if (!blob || len < sizeof(((zones_cfg_t *)0)->version)) {
        *reason = "blob missing or too short to contain a version";
        return ZONES_DECODE_CORRUPT;
    }
    uint8_t version = ((const uint8_t *)blob)[0];

    if (version > ZONES_CFG_VERSION) {
        /* Firmware-rollback case (TODO.md 8.1) -- this build does not know
         * that layout and must not guess at it. Deliberately does NOT check
         * length against anything here: an unknown newer layout could be any
         * size, and the whole point of this branch is refusing to interpret
         * it at all. */
        *reason = "this config was saved by newer firmware -- refusing rather than guessing";
        return ZONES_DECODE_NEWER;
    }

    size_t expected = expected_len_for_version(version);
    if (expected == 0) {
        *reason = "unknown/unsupported zones_cfg version";
        return ZONES_DECODE_CORRUPT;
    }
    if (len != expected) {
        *reason = "blob length does not match its claimed version -- treating as corrupt";
        return ZONES_DECODE_CORRUPT;
    }

    if (version == ZONES_CFG_VERSION) {
        memcpy(out, blob, sizeof(*out));
        uint32_t stored_crc = out->crc32;
        uint32_t computed_crc = compute_zones_crc(out);
        if (computed_crc != stored_crc) {
            memset(out, 0, sizeof(*out));
            *reason = "CRC mismatch -- treating as corrupt";
            return ZONES_DECODE_CORRUPT;
        }
    } else {
        if (!convert_versioned_blob_to_current(version, blob, out)) {
            memset(out, 0, sizeof(*out));
            *reason = "unable to convert stored version to the current layout";
            return ZONES_DECODE_CORRUPT;
        }
        out->version = ZONES_CFG_VERSION;
        /* out->crc32 stays 0 here -- a migrated struct has never been saved
         * in the current format yet, so there is no stored CRC to check
         * against. nvs_save() stamps a real one the next time this config is
         * written, current or not. */
    }

    const char *validate_reason = "invalid stored config";
    if (!validate_zones_cfg(out, &validate_reason)) {
        memset(out, 0, sizeof(*out));
        *reason = validate_reason;
        return ZONES_DECODE_CORRUPT;
    }
    return ZONES_DECODE_OK;
}

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
 * decode_zones_blob() decoder (length check first, typed per-version
 * conversion, validate_zones_cfg(), CRC on the current-version path). *out_found
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

    /* Raw byte buffer, not out_cfg directly: decode_zones_blob() needs the
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
    zones_decode_result_t result = decode_zones_blob(raw, len, out_cfg, &reason);
    switch (result) {
    case ZONES_DECODE_OK:
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
         * validate_zones_cfg()) -- loud enough that an operator can see it,
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
     * compute_zones_crc()/zones_cfg_t::crc32's comments. Any in-RAM edit that
     * lands here (a setter, a POST commit, an import) gets a fresh, correct
     * CRC every time this function runs; there is no path that writes the
     * blob without also re-stamping it. */
    s_zones.cfg.crc32 = compute_zones_crc(&s_zones.cfg);

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

/* Same "compute over a zeroed-crc-field copy" convention as compute_zones_crc(). */
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
 * interpretation" way decode_zones_blob() uses for the zones blob, not
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
 * ZONES_CONFIG_BLOB_MAX_SIZE (512) already has zones_cfg_t sitting at 500;
 * three more floats plus the version/CRC bookkeeping would either force that
 * ceiling up (a real decision with knock-on effects on kiln_cfg_store.c's
 * fixed per-entry size, see that macro's own comment) or claw back yet more
 * bytes from zone_cfg_t for data that has nothing to do with a zone's own
 * thermal record. Also not safety-critical -- nothing on the guard/control
 * path reads it, only Task 2's WARNING predicate above -- so it has no
 * business sharing a version/CRC/load transaction with data that is. */
#define ZONE_NORMALS_CFG_VERSION 1
#define NVS_KEY_ZONE_NORMALS "zone_normals_cfg"

typedef struct {
    uint8_t  version;
    uint8_t  measured_mask; /* bit i = zone i has a measured normal current */
    float    normal_current_a[MAX31856_CHANNEL_COUNT];
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

/* Same bound parse_zone_fields()'s z%u_mode enforces (0-2). */
bool zones_config_set_control_mode(uint8_t zone_index, zone_control_mode_t mode)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if ((unsigned)mode > (unsigned)ZONE_CONTROL_MODE_PID) {
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
    if (!isfinite(min_off_ms) || min_off_ms < 0.0f || min_off_ms > ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
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
     * s_zones.cfg (POST /api/zones, NVS load via validate_zones_cfg(), a
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

/* Validates every field of `cand` -- a fully migrated, CURRENT-version
 * zones_cfg_t -- against the exact bounds parse_zone_fields()/
 * zones_config_set_*() enforce on a live POST. Used only by
 * zones_config_import_blob() below; a config stored by kiln_cfg_store.c may
 * have been saved years ago, under looser bounds, or by firmware this build
 * has since tightened, so it is re-checked here rather than trusted because
 * it was valid once. */
static bool validate_zones_cfg(const zones_cfg_t *cand, const char **err_reason)
{
    if (cand->thermo_count > MAX31856_CHANNEL_COUNT) {
        *err_reason = "thermo_count out of range";
        return false;
    }
    if (cand->relay_count > KILN_IO_RELAY_COUNT) {
        *err_reason = "relay_count out of range";
        return false;
    }
    if (cand->max_simultaneous_relays > KILN_IO_RELAY_COUNT) {
        *err_reason = "max_simultaneous_relays out of range";
        return false;
    }
    if (cand->safety_tc_type > ZONE_TC_TYPE_MAX_REAL) {
        *err_reason = "safety_tc_type out of range";
        return false;
    }
    if (!isfinite(cand->pc_link_abort_silence_ms) || cand->pc_link_abort_silence_ms < 0.0f ||
        cand->pc_link_abort_silence_ms > ZONE_PC_LINK_SILENCE_MS_MAX) {
        *err_reason = "pc_link_abort_silence_ms out of range";
        return false;
    }
    /* 2026-08-27 (ZONES_CFG_VERSION 8->9): timing_profile_count must be at
     * least 1 -- every zone_cfg_t::timing_profile, including a freshly
     * zero-initialized zone's 0, must resolve to a real profile -- and at
     * most MAX31856_CHANNEL_COUNT, the array's fixed capacity (see
     * zones_cfg_t::timing_profile_count's own comment for why that bound,
     * not some larger operator-facing limit, is the correct ceiling). Each
     * IN-USE profile's nine fields get the exact same per-field bounds these
     * nine had as zone_cfg_t fields before this pass -- only WHERE they live
     * changed, not their validation. */
    if (cand->timing_profile_count < 1 || cand->timing_profile_count > MAX31856_CHANNEL_COUNT) {
        *err_reason = "timing_profile_count out of range";
        return false;
    }
    for (uint8_t p = 0; p < cand->timing_profile_count; p++) {
        const zone_timing_profile_t *tp = &cand->timing_profiles[p];
        if (!isfinite(tp->guard_progress_duty_min) || tp->guard_progress_duty_min < 0.0f ||
            tp->guard_progress_duty_min > ZONE_GUARD_DUTY_MAX) {
            *err_reason = "timing profile guard_progress_duty_min out of range";
            return false;
        }
        if (!isfinite(tp->guard_progress_window_s) || tp->guard_progress_window_s < 0.0f ||
            tp->guard_progress_window_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "timing profile guard_progress_window_s out of range";
            return false;
        }
        if (!isfinite(tp->guard_drift_hysteresis_c) || tp->guard_drift_hysteresis_c < 0.0f ||
            tp->guard_drift_hysteresis_c > ZONE_GUARD_MARGIN_C_MAX) {
            *err_reason = "timing profile guard_drift_hysteresis_c out of range";
            return false;
        }
        if (!isfinite(tp->guard_frozen_eps_c) || tp->guard_frozen_eps_c < 0.0f ||
            tp->guard_frozen_eps_c > ZONE_GUARD_EPS_C_MAX) {
            *err_reason = "timing profile guard_frozen_eps_c out of range";
            return false;
        }
        if (!isfinite(tp->guard_cross_zone_period_s) || tp->guard_cross_zone_period_s < 0.0f ||
            tp->guard_cross_zone_period_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "timing profile guard_cross_zone_period_s out of range";
            return false;
        }
        if (!isfinite(tp->bangbang_hysteresis_c) || tp->bangbang_hysteresis_c < 0.0f ||
            tp->bangbang_hysteresis_c > ZONE_GUARD_MARGIN_C_MAX) {
            *err_reason = "timing profile bangbang_hysteresis_c out of range";
            return false;
        }
        if (!isfinite(tp->cooling_limited_margin_c) || tp->cooling_limited_margin_c < 0.0f ||
            tp->cooling_limited_margin_c > ZONE_GUARD_MARGIN_C_MAX) {
            *err_reason = "timing profile cooling_limited_margin_c out of range";
            return false;
        }
        if (!isfinite(tp->cooling_limited_hold_s) || tp->cooling_limited_hold_s < 0.0f ||
            tp->cooling_limited_hold_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "timing profile cooling_limited_hold_s out of range";
            return false;
        }
        if (!isfinite(tp->ramp_lock_band_c) || tp->ramp_lock_band_c < 0.0f ||
            tp->ramp_lock_band_c > ZONE_GUARD_MARGIN_C_MAX) {
            *err_reason = "timing profile ramp_lock_band_c out of range";
            return false;
        }
    }
    uint8_t relay_valid_bits =
        cand->relay_count >= 8 ? 0xFF : (uint8_t)((1u << cand->relay_count) - 1u);
    uint8_t thermo_valid_bits =
        cand->thermo_count >= 8 ? 0xFF : (uint8_t)((1u << cand->thermo_count) - 1u);
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        const zone_cfg_t *z = &cand->zones[i];
        /* tc_type is per-CHANNEL, meaningful past thermo_count too -- same
         * reasoning parse_zone_fields() applies (see its own comment). */
        if (z->tc_type > ZONE_TC_TYPE_MAX_REAL) {
            *err_reason = "zone tc_type out of range";
            return false;
        }
        if (i >= cand->thermo_count) {
            continue; /* unused trailing slot -- matches parse_zone_fields()'s early return */
        }
        if ((z->relay_mask & ~relay_valid_bits) != 0) {
            *err_reason = "zone relay_mask references an unconfigured relay";
            return false;
        }
        if ((z->thermo_mask & ~thermo_valid_bits) != 0) {
            *err_reason = "zone thermo_mask references an unconfigured thermocouple channel";
            return false;
        }
        {
            uint8_t ct_valid_bits = (uint8_t)((1u << ZONE_CT_CHANNEL_COUNT) - 1u);
            if ((z->ct_mask & ~ct_valid_bits) != 0) {
                *err_reason = "zone ct_mask references an unconfigured current-sense channel";
                return false;
            }
        }
        /* 2026-08-27 (ZONES_CFG_VERSION 8->9): must reference a profile that
         * actually exists in THIS candidate config -- timing_profile_count
         * was already bounds-checked above, so this is a straight index
         * check, same shape as thermo_mask/ct_mask's bit-range checks. */
        if (z->timing_profile >= cand->timing_profile_count) {
            *err_reason = "zone timing_profile references a timing profile that doesn't exist";
            return false;
        }
        if (!isfinite(z->cal_offset_c) || z->cal_offset_c < ZONE_CAL_OFFSET_MIN_C ||
            z->cal_offset_c > ZONE_CAL_OFFSET_MAX_C) {
            *err_reason = "zone cal_offset_c out of range";
            return false;
        }
        if (!isfinite(z->pid_kp) || z->pid_kp < 0.0f || z->pid_kp > 1000.0f) {
            *err_reason = "zone pid_kp out of range";
            return false;
        }
        if (!isfinite(z->pid_ki) || z->pid_ki < 0.0f || z->pid_ki > 1000.0f) {
            *err_reason = "zone pid_ki out of range";
            return false;
        }
        if (!isfinite(z->pid_kd) || z->pid_kd < 0.0f || z->pid_kd > 1000.0f) {
            *err_reason = "zone pid_kd out of range";
            return false;
        }
        if (!isfinite(z->max_ramp_c_per_hr) || z->max_ramp_c_per_hr < 0.0f ||
            z->max_ramp_c_per_hr > ZONE_MAX_RAMP_C_PER_HR_MAX) {
            *err_reason = "zone max_ramp_c_per_hr out of range";
            return false;
        }
        if (!isfinite(z->sanity_rate_c_per_min) || z->sanity_rate_c_per_min < 0.0f ||
            z->sanity_rate_c_per_min > ZONE_SANITY_RATE_MAX_C_PER_MIN) {
            *err_reason = "zone sanity_rate_c_per_min out of range";
            return false;
        }
        if (z->control_mode > (uint8_t)ZONE_CONTROL_MODE_PID) {
            *err_reason = "zone control_mode out of range";
            return false;
        }
        if (!isfinite(z->max_temp_c) || z->max_temp_c < 0.0f || z->max_temp_c > ZONE_MAX_TEMP_C_MAX) {
            *err_reason = "zone max_temp_c out of range";
            return false;
        }
        if (!isfinite(z->min_temp_c) || z->min_temp_c < ZONE_MIN_TEMP_C_MIN ||
            z->min_temp_c > ZONE_MIN_TEMP_C_MAX) {
            *err_reason = "zone min_temp_c out of range";
            return false;
        }
        if (!isfinite(z->heater_window_ms) || z->heater_window_ms < 0.0f ||
            z->heater_window_ms > ZONE_HEATER_WINDOW_MS_MAX) {
            *err_reason = "zone heater_window_ms out of range";
            return false;
        }
        if (!isfinite(z->heater_min_on_ms) || z->heater_min_on_ms < 0.0f ||
            z->heater_min_on_ms > ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
            *err_reason = "zone heater_min_on_ms out of range";
            return false;
        }
        if (!isfinite(z->heater_min_off_ms) || z->heater_min_off_ms < 0.0f ||
            z->heater_min_off_ms > ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
            *err_reason = "zone heater_min_off_ms out of range";
            return false;
        }
        if (!isfinite(z->guard_wrong_dir_window_s) || z->guard_wrong_dir_window_s < 0.0f ||
            z->guard_wrong_dir_window_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "zone guard_wrong_dir_window_s out of range";
            return false;
        }
        if (!isfinite(z->guard_wrong_dir_rate_c_per_min) || z->guard_wrong_dir_rate_c_per_min < 0.0f ||
            z->guard_wrong_dir_rate_c_per_min > ZONE_GUARD_RATE_C_PER_MIN_MAX) {
            *err_reason = "zone guard_wrong_dir_rate_c_per_min out of range";
            return false;
        }
        if (!isfinite(z->guard_off_settle_s) || z->guard_off_settle_s < 0.0f ||
            z->guard_off_settle_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "zone guard_off_settle_s out of range";
            return false;
        }
        if (!isfinite(z->guard_runaway_rate_c_per_min) || z->guard_runaway_rate_c_per_min < 0.0f ||
            z->guard_runaway_rate_c_per_min > ZONE_GUARD_RATE_C_PER_MIN_MAX) {
            *err_reason = "zone guard_runaway_rate_c_per_min out of range";
            return false;
        }
        if (!isfinite(z->guard_runaway_margin_c) || z->guard_runaway_margin_c < 0.0f ||
            z->guard_runaway_margin_c > ZONE_GUARD_MARGIN_C_MAX) {
            *err_reason = "zone guard_runaway_margin_c out of range";
            return false;
        }
        if (!isfinite(z->guard_drift_period_s) || z->guard_drift_period_s < 0.0f ||
            z->guard_drift_period_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "zone guard_drift_period_s out of range";
            return false;
        }
        if (!isfinite(z->guard_sensor_fault_debounce_ticks) || z->guard_sensor_fault_debounce_ticks < 0.0f ||
            z->guard_sensor_fault_debounce_ticks > ZONE_GUARD_DEBOUNCE_TICKS_MAX) {
            *err_reason = "zone guard_sensor_fault_debounce_ticks out of range";
            return false;
        }
        if (!isfinite(z->guard_frozen_window_s) || z->guard_frozen_window_s < 0.0f ||
            z->guard_frozen_window_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "zone guard_frozen_window_s out of range";
            return false;
        }
        if (!isfinite(z->cross_zone_max_delta_c) || z->cross_zone_max_delta_c < 0.0f ||
            z->cross_zone_max_delta_c > ZONE_CROSS_ZONE_DELTA_C_MAX) {
            *err_reason = "zone cross_zone_max_delta_c out of range";
            return false;
        }
        /* The nine timing-override checks that used to live here moved to the
         * timing_profiles[] loop above -- z->timing_profile's own bound check,
         * a few lines up, is what stands in their place at THIS per-zone spot
         * now (see ZONES_CFG_VERSION's 8->9 comment). */
        if (!isfinite(z->model_k_dc) || !isfinite(z->model_tau_s) || !isfinite(z->model_dead_time_s) ||
            z->model_k_dc < 0.0f || z->model_tau_s < 0.0f || z->model_dead_time_s < 0.0f ||
            z->model_k_dc > ZONE_MODEL_K_MAX || z->model_tau_s > ZONE_MODEL_TIME_MAX_S ||
            z->model_dead_time_s > ZONE_MODEL_TIME_MAX_S) {
            *err_reason = "zone plant model out of range";
            return false;
        }
    }
    return true;
}

bool zones_config_import_blob(const void *blob, size_t len, char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap) {
        reason_out[0] = '\0';
    }

    /* Same decoder nvs_load_from() uses -- length-vs-claimed-version check
     * before anything is interpreted, typed per-version conversion (never a
     * memcpy of one struct shape over another), validate_zones_cfg(), and a
     * CRC check on the current-version path. This blob may have been saved
     * years ago by older firmware under looser bounds (kiln_cfg_store.c), so
     * it gets exactly the same scrutiny a blob read off flash does -- no
     * separate, looser path for "this one came from a kiln config slot
     * instead of the live NVS key." */
    zones_cfg_t cand;
    const char *reason = "";
    zones_decode_result_t result = decode_zones_blob(blob, len, &cand, &reason);
    if (result != ZONES_DECODE_OK) {
        if (reason_out && reason_cap) {
            snprintf(reason_out, reason_cap, "%s", reason);
        }
        return false;
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
    char json[5120]; /* 4608 -> 5120 (2026-08-27+2, Tasks 1/2/3): one
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
    size_t o = 0;
    int n;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(json + o, sizeof(json) - o, __VA_ARGS__);                                   \
        if (n < 0 || (size_t)n >= sizeof(json) - o) {                                             \
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
            "\"timing_profile\":%u,\"normal_current_measured\":%s,\"normal_current_a\":%.3f}",
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
            z->timing_profile, normal_measured ? "true" : "false", (double)normal_a);
    }
    APPEND("]}");

#undef APPEND

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);

    /* Reached only if `json` is too small for the config it holds. The old
     * behaviour was to send what had been written so far, which is a truncated
     * JSON document: the page's fetch throws on it, and the operator sees a
     * settings page stuck on "Loading" with no idea their zone config is fine
     * and only the response was too big. A 500 with a valid body at least says
     * what happened. Sizing `json` is the actual fix; this is the guard that
     * makes an undersized buffer visible instead of silent. */
truncated:
    ESP_LOGE(TAG, "GET /api/zones did not fit in %u bytes -- raise the buffer", (unsigned)sizeof(json));
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req,
                              "{\"ok\":false,\"error\":\"zone config did not fit in the response "
                              "buffer -- this is a firmware sizing bug, not a bad configuration\"}");
}

/* ---- POST /api/zones ------------------------------------------------------
 * Whole-page submit; every field validated into a scratch struct before
 * anything is written to the in-RAM copy or NVS -- reject cleanly, never
 * partially apply, same discipline as every other untrusted-input boundary
 * in this codebase. */

static bool parse_u8_field(const char *body, const char *key, long min, long max, uint8_t *out)
{
    char val[8];
    int len = http_form_find_field(body, key, val, sizeof(val));
    if (len <= 0) {
        return false;
    }
    char *end = NULL;
    long v = strtol(val, &end, 10);
    /* *end != '\0' catches trailing garbage after a valid numeric prefix
     * (e.g. "1200X" -> strtol happily returns 1200 with end pointing at 'X')
     * -- end == val alone only rejects "no digits at all", not "some digits
     * then junk". Every operator-settable field this function backs is
     * safety-relevant (see this module's header note), so a value that
     * isn't ENTIRELY the number it claims to be must be refused outright,
     * not silently truncated to whatever numeric prefix happened to parse. */
    if (end == val || *end != '\0' || v < min || v > max) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static bool parse_float_field(const char *body, const char *key, float min, float max, float *out)
{
    char val[24];
    int len = http_form_find_field(body, key, val, sizeof(val));
    if (len <= 0) {
        return false;
    }
    char *end = NULL;
    float v = strtof(val, &end);
    /* *end != '\0' -- same trailing-garbage rejection as parse_u8_field()
     * above; see its comment. NaN is already correctly rejected here, and
     * inf is caught incidentally by the finite min/max bounds -- neither of
     * those is what this check is for. */
    if (end == val || *end != '\0' || isnan(v) || v < min || v > max) {
        return false;
    }
    *out = v;
    return true;
}

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
            if (!parse_u8_field(body, key, 0, ZONE_TC_TYPE_MAX_REAL, &tc_type_raw)) {
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
    if (!parse_u8_field(body, key, 0, 0xFF, &relay_mask_raw)) {
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
    if (!parse_u8_field(body, key, 0, timing_profile_count - 1, &z->timing_profile)) {
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
            if (!parse_u8_field(body, key, 0, 0xFF, &thermo_mask_raw)) {
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
            if (!parse_u8_field(body, key, 0, 0xFF, &ct_mask_raw)) {
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
    if (!parse_float_field(body, key, ZONE_CAL_OFFSET_MIN_C, ZONE_CAL_OFFSET_MAX_C, &z->cal_offset_c)) {
        *err_reason = "zone cal_offset_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_kp", i);
    if (!parse_float_field(body, key, 0.0f, 1000.0f, &z->pid_kp)) {
        *err_reason = "zone pid_kp missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_ki", i);
    if (!parse_float_field(body, key, 0.0f, 1000.0f, &z->pid_ki)) {
        *err_reason = "zone pid_ki missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_kd", i);
    if (!parse_float_field(body, key, 0.0f, 1000.0f, &z->pid_kd)) {
        *err_reason = "zone pid_kd missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_ramp", i);
    if (!parse_float_field(body, key, 0.0f, ZONE_MAX_RAMP_C_PER_HR_MAX, &z->max_ramp_c_per_hr)) {
        *err_reason = "zone max_ramp_c_per_hr missing or out of range";
        return false;
    }
    /* 0 = "never configured" (profile_executor.c substitutes its own
     * default); 20 C/min is a generous ceiling -- well above anything this
     * board's bang-bang control could plausibly produce, just a sanity bound
     * against a typo. */
    snprintf(key, sizeof(key), "z%u_sanity", i);
    if (!parse_float_field(body, key, 0.0f, ZONE_SANITY_RATE_MAX_C_PER_MIN, &z->sanity_rate_c_per_min)) {
        *err_reason = "zone sanity_rate_c_per_min missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_mode", i);
    uint8_t mode_raw;
    if (!parse_u8_field(body, key, 0, 2, &mode_raw)) {
        *err_reason = "zone control_mode missing or out of range (0-2)";
        return false;
    }
    z->control_mode = mode_raw;
    /* 1400C ceiling matches PROFILE_TARGET_C_MAX (profiles_http.c) -- a
     * guard 5 limit tighter than what a profile could ever request would be
     * a contradiction between the two checks. */
    snprintf(key, sizeof(key), "z%u_maxtemp", i);
    if (!parse_float_field(body, key, 0.0f, ZONE_MAX_TEMP_C_MAX, &z->max_temp_c)) {
        *err_reason = "zone max_temp_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_mintemp", i);
    if (!parse_float_field(body, key, ZONE_MIN_TEMP_C_MIN, ZONE_MIN_TEMP_C_MAX, &z->min_temp_c)) {
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
    if (!parse_float_field(body, key, 0.0f, ZONE_HEATER_WINDOW_MS_MAX, &z->heater_window_ms)) {
        *err_reason = "zone heater_window_ms missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_minon", i);
    if (!parse_float_field(body, key, 0.0f, ZONE_HEATER_MIN_ON_OFF_MS_MAX, &z->heater_min_on_ms)) {
        *err_reason = "zone heater_min_on_ms missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_minoff", i);
    if (!parse_float_field(body, key, 0.0f, ZONE_HEATER_MIN_ON_OFF_MS_MAX, &z->heater_min_off_ms)) {
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
            if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_wrong_dir_window_s)) {
                *err_reason = "zone guard_wrong_dir_window_s out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_wrongdirrate", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_RATE_C_PER_MIN_MAX, &z->guard_wrong_dir_rate_c_per_min)) {
                *err_reason = "zone guard_wrong_dir_rate_c_per_min out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_offsettle", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_off_settle_s)) {
                *err_reason = "zone guard_off_settle_s out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_runawayrate", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_RATE_C_PER_MIN_MAX, &z->guard_runaway_rate_c_per_min)) {
                *err_reason = "zone guard_runaway_rate_c_per_min out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_runawaymargin", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_MARGIN_C_MAX, &z->guard_runaway_margin_c)) {
                *err_reason = "zone guard_runaway_margin_c out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_driftperiod", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_drift_period_s)) {
                *err_reason = "zone guard_drift_period_s out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_debounce", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_DEBOUNCE_TICKS_MAX, &z->guard_sensor_fault_debounce_ticks)) {
                *err_reason = "zone guard_sensor_fault_debounce_ticks out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_frozenwindow", i);
    {
        char probe[16];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_frozen_window_s)) {
                *err_reason = "zone guard_frozen_window_s out of range";
                return false;
            }
        }
    }
    /* The nine v8 overrides that used to be parsed inline here now live on
     * the timing profile this zone points at -- see z%u_timingprofile above
     * and parse_timing_profile_fields() (tp%u_progressduty..tp%u_ramplock),
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
            if (!parse_float_field(body, key, 0.0f, ZONE_CROSS_ZONE_DELTA_C_MAX, &z->cross_zone_max_delta_c)) {
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
            if (!parse_float_field(body, key, 0.0f, ZONE_MODEL_K_MAX, &z->model_k_dc)) {
                *err_reason = "zone model K out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_tau", i);
    {
        char probe[24];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!parse_float_field(body, key, 0.0f, ZONE_MODEL_TIME_MAX_S, &z->model_tau_s)) {
                *err_reason = "zone model tau out of range";
                return false;
            }
        }
    }
    snprintf(key, sizeof(key), "z%u_deadtime", i);
    {
        char probe[24];
        if (http_form_find_field(body, key, probe, sizeof(probe)) > 0) {
            if (!parse_float_field(body, key, 0.0f, ZONE_MODEL_TIME_MAX_S, &z->model_dead_time_s)) {
                *err_reason = "zone model dead time out of range";
                return false;
            }
        }
    }
    return true;
}

/* Parses tp<p>_name and its nine timing fields into *tp -- the POST /api/zones
 * authority for zone_timing_profile_t, matching parse_zone_fields()'s own
 * discipline: every field checked into a scratch struct, nothing written to
 * *tp on any rejection path partway through (a caller that gets `false` back
 * must not trust *tp's partially-written contents). Unlike the zone fields
 * this profile replaces, every one of these nine is REQUIRED, not OPTIONAL --
 * see the comment on this loop's caller for why "no legacy client to stay
 * compatible with" applies here.
 *
 * The caller (zones_post_handler()) has already confirmed tp<p>_name is
 * present before calling this -- that's how it decided profile slot p is
 * part of the submission at all -- so this function re-reads the name field
 * itself rather than taking it as a parameter, the same "probe, then parse
 * for real" pattern this file's optional-field blocks already use. */
static bool parse_timing_profile_fields(const char *body, uint8_t p, zone_timing_profile_t *tp,
                                        const char **err_reason)
{
    char key[24];

    snprintf(key, sizeof(key), "tp%u_name", p);
    char name[TIMING_PROFILE_NAME_MAX_LEN + 1];
    int name_len = http_form_find_field(body, key, name, sizeof(name));
    if (name_len == -2) {
        *err_reason = "timing profile name too long";
        return false;
    }
    if (name_len < 0) {
        /* Caller already confirmed presence via its own probe immediately
         * before calling this -- reaching "absent" here would mean the body
         * changed between those two reads, which cannot happen (both read
         * the same `body` pointer within one request). Kept as an explicit
         * refusal rather than assumed unreachable. */
        *err_reason = "timing profile name missing";
        return false;
    }
    strncpy(tp->name, name, TIMING_PROFILE_NAME_MAX_LEN);
    tp->name[TIMING_PROFILE_NAME_MAX_LEN] = '\0';

    snprintf(key, sizeof(key), "tp%u_progressduty", p);
    if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_DUTY_MAX, &tp->guard_progress_duty_min)) {
        *err_reason = "timing profile guard_progress_duty_min missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_progresswindow", p);
    if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &tp->guard_progress_window_s)) {
        *err_reason = "timing profile guard_progress_window_s missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_drifthyst", p);
    if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_MARGIN_C_MAX, &tp->guard_drift_hysteresis_c)) {
        *err_reason = "timing profile guard_drift_hysteresis_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_frozeneps", p);
    if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_EPS_C_MAX, &tp->guard_frozen_eps_c)) {
        *err_reason = "timing profile guard_frozen_eps_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_xzoneperiod", p);
    if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &tp->guard_cross_zone_period_s)) {
        *err_reason = "timing profile guard_cross_zone_period_s missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_bbhyst", p);
    if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_MARGIN_C_MAX, &tp->bangbang_hysteresis_c)) {
        *err_reason = "timing profile bangbang_hysteresis_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_coolmargin", p);
    if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_MARGIN_C_MAX, &tp->cooling_limited_margin_c)) {
        *err_reason = "timing profile cooling_limited_margin_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_coolhold", p);
    if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &tp->cooling_limited_hold_s)) {
        *err_reason = "timing profile cooling_limited_hold_s missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_ramplock", p);
    if (!parse_float_field(body, key, 0.0f, ZONE_GUARD_MARGIN_C_MAX, &tp->ramp_lock_band_c)) {
        *err_reason = "timing profile ramp_lock_band_c missing or out of range";
        return false;
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

    char body[ZONES_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "zones body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    zones_cfg_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    if (!parse_u8_field(body, "thermo_count", 0, MAX31856_CHANNEL_COUNT, &tmp.thermo_count)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "thermo_count missing or out of range");
        return ESP_OK;
    }
    if (!parse_u8_field(body, "relay_count", 0, KILN_IO_RELAY_COUNT, &tmp.relay_count)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "relay_count missing or out of range");
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
             * prefix (e.g. "2X"), same gap as parse_u8_field()/
             * parse_float_field() above -- end == val alone lets it through. */
            if (end == val || *end != '\0' || v < 0 || v > KILN_IO_RELAY_COUNT) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "max_simultaneous_relays out of range");
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
            return ESP_OK;
        }
        if (probe_len < 0) {
            break; /* no tp<p>_name -- this and every following slot is absent from this submission */
        }
        const char *err_reason = "invalid timing profile field";
        if (!parse_timing_profile_fields(body, p, &tmp.timing_profiles[p], &err_reason)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_reason);
            return ESP_OK;
        }
        tmp.timing_profile_count = (uint8_t)(p + 1);
    }
    if (tmp.timing_profile_count == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "at least one timing profile (tp0_name) is required");
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
             * numeric parsers in this file -- see parse_u8_field()'s comment. */
            if (end == val || *end != '\0' || v < 0 || v > ZONE_TC_TYPE_MAX_REAL) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "safety_tc_type must be a real thermocouple type (0-7: "
                                    "B/E/J/K/N/R/S/T), not a voltage-input mode");
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
            if (!parse_float_field(body, "pc_link_abort_silence_ms", 0.0f,
                                   ZONE_PC_LINK_SILENCE_MS_MAX, &tmp.pc_link_abort_silence_ms)) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "pc_link_abort_silence_ms out of range (0 = firmware default)");
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
            samples++;
        }
    }

    deps->force_off(deps->ctx); /* choke point -- every path out of this zone goes through here */

    if (outcome == ZONE_SWEEP_ZONE_OK && samples > 0 && out_avg_current_a) {
        *out_avg_current_a = sum_a / (float)samples;
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
        uint32_t refused_sources = 0;
        zone_sweep_zone_outcome_t outcome =
            zone_sweep_run_one_zone(zi, relay_mask, deps, &avg_a, &refused_sources);

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
            /* Kept short and unconditionally non-truncating: out->reason is
             * char[64] (matching zones_http.h's zone_sweep_status_t), and
             * -Werror=format-truncation flags any snprintf() into it that
             * COULD truncate even if this specific zi/refused_sources pair
             * never would. */
            snprintf(out->reason, sizeof(out->reason), "zone %u energize refused (0x%02X)", zi,
                     (unsigned)refused_sources);
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
        .zone_done = zone_sweep_task_zone_done,
        .ctx = NULL,
    };

    zone_sweep_all_result_t result;
    zone_sweep_run_all_zones(s_sweep.zones_total, &hw_deps, &hw_hooks, &result);

    s_sweep.state = result.state;
    strncpy((char *)s_sweep.reason, result.reason, sizeof(s_sweep.reason) - 1);
    s_sweep.reason[sizeof(s_sweep.reason) - 1] = '\0';

    zone_sweep_force_relays_off(); /* final choke point -- covers normal completion too */
    if (s_sweep.state != ZONE_SWEEP_ABORTED && s_sweep.state != ZONE_SWEEP_FAILED) {
        s_sweep.state = ZONE_SWEEP_DONE;
        s_sweep.reason[0] = '\0';
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
    char json[320];
    int n = snprintf(json, sizeof(json),
                     "{\"state\":\"%s\",\"zone_index\":%u,\"zones_done\":%u,\"zones_total\":%u,"
                     "\"reason\":\"%s\"}",
                     zone_sweep_state_str(st.state), st.zone_index, st.zones_done, st.zones_total,
                     reason_escaped);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n > 0 ? (size_t)n : 0);
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

    ESP_LOGI(TAG, "zones API up (thermo_count=%u, relay_count=%u)", s_zones.cfg.thermo_count,
             s_zones.cfg.relay_count);
    return ESP_OK;
}
