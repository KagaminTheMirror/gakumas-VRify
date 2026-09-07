[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'build\pointer-smoother-tests'))
$expectedPrefix = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'build')) +
    [System.IO.Path]::DirectorySeparatorChar
if (-not $buildRoot.StartsWith($expectedPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
    (Split-Path -Leaf $buildRoot) -ne 'pointer-smoother-tests') {
    throw "Refusing to replace unexpected test directory: $buildRoot"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}
$visualStudioRoot = & $vswhere -latest -products '*' -version '[17.0,18.0)' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $visualStudioRoot) {
    throw 'Visual Studio 2022 with the Desktop C++ toolchain was not found.'
}
$devCommand = Join-Path ($visualStudioRoot | Select-Object -First 1) 'Common7\Tools\VsDevCmd.bat'

if (Test-Path -LiteralPath $buildRoot) {
    Remove-Item -LiteralPath $buildRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null

$testSource = Join-Path $projectRoot 'tests\cpp\pointer_smoother_tests.cpp'
$filterSource = Join-Path $projectRoot 'src\vr\input\PointerSmoother.cpp'
$testExe = Join-Path $buildRoot 'pointer_smoother_tests.exe'
$compileCommand = 'call "{0}" -arch=x64 -host_arch=x64 && cd /d "{1}" && cl.exe /nologo /std:c++17 /utf-8 /EHsc /W4 /WX /Fe:"{2}" "{3}" "{4}"' -f `
    $devCommand, $buildRoot, $testExe, $testSource, $filterSource
& $env:ComSpec /d /s /c $compileCommand
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $testExe)) {
    throw 'Pointer smoother test compilation failed.'
}

& $testExe
if ($LASTEXITCODE -ne 0) {
    throw "Pointer smoother tests exited with code $LASTEXITCODE."
}
