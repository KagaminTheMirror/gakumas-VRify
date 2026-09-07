#Requires -Version 7.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath (Join-Path $projectRoot 'upstream.lock.json') -Raw | ConvertFrom-Json
$upstreamRoot = Join-Path $projectRoot '.upstream/localify'
if (-not (Test-Path -LiteralPath (Join-Path $upstreamRoot '.git'))) {
    throw 'Upstream is missing. Run ./scripts/fetch-upstream.ps1 first.'
}
$actualCommit = & git -C $upstreamRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $lock.commit) { throw 'Upstream commit differs from upstream.lock.json.' }
$actualTree = & git -C $upstreamRoot rev-parse 'HEAD^{tree}'
if ($LASTEXITCODE -ne 0 -or $actualTree -ne $lock.tree) { throw 'Upstream tree differs from upstream.lock.json.' }
$remote = & git -C $upstreamRoot remote get-url origin
if ($LASTEXITCODE -ne 0 -or $remote -ne $lock.url) { throw 'Upstream origin differs from upstream.lock.json.' }
$dirty = & git -C $upstreamRoot status --porcelain=v1 --untracked-files=all --ignored
if ($LASTEXITCODE -ne 0 -or $dirty) { throw "Upstream checkout is not clean:`n$($dirty -join "`n")" }
foreach ($substitution in $lock.substitutions) {
    if (-not (Test-Path -LiteralPath (Join-Path $projectRoot $substitution.local) -PathType Leaf)) {
        throw "Missing local substitute: $($substitution.local)"
    }
}
Write-Host "Upstream boundary OK: $actualCommit (tree $actualTree)"
