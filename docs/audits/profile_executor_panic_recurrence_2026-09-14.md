# `profile_executor` panic recurrence, 2026-09-14 — the 2026-09-11 fix held; this is a *different* event wearing the same signature

Follow-up to `docs/audits/fuzzy_first_hardware_run_2026-09-14.md` (`becebe8f`),
which found an unacknowledged crash report during the teardown of the first
hardware run of the fuzzy layer above `strength_pct = 0`, and to
`docs/audits/profile_executor_panic_2026-09-10_root_cause.md`, whose fix
(`a864a610`) this pass was asked to re-examine.

**Bottom line:** `a864a610` did **not** regress and nothing has been added to
the path it fixed. The crash is **not** byte-identical to the 2026-09-09 /
2026-09-10 pair. The static stack story that explained those two does **not**
explain this one, and the tooling needed to close it (a raw coredump read)
does not exist in this repo. Verdict: **insufficient margin / unresolved**,
not regressed — details and the honest limits below.

Commit hashes cited here were verified with `git cat-file -t`: `a864a610`,
`becebe8f`, `c8f7506b`, `7adf191b`, `88bb4333`, `8ac578a7`, `379f3fe6`,
`b64fe09d`, `2c49465a`, `137dea1a` — all `commit`.

## 1. The crash report, in full (read first, and NOT cleared)

`GET /api/crash_report`, read before anything else and re-read unchanged at
the end of this pass:

```
present=true, acknowledged=false,
exc_cause=0 (IllegalInstruction), exc_pc=0xfffffffd, exc_addr=0x00000000,
exc_a0=0x3fca0080, exc_a1_sp=0x3fcb3ae4, exc_task="profile_executo",
found_on_boot_reset_reason="PANIC",
frame_trustworthy=false, backtrace=["0xfffffffd"], backtrace_corrupted=true
```

`get_heap_status` at the time of this pass: `reset_reason='panic/exception'`
(unclean boot), `uptime_s=2430`, internal heap `free=68231 B`,
`min_free=34279 B` — no heap exhaustion.

**Shape: a stack-overflow-class fault, not a deliberate abort.** `exc_pc =
0xfffffffd` is espcoredump's `raw_pc - 3`, i.e. a saved PC of exactly
`0x00000000` — a return through a smashed `a0` (`crash_report.c`'s
`CRASH_REPORT_PC_OF_ZERO`). A `configASSERT`/`abort()` (the 2026-09-04
`safety_poll` shape) leaves a *valid* flash PC, so that shape is ruled out by
`exc_pc` itself. `exc_addr = 0x0` is a consequence of the same fact and
carries no independent information — it is the documented red herring and was
not chased.

**I did NOT acknowledge the crash report.** It is still `acknowledged=false`
and still present on the board, deliberately left for the owner. Nothing in
this pass depended on clearing it.

## 2. ELF match

`find_crash_elf()` resolves
`firmware/KilnFW/build/elf_archive/KilnCtrl-0965ca5755c1.elf`
(archived 2026-09-14T23:56:20Z, `git_commit c8f7506b`) — the flash performed
earlier that evening. All measurements below are against **that** ELF, never
`build/KilnCtrl.elf`.

## 3. Not byte-identical to the 2026-09-09 / 2026-09-10 pair

| field | 2026-09-09 | 2026-09-10 | **2026-09-14 (this one)** |
|---|---|---|---|
| `exc_pc` | `0xfffffffd` | `0xfffffffd` | `0xfffffffd` |
| `exc_addr` | `0x0` | `0x0` | `0x0` |
| `exc_a0` | `0x3fca0070` | `0x3fca0070` | **`0x3fca0080`** (+0x10) |
| `exc_a1_sp` | `0x3fcb2fe4` | `0x3fcb2fe4` | **`0x3fcb3ae4`** (+0xB00) |
| `exc_task` | `profile_executo` | `profile_executo` | `profile_executo` |
| backtrace | corrupted | corrupted | corrupted |

The 2026-09-09/10 pair were byte-for-byte identical in `a0`/`sp` **on the same
firmware build**, which is what made "same deterministic path, same
allocation" a sound inference there. This one is on a *different* build
(`c8f7506b` vs `79d93233`), so differing addresses are expected and prove
nothing on their own — but neither can the reverse be claimed: **there is no
longer positive address evidence that this is the same path.**

The record's dedup key (`crash_report_dump_id()`, `crash_report.c:209-233`)
is built from `exc_pc` + `exc_task` + the backtrace array only — all three
identical across the three instances. So the dedup hash matching carries **no
information** about whether the underlying path is the same; it matches by
construction for any panic of this shape in this task.

## 4. The 2026-09-11 fix is present and has not regressed

`a864a610` is an ancestor of the running commit `c8f7506b`
(`git merge-base --is-ancestor` → yes).

