# checkcache: ok
# check_safety_call_results_checked.ps1 -- a named set of safety-relevant
# calls (relay writes/all-off, heat-enable requests/releases to the safety
# processor) must have their esp_err_t result CAPTURED into a variable at
# every call site outside their own definition -- never called as a bare
# statement whose return value is silently discarded.
#
# WHY THIS EXISTS. 2026-08-31 seed bug (danger_mode.c, commit 2bcdc2d):
# danger_mode_stop() and its timeout task cast kiln_io_owner_command_all_
# relays_off()'s and safety_link_request_enable()'s results to (void) and
# then logged "relays dropped ... no reboot" UNCONDITIONALLY. An owner-queue
# timeout (ESP_ERR_TIMEOUT, a real, reachable post_and_wait() failure) looked
# identical in the log to relays actually opening. The same audit pass found
# a sibling, zones_current_sweep_engine.c's zone_sweep_force_relays_off(),
# which did not even cast the result to (void) -- it never captured it at
# all -- and two more (heat_enable.c's two release sends). All four are now
# fixed; this check exists so a fifth does not ship the same way.
#
# WIDENED 2026-09-07 (docs/audits/persist_save_logging_2026-09-07.md): the
# same discard shape shows up on the persist/save side, not just relay
# writes -- zones_config_store.c's zone_normals_save() logged nothing on
# failure at all (root cause of project_nvs_key_too_long_zone_normals: every
# zone_normals_set()/zone_ct_map_set()/zone_k_ct_set() write silently failed
# on real hardware for weeks with zero log trace). Added nvs_save_store() and
# zone_normals_save() to $patterns below so a persisted-state write whose
# result is dropped on the floor fails this check the same way an unchecked
# relay write does. Both names are reused as `static` function names in more
# than one file (kiln_cfg_store.c and safety_cfg_store.c each define their
# own nvs_save_store()) -- the check is text-pattern based, not symbol-based,
# so this is fine: every call site of either symbol must still assign its
# result, in every file.
#
# WHAT THIS CHECKS. Every call to one of the functions in $patterns below,
# anywhere under firmware/KilnFW/App (excluding App/test/ -- test doubles and
# fakes are not a production code path), MUST be immediately preceded on the
# same logical statement by an assignment to a named variable (e.g.
# "esp_err_t err = kiln_io_owner_command_all_relays_off();"), UNLESS the
# call site is on the allowlist below with a stated reason. A bare
# "fn();" or "(void)fn();" call is a violation. This does NOT check that the
# captured value is actually inspected afterward -- that is a much harder
# static property (would need real control-flow analysis to catch "captured
# but never read" reliably without false positives on, e.g., a value read
# only via a later helper call) and the existing host tests + code review are
# the second line of defense for that half; the assignment requirement alone
# is what would have caught 3 of this pass's 4 findings (the 4th,
# zones_current_sweep_engine.c, had no assignment at all -- the more blatant
# shape).
#
# ALLOWLISTED BY DESIGN, not by exception: profile_executor.c's guard-9 /
# watchdog-retry paths (kiln_io_all_relays_off() called from the FAULT and
# RETRY_RELAYS_OFF watchdog actions) are genuinely fire-and-forget --
# LINK_PROTOCOL.md sec 8's "dropped and retried until the write succeeds"
# means every WATCHDOG_CHECK_PERIOD_MS tick retries unconditionally
# regardless of the previous attempt's outcome, and no log or state anywhere
# on that path asserts the write succeeded (FAULTED is set either way). A
# result that would only ever be discarded again next tick is not a defect
# this check exists to catch, and demanding a variable there would be
# decorative. Allowlisted explicitly below, by file:line-pattern, not by a
# blanket file exclusion -- a NEW unchecked call added anywhere else in the
# same file still fails.
#
# Comments are stripped first (same Get-CodeOnlyLines helper as this repo's
# other check_*.ps1 scripts) so a comment merely discussing one of these
# calls is not counted as a real call.
#
# Usage: powershell -File tools\check_safety_call_results_checked.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$appDir = Join-Path $root "firmware\KilnFW\App"
if (-not (Test-Path $appDir)) {
    throw "check_safety_call_results_checked: $appDir not found -- has KilnFW's App/ moved?"
}
$appDirResolved = (Resolve-Path $appDir).Path

