#Requires -Version 7.0
[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'prepare-source.ps1')
$projectRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $projectRoot 'build/source/src/imgui/imgui_draw.cpp'
if ($OutputDirectory) {
    $destination = [IO.Path]::GetFullPath($OutputDirectory)
    $prefix = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build')) + [IO.Path]::DirectorySeparatorChar
    if (-not $destination.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Output must be under build.' }
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    Copy-Item -LiteralPath $source -Destination (Join-Path $destination 'imgui_draw.cpp')
}
