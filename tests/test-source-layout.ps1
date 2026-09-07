#Requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $projectRoot 'scripts/test-upstream-boundary.ps1')
$stage = Join-Path $projectRoot 'build/source'
if (-not (Test-Path -LiteralPath $stage)) { throw 'Run build.ps1 or scripts/prepare-source.ps1 first.' }
$checks = 0
foreach ($entry in @(
    @{ Root = (Join-Path $projectRoot '.upstream/localify'); Paths = @('src', 'deps') },
    @{ Root = $projectRoot; Paths = @('src/host', 'src/hooks', 'src/vr', 'deps/openxr') }
)) {
    foreach ($directory in $entry.Paths) {
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $entry.Root $directory) -Recurse -File) {
            $relative = [IO.Path]::GetRelativePath($entry.Root, $file.FullName)
            $staged = Join-Path $stage $relative
            if (-not (Test-Path -LiteralPath $staged) -or
                (Get-FileHash -LiteralPath $staged).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) {
                throw "Staged source differs from its owner: $relative"
            }
            $checks++
        }
    }
}
foreach ($forbidden in @('src/GakumasLocalify', 'src/imgui', 'src/main.cpp', 'deps/minhook', 'deps/rapidjson', 'resource')) {
    if (Test-Path -LiteralPath (Join-Path $projectRoot $forbidden)) { throw "Upstream source leaked into the public root: $forbidden" }
}
Write-Host "PASS: $checks staged source files match their owners; public and upstream source directories are separate."
