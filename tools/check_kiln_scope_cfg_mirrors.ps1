# checkcache: ok
# check_kiln_scope_cfg_mirrors.ps1 -- keeps the factory-reset "scope file
# lists" (kKilnScopeFiles in kiln_scope_cfg_files.c, and any sibling
# <x>_scope_cfg_files.c such as a future profiles one) in step with the cfg
# LittleFS mirror files the firmware actually writes.
#
# The problem ("reset one side of a pair", CLAUDE.md): every kiln_nvs
# preference/store also dual-writes a file on the cfg LittleFS partition. A
# kiln-scope factory reset erases the NVS side and deletes the files named in
# kKilnScopeFiles. A NEW kiln_nvs mirror added later and never put in that
# list is silently NOT cleared by the reset. The host test only asserts a
# hardcoded floor (n >= 11), which cannot notice that.
#
# Heuristics (all text-based, comments stripped first; no compile):
#   1. PATH MACROS. Every `#define <NAME>_FILE_PATH "..."` under
#      firmware/KilnFW/App (excluding test/ and the scope-list files) is a
#      mirror path.
#   2. OWNER TUs. The translation units that USE the macro (any .c under App,
#      excluding test/ and the scope-list files). A macro defined inside a .c
#      always counts its defining .c as an owner. If no owner contains a
#      hal_kv_open() (a pure cfg_fs helper such as zones_config_cfg_fs.c, or
#      a macro used only from such helpers), the owner set widens to the
#      "family": every .c whose basename starts with the defining file's
#      stem, where stem = basename minus extension minus a trailing
#      `_cfg_fs`/`_internal`/`_cfg` (kiln_cfg_store_cfg_fs.h ->
#      kiln_cfg_store*.c; zones_config_cfg_fs.h -> zones_config*.c).
#   3. PARTITION. In each owner TU, every hal_kv_open(h, ns, mode, PART) call
#      yields PART as the owning NVS partition. PART is a string literal, or
#      an identifier resolved through `#define X "str"` in the same TU or in
#      any "quoted" header it includes (transitively, headers found by
#      basename anywhere under App; first match wins on equal basenames).
#   4. VERDICT PER MACRO. The union of partitions over its owner TUs must be
#      exactly one string; zero (nothing resolved) or more than one (mixed)
#      is a FAIL, never a silent skip.
#   5. WRITER SITES. Every pref_cfg_fs_save/resolve/load_raw call outside
#      pref_cfg_fs.c must name a known *_FILE_PATH macro as its first
#      argument; a literal or any other expression is a FAIL (an
#      unrecognised mirror path this check cannot attribute to a partition).
#      The same applies to direct cfg_fs_read/cfg_fs_write_atomic/
#      cfg_fs_delete calls whose first argument is a string literal or an
#      ALL_CAPS identifier that is not a known macro. (Lower-case pass-through
#      parameters, e.g. diagnostics_http.c's generic route, are ignored.)
#   6. SCOPE LISTS. kiln_scope_cfg_files.c (partition kiln_nvs) must contain
#      a `k...Files[]` initializer (FAIL if missing). Every entry must be a
#      known macro (else FAIL). Assert: every macro whose partition is
#      kiln_nvs is in the list, and no listed macro resolves to another
#      partition. Any other <x>_scope_cfg_files.c that HAS such an array is
#      policed the same way against partition "<x>_nvs"; one WITHOUT it
#      (e.g. profiles_scope_cfg_files.c, which sweeps directories because
#      per-id files cannot be listed by macro) is reported as info,
#      "directory-swept scope list, not policed here". Mirrors in a
#      partition with no list are info too; macros deleted by other means
#      (PROFILES_HIDDEN_FILE_PATH) are not this check's concern.
#   LIMITS. Writers called through lowercase variables or function pointers
#      (s_write_fn in zones_config_cfg_fs.c / kiln_cfg_store_cfg_fs.c) are
#      not inspected, so a mirror whose macro does not end in _FILE_PATH, or
#      whose path is built at runtime, is invisible to this check.
#      hal_kv_open partition args: NULL = default partition "nvs"; lowercase
#      parameters are ignored; any other unresolved word FAILs, as does any
#      call that does not parse (call count vs parsed count per owner file).
#      The default "nvs" is dropped when a named partition is also present in
#      the same owner set, but only if EVERY NULL open is
#      HAL_KV_MODE_READ_ONLY (legacy migrate-from-default probe, as in
#      relay_cycles.c); a NULL open in any other mode is a real default-nvs
#      write, which a kiln reset never erases, and stays in the set (FAIL as
#      mixed). Alone, "nvs" is a real partition and stands. The call-count
#      check is deliberately fail-closed: it also counts a string literal such
#      as "hal_kv_open (read) failed" (ota_record.c) as a call.
#   7. SANITY FLOOR. Fewer than 8 macros found means the pattern went blind: FAIL.
#
# Exit codes: 0 PASS, 1 FAIL (including any internal error). Never SKIPs: the
# only prerequisite is the source tree itself.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\check_kiln_scope_cfg_mirrors.ps1
#        (-AppDir <path> to point at a simulated App tree)
param(
    [string]$AppDir
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if (-not $AppDir) { $AppDir = Join-Path $root "..\firmware\KilnFW\App" }
$AppDir = (Resolve-Path $AppDir).Path

function Strip-Comments([string]$t) {
    $t = [regex]::Replace($t, '/\*.*?\*/', { param($m) ([regex]::Replace($m.Value, '[^\r\n]', ' ')) }, 'Singleline')
    return [regex]::Replace($t, '//[^\r\n]*', '')
}

try {
    $all = @(Get-ChildItem -Path $AppDir -Recurse -File -Include *.c, *.h |
        Where-Object { $_.FullName -notmatch '[\\/](test|build)[\\/]' })
    $scopeFiles = @($all | Where-Object { $_.Name -match '_scope_cfg_files\.c$' })
    $src = @{}      # full path -> comment-stripped text
    foreach ($f in $all) { $src[$f.FullName] = Strip-Comments ([IO.File]::ReadAllText($f.FullName)) }
    $byName = @{}
    foreach ($f in $all) { if (-not $byName.ContainsKey($f.Name)) { $byName[$f.Name] = $f.FullName } }
    $scopeSet = @($scopeFiles | ForEach-Object { $_.FullName })
    $cFiles = @($all | Where-Object { $_.Extension -eq '.c' -and ($scopeSet -notcontains $_.FullName) })

    function Get-Defines([string]$path, $seen) {
        # NAME -> "string" for this file and its quoted includes, transitively
        $h = @{}
        if ($seen.ContainsKey($path)) { return $h }
        $seen[$path] = $true
        $t = $src[$path]
        foreach ($m in [regex]::Matches($t, '(?m)^\s*#\s*define\s+(\w+)\s+"([^"]*)"')) { $h[$m.Groups[1].Value] = $m.Groups[2].Value }
        foreach ($m in [regex]::Matches($t, '(?m)^\s*#\s*include\s+"([^"]+)"')) {
            $bn = [IO.Path]::GetFileName($m.Groups[1].Value)
            if ($byName.ContainsKey($bn)) {
                $sub = Get-Defines $byName[$bn] $seen
                foreach ($k in $sub.Keys) { if (-not $h.ContainsKey($k)) { $h[$k] = $sub[$k] } }
            }
        }
        return $h
    }

    # 1. macros
    $macros = @{}   # name -> @{ Value; DefFile }
    foreach ($f in $all) {
        if ($scopeSet -contains $f.FullName) { continue }
        foreach ($m in [regex]::Matches($src[$f.FullName], '(?m)^\s*#\s*define\s+(\w+_FILE_PATH)\s+"([^"]*)"')) {
            $macros[$m.Groups[1].Value] = @{ Value = $m.Groups[2].Value; DefFile = $f.FullName }
        }
    }
    if ($macros.Count -lt 8) {
        throw "only $($macros.Count) *_FILE_PATH macro(s) found under $AppDir -- implausibly low (12 expected as of 2026-10-04); this check has gone blind."
    }

    # 2+3+4. resolve each macro to a partition
    $rows = @()
    $problems = New-Object System.Collections.Generic.List[string]
    foreach ($name in ($macros.Keys | Sort-Object)) {
        $def = $macros[$name].DefFile
        $owners = @($cFiles | Where-Object { $src[$_.FullName] -match "\b$name\b" })
        if ($def.EndsWith('.c') -and (@($owners | ForEach-Object { $_.FullName }) -notcontains $def)) { $owners += Get-Item $def }
        $hasKv = @($owners | Where-Object { $src[$_.FullName] -match '\bhal_kv_open\s*\(' })
        $viaFamily = $false
        if ($hasKv.Count -eq 0) {
            $stem = [IO.Path]::GetFileNameWithoutExtension($def) -replace '(_cfg_fs|_internal|_cfg)$', ''
            $owners = @($cFiles | Where-Object { $_.Name.StartsWith($stem + '_') -or $_.Name.StartsWith($stem + '.') })
            $viaFamily = $true
        }
        $parts = New-Object System.Collections.Generic.HashSet[string]
        $why = @()
        $nvsWrite = $false
        foreach ($o in $owners) {
            $defs = $null
            $callCount = [regex]::Matches($src[$o.FullName], '\bhal_kv_open\s*\(').Count
            $matched = [regex]::Matches($src[$o.FullName], '\bhal_kv_open\s*\(\s*[^,]+,[^,]+,\s*([^,]+?)\s*,\s*(\w+|"[^"]*")\s*\)')
            if ($callCount -ne $matched.Count) {
                $problems.Add("${name}: $($o.Name) has $callCount hal_kv_open( call(s) but only $($matched.Count) parse as 4 args with a word/literal partition -- the rest cannot be attributed")
            }
            foreach ($m in $matched) {
                $mode = $m.Groups[1].Value
                $arg = $m.Groups[2].Value
                $p = $null
                if ($arg.StartsWith('"')) { $p = $arg.Trim('"') }
                elseif ($arg -ceq 'NULL') { $p = 'nvs'; if ($mode -cne 'HAL_KV_MODE_READ_ONLY') { $nvsWrite = $true } }   # hal_kv.h: NULL = default partition
                elseif ($arg -cmatch '^[a-z_][a-z0-9_]*$') { continue }   # pass-through parameter, ignored (same rule as step 5)
                else {
                    if ($null -eq $defs) { $defs = Get-Defines $o.FullName @{} }
                    if ($defs.ContainsKey($arg)) { $p = $defs[$arg] }
                    else { $problems.Add("${name}: $($o.Name): hal_kv_open partition argument '$arg' is not resolvable to a string"); continue }
                }
                [void]$parts.Add($p)
            }
        }
        # Legacy-migration probes read the pre-split default partition ("nvs",
        # hal_kv_open(..., NULL)) next to the real one (relay_cycles.c). Drop
        # the default only when a named partition is also present.
        if ($parts.Count -gt 1 -and $parts.Contains('nvs') -and -not $nvsWrite) { [void]$parts.Remove('nvs') }
        $ownerNames = (($owners | ForEach-Object { $_.Name }) -join ',')
        $part = '<unresolved>'
        if ($parts.Count -eq 1) { $part = @($parts)[0] }
        elseif ($parts.Count -gt 1) {
            $part = '<mixed>'
            $problems.Add("$name resolves to MULTIPLE partitions: $(($parts | Sort-Object) -join ', ') (owners: $ownerNames) -- cannot attribute it")
        }
        else { $problems.Add("${name}: no hal_kv_open partition resolved from owner TU(s) [$ownerNames] $($why -join '; ')") }
        $rows += [pscustomobject]@{ Macro = $name; File = $macros[$name].Value; Partition = $part; Owners = $ownerNames + $(if ($viaFamily) { ' (family)' } else { '' }) }
    }
    foreach ($r in $rows) { if ($r.Owners -like '*(family)') { Write-Host "Info: $($r.Macro) -> $($r.Partition) resolved via family owners: $($r.Owners)" } }

    # 5. writer sites
    foreach ($f in $cFiles) {
        if ($f.Name -eq 'pref_cfg_fs.c' -or $f.Name -eq 'cfg_fs.c') { continue }
        $t = $src[$f.FullName]
        foreach ($m in [regex]::Matches($t, '\bpref_cfg_fs_(?:save|resolve|load_raw)\s*\(\s*([^,\)]+?)\s*[,\)]')) {
            $a = $m.Groups[1].Value
            if (-not $macros.ContainsKey($a)) { $problems.Add("$($f.Name): pref_cfg_fs_* site with unrecognised path argument '$a' (not a known *_FILE_PATH macro)") }
        }
        foreach ($m in [regex]::Matches($t, '\bcfg_fs_(?:read|write_atomic|delete)\s*\(\s*([^,\)]+?)\s*[,\)]')) {
            $a = $m.Groups[1].Value
            if (($a.StartsWith('"') -or $a -cmatch '^[A-Z][A-Z0-9_]+$') -and -not $macros.ContainsKey($a)) {
                $problems.Add("$($f.Name): cfg_fs_* site with unrecognised path argument '$a' (not a known *_FILE_PATH macro)")
            }
        }
    }

    # 6. scope lists
    if ($scopeFiles.Count -eq 0) { throw "no *_scope_cfg_files.c found under $AppDir (kiln_scope_cfg_files.c expected)." }
    $checkedParts = @()
    foreach ($sf in $scopeFiles) {
        $x = $sf.Name -replace '_scope_cfg_files\.c$', ''
        $listPart = "${x}_nvs"
        $m = [regex]::Match($src[$sf.FullName], '(?s)\b(k\w*Files)\s*\[\s*\]\s*=\s*\{(.*?)\};')
        if (-not $m.Success) {
            if ($sf.Name -eq 'kiln_scope_cfg_files.c') { $problems.Add("$($sf.Name): could not locate the k...Files[] initializer"); continue }
            Write-Host "Info: $($sf.Name) has no k...Files[] array: directory-swept scope list, not policed here."
            continue
        }
        $checkedParts += $listPart
        $entries = @($m.Groups[2].Value -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        foreach ($e in $entries) {
            if (-not $macros.ContainsKey($e)) { $problems.Add("$($sf.Name): list entry '$e' is not a known *_FILE_PATH macro (literal or unknown)"); continue }
            $r = $rows | Where-Object { $_.Macro -eq $e }
            if ($r.Partition -ne $listPart -and $r.Partition -notlike '<*') {
                $problems.Add("$($sf.Name): lists $e but it resolves to partition '$($r.Partition)', not '$listPart'")
            }
        }
        foreach ($r in $rows) {
            if ($r.Partition -eq $listPart -and ($entries -notcontains $r.Macro)) {
                $problems.Add("$($sf.Name): MISSING $($r.Macro) ($($r.File)) -- resolves to $listPart but a reset of that scope would not delete it")
            }
        }
        Write-Host "$($sf.Name): $($entries.Count) entr(ies) checked against partition '$listPart'."
    }
    $others = @($rows | Where-Object { $_.Partition -notlike '<*' -and ($checkedParts -notcontains $_.Partition) } | ForEach-Object { $_.Partition } | Sort-Object -Unique)
    if ($others.Count -gt 0) { Write-Host "Info: partition(s) with mirrors but no *_scope_cfg_files.c list yet: $($others -join ', ')" }

    Write-Host "Scope cfg mirror check: $($macros.Count) *_FILE_PATH macro(s), $($scopeFiles.Count) scope list(s)."
    if ($problems.Count -gt 0) {
        Write-Host "KILN SCOPE CFG MIRROR CHECK FAILED:" -ForegroundColor Red
        foreach ($p in $problems) { Write-Host "  $p" -ForegroundColor Red }
        Write-Host "Resolved table:" -ForegroundColor Red
        $rows | Format-Table Macro, File, Partition, Owners -AutoSize | Out-String -Width 250 | Write-Host
        exit 1
    }
    Write-Host "PASS: every kiln_nvs cfg mirror is in its scope list and nothing is listed under the wrong partition."
    exit 0
} catch {
    Write-Host "check_kiln_scope_cfg_mirrors.ps1 FAILED: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
