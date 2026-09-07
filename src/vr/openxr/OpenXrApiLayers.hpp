#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gakumas::vr {
class VrLog;
}

namespace gakumas::vr::openxr {

class OpenXrDispatch;

struct ImplicitLayerManifest {
    std::string name;
    std::string disableEnvironment;
    std::string enableEnvironment;
};

struct ImplicitLayerInput {
    std::filesystem::path manifestPath;
    std::string hive;
    bool registryEnabled = false;
    bool manifestReadable = false;
    ImplicitLayerManifest manifest;
};

struct ApiLayerDecision {
    bool enable = false;
    std::string name;
    std::string reason;
    std::filesystem::path manifestPath;
};

struct ApiLayerSelection {
    std::vector<std::string> enabledNames;
    std::vector<ApiLayerDecision> decisions;
    std::vector<std::filesystem::path> searchDirectories;
};

// Pure selector: HKLM-then-HKCU implicit layers first, then XR_ENABLE_API_LAYERS.
// Implicit layers that are registry-enabled and env-allowed are forced even when
// xrEnumerateApiLayerProperties omitted them. XR_ENABLE_API_LAYERS still requires
// an enumerated name. Search directories are only the JSON parents of those
// unenumerated implicit layers. Elevated processes republish enabled HKCU
// implicit layers into HKLM before the loader loads, because Khronos skips
// HKCU and environment overrides above medium integrity.
ApiLayerSelection SelectEnabledApiLayers(
    const std::vector<std::string>& enumeratedNames,
    const std::vector<ImplicitLayerInput>& implicitLayers,
    std::string_view xrEnableApiLayers,
    const std::function<bool(std::string_view)>& environmentIsPresent,
    bool ignoreCurrentUserLayers = false);

bool ProcessIgnoresUserOpenXrOverrides();
bool CurrentUserImplicitLayerNeedsLocalMachinePublish(
    const ImplicitLayerInput& layer,
    const std::function<bool(std::string_view)>& environmentIsPresent);

bool EnvironmentVariableIsPresent(std::string_view name);
std::string ReadXrEnableApiLayersEnvironment();
std::vector<std::string> EnumerateLoaderApiLayerNames(OpenXrDispatch& dispatch, VrLog& log);
std::vector<ImplicitLayerInput> CollectImplicitApiLayers(VrLog& log, bool logEntries = true);
void LogApiLayerSelection(
    const std::vector<std::string>& enumeratedNames,
    const ApiLayerSelection& selection,
    VrLog& log);
void LogOpenXrLoaderSecurityContext(VrLog& log);
void ApplyImplicitApiLayerSearchPath(VrLog& log);
ApiLayerSelection ResolveEnabledApiLayerNames(OpenXrDispatch& dispatch, VrLog& log);
std::vector<std::string> EnabledApiLayerNamesWithoutUnenumeratedImplicit(
    const ApiLayerSelection& selection);

} // namespace gakumas::vr::openxr
