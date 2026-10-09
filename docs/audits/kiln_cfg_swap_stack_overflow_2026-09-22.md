# kiln_cfg_swap stack overflow -- 2026-09-22

## Defect

`POST /api/kiln_configs/apply` for config id 1 on ESP firmware `08f1c451`
panicked live on the bench (docs/COMMISSIONING_TEST_MATRIX.md W42 record at
`b024b33c`, `docs/BENCH_TEST_LOG.md` latest section):

```
Panic reason: ***ERROR*** A stack overflow in task kiln_cfg_swap has been detected.
```

Coredump: `firmware/KilnFW/coredump_archive/coredump-78fe1c6691c5.bin`.
ELF: `firmware/KilnFW/elf_archive/KilnCtrl-91efff0f77d4.elf`. Symbolized
read-only with ESP-IDF's `espcoredump.py` (`info_corefile`,
`--core-format raw`, the IDF-provisioned Python venv and matching GDB, per
`tools/PcTools/src/kilnctrl/coredump_fetch.py`'s own invocation).

### Backtrace summary

```
Crashed task handle: 0x3fcee40c, name: 'kiln_cfg_swap'
Panic reason: ***ERROR*** A stack overflow in task kiln_cfg_swap has been detected.
0x3fcee40c    kiln_cfg_swap      1/1           8176/8   <- STACK USED/FREE (of 8184 usable)
#0  panic_abort (...) at panic.c:471
Backtrace stopped: Cannot access memory at address 0x3fcf3e54
exccause 0x0 (IllegalInstructionCause), excvaddr 0x0, pc 0x4037ffc0 <panic_abort+16>
```

The crashed thread's own backtrace beyond `panic_abort` is unrecoverable --
the overflow itself corrupted the stack it would have to unwind. This is the
expected shape for a genuine FreeRTOS stack-overflow detection (8176 of 8184
bytes used, 8 free at detection), not a tooling gap.

## Root cause

`kiln_cfg_swap_apply()` (`firmware/KilnFW/App/drivers/persist/kiln_cfg_swap.c`),
the function `POST /api/kiln_configs/apply` runs on, carried roughly 4.25 kB
of large local aggregates on its own single stack frame:

- `uint8_t target_blob[896]` (`ZONES_CONFIG_BLOB_MAX_SIZE`)
- `kiln_pkg_safety_t target_pico` (~772 B: `uint16_t count` + `entries[96]` at 8 B each)
- `kiln_cfg_swap_pending_t p` (~1688 B: a `uint8_t rollback_blob[896]` plus a
  `kiln_pkg_safety_t rollback_pico` plus bookkeeping fields)
- `uint8_t readback_blob[896]`

-- already over half of the worker's 8192 B stack (`SWAP_WORKER_STACK_BYTES`,
`kiln_cfg_swap_worker.c`), before counting the function's several 200-byte
reason-string locals or the safety-link UART round-trip chain beneath
`push_and_verify_pico()`/`rollback()`/`persist_pico_flash_fallback()`.

This is the same class of frame this file has already been bitten by once:
`finish_esp_done()`'s own doc comment records that before it was split into
its own `noinline` function, boot recovery's combined switch-branch frame
measured at 8192 B on this SAME 8192 B worker stack, "with 112 B to spare" --
i.e. this task's margin was already known to be razor-thin, and the crash is
consistent with something on this call graph (the 60+ UART round trips to
the Pico plus flash writes noted in the task brief) growing enough since to
tip it over.

`kiln_cfg_swap_boot_recover()` carries an equivalent standalone
`kiln_cfg_swap_pending_t p` (~1688 B), and `finish_esp_done()` (called from
it) carries its own `target_blob`/`target_pico`/`live_blob` (~2.56 kB) --
all on the SAME worker task's stack, since `kiln_cfg_swap_boot_recover()`
runs as that task's first act before its job-queue loop starts.

## Fix (superseded -- see "Review update" below)

`target_blob`, `target_pico`, `p`, and `readback_blob` in
`kiln_cfg_swap_apply()`; `target_blob`, `target_pico`, and `live_blob` in
`finish_esp_done()`; and `p` in `kiln_cfg_swap_boot_recover()` were made
`static` instead of stack locals.

This is safe specifically because these three functions are the ONLY
callers of these large locals and all three run exclusively on the
dedicated `kiln_cfg_swap` worker task (`kiln_cfg_swap_worker.c`):

- `kiln_cfg_swap_boot_recover()` runs once, as that task's first act,
  before its job-queue loop begins.
