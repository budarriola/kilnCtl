# check_recovery_status_fragments.ps1 -- source-level guards on recovery_http.c (no toolchain needed).
#
# 1. GET /api/recovery/status streams through send_frag(), whose static buffer
#    is fixed. The `last_install` fragment carries the longest reason name
#    (update_install_reason_name table, 34 chars today) and a 31-char version;
#    it must fit that buffer in the worst case and must not share a fragment
#    with the numeric heap fields (REVIEW_AUTOFX2 LOW-1: a combined fragment
#    overflowed at a 14-char version and the stream was cut short).
# 2. ota_esp_post must call recovery_install_supersede() BEFORE it sets the
#    boot partition, like recovery_exit_post (REVIEW_AUTOFX2 LOW-3), and must
#    report the supersede result.
# 3. Both images report last_install.id (the install identity mcp_server_update.py
#    matches), built from the loaded record (REVIEW_AUTOFX5 LOW).
#
# Contract (tools/run_all_checks.ps1): exit 0 PASS, anything else FAIL.
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$http = Get-Content -Raw -LiteralPath (Join-Path $here "recovery_http.c")
$rec = Get-Content -Raw -LiteralPath (Join-Path $here "..\..\KilnFW\App\drivers\update\update_install_record.c")
$fails = @()

# --- 1. fragment buffer ---------------------------------------------------
if ($http -notmatch 'static char frag\[(\d+)\]') { throw "send_frag's static frag[N] buffer not found" }
$fragSize = [int]$Matches[1]

$nameFn = [regex]::Match($rec, 'update_install_reason_name\(uint8_t reason\)\s*\{(?<b>.*?)return reason', 'Singleline')
if (-not $nameFn.Success) { throw "update_install_reason_name table not found" }
# Any quoted name counts (digits and capitals too: REVIEW_AUTOFX3 LOW-3), and the number found must equal the enum's.
$names = [regex]::Matches($nameFn.Groups['b'].Value, '"([^"\\]*)"') | ForEach-Object { $_.Groups[1].Value.Length }
$recH = Get-Content -Raw -LiteralPath (Join-Path $here "..\..\KilnFW\App\drivers\update\update_install_record.h")
$enum = [regex]::Match($recH, 'typedef enum \{\s*UPD_INST_REASON_NONE(?<b>.*?)UPD_INST_REASON_COUNT\b', 'Singleline')
if (-not $enum.Success) { throw "UPD_INST_REASON enum not found" }
$enumCount = 1 + ([regex]::Matches($enum.Groups['b'].Value, 'UPD_INST_REASON_[A-Z0-9_]+')).Count
if (@($names).Count -ne $enumCount) { $fails += "reason-name parser found $(@($names).Count) names but the enum has $enumCount" }
$maxReason = ($names | Measure-Object -Maximum).Maximum
if ($rec -notmatch '#define UPD_INST_SEMVER_LEN') {
    $hdr = Get-Content -Raw -LiteralPath (Join-Path $here "..\..\KilnFW\App\drivers\update\update_install_record.h")
} else { $hdr = $rec }
if ($hdr -notmatch 'UPD_INST_SEMVER_LEN\s+(\d+)u?') { throw "UPD_INST_SEMVER_LEN not found" }
$maxVer = [int]$Matches[1] - 1
$maxStr = [Math]::Max($maxReason, [Math]::Max($maxVer, 4))

# Each send_frag( ... ) call: concatenated string literals up to the first literal-ending comma/paren.
$calls = [regex]::Matches($http, 'send_frag\(req,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)')
$lastInstall = @()
foreach ($c in $calls) {
    $lit = ([regex]::Matches($c.Groups[1].Value, '"((?:[^"\\]|\\.)*)"') | ForEach-Object { $_.Groups[1].Value }) -join ''
    if ($lit -match 'last_install') { $lastInstall += $lit }
}
if ($lastInstall.Count -ne 1) { $fails += "expected exactly one send_frag fragment carrying last_install, found $($lastInstall.Count)" }
foreach ($lit in $lastInstall) {
    if ($lit -match 'free_heap|heap_internal_min_free|nvs_failed_mask') {
        $fails += "the last_install fragment shares a send_frag buffer with the numeric status fields"
    }
    $plain = $lit -replace '\\"', '"'
    $nS = ([regex]::Matches($plain, '%s')).Count
    $nU = ([regex]::Matches($plain, '%u|%lu')).Count
    $worst = ($plain -replace '%s|%u|%lu', '').Length + $nS * $maxStr + $nU * 10
    Write-Host "last_install fragment worst case $worst B (reason<=$maxReason, version<=$maxVer) of $fragSize B"
    if ($worst -ge $fragSize) { $fails += "last_install fragment worst case $worst B does not fit frag[$fragSize]" }
}

