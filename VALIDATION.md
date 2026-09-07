# Public source validation

## 2026-09-07 — v3.4.1-vr-stereo.366

Prepared in a new public repository directory with no imported development history. Runtime sources were selected from development commit `7c770a0a96fae031c551c2922e758bf7decd697b`; the public export changes build layout and documentation, not runtime behavior.

- Fetched Localify commit `098a41459e17b130be02353f26d0ab0a2ccad914` directly from its configured remote into a new shallow checkout. Verified tree `622df9ccc55469741c301caa0ed5b30c64c369d3`.
- Repeated fetch successfully reused the checkout. A deliberate modification to its README was rejected; original bytes were restored and the clean boundary check passed again.
- Built Release x64 using a new repository-local Python environment and Conan cache, Python 3.11.5, Visual Studio 2022 17.14.39, Conan 2.32.0, and CMake 4.4.3. No development-tree build outputs or Conan cache were copied.
- `test-source-layout.ps1`: all 472 staged input files matched their original owners by SHA-256; upstream sources remained outside the public source root.
- `test-head-pose-core.ps1` and `test-pointer-smoother.ps1`: compiled and passed.
- All 10 included PowerShell scripts passed syntax parsing.
- All 51 local links in the root Markdown documents resolved. All 117 selected runtime/header files matched the development working tree byte-for-byte before Git line-ending normalization.
- Packaging fetched the official OpenXR loader 1.1.61 archive and verified the pinned archive/DLL hashes. The package includes the project and third-party notices, including notices from all nine installed Conan dependency packages.
- The five-file installer completed against the local game installation. The installed DLL hash matched the Release output below; this was an ordinary installation with diagnostic file logging disabled.
- The public Git index contains 151 selected files. Fetched upstream, tool caches, build products, game data, and internal investigation artifacts are excluded. The newly written workflow/documentation files passed Git whitespace checking; imported source and third-party files retain their existing whitespace.

Release DLL SHA-256:

```text
4ADFEA2F58D32835740FF929DAADC94A025AAAD2B7992192C934F34A9132FC4C
```

The build completed with warnings C4715, C4828, C4834 and LNK4099 (including missing OpenSSL dependency PDBs). This export does not resolve those existing source/dependency warnings. No new headset or visual validation was performed; the checks above establish fetch/build integrity and offline behavior only.

## 2026-09-07 — v1.0.0 public version and baseline

User promoted hardware-accepted v3.4.1-vr-stereo.366 to the baseline.
Adopted semantic release versions, rc candidates and dev.N hardware probes.
Runtime/package version is v1.0.0; settings bottom-right label is
`gakumas-VRify v1.0.0`. No runtime mechanism changes.

Development and public Release builds and upstream boundary checks PASS;
public source-layout check PASS (472 files). Both packages built and five-file
installers completed. Final game installation is the public Release package,
with diagnostic file logging disabled. Installed and public build DLL SHA-256:
`878B0EDE24CAF89CA969EBCA771253DF39518087713E5556A85B8BB0DB37AA60`.
Existing compiler/dependency warnings remain. No new headset/visual test run.
