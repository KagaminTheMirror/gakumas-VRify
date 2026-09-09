#pragma once

#include "camera/FovEquivalence.hpp"
#include "d3d11/SmaaT2xPass.hpp"
#include "d3d11/SmaaT2xJitter.hpp"
#include "d3d11/TscmaaPass.hpp"
#include "pose/RelativePoseBridge.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace gakumas::vr {

using Il2CppGCHandle = void*;
static_assert(sizeof(Il2CppGCHandle) == sizeof(void*));

struct UnityStereoCameraFrame {
    pose::StereoPoseSample trackingSample{};
    pose::StereoComposedPose composedPose{};
    // Cinemachine's own composed pose for this frame, sampled before the
    // headset delta is written back. The actor-shadow pass runs against this
    // so its projection keeps serving the authored shot under 6DoF.
    pose::Pose sourcePose{};
    bool sourcePoseValid = false;
    // Cinemachine `gameRequested` before the free-cam rig substitutes
    // `bridgeBase`. Volume evaluation and default-camera toon lock follow
    // this; `sourcePose` stays the rig / player position.
    pose::Pose cinematicPose{};
    bool cinematicPoseValid = false;
};

// Read-only snapshot taken at CinemachineBrain.PushStateToUnityCamera before
// the VR head-pose write. The hook resolves and validates the live CameraState
// and LensSettings layouts before filling this structure; no guessed field is
// consumed by the renderer.
struct UnitySourceCameraStateDiagnostic {
    std::uint64_t sample = 0;
    bool valid = false;
    bool hasLookAt = false;
    bool physical = false;
    float fieldOfViewDegrees = 0.0F;
    float focalLengthMillimeters = 0.0F;
    float sensorWidthMillimeters = 0.0F;
    float sensorHeightMillimeters = 0.0F;
    float lensShiftX = 0.0F;
    float lensShiftY = 0.0F;
    float rawPositionX = 0.0F;
    float rawPositionY = 0.0F;
    float rawPositionZ = 0.0F;
    float referenceLookAtX = 0.0F;
    float referenceLookAtY = 0.0F;
    float referenceLookAtZ = 0.0F;
    float lookAtDistance = 0.0F;
};

// One-shot, high-information ladder through Unity's ordinary top-level Camera
// queue. No recursive/manual render entry point is ever called:
//   A: one 256x256 camera, cullingMask=0
//   B: two 256x256 cameras, cullingMask=0
//   C: two full-size eye cameras with copied scene state, eye pose/projection
// Each stage occupies exactly one complete outer SRP context. End callbacks only
// record native state; GameObject mutation and texture publication happen after
// DoRenderLoop_Internal has returned.
class UnityStereoRenderer final {
public:
    UnityStereoRenderer() = default;
    ~UnityStereoRenderer() {
        DropPendingStereoGpuPublish();
        ResetAllNativeTextureLeases();
    }
    UnityStereoRenderer(const UnityStereoRenderer&) = delete;
    UnityStereoRenderer& operator=(const UnityStereoRenderer&) = delete;

    friend bool HasPendingStereoGpuPublish() noexcept;
    friend bool ConsumePendingStereoGpuPublish() noexcept;
    friend void DropPendingStereoGpuPublish() noexcept;

    void Tick(
        void* sourceCamera,
        const UnityStereoCameraFrame& frame,
        bool pipelineIdle) noexcept;
    void ObserveSourceCameraStateForDiagnostics(
        const UnitySourceCameraStateDiagnostic& sample) noexcept;
    void OnBeginContext() noexcept;
    void OnBeginCamera(void* camera) noexcept;
    [[nodiscard]] bool OnEndCamera(void* camera) noexcept;
    void OnEndContext(bool pipelineIdle) noexcept;
    void OnRenderLoopCompleted() noexcept;
    // Called only after the verified PostProcessPass original has returned.
    // The source RTHandle is shared by both eyes, so record a Unity
    // CommandBuffer copy into a distinct per-eye RenderTexture while the
    // ScriptableRenderContext still preserves pass order. Native access waits
    // until the complete SRP render loop has returned and submitted the queue.
    [[nodiscard]] bool QueueSmaaT2xMotionVectorCopy(
        void* camera,
        void* renderContext,
        void* renderTexture,
        int renderPassEvent) noexcept;
    // MotionVectorsPersistentData.Update receives CameraData by value. The
    // live-resolved detour passes its local m_ProjectionMatrix here before the
    // original call so only motion history sees the unjittered HMD matrix.
    // `required` distinguishes a non-T2x/non-eye call from a failed correction;
    // no-jitter TSCMAA never requests this projection rewrite.
    [[nodiscard]] bool CorrectSmaaT2xMotionHistoryProjection(
        void* camera,
        float* cameraDataProjection,
        bool* required) noexcept;
    void Release(bool pipelineIdle) noexcept;
    [[nodiscard]] bool ShouldSuppressDepthOfField(
        void* volumeComponent,
        bool originalActive) noexcept;
    // Bracket the VL/Campus passes that derive an actor light direction from
    // the rendering camera: ActorShadowPass (projected actor shadow map) and
    // the actor-parameter passes (MatCap main light, which is what shades hair
    // and accessories onto the body). Pointing the camera transform at the
    // pre-HMD Cinemachine pose keeps the original algorithm but anchors the
    // result to the authored shot. Begin returns true only when End must
    // restore the camera; nested calls are no-ops so passes may overlap.
    [[nodiscard]] bool BeginActorShadowSourceAnchor(const char* passName) noexcept;
    void EndActorShadowSourceAnchor() noexcept;
    // Forensics for the head-following actor lighting: sample the shader
    // globals those passes write, next to the head and shot forwards. If the
    // vectors stay constant while the shading visibly swings, the direction is
    // authored in view space and the swing comes from the shader's view
    // matrix, which no camera-transform bracket can fix.
    void RecordActorLightDiagnostics(const pose::Pose& headPose) noexcept;
    // Volume-trigger source anchor (`.229` / `.266`): keep a proxy
    // transform at the authored Cinemachine pose (`cinematicPose`) and
    // point the source camera's volumeTrigger at it. Never follows the
    // free-cam rig or headset. Called once per Tick on the owner thread;
    // restores the saved trigger when the config toggle is off or the
    // cinematic pose is unavailable.
    void UpdateVolumeTriggerAnchor(
        void* sourceCamera, const UnityStereoCameraFrame& frame) noexcept;
    void RestoreVolumeTriggerAnchor(const char* reason) noexcept;
    [[nodiscard]] bool EnsureVolumeTriggerProxy() noexcept;
    [[nodiscard]] pose::Pose ReadCurrentCameraPoseForDiagnostics() noexcept;
    // Resolves Shader.GetGlobalVector/Matrix and the passes' cached
    // Shader.PropertyToID static ints, rate-limited to one attempt per
    // second. Decoupled from diagnostics: the .89 run proved the diagnostic
    // "30 qualifying campus/source calls" trigger can starve for an entire
    // session (the pass only runs per-frame in some scenes), which left the
    // compensation gated on an ID that never resolved. Returns true once
    // the MatCap main-light ID is known.
    [[nodiscard]] bool EnsureActorLightShaderIds() noexcept;
    // After CampusActorParameterPass / VLActorParameterPass: rim-only
    // light-vector compensate (accepted `.130`) and optional
    // `_MatCapParam.x` bias. The UnityPerCamera cam-pos lock is *not*
    // applied here — both parameter passes are BeforeRendering, and
    // URP SetupCameraProperties runs after them (`.139`–`.142`).
    void ApplyActorMatcapCompensation(
        void* renderContext, void* renderingData) noexcept;
    // MaterialInfo construction can occur before the renderer owner thread is
    // established. Queue only the proven Material pointer here; the owned
    // left-eye RenderActor callback validates and consumes it.
    void QueueActorOutlineMaterial(void* material) noexcept;
    // Called from the proven VLDeferredPass.RenderActor entry immediately
    // before actor GBuffer draws. The pass/culling data has no managed
    // Renderer list, so the first left-eye actor draw consumes at most one
    // deferred scene scan and applies the cached materials before the actor is
    // drawn. Waiting through the source pass gives late actors the last safe
    // opportunity to appear without polling FindObjects.
    void PrepareActorOutlineMaterialsForCurrentDraw() noexcept;
    // `.179` official outline capture at the proven
    // CampusActorParameterPass.Execute entry. Rebuilds the official complete
    // `_OutlineParam` for the current camera from the live Campus settings
    // (`_settings` ranges + `outlineFocalLengthScale` AnimationCurve). The
    // `.176` hardware run proved this reconstruction equals the vector the
    // official writer publishes for every sampled camera, including the mod
    // eye cameras (non-physical: focal derived from FOV through the sensor
    // gate). Read-only: never writes rendering state.
    void CaptureOfficialOutlineVector(void* passInstance) noexcept;
    // Eyes-only matcap-frame lock at `VLDeferredPass.RenderActor`
    // (after official setupCamera, immediately before actor GBuffer
    // draws). Begin records SetGlobalVector(_WorldSpaceCameraPos, shot)
    // plus SetGlobalMatrix(unity_WorldToCamera / unity_CameraToWorld,
    // shot view) — the same calls URP's SetCameraMatrices uses to publish
    // them — and executes on the context so queue order lands them right
    // before the actor draws; End puts the eye values back after the
    // draw. No transform poke, no Setup, no VP writes (`.143` showed both
    // are deferred/overwritten). Does not patch cameraData.m_ViewMatrix.
    [[nodiscard]] bool BeginActorMatcapCamPosLock(
        void* renderContext, void* renderingData) noexcept;
    void EndActorMatcapCamPosLock(
        void* renderContext, void* renderingData) noexcept;
    void NoteToonFollowIndex(int index) noexcept;
    void NoteToonActorSample(
        int index,
        const pose::Vector3& position,
        const pose::Vector3& forward) noexcept;
    // Projected actor shadow (.92–.95): _WorldToActorShadow's rotation
    // follows the head even with directionalType=Fixed, and the transform
    // swap never affected it — UpdateShadowData reads cameraData's cached
    // matrices, not the camera transform, and it runs from
    // DrawActorShadowPass.AddRenderPasses (Setup is LTCG-inlined into that
    // body). These patch cameraData's m_ViewMatrix (and worldSpaceCameraPos)
    // to the authored shot pose around AddRenderPasses, then restore. Only
    // active inside the anchor bracket (same menu toggle, same pose validity).
    [[nodiscard]] bool BeginActorShadowViewPatch(void* renderingData) noexcept;
    void EndActorShadowViewPatch(void* renderingData) noexcept;
    // Ungated forensic heartbeat, called straight from the campus-pass hook
    // (rate-limited there): dumps every shadow-path counter and deadline in
    // one line. The .90 run left an impossible pattern behind (compensation
    // counted 10800 executions while the census in the same call path
    // recorded ~1% of the expected calls and every periodic log line went
    // missing); this measures instead of guessing.
    void LogShadowHeartbeat() noexcept;
    [[nodiscard]] void* CurrentCamera() const noexcept;
    [[nodiscard]] const char* ClassifyCamera(void* camera) const noexcept;
    [[nodiscard]] bool SourceCameraSuppressed() const noexcept {
        return sourceCameraSuppressed_;
    }
    // True while the Grip panel should composite as transparent UI: the
    // vrGripPanelTransparent toggle is on and the source camera is actually
    // unused (tiny mode applied or fully suppressed). Published by the game
    // thread; read cross-thread (VR worker / Present hook bridge).
    [[nodiscard]] bool GripTransparencyArmed() const noexcept {
        return gripTransparencyArmed_.load(std::memory_order_acquire);
    }
    // The VR queue cameras must never become the tracked "main camera"
    // (.114: native Camera.main returned the tagged left eye, the tracker
    // selected it, and Tick — gated on that selection — starved).
    [[nodiscard]] bool IsEyeCamera(const void* camera) const noexcept {
        return camera != nullptr &&
            (camera == eyeCameras_[0] || camera == eyeCameras_[1]);
    }
    // Returns the exact source-NDC -> eye-NDC tangent-plane size conversion
    // captured for this eye and the current tracked pose.  Callers must leave
    // authored data untouched when this fails; there is deliberately no fixed
    // FOV fallback.
    [[nodiscard]] bool TryGetEyeProjectionScale(
        const void* camera,
        float* scaleX,
        float* scaleY,
        std::size_t* eye) const noexcept;
    // Enables the per-camera replacement only after the live layout validates
    // and both detours have installed. If it is false, shared authored state is
    // deliberately left untouched so a failed hardware probe remains visible.
    void SetProjectionEquivalentProFlareReady(bool ready) noexcept {
        projectionEquivalentProFlareReady_.store(
            ready, std::memory_order_release);
    }
    [[nodiscard]] bool ProjectionEquivalentProFlareReady() const noexcept {
        return projectionEquivalentProFlareReady_.load(
            std::memory_order_acquire);
    }
    // While the source is suppressed and vrEyeAsMainCamera is on, return
    // the left eye only (managed get_main). Native tags are never written
    // (.114 Cinemachine origin / flat fallback). Never swap to the right
    // eye mid-frame.
    [[nodiscard]] void* MainCameraOverride() const noexcept;

