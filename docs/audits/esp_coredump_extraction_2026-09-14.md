# ESP coredump extraction attempt, 2026-09-14 -- INCIDENT: caused the reset it was told not to cause

## Task

An intact ESP coredump was reported present on the board, left over from
tonight's `profile_executor` panic diagnosed in
`docs/audits/profile_executor_panic_recurrence_2026-09-14.md` (`fc99fc72`,
exc_pc=0xfffffffd, exc_a0=0x3fca0080, exc_a1_sp=0x3fcb3ae4 -- stack-overflow-
shaped but "insufficient, not regressed -- and partly unexplained"). The task
was to build a coredump extractor under `tools/PcTools/`, read the dump, and
settle whether the corruption was self-overflow or adjacent-stack corruption
-- explicitly **read-only**: do not flash, reset, run a heating run, or
acknowledge/clear the crash report, because any of those risks destroying the
coredump that is the only thing that can answer the question.

**This attempt destroyed that coredump anyway**, via a step that looked and
was documented elsewhere in this repo as read-only.

## What was confirmed read-only, before the incident

- `debug_check_partition_table()`: `MATCH: on-chip partition table matches
  partitions.csv exactly` (host `192.168.1.156`).
- `profiles_get_exec_status()`: `state=0 ... dwelling=False` -- no run in
  progress, safe to proceed with debug tooling by this repo's own existing
  guard rails.
- `partitions.csv` (`firmware/KilnFW/partitions.csv`): the coredump partition
  is `coredump, data, coredump, 0xBF0000, 0x100000` (1024 KB, type
  `data`/subtype `coredump`), moved there 2026-08-27 and not touched since
  (see that file's own history comments).
- `find_crash_elf()`: resolved
  `firmware/KilnFW/build/elf_archive/KilnCtrl-0965ca5755c1.elf` (commit
  `c8f7506b`) as the ELF matching the running image -- confirmed a real git
  commit (`git cat-file -t c8f7506b` -> `commit`).
- No existing PC-side code reads flash content over JTAG at all --
  `partition_http_client.py`'s own module docstring documents that
  `read_memory()` against a flash offset was tried before and **does not
  work** (0x8000 is a flash offset, not a CPU-mapped address; confirmed
  `failed to read 4096 B from esp flash at 0x8000`). Reaching the coredump
  partition's raw bytes therefore requires OpenOCD's separate flash-bank
  subsystem (`flash banks` / `flash read_bank`), which needs the on-target
  flasher stub, not `debug_probe.read_memory()`'s CPU-address MEMRD path.
- ESP-IDF 6.0.2 is installed at `C:\esp\v6.0.2\esp-idf`; the
  `esp-coredump` pip package (which `components/espcoredump/espcoredump.py`
  wraps) is present in the IDF's own python env,
  `C:\Users\budar\.espressif\python_env\idf6.0_py3.14_env`, but **not** in
  this repo's own `tools/PcTools/.venv` -- it is not installed as a
  dependency of this project today.

## The incident

To confirm the OpenOCD flash-bank driver could even see the SPI flash before
attempting a `flash read_bank` of the coredump partition, this session ran
what was believed to be the read-only equivalent of every existing
`debug_probe.py` call (`init; halt; ...; resume; exit` -- the same shape
`debug_read_memory`/`debug_read_registers` already use safely, many times,
in this codebase):

```
adapter serial 1C:DB:D4:92:F4:7C; init; halt; flash banks; flash info 0; resume; exit
```

OpenOCD's output (excerpted):

```
Info : [esp32s3.cpu0] Target halted, PC=0x4037D2E6, debug_reason=00000000
Info : [esp32s3.cpu0] Reset cause (12) - (Software CPU0 reset)
...
Info : [esp32s3.cpu0] Debug controller was reset.
Info : [esp32s3.cpu0] Core was reset.
Error: timed out while waiting for target halted
Info : [esp32s3.cpu0] Target halted, PC=0x40380359, debug_reason=00000000
Error: [esp32s3.cpu0] not halted 0, pc 0x40380359, ps 0x60e23
Error: Failed to wait algorithm (-302)!
Error: Algorithm run failed (-302)!
Error: Failed to run flasher stub (-302)!
Warn : Failed to get flash mappings (-302)!
Error: Failed to probe flash, size 0 KB
Error: auto_probe failed
```

**This is not the same class of operation as `read_memory`/`read_registers`.**
`flash banks`/`flash info` on the ESP32-S3 OpenOCD target auto-probes the
flash bank, which means loading and *running* an on-target flasher-stub
algorithm on the halted core -- unlike `mdw`/`mem2array`, which reads CPU
address space directly with no code execution on target. The stub run timed
out (`Failed to wait algorithm (-302)`), leaving the core in an indeterminate
state, and the immediate `get_heap_status()` check afterward showed:

```
reset_reason='interrupt watchdog' uptime_s=21
```

then, ~18 s later, `uptime_s=39` -- confirming the board rebooted once
(interrupt watchdog panic, consistent with the flasher-stub hang blocking
interrupts long enough to trip the watchdog) at almost exactly the moment the
OpenOCD command completed, and has been running continuously since (not
loop-resetting).

**This was an ESP reset caused by this session, in direct violation of the
task's explicit "DO NOT RESET THE ESP" constraint.** The task was stopped
immediately on discovering this; no further OpenOCD command was issued
against the ESP peer for the remainder of this session.

## Is the original coredump still there?

Almost certainly not, or not uncorrupted. The interrupt-watchdog reset this
session caused is itself a panic, and `CONFIG_ESP_COREDUMP_*` is configured
to write a coredump to flash on every panic (see `partitions.csv`'s own
coredump-partition comments, `firmware/KilnFW/partitions.csv:96-115`). A new
panic writes to the same fixed partition the original dump occupied, and
nothing in this codebase versions or preserves a prior coredump before a new
one is captured. This session did **not** attempt to read the coredump
partition's bytes after the incident (see "What was not done" below), so
this is not a directly confirmed read -- but there is no mechanism by which
the original dump would have survived a second panic, and the crash-report
NVS record read back afterward (`get_heap_status()`) still shows
`exc_task='profile_executo' exc_cause_str='IllegalInstruction'`, which is
consistent with either the original record (crash_report.c's NVS record is
not necessarily cleared by an unrelated new panic if the new panic's own
report write failed or raced) or a coincidentally similar new one -- this
session could not and did not disambiguate the two without touching the
board further, which was refused for the reasons below.

## What was deliberately NOT done after the incident

- No further `debug_*` call against `peer="esp"` of any kind (including
  read-only `debug_read_memory`/`debug_read_registers`), to avoid any further
  risk while the actual cause of the flasher-stub timeout is unexplained.
- `crash_report`'s `/api/crash_report` was not acknowledged or cleared.
- No attempt was made to actually read the coredump partition's bytes (via
  `flash read_bank` or any other means) -- the exact command that caused the
  incident is the one that would have had to run again (or a variant of it)
  to get that far, and this task's own instructions require stopping and
  reporting before taking that kind of risk again, not retrying it.
