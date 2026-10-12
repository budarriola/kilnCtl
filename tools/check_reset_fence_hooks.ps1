# checkcache: ok
# check_reset_fence_hooks.ps1 -- the factory-reset writer fence is only as good as its install lines.
#
# The fence is a set of PREDICATE HOOKS that main.c installs at boot (each is NULL = "refuse nothing" by
# default, which is why a dropped install line is silent -- every writer then simply stops being fenced and
# no test of the writer itself fails):
#   pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer)    pref_cfg_fs.h "WRITER FENCE"
#   cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer)   cfg_fs_mount.c final worker check
#   cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer)         cfg_fs_write_atomic()
#   hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write)                 every kiln_nvs hal_kv write
# and kiln_nvs_reset_refuses_write() must itself consult relay_authority_reset_refuses_kiln_nvs_writer()
# (ea86f4d43: the same reset-in-flight predicate, true only when the reset erases kiln_nvs; so the
# reset job's own task stays exempt).
#
# This check fails when main.c stops making any of those four calls as an unconditional statement of its own
# (backslash continuations are joined and comments are stripped string-literal-aware; each setter must sit at brace
# depth 1 of the app_main() body with no enclosing #if/#ifdef/#ifndef/#elif of ANY condition and a previous code line
# ending in `;`, `{` or `}`, so a commented-out, preprocessor-dead, `if (0)`-wrapped or uncalled-function install does not count; each setter may appear exactly ONCE, so a later
# NULL re-install fails too), or when the kiln_nvs predicate's return is not exactly the three-term expression
# ending in the relay_authority predicate, or when the body of EITHER relay_authority predicate (kiln_nvs and the
# general writer) is edited -- each must be defined exactly once, with an exactly pinned prologue, expression and
# epilogue. Task-identity semantics (another task IS refused while the reset job runs) are pinned by
# test_link_watchdog.c via stub_set_current_task(). Self-tested by tools\negtest.ps1 (REVIEW_DEVBREAK4 R1-R10, REVIEW_DEVBREAK5 F1-F7).
#
# LIMITS: static regex over C text, cannot be complete (REVIEW_DEVBREAKFX6 MED-1); F8/F9/F11/F12/F14b/F15/F17 bypass it. Do NOT extend it further.
# Primary proof: test_main_reset_fence.c compiles the real main.c, runs app_main() and asserts all 4 hooks non-NULL.
#
# Usage: powershell -File tools\check_reset_fence_hooks.ps1 [-MainFile <path to main.c>]
param(
    [string]$MainFile,
    [string]$RelayAuthorityFile
)

$ErrorActionPreference = "Stop"

if (-not $MainFile) {
    $MainFile = Join-Path $PSScriptRoot "..\firmware\KilnFW\App\main.c"
}
if (-not (Test-Path -LiteralPath $MainFile)) {
    Write-Host "FAIL: main.c not found at $MainFile"
    exit 1
}
if (-not $RelayAuthorityFile) {
    $RelayAuthorityFile = Join-Path $PSScriptRoot "..\firmware\KilnFW\App\drivers\owners\relay_authority.c"
}
if (-not (Test-Path -LiteralPath $RelayAuthorityFile)) {
    Write-Host "FAIL: relay_authority.c not found at $RelayAuthorityFile"
    exit 1
}
$text = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $MainFile).Path)
$raText = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $RelayAuthorityFile).Path)

# String- and char-literal-aware comment strip: one left-to-right alternation, so a "/*" inside a string is a string,
# not a comment opener (REVIEW_DEVBREAK5 F6). Literals are kept verbatim.
function Remove-CComments([string]$raw) {
    $t = [regex]::Replace($raw, '\\\r?\n', '')
    $pat = '"(?:\\.|[^"\\\r\n])*"|''(?:\\.|[^''\\\r\n])*''|/\*.*?\*/|//[^\r\n]*'
    return [regex]::Replace($t, $pat, {
        param($m)
        if ($m.Value.StartsWith('/')) { return ' ' }
        return $m.Value
    }, [System.Text.RegularExpressions.RegexOptions]::Singleline)
}

