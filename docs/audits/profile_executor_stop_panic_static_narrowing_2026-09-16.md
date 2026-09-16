# `profile_executor` panic at `profiles_stop()`, 2026-09-16 — static narrowing: two of three hypothesis classes eliminated, and the named mechanism is already gone at HEAD

Fifth pass on the open item `project_profile_executor_panic_at_stop`. Follows
`docs/audits/profile_executor_panic_2026-09-10_root_cause.md`,
`docs/audits/profile_executor_panic_recurrence_2026-09-14.md` and
`docs/audits/profile_executor_coredump_2026-09-15.md`.

This was a **static** pass by construction: no flash, no reset, no firing, no
relay actuation, no MCP server restart. Nothing was read from the board at all.
Every number below comes from source at `953de226` (`origin/main`) and from a
full ESP-IDF target build of that commit made in a clean worktree.

**Bottom line, stated plainly:**

1. The mechanism all three prior audits named — the synchronous safety-link
   frame-send chain running on `profile_executor`'s 4096 B stack — **no longer
   exists at HEAD**. It was removed structurally by `1c8d7f6e`, and this pass
   measures it gone: `heat_enable_release()`'s entire reachable depth is now
   **96 B**, and the executor's deepest path no longer traverses the UART chain
   at all.
2. All four recorded panics predate `1c8d7f6e`. **No panic of this signature
   has ever been observed on a build containing it.**
3. The adjacent-static-stack corruption class (the 2026-09-04 `safety_poll`
   shape) is **eliminated on new evidence** — not merely unconfirmed. This is
   the finding this pass adds that the prior four did not have.
4. It is therefore **not root-caused in the strict sense** (no mechanism proved
   to fire on HEAD), and no fix is committed. But the hypothesis space is now
   down to one surviving class, and the only remaining discriminator requires
   hardware this brief forbids.

**Is it safe to stop iterating statically? Yes.** Every question that source
and a map file can answer has been answered. The next step is a hardware step
and needs the coordinator.

Commit hashes cited here were verified with `git cat-file -t` — `9b9ef3ef`,
`1c8d7f6e`, `059a896e`, `8813bedd`, `ffa92004`, `c8f7506b`, `b38b1498`,
`a864a610`, `379f3fe6`, `953de226`, `51e1ef5`, `b64fe09d` — all `commit`.

---

## 1. The stop path, end to end, with file:line

`profiles_stop()` (the PC-side MCP tool) lands on
`profile_executor_halt()`, `profile_executor_status.c:24`. Its full body:

| line | call | notes |
|---|---|---|
| `profile_executor_status.c:27` | `s_exec.lock == NULL` prestart guard | refuses before `profile_executor_start()` |
| `:31` | `xSemaphoreTake(s_exec.lock, portMAX_DELAY)` | **lock acquired** |
| `:32-34` | early return if `PROFILE_EXEC_IDLE` | gives lock back at `:33` |
| `:36` | `force_all_relays_off()` | `profile_executor_relay_io.c:446`; documented "must be called with `s_exec.lock` held" |
| `:41` | `io_segs_force_all_off(false)` | `profile_executor_relay_io.c:652` |
| `:44` | `relay_authority_release_mask(s_exec.claimed_relay_mask)` | `relay_authority.c:88` |
| `:55` | `relay_authority_heat_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE)` | `relay_authority.c:131` |
| `:59` | `heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE)` | `heat_enable.c:533` → `he_release_common()` `:464` |
| `:65` | `capture_run_snapshot(&halt_snap)` | `profile_executor_firing_stats.c:632` |
| `:77` | `firing_stats_maybe_finalize(&fs_rec)` | `profile_executor_firing_stats.c:606` |
| `:78-81` | `clear_this_runs_faults()`, state → `IDLE`, fault fields cleared | |
| `:82` | `xSemaphoreGive(s_exec.lock)` | **lock released** |
| `:84` | `firing_stats_persist(&fs_rec)` | outside the lock — NVS write via the flash worker |
| `:92` | `adaptive_tune_run_end(&fs_rec, false)` | outside the lock — takes `adaptive_tune_lock` |
| `:~105` | `run_state_note(end_phase, &halt_snap.snap)` | outside the lock |
| `:~118` | `relay_cycles_flush()` | outside the lock; `relay_cycles.c` takes its own |