    struct MethodRef {
        void* function = nullptr;
        void* methodInfo = nullptr;

        [[nodiscard]] bool Ready() const noexcept {
            return function != nullptr && methodInfo != nullptr;
        }

        [[nodiscard]] bool HasInfo() const noexcept {
            return methodInfo != nullptr;
        }
    };

    [[nodiscard]] bool ReadShaderGlobal(
        const MethodRef& getter,
        std::int32_t nameId,
        float* out,
        std::size_t count) noexcept;

private:
    enum class LadderStage : std::uint8_t {
        AdmissionOne,
        AdmissionTwo,
        StereoFull,
        Complete,
        Failed,
    };

    enum class LifetimeWriteCategory : std::uint8_t {
        EyeGameObject,
        EyeCamera,
        SourceCamera,
        TargetTexture,
        MainCameraTag,
        RenderTarget,
        OutlineMaterial,
        ProFlare,
        UiTextureOverlay,
        LiveCameraOverlay,
        CmovParticle,
        Count,
    };

    struct LifetimeWriteRecord {
        std::uint64_t serial = 0;
        std::uint64_t sceneReadyEpoch = 0;
        const char* action = "-";
        void* object = nullptr;
        void* related = nullptr;
        std::intptr_t value = 0;
    };

    struct ManagedApi {
        MethodRef internalCreateGameObject{};
        MethodRef gameObjectSetActive{};
        MethodRef gameObjectGetActiveSelf{};
        MethodRef gameObjectGetLayer{};
        MethodRef gameObjectSetTag{};
        MethodRef gameObjectCompareTag{};
        MethodRef componentGetGameObject{};
        MethodRef addComponent{};
        MethodRef dontDestroyOnLoad{};
        MethodRef cameraCopyFrom{};
        MethodRef cameraSetTargetTexture{};
        MethodRef cameraGetTargetTexture{};
        MethodRef cameraSetCullingMask{};
        MethodRef cameraGetCullingMask{};
        MethodRef cameraResetWorldToCameraMatrix{};
        MethodRef cameraSetNonJitteredProjectionMatrixInjected{};
        MethodRef cameraGetDepth{};
        MethodRef cameraSetDepth{};
        MethodRef cameraGetNearClipPlane{};
        MethodRef cameraSetNearClipPlane{};
        MethodRef cameraGetFarClipPlane{};
        MethodRef cameraSetFarClipPlane{};
        MethodRef cameraGetAllowHdr{};
        MethodRef cameraGetAllowMsaa{};
        MethodRef cameraGetDepthTextureMode{};
        MethodRef cameraSetDepthTextureMode{};
        MethodRef cameraGetFieldOfView{};
        MethodRef cameraSetFieldOfView{};
        MethodRef cameraGetAspect{};
        MethodRef cameraSetAspect{};
        MethodRef cameraGetUsePhysicalProperties{};
        MethodRef cameraGetApertureInjected{};
        MethodRef cameraGetFocusDistanceInjected{};
        MethodRef cameraGetFocalLengthInjected{};
        MethodRef cameraSetFocalLengthInjected{};
        MethodRef cameraGetSensorSizeInjected{};
        MethodRef cameraSetSensorSizeInjected{};
        MethodRef cameraGetGateFittedFieldOfViewInjected{};
        MethodRef cameraGetGateFittedLensShiftInjected{};
        MethodRef cameraGetLensShiftInjected{};
        MethodRef cameraSetLensShiftInjected{};
        MethodRef cameraGetProjectionMatrixInjected{};
        MethodRef cameraGetNonJitteredProjectionMatrixInjected{};
        MethodRef cameraSetProjectionMatrixInjected{};
        MethodRef behaviourSetEnabled{};
        MethodRef behaviourGetEnabled{};
        MethodRef componentGetComponent{};
        MethodRef componentGetTransform{};
        MethodRef gameObjectGetTransform{};
        MethodRef transformGetParent{};
        MethodRef transformSetPositionAndRotationInjected{};
        MethodRef transformGetPositionInjected{};
        MethodRef transformGetRotationInjected{};
        MethodRef matrixFrustumInjected{};
        MethodRef renderTextureConstructor{};
        MethodRef renderTextureDescriptorConstructor{};
        MethodRef renderTextureCreate{};
        MethodRef renderTextureRelease{};
        MethodRef renderTextureGetDescriptor{};
        MethodRef renderTextureGetSrgb{};
        MethodRef renderTextureGetGraphicsFormat{};
        MethodRef textureGetNativeTexturePtr{};
        void* commandBufferClass = nullptr;
        MethodRef commandBufferConstructor{};
        MethodRef commandBufferClear{};
        MethodRef commandBufferCopyTexture{};
        MethodRef renderTargetIdentifierConstructor{};
        MethodRef scriptableRenderContextExecuteCommandBuffer{};
        MethodRef universalSetRenderer{};
        MethodRef universalGetRenderType{};
        MethodRef universalSetRenderType{};
        MethodRef universalGetRenderPostProcessing{};
        MethodRef universalSetRenderPostProcessing{};
        MethodRef universalGetAntialiasing{};
        MethodRef universalSetAntialiasing{};
        MethodRef universalGetAntialiasingQuality{};
        MethodRef universalSetAntialiasingQuality{};
        MethodRef universalGetRequiresDepthOption{};
        MethodRef universalSetRequiresDepthOption{};
        MethodRef universalGetRequiresColorOption{};
        MethodRef universalSetRequiresColorOption{};
        MethodRef universalGetRequiresDepthTexture{};
        MethodRef universalGetRequiresColorTexture{};
        MethodRef universalGetVolumeLayerMask{};
        MethodRef universalSetVolumeLayerMask{};
        MethodRef universalGetVolumeTrigger{};
        MethodRef universalSetVolumeTrigger{};
        MethodRef universalGetVolumeFrameworkUpdateMode{};
        MethodRef universalSetVolumeFrameworkUpdateMode{};
        MethodRef universalGetRequiresVolumeFrameworkUpdate{};
        MethodRef universalGetVolumeStack{};
        MethodRef universalGetOrCreateVolumeStack{};
        MethodRef universalGetTaaPersistentData{};
        MethodRef universalGetMotionVectorsPersistentData{};
        MethodRef universalGetResetHistory{};
        MethodRef cameraExtensionsUpdateVolumeStack{};
        MethodRef universalGetStopNan{};
        MethodRef universalSetStopNan{};
        MethodRef universalGetDithering{};
        MethodRef universalSetDithering{};
        MethodRef universalGetAllowHdrOutput{};
        MethodRef universalSetAllowHdrOutput{};
        MethodRef universalSetResetHistory{};
        MethodRef universalGetRenderShadows{};
        MethodRef universalSetRenderShadows{};
        MethodRef universalSetAllowXrRendering{};
        MethodRef universalGetFastRendering{};
        MethodRef universalSetFastRendering{};
        MethodRef universalGetNeedsAlphaChannel{};
        MethodRef universalSetNeedsAlphaChannel{};
        MethodRef vlAdditionalSetTargetTexture{};
        MethodRef vlControllerGetTargetTextureDescriptor{};
        MethodRef vlControllerGetHdrTargetTextureDescriptor{};
        MethodRef rendererGetSharedMaterials{};
        MethodRef materialGetShader{};
        MethodRef materialFindPass{};
        MethodRef materialHasProperty{};
        MethodRef materialGetVectorInjected{};
        MethodRef materialSetVectorInjected{};
        MethodRef shaderGetPropertyCount{};
        MethodRef shaderGetPropertyName{};
        MethodRef volumeStackGetComponent{};
        MethodRef volumeComponentSetActive{};
        bool lensShiftGetterUsesNativeSelf = false;
        bool lensShiftSetterUsesNativeSelf = false;
        bool projectionGetterUsesNativeSelf = false;
        bool nonJitteredProjectionGetterUsesNativeSelf = false;
        bool projectionUsesNativeSelf = false;
        bool nonJitteredProjectionUsesNativeSelf = false;
        bool transformUsesNativeSelf = false;
        bool transformPositionGetterUsesNativeSelf = false;
        bool transformRotationGetterUsesNativeSelf = false;
        void* gameObjectClass = nullptr;
        void* renderTextureClass = nullptr;
        void* renderTextureDescriptorClass = nullptr;
        void* cameraReflectionType = nullptr;
        void* universalCameraDataReflectionType = nullptr;
        void* vlAdditionalCameraDataReflectionType = nullptr;
        void* vlCameraControllerReflectionType = nullptr;
        void* vlDofReflectionType = nullptr;
        void* urpDofReflectionType = nullptr;
        void* vlBloomReflectionType = nullptr;
        void* urpBloomReflectionType = nullptr;
        void* chromaticAberrationReflectionType = nullptr;
        void* lensDistortionReflectionType = nullptr;
        void* motionBlurReflectionType = nullptr;
        void* vignetteReflectionType = nullptr;
        std::int32_t volumeComponentActiveOffset = -1;
        std::int32_t volumeParameterOverrideOffset = -1;
        std::int32_t volumeParameterFloatValueOffset = -1;
        std::int32_t volumeParameterIntValueOffset = -1;
        std::int32_t vlDofMaxBlurSpreadFieldOffset = -1;
        std::int32_t vlDofForegroundBlurFieldOffset = -1;
        std::int32_t vlDofQualityFieldOffset = -1;
        std::int32_t urpDofModeFieldOffset = -1;
        std::int32_t vlBloomIntensityFieldOffset = -1;
        std::int32_t vlBloomScatterFieldOffset = -1;
        std::int32_t vlBloomDiffusionFieldOffset = -1;
        std::int32_t urpBloomIntensityFieldOffset = -1;
        std::int32_t urpBloomScatterFieldOffset = -1;
        std::int32_t urpBloomDirtIntensityFieldOffset = -1;
        std::int32_t chromaticAberrationIntensityFieldOffset = -1;
        std::int32_t lensDistortionIntensityFieldOffset = -1;
        std::int32_t motionBlurIntensityFieldOffset = -1;
        std::int32_t vignetteIntensityFieldOffset = -1;
        std::int32_t vignetteSmoothnessFieldOffset = -1;
        void* lensFlareClass = nullptr;
        std::int32_t lensFlareScaleOffset = -1;
        std::int32_t lensFlareIntensityOffset = -1;
        void* proFlareClass = nullptr;
        std::int32_t proFlareGlobalScaleOffset = -1;
        std::int32_t proFlareGlobalBrightnessOffset = -1;
        std::int32_t proFlareDynamicEdgeBoostOffset = -1;
        std::int32_t proFlareDynamicCenterBoostOffset = -1;
        void* uiTextureOverlayClass = nullptr;
        std::int32_t uiTextureOverlayShaderOffset = -1;
        std::int32_t uiTextureOverlayTextureOffset = -1;
        std::int32_t uiTextureOverlaySpriteOffset = -1;
        std::int32_t uiTextureOverlayColorOffset = -1;
        std::int32_t uiTextureOverlayClampUvOffset = -1;
        std::int32_t uiTextureOverlayAlphaMaskOffset = -1;
        std::int32_t uiTextureOverlayModeOffset = -1;
        std::int32_t uiTextureOverlayMaterialOffset = -1;
        void* liveCameraOverlayClass = nullptr;
        std::int32_t liveCameraOverlayOffsetFieldOffset = -1;
        std::int32_t liveCameraOverlayScalerFieldOffset = -1;
        void* particleSystemClass = nullptr;
        std::int32_t universalRendererIndexOffset = -1;
        std::int32_t universalTaaSettingsOffset = -1;
        std::int32_t universalClearDepthOffset = -1;
        std::int32_t universalUseScreenCoordOverrideOffset = -1;
        std::int32_t universalScreenSizeOverrideOffset = -1;
        std::int32_t universalScreenCoordScaleBiasOffset = -1;
        std::int32_t universalRenderScaleOffset = -1;
        std::int32_t renderTextureDescriptorValueSize = -1;
    };

