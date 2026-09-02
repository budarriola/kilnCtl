# check_ui_shell_layout.ps1 -- the lint half of the two-layer pattern this
# repo already uses for a rule with no compiler to enforce it (see
# check_ui_budget_asserts.ps1's own header comment for the sibling of this
# script, and firmware/SimFW/tools/check_single_owner.ps1's DMA budget
# section for the pattern's origin). ui_responsive_sweep.mjs (the
# AUTHORITATIVE check) can only tell you a page overflowed or an element
# collided at ONE OF ITS SIX widths; it cannot tell you *why* a later edit
# stopped passing, and -- more to the point here -- it cannot fail at all
# if a WEB_UI_RESPONSIVE_PLAN.md sec 6 rule is deleted outright and the
# page quietly falls back to shrink-to-fit-content sizing that still
# happens to pass every numeric assertion at every swept width. A rule
# that only ever earns width, and never actively causes overflow, can be
# removed by an unrelated cleanup pass with the sweep staying green the
# whole time. This script greps for the exact rules sec 6 added and fails
# loudly if any is missing, the same no-compiler mirror check_ui_budget_
# asserts.ps1 already does for the _Static_assert budget guards.
#
# Proven to fail (2026-09-02, this script's own authoring pass): each
# required line below was temporarily deleted from its file one at a time,
# this script was re-run and confirmed to report exactly that line missing,
# then the deletion was reverted. See the task's own report for the exact
# mutation/output pairs -- not re-run automatically here because that would
# require this script to mutate shipped source, which is worse than not
# proving it at all.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_ui_shell_layout.ps1
$ErrorActionPreference = "Stop"

$driversRoot = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "App\drivers"
if (-not (Test-Path $driversRoot)) {
    throw "check_ui_shell_layout.ps1: expected source tree not found at $driversRoot"
}

# Each entry: the file, and the exact literal substring(s) it must still
# contain. Whitespace is NOT normalized here (unlike check_ui_budget_
# asserts.ps1) -- these are one-line CSS declarations, not multi-line C
# expressions, so an exact match is the more precise check and a reflow
# would be a real, worth-noticing change to the rule's shape.
$required = @(
    @{
        File = "theme.css"
        Rules = @(
            "--kc-shell-max: clamp(480px, 90vw, 1100px);",
            "container-type: inline-size; container-name: kc-card;",
            "@container kc-card (max-width: 240px) {"
        )
    },
    @{
        File = "main_page.html"
        Rules = @(
            "max-width: var(--kc-shell-max);",
            "#channels { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 0.5em; }"
        )
    },
    @{
        File = "zones_page.html"
        Rules = @(
            "max-width: var(--kc-shell-max);",
            "#zones { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap: 0.7em; }"
        )
    },
    @{
        File = "wifi_provision_page.html"
        Rules = @(
            "max-width: var(--kc-shell-max);"
        )
    },
    @{
        File = "ota_page.html"
        Rules = @(
            "max-width: var(--kc-shell-max);"
        )
    },
    @{
        File = "profiles_page.html"
        Rules = @(
            "max-width: var(--kc-shell-max);"
        )
    },
    @{
        File = "safety_commissioning_page.html"
        Rules = @(
            "max-width: var(--kc-shell-max);"
        )
    },
    @{
        File = "backup_page.html"
        Rules = @(
            "width: min(100% - 2rem, var(--kc-shell-max));"
        )
    },
    @{
        File = "diagnostics_page.html"
        Rules = @(
            "width: min(100% - 2rem, var(--kc-shell-max));"
        )
    },
    @{
        File = "safety_page.html"
        Rules = @(
            "width: min(100% - 2rem, var(--kc-shell-max));"
        )
    },
    @{
        File = "safety_config_page.html"
        Rules = @(
            "width: min(100% - 2rem, var(--kc-shell-max));"
        )
    },
    @{
        File = "readiness_page.html"
        Rules = @(
            "width: min(100% - 2rem, var(--kc-shell-max));"
        )
    }
)

$missing = @()
foreach ($entry in $required) {
    $path = Join-Path $driversRoot $entry.File
    if (-not (Test-Path $path)) {
        $missing += "$($entry.File): file not found at $path"
        continue
    }
    $text = Get-Content -Raw -LiteralPath $path
    foreach ($rule in $entry.Rules) {
        if ($text -notlike "*$rule*") {
            $missing += "$($entry.File): missing required rule -- $rule"
        }
    }
}

if ($missing.Count -gt 0) {
    Write-Host "check_ui_shell_layout.ps1: FAILED -- $($missing.Count) required WEB_UI_RESPONSIVE_PLAN.md sec 6 rule(s) missing:" -ForegroundColor Red
    foreach ($m in $missing) { Write-Host "  $m" -ForegroundColor Red }
    exit 1
}

Write-Host "check_ui_shell_layout.ps1: OK -- all $(($required | ForEach-Object { $_.Rules.Count } | Measure-Object -Sum).Sum) sec 6 shell/grid/container-query rules present." -ForegroundColor Green
exit 0
