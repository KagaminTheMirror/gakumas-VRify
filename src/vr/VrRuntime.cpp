#include "VrRuntime.hpp"

#include "VrVersion.hpp"
#include "GripTransparencyTrace.hpp"
#include "VrAaMenu.hpp"
#include "VrFreeCamera.hpp"
#include "input/ScrollInputDiagnostics.hpp"
#include "input/UnityPointerInput.hpp"
#include "config/VrifyConfig.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace gakumas::vr {
namespace {

constexpr std::uint64_t ConfirmedMirrorLayoutState(
    bool frameAvailable,
    bool layoutTransitionPending,
    std::uint64_t layoutGeneration,
    std::uint32_t width,
    std::uint32_t height) noexcept {
    constexpr std::uint64_t kLargestPackableGeneration =
        std::numeric_limits<std::uint64_t>::max() >> 1U;
    if (!frameAvailable || layoutTransitionPending || layoutGeneration == 0 ||
        layoutGeneration > kLargestPackableGeneration || width == 0 || height == 0) {
        return 0;
    }
    return (layoutGeneration << 1U) | (width > height ? 1U : 0U);
}

constexpr std::uint64_t ConfirmedMirrorLayoutGeneration(
    std::uint64_t packedState) noexcept {
    return packedState >> 1U;
}

constexpr bool ConfirmedMirrorLayoutIsLandscape(
    std::uint64_t packedState) noexcept {
    return (packedState & 1U) != 0;
}

static_assert(ConfirmedMirrorLayoutState(false, false, 4, 1920, 1080) == 0);
static_assert(ConfirmedMirrorLayoutState(true, true, 4, 1920, 1080) == 0);
static_assert(ConfirmedMirrorLayoutState(true, false, 0, 1920, 1080) == 0);
static_assert(ConfirmedMirrorLayoutState(true, false, 4, 0, 1080) == 0);
static_assert(ConfirmedMirrorLayoutState(true, false, 4, 1080, 1920) == 8);
static_assert(ConfirmedMirrorLayoutState(true, false, 4, 1920, 1080) == 9);
static_assert(ConfirmedMirrorLayoutGeneration(9) == 4);
static_assert(ConfirmedMirrorLayoutIsLandscape(9));
static_assert(!ConfirmedMirrorLayoutIsLandscape(8));

std::string LuidText(const LUID& luid) {
    std::ostringstream stream;
    stream << std::hex << static_cast<unsigned long>(luid.HighPart) << ':'
           << static_cast<unsigned long>(luid.LowPart);
    return stream.str();
}

std::string FeatureLevelText(D3D_FEATURE_LEVEL level) {
    std::ostringstream stream;
    stream << "0x" << std::hex << static_cast<unsigned>(level);
    return stream.str();
}

bool HasViewFlags(XrViewStateFlags value, XrViewStateFlags required) noexcept {
    return (value & required) == required;
}

std::string ViewFlagsText(XrViewStateFlags flags) {
    std::ostringstream stream;
    stream << "[VR][pose] VIEW_FLAGS flags=0x" << std::hex << flags << std::dec
           << " orientationValid="
           << HasViewFlags(flags, XR_VIEW_STATE_ORIENTATION_VALID_BIT)
           << " positionValid="
           << HasViewFlags(flags, XR_VIEW_STATE_POSITION_VALID_BIT)
           << " orientationTracked="
           << HasViewFlags(flags, XR_VIEW_STATE_ORIENTATION_TRACKED_BIT)
           << " positionTracked="
           << HasViewFlags(flags, XR_VIEW_STATE_POSITION_TRACKED_BIT);
    return stream.str();
}

std::string PoseSampleText(
    std::uint64_t frameIndex,
    const openxr::OpenXrContext::StereoFrame& frame) {
    const bool orientationValid =
        HasViewFlags(frame.viewStateFlags, XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    const bool positionValid =
        HasViewFlags(frame.viewStateFlags, XR_VIEW_STATE_POSITION_VALID_BIT);

    std::ostringstream stream;
    stream << "[VR][pose] POSE_SAMPLE frame=" << frameIndex
           << " predictedTime=" << frame.predictedDisplayTime
           << " periodMs=" << std::fixed << std::setprecision(3)
           << (static_cast<double>(frame.predictedDisplayPeriod) / 1'000'000.0)
           << " shouldRender=" << frame.shouldRender
           << " sourceFrame=" << frame.sourceFrameGeneration
           << " views=" << frame.viewCount;

    if (positionValid && frame.viewCount == 2) {
        const auto& left = frame.views[0].pose.position;
        const auto& right = frame.views[1].pose.position;
        const float deltaX = right.x - left.x;
        const float deltaY = right.y - left.y;
        const float deltaZ = right.z - left.z;
        const float ipd = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
        stream << std::setprecision(4)
               << " leftPos=(" << left.x << ',' << left.y << ',' << left.z << ')'
               << " rightPos=(" << right.x << ',' << right.y << ',' << right.z << ')'
               << " ipd=" << ipd;
    }
    if (orientationValid && frame.viewCount == 2) {
        const auto& left = frame.views[0].pose.orientation;
        const auto& right = frame.views[1].pose.orientation;
        stream << std::setprecision(5)
               << " leftOri=(" << left.x << ',' << left.y << ',' << left.z << ',' << left.w << ')'
               << " rightOri=(" << right.x << ',' << right.y << ',' << right.z << ',' << right.w << ')';
    }
    if (frame.viewCount == 2) {
        const auto& left = frame.views[0].fov;
        const auto& right = frame.views[1].fov;
        stream << std::setprecision(4)
               << " leftFov=(" << left.angleLeft << ',' << left.angleRight << ','
               << left.angleUp << ',' << left.angleDown << ')'
               << " rightFov=(" << right.angleLeft << ',' << right.angleRight << ','
               << right.angleUp << ',' << right.angleDown << ')';
    }
    return stream.str();
}

bool UsePostMessageTestInput() noexcept {
    return GetEnvironmentVariableW(
               L"GAKUMAS_VR_EXPECT_POINTER_INPUT",
               nullptr,
               0) != 0;
}

struct MouseDispatchResult {
    bool delivered = false;
    bool foreground = false;
    UINT inputsSent = 0;
    DWORD error = ERROR_SUCCESS;
    POINT screenPoint{};
};

MouseDispatchResult QueueNativeMouseInput(
    POINT point, int width, int height, bool down, bool up, bool held) noexcept {
    MouseDispatchResult result;
    result.delivered = input::QueueUnityPointer(
        static_cast<float>(point.x) / std::max(1, width - 1),
        static_cast<float>(point.y) / std::max(1, height - 1),
        !up && (down || held), down, up);
    result.error = result.delivered ? ERROR_SUCCESS : ERROR_NOT_READY;
    return result;
}
MouseDispatchResult PostTestMouseInput(
    HWND outputWindow,
    POINT clientPoint,
    bool buttonDown,
    bool buttonUp,
    bool buttonHeld,
    int wheelDelta) noexcept {
    MouseDispatchResult result;
    result.screenPoint = clientPoint;
    if (!ClientToScreen(outputWindow, &result.screenPoint)) {
        result.error = GetLastError();
        return result;
    }

    const LPARAM clientPosition = MAKELPARAM(clientPoint.x, clientPoint.y);
    bool delivered = PostMessageW(
                         outputWindow,
                         WM_MOUSEMOVE,
                         buttonHeld ? MK_LBUTTON : 0,
                         clientPosition) != FALSE;
    if (buttonDown) {
        delivered =
            PostMessageW(outputWindow, WM_LBUTTONDOWN, MK_LBUTTON, clientPosition) != FALSE &&
            delivered;
    }
    if (buttonUp) {
        delivered =
            PostMessageW(outputWindow, WM_LBUTTONUP, 0, clientPosition) != FALSE && delivered;
    }
    if (wheelDelta != 0) {
        const WPARAM wheel = MAKEWPARAM(
            0,
            static_cast<WORD>(static_cast<SHORT>(wheelDelta)));
        delivered =
            PostMessageW(
                outputWindow,
                WM_MOUSEWHEEL,
                wheel,
                MAKELPARAM(result.screenPoint.x, result.screenPoint.y)) != FALSE &&
            delivered;
    }
    result.delivered = delivered;
    result.inputsSent = delivered ? 1U : 0U;
    result.error = delivered ? ERROR_SUCCESS : GetLastError();
    return result;
}

} // namespace

VrRuntime& VrRuntime::Instance() {
    // Intentionally process-lifetime. A function-local object would have its
    // destructor run from the DLL CRT during DLL_PROCESS_DETACH, where Stop()
    // would join under loader lock and could deadlock. Normal teardown must call
    // StopVrRuntime() before the hook owner is shut down; process exit lets the OS
    // reclaim this allocation without executing user code under loader lock.
    static VrRuntime* runtime = new VrRuntime();
    return *runtime;
}

VrRuntime::~VrRuntime() {
    Stop();
}

bool VrRuntime::Start(VrRuntimeConfig config, HookRegistrar registrar) {
    std::lock_guard lock(lifecycleMutex_);

    if (worker_.joinable()) {
        return false;
    }
    confirmedMirrorLayout_.store(0, std::memory_order_release);
    if (!config.enabled) {
        config_ = std::move(config);
        state_.store(VrRuntimeState::Disabled, std::memory_order_release);
        return true;
    }
    if (!registrar.IsValid()) {
        state_.store(VrRuntimeState::Faulted, std::memory_order_release);
        return false;
    }

    if (config.applicationDirectory.empty()) {
        config.applicationDirectory = ResolveApplicationDirectory();
    }
    if (config.applicationDirectory.empty()) {
        state_.store(VrRuntimeState::Faulted, std::memory_order_release);
        return false;
    }
    config.runtimeRetryInterval = std::max(config.runtimeRetryInterval, std::chrono::milliseconds(100));
    config.graphicsPollInterval = std::max(config.graphicsPollInterval, std::chrono::milliseconds(50));

    config_ = std::move(config);
    registrar_ = registrar;
    poseMailbox_.Invalidate();
    stereoRenderMailbox_.Invalidate();
    stereoEyeWidth_.store(0, std::memory_order_release);
    stereoEyeHeight_.store(0, std::memory_order_release);
    stereoTargetGeneration_.store(0, std::memory_order_release);
    stopRequested_.store(false, std::memory_order_release);
    lastOpenXrResult_.store(XR_SUCCESS, std::memory_order_release);
    d3d11HooksReady_ = false;
    activePointerHand_ = 1;
    draggingPointerHand_ = 2;
    dragWindow_ = nullptr;
    dragMoved_ = false;
    thumbstickScrollHorizontal_.Reset();
    thumbstickScrollVertical_.Reset();
    scrollingPointerHand_ = 2;
    nextScrollLogTime_ = 0;

    if (!log_.Open(config_.applicationDirectory, config_.diagnosticsEnabled)) {
        state_.store(VrRuntimeState::Faulted, std::memory_order_release);
        return false;
    }

    log_.Write(
        "[VR][runtime] RUN_BEGIN pid=" + std::to_string(GetCurrentProcessId()) +
        " version=" GAKUMAS_VR_VERSION);
    log_.Write(
        "[VR][runtime] worker starting; nativeMirror=1 headPoseBridge=" +
        std::to_string(config_.cameraPoseBridgeEnabled ? 1 : 0) +
        " stereoProjection=" +
        std::to_string(config_.stereoProjectionEnabled ? 1 : 0) +
        " diagnostics=" +
        std::to_string(config_.diagnosticsEnabled ? 1 : 0));
    try {
        worker_ = std::thread(&VrRuntime::WorkerMain, this);
    } catch (...) {
        Fault("unable to create worker thread");
        log_.Close();
        return false;
    }
    return true;
}

void VrRuntime::Stop() noexcept {
    std::unique_lock lifecycleLock(lifecycleMutex_);
    stopRequested_.store(true, std::memory_order_release);
    confirmedMirrorLayout_.store(0, std::memory_order_release);
    poseMailbox_.Invalidate();
    stereoRenderMailbox_.Invalidate();
    stopChanged_.notify_all();
    d3d11Capture_.WakeWorker();

    if (worker_.joinable()) {
        std::thread worker = std::move(worker_);
        lifecycleLock.unlock();
        worker.join();
        lifecycleLock.lock();
    }

    d3d11Capture_.Detach();
    ReleasePointerDrag();
    ResetThumbstickScroll("runtime-stop");
    openXr_.Reset();
    registrar_ = {};
    d3d11HooksReady_ = false;
    if (state_.load(std::memory_order_acquire) != VrRuntimeState::Disabled) {
        state_.store(VrRuntimeState::Stopped, std::memory_order_release);
    }
    log_.Close();
}

void VrRuntime::DispatchPointerInput(
    const openxr::OpenXrContext::StereoFrame& frame,
    HWND outputWindow) {
    if (!frame.mirrorInputEnabled || frame.mirrorPresentationChanged) {
        ReleasePointerDrag();
        ResetThumbstickScroll(
            frame.mirrorPresentationChanged ? "mirror-presentation-changed" :
                                              "mirror-input-disabled");
        return;
    }
    if (outputWindow == nullptr || !IsWindow(outputWindow)) {
        ReleasePointerDrag();
        ResetThumbstickScroll("invalid-output-window");
        return;
    }

    RECT client{};
    if (!GetClientRect(outputWindow, &client)) {
        ReleasePointerDrag();
        ResetThumbstickScroll("client-rect-failed");
        return;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        ReleasePointerDrag();
        ResetThumbstickScroll("invalid-client-size");
        return;
    }

    if (frame.mirrorLayoutTransitionPending) {
        ReleasePointerDrag();
        ResetThumbstickScroll("layout-transition");
        inputLayoutReady_ = false;
        if (!inputLayoutPendingLogged_) {
            std::ostringstream stream;
            stream << "[VR][input] INPUT_LAYOUT_WAIT generation="
                   << frame.mirrorLayoutGeneration << " mirror="
                   << frame.mirrorWidth << 'x' << frame.mirrorHeight
                   << " client=" << width << 'x' << height
                   << " pending=1";
            log_.Write(stream.str());
            inputLayoutPendingLogged_ = true;
        }
        return;
    }
    inputLayoutPendingLogged_ = false;

    const bool layoutChanged = frame.mirrorLayoutChanged ||
        frame.mirrorLayoutGeneration != inputMirrorLayoutGeneration_;
    if (layoutChanged) {
        ReleasePointerDrag();
        ResetThumbstickScroll("layout-changed");
        inputMirrorLayoutGeneration_ = frame.mirrorLayoutGeneration;
        inputLayoutReady_ = false;
        inputLayoutMismatchLogged_ = false;
        inputLayoutPendingLogged_ = false;
        std::ostringstream stream;
        stream << "[VR][input] INPUT_LAYOUT_CHANGED generation="
               << frame.mirrorLayoutGeneration << " mirror="
               << frame.mirrorWidth << 'x' << frame.mirrorHeight
               << " client=" << width << 'x' << height;
        log_.Write(stream.str());
    }

    const bool validMirrorLayout = frame.mirrorLayoutGeneration != 0 &&
        frame.mirrorWidth != 0 && frame.mirrorHeight != 0;
    const int mirrorOrientation = frame.mirrorWidth == frame.mirrorHeight
        ? 0
        : (frame.mirrorWidth > frame.mirrorHeight ? 1 : -1);
    const int clientOrientation = width == height ? 0 : (width > height ? 1 : -1);
    const double mirrorAspect = validMirrorLayout
        ? static_cast<double>(frame.mirrorWidth) /
              static_cast<double>(frame.mirrorHeight)
        : 0.0;
    const double clientAspect = static_cast<double>(width) / static_cast<double>(height);
    const double aspectScale = std::max(mirrorAspect, clientAspect);
    const bool aspectMatches = validMirrorLayout &&
        mirrorOrientation == clientOrientation && aspectScale > 0.0 &&
        std::abs(mirrorAspect - clientAspect) <= aspectScale * 0.02;
    if (!aspectMatches) {
        ReleasePointerDrag();
        ResetThumbstickScroll("layout-mismatch");
        inputLayoutReady_ = false;
        if (!inputLayoutMismatchLogged_) {
            std::ostringstream stream;
            stream << "[VR][input] INPUT_LAYOUT_WAIT generation="
                   << frame.mirrorLayoutGeneration << " mirror="
                   << frame.mirrorWidth << 'x' << frame.mirrorHeight
                   << " client=" << width << 'x' << height;
            log_.Write(stream.str());
            inputLayoutMismatchLogged_ = true;
        }
        return;
    }
    if (!inputLayoutReady_) {
        std::ostringstream stream;
        stream << "[VR][input] INPUT_LAYOUT_READY generation="
               << frame.mirrorLayoutGeneration << " mirror="
               << frame.mirrorWidth << 'x' << frame.mirrorHeight
               << " client=" << width << 'x' << height;
        log_.Write(stream.str());
        inputLayoutReady_ = true;
        inputLayoutMismatchLogged_ = false;
    }
    // Do not turn a trigger edge sampled on the swapchain-rebuild frame into a
    // click. A held trigger remains held, so the next frame cannot create a new
    // rising edge accidentally.
    if (layoutChanged) {
        return;
    }

    const int dragThresholdPixels = input::PointerGesture::Radius(width, height);

    auto coordinates = [&](const openxr::OpenXrContext::PointerState& pointer) {
        const int x = std::clamp(
            static_cast<int>(std::lround(pointer.u * static_cast<float>(width - 1))),
            0,
            width - 1);
        const int y = std::clamp(
            static_cast<int>(std::lround(pointer.v * static_cast<float>(height - 1))),
            0,
            height - 1);
        return POINT{x, y};
    };

    const bool testBackend = UsePostMessageTestInput();
    auto dispatchMouse = [&](POINT point,
                             bool buttonDown,
                             bool buttonUp,
                             bool buttonHeld,
                             int wheelDelta) {
        return testBackend
                   ? PostTestMouseInput(
                         outputWindow,
                         point,
                         buttonDown,
                         buttonUp,
                         buttonHeld,
                         wheelDelta)
                   : QueueNativeMouseInput(point, width, height, buttonDown,
                                           buttonUp, buttonHeld);
    };
    auto appendBackendStatus = [&](std::ostringstream& stream,
                                   const MouseDispatchResult& result) {
        stream << " backend=" << (testBackend ? "PostMessage" : "UnityNative")
               << " delivered=" << result.delivered;
        if (!testBackend) {
            stream << " queued=" << result.delivered << " error=" << result.error;
        }
    };

    if (draggingPointerHand_ < frame.pointers.size()) {
        input::RenewUnityPointer();
        const auto& pointer = frame.pointers[draggingPointerHand_];
        if (!pointer.triggerHeld) {
            const MouseDispatchResult result = dispatchMouse(
                lastDragClient_, false, true, true, 0);
            if (!result.delivered && !testBackend) input::CancelUnityPointer();
            std::ostringstream stream;
            stream << "[VR][input] " << (dragMoved_ ? "POINTER_DRAG" : "POINTER_CLICK")
                   << " hand=" << (draggingPointerHand_ == 0 ? "left" : "right")
                   << " uv=(" << std::fixed << std::setprecision(3)
                   << dragStartU_ << ',' << dragStartV_ << ") from=("
                   << dragStartClient_.x << ',' << dragStartClient_.y << ") to=("
                   << lastDragClient_.x << ',' << lastDragClient_.y << ')';
            appendBackendStatus(stream, result);
            stream << " maxExcursionPx=" << pointerGesture_.MaximumDistance()
                   << " dragThresholdPx=" << dragThresholdPixels;
            log_.Write(stream.str());
            draggingPointerHand_ = frame.pointers.size();
            dragWindow_ = nullptr;
            dragMoved_ = false;
        } else if (pointer.hovering) {
            const POINT point = coordinates(pointer);
            {
                const long long deltaX =
                    static_cast<long long>(point.x) - dragStartClient_.x;
                const long long deltaY =
                    static_cast<long long>(point.y) - dragStartClient_.y;
                if (!pointerGesture_.Update(static_cast<double>(deltaX),
                        static_cast<double>(deltaY), dragThresholdPixels,
                        frame.predictedDisplayTime)) {
                    // Keep the native pointer on the press target through
                    // jitter and brief excursions; a deliberate drag latches.
                    return;
                }
            }
            const MouseDispatchResult result = dispatchMouse(
                point, false, false, true, 0);
            if (result.delivered) {
                dragMoved_ = true;
                lastDragClient_ = point;
            }
        } else if (!dragMoved_) {
            // A tracking/hover gap cannot count toward continuous intent time.
            pointerGesture_.Update(0, 0, dragThresholdPixels, frame.predictedDisplayTime);
        }
    }

    if (draggingPointerHand_ >= frame.pointers.size()) {
        const std::array<std::size_t, 2> handOrder{
            activePointerHand_ < frame.pointers.size() ? activePointerHand_ : 1U,
            activePointerHand_ == 0 ? 1U : 0U,
        };
        for (const std::size_t hand : handOrder) {
            const auto& pointer = frame.pointers[hand];
            if (!pointer.hovering || !pointer.triggerPressed) {
                continue;
            }

            const POINT point = coordinates(pointer);
            ResetThumbstickScroll("trigger-press");
            const MouseDispatchResult result = dispatchMouse(
                point, true, false, false, 0);
            std::ostringstream stream;
            stream << "[VR][input] POINTER_DOWN hand=" << (hand == 0 ? "left" : "right")
                   << " uv=(" << std::fixed << std::setprecision(3)
                   << pointer.u << ',' << pointer.v << ") client=("
                   << point.x << ',' << point.y << ") dragThresholdPx="
                   << dragThresholdPixels;
            appendBackendStatus(stream, result);
            log_.Write(stream.str());
            if (result.delivered) {
                activePointerHand_ = hand;
                draggingPointerHand_ = hand;
                dragWindow_ = outputWindow;
                dragStartClient_ = point;
                lastDragClient_ = point;
                dragStartU_ = pointer.u;
                dragStartV_ = pointer.v;
                dragMoved_ = false;
                pointerGesture_.Reset();
                dragTestBackend_ = testBackend;
            }
            break;
        }
    }

    if (draggingPointerHand_ < frame.pointers.size()) {
        ResetThumbstickScroll("trigger-drag");
        return;
    }

    // While a VR free-camera mode is active the sticks belong to locomotion;
    // the mirror-panel wheel scroll would otherwise fight the camera.
    if (camera::IsVrFreeCameraLocomotionActive()) {
        ResetThumbstickScroll("free-camera");
        return;
    }

    std::size_t scrollHand = frame.pointers.size();
    float largestDeflection = input::ThumbstickScrollIntegrator::kDeadzone;
    for (std::size_t hand = 0; hand < frame.pointers.size(); ++hand) {
        const auto& pointer = frame.pointers[hand];
        const float deflection = std::max(
            std::abs(pointer.thumbstick.x),
            std::abs(pointer.thumbstick.y));
        if (pointer.thumbstickActive && pointer.hovering &&
            deflection > largestDeflection) {
            largestDeflection = deflection;
            scrollHand = hand;
        }
    }
    if (scrollHand >= frame.pointers.size()) {
        ResetThumbstickScroll("stick-centered-or-hover-lost");
        return;
    }

    activePointerHand_ = scrollHand;
    if (scrollingPointerHand_ != scrollHand) {
        ResetThumbstickScroll("hand-switch");
        scrollingPointerHand_ = scrollHand;
    }

    const auto& scrollPointer = frame.pointers[scrollHand];
    const POINT scrollPoint = coordinates(scrollPointer);
    const bool scrollWasActive = thumbstickScrollHorizontal_.Active() ||
        thumbstickScrollVertical_.Active();
    const input::ThumbstickScrollStep horizontalStep =
        thumbstickScrollHorizontal_.Update(
            frame.predictedDisplayTime,
            scrollPointer.thumbstick.x);
    const input::ThumbstickScrollStep verticalStep =
        thumbstickScrollVertical_.Update(
            frame.predictedDisplayTime,
            scrollPointer.thumbstick.y);
    const bool scrollIsActive = thumbstickScrollHorizontal_.Active() ||
        thumbstickScrollVertical_.Active();
    if (!scrollWasActive && scrollIsActive) {
        input::ResetUnityAnalogScroll();
        nextScrollLogTime_ = 0;
        std::ostringstream beginLog;
        beginLog << "[VR][input] POINTER_SCROLL_BEGIN hand="
                 << (scrollHand == 0 ? "left" : "right")
                 << " stick=(" << std::fixed << std::setprecision(3)
                 << scrollPointer.thumbstick.x << ','
                 << scrollPointer.thumbstick.y << ')'
                 << " normalized=(" << horizontalStep.normalizedDeflection << ','
                 << verticalStep.normalizedDeflection << ')'
                 << " unityRate=(" << std::setprecision(2)
                 << -horizontalStep.unityUnitsPerSecond << ','
                 << verticalStep.unityUnitsPerSecond << ')'
                 << " client=(" << scrollPoint.x << ',' << scrollPoint.y << ')';
        log_.Write(beginLog.str());
    }
    const bool directionChanged = horizontalStep.directionChanged ||
        verticalStep.directionChanged;
    if (directionChanged) {
        input::ResetUnityAnalogScroll();
        std::ostringstream directionLog;
        directionLog << "[VR][input] POINTER_SCROLL_DIRECTION hand="
                     << (scrollHand == 0 ? "left" : "right")
                     << " stick=(" << std::fixed << std::setprecision(3)
                     << scrollPointer.thumbstick.x << ','
                     << scrollPointer.thumbstick.y << ')';
        log_.Write(directionLog.str());
    }
    const bool timingReset = horizontalStep.timingReset ||
        verticalStep.timingReset;
    if (timingReset) {
        input::ResetUnityAnalogScroll();
        std::ostringstream timingLog;
        timingLog << "[VR][input] POINTER_SCROLL_TIMING_RESET hand="
                  << (scrollHand == 0 ? "left" : "right")
                  << " predictedDisplayTime=" << frame.predictedDisplayTime;
        log_.Write(timingLog.str());
    }
    if (horizontalStep.stopped || verticalStep.stopped) {
        input::ResetUnityAnalogScroll();
    }

    const float unityDeltaX = -horizontalStep.unityDelta;
    const float unityDeltaY = verticalStep.unityDelta;
    const bool unityAnalog =
        !testBackend && input::UnityAnalogScrollAvailable();
    if (!testBackend && !unityAnalog) {
        ResetThumbstickScroll("native-scroll-unavailable");
        return;
    }
    const bool horizontalDominant =
        std::abs(scrollPointer.thumbstick.x) >
        std::abs(scrollPointer.thumbstick.y);
    const int fallbackWheelDelta = horizontalDominant
        ? -horizontalStep.wheelDelta
        : verticalStep.wheelDelta;
    if ((unityAnalog && unityDeltaX == 0.0F && unityDeltaY == 0.0F) ||
        (!unityAnalog && fallbackWheelDelta == 0)) {
        return;
    }

    const MouseDispatchResult scrollResult = dispatchMouse(
        scrollPoint,
        false,
        false,
        false,
        unityAnalog ? 0 : fallbackWheelDelta);
    const bool analogQueued = !unityAnalog ||
        (scrollResult.delivered &&
         input::QueueUnityAnalogScrollDelta(unityDeltaX, unityDeltaY));
    constexpr XrDuration kScrollLogInterval = 250'000'000;
    if (!scrollResult.delivered || !analogQueued || nextScrollLogTime_ == 0 ||
        frame.predictedDisplayTime >= nextScrollLogTime_) {
        std::ostringstream scrollLog;
        scrollLog << "[VR][input] POINTER_SCROLL hand="
                  << (scrollHand == 0 ? "left" : "right")
                  << " stick=(" << std::fixed << std::setprecision(3)
                  << scrollPointer.thumbstick.x << ','
                  << scrollPointer.thumbstick.y << ')'
                  << " normalized=(" << horizontalStep.normalizedDeflection << ','
                  << verticalStep.normalizedDeflection << ')'
                  << " unityRate=(" << std::setprecision(2)
                  << -horizontalStep.unityUnitsPerSecond << ','
                  << verticalStep.unityUnitsPerSecond << ')'
                  << " dtMs=(" << std::setprecision(3)
                  << horizontalStep.deltaSeconds * 1000.0F << ','
                  << verticalStep.deltaSeconds * 1000.0F << ')';
        if (unityAnalog) {
            scrollLog << " delta=(" << std::setprecision(5)
                      << unityDeltaX << ',' << unityDeltaY << ')'
                      << " transport=UnityAnalog queued=" << analogQueued;
        } else {
            scrollLog << " delta=" << fallbackWheelDelta
                      << " transport=WindowsWheel";
        }
        scrollLog << " client=(" << scrollPoint.x << ',' << scrollPoint.y << ')';
        appendBackendStatus(scrollLog, scrollResult);
        log_.Write(scrollLog.str());
        if (scrollResult.delivered && analogQueued) {
            nextScrollLogTime_ =
                frame.predictedDisplayTime + kScrollLogInterval;
        }
    }
}

void VrRuntime::ResetThumbstickScroll(std::string_view reason) noexcept {
    const bool wasActive = thumbstickScrollHorizontal_.Active() ||
        thumbstickScrollVertical_.Active();
    const std::size_t previousHand = scrollingPointerHand_;
    thumbstickScrollHorizontal_.Reset();
    thumbstickScrollVertical_.Reset();
    input::ResetUnityAnalogScroll();
    scrollingPointerHand_ = 2;
    nextScrollLogTime_ = 0;
    if (wasActive && !reason.empty()) {
        input::CancelUnityPointer();
        log_.Write(
            "[VR][input] POINTER_SCROLL_END hand=" +
            std::string(previousHand == 0 ? "left" : "right") +
            " reason=" + std::string(reason));
    }
}

void VrRuntime::ReleasePointerDrag() noexcept {
    input::CancelUnityPointer();
    pointerGesture_.Reset();
    if (draggingPointerHand_ >= 2 || dragWindow_ == nullptr) {
        draggingPointerHand_ = 2;
        dragWindow_ = nullptr;
        dragMoved_ = false;
        return;
    }

    const auto result = dragTestBackend_
        ? PostTestMouseInput(dragWindow_, lastDragClient_, false, true, true, 0)
        : MouseDispatchResult{true};
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        log_.Write("[VR][input] POINTER_FORCE_RELEASE delivered=" +
            std::to_string(result.delivered) + " error=" + std::to_string(result.error));
    }
    draggingPointerHand_ = 2;
    dragWindow_ = nullptr;
    dragMoved_ = false;
}

