# Stack-fix batch review (2026-10-09)

Scope: the stack-fix batch on origin/dev, `650f6159e` (analyser review fixes
F2, F3, F5-F8), `794fce57b` (`profiles_http.c` boot-path `profile_t` scratch
to the heap), `6ecf90f6b` (ceiling re-baselines), plus docs `cd9bcac2f` and
`617bcb350`. Findings only; nothing was fixed here.

Measurements used:

- dev tip ELF at `91eb1f0ce`, from the target-build check,
- origin/dev tip ELF at `b4f054492`, from a second target-build check run,
- an older checkbuild ELF at `103b7649d` (2026-10-09 21:01, clean tree apart
  from 4 untracked files). That build comes after the ceilings were last set
  in `0a1cdc7dc` (19:16) and before the batch.

## Answers to the four questions

### Q1. Are the code changes correct?

`794fce57b` (`profiles_http.c`):

- **`nvs_load_files_only`** allocates `2 * sizeof(profile_t)` after the
  non-profiles-partition early return.
  - On NULL it marks every slot `s_profile_rev_unknown = true`, with the rev
    set to the floor, and returns `ESP_ERR_NO_MEM`.
  - Otherwise it frees after the loop, which has no early exit, and before
    `rev_repair_junk`.
  - Free paths are correct. An OOM is an error, never "absent".
- **`nvs_load_all_from`** allocates `sizeof(profile_t)` for the profiles
  partition only.
  - On NULL it sets `any_resolve_err` and every `slot_res_err[]`, and returns
    `ESP_ERR_NO_MEM`.
  - The loop is guarded by `resolved_p &&`, and `free()` runs on both paths
    (`free(NULL)` is fine).
  - Correct.
- **Locks and lifetime:** none involved. Both functions run single-threaded
  at boot inside the `profiles_boot_load` generation bracket, and the
  scratch never escapes.

`650f6159e` (analyser) is correct for what it claims, with the soundness gap
in finding L1 below. `6ecf90f6b` changes ceilings only.

### Q2. Are the four check_all ceiling re-baselines justified?

Yes. Each matches a real measurement, and the growth is code growth, not an
analyser change. The commit gives no cause, and the doc's explanation is
wrong (finding M1).

Method:

- Run `check_all_task_stack_budgets.py --dump-ceilings` on the same dev-tip
  ELF using three analyser versions: the `0a1cdc7dc` copy (which set the old
  values), the `c094c089a` copy, and the dev-tip copy.
- All three print identical numbers: 6160 / 7632 / 7488 / 3904. So no
  analyser change accounts for any of the growth.
- The dev-tip analyser on the `103b7649d` ELF prints exactly the old
  ceilings: 6144 / 7552 / 7472 / 3888.
- Per-path diff between the two ELFs, and an `entry a1, N` frame diff:

| Task | Old -> new | What grew |
|---|---|---|
| http_async_job | 7552 -> 7632 (+80) | New 80 B frame `backup_import_apply_body$constprop$0`, now between `backup_import_job` and `backup_import_apply_two_pass`. It is declared `BACKUP_IMPORT_NOINLINE` and was introduced by `3cb3fb94a` (backup format v7). Deliberate, known. |
| bx_flash_worker | 6144 -> 6160 (+16) | Via the `profiles_handle_message` dispatch root, 5408 -> 5424: `readiness_gate_collect` frame 256 -> 272. `readiness_gate.c` itself is unchanged in the range. Likely a header struct growth (see below). |
| screen_idle | 3888 -> 3904 (+16) | `screen_idle_refresh_inputs` frame 0x450 -> 0x460. It holds a `dashboard_status_t ds` local. `screen_idle.c` is unchanged in the range. |
| lvgl | 7472 -> 7488 (+16) | Via the `refresh_cb` dispatch root, 5456 -> 5472. Five `refresh_cb` definitions exist, and the ambiguous name was not pinned to one. The UI refresh callbacks hold `dashboard_status_t` locals. |

Likely cause of the three +16 B rows:

- `937af9cc7` grew `dashboard_status_t.kiln_cfg_swap_boot_fault_kind` from
  24 to 32 B, in `dashboard_http.h`. That header is included by both
  `readiness_gate.c` and `screen_idle.c`.
