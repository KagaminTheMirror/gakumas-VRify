[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    # This repository is the VR mod source only. The game stays elsewhere.
    [Parameter(Mandatory)]
    [string]$GameRoot,

    [switch]$SkipPackage,

    [switch]$Probe
)

$ErrorActionPreference = 'Stop'

# Repo root != game root. Never copy the game into this repository.
# Install only the Localify/VR whitelist beside gakumas.exe:
#   version.dll
#   openxr_loader.dll
#   gakumas-local\config.json
#   gakumas-local\localizationConfig.json
#   gakumas-vr\config.json

$projectRoot = $PSScriptRoot
$packageRoot = Join-Path $projectRoot 'build\vr-package'

if (-not $SkipPackage) {
    # package-vr.ps1 throws on failure and never sets an exit code, so
    # $LASTEXITCODE here still carries whatever ran before this script.
    $packageArgs = @{ Configuration = $Configuration }
    if ($Probe) {
        $packageArgs['Probe'] = $true
    }
    & (Join-Path $projectRoot 'package-vr.ps1') @packageArgs
    if (-not $?) {
        throw 'package-vr.ps1 failed'
    }
}

if (-not (Test-Path -LiteralPath $GameRoot)) {
    throw "Game root not found: $GameRoot"
}
$gameExe = Join-Path $GameRoot 'gakumas.exe'
if (-not (Test-Path -LiteralPath $gameExe)) {
    throw "gakumas.exe not found under GameRoot: $GameRoot"
}

$versionDll = Join-Path $packageRoot 'version.dll'
$loaderDll = Join-Path $packageRoot 'openxr_loader.dll'
$configJson = Join-Path $packageRoot 'gakumas-local\config.json'
$localizationJson = Join-Path $packageRoot 'gakumas-local\localizationConfig.json'
$vrJson = Join-Path $packageRoot 'gakumas-vr\config.json'
foreach ($path in @($versionDll, $loaderDll, $configJson, $localizationJson, $vrJson)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "VR package missing file: $path. Run package-vr.ps1 first."
    }
}

function Remove-VrOwnedKeys {
    param([hashtable]$Table)
    foreach ($key in @($Table.Keys)) {
        if ($key -like 'vr*') {
            $Table.Remove($key)
        }
    }
}

$gameLocal = Join-Path $GameRoot 'gakumas-local'
$gameVr = Join-Path $GameRoot 'gakumas-vr'
New-Item -ItemType Directory -Path $gameLocal -Force | Out-Null
New-Item -ItemType Directory -Path $gameVr -Force | Out-Null

Copy-Item -LiteralPath $versionDll -Destination (Join-Path $GameRoot 'version.dll') -Force
Copy-Item -LiteralPath $loaderDll -Destination (Join-Path $GameRoot 'openxr_loader.dll') -Force
Copy-Item -LiteralPath $configJson -Destination (Join-Path $gameLocal 'config.json') -Force
$destinationLocalization = Join-Path $gameLocal 'localizationConfig.json'
$mergedLocalization = Get-Content -LiteralPath $localizationJson -Raw |
    ConvertFrom-Json -AsHashtable
if (Test-Path -LiteralPath $destinationLocalization) {
    try {
        $existingLocalization = Get-Content -LiteralPath $destinationLocalization -Raw |
            ConvertFrom-Json -AsHashtable
        foreach ($key in $existingLocalization.Keys) {
            $mergedLocalization[$key] = $existingLocalization[$key]
        }
    }
    catch {
        Write-Warning 'Existing localizationConfig.json is invalid; installing package defaults.'
    }
}
# Localify no longer owns VR keys. Strip leftovers so a later Localify save
# cannot resurrect them, and do not write vrDiagnosticsEnabled here.
Remove-VrOwnedKeys $mergedLocalization
$mergedLocalization | ConvertTo-Json -Depth 20 | Set-Content `
    -LiteralPath $destinationLocalization -Encoding utf8

$vrConfigPath = Join-Path $gameVr 'config.json'
$mergedVr = Get-Content -LiteralPath $vrJson -Raw | ConvertFrom-Json -AsHashtable
$writeVrConfig = $true
if (Test-Path -LiteralPath $vrConfigPath) {
    try {
        $existingVr = Get-Content -LiteralPath $vrConfigPath -Raw |
            ConvertFrom-Json -AsHashtable
        foreach ($key in $existingVr.Keys) {
            $mergedVr[$key] = $existingVr[$key]
        }
    }
    catch {
        Write-Warning 'Existing gakumas-vr/config.json is invalid; leaving it untouched.'
        $writeVrConfig = $false
    }
}
if ($writeVrConfig) {
    # Probe is the only way a packaged install keeps file logging on. A leftover
    # game-side true would otherwise keep writing after a normal install.
    $mergedVr['vrDiagnosticsEnabled'] = [bool]$Probe
    $mergedVr | ConvertTo-Json -Depth 20 | Set-Content `
        -LiteralPath $vrConfigPath -Encoding utf8
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $GameRoot 'version.dll')).Hash
Write-Host "Installed whitelist (5 files) into: $GameRoot"
Write-Host "version.dll SHA-256: $hash"
