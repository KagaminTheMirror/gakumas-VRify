#include "FrameCoordinator.hpp"

namespace gakumas::vr::frame {
namespace {

bool SameGeneration(const FrameIdentity& left, const FrameIdentity& right) noexcept {
    return left.sessionGeneration == right.sessionGeneration;
}

} // namespace

const char* TicketPhaseName(TicketPhase phase) noexcept {
    switch (phase) {
    case TicketPhase::Idle:
        return "idle";
    case TicketPhase::Waiting:
        return "waiting";
    case TicketPhase::Waited:
        return "waited";
    case TicketPhase::CpuPreparing:
        return "cpu-preparing";
    case TicketPhase::CpuReady:
        return "cpu-ready";
    case TicketPhase::Begun:
        return "begun";
    case TicketPhase::Rendering:
        return "rendering";
    case TicketPhase::EndQueued:
        return "end-queued";
    case TicketPhase::Ending:
        return "ending";
    case TicketPhase::Ended:
        return "ended";
    case TicketPhase::Cancelled:
        return "cancelled";
    case TicketPhase::Faulted:
        return "faulted";
    }
    return "unknown";
}

const char* TicketErrorName(TicketError error) noexcept {
    switch (error) {
    case TicketError::None:
        return "none";
    case TicketError::WaitInFlight:
        return "wait-in-flight";
    case TicketError::PrepareOccupied:
        return "prepare-occupied";
    case TicketError::PredecessorNotQueued:
        return "predecessor-not-queued";
    case TicketError::PredecessorNotReturned:
        return "predecessor-not-returned";
    case TicketError::GenerationMismatch:
        return "generation-mismatch";
    case TicketError::AlreadyBegun:
        return "already-begun";
    case TicketError::AlreadyEnded:
        return "already-ended";
    case TicketError::EndOwnershipTaken:
        return "end-ownership-taken";
    case TicketError::SessionFrozen:
        return "session-frozen";
    case TicketError::InvalidState:
        return "invalid-state";
    }
    return "unknown";
}

void FrameCoordinator::Reset(std::uint64_t sessionGeneration) noexcept {
    std::lock_guard lock(mutex_);
    slots_[0].Clear();
    slots_[1].Clear();
    renderIndex_ = -1;
    prepareIndex_ = -1;
    nextFrameId_ = 1;
    sessionGeneration_ = sessionGeneration;
    frozen_ = false;
    waitInFlight_ = false;
    NotifyWaiters();
}

void FrameCoordinator::FreezeNewWaits() noexcept {
    std::lock_guard lock(mutex_);
    frozen_ = true;
    NotifyWaiters();
}

FrameCoordinator::Slot* FrameCoordinator::FindLocked(std::uint64_t frameId) noexcept {
    const int index = FindIndexLocked(frameId);
    return index >= 0 ? &slots_[static_cast<std::size_t>(index)] : nullptr;
}

const FrameCoordinator::Slot* FrameCoordinator::FindLocked(
    std::uint64_t frameId) const noexcept {
    const int index = FindIndexLocked(frameId);
    return index >= 0 ? &slots_[static_cast<std::size_t>(index)] : nullptr;
}

int FrameCoordinator::FindIndexLocked(std::uint64_t frameId) const noexcept {
    if (frameId == 0) {
        return -1;
    }
    for (int index = 0; index < 2; ++index) {
        const Slot& slot = slots_[static_cast<std::size_t>(index)];
        if (slot.phase != TicketPhase::Idle &&
            slot.snapshot.identity.frameId == frameId) {
            return index;
        }
    }
    return -1;
}

int FrameCoordinator::AllocatePrepareIndexLocked() const noexcept {
    if (renderIndex_ == 0) {
        return 1;
    }
    if (renderIndex_ == 1) {
        return 0;
    }
    return 0;
}

void FrameCoordinator::NotifyWaiters() noexcept {
    gateCv_.notify_all();
}

TicketError FrameCoordinator::TryStartWait(
    std::uint64_t unityFrameId,
    FrameIdentity& outIdentity) noexcept {
    std::lock_guard lock(mutex_);
    if (frozen_) {
        return TicketError::SessionFrozen;
    }
    if (waitInFlight_) {
        return TicketError::WaitInFlight;
    }
    if (prepareIndex_ >= 0) {
        return TicketError::PrepareOccupied;
    }
    if (renderIndex_ >= 0) {
        const Slot& render = slots_[static_cast<std::size_t>(renderIndex_)];
        if (!render.endQueued) {
            return TicketError::PredecessorNotQueued;
        }
    }

    const int index = AllocatePrepareIndexLocked();
    Slot& slot = slots_[static_cast<std::size_t>(index)];
    if (slot.phase != TicketPhase::Idle && slot.phase != TicketPhase::Ended &&
        slot.phase != TicketPhase::Cancelled && slot.phase != TicketPhase::Faulted) {
        return TicketError::PrepareOccupied;
    }
    slot.Clear();
    slot.snapshot.identity.frameId = nextFrameId_++;
    slot.snapshot.identity.sessionGeneration = sessionGeneration_;
    slot.snapshot.identity.unityFrameId = unityFrameId;
    slot.snapshot.identity.predecessorFrameId =
        renderIndex_ >= 0
            ? slots_[static_cast<std::size_t>(renderIndex_)].snapshot.identity.frameId
            : 0;
    slot.phase = TicketPhase::Waiting;
    prepareIndex_ = index;
    waitInFlight_ = true;
    outIdentity = slot.snapshot.identity;
    return TicketError::None;
}

void FrameCoordinator::MarkWaitEntered(std::uint64_t frameId) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr || slot->phase != TicketPhase::Waiting) {
        return;
    }
    slot->waitEntered = true;
}