# Identical to this repo's other check_*.ps1 scripts' helper (e.g.
# tools/check_uri_handler_cap.ps1) -- strips block and line comments while
# preserving line NUMBERS (a blank line for each comment-only line, a
# truncated line for a comment that starts mid-line), which a naive
# multi-line regex replace does not: collapsing a block comment's internal
# newlines shifts every later line's reported number.
function Get-CodeOnlyLines {
    param([string]$Path)
    $inBlockComment = $false
    $lines = Get-Content -Path $Path
    $result = @()
    foreach ($line in $lines) {
        $code = $line
        if ($inBlockComment) {
            $endIdx = $code.IndexOf("*/")
            if ($endIdx -ge 0) {
                $code = $code.Substring($endIdx + 2)
                $inBlockComment = $false
            } else {
                $result += ""
                continue
            }
        }
        $lineCommentIdx = $code.IndexOf("//")
        if ($lineCommentIdx -ge 0) {
            $code = $code.Substring(0, $lineCommentIdx)
        }
        while ($true) {
            $startIdx = $code.IndexOf("/*")
            if ($startIdx -lt 0) { break }
            $endIdx = $code.IndexOf("*/", $startIdx)
            if ($endIdx -ge 0) {
                $code = $code.Substring(0, $startIdx) + $code.Substring($endIdx + 2)
            } else {
                $code = $code.Substring(0, $startIdx)
                $inBlockComment = $true
                break
            }
        }
        $result += $code
    }
    return $result
}

# file (relative, forward-slash) -> array of line-content regex fragments
# that are allowed to appear WITHOUT a preceding assignment. Matched against
# the stripped line itself (post comment-removal), not just file:line, so a
# genuinely new unchecked call elsewhere in the same file still fails.
$allowlist = @(
    @{ RelPath = "firmware/KilnFW/App/drivers/control/profile_executor.c"; Snippet = "kiln_io_all_relays_off(s_exec.io);" }
)

function Test-Allowlisted {
    param([string]$RelPath, [string]$Line)
    foreach ($entry in $allowlist) {
        if ($entry.RelPath -ieq $RelPath -and $Line.Contains($entry.Snippet)) { return $true }
    }
    return $false
}

# The named safety-relevant call surface: relay commands and heat-authority
# requests whose failure can leave a relay closed, heat commanded, or the
# safety processor's enable line standing when the board believes it is not.
# Deliberately narrow -- this is the exact set the 2026-08-31 audit examined,
# not every esp_err_t-returning function in the tree (which would make this
# check noisy well past the point of being worth running).
$patterns = @(
    "kiln_io_owner_command_all_relays_off",
    "kiln_io_owner_command_set_relay_mask_authorized",
    "kiln_io_owner_command_set_relay_mask",
    "kiln_io_owner_command_set_io",
    "kiln_io_all_relays_off",
    "safety_link_request_enable",
    "nvs_save_store",
    "zone_normals_save",
    # WIDENED 2026-09-09 (opus review defect A): the commissioning write path's
    # apply_pairs() called estop_verification_clear() as a bare statement and
    # then returned true, so a failed clear left a standing "verified"
    # E-stop record while the POST reported success -- the same discard shape
    # as the 2026-08-31 findings, on the record that stands in for wiring
    # firmware cannot observe. Its own header now requires the caller to
    # surface a non-ESP_OK; this line is the mechanical half of that.
    "estop_verification_clear"
)

# A definition line looks like "esp_err_t kiln_io_owner_command_all_relays_off(void)"
# or "kiln_io_owner_relay_result_t kiln_io_owner_command_set_relay_mask(uint8_t ..."
# (that function's actual return type) -- built per-function below rather
# than one generic regex, since a definition is the only place a RETURN TYPE
# TOKEN sits immediately before the function's own name at the start of a
# line; a call site never does (it is always preceded by "=", "return",
# "if (", "(void)", indentation into a statement, etc., never a bare type
# name).
function Test-IsDefinitionLine {
    param([string]$Line, [string]$Fn)
    return $Line -match "^\s*(static\s+)?[A-Za-z_][A-Za-z0-9_]*\s+\**$Fn\s*\("
}

