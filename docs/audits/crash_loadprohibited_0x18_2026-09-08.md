# Crash audit: `LoadProhibited` / `exc_addr 0x18` / task `main` (2026-09-08)

Status: **NOT root-caused. Crash record deliberately left UNACKNOWLEDGED.**
The record cannot be resolved to a code location with the evidence that exists,
and this pass is a refusal to guess plus the instrumentation that makes the next
occurrence answerable.

## 1. The record, verbatim

`GET http://kilnctl.local/api/crash_report`, 2026-09-08:

```json
{"present":true,"acknowledged":false,"exc_cause":28,"exc_cause_str":"LoadProhibited",
 "exc_pc":"0xfffffffd","exc_addr":"0x00000018","exc_task":"main",
 "found_on_boot_reset_reason":"PANIC","backtrace":["0xfffffffd"],
 "backtrace_corrupted":true}
```

Board context at the same moment (all OBSERVED):

| field | value |
|---|---|
| running partition | `factory` |
| commit / dirty | `fc90f682` / `dirty: true` |
| fw_version | `V1.0_Purchased_This_Board-1569-` |
| fw_build | `Sep  7 2026 22:00:46` (`build_date` `2026-09-08 05:29:10Z`) |
| `recovery_mode` | **true** |
| `reset_reason` (this boot) | `panic/exception` |
| `/api/cfgfs` | `mounted: false`, "not mounted this boot -- recovery mode skipped the mount" |

## 2. The board is NOT in a reboot loop (correction)

`5e9dba8c` and a mid-pass escalation both described an *active* panic/reboot
loop, inferred from `uptime_s` appearing to drop across polls.

OBSERVED, by direct polling of `/api/status`:

- 07:44 `uptime_s=803` -> 07:49 `uptime_s=157`. One reboot, at ~07:46:37.
- 07:51 -> 08:01, sampled every 10 s for 10 minutes: `uptime_s` rose
  monotonically 275 -> 859 with no discontinuity.
- 08:35:37: `uptime_s=2940`. Same boot. **49 minutes of continuous uptime.**

So there is exactly one reboot in the observed window, and the board has been
stable ever since. `reset_reason: panic/exception` does not mean "panicking
now" -- it names the cause of the *current* boot and stays fixed for its whole
life (memory: "reset_reason names the current boot"). Anyone re-deriving a loop
from that field alone will reach the same wrong conclusion again.

What the board actually is: **trapped in recovery mode, and stable there.**
That is self-consistent with the crash. `boot_guard` counts unconfirmed boots;
once over threshold this boot came up in recovery mode, and recovery mode skips
`cfg_fs_mount_device()` and the control subsystems. INFERRED: the panic lives in
a path recovery mode does not execute, which is why it has not recurred in 49
minutes and why the fault cannot be observed live from here.

## 3. The backtrace is unusable, and so is `exc_addr`

`exc_pc` is not the raw saved PC. espcoredump stores
`esp_cpu_process_stack_pc(raw_pc)` (`components/espcoredump/src/port/xtensa/core_dump_port.c:502`),
and that helper is (`components/xtensa/include/esp_cpu_utils.h:20`):

```c
if (pc & 0x80000000) { pc = (pc & 0x3fffffff) | 0x40000000; }
return pc - 3;
```

`0x00000000 - 3 == 0xfffffffd`. **The saved PC was exactly zero.** The single
backtrace entry is that same value, and `corrupted` is set.

This is decisive about what may be concluded:

- A PC of 0 is not a code address in any KilnCtrl image. Symbolizing it is
  impossible; no ELF makes `0xfffffffd` resolve. A previous agent refused to
  symbolize the older `0xfffffffd` record and that refusal was correct -- it is
  correct here for the same arithmetic reason.
- A *load* instruction cannot be executing at address 0. Address 0 would fault
  on instruction fetch (`EXCCAUSE 20`, InstrFetchProhibited), not `EXCCAUSE 28`
  (LoadProhibited). The frame's `pc` and its `exccause`/`exc_vaddr` are
  therefore **mutually inconsistent** -- a frame that was partly overwritten,
  the classic signature of the crashing task's own stack being smashed. The
  same signature (`exc_pc 0xfffffffd`, `backtrace_corrupted`) was produced by
  the *confirmed* main-task stack overflow of `3c36b7e1` /
  `docs/audits/boot_hang_2026-09-08.md`.

Consequence: **`exc_addr 0x18` must not be read as a struct field offset.**

The "0x18 means a null struct pointer dereferenced at field offset 0x18"
reading is a real and usually good heuristic -- and for the record, offset
`0x18` in `esp_partition_t` is exactly `char label[17]`, which the boot path
handles a lot of, so the trail looks inviting. But it only holds if the *stack
pointer* was sane. If `a1` was itself NULL or garbage (which is what a smashed
frame means), then `l32i aX, a1, 0x18` -- an ordinary load of a local at frame
offset 0x18, from any function anywhere -- produces precisely `EXCCAUSE 28` +
`exc_vaddr 0x18`, with no null struct pointer involved at all. The two readings
have completely different fixes and the record as captured **cannot tell them
apart**, because it never stored `a1`. That gap is the real defect found by
this pass, and it is what section 5 fixes.

