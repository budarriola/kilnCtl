# Stack analyser review: origin/dev c094c089 (2026-10-09)

Read-only review of `c094c089` ("LongCallTracker keeps union of l32r literals
and taints on non-l32r writes; sym+off resolves to base; re-baseline
executor/httpd ceilings; 4096 B stacks for ota_rollback_reboot and
danger_mode; persist scratch check covers adaptive_tune_model.c").

## Method

No target build was run (the dispatch rules forbid full target builds). Every
measurement below uses one ELF,
`C:\wt\stkmerge_maiigo\firmware\KilnFW\build\KilnCtrl.elf` (built 20:34 in the
worktree that committed `c094c089` at 20:56; the commit touches no source on
the measured paths except two task stack sizes, which the checkers read from
source). Each checker ran against that ELF from three trees:

| tree | check_all | executor | httpd | main |
|---|---|---|---|---|
| `0a1cdc7d^` (before long calls) | 0/33 measured, 33 INDETERMINATE | 1712 B, OK | OK | 5504 B, OK |
| `c094c089^` | 0/33, 33 INDETERMINATE | 3360 B, OK (LOW) | 4848 B, OK (LOW) | 6384 B, **FAIL** |
| `c094c089` | 0/33, 33 INDETERMINATE | 3920 B, OK (LOW) | 5248 B, **FAIL** (14.0% CRITICAL) | 6384 B, **FAIL** |

A scratch script diffed the call graphs `stack_budget_lib.parse()` builds
before and after the change on the same ELF. It found 3494 edges added, 0
removed, and functions marked indirect going from 930 to 1696 (of 10353).
`test_stack_budget_symbol_bounds` passes (16 tests). The bench board was read
once (`get_stack_margin`, read-only).

## Findings

### F1 HIGH: check_httpd_task_stack_budget fails at c094c089
`firmware/KilnFW/App/test/check_httpd_task_stack_budget.py:151`
(`CEILING_BYTES = 5248`), threshold at `:331`/`:353` (`honest_pct < 15.0`).

The new comment says "Honest free still above 10% of the declared stack". The
checker's CRITICAL bar is 15%, not 10%. 8192 - 5248 - 1800 = 1144 B = 14.0%,
so the check exits 1. **What goes wrong:** the next full `run_all_checks` on
dev (phase 1 builds the ELF) reports `check_httpd_task_stack_budget` as a
NEW failure. Every dev promotion is blocked, or someone lowers the 1800 B
overhead to make it pass. The 400 B that pushed it over is the
worker-only path in F2, so the right fix is F2, not a stack bump or new
ceiling.

### F2 MEDIUM: the executor and httpd re-baselines blame the wrong edge and count a path those tasks cannot run
`check_executor_task_stack_budget.py:150-156`, `check_httpd_task_stack_budget.py:148-151`;
source `firmware/KilnFW/App/drivers/persist/zones_config_store.c:890-897`.

Both comments say the union fix "reaches the __assert_func/panic_abort tail
it used to miss". That is false. The `c094c089^` paths already ended in
`vPortExitCritical -> __assert_func -> esp_system_abort -> panic_abort`. The
only new edge on both deepest paths is `nvs_save -> zones_autosave_job`. The
disassembly shows:

```
420513c6 l32r   a6, (42050c9c <zones_autosave_job>)   ; arg to run_on_flash_worker
420513e9 mov.n  a6, a10                                ; non-l32r write -> taint
42051434 s32i.n a6, a1, 0 ; memw ; l32i.n a6, a1, 0    ; the volatile pointer
42051441 callx8 a6
```

The old tracker dropped `a6` at the first write. The union keeps the literal
and adds the edge, marked tainted. That call is the deliberate
`void (*volatile job)` hiding place. It runs only when
`uart_bridge_ext_is_on_flash_worker()` is true. On `httpd_worker` and
`profile_executor`, `nvs_save()` only dispatches to the worker. The
executor's own comment, two lines up, still says the declared edge "applies
only to the bx_flash_worker task, NOT to this executor measurement". The new
number now contradicts that.

**What goes wrong:** the ceilings now include a ~1.3 KB worker-only frame
chain (`zones_autosave_job` through `populate_pico_half_and_hash`, 944 B).
That causes F1. A later real regression of up to ~560 B (executor) or
~400 B (httpd) on these tasks' own reachable paths would now pass under the
inflated ceiling.

**Fix:** add a per-root edge exclusion: the inverse of `DECLARED_EDGES`,
"`nvs_save -> zones_autosave_job` exists only under the bx_flash_worker
root". Then restore 3360/4848, or re-measure and state the real cause.

### F3 MEDIUM (pre-existing since 0a1cdc7d, kept): resolved long calls into ROM count 0 B and are no longer flagged indirect
`stack_budget_lib.py:358-364` (a resolved `lc` takes the branch that skips
`indirect`) and `deepest()` at `:465` (`if callee not in frames: continue`).

ROM functions have symbols but no disassembled body, so they are never in
`frames`. On this ELF, 6787 resolved, untainted `callx` sites have no target
in `frames`. That is 425 distinct targets, almost all ROM: `memcpy` 1514,
`memset` 1404, `__extendsfdf2` 579, `esp_rom_printf` 176, `crc32_le`,
`pp_post`, `cnx_node_search`, ... Before `0a1cdc7d` each of these set
`indirect[cur] = True`. Now each is a clean edge that adds 0 B with no
warning. **What goes wrong:** this is the dangerous direction. A
graded root whose real deepest leaf is a ROM call (`esp_rom_printf` in a
logging path, the Wi-Fi ROM `pp_post`) reports a total short by that ROM
frame and everything ROM calls below it. Nothing tells the reader that part
of the call is missing. **Fix:** when no resolved target is in `frames`,
treat the site as unresolved (set `indirect`). Or keep a small
ROM-frame table for the handful of ROM routines that are not leaves.

### F4 LOW (no instance today): an implicit clobber from callN/callxN is not modelled
`stack_budget_lib.py:116` (`"call"` in `_NON_WRITING`) and `:136-153`.

A literal held in `aN..a15` survives an intervening `callN`/`callxN` in the
tracker. In hardware, the call clobbers it with the return address or the
return value. Pattern: `l32r a10, cb ; call8 get_fn ; callx8 a10`
(`get_fn(cb)()` with no arguments). The tracker reports a clean edge to `cb`,
untainted. The real target, the returned pointer, is never followed and the
function is not marked indirect, so the under-count is silent. A scan of
this ELF found 0 such sites (a literal clobbered by a call and not reloaded
before the `callx`). **Fix:** on any `callN`/`callxN` with N>0, taint every
tracked register >= aN.

### F5 LOW: callx0 is neither followed nor flagged
`stack_budget_lib.py:96` and `:114`. Both regexes cover only `4|8|12`.

This ELF has 7 `callx0` sites, in `lv_draw_sw_blend_color_to_rgb565`,
`tcp_receive`, `pp_timer_process`, `esp_ipc_isr_handler` and `_stext`. A
function whose only indirect call is `callx0` reads as fully resolved, and
any stack the call0-ABI callee uses is dropped silently. In practice these
callees are small assembly leaves. **Fix:** add `0` to `CALLX_RE` so these
sites at least set `indirect`.

### F6 LOW: two instructions that write a register are in the non-writing list
`stack_budget_lib.py:116-117`. `xsr` writes its AR operand, and `s32c1i`
(which matches the `"s32"` prefix) writes its first operand. Neither taints.
No realistic call-target case exists. Listed for completeness, because a
missed taint is a missed `indirect`.

### F7 MEDIUM: the legacy parser's "sym+off resolves to base" reverses the file's own 2026-09-08 rule
`check_main_task_stack_budget.py:283` vs `:92-109`.

The `CALL_RE` comment explains why an offset caption is dropped. A windowed
call can only target an `entry` prologue, so `<foo+0xNN>` means the real
callee has no symbol, and objdump captioned it against an unrelated
preceding symbol. That is what fabricated the
`backup_export_get_handler -> _stext -> ... 6176 B` path. A `callx` target
obeys the same rule. Crediting `foo` fabricates a `caller -> foo` edge:
`_stext` is filtered by `SECTION_MARKER_NAMES`, but any other preceding
symbol is not. Data literals become names too (`UART2+0x7000` -> `UART2`).
These are harmless only while no frame of that name exists. It is also now
inconsistent with the address-keyed parser, which leaves an offset address
out of `frames`. **What goes wrong:** this errs toward over-counting, so
`check_main` can report a false FAIL through a function the caller never
calls. On this ELF, `main` did not change (6384 B both before and after).
**Fix:** skip offset captions as `CALL_RE` does, and report such sites as
unresolved instead of crediting 0 B silently.
`test_legacy_sym_plus_off_resolves_to_base` encodes the questionable rule.

### F8 LOW: the union credits every data literal as a call edge
`stack_budget_lib.py:136-141`, `:359-361`.

Of the 3494 added edges, most go to data or MMIO addresses
(`0x3fca....`, `0x6000....`, constants). These add 0 B because they are not
in `frames`. Some land on real code: `call_start_cpu0 -> _stext`,
`call_start_cpu1 -> _WindowOverflow4`. A function address loaded as a
callback argument, into a register later reused as a `callx` target, becomes
an edge. That is exactly how F2 arose. **What goes wrong:** this errs toward
over-counting, but it is the same fabricated-reachability class the 2026-09-08
`_stext` fix removed. **Fix:** add an edge only for a literal whose address
is a function entry (`addr in frames`). Keep the taint, so `indirect` still
covers the rest.

### F9 HIGH (pre-existing, not addressed): check_main_task_stack_budget fails on dev
`check_main_task_stack_budget.py` (budget 6144 B = 75% of 8192 B). It reports
6384 B at both `c094c089^` and `c094c089`, and 5504 B (OK) at `0a1cdc7d^`. The
path: `app_main -> profiles_http_start -> nvs_load_all_from (1744 B) ->
nvs_load_files_only (1440 B) -> ... cfg_fs_read (640 B) -> snprintf ->
... -> __assert_func -> panic_abort`. `c094c089` edited this parser but left
the check red. A full dev check run fails it. The two large `nvs_load_*`
frames are real boot-time stack and worth moving to the heap, whatever
happens to the tail.

### Q3 INFO: the INDETERMINATE flood is not a regression
All 33 tasks are INDETERMINATE at `0a1cdc7d^`, `c094c089^` and `c094c089`
alike on the same ELF. The per-task lower bounds are identical except
ordering. The extra 766 functions marked indirect change no task's
classification, because every task already reached some unresolved
`callx`. The check makes no pass claim and has not for some time. It still
grades each lower bound against its ceiling, so a deepening path still
fails it. The masking risk that matters is F3: resolved ROM calls that add 0
B without setting `indirect`.

### Q4 summary: the ceilings
Executor 3920 and httpd 5248 equal what the new walk measures, so they are
reproducible. They are dishonest about the cause and include a path that
cannot run on those tasks (F2). httpd 5248 fails its own checker (F1).

### Q5 LOW: the 4096 B bumps come from static lower bounds, not measurement
`ota_http_esp.c:829/:839`, `danger_mode.c:59/:366/:376`. Before the change,
the static figures were `ota_rollback_reboot` 2464 B of 3072 B (308 B
honest free, 10.0%) and `danger_mode` 2256 B of 3072 B (516 B, 16.8%). Both
are INDETERMINATE lower bounds with the 300 B overhead placeholder. Neither
check was failing. The `danger_mode.c:59` comment says "raised ... for thin
margin", but 16.8% passed. About 1.1 KB of `danger_mode`'s static path is the
first-use `esp_log` mutex-create and `__assert_func` tail. Live on the bench
(3072 B firmware), `danger_mode` shows 2292 B free at worst, 780 B used.
`ota_rollback_reboot` has never run on hardware (`check_all:1069`). The
`danger_mode` bump adds 1024 B of permanent internal DRAM against the owner's
8 KB internal-heap floor, unmeasured. The `ota_rollback_reboot` bump is
transient. Neither is harmful, but call them guessed.

**Resolved for `danger_mode` (2026-10-09): reverted to 3072 B.** Measured:
- Static, `check_all_task_stack_budgets.py` against a fresh KilnFW target ELF
  built from origin/main 129586d4 (origin/dev's tip did not build:
  `backup_import.c:3590` `-Werror=format-truncation`): 800 B walked
  (`danger_mode_task` 64, `safety_link_request_enable` 48, `safety_exchange` 32,
  `safety_drain_inbox` 64, `safety_drain_inbox_ex` 352, `safety_apply_fw_version`
  176, `safety_parse_fw_version` 64), ceiling 2112 B, honest free 1972 B (64.2%
  of 3072 B). Still INDETERMINATE (one unresolved indirect call), so 800 B is a
  lower bound; the pessimistic add-on is the ~1.1 KB first-use `esp_log`
  mutex-create / `__assert_func` tail above.
- Live, `get_stack_margin` on the bench board (read-only): 2292 B free at worst
  of 3072 B, 780 B used. The 2026-09-04 baseline JSON agrees (2320 B free, 752 B
  used).
- Worst case is about 800 + 1100 = 1900 B, leaving at least 38% of 3072 B free,
  far above the 15% critical line. The 4096 B bump had no measured basis and
  cost 1024 B of permanent internal DRAM against the 8 KB floor, so
  `danger_mode.c` (`xTaskCreate`, `stack_margin_register`, comment) is back at
  3072 B. The checker's `"danger_mode": 2256` ceiling row is untouched (another
  session owns that file) and still sits above the measured figure.
  `ota_rollback_reboot` stays 4096 B (transient, never run on hardware).

## Answers in brief
1. The union handles a literal loaded in one block and called in another
   after a forward branch. A backward-branch order or `l32r` then `mov` into
   another register falls back to `indirect`, which is safe. callx0 is
   unhandled (F5). Implicit call clobbers are unmodelled (F4).
2. It can still under-count: ROM targets (F3, real, 6787 sites), the
   call-clobbered return register (F4, theoretical), callx0 (F5), and
   `xsr`/`s32c1i` (F6).
3. The INDETERMINATE flood is not a regression (Q3). It was already 33/33
   before long-call tracking.
4. The ceilings reproduce the new walk's numbers, but the stated cause is
   wrong, they count a path those tasks cannot run, and httpd fails its own
   checker (F1, F2).
5. The 4096 B bumps rest on static lower bounds. No live data supports them
   (Q5).
