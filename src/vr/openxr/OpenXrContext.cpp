#include "OpenXrContext.hpp"
#include "OpenXrApiLayers.hpp"
#include "../d3d11/TextureFingerprint.hpp"
#include "../SceneReadyGate.hpp"
#include "../VrPhotoShutter.hpp"
#include "../pose/HandPoseMailbox.hpp"

#include "../config/VrifyConfig.hpp"
#include "../VrLog.hpp"

#include <d3d11_4.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

extern std::filesystem::path VrConfigJson;

#ifndef XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME
#define XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME "XR_BD_controller_interaction"
#endif

namespace gakumas::vr::openxr {
namespace {

// Single source with the adjustable placement default (PanelPlacement.hpp).
constexpr float kMirrorPlaneZ = -panel::kDefaultPanelDistanceMetres;
constexpr float kMirrorWidthMetres = panel::kDefaultPanelWidthMetres;
// The settings menu is a separate, closer quad with a fixed texture size so
// its layout never depends on the game's window resolution.
constexpr std::uint32_t kMenuTextureWidth = 1280;
constexpr std::uint32_t kMenuTextureHeight = 880;
constexpr float kMenuPlaneZ = -1.05F;
constexpr float kMenuWidthMetres = 1.05F;
constexpr float kMenuHeightMetres = kMenuWidthMetres *
    static_cast<float>(kMenuTextureHeight) /
    static_cast<float>(kMenuTextureWidth);
constexpr std::uint32_t kMenuPaintFailureLimit = 30;
constexpr float kTriggerPressThreshold = 0.55F;
constexpr float kTriggerReleaseThreshold = 0.45F;
constexpr float kGripPressThreshold = 0.70F;
// Quest 3 / VD squeeze often rests around 0.25-0.45 while the user still
// thinks they are holding Grip. The old 0.35 release fired a short-press
// mid-hold and popped the desktop panel before the long-press menu.
constexpr float kGripReleaseThreshold = 0.12F;
constexpr XrDuration kGripMinimumPressDuration = 40'000'000;
constexpr XrDuration kGripMaximumPressDuration = 600'000'000;
constexpr XrDuration kGripLongPressDuration = kGripMaximumPressDuration;
constexpr XrDuration kGripToggleCooldown = 250'000'000;

// Panel-adjust overlay texture: the top strip is the adjust bar quad, the
// lower strip is the head-locked hint/toast quad. One swapchain, two quads
// via subImage rects.
constexpr std::uint32_t kPanelOverlayTextureWidth = 1120;
constexpr std::uint32_t kPanelOverlayTextureHeight = 512;
constexpr std::uint32_t kPanelOverlayBarHeight = 168;
constexpr std::uint32_t kPanelOverlayHintTop = 192;
constexpr std::uint32_t kPanelOverlayHintHeight = 320;
constexpr std::uint32_t kPanelOverlayPaintFailureLimit = 30;
constexpr float kPanelBarWidthRatio = 0.85F;
constexpr float kHintQuadWidthMetres = 0.85F;
constexpr XrDuration kPanelToastDuration = 2'000'000'000;
// A photo press must produce a capture event or a block within this window;
// otherwise the game silently ignored it (50-shot limit) and the toast says
// so instead of pretending success.
constexpr XrDuration kPhotoResultTimeout = 1'500'000'000;
// Hand-motion gains for the VD-style item adjust (height/size = vertical,
// distance = along the panel direction).
constexpr float kPanelAdjustHeightGain = 1.5F;
constexpr float kPanelAdjustDistanceGain = 2.5F;
// Applied to the diagonal sum (dx + dy), so ~1.4x per metre of pure
// diagonal hand travel.
constexpr float kPanelAdjustSizeGain = 1.5F;

pose::Vector3 ToPoseVector(const XrVector3f& value) noexcept {
    return {value.x, value.y, value.z};
}

pose::Pose ToTrackingPose(const XrPosef& value) noexcept {
    return {
        {value.position.x, value.position.y, value.position.z},
        {
            value.orientation.x,
            value.orientation.y,
            value.orientation.z,
            value.orientation.w,
        },
    };
}

XrPosef ToXrPose(const pose::Pose& value) noexcept {
    XrPosef result;
    result.orientation = {
        value.orientation.x,
        value.orientation.y,
        value.orientation.z,
        value.orientation.w,
    };
    result.position = {value.position.x, value.position.y, value.position.z};
    return result;
}

// Eye buffers are the runtime recommendation scaled down, then capped by the
// runtime's swapchain limit. Multiples of two keep the vertical flip pass and
// the game's post-processing away from odd-row edge cases.
std::uint32_t ScaledEyeExtent(
    std::uint32_t recommended,
    float renderScale,
    std::uint32_t maximum) noexcept {
    const auto scaled = static_cast<std::uint32_t>(std::lround(
        static_cast<double>(recommended) * static_cast<double>(renderScale)));
    std::uint32_t extent = std::max(2U, scaled & ~1U);
    if (maximum != 0U) {
        extent = std::min(extent, maximum & ~1U);
    }
    return std::max(2U, extent);
}

std::string VersionText(XrVersion version) {
    return std::to_string(XR_VERSION_MAJOR(version)) + "." +
           std::to_string(XR_VERSION_MINOR(version)) + "." +
           std::to_string(XR_VERSION_PATCH(version));
}

std::string LuidText(const LUID& luid) {
    std::ostringstream stream;
    stream << std::hex << static_cast<unsigned long>(luid.HighPart) << ':'
           << static_cast<unsigned long>(luid.LowPart);
    return stream.str();
}

const char* BlendModeText(XrEnvironmentBlendMode blendMode) noexcept {
    switch (blendMode) {
    case XR_ENVIRONMENT_BLEND_MODE_OPAQUE:
        return "OPAQUE";
    case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE:
        return "ADDITIVE";
    case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND:
        return "ALPHA_BLEND";
    default:
        return "UNKNOWN";
    }
}

const char* ReferenceSpaceText(XrReferenceSpaceType referenceSpaceType) noexcept {
    switch (referenceSpaceType) {
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return "LOCAL";
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return "STAGE";
    default:
        return "UNKNOWN";
    }
}

bool HasSpaceFlags(XrSpaceLocationFlags value, XrSpaceLocationFlags required) noexcept {
    return (value & required) == required;
}

XrVector3f AimDirection(const XrQuaternionf& orientation) noexcept {
    // OpenXR controller aim points down the action space's negative Z axis.
    return {
        -2.0F * (orientation.x * orientation.z + orientation.w * orientation.y),
        2.0F * (orientation.w * orientation.x - orientation.y * orientation.z),
        -1.0F + 2.0F *
                    (orientation.x * orientation.x + orientation.y * orientation.y),
    };
}

} // namespace

OpenXrContext::~OpenXrContext() {
    Reset();
}

OpenXrContext::InitializeResult OpenXrContext::Initialize(
    const std::filesystem::path& applicationDirectory,
    VrLog& log) {
    if (instance_ != XR_NULL_HANDLE) {
        return InitializeResult::Ready;
    }

    ApplyImplicitApiLayerSearchPath(log);

    const auto loaderResult = dispatch_.LoadAppLocal(applicationDirectory, log);
    if (loaderResult == OpenXrDispatch::LoaderResult::Missing) {
        return InitializeResult::LoaderMissing;
    }
    if (loaderResult == OpenXrDispatch::LoaderResult::LoadFailed ||
        loaderResult == OpenXrDispatch::LoaderResult::InvalidLoader) {
        return InitializeResult::Failed;
    }

    XrResult extensionResult = XR_SUCCESS;
    if (!HasD3D11Extension(log, extensionResult)) {
        lastResult_ = extensionResult;
        if (extensionResult == XR_ERROR_RUNTIME_UNAVAILABLE ||
            extensionResult == XR_ERROR_INITIALIZATION_FAILED) {
            return InitializeResult::RetryRuntime;
        }
        return XR_SUCCEEDED(extensionResult)
                   ? InitializeResult::ExtensionMissing
                   : InitializeResult::Failed;
    }

    const char* enabledExtensions[2] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, nullptr};
    uint32_t enabledExtensionCount = 1;
    if (bdControllerInteractionAvailable_) {
        enabledExtensions[1] = XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME;
        enabledExtensionCount = 2;
        log.Write(
            std::string("[VR][runtime] enabling ") +
            XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
    }
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy_s(
        createInfo.applicationInfo.applicationName,
        sizeof(createInfo.applicationInfo.applicationName),
        "Gakumas VR",
        _TRUNCATE);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy_s(
        createInfo.applicationInfo.engineName,
        sizeof(createInfo.applicationInfo.engineName),
        "Unity/Localify",
        _TRUNCATE);
    createInfo.applicationInfo.engineVersion = 0;
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = enabledExtensionCount;
    createInfo.enabledExtensionNames = enabledExtensions;

    const auto layerSelection = ResolveEnabledApiLayerNames(dispatch_, log);
    const auto bindLayerNames = [](const std::vector<std::string>& names) {
        std::vector<const char*> pointers;
        pointers.reserve(names.size());
        for (const auto& name : names) {
            pointers.push_back(name.c_str());
        }
        return pointers;
    };
    const auto tryCreateInstance = [&](const std::vector<std::string>& names) {
        const auto enabledLayerNames = bindLayerNames(names);
        createInfo.enabledApiLayerCount = static_cast<uint32_t>(enabledLayerNames.size());
        createInfo.enabledApiLayerNames =
            enabledLayerNames.empty() ? nullptr : enabledLayerNames.data();
        instance_ = XR_NULL_HANDLE;
        lastResult_ = dispatch_.CreateInstance()(&createInfo, &instance_);
    };

    tryCreateInstance(layerSelection.enabledNames);
    if (XR_FAILED(lastResult_) && !layerSelection.enabledNames.empty()) {
        if (lastResult_ == XR_ERROR_API_LAYER_NOT_PRESENT) {
            const auto enumeratedOnly =
                EnabledApiLayerNamesWithoutUnenumeratedImplicit(layerSelection);
            if (enumeratedOnly.size() != layerSelection.enabledNames.size()) {
                std::ostringstream dropped;
                dropped << "[VR][runtime] API_LAYER_RETRY dropping implicit-registry after "
                        << dispatch_.ResultText(instance_, lastResult_);
                if (!enumeratedOnly.empty()) {
                    dropped << " keep=";
                    for (std::size_t index = 0; index < enumeratedOnly.size(); ++index) {
                        if (index != 0) {
                            dropped << ',';
                        }
                        dropped << enumeratedOnly[index];
                    }
                }
                log.Write(dropped.str());
                tryCreateInstance(enumeratedOnly);
            }
        }
        if (XR_FAILED(lastResult_)) {
            log.Write(
                "[VR][runtime] API_LAYER_RETRY clearing enabledApiLayerNames after " +
                dispatch_.ResultText(instance_, lastResult_));
            tryCreateInstance({});
            if (XR_SUCCEEDED(lastResult_)) {
                log.Write("[VR][runtime] API_LAYER_RETRY succeeded without forced layers");
            }
        }
    }
    if (XR_FAILED(lastResult_)) {
        instance_ = XR_NULL_HANDLE;
        log.Write("[VR][runtime] xrCreateInstance failed: " + dispatch_.ResultText(instance_, lastResult_));
        if (lastResult_ == XR_ERROR_RUNTIME_UNAVAILABLE ||
            lastResult_ == XR_ERROR_INITIALIZATION_FAILED) {
            return InitializeResult::RetryRuntime;
        }
        return InitializeResult::Failed;
    }

    if (!dispatch_.LoadInstanceFunctions(instance_, log)) {
        ResetInstance();
        lastResult_ = XR_ERROR_FUNCTION_UNSUPPORTED;
        return InitializeResult::Failed;
    }

    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    lastResult_ = dispatch_.GetInstanceProperties()(instance_, &properties);
    if (XR_FAILED(lastResult_)) {
        log.Write("[VR][runtime] xrGetInstanceProperties failed: " + dispatch_.ResultText(instance_, lastResult_));
        ResetInstance();
        return InitializeResult::Failed;
    }

    runtimeName_ = properties.runtimeName;
    runtimeVersionText_ = VersionText(properties.runtimeVersion);
    log.Write(
        std::string("[VR][runtime] runtime=") + runtimeName_ +
        " version=" + runtimeVersionText_);
    return InitializeResult::Ready;
}

bool OpenXrContext::HasD3D11Extension(VrLog& log, XrResult& result) {
    uint32_t count = 0;
    result = dispatch_.EnumerateInstanceExtensionProperties()(nullptr, 0, &count, nullptr);
    if (XR_FAILED(result)) {
        log.Write("[VR][runtime] extension enumeration failed: " + std::to_string(static_cast<int>(result)));
        return false;
    }

    std::vector<XrExtensionProperties> extensions;
    for (int attempt = 0; attempt < 3; ++attempt) {
        extensions.assign(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
        uint32_t written = count;
        result = dispatch_.EnumerateInstanceExtensionProperties()(
            nullptr,
            static_cast<uint32_t>(extensions.size()),
            &written,
            extensions.data());
        if (result == XR_ERROR_SIZE_INSUFFICIENT) {
            count = written;
            continue;
        }
        if (XR_FAILED(result)) {
            log.Write(
                "[VR][runtime] extension enumeration data call failed: " +
                std::to_string(static_cast<int>(result)));
            return false;
        }
        if (written < extensions.size()) {
            extensions.resize(written);
        }
        break;
    }
    if (result == XR_ERROR_SIZE_INSUFFICIENT) {
        log.Write("[VR][runtime] extension list kept changing during enumeration");
        return false;
    }

    const bool found = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
        return std::strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    });
    bdControllerInteractionAvailable_ = std::any_of(
        extensions.begin(),
        extensions.end(),
        [](const auto& extension) {
            return std::strcmp(
                       extension.extensionName,
                       XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME) == 0;
        });
    log.Write(
        std::string("[VR][runtime] ") + XR_KHR_D3D11_ENABLE_EXTENSION_NAME +
        (found ? " available" : " NOT AVAILABLE"));
    log.Write(
        std::string("[VR][runtime] ") + XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME +
        (bdControllerInteractionAvailable_ ? " available" : " NOT AVAILABLE"));
    return found;
}

OpenXrContext::SystemResult OpenXrContext::AcquireHeadMountedSystem(VrLog& log) {
    if (systemId_ != XR_NULL_SYSTEM_ID) {
        return SystemResult::Ready;
    }
    if (instance_ == XR_NULL_HANDLE || dispatch_.GetSystem() == nullptr) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        return SystemResult::Failed;
    }

    XrSystemGetInfo getInfo{XR_TYPE_SYSTEM_GET_INFO};
    getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    lastResult_ = dispatch_.GetSystem()(instance_, &getInfo, &systemId_);
    if (lastResult_ == XR_ERROR_FORM_FACTOR_UNAVAILABLE ||
        lastResult_ == XR_ERROR_RUNTIME_UNAVAILABLE) {
        systemId_ = XR_NULL_SYSTEM_ID;
        return SystemResult::RetryHeadset;
    }
    if (XR_FAILED(lastResult_)) {
        systemId_ = XR_NULL_SYSTEM_ID;
        log.Write("[VR][runtime] xrGetSystem failed: " + dispatch_.ResultText(instance_, lastResult_));
        return SystemResult::Failed;
    }

    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    lastResult_ = dispatch_.GetSystemProperties()(instance_, systemId_, &properties);
    if (XR_FAILED(lastResult_)) {
        log.Write("[VR][runtime] xrGetSystemProperties failed: " + dispatch_.ResultText(instance_, lastResult_));
        systemId_ = XR_NULL_SYSTEM_ID;
        return SystemResult::Failed;
    }

    mirrorMaximumWidth_ = properties.graphicsProperties.maxSwapchainImageWidth;
    mirrorMaximumHeight_ = properties.graphicsProperties.maxSwapchainImageHeight;
    if (mirrorMaximumWidth_ == 0 || mirrorMaximumHeight_ == 0) {
        lastResult_ = XR_ERROR_RUNTIME_FAILURE;
        log.Write("[VR][runtime] runtime reported an invalid maximum swapchain size");
        systemId_ = XR_NULL_SYSTEM_ID;
        return SystemResult::Failed;
    }

    systemName_ = properties.systemName;
    log.Write(
        std::string("[VR][runtime] system=") + systemName_ +
        " vendor=" + std::to_string(properties.vendorId) +
        " maxSwapchain=" + std::to_string(mirrorMaximumWidth_) + "x" +
        std::to_string(mirrorMaximumHeight_));
    return SystemResult::Ready;
}

bool OpenXrContext::QueryD3D11Requirements(
    D3D11Requirements& requirements,
    VrLog& log) {
    if (instance_ == XR_NULL_HANDLE || systemId_ == XR_NULL_SYSTEM_ID ||
        dispatch_.GetD3D11GraphicsRequirements() == nullptr) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        return false;
    }

    XrGraphicsRequirementsD3D11KHR graphicsRequirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    lastResult_ = dispatch_.GetD3D11GraphicsRequirements()(
        instance_,
        systemId_,
        &graphicsRequirements);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][runtime] xrGetD3D11GraphicsRequirementsKHR failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    requirements.adapterLuid = graphicsRequirements.adapterLuid;
    requirements.minimumFeatureLevel = graphicsRequirements.minFeatureLevel;
    log.Write(
        "[VR][runtime] required adapter LUID=" + LuidText(requirements.adapterLuid) +
        " minFeatureLevel=0x" + [&] {
            std::ostringstream stream;
            stream << std::hex << static_cast<unsigned>(requirements.minimumFeatureLevel);
            return stream.str();
        }());
    return true;
}

OpenXrContext::SessionResult OpenXrContext::CreateD3D11Session(
    ID3D11Device* device,
    const LUID& adapterLuid,
    D3D_FEATURE_LEVEL featureLevel,
    const D3D11_TEXTURE2D_DESC& sourceFrameDescription,
    std::uint64_t sourceLayoutGeneration,
    const D3D11Requirements& requirements,
    bool stereoProjectionEnabled,
    bool stereoLandscapeOnly,
    float stereoRenderScale,
    VrLog& log) {
    if (session_ != XR_NULL_HANDLE) {
        return SessionResult::Ready;
    }
    if (device == nullptr || instance_ == XR_NULL_HANDLE || systemId_ == XR_NULL_SYSTEM_ID) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        return SessionResult::Failed;
    }
    if (!EqualLuid(adapterLuid, requirements.adapterLuid)) {
        log.Write(
            "[VR][runtime] rejected D3D11 device: Unity LUID=" + LuidText(adapterLuid) +
            " runtime LUID=" + LuidText(requirements.adapterLuid));
        return SessionResult::AdapterMismatch;
    }
    if (featureLevel < requirements.minimumFeatureLevel) {
        log.Write("[VR][runtime] rejected D3D11 device: feature level is below runtime requirement");
        return SessionResult::FeatureLevelMismatch;
    }
    if (sourceFrameDescription.Width == 0 || sourceFrameDescription.Height == 0 ||
        sourceFrameDescription.MipLevels != 1 || sourceFrameDescription.ArraySize != 1 ||
        sourceFrameDescription.SampleDesc.Count != 1 ||
        !IsSupportedMirrorFormat(sourceFrameDescription.Format)) {
        lastResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        log.Write(
            "[VR][display] rejected Unity frame texture: size=" +
            std::to_string(sourceFrameDescription.Width) + "x" +
            std::to_string(sourceFrameDescription.Height) +
            " format=" + std::to_string(static_cast<int>(sourceFrameDescription.Format)) +
            " mips=" + std::to_string(sourceFrameDescription.MipLevels) +
            " arraySize=" + std::to_string(sourceFrameDescription.ArraySize) +
            " samples=" + std::to_string(sourceFrameDescription.SampleDesc.Count));
        return SessionResult::Failed;
    }

    mirrorWidth_ = sourceFrameDescription.Width;
    mirrorHeight_ = sourceFrameDescription.Height;
    mirrorSourceFormat_ = sourceFrameDescription.Format;
    mirrorLayoutGeneration_ = sourceLayoutGeneration;
    stereoProjectionEnabled_ = stereoProjectionEnabled;
    stereoLandscapeOnly_ = stereoLandscapeOnly;
    stereoRenderScale_ = std::clamp(stereoRenderScale, 0.25F, 1.5F);
    projectionDisabledForSession_ = false;

    ID3D11DeviceContext* immediateContext = nullptr;
    device->GetImmediateContext(&immediateContext);
    ID3D11Multithread* multithread = nullptr;
    const bool multithreadProtected =
        immediateContext != nullptr &&
        SUCCEEDED(immediateContext->QueryInterface(IID_PPV_ARGS(&multithread))) &&
        multithread != nullptr &&
        multithread->GetMultithreadProtected() != FALSE;
    if (multithread != nullptr) {
        multithread->Release();
    }
    if (immediateContext != nullptr) {
        immediateContext->Release();
    }
    if (!multithreadProtected) {
        lastResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        log.Write(
            "[VR][display] D3D11 multithread protection was not enabled before "
            "worker-thread session creation");
        return SessionResult::Failed;
    }
    if (!ValidateStereoViewConfiguration(log)) {
        return SessionResult::Failed;
    }
    if (!CreateInputActions(log)) {
        log.Write(
            "[VR][input] controller actions are unavailable; continuing with display only");
        lastResult_ = XR_SUCCESS;
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
    createInfo.next = &binding;
    createInfo.systemId = systemId_;

    lastResult_ = dispatch_.CreateSession()(instance_, &createInfo, &session_);
    if (XR_FAILED(lastResult_)) {
        session_ = XR_NULL_HANDLE;
        log.Write("[VR][runtime] xrCreateSession failed: " + dispatch_.ResultText(instance_, lastResult_));
        if (lastResult_ == XR_ERROR_RUNTIME_UNAVAILABLE ||
            lastResult_ == XR_ERROR_FORM_FACTOR_UNAVAILABLE ||
            lastResult_ == XR_ERROR_SESSION_LOST ||
            lastResult_ == XR_ERROR_INSTANCE_LOST) {
            return SessionResult::RetryRuntime;
        }
        return SessionResult::Failed;
    }

    device->AddRef();
    sessionDevice_ = device;

    if (!SelectEnvironmentBlendMode(log) ||
        !CreateReferenceSpaces(log) ||
        !CreateMirrorSwapchain(log)) {
        const XrResult setupResult = lastResult_;
        ResetSession();
        lastResult_ = setupResult;
        if (lastResult_ == XR_ERROR_RUNTIME_UNAVAILABLE ||
            lastResult_ == XR_ERROR_FORM_FACTOR_UNAVAILABLE ||
            lastResult_ == XR_ERROR_SESSION_LOST ||
            lastResult_ == XR_ERROR_INSTANCE_LOST) {
            return SessionResult::RetryRuntime;
        }
        return SessionResult::Failed;
    }
    if (inputActionsCreated_ && !AttachInputActions(log)) {
        log.Write(
            "[VR][input] controller actions could not be attached; continuing with display only");
    }

    lastResult_ = XR_SUCCESS;
    log.Write("[VR][runtime] xrCreateSession succeeded; frame loop is waiting for READY");
    return SessionResult::Ready;
}

