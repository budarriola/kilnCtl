# Adversarial review: Kconfig-gated task rows in check_all_task_stack_budgets.py (2026-09-15)

Reviewed commit: `3ec08793`, "Stack-budget check: adjudicate Kconfig-gated task
rows instead of hard-failing". Reviewed against parent `3ec08793^` throughout.
`check_all_task_stack_budgets.py` itself is byte-unchanged between `3ec08793`
and `origin/main` as at `827dd887`, so every checker-behaviour result below
applies to mainline as it stands. `827dd887` landed *while this review was in
flight* and rewrote `check_00_kilnfw_target_build.ps1`; it changes the premise
of F1 only, and F1 below is written against the updated base.

## Verdict

The three-way adjudication **is** implemented as described, on every path, and
the "only two rows are gated" completeness claim **is** correct -- I re-derived
it mechanically rather than accepting it. The reported defect is genuinely
fixed. A row without a `kconfig` key is genuinely unchanged: on the same ELF
with the option on, child and parent output is **byte-identical, 0 differing
lines, both exit 0**.

Four defects follow, one of which introduces a *new* false FAIL in the primary
CI path, and one of which makes it possible to excuse a genuinely-deleted task.
None of them invalidates the fix; F1 and F2 should be addressed.

## Method

Clean worktree of `origin/main` at `C:\wt\stkrv_<unique>`, `idf.py set-target
esp32s3` then a full `idf.py build` (the ESP-IDF build in a fresh tree succeeds
once the target is set explicitly; the `hal_sysinfo_esp.c` failure is an
artifact of defaulting to target `esp32`, not a mainline break). All ESP-IDF
builds via the PowerShell tool. The parent version of the checker was
materialised with `git show` and run side by side with the child against the
same ELF/`sdkconfig` pairs, so every result below is a child-vs-parent
comparison rather than a bare assertion.

## Test results -- per test, child and parent

Ten scenarios. Six confirm the design holds; four are the defects below.

| # | Scenario | Child | Parent | Verdict |
|---|---|---|---|---|
| 1 | Probe compiled OUT + `sdkconfig` probe off (the reported defect) | exit 0, `gpio_probe` excluded-by-config, 27 rows graded | exit 1, "could not resolve root symbol `gpio_probe_task`" | fix works; load-bearing |
| 2 | Option ON in `sdkconfig`, symbol absent | exit 1, names the root | exit 1, **identical message** | design holds; **test not load-bearing** |
| 3 | `check_00` shape: main-tree ELF (probe ON) + worktree `sdkconfig` (probe off) | **exit 1**, "must not exist -- but it resolves" | **exit 0**, 28 rows graded | **F1** -- see the note below; `827dd887` stopped `check_00` producing this shape |
| 4 | `sdkconfig` nonexistent | raises, row FAILs, never "assume off" | n/a | design holds |
| 5 | `sdkconfig` present but empty/garbage | returns `False` = **assume off** | n/a | **F3** |
| 6 | Option OFF, symbol present | exit 1, disagreement named | exit 0 (no cross-check) | design holds; load-bearing |
| 7 | Genuine disappearance, non-gated row (`screen_idle_task` renamed, **full fresh rebuild** into an unused dir) | exit 1, names `screen_idle_task` | exit 1 (plus `gpio_probe`) | design holds |
| 8 | Gated row excused with its task symbol obliterated in source | **exit 0**, "verified absent" | exit 1 | **F2, subversion confirmed** |
| 9 | ELF whose `../sdkconfig` does not exist | exit 0, silently falls back to repo-root `sdkconfig`, no warning | n/a | **F4** |
| 10 | Unchanged-rows control: same ELF, option on, child vs parent output | 0 differing lines, exit 0 | 0 differing lines, exit 0 | rows without the key are unchanged |
| 11 | Fresh worktree: published ELF present, no `sdkconfig` at all | exit 1, both gated rows "cannot adjudicate ... no sdkconfig at ...", plus 4 non-gated rows | exit 1, "could not resolve `gpio_probe_task`", plus the same 4 non-gated rows | both FAIL; not a regression |
| 12 | Key missing from an otherwise-valid `sdkconfig` (derived from 5 and 11) | gated row: silently `False` = off | non-gated `CONFIG_KILNCTL_UART_*` row in the **same run**: FAIL, "not found in ..." | **F3**, inconsistent within one file |