function Test-ZeroCond([string]$c) { return ($c -match '^\(?\s*0[uUlL]*\s*\)?$') }
function Test-OneCond([string]$c) { return ($c -match '^\(?\s*1[uUlL]*\s*\)?$') }

# C pre-pass: strip comments (literal-aware), then drop preprocessor-dead regions (`#if 0`, `#if (0)`, the else of
# `#if 1`). Unknown conditions (#ifdef X, #if CONFIG_Y) stay live in every branch, so a duplicate inside one trips the
# exactly-once counts below instead of being silently trusted. (The install statements themselves are NOT trusted to
# this pass: Get-AnchoredInstallCount requires them outside every conditional.)
function Get-LiveC([string]$raw) {
    $t = Remove-CComments $raw
    $out = New-Object System.Collections.Generic.List[string]
    $stack = New-Object System.Collections.Generic.List[object]   # each: @{ Parent; Cond ('false'|'true'|'unknown') }
    $live = $true
    foreach ($ln in ($t -split '\r?\n')) {
        if ($ln -match '^\s*#\s*(?<d>ifdef|ifndef|if)\b(?<c>.*)$') {
            $c = $Matches['c'].Trim()
            $kind = 'unknown'
            if ($Matches['d'] -eq 'if' -and (Test-ZeroCond $c)) { $kind = 'false' }
            elseif ($Matches['d'] -eq 'if' -and (Test-OneCond $c)) { $kind = 'true' }
            $stack.Add(@{ Parent = $live; Cond = $kind })
            $live = $live -and ($kind -ne 'false')
            continue
        }
        if ($ln -match '^\s*#\s*elif\b(?<c>.*)$' -and $stack.Count -gt 0) {
            $top = $stack[$stack.Count - 1]
            $kind = 'unknown'
            if (Test-ZeroCond $Matches['c'].Trim()) { $kind = 'false' }
            $top.Cond = $kind
            $live = $top.Parent -and ($kind -ne 'false')
            continue
        }
        if ($ln -match '^\s*#\s*else\b' -and $stack.Count -gt 0) {
            $top = $stack[$stack.Count - 1]
            $live = $top.Parent -and ($top.Cond -ne 'true')
            continue
        }
        if ($ln -match '^\s*#\s*endif\b' -and $stack.Count -gt 0) {
            $live = $stack[$stack.Count - 1].Parent
            $stack.RemoveAt($stack.Count - 1)
            continue
        }
        if ($live) { $out.Add($ln) }
    }
    return ($out -join "`n")
}

# Structural anchor (REVIEW_DEVBREAK5 MED-1): counts the setter statements `Name(Arg);` that sit (a) inside the
# app_main() body at brace depth 1, (b) with NO enclosing conditional directive of any kind (#if, #ifdef, #ifndef;
# #elif/#else only occur inside one) -- conditions are never evaluated, any conditional fails -- and (c) with the
# previous code line ending in `;`, `{` or `}` (so a braceless if/while/for/else guard cannot own it).
function Get-AnchoredInstallCount([string]$raw, [string]$name, [string]$arg) {
    $t = Remove-CComments $raw
    $lit = '"(?:\\.|[^"\\\r\n])*"|''(?:\\.|[^''\\\r\n])*'''
    $stmtPat = '^' + [regex]::Escape($name) + '\s*\(\s*' + [regex]::Escape($arg) + '\s*\)\s*;'
    $condDepth = 0
    $brace = 0
    $inMain = $false
    $prevCode = ''
    $count = 0
    foreach ($ln in ($t -split '\r?\n')) {
        $trim = $ln.Trim()
        if ($trim -eq '') { continue }
        if ($trim -match '^#\s*(if|ifdef|ifndef)\b') { $condDepth++; continue }
        if ($trim -match '^#\s*endif\b') { $condDepth--; continue }
        if ($trim.StartsWith('#')) { continue }
        $blank = [regex]::Replace($trim, $lit, '""')
        if (-not $inMain -and $brace -eq 0 -and $condDepth -eq 0 -and $blank -match '(?<![A-Za-z0-9_])void\s+app_main\s*\(\s*void\s*\)') {
            $inMain = $true
        }
        if ($inMain -and $brace -eq 0 -and $blank -match ';$') { $inMain = $false }   # a prototype, not the definition
        if ($inMain -and $brace -eq 1 -and $condDepth -eq 0 -and $trim -match $stmtPat -and $prevCode -match '[;{}]$') {
            $count++
        }
        foreach ($ch in $blank.ToCharArray()) {
            if ($ch -eq '{') { $brace++ } elseif ($ch -eq '}') { $brace-- }
        }
        if ($inMain -and $brace -eq 0 -and $blank -match '\}') { $inMain = $false }
        $prevCode = $blank
    }
    return $count
}