# --- 2. ota_esp_post ordering --------------------------------------------
$m = [regex]::Match($http, 'static esp_err_t ota_esp_post\(.*?\n\}', 'Singleline')
if (-not $m.Success) { throw "ota_esp_post not found" }
$body = $m.Value
# The call must be a statement at the function's top indentation (not inside if (0) { } or a nested block).
$mSup = [regex]::Match($body, '(?m)^ {4}(?:int\s+\w+\s*=\s*|\(void\))?recovery_install_supersede\(\)')
$iSup = if ($mSup.Success) { $mSup.Index } else { -1 }
$iSet = $body.IndexOf('recovery_boot_partition_set_and_verify(')
if ($iSup -lt 0) { $fails += "ota_esp_post does not call recovery_install_supersede() as a top-level statement" }
elseif ($iSet -lt 0) { throw "ota_esp_post: boot partition set not found" }
elseif ($iSup -gt $iSet) { $fails += "ota_esp_post supersedes AFTER setting the boot partition" }
if ($body -notmatch 'supersede_rc == 0') { $fails += "ota_esp_post does not report a failed supersede in its response" }

# --- 3. install identity is reported (REVIEW_AUTOFX5) ----------------------
# mcp_server_update.py matches last_install.id against the staged sha256 prefix. Both the recovery
# status fragment and the application's GET /api/update/status must emit it, filled from the record
# (an all-zero id silently disables the match, so a dropped or hard-zeroed field would pass every
# size check above). The recovery fragment must take its id from the record when one is loaded.
foreach ($lit in $lastInstall) {
    if ($lit -notmatch '\\"id\\":\\"%s\\"') { $fails += "the recovery last_install fragment does not carry `"id`":`"%s`"" }
}
$mFrag = [regex]::Match($http, 'send_frag\(req,\s*"\\"last_install\\"[^;]*;', 'Singleline')
if (-not $mFrag.Success -or $mFrag.Value -notmatch ',\s*ir_id\s*\)\s*;') {
    $fails += "the recovery last_install fragment does not pass ir_id as its id argument"
}
if ($http -notmatch 'update_install_id_hex\(\s*have_ir\s*\?\s*ir\.id\s*:\s*no_id\s*,\s*ir_id\s*\)') {
    $fails += "recovery ir_id is not built from the loaded record's id (update_install_id_hex(have_ir ? ir.id : no_id, ir_id))"
}
$app = Get-Content -Raw -LiteralPath (Join-Path $here "..\..\KilnFW\App\drivers\update\update_install_http.c")
$mSt = [regex]::Match($app, 'static esp_err_t status_get_handler\(.*?\n\}', 'Singleline')
if (-not $mSt.Success) { throw "update_install_http.c status_get_handler not found" }
$st = $mSt.Value
if ($st -notmatch '\\"version\\":\\"%s\\",\\"id\\":\\"%s\\"\}\}') { $fails += "app /api/update/status last_install lacks `"id`":`"%s`"" }
if ($st -notmatch 'update_install_id_hex\(\s*res\.id\s*,\s*lid\s*\)') { $fails += "app lid is not built from the read record (update_install_id_hex(res.id, lid))" }
if ($st -notmatch ',\s*lv\s*,\s*lid\s*\)\s*;') { $fails += "app status snprintf does not pass lid after lv" }

if ($fails.Count) {
    $fails | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
Write-Host "PASS: recovery status fragments fit, ota_esp_post supersedes first, install id reported by both images"
exit 0
