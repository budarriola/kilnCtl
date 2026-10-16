#ifndef KILNCTL_TEST_STUBS_FLASH_HOP_HOST_STUB_H
#define KILNCTL_TEST_STUBS_FLASH_HOP_HOST_STUB_H
#include <stdbool.h>

/* Host stand-in for bx_flash_worker as seen by flash_hop_run() (drivers/common/flash_hop.h).
 * flash_hop_host_stub.c defines uart_bridge_ext_run_on_flash_worker(): it counts the dispatch,
 * then runs fn with fake_kv's hal_kv_write_safe_here() forced TRUE (the worker's stack is
 * internal) and flash_hop_stub_on_worker() true, and restores both afterwards. A test that
 * wants a PSRAM-stack caller calls fake_kv_set_write_safe_here(false) first; a write that
 * persists then proves it travelled through the worker. flash_hop_host_stub_is_on.c defines
 * uart_bridge_ext_is_on_flash_worker() alone for executables whose own stub file already
 * provides the run function. */
extern unsigned g_flash_hop_stub_dispatches;
extern bool g_flash_hop_stub_on_worker;
extern bool g_flash_hop_stub_fail; /* when true the dispatch refuses (ESP_ERR_INVALID_STATE), fn does not run */
void flash_hop_stub_reset(void);
#endif