Test 7's rename was restored by hand (no `checkout`/`restore`/`stash`), an empty
`git diff` confirmed, and the poisoned `build_neg` directory deleted; it was a
full rebuild into a directory that had never been built in, never a prebuilt
binary.

## F1 (HIGH) -- new false FAIL in the run_all_checks path

`check_00_kilnfw_target_build.ps1` copies the **main tree's** `sdkconfig` into
the shared `C:\wt\checkbuild`, builds there, and publishes only the resulting
`KilnCtrl.elf`/`.bin` into the *invoking* tree's `firmware/KilnFW/build/`. It
does **not** publish the `sdkconfig` that produced them. So in any worktree the
published ELF's true configuration is the main tree's (`CONFIG_KILNCTL_ENABLE_GPIO_PROBE=y`)
while `--elf`'s `../sdkconfig` is the worktree's own (`# ... is not set`).

That combination is precisely test 3: the child FAILs where the parent PASSED.
The commit's own `sdkconfig`-resolution change is what makes this reachable --
before it, both sides read the same repo-root path and there was no cross-check
to disagree, so the mismatch was invisible and harmless.

Scope of my evidence, stated exactly: I reproduced the artifact combination
`check_00` produces (main-tree ELF staged against a worktree `sdkconfig`), and
measured child exit 1 / parent exit 0 on it. I did **not** run `check_00`
itself, deliberately -- it mirrors and builds the shared main tree into shared
`C:\wt\checkbuild` under a global lock and would interfere with other live
sessions.

This also leaves the commit message's "`run_all_checks.ps1` in that worktree: 92
passed, 0 skipped" unreconciled: on my reading that run should have hit this
FAIL unless `check_00` skipped or the ELF measured was a locally-built one
rather than `check_00`'s published artifact. Worth the implementer confirming
which ELF that run actually measured.

### F1 on the updated base (`827dd887`)

`827dd887` rewrote `check_00` while this review was in flight. It now builds in
a per-tree directory and provisions the build worktree's `sdkconfig` by
**preferring the invoking tree's own** and falling back to the main worktree's
only when the invoking tree has none; artifacts are still published into the
invoking tree's `firmware/KilnFW/build/`. Where the invoking tree has its own
`sdkconfig`, ELF and config now come from the same file and the test-3
disagreement cannot arise. F1 as measured is therefore largely closed -- by a
change to `check_00`, not by this commit.

What remains is the fallback branch, which is the standing
clean-worktree-at-`origin/main` workflow: that tree has no `sdkconfig` of its
own (gitignored), `check_00` builds it against the **main tree's** config, and
the checker then finds no `sdkconfig` beside the published ELF and none at the
repo root either. Measured (test 11): child exit 1, "`gpio_probe`: cannot
adjudicate CONFIG_KILNCTL_ENABLE_GPIO_PROBE: no sdkconfig at ... (build the
project first)", both gated rows plus four non-gated ones. The parent exits 1
on the same input too, for its own reasons, so this is **not a regression** --
but it does mean the clean-worktree path still cannot pass this check after
`check_00` succeeds, and the remedy is for `check_00` to publish the
`sdkconfig` it actually built with alongside the ELF it publishes. That single
change would close both this and F4.

Note also that `sdkconfig.defaults` now pins `CONFIG_IDF_TARGET="esp32s3"`
(same commit), so a fresh tree's `idf.py build` no longer silently targets
`esp32`; a tree built that way gets its own `sdkconfig` with the probe off and
lands in the consistent branch.

## F2 (HIGH) -- a genuinely-deleted gated task is excused

For an excluded row the checker asserts only that the **symbol is absent from
the ELF**. It never runs that row's `stack=` extractor, so nothing checks that
the task still exists in source at all.

Measured (test 8): with the option off, I obliterated every occurrence of
`gpio_probe_task` in `gpio_probe.c` and the checker still reported exit 0 and
"its root symbol was confirmed absent from the ELF, which is exactly what that
option being off should produce." The ELF was untouched and correct -- the
region is compiled out either way -- so this is not a stale-artifact artefact;
it is the adjudication having no source-side evidence to contradict.

