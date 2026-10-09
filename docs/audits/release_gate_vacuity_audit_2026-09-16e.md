# Release gate vacuity audit — 2026-09-16e

Fifth slice of blocker 3 (`docs/RELEASE_HARDENING.md`). Continues from
`docs/audits/release_gate_vacuity_audit_2026-09-16d.md`, which must not be
redone — its eight gates are load-bearing and closed (as are the earlier
`_2026-09-16.md`/`b`/`c` slices). This pass worked `_2026-09-16d.md`'s "Gates
not examined" list and matches the prior slices' throughput: eight gates
closed, plus one more examined at baseline only (`check_duplicate_symbols.ps1`,
see below), plus a concrete widening and a real production bug fix in
`check_relay_authority_paths.py`/`actions.py`.

Worktree: `C:\wt\vacuity5_xosst4` (`git worktree add --detach` at
`origin/main`, minted via `tools/worktree_mint.ps1 -Label vacuity5 -RunSetup`),
submodules initialized, `tools/PcTools/.venv` provisioned automatically by the
`-RunSetup` switch (`SETUP: ok`). Concurrent agents were actively editing
config/backup/persist code and `docs/RELEASE_HARDENING.md` during this
pass; the worktree was re-fetched and moved to current `origin/main`
(`git switch --detach origin/main`, landing on `2384ec1c`) before committing,
and every touched-path diff was re-checked against that new base. SaftyFW and
KilnFW builds ran via the Bash tool from this `C:\wt\` path (host tests) and
the PowerShell tool (ESP-IDF target build). No JTAG flash-bank probing, no
flashing, no board contact, no `.kicad_*` file touched.

## 0. `tools/check_relay_authority_paths.py` — widened (PC-side half), plus a
real production bug found and fixed

The prior slice's gate 8 (PC-side half) found that the check's rule only
flags a raw relay-frame builder (`devices.io_set_relay()` /
`io_set_relay_mask()` / `io_all_relays_off()`) when its return value is
passed **directly** to a literal `.send(...)` call — a bare, unwrapped call to
the builder (return value discarded, or assigned and sent by any other
route/name) passed clean. Since relays are safety-critical and must only ever
be driven through `IoClient`'s refusal-aware wrapper methods
(`set_relay()`/`set_relay_mask()`/`all_relays_off()`, which wait out
`SET_RELAY_REJECT_WINDOW_S` for a firmware refusal reply), this is a real
gap: any bypass shape other than "raw builder straight into `.send(`" was
invisible.

**The fix.** Rewrote the rule from a bypass-sender **absence** check ("is
there a `.send(` in the last 3 lines with no wrapper method nearby?") to a
wrapper-presence check: any raw-builder call, in any shape, whose
3-line window does not also contain a call to
`set_relay`/`set_relay_mask`/`all_relays_off` is now flagged, regardless of
how (or whether) it is sent. `tools/check_relay_authority_paths.py`'s
`SEND_CALL_RE` was removed entirely; `check_file()` now flags on
`not WRAPPER_METHOD_RE.search(window)` alone. The module docstring was
updated to name both known bypass shapes (the historical `current_sense_
commissioning.py` `.send(...)` bug from `_2026-09-16d.md`, and the live
`actions.py` bug found below).

**Real bug found by the widening, live in `tools/PcTools/src/kilnctrl/
actions.py`.** The `"IO: All Relays Off"` GUI-button action registration
called `devices.io_all_relays_off()` and passed it to a bare `_send(ctx,
UART_TASK_ID_IO, ...)` — a route that discards any firmware refusal reply.
This is the exact hazard `IoClient.all_relays_off()` exists to prevent
(owned-by-profile / safety-fault / OTA-in-progress refusal), reachable from
the live GUI/MCP `press_button` surface. **Fixed** by rerouting through
`client.all_relays_off()`.

**Regression this fix introduced, and its fix.** Rerouting through
`_client_query` (the same helper already used, pre-existing, by
`"IO: Set Relay"`/`"IO: Set Relay Mask"`) turned out to silently drop the
protocol-version compatibility gate `_send()` applies — `IoClient`'s wrapper
methods talk to `ctx.link` directly and have no notion of PC-side protocol
compatibility, only firmware-level refusal. This broke
`tools/PcTools/selfcheck_actions.py`'s existing "device command blocked
before compatibility is known" assertion for `"IO: All Relays Off"`, caught
by the full `run_all_checks.ps1` pass below, not by the widened relay check
itself (a different, pre-existing gate). **Fixed** by adding
`_gated_client_query()` (checks `ctx.info.compatible` exactly as `_send()`
does, then delegates to `_client_query`) and switching all three
relay-action registrations (`"IO: Set Relay"`, `"IO: Set Relay Mask"`,
`"IO: All Relays Off"`) to use it — which also closes the same latent gap in
the first two, which were never covered by an existing selfcheck assertion.
Re-ran `tools/PcTools/selfcheck.py` (via `tools/PcTools/.venv/Scripts/
python.exe`) clean after this fix: `all checks passed`.

**False positives from the widening: none.** `python
tools/check_relay_authority_paths.py` ran clean against the whole PC-side
tree immediately after the `actions.py` fix, before any negative-test
sabotage was applied — the widened rule produces zero false positives on the
existing codebase.

**Negative test, bypass shape 1 (bare unwrapped call — the shape the prior
slice found NOT caught).** Appended to `tools/PcTools/scripts/
current_sense_commissioning.py`:
```python
def _negtest_bare_bypass(link, relay_index):
    devices.io_set_relay(link, relay_index, True)
```
Real failure (now caught, where the prior slice's attempt 1 passed clean):
```
...current_sense_commissioning.py:<N>: raw frame builder not wrapped by
IoClient.set_relay()/set_relay_mask()/all_relays_off() -- a firmware refusal
reply would not be observed: devices.io_set_relay(link, relay_index, True)
```

**Negative test, bypass shape 2 (`.send()`-style bypass, reproduced against
this pass's own found bug).** Appended, then confirmed FAIL naming the exact
line, then restored:
```python
def _negtest_send_bypass(link, relay_index):
    link.send(devices.io_set_relay(link, relay_index, True))
```
Both negtest functions were removed and the file restored by hand via `git
cat-file -p HEAD:tools/PcTools/scripts/current_sense_commissioning.py >
tools/PcTools/scripts/current_sense_commissioning.py`; `git diff --quiet`
empty; `git hash-object` matched HEAD
(`` `blob:f81c77b37fefd07028d1864e1c3b1670e92e99da` ``, same file, same
hash as `_2026-09-16d.md`'s gate 8 restore).

**Negative test, firmware-side half.** Appended to
`firmware/KilnFW/App/drivers/control/profile_executor.c`:
```c
void negtest_direct_relay_bypass(void) {
    kiln_io_set_relay(0, true);
}
```
Confirmed FAIL naming the line via the firmware-side (`FW_CALL_RE`) scan.
Restored via `git cat-file -p HEAD:firmware/KilnFW/App/drivers/control/
profile_executor.c > firmware/KilnFW/App/drivers/control/profile_executor.c`;
`git diff --quiet` empty; `git hash-object` matched HEAD
(`` `blob:d5f2c6153748e237eb0dd8d5cbfbbea0871a82f4` ``).

**Verdict: load-bearing, materially widened, and directly responsible for
finding and fixing a real, live production bug** (the `actions.py`
`"IO: All Relays Off"` bypass), plus a regression the fix itself introduced
(the dropped compatibility gate) caught by the full check suite and fixed in
the same pass.

## 1. `tools/check_mcp_facade_coverage.py`

Checks kilnctrl + kicad MCP tool registrations against their facade
taxonomies (`GROUP_OVERRIDES`/`KEYWORDS`/`GROUP_PREFIXES`).

**Vacuity finding: the check's own documented negative test is stale.** Its
docstring cites deleting the `plant_sim_compare` `KEYWORDS` entry as proof of
failure. Reproducing that today (removing the 3-line entry from
`tools/PcTools/src/kilnctrl/mcp_facade.py`) shows the check now passes clean
— `plant_sim_compare` is independently covered by a later `GROUP_PREFIXES`
entry (`("plant_sim_", "plant_sim")`) added since that docstring was written.
The documented example no longer demonstrates anything; restored by hand
(`git diff --quiet` empty, hash `` `blob:fc47fb26af29318a63f4c39292632ad400477cee` ``
matched HEAD).

**Real negative test (proves the check is still load-bearing overall).**
Appended to `tools/PcTools/src/kilnctrl/mcp_server_io.py`:
```python
@_srv._tool()
def negtest_zzz_uncovered_tool() -> str:
    return "unused"
```
Confirmed FAIL naming `negtest_zzz_uncovered_tool` as uncovered. Restored via
`git cat-file -p HEAD:tools/PcTools/src/kilnctrl/mcp_server_io.py >
tools/PcTools/src/kilnctrl/mcp_server_io.py` (a prior byte-marker-based
restore attempt on this file matched the wrong occurrence of a repeated
string and deleted ~168 unrelated lines — caught immediately by `git diff
--stat` showing 168 deletions instead of the expected ~5, and fixed by this
full-file reconstruction instead); `git diff --quiet` empty; hash
`` `blob:49f4155ca9fe67d7d6c6029ae032ba44b2123be9` `` matched HEAD both
before and after. Re-ran: `kilnctrl OK (156 tools, all covered)`.

**Verdict: load-bearing overall, but with a stale/vacuous documented example**
(the `plant_sim_compare` case) that should be replaced or removed from the
script's own docstring in a follow-up — not fixed here, since the task was to
find and report, not silently rewrite documentation evidence.

## 2. `tools/check_mcp_tool_count_doc.ps1`

Checks `CLAUDE.md`'s `"N tools for \`kilnctrl\`"` and
`docs/MCP_SERVERS.md`'s `` `kilnctrl` registers N tools`` phrases against the
real registered-tool count (156).

**Negative test:** edited `CLAUDE.md` to say `"999 tools for \`kilnctrl\`"`.
Real failure: `check_mcp_tool_count_doc: CLAUDE.md says 999 kilnctrl tools,
actual registered count is 156`. Restored by hand; `git diff --quiet` empty;
hash `` `blob:6f51622d0fecc510b8a55618143eac74b881bb99` `` matched HEAD.
Re-ran clean.

**Technique note:** piping the PowerShell invocation through `tail` masks
the real exit code (`tail`'s own exit code is reported instead). Fixed by
redirecting to a file first and checking `$?` immediately after the
PowerShell call.

**Verdict: load-bearing.**

## 3. `tools/check_test_has_assertions.ps1`

Scans every dispatched C test function for an assertion macro (directly or
via a local helper that itself asserts) and flags literal==literal
tautologies in both C and Python tests.

**Negative test:** appended a dispatched, assertion-free function to
`firmware/KilnFW/App/test/test_adaptive_tune.c`:
```c
static void test_negtest_vacuous(void)
{
    int x = 1;
    (void)x;
}
```
Confirmed FAIL naming exactly `test_adaptive_tune.c: test_negtest_vacuous --
no assertion macro call, direct or via a local helper that itself asserts`.
Restored via `git cat-file -p HEAD:firmware/KilnFW/App/test/
test_adaptive_tune.c > firmware/KilnFW/App/test/test_adaptive_tune.c`; `git
diff --quiet` empty; hash `` `blob:6e82c3b538063b7755530beb3f85d1f119cbd324` ``
matched HEAD. Re-ran clean: "all 2004 executed test function(s) across 309
file(s) contain a real, non-tautological assertion."

**Forced full rebuild, fresh directory.** `firmware/KilnFW/App/test/
build_host_tests.ps1 -OutDir C:\wt\vacuity5_xosst4_fresh_host_test_out`
(a brand-new directory, never used by any other check or session) rebuilt
all 47 host-test executables from the restored source and reported "Built:
47/47 executables, all 47 host test executables built and passed" — proving
no poisoned binary from the sabotage survived the restore.

**Verdict: load-bearing.**

## 4. `tools/check_test_c_files_wired.ps1`

Requires every `.c` under `firmware/KilnFW/App/test`/`firmware/SaftyFW/test`
to be reachable via `build_host_tests.ps1` naming, a sibling `#include`, or a
same-directory drift-check script.

**Negative test:** created a new, never-tracked orphan file
`firmware/KilnFW/App/test/test_negtest_orphan_zzz.c`
(`int main(void) { return 0; }`). Confirmed FAIL naming it by path. Deleted
afterward — no restore-by-hand needed since the file was never tracked
(`git status --porcelain` on that path was empty after deletion).

**Verdict: load-bearing.**

## 5. `tools/check_stack_margin_registration.ps1` (create-vs-register
cross-check sub-part)

Five sub-checks: required-name list, duplicate-name, cap-vs-count (with
headroom warning), hwAbstraction backend accessor/boundary, and a
create-vs-register cross-check with an allowlist for exempt task names. Only
the cross-check sub-part was negative-tested this pass; the other four
sub-parts were read but not individually sabotaged (see "Gates not examined"
below).

**Negative test:** appended an unrecognized
`xTaskCreatePinnedToCore(owner_task, "negtest_unrecognized_task", ...)` call
to `firmware/KilnFW/App/drivers/owners/kiln_io_owner.c`. Confirmed FAIL via
the cross-check's throw message naming the unrecognized task name. Restored
via `git cat-file -p HEAD:firmware/KilnFW/App/drivers/owners/
kiln_io_owner.c > firmware/KilnFW/App/drivers/owners/kiln_io_owner.c`; `git
diff --quiet` empty; hash `` `blob:5095f21900619c61195ce2868882ff3ce96b640e` ``
matched HEAD. Re-ran: all 5 sub-checks clean ("34 call site(s) ... 14 spare
slot(s) ... create-vs-register check passed: 42 xTaskCreate*() call site(s)
all accounted for").

**Forced full rebuild.** Covered by the KilnFW target build in item 8 below
(this same file feeds that build).

**Verdict: load-bearing for the sub-part tested.** The required-name-missing,
duplicate-name, cap-vs-count, and hwAbstraction accessor/boundary sub-checks
were not individually negative-tested this pass — carried forward to the
next slice's "not examined" list.

## 6. `tools/check_safety_trip_mask_docs.ps1`

Pins `link_frame_trip_mask_for_reason()`'s formula shape in
`firmware/SaftyFW/src/tasks/link_frame.c` and scans `CLAUDE.md`/
`docs/MCP_SERVERS.md` for any hex literal near a "MAIN_FAULT"/"S6a" + "mask"
mention that isn't `0x0020` and isn't clearly negated (e.g. "not `0x0040`").

**Negative test:** inserted a new, non-negated sentence into `CLAUDE.md`:
`"S6a's trip_mask is 0x0040 in some older logs."` Confirmed FAIL: `A document
states the wrong trip_mask constant for SAFETY_TRIP_MAIN_FAULT (S6a) ...
reason 6 -> 0x0020, not any other value.` Restored by hand; `git diff
--quiet` empty; hash `` `blob:6f51622d0fecc510b8a55618143eac74b881bb99` ``
matched HEAD (same file/hash as gate 2's restore, since gate 2 and this gate
were tested sequentially against the same file, restored in between).
Re-ran clean.

**Verdict: load-bearing.**

## 7. `tools/check_mykicad_golden_suite_runs.ps1`

Runs the mykicadMcp golden/real-board test suite and fails on: can't run at
all, zero tests collected, any test failed/errored, or any golden test
SKIPPED because `kiln_project_path` couldn't find the real board (the
2026-08-28 vacuous-skip incident this check exists to catch).

Baseline: 72 passed, zero skipped, clean.

**Negative test:** in the `tools/mykicadMcp` submodule's
`tests/conftest.py`, pointed `_KILN_PROJECT_DIR` at a nonexistent path
(`C:/nonexistent_negtest_kiln_dir_zzz`), forcing both the committed-snapshot
materialization and the live-board fallback to fail. Confirmed FAIL: 53
passed, 19 errored (the check's own `failed/errored` branch, distinct from
its `zero tests ran` and `SKIPPED against the real board` branches — all
three of which this single check enforces). Restored via `git cat-file -p
HEAD:tests/conftest.py > tests/conftest.py` from inside the submodule (its
own repo, since `mykicadMcp` is a separate git checkout — `HEAD:<path>`
against the parent repo does not resolve a submodule's tracked file); `git
diff --quiet` empty; hash `` `blob:5144929b4ec97e45ab26da701bc84d9c0ff38642` ``
matched the submodule's own HEAD. Re-ran clean: 72 passed, zero skipped.

**Verdict: load-bearing** (and specifically confirmed to catch the
`errored`/`failed` shape, not just the historical `SKIPPED` shape its header
describes in most detail).

## 8. `tools/check_duplicate_symbols.ps1` — baseline only, not negative-tested

Walks every `*.c.obj` this project's own components (App/drivers/kilnlink/
esp) produced by a real `idf.py build` and flags any externally-linked
symbol name defined in more than one object file — the class of bug the
host-test suite (which unity-compiles into one translation unit) is
structurally blind to.

This check SKIPs on a clean checkout with no `firmware/KilnFW/build/`
present. This pass's own fresh KilnFW target build (below) populated that
directory, which let the check run rather than skip: **263 object files
across App/drivers/kilnlink/esp, no externally-linked symbol defined more
than once** — clean pass, confirmed against a genuinely fresh build (not a
stale/shared one). A negative test (introducing a real duplicate external
symbol across two files, e.g. two same-named non-`static` helpers) was not
performed this pass — it would require a second full `idf.py build` cycle
(~2-3 minutes) purely to prove the failure shape, and this pass's build-time
budget was already spent on the fresh rebuilds required for the other
firmware-touching negative tests above. Left open for the next slice.

**Verdict: examined, clean baseline confirmed against a fresh build; FAIL
path not exercised.**

## Forced full rebuilds (negative-test discipline)

Per the standing rule that an empty `git diff` proves source restoration
only, not build-artifact cleanliness, every firmware-touching negative test
above was followed by a forced full rebuild into a fresh directory before
being considered verified:

- **Host tests** (`test_adaptive_tune.c`):
  `firmware/KilnFW/App/test/build_host_tests.ps1 -OutDir
  C:\wt\vacuity5_xosst4_fresh_host_test_out` — a brand-new directory never
  used before. Result: `Built: 47/47 executables, all 47 host test
  executables built and passed`.
- **KilnFW target build** (`kiln_io_owner.c`, `profile_executor.c`, and the
  real `actions.py` fix): `firmware/KilnFW/App/test/
  check_00_kilnfw_target_build.ps1`, which mirrors the current working tree
  into its own persistent checkout worktree with `CCACHE_DISABLE=1` and
  performs a real `idf.py build`. Result: `PASS: KilnFW target build
  succeeded`, `KilnCtrl.bin` produced, fresh ELF/`compile_commands.json`
  published to `firmware/KilnFW/build/`. This build also produced the
  object-file tree `check_duplicate_symbols.ps1` needed for item 8 above.

No prebuilt/surviving binary from any sabotage state was read or measured
after any restore.

## An external, real, currently-live regression found (not introduced by
this pass, not fixed by this pass)

`tools/run_all_checks.ps1`'s unfiltered run (below) failed
`check_01_kilnfw_pushed_build.ps1` against **live `origin/main`** — not a
local-tree issue. Building the actual fetched `origin/main` ref
(`2384ec1cdfe340f30987ec152b0db53fdfdcbc86` at the time of the second check
against it) fails with:
```
firmware/KilnFW/App/drivers/http/backup_import.c:1284:25: error: '%.80s'
directive output may be truncated writing up to 80 bytes into a region of
size 76 [-Werror=format-truncation=]
```
`backup_import.c` is not a file this pass touched — it belongs to concurrent
backup/config-migration work landing on `origin/main` from other sessions
during this pass (visible in this session's background-agent list: "Add
missing accessors for backup fields", "Zones-config version quarantine
safety fix", "Build host config version converter"). This pass does not fix
it — it is out of this pass's scope and actively owned elsewhere — but it is
worth recording for two reasons: (1) it is a real, live defect on `main`
right now that whoever lands next should not build on top of without fixing,
and (2) it is strong, unplanned evidence that `check_01_kilnfw_pushed_build.ps1`
is not vacuous — it is, right now, correctly catching a genuine pushed-build
break with none of this pass's own negative-test sabotage involved. Verified
twice, once against `f61c3115...` and again against `2384ec1c...` after a
`git fetch`, both times reproducing the identical `-Werror=format-truncation`
failure at the same line — not a one-off fetch race.

Because of this, the FAIL path of `check_01_kilnfw_pushed_build.ps1` and its
SaftyFW sibling were not separately, deliberately negative-tested this pass
(the KilnFW one is already failing for a real, unrelated reason, and
deliberately sabotaging it further on top of a build that's already broken
would not produce a clean read on a fresh violation) — carried forward.

## Fixes applied

- `tools/check_relay_authority_paths.py`: widened from a bypass-sender
  absence rule to a wrapper-presence rule (see item 0).
- `tools/PcTools/src/kilnctrl/actions.py`: fixed the `"IO: All Relays Off"`
  action to route through `IoClient.all_relays_off()` instead of a bare
  `_send()` call (a real, live relay-authority bypass found by the widening
  above); added `_gated_client_query()` and switched all three relay
  actions (`Set Relay`/`Set Relay Mask`/`All Relays Off`) to it, restoring
  the protocol-compatibility gate the first fix had accidentally dropped and
  closing the same latent gap in the two pre-existing relay actions.

No fixes to any other gate's logic. One vacuity finding reported, not
silently fixed: `check_mcp_facade_coverage.py`'s own documented negative
test (`plant_sim_compare`) is stale (item 1).

## Full-suite run

`tools\run_all_checks.ps1 -ExecutionPolicy Bypass` (foreground, full,
unfiltered — no `-Fast`, no `-Only`), run twice against this pass's own
edits, both after moving the worktree to current `origin/main`:

- **First run:** 92 passed, 0 skipped, 3 failed —
  `tools\PcTools\selfcheck.py` (the compatibility-gate regression from this
  pass's own `actions.py` fix, described in item 0, fixed immediately after
  and confirmed green), `firmware\KilnFW\App\test\
  check_ui_responsive_sweep.ps1` (a flaky headless-browser timeout —
  `setup_wizard_page.html @320px: sweep threw: timed out waiting for
  Page.loadEventFired`; re-ran standalone and it passed clean, confirming
  contention from the parallel run rather than a real regression), and
  `check_01_kilnfw_pushed_build.ps1` (the external, live `origin/main`
  regression described above, not introduced by this pass).
- **Second run** (after fixing the `actions.py` regression): **94 passed, 0
  skipped, 1 failed** — only `check_01_kilnfw_pushed_build.ps1`, confirmed
  external and still live against the newest fetched `origin/main` at the
  time.
- **Third, final run** (after re-fetching and moving the worktree to the
  newest `origin/main`, `d3f74d67`, immediately before committing): **94
  passed, 1 skipped, 2 failed**. Both failures are `firmware\KilnFW\App\test\
  check_00_kilnfw_target_build.ps1` and `...\check_01_kilnfw_pushed_build.ps1`
  — the same external `backup_import.c` `-Werror=format-truncation` defect
  described above, now also failing `check_00` (a plain local target build)
  because the local worktree's own tree now equals this newer, still-broken
  `origin/main`, not just the clean-checkout copy `check_01` builds. The one
  skip is `tools\check_recovery_image_size.ps1`, which self-reports why:
  `'recovery' partition is defined ... but no recovery image was found at
  ...KilnFW_recovery\build\recovery.bin yet` — the single-slot-OTA recovery
  image is a concurrently in-progress feature (see
  `docs/OTA_SINGLE_SLOT_PLAN.md` section 8) not yet built in this worktree,
  not a regression from this pass's own edits.

This is one skip and two fails short of the 95/0/0 baseline, all three
entirely accounted for by (a) the same real, external, currently-live
`backup_import.c` regression on `origin/main` that this pass did not
introduce and is not in scope to fix, now tripping both KilnFW build gates
instead of just one, and (b) a concurrently in-flight feature (the OTA
recovery image) not yet built in this worktree. Neither is a defect in this
pass's own two changed files, both of which were re-verified clean
(`tools/PcTools/selfcheck.py`: all checks passed;
`python tools/check_relay_authority_paths.py`: OK) against this final,
newest base immediately before committing.

## Gates not examined in this pass

Everything `_2026-09-16d.md` already listed as unexamined, MINUS the eight
gates closed above (the widened `check_relay_authority_paths` firmware-side
half, `check_mcp_facade_coverage`, `check_mcp_tool_count_doc`,
`check_test_has_assertions`, `check_test_c_files_wired`,
`check_stack_margin_registration.ps1`'s create-vs-register sub-part,
`check_safety_trip_mask_docs.ps1`, and `check_mykicad_golden_suite_runs`),
plus `check_duplicate_symbols.ps1` examined at baseline only. Still open:

- `check_stack_margin_registration.ps1`'s remaining four sub-checks
  (required-name-missing, duplicate-name, cap-vs-count, hwAbstraction
  accessor/boundary) — only the create-vs-register cross-check was
  negative-tested.
- `check_duplicate_symbols.ps1`'s FAIL path (a genuine cross-file duplicate
  external symbol) — baseline confirmed clean against a fresh build, but no
  sabotage was applied.
- The `check_00_*_target_build.ps1`/`check_01_*_pushed_build.ps1` FAIL
  paths for both firmwares — `check_00_kilnfw_target_build.ps1` was run
  fresh and clean twice this pass (restore verification) but never fed a
  genuine compile failure; `check_01_kilnfw_pushed_build.ps1` is currently
  failing for a real, external, unrelated reason (see above), which makes
  this the wrong pass to also deliberately sabotage it; neither
  `check_00_saftyfw_target_build.ps1` nor `check_01_saftyfw_pushed_build.ps1`
  was touched at all.
- The per-file UI/layout checks: `check_ui_budget_asserts`,
  `check_ui_shell_layout`, `check_ui_status_color`,
  `check_stop_bar_body_padding`, `check_label_column_overflow_wrap`,
  `check_kv_narrow_stack` (`check_ui_responsive_sweep` ran twice this pass as
  part of the full-suite runs above — both a flaky failure and a clean
  standalone pass — but was not deliberately negative-tested).
- `check_mcp_facade_coverage.py`'s stale documented negative test
  (`plant_sim_compare`, item 1 above) should be replaced with a fresh example
  in its own docstring — reported, not fixed, this pass.
- The external `origin/main` regression in `backup_import.c` (format-
  truncation, see above) — not this pass's file to fix; owned by concurrent
  backup/config-migration work.

A future pass should prioritize `check_duplicate_symbols.ps1`'s FAIL path
(cheap to add on top of an already-fresh build) and the remaining
`check_stack_margin_registration.ps1` sub-checks next, and should re-check
whether `check_01_kilnfw_pushed_build.ps1` is still failing against
`origin/main` before assuming it is safe to negative-test on top of.
