# Builds and runs the host-side unit tests for pid.c / thermal_guard.c /
# heater_output.c / thermo_combine.c / ota_auth.c / ota_interlock.c /
# pid_autotune.c / profile_feasibility.c (+ the sim_plant.c closed-loop
# check) with MSVC, entirely off-target -- no ESP-IDF, no hardware.
# TODO.md 5A.1, 6A.8, 10.8.
#
# profile_feasibility.c is pure math but its headers are real firmware
# headers that name ESP-IDF types, so App/test/stubs/ supplies type-only
# stand-ins (esp_err.h, driver/spi_master.h, esp_spi_owner.h, freertos/*)
# reached via /I. Being on the include path only, they can never shadow a
# real header in an on-target build.
#
# Usage: powershell -File App\test\build_host_tests.ps1
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars -- update this path if MSVC Build Tools moved/were reinstalled."
}

$testDir = $PSScriptRoot
$driversDir = Join-Path $testDir "..\drivers"
$outDir = Join-Path $testDir "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir "kilnctl_host_tests.exe"

$sources = @(
    (Join-Path $testDir "test_main.c"),
    (Join-Path $testDir "test_pid.c"),
    (Join-Path $testDir "test_thermal_guard.c"),
    (Join-Path $testDir "test_heater_output.c"),
    (Join-Path $testDir "test_closed_loop.c"),
    (Join-Path $testDir "test_pid_autotune.c"),
    (Join-Path $testDir "test_sim_kiln.c"),
    (Join-Path $testDir "test_ota_auth.c"),
    (Join-Path $testDir "test_ota_interlock.c"),
    (Join-Path $testDir "test_heat_interlock.c"),
    (Join-Path $testDir "test_thermo_combine.c"),
    (Join-Path $testDir "test_profile_feasibility.c"),
    (Join-Path $testDir "test_profile_plan_curve.c"),
    (Join-Path $testDir "test_wifi_prov.c"),
    (Join-Path $testDir "test_backup_import.c"),
    (Join-Path $testDir "test_ota_record.c"),
    (Join-Path $testDir "test_uart_log_bridge.c"),
    (Join-Path $testDir "test_safety_watchdog.c"),
    (Join-Path $testDir "test_safety_link.c"),
    (Join-Path $testDir "test_dashboard_safety_ready.c"),
    (Join-Path $testDir "test_readiness_commissioning.c"),
    (Join-Path $testDir "test_kiln_cfg_store.c"),
    (Join-Path $testDir "test_safety_cfg_store.c"),
    (Join-Path $testDir "test_rules_eval.c"),
    (Join-Path $testDir "sim_plant.c"),
    (Join-Path $driversDir "pid.c"),
    (Join-Path $driversDir "thermal_guard.c"),
    (Join-Path $driversDir "heater_output.c"),
    (Join-Path $driversDir "pid_autotune.c"),
    (Join-Path $driversDir "ota_auth.c"),
    (Join-Path $driversDir "ota_interlock.c"),
    (Join-Path $driversDir "ota_record.c"),
    (Join-Path $driversDir "heat_interlock.c"),
    (Join-Path $driversDir "thermo_combine.c"),
    (Join-Path $driversDir "profile_feasibility.c"),
    (Join-Path $driversDir "rules_eval.c")
)

$sourceArgs = ($sources | ForEach-Object { '"' + $_ + '"' }) -join " "
# Type-only stand-ins for the ESP-IDF headers the real firmware headers name,
# plus CommonFW's include root for kilnlink/kilnlink_version.h. Include-path
# only, so an on-target build is unaffected.
$stubDir = Join-Path $testDir "stubs"
$commonInc = Join-Path $testDir "..\..\..\CommonFW\include"
$cmd = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /I`"$stubDir`" /I`"$commonInc`" /Fo:`"$outDir\\`" /Fe:`"$exe`" $sourceArgs"

cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "build failed"
}

& $exe
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

# ---- test_zones_http.c: its own SEPARATE executable ------------------------
# See test_zones_http.c's header comment for why: it #includes zones_http.c
# directly (to reach parse_zone_fields(), a `static` function with no other
# seam), which DEFINES the real zones_config_get_*()/set_*() functions --
# and test_backup_import.c above already defines its OWN fake bodies for
# those same names to stub backup_import_apply()'s dependency on
# zones_http.h. Linking both into one executable would be a multiple-
# definition error, so this one is built and run as a second, independent
# binary instead of being added to $sources above.
$exe2 = Join-Path $outDir "kilnctl_host_tests_zones.exe"
$cmd2 = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /std:c11 /I`"$stubDir`" /I`"$commonInc`" " +
        "/Fo:`"$outDir\\zones_`" /Fe:`"$exe2`" `"$(Join-Path $testDir 'test_zones_http.c')`""

cmd.exe /c $cmd2
if ($LASTEXITCODE -ne 0) {
    throw "zones_http build failed"
}

& $exe2
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

# ---- test_safety_cfg_http.c: its own THIRD, separate executable -----------
# Same reason as test_zones_http.c above: it #includes safety_cfg_http.c
# directly to reach its static parse_set_param_body()/build_commissioning_
# json()/apply_pairs() helpers, and defines its own fake bodies for
# safety_cfg_store_get_by_index() and friends -- the main executable already
# links the REAL ones via test_safety_cfg_store.c's #include of safety_cfg_
# store.c, so linking both into one binary would multiply-define every
# safety_cfg_store_* symbol.
$exe3 = Join-Path $outDir "kilnctl_host_tests_safety_cfg_http.exe"
$cmd3 = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /std:c11 /I`"$stubDir`" /I`"$commonInc`" " +
        "/Fo:`"$outDir\\safety_cfg_http_`" /Fe:`"$exe3`" `"$(Join-Path $testDir 'test_safety_cfg_http.c')`""

cmd.exe /c $cmd3
if ($LASTEXITCODE -ne 0) {
    throw "safety_cfg_http build failed"
}

& $exe3
exit $LASTEXITCODE