    struct TargetSpecSnapshot {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint64_t generation = 0;
    };

    [[nodiscard]] bool EnsureManagedApi() noexcept;
    [[nodiscard]] bool EnsureCamera(std::size_t eye) noexcept;
    [[nodiscard]] bool EnsureCameraDataComponents(std::size_t eye) noexcept;
    [[nodiscard]] bool EnsureCameraDataFieldOffsets() noexcept;
    [[nodiscard]] bool UploadMatcapCamPos(
        void* renderContext,
        void* renderingData,
        bool useShotPosition,
        pose::Vector3* shotOut,
        float* eyeCamPosOut,
        bool* haveEyeCamPosOut,
        bool* matsOut,
        bool* vpOut,
        const char** skipOut) noexcept;
    [[nodiscard]] bool IsMatcapFrameVpSwapEligible() noexcept;
    [[nodiscard]] bool TryCurrentToonLightingPose(pose::Pose& out) noexcept;
    void RefreshToonLightingState() noexcept;
    [[nodiscard]] bool TrySelectToonActor(
        const pose::Vector3& reference,
        bool firstPerson,
        pose::Vector3& position,
        pose::Vector3& forward,
        bool& frontOnly) const noexcept;
    [[nodiscard]] bool InvokeContextSetupCameraProperties(
        void* renderContext) noexcept;
    [[nodiscard]] bool EnsureAdmissionTarget(std::size_t eye) noexcept;
    [[nodiscard]] bool EnsureFullTarget(
        std::size_t eye,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t generation) noexcept;
    [[nodiscard]] bool CreateRenderTarget(
        std::size_t eye,
        std::uint32_t width,
        std::uint32_t height,
        const char* family,
        void* descriptorSource,
        void** target,
        Il2CppGCHandle* handle,
        bool fatal = true) noexcept;
    // Live render-scale changes resize the eye buffers. Drop the old pair at a
    // pipeline-idle safe point so the next arm allocates at the new size.
    [[nodiscard]] bool RetireFullTargets(
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t generation) noexcept;
    [[nodiscard]] bool ArmCurrentStage(
        void* sourceCamera,
        const UnityStereoCameraFrame& frame,
        const TargetSpecSnapshot& targetSpec) noexcept;
    [[nodiscard]] bool ConfigureCamera(
        std::size_t eye,
        void* sourceCamera,
        void* target,
        bool sceneEnabled,
        const UnityStereoCameraFrame& frame,
        float sourceDepth) noexcept;
    [[nodiscard]] bool ConfigureCameraData(
        std::size_t eye,
        void* sourceCamera,
        void* target) noexcept;
    void LogSmaaT2xProjectionReadback(
        std::size_t eye,
        const char* stage) noexcept;
    [[nodiscard]] bool CaptureSmaaT2xMotionVectorTexture(
        std::size_t eye,
        ID3D11Texture2D* texture,
        std::uint64_t pairToken,
        int renderPassEvent,
        const char* boundary) noexcept;
    [[nodiscard]] bool CaptureTscmaaMotionVectorTexture(
        std::size_t eye,
        ID3D11Texture2D* texture,
        std::uint64_t pairToken,
        int renderPassEvent,
        const char* boundary) noexcept;
    void BindEyeDepthOfField(std::size_t eye, void* sourceVolumeStack) noexcept;
    void BindEyeUrpDepthOfField(std::size_t eye, void* sourceVolumeStack) noexcept;
    void BindEyeVolumeComponent(
        std::size_t eye,
        void* sourceVolumeStack,
        void* reflectionType,
        std::array<void*, 2>& slots,
        std::array<bool, 2>& logged,
        const char* armedTag,
        const char* skippedTag) noexcept;
    void DeactivateVolumeComponent(void* component, const char* label) noexcept;
    void NeutralizeDepthOfFieldParameters(void* component, const char* label) noexcept;
    void CaptureSourceLens(void* sourceCamera) noexcept;
    void ClearEyeBloomOverrides(std::size_t eye) noexcept;
    void CacheAuthoredBloom(std::size_t eye) noexcept;
    [[nodiscard]] bool TryResolveBloomProjectionScale(
        std::size_t eye,
        float* scale,
        const char** source) const noexcept;
    void ApplyModeBodyBloom(std::size_t eye) noexcept;
    void ApplyModeBodyLensFlareScales() noexcept;
    void DiscoverProFlares() noexcept;
    void WriteProFlareScalesForEyes() noexcept;
    void WriteProFlareScalesForGrip() noexcept;
    void RestoreProFlareScales() noexcept;
    void DropProFlareCache(const char* reason) noexcept;
    void RestoreLensFlareScales() noexcept;
    void DiscoverUiTextureOverlays() noexcept;
    void HideUiTextureOverlaysForEyes() noexcept;
    void RestoreUiTextureOverlays(const char* reason) noexcept;
    void DropUiTextureOverlayCache(const char* reason) noexcept;
    void DiscoverLiveCameraOverlays() noexcept;
    void HideLiveCameraOverlaysForEyes() noexcept;
    void RestoreLiveCameraOverlays(const char* reason) noexcept;
    void DropLiveCameraOverlayCache(const char* reason) noexcept;
    void DiscoverCmovParticles() noexcept;
    void HideCmovParticlesForEyes() noexcept;
    void RestoreCmovParticles(const char* reason) noexcept;
    void DropCmovParticleCache(const char* reason) noexcept;
    void EnsureOutlineMaterialApi() noexcept;
    void EnsureSceneManagerApi() noexcept;
    void RecordLifetimeWrite(
        LifetimeWriteCategory category,
        const char* action,
        void* object,
        void* related = nullptr,
        std::intptr_t value = 0) noexcept;
    void LogLifetimeSnapshot(const char* phase, const char* where) noexcept;
    void ObserveLifetimeSceneReadyEdges(const char* where) noexcept;
    void RefreshSceneIdentity(const char* where) noexcept;
    void RequestVirtualCameraCensus(const char* reason) noexcept;
    void RunVirtualCameraCensusIfDue() noexcept;
    void RunVirtualCameraCensus(const char* phase) noexcept;
    void RequestHeavyDiscover() noexcept;
    void RequestActorOutlineDiscover(const char* reason) noexcept;
    void HoldEyeArm(const char* reason) noexcept;
    void ReleaseEyeArm(const char* reason) noexcept;
    [[nodiscard]] bool SceneContentUnstable() const noexcept;
    [[nodiscard]] bool EyeArmHeld() const noexcept;
    void DiscoverActorOutlineMaterials() noexcept;
    void CaptureQueuedActorOutlineMaterials() noexcept;
    void WriteOutlineMaterialsForEyes() noexcept;
    void WriteOutlineMaterialsForGrip() noexcept;
    void DropOutlineMaterialCache(const char* reason) noexcept;
    void RestoreOutlineMaterials() noexcept;
    void DumpActorOutlineMaterials() noexcept;
    void DeactivateBoundEyeDepthOfField() noexcept;
    [[nodiscard]] void* GetVolumeComponent(
        void* stack,
        void* reflectionType) noexcept;
    [[nodiscard]] bool ApplyEyePoseAndProjection(
        std::size_t eye,
        const UnityStereoCameraFrame& frame,
        float nearClip,
        float farClip) noexcept;
    [[nodiscard]] bool SetGameObjectActive(std::size_t eye, bool active) noexcept;
    [[nodiscard]] bool SetCameraEnabled(std::size_t eye, bool enabled) noexcept;
    [[nodiscard]] bool ReadCameraEnabled(void* camera, bool* enabled) noexcept;
    [[nodiscard]] bool WriteSourceCameraEnabled(void* camera, bool enabled) noexcept;
    void RestoreSourceCamera(const char* reason) noexcept;
    void SuppressSourceCamera(void* sourceCamera) noexcept;
    void SyncSourceCameraSuppression(void* sourceCamera) noexcept;
    void PublishGripTransparency() noexcept;
    void SyncGripUiPassClear(bool armed) noexcept;
    [[nodiscard]] bool ResolveGripUiPassApi() noexcept;
    void ApplyGripUiPassClear() noexcept;
    void PublishGripIntermediateClearTargets() noexcept;

public:
    // .240: the black/white/gray screens share one painter — the UI
    // camera's pooled URP intermediate (never cleared, not a managed
    // RenderTexture, unreachable by enumeration). On the DrawFrameBuffer
    // branch every pass with _passIndex>0 blits that stale intermediate
    // over the framebuffer; on the native branch the UI is drawn into it.
    // While armed, the Execute hook wraps each pass: force the direct
    // framebuffer path, keep pass0's transparent clear, and strip the
    // blit from later passes (_passIndex=0 skips it per the .225 disasm);
    // all fields are restored after the original call returns.
    struct GripUiPassExecState {
        bool modified = false;
        std::int32_t passIndex = 0;
        bool needsClear = false;
        float clearColor[4]{};
        bool drawFrameBuffer = false;
    };
    [[nodiscard]] bool GripUiPassExecuteEnter(
        void* pass, GripUiPassExecState& saved) noexcept;
    void GripUiPassExecuteExit(
        void* pass, const GripUiPassExecState& saved) noexcept;
    // .346: .345 is accepted in produce but regresses lobby decorations.
    // Published by the existing live HomePage visibility census. Unknown and
    // disarmed states retain authored blur; no framebuffer policy changes.

private:
    void ApplySourceTinyMode(void* sourceCamera) noexcept;
    void LiftSourceTinyMode(const char* reason) noexcept;
    void SyncEyeAsMainCameraTag() noexcept;
    void ClearSourceTemporalReset(void* sourceUniversalData) noexcept;
    void RequestOneFrameSourceTemporalReset(void* sourceUniversalData) noexcept;
    [[nodiscard]] bool ReadSourceCutSignature(
        void* sourceCamera,
        float* fieldOfView,
        float* posX,
        float* posY,
        float* posZ) noexcept;
    [[nodiscard]] bool ConsumeSourceTemporalCut(
        void* sourceCamera,
        void* sourceUniversalData,
        bool sourceResetHistory) noexcept;
    void QueueStageDecision(bool passed, const char* reason) noexcept;
    void ApplySceneIneligible() noexcept;
    void MaybeLogPortraitArmed(void* sourceCamera) noexcept;
    void ConsumeStageDecisionAtSafePoint() noexcept;
    void TryPublishStereoAtSafePoint() noexcept;
    struct NativeTextureLease final {
        void* managed = nullptr;
        void* nativePointer = nullptr;
        ID3D11Texture2D* texture = nullptr;
        std::uint64_t generation = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        std::uintptr_t device = 0;
        std::uint64_t hitCount = 0;
        bool srgbKnown = false;
        bool srgb = false;