The second stop-shaped path is the tick loop's non-`RUNNING` teardown branch,
`profile_executor.c:335-383`: `force_all_relays_off()` → `io_segs_force_all_off(false)`
→ `heat_enable_release_backstop(HEAT_ENABLE_CLAIMANT_PROFILE)` →
`firing_stats_maybe_finalize()` under the lock, then `xSemaphoreGive` at `:369`,
then `firing_stats_persist()` / `adaptive_tune_run_end()` outside it.

What the stop path tears down: this run's relay claims (both the per-relay mask
and the shared heat-zone claim), the K4 heat-enable claim, the segment
machinery, the firing-stats record, and the run-state breadcrumb. It signals no
task and waits on no task. It touches exactly three locks —
`s_exec.lock`, `s_he.lock` (inside `heat_enable.c`, taken and released around
small bookkeeping only), and `s_slot_lock` (inside `kiln_io_owner.c`, reached
via `apply_relay()`).

## 2. Is a `configASSERT` reachable on that path? — eliminated, on the exception frame itself

**Eliminated, and not by inspection of the primitives but by a stronger piece of
evidence that applies regardless of which primitives are reachable.**

All four panics report `exc_pc = 0xfffffffd`. `crash_report.c:140` defines
`CRASH_REPORT_PC_OF_ZERO 0xfffffffdu`, and `crash_report.c:441` spells out what
it means: espcoredump stores `esp_cpu_process_stack_pc(pc)`, i.e. `raw_pc - 3`,
so `0xfffffffd` is **a saved PC of exactly `0x00000000`** — the CPU returned
through a smashed `a0`.

A `configASSERT` failure is an `abort()` executed at a **live, valid, in-flash
PC**. It cannot present as a saved PC of zero. So the 2026-09-04 `safety_poll`
shape — `configASSERT(pxQueue->uxItemSize == 0)` reached from `xSemaphoreTake`
— is ruled out by the exception frame itself, for all four instances, without
needing to enumerate anything.

`exc_addr = 0x0` is a **consequence** of the same fact, not independent
evidence. It is the documented red herring and was not chased.

For completeness, the FreeRTOS primitives the stop path does reach, and their
assert behaviour: `xSemaphoreTake(s_exec.lock, portMAX_DELAY)`
(`profile_executor_status.c:31`) and `xSemaphoreTake(s_he.lock, portMAX_DELAY)`
(`heat_enable.c:139`) both `configASSERT` on a NULL handle and on being called
from an ISR — neither applies here (`s_exec.lock` is explicitly NULL-guarded at
`:27`, and this is task context). `xQueueSend(s_cmd_queue, cmd, 0)`
(`kiln_io_owner.c:608`) is non-blocking. None of this changes the verdict
above; it is the exception frame, not this enumeration, that closes the
question.

## 3. `.bss` neighbourhood — the adjacent-static-stack class is ELIMINATED

This is the finding this pass adds. Measured from the map file of a full target
build of `953de226` (`firmware/KilnFW/build/KilnCtrl.map`).

