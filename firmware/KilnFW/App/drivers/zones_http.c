#include "zones_http.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "MAX31856.h"
#include "http_form.h"
#include "kiln_io.h"
#include "thermo_owner.h"
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
 * migrate_zones_cfg_v1_to_current() below for the fill-in. */
#define ZONES_CFG_VERSION 5

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

/* One zone per configured thermocouple channel -- see zones_http.h. Bounded
 * by the hardware, not by anything a client can grow. */
typedef struct {
    char name[ZONE_NAME_MAX_LEN + 1];
    uint8_t relay_mask; /* bit N-1 = relay N belongs to this zone, N in 1..relay_count */
    float cal_offset_c; /* applied via zones_config_apply_cal() by dashboard_http.c and
                         * profile_executor.c, but not by uart_bridge.c -- see header */
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float max_ramp_c_per_hr; /* user-entered ceiling; 0 = never configured */
    float sanity_rate_c_per_min; /* direction/rate sanity threshold; 0 = never
                                   * configured -- see zones_config_get_sanity_rate() */
    uint8_t control_mode;    /* zone_control_mode_t (TODO.md 6A.1) */
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
    /* TODO.md 10.8 (2026-08-17): which MAX31856 channels combine (mean of
     * valid readings, thermo_combine.c) into this zone's control
     * temperature -- bit N-1 = channel N, same convention as relay_mask
     * above. See zones_config_get_thermo_mask()'s doc comment (zones_http.h)
     * for the legacy-mapping default a 0 here falls back to when the field
     * was never explicitly supplied, which is what keeps this addition from
     * being the kind of silent-wipe field growth TODO.md 6A.1's
     * relay_cycles.c note (section 6A.1, 2026-08-12) warns against. */
    uint8_t thermo_mask;
} zone_cfg_t;

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
 * single 0-7 u8 field, smaller still -- also left inside the existing 4096. */
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

/* ---- NVS ---------------------------------------------------------------- */

static esp_err_t nvs_save(void);
static void migrate_zones_cfg_v1_to_current(zones_cfg_t *cfg);

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
 * the three-outcome version handling nvs_load() relies on. *out_found reports
 * whether the key held something WORTH NOT DISTURBING -- true for a
 * decoded-and-trustworthy blob (current or migrated-older) AND for a blob
 * refused as newer-than-firmware (a real config this build must not clobber,
 * even though it can't use it), false for genuine corruption (too short, or
 * the wrong size for its claimed version) where there is nothing being
 * protected and a caller is free to look elsewhere. This is what the
 * one-time migration below keys off. */
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

    size_t len = sizeof(*out_cfg);
    err = nvs_get_blob(h, NVS_KEY_ZONES, out_cfg, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(out_cfg, 0, sizeof(*out_cfg));
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "zones_cfg blob read from '%s' failed (%s) -- treating as unreadable",
                 partition, esp_err_to_name(err));
        memset(out_cfg, 0, sizeof(*out_cfg));
        return ESP_OK;
    }
    if (out_found) {
        *out_found = true;
    }
    /* BUG FIXED 2026-08-13: this used to reject on `len != sizeof(*out_cfg)`
     * BEFORE ever looking at `version`, which made the version-based
     * migration path below dead code for the one case it exists for --
     * adding a field grows sizeof(zones_cfg_t), so a pre-existing (smaller,
     * older-version) blob would always fail that check and get wiped
     * instead of migrated, even though nvs_get_blob() already copied
     * everything the old blob had (out_cfg was zeroed first, so any new
     * trailing fields correctly read as their zero default). Only a blob
     * too short to even contain the `version` byte is unreadable; anything
     * else is the version check's job now, matching what TODO.md 8.2's
     * "three outcomes, not two" actually asked for. */
    if (len < sizeof(out_cfg->version)) {
        ESP_LOGW(TAG, "zones_cfg blob from '%s' is too short to contain a version -- treating as unreadable",
                 partition);
        memset(out_cfg, 0, sizeof(*out_cfg));
        /* FIX 1: this is genuine corruption, not a rollback refusal -- there
         * is no real, understandable-by-someone config being protected here,
         * so unlike the newer-than-firmware case below, this must NOT block
         * a caller (migrate_from_default_partition() via zones_http_start())
         * from trying the other partition for something usable. *out_found
         * reports "nothing worth keeping was found here", matching *out_valid
         * staying false. */
        if (out_found) {
            *out_found = false;
        }
        return ESP_OK;
    }

    if (out_cfg->version == ZONES_CFG_VERSION) {
        if (len != sizeof(*out_cfg)) {
            /* Current version but wrong size can only mean genuine
             * corruption -- a real current-version blob is always written
             * at exactly sizeof(*out_cfg). Same "corrupt, not refused"
             * reasoning as the too-short branch above: *out_found reports
             * nothing worth keeping was found, so migration is still free to
             * run. */
            ESP_LOGW(TAG, "zones_cfg blob from '%s' claims current version but is the wrong size -- "
                          "treating as unreadable", partition);
            memset(out_cfg, 0, sizeof(*out_cfg));
            if (out_found) {
                *out_found = false;
            }
            return ESP_OK;
        }
        if (out_valid) {
            *out_valid = true; /* current version -- happy path */
        }
        return ESP_OK;
    }
    if (out_cfg->version < ZONES_CFG_VERSION) {
        /* Known older layout -- run it through the migration chain. A v1
         * blob is shorter than the current struct (it predates
         * continue_on_zone_trip); nvs_get_blob() already copied everything
         * it had into a zeroed out_cfg, so the new field reads as 0 --
         * exactly the "abort the whole firing" default TODO.md 6A.3 asks
         * for, with no explicit conversion needed. */
        ESP_LOGI(TAG, "zones_cfg from '%s' is version %u, migrating to %u", partition,
                 (unsigned)out_cfg->version, (unsigned)ZONES_CFG_VERSION);
        migrate_zones_cfg_v1_to_current(out_cfg);
        if (out_valid) {
            *out_valid = true; /* migrated -- still a real, trustworthy config */
        }
        return ESP_OK;
    }
    /* out_cfg->version > ZONES_CFG_VERSION: the data was written by NEWER
     * firmware than this build. This is the firmware-rollback case from
     * TODO.md 8.1 -- an operator rolled back after a bad update, and the data
     * on flash may use fields/layout this older build doesn't know about.
     * Wiping it here would destroy config the newer firmware (or a
     * roll-forward back to it) still needs, so refuse to load instead: leave
     * flash untouched and fall back to defaults for this boot only. */
    ESP_LOGW(TAG, "zones_cfg from '%s' is version %u, newer than this firmware's %u -- "
                  "refusing to load, flash data left untouched",
             partition, (unsigned)out_cfg->version, (unsigned)ZONES_CFG_VERSION);
    memset(out_cfg, 0, sizeof(*out_cfg));
    /* FIX 1 (bug found in review): *out_found must be TRUE here, the
     * opposite of what this used to do. A refused newer-version blob is a
     * real, deliberately-protected config -- kiln_nvs genuinely has
     * something -- so a caller deciding whether it is safe to run the
     * legacy-partition migration (zones_http_start()) MUST see this as
     * "found" and skip migration, or it will overwrite the very data this
     * refusal exists to protect with a stale pre-split copy. This is why
     * *out_found and *out_valid are reported separately in the first place:
     * found means "the key existed", valid means "and it's safe to run a
     * kiln against" -- a refused blob is found-but-not-valid, never treated
     * as "nothing here" the way genuine corruption (the two branches above)
     * is. */
    if (out_found) {
        *out_found = true;
    }
    /* out_valid already false: refused, not trustworthy for this boot. */
    return ESP_OK;
}

