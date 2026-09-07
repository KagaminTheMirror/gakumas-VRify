[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [switch]$Probe
)

$ErrorActionPreference = 'Stop'

# This script prepares build/vr-package. Use install-to-game.ps1 -GameRoot
# to install into a separate game directory.

$projectRoot = $PSScriptRoot
& (Join-Path $projectRoot 'scripts/test-upstream-boundary.ps1')
$toolsRoot = Join-Path $projectRoot '.tools'
$versionHeaderPath = Join-Path $projectRoot 'src\vr\VrVersion.hpp'
$versionHeader = Get-Content -Raw -LiteralPath $versionHeaderPath
$versionMatch = [regex]::Match(
    $versionHeader,
    '#define\s+GAKUMAS_VR_VERSION\s+"(?<version>[^"]+)"'
)
if (-not $versionMatch.Success) {
    throw "GAKUMAS_VR_VERSION not found in $versionHeaderPath."
}
$pluginVersion = $versionMatch.Groups['version'].Value
$upstreamVersionHeader = Get-Content -Raw -LiteralPath (Join-Path $projectRoot '.upstream/localify/src/PlatformDefine.hpp')
$upstreamVersionMatch = [regex]::Match($upstreamVersionHeader, '#define\s+PLUGIN_VERSION\s+"(?<version>[^"]+)"')
if (-not $upstreamVersionMatch.Success) {
    throw 'PLUGIN_VERSION not found in the pinned upstream PlatformDefine.hpp.'
}
$upstreamVersion = $upstreamVersionMatch.Groups['version'].Value
$loaderVersion = '1.1.61'
$loaderArchive = Join-Path $toolsRoot "openxr_loader_windows-$loaderVersion.zip"
$loaderRoot = Join-Path $toolsRoot "openxr-loader-$loaderVersion"
$loaderUrl = "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-$loaderVersion/openxr_loader_windows-$loaderVersion.zip"
$loaderArchiveSha256 = 'CFDCA34B8CB4C2BEF6DEA05273DAFF987E58C97D5767C04F0A20B1029EAABE7F'
$loaderDllSha256 = '0C262C9384BE6BC82634E03A324E2344CDFF41A4140FA8935891666184E8C596'
$loaderDll = Join-Path $loaderRoot 'x64\bin\openxr_loader.dll'
$loaderLicense = Join-Path $loaderRoot 'share\doc\openxr\LICENSE'

$versionDll = Join-Path $projectRoot "build\bin\x64\$Configuration\version.dll"
if (-not (Test-Path -LiteralPath $versionDll)) {
    throw "Build output not found: $versionDll. Run build.ps1 first."
}

New-Item -ItemType Directory -Path $toolsRoot -Force | Out-Null
if (-not (Test-Path -LiteralPath $loaderArchive) -or
    (Get-FileHash -Algorithm SHA256 -LiteralPath $loaderArchive).Hash -ne $loaderArchiveSha256) {
    Invoke-WebRequest -UseBasicParsing -Uri $loaderUrl -OutFile $loaderArchive
}
$archiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $loaderArchive).Hash
if ($archiveHash -ne $loaderArchiveSha256) {
    throw "OpenXR loader archive hash mismatch: expected $loaderArchiveSha256, got $archiveHash."
}

if (-not (Test-Path -LiteralPath $loaderDll) -or
    (Get-FileHash -Algorithm SHA256 -LiteralPath $loaderDll).Hash -ne $loaderDllSha256) {
    New-Item -ItemType Directory -Path $loaderRoot -Force | Out-Null
    Expand-Archive -LiteralPath $loaderArchive -DestinationPath $loaderRoot -Force
}
$dllHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $loaderDll).Hash
if ($dllHash -ne $loaderDllSha256) {
    throw "OpenXR loader DLL hash mismatch: expected $loaderDllSha256, got $dllHash."
}
if (-not (Test-Path -LiteralPath $loaderLicense)) {
    throw "OpenXR loader package did not contain its license file."
}

$packageRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'build\vr-package'))
$buildRootPrefix = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'build')) +
    [System.IO.Path]::DirectorySeparatorChar
if (-not $packageRoot.StartsWith($buildRootPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
    (Split-Path -Leaf $packageRoot) -ne 'vr-package') {
    throw "Refusing to replace an unexpected package path: $packageRoot"
}
if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
$localifyConfigRoot = Join-Path $packageRoot 'gakumas-local'
$vrConfigRoot = Join-Path $packageRoot 'gakumas-vr'
New-Item -ItemType Directory -Path $localifyConfigRoot -Force | Out-Null
New-Item -ItemType Directory -Path $vrConfigRoot -Force | Out-Null

Copy-Item -LiteralPath $versionDll -Destination (Join-Path $packageRoot 'version.dll') -Force
Copy-Item -LiteralPath $loaderDll -Destination (Join-Path $packageRoot 'openxr_loader.dll') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot '.upstream/localify/resource/config.json') `
    -Destination (Join-Path $localifyConfigRoot 'config.json') -Force
$localizationSource = Join-Path $projectRoot '.upstream/localify/resource/localizationConfig.json'
$localizationConfig = Get-Content -Raw -LiteralPath $localizationSource |
    ConvertFrom-Json -AsHashtable
$localizationConfig['dbgMode'] = $false
$localizationConfig['enabled'] = $true
$localizationConfig['enableFreeCamera'] = $false
$localizationConfig['targetFrameRate'] = 120
$localizationConfig['unlockAllLive'] = $false
$localizationConfig['unlockAllLiveCostume'] = $false
foreach ($legacyVrKey in @($localizationConfig.Keys)) {
    if ($legacyVrKey -like 'vr*') {
        $localizationConfig.Remove($legacyVrKey)
    }
}
$localizationConfig | ConvertTo-Json -Depth 20 | Set-Content `
    -LiteralPath (Join-Path $localifyConfigRoot 'localizationConfig.json') `
    -Encoding utf8

$vrConfig = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'src\vr\config\defaults.json') |
    ConvertFrom-Json -AsHashtable
$vrConfig['vrDiagnosticsEnabled'] = [bool]$Probe
$vrConfig | ConvertTo-Json -Depth 20 | Set-Content `
    -LiteralPath (Join-Path $vrConfigRoot 'config.json') `
    -Encoding utf8

# Plain-text markers contain each component's own version, without BOM or newline.
[System.IO.File]::WriteAllText((Join-Path $localifyConfigRoot 'version.txt'), $upstreamVersion, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText((Join-Path $vrConfigRoot 'version.txt'), $pluginVersion, [System.Text.UTF8Encoding]::new($false))

Write-Host "Prepared VR package: $packageRoot"
Write-Host "To install into the game (whitelist only), run: .\install-to-game.ps1"
