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
# call site fired twice), so one registration silently lost the coin flip
# every boot with no signal beyond an ESP_LOGE nobody was watching. This
# script is the standing guard against that reopening in either direction:
# a required task losing its stack_margin_register() call site (name
# missing below), or the cap falling behind the real call-site count again
# (same failure mode, same silent ESP_LOGE).
#
# 2026-09-03 follow-up: that "fires twice" call site didn't just spend a
# slot twice -- both firings registered under the SAME literal name
# ("i2c_owner"), with different configured stack sizes (SX1509 4096,
# NS2009 3072). A live stack-margin read cannot tell which of two same-
# named entries is which, and tools/PcTools/src/kilnctrl/
# stack_margin_baseline.py's worst_case_across_conditions() keys its
# result dict by name, so one of the two silently overwrote the other
# with no error -- exactly the "unqualified warning sends debugging to
# the wrong wire" class this repo has already shipped once (two link
# instances, one log tag). Fixed by moving registration out of the shared
# i2c_owner_init() and into each caller (SX1509.c, NS2009.c), each under
# its own distinct name -- "i2c_owner_sx1509" / "i2c_owner_ns2009" below.
# This script's duplicate-name check (further down) is the standing guard
# against that reopening: it fails loud if any two stack_margin_register()
# call sites ever share a literal name again.
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
#        (-DriversDir <path> to smoke-test against a simulated tree)
param(
    [string]$DriversDir
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
if ($DriversDir) {
    $driversDir = (Resolve-Path $DriversDir).Path
} else {
    $driversDir = Join-Path $root "..\firmware\KilnFW\App\drivers"
    $driversDir = (Resolve-Path $driversDir).Path
}
$appDir = Join-Path $root "..\firmware\KilnFW\App"
$appDir = (Resolve-Path $appDir).Path

# Resolve a bare basename anywhere under $driversDir -- agnostic to the
# upcoming move of every drivers/*.c/.h file into layer subdirectories.
function Resolve-DriverFile {
    param([string]$DriversDir, [string]$BaseName)
    $found = @(Get-ChildItem -Path $DriversDir -Filter $BaseName -File -Recurse)
    if ($found.Count -eq 0) {
        throw "check_stack_margin_registration.ps1: expected file '$BaseName' not found anywhere under $DriversDir -- has it moved or been renamed?"
    }
    if ($found.Count -gt 1) {
        $paths = ($found | ForEach-Object { $_.FullName }) -join ", "
        throw "check_stack_margin_registration.ps1: '$BaseName' matched more than one file under $DriversDir ($paths) -- cannot tell which one is the real header."
    }
    return $found[0].FullName
}

$capFile = Resolve-DriverFile -DriversDir $driversDir -BaseName "stack_margin.h"

# main.c itself was split into several main_*.c files (main_boot_early.c,
# main_bridges_bringup.c, main_control_bringup.c, main_network_http.c) --
# app_main()'s bring-up sequence is spread across them, and so is its
# stack_margin_register() call sites (uart_owner_task/uart_owner_evt_task/
# uart_proto_rx live in main_network_http.c, registered against the real
# task handles right after uart_owner_init()/uart_protocol_init() create
# them). This script used to look only at main.c and went blind to that
# split: the three registrations were live at runtime the whole time, this
# check just never saw them, and reported the tasks as unregistered. Glob
# every top-level App/*.c file (non-recursive -- App/drivers is handled,
# recursively, by $driversDir above) rather than naming main.c alone, so a
# future split doesn't reopen the same blind spot.
$mainFiles = @(Get-ChildItem -Path $appDir -Filter "*.c" -File)

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
    "autotune_engine", "boot_button", "danger_mode", "spi_owner",
    "i2c_owner_sx1509", "i2c_owner_ns2009",
    "gpio_probe", "kiln_io_owner", "lvgl", "recovery_exit", "ota_rollback_reboot",
    "ota_pico_rollback", "profile_executor", "profile_exec_wdt",
    "safety_owner_task", "safety_owner_evt", "safety_proto_rx", "safety_poll",
    "screen_idle", "telemetry_log", "thermo_owner", "link_watchdog",
    "bx_flash_worker", "info_uart_bridge", "system_uart_bridge", "httpd_worker",
    "uart_owner_task", "uart_owner_evt_task", "uart_proto_rx"
)

