# Internal heap margin on dev 8fcd3237 (research, 2026-10-09)

Method: code reading plus the `.map` of a fresh checkbuild (`C:\wt\checkbuild_ccbb638618`, 103b7649). One read-only `get_heap_status` call; no board writes, no new build. Boot-time heap-stage numbers were not recoverable (device log ring has rolled over), so item 2's "what grew since 46083" is NOT answered; see Open.

## 1. "BELOW THE 20K DRAM FLOOR"
- Printed by `main_heap_stage()` (`App/main.c:~101`) at every boot heap stage when `free8 = heap_caps_get_free_size(INTERNAL|8BIT) < KILN_DRAM_FREE_FLOOR_BYTES (20480)` (`drivers/common/dram_margin.h:159`).
- It measures total free internal DRAM at that boot stage (not min_free, not DMA). The bench saw 15819.
- Not a stale-number false alarm: `KILN_DRAM_FREE_KNOWN_BYTES` is 46083 (measured 2026-09-05), so boot free fell ~30 kB since. The "DRAM REGRESSION" line should also be printing. The 20480 floor is the owner's 2026-08-27 policy; the 8192 B `min_free` rule (2026-10-01) is a different, looser metric. Both can be true: boot free 15.8 kB, runtime low-water 11.7-14.4 kB, still >= 8192.
- Current idle read (get_heap_status): free 29815, largest 10752, min_free 14371, dma min_free 6583, largest low-water 7936 at uptime 7 s.

## 2. Static consumers (KilnCtrl.map)
- `.dram0.data` 0x5f37 (24.4 kB), `.dram0.bss` 0x18c38 (101.4 kB), `.ext_ram.bss` 0x1e9e4 (125 kB, already PSRAM).
- Largest app .bss objects still internal: `kiln_cfg_store.c s_store` 17132 B; `lvgl_port.c s_lvgl_task_stack` 10240; `main.c ctx` 6208; `ota_http_pico.c s_ota_pico_chunk` 4096 and `ota_http_esp.c s_ota_esp_chunk` 4096; `profile_executor.c s_exec` 3520; `ui_theme.c s_touch_groups` 2176. Everything else in app code is < 2 kB; the rest of the 101 kB is IDF/Wi-Fi/lwIP.
- Heap-side (not in map): task stacks via plain xTaskCreate (e.g. http_async_job 10240), and any allocation <= 8192 B, because `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192` forces those into internal DRAM regardless of `heap_caps_malloc` default; `persist_scratch_alloc` asks for SPIRAM explicitly so it is exempt. `MALLOC_RESERVE_INTERNAL=32768` further reserves internal for DMA/internal-only requests.
- Internal and DMA-capable pools are nearly the same region (total 296299 vs 288131), so DMA min_free tracks internal consumption.

## 3. Backup import and DMA min 3911 B
- Import runs on `http_async_job` with a 10240 B stack from plain `xTaskCreate` (`backup_import.c:4417`, `http_async_job.c:122`): this single allocation takes the largest internal block (dram_watch: 10752 -> small), which is the largest-block low-water of 7936 B < 8704 (SK-04) and a main contributor to DMA min.
- Body, plan, candidate arrays and precheck scratch are already PSRAM (`heap_caps_malloc(SPIRAM)` / `persist_scratch_alloc`). Remaining internal fallbacks: `scratch` and zone/timing candidate arrays fall back to `malloc()` only on PSRAM OOM; `ctx` is tiny. cJSON nodes (<= 8 kB each) land internal by the ALWAYSINTERNAL rule, so a maximal backup's parsed tree is likely the rest of the ~8 kB dip. Not measured.
- The job stack CANNOT be moved to PSRAM: the job reaches NVS/flash writes with the cache disabled, and a PSRAM-stack task asserts (`esp_task_stack_is_sane_cache_disabled`, the 2026-08-31 autotune panic documented in `autotune_engine_step_identify.c:423`).

## 4. Options, ranked by bytes then risk
1. Move `kiln_cfg_store.c s_store` (17132 B) to `EXT_RAM_BSS_ATTR`: +17 kB boot free, restores the floor and the largest-block (static BSS leaves the contiguous region). RISK MEDIUM, NOT DONE: its blob is handed to the NVS/cfg-fs save paths, and PSRAM write-source pointers while the cache is disabled is exactly the class that panicked before; needs a code-path audit of every save that passes `&s_store` directly (use a persist_scratch snapshot copy) plus `check_kilnfw_dram_bss_budget.py` ceiling lowered. Bench verification of a kiln_configs save is required.
2. Make the import's job stack smaller via measured high-water (stack_margin) instead of 10240: each 1 kB frees directly from the largest block. Risk LOW if measured, needs a bench import run to measure; not possible without the board.
3. Statics: `main.c ctx` (6208) and the two OTA 4096 B chunk buffers (8 kB): chunk buffers are write sources for flash and must stay internal (see persist_scratch.h). `main.c ctx` needs a look at what it holds before moving.
4. Lower `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` from 8192 to e.g. 2048 (global): large saving for cJSON/etc, but moves every 2-8 kB allocation to PSRAM including ones used with cache disabled or by DMA drivers; RISK HIGH, owner decision.
5. The 20480 floor and 46083/18432 "known" constants are stale policy: after the real cause is fixed, rebaseline `KILN_DRAM_*_KNOWN_BYTES` from a boot log (do not raise them to silence).