void VrRuntime::WorkerMain() noexcept {
    try {
        WorkerMainImpl();
    } catch (const std::exception& exception) {
        Fault("unhandled worker exception");
        log_.Write(exception.what());
    } catch (...) {
        Fault("unknown unhandled worker exception");
    }

    // These operations are noexcept and intentionally run for every normal or
    // exceptional worker exit. Keeping the boundary here prevents a diagnostic
    // allocation failure from terminating the host game.
    confirmedMirrorLayout_.store(0, std::memory_order_release);
    poseMailbox_.Invalidate();
    stereoRenderMailbox_.Invalidate();
    cameraInputMailbox_.Invalidate();
    ReleasePointerDrag();
    ResetThumbstickScroll("worker-stop");
    openXr_.Reset();
    d3d11Capture_.Detach();
    if (State() != VrRuntimeState::Faulted && State() != VrRuntimeState::Disabled) {
        state_.store(VrRuntimeState::Stopped, std::memory_order_release);
        log_.Write("[VR][runtime] state=Stopped");
    }
    log_.Write("[VR][runtime] worker stopped");
}

void VrRuntime::WorkerMainImpl() {
    openXr_.SetAaMenuHooks(
        &PaintVrAaMenu, &ShutdownVrAaMenu, &FlushVrAaMenuConfigSave);
    openXr_.SetPanelOverlayHooks(&PaintVrPanelOverlay);
    confirmedMirrorLayout_.store(0, std::memory_order_release);
    std::uint64_t capturedGeneration = 0;
    bool headsetWaitWasLogged = false;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        if (!EnsureD3D11Hooks()) {
            if (State() == VrRuntimeState::Faulted) {
                break;
            }
            WaitOrStop(config_.graphicsPollInterval);
            continue;
        }

        SetState(VrRuntimeState::WaitingForRuntime);
        const auto initializeResult = openXr_.Initialize(config_.applicationDirectory, log_);
        lastOpenXrResult_.store(openXr_.LastResult(), std::memory_order_release);
        if (initializeResult == openxr::OpenXrContext::InitializeResult::RetryRuntime) {
            WaitOrStop(config_.runtimeRetryInterval);
            continue;
        }
        if (initializeResult == openxr::OpenXrContext::InitializeResult::LoaderMissing) {
            Fault("app-local openxr_loader.dll is required");
            break;
        }
        if (initializeResult == openxr::OpenXrContext::InitializeResult::ExtensionMissing) {
            Fault("active runtime does not expose XR_KHR_D3D11_enable");
            break;
        }
        if (initializeResult != openxr::OpenXrContext::InitializeResult::Ready) {
            Fault("OpenXR instance initialization failed");
            break;
        }
        SetState(VrRuntimeState::InstanceReady);

        bool systemReady = false;
        while (!stopRequested_.load(std::memory_order_acquire)) {
            const auto systemResult = openXr_.AcquireHeadMountedSystem(log_);
            lastOpenXrResult_.store(openXr_.LastResult(), std::memory_order_release);
            if (systemResult == openxr::OpenXrContext::SystemResult::Ready) {
                systemReady = true;
                break;
            }
            if (systemResult != openxr::OpenXrContext::SystemResult::RetryHeadset) {
                Fault("xrGetSystem/xrGetSystemProperties failed");
                break;
            }

            SetState(VrRuntimeState::WaitingForRuntime);
            if (!headsetWaitWasLogged) {
                log_.Write("[VR][runtime] HMD unavailable; connect Virtual Desktop/headset and retrying");
                WriteVrConsole("OpenXR HMD unavailable; connect headset and retry");
                headsetWaitWasLogged = true;
            }
            if (WaitOrStop(config_.runtimeRetryInterval)) {
                break;
            }
        }
        if (!systemReady) {
            break;
        }
        headsetWaitWasLogged = false;
        SetState(VrRuntimeState::SystemReady);

        openxr::OpenXrContext::D3D11Requirements requirements;
        if (!openXr_.QueryD3D11Requirements(requirements, log_)) {
            lastOpenXrResult_.store(openXr_.LastResult(), std::memory_order_release);
            if (openXr_.LastResult() == XR_ERROR_INSTANCE_LOST ||
                openXr_.LastResult() == XR_ERROR_RUNTIME_UNAVAILABLE) {
                openXr_.ResetInstance();
                WaitOrStop(config_.runtimeRetryInterval);
                continue;
            }
            Fault("xrGetD3D11GraphicsRequirementsKHR failed");
            break;
        }

        SetState(VrRuntimeState::WaitingForGraphics);
        d3d11::D3D11Capture::Snapshot graphics;
        while (!stopRequested_.load(std::memory_order_acquire) &&
               !d3d11Capture_.WaitForSnapshot(
                   capturedGeneration,
                   graphics,
                   config_.graphicsPollInterval)) {
            // Capture detours signal the condition variable. The timeout only
            // bounds shutdown latency when no Unity frame is being presented.
        }
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }

        log_.Write(
            "[VR][runtime] Unity D3D11 captured: LUID=" + LuidText(graphics.adapterLuid) +
            " featureLevel=" + FeatureLevelText(graphics.featureLevel) +
            " flags=" + std::to_string(graphics.deviceFlags) +
            " multithreadProtected=" +
            (graphics.multithreadProtected ? "1" : "0") +
            " frame=" + std::to_string(graphics.frameDescription.Width) + "x" +
            std::to_string(graphics.frameDescription.Height) +
            " frameFormat=" +
            std::to_string(static_cast<int>(graphics.frameDescription.Format)) +
            " frameStableCount=" +
            std::to_string(graphics.frameDescriptionStableCount) +
            " frameGeneration=" + std::to_string(graphics.frameGeneration) +
            " layoutGeneration=" + std::to_string(graphics.layoutGeneration));
        if ((graphics.deviceFlags & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0) {
            log_.Write(
                "[VR][runtime] Unity D3D11 device is SINGLETHREADED; refusing "
                "worker-thread xrCreateSession");
            Fault("Unity D3D11 device cannot be bound safely from the VR worker");
            break;
        }
        if (!graphics.multithreadProtected) {
            log_.Write(
                "[VR][runtime] D3D11 immediate-context multithread protection is unavailable; "
                "refusing worker-thread rendering");
            Fault("Unity D3D11 immediate context cannot be used safely from the VR worker");
            break;
        }

        const auto sessionResult = openXr_.CreateD3D11Session(
            graphics.device,
            graphics.adapterLuid,
            graphics.featureLevel,
            graphics.frameDescription,
            graphics.layoutGeneration,
            requirements,
            config_.stereoProjectionEnabled,
            config_.stereoLandscapeOnly,
            config_.stereoRenderScale,
            log_);
        lastOpenXrResult_.store(openXr_.LastResult(), std::memory_order_release);
        if (sessionResult == openxr::OpenXrContext::SessionResult::AdapterMismatch) {
            Fault("Unity D3D11 adapter LUID does not match the OpenXR runtime requirement");
            break;
        }
        if (sessionResult == openxr::OpenXrContext::SessionResult::FeatureLevelMismatch) {
            Fault("Unity D3D11 feature level is below the OpenXR runtime requirement");
            break;
        }
        if (sessionResult == openxr::OpenXrContext::SessionResult::RetryRuntime) {
            openXr_.ResetInstance();
            capturedGeneration = 0;
            WaitOrStop(config_.runtimeRetryInterval);
            continue;
        }
        if (sessionResult != openxr::OpenXrContext::SessionResult::Ready) {
            Fault("xrCreateSession failed after graphics validation");
            break;
        }

        SetState(VrRuntimeState::SessionReady);
        stereoEyeWidth_.store(openXr_.StereoEyeWidth(), std::memory_order_release);
        stereoEyeHeight_.store(openXr_.StereoEyeHeight(), std::memory_order_release);
        stereoTargetGeneration_.fetch_add(1, std::memory_order_acq_rel);
        std::uint64_t frameIndex = 0;
        std::uint64_t observedRunGeneration = 0;
        bool currentRunPoseReady = false;
        bool currentRunDisplayReady = false;
        bool currentRunInputReady = false;
        bool everPoseReady = false;
        bool runtimeReadyLogged = false;
        bool restartGraphicsSession = false;
        bool graphicsRestartExitRequested = false;
        d3d11::D3D11Capture::FrameSnapshot activeSourceFrame;
        std::optional<std::chrono::steady_clock::time_point> notRenderingSince;
        auto nextDisplayPauseLog = std::chrono::steady_clock::time_point::min();
        XrViewStateFlags previousViewFlags = std::numeric_limits<XrViewStateFlags>::max();
        auto nextPoseSample = std::chrono::steady_clock::now();

        while (!stopRequested_.load(std::memory_order_acquire)) {
            const auto eventResult = openXr_.DrainEvents(log_);
            PumpGripTraceOutput();
            if (eventResult == openxr::OpenXrContext::EventResult::SessionExiting) {
                confirmedMirrorLayout_.store(0, std::memory_order_release);
                if (restartGraphicsSession && !graphicsRestartExitRequested) {
                    restartGraphicsSession = false;
                    log_.Write(
                        "[VR][display] external session exit superseded the "
                        "pending graphics rebuild");
                }
                log_.Write("[VR][pose] session exiting normally");
                break;
            }
            if (eventResult != openxr::OpenXrContext::EventResult::Healthy) {
                lastOpenXrResult_.store(openXr_.LastResult(), std::memory_order_release);
                if (everPoseReady) {
                    log_.Write("[VR][pose] READY_INVALIDATED");
                }
                Fault("OpenXR session or instance became invalid during pose probing");
                break;
            }

            if (restartGraphicsSession) {
                if (!openXr_.IsSessionRunning()) {
                    break;
                }
                if (!graphicsRestartExitRequested) {
                    if (!openXr_.RequestExit(log_)) {
                        lastOpenXrResult_.store(
                            openXr_.LastResult(), std::memory_order_release);
                        Fault(
                            "unable to stop the running OpenXR session for "
                            "graphics rebuild");
                        break;
                    }
                    graphicsRestartExitRequested = true;
                    log_.Write(
                        "[VR][display] GRAPHICS_REBUILD_EXIT_REQUESTED");
                }
                if (WaitOrStop(std::min(
                        config_.graphicsPollInterval,
                        std::chrono::milliseconds(10)))) {
                    break;
                }
                continue;
            }

            const std::uint64_t runGeneration = openXr_.SessionRunGeneration();
            if (runGeneration != observedRunGeneration) {
                confirmedMirrorLayout_.store(0, std::memory_order_release);
                poseMailbox_.Invalidate(
                    runGeneration,
                    static_cast<std::int32_t>(openXr_.ActiveReferenceSpaceType()));
                stereoRenderMailbox_.Invalidate();
                ReleasePointerDrag();
                ResetThumbstickScroll("session-generation");
                observedRunGeneration = runGeneration;
                currentRunPoseReady = false;
                currentRunDisplayReady = false;
                currentRunInputReady = false;
                activePointerHand_ = 1;
                inputMirrorLayoutGeneration_ = 0;
                inputLayoutReady_ = false;
                inputLayoutMismatchLogged_ = false;
                inputLayoutPendingLogged_ = false;
                notRenderingSince.reset();
                nextDisplayPauseLog =
                    std::chrono::steady_clock::time_point::min();
                previousViewFlags = std::numeric_limits<XrViewStateFlags>::max();
                nextPoseSample = std::chrono::steady_clock::now();
                SetState(VrRuntimeState::SessionRunning);
                log_.Write(
                    "[VR][pose] FRAME_LOOP_STARTED generation=" +
                    std::to_string(runGeneration));
            }

            if (!openXr_.IsSessionRunning()) {
                confirmedMirrorLayout_.store(0, std::memory_order_release);
                SetState(VrRuntimeState::SessionReady);
                if (WaitOrStop(std::min(
                        config_.graphicsPollInterval,
                        std::chrono::milliseconds(50)))) {
                    break;
                }
                continue;
            }

            openxr::OpenXrContext::StereoFrame frame;
            d3d11::D3D11Capture::FrameSnapshot latestSourceFrame;
            const bool latestSourceAvailable =
                d3d11Capture_.GetLatestFrame(latestSourceFrame);
            if (latestSourceAvailable) {
                ID3D11Device* sourceDevice = nullptr;
                latestSourceFrame.texture->GetDevice(&sourceDevice);
                const bool sourceDeviceMatches = sourceDevice == graphics.device;
                if (sourceDevice != nullptr) {
                    sourceDevice->Release();
                }
                if (!sourceDeviceMatches) {
                    confirmedMirrorLayout_.store(0, std::memory_order_release);
                    log_.Write(
                        "[VR][display] Unity D3D11 device changed; requesting "
                        "an orderly OpenXR graphics-session rebuild");
                    ReleasePointerDrag();
                    ResetThumbstickScroll("graphics-device-changed");
                    restartGraphicsSession = true;
                    continue;
                }
                activeSourceFrame = std::move(latestSourceFrame);
            } else if (!activeSourceFrame.IsComplete()) {
                confirmedMirrorLayout_.store(0, std::memory_order_release);
                log_.Write(
                    "[VR][display] Unity mirror frame became unavailable before "
                    "the frame loop started; requesting a graphics-session rebuild");
                ReleasePointerDrag();
                ResetThumbstickScroll("mirror-frame-unavailable");
                restartGraphicsSession = true;
                continue;
            }
            confirmedMirrorLayout_.store(
                ConfirmedMirrorLayoutState(
                    latestSourceAvailable,
                    activeSourceFrame.layoutTransitionPending,
                    activeSourceFrame.layoutGeneration,
                    activeSourceFrame.description.Width,
                    activeSourceFrame.description.Height),
                std::memory_order_release);
            // Input to RunFrame (latched across its entry reset): the game
            // thread's Grip transparency request rides the frame so the quad
            // submission can pick source-alpha blending.
            frame.gripPanelTransparent =
                gripPanelTransparent_.load(std::memory_order_acquire);
            d3d11::D3D11Capture::TransparencyProbe transparencyProbe{};
            if (d3d11Capture_.ConsumeTransparencyProbe(&transparencyProbe)) {
                // One-shot forensic record per arming: proves whether the
                // post-Present clear ran (clears/clearErr) and what the
                // presented backbuffer actually contains (corner/centre
                // texels) when the panel still looks frozen or opaque.
                std::ostringstream probeLog;
                probeLog << "[VR][display] GRIP_TRANSPARENCY_PROBE clears="
                         << transparencyProbe.clearCount << " clearErr=0x"
                         << std::hex << transparencyProbe.lastClearError
                         << " sampleErr=0x" << transparencyProbe.sampleError
                         << std::dec << " fmt=" << transparencyProbe.format
                         << " size=" << transparencyProbe.width << 'x'
                         << transparencyProbe.height << std::hex;
                static constexpr const char* kProbePointNames[4] = {
                    "topLeft", "topRight", "centre", "bottomLeft"};
                for (std::size_t index = 0;
                     index < transparencyProbe.pixels.size(); ++index) {
                    probeLog << ' ' << kProbePointNames[index] << "=0x"
                             << std::setw(8) << std::setfill('0')
                             << transparencyProbe.pixels[index];
                }
                log_.Write(probeLog.str());
            }
            d3d11::D3D11Capture::ClearTargetReport clearTargetReport{};
            if (d3d11Capture_.ConsumeClearTargetReport(&clearTargetReport)) {
                // .226 adoption record: which big Unity RenderTextures were
                // matched against the swapchain size and now get cleared to
                // transparent black each Present while armed.
                std::ostringstream reportLog;
                reportLog << "[VR][display] GRIP_TRANSPARENCY_RT candidates="
                          << clearTargetReport.candidates
                          << " adopted=" << clearTargetReport.adopted;
                log_.Write(reportLog.str());
                for (std::uint32_t index = 0; index < clearTargetReport.count;
                     ++index) {
                    const auto& entry = clearTargetReport.entries[index];
                    std::ostringstream entryLog;
                    entryLog << "[VR][display] GRIP_TRANSPARENCY_RT tex=0x"
                             << std::hex << entry.texture << std::dec
                             << " size=" << entry.width << 'x' << entry.height
                             << " fmt=" << entry.format
                             << " adopted=" << entry.adopted << " hr=0x"
                             << std::hex << entry.hresult;
                    log_.Write(entryLog.str());
                }
            }
            const auto frameResult = openXr_.RunFrame(
                frame,
                activeSourceFrame.texture,
                activeSourceFrame.generation,
                activeSourceFrame.layoutGeneration,
                config_.stereoProjectionEnabled ? &stereoRenderMailbox_ : nullptr,
                log_);
            lastOpenXrResult_.store(openXr_.LastResult(), std::memory_order_release);
            if (frameResult == openxr::OpenXrContext::FrameResult::SessionNotRunning) {
                confirmedMirrorLayout_.store(0, std::memory_order_release);
                ResetThumbstickScroll("session-not-running");
                continue;
            }
            if (frameResult != openxr::OpenXrContext::FrameResult::Completed) {
                if (everPoseReady) {
                    log_.Write("[VR][pose] READY_INVALIDATED");
                }
                Fault("OpenXR display/pose frame loop failed");
                break;
            }

            // The VR menu can rescale the eye targets mid-session. Publishing a
            // new generation is what makes Unity drop and rebuild its eye
            // RenderTextures at the new size.
            PublishStereoTargetSpecChange();

            if (frame.referenceSpaceChanged) {
                poseMailbox_.Invalidate(
                    runGeneration,
                    static_cast<std::int32_t>(openXr_.ActiveReferenceSpaceType()));
                stereoRenderMailbox_.Invalidate();
                ReleasePointerDrag();
                ResetThumbstickScroll("reference-space-changed");
                log_.Write(
                    "[VR][pose] REFERENCE_SPACE_BASELINE_INVALIDATED generation=" +
                    std::to_string(runGeneration) + " space=" +
                    std::to_string(static_cast<std::int32_t>(
                        openXr_.ActiveReferenceSpaceType())) +
                    " predictedDisplayTime=" +
                    std::to_string(frame.predictedDisplayTime));
            }

            frame.mirrorLayoutTransitionPending =
                activeSourceFrame.layoutTransitionPending ||
                !latestSourceAvailable;
            if (!frame.referenceSpaceChanged) {
                DispatchPointerInput(
                    frame,
                    activeSourceFrame.outputWindow != nullptr
                        ? activeSourceFrame.outputWindow
                        : graphics.outputWindow);
            }

            {
                camera::VrCameraInputSample cameraInput;
                cameraInput.valid = true;
                cameraInput.menuVisible = frame.aaMenuVisible;
                if (frame.pointers[0].thumbstickActive) {
                    cameraInput.leftStickX = frame.pointers[0].thumbstick.x;
                    cameraInput.leftStickY = frame.pointers[0].thumbstick.y;
                }
                if (frame.pointers[1].thumbstickActive) {
                    cameraInput.rightStickX = frame.pointers[1].thumbstick.x;
                    cameraInput.rightStickY = frame.pointers[1].thumbstick.y;
                }
                const auto& leftPointer = frame.pointers[0];
                cameraInput.sprintHeld = camera::FreeMoveSprintFromLeftTrigger(
                    leftPointer.triggerHeld,
                    leftPointer.hovering && frame.stereoUiPanelVisible,
                    leftPointer.menuHovering && frame.aaMenuVisible,
                    leftPointer.barHovering);
                cameraInput.modePressCount = openXr_.CameraModePressCount();
                cameraInput.charaPressCount = openXr_.CameraCharaPressCount();
                cameraInput.resetPressCount = openXr_.CameraResetPressCount();
                cameraInput.photoPressCount = openXr_.PhotoPressCount();
                cameraInput.publishTimeNanoseconds =
                    pose::MonotonicNowNanoseconds();
                cameraInputMailbox_.Publish(cameraInput);
            }

            ++frameIndex;
            if (frame.frameDiscarded) {
                log_.Write(
                    "[VR][pose] xrBeginFrame discarded an older frame at frame=" +
                    std::to_string(frameIndex));
            }
            if (frame.viewStateFlags != previousViewFlags) {
                previousViewFlags = frame.viewStateFlags;
                log_.Write(ViewFlagsText(frame.viewStateFlags));
            }

            constexpr XrViewStateFlags kRequiredPoseFlags =
                XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                XR_VIEW_STATE_POSITION_VALID_BIT;
            const bool validStereoPose =
                frame.viewCount == 2 &&
                HasViewFlags(frame.viewStateFlags, kRequiredPoseFlags);
            constexpr XrViewStateFlags kRequiredBridgePoseFlags =
                kRequiredPoseFlags |
                XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
                XR_VIEW_STATE_POSITION_TRACKED_BIT;
            const bool validTrackedStereoPose =
                frame.shouldRender && frame.viewCount == 2 &&
                HasViewFlags(frame.viewStateFlags, kRequiredBridgePoseFlags);
            if (validTrackedStereoPose) {
                pose::StereoPoseSample poseSample;
                poseSample.valid = true;
                poseSample.sessionGeneration = runGeneration;
                poseSample.referenceSpaceType = static_cast<std::int32_t>(
                    openXr_.ActiveReferenceSpaceType());
                poseSample.predictedDisplayTime = frame.predictedDisplayTime;
                poseSample.viewStateFlags = frame.viewStateFlags;
                poseSample.viewCount = frame.viewCount;
                for (std::size_t eyeIndex = 0; eyeIndex < poseSample.eyes.size(); ++eyeIndex) {
                    const auto& sourceEye = frame.views[eyeIndex];
                    auto& destinationEye = poseSample.eyes[eyeIndex];
                    destinationEye.pose.position = {
                        sourceEye.pose.position.x,
                        sourceEye.pose.position.y,
                        sourceEye.pose.position.z,
                    };
                    destinationEye.pose.orientation = {
                        sourceEye.pose.orientation.x,
                        sourceEye.pose.orientation.y,
                        sourceEye.pose.orientation.z,
                        sourceEye.pose.orientation.w,
                    };
                    destinationEye.fov = {
                        sourceEye.fov.angleLeft,
                        sourceEye.fov.angleRight,
                        sourceEye.fov.angleUp,
                        sourceEye.fov.angleDown,
                    };
                }
                poseMailbox_.Publish(poseSample);
            } else {
                poseMailbox_.Invalidate(
                    runGeneration,
                    static_cast<std::int32_t>(openXr_.ActiveReferenceSpaceType()));
                stereoRenderMailbox_.Invalidate();
            }
            if (validStereoPose && !currentRunPoseReady) {
                currentRunPoseReady = true;
                SetState(VrRuntimeState::PoseReady);
                log_.Write(
                    "[VR][pose] POSE_STREAM_READY generation=" +
                    std::to_string(runGeneration));
                everPoseReady = true;
            } else if (!validStereoPose && currentRunPoseReady) {
                currentRunPoseReady = false;
                SetState(VrRuntimeState::SessionRunning);
                log_.Write(
                    "[VR][pose] POSE_STREAM_LOST generation=" +
                    std::to_string(runGeneration));
            }

            const auto now = std::chrono::steady_clock::now();
            if (frame.layerSubmitted && !currentRunDisplayReady) {
                currentRunDisplayReady = true;
                log_.Write(
                    "[VR][display] GAME_MIRROR_VISIBLE generation=" +
                    std::to_string(runGeneration) +
                    " sourceFrame=" + std::to_string(frame.sourceFrameGeneration));
            }
            if (frame.stereoLayerSubmitted && frame.stereoFrameGeneration != 0 &&
                frame.stereoFrameGeneration !=
                    lastLoggedProjectionFrameGeneration_ &&
                (frame.stereoFrameGeneration == 1U ||
                 frame.stereoFrameGeneration % 300U == 0U)) {
                lastLoggedProjectionFrameGeneration_ =
                    frame.stereoFrameGeneration;
                log_.Write(
                    "[VR][stereo] PROJECTION_FRAME_SUBMITTED generation=" +
                    std::to_string(frame.stereoFrameGeneration));
            }
            const bool dualPointerPoseReady = std::all_of(
                frame.pointers.begin(),
                frame.pointers.end(),
                [](const openxr::OpenXrContext::PointerState& pointer) {
                    return pointer.poseActive && pointer.poseValid;
                });
            if (dualPointerPoseReady && !currentRunInputReady) {
                currentRunInputReady = true;
                log_.Write(
                    "[VR][input] DUAL_POINTERS_READY generation=" +
                    std::to_string(runGeneration));
            }
            if (!runtimeReadyLogged && currentRunPoseReady && currentRunDisplayReady &&
                currentRunInputReady) {
                runtimeReadyLogged = true;
                log_.Write("[VR][runtime] RUNTIME_READY");
            }

            if (frame.shouldRender) {
                if (notRenderingSince.has_value()) {
                    log_.Write(
                        "[VR][display] DISPLAY_RESUMED afterMs=" +
                        std::to_string(std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            now - *notRenderingSince).count()));
                    notRenderingSince.reset();
                }
            } else if (!notRenderingSince.has_value()) {
                notRenderingSince = now;
                log_.Write(
                    "[VR][display] DISPLAY_PAUSED shouldRender=0; "
                    "keeping OpenXR session for headset return");
            } else if (now - *notRenderingSince >= std::chrono::seconds(5) &&
                       now >= nextDisplayPauseLog) {
                log_.Write(
                    "[VR][display] DISPLAY_PAUSED_WAITING ms=" +
                    std::to_string(std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        now - *notRenderingSince).count()));
                nextDisplayPauseLog = now + std::chrono::seconds(5);
            }

            if (validStereoPose && now >= nextPoseSample) {
                log_.Write(PoseSampleText(frameIndex, frame));
                nextPoseSample = now + std::chrono::seconds(1);
            }
        }
        if (restartGraphicsSession &&
            !stopRequested_.load(std::memory_order_acquire) &&
            State() != VrRuntimeState::Faulted) {
            if (openXr_.IsSessionRunning()) {
                Fault(
                    "OpenXR session remained running after graphics rebuild "
                    "exit request");
                break;
            }
            if (everPoseReady) {
                log_.Write("[VR][pose] READY_INVALIDATED graphicsSessionRebuild=1");
            }
            ReleasePointerDrag();
            ResetThumbstickScroll("graphics-session-rebuild");
            stereoRenderMailbox_.Invalidate();
            confirmedMirrorLayout_.store(0, std::memory_order_release);
            stereoEyeWidth_.store(0, std::memory_order_release);
            stereoEyeHeight_.store(0, std::memory_order_release);
            if (!openXr_.ResetGraphicsSession(log_)) {
                lastOpenXrResult_.store(
                    openXr_.LastResult(), std::memory_order_release);
                Fault("unable to reset the stopped OpenXR graphics session");
                break;
            }
            capturedGeneration = 0;
            SetState(VrRuntimeState::WaitingForGraphics);
            continue;
        }
        break;
    }

}

