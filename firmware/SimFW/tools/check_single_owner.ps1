# check_single_owner.ps1 -- enforces docs/DESIGN_NOTES.md section 4's central rule:
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
# real violation of DESIGN_NOTES.md section 4's rule (which is about one INTERFACE,
# not the GPIO block in the abstract).
#
# DMA is the one peripheral where the include rule is NOT the invariant, and
# this script says so twice: once at the hardware/dma.h rule and once at the
# "DMA SAFETY RULES" section near the bottom. The DMA block is a global pool
# of 12 channels, so "who includes the header" cannot express what actually
# keeps two owners apart -- under an include-only rule a third owner is a
# one-line edit to an allowlist that passes CI while silently exhausting the
# pool. The extra section checks how a channel is ACQUIRED
# (dma_claim_unused_channel only, no fixed-number claims, no raw dma_hw->ch[]
# from a non-owner), that the two DMA IRQ VECTORS are owned by different files
# with one handler each, and that the total channel COUNT still fits in 12 --
# the last derived from the firmware's own channel-count constants using
# docs/HARDWARE.md section 1b.2's closed form, and backed by a _Static_assert
# in src/main.c that the compiler evaluates against the SDK's NUM_DMA_CHANNELS.
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

# The two files (plus their headers) that are allowed to own DMA channels.
# Used both by the hardware/dma.h include rule and by the DMA safety rules
# further down, so the two can never drift apart.
$dmaOwnerFiles = @("drivers/ct_wave_pwm.c", "drivers/ct_wave_pwm.h",
                   "drivers/max31856_pio_engine.c", "drivers/max31856_pio_engine.h")

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
        # Two owners, deliberately -- and note this include rule is only the
        # CHEAP OWNER GATE for DMA, not the invariant. The DMA block is twelve
        # independent channels out of one global pool, not one shared
        # interface, so "who includes the header" is the wrong question on its
        # own: a third owner would be a one-line edit to this Allowed list
        # that passes CI while silently exhausting the pool. What actually
        # keeps the owners from colliding is checked separately below, in the
        # DMA SAFETY RULES section: how a channel is acquired
        # (dma_claim_unused_channel only), who may touch raw channel
        # registers, disjoint IRQ vectors, and the total channel count against
        # docs/HARDWARE.md section 1b's budget. Keep both.
        Pattern = '#include\s*["<]hardware/dma\.h'
        Allowed = $dmaOwnerFiles
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

# ---------------------------------------------------------------------------
# DMA SAFETY RULES
#
# The include rule above answers "who includes hardware/dma.h". That is a
# useful cheap gate but it is NOT the invariant that keeps two DMA owners from
# destroying each other, because the DMA block is a global pool of 12 channels
# rather than one interface. The real invariant, from docs/HARDWARE.md section
# 1b, has three parts, and all three are checked here:
#
#   1. ACQUISITION. A channel is only ever taken with dma_claim_unused_channel()
#      -- never dma_channel_claim(n)/dma_claim_mask(), which name a fixed
#      channel number and will happily take one another owner already holds
#      (or, with required=true, panic at boot on a fixture that was fine
#      yesterday). And raw dma_hw->ch[n] / dma_channel_hw_addr(n) register
#      pokes stay inside the owner files, so nobody drives a channel they did
#      not claim.
#   2. VECTORS. The two DMA IRQ vectors are owned by different files, one
#      irq_set_exclusive_handler() site each. irq_set_exclusive_handler()
#      panics if a vector already has a handler, so a second site on one
#      vector is a boot-time hard fault, not a subtle bug.
#   3. BUDGET. The total number of channels claimed fits in the RP2040's 12.
#      This one is a count, not a pattern, so it is not grepped: it is
#      re-derived below from the same #define'd channel-count constants the
#      firmware itself uses, matching section 1b.2's closed form. src/main.c
#      carries the authoritative _Static_assert (same formula, checked against
#      the SDK's own NUM_DMA_CHANNELS at compile time); this script re-derives
#      it so the budget is also enforced without a toolchain, and refuses to
#      pass if that _Static_assert has been removed.
#
# Why the budget matters more than it looks: every DMA claim in the fixture
# passes required = false, so exhaustion is SILENT (section 1b.5). A 13th
# channel does not panic -- it boots a fixture with no CT output or one dead
# SPI bus.
$dmaFailures = @()

