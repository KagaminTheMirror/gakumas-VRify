#pragma once

#include <filesystem>
#include <string>

namespace GakumasLocal::Config {
    extern bool isConfigInit;

    extern bool dbgMode;
    // File-backed VR diagnostics switch. Unlike the upstream dbgMode, this is
    // sampled only during bootstrap and is not controlled by keyboard input.
    // It gates optional render/camera diagnostics, not VR startup itself.
    extern bool vrDiagnosticsEnabled;
    // Frozen bootstrap result for ordinary VR runtime startup (vrEnabled).
    extern bool vrRuntimeStartupEnabled;
    // Frozen bootstrap result for diagnostics (vrEnabled && vrDiagnosticsEnabled).
    extern bool vrDiagnosticsStartupEnabled;
    // Legacy Localify key. In the VR fork this is the Localify feature
    // layer (unlocks, graphics, extra bundles), not the injected-module
    // lifetime switch and not the text/font/texture localization gate.
    extern bool enabled;
    // VR-owned localization valve. When false, I18n / generic text /
    // MasterDB / font / texture replacements stay off even if `enabled`
    // is true. Default off so other Localify features can run without
    // overlaying Chinese onto the game.
    extern bool vrLocalizeText;
    // VR menu language. 0 = follow Windows UI language on each launch
    // until the user picks a concrete language. 1 zh-CN, 2 zh-TW, 3 ja,
    // 4 en.
    constexpr int kVrMenuLanguageSystem = 0;
    constexpr int kVrMenuLanguageZhCN = 1;
    constexpr int kVrMenuLanguageZhTW = 2;
    constexpr int kVrMenuLanguageJa = 3;
    constexpr int kVrMenuLanguageEn = 4;
    constexpr int kDefaultVrMenuLanguage = kVrMenuLanguageSystem;
    extern int vrMenuLanguage;
    extern bool vrEnabled;
    // Native D3D11/OpenXR-only safety boundary. When true, Unity camera,
    // head-pose, and stereo-render integration remain disabled.
    extern bool vrNativeOnly;
    // First camera-write milestone. This composes relative tracked head motion
    // on top of the game's own Cinemachine pose and remains opt-in by default.
    extern bool vrHeadPoseEnabled;
    extern float vrWorldScale;
    extern bool vrStereoEnabled;
    extern float vrStereoRenderScale;
    // Rollback only: true keeps stereo on landscape Live and leaves
    // portrait on the flat mirror. Default false (.121) so portrait
    // 3D scenes (home / produce) use the same eyes + Grip UI panel.
    extern bool vrStereoLandscapeOnly;
    // Eye actor outline width as a fraction of the official Campus/VL
    // `_OutlineParam` (0.05, 5.0). 1.0 = .71-era fat VR outline. Default
    // is the .77 mode-body lock (29.9/100.24 ≈ 0.30). 0 hides it.
    constexpr float kDefaultVrEyeOutlineWidth = 29.9F / 100.24F;
    extern float vrEyeOutlineWidth;
    // Source-follow bloom range. Off (default) writes the pre-.270
    // mode-body kernel. On writes the .270 FOV-eq kernel (closer to
    // the shot, often brighter). Intensity stays authored. Free
    // camera ignores this and always uses the mode-body kernel.
    extern bool vrBloomFollowSourceCamera;
    // Eye AA: 0 inherit source, 1 force TAA, 2 force SMAA, 3 none,
    // 4 SMAA T2x, 5 TSCMAA. Default is TSCMAA (accepted stereo.210).
    constexpr int kDefaultVrEyeAaMode = 5;
    extern int vrEyeAaMode;
    // URP TemporalAA.Settings prefix. Trailing history/jitter counters
    // stay per-eye and are never written from config.
    // The headset menu exposes quality / jitter / influence only.
    // mipBias, varianceClamp, and CAS stay file-backed so the 24-byte
    // Settings copy still writes photography values; dump shows no
    // Low-quality eye-path consumer for those three.
    constexpr int kDefaultVrEyeTaaQuality = 1;
    constexpr float kDefaultVrEyeTaaFrameInfluence = 0.4F;
    constexpr float kDefaultVrEyeTaaJitterScale = 0.75F;
    constexpr float kDefaultVrEyeTaaMipBias = 0.0F;
    constexpr float kDefaultVrEyeTaaVarianceClamp = 0.9F;
    constexpr float kDefaultVrEyeTaaSharpen = 0.0F;
    extern int vrEyeTaaQuality;
    extern float vrEyeTaaFrameInfluence;
    extern float vrEyeTaaJitterScale;
    extern float vrEyeTaaMipBias;
    extern float vrEyeTaaVarianceClamp;
    extern float vrEyeTaaSharpen;
    // URP AntialiasingQuality: 0 Low, 1 Medium, 2 High.
    extern int vrEyeSmaaQuality;
    // Optional shot-camera anchor for projected actor shadows only (`.95`):
    // hair/accessory self-shadow and large set-piece blobs. Since `.278`, the
    // projected map and ActorShadowData always come from the left eye for both
    // eyes; this switch only selects live-left versus shot-camera generation.
    // URP sun/spot shadows are untouched. Default off; experimental menu tab.
    extern bool vrActorShadowSourceAnchor;
    // Shared Toon/MatCap reference for both actor-eye draws. Since `.276` this
    // is baseline stereo behavior rather than an optional lock: even with no
    // projected-shadow lock, right-eye Toon uses the left-eye frame.
    // The Toon lock remains an independent control. Off follows the live left
    // eye; on selects one shared lock target below. It never permits a separate
    // right-eye Toon calculation.
    extern bool vrActorToonSourceAnchor;
    // Toon lock target (only while the lock is on):
    // 0 = source camera / free-camera rig,
    // 1 = player look-at (free-cam: from player toward nearest actor),
    // 2 = headset look-at (free-cam: from headset toward nearest actor).
    constexpr int kVrToonFollowRefSource = 0;
    constexpr int kVrToonFollowRefPlayer = 1;
    constexpr int kVrToonFollowRefHeadset = 2;
    constexpr int kDefaultVrToonFollowRef = kVrToonFollowRefSource;
    extern int vrToonFollowRef;
    // Shot-camera anchor for volume-stack evaluation (`.229`). Scenes author
    // local blend=0 lighting/post volumes around each shot position; the
    // head-driven source camera crossing their collider edges hard-swaps
    // VLActorParameter and fog (lobby boundary, `.228` census). When on, a
    // proxy transform pinned to the authored Cinemachine pose is written to the
    // source camera's UniversalAdditionalCameraData.volumeTrigger. Default on.
    extern bool vrVolumeSourceAnchor;
    // When true and stereo is healthy, Camera.enabled=false on the
    // Cinemachine output (Grip source). Eyes keep rendering. Default off.
    extern bool vrDisableSourceCamera;
    // While the source camera is suppressed, pulse the eye resetHistory
    // every frame (.107 workaround). Default off: the trail is killed by
    // vrVlMotionBlurOff, and this pulse would hide the remaining TAA jitter.
    extern bool vrSourceOffResetPerFrame;
    // Diagnostic: force the VL JumpFlood outline pass onto its non-temporal
    // branch for eye cameras (zero the by-value CameraData antialiasing so
    // IsTemporalAAEnabled fails inside DoJumpFloodOutline only). The
    // outline distance-field history is a single pass-instance double
    // buffer shared by every camera, so eye reprojection always consumes
    // another camera's silhouette. Default off.
    extern bool vrEyeJumpFloodTemporalOff;
    // Skip VLPostProcessPass.DoVLMotionBlur entirely (detour returns false,
    // identical to the VLMotionBlur.IsActive()==false path). The blur smears
    // the frame along a velocity texture reconstructed from motion vectors
    // with a discrete sampleCount — prime suspect for the source-off
    // multi-silhouette trail (AA-independent, killed by MVPD reset).
    // Authored motion blur is undesirable in VR anyway. Default on.
    extern bool vrVlMotionBlurOff;
    // While the source is suppressed: the left eye GameObject is tagged
    // MainCamera and managed Camera.get_main returns the left eye.
    // .116 hardware proved this has no effect on the source-off TAA
    // jitter; menu row dropped in .119. Default off (config-only).
    extern bool vrEyeAsMainCamera;
    // Keep the source camera rendering (all per-frame pipeline bookkeeping
    // stays live) but redirect its targetTexture to a 128x128 dummy RT so
    // the GPU cost collapses. .118 hardware: TAA normal, perf recovered —
    // this is the accepted source-cost fix; full disable keeps broken TAA.
    // Mutually exclusive with vrDisableSourceCamera (full disable wins).
    extern bool vrSourceCameraTiny;
    // While the source camera is unused (tiny mode active or fully
    // suppressed), clear the desktop backbuffer to transparent black after
    // each Present and submit the Grip quad with source-alpha blending, so
    // only the UI floats over the stereo scene instead of a frozen 3D frame.
    // Inert outside a healthy-stereo 3D scene (title / loading / pure UI keep
    // the opaque quad). Default off.
    extern bool vrGripPanelTransparent;
    // Skip owned-eye true lens cards: drawable DrawFlare shapes (type 1..4),
    // DoVLTextureBlur, the late OverlayCanvas RenderObjectsPass (event 600),
    // CampusLiveCameraOverlay near-plane cards, and Live ParticleSystems
    // whose name/path contains the live-observed `cmov` token
    // (GameObject.SetActive false; never the source/eye camera object).
    // DrawFlare returns are ignored by DoVLAdditive; TextureBlur's caller
    // Swap()s only when the bool is true. OverlayCanvas Execute is void —
    // returning without orig
    // is the same as an empty DrawRenderers. Source / monitor / Grip stay
    // authored. Does not skip VirtualEffect aura or world-anchored ProFlare.
    extern bool vrHideUiTextureOverlay;
    // World-space official-scale glow sticks at both hands. Default off.
    // Shown only when official MeshRenderer materials can be read.
    extern bool vrHandGlowSticks;
    // Skip VLMotionVectorRenderPass.DrawObjectMotionVectors for eye
    // cameras (camera motion vectors still draw). .112 ruled out managed
    // Camera.main; the remaining TAA-jitter suspect is dirty object MVs
    // in the same texture VL motion blur was reading. Default off
    // (diagnostic A/B).
    extern bool vrEyeObjectMotionVectorsOff;
    // Skip DeferredLights.RenderStencilLights for eye cameras only.
    // VLSRP stencil-deferred punctual lights (stage spot / point
    // volumes) compute their specular from the rendering camera via
    // _CameraDepthTexture reconstruction — the only view-dependent
    // add the .131–.134 Campus-global zeroes never reached. Diagnostic
    // A/B for the close-up mid-leg blob; default off. Menu row dropped
    // in .185 — config-only.
    extern bool vrEyeDeferredStencilOff;
    // Eyes-only bias added to `_MatCapParam.x` (toon band threshold).
    // Positive raises the threshold and shrinks the bright zone; 0 =
    // authored. Grip / monitor unchanged. Restored to the menu in .185
    // as 「人物阴影带偏移」.
    extern float vrEyeShadeBandBias;

