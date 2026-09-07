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
        StagingContextUnavailable,
        StagingResourceCreateFailed,
        NoFreeStagingSlot,
    };

    struct PublishDiagnostics final {
        PublishStatus status = PublishStatus::NotAttempted;
        D3D11_TEXTURE2D_DESC leftDescription{};
        D3D11_TEXTURE2D_DESC rightDescription{};
        std::uintptr_t leftDevice = 0;
        std::uintptr_t rightDevice = 0;
        HRESULT stagingHresult = S_OK;
        std::size_t stagingSlot = 0;
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

    private:
        friend class StereoRenderMailbox;
        StereoRenderMailbox* owner_ = nullptr;
        std::size_t slotIndex_ = 0;
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
    void Invalidate() noexcept;

private:
    static constexpr std::size_t kStagingSlotCount = 3;
    static constexpr std::size_t kInvalidSlot = kStagingSlotCount;

    struct StagingSlot final {
        std::array<ID3D11Texture2D*, 2> textures{};
        D3D11_TEXTURE2D_DESC description{};
        std::uint32_t readers = 0;
        bool retired = false;
    };

    static bool IsSupportedDescription(
        const D3D11_TEXTURE2D_DESC& description) noexcept;
    static bool SameDescription(
        const D3D11_TEXTURE2D_DESC& left,
        const D3D11_TEXTURE2D_DESC& right) noexcept;
    static bool AreCopyCompatibleFormats(
        DXGI_FORMAT left,
        DXGI_FORMAT right) noexcept;
    [[nodiscard]] bool EnsureStagingResourcesLocked(
        std::size_t slotIndex,
        ID3D11Device* device,
        const D3D11_TEXTURE2D_DESC& sourceDescription,
        HRESULT& result) noexcept;
    void ReleaseSnapshotLease(std::size_t slotIndex) noexcept;
    void ReleaseSlotResourcesLocked(StagingSlot& slot) noexcept;
    void ReleaseAllSlotsLocked() noexcept;

    mutable std::mutex mutex_;
    std::array<StagingSlot, kStagingSlotCount> slots_{};
    std::size_t publishedSlot_ = kInvalidSlot;
    D3D11_TEXTURE2D_DESC description_{};
    pose::StereoPoseSample trackingSample_{};
    std::uint64_t generation_ = 0;
    std::int64_t hostPublishTimeNanoseconds_ = 0;
};

} // namespace gakumas::vr::d3d11
