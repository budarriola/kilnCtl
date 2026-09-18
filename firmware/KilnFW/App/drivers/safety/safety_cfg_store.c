#include "safety_cfg_store.h"

#include <math.h> /* isfinite() -- safety_ct_cal_convert() */
#include <stdbool.h>
#include <stdlib.h> /* malloc()/free() -- safety_cfg_store_refetch_locked()'s heap scratch, see its comment */
#include <string.h>

#include "esp_log.h"
#include "hal_time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h" /* s_store_lock -- 2026-08-27 audit fix (H5), see its own comment */

#include "hal_kv.h"
#include "nvs_key_check.h"
#include "hal_esp_common.h" /* hal_status_to_esp_err() -- preserve the specific esp_err_t this
                              * file's callers already branch on */

#include "kilnlink/kilnlink_config_page.h"

/* uart_bridge_ext.c's existing internal-SRAM-stack "flash-safe executor"
 * (bx_flash_worker) -- see this file's nvs_save_store()/safety_cfg_store_
 * flush_if_dirty() comments below for why this driver's own NVS write is
 * routed through it rather than executed directly on whatever task calls
 * safety_cfg_store_refetch(). Same mechanism, same precedent,
 * uart_bridge_ext.c:104-127's HAZARD block.
 *
 * Declared here by hand rather than via #include "uart_bridge.h": that
 * header pulls in ILI9488.h/screen_idle.h/kiln_io.h for its many OTHER
 * hardware bridge task declarations, none of which this file needs or wants
 * as a build dependency -- and which are not part of this file's host-test
 * stub surface (App/test/stubs/), so pulling them in broke test_safety_cfg_
 * store.c's direct #include of this .c file. The real declaration and its
 * full doc comment live in uart_bridge.h; this one must be kept in sync with
 * it by hand if that signature ever changes. */
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);

static const char *TAG = "safety_cfg_store";

/* Same partition/namespace convention as zones_http.c/kiln_cfg_store.c -- see
 * this file's header comment for the full "survives an ordinary reflash"
 * rationale. Key distinguishes this module's blob from the others already
 * sharing the "kiln_cfg" namespace (zones_cfg/kilncfgs/...). */
#define KILN_NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_SAFETY_CFG "safetycfg"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);

/* RELAY_LIFE_BUDGET.md -- the safety relay type, stored
 * separately from the SAFETY_CFG_PARAM_TABLE blob above (see
 * safety_cfg_store_get_safety_relay_type()'s doc comment for why: it is not
 * one of that table's Pico-fetched answers). Own key (<=15 chars, same NVS
 * key-length rule every other kiln_nvs consumer in this tree follows), own
 * version byte. */
#define NVS_KEY_SAFETY_RELAY "safetyrelay"
#define SAFETY_RELAY_TYPE_BLOB_VERSION 1u

/* CT_COMMISSIONING_PLAN.md step 1 -- the operator's A_fs/zero_mv inputs and
 * which mechanism last wrote them, per channel. Own key/version for the same
 * "not one of the Pico-fetched answers" reason the relay type above gets
 * one. "safetyctcal" is 11 characters, well inside the 15-char NVS key
 * limit -- checked below like every other literal this file/tree uses,
 * same discipline zones_config_store.c's NVS_KEY_LEN_CHECK documents. */
#define NVS_KEY_SAFETY_CT_CAL "safetyctcal"
#define SAFETY_CT_CAL_BLOB_VERSION 1u

/* S8 rate-guard write provenance (docs/audits/s8_auto_calc_design_2026-09-
 * 09.md "Part 3") -- own key/version, same "not one of the Pico-fetched
 * answers" reasoning as the two blobs above. "safetyrateg" is 11
 * characters, inside the 15-char NVS key limit, checked below like the
 * others. */
#define NVS_KEY_SAFETY_RATE_GUARD "safetyrateg"
#define SAFETY_RATE_GUARD_META_BLOB_VERSION 1u

/* Compile-time guard -- macro now shared via nvs_key_check.h (see that
 * header) so every module with NVS key literals gets the identical check;
 * this file used to define NVS_KEY_LEN_CHECK locally. */
NVS_KEY_LEN_CHECK(NVS_KEY_SAFETY_CFG);
NVS_KEY_LEN_CHECK(NVS_KEY_SAFETY_RELAY);
NVS_KEY_LEN_CHECK(NVS_KEY_SAFETY_CT_CAL);
NVS_KEY_LEN_CHECK(NVS_KEY_SAFETY_RATE_GUARD);

typedef struct {
    uint8_t version;
    uint8_t type; /* relay_type_t */
} safety_relay_type_blob_t;

static relay_type_t s_safety_relay_type = RELAY_TYPE_CONTACTOR;

/* CT_COMMISSIONING_PLAN.md step 1 -- per-channel calibration input state. */
typedef struct {
    uint8_t has_value; /* 0/1 -- never set means "unset", not a real 0 A_fs */
    uint8_t source;    /* safety_ct_cal_source_t */
    float a_fs;
    float zero_mv;
} safety_ct_cal_entry_t;

typedef struct {
    uint8_t version;
    safety_ct_cal_entry_t ch[SAFETY_CT_CAL_CHANNELS];
} safety_ct_cal_blob_t;

static safety_ct_cal_blob_t s_ct_cal;

/* S8 rate-guard write provenance -- who last wrote max_rate_c_per_min
 * (0x0204) and what value they wrote, per safety_cfg_store.h's own doc
 * comment on safety_rate_guard_source_t. */
typedef struct {
    uint8_t version;
    uint8_t has_value; /* 0/1 -- never recorded (or explicitly cleared) means "unknown provenance" */
    uint8_t source;    /* safety_rate_guard_source_t */
    float value;
} safety_rate_guard_meta_blob_t;

static safety_rate_guard_meta_blob_t s_rate_guard_meta;

/* Bump whenever safety_cfg_store_blob_t's on-flash layout changes -- mirrors
 * ZONES_CFG_VERSION/KILN_CFG_STORE_VERSION's role in their own files.
 *
 * SAFETY_CFG_PARAM_TABLE's ROW ORDER is part of that layout, not merely a
 * display order: entries[] is indexed by table position, so inserting a row
 * anywhere but the very end shifts every row after it and silently remaps
 * each already-persisted value onto the WRONG field on the next load -- the
 * exact hazard that table's own header comment warns about.
 *
 * 1 -> 2 (2026-08-29): commit babbfdd inserted ct_installed (0x0109) at the
 * end of sec 1, i.e. in the MIDDLE of the array, without bumping this. Every
 * v1 blob on flash therefore describes sec 2-5 (firing_margin_c onward) one
 * slot below where this build's table looks for it.
 *
 * BE PRECISE ABOUT WHAT SAVED US: that same commit also took
 * SAFETY_CFG_PARAM_COUNT 64 -> 65, so a v1 blob is a different SIZE, and
 * nvs_load_store()'s `len != sizeof(loaded)` check already refused it as
 * unreadable -- no board has actually served remapped values. That is luck,
 * not design: an insertion that REPLACED a row (count unchanged) would have
 * the identical remap with a byte-identical size and would sail straight
 * through. This bump makes the refusal principled rather than incidental --
 * nvs_load_store()'s "older version, no migration path" branch now rejects
 * v1 for the actual reason it is unusable, and the next
 * safety_cfg_store_refetch() refills the empty cache from the Pico, which is
 * the authority on every one of these values anyway.
 *
 * 4 -> 5 (2026-09-08, E-stop pass): estop_active_level (0x0212) appended at
 * the END of the table, same tail-append discipline as 3 -> 4 -- bumped for
 * the same principled reason, not left to the size check.
 *
 * 2 -> 3 (2026-09-06, CT_COMMISSIONING_PLAN.md step 3): ct_topology (0x031F)
 * appended at the END of the table (not mid-array, so no remap hazard this
 * time) -- bumped anyway, principled rather than relying again on the size
 * check alone to save an unbumped version, per this comment's own "the
 * version gets bumped" rule for every table growth.
 *
 * 3 -> 4 (2026-09-08, owner request): tc_offset_c (0x010A) appended at the
 * END of the table, same no-remap-hazard shape as the 2 -> 3 bump.
 *
 * 5 -> 6 (2026-09-18, docs/CT_CHANNEL_MASK_PLAN.md step 2): zone_ct_channel
 * [0..2] (0x0320-0x0322) appended at the END of the table, same tail-append
 * discipline as every bump above. */
#define SAFETY_CFG_STORE_VERSION 6u

/* CONFIG_REFERENCE.md secs 1-5 / COMMISSIONING.md sec 2.1's param_id table,
 * in that document's own order -- table POSITION is what
 * safety_cfg_store_get_by_index() iterates and what the persisted blob's
 * entries[] array is indexed by, so this order must never be reshuffled
 * (only ever appended to) once any board has saved a cache against it: doing
 * so would silently remap every already-cached value to the WRONG field on
 * next load. "Only ever appended to" means appended to the END OF THE ARRAY,
 * not to the end of a SECTION: adding an id at the tail of sec 1 to keep the
 * numeric grouping tidy is still a mid-array insert, and is exactly how
 * SAFETY_CFG_STORE_VERSION came to need its 1 -> 2 bump (see that constant).
 * If a new id must be grouped with an existing section for readability, it
 * goes at the bottom of this table anyway and the version gets bumped. Ids
 * themselves are the permanent identity on the wire
 * (COMMISSIONING.md sec 2.1: "ids are permanent"); this table's row order is
 * this cache's OWN, separate permanence rule for the identical reason. */
typedef struct {
    uint16_t id;
    uint8_t type; /* KILNLINK_PARAM_TYPE_* */
    const char *name;
} safety_cfg_table_row_t;

