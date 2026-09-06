/* hal_sysinfo.h -- system/board identity queries. ESP-only today (SaftyFW
 * has no equivalent surface). See docs/HW_ABSTRACTION_PLAN.md
 * "hal_time / hal_wdt / hal_pwm / hal_sysinfo": "reset reason, running
 * partition, build descriptor, chip temperature, esp_random, core-dump
 * presence. Dedupes crash_report.c (which reads esp_core_dump),
 * ui_page_diagnostics.c:522, dashboard, partition_info and
 * main_network_http's independent esp_ota_get_running_partition calls.
 * esp_partition stays read-only here; OTA writes are out of scope."
 *
 * Consumer census, one operation per real call site, re-verified:
 *  - Reset reason: esp_reset_reason() -- dashboard_http.c:403,
 *    ui_page_diagnostics.c:520, each with its own private
 *    esp_reset_reason_t -> string table (reset_reason_name /
 *    reset_reason_str) that must NOT be collapsed to one shared string set
 *    here -- the two tables' wording differs and this header only need
 *    hand back the enum-shaped value, not render it. crash_report.c also
 *    reads esp_reset_reason() (crash_report.c:312) to decide whether a
 *    reset is worth recording as a crash at all.
 *  - Running partition: esp_ota_get_running_partition() -- independent call
 *    sites at dashboard_http.c:122, ui_page_diagnostics.c:522,
 *    partition_info_http.c:94, ota_http_esp.c:485, exactly the four the
 *    plan names (dashboard/ui_page_diagnostics/partition_info/
 *    main_network_http-family) plus ota_http_esp.c which the plan's prose
 *    did not name individually but is the same call in the same family.
 *    Real fields read off the returned pointer: label (dashboard_http.c,
 *    ui_page_diagnostics.c), size (both), address (dashboard_http.c:126,
 *    fed to esp_image_get_metadata() to learn image_len -- that metadata
 *    call itself stays above this header, out of scope, since it is image
 *    parsing, not partition-table or OTA-op access).
 *  - Build descriptor: esp_app_get_description() -- dashboard_http.c:387,
 *    ui_page_diagnostics.c:511. Real fields read: version, date, time
 *    (both sites format date+time together into one build string).
 *  - Chip die temperature: board_temps.c's temperature_sensor_install /
 *    _enable / _get_celsius / _uninstall sequence (ESP32-S3's on-die
 *    sensor). One caller, one lifecycle (install+enable once at init,
 *    get_celsius polled, uninstall on shutdown/failure) -- covered here as
 *    init/read_celsius/deinit rather than mirroring all four IDF calls
 *    one-for-one, matching hal_time's "primitives that cover the census,
 *    not a 1:1 vendor mirror" approach.
 *  - esp_random(): safety_link.c:358 (esp_boot_id), plus safety_link_
 *    commands.c/_frames.c/_inbox.c/_payload.c/_poll.c which all #include
 *    esp_random.h but (re-checked) call it only via safety_link.c's shared
 *    boot-id path, not independently -- one real logical use, one call
 *    site that matters. ota_http*.c's esp_random.h includes are likewise
 *    all in service of one shared random-token helper, not four
 *    independent uses -- not re-derived in full here since it is
 *    orthogonal to the crash/identity surface this header targets, but
 *    hal_sysinfo_random_u32() is general enough to cover it too.
 *  - Core-dump presence: crash_report.c's esp_core_dump_image_check() /
 *    esp_core_dump_get_summary() / esp_core_dump_image_erase() sequence.
 *    Deliberately exposed as presence-plus-erase only, NOT a full summary
 *    reader -- crash_report.c's own fill_from_summary()/crash_report_
 *    dump_id() logic (deliberately NOT hashing esp_core_dump_summary_t
 *    itself, per that file's own comment on struct-layout fragility) stays
 *    entirely above this header and keeps taking the live IDF
 *    esp_core_dump_summary_t type directly; this header only reports
 *    whether a dump image exists and lets the caller erase it, matching
 *    the plan's explicit "esp_partition stays read-only here" scope limit
 *    (a full summary reader would need to either leak the vendor type
 *    through the interface or duplicate its many fields as a portable
 *    struct, neither of which any real second consumer needs today).
 *
 * Disagreement with the plan: none found in scope. The plan lists six
 * operations (reset reason, running partition, build descriptor, chip
 * temperature, esp_random, core-dump presence) and this header exposes
 * exactly those six, no more -- OTA writes and full core-dump summary
 * parsing are explicitly out of scope per the plan text quoted above.
 *
 * WHAT THIS IS NOT: an OTA-write interface (esp_ota_set_boot_partition,
 * esp_ota_begin/write/end all stay out of scope, per the plan) and not a
 * full esp_core_dump_summary_t reader (see above). Both are call-site logic
 * layered on real IDF headers above this one, same split hal_flash.h draws
 * against config_store's policy layer.
 *
 * Threading/ownership contract:
 *  - Every query here (reset reason, running partition, build descriptor,
 *    esp_random) is a cheap, side-effect-free, stateless read on the real
 *    ESP backend -- dashboard_http.c's own header comment notes this
 *    explicitly for exactly this reason (safe to call on every HTTP poll).
 *    No handle, no init call for those.
 *  - hal_sysinfo_temp_init()/_read_celsius()/_deinit() DO carry a lifecycle
 *    (install/enable/get/uninstall) matching board_temps.c's existing
 *    single-instance module -- call temp_init() once before the first
 *    temp_read_celsius(), matching that file's existing contract.
 *  - hal_sysinfo_coredump_present()/_erase() are boot-time-oriented in
 *    practice (crash_report.c's real flow checks presence once during
 *    startup crash-report generation and erases only after that report is
 *    consumed) but carry no enforced single-caller lock here -- none of the
 *    real call sites need one.
 */
