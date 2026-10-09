# git stash incident — 2026-09-08

An agent finishing `70ed6514` ("/api/cfgfs's nvs_only list was stale and
misleading, not just cosmetic") reported having verified a pre-existing check
failure "via `git stash`". `git stash` is banned in this tree: the working
tree is shared by a dozen concurrently-running agents and other Claude
sessions, and a stash pockets *every* uncommitted change in the tree, not
just the stashing agent's own — a stash/pop cycle can silently lose or
reorder someone else's in-flight work.

## What actually happened

`git stash list` and `refs/stash` are both empty now — the stash was created
and then dropped or cleared, with no `.git/logs/refs/stash` left behind. The
only trace is a dangling merge commit recovered via `git fsck
--unreachable`:

```
f69c99abb908a23881069ace192d22ccd8f380db  2026-09-08 11:19:28 -0700
"WIP on main: deaccc4f full_board_backup.py: capture and restore the cfg
filesystem's raw files"
parents: deaccc4f (base) + 35e320fb (index tree)
```

It is a plain 2-parent stash commit (no third "untracked files" parent), so
it held exactly the files shown by `git show --stat` on it:

- `firmware/KilnFW/App/drivers/persist/cfg_fs_status.c`
- `firmware/KilnFW/App/drivers/persist/cfg_fs_status.h`

`git diff HEAD f69c99ab -- <those two files>` is **empty** — the stashed
content is byte-identical to what later landed in `70ed6514`. Nothing else
was in the stash (no wizard, backup-tooling, on/off-zone, or format-path
files — the concern at the time this was flagged), and nothing is stranded:
the stash's only payload is already in a commit on `main`.

## Verdict

No work was lost, delayed, or is currently stranded. The other agents'
concurrent in-flight files present in the tree at the time
(`safety_trip_words.h`, `test_ramp_assist_cfg.c`,
`tools/PcTools/src/kilnctrl/mcp_server_profiles.py`, `uv.lock`) were never
part of this stash commit's tree diff and remain modified-in-place in the
working tree today, undisturbed.

## Why this is still a live hazard, not a near-miss to shrug off

This time the stashed agent's own change happened to be the *only* uncommitted
change of consequence at that moment, and it got carried through cleanly by
luck of timing, not by any property of `git stash` that makes it safe here.
The mechanism is unconditional: `git stash` snapshots and reverts **the
entire working tree**, every file, regardless of which agent's edit it is.
Had a second agent been mid-edit on any file at that same instant — as
several routinely are (wizard screens, on/off zone work, backup tooling,
format-path fixes, all active around this date) — their uncommitted changes
would have been swept into the same stash entry and reverted out from under
them, then only restored if and when someone knew to `stash pop`/`apply`
that specific entry. `git commit -o <own files>` (or diffing on a worktree
copy) verifies a "pre-existing failure" without touching a single byte
outside the files under test. Use that instead — never `git stash` in this
tree.