# --- Rule 1: acquisition ---------------------------------------------------
$bannedAcquire = @(
    @{
        Pattern = 'dma_channel_claim\s*\('
        What    = "dma_channel_claim(n)"
        Fix     = "claim by number takes a specific channel out of the global pool, which is exactly how two owners collide. Use dma_claim_unused_channel(false) and keep the returned index, like drivers/ct_wave_pwm.c and drivers/max31856_pio_engine.c do."
    },
    @{
        Pattern = 'dma_claim_mask\s*\('
        What    = "dma_claim_mask(mask)"
        Fix     = "same problem as dma_channel_claim(n) -- it names fixed channel numbers. Call dma_claim_unused_channel(false) once per channel needed."
    }
)
# Raw channel-register access. Legal, and used, inside the owner files (e.g.
# max31856_pio_engine.c's data_stop() clears al1_ctrl and drives dma_hw->abort);
# outside them it means poking a channel you never claimed.
$bannedRawHw = @(
    @{ Pattern = 'dma_hw\s*->';            What = "dma_hw-> (raw DMA register block access)" },
    @{ Pattern = 'dma_channel_hw_addr\s*\('; What = "dma_channel_hw_addr(n)" }
)

# --- Rule 2: IRQ vectors ---------------------------------------------------
# Every irq_set_exclusive_handler() call site, plus the #define aliases that
# stand in for a vector token (ct_wave_pwm.c passes CT_WAVE_DMA_IRQ, not
# DMA_IRQ_1).
$dmaIrqSites = @()
$dmaIrqAliases = @{}

foreach ($f in $files) {
    $rel = $f.FullName.Substring($srcRoot.Length + 1) -replace '\\', '/'
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        $line = $codeLines[$i]

        foreach ($b in $bannedAcquire) {
            if ($line -match $b.Pattern) {
                $dmaFailures += "$($rel):$($i + 1): uses $($b.What) -- $($line.Trim())`n      Do this instead: $($b.Fix)`n      See docs/HARDWARE.md section 1b (DMA channel budget)."
            }
        }

        if ($dmaOwnerFiles -notcontains $rel) {
            foreach ($b in $bannedRawHw) {
                if ($line -match $b.Pattern) {
                    $dmaFailures += "$($rel):$($i + 1): uses $($b.What) from a non-owner file -- $($line.Trim())`n      Only [$($dmaOwnerFiles -join ', ')] may touch DMA channel registers, because only they claimed a channel. Go through that owner's public API instead.`n      See docs/HARDWARE.md section 1b (DMA channel budget)."
                }
            }
        }

        # Owners alias the vector behind a #define (ct_wave_pwm.c's
        # CT_WAVE_DMA_IRQ), so matching the literal token alone would miss
        # half the sites and quietly report "no conflict". Record the raw
        # first argument here; resolve aliases per file below.
        if ($line -match 'irq_set_exclusive_handler\s*\(\s*([A-Za-z_][0-9A-Za-z_]*)\s*,') {
            $dmaIrqSites += [pscustomobject]@{
                File   = $rel
                Line   = $i + 1
                Vector = $Matches[1]
                Text   = $line.Trim()
            }
        }
        if ($line -match '#define\s+([A-Za-z_][0-9A-Za-z_]*)\s+(DMA_IRQ_[0-9]+)\s*$') {
            $dmaIrqAliases[$Matches[1]] = $Matches[2]
        }
    }
}

