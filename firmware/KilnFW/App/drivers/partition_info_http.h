// partition_info_http -- GET /api/partitions: the LIVE partition table this
// firmware is actually running with, read straight from the app's own
// esp_partition iterator, not the repo's partitions.csv and not a raw JTAG
// flash read.
//
// FLASH_BUDGET_PLAN.md section 8 item 3 asked "confirm what table is
// actually on the chip" and built tools/PcTools/src/kilnctrl/
// partition_table.py's read_chip_partition_table_bytes() to answer it via
// debug_probe.read_memory() at flash offset 0x8000. That does not work:
// 0x8000 is a FLASH offset, and OpenOCD's read_memory targets the CPU's
// memory-mapped address space, not raw flash -- against the real board it
// fails with "failed to read 4096 B from esp flash at 0x8000" /
// "DEPRECATED! use 'read_memory' not 'mem2array'" / "failed to read
// memory". The parse/diff logic in partition_table.py is genuinely correct
// (unit-tested against synthetic blobs), but the one seam that could not be
// mocked -- the actual chip read -- was never exercised end to end, and it
// turns out not to be reachable the way it was written.
//
// This module answers a BETTER question instead: not "what raw bytes sit
// at flash offset 0x8000" (which needs a JTAG flash-read mechanism this
// codebase does not have -- CLAUDE.md already documents that OpenOCD
// flash-bank operations are unreliable on this board, which is why
// flash_firmware() exists instead of debug_program(peer="esp")), but "what
// partition table is the RUNNING firmware actually using". esp_partition_
// find()/esp_partition_next() walk the table the bootloader already parsed
// and handed to the running app -- no core halt, no flash-bank operation,
// works while the board is busy serving other requests. It also reports
// which slot is currently running (esp_ota_get_running_partition()),
// which a raw table dump cannot.
//
// tools/PcTools/src/kilnctrl/partition_table.py keeps its parse/diff logic
// (parse_partitions_csv(), diff_partition_tables(), PartitionDiff) exactly
// as it was -- that part was always correct and is still unit-tested. Only
// the chip-side read is re-pointed at this endpoint's JSON instead of a
// JTAG flash read; see that module's read_chip_partition_table_from_http()
// and check_chip_partition_table()'s new `use_http` default.
#ifndef PARTITION_INFO_HTTP_H
#define PARTITION_INFO_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers GET /api/partitions on the httpd instance
 * wifi_provision_http_start() already brought up. Non-fatal to app_main on
 * failure, same convention as every other *_http_start() in this
 * directory: logs and returns the esp_err_t, and a failed registration
 * just means the route 404s this boot rather than app_main refusing to
 * come up. */
esp_err_t partition_info_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // PARTITION_INFO_HTTP_H
