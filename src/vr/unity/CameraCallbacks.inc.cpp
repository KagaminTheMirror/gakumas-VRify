// VR-owned integration, included by the patched upstream Hook translation unit.
    void BeforeCameraState(void* self, void* state) {
        PendingCinemachineObservation cameraObservation{};
        try {
            cameraObservation = CaptureCinemachinePose(self, state);
            ApplyVrHeadPose(self, state);
        } catch (...) {
            ReportVrUnityHookException();
        }
    }
    void AfterCameraState(void* self) {
        try {
            while (gakumas::vr::ConsumeLivePauseToggle()) {
                gakumas::vr::ToggleLivePause();
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
        // Photo-scene flag upkeep + queued right-A shutter presses. Same
        // Unity-thread boundary as the pause toggle; alive-checked caches
        // only, no scene scans.
        try {
            gakumas::vr::UpdatePhotoSceneAndConsumeShutterRequests();
        } catch (...) {
            ReportVrUnityHookException();
        }
        // PushStateToUnityCamera is the point where Cinemachine has finished
        // composing the source pose. The A/B/C diagnostic ladder prepares
        // inactive Cameras here and admits them to Unity's ordinary camera list
        // only while no SRP context is active. No render API is called manually.
        try {
            void* outputCamera = ReadCinemachineOutputCamera(self);
            if (outputCamera != nullptr) BootstrapVrSourceCamera();
            const bool selectedSource =
                outputCamera != nullptr && IsSelectedVrMainCamera(outputCamera);
            const bool pipelineIdle = IsUnityRenderPipelineIdle();
            const bool queueDiagnosticHooksReady =
                unityCameraRenderHookReady.load(std::memory_order_acquire) &&
                unityRenderPipelineGuardReady.load(std::memory_order_acquire);
            if (AreVrUnityCameraDiagnosticsEnabled()) {
                // Observe the existing admission inputs; never call get_main
                // ourselves or select a different camera in this probe.
                static thread_local ULONGLONG lastAdmissionLog = 0;
                const auto now = GetTickCount64();
                if (lastAdmissionLog == 0 || now - lastAdmissionLog >= 5000U) {
                    lastAdmissionLog = now;
                    void* tracked = nullptr;
                    {
                        std::lock_guard lock(vrCameraStateMutex);
                        tracked = vrSourceCamera;
                    }
                    const auto frame = ReadVrStereoCameraFrame();
                    std::ostringstream admission;
                    admission << "[VR][stereo] SOURCE_ADMISSION_STATE brain=" << self
                              << " output=" << outputCamera << " tracked=" << tracked
                              << " selected=" << selectedSource << " pipelineIdle=" << pipelineIdle
                              << " hooksReady=" << queueDiagnosticHooksReady
                              << " mainCalls=" << sourceAdmissionMainCalls.load(std::memory_order_relaxed)
                              << " mainNonNull=" << sourceAdmissionMainNonNull.load(std::memory_order_relaxed)
                              << " lastMain=" << sourceAdmissionLastMain.load(std::memory_order_relaxed)
                              << " outputIsEye=" << unityStereoRenderer.IsEyeCamera(outputCamera)
                              << " poseValid=" << frame.trackingSample.valid
                              << " poseRevision=" << frame.trackingSample.revision
                              << " stereo=" << Config::vrStereoEnabled;
                    static_cast<void>(gakumas::vr::WriteVrLog(admission.str()));
                }
            }
            if (queueDiagnosticHooksReady && selectedSource) {
                unityStereoRenderer.Tick(
                    outputCamera,
                    ReadVrStereoCameraFrame(),
                    pipelineIdle);
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
    }
