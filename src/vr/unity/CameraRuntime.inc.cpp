// VR-owned integration, included by the patched upstream Hook translation unit.
#ifdef GKMS_WINDOWS
    struct UnityCameraDiagnosticRecord {
        std::string name;
        std::uint64_t renderCount = 0;
        bool mainCamera = false;
        bool descriptorLogged = false;
        bool mainSelectionLogged = false;
    };

    std::unordered_map<void*, UnityCameraDiagnosticRecord> unityCameraDiagnosticRecords{};
    std::mutex vrCameraStateMutex{};
    void* vrSourceCamera = nullptr;
    std::atomic<std::uint64_t> sourceAdmissionMainCalls = 0;
    std::atomic<std::uint64_t> sourceAdmissionMainNonNull = 0;
    std::atomic<void*> sourceAdmissionLastMain = nullptr;
    std::atomic<void*> sourceCameraMainMethod = nullptr;
    thread_local bool sourceCameraQueryActive = false;
    std::uint64_t unityCameraRenderCallbacks = 0;
    std::atomic<std::uint64_t> cinemachinePoseSamples = 0;
    std::atomic<std::uint64_t> cinemachineInvalidPoseSamples = 0;
    std::atomic_bool vrUnityHookExceptionLogged = false;
    std::atomic_bool unityCameraDiagnosticMarkerLogged = false;
    std::atomic_bool unityCameraMainHookReady = false;
    std::atomic_bool unityCinemachineHookReady = false;
    std::atomic_bool unityCameraRenderHookReady = false;
    std::atomic_bool unityRenderPipelineGuardReady = false;
    std::atomic_bool unityRenderLoopHookHitLogged = false;
    std::atomic_bool vrSourceCameraEyeIgnoredLogged = false;
    std::atomic_bool unityCameraDiagnosticHooksConfigured = false;
    std::atomic_bool cinemachineStateReadsEnabled = true;
    std::atomic_bool cinemachineStateReadFailureLogged = false;
    std::atomic_bool produceTransitionLayoutLogged = false;
    std::mutex vrHeadPoseBridgeMutex{};
    gakumas::vr::pose::RelativePoseBridge vrHeadPoseBridge{};
    std::atomic_bool vrHeadPoseWritesEnabled = true;
    std::atomic_bool vrHeadPoseWriteFailureLogged = false;
    std::uint64_t vrHeadPoseAppliedSamples = 0;
    std::mutex vrStereoCameraFrameMutex{};
    gakumas::vr::UnityStereoCameraFrame vrStereoCameraFrame{};
    bool vrStereoCameraFrameValid = false;
    gakumas::vr::UnityStereoRenderer unityStereoRenderer{};
    void SetFpHeadColorSkip(bool hidden, const char* reason) noexcept;
    void LogFpHeadActorShadowTag(void* pass) noexcept;
    thread_local std::uint32_t unityRenderPipelineDepth = 0;
    thread_local std::uint32_t unityRenderLoopDepth = 0;
    UnityResolve::Method* eyePassEventGetter = nullptr;

    struct PendingCinemachineObservation {
        bool sampled = false;
        bool stateValid = false;
        std::uint64_t sample = 0U;
    };

    struct ProFlareProjectionLayout {
        bool attempted = false;
        bool logged = false;
        bool ready = false;
        bool displayValueType = false;
        bool elementValueType = false;
        bool scheduleShape = false;
        bool updateHelperShape = false;
        bool scaleTelemetry = false;
        bool anamorphicTelemetry = false;
        std::int32_t batchCamera = -1;
        std::int32_t displayElementPointer = -1;
        std::int32_t elementScale = -1;
        std::int32_t elementSize = -1;
        std::int32_t elementAnamorphic = -1;
        std::string elementSizeType{};
        std::string elementAnamorphicType{};
        UnityResolve::Method* scheduleFlares = nullptr;
        UnityResolve::Method* updateElementJobData = nullptr;
    };

    struct ProFlareEyeScheduleContext {
        bool active = false;
        bool logBatch = false;
        std::size_t eye = 0U;
        std::uint64_t sample = 0U;
        std::uint32_t elements = 0U;
        float projectionScaleX = 1.0F;
        float projectionScaleY = 1.0F;
        float scaleX = 1.0F;
        float scaleY = 1.0F;
        float firstSizeX = 0.0F;
        float firstSizeY = 0.0F;
        float firstWrittenX = 0.0F;
        float firstWrittenY = 0.0F;
        float firstElementScale = 0.0F;
        float firstAnamorphicX = 0.0F;
        float firstAnamorphicY = 0.0F;
        float firstAnamorphicZ = 0.0F;
    };

    ProFlareProjectionLayout proFlareProjectionLayout{};
    thread_local ProFlareEyeScheduleContext proFlareEyeScheduleContext{};
    std::array<std::uint64_t, 2> proFlareEyeScheduleSamples{};
    std::array<float, 2> proFlareLastLoggedScaleX{};
    std::array<float, 2> proFlareLastLoggedScaleY{};
    std::array<bool, 2> proFlareMissingProjectionLogged{};

    struct ActorShadowStereoReuseState {
        UnityResolve::Field* featurePass = nullptr;
        UnityResolve::Field* shadowData = nullptr;
        UnityResolve::Field* shadowBias = nullptr;
        UnityResolve::Field* supportsShadow = nullptr;
        UnityResolve::Field* clearFlag = nullptr;
        UnityResolve::Field* actorShadowsKeyword = nullptr;
        UnityResolve::Field* startDistance2 = nullptr;
        UnityResolve::Field* endDistance2 = nullptr;
        UnityResolve::Field* fade = nullptr;
        UnityResolve::Field* strength = nullptr;
        std::int32_t dataFieldHeader = 0;
        UnityResolve::Method* setupReceiver = nullptr;
        UnityResolve::Method* setKeyword = nullptr;
        UnityResolve::Method* configureClear = nullptr;
        std::array<std::byte, 512> leftData{};
        std::size_t dataSize = 0;
        void* leftPass = nullptr;
        bool leftReady = false;
        bool leftSupportsShadow = false;
        std::uint64_t leftCaptures = 0;
        std::uint64_t rightReuses = 0;
        std::uint64_t rightClearPreserves = 0;
        float leftStartDistance2 = 0.0F;
        float leftEndDistance2 = 0.0F;
        float leftFade = 0.0F;
        float leftStrength = 0.0F;
        bool leftDistanceDataReady = false;
    };
    ActorShadowStereoReuseState actorShadowStereoReuse{};
    void ResetActorShadowLeftReuse() noexcept;

    struct EyeRenderObjectsPassFields {
        UnityResolve::Class* passClass = nullptr;
        UnityResolve::Field* renderQueueType = nullptr;
        UnityResolve::Field* filteringSettings = nullptr;
        UnityResolve::Field* cameraSettings = nullptr;
        UnityResolve::Field* profilerTag = nullptr;
        UnityResolve::Field* overrideMaterial = nullptr;
        UnityResolve::Field* overrideMaterialPassIndex = nullptr;
        UnityResolve::Field* overrideShader = nullptr;
        UnityResolve::Field* overrideShaderPassIndex = nullptr;
        UnityResolve::Class* filteringClass = nullptr;
        UnityResolve::Field* renderQueueRange = nullptr;
        UnityResolve::Field* layerMask = nullptr;
        UnityResolve::Field* renderingLayerMask = nullptr;
        UnityResolve::Field* excludeMotionVectorObjects = nullptr;
        UnityResolve::Class* renderQueueRangeClass = nullptr;
        UnityResolve::Field* lowerBound = nullptr;
        UnityResolve::Field* upperBound = nullptr;

        bool Ready() const noexcept {
            return passClass != nullptr && renderQueueType != nullptr &&
                filteringSettings != nullptr && cameraSettings != nullptr &&
                profilerTag != nullptr && overrideMaterial != nullptr &&
                overrideMaterialPassIndex != nullptr && overrideShader != nullptr &&
                overrideShaderPassIndex != nullptr && filteringClass != nullptr &&
                renderQueueRange != nullptr && layerMask != nullptr &&
                renderingLayerMask != nullptr &&
                excludeMotionVectorObjects != nullptr &&
                renderQueueRangeClass != nullptr && lowerBound != nullptr &&
                upperBound != nullptr;
        }
    };

    EyeRenderObjectsPassFields eyeRenderObjectsPassFields{};
    std::atomic<void*> eyeRenderObjectsLastInstance{nullptr};
    int eyeRenderObjectsLogs[3]{};
    int eyeRenderObjectsLastAction[3]{-1, -1, -1};
    int proFlareRenderLogs[3]{};

    bool IsVrUnityRuntimeEnabled() noexcept {
        return Config::vrRuntimeStartupEnabled && !Config::vrNativeOnly;
    }

    bool AreVrUnityCameraDiagnosticsEnabled() noexcept {
        return Config::vrDiagnosticsStartupEnabled && !Config::vrNativeOnly;
    }

    bool DoesVrOwnCamera() noexcept {
        return IsVrUnityRuntimeEnabled() && Config::vrHeadPoseEnabled;
    }

    bool IsVrHeadPoseBridgeEnabled() noexcept {
        return DoesVrOwnCamera() &&
            cinemachineStateReadsEnabled.load(std::memory_order_acquire) &&
            vrHeadPoseWritesEnabled.load(std::memory_order_acquire);
    }

    bool IsLocalifyFreeCameraEnabled() noexcept {
        // In combined mode, VR owns the camera transform. Localify translation
        // and its non-camera features remain enabled, but its legacy free-camera
        // writers stay inert so two controllers never fight over one Transform.
        return Config::enableFreeCamera && !Config::vrRuntimeStartupEnabled;
    }

    void InvalidateVrStereoCameraFrame() noexcept {
        std::lock_guard lock(vrStereoCameraFrameMutex);
        vrStereoCameraFrame = {};
        vrStereoCameraFrameValid = false;
    }

    bool IsSelectedVrMainCamera(void* camera) noexcept {
        if (camera == nullptr) {
            return false;
        }
        std::lock_guard lock(vrCameraStateMutex);
        return camera == vrSourceCamera;
    }

    gakumas::vr::UnityStereoCameraFrame ReadVrStereoCameraFrame() noexcept {
        std::lock_guard lock(vrStereoCameraFrameMutex);
        if (vrStereoCameraFrameValid) {
            return vrStereoCameraFrame;
        }
        // Grip-only runs still write the head pose into the source camera, so
        // the actor-shadow anchor needs the authored shot even when the eye
        // ladder is off. Everything else stays cleared.
        gakumas::vr::UnityStereoCameraFrame frame{};
        frame.sourcePose = vrStereoCameraFrame.sourcePose;
        frame.sourcePoseValid = vrStereoCameraFrame.sourcePoseValid;
        frame.cinematicPose = vrStereoCameraFrame.cinematicPose;
        frame.cinematicPoseValid = vrStereoCameraFrame.cinematicPoseValid;
        return frame;
    }

    bool IsUnityRenderPipelineIdle() noexcept {
        return unityRenderPipelineGuardReady.load(std::memory_order_acquire) &&
            unityRenderLoopDepth == 0U;
    }

    void* InvokeCinemachineOutputCameraRaw(
        void* function,
        void* brain,
        void* methodInfo) noexcept {
        using GetOutputCamera = void* (*)(void*, void*);
        __try {
            return reinterpret_cast<GetOutputCamera>(function)(brain, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    void* ReadCinemachineOutputCamera(void* brain) noexcept {
        if (brain == nullptr) {
            return nullptr;
        }
        static UnityResolve::Method* outputCameraMethod = []() {
            auto* klass = Il2cppUtils::GetClass(
                "Cinemachine.dll", "Cinemachine", "CinemachineBrain");
            if (klass == nullptr) {
                return static_cast<UnityResolve::Method*>(nullptr);
            }
            UnityResolve::Method* exact = nullptr;
            for (auto* candidate : klass->methods) {
                if (candidate == nullptr || candidate->name != "get_OutputCamera" ||
                    candidate->static_function || !candidate->args.empty() ||
                    candidate->return_type == nullptr ||
                    candidate->return_type->name != "UnityEngine.Camera" ||
                    candidate->function == nullptr || candidate->address == nullptr) {
                    continue;
                }
                if (exact != nullptr) {
                    return static_cast<UnityResolve::Method*>(nullptr);
                }
                exact = candidate;
            }
            return exact;
        }();
        if (outputCameraMethod == nullptr) {
            return nullptr;
        }
        return InvokeCinemachineOutputCameraRaw(
            outputCameraMethod->function,
            brain,
            outputCameraMethod->address);
    }

    bool WriteUnityCameraDiagnosticEvent(std::string_view message) noexcept {
        if (AreVrUnityCameraDiagnosticsEnabled()) {
            return gakumas::vr::WriteVrLog(message);
        }
        return false;
    }

    void EnsureUnityCameraDiagnosticMarker() {
        if (!unityCameraDiagnosticHooksConfigured.load(std::memory_order_acquire) ||
            unityCameraDiagnosticMarkerLogged.load(std::memory_order_acquire)) {
            return;
        }
        std::ostringstream stream;
        stream << "[VR][camera] UNITY_CAMERA_DIAGNOSTICS_ENABLED readOnly="
               << !Config::vrHeadPoseEnabled
               << " headPoseBridge=" << Config::vrHeadPoseEnabled
               << " cameraMainHook="
               << unityCameraMainHookReady.load(std::memory_order_relaxed)
               << " cinemachineHook="
               << unityCinemachineHookReady.load(std::memory_order_relaxed)
               << " renderHook="
               << unityCameraRenderHookReady.load(std::memory_order_relaxed)
               << " pipelineGuard="
               << unityRenderPipelineGuardReady.load(std::memory_order_relaxed)
               << " transformHooks=0 fovHooks=0";
        if (WriteUnityCameraDiagnosticEvent(stream.str())) {
            unityCameraDiagnosticMarkerLogged.store(true, std::memory_order_release);
        }
    }

    void ReportVrUnityHookException() noexcept {
        if (vrUnityHookExceptionLogged.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        static_cast<void>(gakumas::vr::WriteVrLog(
            "[VR][runtime] UNITY_HOOK_EXCEPTION; current callback failed"));
    }

    template <typename Return>
    bool InvokeUnityGetterRaw(
        UnityResolve::Method* method,
        void* self,
        Return* result) noexcept {
        if (method == nullptr || method->function == nullptr || self == nullptr ||
            result == nullptr) {
            return false;
        }
        using Getter = Return (*)(void*, void*);
        __try {
            *result = reinterpret_cast<Getter>(method->function)(self, method->address);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    template <typename Return>
    std::optional<Return> InvokeUnityGetter(
        UnityResolve::Method* method,
        void* self) noexcept {
        Return result{};
        if (!InvokeUnityGetterRaw(method, self, &result)) {
            return std::nullopt;
        }
        return result;
    }

    bool TryCopyUnityString(
        const UnityResolve::UnityType::String* value,
        wchar_t* output,
        std::size_t capacity,
        int* copiedLength) noexcept {
        if (value == nullptr || output == nullptr || capacity < 2U ||
            copiedLength == nullptr) {
            return false;
        }
        __try {
            const int length = value->length;
            if (length < 0 || length > 4096) {
                return false;
            }
            const int copyLength = length < static_cast<int>(capacity - 1U)
                ? length
                : static_cast<int>(capacity - 1U);
            for (int index = 0; index < copyLength; ++index) {
                output[index] = value->start_char[index];
            }
            output[copyLength] = L'\0';
            *copiedLength = copyLength;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::string ReadUnityStringValue(
        const UnityResolve::UnityType::String* value) {
        if (value == nullptr) {
            return {};
        }
        std::array<wchar_t, 97> wideName{};
        int wideLength = 0;
        if (!TryCopyUnityString(value, wideName.data(), wideName.size(), &wideLength) ||
            wideLength == 0) {
            return {};
        }
        const int utf8Length = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            wideName.data(),
            wideLength,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (utf8Length <= 0) {
            return {};
        }
        std::string name(static_cast<std::size_t>(utf8Length), '\0');
        if (WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                wideName.data(),
                wideLength,
                name.data(),
                utf8Length,
                nullptr,
                nullptr) != utf8Length) {
            return {};
        }
        for (char& character : name) {
            if (static_cast<unsigned char>(character) < 0x20U) {
                character = ' ';
            }
        }
        if (name.size() > 96U) {
            name.resize(96U);
        }
        return name;
    }

    std::string ReadUnityObjectName(void* object) {
        static auto* method = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll",
            "UnityEngine",
            "Object",
            "get_name");
        const auto value = InvokeUnityGetter<UnityResolve::UnityType::String*>(method, object);
        return value.has_value() ? ReadUnityStringValue(*value) : std::string{};
    }

    template <typename Value>
    std::optional<Value> ReadUnityInstanceField(
        void* instance,
        const UnityResolve::Field* field) noexcept {
        if (instance == nullptr || field == nullptr || field->static_field ||
            field->offset < 0) {
            return std::nullopt;
        }
        Value value{};
        __try {
            std::memcpy(
                &value,
                static_cast<const std::byte*>(instance) + field->offset,
                sizeof(value));
            return value;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return std::nullopt;
        }
    }

    template <typename Value>
    std::optional<Value> ReadUnityValueField(
        void* value,
        const UnityResolve::Field* field) noexcept {
        // UnityResolve reports value-type fields with the 0x10 boxed-object
        // header included. Embedded structs have no header, so nested reads
        // must use the unboxed offset.
        constexpr std::int32_t boxedHeader =
            static_cast<std::int32_t>(sizeof(void*) * 2U);
        if (value == nullptr || field == nullptr || field->static_field ||
            field->offset < boxedHeader) {
            return std::nullopt;
        }
        Value result{};
        __try {
            std::memcpy(
                &result,
                static_cast<const std::byte*>(value) +
                    (field->offset - boxedHeader),
                sizeof(result));
            return result;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return std::nullopt;
        }
    }

    bool TryLogUnityCameraDescriptor(
        void* camera,
        UnityCameraDiagnosticRecord& record) {
        static auto* getDepth = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_depth");
        static auto* getFov = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_fieldOfView");
        static auto* getNear = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_nearClipPlane");
        static auto* getFar = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_farClipPlane");
        static auto* getOrthographic = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_orthographic");
        static auto* getCullingMask = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_cullingMask");
        static auto* getTargetTexture = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_targetTexture");
        static auto* getPixelWidth = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_pixelWidth");
        static auto* getPixelHeight = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_pixelHeight");

        const auto depth = InvokeUnityGetter<float>(getDepth, camera);
        const auto fov = InvokeUnityGetter<float>(getFov, camera);
        const auto nearClip = InvokeUnityGetter<float>(getNear, camera);
        const auto farClip = InvokeUnityGetter<float>(getFar, camera);
        const auto orthographic = InvokeUnityGetter<bool>(getOrthographic, camera);
        const auto cullingMask = InvokeUnityGetter<int>(getCullingMask, camera);
        const auto targetTexture = InvokeUnityGetter<void*>(getTargetTexture, camera);
        const auto pixelWidth = InvokeUnityGetter<int>(getPixelWidth, camera);
        const auto pixelHeight = InvokeUnityGetter<int>(getPixelHeight, camera);

        std::ostringstream stream;
        stream << "[VR][camera] CAMERA_DISCOVERED id=0x" << std::hex
               << reinterpret_cast<std::uintptr_t>(camera) << std::dec
               << " name=\"" << record.name << "\""
               << " main=" << record.mainCamera;
        stream << std::fixed << std::setprecision(3);
        if (depth.has_value()) {
            stream << " depth=" << *depth;
        }
        if (fov.has_value()) {
            stream << " fov=" << *fov;
        }
        if (nearClip.has_value()) {
            stream << " near=" << *nearClip;
        }
        if (farClip.has_value()) {
            stream << " far=" << *farClip;
        }
        if (orthographic.has_value()) {
            stream << " orthographic=" << *orthographic;
        }
        if (cullingMask.has_value()) {
            stream << " cullingMask=0x" << std::hex
                   << static_cast<std::uint32_t>(*cullingMask) << std::dec;
        }
        if (targetTexture.has_value()) {
            stream << " targetTexture=0x" << std::hex
                   << reinterpret_cast<std::uintptr_t>(*targetTexture) << std::dec;
        }
        if (pixelWidth.has_value() && pixelHeight.has_value()) {
            stream << " pixels=" << *pixelWidth << 'x' << *pixelHeight;
        }
        return WriteUnityCameraDiagnosticEvent(stream.str());
    }

    UnityCameraDiagnosticRecord& EnsureUnityCameraDiagnosticRecord(void* camera) {
        auto [entry, inserted] = unityCameraDiagnosticRecords.try_emplace(camera);
        auto& record = entry->second;
        if (inserted) {
            record.name = ReadUnityObjectName(camera);
            record.mainCamera = camera == vrSourceCamera;
        }
        if (!record.descriptorLogged &&
            unityCameraDiagnosticMarkerLogged.load(std::memory_order_acquire) &&
            TryLogUnityCameraDescriptor(camera, record)) {
            record.descriptorLogged = true;
        }
        return record;
    }

    void EnsureUnityMainSelectionLog(
        void* camera,
        UnityCameraDiagnosticRecord& record) {
        if (!record.mainCamera || record.mainSelectionLogged ||
            !unityCameraDiagnosticMarkerLogged.load(std::memory_order_acquire)) {
            return;
        }
        std::ostringstream stream;
        stream << "[VR][camera] MAIN_CAMERA_SELECTED id=0x" << std::hex
               << reinterpret_cast<std::uintptr_t>(camera) << std::dec
               << " name=\"" << record.name << "\"";
        if (WriteUnityCameraDiagnosticEvent(stream.str())) {
            record.mainSelectionLogged = true;
        }
    }

    bool TryReadCinemachineState(
        const void* state,
        UnityResolve::UnityType::Vector3* position,
        UnityResolve::UnityType::Quaternion* rotation,
        float* fov) noexcept {
        if (state == nullptr || position == nullptr || rotation == nullptr ||
            fov == nullptr) {
            return false;
        }
        const auto* bytes = static_cast<const unsigned char*>(state);
        __try {
            std::memcpy(position, bytes + 0x48, sizeof(*position));
            std::memcpy(rotation, bytes + 0x54, sizeof(*rotation));
            std::memcpy(fov, bytes + 0x00, sizeof(*fov));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool FieldTypeContains(
        const UnityResolve::Field* field,
        std::string_view expected) noexcept {
        return field != nullptr && field->type != nullptr &&
            field->type->name.find(expected) != std::string::npos;
    }

    void EnsureProFlareProjectionLayout() noexcept {
        auto& layout = proFlareProjectionLayout;
        if (!layout.attempted) {
            layout.attempted = true;
            constexpr std::int32_t kBoxedHeader = 0x10;
            auto* assembly = UnityResolve::Get("ProFlare.Runtime.dll");
            auto* batchClass = assembly != nullptr
                ? assembly->Get("ProFlareBatchForSRPData", "") : nullptr;
            if (batchClass == nullptr && assembly != nullptr) {
                batchClass = assembly->Get("ProFlareBatchForSRPData");
            }
            auto* elementClass = assembly != nullptr
                ? assembly->Get("ProFlareUpdateElementData", "") : nullptr;
            if (elementClass == nullptr && assembly != nullptr) {
                elementClass = assembly->Get("ProFlareUpdateElementData");
            }
            auto* displayClass = assembly != nullptr
                ? assembly->Get("<>c__DisplayClass48_0", "") : nullptr;
            if (displayClass == nullptr && assembly != nullptr) {
                displayClass = assembly->Get("<>c__DisplayClass48_0");
            }

            const auto* batchCamera = batchClass != nullptr
                ? batchClass->Get<UnityResolve::Field>("camera") : nullptr;
            const auto* displayElement = displayClass != nullptr
                ? displayClass->Get<UnityResolve::Field>(
                      "pFlareUpdateElement") : nullptr;
            const auto* elementScale = elementClass != nullptr
                ? elementClass->Get<UnityResolve::Field>("Scale") : nullptr;
            const auto* elementSize = elementClass != nullptr
                ? elementClass->Get<UnityResolve::Field>("Size") : nullptr;
            const auto* elementAnamorphic = elementClass != nullptr
                ? elementClass->Get<UnityResolve::Field>("Anamorphic") : nullptr;

            layout.displayValueType = displayClass != nullptr &&
                displayClass->address != nullptr &&
                UnityResolve::Invoke<bool>(
                    "il2cpp_class_is_valuetype", displayClass->address);
            layout.elementValueType = elementClass != nullptr &&
                elementClass->address != nullptr &&
                UnityResolve::Invoke<bool>(
                    "il2cpp_class_is_valuetype", elementClass->address);
            layout.batchCamera = batchCamera != nullptr
                ? batchCamera->offset : -1;
            const auto unboxedOffset = [kBoxedHeader](
                                           const UnityResolve::Field* field) {
                return field != nullptr && field->offset >= kBoxedHeader
                    ? field->offset - kBoxedHeader : -1;
            };
            layout.displayElementPointer = unboxedOffset(displayElement);
            layout.elementScale = unboxedOffset(elementScale);
            layout.elementSize = unboxedOffset(elementSize);
            layout.elementAnamorphic = unboxedOffset(elementAnamorphic);
            layout.elementSizeType =
                elementSize != nullptr && elementSize->type != nullptr
                    ? elementSize->type->name : "?";
            layout.elementAnamorphicType =
                elementAnamorphic != nullptr &&
                        elementAnamorphic->type != nullptr
                    ? elementAnamorphic->type->name : "?";

            if (batchClass != nullptr) {
                for (auto* method : batchClass->methods) {
                    if (method == nullptr) {
                        continue;
                    }
                    if (method->name == "ScheduleFlares") {
                        if (layout.scheduleFlares != nullptr) {
                            layout.scheduleFlares = nullptr;
                            break;
                        }
                        layout.scheduleFlares = method;
                    }
                }
                for (auto* method : batchClass->methods) {
                    if (method != nullptr && method->name ==
                            "<ScheduleFlares>g__UpdateElementJobData|48_0") {
                        if (layout.updateElementJobData != nullptr) {
                            layout.updateElementJobData = nullptr;
                            break;
                        }
                        layout.updateElementJobData = method;
                    }
                }
            }
            const auto argContains = [](const UnityResolve::Method* method,
                                        std::size_t index,
                                        std::string_view expected) {
                return method != nullptr && index < method->args.size() &&
                    method->args[index] != nullptr &&
                    method->args[index]->pType != nullptr &&
                    method->args[index]->pType->name.find(expected) !=
                        std::string::npos;
            };
            layout.scheduleShape = layout.scheduleFlares != nullptr &&
                !layout.scheduleFlares->static_function &&
                layout.scheduleFlares->return_type != nullptr &&
                layout.scheduleFlares->return_type->name == "System.Void" &&
                layout.scheduleFlares->args.size() == 1U &&
                argContains(layout.scheduleFlares, 0U, "System.Boolean") &&
                layout.scheduleFlares->function != nullptr &&
                layout.scheduleFlares->address != nullptr;
            layout.updateHelperShape =
                layout.updateElementJobData != nullptr &&
                layout.updateElementJobData->static_function &&
                layout.updateElementJobData->return_type != nullptr &&
                layout.updateElementJobData->return_type->name == "System.Void" &&
                layout.updateElementJobData->args.size() == 8U &&
                argContains(layout.updateElementJobData, 0U, "UnityEngine.Color") &&
                argContains(layout.updateElementJobData, 1U, "System.Single") &&
                argContains(layout.updateElementJobData, 2U, "System.Single") &&
                argContains(layout.updateElementJobData, 3U, "System.Single") &&
                argContains(layout.updateElementJobData, 4U,
                            "<>c__DisplayClass48_0") &&
                argContains(layout.updateElementJobData, 5U,
                            "<>c__DisplayClass48_1") &&
                argContains(layout.updateElementJobData, 6U,
                            "<>c__DisplayClass48_2") &&
                argContains(layout.updateElementJobData, 7U,
                            "<>c__DisplayClass48_3") &&
                layout.updateElementJobData->function != nullptr &&
                layout.updateElementJobData->address != nullptr;

            layout.scaleTelemetry =
                FieldTypeContains(elementScale, "System.Single") &&
                layout.elementScale >= 0 && layout.elementScale <= 0x200;
            layout.anamorphicTelemetry =
                FieldTypeContains(elementAnamorphic, "UnityEngine.Vector3") &&
                layout.elementAnamorphic >= 0 &&
                layout.elementAnamorphic <= 0x200;
            // Functional readiness contains only fields used by the write.
            // Scale/Anamorphic are optional log telemetry; `.173` incorrectly
            // made Anamorphic:Vector2 mandatory and disabled the whole hook.
            layout.ready = layout.displayValueType &&
                layout.elementValueType && layout.scheduleShape &&
                layout.updateHelperShape &&
                FieldTypeContains(batchCamera, "UnityEngine.Camera") &&
                FieldTypeContains(displayElement,
                                  "ProFlareUpdateElementData") &&
                FieldTypeContains(elementSize, "UnityEngine.Vector2") &&
                layout.batchCamera > 0 &&
                layout.displayElementPointer >= 0 &&
                layout.displayElementPointer <= 0x100 &&
                layout.elementSize >= 0 && layout.elementSize <= 0x200;
        }
        if (!layout.logged) {
            std::ostringstream stream;
            stream << "[VR][fov] PRO_FLARE_BATCH_LAYOUT ready="
                   << (layout.ready ? 1 : 0)
                   << " displayValueType=" << (layout.displayValueType ? 1 : 0)
                   << " elementValueType=" << (layout.elementValueType ? 1 : 0)
                   << " scheduleShape=" << (layout.scheduleShape ? 1 : 0)
                   << " helperShape=" << (layout.updateHelperShape ? 1 : 0)
                   << " scaleTelemetry=" << (layout.scaleTelemetry ? 1 : 0)
                   << " anamorphicTelemetry="
                   << (layout.anamorphicTelemetry ? 1 : 0)
                   << " camera=" << layout.batchCamera
                   << " displayElement=" << layout.displayElementPointer
                   << " scale=" << layout.elementScale
                   << " size=" << layout.elementSize
                   << " anamorphic=" << layout.elementAnamorphic
                   << " sizeType=" << layout.elementSizeType
                   << " anamorphicType=" << layout.elementAnamorphicType
                   << " boxedHeader=16";
            layout.logged = gakumas::vr::WriteVrLog(stream.str());
        }
    }

    bool TryReadProFlareBatchCamera(void* batch, void** camera) noexcept {
        if (batch == nullptr || camera == nullptr ||
            !proFlareProjectionLayout.ready ||
            proFlareProjectionLayout.batchCamera <= 0) {
            return false;
        }
        __try {
            std::memcpy(
                camera,
                static_cast<const unsigned char*>(batch) +
                    proFlareProjectionLayout.batchCamera,
                sizeof(*camera));
            return *camera != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool TryReadCurrentProFlareElement(
        void* display0,
        void** element) noexcept {
        const auto& layout = proFlareProjectionLayout;
        if (display0 == nullptr || element == nullptr || !layout.ready) {
            return false;
        }
        __try {
            std::memcpy(
                element,
                static_cast<const unsigned char*>(display0) +
                    layout.displayElementPointer,
                sizeof(*element));
            return *element != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool ScaleCurrentProFlareElementForEye(void* element) noexcept {
        auto& context = proFlareEyeScheduleContext;
        const auto& layout = proFlareProjectionLayout;
        if (element == nullptr || !context.active || !layout.ready ||
            !std::isfinite(context.scaleX) ||
            !std::isfinite(context.scaleY) || context.scaleX <= 0.0F ||
            context.scaleY <= 0.0F || context.scaleX > 64.0F ||
            context.scaleY > 64.0F) {
            return false;
        }
        UnityResolve::UnityType::Vector2 authored{};
        float elementScale = 0.0F;
        UnityResolve::UnityType::Vector3 anamorphic{};
        __try {
            auto* elementBytes = static_cast<unsigned char*>(element);
            std::memcpy(
                &authored, elementBytes + layout.elementSize,
                sizeof(authored));
            if (layout.scaleTelemetry) {
                std::memcpy(
                    &elementScale, elementBytes + layout.elementScale,
                    sizeof(elementScale));
            }
            if (layout.anamorphicTelemetry) {
                std::memcpy(
                    &anamorphic, elementBytes + layout.elementAnamorphic,
                    sizeof(anamorphic));
            }
            if (!std::isfinite(authored.x) || !std::isfinite(authored.y) ||
                std::abs(authored.x) > 1000000.0F ||
                std::abs(authored.y) > 1000000.0F) {
                return false;
            }
            UnityResolve::UnityType::Vector2 written{};
            if (!gakumas::vr::camera::TryScaleViewportSize(
                    authored.x, authored.y, context.scaleX, context.scaleY,
                    written.x, written.y)) {
                return false;
            }
            std::memcpy(
                elementBytes + layout.elementSize, &written,
                sizeof(written));
            if (context.elements == 0U) {
                context.firstSizeX = authored.x;
                context.firstSizeY = authored.y;
                context.firstWrittenX = written.x;
                context.firstWrittenY = written.y;
                context.firstElementScale = elementScale;
                context.firstAnamorphicX = anamorphic.x;
                context.firstAnamorphicY = anamorphic.y;
                context.firstAnamorphicZ = anamorphic.z;
            }
            ++context.elements;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }





    bool IsValidCinemachinePose(
        const UnityResolve::UnityType::Vector3& position,
        const UnityResolve::UnityType::Quaternion& rotation,
        float fov) noexcept {
        const float rotationNormSquared =
            rotation.x * rotation.x + rotation.y * rotation.y +
            rotation.z * rotation.z + rotation.w * rotation.w;
        return std::isfinite(position.x) && std::isfinite(position.y) &&
            std::isfinite(position.z) && std::abs(position.x) < 1000000.0F &&
            std::abs(position.y) < 1000000.0F && std::abs(position.z) < 1000000.0F &&
            std::isfinite(rotation.x) && std::isfinite(rotation.y) &&
            std::isfinite(rotation.z) && std::isfinite(rotation.w) &&
            std::isfinite(rotationNormSquared) && rotationNormSquared > 0.25F &&
            rotationNormSquared < 2.25F && std::isfinite(fov) &&
            fov > 0.1F && fov < 179.9F;
    }

    bool TryWriteCinemachineRawPose(
        void* state,
        const UnityResolve::UnityType::Vector3& position,
        const UnityResolve::UnityType::Quaternion& rotation) noexcept {
        if (state == nullptr) {
            return false;
        }
        auto* bytes = static_cast<unsigned char*>(state);
        __try {
            std::memcpy(bytes + 0x48, &position, sizeof(position));
            std::memcpy(bytes + 0x54, &rotation, sizeof(rotation));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void ReportVrHeadPoseWriteFailure() noexcept {
        if (vrHeadPoseWriteFailureLogged.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        WriteUnityCameraDiagnosticEvent(
            "[VR][camera] HEAD_POSE_WRITE_FAILED; camera writes disabled");
    }

    // ---- VR free camera (upstream FREE/FOLLOW/FIRST_PERSON migration) ----
    // The rig replaces Cinemachine's requested pose as the base the headset
    // delta is composed onto. Everything here runs on the Unity main thread
    // (CinemachineBrain.PushStateToUnityCamera and
    // CampusActorController.LateUpdate), so the anchor globals need no locks;
    // only the controller input crosses threads via VrCameraInputMailbox.
    gakumas::vr::camera::VrFreeCameraRig vrFreeCameraRig;
    UnityResolve::UnityType::Vector3 vrCameraAnchorPosition{};
    UnityResolve::UnityType::Vector3 vrCameraAnchorForward{};
    UnityResolve::UnityType::Quaternion vrCameraAnchorRotation{};
    std::int64_t vrCameraAnchorTimeNanoseconds = 0;
    bool vrFreeCameraLastComposedValid = false;
    gakumas::vr::pose::Pose vrFreeCameraLastComposed{};

    bool IsVrCameraAnchorFresh(std::int64_t nowNanoseconds) noexcept {
        constexpr std::int64_t kAnchorStaleNanoseconds = 500'000'000LL;
        return vrCameraAnchorTimeNanoseconds != 0 &&
            nowNanoseconds - vrCameraAnchorTimeNanoseconds <
                kAnchorStaleNanoseconds;
    }

    // Actor indices are allocated globally by CampusActorManager (LRU cache +
    // reusable-index stack), so a scene's actors are NOT guaranteed to start
    // at 0 — a Live's members can occupy any indices while followCharaIndex
    // still points at a departed lobby actor. Track which indices actually
    // ran CampusActorController.LateUpdate recently so the rig can adopt and
    // cycle real actors instead of hoping for index 0.
    struct VrCameraSeenActorSlot {
        int index = -1;
        std::int64_t lastSeenNanoseconds = 0;
    };
    constexpr std::size_t kVrCameraSeenActorCapacity = 16;
    VrCameraSeenActorSlot vrCameraSeenActors[kVrCameraSeenActorCapacity]{};
    constexpr std::int64_t kVrCameraSeenStaleNanoseconds = 1'000'000'000LL;

    void RecordVrCameraActorIndex(int index, std::int64_t now) noexcept {
        VrCameraSeenActorSlot* stalest = &vrCameraSeenActors[0];
        for (auto& slot : vrCameraSeenActors) {
            if (slot.index == index) {
                slot.lastSeenNanoseconds = now;
                return;
            }
            if (slot.lastSeenNanoseconds < stalest->lastSeenNanoseconds) {
                stalest = &slot;
            }
        }
        stalest->index = index;
        stalest->lastSeenNanoseconds = now;
    }

    // Fills `out` (capacity kVrCameraSeenActorCapacity) with the indices seen
    // within the last second, ascending. Returns the count.
    std::size_t CollectFreshVrCameraActorIndices(
        std::int64_t now,
        int* out) noexcept {
        std::size_t count = 0;
        for (const auto& slot : vrCameraSeenActors) {
            if (slot.index >= 0 &&
                now - slot.lastSeenNanoseconds < kVrCameraSeenStaleNanoseconds) {
                out[count++] = slot.index;
            }
        }
        std::sort(out, out + count);
        return count;
    }

    // Consumes the controller-input mailbox, advances the rig and, when a
    // mode is active, replaces the pose the head-pose bridge composes onto.
    void UpdateVrFreeCameraRig(
        const gakumas::vr::pose::Pose& gameRequested,
        gakumas::vr::pose::Pose& bridgeBase) {
        namespace vrcam = gakumas::vr::camera;

        static std::int64_t lastUpdateNanoseconds = 0;
        static bool countersInitialized = false;
        static std::uint32_t consumedModePresses = 0;
        static std::uint32_t consumedCharaPresses = 0;
        static std::uint32_t consumedResetPresses = 0;

        const std::int64_t now = gakumas::vr::pose::MonotonicNowNanoseconds();
        float dtSeconds = 0.0F;
        if (lastUpdateNanoseconds != 0 && now > lastUpdateNanoseconds) {
            dtSeconds = static_cast<float>(
                static_cast<double>(now - lastUpdateNanoseconds) * 1.0e-9);
        }
        lastUpdateNanoseconds = now;
        constexpr float kMaxFreeCameraDeltaSeconds = 0.10F;
        if (dtSeconds > kMaxFreeCameraDeltaSeconds) {
            dtSeconds = kMaxFreeCameraDeltaSeconds;
        }

        vrcam::VrFreeCameraCommands commands;
        commands.dtSeconds = dtSeconds;
        commands.fpDirectionFollow = Config::vrFpDirectionFollow;
        const int menuModeRequest = vrcam::ConsumeVrFreeCameraModeRequest();
        if (menuModeRequest >= 0) {
            commands.hasModeRequest = true;
            commands.modeRequest =
                static_cast<vrcam::VrFreeCameraMode>(menuModeRequest);
        }
        std::uint32_t charaPresses = 0;

        gakumas::vr::camera::VrCameraInputSample input;
        if (gakumas::vr::ReadVrCameraInput(input) && input.valid) {
            if (!countersInitialized) {
                // First valid sample after startup: adopt the cumulative
                // counters without treating history as fresh presses.
                consumedModePresses = input.modePressCount;
                consumedCharaPresses = input.charaPressCount;
                consumedResetPresses = input.resetPressCount;
                countersInitialized = true;
            }
            commands.modePresses = input.modePressCount - consumedModePresses;
            commands.resetPresses = input.resetPressCount - consumedResetPresses;
            charaPresses = input.charaPressCount - consumedCharaPresses;
            consumedModePresses = input.modePressCount;
            consumedCharaPresses = input.charaPressCount;
            consumedResetPresses = input.resetPressCount;

            gakumas::vr::pose::PoseAdmission admission{};
            const bool ticketOk =
                gakumas::vr::VrRuntime::Instance().CurrentPoseAdmission(admission) &&
                !input.cancelled && input.frameId == admission.frameId &&
                input.sessionGeneration == admission.sessionGeneration;
            if (ticketOk && !input.menuVisible) {
                commands.leftStickX = input.leftStickX;
                commands.leftStickY = input.leftStickY;
                commands.rightStickX = input.rightStickX;
                commands.rightStickY = input.rightStickY;
                commands.sprintHeld = input.sprintHeld;
            }
        }

        vrcam::VrFreeCameraAnchor anchor;
        anchor.valid = IsVrCameraAnchorFresh(now);
        if (anchor.valid) {
            anchor.position = {
                vrCameraAnchorPosition.x,
                vrCameraAnchorPosition.y,
                vrCameraAnchorPosition.z,
            };
            anchor.forward = {
                vrCameraAnchorForward.x,
                vrCameraAnchorForward.y,
                vrCameraAnchorForward.z,
            };
            anchor.rotationValid = true;
            anchor.rotation = {
                vrCameraAnchorRotation.x,
                vrCameraAnchorRotation.y,
                vrCameraAnchorRotation.z,
                vrCameraAnchorRotation.w,
            };
        }

        const auto result = vrFreeCameraRig.Update(
            commands,
            anchor,
            gameRequested,
            vrFreeCameraLastComposedValid,
            vrFreeCameraLastComposed);

        if (result.modeChanged) {
            std::ostringstream stream;
            stream << "[VR][camera] FREECAM_MODE mode="
                   << vrcam::VrFreeCameraModeName(result.mode)
                   << " source=left-x anchorFresh=" << (anchor.valid ? 1 : 0);
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        }
        if (result.resetApplied) {
            std::ostringstream stream;
            stream << "[VR][camera] FREECAM_RESET mode="
                   << vrcam::VrFreeCameraModeName(result.mode);
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        }
        {
            static bool lastSprintHeld = false;
            const bool sprinting =
                (result.mode == vrcam::VrFreeCameraMode::Free ||
                 result.mode == vrcam::VrFreeCameraMode::Follow) &&
                commands.sprintHeld;
            if (sprinting != lastSprintHeld) {
                std::ostringstream stream;
                stream << "[VR][camera] FREECAM_SPRINT held="
                       << (sprinting ? 1 : 0) << " mode="
                       << vrcam::VrFreeCameraModeName(result.mode);
                static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
                lastSprintHeld = sprinting;
            }
        }

        const bool boneAnchorMode =
            result.mode == vrcam::VrFreeCameraMode::Follow ||
            result.mode == vrcam::VrFreeCameraMode::FirstPerson;

        if (charaPresses > 0 && boneAnchorMode &&
            Config::vrCameraYButtonBone == 1) {
            // Menu setting: the Y button switches the FOLLOW anchor bone
            // instead of the character.
            const int bone =
                vrcam::CycleVrFreeCameraFollowBone(charaPresses);
            std::ostringstream stream;
            stream << "[VR][camera] FREECAM_BONE value=" << bone << " key="
                   << vrcam::VrFreeCameraBoneKey(bone);
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        } else if (charaPresses > 0 && boneAnchorMode) {
            // Cycle through the actor indices that actually exist right now
            // (Live members can occupy any global indices; see the seen-actor
            // table). Without live data, fall back to wrapping at 0.
            int seen[kVrCameraSeenActorCapacity];
            const std::size_t seenCount = CollectFreshVrCameraActorIndices(now, seen);
            if (seenCount > 0) {
                int next = seen[0];
                for (std::uint32_t press = 0; press < charaPresses; ++press) {
                    const int current = gakumas::vr::camera::FollowActorIndex();
                    next = seen[0];
                    for (std::size_t i = 0; i < seenCount; ++i) {
                        if (seen[i] > current) {
                            next = seen[i];
                            break;
                        }
                    }
                    gakumas::vr::camera::FollowActorIndex() = next;
                }
            } else if (!anchor.valid) {
                gakumas::vr::camera::FollowActorIndex() = 0;
            } else {
                gakumas::vr::camera::FollowActorIndex() += static_cast<int>(charaPresses);
            }
            std::ostringstream stream;
            stream << "[VR][camera] FREECAM_CHARA index="
                   << gakumas::vr::camera::FollowActorIndex()
                   << " anchorFresh=" << (anchor.valid ? 1 : 0)
                   << " seen=";
            for (std::size_t i = 0; i < seenCount; ++i) {
                if (i > 0) stream << ',';
                stream << seen[i];
            }
            if (seenCount == 0) stream << '-';
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        }

        // Self-heal a dead follow index: if the anchor has gone stale while
        // actors are demonstrably running (Live entered with a leftover lobby
        // index), adopt the lowest live index instead of freezing forever.
        static std::int64_t lastAdoptNanoseconds = 0;
        if (boneAnchorMode && !anchor.valid &&
            now - lastAdoptNanoseconds > 1'000'000'000LL) {
            int seen[kVrCameraSeenActorCapacity];
            const std::size_t seenCount = CollectFreshVrCameraActorIndices(now, seen);
            bool currentSeen = false;
            for (std::size_t i = 0; i < seenCount; ++i) {
                if (seen[i] == gakumas::vr::camera::FollowActorIndex()) {
                    currentSeen = true;
                    break;
                }
            }
            if (seenCount > 0 && !currentSeen) {
                lastAdoptNanoseconds = now;
                const int previous = gakumas::vr::camera::FollowActorIndex();
                gakumas::vr::camera::FollowActorIndex() = seen[0];
                std::ostringstream stream;
                stream << "[VR][camera] FREECAM_INDEX_ADOPT previous="
                       << previous << " adopted=" << seen[0] << " seen=";
                for (std::size_t i = 0; i < seenCount; ++i) {
                    if (i > 0) stream << ',';
                    stream << seen[i];
                }
                static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
            }
        }

        if (result.poseValid) {
            bridgeBase = result.rigPose;
        }
    }
    // ---- end VR free camera ----

    void ApplyVrHeadPose(void* brain, void* state) {
        if (!IsVrHeadPoseBridgeEnabled() || state == nullptr) {
            InvalidateVrStereoCameraFrame();
            return;
        }

        using UnityVector3 = UnityResolve::UnityType::Vector3;
        using UnityQuaternion = UnityResolve::UnityType::Quaternion;
        UnityVector3 gamePosition{};
        UnityQuaternion gameRotation{};
        float gameFov = 0.0F;
        if (!TryReadCinemachineState(
                state, &gamePosition, &gameRotation, &gameFov) ||
            !IsValidCinemachinePose(gamePosition, gameRotation, gameFov)) {
            return;
        }

        gakumas::vr::pose::StereoPoseSample trackedPose;
        gakumas::vr::pose::PoseAdmission admission{};
        if (!gakumas::vr::VrRuntime::Instance().CurrentPoseAdmission(admission) ||
            !gakumas::vr::VrRuntime::Instance().ReadStereoPoseForAdmission(
                admission, trackedPose)) {
            InvalidateVrStereoCameraFrame();
            return;
        }
        const gakumas::vr::pose::Pose gameRequested{
            {gamePosition.x, gamePosition.y, gamePosition.z},
            {gameRotation.x, gameRotation.y, gameRotation.z, gameRotation.w},
        };
        // The VR free camera substitutes its rig pose for the game pose when
        // a mode is active; in Off mode bridgeBase stays == gameRequested.
        gakumas::vr::pose::Pose bridgeBase = gameRequested;
        try {
            UpdateVrFreeCameraRig(gameRequested, bridgeBase);
        } catch (...) {
            ReportVrUnityHookException();
            bridgeBase = gameRequested;
        }
        gakumas::vr::pose::StereoComposedPose composed{};
        const float worldScale = Config::vrWorldScale;
        gakumas::vr::pose::BridgeUpdateResult result;
        {
            std::lock_guard lock(vrHeadPoseBridgeMutex);
            result = vrHeadPoseBridge.UpdateStereo(
                bridgeBase,
                trackedPose,
                gakumas::vr::pose::MonotonicNowNanoseconds(),
                -1,
                worldScale,
                composed,
                &admission);
            if (result == gakumas::vr::pose::BridgeUpdateResult::BaselineLatched) {
                std::ostringstream stream;
                stream << "[VR][camera] HEAD_POSE_BASELINE_LATCHED session="
                       << trackedPose.sessionGeneration
                       << " epoch=" << trackedPose.poseEpoch
                       << " referenceSpace=" << trackedPose.referenceSpaceType
                       << " brain=0x" << std::hex
                       << reinterpret_cast<std::uintptr_t>(brain) << std::dec
                       << " worldScale=" << std::fixed << std::setprecision(3)
                       << worldScale;
                WriteUnityCameraDiagnosticEvent(stream.str());
            }
        }
        if (result != gakumas::vr::pose::BridgeUpdateResult::Applied) {
            InvalidateVrStereoCameraFrame();
            return;
        }

        const UnityVector3 outputPosition(
            composed.center.position.x,
            composed.center.position.y,
            composed.center.position.z);
        const UnityQuaternion outputRotation(
            composed.center.orientation.x,
            composed.center.orientation.y,
            composed.center.orientation.z,
            composed.center.orientation.w);
        // Automatic Memory photos must see the game's authored source shot.
        // Still compose/publish the SAME free-rig + HMD pose below for the VR
        // eyes; never switch modes or reset the rig just to take these photos.
        const bool authoredPhotoSource = gakumas::vr::LiveSourcePhotoProtectionActive();
        static bool photoSourceBypassLogged = false;
        if (authoredPhotoSource != photoSourceBypassLogged) {
            photoSourceBypassLogged = authoredPhotoSource;
            static_cast<void>(gakumas::vr::WriteVrLog(authoredPhotoSource
                ? "[VR][photo] AUTO_PHOTO_SOURCE_POSE bypass=1 hmd=unchanged"
                : "[VR][photo] AUTO_PHOTO_SOURCE_POSE bypass=0 hmd=unchanged"));
        }
        if (!authoredPhotoSource && !TryWriteCinemachineRawPose(
                state, outputPosition, outputRotation)) {
            vrHeadPoseWritesEnabled.store(false, std::memory_order_release);
            InvalidateVrStereoCameraFrame();
            ReportVrHeadPoseWriteFailure();
            return;
        }

        // Remember the composed head pose: next frame's free-camera update
        // uses it as the movement basis and the snap-turn pivot.
        vrFreeCameraLastComposed = composed.center;
        vrFreeCameraLastComposedValid = true;

        {
            std::lock_guard lock(vrStereoCameraFrameMutex);
            vrStereoCameraFrame.trackingSample = trackedPose;
            vrStereoCameraFrame.composedPose = composed;
            // `bridgeBase` is the pose the headset delta was composed onto
            // (Cinemachine's own shot, or the free-camera rig pose when a
            // mode is active). Actor-shadow uses this as `sourcePose`.
            // Volume / default toon use `cinematicPose` (gameRequested).
            vrStereoCameraFrame.sourcePose = bridgeBase;
            vrStereoCameraFrame.sourcePoseValid = true;
            vrStereoCameraFrame.cinematicPose = gameRequested;
            vrStereoCameraFrame.cinematicPoseValid = true;
            vrStereoCameraFrameValid = Config::vrStereoEnabled;
        }
        gakumas::vr::pose::Pose openXrHeadCenter{};
        const bool openXrHeadValid = gakumas::vr::pose::TryCenterStereoPose(
            {trackedPose.eyes[0].pose, trackedPose.eyes[1].pose},
            openXrHeadCenter);
        gakumas::vr::UpdateHandGlowComposeBridge(
            composed.center, openXrHeadCenter, openXrHeadValid, worldScale);
        unityStereoRenderer.NoteToonFollowIndex(gakumas::vr::camera::FollowActorIndex());

        std::lock_guard lock(vrHeadPoseBridgeMutex);
        ++vrHeadPoseAppliedSamples;
        if (vrHeadPoseAppliedSamples == 1U ||
            vrHeadPoseAppliedSamples % 300U == 0U) {
            std::ostringstream stream;
            stream << "[VR][camera] HEAD_POSE_APPLIED sample="
                   << vrHeadPoseAppliedSamples
                   << " session=" << trackedPose.sessionGeneration
                   << " epoch=" << trackedPose.poseEpoch
                   << " sourceRevision=" << trackedPose.revision
                   << " brain=0x" << std::hex
                   << reinterpret_cast<std::uintptr_t>(brain) << std::dec
                   << std::fixed << std::setprecision(5)
                   << " gamePosition=(" << gamePosition.x << ',' << gamePosition.y << ','
                   << gamePosition.z << ')'
                   << " outputPosition=(" << outputPosition.x << ',' << outputPosition.y << ','
                   << outputPosition.z << ')'
                   << " gameRotation=(" << gameRotation.x << ',' << gameRotation.y << ','
                   << gameRotation.z << ',' << gameRotation.w << ')'
                   << " outputRotation=(" << outputRotation.x << ',' << outputRotation.y << ','
                   << outputRotation.z << ',' << outputRotation.w << ')';
            WriteUnityCameraDiagnosticEvent(stream.str());
        }
    }

    void ReportUnreadableCinemachineState(
        std::uint64_t sample,
        void* brain) {
        if (cinemachineStateReadFailureLogged.load(std::memory_order_acquire)) {
            return;
        }
        std::ostringstream stream;
        stream << "[VR][camera] CINEMACHINE_STATE_UNREADABLE sample=" << sample
               << " brain=0x" << std::hex << reinterpret_cast<std::uintptr_t>(brain)
               << std::dec << " readsDisabled=1";
        if (WriteUnityCameraDiagnosticEvent(stream.str())) {
            cinemachineStateReadFailureLogged.store(true, std::memory_order_release);
        }
    }



    int ReadEyeRenderPassEvent(void* pass) noexcept {
        if (pass == nullptr || eyePassEventGetter == nullptr) {
            return (std::numeric_limits<int>::min)();
        }
        const auto value = InvokeUnityGetter<int>(eyePassEventGetter, pass);
        return value.has_value()
            ? *value
            : (std::numeric_limits<int>::min)();
    }











    int EyeRenderObjectsRoleIndex(const char* role) noexcept {
        if (role != nullptr && std::strcmp(role, "left") == 0) {
            return 0;
        }
        if (role != nullptr && std::strcmp(role, "right") == 0) {
            return 1;
        }
        return 2;
    }

    void LogEyeRenderObjectsPolicy(
        const char* role,
        void* camera,
        void* pass,
        int event,
        bool isEye,
        bool skipped,
        bool originalCalled) noexcept {
        const int roleIndex = EyeRenderObjectsRoleIndex(role);
        auto& count = eyeRenderObjectsLogs[roleIndex];
        auto& lastAction = eyeRenderObjectsLastAction[roleIndex];
        const int action = skipped ? 1 : 0;
        const bool changed = lastAction != action;
        lastAction = action;
        ++count;
        if (!changed && count > 8 && count % 600 != 0) {
            return;
        }
        std::ostringstream line;
        line << "[VR][eye-pass] EYE_RENDER_OBJECTS role="
             << (role != nullptr ? role : "?") << " camera=0x" << std::hex
             << reinterpret_cast<std::uintptr_t>(camera) << std::dec
             << " eye=" << (isEye ? 1 : 0)
             << " toggle="
             << (Config::vrHideUiTextureOverlay ? 1 : 0)
             << " event=";
        if (event == (std::numeric_limits<int>::min)()) {
            line << '?';
        } else {
            line << event;
        }
        line << " action=" << (skipped ? "skip" : "execute")
             << " originalCalled=" << (originalCalled ? 1 : 0)
             << " pass=0x" << std::hex
             << reinterpret_cast<std::uintptr_t>(pass) << std::dec;
        static_cast<void>(gakumas::vr::WriteVrLog(line.str()));
    }

    // .149/.150/.181: the only pass after VLPostProcessPass(550) and before
    // FinalBlitPass(1001) is RenderObjectsPass(600) profilerTag OverlayCanvas.
    // Returning without orig leaves that layer's DrawRenderers empty. Event
    // getter miss fails open (event != 600). Source / Grip / other events
    // always execute.
    bool ShouldSkipEyeRenderObjectsPass(void* pass) noexcept {
        if (pass == nullptr || !Config::vrHideUiTextureOverlay) {
            return false;
        }
        void* camera = unityStereoRenderer.CurrentCamera();
        if (!unityStereoRenderer.IsEyeCamera(camera)) {
            return false;
        }
        return ReadEyeRenderPassEvent(pass) == 600;
    }

    // Observe-only: dump.cs ProFlareRenderingSystem.ComputeAndRenderFlares
    // is the actual raster. Do not skip — that would kill accepted .175
    // ordinary flares. Log camera role during the White night 8 s intro.




    void TrackVrSourceCamera(void* camera) {
        if (!IsVrUnityRuntimeEnabled()) {
            return;
        }
        const bool diagnostics = AreVrUnityCameraDiagnosticsEnabled();
        if (diagnostics) {
            EnsureUnityCameraDiagnosticMarker();
        }
        if (camera == nullptr) {
            return;
        }
        // .114 black screen: the left eye got the MainCamera tag, native
        // get_main returned it, this tracker re-pointed vrSourceCamera
        // at the eye, and Tick — gated on IsSelectedVrMainCamera(brain
        // output camera) — never fired again. Eyes stopped re-arming (black
        // HMD) and RestoreSourceCamera was unreachable. The VR queue
        // cameras are never eligible as the tracked source camera.
        if (unityStereoRenderer.IsEyeCamera(camera)) {
            if (!vrSourceCameraEyeIgnoredLogged.exchange(
                    true, std::memory_order_acq_rel)) {
                static_cast<void>(gakumas::vr::WriteVrLog(
                    "[VR][stereo] SOURCE_CAMERA_EYE_IGNORED tracker keeps source"));
            }
            return;
        }
        std::lock_guard lock(vrCameraStateMutex);
        if (camera == vrSourceCamera) {
            if (diagnostics) {
                auto& record = EnsureUnityCameraDiagnosticRecord(camera);
                EnsureUnityMainSelectionLog(camera, record);
            }
            return;
        }
        if (diagnostics && vrSourceCamera != nullptr) {
            const auto previous = unityCameraDiagnosticRecords.find(vrSourceCamera);
            if (previous != unityCameraDiagnosticRecords.end()) {
                previous->second.mainCamera = false;
            }
        }
        vrSourceCamera = camera;
        std::ostringstream tracked;
        tracked << "[VR][stereo] SOURCE_CAMERA_TRACKED id=0x" << std::hex
                << reinterpret_cast<std::uintptr_t>(camera) << std::dec;
        static_cast<void>(gakumas::vr::WriteVrLog(tracked.str()));
        if (diagnostics) {
            auto& record = EnsureUnityCameraDiagnosticRecord(camera);
            record.mainCamera = true;
            record.mainSelectionLogged = false;
            EnsureUnityMainSelectionLog(camera, record);
        }
    }

    bool InvokeSourceCameraMain(void* method, void** result, void** exception) noexcept {
        using Invoke = void* (*)(void*, void*, void**, void**);
        static const auto invoke = reinterpret_cast<Invoke>(GetProcAddress(
            GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_runtime_invoke"));
        if (!invoke || !method) return false;
        __try {
            *result = invoke(method, nullptr, nullptr, exception);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void BootstrapVrSourceCamera() {
        if (!IsVrUnityRuntimeEnabled()) return;
        {
            std::lock_guard lock(vrCameraStateMutex);
            if (vrSourceCamera != nullptr) return;
        }
        // A game-side cached main camera may never call our get_main hook.
        // Query the same public API only while unselected; never disturb the
        // existing source identity while the eye/restore heartbeat owns it.
        static thread_local ULONGLONG lastQuery = 0;
        const auto now = GetTickCount64();
        if (lastQuery != 0 && now - lastQuery < 1000U) return;
        lastQuery = now;
        void* result = nullptr;
        void* exception = nullptr;
        sourceCameraQueryActive = true;
        const bool invoked = InvokeSourceCameraMain(
            sourceCameraMainMethod.load(std::memory_order_acquire), &result, &exception);
        sourceCameraQueryActive = false;
        if (invoked && !exception) TrackVrSourceCamera(result);
        std::ostringstream query;
        query << "[VR][stereo] SOURCE_CAMERA_BOOTSTRAP invoked=" << invoked
              << " exception=" << exception << " result=" << result;
        static_cast<void>(gakumas::vr::WriteVrLog(query.str()));
    }

    void ObserveUnityCameraRender(void* camera) {
        if (!AreVrUnityCameraDiagnosticsEnabled()) {
            return;
        }
        EnsureUnityCameraDiagnosticMarker();
        if (camera == nullptr) {
            return;
        }
        std::lock_guard lock(vrCameraStateMutex);
        auto& record = EnsureUnityCameraDiagnosticRecord(camera);
        EnsureUnityMainSelectionLog(camera, record);
        ++record.renderCount;
        ++unityCameraRenderCallbacks;
        if (unityCameraRenderCallbacks != 1U &&
            unityCameraRenderCallbacks % 600U != 0U) {
            return;
        }

        std::ostringstream stream;
        stream << "[VR][camera] CAMERA_ACTIVITY callbacks="
               << unityCameraRenderCallbacks
               << " unique=" << unityCameraDiagnosticRecords.size()
               << " cinemachineSamples="
               << cinemachinePoseSamples.load(std::memory_order_relaxed)
               << " invalidPoseSamples="
               << cinemachineInvalidPoseSamples.load(std::memory_order_relaxed);
        std::size_t emitted = 0;
        for (const auto& [cameraId, cameraRecord] : unityCameraDiagnosticRecords) {
            if (emitted >= 8U) {
                stream << " more=" << (unityCameraDiagnosticRecords.size() - emitted);
                break;
            }
            stream << " camera" << emitted << "=(0x" << std::hex
                   << reinterpret_cast<std::uintptr_t>(cameraId) << std::dec
                   << ",\"" << cameraRecord.name << "\",renders="
                   << cameraRecord.renderCount << ",main="
                   << cameraRecord.mainCamera << ')';
            ++emitted;
        }
        WriteUnityCameraDiagnosticEvent(stream.str());
    }

    PendingCinemachineObservation CaptureCinemachinePose(
        void* brain,
        void* state) {
        PendingCinemachineObservation observation{};
        if (!AreVrUnityCameraDiagnosticsEnabled()) {
            return observation;
        }
        EnsureUnityCameraDiagnosticMarker();
        using Vector3 = UnityResolve::UnityType::Vector3;
        using Quaternion = UnityResolve::UnityType::Quaternion;
        const auto sample = cinemachinePoseSamples.fetch_add(
            1U,
            std::memory_order_relaxed) + 1U;
        observation.sampled = true;
        observation.sample = sample;
        if (!cinemachineStateReadsEnabled.load(std::memory_order_acquire)) {
            ReportUnreadableCinemachineState(sample, brain);
            return observation;
        }
        Vector3 position{};
        Quaternion rotation{};
        float fov = 0.0F;
        if (!TryReadCinemachineState(state, &position, &rotation, &fov)) {
            cinemachineInvalidPoseSamples.fetch_add(1U, std::memory_order_relaxed);
            cinemachineStateReadsEnabled.store(false, std::memory_order_release);
            ReportUnreadableCinemachineState(sample, brain);
            return observation;
        }
        const float rotationNormSquared =
            rotation.x * rotation.x + rotation.y * rotation.y +
            rotation.z * rotation.z + rotation.w * rotation.w;
        const bool layoutValid = IsValidCinemachinePose(position, rotation, fov);
        if (!layoutValid) {
            const auto invalidSample = cinemachineInvalidPoseSamples.fetch_add(
                1U,
                std::memory_order_relaxed) + 1U;
            if (invalidSample == 1U || invalidSample % 300U == 0U) {
                std::ostringstream invalidStream;
                invalidStream << "[VR][camera] CINEMACHINE_LAYOUT_INVALID sample=" << sample
                              << " invalid=" << invalidSample
                              << " brain=0x" << std::hex
                              << reinterpret_cast<std::uintptr_t>(brain) << std::dec
                              << std::fixed << std::setprecision(5)
                              << " position=(" << position.x << ',' << position.y << ','
                              << position.z << ')'
                              << " rotation=(" << rotation.x << ',' << rotation.y << ','
                              << rotation.z << ',' << rotation.w << ')'
                              << " norm2=" << rotationNormSquared
                              << " fov=" << std::setprecision(3) << fov;
                WriteUnityCameraDiagnosticEvent(invalidStream.str());
            }
            return observation;
        }
        observation.stateValid = true;
        if (sample != 1U && sample % 300U != 0U) {
            return observation;
        }

        std::ostringstream stream;
        stream << "[VR][camera] CINEMACHINE_POSE sample=" << sample
               << " brain=0x" << std::hex << reinterpret_cast<std::uintptr_t>(brain)
               << std::dec << std::fixed << std::setprecision(5)
               << " position=(" << position.x << ',' << position.y << ',' << position.z << ')'
               << " rotation=(" << rotation.x << ',' << rotation.y << ','
               << rotation.z << ',' << rotation.w << ')'
               << " fov=" << std::setprecision(3) << fov;
        WriteUnityCameraDiagnosticEvent(stream.str());
        return observation;
    }



    bool TryCopyNativeBytes(
        const void* source,
        void* destination,
        std::size_t size) noexcept {
        if (source == nullptr || destination == nullptr || size == 0U) {
            return false;
        }
        __try {
            std::memcpy(destination, source, size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }


#endif