## Open
- What grew boot free 46083 -> ~15.8k since 2026-09-05 is not identified; needs a boot `heap stage` log series (get_device_log right after reboot) and comparison against a build from early September (DRAM_PSRAM_PLAN.md). Candidates by size: http_async_job-like 10 kB stacks, 100-slot profile work, Wi-Fi/mbedtls changes.
- No code change made: nothing met "clear, low-risk persist_scratch move".

## 5. What grew boot free from 46083 B to ~15.8 kB (static ELF diff plus stack/config attribution)

Method: `xtensa-esp32s3-elf-size -A` and `nm -S` on archived flashed ELFs (earliest archived is 2026-09-16 ELF-sha256-prefix 1204ef14664b; nothing is archived for 09-05..09-16) versus the newest ELF-sha256-prefix d28019a44fa3 (2026-10-09), plus `git diff 402ab01a5123..origin/dev` (402ab01a5123 = last commit before the end of 2026-09-05) on stack literals and `sdkconfig.defaults`. No board access. The 09-05 baseline and today's 15.8 kB were not necessarily taken at identical boot stages; a boot `heap stage` log series is still the only exact proof.

Totals (static, 09-16 ELF to 10-09 ELF): `.dram0.bss` 90104 -> 100488 (+10384), `.dram0.data` 23335 -> 24359 (+1024), `heap_start` moved +11664 B. The net per-symbol sum of internal data/bss growth is +11310 B. So **static growth explains about 11.7 kB of the ~30 kB**. The remaining ~18 kB is runtime: internal-RAM task stacks created at or near boot. The ELF dated 09-20 (ELF-sha256-prefix f3efd5d89f49, bss 156824) is a sim/test build; ignore it. Static bss was flat 09-24..10-08 (~99-100 kB), so no hidden step change in statics in between; the 10-09 step is only +944 B of data.

### 5a. Ranked table

"Boot" means alive at the app_main_done measurement.

