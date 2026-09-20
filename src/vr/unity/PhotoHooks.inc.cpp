// VR-owned integration, included by the patched upstream Hook translation unit.
#ifdef GKMS_WINDOWS
    // Photo-scene lifecycle (right-A shutter routing). Start caches the
    // presenter; OnFinalize drops it. The Live cache is shared with LivePause;
    // the Photography cache lives in VrPhotoShutter. Targets proven offline in
    // evidence/a-button-photo/ and re-resolved on the live table at install.
    DEFINE_HOOK(void, LiveScenePresenter_Start, (void* self, void* mtd)) {
        gakumas::vr::NoteLiveScenePresenter(self);
        return LiveScenePresenter_Start_Orig(self, mtd);
    }

    DEFINE_HOOK(void, LiveScenePresenter_OnFinalize, (void* self, void* mtd)) {
        gakumas::vr::ForgetLiveScenePresenter(self);
        return LiveScenePresenter_OnFinalize_Orig(self, mtd);
    }

    DEFINE_HOOK(void, PhotographyScenePresenter_Start, (void* self, void* mtd)) {
        gakumas::vr::NotePhotographyScenePresenter(self);
        return PhotographyScenePresenter_Start_Orig(self, mtd);
    }

    DEFINE_HOOK(void, PhotographyScenePresenter_OnFinalize, (void* self, void* mtd)) {
        gakumas::vr::ForgetPhotographyScenePresenter(self);
        return PhotographyScenePresenter_OnFinalize_Orig(self, mtd);
    }

    // stereo.219 hardware: an idol-photography session produced zero
    // PHOTOGRAPHY_SCENE notes while the Live Start hook (same install block)
    // worked, so Start alone is not a reliable anchor for this presenter.
    // Extra anchors: SetEvent / SetPhotoCountView run on the presenter itself;
    // the scene model's IsInitialized setter fires a one-shot presenter scan.
    DEFINE_HOOK(void, PhotographyScenePresenter_SetEvent, (void* self, void* mtd)) {
        gakumas::vr::NotePhotographyScenePresenter(self);
        return PhotographyScenePresenter_SetEvent_Orig(self, mtd);
    }

    DEFINE_HOOK(void, PhotographyScenePresenter_SetPhotoCountView, (void* self, void* mtd)) {
        gakumas::vr::NotePhotographyScenePresenter(self);
        return PhotographyScenePresenter_SetPhotoCountView_Orig(self, mtd);
    }

    DEFINE_HOOK(void, PhotographySceneModel_set_IsInitialized, (void* self, bool value, void* mtd)) {
        gakumas::vr::NotePhotographySceneAnchorEvent();
        return PhotographySceneModel_set_IsInitialized_Orig(self, value, mtd);
    }

    // Ground truth for the "photo taken" toast: both photo scenes play this
    // per-capture thumbnail animation. Invoke success alone lies at the
    // 50-shot limit (OnPhotoButtonAsync returns without capturing).
    // Live's overload carries a by-value Vector3; on MSVC x64 a 12-byte
    // aggregate is passed by pointer, so both forward as opaque pointers.
    DEFINE_HOOK(void, LiveSceneView_PlayCapturePhotoAnimation,
                (void* self, void* data, void* position, void* mtd)) {
        gakumas::vr::NotePhotoCaptured();
        return LiveSceneView_PlayCapturePhotoAnimation_Orig(
            self, data, position, mtd);
    }

    DEFINE_HOOK(void, PhotographyPhotoContentView_PlayCapturePhotoAnimation,
                (void* self, void* data, void* mtd)) {
        gakumas::vr::NotePhotoCaptured();
        return PhotographyPhotoContentView_PlayCapturePhotoAnimation_Orig(
            self, data, mtd);
    }
#endif
