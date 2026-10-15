# check_sdkconfig_pin_guards.ps1 -- REVIEW_XIPLAND MED-1.
#
# A stale generated sdkconfig (XIP_FROM_PSRAM / UART_ISR_IN_IRAM disagreeing with
# sdkconfig.defaults) used to flow silently into target and release builds: the pin check only
# WARNed when the worktree had no sdkconfig, and check_00 copied the main-tree one in unchecked.
# This proves the guards behave, with fixtures only (no build, no board):
#   1. the pin check fails on a stale explicit sdkconfig;
#   2. with no worktree sdkconfig it falls back to build\sdkconfig, then to the main-tree
#      sdkconfig (KILNCTL_MAIN_REPO_ROOT), failing on a stale one, passing on a good one, and
#      only WARN-passing when none exists;
#   3. check_00 verifies the pins on the file it copies BEFORE the copy, and make_release
#      verifies before and after its build.
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$repo = Resolve-Path (Join-Path $here "..\..\..\..")
$pinSrc = Join-Path $here "check_sdkconfig_defaults_applied.ps1"
$failures = New-Object System.Collections.Generic.List[string]
function Expect([string]$what, [bool]$cond) {
    if ($cond) { Write-Host "ok   $what" } else { Write-Host "FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("sdkpin_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $defaults = "CONFIG_SPIRAM_XIP_FROM_PSRAM=y`n# CONFIG_UART_ISR_IN_IRAM is not set`n"
    $good = $defaults + "CONFIG_OTHER=1`n"
    $stale = "# CONFIG_SPIRAM_XIP_FROM_PSRAM is not set`n# CONFIG_UART_ISR_IN_IRAM is not set`n"
    # Build a miniature tree so $PSScriptRoot-relative resolution works in the copied script.
    $tree = Join-Path $tmp "tree"
    $testDir = Join-Path $tree "firmware\KilnFW\App\test"
    New-Item -ItemType Directory -Path $testDir -Force | Out-Null
    # The check needs the real defaults' watched keys; start from the repo's file and only
    # replace nothing -- the fixtures below are sdkconfigs derived from it.
    $realDefaults = Join-Path $here "..\..\sdkconfig.defaults"
    Copy-Item -LiteralPath $realDefaults -Destination (Join-Path $tree "firmware\KilnFW\sdkconfig.defaults")
    Copy-Item -LiteralPath $pinSrc -Destination $testDir
    $pin = Join-Path $testDir "check_sdkconfig_defaults_applied.ps1"
    $tdefaults = Join-Path $tree "firmware\KilnFW\sdkconfig.defaults"

    # A generated sdkconfig that agrees with every pin: the defaults file itself is one.
    $goodCfg = Join-Path $tmp "good.sdkconfig"
    Copy-Item -LiteralPath $tdefaults -Destination $goodCfg
    # A stale one: same, but XIP flipped off.
    $staleCfg = Join-Path $tmp "stale.sdkconfig"
    (Get-Content -LiteralPath $tdefaults) | ForEach-Object {
        if ($_ -match '^CONFIG_SPIRAM_XIP_FROM_PSRAM=') { '# CONFIG_SPIRAM_XIP_FROM_PSRAM is not set' } else { $_ }
    } | Set-Content -LiteralPath $staleCfg

    function Run-Pin([string]$mainRoot, [string[]]$extra) {
        $ErrorActionPreference = 'Continue'  # native stderr must not become a terminating error
        $env:KILNCTL_MAIN_REPO_ROOT = $mainRoot
        $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $pin @extra 2>&1 | Out-String
        $script:lastOut = $out
        return $LASTEXITCODE
    }
    $nomain = Join-Path $tmp "nomain"

    Expect "explicit good sdkconfig passes" ((Run-Pin $nomain @("-SdkconfigPath", $goodCfg)) -eq 0)
    Expect "explicit stale sdkconfig fails" ((Run-Pin $nomain @("-SdkconfigPath", $staleCfg)) -ne 0)
    Expect "nothing anywhere: WARN and pass" ((Run-Pin $nomain @()) -eq 0 -and $script:lastOut -match "WARNING")

    # Main-tree fallback.
    $mainFw = Join-Path $tmp "main\firmware\KilnFW"
    New-Item -ItemType Directory -Path $mainFw -Force | Out-Null
    Copy-Item -LiteralPath $staleCfg -Destination (Join-Path $mainFw "sdkconfig")
    Expect "no worktree sdkconfig, stale main-tree sdkconfig fails" ((Run-Pin (Join-Path $tmp "main") @()) -ne 0)
    Copy-Item -LiteralPath $goodCfg -Destination (Join-Path $mainFw "sdkconfig") -Force
    Expect "no worktree sdkconfig, good main-tree sdkconfig passes" ((Run-Pin (Join-Path $tmp "main") @()) -eq 0 -and $script:lastOut -notmatch "WARNING")

    # build\sdkconfig fallback wins over the main tree.
    $buildDir = Join-Path $tree "firmware\KilnFW\build"
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
    Copy-Item -LiteralPath $staleCfg -Destination (Join-Path $buildDir "sdkconfig")
    Expect "stale build\sdkconfig fails even when the main tree is good" ((Run-Pin (Join-Path $tmp "main") @()) -ne 0)
} finally {
    Remove-Item Env:\KILNCTL_MAIN_REPO_ROOT -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

# Wiring: the consumers must run the pin check, check_00 BEFORE it copies the config in.
$c00 = Get-Content -Raw -LiteralPath (Join-Path $here "check_00_kilnfw_target_build.ps1")
$iPin = $c00.IndexOf("check_sdkconfig_defaults_applied.ps1")
$iCopy = $c00.IndexOf("Copy-Item -LiteralPath `$MainSdkconfig")
Expect "check_00 runs the pin check on `$MainSdkconfig before copying it" ($iPin -ge 0 -and $iCopy -gt $iPin -and $c00 -match '-SdkconfigPath \$MainSdkconfig')
$rel = Get-Content -Raw -LiteralPath (Join-Path $repo "tools\make_release.ps1")
Expect "make_release verifies the pins before the build" ($rel -match 'if \(Test-Path -LiteralPath \$preSdk\) \{ Assert-SdkconfigPins \$preSdk')
Expect "make_release verifies the pins on the published build/sdkconfig" ($rel -match 'if \(Test-Path -LiteralPath \$builtSdk\) \{ Assert-SdkconfigPins \$builtSdk')

if ($failures.Count -gt 0) { Write-Host "CHECK FAILED: $($failures.Count) case(s)" -ForegroundColor Red; exit 1 }
Write-Host "sdkconfig pin guards check passed"
exit 0
