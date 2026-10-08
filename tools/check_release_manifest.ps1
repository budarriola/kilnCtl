# check_release_manifest.ps1 -- standing guard for the release tooling
# (tools/release_manifest.py, tools/make_release.ps1; GITHUB_RELEASE_UPDATE_PLAN.md
# sections 6-8). Nothing here touches a board, the network, or GitHub.
#
#   1. tools/PcTools/tests/test_release_manifest.py: parsers, semver gate, dirty-tree
#      refusal, 0x400000 size gate, generate+validate round trip on synthetic files (also
#      end to end through the CLI against a throwaway git repo), tamper detection.
#   2. make_release.ps1 refuses a non-semver tag.
#   2b. -Publish is refused with -SkipBuild/-BuildDir/-RecoveryBin (provenance).
#   2b2. Release gates (tools/release_gates.py, docs/release_gates.json): the
#      tools/PcTools/tests/test_release_gates.py unit tests run; the tracked gates file
#      validates; a stable-tag -Publish against an open-gates file is refused with "release
#      gates not satisfied" (the gate check runs before every git/build step, so this
#      never reaches a build); a missing -NotesFile is refused.
#   2c. The real Get-AssetToFile (HttpClient, manual redirect) against a local 127.0.0.1
#      server: token required on hop 1, forbidden on hop 2, bytes match; a 404 on hop 2 throws.
#   3. The -Publish REST flow, with Invoke-RestMethod / Get-AssetToFile shadowed by
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

# 1b. signer (tools/sign_release.py, WP11): keys generated at test time, never committed.
$signTest = Join-Path $repoRoot "tools\PcTools\tests\test_sign_release.py"
$ErrorActionPreference = "Continue"
$out = & $python -m unittest $signTest 2>&1 | Out-String
$sgt = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($sgt -ne 0) { Note-Fail "test_sign_release.py failed:`n$out" }
elseif ($out -notmatch 'Ran (\d+) tests' -or [int]$Matches[1] -lt 9) { Note-Fail "test_sign_release.py ran too few tests (vacuous?):`n$out" }
else { Write-Host "ok: $($Matches[0]) (release signer)" }

# 2. bad tag refused -------------------------------------------------------------
# Require the semver refusal text too: a bare exit 1 is also what any later gate (dirty tree, build, ...)
# returns, so with the semver gate removed this passed anyway (vacuity audit 2026-10-07).
$o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "make_release.ps1") -Tag "1.0" 2>&1 | Out-String
if ($LASTEXITCODE -ne 1 -or $o -notmatch "is not semver") { Note-Fail "make_release.ps1 -Tag 1.0 exited $LASTEXITCODE without the 'is not semver' refusal, expected 1 with it:`n$o" }
else { Write-Host "ok: non-semver tag refused" }

# 2b. provenance refusal ------------------------------------------------------------
foreach ($extra in @(@("-SkipBuild"), @("-BuildDir", "C:\nonexistent"), @("-RecoveryBin", "C:\nonexistent.bin"))) {
    $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "make_release.ps1") -Tag "v1.0.0" -Publish @extra 2>&1 | Out-String
    if ($LASTEXITCODE -ne 1 -or $o -notmatch "cannot be combined") { Note-Fail "make_release.ps1 -Publish $($extra -join ' ') was not refused for provenance (exit $LASTEXITCODE)" }
    else { Write-Host "ok: -Publish refused with $($extra[0])" }
}

# 2b2. release gates and notes ---------------------------------------------------------
$gatesTest = Join-Path $repoRoot "tools\PcTools\tests\test_release_gates.py"
$ErrorActionPreference = "Continue"
$out = & $python -m unittest $gatesTest 2>&1 | Out-String
$gt = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($gt -ne 0) { Note-Fail "test_release_gates.py failed:`n$out" }
elseif ($out -notmatch 'Ran (\d+) tests' -or [int]$Matches[1] -lt 10) { Note-Fail "test_release_gates.py ran too few tests (vacuous?):`n$out" }
else { Write-Host "ok: $($Matches[0]) (release gates)" }

$realGates = Join-Path $repoRoot "docs\release_gates.json"
$ErrorActionPreference = "Continue"
$o = & $python (Join-Path $PSScriptRoot "release_gates.py") status --file $realGates 2>&1 | Out-String
$st = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($st -ne 0 -or $o -notmatch 'Release gates: \d+ of \d+ pass') { Note-Fail "docs/release_gates.json did not validate (exit $st):`n$o" }
else { Write-Host "ok: docs/release_gates.json validates" }

