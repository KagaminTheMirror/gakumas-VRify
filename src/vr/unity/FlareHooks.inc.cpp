// VR-owned integration, included by the patched upstream Hook translation unit.
#ifdef GKMS_WINDOWS
    // ProFlare first copies scene data into a per-camera NativeArray and only
    // then schedules its update job.  Bracket that synchronous copy so the
    // local helper can resize the eye-owned element entry after the authored
    // fields have been populated.  No scene-shared ProFlare field is changed.
    DEFINE_HOOK(void, ProFlareBatchForSRPData_ScheduleFlares,
                (void* self, bool isDirty, void* mtd)) {
        if (!unityStereoRenderer.ProjectionEquivalentProFlareReady()) {
            ProFlareBatchForSRPData_ScheduleFlares_Orig(self, isDirty, mtd);
            return;
        }
        const ProFlareEyeScheduleContext previous =
            proFlareEyeScheduleContext;
        proFlareEyeScheduleContext = {};
        EnsureProFlareProjectionLayout();
        void* camera = nullptr;
        std::size_t eye = 2U;
        float projectionScaleX = 1.0F;
        float projectionScaleY = 1.0F;
        float appliedScaleX = 1.0F;
        float appliedScaleY = 1.0F;
        if (TryReadProFlareBatchCamera(self, &camera) &&
            unityStereoRenderer.IsEyeCamera(camera)) {
            const bool projectionValid =
                unityStereoRenderer.TryGetEyeProjectionScale(
                    camera, &projectionScaleX, &projectionScaleY, &eye) &&
                gakumas::vr::camera::TryBuildVerticalUniformProjectionScale(
                    projectionScaleY, appliedScaleX, appliedScaleY);
            if (eye < proFlareEyeScheduleSamples.size()) {
                const std::uint64_t sample =
                    ++proFlareEyeScheduleSamples[eye];
                if (projectionValid) {
                    const float previousX = proFlareLastLoggedScaleX[eye];
                    const float previousY = proFlareLastLoggedScaleY[eye];
                    const bool moved = previousX > 0.0F && previousY > 0.0F &&
                        (std::abs(std::log(appliedScaleX / previousX)) >=
                             0.02F ||
                         std::abs(std::log(appliedScaleY / previousY)) >=
                             0.02F);
                    const bool logBatch = sample == 1U ||
                        sample % 300U == 0U || moved;
                    proFlareEyeScheduleContext.active = true;
                    proFlareEyeScheduleContext.logBatch = logBatch;
                    proFlareEyeScheduleContext.eye = eye;
                    proFlareEyeScheduleContext.sample = sample;
                    proFlareEyeScheduleContext.projectionScaleX =
                        projectionScaleX;
                    proFlareEyeScheduleContext.projectionScaleY =
                        projectionScaleY;
                    proFlareEyeScheduleContext.scaleX = appliedScaleX;
                    proFlareEyeScheduleContext.scaleY = appliedScaleY;
                    if (logBatch) {
                        proFlareLastLoggedScaleX[eye] = appliedScaleX;
                        proFlareLastLoggedScaleY[eye] = appliedScaleY;
                    }
                } else if (!proFlareMissingProjectionLogged[eye]) {
                    proFlareMissingProjectionLogged[eye] = true;
                    std::ostringstream stream;
                    stream << "[VR][fov] PRO_FLARE_BATCH_AUTHORED eye="
                           << (eye == 0U ? "left" : "right")
                           << " sample=" << sample
                           << " reason=no-current-projection fixedFallback=0";
                    static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
                }
            }
        }

        ProFlareBatchForSRPData_ScheduleFlares_Orig(self, isDirty, mtd);

        const ProFlareEyeScheduleContext completed =
            proFlareEyeScheduleContext;
        proFlareEyeScheduleContext = previous;
        if (completed.active && completed.logBatch) {
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(6)
                   << "[VR][fov] PRO_FLARE_BATCH_APPLIED eye="
                   << (completed.eye == 0U ? "left" : "right")
                   << " sample=" << completed.sample
                   << " projectionScaleX=" << completed.projectionScaleX
                   << " projectionScaleY=" << completed.projectionScaleY
                   << " scaleX=" << completed.scaleX
                   << " scaleY=" << completed.scaleY
                   << " axisPolicy=vertical-uniform"
                   << " elements=" << completed.elements
                   << " firstSize=" << completed.firstSizeX << ','
                   << completed.firstSizeY
                   << " firstWritten=" << completed.firstWrittenX << ','
                   << completed.firstWrittenY
                   << " firstElementScale=" << completed.firstElementScale
                   << " firstAnamorphic="
                   << completed.firstAnamorphicX << ','
                   << completed.firstAnamorphicY << ','
                   << completed.firstAnamorphicZ
                   << " sharedGlobalWrites=0";
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        }
    }

    DEFINE_HOOK(void, ProFlare_UpdateElementJobData,
                (UnityResolve::UnityType::Color tintColor,
                 float subScale,
                 float angle,
                 float position,
                 void* display0,
                 void* display1,
                 void* display2,
                 void* display3,
                 void* mtd)) {
        void* currentElement = nullptr;
        static_cast<void>(TryReadCurrentProFlareElement(
            display0, &currentElement));
        ProFlare_UpdateElementJobData_Orig(
            tintColor, subScale, angle, position,
            display0, display1, display2, display3, mtd);
        static_cast<void>(ScaleCurrentProFlareElementForEye(currentElement));
    }
#endif

    // Cinemachine 3（Unity 6）用 SetPositionAndRotation 一次性写相机位置+旋转，
    // set_position/set_rotation 拦截不到相机，必须在这里接管。
