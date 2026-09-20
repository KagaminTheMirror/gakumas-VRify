// VR-owned integration, included by the patched upstream Hook translation unit.
    DEFINE_HOOK(bool, URPDOF_IsActive, (void* self)) {
        if (IsLocalifyFreeCameraEnabled()) return false;
        const bool originalActive = URPDOF_IsActive_Orig(self);
#ifdef GKMS_WINDOWS
        if (unityStereoRenderer.ShouldSuppressDepthOfField(self, originalActive)) {
            return false;
        }
#endif
        return originalActive;
    }