# Resolve one level of aliasing (#define CT_WAVE_DMA_IRQ DMA_IRQ_1), then drop
# every site that is not a DMA vector -- PIO/UART/etc. exclusive handlers are
# none of this rule's business.
$dmaIrqSites = @($dmaIrqSites | ForEach-Object {
    $v = $_.Vector
    $hops = 0
    while ($dmaIrqAliases.ContainsKey($v) -and $hops -lt 8) { $v = $dmaIrqAliases[$v]; $hops++ }
    $_.Vector = $v
    $_
} | Where-Object { $_.Vector -match '^DMA_IRQ_[0-9]+$' })

# Floor check: if hardware/dma.h is included by anyone but not one DMA vector
# site can be resolved, this rule has stopped seeing the code (a renamed SDK
# call, a new aliasing style) and would pass vacuously forever. Say so.
if ($dmaIrqSites.Count -eq 0) {
    $dmaFailures += "DMA IRQ vectors: hardware/dma.h has owners but no irq_set_exclusive_handler(DMA_IRQ_*) call site could be resolved anywhere in src/.`n      Either the DMA completion handlers were removed (then update docs/HARDWARE.md section 1b), or this check can no longer see them -- e.g. the vector is now passed as an expression rather than a plain identifier or a #define alias of one. Do not leave it passing vacuously."
}

foreach ($grp in ($dmaIrqSites | Group-Object Vector)) {
    if ($grp.Count -gt 1) {
        $where = ($grp.Group | ForEach-Object { "$($_.File):$($_.Line)" }) -join ', '
        $dmaFailures += "$($grp.Name): installed by $($grp.Count) irq_set_exclusive_handler() call sites ($where) -- there must be exactly one.`n      irq_set_exclusive_handler() PANICS at boot if the vector already has a handler, so the second site bricks the fixture. Give the second claimant the other DMA vector, or route it through the existing handler (see max31856_pio_engine.c's s_dma_irq_installed guard, which is how one file services two SPI buses on one vector).`n      See docs/HARDWARE.md section 1b (DMA channel budget)."
    }
}
foreach ($grp in ($dmaIrqSites | Group-Object File)) {
    $vectors = @($grp.Group | ForEach-Object { $_.Vector } | Sort-Object -Unique)
    if ($vectors.Count -gt 1) {
        $dmaFailures += "$($grp.Name): owns more than one DMA IRQ vector ($($vectors -join ', ')) -- the two vectors must live in distinct files.`n      docs/HARDWARE.md section 1b's whole no-overlap argument is that DMA_IRQ_1 belongs to drivers/ct_wave_pwm.c and DMA_IRQ_0 to drivers/max31856_pio_engine.c. One file holding both means the vectors are no longer a partition between owners.`n      See docs/HARDWARE.md section 1b (DMA channel budget)."
    }
}

# --- Rule 3: budget --------------------------------------------------------
# docs/HARDWARE.md section 1b.2's closed form:
#     channels = CT zones + sum over buses of (chips_on_bus + 2)
# Each term names the header and the #define the firmware itself uses, so this
# cannot drift from the code the way a hardcoded 11 would. Extra = the per-bus
# dma_load + dma_data pair (section 1b.1).
$dmaChannelsTotal = 12   # RP2040 NUM_DMA_CHANNELS; src/main.c's _Static_assert
                         # checks against the SDK's own macro, not this copy.
$dmaBudgetTerms = @(
    @{ File = "drivers/ct_wave_pwm.h"; Const = "CT_WAVE_PWM_NUM_CHANNELS"; Extra = 0
       Role = "CT sine carrier, 1 channel per CT zone" },
    @{ File = "tasks/spi_emu_a.h";     Const = "SPI_EMU_A_CHANNEL_COUNT";  Extra = 2
       Role = "SPI bus A, 1 sniff per emulated chip + dma_load + dma_data" },
    @{ File = "tasks/spi_emu_b.h";     Const = "SPI_EMU_B_CHANNEL_COUNT";  Extra = 2
       Role = "SPI bus B, 1 sniff per emulated chip + dma_load + dma_data" }
)

