# Reviewer rules

Read `COMMON.md` first. You review a commit in a named worktree before the coordinator
pushes it. You are read-only: do not edit, commit, or push. Do not touch the boards.

## What to check

1. Claims match the code. Open every file the commit message or hand-back cites and confirm
   the constant, function, line, or behaviour is really there and really does what is
   claimed. Wrong file or wrong line is a required fix.
2. Correctness under the firmware invariants in `IMPLEMENTER.md` (stack, locks, NVS, httpd
   buffers, route cap, auth gates, reset-one-side pairs). Ask "who else holds a copy of this
   state" for every reset.
3. Tests are real. A test that cannot fail (gated out before reaching its code, asserting a
   constant, reading a prebuilt binary) is a required fix. Confirm the negative test the
   implementer reports actually exercised the new code. If you run a negative test
   yourself, check `git status --porcelain` in both the worktree and the shared tree
   afterward (see COMMON.md's .NET-relative-path footgun) and restore by hand, never with
   `git checkout --`.
4. Yield and priority semantics on FreeRTOS: `taskYIELD()` never lets a lower-priority task
   run; blocking needs `vTaskDelay`. Check any "yield" added to a long loop.
5. Docs: no credential values, hostnames, or SSIDs; no backticked hex that
   `check_doc_hash_citations.ps1` would read as a commit; ROADMAP edits lean, additive, and
   consistent with neighbouring rows; line endings unchanged (`git diff --stat` shows small
   counts, not a whole-file rewrite).
6. Scope: nothing in the diff the prompt did not ask for, nothing the prompt asked for left
   out silently.

## Hand-back

`PASS` or a numbered list of required fixes, each with `file:line`, what is wrong, and what
would make it right. Separate advisory notes from required fixes. No praise, no restating
the diff.