TicketError FrameCoordinator::CompleteWait(
    std::uint64_t frameId,
    const FrameTiming& timing,
    const pose::StereoPoseSample& tracking) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr || slot->phase != TicketPhase::Waiting) {
        return TicketError::InvalidState;
    }
    slot->snapshot.timing = timing;
    slot->snapshot.tracking = tracking;
    slot->waitReturned = true;
    slot->phase = TicketPhase::Waited;
    waitInFlight_ = false;
    return TicketError::None;
}

TicketError FrameCoordinator::SetEpochs(
    std::uint64_t frameId,
    std::uint64_t referenceSpaceEpoch,
    std::uint64_t eyeTargetGeneration,
    std::uint64_t mirrorLayoutGeneration) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return TicketError::InvalidState;
    }
    slot->snapshot.identity.referenceSpaceEpoch = referenceSpaceEpoch;
    slot->snapshot.identity.eyeTargetGeneration = eyeTargetGeneration;
    slot->snapshot.identity.mirrorLayoutGeneration = mirrorLayoutGeneration;
    return TicketError::None;
}

TicketError FrameCoordinator::MarkCpuReady(std::uint64_t frameId) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return TicketError::InvalidState;
    }
    if (slot->phase != TicketPhase::Waited &&
        slot->phase != TicketPhase::CpuPreparing) {
        return TicketError::InvalidState;
    }
    slot->phase = TicketPhase::CpuReady;
    return TicketError::None;
}

TicketError FrameCoordinator::WaitForGraphicsGate(std::uint64_t frameId) {
    std::unique_lock lock(mutex_);
    for (;;) {
        Slot* slot = FindLocked(frameId);
        if (slot == nullptr) {
            return TicketError::InvalidState;
        }
        if (slot->phase == TicketPhase::Cancelled ||
            slot->phase == TicketPhase::Faulted) {
            return slot->fault != TicketError::None ? slot->fault
                                                    : TicketError::InvalidState;
        }
        const std::uint64_t predecessor = slot->snapshot.identity.predecessorFrameId;
        if (predecessor == 0) {
            return TicketError::None;
        }
        const Slot* pred = FindLocked(predecessor);
        if (pred == nullptr) {
            return TicketError::InvalidState;
        }
        if (!SameGeneration(slot->snapshot.identity, pred->snapshot.identity)) {
            return TicketError::GenerationMismatch;
        }
        if (pred->endReturned) {
            return TicketError::None;
        }
        if (frozen_ && !pred->endReturned) {
            return TicketError::SessionFrozen;
        }
        gateCv_.wait(lock);
    }
}

TicketError FrameCoordinator::TryBegin(std::uint64_t frameId) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return TicketError::InvalidState;
    }
    if (slot->phase == TicketPhase::Begun || slot->phase == TicketPhase::Rendering ||
        slot->phase == TicketPhase::EndQueued || slot->phase == TicketPhase::Ending ||
        slot->phase == TicketPhase::Ended) {
        return TicketError::AlreadyBegun;
    }
    if (slot->phase != TicketPhase::CpuReady) {
        return TicketError::InvalidState;
    }
    const std::uint64_t predecessor = slot->snapshot.identity.predecessorFrameId;
    if (predecessor != 0) {
        Slot* pred = FindLocked(predecessor);
        if (pred == nullptr) {
            return TicketError::InvalidState;
        }
        if (!SameGeneration(slot->snapshot.identity, pred->snapshot.identity)) {
            return TicketError::GenerationMismatch;
        }
        if (!pred->endReturned) {
            return TicketError::PredecessorNotReturned;
        }
    }
    if (renderIndex_ >= 0 && renderIndex_ != FindIndexLocked(frameId)) {
        Slot& previous = slots_[static_cast<std::size_t>(renderIndex_)];
        if (previous.phase != TicketPhase::Ended &&
            previous.phase != TicketPhase::Cancelled &&
            previous.phase != TicketPhase::Faulted) {
            return TicketError::InvalidState;
        }
        previous.Clear();
    }
    const int index = FindIndexLocked(frameId);
    renderIndex_ = index;
    if (prepareIndex_ == index) {
        prepareIndex_ = -1;
    }
    slot->phase = TicketPhase::Begun;
    return TicketError::None;
}