void VrRuntime::PublishStereoTargetSpecChange() {
    const std::uint32_t width = openXr_.StereoEyeWidth();
    const std::uint32_t height = openXr_.StereoEyeHeight();
    if (width == 0 || height == 0 ||
        (width == stereoEyeWidth_.load(std::memory_order_acquire) &&
         height == stereoEyeHeight_.load(std::memory_order_acquire))) {
        return;
    }
    stereoRenderMailbox_.Invalidate();
    stereoEyeWidth_.store(width, std::memory_order_release);
    stereoEyeHeight_.store(height, std::memory_order_release);
    const std::uint64_t generation =
        stereoTargetGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    log_.Write(
        "[VR][stereo] TARGET_SPEC_UPDATED " + std::to_string(width) + "x" +
        std::to_string(height) + " generation=" + std::to_string(generation));
}

bool VrRuntime::EnsureD3D11Hooks() {
    if (d3d11HooksReady_) {
        return true;
    }

    const auto result = d3d11Capture_.InstallHooks(registrar_);
    switch (result) {
    case d3d11::D3D11Capture::HookInstallResult::Installed:
        d3d11HooksReady_ = true;
        log_.Write("[VR][runtime] D3D11 Present capture hooks installed");
        return true;
    case d3d11::D3D11Capture::HookInstallResult::AlreadyInstalled:
        d3d11HooksReady_ = true;
        return true;
    case d3d11::D3D11Capture::HookInstallResult::D3D11NotLoaded:
        SetState(VrRuntimeState::WaitingForGraphics);
        return false;
    case d3d11::D3D11Capture::HookInstallResult::InvalidRegistrar:
        Fault("invalid process hook registrar");
        return false;
    case d3d11::D3D11Capture::HookInstallResult::Failed:
    default:
        Fault("unable to install D3D11 capture hooks");
        return false;
    }
}

