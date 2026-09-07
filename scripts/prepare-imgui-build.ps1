[CmdletBinding()]
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'build\imgui-patched' }
$resolved = [IO.Path]::GetFullPath($OutputDirectory)
$buildPrefix = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build')) + [IO.Path]::DirectorySeparatorChar
if (-not $resolved.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "ImGui staging must stay under the repository build directory: $resolved"
}
$source = Get-Content -LiteralPath (Join-Path $projectRoot '.upstream/localify/src/imgui/imgui_draw.cpp') -Raw
# This pinned ImGui has mutable font-decompression pointers and two lazily
# initialized CJK range arrays outside GImGui. Give both UI threads their own.
$replacements = @{
    'static unsigned char *stb__barrier_out_e, *stb__barrier_out_b;' = 'static thread_local unsigned char *stb__barrier_out_e, *stb__barrier_out_b;'
    'static const unsigned char *stb__barrier_in_b;' = 'static thread_local const unsigned char *stb__barrier_in_b;'
    'static unsigned char *stb__dout;' = 'static thread_local unsigned char *stb__dout;'
    'static ImWchar full_ranges[' = 'static thread_local ImWchar full_ranges['
}
foreach ($entry in $replacements.GetEnumerator()) {
    $count = ([regex]::Matches($source, [regex]::Escape($entry.Key))).Count
    $expected = if ($entry.Key -eq 'static ImWchar full_ranges[') { 2 } else { 1 }
    if ($count -ne $expected) { throw "ImGui upstream drift: expected $expected occurrences of $($entry.Key), found $count" }
    $source = $source.Replace($entry.Key, $entry.Value)
}
New-Item -ItemType Directory -Path $resolved -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $resolved 'imgui_draw.cpp'), $source, [Text.UTF8Encoding]::new($false))