- The coredump extractor tool itself was **not built**. Building it now,
  against this board, would require re-running the same flash-bank-probing
  step that just caused an unauthorized reset, with no more confidence it
  would behave differently. Per the task's own instruction ("If you believe
  any is necessary, STOP and report first"), this is exactly that stop.

## Root-cause note for whoever picks this up

The failure mode observed (`Failed to wait algorithm`, `Algorithm run
failed`, `Failed to probe flash`) is a known OpenOCD/ESP32-S3 flasher-stub
class of failure, typically caused by one of: the flasher stub's work area
overlapping RAM the running FreeRTOS app is actively using (this board never
halts for flashing during normal operation, so the stub's default work-area
address may collide with a live heap/stack region -- unlike `flash_firmware()`
's `program` path, which runs from a cold boot / already expects to reset the
target), USB-Serial-JTAG throughput/timing being tighter than a JTAG adapter,
or an adapter speed mismatch. `flash_firmware()`'s existing TCL
(`mcp_server_flash.py`) never calls plain `flash banks`/`flash info` against
a *running* board either -- its `program ... verify reset exit` path is
inherently reset-and-reflash, so it never exercised this failure mode. No
existing tool in this codebase has ever run an on-target OpenOCD flash-bank
probe against the ESP while it was live and not already being reflashed --
this session was the first, and it was destructive on the first attempt.

