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
