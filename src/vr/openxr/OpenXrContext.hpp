#pragma once

#include "OpenXrDispatch.hpp"
#include "../FrameHitchProbe.hpp"
#include "../PanelPlacement.hpp"
#include "../d3d11/StereoRenderMailbox.hpp"
#include "../d3d11/VerticalFlipPass.hpp"
#include "../frame/FrameCoordinator.hpp"
#include "../input/PointerSmoother.hpp"
#include "../pose/PoseMath.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gakumas::vr {
class VrLog;
}

namespace gakumas::vr::openxr {

class OpenXrContext final {
public:
    enum class InitializeResult {
        Ready,
        RetryRuntime,
        LoaderMissing,
        ExtensionMissing,
        Failed,
    };

    enum class SystemResult {
        Ready,
        RetryHeadset,
        Failed,
    };

    enum class SessionResult {
        Ready,
        AdapterMismatch,
        FeatureLevelMismatch,
        RetryRuntime,
        Failed,
    };

    enum class EventResult {
        Healthy,
        QuitEnded,
        QuitEndFailed,
        SessionExiting,
        SessionLost,
        InstanceLost,
        Failed,
    };

    enum class FrameResult {
        Completed,
        SessionNotRunning,
        SessionLossPending,
        SessionLost,
        InstanceLost,
        GraphicsGateClosed,
        ProtocolRejected,
        Failed,
    };

    struct D3D11Requirements {
        LUID adapterLuid{};
        D3D_FEATURE_LEVEL minimumFeatureLevel = D3D_FEATURE_LEVEL_9_1;
    };

    struct EyeView {
        XrPosef pose{};
        XrFovf fov{};
    };

    struct PointerState {
        bool poseActive = false;
        bool poseValid = false;
        bool hovering = false;
        bool triggerPressed = false;
        bool triggerHeld = false;
        bool gripActive = false;
        bool thumbstickActive = false;
        float triggerValue = 0.0F;
        float gripValue = 0.0F;
        XrVector2f thumbstick{};
        float u = 0.0F;
        float v = 0.0F;
        // Second hit test against the settings-menu quad, which owns its own
        // swapchain and sits closer to the head than the desktop mirror quad.
        bool menuHovering = false;
        float menuU = 0.0F;
        float menuV = 0.0F;
        // Third hit test against the adjust bar floating above the game
        // panel; only populated while panel-adjust mode is active.
        bool barHovering = false;
        float barU = 0.0F;
        float barV = 0.0F;
        XrPosef aimPose{};
        bool gripPoseValid = false;
        bool usedGripPose = false;
        XrPosef gripPose{};
    };

    struct AaMenuCursor {
        bool hovering = false;
        float u = 0.0F;
        float v = 0.0F;
    };

    struct AaMenuStick {
        bool active = false;
        float x = 0.0F;
    };

    struct AaMenuInput {
        bool hovering = false;
        bool triggerHeld = false;
        bool triggerPressed = false;
        bool thumbstickActive = false;
        float u = 0.0F;
        float v = 0.0F;
        float thumbstickX = 0.0F;
        float thumbstickY = 0.0F;
        // Both hands keep an independent cursor. ImGui still has one mouse,
        // driven by the sticky owner in SelectAaMenuInput.
        std::array<AaMenuCursor, 2> cursors{};
        // Sticks stay live without hover so a selected slider can be nudged
        // by either hand after the pointer leaves the row.
        std::array<AaMenuStick, 2> sticks{};
    };

    // What the menu asks the session to do after a paint. Render scale cannot
    // be applied from inside the paint call: it resizes the eye buffers, which
    // only the session and Unity's render thread may do.
    struct AaMenuOutput {
        bool requestClose = false;
        bool requestRenderScale = false;
        bool requestQuit = false;
        bool requestQuitLocalize = false;
        float renderScale = 0.0F;
    };

    using AaMenuPaintFn = bool (*)(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* destination,
        std::uint32_t width,
        std::uint32_t height,
        const AaMenuInput& input,
        AaMenuOutput& output,
        VrLog& log);
    using AaMenuShutdownFn = void (*)() noexcept;
    using AaMenuFlushFn = void (*)(VrLog& log);
    using GameQuitFn = void (*)(bool fromLocalize) noexcept;

