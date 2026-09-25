# check_stack_task_table_consistency.ps1 -- keeps three separately-maintained
# stack-margin tables pointed at the same set of tasks:
#
#   1. TASKS in firmware/KilnFW/App/test/check_all_task_stack_budgets.py
#      (`dict(name="...", ...)` rows -- one per task this script measures a
#      deepest-call-graph depth for).
#   2. CEILING_BYTES in the same file (`"<name>": <bytes>,` -- the regression
#      ceiling each TASKS row is graded against; check_all_task_stack_budgets.py
#      itself already refuses to grade a TASKS row with no CEILING_BYTES entry,
#      but says nothing about a CEILING_BYTES entry left behind for a TASKS row
#      that no longer exists, which is exactly the "reset one side of a pair"
#      class CLAUDE.md calls out -- a stale ceiling nobody is grading against
#      anymore looks like coverage but measures nothing).
#   3. $requiredNames in tools/check_stack_margin_registration.ps1 -- the set
#      of stack_margin_register() call sites that script requires to exist at
#      all (registration only, no depth ceiling).
#
# Nothing before this check cross-referenced these three by name. A task can
# be removed from TASKS/CEILING_BYTES (or added to one and not the other)
# without check_all_task_stack_budgets.py's own row-count symmetry catching a
# stale CEILING_BYTES entry, and a task can be dropped from -- or never added
# to -- $requiredNames without either python-side table noticing, since that
# list lives in a different file entirely. This is the same class of gap
# check_stack_margin_registration.ps1's own "create-vs-register cross-check"
# section closes for xTaskCreate*() vs stack_margin_register() -- this check
# closes the analogous gap one level up, between the two files that grade a
# registered task's stack depth.
#
# Found by writing this check (2026-09-24): "backlight_pwm" had a real
# stack_margin_register() call site (drivers/hw/backlight_pwm.c) and a full
# TASKS/CEILING_BYTES row in check_all_task_stack_budgets.py, but was missing
# from check_stack_margin_registration.ps1's $requiredNames -- a task whose
# depth was being measured and ceilinged every run, silently invisible to the
# registration-required list. Fixed in the same commit that added this check.
#
# NOT EVERY $requiredNames entry is expected to appear in TASKS: four tasks
# (httpd_worker, system_uart_bridge, uart_log_bridge, profile_executor)
# have their OWN dedicated check_*_task_stack_budget.py/.ps1 pair instead
# (check_all_task_stack_budgets.py's own module docstring explains why: those
# four predate this table and needed bespoke live-measurement cross-checks of
# their own). A further small set of $requiredNames entries have no per-task
# ceiling anywhere yet -- pre-existing gaps, out of scope for this pass, named
# explicitly below rather than silently tolerated (same style as
# check_stack_margin_registration.ps1's own $exemptCreatedNames "monitor_task"
# entry). Both exemption lists are hand-maintained and must be updated in the
# same commit that closes (or opens) one of these gaps -- exactly like every
# other hand-maintained list this script cross-checks.
#
# Usage: powershell -File tools\check_stack_task_table_consistency.ps1
#        (-BudgetsPyPath / -RegistrationPs1Path to smoke-test against a copy)
param(
    [string]$BudgetsPyPath,
    [string]$RegistrationPs1Path
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
if ($BudgetsPyPath) {
    $budgetsPyPath = (Resolve-Path $BudgetsPyPath).Path
} else {
    $budgetsPyPath = Join-Path $root "..\firmware\KilnFW\App\test\check_all_task_stack_budgets.py"
    $budgetsPyPath = (Resolve-Path $budgetsPyPath).Path
}
if ($RegistrationPs1Path) {
    $registrationPs1Path = (Resolve-Path $RegistrationPs1Path).Path
} else {
    $registrationPs1Path = Join-Path $root "check_stack_margin_registration.ps1"
    $registrationPs1Path = (Resolve-Path $registrationPs1Path).Path
}

$pyText = Get-Content -Path $budgetsPyPath -Raw
$ps1Text = Get-Content -Path $registrationPs1Path -Raw

# --- 1. TASKS names: every `dict(name="...", ` row. This literal shape
# ("dict(name=") appears nowhere else in the file (comments reference rows as
# TASKS["name"] instead), so a plain scan of the whole file is safe -- no need
# to first bound the TASKS = [ ... ] block. ---------------------------------
$taskNamePattern = 'dict\(name="([^"]+)"'
$taskNames = New-Object System.Collections.Generic.List[string]
foreach ($m in [regex]::Matches($pyText, $taskNamePattern)) {
    $taskNames.Add($m.Groups[1].Value)
}
if ($taskNames.Count -lt 10) {
    throw "check_stack_task_table_consistency.ps1: only found $($taskNames.Count) TASKS row(s) in $budgetsPyPath (expected 25+) -- the TASKS table shape has probably changed (dict(name=...) renamed/restructured) and this scan has gone blind. Update its pattern before trusting its result."
}
$taskNameSet = [System.Collections.Generic.HashSet[string]]::new([string[]]$taskNames)
if ($taskNameSet.Count -ne $taskNames.Count) {
    $dupes = @($taskNames | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
    throw "check_stack_task_table_consistency.ps1: duplicate TASKS row name(s) in $budgetsPyPath : $($dupes -join ', ')"
}

# --- 2. CEILING_BYTES keys: bounded to the CEILING_BYTES = { ... } block so a
# `"name": number,`-shaped line elsewhere in the file (there is none today,
# but this is the same defensive bounding check_all_task_stack_budgets.py's
# own --dump-ceilings uses) can never be mistaken for a ceiling entry. -------
$ceilingBlockMatch = [regex]::Match($pyText, 'CEILING_BYTES = \{(.*?)\r?\n\}\r?\n', 'Singleline')
if (-not $ceilingBlockMatch.Success) {
    throw "check_stack_task_table_consistency.ps1: no 'CEILING_BYTES = { ... }' block found in $budgetsPyPath -- has it been renamed or restructured? Update this script's pattern."
}
$ceilingBlock = $ceilingBlockMatch.Groups[1].Value
$ceilingNamePattern = '(?m)^\s*"([^"]+)":\s*\d+,'
$ceilingNames = New-Object System.Collections.Generic.List[string]
foreach ($m in [regex]::Matches($ceilingBlock, $ceilingNamePattern)) {
    $ceilingNames.Add($m.Groups[1].Value)
}
if ($ceilingNames.Count -lt 10) {
    throw "check_stack_task_table_consistency.ps1: only found $($ceilingNames.Count) CEILING_BYTES entr(y/ies) in $budgetsPyPath (expected 25+) -- the CEILING_BYTES table shape has probably changed and this scan has gone blind. Update its pattern before trusting its result."
}
$ceilingNameSet = [System.Collections.Generic.HashSet[string]]::new([string[]]$ceilingNames)
if ($ceilingNameSet.Count -ne $ceilingNames.Count) {
    $dupes = @($ceilingNames | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
    throw "check_stack_task_table_consistency.ps1: duplicate CEILING_BYTES key(s) in $budgetsPyPath : $($dupes -join ', ')"
}

# --- 3. $requiredNames from check_stack_margin_registration.ps1: bounded to
# the `$requiredNames = @( ... )` array literal, comment lines stripped first
# so a name-shaped string inside an explanatory comment (this file has
# several) is never mistaken for a required name. ---------------------------
$requiredBlockMatch = [regex]::Match($ps1Text, '\$requiredNames = @\((.*?)\r?\n\)\r?\n', 'Singleline')
if (-not $requiredBlockMatch.Success) {
    throw "check_stack_task_table_consistency.ps1: no '`$requiredNames = @( ... )' block found in $registrationPs1Path -- has it been renamed or restructured? Update this script's pattern."
}
$requiredBlock = $requiredBlockMatch.Groups[1].Value
$requiredCodeLines = @()
foreach ($line in ($requiredBlock -split "`r?`n")) {
    $trimmed = $line.TrimStart()
    if ($trimmed.StartsWith("#")) { continue }
    # Strip a trailing inline `# ...` comment (every real entry here is a
    # bare quoted string or a comma-separated list of them, never containing
    # a literal '#').
    $hashIdx = $line.IndexOf("#")
    if ($hashIdx -ge 0) { $line = $line.Substring(0, $hashIdx) }
    $requiredCodeLines += $line
}
$requiredCode = [string]::Join("`n", $requiredCodeLines)
$requiredNames = New-Object System.Collections.Generic.List[string]
foreach ($m in [regex]::Matches($requiredCode, '"([^"]+)"')) {
    $requiredNames.Add($m.Groups[1].Value)
}
if ($requiredNames.Count -lt 20) {
    throw "check_stack_task_table_consistency.ps1: only found $($requiredNames.Count) `$requiredNames entr(y/ies) in $registrationPs1Path (expected 30+) -- the array shape has probably changed and this scan has gone blind. Update its pattern before trusting its result."
}
$requiredNameSet = [System.Collections.Generic.HashSet[string]]::new([string[]]$requiredNames)

Write-Host "check_stack_task_table_consistency: $($taskNameSet.Count) TASKS row(s), $($ceilingNameSet.Count) CEILING_BYTES entr(y/ies), $($requiredNameSet.Count) `$requiredNames entr(y/ies)."

$failures = @()

# --- Check A: TASKS and CEILING_BYTES must name exactly the same set. This
# duplicates part of what check_all_task_stack_budgets.py itself already
# enforces at run time (a TASKS row with no CEILING_BYTES entry fails that
# check directly) -- but ONLY that direction, and only when an ELF is
# available to run it against at all. This check is source-only (no ELF, no
# objdump needed) and additionally catches the direction that script does
# not: a CEILING_BYTES entry whose TASKS row was removed, silently measuring
# nothing and grading nothing, forever. -------------------------------------
foreach ($name in $taskNameSet) {
    if (-not $ceilingNameSet.Contains($name)) {
        $failures += "TASKS row '$name' ($budgetsPyPath) has no matching CEILING_BYTES entry."
    }
}
foreach ($name in $ceilingNameSet) {
    if (-not $taskNameSet.Contains($name)) {
        $failures += "CEILING_BYTES entry '$name' ($budgetsPyPath) has no matching TASKS row -- a stale ceiling left behind after a task was removed grades nothing."
    }
}

# --- Check B: every TASKS row must be a required stack_margin_register()
# name. A task measured for depth by check_all_task_stack_budgets.py but
# absent from $requiredNames means check_stack_margin_registration.ps1 would
# never notice that task LOSING its stack_margin_register() call site --
# exactly the failure mode this pass's own "backlight_pwm" finding was. ------
foreach ($name in $taskNameSet) {
    if (-not $requiredNameSet.Contains($name)) {
        $failures += "TASKS row '$name' ($budgetsPyPath) is not in `$requiredNames ($registrationPs1Path) -- its stack_margin_register() call site could be lost with nothing catching it."
    }
}

# --- Check C: every $requiredNames entry must be accounted for by either a
# TASKS row, or one of the two hand-maintained exemption lists below. An
# entry in neither means a task this repo requires to be registered has no
# depth ceiling anywhere -- a live registration with no regression gate on
# its measured depth, silent until it overflows. ----------------------------
#
# (a) four tasks with their OWN dedicated check_*_task_stack_budget.py/.ps1
# pair instead of a TASKS/CEILING_BYTES row (see check_all_task_stack_budgets
# .py's own module docstring, "STRUCTURE" section, for why these four predate
# and sit outside this table).
$dedicatedCheckerNames = @(
    "httpd_worker",      # check_httpd_task_stack_budget.py/.ps1
    "system_uart_bridge", # check_system_uart_bridge_stack_budget.py/.ps1
    "uart_log_bridge",   # check_uart_log_bridge_stack_budget.py/.ps1
    "profile_executor"   # check_executor_task_stack_budget.py/.ps1
)
# (b) known pre-existing gaps: registered (required) but with NO per-task
# depth ceiling anywhere today, TASKS row or dedicated checker. Flagged here
# by name, same as check_stack_margin_registration.ps1's own $exemptCreatedNames
# "monitor_task" entry, rather than silently tolerated -- a future pass has a
# named target instead of rediscovering these from scratch.
$knownUnceilingedNames = @(
    "spi_owner",         # esp_spi_owner.c spi_owner_task: registered, no TASKS row or dedicated checker yet
    "ui_test_uart_bridge" # uart_bridge_ui_test.c: registered, no TASKS row or dedicated checker yet
)
$dedicatedCheckerSet = [System.Collections.Generic.HashSet[string]]::new([string[]]$dedicatedCheckerNames)
$knownUnceilingedSet = [System.Collections.Generic.HashSet[string]]::new([string[]]$knownUnceilingedNames)

foreach ($name in $requiredNameSet) {
    if ($taskNameSet.Contains($name)) { continue }
    if ($dedicatedCheckerSet.Contains($name)) { continue }
    if ($knownUnceilingedSet.Contains($name)) { continue }
    $failures += "`$requiredNames entry '$name' ($registrationPs1Path) is not a TASKS row, a dedicated-checker name, or a documented known-unceilinged exemption -- either add it to `$dedicatedCheckerNames or `$knownUnceilingedNames in this script with a reason, or give it a TASKS/CEILING_BYTES row."
}

# Names claimed as dedicated-checker or known-gap exemptions that turned out
# to not even be required at all would silently rot the exemption lists
# themselves -- catch that too, same as any other hand-maintained list here.
foreach ($name in $dedicatedCheckerSet) {
    if (-not $requiredNameSet.Contains($name)) {
        $failures += "`$dedicatedCheckerNames entry '$name' (this script) is not in `$requiredNames ($registrationPs1Path) -- stale exemption, remove it or restore the required-name entry."
    }
}
foreach ($name in $knownUnceilingedSet) {
    if (-not $requiredNameSet.Contains($name)) {
        $failures += "`$knownUnceilingedNames entry '$name' (this script) is not in `$requiredNames ($registrationPs1Path) -- stale exemption, remove it or restore the required-name entry."
    }
}

if ($failures.Count -gt 0) {
    Write-Host "STACK TASK TABLE CONSISTENCY CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "$($failures.Count) mismatch(es) between TASKS/CEILING_BYTES ($budgetsPyPath) and `$requiredNames ($registrationPs1Path) -- see messages above."
}

Write-Host "Stack task table consistency check passed: TASKS, CEILING_BYTES, and `$requiredNames agree ($($taskNameSet.Count) TASKS rows, $($dedicatedCheckerSet.Count) dedicated-checker exemptions, $($knownUnceilingedSet.Count) known-unceilinged exemptions)." -ForegroundColor Green

exit 0
