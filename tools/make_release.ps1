# Publish a GitHub Release for an existing, pushed tag and attach the build
# outputs (app APK, both BLE update packages, first-flash files).
#
#   pwsh -File tools\make_release.ps1 -Tag v1.2.1 -Notes <release_notes.md>
#
# Run with PowerShell 7 (pwsh): Windows PowerShell 5 garbles the stdin of
# `git credential fill`.  Uses the GitHub token git's credential manager
# already holds for github.com (never printed).  Build both boards and the
# release APK from the tagged commit first.
param(
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][string]$Notes,
    [string]$Api = 'https://api.github.com/repos/Rambooka/BLE_Password'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$cred = @('protocol=https', 'host=github.com', '') | git credential fill
$token = ($cred | Where-Object { $_ -like 'password=*' }) -replace '^password=', ''
if (-not $token) { throw 'no stored GitHub credential' }
$h = @{ Authorization = "Bearer $token"; Accept = 'application/vnd.github+json';
        'X-GitHub-Api-Version' = '2022-11-28' }

$rel = Invoke-RestMethod -Method Post -Uri "$Api/releases" -Headers $h -ContentType 'application/json' `
    -Body (@{ tag_name = $Tag; name = "BLE_Password $Tag"; body = (Get-Content -Raw $Notes);
              draft = $false; prerelease = $false } | ConvertTo-Json)
"release: $($rel.html_url)"

$assets = [ordered]@{
    "BLE_Password-$Tag-android.apk"                          = @("$repo\android\app\build\outputs\apk\release\app-release.apk", 'application/vnd.android.package-archive')
    "BLE_Password-$Tag-nrf52840dongle-dfu.zip"               = @("$repo\build_nRF52840\dfu_application.zip", 'application/zip')
    "BLE_Password-$Tag-mdbt50q-dfu.zip"                      = @("$repo\build_mdbt50q\dfu_application.zip", 'application/zip')
    "BLE_Password-$Tag-nrf52840dongle-mcuboot-migration.zip" = @("$repo\build_nRF52840\dongle_mcuboot_migration.zip", 'application/zip')
    "BLE_Password-$Tag-nrf52840dongle-app.signed.bin"        = @("$repo\build_nRF52840\BLE_Password\zephyr\zephyr.signed.bin", 'application/octet-stream')
    "BLE_Password-$Tag-mdbt50q-merged.hex"                   = @("$repo\build_mdbt50q\merged.hex", 'text/plain')
}
$uploadBase = $rel.upload_url -replace '\{.*\}$', ''
foreach ($name in $assets.Keys) {
    $path, $type = $assets[$name]
    $a = Invoke-RestMethod -Method Post -Uri "$uploadBase`?name=$name" -Headers $h -ContentType $type -InFile $path
    "uploaded $($a.name) ($($a.size) bytes)"
}
