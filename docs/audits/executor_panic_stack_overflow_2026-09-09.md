# `profile_executor` panic, 2026-09-09 10:38 — diagnosis

Follow-up to `docs/audits/cplval75_aborted_executor_panic_2026-09-09.md`
(commit `11af64cc`), which recorded the panic but explicitly left the cause
open. Read-only pass: nothing was flashed, no firing was started, and the
crash report was **not** acknowledged (it is still `acknowledged=false`, so
`capability_preflight` keeps refusing runs, and the `coredump` partition
image is still unerased — see "The one measurement" below, which depends on
that).

**Bottom line:** the evidence supports a **stack overflow of the
`profile_executor` task on its run-end / guard-escalation path**, not a
numerical fault in the control law. It does not yet identify *what ended the
run* at ~397 s. One measurement, named at the end, settles both halves.

---

## 1. Symbolization: refused, and why

The running image is `e8cfe344`, `fw_build = "Sep  9 2026 07:52:22"`
(`GET /api/status`), committed `2026-09-09T07:51:19-07:00`.

**No archived ELF matches it.** Checked three independent ways over all 104
files in `firmware/KilnFW/build/elf_archive/` plus `build/KilnCtrl.elf`:

* `build_info.h`'s `FW_BUILD_TIME` (`"%H:%M:%SZ"` UTC, so `14:52:22Z`) —
  zero matches; the archive jumps from `04:41:42Z` (2026-09-08 21:42 local)
  straight to `15:53:43Z` (2026-09-09 08:54 local).
* `esp_app_desc_t`'s own `time` string (`07:52:22`) — zero matches.
* The commit string `e8cfe344` — zero matches.

So **nothing was symbolized.** (Note for future passes: an early scan here
used `strings`, which is not installed in this environment and exits
non-zero into an empty capture — i.e. it reported "no matches" vacuously.
The results above come from reading the ELF bytes in Python instead. Same
class as a `check_*.ps1` reporting green with zero coverage.)

## 2. What `exc_pc = 0xfffffffd` actually means

Already answered in this tree, by this tree: `crash_report.c`'s
`CRASH_REPORT_PC_OF_ZERO`. espcoredump stores `exc_pc` as
`esp_cpu_process_stack_pc(raw_pc)`, i.e. `raw_pc - 3`, so `0xfffffffd`
means **the saved PC was exactly `0x00000000`**. The CPU executed at
address 0. `exc_addr = 0x0` is a consequence of that, not an independent
fact — `crash_report_frame_trustworthy()` returns false and the record says
so.

That matters for the prior art the brief pointed at. There are two
precedents with a superficially identical signature, and they are **not**
the same thing:

| | 2026-09-04 `safety_poll` | 2026-09-08 `main` / `httpd_worker` | This panic |
|---|---|---|---|
| cause | `configASSERT` → `abort()` | **stack overflow** | ? |
| `exc_pc` | not recorded as `0xfffffffd` (the `exc_pc`/`exc_a1` interpretation machinery only landed 2026-09-08, `7f7e3d0e`) | `0xfffffffd` | `0xfffffffd` |
| `exc_a0` | — | `0x3fca0098` | `0x3fca0070` |
| `exc_a1_sp` | — | `0x3fcb15e4` | `0x3fcb2fe4` |

