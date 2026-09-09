#include "StereoRenderMailbox.hpp"

#include <utility>

namespace gakumas::vr::d3d11 {

StereoRenderMailbox::Snapshot::~Snapshot() {
    Reset();
}

StereoRenderMailbox::Snapshot::Snapshot(Snapshot&& other) noexcept {
    *this = std::move(other);
}

StereoRenderMailbox::Snapshot& StereoRenderMailbox::Snapshot::operator=(
    Snapshot&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Reset();
    textures = other.textures;
    description = other.description;
    trackingSample = other.trackingSample;
    generation = other.generation;
    hostPublishTimeNanoseconds = other.hostPublishTimeNanoseconds;
    other.textures.fill(nullptr);
    other.description = {};
    other.trackingSample = {};
    other.generation = 0;
    other.hostPublishTimeNanoseconds = 0;
    return *this;
}

void StereoRenderMailbox::Snapshot::Reset() noexcept {
    for (ID3D11Texture2D*& texture : textures) {
        if (texture != nullptr) {
            texture->Release();
            texture = nullptr;
        }
    }
    description = {};
    trackingSample = {};
    generation = 0;
    hostPublishTimeNanoseconds = 0;
}

bool StereoRenderMailbox::Snapshot::IsComplete() const noexcept {
    return textures[0] != nullptr && textures[1] != nullptr &&
        description.Width != 0 && description.Height != 0 &&
        generation != 0 && trackingSample.valid &&
        trackingSample.viewCount == 2;
}

std::size_t StereoRenderMailbox::Snapshot::StagingSlot() const noexcept {
    return 0;
}

StereoRenderMailbox::~StereoRenderMailbox() {
    std::lock_guard lock(mutex_);
    ReleasePublishedLocked();
}

bool StereoRenderMailbox::Publish(
    ID3D11Texture2D* left,
    ID3D11Texture2D* right,
    const pose::StereoPoseSample& trackingSample,
    PublishDiagnostics* diagnostics) noexcept {
    PublishDiagnostics result{};
    const auto finish = [diagnostics, &result](PublishStatus status) noexcept {
        result.status = status;
        if (diagnostics != nullptr) {
            *diagnostics = result;
        }
        return status == PublishStatus::Published;
    };

    if (left == nullptr || right == nullptr) {
        return finish(PublishStatus::NullTexture);
    }
    if (!trackingSample.valid || trackingSample.viewCount != 2) {
        return finish(PublishStatus::InvalidTrackingSample);
    }

    left->GetDesc(&result.leftDescription);
    right->GetDesc(&result.rightDescription);

    ID3D11Device* leftDevice = nullptr;
    ID3D11Device* rightDevice = nullptr;
    left->GetDevice(&leftDevice);
    right->GetDevice(&rightDevice);
    result.leftDevice = reinterpret_cast<std::uintptr_t>(leftDevice);
    result.rightDevice = reinterpret_cast<std::uintptr_t>(rightDevice);

    PublishStatus rejection = PublishStatus::Published;
    if (!IsSupportedDescription(result.leftDescription)) {
        rejection = PublishStatus::UnsupportedLeftDescription;
    } else if (!IsSupportedDescription(result.rightDescription)) {
        rejection = PublishStatus::UnsupportedRightDescription;
    } else if (!SameDescription(
                   result.leftDescription,
                   result.rightDescription)) {
        rejection = PublishStatus::DescriptionMismatch;
    } else if (leftDevice == nullptr || rightDevice == nullptr) {
        rejection = PublishStatus::DeviceUnavailable;
    } else if (leftDevice != rightDevice) {
        rejection = PublishStatus::DeviceMismatch;
    }

    if (leftDevice != nullptr) {
        leftDevice->Release();
    }
    if (rightDevice != nullptr) {
        rightDevice->Release();
    }
    if (rejection != PublishStatus::Published) {
        return finish(rejection);
    }

    left->AddRef();
    right->AddRef();
    {
        std::lock_guard lock(mutex_);
        ReleasePublishedLocked();
        textures_[0] = left;
        textures_[1] = right;
        description_ = result.leftDescription;
        trackingSample_ = trackingSample;
        hostPublishTimeNanoseconds_ = pose::MonotonicNowNanoseconds();
        ++generation_;
    }
    return finish(PublishStatus::Published);
}

const char* StereoRenderMailbox::PublishStatusName(
    PublishStatus status) noexcept {
    switch (status) {
    case PublishStatus::NotAttempted:
        return "not-attempted";
    case PublishStatus::Published:
        return "published";
    case PublishStatus::NullTexture:
        return "null-texture";
    case PublishStatus::InvalidTrackingSample:
        return "invalid-tracking-sample";
    case PublishStatus::UnsupportedLeftDescription:
        return "unsupported-left-description";
    case PublishStatus::UnsupportedRightDescription:
        return "unsupported-right-description";
    case PublishStatus::DescriptionMismatch:
        return "description-mismatch";
    case PublishStatus::DeviceUnavailable:
        return "device-unavailable";
    case PublishStatus::DeviceMismatch:
        return "device-mismatch";
    }
    return "unknown";
}

bool StereoRenderMailbox::HasPublishedLocked() const noexcept {
    return textures_[0] != nullptr && textures_[1] != nullptr &&
        generation_ != 0;
}

bool StereoRenderMailbox::LeasePublishedLocked(Snapshot& snapshot) const noexcept {
    if (!HasPublishedLocked()) {
        return false;
    }
    textures_[0]->AddRef();
    textures_[1]->AddRef();
    snapshot.textures = textures_;
    snapshot.description = description_;
    snapshot.trackingSample = trackingSample_;
    snapshot.generation = generation_;
    snapshot.hostPublishTimeNanoseconds = hostPublishTimeNanoseconds_;
    return true;
}

bool StereoRenderMailbox::ReadLatest(Snapshot& snapshot) const noexcept {
    snapshot.Reset();
    std::lock_guard lock(mutex_);
    return LeasePublishedLocked(snapshot);
}

bool StereoRenderMailbox::ConsumeOnce(Snapshot& snapshot) const noexcept {
    snapshot.Reset();
    std::lock_guard lock(mutex_);
    if (!HasPublishedLocked() || generation_ == consumedGeneration_) {
        return false;
    }
    if (!LeasePublishedLocked(snapshot)) {
        return false;
    }
    consumedGeneration_ = generation_;
    return true;
}

bool StereoRenderMailbox::HasUnconsumed() const noexcept {
    std::lock_guard lock(mutex_);
    return HasPublishedLocked() && generation_ != consumedGeneration_;
}

bool StereoRenderMailbox::PublishedRecently(
    std::int64_t windowNanoseconds) const noexcept {
    std::lock_guard lock(mutex_);
    if (!HasPublishedLocked() || hostPublishTimeNanoseconds_ == 0 ||
        windowNanoseconds < 0) {
        return false;
    }
    const std::int64_t now = pose::MonotonicNowNanoseconds();
    return now >= hostPublishTimeNanoseconds_ &&
        now - hostPublishTimeNanoseconds_ <= windowNanoseconds;
}

void StereoRenderMailbox::Invalidate() noexcept {
    std::lock_guard lock(mutex_);
    ReleasePublishedLocked();
    description_ = {};
    trackingSample_ = {};
    hostPublishTimeNanoseconds_ = 0;
}

bool StereoRenderMailbox::IsSupportedDescription(
    const D3D11_TEXTURE2D_DESC& description) noexcept {
    return description.Width != 0 && description.Height != 0 &&
        description.MipLevels == 1 && description.ArraySize == 1 &&
        description.SampleDesc.Count == 1 &&
        (description.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
         description.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         description.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
         description.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
         description.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         description.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
}

bool StereoRenderMailbox::SameDescription(
    const D3D11_TEXTURE2D_DESC& left,
    const D3D11_TEXTURE2D_DESC& right) noexcept {
    return left.Width == right.Width && left.Height == right.Height &&
        left.MipLevels == right.MipLevels && left.ArraySize == right.ArraySize &&
        left.Format == right.Format &&
        left.SampleDesc.Count == right.SampleDesc.Count &&
        left.SampleDesc.Quality == right.SampleDesc.Quality;
}

void StereoRenderMailbox::ReleasePublishedLocked() noexcept {
    for (ID3D11Texture2D*& texture : textures_) {
        if (texture != nullptr) {
            texture->Release();
            texture = nullptr;
        }
    }
}

} // namespace gakumas::vr::d3d11
