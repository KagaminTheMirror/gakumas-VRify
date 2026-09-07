#include "OpenXrApiLayers.hpp"

#include "OpenXrDispatch.hpp"
#include "../VrLog.hpp"

#include "../../deps/nlohmann/json.hpp"

#include <Windows.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace gakumas::vr::openxr {
namespace {

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return "<invalid UTF-16 path>";
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    return WideToUtf8(path.wstring());
}

std::string TrimCopy(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::vector<std::string> SplitLayerList(std::string_view value) {
    std::vector<std::string> names;
    std::string current;
    const auto flush = [&]() {
        const std::string trimmed = TrimCopy(current);
        current.clear();
        if (!trimmed.empty()) {
            names.push_back(trimmed);
        }
    };
    for (const char ch : value) {
        if (ch == ';' || ch == ':') {
            flush();
        } else {
            current.push_back(ch);
        }
    }
    flush();
    return names;
}

bool ReadManifest(const std::filesystem::path& path, ImplicitLayerManifest& manifest) {
    std::ifstream stream(path);
    if (!stream) {
        return false;
    }

    const auto document = nlohmann::json::parse(stream, nullptr, false, true);
    if (document.is_discarded() || !document.is_object()) {
        return false;
    }
    const auto layer = document.find("api_layer");
    if (layer == document.end() || !layer->is_object()) {
        return false;
    }

    manifest.name = layer->value("name", std::string());
    manifest.disableEnvironment = layer->value("disable_environment", std::string());
    manifest.enableEnvironment = layer->value("enable_environment", std::string());
    return !manifest.name.empty();
}

void CollectImplicitHive(
    HKEY root,
    const char* hiveName,
    VrLog& log,
    std::vector<ImplicitLayerInput>& layers,
    bool logEntries) {
    HKEY key = nullptr;
    const LSTATUS openStatus = RegOpenKeyExW(
        root,
        L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit",
        0,
        KEY_READ | KEY_WOW64_64KEY,
        &key);
    if (openStatus != ERROR_SUCCESS) {
        log.Write(
            std::string("[VR][runtime] API_LAYER_IMPLICIT hive=") + hiveName +
            " missing win32=" + std::to_string(openStatus));
        return;
    }

    DWORD valueCount = 0;
    DWORD maxNameBytes = 0;
    const LSTATUS infoStatus = RegQueryInfoKeyW(
        key,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &valueCount,
        &maxNameBytes,
        nullptr,
        nullptr,
        nullptr);
    if (infoStatus != ERROR_SUCCESS) {
        RegCloseKey(key);
        log.Write(
            std::string("[VR][runtime] API_LAYER_IMPLICIT hive=") + hiveName +
            " query-failed win32=" + std::to_string(infoStatus));
        return;
    }

    std::vector<wchar_t> name(static_cast<std::size_t>(maxNameBytes) + 2U, L'\0');
    for (DWORD index = 0; index < valueCount; ++index) {
        DWORD nameLength = static_cast<DWORD>(name.size());
        DWORD type = 0;
        DWORD enabledDword = 1;
        DWORD enabledSize = sizeof(enabledDword);
        const LSTATUS enumStatus = RegEnumValueW(
            key,
            index,
            name.data(),
            &nameLength,
            nullptr,
            &type,
            reinterpret_cast<LPBYTE>(&enabledDword),
            &enabledSize);
        if (enumStatus != ERROR_SUCCESS) {
            log.Write(
                std::string("[VR][runtime] API_LAYER_IMPLICIT hive=") + hiveName +
                " enum-failed index=" + std::to_string(index) +
                " win32=" + std::to_string(enumStatus));
            continue;
        }

        ImplicitLayerInput input;
        input.manifestPath = std::filesystem::path(std::wstring_view(name.data()));
        input.hive = hiveName;
        input.registryEnabled = type == REG_DWORD && enabledDword == 0;
        input.manifestReadable = ReadManifest(input.manifestPath, input.manifest);
        if (logEntries) {
            log.Write(
                std::string("[VR][runtime] API_LAYER_IMPLICIT hive=") + hiveName +
                " enabled=" + (input.registryEnabled ? "1" : "0") +
                " readable=" + (input.manifestReadable ? "1" : "0") +
                " name=" + input.manifest.name +
                " path=" + PathToUtf8(input.manifestPath));
        }
        layers.push_back(std::move(input));
    }
    RegCloseKey(key);
}

void AppendDecision(
    ApiLayerSelection& selection,
    bool enable,
    std::string name,
    std::string reason,
    std::filesystem::path manifestPath) {
    if (enable && !name.empty()) {
        selection.enabledNames.push_back(name);
    }
    ApiLayerDecision decision;
    decision.enable = enable;
    decision.name = std::move(name);
    decision.reason = std::move(reason);
    decision.manifestPath = std::move(manifestPath);
    selection.decisions.push_back(std::move(decision));
}

bool ImplicitLayerIsEnvironmentAllowed(
    const ImplicitLayerInput& layer,
    const std::function<bool(std::string_view)>& environmentIsPresent) {
    const auto envIsPresent = [&](std::string_view name) {
        return environmentIsPresent && environmentIsPresent(name);
    };
    if (!layer.manifest.disableEnvironment.empty() &&
        envIsPresent(layer.manifest.disableEnvironment)) {
        return false;
    }
    if (!layer.manifest.enableEnvironment.empty() &&
        !envIsPresent(layer.manifest.enableEnvironment)) {
        return false;
    }
    return true;
}

} // namespace

