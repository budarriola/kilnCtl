# `profile_executor` panic, 2026-09-15 — fourth occurrence of the same signature; still not symbolizable

Follow-up to `docs/audits/profile_executor_panic_2026-09-10_root_cause.md` and
`docs/audits/profile_executor_panic_recurrence_2026-09-14.md`. This pass was
asked to symbolize the board's stored coredump read-only (no flash, no reset,
no relay actuation). **No coredump could be read and no new backtrace was
obtained** — the reasons are structural, not an oversight, and are recorded
below rather than guessed past.

All commands run this pass were either serial-link INFO queries (refused
device commands outright — see below) or plain HTTP `GET`s. **Nothing was
acknowledged, flashed, reset, or actuated.**

## 1. Board state at the start of this pass

`connect(port="COM14")` succeeded but the board immediately refused any
non-INFO command: it speaks UART protocol v11, this copy of pc_tools speaks
v12 (documented, expected mismatch — `project_two_protocol_versions`
memory). No firing-status read was possible over the serial link as a
result; `get_heap_status()` (HTTP `GET /api/status`) was used instead and
shows the board idle:

```
reset_reason='interrupt watchdog'   uptime_s=52938  (~14.7 h)
heap_internal: free=67443 B, min_free=32135 B (healthy, no exhaustion)
```

No firing was in progress. Nothing was touched as a result of this check.

## 2. The crash report, read first and NOT acknowledged

`GET /api/crash_report`:

```
present=true, acknowledged=false,
exc_cause=0 (IllegalInstruction), exc_pc=0xfffffffd, exc_addr=0x00000000,
exc_a0=0x3fca0080, exc_a1_sp=0x3fcb3ae4, exc_task="profile_executo",
found_on_boot_reset_reason="PANIC",
frame_trustworthy=false, backtrace=["0xfffffffd"], backtrace_corrupted=true
```

Same shape as all three prior instances: `exc_pc = 0xfffffffd` is
espcoredump's `raw_pc - 3`, i.e. a saved PC of exactly `0x00000000` — a
return through a smashed `a0`, not a live-PC `abort()` (the 2026-09-04
`safety_poll` shape) and not informative as "a null call at `0x0`"
(`CRASH_REPORT_PC_OF_ZERO`, already documented in this tree). `exc_addr =
0x0` is a consequence of the same fact and was not chased.

**Still `acknowledged=false` after this pass.** It was read, not cleared.

## 3. This is the same build as the 2026-09-14 recurrence, and the addresses are byte-identical to it

`get_fw_version()`:

```
commit: c8f7506b   tree: clean   built: 2026-09-14 23:55:17Z
board is 70 commit(s) behind HEAD (HEAD = c6c9a073)
```

`c8f7506b` / `23:55:17Z` is the **exact same flashed image** examined in
`docs/audits/profile_executor_panic_recurrence_2026-09-14.md`. That report's
crash frame:

| field | 2026-09-14 report | **this pass (2026-09-15)** |
|---|---|---|
| `exc_pc` | `0xfffffffd` | `0xfffffffd` |
| `exc_addr` | `0x0` | `0x0` |
| `exc_a0` | `0x3fca0080` | **`0x3fca0080`** (identical) |
| `exc_a1_sp` | `0x3fcb3ae4` | **`0x3fcb3ae4`** (identical) |
| `exc_task` | `profile_executo` | `profile_executo` |

**This is not the same unacknowledged report persisting untouched** — the
boot is a different one. `uptime_s=52938` read at this pass's wall-clock time
(`2026-09-15T18:10:50Z`, confirmed via `date -u`) backs out a boot time of
`2026-09-15T03:28:32Z`, which is *after* the 2026-09-14 audit doc was
committed (`2026-09-14T20:18:28-07:00` = `2026-09-15T03:18:28Z`, ~10 minutes
earlier) — i.e. the board panicked and rebooted again, on the **identical,
unmodified binary**, roughly ten minutes after that report was written and
no one had flashed anything in between (`get_fw_version` confirms `tree:
clean`, same commit/build timestamp both times).

Two independent panics producing byte-identical `exc_a0`/`exc_a1_sp` on the
*same* flashed image is exactly the "same deterministic path, same
allocation" inference the 2026-09-10 report drew for the 09-09/09-10 pair on
*their* shared build (`79d93233`). This is now the **fourth** occurrence of
this exact signature overall (2026-09-09, 2026-09-10, 2026-09-14, and this
one), and the second time two of the four have shared exact addresses on one
unchanged build.

## 4. Symbolization was not possible — both required inputs are gone or absent, not merely unread

**`find_crash_elf()` fails:**

```
error: no archived ELF found for fw_build='Sep 14 2026 16:55:17'
(0 entries in firmware/KilnFW/build/elf_archive/manifest.json)
```

`firmware/KilnFW/build/elf_archive/` currently contains only
`KilnCtrl-latest.elf` (freshly touched today by unrelated build activity —
this working tree has untracked host-test/build artifacts from other
sessions per `git status`). There is no `manifest.json` and no per-hash
archived ELF — specifically, `KilnCtrl-0965ca5755c1.elf`, the archive entry
the 2026-09-14 audit used to match this exact build, is gone. Per this
repo's own rule, **`build/KilnCtrl.elf`/`KilnCtrl-latest.elf` must never be
used to symbolize a crash** since it reflects whatever was most recently
linked, not necessarily what the board is running — and per this session's
CLAUDE.md admonition, no line-number backtrace is guessed in its absence.

