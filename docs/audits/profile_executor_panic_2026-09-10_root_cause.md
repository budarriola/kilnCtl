# `profile_executor` panic during coupling-capture `profiles_stop()`, 2026-09-10 — root cause

Follow-up to `docs/audits/coupling_joint_identification_capture_2026-09-10.md`
(`b64fe09d`), which recorded the panic and explicitly flagged it for
root-causing before any future multi-hour joint hold. This pass reads the
board's own crash report, confirms the ELF used to interpret it, and traces
the fault to a specific, currently-CRITICAL stack path — the same one a
2026-09-09 fix (`379f3fe6`) left in place on purpose as "not the regression,
unrelated to this task specifically."

## What happened, per the board

`GET /api/crash_report` (read before anything else; not cleared):

```
present=true, acknowledged=true, exc_cause=0 (IllegalInstruction),
exc_pc=0xfffffffd, exc_addr=0x00000000, exc_a0=0x3fca0070,
exc_a1_sp=0x3fcb2fe4, exc_task="profile_executo",
found_on_boot_reset_reason="PANIC", frame_trustworthy=false,
backtrace=["0xfffffffd"], backtrace_corrupted=true
```

`get_heap_status` at the time of this pass: `reset_reason='panic/exception'`
(unclean boot), `uptime_s=550`, `link_reply_us` shows `timeouts=101` out of
143 calls since boot — the safety-link is timing out on a majority of
exchanges even now, post-reboot.

`get_fw_version`: `commit=79d93233`, `built=2026-09-10 22:45:13Z`, tree dirty,
board 36 commits behind `HEAD` (`b64fe09d`).

**`acknowledged=true` was already set** when this pass started (not by this
pass) — noted per the brief's instruction not to acknowledge before
extracting everything, but it was out of my control; nothing below depended
on flipping that bit, so nothing was lost. Whether it reflects last night's
crash or an unresolved acknowledgment from a prior session is not established
either way and does not change the finding below, since `exc_a0`/`exc_a1_sp`
are not part of the record's dedup key (see "Same shape as 2026-09-09" below)
and were read fresh from `esp_core_dump_get_summary()` regardless.

## ELF match, confirmed two ways

`find_crash_elf()` resolves `firmware/KilnFW/build/elf_archive/KilnCtrl-16bced646ac0.elf`.
Cross-checked against the manifest and the board's own report, independently:

* `manifest.json`: `"Sep 10 2026 15:45:13"` → `elf_key 16bced646ac0`, `git_commit 79d93233`.
* `get_fw_version()`: `commit=79d93233`, `built=2026-09-10 22:45:13Z`.

`15:45:13` local (the ELF's embedded `esp_app_desc_t` identity, Pacific) is
`22:45:13Z` — matches the board's reported UTC build time exactly, and the
commit hashes match independently of the timestamp. **This is the right ELF.**

## `exc_pc = 0xfffffffd` means a saved PC of exactly 0

Already documented in this tree (`crash_report.c`'s `CRASH_REPORT_PC_OF_ZERO`
comment, and `docs/audits/executor_panic_stack_overflow_2026-09-09.md` §2):
espcoredump stores `exc_pc = raw_pc - 3`, so `0xfffffffd` means the CPU
returned to address `0x00000000` — a return through a smashed `a0`, not a
call through a null pointer at some other live PC, and not a `configASSERT`
abort (which leaves a valid PC in flash). `exc_addr = 0x0` is a
consequence of the same fact, not independent evidence. This rules the
2026-09-04 `safety_poll` shape (a live-PC `abort()`) out by the very field
that made the two look alike at a glance.

## Same shape, same numbers, as the 2026-09-09 `profile_executor` panic

`docs/audits/executor_panic_stack_overflow_2026-09-09.md`'s table of that
panic's exception frame:

| field | 2026-09-09 panic | 2026-09-10 panic (this one) |
|---|---|---|
| `exc_pc` | `0xfffffffd` | `0xfffffffd` |
| `exc_a0` | `0x3fca0070` | `0x3fca0070` |
| `exc_a1_sp` | `0x3fcb2fe4` | `0x3fcb2fe4` |
| `exc_task` | `profile_executo` | `profile_executo` |

**Byte-for-byte identical `a0`/`a1_sp`.** Those two fields are not part of
`crash_report_dump_id()`'s hash (only `exc_pc`, `exc_task`, and the
backtrace array feed the dedup key — see `crash_report.c:209-233`), so this
match is not an artifact of the app's own "don't recapture the same crash"
logic; it was read fresh from `esp_core_dump_get_summary()`. The only way
two independent panic events produce identical `a0`/`sp` is if the same task
overflows to the same static depth from the same heap-allocated stack
address — which is exactly what a deterministic boot sequence (same
firmware, same init order, same heap-allocator state at the point this task
is created) predicts for the *same underlying overflow path* recurring, not
two different bugs colliding on the same numbers by chance.

