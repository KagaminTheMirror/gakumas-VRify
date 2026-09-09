#pragma once

namespace gakumas::vr {

class UnityStereoRenderer;

// The Unity owner thread freezes color/MV metadata and COM texture refs, then
// the ordered graphics callback captures both MV sources before AA/mailbox.
// A cached native pointer retains allocation lifetime, not pixel readiness.
// These entry points never call managed Unity APIs.
void BindStereoGpuPublishOwner(UnityStereoRenderer* renderer) noexcept;
[[nodiscard]] bool HasPendingStereoGpuPublish() noexcept;
[[nodiscard]] bool ConsumePendingStereoGpuPublish() noexcept;
void DropPendingStereoGpuPublish() noexcept;

} // namespace gakumas::vr
