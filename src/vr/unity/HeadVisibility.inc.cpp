// VR-owned integration, included by the patched upstream Hook translation unit.
    // SEH-guarded m_CachedPtr probe. The cached hidden-head shells can be
    // GC-collected while we hold them (our DLL statics are not GC roots), so
    // even reading the wrapper must tolerate a freed page.
    bool IsManagedShellAlive(void* instance) noexcept {
        if (instance == nullptr) {
            return false;
        }
        __try {
            if (*static_cast<void**>(instance) == nullptr) {
                return false;
            }
            return reinterpret_cast<UnityResolve::UnityType::UnityObject*>(instance)
                       ->m_CachedPtr != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // FP head skip (.287): keep face/hair GameObjects active and
    // Renderers enabled so VL skinning stays posed and the official
    // ActorShadowPass.DrawRenderers (same camera cull) still sees a
    // head. Hide wearer color/depth only via Material.SetShaderPassEnabled
    // on instanced get_materials, using CampusActorShader.Pass GBuffer
    // and DepthOnly names. Do not disable ShadowCaster. Do not
    // SetActive(false). Do not write Renderer.enabled / layer mask
    // (.282/.285/.286 killed VL shells and/or left the blob headless).
    // Restore pass enables on leave-FP / retarget.
    // See evidence/fp-head-hide/.
    constexpr int kFpHeadColorSkipPassCount = 5;
    constexpr const char* kFpHeadColorSkipPassNames[kFpHeadColorSkipPassCount] = {
        "UniversalGBufferActor",
        "UniversalGBufferActorHair",
        "UniversalGBufferOutline",
        "DepthOnly",
        "DepthOnlyPass2",
    };

    struct FpHeadSlot {
        Il2CppGCHandle goHandle = nullptr;
        int rendererCount = 0;
        int materialCount = 0;
    };

    struct FpHeadSkipApi {
        UnityResolve::Method* getComponentsInChildren = nullptr;
        UnityResolve::Method* getMaterials = nullptr;
        UnityResolve::Method* getMaterial = nullptr;
        UnityResolve::Method* setShaderPassEnabled = nullptr;
        UnityResolve::Method* getShaderPassEnabled = nullptr;
        UnityResolve::Method* shaderTagIdCtor = nullptr;
        UnityResolve::Field* actorShadowTag = nullptr;
        UnityResolve::Field* preDepthTag = nullptr;
        void* rendererType = nullptr;
        Il2CppGCHandle passNameHandles[kFpHeadColorSkipPassCount]{};
        Il2cppString* passNames[kFpHeadColorSkipPassCount]{};
        bool resolveLogged = false;
        bool shadowTagLogged = false;

        [[nodiscard]] bool Ready() const noexcept {
            return getComponentsInChildren != nullptr &&
                getMaterials != nullptr &&
                setShaderPassEnabled != nullptr &&
                rendererType != nullptr &&
                passNames[0] != nullptr;
        }
    };

    FpHeadSlot fpHeadFaceSlot{};
    FpHeadSlot fpHeadHairSlot{};
    FpHeadSkipApi fpHeadSkipApi{};
    bool fpHeadColorSkipApplied = false;

    UnityResolve::Method* FindExactInstanceMethod(
        UnityResolve::Class* klass,
        std::string_view name,
        std::string_view returnType,
        const std::initializer_list<std::string_view>& argTypes) {
        if (klass == nullptr) {
            return nullptr;
        }
        UnityResolve::Method* found = nullptr;
        for (auto* candidate : klass->methods) {
            if (candidate == nullptr || candidate->name != name ||
                candidate->static_function ||
                candidate->address == nullptr ||
                candidate->return_type == nullptr ||
                candidate->return_type->name != returnType ||
                candidate->args.size() != argTypes.size()) {
                continue;
            }
            bool exact = true;
            std::size_t index = 0;
            for (const auto argType : argTypes) {
                if (candidate->args[index] == nullptr ||
                    candidate->args[index]->pType == nullptr ||
                    std::string_view(candidate->args[index]->pType->name) !=
                        argType) {
                    exact = false;
                    break;
                }
                ++index;
            }
            if (!exact) {
                continue;
            }
            if (found != nullptr) {
                return nullptr;
            }
            found = candidate;
        }
        return found;
    }

    UnityResolve::Method* FindUniqueInstanceMethod(
        UnityResolve::Class* klass,
        std::string_view name,
        const std::initializer_list<std::string_view>& argTypes) {
        if (klass == nullptr) {
            return nullptr;
        }
        UnityResolve::Method* found = nullptr;
        for (auto* candidate : klass->methods) {
            if (candidate == nullptr || candidate->name != name ||
                candidate->static_function ||
                candidate->address == nullptr ||
                candidate->args.size() != argTypes.size()) {
                continue;
            }
            bool exact = true;
            std::size_t index = 0;
            for (const auto argType : argTypes) {
                if (candidate->args[index] == nullptr ||
                    candidate->args[index]->pType == nullptr ||
                    std::string_view(candidate->args[index]->pType->name) !=
                        argType) {
                    exact = false;
                    break;
                }
                ++index;
            }
            if (!exact) {
                continue;
            }
            if (found != nullptr) {
                return nullptr;
            }
            found = candidate;
        }
        return found;
    }

    bool EnsureFpHeadSkipApi() noexcept {
        if (fpHeadSkipApi.Ready()) {
            return true;
        }
        auto* gameObjectClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
        auto* rendererClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer");
        auto* materialClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Material");
        auto* shaderTagClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll",
            "UnityEngine.Rendering",
            "ShaderTagId");
        auto* actorShadowPassClass = Il2cppUtils::GetClass(
            "vl-unity.Runtime.dll", "VL.Rendering", "ActorShadowPass");
        auto* preDepthClass = Il2cppUtils::GetClass(
            "vl-unity.Runtime.dll", "VL.Rendering", "VLPreDepthIDPass");
        fpHeadSkipApi.getComponentsInChildren = FindExactInstanceMethod(
            gameObjectClass,
            "GetComponentsInChildren",
            "UnityEngine.Component[]",
            {"System.Type", "System.Boolean"});
        if (fpHeadSkipApi.getComponentsInChildren == nullptr) {
            // Some IL2CPP dumps spell the array as Component[] without
            // the UnityEngine prefix. Args are the public contract.
            UnityResolve::Method* fallback = nullptr;
            if (gameObjectClass != nullptr) {
                for (auto* candidate : gameObjectClass->methods) {
                    if (candidate == nullptr ||
                        candidate->name != "GetComponentsInChildren" ||
                        candidate->static_function ||
                        candidate->args.size() != 2U ||
                        candidate->args[0] == nullptr ||
                        candidate->args[0]->pType == nullptr ||
                        candidate->args[1] == nullptr ||
                        candidate->args[1]->pType == nullptr ||
                        std::string_view(candidate->args[0]->pType->name) !=
                            "System.Type" ||
                        std::string_view(candidate->args[1]->pType->name) !=
                            "System.Boolean") {
                        continue;
                    }
                    if (fallback != nullptr) {
                        fallback = nullptr;
                        break;
                    }
                    fallback = candidate;
                }
            }
            fpHeadSkipApi.getComponentsInChildren = fallback;
        }
        fpHeadSkipApi.getMaterials = FindExactInstanceMethod(
            rendererClass,
            "get_materials",
            "UnityEngine.Material[]",
            {});
        if (fpHeadSkipApi.getMaterials == nullptr) {
            fpHeadSkipApi.getMaterials = FindUniqueInstanceMethod(
                rendererClass, "get_materials", {});
        }
        fpHeadSkipApi.getMaterial = FindExactInstanceMethod(
            rendererClass, "get_material", "UnityEngine.Material", {});
        if (fpHeadSkipApi.getMaterial == nullptr) {
            fpHeadSkipApi.getMaterial = FindUniqueInstanceMethod(
                rendererClass, "get_material", {});
        }
        fpHeadSkipApi.setShaderPassEnabled = FindExactInstanceMethod(
            materialClass,
            "SetShaderPassEnabled",
            "System.Void",
            {"System.String", "System.Boolean"});
        if (fpHeadSkipApi.setShaderPassEnabled == nullptr) {
            fpHeadSkipApi.setShaderPassEnabled = FindUniqueInstanceMethod(
                materialClass,
                "SetShaderPassEnabled",
                {"System.String", "System.Boolean"});
        }
        fpHeadSkipApi.getShaderPassEnabled = FindExactInstanceMethod(
            materialClass,
            "GetShaderPassEnabled",
            "System.Boolean",
            {"System.String"});
        fpHeadSkipApi.shaderTagIdCtor = FindExactInstanceMethod(
            shaderTagClass, ".ctor", "System.Void", {"System.String"});
        if (rendererClass != nullptr) {
            fpHeadSkipApi.rendererType = rendererClass->GetType();
        }
        if (actorShadowPassClass != nullptr) {
            fpHeadSkipApi.actorShadowTag =
                actorShadowPassClass->Get<UnityResolve::Field>("_shaderTag");
        }
        if (preDepthClass != nullptr) {
            fpHeadSkipApi.preDepthTag =
                preDepthClass->Get<UnityResolve::Field>("_depthIDShaderTagId");
        }
        for (int i = 0; i < kFpHeadColorSkipPassCount; ++i) {
            if (fpHeadSkipApi.passNames[i] != nullptr) {
                continue;
            }
            auto* name = Il2cppString::New(kFpHeadColorSkipPassNames[i]);
            if (name == nullptr) {
                continue;
            }
            fpHeadSkipApi.passNameHandles[i] =
                UnityResolve::Invoke<Il2CppGCHandle>(
                    "il2cpp_gchandle_new", name, false);
            fpHeadSkipApi.passNames[i] = name;
        }
        if (!fpHeadSkipApi.resolveLogged) {
            fpHeadSkipApi.resolveLogged = true;
            int named = 0;
            for (int i = 0; i < kFpHeadColorSkipPassCount; ++i) {
                if (fpHeadSkipApi.passNames[i] != nullptr) {
                    ++named;
                }
            }
            std::ostringstream stream;
            stream << "[VR][camera] FREECAM_HEAD_SKIP_API ready="
                   << (fpHeadSkipApi.Ready() ? 1 : 0)
                   << " getComp="
                   << (fpHeadSkipApi.getComponentsInChildren != nullptr ? 1 : 0)
                   << " getMats="
                   << (fpHeadSkipApi.getMaterials != nullptr ? 1 : 0)
                   << " getMat="
                   << (fpHeadSkipApi.getMaterial != nullptr ? 1 : 0)
                   << " setPass="
                   << (fpHeadSkipApi.setShaderPassEnabled != nullptr ? 1 : 0)
                   << " getPass="
                   << (fpHeadSkipApi.getShaderPassEnabled != nullptr ? 1 : 0)
                   << " type="
                   << (fpHeadSkipApi.rendererType != nullptr ? 1 : 0)
                   << " names=" << named
                   << " shadowTag="
                   << (fpHeadSkipApi.actorShadowTag != nullptr ? 1 : 0)
                   << " predepthTag="
                   << (fpHeadSkipApi.preDepthTag != nullptr ? 1 : 0);
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        }
        return fpHeadSkipApi.Ready();
    }

    UnityResolve::UnityType::GameObject* ResolveFpHeadGo(
        const FpHeadSlot& slot) noexcept {
        if (slot.goHandle == nullptr) {
            return nullptr;
        }
        const auto target =
            UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", slot.goHandle);
        if (!IsManagedShellAlive(target)) {
            return nullptr;
        }
        return static_cast<UnityResolve::UnityType::GameObject*>(target);
    }

    void ReleaseFpHeadHandle(Il2CppGCHandle& handle) noexcept {
        if (handle != nullptr) {
            UnityResolve::Invoke<void>(
                "il2cpp_gchandle_free", std::exchange(handle, nullptr));
        }
    }

    void ReleaseFpHeadSlot(FpHeadSlot& slot) noexcept {
        slot.rendererCount = 0;
        slot.materialCount = 0;
        ReleaseFpHeadHandle(slot.goHandle);
    }

    void WriteFpHeadMaterialPasses(void* material, bool hidden) {
        for (int i = 0; i < kFpHeadColorSkipPassCount; ++i) {
            auto* name = fpHeadSkipApi.passNames[i];
            if (name == nullptr) {
                continue;
            }
            fpHeadSkipApi.setShaderPassEnabled->Invoke<void>(
                material,
                name,
                !hidden,
                fpHeadSkipApi.setShaderPassEnabled->address);
        }
    }

    void WriteFpHeadRendererPasses(
        void* renderer, bool hidden, int& wrote, int& materials) {
        bool wroteAny = false;
        if (fpHeadSkipApi.getMaterials != nullptr) {
            try {
                auto* array =
                    fpHeadSkipApi.getMaterials
                        ->Invoke<UnityResolve::UnityType::Array<void*>*>(
                            renderer, fpHeadSkipApi.getMaterials->address);
                if (array != nullptr) {
                    const int count = static_cast<int>(array->max_length);
                    for (int i = 0; i < count; ++i) {
                        void* material = array->At(static_cast<unsigned>(i));
                        if (!IsManagedShellAlive(material)) {
                            continue;
                        }
                        WriteFpHeadMaterialPasses(material, hidden);
                        ++materials;
                        wroteAny = true;
                    }
                }
            } catch (...) {
            }
        }
        if (wroteAny) {
            ++wrote;
            return;
        }
        if (fpHeadSkipApi.getMaterial == nullptr) {
            return;
        }
        try {
            void* material = fpHeadSkipApi.getMaterial->Invoke<void*>(
                renderer, fpHeadSkipApi.getMaterial->address);
            if (!IsManagedShellAlive(material)) {
                return;
            }
            WriteFpHeadMaterialPasses(material, hidden);
            ++materials;
            ++wrote;
        } catch (...) {
        }
    }

    void WriteFpHeadSlot(
        FpHeadSlot& slot, bool hidden, int& wrote, int& dead) noexcept {
        if (!EnsureFpHeadSkipApi()) {
            return;
        }
        auto* go = ResolveFpHeadGo(slot);
        if (go == nullptr) {
            if (slot.goHandle != nullptr) {
                ++dead;
            }
            return;
        }
        auto* array = fpHeadSkipApi.getComponentsInChildren
            ->Invoke<UnityResolve::UnityType::Array<void*>*>(
                go, fpHeadSkipApi.rendererType, true);
        if (array == nullptr) {
            ++dead;
            return;
        }
        slot.rendererCount = 0;
        slot.materialCount = 0;
        const int count = static_cast<int>(array->max_length);
        for (int i = 0; i < count; ++i) {
            void* renderer = array->At(static_cast<unsigned>(i));
            if (!IsManagedShellAlive(renderer)) {
                ++dead;
                continue;
            }
            ++slot.rendererCount;
            const int wroteBefore = wrote;
            WriteFpHeadRendererPasses(
                renderer, hidden, wrote, slot.materialCount);
            if (wrote == wroteBefore) {
                ++dead;
            }
        }
    }

    void LogFpHeadSkip(
        const char* action,
        const char* reason,
        bool hidden,
        int wrote,
        int dead) noexcept {
        static bool lastHidden = false;
        static std::string lastAction;
        static std::string lastReason;
        static std::int64_t lastNs = 0;
        const std::int64_t nowNs =
            gakumas::vr::pose::MonotonicNowNanoseconds();
        const bool changed = hidden != lastHidden ||
            lastAction != action || lastReason != reason;
        if (!changed && nowNs - lastNs < 1'000'000'000LL) {
            return;
        }
        lastHidden = hidden;
        lastAction = action;
        lastReason = reason;
        lastNs = nowNs;
        std::ostringstream stream;
        stream << "[VR][camera] FREECAM_HEAD_SKIP action=" << action
               << " reason=" << reason
               << " hidden=" << (hidden ? 1 : 0)
               << " wrote=" << wrote
               << " dead=" << dead
               << " face=" << fpHeadFaceSlot.rendererCount
               << " hair=" << fpHeadHairSlot.rendererCount
               << " mats="
               << (fpHeadFaceSlot.materialCount + fpHeadHairSlot.materialCount)
               << " applied=" << (fpHeadColorSkipApplied ? 1 : 0);
        static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
    }

    bool FpHeadSkipHasTargets() noexcept {
        return fpHeadFaceSlot.goHandle != nullptr ||
            fpHeadHairSlot.goHandle != nullptr;
    }

    void SetFpHeadColorSkip(bool hidden, const char* reason) noexcept {
        if (!FpHeadSkipHasTargets()) {
            return;
        }
        if (!hidden && !fpHeadColorSkipApplied) {
            LogFpHeadSkip("hold", reason, hidden, 0, 0);
            return;
        }
        const bool firstHide = hidden && !fpHeadColorSkipApplied;
        int wrote = 0;
        int dead = 0;
        WriteFpHeadSlot(fpHeadFaceSlot, hidden, wrote, dead);
        WriteFpHeadSlot(fpHeadHairSlot, hidden, wrote, dead);
        fpHeadColorSkipApplied = hidden;
        LogFpHeadSkip(
            hidden ? (firstHide ? "apply" : "refresh") : "restore",
            reason,
            hidden,
            wrote,
            dead);
    }

    std::int32_t MakeFpHeadShaderTagId(const char* name) noexcept {
        if (fpHeadSkipApi.shaderTagIdCtor == nullptr || name == nullptr) {
            return 0;
        }
        struct ShaderTagIdValue {
            std::int32_t id;
        };
        ShaderTagIdValue tag{};
        try {
            fpHeadSkipApi.shaderTagIdCtor->Invoke<void>(
                &tag,
                Il2cppString::New(name),
                fpHeadSkipApi.shaderTagIdCtor->address);
        } catch (...) {
            return 0;
        }
        return tag.id;
    }

    void LogFpHeadActorShadowTag(void* pass) noexcept {
        if (fpHeadSkipApi.shadowTagLogged || pass == nullptr) {
            return;
        }
        fpHeadSkipApi.shadowTagLogged = true;
        if (!EnsureFpHeadSkipApi()) {
            return;
        }
        std::int32_t live = 0;
        std::int32_t predepth = 0;
        int haveLive = 0;
        int havePre = 0;
        if (fpHeadSkipApi.actorShadowTag != nullptr &&
            !fpHeadSkipApi.actorShadowTag->static_field &&
            fpHeadSkipApi.actorShadowTag->offset >= 0) {
            live = *reinterpret_cast<std::int32_t*>(
                static_cast<std::byte*>(pass) +
                fpHeadSkipApi.actorShadowTag->offset);
            haveLive = 1;
        }
        if (fpHeadSkipApi.preDepthTag != nullptr &&
            fpHeadSkipApi.preDepthTag->static_field) {
            fpHeadSkipApi.preDepthTag->GetValue(&predepth);
            havePre = 1;
        }
        const std::int32_t caster = MakeFpHeadShaderTagId("ShadowCaster");
        const std::int32_t gbuffer =
            MakeFpHeadShaderTagId("UniversalGBufferActor");
        const std::int32_t hair =
            MakeFpHeadShaderTagId("UniversalGBufferActorHair");
        const std::int32_t depth = MakeFpHeadShaderTagId("DepthOnly");
        std::ostringstream stream;
        stream << "[VR][camera] FREECAM_HEAD_SKIP action=shadow-tag"
               << " live=" << live
               << " haveLive=" << haveLive
               << " caster=" << caster
               << " gbuffer=" << gbuffer
               << " hair=" << hair
               << " depth=" << depth
               << " matchCaster=" << (haveLive && live == caster ? 1 : 0)
               << " matchGbuffer=" << (haveLive && live == gbuffer ? 1 : 0)
               << " matchHair=" << (haveLive && live == hair ? 1 : 0)
               << " matchDepth=" << (haveLive && live == depth ? 1 : 0)
               << " predepth=" << predepth
               << " havePre=" << havePre
               << " preDepthOnly=" << (havePre && predepth == depth ? 1 : 0)
               << " preGbuffer=" << (havePre && predepth == gbuffer ? 1 : 0);
        static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
    }

    void PublishFpHeadSlot(
        FpHeadSlot& slot, UnityResolve::UnityType::GameObject* obj) noexcept {
        SetFpHeadColorSkip(false, "retarget");
        ReleaseFpHeadSlot(slot);
        if (obj == nullptr || !IsNativeObjectAlive(obj)) {
            return;
        }
        // Keep the GO active so VLActorFaceModel does not OnDisable /
        // Dispose. Color skip is SetShaderPassEnabled, not SetActive.
        obj->SetActive(true);
        slot.goHandle = UnityResolve::Invoke<Il2CppGCHandle>(
            "il2cpp_gchandle_new", obj, false);
        std::ostringstream stream;
        stream << "[VR][camera] FREECAM_HEAD_SKIP action=publish"
               << " face=" << fpHeadFaceSlot.rendererCount
               << " hair=" << fpHeadHairSlot.rendererCount;
        static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
    }

    void ApplyHeadVisibility(UnityResolve::UnityType::GameObject* obj, const bool isFace) {
        // Face/hair objects outlive frames and character switches. A raw
        // static pointer is not a GC root (stereo.213). Root the GO and
        // re-resolve materials on every write.
        auto& slot = isFace ? fpHeadFaceSlot : fpHeadHairSlot;
        const auto lastGo = ResolveFpHeadGo(slot);
        const auto isFirstPerson =
            (Config::vrRuntimeStartupEnabled ? gakumas::vr::camera::IsVrFreeCameraFirstPerson() : GKCamera::GetCameraMode() == GKCamera::CameraMode::FIRST_PERSON);

        if (isFirstPerson && obj) {
            if (obj == lastGo) {
                SetFpHeadColorSkip(true, "late-update");
                return;
            }
            PublishFpHeadSlot(slot, obj);
            SetFpHeadColorSkip(true, "publish");
        } else {
            SetFpHeadColorSkip(false, "leave-fp");
            if (lastGo != nullptr && IsNativeObjectAlive(lastGo)) {
                lastGo->SetActive(true);
            }
            ReleaseFpHeadSlot(slot);
        }
    }
