# REVIEW_UPDLOWFX: adversarial review of the UPDLOWFX batch

Scope: `b3cc27846..cc2477c10` on origin/dev, excluding `18f2d5446` and `8fa90166f`
(RECQR, reviewed separately) and `5e5180e4c` (bench log). The reviewed commits are
`11bc27c05`, `035ee6e8d`, `4796a70e9`, `9054e20bf` and `cc2477c10`. The board was not touched.

Result: HIGH 0, MED 0, LOW 4, INFO 10.

## Verified claims

- **No new internal RAM.** The batch adds no static or heap buffer in internal RAM.
  - `heap_admit` adds two locals: `hop_dma` and `hop_dma_ok`.
  - `update_settings_http.c` reuses the existing 257 B `body[]` array as scratch for the
    old repo. It never aliases `repo[]` (64 B): the two are separate arrays in the same
    frame.
  - Lifetime: the old repo is copied into `body[]` after the form fields are parsed out of
    it, and is compared before the function returns.
  - httpd runs single-threaded, so nothing races the compare.
  - The "8 B stack" figure is a source-level estimate and could not be measured. The two
    locals most likely live in registers (INFO-6).
- **Hop record.** `rtc_set` is latched by the first, non-hop `heap_admit(w, false)`, which
  `run_job` always calls before any hop. So every hop computes the DMA verdict with a real
  `rtc_free`, and records REFUSED from the combined internal and DMA verdict
  (`update_fetch.c:311-336`).
- **`update_fetch_heap.h` comments.** The comments are accurate: the first DMA check
  reduces to `pf[best] >= need`.
- **Backoff semantics.**
  - The backoff is cleared only on a case-insensitive repo change, or when the old repo
    cannot be read (`update_settings_http.c:115-127`).
  - The default repo canonicalises to `""`, but `update_settings_repo_copy` returns the
    default name for it. So "set the default explicitly while the default is stored"
    compares equal and does not clear the backoff. That is correct.
- **Release case RL-D01.**
  - The 4xx-is-definite reasoning holds. `ota_recovery_boot_run` answers 409 or 500, and
    `install_prepare` erases the request on every 4xx path. A 500 can leave a request
    behind, and the regex keeps 5xx as may-have-run.
  - "The recovery image has no `/api/status`" is true. Its routes are `/api/recovery/*`,
    `/api/partitions`, `/api/boot_guard` and so on, so the 404 probe distinguishes the
    two images correctly.
- **RELSUITEFX3 citation.** It now names `bbfa0712d`, which is on origin/dev and has the
  same content as `62377b873`. The claim is true.
- **Unattended GitHub update path.** The path is unchanged, apart from MCP text and the
  backoff semantics above. None of the board-side check, download, stage or install logic
  changed.
- **`check_internal_alloc_allowlist.ps1`.** Run in the worktree, it reports `PASS: 31 files`.
- **`check_kilnfw_dram_bss_budget.py`.** It uses `is not None` and FAILs when
  `.dram0.data` is missing. Both are correct.

## LOW

### LOW-1: an in-flight REFUSED makes RL-D01 clean up under a running install

Location: `tools/PcTools/src/kilnctrl/mcp_server_update.py:765` together with
`tools/PcTools/src/kilnctrl/bench_test/cases_release.py:883`.

The new single-flight answer starts with `REFUSED: another confirmed install call ... in
flight`. RL-D01 treats every `REFUSED` as "never sent" and returns `False`, which lets
cleanup run (`update_fetch_cancel`, then `update_stage_clear`).

Failure scenario:
1. An `update_install_start` job is running for the same host. That job takes no bench
   board lock and shares `_INSTALL_INFLIGHT` in the same server process.
2. RL-D01 runs during that job and gets the in-flight `REFUSED` answer.
3. RL-D01 clears the stage that the other call is about to POST or verify.
4. The other call then fails with "no verified stage", or, after its POST, reports against
   a stage that has been cleared.

Fix: return `True` (no cleanup) for the in-flight `REFUSED` prefix, or give it a distinct
prefix such as `BUSY:`.

