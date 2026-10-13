# Review: PSRAM moves (e76e582b) and TOTP handler fixes (f8860c92), 2026-10-09

Read-only review of two commits on origin/dev (tip `fa041edc` at review time).
No board, no target build. Line numbers are against `fa041edc`.

## Summary

| Sev | Where | Finding |
|-----|-------|---------|
| MED | `firmware/KilnFW/App/drivers/http/auth_totp_http.c:497-501` | reset handler: missing-field early return leaves `new_password` / `reset_token` on the stack, not zeroed | FIXED in fdf8153a |
| LOW | `firmware/KilnFW/App/drivers/http/auth_totp_http.c:348,360-437` | forgot handler: stack `body` (holds the TOTP code) is zeroed only on the recv-failure path, never after parsing | FIXED in fdf8153a |
| LOW | `firmware/KilnFW/App/drivers/http/auth_totp_http.c:366-369` | forgot handler: missing-field early return leaves `code` not zeroed | FIXED in fdf8153a |
| INFO | `docs/audits/INTERNAL_HEAP_MARGIN_2026-10-09.md` sec 5c | real saving is about 8.1 kB, not 7.6 kB; `s_touch_groups` is 2176 B, not 1904 B |
| INFO | `firmware/KilnFW/App/drivers/http/auth_totp_http.c:470-472` | reset OOM path records no backoff (no secret involved) |

No defect found in e76e582b. All eight moved objects are safe in PSRAM on this
configuration.

## 1. e76e582b: moves to EXT_RAM_BSS_ATTR

### Configuration facts

- `firmware/KilnFW/sdkconfig.defaults:367` sets `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`.
  The generated bench `sdkconfig` agrees and has
  `# CONFIG_SPIRAM_XIP_FROM_PSRAM is not set`, `# CONFIG_SPIRAM_FETCH_INSTRUCTIONS is not set`,
  `# CONFIG_SPIRAM_RODATA is not set`, `# CONFIG_SPI_FLASH_AUTO_SUSPEND is not set`,
  and `CONFIG_SPIRAM_BOOT_INIT=y` without `SPIRAM_IGNORE_NOTFOUND`.
- So PSRAM cannot be read or written while the flash cache is disabled. During any
  SPI1 flash operation, IDF suspends the scheduler on both cores, parks the other CPU
  in IRAM, and runs only IRAM ISRs (`ESP_INTR_FLAG_IRAM`). Task code therefore cannot
  touch these tables during a cache-disabled window. The only hazards are ISR access
  and passing the object as a flash read/write buffer.
- Zero-init holds. IDF v6.0.2 `esp_system/port/cpu_start.c:798` calls
  `esp_psram_bss_init()`, which `memset`s `_ext_ram_bss_start.._ext_ram_bss_end`
  (`esp_psram/system_layer/esp_psram.c:672-677`), before any app code runs. If PSRAM
  fails to init, boot aborts, so a table is never left uninitialized. The tree already
  had this dependency through other ext-bss users.
- `esp_timer` has no ISR dispatch (`CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD` not
  set), and none of the touched files contains `IRAM_ATTR`.

### Per object

| Object | File:line | Size (ELF) | Accessors | ISR | Flash/NVS buffer | DMA | Verdict |
|--------|-----------|-----------|-----------|-----|------------------|-----|---------|
| `s_login_lockouts` | `drivers/http/web_auth_login_http.c:136` | 960 | login handler (httpd task), lines 203, 225 | no | no (RAM only, no persist call in file) | no | OK |
| `s_totp_lockouts` | `drivers/http/auth_totp_http.c:196` | 960 | forgot/reset handlers (httpd), lines 206, 224 | no | no | no | OK |
| `s_web_auth_table` | `drivers/http/http_session_iface.c:31` | 768 | `http_session_table()` from httpd handlers, `security_backend_web_auth.c`, and `wifi_prov_link.c:372` (`http_auth_any_ap_session_active`, Wi-Fi owner task) | no | no (`web_auth_session.c` never persists) | no | OK |
| `s_touch_groups` | `drivers/ui/ui_theme.c:94` | 2176 | `ui_theme_register_touch_group` (`ui_topbar.c:286`) and `ui_theme_resolve_touch_target` (`lvgl_port.c:530`, indev read path in the LVGL task) | no | no | no | OK |
| `s_scan_stage` | `drivers/net/wifi_prov_api.c:28` | 700 | `do_scan` on the owner task (`wifi_prov.c:851`), `memcpy` out in `wifi_prov_scan` (`wifi_prov_api.c:978`) | no | no | no | OK |
| `records[20]` (`do_scan`) | `drivers/net/wifi_prov_api.c:940` | 1840 | `esp_wifi_scan_get_ap_records()` destination | no | no | no: the driver copies from its own scan list (itself in PSRAM under `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`) | OK |
| `scan_results[20]` | `drivers/net/wifi_prov_link.c:287` | 700 | `select_and_apply_join_candidate` (owner task) via `do_scan` | no | no | no | OK |

