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
    owner_ = other.owner_;
    slotIndex_ = other.slotIndex_;
    other.textures.fill(nullptr);
    other.description = {};
    other.trackingSample = {};
    other.generation = 0;
    other.hostPublishTimeNanoseconds = 0;
    other.owner_ = nullptr;
    other.slotIndex_ = 0;
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
    if (owner_ != nullptr) {
        StereoRenderMailbox* owner = owner_;
        const std::size_t slotIndex = slotIndex_;
        owner_ = nullptr;
        slotIndex_ = 0;
        owner->ReleaseSnapshotLease(slotIndex);
    }
}

bool StereoRenderMailbox::Snapshot::IsComplete() const noexcept {
    return textures[0] != nullptr && textures[1] != nullptr &&
        description.Width != 0 && description.Height != 0 &&
        generation != 0 && trackingSample.valid &&
        trackingSample.viewCount == 2;
}

std::size_t StereoRenderMailbox::Snapshot::StagingSlot() const noexcept {
    return slotIndex_;
}

StereoRenderMailbox::~StereoRenderMailbox() {
    std::lock_guard lock(mutex_);
    publishedSlot_ = kInvalidSlot;
    ReleaseAllSlotsLocked();
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

    if (rejection != PublishStatus::Published) {
        if (leftDevice != nullptr) {
            leftDevice->Release();
        }
        if (rightDevice != nullptr) {
            rightDevice->Release();
        }
        return finish(rejection);
    }

    ID3D11DeviceContext* context = nullptr;
    leftDevice->GetImmediateContext(&context);
    if (context == nullptr) {
        leftDevice->Release();
        rightDevice->Release();
        return finish(PublishStatus::StagingContextUnavailable);
    }

    PublishStatus publishStatus = PublishStatus::Published;
    {
        std::lock_guard lock(mutex_);
        std::size_t slotIndex = kInvalidSlot;
        for (std::size_t offset = 0; offset < slots_.size(); ++offset) {
            const std::size_t candidate = publishedSlot_ == kInvalidSlot
                ? offset
                : (publishedSlot_ + 1U + offset) % slots_.size();
            if (candidate != publishedSlot_ && slots_[candidate].readers == 0) {
                slotIndex = candidate;
                break;
            }
        }
        if (slotIndex == kInvalidSlot) {
            publishStatus = PublishStatus::NoFreeStagingSlot;
        } else if (!EnsureStagingResourcesLocked(
                       slotIndex,
                       leftDevice,
                       result.leftDescription,
                       result.stagingHresult)) {
            publishStatus = PublishStatus::StagingResourceCreateFailed;
        } else {
            StagingSlot& slot = slots_[slotIndex];
            context->CopyResource(slot.textures[0], left);
            context->CopyResource(slot.textures[1], right);
            publishedSlot_ = slotIndex;
            description_ = result.leftDescription;
            trackingSample_ = trackingSample;
            hostPublishTimeNanoseconds_ = pose::MonotonicNowNanoseconds();
            ++generation_;
            result.stagingSlot = slotIndex;
        }
    }
    context->Release();
    leftDevice->Release();
    rightDevice->Release();
    if (publishStatus != PublishStatus::Published) {
        return finish(publishStatus);
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
    case PublishStatus::StagingContextUnavailable:
        return "staging-context-unavailable";
    case PublishStatus::StagingResourceCreateFailed:
        return "staging-resource-create-failed";
    case PublishStatus::NoFreeStagingSlot:
        return "no-free-staging-slot";
    }
    return "unknown";
}

