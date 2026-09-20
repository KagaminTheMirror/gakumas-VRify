// VR-owned integration, included by the patched upstream Hook translation unit.
    DEFINE_HOOK(void, BeginContextRendering, (void* ctx, void* cameras, void* method)) {
        if (unityRenderPipelineGuardReady.load(std::memory_order_acquire) &&
            unityRenderPipelineDepth !=
                (std::numeric_limits<std::uint32_t>::max)()) {
            ++unityRenderPipelineDepth;
        }
        BeginContextRendering_Orig(ctx, cameras, method);
    }

    DEFINE_HOOK(void, EndContextRendering, (void* ctx, void* cameras, void* method)) {
        EndContextRendering_Orig(ctx, cameras, method);
        if (unityRenderPipelineGuardReady.load(std::memory_order_acquire) &&
            unityRenderPipelineDepth != 0) {
            --unityRenderPipelineDepth;
        }
    }

    DEFINE_HOOK(void, BeginCameraRendering, (void* ctx, void* camera, void* method)) {
#ifdef GKMS_WINDOWS
        try {
            unityStereoRenderer.OnBeginCamera(camera);
            if (gakumas::vr::LiveSourcePhotoProtectionActive()) {
                SetFpHeadColorSkip(
                    unityStereoRenderer.IsEyeCamera(camera) &&
                        gakumas::vr::camera::IsVrFreeCameraFirstPerson(),
                    "auto-photo-camera-begin");
            }
            if (std::string_view(unityStereoRenderer.ClassifyCamera(camera)) ==
                "left") {
                ResetActorShadowLeftReuse();
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        BeginCameraRendering_Orig(ctx, camera, method);
    }

    DEFINE_HOOK(void, BeginCameraRenderingManager, (void* ctx, void* camera, void* method)) {
#ifdef GKMS_WINDOWS
        try {
            unityStereoRenderer.OnBeginCamera(camera);
            if (gakumas::vr::LiveSourcePhotoProtectionActive()) {
                SetFpHeadColorSkip(
                    unityStereoRenderer.IsEyeCamera(camera) &&
                        gakumas::vr::camera::IsVrFreeCameraFirstPerson(),
                    "auto-photo-camera-begin");
            }
            if (std::string_view(unityStereoRenderer.ClassifyCamera(camera)) ==
                "left") {
                ResetActorShadowLeftReuse();
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        BeginCameraRenderingManager_Orig(ctx, camera, method);
    }