$dmaTotal = 0
$dmaBreakdown = @()
foreach ($term in $dmaBudgetTerms) {
    $path = Join-Path $srcRoot ($term.File -replace '/', '\')
    if (-not (Test-Path $path)) {
        $dmaFailures += "DMA budget: cannot find $($term.File), which defines $($term.Const).`n      This check re-derives docs/HARDWARE.md section 1b.2's channel count from the firmware's own constants; if the file moved, update `$dmaBudgetTerms in this script (and section 1b's table) in the same commit."
        continue
    }
    $val = $null
    $termLines = Get-CodeOnlyLines -Path $path
    foreach ($l in $termLines) {
        if ($l -match ("#define\s+" + [regex]::Escape($term.Const) + "\s+\(?\s*([0-9]+)\s*[uU]?[lL]*\s*\)?\s*$")) {
            $val = [int]$Matches[1]
            break
        }
    }
    if ($null -eq $val) {
        $dmaFailures += "DMA budget: could not read a plain integer $($term.Const) out of $($term.File).`n      Section 1b.2's budget is derived from that constant. If it became computed rather than a literal #define, move the arithmetic here deliberately -- do not let the budget silently stop being checked."
        continue
    }
    $sub = $val + $term.Extra
    $dmaTotal += $sub
    $dmaBreakdown += "    $($term.Const) ($val) + $($term.Extra) = $sub   [$($term.Role)]"
}

if ($dmaTotal -gt $dmaChannelsTotal) {
    $dmaFailures += ("DMA budget exceeded: $dmaTotal channels claimed, RP2040 has $dmaChannelsTotal.`n" +
        ($dmaBreakdown -join "`n") + "`n" +
        "      Every claim in the fixture passes required=false, so this does NOT panic -- the fixture boots and silently has no CT output or a dead SPI bus (docs/HARDWARE.md section 1b.5).`n" +
        "      Free a channel first: section 1b.4 documents the ONLY slack that exists (pair two CT zones onto one PWM slice, 3 -> 2 channels). Section 1b.3 explains why none of the 8 SPI-side channels can be given up.")
}

# The _Static_assert in src/main.c is the authoritative version of this check
# (it is evaluated against the SDK's real NUM_DMA_CHANNELS, on the real
# constants, by the compiler). This script's arithmetic is the no-toolchain
# mirror of it -- so if the assertion disappears, say so rather than quietly
# becoming the only line of defence.
$mainPath = Join-Path $srcRoot "main.c"
if (Test-Path $mainPath) {
    $mainCode = (Get-CodeOnlyLines -Path $mainPath) -join "`n"
    if ($mainCode -notmatch '_Static_assert\s*\(\s*SIMFW_DMA_CHANNELS_CLAIMED\s*<=\s*NUM_DMA_CHANNELS') {
        $dmaFailures += "main.c: the DMA budget _Static_assert (SIMFW_DMA_CHANNELS_CLAIMED <= NUM_DMA_CHANNELS) is missing.`n      That assertion is the compile-time half of docs/HARDWARE.md section 1b's budget -- it is what turns a 13th DMA channel into a build error instead of a fixture that boots with a dead subsystem. Restore it; do not rely on this script alone."
    }
} else {
    $dmaFailures += "DMA budget: src/main.c not found, so the compile-time _Static_assert could not be confirmed."
}

# ---------------------------------------------------------------------------
# PWM SAFETY RULES
#
# docs/HARDWARE.md section 0 item 9 (the pin-map re-check, commit 1d32e84)
# found a latent trap: PWM slice 3 (the CT waveform generator's free-running
# pacer, drivers/ct_wave_pwm.c) binds no GPIO today, but its own candidate
# outputs -- and each CT channel's own slice's unused channel-B pin -- land on
# six GPIOs docs/HARDWARE.md section 1 gives to non-PWM owners:
#
#   GPIO6/7   -- SPI bus A SCLK/MOSI (spi_emu_a.c, PIO function)
#   GPIO17    -- DRDY_MAIN_2 (spi_emu_a.c, SIO open-drain)
#   GPIO19/21 -- FAULT_MAIN_0/1 (unowned in code yet, but claimed in section 1)
#   GPIO22    -- FAULT_MAIN_2 (unowned in code yet, but claimed in section 1)
#
# A future gpio_set_function(<one of those>, GPIO_FUNC_PWM) is legal C that
# would silently put a free-running PWM carrier onto a claimed signal line.
# The include rule above already makes drivers/ct_wave_pwm.{c,h} the ONLY
# place in the tree allowed to include hardware/pwm.h, so it is also the only
# place gpio_set_function(..., GPIO_FUNC_PWM) can legitimately appear. Two
# checks, same split as the DMA section above:
#
#   1. COMPILE-TIME (the primary guard). drivers/ct_wave_pwm.c defines each
#      CT channel's GPIO as its own macro (CT_WAVE_GPIO_0/1/2) specifically so
#      a _Static_assert can check each one against the six forbidden pins --
#      reassigning a channel to a forbidden GPIO is a build error. This
#      script cannot run the compiler, so it confirms the guard block is
#      still present (not deleted) rather than re-deriving it.
#   2. LINT (this section). Independently greps every file for a literal-pin
#      gpio_set_function(<N>, GPIO_FUNC_PWM) call and checks N against the
#      forbidden list directly -- this catches a hardcoded call added
#      somewhere that bypasses the CT_WAVE_GPIO_* macros entirely, which the
#      _Static_assert above cannot see.
#
# See docs/HARDWARE.md section 0 item 9 for the full derivation.
$pwmFailures = @()

# GPIO -> "signal (owner)" exactly as docs/HARDWARE.md section 1 assigns it.
# Plain @{}, not [ordered]@{} -- PowerShell's OrderedDictionary indexer
# treats an integer key as a POSITIONAL index rather than a dictionary key
# (verified: $h[22] on an [ordered]@{22=...} silently returns $null instead
# of the value), which would make every lookup below resolve to an empty
# string instead of failing loudly. A plain Hashtable's indexer does not have
# that trap.
$pwmForbiddenGpios = @{
    6  = "SPI bus A SCLK (spi_emu_a.c) -- pacer slice 3's own channel-A candidate output"
    7  = "SPI bus A MOSI (spi_emu_a.c) -- pacer slice 3's own channel-B candidate output"
    17 = "DRDY_MAIN_2 (spi_emu_a.c) -- CT zone 0's own PWM slice's unused channel-B pin"
    19 = "FAULT_MAIN_0 (docs/HARDWARE.md section 1, no owner file yet) -- CT zone 1's own PWM slice's unused channel-B pin"
    21 = "FAULT_MAIN_1 (docs/HARDWARE.md section 1, no owner file yet) -- CT zone 2's own PWM slice's unused channel-B pin"
    22 = "FAULT_MAIN_2 (docs/HARDWARE.md section 1, no owner file yet) -- pacer slice 3's own channel-A candidate output"
}

# --- Rule 1: lint every literal-pin gpio_set_function(N, GPIO_FUNC_PWM) ----
foreach ($f in $files) {
    $rel = $f.FullName.Substring($srcRoot.Length + 1) -replace '\\', '/'
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        $line = $codeLines[$i]
        if ($line -match 'gpio_set_function\s*\(\s*([0-9]+)\s*u?\s*,\s*GPIO_FUNC_PWM\b') {
            $pin = [int]$Matches[1]
            if ($pwmForbiddenGpios.ContainsKey($pin)) {
                $pwmFailures += "$($rel):$($i + 1): gpio_set_function(GPIO$pin, GPIO_FUNC_PWM) -- GPIO$pin is $($pwmForbiddenGpios[$pin]).`n      Binding a PWM function here would put a free-running carrier onto that claimed line. See docs/HARDWARE.md section 0 item 9.`n      Line: $($line.Trim())"
            }
        }
    }
}