ApiLayerSelection SelectEnabledApiLayers(
    const std::vector<std::string>& enumeratedNames,
    const std::vector<ImplicitLayerInput>& implicitLayers,
    std::string_view xrEnableApiLayers,
    const std::function<bool(std::string_view)>& environmentIsPresent,
    bool ignoreCurrentUserLayers) {
    ApiLayerSelection selection;
    std::unordered_set<std::string> enumerated;
    enumerated.reserve(enumeratedNames.size());
    for (const auto& name : enumeratedNames) {
        if (!name.empty()) {
            enumerated.insert(name);
        }
    }
    std::unordered_set<std::string> enabled;
    const auto envIsPresent = [&](std::string_view name) {
        return environmentIsPresent && environmentIsPresent(name);
    };

    for (const auto& layer : implicitLayers) {
        if (!layer.registryEnabled) {
            AppendDecision(
                selection,
                false,
                layer.manifest.name,
                "registry-disabled",
                layer.manifestPath);
            continue;
        }
        if (!layer.manifestReadable || layer.manifest.name.empty()) {
            AppendDecision(
                selection,
                false,
                layer.manifest.name,
                "manifest-unreadable",
                layer.manifestPath);
            continue;
        }
        if (!layer.manifest.disableEnvironment.empty() &&
            envIsPresent(layer.manifest.disableEnvironment)) {
            AppendDecision(
                selection,
                false,
                layer.manifest.name,
                "disable-environment",
                layer.manifestPath);
            continue;
        }
        if (!layer.manifest.enableEnvironment.empty() &&
            !envIsPresent(layer.manifest.enableEnvironment)) {
            AppendDecision(
                selection,
                false,
                layer.manifest.name,
                "enable-environment-unset",
                layer.manifestPath);
            continue;
        }
        if (!enabled.insert(layer.manifest.name).second) {
            AppendDecision(
                selection,
                false,
                layer.manifest.name,
                "duplicate",
                layer.manifestPath);
            continue;
        }
        const bool enumeratedLayer =
            enumerated.find(layer.manifest.name) != enumerated.end();
        if (ignoreCurrentUserLayers && layer.hive == "HKCU" && !enumeratedLayer) {
            AppendDecision(
                selection,
                false,
                layer.manifest.name,
                "high-integrity-hkcu",
                layer.manifestPath);
            continue;
        }
        AppendDecision(
            selection,
            true,
            layer.manifest.name,
            enumeratedLayer ? "implicit" : "implicit-registry",
            layer.manifestPath);
        if (!enumeratedLayer && !layer.manifestPath.empty()) {
            selection.searchDirectories.push_back(layer.manifestPath.parent_path());
        }
    }

    for (const auto& name : SplitLayerList(xrEnableApiLayers)) {
        if (enabled.find(name) != enabled.end()) {
            AppendDecision(selection, false, name, "duplicate", {});
            continue;
        }
        if (enumerated.find(name) == enumerated.end()) {
            AppendDecision(selection, false, name, "not-enumerated", {});
            continue;
        }
        enabled.insert(name);
        AppendDecision(selection, true, name, "xr-enable-api-layers", {});
    }
    return selection;
}

bool EnvironmentVariableIsPresent(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const std::string terminated(name);
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableA(terminated.c_str(), nullptr, 0);
    if (length == 0) {
        return GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    }
    return true;
}

