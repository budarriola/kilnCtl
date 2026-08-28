# check_relay_writes_through_owner.ps1 -- kiln_io_set_relay_mask() and
# kiln_io_all_relays_off() may only be called from kiln_io_owner.c (the
# single task that serializes every relay write against the SX1509), never
# from anywhere else in firmware/KilnFW.
#
# WHY THIS EXISTS. kiln_io_owner.c/.h exist because a 2026-08-19 research
# pass found FIVE independent callers writing relay/IO state directly:
# uart_bridge.c's io_bridge_task (UART), dashboard_http.c's
# dashboard_set_relay() (HTTP and, via ui_page_temperature.c, the LCD),
# profile_executor.c's apply_relay()/force_relay_mask_off()/
# sweep_unowned_relays(), and autotune_engine.c's apply_relay(). On
# 2026-08-27 the new zones current-sweep feature became the SIXTH -- caught
# only by an opus review, hours before flashing to a bench with LIVE HEATING
# ELEMENTS. kiln_io_owner.h's own header states why this is not a
# theoretical worry: kiln_io_set_relay_mask() is a read-modify-write against
# the SX1509 data register, and "SX1509.c's own internal mutex ... does NOT
# stop two independent calls from racing on a stale read -- a lost-update on
# the code path that energizes mains contactors."
#
# WHAT THIS CHECKS. Every call to kiln_io_set_relay_mask( or
# kiln_io_all_relays_off( anywhere under firmware/KilnFW/App, outside
# kiln_io_owner.c itself, is a failure -- UNLESS it is on the allowlist below
# with a stated reason.
#
# kiln_io_all_relays_off() carries a genuinely NARROWER licence than
# kiln_io_set_relay_mask(): kiln_io_owner.h:75-89 documents that it is
# deliberately NOT routed through the owner queue for a specific, closed set
# of last-resort fail-safe paths -- the link-loss watchdog, the guard-9
# safety-link-silence watchdog, and kiln_enter_safe_state() -- because
# routing a fail-safe call through the very task that might be the thing
# that's wedged is exactly backwards, and because the call is unconditional
# and only ever turns relays OFF, so a race against the owner task is benign
# in the failure direction. Those specific call sites are allowlisted below
# WITH that reason. kiln_io_set_relay_mask() has NO such documented
# exception anywhere in kiln_io_owner.h -- every call to it outside
# kiln_io_owner.c is a failure, full stop, allowlist or not.
#
# kiln_io.c itself -- the low-level SX1509 driver where both functions are
# DEFINED -- is allowlisted for its own internal call from
# kiln_io_set_relay() to kiln_io_set_relay_mask() (a same-file convenience
# wrapper for a single relay, not a second caller bypassing the owner
# queue). Nothing else in kiln_io.c calls either function outside its own
# definition.
#
# App/test/*.c fake/stub definitions of these two functions (for mocking
# kiln_io in a host-test double) are DEFINITIONS, not calls into the real
# driver, and test doubles are not a production code path this guard is
# protecting -- so App/test/ is excluded from the scan entirely, matching
# this repo's other guards' treatment of test doubles.
#
# Comments are stripped first (same Get-CodeOnlyLines helper as
# firmware/SaftyFW/tools/check_guard_input_producers.ps1) so a comment that
# merely discusses or shows one of these calls -- which this project's own
# comment history does constantly, including in kiln_io_owner.h itself -- is
# not counted as a real call.
#
# Usage: powershell -File tools\check_relay_writes_through_owner.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$appDir = Join-Path $root "firmware\KilnFW\App"
if (-not (Test-Path $appDir)) {
    throw "check_relay_writes_through_owner: $appDir not found -- has it moved? This check is now blind."
}
$appDirResolved = (Resolve-Path $appDir).Path

# Comment stripper, same shape as
# firmware/SaftyFW/tools/check_guard_input_producers.ps1's Get-CodeOnlyLines
# (duplicated rather than imported -- this project has no shared PowerShell
# module mechanism).
function Get-CodeOnlyLines {
    param([string]$Path)
    $inBlockComment = $false
    $result = @()
    foreach ($line in (Get-Content -Path $Path)) {
        $code = $line
        if ($inBlockComment) {
            $endIdx = $code.IndexOf("*/")
            if ($endIdx -ge 0) { $code = $code.Substring($endIdx + 2); $inBlockComment = $false }
            else { $result += ""; continue }
        }
        $lineCommentIdx = $code.IndexOf("//")
        if ($lineCommentIdx -ge 0) { $code = $code.Substring(0, $lineCommentIdx) }
        while ($true) {
            $startIdx = $code.IndexOf("/*")
            if ($startIdx -lt 0) { break }
            $endIdx = $code.IndexOf("*/", $startIdx)
            if ($endIdx -ge 0) { $code = $code.Substring(0, $startIdx) + $code.Substring($endIdx + 2) }
            else { $code = $code.Substring(0, $startIdx); $inBlockComment = $true; break }
        }
        $result += $code
    }
    return $result
}