        void Reset() noexcept;
        [[nodiscard]] bool Matches(
            void* managedObject, std::uint64_t resourceGeneration) const noexcept;
    };
    void ResetNativeTextureLease(NativeTextureLease& lease) noexcept;
    void ResetAllNativeTextureLeases() noexcept;
    [[nodiscard]] bool BindNativeTextureLease(
        void* managed,
        std::uint64_t generation,
        NativeTextureLease& lease,
        const char* role,
        std::size_t eye,
        void** nativePointer,
        ID3D11Texture2D** texture) noexcept;
    [[nodiscard]] bool ReadLeaseSrgb(
        void* managed, NativeTextureLease& lease, bool* srgb) noexcept;
    enum class StereoGpuPublishMode : std::uint8_t {
        MailboxOnly,
        SmaaT2x,
        Tscmaa,
    };
    struct PendingStereoGpuPublish final {
        std::array<ID3D11Texture2D*, 2> colors{};
        std::array<ID3D11Texture2D*, 2> motionSources{};
        std::array<int, 2> motionEvents{};
        std::array<void*, 2> nativePointers{};
        pose::StereoPoseSample tracking{};
        StereoGpuPublishMode mode = StereoGpuPublishMode::MailboxOnly;
        bool srgb = false;
        bool resolveTemporal = false;
        bool comprehensiveProbe = false;
        int quality = 0;
        std::uint32_t phase = 0;
        std::uint64_t token = 0;
        bool valid = false;

        void Reset() noexcept;
        [[nodiscard]] bool HasWork() const noexcept;
    };
    [[nodiscard]] bool QueueStereoGpuPublish(
        const std::array<ID3D11Texture2D*, 2>& colors,
        const std::array<void*, 2>& nativePointers,
        StereoGpuPublishMode mode,
        bool srgb,
        bool resolveTemporal) noexcept;
    [[nodiscard]] bool HasPendingStereoGpuPublish() const noexcept;
    [[nodiscard]] bool ConsumePendingStereoGpuPublish() noexcept;
    void DropPendingStereoGpuPublish() noexcept;
    void ApplyPendingGpuFailure() noexcept;
    // Legacy method/field names are retained because the GPU-ordered copy
    // bridge was proven by SMAA T2x; both native temporal modes share it while
    // feeding distinct pass-owned immutable snapshots and histories.
    [[nodiscard]] bool EnsureSmaaT2xMotionCopyTarget(
        std::size_t eye, void* source) noexcept;
    [[nodiscard]] bool EnsureSmaaT2xMotionCopyCommandBuffer() noexcept;
    [[nodiscard]] bool BindQueuedSmaaT2xMotionVectors() noexcept;
    void RetireSmaaT2xMotionCopyTargets(const char* reason) noexcept;
    void ResetTemporalHistories(const char* reason) noexcept;
    void FallBackSmaaT2x(const char* reason) noexcept;
    void FallBackTscmaa(const char* reason) noexcept;
    void AdvanceStage(bool passed) noexcept;
    [[nodiscard]] bool FailStage(const char* stage, std::size_t eye = 2U) noexcept;
    void Log(std::string_view message) const noexcept;