std::string ReadXrEnableApiLayersEnvironment() {
    const DWORD length = GetEnvironmentVariableA("XR_ENABLE_API_LAYERS", nullptr, 0);
    if (length == 0) {
        return {};
    }
    std::string value(static_cast<std::size_t>(length), '\0');
    const DWORD written =
        GetEnvironmentVariableA("XR_ENABLE_API_LAYERS", value.data(), length);
    if (written == 0) {
        return {};
    }
    if (written < value.size()) {
        value.resize(written);
    } else if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

std::vector<std::string> EnumerateLoaderApiLayerNames(OpenXrDispatch& dispatch, VrLog& log) {
    const auto enumerate = dispatch.EnumerateApiLayerProperties();
    if (enumerate == nullptr) {
        log.Write("[VR][runtime] API_LAYER_ENUM failed missing-xrEnumerateApiLayerProperties");
        return {};
    }

    uint32_t count = 0;
    XrResult result = enumerate(0, &count, nullptr);
    if (XR_FAILED(result)) {
        log.Write(
            "[VR][runtime] API_LAYER_ENUM failed result=" +
            std::to_string(static_cast<int>(result)));
        return {};
    }

    std::vector<XrApiLayerProperties> layers;
    for (int attempt = 0; attempt < 3; ++attempt) {
        layers.assign(count, XrApiLayerProperties{XR_TYPE_API_LAYER_PROPERTIES});
        uint32_t written = count;
        result = enumerate(static_cast<uint32_t>(layers.size()), &written, layers.data());
        if (result == XR_ERROR_SIZE_INSUFFICIENT) {
            count = written;
            continue;
        }
        if (XR_FAILED(result)) {
            log.Write(
                "[VR][runtime] API_LAYER_ENUM failed result=" +
                std::to_string(static_cast<int>(result)));
            return {};
        }
        if (written < layers.size()) {
            layers.resize(written);
        }
        break;
    }
    if (result == XR_ERROR_SIZE_INSUFFICIENT) {
        log.Write("[VR][runtime] API_LAYER_ENUM failed size-changed");
        return {};
    }

    std::vector<std::string> names;
    names.reserve(layers.size());
    for (const auto& layer : layers) {
        if (layer.layerName[0] != '\0') {
            names.emplace_back(layer.layerName);
        }
    }
    return names;
}

std::vector<ImplicitLayerInput> CollectImplicitApiLayers(VrLog& log, bool logEntries) {
    std::vector<ImplicitLayerInput> layers;
    CollectImplicitHive(HKEY_LOCAL_MACHINE, "HKLM", log, layers, logEntries);
    CollectImplicitHive(HKEY_CURRENT_USER, "HKCU", log, layers, logEntries);
    return layers;
}

bool ProcessIgnoresUserOpenXrOverrides() {
    static const bool highIntegrity = []() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(
                GetCurrentProcess(),
                TOKEN_QUERY | TOKEN_QUERY_SOURCE,
                &token)) {
            return false;
        }

        std::uint8_t buffer[SECURITY_MAX_SID_SIZE + sizeof(DWORD)]{};
        DWORD size = 0;
        if (GetTokenInformation(
                token,
                TokenIntegrityLevel,
                buffer,
                sizeof(buffer),
                &size) == 0) {
            CloseHandle(token);
            return false;
        }
        CloseHandle(token);

        const auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
        if (label == nullptr || label->Label.Sid == nullptr) {
            return false;
        }
        const DWORD subAuthorityCount = *GetSidSubAuthorityCount(label->Label.Sid);
        const DWORD rid = *GetSidSubAuthority(label->Label.Sid, subAuthorityCount - 1U);
        return rid > SECURITY_MANDATORY_MEDIUM_RID;
    }();
    return highIntegrity;
}

bool CurrentUserImplicitLayerNeedsLocalMachinePublish(
    const ImplicitLayerInput& layer,
    const std::function<bool(std::string_view)>& environmentIsPresent) {
    return layer.hive == "HKCU" &&
           layer.registryEnabled &&
           layer.manifestReadable &&
           !layer.manifest.name.empty() &&
           !layer.manifestPath.empty() &&
           ImplicitLayerIsEnvironmentAllowed(layer, environmentIsPresent);
}

