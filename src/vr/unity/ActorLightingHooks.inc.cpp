// VR-owned integration, included by the patched upstream Hook translation unit.
    // Two independent paths make actor lighting follow the head under 6DoF:
    //
    //   VL.Rendering.ActorShadowPass          projected actor shadow map
    //     (VLActorShadowGroup blobs on fences, actor self-shadow silhouette)
    //   Campus.Rendering.CampusActorParameterPass  MatCap main light
    //     (UpdateActorCommand(cmd, stack, cameraTransform) + CalcLightVector,
    //      writes _MatCapMainLight — this is what shades hair and accessories
    //      onto the body, and it is not a shadow map at all)
    //
    // Both derive their direction from the rendering camera, so both are
    // bracketed the same way: the pass still runs the game's own algorithm,
    // only the camera transform it reads is pointed back at the pre-HMD
    // Cinemachine pose for the duration of the call. ScriptableRenderContext
    // is a single-pointer struct passed by value; RenderingData is byref.
    template <typename Invoke>
    void RunWithActorShadowAnchor(
        [[maybe_unused]] const char* passName,
        Invoke&& invokeOriginal) {
#ifdef GKMS_WINDOWS
        bool anchored = false;
        try {
            anchored =
                unityStereoRenderer.BeginActorShadowSourceAnchor(passName);
        } catch (...) {
            ReportVrUnityHookException();
        }
        if (anchored) {
            try {
                invokeOriginal();
            } catch (...) {
                unityStereoRenderer.EndActorShadowSourceAnchor();
                throw;
            }
            try {
                unityStereoRenderer.EndActorShadowSourceAnchor();
            } catch (...) {
                ReportVrUnityHookException();
            }
            return;
        }
#endif
        invokeOriginal();
    }

    // ActorShadowPass renders its private RTHandle from the current camera's
    // CullingResults. The live DrawShadow body passes renderingData.cullResults
    // straight to ScriptableRenderContext.DrawRenderers, so the late shot-view
    // patch cannot make caster membership identical between eyes. Cache the
    // left pass data after Setup and let the right Execute reuse the completed
    // left map. This is baseline stereo behavior, independent of the optional
    // source-camera anchor: lock off generates once from the live left eye;
    // lock on generates once from the locked source pose. OnCameraCleanup only
    // disables the receiver keyword; it does not release or clear the RTHandle,
    // so the map remains valid for the next eye.
    void ResetActorShadowLeftReuse() noexcept {
        actorShadowStereoReuse.leftPass = nullptr;
        actorShadowStereoReuse.leftReady = false;
        actorShadowStereoReuse.leftSupportsShadow = false;
        actorShadowStereoReuse.leftDistanceDataReady = false;
    }

    void PrepareActorShadowFeatureCall() noexcept {
        const std::string_view role = unityStereoRenderer.ClassifyCamera(
            unityStereoRenderer.CurrentCamera());
        if (role == "left") {
            ResetActorShadowLeftReuse();
        }
    }

    void CaptureActorShadowLeftData(void* feature) noexcept {
        auto& reuse = actorShadowStereoReuse;
        const std::string_view role = unityStereoRenderer.ClassifyCamera(
            unityStereoRenderer.CurrentCamera());
        if (role != "left" || feature == nullptr || reuse.featurePass == nullptr ||
            reuse.shadowData == nullptr || reuse.shadowBias == nullptr ||
            reuse.supportsShadow == nullptr || reuse.dataSize == 0U ||
            reuse.dataSize > reuse.leftData.size()) {
            return;
        }
        void* pass = Il2cppUtils::ClassGetFieldValue<void*>(
            feature, reuse.featurePass);
        if (pass == nullptr) {
            return;
        }
        // `_supportsShadow` is the left eye's distance/frustum decision, not a
        // validity bit for ActorShadowData. ActorShadowPass is still enqueued
        // and DrawShadow still publishes its receiver constants when this is
        // false. Dropping the capture here made the right eye fall back to an
        // independent UpdateShadowData decision exactly at the fade boundary:
        // left remained at zero while right jumped back to one. Capture and
        // reuse both outcomes so "no shadow" is stereo state too.
        reuse.leftSupportsShadow = Il2cppUtils::ClassGetFieldValue<bool>(
            pass, reuse.supportsShadow);
        std::memcpy(
            reuse.leftData.data(),
            static_cast<const std::byte*>(pass) + reuse.shadowData->offset,
            reuse.dataSize);
        reuse.leftPass = pass;
        reuse.leftReady = true;
        ++reuse.leftCaptures;
        reuse.leftDistanceDataReady = false;
        const auto readDataFloat = [&reuse](const UnityResolve::Field* field,
                                            float& value) {
            const std::int32_t offset = field != nullptr
                ? field->offset - reuse.dataFieldHeader : -1;
            if (offset < 0 ||
                static_cast<std::size_t>(offset) + sizeof(float) >
                    reuse.dataSize) {
                return false;
            }
            std::memcpy(&value,
                        reuse.leftData.data() + offset,
                        sizeof(value));
            return true;
        };
        reuse.leftDistanceDataReady =
            readDataFloat(reuse.startDistance2, reuse.leftStartDistance2) &&
            readDataFloat(reuse.endDistance2, reuse.leftEndDistance2) &&
            readDataFloat(reuse.fade, reuse.leftFade) &&
            readDataFloat(reuse.strength, reuse.leftStrength);
        if (reuse.leftCaptures <= 8U || reuse.leftCaptures % 600U == 0U) {
            std::ostringstream line;
            line << "[VR][shadow] ACTOR_SHADOW_LEFT_CAPTURE role=left"
                 << " pass=" << pass
                 << " bytes=" << reuse.dataSize
                 << " supports=" << (reuse.leftSupportsShadow ? 1 : 0)
                 << " anchor="
                 << (Config::vrActorShadowSourceAnchor ? 1 : 0)
                 << " leftCaptures=" << reuse.leftCaptures;
            if (reuse.leftDistanceDataReady) {
                line << " startDistance2=" << reuse.leftStartDistance2
                     << " endDistance2=" << reuse.leftEndDistance2
                     << " fade=" << reuse.leftFade
                     << " strength=" << reuse.leftStrength;
            }
            static_cast<void>(gakumas::vr::WriteVrLog(line.str()));
        }
    }

    bool ReuseLeftActorShadowForRight(void* pass, void* cmd) noexcept {
        auto& reuse = actorShadowStereoReuse;
        const std::string_view role = unityStereoRenderer.ClassifyCamera(
            unityStereoRenderer.CurrentCamera());
        if (role != "right" || !reuse.leftReady || pass == nullptr ||
            pass != reuse.leftPass ||
            cmd == nullptr || reuse.shadowData == nullptr ||
            reuse.setupReceiver == nullptr || reuse.setKeyword == nullptr ||
            reuse.dataSize == 0U || reuse.dataSize > reuse.leftData.size()) {
            return false;
        }
        Il2cppString* keyword = nullptr;
        UnityResolve::Invoke<void>(
            "il2cpp_field_static_get_value",
            reuse.actorShadowsKeyword->address, &keyword);
        if (keyword == nullptr) {
            return false;
        }
        std::memcpy(
            static_cast<std::byte*>(pass) + reuse.shadowData->offset,
            reuse.leftData.data(), reuse.dataSize);
        Il2cppUtils::ClassSetFieldValue(
            pass, reuse.supportsShadow, reuse.leftSupportsShadow);
        // Match DrawShadow's proven order: enable _ACTOR_SHADOWS, then publish
        // receiver matrices/params. Execute will still bind the RTHandle as the
        // global shadow texture after this hook returns.
        reuse.setKeyword->Invoke<void>(cmd, keyword, true);
        reuse.setupReceiver->Invoke<void>(pass, cmd);
        ++reuse.rightReuses;
        if (reuse.rightReuses <= 8U || reuse.rightReuses % 600U == 0U) {
            std::ostringstream line;
            line << "[VR][shadow] ACTOR_SHADOW_LEFT_REUSE role=right"
                 << " pass=" << pass
                 << " bytes=" << reuse.dataSize
                 << " leftSupports=" << (reuse.leftSupportsShadow ? 1 : 0)
                 << " anchor="
                 << (Config::vrActorShadowSourceAnchor ? 1 : 0)
                 << " leftCaptures=" << reuse.leftCaptures
                 << " rightReuses=" << reuse.rightReuses;
            if (reuse.leftDistanceDataReady) {
                line << " startDistance2=" << reuse.leftStartDistance2
                     << " endDistance2=" << reuse.leftEndDistance2
                     << " fade=" << reuse.leftFade
                     << " strength=" << reuse.leftStrength;
            }
            static_cast<void>(gakumas::vr::WriteVrLog(line.str()));
        }
        return true;
    }

    bool CanReuseLeftActorShadowForRight(void* pass) noexcept {
        const auto& reuse = actorShadowStereoReuse;
        const std::string_view role = unityStereoRenderer.ClassifyCamera(
            unityStereoRenderer.CurrentCamera());
        return role == "right" && reuse.leftReady && pass != nullptr &&
            pass == reuse.leftPass &&
            reuse.dataSize != 0U;
    }

    DEFINE_HOOK(void, ActorShadowPass_Configure,
                (void* self, void* cmd, void* descriptor, void* method)) {
#ifdef GKMS_WINDOWS
        bool preserveLeftMap = false;
        try {
            preserveLeftMap = CanReuseLeftActorShadowForRight(self);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        ActorShadowPass_Configure_Orig(self, cmd, descriptor, method);
#ifdef GKMS_WINDOWS
        if (!preserveLeftMap) {
            return;
        }
        try {
            // ConfigureTarget still has to run for the right pass. Merely
            // skipping Configure was insufficient: ScriptableRenderPass keeps
            // m_ClearFlag on the pass object, so the left eye's All flag could
            // still make ScriptableRenderer clear the shared RT before right
            // Execute. Override the dump-proven base field after the original
            // Configure has refreshed the target. Use the exact public API;
            // the field readback both verifies it and provides a safe fallback
            // if runtime invocation ever stops matching this Unity build.
            auto& reuse = actorShadowStereoReuse;
            constexpr std::int32_t kClearFlagNone = 0;
            reuse.configureClear->RuntimeInvoke<void>(
                self, kClearFlagNone,
                UnityResolve::UnityType::Color(0.0F, 0.0F, 0.0F, 0.0F));
            std::int32_t clearFlag =
                Il2cppUtils::ClassGetFieldValue<std::int32_t>(
                    self, reuse.clearFlag);
            if (clearFlag != kClearFlagNone) {
                Il2cppUtils::ClassSetFieldValue<std::int32_t>(
                    self, reuse.clearFlag, kClearFlagNone);
                clearFlag = Il2cppUtils::ClassGetFieldValue<std::int32_t>(
                    self, reuse.clearFlag);
            }
            ++reuse.rightClearPreserves;
            if (reuse.rightClearPreserves <= 8U ||
                reuse.rightClearPreserves % 600U == 0U) {
                std::ostringstream line;
                line << "[VR][shadow] ACTOR_SHADOW_RIGHT_PRESERVE"
                     << " role=right clearFlag=" << clearFlag
                     << " clearOffset=" << reuse.clearFlag->offset
                     << " preserves=" << reuse.rightClearPreserves;
                static_cast<void>(gakumas::vr::WriteVrLog(line.str()));
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, ActorShadowPass_DrawShadow,
                (void* self, void* cmd, void* context, void* renderingData,
                 void* method)) {
#ifdef GKMS_WINDOWS
        try {
            if (ReuseLeftActorShadowForRight(self, cmd)) {
                return;
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        ActorShadowPass_DrawShadow_Orig(
            self, cmd, context, renderingData, method);
#ifdef GKMS_WINDOWS
        try {
            LogFpHeadActorShadowTag(self);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, ActorShadowPass_Execute,
                (void* self, void* context, void* renderingData, void* method)) {
        // Execute still runs (census + DrawShadow), but the projected
        // matrix is already final here. Keep the transform-swap bracket
        // so any remaining camera-transform reads inside DrawShadow see
        // the shot; the cameraData view patch lives on AddRenderPasses.
        RunWithActorShadowAnchor("actor-shadow", [&] {
            ActorShadowPass_Execute_Orig(
                self, context, renderingData, method);
        });
    }

    // The un-inlinable ancestor of Setup/UpdateShadowData: AddRenderPasses
    // is virtual (slot 7) and URP invokes it polymorphically while walking
    // the renderer-feature list, so LTCG cannot fold it into a caller.
    // .94/.95: Setup itself is inlined into this body (trampoline on the
    // Setup entry never fired); this is the only live hook point that
    // sees cameraData before UpdateShadowData writes _WorldToActorShadow.
    DEFINE_HOOK(void, DrawActorShadowPass_AddRenderPasses,
                (void* self, void* renderer, void* renderingData,
                 void* method)) {
        PrepareActorShadowFeatureCall();
        RunWithActorShadowAnchor("actor-shadow-add", [&] {
#ifdef GKMS_WINDOWS
            bool viewPatched = false;
            try {
                viewPatched = unityStereoRenderer.BeginActorShadowViewPatch(
                    renderingData);
            } catch (...) {
                ReportVrUnityHookException();
            }
            try {
                DrawActorShadowPass_AddRenderPasses_Orig(
                    self, renderer, renderingData, method);
            } catch (...) {
                if (viewPatched) {
                    unityStereoRenderer.EndActorShadowViewPatch(renderingData);
                }
                throw;
            }
            if (viewPatched) {
                try {
                    unityStereoRenderer.EndActorShadowViewPatch(renderingData);
                } catch (...) {
                    ReportVrUnityHookException();
                }
            }
            CaptureActorShadowLeftData(self);
#else
            DrawActorShadowPass_AddRenderPasses_Orig(
                self, renderer, renderingData, method);
#endif
        });
    }

    DEFINE_HOOK(void, CampusActorParameterPass_Execute,
                (void* self, void* context, void* renderingData, void* method)) {
#ifdef GKMS_WINDOWS
        // `.179`: rebuild the official complete _OutlineParam for the
        // current camera from this pass's live settings + focal curve. The
        // stored per-role snapshot is what the eye/Grip material writes put
        // back, replacing the `.176`-condemned fixed-w synthetic vector.
        if (Config::vrRuntimeStartupEnabled) {
            try {
                unityStereoRenderer.CaptureOfficialOutlineVector(self);
            } catch (...) {
                ReportVrUnityHookException();
            }
        }

#endif
        RunWithActorShadowAnchor("campus-actor-param", [&] {
            CampusActorParameterPass_Execute_Orig(
                self, context, renderingData, method);
#ifdef GKMS_WINDOWS
            // The pass just recorded the authored view-space MatCap lights
            // into this context. Rim compensate still needs the projected-
            // shadow bracket's saved head pose; authored rim (toon lock)
            // can run without that bracket.
            try {
                unityStereoRenderer.ApplyActorMatcapCompensation(
                    context, renderingData);
            } catch (...) {
                ReportVrUnityHookException();
            }
#endif
        });
    }

    DEFINE_HOOK(void, MobPenlight_UpdatePenlightParams,
                (void* self, void* context, void* camera, void* method)) {
        MobPenlight_UpdatePenlightParams_Orig(self, context, camera, method);
#ifdef GKMS_WINDOWS
        if (!Config::vrRuntimeStartupEnabled) {
            return;
        }
        try {
            gakumas::vr::AfterOfficialPenlightCamera(self);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, CrowdSystem_RenderCrowd,
                (void* self, void* commandBuffer, int eventType,
                 void* method)) {
        CrowdSystem_RenderCrowd_Orig(
            self, commandBuffer, eventType, method);
#ifdef GKMS_WINDOWS
        if (!Config::vrRuntimeStartupEnabled) {
            return;
        }
        // The original crowd was already drawn. Omit only our extra hands
        // from photo/source cameras; keep both HMD eyes and their colors intact.
        const bool photoGuard = gakumas::vr::LiveSourcePhotoProtectionActive();
        const bool eyeCamera = unityStereoRenderer.IsEyeCamera(unityStereoRenderer.CurrentCamera());
        static unsigned photoHandLogMask = 0;
        if (!photoGuard) photoHandLogMask = 0;
        const unsigned photoHandBit = eyeCamera ? 2U : 1U;
        if (photoGuard && (photoHandLogMask & photoHandBit) == 0U) {
            photoHandLogMask |= photoHandBit;
            static_cast<void>(gakumas::vr::WriteVrLog(eyeCamera
                ? "[VR][photo] AUTO_PHOTO_HANDS camera=eye action=keep"
                : "[VR][photo] AUTO_PHOTO_HANDS camera=non-eye action=skip"));
        }
        if (photoGuard && !eyeCamera) {
            return;
        }
        try {
            gakumas::vr::AfterOfficialCrowdRender(
                self, commandBuffer, eventType);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, MaterialInfo_ctor,
                (void* self, void* material, void* renderer, int materialIndex,
                 void* instancedMaterial, void* method)) {
        MaterialInfo_ctor_Orig(
            self, material, renderer, materialIndex, instancedMaterial, method);
#ifdef GKMS_WINDOWS
        // The live VL.Core.MaterialInfo constructor exposes the exact Material
        // selected for this Renderer slot. Prefer the instanced material when
        // present; the constructor stores it as the modifiable/rendered copy.
        if (!Config::vrRuntimeStartupEnabled) {
            return;
        }
        try {
            unityStereoRenderer.QueueActorOutlineMaterial(
                instancedMaterial != nullptr ? instancedMaterial : material);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, VLDeferredPass_RenderActor,
                 (void* self, void* context, void* renderingData, void* method)) {
#ifdef GKMS_WINDOWS
        // RenderActor is the last proven managed boundary before actor GBuffer
        // draws. Its dump/live shape has no Renderer/Material argument, so use
        // it only to consume a one-shot, content-ready material discovery.
        try {
            unityStereoRenderer.PrepareActorOutlineMaterialsForCurrentDraw();
        } catch (...) {
            ReportVrUnityHookException();
        }
        // After URP SetupCameraProperties + SetCameraMatrices, immediately
        // before actor GBuffer draws. Parameter-pass cam-pos writes are
        // already overwritten by then (`.139`–`.142`); `.143`'s transform
        // poke + Setup pair is deferred past Submit and self-overwritten.
        // Begin uploads the shot cam pos for the actor draws, End restores
        // the eye value so later passes keep the true camera.
        bool locked = false;
        try {
            locked = unityStereoRenderer.BeginActorMatcapCamPosLock(
                context, renderingData);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        {
            VLDeferredPass_RenderActor_Orig(self, context, renderingData, method);
        }
#ifdef GKMS_WINDOWS
        if (locked) {
            try {
                unityStereoRenderer.EndActorMatcapCamPosLock(
                    context, renderingData);
            } catch (...) {
                ReportVrUnityHookException();
            }
        }
#endif
    }

    DEFINE_HOOK(void, VLActorParameterPass_Execute,
                (void* self, void* context, void* renderingData, void* method)) {
        RunWithActorShadowAnchor("vl-actor-param", [&] {
            VLActorParameterPass_Execute_Orig(
                self, context, renderingData, method);
#ifdef GKMS_WINDOWS
            // Same MatCap re-record as the campus pass: scenes that use the
            // generic VL parameter pass (instead of the campus one) would
            // otherwise never receive the compensation.
            try {
                unityStereoRenderer.ApplyActorMatcapCompensation(
                    context, renderingData);
            } catch (...) {
                ReportVrUnityHookException();
            }
#endif
        });
    }