Because `CONFIG_KILNCTL_ENABLE_GPIO_PROBE` defaults to `n`, this is the state of
**every clean worktree and every CI-shaped build**: `gpio_probe` could be
deleted outright and this check would keep passing and keep printing "verified
absent". The parent FAILed the same input.

Cheap fix: for an excluded row, still call `task["stack"]()`. The extractor
regexes match the `xTaskCreate*` call site by task-function name and literal, so
they fail loudly if the task is renamed or removed, while remaining insensitive
to whether it was compiled in. That converts "absence verified" from one-sided
into a genuine two-sided check at no measurement cost.

## F3 (MEDIUM) -- "unreadable sdkconfig" means only "nonexistent"

The commit's stated rule is "sdkconfig unreadable -- FAIL, never assume off",
and for a *missing file* that holds (test 4: raises, row FAILs). But
`_load_sdkconfig()` treats any file it can open as authoritative, and
`sdkconfig_bool()` returns `False` for a key that is simply absent from the
parsed dict. An empty, truncated, or garbage `sdkconfig` therefore reports
**every** option as off (test 5: returned `False`).

The sharpest evidence that this is wrong is internal to the checker. Four
non-gated rows (`safety_owner_evt`, `safety_proto_rx`, `uart_owner_evt_task`,
`uart_proto_rx`) already read `sdkconfig` for their declared stack size, via a
lookup that **FAILs on a missing key** -- "CONFIG_KILNCTL_UART_OWNER_STACK_SIZE
not found in ...". So in one run, against one file, a key that is absent is a
hard error for a stack-size lookup and a silent "the feature is off" for a gate
(test 12). The pre-existing lookup has the right behaviour; the new one does
not.

A truncated `sdkconfig` is not hypothetical here -- `check_00` copies this file
around, and this repo has already had one non-atomic-publish incident
(`docs/audits/stack_budget_remeasure_after_elf_publish_bug_2026-09-10.md`).
Combined with F2, a half-written `sdkconfig` silently excuses every gated row.

Fix: require a positive sanity signal before trusting the parse -- e.g. that the
file yielded a plausible key count, or that a known-always-present key such as
`CONFIG_IDF_TARGET` is present -- and raise otherwise.

## F4 (MEDIUM) -- the repo-root fallback the commit set out to remove is still there

`use_sdkconfig_for_elf()` falls back to `DEFAULT_SDKCONFIG` (repo root) whenever
`--elf`'s `../sdkconfig` is missing, silently and with no warning. Measured
(test 9): an ELF in a directory with no sibling `sdkconfig` was adjudicated
against the worktree's repo-root `sdkconfig`, exit 0, nothing printed.

The `../sdkconfig` assumption itself is worth stating plainly, since it is the
subtlest part of the change:

- `build/KilnCtrl.elf` (the default, and the only path `run_all_checks.ps1`
  exercises -- the `.ps1` wrapper passes `--elf` only when given `-ElfPath`, and
  nothing in the tree passes it): `../sdkconfig` resolves to exactly
  `DEFAULT_SDKCONFIG`. For this path the change is a **no-op**.
- `elf_archive/KilnCtrl-<hash>.elf` and `KilnCtrl-latest.elf`: `elf_archive/` is
  a *sibling* of `build/`, so `../sdkconfig` resolves to the same live
  `firmware/KilnFW/sdkconfig` -- i.e. the config of whatever is configured
  **now**, not the config that produced that archived ELF. The archive stores no
  `sdkconfig` alongside its ELFs. So for archived ELFs the new resolution is
  confidently wrong rather than merely unhelpful, and F1's mis-adjudication
  applies with full force.
- A build directory elsewhere (e.g. `-B build_neg`, or `C:\wt\checkbuild`):
  resolves correctly.

So the change is a no-op where it is actually invoked, correct for a sibling
build dir, and wrong for the archive. It does not make anything worse than the
parent, but it does not deliver the claimed guarantee either.

## Completeness sweep -- done independently, claim confirmed

I did not accept the "no other TASKS root is conditionally compiled" claim. I
walked every `.c` under `firmware/KilnFW/App` and `firmware/hwAbstraction/esp`,
tracking preprocessor nesting depth, and reported every definition of any of the
28 TASKS roots or 7 `extra_roots` callbacks occurring at depth > 0. Exactly two
hits:

