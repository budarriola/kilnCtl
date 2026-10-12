# Review: devbreak3 fixes for REVIEW_DEVBREAK2 (2026-10-11)

Range reviewed on origin/dev (two commits):

| SHA | Subject |
|---|---|
| d4e7001da | devbreak3: harden reset-fence and build-gate checks, gate table touch_cal row, citations, nits (REVIEW_DEVBREAK2 LOW-1..5) |
| a755d2074 | REVIEW_DEVBREAK2: record fix status and measured ceilings |

Method: fresh worktree `C:\wt\rvdb3_tvb5nl` at a755d2074 (minted with
submodules). Every touched check was run there. Pytest and `selfcheck.py`
were run with `uv run --frozen` inside the worktree. Suspected gaps were
negative-tested with `tools\negtest.ps1` (every baseline PASS, every copy
removed, real tree unchanged). The two stack checkers were re-run against
the ELF the fixer measured. No code was changed. The bench board was not
touched.

Summary: no HIGH, no MED. Four LOW. Several INFO. The zones_http_client
change is behaviour-neutral: the wire key is still `expected_generation`.

## Tests run

| Test | Result |
|---|---|
| pytest: test_mcp_server_control_zone_relay_mask, test_bench_test_cases_aux, test_zones_http_client, test_selfcheck_zones_fields, test_config_presets, test_control_set_zone_model, test_mcp_server_aux_convert, test_mcp_server_control_{coupling,model_fields,relay_type,zone_coupling,zone_limits,zone_type}, test_pctools_write_review_2026_10_09, test_web_ui_client, test_live_bench_tuning (every test file that imports zones_http_client / build_post_body, minus the heat/ota/smoke bench cases) | 421 passed, 4 skipped |
| `uv run python selfcheck.py` (tools/PcTools) | "all checks passed"; the renamed row "zones POST top-level fields: _TOP_FIELD_FORM_KEY + _TOP_POST_ONLY_FORM_KEYS match firmware" PASS, no keys missing or extra |
| check_reset_fence_hooks.ps1 | PASS |
| check_build_gate_usage.ps1 | PASS (21 callers) |
| check_build_gate_reentrant.ps1 | PASS |
| check_gate_negative_test_table.ps1 | PASS (198 checks, 199 rows) |
| check_doc_citations.ps1 | PASS |
| check_doc_hash_citations.ps1 | FAIL, 5 citations (see LOW-3; none introduced by this range, one sits in a paragraph this range edited) |
| check_system_uart_bridge_stack_budget.py on `C:\wt\checkbuild_eb602e08e8` ELF (built from 48caa9a8a) | 3216 B deepest, ceiling 3216, honest headroom 580 B, OK |
| check_httpd_task_stack_budget.py on the same ELF | autotune_start_post_handler 4528 B, ceiling 4528, honest free 1864 B (LOW class), OK |

Wire key: `_EXPECTED_GENERATION_KEY = "expected_generation"`
(zones_http_client.py line 675) and build_post_body sets
`fields[_EXPECTED_GENERATION_KEY]`. The firmware reads the same literal
(zones_http_post.c line 362). test_build_post_body_pins_expected_generation
asserts the literal string, and the negtest below proves it does.

## Negative tests

| Target | Mutation | Result |
|---|---|---|
| pytest (test_zones_http_client + test_selfcheck_zones_fields) | `_EXPECTED_GENERATION_KEY = "expected_gen"` | CAUGHT |
| reset_fence_hooks | main.c predicate return gets `&& 0` appended | CAUGHT (confirms fixer) |
| reset_fence_hooks | relay_authority.c `bool refuse = 0 && (s_reset_in_flight_depth != 0) && ...` | CAUGHT (confirms fixer) |
| reset_fence_hooks | relay_authority.c `refuse = false;` inserted between the assignment and `return refuse;` | MISSED |
| reset_fence_hooks | relay_authority.c `if (me != NULL) return false;` inserted after `void *me = ...;` | MISSED |
| reset_fence_hooks | main.c `#define relay_authority_reset_refuses_kiln_nvs_writer() false` above the predicate | MISSED |
| reset_fence_hooks | relay_authority.c `#define s_reset_erases_kiln_nvs false` above the function | MISSED |
| reset_fence_hooks | main.c harmless rename of the parameter `partition` -> `part` (brittleness probe) | CAUGHT (false RED) |
| host test test_link_watchdog (`build_host_tests.ps1 -Only test_link_watchdog`) | the `refuse = false;` mutation above | CAUGHT (test_link_watchdog.c:243, 259, 261) |
| host test test_link_watchdog | the early `return false;` mutation above | CAUGHT (same three lines) |
| build_gate_usage | misuse in a new `firmware/KilnFW/tools/check_build_gate_reentrant.ps1` (same leaf name) | CAUGHT (confirms LOW-2 fix) |
| build_gate_usage | control: misuse in new `tools/gatetools/gate_misuse.ps1` | CAUGHT |
| build_gate_usage | misuse in new `tools/buildtools/gate_misuse.ps1` | MISSED |
| build_gate_usage | misuse in new `firmware/KilnFW/App/build_helpers/gate_misuse.ps1` | MISSED |

