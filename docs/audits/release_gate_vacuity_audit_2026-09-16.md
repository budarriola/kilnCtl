# Release gate vacuity audit — 2026-09-16

Scope: blocker 3 of `docs/RELEASE_HARDENING.md` ("test coverage that
survives this repository's own failure modes"). This is not a search for
gates that fail — it is a search for gates that pass while proving nothing,
per the six confirmed instances listed in that plan section (vacuous pass,
sibling-output read, PASS-shaped-but-exit-0 harness, poisoned-binary
measurement, `skipTest` guard against a hardcoded path, mirror-drift against
a test-local copy).

## Coverage claim, stated honestly

There are roughly 94 `check_*`/`test_*` gates that `tools/run_all_checks.ps1`
drives (a `Get-ChildItem -Filter "check_*.ps1" -Recurse` glob over the whole
repo, plus a handful of explicitly-wired `test_*`/`.py` entries). This audit
negative-tested **7** of them end to end (sabotage → confirmed RED at a named
line → hand-restore → hash/diff match → confirmed GREEN again), and read two
more in enough depth to state a verdict without a live sabotage run. That
leaves roughly **85 gates not examined at all** in this pass — named
explicitly below so this audit does not itself commit the sin it was sent to
find (silently covering a fraction while implying full coverage).

Priority was given, per the plan's own instruction, to gates guarding
safety-relevant call discipline, relay-write ownership, NVS/config-schema
literals, mirror-drift between production and a host-test double, stack
budgets, and the meta-guard that stops a new check from shipping unwired.

## Gates negative-tested (load-bearing, with evidence)

### 1. `tools/check_safety_call_results_checked.ps1`
**Guards:** every call to a named safety-relevant function (relay-off,
relay-mask, heat-enable request, `nvs_save_store`, `zone_normals_save`,
`estop_verification_clear`) outside its own definition must capture its
`esp_err_t`/result into a variable — the 2026-08-31 `danger_mode.c` seed bug
shape.

**Negative test:** in `firmware/KilnFW/App/drivers/bridge/uart_bridge_io.c`,
changed `err = kiln_io_owner_command_all_relays_off();` to a bare
`kiln_io_owner_command_all_relays_off();`. Result:
```
firmware/KilnFW/App/drivers/bridge/uart_bridge_io.c:372: call to
kiln_io_owner_command_all_relays_off() does not capture its return value...
```
exit 1, named line matches the sabotaged one exactly. Restored by hand;
`git diff` empty; re-run passed (`44 call site(s) ... all captured`).

**Verdict: load-bearing.** It also self-guards against going blind: it
throws if fewer than 50 `.c` files are found (tree moved) or fewer than 5
call sites are seen across the whole tree (patterns renamed/moved) — both
would themselves catch a "sibling reads nothing" collapse.

### 2. `tools/check_relay_writes_through_owner.ps1`
**Guards:** `kiln_io_set_relay_mask()`/`kiln_io_all_relays_off()` may only be
called from `kiln_io_owner.c` (or a reasoned, per-function, per-file
allowlist) — the lost-update race on the SX1509 read-modify-write that a
2026-08-19 research pass found FIVE independent direct callers of, and a
sixth caught by review hours before a live-heating-elements bench flash.

**Negative test:** appended an unauthorized `kiln_io_set_relay_mask(io, 0);`
call directly after `main.c`'s allowlisted `kiln_io_all_relays_off(io)` call
in `kiln_enter_safe_state()`. Result:
```
firmware/KilnFW/App/main.c:165: direct call to kiln_io_set_relay_mask()
outside kiln_io_owner.c and not allowlisted
```
exit 1, at the sabotaged line. Restored by hand; `git diff` empty; re-run
passed (`3 call(s)` in the owner file, `4` allowlist entries — one call for
each function stayed distinct, confirming the allowlist is keyed per
function per file, not per file).

**Verdict: load-bearing.** Also self-guards: throws if `kiln_io_owner.c`
cannot be found exactly once, or if zero calls to either function are found
inside it (pattern gone blind).

### 3. `firmware/KilnFW/App/test/approach_rate_cap_mirror_drift_check.py`
**Guards:** the exact mirror-drift failure mode named in the plan —
`test_approach_rate_cap.c`'s hand-written `cap_update_tick()` is a mirror of
`profile_executor.c`'s real per-zone approach-rate-cap loop (which cannot be
driven tick-by-tick from a host test because it lives inside a real FreeRTOS
task body). This is a genuine production-vs-mirror diff, not a
mirror-vs-mirror comparison — the trap the plan calls out explicitly.

