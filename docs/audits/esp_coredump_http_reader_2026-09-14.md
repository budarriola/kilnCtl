# ESP coredump: HTTP reader added; original dump very likely intact, 2026-09-14

## Context

`docs/audits/esp_coredump_extraction_2026-09-14.md` reports an incident where
a believed-read-only OpenOCD flash-bank probe (`flash banks`/`flash info 0`)
against the live ESP loaded and ran an on-target flasher stub, hung, tripped
the interrupt watchdog, and reset the board -- risking the coredump from the
`profile_executor`/`IllegalInstruction` panic (`exc_pc=0xfffffffd`,
`exc_a0=0x3fca0080`, `exc_a1_sp=0x3fcb3ae4`, build `c8f7506b`) that session
was trying to read. That report recommended two things as follow-up, neither
done at the time: (1) actually determine whether the coredump survived, and
(2) build a firmware-side HTTP coredump reader so this class of investigation
never needs JTAG/OpenOCD against a live board again. This pass does both.

**No `debug_*` tool was called against the ESP peer at any point in this
pass.** Everything below is either static source-code reading or plain HTTP
GETs (`kiln_call(name="get_heap_status")`), the same class of call as any
ordinary dashboard poll.

## Task 1 -- did the original coredump survive?

### Does an interrupt-watchdog reset write a coredump on this configuration?

`firmware/KilnFW/sdkconfig` (read directly, per this repo's standing rule to
never reason from Kconfig defaults -- gitignored `sdkconfig` has burned this
project twice already, per CLAUDE.md's FT6336U note) has:

```
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_ENABLE=y
CONFIG_ESP_COREDUMP_CHECK_BOOT=y
# CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE is not set
```

Traced against ESP-IDF 6.0.2 source (`C:\esp\v6.0.2\esp-idf`):

- `components/esp_system/int_wdt.c`'s `esp_int_wdt_cpu_init()` explicitly
  routes the interrupt-watchdog timeout to "an assembly panic handler (see
  riscv/vector.S and xtensa_vectors.S)" -- i.e. an interrupt-watchdog timeout
  is delivered through the **same** `esp_panic_handler()` path as an
  ordinary exception, not a silent hardware-only reset.
- `components/esp_system/panic.c` (`esp_panic_handler_impl`, guarded by
  `CONFIG_ESP_COREDUMP_ENABLE`) calls `esp_core_dump_write(info)`
  **unconditionally** for any panic reaching that handler, watchdog-sourced
  or not, with only a same-panic-handler re-entrancy guard (`s_dumping_core`)
  -- there is no branch that skips the coredump write for a watchdog cause
  specifically.
- `components/espcoredump/src/core_dump_flash.c`'s
  `esp_core_dump_flash_hw_init()` only refuses to overwrite an existing
  coredump when `CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE` is set (the "empty"
  check and its guarding `#if` are compiled out entirely otherwise). This
  board's sdkconfig does **not** set it.

**Conclusion: in general, on this board's configuration, a second panic --
including one whose only proximate cause is an interrupt-watchdog timeout --
goes through the identical code path that would overwrite an existing
coredump with a new one.** There is no config-level protection against it.
This confirms (and gives source-level grounding to) the prior audit's
inference that the incident's reset was A REAL RISK to the original dump,
not a theoretical one.

### Did it actually happen this time? Live evidence says: no.

`firmware/KilnFW/App/drivers/safety/crash_report.c`'s `crash_report_init()`
runs on **every boot**, unconditionally re-parses whatever is currently in
the `coredump` partition via `esp_core_dump_get_summary()`, computes an
identity hash (`crash_report_dump_id()`, over `exc_pc` + the faulting task
name + the backtrace -- deliberately not a raw struct hash, see that
function's own comment on padding), and only overwrites the persisted NVS
record if that hash differs from what's already stored. So the NVS record's
`exc_task`/`exc_cause_str` reflect **whatever the coredump partition
currently contains, freshly reparsed this boot** -- not a stale cached value
from before the incident.

`kiln_call(name="get_heap_status")`, called fresh in this pass:

```
!!! UNACKNOWLEDGED CRASH REPORT !!! exc_task='profile_executo'
exc_cause_str='IllegalInstruction' reset_reason='PANIC'
reset_reason='interrupt watchdog' uptime_s=639
```

Two distinct `reset_reason` values appear here for a reason: the first
(`'PANIC'`) is the reset reason **captured inside the persisted crash
record itself** at the time that record's coredump was originally written;
the second (`'interrupt watchdog'`) is `get_heap_status`'s own separate,
always-current field naming *this boot's* actual reset cause (this repo's
`reset_reason` field always names the CURRENT boot, not a mid-capture
restart). That is: **this boot was caused by the interrupt
watchdog** (the incident), yet the coredump partition, freshly re-parsed
this same boot, still yields `exc_task='profile_executo'`,
`exc_cause_str='IllegalInstruction'` -- i.e. still describes the *original*
panic, not a new interrupt-watchdog-sourced one.

