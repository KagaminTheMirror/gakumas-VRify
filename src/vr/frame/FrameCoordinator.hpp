#pragma once

#include "../pose/StereoPoseMailbox.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace gakumas::vr::frame {

enum class TicketPhase : std::uint8_t {
    Idle = 0,
    Waiting,
    Waited,
    CpuPreparing,
    CpuReady,
    Begun,
    Rendering,
    EndQueued,
    Ending,
    Ended,
    Cancelled,
    Faulted,
};

enum class TicketError : std::uint8_t {
    None = 0,
    WaitInFlight,
    PrepareOccupied,
    PredecessorNotQueued,
    PredecessorNotReturned,
    GenerationMismatch,
    AlreadyBegun,
    AlreadyEnded,
    EndOwnershipTaken,
    SessionFrozen,
    InvalidState,
};

struct FrameIdentity {
    std::uint64_t frameId = 0;
    std::uint64_t sessionGeneration = 0;
    std::uint64_t referenceSpaceEpoch = 0;
    std::uint64_t eyeTargetGeneration = 0;
    std::uint64_t mirrorLayoutGeneration = 0;
    std::uint64_t unityFrameId = 0;
    std::uint64_t predecessorFrameId = 0;
};

struct FrameTiming {
    std::int64_t predictedDisplayTime = 0;
    std::int64_t predictedDisplayPeriod = 0;
    bool shouldRender = false;
};

struct GraphicsCounters {
    std::uint32_t beginEntered = 0;
    std::uint32_t srp = 0;
    std::uint32_t nativePointer = 0;
    std::uint32_t copy = 0;
    std::uint32_t aa = 0;
    std::uint32_t uiGpu = 0;
    std::uint32_t endEntered = 0;

    [[nodiscard]] std::uint32_t GraphicsWork() const noexcept {
        return beginEntered + srp + nativePointer + copy + aa + uiGpu;
    }
};

struct FrameSnapshot {
    FrameIdentity identity{};
    FrameTiming timing{};
    pose::StereoPoseSample tracking{};
    std::uint64_t stereoGeneration = 0;
    std::uint64_t sourceFrameGeneration = 0;
    bool stereoConsumed = false;
    bool leftEyeComplete = false;
    bool rightEyeComplete = false;
    bool mirrorAvailable = false;
};

[[nodiscard]] const char* TicketPhaseName(TicketPhase phase) noexcept;
[[nodiscard]] const char* TicketErrorName(TicketError error) noexcept;

class FrameCoordinator final {
public:
    FrameCoordinator() = default;
    FrameCoordinator(const FrameCoordinator&) = delete;
    FrameCoordinator& operator=(const FrameCoordinator&) = delete;

    void Reset(std::uint64_t sessionGeneration) noexcept;
    void FreezeNewWaits() noexcept;

    [[nodiscard]] TicketError TryStartWait(
        std::uint64_t unityFrameId,
        FrameIdentity& outIdentity) noexcept;
    void MarkWaitEntered(std::uint64_t frameId) noexcept;
    [[nodiscard]] TicketError CompleteWait(
        std::uint64_t frameId,
        const FrameTiming& timing,
        const pose::StereoPoseSample& tracking) noexcept;
    [[nodiscard]] TicketError SetEpochs(
        std::uint64_t frameId,
        std::uint64_t referenceSpaceEpoch,
        std::uint64_t eyeTargetGeneration,
        std::uint64_t mirrorLayoutGeneration) noexcept;
    [[nodiscard]] TicketError MarkCpuReady(std::uint64_t frameId) noexcept;

    [[nodiscard]] TicketError WaitForGraphicsGate(std::uint64_t frameId);
    [[nodiscard]] TicketError TryBegin(std::uint64_t frameId) noexcept;
    [[nodiscard]] TicketError MarkRendering(std::uint64_t frameId) noexcept;
    // Marks the End call as dispatched into the ordered graphics exit without
    // sealing layers. Wait(N+1) may start; BindStereo/RecordGraphics still run.
    [[nodiscard]] TicketError MarkEndDispatched(std::uint64_t frameId) noexcept;
    [[nodiscard]] TicketError MarkEndQueued(std::uint64_t frameId) noexcept;
    [[nodiscard]] TicketError TryEnterEnd(std::uint64_t frameId) noexcept;
    [[nodiscard]] TicketError CompleteEnd(std::uint64_t frameId, bool success) noexcept;
    [[nodiscard]] TicketError Cancel(std::uint64_t frameId, TicketError reason) noexcept;

    [[nodiscard]] TicketError BindStereo(
        std::uint64_t frameId,
        std::uint64_t generation,
        const pose::StereoPoseSample& tracking) noexcept;
    [[nodiscard]] TicketError RecordGraphics(
        std::uint64_t frameId,
        const GraphicsCounters& delta) noexcept;
    [[nodiscard]] TicketError RecordEyeComplete(
        std::uint64_t frameId,
        bool left,
        bool right) noexcept;
    [[nodiscard]] TicketError RecordMirrorAvailable(
        std::uint64_t frameId,
        bool available) noexcept;

    [[nodiscard]] bool CopySnapshot(std::uint64_t frameId, FrameSnapshot& out) const noexcept;
    [[nodiscard]] TicketPhase Phase(std::uint64_t frameId) const noexcept;
    [[nodiscard]] GraphicsCounters Counters(std::uint64_t frameId) const noexcept;
    [[nodiscard]] int SlotIndex(std::uint64_t frameId) const noexcept;
    [[nodiscard]] std::uint64_t RenderFrameId() const noexcept;
    [[nodiscard]] std::uint64_t PrepareFrameId() const noexcept;
    [[nodiscard]] std::uint64_t SessionGeneration() const noexcept;
    [[nodiscard]] bool HasWaitInFlight() const noexcept;
    [[nodiscard]] bool EndQueued(std::uint64_t frameId) const noexcept;
    [[nodiscard]] bool EndEntered(std::uint64_t frameId) const noexcept;
    [[nodiscard]] bool EndReturned(std::uint64_t frameId) const noexcept;
    [[nodiscard]] bool WaitEntered(std::uint64_t frameId) const noexcept;
    [[nodiscard]] bool WaitReturned(std::uint64_t frameId) const noexcept;
    [[nodiscard]] bool Frozen() const noexcept;

private:
    struct Slot final {
        FrameSnapshot snapshot{};
        GraphicsCounters counters{};
        TicketPhase phase = TicketPhase::Idle;
        TicketError fault = TicketError::None;
        bool waitEntered = false;
        bool waitReturned = false;
        bool endQueued = false;
        bool endEntered = false;
        bool endReturned = false;
        bool endOwned = false;

        void Clear() noexcept {
            *this = Slot{};
        }
    };

    [[nodiscard]] Slot* FindLocked(std::uint64_t frameId) noexcept;
    [[nodiscard]] const Slot* FindLocked(std::uint64_t frameId) const noexcept;
    [[nodiscard]] int FindIndexLocked(std::uint64_t frameId) const noexcept;
    [[nodiscard]] int AllocatePrepareIndexLocked() const noexcept;
    void NotifyWaiters() noexcept;

    mutable std::mutex mutex_;
    std::condition_variable gateCv_;
    std::array<Slot, 2> slots_{};
    int renderIndex_ = -1;
    int prepareIndex_ = -1;
    std::uint64_t nextFrameId_ = 1;
    std::uint64_t sessionGeneration_ = 0;
    bool frozen_ = false;
    bool waitInFlight_ = false;
};

} // namespace gakumas::vr::frame
