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
# Local-owned wrap adapter; keep the locked source byte-identical. Validate the
# signature before inserting so upstream changes cannot silently skip this fix.
$wrapPattern = 'const char\* ImFont::CalcWordWrapPositionA\(float scale, const char\* text, const char\* text_end, float wrap_width\) const\r?\n\{'
if ([regex]::Matches($source, $wrapPattern).Count -ne 1) {
    throw 'ImGui upstream drift: expected one CalcWordWrapPositionA definition.'
}
$source = [regex]::Replace($source, $wrapPattern, {
    param($match)
    $match.Value + "`n    if (const char* cjk = gakumas::ui::CjkWordWrapPosition(*this, scale, text, text_end, wrap_width)) return cjk;"
})
$includeMarker = '#include "imgui_internal.h"'
if ([regex]::Matches($source, [regex]::Escape($includeMarker)).Count -ne 1) {
    throw 'ImGui upstream drift: expected one internal header include.'
}
$source = $source.Replace($includeMarker, $includeMarker + "`n" + '#include "host/ImGuiCjkWordWrap.hpp"')
New-Item -ItemType Directory -Path $resolved -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $resolved 'imgui_draw.cpp'), $source, [Text.UTF8Encoding]::new($false))
