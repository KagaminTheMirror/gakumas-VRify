#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
& python (Join-Path $PSScriptRoot 'source_pipeline_tests.py')
if ($LASTEXITCODE -ne 0) { throw 'Source pipeline regression failed.' }