- `d06bf7841` changed `crash_report.h`, which `readiness_gate.c` also
  includes.
- 8 B rounds up to a 16 B frame step. This attribution is inferred from the
  frame diff and the include graph; no bisection build was done.

None of the four hides a regression. All are small, explained by deliberate
changes, and still leave 22-37 % honest free.

### Q3. Is the KilnFW target build broken at `backup_import.c:3659`?

**No.** It was fixed by `2a1d69a84`, which shortened the zone-topology
refusal to fit `err_msg` and added `#include "cfg_fs.h"` to `main.c`.
`2a1d69a84` is an ancestor of origin/dev.

| Commit | `run_all_checks.ps1 -Only check_00_kilnfw_target_build -NoCache` |
|---|---|
| `91eb1f0ce` | 1 passed, 0 failed, NEW 0 |
| `b4f054492` (current origin/dev tip) | 1 passed, 0 failed, NEW 0 |

### Q4. Were negative tests done?

| Change | Negative test |
|---|---|
| `650f6159e` F7 (legacy long-call edge rule) | Negtested. Recorded CAUGHT in `DEV_STACK_ANALYSER_REVIEW_2026-10-09.md`. |
| `650f6159e` F2, F3, F5, F6, F8 | Unit tests added, but **no recorded negtest**. |
| `794fce57b` OOM branches | **No host test and no negtest.** Existing profiles OOM tests inject other sizes (`sizeof(probe)`, `sizeof(struct rev_repair_scratch)`), never `sizeof(profile_t)` or `2 * sizeof(profile_t)`. Negtest run in this review: both mutations MISSED. |
| `6ecf90f6b` | Ceiling values only; nothing to mutate. |

## Coordinator follow-up: every stack-budget check at origin/dev tip

ELF: target build of `b4f054492` (checkbuild tree content equal to
`b4f054492`). Each checker was run directly against that ELF.

| Check | Result | Measured vs limit |
|---|---|---|
| `check_main_task_stack_budget` | PASS | 5568 B vs budget 6144 B |
| `check_executor_task_stack_budget` | PASS (LOW headroom) | 3408 B vs ceiling 3408 B; honest free 1516 B |
| `check_httpd_task_stack_budget` | PASS (LOW headroom) | `profile_exec_start_post_handler` 4448 B vs ceiling 4448 B; honest free 1944 B |
| `check_system_uart_bridge_stack_budget` | PASS | 3136 B |
| `check_uart_log_bridge_stack_budget` | PASS | 2080 B |
| `check_all_task_stack_budgets` | PASS, exit 0 | 33 of 33 INDETERMINATE (lower bounds); bx_flash_worker 6160/6160, http_async_job 7632/7632, lvgl 7488/7488, screen_idle 3904/3904 |
| `check_kilnfw_dram_bss_budget` | PASS | `.dram0.bss` 92360 B vs 101000 B |

The fwbatch12 failure report does not reproduce at the tip. Its numbers
match the code and tables from before this batch:

- main 6464 is the pre-fix figure quoted in the analyser review.
- executor 3968 > 3920 and httpd 4512 > 4496 are checked against ceilings
  that `6ecf90f6b` replaced.

So that tree had an ELF and/or check scripts older than `650f6159e` /
`794fce57b` / `6ecf90f6b`, even though `b4f054492` itself contains them. The
most likely cause is a stale `build/` ELF or an unrebased working copy.

## Findings, severity ranked

**M1 (MED): ceiling re-baseline commit names no cause, and the review doc
gives a false one.**

- `6ecf90f6b` says only "re-measured on dev tip".
- `DEV_STACK_ANALYSER_REVIEW_2026-10-09.md` says the ceilings drifted "with
  no analyser change". `c094c089a` did change `stack_budget_lib`, after
  `0a1cdc7dc` set those values.
- The measurement above shows the growth is code anyway: `3cb3fb94a` (+80),
  and likely `937af9cc7` for the +16 B rows.
- The process gap is real: four ceilings were raised to whatever the walk
  printed, without naming what grew.