void PublishCurrentUserImplicitLayersToLocalMachine(
    const std::vector<ImplicitLayerInput>& implicitLayers,
    VrLog& log) {
    HKEY key = nullptr;
    const LSTATUS openStatus = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit",
        0,
        nullptr,
        0,
        KEY_SET_VALUE | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
        nullptr,
        &key,
        nullptr);
    if (openStatus != ERROR_SUCCESS) {
        log.Write(
            "[VR][runtime] API_LAYER_HKLM_PUBLISH failed win32=" +
            std::to_string(openStatus));
        return;
    }

    for (const auto& layer : implicitLayers) {
        if (!CurrentUserImplicitLayerNeedsLocalMachinePublish(
                layer,
                EnvironmentVariableIsPresent)) {
            continue;
        }

        const std::wstring valueName = layer.manifestPath.wstring();
        DWORD current = 1;
        DWORD currentSize = sizeof(current);
        DWORD type = 0;
        const LSTATUS queryStatus = RegQueryValueExW(
            key,
            valueName.c_str(),
            nullptr,
            &type,
            reinterpret_cast<LPBYTE>(&current),
            &currentSize);
        if (queryStatus == ERROR_SUCCESS && type == REG_DWORD && current == 0) {
            log.Write(
                "[VR][runtime] API_LAYER_HKLM_PUBLISH name=" + layer.manifest.name +
                " path=" + PathToUtf8(layer.manifestPath) + " result=already");
            continue;
        }

        DWORD enabled = 0;
        const LSTATUS writeStatus = RegSetValueExW(
            key,
            valueName.c_str(),
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&enabled),
            sizeof(enabled));
        if (writeStatus != ERROR_SUCCESS) {
            log.Write(
                "[VR][runtime] API_LAYER_HKLM_PUBLISH name=" + layer.manifest.name +
                " path=" + PathToUtf8(layer.manifestPath) +
                " result=failed win32=" + std::to_string(writeStatus));
            continue;
        }
        log.Write(
            "[VR][runtime] API_LAYER_HKLM_PUBLISH name=" + layer.manifest.name +
            " path=" + PathToUtf8(layer.manifestPath) + " result=wrote");
    }
    RegCloseKey(key);
}

void LogOpenXrLoaderSecurityContext(VrLog& log) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_QUERY_SOURCE, &token)) {
        log.Write(
            "[VR][runtime] API_LAYER_INTEGRITY query-failed win32=" +
            std::to_string(GetLastError()));
        return;
    }

    std::uint8_t buffer[SECURITY_MAX_SID_SIZE + sizeof(DWORD)]{};
    DWORD size = 0;
    if (GetTokenInformation(
            token,
            TokenIntegrityLevel,
            buffer,
            sizeof(buffer),
            &size) == 0) {
        log.Write(
            "[VR][runtime] API_LAYER_INTEGRITY query-failed win32=" +
            std::to_string(GetLastError()));
        CloseHandle(token);
        return;
    }
    CloseHandle(token);

    const auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
    if (label == nullptr || label->Label.Sid == nullptr) {
        log.Write("[VR][runtime] API_LAYER_INTEGRITY query-failed missing-sid");
        return;
    }
    const DWORD subAuthorityCount = *GetSidSubAuthorityCount(label->Label.Sid);
    const DWORD rid = *GetSidSubAuthority(label->Label.Sid, subAuthorityCount - 1U);
    const bool high = rid > SECURITY_MANDATORY_MEDIUM_RID;
    std::string line =
        std::string("[VR][runtime] API_LAYER_INTEGRITY high=") + (high ? "1" : "0") +
        " rid=" + std::to_string(rid);
    if (high) {
        line += " loader-skips-hkcu-and-env=1";
    }
    log.Write(line);
}

void ApplyApiLayerSearchDirectories(
    const std::vector<std::filesystem::path>& directories,
    VrLog& log);

void ApplyImplicitApiLayerSearchPath(VrLog& log) {
    LogOpenXrLoaderSecurityContext(log);
    const bool ignoreCurrentUser = ProcessIgnoresUserOpenXrOverrides();
    const auto implicit = CollectImplicitApiLayers(log, true);
    std::vector<std::string> hkLmNames;
    for (const auto& layer : implicit) {
        if (layer.hive == "HKLM" && !layer.manifest.name.empty()) {
            hkLmNames.push_back(layer.manifest.name);
        }
    }
    const auto preview = SelectEnabledApiLayers(
        hkLmNames,
        implicit,
        ReadXrEnableApiLayersEnvironment(),
        EnvironmentVariableIsPresent,
        ignoreCurrentUser);
    if (ignoreCurrentUser) {
        PublishCurrentUserImplicitLayersToLocalMachine(implicit, log);
        log.Write("[VR][runtime] API_LAYER_PATH skipped high-integrity");
        return;
    }
    ApplyApiLayerSearchDirectories(preview.searchDirectories, log);
}