$sourceFiles = Get-ChildItem -Path $appDirResolved -Recurse -File -Include "*.c" |
    Where-Object { $_.FullName -notmatch '[\\/]test[\\/]' -and $_.FullName -notmatch '[\\/]build[\\/]' }

if ($sourceFiles.Count -lt 50) {
    throw "check_safety_call_results_checked: only $($sourceFiles.Count) .c file(s) found under $appDirResolved (excluding test/) -- implausibly low, has the tree moved? This check would pass vacuously."
}

$violations = @()
$callsSeen = 0

foreach ($f in $sourceFiles) {
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    $rel = "firmware/KilnFW/App/" + ($f.FullName.Substring($appDirResolved.Length + 1) -replace '\\', '/')

    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        $line = $codeLines[$i]
        foreach ($fn in $patterns) {
            if ($line -notmatch "\b$fn\s*\(") { continue }
            if (Test-IsDefinitionLine -Line $line -Fn $fn) { continue }
            $callsSeen++
            # A wrapped call ("esp_err_t err =\n    fn(...)" or "TYPE rr =\n
            # fn(...)") puts the assignment on the PREVIOUS line -- join it
            # in before testing, since this check works line-by-line
            # otherwise.
            $joined = if ($i -gt 0) { $codeLines[$i - 1] + " " + $line } else { $line }
            # Checked shapes: "TYPE <name> = fn(...)", "<name> = fn(...)"
            # (re-assignment to an already-declared variable), "return
            # fn(...)" (propagating the result to this function's own
            # caller, who is then responsible for checking it), or the call
            # appearing as the right-hand side of any "= ... fn(" -- possibly
            # on the previous line for a wrapped statement. A bare "fn(...)"
            # or "(void)fn(...)" statement is not.
            $isAssigned = ($joined -match "=\s*(\([^)]*\)\s*)?$fn\s*\(") -or
                          ($line -match "\breturn\s+$fn\s*\(")
            if ($isAssigned) { continue }
            if (Test-Allowlisted -RelPath $rel -Line $line) { continue }
            $violations += "${rel}:$($i + 1): call to $fn() does not capture its return value into a variable -- $($line.Trim())"
        }
    }
}

if ($callsSeen -lt 5) {
    throw "check_safety_call_results_checked: found only $callsSeen call site(s) to the named safety-relevant functions across the whole tree -- implausibly low, has this check gone blind (functions renamed/moved)?"
}

if ($violations.Count -gt 0) {
    Write-Host "SAFETY CALL RESULT CHECK FAILED:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  $v" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A relay-off/relay-write/heat-enable call whose result is not captured" -ForegroundColor Red
    Write-Host "  cannot be checked, and a nearby log or state update can then assert an" -ForegroundColor Red
    Write-Host "  outcome (relay open, heat enable released) that was never confirmed --" -ForegroundColor Red
    Write-Host "  see this script's own header comment for the 2026-08-31 seed bug" -ForegroundColor Red
    Write-Host "  (danger_mode.c, commit 2bcdc2d) this check exists to catch a repeat of." -ForegroundColor Red
    Write-Host "  Capture the result into a named esp_err_t and log ESP_LOGE naming it on" -ForegroundColor Red
    Write-Host "  failure, or add a reasoned allowlist entry to this script if the call is" -ForegroundColor Red
    Write-Host "  genuinely a fire-and-forget retry path (see the header comment's example)." -ForegroundColor Red
    throw "$($violations.Count) unchecked safety-relevant call(s) found"
}

Write-Host "Safety call result check passed: $callsSeen call site(s) to the named safety-relevant functions, all captured into a variable or reasoned-allowlisted ($($allowlist.Count) entries)."
exit 0
