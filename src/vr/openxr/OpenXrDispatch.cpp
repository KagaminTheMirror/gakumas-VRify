#include "OpenXrDispatch.hpp"

#include "../VrLog.hpp"

#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace gakumas::vr::openxr {
namespace {

std::string WindowsErrorMessage(DWORD error) {
    char* buffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<char*>(&buffer),
        0,
        nullptr);

    std::string message;
    if (length != 0 && buffer != nullptr) {
        message.assign(buffer, length);
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
            message.pop_back();
        }
    } else {
        message = "Win32 error " + std::to_string(error);
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}

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

void LogActiveRuntimeManifest(VrLog& log) {
    constexpr wchar_t kOpenXrRegistryKey[] = L"SOFTWARE\\Khronos\\OpenXR\\1";
    constexpr wchar_t kActiveRuntimeValue[] = L"ActiveRuntime";

    DWORD byteCount = 0;
    const DWORD flags = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY;
    LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        kOpenXrRegistryKey,
        kActiveRuntimeValue,
        flags,
        nullptr,
        nullptr,
        &byteCount);
    if (status != ERROR_SUCCESS || byteCount < sizeof(wchar_t)) {
        log.Write(
            "[VR][runtime] active OpenXR runtime manifest unavailable (Win32=" +
            std::to_string(status) + ")");
        return;
    }

    std::vector<wchar_t> manifest(byteCount / sizeof(wchar_t), L'\0');
    status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        kOpenXrRegistryKey,
        kActiveRuntimeValue,
        flags,
        nullptr,
        manifest.data(),
        &byteCount);
    if (status != ERROR_SUCCESS) {
        log.Write(
            "[VR][runtime] active OpenXR runtime manifest read failed (Win32=" +
            std::to_string(status) + ")");
        return;
    }

    const std::wstring_view value(manifest.data());
    log.Write("[VR][runtime] active runtime manifest=" + WideToUtf8(value));
}

} // namespace

OpenXrDispatch::~OpenXrDispatch() {
    Unload();
}

OpenXrDispatch::LoaderResult OpenXrDispatch::LoadAppLocal(
    const std::filesystem::path& applicationDirectory,
    VrLog& log) {
    if (loader_ != nullptr) {
        return LoaderResult::AlreadyLoaded;
    }

    LogActiveRuntimeManifest(log);

    std::error_code error;
    const std::filesystem::path loaderPath =
        std::filesystem::absolute(applicationDirectory / L"openxr_loader.dll", error);
    if (error || !std::filesystem::is_regular_file(loaderPath, error) || error) {
        log.Write("[VR][runtime] app-local openxr_loader.dll is missing");
        return LoaderResult::Missing;
    }

    loader_ = LoadLibraryExW(
        loaderPath.c_str(),
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (loader_ == nullptr) {
        log.Write("[VR][runtime] LoadLibraryExW(openxr_loader.dll) failed: " + WindowsErrorMessage(GetLastError()));
        return LoaderResult::LoadFailed;
    }

    getInstanceProcAddr_ = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        GetProcAddress(loader_, "xrGetInstanceProcAddr"));
    if (getInstanceProcAddr_ == nullptr ||
        !LoadGlobal("xrEnumerateInstanceExtensionProperties", enumerateInstanceExtensionProperties_, log) ||
        !LoadGlobal("xrEnumerateApiLayerProperties", enumerateApiLayerProperties_, log) ||
        !LoadGlobal("xrCreateInstance", createInstance_, log)) {
        log.Write("[VR][runtime] app-local DLL is not a usable OpenXR loader");
        Unload();
        return LoaderResult::InvalidLoader;
    }

    log.Write("[VR][runtime] app-local OpenXR loader loaded");
    return LoaderResult::Loaded;
}

template <typename Function>
bool OpenXrDispatch::LoadGlobal(const char* name, Function& destination, VrLog& log) {
    destination = nullptr;
    PFN_xrVoidFunction function = nullptr;
    const XrResult result = getInstanceProcAddr_(XR_NULL_HANDLE, name, &function);
    if (XR_FAILED(result) || function == nullptr) {
        log.Write(std::string("[VR][runtime] missing global OpenXR function: ") + name);
        return false;
    }
    destination = reinterpret_cast<Function>(function);
    return true;
}

template <typename Function>
bool OpenXrDispatch::LoadInstance(
    XrInstance instance,
    const char* name,
    Function& destination,
    VrLog& log) {
    destination = nullptr;
    PFN_xrVoidFunction function = nullptr;
    const XrResult result = getInstanceProcAddr_(instance, name, &function);
    if (XR_FAILED(result) || function == nullptr) {
        log.Write(std::string("[VR][runtime] missing instance OpenXR function: ") + name);
        return false;
    }
    destination = reinterpret_cast<Function>(function);
    return true;
}

