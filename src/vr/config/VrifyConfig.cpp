#include <string>
#include "VrifyConfig.hpp"
#include "nlohmann/json.hpp"
#include "GakumasLocalify/Log.h"
#include <algorithm>
#include <cmath>
#include <thread>
#include <fstream>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace GakumasLocal::Config {
    bool isConfigInit = false;

    bool dbgMode = false;
    bool vrDiagnosticsEnabled = false;
    bool vrRuntimeStartupEnabled = false;
    bool vrDiagnosticsStartupEnabled = false;
    bool enabled = true;
    bool vrLocalizeText = false;
    int vrMenuLanguage = kDefaultVrMenuLanguage;
    bool vrEnabled = false;
    bool vrNativeOnly = true;
    bool vrHeadPoseEnabled = false;
    float vrWorldScale = 1.0F;
    bool vrStereoEnabled = false;
    float vrStereoRenderScale = 1.0F;
    bool vrStereoLandscapeOnly = false;
    float vrEyeOutlineWidth = kDefaultVrEyeOutlineWidth;
    bool vrBloomFollowSourceCamera = false;
    int vrEyeAaMode = kDefaultVrEyeAaMode;
    int vrEyeTaaQuality = kDefaultVrEyeTaaQuality;
    float vrEyeTaaFrameInfluence = kDefaultVrEyeTaaFrameInfluence;
    float vrEyeTaaJitterScale = kDefaultVrEyeTaaJitterScale;
    float vrEyeTaaMipBias = kDefaultVrEyeTaaMipBias;
    float vrEyeTaaVarianceClamp = kDefaultVrEyeTaaVarianceClamp;
    float vrEyeTaaSharpen = kDefaultVrEyeTaaSharpen;
    int vrEyeSmaaQuality = 2;
    bool vrActorShadowSourceAnchor = false;
    bool vrActorToonSourceAnchor = false;
    int vrToonFollowRef = kDefaultVrToonFollowRef;
    bool vrVolumeSourceAnchor = true;
    bool vrDisableSourceCamera = false;
    bool vrSourceOffResetPerFrame = false;
    bool vrEyeJumpFloodTemporalOff = false;
    bool vrVlMotionBlurOff = true;
    bool vrEyeAsMainCamera = false;
    bool vrEyeObjectMotionVectorsOff = false;
    bool vrEyeDeferredStencilOff = false;
    float vrEyeShadeBandBias = 0.0F;
    bool vrSourceCameraTiny = false;
    bool vrGripPanelTransparent = false;
    bool vrHideUiTextureOverlay = true;
    bool vrHandGlowSticks = false;
    bool vrPointerSmoothEnabled = true;
    float vrPointerSmoothMinCutoff = kDefaultVrPointerSmoothMinCutoff;
    float vrPointerSmoothBeta = kDefaultVrPointerSmoothBeta;
    int vrCameraYButtonBone = 0;
    int vrFpDirectionFollow = 3;
    bool vrPanelCustomized = false;
    bool vrPanelPinned = false;
    float vrPanelOffsetX = 0.0F;
    float vrPanelOffsetY = -0.2F;
    float vrPanelOffsetZ = -2.0F;
    float vrPanelWidth = 1.2F;

    void ResetVrEyeAaToBaseline() {
        vrEyeAaMode = kDefaultVrEyeAaMode;
        vrEyeTaaQuality = kDefaultVrEyeTaaQuality;
        vrEyeTaaFrameInfluence = kDefaultVrEyeTaaFrameInfluence;
        vrEyeTaaJitterScale = kDefaultVrEyeTaaJitterScale;
        vrEyeTaaMipBias = kDefaultVrEyeTaaMipBias;
        vrEyeTaaVarianceClamp = kDefaultVrEyeTaaVarianceClamp;
        vrEyeTaaSharpen = kDefaultVrEyeTaaSharpen;
        vrEyeSmaaQuality = 2;
    }

    void ClampVrEyeAaSettings() {
        const auto clampInt = [](int value, int lo, int hi) {
            return value < lo ? lo : (value > hi ? hi : value);
        };
        const auto clampFloat = [](float value, float lo, float hi, float fallback) {
            if (!std::isfinite(value)) {
                return fallback;
            }
            return std::clamp(value, lo, hi);
        };
        // Stored IDs are stable: 0 inherit, 1 TAA, 2 SMAA, 3 none,
        // 4 SMAA T2x, 5 TSCMAA.
        vrEyeAaMode = clampInt(vrEyeAaMode, 0, 5);
        vrEyeTaaQuality = clampInt(vrEyeTaaQuality, 0, 4);
        vrEyeSmaaQuality = clampInt(vrEyeSmaaQuality, 0, 2);
        vrStereoRenderScale =
            clampFloat(vrStereoRenderScale, 0.25F, 1.5F, 1.0F);
        vrEyeOutlineWidth = clampFloat(
            vrEyeOutlineWidth, 0.0F, 1.0F, kDefaultVrEyeOutlineWidth);
        vrEyeTaaFrameInfluence = clampFloat(
            vrEyeTaaFrameInfluence,
            0.0F,
            1.0F,
            kDefaultVrEyeTaaFrameInfluence);
        vrEyeTaaJitterScale = clampFloat(
            vrEyeTaaJitterScale, 0.0F, 2.0F, kDefaultVrEyeTaaJitterScale);
        vrEyeTaaMipBias = clampFloat(
            vrEyeTaaMipBias, -2.0F, 2.0F, kDefaultVrEyeTaaMipBias);
        vrEyeTaaVarianceClamp = clampFloat(
            vrEyeTaaVarianceClamp,
            0.0F,
            4.0F,
            kDefaultVrEyeTaaVarianceClamp);
        vrEyeTaaSharpen = clampFloat(
            vrEyeTaaSharpen, 0.0F, 1.0F, kDefaultVrEyeTaaSharpen);
        vrEyeShadeBandBias =
            clampFloat(vrEyeShadeBandBias, -0.5F, 0.5F, 0.0F);
    }

    void ResetVrPointerSettings() {
        vrPointerSmoothEnabled = true;
        vrPointerSmoothMinCutoff = kDefaultVrPointerSmoothMinCutoff;
        vrPointerSmoothBeta = kDefaultVrPointerSmoothBeta;
    }

    void ClampVrPointerSettings() {
        const auto clampFloat = [](float value, float lo, float hi, float fallback) {
            if (!std::isfinite(value)) {
                return fallback;
            }
            return std::clamp(value, lo, hi);
        };
        vrPointerSmoothMinCutoff = clampFloat(
            vrPointerSmoothMinCutoff,
            0.15F,
            3.0F,
            kDefaultVrPointerSmoothMinCutoff);
        vrPointerSmoothBeta = clampFloat(
            vrPointerSmoothBeta, 0.0F, 12.0F, kDefaultVrPointerSmoothBeta);
        const auto clampInt = [](int value, int lo, int hi) {
            return value < lo ? lo : (value > hi ? hi : value);
        };
        vrCameraYButtonBone = clampInt(vrCameraYButtonBone, 0, 1);
        vrFpDirectionFollow = clampInt(vrFpDirectionFollow, 0, 3);
        vrMenuLanguage = clampInt(
            vrMenuLanguage, kVrMenuLanguageSystem, kVrMenuLanguageEn);
    }

    namespace {
        nlohmann::json g_vrConfigDocument = nlohmann::json::object();

        void ResetVrConfigFailClosed() {
            vrDiagnosticsEnabled = false;
            vrRuntimeStartupEnabled = false;
            vrDiagnosticsStartupEnabled = false;
            vrEnabled = false;
            vrLocalizeText = false;
            vrMenuLanguage = kDefaultVrMenuLanguage;
            vrNativeOnly = true;
            vrHeadPoseEnabled = false;
            vrWorldScale = 1.0F;
            vrStereoEnabled = false;
            vrStereoRenderScale = 1.0F;
            vrStereoLandscapeOnly = false;
            vrEyeOutlineWidth = kDefaultVrEyeOutlineWidth;
            vrBloomFollowSourceCamera = false;
            ResetVrEyeAaToBaseline();
            vrActorShadowSourceAnchor = false;
            vrActorToonSourceAnchor = false;
            vrToonFollowRef = kDefaultVrToonFollowRef;
            vrVolumeSourceAnchor = true;
            vrDisableSourceCamera = false;
            vrSourceOffResetPerFrame = false;
            vrEyeJumpFloodTemporalOff = false;
            vrVlMotionBlurOff = true;
            vrEyeAsMainCamera = false;
            vrEyeObjectMotionVectorsOff = false;
            vrEyeDeferredStencilOff = false;
            vrEyeShadeBandBias = 0.0F;
            vrSourceCameraTiny = false;
            vrGripPanelTransparent = false;
            vrHideUiTextureOverlay = true;
            vrHandGlowSticks = false;
            ResetVrPointerSettings();
            vrPanelCustomized = false;
            vrPanelPinned = false;
            vrPanelOffsetX = 0.0F;
            vrPanelOffsetY = -0.2F;
            vrPanelOffsetZ = -2.0F;
            vrPanelWidth = 1.2F;
        }

        void ApplyVrConfig(const nlohmann::json& config) {
#define GetVrConfigItem(name) if (config.contains(#name)) name = config.at(#name).get<decltype(name)>()
            if (config.contains("vrDiagnosticsEnabled")) {
                vrDiagnosticsEnabled = config.at("vrDiagnosticsEnabled").get<bool>();
            } else if (config.contains("vrDebugEnabled")) {
                vrDiagnosticsEnabled = config.at("vrDebugEnabled").get<bool>();
            }
            GetVrConfigItem(vrEnabled);
            GetVrConfigItem(vrLocalizeText);
            GetVrConfigItem(vrMenuLanguage);
            if (config.contains("vrNativeOnly")) {
                vrNativeOnly = config.at("vrNativeOnly").get<bool>();
            } else if (config.contains("vrProbeOnly")) {
                vrNativeOnly = config.at("vrProbeOnly").get<bool>();
            }
            GetVrConfigItem(vrHeadPoseEnabled);
            GetVrConfigItem(vrWorldScale);
            GetVrConfigItem(vrStereoEnabled);
            GetVrConfigItem(vrStereoRenderScale);
            GetVrConfigItem(vrStereoLandscapeOnly);
            GetVrConfigItem(vrEyeOutlineWidth);
            GetVrConfigItem(vrBloomFollowSourceCamera);
            GetVrConfigItem(vrEyeAaMode);
            GetVrConfigItem(vrEyeTaaQuality);
            GetVrConfigItem(vrEyeTaaFrameInfluence);
            GetVrConfigItem(vrEyeTaaJitterScale);
            GetVrConfigItem(vrEyeTaaMipBias);
            GetVrConfigItem(vrEyeTaaVarianceClamp);
            GetVrConfigItem(vrEyeTaaSharpen);
            GetVrConfigItem(vrEyeSmaaQuality);
            GetVrConfigItem(vrActorShadowSourceAnchor);
            GetVrConfigItem(vrActorToonSourceAnchor);
            GetVrConfigItem(vrToonFollowRef);
            GetVrConfigItem(vrVolumeSourceAnchor);
            GetVrConfigItem(vrDisableSourceCamera);
            GetVrConfigItem(vrSourceOffResetPerFrame);
            GetVrConfigItem(vrEyeJumpFloodTemporalOff);
            GetVrConfigItem(vrVlMotionBlurOff);
            GetVrConfigItem(vrEyeAsMainCamera);
            GetVrConfigItem(vrEyeObjectMotionVectorsOff);
            GetVrConfigItem(vrEyeDeferredStencilOff);
            GetVrConfigItem(vrEyeShadeBandBias);
            GetVrConfigItem(vrSourceCameraTiny);
            GetVrConfigItem(vrGripPanelTransparent);
            GetVrConfigItem(vrHideUiTextureOverlay);
            GetVrConfigItem(vrHandGlowSticks);
            GetVrConfigItem(vrCameraYButtonBone);
            GetVrConfigItem(vrFpDirectionFollow);
            GetVrConfigItem(vrPanelCustomized);
            GetVrConfigItem(vrPanelPinned);
            GetVrConfigItem(vrPanelOffsetX);
            GetVrConfigItem(vrPanelOffsetY);
            GetVrConfigItem(vrPanelOffsetZ);
            GetVrConfigItem(vrPanelWidth);
#undef GetVrConfigItem
            if (vrToonFollowRef < 0 || vrToonFollowRef > 2) {
                vrToonFollowRef = kDefaultVrToonFollowRef;
            }
            ClampVrEyeAaSettings();
            ClampVrPointerSettings();
            ResetVrPointerSettings();
        }

        nlohmann::json BuildVrConfig() {
            auto config = g_vrConfigDocument.is_object()
                ? g_vrConfigDocument
                : nlohmann::json::object();
            config.erase("vrDebugEnabled");
            config.erase("vrProbeOnly");
            config["schemaVersion"] = 1;
#define SetVrConfigItem(name) config[#name] = name
            SetVrConfigItem(vrDiagnosticsEnabled);
            SetVrConfigItem(vrEnabled);
            SetVrConfigItem(vrLocalizeText);
            SetVrConfigItem(vrMenuLanguage);
            SetVrConfigItem(vrNativeOnly);
            SetVrConfigItem(vrHeadPoseEnabled);
            SetVrConfigItem(vrWorldScale);
            SetVrConfigItem(vrStereoEnabled);
            SetVrConfigItem(vrStereoRenderScale);
            SetVrConfigItem(vrStereoLandscapeOnly);
            ClampVrEyeAaSettings();
            SetVrConfigItem(vrEyeOutlineWidth);
            SetVrConfigItem(vrBloomFollowSourceCamera);
            SetVrConfigItem(vrEyeAaMode);
            SetVrConfigItem(vrEyeTaaQuality);
            SetVrConfigItem(vrEyeTaaFrameInfluence);
            SetVrConfigItem(vrEyeTaaJitterScale);
            SetVrConfigItem(vrEyeTaaMipBias);
            SetVrConfigItem(vrEyeTaaVarianceClamp);
            SetVrConfigItem(vrEyeTaaSharpen);
            SetVrConfigItem(vrEyeSmaaQuality);
            SetVrConfigItem(vrActorShadowSourceAnchor);
            SetVrConfigItem(vrActorToonSourceAnchor);
            SetVrConfigItem(vrToonFollowRef);
            SetVrConfigItem(vrVolumeSourceAnchor);
            SetVrConfigItem(vrDisableSourceCamera);
            SetVrConfigItem(vrSourceOffResetPerFrame);
            SetVrConfigItem(vrEyeJumpFloodTemporalOff);
            SetVrConfigItem(vrVlMotionBlurOff);
            SetVrConfigItem(vrEyeAsMainCamera);
            SetVrConfigItem(vrEyeObjectMotionVectorsOff);
            SetVrConfigItem(vrEyeDeferredStencilOff);
            SetVrConfigItem(vrEyeShadeBandBias);
            SetVrConfigItem(vrSourceCameraTiny);
            SetVrConfigItem(vrGripPanelTransparent);
            SetVrConfigItem(vrHideUiTextureOverlay);
            SetVrConfigItem(vrHandGlowSticks);
            ClampVrPointerSettings();
            SetVrConfigItem(vrPointerSmoothEnabled);
            SetVrConfigItem(vrPointerSmoothMinCutoff);
            SetVrConfigItem(vrPointerSmoothBeta);
            SetVrConfigItem(vrCameraYButtonBone);
            SetVrConfigItem(vrFpDirectionFollow);
            SetVrConfigItem(vrPanelCustomized);
            SetVrConfigItem(vrPanelPinned);
            SetVrConfigItem(vrPanelOffsetX);
            SetVrConfigItem(vrPanelOffsetY);
            SetVrConfigItem(vrPanelOffsetZ);
            SetVrConfigItem(vrPanelWidth);
#undef SetVrConfigItem
            return config;
        }
    }
    bool lazyInit = true;
    bool replaceFont = true;
    bool replaceTexture = true;
    bool forceExportResource = false;
    bool textTest = false;
    bool useMasterTrans = true;
    int gameOrientation = 0;
    bool dumpText = false;
    bool dumpRuntimeTexture = false;
    bool enableFreeCamera = false;
    int targetFrameRate = 0;
    bool unlockAllLive = false;
    bool unlockAllLiveCostume = false;

    bool enableLiveCustomeDress = false;
    std::string liveCustomeHeadId = "";
    std::string liveCustomeCostumeId = "";

    bool loginAsIOS = false;

    bool useCustomeGraphicSettings = false;
    float renderScale = 0.77f;
    int qualitySettingsLevel = 3;
    int volumeIndex = 3;
    int maxBufferPixel = 3384;
    int reflectionQualityLevel = 4;
    int lodQualityLevel = 4;

    bool enableBreastParam = false;
    float bDamping = 0.33f;
    float bStiffness = 0.08f;
    float bSpring = 1.0f;
    float bPendulum = 0.055f;
    float bPendulumRange = 0.15f;
    float bAverage = 0.20f;
    float bRootWeight = 0.5f;
    bool bUseArmCorrection = true;
    bool bUseScale = false;
    float bScale = 1.0f;
    bool bUseLimit = true;
    float bLimitXx = 1.0f;
    float bLimitXy = 1.0f;
    float bLimitYx = 1.0f;
    float bLimitYy = 1.0f;
    float bLimitZx = 1.0f;
    float bLimitZy = 1.0f;

    bool dmmUnlockSize = false;

    void LoadConfig(const std::string& configStr) {
        LoadConfig(configStr, ConfigLoadPurpose::LocalifyReload);
    }

    void LoadConfig(const std::string& configStr, ConfigLoadPurpose purpose) {
        isConfigInit = false;
        try {
            const auto config = nlohmann::json::parse(configStr);
            if (!config.is_object()) {
                throw std::runtime_error("Localify config root must be an object");
            }

            #define GetConfigItem(name) if (config.contains(#name)) name = config[#name]

            GetConfigItem(dbgMode);
            GetConfigItem(enabled);
            // Only bootstrap may import legacy VR fields. Ctrl+U reload must
            // never write (or clamp/reset) any state owned by the VR menu.
            if (purpose == ConfigLoadPurpose::StartupMigration) {
                ApplyVrConfig(config);
            }
            GetConfigItem(lazyInit);
            GetConfigItem(replaceFont);
            GetConfigItem(replaceTexture);
            GetConfigItem(forceExportResource);
            GetConfigItem(gameOrientation);
            GetConfigItem(textTest);
            GetConfigItem(useMasterTrans);
            GetConfigItem(dumpText);
            GetConfigItem(dumpRuntimeTexture);
            GetConfigItem(targetFrameRate);
            GetConfigItem(enableFreeCamera);
            GetConfigItem(unlockAllLive);
            GetConfigItem(unlockAllLiveCostume);
            GetConfigItem(enableLiveCustomeDress);
            GetConfigItem(liveCustomeHeadId);
            GetConfigItem(liveCustomeCostumeId);
            GetConfigItem(loginAsIOS);
            GetConfigItem(useCustomeGraphicSettings);
            GetConfigItem(renderScale);
            GetConfigItem(qualitySettingsLevel);
            GetConfigItem(volumeIndex);
            GetConfigItem(maxBufferPixel);
            GetConfigItem(reflectionQualityLevel);
            GetConfigItem(lodQualityLevel);
            GetConfigItem(enableBreastParam);
            GetConfigItem(bDamping);
            GetConfigItem(bStiffness);
            GetConfigItem(bSpring);
            GetConfigItem(bPendulum);
            GetConfigItem(bPendulumRange);
            GetConfigItem(bAverage);
            GetConfigItem(bRootWeight);
            GetConfigItem(bUseArmCorrection);
            GetConfigItem(bUseScale);
            GetConfigItem(bScale);
            GetConfigItem(bUseLimit);
            GetConfigItem(bLimitXx);
            GetConfigItem(bLimitXy);
            GetConfigItem(bLimitYx);
            GetConfigItem(bLimitYy);
            GetConfigItem(bLimitZx);
            GetConfigItem(bLimitZy);
            GetConfigItem(dmmUnlockSize);
            isConfigInit = true;
        }
        catch (std::exception& e) {
            dbgMode = false;
            enabled = false;
            // A startup failure still fails closed. A desktop reload failure
            // cannot revoke the running OpenXR session's frozen decisions.
            if (purpose == ConfigLoadPurpose::StartupMigration) {
                ResetVrConfigFailClosed();
            }
            isConfigInit = true;
            Log::ErrorFmt("LoadConfig error: %s", e.what());
        }
    }

    void SaveConfig(const std::string& configPath) {
        try {
            nlohmann::json config = nlohmann::json::object();
            if (std::filesystem::exists(configPath)) {
                std::ifstream in(configPath);
                in >> config;
                if (!config.is_object()) {
                    throw std::runtime_error("Localify config root must be an object");
                }
            }
            for (auto it = config.begin(); it != config.end(); ) {
                if (it.key().rfind("vr", 0) == 0) {
                    it = config.erase(it);
                } else {
                    ++it;
                }
            }

            #define SetConfigItem(name) config[#name] = name

            SetConfigItem(dbgMode);
            SetConfigItem(enabled);
            SetConfigItem(lazyInit);
            SetConfigItem(replaceFont);
            SetConfigItem(replaceTexture);
            SetConfigItem(forceExportResource);
            SetConfigItem(gameOrientation);
            SetConfigItem(textTest);
            SetConfigItem(useMasterTrans);
            SetConfigItem(dumpText);
            SetConfigItem(dumpRuntimeTexture);
            SetConfigItem(targetFrameRate);
            SetConfigItem(enableFreeCamera);
            SetConfigItem(unlockAllLive);
            SetConfigItem(unlockAllLiveCostume);
            SetConfigItem(enableLiveCustomeDress);
            SetConfigItem(liveCustomeHeadId);
            SetConfigItem(liveCustomeCostumeId);
            SetConfigItem(loginAsIOS);
            SetConfigItem(useCustomeGraphicSettings);
            SetConfigItem(renderScale);
            SetConfigItem(qualitySettingsLevel);
            SetConfigItem(volumeIndex);
            SetConfigItem(maxBufferPixel);
            SetConfigItem(reflectionQualityLevel);
            SetConfigItem(lodQualityLevel);
            SetConfigItem(enableBreastParam);
            SetConfigItem(bDamping);
            SetConfigItem(bStiffness);
            SetConfigItem(bSpring);
            SetConfigItem(bPendulum);
            SetConfigItem(bPendulumRange);
            SetConfigItem(bAverage);
            SetConfigItem(bRootWeight);
            SetConfigItem(bUseArmCorrection);
            SetConfigItem(bUseScale);
            SetConfigItem(bScale);
            SetConfigItem(bUseLimit);
            SetConfigItem(bLimitXx);
            SetConfigItem(bLimitXy);
            SetConfigItem(bLimitYx);
            SetConfigItem(bLimitYy);
            SetConfigItem(bLimitZx);
            SetConfigItem(bLimitZy);
            SetConfigItem(dmmUnlockSize);

            std::ofstream out(configPath);
            if (!out) {
                Log::ErrorFmt("SaveConfig error: Cannot open file: %s", configPath.c_str());
                return;
            }
            out << config.dump(4);
			Log::Info("SaveConfig success");
        }
        catch (std::exception& e) {
            Log::ErrorFmt("SaveConfig error: %s", e.what());
        }
    }

    void SaveVrConfig(const std::filesystem::path& configPath) {
        try {
            if (!configPath.parent_path().empty()) {
                std::filesystem::create_directories(configPath.parent_path());
            }
            const auto config = BuildVrConfig();
            auto tempPath = configPath;
            tempPath += L".tmp";
            {
                std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
                if (!out) {
                    Log::ErrorFmt(
                        "SaveVrConfig error: Cannot open file: %s",
                        tempPath.string().c_str());
                    return;
                }
                out << config.dump(4);
                out.flush();
                if (!out) {
                    Log::ErrorFmt(
                        "SaveVrConfig error: Cannot write file: %s",
                        tempPath.string().c_str());
                    return;
                }
            }
#ifdef _WIN32
            if (!MoveFileExW(
                    tempPath.c_str(),
                    configPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                const auto error = GetLastError();
                std::filesystem::remove(tempPath);
                Log::ErrorFmt("SaveVrConfig replace error: %lu", error);
                return;
            }
#else
            std::filesystem::rename(tempPath, configPath);
#endif
            g_vrConfigDocument = config;
            Log::Info("SaveVrConfig success");
        }
        catch (const std::exception& e) {
            Log::ErrorFmt("SaveVrConfig error: %s", e.what());
        }
    }

    void LoadVrConfig(
        const std::filesystem::path& configPath,
        const std::filesystem::path& legacyConfigPath) {
        if (!std::filesystem::exists(configPath)) {
            g_vrConfigDocument = nlohmann::json::object();
            SaveVrConfig(configPath);
            Log::InfoFmt(
                "Migrated legacy VR settings from %s to %s",
                legacyConfigPath.string().c_str(),
                configPath.string().c_str());
            return;
        }

        try {
            std::ifstream in(configPath, std::ios::binary);
            if (!in) {
                throw std::runtime_error("cannot open dedicated VR config");
            }
            nlohmann::json config;
            in >> config;
            if (!config.is_object()) {
                throw std::runtime_error("dedicated VR config root must be an object");
            }
            // A present dedicated file is authoritative. Start from safe VR
            // defaults so omitted keys never bleed through from the legacy
            // localizationConfig.json loaded immediately before this call.
            ResetVrConfigFailClosed();
            ApplyVrConfig(config);
            g_vrConfigDocument = std::move(config);
            Log::Info("LoadVrConfig success");
        }
        catch (const std::exception& e) {
            ResetVrConfigFailClosed();
            g_vrConfigDocument = nlohmann::json::object();
            Log::ErrorFmt("LoadVrConfig error (VR disabled): %s", e.what());
        }
    }
}