## LOW-1: the backslash fix made the `build*` directory exclusion live, and it is too wide

d4e7001da changed the exclusion at check_build_gate_usage.ps1 line 26 from
`[\/]...` to `[\\/]...`. Before, it never matched a Windows path, so nothing
was excluded. Now any directory whose name starts with `build` is skipped,
at any depth, matched against the absolute path:

- A gate caller under `tools/buildtools/` or `.../App/build_helpers/` is not
  linted (both MISSED above). Neither path is gitignored, so such a file can
  be committed. No such directory exists today.
- If the repo itself sits under a parent directory whose name starts with
  `build` (for example a worktree minted with `-Label buildfix`, giving
  `C:\wt\buildfix_xxxx\`), every file is excluded. The `$seen -eq 0` guard
  turns that into a loud FAIL, not a vacuous PASS, so this is a false RED.

The same absolute-path match applies to `.venv` and `node_modules`.

Scenario: someone adds a helper directory named `buildtools` with a gated
compile wrapper that sleeps inside the slot; the lint stays green while the
slot is held across the sleep.

Fix: match the exclusion against the repo-relative path (the same string the
exemption already computes), and narrow it to the real output names (`build`,
`build_*` only at known places, or simply `(^|/)build/`), or derive it from
`git ls-files` so only tracked scripts are linted.

## LOW-2: check_reset_fence_hooks body pin is brittle and adds little; the main.c side has no other guard

The new exact-match rules (lines 67 and 77):

- Brittle: a behaviour-neutral rename of the main.c parameter is a false RED
  (CAUGHT above). Reordering operands, `!partition`, a local variable, extra
  parentheses, or renaming `me` in relay_authority.c all fail the same way.
  The failure message is clear, so a refactorer will find it, but the check
  then has to be edited in lockstep.
- Bypassable: the relay_authority.c regex allows any brace-free text before
  the assignment and between it and `return refuse;` (`[^{}]*?`), so a
  braceless early `return false;` or a later `refuse = false;` passes.
  Macros are not seen at all, in either file.

The relay_authority.c bypasses are caught by test_link_watchdog.c (both host
negtests CAUGHT), which the gate table correctly names as the semantic guard.
So the body regex buys little beyond what the host test already gives, at
the price of brittleness.

The main.c predicate is different: main.c is not compiled into any host
test, so check_reset_fence_hooks is its only guard, and a function-like macro
shadowing the predicate name passes. This needs deliberate sabotage, hence
LOW.

Fix (optional): drop or loosen the relay_authority.c body pin and rely on
test_link_watchdog.c; for main.c, also fail when the stripped text contains a
`#define` of `relay_authority_reset_refuses_kiln_nvs_writer`, `strcmp` or
`kiln_nvs_reset_refuses_write`, or move the predicate into a host-testable
file.

## LOW-3: the citation paragraph d4e7001da edited still carries a stale blob pin

d4e7001da edited release_gate_vacuity_audit_2026-09-16g.md around line 153
(LOW-4: line number replaced by a symbol) but left the blob pin for
profile_executor_relay_io.c in the same paragraph (line 163). That file's
blob at d4e7001da, at its parent 9417f11b8 and at the tip is 75f54767, not
the cited value; it changed in 75eee94b2, which landed before the fix. So
check_doc_hash_citations.ps1 was already failing on dev when the fix landed
and still fails. The same stale value is also quoted (backticked) in
REVIEW_DEVBREAK2_2026-10-10.md line 163. The other three failures (two in
REVIEW_TRIPWP1_2026-10-10.md, which backtick hex constants that are not
hashes, and one link_frame.c pin in release_gate_vacuity_audit_2026-09-17.md
after a364d1703) are outside this range.

This is LOW-5 of REVIEW_DEVBREAK2 recurring within hours, while the fix
status says "LOW-5: no change".

Fix: in the 09-16g paragraph replace the blob pin with the symbol reference
the fixer already wrote (the claim is "the file still holds the call", not
byte identity), and de-backtick the non-hash values in the two review docs.

## LOW-4: system-bridge headroom threshold is quoted inconsistently and enforced nowhere

The fix status says "honest headroom 580 B (> 512 B)". The checker's own
comment (check_system_uart_bridge_stack_budget.py line 78) and REVIEW_DEVBREAK2
call the floor 256 B. Neither number is enforced: that checker fails only on
the ceiling, never on honest headroom (unlike the httpd checker, which fails
below 15 %). A ceiling retightened upward, as the comment invites, can walk
headroom below either floor with the check green.

Fix: add an honest-headroom floor to the system-bridge checker (pick one
number, 512 B matches the all-task checker's absolute floor), so a future
stack bump is forced instead of remembered.

## MED-1 measurement: method is right, the ELF is not the landed tip again

- Method: for httpd and the system bridge there is no `--dump-ceilings`, and
  reading the checker's printed deepest path is correct: that printed total
  is exactly the value each checker compares against its ceiling. Re-running
  both on the same ELF reproduces 3216 B and 4528 B.
- Source: the ELF was built from 48caa9a8a. d4e7001da sits 23 commits later,
  and the firmware differs in heat_enable.c, profile_executor.c/.h,
  profile_executor_internal.h, profile_executor_relay_io.c,
  main_control_bringup.c and main.c (75eee94b2 among them). So the "measured
  on the tip" statement holds for 48caa9a8a only.
- Risk is smaller than REVIEW_DEVBREAK2 stated: a ceiling below the tip's
  real depth makes the standing check FAIL on the next target build; it
  cannot hide growth. Only a ceiling above the real depth hides growth, and
  only up to that ceiling. So this is INFO: the next dev-tip
  `check_00_kilnfw_target_build.ps1` run settles it. No checkbuild ELF that
  contains a755d2074 existed at review time, and this review did not build.

## INFO

- The gate table row for check_reset_fence_hooks points at "negtest in
  REVIEW_DEVBREAK3 fix status", but the fixer wrote that status into
  REVIEW_DEVBREAK2_2026-10-10.md. This review independently confirms both
  mutations CAUGHT, so the reference now resolves here; better to point it at
  the DEVBREAK2 file.
- LOW-2 of DEVBREAK2: the exemption is still whole-file for
  tools/check_build_gate_reentrant.ps1 (the per-line marker alternative was
  not taken), so a new misuse added to that file is unchecked. It now cannot
  be spoofed by a same-named file elsewhere (CAUGHT). A relative `-Root .`
  breaks the exemption's Substring and gives a false RED on the reentrant
  test (default invocation is absolute, so harmless).
- check_reset_fence_hooks line 75 strips `//` comments with a literal newline
  inside a single-quoted string rather than `[^\r\n]`; it works on this LF
  checkout, but is easy to break by an editor; use the same pattern as line
  46.
- The gate table's check_reset_fence_hooks row has one more column than its
  neighbours (pre-existing; the table check still passes).
- LOW-1 (touch_cal row) and LOW-4 (09-16g line cite replaced by symbol) are
  correctly fixed; the selfcheck label nit and the constant nit are fixed.

## Fix status (2026-10-11)

- LOW-1: check_build_gate_usage.ps1 matches the repo-relative path against `build`, `build_agent`, `build_sim_*_obj`, `checkbuild_<hex>`, `.git`, `node_modules`, `.venv`; buildtools/ and build_helpers/ misuse CAUGHT (negtest), a build* parent no longer false-REDs.
- LOW-2: check_reset_fence_hooks.ps1 main.c predicate matched with a free parameter name (backreference), macro shadowing refused; relay_authority.c pin keeps the three terms but forbids early return / reassignment and macros. All four MISSED mutations now CAUGHT, rename passes. Line-75 regex now uses [^
].
- LOW-3: stale citations fixed (symbol reference for relay_io, link_frame.c blob refreshed, non-hash hex de-backticked).
- LOW-4: enforced: check_system_uart_bridge_stack_budget.py fails below MIN_HONEST_HEADROOM_BYTES = 512 B (580 B today).
- INFO: gate table row points at the DEVBREAK2 fix status.