bool VrRuntime::WaitOrStop(std::chrono::milliseconds timeout) {
    std::unique_lock lock(waitMutex_);
    return stopChanged_.wait_for(lock, timeout, [&] {
        return stopRequested_.load(std::memory_order_acquire);
    });
}

void VrRuntime::SetState(VrRuntimeState state) {
    const VrRuntimeState previous = state_.exchange(state, std::memory_order_acq_rel);
    if (previous != state) {
        log_.Write(std::string("[VR][runtime] state=") + VrRuntimeStateName(state));
    }
}

void VrRuntime::Fault(std::string_view reason) {
    confirmedMirrorLayout_.store(0, std::memory_order_release);
    poseMailbox_.Invalidate();
    stereoRenderMailbox_.Invalidate();
    state_.store(VrRuntimeState::Faulted, std::memory_order_release);
    log_.Write(std::string("[VR][runtime] FAULT: ") + std::string(reason));
    WriteVrConsole(std::string("OpenXR fault: ") + std::string(reason));
}

std::filesystem::path VrRuntime::ResolveApplicationDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    path.resize(length);
    return std::filesystem::path(std::move(path)).parent_path();
}

VrRuntimeState VrRuntime::State() const noexcept {
    return state_.load(std::memory_order_acquire);
}

XrResult VrRuntime::LastOpenXrResult() const noexcept {
    return lastOpenXrResult_.load(std::memory_order_acquire);
}