An `abort()` leaves a **valid** PC in the frame ("abort() was called at PC
0x42xxxxxx"). A PC of exactly 0 does not come from `abort()`; it comes from
returning through a smashed `a0`. And `exc_a0 = 0x3fca0070` is a DRAM
address, not a code address — a return address that is data.

By this field, the closest precedent is
**`docs/audits/boot_hang_2026-09-08.md`** and
**`docs/audits/firing_history_stack_overflow_2026-09-08.md`** (both confirmed
stack overflows, `exc_a0` in the same `0x3fca00xx` neighbourhood), not the
2026-09-04 `configASSERT`. `check_main_task_stack_budget.py` exists *because*
of the first of those.

## 3. Measured: the executor's run-end path nearly fills its stack

`profile_executor` is created with a **4096-byte** stack
(`profile_executor_start.c:85`) and **is** registered for stack-margin
reporting (`stack_margin_register("profile_executor", …, 4096)`, line 100) —
so brief item 2's registration question is answered: yes, registered.

There is, however, **no `check_*_task_stack_budget.py` for this task.** The
repo has them for `main`, `httpd_worker`, `system_uart_bridge` and
`uart_log_bridge` only. Running the `main` checker with
`--root executor_task_entry --stack-bytes 4096` against `build/KilnCtrl.elf`:

```
deepest static stack path from executor_task_entry: 3504 B  (of 4096 B)
       608 B     608 B  executor_task_entry
       160 B     768 B  adaptive_tune_run_end
       736 B    1504 B  adaptive_tune_refine_coupled_locked
        32 B    1536 B  zones_config_set_coupling_cell
        96 B    1632 B  nvs_save
        32 B    1664 B  zones_config_cfg_fs_save
       928 B    2592 B  zones_config_cfg_fs_save$part$0
       912 B    3504 B  zones_config_json_compute_crc
```

Ranking every direct callee of `executor_task_entry` by the deepest path
under it makes the shape unmistakable:

| total from `executor_task_entry` | via | when it runs |
|---|---|---|
| **3504 B** | `adaptive_tune_run_end` | run end only |
| **3072 B** | `firing_stats_persist` | run end only |
| **2784 B** | `escalate_guard_trip` | guard trip only |
| **2720 B** | `release_profile_relay_claim` | run end only |
| **2688 B** | `heat_enable_release` | run end / not-RUNNING |
| 1488 B | `exec_mode_state_check` | every tick |
| 1360 B | `reload_zone_config` | config reload |
| 1248 B | `pid_family_zone_tick` | every tick |
| 1136 B | `relay_cycles_maybe_persist` | every tick |
| 1008 B | `MAX31856_read_all` | every tick |

**The five deepest paths in this task are all run-end / fault-escalation
paths, and they are 2–2.5x deeper than anything a normal tick reaches.**
That is why 6000-second firings have run on this task without incident: a
healthy firing never goes near the top of that table.

Two independent ways to turn 3504 B into a verdict, both agreeing:

* **The repo's own convention.** `check_httpd_task_stack_budget.py` adds
  `UNMODELED_OVERHEAD_BYTES = 1800` for dispatch/ISR/window-spill the static
  walk cannot see. `4096 - 3504 = 592` naive free; `592 - 1800 =` **-1208 B**
  honest free.
* **This board's own measurement.** `DRAM_PSRAM_STATUS.md`'s live baseline put
  `profile_executor` at 1388 B free of 4096 during a real firing, i.e. 2708 B
  used, against a deepest *tick* path of 1488 B static — roughly 1220 B of
  real, measured, unmodelled overhead on this exact task. Add that to
  3504 B → **~4724 B on a 4096 B stack.**

Both land within ~15 B of each other on the size of the overrun. That is a
wide, unambiguous overflow, not a marginal one.

The static walk is an under-estimate by construction (indirect `callx8` not
followed, recursion cut at first repeat, no ISR/spill modelled) — so it
understates, never overstates, the depth.

### 3a. The specific regression: a fix applied to one side only

`firing_stats_persist()` (`profile_executor_firing_stats.c:508`) declares
`profile_firing_history_blob_t blob;` — **1364 bytes on the caller's stack** —
and the caller, per its own doc comment, is "whichever task ended the run",
which for a guard trip is the executor task itself. `objdump` puts its
prologue at `entry a1, 0x5f0` = **1520 B in one frame**, 2464 B including
`cfg_fs_read`.

`docs/audits/firing_history_stack_overflow_2026-09-08.md` — one day earlier —
is about *this same 1364-byte struct* overflowing the *httpd* stack, and it
was fixed by moving the **read** path's four copies to the heap. The **write**
path's copy, on the smaller 4096 B executor stack, was left where it was.
That is the "reset one side of a pair" shape this repo already tracks,
applied to a fix rather than to a reset.

## 4. What I looked for and did *not* find

* **Degenerate solve / NaN / Inf / divide-by-zero** (brief item 3). Ruled
  out by inspection of `zone_coupling_solve.c` at `e8cfe344`: every division
  (`M[r][k] / M[k][k]`, `sum / M[i][i]`, both diagonal-gain divisions) has
  its *result* `isfinite()`-checked with an enumerated fallback
  (`COUPLING_SOLVE_FALLBACK_*`); there is no `assert`, no `abort`, and no
  unchecked float in the file. The capture independently reports
  `ff_hold_used_matrix` true and `ff_hold` infeasible **never**, in all 80
  running samples. Floats do not fault this CPU, and no float reaches an
  assert on this path.
* **The one live `assert()` in this task.** `profile_executor.c:1528` asserts
  `exec_mode_state_check() == 0` every tick, and it **is live** in the
  flashed image (`CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y`,
  `CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL=2`). I could not construct a
  violation from this run's inputs: rules 4/5 need a bad IDLE/dwelling state,
  rules 1/6 need autotune driving (it was idle), rule 2 needs a non-PID mode
  (all three zones are mode 3), and rule 3 (`faulted && relay_commanded_on`)
  is closed on every path — `escalate_guard_trip()` calls
  `force_zone_relay_off()` in all three of its branches, and `apply_relay()`
  forces `relay_commanded_on = false` whenever the zone is authority-blocked,
  which a per-zone trip makes it. **And**, decisively, an `abort()` from that
  assert would leave a valid PC, not `0xfffffffd`. Not the cause here — but it
  is a real abort surface in this task and should not be forgotten.
* **Lock order / lock held across a blocking call** (brief item 4). Clean.
  The run-end block releases `s_exec.lock` (`xSemaphoreGive`) *before*
  calling `firing_stats_persist()` and `adaptive_tune_run_end()`, so
  `s_at.lock` is never taken under `s_exec.lock`.
  `exec_mode_state_check()` runs under the lock but is pure. No violation
  found.
* **The concurrent session** (brief item 5). Confirmed: the Pico moved
  `safety_config_version` 131 → 132 and `config_crc` 13756 → 64098 in the
  same window, and still reads 132/64098 now. I found **no path** by which an
  incoming safety-config change or link event executes on the
  `profile_executor` task — the executor only ever *reads* cached safety
  state (`relay_authority_zone_blocked()`, `safety_link_get_status()`), and
  frames arrive on `safety_proto_rx` / `safety_owner_evt`. Ruled out as the
  *panicking* path. **Not** ruled out as the *trigger*: a commissioning write
  that changed heat-enable/authority state could plausibly have ended the run
  and thereby entered the deep path (see §5).

## 5. The gap I did not close: what ended the run

The overflow needs the run to have *reached* a run-end path at ~397 s. I
could not confirm what did that.

* Arithmetic against `thermal_guard.c` puts guard 1's window for z2 at
  ~476 s, not ~400 s: `PROGRESS_DUTY_MIN = 0.5`, `PROGRESS_WINDOW_S = 300`,
  and z2's duty crossed 0.5 at t ≈ 176 s (0.451 at 158 s, 0.504 at 178 s).
  **But** all four thresholds are per-zone-overridable
  (`progress_duty_min`, `progress_window_s`, `wrong_dir_window_s`,
  `progress_band_c`) and **no endpoint on this board exposes those live
  values** — neither `control_get_zones` nor `GET /api/status` carries them.
  So the 476 s figure rests on defaults I could not verify, and guard 2's
  arrival-band branch (`WRONG_DIR_WINDOW_S = 120 s`) has a different arming
  condition again.
* Negative evidence, weak in both directions:
  `GET /api/firing_history?profile_id=0` still has `cplval70` (unix
  1788638418) as its newest record — **the cplval75 run was never written** —
  and `/api/profile_exec` reports `last_run.present=false`. That is equally
  consistent with "the run never ended" and with "the run ended and the panic
  happened inside the very first deep call of the run-end sequence". Note the
  order at `profile_executor.c:367-380`: `firing_stats_maybe_finalize()` →
  `firing_stats_persist()` → `adaptive_tune_run_end()`, so a crash inside
  `firing_stats_persist()` — the 3072 B path — leaves exactly this state.
* The K4 lead (owner's suspicion, separate agent): if the large safety relay
  was open, the run was commanding into a dead heat path for its whole life,
  which is precisely the condition guards 1/2 exist to catch. That makes "a
  guard tripped" the most likely trigger without establishing which one, or
  when.

## 6. Confirmed vs. inferred

**Confirmed** (measurement or file contents, this pass):

* No ELF matching the running image exists; nothing was symbolized.
* `exc_pc = 0xfffffffd` means the saved PC was exactly 0 — this repo's own
  `crash_report.c` says so; `exc_a0 = 0x3fca0070` is DRAM, not code.
* `profile_executor` has a 4096 B stack and is registered for stack-margin
  reporting; there is no stack-budget check script for it.
* Deepest static path from `executor_task_entry` = **3504 B of 4096 B**; the
  five deepest paths are all run-end/escalation; normal ticks peak at 1488 B.
* `firing_stats_persist()` puts a 1364 B blob (1520 B frame) on the calling
  task's stack, and the 2026-09-08 fix for the same struct moved only the
  read path off the stack.
* The board has been up 658 s at the time of this pass, having booted at
  10:38:23 — i.e. this is still the post-panic boot, so the live
  `get_stack_margin` figures (`profile_executor: 2760 B free`) describe an
  **idle** boot with no firing and are *not* evidence about the crash.
* `zone_coupling_solve.c` has no assert and no unchecked division.
* No lock-order violation on the run-end path.
* Pico config 131 → 132 during the window; still 132/64098 now.

**Inferred** (reasoning on top of the above):

* That the panic was a stack overflow of the `profile_executor` task. The
  signature match, the 3504 B + ~1220 B measured-overhead arithmetic, the
  fact that the deep paths are exactly the rarely-reached ones, and the
  one-day-old identical bug in the same struct all point one way — but no
  single artifact *proves* it.
* That the run had ended (most likely a guard trip) immediately before the
  panic.
* That `firing_stats_persist()` is the specific frame that tipped it (it is
  the first deep call in the run-end sequence, and its record is missing).

**Not established:** which guard tripped, at what time, and on which zone.

## 7. The one measurement that settles it

**Read the unerased `coredump` partition image and compare
`exc_a1_sp = 0x3fcb2fe4` against `profile_executor`'s recorded stack base.**

`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` with `CAPTURE_DRAM=y` and
`MAX_TASKS_NUM=64`, and the image is **still on the board** — `crash_report`
only erases it on `POST /api/crash_report/clear`, which has deliberately not
been called. The dump's per-task headers carry each task's stack start/end
**without needing any ELF at all**: if `0x3fcb2fe4` sits below
`profile_executor`'s allocated stack base, the overflow is proven in one
glance and every inference in §6 collapses into a fact. If it sits *inside*
the stack, this whole diagnosis is wrong and the `assert()` at
`profile_executor.c:1528` becomes the live hypothesis again.

Do that **before** anything else: it is destroyed by the next
`POST /api/crash_report/clear` and by the next panic
(`CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE` is off).

Second, cheaper, and independent of the board: **build `e8cfe344` in a clean
detached worktree** and re-run
`check_main_task_stack_budget.py --root executor_task_entry --stack-bytes 4096`
against *that* ELF. The numbers in §3 come from `build/KilnCtrl.elf` (built
`17:17:40Z`, i.e. ~HEAD, **not the running image**); four files on the deep
path (`zones_config_cfg_fs.c`, `zones_config_json.h`,
`zones_config_migrate.c`, `zones_config_store.c`) moved +85/-6 lines since
`e8cfe344`, and `profile_executor.c` / `_relay_io.c` / `_internal.h` moved
61. The ~2000-byte gap between run-end and normal-tick paths is far too large
to be an artifact of those deltas, but the exact figure 3504 should not be
quoted as the flashed image's number until it is measured on the flashed
image's ELF.

## 8. Recommended fix (not applied — coordinate first)

1. Move `firing_stats_persist()`'s `profile_firing_history_blob_t blob` to
   `heap_caps_malloc(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`, exactly as
   `firing_stats_load()` and `firing_stats_get_dualwrite_status()` in the
   same file already do, and as the 2026-09-08 audit did for the read path.
2. Do the same for `zones_config_json_compute_crc()`'s 912 B and
   `zones_config_cfg_fs_save$part$0`'s 928 B, which are the other half of the
   3504 B path and are reached from `main` and `httpd_worker` too.
3. Add `check_executor_task_stack_budget.py` (sibling of the `main`/`httpd`
   ones; root `executor_task_entry`, 4096 B, `UNMODELED_OVERHEAD_BYTES =
   1800`) so this cannot reopen silently — and negative-test it by restoring
   one of the stack blobs **by hand**, per `feedback_negative_test_every_check`
   and `feedback_negative_test_restore_by_hand`.
4. **Do not** enlarge the stack instead. Same reasoning the `main` checker
   already prints: the frames are cumulative and will keep growing.

Another session holds uncommitted edits to `profile_executor_run.c`,
`profile_executor_feedforward.c`, `profile_executor_relay_io.c` and
`zone_coupling_solve.c/.h`. Those were read this pass: they add a
`COUPLING_SOLVE_FALLBACK_MIXED_PROVENANCE` path (per
`docs/audits/dc_gain_factor_of_ten_2026-09-09.md` §4) and touch none of the
frames named above — so the fix does not collide with that work, but it lands
in the same module and should be sequenced with it.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
