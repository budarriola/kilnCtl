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
    # S-2a: working tree .gitmodules is present but names only "other"; commit's names "m" with the unpushed pin -> must FAIL
    Set-Content -Path (Join-Path $sup '.gitmodules') -Value "[submodule `"other`"]`n`tpath = other`n`turl = $rp/nonexistent`n"
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $chk -RepoPath $sup 2>&1 | Out-String
    if ($LASTEXITCODE -ne 1 -or $out -notmatch 'push the submodule commit to its remote before landing') { Write-Host "FAIL: commit-gitmodules-differs: exit $LASTEXITCODE`n$out"; $fails++ } else { Write-Host "ok: commit-gitmodules-differs" }
    Remove-Item (Join-Path $sup '.gitmodules') -Force; G -C $sup checkout -q -- .gitmodules
    # S-2b: gitlink not named by .gitmodules (second gitlink "x") -> FAIL
    G -C $sup update-index --add --cacheinfo "160000,$tip,x"; G -C $sup commit -q -m extragl
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $chk -RepoPath $sup 2>&1 | Out-String
    if ($LASTEXITCODE -ne 1 -or $out -notmatch 'not named by .gitmodules') { Write-Host "FAIL: unnamed-gitlink: exit $LASTEXITCODE`n$out"; $fails++ } else { Write-Host "ok: unnamed-gitlink" }
    G -C $sup update-index --force-remove x; G -C $sup commit -q -m rmx
    # S-1a: submodule host down but origin answers -> FAIL (not SKIP); origin down too -> SKIP
    $sup2 = Join-Path $root 'sup2'
    git init -q $sup2 2>&1 | Out-Null
    G -C $sup2 config user.email t@t; G -C $sup2 config user.name t
    Set-Content -Path (Join-Path $sup2 '.gitmodules') -Value "[submodule `"m`"]`n`tpath = m`n`turl = https://nonexistent-host.invalid/x.git`n"
    G -C $sup2 add .gitmodules; G -C $sup2 update-index --add --cacheinfo "160000,$tip,m"; G -C $sup2 commit -q -m s
    G -C $sup2 remote add origin $rp/remote
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $chk -RepoPath $sup2 2>&1 | Out-String
    if ($LASTEXITCODE -ne 1 -or $out -notmatch 'origin answers') { Write-Host "FAIL: origin-up-submodule-down: exit $LASTEXITCODE`n$out"; $fails++ } else { Write-Host "ok: origin-up-submodule-down" }
    G -C $sup2 remote set-url origin https://nonexistent-host.invalid/o.git
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $chk -RepoPath $sup2 2>&1 | Out-String
    if ($LASTEXITCODE -ne 3) { Write-Host "FAIL: both-down: exit $LASTEXITCODE`n$out"; $fails++ } else { Write-Host "ok: both-down" }
    # S-6: no-arg run (default RepoPath) under -File must not crash at param binding (exit 2 = script error)
    Push-Location (Join-Path $PSScriptRoot '..')
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $chk 2>&1 | Out-String
    $code = $LASTEXITCODE
    Pop-Location
    if ($out -match 'Cannot bind|ParameterBinding' -or $out -notmatch 'PASS|SKIP|FAIL: ') { Write-Host "FAIL: no-arg: exit $code`n$out"; $fails++ } else { Write-Host "ok: no-arg (exit $code)" }} catch { Write-Host "ERROR: $_"; $fails++ } finally { Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue }
if ($fails) { Write-Host "$fails FAILED"; exit 1 }
Write-Host "all passed"; exit 0
