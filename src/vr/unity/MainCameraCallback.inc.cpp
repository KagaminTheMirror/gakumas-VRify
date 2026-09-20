    bool ObserveMainCamera(UnityResolve::UnityType::Camera*& ret) {
#ifdef GKMS_WINDOWS
        if (AreVrUnityCameraDiagnosticsEnabled() && !sourceCameraQueryActive) {
            sourceAdmissionMainCalls.fetch_add(1U, std::memory_order_relaxed);
            if (ret) sourceAdmissionMainNonNull.fetch_add(1U, std::memory_order_relaxed);
            sourceAdmissionLastMain.store(ret, std::memory_order_relaxed);
        }
        try {
            TrackVrSourceCamera(ret);
        } catch (...) {
            ReportVrUnityHookException();
        }
        if (void* overrideCam = unityStereoRenderer.MainCameraOverride()) {
            ret = reinterpret_cast<UnityResolve::UnityType::Camera*>(overrideCam);
            return true;
        }
#endif
        return false;
    }