    // Panel-adjust overlay: one texture shared by two quads (adjust bar strip
    // on top, head-locked hint/toast strip below). Painted by VrAaMenu's
    // ImGui infrastructure like the settings menu.
    struct PanelOverlayInput {
        bool adjustMode = false;
        bool pinned = false;
        // 0 none, 1 height, 2 distance, 3 size (currently being adjusted).
        int activeItem = 0;
        // 0 none, 1 photo taken, 2 photo unavailable, 3 paused 2D view.
        int toast = 0;
        bool hovering = false;
        bool triggerHeld = false;
        bool triggerPressed = false;
        float u = 0.0F;
        float v = 0.0F;
    };

    struct PanelOverlayOutput {
        bool pinClicked = false;
        // 1 height, 2 distance, 3 size; 0 = nothing clicked this paint.
        int clickedItem = 0;
        // Pixel size of the fitted hint/toast box painted this frame (0 =
        // nothing drawn). The hint quad crops its subImage to this box so the
        // panel-space quad matches the text instead of the full strip.
        std::uint32_t hintWidthPx = 0;
        std::uint32_t hintHeightPx = 0;
    };

    using PanelOverlayPaintFn = bool (*)(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* destination,
        std::uint32_t width,
        std::uint32_t height,
        const PanelOverlayInput& input,
        PanelOverlayOutput& output,
        VrLog& log);

    struct StereoFrame {
        XrTime predictedDisplayTime = 0;
        XrDuration predictedDisplayPeriod = 0;
        bool shouldRender = false;
        bool frameDiscarded = false;
        bool layerSubmitted = false;
        bool stereoLayerSubmitted = false;
        bool stereoUiPanelVisible = false;
        bool aaMenuVisible = false;
        bool mirrorInputEnabled = false;
        bool mirrorPresentationChanged = false;
        std::uint64_t stereoFrameGeneration = 0;
        bool mirrorLayoutChanged = false;
        bool mirrorLayoutTransitionPending = false;
        bool referenceSpaceChanged = false;
        std::uint64_t sourceFrameGeneration = 0;
        std::uint64_t mirrorLayoutGeneration = 0;
        // Caller input to RunFrame (latched across its entry reset): the
        // game thread's Grip transparency request. When true and the stereo
        // projection layer is live, the desktop quad is submitted with
        // source-alpha blending instead of replacing the scene.
        bool gripPanelTransparent = false;
        std::uint32_t mirrorWidth = 0;
        std::uint32_t mirrorHeight = 0;
        XrViewStateFlags viewStateFlags = 0;
        uint32_t viewCount = 0;
        std::array<EyeView, 2> views{};
        std::array<PointerState, 2> pointers{};
    };

    OpenXrContext() = default;
    ~OpenXrContext();

    OpenXrContext(const OpenXrContext&) = delete;
    OpenXrContext& operator=(const OpenXrContext&) = delete;

    InitializeResult Initialize(const std::filesystem::path& applicationDirectory, VrLog& log);
    SystemResult AcquireHeadMountedSystem(VrLog& log);
    [[nodiscard]] bool QueryD3D11Requirements(D3D11Requirements& requirements, VrLog& log);
    SessionResult CreateD3D11Session(
        ID3D11Device* device,
        const LUID& adapterLuid,
        D3D_FEATURE_LEVEL featureLevel,
        const D3D11_TEXTURE2D_DESC& sourceFrameDescription,
        std::uint64_t sourceLayoutGeneration,
        const D3D11Requirements& requirements,
        bool stereoProjectionEnabled,
        bool stereoLandscapeOnly,
        float stereoRenderScale,
        VrLog& log);
    [[nodiscard]] EventResult DrainEvents(VrLog& log, bool quitting = false);
    [[nodiscard]] FrameResult RunFrame(
        StereoFrame& frame,
        ID3D11Texture2D* sourceFrame,
        std::uint64_t sourceFrameGeneration,
        std::uint64_t sourceLayoutGeneration,
        const d3d11::StereoRenderMailbox* stereoMailbox,
        VrLog& log);
    [[nodiscard]] FrameResult WaitAndPrepare(
        StereoFrame& frame,
        ID3D11Texture2D* sourceFrame,
        std::uint64_t sourceFrameGeneration,
        std::uint64_t sourceLayoutGeneration,
        frame::FrameIdentity& ticket,
        VrLog& log);
    [[nodiscard]] FrameResult BeginPrepared(
        const frame::FrameIdentity& ticket,
        StereoFrame& frame,
        VrLog& log);
    [[nodiscard]] FrameResult SubmitPrepared(
        const frame::FrameIdentity& ticket,
        StereoFrame& frame,
        ID3D11Texture2D* sourceFrame,
        std::uint64_t sourceFrameGeneration,
        const d3d11::StereoRenderMailbox* stereoMailbox,
        VrLog& log);
    [[nodiscard]] FrameResult BeginAndSubmit(
        const frame::FrameIdentity& ticket,
        StereoFrame& frame,
        ID3D11Texture2D* sourceFrame,
        std::uint64_t sourceFrameGeneration,
        const d3d11::StereoRenderMailbox* stereoMailbox,
        VrLog& log);
    [[nodiscard]] FrameResult EndPrepared(
        const frame::FrameIdentity& ticket,
        StereoFrame& frame,
        VrLog& log);
    [[nodiscard]] frame::FrameCoordinator& Coordinator() noexcept;
    [[nodiscard]] const frame::FrameCoordinator& Coordinator() const noexcept;
    [[nodiscard]] bool RequestExit(VrLog& log);

