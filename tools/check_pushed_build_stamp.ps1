# check_pushed_build_stamp.ps1 -- unit test for tools/pushed_build_stamp.ps1,
# the result-reuse logic behind check_01_{kilnfw,saftyfw}_pushed_build.ps1.
# No board, no build, no network: temp stamp dir + random fake shas.
#
# The assertion that matters most is "a stamp for sha A is NEVER reused for
# sha B" (and likewise for a FAIL, corrupt, wrong-schema or case-folded
# stamp): a reused PASS for the wrong sha would silently green an unbuilt
# origin/main. Negative-tested by hand 2026-10-07: changing
# `$s.sha -cne $Sha` to `$false` in Read-PushedBuildStamp makes case 3 fail.

$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "build_lock.ps1")
. (Join-Path $PSScriptRoot "pushed_build_stamp.ps1")

$script:fails = 0
function Assert([bool]$cond, [string]$what) {
    if ($cond) { Write-Host "  ok: $what" } else { Write-Host "  FAIL: $what" -ForegroundColor Red; $script:fails++ }
}
function New-FakeSha { -join ((1..40) | ForEach-Object { '0123456789abcdef'[(Get-Random -Maximum 16)] }) }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("pushed_stamp_test_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null
$env:KILNCTL_PUSHED_STAMP_DIR = $tmp
Remove-Item Env:KILNCTL_PUSHED_BUILD_FORCE -ErrorAction SilentlyContinue
try {
    $name = "unittest" + (Get-Random)
    $shaA = New-FakeSha
    $shaB = New-FakeSha
    $path = Get-PushedBuildStampPath -Name $name

    Write-Host "case 1: no stamp -> must build, lock held"
    $s1 = Enter-PushedBuildSlot -Name $name -Sha $shaA
    Assert (-not $s1.Reused) "not reused with no stamp"
    Assert ($null -ne $s1.Lock) "lock returned"

    Write-Host "case 2: PASS stamp for A -> A reuses"
    Write-PushedBuildStamp -Path $path -Sha $shaA -Detail "unit"
    Exit-PushedBuildSlot -Slot $s1
    $s2 = Enter-PushedBuildSlot -Name $name -Sha $shaA
    Assert ($s2.Reused) "same sha reused"
    Assert ($s2.Stamp.sha -ceq $shaA) "reused stamp names the sha"

    Write-Host "case 3: stamp for A must NOT be reused for B (the key negative)"
    Assert ($null -eq (Read-PushedBuildStamp -Path $path -Sha $shaB)) "different sha not reused"
    $s3 = Enter-PushedBuildSlot -Name $name -Sha $shaB
    Assert (-not $s3.Reused) "Enter for different sha builds"
    Exit-PushedBuildSlot -Slot $s3
    Assert ($null -eq (Read-PushedBuildStamp -Path $path -Sha $shaA.Substring(0, 39))) "short/prefix sha not reused"
    Assert ($null -eq (Read-PushedBuildStamp -Path $path -Sha $shaA.ToUpper()) -or $shaA -ceq $shaA.ToUpper()) "case-folded sha not reused"

    Write-Host "case 4: unusable stamps are ignored"
    Set-Content -LiteralPath $path -Value "{ not json" -Encoding UTF8
    Assert ($null -eq (Read-PushedBuildStamp -Path $path -Sha $shaA)) "corrupt JSON ignored"
    Set-Content -LiteralPath $path -Value ('{"schema":1,"sha":"' + $shaA + '","result":"FAIL"}') -Encoding UTF8
    Assert ($null -eq (Read-PushedBuildStamp -Path $path -Sha $shaA)) "FAIL stamp never reused"
    Set-Content -LiteralPath $path -Value ('{"schema":2,"sha":"' + $shaA + '","result":"PASS"}') -Encoding UTF8
    Assert ($null -eq (Read-PushedBuildStamp -Path $path -Sha $shaA)) "unknown schema ignored"
    Write-PushedBuildStamp -Path $path -Sha $shaA -Detail "unit"
    Assert ($null -ne (Read-PushedBuildStamp -Path $path -Sha $shaA)) "valid stamp read back"
    $env:KILNCTL_PUSHED_BUILD_FORCE = "1"
    Assert ($null -eq (Read-PushedBuildStamp -Path $path -Sha $shaA)) "FORCE=1 ignores stamp"
    Remove-Item Env:KILNCTL_PUSHED_BUILD_FORCE
    Assert ($null -eq (Read-PushedBuildStamp -Path $path -Sha "not-a-sha")) "malformed caller sha never matches"

    Write-Host "case 5: a waiter stops waiting when the holder's stamp appears"
    Remove-Item -LiteralPath $path -Force
    $shaC = New-FakeSha
    $holder = Enter-PushedBuildSlot -Name $name -Sha $shaC
    Assert (-not $holder.Reused) "holder owns the build"
    $repo = $PSScriptRoot
    $job = Start-Job -ScriptBlock {
        param($tools, $dir, $n, $sha)
        $env:KILNCTL_PUSHED_STAMP_DIR = $dir
        . (Join-Path $tools "build_lock.ps1")
        . (Join-Path $tools "pushed_build_stamp.ps1")
        $r = Enter-PushedBuildSlot -Name $n -Sha $sha -PollSeconds 1 -TimeoutSeconds 60
        [PSCustomObject]@{ Reused = $r.Reused; Sha = $r.Stamp.sha }
    } -ArgumentList $repo, $tmp, $name, $shaC
    Start-Sleep -Seconds 4
    Assert ($job.State -eq "Running") "waiter still waiting while no stamp exists (lock held)"
    Write-PushedBuildStamp -Path $path -Sha $shaC -Detail "unit"
    $done = Wait-Job $job -Timeout 30
    Assert ($null -ne $done) "waiter returned within 30s while the holder's lock is STILL held"
    if ($done) {
        $res = Receive-Job $job
        Assert ($res.Reused -eq $true -and $res.Sha -ceq $shaC) "waiter reused holder's result for the same sha"
    }
    Remove-Job $job -Force
    Exit-PushedBuildSlot -Slot $holder

    Write-Host "case 6: both pushed-build checks are wired to the helper"
    $kiln = Get-Content -Raw (Join-Path $PSScriptRoot "..\firmware\KilnFW\App\test\check_01_kilnfw_pushed_build.ps1")
    $safe = Get-Content -Raw (Join-Path $PSScriptRoot "..\firmware\SaftyFW\test\check_01_saftyfw_pushed_build.ps1")
    foreach ($pair in @(@("kilnfw", $kiln), @("saftyfw", $safe))) {
        $t = $pair[1]
        Assert ($t -match 'Enter-PushedBuildSlot' -and $t -match 'Write-PushedBuildStamp' -and $t -match 'Exit-PushedBuildSlot') "$($pair[0]) check uses enter/write/exit"
        Assert ($t -match 'builds .* cleanly\. \[built\]') "$($pair[0]) prints [built] on a real build"
    }
} finally {
    Remove-Item Env:KILNCTL_PUSHED_STAMP_DIR -ErrorAction SilentlyContinue
    Remove-Item Env:KILNCTL_PUSHED_BUILD_FORCE -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:fails -gt 0) { Write-Host "FAILED: $($script:fails) assertion(s)" -ForegroundColor Red; exit 1 }
Write-Host "PASS: pushed-build stamp reuse logic" -ForegroundColor Green
exit 0