bool OpenXrContext::ValidateStereoViewConfiguration(VrLog& log) {
    if (instance_ == XR_NULL_HANDLE || systemId_ == XR_NULL_SYSTEM_ID ||
        dispatch_.EnumerateViewConfigurations() == nullptr ||
        dispatch_.EnumerateViewConfigurationViews() == nullptr) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        return false;
    }

    uint32_t configurationCount = 0;
    lastResult_ = dispatch_.EnumerateViewConfigurations()(
        instance_, systemId_, 0, &configurationCount, nullptr);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][pose] xrEnumerateViewConfigurations failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    std::vector<XrViewConfigurationType> configurations;
    for (int attempt = 0; attempt < 3; ++attempt) {
        configurations.resize(configurationCount);
        uint32_t written = configurationCount;
        lastResult_ = dispatch_.EnumerateViewConfigurations()(
            instance_,
            systemId_,
            static_cast<uint32_t>(configurations.size()),
            &written,
            configurations.data());
        if (lastResult_ == XR_ERROR_SIZE_INSUFFICIENT) {
            configurationCount = written;
            continue;
        }
        if (XR_FAILED(lastResult_)) {
            log.Write(
                "[VR][pose] xrEnumerateViewConfigurations data call failed: " +
                dispatch_.ResultText(instance_, lastResult_));
            return false;
        }
        configurations.resize(std::min(written, static_cast<uint32_t>(configurations.size())));
        break;
    }
    if (lastResult_ == XR_ERROR_SIZE_INSUFFICIENT) {
        log.Write("[VR][pose] view configuration list kept changing");
        return false;
    }
    if (std::find(
            configurations.begin(),
            configurations.end(),
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) == configurations.end()) {
        lastResult_ = XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
        log.Write("[VR][pose] runtime does not support PRIMARY_STEREO");
        return false;
    }

    uint32_t viewCount = 0;
    lastResult_ = dispatch_.EnumerateViewConfigurationViews()(
        instance_,
        systemId_,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0,
        &viewCount,
        nullptr);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][pose] xrEnumerateViewConfigurationViews failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    std::vector<XrViewConfigurationView> views;
    for (int attempt = 0; attempt < 3; ++attempt) {
        views.assign(viewCount, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
        uint32_t written = viewCount;
        lastResult_ = dispatch_.EnumerateViewConfigurationViews()(
            instance_,
            systemId_,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            static_cast<uint32_t>(views.size()),
            &written,
            views.data());
        if (lastResult_ == XR_ERROR_SIZE_INSUFFICIENT) {
            viewCount = written;
            continue;
        }
        if (XR_FAILED(lastResult_)) {
            log.Write(
                "[VR][pose] xrEnumerateViewConfigurationViews data call failed: " +
                dispatch_.ResultText(instance_, lastResult_));
            return false;
        }
        views.resize(std::min(written, static_cast<uint32_t>(views.size())));
        break;
    }
    if (lastResult_ == XR_ERROR_SIZE_INSUFFICIENT) {
        log.Write("[VR][pose] stereo view list kept changing");
        return false;
    }
    if (views.size() != 2) {
        lastResult_ = XR_ERROR_RUNTIME_FAILURE;
        log.Write(
            "[VR][pose] PRIMARY_STEREO reported " + std::to_string(views.size()) +
            " view(s); expected 2");
        return false;
    }

    lastResult_ = XR_SUCCESS;
    log.Write(
        "[VR][pose] PRIMARY_STEREO views=2 recommended=" +
        std::to_string(views[0].recommendedImageRectWidth) + "x" +
        std::to_string(views[0].recommendedImageRectHeight) + "," +
        std::to_string(views[1].recommendedImageRectWidth) + "x" +
        std::to_string(views[1].recommendedImageRectHeight));
    if (!consoleSummaryLogged_ && !runtimeName_.empty() && !systemName_.empty()) {
        ::gakumas::vr::WriteVrConsole(
            "OpenXR runtime=" + runtimeName_ +
            " version=" + runtimeVersionText_ +
            " HMD=" + systemName_ +
            " views=2 recommended=" +
            std::to_string(views[0].recommendedImageRectWidth) + "x" +
            std::to_string(views[0].recommendedImageRectHeight));
        consoleSummaryLogged_ = true;
    }
    recommendedEyeWidth_ = views[0].recommendedImageRectWidth;
    recommendedEyeHeight_ = views[0].recommendedImageRectHeight;
    stereoEyeWidth_ = ScaledEyeExtent(
        recommendedEyeWidth_, stereoRenderScale_, mirrorMaximumWidth_);
    stereoEyeHeight_ = ScaledEyeExtent(
        recommendedEyeHeight_, stereoRenderScale_, mirrorMaximumHeight_);
    if (stereoProjectionEnabled_) {
        log.Write(
            "[VR][stereo] target spec " + std::to_string(stereoEyeWidth_) + "x" +
            std::to_string(stereoEyeHeight_) + " scale=" +
            std::to_string(stereoRenderScale_) + " landscapeOnly=" +
            std::to_string(stereoLandscapeOnly_));
    }
    // The mirror is submitted as a Quad layer, not as a projection view. Its
    // texture is therefore bounded by XrSystemGraphicsProperties' swapchain
    // limits rather than PRIMARY_STEREO's per-view maxImageRect dimensions.
    if (mirrorWidth_ > mirrorMaximumWidth_ ||
        mirrorHeight_ > mirrorMaximumHeight_) {
        lastResult_ = XR_ERROR_SWAPCHAIN_RECT_INVALID;
        log.Write(
            "[VR][display] Unity frame " + std::to_string(mirrorWidth_) + "x" +
            std::to_string(mirrorHeight_) + " exceeds runtime maximum " +
            std::to_string(mirrorMaximumWidth_) + "x" +
            std::to_string(mirrorMaximumHeight_));
        return false;
    }
    return true;
}

bool OpenXrContext::SelectEnvironmentBlendMode(VrLog& log) {
    if (instance_ == XR_NULL_HANDLE || systemId_ == XR_NULL_SYSTEM_ID ||
        dispatch_.EnumerateEnvironmentBlendModes() == nullptr) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        return false;
    }

    uint32_t count = 0;
    lastResult_ = dispatch_.EnumerateEnvironmentBlendModes()(
        instance_,
        systemId_,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0,
        &count,
        nullptr);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][runtime] xrEnumerateEnvironmentBlendModes failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    std::vector<XrEnvironmentBlendMode> modes;
    for (int attempt = 0; attempt < 3; ++attempt) {
        modes.resize(count);
        uint32_t written = count;
        lastResult_ = dispatch_.EnumerateEnvironmentBlendModes()(
            instance_,
            systemId_,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            static_cast<uint32_t>(modes.size()),
            &written,
            modes.data());
        if (lastResult_ == XR_ERROR_SIZE_INSUFFICIENT) {
            count = written;
            continue;
        }
        if (XR_FAILED(lastResult_)) {
            log.Write(
                "[VR][runtime] xrEnumerateEnvironmentBlendModes data call failed: " +
                dispatch_.ResultText(instance_, lastResult_));
            return false;
        }
        if (written < modes.size()) {
            modes.resize(written);
        }
        break;
    }
    if (lastResult_ == XR_ERROR_SIZE_INSUFFICIENT) {
        log.Write("[VR][runtime] environment blend mode list kept changing");
        return false;
    }
    if (modes.empty()) {
        lastResult_ = XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
        log.Write("[VR][runtime] runtime returned no environment blend modes");
        return false;
    }

    const auto opaque = std::find(
        modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
    environmentBlendMode_ = opaque != modes.end() ? *opaque : modes.front();
    lastResult_ = XR_SUCCESS;
    log.Write(
        std::string("[VR][runtime] environment blend mode=") +
        BlendModeText(environmentBlendMode_));
    return true;
}

