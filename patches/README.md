# Locked upstream integration

Production compiles Localify from the locked upstream commit into `build/source`. The private checkout retains upstream history and original files. The public checkout obtains that same commit in ignored `.upstream/localify`. Neither edits upstream in place. `upstream.lock.json` has no whole-file substitutions.

`manifest.json` orders compatibility fixes and named integration seams. Each entry identifies its purpose, regression checks, patch SHA-256, input SHA-256 and output SHA-256 for every affected file. A later patch on the same path names the exact intermediate input. The pipeline verifies the commit/tree, original working files, patch bytes, complete preimages, application and complete postimages. It rejects undeclared changed paths. There is no fuzzy fallback, three-way merge, or alternative fork implementation.

Only successful staging writes `build/source-receipt.json`. Preparation failure invalidates the old receipt. CMake verifies all inputs and staged outputs before compilation, and records the DLL digest after linking. Packaging verifies that receipt and binary. Build directories and receipts are not source artifacts.

## Seam ownership

| Area | Upstream keeps | Owned adapter |
| --- | --- | --- |
| Config | Localify fields, load/save field list | `src/host/localify/ConfigIntegration.hpp`; VR fields/migration/save in `VrifyConfig` |
| DLL startup | Program config parser and globals | `VrBootstrap`: loader-lock-free worker, paths, gates and coordinated lifetime |
| Windows | Keyboard, window and Localify reload business | `WindowsIntegration.hpp`: native hook owner, process window, quit and asset loader |
| Desktop text | System language and lookup | `DesktopText.hpp`: single-key camera notice, selected-language common words |
| Unity | Localify detours, translations and desktop camera | Named callbacks and registration from `src/vr/unity/*.inc.cpp` |

Unity fragments are included at declared points in the *upstream translation unit*. They contain VR implementations, not extracted upstream business functions. Sharing a translation unit permits the existing HookInstaller to register a single detour per shared game entry, with the original pointer still pointing at the game trampoline. There is no dynamic hook chain.

`BeforeCameraState` runs before the original; `AfterCameraState` runs after it. `AfterEndCamera` runs after the original and reports VR render ownership before desktop processing. `SampleVrActor` samples before the original; the private actor scope observes before and applies the accepted gaze processing on exit. VR owns its actor selection, pose samples and mode. Desktop camera state is not used as a VR state mirror. The upstream enum discovery is reused as immutable bone metadata. Material-pass head visibility, live-object checks and GC handles are owned by the integration and also serve the desktop first-person path.

The public and private manifest may differ in Unity seams because their feature baselines differ. The pipeline and patch rules are identical. Private gaze and costume features must not be transferred to public as part of this refactor.

## Regression gates

`tests/test-source-pipeline.ps1` injects bad pins, upstream changes, missing or corrupt patches, context/preimage/postimage errors, stale receipts and binaries. `tests/test-shared-entry-contracts.ps1` compiles detour bodies from the actual staged source against observable game/seam doubles. Those temporary test fragments are never production inputs. Existing config/UI behavior harnesses compile staged upstream config, internationalization and ImGui with adapters.

`scripts/check-source-ownership.py` checks the candidate Git tree (existing tracked files plus nonignored additions), forbids public upstream paths and retired copies, and flags long matching upstream bodies regardless of local function names. Fingerprinting is a review aid, not a semantic proof. Hardware acceptance remains separate from all these offline checks.

## Updating the pin

Review upstream changes against every manifest target. Rebase each reviewed patch onto the new raw source and regenerate all intermediate input/output hashes explicitly. Do not loosen checks or automatically refresh hashes after a failure. Run both repositories' builds, gates and functional checks before packaging a hardware probe. Never edit `build/source` as a maintained source.
