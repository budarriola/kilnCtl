// aux_outputs_cfg -- persisted store of the spare-relay "aux" on/off outputs
// (docs/SPARE_RELAY_ONOFF_PLAN.md sections 3 and 14). A small separate store
// parallel to zones_cfg_t: it deliberately does NOT widen zones[] and bumps no
// ZONES_CFG_VERSION / PROFILE_VERSION / KILN_CFG_STORE_VERSION.
//
// One entry per relay (KILN_IO_RELAY_COUNT entries, entry i = relay i+1), so
// "one aux per relay" holds by construction. Fail-safe state is fixed OFF (no
// field). Relay label/type stay in relay_names. Aux is not part of a kiln
// package. NVS authoritative (kiln_nvs / kiln_cfg / key "aux_out_cfg"), cfg
// mirror "aux_out.dat" through pref_cfg_fs, same shape as display_power_cfg.
//
// CONFLICT INVARIANT: a relay bit may be in at most one of any zone's
// relay_mask or an ENABLED aux entry. Writers of the zones side call
// zones_config_json_validate(), which consults the provider registered with
// zones_config_json_set_aux_enabled_provider(aux_outputs_cfg_enabled_mask) --
// aux_outputs_http_start() does that registration at boot (this module deliberately does not link
// zones_config_json.c). BOOT ORDER: both aux_outputs_cfg_start() and the provider
// registration must run AFTER kiln_cfg_store_init(). relay_names_load() runs
// before nvs_load() in zones_http_start(), so the zones union is still 0 there,
// and kiln_cfg_store_init() re-imports the active package later (main_network_http.c),
// which can change relay_mask; with the provider registered before that import, a
// conflict would fail the import and clear and persist the active package id. This module's
// own setter checks the other side against the zones union its caller passes in. At start(), if the
// persisted pair conflicts (corrupt, or a writer was missed), the aux entry is
// forced disabled IN RAM ONLY, a sticky aux_conflict flag is reported, and the
// zone is never stripped of its relay. The persisted blob is not rewritten.
//
// QUARANTINE: a stored blob whose version is NEWER than this build knows is
// not defaulted: every aux reads disabled this boot, aux_outputs_cfg_quarantined()
// reports it, and set() refuses (never overwrites a newer firmware's data).
// A corrupt/short blob falls back to all-disabled defaults and may be rewritten.
//
// PICO MASK (WP-9): aux_outputs_cfg_enabled_mask() is the accessor the ESP uses
// to strip aux bits from the relay masks it sends the Pico.
#ifndef AUX_OUTPUTS_CFG_H
#define AUX_OUTPUTS_CFG_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "aux_outputs_conflict.h"

/* Blob version this build writes and fully understands (also used by the POST /api/cfgfs/file rule). */
#define AUX_OUTPUTS_CFG_VERSION 1

/* cfg_fs relative path of this item's dual-write mirror (NVS side lives in kiln_nvs). */
#define AUX_OUTPUTS_FILE_PATH "aux_out.dat"

#define AUX_OUTPUTS_COUNT 4u /* == KILN_IO_RELAY_COUNT, asserted in the .c */

#define AUX_HYST_C_DEFAULT 2.0f
#define AUX_MIN_ON_OFF_S_DEFAULT 30
#define AUX_HYST_C_MIN 0.5f
#define AUX_HYST_C_MAX 25.0f
#define AUX_MIN_ON_OFF_S_MIN 1
#define AUX_MIN_ON_OFF_S_MAX 3600
#define AUX_TC_ZONE_NONE 0xFFu /* effective tc_zone when stored tc_zone_plus1 == 0 */

/* On-flash entry. All-zero = disabled, today's behaviour. */
typedef struct {
    uint8_t enabled;       /* 0/1 */
    uint8_t tc_zone_plus1; /* 0 = none, 1..MAX31856_CHANNEL_COUNT = zone 0..n-1 */
    uint16_t min_on_s;     /* 0 -> AUX_MIN_ON_OFF_S_DEFAULT at read */
    float hyst_c;          /* 0 -> AUX_HYST_C_DEFAULT at read */
    uint16_t min_off_s;    /* 0 -> AUX_MIN_ON_OFF_S_DEFAULT at read */
    uint16_t reserved;     /* must be 0 */
} aux_output_entry_t;

