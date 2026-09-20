#Requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
& (Join-Path $projectRoot 'scripts/test-upstream-boundary.ps1')
& python (Join-Path $projectRoot 'scripts/source-pipeline.py') verify
if ($LASTEXITCODE -ne 0) { throw 'Staged sources or ownership checks failed.' }
Write-Host 'PASS: pinned upstream, exact patches, owned overlay and public Git boundary.'
