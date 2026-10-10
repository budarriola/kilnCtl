# Review: firefx3 and toolfx2 (2026-10-10)

Opus review of origin/dev at `f740f6d1a`. This review changes no code.

- **firefx3:** `3c57e1d54`, with docs `962229a3e` and `f1d9d2567`. It fixes the findings in `REVIEW_FIRING_FIX2_2026-10-10.md`.
- **toolfx2:** `1d54d0730`, `e5936650a`, `c43e0a0f2`, `3d9486bbf` and `f740f6d1a`.

## Verdict

Both batches are sound. There is one LOW finding in firefx3 and one MEDIUM finding in the toolfx2 negtest orphan reaping. Three firefx3 behaviours have no test (negtest MISSED). No caller breaks now that `push_verify -Commit` is required.

## Findings

### A-LOW-1: the running pending-OFF retry can cut a relay owned by a relay/IO segment

`zone_off_pending_retry_running()` is in `profile_executor_relay_io.c` (around line 553). It builds `owned` from three sources:

- the relay masks of active zones;
- the relay masks of autotune-active zones;
- `aux_claim_mask`.

Relays owned by a relay/IO segment are missing from that list. `io_seg_start()` claims such a relay as `RELAY_OWNER_PROFILE` and writes it once, at segment start. It is in no zone mask and not in `aux_claim_mask`. The per-bit owner check passes `RELAY_OWNER_PROFILE`, so it does not protect the relay.

How it happens:

1. Relay R still has a pending OFF bit from an earlier run. Bits now survive run start on purpose. This needs a sustained write failure, or a zone relay mask edited between runs.
2. In the new run, an IO segment turns R ON.
3. Within 1 s the retry writes R OFF.
4. `io_segs_tick()` never rewrites R, so it stays OFF for the rest of the segment.

This breaks the commit's claim that the retry "never writes to a relay owned by ... the run". The error is in the safe direction (OFF), so there is no heat hazard. The result is a silent wrong output, for example a vent or fan that does not run.

The F4 fallback in `apply_relay()` (around line 47) has the same gap, and it predates firefx3. Its `fb` is built from `claimed_relay_mask`, which also contains IO-segment bits.

**Fix:** OR into `owned` the relay bit of every `s_exec.io_segs[i]` with `active && is_relay`. Exclude the same bits from the F4 fallback. Add a test with an active IO-segment relay plus a pending bit.

### A-INFO

- **Lock order:** unchanged. The retry runs under `s_exec.lock` and takes zones_cfg (`zones_config_get_relay_mask`) and the autotune lock (`autotune_engine_is_active_on_zone`) in the same order `sweep_unowned_relays()` already documents. The executor task makes no direct I2C call. The write goes to the kiln_io owner queue with a bounded wait (`KILN_IO_OWNER_WAIT_MS`, 200 ms). While the expander keeps failing, that adds at most one bounded wait per second.
- **Races:** none found.
  - Ownership is computed and the OFF write issued under the same lock.
  - `system_mode_gate` refuses mid-run writes to zones, aux outputs and aux-convert.
  - A zone can only go inactive mid-run, never become active.
  - An autotune started mid-run is covered by the `AUTOTUNE` owner check.
  - A live profile edit does not change zone relay masks.
- **Pending bits are not lost or stuck:**
  - A bit owned by an active zone is cleared when `apply_relay()` writes successfully.
  - A bit owned by MANUAL, RULE or AUTOTUNE is dropped and forgotten.
  - A failed write keeps the bits.
  - An unreadable zone mask writes nothing this cycle.
- **Two observations, neither a defect:**
  - A relay with owner NONE that someone turned on by hand while a stale pending bit still exists can be driven OFF. The non-RUNNING retry already behaves this way.
  - The running retry excludes the whole `aux_claim_mask`. The sweep excludes only `aux_claim_mask & enabled_mask`, so the retry is the more conservative of the two.
- **`zone_off_pending_retry_seen`:** never reset across runs. This is harmless; at worst the first retry of a run comes up to 1 s early or late.
- **`send_enable` `reboot_hold` fix:** correct. If the enable lands (`ESP_OK`) while a hold was set in flight, it drops `granted`/`pending`, queues `release_pending` and counts the send. In the failure branch under a hold, `pending = true` is set but has no effect: reconcile is skipped while the hold stands, and the last-out release clears it.
- **Guard 9 reorder:** correct. `guard9_merge_pending()` and `guard9_assert_stale_tick_fault()` now run before `relay_unknown_release_locked()`. The assert ORs APP into `global_fault_source`, so the release can no longer drop the APP fault source.
- **Run-start clear removed:** consistent with keeping pending bits across runs. This removal is what makes A-LOW-1 reachable.

### B-MEDIUM-1: negtest orphan reaping can kill unrelated processes

**FIXED in 46afbf427:** children adopted only if created at or after their parent (`Test-ChildAdoptable`), no `taskkill /T`, spare names on every kill; test in `check_negtest.ps1`, 3 negtest mutations CAUGHT.

`tools/negtest.ps1` builds the tracked tree in `Add-Descendants` (around line 391) and kills it in `Stop-Tracked` (around line 406).

`Add-Descendants` builds the tree from `Win32_Process.ParentProcessId` every 1.5 s. It never checks that a child was created at or after its parent. Windows does not update `ParentProcessId` when a parent exits, and PIDs are reused quickly. That allows two kinds of false descendant:

- **An unrelated process inherits a tracked PID.** A short-lived tracked process exits and its PID is reused by an unrelated process. That process's children are then recorded as descendants.
- **A tracked process inherits a stale parent PID.** A long-lived orphan, such as an MCP server whose launcher exited or another session's build, has a parent PID that a short-lived negtest process now holds. The orphan is then recorded as that process's child.

`Stop-Tracked` re-checks the creation time of the tracked PID itself, which guards against reuse of that PID. It does not guard against wrong recording in the first place. It then runs `taskkill /T /F`. `/T` also kills whatever that process has as children at kill time, including any child whose name is in `SpareNames`; the spare list is honoured only for the tracked PID itself. Any check preset that deliberately starts a shared, long-lived service would also have it killed.

This repo runs many concurrent sessions and long-lived MCP servers on one machine, so this is a real way to kill another session's process.

**Fix:**

- Record a child only if its `CreationDate` is at or after its parent's.
- Drop `/T` in `Stop-Tracked`: every descendant is already tracked and killed one by one.
- Re-apply `SpareNames` to every kill.

### B-LOW / INFO

**FIXED in 46afbf427:** `-RequireAssertion` now matches `  FAIL file:line:` (test in `check_negtest.ps1`); `push_verify.ps1` temp-file delete retries.

- **`push_verify.ps1` (P1-P3) is correct.**
  - An empty `-Commit` exits 2.
  - `-FetchTimeoutSec <= 0` exits 2.
  - The fetch uses an explicit `+refs/heads/X:refs/remotes/R/X` refspec with `--no-tags`.
  - On timeout it kills the fetch tree with `taskkill /T`.
  - Minor: if a killed child still holds a temp output file, the `finally` delete can fail silently and leave the file behind.
- **`main_baseline_lib.ps1` B1/B2 is correct.**
  - B1: the `--format=%(objectname)` argument is quoted.
  - B2a: `mbDev` is dropped when it is an ancestor of `mb`.
  - B2b: exact baselines are ranked HEAD tree 0, dev 1, other 2, with a deterministic tie-break.
  - All three are covered (negtest table).
- **`check_submodule_pins_pushed.ps1`:** the `RepoPath` default is now resolved from `$MyInvocation.MyCommand.Path` in the script body. Correct.
- **negtest N1-N3 are correct.**
  - The saftyfw preset pattern is narrowed to `SAFTYFW HOST TESTS: FAILED`.
  - `-RequireAssertion` is refused when combined with `-ExpectPattern` or a preset.
  - INFO: `-RequireAssertion` is documented as JS-only, but nothing enforces that. The KilnFW host-test framework prints `  FAIL file:line: msg` (`test_common.h:124`), which the case-sensitive `\bFAIL:` never matches. A `-RequireAssertion` run over `build_host_tests.ps1` therefore reports MISSED for every mutation, even ones the tests catch. This review's first run (below) showed exactly that. Consider adding the C form `(?m)^\s+FAIL \S+:\d+:`, or a warning when the command names `build_host_tests`.

## Test gaps (firefx3)

None of these mutations makes `test_profile_executor_prestart`/`test_heat_enable` fail:

- the running retry's per-bit owner check (R1);
- the unreadable-mask early return (R2);
- the autotune-zone exclusion (R4).

Each is a single-line removal that would ship green. Add cases where:

- a pending bit is owned by RULE or AUTOTUNE and is expected to be forgotten, not written;
- `zones_config_get_relay_mask` fails and no write is expected;
- an autotune-active zone's relay is pending and no write is expected.

## Negtest results

Each run used `tools\negtest.ps1`, one throwaway worktree per mutation, and a fresh output directory. The baseline passed every time; `real_tree_unchanged` and `copies_removed` were both true.

Firmware command: `build_host_tests.ps1 -OutDir {OUT} -Only "profile_executor_prestart|heat_enable"`, matched with `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.

| Mutation | Verdict |
|---|---|
| R1: running retry owner check disabled | MISSED |
| R2: unreadable zone mask `return` changed to `continue` | MISSED |
| R3: `aux_claim_mask` removed from `owned` | CAUGHT |
| R4: autotune-zone exclusion removed | MISSED |
| R5: 1 s cadence disabled | CAUGHT |
| H1: `send_enable` reboot_hold branch disabled | CAUGHT |
| B1: `--format=%(objectname)` unquoted (`check_main_baseline.ps1`) | CAUGHT |
| B2a: `mbDev` ancestor drop removed | CAUGHT |
| B2b: dev-exact rank equal to other | CAUGHT |

A first run of the same firmware mutations with `-RequireAssertion` reported all six MISSED, including R3, R5 and H1. That is the pattern artifact described under B-LOW/INFO, not a test gap.

## push_verify caller sweep (`-Commit` now required)

| Caller | Status |
|---|---|
| `tools/land.ps1:362` | Passes `-Commit $script:sha -Branch origin/$Target`. Fine. |
| `tools/check_push_verify.ps1` | The test itself. Updated by toolfx2. |
| `tools/check_worktree_mint.ps1` | Only regex-checks the `-Branch` default. Unaffected. |
| `tools/worktree_mint.ps1` | Mentions push_verify in prose only. |
| `tools/commit_guard.ps1` | Comment only. |
| `tools/dev_promote.ps1` | No push_verify call. |
| CLAUDE.md, `docs/agent_rules/COMMON.md`, `docs/MCP_SERVERS.md` | Prose. MCP_SERVERS.md already shows `-Commit` as required. |

Nothing breaks.