/* Effective (read-side) view: defaults substituted, conflict applied. */
typedef struct {
    bool enabled;   /* false when forced disabled by conflict/quarantine */
    bool conflicted; /* this relay's entry was forced disabled by a zone conflict */
    uint8_t tc_zone; /* 0.. or AUX_TC_ZONE_NONE */
    float hyst_c;
    uint16_t min_on_s;
    uint16_t min_off_s;
} aux_output_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the persisted store (all-disabled if nothing valid), reconciles it
 * against `zones_relay_union` (the OR of every configured zone's relay_mask,
 * zone_owned_relay_mask()). Non-fatal: always ESP_OK. Idempotent. aux_outputs_http_start() calls this
 * AFTER kiln_cfg_store_init() (NOT next to relay_names_load(): the zones union
 * is 0 there and the package re-import can still change it), then registers
 * aux_outputs_cfg_enabled_mask as the zones-validate provider. */
esp_err_t aux_outputs_cfg_start(uint8_t zones_relay_union);

/* relay is 1-based (1..AUX_OUTPUTS_COUNT). false if out of range. */
bool aux_outputs_cfg_get(uint8_t relay, aux_output_t *out);

/* Bit i = relay i+1; ENABLED and not conflicted/quarantined only. */
uint8_t aux_outputs_cfg_enabled_mask(void);

/* Sticky: relays forced disabled at start() because a zone also claims them.
 * Cleared per relay by a successful set() of that relay. */
uint8_t aux_outputs_cfg_conflict_mask(void);
bool aux_outputs_cfg_conflict(void);

bool aux_outputs_cfg_quarantined(void);

/* Validates and persists one relay's entry (relay 1-based). Refused, nothing
 * changed: ESP_ERR_INVALID_ARG (bad relay, field out of range, tc_zone_plus1
 * beyond MAX31856_CHANNEL_COUNT, reserved != 0); ESP_ERR_INVALID_STATE (enabling a
 * relay in `zones_relay_union`, or the store is quarantined). Save first, RAM
 * second: a save failure is returned unchanged and RAM is left exactly as it was. Thread-safe. */
esp_err_t aux_outputs_cfg_set(uint8_t relay, const aux_output_entry_t *entry, uint8_t zones_relay_union);

/* Pure field-range check shared with the backup importer (backup_import.c), so a restored
 * entry is held to exactly the rules aux_outputs_cfg_set() applies: enabled 0/1, reserved 0,
 * tc_zone_plus1 within MAX31856_CHANNEL_COUNT, hyst_c / min_on_s / min_off_s either 0
 * ("default") or within their AUX_* bounds. Touches no state. */
bool aux_outputs_cfg_entry_valid(const aux_output_entry_t *entry);

/* The STORED entry for `relay` (1-based), exactly as persisted: no defaults substituted, no
 * conflict/quarantine applied. For a caller that must put an entry back bit-for-bit. */
bool aux_outputs_cfg_get_raw(uint8_t relay, aux_output_entry_t *out);

/* true = the cfg file re-read right now is valid, at the RAM rev, and its entries equal the RAM
 * entries. A RAM-only read-back cannot see a save that failed or a file the filesystem did not keep.
 * NVS is not consulted: since the dual-write close no save writes it. */
bool aux_outputs_cfg_verify_persisted(void);

/* ---- Zone-to-aux conversion journal (docs/SPARE_RELAY_ONOFF_PLAN.md section 10) ----
 * One small persisted marker, kept in the same NVS namespace as the aux store, written BEFORE the
 * conversion's first write and erased only after its final read-back. If power fails in the middle
 * the marker survives the reboot: /api/readiness reports it, and a resume request can finish the
 * missing steps. `stage`: 1 begun (nothing changed yet), 2 zone freed, 3 aux enabled, 4 profiles
 * rewritten. hyst/min_on/min_off/has_tc are the entry to enable, because freeing the zone zeroes
 * the values they came from. */
typedef struct {
    uint8_t zone;
    uint8_t relay; /* 1-based */
    uint8_t stage;
    uint8_t has_tc;
    float hyst_c;
    uint16_t min_on_s;
    uint16_t min_off_s;
} aux_convert_journal_t;

/* true = a valid marker exists and was copied to *out. */
bool aux_convert_journal_read(aux_convert_journal_t *out);
/* true = written AND read back equal. */
bool aux_convert_journal_write(const aux_convert_journal_t *j);
/* true = the marker is gone (also when there was none). */
bool aux_convert_journal_clear(void);

/* Read-only dual-write status for GET /api/cfgfs (see display_power_cfg.h). */
void aux_outputs_cfg_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid,
                                          uint32_t *nvs_rev, bool *diverged);

#ifdef __cplusplus
}
#endif

#endif // AUX_OUTPUTS_CFG_H