    [[nodiscard]] static const char* StageName(LadderStage stage) noexcept;
    [[nodiscard]] static const char* LifetimeWriteCategoryName(
        LifetimeWriteCategory category) noexcept;
    [[nodiscard]] std::uint8_t ExpectedMask() const noexcept;
    [[nodiscard]] std::size_t ExpectedCameraCount() const noexcept;
    [[nodiscard]] bool IsOwnerThread() noexcept;

    static constexpr std::uint32_t kAdmissionTargetWidth = 256U;
    static constexpr std::uint32_t kAdmissionTargetHeight = 256U;

    ManagedApi api_{};
    bool apiInitialized_ = false;
    std::atomic_bool releaseRequested_{false};
    LadderStage stage_ = LadderStage::AdmissionOne;
    bool stageArmed_ = false;
    bool contextActive_ = false;
    bool decisionPending_ = false;
    bool decisionPassed_ = false;
    const char* decisionReason_ = nullptr;
    bool publishPending_ = false;
    bool beginLogged_ = false;
    bool continuousStereo_ = false;
    bool ownerThreadMismatchLogged_ = false;
    bool renderLoopBoundaryLogged_ = false;
    bool ownedEyeOutsideContextLogged_ = false;
    bool verboseFrameLog_ = true;
    std::array<std::uint64_t, 2> sourceFingerprints_{};
    std::array<bool, 2> sourceFingerprintValid_{};
    std::array<bool, 2> gameObjectActive_{};
    std::array<bool, 2> cameraEnabled_{};
    std::array<void*, 2> eyeGameObjects_{};
    std::array<Il2CppGCHandle, 2> eyeGameObjectHandles_{};
    std::array<void*, 2> eyeCameras_{};
    std::array<Il2CppGCHandle, 2> eyeCameraHandles_{};
    std::array<void*, 2> eyeUniversalCameraData_{};
    std::array<Il2CppGCHandle, 2> eyeUniversalCameraDataHandles_{};
    std::array<void*, 2> eyeVlAdditionalCameraData_{};
    std::array<Il2CppGCHandle, 2> eyeVlAdditionalCameraDataHandles_{};
    std::array<void*, 2> eyeVolumeStacks_{};
    std::array<void*, 2> eyeDofComponents_{};
    std::array<bool, 2> eyeDofBindLogged_{false, false};
    std::array<void*, 2> eyeUrpDofComponents_{};
    std::array<bool, 2> eyeUrpDofBindLogged_{false, false};
    std::array<void*, 2> eyeVlBloomComponents_{};
    std::array<bool, 2> eyeVlBloomBindLogged_{false, false};
    std::array<void*, 2> eyeUrpBloomComponents_{};
    std::array<bool, 2> eyeUrpBloomBindLogged_{false, false};
    std::array<std::uint32_t, 2> historyResetWrites_{0U, 0U};
    std::array<void*, 2> eyeChromaticAberrationComponents_{};
    std::array<bool, 2> eyeChromaticAberrationBindLogged_{false, false};
    std::array<void*, 2> eyeLensDistortionComponents_{};
    std::array<bool, 2> eyeLensDistortionBindLogged_{false, false};
    std::array<void*, 2> eyeMotionBlurComponents_{};
    std::array<bool, 2> eyeMotionBlurBindLogged_{false, false};
    std::array<void*, 2> eyeVignetteComponents_{};
    std::array<bool, 2> eyeVignetteBindLogged_{false, false};
    std::array<void*, 2> eyeTaaPersistentData_{};
    std::array<void*, 2> eyeMotionVectorsPersistentData_{};
    std::array<bool, 2> taaHistoryResetPending_{true, true};
    d3d11::SmaaT2xPass smaaT2xPass_{};
    d3d11::TscmaaPass tscmaaPass_{};
    bool smaaT2xRequestedForPair_ = false;
    bool smaaT2xActiveForPair_ = false;
    bool tscmaaRequestedForPair_ = false;
    bool tscmaaActiveForPair_ = false;
    bool smaaT2xComprehensiveProbeForPair_ = false;
    bool tscmaaComprehensiveProbeForPair_ = false;
    bool smaaT2xReady_ = false;
    bool tscmaaReady_ = false;
    int lastNativeTemporalAaMode_ = 0;
    std::uint32_t smaaT2xPhase_ = 0;
    std::uint32_t smaaT2xPairPhase_ = 0;
    std::uint64_t smaaT2xPairToken_ = 0;
    std::uint64_t lastSmaaT2xPoseEpoch_ = 0;
    int lastSmaaT2xQuality_ = -1;
    std::array<std::uint64_t, 2> smaaT2xCaptureCount_{};
    std::array<std::uint64_t, 2> smaaT2xCaptureFaultCount_{};
    std::array<void*, 2> smaaT2xMotionCopyTargets_{};
    std::array<Il2CppGCHandle, 2> smaaT2xMotionCopyTargetHandles_{};
    std::vector<Il2CppGCHandle> retiredSmaaT2xMotionCopyTargetHandles_;
    void* smaaT2xMotionCopyCommandBuffer_ = nullptr;
    Il2CppGCHandle smaaT2xMotionCopyCommandBufferHandle_ = nullptr;
    std::array<std::uint64_t, 2> smaaT2xMotionCopyTokens_{};
    std::array<int, 2> smaaT2xMotionCopyEvents_{};
    std::uint32_t smaaT2xMotionCopyWidth_ = 0;
    std::uint32_t smaaT2xMotionCopyHeight_ = 0;
    bool smaaT2xMotionCopyBufferLogged_ = false;
    bool smaaT2xMotionSourcesReadyForPair_ = false;
    std::uint64_t smaaT2xMotionBoundaryCount_ = 0;
    std::array<bool, 2> smaaT2xMotionHistoryCorrectedForPair_{};
    std::array<std::uint64_t, 2> smaaT2xMotionHistoryCorrectionCount_{};
    std::array<std::uint64_t, 2> smaaT2xMotionHistoryFaultCount_{};
    std::uint64_t smaaT2xWarmupLogCount_ = 0;
    std::uint64_t smaaT2xResolvedCount_ = 0;
    std::uint64_t tscmaaWarmupLogCount_ = 0;
    std::uint64_t tscmaaResolvedCount_ = 0;
    std::array<std::array<float, 16>, 2> smaaT2xExpectedProjection_{};
    std::array<std::array<float, 16>, 2>
        smaaT2xExpectedNonJitteredProjection_{};
    std::array<bool, 2> smaaT2xExpectedProjectionValid_{};
    std::uint64_t temporalHistoryResetCount_ = 0;
    std::string lastTemporalHistoryResetReason_;
    int lastAppliedAaMode_ = -1;
    int lastAppliedEyeAntialiasing_ = -1;
    int lastAppliedEyeAntialiasingQuality_ = -1;
    std::int32_t lastAppliedTaaQuality_ = -1;
    float lastAppliedTaaFrameInfluence_ = -1.0F;
    float lastAppliedTaaJitterScale_ = -1.0F;
    float lastAppliedTaaMipBias_ = -1.0F;
    float lastAppliedTaaVarianceClamp_ = -1.0F;
    float lastAppliedTaaSharpen_ = -1.0F;
    std::array<void*, 2> admissionTargets_{};
    std::array<Il2CppGCHandle, 2> admissionTargetHandles_{};
    std::array<void*, 2> fullTargets_{};
    std::array<Il2CppGCHandle, 2> fullTargetHandles_{};
    std::array<NativeTextureLease, 2> colorNativeLeases_{};
    std::array<NativeTextureLease, 2> motionNativeLeases_{};
    std::uint64_t smaaT2xMotionCopyEpoch_ = 0;
    // Retired eye textures stay rooted after RenderTexture.Release() freed
    // their GPU surfaces: Unity may still hold managed references, and a
    // collected shell would turn those into a dangling native pointer.
    std::vector<Il2CppGCHandle> retiredFullTargetHandles_;
    std::uint32_t fullWidth_ = 0;
    std::uint32_t fullHeight_ = 0;
    std::uint64_t fullGeneration_ = 0;
    std::uint64_t activeContextEpoch_ = 0;
    std::uint64_t completedContextEpoch_ = 0;
    std::uint64_t stageContextEpoch_ = 0;
    std::uint64_t renderLoopSerial_ = 0;
    bool allowOutlineDiscover_ = true;
    bool outlineDiscoverAtActorDraw_ = true;
    bool allowFlareDiscover_ = true;
    bool allowProFlareDiscover_ = true;
    std::uint64_t publishAfterLoopSerial_ = 0;
    std::uint8_t observedMask_ = 0;
    std::uint64_t armedPoseRevision_ = 0;
    std::uint64_t publishedFrames_ = 0;
    mutable std::mutex pendingGpuMutex_;
    PendingStereoGpuPublish pendingGpuPublish_{};
    std::atomic<const char*> pendingGpuFailure_{nullptr};
    std::uint32_t ownerThreadId_ = 0;
    UnityStereoCameraFrame armedFrame_{};
    void* latestSourceCamera_ = nullptr;
    void* latestSourceUniversalData_ = nullptr;
    void* latestSourceTarget_ = nullptr;
    void* currentCamera_ = nullptr;
    void* sourceDofComponent_ = nullptr;
    void* additionalDataLoggedSource_ = nullptr;
    std::uint64_t additionalDataLoggedGeneration_ = 0;
    std::uint64_t lastEligibleGeneration_ = 0;
    bool stereoInvalidatedForIneligibility_ = true;
    bool portraitArmedLogged_ = false;
    std::uint64_t eyeDofSuppressQueries_ = 0;
    std::uint64_t vlDofIsActiveQueries_ = 0;
    bool eyeDofActiveSuppressedLogged_ = false;
    bool expectOwnedEyeDof_ = false;
    // Actor-shadow anchor. Tick copies `sourcePose` (pre-HMD shot, or
    // the free-cam rig). The saved transform is what
    // BeginActorShadowSourceAnchor overwrote and End must put back
    // before the next pass sees the camera.
    pose::Pose actorShadowAnchorPose_{};
    bool actorShadowAnchorPoseValid_ = false;
    pose::Pose cinematicPose_{};
    bool cinematicPoseValid_ = false;
    pose::Pose playerPose_{};
    bool playerPoseValid_ = false;
    pose::Pose headsetPose_{};
    bool headsetPoseValid_ = false;
    pose::Pose leftEyePose_{};
    bool leftEyePoseValid_ = false;
    enum class ToonLightingReference : std::uint8_t {
        Unavailable,
        LeftEye,
        SourcePose,
        PlayerLookAt,
        HeadsetLookAt,
        CinematicLookAt,
    };
    pose::Pose toonLightingPose_{};
    bool toonLightingPoseValid_ = false;
    ToonLightingReference toonLightingReference_ =
        ToonLightingReference::Unavailable;
    pose::Vector3 toonLightView_{0.0F, 0.0F, 1.0F};
    bool toonLightViewConstructed_ = false;
    bool toonWasPaused_ = false;
    bool toonPauseFrozen_ = false;
    pose::Pose toonFrozenPose_{};
    pose::Vector3 toonFrozenLightView_{0.0F, 0.0F, 1.0F};
    bool toonFrozenLightConstructed_ = false;
    bool toonWasFreeCam_ = false;
    float toonAuthoredPitchDeg_ = 0.0F;
    bool toonAuthoredPitchValid_ = false;
    float toonFrozenPitchDeg_ = 0.0F;
    bool toonFrozenPitchValid_ = false;
    float toonLastLookYawRad_ = 0.0F;
    bool toonLastLookYawValid_ = false;
    int toonFollowIndex_ = 0;
    struct ToonActorSample {
        int index = -1;
        pose::Vector3 position{};
        pose::Vector3 forward{};
        std::int64_t lastSeenNanoseconds = 0;
    };
    static constexpr std::size_t kToonActorCapacity = 16;
    ToonActorSample toonActors_[kToonActorCapacity]{};
    void* actorShadowSavedTransformSelf_ = nullptr;
    pose::Pose actorShadowSavedPose_{};
    bool actorShadowAnchorActive_ = false;
    bool actorShadowAnchorSkipLogged_ = false;
    // Volume-trigger source anchor (`.229`, lobby boundary row). The lobby
    // authors local blend=0 lighting/post volumes around each shot position;
    // our head-pose bridge drives the source camera transform across their
    // collider edges, hard-swapping VLActorParameter/fog (`.228` census +
    // matcap ping-pong). A mod-owned proxy transform follows the pre-HMD
    // `frame.cinematicPose` every Tick and is written to the source camera's
    // UniversalAdditionalCameraData.volumeTrigger, so per-frame volume
    // evaluation stays at the authored pose. Eyes inherit the trigger at
    // configure (existing sourceVolumeTrigger copy). URP falls back to the
    // camera transform if the proxy dies mid-frame (disasm-proven), and the
    // proxy GameObject is recreated on the next Tick; retired handles stay
    // rooted per the `.188` GCHandle rule.
    void* volumeTriggerGameObject_ = nullptr;
    Il2CppGCHandle volumeTriggerGameObjectHandle_ = nullptr;
    void* volumeTriggerTransform_ = nullptr;
    std::vector<Il2CppGCHandle> retiredVolumeTriggerHandles_;
    void* volumeAnchorAppliedData_ = nullptr;
    void* volumeAnchorSourceCamera_ = nullptr;
    void* volumeAnchorSavedTrigger_ = nullptr;
    bool volumeAnchorActive_ = false;
    bool volumeAnchorProxyFailedLogged_ = false;
    // One line per pass/camera pair, so a hardware log shows which of the
    // three passes actually reached the anchor and on which camera.
    std::vector<std::string> actorShadowAnchorLoggedKeys_;
    std::uint64_t actorShadowAnchorSamples_ = 0;
    std::int32_t actorLightMatcapMainId_ = 0;
    std::int32_t actorLightMatcapRimId_ = 0;
    std::int32_t actorLightLitActorMainId_ = 0;
    std::int32_t actorLightLitActorRimId_ = 0;
    std::int32_t actorLightVlSpecColorId_ = 0;
    std::int32_t actorLightGlobalLightParameterId_ = 0;
    std::int32_t actorLightMatcapLightColorId_ = 0;
    std::int32_t actorLightMatcapRimColorId_ = 0;
    std::int32_t actorLightMatcapParamId_ = 0;
    // Unity builtin. GBuffer perspective V = normalize(cb0[21] − P).
    std::int32_t actorLightWorldSpaceCameraPosId_ = 0;
    // Unity builtins published by URP SetCameraMatrices via SetGlobalMatrix
    // (same batch as _WorldSpaceCameraPos, which `.144` proved reaches the
    // actor GBuffer $Globals cb). The matcap frame rows cb0[65–67] follow
    // head pitch through these, not through the native unity_MatrixV.
    std::int32_t actorLightWorldToCameraId_ = 0;
    std::int32_t actorLightCameraToWorldId_ = 0;
    std::int32_t actorLightDirectionId_ = 0;
    std::int32_t actorLightWorldToActorShadowId_ = 0;
    MethodRef shaderGetGlobalVector_{};
    MethodRef shaderGetGlobalMatrix_{};
    bool actorLightDiagnosticResolved_ = false;
    std::uint64_t actorLightDiagnosticTicks_ = 0;
    std::uint64_t actorLightDiagnosticCalls_ = 0;
    std::chrono::steady_clock::time_point actorLightIdRetryAt_{};
    std::chrono::steady_clock::time_point actorLightDiagnosticNextAt_{};
    // Per-window pass-execution counters, indexed [pass][camera class]:
    // pass 0=campus-actor-param 1=vl-actor-param 2=actor-shadow 3=other;
    // camera 0=source 1=left 2=right 3=other/none. Printed and reset every
    // ~10 s so a single hardware run shows which passes actually execute in
    // which scene on which cameras.
    std::array<std::array<std::uint32_t, 4>, 4> passActivityCounts_{};
    std::chrono::steady_clock::time_point passActivityPrintAt_{};
    bool matcapCompResolved_ = false;
    void* vlActorParameterType_ = nullptr;
    void* campusLitActorType_ = nullptr;
    std::int32_t actorParamMainAngleOffset_ = -1;
    std::int32_t actorParamMainSpaceOffset_ = -1;
    std::int32_t actorParamRimAngleOffset_ = -1;
    std::int32_t actorParamRimPowerOffset_ = -1;
    std::int32_t actorParamRimColorOffset_ = -1;
    std::int32_t volumeParameterColorValueOffset_ = -1;
    // _MatCapParam pack (UpdateActorCommand 0x39b): three float volume
    // params at component +0x50 / +0x58 / +0x70 → (x, y, z, 0).
    std::int32_t actorParamMatcapOffsetOffset_ = -1;
    std::int32_t actorParamMatcapSmoothOffset_ = -1;
    std::int32_t actorParamShadeApplyOffset_ = -1;
    bool actorParamFieldsLogged_ = false;
    std::int32_t actorParamGiScaleOffset_ = -1;
    std::int32_t actorParamAddLightOffset_ = -1;
    std::int32_t actorParamAddSpecOffset_ = -1;
    std::int32_t litActorMainAngleOffset_ = -1;
    std::int32_t litActorMainSpaceOffset_ = -1;
    std::int32_t litActorRimAngleOffset_ = -1;
    std::int32_t litActorRimPowerOffset_ = -1;
    std::int32_t volumeParameterVector2ValueOffset_ = -1;
    std::int32_t volumeParameterEnumValueOffset_ = -1;
    MethodRef volumeManagerGetInstance_{};
    MethodRef volumeManagerGetStack_{};
    MethodRef commandBufferSetGlobalVector_{};
    MethodRef commandBufferSetGlobalMatrix_{};
    MethodRef commandBufferClear_{};
    MethodRef commandBufferSetupCameraProperties_{};
    MethodRef commandBufferSetViewProjectionMatrices_{};
    MethodRef contextExecuteCommandBuffer_{};
    MethodRef contextSetupCameraProperties_{};
    MethodRef scriptableRendererSetCameraMatrices_{};
    std::size_t commandBufferSetupCameraPropertiesArgs_ = 0;
    std::size_t contextSetupCameraPropertiesArgs_ = 0;
    std::uint64_t matcapGbufferLockApplied_ = 0;
    bool matcapGbufferEnterLogged_ = false;
    bool matcapGbufferRestoreFailLogged_ = false;
    std::chrono::steady_clock::time_point matcapGbufferLogAt_{};
    bool commandBufferSetViewProjectionIsInjected_ = false;
    bool contextSetupCameraPropertiesHasStereo_ = false;
    bool contextSetupCameraPropertiesHasEye_ = false;
    bool contextSetupCameraPropertiesIsStatic_ = false;
    void* matcapCompCommandBuffer_ = nullptr;
    // -1 unknown; 0 = angle.x is pitch, angle.y is yaw; 1 = swapped. Chosen
    // once by matching the recomputed vector against the live shader global
    // before any compensation has been written.
    int matcapCompAngleOrder_ = -1;
    std::int32_t matcapCompSpaceValue_ = 0;
    std::uint64_t matcapCompApplied_ = 0;
    bool matcapCompFailLogged_ = false;
    bool matcapCompSpaceSkipLogged_ = false;
    bool matcapCompGateLogged_ = false;
    bool matcapCompExecLogged_ = false;
    bool actorLightDiagnosticEnterLogged_ = false;
    // Wall-clock throttle for MATCAP_COMP_APPLIED: the .90 run proved the
    // modulo-based sample (%900) can vanish wholesale, so sample on time.
    std::chrono::steady_clock::time_point matcapCompLogAt_{};
    // VLActorShadow volume readback (projected actor shadow): directionalType
    // {0=Fixed,1=Matcap,2=ShadowLight} decides how the virtual directional
    // light is derived; Matcap uses the camera rotation, matching the user's
    // "projection follows head angle, not position" report.
    bool actorShadowVolumeResolved_ = false;
    void* vlActorShadowType_ = nullptr;
    std::int32_t actorShadowDirectionalTypeOffset_ = -1;
    std::int32_t actorShadowLightDirectionalOffset_ = -1;
    std::int32_t volumeParameterVector3ValueOffset_ = -1;
    // cameraData view-matrix patch around DrawActorShadowPass.AddRenderPasses.
    // Offsets are struct-relative (il2cpp field offsets minus the 0x10 boxed
    // header).
    bool actorShadowViewPatchResolved_ = false;
    std::int32_t renderingDataCameraDataOffset_ = -1;
    std::int32_t cameraDataViewMatrixOffset_ = -1;
    std::int32_t cameraDataProjectionOffset_ = -1;
    std::int32_t cameraDataWorldPosOffset_ = -1;
    bool actorShadowViewPatchActive_ = false;
    float actorShadowViewPatchSavedView_[16]{};
    float actorShadowViewPatchSavedPos_[3]{};
    std::chrono::steady_clock::time_point actorShadowViewPatchLogAt_{};
    bool sourceCameraEndedLogged_ = false;
    void* suppressedSourceCamera_ = nullptr;
    bool sourceCameraSuppressed_ = false;
    // .118 tiny-source mode: source keeps rendering into a 128x128 dummy
    // RT instead of being disabled. See Config::vrSourceCameraTiny.
    void* tinySourceTarget_ = nullptr;
    Il2CppGCHandle tinySourceTargetHandle_ = nullptr;
    void* tinySourceCamera_ = nullptr;
    void* tinySavedTarget_ = nullptr;
    bool tinyModeActive_ = false;
    bool tinyTargetCreateFailed_ = false;
    bool tinyAppliedLogged_ = false;
    bool tinyRebindLogged_ = false;
    // Grip panel transparency armed state; see GripTransparencyArmed().
    std::atomic<bool> gripTransparencyArmed_{false};
    // .220 hardware run proved Camera.clearFlags is dead ground: the write
    // was applied and verified (UICamera 4→2, transparent bg) yet the
    // backbuffer stayed fully opaque — the UI camera renders through the
    // game's own Campus.Common.UIRenderer pipeline, whose UIRenderPass
    // bakes private _needsClear/_clearColor at construction from the
    // UIRendererFeature's serialized fields and never consults the camera.
    // .256: the .224 answer to that (feature-field writes + live-pass
    // stamping + disarm restore) was removed as superseded — the .240
    // Execute wrapper writes the pass fields ahead of every use and
    // restores them right after, so nothing persists across frames. Only
    // the pass-instance offsets the wrapper needs remain.
    int gripUiPassRescanCountdown_ = 0;
    bool gripUiPassApiResolved_ = false;
    bool gripUiPassApiLogged_ = false;
    std::int32_t gripUiPassIndexOffset_ = -1;
    std::int32_t gripUiPassNeedsClearOffset_ = -1;
    std::int32_t gripUiPassClearColorOffset_ = -1;
    std::int32_t gripUiPassDrawFbOffset_ = -1;
    // .225 Execute-hook prefix: log budget refilled on each arm so the
    // per-frame branch observation stays quiet in steady state. Under
    // vrDiagnosticsStartupEnabled the arm refill is 120 lines (~2 s of
    // per-frame branch sequence) instead of 4 (.254 mixing discriminator).
    std::atomic<int> gripExecFixupLogBudget_{0};
    // .261 three-tier Execute policy (game thread computes per rescan in
    // ApplyGripAdvUiMute; render-thread wrapper reads):
    // - force-all while an ADV is visibly open (UIManager._root
    //   activeInHierarchy — the .253 dormant-engine trap fix) OR the home
    //   page is shown (HomePageViewBase._isShow): these screens' verified
    //   look comes from the .240 full forcing (home pass0 is game-native;
    //   fb-stamping alone would strip pass1's intermediate blit and lose
    //   the pass0 UI layer — the .260 probe/EXEC dig).
    // - otherwise, intervene ONLY on game-chosen fb frames (transparent
    //   pass0 stamp + stale-blit strip, never flip branches): kills the
    //   authored opaque clear colors (UIRendererFeature black — the .260
    //   char-select black; Campus variant white) while native frames keep
    //   the .227 transparency (UI drawn into the cleared VL composite:
    //   bag select, produce-resume confirm).
    std::atomic<bool> gripForceAllForExec_{false};
    // .226 Present-hook intermediate clear: hash of the last published
    // native-pointer set, for scan/transition logging (0 = nothing sent).
    std::size_t gripIntermediateLastPublished_ = 0;
    // .230/.231 tried muting the frosted BackgroundBlur panels (param
    // writes, then disabling the host Graphic). Both proved wrong-target on
    // hardware: the boards render near-transparent while armed (their blur
    // source is our cleared attachment) and the opaque black/white fills
    // come from independent backdrop plates — disabling the boards only
    // uncovered white plates behind them. Reverted in .234; the residual is
    // tracked in BACKLOG as a per-screen backdrop-plate dig.
    // .235 plate census (vrDiagnosticsStartupEnabled only): on each arm,
    // enumerate live Graphics and log the opaque near-black / near-white
    // ones with class, color, sprite/texture and parent path — the plate
    // signature hunt for the black/white screens.
    void MaybeRunGripPlateCensus() noexcept;
    void RunGripPlateCensus(const char* phase) noexcept;
    void RunGripCameraCensus(const char* phase) noexcept;
    // .257: frozen-frame family census — active RawImages whose texture is
    // a Texture2D (point-in-time snapshot displays, the CaptureUtility
    // family) plus every VLSRPTargetImage with its Target/Captured type.
    void RunGripSnapshotCensus(const char* phase) noexcept;
    int gripPlateCensusRuns_ = 0;
    int gripPlateCensusDelay_ = 0;
    // .236: census-identified backdrop plates hidden while armed. First
    // target: the ADV engine's opaque black "Background" RawImage (the
    // 初星コミュ black); the ADV 3D itself arrives through a "Render
    // Target" RawImage showing a swapped-aspect VL surface, which the
    // Present hook now clears as well.
    // .247: the universal lever — every screen's CampusScreenCommonView
    // owns a _backgroundRoot GameObject with the game's own public
    // SetBackgroundRootActive(bool). While armed, all screen backdrops
    // (black, white, colored, textured) go off through the official API;
    // saved active states return on disarm.
    struct GripScreenBackgroundMute {
        void* commonView = nullptr;
        bool wasActive = false;
    };
    std::vector<GripScreenBackgroundMute> gripScreenBackgroundMutes_;
    void ApplyGripScreenBackgroundMute() noexcept;
    void RestoreGripScreenBackgroundMute(const char* reason) noexcept;
    // .248: Campus.ADV.UIManager owns the ADV backdrop pair — the
    // _capturedImage RawImage (displays a Texture2D snapshot of the screen
    // underneath: the frozen-popup / propagated-white mechanism) and the
    // _contentBackground RawImage. Hidden through the official getters
    // while armed; enabled states restored on disarm.
    struct GripAdvUiMute {
        void* graphic = nullptr;
        bool wasEnabled = false;
    };
    std::vector<GripAdvUiMute> gripAdvUiMutes_;
    void ApplyGripAdvUiMute() noexcept;
    void RestoreGripAdvUiMute(const char* reason) noexcept;
    struct GripPlateHide {
        void* graphic = nullptr;
        bool wasEnabled = false;
    };
    std::vector<GripPlateHide> gripPlateHides_;
    void ApplyGripPlateHide() noexcept;
    void RestoreGripPlateHide(const char* reason) noexcept;
    bool eyeMainCameraTagApplied_ = false;
    bool eyeMainCameraTagApiLogged_ = false;
    void* eyeMainCameraTaggedObject_ = nullptr;
    void* mainCameraTagString_ = nullptr;
    void* untaggedTagString_ = nullptr;
    bool sourceSuppressLogged_ = false;
    bool sourceReenableFightLogged_ = false;
    bool sourceHistoryResetLatched_ = false;
    bool sourceCutSignatureValid_ = false;
    float lastSourceCutFov_ = 0.0F;
    float lastSourceCutPosX_ = 0.0F;
    float lastSourceCutPosY_ = 0.0F;
    float lastSourceCutPosZ_ = 0.0F;
    bool copyFromSkippedLogged_ = false;
    bool eyeBeginCameraLogged_ = false;
    bool eyePostProcessRestoredLogged_ = false;
    bool eyeDofDeactivatedLogged_ = false;
    bool eyeDofParamsLogged_ = false;
    struct UiTextureOverlayEntry {
        void* component = nullptr;
        void* gameObject = nullptr;
        void* hiddenMaterial = nullptr;
        float authoredColorX = 0.0F;
        float authoredColorY = 0.0F;
        float authoredColorZ = 0.0F;
        float authoredColorW = 0.0F;
        bool hiddenByUs = false;
    };
    std::vector<UiTextureOverlayEntry> uiTextureOverlays_{};
    std::uint64_t uiTextureOverlayDiscoverSerial_ = 0;
    std::chrono::steady_clock::time_point uiTextureOverlayNextDiscoverAt_{};
    std::int32_t uiTextureOverlayColorId_ = 0;
    bool uiTextureOverlaysHidden_ = false;
    bool uiTextureOverlayApiLogged_ = false;
    bool uiTextureOverlayDiscoverLogged_ = false;
    bool uiTextureOverlayHideLogged_ = false;
    bool uiTextureOverlaySkipLogged_ = false;
    bool uiTextureOverlayRestoreLogged_ = false;
    struct LiveCameraOverlayEntry {
        void* component = nullptr;
        void* gameObject = nullptr;
        bool hiddenByUs = false;
    };
    std::vector<LiveCameraOverlayEntry> liveCameraOverlays_{};
    std::uint64_t liveCameraOverlayDiscoverSerial_ = 0;
    std::chrono::steady_clock::time_point liveCameraOverlayNextDiscoverAt_{};
    std::chrono::steady_clock::time_point liveCameraOverlayNextEmptyLogAt_{};
    bool liveCameraOverlaysHidden_ = false;
    bool liveCameraOverlayApiLogged_ = false;
    bool liveCameraOverlayDiscoverLogged_ = false;
    bool liveCameraOverlayHideLogged_ = false;
    bool liveCameraOverlayRestoreLogged_ = false;
    struct CmovParticleEntry {
        void* component = nullptr;
        void* gameObject = nullptr;
        bool hiddenByUs = false;
    };
    std::vector<CmovParticleEntry> cmovParticles_{};
    std::uint64_t cmovParticleDiscoverSerial_ = 0;
    std::chrono::steady_clock::time_point cmovParticleNextDiscoverAt_{};
    std::chrono::steady_clock::time_point cmovParticleNextEmptyLogAt_{};
    bool cmovParticlesHidden_ = false;
    bool cmovParticleApiLogged_ = false;
    bool cmovParticleDiscoverLogged_ = false;
    bool cmovParticleHideLogged_ = false;
    bool cmovParticleRestoreLogged_ = false;
    struct AuthoredBloomSnapshot {
        bool valid = false;
        float vlIntensity = 0.0F;
        std::int32_t vlDiffusion = 0;
        float urpIntensity = 0.0F;
        float urpScatter = 0.0F;
        float urpDirt = 0.0F;
    };