- `kiln_cfg_swap_apply()` runs only from that same loop, serialized by a
  depth-1 queue plus `kiln_cfg_swap_worker_is_busy()`'s interlock -- at
  most one call is ever in flight.
- `finish_esp_done()` is called only from `kiln_cfg_swap_boot_recover()`.

So at most one of the three is ever executing at a time, and a single
static instance per large local cannot be shared between two concurrent
callers. Lighter-weight, non-deep-path call sites that ARE reachable from
other tasks (`kiln_cfg_swap_get_marker()`'s own `p`, `kiln_cfg_swap_is_pending()`,
`persist_marker()`'s callers, etc. -- e.g. the HTTP status route) were left
as ordinary stack locals; making those static too would introduce exactly
the concurrent-writer hazard the fix above avoids.

`SWAP_WORKER_STACK_BYTES` (`kiln_cfg_swap_worker.c`) is left at 8192 B, not
raised. The static-conversion fix alone dropped the measured static
call-depth ceiling to 4608 B (see below) -- 3584 B of honest headroom on the
unchanged declared stack before even subtracting the checker's
`UNMODELED_OVERHEAD_BYTES`, a wide margin over the "112 B to spare" this
same file's `finish_esp_done()` comment records for the pre-fix combined
frame. Raising the stack further was assessed and rejected as unnecessary
DRAM spend given that margin.

## New standing check

No existing check measured this task's static call-depth ceiling before
this pass: `check_stack_margin_registration.ps1` only confirmed
`stack_margin_register("kiln_cfg_swap", ...)` was called (registration,
not depth), and no dedicated `check_kiln_cfg_swap_*_stack_budget.py` file
existed. Added a `kiln_cfg_swap` row to
`firmware/KilnFW/App/test/check_all_task_stack_budgets.py`'s shared
`TASKS`/`CEILING_BYTES` tables (root `swap_worker_task`, stack size read
live from `SWAP_WORKER_STACK_BYTES` via `extract_local_macro()`, never
hand-copied) -- the sibling-task convention this table already uses for
tasks without their own dedicated checker file.

### Ceiling numbers

| | value |
|---|---|
| Declared stack (`SWAP_WORKER_STACK_BYTES`) | 8192 B (unchanged) |
| Measured static call-depth ceiling, pre-fix source rebuilt for this pass | over budget -- see negative test below |
| Measured static call-depth ceiling, post-fix (`--dump-ceilings` against a fresh worktree build) | 4608 B |
| New `CEILING_BYTES["kiln_cfg_swap"]` | 4608 |
| Honest free at that ceiling (before `UNMODELED_OVERHEAD_BYTES`) | 3584 B (43.8% of 8192 B) |

## DRAM impact

`.dram0.bss`/`.dram0.data`, read from `KilnCtrl.map` in the check's own
build worktree (`C:\wt\checkbuild_a0f49f2f8d`), same sdkconfig both times:

| | `.dram0.data` | `.dram0.bss` |
|---|---|---|
| Pre-fix (stack locals, rebuilt for this comparison) | 0x5b47 (23367 B) | 0x17008 (94216 B) |
| Post-fix (static locals) | 0x5b47 (23367 B) | 0x19148 (102728 B) |
| Delta | 0 | **+8512 B** |

`.data` is unaffected (these locals were always uninitialized). The `.bss`
growth (~8.3 KiB) is the five large aggregates (`target_blob`×2,
`target_pico`×2, `p`×2, `readback_blob`, `live_blob`) moving from the
worker task's own stack allocation into static storage -- internal DRAM
`.bss`, not PSRAM: `save_pending()`'s existing `hal_kv_write_safe_here()`
guard means this task must never run on a PSRAM-backed stack (it writes
NVS/flash), and that constraint is unchanged by this fix; these are ordinary
internal-heap statics like the rest of this module's own state
(`s_link`, `s_boot_fault`, etc.), not a new PSRAM allocation.

## Checks

`powershell -ExecutionPolicy Bypass -File tools\run_all_checks.ps1 -AllowSkips -AllowFewerChecks -Only "check_00_kilnfw_target_build|stack|kiln_cfg"`,
run against a fresh KilnFW target build in this worktree: **12 passed, 0
skipped, 0 failed**, including the newly-added `kiln_cfg_swap` row inside
`check_all_task_stack_budgets.ps1`.

### Negative test

1. Reverted the five `static` locals back to plain stack locals by hand
   (not `git checkout --`, since this is a shared tree elsewhere) and
   rebuilt KilnFW target from scratch in this worktree.
2. `check_all_task_stack_budgets.ps1` went red on exactly this task:
   ```
   check_all_task_stack_budgets: FAIL -- 1 of 30 tasks over budget: kiln_cfg_swap
     Fix by moving large locals off the named task's own stack (heap/static, per this codebase's established convention), not by enlarging the stack or raising the ceiling without a documented reason for accepting the new margin.
   ```
   (Every other of the 30 registered tasks stayed within its own ceiling.)
3. Captured the pre-fix `.dram0.bss`/`.dram0.data` numbers above from that
   same rebuilt map file.
4. Restored the five `static` declarations by hand from a saved copy of the
   fixed file (never `git checkout --`, per this repo's standing rule that
   it would discard any other session's WIP in this shared-source build
   worktree), then forced a full rebuild (`check_00_kilnfw_target_build.ps1`
   again, not merely a hand diff) and re-ran the full check selection above,
   confirming 12/12 green against the freshly rebuilt, restored source.

## Review update -- 2026-09-22, worktree `C:\wt\swapstack_yrljuq`

Review of the `static` fix flagged the permanent `.bss` cost above as large
for a board with a real DRAM-exhaustion history
(`project_esp_internal_dram_exhaustion`, sockets reset below ~11.9 kB free;
a 100-slot static once panicked Wi-Fi) and asked for the smallest permanent
footprint that still cannot fail mid-swap. Replaced the `static` locals with
one `heap_caps_malloc(sizeof(*scratch), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`
scratch struct allocated per job, freed before every return, inside a thin
wrapper (`kiln_cfg_swap_apply()`, `finish_esp_done()`,
`kiln_cfg_swap_boot_recover()` are now wrappers around `_impl()` siblings
that take the scratch as a parameter). Each wrapper fails loud
(`set_reason()` for `kiln_cfg_swap_apply()`, `latch_boot_fault()` for the
other two) and returns before any state-mutating step if the allocation is
NULL, so a failed allocation can never leave a partial swap. The task stays
on internal heap only (`MALLOC_CAP_INTERNAL`), unchanged from the `static`
approach, since it writes NVS. Serialization is unchanged from the section
above (depth-1 queue, `is_busy()` interlock, boot_recover runs once before
the job loop) and was re-confirmed against this worktree's `kiln_cfg_swap.c`
and `kiln_cfg_swap_worker.c`; the only other readers of this module's state
(`kiln_cfg_swap_get_boot_fault()`, used by `dashboard_http.c`/
`dashboard_status_http.c`, and `kiln_cfg_swap_worker_get_status()`, used by
`kiln_cfg_http.c`'s status route) read only their own small, mutex-guarded
fields and never touch the heap scratch, so no other task's read races the
allocation/free.

Measured on a side-by-side rebuild of the `static` source against the same
origin/main base (`f5a793ce`), same sdkconfig, in this worktree:

| | `.dram0.data` | `.dram0.bss` | DIRAM total |
|---|---|---|---|
| `static` locals (this doc's original fix) | 23367 B | 102664 B | 199698 B |
| `heap_caps_malloc`/free per job (this update) | 23367 B | 94168 B | 191202 B |
| Delta | 0 | **-8496 B** | **-8496 B** |

Declared task stack unchanged at 8192 B (`SWAP_WORKER_STACK_BYTES`). The
static call-depth ceiling shifted slightly with the new call shape (smaller
per-function frames, same call chain depth): measured 4656 B against a
freshly, fully rebuilt `KilnCtrl.elf` in this worktree (was 4608 B under the
`static` fix) -- `CEILING_BYTES["kiln_cfg_swap"]` in
`check_all_task_stack_budgets.py` updated to 4656 accordingly. Honest free
at that ceiling: 3236 B (39.5% of 8192 B, already net of
`UNMODELED_OVERHEAD_BYTES`).

Negative test repeated against this fix specifically: reverted
`kiln_cfg_swap.c` to the original, pre-any-fix source (`git show
76b78802~1:...`, plain stack locals, no `static`, no heap), forced a full
rebuild, and confirmed `check_all_task_stack_budgets.py` fails loud naming
`kiln_cfg_swap`:

```
total 8720 B; ceiling 4656 B; honest free -828 B (-10.1% of 8192 B)
FAIL: 8720 B exceeds the 4656 B ceiling for kiln_cfg_swap.
FAIL: honest free is negative (-828 B) once the 300 B unmodeled-overhead allowance is counted.
```

Restored the heap-allocation source by hand from a saved copy (never `git
checkout --`), forced another full rebuild (`idf.py fullclean` then `idf.py
build`), and re-ran
`tools\run_all_checks.ps1 -AllowFewerChecks -AllowSkips -Only "stack|kiln_cfg"`
against that freshly rebuilt, restored source: 11/11 passed.
