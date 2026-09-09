#pragma once

#include "input/PointerGesture.hpp"

#include "HookRegistrar.hpp"
#include "VrCameraInputMailbox.hpp"
#include "VrLog.hpp"
#include "d3d11/D3D11Capture.hpp"
#include "d3d11/StereoRenderMailbox.hpp"
#include "frame/FrameCoordinator.hpp"
#include "frame/FrameEndDispatch.hpp"
#include "input/ThumbstickScroll.hpp"
#include "openxr/OpenXrContext.hpp"
#include "pose/StereoPoseMailbox.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>

namespace gakumas::vr {

enum class VrRuntimeState {
    Disabled,
    WaitingForRuntime,
    WaitingForGraphics,
    InstanceReady,
    SystemReady,
    SessionReady,
    SessionRunning,
    PoseReady,
    Faulted,
    Stopped,
};

struct VrRuntimeConfig {
    // Safe default: core/config integration must opt into the runtime explicitly.
    bool enabled = false;
    bool diagnosticsEnabled = false;
    bool cameraPoseBridgeEnabled = false;
    bool stereoProjectionEnabled = false;
    bool stereoLandscapeOnly = false;
    float stereoRenderScale = 1.0F;
    std::filesystem::path applicationDirectory;
    std::chrono::milliseconds runtimeRetryInterval{1000};
    std::chrono::milliseconds graphicsPollInterval{250};
};

class VrRuntime final {
public:
    struct StereoRenderTargetSpec {
        bool enabled = false;
        bool landscape = false;
        std::uint32_t eyeWidth = 0;
        std::uint32_t eyeHeight = 0;
        std::uint64_t generation = 0;
        std::uint64_t mirrorLayoutGeneration = 0;
    };

    static VrRuntime& Instance();

    VrRuntime(const VrRuntime&) = delete;
    VrRuntime& operator=(const VrRuntime&) = delete;

    // Must be called outside DllMain/loader lock. The returned success only
    // means the worker was started; PoseReady is reported asynchronously.
    bool Start(VrRuntimeConfig config, HookRegistrar registrar);
    void Stop() noexcept;

    [[nodiscard]] VrRuntimeState State() const noexcept;
    [[nodiscard]] XrResult LastOpenXrResult() const noexcept;
    [[nodiscard]] std::filesystem::path LogPath() const;
    [[nodiscard]] bool ReadLatestStereoPose(
        pose::StereoPoseSample& sample) const noexcept;
    [[nodiscard]] StereoRenderTargetSpec ReadStereoRenderTargetSpec() const noexcept;
    [[nodiscard]] bool PublishUnityStereoFrame(
        ID3D11Texture2D* left,
        ID3D11Texture2D* right,
        const pose::StereoPoseSample& trackingSample,
        d3d11::StereoRenderMailbox::PublishDiagnostics* diagnostics = nullptr) noexcept;
    void InvalidateUnityStereoFrame() noexcept;
    // Unity hooks execute on the game/render thread. VrLog serializes these
    // sparse camera diagnostics with the OpenXR worker log.
    [[nodiscard]] bool WriteVrLog(std::string_view message) noexcept;
    // Game-thread relay of UnityStereoRenderer::GripTransparencyArmed():
    // arms the Present-hook transparent backbuffer clear and the worker's
    // source-alpha quad submission. Atomic stores only; safe off-worker.
    void SetGripPanelTransparent(bool active) noexcept;
    // .226: native ID3D11Texture2D pointers of the big Unity RenderTextures;
    // the Present hook adopts the swapchain-sized ones and clears them to
    // transparent black each frame while transparency is armed. Empty set
    // releases the adopted references (disarm path).
    void SetGripPanelClearTargets(
        const void* const* textures, std::size_t count) noexcept;
    [[nodiscard]] bool ConsumeLivePauseToggle() noexcept;
    [[nodiscard]] bool ReadVrCameraInput(
        camera::VrCameraInputSample& sample) const noexcept;

    void OnUnityWaitPhase() noexcept;
    [[nodiscard]] int OnUnitySubmitPhase() noexcept;
    void OnGraphicsEndEvent(int eventId) noexcept;
    void EnsureGraphicsBegun() noexcept;
    [[nodiscard]] bool CurrentPoseAdmission(pose::PoseAdmission& admission) const noexcept;
    [[nodiscard]] bool ReadStereoPoseForAdmission(
        const pose::PoseAdmission& admission,
        pose::StereoPoseSample& sample) const noexcept;
    void PumpStandaloneFrame() noexcept;

private:
    [[nodiscard]] bool ShouldDeferStereoSubmit() const noexcept;
    VrRuntime() = default;
    ~VrRuntime();

    void WorkerMain() noexcept;
    void WorkerMainImpl();
    void DispatchPointerInput(
        const openxr::OpenXrContext::StereoFrame& frame,
        HWND outputWindow);
    void ReleasePointerDrag() noexcept;
    void ResetThumbstickScroll(std::string_view reason = {}) noexcept;
    void PublishStereoTargetSpecChange();
    bool EnsureD3D11Hooks();
    bool WaitOrStop(std::chrono::milliseconds timeout);
    void SetState(VrRuntimeState state);
    void Fault(std::string_view reason);
    static std::filesystem::path ResolveApplicationDirectory();

    std::mutex lifecycleMutex_;
    VrRuntimeConfig config_;
    HookRegistrar registrar_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<VrRuntimeState> state_{VrRuntimeState::Disabled};
    std::atomic<XrResult> lastOpenXrResult_{XR_SUCCESS};
    std::mutex waitMutex_;
    std::condition_variable stopChanged_;