bool OpenXrDispatch::LoadInstanceFunctions(XrInstance instance, VrLog& log) {
    ClearInstanceFunctions();
    return LoadInstance(instance, "xrDestroyInstance", destroyInstance_, log) &&
           LoadInstance(instance, "xrGetInstanceProperties", getInstanceProperties_, log) &&
           LoadInstance(instance, "xrPollEvent", pollEvent_, log) &&
           LoadInstance(instance, "xrResultToString", resultToString_, log) &&
           LoadInstance(instance, "xrGetSystem", getSystem_, log) &&
           LoadInstance(instance, "xrGetSystemProperties", getSystemProperties_, log) &&
           LoadInstance(
               instance,
               "xrEnumerateEnvironmentBlendModes",
               enumerateEnvironmentBlendModes_,
               log) &&
           LoadInstance(
               instance,
               "xrGetD3D11GraphicsRequirementsKHR",
               getD3D11GraphicsRequirements_,
               log) &&
           LoadInstance(instance, "xrCreateSession", createSession_, log) &&
           LoadInstance(instance, "xrDestroySession", destroySession_, log) &&
           LoadInstance(instance, "xrEnumerateReferenceSpaces", enumerateReferenceSpaces_, log) &&
           LoadInstance(instance, "xrCreateReferenceSpace", createReferenceSpace_, log) &&
           LoadInstance(instance, "xrCreateActionSpace", createActionSpace_, log) &&
           LoadInstance(instance, "xrLocateSpace", locateSpace_, log) &&
           LoadInstance(instance, "xrDestroySpace", destroySpace_, log) &&
           LoadInstance(instance, "xrEnumerateViewConfigurations", enumerateViewConfigurations_, log) &&
           LoadInstance(
               instance,
               "xrEnumerateViewConfigurationViews",
               enumerateViewConfigurationViews_,
               log) &&
           LoadInstance(instance, "xrEnumerateSwapchainFormats", enumerateSwapchainFormats_, log) &&
           LoadInstance(instance, "xrCreateSwapchain", createSwapchain_, log) &&
           LoadInstance(instance, "xrDestroySwapchain", destroySwapchain_, log) &&
           LoadInstance(instance, "xrEnumerateSwapchainImages", enumerateSwapchainImages_, log) &&
           LoadInstance(instance, "xrAcquireSwapchainImage", acquireSwapchainImage_, log) &&
           LoadInstance(instance, "xrWaitSwapchainImage", waitSwapchainImage_, log) &&
           LoadInstance(instance, "xrReleaseSwapchainImage", releaseSwapchainImage_, log) &&
           LoadInstance(instance, "xrBeginSession", beginSession_, log) &&
           LoadInstance(instance, "xrEndSession", endSession_, log) &&
           LoadInstance(instance, "xrRequestExitSession", requestExitSession_, log) &&
           LoadInstance(instance, "xrWaitFrame", waitFrame_, log) &&
           LoadInstance(instance, "xrBeginFrame", beginFrame_, log) &&
           LoadInstance(instance, "xrEndFrame", endFrame_, log) &&
           LoadInstance(instance, "xrLocateViews", locateViews_, log) &&
           LoadInstance(instance, "xrStringToPath", stringToPath_, log) &&
           LoadInstance(instance, "xrCreateActionSet", createActionSet_, log) &&
           LoadInstance(instance, "xrDestroyActionSet", destroyActionSet_, log) &&
           LoadInstance(instance, "xrCreateAction", createAction_, log) &&
           LoadInstance(instance, "xrDestroyAction", destroyAction_, log) &&
           LoadInstance(
               instance,
               "xrSuggestInteractionProfileBindings",
               suggestInteractionProfileBindings_,
               log) &&
           LoadInstance(instance, "xrAttachSessionActionSets", attachSessionActionSets_, log) &&
           LoadInstance(instance, "xrGetActionStateBoolean", getActionStateBoolean_, log) &&
           LoadInstance(instance, "xrGetActionStateFloat", getActionStateFloat_, log) &&
           LoadInstance(instance, "xrGetActionStateVector2f", getActionStateVector2f_, log) &&
           LoadInstance(instance, "xrGetActionStatePose", getActionStatePose_, log) &&
           LoadInstance(instance, "xrSyncActions", syncActions_, log);
}