TicketError FrameCoordinator::MarkRendering(std::uint64_t frameId) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr ||
        (slot->phase != TicketPhase::Begun && slot->phase != TicketPhase::Rendering)) {
        return TicketError::InvalidState;
    }
    slot->phase = TicketPhase::Rendering;
    return TicketError::None;
}

TicketError FrameCoordinator::MarkEndDispatched(std::uint64_t frameId) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return TicketError::InvalidState;
    }
    if (slot->endQueued) {
        return TicketError::None;
    }
    if (slot->phase != TicketPhase::Begun && slot->phase != TicketPhase::Rendering) {
        return TicketError::InvalidState;
    }
    slot->endQueued = true;
    NotifyWaiters();
    return TicketError::None;
}

TicketError FrameCoordinator::MarkEndQueued(std::uint64_t frameId) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return TicketError::InvalidState;
    }
    if (slot->phase == TicketPhase::EndQueued && slot->endQueued) {
        return TicketError::None;
    }
    if (slot->phase != TicketPhase::Begun && slot->phase != TicketPhase::Rendering) {
        return TicketError::InvalidState;
    }
    slot->phase = TicketPhase::EndQueued;
    slot->endQueued = true;
    NotifyWaiters();
    return TicketError::None;
}

TicketError FrameCoordinator::TryEnterEnd(std::uint64_t frameId) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return TicketError::InvalidState;
    }
    if (slot->endOwned || slot->endEntered || slot->endReturned ||
        slot->phase == TicketPhase::Ended) {
        return slot->endReturned || slot->phase == TicketPhase::Ended
            ? TicketError::AlreadyEnded
            : TicketError::EndOwnershipTaken;
    }
    if (slot->phase != TicketPhase::EndQueued) {
        return TicketError::InvalidState;
    }
    slot->endOwned = true;
    slot->endEntered = true;
    slot->phase = TicketPhase::Ending;
    slot->counters.endEntered += 1U;
    return TicketError::None;
}

TicketError FrameCoordinator::CompleteEnd(std::uint64_t frameId, bool success) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return TicketError::InvalidState;
    }
    if (slot->phase != TicketPhase::Ending || !slot->endOwned) {
        return TicketError::InvalidState;
    }
    slot->endReturned = true;
    slot->phase = success ? TicketPhase::Ended : TicketPhase::Faulted;
    if (!success && slot->fault == TicketError::None) {
        slot->fault = TicketError::InvalidState;
    }
    NotifyWaiters();
    return TicketError::None;
}

TicketError FrameCoordinator::Cancel(std::uint64_t frameId, TicketError reason) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return TicketError::InvalidState;
    }
    if (slot->endOwned && !slot->endReturned) {
        return TicketError::EndOwnershipTaken;
    }
    const int index = FindIndexLocked(frameId);
    slot->phase = TicketPhase::Cancelled;
    slot->fault = reason;
    if (prepareIndex_ == index) {
        prepareIndex_ = -1;
    }
    if (renderIndex_ == index && !slot->endOwned) {
        renderIndex_ = -1;
    }
    if (!slot->waitReturned) {
        waitInFlight_ = false;
    }
    NotifyWaiters();
    return TicketError::None;
}

TicketError FrameCoordinator::BindStereo(
    std::uint64_t frameId,
    std::uint64_t generation,
    const pose::StereoPoseSample& tracking) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return TicketError::InvalidState;
    }
    if (slot->phase == TicketPhase::EndQueued || slot->phase == TicketPhase::Ending ||
        slot->phase == TicketPhase::Ended) {
        return TicketError::AlreadyEnded;
    }
    slot->snapshot.stereoGeneration = generation;
    slot->snapshot.tracking = tracking;
    slot->snapshot.stereoConsumed = generation != 0;
    return TicketError::None;
}

