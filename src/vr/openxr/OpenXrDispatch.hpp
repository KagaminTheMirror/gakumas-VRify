#pragma once

#include "OpenXrApi.hpp"

#include <filesystem>
#include <string>

namespace gakumas::vr {
class VrLog;
}

namespace gakumas::vr::openxr {

class OpenXrDispatch final {
public:
    enum class LoaderResult {
        Loaded,
        AlreadyLoaded,
        Missing,
        LoadFailed,
        InvalidLoader,
    };

    OpenXrDispatch() = default;
    ~OpenXrDispatch();

    OpenXrDispatch(const OpenXrDispatch&) = delete;
    OpenXrDispatch& operator=(const OpenXrDispatch&) = delete;

    LoaderResult LoadAppLocal(const std::filesystem::path& applicationDirectory, VrLog& log);
    void Unload() noexcept;

    [[nodiscard]] bool LoadInstanceFunctions(XrInstance instance, VrLog& log);
    void ClearInstanceFunctions() noexcept;
    [[nodiscard]] std::string ResultText(XrInstance instance, XrResult result) const;

    [[nodiscard]] PFN_xrEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties() const noexcept;
    [[nodiscard]] PFN_xrEnumerateApiLayerProperties EnumerateApiLayerProperties() const noexcept;
    [[nodiscard]] PFN_xrCreateInstance CreateInstance() const noexcept;
    [[nodiscard]] PFN_xrDestroyInstance DestroyInstance() const noexcept;
    [[nodiscard]] PFN_xrGetInstanceProperties GetInstanceProperties() const noexcept;
    [[nodiscard]] PFN_xrPollEvent PollEvent() const noexcept;
    [[nodiscard]] PFN_xrGetSystem GetSystem() const noexcept;
    [[nodiscard]] PFN_xrGetSystemProperties GetSystemProperties() const noexcept;
    [[nodiscard]] PFN_xrEnumerateEnvironmentBlendModes EnumerateEnvironmentBlendModes() const noexcept;
    [[nodiscard]] PFN_xrGetD3D11GraphicsRequirementsKHR GetD3D11GraphicsRequirements() const noexcept;
    [[nodiscard]] PFN_xrCreateSession CreateSession() const noexcept;
    [[nodiscard]] PFN_xrDestroySession DestroySession() const noexcept;
    [[nodiscard]] PFN_xrEnumerateReferenceSpaces EnumerateReferenceSpaces() const noexcept;
    [[nodiscard]] PFN_xrCreateReferenceSpace CreateReferenceSpace() const noexcept;
    [[nodiscard]] PFN_xrCreateActionSpace CreateActionSpace() const noexcept;
    [[nodiscard]] PFN_xrLocateSpace LocateSpace() const noexcept;
    [[nodiscard]] PFN_xrDestroySpace DestroySpace() const noexcept;
    [[nodiscard]] PFN_xrEnumerateViewConfigurations EnumerateViewConfigurations() const noexcept;
    [[nodiscard]] PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews() const noexcept;
    [[nodiscard]] PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats() const noexcept;
    [[nodiscard]] PFN_xrCreateSwapchain CreateSwapchain() const noexcept;
    [[nodiscard]] PFN_xrDestroySwapchain DestroySwapchain() const noexcept;
    [[nodiscard]] PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages() const noexcept;
    [[nodiscard]] PFN_xrAcquireSwapchainImage AcquireSwapchainImage() const noexcept;
    [[nodiscard]] PFN_xrWaitSwapchainImage WaitSwapchainImage() const noexcept;
    [[nodiscard]] PFN_xrReleaseSwapchainImage ReleaseSwapchainImage() const noexcept;
    [[nodiscard]] PFN_xrBeginSession BeginSession() const noexcept;
    [[nodiscard]] PFN_xrEndSession EndSession() const noexcept;
    [[nodiscard]] PFN_xrRequestExitSession RequestExitSession() const noexcept;
    [[nodiscard]] PFN_xrWaitFrame WaitFrame() const noexcept;
    [[nodiscard]] PFN_xrBeginFrame BeginFrame() const noexcept;
    [[nodiscard]] PFN_xrEndFrame EndFrame() const noexcept;
    [[nodiscard]] PFN_xrLocateViews LocateViews() const noexcept;
    [[nodiscard]] PFN_xrStringToPath StringToPath() const noexcept;
    [[nodiscard]] PFN_xrCreateActionSet CreateActionSet() const noexcept;
    [[nodiscard]] PFN_xrDestroyActionSet DestroyActionSet() const noexcept;
    [[nodiscard]] PFN_xrCreateAction CreateAction() const noexcept;
    [[nodiscard]] PFN_xrDestroyAction DestroyAction() const noexcept;
    [[nodiscard]] PFN_xrSuggestInteractionProfileBindings SuggestInteractionProfileBindings() const noexcept;
    [[nodiscard]] PFN_xrAttachSessionActionSets AttachSessionActionSets() const noexcept;
    [[nodiscard]] PFN_xrGetActionStateBoolean GetActionStateBoolean() const noexcept;
    [[nodiscard]] PFN_xrGetActionStateFloat GetActionStateFloat() const noexcept;
    [[nodiscard]] PFN_xrGetActionStateVector2f GetActionStateVector2f() const noexcept;
    [[nodiscard]] PFN_xrGetActionStatePose GetActionStatePose() const noexcept;
    [[nodiscard]] PFN_xrSyncActions SyncActions() const noexcept;

private:
    template <typename Function>
    bool LoadGlobal(const char* name, Function& destination, VrLog& log);