# HAL Phase 1a (WP1) moved esp_spi_owner.c/i2c_owner.c/uart_owner.c/
# uart_protocol.c out of drivers/ entirely, into firmware/hwAbstraction/esp/
# (colocated with the hal_*_esp.c backend files). Those owner files kept
# their own stack_margin_register() call sites byte-identical (esp_spi_owner.c
# registers "spi_owner" directly) -- they did not move to the accessor
# pattern used by the hal_*_esp.c backends below, because they are the same
# KilnFW-owned module the drivers/ scan always covered, just relocated. Miss
# this directory and "spi_owner" silently drops off $registeredNames even
# though the call site is still live at runtime -- scan it same as drivers/.
$hwAbstractionEspOwnersDir = Join-Path $root "..\firmware\hwAbstraction\esp"
$hwAbstractionOwnerSourceFiles = @()
if (Test-Path $hwAbstractionEspOwnersDir) {
    $hwAbstractionEspOwnersDir = (Resolve-Path $hwAbstractionEspOwnersDir).Path
    $hwAbstractionOwnerSourceFiles = @(Get-ChildItem -Path $hwAbstractionEspOwnersDir -Filter "*.c" -Recurse -File)
}

$sourceFiles = @(Get-ChildItem -Path $driversDir -Filter "*.c" -Recurse -File) + $mainFiles + $hwAbstractionOwnerSourceFiles
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
    throw "check_stack_margin_registration.ps1: only found $totalCalls stack_margin_register(`"...`") call site(s) under $driversDir and $appDir, which is implausibly low (25+ expected as of 2026-09-02) -- the registration style has probably changed and this script has gone blind. Update its pattern before trusting its result."
}

Write-Host "Stack margin registration check: $totalCalls call site(s) found across $($sourceFiles.Count) file(s)."

$missing = @($requiredNames | Where-Object { -not $registeredSet.Contains($_) })
if ($missing.Count -gt 0) {
    Write-Host "STACK MARGIN REGISTRATION CHECK FAILED:" -ForegroundColor Red
    Write-Host "  The following task(s) are in this script's required-registration list" -ForegroundColor Red
    Write-Host "  (DRAM_PSRAM_PLAN.md section 7 / stack_margin.h's tracked set) but have" -ForegroundColor Red
    Write-Host "  NO stack_margin_register(`"<name>`", ...) call site anywhere under" -ForegroundColor Red
    Write-Host "  $driversDir or $appDir (App/*.c, non-recursive) :" -ForegroundColor Red
    foreach ($name in $missing) {
        Write-Host "    $name" -ForegroundColor Red
    }
    throw "A task this plan tracks was created without being (or lost being) registered for stack-margin measurement -- its high-water mark is no longer reachable. Either restore its stack_margin_register(`"<name>`", &handle, configured_bytes) call site (matching the xTaskCreate*() that creates it), or, if this task was deliberately retired, remove it from `$requiredNames in this script in the same commit."
}

# Duplicate-name check: two DIFFERENT stack_margin_register() call sites
# must never share a literal name. This is the standing guard for the
# 2026-09-03 i2c_owner bug (see this script's top comment): SX1509.c and
# NS2009.c each drive their own i2c_owner task, at different configured
# stack sizes, but a shared call site inside i2c_owner_init() registered
# both under the one literal name "i2c_owner" -- a stack-margin reader
# could not tell which of the two entries was which, and
# stack_margin_baseline.py's worst_case_across_conditions() (keyed by
# name) silently dropped one of them. A single call site legitimately
# firing more than once at runtime under the SAME name (none left today,
# now that i2c_owner_init() itself no longer registers) would still only
# appear once in this source-level scan, so this check catches the
# source-level half of that bug class: two call sites, same string.
$duplicateNames = @(
    $registeredNames | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name }
)
if ($duplicateNames.Count -gt 0) {
    Write-Host "STACK MARGIN DUPLICATE NAME CHECK FAILED:" -ForegroundColor Red
    Write-Host "  The following name(s) are passed to stack_margin_register() by more" -ForegroundColor Red
    Write-Host "  than one call site under $driversDir or $appDir (App/*.c, non-recursive) :" -ForegroundColor Red
    foreach ($name in $duplicateNames) {
        Write-Host "    $name" -ForegroundColor Red
    }
    throw "Two stack_margin_register() call sites share a literal name -- a live GET_STACK_MARGIN report cannot tell the resulting entries apart, and PC-side tooling keyed by name (stack_margin_baseline.py's worst_case_across_conditions()) will silently drop one. Give each call site its own distinct name (STACK_MARGIN_NAME_MAX in stack_margin.h caps names at 19 usable characters)."
}

