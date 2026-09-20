// VR-owned integration, included by the patched upstream Hook translation unit.
    DEFINE_HOOK(void, ScriptableRenderer_ExecuteRenderPass,
                (void* self, void* context, void* renderPass,
                  void* renderingData, void* method)) {

        ScriptableRenderer_ExecuteRenderPass_Orig(
            self, context, renderPass, renderingData, method);
#ifdef GKMS_WINDOWS
        try {
            gakumas::vr::ObserveSmaaT2xRenderPass(
                renderPass,
                context,
                renderingData,
                ReadEyeRenderPassEvent(renderPass),
                unityStereoRenderer);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, RenderObjectsPass_Execute,
                (void* self, void* context, void* renderingData, void* method)) {
#ifdef GKMS_WINDOWS
        try {
            void* camera = unityStereoRenderer.CurrentCamera();
            const bool isEye = unityStereoRenderer.IsEyeCamera(camera);
            const char* role = unityStereoRenderer.ClassifyCamera(camera);
            const int event = ReadEyeRenderPassEvent(self);
            const bool skip = ShouldSkipEyeRenderObjectsPass(self);
            LogEyeRenderObjectsPolicy(
                role, camera, self, event, isEye, skip, !skip);
            if (skip) {
                return;
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        RenderObjectsPass_Execute_Orig(self, context, renderingData, method);
    }

    DEFINE_HOOK(void, DoRenderLoopInternal,
                (void* pipelineAsset, std::intptr_t loopPtr,
                 void* renderRequest, void* method)) {
        const auto perfSink = [](std::string_view line) noexcept { WriteUnityCameraDiagnosticEvent(line); };
        VR_PERF_SCOPE(whole, "unity.render-hook", perfSink);
        VR_PERF_SCOPE(before, "unity.before-srp", perfSink);
        const bool outerNormalLoop = renderRequest == nullptr &&
            unityRenderLoopDepth == 0U;
        ++unityRenderLoopDepth;
#ifdef GKMS_WINDOWS
        // The current Unity 6000 player invokes the camera callbacks from this
        // native render-loop bridge but does not pass through the managed
        // Begin/EndContextRendering event wrappers we can hook. Treat the
        // outer normal DoRenderLoop invocation itself as the authoritative
        // frame boundary so owned EndCamera callbacks can be attributed.
        if (outerNormalLoop &&
            unityRenderPipelineGuardReady.load(std::memory_order_acquire)) {
            gakumas::vr::VrRuntime::Instance().EnsureGraphicsBegun();
            if (!unityRenderLoopHookHitLogged.exchange(
                    true, std::memory_order_acq_rel)) {
                std::ostringstream boundary;
                boundary << "[VR][stereo] RENDER_LOOP_HOOK_HIT tid="
                         << GetCurrentThreadId()
                         << " loopPtr=0x" << std::hex
                         << static_cast<std::uintptr_t>(loopPtr);
                WriteUnityCameraDiagnosticEvent(boundary.str());
            }
            try {
                unityStereoRenderer.OnBeginContext();
            } catch (...) {
                ReportVrUnityHookException();
            }
        }
#endif
        before.Stop();
        static thread_local gakumas::vr::perf::Accumulator renderTiming;
        gakumas::vr::perf::Scope renderScope(renderTiming,
            Config::vrDiagnosticsStartupEnabled && outerNormalLoop,
            "unity.render-loop-original",
            [](std::string_view line) noexcept { WriteUnityCameraDiagnosticEvent(line); });
        DoRenderLoopInternal_Orig(pipelineAsset, loopPtr, renderRequest, method);
        renderScope.Stop();
#ifdef GKMS_WINDOWS

#endif
        if (unityRenderLoopDepth != 0U) {
            --unityRenderLoopDepth;
        }
#ifdef GKMS_WINDOWS
        // This is the first hook point after Unity has returned from the whole
        // top-level SRP render loop. Unlike EndContextRendering, managed camera
        // mutation and native texture access are no longer inside an SRP callback.
        if (outerNormalLoop && unityRenderLoopDepth == 0U &&
            unityRenderPipelineGuardReady.load(std::memory_order_acquire)) {
            try {
                VR_PERF_SCOPE(contextEnd, "unity.after-srp-context", perfSink);
                unityStereoRenderer.OnEndContext(true);
                contextEnd.Stop();
                unityStereoRenderer.OnRenderLoopCompleted();
                VR_PERF_SCOPE(driver, "unity.after-srp-driver", perfSink);
                gakumas::vr::FrameLoopDriverAfterSrp();
            } catch (...) {
                ReportVrUnityHookException();
            }
        }
#endif
    }
