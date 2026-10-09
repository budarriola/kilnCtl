#ifndef ZONES_CONFIG_JSON_INTERNAL_H
#define ZONES_CONFIG_JSON_INTERNAL_H

/* Internal seams for the zones_config_json.c split (ROADMAP.md M15 "files
 * over 1500 lines should be broken up where it makes sense" -- zones_config_
 * json.c had grown to 1867 lines). This header is NOT public API --
 * zones_config_json.h stays that -- it exists purely so pieces that used to
 * be one translation unit (and could reach each other's `static` helpers for
 * free) can still do so now that they are three, following the same move-
 * only precedent as the wifi_prov.c/autotune_engine.c/dashboard_http.c/
 * profiles_http.c splits landed earlier the same day:
 *
 *   zones_config_convert.c -- per-historical-layout converters:
 *                             expected_len_for_version() (renamed
 *                             zones_cfg_expected_len_for_version -- see
 *                             below) and convert_zone_v1()..convert_zone_v14(),
 *                             one per frozen zone_cfg_v*_t snapshot, plus
 *                             set_default_timing_profile(). No public entry
 *                             point of its own -- every function here is
 *                             called only from zones_config_migrate.c.
 *   zones_config_migrate.c -- convert_versioned_blob_to_current() (the
 *                             per-version dispatch switch, stays static --
 *                             only convert_versioned_blob_to_current()'s own
 *                             caller below needs it), zones_config_json_
 *                             compute_crc(), raise_heater_timing_to_floors()
 *                             (stays static, only called from this file's own
 *                             zones_config_json_decode_blob()), and
 *                             zones_config_json_decode_blob() itself -- the
 *                             public "make a blob off flash trustworthy"
 *                             entry point.
 *   zones_config_json.c    -- settings_source chain-walk wrapper, the load-
 *                             path cycle normalizer, zones_config_json_
 *                             validate(), and the HTTP-free field parsers
 *                             (zones_config_json_parse_u8_field()/_float_
 *                             field()/_field_present()/_parse_timing_
 *                             profile_fields()). Keeps the original TAG
 *                             definition (renamed ZONES_CFG_TAG).
 *
 * Every symbol declared below was `static` in the original single file and
 * is widened to file-scope-internal linkage ONLY because a sibling .c file
 * in this split now calls it directly. `TAG` is renamed `ZONES_CFG_TAG` (not
 * just widened) on the same rule dashboard_http.c's `DASH_TAG` and
 * profiles_http.c's `PROFILES_TAG` follow -- every other driver file in
 * App/drivers has its own `static const char *TAG`, so a bare widened `TAG`
 * would collide with the first sibling that also widens its own.
 * `expected_len_for_version` is likewise renamed `zones_cfg_expected_len_
 * for_version` -- grepping App/drivers repo-wide before this split landed
 * found profiles_http.c already defines its OWN `static size_t expected_len_
 * for_version(uint8_t)` with the identical signature; widening this file's
 * copy under the same bare name would not be an immediate link error (both
 * stay in different TUs, each still `static` from the other's point of view)
 * but is exactly the latent-collision shape flagged by the zones_http.c
 * split's own `page_get_handler` precedent, so it is renamed on sight rather
 * than left to bite the next split. Every other widened symbol here
 * (`convert_zone_v1`..`convert_zone_v14`, `set_default_timing_profile`) was
 * grepped the same way and found clean -- no non-static definition and no
 * same-named `static` elsewhere in App/drivers -- so these keep their
 * original names to match the original file's own vocabulary. */

#include "zones_config_json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- shared log tag ------------------------------------------------------ */
extern const char *ZONES_CFG_TAG;

/* ---- zones_config_convert.c ------------------------------------------------
 * Called only from zones_config_migrate.c's convert_versioned_blob_to_current(). */
size_t zones_cfg_expected_len_for_version(uint8_t version);

void convert_zone_v1(const zone_cfg_v1_t *s, zone_cfg_t *d, uint8_t chan_idx);
void convert_zone_v3(const zone_cfg_v3_t *s, zone_cfg_t *d, uint8_t chan_idx);
void convert_zone_v4(const zone_cfg_v4_t *s, zone_cfg_t *d);
void convert_zone_v5(const zone_cfg_v5_t *s, zone_cfg_t *d);
void convert_zone_v7(const zone_cfg_v7_t *s, zone_cfg_t *d);
void convert_zone_v8(const zone_cfg_v8_t *s, zone_cfg_t *d);
void convert_zone_v9(const zone_cfg_v9_t *s, zone_cfg_t *d);
void convert_zone_v10(const zone_cfg_v10_t *s, zone_cfg_t *d, uint8_t chan_idx);
void convert_zone_v11(const zone_cfg_v11_t *s, zone_cfg_t *d);
void convert_zone_v12(const zone_cfg_v12_t *s, zone_cfg_t *d);
void convert_zone_v13(const zone_cfg_v13_t *s, zone_cfg_t *d);
void convert_zone_v14(const zone_cfg_v14_t *s, zone_cfg_t *d);

/* Versions 1-7 predate the nine timing-override fields entirely -- points
 * every zone at one synthesized, all-zero "Default" profile. Defined in
 * zones_config_convert.c alongside the per-zone converters it pairs with;
 * called from zones_config_migrate.c's convert_versioned_blob_to_current()
 * cases 1-7. */
void set_default_timing_profile(zones_cfg_t *out);

#endif /* ZONES_CONFIG_JSON_INTERNAL_H */