# --- Rule 2: the compile-time guard block must still exist -----------------
# Mirrors the DMA section's "assertion missing" check: this lint's literal-pin
# regex cannot see the CT_WAVE_GPIO_* macro path the real code uses (the pin
# comes in through a variable, not a literal, at the actual call site), so the
# _Static_assert block in drivers/ct_wave_pwm.c is the primary guard for that
# path, not this script. If it disappears, say so rather than passing
# silently on a codebase that is once again unguarded.
$ctWavePath = Join-Path $srcRoot "drivers\ct_wave_pwm.c"
if (Test-Path $ctWavePath) {
    $ctWaveCode = (Get-CodeOnlyLines -Path $ctWavePath) -join "`n"
    $requiredAsserts = @("CT_WAVE_GPIO_0", "CT_WAVE_GPIO_1", "CT_WAVE_GPIO_2")
    foreach ($macroName in $requiredAsserts) {
        if ($ctWaveCode -notmatch ('_Static_assert\s*\(\s*!CT_WAVE_PWM_GPIO_IS_FORBIDDEN\s*\(\s*' + [regex]::Escape($macroName) + '\s*\)')) {
            $pwmFailures += "drivers/ct_wave_pwm.c: the compile-time PWM-pin guard for $macroName (_Static_assert(!CT_WAVE_PWM_GPIO_IS_FORBIDDEN($macroName), ...)) is missing.`n      That assertion is the compile-time half of docs/HARDWARE.md section 0 item 9's guard -- it is what turns reassigning a CT channel onto GPIO6/7/17/19/21/22 into a build error instead of a fixture that silently drives a PWM carrier onto a claimed SPI/FAULT/DRDY line. Restore it; do not rely on this script's lint alone, since the lint only sees literal-pin call sites."
        }
    }
} else {
    $pwmFailures += "PWM safety: drivers/ct_wave_pwm.c not found, so the compile-time guard block could not be confirmed."
}

