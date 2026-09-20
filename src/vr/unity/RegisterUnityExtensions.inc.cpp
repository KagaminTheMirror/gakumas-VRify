// VR-owned integration, included by the patched upstream Hook translation unit.
    void RegisterUnityExtensions(HookInstaller* hookInstaller) {
#ifdef GKMS_WINDOWS
        const bool vrRuntime = IsVrUnityRuntimeEnabled();
#else
        constexpr bool vrRuntime = false;
#endif
        if (Config::enabled || vrRuntime) {
            // The VR free camera samples FOLLOW/FIRST_PERSON bone anchors in
            // this hook, so it must install even when the translation layer
            // (Config::enabled) is off — the VR package ships enabled=false.
            ADD_HOOK(CampusActorController_LateUpdate,
                     Il2cppUtils::GetMethodPointer("campus-submodule.Runtime.dll", "Campus.Common",
                                                   "CampusActorController", "LateUpdate"));

            // Grip transparency Execute-branch fixup (.225); live class
            // proven in campus-submodule.Runtime.dll by the .224 run.
            auto* uiExecute = Il2cppUtils::GetMethodPointer("campus-submodule.Runtime.dll", "Campus.Common.UIRenderer",
                                                           "UIRenderPass", "Execute");

            ADD_HOOK(UIRenderPass_Execute, uiExecute);

            const auto* cameraMainMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_main");
            const bool cameraMainShape = cameraMainMethod != nullptr &&
                cameraMainMethod->static_function && cameraMainMethod->args.empty() &&
                cameraMainMethod->return_type != nullptr &&
                cameraMainMethod->return_type->name == "UnityEngine.Camera" &&
                cameraMainMethod->function != nullptr && cameraMainMethod->address != nullptr;
#ifdef GKMS_WINDOWS
            sourceCameraMainMethod.store(
                cameraMainShape ? cameraMainMethod->address : nullptr,
                std::memory_order_release);
            if (Config::vrDiagnosticsStartupEnabled) {
                std::ostringstream binding;
                binding << "[VR][stereo] SOURCE_CAMERA_MAIN_API ready=" << cameraMainShape
                        << " signature=static UnityEngine.Camera UnityEngine.Camera.get_main()"
                        << " method=" << (cameraMainShape ? cameraMainMethod->address : nullptr)
                        << " function=" << (cameraMainShape ? cameraMainMethod->function : nullptr);
                static_cast<void>(gakumas::vr::WriteVrLog(binding.str()));
            }
#endif
            ADD_HOOK(Camera_get_main,
                     cameraMainShape ? cameraMainMethod->function : nullptr);

            const auto* cinemachineMethod = Il2cppUtils::GetMethod(
                "Cinemachine.dll", "Cinemachine", "CinemachineBrain",
                "PushStateToUnityCamera");
            const bool cinemachineShape = cinemachineMethod != nullptr &&
                !cinemachineMethod->static_function &&
                cinemachineMethod->args.size() == 1U &&
                cinemachineMethod->args.front() != nullptr &&
                cinemachineMethod->args.front()->pType != nullptr &&
                cinemachineMethod->args.front()->pType->name.find("CameraState") !=
                    std::string::npos;
            ADD_HOOK(CinemachineBrain_PushStateToUnityCamera,
                     cinemachineShape ? cinemachineMethod->function : nullptr);

#ifdef GKMS_WINDOWS
            if (vrRuntime) {
                gakumas::vr::InstallLiveAutoPhotoProtection();
                gakumas::vr::InstallGripBlurSource();
                gakumas::vr::input::InstallUnityAnalogScrollHook();
                gakumas::vr::input::InstallUnityPointerInput();
                // Photo-scene lifecycle for the right-A shutter: must install
                // under the frozen VR runtime gate (the VR package ships
                // Config::enabled=false, so translation-block hooks never run).
                ADD_HOOK(LiveScenePresenter_Start,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Live",
                             "LiveScenePresenter", "Start"));
                ADD_HOOK(LiveScenePresenter_OnFinalize,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Live",
                             "LiveScenePresenter", "OnFinalize"));
                ADD_HOOK(PhotographyScenePresenter_Start,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Photography",
                             "PhotographyScenePresenter", "Start"));
                ADD_HOOK(PhotographyScenePresenter_OnFinalize,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Photography",
                             "PhotographyScenePresenter", "OnFinalize"));
                ADD_HOOK(PhotographyScenePresenter_SetEvent,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Photography",
                             "PhotographyScenePresenter", "SetEvent"));
                ADD_HOOK(PhotographyScenePresenter_SetPhotoCountView,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Photography",
                             "PhotographyScenePresenter", "SetPhotoCountView"));
                ADD_HOOK(PhotographySceneModel_set_IsInitialized,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Photography",
                             "PhotographySceneModel", "set_IsInitialized",
                             { "*" }));
                ADD_HOOK(LiveSceneView_PlayCapturePhotoAnimation,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Live",
                             "LiveSceneView", "PlayCapturePhotoAnimation",
                             { "*", "*" }));
                ADD_HOOK(PhotographyPhotoContentView_PlayCapturePhotoAnimation,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Photography",
                             "PhotographyPhotoContentView",
                             "PlayCapturePhotoAnimation", { "*" }));
                // Failure-visible install proof: the ADD_HOOK macro skips a
                // null resolve silently, which stereo.219 hardware could not
                // distinguish from a hook that never fires.
                {
                    std::ostringstream photoHooks;
                    photoHooks
                        << "[VR][photo] HOOK_INSTALL liveStart="
                        << (LiveScenePresenter_Start_Orig != nullptr ? 1 : 0)
                        << " liveFinalize="
                        << (LiveScenePresenter_OnFinalize_Orig != nullptr ? 1 : 0)
                        << " photoStart="
                        << (PhotographyScenePresenter_Start_Orig != nullptr ? 1 : 0)
                        << " photoFinalize="
                        << (PhotographyScenePresenter_OnFinalize_Orig != nullptr ? 1 : 0)
                        << " photoSetEvent="
                        << (PhotographyScenePresenter_SetEvent_Orig != nullptr ? 1 : 0)
                        << " photoCountView="
                        << (PhotographyScenePresenter_SetPhotoCountView_Orig != nullptr ? 1 : 0)
                        << " photoModelInit="
                        << (PhotographySceneModel_set_IsInitialized_Orig != nullptr ? 1 : 0)
                        << " liveCaptureAnim="
                        << (LiveSceneView_PlayCapturePhotoAnimation_Orig != nullptr ? 1 : 0)
                        << " photoCaptureAnim="
                        << (PhotographyPhotoContentView_PlayCapturePhotoAnimation_Orig != nullptr ? 1 : 0);
                    static_cast<void>(
                        gakumas::vr::WriteVrLog(photoHooks.str()));
                }
                EnsureProFlareProjectionLayout();
                ADD_HOOK(
                    ProFlare_UpdateElementJobData,
                    proFlareProjectionLayout.ready &&
                            proFlareProjectionLayout.updateElementJobData != nullptr
                        ? proFlareProjectionLayout.updateElementJobData->function
                        : nullptr);
                ADD_HOOK(
                    ProFlareBatchForSRPData_ScheduleFlares,
                    proFlareProjectionLayout.ready &&
                            proFlareProjectionLayout.scheduleFlares != nullptr
                        ? proFlareProjectionLayout.scheduleFlares->function
                        : nullptr);
                const bool proFlareReplacementReady =
                    gakumas::vr::camera::ProjectionEquivalentProFlareReady(
                        proFlareProjectionLayout.ready,
                        ProFlareBatchForSRPData_ScheduleFlares_Orig != nullptr,
                        ProFlare_UpdateElementJobData_Orig != nullptr);
                unityStereoRenderer.SetProjectionEquivalentProFlareReady(
                    proFlareReplacementReady);
                std::ostringstream proFlareState;
                proFlareState
                    << "[VR][fov] PRO_FLARE_REPLACEMENT_STATE ready="
                    << (proFlareReplacementReady ? 1 : 0)
                    << " layout="
                    << (proFlareProjectionLayout.ready ? 1 : 0)
                    << " scheduleHook="
                    << (ProFlareBatchForSRPData_ScheduleFlares_Orig != nullptr
                            ? 1 : 0)
                    << " elementHook="
                    << (ProFlare_UpdateElementJobData_Orig != nullptr ? 1 : 0)
                    << " failureVisible="
                    << (proFlareReplacementReady ? 0 : 1);
                static_cast<void>(
                    gakumas::vr::WriteVrLog(proFlareState.str()));
            }
