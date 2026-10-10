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
    # unreachable remote must SKIP (3), never PASS
    Set-Content -Path (Join-Path $sup '.gitmodules') -Value "[submodule `"m`"]`n`tpath = m`n`turl = $($root -replace [regex]::Escape('\'),'/')/nonexistent`n"
    G -C $sup add .gitmodules; G -C $sup commit -q -m unreach
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $chk -RepoPath $sup 2>&1 | Out-String
    if ($LASTEXITCODE -ne 3 -or $out -match 'PASS: all') { Write-Host "FAIL: unreachable: exit $LASTEXITCODE`n$out"; $fails++ } else { Write-Host "ok: unreachable" }
} catch { Write-Host "ERROR: $_"; $fails++ } finally { Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue }
if ($fails) { Write-Host "$fails FAILED"; exit 1 }
Write-Host "all passed"; exit 0
