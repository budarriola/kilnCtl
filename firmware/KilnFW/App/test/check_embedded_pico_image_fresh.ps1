# check_embedded_pico_image_fresh.ps1 -- proves the SaftyFW slot images
# embedded into the KilnFW application build are the ones the current
# SaftyFW source tree actually produces, not a stale leftover pair.
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
#   4. each carries exactly one valid identity record, and the two records
#      agree with each other;
#   5. the record's commit matches the current repo HEAD (`git rev-parse
#      --short HEAD`, the same invocation SaftyFW's own build stamps with) --
#      UNLESS the record's `dirty` flag is set, in which case this step
#      WARNS but still PASSES (a deliberately dirty local build is not a
#      staleness bug);
#   6. firmware/KilnFW/build/KilnCtrl.bin actually embeds a matching record
#      (catching a KilnFW build that is stale relative to a freshly rebuilt
#      SaftyFW, even though the SaftyFW bins themselves are current).
#
# All the actual parsing/comparison logic lives in the importable, unit-
# tested tools/PcTools/src/kilnctrl/pico_image_freshness.py -- this wrapper
# only locates files and a Python interpreter and prints the verdict, same
# split as check_httpd_task_stack_budget.ps1/.py.
#
# SKIP CONTRACT (exit 3, run_all_checks.ps1's reserved SKIP status): this
# check SKIPS -- does not PASS -- when the SaftyFW slot bins do not exist yet
# (SaftyFW hasn't been built) or when KilnCtrl.bin doesn't exist yet (KilnFW
# hasn't been built) or git is unavailable. This worktree, minted before the
# other two agents' firmware changes landed, has neither the objcopy step
# nor the EMBED_FILES wiring yet -- SKIP here is the correct, expected
# result until those land, not a bug in this check.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\check_embedded_pico_image_fresh.ps1
#   ... -SlotABin <path> -SlotBBin <path> -KilnCtrlBin <path>   # point at another build/worktree

param(
    [string]$SlotABin,
    [string]$SlotBBin,
    [string]$KilnCtrlBin
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path

if (-not $SlotABin) { $SlotABin = Join-Path $repoRoot "firmware\SaftyFW\build\SaftyFW_slotA.bin" }
if (-not $SlotBBin) { $SlotBBin = Join-Path $repoRoot "firmware\SaftyFW\build\SaftyFW_slotB.bin" }
if (-not $KilnCtrlBin) { $KilnCtrlBin = Join-Path $repoRoot "firmware\KilnFW\build\KilnCtrl.bin" }

# Same venv-or-PATH-fallback pattern as check_bench_test_registry.ps1: a
# worktree checkout has no venv of its own (gitignored, per-clone), so this
# falls back to `python` on PATH there with tools\PcTools\src inserted onto
# sys.path directly rather than hard-failing.
$venvPython = Join-Path $repoRoot "tools\PcTools\.venv\Scripts\python.exe"
$venvCfg = Join-Path $repoRoot "tools\PcTools\.venv\pyvenv.cfg"
if ((Test-Path $venvPython) -and (Test-Path $venvCfg)) {
    $python = $venvPython
} else {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $cmd) { $cmd = Get-Command python3 -ErrorAction SilentlyContinue }
    if (-not $cmd) {
        Write-Host "SKIP: no venv python at $venvPython and no python/python3 on PATH"
        exit 3
    }
    $python = $cmd.Source
}

$pyScript = @'
import sys

repo_root = sys.argv[1]
slot_a = sys.argv[2]
slot_b = sys.argv[3]
kilnctrl_bin = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else None

sys.path.insert(0, repo_root + r"\tools\PcTools\src")
from pathlib import Path
from kilnctrl import pico_image_freshness as fresh

result = fresh.check_slot_bins_fresh(
    Path(slot_a), Path(slot_b),
    Path(kilnctrl_bin) if kilnctrl_bin else None,
    Path(repo_root),
)
print(f"{result.status}: {result.message}")
if result.status == "SKIP":
    sys.exit(3)
elif result.status == "FAIL":
    sys.exit(1)
else:
    sys.exit(0)
'@

$tmpPy = Join-Path $env:TEMP "check_embedded_pico_image_fresh_$PID.py"
Set-Content -Path $tmpPy -Value $pyScript -Encoding utf8
try {
    & $python $tmpPy $repoRoot $SlotABin $SlotBBin $KilnCtrlBin
    $code = $LASTEXITCODE
} finally {
    Remove-Item -ErrorAction SilentlyContinue $tmpPy
}
exit $code
