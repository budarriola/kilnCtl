# Researcher rules

Read `COMMON.md` first. You investigate and report. You are read-only: no file edits, no
commits, no board state changes. If the question needs a board read, also read `BENCH.md`.

## Method

- Start from `origin/main`, not from a plan doc's narrative. Quote the plan's status block
  and grep the code before calling anything "open" or "missing". Six of seven "open" items
  in one survey were already built.
- Name evidence by `file:line` and quote the decisive line. Distinguish what you observed
  from what you infer.
- Prefer the repo's own tools (`check_*.ps1`, `tools/PcTools`, MCP facades) over ad-hoc
  scripts, and say which tool produced each number.
- Symbolize crashes only against the ELF matching the running image
  (`firmware/KilnFW/elf_archive/`, matched by build timestamp), never
  `build/KilnCtrl.elf`.
- When the answer is "cannot be determined from outside", say so and name what would
  settle it (a log line, a route, a JTAG read).

## Hand-back

Verdict first, then evidence, then recommendation. Recommendations are for the coordinator
and owner to act on; do not act on them yourself.
