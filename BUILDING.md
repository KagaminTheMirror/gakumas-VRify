# Building gakumas-VRify

## Requirements

- Windows x64. The native OpenXR/D3D11 DLL requires the Windows SDK and MSVC; Linux and macOS builds are not supported.
- PowerShell 7 (`pwsh`), Git, and Python 3.11 with `python` on `PATH` and working `venv`/`pip` modules.
- Visual Studio 2022 **17.10 or newer within the 2022 release family**, with **Desktop development with C++**, the **MSVC v143 x64/x86 build tools**, and a Windows 10 or Windows 11 SDK. The Conan profile targets MSVC 19.4x. Visual Studio 2019 or a different major toolset is not a substitute.
- Network access to the locked Localify upstream, PyPI, Conan Center, and GitHub (for packaging the OpenXR loader).

A game installation and headset are not needed to compile or run the included offline tests. They are needed to validate actual VR rendering.

## Fetch and build

Run these commands in PowerShell 7:

```powershell
git clone https://github.com/KagaminTheMirror/gakumas-VRify.git
Set-Location gakumas-VRify
./scripts/fetch-upstream.ps1
./build.ps1 -Configuration Release
```

`fetch-upstream.ps1` downloads the exact commit in `upstream.lock.json` into `.upstream/localify/` as a shallow, detached checkout. It verifies the commit, tree, origin URL, and clean working directory. It never merges upstream history into this repository. Repeating the command verifies and reuses the checkout. The locked upstream commit contains its dependency sources, so no recursive submodule update is required.

`build.ps1` also calls the fetch script, so an explicit fetch is optional. It then:

1. Creates a Python environment in `.tools/build/` and installs the pinned versions in `scripts/build-requirements.txt` (Conan 2.32.0 and CMake 4.4.3).
2. Verifies upstream, copies its `src/` and `deps/` into `build/source/`, and adds only the local `src/host/`, `src/hooks/`, `src/vr/`, and `deps/openxr/` directories. An ownership collision stops the build.
3. Generates the narrowly scoped ImGui font-state adaptation under `build/source/build/imgui-patched/`. The checked-out upstream file remains unchanged.
4. Resolves dependencies using `conan-release.lock`, `scripts/vs2022.profile`, and the repository-local `.tools/conan2/` cache.
5. Configures CMake and builds the DLL. The substitution list in `upstream.lock.json` excludes replaced upstream implementation files from compilation.

The output is **`build/bin/x64/Release/version.dll`**, with a PDB alongside it. The build does not install anything into the game. Local source changes must be rebuilt through `build.ps1` so that the combined source tree is refreshed; editing `build/source/` directly is temporary and unsupported.

To reduce memory pressure or build Debug instead:

```powershell
./build.ps1 -Configuration Release -CompilerProcesses 2
./build.ps1 -Configuration Debug
```

Release is the validated distribution configuration. Debug may require additional Conan dependency builds. The dependency graph is locked, but compiler versions, paths, timestamps and available package binaries can affect DLL hashes; byte-identical output is not promised.

## Offline checks

```powershell
./scripts/test-upstream-boundary.ps1
./tests/test-source-layout.ps1
./tests/test-head-pose-core.ps1
./tests/test-pointer-smoother.ps1
```

The source-layout check runs after a build (or `./scripts/prepare-source.ps1`) and verifies that staged inputs match their original files. The C++ tests compile standalone harnesses with VS2022 and test pose calculations/mailboxes and pointer smoothing without a game or OpenXR runtime. They do not establish visual correctness on a headset.

## Package and install

```powershell
./package-vr.ps1 -Configuration Release
./install-to-game.ps1 -Configuration Release -GameRoot 'D:\Games\gakumas'
```

Replace the example path with the directory containing `gakumas.exe`. Close the game first. Packaging creates `build/vr-package/` and downloads the official Khronos OpenXR loader **1.1.61 x64**, checking both the archive and DLL against the SHA-256 values embedded in the script. The installer packages automatically unless passed `-SkipPackage`.

Only these five paths are installed into the game:

```text
version.dll
openxr_loader.dll
gakumas-local/config.json
gakumas-local/localizationConfig.json
gakumas-vr/config.json
```

The installer merges existing settings and keeps VR configuration separate from Localify. The release package contains only the two DLLs, three JSON configuration files, and a `version.txt` in each of `gakumas-local/` and `gakumas-vr/`. The Localify marker contains the pinned upstream PLUGIN_VERSION (currently `v3.4.1`); the VR marker contains GAKUMAS_VR_VERSION (currently `v1.0.0`). Each file contains only its version, without BOM or newline. The five-file development installer does not copy these release markers. License sources are listed in `THIRD_PARTY_NOTICES.md`; licenses, build information and checksum files are not included in the release ZIP. Game assets and translation datasets are not part of this source repository.

Ordinary installs disable VR diagnostic file logging. To collect a diagnostic log:

```powershell
./install-to-game.ps1 -GameRoot 'D:\Games\gakumas' -Probe
```

Restart the game after installation. Logs are written under the game's `gakumas-vr/logs/` directory. Installing again without `-Probe` disables diagnostic file logging.

## Troubleshooting and cleanup

- **Upstream fetch failed:** check access to the URL in `upstream.lock.json`. If a first fetch was interrupted, remove only the incomplete `.upstream/localify/` directory and rerun the fetch script.
- **Upstream is dirty or has the wrong commit:** preserve any intentional edits elsewhere, then remove `.upstream/localify/` and fetch again. The script deliberately does not reset or overwrite a dirty checkout. Follow [UPSTREAM.md](UPSTREAM.md) when changing the pin.
- **VS2022 was not found:** install the required C++ workload and v143 toolset; the script discovers the installation through `vswhere.exe`.
- **Conan/Python downloads failed:** check the failing host and retry. `.tools/` is local to this repository; deleting it recreates the Python environment and Conan cache on the next build.
- **Stale build files:** remove `build/` and rerun `build.ps1`. Both `build/` and `.tools/` contain generated files only. Never use the game installation as a build directory.

See [VALIDATION.md](VALIDATION.md) for the recorded clean-directory Release build results.
