# Build and run the host unit tests for the firmware's pure-C modules.
#   powershell -ExecutionPolicy Bypass -File tests\host\run.ps1
# Needs a host gcc (Strawberry Perl's MinGW gcc by default; override with -Gcc).
# Each tests\host\*_test.c is linked with every pure-C module below.
param([string]$Gcc = 'C:\Strawberry\c\bin\gcc.exe')
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$modules = @("$repo\src\beacon_auth.c", "$repo\src\cfg_crypto.c")
$failed = $false
foreach ($test in Get-ChildItem $PSScriptRoot -Filter '*_test.c') {
    $exe = Join-Path ([IO.Path]::GetTempPath()) ("ble_password_" + $test.BaseName + '.exe')
    Write-Host "--- $($test.Name)"
    & $Gcc -std=c99 -Wall -Wextra -Werror -O2 -I "$repo\src" $test.FullName @modules -o $exe
    if ($LASTEXITCODE -ne 0) { $failed = $true; continue }
    & $exe
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}
if ($failed) { Write-Host "host tests FAILED"; exit 1 }
Write-Host "host tests: all passed"
exit 0