#endif

            const auto contextBoundaryShape = [](const UnityResolve::Method* method) {
                return method != nullptr && method->static_function &&
                    method->return_type != nullptr &&
                    method->return_type->name == "System.Void" &&
                    method->function != nullptr && method->address != nullptr &&
                    method->args.size() == 2U && method->args[0] != nullptr &&
                    method->args[0]->pType != nullptr &&
                    method->args[0]->pType->name ==
                        "UnityEngine.Rendering.ScriptableRenderContext" &&
                    method->args[1] != nullptr &&
                    method->args[1]->pType != nullptr &&
                    method->args[1]->pType->name.find(
                        "System.Collections.Generic.List") != std::string::npos &&
                    method->args[1]->pType->name.find("UnityEngine.Camera") !=
                        std::string::npos;
            };
            const auto* beginContextRenderingMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderPipelineManager", "BeginContextRendering");
            ADD_HOOK(
                BeginContextRendering,
                contextBoundaryShape(beginContextRenderingMethod)
                    ? beginContextRenderingMethod->function
                    : nullptr);
            const auto* endContextRenderingMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderPipelineManager", "EndContextRendering");
            ADD_HOOK(
                EndContextRendering,
                contextBoundaryShape(endContextRenderingMethod)
                    ? endContextRenderingMethod->function
                    : nullptr);

            const auto cameraRenderingShape =
                [](const UnityResolve::Method* method) {
                    return method != nullptr && method->static_function &&
                        method->args.size() == 2U &&
                        method->args[0] != nullptr &&
                        method->args[0]->pType != nullptr &&
                        method->args[0]->pType->name.find(
                            "ScriptableRenderContext") != std::string::npos &&
                        method->args[1] != nullptr &&
                        method->args[1]->pType != nullptr &&
                        method->args[1]->pType->name.find("Camera") !=
                            std::string::npos;
                };
            const auto* beginCameraRenderingMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderPipeline", "BeginCameraRendering");
            ADD_HOOK(
                BeginCameraRendering,
                cameraRenderingShape(beginCameraRenderingMethod)
                    ? beginCameraRenderingMethod->function
                    : nullptr);
            const auto* endCameraRenderingMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderPipeline", "EndCameraRendering");
            ADD_HOOK(EndCameraRendering,
                      cameraRenderingShape(endCameraRenderingMethod)
                          ? endCameraRenderingMethod->function
                          : nullptr);
            const auto* beginCameraRenderingManagerMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderPipelineManager", "BeginCameraRendering");
            ADD_HOOK(
                BeginCameraRenderingManager,
                cameraRenderingShape(beginCameraRenderingManagerMethod)
                    ? beginCameraRenderingManagerMethod->function
                    : nullptr);

            const auto* doRenderLoopMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderPipelineManager", "DoRenderLoop_Internal");
            const bool doRenderLoopShape = doRenderLoopMethod != nullptr &&
                doRenderLoopMethod->static_function &&
                doRenderLoopMethod->return_type != nullptr &&
                doRenderLoopMethod->return_type->name == "System.Void" &&
                doRenderLoopMethod->function != nullptr &&
                doRenderLoopMethod->address != nullptr &&
                doRenderLoopMethod->args.size() == 3U &&
                doRenderLoopMethod->args[0] != nullptr &&
                doRenderLoopMethod->args[0]->pType != nullptr &&
                doRenderLoopMethod->args[0]->pType->name ==
                    "UnityEngine.Rendering.RenderPipelineAsset" &&
                doRenderLoopMethod->args[1] != nullptr &&
                doRenderLoopMethod->args[1]->pType != nullptr &&
                doRenderLoopMethod->args[1]->pType->name == "System.IntPtr" &&
                doRenderLoopMethod->args[2] != nullptr &&
                doRenderLoopMethod->args[2]->pType != nullptr &&
                doRenderLoopMethod->args[2]->pType->name == "UnityEngine.Object";
            ADD_HOOK(
                DoRenderLoopInternal,
                doRenderLoopShape ? doRenderLoopMethod->function : nullptr);



            // Direct owned-eye render-path census. This hooks the exact URP
            // dispatcher that receives every classic ScriptableRenderPass,
            // rather than selecting a component/material by a guessed name.
            // If this player takes the RenderGraph/ExecuteFast route instead,
            // the eye callback will report an empty queue and that is itself
            // the next concrete boundary.
            auto* scriptableRendererClass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll",
                "UnityEngine.Rendering.Universal", "ScriptableRenderer");
            UnityResolve::Method* executeRenderPassMethod = nullptr;
            if (scriptableRendererClass != nullptr) {
                for (auto* candidate : scriptableRendererClass->methods) {
                    if (candidate == nullptr ||
                        candidate->name != "ExecuteRenderPass" ||
                        candidate->static_function ||
                        candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != 3U ||
                        candidate->args[0] == nullptr ||
                        candidate->args[0]->pType == nullptr ||
                        candidate->args[0]->pType->name !=
                            "UnityEngine.Rendering.ScriptableRenderContext" ||
                        candidate->args[1] == nullptr ||
                        candidate->args[1]->pType == nullptr ||
                        candidate->args[1]->pType->name !=
                            "UnityEngine.Rendering.Universal.ScriptableRenderPass" ||
                        candidate->args[2] == nullptr ||
                        candidate->args[2]->pType == nullptr ||
                        candidate->args[2]->pType->name !=
                            "UnityEngine.Rendering.Universal.RenderingData&") {
                        continue;
                    }
                    if (executeRenderPassMethod != nullptr) {
                        executeRenderPassMethod = nullptr;
                        break;
                    }
                    executeRenderPassMethod = candidate;
                }
            }
            auto* scriptableRenderPassClass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll",
                "UnityEngine.Rendering.Universal", "ScriptableRenderPass");
            if (scriptableRenderPassClass != nullptr) {
                for (auto* candidate : scriptableRenderPassClass->methods) {
                    if (candidate == nullptr ||
                        candidate->name != "get_renderPassEvent" ||
                        candidate->static_function ||
                        candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name !=
                            "UnityEngine.Rendering.Universal.RenderPassEvent" ||
                        !candidate->args.empty()) {
                        continue;
                    }
                    if (eyePassEventGetter != nullptr) {
                        eyePassEventGetter = nullptr;
                        break;
                    }
                    eyePassEventGetter = candidate;
                }
            }

            auto* renderObjectsPassClass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll",
                "UnityEngine.Experimental.Rendering.Universal",
                "RenderObjectsPass");
            eyeRenderObjectsPassFields.passClass = renderObjectsPassClass;
            const auto findInstanceField = [](
                    UnityResolve::Class* klass,
                    std::string_view name) -> UnityResolve::Field* {
                if (klass == nullptr) {
                    return nullptr;
                }
                UnityResolve::Field* match = nullptr;
                for (auto* field : klass->fields) {
                    if (field == nullptr || field->name != name ||
                        field->static_field || field->offset < 0) {
                        continue;
                    }
                    if (match != nullptr) {
                        return nullptr;
                    }
                    match = field;
                }
                return match;
            };
            eyeRenderObjectsPassFields.renderQueueType = findInstanceField(
                renderObjectsPassClass, "renderQueueType");
            eyeRenderObjectsPassFields.filteringSettings = findInstanceField(
                renderObjectsPassClass, "m_FilteringSettings");
            eyeRenderObjectsPassFields.cameraSettings = findInstanceField(
                renderObjectsPassClass, "m_CameraSettings");
            eyeRenderObjectsPassFields.profilerTag = findInstanceField(
                renderObjectsPassClass, "m_ProfilerTag");
            eyeRenderObjectsPassFields.overrideMaterial = findInstanceField(
                renderObjectsPassClass, "<overrideMaterial>k__BackingField");
            eyeRenderObjectsPassFields.overrideMaterialPassIndex =
                findInstanceField(
                    renderObjectsPassClass,
                    "<overrideMaterialPassIndex>k__BackingField");
            eyeRenderObjectsPassFields.overrideShader = findInstanceField(
                renderObjectsPassClass, "<overrideShader>k__BackingField");
            eyeRenderObjectsPassFields.overrideShaderPassIndex =
                findInstanceField(
                    renderObjectsPassClass,
                    "<overrideShaderPassIndex>k__BackingField");

            auto* filteringSettingsClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "FilteringSettings");
            eyeRenderObjectsPassFields.filteringClass = filteringSettingsClass;
            eyeRenderObjectsPassFields.renderQueueRange = findInstanceField(
                filteringSettingsClass, "m_RenderQueueRange");
            eyeRenderObjectsPassFields.layerMask = findInstanceField(
                filteringSettingsClass, "m_LayerMask");
            eyeRenderObjectsPassFields.renderingLayerMask = findInstanceField(
                filteringSettingsClass, "m_RenderingLayerMask");
            eyeRenderObjectsPassFields.excludeMotionVectorObjects =
                findInstanceField(
                    filteringSettingsClass, "m_ExcludeMotionVectorObjects");

            auto* renderQueueRangeClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderQueueRange");
            eyeRenderObjectsPassFields.renderQueueRangeClass =
                renderQueueRangeClass;
            eyeRenderObjectsPassFields.lowerBound = findInstanceField(
                renderQueueRangeClass, "m_LowerBound");
            eyeRenderObjectsPassFields.upperBound = findInstanceField(
                renderQueueRangeClass, "m_UpperBound");

            UnityResolve::Method* renderObjectsExecuteMethod = nullptr;
            if (renderObjectsPassClass != nullptr) {
                for (auto* candidate : renderObjectsPassClass->methods) {
                    if (candidate == nullptr || candidate->name != "Execute" ||
                        candidate->static_function || candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != 2U ||
                        candidate->args[0] == nullptr ||
                        candidate->args[0]->pType == nullptr ||
                        candidate->args[0]->pType->name !=
                            "UnityEngine.Rendering.ScriptableRenderContext" ||
                        candidate->args[1] == nullptr ||
                        candidate->args[1]->pType == nullptr ||
                        candidate->args[1]->pType->name !=
                            "UnityEngine.Rendering.Universal.RenderingData&") {
                        continue;
                    }
                    if (renderObjectsExecuteMethod != nullptr) {
                        renderObjectsExecuteMethod = nullptr;
                        break;
                    }
                    renderObjectsExecuteMethod = candidate;
                }
            }
            // SMAA T2x functionally copies the shared motion-vector RTHandle
            // immediately after each eye's verified post-process pass. TSCMAA
            // reuses this proven GPU-ordered boundary. The hook therefore
            // follows the ordinary frozen VR runtime gate; all remaining
            // pass/full-screen census hooks stay diagnostic.
            if (Config::vrRuntimeStartupEnabled) {
                ADD_HOOK(
                    ScriptableRenderer_ExecuteRenderPass,
                    executeRenderPassMethod != nullptr
                        ? executeRenderPassMethod->function
                        : nullptr);
            }


            // OverlayCanvas skip installs independently of diagnostics.
            ADD_HOOK(
                RenderObjectsPass_Execute,
                renderObjectsExecuteMethod != nullptr
                    ? renderObjectsExecuteMethod->function
                    : nullptr);