static const safety_cfg_table_row_t SAFETY_CFG_PARAM_TABLE[] = {
    /* sec 1 -- commissioning, no compiled-in default */
    { 0x0101, KILNLINK_PARAM_TYPE_U8, "tc_source" },
    { 0x0102, KILNLINK_PARAM_TYPE_U8, "borrowed_zone_index" },
    { 0x0103, KILNLINK_PARAM_TYPE_U8, "tc_placement_mode" },
    { 0x0104, KILNLINK_PARAM_TYPE_F32, "abs_max_temp_c" },
    { 0x0105, KILNLINK_PARAM_TYPE_U8, "tc_type" },
    { 0x0106, KILNLINK_PARAM_TYPE_U8, "ct_channel_map[0]" },
    { 0x0107, KILNLINK_PARAM_TYPE_U8, "ct_channel_map[1]" },
    { 0x0108, KILNLINK_PARAM_TYPE_U8, "ct_channel_map[2]" },
    { 0x0109, KILNLINK_PARAM_TYPE_U8, "ct_installed" },
    /* sec 2 -- temperature guards */
    { 0x0201, KILNLINK_PARAM_TYPE_F32, "firing_margin_c" },
    { 0x0202, KILNLINK_PARAM_TYPE_F32, "overshoot_margin_c" },
    { 0x0203, KILNLINK_PARAM_TYPE_U16, "overshoot_time_s" },
    { 0x0204, KILNLINK_PARAM_TYPE_F32, "max_rate_c_per_min" },
    { 0x0205, KILNLINK_PARAM_TYPE_U16, "rate_window_s" },
    { 0x0206, KILNLINK_PARAM_TYPE_U16, "blind_grace_s" },
    { 0x0207, KILNLINK_PARAM_TYPE_U16, "frozen_window_s" },
    { 0x0208, KILNLINK_PARAM_TYPE_F32, "tc_disagreement_c" },
    { 0x0209, KILNLINK_PARAM_TYPE_U16, "tc_disagreement_time_s" },
    { 0x020A, KILNLINK_PARAM_TYPE_F32, "tc_expected_offset_c" },
    { 0x020B, KILNLINK_PARAM_TYPE_F32, "cj_warn_c" },
    { 0x020C, KILNLINK_PARAM_TYPE_F32, "cj_max_c" },
    { 0x020D, KILNLINK_PARAM_TYPE_U16, "cj_time_s" },
    { 0x020E, KILNLINK_PARAM_TYPE_U16, "borrowed_stale_s" },
    { 0x020F, KILNLINK_PARAM_TYPE_U16, "borrowed_stale_trip_s" },
    { 0x0210, KILNLINK_PARAM_TYPE_U8, "borrowed_type_expected" },
    /* 2026-08-23. Not a CONFIG_REFERENCE.md sec 2 guard threshold like the
     * ids above it -- a declaration about the BENCH: "the Pico's own safety
     * thermocouple is not physically wired up". 1 (installed) is the safe
     * default and what every Pico reports until an operator says otherwise;
     * 0 makes S5 stop promoting a permanent bad-read streak to a TRIP and
     * makes the Pico refuse every heating-enable request outright. Listed
     * here so /api/safety-cfg can show and set it like any other param --
     * the Pico is the authority on the value, this table only names it. */
    { 0x0211, KILNLINK_PARAM_TYPE_U8, "safety_tc_installed" },
    /* sec 3 -- current channels */
    { 0x0301, KILNLINK_PARAM_TYPE_F32, "i_present_a" },
    { 0x0302, KILNLINK_PARAM_TYPE_U16, "zero_counts[0]" },
    { 0x0303, KILNLINK_PARAM_TYPE_U16, "zero_counts[1]" },
    { 0x0304, KILNLINK_PARAM_TYPE_U16, "zero_counts[2]" },
    { 0x0305, KILNLINK_PARAM_TYPE_U16, "correlation_window_s" },
    { 0x0306, KILNLINK_PARAM_TYPE_U16, "stuck_on_time_s" },
    { 0x0307, KILNLINK_PARAM_TYPE_U16, "trip_verify_s" },
    { 0x0308, KILNLINK_PARAM_TYPE_F32, "k_ct_v_per_a[0]" },
    { 0x0309, KILNLINK_PARAM_TYPE_F32, "k_ct_v_per_a[1]" },
    { 0x030A, KILNLINK_PARAM_TYPE_F32, "k_ct_v_per_a[2]" },
    { 0x030B, KILNLINK_PARAM_TYPE_F32, "gain[0]" },
    { 0x030C, KILNLINK_PARAM_TYPE_F32, "gain[1]" },
    { 0x030D, KILNLINK_PARAM_TYPE_F32, "gain[2]" },
    { 0x030E, KILNLINK_PARAM_TYPE_F32, "mains_voltage_v" },
    { 0x030F, KILNLINK_PARAM_TYPE_U16, "power_window_s" },
    { 0x0310, KILNLINK_PARAM_TYPE_F32, "ct_cal[0].gain" },
    { 0x0311, KILNLINK_PARAM_TYPE_F32, "ct_cal[1].gain" },
    { 0x0312, KILNLINK_PARAM_TYPE_F32, "ct_cal[2].gain" },
    { 0x0313, KILNLINK_PARAM_TYPE_F32, "ct_cal[0].offset" },
    { 0x0314, KILNLINK_PARAM_TYPE_F32, "ct_cal[1].offset" },
    { 0x0315, KILNLINK_PARAM_TYPE_F32, "ct_cal[2].offset" },
    { 0x0316, KILNLINK_PARAM_TYPE_BOOL, "ct_cal[0].calibrated" },
    { 0x0317, KILNLINK_PARAM_TYPE_BOOL, "ct_cal[1].calibrated" },
    { 0x0318, KILNLINK_PARAM_TYPE_BOOL, "ct_cal[2].calibrated" },
    { 0x0319, KILNLINK_PARAM_TYPE_F32, "max_expected_power_w" },
    /* S14 over-current guard, NEW -- COMMISSIONING_UX.md sec 3.3 */
    { 0x031A, KILNLINK_PARAM_TYPE_F32, "i_normal_a[0]" },
    { 0x031B, KILNLINK_PARAM_TYPE_F32, "i_normal_a[1]" },
    { 0x031C, KILNLINK_PARAM_TYPE_F32, "i_normal_a[2]" },
    { 0x031D, KILNLINK_PARAM_TYPE_U16, "overcurrent_pct" },
    { 0x031E, KILNLINK_PARAM_TYPE_U16, "overcurrent_time_s" },
    /* sec 4 -- link and liveness */
    { 0x0401, KILNLINK_PARAM_TYPE_U16, "context_max_age_s" },
    { 0x0402, KILNLINK_PARAM_TYPE_U16, "link_timeout_s" },
    { 0x0403, KILNLINK_PARAM_TYPE_U16, "link_dead_hard_s" },
    { 0x0404, KILNLINK_PARAM_TYPE_U16, "mainfault_debounce_ms" },
    { 0x0405, KILNLINK_PARAM_TYPE_U16, "telemetry_period_ms" },
    /* sec 5 -- timing and system */
    { 0x0501, KILNLINK_PARAM_TYPE_U16, "startup_grace_s" },
    { 0x0502, KILNLINK_PARAM_TYPE_U16, "estop_debounce_ms" },
    { 0x0503, KILNLINK_PARAM_TYPE_U16, "watchdog_timeout_ms" },
    { 0x0504, KILNLINK_PARAM_TYPE_U16, "config_check_period_s" },
    /* CT_COMMISSIONING_PLAN.md step 3 -- appended at the very end (never
     * mid-array, see this table's own header comment), so no version bump
     * is needed beyond the one SAFETY_CFG_PARAM_COUNT's own growth already
     * requires. 0 = per_zone (safe default, matches every board before this
     * field existed). */
    { 0x031F, KILNLINK_PARAM_TYPE_U8, "ct_topology" },
    /* 0x010A tc_offset_c -- owner request 2026-09-08. Appended at the END of
     * the table, not re-grouped next to tc_type (0x0105) in sec 1, per this
     * table's own "append to the end of the array" rule above -- 3 -> 4
     * bump. */
    { 0x010A, KILNLINK_PARAM_TYPE_F32, "tc_offset_c" },
    /* 0x0212 estop_active_level -- E-stop input polarity, owner decision
     * 2026-09-08. Appended at the very END of the table (NOT next to
     * safety_tc_installed/0x0211 in sec 2, where it belongs by subject
     * matter) for exactly the reason this table's header comment gives: any
     * insertion but a tail-append shifts every later row and silently
     * remaps each already-persisted value onto the wrong field. 4 -> 5 bump.
     * 0 = ACTIVE_HIGH: asserted when the Pico's GPIO9 reads high -- the
     * default, this bench's real wiring, and the only polarity under which
     * a broken E-stop line is detectable (R10's pull-up floats a cut line
     * high, which reads as STOP). 1 = ACTIVE_LOW, for a differently-wired
     * installation, which cannot see a broken line. The Pico is the
     * authority on the value; this table only names it. */
    { 0x0212, KILNLINK_PARAM_TYPE_U8, "estop_active_level" },
    /* 0x0320-0x0322 zone_ct_channel[0..2] -- docs/CT_CHANNEL_MASK_PLAN.md
     * step 2. Which physical CT channel (0-2) each zone's current is read
     * on, superseding the kiln-wide ct_topology enum (0x031F, which new
     * firmware keeps writing as a derived label so an older board still
     * reads something meaningful). Appended at the very END of the table,
     * never next to 0x031F, for the reason this table's header comment
     * gives: any insertion but a tail-append shifts every later row and
     * silently remaps each already-persisted value onto the wrong field.
     * 5 -> 6 bump. The Pico is the authority on the values; this table only
     * names them so the generic commissioning endpoint can write them. */
    { 0x0320, KILNLINK_PARAM_TYPE_U8, "zone_ct_channel[0]" },
    { 0x0321, KILNLINK_PARAM_TYPE_U8, "zone_ct_channel[1]" },
    { 0x0322, KILNLINK_PARAM_TYPE_U8, "zone_ct_channel[2]" },
};

// SAFETY_CFG_PARAM_COUNT (safety_cfg_store.h) is a hand-maintained literal,
// not derived from this table -- it also sizes safety_cfg_store_blob_t's
// on-flash `entries[]` array below and SAFETY_CFG_POST_MAX_PAIRS
// (safety_cfg_write.h). Declaring the table above as `[]` (rather than
// `[SAFETY_CFG_PARAM_COUNT]`) makes the compiler count its own initializers
// so this assert can catch a real divergence: if a row is ever added
// without bumping SAFETY_CFG_PARAM_COUNT, or the macro is bumped without a
// matching row appended, an explicit `[SAFETY_CFG_PARAM_COUNT]` array size
// would silently accept either mistake -- too many initializers is a
// compile error, but too FEW is not: the remaining slots just zero-fill,
// producing a bogus id-0/type-0 row that this cache's lookups
// (SAFETY_CFG_PARAM_TABLE[i].id, safety_cfg_get_row_at()) would then read
// back as real. This is the same "count and table drift" shape as the enum
// count that once silently grew an unintended vote (CLAUDE.md's "An enum
// count granted a vote" note) -- catch it here at compile time instead.
_Static_assert((sizeof(SAFETY_CFG_PARAM_TABLE) / sizeof(SAFETY_CFG_PARAM_TABLE[0])) == SAFETY_CFG_PARAM_COUNT,
               "SAFETY_CFG_PARAM_TABLE's row count must equal SAFETY_CFG_PARAM_COUNT "
               "(safety_cfg_store.h) -- update the macro whenever a row is appended or "
               "removed, or every dependent buffer (safety_cfg_store_blob_t.entries[], "
               "SAFETY_CFG_POST_MAX_PAIRS) silently mis-sizes against the real table");

