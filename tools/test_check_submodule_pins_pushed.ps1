# Test for check_submodule_pins_pushed.ps1 using local scratch "remote" repos.
$ErrorActionPreference = 'Continue'
$chk = Join-Path $PSScriptRoot 'check_submodule_pins_pushed.ps1'
$root = Join-Path ([IO.Path]::GetTempPath()) ("subpinstest_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $root | Out-Null
$fails = 0
function G { & git @args 2>&1 | Out-Null }
try {
    $remote = Join-Path $root 'remote'; $local = Join-Path $root 'local'; $sup = Join-Path $root 'super'
    git init -q $remote 2>&1 | Out-Null
    G -C $remote config user.email t@t; G -C $remote config user.name t
    G -C $remote config uploadpack.allowAnySHA1InWant true
    G -C $remote commit -q --allow-empty -m one
    $old = (git -C $remote rev-parse HEAD).Trim()
    G -C $remote commit -q --allow-empty -m two
    $tip = (git -C $remote rev-parse HEAD).Trim()
    # a commit that exists only in a separate local clone (never pushed)
    git clone -q $remote $local 2>&1 | Out-Null
    G -C $local config user.email t@t; G -C $local config user.name t
    G -C $local commit -q --allow-empty -m unpushed
    $lost = (git -C $local rev-parse HEAD).Trim()

    git init -q $sup 2>&1 | Out-Null
    G -C $sup config user.email t@t; G -C $sup config user.name t
    Set-Content -Path (Join-Path $sup '.gitmodules') -Value "[submodule `"m`"]`n`tpath = m`n`turl = $($remote -replace [regex]::Escape('\'),'/')`n"
    G -C $sup add .gitmodules
    $cases = @(
        @{ n = 'tip'; sha = $tip; want = 0 },
        @{ n = 'reachable-not-tip'; sha = $old; want = 0 },
        @{ n = 'never-pushed'; sha = $lost; want = 1 })
    foreach ($c in $cases) {
        G -C $sup update-index --add --cacheinfo "160000,$($c.sha),m"
        G -C $sup commit -q -m $c.n
        $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $chk -RepoPath $sup 2>&1 | Out-String
        if ($LASTEXITCODE -ne $c.want) { Write-Host "FAIL: $($c.n): exit $LASTEXITCODE, want $($c.want)`n$out"; $fails++ }
        elseif ($c.want -eq 1 -and $out -notmatch 'push the submodule commit to its remote before landing') { Write-Host "FAIL: $($c.n): message missing"; $fails++ }
        else { Write-Host "ok: $($c.n)" }
    }
    function RunNew($name, $want, $gm, $rx) {
        Set-Content -Path (Join-Path $sup '.gitmodules') -Value $gm
        G -C $sup add .gitmodules; G -C $sup commit -q -m $name
        $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $chk -RepoPath $sup 2>&1 | Out-String
        if ($LASTEXITCODE -ne $want -or $out -match 'PASS: all' -or ($rx -and $out -notmatch $rx)) { Write-Host "FAIL: ${name}: exit $LASTEXITCODE, want $want`n$out"; $script:fails++ } else { Write-Host "ok: $name" }
    }
    $rp = $root -replace [regex]::Escape('\'),'/'
    # genuine network failure (unresolvable host) -> SKIP (3)
    RunNew 'dns-outage' 3 "[submodule `"m`"]`n`tpath = m`n`turl = https://nonexistent-host.invalid/x.git`n" 'SKIP:'
    # missing repository / invalid URL -> FAIL (1), never SKIP
    RunNew 'repo-not-found' 1 "[submodule `"m`"]`n`tpath = m`n`turl = $rp/nonexistent`n" 'not a network outage'
    RunNew 'invalid-url' 1 "[submodule `"m`"]`n`tpath = m`n`turl = nosuchscheme://x`n" 'not a network outage'
    # auth failure (HTTP 401/403/404 from a local server is not available offline); a file:// non-repo stands in for "rejected"
    RunNew 'rejected-url' 1 "[submodule `"m`"]`n`tpath = m`n`turl = file:///$rp/nonexistent2`n" 'not a network outage'
    # timeout: a listening socket that never answers -> SKIP, bounded
    $lis = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0); $lis.Start()
    $port = $lis.LocalEndpoint.Port
    $env:KILNCTL_SUBPIN_TIMEOUT_SEC = '3'
    try { RunNew 'timeout' 3 "[submodule `"m`"]`n`tpath = m`n`turl = http://127.0.0.1:$port/x.git`n" 'timed out' } finally { $lis.Stop(); Remove-Item Env:KILNCTL_SUBPIN_TIMEOUT_SEC }
    # S-2: .gitmodules read from the commit; gitlink without .gitmodules at that commit -> FAIL
    G -C $sup rm -q -f .gitmodules; G -C $sup commit -q -m nogm
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $chk -RepoPath $sup 2>&1 | Out-String
    if ($LASTEXITCODE -ne 1 -or $out -match 'PASS') { Write-Host "FAIL: gitlink-no-gitmodules: exit $LASTEXITCODE`n$out"; $fails++ } else { Write-Host "ok: gitlink-no-gitmodules" }
    # S-2: working tree .gitmodules differs from the commit: the commit's wins
    Set-Content -Path (Join-Path $sup '.gitmodules') -Value "[submodule `"m`"]`n`tpath = m`n`turl = $($remote -replace [regex]::Escape('\'),'/')`n"
    G -C $sup add .gitmodules; G -C $sup commit -q -m regood
    G -C $sup update-index --add --cacheinfo "160000,$lost,m"; G -C $sup commit -q -m badpin
    Set-Content -Path (Join-Path $sup '.gitmodules') -Value ''
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $chk -RepoPath $sup 2>&1 | Out-String
    if ($LASTEXITCODE -ne 1) { Write-Host "FAIL: commit-gitmodules: exit $LASTEXITCODE`n$out"; $fails++ } else { Write-Host "ok: commit-gitmodules" }
} catch { Write-Host "ERROR: $_"; $fails++ } finally { Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue }
if ($fails) { Write-Host "$fails FAILED"; exit 1 }
Write-Host "all passed"; exit 0