$openGates = Join-Path ([System.IO.Path]::GetTempPath()) ("relgates_" + [guid]::NewGuid().ToString("N") + ".json")
try {
    '{"schema":1,"gates":[{"id":"x","title":"open one","status":"open","evidence":"","source":"s"}]}' | Set-Content -LiteralPath $openGates -Encoding ascii
    $ErrorActionPreference = "Continue"
    $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "make_release.ps1") -Tag "v1.0.0" -Publish -GatesFile $openGates 2>&1 | Out-String
    $rc = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($rc -ne 1 -or $o -notmatch "release gates not satisfied" -or $o -notmatch "OPEN GATES") { Note-Fail "stable -Publish with an open gate was not refused for gates (exit $rc):`n$o" }
    else { Write-Host "ok: stable -Publish refused while a gate is open" }
} finally { Remove-Item -LiteralPath $openGates -Force -ErrorAction SilentlyContinue }

$ErrorActionPreference = "Continue"
$o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "make_release.ps1") -Tag "v1.0.0" -NotesFile XX 2>&1 | Out-String
$rc = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($rc -ne 1 -or $o -notmatch "-NotesFile .* does not exist") { Note-Fail "missing -NotesFile was not refused (exit $rc)" }
else { Write-Host "ok: missing -NotesFile refused" }

# 2c. real Get-AssetToFile against a local redirect server ------------------------
. (Join-Path $PSScriptRoot "make_release.ps1") -LoadFunctionsOnly
$srvPy = Join-Path ([System.IO.Path]::GetTempPath()) ("relsrv_" + [guid]::NewGuid().ToString("N") + ".py")
$srvOut = $srvPy + ".out"
@'
import http.server
PAYLOAD = bytes(range(256)) * 4096
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        auth = self.headers.get("Authorization")
        port = self.server.server_address[1]
        if self.path in ("/asset", "/asset404"):
            if auth != "Bearer TESTTOKEN" or self.headers.get("Accept") != "application/octet-stream":
                self.send_response(401); self.send_header("Content-Length", "0"); self.end_headers(); return
            self.send_response(302)
            self.send_header("Location", "http://127.0.0.1:%d/%s" % (port, "blob" if self.path == "/asset" else "missing"))
            self.send_header("Content-Length", "0"); self.end_headers(); return
        if self.path == "/blob":
            if auth is not None:
                self.send_response(403); self.send_header("Content-Length", "0"); self.end_headers(); return
            self.send_response(200); self.send_header("Content-Length", str(len(PAYLOAD))); self.end_headers()
            self.wfile.write(PAYLOAD); return
        self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers()
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
'@ | Set-Content -LiteralPath $srvPy -Encoding ascii
$srv = Start-Process -FilePath $python -ArgumentList @($srvPy) -RedirectStandardOutput $srvOut -PassThru -WindowStyle Hidden
try {
    $port = $null
    for ($i = 0; $i -lt 100 -and -not $port; $i++) {
        Start-Sleep -Milliseconds 100
        if (Test-Path $srvOut) { $line = (Get-Content -LiteralPath $srvOut -ErrorAction SilentlyContinue | Select-Object -First 1); if ($line -match '^\d+$') { $port = $line } }
    }
    if (-not $port) { Note-Fail "local redirect server did not start" }
    else {
        $dl = Join-Path ([System.IO.Path]::GetTempPath()) ("reldl_" + [guid]::NewGuid().ToString("N"))
        try {
            Get-AssetToFile "TESTTOKEN" "http://127.0.0.1:$port/asset" $dl
            $gotLen = (Get-Item -LiteralPath $dl).Length
            if ($gotLen -ne 256 * 4096) { Note-Fail "redirect download wrote $gotLen bytes, expected $(256 * 4096)" }
            else { Write-Host "ok: Get-AssetToFile follows 302 with token only on hop 1, streams the body" }
        } catch { Note-Fail "Get-AssetToFile against local redirect server threw: $($_.Exception.Message)" }
        finally { Remove-Item -LiteralPath $dl -Force -ErrorAction SilentlyContinue }
        $dl404 = Join-Path ([System.IO.Path]::GetTempPath()) ("reldl404_" + [guid]::NewGuid().ToString("N"))
        $threw = $false
        try { Get-AssetToFile "TESTTOKEN" "http://127.0.0.1:$port/asset404" $dl404 } catch { $threw = $true }
        if (-not $threw) { Note-Fail "a 404 on the redirect target did not throw" } else { Write-Host "ok: failing redirect target throws" }
        Remove-Item -LiteralPath $dl404 -Force -ErrorAction SilentlyContinue
    }
} finally {
    if ($srv -and -not $srv.HasExited) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $srvPy, $srvOut -Force -ErrorAction SilentlyContinue
}

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
    function Get-AssetToFile([string]$Token, [string]$AssetApiUrl, [string]$OutFile) {
        $script:calls.Add("GETASSET $AssetApiUrl")
        $name = $AssetApiUrl -replace '.*/asset/', ''
        $bytes = [System.IO.File]::ReadAllBytes((Join-Path $work $name))
        if ($script:corrupt -and $name -eq "b.bin") { $bytes[0] = 255 }
        [System.IO.File]::WriteAllBytes($OutFile, $bytes)
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
