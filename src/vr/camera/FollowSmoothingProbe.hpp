#pragma once

#include "../VrFreeCamera.hpp"

#include <array>
#include <iomanip>
#include <locale>
#include <sstream>

namespace gakumas::vr::camera {

// Diagnostics-only numeric capture, with no Unity calls or retained objects.
// Up to 128 owner-thread updates per second; overflow is explicitly counted.
// Batch I/O instead of a log write for every frame. All axes are world XYZ, Y up.
class FollowSmoothingProbe final {
public:
    struct Sample {
        std::int64_t nowNs = 0;
        std::int64_t anchorNs = 0;
        int actor = -1;
        int bone = -1;
        std::uintptr_t actorToken = 0;
        float dt = 0.0F; // actual update interval, before the rig's 100 ms clamp
        FollowSmoothingSettings settings{};
        VrFreeCameraAnchor anchor{};
        VrFreeCameraUpdateResult result{};
        pose::Pose source{};
    };

    template<class Emit>
    void Observe(const Sample& sample, Emit&& emit) {
        if (sample.result.mode != VrFreeCameraMode::Follow) {
            Flush("leave-follow", emit);
            return;
        }
        if (count_ != 0 &&
            (sample.settings.preset != rows_[0].settings.preset ||
             sample.settings.horizontalMs != rows_[0].settings.horizontalMs ||
             sample.settings.verticalMs != rows_[0].settings.verticalMs ||
             sample.actor != rows_[0].actor || sample.bone != rows_[0].bone ||
             sample.actorToken != rows_[0].actorToken || sample.result.resetApplied)) {
            Flush("settings-or-target-change", emit);
        }
        if (count_ != 0 && sample.nowNs - rows_[0].nowNs >= 1'000'000'000LL) {
            Flush("interval", emit);
        }
        if (count_ < rows_.size()) rows_[count_++] = sample;
        else ++dropped_;
    }

    template<class Emit>
    void Flush(const char* reason, Emit&& emit) {
        if (count_ == 0) return;
        const auto& first = rows_[0];
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(4)
            << "[VR][camera] FOLLOW_PROBE_BATCH reason=" << reason
            << " batch=" << ++batch_ << " count=" << count_
            << " dropped=" << dropped_ << " preset=" << first.settings.preset
            << " horizontalMs=" << first.settings.horizontalMs
            << " verticalMs=" << first.settings.verticalMs
            << " actor=" << first.actor << " actorToken=" << first.actorToken
            << " bone=" << first.bone << " worldY=up"
            << " fields=tNs,sampleNs,dt,valid,rawXYZ,filteredXYZ,rigXYZ,rigQuatXYZW,sourceXYZ,sourceQuatXYZW,distance,horizontalMs,verticalMs,step,sampleDt,jump,teleportApplied";
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& s = rows_[i];
            out << "\n[VR][camera] FOLLOW_PROBE_ROW " << s.nowNs << ','
                << s.anchorNs << ',' << s.dt << ',' << s.result.followAnchorValid;
            Vector(out, s.anchor.position);
            Vector(out, s.result.followSmoothedAnchor);
            Vector(out, s.result.rigPose.position);
            Quaternion(out, s.result.rigPose.orientation);
            Vector(out, s.source.position);
            Quaternion(out, s.source.orientation);
            out << ',' << s.result.followDistance << ',' << s.result.followHorizontalMs
                << ',' << s.result.followVerticalMs << ',' << s.result.followStep
                << ',' << s.result.followSampleDt << ',' << s.result.followJumpDetected
                << ',' << s.result.followTeleportApplied;
        }
        emit(out.str());
        count_ = 0;
        dropped_ = 0;
    }

private:
    static void Vector(std::ostream& out, const pose::Vector3& p) {
        out << ',' << p.x << ',' << p.y << ',' << p.z;
    }
    static void Quaternion(std::ostream& out, const pose::Quaternion& q) {
        out << ',' << q.x << ',' << q.y << ',' << q.z << ',' << q.w;
    }
    std::array<Sample, 128> rows_{};
    std::size_t count_ = 0;
    std::uint64_t dropped_ = 0;
    std::uint64_t batch_ = 0;
};

} // namespace gakumas::vr::camera
