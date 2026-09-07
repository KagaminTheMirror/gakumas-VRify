#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 32)]
    [int]$CompilerProcesses = 4
)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
& (Join-Path $projectRoot 'scripts/fetch-upstream.ps1')
$venvRoot = Join-Path $projectRoot '.tools/build'
$pythonExe = Join-Path $venvRoot 'Scripts/python.exe'
$conanExe = Join-Path $venvRoot 'Scripts/conan.exe'
$cmakeExe = Join-Path $venvRoot 'Scripts/cmake.exe'
$buildRoot = Join-Path $projectRoot "build/vs2022/$Configuration"
$lockfile = Join-Path $projectRoot 'conan-release.lock'
$profile = Join-Path $projectRoot 'scripts/vs2022.profile'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Installer vswhere.exe is required.' }
$vsRoot = & $vswhere -latest -products '*' -version '[17.10,18.0)' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vsRoot) { throw 'Visual Studio 2022 17.10+ Desktop C++ tools are required.' }
if (-not (Test-Path $pythonExe)) {
    & python -m venv $venvRoot
    if ($LASTEXITCODE -ne 0) { throw 'Failed to create the build environment.' }
}
& $pythonExe -m pip install --disable-pip-version-check -r (Join-Path $projectRoot 'scripts/build-requirements.txt')
if ($LASTEXITCODE -ne 0) { throw 'Failed to install build tools.' }
$previousConanHome = $env:CONAN_HOME
$previousPath = $env:PATH
try {
    $env:CONAN_HOME = Join-Path $projectRoot '.tools/conan2'
    $env:PATH = (Join-Path $venvRoot 'Scripts') + [IO.Path]::PathSeparator + $env:PATH
    & (Join-Path $projectRoot 'scripts/prepare-source.ps1')
    & $conanExe install (Join-Path $projectRoot 'scripts/conanfile.py') `
        --output-folder $buildRoot --lockfile $lockfile --build missing `
        -pr:h $profile -pr:b $profile -s "build_type=$Configuration" `
        -c "tools.build:jobs=$CompilerProcesses"
    if ($LASTEXITCODE -ne 0) { throw 'Conan dependency installation failed.' }
    & $cmakeExe -S (Join-Path $projectRoot 'scripts') -B $buildRoot `
        -G 'Visual Studio 17 2022' -A x64 `
        "-DCMAKE_GENERATOR_INSTANCE=$vsRoot" `
        "-DCMAKE_CONFIGURATION_TYPES=$Configuration" `
        "-DVR_COMPILER_PROCESSES=$CompilerProcesses" `
        "-DCMAKE_TOOLCHAIN_FILE=$buildRoot/conan_toolchain.cmake"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    & $cmakeExe --build $buildRoot --config $Configuration --parallel $CompilerProcesses
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
} finally {
    $env:CONAN_HOME = $previousConanHome
    $env:PATH = $previousPath
}
$outputPath = Join-Path $projectRoot "build/bin/x64/$Configuration/version.dll"
if (-not (Test-Path $outputPath)) { throw "Missing build output: $outputPath" }
Write-Host "Built: $outputPath"