#ifndef KILNCTL_HAL_SYSINFO_H
#define KILNCTL_HAL_SYSINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* esp_reset_reason_t's real values, as branched on today (dashboard_http.c's
 * reset_reason_name() / ui_page_diagnostics.c's reset_reason_str() each
 * have their own display-string table over this same set) -- this header
 * hands back the enum only; rendering stays a caller concern since the two
 * existing tables' wording differs and neither should be forced to match
 * the other. */
typedef enum {
    HAL_RESET_UNKNOWN = 0,
    HAL_RESET_POWERON,
    HAL_RESET_EXT,
    HAL_RESET_SW,
    HAL_RESET_PANIC,
    HAL_RESET_INT_WDT,
    HAL_RESET_TASK_WDT,
    HAL_RESET_WDT,
    HAL_RESET_DEEPSLEEP,
    HAL_RESET_BROWNOUT,
    HAL_RESET_SDIO,
    HAL_RESET_USB,
    HAL_RESET_JTAG,
    HAL_RESET_EFUSE,
    HAL_RESET_PWR_GLITCH,
    HAL_RESET_CPU_LOCKUP,
} hal_reset_reason_t;

hal_reset_reason_t hal_sysinfo_reset_reason(void);

/* Running-partition facts actually read by the four call sites above --
 * label/size/address, the exact fields dashboard_http.c/ui_page_
 * diagnostics.c/partition_info_http.c/ota_http_esp.c consume. `label` is a
 * caller-owned buffer (matches esp_partition_t::label's own fixed 16-byte
 * array, copied out rather than exposing the vendor pointer). */
typedef struct {
    char     label[17];   /* esp_partition_t::label is 16 bytes + NUL, copied out */
    uint32_t address;     /* flash offset -- dashboard_http.c:126's esp_image_get_metadata() input */
    uint32_t size;         /* bytes -- both display sites' "(N KB)" formatting */
} hal_sysinfo_partition_info_t;

hal_status_t hal_sysinfo_get_running_partition(hal_sysinfo_partition_info_t *out);

/* esp_app_desc_t's version/date/time fields -- the only three fields either
 * real call site reads. Sizes match esp_app_desc.h's own fixed char arrays
 * (32/16/16 bytes) so a caller can snprintf these directly the way
 * dashboard_http.c/ui_page_diagnostics.c already do. */
typedef struct {
    char version[32];
    char date[16];
    char time[16];
    bool valid;   /* false if esp_app_get_description() returned NULL (both
                    * real call sites check this before using the struct) */
} hal_sysinfo_build_info_t;

void hal_sysinfo_get_build_info(hal_sysinfo_build_info_t *out);

/* ESP32-S3 on-die temperature sensor lifecycle, per board_temps.c's real
 * install/enable/get_celsius/uninstall sequence -- see this header's
 * threading contract above for call order. */
hal_status_t hal_sysinfo_temp_init(void);
hal_status_t hal_sysinfo_temp_read_celsius(float *out_c);
hal_status_t hal_sysinfo_temp_deinit(void);

/* esp_random()-shaped: hardware RNG, safety_link.c's boot-id source among
 * others. No seeding call -- esp_random() takes none and needs none
 * (SAR ADC/RF noise backed, per esp_random.h). */
uint32_t hal_sysinfo_random_u32(void);

/* esp_fill_random()-shaped: hardware RNG into a caller buffer -- ota_http.c's
 * ota_challenge_get_handler() nonce fill is the one real call site (2026-09-06
 * migration; see this header's top comment). `len` bytes are written to
 * `buf`; `buf` may be NULL only if `len` is 0. */
void hal_sysinfo_fill_random(void *buf, size_t len);

/* True if a core-dump image is present in flash (esp_core_dump_image_check()
 * succeeding), matching crash_report.c's own gate before it attempts
 * esp_core_dump_get_summary(). Deliberately does not surface the summary
 * itself -- see this header's top comment on why a full summary reader is
 * out of scope. */
bool hal_sysinfo_coredump_present(void);

/* Erases the core-dump image -- crash_report.c's esp_core_dump_image_erase()
 * call, made once the crash report built from it has been consumed. */
hal_status_t hal_sysinfo_coredump_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_SYSINFO_H */