`check_executor_task_stack_budget.py` run against the **confirmed-matching
running-image ELF**:

```
deepest static stack path from executor_task_entry: 1936 B (ceiling 1936 B of a 4096 B profile_executor stack)
  executor_task_entry -> escalate_guard_trip -> release_profile_relay_claim
  -> heat_enable_release -> safety_link_request_enable -> safety_exchange
  -> uart_protocol_send_broadcast -> frame_and_send$constprop$0
  -> hal_uart_send_blocking -> uart_write_bytes -> uart_tx_all$part$0
  -> uart_enable_tx_write_fifo

naive implied free: 2160 B
honest free (naive - 1220 B unmodelled overhead): 940 B (22.9% of 4096 B) -- classified LOW
check_executor_task_stack_budget: OK
```

This is **exactly** the post-`a864a610` figure: 1936 B deepest, 940 B honest
headroom, LOW. The `safety_apply_fw_version` / `safety_apply_diag` side-effect
branches the fix removed are still gone. The measured deepest path lands
precisely *on* `CEILING_BYTES`, which means **nothing has been added to that
path since the fix** — if anything had, the check would be red.

### The three named suspects, individually

| commit | on the board? | effect on `profile_executor`'s stack |
|---|---|---|
| `7adf191b` (divergence hooks calling `profile_executor_halt()`) | **yes** | **none.** The hook fires from `safety_ceiling_sync_reconcile_on_link_up()`, whose only production caller is `safety_link_poll.c:297` — i.e. it runs on the **`safety_poll`** task (8192 B, live HWM 4732 B free / 57.8% headroom), not on `profile_executor`. `profile_executor_halt()`'s own deepest path is 1696 B; 3460 B used + 1696 B still fits 8192 B. Suspect cleared with evidence. |
| `88bb4333` (SIMC sole gain writer) | yes | none measurable — the executor path's deepest is unchanged at the 1936 B ceiling. |
| `8ac578a7` (bucketed `adaptive_tune` window) | **NO** | **not on the board.** `git merge-base --is-ancestor 8ac578a7 c8f7506b` → no; it lands after `c8f7506b` on `main`. It cannot be implicated. |

`becebe8f` is likewise **not** on the board — it is the audit-document commit
for the run; `fuzzy_strength_pct` was changed over HTTP, not by a flash. The
fuzzy code itself was already in `c8f7506b`; its executor-side entry
(`pid_fuzzy_prepare_gains` → `pid_fuzzy_adjust`) measures **240 B** deep, so
enabling it did not materially change this task's stack profile either.

## 5. The "84 seconds after idle" is a misreading — the panic is at run teardown

The source of that figure is `fuzzy_first_hardware_run_2026-09-14.md`'s own
crash block, which records `uptime_s=84 (at time of read)`. **`uptime_s` is
time since the panic reboot**, not the interval between idle and the panic
(project memory: "reset_reason names the current boot"). Corrected reading:
the board panicked and rebooted, and the crash was discovered **84 s later**.

So there is no 84-second-post-idle mystery and no post-run hook to hunt. The
panic is in the run-teardown tick. Nothing on the executor task has a ~84 s
period: the only periodic flash write on this task is
`relay_cycles_maybe_persist()`, whose interval is
`RELAY_CYCLES_PERSIST_INTERVAL_S = 600` (`relay_cycles.h:38`).

What actually runs in the teardown tick, from `profile_executor.c:335-383`
(the `state != RUNNING` branch, entered the first tick after the run reaches
`DONE`), with each call's deepest path measured on the running ELF:

| call | cumulative depth from `executor_task_entry` |
|---|---|
| `force_all_relays_off()` | 896 B |
| `io_segs_force_all_off(false)` | 864 B |
| `heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE)` | **1840 B** |
| `firing_stats_maybe_finalize()` + `firing_stats_persist()` | 1696 B |
| `adaptive_tune_run_end()` | 1776 B |

They run sequentially, so the tick's peak is ~1840 B, under the 1936 B
ceiling. `adaptive_tune` was `enabled=False` on all three zones for this run
(recorded in the run audit), so `adaptive_tune_run_end()` early-outs and its
1776 B branch was not taken at all.

## 6. What could not be established, and why

* **Whether `exc_a1_sp` is actually outside `profile_executor`'s stack.** This
  is the one measurement that would turn "overflow-shaped" into "proven
  overflow", and it was attempted: `s_exec` resolves to `0x3fcafcc4` in the
  running ELF, and JTAG reads (`debug_read_memory`) of that region and of the
  `0x3fcb2000`–`0x3fcb4000` DRAM band were performed live. The band contains
  **two** distinct `0xa5a5a5a5`-filled (FreeRTOS-fill, untouched) regions with
  heap block headers at `0x3fcb2200` and `0x3fcb3800`, and neither could be
  attributed to `profile_executor` specifically without walking the TCB —
  `s_exec`'s `TaskHandle_t task` is at `profile_executor_internal.h:662`, deep
  inside a large struct whose byte offset was not resolved in this pass.
  Stated as unresolved rather than guessed.