# ---------------------------------------------------------------------------
if ($failures.Count -gt 0 -or $dmaFailures.Count -gt 0 -or $pwmFailures.Count -gt 0) {
    if ($failures.Count -gt 0) {
        Write-Host "SINGLE-OWNER CHECK FAILED:" -ForegroundColor Red
        foreach ($f in $failures) {
            Write-Host "  $f" -ForegroundColor Red
        }
    }
    if ($dmaFailures.Count -gt 0) {
        Write-Host "DMA SAFETY CHECK FAILED:" -ForegroundColor Red
        foreach ($f in $dmaFailures) {
            Write-Host "  $f" -ForegroundColor Red
        }
    }
    if ($pwmFailures.Count -gt 0) {
        Write-Host "PWM SAFETY CHECK FAILED:" -ForegroundColor Red
        foreach ($f in $pwmFailures) {
            Write-Host "  $f" -ForegroundColor Red
        }
    }
    throw "$($failures.Count) single-owner violation(s), $($dmaFailures.Count) DMA safety violation(s) and $($pwmFailures.Count) PWM safety violation(s) found -- see docs/DESIGN_NOTES.md section 4 ('every hardware interface has exactly one owner task'), docs/HARDWARE.md section 1b (DMA channel budget) and docs/HARDWARE.md section 0 item 9 (PWM pacer/channel-B latent trap)"
}

Write-Host "Single-owner check passed: hardware/i2c.h, hardware/pio.h, hardware/pwm.h, hardware/dma.h and tusb.h each appear only in their declared owner file(s)."
$vectorSummary = ($dmaIrqSites | Sort-Object Vector | ForEach-Object { "$($_.Vector)->$($_.File)" }) -join ', '
Write-Host "DMA safety check passed: every channel taken via dma_claim_unused_channel(); raw channel registers touched only by the owners; DMA vectors disjoint by file [$vectorSummary]; $dmaTotal of $dmaChannelsTotal channels claimed (HARDWARE.md section 1b)."
Write-Host "PWM safety check passed: no gpio_set_function(..., GPIO_FUNC_PWM) call binds GPIO6/7/17/19/21/22 (SPI bus A SCLK/MOSI, DRDY_MAIN_2, FAULT_MAIN_0/1/2 -- HARDWARE.md section 0 item 9), and drivers/ct_wave_pwm.c's compile-time guard block is present."
exit 0
