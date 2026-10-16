# REVIEW_XIPLAND: adversarial review of the xipland batch

Scope: `c6cf24244..4a0080fa1` on origin/dev, which is four commits:
- `6880bfefb`: KilnFW runs XIP from PSRAM instead of keeping the UART ISR in IRAM, and a refused fetch admission now reports `start_free`.
- `72aa07a15`, `49577afa9`: docs (BENCH_TEST_LOG xipland entry, XIP_PSRAM_INVESTIGATION status).
- `4a0080fa1`: `stack_budget_lib.py` ignores linker section-bound literals as call targets.

The board was not touched. Both target builds below were made for this review with `check_00_kilnfw_target_build.ps1`.

Result: HIGH 0, MED 1, LOW 4, INFO 9.

Fix status (xipfx, 2026-10-14): MED-1, LOW-1 and LOW-4 fixed; LOW-2 and LOW-3 not in this batch.
- MED-1: `check_00_kilnfw_target_build.ps1` runs `check_sdkconfig_defaults_applied.ps1` on the sdkconfig it is about to copy and fails the build on a pinned-key mismatch (fail rather than regenerate: a regenerated config would drop board-tuned values defaults does not pin). The pin check falls back to `build/sdkconfig`, then the main-tree sdkconfig, before it WARNs. `make_release.ps1` verifies the pins before the build and on the published `build/sdkconfig`. `build_kilnfw(target="build")` refuses on a mismatch (`reconfigure` stays reachable as the remedy). New check `check_sdkconfig_pin_guards.ps1`, new tests in `test_build_kilnfw_saftyfw_order.py`. The main-tree sdkconfig itself still needs regenerating (gitignored; coordinator step).
- LOW-1: `stack_budget_lib.py` keys on zero-size NOTYPE symbols with no FUNC symbol at the address; the name list is gone. Tests: `test_unnamed_zero_size_notype_marker_is_not_a_call_edge`, `test_func_symbol_at_the_same_address_keeps_the_entry_walkable`.
- LOW-4: `update_fetch_heap.h` draw table now carries the xipland C row (draw 15936 B) and states the "5 readings" rule against the current build. Comment only, no code change.

## Stack-budget comparison, parent (A) against tip (C)

The lander reported that `check_all_task_stack_budgets.ps1` passes at the tip with 0 of 35 tasks fully measured and all 35 INDETERMINATE. This review built both sides and compared them. The result is the same on both sides, so the xipland batch did not cause it.

Builds, provenance checked from each checkbuild's `build/config/sdkconfig.h`:

| Variant | Commit | Checkbuild | Config |
|---|---|---|---|
| A | `c6cf24244` | `checkbuild_8949119d8e` | `CONFIG_UART_ISR_IN_IRAM 1`, no XIP |
| C | `4a0080fa1` | `checkbuild_fe47888eca` | `CONFIG_SPIRAM_XIP_FROM_PSRAM 1`, ISR not in IRAM |

Both ELFs were published by check_00 at 11:23. Each was graded with both the old stack library (A's tree) and the new one (C's tree):

| Run | Verdict |
|---|---|
| A ELF, old lib | OK, 0 of 35 measured, 35 INDETERMINATE |
| A ELF, new lib | OK, 0 of 35 measured, 35 INDETERMINATE |
| C ELF, new lib | OK, 0 of 35 measured, 35 INDETERMINATE |
| C ELF, old lib | FAIL: `ota_confirm` 10384 B, `screen_idle` 11008 B, `lvgl` 14624 B over budget |

- **Lower bounds match exactly.** With the new library, the per-task lower bound and ceiling are identical between A and C for all 35 tasks. Examples: lvgl 7520 B, http_async_job 7728 B, bx_flash_worker 6352 B, safety_proto_rx 4576 B, ota_confirm 3760 B, screen_idle 3904 B. The library change also leaves every A number unchanged.
- **XIP did not break call-graph resolution.** Section virtual addresses are the same under XIP: `.flash.text` is at 0x42000020 and `.iram0.text` at 0x40374404 in both ELFs. Symbol names and types are the same. Under XIP the ELF gains only two ROM NOTYPE symbols, `Cache_Flash_To_SPIRAM_Copy` and `Cache_Get_IROM_MMU_End`.
- **"0 of 35 measured" is not new.** It is the documented, by-design state since 2026-09-09 (`check_all_task_stack_budgets.py:99-150`): every task's reachable graph contains a `callx`. The check grades the known lower bound against `CEILING_BYTES`, which equals the lower bound for most tasks, so it still fails on any growth of a resolved path.
- **The library change was required, not cosmetic.** Without it, C fails with a false +6624 B (ota_confirm) and +7104 B (screen_idle, lvgl). The cause is `spi_flash_cache2phys()` (IDF `flash_mmap.c:357`, under `CONFIG_SPIRAM_FLASH_LOAD_TO_PSRAM`), which loads `_stext` as a range-compare literal. objdump decodes the literal pool at `_stext` as an `entry` with a large frame. The filter hides no real edge: 0x42000020..0x420045fc (A) and 0x42000020..0x4200441c (C) contain no FUNC symbol. The first FUNC symbol is `console_start_select` in both.