typedef struct {
    uint8_t set;
    kilnlink_param_value_t value;
} safety_cfg_entry_t;

typedef struct {
    uint8_t version;
    uint16_t config_crc; /* 0 = never fetched */
    safety_cfg_entry_t entries[SAFETY_CFG_PARAM_COUNT];
} safety_cfg_store_blob_t;

/* Field-by-field diff of the last successful refetch -- see safety_cfg_store.h's
 * safety_cfg_diff_entry_t comment. Transient, RAM-only (never persisted --
 * it describes an event, not state): computed fresh in
 * safety_cfg_store_refetch_locked() every time it is about to install a new
 * s_store, discarded (count reset to 0) on the very next refetch regardless
 * of outcome, so a caller always sees either "the diff for the refetch that
 * just happened" or "empty, nothing has refetched since I last looked". */
static safety_cfg_diff_entry_t s_diff[SAFETY_CFG_STORE_DIFF_MAX];
static size_t s_diff_count;
static bool s_diff_truncated;
static uint16_t s_diff_from_crc;
static uint16_t s_diff_to_crc;

/* NOT a separate function on purpose: safety_poll_task's own stack is
 * documented near its ceiling (check_all_task_stack_budgets.ps1), and every
 * function CALL on Xtensa's windowed ABI costs a register-window spill
 * whether or not the callee has any locals of its own. This logic used to be
 * its own safety_cfg_compute_diff()/safety_cfg_diff_value_equal() pair,
 * called from refetch_locked() below; that put safety_poll_task 16 B over
 * budget (3120 vs. 3104) for no reason -- refetch_locked() already exists on
 * that same call path with its own frame, so folding the loop and the
 * bit-exact comparison directly into its body (see the call site below,
 * "safety_cfg_store_refetch_locked's own inline diff") adds ZERO new call
 * frames, only a few bytes of additional locals inside a frame that already
 * exists. See that call site for the actual loop. */

size_t safety_cfg_store_diff_count(void)
{
    return s_diff_count;
}

bool safety_cfg_store_get_diff(size_t index, safety_cfg_diff_entry_t *out)
{
    if (!out || index >= s_diff_count) {
        return false;
    }
    *out = s_diff[index];
    return true;
}

bool safety_cfg_store_diff_truncated(void)
{
    return s_diff_truncated;
}

void safety_cfg_store_get_diff_crc_range(uint16_t *out_from_crc, uint16_t *out_to_crc)
{
    if (out_from_crc) {
        *out_from_crc = s_diff_from_crc;
    }
    if (out_to_crc) {
        *out_to_crc = s_diff_to_crc;
    }
}

static safety_cfg_store_blob_t s_store;
/* Set ONLY on a successful safety_cfg_store_refetch() -- a real, live round
 * trip to the Pico. 2026-08-27 audit fix (defect c): safety_cfg_store_init()
 * used to also stamp this on an ordinary NVS load ("a fresh load counts as
 * just fetched"), which made safety_cfg_store_fetched_ms_ago() report board
 * uptime instead of fetch age for any board that boots with a cache already
 * on flash -- see that function's own init()-time note. safety_cfg_store_
 * fetched_ms_ago() measures against this, never against anything persisted
 * (a wall-clock timestamp would need a synced RTC this board does not have;
 * hal_time_now_us()'s monotonic microsecond counter needs none) -- which
 * is exactly why an NVS load, with no live round trip behind it, has nothing
 * honest to stamp here. */
static int64_t s_fetched_at_us = -1;

/* Rate-limits the "page N failed" warning below -- same 5 s cadence and
 * "one line, with a suppressed-count, not silence" discipline
 * uart_protocol.c's RETRY_LOG_INTERVAL_US already uses for its own
 * no-reply-from-peer warning. safety_cfg_store_maybe_refetch() is called
 * once per safety poll (SAFETY_POLL_PERIOD_MS, a few hundred ms) and keeps
 * retrying for as long as the cached CRC disagrees with the peer's -- by
 * design (a Pico that only just came back up should not need a reboot to be
 * noticed) -- but on a link this bandwidth-constrained a genuinely stuck
 * fetch must not cost a log line every single poll forever; it still must
 * never go silent, since a real, persistent failure is exactly what an
 * operator needs to see. */
#define SAFETY_CFG_STORE_REFETCH_LOG_INTERVAL_US 5000000

/* Settle time between one page request and the next -- see the call site in
 * safety_cfg_store_refetch() for the measurement this comes from. */
#define SAFETY_CFG_STORE_INTER_PAGE_GAP_MS 40u
static int64_t s_last_refetch_fail_log_us = 0;
static uint32_t s_refetch_fail_suppressed = 0;

/* Wall-clock ceiling on how long ONE safety_cfg_store_refetch() call may
 * spend across ALL of its page requests combined -- 2026-08-23 fix.
 *
 * This function runs synchronously inside safety_poll_task, and each page it
 * fetches (safety_link_get_config_page()) can legitimately take up to
 * SAFETY_LINK_REPLY_TIMEOUT_MS -- ~1.2s back when this link was capped at
 * 9600 baud by its optocouplers; that ceiling was a property of the (now
 * removed) optocouplers, not of this timeout, so the real worst case scales
 * with whatever KILNCTL_SAFETY_BAUD_RATE is currently set to (see
 * KilnFW/App/drivers/Kconfig) and only gets smaller as the baud goes up --
 * now that that call actually uses its full declared wait instead of
 * abandoning it after the first unrelated frame (safety_link.h's
 * safety_drain_still_waiting(), same date). Without a cap here, a
 * slow-but-answering Pico -- or one that has genuinely gone quiet mid-fetch
 * -- lets a multi-page refetch (CONFIG_REFERENCE.md's ~57 params packs into
 * roughly 2-3 CONFIG_PAGE frames at typical entry sizes) spend several of
 * those REPLY_TIMEOUT budgets back to back, all inside ONE safety_poll_task
 * iteration, on top of that same iteration's own GET_STATUS exchange (which
 * can itself already legitimately spend up to SAFETY_LINK_REPLY_TIMEOUT_MS
 * against a dead link -- that part is unchanged and pre-existing).
 *
 * 2000 ms here, combined with GET_STATUS's own pre-existing ~1.2s worst case
 * (at the old 9600 ceiling -- smaller now) and FW_VERSION's ~50-500ms rare
 * worst case, kept one safety_poll_task iteration's own worst case around
 * 3.2-3.7s at 9600 baud -- comfortably under CONFIG_ESP_TASK_WDT_TIMEOUT_S
 * (5s, sdkconfig.defaults) with well over a second of margin for scheduling
 * jitter and the rest of that iteration's work; a higher current baud only
 * widens that margin. If this budget is exhausted mid-fetch, this function
 * aborts exactly
 * like a failed page request (cache left unchanged, logged, rate-limited) --
 * nothing is lost: safety_cfg_store_maybe_refetch() re-invokes this from
 * page 0 with a FRESH budget on the next poll for as long as the cached CRC
 * keeps disagreeing, so a multi-page fetch simply spreads itself across
 * however many poll iterations it needs instead of trying to fit inside
 * one. */
#define SAFETY_CFG_STORE_REFETCH_BUDGET_MS 2000u

static esp_err_t nvs_partition_init(const char *partition)
{
    return hal_status_to_esp_err(hal_kv_init_partition(partition));
}

static void reset_to_defaults(void)
{
    memset(&s_store, 0, sizeof(s_store));
    s_store.version = SAFETY_CFG_STORE_VERSION;
    s_store.config_crc = 0; /* never fetched */
}

/* Three-outcome load, same discipline zones_http.c's nvs_load_from() and
 * kiln_cfg_store.c's nvs_load_store() document -- current/migrate/refuse.
 * SAFETY_CFG_STORE_VERSION==1 is the only version this build has ever
 * written, so there is no migration chain yet (mirrors kiln_cfg_store.c's own
 * "only version 1 exists today" state); a future bump needs one here, same as
 * those two files. */
static void nvs_load_store(void)
{
    reset_to_defaults();

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return; /* HAL_NOT_FOUND (never saved) or partition trouble -- defaults stand */
    }

    safety_cfg_store_blob_t loaded;
    size_t len = sizeof(loaded);
    err = hal_kv_get_blob(&h, NVS_KEY_SAFETY_CFG, &loaded, &len);
    hal_kv_close(&h);
    if (err != HAL_OK) {
        return; /* nothing stored, or unreadable -- defaults stand */
    }
    if (len < sizeof(loaded.version)) {
        ESP_LOGW(TAG, "safety_cfg_store blob is too short to contain a version -- treating as unreadable");
        return;
    }
    if (loaded.version == SAFETY_CFG_STORE_VERSION) {
        if (len != sizeof(loaded)) {
            ESP_LOGW(TAG, "safety_cfg_store blob claims current version but is the wrong size -- "
                          "treating as unreadable");
            return;
        }
        s_store = loaded;
        return;
    }
    if (loaded.version < SAFETY_CFG_STORE_VERSION) {
        /* No migration chain exists yet -- unreachable until a future bump
         * adds one (see this function's header comment). Fail safe rather
         * than guess at a layout this build has never written: reset stands,
         * same choice kiln_cfg_store.c's nvs_load_store() makes for "wrong
         * size or an unknown version". */
        ESP_LOGW(TAG, "safety_cfg_store blob is version %u with no migration path to %u -- "
                      "resetting to an empty cache",
                 (unsigned)loaded.version, (unsigned)SAFETY_CFG_STORE_VERSION);
        return;
    }
    /* loaded.version > SAFETY_CFG_STORE_VERSION: firmware-rollback case, same
     * as zones_http.c/kiln_cfg_store.c -- refuse to load, flash left
     * untouched, this boot runs with an empty cache. Every param reads
     * set=false until the next successful refetch, which is a strictly safer
     * failure than reinterpreting a newer layout's byte offsets as this
     * version's fields (COMMISSIONING.md sec 1's identical reasoning for the
     * Pico's own config_store). */
    ESP_LOGW(TAG, "safety_cfg_store blob is version %u, newer than this firmware's %u -- "
                  "refusing to load, flash data left untouched",
             (unsigned)loaded.version, (unsigned)SAFETY_CFG_STORE_VERSION);
}

