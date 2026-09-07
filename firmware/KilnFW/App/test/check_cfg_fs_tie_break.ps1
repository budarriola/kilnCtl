# check_cfg_fs_tie_break.ps1 -- pins the ONE invariant the cfg-filesystem
# dual-write bridges share: the divergence tie-break between the file copy
# and the NVS copy must give the file the win only on a STRICTLY higher rev.
#
# WHY THIS IS NARROW ENOUGH TO BE HONEST (CLAUDE.md's standard for a
# mechanical check): it is pinned to one concrete, stable pair of states --
# `file_rev` vs `nvs_rev` inside `App/drivers/persist/*_cfg_fs.c` -- not to
# a general "reset one side of a pair" shape. Those two variable names are
# the shared vocabulary every bridge module already uses, and there is
# exactly one correct comparison for the branch that decides which side wins:
#
#   file strictly ahead  -> the file write landed, the NVS write did not.
#                           File is newer. `file_rev > nvs_rev` -> file wins.
#   NVS strictly ahead   -> the file write failed after NVS advanced.
#                           NVS is newer. else-branch -> NVS wins.
#   EQUAL, bytes differ  -> ONLY reachable when an NVS writer that does not
#                           know about the rev key wrote the blob: firmware
#                           rolled back past the dual-write, or a crash
#                           between the blob write and the rev write. In BOTH
#                           of those the NVS copy is the newer one.
#
# So `file_rev >= nvs_rev` in the winner-selection branch is always a defect:
# it silently discards the edit made on rolled-back firmware and then
# overwrites it on the next save -- exactly the downgrade hazard
# docs/FILESYSTEM_USER_DATA_PLAN.md section 4 introduced the rev counter to
# close. Found in `zones_config_cfg_fs.c` by
# docs/audits/filesystem_migration_review_2026-09-07.md.
#
# TRACKED vs UNTRACKED: this FAILS only on files git already tracks. A
# bridge module that is still untracked working-tree WIP (another session
# mid-pass) is reported as a WARNING instead, so this check cannot turn a
# shared tree red under someone else's half-finished file -- it starts
# enforcing the moment that file is committed.
#
# KNOWN-DEFECTIVE GRACE LIST: bridge modules that landed with `>=` while this
# review was in flight and are owned by other, concurrent passes; they are
# listed below so this check does not turn a shared tree red under work this
# review was not allowed to edit.
# The list is SELF-RETIRING and cannot go vacuous: a grace-listed file that
# no longer contains the pattern is itself a FAILURE telling you to delete
# its entry, so the exemption disappears the moment the defect is fixed
# rather than quietly outliving it.

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$bridgeDir = Join-Path $repoRoot 'firmware\KilnFW\App\drivers\persist'

if (-not (Test-Path $bridgeDir)) {
    Write-Host "FAIL: bridge directory not found: $bridgeDir"
    exit 1
}

$bridges = @(Get-ChildItem -Path $bridgeDir -Filter '*_cfg_fs.c' -File |
             Where-Object { $_.Name -ne 'cfg_fs.c' })

# Positive presence assertion, so a rename/split that moves these modules
# does not leave this check silently passing with zero coverage (the
# `if not path.is_file(): skipTest(...)` failure mode CLAUDE.md calls out).
if ($bridges.Count -lt 1) {
    Write-Host "FAIL: no *_cfg_fs.c dual-write bridge modules found under $bridgeDir."
    Write-Host "      If these were renamed or moved, update this check's path/pattern --"
    Write-Host "      do not delete it; the tie-break invariant still applies wherever they live."
    exit 1
}

# Which of them does git track? Untracked ones only warn (see header).
$tracked = @{}
Push-Location $repoRoot
try {
    $lsOut = & git ls-files -- 'firmware/KilnFW/App/drivers/persist/*_cfg_fs.c' 2>$null
    foreach ($line in $lsOut) {
        if ($line) { $tracked[[System.IO.Path]::GetFileName($line)] = $true }
    }
} finally {
    Pop-Location
}

# `>=` (or the mirrored `<=`) between the two rev variables, in either
# order. Whitespace-tolerant; comments are stripped first so the prose
# explaining WHY `>=` is wrong does not trip the check on itself.
$badPattern = '(file_rev\s*>=\s*nvs_rev)|(nvs_rev\s*<=\s*file_rev)'

# filename -> the pass that owns fixing it. Delete an entry the moment its
# file is fixed; the "grace entry is stale" failure below enforces that.
$graceList = @{
    'pref_cfg_fs.c'     = 'prefs move (FILESYSTEM_USER_DATA_PLAN.md section 5 step 3)'
}

$failures = @()
$warnings = @()
$scanned  = 0
$graceHit = @{}

foreach ($f in $bridges) {
    $scanned++
    $raw = Get-Content -Raw -LiteralPath $f.FullName

    # Strip /* ... */ and // ... comments before matching.
    $code = [regex]::Replace($raw, '/\*.*?\*/', '', 'Singleline')
    $code = [regex]::Replace($code, '//[^\r\n]*', '')

    $lineNo = 0
    foreach ($line in ($code -split "`r?`n")) {
        $lineNo++
        if ($line -match $badPattern) {
            $msg = "$($f.Name): line $lineNo of the comment-stripped source compares the dual-write revs " +
                   "with '>=' -- the file must win only on a STRICTLY higher rev (use 'file_rev > nvs_rev'). " +
                   "An equal rev with differing bytes means the NVS copy is the newer one; see this check's " +
                   "header and docs/audits/filesystem_migration_review_2026-09-07.md."
            if ($graceList.ContainsKey($f.Name)) {
                $graceHit[$f.Name] = $true
                $warnings += "$msg  [KNOWN, GRACE-LISTED -- owned by the $($graceList[$f.Name]); " +
                             "fix it there, then delete its entry from this check's `$graceList]"
            } elseif ($tracked.ContainsKey($f.Name)) {
                $failures += $msg
            } else {
                $warnings += "$msg  [UNTRACKED working-tree file -- warning only until it is committed]"
            }
        }
    }
}

foreach ($w in $warnings) { Write-Host "WARN: $w" }

# SELF-RETIRING: a grace entry whose file is present and no longer defective
# is itself a failure -- that is what keeps this exemption from silently
# outliving the defect it was granted for.
foreach ($name in $graceList.Keys) {
    $present = @($bridges | Where-Object { $_.Name -eq $name }).Count -gt 0
    if ($present -and -not $graceHit.ContainsKey($name)) {
        $failures += ("$name is on this check's `$graceList but no longer uses '>=' -- the defect it was " +
                      "exempted for is fixed. DELETE its `$graceList entry so this check enforces the " +
                      "invariant on it from now on.")
    }
}

if ($failures.Count -gt 0) {
    foreach ($e in $failures) { Write-Host "FAIL: $e" }
    exit 1
}

$trackedScanned = @($bridges | Where-Object { $tracked.ContainsKey($_.Name) }).Count
Write-Host ("PASS: cfg_fs dual-write tie-break is strict (> not >=) in every tracked bridge module " +
            "($scanned scanned, $trackedScanned tracked).")
if ($warnings.Count -gt 0) {
    Write-Host "      ($($warnings.Count) warning(s) above on untracked WIP files.)"
}
exit 0