/* Migrates an older on-flash zones_cfg_t layout forward. v1 -> v2
 * (2026-08-13) added continue_on_zone_trip as a new trailing field; a v1
 * blob is shorter, but nvs_get_blob() already copied it into a zeroed
 * out_cfg before this runs (see nvs_load_from()), so the new field already
 * reads as 0 -- no explicit conversion needed, just the version bump.
 *
 * v3 -> v4 (2026-08-17, TODO.md 10.8) added zone_cfg_t::thermo_mask, and
 * THIS one needs an explicit conversion, unlike every field before it: 0 is
 * not a safe "not configured yet" default here the way it was for
 * continue_on_zone_trip. Every pre-10.8 blob's zones were already reading a
 * real thermocouple, implicitly, through the "zone i <-> channel i" mapping
 * this module used to hard-code (zones_http.h's old scope note) -- if this
 * function left thermo_mask at its zeroed default, every existing zone on
 * every board that has ever saved a config would go dark (thermo_combine.c
 * sees an empty mask, reports zero valid readings, and that's the same
 * "thermocouple invalid" state a real sensor fault produces) the moment
 * this firmware boots, with no operator action and no warning beyond
 * whatever guard 6 eventually trips. Filling in bit i for zone i
 * reproduces the exact mapping every migrated zone was already using, so a
 * migrated board controls off the same channel it always did until an
 * operator explicitly assigns something else. Bounded by
 * MAX31856_CHANNEL_COUNT (the zones[] array size), not thermo_count -- an
 * unconfigured trailing zone slot getting a harmless default bit costs
 * nothing and keeps this loop from needing to know which zones are "real". */
