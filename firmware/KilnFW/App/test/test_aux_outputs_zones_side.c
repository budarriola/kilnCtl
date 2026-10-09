// Zones-side half of WP-1 of docs/SPARE_RELAY_ONOFF_PLAN.md. Lives in the zones
// host-test executable (test_zones_http.c's) because that is the one that links
// the real zones_config_json.c and kiln_scope_cfg_files.c:
//   - zones_config_json_validate() refuses a candidate whose zone relay_mask
//     claims a relay an enabled aux output owns, but ONLY once a provider is
//     registered (so a boot load that runs before the aux store reconciles is
//     never refused over a pre-existing conflict);
//   - the factory-reset "kiln" scope deletes aux_out.dat.
#include <string.h>

#include "test_common.h"

#include "aux_outputs_cfg.h"
#include "kiln_scope_cfg_files.h"
#include "zones_config_json.h"

static uint8_t s_fake_aux_mask = 0;
static uint8_t fake_aux_provider(void) { return s_fake_aux_mask; }

static void make_valid_cfg(zones_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = ZONES_CFG_VERSION;
    c->thermo_count = 2;
    c->relay_count = 4;
    c->timing_profile_count = 1;
    c->zones[0].relay_mask = 0x01;
    c->zones[1].relay_mask = 0x02;
}

static void test_validate_hook(void)
{
    TEST_SECTION("zones_config_json_validate: aux-conflict hook (provider-gated)");
    zones_config_json_set_aux_enabled_provider(NULL);
    zones_cfg_t c;
    const char *why = NULL;
    make_valid_cfg(&c);
    TEST_CHECK(zones_config_json_validate(&c, &why), "baseline candidate validates with no provider registered");

    /* Boot-load safety: even with an aux claiming a zone's relay, NO provider -> not refused. */
    s_fake_aux_mask = 0x02;
    TEST_CHECK(zones_config_json_validate(&c, &why), "no provider: a pre-existing conflict never fails a load");

    zones_config_json_set_aux_enabled_provider(fake_aux_provider);
    why = NULL;
    TEST_CHECK(!zones_config_json_validate(&c, &why), "provider registered: zone claiming an aux-owned relay refused");
    TEST_CHECK(why != NULL && strstr(why, "aux") != NULL, "refusal reason names the aux conflict");

    s_fake_aux_mask = 0x0C; /* relays 3,4 are aux; zones own 1,2 */
    TEST_CHECK(zones_config_json_validate(&c, &why), "disjoint aux/zone sets validate");

    s_fake_aux_mask = 0x08;
    c.zones[1].relay_mask = 0x08; /* zone 1 now claims relay 4 */
    TEST_CHECK(!zones_config_json_validate(&c, &why), "zone claiming relay 4 refused when relay 4 is aux");

    /* A zone slot past thermo_count is unused and must not count (same rule as zone_owned_relay_mask). */
    make_valid_cfg(&c);
    c.zones[2].relay_mask = 0x08;
    TEST_CHECK(zones_config_json_validate(&c, &why), "unused trailing zone slot is not a claim");

    s_fake_aux_mask = 0;
    zones_config_json_set_aux_enabled_provider(NULL);
}

static void test_kiln_scope_lists_aux_file(void)
{
    TEST_SECTION("factory-reset kiln scope names aux_out.dat");
    size_t n = 0;
    const char *const *paths = kiln_scope_cfg_files_list(&n);
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(paths[i], AUX_OUTPUTS_FILE_PATH) == 0) {
            found = true;
        }
    }
    TEST_CHECK(found, "kKilnScopeFiles includes AUX_OUTPUTS_FILE_PATH");
}

void run_test_aux_outputs_zones_side(void)
{
    test_validate_hook();
    test_kiln_scope_lists_aux_file();
}
