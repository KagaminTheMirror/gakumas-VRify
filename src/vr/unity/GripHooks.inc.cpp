// VR-owned integration, included by the patched upstream Hook translation unit.
    // Grip transparency (.225): _needsClear/_clearColor only act on the
    // DrawFrameBuffer branch of UIRenderPass.Execute, and AddRenderPasses
    // rewrites DrawFrameBuffer on every enqueue. The prefix forces pass0
    // onto that branch while transparency is armed; disarmed it is a no-op.
    DEFINE_HOOK(void, UIRenderPass_Execute,
                (void* self, void* context, void* renderingData, void* mtd)) {
        gakumas::vr::UnityStereoRenderer::GripUiPassExecState saved{};
        const bool modified =
            gakumas::vr::GripUiPassExecuteEnter(self, saved);
        void* camera = unityStereoRenderer.CurrentCamera();
        gakumas::vr::GripBlurSourceScope blurScope(self,
            modified && camera && !unityStereoRenderer.IsEyeCamera(camera));
        {
            UIRenderPass_Execute_Orig(self, context, renderingData, mtd);
        }
        if (modified) {
            gakumas::vr::GripUiPassExecuteExit(self, saved);
        }
    }