    VrLog log_;
    d3d11::D3D11Capture d3d11Capture_;
    d3d11::StereoRenderMailbox stereoRenderMailbox_;
    openxr::OpenXrContext openXr_;
    pose::StereoPoseMailbox poseMailbox_;
    camera::VrCameraInputMailbox cameraInputMailbox_;
    std::atomic<std::uint32_t> stereoEyeWidth_{0};
    std::atomic<std::uint32_t> stereoEyeHeight_{0};
    std::atomic<std::uint64_t> stereoTargetGeneration_{0};
    // A single publication keeps the confirmed capture-layout generation and
    // orientation coherent for Unity's render thread. Zero means that the
    // latest capture is unavailable or is still proving a layout transition.
    std::atomic<std::uint64_t> confirmedMirrorLayout_{0};
    // Game thread writes (SetGripPanelTransparent), worker reads per frame.
    std::atomic<bool> gripPanelTransparent_{false};
    std::uint64_t lastLoggedProjectionFrameGeneration_ = 0;
    bool d3d11HooksReady_ = false;
    std::size_t activePointerHand_ = 1;
    std::size_t draggingPointerHand_ = 2;
    HWND dragWindow_ = nullptr;
    POINT dragStartClient_{};
    POINT lastDragClient_{};
    float dragStartU_ = 0.0F;
    float dragStartV_ = 0.0F;
    bool dragMoved_ = false;
    bool dragTestBackend_ = false;
    input::PointerGesture pointerGesture_{};
    input::ThumbstickScrollIntegrator thumbstickScrollHorizontal_{};
    input::ThumbstickScrollIntegrator thumbstickScrollVertical_{};
    std::size_t scrollingPointerHand_ = 2;
    XrTime nextScrollLogTime_ = 0;
    std::uint64_t inputMirrorLayoutGeneration_ = 0;
    bool inputLayoutReady_ = false;
    bool inputLayoutMismatchLogged_ = false;
    bool inputLayoutPendingLogged_ = false;

    d3d11::D3D11Capture::Snapshot sessionGraphics_{};
    d3d11::D3D11Capture::FrameSnapshot activeSourceFrame_{};
    frame::FrameIdentity prepareTicket_{};
    openxr::OpenXrContext::StereoFrame prepareFrame_{};
    std::atomic<bool> unityDriving_{false};
    std::atomic<bool> restartGraphicsRequested_{false};
    std::uint64_t observedRunGeneration_{0};
    bool ticketPrepared_ = false;
    bool ticketBegun_ = false;
    bool ticketSubmitted_ = false;
    std::uint32_t endEventSerial_ = 0;
    bool currentRunPoseReady_ = false;
    bool currentRunDisplayReady_ = false;
    bool currentRunInputReady_ = false;
    bool everPoseReady_ = false;
    bool runtimeReadyLogged_ = false;
    bool displayPaused_ = false;

    void PublishPreparedOutputs(std::uint64_t runGeneration);
    void NoteSubmittedOutputs(
        const openxr::OpenXrContext::StereoFrame& frame, std::uint64_t runGeneration);
    bool RefreshMirrorSource();
    bool ProcessUnityEvents() noexcept;
    frame::FrameEndDispatch endDispatch_;
    d3d11::D3D11Capture::FrameSnapshot queuedSourceFrame_;
};

[[nodiscard]] const char* VrRuntimeStateName(VrRuntimeState state) noexcept;

inline bool StartVrRuntime(VrRuntimeConfig config, HookRegistrar registrar) {
    return VrRuntime::Instance().Start(std::move(config), registrar);
}

inline void StopVrRuntime() noexcept {
    VrRuntime::Instance().Stop();
}

inline bool WriteVrLog(std::string_view message) noexcept {
    return VrRuntime::Instance().WriteVrLog(message);
}

inline void SetGripPanelTransparent(bool active) noexcept {
    VrRuntime::Instance().SetGripPanelTransparent(active);
}

inline void SetGripPanelClearTargets(
    const void* const* textures, std::size_t count) noexcept {
    VrRuntime::Instance().SetGripPanelClearTargets(textures, count);
}

inline bool ConsumeLivePauseToggle() noexcept {
    return VrRuntime::Instance().ConsumeLivePauseToggle();
}

inline bool ReadLatestStereoPose(pose::StereoPoseSample& sample) noexcept {
    return VrRuntime::Instance().ReadLatestStereoPose(sample);
}

inline bool ReadVrCameraInput(camera::VrCameraInputSample& sample) noexcept {
    return VrRuntime::Instance().ReadVrCameraInput(sample);
}

inline VrRuntime::StereoRenderTargetSpec ReadStereoRenderTargetSpec() noexcept {
    return VrRuntime::Instance().ReadStereoRenderTargetSpec();
}

inline bool PublishUnityStereoFrame(
    ID3D11Texture2D* left,
    ID3D11Texture2D* right,
    const pose::StereoPoseSample& trackingSample,
    d3d11::StereoRenderMailbox::PublishDiagnostics* diagnostics = nullptr) noexcept {
    return VrRuntime::Instance().PublishUnityStereoFrame(
        left, right, trackingSample, diagnostics);
}

inline void InvalidateUnityStereoFrame() noexcept {
    VrRuntime::Instance().InvalidateUnityStereoFrame();
}

} // namespace gakumas::vr
