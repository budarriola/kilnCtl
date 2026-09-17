# Verification pass: RELEASE_HARDENING_PLAN blockers 1, 2, 5, 7, 8

Scope: re-verify these five blockers against `origin/main` (bfc8e8a3) by
reading code, tests and git history, not by trusting the plan's own prose.
Blockers 3, 4 and 6 are owned by other sessions and are out of scope here.
No board was touched; this is a read-only pass done in a worktree.

## Headline finding: two more items were already done when the plan was written

Same pattern as blocker 6's `boot_guard_reset_counter()` phantom: the plan's
item 1.1 (coredump readback) and part of item 2 (a soak verdict/exit code)
describe gaps that were already closed, or in item 2's case were closed the
same day the plan shipped. Both are corrected below with commit evidence.

## Blocker 1 — profile_executor panic / coredump readback

**Item 1.1 (coredump readback) — DONE, `4af518ca`.** `read_esp_coredump()`/
`symbolize_coredump()` (`tools/PcTools/src/kilnctrl/coredump_fetch.py`,
`mcp_server_flash.py`) previously invoked `espcoredump` with the dump path as
a bare positional instead of `--core`/`--core-format`, with no `--chip`, and
defaulted to `sys.executable` (the MCP server's own venv, which lacks
`esp_coredump`) — the resulting `ModuleNotFoundError` was misreported as
"very likely an ELF/coredump mismatch," an environment failure dressed as a
substantive verdict, exactly as flagged in the task brief. Fixed and
confirmed live against the board's real stored 748032-byte coredump: with the
fix, `espcoredump` now runs far enough to report a genuine SHA256 mismatch
verdict. Also adds a durable, content-addressed coredump archive
(`archive_coredump()`, `firmware/KilnFW/coredump_archive/`, a sibling of
`build/` for the same durability reason as `elf_archive/`) with a provenance
sidecar (host, `fw_build` at fetch time, fetched-at, sha256), wired into
`read_esp_coredump()`. Test coverage: `tools/PcTools/tests/
test_coredump_symbolize_and_archive.py` (228 new lines, added in the same
commit).

The existing 748032-byte on-board dump remains permanently unsymbolizable:
its matching ELF (build `Sep 14 2026 23:55:17Z`) was lost before the
elf-archive durability fix (`a347e726`, 2026-09-15) landed, and nothing in
`4af518ca` recovers lost ELFs. This is stated plainly in the commit message,
not hidden.

**Residual gap, confirmed real and still open:** `find_crash_elf()`
(`mcp_server_flash.py:970`) resolves the ELF to symbolize against by querying
the board's *currently running* `fw_build` (`GET /api/status` via
`capability_preflight.get_board_info()`), not the build that produced the
*stored* coredump. Its own docstring only promises to match "the ESP's
CURRENTLY RUNNING firmware." A coredump fetched from a board that has since
been reflashed (the exact situation the 2026-09-15 loss illustrates) will be
matched against the wrong ELF unless the caller separately supplies the
dump's own `fw_build` by hand via the `fw_build=` parameter. Nothing in
`4af518ca` closes this; it is not mentioned in that commit's message.

**Items 1.2 (in-firing stack margin) and 1.3 (two clean firings with a
confirmed-absent crash report) — genuinely outstanding, no new evidence
found.** No stability_soak-adjacent stack capture during an active firing,
and no crash-report-checked clean-firing pair, appears anywhere in
`docs/audits/` or in a CSV/log under the repo as of this pass.

## Blocker 2 — long-duration soak with a verdict

**Partially done, more than the plan credits.** `tools/PcTools/scripts/
stability_soak.py` already has a real PASS/FAIL verdict and a non-zero exit
code (`main()` returns 1 on any accumulated problem, 2 on a precondition
failure, 0 on PASS; `sys.exit(main())`), not merely a CSV as the plan's
"How to know it is closed" section states. It already flags: a heap-floor
breach (`HEAP_INTERNAL_FLOOR_BYTES`), a DOWN trend on heap-free or on
stack-min-headroom%, and a positive delta on any of `crc_errors`/`timeouts`/
`broadcast_dropped` (the flat-counter check the plan itself endorses).
Landed across `95ca7e6e` (verdict + script), `e84a2db5`, `3780030f`.

**What is still genuinely missing, confirmed by reading the code:**