bool StereoRenderMailbox::ReadLatest(Snapshot& snapshot) const noexcept {
    snapshot.Reset();
    std::lock_guard lock(mutex_);
    if (publishedSlot_ == kInvalidSlot || generation_ == 0) {
        return false;
    }
    StagingSlot& slot = const_cast<StagingSlot&>(slots_[publishedSlot_]);
    if (slot.textures[0] == nullptr || slot.textures[1] == nullptr) {
        return false;
    }
    slot.textures[0]->AddRef();
    slot.textures[1]->AddRef();
    ++slot.readers;
    snapshot.textures = slot.textures;
    snapshot.description = description_;
    snapshot.trackingSample = trackingSample_;
    snapshot.generation = generation_;
    snapshot.hostPublishTimeNanoseconds = hostPublishTimeNanoseconds_;
    snapshot.owner_ = const_cast<StereoRenderMailbox*>(this);
    snapshot.slotIndex_ = publishedSlot_;
    return true;
}

void StereoRenderMailbox::Invalidate() noexcept {
    std::lock_guard lock(mutex_);
    publishedSlot_ = kInvalidSlot;
    for (StagingSlot& slot : slots_) {
        if (slot.readers == 0) {
            ReleaseSlotResourcesLocked(slot);
        } else {
            slot.retired = true;
        }
    }
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
        AreCopyCompatibleFormats(left.Format, right.Format) &&
        left.SampleDesc.Count == right.SampleDesc.Count &&
        left.SampleDesc.Quality == right.SampleDesc.Quality;
}

bool StereoRenderMailbox::AreCopyCompatibleFormats(
    DXGI_FORMAT left,
    DXGI_FORMAT right) noexcept {
    const auto family = [](DXGI_FORMAT format) noexcept {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return 1;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return 2;
        default:
            return 0;
        }
    };
    const int leftFamily = family(left);
    return leftFamily != 0 && leftFamily == family(right);
}

bool StereoRenderMailbox::EnsureStagingResourcesLocked(
    std::size_t slotIndex,
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC& sourceDescription,
    HRESULT& result) noexcept {
    result = E_FAIL;
    if (slotIndex >= slots_.size() || device == nullptr) {
        return false;
    }
    StagingSlot& slot = slots_[slotIndex];
    bool reusable = slot.textures[0] != nullptr && slot.textures[1] != nullptr &&
        SameDescription(slot.description, sourceDescription);
    if (reusable) {
        ID3D11Device* slotDevice = nullptr;
        slot.textures[0]->GetDevice(&slotDevice);
        reusable = slotDevice == device;
        if (slotDevice != nullptr) {
            slotDevice->Release();
        }
    }
    if (reusable) {
        slot.retired = false;
        result = S_OK;
        return true;
    }

    ReleaseSlotResourcesLocked(slot);
    D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
    stagingDescription.Usage = D3D11_USAGE_DEFAULT;
    stagingDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    stagingDescription.CPUAccessFlags = 0;
    stagingDescription.MiscFlags = 0;
    for (ID3D11Texture2D*& texture : slot.textures) {
        result = device->CreateTexture2D(&stagingDescription, nullptr, &texture);
        if (FAILED(result) || texture == nullptr) {
            ReleaseSlotResourcesLocked(slot);
            return false;
        }
    }
    slot.description = sourceDescription;
    slot.retired = false;
    result = S_OK;
    return true;
}

void StereoRenderMailbox::ReleaseSnapshotLease(std::size_t slotIndex) noexcept {
    std::lock_guard lock(mutex_);
    if (slotIndex < slots_.size() && slots_[slotIndex].readers != 0) {
        StagingSlot& slot = slots_[slotIndex];
        --slot.readers;
        if (slot.readers == 0 && slot.retired) {
            ReleaseSlotResourcesLocked(slot);
        }
    }
}

void StereoRenderMailbox::ReleaseSlotResourcesLocked(StagingSlot& slot) noexcept {
    for (ID3D11Texture2D*& texture : slot.textures) {
        if (texture != nullptr) {
            texture->Release();
            texture = nullptr;
        }
    }
    slot.description = {};
    slot.retired = false;
}

void StereoRenderMailbox::ReleaseAllSlotsLocked() noexcept {
    for (StagingSlot& slot : slots_) {
        ReleaseSlotResourcesLocked(slot);
        slot.readers = 0;
    }
}

} // namespace gakumas::vr::d3d11
