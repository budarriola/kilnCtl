/* Host-test stub. Real esp_attr.h places a symbol in a specific linker
 * section (IRAM, DMA-capable pool, external PSRAM .bss, ...); none of that
 * exists in an off-target MSVC build, so every attribute here is a no-op --
 * we're compiling and running the plain logic, not testing memory placement.
 *
 * Add attributes here as drivers start using them; keep them empty. */
#ifndef ESP_ATTR_STUB_H
#define ESP_ATTR_STUB_H

#define EXT_RAM_BSS_ATTR
#define EXT_RAM_ATTR
#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_IRAM_ATTR
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
#define RTC_RODATA_ATTR

#endif /* ESP_ATTR_STUB_H */
