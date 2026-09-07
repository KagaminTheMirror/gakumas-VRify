# Third-party notices

gakumas-VRify builds on Localify and includes adapted integration code under [GPL-3.0](LICENSE). Preserve upstream copyright and license notices in source and binary distributions.

| Component | Source and notices |
| --- | --- |
| Localify | Pinned by `upstream.lock.json`; fetched license: `.upstream/localify/LICENSE` |
| OpenXR header subset | [Provenance](deps/openxr/README.md), [Apache-2.0](deps/openxr/LICENSE-APACHE-2.0.txt) or [MIT](deps/openxr/LICENSE-MIT.txt) |
| OpenXR loader | Official Khronos 1.1.61 binary fetched by `package-vr.ps1`; its supplied license is available in `.tools/openxr-loader-1.1.61/share/doc/openxr/LICENSE` after packaging |
| SMAA | [License and copyright notices](src/vr/d3d11/smaa/LICENSE.txt) |
| CMAA2 | [License and copyright notices](src/vr/d3d11/cmaa2/LICENSE.txt) |
| Dear ImGui | Fetched upstream `src/imgui/LICENSE.txt` |
| MinHook | Fetched upstream `deps/minhook/LICENSE.txt` |
| RapidJSON | Fetched upstream `deps/rapidjson/license.txt`, including its component notices |
| UnityResolve | Fetched upstream `src/deps/UnityResolve/LICENSE` |
| Conan dependencies | Versions/revisions pinned in `conan-release.lock`; package notices are in `.tools/conan2/p/*/p/licenses/` after dependency installation |
| Quest controller illustrations | [Supplied Oculus usage terms](docs/controller-guide/source/Oculus%20lineart%20attribution.txt); the exported guides refer to the corresponding controllers |

Third-party materials retain their own terms. The project's GPL license does not replace the controller illustration terms or other separately licensed components. No game assets are distributed here.