## 4. ELF matching: no match exists

The running build is `-1569-`, `Sep  7 2026 22:00:46`, built from a *dirty*
tree. Every `KilnCtrl*.elf` on this machine was enumerated and its embedded
`esp_app_desc_t` read:

| ELF | version | build |
|---|---|---|
| `firmware/KilnFW/build/KilnCtrl.elf` (+ `elf_archive/KilnCtrl-308877a71382.elf`) | `-1566-` | Sep 7 2026 20:32:10 |
| `C:/wt/espflash/.../KilnCtrl.elf` (+ its `elf_archive/KilnCtrl-454a7111a2ce.elf`) | `-1568-` | Sep 7 2026 21:36:24 |

Nothing carries `-1569-` / `22:00:46`. The archive entry it would have been
written to no longer exists, and since the tree was dirty at build time that
exact image is not reproducible from `fc90f682` either. Rebuilding was **not**
done: the only thing an ELF buys here is symbolization of `0xfffffffd`, which
no ELF can provide. Structural reasoning about `esp_partition_t` used the
`-1568-` build, whose firmware source is identical to `fc90f682` (`fc90f682`
changed only a plan `.md` over `0b6e82b7`).

`check_main_task_stack_budget.py` was run against the `-1568-` ELF: deepest
*static* path from `app_main` is 4864 B against an 8192 B stack. That does not
clear a stack overflow -- the check cannot follow indirect calls (VFS/LittleFS
dispatch, function pointers), which is where the remaining depth would hide.

## 5. What was changed

No speculative null check was added. Adding one where the evidence does not
support a null pointer would have buried the defect behind a plausible-looking
fix -- the exact failure mode the "never acknowledge an unexplained crash" rule
exists to prevent.

What was fixed is the thing that made this crash undiagnosable:

1. `crash_report_record_t` now stores `exc_a0` and **`exc_a1`, the crashing
   frame's stack pointer** (both already present in
   `esp_core_dump_summary_extra_info_t.exc_a[]`, previously discarded). `a1` is
   the single field that decides section 3's ambiguity in one glance: a sane
   `a1` means a null struct pointer at field offset `0x18`; a null or garbage
   `a1` means a smashed stack and a meaningless `exc_vaddr`.
   `CRASH_REPORT_RECORD_VERSION` 1 -> 2, size check 152 -> 160.
2. New pure predicate `crash_report_frame_trustworthy()`: false when the
   backtrace is corrupted, or when `exc_pc == 0xfffffffd` (the
   `esp_cpu_process_stack_pc(0)` sentinel). `crash_report_init()` now logs a
   loud line saying the frame is not self-consistent and that `exc_addr` must
   not be read as a struct offset.
3. `GET /api/crash_report` gained `exc_a0`, `exc_a1_sp` and
   `frame_trustworthy`; the Diagnostics page renders both registers and, when
   the frame is untrustworthy, prints the warning in words next to them.

Host tests (`test_crash_report.c`, both new sections pass; full host suite
31/31 executables built, `test_crash_report` all green):

- `exc_a0`/`exc_a1` round-trip through NVS, plus a negative test proving `a1`
  is inside the record CRC (flipping it alone invalidates the record) rather
  than appended outside the integrity check.
- `crash_report_frame_trustworthy()` is exercised on the **exact live record**
  (cause 28 / addr 0x18 / pc 0xfffffffd / bt corrupted) and refuses it; on a
  healthy record, where it returns true (so the predicate is not vacuously
  false); and on each condition in isolation, so neither is carrying the other.

The host-test stub `stubs/esp_core_dump.h` gained `exc_a[16]` to match the real
`esp_core_dump_summary_extra_info_t`.

## 6. Reflash

**No reflash is required for stability.** The board has been up 49 minutes and
is answering HTTP; it is trapped in recovery mode, not looping. Flashing a
"last known-good" build was considered and rejected on that basis: it would
destroy the `coredump` partition image that is currently the only remaining
first-hand evidence of this crash, and it would not fix anything presently
broken.

The changes in section 5 are diagnostics for the *next* occurrence and take
effect only when something is flashed for another reason. Note that flashing
them discards the current v1 record (version bump) -- acceptable only because
section 1 transcribes it in full.

## 7. Next step, when a power cycle is available

The experiment that would settle this is to leave recovery mode
(`POST /api/ota/esp/recovery_exit`) and let a normal boot run
`cfg_fs_mount_device()` again, with the new `a1` capture in place. It was NOT
run in this pass: the owner cannot power-cycle today, and this board has twice
been bricked into a permanent recovery loop from this area (`e7b8efc`,
2026-08-22, and `e916b120`). Re-entering the crashing path with no physical
recovery available is not a trade worth making for a board that is currently
reachable and safe.

The alternative, which needs no reboot at all: read the `coredump` partition
(1 MiB at `0x00BF0000`, per `/api/partitions`) over JTAG and decode it with
`espcoredump.py`. That gives the full register set and stack directly -- it is
what would have answered this in one step. It was not done here because
OpenOCD's ESP flash read runs a stub on the halted target and effectively
forces a reset, and `debug_reset` clears the RTC slow memory another agent's
recovery work depends on.