static void migrate_zones_cfg_v1_to_current(zones_cfg_t *cfg)
{
    cfg->version = ZONES_CFG_VERSION;
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        if (cfg->zones[i].thermo_mask == 0) {
            cfg->zones[i].thermo_mask = (uint8_t)(1u << i);
        }
    }
    /* 4 -> 5 (2026-08-21, TODO.md owner-report item 1): every blob older than
     * version 5 predates zone_cfg_t::tc_type / zones_cfg_t::safety_tc_type
     * entirely, so both fields arrived here zeroed by nvs_get_blob() reading
     * into a zeroed out_cfg (see nvs_load_from()). Unlike thermo_mask above,
     * 0 is NOT a safe "not configured" reading for either field -- 0 decodes
     * as THERMO_TC_B, a real and different thermocouple type, not an
     * "unset" sentinel. Every board that has ever saved a zones config was
     * running with THERMO_TC_K in the actual hardware register the whole
     * time (MAX31856.c's now-fixed hardcoded default), so filling in
     * THERMO_TC_K here for every channel -- and for the safety processor's
     * own setting -- is what keeps a migrated board's thermocouples reading
     * the same temperatures after this upgrade as they did before it, rather
     * than silently relinearizing every reading against the wrong type the
     * moment this firmware boots. An operator who deliberately wants
     * something else still has to say so explicitly on the page, same as
     * any first-time use of a brand-new field. */
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        cfg->zones[i].tc_type = THERMO_TC_K;
    }
    cfg->safety_tc_type = THERMO_TC_K;
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
    if (!blob || len < sizeof(((zones_cfg_t *)0)->version)) {
        if (reason_out && reason_cap) {
            snprintf(reason_out, reason_cap, "blob missing or too short to contain a version");
        }
        return false;
    }

    zones_cfg_t cand;
    memset(&cand, 0, sizeof(cand));
    memcpy(&cand, blob, len < sizeof(cand) ? len : sizeof(cand));

    /* Same three-outcome version handling as nvs_load_from() -- see that
     * function's comment. current / older-migrated / newer-REFUSED, no
     * guessing at a layout this build doesn't know. */
    if (cand.version == ZONES_CFG_VERSION) {
        if (len != sizeof(cand)) {
            if (reason_out && reason_cap) {
                snprintf(reason_out, reason_cap,
                         "current-version config is the wrong size -- treating as corrupt");
            }
            return false;
        }
    } else if (cand.version < ZONES_CFG_VERSION) {
        migrate_zones_cfg_v1_to_current(&cand);
    } else {
        if (reason_out && reason_cap) {
            snprintf(reason_out, reason_cap,
                     "this config was saved by newer firmware (zones layout v%u, this build "
                     "knows v%u) -- refusing rather than guessing",
                     (unsigned)cand.version, (unsigned)ZONES_CFG_VERSION);
        }
        return false;
    }

    const char *err_reason = "invalid stored config";
    if (!validate_zones_cfg(&cand, &err_reason)) {
        if (reason_out && reason_cap) {
            snprintf(reason_out, reason_cap, "%s", err_reason);
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

static esp_err_t zones_get_handler(httpd_req_t *req)
{
    char json[2688]; /* 1024 -> 1536 with heater_window_ms/min_on_ms/min_off_ms,
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
            goto send;                                                                            \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    APPEND("{\"thermo_count\":%u,\"relay_count\":%u,\"max_simultaneous_relays\":%u,"
           "\"continue_on_zone_trip\":%s,\"safety_tc_type\":%u,\"zones\":[",
           s_zones.cfg.thermo_count, s_zones.cfg.relay_count, s_zones.cfg.max_simultaneous_relays,
           s_zones.cfg.continue_on_zone_trip ? "true" : "false", s_zones.cfg.safety_tc_type);
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        const zone_cfg_t *z = &s_zones.cfg.zones[i];
        char name_escaped[ZONE_NAME_MAX_LEN * 2 + 1];
        json_escape(z->name, name_escaped, sizeof(name_escaped));
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
            "\"tc_type\":%u}",
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
            (double)z->model_tau_s, (double)z->model_dead_time_s, z->tc_type);
    }
    APPEND("]}");

#undef APPEND

send:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
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
                              const zone_cfg_t *current_z, zone_cfg_t *z, const char **err_reason)
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

static esp_err_t zones_post_handler(httpd_req_t *req)
{
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

    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        const char *err_reason = "invalid zone field";
        /* &s_zones.cfg.zones[i]: the LIVE value, for z%u_tctype's
         * omit-means-preserve fallback (see parse_zone_fields()'s comment) --
         * tmp itself is zeroed, so tmp.zones[i] can't supply "what this
         * channel is already set to." */
        if (!parse_zone_fields(body, i, tmp.thermo_count, tmp.relay_count, &s_zones.cfg.zones[i],
                               &tmp.zones[i], &err_reason)) {
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

    /* Commit point: every rejection above returned before touching s_zones,
     * so this is the first and only line at which the submission becomes the
     * live config -- and therefore the only place in this handler the
     * generation may advance. A 400'd submission changed nothing and must
     * not make a running profile re-read identical settings (TODO.md
     * 6A.7). */
    s_zones.cfg = tmp;
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
    return httpd_resp_sendstr(req, "ok");
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
    static const httpd_uri_t get_uri = {
        .uri = "/api/zones", .method = HTTP_GET, .handler = zones_get_handler,
    };
    static const httpd_uri_t post_uri = {
        .uri = "/api/zones", .method = HTTP_POST, .handler = zones_post_handler,
    };
    err = httpd_register_uri_handler(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/settings/zones) failed: %s", esp_err_to_name(err));
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

    ESP_LOGI(TAG, "zones API up (thermo_count=%u, relay_count=%u)", s_zones.cfg.thermo_count,
             s_zones.cfg.relay_count);
    return ESP_OK;
}