/* True iff the CURRENTLY EXECUTING task's own stack lives in external RAM
 * (PSRAM) -- a local variable's address is as good a proxy as any for "where
 * is my stack", since it is allocated on whichever stack the calling task is
 * currently running on, by construction.
 *
 * 2026-08-23 panic fix, ESP-IDF's constraint, not this driver's own: a
 * flash/NVS write disables the cache (spi_flash_disable_interrupts_caches_
 * and_other_cpu(), on the esp_flash_write()/esp_partition_read() path NVS
 * uses underneath), which makes PSRAM unreachable while it is disabled --
 * ESP-IDF's own esp_task_stack_is_sane_cache_disabled() (inlined at the top
 * of that function) asserts if the calling task's stack lives there,
 * ABORTING THE WHOLE BOARD rather than failing just this one call. This is
 * exactly the hazard uart_bridge_ext.c:104-127's HAZARD block documents for
 * the CONTROL/PROFILES/AUTOTUNE UART bridge tasks (also PSRAM-stacked, same
 * reason -- internal-DRAM exhaustion at boot), reproduced here for
 * safety_poll_task (safety_link.c:1636-1638's PSRAM stack comment): a
 * successful safety_cfg_store_refetch() calling this function directly, from
 * that task, panicked the board the first time the fetch ever actually
 * succeeded (previously it always timed out first, so this code path had
 * never executed). See nvs_save_store()'s own comment just below for the
 * fix (route the write through uart_bridge_ext.c's flash-safe worker
 * instead) -- this predicate is the belt to that fix's suspenders: even if a
 * FUTURE caller reaches nvs_save_store() directly from the wrong task
 * (bypassing safety_cfg_store_flush_if_dirty()), this refuses loudly with a
 * diagnosable error instead of aborting the board. */
static bool caller_stack_is_external(void)
{
    return !hal_kv_write_safe_here();
}

static esp_err_t nvs_save_store(void)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(TAG, "nvs_save_store: REFUSING -- calling task's stack is in external RAM "
                      "(PSRAM). A flash/NVS write from here would abort the whole board "
                      "(ESP-IDF's esp_task_stack_is_sane_cache_disabled(), not a constraint of "
                      "this driver -- see caller_stack_is_external()'s comment). Call this "
                      "through safety_cfg_store_flush_if_dirty() (which routes it via "
                      "uart_bridge_ext_run_on_flash_worker()) instead of directly.");
        return ESP_ERR_INVALID_STATE;
    }
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    s_store.version = SAFETY_CFG_STORE_VERSION;
    err = hal_kv_set_blob(&h, NVS_KEY_SAFETY_CFG, &s_store, sizeof(s_store));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
}

/* True once s_store has changed in RAM since it was last successfully
 * persisted to flash. Set by safety_cfg_store_refetch() right after it
 * updates s_store; cleared only by safety_cfg_store_flush_if_dirty() on a
 * successful flush.
 *
 * 2026-08-27 audit fix (H5): this used to claim "not locked: every writer of
 * s_store/s_dirty (safety_cfg_store_refetch(), always on safety_poll_task)
 * runs its own flush call to completion... there is no window where two
 * tasks touch either at once." That became FALSE the moment safety_cfg_
 * http.c's commissioning POST handler started calling safety_cfg_store_
 * refetch() itself (confirm_commit_landed(), to force a live read-back after
 * a commit) -- that runs on the httpd worker task, concurrently with
 * safety_poll_task's own safety_cfg_store_maybe_refetch() every ~500ms.
 * TWO tasks can now call safety_cfg_store_refetch() at once, racing
 * s_store/s_dirty/s_fetched_at_us and potentially submitting two concurrent
 * flush_if_dirty() jobs to the flash worker.
 *
 * Fixed by serializing WRITERS: s_store_lock (below) is taken for the whole
 * body of safety_cfg_store_refetch() (see the safety_cfg_store_refetch_
 * locked()/safety_cfg_store_refetch() split just below it), so at most one
 * task is ever inside the scratch-build/commit/flush sequence at a time,
 * regardless of which task called in. (Pre-existing note, still true and
 * still out of scope for this fix: s_store itself has no lock against
 * concurrent READERS on other tasks either -- e.g. safety_cfg_http.c's
 * commissioning JSON build -- that is an existing property of this file,
 * not introduced or worsened here.) */
static bool s_dirty = false;

/* 2026-08-27 audit fix (H5) -- see s_dirty's comment just above for the full
 * story. Created lazily (ensure_store_lock()) rather than only in
 * safety_cfg_store_init(): several host tests call safety_cfg_store_
 * refetch() directly without ever calling init() first (they seed s_store by
 * hand instead), and this must still be safe there. xSemaphoreTake()'s
 * return value is deliberately NOT checked -- same convention profile_
 * executor.c's s_exec.lock already uses (xSemaphoreTake(..., portMAX_DELAY)
 * blocks until it succeeds on real FreeRTOS; the host-test stub's always-
 * pdFALSE return is a fidelity gap in the STUB, not a real failure mode, and
 * every existing caller of a portMAX_DELAY take in this codebase already
 * ignores it for that reason). */
static SemaphoreHandle_t s_store_lock = NULL;

static void ensure_store_lock(void)
{
    if (!s_store_lock) {
        s_store_lock = xSemaphoreCreateMutex();
    }
}

/* The actual flash write, run ON bx_flash_worker's own internal-RAM stack
 * (see uart_bridge_ext_run_on_flash_worker()'s doc comment) rather than on
 * whoever calls safety_cfg_store_flush_if_dirty(). `arg` is the esp_err_t*
 * this job reports its result back through -- safe to point at the caller's
 * own stack local, since uart_bridge_ext_run_on_flash_worker() blocks the
 * caller for the whole call (same contract bx_run_on_internal_stack()
 * documents in uart_bridge_ext.c). */
static void nvs_save_store_job(void *arg)
{
    esp_err_t *out_err = (esp_err_t *)arg;
    *out_err = nvs_save_store();
}

/* Flushes s_store to NVS if (and only if) it has changed since the last
 * successful flush -- the "mark dirty, flush later on a safe task" half of
 * the 2026-08-23 panic fix. safety_cfg_store_refetch() is this function's
 * only caller today (right after marking s_store dirty), but it is exported
 * (safety_cfg_store.h) so a future caller on any task can trigger a flush
 * without ever risking a direct nvs_save_store() call of its own -- the
 * worker hand-off happens here, once, in one place. */
esp_err_t safety_cfg_store_flush_if_dirty(void)
{
    if (!s_dirty) {
        return ESP_OK; /* nothing to do -- already persisted */
    }
    esp_err_t save_err = ESP_FAIL;
    // Not reachable on-worker today; covered by bx_run_on_internal_stack()'s
    // generic backstop if that ever changes.
    esp_err_t submit_err = uart_bridge_ext_run_on_flash_worker(nvs_save_store_job, &save_err);
    if (submit_err != ESP_OK) {
        /* The worker isn't up yet (boot ordering -- app_main calls
         * uart_bridge_ext_start_flash_worker() early, but "early" is still
         * after safety_link_start(), so a refetch that races the very start
         * of boot could in principle see this) or its queue/lock could not
         * be used. Same "live but will not survive a reboot" outcome as an
         * NVS write failure below: s_dirty stays true, so the NEXT
         * successful refetch (or any other future caller of this function)
         * tries again. */
        ESP_LOGE(TAG, "safety_cfg_store: could not hand the NVS flush to the flash-safe worker "
                      "(%s) -- cache is live but will not persist this attempt",
                 esp_err_to_name(submit_err));
        return submit_err;
    }
    if (save_err == ESP_OK) {
        s_dirty = false;
    }
    return save_err;
}

/* Three-outcome load, same discipline as nvs_load_store() above: a missing
 * blob (first boot) or a version this build doesn't recognise falls back to
 * the RELAY_TYPE_CONTACTOR default rather than guessing at a layout; a
 * stored SSR value (should never happen -- the setter refuses it -- but a
 * hand-edited or pre-validation blob is not impossible) is also normalized
 * to the default rather than trusted, so this function can never hand back
 * an invalid type. */
static void load_safety_relay_type(void)
{
    s_safety_relay_type = RELAY_TYPE_CONTACTOR;

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return; /* never saved, or partition trouble -- default stands */
    }
    safety_relay_type_blob_t loaded;
    size_t len = sizeof(loaded);
    err = hal_kv_get_blob(&h, NVS_KEY_SAFETY_RELAY, &loaded, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(loaded)) {
        return; /* nothing stored, unreadable, or wrong size -- default stands */
    }
    if (loaded.version != SAFETY_RELAY_TYPE_BLOB_VERSION) {
        ESP_LOGW(TAG, "safety relay type blob is version %u, this build knows only %u -- "
                      "resetting to the default",
                 (unsigned)loaded.version, (unsigned)SAFETY_RELAY_TYPE_BLOB_VERSION);
        return;
    }
    if (loaded.type != (uint8_t)RELAY_TYPE_CONTACTOR && loaded.type != (uint8_t)RELAY_TYPE_MERCURY) {
        ESP_LOGW(TAG, "safety relay type blob holds an invalid type (%u) -- resetting to the default",
                 (unsigned)loaded.type);
        return;
    }
    s_safety_relay_type = (relay_type_t)loaded.type;
}

/* Direct write, no flash-worker indirection -- see this function's own doc
 * comment in safety_cfg_store.h for why that's safe here (httpd-worker-only
 * caller). */
static esp_err_t save_safety_relay_type(void)
{
    /* check_nvs_write_guard_coverage.ps1 requires every NVS write in a
     * PSRAM-stack-guarded module to carry the same caller_stack_is_external()
     * refusal nvs_save_store() above does -- this write is only ever reached
     * from the httpd worker's POST handler in practice (see this function's
     * own doc comment in safety_cfg_store.h), but a future caller reaching it
     * from safety_poll_task (PSRAM stack) would abort the board exactly like
     * a direct nvs_save_store() call would, so the belt-and-suspenders check
     * applies here too, not only to the Pico-param cache's own write path. */
    if (caller_stack_is_external()) {
        ESP_LOGE(TAG, "save_safety_relay_type: REFUSING -- calling task's stack is in external RAM "
                      "(PSRAM). A flash/NVS write from here would abort the whole board -- see "
                      "caller_stack_is_external()'s comment.");
        return ESP_ERR_INVALID_STATE;
    }
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    safety_relay_type_blob_t blob = {
        .version = SAFETY_RELAY_TYPE_BLOB_VERSION,
        .type = (uint8_t)s_safety_relay_type,
    };
    err = hal_kv_set_blob(&h, NVS_KEY_SAFETY_RELAY, &blob, sizeof(blob));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
}

relay_type_t safety_cfg_store_get_safety_relay_type(void)
{
    return s_safety_relay_type;
}