### LOW-2: an unreadable install record answers "ok - nothing recorded"

Location: `tools/PcTools/src/kilnctrl/mcp_server_update.py:751-753`.

The plain status branch tests `bli0.get("state") != "recorded"`. A `last_install.state`
of `unreadable` (a corrupt NVS result record) therefore takes the
`ok - nothing staged and no install recorded` answer.

Failure scenario:
1. The MCP server restarts, so there is no pending record.
2. The result record is corrupt, for example from the AUTOFX2 INFO-1 double fault.
3. `update_install_staged(confirm=False)` reports `ok` and "no install recorded", when the
   board actually says it cannot read the record.

The docstring at `:872` is stale as well: it still says that case answers UNKNOWN
"cannot confirm".

No test covers `unreadable`. Fix: return UNKNOWN for `state == "unreadable"` and correct
the docstring.

### LOW-3: the 404 tail can call a successful install "a downgrade"

Location: `tools/PcTools/src/kilnctrl/mcp_server_update.py:838-841`.

After the poll loop ends with only 404s, the tool probes `_read_uptime()`. If that probe
answers, the text says the board runs "an application WITHOUT the install route (a
downgrade?)".

Failure scenario:
1. The default `wait_s` is 240 s (`:859`), and the bench application comes back at board
   uptime 231-235 s.
2. The recovery image keeps answering 404 until the last poll.
3. The new application then comes up between that last poll and the probe.
4. The probe answers, so a successful install is reported as a probable downgrade.

The probe also uses its own 5 s timeout (`:624`) and is not clamped to the deadline, so
the call can overrun `wait_s` by up to 5 s.

Fix: when the probe answers, re-read `/api/update/status` once before judging. If it
answers, use that record.

### LOW-4: test gap, a realistic 5xx is not pinned as may-have-run

Location: `tools/PcTools/tests/test_bench_test_cases_release.py:1072`.

The only 5xx in the parametrized may-have-run cases is the truncated string
`"FAILED: install refused or lost: HTTP 5"`. It does not match `HTTP \d\d\d` either, so
no test separates 4xx from 5xx.

The negtest mutation `rl_any_http_definite` (regex `HTTP 4\d\d` to `HTTP \d\d\d`) was
MISSED. That mutation would let RL-D01 clear the stage after a 500, which can leave an
install request behind.

Fix: use `"FAILED: install refused or lost: HTTP 500 ..."` in that case.

## Fix status (lowfx10)

