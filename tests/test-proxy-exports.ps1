param([string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
& python (Join-Path $PSScriptRoot 'check-proxy-exports.py') --root $ProjectRoot
if ($LASTEXITCODE -ne 0) { throw 'Proxy export set changed.' }