**`read_esp_coredump()` fails with `404` on `GET /api/coredump/info`** — and
this is expected, not evidence the coredump partition is empty. The
HTTP coredump-reader endpoint (`diagnostics_http.c`, wired up in
`b38b1498` "Add HTTP-based ESP coredump reader") was committed
**`2026-09-14T21:40:16-07:00` = `2026-09-15T04:40:16Z`** — after the running
image was built (`23:55:17Z` on 09-14) and after the panic that produced
this report (`~03:28:32Z` on 09-15, per §3). Confirmed structurally:

```
$ git merge-base --is-ancestor b38b1498 c8f7506b
b38b1498 is NOT ancestor of c8f7506b
```

The endpoint simply does not exist in the firmware currently on the board.
The tool built specifically to close this gap (`read_esp_coredump`, itself a
response to the "no tool in this repo" finding in the 2026-09-14 audit) is
one flash away from being usable, but the running board predates it. This is
not a defect in the tool; it is a version-skew accident of timing.

**Net result: no raw coredump, no matching ELF, no new backtrace, no
task-stack high-water-mark read.** Nothing beyond the crash-report summary
fields above (§2) could be extracted this pass.

## 5. Is this already fixed at HEAD?

Checked whether anything on the previously-identified deepest chain
(`executor_task_entry -> escalate_guard_trip -> release_profile_relay_claim
-> heat_enable_release -> safety_link_request_enable -> safety_exchange ->
...`) changed between the running commit and HEAD:

```
$ git log --oneline c8f7506b..HEAD -- \
    firmware/KilnFW/App/drivers/control/profile_executor_status.c \
    firmware/KilnFW/App/drivers/control/heat_enable.c
(no output -- zero commits)
```

Confirmed directly in the current tree (which is ahead of the board, at
HEAD):

* `profile_executor_status.c:59` and `:153` still call
  `heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE)` directly from the
  executor task, unchanged.
* `heat_enable.c:202` (`heat_enable_release()`) still calls
  `safety_link_request_enable(s_he.safety, false)` **synchronously**,
  unchanged.
* `check_executor_task_stack_budget.py` still carries `CEILING_BYTES = 1936`
  and `UNMODELED_OVERHEAD_BYTES = 1220` (unchanged from the values the
  2026-09-14 audit measured as "LOW, 940 B / 22.9 % honest headroom").

**Recommendation #2 from the 2026-09-14 audit — deferring
`heat_enable_release()`'s safety-link send off `profile_executor`'s 4096 B
stack onto `safety_poll`'s 8192 B stack — has not been applied at HEAD.**
Nothing else on the implicated path has changed either. This is **not fixed**
at HEAD; it is the same open, un-symbolized, recurring fault the two prior
audits described, now with a fourth confirmed instance and, per §3, evidence
that it can strike twice on one unmodified flashed image within hours.

## 6. What this pass adds, and what it still cannot settle

Adds: a fourth data point, and the first case of exact-address recurrence
*within* a single flashed build over a short window (~10 minutes to next
boot, ~14.7 h of subsequent idle uptime with no further panic recorded).
That strengthens "deterministic recurring path in this task" over "one-off
ISR-timing bad luck" as the more likely explanation, but does not by itself
distinguish the 2026-09-14 report's two remaining hypotheses:

* a plain self-overflow of `profile_executor`'s own 4096 B stack (the
  checker's `UNMODELED_OVERHEAD_BYTES` estimate being an underestimate for
  the real teardown-tick ISR/scheduler cost), or
* adjacent-stack corruption from a neighbouring task (the 2026-09-04
  `safety_poll`/`thermo_owner.c` class already documented in this tree).

Settling that still requires either (a) a raw coredump read against a board
running firmware built after `b38b1498`, giving `exc_a1_sp` against
`profile_executor`'s actual allocated bounds, or (b) a JTAG TCB walk of the
kind attempted (but not completed — the `s_exec.task` field offset was not
resolved) in the 2026-09-14 pass. Neither was attempted here: (a) is
impossible on this board's current image for the timing reason in §4, and
(b) was out of scope for a read-only pass and is a debug-halt operation this
brief explicitly excluded.

## 7. Recommendation (nothing applied)

1. **Flash `b38b1498` or later** (bringing the coredump-reader endpoint
   onto the board) the next time this board is reflashed for any reason —
   at that point, the *next* occurrence of this signature becomes
   symbolizable via `read_esp_coredump()` with no further tooling work
   needed. This is a "make the next data point actually usable" action, not
   a fix.
2. The 2026-09-14 audit's recommendation #2 is now supported by a second,
   independent recurrence and should be prioritized: move
   `heat_enable_release()`'s `safety_link_request_enable()` call off
   `profile_executor`'s tick/halt path onto `safety_poll` (8192 B, generous
   headroom), which the 2026-09-10 report already scoped as the safer
   alternative to enlarging `profile_executor`'s stack.
3. Do not clear or acknowledge the current crash report — it is the fourth
   instance and the owner has not yet reviewed any of them per
   `get_heap_status()`'s standing banner; left present and unacknowledged
   per this brief.
4. As before: do not start another multi-hour firing on this exact build
   before either (1) or (2) lands, since a fifth panic on unchanged firmware
   is the likely outcome and would again produce an unsymbolizable report.

No firmware change, acknowledgment, flash, reset, or relay actuation was
performed by this pass.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
