#Requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath (Join-Path $projectRoot 'upstream.lock.json') -Raw | ConvertFrom-Json
$upstreamRoot = Join-Path $projectRoot '.upstream/localify'
if ($lock.commit -notmatch '^[0-9a-f]{40}$' -or $lock.tree -notmatch '^[0-9a-f]{40}$') {
    throw 'The upstream lock must contain full commit and tree hashes.'
}
if (Test-Path -LiteralPath (Join-Path $upstreamRoot '.git')) {
    & (Join-Path $PSScriptRoot 'test-upstream-boundary.ps1')
    Write-Host 'The locked upstream checkout is already available.'
    return
}
if ((Test-Path -LiteralPath $upstreamRoot) -and @(Get-ChildItem -Force -LiteralPath $upstreamRoot).Count) {
    throw "Refusing to initialize a non-empty directory: $upstreamRoot"
}
New-Item -ItemType Directory -Force -Path $upstreamRoot | Out-Null
& git init $upstreamRoot
if ($LASTEXITCODE -ne 0) { throw 'Upstream git init failed.' }
& git -C $upstreamRoot config core.autocrlf false
if ($LASTEXITCODE -ne 0) { throw 'Upstream Git configuration failed.' }
& git -C $upstreamRoot remote add origin $lock.url
if ($LASTEXITCODE -ne 0) { throw 'Adding the upstream remote failed.' }
& git -C $upstreamRoot fetch --depth 1 origin $lock.commit
if ($LASTEXITCODE -ne 0) { throw "Upstream fetch failed. Remove the incomplete $upstreamRoot directory and retry." }
& git -C $upstreamRoot -c advice.detachedHead=false checkout --detach $lock.commit
if ($LASTEXITCODE -ne 0) { throw 'Checking out the pinned upstream commit failed.' }
& (Join-Path $PSScriptRoot 'test-upstream-boundary.ps1')
