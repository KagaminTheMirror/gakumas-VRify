#Requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$receipt = Join-Path $projectRoot 'build/source-receipt.json'
try {
    & (Join-Path $PSScriptRoot 'test-upstream-boundary.ps1')
    & python (Join-Path $PSScriptRoot 'source-pipeline.py') stage
    if ($LASTEXITCODE -ne 0) { throw 'Source preparation failed.' }
} catch {
    if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt -Force }
    throw
}
