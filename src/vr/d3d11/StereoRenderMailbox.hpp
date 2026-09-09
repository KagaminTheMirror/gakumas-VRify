#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>

#include "../pose/StereoPoseMailbox.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace gakumas::vr::d3d11 {

// Ticket-owned stereo eye lease. Publish AddRefs the caller's textures; it
// does not CopyResource into mailbox-owned staging. ConsumeOnce still hands
// each generation to exactly one ticket. Production Publish and FlipStereo
// share one ordered graphics callback, so the next Unity write cannot start
// until that consumer has recorded its GPU commands.
class StereoRenderMailbox final {
public:
    enum class PublishStatus : std::uint8_t {
        NotAttempted,
        Published,
        NullTexture,
        InvalidTrackingSample,
        UnsupportedLeftDescription,
        UnsupportedRightDescription,
        DescriptionMismatch,
        DeviceUnavailable,
        DeviceMismatch,
    };

    struct PublishDiagnostics final {
        PublishStatus status = PublishStatus::NotAttempted;
        D3D11_TEXTURE2D_DESC leftDescription{};
        D3D11_TEXTURE2D_DESC rightDescription{};
        std::uintptr_t leftDevice = 0;
        std::uintptr_t rightDevice = 0;
    };

    struct Snapshot final {
        std::array<ID3D11Texture2D*, 2> textures{};
        D3D11_TEXTURE2D_DESC description{};
        pose::StereoPoseSample trackingSample{};
        std::uint64_t generation = 0;
        std::int64_t hostPublishTimeNanoseconds = 0;

        Snapshot() = default;
        ~Snapshot();
        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;
        Snapshot(Snapshot&& other) noexcept;
        Snapshot& operator=(Snapshot&& other) noexcept;

        void Reset() noexcept;
        [[nodiscard]] bool IsComplete() const noexcept;
        [[nodiscard]] std::size_t StagingSlot() const noexcept;
    };

    StereoRenderMailbox() = default;
    ~StereoRenderMailbox();
    StereoRenderMailbox(const StereoRenderMailbox&) = delete;
    StereoRenderMailbox& operator=(const StereoRenderMailbox&) = delete;

    [[nodiscard]] bool Publish(
        ID3D11Texture2D* left,
        ID3D11Texture2D* right,
        const pose::StereoPoseSample& trackingSample,
        PublishDiagnostics* diagnostics = nullptr) noexcept;
    [[nodiscard]] static const char* PublishStatusName(
        PublishStatus status) noexcept;
    [[nodiscard]] bool ReadLatest(Snapshot& snapshot) const noexcept;
    // Consumes the current published generation once. A later call without a
    // newer Publish returns false so a new ticket cannot steal the previous
    // pair as latest-wins reuse.
    [[nodiscard]] bool ConsumeOnce(Snapshot& snapshot) const noexcept;
    [[nodiscard]] bool HasUnconsumed() const noexcept;
    [[nodiscard]] bool PublishedRecently(std::int64_t windowNanoseconds) const noexcept;
    void Invalidate() noexcept;

private:
    static bool IsSupportedDescription(
        const D3D11_TEXTURE2D_DESC& description) noexcept;
    static bool SameDescription(
        const D3D11_TEXTURE2D_DESC& left,
        const D3D11_TEXTURE2D_DESC& right) noexcept;
    [[nodiscard]] bool HasPublishedLocked() const noexcept;
    [[nodiscard]] bool LeasePublishedLocked(Snapshot& snapshot) const noexcept;
    void ReleasePublishedLocked() noexcept;

    mutable std::mutex mutex_;
    std::array<ID3D11Texture2D*, 2> textures_{};
    D3D11_TEXTURE2D_DESC description_{};
    pose::StereoPoseSample trackingSample_{};
    std::uint64_t generation_ = 0;
    mutable std::uint64_t consumedGeneration_ = 0;
    std::int64_t hostPublishTimeNanoseconds_ = 0;
};

} // namespace gakumas::vr::d3d11
