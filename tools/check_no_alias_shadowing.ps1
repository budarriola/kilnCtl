# check_no_alias_shadowing.ps1 -- fail if a tracked .ps1 under tools/ or firmware/ defines a
# function whose name equals a built-in PowerShell alias (Rm, Del, Ls, Cat, Kill, Sleep, ...).
# A function beats an alias in command resolution, so a helper named `Rm` silently shadowed
# Remove-Item and hung a suite run 1h40m on its interactive "Confirm?" prompt.
# The alias list comes from Get-Alias at check time; names compared case-insensitively
# after stripping a scope prefix (global:/script:/local:/private:).
# checkcache: ok
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = "Stop"
$aliases = @{}
foreach ($a in Get-Alias) { $aliases[$a.Name.ToLowerInvariant()] = $a.Definition }
if ($aliases.Count -lt 50) { Write-Host "FAIL: Get-Alias returned only $($aliases.Count) aliases (vacuous)"; exit 1 }
$files = @(git -C $Root ls-files -- 'tools/*.ps1' 'firmware/*.ps1' | ForEach-Object { Join-Path $Root $_ })
if ($files.Count -lt 20) { Write-Host "FAIL: only $($files.Count) tracked .ps1 files found (vacuous)"; exit 1 }
$bad = 0
foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f)) { continue }
    $tokens = $null; $errs = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($f, [ref]$tokens, [ref]$errs)
    if ($null -eq $ast) { continue }
    $funcs = $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true)
    foreach ($fn in $funcs) {
        $name = ($fn.Name -replace '^(?i)(global|script|local|private):', '').ToLowerInvariant()
        if ($aliases.ContainsKey($name)) {
            $rel = $f.Substring($Root.Length).TrimStart('\', '/')
            Write-Host "FAIL: ${rel}:$($fn.Extent.StartLineNumber) defines function '$($fn.Name)' shadowing built-in alias '$name' -> $($aliases[$name])"
            $bad++
        }
    }
}
if ($bad -gt 0) { Write-Host "$bad alias-shadowing function(s). Rename (e.g. Remove-Tree, not Rm)."; exit 1 }
Write-Host "PASS: no function in $($files.Count) tracked .ps1 files shadows a built-in alias ($($aliases.Count) aliases)"
exit 0