#ifdef GKMS_WINDOWS
            {
                std::ostringstream hookLine;
                hookLine << "[VR][eye-pass] EYE_RENDER_OBJECTS_HOOK ready="
                         << (renderObjectsExecuteMethod != nullptr &&
                                     eyePassEventGetter != nullptr
                                 ? 1
                                 : 0)
                         << " installed="
                         << (RenderObjectsPass_Execute_Orig != nullptr ? 1 : 0)
                         << " eventGetter="
                         << (eyePassEventGetter != nullptr ? 1 : 0)
                         << " signature=System.Void Execute("
                         << "UnityEngine.Rendering.ScriptableRenderContext,"
                         << "UnityEngine.Rendering.Universal.RenderingData&)";
                static_cast<void>(gakumas::vr::WriteVrLog(hookLine.str()));
            }
#endif

            const auto findUniqueInstanceVoidByArity = [](
                    UnityResolve::Class* klass,
                    std::string_view methodName,
                    std::size_t expectedArgCount) -> UnityResolve::Method* {
                if (klass == nullptr) {
                    return nullptr;
                }
                UnityResolve::Method* exact = nullptr;
                for (auto* candidate : klass->methods) {
                    if (candidate == nullptr || candidate->name != methodName ||
                        candidate->static_function || candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != expectedArgCount) {
                        continue;
                    }
                    if (exact != nullptr) {
                        return nullptr;
                    }
                    exact = candidate;
                }
                return exact;
            };
            // Actor-lighting passes all override
            // ScriptableRenderPass.Execute(ScriptableRenderContext,
            // ref RenderingData). Accept only that exact shape, resolved on
            // the live table, and let a miss disable the anchor for that pass
            // instead of guessing an address.
            const auto resolveActorPassExecute =
                [](const char* assembly, const char* nameSpace,
                   const char* className) -> void* {
                    auto* klass =
                        Il2cppUtils::GetClass(assembly, nameSpace, className);
                    if (klass == nullptr) {
                        return nullptr;
                    }
                    for (const auto* candidate : klass->methods) {
                        if (candidate == nullptr ||
                            candidate->name != "Execute" ||
                            candidate->static_function ||
                            candidate->function == nullptr ||
                            candidate->return_type == nullptr ||
                            candidate->return_type->name != "System.Void" ||
                            candidate->args.size() != 2U ||
                            candidate->args[0] == nullptr ||
                            candidate->args[0]->pType == nullptr ||
                            candidate->args[0]->pType->name !=
                                "UnityEngine.Rendering.ScriptableRenderContext" ||
                            candidate->args[1] == nullptr ||
                            candidate->args[1]->pType == nullptr ||
                            candidate->args[1]->pType->name.find(
                                "RenderingData") == std::string::npos) {
                            continue;
                        }
                        return candidate->function;
                    }
                    return nullptr;
                };
            ADD_HOOK(
                ActorShadowPass_Execute,
                resolveActorPassExecute(
                    "vl-unity.Runtime.dll", "VL.Rendering", "ActorShadowPass"));
            auto* actorShadowPassClass = Il2cppUtils::GetClass(
                "vl-unity.Runtime.dll", "VL.Rendering", "ActorShadowPass");
            auto* drawActorShadowFeatureClass = Il2cppUtils::GetClass(
                "vl-unity.Runtime.dll", "VL.Rendering", "DrawActorShadowPass");
            actorShadowStereoReuse.featurePass = findInstanceField(
                drawActorShadowFeatureClass, "_pass");
            actorShadowStereoReuse.shadowData = findInstanceField(
                actorShadowPassClass, "_actorShadowData");
            actorShadowStereoReuse.shadowBias = findInstanceField(
                actorShadowPassClass, "_actorShadowBias");
            actorShadowStereoReuse.supportsShadow = findInstanceField(
                actorShadowPassClass, "_supportsShadow");
            auto* actorShadowScriptableRenderPassClass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll",
                "UnityEngine.Rendering.Universal", "ScriptableRenderPass");
            actorShadowStereoReuse.clearFlag = findInstanceField(
                actorShadowScriptableRenderPassClass, "m_ClearFlag");
            if (actorShadowScriptableRenderPassClass != nullptr) {
                constexpr std::array<std::string_view, 2> clearArgs = {
                    "UnityEngine.Rendering.ClearFlag",
                    "UnityEngine.Color",
                };
                for (auto* candidate :
                     actorShadowScriptableRenderPassClass->methods) {
                    if (candidate == nullptr ||
                        candidate->name != "ConfigureClear" ||
                        candidate->static_function ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != clearArgs.size()) {
                        continue;
                    }
                    bool exact = true;
                    for (std::size_t index = 0U;
                         index < clearArgs.size(); ++index) {
                        if (candidate->args[index] == nullptr ||
                            candidate->args[index]->pType == nullptr ||
                            std::string_view(candidate->args[index]->pType->name) !=
                                clearArgs[index]) {
                            exact = false;
                            break;
                        }
                    }
                    if (!exact ||
                        actorShadowStereoReuse.configureClear != nullptr) {
                        actorShadowStereoReuse.configureClear = nullptr;
                        break;
                    }
                    actorShadowStereoReuse.configureClear = candidate;
                }
            }
            auto* actorShadowDataClass = Il2cppUtils::GetClass(
                "vl-unity.Runtime.dll", "", "ActorShadowData");
            if (actorShadowDataClass != nullptr &&
                actorShadowDataClass->address != nullptr &&
                UnityResolve::Invoke<bool>(
                    "il2cpp_class_is_valuetype",
                    actorShadowDataClass->address)) {
                actorShadowStereoReuse.dataFieldHeader =
                    static_cast<std::int32_t>(2U * sizeof(void*));
            }
            actorShadowStereoReuse.startDistance2 = findInstanceField(
                actorShadowDataClass, "startDistance2");
            actorShadowStereoReuse.endDistance2 = findInstanceField(
                actorShadowDataClass, "endDistance2");
            actorShadowStereoReuse.fade = findInstanceField(
                actorShadowDataClass, "fade");
            actorShadowStereoReuse.strength = findInstanceField(
                actorShadowDataClass, "strength");
            std::uint32_t actorShadowsKeywordFlags = 0U;
            if (actorShadowPassClass != nullptr) {
                for (auto* field : actorShadowPassClass->fields) {
                    if (field != nullptr && field->address != nullptr &&
                        field->name == "_ACTOR_SHADOWS" &&
                        field->type != nullptr &&
                        field->type->name == "System.String") {
                        const std::uint32_t flags =
                            UnityResolve::Invoke<std::uint32_t>(
                                "il2cpp_field_get_flags", field->address);
                        constexpr std::uint32_t kFieldAttributeStatic = 0x10U;
                        if ((flags & kFieldAttributeStatic) == 0U) {
                            continue;
                        }
                        if (actorShadowStereoReuse.actorShadowsKeyword != nullptr) {
                            actorShadowStereoReuse.actorShadowsKeyword = nullptr;
                            actorShadowsKeywordFlags = 0U;
                            break;
                        }
                        actorShadowStereoReuse.actorShadowsKeyword = field;
                        actorShadowsKeywordFlags = flags;
                    }
                }
            }
            if (actorShadowStereoReuse.shadowData != nullptr &&
                actorShadowStereoReuse.shadowBias != nullptr &&
                actorShadowStereoReuse.shadowBias->offset >
                    actorShadowStereoReuse.shadowData->offset) {
                actorShadowStereoReuse.dataSize = static_cast<std::size_t>(
                    actorShadowStereoReuse.shadowBias->offset -
                    actorShadowStereoReuse.shadowData->offset);
            }
            actorShadowStereoReuse.setupReceiver =
                findUniqueInstanceVoidByArity(
                actorShadowPassClass,
                "SetupActorShadowReceiverConstants",
                1U);
            UnityResolve::Method* actorShadowDrawMethod = nullptr;
            UnityResolve::Method* actorShadowConfigureMethod = nullptr;
            if (actorShadowPassClass != nullptr) {
                actorShadowDrawMethod = findUniqueInstanceVoidByArity(
                    actorShadowPassClass, "DrawShadow", 3U);
                actorShadowConfigureMethod = findUniqueInstanceVoidByArity(
                    actorShadowPassClass, "Configure", 2U);
            }
            auto* coreUtilsClass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Core.Runtime.dll",
                "UnityEngine.Rendering", "CoreUtils");
            if (coreUtilsClass != nullptr) {
                constexpr std::array<std::string_view, 3> keywordArgs = {
                    "UnityEngine.Rendering.CommandBuffer",
                    "System.String",
                    "System.Boolean",
                };
                UnityResolve::Method* exactKeyword = nullptr;
                for (auto* candidate : coreUtilsClass->methods) {
                    if (candidate == nullptr || candidate->name != "SetKeyword" ||
                        !candidate->static_function || candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != keywordArgs.size()) {
                        continue;
                    }
                    bool exact = true;
                    for (std::size_t i = 0; i < keywordArgs.size(); ++i) {
                        if (candidate->args[i] == nullptr ||
                            candidate->args[i]->pType == nullptr ||
                            std::string_view(candidate->args[i]->pType->name) !=
                                keywordArgs[i]) {
                            exact = false;
                            break;
                        }
                    }
                    if (!exact) {
                        continue;
                    }
                    if (exactKeyword != nullptr) {
                        exactKeyword = nullptr;
                        break;
                    }
                    exactKeyword = candidate;
                }
                actorShadowStereoReuse.setKeyword = exactKeyword;
            }
            const bool actorShadowReuseReady =
                actorShadowStereoReuse.featurePass != nullptr &&
                actorShadowStereoReuse.shadowData != nullptr &&
                actorShadowStereoReuse.shadowBias != nullptr &&
                actorShadowStereoReuse.supportsShadow != nullptr &&
                actorShadowStereoReuse.clearFlag != nullptr &&
                actorShadowStereoReuse.configureClear != nullptr &&
                actorShadowStereoReuse.actorShadowsKeyword != nullptr &&
                actorShadowStereoReuse.setupReceiver != nullptr &&
                actorShadowStereoReuse.setKeyword != nullptr &&
                actorShadowDrawMethod != nullptr &&
                actorShadowConfigureMethod != nullptr &&
                actorShadowStereoReuse.dataSize == 0x11CU;
            ADD_HOOK(
                ActorShadowPass_Configure,
                actorShadowReuseReady && actorShadowConfigureMethod != nullptr
                    ? actorShadowConfigureMethod->function : nullptr);
            ADD_HOOK(
                ActorShadowPass_DrawShadow,
                actorShadowReuseReady && actorShadowDrawMethod != nullptr
                    ? actorShadowDrawMethod->function : nullptr);
            // Virtual (slot 7) — called through the vtable by URP's feature
            // walk, so it always has a live function body even when LTCG
            // inlines Setup/UpdateShadowData into it. The .94 Setup trampoline
            // never entered; .95 hardware confirmed this is the live path.
            const auto resolveDrawActorShadowAddRenderPasses = []() -> void* {
                auto* klass = Il2cppUtils::GetClass(
                    "vl-unity.Runtime.dll", "VL.Rendering",
                    "DrawActorShadowPass");
                if (klass == nullptr) {
                    return nullptr;
                }
                for (const auto* candidate : klass->methods) {
                    if (candidate == nullptr ||
                        candidate->name != "AddRenderPasses" ||
                        candidate->static_function ||
                        candidate->function == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != 2U ||
                        candidate->args[1] == nullptr ||
                        candidate->args[1]->pType == nullptr ||
                        candidate->args[1]->pType->name.find(
                            "RenderingData") == std::string::npos) {
                        continue;
                    }
                    return candidate->function;
                }
                return nullptr;
            };
            const void* addRenderPassesAddr =
                resolveDrawActorShadowAddRenderPasses();
            ADD_HOOK(
                DrawActorShadowPass_AddRenderPasses,
                const_cast<void*>(addRenderPassesAddr));
