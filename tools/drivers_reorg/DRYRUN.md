# drivers/ reorg dry run (2026-09-05, revised)

Output of `tools/drivers_reorg/plan_moves.ps1 -DryRun` against the
coordinator-revised `mapping.csv` (decisions A-E below). Nothing was applied;
this whole pass is preparation only.

## -1. Opus pre-apply review pass (2026-09-05, this session) -- 12 blockers fixed

A second Opus review of `f4411bb` found 12 blockers before `-Apply` could
ever be trusted. All 12 are fixed in `plan_moves.ps1`/`mapping.csv` this
session; `stack_margin.h` was touched only transiently for negative test 8
below and is byte-identical to HEAD afterward (`git diff` empty).

1. **`build_host_tests.ps1`'s `Join-Path $driversDir "<name>"` bare-basename
   form.** New `$joinPathPattern` in section 4b rewrites the quoted
   basename argument to `"<layer>/<name>"` (e.g. `(Join-Path $driversDir
   "pid.c")` -> `(Join-Path $driversDir "control/pid.c")`) -- confirmed via
   `-PreviewDir` against all ~30+ occurrences (more than the "~30, lines
   90-114/205-207/220" the coordinator named; every `Join-Path $driversDir
   "<name>"` site across cmd1-cmd23 is covered, not just the three ranges
   originally called out). The same pass expands any literal `/I`"$driversDir`"`
   flag into one `/I` per layer subdir (`$layerNames`), since a bare
   `#include` that used to resolve via `$driversDir`'s single flat directory
   now needs the whole layer set on the include path.
2. **`$candidateFiles` was `*.ps1,*.py` only.** `firmware/KilnFW/App/test`
   now also scans `*.c,*.h` (every other search dir stays script-only) --
   this is what makes the 153 `#include "../drivers/<file>"` host-test-mirror
   lines rewritable at all; they were invisible to the old filter.
3. **Path-qualified `espInterfaces/` includes inside `drivers/` itself.**
   New section 4c scans `drivers/**/*.c,*.h` for bare `#include
   "espInterfaces/<name>"` (no `App/drivers/`/`../drivers/` prefix -- these
   are real in-tree production includes, not check-script literals) and
   strips the segment: `gpio_probe.h:5`, `uart_bridge.h:6`,
   `uart_bridge_internal.h:30`, `uart_log_bridge.h:5` all confirmed rewired
   to bare `#include "uart_protocol.h"` in a `-PreviewDir` run (4/4 files, 4
   lines).
4. **`KILNCTL_GZIP_ASSETS` quoted asset literals must not be rewritten in
   place.** Section 2 now carves that whole `set(...)` block out via a
   placeholder before the generic per-literal rewrite runs, and splices it
   back verbatim; a dedicated section 2c instead rewrites each of the 16
   asset entries to `"<layer>/<basename>"` (they span more than one layer --
   14 are `http`, but `ota_page.html` and `wifi_provision_page.html` are
   `net` per `mapping.csv`) and restructures the `kilnctl_gz_src`/
   `kilnctl_gz_out` pair so `kilnctl_gz_src` reads the layer-qualified path
   while `kilnctl_gz_out` is rebuilt from `get_filename_component(...
   NAME)` -- flat, so the `EMBED_TXTFILES`
   `_binary_<name>_gz_start`/`_end` symbol names are provably unchanged by
   this move. Verified in the `-PreviewDir` copy (see section 2c output
   below) and by running the rewritten `CMakeLists.txt` through `cmake -P`
   (see "Preview verification" below).
5. **`tuning_recommendations_fallback.json` embedded in a longer string.**
   The generic literal rewrite is now one combined regex per basename,
   `(["/])<basename>"`, matching both the bare-quoted SRCS form and a
   basename embedded after a `/` in a longer path, and echoing back
   whichever character preceded it. `"${CMAKE_CURRENT_SOURCE_DIR}/tuning_
   recommendations_fallback.json"` -> `.../http/tuning_recommendations_
   fallback.json"`, confirmed in the preview copy.
   **Bug found and fixed while implementing this**: the first version ran
   the bare-quote and embedded-path substitutions as two *separate*
   sequential `-replace` calls against the same mutated `$text`; the second
   pass re-matched the `/` the first pass had just inserted before the new
   `"<layer>/<basename>"` text and double-prefixed it (`"hw/hw/SX1509.c"`,
   caught across the entire SRCS list in a `-PreviewDir` run). Fixed by
   using one combined pattern/replacement per basename instead of two.
6. **`INCLUDE_DIRS` regex swallowed the trailing newline+indent.** The
   original `(?:"[^"]*"\s*)+` capture group's own trailing `\s*` includes
   the whitespace *inside* group 1's captured value -- so "replace only
   Groups[1]" alone (the first fix attempt) still deleted that whitespace
   and reproduced `"ui"PRIV_INCLUDE_DIRS` in a `-PreviewDir` run. Real fix:
   trim the replacement span to end right after the *last* `"` inside
   group 1's captured text (`$lastQuoteIdx`), leaving every byte after it
   -- including the newline and indentation before `PRIV_INCLUDE_DIRS` --
   completely untouched. A whitespace-preservation assertion was added
   (`$tailAfterOld -eq $tailAfterNew`, comparing the 40 chars following the
   old and new spans) so a future edit that reintroduces this class of bug
   fails loud instead of shipping a syntax error. `"."` is kept in the new
   list (`$includeDirsEntries = @(".") + $layerNames`) per the coordinator's
   instruction -- dropping it would have been an unstated behavior change.
   Confirmed via `-PreviewDir`: `INCLUDE_DIRS "." "bridge" "common"
   "control" "http" "hw" "net" "owners" "persist" "safety" "sim" "ui"` on
   its own line, `PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"` still on
   the very next line with its original indentation.
7. **Section 0: CSV old_path/new_path duplicates and nonexistent old_path.**
   New section 0a. Duplicate `old_path`/`new_path` rows are a pure
   data-authoring bug independent of repo state, so they fail on *every*
   invocation including `-DryRun` (negative-tested below). A nonexistent
   `old_path` is allowed to be a *pending* row (a mapping.csv entry for a
   file another in-flight session is expected to add) so it only hard-fails
   under `-Apply`; `-DryRun` reports it as a warning naming the file. The
   missing-old_path count is 0 in the current dry run -- every mapped file
   is on disk today -- so the -Apply-only gate is exercised by the
   mechanism, not by any specific row, in this pass.
8. **Section 5 (upward-include verifier) now hard-fails `-Apply`.**
   Previously it only printed the list. `if ($violations.Count -gt 0 -and
   $Apply) { exit 1 }` added -- negative-tested below by injecting a fake
   upward include into a real driver header, confirming the count and the
   named violation both appear, then reverting the file byte-for-byte.
9. **Coordinator tier decisions applied.** `sim` moves from the bottom tier
   (`owners/hw/sim`) to the MID tier (`control/safety/persist/net/sim`) in
   `$tierOf` -- a sim backend legitimately reads config the way
   control/persist/net do, not a bottom-tier device driver. `factory_reset.c`/
   `.h` move from `persist` to `http` in `mapping.csv` (it registers an
   httpd route, same as the rest of the `*_http.c` family). `mapping.csv` has
   a row for `firmware/KilnFW/App/drivers/profiles_store.h` (persist); the
   `profile_executor_run.c -> profiles_http.h` residual itself was
   deliberately left untouched, as instructed.
10. **Section 6 `$already` scoping bug.** `$already`/`$missingEntries` are
    now computed once inside section 2b (from `$includeDirsEntries`, which
    includes `"."`) and exposed via `$script:includeDirsAlreadyCount` /
    `$script:includeDirsClauseMissing`, read back explicitly by section 6's
    summary line, which now prints "INCLUDE_DIRS clause not found" by name
    if section 2b couldn't locate the clause at all, instead of silently
    treating a missing clause the same as "0 dirs to add".
11. **Working-tree cleanliness gate for `-Apply`.** New `-AllowDirty`
    switch; `-Apply` (without it) runs `git status --porcelain --
    firmware/KilnFW/App/drivers firmware/KilnFW/App/test
    firmware/KilnFW/App/drivers/CMakeLists.txt tools/` before touching
    anything and refuses if it's non-empty, naming the dirty files. This
    check was never exercised in this session (per the sandboxing rule,
    `-Apply` itself is never invoked here) but was read-reviewed against
    the current (dirty, per the session's own git status) tree by hand: the
    `git status --porcelain -- <paths>` invocation and message format were
    confirmed correct by inspection, not by running `-Apply`.
12. **Markdown citations of `App/drivers/<file>[:line]`.** 111 `.md` files
    are now scanned (`docs/`, `firmware/**/docs`, any `firmware/**/*.md`,
    plus the root `ROADMAP.md`/`CLAUDE.md`/`TODO.md` family) through the
    same `$siteRewritePattern`/rewrite loop as the check/test scripts --
    111 files found, 25 of them actually cite a moved `App/drivers/<file>`
    (part of the 641 total sites across 188 files in the final summary
    below). `check_uri_handler_cap.ps1`,
    `check_nvs_write_guard_coverage.ps1` and
    `check_host_embed_symbols_defined.ps1` are explicitly excluded by name
    from every scan (`$otherAgentOwnedScripts`) since another agent is
    making them layout-agnostic in parallel -- this rewriter never touches
    them, avoiding a conflicting edit.

### Negative tests (both performed this session, both reverted)

- **Finding 7 (mapping.csv duplicate row).** Appended a bogus row
  (`firmware/KilnFW/App/drivers/pid.c,.../NEGATIVE_TEST_BOGUS_DUPLICATE.c,...`)
  duplicating `pid.c`'s existing `old_path`, ran `plan_moves.ps1 -DryRun`:
  exited 1 with `mapping.csv: duplicate old_path
  'firmware/KilnFW/App/drivers/pid.c'`. Removed the row immediately after;
  `mapping.csv` is back to 364 lines with zero `NEGATIVE_TEST` occurrences.
- **Finding 8 (fake upward include).** Temporarily inserted `#include
  "pid.h"` (control, tier 1) as the first line of
  `firmware/KilnFW/App/drivers/stack_margin.h` (common, tier 3 -- a real
  upward include by construction). `plan_moves.ps1 -DryRun` reported "2
  upward include(s) remain" (up from the baseline 1) and named the
  injected one explicitly: `stack_margin.h [common]:1 includes "pid.h"
  [control] -- bottom-tier file including a higher-tier header`. Reverted
  the single inserted line immediately after; `git diff -- firmware/KilnFW/
  App/drivers/stack_margin.h` is empty (byte-identical to HEAD). This
  demonstrates the exact detection `-Apply` gates on (finding 8) without
  ever invoking `-Apply` itself, per this task's sandboxing rule.

### Final refreshed dry-run summary (this session, after all 12 fixes)

```
=== 6. Dry-run diff preview (counts) ===
  git mv                         : 359 file(s), 4 STAY (no-op)
  CMakeLists SRCS rewrite        : 186 literal(s) in drivers/CMakeLists.txt, 0 in App/CMakeLists.txt
  INCLUDE_DIRS rewrite           : 11 of 12 '.'+layer dir(s) to add (0 = already up to date)
  Path-keyed site rewrite        : 641 line(s) across 188 file(s) (incl. markdown + Join-Path $driversDir sites)
  espInterfaces/ include strip   : 4 line(s) across 4 file(s)
  Unmapped 'drivers/<name>' hits : 40 (of which 0 would hard-fail -Apply)
  Upward includes remaining      : 1 (would hard-fail -Apply)
  Missing old_path row(s)        : 0 (would hard-fail -Apply; benign in -DryRun -- see section 0a)
```

The 0-genuinely-unmapped figure reflects one more fix made while writing
this pass: `$knownPlaceholders` grew from 6 to ~30 entries to absorb two new
false-positive shapes the markdown scan (finding 12) surfaced -- bare
module-name prose mentions with no file extension (`board_temps`,
`profiles_http`, `sim_backend`, ...) and UnitTestFw hardware-component
citations whose enclosing line didn't happen to also say "UnitTestFw"
(`AD9833.c`, `DcDac.c`, `ILI9488.c`, `PCF8575.c`, `SSD1306.c`). A second,
smaller bug was fixed alongside it: `check_c_files_in_cmakelists.ps1`'s
`Join-Path $driversDir "CMakeLists.txt"` (a STAY file, correctly never
rewritten) was being tracked in `$unmapped` under a literal-PowerShell-call
string shape that the genuinely-unmapped filter's basename extraction
couldn't parse, so it spuriously counted as a hard-`-Apply`-failure
candidate; now tracked as `"Join-Path/<basename>"`, matching the same shape
the filter already handles for every other site.

### Preview verification (`-PreviewDir`)

`plan_moves.ps1 -DryRun -PreviewDir <scratch>` writes a rewritten copy of
every file this script would touch under `<scratch>/<repo-relative-path>`,
leaving the real tree untouched -- 194 files in the current run. Spot
checks against the preview copy:
- `firmware/KilnFW/App/drivers/CMakeLists.txt`: `INCLUDE_DIRS "." "bridge"
  "common" "control" "http" "hw" "net" "owners" "persist" "safety" "sim"
  "ui"` on one line, `PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_BINARY_DIR}"`
  immediately after on its own line (finding 6, fixed). SRCS entries read
  `"hw/SX1509.c"`, `"owners/kiln_io_owner.c"`, `"ui/lvgl_port.c"`, etc. --
  single layer prefix, no `"hw/hw/..."` doubling (finding 5's fix
  regression-checked). `KILNCTL_TUNING_REC_FALLBACK` reads
  `"${CMAKE_CURRENT_SOURCE_DIR}/http/tuning_recommendations_fallback.json"`
  (finding 5). The `KILNCTL_GZIP_ASSETS` block reads `"net/wifi_provision_
  page.html" "http/main_page.html" ... "http/zones_page.html"` (14 http +
  2 net, all 16 layer-qualified) and the loop body is
  `get_filename_component(kilnctl_gz_basename "${asset}" NAME)` /
  `set(kilnctl_gz_src "${CMAKE_CURRENT_SOURCE_DIR}/${asset}")` /
  `set(kilnctl_gz_out "${CMAKE_CURRENT_BINARY_DIR}/${kilnctl_gz_basename}.gz")`
  (finding 4).
- Ran the preview copy of `drivers/CMakeLists.txt` through `cmake -P`
  directly (this repo has cmake 4.4.2 on PATH). It parsed cleanly past both
  the `INCLUDE_DIRS` line and the entire `KILNCTL_GZIP_ASSETS` block --
  `Found Python3` printed, then it reached the `execute_process` copying
  `tuning_recommendations_fallback.json` and failed only on a *path*
  (`FileNotFoundError` -- the preview dir doesn't contain a copy of the
  actual asset files, only the rewritten scripts/CMakeLists.txt), not a
  syntax error. That failure mode is expected and is exactly the evidence
  requested: a real CMake syntax error (e.g. the pre-fix `"ui"PRIV_
  INCLUDE_DIRS`) would have failed at the *parse* stage, before
  `find_package(Python3)` ever ran.
- `firmware/KilnFW/App/test/build_host_tests.ps1`: `(Join-Path $driversDir
  "control/pid.c")`, `(Join-Path $driversDir "hw/max31856_codec.c")`, etc.
  (finding 1).
- `firmware/KilnFW/App/drivers/gpio_probe.h`: `#include "uart_protocol.h"`
  (finding 3, espInterfaces/ prefix stripped).
- `ROADMAP.md`/`CLAUDE.md`/firmware doc citations rewritten in place
  (finding 12) -- 25 markdown files touched, 111 total scanned.

## 0. Coordinator decisions applied this pass

## 0. Coordinator decisions applied this pass

- **A -- layout deviation from the original plan.** Target directories are
  subdirectories of the existing `drivers` ESP-IDF component --
  `firmware/KilnFW/App/drivers/<layer>/` -- NOT sibling `App/<layer>/`
  components as earlier drafts of the plan implied. One component remains;
  `CMakeLists.txt`, `Kconfig`, `README.md` and `gen_build_info.cmake` all
  **STAY** at `firmware/KilnFW/App/drivers/` (mapping.csv rows have
  `old_path == new_path`, rationale prefixed `STAY:`). Reason: this needs
  zero `REQUIRES`/component-boundary churn, `Kconfig` options already span
  net/ui/safety and have no natural single new home, and `README.md`
  describes the whole former tree rather than one layer. `INCLUDE_DIRS`
  becomes the eleven subdirs (`hw, owners, control, safety, persist, net,
  http, ui, bridge, sim, common`) instead of ten sibling directories -- see
  section 3.
- **B -- new bottom-most tier `common`**, below `hw`, for pure leaf
  headers/utilities: `uart_task_ids.h`, `stack_margin.{c,h}`,
  `stack_margin_calc.h`, `http_form.h`, `web_encoding.{c,h}`,
  `httpd_socket_budget.h`, `dram_margin.h`, `bx_worker_reentrancy.h`. Each
  was checked by hand before placement: every one includes only ESP-IDF/libc
  headers (or each other) and nothing from `control/safety/persist/net/
  owners/hw`, so **all eight passed the leaf check** -- none were left in
  place. `stack_margin.c` was included in the tier move alongside its header
  since it has the same include profile. `settings.h` was originally
  proposed here too, but **correction 2026-09-05**: it includes
  `driver/gpio.h`, `driver/spi_master.h` and `driver/uart.h` -- ESP-IDF
  peripheral headers, not leaf/libc -- so it failed the leaf check and moved
  to `hw` instead (mapping.csv).
- **C -- every `*_http.c/.h` / `*_http_*.c` file goes to `http`** regardless
  of subject matter: `ota_http*`, `wifi_provision_http*`, `safety_cfg_http*`,
  `adaptive_tune_http*`, plus `backup_export.c`/`backup_import.c` (paired
  with `backup_http.c`, so grouped with it rather than left in `persist`).
  16 files moved layer under this rule (see mapping.csv rows whose rationale
  starts "Decision C"). Exception: `zones_http_internal.h` -- read and
  confirmed it is the shared internals of the `zones_http.c` split, consumed
  by `zones_config_store.c`/`zones_config_accessors.c` (persist) as well as
  the `zones_current_sweep_*`/`zones_http_*` files -- placed in `persist`
  per the exception clause, since a `zones_config_*.c` file is a consumer.
- **D -- proposed, then REVERTED (see section 5 update)**: originally moved
  `sim` to the top tier in the verifier (with `ui/http/bridge`), reasoning
  the sim backend drives the system from above. This turned four existing
  `control -> sim_backend.h` includes into new upward-include findings.
  Reverted 2026-09-05: `sim` is a hardware substitute, the same tier as the
  real hw drivers it stands in for, not a top-tier orchestrator -- final
  tiering is `ui/http/bridge` (top) -> `control/safety/persist/net` (mid) ->
  `owners/hw/sim` (bottom) -> `common` (bottom-most). `plan_moves.ps1`'s
  `$tierOf` and mapping.csv both reflect this final, reverted state.
- **E -- all remaining `AMBIGUOUS:`-prefixed rows accepted** as the agent
  originally proposed: `heat_enable`/`heat_interlock` -> control,
  `kiln_io`/`kiln_io_owner`/`relay_authority` -> owners, `relay_cycles` ->
  persist, `thermo_combine` -> control,
  `tuning_recommendations_fallback.json` -> http, `wifi_status_ui` -> ui,
  `zone_settings_source_chain.h` -> persist. The `AMBIGUOUS:` prefix is
  stripped in mapping.csv; each row keeps a short "Coordinator-accepted
  placement: ..." rationale.

## 0a. Opus review pass (2026-09-05) applied to `plan_moves.ps1`/`mapping.csv`

- `-Apply` now creates all eleven layer directories up front and calls
  `git mv -- $old $new` directly (no `Invoke-Expression`), failing loud on
  any individual `git mv` error.
- `-Apply` now rewrites `INCLUDE_DIRS` in `drivers/CMakeLists.txt` to list
  all eleven layer subdirs (idempotent -- a second run is a no-op), and
  refuses (`exit 1`) if it cannot locate/verify the `INCLUDE_DIRS` clause.
- `-Apply` now mechanically rewrites the host-test mirror `#include
  "../drivers/X.c"` sites (~66 files, was undercounted at "~84/12" in the
  original review comment) and the path-keyed `check_*`/`selfcheck*`
  scripts from the same literal map used for the CMakeLists rewrite --
  section 4b below. Any `drivers/<name>` literal that cannot be mapped to
  either a moved file or a known STAY/placeholder/out-of-scope name is a
  hard `-Apply` failure, not a silent skip.
- File writes use UTF-8 without a BOM (`[System.IO.File]::WriteAllText`
  with a BOM-less `UTF8Encoding`) instead of `Set-Content -NoNewline`,
  which wrote the system ANSI codepage and dropped the trailing newline.
- `mapping.csv`: `settings.h` corrected from `common` to `hw` -- it
  includes `driver/gpio.h`, `driver/spi_master.h`, `driver/uart.h` (ESP-IDF
  peripheral headers), so it fails the leaf-header check decision B
  requires. `zone_settings_source_chain.h`'s rationale, which said it sat
  "alongside settings.h", is corrected to note settings.h moved out.
- `^\s*#include` in the verifier's regex became `^\s*#\s*include` (a
  `#include` with space before the directive name, while rare, would
  otherwise be missed).
- Section 0 (new): a mapping-completeness check runs before anything else
  and fails, naming the file(s), if any real file under
  `firmware/KilnFW/App/drivers/**` has no mapping.csv row. Negative-tested
  2026-09-05: temporarily deleted the `flash_worker.h` row, confirmed the
  script fails with exit 1 and names exactly that file, then restored the
  row (verified byte-identical via `diff` against a backup copy) before
  resuming. This check also caught two real, not hypothetical, files
  missing from `mapping.csv` mid-session -- `zones_config_query.h` (a new
  narrow header that landed on 2026-09-05, `9b38354`) and
  `wifi_provision_state.h` (from item 9, `76bb15a`, already present by the
  time this pass ran) -- both now have rows (persist and net tier
  respectively).
- Section 6 (new): a `-DryRun` counts summary (git mv, CMakeLists literal
  hits per file, INCLUDE_DIRS dirs to add, path-keyed rewrite lines/files,
  unmapped-literal counts split by hard-fail vs. benign, upward includes
  remaining) prints at the end of every dry run.

## 1. git mv plan

**Refreshed 2026-09-05 (this session):** 359 files map into the eleven layer
subdirs (`mapping.csv`, 363 rows minus the 4 STAY rows from decision A -- up
from 358/362 in the prior pass: item 9 added the `profiles_store.h` row).
No basename collisions across the eleven target dirs. Full list of `git mv`
commands: run the script, or read `mapping.csv` directly (one row per file).

358 files map into the eleven layer subdirs (`mapping.csv`, 362 rows minus
the 4 STAY rows from decision A -- up from 354/358 in the prior pass: the
0a review added `zones_config_query.h` (persist), confirmed
`wifi_provision_state.h`/`flash_worker.h` were already present, and the
mapping-completeness check in section 0 now guarantees this count always
matches every real file under `firmware/KilnFW/App/drivers/**`). No basename
collisions across the eleven target dirs. Full list of `git mv` commands:
run the script, or read `mapping.csv` directly (one row per file).

## 2. CMakeLists literal SRCS rewrite

- `firmware/KilnFW/App/drivers/CMakeLists.txt`: **201** literal quoted-filename
  SRCS occurrences found (unchanged from the prior pass -- decision A keeps
  this file in place, so the count doesn't shift).
- `firmware/KilnFW/App/CMakeLists.txt`: **0** literal `drivers/<file>` paths
  (`REQUIRES drivers` only).
- **Consequence for -Apply, now resolved by decision A**: since the single
  `drivers` component is retained, the fix is a straight in-place rewrite of
  each SRCS literal from `"foo.c"` to `"<layer>/foo.c"` inside the
  unchanged `drivers/CMakeLists.txt` -- no new per-layer `CMakeLists.txt`
  fragments, and no `App/CMakeLists.txt` restructuring. `plan_moves.ps1
  -Apply` already does this rewrite (section 2 of the script).

## 3. Bare-include feasibility

**Preferred: keep bare `#include "x.h"`.** List all **eleven** new
subdirectories in the `drivers` component's own `INCLUDE_DIRS`
(`idf_component_register` in `firmware/KilnFW/App/drivers/CMakeLists.txt`,
which stays put per decision A) -- the same mechanism that makes `drivers/`
itself a single include-path entry today, just with the entry list grown
from 1 to 11 and nested one level deeper. `mapping.csv`'s 358 target paths
have **zero basename collisions** across the eleven directories, so an
unqualified include can never resolve ambiguously post-move. No `#include`
line needs to become path-qualified.

## 4. Path-keyed check/test sites

Scanned `tools/**/*.ps1`, `tools/**/*.py`, and
`firmware/KilnFW/App/test/**/*.ps1|*.py` (excluding `.venv`/`node_modules`/
`site-packages`/`.git`) for the literal `drivers/` or any of the 358 mapped
filenames: **1423 matching lines across 341 files**. As before, the large
majority are documentation/docstring path citations in
`tools/PcTools/src/kilnctrl/*.py` -- stale after the move but **not a
runtime break**. Below are the two truly actionable subsets, refreshed for
the decision-A subdirectory layout (a rewrite now becomes
`.../App/drivers/<layer>/<file>`, not `.../App/<layer>/<file>`).

### 4a. Runtime code that resolves a `drivers/` filesystem path (WILL break)

```
firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py:151:  uart_ids = root / "firmware/KilnFW/App/drivers/uart_task_ids.h"
firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py:153:  max_payload_hdr = root / "firmware/KilnFW/App/drivers/espInterfaces/uart_protocol.h"
```
Rewrite to `firmware/KilnFW/App/drivers/common/uart_task_ids.h` (decision B
moved it to `common`) and `firmware/KilnFW/App/drivers/owners/uart_protocol.h`
(the `espInterfaces/` subfolder is flattened into `owners/` by the move, so
the `espInterfaces/` path segment disappears entirely, not just `drivers/`).

### 4b. check_*.ps1 / check_*.py / selfcheck*.py scripts hardcoding `App/drivers/...`

Same 18 scripts as the prior pass -- decisions A-E only change *where*
inside `drivers/` each literal now points, not which scripts need editing.
Each needs its literal path(s) rewritten to `App/drivers/<layer>/...` in the
same commit as the move, and re-run afterward to confirm it can still go
red:

```
firmware/KilnFW/App/test/check_flash_worker_lint.ps1                 -> App/drivers/bridge/... (uart_bridge_ext.c family)
firmware/KilnFW/App/test/check_thermal_guard_input_producers.ps1     -> App/drivers/control/thermal_guard.c
tools/PcTools/selfcheck.py                                            -> spans several new layer subdirs
tools/PcTools/selfcheck_zones_fields.py                               -> App/drivers/persist/zones_config_*.*
tools/check_bridge_reject_reason.ps1                                  -> App/drivers/bridge/uart_bridge*.c
tools/check_c_files_in_cmakelists.ps1                                 -> reads App/drivers/CMakeLists.txt (STAYS put, decision A) against files now under App/drivers/<layer>/
tools/check_doc_citations.ps1                                         -> spans several new layer subdirs
tools/check_duplicate_symbols.ps1                                     -> spans several new layer subdirs
tools/check_hal_include_boundary.ps1                                  -> App/drivers/http/{dashboard_http.c,ota_http.c,ota_http_esp.c,ota_http_pico.c,ota_http_recovery.c,partition_info_http.c}, App/drivers/ui/ui_page_diagnostics.c, App/drivers/net/wifi_prov*.c/.h (note: ota_http*.c moved net->http under decision C, wifi_prov*.c/.h stayed net -- only wifi_provision_http*.c/.h moved to http)
tools/check_heat_enable_wiring.ps1                                    -> App/drivers/control/heat_enable.c, App/drivers/safety/danger_mode.c, App/drivers/bridge/uart_bridge.c
tools/check_host_embed_symbols_defined.ps1                            -> parses "#include \"../drivers/X.c\"" convention -- becomes "../drivers/<layer>/X.c"
tools/check_no_duplicate_crc.ps1                                      -> also has a firmware/UnitTestFw/... drivers/ literal -- UnitTestFw is explicitly OUT of scope, do not touch that one
tools/check_relay_writes_through_owner.ps1                            -> RelPath = ".../drivers/owners/kiln_io.c", ".../drivers/control/profile_executor.c"
tools/check_safety_baud_sync.ps1                                      -> $kconfigPath = '.../App/drivers/Kconfig' (unchanged -- decision A keeps Kconfig at the component root)
tools/check_safety_call_results_checked.ps1                           -> RelPath = ".../drivers/control/profile_executor.c"
tools/check_stack_margin_baseline.py                                  -> App/drivers/common/stack_margin.* (decision B moved it out of safety/)
tools/check_uart_version_independence.ps1                             -> App/drivers/bridge/... / App/drivers/common/uart_task_ids.h
tools/check_uri_handler_cap.ps1                                       -> spans App/drivers/http/*
```

**Hand-fix, not covered by the script's scan (finding 6):**
`firmware/CommonFW/test/vectors/frame_vectors.json:2` cites
`App/drivers/espInterfaces/uart_protocol.c`. `plan_moves.ps1`'s path-keyed
site scan (section 4) only walks `*.ps1`/`*.py` (plus `*.c`/`*.h` under
`App/test`) and `*.md` -- it does not scan `*.json`, so this citation is not
rewritten automatically. Fix by hand in the same commit as the move:
`App/drivers/espInterfaces/uart_protocol.c` -> `App/drivers/owners/uart_protocol.c`
(the `espInterfaces/` segment is dropped, same as every other espInterfaces
site -- section 4c of this doc).

### 4c. Host-test "mirror" scripts under App/test that #include a drivers/*.c file directly

Same 12 scripts as the prior pass; each `#include "../drivers/X.c"` or path
literal becomes `"../drivers/<layer>/X.c"`:

```
firmware/KilnFW/App/test/approach_rate_cap_mirror_drift_check.py      -> App/drivers/control/... (profile_executor family)
firmware/KilnFW/App/test/attribute_str_pool.py                        -> check target layer at edit time
firmware/KilnFW/App/test/flash_worker_lint.py                         -> App/drivers/bridge/uart_bridge_ext*.c
firmware/KilnFW/App/test/frame_a_offset_drift_check.py                -> App/drivers/safety/safety_link_frame*.c
firmware/KilnFW/App/test/fuzzy_gain_mirror_drift_check.py             -> App/drivers/control/pid_fuzzy.c
firmware/KilnFW/App/test/heater_output_pwm_drift_check.py             -> App/drivers/control/heater_output.c
firmware/KilnFW/App/test/pid_fuzzy_drift_check.py                     -> App/drivers/control/pid_fuzzy.c
firmware/KilnFW/App/test/power_diag_flag_mirror_drift_check.py        -> check target layer at edit time
firmware/KilnFW/App/test/ramp_lock_decision_mirror_drift_check.py     -> App/drivers/control/profile_executor*.c
firmware/KilnFW/App/test/ramp_stepping_gate_mirror_drift_check.py     -> App/drivers/control/profile_executor*.c
firmware/KilnFW/App/test/source_path_drift_check.py                   -> spans several new layer subdirs
firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py           -> App/drivers/common/uart_task_ids.h, App/drivers/owners/uart_protocol.h
```

`firmware/KilnFW/App/test/build_host_tests.ps1` and
`firmware/KilnFW/App/test/stubs/*` still contain **no** hardcoded `drivers/`
literal.

## 5. Include-direction verifier

Tiers (decision A keeps the same grouping; decision D's `sim`-up proposal
below was reverted -- see the UPDATE that follows):
`ui/http/bridge` (top) -> `control/safety/persist/net` (mid) ->
`owners/hw/sim` (lower) -> `common` (bottom-most, decision B).

**UPDATE (2026-09-05, coordinator pass on Fix 4/5/6):** applied. Decision D
was reverted -- `sim` moved back to the bottom tier (`owners/hw/sim`) in
`plan_moves.ps1`'s `$tierOf`, since it is a hardware substitute, not a
top-tier orchestrator; this resolved the 4 Fix-4 `control -> sim_backend.h`
findings for free (`autotune_engine_internal.h`, `profile_executor.c`,
`profile_executor_relay_io.c`, `profile_executor_run.c` all sit at the same
tier as `sim` now). Fix 5's five owner-interlock pairs are now a named
allowlist in the verifier (`$allowedUpwardIncludes` in `plan_moves.ps1`,
keyed `"<includer basename>|<included basename>"`) rather than silently
tolerated -- any *other* upward include from those same files still reports.
Fix 6 shipped: a new `firmware/KilnFW/App/drivers/flash_worker.h` declares
only `uart_bridge_ext_run_on_flash_worker()` (with the re-entrancy hazard
documented on it directly); `log_store_mount.c` now includes it instead of
`uart_bridge.h`, and `uart_bridge.h` itself `#include`s it back so the
declaration has one source. Mapped to `common` in `mapping.csv` (it includes
only `esp_err.h`, nothing above `common`). Grepped the rest of `drivers/`
for other `uart_bridge.h` includes that exist solely for this call: none --
every other hit is either the bridge family itself (`uart_bridge*.c`,
`uart_bridge_ext*.c`, which need the full API) or a comment/prose match
(`safety_cfg_store.c`, `autotune_engine_internal.h` already hand-declare the
function and don't include the header at all -- the same false-positive
shape the anchored regex above already accounts for).

Sim reclassification surfaced two genuine (not false-positive) upward
includes directly on `sim_backend.c` itself, now tier 2 (`sim`) same as
`owners/hw`: `sim_backend.c:16 #include "wifi_provision_http.h"` [http] and
`sim_backend.c:17 #include "zones_config_accessors.h"` [persist]. Checked
what it actually calls from each -- one accessor apiece:
`wifi_provision_http_get_server()` (line 332) and
`zones_config_get_thermo_count()` (line 45). Both are narrow; per the
group-4 instruction this is a **propose, don't implement** finding:
- `wifi_provision_http.h`: already covered by Fix 3 below (a narrow
  `wifi_provision_state.h` exposing `wifi_provision_http_get_server()` would
  serve `sim_backend.c` too, alongside `factory_reset.c` and `wifi_prov.c`
  -- no separate header needed, just add `sim_backend.c` to that fix's
  consumer list once it lands). Confirmed via `git log -1 --format='%H %ci
  %s' -- firmware/KilnFW/App/drivers/wifi_provision_http.h` that no other
  session has narrowed it since the 2026-08-16 tree reorg -- it is still
  the full httpd-handler header, so this is real, not stale.
- `zones_config_accessors.h`: propose pulling
  `zones_config_get_thermo_count()` (and any other single-purpose read-only
  accessors `sim`/`hw`-tier files need) into a narrow
  `zones_config_query.h` (home: `persist`, alongside
  `zones_config_accessors.h`), with `zones_config_accessors.h` including it
  back for its own use -- same split pattern as Fix 1/2/3. **DONE 2026-09-05
  (`9b38354`, outside this task's scope):** `zones_config_query.h` now
  exists exactly as proposed; `mapping.csv` gained a row for it (persist)
  during the 0a review pass since it was a real file missing from the map
  (caught by the section-0 completeness check). It is still a `sim ->
  persist` upward include under decision D's reverted tiering, tracked in
  the updated count below.

**22 upward includes remain (now 5 after the above -- confirmed via a
`plan_moves.ps1 -DryRun` re-run 2026-09-05)** (down from 73 in the prior pass -- decisions B
and C's reclassifications resolved the other 51 without any code change,
since they were all narrow shared-header cases exactly as flagged before).
One entry from the prior pass, `safety_cfg_store.c [safety]:23 includes
"uart_bridge.h" [bridge]`, and one new candidate, `backlight_pwm.h [hw]
includes "screen_idle.h" [ui]`, turned out to be **false positives** in the
verifier itself: both are comments describing what the file *deliberately
does not* include (`safety_cfg_store.c` declares the one function it needs
by hand instead of pulling in all of `uart_bridge.h`; `backlight_pwm.h`
explicitly documents "Deliberately NOT #include ... here"). The verifier's
regex matched the string `#include "x.h"` inside prose. Fixed in
`plan_moves.ps1` by anchoring the match to `^\s*#include\s*"..."` (an actual
directive, optional leading whitespace only) instead of matching anywhere in
the line -- confirmed neither file has a real matching `#include` line.

**UPDATE (0a review, 2026-09-05 re-run):** the verifier regex was tightened
further to `^\s*#\s*include` (a `#include` with whitespace before the
directive name still counts). Current count is **4**: the 3 already listed
as Fix 1/3/4 below (one site each, `profiles_http.h`,
`wifi_provision_state.h` via `sim_backend.c`, `zones_config_query.h` via
`sim_backend.c`) plus one from Fix 2's family, `factory_reset.c [persist]:14
includes "ota_http.h" [http]`, confirmed still open by the same re-run.

Residuals, grouped by proposed fix:

**Fix 1 -- split `profiles_http.h` into a types header (same shape as plan
item 4).** 5 sites: `profiles_builtin.h [persist]:28`,
`profile_executor.h [control]:80`, `profile_executor_state.h [control]:23`,
`profile_feasibility.h [control]:28`, `run_state.h [control]:46`. All five
only need shared profile types/constants, not the httpd handler
declarations. Proposed fix: pull the type/constant declarations these five
actually use out of `profiles_http.h` into a new `profiles_types.h` (home:
`persist`, alongside `profiles_builtin.h`), and have `profiles_http.h`
`#include` it back for its own use -- same pattern as the plan's existing
item 4 split.

**Fix 2 -- split `ota_http.h`'s interlock/status query API out of the httpd
handler header.** 5 sites: `autotune_engine_internal.h [control]:74`
(`ota_http_heat_blocked_by_update()`), `factory_reset.c [persist]:14`
(`ota_http_authenticate_request()`/interlock checks),
`kiln_cfg_store.c [persist]:12` (`ota_http_check_interlocks()`),
`ota_pico_relay.c [net]:69` (progress/fail-reason accessors),
`zones_current_sweep_task.c [control]:15`. None of these five touch httpd
route registration -- they all want a handful of query functions
(`ota_http_check_interlocks`, `ota_http_authenticate_request`,
`ota_http_heat_blocked_by_update`, progress/fail-reason getters). Proposed
fix: move those declarations into the existing `ota_state.h` (already
`net`-tier, already a shared status header) or a new `ota_interlock.h`
companion, and have `ota_http.c` implement them there instead of in
`ota_http.h`.

**Fix 3 -- same pattern for `wifi_provision_http.h`.** 2 sites:
`factory_reset.c [persist]:16` (`wifi_provision_http_get_server()`),
`wifi_prov.c [net]:93`. Proposed fix: expose the one or two accessors these
callers need from a narrow `wifi_provision_state.h` (net-tier, alongside
`wifi_prov_internal.h`) instead of the full httpd-handler header.

**Fix 4 -- RESOLVED 2026-09-05 (decision D reverted, see update above).**
`sim_backend.h` consumers in `control` are a direct consequence
of decision D, not a pre-existing gap.** 4 sites:
`autotune_engine_internal.h [control]:80`, `profile_executor.c
[control]:34`, `profile_executor_relay_io.c [control]:23`,
`profile_executor_run.c [control]:24` -- all call
`sim_backend_enabled()`/`sim_backend_read_all()` to get simulated
thermocouple readings when sim mode is on, the same role a real hw driver
plays for `profile_executor`. Decision D puts `sim` at the top tier because
the sim *backend* drives the system from above (http/bridge-style
injection), but this specific direction -- control *reading* from sim as a
data source -- is the opposite relationship. Proposed fix: split a narrow
`sim_reader.h` (home: `hw`, next to the real thermocouple drivers it
substitutes for) exposing only `sim_backend_enabled()`/
`sim_backend_read_all()`, and keep the rest of `sim_backend.h` (the
orchestration/injection API used from the top tier) where decision D put
it. Flag for the coordinator: this may instead be judged an intentional
exception to keep in place as-is, since sim-as-hw-substitute is a
well-established pattern in this codebase (compare `kiln_io_owner.c`'s hw
abstraction) -- either the split or an explicit exception comment resolves
the finding.

**Fix 5 -- RESOLVED 2026-09-05 (verifier allowlist added, see update
above).** `kiln_io_owner`/`relay_authority`'s interlock includes are very
likely a deliberate exception, not a bug.** 4 sites:
`kiln_io_owner.c [owners]:16` (`danger_mode.h` [safety]),
`kiln_io_owner.c [owners]:17` (`heat_interlock.h` [control]),
`kiln_io_owner.c [owners]:18` (`ota_state.h` [net]),
`kiln_io_owner.h [owners]:116` (`safety_link.h` [safety]),
`relay_authority.h [owners]:23` (`safety_link.h` [safety]). These are the
owner modules that arbitrate direct relay/GPIO access reaching *up* to
consult the safety/control state that gates whether a write is allowed at
all -- CLAUDE.md's "Bypassed owner module bug class" note is explicit that
every relay write must route through these owners with the interlocks
consulted, so an owner checking `danger_mode`/`heat_interlock`/
`safety_link` state before acting is the safety property, not an
architecture violation. Proposed resolution: leave these five in place and
record the exception explicitly (a one-line comment at each `#include`, or
a documented carve-out in the tier table) rather than attempting a header
split that would only relocate the same coupling.

**Fix 6 -- RESOLVED 2026-09-05 (`flash_worker.h` shipped, see update
above).** `log_store_mount.c [persist]:9` includes `uart_bridge.h`
[bridge].** Real, single-purpose include:
`uart_bridge_ext_run_on_flash_worker()`, the flash-safe-executor dispatch
CLAUDE.md's "Flash worker re-entrancy"/"PSRAM stack + NVS = panic" notes
describe. Proposed fix: declare that one function by hand the same way
`safety_cfg_store.c` already does for a different `uart_bridge_ext.c`
function (see the false-positive note above for the precedent), or split it
into a narrow `flash_worker.h` at a shared tier (candidate: `common`, since
it is a single function pointer dispatch with no other bridge-layer
dependency) so both callers stop pulling in all of `uart_bridge.h`.

## 5 (continued). Residuals refreshed 2026-09-05 (this session, item 9)

Re-running `plan_moves.ps1 -DryRun` after moving `sim` to the MID tier
(`control/safety/persist/net/sim`, superseding the "decision D reverted"
bottom-tier placement above) drops the residual count from 4 to **1**:
every `sim_backend.c`/`*_internal.h [control]` site that used to read
`sim_backend.h [sim]` as an upward include is now same-tier (both MID), and
the `sim_backend.c -> zones_config_query.h [persist]` site from Fix 4's
"sim reclassification" discussion is likewise now same-tier. The
`factory_reset.c -> ota_http.h` (Fix 2) and `factory_reset.c ->
wifi_provision_http.h` (Fix 3) sites are also resolved: `factory_reset.c`
itself moved to `http` (item 9), so both are now same-tier (`http -> http`)
rather than `persist -> http`.

**1 upward include remains**: `sim_backend.c [sim]:16 includes
"wifi_provision_http.h" [http]` -- unaffected by the sim-tier change since
`http` (tier 0) is still strictly above `sim`'s new tier (1). This is the
same real, narrow dependency Fix 3 already proposed a resolution for
(`wifi_provision_http_get_server()`, one accessor); `wifi_provision_state.h`
already exists with exactly that accessor (item 9's narrow-header family).
Another agent is repointing `sim_backend.c` at `wifi_provision_state.h` in a
parallel pass, so this residual is expected to resolve by that code change
landing, not by this script's `$allowedUpwardIncludes` table -- do **not**
add a `sim_backend.c|wifi_provision_http.h` entry there; that would paper
over a real, fixable upward include instead of letting the in-flight fix
close it. Confirmed via `plan_moves.ps1 -DryRun`'s section 5 output and
negative-test 8 above (which temporarily added a second, injected violation
on top of this real one, then reverted it).
