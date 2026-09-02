# check_stack_margin_registration.ps1 -- keeps every long-lived internal
# FreeRTOS task registered with stack_margin.c (App/drivers), so its
# uxTaskGetStackHighWaterMark() reading stays reachable, and keeps
# STACK_MARGIN_MAX_TASKS ahead of the real registered-call-site count.
#
# DRAM_PSRAM_PLAN.md section 7 (2026-09-02, cap-raise pass): every one of
# that section's 7.3 relocation candidates (kiln_io_owner, thermo_owner,
# spi_owner, i2c_owner, screen_idle) already had a stack_margin_register()
# call site -- the actual blocker was STACK_MARGIN_MAX_TASKS sitting at
# 28/28 with the real boot-time registration count at 29 (i2c_owner_init()
# is shared by two live callers -- SX1509 and NS2009 -- so its one source
# call site fires twice), so one registration silently lost the coin flip
# every boot with no signal beyond an ESP_LOGE nobody was watching. This
# script is the standing guard against that reopening in either direction:
# a required task losing its stack_margin_register() call site (name
# missing below), or the cap falling behind the real call-site count again
# (same failure mode, same silent ESP_LOGE).
#
# Why a required-name list instead of comparing xTaskCreate*() counts to
# stack_margin_register() counts directly (the check_uri_handler_cap.ps1
# style): this codebase creates far more FreeRTOS tasks than it registers
# for stack-margin reporting -- short-lived, self-deleting UI/zone/wifi
# helper tasks (ui_page_network.c, zones_current_sweep_engine.c, and
# others) were never meant to be tracked here, and a blind create-vs-
# register count would either misfire on that legitimate debt or force
# this check to register tasks nobody asked this pass to touch. The
# required list below is exactly DRAM_PSRAM_PLAN.md's own tracked set
# (stack_margin.h's header-comment enumeration plus the section 7.3
# candidates this pass unblocked) -- the tasks this plan's own
# documentation says must be measurable. A future pass extending that
# tracked set should add the new name(s) here in the same commit that adds
# the stack_margin_register() call site, exactly like check_uri_handler_
# cap.ps1's route count is expected to move with wifi_provision_http.c.
#
# Usage: powershell -File tools\check_stack_margin_registration.ps1
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$driversDir = Join-Path $root "..\firmware\KilnFW\App\drivers"
$driversDir = (Resolve-Path $driversDir).Path
$mainFile = Join-Path $root "..\firmware\KilnFW\App\main.c"
$mainFile = (Resolve-Path $mainFile).Path
$capFile = Join-Path $driversDir "stack_margin.h"

# Same comment-stripping helper as check_uri_handler_cap.ps1 /
# check_bridge_reject_reason.ps1 / check_uart_version_independence.ps1
# (duplicated rather than imported -- this project has no shared
# PowerShell module mechanism).
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

# Every task DRAM_PSRAM_PLAN.md / stack_margin.h's own header comment tracks
# as needing a live stack_margin_register() call site. See this script's
# top comment for why this is a named list rather than a blind create-vs-
# register count.
$requiredNames = @(
    "autotune_engine", "boot_button", "danger_mode", "spi_owner", "i2c_owner",
    "gpio_probe", "kiln_io_owner", "lvgl", "recovery_exit", "ota_rollback_reboot",
    "ota_pico_rollback", "profile_executor", "profile_exec_wdt",
    "safety_owner_task", "safety_owner_evt", "safety_proto_rx", "safety_poll",
    "screen_idle", "telemetry_log", "thermo_owner", "link_watchdog",
    "bx_flash_worker", "info_uart_bridge", "system_uart_bridge", "httpd_worker",
    "uart_owner_task", "uart_owner_evt_task", "uart_proto_rx"
)

$sourceFiles = @(Get-ChildItem -Path $driversDir -Filter "*.c" -Recurse -File) + @(Get-Item -Path $mainFile)
if ($sourceFiles.Count -lt 5) {
    throw "check_stack_margin_registration.ps1: only $($sourceFiles.Count) .c file(s) found -- has the drivers directory moved? Update this script's target directory."
}

$registerPattern = 'stack_margin_register\s*\(\s*"([^"]+)"'
$registeredNames = New-Object System.Collections.Generic.List[string]
foreach ($f in $sourceFiles) {
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    foreach ($line in $codeLines) {
        foreach ($m in [regex]::Matches($line, $registerPattern)) {
            $registeredNames.Add($m.Groups[1].Value)
        }
    }
}
$registeredSet = [System.Collections.Generic.HashSet[string]]::new([string[]]$registeredNames)
$totalCalls = $registeredNames.Count

