#Requires -Version 7.0
param([string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$root = $ProjectRoot
& python (Join-Path $PSScriptRoot 'prepare-shared-entry-test.py') --root $root
if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare actual staged hook bodies.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$build = Join-Path $root 'build/shared-entry-tests'
$source = Join-Path $PSScriptRoot 'cpp/shared_entry_tests.cpp'
$command = 'call "{0}" -arch=x64 -host_arch=x64 && cl.exe /nologo /std:c++20 /utf-8 /EHsc /I"{1}" /Fo:"{1}\\" /Fe:"{1}\contracts.exe" "{2}"' -f $dev,$build,$source
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw 'Shared hook contract compilation failed.' }
& (Join-Path $build 'contracts.exe')
if ($LASTEXITCODE -ne 0) { throw 'Shared hook behavior failed.' }
