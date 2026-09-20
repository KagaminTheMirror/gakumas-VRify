#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& python (Join-Path $root 'scripts/source-pipeline.py') verify
if ($LASTEXITCODE -ne 0) { throw 'Prepare staged sources before testing.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$build = Join-Path $root 'build/hook-manager-tests'
New-Item -ItemType Directory -Force -Path $build | Out-Null
$stage = Join-Path $root 'build/source'
$source = Join-Path $PSScriptRoot 'cpp/hook_manager_tests.cpp'
$command = 'call "{0}" -arch=x64 -host_arch=x64 && cl.exe /nologo /std:c++20 /utf-8 /EHsc /FIcstdio /I"{1}/src" /I"{1}/deps/minhook/include" /Fo:"{2}\\" /Fe:"{2}/contracts.exe" "{3}" "{1}/src/hooks/HookManager.cpp"' -f $dev,$stage,$build,$source
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw 'HookManager test compilation failed.' }
& (Join-Path $build 'contracts.exe')
if ($LASTEXITCODE -ne 0) { throw 'HookManager behavior failed.' }