**The decisive structural fact: `profile_executor`'s stack is not in `.bss` at
all.** `profile_executor_start.c:85` creates it with
`xTaskCreatePinnedToCore(executor_task_entry, "profile_executor", 4096, ...)` —
a **heap**-allocated stack. The 2026-09-04 `safety_poll` mechanism required a
*statically allocated* task stack (`s_lvgl_task_stack`, an array in `.bss`)
sitting next to a *static* victim (`thermo_owner.c`'s `s_slots[]`) so that
overrunning one linearly wrote into the other. That geometry does not exist
here in either direction.

The `.bss` neighbourhood of `s_exec`, with distances:

```
0x3fcb3bc4   0x1e0   s_base_dir            (cfg_fs.c)
0x3fcb3da4   0x008   s_dw                  (dualwrite_window.c)   <- immediately below
0x3fcb3dac   0xc64   s_exec                (profile_executor.c)   <- 3172 B
0x3fcb4a10   0x003   s_infeasible_active$0 (profile_executor_feedforward.c) <- immediately above
0x3fcb4a13   0x003   s_fallback_active$1   (profile_executor_feedforward.c)
...
0x3fcb5b20           _bss_end = _heap_start
```

* `s_exec` spans `0x3fcb3dac`–`0x3fcb4a10` (3172 B).
* Its immediate neighbours are an 8-byte `s_dw` below and a 3-byte
  `s_infeasible_active$0` above. **Neither is a stack**; neither is written by
  any deep or unbounded path.
* `s_exec` ends 4368 B below `_heap_start` (`0x3fcb5b20`).

There are exactly **two** static task stacks in `.bss` in the whole image:

| symbol | address | size | distance to `s_exec` |
|---|---|---|---|
| `s_coredump_stack` (`core_dump_common.c`) | `0x3fca0420` | `0x764` (1892 B) | ~78 KB below |
| `s_lvgl_task_stack$2` (`lvgl_port.c`) | `0x3fca3070` | `0x2000` (8192 B) | ends `0x3fca5070`, i.e. **60,732 B (0xED3C) below `s_exec`**, with well over a thousand intervening symbols |

For `s_lvgl_task_stack` to reach `s_exec` it would have to overrun by ~60 KB
and corrupt everything in between without any other symptom — and that stack
is the one whose overrun was *already fixed* (`51e1ef5`). `s_coredump_stack` is
only live during a panic dump, after the fault.

**Verdict: no static symbol adjacent to `s_exec` can plausibly corrupt it, and
`profile_executor`'s own stack is not a `.bss` object, so the documented
2026-09-04 corruption class cannot apply to this panic.** Eliminated.

One caveat kept honest: this eliminates adjacent-***static***-stack corruption.
Adjacent-***heap***-block corruption — another heap-allocated task stack
overrunning into `profile_executor`'s heap-allocated stack — is *not* excluded
by this analysis, and the map file cannot speak to it (heap layout is a runtime
property). That remains inside the one surviving hypothesis class in §6.

Consistency note on the recorded addresses: the 2026-09-14 pass resolved
`s_exec` at `0x3fcafcc4` in the running (`c8f7506b`) image. With
`sizeof(s_exec) = 0xc64`, that struct spans `0x3fcafcc4`–`0x3fcb0928`, and the
recorded `exc_a1_sp = 0x3fcb3ae4` is **well above it** — so the faulting SP was
*not* inside `s_exec`. That tempting reading is closed too.

## 4. Lock-order and lock-across-producer — no violation at HEAD

**Lock order (`s_exec.lock` before `s_at.lock`, never the reverse): respected.**
`adaptive_tune_run_end()` — the only `adaptive_tune_lock` acquirer on this path
— is called at `profile_executor_status.c:92`, which is **after** the
`xSemaphoreGive(s_exec.lock)` at `:82`. Same in the tick-loop teardown branch:
the give is at `profile_executor.c:369`, the `adaptive_tune_run_end()` at
`:378`. The two locks are never held simultaneously on the stop path, so the
order cannot be inverted here.

**Lock-across-producer / blocking call: no violation, and this is a change since
the panics.** This pass initially read `halt()` holding `s_exec.lock` across
`heat_enable_release()` (`:59`) as exactly that violation — which it *was*, on
every build that panicked. It is not any more:

* `heat_enable_release()` (`heat_enable.c:533`) → `he_release_common()` (`:464`)
  now performs **bookkeeping only** under a short `s_he.lock`, sets
  `release_pending`, and returns.
* The blocking `safety_link_request_enable()` exchange was moved out by
  `1c8d7f6e` into `heat_enable_service_pending_release()` (`heat_enable.c:~570`),
  drained once per loop by `safety_poll_task` on its own 8192 B stack.
* Measured on the HEAD build: **`heat_enable_release()`'s entire reachable depth
  is 96 B** (`heat_enable_release` 32 B → `he_release_common` 32 B → `he_lock`
  32 B). There is no UART chain under it any more.

The other calls made under `s_exec.lock` were checked individually and none is a
producer or a blocking wait: `force_all_relays_off()` iterates zones and calls
`apply_relay()`, whose only enqueue is `xQueueSend(s_cmd_queue, cmd, 0)` —
**timeout 0, non-blocking** (`kiln_io_owner.c:608`); `capture_run_snapshot()` is
`memset`/`strncpy`/field copies; `firing_stats_maybe_finalize()` builds a record
into a caller-owned buffer and deliberately leaves the NVS write to the caller
*after* the unlock (its own doc comment says so). The genuinely expensive work —
`firing_stats_persist()`, `adaptive_tune_run_end()`, `run_state_note()`,
`relay_cycles_flush()` — is all outside the lock.

`apply_relay()` does take `kiln_io_owner`'s `s_slot_lock` (`portMAX_DELAY`)
while `s_exec.lock` is held, a short leaf mutex over non-blocking bookkeeping.
That is pre-existing on every tick, not specific to the stop path, and is not
the `dashboard_get_status()`-class violation the invariant is aimed at.

## 5. Does `9b9ef3ef` interact? — no

`9b9ef3ef` ("Close the heat-enable stale-claim window the release epoch
missed") changes only *when* `release_epoch[who]` is advanced, inside the
existing `s_he.lock` critical section in `he_release_common()`. Concretely it:

* factors the old `heat_enable_release()` body into
  `he_release_common(who, bit, stop_transition)`;
* changes `if (was_held)` to `if (stop_transition || was_held)`;
* adds the thin wrapper `heat_enable_release_backstop()` for the two per-tick
  idle backstops (`profile_executor.c:353`, `autotune_engine.c:~861`).

It adds one `bool` parameter and no call, no allocation, no blocking wait and no
new lock. The measurement confirms the absence of any stack effect: the whole
`heat_enable_release()` chain is 96 B, and the executor's deepest path does not
pass through `heat_enable` at all.

`profiles_stop()` *is* on the release side, so the brief's question is a fair
one — but the behavioural change is "an `acquire_since()` that raced a stop now
refuses", which can only ever *suppress* a `REQUEST_ENABLE(true)`. It cannot
produce a return through a smashed `a0`.

**Verdict: `9b9ef3ef` does not touch the panic path. Unrelated.**

## 6. The named mechanism is already gone at HEAD — measured

All three prior audits converged on one chain as the dominant contributor:

```
executor_task_entry -> ... -> heat_enable_release -> safety_link_request_enable
  -> safety_exchange -> uart_protocol_send_broadcast -> frame_and_send$constprop$0
  -> hal_uart_send_blocking -> uart_write_bytes -> ...
```

measured at 2784 B (2026-09-10, 92 B honest headroom, CRITICAL) and then 1936 B
after `a864a610` (940 B honest headroom, LOW).

Measured this pass, against a full target build of `953de226`:

```
deepest static stack path from executor_task_entry: 1776 B (ceiling 1936 B of a 4096 B profile_executor stack)
       608 B     608 B cumulative  executor_task_entry
       160 B     768 B cumulative  adaptive_tune_run_end
       736 B    1504 B cumulative  adaptive_tune_refine_coupled_locked
       160 B    1664 B cumulative  adaptive_tune_coupled_fit
       112 B    1776 B cumulative  zone_coupling_gauss_solve_partial_pivot_vec

naive implied free: 2320 B
honest free (naive - 1220 B unmodelled overhead): 1100 B (26.9% of 4096 B) -- classified LOW
check_executor_task_stack_budget: OK
```

And rooted at the halt entry point specifically:

```
deepest static stack path from profile_executor_halt: 1632 B
       464 B     464 B cumulative  profile_executor_halt
       160 B     624 B cumulative  adaptive_tune_run_end
       736 B    1360 B cumulative  adaptive_tune_refine_coupled_locked
       160 B    1520 B cumulative  adaptive_tune_coupled_fit
       112 B    1632 B cumulative  zone_coupling_gauss_solve_partial_pivot_vec

honest free: 1244 B (30.4% of 4096 B) -- classified OK
```

**The UART frame-send chain is no longer the deepest path, and no longer
reachable from `heat_enable_release()` at all.** The deepest path is now the
adaptive-tune coupled solve, which runs *outside* `s_exec.lock` and is
`enabled`-gated per zone. This is recommendation #2 of the 2026-09-14 audit and
#2 of the 2026-09-15 audit, finally landed — by `1c8d7f6e`, with
`059a896e` / `8813bedd` / `ffa92004` / `9b9ef3ef` as the follow-up review fixes.

**Timeline, which is the load-bearing point:** all four panics are on builds
`79d93233` (09-09, 09-10) and `c8f7506b` (09-14, 09-15, built 2026-09-14
23:55:17Z). `1c8d7f6e` landed 2026-09-15. Every recorded instance predates the
fix. **No occurrence of this signature has ever been observed on firmware
containing `1c8d7f6e`** — and, per the 2026-09-15 audit, the board has not been
reflashed since, so that is a statement about absence of opportunity, not about
demonstrated absence of the bug.

## 7. What was eliminated, and with what evidence

| hypothesis | verdict | evidence |
|---|---|---|
| `configASSERT`/`abort()` (2026-09-04 `safety_poll` shape) | **eliminated** | `exc_pc = 0xfffffffd` = saved PC of 0 (`crash_report.c:140`, `:441`). An `abort()` leaves a valid flash PC. Holds for all four instances. |
| Null call through `exc_addr = 0x0` | **eliminated** | consequence of the above, not independent evidence |
| Adjacent **static** stack corrupting `s_exec` | **eliminated (new)** | `profile_executor`'s stack is heap-allocated (`profile_executor_start.c:85`); nearest static task stack is `s_lvgl_task_stack$2`, 60,732 B away; `s_exec`'s immediate `.bss` neighbours are an 8 B and a 3 B scalar |
| Faulting SP inside `s_exec` | **eliminated (new)** | `s_exec` = `0x3fcafcc4`+`0xc64` in the running image; `exc_a1_sp = 0x3fcb3ae4` is above that range |
| Lock-order inversion (`s_at` before `s_exec`) | **eliminated** | `adaptive_tune_run_end()` is called after `xSemaphoreGive(s_exec.lock)` on both stop paths (`profile_executor_status.c:82`/`:92`, `profile_executor.c:369`/`:378`) |
| Lock held across a producer/blocking call | **eliminated at HEAD** | `heat_enable_release()` is 96 B and non-blocking since `1c8d7f6e`; every other under-lock call is bookkeeping or a timeout-0 enqueue. **Was true on all four panicking builds.** |
| `9b9ef3ef` involvement | **eliminated** | bookkeeping-only epoch change under an existing lock; adds no depth, no blocking |
| Deep synchronous UART chain on the 4096 B stack | **removed at HEAD, unconfirmed on hardware** | measured 2784 → 1936 → gone; deepest path is now 1776 B via adaptive-tune |

## 8. What remains open

**One hypothesis class survives: a stack-overflow-shaped fault on
`profile_executor` whose remaining contribution is not statically visible** —
either the `UNMODELED_OVERHEAD_BYTES = 1220` figure understating real
ISR/window-spill cost on the teardown tick, or adjacent-**heap**-block
corruption (another heap-allocated task stack overrunning into this one), which
no map file can rule on.

What cannot be settled statically, and why:

* **No new backtrace.** The board's stored coredump is unreadable: the running
  image (`c8f7506b`) predates `b38b1498`, which added the
  `GET /api/coredump/info` endpoint, so `read_esp_coredump()` 404s; and the
  archived ELF matching that build was pruned before the 2026-09-15 pass, so
  `find_crash_elf()` fails. Both inputs are gone.
* **No live measurement.** Confirming whether the fault still occurs requires
  flashing and running a firing — explicitly forbidden by this brief, and
  correctly so.

## 9. The single highest-value next experiment

**Flash HEAD (or anything at or after `b38b1498`) and run one multi-hour hold to
a `profiles_stop()`.** This is one action that settles two things at once:

1. It puts a build containing `1c8d7f6e` on the board for the first time. If the
   signature does not recur across several long runs, the fix is confirmed and
   this item closes.
2. It puts the coredump-reader endpoint on the board. If the signature *does*
   recur, `read_esp_coredump()` yields the real backtrace and
   `exc_a1_sp`-against-actual-stack-bounds — turning four passes of inference
   into fact in a single read.

Preconditions, from the existing record: archive the ELF at flash time
(`flash_firmware()` does this) so the next report is symbolizable; do not clear
the current crash report first (it is the fourth instance and still
`acknowledged=false`); and expect `flash_firmware()`'s verify step to matter
here, since the board is ~70 commits behind.

**This requires bench access and a firing. It is a coordinator action — this
pass stops here rather than forcing it.**

## 10. What this pass did and did not do

Did: read source at `953de226`; ran one full ESP-IDF target build in a clean
worktree; ran `check_executor_task_stack_budget.py` against that build at three
roots; read the map file. Nothing was written to any firmware source file.

Did **not**: flash, reset, actuate a relay, start a firing, read from the board,
acknowledge or clear a crash report, or restart an MCP server.

No firmware change is committed by this pass. The mechanism is narrowed, not
proved, and per the brief a plausible-sounding patch to a panic nobody
understands is worse than the open bug.
