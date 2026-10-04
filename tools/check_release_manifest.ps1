# check_release_manifest.ps1 -- standing guard for the release tooling
# (tools/release_manifest.py, tools/make_release.ps1; GITHUB_RELEASE_UPDATE_PLAN.md
# sections 6-8). Nothing here touches a board, the network, or GitHub.
#
#   1. tools/PcTools/tests/test_release_manifest.py: parsers, semver gate, dirty-tree
#      refusal, 4 MB size gate, generate+validate round trip on synthetic files (also
#      end to end through the CLI against a throwaway git repo), tamper detection.
#   2. make_release.ps1 refuses a non-semver tag.
#   3. The -Publish REST flow, with Invoke-RestMethod / Invoke-WebRequest shadowed by
#      recording functions: order is create-draft, upload each asset, verify each, and
#      only then PATCH draft=false; a corrupted re-download must raise and must NOT
#      PATCH; the token must never appear in output; -WhatIf must call nothing.
#
# Exit 0 pass, 1 fail. Negative-tested when added (see docs/RELEASING.md).
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$failures = New-Object System.Collections.Generic.List[string]
function Note-Fail([string]$m) { $failures.Add($m); Write-Host "FAIL: $m" -ForegroundColor Red }

$python = $null
$venvPython = Join-Path $repoRoot "tools\PcTools\.venv\Scripts\python.exe"
if ((Test-Path $venvPython) -and (Test-Path (Join-Path $repoRoot "tools\PcTools\.venv\pyvenv.cfg"))) {
    $python = $venvPython
} else {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $cmd) { $cmd = Get-Command python3 -ErrorAction SilentlyContinue }
    if (-not $cmd) { Write-Host "FAIL: no venv python and no python on PATH"; exit 1 }
    $python = $cmd.Source
}

# 1. unit tests ------------------------------------------------------------------
$testFile = Join-Path $repoRoot "tools\PcTools\tests\test_release_manifest.py"
$ErrorActionPreference = "Continue"
$out = & $python -m unittest $testFile 2>&1 | Out-String
$ut = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($ut -ne 0) { Note-Fail "test_release_manifest.py failed:`n$out" }
elseif ($out -notmatch 'Ran (\d+) tests' -or [int]$Matches[1] -lt 15) { Note-Fail "test_release_manifest.py ran too few tests (vacuous?):`n$out" }
else { Write-Host "ok: $($Matches[0])" }

# 2. bad tag refused -------------------------------------------------------------
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "make_release.ps1") -Tag "1.0" | Out-Null
if ($LASTEXITCODE -ne 1) { Note-Fail "make_release.ps1 -Tag 1.0 exited $LASTEXITCODE, expected 1 (refusal)" }
else { Write-Host "ok: non-semver tag refused" }

# 3. publish flow with mocked REST -----------------------------------------------
. (Join-Path $PSScriptRoot "make_release.ps1") -LoadFunctionsOnly

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("relchk_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    [System.IO.File]::WriteAllBytes((Join-Path $work "a.bin"), [byte[]](1..200))
    [System.IO.File]::WriteAllBytes((Join-Path $work "b.bin"), [byte[]](5..99))
    $script:calls = New-Object System.Collections.Generic.List[string]
    $script:corrupt = $false
    $secret = "SECRET-TOKEN-XYZ-12345"

    function Invoke-RestMethod {
        param($Method, $Uri, $Headers, $Body, $ContentType, $InFile)
        $script:calls.Add("$Method $Uri")
        if ($Method -eq "Post" -and $Uri -match '/releases$') {
            $b = $Body | ConvertFrom-Json
            if (-not $b.draft) { throw "mock: release must be created as draft" }
            if ($b.target_commitish -ne ("c" * 40)) { throw "mock: target_commitish missing" }
            return [pscustomobject]@{ id = 42 }
        }
        if ($Method -eq "Post") { return [pscustomobject]@{ url = "https://api.github.com/asset/" + ($Uri -replace '.*name=', '') } }
        if ($Method -eq "Patch") {
            if (($Body | ConvertFrom-Json).draft -ne $false) { throw "mock: PATCH must set draft=false" }
            return $null
        }
    }
    function Invoke-WebRequest {
        param($Uri, $Headers, $OutFile, $MaximumRedirection, [switch]$UseBasicParsing)
        $script:calls.Add("GETASSET $Uri")
        $name = $Uri -replace '.*/asset/', ''
        $bytes = [System.IO.File]::ReadAllBytes((Join-Path $work $name))
        if ($script:corrupt -and $name -eq "b.bin") { $bytes[0] = 255 }
        return [pscustomobject]@{ Content = $bytes }
    }

    $args1 = @{ Token = $secret; Repo = "o/r"; Tag = "v1.0.0"; Commit = ("c" * 40); Dir = $work }

    $o = & { Invoke-ReleasePublish @args1 } *>&1 | Out-String
    $seq = ($script:calls -join "|")
    $want = "Post https://api.github.com/repos/o/r/releases|Post https://uploads.github.com/repos/o/r/releases/42/assets?name=a.bin|GETASSET https://api.github.com/asset/a.bin|Post https://uploads.github.com/repos/o/r/releases/42/assets?name=b.bin|GETASSET https://api.github.com/asset/b.bin|Patch https://api.github.com/repos/o/r/releases/42"
    if ($seq -ne $want) { Note-Fail "publish call order wrong:`n got:  $seq`n want: $want" }
    elseif ($o -match [regex]::Escape($secret)) { Note-Fail "token leaked into publish output" }
    else { Write-Host "ok: publish order create/upload/verify/PATCH-last, token not printed" }

    $script:calls.Clear(); $script:corrupt = $true
    $threw = $false
    try { & { Invoke-ReleasePublish @args1 } *>&1 | Out-Null } catch { $threw = $true }
    if (-not $threw) { Note-Fail "corrupted re-download did not raise" }
    elseif (($script:calls -join "|") -match "Patch") { Note-Fail "release was published despite a sha256 mismatch" }
    else { Write-Host "ok: sha256 mismatch aborts before draft=false" }

    $script:calls.Clear(); $script:corrupt = $false
    & { Invoke-ReleasePublish @args1 -WhatIf } *>&1 | Out-Null
    if ($script:calls.Count -ne 0) { Note-Fail "-WhatIf made $($script:calls.Count) REST call(s)" }
    else { Write-Host "ok: -WhatIf calls nothing" }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) { Write-Host "check_release_manifest.ps1: $($failures.Count) failure(s)" -ForegroundColor Red; exit 1 }
Write-Host "PASS: release manifest tooling"
exit 0
