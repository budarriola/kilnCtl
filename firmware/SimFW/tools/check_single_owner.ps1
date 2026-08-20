# check_single_owner.ps1 -- enforces docs/PLAN.md section 4's central rule:
#
#   "every hardware interface has exactly one owner task; nobody else touches
#   that peripheral."
#
# Same doctrine, same enforcement style as firmware/SaftyFW/tools/
# check_isolation.ps1: grep the actual #include lines (comments stripped
# first, so this file's own header can name the headers it's checking without
# tripping itself) and fail loudly, naming the offending file, if a
# single-owner peripheral header shows up anywhere outside its declared
# owner(s).
#
# The allowlist below was built by reading the *real* current includes under
# firmware/SimFW/src (2026-08-20), not from an idealized task map -- e.g.
# tusb.h is owned by usb_owner.c AND usb_descriptors.c (the descriptor table
# is a second file by necessity, both compiled into the one owner task), and
# other tasks that need USB (cmd_task.c, telemetry.c) go through
# usb_owner.h's public API (usb_owner_send_reply()/usb_owner_send_broadcast())
# rather than including tusb.h themselves -- which is exactly the doctrine
# working as intended, not a hole in it. Likewise hardware/pio.h is only ever
# included by the MAX31856 PIO driver (drivers/max31856_pio_engine.{c,h});
# the two SPI-emulation owner tasks (spi_emu_a.c/spi_emu_b.c) reach PIO only
# through that driver's own header, never hardware/pio.h directly.
#
# hardware/gpio.h is deliberately NOT one of the checked headers here: unlike
# I2C/PIO/PWM+DMA/USB, GPIO is used by several genuinely different owners for
# genuinely different pins (DRDY/~FAULT lines via the PIO driver, discrete
# I/O via i2c_owner.c's non-I2C GPIO helpers, CT sine synthesis via
# ct_wave_pwm.c) with no single peripheral being shared -- a blanket
# "hardware/gpio.h has one owner" rule would be false by construction, not a
# real violation of PLAN.md section 4's rule (which is about one INTERFACE,
# not the GPIO block in the abstract).
#
# Usage: powershell -File firmware\SimFW\tools\check_single_owner.ps1
$ErrorActionPreference = "Stop"

$simfwRoot = Split-Path -Parent $PSScriptRoot
$srcRoot = Join-Path $simfwRoot "src"

if (-not (Test-Path $srcRoot)) {
    throw "check_single_owner.ps1: expected source tree not found at $srcRoot"
}

# Same comment-stripping helper as check_isolation.ps1, so a doc comment
# naming a header (like this file's own header above) never counts as code.
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

# Header -> regex matching its #include line -> allowed owner file(s),
# relative to firmware/SimFW/src, forward-slash.
$rules = @(
    @{
        Name    = "hardware/i2c.h"
        Pattern = '#include\s*["<]hardware/i2c\.h'
        Allowed = @("drivers/mcp23017.c", "drivers/mcp23017.h", "tasks/i2c_owner.c", "tasks/i2c_owner.h")
    },
    @{
        Name    = "hardware/pio.h"
        Pattern = '#include\s*["<]hardware/pio\.h'
        Allowed = @("drivers/max31856_pio_engine.c", "drivers/max31856_pio_engine.h")
    },
    @{
        Name    = "hardware/pwm.h"
        Pattern = '#include\s*["<]hardware/pwm\.h'
        Allowed = @("drivers/ct_wave_pwm.c", "drivers/ct_wave_pwm.h")
    },
    @{
        Name    = "hardware/dma.h"
        Pattern = '#include\s*["<]hardware/dma\.h'
        Allowed = @("drivers/ct_wave_pwm.c", "drivers/ct_wave_pwm.h")
    },
    @{
        Name    = "tusb.h (TinyUSB)"
        Pattern = '#include\s*["<]tusb\.h'
        Allowed = @("tasks/usb_owner.c", "tasks/usb_descriptors.c")
    }
)

$excludeDirs = @('\\build\\', '\\managed_components\\', '\\\.venv\\')

$files = Get-ChildItem -Path $srcRoot -Recurse -File -Include "*.c", "*.h" -ErrorAction SilentlyContinue |
    Where-Object {
        $full = $_.FullName
        foreach ($ex in $excludeDirs) {
            if ($full -match $ex) { return $false }
        }
        return $true
    }

$failures = @()
foreach ($f in $files) {
    $rel = $f.FullName.Substring($srcRoot.Length + 1) -replace '\\', '/'
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    foreach ($rule in $rules) {
        if ($rule.Allowed -contains $rel) { continue }
        for ($i = 0; $i -lt $codeLines.Count; $i++) {
            if ($codeLines[$i] -match $rule.Pattern) {
                $failures += "$($rel):$($i + 1): includes $($rule.Name), whose only owner(s) are [$($rule.Allowed -join ', ')] -- $($codeLines[$i].Trim())"
            }
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "SINGLE-OWNER CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "$($failures.Count) single-owner violation(s) found -- see docs/PLAN.md section 4 ('every hardware interface has exactly one owner task')"
}

Write-Host "Single-owner check passed: hardware/i2c.h, hardware/pio.h, hardware/pwm.h, hardware/dma.h and tusb.h each appear only in their declared owner file(s)."
exit 0
