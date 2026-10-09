// Legacy pre-split copies in the DEFAULT `nvs` partition's "kiln_cfg" namespace.
//
// Before the 2026-08-13 partition split, zones_cfg, the run-state record, relay
// cycle counts and fire profiles lived in the default partition. The boot-time
// migrations (zones_config_store.c, run_state.c, relay_cycles.c, profiles_http.c)
// copy them into kiln_nvs/profiles_nvs and leave the old copy behind. factory
// reset erases kiln_nvs/profiles_nvs wholesale but must never erase the default
// partition wholesale (kiln_auth, TOTP and the Wi-Fi driver share it), so a
// never-migrated pre-split board could resurrect old data through those
// migrations after a reset. These two functions erase exactly the legacy keys.
// Erase-first, checked: any failure other than "not found" is returned.
#ifndef LEGACY_DEFAULT_NVS_H
#define LEGACY_DEFAULT_NVS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "kiln" scope: zones_cfg, run_state, relay_cyc legacy keys. */
esp_err_t legacy_default_nvs_erase_kiln(void);
/* "profiles" scope: prof_used, prof0..prof7 legacy keys. */
esp_err_t legacy_default_nvs_erase_profiles(void);
/* Erase only the relay cycle count key (used by the relay_cycles migration
 * after a verified copy). */
esp_err_t legacy_default_nvs_erase_relay_cycles(void);

#ifdef __cplusplus
}
#endif

#endif // LEGACY_DEFAULT_NVS_H