    bool eyeBloomParamsLogged_ = false;
    bool lastBloomComfortPath_ = false;
    bool eyeFovBundleLogged_ = false;
    bool lensFlareScaleLogged_ = false;
    bool proFlareScaleLogged_ = false;
    bool proFlareGripWriteLogged_ = false;
    bool proFlareEyeScaled_ = false;
    bool outlineMaterialApiReady_ = false;
    bool outlineEyeScaled_ = false;
    bool outlineMaterialLogged_ = false;
    bool outlineGripRestoreLogged_ = false;
    float outlineGripLoggedScale_ = -1.0F;
    std::chrono::steady_clock::time_point outlineGripLogAt_{};
    float lastOutlineWidthScale_ = -1.0F;
    float loggedOutlineWidthScale_ = -1.0F;
    float outlineEyeLoggedW_ = -1.0F;
    bool outlineMaterialDumpLogged_ = false;
    bool materialGetVectorUsesOutParam_ = false;
    int outlineParamId_ = 0;
    const char* outlineGetVectorName_ = "-";
    const char* outlineSetVectorName_ = "-";
    void* outlineFindPassForwardName_ = nullptr;
    void* outlineFindPassGBufferName_ = nullptr;
    struct AuthoredOutlineMaterial {
        void* material = nullptr;
        float authoredX = 0.05F;
        float authoredY = 5.0F;
        float authoredZ = 0.011111F;
        float authoredW = 4.987752F;
        bool authoredValid = false;
    };
    std::vector<AuthoredOutlineMaterial> actorOutlineMaterials_{};
    struct OfficialOutlineCapture {
        bool valid = false;
        float fovDegrees = 0.0F;
        std::array<float, 4> value{};
    };
    void EnsureOutlineOfficialApi() noexcept;
    bool outlineOfficialApiAttempted_ = false;
    MethodRef outlineCurveEvaluate_{};
    std::int32_t outlineOfficialSettingsOffset_ = -1;
    std::array<std::int32_t, 4> outlineOfficialFieldOffsets_{{-1, -1, -1, -1}};
    // Slot 0 = latest non-eye actor camera (Grip/source), slot 1 = eyes.
    std::array<OfficialOutlineCapture, 2> officialOutline_{};
    std::array<bool, 2> officialOutlineLogged_{{false, false}};
    std::array<std::array<float, 4>, 2> officialOutlineLoggedValue_{};
    std::array<std::chrono::steady_clock::time_point, 2>
        officialOutlineLoggedAt_{};
    std::mutex pendingOutlineMaterialMutex_{};
    std::vector<void*> pendingOutlineMaterials_{};
    std::atomic<bool> outlineCaptureEnabled_{true};
    std::atomic<bool> outlineCapturePending_{false};
    std::uint64_t outlineMaterialDiscoverSerial_ = 0;
    MethodRef sceneManagerGetSceneCount_{};
    MethodRef sceneManagerGetLoadedSceneCount_{};
    MethodRef sceneManagerGetActiveSceneInjected_{};
    bool sceneManagerApiReady_ = false;
    bool sceneIdentityValid_ = false;
    std::array<
        LifetimeWriteRecord,
        static_cast<std::size_t>(LifetimeWriteCategory::Count)>
        lifetimeWrites_{};
    std::uint64_t lifetimeWriteSerial_ = 0;
    std::uint64_t lifetimeSnapshotSerial_ = 0;
    std::uint64_t observedSceneReadyRevokeSerial_ = 0;
    std::uint64_t observedSceneReadyReleaseSerial_ = 0;
    bool outlineEmptyDiscoverLogged_ = false;
    bool sceneReadyParked_ = false;
    bool eyeArmHeld_ = false;
    bool eyeArmAwaitingSettle_ = false;
    bool eyeArmSkipLogged_ = false;
    int sceneCount_ = 0;
    int loadedSceneCount_ = 0;
    int activeSceneHandle_ = 0;
    bool virtualCameraCensusIdentityObserved_ = false;
    bool virtualCameraCensusPending_ = false;
    bool virtualCameraCensusApiLogged_ = false;
    int virtualCameraCensusSceneCount_ = 0;
    int virtualCameraCensusLoadedSceneCount_ = 0;
    int virtualCameraCensusActiveSceneHandle_ = 0;
    std::uint32_t virtualCameraCensusDelayTicks_ = 0;
    std::uint32_t virtualCameraCensusRunsRemaining_ = 0;
    std::uint64_t virtualCameraCensusSerial_ = 0;
    const char* virtualCameraCensusReason_ = "startup";
    const char* eyeArmHoldReason_ = nullptr;
    std::atomic<bool> projectionEquivalentProFlareReady_{false};
    float bloomSourceFovDegrees_ = 29.9F;
    std::array<AuthoredBloomSnapshot, 2> authoredBloom_{};
    struct AuthoredFlareScale {
        void* component = nullptr;
        float scale = 1.0F;
        float intensity = 1.0F;
    };
    struct AuthoredProFlareScale {
        void* component = nullptr;
        float globalScale = 1.0F;
        float globalBrightness = 1.0F;
        float dynamicEdgeBoost = 0.0F;
        float dynamicCenterBoost = 0.0F;
    };
    std::vector<AuthoredFlareScale> authoredFlareScales_{};
    std::vector<AuthoredProFlareScale> authoredProFlareScales_{};
    std::uint64_t lensFlareDiscoverSerial_ = 0;
    std::uint64_t proFlareDiscoverSerial_ = 0;
    float bloomEyeFovDegrees_ = 0.0F;
    float sourceLiveFovDegrees_ = 0.0F;
    float sourceFittedFovDegrees_ = 0.0F;
    float sourceAspect_ = 0.0F;
    float sourceFocalLengthMillimeters_ = 0.0F;
    float sourceSensorWidthMillimeters_ = 0.0F;
    float sourceSensorHeightMillimeters_ = 0.0F;
    float lastLoggedSourceFovDegrees_ = 0.0F;
    UnitySourceCameraStateDiagnostic sourceCameraStateDiagnostic_{};
    float fovDiagnosticLastProjectionM11_ = 0.0F;
    float fovDiagnosticLastLookAtDistance_ = 0.0F;
    std::uint64_t fovDiagnosticLastStateSample_ = 0;
    std::uint64_t fovDiagnosticLogs_ = 0;
    bool sourcePhysicalProperties_ = false;
    std::uint64_t sourceLensSamples_ = 0;
    std::array<camera::ProjectionMap, 2> sourceToEyeProjectionMaps_{};
    std::array<bool, 2> sourceToEyeProjectionValid_{};
    std::uint64_t sourceToEyeProjectionRevision_ = 0;
    void* sourceToEyeProjectionCamera_ = nullptr;
    void* latestSourceVolumeStack_ = nullptr;
    UnityStereoCameraFrame latestFrame_{};
    TargetSpecSnapshot latestTargetSpec_{};
    const char* failureStage_ = nullptr;
};

// Render-thread bridge for the UIRenderPass.Execute hook (Hook.cpp). Enter
// returns true when the pass was modified for the armed direct-framebuffer
// path and must be restored via Exit after the original Execute returns.
// No-ops when the renderer instance or field offsets are not published.
[[nodiscard]] bool GripUiPassExecuteEnter(
    void* pass, UnityStereoRenderer::GripUiPassExecState& saved) noexcept;
// Read-only gate for UI material submission, which can precede Execute.
[[nodiscard]] bool GripUiTransparencyArmed() noexcept;
void GripUiPassExecuteExit(
    void* pass, const UnityStereoRenderer::GripUiPassExecState& saved) noexcept;

} // namespace gakumas::vr