- Stale comment: the `bx_flash_worker` note still says
  "3840 = measured 3808 + 32".

**M2 (MED): the `794fce57b` OOM branches are untested and un-negtested.**

- Both new allocation-failure paths decide fail-closed behaviour: slots
  marked rev-unknown, saves refused.
- No host test injects `sizeof(profile_t)` or `2 * sizeof(profile_t)`.
  `KILNCTL_PERSIST_SCRATCH_TEST_HOOK` already supports this.
- A mutation that returns `ESP_OK` without marking slots unknown would go
  unnoticed. Confirmed: both mutations below were MISSED.

**L1 (LOW): non-entry long-call literals are dropped silently (`650f6159e`
F8).**

- An `l32r` literal that is inside a function but not at its entry now makes
  no edge and does not mark the caller `indirect`.
- Test `FILTER_D` codifies this: `callx8` to `far_fn+0x4` is not an edge.
- The sound choice is to treat it as an unresolved indirect call. Today it
  quietly shortens the walk.
- The legacy name-keyed parser (main, executor, httpd) has no indirect
  concept at all, so the same case there is a silent lower bound reported as
  OK.

**L2 (LOW): a double OOM at boot shows an empty profile table with nothing to
explain it.**

- If `nvs_load_all_from` and then the `nvs_load_files_only` retry both fail
  to allocate, the files-only path returns with no log.
- `profiles_boot_load_body` discards that result with `(void)`.
- Saves are refused (fail-closed), but no HTTP surface reports the load
  failure, so the profiles look absent to the operator.
- This follows the existing degraded-boot pattern; it is not new with this
  batch.

**L3 (LOW): every check_all task is INDETERMINATE.**

- At the tip, `check_all_task_stack_budgets` reports "0 of 33 tasks fully
  measured; 33 INDETERMINATE" and exits 0. Every ceiling is a lower bound.
- The F3 ROM-call warning fires for 33 of 33 tasks.
- Not caused by this batch, but it limits what a green check_all run proves.

**INFO:**

- I1: the `nvs_load_all_from` OOM comment says "NVS-decoded content left as
  loaded". On any error the caller memsets `s_profiles` and retries
  files-only, so the content is not kept.
- I2: the F9 figures disagree. The review doc says main 6464 -> 5568; the
  `794fce57b` message says 6384 over 6144. The post-fix 5568 is confirmed.
  The pre-fix figure depends on which analyser revision measured it.
- I3: `drop_worker_only_edges` resolves `nvs_save` with no path hint. If the
  name ever becomes ambiguous, the `ValueError` is swallowed and the edge is
  kept. That is conservative, but it gives no warning. The name is unique
  today.
- I4: a direct `call8` to a ROM address is not included in the F3 bodyless
  warning; only resolved long calls are.

## Negtest run (this review)

`tools/negtest.ps1`, command
`build_host_tests.ps1 -OutDir {OUT} -Only 'host_tests_profiles_http\.exe'`,
expect pattern `  FAIL |check\(s\) FAILED`. Two mutations of `profiles_http.c`:

1. `nvs_load_files_only` OOM returns `ESP_OK` with slots left known.
2. `nvs_load_all_from` OOM leaves `any_resolve_err` and `slot_res_err[]`
   false.

Result: baseline PASS (exit 0); **both mutations MISSED** (exit 0, no
failure line). Base `b4f054492`; real tree unchanged, copies removed. This
confirms M2: nothing in the host suite pins either OOM branch.

## Tests run

- `run_all_checks.ps1 -Only check_00_kilnfw_target_build -NoCache` at
  `91eb1f0ce` and at `b4f054492`: both PASS.
- `build_host_tests.ps1 -Only 'host_tests_profiles_http\.exe'` at
  `91eb1f0ce`: 1387/1387 passed.
- Every `check_*stack_budget*` and `check_kilnfw_dram_bss_budget` against
  the `b4f054492` ELF: all PASS (table above).
- `check_all_task_stack_budgets.py --dump-ceilings` with three analyser
  revisions on one ELF, and the dev-tip analyser on the `103b7649d` ELF:
  attribution in Q2.
