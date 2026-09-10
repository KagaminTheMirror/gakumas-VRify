#pragma once

namespace gakumas::vr {
enum class GripBlurDiscoverAction { Refresh, Reuse };

// Lifecycle hooks make an empty cache reusable, not terminal. A later
// OnEnable/AddBlur/scene-identity dirty forces another FindObjects scan.
// Missing hooks keep the previous every-pass global discover.
inline GripBlurDiscoverAction GripBlurDiscoverDecision(
    bool hooksReady, bool seeded, bool dirty) noexcept {
    if (!hooksReady || !seeded || dirty) return GripBlurDiscoverAction::Refresh;
    return GripBlurDiscoverAction::Reuse;
}
} // namespace gakumas::vr