bool safety_cfg_store_set_safety_relay_type(relay_type_t type, esp_err_t *out_nvs_err)
{
    if (out_nvs_err) {
        *out_nvs_err = ESP_OK;
    }
    if (type != RELAY_TYPE_CONTACTOR && type != RELAY_TYPE_MERCURY) {
        /* Refuses RELAY_TYPE_SSR and any other value -- see this function's
         * doc comment. */
        return false;
    }
    s_safety_relay_type = type;
    relay_cycles_set_type(RELAY_CYCLES_SAFETY_INDEX, type, 0);
    esp_err_t err = save_safety_relay_type();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "safety_cfg_store_set_safety_relay_type: NVS write failed (%s) -- "
                      "type applied live but will not survive a reboot",
                 esp_err_to_name(err));
    }
    if (out_nvs_err) {
        *out_nvs_err = err;
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* CT_COMMISSIONING_PLAN.md step 1                                        */
/* ---------------------------------------------------------------------- */

static void reset_ct_cal_to_defaults(void)
{
    memset(&s_ct_cal, 0, sizeof(s_ct_cal));
    s_ct_cal.version = SAFETY_CT_CAL_BLOB_VERSION;
}

/* Same current/migrate(none-yet)/refuse discipline as load_safety_relay_
 * type() -- see that function's own comment for the reasoning, identical
 * here. */
static void load_ct_cal(void)
{
    reset_ct_cal_to_defaults();

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return; /* never saved, or partition trouble -- defaults stand */
    }
    safety_ct_cal_blob_t loaded;
    size_t len = sizeof(loaded);
    err = hal_kv_get_blob(&h, NVS_KEY_SAFETY_CT_CAL, &loaded, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(loaded)) {
        return; /* nothing stored, unreadable, or wrong size -- defaults stand */
    }
    if (loaded.version != SAFETY_CT_CAL_BLOB_VERSION) {
        ESP_LOGW(TAG, "safety CT calibration-input blob is version %u, this build knows only %u -- "
                      "resetting to defaults",
                 (unsigned)loaded.version, (unsigned)SAFETY_CT_CAL_BLOB_VERSION);
        return;
    }
    s_ct_cal = loaded;
}

/* Direct write, no flash-worker indirection -- same "httpd-worker-only
 * caller" reasoning as save_safety_relay_type()'s own comment; this is only
 * ever reached from the httpd worker's commissioning POST handler. */
static esp_err_t save_ct_cal(void)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(TAG, "save_ct_cal: REFUSING -- calling task's stack is in external RAM (PSRAM). "
                      "A flash/NVS write from here would abort the whole board -- see "
                      "caller_stack_is_external()'s comment.");
        return ESP_ERR_INVALID_STATE;
    }
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    s_ct_cal.version = SAFETY_CT_CAL_BLOB_VERSION;
    err = hal_kv_set_blob(&h, NVS_KEY_SAFETY_CT_CAL, &s_ct_cal, sizeof(s_ct_cal));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
}

/* SAFETY_CT_CAL_DEFAULT_GAIN (0.715, R46/R43 physical default) is declared in
 * safety_cfg_store.h now -- shared with safety_cfg_http.c's
 * ct_auto_zero_counts_to_mv() fallback, see that header comment. */

bool safety_ct_cal_convert(float a_fs, float zero_mv, float gain, float *out_k_ct_v_per_a,
                            uint16_t *out_zero_counts)
{
    if (!isfinite(a_fs) || a_fs < SAFETY_CT_CAL_A_FS_MIN || a_fs > SAFETY_CT_CAL_A_FS_MAX) {
        return false;
    }
    if (!isfinite(zero_mv) || zero_mv < SAFETY_CT_CAL_ZERO_MV_MIN || zero_mv > SAFETY_CT_CAL_ZERO_MV_MAX) {
        return false;
    }
    if (!isfinite(gain) || gain <= 0.0f) {
        return false;
    }
    float k = 1.0f / a_fs;
    if (!isfinite(k) || k <= 0.0f) {
        return false;
    }
    /* zero_counts = zero_mv/1000 * gain * 4096/3.3 -- CURRENT_SENSE.md's
     * model, CT_COMMISSIONING_PLAN.md step 1's own formula verbatim. A
     * negative zero_mv can drive this negative; the ADC itself cannot report
     * negative counts, so clamp the floor at 0 rather than let a cast to
     * uint16_t wrap a negative float into a huge unsigned value -- that wrap
     * is the actual failure this clamp exists to prevent, not a cosmetic
     * nicety. */
    float counts_f = (zero_mv / 1000.0f) * gain * (4096.0f / 3.3f);
    if (!isfinite(counts_f)) {
        return false;
    }
    if (counts_f < 0.0f) {
        counts_f = 0.0f;
    }
    if (counts_f > 65535.0f) {
        counts_f = 65535.0f; /* wire type is u16 -- never overflow it either */
    }
    if (out_k_ct_v_per_a) {
        *out_k_ct_v_per_a = k;
    }
    if (out_zero_counts) {
        *out_zero_counts = (uint16_t)(counts_f + 0.5f); /* round to nearest */
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* S8 rate-guard write provenance                                         */
/* ---------------------------------------------------------------------- */

static void reset_rate_guard_meta_to_defaults(void)
{
    memset(&s_rate_guard_meta, 0, sizeof(s_rate_guard_meta));
    s_rate_guard_meta.version = SAFETY_RATE_GUARD_META_BLOB_VERSION;
}

/* Same current/migrate(none-yet)/refuse discipline as load_ct_cal(). */
static void load_rate_guard_meta(void)
{
    reset_rate_guard_meta_to_defaults();

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return;
    }
    safety_rate_guard_meta_blob_t loaded;
    size_t len = sizeof(loaded);
    err = hal_kv_get_blob(&h, NVS_KEY_SAFETY_RATE_GUARD, &loaded, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(loaded)) {
        return;
    }
    if (loaded.version != SAFETY_RATE_GUARD_META_BLOB_VERSION) {
        ESP_LOGW(TAG, "safety rate-guard provenance blob is version %u, this build knows only %u -- "
                      "resetting to defaults",
                 (unsigned)loaded.version, (unsigned)SAFETY_RATE_GUARD_META_BLOB_VERSION);
        return;
    }
    s_rate_guard_meta = loaded;
}

/* Same "httpd-worker-only caller" discipline as save_ct_cal(). */
static esp_err_t save_rate_guard_meta(void)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(TAG, "save_rate_guard_meta: REFUSING -- calling task's stack is in external RAM "
                      "(PSRAM). A flash/NVS write from here would abort the whole board -- see "
                      "caller_stack_is_external()'s comment.");
        return ESP_ERR_INVALID_STATE;
    }
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    s_rate_guard_meta.version = SAFETY_RATE_GUARD_META_BLOB_VERSION;
    err = hal_kv_set_blob(&h, NVS_KEY_SAFETY_RATE_GUARD, &s_rate_guard_meta, sizeof(s_rate_guard_meta));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
}

bool safety_cfg_store_get_rate_guard_meta(safety_rate_guard_source_t *out_source, float *out_value,
                                           bool *out_has_value)
{
    if (out_has_value) {
        *out_has_value = s_rate_guard_meta.has_value != 0;
    }
    if (!s_rate_guard_meta.has_value) {
        return false;
    }
    if (out_source) {
        *out_source = (safety_rate_guard_source_t)s_rate_guard_meta.source;
    }
    if (out_value) {
        *out_value = s_rate_guard_meta.value;
    }
    return true;
}

bool safety_cfg_store_set_rate_guard_meta(safety_rate_guard_source_t source, float value,
                                           esp_err_t *out_nvs_err)
{
    if (out_nvs_err) {
        *out_nvs_err = ESP_OK;
    }
    s_rate_guard_meta.has_value = 1;
    s_rate_guard_meta.source = (uint8_t)source;
    s_rate_guard_meta.value = value;
    esp_err_t err = save_rate_guard_meta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "safety_cfg_store_set_rate_guard_meta: NVS write failed (%s) -- provenance "
                      "label applied live but will not survive a reboot",
                 esp_err_to_name(err));
    }
    if (out_nvs_err) {
        *out_nvs_err = err;
    }
    return true;
}

void safety_cfg_store_clear_rate_guard_meta(void)
{
    reset_rate_guard_meta_to_defaults();
    esp_err_t err = save_rate_guard_meta();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "safety_cfg_store_clear_rate_guard_meta: NVS write failed (%s)", esp_err_to_name(err));
    }
}

static int index_for_id(uint16_t id); /* defined below -- forward declared for ct_cal_channel_gain() */

static float ct_cal_channel_gain(size_t ch)
{
    static const uint16_t GAIN_IDS[SAFETY_CT_CAL_CHANNELS] = { 0x030B, 0x030C, 0x030D };
    if (ch >= SAFETY_CT_CAL_CHANNELS) {
        return SAFETY_CT_CAL_DEFAULT_GAIN;
    }
    int idx = index_for_id(GAIN_IDS[ch]);
    if (idx < 0 || !s_store.entries[idx].set) {
        return SAFETY_CT_CAL_DEFAULT_GAIN;
    }
    float g = s_store.entries[idx].value.f32_val;
    return (isfinite(g) && g > 0.0f) ? g : SAFETY_CT_CAL_DEFAULT_GAIN;
}

float safety_cfg_store_ct_cal_channel_gain(size_t ch)
{
    return ct_cal_channel_gain(ch);
}

bool safety_cfg_store_get_ct_cal_input(size_t ch, float *out_a_fs, float *out_zero_mv,
                                        safety_ct_cal_source_t *out_source)
{
    if (ch >= SAFETY_CT_CAL_CHANNELS || !s_ct_cal.ch[ch].has_value) {
        return false;
    }
    if (out_a_fs) {
        *out_a_fs = s_ct_cal.ch[ch].a_fs;
    }
    if (out_zero_mv) {
        *out_zero_mv = s_ct_cal.ch[ch].zero_mv;
    }
    if (out_source) {
        *out_source = (safety_ct_cal_source_t)s_ct_cal.ch[ch].source;
    }
    return true;
}

bool safety_cfg_store_set_ct_cal_input(size_t ch, float a_fs, float zero_mv,
                                        safety_ct_cal_source_t source, float *out_k_ct_v_per_a,
                                        uint16_t *out_zero_counts, esp_err_t *out_nvs_err)
{
    if (out_nvs_err) {
        *out_nvs_err = ESP_OK;
    }
    if (ch >= SAFETY_CT_CAL_CHANNELS) {
        return false;
    }
    /* Manual wins over the sweep -- CT_COMMISSIONING_PLAN.md step 1: "the
     * sweep must not overwrite a manual value." AUTO_ZERO is a deliberate
     * per-channel operator action, always applied regardless of the current
     * source (see the header comment on safety_ct_cal_source_t). */
    if (source == SAFETY_CT_CAL_SOURCE_SWEEP && s_ct_cal.ch[ch].has_value &&
        (safety_ct_cal_source_t)s_ct_cal.ch[ch].source == SAFETY_CT_CAL_SOURCE_MANUAL) {
        return false;
    }
    float gain = ct_cal_channel_gain(ch);
    float k = 0.0f;
    uint16_t zc = 0;
    if (!safety_ct_cal_convert(a_fs, zero_mv, gain, &k, &zc)) {
        return false;
    }
    s_ct_cal.ch[ch].has_value = 1;
    s_ct_cal.ch[ch].source = (uint8_t)source;
    s_ct_cal.ch[ch].a_fs = a_fs;
    s_ct_cal.ch[ch].zero_mv = zero_mv;
    esp_err_t err = save_ct_cal();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "safety_cfg_store_set_ct_cal_input: NVS write failed (%s) -- applied live but "
                      "will not survive a reboot",
                 esp_err_to_name(err));
    }
    if (out_nvs_err) {
        *out_nvs_err = err;
    }
    if (out_k_ct_v_per_a) {
        *out_k_ct_v_per_a = k;
    }
    if (out_zero_counts) {
        *out_zero_counts = zc;
    }
    return true;
}