    // One Euro filter on the controller cursor UV. Locked to the .169
    // defaults; the Input tab was removed and file values are ignored
    // on load.
    constexpr float kDefaultVrPointerSmoothMinCutoff = 0.50F;
    // UV is 0-1, so speed is O(1)/s. Casiez's 0.007 is a pixel-space
    // number and would swallow a short swipe on this plane.
    // .169 hardware: 0.50 / 8.00 felt right; follow mattered more than extra
    // still-lock.
    constexpr float kDefaultVrPointerSmoothBeta = 8.00F;
    extern bool vrPointerSmoothEnabled;
    extern float vrPointerSmoothMinCutoff;
    extern float vrPointerSmoothBeta;

    // VR free camera. Y button function: 0 = switch character,
    // 1 = switch FOLLOW anchor bone. First person (`vrFpDirectionFollow`):
    // 0 = none (in cycle, yaw manual), 1 = turn, 2 = turn + tilt,
    // 3 = disabled (default; hidden from the X cycle and the mode combo).
    extern int vrCameraYButtonBone;
    extern int vrFpDirectionFollow;

    // Adjustable game-panel placement (right-A adjust mode). The offset is
    // the head-to-panel-centre vector in VIEW space; `pinned` re-anchors the
    // direction in stage space at runtime so it stops turning with the head.
    // Values are sanitized by the OpenXR worker on load (PanelPlacement.hpp
    // clamps) and written back when adjust mode exits.
    extern bool vrPanelCustomized;
    extern bool vrPanelPinned;
    extern float vrPanelOffsetX;
    extern float vrPanelOffsetY;
    extern float vrPanelOffsetZ;
    extern float vrPanelWidth;