$stripped = Get-LiveC $text

$required = @(
    @{ Name = 'pref_cfg_fs_set_reset_refuse_hook'; Arg = 'relay_authority_reset_refuses_writer' },
    @{ Name = 'cfg_fs_mount_set_write_refuse_hook'; Arg = 'relay_authority_reset_refuses_writer' },
    @{ Name = 'cfg_fs_set_write_refuse_hook'; Arg = 'relay_authority_reset_refuses_writer' },
    @{ Name = 'hal_kv_set_write_refuse_hook'; Arg = 'kiln_nvs_reset_refuses_write' }
)

$fail = 0
foreach ($r in $required) {
    $esc = [regex]::Escape($r.Name)
    # Exactly one mention of the setter (a later NULL re-install, a decoy, or a second call all add a mention) ...
    $all = [regex]::Matches($stripped, '(?<![A-Za-z0-9_])' + $esc + '(?![A-Za-z0-9_])').Count
    # ... and it must be an unconditional statement of its own: first token on its line, exact argument, `;`.
    $stmt = [regex]::Matches($stripped, '(?m)^[ \t]*' + $esc + '\s*\(\s*' + [regex]::Escape($r.Arg) + '\s*\)\s*;').Count
    $anch = Get-AnchoredInstallCount $text $r.Name $r.Arg
    if ($stmt -ne 1 -or $all -ne 1 -or $anch -ne 1) {
        Write-Host "FAIL: main.c must call $($r.Name)($($r.Arg)); exactly once as its own statement directly in the app_main() body (brace depth 1, outside every #if/#ifdef/#ifndef/#elif, previous code line ending in ; { or }) (found $stmt such statement(s), $all mention(s), $anch anchored) -- the factory-reset writer fence it carries is off or overridden"
        $fail++
    }
}

