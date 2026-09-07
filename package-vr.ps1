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
$licenseRoot = Join-Path $packageRoot 'licenses'
New-Item -ItemType Directory -Path $localifyConfigRoot -Force | Out-Null
New-Item -ItemType Directory -Path $vrConfigRoot -Force | Out-Null
New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null

Copy-Item -LiteralPath $versionDll -Destination (Join-Path $packageRoot 'version.dll') -Force
Copy-Item -LiteralPath $loaderDll -Destination (Join-Path $packageRoot 'openxr_loader.dll') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot '.upstream/localify/resource/config.json') `
    -Destination (Join-Path $localifyConfigRoot 'config.json') -Force
Copy-Item -LiteralPath $loaderLicense `
    -Destination (Join-Path $licenseRoot 'openxr-loader-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') `
    -Destination (Join-Path $licenseRoot 'gakumas-localify-GPL-3.0.txt') -Force

$notices = @{
    '.upstream/localify/src/imgui/LICENSE.txt' = 'imgui-LICENSE.txt'
    '.upstream/localify/deps/minhook/LICENSE.txt' = 'minhook-LICENSE.txt'
    '.upstream/localify/deps/rapidjson/license.txt' = 'rapidjson-LICENSE.txt'
    '.upstream/localify/src/deps/UnityResolve/LICENSE' = 'UnityResolve-LICENSE.txt'
    'deps/openxr/LICENSE-APACHE-2.0.txt' = 'openxr-headers-APACHE-2.0.txt'
    'deps/openxr/LICENSE-MIT.txt' = 'openxr-headers-MIT.txt'
    'src/vr/d3d11/smaa/LICENSE.txt' = 'smaa-LICENSE.txt'
    'src/vr/d3d11/cmaa2/LICENSE.txt' = 'cmaa2-LICENSE.txt'
}
foreach ($notice in $notices.GetEnumerator()) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $notice.Key) -Destination (Join-Path $licenseRoot $notice.Value)
}
# Conan package folders have unique cache IDs. Preserve each package's complete
# license directory, including notices for transitive dependencies.
foreach ($cachePackage in Get-ChildItem -LiteralPath (Join-Path $toolsRoot 'conan2/p') -Directory) {
    $packageLicenses = Join-Path $cachePackage.FullName 'p/licenses'
    if (Test-Path -LiteralPath $packageLicenses) {
        Copy-Item -LiteralPath $packageLicenses -Destination (Join-Path $licenseRoot "conan-$($cachePackage.Name)") -Recurse
    }
}

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

$hashEntries = @(
    'version.dll',
    'openxr_loader.dll',
    'gakumas-local\config.json',
    'gakumas-local\localizationConfig.json',
    'gakumas-vr\config.json'
)
$hashLines = foreach ($entry in $hashEntries) {
    $path = Join-Path $packageRoot $entry
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
    "$hash  $entry"
}
$hashLines | Set-Content -LiteralPath (Join-Path $packageRoot 'SHA256SUMS.txt') -Encoding ascii

@(
    'Gakumas VR Localify stereo runtime package'
    "Upstream Localify commit: $((Get-Content -LiteralPath (Join-Path $projectRoot 'upstream.lock.json') -Raw | ConvertFrom-Json).commit)"
    "Fork build: $pluginVersion"
    "OpenXR loader: Khronos release-$loaderVersion x64"
    'Translation: disabled'
    'VR mode: ordinary-Camera stereo at 100% linear OpenXR resolution (menu max 1.5) with immutable three-slot eye snapshots, post-wait latest-frame selection, repeated-frame swapchain reuse, source TAA tuning plus independent per-eye TAA/motion histories, persistent eye-camera lifecycle, independent Volume stacks owned via ViaScripting, source final RenderTexture descriptor cloning, ambiguity-skipping optional VLSRP final/HDR diagnostics, source shadow participation, OpenXR-derived scalar intrinsics including physical film-gate/focal mapping with source aperture and focus distance preserved, restored eye URP post-processing with VLDOF off, VLBloom intensity left at the authored value, bloom diffusion/scatter scaled from a locked 29.9-degree mode-body FOV, eye actor _OutlineParam.xy scaled by a menu width 0-1 (default 29.9/100.24, max = pre-fix authored), SDR OpenXR output enforcement, reference-space reset handling, hidden-panel mirror-copy suspension, final pixel-row flip, short-press Grip UI panel, long-press Grip VR settings menu with live render-scale apply and outline-width reset, right-B Live and StoryPlayer pause/resume without inter-press cooldown, owned-eye VLSkyPass ComputePixelCoord view-dir matrix replacement (fovP02), completion-driven scene-ready gating with a frozen portrait loading latch, and OpenXR session kept alive across shouldRender=false headset remove/return'
    'Eye projection: farClipPlane and Matrix4x4.Frustum use max(authored source far, 5000) on owned eyes only; source/Grip camera is unchanged.'
) | Set-Content -LiteralPath (Join-Path $packageRoot 'BUILD_INFO.txt') -Encoding utf8

Write-Host "Prepared VR package: $packageRoot"
Write-Host "To install into the game (whitelist only), run: .\install-to-game.ps1"