# Sanity floor: this codebase carries 25+ stack_margin_register() call sites
# today (2026-09-02). If the pattern above ever stops matching real calls --
# the function gets renamed, registration moves to a macro, etc. -- this
# check must go loud rather than quietly start passing on an undercount.
if ($totalCalls -lt 20) {
    throw "check_stack_margin_registration.ps1: only found $totalCalls stack_margin_register(`"...`") call site(s) under $driversDir and $mainFile, which is implausibly low (25+ expected as of 2026-09-02) -- the registration style has probably changed and this script has gone blind. Update its pattern before trusting its result."
}

Write-Host "Stack margin registration check: $totalCalls call site(s) found across $($sourceFiles.Count) file(s)."

$missing = @($requiredNames | Where-Object { -not $registeredSet.Contains($_) })
if ($missing.Count -gt 0) {
    Write-Host "STACK MARGIN REGISTRATION CHECK FAILED:" -ForegroundColor Red
    Write-Host "  The following task(s) are in this script's required-registration list" -ForegroundColor Red
    Write-Host "  (DRAM_PSRAM_PLAN.md section 7 / stack_margin.h's tracked set) but have" -ForegroundColor Red
    Write-Host "  NO stack_margin_register(`"<name>`", ...) call site anywhere under" -ForegroundColor Red
    Write-Host "  $driversDir or $mainFile :" -ForegroundColor Red
    foreach ($name in $missing) {
        Write-Host "    $name" -ForegroundColor Red
    }
    throw "A task this plan tracks was created without being (or lost being) registered for stack-margin measurement -- its high-water mark is no longer reachable. Either restore its stack_margin_register(`"<name>`", &handle, configured_bytes) call site (matching the xTaskCreate*() that creates it), or, if this task was deliberately retired, remove it from `$requiredNames in this script in the same commit."
}

# Cap check: STACK_MARGIN_MAX_TASKS must stay ahead of the real call-site
# count, same mechanism (and same past failure) as check_uri_handler_cap.ps1
# for config.max_uri_handlers. Registered names are found ABOVE by grep, not
# assumed; a call site whose stack_margin_register() call is reached more
# than once at runtime (i2c_owner_init(), shared by SX1509 and NS2009 --
# see stack_margin.h's cap-raise comment) still counts as 1 here, so this
# floor is a minimum, not the true worst-case boot-time count -- headroom
# below exists precisely to cover that gap too.
if (-not (Test-Path $capFile)) {
    throw "check_stack_margin_registration.ps1: expected cap file not found at $capFile -- has stack_margin.h moved? Update this script's path."
}
$capLines = Get-CodeOnlyLines -Path $capFile
$capDefinePattern = '#define\s+STACK_MARGIN_MAX_TASKS\s+(\d+)u?'
$capValue = $null
foreach ($line in $capLines) {
    if ($line -match $capDefinePattern) {
        $capValue = [int]$Matches[1]
        break
    }
}
if ($null -eq $capValue) {
    throw "check_stack_margin_registration.ps1: no '#define STACK_MARGIN_MAX_TASKS <N>' found in $capFile -- has it been renamed or restructured? Update this script's pattern."
}

Write-Host "STACK_MARGIN_MAX_TASKS = $capValue."

if ($capValue -lt $totalCalls) {
    Write-Host "STACK MARGIN CAP CHECK FAILED:" -ForegroundColor Red
    Write-Host "  STACK_MARGIN_MAX_TASKS = $capValue in $capFile" -ForegroundColor Red
    Write-Host "  but $totalCalls stack_margin_register() call sites exist." -ForegroundColor Red
    throw "STACK_MARGIN_MAX_TASKS ($capValue) is below the real call-site count ($totalCalls) -- stack_margin_register() will silently fail (ESP_LOGE, non-fatal) for whichever task registers after the registry fills, and the user-visible symptom is that task's high-water mark quietly never appearing in a GET_STACK_MARGIN reply. Raise STACK_MARGIN_MAX_TASKS in $capFile to at least $totalCalls plus headroom for tasks whose registration call site fires more than once at runtime (i2c_owner_init() etc.) and for future growth."
}

# 8-slot floor (not 4, like the URI handler check): this cap also has to
# absorb the "one source call site, multiple runtime callers" case
# (i2c_owner_init() today), which a source-level count cannot see.
$headroom = $capValue - $totalCalls
if ($headroom -lt 8) {
    Write-Host "Stack margin cap check passed, but headroom is thin ($headroom spare slot(s) for $totalCalls call sites against a cap of $capValue) -- the next task registered anywhere, or another shared-init-function double-registration like i2c_owner_init(), may need another bump soon." -ForegroundColor Yellow
} else {
    Write-Host "Stack margin cap check passed: $headroom spare slot(s)." -ForegroundColor Green
}
exit 0