std::filesystem::path VrRuntime::LogPath() const {
    return log_.Path();
}

bool VrRuntime::ReadLatestStereoPose(
    pose::StereoPoseSample& sample) const noexcept {
    return poseMailbox_.Read(sample);
}

VrRuntime::StereoRenderTargetSpec VrRuntime::ReadStereoRenderTargetSpec() const noexcept {
    StereoRenderTargetSpec spec;
    const std::uint64_t confirmedLayout =
        confirmedMirrorLayout_.load(std::memory_order_acquire);
    spec.eyeWidth = stereoEyeWidth_.load(std::memory_order_acquire);
    spec.eyeHeight = stereoEyeHeight_.load(std::memory_order_acquire);
    spec.generation = stereoTargetGeneration_.load(std::memory_order_acquire);
    spec.mirrorLayoutGeneration =
        ConfirmedMirrorLayoutGeneration(confirmedLayout);
    spec.landscape = ConfirmedMirrorLayoutIsLandscape(confirmedLayout);
    spec.enabled = config_.stereoProjectionEnabled && spec.eyeWidth != 0 &&
        spec.eyeHeight != 0 && confirmedLayout != 0 &&
        (!config_.stereoLandscapeOnly || spec.landscape) &&
        State() == VrRuntimeState::PoseReady;
    return spec;
}

