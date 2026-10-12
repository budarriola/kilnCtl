# Review: DEVBREAKFX17 (cc0441c70, bfc99014c, 52c0c46a6) -- 2026-10-12

This is a short, scoped review of the DEVBREAKFX17 commits on origin/dev, which fix REVIEW_DEVBREAKFX14:

- `cc0441c70`: raises the `safety_uart_bridge` stack from 4096 to 4608 B (LOW-3). It also refreshes the
  `bx_flash_worker`/`http_async_job` ceiling comments (LOW-2) and the LCDFX13 checkbuild wording (INFO-4).
- `bfc99014c` and `52c0c46a6`: add the fix-status section to REVIEW_DEVBREAKFX14 and then correct its SHA.

I reviewed them in worktree `C:\wt\rvdevbreakfx17_nvgyou`, detached at origin/dev. No negtest was run: a stack-size
bump has no meaningful mutation.

**Counts: HIGH 0, MED 0, LOW 2, INFO 3.**

## What checks out

- **Create and register sizes match.** `uart_bridge_safety.c` passes 4608 to both
  `xTaskCreatePinnedToCoreWithCaps(..., "safety_uart_bridge", 4608, ...)` and
  `stack_margin_register("safety_uart_bridge", ..., 4608)`.
- **The checker reads the new size.** `check_all_task_stack_budgets.py:765-767` extracts the declared stack from that
  create call by regex, so no hardcoded copy needed to change there. The ceiling stays at 3328 B, so honest free is
  now 4608 - 3328 - 300 = 980 B (21.3%), up from 468 B (11.4%).
- **ARCHITECTURE.md** now lists 4608. No other live copy of 4096 for this task remains outside history. The 4096
  values in `docs/BENCH_TEST_LOG.md` and `docs/audits/` are historical records and are correct to keep.
- **bx_flash_worker comment.** It says 6352 B (was 6336 B), honest free 3588 B (35.0%). That matches the 44289aa41
  ELF figures in REVIEW_DEVBREAKFX14: 10240 - 6352 - 300 = 3588, and 3588 / 10240 = 35.04%.
- **http_async_job comment.** The 7728 B total, the 5728 B `backup_import_job` chain and the 2212 B (21.6%) honest
  free are all right: 10240 - 7728 - 300 = 2212, and 2212 / 10240 = 21.60%. The one wrong figure is the
  "was" value (LOW-1).
- **Fix-status section (`bfc99014c`, `52c0c46a6`).** After the rebase correction it cites `cc0441c70`, which exists
  on origin/dev and contains all three fixes.

## Internal-RAM effect (INFO-1)

**Zero internal RAM.** The stack comes from `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. In ESP-IDF v6.0.2,
`xTaskCreatePinnedToCoreWithCaps` (`components/freertos/esp_additions/idf_additions.c`) allocates the stack with
`heap_caps_malloc(usStackDepth * sizeof(StackType_t), uxMemoryCaps)`. `StackType_t` is one byte on this port, so
the +512 B comes from PSRAM. `sdkconfig.defaults` has `CONFIG_SPIRAM=y` and
`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`.

The TCB comes from `pvPortMalloc` (internal RAM), but its size does not depend on the stack depth. The 8 KB
`heap_internal` floor is unaffected. PSRAM has ample room for 512 B.

## Findings

### LOW-1: the http_async_job comment gives the wrong previous value

`check_all_task_stack_budgets.py:1151` now reads "re-measured 7728 B (was 7632 B)". The previous ceiling and
measurement was 7712 B:

- `4d2a3eef4~1` has `"http_async_job": 7712`.
- REVIEW_DEVBREAKFX14 measures 7712 on the parent ELF of `44289aa41`.

7632 is the value from two measurements back. The fixer copied the "(was 7632)" from the old comment, which already
went stale when the ceiling moved from 7632 to 7712. The bx_flash_worker line got this right ("was 6336 B").

Fix: change it to "was 7712 B".

### LOW-2: the "4KB" comment in uart_bridge_safety.c still names the old size

`uart_bridge_safety.c:328` still ends the PSRAM-stack rationale with "It still should not fail for want of a
contiguous 4KB." The stack is now 4608 B, so this is the one remaining stale copy of the old size for this task.

Fix: say "4.5 KB", or drop the number ("for want of a contiguous stack-sized block").

### INFO-2: the ARCHITECTURE.md line citation for safety_uart_bridge is stale

The row cites `App/drivers/bridge/uart_bridge_safety.c:294`. The create call is at line 330. This predates the
fix, which only changed the size column. Neighbouring rows may be similarly stale. Not worth a commit of its own.

### INFO-3: the INFO-4 rewording is accurate, and verified

The original text of REVIEW_LCDFX13 (`6021e41b8`, line 108) read "A survey of `30301fbe48` found 110 alias groups".
So the survey used `C:\wt\checkbuild_30301fbe48`, which is the first row of the table. The new wording ("the
checkbuild directory in the first row of the table above") names exactly that.

I reproduced the survey read-only. I used the tip's `check_lcd_sink_callgraph.py` `parse()` alias capture on both
build directories:

| Build | Referenced symbols | Alias groups | In `drivers/` |
|---|---|---|---|
| `C:\wt\checkbuild_30301fbe48` | 22771 (same as the review's table) | 110 | 4 |
| `C:\wt\checkbuild_280d180149` | 22782 (since rebuilt; the review recorded 22779) | 110 | 4 |

The four `drivers/` groups are:

- `pid_fuzzy.c::clamp_gain`/`sanitize_base`
- `dashboard_json.c::hex$0/$1`
- two `safety_cfg_http.c` constant-table pairs

None of them is UI. So both the claim and its attribution hold.
