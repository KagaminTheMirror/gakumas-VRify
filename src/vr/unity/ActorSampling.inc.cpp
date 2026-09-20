    void ReportVrBoneFailure(const char* reason, int index) {
        if (!AreVrUnityCameraDiagnosticsEnabled()) return;
        static std::int64_t last = 0;
        const auto now = gakumas::vr::pose::MonotonicNowNanoseconds();
        if (now - last <= 1'000'000'000LL) return;
        last = now;
        std::ostringstream message;
        message << "[VR][camera] " << reason << " index=" << index;
        static_cast<void>(gakumas::vr::WriteVrLog(message.str()));
    }
// VR-owned integration, included by the patched upstream Hook translation unit.
    bool ActorBonesReady(void* actor) {
        static auto* klass = Il2cppUtils::GetClass("campus-submodule.Runtime.dll", "Campus.Common", "CampusActorController");
        static auto* field = UnityResolve::Invoke<Il2cppUtils::FieldInfo*>("il2cpp_class_get_field_from_name", klass->address, "_humanBodyBoneMap");
        static bool fieldMissLogged = false;
        if (field == nullptr && !fieldMissLogged) {
            fieldMissLogged = true;
            if (AreVrUnityCameraDiagnosticsEnabled()) {
                static_cast<void>(gakumas::vr::WriteVrLog("[VR][camera] FREECAM_BONES_FIELD_MISSING _humanBodyBoneMap not found; skipping readiness gate"));
            }
        }
        return !field || Il2cppUtils::ClassGetFieldValue<void*>(actor, field) != nullptr;
    }
    void SampleVrActor(void* actor) {
        static auto* klass = Il2cppUtils::GetClass("campus-submodule.Runtime.dll", "Campus.Common", "CampusActorController");
        static auto* indexMethod = Il2cppUtils::il2cpp_class_get_method_from_name(klass->address, "get_index", 0);
        const int index = indexMethod ? reinterpret_cast<int (*)(void*)>(indexMethod->methodPointer)(actor) : 0;
        gakumas::vr::NoteFollowCostumeActor(actor, index);
        const bool follow = gakumas::vr::camera::IsVrFreeCameraBoneAnchorWanted();
        const bool toon = Config::vrActorToonSourceAnchor && Config::vrToonFollowRef != Config::kVrToonFollowRefSource && gakumas::vr::camera::IsVrFreeCameraLocomotionActive();
        static bool needRestoreHides = false;
        const auto restore = [&] {
            if (!needRestoreHides) return;
            needRestoreHides = false;
            ApplyHeadVisibility(nullptr, false);
            ApplyHeadVisibility(nullptr, true);
        };
        if (!follow && !toon) { restore(); return; }
        const bool bonesReady = ActorBonesReady(actor);
        static auto* parent = UnityResolve::Invoke<void*>("il2cpp_class_get_parent", klass->address);
        static auto* boneMethod = Il2cppUtils::il2cpp_class_get_method_from_name(parent, "GetHumanBodyBoneTransform", 1);
        auto bone = boneMethod ? reinterpret_cast<UnityResolve::UnityType::Transform* (*)(void*, int)>(boneMethod->methodPointer) : nullptr;
        if (toon && bonesReady && bone) {
            auto* transform = bone(actor, 8);
            if (!transform) transform = bone(actor, 0);
            if (transform) {
                const auto position = transform->GetPosition(), forward = transform->GetForward();
                unityStereoRenderer.NoteToonActorSample(index, {position.x, position.y, position.z}, {forward.x, forward.y, forward.z});
            }
        }
        if (!follow) { restore(); return; }
        RecordVrCameraActorIndex(index, gakumas::vr::pose::MonotonicNowNanoseconds());
        if (index != gakumas::vr::camera::FollowActorIndex()) return;
        // Reuse the upstream enum discovery; the VR selection itself remains independent.
        static const int headBone = InitBodyParts() ? GKCamera::bodyPartsEnum.GetValueByName("Head") : 0xA;
        if (!bonesReady) { ReportVrBoneFailure("FREECAM_BONES_NOT_READY", index); return; }
        const int boneId = gakumas::vr::camera::IsVrFreeCameraFirstPerson() ? headBone : gakumas::vr::camera::ReadVrFreeCameraFollowBone();
        auto* transform = bone ? bone(actor, boneId) : nullptr;
        if (!transform) { ReportVrBoneFailure("FREECAM_ANCHOR_NULL_TRANS", index); return; }
        vrCameraAnchorPosition = transform->GetPosition();
        vrCameraAnchorForward = transform->GetForward();
        vrCameraAnchorRotation = transform->GetRotation();
        vrCameraAnchorActor = index;
        vrCameraAnchorBone = boneId;
        vrCameraAnchorActorToken = reinterpret_cast<std::uintptr_t>(actor);
        gakumas::vr::camera::PublishVrFollowActorController(actor);
        vrCameraAnchorTimeNanoseconds = gakumas::vr::pose::MonotonicNowNanoseconds();
        static auto* rootField = klass->Get<UnityResolve::Field>("_rootBody");
        auto* body = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Transform*>(actor, rootField);
        if (!body) return;
        auto* model = body->GetParent();
        if (!model) return;
        for (int child = 0; child < model->GetChildCount(); ++child) {
            auto* part = model->GetChild(child);
            const auto name = part->GetName();
            if (name == "Root_Hair") { ApplyHeadVisibility(part->GetGameObject(), false); needRestoreHides = true; }
            if (name != "Root_Face") continue;
            for (int mesh = 0; mesh < part->GetChildCount(); ++mesh) {
                auto* renderer = part->GetChild(mesh);
                if (renderer->GetName() == "VLSkinningRenderer") { ApplyHeadVisibility(renderer->GetGameObject(), true); needRestoreHides = true; }
            }
        }
    }