| # | Bytes | Symbol / task | Commit, date | Boot-resident | Can move to PSRAM? |
|---|-------|---------------|--------------|---------------|--------------------|
| 1 | 8192 | task `kiln_cfg_swap` stack (`SWAP_WORKER_STACK_BYTES`, plain xTaskCreate, `kiln_cfg_swap_worker.c`) | bf814e49, 2026-09-18 | yes, never exits | NO: reaches NVS/cfg-fs writes with cache disabled (`esp_task_stack_is_sane_cache_disabled`). Can only shrink to measured high-water (`stack_margin`) |
| 2 | 8192 | task `pico_auto_update` stack (`PICO_AUTO_UPDATE_TASK_STACK`, `pico_auto_update_boot.c`) | 008fd353, 2026-09-18 | transient (self-deletes) but alive in early boot stages | NO (flash reads/writes and UART to Pico). Affects boot-stage readings, not steady state |
| 3 | 5120 | task `ota_confirm` 3072 -> 5120 (`main_network_http.c`) | bump is in the 09-05..10-09 diff, commit not isolated | yes until boot confirmed healthy | NO (NVS write in the boot_guard clear) |
| 4 | 2048 | `profile_executor` stack 4096 -> 6144 | 2a107fb6, 2026-09-24 | yes | NO (autotune panic precedent, `autotune_engine_step_identify.c:423`) |
| 5 | 2048 | `s_lvgl_task_stack` 8192 -> 10240 (static, inside the +11.3 kB) | eb83c1ac, 2026-09-30 | yes | Maybe: needs `xTaskCreateStaticPinnedToCore` with an EXT_RAM array and proof LVGL never runs a cache-off path on that task; display flush buffers must stay internal DMA. Medium risk |
| 6 | 2048 | `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 8192 -> 10240 | 0a1cdc7d, 2026-10-09 | yes | NO (IDF-created, internal; app_main touches NVS). Landed after the 15.8 kB bench read |
| 7 | 1904 | `ui_theme.c s_touch_groups` 272 -> 2176 | UI work, 2026-09 (not isolated) | yes | Probably yes (UI-only data); verify not read with cache off |
| 8 | 1846 | `s_bulk_pairs` (new, kiln-config apply transaction) | c2c9eff2 2026-09-14, moved by 3e0b2fa4 2026-09-16 | yes | Only with a snapshot copy to any save path (worker writes NVS) |
| 9 | 1464 | `fst_storage` (static-ified telemetry_log local) | d4dfd75c, 2026-09-23 | yes | Check first: it may be a flash write source |
| 10 | 1080 + 700 | `s_scan_buf`, `scan_results$1` (Pico auto-update / scan) | 008fd353, 2026-09-18 | yes | Yes if not DMA; verify |
| 11 | 1024 each | stack bumps: `system_uart_bridge` 3072 -> 4096 (0a1cdc7d, 10-09), `touch_uart_bridge` 3072 -> 4096 (834c3841, 09-24), ~~`danger_mode` 3072 -> 4096 (c094c089, 10-09)~~ reverted to 3072 in 473f597e (measured 800 B) | as listed | yes | Possible via `...WithCaps(SPIRAM)` if the task never reaches flash/NVS or a cache-off path; `touch` and `info` bridges already use WithCaps (check which caps), `system_uart_bridge` uses plain xTaskCreate |
| 12 | 512 | `info_uart_bridge` 3584 -> 4096 | eb83c1ac, 2026-09-30 | yes | Already WithCaps; check caps |
| 13 | 960 + 960 + 768 | `s_login_lockouts`, `s_totp_lockouts`, `s_web_auth_table` | a3b59e9c 09-20, dcca2386 09-28, ca7a7d31 09-17 | yes | Yes (RAM-only tables); ~2.7 kB total |
| 14 | 904 + 600 + 592 + 576 | `s_gate`, `s_save_lock`, `s_stage`, `s_reply_slots` | 40f5635b 10-05 and the 10-09 save-mutex commits | yes | Mutex/StaticSemaphore storage must stay internal; plain data only |
| 15 | 616 | `main.c ctx` 5616 -> 6232 | various | yes | Needs a look (section 4 option 3) |
| 16 | 348 + 368 | `s_exec` 3172 -> 3520, `s_profile_rev` 32 -> 400 | profile executor / rev-floor work, 2026-10 | yes | `s_profile_rev` is persisted: keep internal |

Offsetting shrinkage in the same diff: `s_profiles` -3396, `adaptive_tune_zones` -2700, `extra_names` -2304, `pico_fields`/`esp_fields` -2328 (moved or removed); already in the +11310 net.

### 5b. Arithmetic and confidence

- Static net: +11.7 kB (measured from the ELFs).
- Persistent boot stacks added or grown since 09-05 and not in `.bss`: `kiln_cfg_swap` 8192, `profile_executor` +2048, `touch` +1024, `info` +512, about 11.8 kB. The 10-09 bumps (`main` +2048, `system` +1024) landed after the 15.8 kB bench read, so they are not in it but will lower the next read by ~3 kB. (`danger_mode` +1024 was also bumped then, but reverted to 3072 in 473f597e, so it contributes nothing.)
- Transient but visible at early boot stages: `pico_auto_update` 8192 and `ota_confirm` 5120 (~13.3 kB). They count if app_main_done runs while they are alive and the 09-05 baseline did not have them.
- 11.7 + 11.8 = 23.5 kB derived; the gap to ~30 kB is plausibly the transient pair, Wi-Fi/lwIP/mbedTLS internal pools, and the 09-05..09-16 window with no archived ELF. `sdkconfig.defaults` added no internal-RAM growth: the new WiFi/lwIP lines (`STATIC_RX_BUFFER_NUM=10`, `RX_BA_WIN=6`, `TCP_OOSEQ_MAX_PBUFS=4`, a513aa75) pin the bench-hand-set values, and the mbedTLS lines (`EXTERNAL_MEM_ALLOC`, `DYNAMIC_BUFFER`, `DYNAMIC_FREE_*`, 40f5635b, 10-05) reduce internal use. `KILNCTL_ENABLE_GPIO_PROBE=y` was pinned in the same commit; its task stack is not in this table (not measured).
- Biggest safe lever: shrink `kiln_cfg_swap` and `pico_auto_update` to measured high-water (each 8 kB, cannot go to PSRAM). Needs a bench run to measure, so deferred.
- Cheapest PSRAM candidates that avoid flash paths: web-auth tables (~2.7 kB), `s_touch_groups` (1.9 kB), scan buffers (1.8 kB); audit each against the persist_scratch.h rule (never pass a PSRAM pointer as a flash write source).
- To close the remaining uncertainty: take a `heap stage` boot-log series on the current build and on one built from `402ab01a5123` in a clean worktree.

### 5c. PSRAM moves applied (no board, no target build)

Moved to `EXT_RAM_BSS_ATTR` (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`; host stub defines it empty): `s_login_lockouts`, `s_totp_lockouts`, `s_web_auth_table` (~2.7 kB; plain data, locks are separate handles), `s_touch_groups` (1904 B; LVGL task only), `s_scan_stage`, `records[20]` in `do_scan` (driver memcpy target, not DMA), `scan_results[20]` in `wifi_prov_link.c` (~3 kB together). Expected `.dram0.bss` reduction about 7.6 kB; NOT measured (target build is not run by this pass, rules forbid it) -- measure with `xtensa-esp32s3-elf-size -A` on the next full build and ratchet `check_kilnfw_dram_bss_budget`.
Left alone: `pico_image_source.c s_scan_buf` (passed straight to `esp_partition_read`). `kiln_cfg_swap` worker is a persistent queue worker (serves on-demand applies), so it cannot self-delete without a lazy-spawn redesign; flash writes, so not PSRAM. `pico_auto_update` uses dynamic `xTaskCreate` + `vTaskDelete(NULL)`, so its 8 kB stack is freed after the idle task reaps it.
