# lib_saftyfw_slot_images.ps1 -- shared helper, NOT a check itself (deliberately
# named so tools/run_all_checks.ps1's check_*.ps1 glob does not pick it up,
# the same way tools/build_lock.ps1 is not a check).
#
# Dot-source this file, then call Ensure-SaftyfwSlotImages against a worktree
# that has firmware\SaftyFW checked out. It builds
# firmware\SaftyFW\build\SaftyFW_slotA.bin / SaftyFW_slotB.bin if missing or
# stale relative to the worktree's own SaftyFW source, and FAILs loudly
# (never SKIPs) if the arm-none-eabi toolchain is unavailable -- the caller
# is expected to define a `Fail([string]$msg)` function in scope (both
# check_00_kilnfw_target_build.ps1 and check_01_kilnfw_pushed_build.ps1 do)
# since dot-sourcing this file resolves `Fail` dynamically against the
# caller's own scope at call time.
#
# ORIGIN (2026-09-20). firmware\KilnFW\App\drivers\CMakeLists.txt's
# EMBED_FILES step FATAL_ERRORs at configure time if these two .bin files are
# missing -- introduced by the Pico embedded auto-update chain (docs/PICO_AUTO_UPDATE_PLAN.md). This
# logic first lived only in check_00_kilnfw_target_build.ps1, which builds a
# mirrored copy of the local working tree and so happened to pick up a
# SaftyFW build directory from earlier manual work; check_01_kilnfw_pushed_
# build.ps1 builds a PRISTINE `git worktree add` of origin/main with no such
# history, so bare origin/main did not build under it and check_01 went red.
# Factored out here so both checks build the slot images identically instead
# of drifting, and so a from-scratch worktree (SaftyFW checkout, no history)
# is exercised the same way for either check that needs it.
#
# This mirrors firmware\SaftyFW\test\check_00_saftyfw_target_build.ps1's own
# cmake -G Ninja / ninja invocation (same toolchain, same PICO_SDK_PATH
# default) rather than reinventing it. It does not itself objcopy the ELFs to
# .bin -- firmware\SaftyFW\CMakeLists.txt's own POST_BUILD step
# (saftyfw_add_slot_executable(), ~line 558) already runs
# ${CMAKE_OBJCOPY} -O binary on every SaftyFW_slotA/SaftyFW_slotB build and
# writes the .bin next to the .elf; this helper only builds the ninja targets
# and verifies both the .elf and .bin exist afterward.
#
# SKIP is not available to a caller of this helper: KilnFW's own build cannot
# proceed at all without these two files once they are missing, so a missing
# arm-none-eabi toolchain is a hard FAIL here, not a soft SKIP, regardless of
# which check called it.

function Get-SaftyfwSlotSourceTime([string[]] $roots) {
    $newest = $null
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $candidate = Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -notmatch '\\build\\' -and $_.FullName -notmatch '\\components\\lvgl\\' } |
            Measure-Object -Property LastWriteTime -Maximum
        if ($candidate.Maximum -and (-not $newest -or $candidate.Maximum -gt $newest)) {
            $newest = $candidate.Maximum
        }
    }
    return $newest
}