static int index_for_id(uint16_t id)
{
    for (size_t i = 0; i < SAFETY_CFG_PARAM_COUNT; i++) {
        if (SAFETY_CFG_PARAM_TABLE[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

esp_err_t safety_cfg_store_init(void)
{
    /* 2026-08-28 audit fix (H5 lazy-creation race): created here, FIRST, before
     * any early return below -- not lazily inside safety_cfg_store_refetch()
     * only. safety_link_start() (main.c) creates safety_poll_task ~434 lines
     * BEFORE main.c calls this function, so the poll task can reach a refetch
     * (and therefore ensure_store_lock()) before safety_cfg_store_init() ever
     * runs. If this were left to lazy check-then-create only, two tasks
     * racing a NULL s_store_lock could each create their own mutex -- one
     * handle leaked, and the two tasks would serialize against DIFFERENT
     * objects, defeating the whole point of s_store_lock. Calling it here,
     * unconditionally, as the first statement -- before the part_err early
     * return -- guarantees the mutex exists before safety_link_start() even
     * runs (this function is called from main.c before that, in every real
     * boot ordering that matters; the only path that can still race it is a
     * host test that calls safety_cfg_store_refetch() directly without ever
     * calling init(), which is exactly why ensure_store_lock() stays, unused
     * in this path but present, inside refetch() too -- see its own
     * comment). */
    ensure_store_lock();

    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init for '%s' failed: %s -- safety commissioning cache will not persist",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
        reset_to_defaults();
        return part_err;
    }
    nvs_load_store();
    /* RELAY_LIFE_BUDGET.md -- load the safety relay type on every
     * boot, not only after a fresh POST, and push it into relay_cycles.c
     * immediately so the budget calculation is correct from the first
     * dashboard/LCD read. relay_cycles_init() runs earlier in boot
     * (main_control_bringup.c) than this function (main_network_http.c), so
     * relay_cycles.c's own state already exists by the time this call lands;
     * relay_cycles_set_type() is RAM-only regardless (see its own header
     * comment), so the ordering isn't even load-bearing here. */
    load_safety_relay_type();
    relay_cycles_set_type(RELAY_CYCLES_SAFETY_INDEX, s_safety_relay_type, 0);
    /* CT_COMMISSIONING_PLAN.md step 1 -- load the operator's A_fs/zero_mv
     * inputs and their source markers, same "every boot, not only after a
     * fresh POST" reasoning as the relay type just above. */
    load_ct_cal();
    /* S8 rate-guard write provenance -- same "every boot" reasoning. */
    load_rate_guard_meta();
    /* 2026-08-27 audit fix (defect c): this used to stamp s_fetched_at_us =
     * hal_time_now_us() here whenever the loaded blob's config_crc != 0
     * ("a load from NVS counts as fetched"). That was a DIFFERENT and worse
     * lie than the one the 2026-08-22 fix above already closed: it made
     * fetched_ms_ago report BOARD UPTIME, not fetch age, for any board that
     * booted with a real cache on flash -- "fetched 10 min ago" for a value
     * this boot never actually asked the Pico about, potentially loaded off
     * an image days old and from a Pico that has since been reflashed or
     * reconfigured. safety_cfg_store_fetched_ms_ago()'s own contract
     * ("milliseconds since the last successful refetch") is a LIVE claim --
     * an NVS load is not a fetch, it is a memory of one, and this function
     * must not manufacture a plausible-looking age for it. s_fetched_at_us
     * is left at its `-1` ("never fetched this boot") default; only
     * safety_cfg_store_refetch() below -- an actual live round trip to the
     * Pico -- is allowed to stamp it. The values themselves are still loaded
     * and served (config_crc != 0 still means "set" is meaningful per
     * parameter), only their reported age changes: null/never until this
     * boot's first real refetch, rather than a fabricated "just now". */
    return ESP_OK;
}

uint16_t safety_cfg_store_cached_crc(void)
{
    return s_store.config_crc;
}

/* 2026-09-15 review (review_divergence_check_561efa3b_2026-09-15.md,
 * MEDIUM 5): true whenever the cache is KNOWN to disagree with the live
 * Pico's config_crc and a refetch has not yet caught up -- set the instant
 * safety_cfg_store_maybe_refetch() sees the mismatch (even while it is still
 * backing off, not just while a fetch is actually in flight), cleared only
 * once a refetch actually lands. A refetch failure (bad link, a Pico reboot)
 * leaves the PREVIOUS cache contents in place (safety_cfg_store_refetch_
 * locked()'s own "stage into scratch first" comment), which is exactly the
 * MEDIUM 5 hazard: those stale values keep reading as confidently SET long
 * after they stopped being true. A caller comparing against the live Pico
 * (safety_ceiling_sync.c's broadened standing-divergence fields) must treat
 * every entry as UNKNOWN, not "still matching", while this reads true --
 * see that file's own use of this accessor. */
static bool s_cache_stale = false;

bool safety_cfg_store_cache_is_stale(void)
{
    return s_cache_stale;
}

/* 2026-09-15 review (review_divergence_fixes_b2e7017f_2026-09-15.md,
 * MEDIUM 3): see safety_cfg_store_cache_generation()'s own doc comment
 * (safety_cfg_store.h) -- bumped only at the single successful-refetch
 * choke point in safety_cfg_store_refetch_locked(), never here. */
static uint32_t s_cache_generation = 0;

uint32_t safety_cfg_store_cache_generation(void)
{
    return s_cache_generation;
}

/* 2026-09-15 review follow-up (item G): moved here from safety_cfg_http.c.
 * It lived there because the HTTP commissioning handler is what observes the
 * refusal, but its only READER is safety_ceiling_sync.c -- a safety/ module,
 * which had to include an http/ header to reach it (a layering inversion,
 * and a fake every host-test executable linking safety_ceiling_sync.c then
 * had to supply). This module is the natural owner: it already holds the
 * ESP's view of the Pico's config state, and both sides already depend on
 * it. Writer: safety_cfg_http.c's commissioning_post_handler(). Reader:
 * safety_ceiling_sync.c's standing-divergence warning.
 *
 * Window-bounded (not "ever, since boot") so a long-past refusal the
 * operator already acted on does not keep claiming to explain a NEW,
 * unrelated divergence. A plain int64_t read/write with no lock: a torn read
 * on either side can only mis-date the hint text in a warning string, never
 * affect an enforcement decision. */
#define SAFETY_CFG_STORE_ARMED_REFUSAL_WINDOW_US ((int64_t)5 * 60 * 1000 * 1000)
static int64_t s_last_armed_refusal_us = -SAFETY_CFG_STORE_ARMED_REFUSAL_WINDOW_US;

void safety_cfg_store_note_armed_refusal(void)
{
    s_last_armed_refusal_us = (int64_t)hal_time_now_us();
}

bool safety_cfg_store_recent_armed_refusal(void)
{
    int64_t now_us = (int64_t)hal_time_now_us();
    return (now_us - s_last_armed_refusal_us) < SAFETY_CFG_STORE_ARMED_REFUSAL_WINDOW_US;
}

uint32_t safety_cfg_store_fetched_ms_ago(void)
{
    if (s_fetched_at_us < 0) {
        return UINT32_MAX;
    }
    int64_t elapsed_us = (int64_t)hal_time_now_us() - s_fetched_at_us;
    if (elapsed_us < 0) {
        elapsed_us = 0; /* clock went backwards somehow -- never report a negative age */
    }
    int64_t elapsed_ms = elapsed_us / 1000;
    if (elapsed_ms > (int64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)elapsed_ms;
}

size_t safety_cfg_store_param_count(void)
{
    return SAFETY_CFG_PARAM_COUNT;
}

bool safety_cfg_store_get_by_index(size_t index, safety_cfg_param_t *out)
{
    if (!out || index >= SAFETY_CFG_PARAM_COUNT) {
        return false;
    }
    const safety_cfg_table_row_t *row = &SAFETY_CFG_PARAM_TABLE[index];
    const safety_cfg_entry_t *entry = &s_store.entries[index];
    out->param_id = row->id;
    out->name = row->name;
    out->type = row->type;
    out->set = entry->set != 0;
    out->value = entry->value; /* meaningless when !out->set -- caller's job to check first */
    return true;
}

bool safety_cfg_store_lookup(uint16_t param_id, uint8_t *out_type, const char **out_name)
{
    int idx = index_for_id(param_id);
    if (idx < 0) {
        return false;
    }
    if (out_type) {
        *out_type = SAFETY_CFG_PARAM_TABLE[idx].type;
    }
    if (out_name) {
        *out_name = SAFETY_CFG_PARAM_TABLE[idx].name;
    }
    return true;
}

/* Heap-allocated bundle for safety_cfg_store_refetch_locked()'s two large
 * locals -- 2026-09-08 httpd-stack pass (CLAUDE.md "httpd stack" note).
 * This function is reachable from BOTH safety_poll_task (PSRAM stack, see
 * safety_link.c's "PSRAM stack" comment near xTaskCreatePinnedToCoreWithCaps)
 * AND the shared httpd worker task (internal DRAM, tight -- 632-468 B free
 * measured 2026-09-08) via safety_cfg_http.c's confirm_commit_landed() /
 * ct_cal_post_handler / revert_post_handler's apply_pairs() chain. A struct
 * this size (scratch alone is SAFETY_CFG_PARAM_COUNT entries; page is a full
 * KILNLINK_CONFIG_PAGE_MAX_ENTRIES-entry frame) sitting on whichever task's
 * stack happens to call in is exactly the shared-chain cost
 * check_httpd_task_stack_budget.py's ct_cal_post_handler / revert_post_
 * handler entries were flagging. Moved to a single ordinary malloc() (plain
 * internal-DRAM heap, same convention as setup_progress_http.c's scratch --
 * NOT MALLOC_CAP_SPIRAM: this function only marks s_dirty and defers the
 * actual NVS write to the flash-safe worker below, so there is no flash
 * write on this stack to keep off PSRAM, but there is also no reason to
 * prefer PSRAM over the plain heap for a transient decode buffer), freed on
 * every return path. Allocation failure degrades to a clean `return false`
 * (cache left unchanged, same outcome as any other failed refetch below) --
 * never a partial commit to the Pico or a half-built cache. */
typedef struct {
    safety_cfg_store_blob_t scratch;
    kilnlink_config_page_t page;
} safety_cfg_store_refetch_scratch_t;

/* 2026-09-10 stack-budget pass (safety_poll over its 3104 B ceiling by 16 B
 * once safety_cfg_store_maybe_refetch()'s call into this file joined
 * safety_poll_task's call graph today): these two rate-limited failure-log
 * helpers used to be inlined directly in safety_cfg_store_refetch_locked()
 * below. Each holds its own `now_us` local plus a multi-arg ESP_LOGW/ESP_LOGE
 * call, and a function's stack frame is sized for the UNION of every
 * branch's locals, not just the branch actually taken -- so those bytes were
 * charged against refetch_locked()'s frame (and therefore against
 * safety_poll_task's worst-case depth) on every call, success path included,
 * even though they are needed only on a failure. Pulling them out into their
 * own static functions moves that reservation onto helper frames that are
 * NOT on the path this checker measures as worst-case (get_config_page's own
 * chain dominates), which is what actually recovers the bytes -- not the
 * extraction itself. Both are marked noinline: each has exactly one call
 * site, which GCC -O2 happily inlines right back into refetch_locked()
 * (measured: extracting without noinline left refetch_locked()'s own frame
 * unchanged at 80 B). Behavior is unchanged: same rate-limit state
 * (s_last_refetch_fail_log_us/s_refetch_fail_suppressed), same messages. */
/* Portable noinline: this file's own host tests (test_safety_cfg_store.c,
 * MSVC via build_host_tests.ps1) don't understand GCC/Xtensa's
 * __attribute__((noinline)) syntax at all -- it is a hard syntax error under
 * cl.exe, not merely a no-op, so guard it rather than assume every compiler
 * that builds this file is GCC-compatible. Behavior on the real ESP-IDF
 * (Xtensa GCC) target is unchanged; the host build simply never inlines
 * these differently than any other static function, which is irrelevant off-
 * target (no stack-budget checker runs against a host .exe). */
#if defined(_MSC_VER)
#define SAFETY_CFG_STORE_NOINLINE
#else
#define SAFETY_CFG_STORE_NOINLINE __attribute__((noinline))
#endif

static void SAFETY_CFG_STORE_NOINLINE safety_cfg_store_log_budget_exceeded(uint8_t page_index)
{
    int64_t now_us = (int64_t)hal_time_now_us();
    if (now_us - s_last_refetch_fail_log_us >= SAFETY_CFG_STORE_REFETCH_LOG_INTERVAL_US) {
        ESP_LOGW(TAG, "safety_cfg_store_refetch: wall-clock budget (%u ms) exhausted after "
                      "page %u -- cache left unchanged, retrying on a later poll",
                 (unsigned)SAFETY_CFG_STORE_REFETCH_BUDGET_MS, (unsigned)page_index);
        s_last_refetch_fail_log_us = now_us;
        s_refetch_fail_suppressed = 0;
    } else {
        s_refetch_fail_suppressed++;
    }
}

static void SAFETY_CFG_STORE_NOINLINE safety_cfg_store_log_page_fetch_failed(uint8_t page_index, esp_err_t err)
{
    int64_t now_us = (int64_t)hal_time_now_us();
    if (now_us - s_last_refetch_fail_log_us >= SAFETY_CFG_STORE_REFETCH_LOG_INTERVAL_US) {
        if (s_refetch_fail_suppressed > 0) {
            ESP_LOGW(TAG, "safety_cfg_store_refetch: page %u failed (%s) -- cache left "
                          "unchanged (+%lu more failed attempts suppressed)",
                     (unsigned)page_index, esp_err_to_name(err),
                     (unsigned long)s_refetch_fail_suppressed);
        } else {
            ESP_LOGW(TAG, "safety_cfg_store_refetch: page %u failed (%s) -- cache left unchanged",
                     (unsigned)page_index, esp_err_to_name(err));
        }
        s_last_refetch_fail_log_us = now_us;
        s_refetch_fail_suppressed = 0;
    } else {
        s_refetch_fail_suppressed++;
    }
}

/* The actual refetch body -- unchanged in substance from before the H5 fix,
 * just renamed and made static so safety_cfg_store_refetch() below can wrap
 * it with s_store_lock. MUST NOT be called directly by anything except that
 * wrapper -- every early `return false` here is safe only because the
 * wrapper always pairs its xSemaphoreTake() with an xSemaphoreGive() around
 * whatever this returns, success or failure. */
static bool safety_cfg_store_refetch_locked(SafetyLinkClass *link, uint16_t config_crc)
{
    safety_cfg_store_refetch_scratch_t *scr = malloc(sizeof(*scr));
    if (!scr) {
        ESP_LOGE(TAG, "safety_cfg_store_refetch: malloc(%u) failed -- cache left unchanged",
                 (unsigned)sizeof(*scr));
        return false;
    }

    /* Staged into a scratch copy first -- an interrupted refetch (a page
     * request times out or fails to decode partway through) must leave the
     * PREVIOUS cache exactly as it was, never a mix of old and new pages with
     * no way to tell which is which. Nothing touches s_store until every
     * page has been read successfully. */
    safety_cfg_store_blob_t *scratch = &scr->scratch;
    memset(scratch, 0, sizeof(*scratch));
    scratch->version = SAFETY_CFG_STORE_VERSION;
    scratch->config_crc = config_crc;

    /* SAFETY_CFG_STORE_REFETCH_BUDGET_MS -- 2026-08-23 fix, see that
     * constant's own comment. This whole function runs synchronously inside
     * safety_poll_task, and each page it fetches can legitimately take up to
     * SAFETY_LINK_REPLY_TIMEOUT_MS now that safety_link_get_config_page()
     * actually uses its full declared wait. A multi-page fetch must not be
     * allowed to spend an unbounded number of those budgets back to back
     * inside one poll iteration. */
    int64_t refetch_started_us = (int64_t)hal_time_now_us();

    uint8_t page_index = 0;
    for (;;) {
        int64_t elapsed_us = (int64_t)hal_time_now_us() - refetch_started_us;
        if (elapsed_us >= (int64_t)SAFETY_CFG_STORE_REFETCH_BUDGET_MS * 1000) {
            /* Same "cache left unchanged, retry next poll" outcome as a
             * failed page request below -- safety_cfg_store_maybe_refetch()
             * re-invokes this from scratch (page 0) on the very next poll as
             * long as the CRC still disagrees, so nothing here is lost, only
             * deferred to a later iteration that gets a fresh budget. */
            safety_cfg_store_log_budget_exceeded(page_index);
            free(scr);
            return false;
        }

        /* Pace consecutive page requests. Page N+1 otherwise goes out the
         * instant page N is satisfied -- and when page N came from the stash
         * that is with no wire round trip at all, so the request can leave
         * while the Pico is still transmitting the previous reply.
         *
         * Measured on the bench: the Pico's counters show 1195 requests seen,
         * 1195 handled and 1195 broadcast, with the last reply 157 bytes
         * (page 1) -- yet the ESP receives page 0's reply every time and
         * page 1's never. Two similar frames from the same code path, one
         * arriving and one not; what differs is that page 1 is asked for
         * back to back.
         *
         * A whole config fetch happens once per config change, so tens of
         * milliseconds here cost nothing anyone can observe, and the wall
         * clock budget above still bounds the whole call.
         *
         * BE CLEAR ABOUT WHAT THIS DID: pacing did NOT fix the missing page 1.
         * It is kept because back-to-back requests on a shared link are worth
         * pacing regardless, but the symptom survived it unchanged, so the
         * cause is not "the request went out too early". What the Pico's
         * counters prove is only that it CALLED link_task_send_broadcast() --
         * not that the bytes reached the wire. Settling this needs either a
         * scope/analyser capture of the page-1 reply or a TX-completion (not
         * TX-queued) counter on the Pico side. Do not add another speculative
         * fix here without one of those. */
        if (page_index > 0) {
            vTaskDelay(pdMS_TO_TICKS(SAFETY_CFG_STORE_INTER_PAGE_GAP_MS));
        }

        kilnlink_config_page_t *page = &scr->page;
        esp_err_t err = safety_link_get_config_page(link, page_index, page);
        if (err != ESP_OK) {
            safety_cfg_store_log_page_fetch_failed(page_index, err);
            free(scr);
            return false;
        }
        for (uint8_t i = 0; i < page->entry_count; i++) {
            const kilnlink_config_page_entry_t *e = &page->entries[i];
            int idx = index_for_id(e->param_id);
            if (idx < 0) {
                /* COMMISSIONING.md sec 2: version-tolerant -- an id this
                 * build's table predates (a newer Pico) is simply not
                 * something this cache can show; skipped, not an error. */
                continue;
            }
            /* 2026-08-27 audit fix (commissioning-write defect d): this used
             * to be an unconditional `= 1` -- every entry the Pico sent was
             * reported set regardless of whether IT considered the field
             * set, so a genuinely-unset no-safe-default field (abs_max_temp_c
             * before commissioning, most dangerously) showed up on the
             * operator page as "{set:true, value:0}" -- 0 on that field means
             * the overtemperature guard never trips. `e->set` now carries the
             * Pico's own answer, decoded off KILNLINK_CONFIG_PAGE_UNSET_BIT
             * (kilnlink_config_page.h's own header comment has the full wire
             * format and protocol-version-bump reasoning). A pre-fix Pico
             * never sets that bit, so this degrades to the old (less honest,
             * but not WRONGLY MORE confident) behavior against one. */
            scratch->entries[idx].set = e->set ? 1 : 0;
            scratch->entries[idx].value = e->value;
        }
        if (!page->more) {
            break;
        }
        page_index++;
        if (page_index == 0) {
            /* uint8_t wrapped -- 256 pages is not a real page count for a
             * 57-entry table (even at the smallest 4-byte entries that is
             * ~14 pages worst case); a Pico claiming more forever is a
             * protocol fault, not something to loop on forever. */
            ESP_LOGE(TAG, "safety_cfg_store_refetch: page_index wrapped without more==0 -- aborting");
            free(scr);
            return false;
        }
    }

    /* safety_cfg_store_refetch_locked's own inline diff -- see the comment
     * above safety_cfg_store_diff_count() for why this is not a separate
     * function. Same logic that function used to have: one param at a time,
     * SAFETY_CFG_PARAM_TABLE order, bit-exact comparison (a diff tool that
     * used epsilon-fuzzy float comparison could silently hide a real, if
     * tiny, change from the operator). */
    s_diff_count = 0;
    s_diff_truncated = false;
    s_diff_from_crc = s_store.config_crc;
    s_diff_to_crc = scratch->config_crc;
    for (size_t di = 0; di < SAFETY_CFG_PARAM_COUNT; di++) {
        const safety_cfg_entry_t *o = &s_store.entries[di];
        const safety_cfg_entry_t *n = &scratch->entries[di];
        bool changed;
        if (!o->set && !n->set) {
            changed = false;
        } else if (o->set != n->set) {
            changed = true;
        } else {
            switch (SAFETY_CFG_PARAM_TABLE[di].type) {
            case KILNLINK_PARAM_TYPE_BOOL:
            case KILNLINK_PARAM_TYPE_U8:
                changed = (o->value.u8_val != n->value.u8_val);
                break;
            case KILNLINK_PARAM_TYPE_U16:
                changed = (o->value.u16_val != n->value.u16_val);
                break;
            case KILNLINK_PARAM_TYPE_F32:
                changed = (memcmp(&o->value.f32_val, &n->value.f32_val, sizeof(o->value.f32_val)) != 0);
                break;
            default:
                changed = false; /* unknown type: never claim a diff we cannot decode */
                break;
            }
        }
        if (!changed) {
            continue;
        }
        if (s_diff_count >= SAFETY_CFG_STORE_DIFF_MAX) {
            s_diff_truncated = true;
            continue;
        }
        s_diff[s_diff_count].name = SAFETY_CFG_PARAM_TABLE[di].name;
        s_diff[s_diff_count].type = SAFETY_CFG_PARAM_TABLE[di].type;
        s_diff[s_diff_count].old_set = o->set;
        s_diff[s_diff_count].old_value = o->value;
        s_diff[s_diff_count].new_set = n->set;
        s_diff[s_diff_count].new_value = n->value;
        s_diff_count++;
    }

    s_store = *scratch;
    free(scr);
    /* 2026-09-15 review (LOW 9): clear the staleness flag at the single
     * choke point every successful refetch (maybe_refetch, refetch,
     * refetch_nonblocking) funnels through, not only inside
     * safety_cfg_store_maybe_refetch()'s own CRC-match branch. Previously a
     * direct safety_cfg_http.c confirm_commit_landed() refetch (bypassing
     * maybe_refetch entirely) left s_cache_stale set until the NEXT poll
     * tick's maybe_refetch happened to observe matching CRCs, producing a
     * transient false "cache stale" standing-divergence warning in the exact
     * window right after a confirmed commissioning commit. Also bumps the
     * generation counter (MEDIUM 3) so a caller can detect "a fetch landed
     * since I last checked" without a lock. */
    s_cache_stale = false;
    s_cache_generation++;
    s_fetched_at_us = (int64_t)hal_time_now_us();
    /* 2026-08-23 fix: no longer nvs_save_store() directly -- this function
     * runs on safety_poll_task, whose stack is PSRAM (safety_link.c:1636-
     * 1638), and a flash write from there aborts the board (see
     * nvs_save_store()'s own comment). Mark dirty and flush via the
     * flash-safe worker instead -- same "HAZARD" class and same fix shape
     * uart_bridge_ext.c:104-127 already established for CONTROL/PROFILES/
     * AUTOTUNE. */
    s_dirty = true;
    esp_err_t save_err = safety_cfg_store_flush_if_dirty();
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "safety_cfg_store_refetch: fetched OK but the NVS flush failed (%s) -- live "
                      "but will not survive a reboot",
                 esp_err_to_name(save_err));
    }
    ESP_LOGI(TAG, "safety_cfg_store_refetch: refreshed cache, config_crc=0x%04X", (unsigned)config_crc);
    return true;
}

/* Public entry point -- 2026-08-27 audit fix (H5). Wraps safety_cfg_store_
 * refetch_locked() in s_store_lock so the two tasks that can now both call
 * this (safety_poll_task via safety_cfg_store_maybe_refetch(), and the httpd
 * worker via safety_cfg_http.c's confirm_commit_landed()) can never run the
 * scratch-build/commit/flush sequence concurrently -- see s_dirty's own
 * comment above for the full "this used to be single-task, now it isn't"
 * story. `link` may not be NULL (checked before the lock is even touched, so
 * a bad call never blocks on it). */
bool safety_cfg_store_refetch(SafetyLinkClass *link, uint16_t config_crc)
{
    if (!link) {
        return false;
    }
    ensure_store_lock();
    if (s_store_lock) {
        xSemaphoreTake(s_store_lock, portMAX_DELAY);
    }
    bool ok = safety_cfg_store_refetch_locked(link, config_crc);
    if (s_store_lock) {
        xSemaphoreGive(s_store_lock);
    }
    return ok;
}

/* Poll-side entry point -- 2026-08-28 audit fix (N2, BLOCKER). Unlike
 * safety_cfg_store_refetch() above (httpd worker, blocks with portMAX_DELAY),
 * this is the ONLY path safety_poll_task may take: a non-blocking try on
 * s_store_lock. If the httpd worker's confirm_commit_landed() is mid-refetch
 * (holding the lock for up to SAFETY_CFG_STORE_REFETCH_BUDGET_MS, 2s, plus a
 * synchronous NVS flush), this returns false IMMEDIATELY instead of blocking
 * safety_poll_task behind it.
 *
 * This is safe to just skip: safety_cfg_store_maybe_refetch() re-invokes from
 * page 0 on the very next poll (~500ms) for as long as the cached CRC still
 * disagrees with the peer's -- nothing here is lost, only deferred one
 * iteration, exactly the same "retry next poll, fresh budget" contract the
 * wall-clock budget abort inside safety_cfg_store_refetch_locked() already
 * relies on.
 *
 * THE RULE THIS SERVES: safety_poll_task's own iteration is already budgeted
 * up to ~3.2-3.7s (SAFETY_CFG_STORE_REFETCH_BUDGET_MS's comment) against the
 * 5s task-WDT-adjacent margin, and the Pico's link_timeout_s is watching this
 * task's cadence for real -- two previous fixes here were reverted for
 * lengthening that task's blocking (see this file's own history). Blocking
 * safety_poll_task behind an httpd commissioning POST for up to ~2-2.5s BEFORE
 * its own iteration even starts is exactly that mistake; this function is the
 * fix. Do not change safety_poll_task's caller to use safety_cfg_store_
 * refetch() (portMAX_DELAY) instead of this. */
bool safety_cfg_store_refetch_nonblocking(SafetyLinkClass *link, uint16_t config_crc)
{
    if (!link) {
        return false;
    }
    ensure_store_lock();
    if (s_store_lock) {
        if (xSemaphoreTake(s_store_lock, 0) != pdTRUE) {
            /* httpd worker holds it right now -- do not wait. Caller
             * (safety_cfg_store_maybe_refetch()) treats this exactly like a
             * failed page request: cache left unchanged, retried next poll. */
            return false;
        }
    }
    bool ok = safety_cfg_store_refetch_locked(link, config_crc);
    if (s_store_lock) {
        xSemaphoreGive(s_store_lock);
    }
    return ok;
}

/* Retry backoff. Without one, a fetch that fails is retried on every single
 * 500 ms poll for as long as the CRC disagrees -- and on this link that is not
 * merely noisy, it is self-sustaining.
 *
 * Measured on the bench: the Pico's own counters showed it seeing, handling
 * and broadcasting a reply to 868 of 868 page requests, while the ESP recorded
 * 158 frames received against 194 timeouts and only 155 CONFIG_PAGE frames for
 * roughly 340 requests. The replies are real and the wire is fine; they are
 * being dropped at the ESP's 4-deep inbox, which the refetch storm's own extra
 * traffic (two requests and two large replies every poll, on top of GET_STATUS
 * / DIAG / POWER) is what overruns. A failed fetch therefore causes the
 * congestion that makes the next fetch fail. It ran for hours without
 * converging.
 *
 * Backing off turns that around: the link quiets between attempts, the inbox
 * drains, and the retry lands with room to succeed. Retrying *is* still the
 * right behaviour (a Pico that just came back should not need a reboot to be
 * noticed) -- just not at poll rate. Reset to the floor whenever the peer's
 * CRC changes, so a real config change is picked up promptly however long the
 * previous failure had been backing off. */
#define SAFETY_CFG_STORE_RETRY_MIN_MS 2000u
#define SAFETY_CFG_STORE_RETRY_MAX_MS 30000u
static uint32_t s_retry_delay_ms = SAFETY_CFG_STORE_RETRY_MIN_MS;
static int64_t s_retry_not_before_us = 0;
static uint16_t s_retry_crc = 0;

bool safety_cfg_store_maybe_refetch(SafetyLinkClass *link, uint16_t live_config_crc)
{
    if (s_store.config_crc == live_config_crc) {
        s_cache_stale = false;
        return false; /* steady state -- no UART traffic at all, by design */
    }
    /* MEDIUM 5 fix: mark stale the instant a mismatch is seen -- including
     * while still backing off below -- not only while a fetch attempt is
     * actually in flight. */
    s_cache_stale = true;

    int64_t now_us = (int64_t)hal_time_now_us();
    if (live_config_crc != s_retry_crc) {
        /* A different config than the one we have been failing to fetch --
         * treat it as a fresh problem, not a continuation of the old one. */
        s_retry_crc = live_config_crc;
        s_retry_delay_ms = SAFETY_CFG_STORE_RETRY_MIN_MS;
        s_retry_not_before_us = 0;
        /* Any page held from the previous configuration is now meaningless --
         * see safety_link_clear_stashed_config_page()'s comment. */
        safety_link_clear_stashed_config_page(link);
    } else if (now_us < s_retry_not_before_us) {
        return false; /* still backing off -- deliberately no UART traffic */
    }

    if (safety_cfg_store_refetch_nonblocking(link, live_config_crc)) {
        s_retry_delay_ms = SAFETY_CFG_STORE_RETRY_MIN_MS;
        s_retry_not_before_us = 0;
        s_cache_stale = false;
        return true;
    }

    s_retry_not_before_us = (int64_t)hal_time_now_us() + (int64_t)s_retry_delay_ms * 1000;
    if (s_retry_delay_ms < SAFETY_CFG_STORE_RETRY_MAX_MS) {
        s_retry_delay_ms *= 2u;
        if (s_retry_delay_ms > SAFETY_CFG_STORE_RETRY_MAX_MS) {
            s_retry_delay_ms = SAFETY_CFG_STORE_RETRY_MAX_MS;
        }
    }
    return false;
}
