// Host test: the REAL main.c app_main() installs all four factory-reset writer-fence hooks
// (REVIEW_DEVBREAKFX6 MED-1). tools/check_reset_fence_hooks.ps1 reads main.c as text and has been bypassed by
// ever-new shapes of dead code (F8-F17); this test instead COMPILES main.c into the executable, replaces the
// bring-up phases and the four setters with recording stubs, runs app_main(), and asserts each hook was
// installed (non-NULL, the exact predicate) BEFORE the first bring-up phase ran. Any construct that makes an install
// dead at runtime (if (0), goto, early return, renamed app_main, digraph blocks, ...) fails here regardless of syntax.
// The predicates are the REAL ones: relay_authority.c is linked and main.c's own kiln_nvs predicate is reached
// through the recorded hal_kv hook.
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

// main_internal.h pulls in driver headers (uart_owner, panel_spi, ...) that collide with this suite's stubs, and
// app_main() uses none of them. Pre-define its include guard and supply only the declarations main.c needs
// (the phase prototypes below are NOT compared against main_internal.h here; drift is caught only by the target build).
#define MAIN_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { int unused; } main_boot_ctx_t;
// Likewise kiln_io.h / safety_link.h / profile_executor.h drag in uart_protocol.h; main.c only passes these types
// through main_kiln_enter_safe_state(), which this test never calls.
#define KILN_IO_H
#define KILN_IO_RELAY_COUNT 4u /* mirrors kiln_io.h; the real header is faked out above */
#define PROFILE_EXECUTOR_H
typedef struct { int unused; } profile_exec_status_t; /* dashboard_http.h prototype only */
#define UART_LOG_BRIDGE_H
void uart_log_bridge_early_init(void);
#include "esp_err.h"
#include "safety_link.h" // real header (relay_authority.h needs it); its stub deps are the usual ones
#include "relay_authority.h"
#include <stddef.h>
typedef struct kiln_io_s kiln_io_t;
esp_err_t kiln_io_all_relays_off(kiln_io_t *io);
void profile_executor_note_external_app_hold(void);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
extern const char *MAIN_TAG;
void main_heap_stage(const char *stage);
void main_boot_early(main_boot_ctx_t *ctx);
void main_control_bringup(main_boot_ctx_t *ctx);
void main_network_http_bringup(main_boot_ctx_t *ctx);
void main_bridges_bringup(main_boot_ctx_t *ctx);

// The real file under test.
#include "../main.c"

static bool (*s_hook_pref)(void);
static bool (*s_hook_mount)(void);
static bool (*s_hook_cfgfs)(void);
static hal_kv_write_refuse_fn_t s_hook_kv;
static int s_phase_seen;
static int s_hooks_set_at_first_phase = -1;

static int hooks_set_now(void)
{
    return (s_hook_pref != NULL) + (s_hook_mount != NULL) + (s_hook_cfgfs != NULL) + (s_hook_kv != NULL);
}

void pref_cfg_fs_set_reset_refuse_hook(pref_cfg_fs_reset_refuse_fn_t fn) { s_hook_pref = fn; }
void cfg_fs_mount_set_write_refuse_hook(bool (*fn)(void)) { s_hook_mount = fn; }
void cfg_fs_set_write_refuse_hook(bool (*fn)(void)) { s_hook_cfgfs = fn; }
void hal_kv_set_write_refuse_hook(hal_kv_write_refuse_fn_t fn) { s_hook_kv = fn; }

static void phase(void)
{
    if (s_phase_seen++ == 0) s_hooks_set_at_first_phase = hooks_set_now();
}
void uart_log_bridge_early_init(void) {}
void uart_bridge_ext_save_reservation_init(void) {}
void main_boot_early(main_boot_ctx_t *c) { (void)c; phase(); }
void main_control_bringup(main_boot_ctx_t *c) { (void)c; phase(); }
void main_network_http_bringup(main_boot_ctx_t *c) { (void)c; phase(); }
void main_bridges_bringup(main_boot_ctx_t *c) { (void)c; phase(); }

uint32_t safety_link_get_fault_sources(SafetyLinkClass *link) { (void)link; return 0; }
esp_err_t kiln_io_all_relays_off(kiln_io_t *io) { (void)io; return ESP_OK; }
esp_err_t safety_link_set_fault_source(SafetyLinkClass *l, uint32_t s, bool on) { (void)l; (void)s; (void)on; return ESP_OK; }
void profile_executor_note_external_app_hold(void) {}
void dashboard_prime_flash_facts(void) {}
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 100000; }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 100000; }

static void test_app_main_installs_all_four_hooks(void)
{
    TEST_SECTION("app_main() installs the four factory-reset fence hooks before any bring-up phase");
    app_main();
    TEST_CHECK(s_phase_seen == 4, "app_main ran all four bring-up phases");
    TEST_CHECK(s_hooks_set_at_first_phase == 4, "all four hooks were already installed when the first phase ran");
    TEST_CHECK(s_hook_pref == relay_authority_reset_refuses_writer, "pref_cfg_fs hook is relay_authority_reset_refuses_writer");
    TEST_CHECK(s_hook_mount == relay_authority_reset_refuses_writer, "cfg_fs_mount hook is relay_authority_reset_refuses_writer");
    TEST_CHECK(s_hook_cfgfs == relay_authority_reset_refuses_writer, "cfg_fs hook is relay_authority_reset_refuses_writer");
    TEST_CHECK(s_hook_kv != NULL, "hal_kv hook installed");
}

static void test_installed_hooks_behave(void)
{
    TEST_SECTION("the installed hooks refuse writers only while a factory reset is in flight");
    TEST_CHECK(s_hook_kv != NULL, "hal_kv hook present");
    if (s_hook_kv == NULL) return;
    TEST_CHECK(!s_hook_kv("kiln_nvs"), "no reset: kiln_nvs write allowed");
    relay_authority_reset_set_erases_kiln_nvs(true);
    relay_authority_reset_in_flight_begin();
    TEST_CHECK(s_hook_kv("kiln_nvs"), "reset erasing kiln_nvs: kiln_nvs write refused");
    TEST_CHECK(!s_hook_kv("nvs"), "other partition not refused");
    TEST_CHECK(!s_hook_kv(NULL), "NULL partition not refused");
    TEST_CHECK(s_hook_pref(), "pref_cfg_fs hook refuses while the mark is set");
    relay_authority_reset_in_flight_end();
    relay_authority_reset_set_erases_kiln_nvs(false);
    relay_authority_reset_in_flight_begin();
    TEST_CHECK(!s_hook_kv("kiln_nvs"), "reset NOT erasing kiln_nvs: kiln_nvs write allowed");
    relay_authority_reset_in_flight_end();
    TEST_CHECK(!s_hook_kv("kiln_nvs"), "mark cleared: allowed again");
    TEST_CHECK(!s_hook_pref(), "pref_cfg_fs hook allows after the mark clears");
}

int main(void)
{
    test_app_main_installs_all_four_hooks();
    test_installed_hooks_behave();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
