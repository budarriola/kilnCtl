# Review: lcdfx3 (1c7f60091, f0e615d07), 2026-10-10

Adversarial review of the fixes for LOW-1, LOW-2 and the INFO items in
`docs/audits/REVIEW_LCDFX2_DEVBREAK_2026-10-10.md`:
`tools/check_source_bytes.ps1` (new), `tools/check_lcd_admin_gates.ps1`
(comment stripping, C3 hub pre-gate rule, relock rule), the four repaired
raw-byte files, and the gate evidence row. Reviewed at base `f0e615d07` in a
minted worktree. No code was changed.

## Runs

| Command | Result |
|---|---|
| `tools/check_source_bytes.ps1` | PASS (2576 tracked text files) |
| `tools/check_lcd_admin_gates.ps1` | PASS |
| `firmware/KilnFW_recovery/main/check_recovery_health.ps1` | PASS (110 assertions) |
| `tools/check_negtest.ps1` | PASS (246 assertions) |
| `tools/check_gate_negative_test_table.ps1` | **FAIL**, see LOW-4 (predates lcdfx3) |

Negtests (`tools\negtest.ps1 -Preset check`, baseline PASS both times,
`real_tree_unchanged: true`, copies removed):

| Check | Mutation | Verdict |
|---|---|---|
| check_lcd_admin_gates | M1: `safety_nav_cb` keeps its gate, then calls `safety_open_apply(NULL);` after it | **MISSED** |
| check_lcd_admin_gates | M2: `if (0) run_gated(..., profiles_open_apply, NULL); else profiles_open_apply(NULL);` | **MISSED** |
| check_lcd_admin_gates | M3: gated `safety_nav_cb` moved under `#if 0`, an ungated twin under `#else` | **MISSED** |
| check_lcd_admin_gates | M4: `if (0) ui_page_network_relock_close();` in `handle_lcd_relock_to_home` | **MISSED** |
| check_lcd_admin_gates | M5 (positive control): gate prompt text `"Enter // PIN /*"` | PASS, as it should (the stripper skips strings) |
| check_source_bytes | T1 (sanity): BS byte in a comment of tracked, dirty `kiln_ui.c` | CAUGHT (`control byte 0x08@line 139`) |
| check_source_bytes | U1: new untracked `.c` file with a BS byte | **MISSED** |
| check_source_bytes | D1/D2: BS / NUL byte in tracked `docs/agent_rules/COMMON.md` | **MISSED** (extension excluded) |
| check_source_bytes | K1: lone CR in tracked `firmware/KilnFW/App/drivers/Kconfig` | **MISSED** (extension excluded) |

