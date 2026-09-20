// VR-owned integration, included by the patched upstream Hook translation unit.
    bool AfterEndCamera(void* camera) {
        bool ownedByVrQueue = false;
#ifdef GKMS_WINDOWS
        try {
            ownedByVrQueue = unityStereoRenderer.OnEndCamera(camera);
            if (gakumas::vr::LiveSourcePhotoProtectionActive()) {
                SetFpHeadColorSkip(gakumas::vr::camera::IsVrFreeCameraFirstPerson(),
                    "auto-photo-camera-end");
            }
            if (!ownedByVrQueue) {
                ObserveUnityCameraRender(camera);
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif

        return ownedByVrQueue;
    }