# Cap check: STACK_MARGIN_MAX_TASKS must stay ahead of the real call-site
# count, same mechanism (and same past failure) as check_uri_handler_cap.ps1
# for config.max_uri_handlers. Registered names are found ABOVE by grep, not
# assumed. As of the 2026-09-03 fix, every stack_margin_register() call
# site fires exactly once at runtime (i2c_owner_init() itself no longer
# registers; SX1509.c and NS2009.c each register their own owner task
# under their own name after calling it) -- but a future shared-init
# helper could reintroduce a call site that fires more than once, and a
# source-level grep can't see that. Headroom below stays deliberately
# generous to cover that class, not just future task growth.
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

# --- hwAbstraction extension ---------------------------------------------
#
# firmware/hwAbstraction/**'s ESP/Pico backends (hal_*_esp.c / hal_*_pico.c --
# pure hardware plumbing, no pre-existing KilnFW/SaftyFW ties) create their
# own FreeRTOS tasks (xTaskCreate*) but hwAbstraction is a one-way boundary
# for THEM -- a backend must never #include a KilnFW header such as
# stack_margin.h, so it cannot call stack_margin_register() itself. The
# accessor pattern instead: each backend that creates a task exposes a
# hal_<name>_get_task_handle() function in the SAME file, and a KilnFW-side
# caller (outside hwAbstraction) is expected to register that handle.
#
# The files HAL Phase 1a (WP1/WP2) relocated INTO hwAbstraction --
# esp_spi_owner.c/owner_slot_pool.c/i2c_owner.c/uart_owner.c/uart_protocol.c
# under esp/, spi_owner.c/uart_owner.c/uart_owner_tx_policy.c under pico/ --
# are a different thing: they are the same KilnFW/SaftyFW-owned owner
# modules the drivers/ (or src/tasks/) scan always covered, just relocated
# byte-identical by the move. They already call stack_margin_register()
# directly (esp_spi_owner.c registers "spi_owner") and that is not a fresh
# boundary violation to fix here -- it is pre-existing, accepted behavior
# that predates the move and is out of scope for Phase 1a. Applying the
# backend accessor rule to them would fail on every one of them the day
# after the move, for code nobody touched. The two file families are told
# apart by name: "owner" files match *_owner.c/.h, *_owner_*.c/.h,
# owner_slot_pool.*, or uart_protocol.* (all pre-existing KilnFW/SaftyFW
# modules); everything else under hwAbstraction/ -- the hal_*.c/.h backend
# files -- is held to the accessor/no-include rule below. If a future pass
# ever refactors an owner file itself onto the accessor pattern, drop it
# from $ownerFileNamePattern in the same commit that removes its direct
# stack_margin_register()/stack_margin.h use.
#
# Two things are enforced here, backend files only:
#
#   1. any backend file under firmware/hwAbstraction/ that calls
#      xTaskCreate*() must define a function matching
#      hal_[a-z0-9_]+_get_task_handle in the SAME file -- otherwise the
#      task it creates has no way for KilnFW/SaftyFW to ever reach its
#      handle, and it fails naming the file.
#   2. no backend file under firmware/hwAbstraction/ may #include
#      stack_margin.h -- doing so would be the boundary violation this
#      split exists to prevent (hwAbstraction reaching into KilnFW).
#
# If the accessor has not landed in a given backend yet, this legitimately
# fails naming that file -- that is not a false positive, it is exactly
# what this check exists to report until the accessor call site is added.
# Matched against the file's basename only (not full path): anything with
# "owner" anywhere in the name (esp_spi_owner.c, owner_slot_pool.c,
# i2c_owner.c, uart_owner.c, uart_owner_tx_policy.c, spi_owner.c, ...) or
# named uart_protocol.*.
#
# Previously this was the loose regex 'owner|^uart_protocol\.(c|h)$', which
# matches "owner" ANYWHERE in a basename -- broad enough to silently exempt
# a future non-owner backend file that merely has "owner" in its name (e.g.
# a hypothetical "screen_owner_button.c" hal backend) from the accessor/
# no-include rule it should be held to. Replaced with an explicit basename
# allowlist of the 8 owner .c files (and their .h counterparts) that exist
# today, per this section's comment above:
#   esp/:  esp_spi_owner.c, owner_slot_pool.c, i2c_owner.c, uart_owner.c,
#          uart_protocol.c
#   pico/: spi_owner.c, uart_owner.c, uart_owner_tx_policy.c
# (esp's and pico's uart_owner.c share a basename and are both covered by
# the single "uart_owner.c"/"uart_owner.h" entries below, matching the
# pre-existing basename-only semantics.) A file added later must be named
# exactly one of these to be treated as a pre-existing owner module --
# adding "owner" to a new name no longer exempts it.
$ownerFileNames = @(
    "esp_spi_owner.c", "esp_spi_owner.h",
    "owner_slot_pool.c", "owner_slot_pool.h",
    "i2c_owner.c", "i2c_owner.h",
    "uart_owner.c", "uart_owner.h",
    "uart_protocol.c", "uart_protocol.h",
    "spi_owner.c", "spi_owner.h",
    "uart_owner_tx_policy.c", "uart_owner_tx_policy.h"
)
$hwAbstractionRoot = Join-Path $root "..\firmware\hwAbstraction"
if (Test-Path $hwAbstractionRoot) {
    $hwAbstractionRootResolved = (Resolve-Path $hwAbstractionRoot).Path
    $hwAbstractionFiles = @(Get-ChildItem -Path $hwAbstractionRootResolved -Recurse -File -Include "*.c", "*.h")

    $hwAbstractionViolations = @()
    $accessorPattern = '\bhal_[a-z0-9_]+_get_task_handle\b'
    $createPattern = '\bxTaskCreate\w*\s*\('
    $includePattern = '#\s*include\s*[<"]stack_margin\.h[>"]'

    foreach ($f in $hwAbstractionFiles) {
        if ($ownerFileNames -contains $f.Name) {
            # Pre-existing owner module relocated byte-identical by HAL
            # Phase 1a/1b -- not held to the backend accessor/no-include
            # rule. See this section's top comment.
            continue
        }
        $codeLines = Get-CodeOnlyLines -Path $f.FullName
        $relPath = "firmware/hwAbstraction/" + (($f.FullName.Substring($hwAbstractionRootResolved.Length + 1)) -replace '\\', '/')

        $createsTask = $false
        $hasAccessor = $false
        $includesStackMargin = $false
        foreach ($line in $codeLines) {
            if ($line -match $createPattern) { $createsTask = $true }
            if ($line -match $accessorPattern) { $hasAccessor = $true }
            if ($line -match $includePattern) { $includesStackMargin = $true }
        }

        if ($createsTask -and -not $hasAccessor) {
            $hwAbstractionViolations += "${relPath}: calls xTaskCreate*() but defines no hal_..._get_task_handle() accessor in the same file -- KilnFW has no way to reach this task's handle to register it for stack-margin reporting."
        }
        if ($includesStackMargin) {
            $hwAbstractionViolations += "${relPath}: #includes stack_margin.h -- hwAbstraction must not include KilnFW headers (one-way boundary violation)."
        }
    }

    if ($hwAbstractionViolations.Count -gt 0) {
        Write-Host ""
        Write-Host "HWABSTRACTION STACK-MARGIN BOUNDARY CHECK FAILED:" -ForegroundColor Red
        foreach ($v in $hwAbstractionViolations) {
            Write-Host "  $v" -ForegroundColor Red
        }
        throw "$($hwAbstractionViolations.Count) firmware/hwAbstraction/ file(s) violate the stack-margin accessor contract or the one-way KilnFW-include boundary -- see messages above."
    }
    Write-Host "hwAbstraction stack-margin boundary check passed: $($hwAbstractionFiles.Count) file(s) scanned under $hwAbstractionRootResolved." -ForegroundColor Green
} else {
    Write-Host "WARNING: $hwAbstractionRoot not found -- skipping hwAbstraction stack-margin boundary check." -ForegroundColor Yellow
}

exit 0
