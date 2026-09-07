#Requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'test-upstream-boundary.ps1')
$upstreamRoot = Join-Path $projectRoot '.upstream/localify'
$stage = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build/source'))
$buildPrefix = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build')) + [IO.Path]::DirectorySeparatorChar
if (-not $stage.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase) -or (Split-Path -Leaf $stage) -ne 'source') {
    throw "Unexpected source staging path: $stage"
}
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
foreach ($directory in @('src', 'deps')) {
    Copy-Item -LiteralPath (Join-Path $upstreamRoot $directory) -Destination $stage -Recurse
}
# Only these local directories may be added to the untouched upstream tree.
foreach ($directory in @('src/host', 'src/hooks', 'src/vr', 'deps/openxr')) {
    $destination = Join-Path $stage $directory
    if (Test-Path -LiteralPath $destination) { throw "Local/upstream ownership collision: $directory" }
    Copy-Item -LiteralPath (Join-Path $projectRoot $directory) -Destination $destination -Recurse
}
& (Join-Path $PSScriptRoot 'prepare-imgui-build.ps1') -OutputDirectory (Join-Path $stage 'build/imgui-patched')
Write-Host "Prepared combined build inputs: $stage"
