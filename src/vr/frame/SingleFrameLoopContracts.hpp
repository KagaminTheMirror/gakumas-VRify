#pragma once

#include <cstdint>

namespace gakumas::vr::frame {

// M2 production contracts for scroll, pointer lease, and ticket pose admission.
inline constexpr std::int64_t kScrollMaxCompensationNanoseconds = 100'000'000;
inline constexpr float kScrollMaxUnityUnitsPerSecond = 18.75F;
inline constexpr float kScrollMaxUnityUnitsPerTicket =
    kScrollMaxUnityUnitsPerSecond *
    (static_cast<float>(kScrollMaxCompensationNanoseconds) / 1'000'000'000.0F);

inline constexpr std::uint64_t kPointerShortBlockMs[] = {90, 300, 500};
inline constexpr std::uint64_t kPointerDisconnectMs = 2000;

inline constexpr std::int64_t kPoseBlockedButValidMs[] = {300, 500};

// Extra PlayerLoop ticks after a live stereo publish must not Begin/End a
// mirror fallback. Hold the waited ticket until ConsumeOnce can take a new
// pair, or until this window elapses and 2D/ineligible fallback is legal.
inline constexpr std::int64_t kStereoSubmitHoldNanoseconds = 80'000'000;

} // namespace gakumas::vr::frame