#ifdef GKMS_WINDOWS
            // ADD_HOOK only reports through printf (no console on the game
            // process), so mirror the resolve results into vr.log; the
            // .94 run could not distinguish "resolver missed" from "hook
            // installed but the body was never entered".
            {
                const auto logMethodCandidates = [](UnityResolve::Class* klass) {
                    if (klass == nullptr) {
                        return;
                    }
                    for (const auto* candidate : klass->methods) {
                        if (candidate == nullptr ||
                            (candidate->name != "Configure" &&
                             candidate->name != "DrawShadow" &&
                             candidate->name !=
                                 "SetupActorShadowReceiverConstants")) {
                            continue;
                        }
                        std::ostringstream methodStream;
                        methodStream
                            << "[VR][shadow] ACTOR_SHADOW_METHOD_TABLE"
                            << " name=" << candidate->name
                            << " function=" << candidate->function
                            << " info=" << candidate->address
                            << " static=" << (candidate->static_function ? 1 : 0)
                            << " return="
                            << (candidate->return_type != nullptr
                                    ? candidate->return_type->name : "?")
                            << " argc=" << candidate->args.size();
                        for (std::size_t index = 0U;
                             index < candidate->args.size(); ++index) {
                            const auto* arg = candidate->args[index];
                            methodStream << " arg" << index << "="
                                         << (arg != nullptr ? arg->name : "?")
                                         << ":"
                                         << (arg != nullptr && arg->pType != nullptr
                                                 ? arg->pType->name : "?");
                        }
                        static_cast<void>(gakumas::vr::WriteVrLog(
                            methodStream.str()));
                    }
                };
                logMethodCandidates(actorShadowPassClass);
                std::ostringstream resolveStream;
                resolveStream << "[VR][shadow] SHADOW_HOOK_RESOLVE"
                              << " addRenderPasses=" << addRenderPassesAddr
                              << " drawShadow="
                              << (actorShadowDrawMethod != nullptr
                                      ? actorShadowDrawMethod->function : nullptr)
                              << " configure="
                              << (actorShadowConfigureMethod != nullptr
                                      ? actorShadowConfigureMethod->function : nullptr)
                              << " reuseReady="
                              << (actorShadowReuseReady ? 1 : 0)
                              << " prereq="
                              << (actorShadowStereoReuse.featurePass != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.shadowData != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.shadowBias != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.supportsShadow != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.clearFlag != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.configureClear != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.actorShadowsKeyword != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.setupReceiver != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.setKeyword != nullptr ? 1 : 0)
                              << (actorShadowDrawMethod != nullptr ? 1 : 0)
                              << (actorShadowConfigureMethod != nullptr ? 1 : 0)
                              << " dataOffset="
                              << (actorShadowStereoReuse.shadowData != nullptr
                                      ? actorShadowStereoReuse.shadowData->offset : -1)
                              << " dataSize=" << actorShadowStereoReuse.dataSize
                              << " clearOffset="
                              << (actorShadowStereoReuse.clearFlag != nullptr
                                      ? actorShadowStereoReuse.clearFlag->offset : -1)
                              << " configureClear="
                              << (actorShadowStereoReuse.configureClear != nullptr
                                      ? actorShadowStereoReuse.configureClear->function
                                      : nullptr)
                              << " dataFieldHeader="
                              << actorShadowStereoReuse.dataFieldHeader
                              << " keywordOffset="
                              << (actorShadowStereoReuse.actorShadowsKeyword != nullptr
                                      ? actorShadowStereoReuse.actorShadowsKeyword->offset : -1)
                              << " keywordFlags=" << actorShadowsKeywordFlags
                              << " distanceOffsets="
                              << (actorShadowStereoReuse.startDistance2 != nullptr
                                      ? actorShadowStereoReuse.startDistance2->offset -
                                            actorShadowStereoReuse.dataFieldHeader : -1)
                              << ","
                              << (actorShadowStereoReuse.endDistance2 != nullptr
                                      ? actorShadowStereoReuse.endDistance2->offset -
                                            actorShadowStereoReuse.dataFieldHeader : -1)
                              << ","
                              << (actorShadowStereoReuse.fade != nullptr
                                      ? actorShadowStereoReuse.fade->offset -
                                            actorShadowStereoReuse.dataFieldHeader : -1)
                              << ","
                              << (actorShadowStereoReuse.strength != nullptr
                                      ? actorShadowStereoReuse.strength->offset -
                                            actorShadowStereoReuse.dataFieldHeader : -1);
                static_cast<void>(
                    gakumas::vr::WriteVrLog(resolveStream.str()));
            }