Had the incident's panic actually reached `esp_core_dump_write()` and
overwritten the partition, the freshly-reparsed summary this boot would
describe *that* panic instead -- a different task context, a different
`exc_pc`, almost certainly a different `exc_cause` (not `IllegalInstruction`
again) -- and `crash_report_dump_id()`'s hash, built from exactly those
fields, would not collide with the stored one by chance. It does not: the
record read back is bit-for-bit the same *identity* as the pre-incident
description.

**Conclusion: the original coredump very likely survived.** Plausible
reading: the flasher-stub hang happened while the CPU was under OpenOCD's
own control (halted / mid-algorithm), a state in which the interrupt-vector
path the software panic handler depends on may not run at all -- in which
case the eventual interrupt-watchdog-attributed reset was a lower-level
hardware reset that never reached `esp_panic_handler()`/
`esp_core_dump_write()` in the first place, consistent with what's actually
observed. This is inference from behavior, not a byte-for-byte read of the
partition (deliberately not attempted in this pass -- see below).

**This was not disturbed further.** No `/api/crash_report/ack`, no
`/api/crash_report/clear`, no coredump-partition read via the new
`/api/coredump/chunk` endpoint added below (untested against the live
board -- see Task 2's own "not flashed" note). The board was not reset, and
no `debug_*` tool was called against `peer="esp"`.

### Current crash-report state (informational only)

Also from the same `get_heap_status()` call: `heap_internal free=68127 B`,
`min_free=34519 B`; the board has been up continuously (uptime 639 s at time
of check) with no further resets since the incident.

## Task 2 -- firmware-side HTTP coredump reader

Two new endpoints in `firmware/KilnFW/App/drivers/http/diagnostics_http.c`,
next to the existing `/api/crash_report*` handlers:

- **`GET /api/coredump/info`** -- `{"ok":true,"present":bool,"data_len":N,
  "partition_size":N,"chunk_size":4096}`. `present`/`data_len` come from a
  new `hal_sysinfo_coredump_get_info()` (`firmware/hwAbstraction/esp/sysinfo/
  hal_sysinfo_esp.c`) that reads the coredump image's own self-reported
  length (the little-endian `uint32_t` espcoredump's on-flash format stores
  at partition offset 0; `0xFFFFFFFF` = never written) via
  `esp_partition_read()`, plus the partition's fixed size from
  `esp_partition_find_first(..., ESP_PARTITION_SUBTYPE_DATA_COREDUMP, ...)`.
- **`GET /api/coredump/chunk?offset=N&len=M`** -- raw
  `application/octet-stream` bytes from `[offset, offset+len)` of the
  `coredump` partition, via a new `hal_sysinfo_coredump_read()` wrapping
  `esp_partition_read()` directly (refuses -- `HAL_INVALID_ARG` --
  rather than truncates a span that would run past the partition's actual
  size). `len` is **clamped to `COREDUMP_HTTP_CHUNK_MAX` = 4096 bytes**
  server-side regardless of what's requested.

**Chunk size and buffer location, per this repo's standing httpd-stack rule**
(`docs/PROJECT_STATUS.md`'s "two documented panics from big locals on the 8 KB
shared httpd task stack", and CLAUDE.md's own `httpd stack blob class` note):
the 4096-byte transfer buffer in `coredump_chunk_get_handler()` is
**heap-allocated** (`malloc`, freed on every return path including error
paths) rather than a stack array of any size -- so this endpoint adds zero
bytes to the httpd task's own stack footprint beyond the handler's few local
scalars. A caller wanting the whole coredump image loops over `/chunk` --
`coredump_fetch.fetch_coredump_over_http()` (below) does exactly that, one
`data_len`-sized image (not the full 1 MB partition) at a time.

`hal_sysinfo.h`/`hal_sysinfo_esp.c` additions are scoped narrowly (raw
partition bytes only, still read-only -- consistent with that header's
existing "esp_partition stays read-only here" line) rather than reopening
its documented six-operation scope; the new functions are additive, and the
header's own top comment now documents why (crash_report.c's own future
extension, matching the pattern its own no-full-summary-reader decision
already anticipated).

Verified this compiles: `ninja -C firmware/KilnFW/build
esp-idf/hwabstraction_esp/.../hal_sysinfo_esp.c.obj` succeeds cleanly, and
`diagnostics_http.c`'s object built cleanly earlier in the same `ninja`
invocation (before it hit an unrelated, pre-existing failure -- see "Known
unrelated failures" below). **Not flashed** -- per this task's explicit
instruction, the board currently holds the (very likely intact) original
coredump and flashing is one of the things that could disturb it; a later
pass with a free board and the owner's awareness is the right time.

### PC-side tool: `tools/PcTools/src/kilnctrl/coredump_fetch.py`

- `get_coredump_info(host)` / `fetch_coredump_over_http(host, out_path)` --
  fetches the coredump's `data_len` bytes in bounded chunks (default 4096,
  matching the firmware's own clamp), writing to a `.part` path and only
  `os.replace()`-ing it into place on a byte-count match with `data_len`,
  so a mid-transfer failure never leaves a file that looks complete. Fails
  loud (`CoredumpFetchError`) on: no coredump present, a self-inconsistent
  `data_len > partition_size`, an empty chunk, or a short overall transfer.
- `symbolize_coredump(coredump_path, elf_path, fw_build=...)` -- runs
  ESP-IDF's `espcoredump.py info_corefile` against the fetched file and the
  given ELF. Refuses before ever invoking espcoredump if `elf_path` or
  `coredump_path` doesn't exist, or `IDF_PATH`/`espcoredump.py` can't be
  found. **Never downgrades or swallows an espcoredump failure**: any
  non-zero exit (which is exactly what happens on esp_coredump's own
  SHA256-mismatch refusal) is re-raised as `CoredumpSymbolizeError` carrying
  the `elf_path`, `fw_build`, and espcoredump's full stdout/stderr verbatim
  -- never a silent fallback to a different ELF chosen by guesswork.
- Wired into the MCP facade as `read_esp_coredump(host=None, out_path=None,
  elf_path=None, symbolize=True)` in `tools/PcTools/src/kilnctrl/
  mcp_server_flash.py`, next to `find_crash_elf`. When `elf_path` isn't
  given, it resolves the board's live `fw_build` and calls the existing
  `elf_archive.find_kiln_elf_for_build()` -- `debug_read_symbol(peer="esp")`
  (fixed in the prior incident's pass, per that audit's "Secondary tooling
  defect" section) is available for further offline symbol lookups against
  whatever ELF this resolves, without rebuilding that capability here.

### ELF-mismatch negative test

`tools/PcTools/tests/test_coredump_fetch.py`, 8 tests, all passing:

- `FetchOverHttpTests` (4 tests) run a real local `http.server` standing in
  for the two firmware endpoints (not a mocked `urllib` call) and prove:
  a full transfer matches the source bytes exactly with no leftover `.part`
  file; `present:false` is refused loudly with no output file created; a
  mid-transfer HTTP failure leaves **no** partial or `.part` file behind;
  and a self-inconsistent `partition_size < data_len` is refused rather
  than silently clamped.
- `SymbolizeMismatchTests` (4 tests, `subprocess.run` mocked -- `esp_coredump`
  is not installed in this repo's own venv, only in the IDF's, per the prior
  audit's own finding) prove: **a non-zero espcoredump exit (the ELF-mismatch
  shape) raises `CoredumpSymbolizeError` naming both the ELF path and the
  `fw_build` that were tried, plus propagates the SHA256 message verbatim**
  (the actual required negative test); a matching/successful run returns
  espcoredump's stdout; a missing ELF file and a missing `IDF_PATH` are both
  refused **before** `subprocess.run` is ever called (asserted via
  `run_mock.assert_not_called()`).

Run: `tools/PcTools/.venv/Scripts/python.exe -m pytest
tools/PcTools/tests/test_coredump_fetch.py -q` -> `8 passed`.

## Full suite results

`tools/PcTools/.venv/Scripts/python.exe -m pytest tools/PcTools/tests -q` ->
**2332 passed, 1 failed, 12 skipped** (929.76s). The one failure,
`test_uart_version_independence.py::test_wire_version_matches_on_both_sides`
(asserts the firmware's wire version constant is `11`; it now reads `12`),
is pre-existing and unrelated to this pass -- the UART link's own protocol
version (11) and the newer `kilnlink`-level API version (12) are two legitimately
different numbers, and this test's hardcoded `== 11` has not been updated
for that split. Not touched here; attributed to whichever session's pass
introduced the version-12 wire change, not this one.

`tools\run_all_checks.ps1` (`-ExecutionPolicy Bypass`, foreground): **88
passed, 0 skipped, 6 failed** on the first run. Two of the six were caused by
this pass and were fixed before this doc was written:

- `tools\check_mcp_facade_coverage.ps1` -- `read_esp_coredump` had no
  taxonomy entry. Fixed: added a `GROUP_OVERRIDES["read_esp_coredump"] =
  "debug"` entry plus `KEYWORDS["read_esp_coredump"]` in
  `tools/PcTools/src/kilnctrl/mcp_facade.py`. Re-run: `kilnctrl OK (155
  tools, all covered)`.
- `tools\check_mcp_tool_count_doc.ps1` -- CLAUDE.md and docs/MCP_SERVERS.md
  both said 154 kilnctrl tools; actual is now 155. Fixed: both counts
  updated (CLAUDE.md's date note bumped to 2026-09-14 alongside it). Re-run:
  `OK (155 tools: 150 decorated + 5 workbench-attached, both docs agree)`.

The remaining four are **pre-existing and unrelated to this pass**, all
pointing at `firmware/KilnFW/App/drivers/persist/kiln_cfg_swap.c` and
`firmware/KilnFW/App/test/test_kiln_cfg_swap.c` -- files explicitly called
out as another session's in-progress `kiln_cfg_store`/config-swap work in
this task's own concurrency instructions, not edited here:

- `firmware\KilnFW\App\test\check_flash_worker_lint.ps1` -- direct
  `hal_kv_set_blob`/`hal_kv_commit` calls in `kiln_cfg_swap.c:119,121`
  outside the flash-worker allowlist.
- `firmware\SaftyFW\tools\check_link_impl_isolation.ps1` -- a CRC
  implementation (`pending_crc()`) in `kiln_cfg_swap.c:47` and
  `test_kiln_cfg_swap.c:344`, outside `firmware/CommonFW`.
- `tools\check_c_files_in_cmakelists.ps1` -- `kiln_cfg_swap.c` not yet
  referenced by `firmware/KilnFW/App/drivers/CMakeLists.txt`.
- `firmware\KilnFW\App\test\check_01_kilnfw_pushed_build.ps1` -- fails
  building `origin/main` (`8d685bfe`) in a clean worktree; this exercises
  pushed history, not this session's working tree, so it is unaffected by
  (and predates) any change in this pass.

Also confirmed independently: `build_kilnfw` (via `kiln_call`) fails with a
pre-existing, unrelated `-Werror=format-truncation=` error in
`firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c:1479` (a `snprintf`
whose format string can produce more than its 192-byte buffer) -- also not
this session's file. To isolate this session's own two changed files from
that unrelated failure, both were compiled directly and individually
confirmed clean: `hal_sysinfo_esp.c.obj` (via targeted `ninja` invocation)
and `diagnostics_http.c.obj` (built successfully earlier in the same whole-
project `ninja` run, before it reached the unrelated `kiln_cfg_store.c`
failure).

`tools\check_doc_hash_citations.ps1`: run after writing this doc, must pass
before commit (hashes cited above, verified via `git cat-file -t <hash>`:
`c8f7506b` commit, `51e1ef5` commit).

## Files touched

- `firmware/hwAbstraction/interface/hal_sysinfo.h` -- new
  `hal_sysinfo_coredump_info_t`, `hal_sysinfo_coredump_get_info()`,
  `hal_sysinfo_coredump_read()`.
- `firmware/hwAbstraction/esp/sysinfo/hal_sysinfo_esp.c` -- implementations.
- `firmware/KilnFW/App/drivers/http/diagnostics_http.c` -- new
  `/api/coredump/info` and `/api/coredump/chunk` handlers + registration.
- `tools/PcTools/src/kilnctrl/coredump_fetch.py` -- new (fetch + symbolize).
- `tools/PcTools/src/kilnctrl/mcp_server_flash.py` -- new `read_esp_coredump`
  tool.
- `tools/PcTools/src/kilnctrl/mcp_facade.py` -- taxonomy entries for the new
  tool.
- `tools/PcTools/tests/test_coredump_fetch.py` -- new, 8 tests.
- `CLAUDE.md`, `docs/MCP_SERVERS.md` -- kilnctrl tool count 154 -> 155.

## Summary for whoever reads this next

1. An interrupt-watchdog reset **does, in general, go through the same
   coredump-writing panic path as any other panic** on this board's
   configuration -- there is no built-in protection against it overwriting
   an existing coredump (`CONFIG_ESP_COREDUMP_FLASH_NO_OVERWRITE` is unset).
2. Despite that, live evidence (a fresh re-parse of the coredump partition
   this boot, via the existing `crash_report_init()`/`/api/crash_report`
   path) says the **original `profile_executor`/`IllegalInstruction` dump
   very likely survived** the incident's watchdog reset unchanged.
3. Nothing further was done to the board to confirm this more directly --
   no coredump bytes were read, no acknowledge/clear, no `debug_*` call.
4. The durable fix -- a firmware HTTP coredump reader plus a PC-side fetch/
   symbolize tool with a real ELF-mismatch negative test -- is built,
   compiles, and is unit-tested, but **not flashed**. Getting the actual
   dump off the board is a follow-up pass, gated on the owner's awareness
   per this task's own instruction, using the now-existing `read_esp_coredump`
   tool once a flash is judged safe.
