# Upstream and source ownership

Localify upstream: https://git.chinosk6.cn/chinosk/gkms-localify-dmm.git

The authoritative pin is [upstream.lock.json](upstream.lock.json): commit `098a41459e17b130be02353f26d0ab0a2ccad914`, tree `622df9ccc55469741c301caa0ed5b30c64c369d3`.

| Location | Ownership and purpose |
| --- | --- |
| `src/vr/` | VR runtime, OpenXR/D3D11 rendering, input, settings, and Localify integration adapters |
| `src/host/`, `src/hooks/` | Injection host, hook management, and desktop UI integration |
| `deps/openxr/` | Minimal OpenXR header subset with Khronos license notices |
| `scripts/`, root PowerShell scripts | Public fetch, build, package, and installation workflow |
| `.upstream/localify/` | Ignored, pinned upstream checkout; never edit in place |
| `build/source/` | Ignored combined build input; regenerated from upstream and local sources |

The public repository starts with selected source files, not the development repository's Git history. Internal hardware logs, extracted game data, investigation notes, development backlogs, private machine configuration, and generated build products are excluded.

The pinned raw source is verified and staged, then receives the exact patches in `patches/manifest.json`. CMake directly compiles the resulting upstream business implementations and this repository's owned adapters. There are no whole-file substitutions. ImGui uses the same patch pipeline. Invalid patch inputs/outputs or stale receipts stop build, packaging and installation.

See [patches/README.md](patches/README.md) for the shared pipeline and named seam contracts. `scripts/check-source-ownership.py` checks candidate tracked files and untracked additions for forbidden upstream trees, retired copies and matching long upstream function bodies. Generated source remains ignored.

## Updating upstream

1. Review changes between the current pin and the candidate upstream commit, especially every path in the patch manifest.
2. Port required behavior into the local adapters. Do not patch the fetched checkout as a permanent fix.
3. Update the full commit and tree hashes in `upstream.lock.json` together. Review and regenerate each exact patch preimage/postimage digest; never bypass a mismatch.
4. Preserve any useful local work in `.upstream/localify/`, then remove that generated checkout and rerun `scripts/fetch-upstream.ps1`.
5. Run the boundary and source-layout checks, relevant C++ tests, a Release build, packaging, and hardware validation before publishing a new runtime.

Do not merge the development repository or upstream history into this public repository. See [BUILDING.md](BUILDING.md) for commands.
