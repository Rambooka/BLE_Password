# Lint + unit-test the firmware (clang-tidy, host tests in tests/host) and the
# Android app (Android Lint, JVM unit tests).
#
#   powershell -File tools\lint.ps1              # both
#   powershell -File tools\lint.ps1 -Firmware    # clang-tidy only
#   powershell -File tools\lint.ps1 -Android     # Android Lint only
#
# The firmware lint reuses the compile flags from a `west build` (run a build
# first), so it sees the real Zephyr includes and Kconfig.  Both boards are
# checked because main.c has board-specific #ifdefs (SINGLE_LED).
# Exit code: 0 = no findings, 1 = findings or a tool failure.
param(
    [switch]$Firmware,
    [switch]$Android,
    [string]$AndroidDir = (Join-Path $PSScriptRoot '..\android'),
    [string]$ClangTidy = 'C:\Program Files\LLVM\bin\clang-tidy.exe'
)
if (-not $Firmware -and -not $Android) { $Firmware = $true; $Android = $true }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$failed = $false

if ($Firmware) {
    # GCC-only flags that clang doesn't know; everything else is passed through.
    $gccOnly = '^(-fno-printf-return-value|-mfp16-format=.*|-mtp=.*|-fno-reorder-functions|' +
               '--param=.*|-fno-defer-pop|-specs=.*|-fdiagnostics-color=.*)$'
    foreach ($bd in 'build_nRF52840', 'build_mdbt50q') {
        $db = Join-Path $repo "$bd\BLE_Password\compile_commands.json"
        if (-not (Test-Path $db)) {
            Write-Host "[$bd] no compile_commands.json - run west build first" -ForegroundColor Yellow
            $failed = $true
            continue
        }
        # Parenthesised so Windows PowerShell 5 enumerates the JSON array.
        # Only our own sources: the app target also compiles NCS library files.
        $src = Join-Path $repo 'src'
        $entries = @((Get-Content $db -Raw | ConvertFrom-Json) |
                     Where-Object { ($_.file -replace '/', '\').StartsWith("$src\", 'OrdinalIgnoreCase') })
        foreach ($e in $entries) {
            $e.command = (($e.command -split ' ') | Where-Object { $_ -notmatch $gccOnly }) -join ' '
            $e.command += ' --target=arm-none-eabi -Wno-unknown-warning-option'
        }
        $tmp = Join-Path ([IO.Path]::GetTempPath()) "ble_password_tidy_$bd"
        New-Item -ItemType Directory -Force $tmp | Out-Null
        ConvertTo-Json -InputObject $entries -Depth 4 | Set-Content (Join-Path $tmp 'compile_commands.json')

        Write-Host "=== clang-tidy [$bd] ===" -ForegroundColor Cyan
        $out = & $ClangTidy -p $tmp --quiet ($entries | ForEach-Object file) 2>&1 |
               ForEach-Object { "$_" } |
               Where-Object { $_ -notmatch '^\d+ warnings? generated|^Suppressed \d+ warnings|^Use -header-filter|^Error while processing' }
        $out | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0 -or ($out -match ': (warning|error):')) { $failed = $true }
    }

    Write-Host "=== host unit tests (tests/host) ===" -ForegroundColor Cyan
    & (Join-Path $repo 'tests\host\run.ps1') | Select-Object -Last 1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}

if ($Android) {
    Write-Host "=== Android Lint ($AndroidDir) ===" -ForegroundColor Cyan
    if (-not (Test-Path (Join-Path $AndroidDir 'gradlew.bat'))) {
        Write-Host "Android project not found at $AndroidDir (use -AndroidDir)" -ForegroundColor Yellow
        $failed = $true
    } else {
        if (-not $env:JAVA_HOME) { $env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr' }
        Push-Location $AndroidDir
        & .\gradlew.bat :app:lintRelease :app:testDebugUnitTest -q
        if ($LASTEXITCODE -ne 0) { $failed = $true }
        Pop-Location
        Write-Host "Report: $AndroidDir\app\build\reports\lint-results-release.html"
    }
}

if ($failed) { exit 1 }