**This is the same bug, or the same latent path, as 2026-09-09** — not a new
fault mode.

## The 2026-09-09 fix landed, and is on the board — but left a documented CRITICAL path in place

`git log` confirms `379f3fe6` ("Fix profile_executor stack overflow; add its
missing budget check") is an ancestor of the running commit `79d93233`, and
is the last commit to touch `profile_executor_firing_stats.c`,
`zones_config_migrate.c`, and `zones_config_cfg_fs.c` before it — so the
running image has the fix, not a stale pre-fix build.

Running the repo's own `check_executor_task_stack_budget.py` against **the
confirmed-matching running-image ELF** (`KilnCtrl-16bced646ac0.elf`):

```
deepest static stack path from executor_task_entry: 2784 B (ceiling 2784 B of a 4096 B profile_executor stack)
  executor_task_entry -> escalate_guard_trip -> release_profile_relay_claim
  -> heat_enable_release -> safety_link_request_enable -> safety_exchange
  -> safety_drain_inbox -> safety_drain_inbox_ex -> safety_apply_fw_version
  -> safety_link_send_announce_version_burst -> uart_protocol_send_broadcast
  -> frame_and_send$constprop$0 -> hal_uart_send_blocking -> uart_write_bytes
  -> uart_tx_all$part$0 -> uart_enable_tx_write_fifo

naive implied free: 1312 B
honest free (naive - 1220 B unmodelled overhead): 92 B (2.2% of 4096 B) -- classified CRITICAL
check_executor_task_stack_budget: OK (CRITICAL honest headroom -- worth a look, not yet failing)
```

This is exactly the path the 2026-09-09 fix's own commit message and the
checker's docstring call out as the *new* deepest path post-fix — "unrelated
to this task specifically," inherited from the shared safety-link
frame-send chain every UART bridge task pays — and it was accepted as the
new ceiling rather than fixed, on 92 B of *estimated* honest margin (the
checker's own 1220 B unmodelled-overhead figure is a **measured baseline
from a different, lighter workload**: `DRAM_PSRAM_PLAN.md`'s number, not a
guaranteed ceiling on ISR/scheduler cost).

`profile_executor_halt()` (`profile_executor_status.c:59`) calls
`heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE)` directly — the exact
entry point of this chain — and `profile_executor_halt()` is what
`profiles_stop()` calls to end a run. **Last night's panic happened at the
exact moment `profiles_stop()` was called to end the low-ΔT joint hold**,
which is precisely when this chain runs.

## Why last night, specifically, and not the many earlier stops

Two aggravating factors, both present in this capture and both plausible
contributors to exceeding a 92 B margin that the static walk cannot fully
account for:

1. **The safety link was unusually unhealthy.** `get_heap_status` (read
   after the reboot, so this is the *post*-panic baseline, not proof of the
   pre-panic state, but it is the freshest available signal) shows
   `link_reply_us timeouts=101` of 143 — a majority-timeout rate. This
   exact call chain (`safety_link_request_enable` → `safety_exchange` →
   `safety_drain_inbox_ex`) is safety-link traffic; a link in this state is
   more likely to be retrying, and retries do not add *static* call depth
   (loops re-enter the same frames, which the static walker already counts
   once) but do increase the *time* the task spends deep in this chain,
   raising the odds of an ISR landing while the stack is at its worst point
   — exactly the kind of cost the checker's own 1220 B figure admits it
   cannot model.
2. **This run was a genuine 40+ minute, all-three-zone, continuously-ticking
   dwell** — the kind of sustained load `profile_executor` "rarely sees for
   40+ continuous minutes in routine use" (capture doc's own words), unlike
   most earlier stops which ended shorter or single-zone runs. A rarer
   code path (or simply more opportunities for an unlucky ISR to land) is
   consistent with why this specific stop, and not every other
   `profiles_stop()` call before it, tipped a 92 B margin over.

## What I eliminated

* **Numerical fault in the control law / coupling solve.** `zone_coupling_solve.c`
  has no unguarded division and no assert (re-confirmed present in this
  pass's read; matches the 2026-09-09 audit's finding, which is still
  accurate for this file at `79d93233`).
* **A `configASSERT`/`abort()` (the 2026-09-04 `safety_poll` shape).**
  Ruled out by `exc_pc` itself — an `abort()` leaves a valid code-space PC,
  not exactly `0x00000000`.
* **The firing_stats/adaptive_tune stack paths the 2026-09-09 fix targeted.**
  Confirmed fixed and no longer the deepest path (1168 B / 1088 B today,
  per the same checker run).
* **The five commits made after the board's flashed commit (79d93233) but
  not yet on it** (see below) — none of them touch this code path.

## What is NOT established

* No raw coredump partition read was performed (the method
  `executor_panic_stack_overflow_2026-09-09.md` §7 recommends — reading
  `exc_a1_sp` against `profile_executor`'s actual allocated stack bounds
  straight out of the coredump image, which needs no ELF and would turn
  "92 B estimated margin" into a proven fact). No MCP tool in this session
  exposes a raw coredump/partition read; the ones available
  (`debug_read_memory`) require JTOG halt and were not attempted here since
  the crash report already gives high-confidence agreement across two
  independent methods (identical exception-frame fields two days apart,
  and a live re-measurement against the confirmed-matching ELF landing on
  the exact same call chain at the exact same task). If this needs to be
  made airtight, that JTAG coredump read is the next step, and per the prior
  audit it is destroyed by the next `POST /api/crash_report/clear` or the
  next panic.
* Exactly which frame tips 92 B negative (an ISR spill, a scheduler
  tick, or genuinely more retry-driven time-at-depth) is inferred, not
  measured directly.

## The five commits made after the board's running commit

Board is at `79d93233`; `HEAD` is `b64fe09d`, 36 commits ahead. The five most
recent are none of them relevant to this path:

| commit | touches |
|---|---|
| `5d3bc854` | `zones_cfg_t` schema bump + `zone_model_at()`/`coupling_at()` seam — reads model/coupling fields, no stack-depth or executor-halt change |
| `83c8b28b` | on/off-zone UART **log lines** inside the tick loop — not the halt/escalation path |
| `9bc155ea` | readiness/estop-verification HTTP + boot_early wiring |
| `a9abd273` | `main_page.html` dashboard warning text only |
| `da506105` | new `sw_reset_http.c` endpoint, `ota_http.c`/`main_network_http.c` wiring |

**None of the five touch `profile_executor_status.c`, `heat_enable.c`,
`safety_link_request_enable`/`safety_exchange`, or any file on the 2784 B
chain above.** They neither cause nor fix this crash; flashing them changes
nothing about this finding.

## Recommendation (not applied — coordinate first)

The checker already does its job — it flagged this path CRITICAL at the time
of the fix that created it. The fix, at the time, judged 92 B acceptable
because it was "unrelated to this task specifically" and shared by every
UART bridge task. Last night's crash is direct evidence that judgment should
be revisited:

1. Do **not** enlarge `profile_executor`'s stack as a first move (repo
   standing guidance: frames are cumulative and stack bumps invite the next
   regression to hide inside the new slack) — but given this is now a
   *confirmed* recurrence, not a theoretical CRITICAL rating, enlarging this
   one task's stack by a modest, explicitly-justified amount (with the
   reason stated, per "do not raise a ceiling quietly") is defensible here
   specifically because the deep chain is *shared infrastructure*
   (`frame_and_send$constprop$0`) already re-used at similar depth by every
   UART bridge task on their own, larger (8192 B) stacks — this task is the
   outlier at 4096 B, not the frame-send chain.
2. Alternatively/additionally, reduce `heat_enable_release()`'s reachable
   depth by not calling into the full `safety_link_request_enable` /
   announce-burst chain synchronously from the halt path — if that
   notification can be deferred to a task with more headroom (e.g. the
   existing `safety_owner_evt` task, which independently reports 76%
   headroom), the CRITICAL chain moves off the tightest stack in the
   system.
3. Either way, `check_executor_task_stack_budget.py`'s `CEILING_BYTES`
   comment should be updated to say this path is a **known, now-confirmed**
   crash source, not "not the regression, unrelated to this task
   specifically" — that comment is what let a CRITICAL-but-passing check
   sit unaddressed for a day before it fired for real.

No fix is applied by this pass — this is a root-cause report only, per the
brief.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