void OpenXrDispatch::ClearInstanceFunctions() noexcept {
    destroyInstance_ = nullptr;
    getInstanceProperties_ = nullptr;
    pollEvent_ = nullptr;
    resultToString_ = nullptr;
    getSystem_ = nullptr;
    getSystemProperties_ = nullptr;
    enumerateEnvironmentBlendModes_ = nullptr;
    getD3D11GraphicsRequirements_ = nullptr;
    createSession_ = nullptr;
    destroySession_ = nullptr;
    enumerateReferenceSpaces_ = nullptr;
    createReferenceSpace_ = nullptr;
    createActionSpace_ = nullptr;
    locateSpace_ = nullptr;
    destroySpace_ = nullptr;
    enumerateViewConfigurations_ = nullptr;
    enumerateViewConfigurationViews_ = nullptr;
    enumerateSwapchainFormats_ = nullptr;
    createSwapchain_ = nullptr;
    destroySwapchain_ = nullptr;
    enumerateSwapchainImages_ = nullptr;
    acquireSwapchainImage_ = nullptr;
    waitSwapchainImage_ = nullptr;
    releaseSwapchainImage_ = nullptr;
    beginSession_ = nullptr;
    endSession_ = nullptr;
    requestExitSession_ = nullptr;
    waitFrame_ = nullptr;
    beginFrame_ = nullptr;
    endFrame_ = nullptr;
    locateViews_ = nullptr;
    stringToPath_ = nullptr;
    createActionSet_ = nullptr;
    destroyActionSet_ = nullptr;
    createAction_ = nullptr;
    destroyAction_ = nullptr;
    suggestInteractionProfileBindings_ = nullptr;
    attachSessionActionSets_ = nullptr;
    getActionStateBoolean_ = nullptr;
    getActionStateFloat_ = nullptr;
    getActionStateVector2f_ = nullptr;
    getActionStatePose_ = nullptr;
    syncActions_ = nullptr;
}

void OpenXrDispatch::Unload() noexcept {
    ClearInstanceFunctions();
    enumerateInstanceExtensionProperties_ = nullptr;
    enumerateApiLayerProperties_ = nullptr;
    createInstance_ = nullptr;
    getInstanceProcAddr_ = nullptr;
    if (loader_ != nullptr) {
        FreeLibrary(loader_);
        loader_ = nullptr;
    }
}

std::string OpenXrDispatch::ResultText(XrInstance instance, XrResult result) const {
    char resultName[XR_MAX_RESULT_STRING_SIZE]{};
    if (instance != XR_NULL_HANDLE && resultToString_ != nullptr &&
        XR_SUCCEEDED(resultToString_(instance, result, resultName))) {
        return resultName;
    }
    return std::to_string(static_cast<int>(result));
}

PFN_xrEnumerateInstanceExtensionProperties OpenXrDispatch::EnumerateInstanceExtensionProperties() const noexcept {
    return enumerateInstanceExtensionProperties_;
}

PFN_xrEnumerateApiLayerProperties OpenXrDispatch::EnumerateApiLayerProperties() const noexcept {
    return enumerateApiLayerProperties_;
}

PFN_xrCreateInstance OpenXrDispatch::CreateInstance() const noexcept {
    return createInstance_;
}

PFN_xrDestroyInstance OpenXrDispatch::DestroyInstance() const noexcept {
    return destroyInstance_;
}

PFN_xrGetInstanceProperties OpenXrDispatch::GetInstanceProperties() const noexcept {
    return getInstanceProperties_;
}

PFN_xrPollEvent OpenXrDispatch::PollEvent() const noexcept {
    return pollEvent_;
}

PFN_xrGetSystem OpenXrDispatch::GetSystem() const noexcept {
    return getSystem_;
}

PFN_xrGetSystemProperties OpenXrDispatch::GetSystemProperties() const noexcept {
    return getSystemProperties_;
}

PFN_xrEnumerateEnvironmentBlendModes OpenXrDispatch::EnumerateEnvironmentBlendModes() const noexcept {
    return enumerateEnvironmentBlendModes_;
}

PFN_xrGetD3D11GraphicsRequirementsKHR OpenXrDispatch::GetD3D11GraphicsRequirements() const noexcept {
    return getD3D11GraphicsRequirements_;
}

PFN_xrCreateSession OpenXrDispatch::CreateSession() const noexcept {
    return createSession_;
}

PFN_xrDestroySession OpenXrDispatch::DestroySession() const noexcept {
    return destroySession_;
}