#endif
            // The MatCap main light — hair/accessory shading on the body.
            ADD_HOOK(
                CampusActorParameterPass_Execute,
                resolveActorPassExecute(
                    "campus-submodule.Runtime.dll", "Campus.Rendering",
                    "CampusActorParameterPass"));
            ADD_HOOK(
                VLActorParameterPass_Execute,
                resolveActorPassExecute(
                    "Unity.RenderPipelines.Universal.Runtime.dll",
                    "VL.Rendering", "VLActorParameterPass"));
            const auto resolveMaterialInfoConstructor = []() -> void* {
                auto* klass = Il2cppUtils::GetClass(
                    "vl-unity.Runtime.dll", "VL.Core", "MaterialInfo");
                if (klass == nullptr) {
                    return nullptr;
                }
                static constexpr const char* kArgs[] = {
                    "UnityEngine.Material", "UnityEngine.Renderer",
                    "System.Int32", "UnityEngine.Material",
                };
                for (const auto* candidate : klass->methods) {
                    if (candidate == nullptr || candidate->name != ".ctor" ||
                        candidate->static_function ||
                        candidate->function == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != std::size(kArgs)) {
                        continue;
                    }
                    bool exact = true;
                    for (std::size_t index = 0; index < std::size(kArgs);
                         ++index) {
                        exact = exact && candidate->args[index] != nullptr &&
                            candidate->args[index]->pType != nullptr &&
                            candidate->args[index]->pType->name == kArgs[index];
                    }
                    if (exact) {
                        return candidate->function;
                    }
                }
                return nullptr;
            };
            const auto resolveUpdatePenlightParams = []() -> void* {
                static constexpr const char* kAssemblies[] = {
                    "Assembly-CSharp.dll",
                    "campus-submodule.Runtime.dll",
                };
                for (const char* assembly : kAssemblies) {
                    auto* klass = Il2cppUtils::GetClass(
                        assembly, "Campus.MobAudience",
                        "MobAudiencePenlightController");
                    if (klass == nullptr) {
                        continue;
                    }
                    for (const auto* candidate : klass->methods) {
                        if (candidate == nullptr ||
                            candidate->name != "UpdatePenlightParams" ||
                            candidate->static_function ||
                            candidate->function == nullptr ||
                            candidate->return_type == nullptr ||
                            candidate->return_type->name != "System.Void" ||
                            candidate->args.size() != 2U ||
                            candidate->args[0] == nullptr ||
                            candidate->args[0]->pType == nullptr ||
                            candidate->args[1] == nullptr ||
                            candidate->args[1]->pType == nullptr) {
                            continue;
                        }
                        const auto& contextType =
                            candidate->args[0]->pType->name;
                        const auto& cameraType =
                            candidate->args[1]->pType->name;
                        if (contextType.find("ScriptableRenderContext") !=
                                std::string::npos &&
                            cameraType.find("Camera") != std::string::npos) {
                            return candidate->function;
                        }
                    }
                }
                return nullptr;
            };
            std::string crowdRenderSignature = "-";
            const auto resolveCrowdRender =
                [&crowdRenderSignature]() -> void* {
                static constexpr const char* kAssemblies[] = {
                    "Assembly-CSharp.dll",
                    "campus-submodule.Runtime.dll",
                };
                for (const char* assembly : kAssemblies) {
                    auto* klass = Il2cppUtils::GetClass(
                        assembly, "Campus.Crowd", "CrowdSystem");
                    if (klass == nullptr) {
                        continue;
                    }
                    for (const auto* candidate : klass->methods) {
                        if (candidate == nullptr ||
                            candidate->name != "RenderCrowd" ||
                            candidate->return_type == nullptr) {
                            continue;
                        }
                        std::ostringstream signature;
                        signature << candidate->return_type->name
                                  << " RenderCrowd(";
                        for (std::size_t index = 0;
                             index < candidate->args.size(); ++index) {
                            if (index != 0U) {
                                signature << ",";
                            }
                            signature << (candidate->args[index] != nullptr &&
                                    candidate->args[index]->pType != nullptr
                                ? candidate->args[index]->pType->name
                                : "-");
                        }
                        signature << ")";
                        crowdRenderSignature = signature.str();

                        if (candidate->static_function ||
                            candidate->function == nullptr ||
                            candidate->return_type->name != "System.Void" ||
                            candidate->args.size() != 2U ||
                            candidate->args[0] == nullptr ||
                            candidate->args[0]->pType == nullptr ||
                            candidate->args[1] == nullptr ||
                            candidate->args[1]->pType == nullptr) {
                            continue;
                        }
                        const auto& commandBufferType =
                            candidate->args[0]->pType->name;
                        const auto& eventType =
                            candidate->args[1]->pType->name;
                        const bool exactEvent =
                            eventType == "CrowdSystem.EventType" ||
                            eventType ==
                                "Campus.Crowd.CrowdSystem.EventType";
                        if (commandBufferType ==
                                "UnityEngine.Rendering.CommandBuffer" &&
                            exactEvent) {
                            return candidate->function;
                        }
                    }
                }
                return nullptr;
            };
            const void* materialInfoCtorAddr = nullptr;
            const void* updatePenlightParamsAddr = nullptr;
            const void* crowdRenderAddr = nullptr;
            if (vrRuntime) {
                materialInfoCtorAddr = resolveMaterialInfoConstructor();
                ADD_HOOK(
                    MaterialInfo_ctor,
                    const_cast<void*>(materialInfoCtorAddr));
                updatePenlightParamsAddr = resolveUpdatePenlightParams();
                ADD_HOOK(
                    MobPenlight_UpdatePenlightParams,
                    const_cast<void*>(updatePenlightParamsAddr));
                crowdRenderAddr = resolveCrowdRender();
                ADD_HOOK(
                    CrowdSystem_RenderCrowd,
                    const_cast<void*>(crowdRenderAddr));
            }