# --- Allowlist: {RelPath; Function; Reason}. Function is one of
# "kiln_io_set_relay_mask" or "kiln_io_all_relays_off" -- an entry only
# licenses that ONE function in that ONE file, so a file allowlisted for
# kiln_io_all_relays_off() still fails if it ever grows a direct
# kiln_io_set_relay_mask() call. ---
$allowlist = @(
    @{
        RelPath  = "firmware/KilnFW/App/drivers/kiln_io.c"
        Function = "kiln_io_set_relay_mask"
        Reason   = "definition site of both functions; kiln_io_set_relay() in this same file calls kiln_io_set_relay_mask() as a single-relay convenience wrapper around its own sibling function -- not a second caller bypassing the owner queue."
    },
    @{
        RelPath  = "firmware/KilnFW/App/drivers/kiln_io.c"
        Function = "kiln_io_all_relays_off"
        Reason   = "definition site of both functions."
    },
    @{
        RelPath  = "firmware/KilnFW/App/main.c"
        Function = "kiln_io_all_relays_off"
        Reason   = "kiln_enter_safe_state() -- the shutdown/panic path documented as a licensed exception in kiln_io_owner.h:75-89 (app_main giving up must still be able to drop relays even if the owner task is the thing that's wedged)."
    },
    @{
        RelPath  = "firmware/KilnFW/App/drivers/profile_executor.c"
        Function = "kiln_io_all_relays_off"
        Reason   = "guard 9 / safety-link-silence watchdog abort path -- documented as a licensed exception in kiln_io_owner.h:75-89 (must still run if the main control task, which is what normally posts to the owner, is the thing that's stuck)."
    }
)

function Test-Allowlisted {
    param([string]$RelPath, [string]$Function)
    foreach ($entry in $allowlist) {
        if ($entry.RelPath -ieq $RelPath -and $entry.Function -ieq $Function) { return $true }
    }
    return $false
}

$patterns = @("kiln_io_set_relay_mask", "kiln_io_all_relays_off")

$sourceFiles = Get-ChildItem -Path $appDirResolved -Recurse -File -Include "*.c" |
    Where-Object { $_.FullName -notmatch '[\\/]test[\\/]' -and $_.FullName -notmatch '[\\/]build[\\/]' }

if ($sourceFiles.Count -lt 50) {
    throw "check_relay_writes_through_owner: only $($sourceFiles.Count) .c file(s) found under $appDirResolved (excluding test/) -- implausibly low, has the tree moved? This check would pass vacuously."
}

$ownerFile = Join-Path $appDirResolved "drivers\kiln_io_owner.c"
if (-not (Test-Path $ownerFile)) {
    throw "check_relay_writes_through_owner: $ownerFile not found -- has kiln_io_owner.c moved or been renamed? This check is now blind."
}
$ownerFileResolved = (Resolve-Path $ownerFile).Path

$violations = @()
$ownerCallsSeen = 0

foreach ($f in $sourceFiles) {
    $isOwnerFile = ($f.FullName -ieq $ownerFileResolved)
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    $rel = "firmware/KilnFW/App/" + ($f.FullName.Substring($appDirResolved.Length + 1) -replace '\\', '/')

    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        $line = $codeLines[$i]
        foreach ($fn in $patterns) {
            # A CALL, not a definition: `fn(` not preceded by a return-type
            # word immediately at line start defining it. We don't need to
            # distinguish call vs definition here for kiln_io_owner.c (always
            # allowed) or for App/test/ (excluded above); the one file where
            # it matters is kiln_io.c, which is fully allowlisted for both
            # functions anyway (it's the definition site), so a simple
            # substring-of-call-syntax match is sufficient and does not need
            # to special-case "esp_err_t kiln_io_set_relay_mask(...)"
            # definition lines vs. call sites.
            if ($line -match "\b$fn\s*\(") {
                if ($isOwnerFile) {
                    $ownerCallsSeen++
                    continue
                }
                if (Test-Allowlisted -RelPath $rel -Function $fn) {
                    continue
                }
                $violations += "${rel}:$($i + 1): direct call to $fn() outside kiln_io_owner.c and not allowlisted"
            }
        }
    }
}

if ($ownerCallsSeen -lt 1) {
    throw "check_relay_writes_through_owner: found zero calls to kiln_io_set_relay_mask()/kiln_io_all_relays_off() inside kiln_io_owner.c itself -- the pattern or the owner file's contents have changed and this check has gone blind. Update it before trusting this result."
}

if ($violations.Count -gt 0) {
    Write-Host "RELAY WRITE OWNERSHIP CHECK FAILED:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  $v" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  kiln_io_set_relay_mask() is a read-modify-write against the SX1509 data" -ForegroundColor Red
    Write-Host "  register; SX1509.c's mutex serializes each call's own I2C transaction but" -ForegroundColor Red
    Write-Host "  does NOT stop two independent callers from racing on a stale read -- a" -ForegroundColor Red
    Write-Host "  lost-update on the code path that energizes mains contactors. Route the" -ForegroundColor Red
    Write-Host "  write through kiln_io_owner.c instead (kiln_io_owner_command_set_relay*)," -ForegroundColor Red
    Write-Host "  or, for kiln_io_all_relays_off() ONLY, add a reasoned allowlist entry if" -ForegroundColor Red
    Write-Host "  this is genuinely a last-resort fail-safe path per kiln_io_owner.h:75-89." -ForegroundColor Red
    throw "$($violations.Count) direct relay-write call(s) found outside kiln_io_owner.c"
}

Write-Host "Relay write ownership check passed: kiln_io_set_relay_mask()/kiln_io_all_relays_off() called only from kiln_io_owner.c ($ownerCallsSeen call(s) there) or a reasoned allowlist entry ($($allowlist.Count) entries)."
exit 0