Scratch-repo experiment (outside this repo): a tracked `a.c` made dirty with
a NUL fails the check (exit 1); the same file held open with
`FileShare.None` while the check runs passes it (exit 0, "1 tracked text
files clean"). See LOW-1.

## Findings

### LOW-1: check_source_bytes passes silently when `git add -u` fails

`& git -C $Repo add -u 2>$null | Out-Null` runs under
`$ErrorActionPreference = "Continue"`, and `$LASTEXITCODE` is never checked.
`git add -u` stops at the first file it cannot read
("fatal: updating files failed"). The private index then still holds the
unrefreshed copy of the real index, so **every** dirty working-tree edit goes
unchecked and the check prints PASS.

Scenario: the main tree lives under OneDrive. A sync client, antivirus scan or
editor holds one stat-dirty file without read sharing. An agent adds a raw NUL
to another file and runs the check: PASS. The scratch-repo run above shows
this with a single locked file.

Fix: check `$LASTEXITCODE` after `git add -u` and print a FAIL line with git's
stderr. Do not fall back to the unrefreshed index.

### LOW-2: an empty file list is a vacuous PASS

The exit codes of `rev-parse`, `ls-files -s` and `ls-files --eol` are not
checked. If `ls-files -s` fails or prints nothing (a corrupt private index, a
wrong `-Repo`, a git error), `$files.Count` is 0. `Scan` then returns nothing,
and the check prints `source byte check passed: 0 tracked text files clean.`
with exit 0.

Fix: check `$LASTEXITCODE` after each git call, and refuse when fewer than a
sane floor of files was scanned (for example 1000), or when a known anchor such
as `tools/check_source_bytes.ps1` itself is missing from the list.

### LOW-3: check_lcd_admin_gates C3 inspects only the text before the gate

The C3 rule takes the first `run_gated(... LCD_PIN_ROLE_USER` in each
`*_nav_cb` body and refuses a page-open call before it. Nothing after it is
inspected, and a callback with no USER gate is skipped (`continue`).
M1 (a direct `safety_open_apply(NULL);` after the gate) is MISSED. The PIN
keypad then appears over a Safety page that is already open, so the USER gate
is bypassed. This is a plausible slip: someone adds a direct open "to refresh"
the page. M2 (a gate under `if (0)`) and M3 (a gated copy under `#if 0` with an
ungated `#else` twin) are deliberate bypasses and are also MISSED. M3 still
satisfies both the per-target text rule and the `navN >= 5` floor, because the
`#if 0` copy is counted.

Fix: in each hub nav callback, after comment stripping:
- remove the `run_gated(...)` statement and refuse any remaining
  `kiln_ui_show(` / `_open(` / `_open_apply(` call in the body;
- require the gate statement to be unconditional (not preceded by `if`,
  `else`, `?` or `return;`);
- blank `#if 0 ... #endif` regions, or refuse any `#if 0` in the scanned UI
  files;
- enumerate the callbacks from the `build_nav_item(grid, "...", <cb>)` calls
  instead of the `*_nav_cb(lv_event_t *e)` name pattern, so a renamed
  callback cannot drop out of the scan. An enumerated callback that has no
  gate and is not an allowlisted ADMIN-gated one (touch cal) should fail.

### LOW-4: check_gate_negative_test_table is red on origin/dev (predates lcdfx3)

At `f0e615d07` and at the current dev tip it fails:
`no row for discovered check: tools/check_touch_cal_exit_target_caller.ps1`
(added in fa2c7d9a7) and `tools/test_check_submodule_pins_pushed.ps1`
(dfabdd951). Both landed before f0e615d07, which edited this table and its
counts without running the check. lcdfx3 did not cause the failure, but it
left the check red.

Fix: add rows for both checks (NOT AUDITED is allowed), recount, and rerun
the check.

### INFO-1: the `# checkcache: ok` marker is valid

The cache stores a result only for a clean tree (`git status --porcelain`
empty), keyed by the HEAD tree hash plus the git version in the env
fingerprint. On a clean tree the real index equals the HEAD tree, and
`git add -u` rehashes stat-dirty files to the same blobs. Git's existing-CRLF
safety rule also keeps any CRLF-in-index blob unchanged under
`core.autocrlf=true`, which is set here. So the scanned blobs and the
`ls-files --eol` index classification are a pure function of the HEAD tree.
The check reads `$GIT_DIR/index` and writes a temp directory and loose
objects. On a clean tree none of these change the result. No change needed.

### INFO-2: the real index is never written, and the copy race is benign

`GIT_INDEX_FILE` points at the temp copy, so `git add -u` takes
`<tmp>/index.lock`, never `$GIT_DIR/index.lock`. A concurrent git in the same
worktree replaces the index by writing `index.lock` and then renaming it.
`Copy-Item` therefore reads either the old or the new generation, never a torn
file. The copy takes milliseconds, which is no longer than an ordinary
`git status` read. Git for Windows retries a rename that hits a sharing
violation. `core.splitIndex` is unset, so no shared-index file is written into
`$GIT_DIR`. For dirty files, `git add -u` writes loose objects into the shared
object store; these are harmless garbage that gc prunes. Leftovers: the
`finally` block removes `%TEMP%\srcbytes_<guid>`. Only a hard kill (for example
a run_all_checks timeout) leaks one, a few MB with no sweep. Optional fix: at
startup, remove `srcbytes_*` directories older than an hour.

### INFO-3: extensions not covered

`.md` (556 files), `.jsonl`, `.tsv`, `.in`, `Kconfig`/`*.projbuild`,
`sdkconfig*.defaults`, `.toml`, `.yml` and `.gitignore` are not scanned
(D1, D2 and K1 MISSED). A NUL in a `.md` makes git class the file as binary,
which hides its diffs from review, the same blindness LOW-1 of the earlier
review was about. Kconfig and the defaults files feed the build. Today these
types are clean of control bytes. Only `firmware/SaftyFW/docs/CONFIG_REFERENCE.md`
is CRLF in the index. Fix: add `.md` (normalize that one file or tolerate CRLF
for `.md`), plus the Kconfig, defaults, `.in`, `.jsonl`, `.tsv`, `.toml` and
`.yml` types. `CMakeLists.txt` is special-cased, but `.txt` already covers it.

### INFO-4: untracked files are not scanned (U1)

The scan reads `git ls-files` only. A new file that has not been `git add`ed
yet escapes a pre-commit local run. The coordinator's full run on the dev tip
sees committed files, so dev is still guarded. Fix (optional): also scan
`git ls-files --others --exclude-standard` files of a text type, for example
`git add -A -- <text pathspecs>` into the private index.

### INFO-5: false positives

There are none today: 2576 files pass. Binaries are excluded by extension, so
a binary fixture saved as `.txt`, `.json` or `.csv` would be refused. None
exists. A UTF-16 text file would be refused (NUL bytes), which is desirable.
`.ps1` CRLF in the index is tolerated, consistent with `.gitattributes`
`*.ps1 text eol=crlf`. A lone CR in a `.ps1` is still refused. Form feed is
refused (GNU page breaks would trip it; none exist). DEL (0x7F) is not
refused. No action needed.

### INFO-6: comment stripper edge cases

The stripper is correct for strings and char literals, including escapes
(M5). Two cases are not handled:
- A `\`-newline continuation of a `//` comment. C continues the comment, but
  the stripper resumes code at the newline.
- `#if 0` blocks (see LOW-3).

Both are rare. The pass message still reads "N gates plus L9 init order" and
does not mention the C3 and relock rules (cosmetic).

### INFO-7: the four repaired files

- `test_recovery_health.c`: `'\r'` is restored, and check_recovery_health
  passes.
- The comments in `check_bootloader_builds.ps1:50` and `check_isolation.ps1:36`
  are repaired.
- `kiln_cfg_swap.c`: the stale comment is merged correctly.

The `check_negtest.ps1` change from `` `r?`n `` is not too permissive. The
lazy `(.*?)` stops before the optional CR, so the captured Add-Type body never
carries a trailing CR. Both the CRLF working copy and the LF index form match,
and `}` must start a line. check_negtest passes.

The same writer damage left a sibling the byte check cannot see:
`check_bootloader_builds.ps1:53` reads `.TrimEnd('')` and has done so since
131ed7b0e. The intended text was `'\'`: the writer consumed the backslash.
`TrimEnd('')` only trims whitespace, so this is harmless today, because
`GetFullPath` of the `Join-Path` result has no trailing backslash. The
"whole-tree scan found no others" note is accurate only for control bytes.
Fix: write `TrimEnd('\')`.


## Fix status (lcdfx4)

LOW-1, LOW-2, LOW-3 and INFO-7 are fixed (commit recorded in git log: "Checks: source-bytes fails on git errors..."); INFO-2 temp sweep added. All reviewer mutations M1-M3 plus an ungated-callback case and git-failure/empty-list cases are CAUGHT by tools/negtest.ps1. LOW-4 is owned by another fixer.
