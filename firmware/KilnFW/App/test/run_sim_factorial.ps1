# run_sim_factorial.ps1 -- builds and runs sim_factorial_driver.exe (the
# factorial CELL DRIVER, docs/audits/scenario_factorial_driver_2026-09-14.md).
#
# Deliberately NOT wired into build_host_tests.ps1: 263 cells x 3 arms is
# real compute the ~60s CI suite must not inherit (task requirement: "a
# separate --suite factorial target ... do not inflate CI"). Run this
# manually. Always pass a private -OutDir -- the shared App/test/build is
# under contention from concurrent sessions/agents.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File App\test\run_sim_factorial.ps1 [-OutDir <path>] [-Shards 4] [-SkipDeterminism]
param(
    [string]$OutDir = (Join-Path $env:TEMP "kilnctl_sim_factorial_build"),
    [int]$Shards = 4,
    [switch]$SkipDeterminism
)
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars -- update this path if MSVC Build Tools moved/were reinstalled."
}

$testDir = $PSScriptRoot
$driversDir = Join-Path $testDir "..\drivers"
$hwAbsDir = Join-Path $testDir "..\..\..\hwAbstraction"
$stubDir = Join-Path $testDir "stubs"
$commonInc = Join-Path $testDir "..\..\..\CommonFW\include"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