void LogApiLayerSelection(
    const std::vector<std::string>& enumeratedNames,
    const ApiLayerSelection& selection,
    VrLog& log) {
    log.Write(
        "[VR][runtime] API_LAYER_ENUM count=" + std::to_string(enumeratedNames.size()));
    for (const auto& name : enumeratedNames) {
        log.Write("[VR][runtime] API_LAYER_ENUM name=" + name);
    }
    for (const auto& decision : selection.decisions) {
        const char* token = decision.enable ? "API_LAYER_ENABLE" : "API_LAYER_SKIP";
        log.Write(
            std::string("[VR][runtime] ") + token + " name=" + decision.name +
            " reason=" + decision.reason +
            (decision.manifestPath.empty()
                 ? std::string()
                 : " path=" + PathToUtf8(decision.manifestPath)));
    }
    std::ostringstream enabled;
    enabled << "[VR][runtime] API_LAYER_CREATE count=" << selection.enabledNames.size();
    if (!selection.enabledNames.empty()) {
        enabled << " names=";
        for (std::size_t index = 0; index < selection.enabledNames.size(); ++index) {
            if (index != 0) {
                enabled << ',';
            }
            enabled << selection.enabledNames[index];
        }
    }
    log.Write(enabled.str());
}

void ApplyApiLayerSearchDirectories(
    const std::vector<std::filesystem::path>& directories,
    VrLog& log) {
    if (directories.empty()) {
        return;
    }

    DWORD existingLength = GetEnvironmentVariableW(L"XR_API_LAYER_PATH", nullptr, 0);
    std::wstring merged;
    if (existingLength > 1) {
        merged.assign(static_cast<std::size_t>(existingLength), L'\0');
        const DWORD written =
            GetEnvironmentVariableW(L"XR_API_LAYER_PATH", merged.data(), existingLength);
        if (written > 0 && written < merged.size()) {
            merged.resize(written);
        } else if (!merged.empty() && merged.back() == L'\0') {
            merged.pop_back();
        }
    }

    std::unordered_set<std::wstring> seen;
    const auto remember = [&](std::wstring_view value) {
        if (value.empty()) {
            return;
        }
        seen.insert(std::wstring(value));
    };
    std::wstring current;
    for (const wchar_t ch : merged) {
        if (ch == L';') {
            remember(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    remember(current);

    for (const auto& directory : directories) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(directory, error);
        if (error || absolute.empty()) {
            continue;
        }
        const std::wstring path = absolute.wstring();
        if (!seen.insert(path).second) {
            continue;
        }
        if (!merged.empty() && merged.back() != L';') {
            merged.push_back(L';');
        }
        merged += path;
    }

    if (!SetEnvironmentVariableW(L"XR_API_LAYER_PATH", merged.c_str())) {
        log.Write(
            "[VR][runtime] API_LAYER_PATH failed win32=" +
            std::to_string(GetLastError()));
        return;
    }
    log.Write("[VR][runtime] API_LAYER_PATH " + WideToUtf8(merged));
}

ApiLayerSelection ResolveEnabledApiLayerNames(OpenXrDispatch& dispatch, VrLog& log) {
    const auto enumerated = EnumerateLoaderApiLayerNames(dispatch, log);
    const auto selection = SelectEnabledApiLayers(
        enumerated,
        CollectImplicitApiLayers(log, false),
        ReadXrEnableApiLayersEnvironment(),
        EnvironmentVariableIsPresent,
        ProcessIgnoresUserOpenXrOverrides());
    LogApiLayerSelection(enumerated, selection, log);
    return selection;
}

std::vector<std::string> EnabledApiLayerNamesWithoutUnenumeratedImplicit(
    const ApiLayerSelection& selection) {
    std::unordered_set<std::string> forcedUnenumerated;
    for (const auto& decision : selection.decisions) {
        if (decision.enable && decision.reason == "implicit-registry") {
            forcedUnenumerated.insert(decision.name);
        }
    }

    std::vector<std::string> names;
    names.reserve(selection.enabledNames.size());
    for (const auto& name : selection.enabledNames) {
        if (forcedUnenumerated.find(name) == forcedUnenumerated.end()) {
            names.push_back(name);
        }
    }
    return names;
}

} // namespace gakumas::vr::openxr