`s_remote_totp_slot` (60 B) and `s_web_auth_table_init_done` stay internal, which is
harmless.

PSRAM stacks: these are statics, not stacks. No accessor runs on a task whose stack
was moved, and the move does not change any task's ability to do flash operations.

Locking: every lock is a separate handle (`s_totp_lock`, the session mutex), still
internal. Contention and latency are negligible. The cost is one PSRAM cache line per
touch or login scan.

### Saving

Sizes come from `xtensa-esp32s3-elf-nm -S` on `firmware/KilnFW/elf_archive/KilnCtrl-latest.elf`
(linked 2026-10-09 20:40, before this commit, all eight objects in `.bss` at `0x3fc...`):
960 + 960 + 768 + 2176 + 700 + 1840 + 700 = **8104 B** (0x880 for `s_touch_groups`;
the commit note says 1904). The real `.dram0.bss` reduction is about 8.1 kB less
alignment slack, so the "~7.6 kB" estimate is conservative and the saving is real.
Still to do, as the commit note says: measure `.dram0.bss` on the next target build
and ratchet `check_kilnfw_dram_bss_budget`. Another 700 B candidate of the same shape
remains internal: `s_scan_results` (0x2bc, `drivers/ui/ui_page_network_manage.c:98`, LVGL task and memcpy only). Moved to PSRAM in fdf8153a.

## 2. f8860c92: auth_totp_http.c CRITICAL-1 / HIGH-1 / LOW-3

### Fixes verified

- CRITICAL-1 (forgot handler, `auth_totp_http.c:353`): the recv-failure path used to
  call `totp_secure_zero(body, RESET_BODY_MAX)` (512 B into a 128 B stack array, a
  384 B stack overwrite) and then `free(body)` on a stack array (heap corruption).
  It now zeroes `sizeof(body)` and does not free. Correct.
- HIGH-1 (reset handler, `auth_totp_http.c:478-479`): the recv-failure path now zeroes
  and frees the 512 B heap body. The leak is gone. Correct.
- LOW-3 (`auth_totp_http.c:466-469`): PSRAM first, then any 8-bit heap. `free()` is
  valid for `heap_caps_malloc` memory. The buffer only goes to `httpd_req_recv` (lwIP
  memcpy) and `http_form_find_field`, never to flash or DMA. Correct.
- Reset handler happy path and parse path: the body is zeroed and freed right after
  parsing (`:494-496`) on every path past the OOM check. Correct.
- `totp_secure_zero` writes through a volatile pointer (`drivers/net/totp.c:10-16`),
  so the compiler will not elide it.

### Findings

**MED: reset handler leaves the plaintext new password on the stack on the
missing-field path** (`auth_totp_http.c:497-501`). `new_password[]` and `reset_token[]`
are filled by `http_form_find_field` before the `username_len < 0 || token_len < 0 ||
password_len < 0` check. The early return zeroes neither. Example: a body with
`new_password=` but no `reset_token` (or an over-long field, which returns -2 after
`http_form_url_decode` may have already written part of the value into the buffer)
leaves the full or partial new admin password on the httpd task stack until something
overwrites it. Fix: zero `reset_token` and `new_password` before that return.
`username` is not secret.

**LOW: forgot handler never zeroes the stack body after parsing**
(`auth_totp_http.c:348`, `360` through `437`). `body` holds `username=...&code=NNNNNN`.
It is zeroed only on the recv-failure path (`:353`). The normal path zeroes the parsed
`code` (`:410`) but leaves the raw copy in `body`. The code is short-lived (one TOTP
step plus skew) and single-use once consumed, so exposure is small. Fix: call
`totp_secure_zero(body, sizeof(body))` right after the two `http_form_find_field` calls,
which covers every later return.

**LOW: forgot handler missing-field return leaves `code` set** (`auth_totp_http.c:366-369`).
If `username` is absent but `code` is present, the unconsumed (still valid) code stays
in `code[]` and `body`. The same fix as above covers `body`. Zero `code` on this return
too.

**INFO: reset OOM path** (`:470-472`) returns 500 without `totp_backoff_record`. No
secret is involved, and an attacker cannot drive the heap to OOM cheaply. Noted for
consistency only.

Every other return path in both handlers that runs after a secret is materialized
zeroes it (`scratch`, `dummy` is all-zero by design, `token_raw`, `token_hex`, `resp`,
`admin_record`, `reset_token`/`new_password` on the main path).
