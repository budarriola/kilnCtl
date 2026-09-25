# check_stack_margin_registration.ps1 -- keeps every long-lived internal
# FreeRTOS task registered with stack_margin.c (App/drivers), so its
# uxTaskGetStackHighWaterMark() reading stays reachable, and keeps
# STACK_MARGIN_MAX_TASKS ahead of the real registered-call-site count.
#
# DRAM_PSRAM_STATUS.md section 7 (2026-09-02, cap-raise pass): every one of
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
# required list below is exactly DRAM_PSRAM_STATUS.md's own tracked set
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

# Every task DRAM_PSRAM_STATUS.md / stack_margin.h's own header comment tracks
# as needing a live stack_margin_register() call site. See this script's
# top comment for why this is a named list rather than a blind create-vs-
# register count.
$requiredNames = @(
    "autotune_engine", "boot_button", "danger_mode", "spi_owner",
    "i2c_owner_sx1509",
    "i2c_owner_ns2009",  # liveness: config -- only created if a runtime i2c probe finds an NS2009 (NS2009.c); this bench has an FT6336U instead
    "kiln_io_owner", "lvgl",
    "gpio_probe",  # liveness: config -- only created when CONFIG_KILNCTL_ENABLE_GPIO_PROBE=y (Kconfig)
    "backlight_pwm",  # liveness: config -- only created when CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE=y (Kconfig, defaults y); found missing from this list by check_stack_task_table_consistency.ps1 (2026-09-24) despite having a real stack_margin_register() call site and a check_all_task_stack_budgets.py TASKS/CEILING_BYTES row all along
    "recovery_exit",  # liveness: on-demand -- transient task an HTTP handler (ota_http_recovery.c) creates per POST /api/ota/esp/recovery_exit call
    "ota_rollback_reboot",  # liveness: on-demand -- transient task ota_http_esp.c's rollback handler creates on demand
    "ota_pico_rollback",  # liveness: on-demand -- transient task ota_http_pico.c's rollback handler creates on demand
    "profile_executor", "profile_exec_wdt",
    "safety_owner_evt", "safety_proto_rx", "safety_poll",
    "screen_idle", "telemetry_log", "thermo_owner", "link_watchdog",
    "bx_flash_worker", "info_uart_bridge", "system_uart_bridge", "httpd_worker",
    "uart_owner_evt_task", "uart_proto_rx",
    "thermo_uart_bridge", "touch_uart_bridge", "ui_test_uart_bridge",
    "io_uart_bridge", "uart_log_bridge", "safety_uart_bridge",

    # docs/KILN_PROFILES_PLAN.md item 5 -- the dedicated kiln-config swap
    # worker (kiln_cfg_swap_worker.c), added here in the SAME commit as its
    # stack_margin_register() call site per this script's own header rule.
    # It runs the deepest non-httpd call chain that feature has (a stack-
    # allocated kiln_cfg_swap_pending_t is ~1.7 kB on its own frame), which
    # is precisely why it is not allowed on the shared httpd worker stack --
    # so its live margin has to stay measurable.
    "kiln_cfg_swap",

    # Review finding at 4ddad119 (CLAUDE.md "Register every new task for
    # stack-margin reporting"): zones_current_sweep_task.c's one-shot,
    # self-deleting zone_sweep task had a stack_margin_register() call site
    # added and no ceiling check of its own -- same on-demand shape as
    # recovery_exit/ota_pico_rollback/ota_rollback_reboot below (a POST
    # handler, here zones_current_sweep_start(), can (re)start it any number
    # of times per boot), registered under the same literal name as its own
    # FreeRTOS task name via &s_sweep.task, the slot it is already created
    # into and cleared back to NULL from when it self-deletes.
    "zone_sweep",  # liveness: on-demand -- transient task an HTTP handler (zones_http.c's sweep_start_post_handler()) creates per POST /api/zones/sweep/start call

    # docs/PICO_AUTO_UPDATE_PLAN.md G3 -- the one-shot boot-time Pico
    # auto-update evaluator (App/drivers/net/pico_auto_update_boot.c). It
    # self-deletes once it has a verdict, so it is short-lived rather than a
    # service, but it runs on EVERY boot and does a bounded link wait plus a
    # flash scan before deciding, which is exactly the shape whose high-water
    # mark has to stay measurable. Tagged boot-once (not config/on-demand):
    # ABSENT would still mean its stack_margin_register() call site never
    # fired at all, which is a fault; DEAD after boot is its normal state.
    "pico_auto_update"  # liveness: boot-once
)
# 2026-09-08: the six UART bridge tasks above (thermo/touch/ui_test/io/
# uart_log/safety) were long-lived (`while (true)`, never self-deleting)
# peers of system_uart_bridge/info_uart_bridge that existed and ran every
# boot with NO stack_margin_register() call site at all -- this
# required-name list, being hand-maintained, could not see that gap by
# construction (docs/audits/2026-09-08-stack-margin-audit.md, "Unregistered
# long-lived tasks" -- that audit named five; safety_uart_bridge was found
# and fixed in the same pass, same bug, same file family). See the
# create-vs-register cross-check further down for the mechanical guard
# against this happening again for a task this list doesn't yet name.
# safety_owner_task / uart_owner_task (the uart_owner request-queue worker
# task, one per owner instance) were deleted 2026-09-06 (uart collapse):
# uart_owner_transfer() -- the only caller that ever reached that task -- had
# zero real callers left in KilnFW, so the whole request-queue/worker-task
# pair was dead code. Only each owner's event task (*_owner_evt/*_evt_task)
# remains and stays required above.

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
    Write-Host "  (DRAM_PSRAM_STATUS.md section 7 / stack_margin.h's tracked set) but have" -ForegroundColor Red
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
    # Scan only git-tracked files. An untracked scratch/work directory left
    # by a concurrent check (test_host_fakes.ps1's mutant sources,
    # compile_headers.ps1's dummy backends) is never a real source hit, and
    # walking the working tree made this check fail spuriously in parallel
    # runs while passing under -Only: a file enumerated here can vanish
    # before Get-CodeOnlyLines reads it, which is fatal under
    # $ErrorActionPreference = "Stop" (9 of 10 runs against a churning
    # scratch dir). check_no_duplicate_crc.ps1 already solved the identical
    # problem this way; same fix, same reason.
    Push-Location $hwAbstractionRootResolved
    try {
        $hwTrackedRel = @(git ls-files -- "*.c" "*.h")
    } finally {
        Pop-Location
    }
    $hwAbstractionFiles = @($hwTrackedRel |
        ForEach-Object { Join-Path $hwAbstractionRootResolved ($_ -replace '/', '\') } |
        Where-Object { Test-Path -LiteralPath $_ } |
        ForEach-Object { Get-Item -LiteralPath $_ })

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

# --- Create-vs-register cross-check ---------------------------------------
#
# 2026-09-08 fix (docs/audits/2026-09-08-stack-margin-audit.md): the
# required-name list above is hand-maintained, so it can only ever catch a
# task LOSING a call site it once had -- it cannot see a task that never had
# one, because nobody added its name to the list either. That is exactly how
# thermo_bridge_task/touch_bridge_task/ui_test_bridge_task/io_bridge_task/
# uart_log_bridge_task/safety_bridge_task went unmeasured: six long-lived
# (`while (true)`, never self-deleting) tasks, created every boot, invisible
# to this script because they were never in $requiredNames either. This
# section is the mechanical guard against that class reopening: it finds
# EVERY xTaskCreate*() call site under the same scanned files, extracts the
# FreeRTOS task-name string literal (the 2nd argument -- a debug-visible
# symbol that survives file moves/splits, unlike a path), and requires each
# one to either already be a registered stack_margin name OR appear in the
# explicit, commented exemption list below. An unrecognised name -- not
# registered, not exempted -- fails loud, by name: that is the "a required
# task losing its registration" AND "a task nobody ever wired in" cases both
# covered by one mechanism, keyed on the created task's own name rather than
# on which file happened to create it.
$createPattern2 = 'xTaskCreate\w*\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"([^"]+)"'
$createdTasks = @()
foreach ($f in $sourceFiles) {
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    $joined = [string]::Join("`n", $codeLines)
    foreach ($m in [regex]::Matches($joined, $createPattern2)) {
        $createdTasks += [PSCustomObject]@{
            FreeRtosName = $m.Groups[2].Value
            TaskFunction = $m.Groups[1].Value
            File         = $f.Name
        }
    }
}

# Exemptions: tasks legitimately not tracked by stack_margin_register(),
# each with a stated reason. Two categories --
#   (a) short-lived / self-deleting helper tasks (the required-name list's
#       own design already excludes these; this is the same exclusion,
#       applied mechanically instead of by omission)
#   (b) long-lived tasks registered for real, but under a documented,
#       DIFFERENT literal name than the FreeRTOS task-name string (so an
#       exact-string match against $registeredSet would false-positive) --
#       each of these has its own explanatory comment elsewhere in this
#       codebase (i2c_owner.c, esp_spi_owner.c, ota_http_recovery.c).
$exemptCreatedNames = @{
    # (a) short-lived / self-deleting -- exits on its own, no ongoing stack
    # margin to track past the moment it was created.
    "wifi_mode_ui"      = "ui_page_network.c mode_worker_task: one-shot Wi-Fi mode change, self-deletes"
    "wifi_ap_id_ui"     = "ui_page_network.c ap_identity_worker_task: one-shot AP identity fetch, self-deletes"
    "wifi_connect_ui"   = "ui_page_network_manage.c connect_worker_task: one-shot connect attempt, self-deletes"
    "wifi_forget_ui"    = "ui_page_network_manage.c forget_worker_task: one-shot forget-network call, self-deletes"
    "wifi_scan_ui"      = "ui_page_network_manage.c scan_worker_task: one-shot Wi-Fi scan, self-deletes"
    "ota_confirm"       = "main_network_http.c main_ota_rollback_confirm_task: one-shot OTA confirm timer, self-deletes"
    "cfg_autofmt"       = "cfg_fs_mount.c cfg_fs_auto_format_task: one-shot cfg filesystem format, self-deletes"
    "dns_hijack"        = "wifi_prov_link.c dns_hijack_task: provisioning-only captive-portal DNS, self-deletes"
    "factory_reset_reboot" = "factory_reset.c reboot_task: one-shot reboot-after-delay, never returns to measure"
    "ota_rollback_reboot"  = "ota_http_esp.c ota_rollback_reboot_task: one-shot reboot-after-delay, never returns to measure"
    "ota_pico_rollback"    = "ota_http_pico.c ota_pico_rollback_task: one-shot reboot-after-delay, never returns to measure"
    "recovery_exit_reboot" = "ota_http_recovery.c ota_recovery_exit_reboot_task: registered under the shortened name 'recovery_exit' (see that file's comment), not this FreeRTOS task-name string"
    "sw_reset_reboot"      = "sw_reset_http.c sw_reset_reboot_task: one-shot reboot-after-delay (send ANNOUNCE_REBOOT, then hal_wdt_reboot()), never returns to measure -- same shape as factory_reset_reboot/ota_rollback_reboot above"
    "ota_pico_relay"    = "ota_pico_relay.c relay_task_fn: one-shot relay session, self-deletes"
    "wifi_prov_owner"   = "wifi_prov.c owner_task: provisioning-only command owner, torn down with the provisioning session"
    "info_boot_push"    = "uart_bridge_info.c info_boot_push_task: one-shot boot version push, self-deletes"
    # (b) long-lived, registered for real, under a different literal name.
    "i2c_owner_task"    = "i2c_owner.c owner_task: registered per-caller under 'i2c_owner_sx1509'/'i2c_owner_ns2009' (SX1509.c/NS2009.c), not this FreeRTOS task-name string -- see i2c_owner.c's own comment"
    "spi_owner_task"    = "esp_spi_owner.c spi_owner_task: registered as 'spi_owner' (its own stack_margin_register() call site), not this FreeRTOS task-name string"
    # Known pre-existing gaps, out of scope for this pass (not part of the
    # 6-task fix above; flagged rather than silently exempted so a future
    # pass has a named target instead of rediscovering these from scratch):
    "monitor_task"      = "monitor_task.c monitor_task_entry: long-lived heartbeat task, NOT YET registered for stack-margin reporting -- pre-existing gap, out of scope for docs/audits/2026-09-08-stack-margin-audit.md, flagged for a follow-up pass"
}

$unrecognized = @()
foreach ($t in $createdTasks) {
    $freeRtosName = $t.FreeRtosName
    if ($registeredSet.Contains($freeRtosName)) { continue }
    if ($exemptCreatedNames.ContainsKey($freeRtosName)) { continue }
    $unrecognized += "$freeRtosName (task function '$($t.TaskFunction)' in $($t.File))"
}

if ($unrecognized.Count -gt 0) {
    Write-Host "STACK MARGIN CREATE-VS-REGISTER CHECK FAILED:" -ForegroundColor Red
    Write-Host "  The following xTaskCreate*() call site(s) create a task that is neither" -ForegroundColor Red
    Write-Host "  a registered stack_margin name nor a documented exemption:" -ForegroundColor Red
    foreach ($u in $unrecognized) {
        Write-Host "    $u" -ForegroundColor Red
    }
    throw "A task is created (xTaskCreate*()) without a stack_margin_register() call site and without being added to this script's `$exemptCreatedNames allowlist with a stated reason -- its high-water mark is unreachable by any tooling. Either add a stack_margin_register() call site (and, if long-lived, add it to `$requiredNames above), or add it to `$exemptCreatedNames with a one-line justification (short-lived/self-deleting, or registered under a documented different name)."
}

Write-Host "Stack margin create-vs-register check passed: $($createdTasks.Count) xTaskCreate*() call site(s) all accounted for (registered or exempted)." -ForegroundColor Green

exit 0