1. **The trend test is not a statistically significant slope test.**
   `_trend_direction()` (line 279) is explicitly documented as "very small
   trend classifier: last value vs. first" — a two-point comparison with a
   5%-of-first-value threshold (`TREND_MIN_SAMPLES = 5` just gates on having
   ≥5 rows, it does not fit a slope). Noisy middle samples are invisible to
   it. The plan's ask — "a statistically significant downward slope is a
   failure even if no bound is breached" — is not implemented.
2. **The harness has never been negative-tested.** `grep` across
   `tools/PcTools/tests/*.py` for `stability_soak` or `_trend_direction`
   finds nothing, and `git log` on the script shows only three commits, none
   test-only. No synthetic degrading series has been fed through it to
   confirm it goes RED — the exact standing requirement (`feedback_negative_test_every_check.md`)
   this repo has been burned by eight times before.
3. **No run of the intended release duration exists.** No
   `stability_soak_*.csv` file exists anywhere in the tree, and no audit
   references a completed run. The "harness exists but has never been run
   for the release duration, with or without a firing" gap the plan
   describes is accurate and unchanged.

Net: the plan should stop asking for "extend with a verdict" (done) and ask
instead for a real slope test, a negative test of the harness, and the actual
run.

## Blocker 5 — failure injection and recovery

**Power-loss-mid-write item: the "flagged and untouched" claim is stale —
the defect is fixed, and partially bench-verified.** The plan states the
`config_store` `next_write_slot` torn-slot reprogramming defect is "flagged
and untouched." `docs/CONFIG_FILESYSTEM.md` (read in full) instead documents
a fix landed 2026-09-14: `config_store_next_write_slot()` previously trusted
`latest_valid + 1` was still erased; a slot torn by a power cut mid-program
failed its own CRC (correctly skipped by the boot scan) but stayed
non-erased, so the next write silently AND-corrupted onto it while
`hal_flash_program()` still reported `HAL_OK`. Fixed by (a) verifying the
target slot is actually erased before programming, switching sectors and
erasing if not, and (b) a read-back verify after every program. Full detail
and host-test coverage (torn-slot-reuse, ordinary and never-committed cases):
`docs/audits/rp2040_config_store_write_atomicity_2026-09-14.md`.

**Bench-verification is real but partial, and the plan's specific ask
("prove it by cutting power during a write, repeatedly") is not met.** The
2026-09-14 bench pass (`88bb4333`) flashed the fix, confirmed a benign
commissioning field round-trips correctly across two real
`debug_reset(peer="pico")` reboots, and confirmed all safety-relevant config
(`abs_max_temp_c`, S8 rate guard, `tc_type`, CT cal) read back unchanged.
It explicitly did **not** attempt to tear a real flash program mid-write —
judged unsafe/irreversible on the only bench Pico. So (b)'s actual
mismatch-detected branch and (a)'s slot-skip-and-switch behavior for a
genuinely torn slot remain proven only by the host-test power-cut-injection
harness (`test_config_store_flash.c`), never on real hardware. The plan's
literal ask (repeated real power cuts during a write, confirmed intact
read-back every time) is therefore still open, by deliberate safety choice
rather than oversight — this should be stated as a bench-verification
tradeoff, not left as "untouched."

**Everything else in this blocker: no new evidence, plan text is accurate.**
- Pico reboot mid-firing: the general exercise (reboot the Pico during a live
  firing, confirm ESP blocks heat / recovers link / re-establishes dedup) has
  no script and no audit record. `grep` for `pico_reboot` or similar finds
  nothing outside the plan itself. Genuinely outstanding.
- Dead-link 30 s firing-abort: `tools/PcTools/scripts/
  bench_firing_abort_stopwatch.py` exists (confirmed read) and is
  purpose-built for this, but no audit or log shows it has ever been run
  against a live firing — only referenced in pre-existing planning docs
  (`one_firing_bench_plan_2026-09-07.md`, `session_summary_2026-09-07.md`),
  not execution records. Genuinely outstanding, cheap, as the plan says.
- Thermocouple fault on the safety-processor side, welded contactor: no new
  evidence found; plan text stands.

## Blocker 7 — config migration and hostile-input coverage