bool VrRuntime::PublishUnityStereoFrame(
    ID3D11Texture2D* left,
    ID3D11Texture2D* right,
    const pose::StereoPoseSample& trackingSample,
    d3d11::StereoRenderMailbox::PublishDiagnostics* diagnostics) noexcept {
    if (!config_.stereoProjectionEnabled || State() != VrRuntimeState::PoseReady) {
        return false;
    }
    return stereoRenderMailbox_.Publish(left, right, trackingSample, diagnostics);
}

void VrRuntime::InvalidateUnityStereoFrame() noexcept {
    stereoRenderMailbox_.Invalidate();
}

bool VrRuntime::WriteVrLog(std::string_view message) noexcept {
    return log_.Write(message);
}

void VrRuntime::SetGripPanelTransparent(bool active) noexcept {
    gripPanelTransparent_.store(active, std::memory_order_release);
    d3d11Capture_.SetTransparentBackbufferClear(active);
}

void VrRuntime::SetGripPanelClearTargets(
    const void* const* textures, std::size_t count) noexcept {
    d3d11Capture_.SetTransparentClearTargets(textures, count);
}

bool VrRuntime::ConsumeLivePauseToggle() noexcept {
    return openXr_.ConsumeLivePauseToggle();
}

bool VrRuntime::ReadVrCameraInput(
    camera::VrCameraInputSample& sample) const noexcept {
    return cameraInputMailbox_.Read(sample);
}

const char* VrRuntimeStateName(VrRuntimeState state) noexcept {
    switch (state) {
    case VrRuntimeState::Disabled:
        return "Disabled";
    case VrRuntimeState::WaitingForRuntime:
        return "WaitingForRuntime";
    case VrRuntimeState::WaitingForGraphics:
        return "WaitingForGraphics";
    case VrRuntimeState::InstanceReady:
        return "InstanceReady";
    case VrRuntimeState::SystemReady:
        return "SystemReady";
    case VrRuntimeState::SessionReady:
        return "SessionReady";
    case VrRuntimeState::SessionRunning:
        return "SessionRunning";
    case VrRuntimeState::PoseReady:
        return "PoseReady";
    case VrRuntimeState::Faulted:
        return "Faulted";
    case VrRuntimeState::Stopped:
        return "Stopped";
    default:
        return "Unknown";
    }
}

} // namespace gakumas::vr
