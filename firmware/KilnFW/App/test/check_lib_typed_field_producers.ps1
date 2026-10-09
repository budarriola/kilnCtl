# check_lib_typed_field_producers.ps1 -- unit cases for lib_typed_field_producers.ps1.
# `.field = {0}` is a constant placeholder and must NOT credit a producer.
$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "lib_typed_field_producers.ps1")
$fails = 0
function Assert([bool]$c, [string]$w) { if ($c) { Write-Host "  ok: $w" } else { Write-Host "  FAIL: $w" -ForegroundColor Red; $script:fails++ } }
Assert (-not (Test-FieldProducedInText -InitText '{ .a = {0}, .b = 1 }' -Vars @() -Field 'a' -CodeText '')) '.a = {0} is not a producer'
Assert (-not (Test-FieldProducedInText -InitText '{ .a = { 0 } }' -Vars @() -Field 'a' -CodeText '')) '.a = { 0 } is not a producer'
Assert (-not (Test-FieldProducedInText -InitText '{ .a = 0, .b = 1 }' -Vars @() -Field 'a' -CodeText '')) '.a = 0 is not a producer'
Assert (Test-FieldProducedInText -InitText '{ .a = compute(x), .b = 1 }' -Vars @() -Field 'a' -CodeText '') '.a = compute(x) is a producer'
Assert (Test-FieldProducedInText -InitText '{ .a = {1} }' -Vars @() -Field 'a' -CodeText '') '.a = {1} is a producer'
if ($fails -gt 0) { Write-Host "check_lib_typed_field_producers: $fails FAILED" -ForegroundColor Red; exit 1 }
Write-Host "check_lib_typed_field_producers: all cases passed"; exit 0