# Macros must not shadow the predicates, their callee, the setters or the helpers used (a function-like or object-like
# `#define` makes the real body irrelevant); main.c is not compiled into any host test, so this check is its only guard.
$mainNames = @('relay_authority_reset_refuses_kiln_nvs_writer', 'relay_authority_reset_refuses_writer', 'kiln_nvs_reset_refuses_write', 'strcmp', 'NULL') + @($required | ForEach-Object { $_.Name })
$mainMacro = '(?m)^\s*#\s*(define|undef)\s+(' + (($mainNames | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')\b'
if ($stripped -match $mainMacro) {
    Write-Host 'FAIL: main.c #defines/#undefs a reset-fence predicate, setter, its relay_authority callee, strcmp or NULL -- the fence is shadowed'
    $fail++
}

# The kiln_nvs predicate's whole body must be a single return of partition != NULL && strcmp(partition, "kiln_nvs") == 0
# && relay_authority_reset_refuses_kiln_nvs_writer(). The parameter name is free (a rename is harmless), but every use
# must be the same identifier (backreference). Anything appended or inserted fails. It must be defined exactly once.
$predPat = 'static\s+bool\s+kiln_nvs_reset_refuses_write\s*\(\s*const\s+char\s*\*\s*(?<p>[A-Za-z_]\w*)\s*\)\s*\{\s*return\s+\k<p>\s*!=\s*NULL\s*&&\s*strcmp\s*\(\s*\k<p>\s*,\s*"kiln_nvs"\s*\)\s*==\s*0\s*&&\s*relay_authority_reset_refuses_kiln_nvs_writer\s*\(\s*\)\s*;\s*\}'
$predDefs = [regex]::Matches($stripped, '(?<![A-Za-z0-9_])bool\s+kiln_nvs_reset_refuses_write\s*\(').Count
if ($predDefs -ne 1 -or $stripped -notmatch $predPat) {
    Write-Host "FAIL: kiln_nvs_reset_refuses_write() must be defined exactly once (found $predDefs) with a body that is exactly one return of: <arg> != NULL && strcmp(<arg>, `"kiln_nvs`") == 0 && relay_authority_reset_refuses_kiln_nvs_writer()"
    $fail++
}

# The shared predicates in relay_authority.c: both bodies are pinned token for token (prologue, expression, epilogue),
# each defined exactly once outside any dead region. Semantics (another task is refused) live in test_link_watchdog.c.
$raStripped = Get-LiveC $raText
$raNames = @('s_reset_in_flight_depth', 's_reset_erases_kiln_nvs', 's_reset_job_task', 's_heat_claim_mux', 'xTaskGetCurrentTaskHandle', 'portENTER_CRITICAL', 'portEXIT_CRITICAL', 'relay_authority_reset_refuses_kiln_nvs_writer', 'relay_authority_reset_refuses_writer')
if ($raStripped -match ('(?m)^\s*#\s*(define|undef)\s+(' + ($raNames -join '|') + ')\b')) {
    Write-Host 'FAIL: relay_authority.c #defines/#undefs reset state, the critical-section/task-handle primitives or a reset predicate -- a body is shadowed'
    $fail++
}
$prologue = '\{\s*void\s*\*\s*me\s*=\s*\(\s*void\s*\*\s*\)\s*xTaskGetCurrentTaskHandle\s*\(\s*\)\s*;\s*portENTER_CRITICAL\s*\(\s*&s_heat_claim_mux\s*\)\s*;\s*'
$epilogue = '\s*;\s*portEXIT_CRITICAL\s*\(\s*&s_heat_claim_mux\s*\)\s*;\s*return\s+refuse\s*;\s*\}'
$jobTerm = '\(\s*s_reset_job_task\s*==\s*NULL\s*\|\|\s*s_reset_job_task\s*!=\s*me\s*\)'
$bodies = @(
    @{ Fn = 'relay_authority_reset_refuses_kiln_nvs_writer'; Expr = 'bool\s+refuse\s*=\s*\(\s*s_reset_in_flight_depth\s*!=\s*0\s*\)\s*&&\s*s_reset_erases_kiln_nvs\s*&&\s*' + $jobTerm; Shape = '(s_reset_in_flight_depth != 0) && s_reset_erases_kiln_nvs && (s_reset_job_task == NULL || s_reset_job_task != me)' },
    @{ Fn = 'relay_authority_reset_refuses_writer'; Expr = 'bool\s+refuse\s*=\s*\(\s*s_reset_in_flight_depth\s*!=\s*0\s*\)\s*&&\s*' + $jobTerm; Shape = '(s_reset_in_flight_depth != 0) && (s_reset_job_task == NULL || s_reset_job_task != me)' }
)
foreach ($b in $bodies) {
    $defs = [regex]::Matches($raStripped, '(?<![A-Za-z0-9_])bool\s+' + $b.Fn + '\s*\(').Count
    $pat = 'bool\s+' + $b.Fn + '\s*\(\s*void\s*\)\s*' + $prologue + $b.Expr + $epilogue
    if ($defs -ne 1 -or $raStripped -notmatch $pat) {
        Write-Host "FAIL: $($b.Fn)() in relay_authority.c must be defined exactly once (found $defs) and be exactly: void *me = (void *)xTaskGetCurrentTaskHandle(); portENTER_CRITICAL(&s_heat_claim_mux); bool refuse = $($b.Shape); portEXIT_CRITICAL(&s_heat_claim_mux); return refuse; -- no early return, local shadow or reassignment"
        $fail++
    }
}

if ($fail -gt 0) {
    Write-Host "check_reset_fence_hooks: $fail problem(s)"
    exit 1
}
Write-Host "PASS: check_reset_fence_hooks (all 4 reset-fence hooks installed by main.c; both relay_authority predicates pinned)"
exit 0