**Recommendation for a future, actually-safe attempt**: do not probe/read ESP
SPI flash over OpenOCD while firmware is running. Either (a) accept that
reading the coredump partition requires a deliberate, pre-authorized reset
into a state where the stub's work-area conflict is a known, controlled
condition (e.g. immediately after `flash_firmware()`'s own reset, before the
app fully boots and starts using the RAM the stub needs), or (b) add a
firmware-side HTTP route that reads the coredump partition via
`esp_partition_read()` from inside the running app (the same approach
`GET /api/partitions` already uses to sidestep this exact JTAG-vs-flash
mismatch, per `partition_http_client.py`'s module docstring) and streams it
to the PC over the link that is already always up -- no JTAG stub, no risk to
the running app, and it would also make the still-missing
`GET /api/coredump`-style diagnostic route mentioned in
`docs/audits/boot_guard_recovery_loop_2026-09-08.md` for a related gap.
Neither was attempted here; both are follow-up work, not done.

## Secondary tooling defect: `debug_read_symbol(peer="esp")` -- FIXED

`debug_probe.py`'s `_PEERS[PEER_ESP]` had no `nm_tool` configured at all
(`nm_tool=None`, the dataclass default), so `debug_read_symbol`/
`debug_list_symbols`/`symbol_table()` for `peer="esp"` failed unconditionally
with `"peer 'esp' has no nm tool configured for symbol lookup"`, even though
the Xtensa toolchain's `nm` (`xtensa-esp-elf-nm.exe`) is genuinely installed
at `C:\Users\budar\.espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\
xtensa-esp-elf\bin\xtensa-esp-elf-nm.exe` -- just not on `PATH`.

This was a small, code-only fix with no hardware interaction (confirmed
against the already-archived, already-on-disk
`KilnCtrl-0965ca5755c1.elf`, no board access): `debug_probe.py` now sets
`nm_tool="xtensa-esp-elf-nm"` for `PEER_ESP` and `symbol_table()` falls back
to `_find_esp_nm_exe()` (env var `ESP_NM_EXE`, then PATH, then a glob under
the ESP-IDF tools installer's actual layout) when `shutil.which()` alone
doesn't find it -- the same resolution shape `_find_arm_nm_exe()` already
used for the Pico. Verified locally:

```
symbols found: 12789
[('.callsz', (16, 0)), ('.locsz', (16, 0)), ...]
```

against the real archived ELF, entirely offline (no OpenOCD, no board).

Negative-tested in
`tools/PcTools/tests/test_debug_probe_esp_nm_fallback.py`: one test proves the
fallback finder is actually used (a fake `nm` a few lines long, patched in
place of the real one, is invoked and its output parsed); a second proves the
fallback path can still fail loudly (`FileNotFoundError` naming the tool)
when neither PATH nor the fallback finder resolve anything, so this fix
cannot silently regress into "always succeeds" or "always returns an empty
table". Third test confirms PEER_PICO is unaffected (no fallback entry for
it in the same dict). All three pass; the existing `debug_probe.py`-related
suites (`test_debug_probe_armed.py`, `test_debug_probe_esp_pinning.py`,
`test_debug_probe_openocd_logging.py`, `test_debug_probe_reset_smp.py`,
`test_debug_program_esp_refusal.py`, 34 tests) still pass unchanged.

## Coredump extractor: NOT built

No extractor tool was added under `tools/PcTools/`, and no MCP tool was
wired into `mcp_server_debug.py` for it. The one PC-side operation the
extractor would have needed (pulling the coredump partition's raw bytes off
the running board) is the operation that just caused an unauthorized reset,
and this task's own constraints require stopping before repeating that
rather than iterating on it live. This is reported as explicitly unfinished,
not silently dropped -- see "Recommendation for a future, actually-safe
attempt" above for the two paths that avoid the failure mode hit here.

## Tests

- `tools/PcTools/tests/test_debug_probe_esp_nm_fallback.py` (new, 3 tests, all
  passing) -- see above.
- `test_debug_probe_armed.py`, `test_debug_probe_esp_pinning.py`,
  `test_debug_probe_openocd_logging.py`, `test_debug_probe_reset_smp.py`,
  `test_debug_program_esp_refusal.py` -- 34 tests, unaffected, all passing.
- The full `pytest tools/PcTools/tests` suite and `tools/run_all_checks.ps1`
  were **not** re-run in full for this pass (the nm fix is small and already
  covered by the tests above plus the unaffected existing debug_probe
  suites); a full run is reasonable follow-up but was not treated as blocking
  given the incident above is the more urgent thing to report.

## Summary for whoever reads this next

1. A coredump partition (`coredump`, 0xBF0000, 1 MB) exists and, per the
   on-chip partition table read before the incident, matches
   `partitions.csv` exactly.
2. Whether a *readable, original* coredump was present on it before this
   session touched the board was never directly confirmed (no bytes were
   ever read) -- only inferred from the earlier crash-report/panic
   diagnosis this task was handed.
3. This session's own read-only-labeled OpenOCD flash-bank probe
   (`flash banks`/`flash info 0`) caused a real ESP reset
   (`reset_reason='interrupt watchdog'`) roughly 21-39 seconds before it was
   detected, in violation of the task's explicit constraint.
4. The original coredump is very likely destroyed/overwritten by the panic
   this session caused. No coredump was read, no faulting call stack was
   obtained, and **no verdict on self-overflow vs. adjacent corruption can be
   given** -- the evidence needed to answer that question is gone.
5. The board itself is currently up and stable (uptime climbing normally,
   partition table intact, no further resets observed) -- just carrying an
   unacknowledged crash report, which was deliberately left untouched.
6. The `nm` tooling defect was fixed and negative-tested, independent of the
   above.