* **The real backtrace.** There is **no tool in this repo to extract the raw
  coredump partition** — `kiln_find` over the whole 151-tool surface returns
  only `find_crash_elf`, `debug_read_memory`, `debug_read_symbol` and
  `debug_check_partition_table`; nothing runs `espcoredump.py info_corefile`.
  The 2026-09-09 audit's §7 recommended exactly this read and it has still
  never been performed. **This is the single highest-value missing capability
  for this bug class**, and it is destroyed by the next panic or the next
  `POST /api/crash_report/clear`.
* `debug_read_symbol(peer="esp", ...)` fails outright: *"peer 'esp' has no nm
  tool configured for symbol lookup"* — a real, small MCP-tooling defect (the
  ESP toolchain's `xtensa-esp32s3-elf-nm` is present and was used by hand in
  this pass to resolve `s_exec`). Worth fixing.

## 7. Verdict: not regressed; insufficient, and now partly unexplained

* **Regressed — ruled out.** The fixed path is gone, the ceiling is unchanged
  at 1936 B, and the check is green on the running image.
* **Unrelated — not supported.** Same task, same fault shape, same phase of
  the run (teardown) as the two prior instances. Three panics of this exact
  shape in this exact task across three different builds is not coincidence.
* **Insufficient — the supported reading.** `a864a610` bought real, structural
  margin (2784 → 1936 B deepest; 92 → 940 B honest headroom) and a panic still
  occurred on the same task in the same phase. That means either the 1220 B
  `UNMODELED_OVERHEAD_BYTES` constant — which the checker's own docstring
  admits is a live measurement taken under a *different, lighter* workload,
  not a guaranteed ceiling — understates the real ISR/window-spill cost on the
  teardown tick, or the fault is not a plain self-overflow of this task at all
  (the adjacent-stack-corruption class already recorded in this tree for the
  2026-09-04 `safety_poll` panic). **The static walk cannot distinguish these
  two, and no measurement in this pass could either.**

## 8. Recommendation (nothing applied — deliberately)

No firmware change was made. A rushed edit on a panic path with an unexplained
fault is worse than a precise diagnosis, and the two candidate fixes point in
different directions:

1. **First, build the missing tool** — a coredump extractor
   (`espcoredump.py info_corefile` against the archived ELF, wrapped as an MCP
   tool). It is the only thing that turns this from inference into fact, it
   costs no bench time, and the current coredump is still intact and
   unacknowledged on the board *right now*. Do this **before** anything that
   panics the board again or clears the report.
2. **Then**, if it confirms a self-overflow: take option 2 of the 2026-09-10
   report, which `a864a610` only half-took. `heat_enable_release()` called from
   the executor tick still reaches `safety_exchange()` →
   `uart_protocol_send_broadcast()` → `frame_and_send$constprop$0()` (1232 B)
   **synchronously** — the fix deferred the *side-effect* sends but not the
   *request* send. Deferring that to `safety_poll` (8192 B, 4732 B free) as
   well would take this task's deepest path from 1936 B to roughly the
   `adaptive_tune_run_end` branch at 1168 B.
3. **Only if that is not viable**, raise this task's stack from 4096 B. It is
   the outlier — every other task sharing the frame-send chain runs on 8192 B —
   but the repo's standing guidance is right that a stack bump hides the next
   regression, so it should be the fallback, with the reason stated loudly in
   `profile_executor_start.c`, not the first move.
4. Do **not** start another multi-hour firing on this build until (1) is done —
   a second panic destroys the coredump that is currently sitting on the board.

## 9. Checks run

* `tools/run_all_checks.ps1` (`-ExecutionPolicy Bypass`): **94 passed, 0
  skipped, 0 failed** — including `check_executor_task_stack_budget.ps1`.
* KilnFW host tests (`firmware/KilnFW/App/test/build_host_tests.ps1`): every
  suite built and ran with no test failure, but the script exits non-zero on
  `MISMATCH: 44 executables built but 42 were expected`. This is **not** a
  failure caused by this pass: `$totalExpected = 42`
  (`build_host_tests.ps1:1719`) is a stale hardcoded count, left behind by
  concurrently-landing test executables (the untracked `cfg_fs_test_*` /
  `cfg_fs_status_test_*` build directories in the working tree). It is the
  known "splits break filename-keyed checks" class. Left for the owning
  session to bump rather than edited here, since that file is shared and
  another pass is mid-flight in it.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
