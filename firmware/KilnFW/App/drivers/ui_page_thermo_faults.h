// LCD thermocouple-fault diagnostics page -- the user explicitly asked for
// MAX31856 fault/status visibility as its OWN page, separate from
// ui_page_diagnostics.c's ESP-only system info (firmware version, uptime,
// heap, ESP32 die temp). This page shows, per channel (MAX31856_CHANNEL_COUNT,
// 3 on this board): a fault summary derived from MAX31856Reading::fault_status
// (all eight SR bits named via MAX31856_MASK_*/MAX31856_FAULT_TCRANGE/
// MAX31856_FAULT_CJRANGE, see MAX31856.h), fault_pin_asserted, and spi_failed.
// See this file's .c for the live-data path (thermo_owner_command_read_all())
// and the no-scroll-budget row design.
#ifndef UI_PAGE_THERMO_FAULTS_H
#define UI_PAGE_THERMO_FAULTS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first
 * kiln_ui_show("thermo_faults"). */
lv_obj_t *ui_page_thermo_faults_build(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_THERMO_FAULTS_H