Conclusion: no HIGH. XIP and the library change did not turn a measuring check into a vacuous pass. The check gives the same lower-bound-only assurance it gave on the parent. LOW-1 covers how fragile the name-keyed filter is.

## MED

### MED-1: builds without a worktree sdkconfig get neither link fix, and the pin check cannot see it

Files:
- main tree `firmware/KilnFW/sdkconfig` (gitignored, dated 2026-10-11)
- `firmware/KilnFW/App/test/check_00_kilnfw_target_build.ps1:283-292`
- `firmware/KilnFW/App/test/check_sdkconfig_defaults_applied.ps1:149-159`
- `tools/make_release.ps1:269-273`

**The problem.** The main tree is at `4a0080fa1`, but its generated sdkconfig still has `# CONFIG_SPIRAM_XIP_FROM_PSRAM is not set` and `# CONFIG_UART_ISR_IN_IRAM is not set`. Running `check_sdkconfig_defaults_applied.ps1 -SdkconfigPath <main tree sdkconfig>` fails on `CONFIG_SPIRAM_XIP_FROM_PSRAM`. ESP-IDF keeps the generated value over `sdkconfig.defaults`, so this config builds with both fixes off. That is the configuration `LINK_CRC_DURING_FETCH.md` showed drops kilnlink frames during a flash erase or write.

**Every build that falls back to the main-tree sdkconfig ships that configuration:**
- `check_00_kilnfw_target_build.ps1` copies the main-tree sdkconfig into a fresh worktree that has none (lines 283-292).
- `make_release.ps1` builds through check_00.
- MCP `build_kilnfw` builds in the main tree.

**Nothing in a standard run flags it:**
- In a fresh worktree, `check_sdkconfig_defaults_applied.ps1` finds no `firmware/KilnFW/sdkconfig` and exits 0 with a WARNING (lines 149-159).
- check_00 publishes the config it actually built with to `build/sdkconfig`, not the path the pin check reads.
- The ELF-reading checks then grade a non-XIP ELF and pass.

**Failure scenario.** A release (or a dev JTAG image) is built from a fresh worktree or the main tree after this landing. Every check is green. On the board, a GitHub fetch again produces kilnlink crc/len/timeout errors while the stage partition is erased and written. The bench trial never saw this because it used a hand-copied worktree sdkconfig (BENCH_TEST_LOG xipland entry, "worktree sdkconfig only, restored").

**Not introduced by this batch.** The main-tree sdkconfig was already stale against the 2026-10-13 `CONFIG_UART_ISR_IN_IRAM=y` pin. This batch makes a second key load-bearing without closing the gap.

**Fix:**
1. Regenerate the main-tree sdkconfig so that `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` (plus its FETCH_INSTRUCTIONS and RODATA dependents).
2. In check_00, run the pinned-key comparison against the sdkconfig it is about to build with, and `Fail` on any mismatch. make_release then inherits this.
3. Make `check_sdkconfig_defaults_applied.ps1` fall back to `build/sdkconfig`, or to the main-tree sdkconfig that check_00 would use, before it settles for a WARNING pass.

## LOW

### LOW-1: the section-bound filter matches by name, but three names alias the same address

Files: `firmware/KilnFW/App/test/stack_budget_lib.py:113-123`, `:427-428`

At 0x42000020 the ELF has three zero-size NOTYPE aliases: `_stext`, `_text_start` and `_instruction_reserved_start`. The filter lists the first two but not `_instruction_reserved_start`. It also does not cover `_rodata_reserved_start`, or any future section-bound symbol.

**Failure scenario.** A binutils update, or a symbol-order change, makes objdump caption the `l32r` literal `<_instruction_reserved_start>`. The bogus 7 KB edge returns as a false FAIL on ota_confirm, screen_idle and lvgl. The tempting "fix" is to raise those three ceilings by about 7 KB. That would silently hide 7 KB of real growth on the LCD task for good.

**Fix.** Never create a frame or a call target from a zero-size NOTYPE symbol. Key frames on `FUNC`-typed symbols from `symbol_sizes()`'s symbol table, rather than on a name list.

### LOW-2: the forced-panic coredump was never run under XIP

Files: `docs/audits/XIP_PSRAM_INVESTIGATION.md:154` (an acceptance item), `docs/BENCH_TEST_LOG.md:3392`, `docs/BENCH_TEST_LOG.md:3423` ("forced panic coredump SKIP")

