# REVIEW_FLASH_MMAP_SERIALIZATION: flash mmap readers vs SPI1 writers (XIP from PSRAM)

Date 2026-10-17. Scope: firmware/KilnFW (app image) on ESP-IDF v6.0.2. Follows
AUDIT_XIP_CACHE_DISABLED MED-1 (only the mark-valid read-back was bounded).

Counts: HIGH 0, MED 1 (fixed), LOW 3 (two fixed by the MED fix), INFO 4.

## IDF semantics (verified in source)

- `spi_flash_os_func_app.c:32`: `SPI_FLASH_CACHE_NO_DISABLE` is true for
  `SPIRAM_FETCH_INSTRUCTIONS && SPIRAM_RODATA` (what `CONFIG_SPIRAM_XIP_FROM_PSRAM=y`
  selects). `spi1_start`/`spi1_end` (`:132`, `:180`) then take only the private
  `s_spi1_flash_mutex`; the cache stays enabled and the other core keeps running.
- `CONFIG_SPI_FLASH_AUTO_SUSPEND` is not set. An SPI0 (cache) read while the chip is
  busy erasing/programming returns undefined data.
- `esp_mmu_map` (`esp_mm/esp_mmu_map.c:440-462`) freezes the cache only while it writes
  MMU entries, and invalidates the new range. The reads through the window afterwards
  are not serialized against SPI1.
- After a write, `flash_end_flush_cache` invalidates only the written range, so a line
  misread from an unrelated region stays cached until it is evicted or remapped
  (each `read_otadata` maps afresh, so a retry re-reads).
- `esp_partition_read`/`esp_flash_read` on an unencrypted partition use SPI1 and
  the same mutex: safe. Flash encryption is off.

## Mmap-class readers (app)

| Site | IDF path | Before fix |
|---|---|---|
| `main_network_http.c:183` mark-valid, `:207` state read-back | `read_otadata` mmap + `rewrite_ota_seq` | races; bounded retry only |
| `ota_http_esp.c` `esp_ota_end` | `esp_image_verify` -> `bootloader_mmap` | races (fail-closed) |
| `ota_http_esp.c` rollback check + `mark_app_invalid_rollback_and_reboot` | `read_otadata` + rewrite | races |
| `boot_partition_verify.c:20-24` | `image_validate`/`esp_image_verify`, `read_otadata` | races (read-back verified) |
| `recovery_switch.c:41` | `esp_image_verify` | races (fail-closed) |
| `crash_report.c:582` | `esp_core_dump_get_summary` (core_dump_elf mmap) | races, persisted |
| `ui_page_diagnostics.c:651` prime | `read_otadata` | boot, low exposure |
| `dashboard_http.c:136` `esp_image_get_metadata` | mmap | app_main before tasks |

## Flash writers (all go through `esp_flash_default_chip` os_func start/end)

NVS (`hal_kv_esp.c`, every store, Wi-Fi driver `nvs.net80211`), LittleFS cfg
(`cfg_fs*.c`), stage writer (`update_http.c:55,60`), `pico_img_stage.c:98,106`,
ESP OTA (`esp_ota_write`/`esp_ota_begin` erase, `ota_http_esp.c`), coredump erase
(`system_uart_bridge`), bx_flash_worker / `flash_hop_run` callers. Task identity does
not matter: any of them can be mid-erase while any reader runs.

## Findings

### MED-1 (FIXED): no mmap reader was serialized against any SPI1 writer

Every row of the reader table could read torn data during a concurrent erase from any
writer above. The worst outcome is the otadata rewrite paths: `rewrite_ota_seq` and
`mark_app_invalid_rollback` compute the new entry from a misread active entry
(AUDIT_XIP_CACHE_DISABLED MED-1: next reset lands in recovery). The confirmfx retry
only bounds one site, and its read-back is itself an unserialized mmap read.

Fix (`flash_xip_lock.c`, `flash_xip_lock_esp.c`): `flash_xip_lock_install()`, first
thing in `app_main`, copies `esp_flash_default_chip->os_func` and wraps `start`/`end`
with one recursive mutex. Every SPI1 op from any task now holds it for its bracket
(which covers the busy-wait). Each reader in the table (except the app_main-only
`esp_image_get_metadata`) brackets its IDF call with
`flash_xip_reader_enter()/exit()`. Recursive so reader-writers (mark-valid,
`esp_ota_end`, set_boot) re-enter on their own task. Lock order is always app mutex
then `s_spi1_flash_mutex`. A failed `start` releases the lock (IDF never calls `end`).
Panic coredump swaps `os_func` to noos, bypassing the wrapper. No-op when XIP is off.
Host test `test_flash_xip_lock.c`, negtest 5/5 CAUGHT.

Cost: a writer can wait the length of one reader bracket (an `esp_image_verify` of the
app image, ~100s of ms, only during OTA/recovery entry); readers wait one SPI1 bracket
(one sector erase).

### LOW-1 (FIXED by MED-1): image verifies fail spuriously (fail-closed)
`esp_ota_end`, `recovery_switch` verify, `boot_partition_set_and_verify`.

### LOW-2 (FIXED by MED-1): crash summary from a misread coredump is persisted
`crash_report.c:582` now reads under the lock.

### LOW-3 (open): new mmap callers are not linted
A future `esp_ota_*`/`esp_image_*`/`esp_partition_mmap` call outside a bracket
reopens the race. `check_psram_task_stacks.ps1` lists such calls only for PSRAM-stack
files. Fix: a check that each mmap-class call sits inside a reader bracket.

### INFO
- INFO-1: `dashboard_prime_flash_facts` mmap runs in app_main before any other app
  task exists; left unbracketed.
- INFO-2: KilnFW_recovery does not enable XIP from PSRAM, so IDF disables the cache
  for every SPI1 op there: not affected.
- INFO-3: `esp_partition_read` users (`update_http.c` SHA, `pico_image_source.c`,
  `ota_pico_relay.c`, `cfg_fs_mount.c`, LittleFS) use SPI1: already serialized by IDF.
- INFO-4: `CONFIG_SPI_FLASH_AUTO_SUSPEND` would also close this in hardware, but needs
  the flash part qualified; the lock does not depend on it.
- Needs a bench soak (OTA, recovery entry/exit, first-boot confirm) to confirm no
  latency regressions; not run (bench not touched).
