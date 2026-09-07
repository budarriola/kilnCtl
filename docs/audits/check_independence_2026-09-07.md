# Check independence audit — 2026-09-07

Trigger: `e19f4e2f` found `check_mcp_tool_count_doc.ps1` recomputing its
"expected" tool count with the same flawed regex the docs it checked had
used — the check could agree with a wrong doc and never disagree. That has
since been fixed (the script now derives the real count independently from
`@_srv._tool()` decorator lines plus `workbench.attach(...)`/`BUNDLES`
parsing, and compares both `CLAUDE.md` and `docs/MCP_SERVERS.md` against
that independently-derived number). This audit sweeps every `check_*.ps1`/
`check_*.py` discovered by `tools/run_all_checks.ps1` for the same shape
(self-referential expected value) and for silent-skip-on-missing-input.

Columns: **Assertion** (what must hold), **Expected-value source** (where
the "should be" number/text comes from), **Independent?** (is that source
different from the artifact being checked), **Can-fail proven?** (sampled
from the check's own header/design notes — most of these checks document
their own real-instance failure or a negative-test pass at authoring time;
noted `documented` where the header cites the real bug it was proven
against, `not verified this pass` where I did not re-run a break/fix cycle).

## Firmware/KilnFW/App/test

| Check | Assertion | Expected-value source | Independent | Can-fail |
|---|---|---|---|---|
| check_approach_rate_cap_mirror_drift.ps1 (→ .py) | test mirror's arithmetic body matches profile_executor.c's real body | production .c file (profile_executor.c) | Yes | documented |
| check_fuzzy_gain_mirror_drift.ps1 (→ .py) | test_closed_loop.c mirror matches pid_fuzzy_prepare_gains() | production .c file | Yes | documented |
| check_heater_output_pwm_drift.ps1 (→ .py) | plant_sim.py's `_pwm_render()` numerically matches heater_output.c (MSVC harness) | real heater_output.c compiled and run | Yes | documented |
| check_pid_fuzzy_drift.ps1 (→ .py) | fuzzy_band_probe.py numerically matches pid_fuzzy.c (MSVC harness) | real pid_fuzzy.c compiled and run | Yes | documented |
| check_power_diag_flag_mirror_drift.ps1 (→ .py) | flag constants agree across 3 C headers | the other two headers, not each other's doc | Yes | documented |
| check_ramp_lock_decision_mirror_drift.ps1 (→ .py) | test_ramp_lock_onesided.c mirrors profile_executor.c's lock_lagging_mask() | production .c file | Yes | documented |
| check_ramp_stepping_gate_mirror_drift.ps1 (→ .py) | same mirror file vs. profile_executor.c's step_schedule() | production .c file | Yes | documented |
| check_frame_a_offset_drift.ps1 (→ .py) | byte offsets agree across 3 files that encode Frame A | independent files, not a shared table | Yes | documented |
| check_source_path_drift.ps1 (→ .py) | every path literal referenced by scanned .py/.ps1 resolves on disk | `git ls-files` / filesystem, not the literal's own file | Yes | documented (fail-closed floor on match count too) |
| check_stub_signature_drift.ps1 (→ .py) | host stub signatures match real ESP-IDF headers | real installed ESP-IDF headers (skips gracefully only when IDF absent — a legitimate, documented non-failure state; missing Python still fails) | Yes | documented (audited 0 mismatches 2026-09-04, `--fatal-on-clean`) |
| check_wire_protocol_fingerprint.ps1 (→ .py) | live #define fingerprint == checked-in manifest, version bump required on drift | `wire_protocol_fingerprints.json`, human-updated only via explicit `--update` | Yes (manifest is a human-reviewed record, not a cache the script writes on its own during a normal run) | documented |
| check_flash_partition_map.ps1 | partition table arithmetic (gaps/overlaps/remainder) is internally consistent and matches doc's cited figures | `partitions.csv`, computed by the script itself from the CSV, not from the doc | Yes (CSV is the artifact; doc figures are downstream of it, checked against script's independent recompute) | documented |
| check_flash_worker_lint.ps1 (→ .py) | flash-worker re-entrancy lint over drivers/*.c | static grep over production source | Yes | documented |
| check_js_host_tests.ps1 | Node-only JS harnesses actually get executed and pass | the harness files' own assertions | Yes | documented |
| check_kv_narrow_stack.ps1 | `.kv dd{min-width:0}` + narrow-viewport stacking rule present in theme.css | theme.css text itself (grep for exact rule) | Partial — no-compiler mirror of a CSS rule; independent of any doc, but a same-shaped rule with different text would not be caught (documented limit, not vacuous) | documented |
| check_label_column_overflow_wrap.ps1 | label-column CSS avoids `overflow-wrap:anywhere`/`word-break:break-all` while value column may use it | theme.css text | Yes | documented (3 real 2026-09-01 instances) |
| check_profile_executor_wd_input_producers.ps1 | every `profile_executor_wd_input_t` field assigned by a production .c file | struct fields (header) vs. drivers/*.c producers | Yes | documented |
| check_safety_link_status_producers.ps1 | every `safety_link_status_t` field assigned by production frame-parsing code | struct fields vs. safety_link_frames.c | Yes | documented |
| check_sdkconfig_defaults_applied.ps1 | specific plan-changed keys reached the generated `sdkconfig`, not just `sdkconfig.defaults` | generated (gitignored) `sdkconfig`, independent of `sdkconfig.defaults` | Yes | documented (a0b8711 regression) |
| check_stop_bar_body_padding.ps1 | nav.js/app.js still contain the specific lines the fixed-bar padding fix depends on | source text (no renderer available) | Partial — text-presence proxy, documented as the best available without a browser | documented as intentional proxy |
| check_thermal_guard_cfg_producers.ps1 | every `thermal_guard_cfg_t` field assigned by production code or zones_config accessor | struct fields vs. producer files | Yes | documented (progress_band_c real gap) |
| check_thermal_guard_input_producers.ps1 | every `thermal_guard_input_t` field assigned in App/drivers | struct fields vs. producer files | Yes | documented |
| check_ui_budget_asserts.ps1 | specific `_Static_assert` budget lines still present verbatim | source text of the .c files (mirrors what the compiler enforces) | Yes — compiler is authoritative, this is the no-compiler mirror against deletion | documented |
| check_ui_responsive_sweep.ps1 | headless-Chrome sweep of every `*_page.html` at 6 widths: no overflow/overlap/undersized targets | live-rendered DOM layout | Yes | documented; **silent-skip risk**: exits 0 when `node`/deps missing (see Findings) |
| check_ui_shell_layout.ps1 | specific WEB_UI_RESPONSIVE.md sec-6 CSS rules still present | source text (no-compiler mirror of what the sweep can't prove was never deleted) | Yes | documented (proven to fail 2026-09-02) |
| check_ui_status_color.ps1 (→ .mjs) | every status token clears contrast floor; no new colour-only rule outside allowlist | computed contrast against theme.css colors + allowlist/exceptions JSON (human-reviewed) | Yes | documented; **silent-skip risk**: exits 0 when `node` missing |
| check_wire_protocol_fingerprint.ps1 | (listed above) | | | |

## Firmware/KilnFW (other) / SaftyFW / PcTools

| Check | Assertion | Expected-value source | Independent | Can-fail |
|---|---|---|---|---|
| check_gcov_coverage.py | new/changed lines in a commit are covered by gcovr | gcovr's own coverage XML, from an actual instrumented build | Yes | not verified this pass (standard gcovr wrapper, no repo-specific self-reference found) |
| check_bootloader_builds.ps1 | SaftyFW bootloader (a separate top-level CMake project) still builds standalone | cmake/ninja actually building it | Yes | documented (f0d5eb7 regression) |
| check_guard_input_producers.ps1 | every `safety_guard_input_t` field assigned by `safety_core_build_input()` | that function's body vs. the struct | Yes | documented (3 real instances) |
| check_isolation.ps1 | safety_core.c doesn't `#include` the link header; link_task.c doesn't touch GPIO6 | source text, grep for the two named violations | Yes | documented |
| check_link_impl_isolation.ps1 | no second CRC/byte-stuffing implementation exists outside CommonFW | grep for function definitions matching kilnlink's own names, across the whole tree except CommonFW | Yes | documented |
| check_unused_setters.ps1 | every public `*_set_*()` has ≥1 call site outside its own def | grep for call sites vs. declarations, across all of src/ | Yes | documented (current_sense_set_cal real instance) |
| check_chip_partition_table.py | RUNNING firmware's partition table matches `partitions.csv` | live board read via debug interface (not JTAG memory-read, board's own reported partitions) | Yes | documented (notes an abandoned broken approach — evidence of real iteration) |

## tools/

| Check | Assertion | Expected-value source | Independent | Can-fail |
|---|---|---|---|---|
| check_bridge_reject_reason.ps1 | reasonless-reject replies aren't byte-identical to legitimate empty-success replies for ambiguous subcommands | source text of the reply-building call sites | Yes | documented |
| check_c_files_in_cmakelists.ps1 | every tracked .c file is named in the CMakeLists.txt that builds it (or allowlisted) | CMakeLists.txt vs. `git ls-files`, two independently-maintained lists | Yes | documented (tick_timing.c real 2026-08-28 instance) |
| check_doc_citations.ps1 | every `file.c:NNN` doc citation resolves to an existing file/line | the cited file's real line count | Yes | documented (5th stale-claim audit) |
| check_doc_hash_citations.ps1 | every backtick-quoted hex token in docs resolves to a real git commit | `git cat-file -e <hash>^{commit}` | Yes | documented; known-false-positive allowlist requires human confirmation per entry, not auto-derived |
| check_duplicate_symbols.ps1 | no external symbol is defined in >1 KilnFW build object | real build output (.obj files) | Yes | documented; **silent-skip**: exits 0 (loud SKIP message) when `firmware/KilnFW/build/` doesn't exist — see Findings |
| check_hal_include_boundary.ps1 | HAL boundary include rules (strict allowlist + a now-empty ratchet) hold | source #include lines vs. an allowlist file + a checked-in count baseline, bumped only via explicit `-UpdateBaseline` | Yes (ratchet baseline is human-gated, same shape as wire_protocol_fingerprint's `--update`) | documented |
| check_heartbeat_contract.ps1 | PC-side `link_hub.py` heartbeat actually keeps the C-side link watchdog fed | cross-language pairing: Python producer vs. C `#define` consumer, neither derived from the other | Yes | documented (4th "producer doesn't exist" instance class) |
| check_heat_enable_wiring.ps1 | every real heat-commanding path also acquires/releases safety-processor heat_enable | grep over profile_executor.c/autotune_engine.c/danger_mode.c/uart_bridge.c call sites | Yes | documented (real, live-hardware-adjacent 2026-08-29 bug) |
| check_host_embed_symbols_defined.ps1 | every ESP-IDF EMBED_FILES symbol referenced by a host-compiled driver has a host-side definition | test/*.c definitions vs. drivers/*.c references | Yes | documented (333dd4e real instance) |
| check_mcp_facade_coverage.ps1/.py | every registered MCP tool reachable via search facade; every taxonomy entry names a real tool | live tool registration (`@_srv._tool()` + workbench bundles) vs. `kicad_facade.py`/taxonomy — two independently maintained artifacts | Yes | documented (safety_get_diag, coupled_ident_*, plant_sim_* real gaps) |
| check_mcp_tool_count_doc.ps1 | CLAUDE.md and docs/MCP_SERVERS.md's quoted kilnctrl tool counts match the real registered count | **now** independently recomputed from `mcp_server*.py` decorator lines + `workbench.py` BUNDLES table (previously: recomputed with the same flawed grep the docs used — the `e19f4e2f` bug) | **Yes, as of the fix already in tree** | documented; this is the instance that triggered this audit |
| check_mykicad_golden_suite_runs.ps1 | mykicadMcp's golden/real-board pytest suite actually runs (0 collected or any SKIPPED test in a golden file = fail) | pytest's own run/skip/collect status against the real committed kiln.kicad_pcb | Yes | documented (conftest fixture regression, ~19 tests silently skipped for over a week) |
| check_no_duplicate_crc.ps1 | no second CRC-16/byte-stuffing implementation exists outside CommonFW | grep for the literal 0x1021 polynomial constant, tree-wide except CommonFW | Yes | documented |
| check_nvs_key_length.ps1 | every NVS key/namespace/partition literal ≤15 usable chars | ESP-IDF's real `NVS_KEY_NAME_MAX_SIZE` constant (16), independent of any doc | Yes | documented (zone_normals_cfg real instance) |
| check_nvs_write_guard_coverage.ps1 | every NVS/flash write call site in PSRAM-stack-guarded modules carries the `caller_stack_is_external()` guard | source text, call-site-level scan (not file-level) | Yes | documented (3rd-pass regression found one level down from an earlier "fixed" pass) |
| check_relay_authority_paths.ps1/.py | no PC-side relay-write call bypasses `relay_authority`/`io.set_relay()` | source-level call-site scan under tools/PcTools | Yes | documented (current_sense_commissioning.py real instance). *Off-limits for edits this pass — audited read-only per task scope.* |
| check_relay_writes_through_owner.ps1 | `kiln_io_set_relay_mask()`/`kiln_io_all_relays_off()` only called from kiln_io_owner.c | grep over all of firmware/KilnFW for call sites outside that one file | Yes | documented (5, then 6, real bypasses found before this existed) |
| check_safety_baud_sync.ps1 | baud rate literal agrees across 4 independent source locations | the other 3 locations, not a shared constant | Yes | documented (measured real framing-error storm on mismatch) |
| check_safety_call_results_checked.ps1 | named safety-relevant calls' `esp_err_t` result is captured into a variable at every call site | source-level call-site scan (AST/regex over the call, not a self-copy) | Yes | documented (danger_mode.c 2bcdc2d real instance, widened 2026-09-07) |
| check_safety_trip_words_sync.ps1 | trip/warn word tables agree across 3 hand-mirrored locations (1 C header, 2 HTML/JS files) | the other locations, keyed by guard id, not a shared source | Yes | documented (52f4c944 real instance) |
| check_stack_margin_baseline.ps1 (→ .py) | checked-in stack-margin baseline JSON's `level` label matches independent classifier; no CRITICAL; no un-allowlisted LOW | `stack_margin_calc.h`'s 15%/30% classifier (independent function) applied to the file's own numbers, plus a human-reviewed `KNOWN_LOW_ALLOWLIST` | Yes for the label-vs-classifier check; the underlying hwm/configured numbers are a stale, human-recaptured snapshot by design (documented explicitly as a floor, not a live sensor) | documented |
| check_stack_margin_registration.ps1 | every long-lived task is registered with stack_margin.c; `STACK_MARGIN_MAX_TASKS` ≥ real call-site count | source-level task list vs. real `stack_margin_register()` call sites, plus an independently-maintained required-task list | Yes | documented (2026-09-02 cap-vs-count off-by-one real instance) |
| check_test_has_assertions.ps1 | every executed test function has ≥1 real (non-tautological) assertion | syntactic scan of the test file itself for assertion calls / literal-vs-literal tautology shape | Yes — checks the test's own text but the assertion is about a structural property (presence/tautology-shape) of that text, not a value copied from elsewhere in the same file | documented (2 real vacuous-test instances found the same session it was authored) |
| check_uart_version_independence.ps1 | `UART_PROTOCOL_VERSION` never textually derived from `KILNLINK_*` | source text of the one #define site | Yes (checks for an anti-pattern in the definition itself, not a copied value) | documented (3 real historical instances) |
| check_uri_handler_cap.ps1 | `max_uri_handlers` cap ≥ real count of `.uri = "..."` route registrations tree-wide | grep count over every drivers/*.c file, independent of the cap's own comment history | Yes | documented (4th real regression, cap fell behind 3 times already) |

## Findings

1. **No new self-referential ("checks itself") instances found.** The one
   known instance of the exact `e19f4e2f` shape —
   `check_mcp_tool_count_doc.ps1` recomputing its expectation with the same
   flawed method as the doc it graded — is already fixed in this tree: it
   now derives the real tool count from `@_srv._tool()` decorator lines plus
   `workbench.py`'s `BUNDLES` table, independent of either doc's own prose.
   Every other check sampled compares two artifacts that are maintained (and
   can drift) independently: production code vs. a test mirror, one header
   vs. another, a doc citation vs. the real file, a struct definition vs. its
   producer file, a live board read vs. a checked-in CSV, or a
   human-reviewed manifest requiring an explicit `--update`/`-UpdateBaseline`
   step (never a value the script silently re-derives from the same source
   on every run). No instance of "mirror compared against a copy of the
   mirror" or "allowlist read from the file it audits" was found.

2. **Silent-skip-on-missing-input (category 4), three confirmed:**
   - `check_duplicate_symbols.ps1` — exits 0 with a yellow "SKIP: no
     firmware/KilnFW/build/ found" message when the build directory doesn't
     exist. Loud in its own output, but `run_all_checks.ps1` has no
     SKIP-vs-PASS distinction (grepped — it only branches on exit code), so
     a fresh checkout or a build that was never run reports this check as a
     plain pass in the aggregate.
   - `check_ui_status_color.ps1` — exits 0 ("SKIPPED -- no `node` on PATH")
     when Node isn't installed, same aggregate-reporting gap.
   - `check_ui_responsive_sweep.ps1` — has 3 early `exit 0`s (missing
     `node`, plus at least one more environment-precondition path) with the
     same shape.
   These are documented, deliberate design choices (their headers
   explicitly reason that a missing toolchain is an environment fact, not a
   defect) and are not vacuous in the `e19f4e2f` sense — but they do match
   the literal category-4 ask ("exits 0 when its input is absent") and mean
   a machine with no Node on PATH silently loses UI-layout and
   colour-contrast coverage while `run_all_checks.ps1` still reports green.
   Not fixed this pass (would require `run_all_checks.ps1` itself to gain a
   SKIP status distinct from PASS — a design change to the runner, out of
   this audit's scope, flagged for a follow-up).

3. **No fixes were needed this pass.** The specific defect this audit was
   commissioned to sweep for had already been remediated before this audit
   began; the remaining ~55 checks sampled all derive their expected value
   from a source materially independent of the artifact under test.

## Scope note

This is a documentation/design-level audit (each check's own header comment
plus its comparison logic), not a fresh break/break/restore negative-test
pass on all 61 checks — that would be a multi-day undertaking on its own.
`check_relay_authority_paths.ps1`/`.py` were read but not modified, per this
session's scope restriction. Every check listed already carries, in its own
header, a cited real-world instance of the defect it was written to catch
(or, for the handful without one, an explicit design rationale) — used here
as the "can-fail proven" evidence in place of re-running a live
break/fix/restore cycle on each one.
