#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gakumas::vr::gaze {
// Pure decision core; the adapter supplies verified pre-solver writeback data.
enum class NaturalState { Original, Recovering, Looking, Releasing, Holding };
enum class NaturalReason { None, Invalid, Disabled, Prohibited, Boundary, Motion };
struct NaturalInput {
    std::uintptr_t actor = 0;
    double dt = 0;
    bool valid = false, requested = false, prohibited = false, officialInRange = false;
    bool degenerateDirection = false;
    double yaw = 0, pitch = 0, yawRate = 0, pitchRate = 0;
    double animationSpeed = 0;
    double horizontalLimit = 0, verticalLimit = 0;
    // Optional official reference cone; separate from head-relative yaw/pitch.
    double officialAngle = 0, officialAngleRate = 0, officialLimit = 0;
};
struct NaturalOutput {
    NaturalState state = NaturalState::Original;
    NaturalReason reason = NaturalReason::None;
    double participation = 0;
};
struct NaturalTuning {
    double prediction, exitMargin, recoverMargin;
    double exitSpeed, recoverSpeed, recoverTargetSpeed;
    double stableTime, holdTime;
};
inline NaturalTuning NaturalPreset(int preset) {
    switch(preset) {
    case 0: return {.20,8,12,240,140,120,.30,.65}; // Stability first.
    case 2: return {.10,3,5,360,220,180,.12,.35}; // Gaze first.
    default:return {.15,5,8,300,180,150,.20,.50}; // dev.474 unchanged.
    }
}
struct NaturalPolicy {
    NaturalOutput output;
    std::uintptr_t identity = 0;
    double elapsed = 0, hold = 0, safe = 0, from = 0;
    int preference = 1;
    static double Ease(double t) { t=std::clamp(t,0.0,1.0);return t*t*t*(10+t*(-15+6*t)); }
    void Reset(std::uintptr_t actor=0) { *this=NaturalPolicy{};identity=actor; }
    NaturalOutput Step(const NaturalInput& in, int preset=1) {
        if(identity!=in.actor) Reset(in.actor);
        preset=preset>=0 && preset<=2 ? preset : 1;
        // Changing preference never restarts an active fade or jumps its gain.
        // Reentry evidence is collected afresh under the new conditions.
        if(preference!=preset){safe=0;preference=preset;}
        const auto tuning=NaturalPreset(preset);
        const bool finite=std::isfinite(in.dt)&&std::isfinite(in.yaw)&&std::isfinite(in.pitch)&&
            std::isfinite(in.yawRate)&&std::isfinite(in.pitchRate)&&std::isfinite(in.animationSpeed)&&
            std::isfinite(in.horizontalLimit)&&std::isfinite(in.verticalLimit)&&
            std::isfinite(in.officialAngle)&&std::isfinite(in.officialAngleRate)&&std::isfinite(in.officialLimit);
        const bool usable=in.actor&&in.valid&&finite&&in.dt>=0&&in.dt<=0.1&&
            in.horizontalLimit>25&&in.verticalLimit>20&&!in.degenerateDirection;
        // Pausing consumes no state time. A discontinuous/invalid sample causes
        // a conservative release; it cannot produce a large one-step jump.
        const double dt=std::isfinite(in.dt)?std::clamp(in.dt,0.0,0.05):0.0;
        // The utility's cone is not a head-local yaw/pitch rectangle. Applying
        // it three times plus a 25-degree reentry margin starved dev.473.
        const bool officialCone=in.officialLimit>0;
        const double yh=officialCone?85:in.horizontalLimit-15;
        const double ph=officialCone?65:in.verticalLimit-10;
        const bool coneBoundary=officialCone && (in.officialAngle>=in.officialLimit-tuning.exitMargin ||
            in.officialAngle+tuning.prediction*std::max(0.0,in.officialAngleRate)>=in.officialLimit-tuning.exitMargin);
        const bool boundary=coneBoundary || !in.officialInRange || std::abs(in.yaw)>=yh || std::abs(in.pitch)>=ph ||
            std::abs(in.yaw+tuning.prediction*in.yawRate)>=yh || std::abs(in.pitch+tuning.prediction*in.pitchRate)>=ph;
        const bool motion=std::abs(in.animationSpeed)>=tuning.exitSpeed;
        NaturalReason reason=!usable?NaturalReason::Invalid:!in.requested?NaturalReason::Disabled:
            in.prohibited?NaturalReason::Prohibited:boundary?NaturalReason::Boundary:
            motion?NaturalReason::Motion:NaturalReason::None;
        const bool danger=reason!=NaturalReason::None;
        const bool coneSettled=!officialCone || (in.officialAngle<in.officialLimit-tuning.recoverMargin &&
            in.officialAngle+tuning.prediction*std::max(0.0,in.officialAngleRate)<in.officialLimit-tuning.recoverMargin);
        const bool settled=!danger && coneSettled && std::abs(in.yaw)<yh-5 && std::abs(in.pitch)<ph-5 &&
            std::abs(in.animationSpeed)<tuning.recoverSpeed && std::abs(in.yawRate)<tuning.recoverTargetSpeed &&
            std::abs(in.pitchRate)<tuning.recoverTargetSpeed;
        if(danger) {output.reason=reason;safe=0;}
        if(danger && (output.state==NaturalState::Recovering||output.state==NaturalState::Looking)) {
            from=output.participation;elapsed=0;output.state=NaturalState::Releasing;
            return output; // Preserve the exact value at the reversal boundary.
        }
        switch(output.state) {
        case NaturalState::Original:
        case NaturalState::Holding:
            hold+=dt;safe=settled?safe+dt:0;
            if(settled && safe>=tuning.stableTime && (output.state==NaturalState::Original||hold>=tuning.holdTime)) {
                elapsed=0;from=output.participation;output.state=NaturalState::Recovering;output.reason=NaturalReason::None;
            }
            break;
        case NaturalState::Recovering:
            elapsed+=dt;output.participation=from+(1-from)*Ease(elapsed/0.6);
            if(elapsed>=0.6){output.participation=1;output.state=NaturalState::Looking;}
            break;
        case NaturalState::Looking: break;
        case NaturalState::Releasing:
            elapsed+=dt;output.participation=from*(1-Ease(elapsed/0.3));
            if(elapsed>=0.3){output.participation=0;output.state=NaturalState::Holding;hold=0;safe=0;}
            break;
        }
        return output;
    }
};
}