PFN_xrEnumerateReferenceSpaces OpenXrDispatch::EnumerateReferenceSpaces() const noexcept {
    return enumerateReferenceSpaces_;
}

PFN_xrCreateReferenceSpace OpenXrDispatch::CreateReferenceSpace() const noexcept {
    return createReferenceSpace_;
}

PFN_xrCreateActionSpace OpenXrDispatch::CreateActionSpace() const noexcept {
    return createActionSpace_;
}

PFN_xrLocateSpace OpenXrDispatch::LocateSpace() const noexcept {
    return locateSpace_;
}

PFN_xrDestroySpace OpenXrDispatch::DestroySpace() const noexcept {
    return destroySpace_;
}

PFN_xrEnumerateViewConfigurations OpenXrDispatch::EnumerateViewConfigurations() const noexcept {
    return enumerateViewConfigurations_;
}

PFN_xrEnumerateViewConfigurationViews OpenXrDispatch::EnumerateViewConfigurationViews() const noexcept {
    return enumerateViewConfigurationViews_;
}

PFN_xrEnumerateSwapchainFormats OpenXrDispatch::EnumerateSwapchainFormats() const noexcept {
    return enumerateSwapchainFormats_;
}

PFN_xrCreateSwapchain OpenXrDispatch::CreateSwapchain() const noexcept {
    return createSwapchain_;
}

PFN_xrDestroySwapchain OpenXrDispatch::DestroySwapchain() const noexcept {
    return destroySwapchain_;
}

PFN_xrEnumerateSwapchainImages OpenXrDispatch::EnumerateSwapchainImages() const noexcept {
    return enumerateSwapchainImages_;
}

PFN_xrAcquireSwapchainImage OpenXrDispatch::AcquireSwapchainImage() const noexcept {
    return acquireSwapchainImage_;
}

PFN_xrWaitSwapchainImage OpenXrDispatch::WaitSwapchainImage() const noexcept {
    return waitSwapchainImage_;
}

PFN_xrReleaseSwapchainImage OpenXrDispatch::ReleaseSwapchainImage() const noexcept {
    return releaseSwapchainImage_;
}

PFN_xrBeginSession OpenXrDispatch::BeginSession() const noexcept {
    return beginSession_;
}

PFN_xrEndSession OpenXrDispatch::EndSession() const noexcept {
    return endSession_;
}

PFN_xrRequestExitSession OpenXrDispatch::RequestExitSession() const noexcept {
    return requestExitSession_;
}

PFN_xrWaitFrame OpenXrDispatch::WaitFrame() const noexcept {
    return waitFrame_;
}

PFN_xrBeginFrame OpenXrDispatch::BeginFrame() const noexcept {
    return beginFrame_;
}

PFN_xrEndFrame OpenXrDispatch::EndFrame() const noexcept {
    return endFrame_;
}

PFN_xrLocateViews OpenXrDispatch::LocateViews() const noexcept {
    return locateViews_;
}

PFN_xrStringToPath OpenXrDispatch::StringToPath() const noexcept {
    return stringToPath_;
}

PFN_xrCreateActionSet OpenXrDispatch::CreateActionSet() const noexcept {
    return createActionSet_;
}

PFN_xrDestroyActionSet OpenXrDispatch::DestroyActionSet() const noexcept {
    return destroyActionSet_;
}

PFN_xrCreateAction OpenXrDispatch::CreateAction() const noexcept {
    return createAction_;
}

PFN_xrDestroyAction OpenXrDispatch::DestroyAction() const noexcept {
    return destroyAction_;
}

PFN_xrSuggestInteractionProfileBindings OpenXrDispatch::SuggestInteractionProfileBindings() const noexcept {
    return suggestInteractionProfileBindings_;
}

PFN_xrAttachSessionActionSets OpenXrDispatch::AttachSessionActionSets() const noexcept {
    return attachSessionActionSets_;
}

PFN_xrGetActionStateBoolean OpenXrDispatch::GetActionStateBoolean() const noexcept {
    return getActionStateBoolean_;
}

PFN_xrGetActionStateFloat OpenXrDispatch::GetActionStateFloat() const noexcept {
    return getActionStateFloat_;
}

PFN_xrGetActionStateVector2f OpenXrDispatch::GetActionStateVector2f() const noexcept {
    return getActionStateVector2f_;
}

PFN_xrGetActionStatePose OpenXrDispatch::GetActionStatePose() const noexcept {
    return getActionStatePose_;
}

PFN_xrSyncActions OpenXrDispatch::SyncActions() const noexcept {
    return syncActions_;
}

} // namespace gakumas::vr::openxr