**"Migration test that runs every historical schema version forward" —
confirmed genuinely outstanding, and the code shows it was a deliberate
narrowing, not an oversight.** `ZONES_CFG_VERSION` is 26
(`zones_config_json.h`). `test_zones_http.c`'s CRC-verification-before-migrate
fix (Opus review finding 10, 2026-09-10) is explicit in its own comment: it
verifies the immediately-prior version's CRC "narrowly, for
ZONES_CFG_VERSION-1 only, not every historical version (see that fix's own
comment for why)." There is no corpus of v1-through-v26 real persisted blobs
run forward to current anywhere in the test suite. This item stands exactly
as the plan describes it.

**"Import of a deliberately hostile config" — substantially more coverage
already exists than the plan credits, though not a single end-to-end system
test.** `firmware/KilnFW/App/test/test_backup_import.c` already covers:
malformed/truncated body (`test_malformed_body_writes_nothing`), wrong `kind`
(`test_wrong_kind_refused`), a version newer than this firmware understands
(`test_unknown_version_refused`), out-of-range model values
(`test_out_of_range_model_rejected`), overlong names
(`test_overlong_zone_name_rejected`, `test_overlong_profile_name_rejected`),
and several structural-invariant attacks (self-referencing coupling/settings
cycles, `test_settings_source_cross_entry_cycle_rejected_before_any_commit`).
Separately, at the NVS-blob level (not the JSON backup path), bad-CRC and
refused-newer-than-firmware blobs are covered and preserved un-overwritten:
`test_nvs_load_from_bad_crc_is_rejected`,
`test_decode_zones_blob_refuses_a_v25_blob_with_a_corrupted_crc`,
`test_relay_names_load_bad_crc_is_rejected`,
`test_zones_http_start_refused_newer_blob_not_overwritten`. What remains
missing, narrowly: no single test states the release-gate property directly
("no hostile input of any of these shapes can produce a bootable state that
commands heat") as one assertion rather than as inference from several
separate unit tests, and the schema-downgrade-runs-on-defaults hazard is
still open — but that is tracked under blocker 6, not this one, and blocker 6
is out of my scope. Recommend narrowing this bullet's size from M to S given
how much groundwork already exists, rather than removing it.

## Blocker 8 — first-boot / first-firing checklist

**Confirmed genuinely outstanding, no phantom.** `find docs -iname
"*FIRST_FIRING*"` and `*FIRST*CHECKLIST*` return nothing.
`docs/SETUP_WIZARD.md` does not mention a first-boot or first-firing gated
stage. No document anywhere states, in order, what an operator does between
mounting the controller and leaving a firing unattended. Plan text is
accurate as written; no update needed beyond what is reflected in the plan
edit accompanying this audit.

## Summary table

| Item | Plan's claim | Verified state | Evidence |
|---|---|---|---|
| 1.1 coredump readback | open | **DONE** | `4af518ca` |
| 1.1 residual: ELF keyed to running build | (not previously named) | **confirmed real gap** | `mcp_server_flash.py:970-1005` |
| 1.2/1.3 in-firing stack + clean firings | open | open, unchanged | no evidence found |
| 2 soak verdict/exit code | "does not exist... only a CSV" | **mostly DONE** | `95ca7e6e`, `e84a2db5`, `3780030f` |
| 2 statistically-significant slope test | (implied needed) | **still open** | `_trend_direction()` is first-vs-last |
| 2 harness negative-tested | (implied needed) | **still open** | no test file references it |
| 2 full-duration run | open | open, unchanged | no CSV artifact anywhere |
| 5 config_store torn-slot defect | "flagged and untouched" | **fixed**, host-tested, partially bench-verified | `docs/CONFIG_FILESYSTEM.md`, `rp2040_config_store_write_atomicity_2026-09-14.md`, `88bb4333` |
| 5 repeated real power-cut proof | open | open (deliberately not attempted, safety) | same audit, §8 |
| 5 Pico reboot mid-firing | open | open, unchanged | no evidence found |
| 5 dead-link abort exercise | open | open, unchanged | script exists, unrun |
| 7 migration-forward corpus | open | open, confirmed by design comment | `test_zones_http.c` CRC-narrowing comment |
| 7 hostile-config import | open, size M | **substantially covered**, size S remains | `test_backup_import.c`, `test_zones_http.c` |
| 8 first-firing checklist | open | open, unchanged | file does not exist |
