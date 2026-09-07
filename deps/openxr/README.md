# OpenXR headers

This directory contains the minimal ABI subset needed by the D3D11 session,
swapchain display, frame/pose loop, dual-controller action input, and
`xrEnumerateApiLayerProperties`. The definitions were extracted without
semantic changes from Khronos OpenXR-SDK `release-1.1.61`, commit
`5267613edf3d937e3d77556a106a65c2f82b25c6`.

Upstream: <https://github.com/KhronosGroup/OpenXR-SDK>

The generated headers are offered under `Apache-2.0 OR MIT`, matching their
upstream SPDX headers. They are intentionally incomplete: add future API surface
from the same generated upstream headers instead of guessing ABI definitions.

The runtime loader itself is not linked into `version.dll`. Deployment must put
the official 64-bit `openxr_loader.dll` next to the game executable; the runtime
loads that exact app-local path dynamically.