**Negative test:** changed production's `max_step_c = cap_c_per_hr * (dt_s /
3600.0f);` to `/ 1800.0f` (a real arithmetic drift, doubling the effective
cap). Result: FAILED, printing both normalized fragments side by side with
the one differing line (`3600.0f` vs `1800.0f`) visible in each. Restored by
hand; `git diff` on the file was empty (this repo's autocrlf setting makes
`git hash-object` mismatch a clean restore here — see note below); re-run
passed (`16 normalized lines match`).

**Verdict: load-bearing**, and structurally the opposite of the mirror-drift
trap: it extracts the real production loop by anchoring on a line proven
unique in the file (`z->effective_target_c += delta_c;`), walks backward to
the enclosing `for`, and fails closed (exit 1) if the anchor stops being
unique or the mirror's function can't be found at all — an extraction
failure is not treated as "nothing to compare, pass."

**CRLF note:** during restoration, `git hash-object` on the restored file
did not match `git rev-parse HEAD:<path>` even though `git diff` reported no
differences. This repo's tracked files carry LF blobs with autocrlf
conversion on checkout; `git hash-object` hashes the raw working-tree bytes
without applying that filter, so the two commands can disagree with no real
difference in content. For every restoration in this audit, `git diff
--quiet` (or empty `git diff` output) was treated as the authoritative
proof, matching this repo's own `project_git_show_md5_crlf_mismatch` memory
note; `git hash-object` was cross-checked as a second signal, not as sole
proof, and it agreed once autocrlf was accounted for on the other six.

### 4. `firmware/KilnFW/App/test/check_main_task_stack_budget.py`
**Guards:** the `main` task's worst-case static stack path (measured by
disassembling the real ELF and walking the direct call graph from
`app_main`) stays under 75% of `CONFIG_ESP_MAIN_TASK_STACK_SIZE` — the exact
class of bug (`218f65f7`, `IllegalInstruction`, corrupted backtrace) that
overflowed a 8192 B stack via three separately-reasonable-looking frames
that only became reachable together once the `cfg` LittleFS partition
mounted.

**Negative test:** this script already ships a `--stack-bytes` override
built for exactly this purpose. Ran it against the real `KilnCtrl.elf`
present in the shared tree (`firmware/KilnFW/build/KilnCtrl.elf`, built by
another session — read-only, not rebuilt or modified by this audit) with
`--stack-bytes 100`, forcing a 75-byte budget against the real measured
3632 B deepest path. Result: `FAIL -- 3632 B exceeds the 75 B budget`,
naming the same call path (`nvs_save_slot` → `profiles_cfg_fs_save` →
`profiles_cfg_fs_save$part$0` → `profile_encode_current_blob`) that the
normal run reports. No source or build artifact was touched, so no
restoration was needed.

**Verdict: load-bearing** for the arithmetic and fail path. **Caveat, not
tested by this audit:** whether the disassembly/call-graph EXTRACTION itself
still matches the current toolchain's actual codegen (objdump call-target
captioning, `entry a1,N` prologue shape) was not independently re-verified
here — that would require an intentional stack-inflating source change and a
full ESP-IDF rebuild, out of scope for this pass's budget. This class of
extraction drift is exactly what the script's own `SECTION_MARKER_NAMES`
comment documents was found and fixed once already (2026-09-08,
`_stext`-captioned fabricated edges), so it is a real, previously-realized
risk, not a hypothetical one — flagged as a documented gap below, not
closed.

### 5. `tools/check_nvs_key_length.ps1`
**Guards:** every `#define <NVS_KEY|NVS_NAMESPACE|NVS_PARTITION...> "..."`
literal in tracked `firmware/KilnFW/App` source is ≤15 usable characters
(ESP-IDF's `NVS_KEY_NAME_MAX_SIZE` is 16 including NUL) — the exact class
that let `"zone_normals_cfg"` (16 chars) silently fail `nvs_set_blob()` on
real hardware for weeks with zero log trace
(`project_nvs_key_too_long_zone_normals`).

**Negative test:** appended
`#define ZZ_AUDIT_NVS_KEY_TOO_LONG_LITERAL "this_is_seventeen"` (17 chars)
to `adaptive_tune.h`. Result: named the exact file:line and literal, exit 1.
Restored by hand; `git diff` empty; re-run passed (`410 files scanned, 0
violations`).

**Verdict: load-bearing.**

### 6. `firmware/KilnFW/App/test/cfgfs_nvs_only_drift_check.py`
**Guards:** a new `persist/*_cfg_fs.c` bridge file appearing without its
module prefix being added to `EXPECTED_BRIDGE_MODULES` — the exact
2026-09-08 drift where `/api/cfgfs`'s hand-maintained `nvs_only` list went
stale against real file-backing migrations (`prefs`/`profiles`).

**Negative test:** created an empty
`firmware/KilnFW/App/drivers/persist/zz_audit_dummy_cfg_fs.c` (untracked, so
no restoration needed beyond deleting it). Result: `FAILED — New bridge file
found: persist/zz_audit_dummy_cfg_fs.c`, exit 1. Deleted; `git status`
confirms nothing tracked was touched.

**Verdict: load-bearing.** Deliberately narrow in scope (documented in its
own module docstring) — it does not try to cross-check `cfg_fs_status.c`'s
actual `nvs_only`/`nvs_permanent` array contents against real bridge
coverage (a many-to-one mapping it states is not mechanically recoverable
from filenames alone), only that a brand-new bridge file gets a human's
attention. That is an honestly-scoped, not vacuous, limitation.

### 7. `tools/check_no_orphaned_checks.ps1`
**Guards:** the meta-failure this whole audit is about — a new `check_*`/
`test_*` file under `tools/` or `firmware/*/test/` that `run_all_checks.ps1`
never actually runs, neither via its `check_*.ps1` glob nor explicit wiring.
Found unwired at least three times previously
(`test_regsp_margin_against_declared.py`, `test_regsp_stale_literal.py`,
`check_saftyfw_task_count.py`).

**First attempt was a non-test:** dropping a bare `check_zz_audit_dummy.ps1`
under `tools/` PASSED — correctly, once the mechanism is understood: any
`check_*.ps1` anywhere under `tools/`/`firmware/` is unconditionally covered
by `run_all_checks.ps1`'s own recursive glob (verified by reading
`run_all_checks.ps1`'s matching `Get-ChildItem -Filter "check_*.ps1"
-Recurse` block, same exclusions), so that file was never actually an
orphan — it was exactly the covered case the design describes. This is
itself a small instance of the failure class the audit is hunting
(a negative test that doesn't actually falsify the property in question)
and is recorded here rather than quietly redone.

**Real negative test:** created
`firmware/KilnFW/App/test/test_zz_audit_orphan.py` (a `test_*.py`, which is
NOT covered by the `.ps1` glob and has no matching wrapper). Result:
```
FAILED: found 1 check/test file(s) ... test_zz_audit_orphan.py
```
exit 1. Deleted (untracked); `git status` confirms clean.

**Verdict: load-bearing**, and its self-consistency with
`run_all_checks.ps1`'s actual glob (same three exclusion patterns:
`\build\`, `\node_modules\`, dot-directories) was independently confirmed by
reading both scripts side by side, not assumed.

## Gates read and reasoned about, not live-sabotaged (documented, not tested)

Time/scope did not allow a full sabotage-rebuild-restore cycle for every
priority-class gate. These were read in full and judged structurally sound
by inspection; they are named here rather than silently omitted:

- **`tools/check_doc_hash_citations.ps1`** — resolves every backtick-quoted
  7-40 char lowercase-hex token in tracked docs against
  `git cat-file -e <hash>^{commit}`, with a small, explicitly-keyed
  false-positive allowlist for known non-hash literals. Sound design (real
  git-history lookup, not a static string match); not sabotaged live in this
  pass because doing so risks tripping the same script mid-audit on a
  planted bad hash in a tracked file.
- **`tools/check_stack_margin_registration.ps1`** — required-name list of
  `stack_margin_register()` call sites plus a `STACK_MARGIN_MAX_TASKS`
  vs. real-call-site-count guard, with a documented duplicate-name check
  (the `i2c_owner` double-registration bug). Read in full; the design
  (explicit required-name list rather than a blind create-vs-register count,
  with reasoning given for why) is sound. Not sabotaged live — doing so
  meaningfully requires touching `stack_margin.h`/call sites and a rebuild,
  which this pass's remaining budget did not extend to.
- **`tools/check_safety_trip_mask_docs.ps1`** — pins `SAFETY_TRIP_MAIN_FAULT`
  trip_mask documentation (`0x0020`, not `0x0040`) against `link_frame.c`'s
  actual `1 << (reason - 1)` formula, and fails loudly if that formula's
  shape itself changes rather than silently validating stale doc text
  against a formula that no longer exists. Read in full; sound. Not
  sabotaged live in this pass.
- **`tools/check_test_has_assertions.ps1`** — catches the two purely
  mechanical vacuous-test shapes found in the 2026-08-31 sweep (no assertion
  call at all; an assertion whose both sides are literals the test itself
  wrote). Read in full; its own header is honest about being a syntactic
  scan, not a compiler, and names its limits. Not sabotaged live in this
  pass.

## Gates not examined at all in this pass

Everything else under the `check_*`/`test_*` family — roughly 85 gates,
including but not limited to: every other `*_mirror_drift`/`*_drift`
check (`check_fuzzy_gain_mirror_drift`, `check_power_diag_flag_mirror_drift`,
`check_ramp_lock_decision_mirror_drift`, `check_ramp_stepping_gate_mirror_
drift`, `check_readiness_ct_channel_map_mirror_drift`, `check_safety_cfg_
param_table_mirror_drift`, `check_heater_output_pwm_drift`, `check_frame_a_
offset_drift`, `check_source_path_drift`, `check_stub_signature_drift`), the
remaining stack-budget checks (`check_httpd_task_stack_budget`,
`check_executor_task_stack_budget`, `check_system_uart_bridge_stack_budget`,
`check_uart_log_bridge_stack_budget`, SaftyFW's `check_saftyfw_task_stack_
budgets`), the SaftyFW isolation/guard-producer family (`check_isolation`,
`check_link_impl_isolation`, `check_guard_input_producers`, `check_unused_
setters`, `check_thermo_snapshot_producers`), the two `check_00_*_target_
build.ps1`/`check_01_*_pushed_build.ps1` pairs (already named in the plan as
its strongest existing coverage, not re-verified here), `check_uart_version_
independence`, `check_uri_handler_cap`, `check_heartbeat_contract`,
`check_heat_enable_wiring`, `check_c_files_in_cmakelists`, `check_duplicate_
symbols`, `check_host_embed_symbols_defined`, `check_no_duplicate_crc`,
`check_safety_baud_sync`, `check_test_c_files_wired`, `check_uart_version_
independence`, `check_mcp_facade_coverage`, `check_mcp_tool_count_doc`,
`check_mykicad_golden_suite_runs`, `check_relay_authority_paths`, and the
per-file UI/layout checks (`check_ui_budget_asserts`, `check_ui_responsive_
sweep`, `check_ui_shell_layout`, `check_ui_status_color`, `check_stop_bar_
body_padding`, `check_label_column_overflow_wrap`, `check_kv_narrow_stack`).
None of these were read or negative-tested in this pass. A future pass
should extend this table rather than assume the ones above generalize.

## Fixes applied

None. All 7 negative-tested gates and the 4 read-only-reviewed gates were
found load-bearing, correctly scoped, and correctly self-guarding against
going blind (missing tree, renamed symbol, extraction failure). No vacuous
gate was found in this pass's sample. This is a genuinely different outcome
from the plan's stated history (eight prior vacuous-pass instances existed
before negative-testing became standing practice) — read as evidence that
the practice has been working on the gates it has already been applied to,
not as evidence the remaining ~85 are equally sound.

## `tools/run_all_checks.ps1` result (2026-09-16, HEAD `3c3c688`)

`90 passed, 0 skipped, 4 failed` — below the `95 passed, 0 skipped, 0 failed`
baseline, but not attributable to this audit's own edits (every negative
test above was hand-restored and independently confirmed via `git diff
--quiet` before this run). This repo's tree is explicitly shared with other
sessions' live WIP, and `git status` at run time showed a large uncommitted
in-progress change touching `heat_enable.c/.h`, `profile_executor*.c`,
`safety_link_frames.c` and a brand-new, not-yet-wired
`firmware/KilnFW/App/drivers/safety/heat_owner_active_decide.c` — none of
which this audit touched. The four failures trace directly to that WIP, not
to a regression this audit introduced or missed:

1. **`check_00_kilnfw_target_build.ps1`** — FAILED: linker error, `undefined
   reference to 'heat_owner_active_decide'`. `heat_owner_active_decide.c`
   exists on disk (confirmed by `grep`) and is called from
   `safety_link_frames.c:511`, but is mid-extraction from `heat_enable.c`
   and not yet reflected correctly in the link (its object is either not
   yet added to `App/drivers/CMakeLists.txt`'s committed state or the CMake
   change is itself part of the same uncommitted WIP — that file shows
   modified in `git status` too). This is another session's in-flight
   refactor, not a pre-existing defect at a stable commit; it does not
   belong to blocker 3's scope and this audit did not sabotage anything
   under `drivers/safety/` or `drivers/control/heat_enable*`.
2. **`check_saftyfw_task_stack_budgets.ps1`** — FAILED: "STALE ELF", because
   `firmware/SaftyFW/src/tasks/safety_core.h` was written (by another
   session) after `SaftyFW.elf` was last built — the check's own staleness
   guard (2026-09-09 fix) correctly refused to report a measurement rather
   than repeat the "measured 184 B with no seqlock code in it" false-good
   reading. This is the guard working as designed against concurrent
   editing, not a broken gate.
3. **`check_mykicad_golden_suite_runs.ps1`** — FAILED: a Python traceback
   from `pytest` inside the `mykicadMcp` venv, no diagnostic detail
   propagated by the `Out-String` capture. Not investigated further this
   pass (out of blocker 3's priority list — sourcing/KiCad tooling, not
   safety/config/stack-budget/build); flagged here rather than silently
   absorbed into "pre-existing."
4. **`check_doc_citations.ps1`** — FAILED: nine stale `file:line` citations
   in `docs/audits/review_heat_enable_epoch_ffa92004_tc_type_28e89a85_
   2026-09-16.md` (added by the same concurrent session, per `git status`
   showing it as a new, uncommitted `A` file) pointing past
   `test_heat_enable.c`'s and `profile_executor.c`'s current end-of-file —
   consistent with citations written against a pre-refactor line count
   before the same in-flight `heat_enable` split shifted lines underneath
   them. Not this audit's file, not this audit's edit.

No commit exists yet at a "before this WIP" point to diff against for
items 1, 2 and 4 — the WIP is uncommitted, so the correct comparison is "is
this failure explained by uncommitted changes visible in `git status` right
now," not "does it also fail at some parent commit," and the answer for all
three is yes, by direct inspection of what changed. Item 3 was not traced to
a specific change and is named as an open question rather than assumed
pre-existing.

## Open item carried forward

`check_main_task_stack_budget.py`'s (and by the same construction, its
sibling stack-budget scripts') objdump-based call-graph EXTRACTION has
already drifted from real codegen once (`_stext`-captioned fabricated
edges, fixed 2026-09-08) and was not re-verified against the current
toolchain by an intentional source-side stack inflation + rebuild in this
pass — only its compare-and-fail arithmetic was exercised, via the
`--stack-bytes` override, against a real (not sabotaged) ELF already present
in the shared tree. A real closure of this item requires: add an
intentionally oversized local to a function reachable from `app_main`,
`build_kilnfw`, confirm the script's own reported worst-case path grows to
include it, then hand-revert and rebuild clean.