- LOW-1 fixed: the in-flight answer now begins `BUSY:` and says nothing was sent; RL-D01 treats it as may-have-run and never clears the stage (`mcp_server_update.py`, `cases_release.py`).
- LOW-2 fixed: `last_install.state=="unreadable"` answers UNKNOWN; docstring updated.
- LOW-3 fixed: default `wait_s` is the 270 s cap (margin over the ~231-235 s bench return); the post-loop 404 tail does one re-read bounded by `_TAIL_PROBE_S` (2.5 s) and reports "a downgrade?" only when the install route stays absent.
- LOW-4 fixed: RL-D01 test now pins `HTTP 500`, `HTTP 503` and the BUSY string as may-have-run.
- Negtest (base `424aecbd4`, pytest preset): unreadable_branch_off, busy_back_to_refused, default_wait_240, tail_reread_off, tail_probe_unbounded, downgrade_sentinel_off, rld01_5xx_treated_definite (the reviewer's MISSED `rl_any_http_definite` class) and rld01_busy_treated_never_sent all CAUGHT at test assertions.
- INFO items deferred, not touched.

## INFO

- **INFO-1: hop path without `rtc_set`.**
  - Location: `update_fetch.c:311-336`.
  - If a hop ever ran with `rtc_set == false`, `hop_dma_ok` would stay `true` and a DMA
    refusal would no longer be recorded. (The removed post-hoc overwrite used to cover
    that case.)
  - The path is unreachable today, because `run_job` admits first.
- **INFO-2: settle log not described.**
  - Location: `update_fetch.c:304`.
  - The comment on pair 0's sample window omits the `heap settle` ESP_LOGI, which runs
    inside that window when `tries > 0`.
- **INFO-3: duplicate fail key stays UNKNOWN.**
  - The `before_fail` guard keys on `(code, reason, id)`.
  - A genuine repeat failure with an identical key, after a post-unknown POST, stays
    UNKNOWN until the deadline. This is the safe direction.
- **INFO-4: single-flight key aliasing.**
  - `_INSTALL_INFLIGHT` is keyed on the resolved host string.
  - A hostname and its IP are different keys, so two calls that address the same board
    by different names are not serialised.
- **INFO-5: race with a concurrent `confirm=False` call.**
  - A concurrent `confirm=False` call can pop `_PENDING_INSTALL` between the POST and the
    exception wrapper.
  - The wrapper then reports the raw `error:` text instead of UNKNOWN.
  - This needs two concurrent calls on the same host.
- **INFO-6: stack claim unmeasured.** The "0 B static, 8 B stack" statement for the hop
  DMA record is plausible but unmeasured.
- **INFO-7: allowlist count overstated.**
  - Location: `docs/audits/REVIEW_IRAMGATE.md:164`.
  - The text cites "76 existing ungated `MALLOC_CAP_INTERNAL` uses".
  - A grep finds 77 `MALLOC_CAP_INTERNAL` lines in total, but only about 35 are
    `heap_caps_*` or `*WithCaps` allocation calls.
  - IRAMGATE LOW-1 therefore stays only partly fixed, as that doc says.
- **INFO-8: repo change via backup import keeps the backoff.**
  - `backup_import.c` changes the repo without clearing the GitHub backoff.
  - This predates the batch and is conservative.
  - An A-to-B-to-A flip through the settings route does clear the backoff, which is by
    design: GitHub's anonymous limit is per IP, not per repo.
- **INFO-9: ceiling of 0 not reachable from the wrapper.**
  - The DRAM budget ps1 wrapper passes a ceiling only when it is greater than 0.
  - A ceiling of 0 cannot be requested through the wrapper.
- **INFO-10: stale docstring and test style.**
  - The `update_install_start` docstring (`mcp_server_update.py:908`) still says a 404
    means the recovery image is applying. Since the LOW-3 probe was added, it can also
    mean an application without the route.
  - One test uses a one-line compound `with` that is hard to read.

## Negtests (spot-check, `tools\negtest.ps1`, base `cc2477c10`)

Firmware runs used `build_host_tests.ps1 -Only "test_update_fetch\.c|test_update_http_refusals\.c"`
with the kilnfw-host ExpectPattern. The baseline passed. Every mutant compiled: there are
no BUILD FAILURES in any log. The negtest reported `real_tree_unchanged: true`.

| Mutation | Verdict | Assertion |
|---|---|---|
| `hop_dma_not_recorded` | CAUGHT | `test_update_fetch.c:413` "a hop refused on DMA is reported refused" |
| `settings_case_sensitive` | CAUGHT | `test_update_http_refusals.c:192` "case-only repo change leaves the backoff in place" |
| `settings_never_clears` | CAUGHT | `test_update_http_refusals.c:180`, `:195` |
| `settings_prefix_equal` | MISSED | no test with a prefix-equal repo (INFO; `repo_same_ci` checks both terminators) |
| `start_dma_as_hop` | CAUGHT | `test_update_fetch.c:926` "a START DMA refusal is not recorded as a hop refusal" |

Pytest runs used preset `pytest` on `test_mcp_server_update_install.py` and
`test_bench_test_cases_release.py`. The baseline passed and the negtest reported
`real_tree_unchanged: true`.

| Mutation | Verdict | Failing test |
|---|---|---|
| `inflight_never_added` | CAUGHT | `test_marker_is_held_during_the_call_so_a_concurrent_call_is_refused` |
| `before_fail_ignored` | CAUGHT | `test_stale_failure_record_does_not_end_a_lost_post_install` |
| `probe_404_off` | CAUGHT | `test_404_while_app_status_answers_is_an_app_without_the_install_route` |
| `rl_any_http_definite` | MISSED | LOW-4 |

The batch's own recorded negtest CAUGHT claims are consistent with these results.
