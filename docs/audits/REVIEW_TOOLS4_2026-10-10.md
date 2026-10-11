# Review: tools batch 4 (toolfx6, auxgap), 2026-10-10

Reviewer: Opus, worktree at origin/dev 68e3b4966. No board access.

Commits reviewed:

- `ac2b1b217` (toolfx6): land.ps1/dev_promote.ps1 `-PinCheckScript`/`-ChecksScript`
  seam refusals, OVERRIDE notice, `Add-SshBatch` in
  `check_submodule_pins_pushed.ps1`, wider network-error SKIP patterns, HEAD
  guard, reworked tests.
- `0cffc8086` (auxgap): MCP tool `control_set_zone_relay_mask`
  (`mcp_server_control.py`), bench case AX-C03 using it, tool count 232,
  `test_mcp_server_control_zone_relay_mask.py`.

## Summary

| Sev | Commit | Finding |
|-----|--------|---------|
| HIGH | 0cffc8086 | AX-C03 expects HTTP 400; firmware answers 409 for an aux-owned relay. Case can never PASS on hardware. |
| MED | 0cffc8086 | AX-C03 accepts any 400 as PASS without checking the body (unconfigured-relay 400 is a false PASS). |
| MED | 0cffc8086 | AX-C03 neither restores nor taints when the write was accepted but the tool reported read-back/collateral/re-fetch failure. |
| MED | ac2b1b217 | dev_promote seam guard uses a path-string comparison; a stub pin check can be pointed at the real repo via another path spelling or a worktree copy of the script. |
| LOW-MED | ac2b1b217 | land.ps1 seam gate only requires `-AllowStandaloneClone`, which does nothing else inside a linked worktree. |
| LOW | 0cffc8086 | Tool range is 0..0xFFFF; firmware parses a u8. |
| LOW | 0cffc8086 | Omit-preserved field stripping in the tool is untested (negtest MISSED). |
| LOW | 0cffc8086 | `docs/BENCH_TEST_SYSTEM_PLAN.md:322` AX-C03 row stale. |
| LOW | 0cffc8086 | CLAUDE.md count sentence still says "as of 2026-10-07". |
| LOW | ac2b1b217 | New `schannel` netPattern alternative untested (negtest MISSED). |
| LOW | ac2b1b217 | dev_promote OVERRIDE line prints "pass(stub:...)" before the stub runs. |
| LOW | pre-existing | `test_env_opt_in` fails when `KILNCTL_AUX_BENCH_CONFIRM=1` is set in the environment. |

The tool itself (`control_set_zone_relay_mask`) is sound: confirm gate is
`confirm is not True`, mid-run precheck plus mode-gate 409 mapping, body built
through `_strip_omit_preserved_zone_fields(build_post_body(...), None)` like
`control_set_zone_type`, exact read-back of `relay_mask`, collateral diff
excluding only the target zone's `relay_mask`.

The network SKIP patterns cannot by themselves turn an auth failure into a
passing pin check (analysis below).

## HIGH-1: AX-C03 expects 400, firmware answers 409

`tools/PcTools/src/kilnctrl/bench_test/cases_aux.py` `_case_ax_c03` returns
PASS only when the write status is 400. Firmware:

- `firmware/KilnFW/App/drivers/http/zones_http_post.c:650-667` — the aux
  conflict sets `httpd_resp_set_status(req, "409 Conflict")`, body "a zone
  relay_mask claims a relay an aux (spare-relay) output already owns --
  disable that aux output first".
- `firmware/KilnFW/App/test/test_zones_http.c:2343` asserts 409 for this case.
- `docs/SPARE_RELAY_ONOFF_PLAN.md:474`: "zone relay_mask containing relay 4 ->
  expect 409".

On hardware AX-C03 therefore reports INCONCLUSIVE "no firmware verdict
(status 409)" forever. Same mistake AX-C02 made before
(`docs/BENCH_TEST_LOG.md:3091`). The unit tests
(`test_bench_test_cases_aux.py` `C03DefaultWriterTest`) encode invented
firmware text "refused by firmware (HTTP 400): claimed by an aux output", so
they pass against the wrong contract.

Fix: expect 409; require the refusal text to contain "aux" / "already owns";
update the tests to the real firmware body.

## MED-1: any 400 is a PASS

`zones_http_post_parse.c:173-184` answers 400 "zone relay_mask references an
unconfigured relay" when a mask bit exceeds `relay_count`. If the bench's
relay 4 is outside the configured count, AX-C03 (once fixed to 409 this
specific path goes away, but the general point stands) must not accept a
refusal for an unrelated reason. Check the body, not just the status.

## MED-2: accepted-but-unverified write is not restored or tainted

`_default_zone_mask_fns` `post()` maps "refused by firmware (HTTP n)" to n,
an "ok" result to 200, and everything else to `None`. The tool can return
after the POST was accepted by firmware:

- `FAILED: read-back ...`
- `FAILED: ... collateral ...`
- `error: POST returned ok but re-fetch failed ...`

All map to `None`, so the case returns INCONCLUSIVE without calling restore
and without setting `ctx["_tainted"]`. The board may be left with relay 4 in
the zone mask while later cases run on an untainted run. Fix: treat any
result other than an explicit firmware refusal or a pre-POST refusal
(confirm/running/mode gate) as "possibly applied": taint and attempt restore.

## MED-3: dev_promote seam guard is a path-string check