    void ResetVrEyeAaToBaseline();
    void ResetVrPointerSettings();
    void ClampVrEyeAaSettings();
    void ClampVrPointerSettings();
    extern bool lazyInit;
    extern bool replaceFont;
    extern bool replaceTexture;
    extern bool forceExportResource;
    extern int gameOrientation;
    extern bool textTest;
    extern bool useMasterTrans;
    extern bool dumpText;
    extern bool dumpRuntimeTexture;
    extern bool enableFreeCamera;
    extern int targetFrameRate;
    extern bool unlockAllLive;
    extern bool unlockAllLiveCostume;

    extern bool enableLiveCustomeDress;
    extern std::string liveCustomeHeadId;
    extern std::string liveCustomeCostumeId;

    extern bool loginAsIOS;

    extern bool useCustomeGraphicSettings;
    extern float renderScale;
    extern int qualitySettingsLevel;
    extern int volumeIndex;
    extern int maxBufferPixel;

    extern int reflectionQualityLevel;
    extern int lodQualityLevel;

    extern bool enableBreastParam;
    extern float bDamping;
    extern float bStiffness;
    extern float bSpring;
    extern float bPendulum;
    extern float bPendulumRange;
    extern float bAverage;
    extern float bRootWeight;
    extern bool bUseArmCorrection;
    extern bool bUseScale;
    extern float bScale;
    extern bool bUseLimit;
    extern float bLimitXx;
    extern float bLimitXy;
    extern float bLimitYx;
    extern float bLimitYy;
    extern float bLimitZx;
    extern float bLimitZy;

    extern bool dmmUnlockSize;

    enum class ConfigLoadPurpose { LocalifyReload, StartupMigration };
    void LoadConfig(const std::string& configStr);
    void LoadConfig(const std::string& configStr,
        ConfigLoadPurpose purpose);
    void SaveConfig(const std::string& configPath);
    void LoadVrConfig(
        const std::filesystem::path& configPath,
        const std::filesystem::path& legacyConfigPath);
    void SaveVrConfig(const std::filesystem::path& configPath);
}