TicketError FrameCoordinator::RecordGraphics(
    std::uint64_t frameId,
    const GraphicsCounters& delta) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return TicketError::InvalidState;
    }
    if (slot->phase != TicketPhase::Begun && slot->phase != TicketPhase::Rendering) {
        return TicketError::InvalidState;
    }
    slot->counters.beginEntered += delta.beginEntered;
    slot->counters.srp += delta.srp;
    slot->counters.nativePointer += delta.nativePointer;
    slot->counters.copy += delta.copy;
    slot->counters.aa += delta.aa;
    slot->counters.uiGpu += delta.uiGpu;
    slot->counters.endEntered += delta.endEntered;
    return TicketError::None;
}

TicketError FrameCoordinator::RecordEyeComplete(
    std::uint64_t frameId,
    bool left,
    bool right) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return TicketError::InvalidState;
    }
    if (left) {
        if (slot->snapshot.leftEyeComplete) {
            return TicketError::InvalidState;
        }
        slot->snapshot.leftEyeComplete = true;
    }
    if (right) {
        if (slot->snapshot.rightEyeComplete) {
            return TicketError::InvalidState;
        }
        slot->snapshot.rightEyeComplete = true;
    }
    return TicketError::None;
}

TicketError FrameCoordinator::RecordMirrorAvailable(
    std::uint64_t frameId,
    bool available) noexcept {
    std::lock_guard lock(mutex_);
    Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return TicketError::InvalidState;
    }
    slot->snapshot.mirrorAvailable = available;
    return TicketError::None;
}

bool FrameCoordinator::CopySnapshot(std::uint64_t frameId, FrameSnapshot& out) const noexcept {
    std::lock_guard lock(mutex_);
    const Slot* slot = FindLocked(frameId);
    if (slot == nullptr) {
        return false;
    }
    out = slot->snapshot;
    return true;
}

TicketPhase FrameCoordinator::Phase(std::uint64_t frameId) const noexcept {
    std::lock_guard lock(mutex_);
    const Slot* slot = FindLocked(frameId);
    return slot != nullptr ? slot->phase : TicketPhase::Idle;
}

GraphicsCounters FrameCoordinator::Counters(std::uint64_t frameId) const noexcept {
    std::lock_guard lock(mutex_);
    const Slot* slot = FindLocked(frameId);
    return slot != nullptr ? slot->counters : GraphicsCounters{};
}

int FrameCoordinator::SlotIndex(std::uint64_t frameId) const noexcept {
    std::lock_guard lock(mutex_);
    return FindIndexLocked(frameId);
}

std::uint64_t FrameCoordinator::RenderFrameId() const noexcept {
    std::lock_guard lock(mutex_);
    if (renderIndex_ < 0) {
        return 0;
    }
    return slots_[static_cast<std::size_t>(renderIndex_)].snapshot.identity.frameId;
}

std::uint64_t FrameCoordinator::PrepareFrameId() const noexcept {
    std::lock_guard lock(mutex_);
    if (prepareIndex_ < 0) {
        return 0;
    }
    return slots_[static_cast<std::size_t>(prepareIndex_)].snapshot.identity.frameId;
}

std::uint64_t FrameCoordinator::SessionGeneration() const noexcept {
    std::lock_guard lock(mutex_);
    return sessionGeneration_;
}

bool FrameCoordinator::HasWaitInFlight() const noexcept {
    std::lock_guard lock(mutex_);
    return waitInFlight_;
}

bool FrameCoordinator::EndQueued(std::uint64_t frameId) const noexcept {
    std::lock_guard lock(mutex_);
    const Slot* slot = FindLocked(frameId);
    return slot != nullptr && slot->endQueued;
}

bool FrameCoordinator::EndEntered(std::uint64_t frameId) const noexcept {
    std::lock_guard lock(mutex_);
    const Slot* slot = FindLocked(frameId);
    return slot != nullptr && slot->endEntered;
}

bool FrameCoordinator::EndReturned(std::uint64_t frameId) const noexcept {
    std::lock_guard lock(mutex_);
    const Slot* slot = FindLocked(frameId);
    return slot != nullptr && slot->endReturned;
}

bool FrameCoordinator::WaitEntered(std::uint64_t frameId) const noexcept {
    std::lock_guard lock(mutex_);
    const Slot* slot = FindLocked(frameId);
    return slot != nullptr && slot->waitEntered;
}

bool FrameCoordinator::WaitReturned(std::uint64_t frameId) const noexcept {
    std::lock_guard lock(mutex_);
    const Slot* slot = FindLocked(frameId);
    return slot != nullptr && slot->waitReturned;
}

bool FrameCoordinator::Frozen() const noexcept {
    std::lock_guard lock(mutex_);
    return frozen_;
}

} // namespace gakumas::vr::frame
