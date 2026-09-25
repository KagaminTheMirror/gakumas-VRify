// Included inside OpenXrContext.cpp's namespace. No Unity object/state writes.
void OpenXrContext::ResetAnalyticPointers() noexcept {
    pointerAaPass_.Reset();
    pointerEyeImages_.clear();
    if (pointerEyeSwapchain_ != XR_NULL_HANDLE && dispatch_.DestroySwapchain())
        dispatch_.DestroySwapchain()(pointerEyeSwapchain_);
    pointerEyeSwapchain_=XR_NULL_HANDLE;
    pointerEyeFormat_=DXGI_FORMAT_UNKNOWN;
    pointerEyeWidth_=pointerEyeHeight_=0;
    pointerEyeFailed_=false;
}

bool OpenXrContext::RenderAnalyticPointers(const StereoFrame& frame,
    const GpuSubmitSnapshot& gpu, SubmitLayers& layers, VrLog& log) {
    if (pointerEyeFailed_ || frame.viewCount!=2 ||
        (frame.viewStateFlags & (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) !=
            (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return false;
    const auto fail=[&](const char* step, XrResult result) {
        ResetAnalyticPointers(); pointerEyeFailed_=true;
        log.Write(std::string("[VR][pointer] POINTER_ANALYTIC_FAILED step=")+step+
            " result="+dispatch_.ResultText(instance_,result));
        return false;
    };
    if (pointerEyeSwapchain_==XR_NULL_HANDLE) {
        if (!recommendedEyeWidth_ || !recommendedEyeHeight_) return fail("extent",XR_ERROR_VALIDATION_FAILURE);
        uint32_t count=0;
        auto result=dispatch_.EnumerateSwapchainFormats()(session_,0,&count,nullptr);
        if (XR_FAILED(result) || !count) return fail("formats",result);
        std::vector<int64_t> formats(count);
        result=dispatch_.EnumerateSwapchainFormats()(session_,count,&count,formats.data());
        if (XR_FAILED(result)) return fail("formats",result);
        for (auto candidate:{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
            if (std::find(formats.begin(),formats.end(),candidate)!=formats.end()) {
                pointerEyeFormat_=candidate; break;
            }
        }
        if (pointerEyeFormat_==DXGI_FORMAT_UNKNOWN) return fail("format",XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
        pointerEyeWidth_=std::min(recommendedEyeWidth_,mirrorMaximumWidth_);
        pointerEyeHeight_=std::min(recommendedEyeHeight_,mirrorMaximumHeight_);
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.width=pointerEyeWidth_; info.height=pointerEyeHeight_;
        info.format=pointerEyeFormat_; info.arraySize=2;
        info.sampleCount=info.faceCount=info.mipCount=1;
        result=dispatch_.CreateSwapchain()(session_,&info,&pointerEyeSwapchain_);
        if (XR_FAILED(result)) return fail("create",result);
        result=dispatch_.EnumerateSwapchainImages()(pointerEyeSwapchain_,0,&count,nullptr);
        if (XR_FAILED(result) || !count) return fail("images",result);
        pointerEyeImages_.assign(count,XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        result=dispatch_.EnumerateSwapchainImages()(pointerEyeSwapchain_,count,&count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(pointerEyeImages_.data()));
        if (XR_FAILED(result)) return fail("images",result);
        log.Write("[VR][pointer] POINTER_ANALYTIC_READY eyes=2 size="+
            std::to_string(pointerEyeWidth_)+"x"+std::to_string(pointerEyeHeight_));
    }
    std::array<d3d11::PointerAaEye,2> eyes{};
    const auto& panel=gpu.pointerPanelPoseBase;
    const float height=gpu.mirrorWidth ? gpu.panelQuadWidth*gpu.mirrorHeight/gpu.mirrorWidth : 0.0F;
    for (std::size_t eye=0; eye<eyes.size(); ++eye) {
        const auto& view=frame.views[eye];
        const auto inverse=pose::Conjugate(ToTrackingPose(view.pose).orientation);
        const auto relativeOrientation=pose::Multiply(inverse,panel.orientation);
        const auto axisX=pose::Rotate(relativeOrientation,{1,0,0});
        const auto axisY=pose::Rotate(relativeOrientation,{0,1,0});
        eyes[eye].tangents={std::tan(view.fov.angleLeft),std::tan(view.fov.angleRight),
            std::tan(view.fov.angleDown),std::tan(view.fov.angleUp)};
        for (std::size_t hand=0; hand<frame.pointers.size(); ++hand) {
            const auto& p=frame.pointers[hand];
            if (!p.hovering || !std::isfinite(p.u) || !std::isfinite(p.v)) continue;
            const float radius=p.mirrorCursorRadius.x*gpu.panelQuadWidth;
            if (!std::isfinite(radius) || radius<=0) continue;
            const auto offset=pose::Rotate(panel.orientation,
                {(p.u-0.5F)*gpu.panelQuadWidth,(0.5F-p.v)*height,0});
            const auto center=pose::Rotate(inverse,{panel.position.x+offset.x-view.pose.position.x,
                panel.position.y+offset.y-view.pose.position.y,panel.position.z+offset.z-view.pose.position.z});
            if (!pose::IsFinite(center) || !pose::IsFinite(axisX) || !pose::IsFinite(axisY)) continue;
            eyes[eye].discs[hand]={{center.x,center.y,center.z,radius},
                {axisX.x,axisX.y,axisX.z,0},{axisY.x,axisY.y,axisY.z,0}};
        }
    }
    uint32_t index=0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    auto result=dispatch_.AcquireSwapchainImage()(pointerEyeSwapchain_,&acquire,&index);
    if (XR_FAILED(result)) return fail("acquire",result);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout=XR_INFINITE_DURATION;
    result=dispatch_.WaitSwapchainImage()(pointerEyeSwapchain_,&wait);
    if (result!=XR_SUCCESS) return fail("wait",result);
    bool painted=false;
    {
        VR_PERF_SCOPE(pointerPaint,"xr.pointer-analytic",[&](std::string_view line) noexcept {log.Write(line);});
        painted=index<pointerEyeImages_.size() && pointerEyeImages_[index].texture &&
            pointerAaPass_.Render(sessionDevice_,sessionContext_,pointerEyeImages_[index].texture,
                pointerEyeFormat_,eyes);
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    result=dispatch_.ReleaseSwapchainImage()(pointerEyeSwapchain_,&release);
    if (XR_FAILED(result) || !painted) return fail("paint/release",XR_FAILED(result)?result:XR_ERROR_RUNTIME_FAILURE);
    auto& projection=layers.pointerProjection;
    projection={XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space=stageSpace_!=XR_NULL_HANDLE ? stageSpace_ : localSpace_;
    projection.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    projection.viewCount=2; projection.views=layers.pointerViews.data();
    for (std::size_t eye=0; eye<2; ++eye) {
        auto& view=layers.pointerViews[eye]; view={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        view.pose=frame.views[eye].pose; view.fov=frame.views[eye].fov;
        view.subImage.swapchain=pointerEyeSwapchain_;
        view.subImage.imageArrayIndex=static_cast<uint32_t>(eye);
        view.subImage.imageRect.extent={static_cast<int32_t>(pointerEyeWidth_),static_cast<int32_t>(pointerEyeHeight_)};
    }
    return true;
}