    template <typename Function>
    bool LoadInstance(XrInstance instance, const char* name, Function& destination, VrLog& log);

    HMODULE loader_ = nullptr;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr_ = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties enumerateInstanceExtensionProperties_ = nullptr;
    PFN_xrEnumerateApiLayerProperties enumerateApiLayerProperties_ = nullptr;
    PFN_xrCreateInstance createInstance_ = nullptr;

    PFN_xrDestroyInstance destroyInstance_ = nullptr;
    PFN_xrGetInstanceProperties getInstanceProperties_ = nullptr;
    PFN_xrPollEvent pollEvent_ = nullptr;
    PFN_xrResultToString resultToString_ = nullptr;
    PFN_xrGetSystem getSystem_ = nullptr;
    PFN_xrGetSystemProperties getSystemProperties_ = nullptr;
    PFN_xrEnumerateEnvironmentBlendModes enumerateEnvironmentBlendModes_ = nullptr;
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11GraphicsRequirements_ = nullptr;
    PFN_xrCreateSession createSession_ = nullptr;
    PFN_xrDestroySession destroySession_ = nullptr;
    PFN_xrEnumerateReferenceSpaces enumerateReferenceSpaces_ = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace_ = nullptr;
    PFN_xrCreateActionSpace createActionSpace_ = nullptr;
    PFN_xrLocateSpace locateSpace_ = nullptr;
    PFN_xrDestroySpace destroySpace_ = nullptr;
    PFN_xrEnumerateViewConfigurations enumerateViewConfigurations_ = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumerateViewConfigurationViews_ = nullptr;
    PFN_xrEnumerateSwapchainFormats enumerateSwapchainFormats_ = nullptr;
    PFN_xrCreateSwapchain createSwapchain_ = nullptr;
    PFN_xrDestroySwapchain destroySwapchain_ = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateSwapchainImages_ = nullptr;
    PFN_xrAcquireSwapchainImage acquireSwapchainImage_ = nullptr;
    PFN_xrWaitSwapchainImage waitSwapchainImage_ = nullptr;
    PFN_xrReleaseSwapchainImage releaseSwapchainImage_ = nullptr;
    PFN_xrBeginSession beginSession_ = nullptr;
    PFN_xrEndSession endSession_ = nullptr;
    PFN_xrRequestExitSession requestExitSession_ = nullptr;
    PFN_xrWaitFrame waitFrame_ = nullptr;
    PFN_xrBeginFrame beginFrame_ = nullptr;
    PFN_xrEndFrame endFrame_ = nullptr;
    PFN_xrLocateViews locateViews_ = nullptr;
    PFN_xrStringToPath stringToPath_ = nullptr;
    PFN_xrCreateActionSet createActionSet_ = nullptr;
    PFN_xrDestroyActionSet destroyActionSet_ = nullptr;
    PFN_xrCreateAction createAction_ = nullptr;
    PFN_xrDestroyAction destroyAction_ = nullptr;
    PFN_xrSuggestInteractionProfileBindings suggestInteractionProfileBindings_ = nullptr;
    PFN_xrAttachSessionActionSets attachSessionActionSets_ = nullptr;
    PFN_xrGetActionStateBoolean getActionStateBoolean_ = nullptr;
    PFN_xrGetActionStateFloat getActionStateFloat_ = nullptr;
    PFN_xrGetActionStateVector2f getActionStateVector2f_ = nullptr;
    PFN_xrGetActionStatePose getActionStatePose_ = nullptr;
    PFN_xrSyncActions syncActions_ = nullptr;
};

} // namespace gakumas::vr::openxr