    // Rebuild only graphics-bound session resources after Unity replaces its
    // D3D11 device. The instance, system and action definitions remain valid.
    [[nodiscard]] bool ResetGraphicsSession(VrLog& log) noexcept;
    void ResetInstance() noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool HasInstance() const noexcept;
    [[nodiscard]] bool HasSystem() const noexcept;
    [[nodiscard]] bool HasSession() const noexcept;
    [[nodiscard]] XrSessionState SessionState() const noexcept;
    [[nodiscard]] bool IsSessionRunning() const noexcept;
    [[nodiscard]] std::uint64_t SessionRunGeneration() const noexcept;
    [[nodiscard]] XrEnvironmentBlendMode EnvironmentBlendMode() const noexcept;
    [[nodiscard]] XrReferenceSpaceType ActiveReferenceSpaceType() const noexcept;
    [[nodiscard]] bool HasStageSpace() const noexcept;
    [[nodiscard]] std::uint32_t StereoEyeWidth() const noexcept;
    [[nodiscard]] std::uint32_t StereoEyeHeight() const noexcept;
    // True when there is no mirror swapchain yet, or the Unity source can
    // CopyResource onto the current swapchain. False means Wait must drain
    // the graphics callback before EnsureMirrorLayout can rebuild.
    [[nodiscard]] bool MirrorSwapchainMatches(ID3D11Texture2D* sourceFrame) const noexcept;
    [[nodiscard]] XrResult LastResult() const noexcept;
    // OpenXR worker writes; Unity thread consumes. Rising-edge B only.
    [[nodiscard]] bool ConsumeLivePauseToggle() noexcept;
    // Cumulative rising-edge counters for the VR free-camera buttons. The
    // consumer diffs against its last-seen value; the counters are never
    // reset so a session restart cannot produce phantom presses.
    [[nodiscard]] std::uint32_t CameraModePressCount() const noexcept;
    [[nodiscard]] std::uint32_t CameraCharaPressCount() const noexcept;
    [[nodiscard]] std::uint32_t CameraResetPressCount() const noexcept;
    // Right-A presses routed to the game's photo shutter (photo scenes only).
    [[nodiscard]] std::uint32_t PhotoPressCount() const noexcept;
    void SetAaMenuHooks(
        AaMenuPaintFn paint,
        AaMenuShutdownFn shutdown,
        AaMenuFlushFn flush) noexcept;
    void SetGameQuitHook(GameQuitFn quit) noexcept;
    void SetPanelOverlayHooks(PanelOverlayPaintFn paint) noexcept;

private:
    bool HasD3D11Extension(VrLog& log, XrResult& result);
    bool ValidateStereoViewConfiguration(VrLog& log);
    bool SelectEnvironmentBlendMode(VrLog& log);
    bool CreateReferenceSpaces(VrLog& log);
    bool CreateInputActions(VrLog& log);
    bool AttachInputActions(VrLog& log);
    void SyncPointerInput(
        std::array<PointerState, 2>& pointers,
        XrTime displayTime,
        const pose::Pose& openXrHeadCenter,
        bool openXrHeadValid,
        VrLog& log);
    void PollLivePauseButton(XrTime displayTime, VrLog& log);
    void PollCameraButtons(XrTime displayTime, VrLog& log);
    void PollPanelAdjustButton(XrTime displayTime, VrLog& log);
    // Per-frame panel placement: photo-scene sync, toast lifetime, head pose
    // in the projection base space and the derived quad poses for hit tests
    // and layer submission. Runs before SyncPointerInput.
    void UpdatePanelPlacementFrame(XrTime displayTime, VrLog& log);
    void UpdatePanelAdjustInteractions(
        const std::array<PointerState, 2>& pointers,
        XrTime displayTime,
        VrLog& log);
    void SetPanelAdjustMode(bool active, const char* reason, VrLog& log);
    void TogglePanelPin(VrLog& log);
    void ResetPanelPlacement(VrLog& log);
    void SavePanelPlacementIfDirty(VrLog& log);
    void LoadPanelPlacementFromConfig() noexcept;
    bool EnsurePanelOverlaySwapchain(VrLog& log);
    bool RenderPanelOverlayFrame(
        const std::array<PointerState, 2>& pointers,
        VrLog& log);
    void ResetPanelOverlaySwapchain() noexcept;
    void UpdateStereoUiPanelState(
        const std::array<PointerState, 2>& pointers,
        bool stereoSceneEligible,
        bool projectionReady,
        bool mirrorReady,
        XrTime displayTime,
        StereoFrame& frame,
        VrLog& log);
    void RefreshStereoUiInputState(
        bool projectionReady, bool mirrorReady, StereoFrame& frame);
    bool CreateMirrorSwapchain(VrLog& log);
    bool EnsureProjectionSwapchain(
        const d3d11::StereoRenderMailbox::Snapshot& stereoFrame,
        VrLog& log);
    bool CreateProjectionSwapchain(
        const D3D11_TEXTURE2D_DESC& sourceDescription,
        VrLog& log);
    bool RenderProjectionFrame(
        const d3d11::StereoRenderMailbox::Snapshot& stereoFrame,
        VrLog& log);
    bool EnsureMirrorLayout(
        ID3D11Texture2D* sourceFrame,
        std::uint64_t sourceLayoutGeneration,
        bool allowRebuild,
        bool& layoutChanged,
        VrLog& log);
    bool DestroyMirrorSwapchainForRebuild(VrLog& log);
    // Recompute the eye target spec from the cached runtime recommendation.
    // Returns true when the spec actually changed, which is the runtime's signal
    // to publish a new target generation.
    bool ApplyStereoRenderScale(float renderScale, VrLog& log);
    bool EnsureMenuSwapchain(VrLog& log);
    bool RenderMenuFrame(
        const std::array<PointerState, 2>& pointers,
        VrLog& log);
    bool RenderMirrorFrame(
        ID3D11Texture2D* sourceFrame,
        std::uint64_t sourceFrameGeneration,
        const std::array<PointerState, 2>& pointers,
        VrLog& log);
    void OverlayPointerCursors(
        ID3D11Texture2D* destination,
        const std::array<PointerState, 2>& pointers) noexcept;
    void ResetInputSession() noexcept;
    void ResetInputActions() noexcept;
    void ResetMirrorSwapchain() noexcept;
    void ReleasePortraitLatch() noexcept;
    [[nodiscard]] bool EnsurePortraitLatch(
        const D3D11_TEXTURE2D_DESC& sourceDescription,
        VrLog& log) noexcept;
    void ResetMenuSwapchain() noexcept;
    void ResetProjectionSwapchain() noexcept;
    void ResetSessionChildren() noexcept;
    void ClearSessionState() noexcept;
    void ResetSession() noexcept;
    struct SubmitLayers {
        XrTime displayTime = 0;
        XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::array<XrCompositionLayerProjectionView, 2> projectionViews{
            XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
            XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        };
        XrCompositionLayerQuad mirror{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad bar{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad hint{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad menu{XR_TYPE_COMPOSITION_LAYER_QUAD};
        std::array<const XrCompositionLayerBaseHeader*, 5> layerPtrs{};
        std::uint32_t layerCount = 0;
        bool projectionReady = false;
        bool mirrorReady = false;
        bool valid = false;
    };

    // Immutable GPU submit parameters frozen at Wait return. Submit/End of
    // ticket N must read this copy so Wait(N+1) can mutate live UI/pose/layout.
    struct GpuSubmitSnapshot {
        bool aaMenuVisible = false;
        bool stereoUiPanelVisible = false;
        bool panelAdjustMode = false;
        int panelToastKind = 0;
        std::array<bool, 2> gripHeld{};
        bool panelPoseUsesBase = false;
        pose::Pose panelPoseView{};
        pose::Pose panelPoseBase{};
        pose::Pose barPoseView{};
        pose::Pose barPoseBase{};
        pose::Pose hintPoseView{};
        float panelQuadWidth = 0.0F;
        float barQuadWidth = 0.0F;
        float barQuadHeight = 0.0F;
        std::uint32_t panelHintWidthPx = 0;
        std::uint32_t panelHintHeightPx = 0;
        std::uint32_t mirrorWidth = 0;
        std::uint32_t mirrorHeight = 0;
        std::uint64_t mirrorLayoutGeneration = 0;
    };

    struct FrameWork {
        frame::FrameIdentity identity{};
        StereoFrame frame{};
        GpuSubmitSnapshot gpuSnapshot{};
        bool sessionLossPending = false;
        bool began = false;
        XrResult locateResult = XR_SUCCESS;
        XrResult mirrorResult = XR_SUCCESS;
        std::uint32_t locateViewCount = 0;
        XrTime appliedReferenceSpaceChangeTime = 0;
        std::size_t appliedReferenceSpaceChangeCount = 0;
        XrTime effectiveProjectionTrackingTimeFloor = 0;
        d3d11::StereoRenderMailbox::Snapshot stereo;
        bool stereoBound = false;
        bool projectionReady = false;
        bool mirrorReady = false;
        bool menuReady = false;
        bool overlayReady = false;
        pose::StereoPoseSample projectionTracking{};
        ID3D11Texture2D* sourceFrame = nullptr;
        std::uint64_t sourceFrameGeneration = 0;
        std::uint64_t sourceLayoutGeneration = 0;
        int endEventId = 0;
        SubmitLayers layers{};
    };

    [[nodiscard]] FrameWork* WorkFor(std::uint64_t frameId) noexcept;
    void FreezeGpuSubmitSnapshot(FrameWork& work) noexcept;
    void ResetFrameProtocol() noexcept;
    void LogFrame(
        VrLog& log,
        std::string_view token,
        const frame::FrameIdentity& identity,
        XrTime displayTime);
    [[nodiscard]] static EventResult ClassifyEventResult(XrResult result) noexcept;
    [[nodiscard]] static FrameResult ClassifyFrameResult(XrResult result) noexcept;
    static bool EqualLuid(const LUID& left, const LUID& right) noexcept;
    static bool IsSupportedMirrorFormat(DXGI_FORMAT format) noexcept;
    static bool AreCopyCompatibleFormats(DXGI_FORMAT left, DXGI_FORMAT right) noexcept;

    frame::FrameCoordinator coordinator_;
    std::array<FrameWork, 2> works_{};
    std::uint64_t referenceSpaceEpoch_ = 0;
    OpenXrDispatch dispatch_;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    XrSpace stageSpace_ = XR_NULL_HANDLE;
    XrActionSet inputActionSet_ = XR_NULL_HANDLE;
    XrAction aimPoseAction_ = XR_NULL_HANDLE;
    XrAction gripPoseAction_ = XR_NULL_HANDLE;
    XrAction triggerAction_ = XR_NULL_HANDLE;
    XrAction gripAction_ = XR_NULL_HANDLE;
    XrAction thumbstickAction_ = XR_NULL_HANDLE;
    XrAction livePauseAction_ = XR_NULL_HANDLE;
    XrAction cameraModeAction_ = XR_NULL_HANDLE;
    XrAction cameraCharaAction_ = XR_NULL_HANDLE;
    XrAction cameraResetAction_ = XR_NULL_HANDLE;
    std::array<XrPath, 2> handPaths_{};
    std::array<XrSpace, 2> aimSpaces_{};
    std::array<XrSpace, 2> gripSpaces_{};
    std::array<bool, 2> triggerHeld_{};
    std::array<gakumas::vr::input::PointerUvSmoother, 2> mirrorPointerSmoothers_{};
    std::array<gakumas::vr::input::PointerUvSmoother, 2> menuPointerSmoothers_{};
    std::array<XrTime, 2> pointerFilterTime_{};
    std::uint64_t pointerFilterLayoutGeneration_ = 0;
    bool pointerSmoothLogged_ = false;
    std::array<bool, 2> gripStateInitialized_{};
    std::array<bool, 2> gripHeld_{};
    std::array<bool, 2> gripGestureValid_{};
    std::array<bool, 2> gripLongPressFired_{};
    std::array<XrTime, 2> gripPressTime_{};
    XrTime gripToggleCooldownUntil_ = 0;
    bool livePauseHeld_ = false;
    XrTime livePauseInactiveSince_ = 0;
    std::atomic<std::uint32_t> livePauseToggleCount_{0};
    bool cameraModeHeld_ = false;
    bool cameraCharaHeld_ = false;
    bool cameraResetHeld_ = false;
    bool cameraResetFired_ = false;
    XrTime cameraResetPressTime_ = 0;
    std::atomic<std::uint32_t> cameraModePressCount_{0};
    std::atomic<std::uint32_t> cameraCharaPressCount_{0};
    std::atomic<std::uint32_t> cameraResetPressCount_{0};
    bool stereoUiPanelVisible_ = false;
    bool aaMenuVisible_ = false;
    // Last submitted scene role; CPU admission cannot depend on eyes that
    // have not been rendered yet. Written by Submit, read by the next Wait.
    std::atomic<bool> inputProjectionReady_{false};
    // --- Right-A: photo shutter routing + panel adjust mode. ---
    XrAction panelAdjustAction_ = XR_NULL_HANDLE;
    bool panelAdjustButtonHeld_ = false;
    std::atomic<std::uint32_t> photoPressCount_{0};
    bool photoSceneActive_ = false;
    panel::Placement panelPlacement_{};
    bool panelPlacementLoaded_ = false;
    bool panelPlacementDirty_ = false;
    // Pinned direction re-expressed in the projection base space; re-anchored
    // from the view-space offset whenever pin turns on or a session begins.
    // The orientation is captured from the displayed pose at pin time so the
    // panel freezes exactly where it appeared (no roll-free tilt jump).
    pose::Vector3 panelOffsetBase_{};
    pose::Quaternion panelPinnedOrientationBase_{};
    bool panelPinnedAnchorValid_ = false;
    bool panelAdjustMode_ = false;
    int panelAdjustItem_ = 0;
    std::size_t panelAdjustHand_ = 1;
    std::array<bool, 2> panelGrabActive_{};
    std::array<pose::Vector3, 2> panelHandLastView_{};
    std::array<bool, 2> panelHandLastValid_{};
    // Aim-ray directions for the angular grab (panel follows the ray 1:1).
    std::array<pose::Vector3, 2> panelGrabLastAimDir_{};
    std::array<bool, 2> panelGrabAimValid_{};
    bool panelScaleActive_ = false;
    float panelScaleBaselineSeparation_ = 0.0F;
    float panelScaleBaselineWidth_ = 0.0F;
    // Per-frame derived state (UpdatePanelPlacementFrame).
    pose::Pose headPoseBase_{};
    bool headPoseBaseValid_ = false;
    pose::Pose panelPoseView_{
        {0.0F, 0.0F, -panel::kDefaultPanelDistanceMetres}, {}};
    pose::Pose panelPoseBase_{};
    bool panelPoseUsesBase_ = false;
    float panelQuadWidth_ = panel::kDefaultPanelWidthMetres;
    float panelQuadHeight_ = 0.0F;
    pose::Pose barPoseView_{};
    pose::Pose barPoseBase_{};
    float barQuadWidth_ = 0.0F;
    float barQuadHeight_ = 0.0F;
    pose::Pose hintPoseView_{{0.0F, 0.0F, -1.1F}, {}};
    std::size_t panelBarPointerHand_ = 1;
    // Toast on the head-locked hint strip: 0 none, 1 photo ok, 2 unavailable,
    // 3 paused 2D view (Right-A shortcut only).
    int panelToastKind_ = 0;
    XrTime panelToastUntil_ = 0;
    // A routed photo press that produced neither a capture event nor a block
    // by this deadline toasts "unavailable" (e.g. the silent 50-shot limit).
    XrTime photoResultPendingUntil_ = 0;
    // Fitted hint/toast box painted this frame (pixels; 0 = no hint quad).
    std::uint32_t panelHintWidthPx_ = 0;
    std::uint32_t panelHintHeightPx_ = 0;
    std::uint32_t lastPhotoResultGeneration_ = 0;
    bool photoResultGenerationInitialized_ = false;
    PanelOverlayPaintFn panelOverlayPainter_ = nullptr;
    XrSwapchain panelOverlaySwapchain_ = XR_NULL_HANDLE;
    std::uint32_t panelOverlayWidth_ = 0;
    std::uint32_t panelOverlayHeight_ = 0;
    std::int64_t panelOverlayFormat_ = 0;
    std::vector<XrSwapchainImageD3D11KHR> panelOverlayImages_;
    bool panelOverlaySwapchainFailed_ = false;
    std::uint32_t panelOverlayPaintFailures_ = 0;
    // --- end right-A state ---
    // Same sticky-owner default as the desktop panel: right hand, until a
    // left-hand press on the menu takes over.
    std::size_t menuPointerHand_ = 1;
    bool stereoSceneEligible_ = false;
    AaMenuPaintFn aaMenuPainter_ = nullptr;
    AaMenuShutdownFn aaMenuShutdown_ = nullptr;
    AaMenuFlushFn aaMenuFlush_ = nullptr;
    GameQuitFn gameQuit_ = nullptr;
    std::atomic<bool> mirrorInputStateInitialized_{false};
    std::atomic<bool> lastMirrorInputEnabled_{true};
    bool mirrorCopySuspended_ = false;
    bool inputActionsCreated_ = false;
    bool inputSessionReady_ = false;
    bool inputErrorLogged_ = false;
    XrEnvironmentBlendMode environmentBlendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;
    bool exitRequested_ = false;
    std::uint64_t sessionRunGeneration_ = 0;
    std::vector<XrTime> pendingReferenceSpaceChangeTimes_;
    XrTime projectionTrackingTimeFloor_ = 0;
    XrSwapchain mirrorSwapchain_ = XR_NULL_HANDLE;
    std::uint32_t mirrorWidth_ = 0;
    std::uint32_t mirrorHeight_ = 0;
    std::uint32_t mirrorMaximumWidth_ = 0;
    std::uint32_t mirrorMaximumHeight_ = 0;
    std::uint64_t mirrorLayoutGeneration_ = 0;
    DXGI_FORMAT mirrorSourceFormat_ = DXGI_FORMAT_UNKNOWN;
    std::int64_t mirrorSwapchainFormat_ = 0;
    std::vector<XrSwapchainImageD3D11KHR> mirrorImages_;
    ID3D11Texture2D* portraitLatchTexture_ = nullptr;
    std::uint32_t portraitLatchWidth_ = 0;
    std::uint32_t portraitLatchHeight_ = 0;
    DXGI_FORMAT portraitLatchFormat_ = DXGI_FORMAT_UNKNOWN;
    bool portraitLatchValid_ = false;
    bool portraitLatchCaptureLogged_ = false;
    bool portraitLatchFrozenLogged_ = false;
    bool portraitLatchFollowLogged_ = false;
    // The settings menu owns a dedicated quad swapchain so it never shares,
    // clears or overwrites the desktop mirror image.
    XrSwapchain menuSwapchain_ = XR_NULL_HANDLE;
    std::uint32_t menuWidth_ = 0;
    std::uint32_t menuHeight_ = 0;
    std::int64_t menuSwapchainFormat_ = 0;
    std::vector<XrSwapchainImageD3D11KHR> menuImages_;
    bool menuSwapchainFailed_ = false;
    std::uint32_t menuPaintFailures_ = 0;
    bool stereoProjectionEnabled_ = false;
    bool stereoLandscapeOnly_ = false;
    std::atomic<bool> projectionDisabledForSession_{false};
    float stereoRenderScale_ = 1.0F;
    std::atomic<bool> pendingStereoRenderScaleValid_{false};
    std::atomic<float> pendingStereoRenderScale_{1.0F};
    // Runtime recommendation kept so the menu can rescale the eye targets
    // without tearing the session down and re-enumerating view configurations.
    std::uint32_t recommendedEyeWidth_ = 0;
    std::uint32_t recommendedEyeHeight_ = 0;
    std::string runtimeName_;
    std::string runtimeVersionText_;
    std::string systemName_;
    bool consoleSummaryLogged_ = false;
    bool bdControllerInteractionAvailable_ = false;
    std::uint32_t stereoEyeWidth_ = 0;
    std::uint32_t stereoEyeHeight_ = 0;
    XrSwapchain projectionSwapchain_ = XR_NULL_HANDLE;
    std::int64_t projectionSwapchainFormat_ = 0;
    D3D11_TEXTURE2D_DESC projectionSourceDescription_{};
    std::vector<XrSwapchainImageD3D11KHR> projectionImages_;
    std::uint64_t lastMirrorFingerprintGeneration_ = 0;
    std::uint64_t mirrorFingerprint_ = 0;
    bool mirrorFingerprintValid_ = false;
    std::uint64_t lastProjectionFingerprintGeneration_ = 0;
    std::array<std::uint64_t, 2> projectionFingerprints_{};
    std::array<bool, 2> projectionFingerprintValid_{};
    std::uint64_t lastSubmittedStereoGeneration_ = 0;
    pose::StereoPoseSample lastSubmittedStereoTrackingSample_{};
    std::int64_t lastSubmittedStereoHostPublishTimeNanoseconds_ = 0;
    std::uint64_t stereoSubmissionCount_ = 0;
    std::uint64_t freshStereoSubmissionCount_ = 0;
    std::uint64_t repeatedStereoSubmissionCount_ = 0;
    bool repeatedStereoReuseLogged_ = false;
    d3d11::VerticalFlipPass projectionVerticalFlip_;
    ID3D11DeviceContext* sessionContext_ = nullptr;
    ID3D11Device* sessionDevice_ = nullptr;
    perf::EndGpuMarker endGpuMarker_;
    bool hitchReadyLogged_ = false;
    std::atomic<XrResult> lastResult_{XR_SUCCESS};
};

// Both hands always publish independent menu cursors. ImGui still has one
// mouse, so ownership sticks to the last clicking hand the same way the
// desktop panel does: a held trigger cannot be stolen, a fresh press can
// take over when nobody is dragging, and hover alone never jumps the mouse.
inline OpenXrContext::AaMenuInput SelectAaMenuInput(
    const std::array<OpenXrContext::PointerState, 2>& pointers,
    std::size_t& ownerHand) noexcept {
    OpenXrContext::AaMenuInput input{};
    for (std::size_t hand = 0; hand < pointers.size(); ++hand) {
        input.cursors[hand].hovering = pointers[hand].menuHovering;
        input.cursors[hand].u = pointers[hand].menuU;
        input.cursors[hand].v = pointers[hand].menuV;
        input.sticks[hand].active = pointers[hand].thumbstickActive;
        input.sticks[hand].x = pointers[hand].thumbstick.x;
    }
    if (ownerHand >= pointers.size()) {
        ownerHand = 1;
    }

    const auto assignOwner = [&](std::size_t hand) {
        const auto& pointer = pointers[hand];
        ownerHand = hand;
        input.hovering = pointer.menuHovering;
        input.triggerHeld = pointer.menuHovering && pointer.triggerHeld;
        input.triggerPressed = pointer.menuHovering && pointer.triggerPressed;
        input.thumbstickActive =
            pointer.menuHovering && pointer.thumbstickActive;
        input.u = pointer.menuU;
        input.v = pointer.menuV;
        input.thumbstickX = pointer.thumbstick.x;
        input.thumbstickY = pointer.thumbstick.y;
    };
    const std::array<std::size_t, 2> order{
        ownerHand,
        ownerHand == 0 ? 1U : 0U,
    };

    if (pointers[ownerHand].menuHovering && pointers[ownerHand].triggerHeld) {
        assignOwner(ownerHand);
        return input;
    }
    for (const std::size_t hand : order) {
        if (pointers[hand].menuHovering && pointers[hand].triggerPressed) {
            assignOwner(hand);
            return input;
        }
    }
    for (const std::size_t hand : order) {
        if (pointers[hand].menuHovering) {
            assignOwner(hand);
            return input;
        }
    }
    return input;
}

// Either stick can nudge a selected slider. Hover is not required; both
// hands add independently so pushing the same way stacks.
inline constexpr float kAaMenuSliderDeadzone = 0.30F;
// Full deflection crosses a 0-1 slider in about 4.5 s (was 1.2 s at 0.85).
inline constexpr float kAaMenuSliderRate = 0.22F;

[[nodiscard]] inline bool AaMenuSliderStickActive(
    const std::array<OpenXrContext::AaMenuStick, 2>& sticks) noexcept {
    for (const auto& stick : sticks) {
        if (stick.active && std::abs(stick.x) > kAaMenuSliderDeadzone) {
            return true;
        }
    }
    return false;
}

inline void NudgeAaMenuSlider(
    float& value,
    float minimum,
    float maximum,
    float deltaSeconds,
    const std::array<OpenXrContext::AaMenuStick, 2>& sticks) noexcept {
    const float span = maximum - minimum;
    if (span <= 0.0F || deltaSeconds <= 0.0F) {
        return;
    }
    for (const auto& stick : sticks) {
        if (!stick.active || std::abs(stick.x) <= kAaMenuSliderDeadzone) {
            continue;
        }
        float next = value + stick.x * span * deltaSeconds * kAaMenuSliderRate;
        if (next < minimum) {
            next = minimum;
        }
        if (next > maximum) {
            next = maximum;
        }
        value = next;
    }
}

} // namespace gakumas::vr::openxr