bool OpenXrContext::CreateReferenceSpaces(VrLog& log) {
    if (session_ == XR_NULL_HANDLE ||
        dispatch_.EnumerateReferenceSpaces() == nullptr ||
        dispatch_.CreateReferenceSpace() == nullptr) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        return false;
    }

    uint32_t count = 0;
    lastResult_ = dispatch_.EnumerateReferenceSpaces()(session_, 0, &count, nullptr);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][runtime] xrEnumerateReferenceSpaces failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    std::vector<XrReferenceSpaceType> spaces;
    for (int attempt = 0; attempt < 3; ++attempt) {
        spaces.resize(count);
        uint32_t written = count;
        lastResult_ = dispatch_.EnumerateReferenceSpaces()(
            session_,
            static_cast<uint32_t>(spaces.size()),
            &written,
            spaces.data());
        if (lastResult_ == XR_ERROR_SIZE_INSUFFICIENT) {
            count = written;
            continue;
        }
        if (XR_FAILED(lastResult_)) {
            log.Write(
                "[VR][runtime] xrEnumerateReferenceSpaces data call failed: " +
                dispatch_.ResultText(instance_, lastResult_));
            return false;
        }
        if (written < spaces.size()) {
            spaces.resize(written);
        }
        break;
    }
    if (lastResult_ == XR_ERROR_SIZE_INSUFFICIENT) {
        log.Write("[VR][runtime] reference space list kept changing");
        return false;
    }

    const bool hasLocal = std::find(
        spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_LOCAL) != spaces.end();
    const bool hasView = std::find(
        spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_VIEW) != spaces.end();
    const bool hasStage = std::find(
        spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end();
    if (!hasLocal || !hasView) {
        lastResult_ = XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
        log.Write("[VR][runtime] runtime does not support required VIEW/LOCAL reference spaces");
        return false;
    }

    XrReferenceSpaceCreateInfo createInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    createInfo.poseInReferenceSpace.orientation.w = 1.0F;
    lastResult_ = dispatch_.CreateReferenceSpace()(session_, &createInfo, &viewSpace_);
    if (XR_FAILED(lastResult_)) {
        viewSpace_ = XR_NULL_HANDLE;
        log.Write(
            "[VR][runtime] xrCreateReferenceSpace(VIEW) failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    lastResult_ = dispatch_.CreateReferenceSpace()(session_, &createInfo, &localSpace_);
    if (XR_FAILED(lastResult_)) {
        localSpace_ = XR_NULL_HANDLE;
        log.Write(
            "[VR][runtime] xrCreateReferenceSpace(LOCAL) failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    if (hasStage) {
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        lastResult_ = dispatch_.CreateReferenceSpace()(session_, &createInfo, &stageSpace_);
        if (XR_FAILED(lastResult_)) {
            stageSpace_ = XR_NULL_HANDLE;
            if (lastResult_ == XR_ERROR_SESSION_LOST ||
                lastResult_ == XR_ERROR_INSTANCE_LOST ||
                lastResult_ == XR_ERROR_RUNTIME_UNAVAILABLE) {
                log.Write(
                    "[VR][runtime] xrCreateReferenceSpace(STAGE) failed: " +
                    dispatch_.ResultText(instance_, lastResult_));
                return false;
            }
            log.Write(
                "[VR][runtime] STAGE reference space unavailable (" +
                dispatch_.ResultText(instance_, lastResult_) + "); using LOCAL");
        }
    }

    lastResult_ = XR_SUCCESS;
    log.Write(
        std::string("[VR][runtime] active reference space=") +
        ReferenceSpaceText(ActiveReferenceSpaceType()));
    return true;
}

bool OpenXrContext::CreateInputActions(VrLog& log) {
    if (inputActionsCreated_) {
        return true;
    }
    if (instance_ == XR_NULL_HANDLE || dispatch_.StringToPath() == nullptr ||
        dispatch_.CreateActionSet() == nullptr || dispatch_.CreateAction() == nullptr ||
        dispatch_.SuggestInteractionProfileBindings() == nullptr) {
        lastResult_ = XR_ERROR_FUNCTION_UNSUPPORTED;
        return false;
    }

    auto stringToPath = [&](const char* text, XrPath& path) {
        const XrResult result = dispatch_.StringToPath()(instance_, text, &path);
        if (XR_FAILED(result)) {
            lastResult_ = result;
            log.Write(
                std::string("[VR][input] xrStringToPath failed for ") + text + ": " +
                dispatch_.ResultText(instance_, result));
            return false;
        }
        return true;
    };

    if (!stringToPath("/user/hand/left", handPaths_[0]) ||
        !stringToPath("/user/hand/right", handPaths_[1])) {
        ResetInputActions();
        return false;
    }

    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(setInfo.actionSetName, "gakumas_vr_input");
    strcpy_s(setInfo.localizedActionSetName, "Gakumas VR input");
    lastResult_ = dispatch_.CreateActionSet()(instance_, &setInfo, &inputActionSet_);
    if (XR_FAILED(lastResult_)) {
        inputActionSet_ = XR_NULL_HANDLE;
        log.Write(
            "[VR][input] xrCreateActionSet failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.countSubactionPaths = static_cast<uint32_t>(handPaths_.size());
    actionInfo.subactionPaths = handPaths_.data();
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(actionInfo.actionName, "pointer_aim");
    strcpy_s(actionInfo.localizedActionName, "Pointer aim");
    lastResult_ = dispatch_.CreateAction()(inputActionSet_, &actionInfo, &aimPoseAction_);
    if (XR_FAILED(lastResult_)) {
        aimPoseAction_ = XR_NULL_HANDLE;
        log.Write(
            "[VR][input] xrCreateAction(pointer_aim) failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        ResetInputActions();
        return false;
    }

    actionInfo = XrActionCreateInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.countSubactionPaths = static_cast<uint32_t>(handPaths_.size());
    actionInfo.subactionPaths = handPaths_.data();
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(actionInfo.actionName, "hand_grip_pose");
    strcpy_s(actionInfo.localizedActionName, "Hand grip pose");
    lastResult_ = dispatch_.CreateAction()(
        inputActionSet_, &actionInfo, &gripPoseAction_);
    if (XR_FAILED(lastResult_)) {
        gripPoseAction_ = XR_NULL_HANDLE;
        log.Write(
            "[VR][input] grip pose unavailable; hand glow sticks fall back to aim");
        lastResult_ = XR_SUCCESS;
    }

    actionInfo = XrActionCreateInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.countSubactionPaths = static_cast<uint32_t>(handPaths_.size());
    actionInfo.subactionPaths = handPaths_.data();
    actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    strcpy_s(actionInfo.actionName, "pointer_trigger");
    strcpy_s(actionInfo.localizedActionName, "Pointer trigger");
    lastResult_ = dispatch_.CreateAction()(inputActionSet_, &actionInfo, &triggerAction_);
    if (XR_FAILED(lastResult_)) {
        triggerAction_ = XR_NULL_HANDLE;
        log.Write(
            "[VR][input] xrCreateAction(pointer_trigger) failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        ResetInputActions();
        return false;
    }

    actionInfo = XrActionCreateInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.countSubactionPaths = static_cast<uint32_t>(handPaths_.size());
    actionInfo.subactionPaths = handPaths_.data();
    actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    strcpy_s(actionInfo.actionName, "stereo_ui_grip");
    strcpy_s(actionInfo.localizedActionName, "Stereo UI grip");
    lastResult_ = dispatch_.CreateAction()(inputActionSet_, &actionInfo, &gripAction_);
    if (XR_FAILED(lastResult_)) {
        gripAction_ = XR_NULL_HANDLE;
        log.Write(
            "[VR][ui] grip action unavailable; stereo UI panel toggle disabled");
        lastResult_ = XR_SUCCESS;
    }

    actionInfo = XrActionCreateInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.countSubactionPaths = static_cast<uint32_t>(handPaths_.size());
    actionInfo.subactionPaths = handPaths_.data();
    actionInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    strcpy_s(actionInfo.actionName, "list_scroll");
    strcpy_s(actionInfo.localizedActionName, "List scroll");
    lastResult_ = dispatch_.CreateAction()(
        inputActionSet_, &actionInfo, &thumbstickAction_);
    if (XR_FAILED(lastResult_)) {
        thumbstickAction_ = XR_NULL_HANDLE;
        log.Write(
            "[VR][input] xrCreateAction(list_scroll) failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        ResetInputActions();
        return false;
    }

    actionInfo = XrActionCreateInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.countSubactionPaths = 0;
    actionInfo.subactionPaths = nullptr;
    actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strcpy_s(actionInfo.actionName, "live_pause");
    strcpy_s(actionInfo.localizedActionName, "Live pause");
    lastResult_ = dispatch_.CreateAction()(
        inputActionSet_, &actionInfo, &livePauseAction_);
    if (XR_FAILED(lastResult_)) {
        livePauseAction_ = XR_NULL_HANDLE;
        log.Write(
            "[VR][live] pause action unavailable; right B toggle disabled");
        lastResult_ = XR_SUCCESS;
    }

    // VR free-camera buttons: left X cycles the mode, left Y switches the
    // followed character, a long press on the left thumbstick click resets
    // the current mode. All optional: a failed creation only disables the
    // free camera controls.
    auto createCameraAction = [&](const char* name,
                                  const char* localizedName,
                                  XrAction& action) {
        actionInfo = XrActionCreateInfo{XR_TYPE_ACTION_CREATE_INFO};
        actionInfo.countSubactionPaths = 0;
        actionInfo.subactionPaths = nullptr;
        actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        strcpy_s(actionInfo.actionName, name);
        strcpy_s(actionInfo.localizedActionName, localizedName);
        lastResult_ = dispatch_.CreateAction()(
            inputActionSet_, &actionInfo, &action);
        if (XR_FAILED(lastResult_)) {
            action = XR_NULL_HANDLE;
            log.Write(
                std::string("[VR][camera] action unavailable: ") + name);
            lastResult_ = XR_SUCCESS;
        }
    };
    createCameraAction("camera_mode", "Camera mode", cameraModeAction_);
    createCameraAction("camera_chara", "Camera character", cameraCharaAction_);
    createCameraAction("camera_reset", "Camera reset", cameraResetAction_);
    // Right A: photo shutter in photo scenes, panel adjust everywhere else.
    createCameraAction(
        "panel_a_button", "Panel adjust / photo", panelAdjustAction_);

    struct FaceButtons {
        const char* cameraMode;
        const char* cameraChara;
        const char* cameraReset;
        const char* livePause;
        const char* aButton;
    };
    const FaceButtons touchFaceButtons{
        "/user/hand/left/input/x/click",
        "/user/hand/left/input/y/click",
        "/user/hand/left/input/thumbstick/click",
        "/user/hand/right/input/b/click",
        "/user/hand/right/input/a/click"};
    // Index has A/B on both hands and no X/Y. Left A/B keep the Touch camera
    // mapping; right A/B keep pause and photo / panel-adjust.
    const FaceButtons indexFaceButtons{
        "/user/hand/left/input/a/click",
        "/user/hand/left/input/b/click",
        "/user/hand/left/input/thumbstick/click",
        "/user/hand/right/input/b/click",
        "/user/hand/right/input/a/click"};

    auto suggestProfile = [&](const char* profileText,
                              const std::array<const char*, 2>& aimTexts,
                              const std::array<const char*, 2>& triggerTexts,
                              bool includeThumbstick,
                              bool includeGrip,
                              bool includeLivePause,
                              bool includeCameraButtons,
                              bool includeAButton,
                              bool includeGripPose,
                              const FaceButtons& faceButtons) {
        XrPath profile = XR_NULL_PATH;
        std::array<XrPath, 2> aimPaths{};
        std::array<XrPath, 2> triggerPaths{};
        std::array<XrPath, 2> thumbstickPaths{};
        std::array<XrPath, 2> gripPaths{};
        std::array<XrPath, 2> gripPosePaths{};
        XrPath livePausePath = XR_NULL_PATH;
        XrPath cameraModePath = XR_NULL_PATH;
        XrPath cameraCharaPath = XR_NULL_PATH;
        XrPath cameraResetPath = XR_NULL_PATH;
        XrPath aButtonPath = XR_NULL_PATH;
        if (!stringToPath(profileText, profile)) {
            return false;
        }
        for (std::size_t hand = 0; hand < handPaths_.size(); ++hand) {
            if (!stringToPath(aimTexts[hand], aimPaths[hand]) ||
                !stringToPath(triggerTexts[hand], triggerPaths[hand])) {
                return false;
            }
        }
        if (includeThumbstick &&
            (!stringToPath("/user/hand/left/input/thumbstick", thumbstickPaths[0]) ||
             !stringToPath("/user/hand/right/input/thumbstick", thumbstickPaths[1]))) {
            return false;
        }
        if (includeGrip &&
            (gripAction_ == XR_NULL_HANDLE ||
             !stringToPath("/user/hand/left/input/squeeze/value", gripPaths[0]) ||
             !stringToPath("/user/hand/right/input/squeeze/value", gripPaths[1]))) {
            return false;
        }
        if (includeGripPose &&
            (gripPoseAction_ == XR_NULL_HANDLE ||
             !stringToPath("/user/hand/left/input/grip/pose", gripPosePaths[0]) ||
             !stringToPath("/user/hand/right/input/grip/pose", gripPosePaths[1]))) {
            return false;
        }
        if (includeLivePause &&
            (livePauseAction_ == XR_NULL_HANDLE ||
             !stringToPath(faceButtons.livePause, livePausePath))) {
            return false;
        }
        if (includeCameraButtons &&
            (cameraModeAction_ == XR_NULL_HANDLE ||
             cameraCharaAction_ == XR_NULL_HANDLE ||
             cameraResetAction_ == XR_NULL_HANDLE ||
             !stringToPath(faceButtons.cameraMode, cameraModePath) ||
             !stringToPath(faceButtons.cameraChara, cameraCharaPath) ||
             !stringToPath(faceButtons.cameraReset, cameraResetPath))) {
            return false;
        }
        if (includeAButton &&
            (panelAdjustAction_ == XR_NULL_HANDLE ||
             !stringToPath(faceButtons.aButton, aButtonPath))) {
            return false;
        }

        std::array<XrActionSuggestedBinding, 16> bindings{};
        bindings[0] = {aimPoseAction_, aimPaths[0]};
        bindings[1] = {aimPoseAction_, aimPaths[1]};
        bindings[2] = {triggerAction_, triggerPaths[0]};
        bindings[3] = {triggerAction_, triggerPaths[1]};
        bindings[4] = {thumbstickAction_, thumbstickPaths[0]};
        bindings[5] = {thumbstickAction_, thumbstickPaths[1]};
        bindings[6] = {gripAction_, gripPaths[0]};
        bindings[7] = {gripAction_, gripPaths[1]};
        uint32_t count = includeGrip ? 8U : (includeThumbstick ? 6U : 4U);
        if (includeGripPose) {
            bindings[count] = {gripPoseAction_, gripPosePaths[0]};
            ++count;
            bindings[count] = {gripPoseAction_, gripPosePaths[1]};
            ++count;
        }
        if (includeLivePause) {
            bindings[count] = {livePauseAction_, livePausePath};
            ++count;
        }
        if (includeCameraButtons) {
            bindings[count] = {cameraModeAction_, cameraModePath};
            ++count;
            bindings[count] = {cameraCharaAction_, cameraCharaPath};
            ++count;
            bindings[count] = {cameraResetAction_, cameraResetPath};
            ++count;
        }
        if (includeAButton) {
            bindings[count] = {panelAdjustAction_, aButtonPath};
            ++count;
        }
        XrInteractionProfileSuggestedBinding suggested{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = profile;
        suggested.countSuggestedBindings = count;
        suggested.suggestedBindings = bindings.data();
        const XrResult result =
            dispatch_.SuggestInteractionProfileBindings()(instance_, &suggested);
        if (XR_FAILED(result)) {
            log.Write(
                std::string("[VR][input] bindings unavailable for ") + profileText +
                ": " + dispatch_.ResultText(instance_, result));
            return false;
        }
        return true;
    };

    const std::array<const char*, 2> aimPaths{
        "/user/hand/left/input/aim/pose",
        "/user/hand/right/input/aim/pose"};
    const std::array<const char*, 2> triggerValuePaths{
        "/user/hand/left/input/trigger/value",
        "/user/hand/right/input/trigger/value"};
    const bool touchPause = livePauseAction_ != XR_NULL_HANDLE;
    const bool touchCamera = cameraModeAction_ != XR_NULL_HANDLE &&
        cameraCharaAction_ != XR_NULL_HANDLE &&
        cameraResetAction_ != XR_NULL_HANDLE;
    const bool touchA = panelAdjustAction_ != XR_NULL_HANDLE;
    const bool wantGripPose = gripPoseAction_ != XR_NULL_HANDLE;
    auto suggestModernLadder = [&](const char* profileText,
                                   const FaceButtons& faceButtons) {
        struct ModernSuggest {
            bool full = false;
            bool camera = false;
            bool gripPause = false;
            bool grip = false;
            bool any = false;
        };
        ModernSuggest result{};
        result.full = touchCamera && touchA && suggestProfile(
            profileText,
            aimPaths,
            triggerValuePaths,
            true,
            true,
            touchPause,
            true,
            true,
            wantGripPose,
            faceButtons);
        result.camera = result.full ||
            (touchCamera && suggestProfile(
                profileText,
                aimPaths,
                triggerValuePaths,
                true,
                true,
                touchPause,
                true,
                false,
                wantGripPose,
                faceButtons));
        result.gripPause = result.camera || suggestProfile(
            profileText,
            aimPaths,
            triggerValuePaths,
            true,
            true,
            touchPause,
            false,
            false,
            wantGripPose,
            faceButtons);
        result.grip = result.gripPause || suggestProfile(
            profileText,
            aimPaths,
            triggerValuePaths,
            true,
            true,
            false,
            false,
            false,
            wantGripPose,
            faceButtons);
        result.any = result.grip || suggestProfile(
            profileText,
            aimPaths,
            triggerValuePaths,
            true,
            false,
            false,
            false,
            false,
            false,
            faceButtons);
        return result;
    };

    const auto touchSuggestedSet = suggestModernLadder(
        "/interaction_profiles/oculus/touch_controller",
        touchFaceButtons);
    const auto indexSuggestedSet = suggestModernLadder(
        "/interaction_profiles/valve/index_controller",
        indexFaceButtons);
    const auto pico4SuggestedSet = suggestModernLadder(
        "/interaction_profiles/bytedance/pico4_controller",
        touchFaceButtons);
    const auto picoNeo3SuggestedSet = suggestModernLadder(
        "/interaction_profiles/bytedance/pico_neo3_controller",
        touchFaceButtons);
    // Pico 4 Ultra sometimes exposes this unofficial sibling; fail-soft.
    const auto pico4sSuggestedSet = suggestModernLadder(
        "/interaction_profiles/bytedance/pico4s_controller",
        touchFaceButtons);
    // G3 has no analog squeeze or face buttons. Pointer + stick only.
    const bool picoG3Suggested = suggestProfile(
        "/interaction_profiles/bytedance/pico_g3_controller",
        aimPaths,
        triggerValuePaths,
        true,
        false,
        false,
        false,
        false,
        false,
        touchFaceButtons);
    const bool simpleSuggested = suggestProfile(
        "/interaction_profiles/khr/simple_controller",
        aimPaths,
        {"/user/hand/left/input/select/click", "/user/hand/right/input/select/click"},
        false,
        false,
        false,
        false,
        false,
        false,
        touchFaceButtons);

    const bool picoSuggested = pico4SuggestedSet.any ||
        picoNeo3SuggestedSet.any ||
        pico4sSuggestedSet.any ||
        picoG3Suggested;

    inputActionsCreated_ = true;
    lastResult_ = XR_SUCCESS;
    log.Write(
        std::string("[VR][input] dual-controller actions ready; touch=") +
        (touchSuggestedSet.any ? "1" : "0") + " simple=" +
        (simpleSuggested ? "1" : "0") + " index=" +
        (indexSuggestedSet.any ? "1" : "0") + " pico=" +
        (picoSuggested ? "1" : "0") + " grip=" +
        (touchSuggestedSet.grip ? "1" : "0") + " gripPose=" +
        (wantGripPose && touchSuggestedSet.grip ? "1" : "0") + " pause=" +
        (touchSuggestedSet.gripPause ? "1" : "0") + " camera=" +
        (touchSuggestedSet.camera ? "1" : "0") + " aButton=" +
        (touchSuggestedSet.full ? "1" : "0"));
    return true;
}

bool OpenXrContext::AttachInputActions(VrLog& log) {
    if (!inputActionsCreated_ || session_ == XR_NULL_HANDLE ||
        dispatch_.AttachSessionActionSets() == nullptr ||
        dispatch_.CreateActionSpace() == nullptr) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        return false;
    }

    const XrActionSet actionSets[]{inputActionSet_};
    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = actionSets;
    lastResult_ = dispatch_.AttachSessionActionSets()(session_, &attachInfo);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][input] xrAttachSessionActionSets failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    for (std::size_t hand = 0; hand < aimSpaces_.size(); ++hand) {
        XrActionSpaceCreateInfo createInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        createInfo.action = aimPoseAction_;
        createInfo.subactionPath = handPaths_[hand];
        createInfo.poseInActionSpace.orientation.w = 1.0F;
        lastResult_ = dispatch_.CreateActionSpace()(session_, &createInfo, &aimSpaces_[hand]);
        if (XR_FAILED(lastResult_)) {
            aimSpaces_[hand] = XR_NULL_HANDLE;
            const XrResult setupResult = lastResult_;
            log.Write(
                "[VR][input] xrCreateActionSpace failed: " +
                dispatch_.ResultText(instance_, lastResult_));
            ResetInputSession();
            lastResult_ = setupResult;
            return false;
        }
    }

    if (gripPoseAction_ != XR_NULL_HANDLE) {
        for (std::size_t hand = 0; hand < gripSpaces_.size(); ++hand) {
            XrActionSpaceCreateInfo createInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            createInfo.action = gripPoseAction_;
            createInfo.subactionPath = handPaths_[hand];
            createInfo.poseInActionSpace.orientation.w = 1.0F;
            lastResult_ = dispatch_.CreateActionSpace()(
                session_, &createInfo, &gripSpaces_[hand]);
            if (XR_FAILED(lastResult_)) {
                gripSpaces_[hand] = XR_NULL_HANDLE;
                log.Write(
                    "[VR][input] grip action space unavailable; hand=" +
                    std::to_string(hand) + " " +
                    dispatch_.ResultText(instance_, lastResult_));
                lastResult_ = XR_SUCCESS;
            }
        }
    }

    triggerHeld_.fill(false);
    mirrorPointerSmoothers_ = {};
    menuPointerSmoothers_ = {};
    pointerFilterTime_.fill(0);
    pointerFilterLayoutGeneration_ = 0;
    pointerSmoothLogged_ = false;
    gripStateInitialized_.fill(false);
    gripHeld_.fill(false);
    gripGestureValid_.fill(false);
    gripLongPressFired_.fill(false);
    gripPressTime_.fill(0);
    gripToggleCooldownUntil_ = 0;
    livePauseHeld_ = false;
    livePauseInactiveSince_ = 0;
    livePauseToggleCount_.store(0U, std::memory_order_release);
    cameraModeHeld_ = false;
    cameraCharaHeld_ = false;
    cameraResetHeld_ = false;
    cameraResetFired_ = false;
    cameraResetPressTime_ = 0;
    stereoUiPanelVisible_ = false;
    aaMenuVisible_ = false;
    menuPointerHand_ = 1;
    stereoSceneEligible_ = false;
    mirrorInputStateInitialized_ = false;
    inputErrorLogged_ = false;
    inputSessionReady_ = true;
    lastResult_ = XR_SUCCESS;
    log.Write(
        std::string("[VR][input] left/right aim spaces attached gripPose=") +
        (gripSpaces_[0] != XR_NULL_HANDLE || gripSpaces_[1] != XR_NULL_HANDLE
             ? "1"
             : "0"));
    return true;
}

void OpenXrContext::SyncPointerInput(
    std::array<PointerState, 2>& pointers,
    XrTime displayTime,
    VrLog& log) {
    pointers = {};
    if (pointerFilterLayoutGeneration_ != mirrorLayoutGeneration_) {
        pointerFilterLayoutGeneration_ = mirrorLayoutGeneration_;
        mirrorPointerSmoothers_ = {};
        pointerFilterTime_.fill(0);
    }
    if (!inputSessionReady_ || viewSpace_ == XR_NULL_HANDLE ||
        dispatch_.SyncActions() == nullptr || dispatch_.GetActionStatePose() == nullptr ||
        dispatch_.GetActionStateFloat() == nullptr ||
        dispatch_.GetActionStateVector2f() == nullptr ||
        dispatch_.LocateSpace() == nullptr) {
        pose::HandTrackingMailbox().Invalidate();
        return;
    }

    const XrActiveActionSet activeSet{inputActionSet_, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeSet;
    const XrResult syncResult = dispatch_.SyncActions()(session_, &syncInfo);
    if (XR_FAILED(syncResult)) {
        if (!inputErrorLogged_) {
            log.Write(
                "[VR][input] xrSyncActions failed: " +
                dispatch_.ResultText(instance_, syncResult));
            inputErrorLogged_ = true;
        }
        triggerHeld_.fill(false);
        livePauseHeld_ = false;
        pose::HandTrackingMailbox().Invalidate();
        return;
    }

    constexpr XrSpaceLocationFlags kRequiredLocationFlags =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;

    for (std::size_t hand = 0; hand < pointers.size(); ++hand) {
        auto& pointer = pointers[hand];
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.subactionPath = handPaths_[hand];

        XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
        getInfo.action = aimPoseAction_;
        const XrResult poseResult =
            dispatch_.GetActionStatePose()(session_, &getInfo, &poseState);

        XrActionStateFloat triggerState{XR_TYPE_ACTION_STATE_FLOAT};
        getInfo.action = triggerAction_;
        const XrResult triggerResult =
            dispatch_.GetActionStateFloat()(session_, &getInfo, &triggerState);

        XrActionStateFloat gripState{XR_TYPE_ACTION_STATE_FLOAT};
        XrResult gripResult = XR_SUCCESS;
        if (gripAction_ != XR_NULL_HANDLE) {
            getInfo.action = gripAction_;
            gripResult =
                dispatch_.GetActionStateFloat()(session_, &getInfo, &gripState);
        }

        XrActionStateVector2f thumbstickState{XR_TYPE_ACTION_STATE_VECTOR2F};
        getInfo.action = thumbstickAction_;
        const XrResult thumbstickResult =
            dispatch_.GetActionStateVector2f()(session_, &getInfo, &thumbstickState);
        if (XR_FAILED(poseResult) || XR_FAILED(triggerResult)) {
            if (!inputErrorLogged_) {
                const XrResult result = XR_FAILED(poseResult) ? poseResult : triggerResult;
                log.Write(
                    "[VR][input] controller action query failed: " +
                    dispatch_.ResultText(instance_, result));
                inputErrorLogged_ = true;
            }
            triggerHeld_[hand] = false;
            continue;
        }

        pointer.poseActive = poseState.isActive == XR_TRUE;
        pointer.triggerValue =
            triggerState.isActive == XR_TRUE ? triggerState.currentState : 0.0F;
        const bool wasHeld = triggerHeld_[hand];
        if (wasHeld) {
            triggerHeld_[hand] = pointer.triggerValue > kTriggerReleaseThreshold;
        } else {
            triggerHeld_[hand] = pointer.triggerValue >= kTriggerPressThreshold;
        }
        pointer.triggerPressed = !wasHeld && triggerHeld_[hand];
        pointer.triggerHeld = triggerHeld_[hand];
        if (gripAction_ != XR_NULL_HANDLE && XR_SUCCEEDED(gripResult) &&
            gripState.isActive == XR_TRUE) {
            pointer.gripActive = true;
            pointer.gripValue = gripState.currentState;
        }
        if (XR_SUCCEEDED(thumbstickResult) && thumbstickState.isActive == XR_TRUE) {
            pointer.thumbstickActive = true;
            pointer.thumbstick = thumbstickState.currentState;
        } else if (XR_FAILED(thumbstickResult) && !inputErrorLogged_) {
            log.Write(
                "[VR][input] thumbstick action query failed: " +
                dispatch_.ResultText(instance_, thumbstickResult));
            inputErrorLogged_ = true;
        }

        auto resetHandFilter = [&]() {
            mirrorPointerSmoothers_[hand].Reset();
            menuPointerSmoothers_[hand].Reset();
            pointerFilterTime_[hand] = 0;
        };

        if (!pointer.poseActive || aimSpaces_[hand] == XR_NULL_HANDLE) {
            resetHandFilter();
            continue;
        }

        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const XrResult locateResult = dispatch_.LocateSpace()(
            aimSpaces_[hand], viewSpace_, displayTime, &location);
        if (XR_FAILED(locateResult) ||
            !HasSpaceFlags(location.locationFlags, kRequiredLocationFlags)) {
            resetHandFilter();
            continue;
        }

        pointer.poseValid = true;
        pointer.aimPose = location.pose;
        const XrVector3f direction = AimDirection(location.pose.orientation);

        // Oriented-quad hits: the game panel follows the adjustable placement
        // (pinned placements were re-expressed in VIEW space this frame); the
        // settings menu stays hard head-locked.
        const pose::Vector3 rayOrigin = ToPoseVector(location.pose.position);
        const pose::Vector3 rayDirection = ToPoseVector(direction);
        pointer.hovering = panel::RayQuadUv(
            panelPoseView_,
            panelQuadWidth_,
            panelQuadHeight_,
            rayOrigin,
            rayDirection,
            pointer.u,
            pointer.v);
        const pose::Pose menuPose{{0.0F, 0.0F, kMenuPlaneZ}, {}};
        pointer.menuHovering = panel::RayQuadUv(
            menuPose,
            kMenuWidthMetres,
            kMenuHeightMetres,
            rayOrigin,
            rayDirection,
            pointer.menuU,
            pointer.menuV);
        if (panelAdjustMode_) {
            pointer.barHovering = panel::RayQuadUv(
                barPoseView_,
                barQuadWidth_,
                barQuadHeight_,
                rayOrigin,
                rayDirection,
                pointer.barU,
                pointer.barV);
        }

        float dtSeconds = 0.0F;
        if (pointerFilterTime_[hand] != 0 && displayTime > pointerFilterTime_[hand]) {
            dtSeconds = static_cast<float>(
                static_cast<double>(displayTime - pointerFilterTime_[hand]) *
                1.0e-9);
            dtSeconds = std::min(dtSeconds, 0.10F);
        }
        pointerFilterTime_[hand] = displayTime;

        const bool smoothEnabled = GakumasLocal::Config::vrPointerSmoothEnabled;
        const float minCutoff = GakumasLocal::Config::vrPointerSmoothMinCutoff;
        const float beta = GakumasLocal::Config::vrPointerSmoothBeta;
        if (!pointerSmoothLogged_) {
            pointerSmoothLogged_ = true;
            std::ostringstream stream;
            stream << "[VR][input] POINTER_SMOOTH enabled="
                   << (smoothEnabled ? "1" : "0")
                   << " minCutoff=" << minCutoff << " beta=" << beta;
            log.Write(stream.str());
        }
        if (!smoothEnabled) {
            mirrorPointerSmoothers_[hand].Reset();
            menuPointerSmoothers_[hand].Reset();
            continue;
        }
        // The press frame stays filtered so a tap lands on the stable
        // hover point. Once the trigger is held, follow raw aim so a
        // short swipe still clears the Windows tap/drag radius.
        const bool liveDrag = pointer.triggerHeld && !pointer.triggerPressed;
        if (pointer.hovering && !liveDrag) {
            mirrorPointerSmoothers_[hand].Filter(
                true, pointer.u, pointer.v, dtSeconds, minCutoff, beta,
                pointer.u, pointer.v);
        } else {
            mirrorPointerSmoothers_[hand].Reset();
        }
        if (pointer.menuHovering && !liveDrag) {
            menuPointerSmoothers_[hand].Filter(
                true,
                pointer.menuU,
                pointer.menuV,
                dtSeconds,
                minCutoff,
                beta,
                pointer.menuU,
                pointer.menuV);
        } else {
            menuPointerSmoothers_[hand].Reset();
        }
    }

    const XrSpace trackingSpace =
        stageSpace_ != XR_NULL_HANDLE ? stageSpace_ : localSpace_;
    pose::HandPoseSample handSample{};
    if (trackingSpace != XR_NULL_HANDLE) {
        for (std::size_t hand = 0; hand < pointers.size(); ++hand) {
            auto& pointer = pointers[hand];
            auto locateHand = [&](XrSpace space, XrPosef& outPose) -> bool {
                if (space == XR_NULL_HANDLE) {
                    return false;
                }
                XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
                const XrResult locateResult = dispatch_.LocateSpace()(
                    space, trackingSpace, displayTime, &location);
                if (XR_FAILED(locateResult) ||
                    !HasSpaceFlags(
                        location.locationFlags, kRequiredLocationFlags)) {
                    return false;
                }
                outPose = location.pose;
                return true;
            };

            XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
            getInfo.subactionPath = handPaths_[hand];
            bool usedGrip = false;
            XrPosef trackingPose{};
            if (gripPoseAction_ != XR_NULL_HANDLE &&
                gripSpaces_[hand] != XR_NULL_HANDLE) {
                XrActionStatePose gripPoseState{XR_TYPE_ACTION_STATE_POSE};
                getInfo.action = gripPoseAction_;
                const XrResult gripPoseResult = dispatch_.GetActionStatePose()(
                    session_, &getInfo, &gripPoseState);
                if (XR_SUCCEEDED(gripPoseResult) &&
                    gripPoseState.isActive == XR_TRUE &&
                    locateHand(gripSpaces_[hand], trackingPose)) {
                    usedGrip = true;
                }
            }
            if (!usedGrip && !locateHand(aimSpaces_[hand], trackingPose)) {
                continue;
            }
            pointer.gripPoseValid = true;
            pointer.usedGripPose = usedGrip;
            pointer.gripPose = trackingPose;
            handSample.hands[hand].valid = true;
            handSample.hands[hand].usedGrip = usedGrip;
            handSample.hands[hand].pose = ToTrackingPose(trackingPose);
        }
    }
    pose::HandTrackingMailbox().Publish(handSample);

    PollLivePauseButton(displayTime, log);
    PollCameraButtons(displayTime, log);
    PollPanelAdjustButton(displayTime, log);
    UpdatePanelAdjustInteractions(pointers, displayTime, log);
}

void OpenXrContext::PollLivePauseButton(XrTime displayTime, VrLog& log) {
    if (livePauseAction_ == XR_NULL_HANDLE ||
        dispatch_.GetActionStateBoolean() == nullptr) {
        return;
    }

    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = livePauseAction_;
    getInfo.subactionPath = XR_NULL_PATH;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    const XrResult result =
        dispatch_.GetActionStateBoolean()(session_, &getInfo, &state);
    if (XR_FAILED(result)) {
        if (!inputErrorLogged_) {
            log.Write(
                "[VR][live] pause button query failed: " +
                dispatch_.ResultText(instance_, result));
            inputErrorLogged_ = true;
        }
        livePauseHeld_ = false;
        return;
    }
    if (state.isActive != XR_TRUE) {
        if (livePauseInactiveSince_ == 0) {
            livePauseInactiveSince_ = displayTime;
        }
        constexpr XrDuration kInactiveClear = 500'000'000;
        if (displayTime - livePauseInactiveSince_ > kInactiveClear) {
            livePauseHeld_ = false;
        }
        return;
    }
    livePauseInactiveSince_ = 0;

    const bool pressed = state.currentState == XR_TRUE;
    // Rising edge only: no inter-press cooldown. Each distinct press
    // queues one toggle so rapid B taps behave like a media player.
    if (pressed && !livePauseHeld_) {
        livePauseToggleCount_.fetch_add(1U, std::memory_order_acq_rel);
        log.Write("[VR][live] pause toggle requested source=right-b");
    }
    livePauseHeld_ = pressed;
}

void OpenXrContext::PollCameraButtons(XrTime displayTime, VrLog& log) {
    if (dispatch_.GetActionStateBoolean() == nullptr) {
        return;
    }

    const auto readPressed = [&](XrAction action, bool& pressed) {
        if (action == XR_NULL_HANDLE) {
            return false;
        }
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
        getInfo.action = action;
        getInfo.subactionPath = XR_NULL_PATH;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        const XrResult result =
            dispatch_.GetActionStateBoolean()(session_, &getInfo, &state);
        if (XR_FAILED(result) || state.isActive != XR_TRUE) {
            return false;
        }
        pressed = state.currentState == XR_TRUE;
        return true;
    };

    bool pressed = false;
    if (readPressed(cameraModeAction_, pressed)) {
        if (pressed && !cameraModeHeld_) {
            cameraModePressCount_.fetch_add(1U, std::memory_order_acq_rel);
            log.Write("[VR][camera] FREECAM_BUTTON mode source=left-x");
        }
        cameraModeHeld_ = pressed;
    } else {
        cameraModeHeld_ = false;
    }

    pressed = false;
    if (readPressed(cameraCharaAction_, pressed)) {
        if (pressed && !cameraCharaHeld_) {
            cameraCharaPressCount_.fetch_add(1U, std::memory_order_acq_rel);
            log.Write("[VR][camera] FREECAM_BUTTON chara source=left-y");
        }
        cameraCharaHeld_ = pressed;
    } else {
        cameraCharaHeld_ = false;
    }

    // Reset fires once after the left thumbstick click is held for 0.3s, so
    // an accidental tap while steering never resets the camera.
    constexpr XrDuration kResetHoldDuration = 300'000'000;
    pressed = false;
    if (readPressed(cameraResetAction_, pressed) && pressed) {
        if (!cameraResetHeld_) {
            cameraResetHeld_ = true;
            cameraResetFired_ = false;
            cameraResetPressTime_ = displayTime;
        } else if (!cameraResetFired_ && displayTime >= cameraResetPressTime_ &&
                   displayTime - cameraResetPressTime_ >= kResetHoldDuration) {
            cameraResetFired_ = true;
            if (panelAdjustMode_) {
                // Inside adjust mode the same hold recentres the game panel
                // instead of the free camera.
                ResetPanelPlacement(log);
            } else {
                cameraResetPressCount_.fetch_add(1U, std::memory_order_acq_rel);
                log.Write(
                    "[VR][camera] FREECAM_BUTTON reset source=left-stick-long-press");
            }
        }
    } else {
        cameraResetHeld_ = false;
        cameraResetFired_ = false;
        cameraResetPressTime_ = 0;
    }
}

void OpenXrContext::PollPanelAdjustButton(XrTime displayTime, VrLog& log) {
    if (panelAdjustAction_ == XR_NULL_HANDLE ||
        dispatch_.GetActionStateBoolean() == nullptr) {
        return;
    }

    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = panelAdjustAction_;
    getInfo.subactionPath = XR_NULL_PATH;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    const XrResult result =
        dispatch_.GetActionStateBoolean()(session_, &getInfo, &state);
    if (XR_FAILED(result) || state.isActive != XR_TRUE) {
        panelAdjustButtonHeld_ = false;
        return;
    }

    const bool pressed = state.currentState == XR_TRUE;
    if (pressed && !panelAdjustButtonHeld_) {
        // Route at press time: the photo scene flag is refreshed every frame
        // in UpdatePanelPlacementFrame, but a fresh read keeps the edge and
        // the routing consistent.
        photoSceneActive_ = PhotoSceneActive();
        if (panel::RouteAButton(photoSceneActive_) ==
            panel::AButtonRoute::PhotoShutter) {
            if (panelAdjustMode_) {
                SetPanelAdjustMode(false, "photo-scene-press", log);
            }
            // Pause-2D freezes the source view the official shutter captures.
            // Block the Right-A shortcut only; a Grip UI click still reaches
            // the game's own photo button.
            if (GakumasLocal::Config::vrSourceCameraTiny) {
                panelToastKind_ = 3;
                panelToastUntil_ = displayTime + kPanelToastDuration;
                photoResultPendingUntil_ = 0;
                log.Write(
                    "[VR][photo] PHOTO_BUTTON_BLOCKED source=right-a "
                    "reason=paused-2d");
            } else {
                photoPressCount_.fetch_add(1U, std::memory_order_acq_rel);
                photoResultPendingUntil_ = displayTime + kPhotoResultTimeout;
                log.Write("[VR][photo] PHOTO_BUTTON source=right-a");
            }
        } else {
            SetPanelAdjustMode(!panelAdjustMode_, "right-a", log);
        }
    }
    panelAdjustButtonHeld_ = pressed;
}

void OpenXrContext::SetPanelAdjustMode(
    bool active, const char* reason, VrLog& log) {
    if (panelAdjustMode_ == active) {
        return;
    }
    panelAdjustMode_ = active;
    panelAdjustItem_ = 0;
    panelGrabActive_.fill(false);
    panelHandLastValid_.fill(false);
    panelScaleActive_ = false;
    if (active) {
        // The menu and the adjust bar share trigger/grip semantics; only one
        // may own the controllers.
        aaMenuVisible_ = false;
        if (aaMenuFlush_ != nullptr) {
            aaMenuFlush_(log);
        }
        stereoUiPanelVisible_ = true;
        panelBarPointerHand_ = 1;
    } else {
        SavePanelPlacementIfDirty(log);
    }
    log.Write(
        std::string("[VR][panel] PANEL_ADJUST mode=") + (active ? "1" : "0") +
        " reason=" + (reason != nullptr ? reason : "unknown"));
}

void OpenXrContext::TogglePanelPin(VrLog& log) {
    if (!panelPlacement_.pinned) {
        panelPlacement_.pinned = true;
        panelPlacement_.customized = true;
        if (headPoseBaseValid_) {
            panelOffsetBase_ = panel::ViewOffsetToBase(
                headPoseBase_, panelPlacement_.offset);
            panelPinnedOrientationBase_ = panel::CapturedPanelOrientation(
                headPoseBase_, panelPlacement_.offset);
            panelPinnedAnchorValid_ = true;
        } else {
            panelPinnedAnchorValid_ = false;
        }
    } else {
        if (panelPinnedAnchorValid_ && headPoseBaseValid_) {
            panelPlacement_.offset = panel::ClampPanelOffset(
                panel::BaseOffsetToView(headPoseBase_, panelOffsetBase_));
        }
        panelPlacement_.pinned = false;
        panelPinnedAnchorValid_ = false;
    }
    panelPlacementDirty_ = true;
    log.Write(
        std::string("[VR][panel] PANEL_PIN pinned=") +
        (panelPlacement_.pinned ? "1" : "0") + " anchored=" +
        (panelPinnedAnchorValid_ ? "1" : "0"));
}

void OpenXrContext::ResetPanelPlacement(VrLog& log) {
    panelPlacement_ = panel::Placement{};
    panelPinnedAnchorValid_ = false;
    panelAdjustItem_ = 0;
    panelGrabActive_.fill(false);
    panelHandLastValid_.fill(false);
    panelScaleActive_ = false;
    panelPlacementDirty_ = true;
    log.Write("[VR][panel] PANEL_PLACEMENT_RESET source=left-stick-long-press");
}

void OpenXrContext::LoadPanelPlacementFromConfig() noexcept {
    namespace Config = GakumasLocal::Config;
    panelPlacement_.customized = Config::vrPanelCustomized;
    panelPlacement_.pinned = Config::vrPanelPinned;
    panelPlacement_.offset = panel::ClampPanelOffset({
        Config::vrPanelOffsetX,
        Config::vrPanelOffsetY,
        Config::vrPanelOffsetZ,
    });
    panelPlacement_.width = panel::ClampPanelWidth(Config::vrPanelWidth);
    panelPinnedAnchorValid_ = false;
}

void OpenXrContext::SavePanelPlacementIfDirty(VrLog& log) {
    if (!panelPlacementDirty_) {
        return;
    }
    panelPlacementDirty_ = false;
    namespace Config = GakumasLocal::Config;
    Config::vrPanelCustomized = panelPlacement_.customized;
    Config::vrPanelPinned = panelPlacement_.pinned;
    Config::vrPanelOffsetX = panelPlacement_.offset.x;
    Config::vrPanelOffsetY = panelPlacement_.offset.y;
    Config::vrPanelOffsetZ = panelPlacement_.offset.z;
    Config::vrPanelWidth = panelPlacement_.width;
    Config::SaveVrConfig(VrConfigJson);
    std::ostringstream saved;
    saved << "[VR][panel] PANEL_PLACEMENT_SAVED customized="
          << (panelPlacement_.customized ? 1 : 0)
          << " pinned=" << (panelPlacement_.pinned ? 1 : 0)
          << " offset=(" << panelPlacement_.offset.x << ","
          << panelPlacement_.offset.y << "," << panelPlacement_.offset.z
          << ") width=" << panelPlacement_.width;
    log.Write(saved.str());
}

void OpenXrContext::UpdatePanelPlacementFrame(XrTime displayTime, VrLog& log) {
    if (!panelPlacementLoaded_) {
        LoadPanelPlacementFromConfig();
        panelPlacementLoaded_ = true;
    }

    // Scene routing: a rising photo scene ends adjust mode (panel returns to
    // the default head-locked front in photo scenes).
    const bool photoScene = PhotoSceneActive();
    if (photoScene != photoSceneActive_) {
        photoSceneActive_ = photoScene;
        if (photoScene && panelAdjustMode_) {
            SetPanelAdjustMode(false, "photo-scene", log);
        }
    }

    // Shutter feedback toast. Success events come from the game's capture
    // animation; a press that produced no event by the deadline was silently
    // ignored (50-shot limit) and toasts "unavailable".
    std::uint32_t resultGeneration = 0;
    std::uint32_t resultCode = 0;
    ReadPhotoShutterResult(resultGeneration, resultCode);
    if (!photoResultGenerationInitialized_) {
        lastPhotoResultGeneration_ = resultGeneration;
        photoResultGenerationInitialized_ = true;
    } else if (resultGeneration != lastPhotoResultGeneration_) {
        lastPhotoResultGeneration_ = resultGeneration;
        if (resultCode == kPhotoShutterTaken) {
            panelToastKind_ = 1;
        } else if (resultCode == kPhotoShutterPaused2D) {
            panelToastKind_ = 3;
        } else {
            panelToastKind_ = 2;
        }
        panelToastUntil_ = displayTime + kPanelToastDuration;
        photoResultPendingUntil_ = 0;
    }
    if (photoResultPendingUntil_ != 0 &&
        displayTime >= photoResultPendingUntil_) {
        photoResultPendingUntil_ = 0;
        panelToastKind_ = 2;
        panelToastUntil_ = displayTime + kPanelToastDuration;
        log.Write("[VR][photo] PHOTO_RESULT_TIMEOUT toast=unavailable");
    }
    if (panelToastKind_ != 0 &&
        (panelToastUntil_ == 0 || displayTime >= panelToastUntil_)) {
        panelToastKind_ = 0;
        panelToastUntil_ = 0;
    }

    // Head pose in the projection base space (pin math + view re-expression).
    headPoseBaseValid_ = false;
    XrSpace baseSpace =
        stageSpace_ != XR_NULL_HANDLE ? stageSpace_ : localSpace_;
    if (baseSpace != XR_NULL_HANDLE && viewSpace_ != XR_NULL_HANDLE &&
        dispatch_.LocateSpace() != nullptr) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags kRequiredFlags =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (XR_SUCCEEDED(dispatch_.LocateSpace()(
                viewSpace_, baseSpace, displayTime, &location)) &&
            HasSpaceFlags(location.locationFlags, kRequiredFlags)) {
            headPoseBase_.position = ToPoseVector(location.pose.position);
            headPoseBase_.orientation = {
                location.pose.orientation.x,
                location.pose.orientation.y,
                location.pose.orientation.z,
                location.pose.orientation.w,
            };
            headPoseBaseValid_ = true;
        }
    }

    const bool useCustom = panelPlacement_.customized && !photoSceneActive_;
    const bool pinnedActive = useCustom && panelPlacement_.pinned;
    if (pinnedActive) {
        if (!panelPinnedAnchorValid_ && headPoseBaseValid_) {
            panelOffsetBase_ = panel::ViewOffsetToBase(
                headPoseBase_, panelPlacement_.offset);
            panelPinnedOrientationBase_ = panel::CapturedPanelOrientation(
                headPoseBase_, panelPlacement_.offset);
            panelPinnedAnchorValid_ = true;
            log.Write("[VR][panel] PANEL_PIN_ANCHORED source=frame");
        }
    } else {
        panelPinnedAnchorValid_ = false;
    }

    // Photo scenes pin the default panel dead ahead; the other scenes'
    // default is the tuned adjustable placement (same values a reset gives).
    panelQuadWidth_ = useCustom
        ? panelPlacement_.width
        : (photoSceneActive_ ? kMirrorWidthMetres
                             : panel::kDefaultAdjustWidthMetres);
    panelQuadHeight_ = mirrorWidth_ == 0
        ? 0.0F
        : panelQuadWidth_ * static_cast<float>(mirrorHeight_) /
            static_cast<float>(mirrorWidth_);

    const pose::Vector3 offsetView = useCustom
        ? panelPlacement_.offset
        : (photoSceneActive_
               ? pose::Vector3{0.0F, 0.0F, kMirrorPlaneZ}
               : pose::Vector3{
                     0.0F,
                     panel::kDefaultAdjustOffsetYMetres,
                     panel::kDefaultAdjustOffsetZMetres});
    if (pinnedActive && panelPinnedAnchorValid_ && headPoseBaseValid_) {
        panelPoseBase_ = panel::PinnedPanelPose(
            headPoseBase_, panelOffsetBase_, panelPinnedOrientationBase_);
        panelPoseView_ = panel::PoseInViewSpace(headPoseBase_, panelPoseBase_);
        panelPoseUsesBase_ = true;
    } else {
        panelPoseView_ =
            panel::PanelPoseFromOffset({0.0F, 0.0F, 0.0F}, offsetView);
        panelPoseUsesBase_ = false;
    }

    barQuadWidth_ = panelQuadWidth_ * kPanelBarWidthRatio;
    barQuadHeight_ = barQuadWidth_ *
        static_cast<float>(kPanelOverlayBarHeight) /
        static_cast<float>(kPanelOverlayTextureWidth);
    barPoseView_ = panel::AttachedBarPose(
        panelPoseView_, panelQuadHeight_, barQuadHeight_);
    if (panelPoseUsesBase_) {
        barPoseBase_ = panel::AttachedBarPose(
            panelPoseBase_, panelQuadHeight_, barQuadHeight_);
    }
    hintPoseView_ = panel::PanelPoseFromOffset(
        {0.0F, 0.0F, 0.0F}, {0.0F, -0.36F, -1.10F});
}

void OpenXrContext::UpdatePanelAdjustInteractions(
    const std::array<PointerState, 2>& pointers,
    XrTime displayTime,
    VrLog& log) {
    static_cast<void>(displayTime);
    if (!panelAdjustMode_) {
        panelGrabActive_.fill(false);
        panelHandLastValid_.fill(false);
        panelGrabAimValid_.fill(false);
        panelScaleActive_ = false;
        return;
    }

    std::array<pose::Vector3, 2> handView{};
    std::array<pose::Vector3, 2> aimDirView{};
    std::array<bool, 2> handValid{};
    for (std::size_t hand = 0; hand < pointers.size(); ++hand) {
        handValid[hand] = pointers[hand].poseValid;
        if (handValid[hand]) {
            handView[hand] = ToPoseVector(pointers[hand].aimPose.position);
            aimDirView[hand] = ToPoseVector(
                AimDirection(pointers[hand].aimPose.orientation));
        }
    }

    const bool editBase = panelPoseUsesBase_ && headPoseBaseValid_;
    // Pinned edits keep the captured orientation in sync with the offset so
    // the panel keeps facing the user exactly as displayed while editing.
    const auto syncPinnedFromBase = [&]() {
        if (headPoseBaseValid_) {
            panelPlacement_.offset = panel::ClampPanelOffset(
                panel::BaseOffsetToView(headPoseBase_, panelOffsetBase_));
            panelPinnedOrientationBase_ = panel::CapturedPanelOrientation(
                headPoseBase_, panelPlacement_.offset);
        }
    };
    const auto markEdited = [&]() {
        panelPlacement_.customized = true;
        panelPlacementDirty_ = true;
    };
    const auto applyHeight = [&](float amount) {
        if (editBase) {
            panelOffsetBase_ =
                panel::ApplyHeightDelta(panelOffsetBase_, amount);
            syncPinnedFromBase();
        } else {
            panelPlacement_.offset =
                panel::ApplyHeightDelta(panelPlacement_.offset, amount);
        }
        markEdited();
    };
    const auto applyDistance = [&](float amount) {
        if (editBase) {
            panelOffsetBase_ =
                panel::ApplyDistanceDelta(panelOffsetBase_, amount);
            syncPinnedFromBase();
        } else {
            panelPlacement_.offset =
                panel::ApplyDistanceDelta(panelPlacement_.offset, amount);
        }
        markEdited();
    };
    // Grab rotation: the panel follows the aim ray 1:1 in angle, so a far
    // panel stays exactly as responsive as a near one.
    const auto applyRotation = [&](const pose::Quaternion& rotationView) {
        if (editBase) {
            const pose::Quaternion rotationBase = pose::Multiply(
                pose::Multiply(headPoseBase_.orientation, rotationView),
                pose::Conjugate(headPoseBase_.orientation));
            panelOffsetBase_ = panel::ClampPanelOffset(
                pose::Rotate(rotationBase, panelOffsetBase_));
            syncPinnedFromBase();
        } else {
            panelPlacement_.offset = panel::ClampPanelOffset(
                pose::Rotate(rotationView, panelPlacement_.offset));
        }
        markEdited();
    };

    // --- VD-style item adjust: lateral (left/right) hand motion edits the
    // selected property, trigger confirms. Grabs pause while an item is
    // active; grip keeps its own forward/back logic. ---
    if (panelAdjustItem_ != 0) {
        panelGrabActive_.fill(false);
        panelGrabAimValid_.fill(false);
        panelScaleActive_ = false;
        const std::size_t hand = panelAdjustHand_ < pointers.size()
            ? panelAdjustHand_
            : 1U;
        if (pointers[hand].triggerPressed) {
            log.Write(
                "[VR][panel] PANEL_ADJUST_ITEM confirm item=" +
                std::to_string(panelAdjustItem_));
            panelAdjustItem_ = 0;
        } else if (handValid[hand] && panelHandLastValid_[hand]) {
            const pose::Vector3 delta =
                panel::VectorSubtract(handView[hand], panelHandLastView_[hand]);
            // VD-style axes: height = vertical, distance = lateral,
            // size = the up-right / down-left diagonal.
            const float lateral = delta.x;
            const float vertical = editBase
                ? pose::Rotate(headPoseBase_.orientation, delta).y
                : delta.y;
            switch (panelAdjustItem_) {
            case 1: // Height: up = higher.
                applyHeight(vertical * kPanelAdjustHeightGain);
                break;
            case 2: // Distance: right = farther.
                applyDistance(lateral * kPanelAdjustDistanceGain);
                break;
            case 3: // Size: up-right = bigger, down-left = smaller.
                panelPlacement_.width = panel::ApplySizeDelta(
                    panelPlacement_.width,
                    (lateral + delta.y) * kPanelAdjustSizeGain);
                markEdited();
                break;
            default:
                break;
            }
        }
        for (std::size_t hand2 = 0; hand2 < pointers.size(); ++hand2) {
            panelHandLastView_[hand2] = handView[hand2];
            panelHandLastValid_[hand2] = handValid[hand2];
        }
        return;
    }

    // --- Grip grab / two-hand scale. ---
    for (std::size_t hand = 0; hand < pointers.size(); ++hand) {
        const auto& pointer = pointers[hand];
        const bool pressed = pointer.gripActive &&
            (panelGrabActive_[hand]
                 ? pointer.gripValue > kGripReleaseThreshold
                 : pointer.gripValue >= kGripPressThreshold);
        if (!panelGrabActive_[hand] && pressed && handValid[hand]) {
            const std::size_t other = hand == 0 ? 1U : 0U;
            // First grab must point at the panel; the second hand may squeeze
            // anywhere to start the two-hand scale.
            if (pointer.hovering || panelGrabActive_[other]) {
                panelGrabActive_[hand] = true;
                panelHandLastView_[hand] = handView[hand];
                panelHandLastValid_[hand] = true;
                panelGrabLastAimDir_[hand] = aimDirView[hand];
                panelGrabAimValid_[hand] = true;
                log.Write(
                    std::string("[VR][panel] PANEL_GRAB start hand=") +
                    (hand == 0 ? "left" : "right"));
            }
        } else if (panelGrabActive_[hand] && !pressed) {
            panelGrabActive_[hand] = false;
            panelGrabAimValid_[hand] = false;
            log.Write(
                std::string("[VR][panel] PANEL_GRAB end hand=") +
                (hand == 0 ? "left" : "right"));
        }
    }

    const bool bothGrabbing = panelGrabActive_[0] && panelGrabActive_[1];
    if (bothGrabbing && handValid[0] && handValid[1]) {
        const float separation = panel::VectorLength(
            panel::VectorSubtract(handView[0], handView[1]));
        if (!panelScaleActive_) {
            panelScaleActive_ = true;
            panelScaleBaselineSeparation_ = separation;
            panelScaleBaselineWidth_ = panelPlacement_.customized
                ? panelPlacement_.width
                : panelQuadWidth_;
        }
        panelPlacement_.width = panel::ScaledPanelWidth(
            panelScaleBaselineWidth_,
            panelScaleBaselineSeparation_,
            separation);
        markEdited();
    } else {
        panelScaleActive_ = false;
        for (std::size_t hand = 0; hand < pointers.size(); ++hand) {
            if (!panelGrabActive_[hand] || !handValid[hand]) {
                continue;
            }
            // Angular follow: rotate the offset by the aim ray's rotation
            // since the previous frame (1:1 in angle at any distance).
            if (panelGrabAimValid_[hand]) {
                applyRotation(panel::RotationBetween(
                    panelGrabLastAimDir_[hand], aimDirView[hand]));
            }
            // Forward/back keeps the translational radial logic: hand motion
            // along the panel direction moves it closer or farther.
            if (panelHandLastValid_[hand]) {
                const float length =
                    panel::VectorLength(panelPoseView_.position);
                if (length > 0.0001F) {
                    const pose::Vector3 direction = panel::VectorScale(
                        panelPoseView_.position, 1.0F / length);
                    const float radial = panel::VectorDot(
                        panel::VectorSubtract(
                            handView[hand], panelHandLastView_[hand]),
                        direction);
                    if (radial != 0.0F) {
                        applyDistance(radial);
                    }
                }
            }
        }
    }

    for (std::size_t hand = 0; hand < pointers.size(); ++hand) {
        panelHandLastView_[hand] = handView[hand];
        panelHandLastValid_[hand] = handValid[hand];
        panelGrabLastAimDir_[hand] = aimDirView[hand];
        panelGrabAimValid_[hand] = panelGrabActive_[hand] && handValid[hand];
    }
}

bool OpenXrContext::EnsurePanelOverlaySwapchain(VrLog& log) {
    if (panelOverlaySwapchain_ != XR_NULL_HANDLE) {
        return true;
    }
    if (panelOverlaySwapchainFailed_ || session_ == XR_NULL_HANDLE ||
        sessionDevice_ == nullptr || sessionContext_ == nullptr ||
        dispatch_.EnumerateSwapchainFormats() == nullptr ||
        dispatch_.CreateSwapchain() == nullptr ||
        dispatch_.EnumerateSwapchainImages() == nullptr) {
        return false;
    }

    uint32_t formatCount = 0;
    if (XR_FAILED(dispatch_.EnumerateSwapchainFormats()(
            session_, 0, &formatCount, nullptr)) ||
        formatCount == 0) {
        panelOverlaySwapchainFailed_ = true;
        log.Write("[VR][panel] overlay swapchain format count query failed");
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    uint32_t written = formatCount;
    if (XR_FAILED(dispatch_.EnumerateSwapchainFormats()(
            session_,
            static_cast<uint32_t>(formats.size()),
            &written,
            formats.data()))) {
        panelOverlaySwapchainFailed_ = true;
        log.Write("[VR][panel] overlay swapchain format enumeration failed");
        return false;
    }
    formats.resize(std::min(written, static_cast<uint32_t>(formats.size())));

    const std::array<DXGI_FORMAT, 4> preferred{
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };
    panelOverlayFormat_ = 0;
    for (const DXGI_FORMAT candidate : preferred) {
        if (std::find(formats.begin(), formats.end(),
                      static_cast<int64_t>(candidate)) != formats.end()) {
            panelOverlayFormat_ = static_cast<int64_t>(candidate);
            break;
        }
    }
    if (panelOverlayFormat_ == 0) {
        panelOverlaySwapchainFailed_ = true;
        log.Write(
            "[VR][panel] no 8-bit RGBA swapchain format for the overlay quads");
        return false;
    }

    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.format = panelOverlayFormat_;
    createInfo.sampleCount = 1;
    createInfo.width = kPanelOverlayTextureWidth;
    createInfo.height = kPanelOverlayTextureHeight;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    XrResult result = dispatch_.CreateSwapchain()(
        session_, &createInfo, &panelOverlaySwapchain_);
    if (XR_FAILED(result)) {
        panelOverlaySwapchain_ = XR_NULL_HANDLE;
        panelOverlaySwapchainFailed_ = true;
        log.Write(
            "[VR][panel] overlay xrCreateSwapchain failed: " +
            dispatch_.ResultText(instance_, result));
        return false;
    }

    uint32_t imageCount = 0;
    result = dispatch_.EnumerateSwapchainImages()(
        panelOverlaySwapchain_, 0, &imageCount, nullptr);
    if (XR_FAILED(result) || imageCount == 0) {
        panelOverlaySwapchainFailed_ = true;
        log.Write("[VR][panel] overlay swapchain image count failed");
        ResetPanelOverlaySwapchain();
        return false;
    }
    panelOverlayImages_.assign(
        imageCount, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    written = imageCount;
    result = dispatch_.EnumerateSwapchainImages()(
        panelOverlaySwapchain_,
        imageCount,
        &written,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(
            panelOverlayImages_.data()));
    if (XR_FAILED(result) || written != imageCount) {
        panelOverlaySwapchainFailed_ = true;
        log.Write("[VR][panel] overlay swapchain image enumeration failed");
        ResetPanelOverlaySwapchain();
        return false;
    }
    for (const auto& image : panelOverlayImages_) {
        if (image.texture == nullptr) {
            panelOverlaySwapchainFailed_ = true;
            log.Write(
                "[VR][panel] runtime returned a null overlay swapchain texture");
            ResetPanelOverlaySwapchain();
            return false;
        }
    }

    panelOverlayWidth_ = kPanelOverlayTextureWidth;
    panelOverlayHeight_ = kPanelOverlayTextureHeight;
    panelOverlayPaintFailures_ = 0;
    log.Write(
        "[VR][panel] PANEL_OVERLAY_SWAPCHAIN_READY " +
        std::to_string(panelOverlayWidth_) + "x" +
        std::to_string(panelOverlayHeight_) + " format=" +
        std::to_string(panelOverlayFormat_) + " images=" +
        std::to_string(imageCount));
    return true;
}

bool OpenXrContext::RenderPanelOverlayFrame(
    const std::array<PointerState, 2>& pointers,
    VrLog& log) {
    if (panelOverlayPainter_ == nullptr || !EnsurePanelOverlaySwapchain(log) ||
        dispatch_.AcquireSwapchainImage() == nullptr ||
        dispatch_.WaitSwapchainImage() == nullptr ||
        dispatch_.ReleaseSwapchainImage() == nullptr) {
        return false;
    }

    // Bar cursor keeps the same sticky-owner semantics as the menu: a held
    // trigger cannot be stolen, a fresh press can take over, hover follows.
    std::size_t owner = panelBarPointerHand_ < pointers.size()
        ? panelBarPointerHand_
        : 1U;
    if (!(pointers[owner].barHovering && pointers[owner].triggerHeld)) {
        const std::array<std::size_t, 2> order{owner, owner == 0 ? 1U : 0U};
        bool assigned = false;
        for (const std::size_t hand : order) {
            if (pointers[hand].barHovering && pointers[hand].triggerPressed) {
                owner = hand;
                assigned = true;
                break;
            }
        }
        if (!assigned) {
            for (const std::size_t hand : order) {
                if (pointers[hand].barHovering) {
                    owner = hand;
                    break;
                }
            }
        }
    }
    panelBarPointerHand_ = owner;

    PanelOverlayInput input;
    input.adjustMode = panelAdjustMode_;
    input.pinned = panelPlacement_.pinned;
    input.activeItem = panelAdjustItem_;
    input.toast = panelToastKind_;
    input.hovering = pointers[owner].barHovering;
    input.u = pointers[owner].barU;
    input.v = pointers[owner].barV;
    input.triggerHeld =
        pointers[owner].barHovering && pointers[owner].triggerHeld;
    input.triggerPressed =
        pointers[owner].barHovering && pointers[owner].triggerPressed;
    if (panelAdjustItem_ != 0) {
        // While an item is being adjusted the trigger is the confirm button
        // (handled by UpdatePanelAdjustInteractions), never a bar click.
        input.triggerHeld = false;
        input.triggerPressed = false;
    }

    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t imageIndex = 0;
    XrResult result = dispatch_.AcquireSwapchainImage()(
        panelOverlaySwapchain_, &acquireInfo, &imageIndex);
    if (XR_FAILED(result) || imageIndex >= panelOverlayImages_.size()) {
        log.Write(
            "[VR][panel] overlay xrAcquireSwapchainImage failed: " +
            dispatch_.ResultText(instance_, result));
        return false;
    }

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    result = dispatch_.WaitSwapchainImage()(panelOverlaySwapchain_, &waitInfo);
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(result)) {
        dispatch_.ReleaseSwapchainImage()(panelOverlaySwapchain_, &releaseInfo);
        log.Write(
            "[VR][panel] overlay xrWaitSwapchainImage failed: " +
            dispatch_.ResultText(instance_, result));
        return false;
    }

    PanelOverlayOutput output;
    const bool painted = panelOverlayPainter_(
        sessionDevice_,
        sessionContext_,
        panelOverlayImages_[imageIndex].texture,
        panelOverlayWidth_,
        panelOverlayHeight_,
        input,
        output,
        log);
    result = dispatch_.ReleaseSwapchainImage()(panelOverlaySwapchain_, &releaseInfo);
    if (XR_FAILED(result)) {
        log.Write(
            "[VR][panel] overlay xrReleaseSwapchainImage failed: " +
            dispatch_.ResultText(instance_, result));
        return false;
    }

    // The hint quad crops to the fitted text box the painter reported.
    panelHintWidthPx_ = painted ? output.hintWidthPx : 0;
    panelHintHeightPx_ = painted ? output.hintHeightPx : 0;

    if (painted && panelAdjustMode_) {
        if (output.pinClicked) {
            TogglePanelPin(log);
        }
        if (output.clickedItem >= 1 && output.clickedItem <= 3) {
            if (panelAdjustItem_ == output.clickedItem) {
                panelAdjustItem_ = 0;
            } else {
                panelAdjustItem_ = output.clickedItem;
                panelAdjustHand_ = owner;
                panelHandLastValid_.fill(false);
                panelGrabActive_.fill(false);
                panelScaleActive_ = false;
            }
            log.Write(
                "[VR][panel] PANEL_ADJUST_ITEM select item=" +
                std::to_string(panelAdjustItem_) + " hand=" +
                (owner == 0 ? "left" : "right"));
        }
    }

    if (!painted) {
        ++panelOverlayPaintFailures_;
        if (panelOverlayPaintFailures_ == 1 ||
            panelOverlayPaintFailures_ == kPanelOverlayPaintFailureLimit) {
            log.Write(
                "[VR][panel] overlay paint failed count=" +
                std::to_string(panelOverlayPaintFailures_));
        }
        if (panelOverlayPaintFailures_ >= kPanelOverlayPaintFailureLimit) {
            SetPanelAdjustMode(false, "paint-failure", log);
            panelOverlayPaintFailures_ = 0;
        }
        return false;
    }
    panelOverlayPaintFailures_ = 0;
    return true;
}

void OpenXrContext::ResetPanelOverlaySwapchain() noexcept {
    panelOverlayImages_.clear();
    if (panelOverlaySwapchain_ != XR_NULL_HANDLE &&
        dispatch_.DestroySwapchain() != nullptr) {
        dispatch_.DestroySwapchain()(panelOverlaySwapchain_);
    }
    panelOverlaySwapchain_ = XR_NULL_HANDLE;
    panelOverlayWidth_ = 0;
    panelOverlayHeight_ = 0;
    panelOverlayFormat_ = 0;
    panelOverlaySwapchainFailed_ = false;
    panelOverlayPaintFailures_ = 0;
}

void OpenXrContext::UpdateStereoUiPanelState(
    const std::array<PointerState, 2>& pointers,
    bool stereoSceneEligible,
    bool projectionReady,
    bool mirrorReady,
    XrTime displayTime,
    StereoFrame& frame,
    VrLog& log) {
    if (stereoSceneEligible != stereoSceneEligible_) {
        stereoSceneEligible_ = stereoSceneEligible;
        // Scene role changed (2D fallback <-> stereo overlay); a live adjust
        // session would be manipulating a stale panel, so end it (saving any
        // dirty placement) together with the stereo-panel visibility reset.
        // The settings menu owns an independent swapchain and remains valid in
        // the 2D fallback, so a scene-role change must not dismiss it.
        SetPanelAdjustMode(false, "scene-eligibility", log);
        stereoUiPanelVisible_ = false;
        gripStateInitialized_.fill(false);
        gripHeld_.fill(false);
        gripGestureValid_.fill(false);
        gripLongPressFired_.fill(false);
        gripPressTime_.fill(0);
    }

    bool toggleRequested = false;
    bool longPressRequested = false;
    std::size_t toggleHand = pointers.size();
    std::size_t longPressHand = pointers.size();
    for (std::size_t hand = 0; hand < pointers.size(); ++hand) {
        const auto& pointer = pointers[hand];
        if (!pointer.gripActive) {
            gripStateInitialized_[hand] = false;
            gripHeld_[hand] = false;
            gripGestureValid_[hand] = false;
            gripLongPressFired_[hand] = false;
            gripPressTime_[hand] = 0;
            continue;
        }

        const bool pressedNow = gripHeld_[hand]
            ? pointer.gripValue > kGripReleaseThreshold
            : pointer.gripValue >= kGripPressThreshold;
        if (!gripStateInitialized_[hand]) {
            // Synchronize without producing an edge. Entering VR while Grip is
            // already held must never toggle the UI on its eventual release.
            gripStateInitialized_[hand] = true;
            gripHeld_[hand] = pressedNow;
            gripGestureValid_[hand] = false;
            gripLongPressFired_[hand] = false;
            gripPressTime_[hand] = pressedNow ? displayTime : 0;
            continue;
        }

        if (!gripHeld_[hand] && pressedNow) {
            gripHeld_[hand] = true;
            gripPressTime_[hand] = displayTime;
            // A long Grip opens settings over either the immersive projection
            // or the ordinary 2D fallback. Short Grip remains stereo-only in
            // the release branch below.
            gripGestureValid_[hand] = mirrorReady;
            gripLongPressFired_[hand] = false;
            continue;
        }

        if (gripHeld_[hand] && !pressedNow) {
            const XrDuration duration = displayTime >= gripPressTime_[hand]
                ? displayTime - gripPressTime_[hand]
                : 0;
            if (gripGestureValid_[hand] && stereoSceneEligible && projectionReady &&
                mirrorReady &&
                duration >= kGripMinimumPressDuration &&
                duration <= kGripMaximumPressDuration &&
                displayTime >= gripToggleCooldownUntil_) {
                toggleRequested = true;
                if (toggleHand >= pointers.size()) {
                    toggleHand = hand;
                }
            }
            gripHeld_[hand] = false;
            gripGestureValid_[hand] = false;
            gripLongPressFired_[hand] = false;
            gripPressTime_[hand] = 0;
        } else if (gripHeld_[hand] &&
                   (displayTime < gripPressTime_[hand] ||
                    displayTime - gripPressTime_[hand] >
                        kGripLongPressDuration)) {
            if (gripGestureValid_[hand] && !gripLongPressFired_[hand] &&
                mirrorReady &&
                displayTime >= gripPressTime_[hand] &&
                displayTime >= gripToggleCooldownUntil_) {
                longPressRequested = true;
                if (longPressHand >= pointers.size()) {
                    longPressHand = hand;
                }
                gripLongPressFired_[hand] = true;
            }
            gripGestureValid_[hand] = false;
        }

        if (!mirrorReady) {
            gripGestureValid_[hand] = false;
        }
    }

    // Adjust mode owns the grip (grab/scale); no panel or menu toggles until
    // A exits the mode.
    if (longPressRequested && !panelAdjustMode_) {
        aaMenuVisible_ = !aaMenuVisible_;
        if (aaMenuVisible_) {
            stereoUiPanelVisible_ = false;
            menuPointerHand_ = 1;
        } else if (aaMenuFlush_ != nullptr) {
            aaMenuFlush_(log);
        }
        gripToggleCooldownUntil_ = displayTime + kGripToggleCooldown;
        gripGestureValid_.fill(false);
        log.Write(
            std::string("[VR][ui] STEREO_AA_MENU visible=") +
            (aaMenuVisible_ ? "1" : "0") + " hand=" +
            (longPressHand == 0 ? "left" : "right") + " gesture=long-press");
    } else if (toggleRequested && !panelAdjustMode_) {
        if (aaMenuVisible_) {
            gripToggleCooldownUntil_ = displayTime + kGripToggleCooldown;
            gripGestureValid_.fill(false);
            log.Write(
                std::string("[VR][ui] STEREO_AA_MENU keep-open ignored-short-press hand=") +
                (toggleHand == 0 ? "left" : "right"));
        } else {
            stereoUiPanelVisible_ = !stereoUiPanelVisible_;
            gripToggleCooldownUntil_ = displayTime + kGripToggleCooldown;
            gripGestureValid_.fill(false);
            log.Write(
                std::string("[VR][ui] STEREO_UI_PANEL visible=") +
                (stereoUiPanelVisible_ ? "1" : "0") + " hand=" +
                (toggleHand == 0 ? "left" : "right") + " gesture=short-press");
        }
    }

    if (!stereoSceneEligible) {
        stereoUiPanelVisible_ = false;
    }
    // The menu lives on its own swapchain, so it no longer depends on the
    // mirror quad being ready. RunFrame narrows this to the painted result.
    frame.aaMenuVisible = aaMenuVisible_;
    frame.stereoUiPanelVisible =
        projectionReady && mirrorReady && stereoUiPanelVisible_ &&
        !frame.aaMenuVisible;
    frame.mirrorInputEnabled = mirrorReady && !aaMenuVisible_ &&
        !panelAdjustMode_ &&
        (!projectionReady || frame.stereoUiPanelVisible);
    frame.mirrorPresentationChanged = mirrorInputStateInitialized_ &&
        frame.mirrorInputEnabled != lastMirrorInputEnabled_;
    mirrorInputStateInitialized_ = true;
    lastMirrorInputEnabled_ = frame.mirrorInputEnabled;
}

bool OpenXrContext::CreateMirrorSwapchain(VrLog& log) {
    if (session_ == XR_NULL_HANDLE || sessionDevice_ == nullptr ||
        dispatch_.EnumerateSwapchainFormats() == nullptr ||
        dispatch_.CreateSwapchain() == nullptr ||
        dispatch_.EnumerateSwapchainImages() == nullptr) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        return false;
    }

    uint32_t formatCount = 0;
    lastResult_ = dispatch_.EnumerateSwapchainFormats()(
        session_, 0, &formatCount, nullptr);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][display] xrEnumerateSwapchainFormats failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    std::vector<int64_t> formats;
    for (int attempt = 0; attempt < 3; ++attempt) {
        formats.resize(formatCount);
        uint32_t written = formatCount;
        lastResult_ = dispatch_.EnumerateSwapchainFormats()(
            session_,
            static_cast<uint32_t>(formats.size()),
            &written,
            formats.data());
        if (lastResult_ == XR_ERROR_SIZE_INSUFFICIENT) {
            formatCount = written;
            continue;
        }
        if (XR_FAILED(lastResult_)) {
            log.Write(
                "[VR][display] swapchain format data call failed: " +
                dispatch_.ResultText(instance_, lastResult_));
            return false;
        }
        formats.resize(std::min(written, static_cast<uint32_t>(formats.size())));
        break;
    }
    if (lastResult_ == XR_ERROR_SIZE_INSUFFICIENT || formats.empty()) {
        lastResult_ = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        log.Write("[VR][display] runtime returned no stable swapchain format list");
        return false;
    }

    const bool bgraSource =
        AreCopyCompatibleFormats(mirrorSourceFormat_, DXGI_FORMAT_B8G8R8A8_UNORM);
    const std::array<DXGI_FORMAT, 2> preferredFormats = bgraSource
        ? std::array<DXGI_FORMAT, 2>{
              DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
              DXGI_FORMAT_B8G8R8A8_UNORM}
        : std::array<DXGI_FORMAT, 2>{
              DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
              DXGI_FORMAT_R8G8B8A8_UNORM};
    mirrorSwapchainFormat_ = 0;
    for (const DXGI_FORMAT preferred : preferredFormats) {
        const int64_t candidate = static_cast<int64_t>(preferred);
        if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) {
            mirrorSwapchainFormat_ = candidate;
            break;
        }
    }
    if (mirrorSwapchainFormat_ == 0) {
        lastResult_ = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        log.Write(
            "[VR][display] runtime exposes no copy-compatible format for Unity format=" +
            std::to_string(static_cast<int>(mirrorSourceFormat_)));
        return false;
    }

    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.format = mirrorSwapchainFormat_;
    createInfo.sampleCount = 1;
    createInfo.width = mirrorWidth_;
    createInfo.height = mirrorHeight_;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    lastResult_ = dispatch_.CreateSwapchain()(
        session_, &createInfo, &mirrorSwapchain_);
    if (XR_FAILED(lastResult_)) {
        mirrorSwapchain_ = XR_NULL_HANDLE;
        log.Write(
            "[VR][display] xrCreateSwapchain failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    uint32_t imageCount = 0;
    lastResult_ = dispatch_.EnumerateSwapchainImages()(
        mirrorSwapchain_, 0, &imageCount, nullptr);
    if (XR_FAILED(lastResult_) || imageCount == 0) {
        log.Write(
            "[VR][display] swapchain image count failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }
    mirrorImages_.assign(
        imageCount,
        XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    uint32_t written = imageCount;
    lastResult_ = dispatch_.EnumerateSwapchainImages()(
        mirrorSwapchain_,
        imageCount,
        &written,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(mirrorImages_.data()));
    if (XR_FAILED(lastResult_) || written != imageCount) {
        log.Write(
            "[VR][display] swapchain image enumeration failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    for (const auto& image : mirrorImages_) {
        if (image.texture == nullptr) {
            lastResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
            log.Write("[VR][display] runtime returned a null D3D11 swapchain texture");
            return false;
        }

        D3D11_TEXTURE2D_DESC textureDescription{};
        image.texture->GetDesc(&textureDescription);
        if (textureDescription.Width != mirrorWidth_ ||
            textureDescription.Height != mirrorHeight_ ||
            textureDescription.MipLevels != 1 || textureDescription.ArraySize != 1 ||
            textureDescription.SampleDesc.Count != 1 ||
            !AreCopyCompatibleFormats(
                textureDescription.Format,
                static_cast<DXGI_FORMAT>(mirrorSwapchainFormat_))) {
            lastResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
            log.Write(
                "[VR][display] runtime returned an incompatible mirror texture: size=" +
                std::to_string(textureDescription.Width) + "x" +
                std::to_string(textureDescription.Height) +
                " textureFormat=" +
                std::to_string(static_cast<int64_t>(textureDescription.Format)) +
                " negotiatedFormat=" + std::to_string(mirrorSwapchainFormat_));
            return false;
        }
    }
    sessionDevice_->GetImmediateContext(&sessionContext_);
    if (sessionContext_ == nullptr) {
        lastResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        log.Write("[VR][display] D3D11 immediate context is unavailable");
        return false;
    }

    lastResult_ = XR_SUCCESS;
    log.Write(
        "[VR][display] mirror swapchain ready " +
        std::to_string(mirrorWidth_) + "x" +
        std::to_string(mirrorHeight_) +
        " sourceFormat=" + std::to_string(static_cast<int>(mirrorSourceFormat_)) +
        " swapchainFormat=" + std::to_string(mirrorSwapchainFormat_) +
        " images=" + std::to_string(imageCount));
    return true;
}

bool OpenXrContext::DestroyMirrorSwapchainForRebuild(VrLog& log) {
    if (mirrorSwapchain_ != XR_NULL_HANDLE) {
        if (dispatch_.DestroySwapchain() == nullptr) {
            lastResult_ = XR_ERROR_FUNCTION_UNSUPPORTED;
            log.Write("[VR][display] xrDestroySwapchain is unavailable during layout rebuild");
            return false;
        }
        lastResult_ = dispatch_.DestroySwapchain()(mirrorSwapchain_);
        if (XR_FAILED(lastResult_)) {
            log.Write(
                "[VR][display] xrDestroySwapchain failed during layout rebuild: " +
                dispatch_.ResultText(instance_, lastResult_));
            return false;
        }
    }

    mirrorImages_.clear();
    mirrorSwapchain_ = XR_NULL_HANDLE;
    mirrorWidth_ = 0;
    mirrorHeight_ = 0;
    mirrorLayoutGeneration_ = 0;
    mirrorSourceFormat_ = DXGI_FORMAT_UNKNOWN;
    mirrorSwapchainFormat_ = 0;
    ReleasePortraitLatch();
    if (sessionContext_ != nullptr) {
        sessionContext_->Release();
        sessionContext_ = nullptr;
    }
    return true;
}

bool OpenXrContext::EnsureMirrorLayout(
    ID3D11Texture2D* sourceFrame,
    std::uint64_t sourceLayoutGeneration,
    bool& layoutChanged,
    VrLog& log) {
    layoutChanged = false;
    if (sourceFrame == nullptr || session_ == XR_NULL_HANDLE ||
        sessionDevice_ == nullptr) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDescription{};
    sourceFrame->GetDesc(&sourceDescription);
    if (sourceDescription.Width == 0 || sourceDescription.Height == 0 ||
        sourceDescription.MipLevels != 1 || sourceDescription.ArraySize != 1 ||
        sourceDescription.SampleDesc.Count != 1 ||
        !IsSupportedMirrorFormat(sourceDescription.Format)) {
        lastResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        log.Write(
            "[VR][display] rejected changed Unity frame texture: size=" +
            std::to_string(sourceDescription.Width) + "x" +
            std::to_string(sourceDescription.Height) +
            " format=" + std::to_string(static_cast<int>(sourceDescription.Format)) +
            " mips=" + std::to_string(sourceDescription.MipLevels) +
            " arraySize=" + std::to_string(sourceDescription.ArraySize) +
            " samples=" + std::to_string(sourceDescription.SampleDesc.Count));
        return false;
    }

    if (sourceDescription.Width == mirrorWidth_ &&
        sourceDescription.Height == mirrorHeight_ &&
        sourceDescription.Format == mirrorSourceFormat_ &&
        mirrorSwapchain_ != XR_NULL_HANDLE) {
        mirrorLayoutGeneration_ = sourceLayoutGeneration;
        return true;
    }

    if (sourceDescription.Width > mirrorMaximumWidth_ ||
        sourceDescription.Height > mirrorMaximumHeight_) {
        lastResult_ = XR_ERROR_SWAPCHAIN_RECT_INVALID;
        log.Write(
            "[VR][display] changed Unity frame " +
            std::to_string(sourceDescription.Width) + "x" +
            std::to_string(sourceDescription.Height) +
            " exceeds runtime maximum " + std::to_string(mirrorMaximumWidth_) + "x" +
            std::to_string(mirrorMaximumHeight_));
        return false;
    }

    const std::uint32_t previousWidth = mirrorWidth_;
    const std::uint32_t previousHeight = mirrorHeight_;
    const DXGI_FORMAT previousFormat = mirrorSourceFormat_;
    const std::uint64_t previousGeneration = mirrorLayoutGeneration_;

    // No swapchain image is acquired between frames, so the mirror swapchain
    // can be replaced without rebuilding the OpenXR session, spaces or actions.
    if (!DestroyMirrorSwapchainForRebuild(log)) {
        return false;
    }
    mirrorWidth_ = sourceDescription.Width;
    mirrorHeight_ = sourceDescription.Height;
    mirrorSourceFormat_ = sourceDescription.Format;
    mirrorLayoutGeneration_ = sourceLayoutGeneration;
    if (!CreateMirrorSwapchain(log)) {
        log.Write(
            "[VR][display] mirror layout replacement failed old=" +
            std::to_string(previousWidth) + "x" + std::to_string(previousHeight) +
            " new=" + std::to_string(sourceDescription.Width) + "x" +
            std::to_string(sourceDescription.Height));
        return false;
    }

    layoutChanged = true;
    lastResult_ = XR_SUCCESS;
    log.Write(
        "[VR][display] MIRROR_LAYOUT_CHANGED old=" +
        std::to_string(previousWidth) + "x" + std::to_string(previousHeight) +
        " oldFormat=" + std::to_string(static_cast<int>(previousFormat)) +
        " oldGeneration=" + std::to_string(previousGeneration) +
        " new=" + std::to_string(mirrorWidth_) + "x" +
        std::to_string(mirrorHeight_) +
        " newFormat=" + std::to_string(static_cast<int>(mirrorSourceFormat_)) +
        " newGeneration=" + std::to_string(mirrorLayoutGeneration_));
    return true;
}

bool OpenXrContext::RenderMirrorFrame(
    ID3D11Texture2D* sourceFrame,
    std::uint64_t sourceFrameGeneration,
    const std::array<PointerState, 2>& pointers,
    VrLog& log) {
    if (sourceFrame == nullptr || mirrorSwapchain_ == XR_NULL_HANDLE ||
        sessionContext_ == nullptr ||
        dispatch_.AcquireSwapchainImage() == nullptr ||
        dispatch_.WaitSwapchainImage() == nullptr ||
        dispatch_.ReleaseSwapchainImage() == nullptr) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDescription{};
    sourceFrame->GetDesc(&sourceDescription);
    if (sourceDescription.Width != mirrorWidth_ ||
        sourceDescription.Height != mirrorHeight_ ||
        sourceDescription.MipLevels != 1 || sourceDescription.ArraySize != 1 ||
        sourceDescription.SampleDesc.Count != 1 ||
        !AreCopyCompatibleFormats(
            sourceDescription.Format,
            static_cast<DXGI_FORMAT>(mirrorSwapchainFormat_))) {
        lastResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        log.Write(
            "[VR][display] Unity mirror source changed incompatibly: size=" +
            std::to_string(sourceDescription.Width) + "x" +
            std::to_string(sourceDescription.Height) +
            " format=" + std::to_string(static_cast<int>(sourceDescription.Format)));
        return false;
    }

    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t imageIndex = 0;
    lastResult_ = dispatch_.AcquireSwapchainImage()(
        mirrorSwapchain_, &acquireInfo, &imageIndex);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][display] xrAcquireSwapchainImage failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    lastResult_ = dispatch_.WaitSwapchainImage()(mirrorSwapchain_, &waitInfo);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][display] xrWaitSwapchainImage failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }
    if (imageIndex >= mirrorImages_.size()) {
        lastResult_ = XR_ERROR_INDEX_OUT_OF_RANGE;
        log.Write("[VR][display] runtime returned an invalid swapchain image index");
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        dispatch_.ReleaseSwapchainImage()(mirrorSwapchain_, &releaseInfo);
        lastResult_ = XR_ERROR_INDEX_OUT_OF_RANGE;
        return false;
    }

    const auto latchPolicy = CurrentPortraitLatchPolicy();
    ID3D11Texture2D* copySource = sourceFrame;
    if (latchPolicy == PortraitLatchPolicy::CaptureLoading) {
        if (EnsurePortraitLatch(sourceDescription, log)) {
            sessionContext_->CopyResource(portraitLatchTexture_, sourceFrame);
            portraitLatchValid_ = true;
            if (!portraitLatchCaptureLogged_) {
                portraitLatchCaptureLogged_ = true;
                portraitLatchFrozenLogged_ = false;
                portraitLatchFollowLogged_ = false;
                log.Write("[VR][display] PORTRAIT_LATCH capturing");
            }
        }
    } else if (latchPolicy == PortraitLatchPolicy::HoldFrozen) {
        if (!portraitLatchValid_ && EnsurePortraitLatch(sourceDescription, log)) {
            sessionContext_->CopyResource(portraitLatchTexture_, sourceFrame);
            portraitLatchValid_ = true;
            if (!portraitLatchFrozenLogged_) {
                portraitLatchFrozenLogged_ = true;
                log.Write("[VR][display] PORTRAIT_LATCH bootstrap-freeze");
            }
        }
        if (portraitLatchValid_ && portraitLatchTexture_ != nullptr) {
            copySource = portraitLatchTexture_;
            if (!portraitLatchFrozenLogged_) {
                portraitLatchFrozenLogged_ = true;
                portraitLatchCaptureLogged_ = false;
                portraitLatchFollowLogged_ = false;
                log.Write("[VR][display] PORTRAIT_LATCH frozen");
            }
        }
    } else {
        portraitLatchCaptureLogged_ = false;
        portraitLatchFrozenLogged_ = false;
        if (!portraitLatchFollowLogged_) {
            portraitLatchFollowLogged_ = true;
            log.Write("[VR][display] PORTRAIT_LATCH following");
        }
    }
    sessionContext_->CopyResource(mirrorImages_[imageIndex].texture, copySource);
    if (sourceFrameGeneration != lastMirrorFingerprintGeneration_ &&
        sourceFrameGeneration <= 2U) {
        lastMirrorFingerprintGeneration_ = sourceFrameGeneration;
        std::uint64_t fingerprint = 0;
        const bool sampled = d3d11::ComputeTextureFingerprint(
            sessionContext_, sourceFrame, 0, fingerprint);
        std::ostringstream content;
        content << "[VR][display] MIRROR_SOURCE_FINGERPRINT generation="
                << sourceFrameGeneration << " value=";
        if (sampled) {
            content << "0x" << std::hex << fingerprint << std::dec
                    << " changed="
                    << (!mirrorFingerprintValid_ ||
                        mirrorFingerprint_ != fingerprint);
            mirrorFingerprint_ = fingerprint;
            mirrorFingerprintValid_ = true;
        } else {
            content << "unavailable";
        }
        log.Write(content.str());
    }
    OverlayPointerCursors(mirrorImages_[imageIndex].texture, pointers);

    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    lastResult_ = dispatch_.ReleaseSwapchainImage()(mirrorSwapchain_, &releaseInfo);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][display] xrReleaseSwapchainImage failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }
    return true;
}

bool OpenXrContext::EnsureMenuSwapchain(VrLog& log) {
    if (menuSwapchain_ != XR_NULL_HANDLE) {
        return true;
    }
    if (menuSwapchainFailed_ || session_ == XR_NULL_HANDLE ||
        sessionDevice_ == nullptr || sessionContext_ == nullptr ||
        dispatch_.EnumerateSwapchainFormats() == nullptr ||
        dispatch_.CreateSwapchain() == nullptr ||
        dispatch_.EnumerateSwapchainImages() == nullptr) {
        return false;
    }

    uint32_t formatCount = 0;
    if (XR_FAILED(dispatch_.EnumerateSwapchainFormats()(
            session_, 0, &formatCount, nullptr)) ||
        formatCount == 0) {
        menuSwapchainFailed_ = true;
        log.Write("[VR][menu] swapchain format count query failed");
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    uint32_t written = formatCount;
    if (XR_FAILED(dispatch_.EnumerateSwapchainFormats()(
            session_,
            static_cast<uint32_t>(formats.size()),
            &written,
            formats.data()))) {
        menuSwapchainFailed_ = true;
        log.Write("[VR][menu] swapchain format enumeration failed");
        return false;
    }
    formats.resize(std::min(written, static_cast<uint32_t>(formats.size())));

    // The menu is painted into an owned UNORM buffer and copied in, so an
    // sRGB quad format keeps the compositor from re-encoding the pixels.
    const std::array<DXGI_FORMAT, 4> preferred{
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };
    menuSwapchainFormat_ = 0;
    for (const DXGI_FORMAT candidate : preferred) {
        if (std::find(formats.begin(), formats.end(),
                      static_cast<int64_t>(candidate)) != formats.end()) {
            menuSwapchainFormat_ = static_cast<int64_t>(candidate);
            break;
        }
    }
    if (menuSwapchainFormat_ == 0) {
        menuSwapchainFailed_ = true;
        log.Write("[VR][menu] no 8-bit RGBA swapchain format for the menu quad");
        return false;
    }

    const std::uint32_t width = mirrorMaximumWidth_ == 0
        ? kMenuTextureWidth
        : std::min(kMenuTextureWidth, mirrorMaximumWidth_);
    const std::uint32_t height = mirrorMaximumHeight_ == 0
        ? kMenuTextureHeight
        : std::min(kMenuTextureHeight, mirrorMaximumHeight_);
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.format = menuSwapchainFormat_;
    createInfo.sampleCount = 1;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    XrResult result =
        dispatch_.CreateSwapchain()(session_, &createInfo, &menuSwapchain_);
    if (XR_FAILED(result)) {
        menuSwapchain_ = XR_NULL_HANDLE;
        menuSwapchainFailed_ = true;
        log.Write(
            "[VR][menu] xrCreateSwapchain failed: " +
            dispatch_.ResultText(instance_, result));
        return false;
    }

    uint32_t imageCount = 0;
    result = dispatch_.EnumerateSwapchainImages()(
        menuSwapchain_, 0, &imageCount, nullptr);
    if (XR_FAILED(result) || imageCount == 0) {
        menuSwapchainFailed_ = true;
        log.Write("[VR][menu] swapchain image count failed");
        ResetMenuSwapchain();
        return false;
    }
    menuImages_.assign(
        imageCount, XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    written = imageCount;
    result = dispatch_.EnumerateSwapchainImages()(
        menuSwapchain_,
        imageCount,
        &written,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(menuImages_.data()));
    if (XR_FAILED(result) || written != imageCount) {
        menuSwapchainFailed_ = true;
        log.Write("[VR][menu] swapchain image enumeration failed");
        ResetMenuSwapchain();
        return false;
    }
    for (const auto& image : menuImages_) {
        if (image.texture == nullptr) {
            menuSwapchainFailed_ = true;
            log.Write("[VR][menu] runtime returned a null menu swapchain texture");
            ResetMenuSwapchain();
            return false;
        }
    }

    menuWidth_ = width;
    menuHeight_ = height;
    menuPaintFailures_ = 0;
    log.Write(
        "[VR][menu] MENU_SWAPCHAIN_READY " + std::to_string(menuWidth_) + "x" +
        std::to_string(menuHeight_) + " format=" +
        std::to_string(menuSwapchainFormat_) + " images=" +
        std::to_string(imageCount));
    return true;
}

bool OpenXrContext::ApplyStereoRenderScale(float renderScale, VrLog& log) {
    const float clamped = std::clamp(renderScale, 0.25F, 1.5F);
    stereoRenderScale_ = clamped;
    if (!stereoProjectionEnabled_ || recommendedEyeWidth_ == 0 ||
        recommendedEyeHeight_ == 0) {
        return false;
    }
    const std::uint32_t width =
        ScaledEyeExtent(recommendedEyeWidth_, clamped, mirrorMaximumWidth_);
    const std::uint32_t height =
        ScaledEyeExtent(recommendedEyeHeight_, clamped, mirrorMaximumHeight_);
    if (width == stereoEyeWidth_ && height == stereoEyeHeight_) {
        return false;
    }
    log.Write(
        "[VR][stereo] RENDER_SCALE_APPLIED scale=" + std::to_string(clamped) +
        " target=" + std::to_string(width) + "x" + std::to_string(height) +
        " previous=" + std::to_string(stereoEyeWidth_) + "x" +
        std::to_string(stereoEyeHeight_));
    stereoEyeWidth_ = width;
    stereoEyeHeight_ = height;
    // The projection swapchain is not touched here: this runs inside a frame
    // whose layer may still reference it. EnsureProjectionSwapchain rebuilds it
    // on the first stereo frame that arrives at the new size.
    return true;
}

bool OpenXrContext::RenderMenuFrame(
    const std::array<PointerState, 2>& pointers,
    VrLog& log) {
    if (aaMenuPainter_ == nullptr || !EnsureMenuSwapchain(log) ||
        dispatch_.AcquireSwapchainImage() == nullptr ||
        dispatch_.WaitSwapchainImage() == nullptr ||
        dispatch_.ReleaseSwapchainImage() == nullptr) {
        return false;
    }

    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t imageIndex = 0;
    XrResult result = dispatch_.AcquireSwapchainImage()(
        menuSwapchain_, &acquireInfo, &imageIndex);
    if (XR_FAILED(result) || imageIndex >= menuImages_.size()) {
        log.Write(
            "[VR][menu] xrAcquireSwapchainImage failed: " +
            dispatch_.ResultText(instance_, result));
        return false;
    }

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    result = dispatch_.WaitSwapchainImage()(menuSwapchain_, &waitInfo);
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(result)) {
        dispatch_.ReleaseSwapchainImage()(menuSwapchain_, &releaseInfo);
        log.Write(
            "[VR][menu] xrWaitSwapchainImage failed: " +
            dispatch_.ResultText(instance_, result));
        return false;
    }

    AaMenuOutput output;
    const bool painted = aaMenuPainter_(
        sessionDevice_,
        sessionContext_,
        menuImages_[imageIndex].texture,
        menuWidth_,
        menuHeight_,
        SelectAaMenuInput(pointers, menuPointerHand_),
        output,
        log);
    // Every acquired image must be released, otherwise the next acquire
    // returns XR_ERROR_CALL_ORDER_INVALID and the worker stops submitting.
    result = dispatch_.ReleaseSwapchainImage()(menuSwapchain_, &releaseInfo);
    if (XR_FAILED(result)) {
        log.Write(
            "[VR][menu] xrReleaseSwapchainImage failed: " +
            dispatch_.ResultText(instance_, result));
        return false;
    }

    if (output.requestRenderScale) {
        ApplyStereoRenderScale(output.renderScale, log);
    }
    if (output.requestClose) {
        aaMenuVisible_ = false;
        if (aaMenuFlush_ != nullptr) {
            aaMenuFlush_(log);
        }
        log.Write("[VR][ui] STEREO_AA_MENU visible=0 gesture=menu-close");
    }
    if (output.requestQuit) {
        log.Write("[VR][menu] process exit requested");
        TerminateProcess(GetCurrentProcess(), 0);
    }
    if (!painted) {
        ++menuPaintFailures_;
        if (menuPaintFailures_ == 1 ||
            menuPaintFailures_ == kMenuPaintFailureLimit) {
            log.Write(
                "[VR][menu] menu paint failed count=" +
                std::to_string(menuPaintFailures_));
        }
        if (menuPaintFailures_ >= kMenuPaintFailureLimit) {
            aaMenuVisible_ = false;
            if (aaMenuFlush_ != nullptr) {
                aaMenuFlush_(log);
            }
            menuPaintFailures_ = 0;
            log.Write("[VR][ui] STEREO_AA_MENU visible=0 gesture=paint-failure");
        }
        return false;
    }
    menuPaintFailures_ = 0;
    return true;
}

bool OpenXrContext::CreateProjectionSwapchain(
    const D3D11_TEXTURE2D_DESC& sourceDescription,
    VrLog& log) {
    if (session_ == XR_NULL_HANDLE || sessionDevice_ == nullptr ||
        dispatch_.CreateSwapchain() == nullptr ||
        dispatch_.EnumerateSwapchainFormats() == nullptr ||
        dispatch_.EnumerateSwapchainImages() == nullptr) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        return false;
    }

    uint32_t formatCount = 0;
    lastResult_ = dispatch_.EnumerateSwapchainFormats()(session_, 0, &formatCount, nullptr);
    if (XR_FAILED(lastResult_) || formatCount == 0) {
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    uint32_t written = formatCount;
    lastResult_ = dispatch_.EnumerateSwapchainFormats()(
        session_, formatCount, &written, formats.data());
    if (XR_FAILED(lastResult_) || written == 0) {
        return false;
    }
    formats.resize(std::min(written, formatCount));
    const bool bgra = AreCopyCompatibleFormats(
        sourceDescription.Format,
        DXGI_FORMAT_B8G8R8A8_UNORM);
    const std::array<DXGI_FORMAT, 2> preferred = bgra
        ? std::array<DXGI_FORMAT, 2>{
              DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
              DXGI_FORMAT_B8G8R8A8_UNORM}
        : std::array<DXGI_FORMAT, 2>{
              DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
              DXGI_FORMAT_R8G8B8A8_UNORM};
    projectionSwapchainFormat_ = 0;
    for (const DXGI_FORMAT candidate : preferred) {
        if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(candidate)) !=
            formats.end()) {
            projectionSwapchainFormat_ = static_cast<int64_t>(candidate);
            break;
        }
    }
    if (projectionSwapchainFormat_ == 0) {
        lastResult_ = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        return false;
    }

    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.format = projectionSwapchainFormat_;
    createInfo.sampleCount = 1;
    createInfo.width = sourceDescription.Width;
    createInfo.height = sourceDescription.Height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 2;
    createInfo.mipCount = 1;
    lastResult_ = dispatch_.CreateSwapchain()(session_, &createInfo, &projectionSwapchain_);
    if (XR_FAILED(lastResult_)) {
        projectionSwapchain_ = XR_NULL_HANDLE;
        return false;
    }

    uint32_t imageCount = 0;
    lastResult_ = dispatch_.EnumerateSwapchainImages()(
        projectionSwapchain_, 0, &imageCount, nullptr);
    if (XR_FAILED(lastResult_) || imageCount == 0) {
        ResetProjectionSwapchain();
        return false;
    }
    projectionImages_.assign(
        imageCount,
        XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    written = imageCount;
    lastResult_ = dispatch_.EnumerateSwapchainImages()(
        projectionSwapchain_,
        imageCount,
        &written,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(projectionImages_.data()));
    if (XR_FAILED(lastResult_) || written != imageCount) {
        ResetProjectionSwapchain();
        return false;
    }
    for (const auto& image : projectionImages_) {
        if (image.texture == nullptr) {
            lastResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
            ResetProjectionSwapchain();
            return false;
        }
        D3D11_TEXTURE2D_DESC description{};
        image.texture->GetDesc(&description);
        if (description.Width != sourceDescription.Width ||
            description.Height != sourceDescription.Height ||
            description.ArraySize != 2 || description.MipLevels != 1 ||
            description.SampleDesc.Count != 1 ||
            !AreCopyCompatibleFormats(
                description.Format,
                static_cast<DXGI_FORMAT>(projectionSwapchainFormat_))) {
            lastResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
            ResetProjectionSwapchain();
            return false;
        }
    }
    if (!projectionVerticalFlip_.Initialize(sessionDevice_)) {
        log.Write(
            "[VR][stereo] PROJECTION_VERTICAL_FLIP_FAILED stage=initialize status=" +
            std::string(d3d11::VerticalFlipPass::StatusName(
                projectionVerticalFlip_.LastStatus())) +
            " hr=" + std::to_string(projectionVerticalFlip_.LastHresult()));
        lastResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        ResetProjectionSwapchain();
        return false;
    }
    projectionSourceDescription_ = sourceDescription;
    D3D11_TEXTURE2D_DESC projectionImageDescription{};
    projectionImages_.front().texture->GetDesc(&projectionImageDescription);
    log.Write(
        "[VR][stereo] PROJECTION_SWAPCHAIN_READY " +
        std::to_string(sourceDescription.Width) + "x" +
        std::to_string(sourceDescription.Height) + " array=2 format=" +
        std::to_string(projectionSwapchainFormat_) + " images=" +
        std::to_string(imageCount) + " resourceFormat=" +
        std::to_string(static_cast<unsigned int>(
            projectionImageDescription.Format)));
    log.Write("[VR][stereo] PROJECTION_VERTICAL_FLIP_READY mode=pixel-load-y-flip");
    lastResult_ = XR_SUCCESS;
    return true;
}

bool OpenXrContext::EnsureProjectionSwapchain(
    const d3d11::StereoRenderMailbox::Snapshot& stereoFrame,
    VrLog& log) {
    const auto& description = stereoFrame.description;
    const bool compatible = projectionSwapchain_ != XR_NULL_HANDLE &&
        description.Width == projectionSourceDescription_.Width &&
        description.Height == projectionSourceDescription_.Height &&
        description.Format == projectionSourceDescription_.Format &&
        description.ArraySize == 1 && description.MipLevels == 1 &&
        description.SampleDesc.Count == 1;
    if (compatible) {
        return true;
    }
    ResetProjectionSwapchain();
    return CreateProjectionSwapchain(description, log);
}

bool OpenXrContext::RenderProjectionFrame(
    const d3d11::StereoRenderMailbox::Snapshot& stereoFrame,
    VrLog& log) {
    if (!EnsureProjectionSwapchain(stereoFrame, log) ||
        sessionContext_ == nullptr || dispatch_.AcquireSwapchainImage() == nullptr ||
        dispatch_.WaitSwapchainImage() == nullptr ||
        dispatch_.ReleaseSwapchainImage() == nullptr) {
        return false;
    }
    for (ID3D11Texture2D* source : stereoFrame.textures) {
        ID3D11Device* sourceDevice = nullptr;
        source->GetDevice(&sourceDevice);
        const bool correctDevice = sourceDevice == sessionDevice_;
        if (sourceDevice != nullptr) {
            sourceDevice->Release();
        }
        if (!correctDevice) {
            lastResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
            return false;
        }
    }

    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t imageIndex = 0;
    lastResult_ = dispatch_.AcquireSwapchainImage()(
        projectionSwapchain_, &acquireInfo, &imageIndex);
    if (XR_FAILED(lastResult_) || imageIndex >= projectionImages_.size()) {
        return false;
    }
    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    lastResult_ = dispatch_.WaitSwapchainImage()(projectionSwapchain_, &waitInfo);
    if (XR_FAILED(lastResult_)) {
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        dispatch_.ReleaseSwapchainImage()(projectionSwapchain_, &releaseInfo);
        return false;
    }

    ID3D11Texture2D* destination = projectionImages_[imageIndex].texture;
    if (!projectionVerticalFlip_.FlipStereo(
            sessionContext_, stereoFrame.textures, destination)) {
        log.Write(
            "[VR][stereo] PROJECTION_VERTICAL_FLIP_FAILED stage=draw status=" +
            std::string(d3d11::VerticalFlipPass::StatusName(
                projectionVerticalFlip_.LastStatus())) +
            " hr=" + std::to_string(projectionVerticalFlip_.LastHresult()));
        XrSwapchainImageReleaseInfo releaseInfo{
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult releaseResult =
            dispatch_.ReleaseSwapchainImage()(projectionSwapchain_, &releaseInfo);
        if (XR_FAILED(releaseResult)) {
            log.Write(
                "[VR][stereo] projection swapchain release after vertical-flip "
                "failure also failed: " +
                dispatch_.ResultText(instance_, releaseResult));
        }
        lastResult_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        return false;
    }
    if (stereoFrame.generation <= 2U ||
        stereoFrame.generation % 300U == 0U) {
        log.Write(
            "[VR][stereo] PROJECTION_VERTICAL_FLIP_APPLIED generation=" +
            std::to_string(stereoFrame.generation) + " viewFormat=" +
            std::to_string(static_cast<unsigned int>(
                projectionVerticalFlip_.ViewFormat())));
    }
    if (stereoFrame.generation != lastProjectionFingerprintGeneration_ &&
        stereoFrame.generation <= 2U) {
        lastProjectionFingerprintGeneration_ = stereoFrame.generation;
        std::array<std::uint64_t, 2> fingerprints{};
        std::array<bool, 2> sampled{};
        for (UINT eye = 0; eye < 2; ++eye) {
            sampled[eye] = d3d11::ComputeTextureFingerprint(
                sessionContext_, destination, D3D11CalcSubresource(0, eye, 1),
                fingerprints[eye]);
        }
        std::ostringstream content;
        content << "[VR][stereo] PROJECTION_DESTINATION_FINGERPRINT generation="
                << stereoFrame.generation;
        for (std::size_t eye = 0; eye < fingerprints.size(); ++eye) {
            content << ' ' << (eye == 0U ? "left=" : "right=");
            if (sampled[eye]) {
                content << "0x" << std::hex << fingerprints[eye] << std::dec
                        << " changed="
                        << (!projectionFingerprintValid_[eye] ||
                            projectionFingerprints_[eye] != fingerprints[eye]);
                projectionFingerprints_[eye] = fingerprints[eye];
                projectionFingerprintValid_[eye] = true;
            } else {
                content << "unavailable";
            }
        }
        log.Write(content.str());
    }
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    lastResult_ = dispatch_.ReleaseSwapchainImage()(projectionSwapchain_, &releaseInfo);
    if (XR_FAILED(lastResult_)) {
        log.Write("[VR][stereo] projection swapchain release failed");
        return false;
    }
    return true;
}

void OpenXrContext::OverlayPointerCursors(
    ID3D11Texture2D* destination,
    const std::array<PointerState, 2>& pointers) noexcept {
    if (destination == nullptr || sessionContext_ == nullptr ||
        mirrorWidth_ == 0 || mirrorHeight_ == 0) {
        return;
    }

    const UINT diameter = std::clamp(
        std::min(mirrorWidth_, mirrorHeight_) / 80U,
        8U,
        18U);
    const int radius = static_cast<int>(diameter / 2U);
    const bool bgra = AreCopyCompatibleFormats(
        static_cast<DXGI_FORMAT>(mirrorSwapchainFormat_),
        DXGI_FORMAT_B8G8R8A8_UNORM);

    for (std::size_t hand = 0; hand < pointers.size(); ++hand) {
        const auto& pointer = pointers[hand];
        if (!pointer.hovering) {
            continue;
        }

        auto packedPixel = [&](const std::array<std::uint8_t, 4>& rgba) {
            const std::uint8_t byte0 = bgra ? rgba[2] : rgba[0];
            const std::uint8_t byte2 = bgra ? rgba[0] : rgba[2];
            return static_cast<std::uint32_t>(byte0) |
                   (static_cast<std::uint32_t>(rgba[1]) << 8U) |
                   (static_cast<std::uint32_t>(byte2) << 16U) |
                   (static_cast<std::uint32_t>(rgba[3]) << 24U);
        };
        std::array<std::uint32_t, 28U * 28U> outlinePixels{};
        std::array<std::uint32_t, 28U * 28U> whitePixels{};
        outlinePixels.fill(packedPixel({24, 24, 24, 255}));
        whitePixels.fill(packedPixel({255, 255, 255, 255}));

        const int centerX = static_cast<int>(std::lround(
            pointer.u * static_cast<float>(mirrorWidth_ - 1U)));
        const int centerY = static_cast<int>(std::lround(
            pointer.v * static_cast<float>(mirrorHeight_ - 1U)));
        auto updateBox = [&](int left,
                             int top,
                             int right,
                             int bottom,
                             const std::array<std::uint32_t, 28U * 28U>& pixels) {
            left = std::clamp(left, 0, static_cast<int>(mirrorWidth_));
            right = std::clamp(right, 0, static_cast<int>(mirrorWidth_));
            top = std::clamp(top, 0, static_cast<int>(mirrorHeight_));
            bottom = std::clamp(bottom, 0, static_cast<int>(mirrorHeight_));
            if (left >= right || top >= bottom) {
                return;
            }
            D3D11_BOX box{};
            box.left = static_cast<UINT>(left);
            box.top = static_cast<UINT>(top);
            box.front = 0;
            box.right = static_cast<UINT>(right);
            box.bottom = static_cast<UINT>(bottom);
            box.back = 1;
            sessionContext_->UpdateSubresource(
                destination,
                0,
                &box,
                pixels.data(),
                static_cast<UINT>(right - left) * sizeof(std::uint32_t),
                0);
        };

        auto drawDisc = [&](int discRadius,
                            const std::array<std::uint32_t, 28U * 28U>& pixels) {
            auto halfWidthAt = [&](int y) {
                return static_cast<int>(std::floor(std::sqrt(
                    static_cast<double>(discRadius * discRadius - y * y))));
            };
            for (int top = -discRadius; top <= discRadius;) {
                const int halfWidth = halfWidthAt(top);
                int bottom = top + 1;
                while (bottom <= discRadius && halfWidthAt(bottom) == halfWidth) {
                    ++bottom;
                }
                updateBox(
                    centerX - halfWidth,
                    centerY + top,
                    centerX + halfWidth + 1,
                    centerY + bottom,
                    pixels);
                top = bottom;
            }
        };
        drawDisc(radius, outlinePixels);
        const int innerRadius = std::max(radius - 2, 1);
        drawDisc(innerRadius, whitePixels);
    }
}

OpenXrContext::EventResult OpenXrContext::DrainEvents(VrLog& log) {
    if (instance_ == XR_NULL_HANDLE || dispatch_.PollEvent() == nullptr) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        return EventResult::Failed;
    }

    // An upper bound keeps a misbehaving OpenXR implementation from monopolizing this runtime
    // worker. A subsequent tick continues draining if more events remain.
    constexpr uint32_t kMaximumEventsPerDrain = 256;
    uint32_t drained = 0;
    for (; drained < kMaximumEventsPerDrain; ++drained) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        lastResult_ = dispatch_.PollEvent()(instance_, &event);
        if (lastResult_ == XR_EVENT_UNAVAILABLE) {
            lastResult_ = XR_SUCCESS;
            break;
        }
        if (XR_FAILED(lastResult_)) {
            log.Write("[VR][runtime] xrPollEvent failed: " + dispatch_.ResultText(instance_, lastResult_));
            return ClassifyEventResult(lastResult_);
        }

        if (event.type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
            const auto& eventsLost =
                *reinterpret_cast<const XrEventDataEventsLost*>(&event);
            lastResult_ = XR_ERROR_RUNTIME_FAILURE;
            log.Write(
                "[VR][runtime] OpenXR lost " +
                std::to_string(eventsLost.lostEventCount) +
                " event(s); session state can no longer be trusted");
            return EventResult::Failed;
        }
        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            sessionRunning_ = false;
            log.Write("[VR][runtime] OpenXR reported instance loss pending");
            return EventResult::InstanceLost;
        }
        if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const auto& spaceChanged =
                *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
            if (spaceChanged.session == session_) {
                const bool active =
                    spaceChanged.referenceSpaceType == ActiveReferenceSpaceType();
                log.Write(
                    std::string("[VR][pose] REFERENCE_SPACE_CHANGE_PENDING space=") +
                    ReferenceSpaceText(spaceChanged.referenceSpaceType) +
                    " active=" + (active ? "1" : "0") +
                    " changeTime=" + std::to_string(spaceChanged.changeTime) +
                    " poseValid=" + std::to_string(spaceChanged.poseValid));
                if (active &&
                    std::find(
                        pendingReferenceSpaceChangeTimes_.begin(),
                        pendingReferenceSpaceChangeTimes_.end(),
                        spaceChanged.changeTime) ==
                        pendingReferenceSpaceChangeTimes_.end()) {
                    pendingReferenceSpaceChangeTimes_.push_back(spaceChanged.changeTime);
                    std::sort(
                        pendingReferenceSpaceChangeTimes_.begin(),
                        pendingReferenceSpaceChangeTimes_.end());
                }
            }
        }
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& stateChanged =
                *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (stateChanged.session == session_) {
                sessionState_ = stateChanged.state;
                log.Write(
                    "[VR][runtime] session state=" +
                    std::to_string(static_cast<int>(stateChanged.state)));

                if (stateChanged.state == XR_SESSION_STATE_READY && !sessionRunning_) {
                    if (dispatch_.BeginSession() == nullptr) {
                        lastResult_ = XR_ERROR_FUNCTION_UNSUPPORTED;
                        log.Write("[VR][runtime] xrBeginSession is unavailable");
                        return EventResult::Failed;
                    }

                    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                    beginInfo.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    lastResult_ = dispatch_.BeginSession()(session_, &beginInfo);
                    if (XR_FAILED(lastResult_)) {
                        log.Write(
                            "[VR][runtime] xrBeginSession failed: " +
                            dispatch_.ResultText(instance_, lastResult_));
                        return ClassifyEventResult(lastResult_);
                    }
                    if (lastResult_ == XR_SESSION_LOSS_PENDING) {
                        sessionRunning_ = false;
                        return EventResult::SessionLost;
                    }
                    sessionRunning_ = true;
                    exitRequested_ = false;
                    triggerHeld_.fill(false);
                    ++sessionRunGeneration_;
                    log.Write("[VR][runtime] xrBeginSession succeeded");
                }

                if (stateChanged.state == XR_SESSION_STATE_STOPPING && sessionRunning_) {
                    if (dispatch_.EndSession() == nullptr) {
                        lastResult_ = XR_ERROR_FUNCTION_UNSUPPORTED;
                        log.Write("[VR][runtime] xrEndSession is unavailable");
                        return EventResult::Failed;
                    }

                    lastResult_ = dispatch_.EndSession()(session_);
                    // The OpenXR session is considered not running after this
                    // call even if the runtime reports an error.
                    sessionRunning_ = false;
                    triggerHeld_.fill(false);
                    if (XR_FAILED(lastResult_)) {
                        log.Write(
                            "[VR][runtime] xrEndSession failed: " +
                            dispatch_.ResultText(instance_, lastResult_));
                        return ClassifyEventResult(lastResult_);
                    }
                    log.Write("[VR][runtime] xrEndSession succeeded");
                }

                if (stateChanged.state == XR_SESSION_STATE_LOSS_PENDING) {
                    sessionRunning_ = false;
                    return EventResult::SessionLost;
                }
                if (stateChanged.state == XR_SESSION_STATE_EXITING) {
                    sessionRunning_ = false;
                    return EventResult::SessionExiting;
                }
            }
        }
    }

    if (drained == kMaximumEventsPerDrain) {
        lastResult_ = XR_ERROR_RUNTIME_FAILURE;
        log.Write("[VR][runtime] event queue did not quiesce after 256 events");
        return EventResult::Failed;
    }

    if (drained != 0) {
        log.Write("[VR][runtime] drained " + std::to_string(drained) + " OpenXR event(s)");
    }
    return EventResult::Healthy;
}

OpenXrContext::FrameResult OpenXrContext::RunFrame(
    StereoFrame& frame,
    ID3D11Texture2D* sourceFrame,
    std::uint64_t sourceFrameGeneration,
    std::uint64_t sourceLayoutGeneration,
    const d3d11::StereoRenderMailbox* stereoMailbox,
    VrLog& log) {
    // gripPanelTransparent is the only caller-input field on the otherwise
    // output-only frame; latch it across the reset.
    const bool gripPanelTransparent = frame.gripPanelTransparent;
    frame = StereoFrame{};
    frame.gripPanelTransparent = gripPanelTransparent;

    if (session_ == XR_NULL_HANDLE || localSpace_ == XR_NULL_HANDLE) {
        lastResult_ = XR_ERROR_HANDLE_INVALID;
        log.Write("[VR][runtime] frame skipped: session or LOCAL space is unavailable");
        return FrameResult::Failed;
    }
    if (!sessionRunning_) {
        lastResult_ = XR_ERROR_SESSION_NOT_RUNNING;
        return FrameResult::SessionNotRunning;
    }
    if (dispatch_.WaitFrame() == nullptr || dispatch_.BeginFrame() == nullptr ||
        dispatch_.LocateViews() == nullptr || dispatch_.EndFrame() == nullptr) {
        lastResult_ = XR_ERROR_FUNCTION_UNSUPPORTED;
        log.Write("[VR][runtime] frame entry points are unavailable");
        return FrameResult::Failed;
    }

    bool mirrorLayoutChanged = false;
    if (!EnsureMirrorLayout(
            sourceFrame,
            sourceLayoutGeneration,
            mirrorLayoutChanged,
            log)) {
        return ClassifyFrameResult(lastResult_);
    }
    frame.mirrorLayoutChanged = mirrorLayoutChanged;
    frame.mirrorLayoutGeneration = mirrorLayoutGeneration_;
    frame.mirrorWidth = mirrorWidth_;
    frame.mirrorHeight = mirrorHeight_;

    bool sessionLossPending = false;
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    lastResult_ = dispatch_.WaitFrame()(session_, &waitInfo, &frameState);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][runtime] xrWaitFrame failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return ClassifyFrameResult(lastResult_);
    }
    sessionLossPending = lastResult_ == XR_SESSION_LOSS_PENDING;
    frame.predictedDisplayTime = frameState.predictedDisplayTime;
    frame.predictedDisplayPeriod = frameState.predictedDisplayPeriod;
    frame.shouldRender = frameState.shouldRender == XR_TRUE;

    XrTime appliedReferenceSpaceChangeTime = 0;
    std::size_t appliedReferenceSpaceChangeCount = 0;
    for (const XrTime changeTime : pendingReferenceSpaceChangeTimes_) {
        if (frameState.predictedDisplayTime < changeTime) {
            break;
        }
        appliedReferenceSpaceChangeTime =
            std::max(appliedReferenceSpaceChangeTime, changeTime);
        ++appliedReferenceSpaceChangeCount;
    }
    frame.referenceSpaceChanged = appliedReferenceSpaceChangeCount != 0;
    const XrTime effectiveProjectionTrackingTimeFloor = std::max(
        projectionTrackingTimeFloor_, appliedReferenceSpaceChangeTime);

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    lastResult_ = dispatch_.BeginFrame()(session_, &beginInfo);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][runtime] xrBeginFrame failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return ClassifyFrameResult(lastResult_);
    }
    sessionLossPending =
        sessionLossPending || lastResult_ == XR_SESSION_LOSS_PENDING;
    frame.frameDiscarded = lastResult_ == XR_FRAME_DISCARDED;

    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = frameState.predictedDisplayTime;
    locateInfo.space = stageSpace_ != XR_NULL_HANDLE ? stageSpace_ : localSpace_;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> views{
        XrView{XR_TYPE_VIEW},
        XrView{XR_TYPE_VIEW},
    };
    uint32_t viewCount = 0;
    const XrResult locateResult = dispatch_.LocateViews()(
        session_,
        &locateInfo,
        &viewState,
        static_cast<uint32_t>(views.size()),
        &viewCount,
        views.data());

    if (XR_SUCCEEDED(locateResult)) {
        sessionLossPending =
            sessionLossPending || locateResult == XR_SESSION_LOSS_PENDING;
        frame.viewStateFlags = viewState.viewStateFlags;
        frame.viewCount = std::min(viewCount, static_cast<uint32_t>(views.size()));
        for (uint32_t index = 0; index < frame.viewCount; ++index) {
            frame.views[index].pose = views[index].pose;
            frame.views[index].fov = views[index].fov;
        }
    } else {
        log.Write(
            "[VR][runtime] xrLocateViews failed: " +
            dispatch_.ResultText(instance_, locateResult));
    }

    // Panel placement first: SyncPointerInput hit-tests against the poses
    // this computes for the current predicted display time.
    UpdatePanelPlacementFrame(frameState.predictedDisplayTime, log);
    SyncPointerInput(frame.pointers, frameState.predictedDisplayTime, log);

    // xrWaitFrame is the longest blocking section in this worker. Read the
    // mailbox only after it returns so the copy uses the newest immutable
    // generation instead of a pre-wait pose paired with pixels Unity may have
    // already replaced.
    d3d11::StereoRenderMailbox::Snapshot latestStereoFrame;
    const bool stereoFrameAvailable = stereoMailbox != nullptr &&
        stereoMailbox->ReadLatest(latestStereoFrame);
    const d3d11::StereoRenderMailbox::Snapshot* stereoFrame =
        stereoFrameAvailable ? &latestStereoFrame : nullptr;

    bool mirrorReady = false;
    bool menuReady = false;
    bool overlayReady = false;
    bool projectionReady = false;
    const pose::StereoPoseSample* projectionTrackingSample = nullptr;
    XrResult mirrorResult = XR_SUCCESS;
    if (frame.shouldRender) {
        constexpr std::int64_t kMaximumStereoFrameAgeNanoseconds = 250'000'000LL;
        const std::int64_t stereoCheckTimeNanoseconds =
            pose::MonotonicNowNanoseconds();
        const std::int64_t stereoFrameAgeNanoseconds =
            stereoFrame != nullptr && stereoFrame->hostPublishTimeNanoseconds > 0 &&
                stereoCheckTimeNanoseconds >= stereoFrame->hostPublishTimeNanoseconds
            ? stereoCheckTimeNanoseconds - stereoFrame->hostPublishTimeNanoseconds
            : -1;
        const bool stereoFrameFresh = stereoFrame != nullptr &&
            !frame.referenceSpaceChanged &&
            stereoFrame->IsComplete() &&
            stereoFrame->trackingSample.sessionGeneration == sessionRunGeneration_ &&
            stereoFrame->trackingSample.referenceSpaceType ==
                static_cast<std::int32_t>(ActiveReferenceSpaceType()) &&
            stereoFrame->trackingSample.predictedDisplayTime >=
                effectiveProjectionTrackingTimeFloor &&
            stereoFrame->hostPublishTimeNanoseconds > 0 &&
            stereoFrameAgeNanoseconds >= 0 &&
            stereoFrameAgeNanoseconds <= kMaximumStereoFrameAgeNanoseconds;
        const bool stereoSceneEligible = stereoProjectionEnabled_ &&
            !projectionDisabledForSession_ &&
            (!stereoLandscapeOnly_ || mirrorWidth_ >= mirrorHeight_);
        if (stereoSceneEligible &&
            stereoFrameFresh) {
            const bool newStereoFrame =
                stereoFrame->generation != lastSubmittedStereoGeneration_;
            projectionReady = newStereoFrame
                ? RenderProjectionFrame(*stereoFrame, log)
                : projectionSwapchain_ != XR_NULL_HANDLE &&
                    lastSubmittedStereoTrackingSample_.valid;
            if (!projectionReady) {
                log.Write(
                    "[VR][stereo] PROJECTION_DISABLED_FOR_SESSION result=" +
                    dispatch_.ResultText(instance_, lastResult_));
                projectionDisabledForSession_ = true;
                ResetProjectionSwapchain();
            } else {
                if (newStereoFrame) {
                    lastSubmittedStereoGeneration_ = stereoFrame->generation;
                    lastSubmittedStereoTrackingSample_ =
                        stereoFrame->trackingSample;
                    lastSubmittedStereoHostPublishTimeNanoseconds_ =
                        stereoFrame->hostPublishTimeNanoseconds;
                }
                projectionTrackingSample =
                    &lastSubmittedStereoTrackingSample_;
                frame.stereoFrameGeneration = lastSubmittedStereoGeneration_;
                ++stereoSubmissionCount_;
                if (newStereoFrame) {
                    ++freshStereoSubmissionCount_;
                } else {
                    ++repeatedStereoSubmissionCount_;
                    if (!repeatedStereoReuseLogged_) {
                        repeatedStereoReuseLogged_ = true;
                        log.Write(
                            "[VR][stereo] PROJECTION_REUSED generation=" +
                            std::to_string(lastSubmittedStereoGeneration_) +
                            " copy=skipped poseRevision=" +
                            std::to_string(
                                lastSubmittedStereoTrackingSample_.revision));
                    }
                }
                if (stereoSubmissionCount_ <= 4U ||
                    stereoSubmissionCount_ % 300U == 0U) {
                    std::ostringstream timing;
                    timing << "[VR][stereo] PROJECTION_TIMING samples="
                           << stereoSubmissionCount_
                           << " generation=" << stereoFrame->generation
                           << " new=" << newStereoFrame
                           << " copied=" << newStereoFrame
                           << " stagingSlot=" << stereoFrame->StagingSlot()
                           << " fresh=" << freshStereoSubmissionCount_
                           << " repeated=" << repeatedStereoSubmissionCount_
                           << " ageMs="
                           << static_cast<double>(stereoFrameAgeNanoseconds) /
                                  1'000'000.0
                           << " poseRevision="
                           << stereoFrame->trackingSample.revision
                           << " predictedDisplayTime="
                           << stereoFrame->trackingSample.predictedDisplayTime
                           << " submitDisplayTime="
                           << frameState.predictedDisplayTime
                           << " displayDeltaMs="
                           << static_cast<double>(
                                  frameState.predictedDisplayTime -
                                  stereoFrame->trackingSample.predictedDisplayTime) /
                                  1'000'000.0;
                    log.Write(timing.str());
                }
            }
        }
        const bool mirrorSwapchainReady = mirrorSwapchain_ != XR_NULL_HANDLE;
        UpdateStereoUiPanelState(
            frame.pointers,
            stereoSceneEligible,
            projectionReady,
            mirrorSwapchainReady,
            frameState.predictedDisplayTime,
            frame,
            log);
        bool gripNeedsMirrorRefresh = false;
        for (std::size_t hand = 0; hand < frame.pointers.size(); ++hand) {
            const auto& pointer = frame.pointers[hand];
            gripNeedsMirrorRefresh = gripNeedsMirrorRefresh || gripHeld_[hand] ||
                (pointer.gripActive &&
                 pointer.gripValue >= kGripPressThreshold);
        }
        const bool mirrorRefreshRequired =
            !projectionReady || stereoUiPanelVisible_ ||
            gripNeedsMirrorRefresh || panelAdjustMode_;
        if (mirrorRefreshRequired) {
            if (mirrorCopySuspended_) {
                mirrorCopySuspended_ = false;
                log.Write("[VR][display] MIRROR_COPY_RESUMED reason=grip-or-panel-or-fallback");
            }
            mirrorReady = RenderMirrorFrame(
                sourceFrame, sourceFrameGeneration, frame.pointers, log);
            mirrorResult = lastResult_;
            if (mirrorReady) {
                frame.sourceFrameGeneration = sourceFrameGeneration;
            }
        } else if (!mirrorCopySuspended_) {
            mirrorCopySuspended_ = true;
            log.Write("[VR][display] MIRROR_COPY_SUSPENDED reason=stereo-panel-hidden");
        }
        if (!mirrorReady) {
            frame.stereoUiPanelVisible = false;
            frame.mirrorInputEnabled = false;
        } else {
            frame.stereoUiPanelVisible =
                projectionReady && stereoUiPanelVisible_ && !aaMenuVisible_;
            frame.mirrorInputEnabled = !aaMenuVisible_ && !panelAdjustMode_ &&
                (!projectionReady || frame.stereoUiPanelVisible);
        }
        if (aaMenuVisible_) {
            menuReady = RenderMenuFrame(frame.pointers, log);
        }
        if (panelAdjustMode_ || panelToastKind_ != 0) {
            overlayReady = RenderPanelOverlayFrame(frame.pointers, log);
        }
    } else {
        UpdateStereoUiPanelState(
            frame.pointers,
            false,
            false,
            false,
            frameState.predictedDisplayTime,
            frame,
            log);
    }
    frame.aaMenuVisible = menuReady;

    // xrEndFrame is required for every successful xrBeginFrame, including
    // shouldRender == false and all xrLocateViews failure paths.
    // The game panel rides the adjustable placement: head-locked VIEW pose by
    // default, or the base-space pinned pose (direction frozen, position
    // following the head) computed by UpdatePanelPlacementFrame.
    XrSpace panelSpace = panelPoseUsesBase_
        ? (stageSpace_ != XR_NULL_HANDLE ? stageSpace_ : localSpace_)
        : viewSpace_;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = panelSpace;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = mirrorSwapchain_;
    quad.subImage.imageRect.extent.width = static_cast<int32_t>(mirrorWidth_);
    quad.subImage.imageRect.extent.height = static_cast<int32_t>(mirrorHeight_);
    quad.pose = ToXrPose(panelPoseUsesBase_ ? panelPoseBase_ : panelPoseView_);
    quad.size.width = panelQuadWidth_;
    quad.size.height = mirrorWidth_ == 0
        ? 0.0F
        : quad.size.width *
            static_cast<float>(mirrorHeight_) /
            static_cast<float>(mirrorWidth_);
    // Grip transparency: with the backbuffer cleared to alpha-0 black, UI
    // drawn via SrcAlpha blending is already premultiplied, matching the
    // OpenXR source-alpha convention. Only over a live projection layer —
    // the title / loading / missing-stereo fallback stays opaque.
    if (frame.gripPanelTransparent && projectionReady) {
        quad.layerFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    }
    // Adjust bar: panel-attached strip from the shared overlay texture.
    XrCompositionLayerQuad barQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    barQuad.space = panelSpace;
    barQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    barQuad.subImage.swapchain = panelOverlaySwapchain_;
    barQuad.subImage.imageRect.offset = {0, 0};
    barQuad.subImage.imageRect.extent.width =
        static_cast<int32_t>(kPanelOverlayTextureWidth);
    barQuad.subImage.imageRect.extent.height =
        static_cast<int32_t>(kPanelOverlayBarHeight);
    barQuad.pose = ToXrPose(panelPoseUsesBase_ ? barPoseBase_ : barPoseView_);
    barQuad.size.width = barQuadWidth_;
    barQuad.size.height = barQuadHeight_;
    // Hint / toast: head-locked quad cropped to the fitted text box the
    // painter reported (centered horizontally in the strip).
    constexpr float kHintMetresPerPixel =
        kHintQuadWidthMetres / static_cast<float>(kPanelOverlayTextureWidth);
    const std::uint32_t hintWidthPx =
        std::min(panelHintWidthPx_, kPanelOverlayTextureWidth);
    const std::uint32_t hintHeightPx =
        std::min(panelHintHeightPx_, kPanelOverlayHintHeight);
    XrCompositionLayerQuad hintQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    hintQuad.space = viewSpace_;
    hintQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    hintQuad.subImage.swapchain = panelOverlaySwapchain_;
    hintQuad.subImage.imageRect.offset = {
        static_cast<int32_t>((kPanelOverlayTextureWidth - hintWidthPx) / 2U),
        static_cast<int32_t>(kPanelOverlayHintTop)};
    hintQuad.subImage.imageRect.extent.width =
        static_cast<int32_t>(hintWidthPx);
    hintQuad.subImage.imageRect.extent.height =
        static_cast<int32_t>(hintHeightPx);
    hintQuad.pose = ToXrPose(hintPoseView_);
    hintQuad.size.width = static_cast<float>(hintWidthPx) * kHintMetresPerPixel;
    hintQuad.size.height =
        static_cast<float>(hintHeightPx) * kHintMetresPerPixel;
    XrCompositionLayerQuad menuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    menuQuad.space = viewSpace_;
    menuQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    menuQuad.subImage.swapchain = menuSwapchain_;
    menuQuad.subImage.imageRect.extent.width = static_cast<int32_t>(menuWidth_);
    menuQuad.subImage.imageRect.extent.height = static_cast<int32_t>(menuHeight_);
    menuQuad.pose.orientation.w = 1.0F;
    menuQuad.pose.position.z = kMenuPlaneZ;
    menuQuad.size.width = kMenuWidthMetres;
    menuQuad.size.height = kMenuHeightMetres;
    std::array<XrCompositionLayerProjectionView, 2> projectionViews{
        XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
    };
    XrCompositionLayerProjection projection{
        XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = stageSpace_ != XR_NULL_HANDLE ? stageSpace_ : localSpace_;
    projection.viewCount = static_cast<uint32_t>(projectionViews.size());
    projection.views = projectionViews.data();
    if (projectionReady) {
        for (std::size_t eyeIndex = 0; eyeIndex < projectionViews.size(); ++eyeIndex) {
            const auto& trackedEye =
                projectionTrackingSample->eyes[eyeIndex];
            projectionViews[eyeIndex].pose.position = {
                trackedEye.pose.position.x,
                trackedEye.pose.position.y,
                trackedEye.pose.position.z,
            };
            projectionViews[eyeIndex].pose.orientation = {
                trackedEye.pose.orientation.x,
                trackedEye.pose.orientation.y,
                trackedEye.pose.orientation.z,
                trackedEye.pose.orientation.w,
            };
            projectionViews[eyeIndex].fov = {
                trackedEye.fov.angleLeft,
                trackedEye.fov.angleRight,
                trackedEye.fov.angleUp,
                trackedEye.fov.angleDown,
            };
            projectionViews[eyeIndex].subImage.swapchain = projectionSwapchain_;
            projectionViews[eyeIndex].subImage.imageRect.extent.width =
                static_cast<int32_t>(projectionSourceDescription_.Width);
            projectionViews[eyeIndex].subImage.imageRect.extent.height =
                static_cast<int32_t>(projectionSourceDescription_.Height);
            projectionViews[eyeIndex].subImage.imageArrayIndex =
                static_cast<uint32_t>(eyeIndex);
        }
    }
    std::array<const XrCompositionLayerBaseHeader*, 5> layers{};
    uint32_t layerCount = 0;
    if (projectionReady) {
        layers[layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    }
    // A projection frame is the immersive view. The opaque desktop Quad is
    // the Grip-summoned UI panel over stereo, and the fallback for title /
    // loading / any missing stereo frame; if it were layered after
    // projection while hidden it would completely cover the 3D scene.
    // The settings quad replaces the ordinary 2D fallback while it is open.
    // Closing (or a paint failure) makes the continuously refreshed 2D quad
    // return on the following frame without changing any Unity camera state.
    const bool mirrorLayerVisible = mirrorReady && !frame.aaMenuVisible &&
        (!projectionReady || frame.stereoUiPanelVisible);
    if (mirrorLayerVisible) {
        layers[layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }
    // Adjust-mode overlays: the bar rides the panel, the hint/toast strip is
    // head-locked. The toast also shows without adjust mode (photo scenes).
    if (overlayReady && panelAdjustMode_ && mirrorLayerVisible) {
        layers[layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&barQuad);
    }
    if (overlayReady && hintWidthPx != 0 && hintHeightPx != 0 &&
        (panelAdjustMode_ || panelToastKind_ != 0)) {
        layers[layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hintQuad);
    }
    // The settings menu is the topmost layer and never replaces the scene.
    if (menuReady) {
        layers[layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad);
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = environmentBlendMode_;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount != 0 ? layers.data() : nullptr;
    frame.layerSubmitted = layerCount != 0;
    frame.stereoLayerSubmitted = projectionReady;
    const XrResult endResult = dispatch_.EndFrame()(session_, &endInfo);
    if (XR_FAILED(endResult)) {
        lastResult_ = endResult;
        log.Write(
            "[VR][runtime] xrEndFrame failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return ClassifyFrameResult(lastResult_);
    }
    sessionLossPending =
        sessionLossPending || endResult == XR_SESSION_LOSS_PENDING;

    if (frame.referenceSpaceChanged) {
        projectionTrackingTimeFloor_ = effectiveProjectionTrackingTimeFloor;
        pendingReferenceSpaceChangeTimes_.erase(
            pendingReferenceSpaceChangeTimes_.begin(),
            pendingReferenceSpaceChangeTimes_.begin() +
                static_cast<std::ptrdiff_t>(appliedReferenceSpaceChangeCount));
        log.Write(
            std::string("[VR][pose] REFERENCE_SPACE_CHANGE_APPLIED active=") +
            ReferenceSpaceText(ActiveReferenceSpaceType()) +
            " changeTime=" + std::to_string(appliedReferenceSpaceChangeTime) +
            " predictedDisplayTime=" + std::to_string(frame.predictedDisplayTime) +
            " coalesced=" + std::to_string(appliedReferenceSpaceChangeCount));
    }

    if (!mirrorReady && !projectionReady && frame.shouldRender) {
        lastResult_ = mirrorResult;
        return ClassifyFrameResult(lastResult_);
    }

    if (XR_FAILED(locateResult)) {
        lastResult_ = locateResult;
        return ClassifyFrameResult(lastResult_);
    }
    if (viewCount != views.size()) {
        lastResult_ = XR_ERROR_RUNTIME_FAILURE;
        log.Write(
            "[VR][runtime] xrLocateViews returned " + std::to_string(viewCount) +
            " view(s); PRIMARY_STEREO requires exactly 2");
        return FrameResult::Failed;
    }
    if (sessionLossPending) {
        lastResult_ = XR_SESSION_LOSS_PENDING;
        return FrameResult::SessionLossPending;
    }

    lastResult_ = XR_SUCCESS;
    return FrameResult::Completed;
}

bool OpenXrContext::RequestExit(VrLog& log) {
    if (exitRequested_) {
        return true;
    }
    if (session_ == XR_NULL_HANDLE || !sessionRunning_ ||
        dispatch_.RequestExitSession() == nullptr) {
        lastResult_ = XR_ERROR_SESSION_NOT_RUNNING;
        return false;
    }

    lastResult_ = dispatch_.RequestExitSession()(session_);
    if (XR_FAILED(lastResult_)) {
        log.Write(
            "[VR][runtime] xrRequestExitSession failed: " +
            dispatch_.ResultText(instance_, lastResult_));
        return false;
    }
    exitRequested_ = true;
    log.Write("[VR][runtime] xrRequestExitSession succeeded");
    return true;
}

void OpenXrContext::ResetMenuSwapchain() noexcept {
    if (aaMenuShutdown_ != nullptr) {
        aaMenuShutdown_();
    }
    menuImages_.clear();
    if (menuSwapchain_ != XR_NULL_HANDLE &&
        dispatch_.DestroySwapchain() != nullptr) {
        dispatch_.DestroySwapchain()(menuSwapchain_);
    }
    menuSwapchain_ = XR_NULL_HANDLE;
    menuWidth_ = 0;
    menuHeight_ = 0;
    menuSwapchainFormat_ = 0;
    menuSwapchainFailed_ = false;
    menuPaintFailures_ = 0;
}

void OpenXrContext::ReleasePortraitLatch() noexcept {
    if (portraitLatchTexture_ != nullptr) {
        portraitLatchTexture_->Release();
        portraitLatchTexture_ = nullptr;
    }
    portraitLatchWidth_ = 0;
    portraitLatchHeight_ = 0;
    portraitLatchFormat_ = DXGI_FORMAT_UNKNOWN;
    portraitLatchValid_ = false;
    portraitLatchCaptureLogged_ = false;
    portraitLatchFrozenLogged_ = false;
    portraitLatchFollowLogged_ = false;
}

bool OpenXrContext::EnsurePortraitLatch(
    const D3D11_TEXTURE2D_DESC& sourceDescription,
    VrLog& log) noexcept {
    if (portraitLatchTexture_ != nullptr &&
        portraitLatchWidth_ == sourceDescription.Width &&
        portraitLatchHeight_ == sourceDescription.Height &&
        portraitLatchFormat_ == sourceDescription.Format) {
        return true;
    }
    ReleasePortraitLatch();
    if (sessionDevice_ == nullptr ||
        sourceDescription.Width == 0 || sourceDescription.Height == 0) {
        return false;
    }
    D3D11_TEXTURE2D_DESC latchDescription = sourceDescription;
    latchDescription.MipLevels = 1;
    latchDescription.ArraySize = 1;
    latchDescription.SampleDesc.Count = 1;
    latchDescription.SampleDesc.Quality = 0;
    latchDescription.Usage = D3D11_USAGE_DEFAULT;
    latchDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    latchDescription.CPUAccessFlags = 0;
    latchDescription.MiscFlags = 0;
    ID3D11Texture2D* texture = nullptr;
    const HRESULT created =
        sessionDevice_->CreateTexture2D(&latchDescription, nullptr, &texture);
    if (FAILED(created) || texture == nullptr) {
        log.Write(
            "[VR][display] PORTRAIT_LATCH create failed hr=" +
            std::to_string(static_cast<long>(created)));
        return false;
    }
    portraitLatchTexture_ = texture;
    portraitLatchWidth_ = sourceDescription.Width;
    portraitLatchHeight_ = sourceDescription.Height;
    portraitLatchFormat_ = sourceDescription.Format;
    return true;
}

void OpenXrContext::ResetMirrorSwapchain() noexcept {
    ReleasePortraitLatch();
    mirrorImages_.clear();
    if (mirrorSwapchain_ != XR_NULL_HANDLE &&
        dispatch_.DestroySwapchain() != nullptr) {
        dispatch_.DestroySwapchain()(mirrorSwapchain_);
    }
    mirrorSwapchain_ = XR_NULL_HANDLE;
    mirrorWidth_ = 0;
    mirrorHeight_ = 0;
    mirrorLayoutGeneration_ = 0;
    mirrorSourceFormat_ = DXGI_FORMAT_UNKNOWN;
    mirrorSwapchainFormat_ = 0;
    lastMirrorFingerprintGeneration_ = 0;
    mirrorFingerprint_ = 0;
    mirrorFingerprintValid_ = false;
    if (sessionContext_ != nullptr) {
        sessionContext_->Release();
        sessionContext_ = nullptr;
    }
}

void OpenXrContext::ResetProjectionSwapchain() noexcept {
    projectionVerticalFlip_.Reset();
    projectionImages_.clear();
    if (projectionSwapchain_ != XR_NULL_HANDLE &&
        dispatch_.DestroySwapchain() != nullptr) {
        dispatch_.DestroySwapchain()(projectionSwapchain_);
    }
    projectionSwapchain_ = XR_NULL_HANDLE;
    projectionSwapchainFormat_ = 0;
    projectionSourceDescription_ = {};
    lastProjectionFingerprintGeneration_ = 0;
    projectionFingerprints_.fill(0);
    projectionFingerprintValid_.fill(false);
    lastSubmittedStereoGeneration_ = 0;
    lastSubmittedStereoTrackingSample_ = {};
    lastSubmittedStereoHostPublishTimeNanoseconds_ = 0;
    stereoSubmissionCount_ = 0;
    freshStereoSubmissionCount_ = 0;
    repeatedStereoSubmissionCount_ = 0;
    repeatedStereoReuseLogged_ = false;
}

void OpenXrContext::ResetInputSession() noexcept {
    pose::HandTrackingMailbox().Invalidate();
    for (auto& space : aimSpaces_) {
        if (space != XR_NULL_HANDLE && dispatch_.DestroySpace() != nullptr) {
            dispatch_.DestroySpace()(space);
        }
        space = XR_NULL_HANDLE;
    }
    for (auto& space : gripSpaces_) {
        if (space != XR_NULL_HANDLE && dispatch_.DestroySpace() != nullptr) {
            dispatch_.DestroySpace()(space);
        }
        space = XR_NULL_HANDLE;
    }
    triggerHeld_.fill(false);
    mirrorPointerSmoothers_ = {};
    menuPointerSmoothers_ = {};
    pointerFilterTime_.fill(0);
    pointerFilterLayoutGeneration_ = 0;
    pointerSmoothLogged_ = false;
    gripStateInitialized_.fill(false);
    gripHeld_.fill(false);
    gripGestureValid_.fill(false);
    gripLongPressFired_.fill(false);
    gripPressTime_.fill(0);
    gripToggleCooldownUntil_ = 0;
    livePauseHeld_ = false;
    livePauseInactiveSince_ = 0;
    livePauseToggleCount_.store(0U, std::memory_order_release);
    cameraModeHeld_ = false;
    cameraCharaHeld_ = false;
    cameraResetHeld_ = false;
    cameraResetFired_ = false;
    cameraResetPressTime_ = 0;
    stereoUiPanelVisible_ = false;
    aaMenuVisible_ = false;
    menuPointerHand_ = 1;
    stereoSceneEligible_ = false;
    // Adjust-mode transients die with the session; the placement itself is
    // config-backed and survives. Dirty edits are not saved here: session
    // teardown may run without a usable log/config context, and A-exit /
    // scene-exit already save.
    panelAdjustMode_ = false;
    panelAdjustItem_ = 0;
    panelAdjustButtonHeld_ = false;
    panelGrabActive_.fill(false);
    panelHandLastValid_.fill(false);
    panelScaleActive_ = false;
    panelBarPointerHand_ = 1;
    panelPinnedAnchorValid_ = false;
    headPoseBaseValid_ = false;
    panelToastKind_ = 0;
    panelToastUntil_ = 0;
    photoResultPendingUntil_ = 0;
    photoResultGenerationInitialized_ = false;
    mirrorInputStateInitialized_ = false;
    lastMirrorInputEnabled_ = true;
    mirrorCopySuspended_ = false;
    inputSessionReady_ = false;
    inputErrorLogged_ = false;
}

void OpenXrContext::ResetInputActions() noexcept {
    if (panelAdjustAction_ != XR_NULL_HANDLE && dispatch_.DestroyAction() != nullptr) {
        dispatch_.DestroyAction()(panelAdjustAction_);
    }
    panelAdjustAction_ = XR_NULL_HANDLE;
    panelAdjustButtonHeld_ = false;
    if (cameraResetAction_ != XR_NULL_HANDLE && dispatch_.DestroyAction() != nullptr) {
        dispatch_.DestroyAction()(cameraResetAction_);
    }
    cameraResetAction_ = XR_NULL_HANDLE;
    if (cameraCharaAction_ != XR_NULL_HANDLE && dispatch_.DestroyAction() != nullptr) {
        dispatch_.DestroyAction()(cameraCharaAction_);
    }
    cameraCharaAction_ = XR_NULL_HANDLE;
    if (cameraModeAction_ != XR_NULL_HANDLE && dispatch_.DestroyAction() != nullptr) {
        dispatch_.DestroyAction()(cameraModeAction_);
    }
    cameraModeAction_ = XR_NULL_HANDLE;
    cameraModeHeld_ = false;
    cameraCharaHeld_ = false;
    cameraResetHeld_ = false;
    cameraResetFired_ = false;
    cameraResetPressTime_ = 0;
    if (livePauseAction_ != XR_NULL_HANDLE && dispatch_.DestroyAction() != nullptr) {
        dispatch_.DestroyAction()(livePauseAction_);
    }
    livePauseAction_ = XR_NULL_HANDLE;
    livePauseHeld_ = false;
    if (thumbstickAction_ != XR_NULL_HANDLE && dispatch_.DestroyAction() != nullptr) {
        dispatch_.DestroyAction()(thumbstickAction_);
    }
    thumbstickAction_ = XR_NULL_HANDLE;
    if (gripAction_ != XR_NULL_HANDLE && dispatch_.DestroyAction() != nullptr) {
        dispatch_.DestroyAction()(gripAction_);
    }
    gripAction_ = XR_NULL_HANDLE;
    if (gripPoseAction_ != XR_NULL_HANDLE && dispatch_.DestroyAction() != nullptr) {
        dispatch_.DestroyAction()(gripPoseAction_);
    }
    gripPoseAction_ = XR_NULL_HANDLE;
    if (triggerAction_ != XR_NULL_HANDLE && dispatch_.DestroyAction() != nullptr) {
        dispatch_.DestroyAction()(triggerAction_);
    }
    triggerAction_ = XR_NULL_HANDLE;
    if (aimPoseAction_ != XR_NULL_HANDLE && dispatch_.DestroyAction() != nullptr) {
        dispatch_.DestroyAction()(aimPoseAction_);
    }
    aimPoseAction_ = XR_NULL_HANDLE;
    if (inputActionSet_ != XR_NULL_HANDLE && dispatch_.DestroyActionSet() != nullptr) {
        dispatch_.DestroyActionSet()(inputActionSet_);
    }
    inputActionSet_ = XR_NULL_HANDLE;
    handPaths_.fill(XR_NULL_PATH);
    inputActionsCreated_ = false;
}

void OpenXrContext::ResetSessionChildren() noexcept {
    pendingReferenceSpaceChangeTimes_.clear();
    projectionTrackingTimeFloor_ = 0;
    ResetProjectionSwapchain();
    ResetMenuSwapchain();
    ResetPanelOverlaySwapchain();
    ResetMirrorSwapchain();
    ResetInputSession();
    // Spaces are children of the session and must be released first.
    if (stageSpace_ != XR_NULL_HANDLE && dispatch_.DestroySpace() != nullptr) {
        dispatch_.DestroySpace()(stageSpace_);
    }
    stageSpace_ = XR_NULL_HANDLE;
    if (localSpace_ != XR_NULL_HANDLE && dispatch_.DestroySpace() != nullptr) {
        dispatch_.DestroySpace()(localSpace_);
    }
    localSpace_ = XR_NULL_HANDLE;
    if (viewSpace_ != XR_NULL_HANDLE && dispatch_.DestroySpace() != nullptr) {
        dispatch_.DestroySpace()(viewSpace_);
    }
    viewSpace_ = XR_NULL_HANDLE;
}

void OpenXrContext::ClearSessionState() noexcept {
    session_ = XR_NULL_HANDLE;
    sessionState_ = XR_SESSION_STATE_UNKNOWN;
    sessionRunning_ = false;
    exitRequested_ = false;
    sessionRunGeneration_ = 0;
    environmentBlendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    projectionDisabledForSession_ = false;
    if (sessionDevice_ != nullptr) {
        sessionDevice_->Release();
        sessionDevice_ = nullptr;
    }
}

void OpenXrContext::ResetSession() noexcept {
    ResetSessionChildren();
    if (session_ != XR_NULL_HANDLE && dispatch_.DestroySession() != nullptr) {
        dispatch_.DestroySession()(session_);
    }
    ClearSessionState();
}

bool OpenXrContext::ResetGraphicsSession(VrLog& log) noexcept {
    if (sessionRunning_) {
        lastResult_ = XR_ERROR_SESSION_RUNNING;
        log.Write(
            "[VR][display] refusing to destroy a running OpenXR session for "
            "graphics rebuild");
        return false;
    }
    ResetSessionChildren();
    if (session_ != XR_NULL_HANDLE) {
        if (dispatch_.DestroySession() == nullptr) {
            lastResult_ = XR_ERROR_FUNCTION_UNSUPPORTED;
            log.Write(
                "[VR][display] xrDestroySession is unavailable during graphics rebuild");
            return false;
        }
        lastResult_ = dispatch_.DestroySession()(session_);
        if (XR_FAILED(lastResult_)) {
            log.Write(
                "[VR][display] xrDestroySession failed during graphics rebuild: " +
                dispatch_.ResultText(instance_, lastResult_));
            return false;
        }
    }
    ClearSessionState();
    lastResult_ = XR_SUCCESS;
    log.Write("[VR][display] graphics-bound OpenXR session reset");
    return true;
}

void OpenXrContext::ResetInstance() noexcept {
    ResetSession();
    systemId_ = XR_NULL_SYSTEM_ID;
    mirrorMaximumWidth_ = 0;
    mirrorMaximumHeight_ = 0;
    runtimeName_.clear();
    runtimeVersionText_.clear();
    systemName_.clear();
    consoleSummaryLogged_ = false;
    bdControllerInteractionAvailable_ = false;
    ResetInputActions();

    if (instance_ != XR_NULL_HANDLE && dispatch_.DestroyInstance() != nullptr) {
        dispatch_.DestroyInstance()(instance_);
    }
    instance_ = XR_NULL_HANDLE;
    dispatch_.ClearInstanceFunctions();
}

void OpenXrContext::Reset() noexcept {
    ResetInstance();
    dispatch_.Unload();
}

bool OpenXrContext::HasInstance() const noexcept {
    return instance_ != XR_NULL_HANDLE;
}

bool OpenXrContext::HasSystem() const noexcept {
    return systemId_ != XR_NULL_SYSTEM_ID;
}

bool OpenXrContext::HasSession() const noexcept {
    return session_ != XR_NULL_HANDLE;
}

XrSessionState OpenXrContext::SessionState() const noexcept {
    return sessionState_;
}

bool OpenXrContext::IsSessionRunning() const noexcept {
    return sessionRunning_;
}

std::uint64_t OpenXrContext::SessionRunGeneration() const noexcept {
    return sessionRunGeneration_;
}

XrEnvironmentBlendMode OpenXrContext::EnvironmentBlendMode() const noexcept {
    return environmentBlendMode_;
}

XrReferenceSpaceType OpenXrContext::ActiveReferenceSpaceType() const noexcept {
    return stageSpace_ != XR_NULL_HANDLE
               ? XR_REFERENCE_SPACE_TYPE_STAGE
               : XR_REFERENCE_SPACE_TYPE_LOCAL;
}

bool OpenXrContext::HasStageSpace() const noexcept {
    return stageSpace_ != XR_NULL_HANDLE;
}

std::uint32_t OpenXrContext::StereoEyeWidth() const noexcept {
    return stereoProjectionEnabled_ ? stereoEyeWidth_ : 0U;
}

std::uint32_t OpenXrContext::StereoEyeHeight() const noexcept {
    return stereoProjectionEnabled_ ? stereoEyeHeight_ : 0U;
}

XrResult OpenXrContext::LastResult() const noexcept {
    return lastResult_;
}

bool OpenXrContext::ConsumeLivePauseToggle() noexcept {
    auto pending = livePauseToggleCount_.load(std::memory_order_acquire);
    while (pending > 0U) {
        if (livePauseToggleCount_.compare_exchange_weak(
                pending,
                pending - 1U,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

std::uint32_t OpenXrContext::CameraModePressCount() const noexcept {
    return cameraModePressCount_.load(std::memory_order_acquire);
}

std::uint32_t OpenXrContext::CameraCharaPressCount() const noexcept {
    return cameraCharaPressCount_.load(std::memory_order_acquire);
}

std::uint32_t OpenXrContext::CameraResetPressCount() const noexcept {
    return cameraResetPressCount_.load(std::memory_order_acquire);
}

std::uint32_t OpenXrContext::PhotoPressCount() const noexcept {
    return photoPressCount_.load(std::memory_order_acquire);
}

void OpenXrContext::SetAaMenuHooks(
    AaMenuPaintFn paint, AaMenuShutdownFn shutdown, AaMenuFlushFn flush) noexcept {
    aaMenuPainter_ = paint;
    aaMenuShutdown_ = shutdown;
    aaMenuFlush_ = flush;
}

void OpenXrContext::SetPanelOverlayHooks(PanelOverlayPaintFn paint) noexcept {
    panelOverlayPainter_ = paint;
}

OpenXrContext::EventResult OpenXrContext::ClassifyEventResult(
    XrResult result) noexcept {
    if (result == XR_ERROR_INSTANCE_LOST) {
        return EventResult::InstanceLost;
    }
    if (result == XR_ERROR_SESSION_LOST) {
        return EventResult::SessionLost;
    }
    return EventResult::Failed;
}

OpenXrContext::FrameResult OpenXrContext::ClassifyFrameResult(
    XrResult result) noexcept {
    if (result == XR_SESSION_LOSS_PENDING) {
        return FrameResult::SessionLossPending;
    }
    if (result == XR_ERROR_SESSION_NOT_RUNNING) {
        return FrameResult::SessionNotRunning;
    }
    if (result == XR_ERROR_SESSION_LOST) {
        return FrameResult::SessionLost;
    }
    if (result == XR_ERROR_INSTANCE_LOST) {
        return FrameResult::InstanceLost;
    }
    return FrameResult::Failed;
}

bool OpenXrContext::EqualLuid(const LUID& left, const LUID& right) noexcept {
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

bool OpenXrContext::IsSupportedMirrorFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

bool OpenXrContext::AreCopyCompatibleFormats(
    DXGI_FORMAT left,
    DXGI_FORMAT right) noexcept {
    const auto family = [](DXGI_FORMAT format) noexcept {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return 1;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return 2;
        default:
            return 0;
        }
    };
    const int leftFamily = family(left);
    return leftFamily != 0 && leftFamily == family(right);
}

} // namespace gakumas::vr::openxr