#ifdef GKMS_WINDOWS
            if (vrRuntime) {
                static_cast<void>(gakumas::vr::WriteVrLog(
                    std::string(
                        "[VR][stereo] EYE_OUTLINE_CAPTURE_HOOK resolved=") +
                    (materialInfoCtorAddr != nullptr ? "1" : "0")));
                static_cast<void>(gakumas::vr::WriteVrLog(
                    std::string(
                        "[VR][stereo] HAND_GLOW hook UpdatePenlightParams=") +
                    (updatePenlightParamsAddr != nullptr ? "1" : "0")));
                static_cast<void>(gakumas::vr::WriteVrLog(
                    std::string(
                        "[VR][stereo] HAND_GLOW crowd hook RenderCrowd=") +
                    (crowdRenderAddr != nullptr ? "1" : "0") +
                    " signature=" + crowdRenderSignature));
            }
#endif
            const auto resolveDeferredRenderActor = []() -> void* {
                static constexpr const char* kAssemblies[] = {
                    "Unity.RenderPipelines.Universal.Runtime.dll",
                    "vl-unity.Runtime.dll",
                };
                for (const char* assembly : kAssemblies) {
                    auto* klass = Il2cppUtils::GetClass(
                        assembly, "VL.Rendering", "VLDeferredPass");
                    if (klass == nullptr) {
                        continue;
                    }
                    for (const auto* candidate : klass->methods) {
                        if (candidate == nullptr ||
                            candidate->name != "RenderActor" ||
                            candidate->static_function ||
                            candidate->function == nullptr ||
                            candidate->return_type == nullptr ||
                            candidate->return_type->name != "System.Void" ||
                            candidate->args.size() != 2U ||
                            candidate->args[0] == nullptr ||
                            candidate->args[0]->pType == nullptr ||
                            candidate->args[0]->pType->name !=
                                "UnityEngine.Rendering.ScriptableRenderContext" ||
                            candidate->args[1] == nullptr ||
                            candidate->args[1]->pType == nullptr ||
                            candidate->args[1]->pType->name.find(
                                "RenderingData") == std::string::npos) {
                            continue;
                        }
                        return candidate->function;
                    }
                }
                return nullptr;
            };
            const void* renderActorAddr = resolveDeferredRenderActor();
            ADD_HOOK(
                VLDeferredPass_RenderActor,
                const_cast<void*>(renderActorAddr));
#ifdef GKMS_WINDOWS
            {
                std::ostringstream resolveStream;
                resolveStream << "[VR][shadow] MATCAP_GBUFFER_HOOK_RESOLVE"
                              << " renderActor=" << renderActorAddr;
                static_cast<void>(
                    gakumas::vr::WriteVrLog(resolveStream.str()));
            }
#endif
        }

#ifdef GKMS_WINDOWS
        if (vrRuntime) {
            unityCameraMainHookReady.store(
                Camera_get_main_Orig != nullptr,
                std::memory_order_relaxed);
            unityCinemachineHookReady.store(
                CinemachineBrain_PushStateToUnityCamera_Orig != nullptr,
                std::memory_order_relaxed);
            unityCameraRenderHookReady.store(
                EndCameraRendering_Orig != nullptr,
                std::memory_order_relaxed);
            unityRenderPipelineGuardReady.store(
                DoRenderLoopInternal_Orig != nullptr,
                std::memory_order_release);
            unityCameraDiagnosticHooksConfigured.store(true, std::memory_order_release);
            EnsureUnityCameraDiagnosticMarker();
        }
#endif

    }