function Ensure-SaftyfwSlotImages([string]$KilnfwWorktreePath, [string]$CcacheExe = "") {
    $SaftyfwWorktreeDir = Join-Path $KilnfwWorktreePath "firmware\SaftyFW"
    $SaftyfwBuildDir = Join-Path $SaftyfwWorktreeDir "build"
    $slotABin = Join-Path $SaftyfwBuildDir "SaftyFW_slotA.bin"
    $slotBBin = Join-Path $SaftyfwBuildDir "SaftyFW_slotB.bin"
    $slotAElf = Join-Path $SaftyfwBuildDir "SaftyFW_slotA.elf"
    $slotBElf = Join-Path $SaftyfwBuildDir "SaftyFW_slotB.elf"

    $saftyfwNewest = Get-SaftyfwSlotSourceTime @($SaftyfwWorktreeDir)
    $slotBinsStale = $true
    if ((Test-Path -LiteralPath $slotABin) -and (Test-Path -LiteralPath $slotBBin)) {
        $slotATime = (Get-Item -LiteralPath $slotABin).LastWriteTime
        $slotBTime = (Get-Item -LiteralPath $slotBBin).LastWriteTime
        if ((-not $saftyfwNewest) -or (($slotATime -gt $saftyfwNewest) -and ($slotBTime -gt $saftyfwNewest))) {
            $slotBinsStale = $false
        }
    }

    if ($slotBinsStale) {
        $haveArmToolchain = [bool](Get-Command "arm-none-eabi-gcc" -ErrorAction SilentlyContinue)
        if (-not $haveArmToolchain -and -not $env:PICO_TOOLCHAIN_PATH) {
            Fail "firmware\SaftyFW\build\SaftyFW_slotA.bin/SaftyFW_slotB.bin are missing or stale in $SaftyfwBuildDir and arm-none-eabi-gcc is not on PATH (PICO_TOOLCHAIN_PATH not set either) -- App/drivers/CMakeLists.txt's EMBED_FILES guard requires these two files, and this check cannot build them without the arm-none-eabi toolchain."
        }
        if (-not $env:PICO_SDK_PATH) {
            $env:PICO_SDK_PATH = "C:\pico-tools\pico-sdk"
            Write-Host "PICO_SDK_PATH not set -- defaulting to $env:PICO_SDK_PATH (see firmware\SaftyFW\CMakeLists.txt header comment)."
        }
        if (-not (Test-Path -LiteralPath $SaftyfwBuildDir)) {
            New-Item -ItemType Directory -Path $SaftyfwBuildDir | Out-Null
        }
        Push-Location $SaftyfwWorktreeDir
        try {
            Write-Host "Configuring SaftyFW (cmake -G Ninja -B build .) in $SaftyfwWorktreeDir for the embedded slot images ..."
            # -CcacheExe (check_00_kilnfw_target_build.ps1 only, after
            # lib_kilnfw_ccache.ps1's Enable-KilnfwCcache has set and asserted
            # the environment): compile through that ccache. Otherwise the
            # launcher is set explicitly EMPTY, so a build directory is never
            # left launching ccache under a caller that did not configure it.
            $launcher = ""
            if ($CcacheExe) { $launcher = $CcacheExe -replace '\\', '/' }
            cmake -G Ninja -B build . "-DCMAKE_C_COMPILER_LAUNCHER=$launcher" "-DCMAKE_CXX_COMPILER_LAUNCHER=$launcher" 2>&1 | Write-Host
            if ($LASTEXITCODE -ne 0) {
                Fail "cmake configure of $SaftyfwWorktreeDir failed (exit $LASTEXITCODE) while building the SaftyFW slot images App/drivers/CMakeLists.txt's EMBED_FILES guard requires."
            }
        } finally {
            if ((Get-Location).Path -eq $SaftyfwWorktreeDir) { Pop-Location }
        }
        Push-Location $SaftyfwBuildDir
        try {
            Write-Host "Building SaftyFW_slotA/SaftyFW_slotB (ninja) ..."
            if (-not (Get-Command Invoke-KilnGatedCmd -ErrorAction SilentlyContinue)) { . (Join-Path $PSScriptRoot "..\..\..\..\tools\build_gate.ps1") }
            Invoke-KilnGatedCmd -Command "ninja SaftyFW_slotA SaftyFW_slotB 2>&1" -Label "saftyfw_slot_images" | Write-Host
            if ($LASTEXITCODE -ne 0) {
                Fail "ninja build of SaftyFW_slotA/SaftyFW_slotB failed (exit $LASTEXITCODE) -- see output above. KilnFW's own build cannot proceed without these two images (App/drivers/CMakeLists.txt's EMBED_FILES guard)."
            }
        } finally {
            Pop-Location
        }
        foreach ($pair in @(@{Elf=$slotAElf; Bin=$slotABin}, @{Elf=$slotBElf; Bin=$slotBBin})) {
            if (-not (Test-Path -LiteralPath $pair.Elf)) {
                Fail "ninja reported success but $($pair.Elf) does not exist -- refusing to report PASS without the real SaftyFW slot artifact."
            }
            if (-not (Test-Path -LiteralPath $pair.Bin)) {
                Fail "ninja reported success but $($pair.Bin) does not exist -- firmware\SaftyFW\CMakeLists.txt's saftyfw_add_slot_executable() POST_BUILD objcopy step should have produced it alongside $($pair.Elf)."
            }
        }
        Write-Host "SaftyFW slot images built and staged: $slotABin, $slotBBin"
    } else {
        Write-Host "SaftyFW slot images already current in $SaftyfwBuildDir (newer than all mirrored SaftyFW source) -- not rebuilding."
    }
}