`tools/dev_promote.ps1:45-47` sets `$foreignRepo` by comparing
`Resolve-Path -RepoPath` with the parent of the script's own directory as
strings. A worktree copy of `tools\dev_promote.ps1` run with
`-RepoPath <main tree>`, or any alternate spelling of the same repo (8.3 short
name, junction, subst drive), counts as "foreign", so `-PinCheckScript <stub>`
is accepted and `-Push` really pushes main. Fix: compare repository identity
(`git rev-parse --git-common-dir`, or origin URL equals the real remote), or
require a scratch-repo marker file the test fixture creates.

## LOW-MED: land.ps1 seam gate is one extra flag

`tools/land.ps1:192-196` refuses `-PinCheckScript`/`-ChecksScript` unless
`-AllowStandaloneClone`. Inside a linked worktree that flag is read only at
line 189 and has no other effect, so
`-AllowStandaloneClone -PinCheckScript stub` lands to the real origin/dev.
Mitigated: the verdict JSON records `submodule_pins = "pass(stub:...)"` and
`checks_script_override`. Consider applying the same repo-identity test as
MED-3 instead of a flag.

## LOWs

- Tool `ZONE_RELAY_MASK_MAX = 0xFFFF`; firmware parses the mask as u8. Values
  above 0xFF reach a firmware 400 instead of a local refusal. Clamp to 0xFF.
- `test_mcp_server_control_zone_relay_mask.py` mocks `build_post_body` to
  return `"body"`; replacing the strip call with identity is MISSED (negtest
  below). Add a test that a real zones GET body yields a POST body without the
  omit-preserved fields.
- `docs/BENCH_TEST_SYSTEM_PLAN.md:322` still says AX-C03 is "injected writer
  only ... no narrow tool exists | HTTP 400". Update to the tool and 409.
- CLAUDE.md: the count sentence moved to 232 but still says "as of
  2026-10-07".
- `dev_promote.ps1` OVERRIDE notice prints "pass(stub:...)" before the stub
  has run; say "override: stub" and report its exit code after.
- `test_bench_test_cases_aux.py::test_env_opt_in` fails when
  `KILNCTL_AUX_BENCH_CONFIRM=1` is set in the environment (it is on the bench
  PC). Pre-existing (0feb15855); clear the variable with `mock.patch.dict`.

## Network SKIP patterns: can an auth failure become a passing pin check?

`check_submodule_pins_pushed.ps1` exits 0 PASS / 1 FAIL / 3 SKIP. New
`netPattern` alternatives: `Connection (was )?reset`, `errno 10054`,
`schannel: (failed to receive handshake|SEC_E_)`, `SSL_connect`,
`returned error: 5\d\d`.

- The pattern applies only to `ls-remote` failures; a fetch failure is always
  FAIL.
- Any skip triggers the origin probe: origin answering, or failing for a
  non-network reason, turns SKIP into FAIL. SKIP survives only when origin
  also fails with a network pattern or times out.
- 401/403 are not matched; `Add-SshBatch` adds `-o BatchMode=yes` (ssh) /
  `-batch` (plink, tortoiseplink) so a credential prompt fails fast rather
  than hanging into a timeout. Wrappers are left untouched.
- Edge: `schannel: SEC_E_` / `SSL_connect` can match a persistent
  certificate-trust failure. If origin fails the same way the result is exit 3
  and land proceeds with a WARNING — but the push to the same origin then
  fails too, so nothing lands. Low risk.
- dev_promote does not refuse a pin exit 3 either; same reasoning applies.

Verdict: no false PASS path found.

The HEAD guard (`rev-parse --verify -q` plus 40-hex check) is correct.

## Tests run

All with the main-tree venv interpreter, `PYTHONPATH` at the worktree `src`,
link stubbed (no bench access).

- pytest `test_mcp_server_control_zone_relay_mask.py`,
  `test_bench_test_cases_aux.py`: 69 passed (with `KILNCTL_AUX_BENCH_CONFIRM`
  unset; see LOW).
- `tools/check_land.ps1`: exit 0.
- `tools/check_dev_promote.ps1`: exit 0.
- `tools/test_check_submodule_pins_pushed.ps1`: exit 0.

## Negative tests (`tools/negtest.ps1`)

pytest preset on the two Python test files:

- CAUGHT: `confirm_truthy`, `running_gate_removed`, `modegate_mapping_removed`,
  `readback_skipped`, `collateral_skipped`, `c03_no_taint`,
  `c03_accepted_as_none`, `c03_no_restore`, `c03_restore_wrong_mask`.
- `strip_removed` was CAUGHT only because the mutation called
  `build_post_body` twice and broke `assert_called_once_with`. Redone as an
  identity (`strip_identity`): **MISSED** (see LOW on strip coverage).

check preset on the PowerShell suites:

- `check_land.ps1`: `land_seam_gate_removed`, `land_pin_override_unrecorded`
  CAUGHT.
- `check_dev_promote.ps1`: `dp_foreign_gate_removed`,
  `dp_override_notice_removed` CAUGHT.
- `test_check_submodule_pins_pushed.ps1`: `plink_branch_removed`,
  `wrapper_gets_batchmode` CAUGHT; `schannel_net_removed` (drop the
  `schannel: (failed to receive handshake|SEC_E_)` alternative) **MISSED** —
  the new schannel pattern has no test. LOW: add a case for each new
  `netPattern` alternative, including one proving a certificate-trust failure
  with a reachable origin still FAILs.