- `drivers/bridge/gpio_probe.c:157` `gpio_probe_task` under `#if !CONFIG_KILNCTL_ENABLE_GPIO_PROBE`
- `drivers/hw/backlight_pwm.c:66` `backlight_pwm_task` under `#if CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE`

The `backlight_pwm` claim checks out specifically: the `#if` at line 57 spans to
`#endif` at 187 with an `#else` no-op stub at 164, wrapping both the task body
and its `xTaskCreate` at line 153.

Build-system gating, checked separately since a source excluded by CMake would
not show up above: `drivers/CMakeLists.txt` registers `gpio_probe.c`,
`backlight_pwm.c` and `screen_idle.c` unconditionally; its only conditional
source group, `SIM_SRCS`, registers no task. `hwabstraction_esp`'s `SRCS` list
is unconditional. `extra_roots` is complete against the five `lv_timer_create`
sites in `drivers/ui/ui_page_*.c`, and `ili9488_flush_cb`/`touch_read_cb` are
defined *outside* `lvgl_port.c`'s `#if KILNCTL_SPI_ASYNC_FLUSH` regions.

## F5 (LOW) -- gating that the `kconfig` key cannot express

`KILNCTL_SPI_ASYNC_FLUSH` (and its siblings in `drivers/hw/settings.h`) is a
plain `#if` on a **normalised macro**, defined as 1/0 from `CONFIG_KILNCTL_SPI_ASYNC_FLUSH`.
It gates helper functions only today, no root, so nothing is wrong now. But the
`kconfig=` mechanism matches a literal `CONFIG_*` key against `sdkconfig`; if a
root ever moves inside one of these normalised `#if`s, the table cannot express
its gate and would silently fall back to "missing root = FAIL". Worth a comment
in the TASKS header noting that the key models `#if CONFIG_X` only.

Also minor: `--dump-ceilings` returns before the excluded-row report, so a
baseline regenerated with an option off silently omits that row's
`CEILING_BYTES` entry. This is self-correcting rather than dangerous -- a later
build with the option on FAILs loudly with "no `CEILING_BYTES` entry" -- but it
is a foot-gun for whoever regenerates the table.

## Is the checker weaker?

Yes, in one specific and real way, and no in general.

- **Weaker:** F2. For a row whose option is off -- which for `gpio_probe` is
  every clean worktree and every default build -- a genuinely deleted or renamed
  task is indistinguishable from a correctly compiled-out one, and is reported
  as "verified absent". The parent caught this by accident (it failed on
  *everything* absent); the child excuses it by design. That is the entire
  subversion the review asked about, and it does not require a mismatched `#if`
  to trigger -- plain deletion suffices.
- **Can "option off means verified absent" be subverted by an `#if` whose
  condition no longer matches the declared symbol?** Only in the safe
  direction. If the `#if` is renamed while the task still compiles in, the
  symbol resolves and the child FAILs loudly (test 6) -- noisy, but never a
  false pass. The dangerous direction is F2, where there is no `#if` mismatch at
  all.
- **Not weaker:** every non-gated row is byte-identically handled (test 10), a
  genuine disappearance still FAILs (test 7), and an on-option vanished root
  still FAILs (test 2).

## Note on the offered evidence

The commit's third negative test -- "with the option flipped on but the symbol
absent, the gated row still FAILS loudly" -- reproduces exactly (test 2), but
the **parent produces the identical failure on the identical input**. It cannot
catch the regression it names and is not load-bearing. The first two negative
tests are load-bearing (tests 1 and 7 discriminate child from parent). The
positive test is load-bearing.

## Recommendations, in priority order

1. F2: call the row's `stack=` extractor even when excluded, so absence is
   verified on both the ELF side and the source side.
2. F1: have `check_00` publish the `sdkconfig` it built with alongside the ELF,
   or have the checker prefer a `sdkconfig` published next to the ELF; failing
   either, do not treat ELF/`sdkconfig` disagreement as a FAIL when the ELF was
   not built from the tree being checked.
3. F3: refuse a `sdkconfig` that parses to implausibly few keys instead of
   reporting every option off.
4. F4: make the repo-root fallback warn, and store a `sdkconfig` snapshot beside
   archived ELFs.