The panic path disables the cache and writes the coredump to flash (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`). Under XIP, instruction fetch now goes through PSRAM, behind the same cache. IDF supports this, but this board has never exercised it.

**Failure scenario.** The next real panic leaves no readable coredump or crash report. The 2026-09-04 class of crash then goes undiagnosed.

**Fix.** Run one forced panic on a C build once a tool exists, for example a JTAG-triggered `abort` through `debug_*`. Until then, list it as an open item rather than closing the investigation.

### LOW-3: display_flush max of 574 ms under C has no matching A baseline

File: `docs/BENCH_TEST_LOG.md:3392`

The last documented A quiet maximum was 205 ms at uptime 193 s (`BENCH_TEST_LOG.md:2827`). The 574 ms reading was taken during a latency soak. Under XIP, LVGL draw buffers and instruction fetch now share the PSRAM bus.

**Failure scenario.** A real UI-latency regression, from PSRAM contention, is filed as noise because no A soak was taken under the same load.

**Fix.** Run the same 120 s `latency_soak` on A and on C, and compare the `display_flush_us` max and mean.

### LOW-4: the fetch-heap draw table still describes the ISR-in-IRAM build as current

File: `firmware/KilnFW/App/drivers/update/update_fetch_heap.h:20-50`

The table's BENCH-PENDING note ("only benchupd5 ran on this firmware") and its idle-board rows describe the ISR-in-IRAM firmware. The C readings are not in it:
- start free 44399 B
- low-water 28463 B
- draw about 15.9 KB

**Failure scenario.** A later sizing pass treats benchupd5 as the current build's only datum, and the 5-reading requirement is counted against the wrong firmware.

**Fix.** Add the C row and restate which build the "5 readings" rule applies to.

## INFO

- **INFO-1: the `heap_admit()` fix is correct** (`update_fetch.c:384-390`). It records the refusing reading only on a first admission (`!hop`) that has not already set `rtc_set`. The admitted path still overwrites it with the same value.
  - Negtest re-run, kilnfw-host, with the required ExpectPattern: the mutation `if (!hop)` to `if (0 && !hop)` was CAUGHT.
  - The baseline passed (146 s), so the mutant compiled.
  - The failure is the real assertion: `FAIL ...test_update_fetch.c:777: refused admission reports the refusing reading as heap.start_free, not 0`. It is not a build failure.
- **INFO-2: the four sdkconfig-pin mutants are real.** XIP line deleted, XIP set to not-set, ISR line deleted, ISR set to `=y`: each was CAUGHT by `check_sdkconfig_defaults_applied.ps1` with `CHECK FAILED`, from the missing-key guard (lines 124-128) or the `$mustBe` guard (lines 133-147). Both guards run before the generated-sdkconfig step, so the mutants are caught even with no sdkconfig present. That generated-sdkconfig step is the MED-1 gap.
- **INFO-3: the new stack-library test is real.** Replacing the filter with `if False:` was CAUGHT by `test_linker_boundary_literal_is_not_a_call_edge`.
- **INFO-4: the install job's UNKNOWN is explained, and it is not an XIP effect.** The installed app was `v1.0.0-pre.3` (`b9fa1748e`, 2026-10-12), which has no `/api/update/status` route. That route arrived on 2026-10-13 (`update_install_http.c:241`).
  - `mcp_server_update.py:824-880` ends deliberately as UNKNOWN when `/api/update/status` answers 404 while `/api/status` answers.
  - The apply ran in the recovery image, which has no XIP. The app it booted was pre.3, which also has no XIP.
- **INFO-5: the recovery image is unaffected.** `firmware/KilnFW_recovery` is a separate IDF project with its own `sdkconfig.defaults` (octal PSRAM at 40 MHz, no XIP). It does not build `hal_uart_esp.c`. Nothing in the batch touches it.
- **INFO-6: PSRAM headroom is ample.** A's peak PSRAM use was about 400 KB (SPIRAM min_free 7.84 MB of 8.24 MB, `BENCH_TEST_LOG.md:2828`). C leaves 5.37 MB after copying about 2.9 MB of text and rodata, so the margin is more than 4.9 MB.
  - C's SPIRAM min_free was not recorded. Record it at the next C soak.
- **INFO-7: const data in PSRAM does not change DMA behaviour.** Under XIP, `esp_ptr_external_ram()` is true for rodata (IDF `esp_psram.c:526`).
  - `spi_common.c:403-408` copies a non-DMA-capable external tx buffer into an internal DMA bounce buffer, which is what happened for flash rodata before. The project never sets `psram_dma_direct`.
  - `esp_flash_write` still copies a non-DRAM source through a 32 B stack buffer (`esp_flash_api.c:1116`).
  - No firmware call site uses `esp_ptr_dma_capable` or `esp_ptr_in_drom` (checked with grep).
- **INFO-8: the PSRAM-stack NVS-write refusal is now over-conservative** (`hal_kv_esp.c:471`). Under XIP, the cache stays on during SPI1 erase and write (IDF `spi_flash_os_func_app.c:32`, `SPI_FLASH_CACHE_NO_DISABLE`). This is harmless, so keep it.
- **INFO-9: where the UART ISR still stalls.** With the ISR not in IRAM, it still stalls on the paths that disable the cache: `esp_mmu_map` from `esp_image_verify`, stage verify, `recovery_switch.c:41`, and the panic path.
  - The bench fetch, stage and verify showed 0 link crc/len/timeouts over 445 frames.
  - No `ESP_INTR_FLAG_IRAM` registration in KilnFW calls code that has moved.
  - `io_bridge_isr` (`uart_bridge_io.c:68`) is `IRAM_ATTR`, but it is installed through `gpio_install_isr_service(0)`, the same as before.