. (Join-Path $PSScriptRoot "../../../../tools/build_lock.ps1")
$buildLockName = "kilnfw_sim_factorial_" + ([System.Text.RegularExpressions.Regex]::Replace($OutDir, '[^A-Za-z0-9]+', '_'))
$buildLock = Enter-BuildLock -Name $buildLockName
try {
    $rsp = Join-Path $OutDir "sim_factorial_flags.rsp"
    $rspLines = @(
        "/nologo", "/W3", "/EHsc",
        "/I`"$stubDir`"", "/I`"$commonInc`"", "/I`"$driversDir`"",
        "/I`"$driversDir\bridge`"", "/I`"$driversDir\common`"", "/I`"$driversDir\control`"",
        "/I`"$driversDir\http`"", "/I`"$driversDir\hw`"", "/I`"$driversDir\net`"",
        "/I`"$driversDir\owners`"", "/I`"$driversDir\persist`"", "/I`"$driversDir\safety`"",
        "/I`"$driversDir\sim`"", "/I`"$driversDir\ui`"",
        "/I`"$hwAbsDir\esp\spi`"", "/I`"$hwAbsDir\esp\i2c`"", "/I`"$hwAbsDir\esp\uart`"",
        "/I`"$hwAbsDir\interface`"", "/I`"$hwAbsDir\host`"", "/I`"$hwAbsDir\esp\common`""
    )
    [System.IO.File]::WriteAllText($rsp, ($rspLines -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

    $exe = Join-Path $OutDir "kilnctl_sim_factorial_driver.exe"
    # The last eight entries below arrived with ADAPTIVE_FUZZY_EVALUATION_PLAN.md
    # sec 5's two adaptive arms: sim_factorial_driver.c now #includes
    # adaptive_tune.c/_model.c/_ki.c directly into its own TU (same one-TU
    # convention sim_scenarios_adaptive.c and test_adaptive_tune.c use, so
    # those three are NOT listed here), which drags in that module's much
    # larger link surface -- zone_coupling_solve, the cfg_fs/pref_cfg_fs
    # persistence pair, flash_worker_wait, hal_kv's host fake and hal_status.
    # pid_fuzzy_confidence.c is the sec 3 confidence gate, linked for real
    # rather than mirrored.
    $sourceArgs = "`"$(Join-Path $testDir 'sim_factorial_driver.c')`" `"$(Join-Path $testDir 'sim_factorial_design.c')`" " +
            "`"$(Join-Path $testDir 'sim_plant.c')`" `"$(Join-Path $testDir 'sim_high_temp.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid.c')`" `"$(Join-Path $driversDir 'control/pid_fuzzy.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid_autotune.c')`" `"$(Join-Path $driversDir 'control/firing_score.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid_fuzzy_confidence.c')`" " +
            "`"$(Join-Path $driversDir 'control/zone_coupling_solve.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs.c')`" `"$(Join-Path $driversDir 'persist/pref_cfg_fs.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs_status.c')`" " +
            "`"$(Join-Path $driversDir 'persist/flash_worker_wait.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""
    $cmd = "call `"$vcvars`" x64 >nul && cl @`"$rsp`" /std:c11 /Fo:`"$OutDir\\`" /Fe:`"$exe`" $sourceArgs"

    Write-Host "=== building sim_factorial_driver.exe into $OutDir ==="
    if (Test-Path $exe) { Remove-Item $exe -Force }
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) {
        throw "sim_factorial_driver.exe build failed (exit $LASTEXITCODE)."
    }

    Write-Host "=== --of 1 canonical run ==="
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $of1Raw = & $exe --shard 0 --of 1
    $sw.Stop()
    if ($LASTEXITCODE -ne 0) { throw "sim_factorial_driver --of 1 exited $LASTEXITCODE." }
    $of1Raw | Out-File -FilePath (Join-Path $OutDir "factorial_of1.tsv") -Encoding utf8
    Write-Host ("--of 1 runtime: {0:N1} s" -f $sw.Elapsed.TotalSeconds)

    if ($SkipDeterminism) {
        Write-Host "SkipDeterminism set -- not running the sharded comparison."
        exit 0
    }

    function Get-DataRows($rawLines) {
        $rows = @()
        foreach ($line in $rawLines) {
            if ($line -match '^(ST\d[\w-]*)\t\d+\t.*\t(A_\S+)\t') {
                $rows += [PSCustomObject]@{ CellId = $Matches[1]; Arm = $Matches[2]; Line = $line }
            }
            # The adaptive arms' sec 7 instrumentation rides on its own line
            # shape (see sim_factorial_driver.c's ADAPTIVE_DIAG printf). It is
            # compared here too: it carries the per-firing state that decides
            # whether an adaptive arm did anything at all, so leaving it out of
            # the determinism check would let the arms' behaviour diverge
            # between --of 1 and --of N without failing anything.
            elseif ($line -match '^ADAPTIVE_DIAG\t(ST\d[\w-]*)\t(A_\S+)\t') {
                $rows += [PSCustomObject]@{ CellId = $Matches[1]; Arm = "ZZDIAG_" + $Matches[2]; Line = $line }
            }
        }
        return $rows | Sort-Object CellId, Arm | ForEach-Object { $_.Line }
    }
    $of1Rows = Get-DataRows $of1Raw
    Write-Host "  --of 1: $($of1Rows.Count) data rows"

    Write-Host "=== --of $Shards sharded run ==="
    $shardOutFiles = @()
    $procs = @()
    for ($i = 0; $i -lt $Shards; $i++) {
        $outFile = Join-Path $OutDir "shard_$i.out.txt"
        $errFile = Join-Path $OutDir "shard_$i.err.txt"
        $shardOutFiles += $outFile
        $p = Start-Process -FilePath $exe -ArgumentList @("--shard", "$i", "--of", "$Shards") `
                -NoNewWindow -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
        $handle = $p.Handle
        $procs += $p
    }
    $procs | ForEach-Object { $_.WaitForExit() }
    for ($i = 0; $i -lt $Shards; $i++) {
        if ($procs[$i].ExitCode -ne 0) {
            Write-Host "shard $i exited $($procs[$i].ExitCode):"
            Get-Content $shardOutFiles[$i]
            throw "sim_factorial_driver shard $i/$Shards failed."
        }
    }
    $ofNRaw = @()
    foreach ($f in $shardOutFiles) { $ofNRaw += Get-Content $f }
    $ofNRows = Get-DataRows $ofNRaw
    Write-Host "  --of $Shards (aggregated): $($ofNRows.Count) data rows"

    Write-Host "=== byte-identical comparison ==="
    if ($of1Rows.Count -ne $ofNRows.Count) {
        Write-Host "FAIL: row count differs -- of1=$($of1Rows.Count) ofN=$($ofNRows.Count)"
        exit 1
    }
    $diffCount = 0
    for ($i = 0; $i -lt $of1Rows.Count; $i++) {
        if ($of1Rows[$i] -cne $ofNRows[$i]) {
            $diffCount++
            if ($diffCount -le 10) {
                Write-Host "DIFF at row $i :"
                Write-Host "  of1: $($of1Rows[$i])"
                Write-Host "  ofN: $($ofNRows[$i])"
            }
        }
    }
    if ($diffCount -gt 0) {
        Write-Host "FAIL: $diffCount of $($of1Rows.Count) rows differ between --of 1 and --of $Shards."
        exit 1
    }
    Write-Host "PASS: --of 1 and --of $Shards data rows are byte-identical ($($of1Rows.Count) rows compared)."
    exit 0
} finally {
    Exit-BuildLock -Lock $buildLock
}
