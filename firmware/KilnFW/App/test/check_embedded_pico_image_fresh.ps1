# check_embedded_pico_image_fresh.ps1 -- proves the two SaftyFW slot images
# the KilnFW application build embeds are a matched, current pair, not a
# stale or mismatched one.
#
# BACKGROUND (docs/PICO_AUTO_UPDATE_PLAN.md, 2026-09-20 owner decision): the
# KilnFW application now EMBED_FILES two SaftyFW slot images
# (firmware/SaftyFW/build/SaftyFW_slotA.bin / SaftyFW_slotB.bin, produced by
# check_00_saftyfw_target_build.ps1 via objcopy from the slot-linked ELFs)
# so the ESP can update the Pico automatically at boot. Each slot image
# carries a magic-tagged saftyfw_image_identity_t record
# (firmware/CommonFW/include/kilnlink/saftyfw_image_identity.h). This check
# verifies, in order:
#   1. both slot .bin files exist;
#   2. they are equal length (position-dependent linking, same source);
#   3. they are NOT byte-identical (they must differ -- same source, two
#      different link addresses);
#   4. each carries exactly one valid identity record;
#   5. the two records agree with each other -- commit, dirty flag, AND
#      config_format_version (D5: a schema mismatch between slots is just
#      as real a defect as a commit mismatch, and used to slip through);
#   6. the record's commit matches `git log -1 --format=%h` scoped to
#      firmware/SaftyFW, firmware/CommonFW, firmware/hwAbstraction/pico,
#      firmware/hwAbstraction/common and firmware/hwAbstraction/interface
#      (the same pathspec SaftyFW's own build stamps with, NOT plain `git
#      rev-parse --short HEAD` -- see docs/PICO_AUTO_UPDATE_PLAN.md sec 13) --
#      UNLESS the record's `dirty` flag is set, in which case this step is
#      skipped and the check reports WARN instead of PASS (a deliberately
#      dirty local build is not a staleness bug, but it is worth flagging
#      visibly rather than reading as an ordinary silent PASS -- see D3).
#
# OUT OF SCOPE (opus review 2026-09-20, D1): this check does NOT look at
# firmware/KilnFW/build/KilnCtrl.bin's own embedded copy of the record.
# check_00_kilnfw_target_build.ps1 builds in an isolated
# `C:\wt\checkbuild_*` worktree, so the in-tree KilnCtrl.bin this standing
# check would otherwise read is whatever a previous, unrelated build left
# behind -- comparing against it would FAIL on an ordinary healthy tree
# whenever that leftover .bin was stale or missing, for reasons having
# nothing to do with whether the slot bins themselves are fresh. The
# KilnCtrl.bin-embedding comparison still exists, but only as a best-effort
# provenance note at actual flash time (`_pico_image_provenance_note()` in
# tools/PcTools/src/kilnctrl/mcp_server_flash.py), never as a standing gate.
#
# KNOWN LIMITATION (D4): the identity record carries no slot indicator, so
# this check cannot detect slotA and slotB content being swapped between the
# two output files (a swapped pair still agrees on every field this check
# can see) -- see pico_image_freshness.py's module docstring.
#
# All the actual parsing/comparison logic lives in the importable, unit-
# tested tools/PcTools/src/kilnctrl/pico_image_freshness.py -- this wrapper
# only locates files and a Python interpreter and prints the verdict, same
# split as check_httpd_task_stack_budget.ps1/.py.
#
# STATUS CONTRACT: this check can print PASS, WARN (a visible warning line,
# still exit 0 -- distinct from a silent PASS whose message merely mentions
# "WARNING", per D3), FAIL (exit 1), or SKIP (exit 3, run_all_checks.ps1's
# reserved SKIP status) when the SaftyFW slot bins do not exist yet
# (SaftyFW hasn't been built) or git is unavailable.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\check_embedded_pico_image_fresh.ps1
#   ... -SlotABin <path> -SlotBBin <path>   # point at another build/worktree

param(
    [string]$SlotABin,
    [string]$SlotBBin
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path

if (-not $SlotABin) { $SlotABin = Join-Path $repoRoot "firmware\SaftyFW\build\SaftyFW_slotA.bin" }
if (-not $SlotBBin) { $SlotBBin = Join-Path $repoRoot "firmware\SaftyFW\build\SaftyFW_slotB.bin" }

# Same venv-or-PATH-fallback pattern as check_bench_test_registry.ps1: a
# worktree checkout has no venv of its own (gitignored, per-clone), so this
# falls back to `python` on PATH there with tools\PcTools\src inserted onto
# sys.path directly rather than hard-failing.
. (Join-Path $repoRoot "tools\lib_pctools_python.ps1")
$python = Resolve-PcToolsPython -RepoRoot $repoRoot
if (-not $python) {
    Write-Host "SKIP: no PcTools python (worktree venv, KILNCTL_PCTOOLS_PYTHON, main tree venv) and no python/python3 on PATH"
    exit 3
}

$pyScript = @'
import sys

repo_root = sys.argv[1]
slot_a = sys.argv[2]
slot_b = sys.argv[3]

sys.path.insert(0, repo_root + r"\tools\PcTools\src")
from pathlib import Path
from kilnctrl import pico_image_freshness as fresh

result = fresh.check_slot_bins_fresh(Path(slot_a), Path(slot_b), Path(repo_root))
print(f"{result.status}: {result.message}")
if result.status == "SKIP":
    sys.exit(3)
elif result.status == "FAIL":
    sys.exit(1)
elif result.status == "WARN":
    sys.exit(2)
else:
    sys.exit(0)
'@

$tmpPy = Join-Path $env:TEMP "check_embedded_pico_image_fresh_$PID.py"
Set-Content -Path $tmpPy -Value $pyScript -Encoding utf8
try {
    $output = & $python $tmpPy $repoRoot $SlotABin $SlotBBin
    $code = $LASTEXITCODE
} finally {
    Remove-Item -ErrorAction SilentlyContinue $tmpPy
}
if ($code -eq 2) {
    # WARN (D3): print visibly as a warning, distinct from a silent PASS
    # whose message merely happens to mention "WARNING", but still exit 0 --
    # a dirty local build is not a failure.
    foreach ($line in $output) { Write-Warning $line }
    exit 0
}

# SKIP-FAST (2026-09-23): -Fast skips check_00_saftyfw_target_build.ps1, the
# sole producer of both slot .bin files -- so pico_image_freshness.py's
# "SaftyFW slot bins not built yet" SKIP (checked here by its exact message,
# never by env var alone) is a direct, expected consequence of -Fast, not a
# defect. Relabeled to SKIP-FAST only for that specific reason and only when
# run_all_checks.ps1 set KILNCTL_CHECKS_FAST (i.e. -Fast is actually in
# effect); pico_image_freshness.py itself stays a pure, env-agnostic,
# unit-testable module -- this relabeling lives only in this thin wrapper.
# Any OTHER SKIP reason (e.g. git unavailable) is left as a plain SKIP.
if ($code -eq 3 -and $env:KILNCTL_CHECKS_FAST -and
        ($output -join "`n") -match 'SKIP: SaftyFW slot bins not built yet') {
    $output | ForEach-Object { Write-Host ($_ -replace '^SKIP:', 'SKIP-FAST:') }
    exit 3
}

$output | Write-Host
exit $code
